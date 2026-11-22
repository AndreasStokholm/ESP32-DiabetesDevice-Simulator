// SPDX-License-Identifier: GPL-3.0-or-later
#include "radio.h"

#include <NimBLEDevice.h>

#include <mutex>

#include "adv_payload.h"
#include "dash_internal.h"
#include "fail.h"
#include "sensor_app.h"
#include "util.h"

namespace radio {

// The two advertising sets, numbered as ESP-Sim's main/ble.c numbers them:
// instance 0 is the pump's (the pod, or the Medtrum base), instance 1 the
// CGM's. They advertise at the same time.
static const uint8_t pumpInstance = 0;
static const uint8_t cgmInstance = 1;

// MARK: - The CGM's GATT layout (the core's characteristics, lib/cgmsim)

static const char* cgmServiceUUID = "F8083532-849E-531C-C594-30F1F86A4EA5";
// Indexed by cgm_chr_t, NOT by the old link.h Char enum: the core numbers the
// characteristics differently (COMM, CONTROL, AUTH, BACKFILL) and has a slot
// for COMM, which the old chars[4] did not.
static const char* cgmCharUUIDs[CGM_CHR_COUNT] = {
    "F8083533-849E-531C-C594-30F1F86A4EA5",  // COMM
    "F8083534-849E-531C-C594-30F1F86A4EA5",  // CONTROL
    "F8083535-849E-531C-C594-30F1F86A4EA5",  // AUTH
    "F8083536-849E-531C-C594-30F1F86A4EA5",  // BACKFILL
};
static const char* cgmCharNames[CGM_CHR_COUNT] = {"comm", "control", "auth", "backfill"};
static const char* serviceBUUID = "F8084532-849E-531C-C594-30F1F86A4EA5";
static const char* charEUUID = "F8084533-849E-531C-C594-30F1F86A4EA5";
static const char* charFUUID = "F8084534-849E-531C-C594-30F1F86A4EA5";

// MARK: - The pod's GATT layout (ESP-Sim's ble.c)

static const char* podServiceUUID = "1a7e4024-e3ed-4464-8b7e-751e03d0dc5f";
static const char* podCmdUUID = "1a7e2441-e3ed-4464-8b7e-751e03d0dc5f";
static const char* podDataUUID = "1a7e2442-e3ed-4464-8b7e-751e03d0dc5f";    // DASH
static const char* podO5DataUUID = "1a7e2443-e3ed-4464-8b7e-751e03d0dc5f";  // Omnipod 5

// MARK: - The Medtrum patch's GATT layout (ESP-Sim's ble.c)

static const char* medtrumServiceUUID = "669A9001-0008-968F-E311-6050405558B3";
static const char* medtrumReadUUID = "669a9120-0008-968F-E311-6050405558B3";   // status notifications
static const char* medtrumWriteUUID = "669a9101-0008-968F-E311-6050405558B3";  // responses

static NimBLEServer* server;

static std::mutex mu;

// Up to two centrals at once: a phone and a watch (or a phone's CGM and pump
// connections). The pump and the Medtrum base stay single-link -- they are one
// phone's pump -- so their code asks the helpers below for "the" link. Guarded
// by mu.
static const int kMaxLinks = 2;
static_assert(kMaxLinks == CGM_MAX_SESSIONS, "one core session per Bluetooth link");
struct Link {
  uint16_t handle;
  bool used;
  bool podUsed;
  bool medtrumUsed;
  // The CGM's session for this link (a core session token, 0..CGM_MAX_SESSIONS-1)
  // and whether the CGM touched the link at all. The core owns each central's
  // login, deliveries and backfill cursor; the radio only maps token -> link.
  bool cgmUsed;
  cgm_session_t cgmSession;
  // Frames queued by the core for this session but not yet sent. A hang-up the
  // core asked for waits until this reaches zero, so an injected notification
  // delay cannot close the link before its reading has left the queue.
  int cgmPending;
  // Whether a reading-carrying notification was actually delivered on this
  // link. The panel's collected count only; the core owns the hang-up state.
  bool cgmCollected;
  // One bit per FailDevice whose injected accept decision was taken on this
  // link. Deferred decisions are per (link, device): a link that first touched
  // the CGM service can still exercise a pending pod drop on its first pod
  // event, and vice versa.
  uint8_t acceptMask;
  uint32_t gen;
};
static Link links[kMaxLinks];
// Unique per connection, never reset: a queued CGM notification or a pending
// termination must not match a later link that reused the handle.
static uint32_t linkGenCounter = 0;
// The CCCD value per link and CGM characteristic: the core sends to one
// session, and this says whether that link subscribed to the characteristic.
// Guarded by mu.
static uint16_t cgmSubs[kMaxLinks][CGM_CHR_COUNT];

// The CGM's side of the link: the handles the core's notifications go to and
// the advert the core asked for. Guarded by mu (handles are written before the
// server starts and on subscribe).
static uint16_t cgmHandles[CGM_CHR_COUNT];
static std::string cgmAdvData;
static std::string cgmAdvName;
// The core's advertising window. It is NOT cleared when a central connects:
// with two links the set stays on while a slot remains, so a watch that scans
// after the phone has connected can still find the sensor. The core's own
// cgm_tick closes the window when the session is over; the last CGM
// onDisconnect clears this copy to mirror the core's silent off.
static bool cgmAdvOn = false;
// The sensor's random static address, applied to the CGM advertising instance
// by tick() (the core hands it in with every advertise() call). Guarded by mu
// like the advertisement itself.
static uint8_t cgmAdvAddr[6];
static bool cgmAdvAddrValid = false;
// A hang-up the core asked for, one entry per session. The core calls
// radio::cgmDisconnect(session) from inside its own mutex, so tick() carries
// out the termination: no sensor:: call may happen there before the core's call
// returns. Each entry names the link and generation that held the session when
// the core asked, so a reused link cannot be terminated by a stale request.
// Guarded by mu.
struct CgmDrop {
  bool on;
  uint16_t handle;
  uint32_t gen;
};
static CgmDrop cgmDrops[kMaxLinks];

// The CGM's notifications go through a queue and a task of their own: the core
// calls radio::cgmNotify from inside its mutex, so the injected delay is waited
// out here, never while the core's mutex is held. One queue entry is one core
// send to one session's link; the order is kept and the recipient's link,
// generation and subscription are re-checked after the wait.
struct CgmOut {
  uint16_t handle;  // the link it was queued for
  uint32_t gen;     // ...and its generation
  uint8_t chr;
  uint8_t len;
  uint8_t data[32];
};
static QueueHandle_t cgmOut;
static void cgmSendTask(void*);

// The G7's advertising interval, 160 ms in the extended advertiser's 0.625 ms
// units (the old sensor glue's timing.advertisingInterval).
static const uint32_t cgmAdvInterval = 160 * 1000 / 625;

// The pod's side of the link.
static QueueHandle_t podEvents;
static uint16_t podCmdHandle, podDataHandle, podO5DataHandle;
static std::string podAdvData;  // guarded by mu
static bool podIsO5 = false, podPaired = false;

// The Medtrum patch's side of the link.
static QueueHandle_t medtrumEvents;
static uint16_t medtrumReadHandle, medtrumWriteHandle;
static volatile bool medtrumStatusNotify = false;
static std::string medtrumAdvData;  // guarded by mu
static bool medtrumAdvOn = false;
// Which pump simulator is selected (see radio.h). The default is PUMP_POD, so
// a board with no saved choice behaves as it did before the selector existed.
static int pumpSel = PUMP_POD;

static std::string addrString(const uint8_t* val) {
  char b[18];
  snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", val[5], val[4], val[3], val[2], val[1], val[0]);
  return b;
}

static std::string addrString(const ble_addr_t& a) { return addrString(a.val); }

static void podPush(uint8_t type, const uint8_t* data, size_t n) {
  dash_ev_t ev = {};
  ev.type = type;
  ev.len = (uint8_t)n;
  if (n) memcpy(ev.data, data, n);
  if (xQueueSend(podEvents, &ev, 0) != pdTRUE) logf("pod: event queue full: a packet from the phone was dropped");
}

static void medtrumPush(uint8_t type, const uint8_t* data, size_t n) {
  medtrum_ev_t ev = {};
  ev.type = type;
  ev.len = (uint8_t)n;
  if (n) memcpy(ev.data, data, n);
  if (xQueueSend(medtrumEvents, &ev, 0) != pdTRUE)
    logf("medtrum: event queue full: a packet from the phone was dropped");
}

// MARK: - The links (every reader and writer holds mu)

static int slotLocked(uint16_t h) {
  for (int i = 0; i < kMaxLinks; i++) {
    if (links[i].used && links[i].handle == h) return i;
  }
  return -1;
}

static Link* linkLocked(uint16_t h) {
  int i = slotLocked(h);
  return i < 0 ? nullptr : &links[i];
}

static Link* freeLinkLocked() {
  for (int i = 0; i < kMaxLinks; i++) {
    if (!links[i].used) return &links[i];
  }
  return nullptr;
}

// "The" link for the single-link pump code. The pump's own callbacks use
// podLinkLocked()/medtrumLinkLocked() below; there is no first-live fallback on
// purpose: guessing a link can terminate the CGM's or the other pump's phone.
static Link* podLinkLocked() {
  for (int i = 0; i < kMaxLinks; i++) {
    if (links[i].used && links[i].podUsed) return &links[i];
  }
  return nullptr;
}

static Link* medtrumLinkLocked() {
  for (int i = 0; i < kMaxLinks; i++) {
    if (links[i].used && links[i].medtrumUsed) return &links[i];
  }
  return nullptr;
}

static int liveCountLocked() {
  int n = 0;
  for (int i = 0; i < kMaxLinks; i++) n += links[i].used ? 1 : 0;
  return n;
}

static int cgmCountLocked() {
  int n = 0;
  for (int i = 0; i < kMaxLinks; i++) n += (links[i].used && links[i].cgmUsed) ? 1 : 0;
  return n;
}

// The link a core session is on, or nullptr. Session tokens are not reused by
// a new link until cgm_on_disconnect has closed the old session, and every
// queued frame also carries the link's generation.
static Link* linkForSessionLocked(cgm_session_t session) {
  for (int i = 0; i < kMaxLinks; i++) {
    if (links[i].used && links[i].cgmUsed && links[i].cgmSession == session) return &links[i];
  }
  return nullptr;
}

static bool isLive(uint16_t h) {
  std::lock_guard<std::mutex> l(mu);
  return slotLocked(h) >= 0;
}

// A GATT event without a descriptor cannot name its link: skip it rather than
// guess (with two links a guess can act on the wrong central).
static uint16_t eventHandle(const ble_gap_conn_desc* desc) {
  return desc ? desc->conn_handle : BLE_HS_CONN_HANDLE_NONE;
}

// A connect decision taken when the advertised sets cannot name the device is
// deferred to the link's first GATT event, which does name it: drop/refuse are
// per device, so a pending pod drop must not consume the CGM's first link (or
// the other way round). Returns false when the link was refused (it has been
// closed) and the caller must stop.
static bool acceptCheck(uint16_t h, FailDevice device, const char* who) {
  bool decide = false;
  {
    std::lock_guard<std::mutex> l(mu);
    Link* lk = linkLocked(h);
    if (!lk) return false;
    uint8_t bit = (uint8_t)(1u << (int)device);
    if ((lk->acceptMask & bit) == 0) {
      lk->acceptMask |= bit;
      decide = true;
    }
  }
  if (decide && !fail_decide(device, FAIL_ACCEPT, 0)) {
    logf("%s: injected failure: %s connection dropped", who, fail_device_name(device));
    // This runs inside a NimBLE GATT callback: use the server's disconnect.
    if (server) server->disconnect(h);
    return false;
  }
  return true;
}

// Marks the link as the selected pump's. Returns true the first time, so the
// caller pushes CONNECTED exactly once, at the point the link became a pump's
// (at connect with the CGM off, otherwise on its first pump GATT event).
static bool markPump(uint16_t h, bool podSide) {
  bool fresh = false;
  std::lock_guard<std::mutex> l(mu);
  Link* lk = linkLocked(h);
  if (!lk) return false;
  if (podSide ? !lk->podUsed : !lk->medtrumUsed) {
    if (podSide) lk->podUsed = true;
    else lk->medtrumUsed = true;
    fresh = true;
  }
  return fresh;
}

// The CGM's first GATT event on a link: the link is marked the CGM's and gets a
// core session token (the link's slot). The caller tells the core once, outside
// this lock. A link that is already the CGM's keeps its session.
static bool markCgmUsed(uint16_t h, cgm_session_t* session, bool* fresh) {
  std::lock_guard<std::mutex> l(mu);
  Link* lk = linkLocked(h);
  if (!lk) return false;
  if (!lk->cgmUsed) {
    lk->cgmSession = (cgm_session_t)slotLocked(h);
    lk->cgmUsed = true;
    *fresh = true;
  } else {
    *fresh = false;
  }
  *session = lk->cgmSession;
  return true;
}

static void setCgmSub(uint16_t h, cgm_chr_t chr, uint16_t subValue) {
  std::lock_guard<std::mutex> l(mu);
  int i = slotLocked(h);
  if (i < 0) return;
  cgmSubs[i][chr] = subValue;
}

// Whether a notification carries the reading, for the panel's collected count:
// CONTROL's glucose message (G6 0x31/0x4f, G7 0x4e) or a BACKFILL record.
static bool cgmCarriesReading(uint8_t chr, const uint8_t* d, size_t n) {
  if (chr == CGM_CHR_BACKFILL) return n >= 2;
  if (chr != CGM_CHR_CONTROL || n < 1) return false;
  return d[0] == 0x31 || d[0] == 0x4f || d[0] == 0x4e;
}

// Whether another live link already holds this pump. A pump is one phone's, so
// a second pod/Medtrum phone is refused instead of being merged into the same
// event queue.
static bool pumpOwnedByOther(uint16_t h, bool podSide) {
  std::lock_guard<std::mutex> l(mu);
  for (int i = 0; i < kMaxLinks; i++) {
    if (!links[i].used || links[i].handle == h) continue;
    if (podSide ? links[i].podUsed : links[i].medtrumUsed) return true;
  }
  return false;
}

// Ends one link, but only the same connection: the handle may have been reused
// while tick() or setPump() was deciding. The check and the terminate are under
// one lock, so a disconnect+reuse cannot slip in between (GAP events are
// queued, not dispatched from the call).
static void terminateLink(uint16_t h, uint32_t gen) {
  std::lock_guard<std::mutex> l(mu);
  Link* lk = linkLocked(h);
  if (!lk || lk->gen != gen) return;
  ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
}

// The pump the selector chose. The unselected pump's service stays in the GATT
// table (nothing invites a phone to it: its advertisement is not in the set),
// but a phone with a cached address can still reach it. Such an access must not
// be served, and the link must not stay held, so it is dropped.
static bool pumpSelected(bool podSide) {
  int sel = pump();
  return podSide ? sel == PUMP_POD : sel == PUMP_MEDTRUM;
}

static void pumpRefuse(uint16_t c, const char* who) {
  logf("%s: not selected; closing the link", who);
  // This runs inside a NimBLE GATT callback, so use the server's own
  // disconnect (the same call onConnect uses) rather than the host's GAP API.
  if (server) server->disconnect(c);
}

// Refuse a link for a reason other than the selector (a second phone for the
// same single-link pump). Called from a GATT callback, so it uses the server's
// disconnect rather than the host's GAP API.
static void closeLink(uint16_t c, const char* who, const char* why) {
  logf("%s: %s; closing the link", who, why);
  if (server) server->disconnect(c);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
    uint16_t h = desc->conn_handle;
    std::string addr = addrString(desc->peer_ota_addr);
    bool full = false;
    {
      std::lock_guard<std::mutex> l(mu);
      full = freeLinkLocked() == nullptr;
    }
    if (full) {
      logf("%s: a third central; two links at a time, closing it", addr.c_str());
      s->disconnect(h);
      return;
    }
    // With no pump selected and the CGM off, nothing on the board invites a
    // phone: accept the connection (the BLE stack already has) and drop it at
    // once. With the CGM on the link is the CGM's to use; a pump that reaches
    // for its service anyway is dropped by the pump callbacks below.
    int sel = pump();
    bool cgmOff = sensor::kind() == CGM_OFF;
    if (sel == PUMP_OFF && cgmOff) {
      logf("%s: no simulator is selected; closing it", addr.c_str());
      s->disconnect(h);
      return;
    }
    // With the CGM off the selected pump is the only device that can connect,
    // and it stays on one link. Refuse a second pump phone here, before it can
    // consume an injected drop/refusal.
    if (cgmOff && sel != PUMP_OFF && pumpOwnedByOther(h, sel == PUMP_POD)) {
      logf("%s: a second %s phone; the pump stays on one link, closing it", addr.c_str(),
           sel == PUMP_POD ? "pod" : "Medtrum");
      s->disconnect(h);
      return;
    }
    // An injected drop/refusal is per device. If the advertisements name the
    // only device that could be connecting, decide now; with both sets on the
    // air the answer comes with the link's first GATT event (acceptCheck),
    // which knows which service the phone reached for.
    int device = -1;
    if (sel == PUMP_OFF) device = FAIL_CGM;  // only the CGM can be connecting
    else if (cgmOff) device = sel == PUMP_POD ? FAIL_POD : FAIL_MEDTRUM;
    if (device >= 0 && !fail_decide((FailDevice)device, FAIL_ACCEPT, monoMs())) {
      logf("%s: injected failure: %s connection dropped", addr.c_str(), fail_device_name((FailDevice)device));
      s->disconnect(h);
      return;
    }

    bool podFresh = false, medFresh = false;
    int n = 0;
    {
      std::lock_guard<std::mutex> l(mu);
      Link* lk = freeLinkLocked();
      if (!lk) return;  // the other task took it; the stack will call onDisconnect
      *lk = Link{};
      lk->handle = h;
      lk->used = true;
      lk->gen = ++linkGenCounter;
      lk->acceptMask = device >= 0 ? (uint8_t)(1u << device) : 0;
      // With the CGM off the link can only be a pump's, so mark the selected
      // one now: a pump phone that connects but has not yet subscribed or
      // written must still release its link when the selection changes (see
      // setPump). With the CGM on the link could be the CGM's, so the pump's
      // first GATT event marks it instead.
      if (cgmOff) {
        if (sel == PUMP_POD) {
          lk->podUsed = true;
          podFresh = true;
        } else if (sel == PUMP_MEDTRUM) {
          lk->medtrumUsed = true;
          medFresh = true;
        }
      }
      n = liveCountLocked();
    }
    logf("%s connected (handle 0x%04x, %d of %d links)", addr.c_str(), h, n, kMaxLinks);
    // The CGM is told about the link on its first GATT event, not here (see
    // cgmLinkUp()): a link it never touches is the pod's or the patch's, and
    // the core's CONN_MAX timer must not end it.
    if (podFresh) podPush(DASH_EV_CONNECTED, nullptr, 0);
    if (medFresh) medtrumPush(MEDTRUM_EV_CONNECTED, nullptr, 0);
  }

