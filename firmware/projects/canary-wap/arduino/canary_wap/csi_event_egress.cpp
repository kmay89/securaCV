/**
 * @file csi_event_egress.cpp
 * @brief See csi_event_egress.h. Moved verbatim in behavior from
 *        csi_mqtt.cpp (the watermark, its NVS ceiling and the iterate_since
 *        backfill) and csi_integration.cpp's commit hook (the live publish
 *        and the SD append).
 */

#include "csi_event_egress.h"

#include "csi_event_backfill.h"   /* staged copy — ceiling_for, restore (F47, F46) */
#include "csi_event_id_floor.h"   /* staged copy — must_persist, the shared write cadence */
#include "csi_event_log.h"
#include "csi_integration.h"
#include "csi_mqtt.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

namespace csi_event_egress {

namespace {

constexpr const char* SETTINGS_NS = "csi";

/* Highest event_id published since boot. The live publish and the
 * backfill replay both update it; on a clean run after an HA outage the
 * next CONNECTED triggers iterate_since(this) which only emits the events
 * the broker missed. Across a reboot begin() restores it from the delivery
 * CEILING below (F47): replaying "today's full log" sent up to
 * BACKFILL_MAX ids Home Assistant's replay gate refuses — HA can only have
 * verified ids this device handed over, and the ceiling is always above
 * every one of those. */
uint32_t s_last_published_event_id = 0;
/* The delivery ceiling NVS holds (0 = none yet) under NVS_KEY_DELIVERED
 * (csi_mqtt.h): csi_event_backfill's rule over this sketch's own
 * iterate_since backfill — the same key the canary PIO tree's egress
 * writes, persisted BEFORE an id is handed over, capped at the id
 * allocator's persisted floor so a new boot's ids are never read as
 * delivered (csi_event_backfill::ceiling_for). */
uint32_t s_delivered_ceiling = 0;
bool     s_watermark_restored = false;

/* csi_event_backfill's persist_for, over this sketch's NVS (F47): before
 * an id is handed over, NVS must already hold a ceiling above it, capped
 * at the id allocator's persisted floor. A failed write leaves
 * s_delivered_ceiling unchanged so the next hand-over tries again; the
 * row still goes out — delivery cannot wait on flash, the same trade the
 * id floor makes. */
void persist_delivered_ceiling(uint32_t event_id) {
  if (event_id == 0) return;
  if (!csi_event_id_floor::must_persist(s_delivered_ceiling, event_id)) return;
  const uint32_t c = csi_event_backfill::ceiling_for(
      event_id, csi_integration::event_id_floor_stored());
  Preferences prefs;
  if (!prefs.begin(SETTINGS_NS, /*readOnly=*/false)) return;
  const bool wrote = prefs.putULong(csi_mqtt::NVS_KEY_DELIVERED, (unsigned long)c) > 0;
  prefs.end();
  if (wrote) s_delivered_ceiling = c;
}

/* The "publish-then-advance-watermark" pattern, so the live emit and the
 * backfill replay agree on what counts as "HA has seen this id" (PR #395
 * review r3213834627): the watermark advances only when the enqueue
 * succeeded AND the id is higher than what we already tracked. Returns the
 * publish outcome so the backfill can stop mid-replay (PR #395 review
 * r3213834314). */
bool publish_and_advance(const csi_event_record_t& rec, uint16_t bundled, bool replay) {
  persist_delivered_ceiling(rec.event_id);
  if (csi_mqtt::publish_event_row(rec, bundled, replay) != csi_mqtt::EventSend::kSent) {
    return false;
  }
  if (rec.event_id > s_last_published_event_id) {
    s_last_published_event_id = rec.event_id;
  }
  return true;
}

/* iterate_since callback used by the backfill drain. We stop iterating
 * the moment either the broker drops OR a publish fails to enqueue
 * (queue full, network glitch, etc.) so the watermark doesn't tick
 * past a record that never reached HA — letting later successful
 * publishes "skip over" the failed one would permanently lose the
 * event on subsequent reconnects (PR #395 review r3213834314). The
 * next CONNECTED rearms the request and we resume from the unchanged
 * watermark. For backfill, the timestamp is the event's first_seen_ms
 * so HA's history places it at the right moment instead of "now", the
 * bundled count comes straight from the on-disk record, and is_replay
 * marks the payload so HA Device Triggers can filter it out (PR #398
 * review r3214114357). */
bool backfill_publish_cb(const csi_event_record_t* rec, void* /*user*/) {
  if (!csi_mqtt::connected()) return false;
  return publish_and_advance(*rec, rec->bundled_count, /*replay=*/true);
}

}  // namespace

void begin() {
  /* F47: restore the delivery watermark from the persisted ceiling — on
   * the boot-time call ONLY. The ceiling is written kStride ahead of the
   * id it covers, so on a runtime re-init (a /api/mqtt/config POST, the
   * connection test) reading it back would jump a live watermark past ids
   * committed but not yet handed over, and iterate_since() would then skip
   * them for good; the RAM watermark is exact while the firmware runs, so
   * a re-init keeps it. Only a reboot loses it, and only there is the
   * stride's skip the accepted trade.
   * The rule is csi_event_backfill::restore(), Planner::begin's, host-
   * tested in test_csi_event_backfill.cpp. With no ceiling on record (the
   * first boot of this firmware), every id below the restored id floor is
   * treated as delivered — an earlier image may have published it, and HA
   * would refuse it again — and the record starts here, written now so
   * rows still on the card survive a reboot as owed instead of falling
   * under the same assumption (a ceiling is never 0). A ceiling the id
   * allocator did not follow (past kHoldLimit; an older firmware wrote one
   * for a forged card line) is no record either (backlog F46): kept, it
   * would read every row this boot commits as delivered, on every boot.
   * A failed write retries on the next hand-over. */
  if (s_watermark_restored) return;
  s_watermark_restored = true;
  Preferences prefs;
  if (prefs.begin(SETTINGS_NS, /*readOnly=*/true)) {
    s_delivered_ceiling = (uint32_t)prefs.getULong(csi_mqtt::NVS_KEY_DELIVERED, 0);
    prefs.end();
  }
  const csi_event_backfill::Restored restored = csi_event_backfill::restore(
      s_delivered_ceiling, csi_integration::event_id_floor_stored());
  if (restored.through > s_last_published_event_id) {
    s_last_published_event_id = restored.through;
  }
  if (restored.write != 0) {
    s_delivered_ceiling = 0;   /* no record until the rewrite lands */
    Preferences rw;
    if (rw.begin(SETTINGS_NS, /*readOnly=*/false)) {
      const uint32_t c = s_last_published_event_id + 1;
      if (rw.putULong(csi_mqtt::NVS_KEY_DELIVERED, (unsigned long)c) > 0) {
        s_delivered_ceiling = c;
      }
      rw.end();
    }
  }
}

void on_committed(uint32_t                  event_id,
                  const char*               module_id,
                  const char*               type_name,
                  csi_event_category_t      category,
                  csi_privacy_class_t       privacy,
                  const csi_event_values_t* values) {
  if (!values) return;
  /* Forward to MQTT (no-op when the bridge is disabled or the broker is
   * unreachable): the live body, stamped now, bundled 1, not a replay.
   * event_id flows through so the watermark tracks the high-water-mark
   * for backfill on reconnect. */
  csi_event_record_t live;
  memset(&live, 0, sizeof(live));
  live.event_id      = event_id;
  live.first_seen_ms = (uint32_t)millis();
  live.last_seen_ms  = live.first_seen_ms;
  live.category      = category;
  live.privacy       = privacy;
  live.bundled_count = 1;
  live.values        = *values;
  strncpy(live.module_id, module_id ? module_id : "?", CSI_EVENT_NAME_MAX - 1);
  strncpy(live.type_name, type_name ? type_name : "?", CSI_EVENT_NAME_MAX - 1);
  (void)publish_and_advance(live, /*bundled=*/1, /*replay=*/false);
  (void)csi_mqtt::publish_tamper_bridge(module_id, type_name, values);

  /* Persist to SD so today's history survives a reboot AND so the MQTT
   * bridge can backfill HA after an outage (csi_event_log iterate_since
   * walks the same file). The full record (including first_seen_ms /
   * last_seen_ms / bundled_count, which the bundler filled in inside the
   * ring) lives in the in-memory ring; pull a copy via csi_event_find so
   * the on-disk row matches what csi_event_recent would return. The HTTP
   * task can dismiss the row between its commit and this copy (the MQTT
   * publish above sits in between); append() writes it "dismissed":0
   * regardless, and the queued dismissal follows as its own line
   * (csi_event_log.h, queue_dismissal), so the original is never mistaken
   * for a dismissal and lost. */
  csi_event_record_t persist_rec;
  if (csi_event_find(event_id, &persist_rec)) {
    csi_event_log::append(&persist_rec);
  }
}

void pump() {
  /* Backfill drain. esp_mqtt runs its own task and signals reconnect via
   * csi_mqtt::take_backfill_request(); we drain on the main loop because
   * the SD walk can take longer than the MQTT event callback should hold,
   * and append() also runs on the main loop so we serialize naturally
   * without a mutex. */
  if (csi_mqtt::take_backfill_request()) {
    if (csi_mqtt::connected()) {
      const size_t n = csi_event_log::iterate_since(
          s_last_published_event_id, backfill_publish_cb, nullptr);
      if (n > 0) {
        Serial.printf("[MQTT] backfill replayed %u events past id=%lu\n",
                      (unsigned)n, (unsigned long)s_last_published_event_id);
      }
    }
  }
}

uint32_t watermark() { return s_last_published_event_id; }

#ifdef CSI_TEST_HOST_BUILD
void test_reset() {
  s_last_published_event_id = 0;
  s_delivered_ceiling = 0;
  s_watermark_restored = false;
}
#endif

}  // namespace csi_event_egress
