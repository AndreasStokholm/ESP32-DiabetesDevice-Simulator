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

// The pod's type, for the pump selector: setType() becomes a new pod of that
// type (what `pod mode dash|o5` does), type() reports the current one so the
// selector can report pod-dash or pod-o5 and only reset the pod when it has to.
void setType(dash_pod_type_t type);
dash_pod_type_t type();

// The control panel's /status JSON.
std::string statusJson();
// One control-panel command ({"command": ..., "value": ...}); returns an
// error message, or nullptr.
const char* command(const std::string& json);
// Checks the pod's crypto on this board (AES, SHA-256, and the P-256 key
// agreement and signatures Omnipod 5 pairing uses). Returns a report.
std::string selfTest();
}  // namespace pod
