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
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;

// The CGM's side of the link: the handles the core's notifications go to, the
// CCCD value per characteristic, and the advert the core asked for. Guarded by
// mu (handles and subs are written before the server starts and on subscribe).
static uint16_t cgmHandles[CGM_CHR_COUNT];
static volatile uint16_t cgmSubs[CGM_CHR_COUNT];
static std::string cgmAdvData;
static std::string cgmAdvName;
static bool cgmAdvOn = false;
// A hang-up the core asked for, done from tick(): the core calls
// radio::cgmDisconnect from inside its own mutex, so no sensor:: call may
// happen here before that call returns. The link is the pod's too, so the
// hang-up waits for a pause in the pod's traffic, with a 20 s deadline.
static volatile bool cgmDrop = false;
// The 20 s deadline: a separate flag, so a zero timestamp is not also "no
// deadline" (tick is the only writer while the drop is pending; cgmDisconnect
// starts a fresh one when a new request arrives).
static volatile bool cgmDropTimed = false;
static volatile uint32_t cgmDropAt = 0;  // 32-bit monotonic ms; tick's clock
// The CGM was told about the current link (its first GATT event). The core is
// not told at connect time: a link the CGM never touches is the pod's or the
// patch's, and the core's own session timer (CONN_MAX) would end it, logging a
// hang-up every second until it did (see cgmLinkUp()).
static volatile bool cgmUsed = false;
// A new value for every connection, so a notification queued (or delayed) for
// the old link cannot reach a new phone that happens to reuse the handle.
static volatile uint32_t linkGen = 0;

// The CGM's notifications go through a queue and a task of their own: the core
// calls radio::cgmNotify from inside its mutex, so the injected delay is
// waited out here, never while the core's mutex is held. The order is kept,
// and the link, its generation and the subscription are re-checked after the
// wait.
struct CgmOut {
  uint16_t conn;   // the link it was queued for
  uint32_t gen;    // ...and its generation
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
static volatile uint32_t podLastTraffic = 0;

// The Medtrum patch's side of the link.
static QueueHandle_t medtrumEvents;
static uint16_t medtrumReadHandle, medtrumWriteHandle;
static volatile bool medtrumStatusNotify = false;
static std::string medtrumAdvData;  // guarded by mu
static bool medtrumAdvOn = false;
// Which pump simulator is selected (see radio.h). The default is PUMP_POD, so
// a board with no saved choice behaves as it did before the selector existed.
static int pumpSel = PUMP_POD;
// Whether the current link is a pump's: its GATT service was written or
// subscribed, or (with the CGM off) it connected while that pump was selected.
// A link the CGM alone touched is not a pump's, so switching the pump selection
// must not drop it; the flags are cleared on every disconnect.
static volatile bool podUsed = false;
static volatile bool medtrumUsed = false;

static std::string addrString(const ble_addr_t& a) {
  char b[18];
  snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", a.val[5], a.val[4], a.val[3], a.val[2], a.val[1], a.val[0]);
  return b;
}

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
    std::string addr = addrString(desc->peer_ota_addr);
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
      logf("%s: a second phone; one at a time, closing it", addr.c_str());
      s->disconnect(desc->conn_handle);
      return;
    }
    // With no pump selected and the CGM off, nothing on the board invites a
    // phone: accept the connection (the BLE stack already has) and drop it at
    // once, so no phone can sit on the one link. With the CGM on the link is
    // the CGM's to use; a pump that reaches for its service anyway is dropped
    // by the pump callbacks below.
    if (pump() == PUMP_OFF && sensor::kind() == CGM_OFF) {
      logf("%s: no simulator is selected; closing it", addr.c_str());
      s->disconnect(desc->conn_handle);
      return;
    }
    // A refused connect is still accepted by the BLE stack, then dropped at
    // once. One set of decisions for the one shared link; the first device
    // with a pending drop or refusal names it in the log.
    int64_t acceptNow = monoMs();
    for (int i = 0; i < FAIL_DEVICE_COUNT; i++) {
      FailDevice d = (FailDevice)i;
      if (!fail_decide(d, FAIL_ACCEPT, acceptNow)) {
        logf("%s: injected failure: %s connection dropped", addr.c_str(), fail_device_name(d));
        s->disconnect(desc->conn_handle);
        return;
      }
    }
    logf("%s connected", addr.c_str());
    conn = desc->conn_handle;
    ++linkGen;  // anything queued for the old link is stale now
    // The CGM is told about the link on its first GATT event, not here (see
    // cgmLinkUp()): a link it never touches is the pod's or the patch's, and
    // the core's CONN_MAX timer must not end it.
    cgmUsed = false;
    // With the CGM off the link can only be a pump's, so mark the selected one
    // now: a pump phone that connects but has not yet subscribed or written must
    // still release the one link when the selection changes (see setPump). With
    // the CGM on the link could be the CGM's, so the pump's first GATT event
    // marks it instead.
    if (sensor::kind() == CGM_OFF) {
      int sel = pump();
      if (sel == PUMP_POD) podUsed = true;
      else if (sel == PUMP_MEDTRUM) medtrumUsed = true;
    }
    podPush(DASH_EV_CONNECTED, nullptr, 0);
    medtrumPush(MEDTRUM_EV_CONNECTED, nullptr, 0);
  }

  void onDisconnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
    if (desc->conn_handle != conn) return;
    conn = BLE_HS_CONN_HANDLE_NONE;
    cgmDrop = false;
    cgmDropTimed = false;
    bool hadCgm = cgmUsed;
    cgmUsed = false;
    podUsed = false;
    medtrumUsed = false;
    // No stale CCCD: a notification that was queued for this link is dropped
    // (the generation check), and a new phone has to subscribe afresh.
    for (int i = 0; i < CGM_CHR_COUNT; i++) cgmSubs[i] = 0;
    std::string addr = addrString(desc->peer_ota_addr);
    logf("%s disconnected", addr.c_str());
    // Only a link the CGM knew about is the CGM's to close; the core logs a
    // disconnect for every call.
    if (hadCgm) sensor::onDisconnect();
    medtrumStatusNotify = false;
    podPush(DASH_EV_DISCONNECTED, nullptr, 0);
    medtrumPush(MEDTRUM_EV_DISCONNECTED, nullptr, 0);
  }
};

