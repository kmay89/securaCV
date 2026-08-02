// canary/companion/settings_nav.h — the on-glass settings surface for a screen
// you operate with one thumb, in the dark, half asleep. Pure, host-testable:
// this file owns WHERE YOU ARE and WHAT A GESTURE DOES; nothing here draws.
//
// Design: docs/design/canary_companion.md §3.5.
//
// The stored preferences themselves are NOT redefined here — the display's
// `canary/glass_settings.h` blob already owns day brightness, night screen,
// red shift, peek window and quiet hours, along with the debounced-write and
// separate-calibration-key discipline that was learned the hard way. This is
// the navigation model over it, plus the two knobs a wrist device adds that a
// nightstand never needed (haptic strength, wake-on-raise).
//
// ── Why a settings tree needs its own engine ─────────────────────────────────
//
// On a 4.3" dashboard, settings are a grid you look at. On a 410 × 502 glass
// strapped to a wrist, they are a sequence you feel your way along, and the
// failure modes are different in kind:
//
//   * A mis-tap must never commit something destructive. So the tree has
//     exactly one destructive leaf and it is the only one that confirms.
//   * You must always be able to get OUT without finding a button. So a long
//     press exits from ANY depth, and there is no screen without that escape.
//   * Editing must not be modal in a way that survives you walking away. So an
//     edit that goes untouched reverts, and the revert is silent — a settings
//     screen that timed out into a committed change is a bug you find weeks
//     later on a device that got dim for no reason you can reconstruct.
//
// All three are host-testable, and all three have a test that would fail if
// someone "simplified" them away.

#ifndef CANARY_COMPANION_SETTINGS_NAV_H
#define CANARY_COMPANION_SETTINGS_NAV_H

#include <stdint.h>

namespace canary {
namespace companion {

// ── Gestures ─────────────────────────────────────────────────────────────────
//
// Four, and no more. Every additional gesture on a small round glass is a
// gesture someone performs by accident.

enum class Gesture : uint8_t {
  None = 0,
  Tap,        // select / step a value
  SwipeUp,    // previous item
  SwipeDown,  // next item
  LongPress,  // back — and, from the root, leave settings entirely
};

// Long press threshold. Matches the display's `CD_LONGPRESS_MS` deliberately:
// a household with both devices should not have two different "hold" lengths.
static constexpr uint16_t NAV_LONGPRESS_MS = 900;

// An edit left untouched this long reverts and exits. Long enough to think,
// short enough that a watch left face-up on a table does not sit in an editor.
static constexpr uint32_t NAV_EDIT_TIMEOUT_MS = 20000;

// The whole surface closes after this with no input at all.
static constexpr uint32_t NAV_IDLE_EXIT_MS = 45000;

// ── The tree ─────────────────────────────────────────────────────────────────
//
// One level of pages, each with leaves. Deliberately not deeper: a wrist device
// with three levels of menu is a device whose settings nobody ever finds twice.

enum class NavPage : uint8_t {
  Root = 0,   // not a page — the list of pages
  Night,      // how the glass spends the dark
  Wake,       // the gentle alarm
  Feel,       // haptics and sound
  Bird,       // the companion: is the pet on this device at all
  About,      // identity, firmware, the one destructive leaf
  PAGE_COUNT,
};

static constexpr uint8_t NAV_PAGE_COUNT = static_cast<uint8_t>(NavPage::PAGE_COUNT);

enum class NavItem : uint8_t {
  // Night
  NightStyleChoice = 0,  // go dark / keep a glow
  NightBrightness,       // the calibrated floor
  QuietStart,
  QuietEnd,
  RedShift,
  // Wake
  WakeEnabled,
  WakeHour,
  WakeMinute,
  // Feel
  HapticStrength,
  SoundEnabled,
  WakeOnRaise,
  // Bird
  PetEnabled,
  PetName,
  // About
  DeviceInfo,
  ForgetEverything,  // the one destructive leaf
  ITEM_COUNT,
};

static constexpr uint8_t NAV_ITEM_COUNT = static_cast<uint8_t>(NavItem::ITEM_COUNT);

// Which page owns each item, in item order. A flat table rather than a nested
// structure so the whole tree is one cache line and one glance.
static constexpr NavPage NAV_ITEM_PAGE[NAV_ITEM_COUNT] = {
    NavPage::Night, NavPage::Night, NavPage::Night, NavPage::Night, NavPage::Night,
    NavPage::Wake,  NavPage::Wake,  NavPage::Wake,
    NavPage::Feel,  NavPage::Feel,  NavPage::Feel,
    NavPage::Bird,  NavPage::Bird,
    NavPage::About, NavPage::About,
};

// The only leaf that may not be committed by a single tap.
inline bool nav_item_is_destructive(NavItem i) {
  return i == NavItem::ForgetEverything;
}

// Leaves that are pure information — tapping them does nothing at all, which is
// itself worth encoding so a tap does not fall through to some default action.
inline bool nav_item_is_readonly(NavItem i) { return i == NavItem::DeviceInfo; }

inline uint8_t nav_items_on_page(NavPage p) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < NAV_ITEM_COUNT; i++) {
    if (NAV_ITEM_PAGE[i] == p) n++;
  }
  return n;
}

