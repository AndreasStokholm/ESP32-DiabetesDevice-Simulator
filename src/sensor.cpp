#include "sensor.h"

#include <LittleFS.h>
#include <esp_random.h>
#include <math.h>

#include "crypto.h"

static const char* statePath = "/state.json";
static const char* tmpPath = "/state.tmp";

const char* advertModeName(AdvertMode m) {
  switch (m) {
    case AdvertMode::Full: return "full";
    case AdvertMode::Bare: return "bare";
    default: return "off";
  }
}

const char* linkAnswerName(LinkAnswer a) {
  switch (a) {
    case LinkAnswer::Accepted: return "accepted";
    case LinkAnswer::Unanswered: return "unanswered";
    default: return "failed to establish";
  }
}

// Models are the G7-family sensors and the name prefix each advertises.
static const char* modelPrefix(const std::string& m) {
  if (m == "oneplus") return "DX02";
  if (m == "stelo") return "DX01";
  if (m == "g7") return "DXCM";
  return nullptr;
}

// Presets are from captured 52 replies:
//   10-day: 52 00 c0d70d00 5406 00020404 ff 0c00
//   15-day: 52 00 406f1400 880e 00010a04 ff 1100
void State::applyPreset(int d) {
  if (d != 15) d = 10;
  days = d;
  if (d == 10) {
    sessionLength = 907200 * SEC, warmup = 1620 * SEC, algorithm = 0x04040200, maxLifetimeDays = 12;
  } else {
    sessionLength = 1339200 * SEC, warmup = 3720 * SEC, algorithm = 0x040a0100, maxLifetimeDays = 17;
  }
}

static uint32_t randomBelow(uint32_t n) { return esp_random() % n; }

bool newState(SensorConfig c, State& s, std::string& err) {
  if (c.model.empty()) c.model = "g7";
  if (!modelPrefix(c.model)) {
    err = "model " + c.model + ": want g7, oneplus or stelo";
    return false;
  }
  if (c.days == 0) c.days = c.model == "stelo" ? 15 : 10;
  if (c.days != 10 && c.days != 15) {
    err = "days: want 10 or 15";
    return false;
  }
  if (c.code.empty()) {
    char b[5];
    snprintf(b, sizeof b, "%04u", (unsigned)randomBelow(10000));
    c.code = b;
  }
  if (c.code.size() != 4 || c.code.find_first_not_of("0123456789") != std::string::npos) {
    err = "code " + c.code + ": want 4 digits";
    return false;
  }
  if (c.serial == 0) c.serial = 3000000000ULL + randomBelow(999999999);
  int64_t age = c.ageMinutes < 0 ? 2 * HOUR : (int64_t)(c.ageMinutes * MINUTE);

  s = State();
  s.serial = c.serial;
  s.code = c.code;
  s.model = c.model;
  // The two-letter name suffix comes from the serial, so a given serial
  // always presents as the same sensor (name and BLE address).
  s.suffix = {(char)('a' + (c.serial / 26) % 26), (char)('a' + c.serial % 26)};
  s.applyPreset(c.days);
  if (c.lifetimeHours >= 0) {
    if (c.lifetimeHours <= 0) {
      err = "lifetime must be positive";
      return false;
    }
    s.sessionLength = (int64_t)(c.lifetimeHours * HOUR) + GracePeriod;
  }
  if (c.warmupMinutes >= 0) s.warmup = (int64_t)(c.warmupMinutes * MINUTE);
  // The 52 reply carries warmup as 16-bit seconds.
  if (s.warmup < 0 || s.warmup > 0xffff * SEC) {
    err = "warmup: want 0 to 1092 minutes";
    return false;
  }
  if (s.warmup >= s.lifetime()) {
    err = "warmup must be shorter than the lifetime";
    return false;
  }
  s.savedAge = age;
  if (int64_t w = wallMs()) s.activationEpoch = w - age;
  identity::newLeafKey(s.leafKey);
  return true;
}

// MARK: - Persistence (a JSON file on LittleFS, like g7state.json)

