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
struct Link {
  uint16_t handle;
  bool used;
  bool podUsed;
  bool medtrumUsed;
  bool cgmSubscribed;  // subscribed to a characteristic that carries readings
  uint32_t lastTraffic;
  // Beyond the bookkeeping above: whether the CGM touched this link (its first
  // GATT event), whether it has been sent a reading since it connected (the
  // hang-up's "collected" test), whether the injected-accept decision was
  // deferred until the first event named the device, and a generation so a
  // queued notification cannot reach a link that reused the handle.
  bool cgmUsed;
  bool cgmCollected;
  bool acceptChecked;
  uint8_t acceptDevice;
  uint32_t gen;
};
static Link links[kMaxLinks];
// Unique per connection, never reset: a queued CGM notification or a pending
// termination must not match a later link that reused the handle.
static uint32_t linkGenCounter = 0;
// The CCCD value per link and CGM characteristic: a notification goes to every
// central subscribed to the characteristic that carries it. Guarded by mu.
static uint16_t cgmSubs[kMaxLinks][CGM_CHR_COUNT];
// The handle of the subscribed central that made the last hang-up wait (0 when
// none did), only so the wait is logged once per handle.
static volatile uint16_t cgmHeldBy = 0;

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
// A hang-up the core asked for, done from tick(): the core calls
// radio::cgmDisconnect from inside its own mutex, so no sensor:: call may
// happen here before that call returns. The CGM's links may share the radio
// with a pump, so the hang-up waits for a pause in that pump's traffic, with a
// 20 s deadline; it also waits until every subscribed CGM central has been
// sent a reading (see tick()).
static volatile bool cgmDrop = false;
// The 20 s deadline: a separate flag, so a zero timestamp is not also "no
// deadline" (tick is the only writer while the drop is pending; cgmDisconnect
// starts a fresh one when a new request arrives).
static volatile bool cgmDropTimed = false;
static volatile uint32_t cgmDropAt = 0;  // 32-bit monotonic ms; tick's clock