// The nth item on a page, in table order.
inline NavItem nav_item_at(NavPage p, uint8_t index) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < NAV_ITEM_COUNT; i++) {
    if (NAV_ITEM_PAGE[i] != p) continue;
    if (n == index) return static_cast<NavItem>(i);
    n++;
  }
  return static_cast<NavItem>(0);
}

// ── Where you are ────────────────────────────────────────────────────────────

enum class NavLevel : uint8_t {
  Closed = 0,  // not in settings
  Pages,       // choosing a page
  Items,       // choosing an item on a page
  Editing,     // changing one value
  Confirming,  // the destructive leaf, waiting for a deliberate second tap
};

struct NavState {
  NavLevel level = NavLevel::Closed;
  NavPage page = NavPage::Night;
  uint8_t item_index = 0;
  uint8_t page_index = 0;
  uint32_t last_input_ms = 0;
  bool dirty = false;      // an edit is pending commit (caller debounces the write)
  bool reverted = false;   // set for one step when a timeout undid an edit
  bool committed = false;  // set for one step when an edit was committed
  bool exited = false;     // set for one step when the surface closed
};

// ── The one-shot flags ───────────────────────────────────────────────────────
//
// `reverted` / `committed` / `exited` are consumed by the renderer on the tick
// they appear and cleared at the top of the next step. They exist so the UI can
// say "not saved" out loud when a timeout undoes something — a silent revert is
// how a user ends up not trusting their own settings screen.

inline void nav_clear_oneshots(NavState& s) {
  s.reverted = false;
  s.committed = false;
  s.exited = false;
}

inline void nav_open(NavState& s, uint32_t now_ms) {
  s.level = NavLevel::Pages;
  s.page_index = 0;
  s.page = static_cast<NavPage>(1);  // skip Root — it is the list, not a page
  s.item_index = 0;
  s.last_input_ms = now_ms;
  s.dirty = false;
  nav_clear_oneshots(s);
}

inline void nav_close(NavState& s) {
  s.level = NavLevel::Closed;
  s.dirty = false;
  s.exited = true;
}

// Pages are 1..NAV_PAGE_COUNT-1 (Root is the container, not a destination).
static constexpr uint8_t NAV_FIRST_PAGE = 1;
static constexpr uint8_t NAV_LAST_PAGE = NAV_PAGE_COUNT - 1;

inline void nav_step_page(NavState& s, int delta) {
  int p = static_cast<int>(s.page_index) + delta;
  const int span = NAV_LAST_PAGE - NAV_FIRST_PAGE + 1;
  while (p < 0) p += span;
  p %= span;
  s.page_index = static_cast<uint8_t>(p);
  s.page = static_cast<NavPage>(NAV_FIRST_PAGE + p);
}

