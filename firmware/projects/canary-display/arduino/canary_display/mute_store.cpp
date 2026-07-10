// src/fleet/mute_store.cpp — NVS-backed mute persistence (see header).
//
// Slot-based layout under the "scv-mute" namespace (Preferences keys cap at
// 15 chars): m<i>_id / m<i>_at, i in [0, CD_FLEET_MAX_DEVICES). A cleared
// or expired mute frees its slot. Tiny, write-light (mutes are a human
// gesture, not telemetry).
#include "mute_store.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "fleet_instance.h"

namespace canary::fleet {

namespace {
constexpr int SLOTS = CD_FLEET_MAX_DEVICES;

void slot_keys(int i, char* kid, size_t kid_cap, char* kat, size_t kat_cap) {
  snprintf(kid, kid_cap, "m%d_id", i);
  snprintf(kat, kat_cap, "m%d_at", i);
}
}  // namespace

void mute_store_put(const char* id, uint32_t until_epoch) {
  if (!id || !id[0]) return;
  Preferences prefs;
  if (!prefs.begin("scv-mute", /*readOnly=*/false)) return;
  char kid[12], kat[12];
  int free_slot = -1;
  for (int i = 0; i < SLOTS; i++) {
    slot_keys(i, kid, sizeof(kid), kat, sizeof(kat));
    String sid = prefs.getString(kid, "");
    if (sid.length() == 0) {
      if (free_slot < 0) free_slot = i;
      continue;
    }
    if (strcmp(sid.c_str(), id) == 0) {
      if (until_epoch == 0) {
        prefs.remove(kid);
        prefs.remove(kat);
      } else {
        prefs.putUInt(kat, until_epoch);
      }
      prefs.end();
      return;
    }
  }
  if (until_epoch != 0 && free_slot >= 0) {
    slot_keys(free_slot, kid, sizeof(kid), kat, sizeof(kat));
    prefs.putString(kid, id);
    prefs.putUInt(kat, until_epoch);
  }
  prefs.end();
}

int mute_store_apply(uint32_t now_ms, uint32_t now_epoch) {
  if (now_epoch < 1700000000UL) return 0;  // no honest clock, no application
  Preferences prefs;
  if (!prefs.begin("scv-mute", /*readOnly=*/false)) return 0;
  auto& fleet = the_fleet();
  char kid[12], kat[12];
  int applied = 0;
  for (int i = 0; i < SLOTS; i++) {
    slot_keys(i, kid, sizeof(kid), kat, sizeof(kat));
    String sid = prefs.getString(kid, "");
    if (sid.length() == 0) continue;
    const uint32_t at = prefs.getUInt(kat, 0);
    if (at <= now_epoch) {  // expired across the reboot — prune
      prefs.remove(kid);
      prefs.remove(kat);
      continue;
    }
    const uint32_t remain_s = at - now_epoch;
    if (fleet.set_mute(sid.c_str(), true, now_ms + remain_s * 1000UL)) {
      applied++;
      log_header("MUTE");
      canary::dbg_serial().printf("%s muted for %lus more (persisted)\n",
                                  sid.c_str(), (unsigned long)remain_s);
    }
  }
  prefs.end();
  return applied;
}

}  // namespace canary::fleet