// The CGM's notifications go through a queue and a task of their own: the core
// calls radio::cgmNotify from inside its mutex, so the injected delay is
// waited out here, never while the core's mutex is held. The order is kept,
// and every recipient's link, generation and subscription are re-checked after
// the wait.
struct CgmOut {
  struct To {
    uint16_t handle;  // the link it was queued for
    uint32_t gen;     // ...and its generation
  };
  To to[kMaxLinks];
  uint8_t nTo;       // 0..kMaxLinks: every central subscribed at queue time
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

// "The" link for the single-link pump code: the first live handle. The pump's
// own callbacks use podLinkLocked()/medtrumLinkLocked() below; this is the
// conn-style answer where a caller only needs to know whether anything is up.
static uint16_t firstHandleLocked() {
  for (int i = 0; i < kMaxLinks; i++) {
    if (links[i].used) return links[i].handle;
  }
  return BLE_HS_CONN_HANDLE_NONE;
}

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

// A link the CGM never touched: a pump phone that has connected but whose
// first GATT event has not marked the link yet.
static Link* firstNonCgmLocked() {
  for (int i = 0; i < kMaxLinks; i++) {
    if (links[i].used && !links[i].cgmUsed) return &links[i];
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

static bool isLive(uint16_t h) {
  std::lock_guard<std::mutex> l(mu);
  return slotLocked(h) >= 0;
}

// A GATT event without a descriptor (the NimBLE version this board pins gives
// one, but the old code tolerated its absence): pick a live link.
static uint16_t eventHandle(const ble_gap_conn_desc* desc) {
  if (desc) return desc->conn_handle;
  std::lock_guard<std::mutex> l(mu);
  return firstHandleLocked();
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
    if (!lk->acceptChecked) {
      lk->acceptChecked = true;
      lk->acceptDevice = (uint8_t)device;
      decide = true;
    } else if (lk->acceptDevice != (uint8_t)device) {
      return true;  // the first event on the link already named its device
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
  lk->lastTraffic = (uint32_t)monoMs();
  if (podSide ? !lk->podUsed : !lk->medtrumUsed) {
    if (podSide) lk->podUsed = true;
    else lk->medtrumUsed = true;
    fresh = true;
  }
  return fresh;
}

// The CGM's first GATT event on a link: the core is told once per link (the
// glue maps that to its one session), and the link is marked the CGM's.
static bool markCgmUsed(uint16_t h) {
  std::lock_guard<std::mutex> l(mu);
  Link* lk = linkLocked(h);
  if (!lk) return false;
  lk->lastTraffic = (uint32_t)monoMs();
  if (lk->cgmUsed) return false;
  lk->cgmUsed = true;
  return true;
}

static void setCgmSub(uint16_t h, cgm_chr_t chr, uint16_t subValue) {
  std::lock_guard<std::mutex> l(mu);
  int i = slotLocked(h);
  if (i < 0) return;
  cgmSubs[i][chr] = subValue;
  // "Subscribed" for the hang-up means subscribed to something that carries a
  // reading: CONTROL (the glucose notifications) or BACKFILL (the records a
  // receiver catches up on). Auth and COMM alone are not a reading collector.
  links[i].cgmSubscribed = cgmSubs[i][CGM_CHR_CONTROL] != 0 || cgmSubs[i][CGM_CHR_BACKFILL] != 0;
}

// Whether this notification carries a reading, so the link that receives it has
// collected. CONTROL is the glucose message (G6 0x31/0x4f, G7 0x4e); BACKFILL
// carries the missed records. Other CONTROL replies (session status, version)
// are not the reading.
static bool cgmCarriesReading(uint8_t chr, const uint8_t* d, size_t n) {
  if (chr == CGM_CHR_BACKFILL) return n >= 2;
  if (chr != CGM_CHR_CONTROL || n < 1) return false;
  return d[0] == 0x31 || d[0] == 0x4f || d[0] == 0x4e;
}

// Ends one link, but only the same connection: the handle may have been reused
// while tick() or setPump() was deciding.
static void terminateLink(uint16_t h, uint32_t gen) {
  bool same;
  {
    std::lock_guard<std::mutex> l(mu);
    Link* lk = linkLocked(h);
    same = lk && lk->gen == gen;
  }
  if (same) ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
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

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
    uint16_t h = desc->conn_handle;
    std::string addr = addrString(desc->peer_ota_addr);
    {
      std::lock_guard<std::mutex> l(mu);
      if (!freeLinkLocked()) {
        logf("%s: a third central; two links at a time, closing it", addr.c_str());
        s->disconnect(h);
        return;
      }
    }
    // With no pump selected and the CGM off, nothing on the board invites a
    // phone: accept the connection (the BLE stack already has) and drop it at
    // once. With the CGM on the link is the CGM's to use; a pump that reaches
    // for its service anyway is dropped by the pump callbacks below.
    if (pump() == PUMP_OFF && sensor::kind() == CGM_OFF) {
      logf("%s: no simulator is selected; closing it", addr.c_str());
      s->disconnect(h);
      return;
    }
    // An injected drop/refusal is per device. If the advertisements name the
    // only device that could be connecting, decide now; with both sets on the
    // air the answer comes with the link's first GATT event (acceptCheck),
    // which knows which service the phone reached for.
    int sel = pump();
    bool cgmOff = sensor::kind() == CGM_OFF;
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
      lk->lastTraffic = (uint32_t)monoMs();
      lk->gen = ++linkGenCounter;
      lk->acceptChecked = device >= 0;
      lk->acceptDevice = device >= 0 ? (uint8_t)device : 0;
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
    {
      std::lock_guard<std::mutex> l(mu);
      int i = slotLocked(h);
      if (i < 0) return;
      hadCgm = links[i].cgmUsed;
      hadPod = links[i].podUsed;
      hadMed = links[i].medtrumUsed;
      // No stale CCCD: a notification queued for this link is dropped (the
      // generation check), and a new central has to subscribe afresh.
      for (int c = 0; c < CGM_CHR_COUNT; c++) cgmSubs[i][c] = 0;
      links[i] = Link{};
      anyCgmLeft = cgmCountLocked() > 0;
      // Mirror the core's silent advertising-off after a connection. The core
      // set c->advertising = false in cgm_on_connect without calling
      // advertise(false); onDisconnect only clears c->connected, so a window
      // the core considers closed is never announced. With no CGM link left,
      // the radio's copy can only follow that false; if the core opens a new
      // window it calls advertise(true) again.
      if (!anyCgmLeft) cgmAdvOn = false;
    }
    // The hang-up belongs to the CGM session; it survives one central leaving
    // while another is still on the air.
    if (!anyCgmLeft) {
      cgmDrop = false;
      cgmDropTimed = false;
      cgmHeldBy = 0;
    }
    std::string addr = addrString(desc->peer_ota_addr);
    logf("%s disconnected (handle 0x%04x; %s%s%s)", addr.c_str(), h, hadCgm ? "cgm " : "",
         hadPod ? "pod " : "", hadMed ? "medtrum" : "");
    // The core is told once per link; the glue maps that to its one session
    // (the first connect opens it, the last disconnect closes it).
    if (hadCgm) sensor::onDisconnect();
    if (hadMed) {
      medtrumStatusNotify = false;
      medtrumPush(MEDTRUM_EV_DISCONNECTED, nullptr, 0);
    }
    if (hadPod) podPush(DASH_EV_DISCONNECTED, nullptr, 0);
  }
};

// One GATT event each way for the core: the core's own protocol runs inside
// sensor::, so the radio only maps UUID to cgm_chr_t and hands the bytes over.

// The CGM's first event on a link: tell the core, and mark the link the CGM's.
// The core is told once per link; sensor_app maps the first connect to its one
// session and the last disconnect out of it.
//
// The CGM advertising set is deliberately NOT cleared here. cgm_on_connect()
// sets the core's c->advertising = false without calling advertise(false), and
// with two links the set must stay on while a slot remains: otherwise a watch
// that scans after the phone connected could never find the sensor. tick()
// stops the set once both links are the CGM's. When the last CGM link leaves,
// onDisconnect() clears the radio's copy to match that silent false; the core
// calls advertise(true) again if it opens a new window.
static void cgmLinkUp(uint16_t h) {
  if (!markCgmUsed(h)) return;
  sensor::onConnect();
}

class CgmCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit CgmCallbacks(cgm_chr_t chr) : chr(chr) {}

  void onWrite(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    uint16_t h = eventHandle(desc);
    if (!isLive(h)) return;
    if (!acceptCheck(h, FAIL_CGM, "cgm")) return;
    cgmLinkUp(h);
    if (fail_decide(FAIL_CGM, FAIL_CONSUME_MESSAGE, 0)) {
      logf("cgm: injected failure: message ignored");
      return;
    }
    NimBLEAttValue v = ch->getValue();
    sensor::onWrite(chr, v.data(), v.length());
  }

  void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc* desc, uint16_t subValue) override {
    uint16_t h = eventHandle(desc);
    if (!isLive(h)) return;
    if (!acceptCheck(h, FAIL_CGM, "cgm")) return;
    cgmLinkUp(h);
    // The core answers a subscription at once (the auth status, the reading), so
    // the CCCD value is recorded first: cgmNotify sends only to subscribers.
    setCgmSub(h, chr, subValue);
    sensor::onSubscribe(chr, subValue != 0);
  }

  void onRead(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    uint16_t h = eventHandle(desc);
    if (!isLive(h)) return;
    if (!acceptCheck(h, FAIL_CGM, "cgm")) return;
    cgmLinkUp(h);
    uint8_t buf[64];
    size_t n = sensor::onRead(chr, buf, sizeof buf);
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
    if (!isLive(h)) return;
    if (!pumpSelected(true)) {
      pumpRefuse(h, "pod");
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
    if (!isLive(h)) return;
    if (!pumpSelected(true)) {
      pumpRefuse(h, "pod");
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
    if (!isLive(h)) return;
    if (!pumpSelected(false)) {
      pumpRefuse(h, "medtrum");
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
    if (!isLive(h)) return;
    if (!pumpSelected(false)) {
      pumpRefuse(h, "medtrum");
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
  // A hang-up the core asked for: carried out here, never from inside the
  // call that asked (the core holds its mutex there, and the NimBLE disconnect
  // callback calls back into the core). With two CGM links it must end only
  // when every subscribed central has collected (or the 20 s deadline fires);
  // the pump's traffic pause matters only while a pump shares the radio, and
  // the deadline bounds that wait too.
  int64_t now = monoMs();
  uint32_t now32 = (uint32_t)now;  // the cross-task timestamps are 32-bit
  if (cgmDrop) {
    uint16_t victims[kMaxLinks];
    uint32_t gens[kMaxLinks];
    int nVictims = 0;
    uint16_t heldBy = 0;
    uint32_t pumpTraffic = 0;
    bool pumpTrafficKnown = false;
    int cgmCount = 0;
    {
      std::lock_guard<std::mutex> l(mu);
      for (int i = 0; i < kMaxLinks; i++) {
        Link& lk = links[i];
        if (!lk.used) continue;
        if (lk.podUsed) {
          pumpTraffic = lk.lastTraffic;
          pumpTrafficKnown = true;
        }
        if (!lk.cgmUsed) continue;
        cgmCount++;
        victims[nVictims] = lk.handle;
        gens[nVictims] = lk.gen;
        nVictims++;
        // A subscribed central that has not been sent a reading yet is still
        // collecting. Its handle is the one that holds the hang-up: every CGM
        // link stays up until all of them have collected.
        if (lk.cgmSubscribed && !lk.cgmCollected && heldBy == 0) heldBy = lk.handle;
      }
    }
    if (!cgmCount) {
      // The links are gone, or the CGM never used them: nothing to end here.
      cgmDrop = false;
      cgmDropTimed = false;
      cgmHeldBy = 0;
    } else {
      if (!cgmDropTimed) {
        cgmDropTimed = true;
        cgmDropAt = now32;
      }
      if (heldBy) {
        if (cgmHeldBy != heldBy) {
          cgmHeldBy = heldBy;
          logf("cgm: hang-up held open by handle 0x%04x: it is subscribed to the reading but has not collected yet", heldBy);
        }
      } else {
        cgmHeldBy = 0;
      }
      // Signed elapsed: another task may stamp lastTraffic after now32 was
      // taken, which must read as "just now" (a small negative), not as a huge
      // unsigned idle. Horizon: 2^31 ms (~24 days), far beyond the deadline.
      int32_t sincePod = pumpTrafficKnown ? (int32_t)(now32 - pumpTraffic) : (1 << 30);
      int32_t sinceDrop = (int32_t)(now32 - cgmDropAt);
      bool overdue = sinceDrop >= 20000;
      // All subscribed centrals have collected and the pump has paused, or the
      // 20 s deadline fired. Only the same connections are ended: the handle
      // may have been reused while this tick decided (terminateLink checks).
      if ((!heldBy && sincePod >= 1500) || overdue) {
        cgmDrop = false;
        cgmDropTimed = false;
        cgmHeldBy = 0;
        logf("cgm: hanging up so the centrals collect the reading");
        for (int i = 0; i < nVictims; i++) terminateLink(victims[i], gens[i]);
      }
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
  int cgmCount = cgmCountLocked();
  l.unlock();

  // Who wants to be found? Two sets: instance 0 is the pump's (the pod, or the
  // Medtrum base when the selector says so; never both, never the CGM),
  // instance 1 is the CGM's. The pump set stops once its own phone is on the
  // link; another link (a CGM central) does not stop it. The CGM set stays on
  // while it has a free link, so a watch can find the sensor after the phone
  // connected, and closes when both links are the CGM's, when the core's
  // window closes, or while a hang-up is pending. An injected quiet window only
  // clears that device's want flag; its payload string is untouched.
  // The pod and the Medtrum base are mutually exclusive: exactly one of the two
  // may use the pump set, and PUMP_OFF lets neither.
  int sel = pump();
  bool podWants = sel == PUMP_POD && !podData.empty() && (!podUp || !paired);
  bool medWants = sel == PUMP_MEDTRUM && medOn && !medData.empty() && !medUp;
  bool cgmWants = (int)sensor::kind() != 0 && cgmOn && cgmCount < kMaxLinks && !cgmDrop;
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

// Sends every queued notification to each central that was subscribed to the
// characteristic when it was queued, after the injected delay. "Every" is the
// difference from the old single-link code: a phone and a watch subscribed to
// CONTROL both receive the reading.
static void cgmSendTask(void*) {
  CgmOut m = {};
  for (;;) {
    if (xQueueReceive(cgmOut, &m, portMAX_DELAY) != pdTRUE) continue;
    unsigned delay = (unsigned)fail_decide(FAIL_CGM, FAIL_NOTIFY_DELAY_MS, 0);
    if (delay) vTaskDelay(pdMS_TO_TICKS(delay));
    for (int r = 0; r < m.nTo; r++) {
      uint16_t c = m.to[r].handle;
      uint16_t sub = 0;
      bool live = false;
      {
        std::lock_guard<std::mutex> l(mu);
        int i = slotLocked(c);
        if (i >= 0 && links[i].gen == m.to[r].gen && links[i].cgmUsed) {
          live = true;
          sub = cgmSubs[i][m.chr];
        }
      }
      // The central may have left, or a new one taken the same handle, while we
      // waited: a queued message belongs to the link it was queued for.
      if (!live) continue;
      if (!sub) {
        logf("cgm: %s -> %s dropped: handle 0x%04x is no longer subscribed", cgmCharNames[m.chr],
             hexstr(m.data, m.len).c_str(), c);
        continue;
      }
      os_mbuf* om = ble_hs_mbuf_from_flat(m.data, m.len);
      if (!om) {
        logf("cgm: notification dropped: no buffers");
        continue;
      }
      // A central subscribes to notify, or (the auth characteristic) to indicate.
      int rc = (sub & 1) ? ble_gattc_notify_custom(c, cgmHandles[m.chr], om)
                         : ble_gattc_indicate_custom(c, cgmHandles[m.chr], om);
      if (rc != 0) {
        logf("cgm: notification to handle 0x%04x failed: %d", c, rc);
        continue;
      }
      // Delivered: if it carries the reading, this central has collected, so it
      // no longer holds a hang-up.
      std::lock_guard<std::mutex> l(mu);
      Link* lk = linkLocked(c);
      if (lk && lk->gen == m.to[r].gen) {
        if (cgmCarriesReading(m.chr, m.data, m.len)) lk->cgmCollected = true;
        lk->lastTraffic = (uint32_t)monoMs();
      }
    }
  }
}

// Called by the core from inside its own mutex: queue the message for every
// central subscribed to its characteristic and return. The task above sends
// it once per central, as the reference's ble_cgm_send does (no ENOMEM retry
// loop), and applies that device's injected delay once.
void cgmNotify(cgm_chr_t chr, const uint8_t* p, size_t n) {
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
    for (int i = 0; i < kMaxLinks; i++) {
      if (!links[i].used || !links[i].cgmUsed || cgmSubs[i][chr] == 0) continue;
      m.to[m.nTo].handle = links[i].handle;
      m.to[m.nTo].gen = links[i].gen;
      m.nTo++;
    }
  }
  if (m.nTo == 0) {
    logf("cgm: %s -> no subscribed central", cgmCharNames[chr]);
    return;
  }
  if (xQueueSend(cgmOut, &m, 0) != pdTRUE) logf("cgm: notification queue full: a message was dropped");
}

// The core calls this from inside its own mutex, possibly on the NimBLE
// callback thread: only mark the hang-up. tick() terminates the CGM's links
// (if the CGM ever used them), and the NimBLE disconnect callback then calls
// sensor::onDisconnect for each.
void cgmDisconnect() {
  bool any;
  {
    std::lock_guard<std::mutex> l(mu);
    any = cgmCountLocked() > 0;
  }
  if (!any) return;
  // A request that arrives while none is pending starts a fresh 20 s deadline;
  // the core asks again every second while it is pending, which must not
  // restart it.
  if (!cgmDrop) cgmDropTimed = false;
  cgmDrop = true;
}

bool cgmConnected() {
  std::lock_guard<std::mutex> l(mu);
  return cgmCountLocked() > 0;
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
  // The link may have gone (or been replaced) while we waited: do not send
  // the old device's message to a new phone that reused the handle.
  {
    std::lock_guard<std::mutex> l(mu);
    Link* lk = linkLocked(c);
    if (!lk || lk->gen != gen || !lk->podUsed) return;
    lk->lastTraffic = (uint32_t)monoMs();
  }
  // With a dozen fragments queued, NimBLE can run short of buffers for a
  // moment: wait for them rather than lose a fragment.
  for (int tries = 0; tries < 100; tries++) {
    os_mbuf* om = ble_hs_mbuf_from_flat(p, (uint16_t)n);
    if (om) {
      int rc = ble_gattc_notify_custom(c, handle, om);
      if (rc == 0) return;
      if (rc != BLE_HS_ENOMEM) {
        logf("pod: notification failed: %d", rc);
        return;
      }
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
    // A pump phone that connected but has not reached the pod service yet is
    // still a link the pump may want closed; never pick a CGM-only link.
    if (!lk) lk = firstNonCgmLocked();
    if (lk) {
      c = lk->handle;
      gen = lk->gen;
    }
  }
  if (c != BLE_HS_CONN_HANDLE_NONE) terminateLink(c, gen);
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
  // The link may have gone (or been replaced) while we waited.
  {
    std::lock_guard<std::mutex> l(mu);
    Link* lk = linkLocked(c);
    if (!lk || lk->gen != gen || !lk->medtrumUsed) return;
    lk->lastTraffic = (uint32_t)monoMs();
  }
  // With a dozen fragments queued, NimBLE can run short of buffers for a
  // moment: wait for them rather than lose a fragment.
  for (int tries = 0; tries < 100; tries++) {
    os_mbuf* om = ble_hs_mbuf_from_flat(p, (uint16_t)n);
    if (om) {
      int rc = ble_gattc_notify_custom(c, handle, om);
      if (rc == 0) return;
      if (rc != BLE_HS_ENOMEM) {
        logf("medtrum: notification failed: %d", rc);
        return;
      }
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
    // A base phone that connected but has not reached the Medtrum service yet
    // is still a link the patch may want closed; never pick a CGM-only link.
    if (!lk) lk = firstNonCgmLocked();
    if (lk) {
      c = lk->handle;
      gen = lk->gen;
    }
  }
  if (c != BLE_HS_CONN_HANDLE_NONE) terminateLink(c, gen);
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
