// ESP-Sim + G7: one ESP32-S3 that is both an Omnipod pod simulator and a
// Dexcom G7 sensor simulator, for testing Loop, Trio and other apps with no
// real pod or sensor.
//
//   the pod   ESP-Sim's pod core (github.com/nightscout/ESP-Sim), in lib/dashpod
//   the G7    a C++ port of LoopKit/g7sim (github.com/LoopKit/g7sim)
//
// TEST EQUIPMENT ONLY. Not a Dexcom or Insulet product. It delivers no
// insulin and every value it reports is made up. Never use it, or an app
// paired to it, for therapy.
//
// Control: the USB serial console (type `help`) and the web panels, the G7's
// at / and the pod's at /pod. With no Wi-Fi network saved, the board runs its
// own access point (name and password are printed at boot).
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_sntp.h>

#include "crypto.h"
#include "index_html.h"
#include "pod.h"
#include "pod_html.h"
#include "radio.h"
#include "sensor.h"

static Sensor* sensor;
static WebServer web(80);
static bool clockSynced = false, wifiConfigured = false, apStarted = false;
static volatile int staFailures = 0;
static int64_t restartAt = 0;
static String apSsid, apPass;

// MARK: - Commands shared by the console and the API

static void replaceSensor(const State& next) {
  sensor->retire();
  saveState(next);
  logf("restarting as the new sensor");
  delay(300);
  ESP.restart();
}

static bool configFromJson(JsonVariantConst j, SensorConfig& c) {
  c.model = j["model"] | "";
  c.days = j["days"] | 0;
  c.code = j["code"] | "";
  if (j["serial"].is<const char*>()) c.serial = strtoull(j["serial"].as<const char*>(), nullptr, 10);
  else c.serial = (uint64_t)(j["serial"] | 0.0);
  if (!j["ageMinutes"].isNull()) c.ageMinutes = j["ageMinutes"].as<double>();
  if (!j["lifetimeHours"].isNull()) c.lifetimeHours = j["lifetimeHours"].as<double>();
  if (!j["warmupMinutes"].isNull()) c.warmupMinutes = j["warmupMinutes"].as<double>();
  return true;
}

static bool modeFromJson(JsonVariantConst j, GlucoseMode& m) {
  // As g7sim decodes it: any kind but "fixed" and "walk" is a sine, and an
  // omitted number is 0 (a sine's period of 0 means 240 minutes).
  m.kind = j["kind"] | "";
  if (m.kind.empty()) return false;
  m.value = j["value"] | 0.0;
  m.amplitude = j["amplitude"] | 0.0;
  m.periodMinutes = j["periodMinutes"] | 0.0;
  m.algorithmState = j["algorithmState"] | 0;
  return true;
}

// MARK: - HTTP API (api.go)

static void sendJson(int code, JsonDocument& d) {
  String s;
  serializeJsonPretty(d, s);
  web.send(code, "application/json", s);
}

static void sendStatus() {
  JsonDocument d;
  sensor->status(d);
  sendJson(200, d);
}

static void saveWifi(const String& ssid, const String& pass) {
  Preferences p;
  p.begin("net", false);
  if (ssid.isEmpty()) {
    p.clear();
    logf("Wi-Fi settings cleared");
  } else {
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    logf("Wi-Fi set to %s", ssid.c_str());
  }
  p.end();
}

