// The simulated Medtrum Nano patch on the board: wires the portable patch
// core (lib/medtrumpatch) to the radio, the clock and flash, runs the connect
// session and the delivery commands, and takes the control panel's commands.
//
// Ported from ESP-Sim's main/medtrum_app.c. TEST EQUIPMENT ONLY: it delivers
// nothing, and what it reports is made up. Never use it, or an app paired to
// it, for therapy.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "medtrum_app.h"

#include <esp_random.h>
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

#include "medtrum_serial.h"
#include "radio.h"
#include "util.h"

namespace medtrum {

// The default base serial: the bytes the app reads out of the advertisement,
// in order. Reversed and decrypted they are MD0201 (200U). The panel switches
// to 52DF1614 (MD8301, 300U) with `medtrumSetModel`.
static const uint8_t defaultSn[4] = {0x4A, 0x12, 0xD8, 0x28};
#define MEDTRUM_DEFAULT_UNITS 200

static medtrum_patch_t patch;
static medtrum_request_t req;
static SemaphoreHandle_t mu;
static nvs_handle_t nvs;

// MARK: - The platform, as the patch core sees it

static int64_t nowMs(void*) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void fillRandom(void*, uint8_t* p, size_t n) { esp_fill_random(p, n); }

static void logLine(void*, const char* msg) { logf("medtrum: %s", msg); }

static const medtrum_io_t io = {
    .ctx = nullptr,
    .next_event = [](void*, medtrum_ev_t* ev, uint32_t t) { return radio::medtrumNextEvent(ev, t); },
    .notify = [](void*, const uint8_t* p, size_t n) { radio::medtrumNotify(p, n); },
    .disconnect = [](void*) { radio::medtrumDisconnect(); },
    .advertise = [](void*, bool on, const uint8_t sn[4]) { radio::medtrumAdvertise(on, sn); },
    .now_ms = nowMs,
    .random = fillRandom,
    .log = logLine,
};

#define LOCKED(stmt)                       \
  do {                                     \
    xSemaphoreTake(mu, portMAX_DELAY);     \
    stmt;                                  \
    xSemaphoreGive(mu);                    \
  } while (0)

// MARK: - The serial and its model

// MedtrumKit reads the serial big-endian and simpleDecrypts it.
static const char* modelName(const uint8_t sn[4]) {
  uint32_t word = ((uint32_t)sn[0] << 24) | ((uint32_t)sn[1] << 16) | ((uint32_t)sn[2] << 8) | sn[3];
  uint32_t type = medtrum_simple_decrypt(word);
  if (type >= 126000000u && type < 127000000u) return "MD0201";
  if (type >= 127000000u && type < 128000000u) return "MD5201";
  if (type >= 128000000u && type < 129000000u) return "MD8201";
  if (type >= 130000000u && type < 131000000u) return "MD0202";
  if (type >= 131000000u && type < 132000000u) return "MD5202";
  if (type >= 148000000u && type < 149000000u) return "MD8301";
  return "INVALID";
}


// MARK: - Persistence (NVS "medtrum"/"state", distinct from the pod's)

static bool validState(const medtrum_saved_t* st) {
  return st->magic == MEDTRUM_STATE_MAGIC && st->version == MEDTRUM_STATE_VERSION;
}

static bool loadState(medtrum_saved_t* st) {
  size_t len = sizeof *st;
  return nvs_get_blob(nvs, "state", st, &len) == ESP_OK && len == sizeof *st && validState(st);
}

static void saveState() {
  medtrum_saved_t st = {};
  st.magic = MEDTRUM_STATE_MAGIC;
  st.version = MEDTRUM_STATE_VERSION;
  st.device_type = patch.device_type;
  st.status = patch.status;
  st.delivery = patch.delivery;
  memcpy(st.sn, patch.sn, 4);
  memcpy(st.sw_version, patch.sw_version, 3);
  esp_err_t err = nvs_set_blob(nvs, "state", &st, sizeof st);
  if (err == ESP_OK) err = nvs_commit(nvs);
  if (err != ESP_OK) logf("medtrum: could not save the patch state: %s", esp_err_to_name(err));
}

// A fresh patch identity, as the reference assigns on activation. The app
// validates every later notification against a 16-bit patch id
// (basal.patchId / storage.patchId), so only the low two bytes are random;
// the high two stay zero.
static void assignPatchId() {
  uint8_t id[4] = {0, 0, 0, 0};
  esp_fill_random(id, 2);
  medtrum_patch_set_patch_id(&patch, id);
}

// MARK: - Requests

static bool onWrite(const uint8_t* data, size_t n, medtrum_frag_t* frags, int* count) {
  if (!medtrum_request_feed(&req, data, n)) return false;
  // Only a command that changed the model needs a flash commit; a GET_TIME
  // poll, a sync or a subscribe must not wear NVS.
  medtrum_status_t wasStatus = patch.status;
  medtrum_delivery_t wasDelivery = patch.delivery;
  int got = medtrum_patch_handle(&patch, req.command, req.sequence, req.content, req.content_len, frags,
                                 MEDTRUM_FRAGS_MAX);
  if (got < 0) {
    logf("medtrum: unhandled command %u", req.command);
    return false;
  }
  *count = got;
  if (memcmp(&wasStatus, &patch.status, sizeof wasStatus) != 0 ||
      memcmp(&wasDelivery, &patch.delivery, sizeof wasDelivery) != 0) {
    saveState();
  }
  return true;
}

// MARK: - The run loop

static void medtrumTask(void*) {
  // A stop flag rather than `return` inside LOCKED: the mutex must always be
  // given back, even when the host tests ask the loop to end.
  bool stop = false;
  while (!stop) {
    medtrum_ev_t ev;
    bool got = io.next_event(io.ctx, &ev, 1000);
    // Collect replies and the status payload under the lock, then send them
    // after it has been released: an injected notification delay must not
    // stall the web server, which takes the same lock.
    medtrum_frag_t frags[MEDTRUM_FRAGS_MAX];
    int count = 0;
    bool sendStatus = false;
    uint8_t statusPayload[MEDTRUM_STATUS_MAX];
    size_t statusLen = 0;
    LOCKED({
      medtrum_patch_tick(&patch, nowMs(nullptr));
      if (!got) {
        sendStatus = true;
      } else {
        switch (ev.type) {
          case MEDTRUM_EV_CONNECTED:
            logf("medtrum: phone connected");
            break;
          case MEDTRUM_EV_DISCONNECTED:
            logf("medtrum: phone disconnected");
            patch.authenticated = false;
            saveState();
            break;
          case MEDTRUM_EV_WRITE:
            sendStatus = onWrite(ev.data, ev.len, frags, &count);
            break;
          case MEDTRUM_EV_STOP:
            stop = true;
            break;
          default:
            break;
        }
      }
      if (sendStatus) statusLen = medtrum_patch_notification(&patch, statusPayload, sizeof statusPayload);
    });
    for (int i = 0; i < count; i++) io.notify(io.ctx, frags[i].data, frags[i].len);
    if (statusLen > 0) radio::medtrumNotifyStatus(statusPayload, statusLen);
  }
}

void begin() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK || nvs_open("medtrum", NVS_READWRITE, &nvs) != ESP_OK) {
    logf("medtrum: NVS unavailable; the patch will not persist");
  }
  mu = xSemaphoreCreateMutex();
  medtrum_request_init(&req);

  medtrum_saved_t st;
  bool have = loadState(&st);
  uint8_t sn[4];
  memcpy(sn, have ? st.sn : defaultSn, 4);
  medtrum_patch_init(&patch, sn);
  patch.device_type = 80;
  patch.sw_version[0] = 12;
  patch.sw_version[1] = 1;
  patch.sw_version[2] = 3;
  if (have) {
    patch.device_type = st.device_type;
    memcpy(patch.sw_version, st.sw_version, 3);
    patch.status = st.status;
    patch.delivery = st.delivery;
    patch.delivery.last_ms = INT64_MIN; /* the wall clock restarted */
  } else {
    medtrum_patch_set_state(&patch, MEDTRUM_STATE_FILLED);
    medtrum_patch_set_reservoir(&patch, (uint16_t)(MEDTRUM_DEFAULT_UNITS * 20));
    medtrum_patch_set_battery(&patch, 5901296); /* 5.97 V / 2.81 V */
    saveState();
  }
  if (patch.delivery.patch_id[0] == 0 && patch.delivery.patch_id[1] == 0 && patch.delivery.patch_id[2] == 0 &&
      patch.delivery.patch_id[3] == 0) {
    assignPatchId();
    saveState();
  }
  logf("medtrum: simulating a Medtrum %s, serial %02X%02X%02X%02X. TEST EQUIPMENT: delivers nothing.",
       modelName(patch.sn), patch.sn[0], patch.sn[1], patch.sn[2], patch.sn[3]);
}

