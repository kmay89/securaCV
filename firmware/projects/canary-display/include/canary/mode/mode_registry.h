// Mode registry — pure, host-testable core (no Arduino, no NVS, no LVGL).
//
// The display is one piece of glass with several jobs — "gears": the fleet
// face it ships with, the network-silent peripheral bench, a scripted demo,
// on-glass diagnostics, and the touch-QA arcade. This header holds every
// decision worth testing without a board: which gear a boot lands in, what
// each gear is allowed to do (network / OTA / watchdog / exit), and how the
// stored NVS token round-trips — including the migration of the legacy
// `devmode` bool the 4.3B dev-mode latch wrote before modes existed.
//
// The runtime glue (the main.cpp branch, the Settings doorway, the actual
// Preferences read/write) stays a thin, unclever shell around these tables —
// the same split as field_io_logic.h / modbus_rtu.h / rtc_pcf.h.
// Spec: docs/hardware/display_modes.md.
//
// Two invariants this core enforces by construction (tested on the host):
//   1. Fail-safe: an unknown/corrupt token, or a mode this build doesn't
//      carry, always resolves to Fleet — a bad NVS blob or a firmware
//      downgrade can never strand a wall display outside its product face.
//   2. No roach motels: every non-fleet mode is exit-by-long-press and wears
//      an unmissable on-glass banner — a demo unit can't impersonate a real
//      fleet, and there is always a way home that needs no app, no console.

#ifndef CANARY_MODE_MODE_REGISTRY_H
#define CANARY_MODE_MODE_REGISTRY_H

#include <stdint.h>
#include <string.h>

namespace canary {
namespace mode {

// The gears. Order is stable API (tokens below are the wire/NVS contract).
enum class Mode : uint8_t {
  Fleet = 0,  // the product: fleet face, full network stack (default)
  Bench,      // dev playground: guided peripheral bench, network-silent
  Demo,       // scripted synthetic fleet through the REAL faces, network-silent
  Debug,      // on-glass diagnostics; network UP (the link is the patient)
  Arcade,     // touch/display QA that happens to be fun; network-silent
};

inline constexpr uint8_t MODE_COUNT = 5;

// What this build carries (compile-time flags, passed as plain booleans so
// the core stays define-free and host-testable).
struct BuildCaps {
  bool dedicated_bench = false;  // FEATURE_PLAYGROUND env: boots the bench, always
  bool has_devmode = false;      // FEATURE_DEVMODE: fleet build that carries the bench
  bool has_demo = false;         // FEATURE_DEMO_MODE
  bool has_debug = false;        // FEATURE_DEBUG_MODE
  bool has_arcade = false;       // FEATURE_ARCADE
};

// What a gear may do. One row per mode, resolved from mode_policy() — the
// runtime consults this table instead of scattering per-mode #ifs.
struct ModePolicy {
  bool network;     // WiFi/MQTT/discovery ever initialized this boot
  bool ota;         // may take updates (only ever with the full product face)
  bool watchdog;    // task watchdog armed (bench/demo/debug pause freely)
  bool touch_exit;  // 3 s long-press exits back to Fleet (all non-fleet modes)
  bool banner;      // wears a persistent non-fleet chip on the glass
};

// ── NVS token contract ──────────────────────────────────────────────────────
// Short stable strings under the existing "securacv" namespace, key "mode".
// Absence of the key means Fleet. Unknown tokens mean Fleet (fail-safe).

inline const char* mode_token(Mode m) {
  switch (m) {
    case Mode::Fleet:  return "fleet";
    case Mode::Bench:  return "bench";
    case Mode::Demo:   return "demo";
    case Mode::Debug:  return "debug";
    case Mode::Arcade: return "arcade";
  }
  return "fleet";
}

inline Mode mode_from_token(const char* s) {
  if (!s || !s[0]) return Mode::Fleet;
  if (strcmp(s, "bench") == 0)  return Mode::Bench;
  if (strcmp(s, "demo") == 0)   return Mode::Demo;
  if (strcmp(s, "debug") == 0)  return Mode::Debug;
  if (strcmp(s, "arcade") == 0) return Mode::Arcade;
  return Mode::Fleet;  // "fleet", corrupt, or from-the-future: the product face
}

// ── Capability gate ─────────────────────────────────────────────────────────

inline bool mode_allowed(const BuildCaps& caps, Mode m) {
  switch (m) {
    case Mode::Fleet:  return true;  // always — it is the fail-safe
    case Mode::Bench:  return caps.dedicated_bench || caps.has_devmode;
    case Mode::Demo:   return caps.has_demo;
    case Mode::Debug:  return caps.has_debug;
    case Mode::Arcade: return caps.has_arcade;
  }
  return false;
}

// ── Boot resolution ─────────────────────────────────────────────────────────
// Decides the gear for this boot from the build caps, the stored token, and
// the legacy dev-mode bool (older 4.3B firmware wrote "devmode"=true; a
// firmware update must land those units in the bench they asked for, once,
// and the runtime then rewrites the latch in the new grammar).
//
//   - A dedicated bench build boots the bench. Always. Latches are ignored —
//     the flash IS the mode choice (matches today's FEATURE_PLAYGROUND).
//   - Else the stored token wins if this build actually carries that mode;
//     a mode the build lacks resolves to Fleet (fail-safe, invariant 1).
//   - Else the legacy devmode bool maps to Bench — but only when no token
//     was ever written (a present token is the newer write and wins).

inline Mode resolve_boot_mode(const BuildCaps& caps, const char* stored_token,
                              bool legacy_devmode) {
  if (caps.dedicated_bench) return Mode::Bench;
  const bool has_token = (stored_token && stored_token[0]);
  Mode want = mode_from_token(stored_token);
  if (!has_token && legacy_devmode) want = Mode::Bench;
  return mode_allowed(caps, want) ? want : Mode::Fleet;
}

// ── Policy table ────────────────────────────────────────────────────────────
// The one place per-mode rules live. Notes:
//   - Only Fleet gets OTA: an update mid-bench/demo/debug session is never
//     what the operator meant, and a mode that can't phone home can't be
//     talked into an update either.
//   - Debug is the one non-fleet gear with the network UP — the link itself
//     is the thing being diagnosed. It still takes no updates and publishes
//     nothing beyond the normal status heartbeat.
//   - The watchdog arms only under Fleet: every other gear stops the world
//     on purpose (a paused bench card or a breakpointed dump is not a hang).

inline ModePolicy mode_policy(Mode m) {
  switch (m) {
    case Mode::Fleet:  return {true,  true,  true,  false, false};
    case Mode::Bench:  return {false, false, false, true,  true};
    case Mode::Demo:   return {false, false, false, true,  true};
    case Mode::Debug:  return {true,  false, false, true,  true};
    case Mode::Arcade: return {false, false, false, true,  true};
  }
  return {true, true, true, false, false};  // unreachable: Fleet defaults
}

}  // namespace mode
}  // namespace canary

#endif  // CANARY_MODE_MODE_REGISTRY_H
