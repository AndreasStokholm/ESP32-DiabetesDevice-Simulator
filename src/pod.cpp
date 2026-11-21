// The Omnipod pod half of the board (after ESP-Sim's main.c and store.c).
//
// TEST EQUIPMENT ONLY: it delivers no insulin, and what it reports is made
// up. Never use it, or an app paired to it, for therapy.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pod.h"

#include <Arduino.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sys/time.h>
#include <time.h>

#include "radio.h"
#include "util.h"

namespace pod {

static dash_pod_t thePod;
static dash_state_t restored;
static SemaphoreHandle_t mu;
static nvs_handle_t nvs;

// MARK: - Storage: the same NVS namespace and key as ESP-Sim

static bool loadState(dash_state_t* st) {
  size_t len = sizeof *st;
  return nvs_get_blob(nvs, "state", st, &len) == ESP_OK && len == sizeof *st && dash_state_valid(st);
}

static void saveState(void*, const dash_state_t* st) {
  esp_err_t err = nvs_set_blob(nvs, "state", st, sizeof *st);
  if (err == ESP_OK) err = nvs_commit(nvs);
  if (err != ESP_OK) logf("pod: could not save the pod state: %s", esp_err_to_name(err));
}

// MARK: - The platform, as the pod core sees it

static int64_t nowMs(void*) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void crash(void*) {
  logf("pod: rebooting, as the control panel asked");
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_restart();
}

static const dash_io_t io = {
    .ctx = nullptr,
    .next_event = [](void*, dash_ev_t* ev, uint32_t t) { return radio::podNextEvent(ev, t); },
    .notify_cmd = [](void*, const uint8_t* p, size_t n) { radio::podNotifyCmd(p, n); },
    .notify_data = [](void*, const uint8_t* p, size_t n) { radio::podNotifyData(p, n); },
    .disconnect = [](void*) { radio::podDisconnect(); },
    .advertise = [](void*, const dash_adv_t* adv) { radio::podAdvertise(adv); },
    .now_ms = nowMs,
    .save = saveState,
    .lock = [](void*) { xSemaphoreTake(mu, portMAX_DELAY); },
    .unlock = [](void*) { xSemaphoreGive(mu); },
    .crash = crash,
};

// The firmware's build time, as a last resort for the clock.
static time_t buildTime() {
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {0};
  struct tm tm = {};
  memcpy(mon, __DATE__, 3);
  tm.tm_mon = (int)((strstr(months, mon) - months) / 3);
  tm.tm_mday = atoi(__DATE__ + 4);
  tm.tm_year = atoi(__DATE__ + 7) - 1900;
  tm.tm_hour = atoi(__TIME__);
  tm.tm_min = atoi(__TIME__ + 3);
  tm.tm_sec = atoi(__TIME__ + 6);
  return mktime(&tm);
}

// The pod's age and delivery timing come from the wall clock. Without a
// network it starts at 1970: pick up from the pod's last save (losing only
// the time the board was off) until NTP sets it. The sensor does not use this
// stand-in time: see wallMs().
static void setClock(bool haveState) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec > 1700000000) return;
  time_t t = buildTime();
  if (haveState && restored.saved_ms / 1000 > t) t = (time_t)(restored.saved_ms / 1000);
  tv.tv_sec = t;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

// The pod core logs through ESP-IDF's logger, which Arduino leaves on the
// UART: send it to the USB console with everything else.
static int idfLog(const char* fmt, va_list ap) {
  char buf[256];
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  while (n > 0 && n < (int)sizeof buf && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
  if (buf[0]) logf("%s", buf);
  return n;
}

void begin() {
  esp_log_set_vprintf(idfLog);
  esp_log_level_set("dashpod", ESP_LOG_INFO);
  setenv("TZ", "UTC0", 1);
  tzset();
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK || nvs_open("dashpod", NVS_READWRITE, &nvs) != ESP_OK) {
    logf("pod: NVS unavailable; the pod will not persist");
  }

  bool have = loadState(&restored);
  setClock(have);
  mu = xSemaphoreCreateMutex();
  dash_pod_init(&thePod, &io, have ? &restored : nullptr);
  if (have) {
    logf("pod: restored the %s pod: %s, progress %d, reservoir %.2f U",
         thePod.st.pod_type == DASH_POD_O5 ? "Omnipod 5" : "DASH", thePod.st.paired ? "paired" : "not paired",
         thePod.st.progress, thePod.st.reservoir * 0.05);
  } else {
    // A board with no pod yet starts as an Omnipod 5. (The pod core's own
    // default is a DASH; `pod mode dash` or either panel switches.)
    dash_state_fresh(&thePod.st, io.now_ms(io.ctx), DASH_POD_O5);
    logf("pod: a new Omnipod 5 pod: not paired, 150 U (pod mode dash, or either panel, switches it to DASH)");
    saveState(nullptr, &thePod.st);
  }
}

