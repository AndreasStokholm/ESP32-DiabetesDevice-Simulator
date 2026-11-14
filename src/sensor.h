#pragma once
// The emulated sensor (port of g7/sensor.go, state.go, glucose.go,
// messages.go, advert.go).
//
// Time: everything runs on the *sensor clock*, milliseconds since activation.
// The phone only ever sees sensor-relative times (the 4e reply carries the
// sensor's age and each reading's), so no wall clock is needed. The sensor
// clock survives a reboot from the state file; when Wi-Fi + NTP is available
// it is re-anchored to the wall clock so power-off time is not lost.

#include <ArduinoJson.h>

#include <deque>
#include <map>
#include <mutex>

#include "util.h"

constexpr int64_t SEC = 1000;
constexpr int64_t MINUTE = 60 * SEC;
constexpr int64_t HOUR = 60 * MINUTE;

// Algorithm states the 4e and backfill records carry.
constexpr uint8_t algoWarmup = 2;
constexpr uint8_t algoOK = 6;
constexpr uint8_t algoExpired = 24;

constexpr uint8_t DisplayMedical = 1, DisplayPhone = 2, DisplayWatch = 3;

constexpr int64_t GracePeriod = 12 * HOUR;
constexpr size_t keptReadings = 24 * 12;

enum class AdvertMode { Off, Full, Bare };
const char* advertModeName(AdvertMode m);

enum class LinkAnswer { Accepted, Unanswered, FailedToEstablish };
const char* linkAnswerName(LinkAnswer a);

struct Timing {
  int64_t readingInterval = 300 * SEC;
  int64_t burstDelay = 2500;
  int64_t servedBurst = 7 * SEC;
  int64_t extendedBurst = 26 * SEC;
  int64_t lateBurst = 8 * SEC;
  int64_t emptyBurst = 25 * SEC;
  int64_t extendedTailAfterClose = 19 * SEC;
  int64_t servedTailAfterClose = 1 * SEC;
  int extendedWindows = 2;
  int64_t minuteCallLength = 3 * SEC;
  int64_t advertisingInterval = 160;  // ms
  int64_t slotHold = 15 * MINUTE;
  int rejectionsToLock = 4;
  int64_t rejectionLockout = 10 * MINUTE;
};

struct GlucoseMode {
  std::string kind = "sine";  // sine, fixed or walk
  double value = 120, amplitude = 50, periodMinutes = 240;
  uint8_t algorithmState = 0;  // when set, replaces the computed state
};

struct Reading {
  int32_t index = 0;
  uint32_t time = 0;  // sensor seconds
  bool hasGlucose = false;
  uint16_t glucose = 0;
  uint8_t state = 0;
  bool hasTrend = false;
  double trend = 0;  // mg/dL/min
  bool displayOnly = false;
};

struct Slot {
  Bytes key;
  int64_t lastSeen = 0;  // sensor ms
  int misses = 0;
};

struct Calibration {
  uint16_t glucose = 0;
  uint32_t sensorTime = 0;
  uint8_t displayType = 0;
};

struct State {
  uint64_t serial = 0;
  std::string code, model = "g7", suffix;
  int days = 10;
  int64_t sessionLength = 0, warmup = 0;  // ms
  uint32_t algorithm = 0;
  uint16_t maxLifetimeDays = 0;
  uint8_t leafKey[32] = {};

  int64_t savedAge = 0;         // the sensor clock when last saved
  int64_t activationEpoch = 0;  // wall ms of activation, once known (NTP)

  std::map<uint8_t, Slot> slots;
  bool hasCalibration = false;
  Calibration calibration;
  double calOffset = 0;
  GlucoseMode glucose;
  std::deque<Reading> readings;

  int64_t lifetime() const { return sessionLength - GracePeriod; }
  void applyPreset(int days);
};

// What a new sensor should look like; negative values pick defaults.
struct SensorConfig {
  std::string model, code;
  int days = 0;
  uint64_t serial = 0;
  double ageMinutes = -1, lifetimeHours = -1, warmupMinutes = -1;
};
// Builds a new sensor's state, or returns false with a reason.
bool newState(SensorConfig c, State& out, std::string& err);

bool loadState(State& s);
bool saveState(const State& s);

struct AuthVerdict {
  uint8_t status, detail;
};

class Sensor {
 public:
  Timing timing;

  explicit Sensor(State st);

  // The sensor clock, ms since activation.
  int64_t now();
  // Re-anchor the sensor clock once the wall clock is known.
  void syncWallClock(int64_t wall);

  AdvertMode tick();
  // The index of the latest reading (-1 before the first).
  int latestIndex();
  uint8_t typesInUse();

  LinkAnswer linkOpened(const std::string& addr, AdvertMode mode);
  void linkClosed();
  bool admitDisplay(const std::string& addr, uint8_t type);
  Bytes slotKey(uint8_t type);
  AuthVerdict authenticated(const std::string& addr, uint8_t type, const Bytes& newKey);
  AuthVerdict rejectedMismatch();
  void served(const std::string& addr, uint8_t type);

  // The 4e reply for the latest reading, or empty before activation.
  Bytes glucoseMessage();
  std::vector<Reading> backfill(uint32_t start, uint32_t end);
  void calibrate(uint16_t glucose, uint32_t at, uint8_t type);
  Bytes calibrationBounds();
  Bytes extendedVersion();
  Bytes transmitterVersion();

  std::string name();
  std::string model();
  uint64_t serial();
  std::string pairingCode();
  uint16_t serialChecksum();

  void status(JsonDocument& doc);
  void setGlucoseMode(const GlucoseMode& m);
  void forgetSlots();
  void retire();  // stop saving, before a replacement is written
  void persist();

 private:
  std::mutex mu;
  State st;
  int64_t clockOffset;  // sensor ms = monoMs() + clockOffset
  bool retired = false;

  struct Window {
    int index = -1;
    int64_t start = 0;
    std::map<uint8_t, bool> servedTypes;
    std::map<std::string, bool> servedAddrs;
    bool anyLink = false;
    int openLinks = 0;
    int64_t lastClose = 0;
  };
  bool haveWin = false;
  Window win;
  int rejections = 0;
  int64_t lockedUntil = -1;

  int64_t nowLocked() { return monoMs() + clockOffset; }
  int readingIndex(int64_t t);
  bool fillReadings(int64_t t);
  Reading makeReading(int i);
  double raw(int i);
  void closeWindow(int64_t now);
  bool isHeld(const Slot& s, int64_t now);
  int64_t burstEnd(int64_t now);
  bool displayMissing();
  AuthVerdict reject(AuthVerdict v, int64_t now);
  void save();
  const Reading* latest() const { return st.readings.empty() ? nullptr : &st.readings.back(); }
};

// Wire encoders.
Bytes backfillRecord(const Reading& r);