  void onDisconnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
    uint16_t h = desc->conn_handle;
    bool hadCgm = false, hadPod = false, hadMed = false, anyCgmLeft = false;
    cgm_session_t cgmSession = 0;
    {
      std::lock_guard<std::mutex> l(mu);
      int i = slotLocked(h);
      if (i < 0) return;
      hadCgm = links[i].cgmUsed;
      cgmSession = links[i].cgmSession;
      hadPod = links[i].podUsed;
      hadMed = links[i].medtrumUsed;
      // No stale CCCD: a notification queued for this link is dropped (the
      // generation check), and a new central has to subscribe afresh.
      for (int c = 0; c < CGM_CHR_COUNT; c++) cgmSubs[i][c] = 0;
      cgmDrops[i] = CgmDrop{};
      links[i] = Link{};
      anyCgmLeft = cgmCountLocked() > 0;
      // Mirror the core's silent advertising-off after a connection. The core
      // set c->advertising = false in cgm_on_connect without calling
      // advertise(false); onDisconnect only clears that session, so a window
      // the core considers closed is never announced. With no CGM link left,
      // the radio's copy can only follow that false; if the core opens a new
      // window it calls advertise(true) again.
      if (!anyCgmLeft) cgmAdvOn = false;
    }
    std::string addr = addrString(desc->peer_ota_addr);
    logf("%s disconnected (handle 0x%04x; %s%s%s)", addr.c_str(), h, hadCgm ? "cgm " : "",
         hadPod ? "pod " : "", hadMed ? "medtrum" : "");
    // Close exactly the core session this link held. A watch leaving does not
    // touch the phone's session.
    if (hadCgm) sensor::onDisconnect(cgmSession);
    if (hadMed) {
      medtrumStatusNotify = false;
      medtrumPush(MEDTRUM_EV_DISCONNECTED, nullptr, 0);
    }
    if (hadPod) podPush(DASH_EV_DISCONNECTED, nullptr, 0);
  }
};