static void startWeb() {
  web.on("/", HTTP_GET, [] { web.send_P(200, "text/html; charset=utf-8", indexHTML); });
  web.on("/status", HTTP_GET, sendStatus);
  web.on("/glucose", HTTP_POST, [] {
    JsonDocument j;
    GlucoseMode m;
    if (deserializeJson(j, web.arg("plain")) || !modeFromJson(j, m)) {
      web.send(400, "text/plain", "bad glucose mode\n");
      return;
    }
    sensor->setGlucoseMode(m);
    logf("glucose mode now %s %.0f", m.kind.c_str(), m.value);
    sendStatus();
  });
  web.on("/forget", HTTP_POST, [] {
    sensor->forgetSlots();
    logf("forgot every display's key");
    sendStatus();
  });
  web.on("/new", HTTP_POST, [] {
    JsonDocument j;
    SensorConfig c;
    if (deserializeJson(j, web.arg("plain"))) {
      web.send(400, "text/plain", "bad sensor config\n");
      return;
    }
    configFromJson(j, c);
    State next;
    std::string err;
    if (!newState(c, next, err)) {
      web.send(400, "text/plain", (err + "\n").c_str());
      return;
    }
    JsonDocument r;
    r["name"] = std::string(c.model == "stelo" ? "DX01" : c.model == "oneplus" ? "DX02" : "DXCM") + next.suffix;
    r["code"] = next.code;
    r["serial"] = next.serial;
    sendJson(200, r);
    logf("replacing %s with a new %s sensor, code %s", sensor->name().c_str(), next.model.c_str(), next.code.c_str());
    replaceSensor(next);
  });

  // The pod's panel and API (ESP-Sim's, under /pod).
  web.on("/pod", HTTP_GET, [] { web.send_P(200, "text/html; charset=utf-8", podHTML); });
  web.on("/pod/status", HTTP_GET, [] {
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "application/json", pod::statusJson().c_str());
  });
  web.on("/pod/command", HTTP_POST, [] {
    if (const char* err = pod::command(web.arg("plain").c_str())) {
      web.send(400, "text/plain", err);
      return;
    }
    web.send(200, "application/json", pod::statusJson().c_str());
  });
  web.on("/pod/wifi", HTTP_POST, [] {
    JsonDocument j;
    if (deserializeJson(j, web.arg("plain")) || !j["ssid"].is<const char*>() || !j["password"].is<const char*>()) {
      web.send(400, "text/plain", "want {\"ssid\": ..., \"password\": ...}");
      return;
    }
    String ssid = j["ssid"].as<const char*>(), pass = j["password"].as<const char*>();
    if (ssid.length() && pass.length() && pass.length() < 8) {
      web.send(400, "text/plain", "a WPA2 password has at least 8 characters");
      return;
    }
    saveWifi(ssid, pass);
    web.send(200, "text/plain", "saved; restarting\n");
    restartAt = monoMs() + 1000;
  });
  web.onNotFound([] { web.send(404, "text/plain", "not found\n"); });
  web.begin();
  MDNS.begin("esp-sim");
  MDNS.addService("http", "tcp", 80);
}

// MARK: - Wi-Fi: the web panels, and NTP for the clocks
//
// With a saved network the board joins it and takes the time from NTP.
// Without one, or if joining keeps failing, it runs its own access point (as
// ESP-Sim does): its password comes from the chip's MAC and is printed on the
// console, so no credential is built into the firmware.

static void startAP() {
  if (apStarted) return;
  apStarted = true;
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  char ssid[33], pass[17];
  snprintf(ssid, sizeof ssid, "ESP-Sim-%02X%02X", mac[4], mac[5]);
  snprintf(pass, sizeof pass, "pod-%02x%02x%02x%02x", mac[2] ^ 0x5a, mac[3] ^ 0xa5, mac[4] ^ 0x3c, mac[5] ^ 0xc3);
  WiFi.mode(wifiConfigured ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(ssid, pass, 1, 0, 4);
  apSsid = ssid;
  apPass = pass;
  logf("Wi-Fi: access point \"%s\", password \"%s\": panels at http://192.168.4.1/", ssid, pass);
}

static void startWifi() {
  Preferences p;
  p.begin("net", false);
  String ssid = p.isKey("ssid") ? p.getString("ssid") : "", pass = p.isKey("pass") ? p.getString("pass") : "";
  p.end();
  WiFi.setHostname("esp-sim");
  if (ssid.isEmpty()) {
    startAP();
    return;
  }
  wifiConfigured = true;
  // Say why a join fails (once per distinct reason), so a hotspot problem is visible.
  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        static int lastReason = -1;
        staFailures++;
        int r = info.wifi_sta_disconnected.reason;
        if (r == lastReason) return;
        lastReason = r;
        const char* why = r == WIFI_REASON_NO_AP_FOUND        ? "network not found (2.4 GHz off, or out of range)"
                          : r == WIFI_REASON_AUTH_FAIL        ? "authentication failed"
                          : r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ? "wrong password (handshake timeout)"
                          : r == WIFI_REASON_HANDSHAKE_TIMEOUT ? "wrong password (handshake timeout)"
                          : r == WIFI_REASON_ASSOC_FAIL       ? "association failed"
                                                              : "see reason code";
        logf("Wi-Fi: join failed, reason %d: %s", r, why);
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  sntp_set_time_sync_notification_cb([](struct timeval*) { markClockReal(); });
  WiFi.mode(WIFI_STA);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.begin(ssid.c_str(), pass.c_str());
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.apple.com");
  logf("Wi-Fi: joining %s", ssid.c_str());
}

static void pollWifi() {
  static bool wasUp = false;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wasUp) {
    staFailures = 0;
    logf("Wi-Fi up: panels at http://%s/  (http://esp-sim.local/)", WiFi.localIP().toString().c_str());
  }
  wasUp = up;
  if (!up && staFailures >= 5 && !apStarted) {
    logf("Wi-Fi: could not join the saved network: starting the access point too");
    startAP();
  }
  // Keep trying while the network is away (e.g. a phone hotspot that sleeps).
  static int64_t lastTry = monoMs();
  if (!up && monoMs() - lastTry > 30 * SEC) {
    lastTry = monoMs();
    WiFi.reconnect();
  }
  if (!clockSynced) {
    if (int64_t w = wallMs()) {
      clockSynced = true;
      sensor->syncWallClock(w);
    }
  }
}