void start() {
  xTaskCreatePinnedToCore(
      [](void*) {
        dash_pod_run(&thePod);
        vTaskDelete(nullptr);
      },
      "pod", 12288, nullptr, 5, nullptr, tskNO_AFFINITY);
}

void setType(dash_pod_type_t type) { dash_pod_set_type(&thePod, type); }

dash_pod_type_t type() {
  dash_pod_type_t t = DASH_POD_DASH;
  xSemaphoreTake(mu, portMAX_DELAY);
  t = (dash_pod_type_t)thePod.st.pod_type;
  xSemaphoreGive(mu);
  return t;
}

std::string statusJson() {
  char json[1024];
  int n = dash_pod_status_json(&thePod, json, sizeof json);
  return n < 0 ? "{}" : std::string(json, n);
}

const char* command(const std::string& json) { return dash_pod_panel_command(&thePod, json.data(), json.size()); }

std::string selfTest() {
  std::string out;
  int64_t t0 = monoMs();
  auto check = [&](const char* what, bool ok) { out += std::string(ok ? "  PASS " : "  FAIL ") + what + "\n"; };

  // AES-128 (FIPS 197 appendix C.1) and SHA-256("abc").
  static const uint8_t key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  static const uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  static const uint8_t ct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
  uint8_t buf[32];
  dash_aes128_encrypt_block(key, pt, buf);
  check("AES-128 known answer", memcmp(buf, ct, 16) == 0);
  static const uint8_t abc[4] = {0xba, 0x78, 0x16, 0xbf};
  dash_sha256((const uint8_t*)"abc", 3, buf);
  check("SHA-256 known answer", memcmp(buf, abc, 4) == 0);

  // P-256: the generator times 1 is the generator.
  uint8_t one[32] = {0}, g[64];
  one[31] = 1;
  static const uint8_t gx[4] = {0x6b, 0x17, 0xd1, 0xf2}, gy[4] = {0x4f, 0xe3, 0x42, 0xe2};
  check("P-256 public key known answer",
        dash_p256_public(one, g) == 0 && memcmp(g, gx, 4) == 0 && memcmp(g + 32, gy, 4) == 0);

  // Two parties: key agreement both ways, and a signature that verifies
  // (and stops verifying when the message changes).
  uint8_t a[32], b[32], pa[64], pb[64], sa[32], sb[32], sig[64];
  bool keys = dash_p256_keygen(a) == 0 && dash_p256_keygen(b) == 0 && dash_p256_public(a, pa) == 0 &&
              dash_p256_public(b, pb) == 0;
  check("P-256 key generation", keys);
  check("P-256 key agreement (ECDH) matches both ways",
        keys && dash_p256_ecdh(a, pb, sa) == 0 && dash_p256_ecdh(b, pa, sb) == 0 && memcmp(sa, sb, 32) == 0);
  uint8_t msg[40];
  dash_random(msg, sizeof msg);
  bool sigOk = keys && dash_p256_sign(a, msg, sizeof msg, sig) == 0;
  check("ECDSA signature verifies", sigOk && dash_p256_verify(pa, msg, sizeof msg, sig) == 0);
  msg[0] ^= 1;
  check("ECDSA rejects a changed message", sigOk && dash_p256_verify(pa, msg, sizeof msg, sig) != 0);
  check("ECDSA rejects the wrong key", sigOk && dash_p256_verify(pb, msg, sizeof msg, sig) != 0);
  out += "  took " + std::to_string((long)(monoMs() - t0)) + " ms\n";
  return out;
}

}  // namespace pod
