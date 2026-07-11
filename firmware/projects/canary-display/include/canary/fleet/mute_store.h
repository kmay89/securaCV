#pragma once
#include <stdint.h>

// Mute persistence (display_care_wave.md §3). A mute is a promise about
// wall-clock time ("quiet until morning"), so it must survive a reboot —
// a power blip that silently resurrects a 3 a.m. nag breaks the promise,
// and one that silently forgets a bypass would be worse in the other
// direction. Epochs live in NVS; the fleet model stays millis-domain, so
// application happens once the clock is valid.

namespace canary::fleet {

// Record (or clear, until_epoch == 0) one witness's mute in NVS.
void mute_store_put(const char* id, uint32_t until_epoch);

// Re-apply persisted, unexpired mutes to the fleet model, converting epoch
// deadlines into the millis domain. Call once after SNTP first syncs
// (needs both clocks); returns how many mutes were applied. Expired
// entries are pruned in place.
int mute_store_apply(uint32_t now_ms, uint32_t now_epoch);

}  // namespace canary::fleet
