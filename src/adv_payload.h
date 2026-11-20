/* The three devices' advertising payloads, assembled byte by byte.
 *
 * Pure C (no Arduino, no NimBLE), so the host test can pin every byte:
 * test/medtrum/test_adv.c checks these against the bytes the bases send.
 * src/radio.cpp only hands the result to the advertiser.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The longest legacy advertisement (31 bytes of AD structures). */
#define ADV_MAX 31

/* A complete local name field: length, type 0x09, the name. Also used as the
 * scan response for all three devices. Returns 0 when it does not fit. */
static inline size_t adv_name(const char *name, uint8_t *out, size_t cap)
{
    size_t len = name ? strlen(name) : 0;
    if (len + 2 > cap) {
        return 0;
    }
    out[0] = (uint8_t)(len + 1);
    out[1] = 0x09;
    if (len) {
        memcpy(out + 2, name, len);
    }
    return len + 2;
}

/* The CGM's, exactly as ESP-Sim's main/ble.c builds it:
 *   02 01 06      flags: LE general discoverable, BR/EDR not supported
 *   03 03 BC FE   complete list of 16-bit service UUIDs: 0xFEBC, little-endian
 *   LL 09 <name>  complete local name (the phone filters its scan on both)
 * Returns 0 when the whole advertisement would not fit. */
static inline size_t adv_cgm(const char *name, uint8_t *out, size_t cap)
{
    size_t len = name ? strlen(name) : 0;
    if (7 + 2 + len > cap) {
        return 0;
    }
    size_t n = 0;
    out[n++] = 0x02;
    out[n++] = 0x01;
    out[n++] = 0x06;
    out[n++] = 0x03;
    out[n++] = 0x03;
    out[n++] = 0xBC;
    out[n++] = 0xFE;
    out[n++] = (uint8_t)(len + 1);
    out[n++] = 0x09;
    if (len) {
        memcpy(out + n, name, len);
        n += len;
    }
    return n;
}

/* The DASH pod's: flags, then the 16-bit service list (9 UUIDs, little-endian)
 * that dash_advertised_services() supplies, as ESP-Sim's ble.c sends it. */
static inline size_t adv_pod(const uint16_t svc[9], uint8_t *out, size_t cap)
{
    if (3 + 2 + 9 * 2 > cap) {
        return 0;
    }
    size_t n = 0;
    out[n++] = 0x02;
    out[n++] = 0x01;
    out[n++] = 0x06;
    out[n++] = 0x13;
    out[n++] = 0x03;
    for (int i = 0; i < 9; i++) {
        out[n++] = (uint8_t)(svc[i] & 0xff);
        out[n++] = (uint8_t)(svc[i] >> 8);
    }
    return n;
}

/* The Omnipod 5 pod's: flags, then one 128-bit service UUID with the paired
 * controller's id in it (dash_o5_advertised_uuid() supplies the 16 bytes). */
static inline size_t adv_pod_o5(const uint8_t uuid[16], uint8_t *out, size_t cap)
{
    if (3 + 2 + 16 > cap) {
        return 0;
    }
    size_t n = 0;
    out[n++] = 0x02;
    out[n++] = 0x01;
    out[n++] = 0x06;
    out[n++] = 0x11;
    out[n++] = 0x07;
    memcpy(out + n, uuid, 16);
    return n + 16;
}

/* The Medtrum base's: flags, the name "MT", and the manufacturer data the app
 * reads the base serial, device type and version from. */
static inline size_t adv_medtrum(const uint8_t sn[4], uint8_t *out, size_t cap)
{
    if (3 + 5 + 11 > cap) {
        return 0;
    }
    size_t n = 0;
    out[n++] = 0x02;
    out[n++] = 0x01;
    out[n++] = 0x06;
    out[n++] = 0x03;
    out[n++] = 0x09;
    out[n++] = 'M';
    out[n++] = 'T';
    out[n++] = 0x09;
    out[n++] = 0xFF;
    out[n++] = 0x00;
    out[n++] = 0x00;
    out[n++] = sn[0];
    out[n++] = sn[1];
    out[n++] = sn[2];
    out[n++] = sn[3];
    out[n++] = 0x01;
    out[n++] = 0x01;
    return n;
}
