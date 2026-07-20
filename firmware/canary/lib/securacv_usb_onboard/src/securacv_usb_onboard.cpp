/*
 * SecuraCV Canary — USB onboarding implementation (firmware glue).
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 *
 * The board-agnostic trust model and payload rules live in
 * common/usb/usb_onboard_logic.h and are host-tested; this file is only the
 * TinyUSB/HID/MSC/SD plumbing that plays back what that logic decided.
 */

#include "securacv_usb_onboard.h"

#include <Arduino.h>

// HID keyboard + MSC only exist in a USB-OTG (TinyUSB) build. FEATURE_USB_ONBOARD
// is defined only by [env:usb-onboard], which also sets ARDUINO_USB_MODE=0, but
// we guard on both so the file compiles harmlessly in every other profile.
#if defined(FEATURE_USB_ONBOARD) && FEATURE_USB_ONBOARD && \
    defined(ARDUINO_USB_MODE) && (ARDUINO_USB_MODE == 0)
  #define USB_ONBOARD_ACTIVE 1
#else
  #define USB_ONBOARD_ACTIVE 0
#endif

#if USB_ONBOARD_ACTIVE
  #include <USB.h>
  #include <USBHIDKeyboard.h>
  #include <USBMSC.h>
  #include <SD.h>
#endif