static std::string toHex(const uint8_t* p, size_t n) { return hexstr(p, n); }
static Bytes fromHex(const char* s) {
  Bytes b;
  if (!s) return b;
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
    char t[3] = {s[i], s[i + 1], 0};
    b.push_back(strtoul(t, nullptr, 16));
  }
  return b;
}

bool saveState(const State& s) {
  JsonDocument d;
  d["serial"] = std::to_string(s.serial);
  d["code"] = s.code;
  d["model"] = s.model;
  d["suffix"] = s.suffix;
  d["days"] = s.days;
  d["sessionLength"] = (double)s.sessionLength;
  d["warmup"] = (double)s.warmup;
  d["algorithm"] = s.algorithm;
  d["maxLifetimeDays"] = s.maxLifetimeDays;
  d["leafKey"] = toHex(s.leafKey, 32);
  d["savedAge"] = (double)s.savedAge;
  d["activationEpoch"] = (double)s.activationEpoch;
  JsonObject slots = d["slots"].to<JsonObject>();
  for (auto& [t, sl] : s.slots) {
    JsonObject o = slots[std::to_string(t)].to<JsonObject>();
    o["key"] = toHex(sl.key.data(), sl.key.size());
    o["lastSeen"] = (double)sl.lastSeen;
    o["misses"] = sl.misses;
  }
  if (s.hasCalibration) {
    JsonObject c = d["calibration"].to<JsonObject>();
    c["glucose"] = s.calibration.glucose;
    c["sensorTime"] = s.calibration.sensorTime;
    c["displayType"] = s.calibration.displayType;
  }
  d["calOffset"] = s.calOffset;
  JsonObject g = d["glucose"].to<JsonObject>();
  g["kind"] = s.glucose.kind;
  g["value"] = s.glucose.value;
  g["amplitude"] = s.glucose.amplitude;
  g["periodMinutes"] = s.glucose.periodMinutes;
  g["algorithmState"] = s.glucose.algorithmState;
  // Readings, compactly: [index, time, glucose|-1, state, trend|null, displayOnly]
  JsonArray rs = d["readings"].to<JsonArray>();
  for (auto& r : s.readings) {
    JsonArray a = rs.add<JsonArray>();
    a.add(r.index);
    a.add(r.time);
    a.add(r.hasGlucose ? (int)r.glucose : -1);
    a.add(r.state);
    if (r.hasTrend) a.add(r.trend); else a.add(nullptr);
    a.add(r.displayOnly ? 1 : 0);
  }

  File f = LittleFS.open(tmpPath, "w");
  if (!f) {
    logf("saving state: cannot open %s", tmpPath);
    return false;
  }
  size_t n = serializeJson(d, f);
  f.close();
  if (n == 0 || !LittleFS.rename(tmpPath, statePath)) {
    logf("saving state: write failed");
    return false;
  }
  return true;
}

