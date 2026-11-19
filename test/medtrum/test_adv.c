/* Host tests for the advertising rotation: the 3-way choice src/radio.cpp's
 * tick() makes over the one advertising set. It includes the production header
 * (../../src/adv_rotation.h, pure C) so the tested selection is the code the
 * firmware runs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>

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

int main(void) {
    test_none();
    test_single();
    test_g7_pod_matches_two_way();
    test_pairs();
    test_three_way();
    test_slot_width();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
