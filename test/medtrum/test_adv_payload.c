/* Host tests for the advertising payload builders in src/adv_payload.h: the
 * bytes each device's advertisement and scan response are built from. It
 * includes the production header (../../src/adv_payload.h, pure C), so the
 * bytes it pins are exactly what src/radio.cpp hands to the advertiser.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "adv_payload.h"

static int checks, failures;

#define CHECK(x)                                                \
    do {                                                        \
        checks++;                                               \
        if (!(x)) {                                             \
            failures++;                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        }                                                       \
    } while (0)

/* The CGM's advertisement, byte for byte: flags, the 0xFEBC service the phones
 * filter on (little-endian), then the complete name (ESP-Sim's main/ble.c). */
static void test_cgm_payload(void) {
    uint8_t b[ADV_MAX];
    static const uint8_t g7[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0xBC, 0xFE,
                                 0x07, 0x09, 'D', 'X', 'C', 'M', 'a', 'b'};
    CHECK(adv_cgm("DXCMab", b, sizeof b) == sizeof g7);
    CHECK(memcmp(b, g7, sizeof g7) == 0);

    /* A G6 is "Dexcom" plus the id's last two characters. */
    static const uint8_t g6[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0xBC, 0xFE,
                                 0x09, 0x09, 'D', 'e', 'x', 'c', 'o', 'm', 'A', 'B'};
    CHECK(adv_cgm("DexcomAB", b, sizeof b) == sizeof g6);
    CHECK(memcmp(b, g6, sizeof g6) == 0);

    /* No name (kind off): flags and the service only. */
    static const uint8_t none[] = {0x02, 0x01, 0x06, 0x03, 0x03, 0xBC, 0xFE, 0x01, 0x09};
    CHECK(adv_cgm("", b, sizeof b) == sizeof none);
    CHECK(memcmp(b, none, sizeof none) == 0);

    /* Over 31 bytes is refused, not truncated; so is a short buffer. */
    CHECK(adv_cgm("01234567890123456789012", b, sizeof b) == 0);
    CHECK(adv_cgm("DXCMab", b, sizeof g7 - 1) == 0);
    CHECK(adv_cgm("DXCMab", b, sizeof g7) == sizeof g7);
}

/* The scan response: one complete local name field, the same for all three. */
static void test_name_field(void) {
    uint8_t b[ADV_MAX];
    static const uint8_t want[] = {0x07, 0x09, 'D', 'X', 'C', 'M', 'a', 'b'};
    CHECK(adv_name("DXCMab", b, sizeof b) == sizeof want);
    CHECK(memcmp(b, want, sizeof want) == 0);
    /* The patch's and the pod's names. */
    static const uint8_t mt[] = {0x03, 0x09, 'M', 'T'};
    CHECK(adv_name("MT", b, sizeof b) == sizeof mt);
    CHECK(memcmp(b, mt, sizeof mt) == 0);
    CHECK(adv_name("DXCMab", b, 7) == 0);
    CHECK(adv_name("DXCMab", b, 8) == 8);
}

/* The DASH pod's: flags, then nine 16-bit service UUIDs little-endian. */
static void test_pod_payload(void) {
    uint16_t svc[9] = {0x1234, 0xAB01, 0x0000, 0xFEDC, 0x1000, 0x1001, 0x1002, 0x1003, 0xFFFF};
    uint8_t b[ADV_MAX];
    static const uint8_t want[] = {0x02, 0x01, 0x06, 0x13, 0x03,
                                   0x34, 0x12, 0x01, 0xAB, 0x00, 0x00, 0xDC, 0xFE, 0x00,
                                   0x10, 0x01, 0x10, 0x02, 0x10, 0x03, 0x10, 0xFF, 0xFF};
    CHECK(adv_pod(svc, b, sizeof b) == sizeof want);
    CHECK(memcmp(b, want, sizeof want) == 0);
    CHECK(adv_pod(svc, b, sizeof want - 1) == 0);
}

/* The Omnipod 5 pod's: flags, then one 128-bit UUID. */
static void test_pod_o5_payload(void) {
    uint8_t uuid[16];
    uint8_t b[ADV_MAX];
    static const uint8_t want[21] = {0x02, 0x01, 0x06, 0x11, 0x07,
                                     0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; i++) uuid[i] = (uint8_t)i;
    CHECK(adv_pod_o5(uuid, b, sizeof b) == sizeof want);
    CHECK(memcmp(b, want, sizeof want) == 0);
    CHECK(adv_pod_o5(uuid, b, sizeof want - 1) == 0);
}

/* The Medtrum base's: flags, the name "MT", and manufacturer data formed of
 * company 0x4781 (18305), the serial, type and version. The company id is the
 * real one: an app that gates on it (Rockit) has to see a base it would accept.
 * ESP-Sim sends 0x0000 here and is the one deviation from its bytes. */
static void test_medtrum_payload(void) {
    uint8_t sn[4] = {0x12, 0x34, 0x56, 0x78};
    uint8_t b[ADV_MAX];
    static const uint8_t want[] = {0x02, 0x01, 0x06, 0x03, 0x09, 'M', 'T',
                                   0x09, 0xFF, 0x81, 0x47, 0x12, 0x34, 0x56, 0x78, 0x01, 0x01};
    CHECK(adv_medtrum(sn, b, sizeof b) == sizeof want);
    CHECK(memcmp(b, want, sizeof want) == 0);
    CHECK(adv_medtrum(sn, b, sizeof want - 1) == 0);
}

int main(void) {
    test_cgm_payload();
    test_name_field();
    test_pod_payload();
    test_pod_o5_payload();
    test_medtrum_payload();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
