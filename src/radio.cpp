// SPDX-License-Identifier: GPL-3.0-or-later
#include "radio.h"

#include <NimBLEDevice.h>

#include <mutex>

#include "dash_internal.h"
#include "link.h"

namespace radio {

static const uint8_t inst = 0;  // the one advertising set

// MARK: - The G7's GATT layout (g7sim's ble.go)

static const char* cgmServiceUUID = "F8083532-849E-531C-C594-30F1F86A4EA5";
static const char* communicationUUID = "F8083533-849E-531C-C594-30F1F86A4EA5";
static const char* charUUIDs[4] = {
    "F8083535-849E-531C-C594-30F1F86A4EA5",  // auth
    "F8083534-849E-531C-C594-30F1F86A4EA5",  // control
    "F8083536-849E-531C-C594-30F1F86A4EA5",  // backfill
    "F8083538-849E-531C-C594-30F1F86A4EA5",  // certificate
};
static const char* serviceBUUID = "F8084532-849E-531C-C594-30F1F86A4EA5";
static const char* charEUUID = "F8084533-849E-531C-C594-30F1F86A4EA5";
static const char* charFUUID = "F8084534-849E-531C-C594-30F1F86A4EA5";

// MARK: - The pod's GATT layout (ESP-Sim's ble.c)

static const char* podServiceUUID = "1a7e4024-e3ed-4464-8b7e-751e03d0dc5f";
static const char* podCmdUUID = "1a7e2441-e3ed-4464-8b7e-751e03d0dc5f";
static const char* podDataUUID = "1a7e2442-e3ed-4464-8b7e-751e03d0dc5f";    // DASH
static const char* podO5DataUUID = "1a7e2443-e3ed-4464-8b7e-751e03d0dc5f";  // Omnipod 5

static Sensor* sensor;
static NimBLEServer* server;
static NimBLECharacteristic* chars[4];

static std::mutex mu;
static std::shared_ptr<Link> link;  // the one phone, as the G7 sees it
static volatile uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static AdvertMode mode = AdvertMode::Off;
static uint8_t types = 0;

// The pod's side of the link.
static QueueHandle_t podEvents;
static uint16_t podCmdHandle, podDataHandle, podO5DataHandle;
static std::string podAdvData;  // guarded by mu
static bool podIsO5 = false, podPaired = false;
static volatile int64_t podLastTraffic = 0;

static std::shared_ptr<Link> currentLink(uint16_t handle) {
  std::lock_guard<std::mutex> l(mu);
  return link && link->conn == handle ? link : nullptr;
}

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

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
    std::string addr = addrString(desc->peer_ota_addr);
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
      logf("%s: a second phone; one at a time, closing it", addr.c_str());
      s->disconnect(desc->conn_handle);
      return;
    }
    AdvertMode m;
    {
      std::lock_guard<std::mutex> l(mu);
      m = mode;
    }
    logf("%s connected", addr.c_str());
    // A real G7 refuses the link of a display that has already collected the
    // reading. This link is the pod's too, so it is always taken.
    sensor->linkOpened(addr, m);
    auto l = std::make_shared<Link>(desc->conn_handle, addr, m);
    {
      std::lock_guard<std::mutex> g(mu);
      link = l;
    }
    conn = desc->conn_handle;
    startHandler(l, sensor);
    podPush(DASH_EV_CONNECTED, nullptr, 0);
  }

  void onDisconnect(NimBLEServer*, ble_gap_conn_desc* desc) override {
    if (desc->conn_handle != conn) return;
    std::shared_ptr<Link> l;
    {
      std::lock_guard<std::mutex> g(mu);
      l = link;
      link = nullptr;
    }
    conn = BLE_HS_CONN_HANDLE_NONE;
    if (l) {
      l->connected = false;
      logf("%s disconnected", l->addr.c_str());
    }
    sensor->linkClosed();
    podPush(DASH_EV_DISCONNECTED, nullptr, 0);
  }
};

class CharCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit CharCallbacks(Char c) : c(c) {}

  void onWrite(NimBLECharacteristic* ch, ble_gap_conn_desc* desc) override {
    auto l = currentLink(desc->conn_handle);
    if (!l) return;
    NimBLEAttValue v = ch->getValue();
    l->enqueue(c, v.data(), v.length(), false);
  }

  void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc* desc, uint16_t subValue) override {
    auto l = currentLink(desc->conn_handle);
    if (!l) return;
    l->subs[(int)c] = subValue;
    l->enqueue(c, nullptr, 0, true);
  }

 private:
  Char c;
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
    podLastTraffic = monoMs();
    podPush(cmd ? DASH_EV_CMD : DASH_EV_DATA, v.data(), v.length());
  }

 private:
  bool cmd;
};

static const uint32_t rw = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY |
                           NIMBLE_PROPERTY::INDICATE;
static const uint32_t podRw = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY;

