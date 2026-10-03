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
// (Connecting), or the human failure reason (Fail). May be null. `narrow`
// is the reason in fewer words (Fail: join_failure_label_narrow), for a
// title too narrow for it (onboard_layout.h's scene lines, F65). May be null.
void onboard_ui_stage(ObStage st, const char* detail,
                      const char* narrow = nullptr);

// Append/replace the small hint line on the current scene (e.g. the manual
// "or visit 192.168.4.1" fallback if the captive sheet never popped).
// `narrow` is an optional shorter form: on small glass's Join scene the row
// shows `line` when it fits and `narrow` when it does not (onboard_layout.h's
// join_lines — never an ellipsis).
void onboard_ui_hint(const char* line, const char* narrow = nullptr);

// Drive the stage's animation (breath / sweep). Call every loop pass.
void onboard_ui_tick(uint32_t now_ms);

// Cross-fade to the normal UI screen and auto-delete the onboarding scene.
void onboard_ui_finish();

// Where the current scene seats the brand mark: its box's top-left on the
// panel and its side, as onboard_layout.h's bird_seat() names them for this
// glass (the bird draws there, breathing onboardlayout::kBirdBreath px
// either way, or hops from there). False while no onboarding screen is up.
// The emulator's onboarding probe holds the drawn bird to it (F89).
bool onboard_ui_bird_seat(int* x, int* y, int* side);

// Where the Join scene's layout seats the halo and the QR card on this glass
// (F184): each box's top-left on the panel and its side, as onboard_layout.h's
// stack names them (small_join on small glass, wide_join on the 800x480
// line) and onboard_ui_create() aligns them (TOP_MID, the halo's x offset).
// The layout's answer, not LVGL's: the emulator's onboarding probe holds the
// card it reads off the framebuffer and the arc LVGL laid out to it. False
// while no onboarding screen is up.
struct OnboardJoinBoxes {
  int ring_x, ring_y, ring_d;     // the halo's box (the arc object's side)
  int card_x, card_y, card_side;  // the QR card's box
};
bool onboard_ui_join_layout(OnboardJoinBoxes* out);

}  // namespace canary::ui
