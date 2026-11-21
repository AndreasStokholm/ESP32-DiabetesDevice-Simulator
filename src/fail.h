#pragma once
// One failure-injection state per device (pod, CGM, Medtrum), and the pure
// decisions the radio asks of it. No Arduino, no NimBLE, no logging: the host
// test in test/fail/ builds this with plain cc. src/radio.cpp applies the
// decisions; src/main.cpp parses the request into FailRequest and fail_apply().
//
// The five knobs are all enforced in src/radio.cpp, for all three devices:
//
//   drop     one connection is accepted, then dropped at once
//   refuse   the next N connections are accepted, then dropped
//   quiet    the device is left out of its advertising set for that time
//   delay    every outgoing notification waits that long (the CGM's too: its
//            notifications have a task of their own, so the wait never holds
//            the CGM core's mutex)
//   ignore   the next N messages from the phone are dropped, then it resumes
//
// One BLE link is shared by all three simulators, so a refused connection is
// refused for the link: the server accepts it, then disconnects immediately.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FAIL_DEVICE_COUNT 3
#define FAIL_ABSENT (-1)

typedef enum FailDevice {
  FAIL_POD = 0,
  FAIL_G7 = 1,        // kept for the panels' and the /fail API's "g7"
  FAIL_MEDTRUM = 2,
  FAIL_CGM = FAIL_G7, // the same slot: a G6 transmitter or a G7 sensor
} FailDevice;

// The questions radio.cpp asks. ACCEPT and CONSUME_MESSAGE consume one pending
// count when they answer "no"/"drop"; the other two are observations.
typedef enum FailAction {
  FAIL_ACCEPT = 0,        // 1 keep the connection, 0 accept then drop it
  FAIL_NOTIFY_DELAY_MS,   // delay before each notification, in milliseconds
  FAIL_ADVERTISE,         // 1 may advertise, 0 quiet for now
  FAIL_CONSUME_MESSAGE,   // 1 drop this message, 0 pass it on
} FailAction;

typedef struct FailState {
  uint8_t dropPending;     // drop the next connection once
  uint16_t refuseConnects; // drop this many subsequent connections
  uint16_t ignoreMessages; // drop this many messages
  uint16_t delayMs;        // delay each outgoing notification
  int64_t quietUntilMs;    // do not advertise before this monotonic time
} FailState;

// -1 means "leave it alone"; 0 clears the field. drop uses 0/1 for clear/set.
typedef struct FailRequest {
  int drop;
  int refuse;
  int quietMs;
  int delayMs;
  int ignore;
} FailRequest;

// The one decision radio.cpp uses. Out-of-range devices are harmless: the
// connection and advertising decisions default to "allow", the others to 0.
long long fail_decide(FailDevice device, FailAction action, int64_t nowMs);

void fail_clear_all(void);
void fail_clear(FailDevice device);
FailState fail_get(FailDevice device);
void fail_set_drop(FailDevice device, int on);
void fail_set_refuse(FailDevice device, unsigned connects);
void fail_set_quiet_for(FailDevice device, unsigned ms, int64_t nowMs);
void fail_set_delay(FailDevice device, unsigned ms);
void fail_set_ignore(FailDevice device, unsigned messages);

// The request-parser decision: validate every present field first, then apply
// them all to one device. Returns NULL on success, or a static error string.
// Out-of-range values leave the state untouched.
const char* fail_apply(FailDevice device, const FailRequest* request, int64_t nowMs);

// "pod", "g7" (or "cgm") and "medtrum" -> 0..2; anything else (including
// "all", which the request parser expands itself) -> -1.
int fail_device_from_name(const char* name);
const char* fail_device_name(FailDevice device);

#ifdef __cplusplus
}
#endif
