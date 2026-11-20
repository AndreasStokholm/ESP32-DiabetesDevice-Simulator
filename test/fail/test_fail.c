#include "../src/fail.h"
#include "test.h"

int failures, checks;

static int accept(FailDevice d, int64_t now) { return (int)fail_decide(d, FAIL_ACCEPT, now); }
static unsigned delay_ms(FailDevice d) { return (unsigned)fail_decide(d, FAIL_NOTIFY_DELAY_MS, 0); }
static int advertise(FailDevice d, int64_t now) { return (int)fail_decide(d, FAIL_ADVERTISE, now); }
static int consume(FailDevice d) { return (int)fail_decide(d, FAIL_CONSUME_MESSAGE, 0); }

static FailRequest absent(void) {
    FailRequest r = {FAIL_ABSENT, FAIL_ABSENT, FAIL_ABSENT, FAIL_ABSENT, FAIL_ABSENT};
    return r;
}

static void reset(void) { fail_clear_all(); }

static void test_defaults(void)
{
    reset();
    for (int i = 0; i < FAIL_DEVICE_COUNT; i++) {
        FailDevice d = (FailDevice)i;
        CHECK(accept(d, 1000) == 1);
        CHECK(delay_ms(d) == 0);
        CHECK(advertise(d, 1000) == 1);
        CHECK(consume(d) == 0);
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
    CHECK(accept(FAIL_POD, 100) == 0);
    CHECK(fail_get(FAIL_POD).dropPending == 0);
    CHECK(accept(FAIL_POD, 100) == 1);
    CHECK(accept(FAIL_POD, 100) == 1);

    fail_set_drop(FAIL_POD, 1);
    fail_set_drop(FAIL_POD, 0);
    CHECK(accept(FAIL_POD, 100) == 1);
}

static void test_refuse_then_accept(void)
{
    reset();
    fail_set_refuse(FAIL_MEDTRUM, 3);
    CHECK(accept(FAIL_MEDTRUM, 100) == 0);
    CHECK(accept(FAIL_MEDTRUM, 100) == 0);
    CHECK(accept(FAIL_MEDTRUM, 100) == 0);
    CHECK(fail_get(FAIL_MEDTRUM).refuseConnects == 0);
    CHECK(accept(FAIL_MEDTRUM, 100) == 1);
    CHECK(accept(FAIL_MEDTRUM, 100) == 1);

    /* Dropping once comes before the refusal count. */
    reset();
    fail_set_drop(FAIL_G7, 1);
    fail_set_refuse(FAIL_G7, 1);
    CHECK(accept(FAIL_G7, 0) == 0);  /* the drop */
    CHECK(accept(FAIL_G7, 0) == 0);  /* the refusal */
    CHECK(accept(FAIL_G7, 0) == 1);
}

static void test_quiet_window(void)
{
    reset();
    fail_set_quiet_for(FAIL_G7, 1000, 5000);
    CHECK(fail_get(FAIL_G7).quietUntilMs == 6000);
    CHECK(advertise(FAIL_G7, 4999) == 0);
    CHECK(advertise(FAIL_G7, 5000) == 0);
    CHECK(advertise(FAIL_G7, 5999) == 0);
    CHECK(advertise(FAIL_G7, 6000) == 1);
    CHECK(advertise(FAIL_G7, 9000) == 1);

    fail_set_quiet_for(FAIL_G7, 0, 5000);
    CHECK(advertise(FAIL_G7, 5000) == 1);
    fail_set_quiet_for(FAIL_POD, 200, 0);
    CHECK(advertise(FAIL_POD, 100) == 0);
    CHECK(advertise(FAIL_POD, 200) == 1);
    CHECK(advertise(FAIL_G7, 100) == 1);
}

static void test_delay(void)
{
    reset();
    fail_set_delay(FAIL_POD, 250);
    CHECK(delay_ms(FAIL_POD) == 250);
    CHECK(fail_decide(FAIL_POD, FAIL_NOTIFY_DELAY_MS, 0) == 250);
    CHECK(delay_ms(FAIL_G7) == 0);
    fail_set_delay(FAIL_POD, 0);
    CHECK(delay_ms(FAIL_POD) == 0);
}

static void test_ignore(void)
{
    reset();
    fail_set_ignore(FAIL_POD, 2);
    CHECK(consume(FAIL_POD) == 1);
    CHECK(consume(FAIL_POD) == 1);
    CHECK(fail_get(FAIL_POD).ignoreMessages == 0);
    CHECK(consume(FAIL_POD) == 0);
    CHECK(consume(FAIL_POD) == 0);

    fail_set_ignore(FAIL_G7, 1);
    fail_set_ignore(FAIL_MEDTRUM, 1);
    CHECK(consume(FAIL_G7) == 1);
    CHECK(consume(FAIL_G7) == 0);
    CHECK(consume(FAIL_MEDTRUM) == 1);
    CHECK(consume(FAIL_MEDTRUM) == 0);
}

static void test_apply(void)
{
    reset();
    FailRequest r = absent();
    r.drop = 1;
    r.refuse = 2;
    r.quietMs = 1000;
    r.delayMs = 250;
    r.ignore = 2;
    CHECK(fail_apply(FAIL_POD, &r, 5000) == 0);
    FailState s = fail_get(FAIL_POD);
    CHECK(s.dropPending == 1);
    CHECK(s.refuseConnects == 2);
    CHECK(s.quietUntilMs == 6000);
    CHECK(s.delayMs == 250);
    CHECK(s.ignoreMessages == 2);
    CHECK(fail_get(FAIL_G7).delayMs == 0);

    /* Absent fields are left alone. */
    FailRequest keep = absent();
    CHECK(fail_apply(FAIL_POD, &keep, 9000) == 0);
    s = fail_get(FAIL_POD);
    CHECK(s.quietUntilMs == 6000);
    CHECK(s.delayMs == 250);
    CHECK(s.refuseConnects == 2);

    /* Zero clears; drop uses 0/1. */
    FailRequest clear = absent();
    clear.drop = 0;
    clear.refuse = 0;
    clear.quietMs = 0;
    clear.delayMs = 0;
    clear.ignore = 0;
    CHECK(fail_apply(FAIL_POD, &clear, 9000) == 0);
    s = fail_get(FAIL_POD);
    CHECK(s.dropPending == 0 && s.refuseConnects == 0 && s.quietUntilMs == 0 &&
          s.delayMs == 0 && s.ignoreMessages == 0);

    /* Boundaries are accepted. */
    FailRequest edge = absent();
    edge.refuse = 1000;
    edge.quietMs = 86400000;
    edge.delayMs = 60000;
    edge.ignore = 1000;
    CHECK(fail_apply(FAIL_G7, &edge, 0) == 0);

    /* Each range is rejected, and a rejected request changes nothing. */
    fail_set_delay(FAIL_G7, 10);
    FailRequest bad = absent();
    bad.drop = 2;
    CHECK(fail_apply(FAIL_G7, &bad, 0) != 0);
    bad = absent();
    bad.refuse = 1001;
    CHECK(strcmp(fail_apply(FAIL_G7, &bad, 0), "refuse is 0-1000 connects") == 0);
    bad = absent();
    bad.refuse = -2;
    CHECK(fail_apply(FAIL_G7, &bad, 0) != 0);
    bad = absent();
    bad.quietMs = 86400001;
    CHECK(fail_apply(FAIL_G7, &bad, 0) != 0);
    bad = absent();
    bad.delayMs = 60001;
    CHECK(fail_apply(FAIL_G7, &bad, 0) != 0);
    bad = absent();
    bad.ignore = 1001;
    CHECK(fail_apply(FAIL_G7, &bad, 0) != 0);
    bad = absent();
    bad.delayMs = 999;
    bad.ignore = 1001;
    CHECK(fail_apply(FAIL_G7, &bad, 0) != 0);
    CHECK(fail_get(FAIL_G7).delayMs == 10);

    /* Invalid devices and a missing request are rejected. */
    FailRequest ok = absent();
    ok.ignore = 1;
    CHECK(fail_apply((FailDevice)99, &ok, 0) != 0);
    CHECK(fail_apply(FAIL_POD, NULL, 0) != 0);
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
    CHECK(accept((FailDevice)99, 0) == 1);
    CHECK(advertise((FailDevice)99, 0) == 1);
    CHECK(delay_ms((FailDevice)99) == 0);
    CHECK(consume((FailDevice)99) == 0);
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
    test_apply();
    test_clear();
    test_names();
    test_invalid_device_is_harmless();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
