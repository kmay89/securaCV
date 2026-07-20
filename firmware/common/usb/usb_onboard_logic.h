/*
 * SecuraCV Canary — USB onboarding core logic (host-testable, no Arduino).
 *
 * The pure, board-agnostic half of the "plug me in" experience described in
 * docs/design/usb_onboard.md. When the Canary is plugged into a computer on a
 * USB-OTG build it enumerates as a composite device — the serial console (CDC),
 * a read-only evidence drive (MSC, see usb_evidence_drive), AND a HID keyboard.
 * The keyboard exists for exactly one purpose: to open the owner's help page
 * (https://securacv.com/canary) so a person who just plugged the device in
 * lands somewhere that explains recovery and unsealing.
 *
 * A keyboard that types on its own is the BadUSB attack pattern, so the trust
 * model is the whole point and it lives HERE, in testable logic, not buried in
 * device glue:
 *
 *   1. The device NEVER types on its own. Enumerating does nothing. It types
 *      only after a human presses the physical BOOT button (a CONFIRM event),
 *      and only inside a short arming window that the console opened by
 *      ANNOUNCING, in plain text, the exact URL it is about to type.
 *   2. The payload is fixed at compile time and validated at run time: the only
 *      thing the keyboard can ever emit is the configured https help origin.
 *      build_launch_plan() refuses any URL that is not allow-listed, so even a
 *      corrupted device id can never turn into a typed shell command.
 *   3. It is one-shot per arming and defaults OFF (FEATURE_USB_ONBOARD=0).
 *
 * The Arduino/TinyUSB/HID glue lives in canary/lib/securacv_usb_onboard/
 * (firmware only, FEATURE_USB_ONBOARD). This file must compile hosted for
 * tests_host/test_usb_onboard_logic.cpp with -Wall -Wextra -Werror.
 */

#ifndef SECURACV_USB_ONBOARD_LOGIC_H
#define SECURACV_USB_ONBOARD_LOGIC_H

#include <stddef.h>
#include <stdint.h>

