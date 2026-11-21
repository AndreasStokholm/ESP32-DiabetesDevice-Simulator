// ESP-Sim + G7: one ESP32-S3 that is both an Omnipod pod simulator and a
// Dexcom G6/G7 sensor simulator, for testing Loop, Trio and other apps with no
// real pod or sensor.
//
//   the pod   ESP-Sim's pod core (github.com/nightscout/ESP-Sim), in lib/dashpod
//   the CGM   ESP-Sim's portable G6/G7 core (the same repo), in lib/cgmsim,
//             wired up by src/sensor_app.cpp
//
// TEST EQUIPMENT ONLY. Not a Dexcom or Insulet product. It delivers no
// insulin and every value it reports is made up. Never use it, or an app
// paired to it, for therapy.
//
// Control: the USB serial console (type `help`) and the web pages: the home
// page at /, the CGM's at /g7 and the pod's at /pod. With no Wi-Fi network saved,
// the board runs its own access point (name and password are printed at boot)
// and answers every HTTP probe as a captive portal, so a phone that joins it
// gets the Wi-Fi sheet at /setup.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_sntp.h>
#include <math.h>

#include "fail.h"
#include "home_html.h"
#include "index_html.h"
#include "medtrum_app.h"
#include "medtrum_html.h"
#include "pod.h"
#include "pod_html.h"
#include "portal.h"
#include "radio.h"
#include "sensor_app.h"
#include "settings_html.h"
#include "setup_html.h"
#include "util.h"

constexpr int64_t SEC = 1000;  // the console and the timers work in these

// The firmware's commit. tools/make_flasher.sh pins one for the flasher's
// manifest, but the PlatformIO build has no hook to pass it to the firmware
// yet, so a plain `pio run` says "unknown" instead of inventing a hash.
#ifndef ESP_SIM_COMMIT
#define ESP_SIM_COMMIT "unknown"
#endif

static WebServer web(80);
static bool wifiConfigured = false, apStarted = false;
static volatile int staFailures = 0;
static int64_t restartAt = 0;
static String apSsid, apPass;
static DNSServer dns;
static bool dnsStarted = false;

// MARK: - Commands shared by the console and the API

// What POST /glucose and the console's `glucose` do: the core keeps one next
// glucose and a trend. g7sim's sine and random walk have no counterpart in
// the core, so `kind` only decides the log line: the value is the next reading
// either way, and `algorithmState` is the forced state (0 returns the sensor
// to normal, as in g7sim). Returns nullptr, or what was wrong.
static const char* applyGlucose(const std::string& kind, double value, int trend, int state) {
  if (kind.empty()) return "want a glucose kind";
  long g = lround(value);
  if (g < 40) g = 40;
  if (g > 400) g = 400;
  char body[96];
  snprintf(body, sizeof body, "{\"command\":\"cgmSetGlucose\",\"glucose\":%ld,\"trend\":%d}", g, trend);
  if (const char* err = sensor::command(body)) return err;
  snprintf(body, sizeof body, "{\"command\":\"cgmSetState\",\"state\":%d}", state);
  if (const char* err = sensor::command(body)) return err;
  if (kind != "fixed") {
    logf("glucose: the CGM core holds a fixed next glucose, not a %s; %ld is the next reading", kind.c_str(), g);
  }
  return nullptr;
}

// A new sensor of one kind, `ageMinutes` old (0: now), with a random id.
static const char* applyNewCgm(int kind, long ageMinutes) {
  char body[128];
  snprintf(body, sizeof body, "{\"command\":\"cgmNewSensor\",\"kind\":%d,\"ageMinutes\":%ld}", kind,
           ageMinutes);
  return sensor::command(body);
}

