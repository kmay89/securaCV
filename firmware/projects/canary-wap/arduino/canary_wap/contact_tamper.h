/**
 * @file contact_tamper.h
 * @brief Debounce + hold for an enclosure tamper contact (a reed or hall
 *        switch on a GPIO) — the data feed behind
 *        tamper_events_watch_contact(). Pure, Arduino-free, host-tested
 *        (firmware/tests_host/test_contact_tamper.cpp).
 *
 * The host samples the pin once per loop — digitalRead(TAMPER_PIN_DEFAULT)
 * == TAMPER_ACTIVE, i.e. "the contact says the enclosure is open" — and
 * passes that raw level with millis(). This header decides when the level
 * is a real change. A reed switch bounces for a few milliseconds on every
 * transition, and a knock on a closed lid can crack the contact for less
 * than a visible instant; neither is "the enclosure was opened". So a new
 * level is accepted only after it has been seen on `debounce_samples`
 * consecutive samples AND has held for `min_hold_ms` (wrap-safe unsigned
 * time math), and one sample back at the accepted level cancels the
 * candidate.
 *
 * The first sample is adopted silently: booting with the lid off (a bench,
 * an install in progress) is a configuration, not an intrusion — the same
 * rule the tamper watcher applies to an SD card that was never there.
 *
 * Lives beside tamper_events_module (and is staged into the canary-wap
 * sketch with it by setup.sh, byte-gated by check_csi_sync.sh) because it
 * is that module's contact feed: the module owns the narration, this owns
 * the electrical truth.
 */

#ifndef SECURACV_CONTACT_TAMPER_H
#define SECURACV_CONTACT_TAMPER_H

#include <stdint.h>

namespace contact_tamper {

struct Policy {
  uint8_t  debounce_samples;  // consecutive samples at the new level (0 acts as 1)
  uint32_t min_hold_ms;       // ...and held at least this long
};

// Five samples and 300 ms: longer than reed bounce and a knock on the lid,
// far shorter than a lid being lifted off.
constexpr Policy kDefaultPolicy = {5, 300};

struct State {
  bool     adopted;           // first sample taken
  bool     open;              // the accepted (debounced) level
  uint8_t  pending;           // consecutive samples at the other level
  uint32_t pending_since_ms;  // when that run began
};

constexpr State kInitial = {false, false, 0, 0};

enum class Transition : uint8_t {
  NONE   = 0,  // nothing accepted this sample (including the adopting one)
  OPENED = 1,  // closed -> open, debounced and held
  CLOSED = 2,  // open -> closed, debounced and held
};

// Feed one raw sample. `raw_open` is true when the pin reads the tamper
// level. Returns the accepted transition, if any; s->open is the level to
// report afterwards.
inline Transition sample(State* s, bool raw_open, uint32_t now_ms,
                         const Policy& p = kDefaultPolicy) {
  if (!s->adopted) {
    s->adopted = true;
    s->open = raw_open;
    s->pending = 0;
    s->pending_since_ms = now_ms;
    return Transition::NONE;
  }
  if (raw_open == s->open) {
    s->pending = 0;  // back at the accepted level: the candidate was noise
    return Transition::NONE;
  }
  if (s->pending == 0) s->pending_since_ms = now_ms;
  if (s->pending < 0xFFu) s->pending++;
  const uint8_t need = p.debounce_samples ? p.debounce_samples : 1u;
  if (s->pending < need) return Transition::NONE;
  if ((uint32_t)(now_ms - s->pending_since_ms) < p.min_hold_ms) {
    return Transition::NONE;
  }
  s->open = raw_open;
  s->pending = 0;
  return raw_open ? Transition::OPENED : Transition::CLOSED;
}

}  // namespace contact_tamper

#endif  // SECURACV_CONTACT_TAMPER_H