// MARK: - Serial console

static void printStatus() {
  JsonDocument d;
  sensor->status(d);
  d.remove("slots");
  String s;
  serializeJsonPretty(d, s);
  Serial.println(s);
  JsonDocument d2;
  sensor->status(d2);
  Serial.print("slots: ");
  serializeJson(d2["slots"], Serial);
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) Serial.printf("web: http://%s/\n", WiFi.localIP().toString().c_str());
  if (apStarted)
    Serial.printf("web (access point \"%s\", password \"%s\"): http://192.168.4.1/\n", apSsid.c_str(), apPass.c_str());
  Serial.printf("pod: %s\n", pod::statusJson().c_str());
}

static void podCommand(const std::string& json) {
  if (const char* err = pod::command(json)) Serial.printf("pod: %s\n", err);
  else Serial.printf("pod: %s\n", pod::statusJson().c_str());
}

static void help() {
  Serial.println(
      "ESP-Sim + G7 console. Test equipment only: simulated data, no insulin.\n"
      "G7 sensor:\n"
      "  status                              sensor, pairing code, latest reading, slots\n"
      "  glucose {\"kind\":\"fixed\",\"value\":65}  g7sim's POST /glucose: kind fixed|sine|walk, value,\n"
      "                                      amplitude, periodMinutes, algorithmState (18 temp issue, 25 failed)\n"
      "  forget                              g7sim's POST /forget: drop every display's key\n"
      "  new {\"model\":\"stelo\",\"code\":\"1234\"}   g7sim's POST /new (restarts): model g7|oneplus|stelo, days, code,\n"
      "                                      serial, ageMinutes, lifetimeHours, warmupMinutes; or new key=value ...\n"
      "  shorthand, keeping the algorithm state: glucose fixed <mg/dL> | glucose sine <mean> [amp] [period]\n"
      "      | glucose walk <target> | algo <state>\n"
      "Omnipod pod:\n"
      "  pod                                 the pod's status\n"
      "  pod fresh                           forget the pod and start as a new, unpaired one\n"
      "  pod mode dash|o5                    become a new DASH or Omnipod 5 pod\n"
      "  pod changeReservoir <units>         set the reservoir (0-200 U)\n"
      "  pod setActiveTime <minutes>         set how long the pod has been active\n"
      "  pod setAlerts <mask>                raise alert slots (0-255, 0 clears)\n"
      "  pod setFault <code>                 raise a fault (0 clears)\n"
      "  pod crashNextCommand before|after   reboot before or after processing the next delivery command\n"
      "  pod ignoreNextMessages <count>      stay connected but leave that many messages unanswered\n"
      "  pod failNextConnects <count> [sec]  loopkit/pod's command; needs its ImpossiBLE transport\n"
      "  pod selftest                        check the pod's crypto on this board\n"
      "Board:\n"
      "  wifi <ssid> [password] | wifi off   join Wi-Fi for the web panels + NTP (restarts)\n"
      "  scan                                list the Wi-Fi networks the board can see\n"
      "  reboot");
}

static std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
    size_t j = i;
    while (j < s.size() && !isspace((unsigned char)s[j])) j++;
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

