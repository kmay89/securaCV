// include/canary/ui/help_verdict.h — the glass's help-QR verdict (pure)
//
// Arduino-free, LVGL-free: string.h/stdio.h only, host-tested in
// tests_host/test_help_verdict.cpp. The Settings "get help" page turns the
// display's own current verdict into a Help Desk deep link a phone scans —
// the sibling of the WAP's help_qr_logic.h (canary-wap), with the display's
// own verdict inputs: this device has no probe self-test; what it knows is
// its hub link and, via the fleet model, whether witnesses have gone quiet
// or failed verification. Design doc: docs/design/automated_help_desk.md.
//
// PRIVACY: the composed URL carries only a coarse verdict anchor — never a
// device id, a witness name, a network name, or a key. A QR is forever
// once scanned; keep it inert. The anchors are the website's own
// #s-<symptom> ids (securacv_website js/help-catalog.js), pinned by the
// host test's charset check.
//
// Copyright (c) 2026 ERRERlabs / Karl May
// License: Apache-2.0
#pragma once

#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace canary::ui::help_verdict {

// Worst-first anchor for what the glass can see. Precedence:
//   1. a verification failure (a Failed badge) — the one state worth the
//      owner's attention over everything else on a witness display;
//   2. a hub that is configured but unreachable — explains stale rows too;
//   3. a witness gone Stale/Lost while the hub is fine — the quiet-Canary
//      story is then about that Canary, not the hub.
// A display nobody has pointed at a hub yet (placeholder broker) is not
// broken — it gets the bare Help Desk, same as all-well. "" = no anchor.
inline const char* anchor(bool any_verify_failed, bool hub_down,
                          bool any_witness_quiet) {
  if (any_verify_failed) return "s-not-verified";
  if (hub_down) return "s-hub-unreachable";
  if (any_witness_quiet) return "s-stale-witness";
  return "";
}

// Compose the full URL. Same refusal semantics as the WAP composer: base
// is a parameter so tests pin composition; overflow returns 0 with out
// NUL'd — never a truncated URL, because half a URL in a QR scans to a
// 404 on the exact glass someone is asking for help.
inline size_t compose(char* out, size_t cap, const char* base,
                      bool any_verify_failed, bool hub_down,
                      bool any_witness_quiet) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!base || base[0] == '\0') return 0;
  const char* a = anchor(any_verify_failed, hub_down, any_witness_quiet);
  int n = (a[0] != '\0') ? snprintf(out, cap, "%s#%s", base, a)
                         : snprintf(out, cap, "%s", base);
  if (n <= 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}

}  // namespace canary::ui::help_verdict