// One GATT event each way for the core: the core's own protocol runs inside
// sensor::, so the radio only maps UUID to cgm_chr_t and hands the bytes over.

// The CGM's first event on a link: tell the core, then mirror its
// advertising-off. cgm_on_connect() sets the core's c->advertising = false
// without calling advertise(false), so the radio's copy is cleared here to
// match; otherwise the CGM set would keep advertising the CGM outside the
// core's 60-second window, and signal loss could not stop it.
//
// The clear comes AFTER sensor::onConnect() returns, and that order is what
// makes it race-safe: a sensor task already inside cgm_tick -> advertise(true)
// holds the core's mutex, so it finishes its cgmAdvertise() before onConnect
// can return, and once the core is connected cgm_tick makes no further
// advertise(true) call. So both orders end with radio false / core false.
static void cgmLinkUp() {
  if (cgmUsed) return;
  cgmUsed = true;
  sensor::onConnect();
  std::lock_guard<std::mutex> l(mu);
  cgmAdvOn = false;
}

class CgmCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit CgmCallbacks(cgm_chr_t chr) : chr(chr) {}

  void onWrite(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    if (desc->conn_handle != conn) return;
    cgmLinkUp();
    if (fail_decide(FAIL_CGM, FAIL_CONSUME_MESSAGE, 0)) {
      logf("cgm: injected failure: message ignored");
      return;
    }
    NimBLEAttValue v = ch->getValue();
    sensor::onWrite(chr, v.data(), v.length());
  }

  void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc* desc, uint16_t subValue) override {
    if (desc && desc->conn_handle != conn) return;
    cgmLinkUp();
    // The core answers a subscription at once (the auth status, the reading), so
    // the CCCD value is recorded first: cgmNotify sends only to a subscriber.
    cgmSubs[chr] = subValue;
    sensor::onSubscribe(chr, subValue != 0);
  }

  void onRead(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    if (desc && desc->conn_handle != conn) return;
    cgmLinkUp();
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
    if (desc->conn_handle != conn) return;
    if (!pumpSelected(true)) {
      pumpRefuse(desc->conn_handle, "pod");
      return;
    }
    NimBLEAttValue v = ch->getValue();
    if (v.length() > DASH_PKT_MAX) return;
    if (fail_decide(FAIL_POD, FAIL_CONSUME_MESSAGE, 0)) {
      logf("pod: injected failure: message ignored");
      return;
    }
    podUsed = true;
    podLastTraffic = (uint32_t)monoMs();
    podPush(cmd ? DASH_EV_CMD : DASH_EV_DATA, v.data(), v.length());
  }

  // A pod phone subscribes to the data characteristic as it connects, before
  // its first write; mark the link as the pod's then too (as the Medtrum read
  // callback does), so a switch releases it even before any write.
  void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc* desc, uint16_t) override {
    if (desc && desc->conn_handle != conn) return;
    if (!pumpSelected(true)) {
      if (desc) pumpRefuse(desc->conn_handle, "pod");
      return;
    }
    podUsed = true;
  }

 private:
  bool cmd;
};

class MedtrumWriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    if (desc->conn_handle != conn) return;
    if (!pumpSelected(false)) {
      pumpRefuse(desc->conn_handle, "medtrum");
      return;
    }
    NimBLEAttValue v = ch->getValue();
    if (v.length() > MEDTRUM_FRAME_MAX) return;
    if (fail_decide(FAIL_MEDTRUM, FAIL_CONSUME_MESSAGE, 0)) {
      logf("medtrum: injected failure: message ignored");
      return;
    }
    medtrumUsed = true;
    medtrumPush(MEDTRUM_EV_WRITE, v.data(), v.length());
  }
};

class MedtrumReadCallbacks : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc* desc, uint16_t subValue) override {
    if (desc && desc->conn_handle != conn) return;
    if (!pumpSelected(false)) {
      if (desc) pumpRefuse(desc->conn_handle, "medtrum");
      return;
    }
    medtrumUsed = true;
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
  logf("Bluetooth address %s (the pod, the CGM and the patch are one device)",
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
    cgmSubs[i] = 0;
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
// it. instance 0 is the pump's, instance 1 the CGM's.
static bool advertise(uint8_t instance, const std::string& data, const std::string& scanRsp,
                      uint32_t interval) {
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
  if (!adv->setInstanceData(instance, a)) return false;
  NimBLEExtAdvertisement r;
  r.setLegacyAdvertising(true);
  r.setData((const uint8_t*)scanRsp.data(), scanRsp.size());
  if (!adv->setScanResponseData(instance, r)) return false;
  return adv->start(instance);
}

// (Re)starts one set with this payload and scan response, or stops it when the
// payload is empty. Each set keeps its own `current`, so one set's change never
// restarts the other. Returns true when the set was (re)started.
static bool advertiseSet(uint8_t instance, std::string& current, const std::string& data,
                         const std::string& scanRsp, uint32_t interval, const char* label) {
  if (data.empty()) {
    if (ble_gap_ext_adv_active(instance)) NimBLEDevice::getAdvertising()->stop(instance);
    current.clear();
    return false;
  }
  if (ble_gap_ext_adv_active(instance) && data == current) return false;
  if (!advertise(instance, data, scanRsp, interval)) {
    static int64_t last = -60000;
    if (monoMs() - last > 10000) {
      last = monoMs();
      logf("advertising %s could not start (%u bytes)", label, (unsigned)data.size());
    }
    return false;
  }
  current = data;
  return true;
}

void cgmAdvertise(bool on, const char* name, const uint8_t addr[6]) {
  // The CGM's own advertising set, on the chip's own address: the core's
  // per-sensor random static address is not applied here (the apps find the
  // device by name and service).
  (void)addr;
  std::string nm = name ? name : "";
  // Exactly the reference's CGM advertisement (ESP-Sim's main/ble.c), built by
  // the pure adv_cgm() the host test pins: flags, the complete list of 16-bit
  // service UUIDs with the 0xFEBC the phones filter their scan on
  // (little-endian), then the complete local name.
  uint8_t buf[ADV_MAX];
  size_t pn = on ? adv_cgm(nm.c_str(), buf, sizeof buf) : 0;
  if (on && !pn) logf("cgm: \"%s\" does not fit one advertisement; without one it cannot be found", nm.c_str());
  std::string p(pn ? (const char*)buf : "", pn);
  {
    std::lock_guard<std::mutex> l(mu);
    cgmAdvOn = on && pn != 0;
    cgmAdvData = p;
    cgmAdvName = nm;
  }
  logf("cgm: %s as %s", on && pn ? "advertising" : "not advertising", nm.empty() ? "(off)" : nm.c_str());
}

void tick() {
  // A hang-up the core asked for: carried out here, never from inside the
  // call that asked (the core holds its mutex there, and the NimBLE disconnect
  // callback calls back into the core). The one link is the pod's as well, so
  // the CGM's hang-up waits for the pod's traffic to pause first, as it always
  // did, and gives up waiting after 20 s.
  int64_t now = monoMs();
  uint32_t now32 = (uint32_t)now;  // the cross-task timestamps are 32-bit
  if (cgmDrop) {
    uint16_t c = conn;
    uint32_t gen = linkGen;
    if (c == BLE_HS_CONN_HANDLE_NONE || !cgmUsed) {
      // The link is gone, or the CGM never used it: nothing to end here.
      cgmDrop = false;
      cgmDropTimed = false;
    } else {
      if (!cgmDropTimed) {
        cgmDropTimed = true;
        cgmDropAt = now32;
      }
      // Signed elapsed: podNotify/PodCallbacks may stamp podLastTraffic from
      // another task after now32 was taken, which must read as "just now" (a
      // small negative), not as a huge unsigned idle. Horizon: 2^31 ms, i.e.
      // about 24 days of uptime per link, far beyond the 20 s deadline.
      int32_t sincePod = (int32_t)(now32 - podLastTraffic);
      int32_t sinceDrop = (int32_t)(now32 - cgmDropAt);
      // The link may have been replaced while this tick was deciding (the
      // handle could be reused): only end the same link.
      if ((sincePod >= 1500 || sinceDrop >= 20000) && conn == c && linkGen == gen) {
        cgmDrop = false;
        cgmDropTimed = false;
        logf("cgm: hanging up so the phone collects the reading");
        ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
      }
    }
  }

  std::unique_lock<std::mutex> l(mu);
  bool cgmOn = cgmAdvOn;
  std::string cgmData = cgmAdvData, cgmNm = cgmAdvName;
  std::string podData = podAdvData;
  bool paired = podPaired;
  std::string medData = medtrumAdvData;
  bool medOn = medtrumAdvOn;
  l.unlock();

  // Who wants to be found? Two sets now: instance 0 is the pump's (the pod, or
  // the Medtrum base when the selector says so; never both, never the CGM),
  // instance 1 is the CGM's. The pump set is chosen by the selector; the CGM
  // set follows the core's schedule (it opens a window per reading and closes
  // it when the phone has collected). An injected quiet window only clears that
  // device's want flag; its payload string is untouched.
  bool connected = conn != BLE_HS_CONN_HANDLE_NONE;
  // The pod and the Medtrum base are mutually exclusive: exactly one of the two
  // may use the pump set, and PUMP_OFF lets neither.
  int sel = pump();
  bool podWants = sel == PUMP_POD && !podData.empty() && (!connected || !paired);
  bool medWants = sel == PUMP_MEDTRUM && medOn && !medData.empty() && !connected;
  bool cgmWants = (int)sensor::kind() != 0 && cgmOn && !connected;
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

  // Each set keeps its own payload string: "already carrying this" is decided
  // per set, so one set's change never restarts the other.
  static std::string pumpCurrent, cgmCurrent;
  static unsigned seen = 0;

  // Instance 0: the pump set. The selector names the one pump that may use it;
  // PUMP_OFF, or a pump that does not want to be found, leaves it stopped.
  std::string pumpData, pumpRsp;
  const char* pumpLabel = sel == PUMP_MEDTRUM ? "Medtrum" : "pod";
  unsigned pumpBit = sel == PUMP_MEDTRUM ? 4u : 2u;
  if (sel == PUMP_POD && podWants) {
    pumpData = podData;
    pumpRsp = named(DASH_ADVERTISED_NAME);
  } else if (sel == PUMP_MEDTRUM && medWants) {
    pumpData = medData;
    pumpRsp = named("MT");
  }
  if (advertiseSet(pumpInstance, pumpCurrent, pumpData, pumpRsp, 0x00f4, pumpLabel) &&
      !(seen & pumpBit)) {
    seen |= pumpBit;
    logf("advertising %s (%u bytes)", pumpLabel, (unsigned)pumpData.size());
  }

  // Instance 1: the CGM set, whenever the core wants it. Otherwise it stops.
  std::string cgmSetData, cgmSetRsp;
  if (cgmWants) {
    cgmSetData = cgmData;
    cgmSetRsp = named(cgmNm);
  }
  if (advertiseSet(cgmInstance, cgmCurrent, cgmSetData, cgmSetRsp, cgmAdvInterval, "CGM") &&
      !(seen & 1u)) {
    seen |= 1u;
    logf("advertising CGM (%u bytes)", (unsigned)cgmSetData.size());
  }
}

int openLinks() { return conn != BLE_HS_CONN_HANDLE_NONE ? 1 : 0; }

// MARK: - The CGM's radio

// Sends the queued notifications, one at a time, after the injected delay.
static void cgmSendTask(void*) {
  CgmOut m = {};
  for (;;) {
    if (xQueueReceive(cgmOut, &m, portMAX_DELAY) != pdTRUE) continue;
    unsigned delay = (unsigned)fail_decide(FAIL_CGM, FAIL_NOTIFY_DELAY_MS, 0);
    if (delay) vTaskDelay(pdMS_TO_TICKS(delay));
    uint16_t c = conn;
    // The phone may have left, or a new one taken the same handle, while we
    // waited: a queued message belongs to the link it was queued for.
    if (c == BLE_HS_CONN_HANDLE_NONE || c != m.conn || linkGen != m.gen) continue;
    uint16_t sub = cgmSubs[m.chr];
    if (!sub) {
      logf("cgm: %s -> %s dropped: not subscribed", cgmCharNames[m.chr], hexstr(m.data, m.len).c_str());
      continue;
    }
    os_mbuf* om = ble_hs_mbuf_from_flat(m.data, m.len);
    if (!om) {
      logf("cgm: notification dropped: no buffers");
      continue;
    }
    // A phone subscribes to notify, or (the auth characteristic) to indicate.
    int rc = (sub & 1) ? ble_gattc_notify_custom(c, cgmHandles[m.chr], om)
                       : ble_gattc_indicate_custom(c, cgmHandles[m.chr], om);
    if (rc != 0) logf("cgm: notification failed: %d", rc);
  }
}

// Called by the core from inside its own mutex: queue the message and return.
// The task above sends it once, as the reference's ble_cgm_send does (no
// ENOMEM retry loop), and applies that device's injected delay.
void cgmNotify(cgm_chr_t chr, const uint8_t* p, size_t n) {
  if (conn == BLE_HS_CONN_HANDLE_NONE || !cgmOut) return;
  if (n > sizeof(CgmOut::data)) {
    logf("cgm: a %u-byte notification does not fit the queue; dropped", (unsigned)n);
    return;
  }
  CgmOut m = {};
  m.conn = conn;
  m.gen = linkGen;
  m.chr = (uint8_t)chr;
  m.len = (uint8_t)n;
  if (n) memcpy(m.data, p, n);
  if (xQueueSend(cgmOut, &m, 0) != pdTRUE) logf("cgm: notification queue full: a message was dropped");
}

// The core calls this from inside its own mutex, possibly on the NimBLE
// callback thread: only mark the hang-up. tick() terminates the link (if the
// CGM ever used it), and the NimBLE disconnect callback then calls
// sensor::onDisconnect.
void cgmDisconnect() {
  if (conn == BLE_HS_CONN_HANDLE_NONE) return;
  // A request that arrives while none is pending starts a fresh 20 s deadline;
  // the core asks again every second while it is pending, which must not
  // restart it.
  if (!cgmDrop) cgmDropTimed = false;
  cgmDrop = true;
}

bool cgmConnected() { return conn != BLE_HS_CONN_HANDLE_NONE; }

// MARK: - The pod's radio

bool podConnected() { return conn != BLE_HS_CONN_HANDLE_NONE; }

bool podNextEvent(dash_ev_t* ev, uint32_t timeoutMs) {
  TickType_t t = timeoutMs == DASH_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
  if (t == 0 && timeoutMs > 0) t = 1;
  return xQueueReceive(podEvents, ev, t) == pdTRUE;
}

static void podNotify(uint16_t handle, const uint8_t* p, size_t n) {
  uint16_t c = conn;
  if (c == BLE_HS_CONN_HANDLE_NONE) return;
  uint32_t gen = linkGen;
  unsigned delay = (unsigned)fail_decide(FAIL_POD, FAIL_NOTIFY_DELAY_MS, 0);
  if (delay) vTaskDelay(pdMS_TO_TICKS(delay));
  // The link may have gone (or been replaced) while we waited: do not send
  // the old device's message to a new phone that reused the handle.
  if (conn != c || linkGen != gen) return;
  podLastTraffic = (uint32_t)monoMs();
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

void podNotifyData(const uint8_t* p, size_t n) { podNotify(podIsO5 ? podO5DataHandle : podDataHandle, p, n); }

void podDisconnect() {
  uint16_t c = conn;
  if (c != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
}

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
  logf("pod: advertising as %s %s pod (fault 0x%02x, alerts 0x%02x)", paired ? "a paired" : "an unpaired",
       o5 ? "Omnipod 5" : "DASH", adv->fault, adv->alerts);
}

// MARK: - The Medtrum patch's radio

bool medtrumConnected() { return conn != BLE_HS_CONN_HANDLE_NONE; }

bool medtrumNextEvent(medtrum_ev_t* ev, uint32_t timeoutMs) {
  TickType_t t = timeoutMs == MEDTRUM_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
  if (t == 0 && timeoutMs > 0) t = 1;
  return xQueueReceive(medtrumEvents, ev, t) == pdTRUE;
}

static void medtrumNotifyRaw(uint16_t handle, const uint8_t* p, size_t n) {
  uint16_t c = conn;
  if (c == BLE_HS_CONN_HANDLE_NONE) return;
  uint32_t gen = linkGen;
  unsigned delay = (unsigned)fail_decide(FAIL_MEDTRUM, FAIL_NOTIFY_DELAY_MS, 0);
  if (delay) vTaskDelay(pdMS_TO_TICKS(delay));
  // The link may have gone (or been replaced) while we waited.
  if (conn != c || linkGen != gen) return;
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
  uint16_t c = conn;
  if (c != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
}

void setPump(int p) {
  if (p != PUMP_OFF && p != PUMP_POD && p != PUMP_MEDTRUM) return;
  {
    std::lock_guard<std::mutex> l(mu);
    pumpSel = p;
  }
  // The pump that is no longer selected must not keep the one link. A link the
  // CGM alone touched is not a pump's, so it is left alone.
  uint16_t c = conn;
  bool heldByOther = (p == PUMP_OFF && (podUsed || medtrumUsed)) || (p == PUMP_POD && medtrumUsed) ||
                     (p == PUMP_MEDTRUM && podUsed);
  if (c != BLE_HS_CONN_HANDLE_NONE && heldByOther) {
    logf("pump simulators: dropping the link the unselected pump held");
    ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
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
  logf("medtrum: %s, serial %02X%02X%02X%02X", on ? "advertising" : "not advertising", snv[0], snv[1], snv[2],
       snv[3]);
}

}  // namespace radio