static void command(const std::string& line) {
  auto a = split(line);
  if (a.empty()) return;
  const std::string& cmd = a[0];
  auto num = [&](size_t i, double def) { return i < a.size() ? atof(a[i].c_str()) : def; };

  // What follows the command word, for the commands that take g7sim's JSON.
  size_t sp = line.find_first_of(" \t");
  std::string rest = sp == std::string::npos ? "" : line.substr(line.find_first_not_of(" \t", sp) == std::string::npos ? line.size() : line.find_first_not_of(" \t", sp));
  bool jsonArg = !rest.empty() && rest[0] == '{';

  if (cmd == "help" || cmd == "?") {
    help();
  } else if (cmd == "status") {
    printStatus();
  } else if (cmd == "glucose" && jsonArg) {
    // g7sim's POST /glucose, body and all: the mode is replaced whole, so
    // leaving out algorithmState returns the sensor to normal.
    JsonDocument j;
    GlucoseMode m;
    if (deserializeJson(j, rest) || !modeFromJson(j, m)) {
      Serial.println("bad glucose mode");
      return;
    }
    sensor->setGlucoseMode(m);
    logf("glucose mode now %s %.0f, algorithm state %u (from the next reading)", m.kind.c_str(), m.value, m.algorithmState);
  } else if (cmd == "glucose" && a.size() >= 2) {
    JsonDocument cur;
    sensor->status(cur);
    GlucoseMode m;
    m.kind = a[1];
    m.algorithmState = cur["glucoseMode"]["algorithmState"] | 0;
    if (m.kind == "fixed" || m.kind == "walk") {
      m.value = num(2, 120);
    } else if (m.kind == "sine") {
      m.value = num(2, 120), m.amplitude = num(3, 50), m.periodMinutes = num(4, 240);
    } else {
      Serial.println("glucose: want fixed, sine or walk");
      return;
    }
    sensor->setGlucoseMode(m);
    logf("glucose mode now %s %.0f (from the next reading)", m.kind.c_str(), m.value);
  } else if (cmd == "algo" && a.size() == 2) {
    JsonDocument cur;
    sensor->status(cur);
    GlucoseMode m;
    modeFromJson(cur["glucoseMode"], m);
    m.algorithmState = atoi(a[1].c_str());
    sensor->setGlucoseMode(m);
    logf("algorithm state now %u (0 = normal)", m.algorithmState);
  } else if (cmd == "forget") {
    sensor->forgetSlots();
    logf("forgot every display's key");
  } else if (cmd == "new") {
    SensorConfig c;
    JsonDocument j;
    if (jsonArg) {  // g7sim's POST /new
      if (deserializeJson(j, rest)) {
        Serial.println("bad sensor config");
        return;
      }
      configFromJson(j, c);
    }
    for (size_t i = 1; !jsonArg && i < a.size(); i++) {
      size_t eq = a[i].find('=');
      if (eq == std::string::npos) {
        Serial.printf("new: expected key=value, got %s\n", a[i].c_str());
        return;
      }
      std::string k = a[i].substr(0, eq), v = a[i].substr(eq + 1);
      if (k == "model") c.model = v;
      else if (k == "code") c.code = v;
      else if (k == "serial") c.serial = strtoull(v.c_str(), nullptr, 10);
      else if (k == "ageMinutes" || k == "age") c.ageMinutes = atof(v.c_str());
      else if (k == "days") c.days = atoi(v.c_str());
      else if (k == "lifetimeHours" || k == "lifetime") c.lifetimeHours = atof(v.c_str());
      else if (k == "warmupMinutes" || k == "warmup") c.warmupMinutes = atof(v.c_str());
      else {
        Serial.printf("new: unknown key %s\n", k.c_str());
        return;
      }
    }
    State next;
    std::string err;
    if (!newState(c, next, err)) {
      Serial.printf("new: %s\n", err.c_str());
      return;
    }
    logf("replacing %s with a new %s sensor, code %s", sensor->name().c_str(), next.model.c_str(), next.code.c_str());
    replaceSensor(next);
  } else if (cmd == "pod") {
    std::string sub = a.size() >= 2 ? a[1] : "";
    std::string v = a.size() >= 3 ? a[2] : "";
    // The commands and their names are loopkit/pod's control API:
    // `pod <command> <value>` sends {"command": <command>, "value": <value>}.
    auto isNumber = [](const std::string& x) {
      char* end = nullptr;
      strtod(x.c_str(), &end);
      return !x.empty() && *end == 0;
    };
    // The names this console used before it took loopkit/pod's.
    static const std::pair<const char*, const char*> old[] = {
        {"new", "fresh"}, {"type", "mode"}, {"reservoir", "changeReservoir"},
        {"age", "setActiveTime"}, {"fault", "setFault"}, {"alerts", "setAlerts"}};
    for (auto& [was, now] : old)
      if (sub == was) sub = now;

    if (sub.empty()) Serial.printf("pod: %s\n", pod::statusJson().c_str());
    else if (sub == "selftest") Serial.printf("pod self-test:\n%s", pod::selfTest().c_str());
    else if (sub == "fresh") podCommand("{\"command\":\"resetPod\"}");
    else if (sub == "mode") {
      if (v == "dash" || v == "o5") podCommand(std::string("{\"command\":\"setPodType\",\"value\":") + (v == "o5" ? "1" : "0") + "}");
      else Serial.println("pod mode: want dash or o5");
    } else if (sub == "crashNextCommand") {
      if (v == "before" || v == "after" || v == "true" || v == "false")
        podCommand(std::string("{\"command\":\"crashNextCommand\",\"beforeProcessing\":") + (v == "before" || v == "true" ? "true" : "false") + "}");
      else Serial.println("pod crashNextCommand: want before or after");
    } else if (sub.find_first_of("\"\\{}") != std::string::npos || (!v.empty() && !isNumber(v))) {
      Serial.println("pod: want pod <command> <number>; try help");
    } else {
      std::string json = "{\"command\":\"" + sub + "\"";
      if (!v.empty()) json += ",\"value\":" + v;
      if (sub == "failNextConnects" && a.size() >= 4 && isNumber(a[3])) json += ",\"hangUpAfter\":" + a[3];
      podCommand(json + "}");
    }
  } else if (cmd == "wifi" && a.size() >= 2) {
    saveWifi(a[1] == "off" ? "" : a[1].c_str(), a.size() >= 3 ? a[2].c_str() : "");
    sensor->persist();
    delay(200);
    ESP.restart();
  } else if (cmd == "scan") {
    int n = WiFi.scanNetworks();
    Serial.printf("%d networks (2.4 GHz only):\n", n);
    for (int i = 0; i < n; i++)
      Serial.printf("  %-32s ch %2d  %4d dBm  auth %d\n", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
                    WiFi.encryptionType(i));
    WiFi.scanDelete();
  } else if (cmd == "reboot") {
    sensor->persist();
    delay(200);
    ESP.restart();
  } else {
    Serial.printf("unknown command: %s (try help)\n", line.c_str());
  }
}

