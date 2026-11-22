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

/* A one-entry schedule of `rate` pulses of 0.05 U per hour from minute 0, set
 * the way the controller does: SET_BASAL_PROFILE. */
static void set_scheduled_basal(medtrum_patch_t *p, uint16_t rate)
{
    uint32_t v = (uint32_t)rate << 12; /* one entry, starting at minute 0 */
    uint8_t content[5] = {MEDTRUM_BASAL_STANDARD, 1, (uint8_t)v, (uint8_t)(v >> 8),
                          (uint8_t)(v >> 16)};
    medtrum_frag_t frags[MEDTRUM_FRAGS_MAX];
    int n = medtrum_patch_command(p, MEDTRUM_CMD_SET_BASAL_PROFILE, 0, content, sizeof content, frags,
                                  MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(p->delivery.scheduled_rate == rate);
    CHECK(p->status.basal_delivery == 0);
}

/* An absolute temp basal at `rate` pulses of 0.05 U per hour for `minutes`. */
static void set_temp_basal(medtrum_patch_t *p, uint16_t rate, uint16_t minutes)
{
    uint8_t content[5] = {MEDTRUM_BASAL_ABSOLUTE_TEMP, (uint8_t)rate, (uint8_t)(rate >> 8),
                          (uint8_t)minutes, (uint8_t)(minutes >> 8)};
    medtrum_frag_t frags[MEDTRUM_FRAGS_MAX];
    int n = medtrum_patch_command(p, MEDTRUM_CMD_SET_TEMP_BASAL, 0, content, sizeof content, frags,
                                  MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(p->delivery.temp_rate == rate);
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

/* A scheduled basal of 1.0 U/h (20 pulses/h) draws one U in an hour: 20
 * pulses, whether the hour arrives in one tick or in sixty. */
static void test_scheduled_basal_hour(void)
{
    medtrum_patch_t p;
    medtrum_patch_init(&p, (uint8_t[]){1, 2, 3, 4});
    medtrum_patch_set_state(&p, MEDTRUM_STATE_ACTIVE);
    medtrum_patch_set_reservoir(&p, 400); /* 20.0 U */
    set_scheduled_basal(&p, 20);          /* 1.0 U/h */

    medtrum_patch_tick(&p, 1000000); /* records the clock */
    tick_to(&p, 1000000, 4600000, 60000);

    CHECK(p.delivery.patch_time == 3600);
    CHECK(p.status.reservoir == 380);   /* 20 pulses = 1.00 U, exact at 1 U/h */
    CHECK(p.status.basal_delivery == 20);

    /* The same hour in a single tick draws the same: the charge is a delta
     * from the segment start, so the tick rate cannot change it. */
    medtrum_patch_t q;
    medtrum_patch_init(&q, (uint8_t[]){1, 2, 3, 4});
    medtrum_patch_set_state(&q, MEDTRUM_STATE_ACTIVE);
    medtrum_patch_set_reservoir(&q, 400);
    set_scheduled_basal(&q, 20);
    medtrum_patch_tick(&q, 1000000);
    medtrum_patch_tick(&q, 4600000); /* +3600 s in one step */
    CHECK(q.status.reservoir == p.status.reservoir);
    CHECK(q.status.basal_delivery == p.status.basal_delivery);
}

/* A temp basal draws at the temp rate, and the schedule picks up at the
 * scheduled rate from the second the temp ends. */
static void test_temp_basal_rate(void)
{
    medtrum_patch_t p;
    medtrum_patch_init(&p, (uint8_t[]){1, 2, 3, 4});
    medtrum_patch_set_state(&p, MEDTRUM_STATE_ACTIVE);
    medtrum_patch_set_reservoir(&p, 400);
    set_scheduled_basal(&p, 20);            /* 1.0 U/h */
    set_temp_basal(&p, 40, 30);             /* 2.0 U/h for 30 min */

    medtrum_patch_tick(&p, 1000000); /* records the clock */
    tick_to(&p, 1000000, 2740000, 60000);
    CHECK(p.delivery.patch_time == 1740);
    /* 1740 s at 40 pulses/h = 19.3 pulses, so 19 whole pulses of 0.05 U. */
    CHECK(p.status.basal_delivery == 19);
    CHECK(p.status.reservoir == 381);

    medtrum_patch_tick(&p, 2800000); /* 1800 s: the temp ends */
    CHECK(p.delivery.temp_end == 0);
    CHECK(p.status.basal_type == MEDTRUM_BASAL_STANDARD);
    CHECK(p.status.reservoir == 380); /* 20 pulses = 1.00 U at 2 U/h for 0.5 h */
    CHECK(p.status.basal_delivery == 0); /* a fresh segment at the schedule */

    tick_to(&p, 2800000, 4600000, 60000); /* the next 30 min at 1 U/h */
    CHECK(p.delivery.patch_time == 3600);
    CHECK(p.status.basal_delivery == 10);
    CHECK(p.status.reservoir == 370);
}

/* Nothing draws while the patch is suspended, and resuming does not draw the
 * suspended hour after the fact. */
static void test_no_basal_while_suspended(void)
{
    medtrum_patch_t p;
    medtrum_patch_init(&p, (uint8_t[]){1, 2, 3, 4});
    medtrum_patch_set_state(&p, MEDTRUM_STATE_ACTIVE);
    medtrum_patch_set_reservoir(&p, 400);
    set_scheduled_basal(&p, 20); /* 1.0 U/h */

    medtrum_frag_t frags[MEDTRUM_FRAGS_MAX];
    uint8_t suspend[] = {3, 120};
    int n = medtrum_patch_command(&p, MEDTRUM_CMD_SUSPEND_PUMP, 0, suspend, sizeof suspend, frags,
                                  MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(p.status.state == MEDTRUM_STATE_SUSPENDED);

    medtrum_patch_tick(&p, 1000000); /* records the clock */
    tick_to(&p, 1000000, 4600000, 60000); /* an hour suspended */
    CHECK(p.delivery.patch_time == 3600);
    CHECK(p.status.reservoir == 400);
    CHECK(p.status.basal_delivery == 0);

    n = medtrum_patch_command(&p, MEDTRUM_CMD_RESUME_PUMP, 0, NULL, 0, frags, MEDTRUM_FRAGS_MAX);
    CHECK(n == 1);
    CHECK(p.status.state == MEDTRUM_STATE_ACTIVE);

    tick_to(&p, 4600000, 8200000, 60000); /* an hour at 1 U/h */
    CHECK(p.delivery.patch_time == 7200);
    CHECK(p.status.reservoir == 380); /* only the resumed hour */
    CHECK(p.status.basal_delivery == 20);
}

/* A basal that runs the reservoir dry clamps at 0, and a refill does not then
 * deliver the empty hours in one burst. */
static void test_basal_empties_reservoir(void)
{
    medtrum_patch_t p;
    medtrum_patch_init(&p, (uint8_t[]){1, 2, 3, 4});
    medtrum_patch_set_state(&p, MEDTRUM_STATE_ACTIVE);
    medtrum_patch_set_reservoir(&p, 5); /* 0.25 U: under half an hour at 1 U/h */
    set_scheduled_basal(&p, 20);

    medtrum_patch_tick(&p, 1000000);
    tick_to(&p, 1000000, 4600000, 300000); /* an hour at 1 U/h */
    CHECK(p.delivery.patch_time == 3600);
    CHECK(p.status.reservoir == 0);
    CHECK(p.status.basal_delivery == 0); /* no backlog kept while empty */

    /* The panel refills; the next hour draws its own 20 pulses only. */
    medtrum_patch_set_reservoir(&p, 100);
    tick_to(&p, 4600000, 8200000, 60000);
    CHECK(p.delivery.patch_time == 7200);
    CHECK(p.status.basal_delivery == 20);
    CHECK(p.status.reservoir == 80);
}

int main(void)
{
    test_four_unit_bolus();
    test_two_ticks_no_double_count();
    test_bolus_larger_than_reservoir();
    test_exact_drain_completes();
    test_unrelated_tick();
    test_scheduled_basal_hour();
    test_temp_basal_rate();
    test_no_basal_while_suspended();
    test_basal_empties_reservoir();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}