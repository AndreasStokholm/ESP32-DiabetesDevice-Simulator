#include "../src/fail.h"
#include "test.h"

int failures, checks;

static void reset(void) { fail_clear_all(); }

static void test_defaults(void)
{
    reset();
    for (int i = 0; i < FAIL_DEVICE_COUNT; i++) {
        FailDevice d = (FailDevice)i;
        CHECK(fail_should_accept(d, 1000) == 1);
        CHECK(fail_notify_delay_ms(d) == 0);
        CHECK(fail_should_advertise(d, 1000) == 1);
        CHECK(fail_consume_message(d) == 0);
        FailState s = fail_get(d);
        CHECK(s.dropPending == 0);
        CHECK(s.refuseConnects == 0);
        CHECK(s.ignoreMessages == 0);
        CHECK(s.delayMs == 0);
        CHECK(s.quietUntilMs == 0);
    }
}

static void test_drop_once(void)
{
    reset();
    fail_set_drop(FAIL_POD, 1);
    CHECK(fail_should_accept(FAIL_POD, 100) == 0);
    CHECK(fail_get(FAIL_POD).dropPending == 0);
    CHECK(fail_should_accept(FAIL_POD, 100) == 1);
    CHECK(fail_should_accept(FAIL_POD, 100) == 1);

    /* Clearing in the other direction. */
    fail_set_drop(FAIL_POD, 1);
    fail_set_drop(FAIL_POD, 0);
    CHECK(fail_should_accept(FAIL_POD, 100) == 1);
}

static void test_refuse_then_accept(void)
{
    reset();
    fail_set_refuse(FAIL_MEDTRUM, 3);
    CHECK(fail_should_accept(FAIL_MEDTRUM, 100) == 0);
    CHECK(fail_should_accept(FAIL_MEDTRUM, 100) == 0);
    CHECK(fail_should_accept(FAIL_MEDTRUM, 100) == 0);
    CHECK(fail_get(FAIL_MEDTRUM).refuseConnects == 0);
    CHECK(fail_should_accept(FAIL_MEDTRUM, 100) == 1);
    CHECK(fail_should_accept(FAIL_MEDTRUM, 100) == 1);

    /* Dropping once comes before the refusal count. */
    reset();
    fail_set_drop(FAIL_G7, 1);
    fail_set_refuse(FAIL_G7, 1);
    CHECK(fail_should_accept(FAIL_G7, 0) == 0);  /* the drop */
    CHECK(fail_should_accept(FAIL_G7, 0) == 0);  /* the refusal */
    CHECK(fail_should_accept(FAIL_G7, 0) == 1);
}

static void test_quiet_window(void)
{
    reset();
    fail_set_quiet_for(FAIL_G7, 1000, 5000);
    CHECK(fail_get(FAIL_G7).quietUntilMs == 6000);
    CHECK(fail_should_advertise(FAIL_G7, 4999) == 0);
    CHECK(fail_should_advertise(FAIL_G7, 5000) == 0);
    CHECK(fail_should_advertise(FAIL_G7, 5999) == 0);
    CHECK(fail_should_advertise(FAIL_G7, 6000) == 1);
    CHECK(fail_should_advertise(FAIL_G7, 9000) == 1);

    /* Zero clears the window; the other devices are untouched. */
    fail_set_quiet_for(FAIL_G7, 0, 5000);
    CHECK(fail_should_advertise(FAIL_G7, 5000) == 1);
    fail_set_quiet_for(FAIL_POD, 200, 0);
    CHECK(fail_should_advertise(FAIL_POD, 100) == 0);
    CHECK(fail_should_advertise(FAIL_POD, 200) == 1);
    CHECK(fail_should_advertise(FAIL_G7, 100) == 1);
}

