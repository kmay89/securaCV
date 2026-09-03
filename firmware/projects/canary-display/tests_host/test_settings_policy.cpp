// Host test for the LAN write policy (include/canary/net/settings_policy.h):
// which /api/set keys the display refuses from the network outright, for
// every caller and every token, because they enable the device's one
// owner-controlled outbound path (wx_direct) or store the coarse location it
// would carry (wx_loc). Those are settable only on the glass. No Arduino, no
// board.
//
// Prints "ALL SETTINGS POLICY TESTS PASSED" on success (a CI grep makes a
// silent pass impossible to fake). Build (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-display/include
//     firmware/projects/canary-display/tests_host/test_settings_policy.cpp -o t && ./t

#include "canary/net/settings_policy.h"

#include <cstdio>
#include <cstring>

using namespace canary::net;

static int g_fail = 0;

#define CHECK(cond, msg)                                             \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

// ── Every egress / location key is on-glass only ─────────────────────────
static void test_egress_and_location_keys_are_on_glass_only() {
  CHECK(settings_key_is_on_glass_only("wx_direct"),
        "wx_direct (the standalone-weather opt-in) is on-glass only");
  CHECK(settings_key_is_on_glass_only("wx_loc"),
        "wx_loc (the coarse location) is on-glass only");

  // The served table and the predicate are one thing: GET /api/settings
  // lists kOnGlassOnlyKeys under `on_glass`, so every listed key must also
  // be refused, and every refused key above must be listed.
  for (size_t i = 0; i < kOnGlassOnlyKeyCount; i++) {
    CHECK(settings_key_is_on_glass_only(kOnGlassOnlyKeys[i]),
          "every key in the served table is refused by the predicate");
    CHECK(kOnGlassOnlyKeys[i] != nullptr && kOnGlassOnlyKeys[i][0] != '\0',
          "no empty entry in the table");
    for (const char* p = kOnGlassOnlyKeys[i]; *p; ++p) {
      CHECK((*p >= 'a' && *p <= 'z') || *p == '_' || (*p >= '0' && *p <= '9'),
            "table entries are the wire form: lowercase identifiers");
    }
  }
  bool has_direct = false, has_loc = false;
  for (size_t i = 0; i < kOnGlassOnlyKeyCount; i++) {
    if (std::strcmp(kOnGlassOnlyKeys[i], "wx_direct") == 0) has_direct = true;
    if (std::strcmp(kOnGlassOnlyKeys[i], "wx_loc") == 0) has_loc = true;
  }
  CHECK(has_direct && has_loc, "the table names both keys the handler refuses");

  // The refusal log prints the TABLE's constant, never the request's bytes:
  // the lookup must hand back the very pointer it matched (identity, not
  // just equal text), so a caller's buffer can never reach the log line.
  char req[] = "wx_direct";
  const char* hit = settings_key_on_glass_only_name(req);
  CHECK(hit != nullptr, "the lookup finds wx_direct");
  CHECK(hit != req, "the lookup never returns the caller's own buffer");
  bool is_table_entry = false;
  for (size_t i = 0; i < kOnGlassOnlyKeyCount; i++) {
    if (hit == kOnGlassOnlyKeys[i]) is_table_entry = true;
  }
  CHECK(is_table_entry, "the lookup returns the table entry it matched");
  CHECK(settings_key_on_glass_only_name("wx_loc") == kOnGlassOnlyKeys[1],
        "wx_loc resolves to its own table entry");
  // Growing this list is deliberate work — the mirror page's read-only rows
  // and the docs name the class — so a change here must be a change there.
  CHECK(kOnGlassOnlyKeyCount == 2,
        "on-glass-only key count changed: update the mirror page and docs too");
}

// ── Every ordinary knob stays ordinary (Origin + CSRF, not refused) ──────
static void test_ordinary_keys_are_not_on_glass_only() {
  // Every key handle_settings_set accepts on any flavor, plus the /api/tz
  // route's value name: look, brightness, night behavior, the lamp and the
  // zone. None of these opens a network path or stores a location.
  static const char* const kOrdinary[] = {
      "day_pct",     "bright_pct",     "character",    "clock_style",
      "orientation", "clock_12h",      "night_screen", "red_shift",
      "peek_s",      "night_start_hh", "night_end_hh", "night_step",
      "lamp_scene",  "lamp_auto",      "lamp_pct",     "lamp_hue",
      "lamp_minutes", "auto_rotate",   "tz",
  };
  for (const char* k : kOrdinary) {
    CHECK(!settings_key_is_on_glass_only(k),
          "an ordinary knob must not be refused as on-glass only");
    CHECK(settings_key_on_glass_only_name(k) == nullptr,
          "an ordinary knob has no on-glass-only name");
  }
}

// ── Unknown and near-miss keys are not on-glass only (exact match) ──────
static void test_unknown_and_near_miss_keys() {
  // Unknown keys fall through to the handler's own 400 — the policy must
  // neither claim them nor let a near-miss spelling slip past as ordinary
  // while the handler stores it under the real name. The handler matches
  // exact strings too, so exact here is the only consistent answer.
  static const char* const kNotOnGlass[] = {
      "",          "wx",         "wx_direct ", " wx_direct", "WX_DIRECT",
      "Wx_Loc",    "wx_directx", "wx_loc_set", "wx_lat10",   "wx_lon10",
      "wx_status", "on_glass",   "bogus",      "day_pct\0wx_direct",
  };
  for (const char* k : kNotOnGlass) {
    CHECK(!settings_key_is_on_glass_only(k),
          "an unknown or near-miss key is not classified on-glass only");
    CHECK(settings_key_on_glass_only_name(k) == nullptr,
          "an unknown or near-miss key has no on-glass-only name");
  }
  CHECK(!settings_key_is_on_glass_only(nullptr), "a null key is not a match");
  CHECK(settings_key_on_glass_only_name(nullptr) == nullptr,
        "a null key has no on-glass-only name");
}

int main() {
  test_egress_and_location_keys_are_on_glass_only();
  test_ordinary_keys_are_not_on_glass_only();
  test_unknown_and_near_miss_keys();
  if (g_fail) {
    std::printf("%d SETTINGS POLICY CHECK(S) FAILED\n", g_fail);
    return 1;
  }
  std::printf("ALL SETTINGS POLICY TESTS PASSED\n");
  return 0;
}