// g7sim's /status shape, filled from the core: the field names the old clients
// (src/home_html.h among them) read stay the same. The fields the core does
// not have are reported empty rather than invented: it has no pairing code (it
// auto-authenticates) and no per-display slots (it bonds every phone), so
// `code` is "" and `slots` is {}. It keeps one next glucose and a trend, not
// g7sim's waveforms, so `glucoseMode.kind` is "fixed". The core's own view is
// under `cgm`, and the panel reads /g7/status rather than this.
static void buildStatus(JsonDocument& d) {
  JsonDocument core;
  if (deserializeJson(core, String(sensor::statusJson().c_str()))) return;
  const int kind = core["kind"] | 0;
  const int64_t activated = core["activated"] | 0LL;
  d["name"] = String(core["name"] | "");
  d["serial"] = 0;
  d["code"] = "";
  d["model"] = kind == CGM_G6 ? "g6" : kind == CGM_G7 ? "g7" : "off";
  d["days"] = 10;
  d["lifetimeHours"] = kind == CGM_G6 ? 240 : 252;  // 10 days; a G7 carries its 12-hour grace period
  d["warmupMinutes"] = kind == CGM_G6 ? 120 : 27;
  d["ageSeconds"] = sensor::now() - activated;
  JsonObject g = d["glucoseMode"].to<JsonObject>();
  g["kind"] = "fixed";
  g["value"] = core["glucose"] | 120;
  g["amplitude"] = 0;
  g["periodMinutes"] = 0;
  int forced = core["forcedState"] | 0;
  if (forced) g["algorithmState"] = forced;
  d["slots"].to<JsonObject>();  // empty: the core has no pairing slots
  JsonObject last = core["last"].as<JsonObject>();
  if (!last.isNull()) {
    JsonObject o = d["latest"].to<JsonObject>();
    o["index"] = (int)(last["seq"] | 0) - 1;
    o["time"] = (uint32_t)((last["t"] | 0LL) - activated);
    if ((last["glucose"] | 0) > 0) {
      o["glucose"] = last["glucose"];
      // As g7sim: a reading only carries a trend when it has a glucose value.
      o["trend"] = (core["trend"] | 0) / 10.0;  // tenths of mg/dL per minute
    }
    o["state"] = last["state"] | 0;
  }
  JsonObject c = d["cgm"].to<JsonObject>();  // additive: the core's own fields
  c["kind"] = kind;
  c["id"] = String(core["id"] | "");
  c["connected"] = core["connected"] | false;
  c["advertising"] = core["advertising"] | false;
  c["radioConnected"] = core["radioConnected"] | false;
  c["openLinks"] = radio::openLinks();
}

// MARK: - HTTP API (api.go)

static void sendJson(int code, JsonDocument& d) {
  String s;
  serializeJsonPretty(d, s);
  web.send(code, "application/json", s);
}

static void sendStatus() {
  JsonDocument d;
  buildStatus(d);
  sendJson(200, d);
}

// MARK: - Failure injection