namespace usb_onboard {

namespace {

Config       s_cfg = { nullptr, nullptr, false };
State        s_state = State::DISABLED;
LaunchMethod s_method = LaunchMethod::MANUAL;
uint32_t     s_armed_since_ms = 0;
bool         s_begun = false;
bool         s_msc_up = false;

// The allow-list prefix the typed URL must match: the compile-time help origin.
const char* allowed_prefix() {
  return s_cfg.help_url_base ? s_cfg.help_url_base : "https://securacv.com/canary";
}

const char* state_name(State s) {
  switch (s) {
    case State::DISABLED: return "disabled";
    case State::IDLE:     return "idle (quiet)";
    case State::ARMED:    return "ARMED — press BOOT to open help";
    case State::LAUNCHED: return "launched";
  }
  return "?";
}

const char* method_name(LaunchMethod m) {
  switch (m) {
    case LaunchMethod::MANUAL:         return "manual (type URL, you press Enter)";
    case LaunchMethod::MAC_SPOTLIGHT:  return "macOS (Spotlight)";
    case LaunchMethod::WIN_RUN:        return "Windows (Run box)";
    case LaunchMethod::GNOME_TERMINAL: return "Linux (Activities search)";
  }
  return "?";
}

// Apply an Outcome from the pure state machine, recording the arm timestamp.
void apply(Outcome o) {
  if (o.next == State::ARMED && s_state != State::ARMED) {
    s_armed_since_ms = millis();
  }
  s_state = o.next;
}

#if USB_ONBOARD_ACTIVE
USBHIDKeyboard s_kbd;
USBMSC         s_msc;

void tap_chord(const Chord& c) {
  if (c.mods & MOD_GUI)   s_kbd.press(KEY_LEFT_GUI);
  if (c.mods & MOD_CTRL)  s_kbd.press(KEY_LEFT_CTRL);
  if (c.mods & MOD_ALT)   s_kbd.press(KEY_LEFT_ALT);
  if (c.mods & MOD_SHIFT) s_kbd.press(KEY_LEFT_SHIFT);
  if (c.key != 0)         s_kbd.press((uint8_t)c.key);
  delay(40);
  s_kbd.releaseAll();
}

// Play the plan on the HID keyboard. `p.valid` has already been checked against
// the allow-list by build_launch_plan(); we re-assert it here so a future caller
// can never reach the typing path with an unvalidated URL.
void emit_plan(const LaunchPlan& p) {
  if (!p.valid) {
    Serial.println("[usb-onboard] refused: URL failed the help-origin allow-list");
    return;
  }
  const bool has_prelude = (p.prelude.key != 0) || (p.prelude.mods != MOD_NONE);
  if (has_prelude) {
    tap_chord(p.prelude);
    if (p.prelude_delay_ms) delay(p.prelude_delay_ms);
  }
  s_kbd.print(p.url);
  if (p.press_enter) s_kbd.write(KEY_RETURN);
}

// ── Read-only SD exposure over USB MSC (navigate the files) ─────────────────
// Read-only at the USB protocol level: the write callback always fails, so a
// host can browse /WITNESS, /HEALTH, /CHAIN but cannot alter the evidence.
int32_t msc_read_cb(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  const uint32_t ssz = SD.sectorSize();
  if (ssz == 0 || (offset % ssz) != 0 || (bufsize % ssz) != 0) return -1;
  uint8_t* out = static_cast<uint8_t*>(buffer);
  const uint32_t first = lba + (offset / ssz);
  const uint32_t count = bufsize / ssz;
  for (uint32_t i = 0; i < count; ++i) {
    if (!SD.readRAW(out + (i * ssz), first + i)) return -1;
  }
  return static_cast<int32_t>(bufsize);
}

int32_t msc_write_cb(uint32_t, uint32_t, uint8_t*, uint32_t) {
  return -1;  // read-only: the evidence drive is never writable over USB
}

bool msc_start_stop_cb(uint8_t /*power*/, bool /*start*/, bool load_eject) {
  if (load_eject) {
    // Host ejected — treat like a re-lock of the onboarding session.
    apply(step(s_state, Event::CANCEL));
  }
  return true;
}

bool bring_up_msc() {
  const uint32_t sectors = SD.numSectors();
  const uint32_t ssz     = SD.sectorSize();
  if (sectors == 0 || ssz == 0) return false;
  s_msc.vendorID("SecuraCV");
  s_msc.productID("Canary-Eviden");   // <=16 chars per USB MSC inquiry
  s_msc.productRevision("2.2");
  s_msc.onRead(msc_read_cb);
  s_msc.onWrite(msc_write_cb);
  s_msc.onStartStop(msc_start_stop_cb);
  s_msc.mediaPresent(true);
  s_msc.isWritable(false);            // enforce read-only to the host
  return s_msc.begin(sectors, ssz);
}
#endif  // USB_ONBOARD_ACTIVE

}  // namespace

bool begin(const Config& cfg) {
  s_cfg = cfg;
#if USB_ONBOARD_ACTIVE
  s_kbd.begin();
  if (cfg.expose_msc) {
    s_msc_up = bring_up_msc();
    if (!s_msc_up) {
      Serial.println("[usb-onboard] MSC not started (SD not mounted?) — HID only");
    }
  }
  USB.productName("SecuraCV Canary");
  USB.begin();
  apply(step(State::DISABLED, Event::ENABLE));   // DISABLED → IDLE
  s_begun = true;
  Serial.println("[usb-onboard] ready — HID keyboard idle; press console 'u' to open help");
  return true;
#else
  (void)cfg;
  s_begun = false;
  s_state = State::DISABLED;
  return false;  // not an OTG build — onboarding unavailable
#endif
}

void poll() {
  if (!s_begun) return;
  if (s_state == State::ARMED &&
      arm_expired(s_armed_since_ms, millis())) {
    apply(step(s_state, Event::TIMEOUT));
    Serial.println("[usb-onboard] arming window elapsed — re-locked (no keys sent)");
  }
}

void request_launch() {
  if (!s_begun) {
    Serial.println("[usb-onboard] not in this build (needs the usb-onboard OTG profile)");
    return;
  }
  Outcome o = step(s_state, Event::REQUEST);
  apply(o);
  if (!o.announce) {
    Serial.println("[usb-onboard] cannot arm from the current state");
    return;
  }
  // ANNOUNCE, in plain text, exactly what will be typed — the trust keystone.
  char url[256];
  build_help_url(allowed_prefix(), s_cfg.device_id, "onboard", url, sizeof(url));
  Serial.println("\n=== Open help page? ===");
  Serial.printf("  Method : %s\n", method_name(s_method));
  Serial.printf("  Will type: %s\n", url);
  Serial.println("  Nothing is typed until you press the physical BOOT button.");
  Serial.println("  Press any other key to cancel. Auto-cancels in 15s.");
}

void confirm() {
  if (!s_begun) return;
  if (s_state != State::ARMED) return;  // ignore stray presses
  char url[256];
  build_help_url(allowed_prefix(), s_cfg.device_id, "onboard", url, sizeof(url));
  Outcome o = step(s_state, Event::CONFIRM);
  apply(o);
  if (!o.emit) return;
#if USB_ONBOARD_ACTIVE
  LaunchPlan plan = build_launch_plan(s_method, url, allowed_prefix());
  Serial.printf("[usb-onboard] opening: %s\n", url);
  emit_plan(plan);
#else
  Serial.printf("[usb-onboard] (would open: %s)\n", url);
#endif
}

void cancel() {
  if (!s_begun) return;
  Outcome o = step(s_state, Event::CANCEL);
  if (o.relock) {
    apply(o);
    Serial.println("[usb-onboard] cancelled — re-locked");
  }
}

void set_method(LaunchMethod m) {
  s_method = m;
  Serial.printf("[usb-onboard] launch method: %s\n", method_name(m));
}

LaunchMethod method() { return s_method; }

void on_unplug() {
  if (!s_begun) return;
  apply(step(s_state, Event::UNPLUG));
}

State state() { return s_state; }

void print_status() {
  Serial.println("\n=== USB Onboarding ===");
  Serial.printf("  State : %s\n", state_name(s_state));
  Serial.printf("  Method: %s\n", method_name(s_method));
  Serial.printf("  Drive : %s\n",
                s_msc_up ? "read-only SD exposed (browse /WITNESS, /HEALTH, /CHAIN)"
                         : "not exposed");
  if (!s_begun) {
    Serial.println("  (build without USB-OTG onboarding — flash the usb-onboard profile)");
  }
  Serial.println();
}

void print_recovery_guide() {
  Serial.println("\n=== Recovery — your evidence is safe by design ===");
  Serial.println("  1. The witness log at /WITNESS/records.jsonl is append-only and");
  Serial.println("     signed (Ed25519, SHA-256 hash chain). It survives a factory");
  Serial.println("     reset — reset erases NVS config, never the SD evidence.");
  Serial.println("  2. On boot the device reconciles its cached chain head with the");
  Serial.println("     SD tail: the SD wins only if it is strictly ahead AND its");
  Serial.println("     signature verifies under this device's key (SD-wins recovery).");
  Serial.println("  3. To recover a device that lost its NVS head, just re-seat the");
  Serial.println("     SD and reboot; the chain head is rebuilt from the SD tail.");
  Serial.println("  4. To verify off-device, browse the read-only drive (console 'v'");
  Serial.println("     exposes it) and run tools/verify_witness_log.py on records.jsonl.");
  Serial.println("  Full walkthrough: https://securacv.com/canary\n");
}

void print_unseal_guide() {
  Serial.println("\n=== Unsealing sealed evidence ===");
  Serial.println("  Sealed snapshots (.svlt) are encrypted to YOUR public key with an");
  Serial.println("  ephemeral X25519 + ChaCha20-Poly1305 box. The Canary holds only the");
  Serial.println("  public key and is structurally unable to decrypt them — write-only");
  Serial.println("  escrow. That is the privacy guarantee, not a limitation.");
  Serial.println("  To open them:");
  Serial.println("  1. Copy the .svlt files off the read-only drive (/VAULT).");
  Serial.println("  2. On your computer, run tools/unseal_snapshot.py with your");
  Serial.println("     private key. Only you can do this; the device never can.");
  Serial.println("  Full walkthrough: https://securacv.com/canary\n");
}

}  // namespace usb_onboard