bool loadState(State& s) {
  File f = LittleFS.open(statePath, "r");
  if (!f) return false;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) {
    logf("state file unreadable: %s", e.c_str());
    return false;
  }
  s = State();
  s.serial = strtoull(d["serial"] | "0", nullptr, 10);
  s.code = d["code"] | "";
  s.model = d["model"] | "g7";
  s.suffix = d["suffix"] | "aa";
  s.days = d["days"] | 10;
  s.sessionLength = (int64_t)(d["sessionLength"] | 0.0);
  s.warmup = (int64_t)(d["warmup"] | 0.0);
  s.algorithm = d["algorithm"] | 0;
  s.maxLifetimeDays = d["maxLifetimeDays"] | 0;
  Bytes lk = fromHex(d["leafKey"] | "");
  if (lk.size() != 32 || s.serial == 0 || s.code.size() != 4) {
    logf("state file incomplete");
    return false;
  }
  memcpy(s.leafKey, lk.data(), 32);
  s.savedAge = (int64_t)(d["savedAge"] | 0.0);
  s.activationEpoch = (int64_t)(d["activationEpoch"] | 0.0);
  for (JsonPair p : d["slots"].as<JsonObject>()) {
    Slot sl;
    sl.key = fromHex(p.value()["key"] | "");
    sl.lastSeen = (int64_t)(p.value()["lastSeen"] | 0.0);
    sl.misses = p.value()["misses"] | 0;
    s.slots[(uint8_t)atoi(p.key().c_str())] = sl;
  }
  if (d["calibration"].is<JsonObject>()) {
    s.hasCalibration = true;
    s.calibration.glucose = d["calibration"]["glucose"] | 0;
    s.calibration.sensorTime = d["calibration"]["sensorTime"] | 0;
    s.calibration.displayType = d["calibration"]["displayType"] | 0;
  }
  s.calOffset = d["calOffset"] | 0.0;
  s.glucose.kind = d["glucose"]["kind"] | "sine";
  s.glucose.value = d["glucose"]["value"] | 120.0;
  s.glucose.amplitude = d["glucose"]["amplitude"] | 50.0;
  s.glucose.periodMinutes = d["glucose"]["periodMinutes"] | 240.0;
  s.glucose.algorithmState = d["glucose"]["algorithmState"] | 0;
  for (JsonArray a : d["readings"].as<JsonArray>()) {
    Reading r;
    r.index = a[0];
    r.time = a[1];
    int g = a[2];
    r.hasGlucose = g >= 0;
    r.glucose = r.hasGlucose ? g : 0;
    r.state = a[3];
    r.hasTrend = !a[4].isNull();
    r.trend = a[4] | 0.0;
    r.displayOnly = (a[5] | 0) != 0;
    s.readings.push_back(r);
  }
  if (s.sessionLength == 0 || s.algorithm == 0) {
    int64_t sl = s.sessionLength, w = s.warmup;
    s.applyPreset(s.days);
    if (sl) s.sessionLength = sl, s.warmup = w;
  }
  return true;
}

// MARK: - Wire encoders (messages.go)

static uint8_t trendByte(const Reading& r) {
  if (!r.hasTrend) return 0x7f;
  long t = lround(r.trend * 10);
  if (t > 126) t = 126;
  if (t < -127) t = -127;
  return (uint8_t)(int8_t)t;
}

static uint16_t glucoseWord(const Reading& r) { return r.hasGlucose ? (r.glucose & 0xfff) : 0xffff; }

static uint8_t flags(const Reading& r) { return 0x0f | (r.displayOnly ? 0x10 : 0); }

// 4e: the latest reading and how old it is.
static Bytes glucoseMessage(const Reading& r, uint32_t sensorNow) {
  Bytes b = {0x4e, 0x00};
  le32(b, sensorNow);
  le16(b, (uint16_t)r.index);
  b.push_back(0x00);
  b.push_back(0x01);
  le16(b, (uint16_t)(sensorNow - r.time));
  le16(b, glucoseWord(r));
  b.push_back(r.state);
  b.push_back(trendByte(r));
  le16(b, r.hasGlucose ? r.glucose : 0xffff);
  b.push_back(flags(r));
  return b;
}

// One 9-byte record on the backfill characteristic.
Bytes backfillRecord(const Reading& r) {
  Bytes b = {(uint8_t)r.time, (uint8_t)(r.time >> 8), (uint8_t)(r.time >> 16), 0x00};
  le16(b, glucoseWord(r));
  b.push_back(r.state);
  b.push_back(flags(r));
  b.push_back(trendByte(r));
  return b;
}

// MARK: - Sensor

Sensor::Sensor(State s) : st(std::move(s)) {
  clockOffset = st.savedAge - monoMs();
  std::lock_guard<std::mutex> l(mu);
  fillReadings(nowLocked());
}

int64_t Sensor::now() {
  std::lock_guard<std::mutex> l(mu);
  return nowLocked();
}

