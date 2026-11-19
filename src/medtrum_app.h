#pragma once
// The Medtrum patch half of the board: ESP-Sim's portable patch core
// (lib/medtrumpatch) with its clock, storage and task (after ESP-Sim's
// main/medtrum_app.c and main/store.c).
//
// TEST EQUIPMENT ONLY: it delivers nothing, and what it reports is made up.
// Never use it, or an app paired to it, for therapy.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>

#include "medtrum.h"

namespace medtrum {
#define MEDTRUM_STATE_MAGIC 0x4d445452u /* "MDTR" */
#define MEDTRUM_STATE_VERSION 1u

// What survives a reboot: the base identity and the delivery model. The
// session (auth, token) does not.
typedef struct {
  uint32_t magic;
  uint32_t version;
  uint8_t sn[4];          /* the advertised serial, as the app reads it */
  uint8_t device_type;
  uint8_t sw_version[3];
  medtrum_status_t status;
  medtrum_delivery_t delivery;
} medtrum_saved_t;

// Restores the patch (or makes a fresh 200 U one) and sets up storage. Call
// after pod::begin(), before the radio starts.
void begin();
// Advertises and starts serving phones. Call once the radio is up.
void start();

// The control panel's /medtrum/status JSON.
std::string statusJson();
// One control-panel command ({"command": ...}); returns an error message, or
// nullptr.
const char* command(const std::string& json);
}  // namespace medtrum