static String failStateJson() {
  JsonDocument d;
  int64_t now = monoMs();
  for (int i = 0; i < FAIL_DEVICE_COUNT; i++) {
    FailState s = fail_get((FailDevice)i);
    JsonObject o = d[fail_device_name((FailDevice)i)].to<JsonObject>();
    o["drop"] = s.dropPending != 0;
    o["refuse"] = s.refuseConnects;
    o["quietMs"] = s.quietUntilMs > now ? s.quietUntilMs - now : 0;
    o["delayMs"] = s.delayMs;
    o["ignore"] = s.ignoreMessages;
  }
  String out;
  serializeJsonPretty(d, out);
  return out;
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

// MARK: - The captive portal

// The `setup=done` cookie that "Continue without joining" sets: once present,
// / in AP mode serves the app instead of redirecting back to the sheet.
static bool setupDone() {
  String cookies = web.header("Cookie");
  int start = 0;
  while (start < (int)cookies.length()) {
    int end = cookies.indexOf(';', start);
    if (end < 0) end = cookies.length();
    String kv = cookies.substring(start, end);
    kv.trim();
    int eq = kv.indexOf('=');
    if (eq > 0) {
      String name = kv.substring(0, eq), value = kv.substring(eq + 1);
      name.trim();
      value.trim();
      if (name == "setup" && value == "done") return true;
    }
    start = end + 1;
  }
  return false;
}

// A saved network the board is actually on. In AP mode, or while a saved
// network is unreachable and the AP has taken over, this is false.
static bool hasNetwork() { return wifiConfigured && WiFi.status() == WL_CONNECTED; }

// The one Wi-Fi save handler: /settings/wifi is its home, and /setup/wifi,
// /pod/wifi and /medtrum/wifi stay as aliases to the SAME handler. Save, then
// let the loop restart.
static void sendWifiSaved() {
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
}

// MARK: - The board's own page: Network + Board

static String savedSsid() {
  Preferences p;
  p.begin("net", true);
  String ssid = p.isKey("ssid") ? p.getString("ssid") : "";
  p.end();
  return ssid;
}

// MARK: - The pump selector

// The selector's canonical values, and the old names they replace: `pod` and
// `both` both become pod-o5 (there is no "both" any more -- the pod and the
// Medtrum base are mutually exclusive), and `medtrum` is unchanged. Returns the
// canonical name, or nullptr for anything else; *wasBoth is set when the old
// "both" was seen, so the caller can say it is gone.
static const char* canonicalPump(const char* v, bool* wasBoth) {
  *wasBoth = false;
  if (!v) return nullptr;
  if (strcmp(v, "off") == 0) return "off";
  if (strcmp(v, "pod-dash") == 0) return "pod-dash";
  if (strcmp(v, "pod-o5") == 0) return "pod-o5";
  if (strcmp(v, "medtrum") == 0) return "medtrum";
  if (strcmp(v, "pod") == 0) return "pod-o5";
  if (strcmp(v, "both") == 0) {
    *wasBoth = true;
    return "pod-o5";
  }
  return nullptr;
}

// Applies a canonical pump choice: the radio gets the selection, and a pod
// choice also makes the pod the right type (a new pod, as `pod mode` does).
// Only a pod that is the wrong type is reset, so a reboot keeps its state.
static void applyPumpChoice(const char* c) {
  if (strcmp(c, "off") == 0) {
    radio::setPump(radio::PUMP_OFF);
  } else if (strcmp(c, "medtrum") == 0) {
    radio::setPump(radio::PUMP_MEDTRUM);
  } else {
    dash_pod_type_t want = strcmp(c, "pod-dash") == 0 ? DASH_POD_DASH : DASH_POD_O5;
    if (pod::type() != want) pod::setType(want);
    radio::setPump(radio::PUMP_POD);
  }
}

// Persists the selector's canonical value and applies it. Returns that value.
static const char* savePumpChoice(const char* c) {
  Preferences pr;
  pr.begin("board", false);
  pr.putString("pump", c);
  pr.end();
  applyPumpChoice(c);
  return c;
}

// What /settings/status reports: the radio's selection, with the pod's type.
static const char* pumpChoiceName() {
  int sel = radio::pump();
  if (sel == radio::PUMP_OFF) return "off";
  if (sel == radio::PUMP_MEDTRUM) return "medtrum";
  return pod::type() == DASH_POD_O5 ? "pod-o5" : "pod-dash";
}

static void sendSettingsStatus() {
  JsonDocument d;
  d["wifi"]["ssid"] = savedSsid();
  d["wifi"]["joined"] = hasNetwork();
  d["ip"] = hasNetwork() ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  d["clock"] = wallMs() ? "set from NTP" : "not set (NTP never ran)";
  d["firmware"]["commit"] = ESP_SIM_COMMIT;
  d["firmware"]["built"] = __DATE__ " " __TIME__;
  d["pump"] = pumpChoiceName();
  sendJson(200, d);
}

// The OS probes: answer each one the way that closes (or opens) its sheet.
static void sendPortalProbe() {
  switch (portalDecide(web.uri().c_str(), "GET", hasNetwork() ? 1 : 0, setupDone() ? 1 : 0)) {
    case PORTAL_PROBE_APPLE_HTML:
      web.send_P(200, "text/html; charset=utf-8", setupHTML);
      break;
    case PORTAL_PROBE_APPLE_SUCCESS:
      web.send_P(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
      break;
    case PORTAL_PROBE_ANDROID_REDIRECT:
    case PORTAL_PROBE_WINDOWS_REDIRECT:
    case PORTAL_PROBE_FWLINK:
      web.sendHeader("Location", "/setup");
      web.send(302, "text/plain", "");
      break;
    case PORTAL_PROBE_ANDROID_204:
      web.send(204, "text/plain", "");
      break;
    case PORTAL_PROBE_CONNECTTEST:
      web.send_P(200, "text/plain", "Microsoft Connect Test");
      break;
    case PORTAL_PROBE_NCSI:
      web.send_P(200, "text/plain", "Microsoft NCSI");
      break;
    default:
      web.send(404, "text/plain", "not found\n");
      break;
  }
}

static void startWeb() {
  // The captive portal's own pages.
  web.on("/", HTTP_GET, [] {
    if (portalDecide("/", "GET", hasNetwork() ? 1 : 0, setupDone() ? 1 : 0) == PORTAL_REDIRECT_SETUP) {
      web.sendHeader("Location", "/setup");
      web.send(302, "text/plain", "");
      return;
    }
    web.send_P(200, "text/html; charset=utf-8", homeHTML);
  });
  web.on("/g7", HTTP_GET, [] { web.send_P(200, "text/html; charset=utf-8", indexHTML); });
  web.on("/setup", HTTP_GET, [] { web.send_P(200, "text/html; charset=utf-8", setupHTML); });
  web.on("/setup/wifi", HTTP_POST, sendWifiSaved);
  web.on("/setup/dismiss", HTTP_POST, [] {
    web.sendHeader("Set-Cookie", "setup=done; Path=/; Max-Age=604800");
    web.send(200, "text/plain", "dismissed\n");
  });
  web.on("/hotspot-detect.html", HTTP_GET, sendPortalProbe);
  web.on("/library/test/success.html", HTTP_GET, sendPortalProbe);
  web.on("/generate_204", HTTP_GET, sendPortalProbe);
  web.on("/gen_204", HTTP_GET, sendPortalProbe);
  web.on("/connecttest.txt", HTTP_GET, sendPortalProbe);
  web.on("/ncsi.txt", HTTP_GET, sendPortalProbe);
  web.on("/fwlink/", HTTP_GET, sendPortalProbe);
  web.on("/fwlink", HTTP_GET, sendPortalProbe);
  web.on("/settings", HTTP_GET, [] { web.send_P(200, "text/html; charset=utf-8", settingsHTML); });
  web.on("/settings/status", HTTP_GET, sendSettingsStatus);
  web.on("/settings/wifi", HTTP_POST, sendWifiSaved);
  web.on("/settings/forget", HTTP_POST, [] {
    saveWifi("", "");
    web.send(200, "text/plain", "forgotten; restarting\n");
    restartAt = monoMs() + 1000;
  });
  web.on("/settings/reboot", HTTP_POST, [] {
    web.send(200, "text/plain", "rebooting\n");
    restartAt = monoMs() + 1000;
  });
  // Which pump simulator advertises. A phone runs one pump app, and the pod and
  // the Medtrum base are mutually exclusive: exactly one may advertise and hold
  // the one link. The old `pod`/`both` names are still accepted, so a saved
  // board (and the old settings page) keeps working.
  web.on("/settings/pump", HTTP_POST, [] {
    JsonDocument j;
    if (deserializeJson(j, web.arg("plain")) || !j["pump"].is<const char*>()) {
      web.send(400, "text/plain", "want {\"pump\":\"off|pod-dash|pod-o5|medtrum\"}\n");
      return;
    }
    bool wasBoth = false;
    const char* c = canonicalPump(j["pump"].as<const char*>(), &wasBoth);
    if (!c) {
      web.send(400, "text/plain", "pump is off, pod-dash, pod-o5 or medtrum\n");
      return;
    }
    if (wasBoth) logf("pump simulators: \"both\" is gone; the pod and the Medtrum base are mutually exclusive; using pod-o5");
    savePumpChoice(c);
    logf("pump simulators: %s", c);
    web.send(200, "text/plain", strcmp(c, "off") == 0        ? "no pump advertises\n"
                                  : strcmp(c, "medtrum") == 0 ? "the Medtrum base advertises\n"
                                  : strcmp(c, "pod-dash") == 0 ? "an Omnipod DASH pod advertises\n"
                                                               : "an Omnipod 5 pod advertises\n");
  });
  web.on("/fail", HTTP_POST, [] {
    JsonDocument j;
    if (deserializeJson(j, web.arg("plain")) || !j["device"].is<const char*>()) {
      web.send(400, "text/plain", "want {\"device\":\"pod|g7|medtrum|all\", ...}\n");
      return;
    }
    const char* name = j["device"].as<const char*>();
    int dev = fail_device_from_name(name);
    bool all = strcmp(name, "all") == 0;
    if (dev < 0 && !all) {
      web.send(400, "text/plain", "device is pod, g7 (cgm), medtrum or all");
      return;
    }
    FailRequest r = {FAIL_ABSENT, FAIL_ABSENT, FAIL_ABSENT, FAIL_ABSENT, FAIL_ABSENT};
    if (!j["drop"].isNull()) {
      if (!j["drop"].is<bool>()) {
        web.send(400, "text/plain", "drop is true or false");
        return;
      }
      r.drop = j["drop"].as<bool>() ? 1 : 0;
    }
    if (!j["refuse"].isNull()) {
      if (!j["refuse"].is<int>() || j["refuse"].as<int>() < 0) {
        web.send(400, "text/plain", "refuse is 0-1000 connects");
        return;
      }
      r.refuse = j["refuse"].as<int>();
    }
    if (!j["quietMs"].isNull()) {
      if (!j["quietMs"].is<int>() || j["quietMs"].as<int>() < 0) {
        web.send(400, "text/plain", "quietMs is 0-86400000 ms");
        return;
      }
      r.quietMs = j["quietMs"].as<int>();
    }
    if (!j["delayMs"].isNull()) {
      if (!j["delayMs"].is<int>() || j["delayMs"].as<int>() < 0) {
        web.send(400, "text/plain", "delayMs is 0-60000 ms");
        return;
      }
      r.delayMs = j["delayMs"].as<int>();
    }
    if (!j["ignore"].isNull()) {
      if (!j["ignore"].is<int>() || j["ignore"].as<int>() < 0) {
        web.send(400, "text/plain", "ignore is 0-1000 messages");
        return;
      }
      r.ignore = j["ignore"].as<int>();
    }
    int64_t now = monoMs();
    const char* err = nullptr;
    auto applyOne = [&](FailDevice d) {
      if (!err) err = fail_apply(d, &r, now);
    };
    if (all) {
      for (int i = 0; i < FAIL_DEVICE_COUNT; i++) applyOne((FailDevice)i);
    } else {
      applyOne((FailDevice)dev);
    }
    if (err) {
      web.send(400, "text/plain", err);
      return;
    }
    const char* who = all ? "all" : name;
    if (r.drop >= 0) logf("fail: %s %s", who, r.drop ? "drops the next connect" : "drop cleared");
    if (r.refuse >= 0) logf("fail: %s refuses %d connects", who, r.refuse);
    if (r.quietMs >= 0) logf("fail: %s quiet for %d ms", who, r.quietMs);
    if (r.delayMs >= 0) logf("fail: %s delays notifications by %d ms", who, r.delayMs);
    if (r.ignore >= 0) logf("fail: %s ignores %d messages", who, r.ignore);
    web.send(200, "application/json", failStateJson());
  });
  web.on("/status", HTTP_GET, sendStatus);
  // The CGM's panel and its own API (the new core, under /g7).
  web.on("/g7/status", HTTP_GET, [] {
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "application/json", sensor::statusJson().c_str());
  });
  web.on("/g7/command", HTTP_POST, [] {
    String body = web.arg("plain");
    if (const char* err = sensor::command(body.c_str())) {
      web.send(400, "text/plain", err);
      return;
    }
    web.send(200, "application/json", sensor::statusJson().c_str());
  });
  // g7sim's four requests, unchanged at the root and with their field names:
  // they are the same controls the panel's /g7/command takes.
  web.on("/glucose", HTTP_POST, [] {
    JsonDocument j;
    if (deserializeJson(j, web.arg("plain")) || !j["kind"].is<const char*>()) {
      web.send(400, "text/plain", "bad glucose mode\n");
      return;
    }
    std::string kind = j["kind"].as<const char*>();
    double value = j["value"] | 0.0;
    if (const char* err = applyGlucose(kind, value, j["trend"] | 0, j["algorithmState"] | 0)) {
      web.send(400, "text/plain", err);
      return;
    }
    logf("glucose now %s %.0f (from the next reading)", kind.c_str(), value);
    sendStatus();
  });
  web.on("/forget", HTTP_POST, [] {
    if (const char* err = sensor::command("{\"command\":\"cgmForget\"}")) {
      web.send(400, "text/plain", err);
      return;
    }
    logf("forget: dropped the CGM's link; the core keeps no pairing to forget");
    sendStatus();
  });
  web.on("/new", HTTP_POST, [] {
    JsonDocument j;
    if (deserializeJson(j, web.arg("plain"))) {
      web.send(400, "text/plain", "bad sensor config\n");
      return;
    }
    // g7sim's body: `model` picks the kind (g6 as an extension; g7, oneplus and
    // stelo are all a G7 sensor here), `ageMinutes` ages it. `days`, `code`,
    // `serial`, `lifetimeHours` and `warmupMinutes` are taken and not applied:
    // the core's G7 session is fixed at 10.5 days with a 27-minute warm-up and
    // it has no pairing code.
    std::string model = j["model"] | "";
    int kind = model == "g6" ? (int)CGM_G6 : (int)CGM_G7;
    long age = j["ageMinutes"].isNull() ? 0 : lround(j["ageMinutes"].as<double>());
    if (age < 0) age = 0;
    if (!j["days"].isNull() || (j["code"].is<const char*>() && *j["code"].as<const char*>()) ||
        !j["lifetimeHours"].isNull() || !j["warmupMinutes"].isNull()) {
      logf("new: the CGM core has a fixed 10.5-day G7 session (27 min warm-up) and no pairing code; those fields are not applied");
    }
    if (const char* err = applyNewCgm(kind, age)) {
      web.send(400, "text/plain", err);
      return;
    }
    JsonDocument r;
    r["name"] = String(sensor::name().c_str());
    r["code"] = "";  // the core auto-authenticates: it has no pairing code
    r["serial"] = 0;
    sendJson(200, r);
    logf("replacing the CGM with a new %s, restarting", kind == CGM_G6 ? "G6 transmitter" : "G7 sensor");
    restartAt = monoMs() + 1000;
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
  web.on("/pod/wifi", HTTP_POST, sendWifiSaved);
  // The Medtrum patch's panel and API (under /medtrum).
  web.on("/medtrum", HTTP_GET, [] { web.send_P(200, "text/html; charset=utf-8", medtrumHTML); });
  web.on("/medtrum/status", HTTP_GET, [] {
    web.sendHeader("Cache-Control", "no-store");
    web.send(200, "application/json", medtrum::statusJson().c_str());
  });
  web.on("/medtrum/command", HTTP_POST, [] {
    String body = web.arg("plain");
    // The patch core has no crash hook, so the base reboots itself: the phone
    // loses its in-flight command and finds the base again. Compare the JSON
    // `command` field exactly, as the pod's panel does; a stray token in any
    // other field must not reboot the board.
    JsonDocument j;
    if (!deserializeJson(j, body) && j["command"].is<const char*>() &&
        strcmp(j["command"].as<const char*>(), "medtrumCrash") == 0) {
      web.send(200, "application/json", medtrum::statusJson().c_str());
      restartAt = monoMs() + 1500;
      return;
    }
    if (const char* err = medtrum::command(body.c_str())) {
      web.send(400, "text/plain", err);
      return;
    }
    web.send(200, "application/json", medtrum::statusJson().c_str());
  });
  web.on("/medtrum/wifi", HTTP_POST, sendWifiSaved);
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

// The captive DNS: every A record answers with the board's own AP address,
// so a probe for any name lands here. It only runs while the AP is up.
static void startDns() {
  if (dnsStarted) return;
  IPAddress ip = WiFi.softAPIP();
  if (ip[0] == 0) return;
  dns.start(53, "*", ip);
  dnsStarted = true;
  logf("Wi-Fi: DNS answers every name with %s", ip.toString().c_str());
}

static void stopDns() {
  if (!dnsStarted) return;
  dns.stop();
  dnsStarted = false;
  logf("Wi-Fi: captive DNS stopped");
}

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
  startDns();
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
    stopDns();
    logf("Wi-Fi up: panels at http://%s/  (http://esp-sim.local/)", WiFi.localIP().toString().c_str());
  }
  wasUp = up;
  if (!up && staFailures >= 5 && !apStarted) {
    logf("Wi-Fi: could not join the saved network: starting the access point too");
    startAP();
  }
  // The AP may still be up after a later drop (a sleeping hotspot), and the
  // sheet needs its wildcard DNS again; it was stopped when the board joined.
  if (!up && apStarted && !dnsStarted) {
    startDns();
  }
  // Keep trying while the network is away (e.g. a phone hotspot that sleeps).
  static int64_t lastTry = monoMs();
  if (!up && monoMs() - lastTry > 30 * SEC) {
    lastTry = monoMs();
    WiFi.reconnect();
  }
  // The CGM keeps its own monotonic clock (it starts from the wall clock at
  // boot), so NTP setting the wall clock changes nothing it reports.
}

// MARK: - Serial console

static void printStatus() {
  JsonDocument d;
  buildStatus(d);
  d.remove("slots");
  String s;
  serializeJsonPretty(d, s);
  Serial.println(s);
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
      "The CGM (Dexcom G6 transmitter or G7 sensor, from ESP-Sim's portable core):\n"
      "  status                              the CGM's status, the web address and the pod's\n"
      "  kind g6|g7|off                      be a G6 transmitter, a G7 sensor, or neither\n"
      "  id <id>                             set the id: 6 characters for a G6, 2 for a G7\n"
      "  glucose {\"kind\":\"fixed\",\"value\":65}  g7sim's POST /glucose: kind fixed|sine|walk, value,\n"
      "                                      trend (0.1 mg/dL/min), amplitude, periodMinutes,\n"
      "                                      algorithmState (18 temp issue, 25 failed)\n"
      "  cal <mg/dL>                         the last calibration (and the next reading)\n"
      "  forget                              g7sim's POST /forget: drop the current link\n"
      "  new {\"model\":\"g7\",\"ageMinutes\":0}    g7sim's POST /new (restarts): model g6|g7|oneplus|stelo,\n"
      "                                      ageMinutes; days, code, serial, lifetimeHours and\n"
      "                                      warmupMinutes are taken and not applied; or new key=value ...\n"
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
      "Failure injection (one radio, one decision set per device):\n"
      "  fail <pod|g7 (cgm)|medtrum|all> drop  drop the next connection once\n"
      "  fail <device> refuse <n>           drop the next n connections\n"
      "  fail <device> quiet <ms>           stop advertising that device for ms\n"
      "  fail <device> delay <ms>           delay each notification from that device\n"
      "  fail <device> ignore <n>           drop the next n messages from the phone\n"
      "  fail <device> clear                clear every knob for that device\n"
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
  } else if (cmd == "kind" && a.size() == 2) {
    int k = a[1] == "g6" ? (int)CGM_G6 : a[1] == "g7" ? (int)CGM_G7 : a[1] == "off" ? (int)CGM_OFF : -1;
    if (k < 0) {
      Serial.println("kind: want g6, g7 or off");
      return;
    }
    char body[64];
    snprintf(body, sizeof body, "{\"command\":\"cgmSetKind\",\"kind\":%d}", k);
    if (const char* err = sensor::command(body)) Serial.printf("kind: %s\n", err);
    else logf("the CGM is now %s (%s)", a[1].c_str(), sensor::name().c_str());
  } else if ((cmd == "id" || cmd == "cal") && a.size() == 2) {
    char body[96];
    if (cmd == "id") snprintf(body, sizeof body, "{\"command\":\"cgmSetId\",\"id\":\"%.8s\"}", a[1].c_str());
    else snprintf(body, sizeof body, "{\"command\":\"cgmSetCalibration\",\"glucose\":%d}", atoi(a[1].c_str()));
    if (const char* err = sensor::command(body)) Serial.printf("%s: %s\n", cmd.c_str(), err);
    else if (cmd == "id") logf("the CGM's id is now %s (%s)", a[1].c_str(), sensor::name().c_str());
    else logf("calibrated to %s mg/dL", a[1].c_str());
  } else if (cmd == "glucose" && jsonArg) {
    // g7sim's POST /glucose, body and all: the state is replaced whole, so
    // leaving out algorithmState returns the sensor to normal.
    JsonDocument j;
    if (deserializeJson(j, rest) || !j["kind"].is<const char*>()) {
      Serial.println("bad glucose mode");
      return;
    }
    std::string kind = j["kind"].as<const char*>();
    double value = j["value"] | 0.0;
    if (const char* err = applyGlucose(kind, value, j["trend"] | 0, j["algorithmState"] | 0)) {
      Serial.printf("glucose: %s\n", err);
      return;
    }
    logf("glucose now %s %.0f, algorithm state %d (from the next reading)", kind.c_str(), value,
         (int)(j["algorithmState"] | 0));
  } else if (cmd == "glucose" && a.size() >= 2) {
    std::string kind = a[1];
    if (kind != "fixed" && kind != "sine" && kind != "walk") {
      Serial.println("glucose: want fixed, sine or walk");
      return;
    }
    // The core keeps one next glucose and a trend, so a sine's swing
    // parameters are taken and not applied: the value is the next reading.
    if (const char* err = applyGlucose(kind, num(2, 120), sensor::trend(), sensor::forcedState())) {
      Serial.printf("glucose: %s\n", err);
      return;
    }
    logf("glucose now %s %.0f (from the next reading)", kind.c_str(), num(2, 120));
  } else if (cmd == "algo" && a.size() == 2) {
    char body[64];
    snprintf(body, sizeof body, "{\"command\":\"cgmSetState\",\"state\":%d}", atoi(a[1].c_str()));
    if (const char* err = sensor::command(body)) Serial.printf("algo: %s\n", err);
    else logf("algorithm state now %d (0 = normal)", atoi(a[1].c_str()));
  } else if (cmd == "forget") {
    if (const char* err = sensor::command("{\"command\":\"cgmForget\"}")) Serial.printf("forget: %s\n", err);
    else logf("forget: dropped the CGM's link; the core keeps no pairing to forget");
  } else if (cmd == "new") {
    // g7sim's POST /new, as the console types it.
    int kind = CGM_G7;
    long age = 0;
    if (jsonArg) {
      JsonDocument j;
      if (deserializeJson(j, rest)) {
        Serial.println("bad sensor config");
        return;
      }
      std::string model = j["model"] | "";
      kind = model == "g6" ? (int)CGM_G6 : (int)CGM_G7;
      if (!j["ageMinutes"].isNull()) age = lround(j["ageMinutes"].as<double>());
      if (!j["days"].isNull() || !j["lifetimeHours"].isNull() || !j["warmupMinutes"].isNull()) {
        logf("new: the CGM core has a fixed 10.5-day G7 session (27 min warm-up); those fields are not applied");
      }
    }
    for (size_t i = 1; !jsonArg && i < a.size(); i++) {
      size_t eq = a[i].find('=');
      if (eq == std::string::npos) {
        Serial.printf("new: expected key=value, got %s\n", a[i].c_str());
        return;
      }
      std::string k = a[i].substr(0, eq), v = a[i].substr(eq + 1);
      if (k == "model") kind = v == "g6" ? CGM_G6 : CGM_G7;
      else if (k == "ageMinutes" || k == "age") age = lround(atof(v.c_str()));
      else if (k == "days" || k == "code" || k == "serial" || k == "lifetimeHours" || k == "lifetime" ||
               k == "warmupMinutes" || k == "warmup") {
        // Taken for g7sim's command line; the core has no use for them.
      }
      else {
        Serial.printf("new: unknown key %s\n", k.c_str());
        return;
      }
    }
    if (age < 0) age = 0;
    if (const char* err = applyNewCgm(kind, age)) {
      Serial.printf("new: %s\n", err);
      return;
    }
    logf("replacing the CGM with a new %s, restarting", kind == CGM_G6 ? "G6 transmitter" : "G7 sensor");
    delay(200);
    ESP.restart();
  } else if (cmd == "fail") {
    if (a.size() < 3) {
      Serial.println("fail: want fail <pod|g7|medtrum|all> <drop|refuse|quiet|delay|ignore|clear> [n]");
      return;
    }
    bool all = a[1] == "all";
    int dev = fail_device_from_name(a[1].c_str());
    if (dev < 0 && !all) {
      Serial.println("fail: device is pod, g7 (cgm), medtrum or all");
      return;
    }
    const std::string& what = a[2];
    if (what == "clear") {
      if (all) fail_clear_all();
      else fail_clear((FailDevice)dev);
      logf("fail: %s cleared", all ? "every device" : a[1].c_str());
      Serial.println(failStateJson().c_str());
      return;
    }
    if (what != "drop" && what != "refuse" && what != "quiet" && what != "delay" && what != "ignore") {
      Serial.println("fail: want drop, refuse, quiet, delay, ignore or clear");
      return;
    }
    FailRequest r = {FAIL_ABSENT, FAIL_ABSENT, FAIL_ABSENT, FAIL_ABSENT, FAIL_ABSENT};
    int n = 0;
    if (what == "drop") {
      r.drop = 1;
    } else {
      if (a.size() < 4) {
        Serial.println("fail: want a number for refuse, quiet, delay or ignore");
        return;
      }
      double v = atof(a[3].c_str());
      if (!(v >= 0) || v > 86400000.0) {
        Serial.println("fail: number is out of range");
        return;
      }
      n = (int)v;
      if (what == "refuse") r.refuse = n;
      else if (what == "quiet") r.quietMs = n;
      else if (what == "delay") r.delayMs = n;
      else r.ignore = n;
    }
    int64_t now = monoMs();
    const char* err = nullptr;
    auto applyOne = [&](FailDevice d) {
      if (!err) err = fail_apply(d, &r, now);
    };
    if (all) {
      for (int i = 0; i < FAIL_DEVICE_COUNT; i++) applyOne((FailDevice)i);
    } else {
      applyOne((FailDevice)dev);
    }
    if (err) {
      Serial.printf("fail: %s\n", err);
      return;
    }
    const char* who = all ? "all" : a[1].c_str();
    if (r.drop >= 0) logf("fail: %s drops the next connect", who);
    else if (r.refuse >= 0) logf("fail: %s refuses %d connects", who, n);
    else if (r.quietMs >= 0) logf("fail: %s quiet for %d ms", who, n);
    else if (r.delayMs >= 0) logf("fail: %s delays notifications by %d ms", who, n);
    else if (r.ignore >= 0) logf("fail: %s ignores %d messages", who, n);
    Serial.println(failStateJson().c_str());
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
      if (v == "dash" || v == "o5") {
        std::string json = std::string("{\"command\":\"setPodType\",\"value\":") + (v == "o5" ? "1" : "0") + "}";
        if (const char* err = pod::command(json)) {
          Serial.printf("pod: %s\n", err);
        } else {
          Serial.printf("pod: %s\n", pod::statusJson().c_str());
          // Keep the selector's saved choice in step with the pod's type.
          savePumpChoice(v == "o5" ? "pod-o5" : "pod-dash");
        }
      } else {
        Serial.println("pod mode: want dash or o5");
      }
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
  logf("ESP-Sim + G7: an Omnipod pod and Dexcom G6/G7 simulator. TEST EQUIPMENT: no insulin, made-up data; never use for therapy.");

  if (!LittleFS.begin(true)) logf("LittleFS mount failed; state will not persist");

  // The CGM core: restores its saved state, or migrates the old g7sim shape,
  // and sets up its own storage and clock (it logs the kind, name and id).
  // After the pod's and the patch's begin(), before the radio starts.
  pod::begin();
  medtrum::begin();
  sensor::begin();

  startWifi();
  startWeb();
  radio::begin();
  {
    Preferences pr;
    pr.begin("board", true);
    String p = pr.isKey("pump") ? pr.getString("pump") : "";
    pr.end();
    bool wasBoth = false;
    const char* c = p.length() ? canonicalPump(p.c_str(), &wasBoth) : nullptr;
    if (wasBoth) logf("pump simulators: \"both\" is gone; the pod and the Medtrum base are mutually exclusive; using pod-o5");
    if (c) {
      applyPumpChoice(c);
      if (p != c) {
        // Rewrite the old name so the migration runs once, not every boot.
        Preferences w;
        w.begin("board", false);
        w.putString("pump", c);
        w.end();
      }
      logf("pump simulators: %s", c);
    } else if (p.length()) {
      logf("pump simulators: unknown saved value \"%s\"; using %s", p.c_str(), pumpChoiceName());
    } else {
      logf("pump simulators: %s", pumpChoiceName());
    }
  }
  sensor::start();
  pod::start();
  medtrum::start();
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
  if (dnsStarted) dns.processNextRequest();
  web.handleClient();
  if (restartAt && now >= restartAt) {
    ESP.restart();  // the CGM's state is committed as it changes (NVS)
  }
  delay(5);
}
