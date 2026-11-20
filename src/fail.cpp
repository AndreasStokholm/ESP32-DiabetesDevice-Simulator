#include "fail.h"

#include <string.h>

// The state is touched by three task contexts (the web/console loop, the
// NimBLE host task and the pod/Medtrum tasks). A tiny critical section keeps
// decrements and the 64-bit quiet deadline from tearing. The host test has no
// FreeRTOS, so the lock is a no-op there.
#if defined(ESP_PLATFORM) || defined(ARDUINO)
#include <freertos/FreeRTOS.h>
static portMUX_TYPE failMux = portMUX_INITIALIZER_UNLOCKED;
#define FAIL_LOCK() portENTER_CRITICAL(&failMux)
#define FAIL_UNLOCK() portEXIT_CRITICAL(&failMux)
#else
#define FAIL_LOCK() ((void)0)
#define FAIL_UNLOCK() ((void)0)
#endif

static FailState states[FAIL_DEVICE_COUNT];
static const char* const names[FAIL_DEVICE_COUNT] = {"pod", "g7", "medtrum"};

static int valid(FailDevice d) { return (int)d >= 0 && (int)d < FAIL_DEVICE_COUNT; }

static uint16_t clamp16(unsigned n) { return (uint16_t)(n > 65535u ? 65535u : n); }

long long fail_decide(FailDevice device, FailAction action, int64_t nowMs) {
  if (!valid(device)) return action == FAIL_ACCEPT || action == FAIL_ADVERTISE ? 1 : 0;
  FailState* s = &states[device];
  long long out = 0;
  FAIL_LOCK();
  switch (action) {
    case FAIL_ACCEPT:
      if (s->dropPending) {
        s->dropPending = 0;
      } else if (s->refuseConnects) {
        s->refuseConnects--;
      } else {
        out = 1;
      }
      break;
    case FAIL_NOTIFY_DELAY_MS:
      out = s->delayMs;
      break;
    case FAIL_ADVERTISE:
      out = nowMs >= s->quietUntilMs ? 1 : 0;
      break;
    case FAIL_CONSUME_MESSAGE:
      if (s->ignoreMessages) {
        s->ignoreMessages--;
        out = 1;
      }
      break;
  }
  FAIL_UNLOCK();
  return out;
}

void fail_clear_all(void) {
  FAIL_LOCK();
  memset(states, 0, sizeof states);
  FAIL_UNLOCK();
}

void fail_clear(FailDevice device) {
  if (!valid(device)) return;
  FAIL_LOCK();
  memset(&states[device], 0, sizeof states[device]);
  FAIL_UNLOCK();
}

FailState fail_get(FailDevice device) {
  FailState copy = {};
  if (!valid(device)) return copy;
  FAIL_LOCK();
  copy = states[device];
  FAIL_UNLOCK();
  return copy;
}

void fail_set_drop(FailDevice device, int on) {
  if (!valid(device)) return;
  FAIL_LOCK();
  states[device].dropPending = on ? 1 : 0;
  FAIL_UNLOCK();
}

void fail_set_refuse(FailDevice device, unsigned connects) {
  if (!valid(device)) return;
  FAIL_LOCK();
  states[device].refuseConnects = clamp16(connects);
  FAIL_UNLOCK();
}

void fail_set_quiet_for(FailDevice device, unsigned ms, int64_t nowMs) {
  if (!valid(device)) return;
  FAIL_LOCK();
  states[device].quietUntilMs = ms ? nowMs + (int64_t)ms : 0;
  FAIL_UNLOCK();
}

void fail_set_delay(FailDevice device, unsigned ms) {
  if (!valid(device)) return;
  FAIL_LOCK();
  states[device].delayMs = clamp16(ms);
  FAIL_UNLOCK();
}

void fail_set_ignore(FailDevice device, unsigned messages) {
  if (!valid(device)) return;
  FAIL_LOCK();
  states[device].ignoreMessages = clamp16(messages);
  FAIL_UNLOCK();
}

const char* fail_apply(FailDevice device, const FailRequest* request, int64_t nowMs) {
  if (!request) return "missing failure request";
  if (!valid(device)) return "device is pod, g7 or medtrum";
  if (request->drop < FAIL_ABSENT || request->drop > 1) return "drop is true or false";
  if (request->refuse < FAIL_ABSENT || request->refuse > 1000) return "refuse is 0-1000 connects";
  if (request->quietMs < FAIL_ABSENT || request->quietMs > 86400000) return "quietMs is 0-86400000 ms";
  if (request->delayMs < FAIL_ABSENT || request->delayMs > 60000) return "delayMs is 0-60000 ms";
  if (request->ignore < FAIL_ABSENT || request->ignore > 1000) return "ignore is 0-1000 messages";

  FailState* s = &states[device];
  FAIL_LOCK();
  if (request->drop >= 0) s->dropPending = request->drop ? 1 : 0;
  if (request->refuse >= 0) s->refuseConnects = (uint16_t)request->refuse;
  if (request->quietMs >= 0) s->quietUntilMs = request->quietMs ? nowMs + (int64_t)request->quietMs : 0;
  if (request->delayMs >= 0) s->delayMs = (uint16_t)request->delayMs;
  if (request->ignore >= 0) s->ignoreMessages = (uint16_t)request->ignore;
  FAIL_UNLOCK();
  return 0;
}

int fail_device_from_name(const char* name) {
  if (!name) return -1;
  // The CGM's slot is "g7" in the /fail API and on the panels; "cgm" is the
  // same slot, named for what it now carries (a G6 transmitter or a G7).
  if (strcmp(name, "cgm") == 0) return FAIL_CGM;
  for (int i = 0; i < FAIL_DEVICE_COUNT; i++)
    if (strcmp(name, names[i]) == 0) return i;
  return -1;
}

const char* fail_device_name(FailDevice device) {
  return valid(device) ? names[(int)device] : "unknown";
}
