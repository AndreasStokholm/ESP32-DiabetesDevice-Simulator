#pragma once
// The CGM half of the board: ESP-Sim's portable G6/G7 core (lib/cgmsim)
// with its clock, storage and task (after ESP-Sim's main/cgm_app.c).
//
// TEST EQUIPMENT ONLY: the readings are made up. Never let an app that
// controls a real pump or pod read them.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>

// cgm.h is the verbatim portable core and carries no extern "C" guard -- upstream
// it is only ever included from C. This translation unit is C++, so without the
// guard every cgm_* reference is mangled and the link fails with undefined
// references. Wrap it here rather than editing the ported header.
extern "C" {
#include "cgm.h"
}

// NOTE(assembly): C++ cannot have both this namespace and a global variable
// named `sensor` in one translation unit, so src/main.cpp has no global of
// that name: the CGM is reached through this namespace only.

namespace sensor {
#define SENSOR_STATE_MAGIC 0x43474d31u /* "CGM1" */
#define SENSOR_STATE_VERSION 1u

// What survives a reboot: the whole core state (identity, activation, the
// panel's glucose/trend and the 3-hour reading history), and the clock's
// reading at the last save so the sensor clock continues instead of jumping.
// The struct is a blob so a restore is one copy; the magic and version reject
// a blob from an older build.
typedef struct {
  uint32_t magic;
  uint32_t version;
  int64_t saved_at; /* the simulated clock's Unix seconds at the last save */
  cgm_state_t state;
} sensor_saved_t;

// Restores the CGM (migrating the old g7sim-shaped state if it is the first
// boot) and sets up storage. Call after pod::begin(), before the radio starts.
void begin();
// Starts serving phones. Call once the radio is up.
void start();

// The control panel's /g7 status JSON (the core's, plus the radio's own view
// of the link and the sensor's age).
std::string statusJson();
// One control-panel command ({"command": ...}); returns an error message, or
// nullptr.
const char* command(const std::string& json);

// What the panel and the radio ask about.
cgm_kind_t kind();
std::string name();
// The simulated clock's Unix seconds (the readings' and activation's time).
int64_t now();
// What the panel set: the next reading's glucose and trend (tenths of mg/dL
// per minute), and the forced algorithm state (0: from the session's age).
int glucose();
int trend();
int forcedState();

// From the radio, one call per GATT event. These take the core's mutex; the
// radio must not hold its own lock while calling them (the core calls back
// into radio::cgmNotify/cgmAdvertise from inside the same lock).
void onConnect();
void onDisconnect();
void onSubscribe(cgm_chr_t chr, bool on);
void onWrite(cgm_chr_t chr, const uint8_t* p, size_t n);
// Fills out with the auth characteristic's value; returns its length.
size_t onRead(cgm_chr_t chr, uint8_t* out, size_t cap);
}  // namespace sensor
