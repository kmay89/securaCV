// First-boot onboarding scenes — the glass as the setup guide.
//
// Zero-touch by design: every scene advances on a NETWORK event (phone joins
// the AP, portal opens, credentials arrive, STA connects), never on a tap.
// The user points a phone camera at the glass and follows it; the display
// narrates honestly at each step and no state is a dead end.
//
// Runs on its own LVGL screen; onboard_ui_finish() cross-fades back to the
// normal UI (already created underneath) and auto-deletes every onboarding
// object — steady-state RAM is untouched by having onboarded.
//
// Motion budget (setup is an active-attention moment, still rationed):
//   scene fade-in 260 ms · waiting breath 2.4 s · connect sweep 1.2 s/rev ·
//   success bloom 500 ms · handoff cross-fade 420 ms. Nothing else moves.
#pragma once
#include <stdint.h>

namespace canary::ui {

enum class ObStage : uint8_t {
  Hello,        // "Hello." — a beat of welcome before any instruction
  Join,         // WIFI: QR + SSID/password fallback, breathing while it waits
  PhoneJoined,  // phone on the AP — "check your phone", portal is popping
  Connecting,   // credentials received, STA join in flight — sweep
  Fail,         // specific, amber, recoverable — portal is still open
  Success,      // "You're in." — bloom, then handoff
};

// Build the onboarding screen and load it. Safe to call only when LVGL is up.
void onboard_ui_create(const char* ap_ssid, const char* ap_pass);

// Enter a stage. `detail` is stage-specific: the home SSID being joined
// (Connecting), or the human failure reason (Fail). May be null.
void onboard_ui_stage(ObStage st, const char* detail);

// Append/replace the small hint line on the current scene (e.g. the manual
// "or visit 192.168.4.1" fallback if the captive sheet never popped).
void onboard_ui_hint(const char* line);

// Drive the stage's animation (breath / sweep). Call every loop pass.
void onboard_ui_tick(uint32_t now_ms);

// Cross-fade to the normal UI screen and auto-delete the onboarding scene.
void onboard_ui_finish();

}  // namespace canary::ui