static void test_delay(void)
{
    reset();
    fail_set_delay(FAIL_POD, 250);
    CHECK(fail_notify_delay_ms(FAIL_POD) == 250);
    CHECK(fail_decide(FAIL_POD, FAIL_NOTIFY_DELAY_MS, 0) == 250);
    CHECK(fail_notify_delay_ms(FAIL_G7) == 0);
    fail_set_delay(FAIL_POD, 0);
    CHECK(fail_notify_delay_ms(FAIL_POD) == 0);
}

static void test_ignore(void)
{
    reset();
    fail_set_ignore(FAIL_POD, 2);
    CHECK(fail_consume_message(FAIL_POD) == 1);
    CHECK(fail_consume_message(FAIL_POD) == 1);
    CHECK(fail_get(FAIL_POD).ignoreMessages == 0);
    CHECK(fail_consume_message(FAIL_POD) == 0);
    CHECK(fail_consume_message(FAIL_POD) == 0);

    /* Per-device counters. */
    fail_set_ignore(FAIL_G7, 1);
    fail_set_ignore(FAIL_MEDTRUM, 1);
    CHECK(fail_consume_message(FAIL_G7) == 1);
    CHECK(fail_consume_message(FAIL_G7) == 0);
    CHECK(fail_consume_message(FAIL_MEDTRUM) == 1);
    CHECK(fail_consume_message(FAIL_MEDTRUM) == 0);
}

static void test_clear(void)
{
    reset();
    fail_set_drop(FAIL_POD, 1);
    fail_set_refuse(FAIL_POD, 4);
    fail_set_quiet_for(FAIL_POD, 500, 0);
    fail_set_delay(FAIL_POD, 90);
    fail_set_ignore(FAIL_POD, 7);
    fail_clear(FAIL_POD);
    FailState s = fail_get(FAIL_POD);
    CHECK(s.dropPending == 0 && s.refuseConnects == 0 && s.quietUntilMs == 0 &&
          s.delayMs == 0 && s.ignoreMessages == 0);

    for (int i = 0; i < FAIL_DEVICE_COUNT; i++) {
        fail_set_drop((FailDevice)i, 1);
        fail_set_delay((FailDevice)i, 10);
    }
    fail_clear_all();
    for (int i = 0; i < FAIL_DEVICE_COUNT; i++) {
        FailState c = fail_get((FailDevice)i);
        CHECK(c.dropPending == 0 && c.delayMs == 0);
    }
}

static void test_names(void)
{
    CHECK(fail_device_from_name("pod") == FAIL_POD);
    CHECK(fail_device_from_name("g7") == FAIL_G7);
    CHECK(fail_device_from_name("medtrum") == FAIL_MEDTRUM);
    CHECK(fail_device_from_name("all") == -1);
    CHECK(fail_device_from_name("POD") == -1);
    CHECK(fail_device_from_name("") == -1);
    CHECK(fail_device_from_name(0) == -1);

    CHECK(strcmp(fail_device_name(FAIL_POD), "pod") == 0);
    CHECK(strcmp(fail_device_name(FAIL_G7), "g7") == 0);
    CHECK(strcmp(fail_device_name(FAIL_MEDTRUM), "medtrum") == 0);
    CHECK(strcmp(fail_device_name((FailDevice)99), "unknown") == 0);
}

static void test_invalid_device_is_harmless(void)
{
    reset();
    CHECK(fail_should_accept((FailDevice)99, 0) == 1);
    CHECK(fail_should_advertise((FailDevice)99, 0) == 1);
    CHECK(fail_notify_delay_ms((FailDevice)99) == 0);
    CHECK(fail_consume_message((FailDevice)99) == 0);
    fail_set_drop((FailDevice)99, 1);
    fail_set_delay((FailDevice)99, 5);
    CHECK(fail_get((FailDevice)99).dropPending == 0);
}

int main(void)
{
    test_defaults();
    test_drop_once();
    test_refuse_then_accept();
    test_quiet_window();
    test_delay();
    test_ignore();
    test_clear();
    test_names();
    test_invalid_device_is_harmless();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
