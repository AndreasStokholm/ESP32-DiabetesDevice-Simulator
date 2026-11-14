#include "util.h"

#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>

static SemaphoreHandle_t logLock() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}

void logf(const char* fmt, ...) {
  char buf[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);

  char stamp[40];
  int64_t w = wallMs();
  if (w) {
    time_t t = w / 1000;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(stamp, sizeof stamp, "%02d:%02d:%02d.%03dZ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(w % 1000));
  } else {
    int64_t m = monoMs();
    snprintf(stamp, sizeof stamp, "+%lld.%03lld", m / 1000, m % 1000);
  }
  xSemaphoreTake(logLock(), portMAX_DELAY);
  Serial.printf("%s %s\n", stamp, buf);
  xSemaphoreGive(logLock());
}

std::string hexstr(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    s.push_back(d[p[i] >> 4]);
    s.push_back(d[p[i] & 15]);
  }
  return s;
}

Bytes randomBytes(size_t n) {
  Bytes b(n);
  esp_fill_random(b.data(), n);
  return b;
}

int64_t monoMs() { return esp_timer_get_time() / 1000; }

static volatile bool clockIsReal = false;

void markClockReal() { clockIsReal = true; }

int64_t wallMs() {
  // Only NTP time counts: the pod sets a stand-in clock when there is no
  // network, which must not anchor the sensor.
  if (!clockIsReal) return 0;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int hwRng(void*, unsigned char* out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}
