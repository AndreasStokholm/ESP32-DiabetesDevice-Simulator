#include <NimBLEDevice.h>

#include <deque>

#include "crypto.h"
#include "link.h"

// The handshake and session, as the sensor sees them. The client in
// G7SensorKit drives every exchange: it writes a request and waits for the
// reply, so this side is a straight-line state machine.

static const int64_t handshakeTimeout = 65 * SEC;  // outlasts the client's own 60 s pairing deadline
static const size_t certMTU = 20;                  // the cert characteristic streams 20-byte chunks

enum : uint8_t {
  opAuthChallengeTx = 0x02,
  opAuthChallengeRx = 0x03,
  opAuthAnswer = 0x04,
  opAuthStatus = 0x05,
  opFinalize = 0x06,
  opBondRequest = 0x07,
  opBondConfirmed = 0x08,
  opDisconnect = 0x09,
  opKeyRound = 0x0A,
  opCertificate = 0x0B,
  opKeyChallenge = 0x0C,
  opCalibrationBounds = 0x32,
  opCalibrate = 0x34,
  opTransmitterVer = 0x4A,
  opGlucose = 0x4E,
  opExtendedVersion = 0x52,
  opBackfill = 0x59,
};

uint16_t charHandles[4];

const char* charName(Char c) {
  static const char* n[] = {"auth", "control", "backfill", "cert"};
  return n[(int)c];
}

// MARK: - Link

Link::Link(uint16_t conn, const std::string& addr, AdvertMode mode) : conn(conn), addr(addr), mode(mode) {
  for (auto& s : subs) s = 0;
  q = xQueueCreate(128, sizeof(InMsg*));
}

Link::~Link() {
  InMsg* m;
  while (xQueueReceive(q, &m, 0) == pdTRUE) delete m;
  vQueueDelete(q);
}

void Link::enqueue(Char c, const uint8_t* data, size_t len, bool subscribed) {
  if (finished) return;
  InMsg* m = new InMsg{c, subscribed, Bytes(data, data + len)};
  if (xQueueSend(q, &m, 0) != pdTRUE) {
    logf("%s: event queue full, dropping %s write", addr.c_str(), charName(c));
    delete m;
  }
}

bool Link::next(InMsg& out, int64_t timeoutMs) {
  int64_t deadline = monoMs() + std::max<int64_t>(timeoutMs, 0);
  for (;;) {
    InMsg* m = nullptr;
    if (xQueueReceive(q, &m, 0) == pdTRUE) {
      out = std::move(*m);
      delete m;
      return true;
    }
    if (!connected) return false;
    int64_t left = deadline - monoMs();
    if (left <= 0) return false;
    if (xQueueReceive(q, &m, pdMS_TO_TICKS(std::min<int64_t>(left, 100))) == pdTRUE) {
      out = std::move(*m);
      delete m;
      return true;
    }
  }
}

