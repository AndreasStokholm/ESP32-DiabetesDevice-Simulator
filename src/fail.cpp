#include "fail.h"

#include <string.h>

static FailState states[FAIL_DEVICE_COUNT];
static const char* const names[FAIL_DEVICE_COUNT] = {"pod", "g7", "medtrum"};

static int valid(FailDevice d) { return (int)d >= 0 && (int)d < FAIL_DEVICE_COUNT; }

static uint16_t clamp16(unsigned n) { return (uint16_t)(n > 65535u ? 65535u : n); }

long long fail_decide(FailDevice device, FailAction action, int64_t nowMs) {
  if (!valid(device)) return action == FAIL_ACCEPT || action == FAIL_ADVERTISE ? 1 : 0;
  FailState* s = &states[device];
  switch (action) {
    case FAIL_ACCEPT:
      if (s->dropPending) {
        s->dropPending = 0;
        return 0;
      }
      if (s->refuseConnects) {
        s->refuseConnects--;
        return 0;
      }
      return 1;
    case FAIL_NOTIFY_DELAY_MS:
      return s->delayMs;
    case FAIL_ADVERTISE:
      return nowMs >= s->quietUntilMs ? 1 : 0;
    case FAIL_CONSUME_MESSAGE:
      if (s->ignoreMessages) {
        s->ignoreMessages--;
        return 1;
      }
      return 0;
  }
  return 0;
}

void fail_clear_all(void) { memset(states, 0, sizeof states); }

void fail_clear(FailDevice device) {
  if (valid(device)) memset(&states[device], 0, sizeof states[device]);
}

FailState fail_get(FailDevice device) {
  FailState empty = {};
  if (!valid(device)) return empty;
  return states[device];
}

void fail_set_drop(FailDevice device, int on) {
  if (valid(device)) states[device].dropPending = on ? 1 : 0;
}

void fail_set_refuse(FailDevice device, unsigned connects) {
  if (valid(device)) states[device].refuseConnects = clamp16(connects);
}

void fail_set_quiet_for(FailDevice device, unsigned ms, int64_t nowMs) {
  if (valid(device)) states[device].quietUntilMs = ms ? nowMs + (int64_t)ms : 0;
}

void fail_set_delay(FailDevice device, unsigned ms) {
  if (valid(device)) states[device].delayMs = clamp16(ms);
}

void fail_set_ignore(FailDevice device, unsigned messages) {
  if (valid(device)) states[device].ignoreMessages = clamp16(messages);
}

int fail_device_from_name(const char* name) {
  if (!name) return -1;
  for (int i = 0; i < FAIL_DEVICE_COUNT; i++)
    if (strcmp(name, names[i]) == 0) return i;
  return -1;
}

const char* fail_device_name(FailDevice device) {
  return valid(device) ? names[(int)device] : "unknown";
}