void start() {
  radio::medtrumAdvertise(true, patch.sn);
  xTaskCreatePinnedToCore(
      [](void*) {
        medtrumTask(nullptr);
        vTaskDelete(nullptr);
      },
      "medtrum", 12288, nullptr, 5, nullptr, tskNO_AFFINITY);
}

// MARK: - The panel

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

// The number after "key", as a float: the reservoir is set in 0.05 U steps.
static bool jsonDouble(const char* j, const char* key, double* out) {
  char pat[24];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char* p = strstr(j, pat);
  if (!p) return false;
  p += strlen(pat);
  while (*p == ' ' || *p == ':') p++;
  char* end;
  double v = strtod(p, &end);
  if (end == p) return false;
  *out = v;
  return true;
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

std::string statusJson() {
  char json[512];
  int n = 0;
  LOCKED({
    n = snprintf(json, sizeof json,
                 "{\"serial\":\"%02X%02X%02X%02X\",\"model\":\"%s\",\"state\":%u,"
                 "\"reservoir\":%.2f,\"battery\":%.2f,\"patchTime\":%u,\"connected\":%s}",
                 patch.sn[0], patch.sn[1], patch.sn[2], patch.sn[3], modelName(patch.sn),
                 (unsigned)patch.status.state, patch.status.reservoir * 0.05,
                 (double)(patch.status.battery >> 12) / 512.0, (unsigned)patch.delivery.patch_time,
                 radio::medtrumConnected() ? "true" : "false");
  });
  if (n <= 0 || n >= (int)sizeof json) return "{}";
  return std::string(json, n);
}

// {"command": "medtrumSetSerial", "sn": "4A12D828"}, "medtrumSetModel" {units:
// 200|300}, "medtrumSetReservoir" {units}, "medtrumSetState" {state},
// "medtrumSetFault" {state}, "medtrumReset". Returns NULL, or what was wrong.
const char* command(const std::string& json) {
  const char* body = json.c_str();
  char cmd[32], hex[16];
  long v;
  if (!jsonStr(body, "command", cmd, sizeof cmd)) return "want {\"command\": ...}";
  if (strcmp(cmd, "medtrumSetSerial") == 0) {
    uint8_t sn[4];
    if (!jsonStr(body, "sn", hex, sizeof hex) || !medtrum_serial_parse(hex, sn)) {
      return "sn is 8 hex digits, e.g. \"4A12D828\"";
    }
    // The app derives the model from this serial and refuses a base it cannot
    // read one from, so refuse it here rather than let Trio say "Incorrect
    // serial number received" after a scan.
    if (!medtrum_serial_valid(sn)) {
      return "that serial does not validate in the app: 4A12D828 (200U) or 52DF1614 (300U)";
    }
    LOCKED({
      memcpy(patch.sn, sn, 4);
      patch.authenticated = false;
      saveState();
      io.advertise(io.ctx, true, patch.sn);
    });
  } else if (strcmp(cmd, "medtrumSetModel") == 0) {
    if (!jsonNumber(body, "units", &v) || (v != 200 && v != 300)) return "units is 200 or 300";
    uint8_t sn[4] = {0x4A, 0x12, 0xD8, 0x28};
    if (v == 300) {
      sn[0] = 0x52;
      sn[1] = 0xDF;
      sn[2] = 0x16;
      sn[3] = 0x14;
    }
    LOCKED({
      memcpy(patch.sn, sn, 4);
      patch.authenticated = false;
      saveState();
      io.advertise(io.ctx, true, patch.sn);
    });
  } else if (strcmp(cmd, "medtrumSetReservoir") == 0) {
    double units;
    if (!jsonDouble(body, "units", &units) || units < 0 || units > 300) return "units is 0 to 300";
    LOCKED({
      // One pulse is 0.05 U; round 186.8 to 3736 pulses.
      medtrum_patch_set_reservoir(&patch, (uint16_t)lround(units / 0.05));
      saveState();
    });
  } else if (strcmp(cmd, "medtrumSetState") == 0) {
    if (!jsonNumber(body, "state", &v) || v < 0 || v > 255) return "state is 0 to 255";
    LOCKED({
      medtrum_patch_set_state(&patch, (uint8_t)v);
      saveState();
    });
  } else if (strcmp(cmd, "medtrumSetFault") == 0) {
    if (!jsonNumber(body, "state", &v) || v < 0 || v > 255) return "state is 0 to 255";
    LOCKED({
      medtrum_patch_set_fault(&patch, (uint8_t)v);
      saveState();
    });
  } else if (strcmp(cmd, "medtrumReset") == 0) {
    LOCKED({
      uint8_t sn[4];
      memcpy(sn, patch.sn, 4);
      medtrum_patch_init(&patch, sn);
      patch.device_type = 80;
      patch.sw_version[0] = 12;
      patch.sw_version[1] = 1;
      patch.sw_version[2] = 3;
      medtrum_patch_set_state(&patch, MEDTRUM_STATE_FILLED);
      medtrum_patch_set_reservoir(&patch, (uint16_t)(MEDTRUM_DEFAULT_UNITS * 20));
      medtrum_patch_set_battery(&patch, 5901296);
      assignPatchId();
      saveState();
    });
  } else {
    return "unknown Medtrum command";
  }
  return nullptr;
}

}  // namespace medtrum
