/* Host tests for the advertising: the 3-way rotation src/radio.cpp's tick()
 * makes over the one advertising set, and the bytes each device's payload is
 * built from. It includes the production headers (../../src/adv_rotation.h and
 * ../../src/adv_payload.h, both pure C) so the tested choice and the tested
 * bytes are what the firmware sends.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "adv_payload.h"
#include "adv_rotation.h"

static int checks, failures;

#define CHECK(x)                                                \
    do {                                                        \
        checks++;                                               \
        if (!(x)) {                                             \
            failures++;                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        }                                                       \
    } while (0)

static int slot(int g7, int pod, int med, long long now_ms) {
    adv_wants_t w = {g7, pod, med};
    return adv_rotation_slot(&w, now_ms);
}

/* Nobody wants the set: nothing to advertise. */
static void test_none(void) {
    CHECK(slot(0, 0, 0, 0) == ADV_NONE);
    CHECK(slot(0, 0, 0, 1500) == ADV_NONE);
    CHECK(slot(0, 0, 0, 123456) == ADV_NONE);
}

/* One device: it always gets the set, whatever the clock. */
static void test_single(void) {
    for (long long t = 0; t < 30000; t += 1500) {
        CHECK(slot(1, 0, 0, t) == ADV_G7);
        CHECK(slot(0, 1, 0, t) == ADV_POD);
        CHECK(slot(0, 0, 1, t) == ADV_MED);
    }
}

/* G7 and pod: the original two-way rotation, unchanged (even G7, odd pod). */
static void test_g7_pod_matches_two_way(void) {
    for (long long k = 0; k < 12; k++) {
        CHECK(slot(1, 1, 0, k * 1500) == (k % 2 == 0 ? ADV_G7 : ADV_POD));
    }
}

/* Pairs involving the patch keep the G7-then-pod-then-Medtrum order. */
static void test_pairs(void) {
    CHECK(slot(1, 0, 1, 0) == ADV_G7);
    CHECK(slot(1, 0, 1, 1500) == ADV_MED);
    CHECK(slot(0, 1, 1, 0) == ADV_POD);
    CHECK(slot(0, 1, 1, 1500) == ADV_MED);
}

/* All three: one slot each, in order, then around again. */
static void test_three_way(void) {
    CHECK(slot(1, 1, 1, 0) == ADV_G7);
    CHECK(slot(1, 1, 1, 1500) == ADV_POD);
    CHECK(slot(1, 1, 1, 3000) == ADV_MED);
    CHECK(slot(1, 1, 1, 4500) == ADV_G7);
    CHECK(slot(1, 1, 1, 6000) == ADV_POD);
    CHECK(slot(1, 1, 1, 7500) == ADV_MED);
}

/* The slot only changes every 1500 ms. */
static void test_slot_width(void) {
    CHECK(slot(1, 1, 1, 1499) == ADV_G7);
    CHECK(slot(1, 1, 1, 1500) == ADV_POD);
    CHECK(slot(1, 1, 1, 2999) == ADV_POD);
    CHECK(slot(1, 1, 1, 3000) == ADV_MED);
}

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

/* The Medtrum base's: flags, the name "MT", the manufacturer data with the
 * serial (little-endian order as the app reads it), type and version. */
static void test_medtrum_payload(void) {
    uint8_t sn[4] = {0x12, 0x34, 0x56, 0x78};
    uint8_t b[ADV_MAX];
    static const uint8_t want[] = {0x02, 0x01, 0x06, 0x03, 0x09, 'M', 'T',
                                   0x09, 0xFF, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0x01, 0x01};
    CHECK(adv_medtrum(sn, b, sizeof b) == sizeof want);
    CHECK(memcmp(b, want, sizeof want) == 0);
    CHECK(adv_medtrum(sn, b, sizeof want - 1) == 0);
}

int main(void) {
    test_none();
    test_single();
    test_g7_pod_matches_two_way();
    test_pairs();
    test_three_way();
    test_slot_width();
    test_cgm_payload();
    test_name_field();
    test_pod_payload();
    test_pod_o5_payload();
    test_medtrum_payload();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
