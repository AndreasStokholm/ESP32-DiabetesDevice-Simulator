#include "../src/portal.h"
#include "test.h"

int failures, checks;

static void want(PortalAction expected, const char *path, const char *method,
                 int wifiSaved, int setupCookie)
{
    PortalAction got = portalDecide(path, method, wifiSaved, setupCookie);
    checks++;
    if (got != expected) {
        failures++;
        fprintf(stderr,
                "FAIL %s %s (wifiSaved=%d cookie=%d): got %s, want %s\n",
                method, path, wifiSaved, setupCookie,
                portalActionName(got), portalActionName(expected));
    }
}

static void test_apple(void)
{
    want(PORTAL_PROBE_APPLE_HTML, "/hotspot-detect.html", "GET", 0, 0);
    want(PORTAL_PROBE_APPLE_HTML, "/hotspot-detect.html", "GET", 1, 0);
    want(PORTAL_PROBE_APPLE_HTML, "/hotspot-detect.html?x=1", "GET", 0, 0);
    want(PORTAL_PROBE_APPLE_SUCCESS, "/library/test/success.html", "GET", 0, 0);
    want(PORTAL_PROBE_APPLE_SUCCESS, "/library/test/success.html?cache=1", "GET", 1, 0);
}

static void test_android(void)
{
    want(PORTAL_PROBE_ANDROID_204, "/generate_204", "GET", 0, 0);
    want(PORTAL_PROBE_ANDROID_204, "/gen_204", "GET", 0, 0);
    want(PORTAL_PROBE_ANDROID_204, "/generate_204?ts=42", "GET", 1, 0);
}

static void test_windows(void)
{
    want(PORTAL_PROBE_CONNECTTEST, "/connecttest.txt", "GET", 0, 0);
    want(PORTAL_PROBE_NCSI, "/ncsi.txt", "GET", 0, 0);
    want(PORTAL_PROBE_FWLINK, "/fwlink/", "GET", 0, 0);
    want(PORTAL_PROBE_FWLINK, "/fwlink/?LinkId=123456", "GET", 0, 0);
    want(PORTAL_PROBE_FWLINK, "/fwlink/?LinkId=123456", "GET", 1, 0);
    /* A path that merely starts with the same letters is not the probe. */
    want(PORTAL_SERVE_APP, "/fwlinker", "GET", 0, 0);
}

static void test_root_and_cookie(void)
{
    /* AP mode: the root is the sheet until dismissed. */
    want(PORTAL_REDIRECT_SETUP, "/", "GET", 0, 0);
    want(PORTAL_REDIRECT_SETUP, "/?from=qr", "GET", 0, 0);
    want(PORTAL_REDIRECT_SETUP, "/", "HEAD", 0, 0);

    /* The cookie dismisses the sheet for good. */
    want(PORTAL_SERVE_APP, "/", "GET", 0, 1);

    /* With a network the board serves the app, cookie or not. */
    want(PORTAL_SERVE_APP, "/", "GET", 1, 0);
    want(PORTAL_SERVE_APP, "/", "GET", 1, 1);

    /* A POST to the root is never redirected. */
    want(PORTAL_SERVE_APP, "/", "POST", 0, 0);
}

static void test_setup(void)
{
    /* /setup is always the sheet, in AP mode and on a joined network. */
    want(PORTAL_SERVE_SETUP, "/setup", "GET", 0, 0);
    want(PORTAL_SERVE_SETUP, "/setup", "GET", 0, 1);
    want(PORTAL_SERVE_SETUP, "/setup", "GET", 1, 0);
    want(PORTAL_SERVE_SETUP, "/setup?x=1", "GET", 0, 0);

    /* Critically: a POST to /setup is never redirected, cookie or not. */
    want(PORTAL_SERVE_SETUP, "/setup", "POST", 0, 0);
    want(PORTAL_SERVE_SETUP, "/setup", "POST", 0, 1);

    /* The form's own endpoints are app routes, never the sheet or a redirect. */
    want(PORTAL_SERVE_APP, "/setup/wifi", "POST", 0, 0);
    want(PORTAL_SERVE_APP, "/setup/wifi", "POST", 0, 1);
    want(PORTAL_SERVE_APP, "/setup/dismiss", "POST", 0, 0);
}

static void test_app(void)
{
    want(PORTAL_SERVE_APP, "/g7", "GET", 0, 0);
    want(PORTAL_SERVE_APP, "/pod", "GET", 0, 1);
    want(PORTAL_SERVE_APP, "/settings", "GET", 1, 0);
    want(PORTAL_SERVE_APP, "/status", "GET", 0, 0);
    want(PORTAL_SERVE_APP, "/new", "POST", 0, 0);
    want(PORTAL_SERVE_APP, "/glucose", "POST", 1, 1);
    want(PORTAL_SERVE_APP, "", "GET", 0, 0);
}

static void test_methods(void)
{
    /* Probes answer GET/HEAD only; other methods are ordinary requests. */
    want(PORTAL_SERVE_APP, "/generate_204", "POST", 0, 0);
    want(PORTAL_SERVE_APP, "/hotspot-detect.html", "POST", 0, 0);
    want(PORTAL_PROBE_APPLE_SUCCESS, "/library/test/success.html", "head", 0, 0);
    want(PORTAL_PROBE_ANDROID_204, "/generate_204", "get", 0, 0);
}

static void test_robustness(void)
{
    /* Missing method and missing path must not crash; they are plain requests. */
    CHECK(portalDecide("/", NULL, 0, 0) == PORTAL_SERVE_APP);
    CHECK(portalDecide(NULL, "GET", 0, 0) == PORTAL_SERVE_APP);
    CHECK(portalDecide(NULL, NULL, 0, 0) == PORTAL_SERVE_APP);

    /* Every action has a distinct, non-empty name. */
    static const PortalAction all[] = {
        PORTAL_SERVE_SETUP, PORTAL_REDIRECT_SETUP, PORTAL_PROBE_APPLE_HTML,
        PORTAL_PROBE_APPLE_SUCCESS, PORTAL_PROBE_ANDROID_204,
        PORTAL_PROBE_CONNECTTEST, PORTAL_PROBE_NCSI, PORTAL_PROBE_FWLINK,
        PORTAL_SERVE_APP};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        const char *name = portalActionName(all[i]);
        CHECK(name && name[0]);
        for (size_t j = 0; j < i; j++)
            CHECK(strcmp(name, portalActionName(all[j])) != 0);
    }
    CHECK(strcmp(portalActionName(PORTAL_SERVE_APP), "serve_app") == 0);
}

int main(void)
{
    test_apple();
    test_android();
    test_windows();
    test_root_and_cookie();
    test_setup();
    test_app();
    test_methods();
    test_robustness();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
