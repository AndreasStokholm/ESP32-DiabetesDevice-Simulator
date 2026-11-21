/* The base serial, and the check that keeps the panel and the app agreeing.
 *
 * There is ONE order, and it is the order the advertisement carries: a 200U base
 * advertises 4A 12 D8 28 and the user types 4A12D828, a 300U base advertises
 * 52 DF 16 14 and the user types 52DF1614. MedtrumKit looks like it reverses the
 * typed serial (BluetoothManager.swift:271), but its Data.toInt64() is
 * little-endian, so the two reversals cancel: the string it validates is this
 * order. Proven with the core's own decrypt, in test/medtrum/serial_tests:
 *
 *   4A12D828 -> 126022576  MD0201      (valid)
 *   28D8124A -> 1847467079 INVALID
 *   52DF1614 -> 148016470  MD8301      (valid)
 *   1416DF52 -> 493115731  INVALID
 *
 * So this header parses that order and answers whether the app would accept the
 * serial. The panel refuses a serial the app would reject, instead of letting the
 * user find out from Trio.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "medtrum.h" /* medtrum_simple_decrypt */

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

/* "4A12D828" -> the four bytes the advertisement carries. Exactly 8 hex digits. */
static inline bool medtrum_serial_parse(const char *hex, uint8_t sn[4]) {
    if (hex == NULL || strlen(hex) != 8) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        int hi = medtrum_hexval(hex[2 * i]);
        int lo = medtrum_hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        sn[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/* The model the app derives from the serial, as it does: the four bytes read
 * big-endian, then decrypted. "INVALID" is what makes Trio say "Incorrect serial
 * number received". */
static inline const char *medtrum_serial_model(const uint8_t sn[4]) {
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

static inline bool medtrum_serial_valid(const uint8_t sn[4]) {
    return strcmp(medtrum_serial_model(sn), "INVALID") != 0;
}
