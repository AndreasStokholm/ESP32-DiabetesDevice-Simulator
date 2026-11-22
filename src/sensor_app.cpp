// The simulated Dexcom G6 transmitter / G7 sensor on the board: wires the
// portable CGM core (lib/cgmsim) to the radio, the clock and flash, and takes
// the control panel's commands. Ported from ESP-Sim's main/cgm_app.c.
//
// TEST EQUIPMENT ONLY: the readings are made up. Never let an app that
// controls a real pump or pod read them.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sensor_app.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <math.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "radio.h"
#include "util.h"

// The radio side of the core lives in src/radio.{h,cpp}: cgmNotify sends on
// the characteristic the phone subscribed to (once, as the reference does),
// cgmDisconnect only marks a hang-up for the radio's tick() to carry out (the
// core calls it from inside this file's mutex), and cgmAdvertise builds the
// FEBC advertisement under sensor::name(). Every CGM GATT event calls back in
// here: onConnect, onDisconnect, onSubscribe, onWrite and onRead.

namespace sensor {

static cgm_t c;
static SemaphoreHandle_t mu;
static SemaphoreHandle_t saveMu;
static nvs_handle_t nvs;
static bool nvsReady;
// State changed since the task last committed it. Both are guarded by mu.
static bool dirty;
static uint32_t saveGen;
// The highest generation handed to flash; guarded by saveMu.
static uint32_t savedGen;

#define LOCKED(stmt)                   \
  do {                                 \
    xSemaphoreTake(mu, portMAX_DELAY); \
    stmt;                              \
    xSemaphoreGive(mu);                \
  } while (0)

// MARK: - The platform, as the core sees it

static int64_t clockBase; // Unix seconds that monoMs()==0 corresponds to

// The simulated clock is monotonic: a saved state continues from its
// saved_at and advances with the monotonic clock, so it never jumps when NTP
// sets the wall clock (the phone only ever sees sensor-relative times). A
// fresh board starts from the wall clock, or a plausible epoch before NTP has
// set one.
static int64_t wallSeconds() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec;
}

static int64_t nowSeconds(void*) { return clockBase + monoMs() / 1000; }

static void logLine(void*, const char* msg) { logf("sensor: %s", msg); }

static bool persist(const cgm_state_t* st);
static bool loadBlob(cgm_state_t* out, int64_t* savedAt);

static const cgm_io_t io = {
    .ctx = nullptr,
    .send = [](void*, cgm_chr_t chr, const uint8_t* p, size_t n) { radio::cgmNotify(chr, p, n); },
    .disconnect = [](void*) { radio::cgmDisconnect(); },
    // Runs under the core's lock (cgm_tick calls it), so the id can be read here and
    // the checksum computed here; a call back into sensor::* for the crc would take
    // the same mutex again and deadlock the sensor task and every radio tick.
    .advertise = [](void*, bool on, const char* name, const uint8_t addr[6]) {
      uint16_t crc = cgm_crc16((const uint8_t*)c.st.id, strnlen(c.st.id, sizeof c.st.id));
      radio::cgmAdvertise(on, name, addr, crc);
    },
    .now = nowSeconds,
    .save = [](void*, const cgm_state_t*) {
      // Called by the core under mu, possibly on the NimBLE callback thread.
      // Only mark it: a flash commit there would stall every other core call,
      // so sensorTask (or a panel command) does the write.
      dirty = true;
      saveGen++;
    },
    .log = logLine,
};

// MARK: - Persistence (NVS "cgm"/"state", distinct from the pod's and the
// patch's)