// One GATT event each way for the core: the core's own protocol runs inside
// sensor::, so the radio only maps UUID to cgm_chr_t and hands the bytes over.

// The CGM's first event on a link: give the link a core session and tell the
// core, once. The session token is the link's slot, so every later send and
// hang-up names exactly one central; cgmNotify and cgmDisconnect map it back
// through linkForSessionLocked().
//
// The CGM advertising set is deliberately NOT cleared here. cgm_on_connect()
// sets the core's own advertising flag false without calling advertise(false),
// and with two links the set must stay on while a slot remains: otherwise a
// watch that scans after the phone connected could never find the sensor. tick()
// stops the set once both links are the CGM's. When the last CGM link leaves,
// onDisconnect() clears the radio's copy to match that silent false; the core
// calls advertise(true) again if it opens a new window.
static bool cgmLinkUp(uint16_t h, cgm_session_t* session) {
  bool fresh = false;
  if (!markCgmUsed(h, session, &fresh)) return false;
  if (fresh) sensor::onConnect(*session);
  return true;
}

class CgmCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit CgmCallbacks(cgm_chr_t chr) : chr(chr) {}

  void onWrite(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    uint16_t h = eventHandle(desc);
    if (h == BLE_HS_CONN_HANDLE_NONE || !isLive(h)) return;
    if (!acceptCheck(h, FAIL_CGM, "cgm")) return;
    cgm_session_t session;
    if (!cgmLinkUp(h, &session)) return;
    if (fail_decide(FAIL_CGM, FAIL_CONSUME_MESSAGE, 0)) {
      logf("cgm: injected failure: message ignored");
      return;
    }
    NimBLEAttValue v = ch->getValue();
    sensor::onWrite(session, chr, v.data(), v.length());
  }

  void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc* desc, uint16_t subValue) override {
    uint16_t h = eventHandle(desc);
    if (h == BLE_HS_CONN_HANDLE_NONE || !isLive(h)) return;
    if (!acceptCheck(h, FAIL_CGM, "cgm")) return;
    cgm_session_t session;
    if (!cgmLinkUp(h, &session)) return;
    // The core answers a subscription at once (the auth status, the cached
    // reading for that session), so the CCCD value is recorded first: cgmNotify
    // sends only to a subscriber.
    setCgmSub(h, chr, subValue);
    sensor::onSubscribe(session, chr, subValue != 0);
  }

  void onRead(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    uint16_t h = eventHandle(desc);
    if (h == BLE_HS_CONN_HANDLE_NONE || !isLive(h)) return;
    if (!acceptCheck(h, FAIL_CGM, "cgm")) return;
    cgm_session_t session;
    if (!cgmLinkUp(h, &session)) return;
    uint8_t buf[64];
    size_t n = sensor::onRead(session, chr, buf, sizeof buf);
    if (n) ch->setValue(buf, n);
  }

 private:
  cgm_chr_t chr;
};

class ServiceBCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    NimBLEAttValue v = ch->getValue();
    logf("%s service B write %s", addrString(desc->peer_ota_addr).c_str(), hexstr(v.data(), v.length()).c_str());
  }
};

class PodCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit PodCallbacks(bool cmd) : cmd(cmd) {}

  void onWrite(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    uint16_t h = eventHandle(desc);
    if (h == BLE_HS_CONN_HANDLE_NONE || !isLive(h)) return;
    if (!pumpSelected(true)) {
      pumpRefuse(h, "pod");
      return;
    }
    if (pumpOwnedByOther(h, true)) {
      closeLink(h, "pod", "a second pod phone; the pump stays on one link");
      return;
    }
    if (!acceptCheck(h, FAIL_POD, "pod")) return;
    // The pump is one phone's: its first GATT event on a link is what makes
    // that link the pod's when both advertised sets could have invited it.
    if (markPump(h, true)) podPush(DASH_EV_CONNECTED, nullptr, 0);
    NimBLEAttValue v = ch->getValue();
    if (v.length() > DASH_PKT_MAX) return;
    if (fail_decide(FAIL_POD, FAIL_CONSUME_MESSAGE, 0)) {
      logf("pod: injected failure: message ignored");
      return;
    }
    podPush(cmd ? DASH_EV_CMD : DASH_EV_DATA, v.data(), v.length());
  }

  // A pod phone subscribes to the data characteristic as it connects, before
  // its first write; mark the link as the pod's then too (as the Medtrum read
  // callback does), so a switch releases it even before any write.
  void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc* desc, uint16_t) override {
    uint16_t h = eventHandle(desc);
    if (h == BLE_HS_CONN_HANDLE_NONE || !isLive(h)) return;
    if (!pumpSelected(true)) {
      pumpRefuse(h, "pod");
      return;
    }
    if (pumpOwnedByOther(h, true)) {
      closeLink(h, "pod", "a second pod phone; the pump stays on one link");
      return;
    }
    if (!acceptCheck(h, FAIL_POD, "pod")) return;
    if (markPump(h, true)) podPush(DASH_EV_CONNECTED, nullptr, 0);
  }

 private:
  bool cmd;
};

class MedtrumWriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    uint16_t h = eventHandle(desc);
    if (h == BLE_HS_CONN_HANDLE_NONE || !isLive(h)) return;
    if (!pumpSelected(false)) {
      pumpRefuse(h, "medtrum");
      return;
    }
    if (pumpOwnedByOther(h, false)) {
      closeLink(h, "medtrum", "a second Medtrum phone; the base stays on one link");
      return;
    }
    if (!acceptCheck(h, FAIL_MEDTRUM, "medtrum")) return;
    if (markPump(h, false)) medtrumPush(MEDTRUM_EV_CONNECTED, nullptr, 0);
    NimBLEAttValue v = ch->getValue();
    if (v.length() > MEDTRUM_FRAME_MAX) return;
    if (fail_decide(FAIL_MEDTRUM, FAIL_CONSUME_MESSAGE, 0)) {
      logf("medtrum: injected failure: message ignored");
      return;
    }
    medtrumPush(MEDTRUM_EV_WRITE, v.data(), v.length());
  }
};

class MedtrumReadCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc* desc, uint16_t subValue) override {
    uint16_t h = eventHandle(desc);
    if (h == BLE_HS_CONN_HANDLE_NONE || !isLive(h)) return;
    if (!pumpSelected(false)) {
      pumpRefuse(h, "medtrum");
      return;
    }
    if (pumpOwnedByOther(h, false)) {
      closeLink(h, "medtrum", "a second Medtrum phone; the base stays on one link");
      return;
    }
    if (!acceptCheck(h, FAIL_MEDTRUM, "medtrum")) return;
    if (markPump(h, false)) medtrumPush(MEDTRUM_EV_CONNECTED, nullptr, 0);
    medtrumStatusNotify = subValue != 0;
  }
};

static const uint32_t rw = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY |
                           NIMBLE_PROPERTY::INDICATE;
static const uint32_t podRw = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY;

