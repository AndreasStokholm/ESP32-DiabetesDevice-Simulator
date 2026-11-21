/* Host tests for src/medtrum_serial.h: which serial the app accepts, and the
 * parsing the panel's serial field goes through.
 *
 * The numbers below are the core's own decrypt, so this is the app's validation
 * pinned in the same code path the panel uses.
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

static void test_accepted(void) {
    uint8_t sn[4];
    CHECK(medtrum_serial_parse("4A12D828", sn));
    CHECK(strcmp(medtrum_serial_model(sn), "MD0201") == 0);
    CHECK(medtrum_serial_valid(sn));
    CHECK(medtrum_serial_parse("52DF1614", sn));
    CHECK(strcmp(medtrum_serial_model(sn), "MD8301") == 0);
    CHECK(medtrum_serial_valid(sn));
    /* Lower case is the same serial. */
    CHECK(medtrum_serial_parse("4a12d828", sn));
    CHECK(medtrum_serial_valid(sn));
}

/* The reversed strings look plausible and are what a careful reader of
 * MedtrumKit's source would type. The app rejects them, so the panel must too. */
static void test_rejected_by_the_app(void) {
    uint8_t sn[4];
    CHECK(medtrum_serial_parse("28D8124A", sn));
    CHECK(!medtrum_serial_valid(sn));
    CHECK(strcmp(medtrum_serial_model(sn), "INVALID") == 0);
    CHECK(medtrum_serial_parse("1416DF52", sn));
    CHECK(!medtrum_serial_valid(sn));
}

static void test_parse_rejects(void) {
    uint8_t sn[4];
    CHECK(!medtrum_serial_parse("", sn));
    CHECK(!medtrum_serial_parse("4A12D82", sn));   /* 7 digits */
    CHECK(!medtrum_serial_parse("4A12D8280", sn)); /* 9 digits */
    CHECK(!medtrum_serial_parse("4A12D82G", sn));  /* not hex */
    CHECK(!medtrum_serial_parse("4A12 828", sn));  /* space */
    CHECK(!medtrum_serial_parse(NULL, sn));
}

int main(void) {
    test_accepted();
    test_rejected_by_the_app();
    test_parse_rejects();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