inline void nav_step_item(NavState& s, int delta) {
  const uint8_t n = nav_items_on_page(s.page);
  if (n == 0) return;
  int i = static_cast<int>(s.item_index) + delta;
  while (i < 0) i += n;
  s.item_index = static_cast<uint8_t>(i % n);
}

inline NavItem nav_current_item(const NavState& s) {
  return nav_item_at(s.page, s.item_index);
}

// One input. `g` may be Gesture::None, which is how timeouts are delivered:
// the caller ticks this every frame and the engine decides when time is up.
inline void nav_input(NavState& s, Gesture g, uint32_t now_ms) {
  nav_clear_oneshots(s);
  if (s.level == NavLevel::Closed) return;

  // ── Timeouts, before any gesture handling ──
  if (g == Gesture::None) {
    const uint32_t idle = now_ms - s.last_input_ms;
    if (s.level == NavLevel::Editing && idle >= NAV_EDIT_TIMEOUT_MS) {
      // Revert, loudly. The caller has the pre-edit value; we only say "undo".
      s.dirty = false;
      s.reverted = true;
      s.level = NavLevel::Items;
      s.last_input_ms = now_ms;
      return;
    }
    if (s.level == NavLevel::Confirming && idle >= NAV_EDIT_TIMEOUT_MS) {
      s.level = NavLevel::Items;  // an unconfirmed wipe is simply not a wipe
      s.last_input_ms = now_ms;
      return;
    }
    if (idle >= NAV_IDLE_EXIT_MS) nav_close(s);
    return;
  }

  s.last_input_ms = now_ms;

  // ── Back / exit, from any depth ──
  if (g == Gesture::LongPress) {
    switch (s.level) {
      case NavLevel::Editing:
        // Backing out of an edit KEEPS it — you got here by choosing values and
        // watching them change. Only a timeout reverts, because only a timeout
        // means nobody was looking.
        s.level = NavLevel::Items;
        s.committed = s.dirty;
        return;
      case NavLevel::Confirming:
        s.level = NavLevel::Items;
        return;
      case NavLevel::Items:
        s.level = NavLevel::Pages;
        return;
      case NavLevel::Pages:
        nav_close(s);
        return;
      default:
        return;
    }
  }

  const int delta = (g == Gesture::SwipeDown) ? 1 : (g == Gesture::SwipeUp) ? -1 : 0;

  switch (s.level) {
    case NavLevel::Pages:
      if (delta != 0) {
        nav_step_page(s, delta);
      } else if (g == Gesture::Tap) {
        s.item_index = 0;
        s.level = NavLevel::Items;
      }
      return;

    case NavLevel::Items:
      if (delta != 0) {
        nav_step_item(s, delta);
      } else if (g == Gesture::Tap) {
        const NavItem it = nav_current_item(s);
        if (nav_item_is_readonly(it)) return;  // a tap on info does nothing
        s.level = nav_item_is_destructive(it) ? NavLevel::Confirming
                                              : NavLevel::Editing;
      }
      return;

    case NavLevel::Editing:
      // Swipes and taps both step the value; the caller owns the value itself.
      // Reporting `dirty` rather than writing is what lets the caller debounce
      // a drag into one flash write instead of hundreds.
      s.dirty = true;
      return;

    case NavLevel::Confirming:
      // Only a tap confirms, and only from this state — reachable exclusively
      // by a deliberate tap on the destructive leaf, so a wipe costs two
      // separate intentional gestures and can never be one slip.
      if (g == Gesture::Tap) {
        s.committed = true;
        s.level = NavLevel::Items;
      }
      return;

    default:
      return;
  }
}

}  // namespace companion
}  // namespace canary

#endif  // CANARY_COMPANION_SETTINGS_NAV_H