void Sensor::syncWallClock(int64_t wall) {
  std::lock_guard<std::mutex> l(mu);
  int64_t age = nowLocked();
  if (st.activationEpoch == 0) {
    st.activationEpoch = wall - age;
    logf("wall clock set; sensor activation anchored");
    save();
    return;
  }
  int64_t trueAge = wall - st.activationEpoch;
  if (trueAge > age + 2 * SEC) {
    clockOffset += trueAge - age;
    logf("wall clock set; sensor clock advanced %lld s for time spent powered off", (trueAge - age) / SEC);
  } else if (trueAge < age - 2 * SEC) {
    // Never run the sensor clock backwards; re-anchor activation instead.
    st.activationEpoch = wall - age;
    logf("wall clock set; activation re-anchored");
  }
  save();
}

int Sensor::readingIndex(int64_t t) { return t < 0 ? -1 : (int)(t / timing.readingInterval); }

static double normFloat() {
  double u1 = (esp_random() + 1.0) / 4294967297.0;
  double u2 = esp_random() / 4294967296.0;
  return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

double Sensor::raw(int i) {
  const GlucoseMode& m = st.glucose;
  if (m.kind == "fixed") return m.value;
  if (m.kind == "walk") {
    double prev = m.value;
    if (auto r = latest(); r && r->hasGlucose) prev = r->glucose - st.calOffset;
    return prev + (m.value - prev) * 0.05 + normFloat() * 2;
  }
  double period = m.periodMinutes > 0 ? m.periodMinutes : 240;
  double minutes = (double)i * timing.readingInterval / MINUTE;
  return m.value + m.amplitude * sin(2 * M_PI * minutes / period);
}

Reading Sensor::makeReading(int i) {
  Reading r;
  r.index = i;
  r.time = (uint32_t)((int64_t)i * timing.readingInterval / SEC);
  int64_t age = (int64_t)r.time * SEC;
  if (age < st.warmup) {
    r.state = algoWarmup;
    return r;
  }
  if (age > st.sessionLength) {
    r.state = algoExpired;
    return r;
  }
  r.state = algoOK;
  if (uint8_t s = st.glucose.algorithmState) {
    r.state = s;
    if (s != algoOK) return r;
  }
  double v = std::max(40.0, std::min(400.0, raw(i) + st.calOffset));
  r.hasGlucose = true;
  r.glucose = (uint16_t)lround(v);
  if (auto prev = latest(); prev && prev->hasGlucose && prev->index == i - 1) {
    double trend = (v - prev->glucose) / ((double)timing.readingInterval / MINUTE);
    r.hasTrend = true;
    r.trend = round(trend * 10) / 10;
  }
  return r;
}

bool Sensor::fillReadings(int64_t t) {
  int last = readingIndex(t);
  int next = st.readings.empty() ? 0 : st.readings.back().index + 1;
  // A long gap (e.g. the clock jumped forward) only needs the last day.
  if (last - next >= (int)keptReadings) next = last - keptReadings + 1;
  bool added = false;
  for (int i = next; i <= last; i++) {
    st.readings.push_back(makeReading(i));
    while (st.readings.size() > keptReadings) st.readings.pop_front();
    added = true;
  }
  return added;
}

static std::string describe(const Reading& r) {
  char g[8] = "---", t[12] = "-";
  if (r.hasGlucose) snprintf(g, sizeof g, "%u", r.glucose);
  if (r.hasTrend) snprintf(t, sizeof t, "%+.1f", r.trend);
  char out[96];
  snprintf(out, sizeof out, "%s mg/dL trend %s state %u at sensor %us", g, t, r.state, r.time);
  return out;
}

AdvertMode Sensor::tick() {
  std::lock_guard<std::mutex> l(mu);
  int64_t now = nowLocked();

  if (fillReadings(now)) {
    logf("reading %d: %s", latest()->index, describe(*latest()).c_str());
    save();
  }

  int idx = readingIndex(now);
  if (idx < 0) return AdvertMode::Off;
  if (!haveWin || win.index != idx) {
    closeWindow(now);
    win = Window();
    win.index = idx;
    win.start = (int64_t)idx * timing.readingInterval + timing.burstDelay;
    haveWin = true;
  }
  if (now < lockedUntil) return AdvertMode::Off;

  // A sensor that has never paired stays fully discoverable, so a phone can
  // find it to pair at any time. It settles into the measured grid once a
  // phone has connected.
  if (!st.slots.count(DisplayPhone)) return AdvertMode::Full;

  if (now < win.start) return AdvertMode::Off;
  if (now < burstEnd(now)) return AdvertMode::Full;
  if (displayMissing()) {
    int64_t offset = now - win.start;
    for (int m = 1; m <= 4; m++) {
      int64_t call = m * MINUTE;
      if (offset >= call && offset < call + timing.minuteCallLength) return AdvertMode::Bare;
    }
  }
  return AdvertMode::Off;
}

int Sensor::latestIndex() {
  std::lock_guard<std::mutex> l(mu);
  return latest() ? latest()->index : -1;
}

void Sensor::closeWindow(int64_t now) {
  if (!haveWin) return;
  for (auto& [t, slot] : st.slots) {
    if (win.servedTypes.count(t)) {
      slot.misses = 0;
    } else if (isHeld(slot, now)) {
      slot.misses++;
      logf("window %d: display type %u did not collect (%d in a row)", win.index, t, slot.misses);
    }
  }
  save();
}

bool Sensor::isHeld(const Slot& s, int64_t now) { return now - s.lastSeen < timing.slotHold; }

int64_t Sensor::burstEnd(int64_t now) {
  const Timing& t = timing;
  if (!win.anyLink) return win.start + t.emptyBurst;
  int64_t lastClose = win.openLinks > 0 ? now : win.lastClose;
  bool missing = false, extended = false;
  for (auto& [typ, slot] : st.slots) {
    if (isHeld(slot, now) && !win.servedTypes.count(typ)) {
      missing = true;
      if (slot.misses < t.extendedWindows) extended = true;
    }
  }
  auto end = [&](int64_t burst, int64_t tail) { return std::max(win.start + burst, lastClose + tail); };
  if (!missing) return end(t.servedBurst, t.servedTailAfterClose);
  if (extended) return end(t.extendedBurst, t.extendedTailAfterClose);
  return end(t.lateBurst, t.servedTailAfterClose);
}

bool Sensor::displayMissing() {
  for (auto& [t, slot] : st.slots)
    if (!win.servedTypes.count(t)) return true;
  return false;
}

uint8_t Sensor::typesInUse() {
  std::lock_guard<std::mutex> l(mu);
  int64_t now = nowLocked();
  uint8_t b = 0;
  for (auto& [t, slot] : st.slots)
    if (t >= 1 && t <= 8 && isHeld(slot, now)) b |= 1 << (t - 1);
  return b;
}

LinkAnswer Sensor::linkOpened(const std::string& addr, AdvertMode mode) {
  std::lock_guard<std::mutex> l(mu);
  if (!haveWin) return LinkAnswer::Unanswered;
  if (win.servedAddrs.count(addr)) {
    if (mode == AdvertMode::Bare) {
      logf("%s: already served reading %d; not answering at a minute call", addr.c_str(), win.index);
      return LinkAnswer::Unanswered;
    }
    logf("%s: already served reading %d; the link fails to establish", addr.c_str(), win.index);
    return LinkAnswer::FailedToEstablish;
  }
  win.anyLink = true;
  win.openLinks++;
  return LinkAnswer::Accepted;
}

void Sensor::linkClosed() {
  std::lock_guard<std::mutex> l(mu);
  if (!haveWin) return;
  if (win.openLinks > 0) win.openLinks--;
  win.lastClose = nowLocked();
}

bool Sensor::admitDisplay(const std::string& addr, uint8_t type) {
  std::lock_guard<std::mutex> l(mu);
  if (haveWin && win.servedTypes.count(type)) {
    logf("%s: display type %u already collected reading %d; dropping", addr.c_str(), type, win.index);
    return false;
  }
  return true;
}

Bytes Sensor::slotKey(uint8_t type) {
  std::lock_guard<std::mutex> l(mu);
  auto it = st.slots.find(type);
  return it == st.slots.end() ? Bytes() : it->second.key;
}

static const AuthVerdict verdictBonded{0x01, 0x01}, verdictPaired{0x01, 0x00}, verdictMismatch{0x02, 0x01},
    verdictNoAppKey{0x02, 0x03};

AuthVerdict Sensor::authenticated(const std::string& addr, uint8_t type, const Bytes& newKey) {
  std::lock_guard<std::mutex> l(mu);
  int64_t now = nowLocked();
  if (!newKey.empty()) {
    // A completed key exchange means the client has the pairing code, so it
    // takes the slot, replacing any earlier key for this display type.
    Slot s;
    s.key = newKey;
    s.lastSeen = now;
    st.slots[type] = s;
    rejections = 0;
    save();
    return verdictPaired;
  }
  auto it = st.slots.find(type);
  if (it == st.slots.end()) return reject(verdictNoAppKey, now);
  it->second.lastSeen = now;
  rejections = 0;
  return verdictBonded;
}

AuthVerdict Sensor::rejectedMismatch() {
  std::lock_guard<std::mutex> l(mu);
  return reject(verdictMismatch, nowLocked());
}

AuthVerdict Sensor::reject(AuthVerdict v, int64_t now) {
  if (++rejections >= timing.rejectionsToLock) {
    lockedUntil = now + timing.rejectionLockout;
    rejections = 0;
    logf("%d rejections in a row: not accepting connections for %lld min", timing.rejectionsToLock,
         timing.rejectionLockout / MINUTE);
  }
  return v;
}

void Sensor::served(const std::string& addr, uint8_t type) {
  std::lock_guard<std::mutex> l(mu);
  if (!haveWin) return;
  win.servedAddrs[addr] = true;
  win.servedTypes[type] = true;
  auto it = st.slots.find(type);
  if (it != st.slots.end()) {
    it->second.lastSeen = nowLocked();
    it->second.misses = 0;
  }
}

Bytes Sensor::glucoseMessage() {
  std::lock_guard<std::mutex> l(mu);
  int64_t now = nowLocked();
  fillReadings(now);
  if (!latest()) return {};
  return ::glucoseMessage(*latest(), (uint32_t)(now / SEC));
}

std::vector<Reading> Sensor::backfill(uint32_t start, uint32_t end) {
  std::lock_guard<std::mutex> l(mu);
  std::vector<Reading> out;
  for (auto& r : st.readings)
    if (r.time >= start && r.time <= end) out.push_back(r);
  return out;
}

void Sensor::calibrate(uint16_t glucose, uint32_t at, uint8_t type) {
  std::lock_guard<std::mutex> l(mu);
  st.hasCalibration = true;
  st.calibration = {glucose, at, type};
  // Shift later readings so the one nearest `at` would have read glucose.
  const Reading* nearest = nullptr;
  auto diff = [](uint32_t a, uint32_t b) { return a > b ? a - b : b - a; };
  for (auto& r : st.readings)
    if (r.hasGlucose && (!nearest || diff(r.time, at) < diff(nearest->time, at))) nearest = &r;
  if (nearest) st.calOffset += (double)glucose - nearest->glucose;
  save();
}

Bytes Sensor::calibrationBounds() {
  std::lock_guard<std::mutex> l(mu);
  Bytes b = {0x32, 0x00, 0x01};  // session number 1
  le32(b, 0);
  uint16_t glucose = 0;
  uint32_t at = 0;
  uint8_t status = 1, display = 0;
  if (st.hasCalibration) {
    glucose = st.calibration.glucose, at = st.calibration.sensorTime, status = 3;
    display = st.calibration.displayType;
  }
  le16(b, glucose);
  le32(b, at);
  b.push_back(status);
  b.push_back(0x01);
  b.push_back(display);
  le32(b, at);
  return b;
}

Bytes Sensor::extendedVersion() {
  std::lock_guard<std::mutex> l(mu);
  Bytes b = {0x52, 0x00};
  le32(b, (uint32_t)(st.sessionLength / SEC));
  le16(b, (uint16_t)(st.warmup / SEC));
  le32(b, st.algorithm);
  b.push_back(0xff);  // hardware version
  le16(b, st.maxLifetimeDays);
  return b;
}

Bytes Sensor::transmitterVersion() {
  std::lock_guard<std::mutex> l(mu);
  Bytes b = {0x4a, 0x00, 0x2c, 0xc0, 0x69, 0x48};
  le32(b, 0x0000379c);
  le32(b, 0x41414731);
  for (int i = 0; i < 6; i++) b.push_back(st.serial >> (8 * i));
  return b;
}

std::string Sensor::model() {
  std::lock_guard<std::mutex> l(mu);
  return modelPrefix(st.model) ? st.model : "g7";
}

std::string Sensor::name() {
  std::lock_guard<std::mutex> l(mu);
  const char* p = modelPrefix(st.model);
  return std::string(p ? p : "DXCM") + st.suffix;
}

uint64_t Sensor::serial() {
  std::lock_guard<std::mutex> l(mu);
  return st.serial;
}

std::string Sensor::pairingCode() {
  std::lock_guard<std::mutex> l(mu);
  return st.code;
}

// CRC-16/XMODEM of the serial's ASCII digits, which the advertisement carries
// so a scanner can match the package it scanned.
uint16_t Sensor::serialChecksum() {
  std::string s = std::to_string(serial());
  uint16_t crc = 0;
  for (char c : s) {
    crc ^= (uint16_t)(uint8_t)c << 8;
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

static std::string iso(int64_t wall) {
  time_t t = wall / 1000;
  struct tm tm;
  gmtime_r(&t, &tm);
  char b[32];
  strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return b;
}

void Sensor::status(JsonDocument& d) {
  std::string nm = name(), mdl = model();
  std::lock_guard<std::mutex> l(mu);
  int64_t now = nowLocked();
  d["name"] = nm;
  d["serial"] = st.serial;
  d["code"] = st.code;
  d["model"] = mdl;
  d["days"] = st.days;
  d["lifetimeHours"] = (double)st.lifetime() / HOUR;
  d["warmupMinutes"] = (double)st.warmup / MINUTE;
  d["ageSeconds"] = now / SEC;
  if (st.activationEpoch) d["activation"] = iso(st.activationEpoch);
  JsonObject slots = d["slots"].to<JsonObject>();
  for (auto& [t, sl] : st.slots) {
    JsonObject o = slots[std::to_string(t)].to<JsonObject>();
    o["lastSeenAgoSeconds"] = (now - sl.lastSeen) / SEC;
    o["held"] = isHeld(sl, now);
    o["misses"] = sl.misses;
  }
  JsonObject g = d["glucoseMode"].to<JsonObject>();
  g["kind"] = st.glucose.kind;
  g["value"] = st.glucose.value;
  g["amplitude"] = st.glucose.amplitude;
  g["periodMinutes"] = st.glucose.periodMinutes;
  if (st.glucose.algorithmState) g["algorithmState"] = st.glucose.algorithmState;
  if (auto r = latest()) {
    JsonObject o = d["latest"].to<JsonObject>();
    o["index"] = r->index;
    o["time"] = r->time;
    if (r->hasGlucose) o["glucose"] = r->glucose;
    o["state"] = r->state;
    if (r->hasTrend) o["trend"] = r->trend;
  }
  if (haveWin) {
    JsonObject w = d["window"].to<JsonObject>();
    w["index"] = win.index;
    w["openLinks"] = win.openLinks;
    JsonArray served = w["servedTypes"].to<JsonArray>();
    for (auto& [t, _] : win.servedTypes) served.add(t);
  }
  if (now < lockedUntil) d["lockedForSeconds"] = (lockedUntil - now) / SEC;
}

void Sensor::setGlucoseMode(const GlucoseMode& m) {
  std::lock_guard<std::mutex> l(mu);
  st.glucose = m;
  save();
}

void Sensor::forgetSlots() {
  std::lock_guard<std::mutex> l(mu);
  st.slots.clear();
  save();
}

void Sensor::retire() {
  std::lock_guard<std::mutex> l(mu);
  retired = true;
}

void Sensor::persist() {
  std::lock_guard<std::mutex> l(mu);
  save();
}

void Sensor::save() {
  if (retired) return;
  st.savedAge = nowLocked();
  saveState(st);
}
