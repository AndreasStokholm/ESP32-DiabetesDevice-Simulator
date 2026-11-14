#pragma once
// Small helpers shared by every part of the emulator.

#include <Arduino.h>
#include <stdint.h>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;

// Thread-safe printf-style log line on the USB serial console, prefixed with
// the uptime (and the wall-clock time once NTP has set it).
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

std::string hexstr(const uint8_t* p, size_t n);
inline std::string hexstr(const Bytes& b) { return hexstr(b.data(), b.size()); }

Bytes randomBytes(size_t n);

// Monotonic milliseconds since boot.
int64_t monoMs();

// Wall-clock milliseconds since the Unix epoch, or 0 until NTP has set the clock.
int64_t wallMs();
// Called when NTP sets the clock.
void markClockReal();

// Little-endian appenders, as the G7 wire format uses.
inline void le16(Bytes& b, uint16_t v) { b.push_back(v); b.push_back(v >> 8); }
inline void le32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; i++) b.push_back(v >> (8 * i));
}
inline uint16_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// mbedTLS random callback backed by the ESP32 hardware RNG.
int hwRng(void* ctx, unsigned char* out, size_t len);
