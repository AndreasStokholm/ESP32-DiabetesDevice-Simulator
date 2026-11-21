/* Host tests for src/medtrum_serial.h: the one place the base serial changes
 * between the order the app asks for and the order the advertisement carries.
 *
 * The panel shows the app order, so these checks are what keeps "what you read
 * on the web page" equal to "what you type in Trio".
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "medtrum_serial.h"

static int checks, failures;

#define CHECK(x)                                                \
    do {                                                        \
        checks++;                                               \
        if (!(x)) {                                             \
            failures++;                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        }                                                       \
    } while (0)

static void test_200u(void) {
    uint8_t wire[4];
    char app[9];
    CHECK(medtrum_serial_app_to_wire("28D8124A", wire));
    CHECK(wire[0] == 0x4A && wire[1] == 0x12 && wire[2] == 0xD8 && wire[3] == 0x28);
    medtrum_serial_wire_to_app(wire, app);
    CHECK(strcmp(app, "28D8124A") == 0);
    /* Lower case is the same serial. */
    CHECK(medtrum_serial_app_to_wire("28d8124a", wire));
    CHECK(wire[0] == 0x4A && wire[3] == 0x28);
}

static void test_300u(void) {
    uint8_t wire[4];
    char app[9];
    CHECK(medtrum_serial_app_to_wire("1416DF52", wire));
    CHECK(wire[0] == 0x52 && wire[1] == 0xDF && wire[2] == 0x16 && wire[3] == 0x14);
    medtrum_serial_wire_to_app(wire, app);
    CHECK(strcmp(app, "1416DF52") == 0);
}

/* The two simulator serials MedtrumKit hardcodes when a base sends no
 * manufacturer data are the app order of the bytes this project advertises.
 * If this ever fails, the app and the panel have drifted apart. */
static void test_matches_medtrumkit_bypass(void) {
    uint8_t wire[4];
    CHECK(medtrum_serial_app_to_wire("28D8124A", wire));
    /* MedtrumKit: pumpSN: Data([0x28, 0xD8, 0x12, 0x4A]) is the app order. */
    CHECK(wire[0] == 0x4A && wire[1] == 0x12 && wire[2] == 0xD8 && wire[3] == 0x28);
    CHECK(medtrum_serial_app_to_wire("1416DF52", wire));
    /* MedtrumKit: pumpSN: Data([0x14, 0x16, 0xDF, 0x52]). */
    CHECK(wire[0] == 0x52 && wire[1] == 0xDF && wire[2] == 0x16 && wire[3] == 0x14);
}

static void test_rejects_bad_input(void) {
    uint8_t wire[4];
    CHECK(!medtrum_serial_app_to_wire("", wire));
    CHECK(!medtrum_serial_app_to_wire("28D8124", wire));     /* 7 digits */
    CHECK(!medtrum_serial_app_to_wire("28D8124AB", wire));   /* 9 digits */
    CHECK(!medtrum_serial_app_to_wire("28D8124G", wire));    /* not hex */
    CHECK(!medtrum_serial_app_to_wire("28D8 24A", wire));    /* space */
    CHECK(!medtrum_serial_app_to_wire(NULL, wire));
}

int main(void) {
    test_200u();
    test_300u();
    test_matches_medtrumkit_bypass();
    test_rejects_bad_input();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
