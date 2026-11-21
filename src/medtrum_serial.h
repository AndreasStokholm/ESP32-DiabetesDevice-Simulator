/* The base serial has two orders, and the two must not be confused.
 *
 * The WIRE order is what the advertisement's manufacturer data carries (bytes
 * 2..5) and what the patch core stores. The APP order is what MedtrumKit's
 * onboarding asks the user to type: it reverses the typed serial before
 * comparing it with the advertisement, so
 *
 *     advert bytes == reverse(typed serial)
 *
 *   Trio/MedtrumKit/.../BluetoothManager.swift:271  pumpSNState = Data(pumpSNState.reversed())
 *   Trio/MedtrumKit/.../BluetoothManager.swift:287  guard pumpSN == pumpSNState
 *
 * A 200U base therefore advertises 4A 12 D8 28 and the user types 28D8124A.
 *
 * The web UI shows and accepts the APP order, because that is the string the
 * user has to enter in the app: what you read on the panel is what you type in
 * Trio. These two functions are the only conversion, and the host test pins them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static inline int medtrum_hexval(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* The app's serial ("28D8124A") -> the wire bytes the advertisement carries
 * (4A 12 D8 28). Rejects anything that is not exactly 8 hex digits. */
static inline bool medtrum_serial_app_to_wire(const char *app, uint8_t wire[4]) {
    if (app == NULL || strlen(app) != 8) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        int hi = medtrum_hexval(app[2 * i]);
        int lo = medtrum_hexval(app[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        /* The app order is the wire order reversed. */
        wire[3 - i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* The wire bytes -> the string the app asks for, uppercase and NUL-terminated.
 * `app` must have room for 9 characters. */
static inline void medtrum_serial_wire_to_app(const uint8_t wire[4], char app[9]) {
    snprintf(app, 9, "%02X%02X%02X%02X", wire[3], wire[2], wire[1], wire[0]);
}
