/*
 * SecuraCV Canary WAP — help QR verdict → Help Desk URL (host-testable)
 *
 * Arduino-free: string.h/stdio.h only. The dashboard's "Help QR" turns the
 * device's own current verdict into a QR a phone scans to land on the exact
 * fix at the website's Help Desk — no typing, no transcription, no guessing.
 * Design doc: docs/design/automated_help_desk.md (Phase 2, the help QR).
 *
 * TWO PROBE-ID NAMESPACES EXIST, AND THIS HEADER IS THE BRIDGE. The WAP's
 * self-test speaks its own ids ("wifi", "sd", "bluetooth", ... —
 * selftest_api.h); the website's #probe-<id> chips render the KERNEL
 * diagnostics namespace mirrored in onboarding-spec.json ("wifi_ok",
 * "sd_card", ...), and its #s-<id> cards are the symptom catalog
 * (securacv_website js/help-catalog.js). The two lists both having ten
 * entries is exactly how they get conflated — so the mapping is explicit,
 * per probe, and pinned by tests_host/test_help_qr_logic.cpp.
 *
 * PRIVACY: the composed URL carries only a coarse verdict anchor — the same
 * information GET /api/selftest already serves unauthenticated on the AP
 * (the AP is the boundary; selftest_api.h:653). Never a device id, never a
 * network name, never a token. A URL is forever once scanned; keep it inert.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_HELP_QR_LOGIC_H
#define SECURACV_HELP_QR_LOGIC_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace help_qr_logic {

// Map one canary-wap self-test probe id (selftest_api.h namespace) to the
// Help Desk anchor that fixes it. "" = no dedicated anchor — the caller
// falls back to the bare Help Desk, whose symptom search is still not a
// dead end. Only probes that CAN fail need rows: the optional peripherals
// (gps/power/microphone/buzzer/tamper) never FAIL by contract
// (selftest_api.h — PASS/SKIP/ABSENT only), so they never reach this table.
inline const char* anchor_for_probe(const char* probe_id) {
  if (!probe_id) return "";
  struct Row { const char* probe; const char* anchor; };
  static constexpr Row MAP[] = {
    { "wifi",      "probe-wifi_ok" },      // the kernel wifi probe's recovery steps fit
    { "sd",        "probe-sd_card" },      // reseat/replace-card steps
    { "bluetooth", "s-ble-not-working" },  // the BLE ladder + the full-build truth
    // "camera" and "gpio" can fail but have no dedicated page anchor yet;
    // they deliberately fall through to the bare Help Desk rather than
    // landing on a wrong-but-plausible fix.
  };
  for (const Row& r : MAP) {
    if (strcmp(probe_id, r.probe) == 0) return r.anchor;
  }
  return "";
}

// Compose the Help Desk URL for the device's current verdict, worst first:
// safe mode explains everything else (a recovering device shows a wall of
// SKIP rows that reads as mass failure), a down hub explains stale/quiet
// symptoms, then the first failing probe with a page anchor. All-clear —
// or nothing mapped — composes the bare Help Desk.
//
// base is a parameter so tests pin composition, not a constant. Returns
// chars written (excluding NUL); 0 when out is too small, with out[0]
// NUL'd — never a truncated URL, because half a URL in a QR scans to a
// 404 on the exact device that needs help.
inline size_t compose_help_url(char* out, size_t cap, const char* base,
                               bool safe_mode, bool hub_down,
                               const char* const* failing,
                               size_t failing_count) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!base || base[0] == '\0') return 0;

  const char* anchor = "";
  if (safe_mode) {
    anchor = "s-safe-mode";
  } else if (hub_down) {
    anchor = "s-hub-unreachable";
  } else {
    for (size_t i = 0; i < failing_count; ++i) {
      const char* a = anchor_for_probe(failing ? failing[i] : nullptr);
      if (a[0] != '\0') { anchor = a; break; }
    }
  }

  int n = (anchor[0] != '\0') ? snprintf(out, cap, "%s#%s", base, anchor)
                              : snprintf(out, cap, "%s", base);
  if (n <= 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}

}  // namespace help_qr_logic

#endif  // SECURACV_HELP_QR_LOGIC_H