void begin() {
  podEvents = xQueueCreate(64, sizeof(dash_ev_t));
  medtrumEvents = xQueueCreate(16, sizeof(medtrum_ev_t));
  // Deep enough for a backfill burst (a dozen frames plus the records).
  cgmOut = xQueueCreate(64, sizeof(CgmOut));
  if (!cgmOut) {
    logf("cgm: notification queue unavailable; notifications will be dropped");
  } else if (xTaskCreate(cgmSendTask, "cgmsend", 6144, nullptr, 5, nullptr) != pdPASS) {
    logf("cannot start the CGM notification task");
  }

  // The GAP device name is the CGM's: the app's CGM code knows its device by
  // name (Dexcom+id for a G6, DXCM+id for a G7). The pod's code only looks for
  // its own name in the advertisement, which the pump set supplies.
  std::string name = sensor::name();
  if (name.empty()) name = "ESP-Sim";  // no CGM kind set: still a named device
  NimBLEDevice::init(name.c_str());
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(251);  // Omnipod 5 packets are 244 bytes
  logf("Bluetooth address %s (the pump and the patch advertise from it; the CGM uses the sensor's)",
       NimBLEDevice::getAddress().toString().c_str());

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  // NimBLE-Arduino 1.4.3 leaves the extended advertiser's callbacks
  // uninitialised, and calls them when a set stops: install the defaults.
  NimBLEDevice::getAdvertising()->setCallbacks(nullptr);

  NimBLEService* cgm = server->createService(cgmServiceUUID);
  NimBLECharacteristic* cgmChars[CGM_CHR_COUNT];
  for (int i = 0; i < CGM_CHR_COUNT; i++) {
    // ESP-Sim's ble.c gives COMM, AUTH and BACKFILL the READ bit (the G6's
    // active mode reads the auth challenge; CGMBLEKit reads COMM/BACKFILL in
    // some paths) and leaves CONTROL write/notify/indicate. The
    // write-without-response bit is a superset the phone need not use.
    bool readable = (cgm_chr_t)i != CGM_CHR_CONTROL;
    cgmChars[i] = cgm->createCharacteristic(cgmCharUUIDs[i], readable ? (rw | NIMBLE_PROPERTY::READ) : rw);
    cgmChars[i]->setCallbacks(new CgmCallbacks((cgm_chr_t)i));
  }
  NimBLEService* b = server->createService(serviceBUUID);
  for (const char* u : {charEUUID, charFUUID}) b->createCharacteristic(u, rw)->setCallbacks(new ServiceBCallbacks());

  NimBLEService* pod = server->createService(podServiceUUID);
  NimBLECharacteristic* pc = pod->createCharacteristic(podCmdUUID, podRw);
  NimBLECharacteristic* pd = pod->createCharacteristic(podDataUUID, podRw);
  NimBLECharacteristic* po = pod->createCharacteristic(podO5DataUUID, podRw);
  pc->setCallbacks(new PodCallbacks(true));
  pd->setCallbacks(new PodCallbacks(false));
  po->setCallbacks(new PodCallbacks(false));

  NimBLEService* med = server->createService(medtrumServiceUUID);
  NimBLECharacteristic* mr =
      med->createCharacteristic(medtrumReadUUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* mw = med->createCharacteristic(medtrumWriteUUID, podRw);
  mr->setCallbacks(new MedtrumReadCallbacks());
  mw->setCallbacks(new MedtrumWriteCallbacks());

  cgm->start();
  b->start();
  pod->start();
  med->start();
  server->start();
  for (int i = 0; i < CGM_CHR_COUNT; i++) {
    for (int l = 0; l < kMaxLinks; l++) cgmSubs[l][i] = 0;
    cgmHandles[i] = cgmChars[i]->getHandle();
  }
  podCmdHandle = pc->getHandle();
  podDataHandle = pd->getHandle();
  podO5DataHandle = po->getHandle();
  medtrumReadHandle = mr->getHandle();
  medtrumWriteHandle = mw->getHandle();
}

// MARK: - Advertising

// A complete local name field: length, type 0x09, the name.
static std::string named(const std::string& name) {
  uint8_t b[ADV_MAX];
  size_t n = adv_name(name.c_str(), b, sizeof b);
  return n ? std::string((const char*)b, n) : std::string();
}

// (Re)configures one advertising set as a legacy connectable advert and starts
// it. instance 0 is the pump's, instance 1 the CGM's. addr, for the CGM set, is
// the sensor's random static address; NULL keeps the device address.
// Every write below is per instance: setInstanceData() configures this instance
// and sets its advertising data, setScanResponseData() sets this instance's
// scan response, set_addr() sets this instance's random address, and start()
// starts this instance. NimBLE-Arduino 1.4.3 has no device-wide data setter on
// this path (its m_scanResp/m_advParams members are unused), so no field here
// can reach the other set.
static bool advertise(uint8_t instance, const std::string& data, const std::string& scanRsp,
                      uint32_t interval, const uint8_t* addr) {
  NimBLEExtAdvertising* adv = NimBLEDevice::getAdvertising();
  if (ble_gap_ext_adv_active(instance)) adv->stop(instance);
  NimBLEExtAdvertisement a;
  a.setLegacyAdvertising(true);
  a.setConnectable(true);
  a.setScannable(true);
  a.setMinInterval(interval);
  a.setMaxInterval(interval);
  a.setTxPower(9);
  a.setData((const uint8_t*)data.data(), data.size());
  if (addr) {
    // A set with its own random address has to be configured with the random
    // own-address type, and NimBLEExtAdvertisement has no other setter for it.
    // setAddress() also makes setInstanceData() apply the address, and wants
    // it in printed order: the core's addr[] is the controller's little-endian
    // order, so reverse it here.
    uint8_t printed[6];
    for (int i = 0; i < 6; i++) printed[i] = addr[5 - i];
    a.setAddress(NimBLEAddress(printed, BLE_ADDR_RANDOM));
  }
  if (!adv->setInstanceData(instance, a)) return false;
  // The random address belongs to the instance, not to the advertising data.
  // setInstanceData() reconfigures the instance, so apply the address again
  // after it: start() then advertises from the sensor's address, not the
  // chip's. It also keeps working when the payload changes but the address does
  // not, since every reconfiguration goes through here.
  if (addr) {
    ble_addr_t ra;
    ra.type = BLE_ADDR_RANDOM;
    memcpy(ra.val, addr, 6);
    int rc = ble_gap_ext_adv_set_addr(instance, &ra);
    if (rc) {
      logf("advertising set %u: could not set its address: %d", instance, rc);
      return false;
    }
  }
  NimBLEExtAdvertisement r;
  r.setLegacyAdvertising(true);
  r.setData((const uint8_t*)scanRsp.data(), scanRsp.size());
  if (!adv->setScanResponseData(instance, r)) return false;
  return adv->start(instance);
}

// (Re)starts one set with this payload and scan response, or stops it when the
// payload is empty. Each set keeps its own payload and scan-response strings, so
// one set's change never restarts the other and a stale scan response cannot be
// kept: the scan response is re-applied whenever either part changes. Returns
// true when the set was (re)started.
static bool advertiseSet(uint8_t instance, std::string& current, std::string& currentRsp,
                         const std::string& data, const std::string& scanRsp, uint32_t interval,
                         const char* label, const uint8_t* addr = nullptr) {
  if (data.empty()) {
    if (ble_gap_ext_adv_active(instance)) NimBLEDevice::getAdvertising()->stop(instance);
    current.clear();
    currentRsp.clear();
    return false;
  }
  if (ble_gap_ext_adv_active(instance) && data == current && scanRsp == currentRsp) return false;
  if (!advertise(instance, data, scanRsp, interval, addr)) {
    static int64_t last = -60000;
    if (monoMs() - last > 10000) {
      last = monoMs();
      logf("advertising %s could not start (%u bytes)", label, (unsigned)data.size());
    }
    return false;
  }
  current = data;
  currentRsp = scanRsp;
  return true;
}

void cgmAdvertise(bool on, const char* name, const uint8_t addr[6], uint16_t serialCrc) {
  // The CGM's own advertising set, on the sensor's random static address: the
  // core generates it and hands it in here; tick() applies it to instance 1.
  std::string nm = name ? name : "";
  // Exactly the reference's CGM advertisement (ESP-Sim's main/ble.c), built by
  // the pure adv_cgm() the host test pins: flags, the complete list of 16-bit
  // service UUIDs with the 0xFEBC the phones filter their scan on
  // (little-endian), then the complete local name.
  uint8_t buf[ADV_MAX];
  // The advertisement a real sensor sends carries Dexcom's company id, the
  // CRC-16 of the serial and the display types collecting readings; the apps
  // read the serial out of it (and g7sim's build sent exactly this).
  size_t pn = on ? adv_cgm(nm.c_str(), serialCrc, 0x04, buf, sizeof buf) : 0;
  if (on && !pn) logf("cgm: \"%s\" does not fit one advertisement; without one it cannot be found", nm.c_str());
  std::string p(pn ? (const char*)buf : "", pn);
  {
    std::lock_guard<std::mutex> l(mu);
    cgmAdvOn = on && pn != 0;
    cgmAdvData = p;
    cgmAdvName = nm;
    if (addr) {
      memcpy(cgmAdvAddr, addr, 6);
      cgmAdvAddrValid = true;
    }
  }
  logf("cgm: %s as %s", on && pn ? "advertising" : "not advertising", nm.empty() ? "(off)" : nm.c_str());
}

void tick() {
  // A hang-up the core asked for, one session at a time: carried out here,
  // never from inside the call that asked (the core holds its mutex there, and
  // the NimBLE disconnect callback calls back into it). Each entry names the
  // link and generation the session was on, so a reused handle is left alone.
  // A session whose frame is still queued (an injected notification delay) is
  // held until the queue has drained, then terminated.
  int64_t now = monoMs();
  {
    uint16_t victims[kMaxLinks];
    uint32_t gens[kMaxLinks];
    int nVictims = 0;
    {
      std::lock_guard<std::mutex> l(mu);
      for (int i = 0; i < kMaxLinks; i++) {
        if (!cgmDrops[i].on) continue;
        Link* lk = linkLocked(cgmDrops[i].handle);
        if (lk && lk->gen == cgmDrops[i].gen && lk->cgmPending > 0) continue; // wait for the queue
        // The link is gone or its frame is out: terminate the same connection only.
        victims[nVictims] = cgmDrops[i].handle;
        gens[nVictims] = cgmDrops[i].gen;
        nVictims++;
        cgmDrops[i] = CgmDrop{};
      }
    }
    if (nVictims) {
      logf("cgm: hanging up %d session%s", nVictims, nVictims == 1 ? "" : "s");
      for (int i = 0; i < nVictims; i++) terminateLink(victims[i], gens[i]);
    }
  }

  std::unique_lock<std::mutex> l(mu);
  bool cgmOn = cgmAdvOn;
  std::string cgmData = cgmAdvData, cgmNm = cgmAdvName;
  uint8_t cgmAddr[6];
  memcpy(cgmAddr, cgmAdvAddr, sizeof cgmAddr);
  bool cgmAddrValid = cgmAdvAddrValid;
  std::string podData = podAdvData;
  bool paired = podPaired;
  std::string medData = medtrumAdvData;
  bool medOn = medtrumAdvOn;
  bool podUp = podLinkLocked() != nullptr;
  bool medUp = medtrumLinkLocked() != nullptr;
  int live = liveCountLocked();
  l.unlock();

  // Who wants to be found? Two sets: instance 0 is the pump's (the pod, or the
  // Medtrum base when the selector says so; never both, never the CGM),
  // instance 1 is the CGM's. The pump set stops once its own phone is on the
  // link; another link (a CGM central) does not stop it. The CGM set stays on
  // while a link is free, so a watch can find the sensor after the phone
  // connected, and closes when both links are in use or when the core's window
  // closes. A hang-up of one session does not stop the set while the other
  // session (or a free slot) needs it. An injected quiet window only clears
  // that device's want flag; its payload string is untouched.
  // The pod and the Medtrum base are mutually exclusive: exactly one of the two
  // may use the pump set, and PUMP_OFF lets neither.
  int sel = pump();
  bool podWants = sel == PUMP_POD && !podData.empty() && (!podUp || !paired);
  bool medWants = sel == PUMP_MEDTRUM && medOn && !medData.empty() && !medUp;
  bool cgmWants = (int)sensor::kind() != 0 && cgmOn && live < kMaxLinks;
  cgmWants = cgmWants && fail_decide(FAIL_CGM, FAIL_ADVERTISE, now);
  podWants = podWants && fail_decide(FAIL_POD, FAIL_ADVERTISE, now);
  medWants = medWants && fail_decide(FAIL_MEDTRUM, FAIL_ADVERTISE, now);
  // Log the WANT set, not what each set is doing: the wants change only when a
  // device appears or stops. Answers "did Medtrum ever want the set?" in one
  // line, without spamming the console.
  static int lastWants = -1;
  int w = (cgmWants ? 1 : 0) | (podWants ? 2 : 0) | (medWants ? 4 : 0);
  if (w != lastWants) {
    lastWants = w;
    logf("advertising wants: cgm=%d pod=%d medtrum=%d", cgmWants, podWants, medWants);
  }

  // Each set keeps its own payload and scan-response strings: "already carrying
  // this" is decided per set, so one set's change never restarts the other.
  static std::string pumpCurrent, pumpCurrentRsp, cgmCurrent, cgmCurrentRsp;
  // The pump instance is shared by two families, so log its first start and any
  // later start of the other family (the address is the same; which pump is on
  // the air is not). The CGM set has one family and needs one line.
  static std::string pumpLoggedAs;
  static bool cgmLogged = false;
  // The address the CGM set is advertising from. It belongs to the advertising
  // instance rather than the payload, and a new sensor can keep the same name
  // and serial (the same payload) but get a new random address: force the set
  // to be reconfigured when that happens.
  static uint8_t cgmCurrentAddr[6];
  static bool cgmCurrentAddrValid = false;
  bool cgmAddrChanged =
      cgmAddrValid &&
      (!cgmCurrentAddrValid || memcmp(cgmAddr, cgmCurrentAddr, sizeof cgmCurrentAddr) != 0);
  if (cgmAddrChanged) {
    cgmCurrent.clear();
    cgmCurrentRsp.clear();
  }

  // Instance 0: the pump set. The selector names the one pump that may use it;
  // PUMP_OFF, or a pump that does not want to be found, leaves it stopped.
  std::string pumpData, pumpRsp;
  const char* pumpLabel = sel == PUMP_MEDTRUM ? "Medtrum" : "pod";
  if (sel == PUMP_POD && podWants) {
    pumpData = podData;
    pumpRsp = named(DASH_ADVERTISED_NAME);
  } else if (sel == PUMP_MEDTRUM && medWants) {
    pumpData = medData;
    pumpRsp = named("MT");
  }
  if (advertiseSet(pumpInstance, pumpCurrent, pumpCurrentRsp, pumpData, pumpRsp, 0x00f4, pumpLabel) &&
      pumpLoggedAs != pumpLabel) {
    pumpLoggedAs = pumpLabel;
    std::string pumpAddr = NimBLEDevice::getAddress().toString();
    logf("advertising pump on %s (%s, %u bytes)", pumpAddr.c_str(), pumpLabel, (unsigned)pumpData.size());
  }

  // Instance 1: the CGM set, whenever the core wants it. Otherwise it stops.
  std::string cgmSetData, cgmSetRsp;
  if (cgmWants) {
    cgmSetData = cgmData;
    cgmSetRsp = named(cgmNm);
  }
  bool cgmStarted =
      advertiseSet(cgmInstance, cgmCurrent, cgmCurrentRsp, cgmSetData, cgmSetRsp, cgmAdvInterval, "CGM",
                    cgmAddrValid ? cgmAddr : nullptr);
  if (cgmStarted && !cgmLogged) {
    cgmLogged = true;
    std::string cgmAddrText = cgmAddrValid ? addrString(cgmAddr) : "(device address)";
    logf("advertising CGM on %s (%u bytes)", cgmAddrText.c_str(), (unsigned)cgmSetData.size());
  }
  if (cgmStarted && cgmAddrValid) {
    memcpy(cgmCurrentAddr, cgmAddr, sizeof cgmCurrentAddr);
    cgmCurrentAddrValid = true;
  }
}

int openLinks() {
  std::lock_guard<std::mutex> l(mu);
  return liveCountLocked();
}

// MARK: - The CGM's radio

// Marks one queued frame as done. It decrements only the same connection it
// was queued for: a link reuse must not consume the new link's count.
static void cgmReleasePending(uint16_t c, uint32_t gen) {
  std::lock_guard<std::mutex> l(mu);
  Link* lk = linkLocked(c);
  if (lk && lk->gen == gen && lk->cgmPending > 0) lk->cgmPending--;
}

// Sends one queued notification to the one session's link, after the injected
// delay. The core emits one send per session, so a phone and a watch each get
// their own frame; this task only delivers it.
static void cgmSendTask(void*) {
  CgmOut m = {};
  for (;;) {
    if (xQueueReceive(cgmOut, &m, portMAX_DELAY) != pdTRUE) continue;
    unsigned delay = (unsigned)fail_decide(FAIL_CGM, FAIL_NOTIFY_DELAY_MS, 0);
    if (delay) vTaskDelay(pdMS_TO_TICKS(delay));
    uint16_t c = m.handle;
    bool gone = false, notSub = false, noBuf = false;
    int failRc = 0;
    {
      // The liveness/generation/subscription check and the stack call are
      // under one lock: a disconnect + handle reuse cannot slip in between.
      // GATT/GAP events are queued, so the call does not dispatch into us.
      std::lock_guard<std::mutex> l(mu);
      int i = slotLocked(c);
      if (i < 0 || links[i].gen != m.gen || !links[i].cgmUsed) {
        gone = true;
      } else {
        uint16_t sub = cgmSubs[i][m.chr];
        if (!sub) {
          notSub = true;
        } else {
          os_mbuf* om = ble_hs_mbuf_from_flat(m.data, m.len);
          if (!om) {
            noBuf = true;
          } else {
            // A central subscribes to notify, or (the auth characteristic) to
            // indicate.
            failRc = (sub & 1) ? ble_gattc_notify_custom(c, cgmHandles[m.chr], om)
                               : ble_gattc_indicate_custom(c, cgmHandles[m.chr], om);
            if (failRc == 0) {
              if (cgmCarriesReading(m.chr, m.data, m.len)) links[i].cgmCollected = true;
            }
          }
        }
      }
    }
    cgmReleasePending(c, m.gen);
    if (gone) continue;
    if (notSub) {
      logf("cgm: %s -> %s dropped: handle 0x%04x is no longer subscribed", cgmCharNames[m.chr],
           hexstr(m.data, m.len).c_str(), c);
    } else if (noBuf) {
      logf("cgm: notification dropped: no buffers");
    } else if (failRc != 0) {
      logf("cgm: notification to handle 0x%04x failed: %d", c, failRc);
    }
  }
}

// Called by the core from inside its own mutex, with the session that asked for
// this send: queue it for that session's link and return. The task above sends
// it (no ENOMEM retry loop), applying the injected delay once. If the session's
// link has gone, or never subscribed to the characteristic, there is nobody to
// send to. A queued frame counts against the session's hang-up until it has
// actually been sent or dropped.
void cgmNotify(cgm_session_t session, cgm_chr_t chr, const uint8_t* p, size_t n) {
  if (!cgmOut) return;
  if (n > sizeof(CgmOut::data)) {
    logf("cgm: a %u-byte notification does not fit the queue; dropped", (unsigned)n);
    return;
  }
  CgmOut m = {};
  m.chr = (uint8_t)chr;
  m.len = (uint8_t)n;
  if (n) memcpy(m.data, p, n);
  {
    std::lock_guard<std::mutex> l(mu);
    Link* lk = linkForSessionLocked(session);
    if (!lk) {
      logf("cgm: session %u has no live link", (unsigned)session);
      return;
    }
    int i = slotLocked(lk->handle);
    if (i < 0 || cgmSubs[i][chr] == 0) {
      logf("cgm: %s -> session %u is not subscribed", cgmCharNames[chr], (unsigned)session);
      return;
    }
    m.handle = lk->handle;
    m.gen = lk->gen;
    lk->cgmPending++;
  }
  if (xQueueSend(cgmOut, &m, 0) != pdTRUE) {
    cgmReleasePending(m.handle, m.gen);
    logf("cgm: notification queue full: a message was dropped");
  }
}

// The core calls this from inside its own mutex, possibly on the NimBLE
// callback thread, with the one session it is done with: only mark that
// session's hang-up. tick() terminates that link, and the NimBLE disconnect
// callback then calls sensor::onDisconnect for that session.
void cgmDisconnect(cgm_session_t session) {
  std::lock_guard<std::mutex> l(mu);
  Link* lk = linkForSessionLocked(session);
  if (!lk) return;  // the session's link has already gone
  int i = slotLocked(lk->handle);
  if (i < 0) return;
  // The core asks again every second until the link closes; overwriting with
  // the current link is harmless and keeps a reused link's request current.
  cgmDrops[i].on = true;
  cgmDrops[i].handle = lk->handle;
  cgmDrops[i].gen = lk->gen;
}

bool cgmConnected() {
  std::lock_guard<std::mutex> l(mu);
  return cgmCountLocked() > 0;
}

// Panel-only: links that actually received a reading-carrying notification and
// are still subscribed to a reading characteristic. The core owns the hang-up;
// this is the old radio-side count, kept because only the send task knows a
// notification really left the stack.
int cgmCollectedCentrals() {
  std::lock_guard<std::mutex> l(mu);
  int n = 0;
  for (int i = 0; i < kMaxLinks; i++) {
    bool subs = cgmSubs[i][CGM_CHR_CONTROL] != 0 || cgmSubs[i][CGM_CHR_BACKFILL] != 0;
    if (links[i].used && links[i].cgmUsed && links[i].cgmCollected && subs) n++;
  }
  return n;
}

// MARK: - The pod's radio

bool podConnected() {
  std::lock_guard<std::mutex> l(mu);
  return podLinkLocked() != nullptr;
}

bool podNextEvent(dash_ev_t* ev, uint32_t timeoutMs) {
  TickType_t t = timeoutMs == DASH_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
  if (t == 0 && timeoutMs > 0) t = 1;
  return xQueueReceive(podEvents, ev, t) == pdTRUE;
}

static void podNotify(uint16_t handle, const uint8_t* p, size_t n) {
  uint16_t c;
  uint32_t gen;
  {
    std::lock_guard<std::mutex> l(mu);
    Link* lk = podLinkLocked();
    if (!lk) return;  // no pod phone on the air
    c = lk->handle;
    gen = lk->gen;
  }
  unsigned delay = (unsigned)fail_decide(FAIL_POD, FAIL_NOTIFY_DELAY_MS, 0);
  if (delay) vTaskDelay(pdMS_TO_TICKS(delay));
  // With a dozen fragments queued, NimBLE can run short of buffers for a
  // moment: wait for them rather than lose a fragment. The check and the call
  // share the lock, so a disconnect + handle reuse cannot slip between them.
  for (int tries = 0; tries < 100; tries++) {
    bool gone = false;
    int rc = 0;
    {
      std::lock_guard<std::mutex> l(mu);
      Link* lk = linkLocked(c);
      if (!lk || lk->gen != gen || !lk->podUsed) {
        gone = true;
      } else {
        os_mbuf* om = ble_hs_mbuf_from_flat(p, (uint16_t)n);
        if (!om) {
          rc = BLE_HS_ENOMEM;
        } else {
          rc = ble_gattc_notify_custom(c, handle, om);
        }
      }
    }
    if (gone) return;
    if (rc == 0) return;
    if (rc != BLE_HS_ENOMEM) {
      logf("pod: notification failed: %d", rc);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  logf("pod: notification dropped: no buffers");
}

void podNotifyCmd(const uint8_t* p, size_t n) { podNotify(podCmdHandle, p, n); }

void podNotifyData(const uint8_t* p, size_t n) {
  bool o5;
  {
    std::lock_guard<std::mutex> l(mu);
    o5 = podIsO5;
  }
  podNotify(o5 ? podO5DataHandle : podDataHandle, p, n);
}

void podDisconnect() {
  uint16_t c = BLE_HS_CONN_HANDLE_NONE;
  uint32_t gen = 0;
  {
    std::lock_guard<std::mutex> l(mu);
    Link* lk = podLinkLocked();
    if (!lk) return;  // no link is the pod's; never guess another device's
    c = lk->handle;
    gen = lk->gen;
  }
  terminateLink(c, gen);
}

// These two are REQUESTS. The pump selector (radio::pump()) decides whose payload
// the pump advertising set actually carries; only tick() logs what went on air
// ("advertising pump on <addr> ..."), so a "wants" line here is not evidence
// that the device is on the air.
void podAdvertise(const dash_adv_t* adv) {
  bool o5 = adv->pod_type == DASH_POD_O5;
  // The pod's bytes are the reference's; the pure builders in adv_payload.h
  // assemble them and the host test pins them.
  uint8_t buf[ADV_MAX];
  size_t n = 0;
  if (o5) {
    // One 128-bit UUID with the paired controller's id in it.
    uint8_t le[16];
    dash_o5_advertised_uuid(adv->pdm_id, le);
    n = adv_pod_o5(le, buf, sizeof buf);
  } else {
    uint16_t svc[9];
    dash_advertised_services(adv->id, adv->fault, adv->alerts, svc);
    n = adv_pod(svc, buf, sizeof buf);
  }
  std::string p((const char*)buf, n);
  bool paired = o5 ? adv->pdm_id != nullptr : adv->id != nullptr;
  {
    std::lock_guard<std::mutex> l(mu);
    podIsO5 = o5;
    podPaired = paired;
    podAdvData = p;
  }
  logf("pod: wants the advertising set as %s %s pod (fault 0x%02x, alerts 0x%02x)", paired ? "a paired" : "an unpaired",
       o5 ? "Omnipod 5" : "DASH", adv->fault, adv->alerts);
}

// MARK: - The Medtrum patch's radio

bool medtrumConnected() {
  std::lock_guard<std::mutex> l(mu);
  return medtrumLinkLocked() != nullptr;
}

bool medtrumNextEvent(medtrum_ev_t* ev, uint32_t timeoutMs) {
  TickType_t t = timeoutMs == MEDTRUM_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
  if (t == 0 && timeoutMs > 0) t = 1;
  return xQueueReceive(medtrumEvents, ev, t) == pdTRUE;
}

static void medtrumNotifyRaw(uint16_t handle, const uint8_t* p, size_t n) {
  uint16_t c;
  uint32_t gen;
  {
    std::lock_guard<std::mutex> l(mu);
    Link* lk = medtrumLinkLocked();
    if (!lk) return;  // no base phone on the air
    c = lk->handle;
    gen = lk->gen;
  }
  unsigned delay = (unsigned)fail_decide(FAIL_MEDTRUM, FAIL_NOTIFY_DELAY_MS, 0);
  if (delay) vTaskDelay(pdMS_TO_TICKS(delay));
  // Same retry shape as the pod's: the check and the call share the lock, so a
  // disconnect + handle reuse cannot slip between them.
  for (int tries = 0; tries < 100; tries++) {
    bool gone = false;
    int rc = 0;
    {
      std::lock_guard<std::mutex> l(mu);
      Link* lk = linkLocked(c);
      if (!lk || lk->gen != gen || !lk->medtrumUsed) {
        gone = true;
      } else {
        os_mbuf* om = ble_hs_mbuf_from_flat(p, (uint16_t)n);
        if (!om) {
          rc = BLE_HS_ENOMEM;
        } else {
          rc = ble_gattc_notify_custom(c, handle, om);
        }
      }
    }
    if (gone) return;
    if (rc == 0) return;
    if (rc != BLE_HS_ENOMEM) {
      logf("medtrum: notification failed: %d", rc);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  logf("medtrum: notification dropped: no buffers");
}

// A command response, on the WRITE characteristic.
void medtrumNotify(const uint8_t* p, size_t n) { medtrumNotifyRaw(medtrumWriteHandle, p, n); }

// A status notification, on the READ characteristic, once the phone subscribed.
void medtrumNotifyStatus(const uint8_t* p, size_t n) {
  if (medtrumStatusNotify) medtrumNotifyRaw(medtrumReadHandle, p, n);
}

void medtrumDisconnect() {
  uint16_t c = BLE_HS_CONN_HANDLE_NONE;
  uint32_t gen = 0;
  {
    std::lock_guard<std::mutex> l(mu);
    Link* lk = medtrumLinkLocked();
    if (!lk) return;  // no link is the base's; never guess another device's
    c = lk->handle;
    gen = lk->gen;
  }
  terminateLink(c, gen);
}

void setPump(int p) {
  if (p != PUMP_OFF && p != PUMP_POD && p != PUMP_MEDTRUM) return;
  uint16_t drop[kMaxLinks];
  uint32_t gens[kMaxLinks];
  int n = 0;
  {
    std::lock_guard<std::mutex> l(mu);
    pumpSel = p;
    // The pump that is no longer selected must not keep its link. A link the
    // CGM alone touched is not a pump's, so it is left alone.
    for (int i = 0; i < kMaxLinks; i++) {
      Link& lk = links[i];
      if (!lk.used) continue;
      bool heldByOther = (p == PUMP_OFF && (lk.podUsed || lk.medtrumUsed)) ||
                         (p == PUMP_POD && lk.medtrumUsed) || (p == PUMP_MEDTRUM && lk.podUsed);
      if (!heldByOther) continue;
      drop[n] = lk.handle;
      gens[n] = lk.gen;
      n++;
    }
  }
  if (n) {
    logf("pump simulators: dropping the link the unselected pump held");
    for (int i = 0; i < n; i++) terminateLink(drop[i], gens[i]);
  }
}

int pump() {
  std::lock_guard<std::mutex> l(mu);
  return pumpSel;
}

void medtrumAdvertise(bool on, const uint8_t sn[4]) {
  uint8_t snv[4] = {0, 0, 0, 0};
  if (sn) memcpy(snv, sn, 4);
  // Flags, the name "MT", and the manufacturer data the app reads the base
  // serial, device type and version from; the pure builder the host test pins.
  uint8_t buf[ADV_MAX];
  size_t n = adv_medtrum(snv, buf, sizeof buf);
  std::string p((const char*)buf, n);
  {
    std::lock_guard<std::mutex> l(mu);
    medtrumAdvOn = on;
    medtrumAdvData = p;
  }
  logf("medtrum: %s the advertising set, serial %02X%02X%02X%02X", on ? "wants" : "releases", snv[0], snv[1], snv[2],
       snv[3]);
}

}  // namespace radio
