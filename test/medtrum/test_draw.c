/*
 * Host tests for the reservoir draw: a delivered bolus consumes the
 * reservoir, in pulses of 0.05 U, by the increment each tick delivers.
 *
 * The core is portable C: this binary links only lib/medtrumpatch.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "medtrum.h"
#include "test.h"

int failures, checks;

static void set_bolus(medtrum_patch_t *p, uint16_t pulses)
{
    uint8_t content[4] = {1, (uint8_t)pulses, (uint8_t)(pulses >> 8), 0};
    medtrum_frag_t frags[MEDTRUM_FRAGS_MAX];
    int n = medtrum_patch_command(p, MEDTRUM_CMD_SET_BOLUS, 0, content, sizeof content, frags,
                                  MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(p->delivery.bolus_active);
    CHECK(p->delivery.bolus_total == pulses);
    CHECK(p->status.bolus_delivered == 0);
}

/* Ticks from `last_ms` to `end_ms` in `step_ms` steps, one tick per step. */
static void tick_to(medtrum_patch_t *p, int64_t last_ms, int64_t end_ms, int64_t step_ms)
{
    for (int64_t t = last_ms; t < end_ms; t += step_ms) {
        medtrum_patch_tick(p, t);
    }
    medtrum_patch_tick(p, end_ms);
}

/* A 4 U bolus (80 pulses) over its run time (one pulse every 2 s, so 160 s)
 * leaves the reservoir exactly 80 pulses lower. */
static void test_four_unit_bolus(void)
{
    medtrum_patch_t p;
    medtrum_patch_init(&p, (uint8_t[]){1, 2, 3, 4});
    medtrum_patch_set_state(&p, MEDTRUM_STATE_ACTIVE);
    medtrum_patch_set_reservoir(&p, 400); /* 20.0 U */
    set_bolus(&p, 80);                    /* 4.0 U */

    medtrum_patch_tick(&p, 1000000); /* records the clock */
    tick_to(&p, 1000000, 1160000, 10000);

    CHECK(p.status.reservoir == 320);
    CHECK(p.status.bolus_delivered == 80);
    CHECK(p.delivery.bolus_active == false);
    CHECK(p.status.bolus_completed);
}

/* Two ticks inside the same run charge only their own increment: after 20 s
 * (10 pulses) and 40 s (20 pulses) the reservoir is down 20, not 30. */
static void test_two_ticks_no_double_count(void)
{
    medtrum_patch_t p;
    medtrum_patch_init(&p, (uint8_t[]){1, 2, 3, 4});
    medtrum_patch_set_state(&p, MEDTRUM_STATE_ACTIVE);
    medtrum_patch_set_reservoir(&p, 400);
    set_bolus(&p, 80);

    medtrum_patch_tick(&p, 1000000); /* records the clock */
    medtrum_patch_tick(&p, 1020000); /* +20 s: 10 pulses */
    CHECK(p.status.bolus_delivered == 10);
    CHECK(p.status.reservoir == 390);

    medtrum_patch_tick(&p, 1040000); /* +20 s: 20 pulses in total */
    CHECK(p.status.bolus_delivered == 20);
    CHECK(p.status.reservoir == 380);

    /* A re-tick with the same clock is a no-op. */
    medtrum_patch_tick(&p, 1040000);
    CHECK(p.status.bolus_delivered == 20);
    CHECK(p.status.reservoir == 380);

    /* A tick that delivers no new pulse (an odd second) charges nothing. */
    medtrum_patch_tick(&p, 1041000);
    CHECK(p.status.reservoir == 380);
    medtrum_patch_tick(&p, 1042000); /* +1.2 s: pulse 21 */
    CHECK(p.status.bolus_delivered == 21);
    CHECK(p.status.reservoir == 379);

    /* The whole run still costs exactly 80 pulses. */
    tick_to(&p, 1042000, 1200000, 10000);
    CHECK(p.status.bolus_delivered == 80);
    CHECK(p.status.reservoir == 320);
}

/* A bolus larger than the reservoir empties it, clamps at 0, and stops: a
 * patch with nothing left reports empty rather than delivering air. */
static void test_bolus_larger_than_reservoir(void)
{
    medtrum_patch_t p;
    medtrum_patch_init(&p, (uint8_t[]){1, 2, 3, 4});
    medtrum_patch_set_state(&p, MEDTRUM_STATE_ACTIVE);
    medtrum_patch_set_reservoir(&p, 20); /* 1.0 U */
    set_bolus(&p, 200);                  /* 10.0 U */

    medtrum_patch_tick(&p, 1000000); /* records the clock */
    tick_to(&p, 1000000, 1400000, 10000);

    CHECK(p.status.reservoir == 0);
    CHECK(p.delivery.bolus_active == false);
    CHECK(p.status.bolus_delivered == 20); /* only what was left */

    /* Further ticks cannot go below zero or revive the bolus. */
    medtrum_patch_tick(&p, 1400000 + 3600000);
    CHECK(p.status.reservoir == 0);
    CHECK(p.status.bolus_delivered == 20);
    CHECK(p.delivery.bolus_active == false);
}

/* A bolus that exactly drains the reservoir is still a completed bolus. */
static void test_exact_drain_completes(void)
{
    medtrum_patch_t p;
    medtrum_patch_init(&p, (uint8_t[]){1, 2, 3, 4});
    medtrum_patch_set_state(&p, MEDTRUM_STATE_ACTIVE);
    medtrum_patch_set_reservoir(&p, 40);
    set_bolus(&p, 40);

    medtrum_patch_tick(&p, 1000000);
    tick_to(&p, 1000000, 1080000, 10000);

    CHECK(p.status.reservoir == 0);
    CHECK(p.status.bolus_delivered == 40);
    CHECK(p.status.bolus_completed);
    CHECK(p.delivery.bolus_active == false);
}

/* A tick with no bolus running changes nothing in the reservoir. */
static void test_unrelated_tick(void)
{
    medtrum_patch_t p;
    medtrum_patch_init(&p, (uint8_t[]){1, 2, 3, 4});
    medtrum_patch_set_state(&p, MEDTRUM_STATE_ACTIVE);
    medtrum_patch_set_reservoir(&p, 100); /* 5.0 U */

    CHECK(!p.delivery.bolus_active);
    medtrum_patch_tick(&p, 1000000); /* records the clock */
    tick_to(&p, 1000000, 4600000, 60000);

    CHECK(p.status.reservoir == 100);
    CHECK(p.status.bolus_delivered == 0);
    CHECK(!p.status.bolus_completed);
    CHECK(!p.delivery.bolus_active);
}

int main(void)
{
    test_four_unit_bolus();
    test_two_ticks_no_double_count();
    test_bolus_larger_than_reservoir();
    test_exact_drain_completes();
    test_unrelated_tick();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}