void begin(Sensor* s) {
  sensor = s;
  podEvents = xQueueCreate(64, sizeof(dash_ev_t));

  // The GAP device name is the sensor's: the app's G7 code knows its sensor
  // by name, and its pod code only looks for one particular name.
  NimBLEDevice::init(sensor->name());
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(251);  // Omnipod 5 packets are 244 bytes
  logf("Bluetooth address %s (the pod and the G7 are one device)", NimBLEDevice::getAddress().toString().c_str());

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  // NimBLE-Arduino 1.4.3 leaves the extended advertiser's callbacks
  // uninitialised, and calls them when a set stops: install the defaults.
  NimBLEDevice::getAdvertising()->setCallbacks(nullptr);

  NimBLEService* cgm = server->createService(cgmServiceUUID);
  cgm->createCharacteristic(communicationUUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::INDICATE);
  for (int i = 0; i < 4; i++) {
    chars[i] = cgm->createCharacteristic(charUUIDs[i], rw);
    chars[i]->setCallbacks(new CharCallbacks((Char)i));
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

  cgm->start();
  b->start();
  pod->start();
  server->start();
  for (int i = 0; i < 4; i++) charHandles[i] = chars[i]->getHandle();
  podCmdHandle = pc->getHandle();
  podDataHandle = pd->getHandle();
  podO5DataHandle = po->getHandle();
}

// MARK: - Advertising

static std::string named(const std::string& name) {
  std::string p = {(char)(name.size() + 1), 0x09};
  return p + name;
}

static std::string g7Advertisement(AdvertMode m, uint8_t types) {
  if (m == AdvertMode::Bare) return {0x02, 0x01, 0x04};  // the minute calls carry the Flags field alone
  uint16_t crc = sensor->serialChecksum();
  std::string p = {0x02, 0x01, 0x06, 0x03, 0x03, (char)0xBC, (char)0xFE};
  p += {0x07, (char)0xFF, (char)0xD0, 0x00, (char)(crc & 0xff), (char)(crc >> 8), (char)types, 0x04};
  return p + named(sensor->name());
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

// Once per reading, when a phone that has collected one is still on the link:
// hang up, as a G7 does, so the app's G7 code reconnects and collects the new
// reading. Waits for the pod's traffic to pause first.
static void hangUpForNewReading() {
  static int lastIndex = -2;
  static int64_t dropBy = 0;
  int idx = sensor->latestIndex();
  if (lastIndex == -2) lastIndex = idx;
  if (idx != lastIndex) {
    lastIndex = idx;
    dropBy = monoMs() + 20 * SEC;
  }
  if (!dropBy) return;
  std::shared_ptr<Link> l;
  {
    std::lock_guard<std::mutex> g(mu);
    l = link;
  }
  if (!l || !l->served) {
    dropBy = 0;  // nobody waiting on the link: the next connection reads the latest
    return;
  }
  int64_t now = monoMs();
  if (now - podLastTraffic < 1500 && now < dropBy) return;
  dropBy = 0;
  logf("%s: a new reading: hanging up so the phone collects it", l->addr.c_str());
  ble_gap_terminate(l->conn, BLE_ERR_REM_USER_CONN_TERM);
}

void tick() {
  AdvertMode m = sensor->tick();
  uint8_t t = sensor->typesInUse();
  hangUpForNewReading();

  std::unique_lock<std::mutex> l(mu);
  bool changed = m != mode || (m == AdvertMode::Full && t != types);
  mode = m;
  types = t;
  std::string podData = podAdvData;
  bool paired = podPaired;
  l.unlock();
  if (changed) logf("G7 advertising %s (types in use %02x)", advertModeName(m), t);

  // Who needs to be found? The G7 on its own schedule; the pod whenever no
  // phone is connected, and until it is paired. When both do, they take turns.
  bool connected = conn != BLE_HS_CONN_HANDLE_NONE;
  bool wantG7 = m != AdvertMode::Off;
  bool wantPod = !podData.empty() && (!connected || !paired);
  if (wantG7 && wantPod) (monoMs() / 1500) % 2 ? wantG7 = false : wantPod = false;

  static std::string current;
  bool active = ble_gap_ext_adv_active(inst);
  if (!wantG7 && !wantPod) {
    if (active) NimBLEDevice::getAdvertising()->stop(inst);
    current.clear();
    return;
  }
  std::string data = wantG7 ? g7Advertisement(m, t) : podData;
  if (active && data == current) return;
  bool ok = wantG7 ? advertise(data, named(sensor->name()), sensor->timing.advertisingInterval * 1000 / 625)
                   : advertise(data, named(DASH_ADVERTISED_NAME), 0x00f4);  // 152.5 ms, as ESP-Sim
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
  podLastTraffic = monoMs();
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
  std::string p = {0x02, 0x01, 0x06};
  if (o5) {
    // One 128-bit UUID with the paired controller's id in it.
    uint8_t le[16];
    dash_o5_advertised_uuid(adv->pdm_id, le);
    p += {0x11, 0x07};
    p.append((const char*)le, 16);
  } else {
    uint16_t svc[9];
    dash_advertised_services(adv->id, adv->fault, adv->alerts, svc);
    p += {0x13, 0x03};
    for (uint16_t u : svc) {
      p += (char)(u & 0xff);
      p += (char)(u >> 8);
    }
  }
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

}  // namespace radio
