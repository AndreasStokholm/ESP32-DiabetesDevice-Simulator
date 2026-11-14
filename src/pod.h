#pragma once
// The Omnipod pod half of the board: ESP-Sim's pod core (lib/dashpod) with its
// clock, storage and task (after ESP-Sim's main.c and store.c).
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>

#include "dash.h"

namespace pod {
// Restores the pod (or makes a new DASH pod) and sets a stand-in clock when
// the board has none. Call before the radio starts.
void begin();
// Starts serving phones. Call once the radio is up.
void start();

// The control panel's /status JSON.
std::string statusJson();
// One control-panel command ({"command": ..., "value": ...}); returns an
// error message, or nullptr.
const char* command(const std::string& json);
// Checks the pod's crypto on this board (AES, SHA-256, and the P-256 key
// agreement and signatures Omnipod 5 pairing uses). Returns a report.
std::string selfTest();
}  // namespace pod
