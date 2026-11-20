// SPDX-License-Identifier: GPL-3.0-or-later
#include "radio.h"

#include <NimBLEDevice.h>

#include <mutex>

#include "adv_payload.h"
#include "adv_rotation.h"
#include "dash_internal.h"
#include "fail.h"
#include "sensor_app.h"
#include "util.h"

namespace radio {

static const uint8_t inst = 0;  // the one advertising set

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
static volatile uint32_t cgmDropAt = 0;  // 32-bit: written by two tasks
// The CGM was used on the current link (any of its GATT events). The core ends
// a session after CONN_MAX; a link the CGM never touched is the pod's or the
// patch's, and must not be torn down by that timer.
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

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
    std::string addr = addrString(desc->peer_ota_addr);
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
      logf("%s: a second phone; one at a time, closing it", addr.c_str());
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
    cgmUsed = false;
    // The core marks its own advertising off when it sees the connection
    // (cgm_on_connect) without calling advertise(false), so the radio's copy is
    // cleared here to match: otherwise the rotation would keep advertising the
    // CGM outside the core's 60-second window, and signal loss could not stop
    // it.
    {
      std::lock_guard<std::mutex> l(mu);
      cgmAdvOn = false;
    }
    // The core does not hold the radio's mutex, and this callback holds none
    // of it either: it may send (cgmNotify) and hang up (cgmDisconnect, which
    // tick() carries out) from inside its own mutex.
    sensor::onConnect();
    podPush(DASH_EV_CONNECTED, nullptr, 0);
    medtrumPush(MEDTRUM_EV_CONNECTED, nullptr, 0);
  }

  void onDisconnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
    if (desc->conn_handle != conn) return;
    conn = BLE_HS_CONN_HANDLE_NONE;
    cgmDrop = false;
    cgmDropAt = 0;
    cgmUsed = false;
    // No stale CCCD: a notification that was queued for this link is dropped
    // (the generation check), and a new phone has to subscribe afresh.
    for (int i = 0; i < CGM_CHR_COUNT; i++) cgmSubs[i] = 0;
    std::string addr = addrString(desc->peer_ota_addr);
    logf("%s disconnected", addr.c_str());
    sensor::onDisconnect();
    medtrumStatusNotify = false;
    podPush(DASH_EV_DISCONNECTED, nullptr, 0);
    medtrumPush(MEDTRUM_EV_DISCONNECTED, nullptr, 0);
  }
};

// One GATT event each way for the core: the core's own protocol runs inside
// sensor::, so the radio only maps UUID to cgm_chr_t and hands the bytes over.
class CgmCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit CgmCallbacks(cgm_chr_t chr) : chr(chr) {}

  void onWrite(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    if (desc->conn_handle != conn) return;
    cgmUsed = true;
    if (fail_decide(FAIL_CGM, FAIL_CONSUME_MESSAGE, 0)) {
      logf("cgm: injected failure: message ignored");
      return;
    }
    NimBLEAttValue v = ch->getValue();
    sensor::onWrite(chr, v.data(), v.length());
  }

  void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc* desc, uint16_t subValue) override {
    if (desc && desc->conn_handle != conn) return;
    cgmUsed = true;
    // The core answers a subscription at once (the auth status, the reading), so
    // the CCCD value is recorded first: cgmNotify sends only to a subscriber.
    cgmSubs[chr] = subValue;
    sensor::onSubscribe(chr, subValue != 0);
  }

  void onRead(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    if (desc && desc->conn_handle != conn) return;
    cgmUsed = true;
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
    NimBLEAttValue v = ch->getValue();
    if (v.length() > DASH_PKT_MAX) return;
    if (fail_decide(FAIL_POD, FAIL_CONSUME_MESSAGE, 0)) {
      logf("pod: injected failure: message ignored");
      return;
    }
    podLastTraffic = (uint32_t)monoMs();
    podPush(cmd ? DASH_EV_CMD : DASH_EV_DATA, v.data(), v.length());
  }

 private:
  bool cmd;
};

class MedtrumWriteCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    if (desc->conn_handle != conn) return;
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
  void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc*, uint16_t subValue) override {
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
  // its own name in the advertisement, which the rotation supplies.
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

// (Re)configures the advertising set as a legacy connectable advert and starts it.
static bool advertise(const std::string& data, const std::string& scanRsp, uint32_t interval) {
  NimBLEExtAdvertising* adv = NimBLEDevice::getAdvertising();
  if (ble_gap_ext_adv_active(inst)) adv->stop(inst);
  NimBLEExtAdvertisement a;
  a.setLegacyAdvertising(true);
  a.setConnectable(true);
  a.setScannable(true);
  a.setMinInterval(interval);
  a.setMaxInterval(interval);
  a.setTxPower(9);
  a.setData((const uint8_t*)data.data(), data.size());
  if (!adv->setInstanceData(inst, a)) return false;
  NimBLEExtAdvertisement r;
  r.setLegacyAdvertising(true);
  r.setData((const uint8_t*)scanRsp.data(), scanRsp.size());
  if (!adv->setScanResponseData(inst, r)) return false;
  return adv->start(inst);
}

void cgmAdvertise(bool on, const char* name, const uint8_t addr[6]) {
  // One advertising set, on the chip's own address: the core's per-sensor
  // random static address is for the reference's second extended-adv instance
  // and is not used here (the apps find the device by name and service).
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
  // did, and gives up waiting after 20 s. A link the CGM never touched is not
  // the CGM's to end: the core's 30 s session cap must not tear down a pod or
  // patch session (P1: the cap is the core's, but the link is shared).
  int64_t now = monoMs();
  uint32_t now32 = (uint32_t)now;  // the cross-task deadlines are 32-bit
  if (cgmDrop) {
    uint16_t c = conn;
    if (c == BLE_HS_CONN_HANDLE_NONE) {
      cgmDrop = false;
      cgmDropAt = 0;
    } else if (!cgmUsed) {
      // Nothing CGM has happened on this link: leave the pod's session up. The
      // core asks again every second; this clears each request.
      cgmDrop = false;
      cgmDropAt = 0;
    } else {
      if (!cgmDropAt) cgmDropAt = now32;
      if (now32 - podLastTraffic >= 1500u || now32 - cgmDropAt >= 20000u) {
        cgmDrop = false;
        cgmDropAt = 0;
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

  // Who needs to be found? The CGM on the core's schedule (it opens a window
  // per reading and closes it when the phone has collected); the pod whenever
  // no phone is connected, and until it is paired; the Medtrum patch whenever
  // it is switched on and no phone is connected. One advertising set, so when
  // more than one wants to be found they take turns. An injected quiet window
  // only clears that device's want flag; its payload string is untouched.
  bool connected = conn != BLE_HS_CONN_HANDLE_NONE;
  adv_wants_t wants = {
      .g7 = adv_cgm_wants((int)sensor::kind(), cgmOn && !connected) && fail_decide(FAIL_CGM, FAIL_ADVERTISE, now),
      .pod = !podData.empty() && (!connected || !paired) && fail_decide(FAIL_POD, FAIL_ADVERTISE, now),
      .med = medOn && !medData.empty() && !connected && fail_decide(FAIL_MEDTRUM, FAIL_ADVERTISE, now),
  };
  int choice = adv_rotation_slot(&wants, now);

  static std::string current;
  bool active = ble_gap_ext_adv_active(inst);
  if (choice == ADV_NONE) {
    if (active) NimBLEDevice::getAdvertising()->stop(inst);
    current.clear();
    return;
  }
  // The order (CGM, pod, Medtrum) keeps the pod's payload byte-exact: it is
  // the same string it always was, only now sharing the set three ways.
  std::string data, scanRsp;
  uint32_t interval;
  if (choice == ADV_CGM) {
    data = cgmData;
    scanRsp = named(cgmNm);
    interval = cgmAdvInterval;  // the CGM's 160 ms
  } else if (choice == ADV_POD) {
    data = podData;
    scanRsp = named(DASH_ADVERTISED_NAME);
    interval = 0x00f4;
  } else {
    data = medData;
    scanRsp = named("MT");
    interval = 0x00f4;  // 152.5 ms, as ESP-Sim
  }
  if (active && data == current) return;
  bool ok = advertise(data, scanRsp, interval);
  if (ok) {
    current = data;
  } else {
    static int64_t last = -60000;
    if (monoMs() - last > 10000) {
      last = monoMs();
      logf("advertising could not start");
    }
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
// callback thread: only mark the hang-up. tick() terminates the link, and the
// NimBLE disconnect callback then calls sensor::onDisconnect.
void cgmDisconnect() {
  if (conn != BLE_HS_CONN_HANDLE_NONE) cgmDrop = true;
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
