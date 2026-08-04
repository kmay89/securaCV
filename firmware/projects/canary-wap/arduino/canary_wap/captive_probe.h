/*
 * SecuraCV Canary WAP — Captive-portal connectivity-probe response policy
 *
 * Pure, Arduino-free logic split out of canary_wap.ino so it can be
 * unit-tested on the host (see tests_host/test_captive_probe.cpp). The
 * firmware glue (httpd_resp_* calls and serving the instruction HTML) stays in
 * the .ino and calls respond() here — the same split as captive_dns.h.
 *
 * The OS connectivity probes decide whether a phone *stays* on our setup AP,
 * and the right answer is platform-specific (the "hybrid" strategy):
 *
 *   - Apple   (/hotspot-detect.html, /library/test/success.html): 200 + the
 *     instruction HTML page, NOT Apple's "<TITLE>Success</TITLE>" token. That
 *     pops the Captive Network Assistant sheet (which keeps the Wi-Fi
 *     association up) and points the user at canary.local, instead of marking
 *     the network "online" and closing the sheet.
 *   - Android (/generate_204, /gen_204): 204 No Content, so Android marks the
 *     AP validated/online and never shows the no-internet sheet or falls back
 *     to cellular (the disconnect that stranded users before they reached
 *     canary.local).
 *   - Windows (/connecttest.txt, /ncsi.txt): the exact NCSI success bodies, so
 *     Windows keeps the adapter on the AP instead of flagging "No Internet".
 *
 * See firmware/LESSONS_LEARNED.md, "Networking & Captive Portal".
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_CAPTIVE_PROBE_H
#define SECURACV_CAPTIVE_PROBE_H

#include <cstring>

namespace captive_probe {

// Exact bodies Windows' Network Connectivity Status Indicator expects.
static constexpr const char* NCSI_NCSI_BODY        = "Microsoft NCSI";
static constexpr const char* NCSI_CONNECTTEST_BODY = "Microsoft Connect Test";

// Content types for the bodied responses (Android's 204 carries none).
static constexpr const char* CT_HTML  = "text/html; charset=utf-8";
static constexpr const char* CT_PLAIN = "text/plain";

enum class ProbeKind {
  None,                  // not a recognized probe path
  AppleInstructionPage,  // 200 text/html — firmware serves CAPTIVE_PORTAL_HTML
  AndroidNoContent,      // 204, empty body
  WindowsNcsiBody,       // 200 text/plain — `body` holds the NCSI string
};

struct ProbeResponse {
  ProbeKind   kind;
  const char* content_type;  // nullptr for the 204 (Android) case
  const char* body;          // NCSI string for Windows; nullptr otherwise
};

// True when the path component of `path` — everything before a '?' query or
// '#' fragment, or the whole string if neither is present — exactly equals
// `lit`. Probe URLs sometimes carry a cache-busting query (e.g.
// /generate_204?ts=...), and req->uri includes it, so an exact strcmp would
// misroute those; matching the path component keeps them on the right
// per-platform response (the behavior the old unconditional handlers had).
inline bool path_is(const char* path, const char* lit) {
  if (path == nullptr || lit == nullptr) return false;
  size_t i = 0;
  for (; lit[i] != '\0'; ++i) {
    if (path[i] != lit[i]) return false;  // diverged (also catches path ending early)
  }
  return path[i] == '\0' || path[i] == '?' || path[i] == '#';
}

inline bool path_contains(const char* path, const char* needle) {
  return path != nullptr && std::strstr(path, needle) != nullptr;
}

// The Windows NCSI body for a probe path: "Microsoft NCSI" for ncsi.txt,
// "Microsoft Connect Test" otherwise (connecttest.txt). Mirrors the firmware's
// strstr(uri, "ncsi") branch exactly (substring, so a query string is fine).
inline const char* windows_ncsi_body(const char* path) {
  return path_contains(path, "ncsi") ? NCSI_NCSI_BODY : NCSI_CONNECTTEST_BODY;
}

// Classify a request path into the probe platform whose connectivity check it
// is. Recognizes the exact probe URIs the firmware registers — matching the
// path component, so a trailing query string (req->uri keeps it) still routes
// to the right platform; anything else → None.
inline ProbeKind classify(const char* path) {
  if (path_is(path, "/hotspot-detect.html") ||
      path_is(path, "/library/test/success.html")) {
    return ProbeKind::AppleInstructionPage;
  }
  if (path_is(path, "/generate_204") || path_is(path, "/gen_204")) {
    return ProbeKind::AndroidNoContent;
  }
  if (path_is(path, "/connecttest.txt") || path_is(path, "/ncsi.txt")) {
    return ProbeKind::WindowsNcsiBody;
  }
  return ProbeKind::None;
}

// Full response descriptor for a probe path. The firmware reads `kind` to pick
// the status line, sets `content_type` (when non-null), and sends `body`
// (Windows), CAPTIVE_PORTAL_HTML (Apple), or nothing (Android 204).
//
// Unrecognized paths fall back to the Apple instruction page: a 200 HTML page
// is the connection-preserving safe default. In the firmware this never fires
// — only the six exact probe URIs are routed here; everything else hits the
// HTTPS redirect — but the fallback keeps a misregistration from 404-ing a
// probe and tripping a disconnect.
inline ProbeResponse respond(const char* path) {
  switch (classify(path)) {
    case ProbeKind::AndroidNoContent:
      return { ProbeKind::AndroidNoContent, nullptr, nullptr };
    case ProbeKind::WindowsNcsiBody:
      return { ProbeKind::WindowsNcsiBody, CT_PLAIN, windows_ncsi_body(path) };
    case ProbeKind::AppleInstructionPage:
    case ProbeKind::None:
    default:
      return { ProbeKind::AppleInstructionPage, CT_HTML, nullptr };
  }
}

}  // namespace captive_probe

#endif /* SECURACV_CAPTIVE_PROBE_H */
