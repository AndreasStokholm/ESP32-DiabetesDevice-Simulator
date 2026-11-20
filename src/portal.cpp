#include "portal.h"

// Small, allocation-free string helpers: this file stays pure and links
// without libstdc++ so the host test can build it with plain cc.

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

// True when path is exactly want, or want followed by a query/fragment.
static int path_is(const char* path, const char* want) {
  for (; *want; path++, want++)
    if (*path != *want) return 0;
  return *path == 0 || *path == '?' || *path == '#';
}

// True when path starts with want and want ends on a component boundary.
static int path_under(const char* path, const char* want) {
  for (; *want; path++, want++)
    if (*path != *want) return 0;
  return *path == 0 || *path == '/' || *path == '?' || *path == '#';
}

static int method_is(const char* method, const char* want) {
  for (; *want; method++, want++)
    if (lower(*method) != lower(*want)) return 0;
  return *method == 0;
}

PortalAction portalDecide(const char* path, const char* method, int wifiSaved, int setupCookie) {
  if (!path) path = "";
  const int get = method && (method_is(method, "GET") || method_is(method, "HEAD"));

  // The OS probes only answer GET/HEAD. A POST to one of these paths falls
  // through to the normal app (which will 404 or handle it) instead of being
  // mistaken for a probe.
  if (get) {
    if (path_is(path, "/hotspot-detect.html")) return PORTAL_PROBE_APPLE_HTML;
    if (path_is(path, "/library/test/success.html")) return PORTAL_PROBE_APPLE_SUCCESS;
    if (path_is(path, "/generate_204") || path_is(path, "/gen_204")) return PORTAL_PROBE_ANDROID_204;
    if (path_is(path, "/connecttest.txt")) return PORTAL_PROBE_CONNECTTEST;
    if (path_is(path, "/ncsi.txt")) return PORTAL_PROBE_NCSI;
    if (path_under(path, "/fwlink")) return PORTAL_PROBE_FWLINK;

    if (path_is(path, "/setup")) return PORTAL_SERVE_SETUP;
    if (path_is(path, "/")) {
      // In AP mode the root is the sheet, until the reader has dismissed it.
      if (!wifiSaved && !setupCookie) return PORTAL_REDIRECT_SETUP;
      return PORTAL_SERVE_APP;
    }
    return PORTAL_SERVE_APP;
  }

  // A POST to /setup is never redirected: the sheet is served as-is. The
  // form's own endpoints (/setup/wifi, /setup/dismiss) are not /setup, so
  // they fall through to the app.
  if (path_is(path, "/setup")) return PORTAL_SERVE_SETUP;
  return PORTAL_SERVE_APP;
}

const char* portalActionName(PortalAction action) {
  switch (action) {
    case PORTAL_SERVE_SETUP: return "serve_setup";
    case PORTAL_REDIRECT_SETUP: return "redirect_setup";
    case PORTAL_PROBE_APPLE_HTML: return "probe_apple_html";
    case PORTAL_PROBE_APPLE_SUCCESS: return "probe_apple_success";
    case PORTAL_PROBE_ANDROID_204: return "probe_android_204";
    case PORTAL_PROBE_CONNECTTEST: return "probe_connecttest";
    case PORTAL_PROBE_NCSI: return "probe_ncsi";
    case PORTAL_PROBE_FWLINK: return "probe_fwlink";
    case PORTAL_SERVE_APP: return "serve_app";
  }
  return "unknown";
}