static void pollConsole() {
  static std::string line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (!line.empty()) {
        Serial.printf("> %s\n", line.c_str());
        command(line);
      }
      line.clear();
    } else if (line.size() < 200) {
      line.push_back(c);
    }
  }
}

// MARK: - Main

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(50);  // do not stall for long when no one is reading the console
  delay(1500);
  logf("ESP-Sim + G7: an Omnipod pod and Dexcom G7 simulator. TEST EQUIPMENT: no insulin, made-up data; never use for therapy.");

  if (!LittleFS.begin(true)) logf("LittleFS mount failed; state will not persist");

  State st;
  if (!loadState(st)) {
    std::string err;
    newState(SensorConfig(), st, err);
    saveState(st);
    logf("new sensor");
  }
  identity::init(st.leafKey, st.serial);
  sensor = new Sensor(st);

  JsonDocument d;
  sensor->status(d);
  logf("%s (%s, %d-day preset, lifetime %.0f h, warmup %.0f min) serial %llu, sensor age %lld min",
       sensor->name().c_str(), sensor->model().c_str(), d["days"].as<int>(), d["lifetimeHours"].as<double>(),
       d["warmupMinutes"].as<double>(), sensor->serial(), sensor->now() / MINUTE);
  logf("PAIRING CODE %s", sensor->pairingCode().c_str());

  pod::begin();
  startWifi();
  startWeb();
  radio::begin(sensor);
  pod::start();
  logf("ready — type 'help' on this console");
}

void loop() {
  static int64_t lastTick = 0;
  int64_t now = monoMs();
  if (now - lastTick >= 50) {
    lastTick = now;
    radio::tick();
  }
  pollConsole();
  if (wifiConfigured) pollWifi();
  web.handleClient();
  if (restartAt && now >= restartAt) {
    sensor->persist();
    ESP.restart();
  }
  delay(5);
}