namespace usb_onboard {

// ════════════════════════════════════════════════════════════════════════════
// 1. Consent state machine (who is allowed to make the keyboard type)
// ════════════════════════════════════════════════════════════════════════════

enum class State : uint8_t {
  DISABLED = 0,  // feature off — the HID keyboard never types, ever
  IDLE,          // enumerated and quiet; waiting for the owner to ask
  ARMED,         // owner asked ('u'); URL announced; awaiting BOOT confirm
  LAUNCHED,      // keystrokes emitted once; will not repeat without re-arming
};

enum class Event : uint8_t {
  ENABLE,        // build/init brought the feature up
  REQUEST,       // owner asked to launch help (console 'u' / long-press)
  CONFIRM,       // physical BOOT-button confirmation seen
  TIMEOUT,       // arming window elapsed with no confirm
  CANCEL,        // owner backed out (any key on the console)
  UNPLUG,        // USB cable removed — re-lock everything
};

// Default arming window: how long a REQUEST stays open for the physical
// CONFIRM before it re-locks. Short enough that a walk-away re-locks, long
// enough for a person to reach for the button.
static constexpr uint32_t kDefaultArmWindowMs = 15000;

struct Outcome {
  State next;
  bool announce;   // glue should print the exact URL + "press BOOT to open"
  bool emit;       // glue should play the keystroke plan (the only path that types)
  bool relock;     // glue should note we returned to a quiet state
};

// Pure transition. `armed_since_ms`/`now_ms` are only consulted for TIMEOUT;
// callers compute elapsed themselves so this stays clock-free and testable.
inline Outcome step(State s, Event e) {
  Outcome o{ s, false, false, false };
  switch (e) {
    case Event::ENABLE:
      if (s == State::DISABLED) { o.next = State::IDLE; }
      return o;

    case Event::REQUEST:
      // Re-arming from LAUNCHED is allowed (owner wants the page again), but
      // never from DISABLED.
      if (s == State::IDLE || s == State::LAUNCHED) {
        o.next = State::ARMED;
        o.announce = true;
      }
      return o;

    case Event::CONFIRM:
      // The ONLY edge that types. Requires having passed through ARMED.
      if (s == State::ARMED) {
        o.next = State::LAUNCHED;
        o.emit = true;
      }
      return o;

    case Event::TIMEOUT:
    case Event::CANCEL:
      if (s == State::ARMED) { o.next = State::IDLE; o.relock = true; }
      return o;

    case Event::UNPLUG:
      // Never re-enable a disabled feature; otherwise drop back to quiet IDLE.
      if (s != State::DISABLED) { o.next = State::IDLE; o.relock = true; }
      return o;
  }
  return o;
}

// True once the arming window has elapsed — glue feeds this to raise TIMEOUT.
inline bool arm_expired(uint32_t armed_since_ms, uint32_t now_ms,
                        uint32_t window_ms = kDefaultArmWindowMs) {
  return (uint32_t)(now_ms - armed_since_ms) >= window_ms;
}

// ════════════════════════════════════════════════════════════════════════════
// 2. Help-URL construction + allow-listing
// ════════════════════════════════════════════════════════════════════════════

// A device id can be typed into a URL query, so only the characters that are
// safe there unescaped survive. Everything else is dropped (not percent-encoded
// — a Canary device id is already [A-Za-z0-9_-], this is belt-and-suspenders).
inline bool url_id_char_ok(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_';
}

// Build "<base>?d=<device_id>&r=<reason>". device_id and reason are sanitized to
// url_id_char_ok(). Always NUL-terminates; returns the length written (0 on a
// bad argument or when the base alone would not fit).
inline size_t build_help_url(const char* base, const char* device_id,
                             const char* reason, char* out, size_t out_len) {
  if (!base || !out || out_len == 0) { if (out && out_len) out[0] = '\0'; return 0; }

  size_t n = 0;
  auto put = [&](char c) { if (n + 1 < out_len) out[n++] = c; };
  auto put_str = [&](const char* s) { for (; s && *s; ++s) put(*s); };
  auto put_clean = [&](const char* s) {
    for (; s && *s; ++s) { if (url_id_char_ok(*s)) put(*s); }
  };

  put_str(base);
  if (n + 1 >= out_len) { out[n < out_len ? n : out_len - 1] = '\0'; return n; }

  bool have_q = false;
  if (device_id && *device_id) {
    put('?'); have_q = true; put_str("d="); put_clean(device_id);
  }
  if (reason && *reason) {
    put(have_q ? '&' : '?'); put_str("r="); put_clean(reason);
  }
  out[n] = '\0';
  return n;
}

// A character permitted in the typed URL. Positive allow-list (stronger than a
// deny-list): letters, digits, the RFC-3986 unreserved set, and the structural
// characters our help URLs actually use (`:/?#=&%`). Every shell metacharacter
// — whitespace, quotes, `;` `|` `$` `<` `>` backtick backslash `^` `{}` `()` `*`
// `!` — is therefore excluded, so an OS-hotkey launch cannot break out of
// "open this URL" into a second command.
inline bool url_body_char_ok(unsigned char c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9')) return true;
  switch (c) {
    case '-': case '.': case '_': case '~':   // unreserved
    case ':': case '/': case '?': case '#':   // structural
    case '=': case '&': case '%':             // query
      return true;
    default:
      return false;
  }
}

// Strict allow-list: a URL the keyboard is permitted to type. It must begin
// with the configured https help origin, every character must pass
// url_body_char_ok(), and it must not contain "&&" (which would chain commands
// in a Windows Run box). MANUAL mode never runs a command, but this gate holds
// for every method so the keyboard is provably incapable of emitting anything
// but a single help link.
inline bool is_allowed_help_url(const char* url, const char* allowed_prefix) {
  if (!url || !allowed_prefix) return false;

  // Origin must itself be https.
  const char* https = "https://";
  for (size_t j = 0; https[j]; ++j) {
    if (allowed_prefix[j] != https[j]) return false;
  }
  // Must start with the exact configured origin.
  size_t i = 0;
  for (; allowed_prefix[i]; ++i) {
    if (url[i] != allowed_prefix[i]) return false;
  }
  // Remainder may only contain URL-safe characters, and never "&&".
  for (const char* p = url; *p; ++p) {
    if (!url_body_char_ok((unsigned char)*p)) return false;
    if (p[0] == '&' && p[1] == '&') return false;
  }
  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// 3. Keystroke plan (what the glue plays back on the HID keyboard)
// ════════════════════════════════════════════════════════════════════════════

// The host OS cannot be reliably detected over HID, so the owner picks the
// method on the console (default MANUAL — the safest). MANUAL types the URL as
// plain text and stops; the person presses Enter in whatever field is focused
// (ideally a browser address bar). The OS methods add a leading hotkey that
// opens a launcher/box first. No method can run an arbitrary command: the plan
// only ever carries the allow-listed help URL as its literal text.
enum class LaunchMethod : uint8_t {
  MANUAL = 0,     // type URL text only; no hotkey, no auto-Enter
  MAC_SPOTLIGHT,  // GUI+Space, type URL, Enter
  WIN_RUN,        // GUI+R, type URL, Enter
  GNOME_TERMINAL, // for Linux desktops that map GUI to Activities/search
};

// Abstract modifier bits (map to HID usage in the glue; kept Arduino-free here).
enum : uint8_t {
  MOD_NONE = 0,
  MOD_GUI  = 1 << 0,  // Cmd / Windows / Super
  MOD_CTRL = 1 << 1,
  MOD_ALT  = 1 << 2,
  MOD_SHIFT= 1 << 3,
};

// A single prelude keystroke (a chord like GUI+Space). key==0 means "no chord".
struct Chord {
  uint8_t mods;   // MOD_* bitfield
  char    key;    // printable trigger key, e.g. ' ' or 'r'; 0 = none
};

struct LaunchPlan {
  Chord    prelude;         // hotkey to open a launcher (key==0 for MANUAL)
  uint16_t prelude_delay_ms;// settle time after the prelude before typing
  char     url[256];        // the literal text to type (allow-listed help URL)
  bool     press_enter;     // press Enter after typing the URL
  bool     valid;           // false → glue must type NOTHING
};

// Build the plan for `method` targeting `url`, which must pass
// is_allowed_help_url(url, allowed_prefix). On any failure the returned plan is
// {valid=false} and carries an empty url — the glue treats invalid as "do not
// type", so a rejected URL can never be partially emitted.
inline LaunchPlan build_launch_plan(LaunchMethod method, const char* url,
                                    const char* allowed_prefix) {
  LaunchPlan p{};
  p.url[0] = '\0';
  p.valid = false;

  if (!is_allowed_help_url(url, allowed_prefix)) return p;

  // Copy URL with a hard bound (defense-in-depth; allow-list already capped
  // the character set).
  size_t n = 0;
  for (; url[n] && n + 1 < sizeof(p.url); ++n) p.url[n] = url[n];
  p.url[n] = '\0';
  if (url[n] != '\0') return p;  // URL longer than the buffer → refuse

  switch (method) {
    case LaunchMethod::MANUAL:
      p.prelude = { MOD_NONE, 0 };
      p.prelude_delay_ms = 0;
      p.press_enter = false;
      break;
    case LaunchMethod::MAC_SPOTLIGHT:
      p.prelude = { MOD_GUI, ' ' };
      p.prelude_delay_ms = 400;
      p.press_enter = true;
      break;
    case LaunchMethod::WIN_RUN:
      p.prelude = { MOD_GUI, 'r' };
      p.prelude_delay_ms = 400;
      p.press_enter = true;
      break;
    case LaunchMethod::GNOME_TERMINAL:
      p.prelude = { MOD_GUI, 0 };  // tap Super to open Activities search
      p.prelude_delay_ms = 500;
      p.press_enter = true;
      break;
  }
  p.valid = true;
  return p;
}

}  // namespace usb_onboard

#endif  // SECURACV_USB_ONBOARD_LOGIC_H