static bool persist(const cgm_state_t* st) {
  if (!nvsReady) {
    static bool said;
    if (!said) {
      said = true;
      logf("sensor: NVS unavailable; the CGM will not persist");
    }
    return false;
  }
  sensor_saved_t s = {};
  s.magic = SENSOR_STATE_MAGIC;
  s.version = SENSOR_STATE_VERSION;
  s.saved_at = nowSeconds(nullptr);
  s.state = *st;
  esp_err_t err = nvs_set_blob(nvs, "state", &s, sizeof s);
  if (err == ESP_OK) err = nvs_commit(nvs);
  if (err != ESP_OK) {
    logf("sensor: could not save the CGM state: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

static bool loadBlob(cgm_state_t* out, int64_t* savedAt) {
  if (!nvsReady) return false;
  sensor_saved_t s;
  size_t len = sizeof s;
  esp_err_t err = nvs_get_blob(nvs, "state", &s, &len);
  if (err != ESP_OK) {
    // First boot has no blob; that is not a rejection and needs no log.
    if (err != ESP_ERR_NVS_NOT_FOUND) {
      logf("sensor: could not read the saved CGM state: %s; starting fresh", esp_err_to_name(err));
    }
    return false;
  }
  if (len != sizeof s) {
    logf("sensor: saved CGM state is %u bytes, want %u; discarded", (unsigned)len, (unsigned)sizeof s);
    return false;
  }
  if (s.magic != SENSOR_STATE_MAGIC) {
    logf("sensor: saved CGM state magic 0x%08x, want 0x%08x; discarded", (unsigned)s.magic,
         (unsigned)SENSOR_STATE_MAGIC);
    return false;
  }
  if (s.version != SENSOR_STATE_VERSION) {
    logf("sensor: saved CGM state version %u, want %u; discarded", (unsigned)s.version,
         (unsigned)SENSOR_STATE_VERSION);
    return false;
  }
  if (s.state.version != CGM_STATE_VERSION) {
    logf("sensor: saved state has core version %u, want %u; starting fresh", (unsigned)s.state.version,
         (unsigned)CGM_STATE_VERSION);
    return false;
  }
  *out = s.state;
  if (savedAt) *savedAt = s.saved_at;
  return true;
}

// Hands a snapshot to flash. Called with mu NOT held (the task's deferred
// save) or with it held (a panel command, which must persist immediately).
// The generation check stops an older snapshot from overwriting a newer one
// that a panel command committed while the task was on its way to flash.
// Returns whether this snapshot reached flash.
static bool saveSnapshot(const cgm_state_t* st, uint32_t gen) {
  xSemaphoreTake(saveMu, portMAX_DELAY);
  bool ok = true;
  if ((int32_t)(gen - savedGen) > 0) {
    savedGen = gen;  // attempted: never let a stale snapshot win afterwards
    ok = persist(st);
  }
  xSemaphoreGive(saveMu);
  return ok;
}

// Commits now, with mu held. Only the panel's commands use this: they run on
// the HTTP thread, where a short flash write is not holding up the radio. A
// failed write leaves dirty set, so the task retries it with a fresh snapshot
// and a new generation.
static void persistNowLocked() {
  uint32_t gen = ++saveGen;
  if (saveSnapshot(&c.st, gen)) dirty = false;  // this snapshot is the whole current state
}

// Writes the new blob, reads it back, and only then lets the caller remove the
// old state. A failed save leaves the old state exactly where it was, so the
// next boot retries the migration instead of half-migrating.
static bool persistAndVerify(const cgm_state_t* st) {
  if (!persist(st)) return false;
  cgm_state_t back;
  if (!loadBlob(&back, nullptr) || back.kind != st->kind || back.activated != st->activated ||
      memcmp(back.id, st->id, sizeof back.id) != 0) {
    logf("sensor: the saved CGM state does not read back; leaving the old state in place");
    return false;
  }
  return true;
}

// MARK: - Migration from the old g7sim-shaped sensor

// The old sensor kept its state one of two ways, depending on the build:
// Preferences namespace "sensor" (name/code/age), or JSON at /state.json on
// LittleFS (the current board). Both are read here; the new "cgm" blob is
// written first and only then are the old keys/files removed.

static cgm_kind_t kindOfName(const std::string& n) {
  return n.rfind("Dexcom", 0) == 0 && n.size() >= 8 ? CGM_G6 : CGM_G7;
}

static void idOfName(const std::string& n, cgm_kind_t kind, char id[8]) {
  id[0] = 0;
  if (kind == CGM_G6 && n.size() >= 6) {
    std::string t = n.substr(n.size() - 6);
    if (cgm_valid_g6_id(t.c_str())) {
      memcpy(id, t.c_str(), 6);
      id[6] = 0;
    }
  } else if (n.size() >= 2) {
    id[0] = n[n.size() - 2];
    id[1] = n[n.size() - 1];
    id[2] = 0;
  }
}

static bool migrateStateFile(cgm_state_t* out, int64_t* clockHint) {
  if (clockHint) *clockHint = 0;
  if (!LittleFS.begin(false)) {
    logf("sensor: LittleFS unavailable; cannot read the old sensor state");
    return false;
  }
  File f = LittleFS.open("/state.json", "r");
  if (!f) return false;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) {
    logf("sensor: old /state.json is unreadable (%s); the old sensor cannot be mapped", e.c_str());
    return false;
  }
  std::string model = d["model"] | "g7";
  std::string suffix = d["suffix"] | "";
  int64_t savedAgeMs = (int64_t)(d["savedAge"] | 0.0);
  int64_t activationMs = (int64_t)(d["activationEpoch"] | 0.0);
  int64_t warmupMs = (int64_t)(d["warmup"] | 0.0);
  int64_t sessionMs = (int64_t)(d["sessionLength"] | 0.0);
  int64_t now = nowSeconds(nullptr);

  cgm_state_fresh(out, CGM_G7, nullptr, now);
  if (suffix.size() == 2) {
    memcpy(out->id, suffix.data(), 2);
  } else {
    logf("sensor: old suffix '%s' is not two characters; keeping a random name", suffix.c_str());
  }
  out->activated = activationMs > 0 ? activationMs / 1000 : now - savedAgeMs / 1000;
  out->session_start = out->activated;
  out->warmup_done = out->activated + (warmupMs > 0 ? warmupMs / 1000 : 27 * 60);
  // If the old sensor carried a real activation date, this is its age when it
  // was saved. The caller uses it to keep the clock from starting before it.
  if (clockHint && activationMs > 0) *clockHint = activationMs / 1000 + savedAgeMs / 1000;

  if (model != "g7") {
    logf("sensor: old model %s advertised DX01/DX02; the CGM core advertises DXCM+id", model.c_str());
  }
  if (sessionMs > 0 && sessionMs / 1000 != 10 * 86400 + 12 * 3600) {
    logf("sensor: the old %.1f-day session is not in the CGM core; using its 10.5-day G7 session",
         sessionMs / 86400000.0);
  }

  int g = d["glucose"]["value"] | 120;
  out->glucose = (uint16_t)(g < 40 ? 40 : g > 400 ? 400 : g);
  out->forced_state = (uint8_t)(d["glucose"]["algorithmState"] | 0);
  std::string wave = d["glucose"]["kind"] | "sine";
  if (wave != "fixed") {
    logf("sensor: the old %s waveform is not in the CGM core; using a fixed next glucose and the recorded trend",
         wave.c_str());
  }
  if (d["calibration"].is<JsonObject>()) {
    logf("sensor: the old G7 calibration is not in the CGM core; it is dropped");
  }
  // The old pairing code and the per-display keys have no home in the core:
  // say so, rather than dropping them silently.
  std::string oldCode = d["code"] | "";
  if (!oldCode.empty()) {
    logf("sensor: the old pairing code %s is not part of the CGM core (it auto-authenticates); not carried over",
         oldCode.c_str());
  }
  JsonObject oldSlots = d["slots"].as<JsonObject>();
  if (!oldSlots.isNull() && oldSlots.size() > 0) {
    logf("sensor: the old %u display slots are not in the CGM core (it bonds every phone); not carried over",
         (unsigned)oldSlots.size());
  }

  JsonArray arr = d["readings"].as<JsonArray>();
  int count = (int)arr.size();
  int skip = count > CGM_HISTORY ? count - CGM_HISTORY : 0;
  int i = 0;
  uint32_t lastSeq = 0;
  for (JsonArray a : arr) {
    if (i++ < skip) continue;
    cgm_reading_t r = {};
    r.t = (uint32_t)(out->activated + (int64_t)(a[1] | 0));
    r.seq = (uint32_t)(a[0] | 0) + 1;
    int rg = a[2] | -1;
    r.glucose = rg >= 0 ? (uint16_t)rg : 0;
    r.state = (uint8_t)(a[3] | 0);
    if (r.glucose) {
      long tenths = lround((a[4] | 0.0) * 10.0);
      if (tenths > 126) tenths = 126;
      if (tenths < -127) tenths = -127;
      r.trend = (int8_t)tenths;
    } else {
      r.trend = 0x7f;
    }
    out->hist[out->n_hist++] = r;
    lastSeq = r.seq;
  }
  if (out->n_hist) {
    const cgm_reading_t* last = &out->hist[out->n_hist - 1];
    if (last->glucose) out->glucose = last->glucose;
    if (last->trend != 0x7f) out->trend = last->trend;
    out->next_n = lastSeq;
  } else {
    out->next_n = 0;
  }
  logf("sensor: migrated the old sensor (id %.8s, %d readings, state %u)", out->id, (int)out->n_hist,
       (unsigned)out->forced_state);
  return true;
}

static bool migratePrefs(cgm_state_t* out) {
  Preferences p;
  // Read-only: a board with no old sensor must not get an empty namespace.
  if (!p.begin("sensor", true)) return false;
  bool any = p.isKey("name") || p.isKey("code") || p.isKey("age") || p.isKey("ageMinutes");
  if (!any) {
    p.end();
    return false;
  }
  std::string name = std::string(p.getString("name", "").c_str());
  std::string code = std::string(p.getString("code", "").c_str());
  uint32_t ageMin = p.isKey("ageMinutes") ? p.getUInt("ageMinutes", 0) : p.getUInt("age", 0);
  p.end();

  cgm_kind_t kind = kindOfName(name);
  char id[8];
  idOfName(name, kind, id);
  if (kind == CGM_G6 && !id[0]) {
    logf("sensor: old name '%s' has no valid 6-character G6 transmitter id; using a random one", name.c_str());
  }
  int64_t now = nowSeconds(nullptr);
  cgm_state_fresh(out, kind, kind == CGM_G6 && id[0] ? id : nullptr, now);
  if (kind == CGM_G7 && id[0]) {
    memcpy(out->id, id, 2);
  }
  out->activated = now - (int64_t)ageMin * 60;
  out->session_start = out->activated;
  out->warmup_done = out->activated + (kind == CGM_G6 ? 2 * 3600 : 27 * 60);
  if (!code.empty()) {
    logf("sensor: old pairing code %s is not part of the CGM core (it auto-authenticates); not carried over",
         code.c_str());
  }
  logf("sensor: migrated the old 'sensor' keys (name '%s', age %u min)", name.c_str(), (unsigned)ageMin);
  return true;
}

// Removes the old state only after the new blob is committed. Each failure is
// logged; the new blob already exists, so the next boot will not re-migrate.
static void clearOldState() {
  Preferences p;
  bool any = false;
  if (p.begin("sensor", true)) {
    any = p.isKey("name") || p.isKey("code") || p.isKey("age") || p.isKey("ageMinutes");
    p.end();
  }
  if (any && p.begin("sensor", false)) {
    bool ok = p.clear();
    p.end();
    logf("sensor: cleared the old 'sensor' keys%s", ok ? "" : " (clear failed)");
  }
  if (LittleFS.begin(false)) {
    bool had = LittleFS.exists("/state.json");
    if (had && !LittleFS.remove("/state.json")) logf("sensor: could not remove the old /state.json");
    if (LittleFS.exists("/state.tmp") && !LittleFS.remove("/state.tmp")) {
      logf("sensor: could not remove the old /state.tmp");
    }
    if (had) logf("sensor: removed the old /state.json");
  }
}

enum class Migration { None, Mapped, Failed };

static Migration migrate(cgm_state_t* out, int64_t* clockHint) {
  // The JSON file is richer and is what this board actually kept, so it wins
  // over the briefed prefs layout. If it exists but cannot be read, still try
  // the prefs: a board may briefly have had both.
  if (migrateStateFile(out, clockHint)) return Migration::Mapped;
  bool fileFound = LittleFS.exists("/state.json");
  if (migratePrefs(out)) return Migration::Mapped;
  if (fileFound) {
    // Present but unmappable, and no prefs to fall back on: start fresh (the
    // caller persists the fresh state, then clears the unusable file).
    return Migration::Failed;
  }
  // A 'sensor' namespace that exists but holds none of the three keys is not
  // a sensor; treat it as absent.
  return Migration::None;
}

// MARK: - The task

static void sensorTask(void*) {
  for (;;) {
    // cgm_tick may make a reading and call io.save, and a BLE callback may
    // have marked dirty since the last pass. Take one consistent snapshot and
    // commit it outside mu, so a flash write never blocks a radio callback.
    // Each attempt takes a new generation: a failed write leaves dirty set and
    // the next pass retries with a newer snapshot (which will not be skipped by
    // the generation check).
    cgm_state_t snapshot;
    uint32_t gen = 0;
    bool flush = false;
    LOCKED({
      cgm_tick(&c);
      if (dirty) {
        snapshot = c.st;
        gen = ++saveGen;
        flush = true;
      }
    });
    if (flush && saveSnapshot(&snapshot, gen)) {
      LOCKED({
        if (saveGen == gen) dirty = false;  // nothing newer arrived meanwhile
      });
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// MARK: - Lifecycle

void begin() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  nvsReady = err == ESP_OK && nvs_open("cgm", NVS_READWRITE, &nvs) == ESP_OK;
  if (!nvsReady) logf("sensor: NVS namespace 'cgm' unavailable; the CGM will not persist");
  mu = xSemaphoreCreateMutex();
  saveMu = xSemaphoreCreateMutex();
  dirty = false;
  saveGen = savedGen = 0;

  cgm_state_t st;
  int64_t savedAt = 0;
  bool have = loadBlob(&st, &savedAt);
  int64_t wall = wallSeconds();
  clockBase = have && savedAt > 0 ? savedAt : (wall >= 1577836800 ? wall : 1700000000);
  clockBase -= monoMs() / 1000;
  bool oldFound = LittleFS.begin(false) && LittleFS.exists("/state.json");
  if (!have) {
    int64_t hint = 0;
    Migration m = migrate(&st, &hint);
    if (hint > 0 && hint > nowSeconds(nullptr)) {
      // The old sensor was activated later than this board's clock thinks it
      // is (no NTP yet, and the old state has a real date): start the clock at
      // the old sensor's age instead of reporting a future activation.
      clockBase += hint - nowSeconds(nullptr);
    }
    if (m == Migration::Mapped) {
      if (persistAndVerify(&st)) {
        clearOldState();
      } else {
        logf("sensor: migration not saved; the old sensor state is left in place");
      }
    } else {
      if (m == Migration::Failed) logf("sensor: old sensor state could not be mapped; starting fresh");
      cgm_state_fresh(&st, CGM_G7, nullptr, nowSeconds(nullptr));
      bool saved = persistAndVerify(&st);
      if (oldFound && saved) clearOldState();
    }
  } else {
    // A blob from this build is authoritative. Clear any old state that a
    // migration may have left behind (a reset between the commit and the
    // clear), so no key or file survives a second boot.
    clearOldState();
  }

  // Offer the latest reading once after every boot (the radio is fresh), so
  // the phone can find the sensor before the next 5-minute reading is due.
  st.delivered = 0;

  cgm_init(&c, &st, &io);

  char nm[16];
  cgm_name(&c.st, nm);
  int64_t now = nowSeconds(nullptr);
  logf("sensor: simulating a %s %s, id %.8s, sensor age %.0f min. TEST EQUIPMENT: readings are made up.",
       c.st.kind == CGM_G6 ? "Dexcom G6 transmitter" : c.st.kind == CGM_G7 ? "Dexcom G7 sensor" : "CGM (off)",
       nm, c.st.id, (now - c.st.activated) / 60.0);
}

void start() {
  xTaskCreatePinnedToCore(
      [](void*) {
        sensorTask(nullptr);
        vTaskDelete(nullptr);
      },
      "cgm", 8192, nullptr, 5, nullptr, tskNO_AFFINITY);
}

// MARK: - The radio's calls

void onConnect() { LOCKED({ cgm_on_connect(&c); }); }
void onDisconnect() { LOCKED({ cgm_on_disconnect(&c); }); }
void onSubscribe(cgm_chr_t chr, bool on) { LOCKED({ cgm_on_subscribe(&c, chr, on); }); }
void onWrite(cgm_chr_t chr, const uint8_t* p, size_t n) { LOCKED({ cgm_on_write(&c, chr, p, n); }); }

size_t onRead(cgm_chr_t chr, uint8_t* out, size_t cap) {
  size_t n = 0;
  LOCKED({ n = cgm_on_read(&c, chr, out, cap); });
  return n;
}

// MARK: - The panel

cgm_kind_t kind() {
  cgm_kind_t k = CGM_OFF;
  LOCKED({ k = (cgm_kind_t)c.st.kind; });
  return k;
}

std::string name() {
  char nm[16] = {0};
  LOCKED({ cgm_name(&c.st, nm); });
  return std::string(nm);
}

int64_t now() {
  int64_t t = 0;
  LOCKED({ t = nowSeconds(nullptr); });
  return t;
}

int glucose() {
  int g = 0;
  LOCKED({ g = c.st.glucose; });
  return g;
}

int trend() {
  int t = 0;
  LOCKED({ t = c.st.trend; });
  return t;
}

int forcedState() {
  int s = 0;
  LOCKED({ s = c.st.forced_state; });
  return s;
}

std::string statusJson() {
  bool radioUp = radio::cgmConnected();
  char json[1024];
  size_t n = 0;
  int64_t now = 0, activated = 0, warmupDone = 0;
  LOCKED({
    n = cgm_status_json(&c, json, sizeof json);
    now = nowSeconds(nullptr);
    activated = c.st.activated;
    warmupDone = c.st.warmup_done;
  });
  if (n == 0 || n >= sizeof json) return "{}";
  std::string s(json, n);
  if (!s.empty() && s.back() == '}') s.pop_back();
  s += ",\"radioConnected\":";
  s += radioUp ? "true" : "false";
  // The core reports clock times (Unix seconds); the panel shows the age, so
  // it does not have to trust the browser's clock against an NTP-less board.
  char extra[128];
  snprintf(extra, sizeof extra, ",\"now\":%lld,\"ageSeconds\":%lld,\"warmupLeftSeconds\":%lld}", (long long)now,
           (long long)(now - activated), (long long)(warmupDone > now ? warmupDone - now : 0));
  s += extra;
  return s;
}

// The number after "key": in a small JSON object, or false.
static bool jsonNumber(const char* j, const char* key, long* out) {
  char pat[24];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char* p = strstr(j, pat);
  if (!p) return false;
  p += strlen(pat);
  while (*p == ' ' || *p == ':') p++;
  char* end;
  long v = strtol(p, &end, 10);
  if (end == p) return false;
  *out = v;
  return true;
}

static bool jsonBool(const char* j, const char* key, bool* out) {
  char pat[24];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char* p = strstr(j, pat);
  if (!p) return false;
  p += strlen(pat);
  while (*p == ' ' || *p == ':') p++;
  if (strncmp(p, "true", 4) == 0 || *p == '1') {
    *out = true;
    return true;
  }
  if (strncmp(p, "false", 5) == 0 || *p == '0') {
    *out = false;
    return true;
  }
  return false;
}

static bool jsonStr(const char* j, const char* key, char* out, size_t cap) {
  char pat[24];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char* p = strstr(j, pat);
  if (!p) return false;
  p += strlen(pat);
  while (*p == ' ' || *p == ':') p++;
  if (*p++ != '"') return false;
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
  out[n] = 0;
  return *p == '"';
}

static bool validId(cgm_kind_t kind, const char* id) {
  if (kind == CGM_G6) return cgm_valid_g6_id(id);
  if (kind == CGM_G7) return id && strlen(id) == 2;
  return id == nullptr || id[0] == 0;
}

// {"command": "cgmSetGlucose", "glucose": 120, "trend": -10}, and so on.
// Returns nullptr, or what was wrong.
const char* command(const std::string& json) {
  const char* body = json.c_str();
  char cmd[32], id[16];
  long v;
  if (!jsonStr(body, "command", cmd, sizeof cmd)) return "want {\"command\": ...}";

  if (strcmp(cmd, "cgmSetGlucose") == 0) {
    long trend = 0;
    if (!jsonNumber(body, "glucose", &v) || v < 40 || v > 400) return "glucose is 40 to 400";
    if (jsonNumber(body, "trend", &trend) && (trend < -127 || trend > 126)) return "trend is -127 to 126";
    LOCKED({
      cgm_set_glucose(&c, (uint16_t)v, (int8_t)trend);
      persistNowLocked();  // the HTTP thread: commit now
    });
  } else if (strcmp(cmd, "cgmSetState") == 0) {
    if (!jsonNumber(body, "state", &v) || v < 0 || v > 255) return "state is 0 to 255";
    LOCKED({
      cgm_set_state(&c, (uint8_t)v);
      persistNowLocked();
    });
  } else if (strcmp(cmd, "cgmSetSignalLoss") == 0) {
    bool on;
    if (!jsonBool(body, "on", &on)) return "on is true or false";
    LOCKED({
      cgm_set_signal_loss(&c, on);
      persistNowLocked();
    });
  } else if (strcmp(cmd, "cgmSkipWarmup") == 0) {
    LOCKED({
      cgm_skip_warmup(&c);
      persistNowLocked();
    });
  } else if (strcmp(cmd, "cgmNewSensor") == 0 || strcmp(cmd, "cgmSetKind") == 0 ||
             strcmp(cmd, "cgmSetId") == 0) {
    bool haveKind = jsonNumber(body, "kind", &v);
    bool haveId = jsonStr(body, "id", id, sizeof id);
    // The core starts a new sensor now; a panel that wants an older one says
    // how many minutes old (g7sim's ageMinutes).
    long ageMinutes = 0;
    bool haveAge = jsonNumber(body, "ageMinutes", &ageMinutes) && ageMinutes > 0;
    if (haveAge && ageMinutes > 100 * 24 * 60) return "ageMinutes is 0 to 144000";
    if (!haveKind && strcmp(cmd, "cgmSetKind") == 0) return "kind is 0, 6 or 7";
    if (!haveId && strcmp(cmd, "cgmSetId") == 0) return "id is 6 characters (G6) or 2 (G7)";
    if (haveKind && v != 0 && v != 6 && v != 7) return "kind is 0, 6 or 7";
    cgm_kind_t cur;
    LOCKED({ cur = (cgm_kind_t)c.st.kind; });
    cgm_kind_t k = haveKind ? (cgm_kind_t)v : cur;
    if (k == CGM_OFF && strcmp(cmd, "cgmSetId") == 0) return "kind is off; use cgmSetKind first";
    if (haveId && !validId(k, id)) {
      return k == CGM_G6 ? "id is 6 characters, 0-9 or A-Z without I/O" : "id is 2 characters";
    }
    if (strcmp(cmd, "cgmSetKind") == 0 && !haveId && k == cur) {
      // Already that kind: nothing to reset.
    } else {
      bool setG7Id = haveId && k == CGM_G7;
      LOCKED({
        cgm_new_sensor(&c, k, haveId ? id : nullptr);
        // cgm_state_fresh() only honours id for G6; a G7 id is randomised
        // there. The core is verbatim, so put the caller's suffix back here.
        if (setG7Id) {
          memcpy(c.st.id, id, 2);
          c.st.id[2] = 0;
        }
        if (haveAge) {
          int64_t t = (int64_t)ageMinutes * 60;
          int64_t now = nowSeconds(nullptr);
          c.st.activated = now - t;
          c.st.session_start = c.st.activated;
          c.st.warmup_done = c.st.activated + (c.st.kind == CGM_G6 ? 2 * 3600 : 27 * 60);
          c.st.next_n = (uint32_t)(t / CGM_READING_INTERVAL);
        }
        persistNowLocked();  // the HTTP thread: commit the new identity now
        logf("sensor: new CGM, id %.8s (%s)", c.st.id, k == CGM_G6 ? "G6" : k == CGM_G7 ? "G7" : "off");
      });
    }
  } else if (strcmp(cmd, "cgmSetCalibration") == 0) {
    // What a G6 calibration write does on the control characteristic: the
    // reading the panel last calibrated to, and the next reading set to it.
    if (!jsonNumber(body, "glucose", &v) || v < 40 || v > 400) return "glucose is 40 to 400";
    LOCKED({
      c.st.calibration = (uint16_t)v;
      c.st.calibration_t = (uint32_t)nowSeconds(nullptr);
      c.st.glucose = (uint16_t)v;
      persistNowLocked();
    });
  } else if (strcmp(cmd, "cgmForget") == 0) {
    // The core has no pairing slots: it is authenticated-and-bonded for every
    // phone. "Forget" can only drop the current link (outside the glue's
    // mutex, since the radio's disconnect may call sensor::onDisconnect).
    bool linked = false;
    LOCKED({
      linked = c.connected;
      if (linked) cgm_on_disconnect(&c);
    });
    if (linked) radio::cgmDisconnect();
    logf("sensor: forget: dropped the link; the CGM core keeps no pairing to forget");
  } else {
    return "unknown CGM command";
  }
  return nullptr;
}

}  // namespace sensor