bool Link::notify(Char c, const Bytes& b) {
  uint16_t sub = subs[(int)c];
  if (!connected || !sub) {
    logf("%s %s -> %s dropped: not subscribed", addr.c_str(), charName(c), hexstr(b).c_str());
    return false;
  }
  logf("%s %s -> %s", addr.c_str(), charName(c), hexstr(b).c_str());
  for (int attempt = 0; attempt < 200 && connected; attempt++) {
    os_mbuf* om = ble_hs_mbuf_from_flat(b.data(), b.size());
    if (!om) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    // The mbuf is consumed whatever the outcome.
    int rc = (sub & 1) ? ble_gattc_notify_custom(conn, charHandles[(int)c], om)
                       : ble_gattc_indicate_custom(conn, charHandles[(int)c], om);
    if (rc == 0) return true;
    if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY && rc != BLE_HS_EALREADY) {
      logf("%s %s: notify failed rc=%d", addr.c_str(), charName(c), rc);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return false;
}

void Link::close() {
  if (!connected) return;
  logf("%s: sensor closing the link", addr.c_str());
  ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

// MARK: - Handler

namespace {

class Handler {
 public:
  Handler(std::shared_ptr<Link> l, Sensor* s) : link(std::move(l)), sensor(s) {}
  // One handshake and session. False when the handshake failed and the link
  // must be dropped for the phone to start over.
  bool protocol();

 private:
  std::shared_ptr<Link> link;
  Sensor* sensor;
  // Messages read ahead of what a step wanted, kept for the step that does.
  std::deque<Bytes> authQ, ctrlQ;
  Bytes certBuf;

  std::unique_ptr<JPAKE> jpake;
  Bytes sessionKey;
  uint8_t displayType = 0;

  const char* addr() { return link->addr.c_str(); }

  bool keyExchange(const Bytes& first);
  bool challenge(const Bytes& msg, bool fresh);
  bool certificatePhase();
  bool keyChallenge();
  bool finalize();
  void session();
  void handleControl(const Bytes& msg);

  bool waitAuth(int64_t timeout, Bytes& out);
  bool waitControl(int64_t timeout, Bytes& out);
  bool waitCert(size_t n, int64_t timeout, Bytes& out);
  void stash(InMsg& m);
  void notifyCert(const Bytes& data);
};

bool Handler::protocol() {
  // The link is shared with the pod, so the phone may open the G7's
  // handshake at any time after connecting, or never: wait as long as the
  // link lasts.
  Bytes first;
  while (!waitAuth(10 * SEC, first)) {
    if (!link->connected) return true;
  }
  if (first.empty()) return true;
  jpake.reset();
  sessionKey.clear();
  certBuf.clear();
  ctrlQ.clear();
  switch (first[0]) {
    case opKeyRound: {  // a fresh pairing opens with the key exchange
      if (!keyExchange(first)) return false;
      Bytes ch;
      if (!waitAuth(handshakeTimeout, ch)) return false;
      if (!challenge(ch, true)) return false;
      if (!certificatePhase() || !keyChallenge() || !finalize()) return false;
      break;
    }
    case opAuthChallengeTx:  // a reconnect opens straight with the challenge
      if (!challenge(first, false)) return false;
      break;
    default:
      logf("%s: unexpected opening message %s", addr(), hexstr(first).c_str());
      return true;
  }
  session();
  return true;
}

// MARK: EC-JPAKE

bool Handler::keyExchange(const Bytes& first) {
  jpake.reset(new JPAKE(sensor->pairingCode()));
  for (uint8_t round = 0; round <= 2; round++) {
    Bytes msg = first;
    if (round > 0 && !waitAuth(handshakeTimeout, msg)) return false;
    if (msg.size() < 2 || msg[0] != opKeyRound || msg[1] != round) {
      logf("%s: expected key-exchange round %u, got %s", addr(), round, hexstr(msg).c_str());
      return false;
    }

    Bytes mine;
    switch (round) {
      case 0: mine = jpake->round1(); break;
      case 1: mine = jpake->round2(); break;
      case 2:
        if (!jpake->round3(mine)) {
          logf("%s: round 3 before the client's rounds", addr());
          return false;
        }
        break;
    }
    notifyCert(mine);

    Bytes theirs;
    if (!waitCert(PCertLength, handshakeTimeout, theirs)) return false;
    if (round < 2) {
      bool verified = false;
      if (!jpake->setClientRound(round, theirs, &verified)) {
        logf("%s: client's round %u is not a valid point", addr(), round);
        return false;
      }
      if (!verified) logf("%s: client's round %u proof did not verify", addr(), round);
    } else {
      if (!jpake->sharedKey(theirs, sessionKey)) {
        logf("%s: shared key failed", addr());
        return false;
      }
    }
  }
  logf("%s: key exchange complete", addr());
  return true;
}

// MARK: AES challenge

bool Handler::challenge(const Bytes& msg, bool fresh) {
  if (msg.size() < 10 || msg[0] != opAuthChallengeTx) {
    logf("%s: malformed challenge %s", addr(), hexstr(msg).c_str());
    return false;
  }
  const uint8_t* clientChallenge = &msg[1];
  displayType = msg[9];

  // A real G7 refuses a display that already collected this reading. Here
  // the phone can open a session at any time on the shared link, so it is
  // always admitted and simply reads the latest reading again.

  Bytes key, newKey;
  if (fresh) {
    key = newKey = sessionKey;
  } else {
    key = sensor->slotKey(displayType);
  }

  // Answer the client's challenge under the key, plus our own challenge.
  // With no key (a reconnect after the slot was forgotten), answer with
  // noise: the client's own check then fails and it re-pairs with the code.
  Bytes resp = {opAuthChallengeRx};
  Bytes answer = key.empty() ? randomBytes(8) : encryptChallenge(clientChallenge, key);
  resp.insert(resp.end(), answer.begin(), answer.end());
  Bytes sensorChallenge = randomBytes(8);
  resp.insert(resp.end(), sensorChallenge.begin(), sensorChallenge.end());
  link->notify(Char::Auth, resp);
  if (key.empty()) {
    logf("%s: no key for display type %u; client will re-pair", addr(), displayType);
    return false;
  }

  Bytes reply;
  if (!waitAuth(handshakeTimeout, reply)) return false;
  if (reply.size() < 9 || reply[0] != opAuthAnswer) return false;
  Bytes expected = encryptChallenge(sensorChallenge.data(), key);
  if (!std::equal(expected.begin(), expected.end(), reply.begin() + 1)) {
    AuthVerdict v = sensor->rejectedMismatch();
    link->notify(Char::Auth, {opAuthStatus, v.status, v.detail});
    logf("%s: challenge answer did not match", addr());
    return false;
  }

  AuthVerdict v = sensor->authenticated(link->addr, displayType, newKey);
  link->notify(Char::Auth, {opAuthStatus, v.status, v.detail});
  if (v.status != 0x01) {
    logf("%s: refused (status %02x detail %02x)", addr(), v.status, v.detail);
    return false;
  }
  logf("%s: authenticated as display type %u", addr(), displayType);
  return true;
}

// MARK: Certificate exchange (fresh pairing only)

bool Handler::certificatePhase() {
  const auto& chain = identity::certChain();
  for (;;) {
    Bytes msg;
    if (!waitAuth(handshakeTimeout, msg)) return false;
    if (msg.size() < 6 || msg[0] != opCertificate) {
      logf("%s: expected a certificate request, got %s", addr(), hexstr(msg).c_str());
      return false;
    }
    uint8_t index = msg[1];
    uint32_t clientCertLen = rd32(&msg[2]);

    // A zero-length request is the terminator: acknowledge and stop.
    if (clientCertLen == 0) {
      link->notify(Char::Auth, {opCertificate, 0x00, index, 0x00, 0x00});
      return true;
    }

    // Our certificate at this index (the dummy CA at 0, the leaf at 1); the
    // ack declares its length as 4-byte little-endian.
    Bytes cert = index < chain.size() ? chain[index] : Bytes();
    Bytes ack = {opCertificate, 0x00, index};
    le32(ack, cert.size());
    link->notify(Char::Auth, ack);
    notifyCert(cert);

    Bytes theirs;
    if (!waitCert(clientCertLen, handshakeTimeout, theirs)) return false;
  }
}

// MARK: Key challenge (fresh pairing only)

bool Handler::keyChallenge() {
  Bytes msg;
  if (!waitAuth(handshakeTimeout, msg)) return false;
  if (msg.size() < 17 || msg[0] != opKeyChallenge) {
    logf("%s: expected the key challenge, got %s", addr(), hexstr(msg).c_str());
    return false;
  }
  Bytes displayNonce(msg.begin() + 1, msg.begin() + 17);

  // Our challenge to the display: 16 bytes it must sign with Dexcom's display key.
  Bytes sensorChallenge = randomBytes(16);
  Bytes ack = {opKeyChallenge, 0x00};
  ack.insert(ack.end(), sensorChallenge.begin(), sensorChallenge.end());
  link->notify(Char::Auth, ack);

  // Sign the display's nonce with the sensor leaf key, as a real sensor does.
  notifyCert(identity::signChallenge(displayNonce));

  // The display answers with its signature over our challenge. Advisory, as
  // in g7sim: logged, not enforced.
  Bytes displaySig;
  if (!waitCert(64, handshakeTimeout, displaySig)) return false;
  if (identity::verifyDisplaySignature(sensorChallenge, displaySig)) {
    logf("%s: display authenticated — holds Dexcom's display key", addr());
  } else {
    logf("%s: display's key-challenge signature did not verify (advisory)", addr());
  }
  return true;
}

// MARK: Finalize

bool Handler::finalize() {
  Bytes msg;
  if (!waitAuth(handshakeTimeout, msg) || msg.empty() || msg[0] != opFinalize) return false;
  link->notify(Char::Auth, {opFinalize, 0x1E});
  if (!waitAuth(handshakeTimeout, msg) || msg.empty() || msg[0] != opBondRequest) return false;
  link->notify(Char::Auth, {opBondRequest, 0x00});
  // The sensor confirms the bond; the client treats this as optional.
  link->notify(Char::Auth, {opBondConfirmed, 0x00});
  logf("%s: paired", addr());
  return true;
}

// MARK: Session

// A real G7 hangs up a few seconds after the phone stops asking. This link is
// the pod's too, so the session stays open until the link ends or the phone
// opens a new handshake; the radio drops the link at the next reading.
void Handler::session() {
  for (;;) {
    Bytes msg;
    if (waitControl(1 * SEC, msg)) {
      if (!msg.empty()) handleControl(msg);
      continue;
    }
    if (!link->connected || !authQ.empty()) return;
  }
}

void Handler::handleControl(const Bytes& msg) {
  switch (msg[0]) {
    case opGlucose: {
      Bytes g = sensor->glucoseMessage();
      if (g.empty()) return;
      link->notify(Char::Control, g);
      sensor->served(link->addr, displayType);
      link->served = true;
      break;
    }
    case opBackfill: {
      if (msg.size() >= 9) {
        uint32_t start = rd32(&msg[1]), end = rd32(&msg[5]);
        auto records = sensor->backfill(start, end);
        // Two 9-byte records per notification, as a G7 packs them.
        for (size_t i = 0; i < records.size(); i += 2) {
          Bytes frame = backfillRecord(records[i]);
          if (i + 1 < records.size()) {
            Bytes second = backfillRecord(records[i + 1]);
            frame.insert(frame.end(), second.begin(), second.end());
          }
          if (!link->notify(Char::Backfill, frame)) break;
        }
        logf("%s: backfilled %u readings (%u..%u)", addr(), (unsigned)records.size(), start, end);
      }
      // The acknowledgement doubles as the end-of-stream marker.
      link->notify(Char::Control, {opBackfill, 0x00});
      break;
    }
    case opExtendedVersion:
      link->notify(Char::Control, sensor->extendedVersion());
      break;
    case opTransmitterVer:
      link->notify(Char::Control, sensor->transmitterVersion());
      break;
    case opCalibrate:
      if (msg.size() >= 7) {
        uint16_t glucose = rd16(&msg[1]);
        uint32_t age = rd32(&msg[3]);
        sensor->calibrate(glucose, age, displayType);
        link->notify(Char::Control, {opCalibrate, 0x00, 0x01, 0x00});  // accepted
        logf("%s: calibrated to %u mg/dL at sensor age %us", addr(), glucose, age);
      }
      break;
    case opCalibrationBounds:
      link->notify(Char::Control, sensor->calibrationBounds());
      break;
    case opDisconnect:
      logf("%s: client asked to disconnect", addr());
      break;
    default:
      logf("%s: unhandled control opcode %02x", addr(), msg[0]);
  }
}

// MARK: Reading the central's writes

void Handler::stash(InMsg& m) {
  if (m.subscribed) return;
  switch (m.ch) {
    case Char::Auth: authQ.push_back(std::move(m.data)); break;
    case Char::Control: ctrlQ.push_back(std::move(m.data)); break;
    case Char::Certificate: certBuf.insert(certBuf.end(), m.data.begin(), m.data.end()); break;
    default: break;
  }
}

bool Handler::waitAuth(int64_t timeout, Bytes& out) {
  if (!authQ.empty()) {
    out = std::move(authQ.front());
    authQ.pop_front();
    return true;
  }
  int64_t deadline = monoMs() + timeout;
  for (;;) {
    InMsg m;
    if (!link->next(m, deadline - monoMs())) return false;
    if (!m.subscribed && m.ch == Char::Auth) {
      out = std::move(m.data);
      return true;
    }
    stash(m);
  }
}

bool Handler::waitControl(int64_t timeout, Bytes& out) {
  if (!ctrlQ.empty()) {
    out = std::move(ctrlQ.front());
    ctrlQ.pop_front();
    return true;
  }
  int64_t deadline = monoMs() + timeout;
  for (;;) {
    InMsg m;
    if (!link->next(m, deadline - monoMs())) return false;
    if (!m.subscribed && m.ch == Char::Control) {
      out = std::move(m.data);
      return true;
    }
    stash(m);
  }
}

bool Handler::waitCert(size_t n, int64_t timeout, Bytes& out) {
  int64_t deadline = monoMs() + timeout;
  while (certBuf.size() < n) {
    InMsg m;
    if (!link->next(m, deadline - monoMs())) return false;
    stash(m);
  }
  out.assign(certBuf.begin(), certBuf.begin() + n);
  certBuf.erase(certBuf.begin(), certBuf.begin() + n);
  return true;
}

// Streams a payload on the certificate characteristic in 20-byte chunks the
// client reassembles by byte count.
void Handler::notifyCert(const Bytes& data) {
  for (size_t i = 0; i < data.size(); i += certMTU) {
    size_t end = std::min(i + certMTU, data.size());
    if (!link->notify(Char::Certificate, Bytes(data.begin() + i, data.begin() + end))) return;
  }
}

struct TaskArg {
  std::shared_ptr<Link> link;
  Sensor* sensor;
};

void handlerTask(void* p) {
  TaskArg* a = static_cast<TaskArg*>(p);
  std::shared_ptr<Link> link = std::move(a->link);
  Sensor* sensor = a->sensor;
  delete a;
  {
    Handler h(link, sensor);
    while (link->connected) {
      if (!h.protocol()) {
        // The phone starts over on a new connection (the pod's side of the
        // link reconnects with it).
        link->close();
        break;
      }
    }
  }
  link->finished = true;
  link.reset();
  vTaskDelete(nullptr);
}

}  // namespace

void startHandler(std::shared_ptr<Link> link, Sensor* sensor) {
  // J-PAKE and ECDSA need a roomy stack.
  auto* a = new TaskArg{std::move(link), sensor};
  if (xTaskCreate(handlerTask, "g7link", 16384, a, 5, nullptr) != pdPASS) {
    logf("cannot start a link task");
    a->link->close();
    delete a;
  }
}
