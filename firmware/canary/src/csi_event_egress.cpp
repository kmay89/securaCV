/*
 * SecuraCV Canary — committed csi_event egress (backlog F29; SD event log
 * and reconnect backfill, F37). See header.
 */

#include "csi_event_egress.h"
#include "canary_config.h"

#if FEATURE_CSI

#include <Arduino.h>
#include <Preferences.h>

#include "csi_event.h"
#include "csi_event_id_floor.h"

#if FEATURE_HA_MQTT
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

#include "csi_event_backfill.h"
#include "csi_event_log.h"
#include "csi_event_wire.h"
#include "identity/device_signature.h"
#include "securacv_mqtt.h"
#include "securacv_witness.h"
#endif

namespace {

/* ── Event-id floor (NVS) ─────────────────────────────────────────────────
 * The policy is common/csi/src/csi_event_id_floor.h, shared with the
 * canary-wap's csi_integration.cpp and host-tested across modeled reboots:
 * s_id_floor_stored is the value NVS holds, and an allocation at or past
 * it writes id + STRIDE before the id goes out. So NVS is always above
 * every id handed out, and a reboot skips ids at worst and reuses none. */
constexpr const char* kNvsNamespace = "securacv";
constexpr const char* kNvsKeyEventId = "csi.evid";
/* The SD backfill's delivery ceiling (csi_event_backfill.h): always above
 * every event id handed to the MQTT layer, written before the id goes. */
constexpr const char* kNvsKeyDelivered = "csi.evsent";
/* Written from whichever task allocates an id (the allocator hook), read by
 * the loop task's pump: whole-word atomics. */
uint32_t              s_id_floor_stored = 0;

void restore_event_id_floor() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/true)) return;
  const uint32_t persisted = (uint32_t)prefs.getULong(kNvsKeyEventId, 0);
  prefs.end();
  if (persisted > 0) {
    csi_event_set_event_id_floor(persisted);
    __atomic_store_n(&s_id_floor_stored, persisted, __ATOMIC_RELAXED);
  }
}

void persist_event_id_floor(uint32_t new_id) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) return;  // retried next id
  const uint32_t next_floor = csi_event_id_floor::floor_for(new_id);
  const bool wrote = prefs.putULong(kNvsKeyEventId, (unsigned long)next_floor) > 0;
  prefs.end();
  if (wrote) __atomic_store_n(&s_id_floor_stored, next_floor, __ATOMIC_RELAXED);
}

#if FEATURE_HA_MQTT
static_assert(csi_event_wire::SIG_B64URL_CAP == device_signature::SIG_B64URL_CAP,
              "csi_event_wire's signature buffer must hold a device_signature sig");

/* One committed row, copied out of the chokepoint's callback. */
struct CommittedEvent {
  uint32_t           event_id;
  uint32_t           committed_ms;
  char               module_id[CSI_EVENT_NAME_MAX];
  char               type_name[CSI_EVENT_NAME_MAX];
  uint8_t            category;
  uint8_t            privacy;
  csi_event_values_t values;
};

/* Eight rows: the modules' per-hour ceilings keep the commit rate far
 * below one per loop pass, so a full queue means the loop task is stuck —
 * and then dropping (counted) beats blocking the emitter. */
constexpr UBaseType_t kQueueDepth = 8;
constexpr int         kPumpBudget = 4;   // rows per loop pass
QueueHandle_t         s_queue = nullptr;
uint32_t              s_dropped = 0;     // queue full; atomic add
uint32_t              s_dropped_said = 0;  // what the log last reported

void copy_name(char (&dst)[CSI_EVENT_NAME_MAX], const char* src) {
  strncpy(dst, src ? src : "", CSI_EVENT_NAME_MAX - 1);
  dst[CSI_EVENT_NAME_MAX - 1] = '\0';
}

/* The boot story is already on this device's tamper topic: main.cpp
 * publishes the power-events classifier's verdict once per boot
 * (canary_power_events.h — power_loss for a brownout or a restored outage,
 * unexpected_reboot for any fault reset, watchdogs included, with the
 * outage length the csi row cannot carry). Bridging system.integrity's
 * boot kinds too would narrate one reset twice, in two words for a
 * watchdog. So the bridge carries the kinds only this module knows (SD,
 * enclosure); every row, boot kinds included, still rides `events`. */
bool boot_story_bridged_elsewhere(const char* kind) {
  return strcmp(kind, "power_loss") == 0 || strcmp(kind, "watchdog") == 0 ||
         strcmp(kind, "unexpected_reboot") == 0;
}

/* ── SD event log + reconnect backfill (backlog F37) ─────────────────────
 * The decisions are common/csi/src/csi_event_backfill.h (pure,
 * host-tested); the card I/O is src/csi_event_log.cpp under this tree's
 * loop-task SD rule; this is the glue. All of it runs in
 * csi_event_egress_pump(), on the loop task. */
csi_event_backfill::Planner s_backfill;
uint32_t                    s_dest_epoch = 0;
char                        s_owner_fp[17] = "";
uint32_t                    s_replay_run = 0;  // rows replayed in the current backlog

size_t build_body(char* body, size_t cap, const csi_event_record_t& rec,
                  uint16_t bundled, bool replay) {
  const csi_event_wire::Signer signer = {
    &device_signature::sign_event,
    device_signature::SCHEMA_V,
    device_signature::ALG_NAME,
    device_signature::fingerprint_hex(),
  };
  return csi_event_wire::build_event_body(
      body, cap, rec.event_id, rec.module_id, rec.type_name, rec.category,
      rec.privacy, &rec.values, rec.first_seen_ms, bundled, replay, signer);
}

class EgressPort : public csi_event_backfill::Port {
 public:
  csi_event_backfill::AppendResult card_append(const char* line, size_t len) override {
    return csi_event_log::append(line, len);
  }
  size_t card_read(uint32_t off, char* buf, size_t cap) override {
    return csi_event_log::read_at(off, buf, cap);
  }
  /* A row committed just now: F29's live body (bundled 1, not a replay). */
  csi_event_backfill::Sent send_live(const csi_event_record_t& rec) override {
    const size_t n = build_body(m_body, sizeof(m_body), rec, /*bundled=*/1, /*replay=*/false);
    if (n == 0) return csi_event_backfill::Sent::kNever;
    return mqtt_publish_event_live(m_body) ? csi_event_backfill::Sent::kYes
                                           : csi_event_backfill::Sent::kNotNow;
  }
  /* A row from the card: the canary-wap's backfill body (the logged bundle
   * count, the committed time), a replay unless it was committed while the
   * link was up. */
  csi_event_backfill::Sent send_backfill(const csi_event_record_t& rec, bool fresh) override {
    const size_t n = build_body(m_body, sizeof(m_body), rec, rec.bundled_count, !fresh);
    if (n == 0) return csi_event_backfill::Sent::kNever;
    return mqtt_publish_event_live(m_body) ? csi_event_backfill::Sent::kYes
                                           : csi_event_backfill::Sent::kNotNow;
  }
  /* Not on the card: F29's path — live, or into the MQTT layer's offline
   * queue with `"replay":true` when built while the link is down. */
  bool hand_to_queue(const csi_event_record_t& rec, bool deferred) override {
    const size_t n = build_body(m_body, sizeof(m_body), rec, /*bundled=*/1, deferred);
    return n > 0 && mqtt_publish_event(m_body);
  }
  bool persist_ceiling(uint32_t ceiling) override {
    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) return false;
    const bool wrote = prefs.putULong(kNvsKeyDelivered, (unsigned long)ceiling) > 0;
    prefs.end();
    return wrote;
  }

 private:
  char m_body[768];  // static storage via the static port below, not the loop stack
};
EgressPort s_port;

csi_event_backfill::Link current_link() {
  csi_event_backfill::Link link;
  link.accepting = mqtt_accepting();
  link.connected = mqtt_connected();
  link.id_floor  = __atomic_load_n(&s_id_floor_stored, __ATOMIC_RELAXED);
  link.now_ms    = millis();
  return link;
}

csi_event_record_t to_record(const CommittedEvent& ev) {
  csi_event_record_t rec;
  memset(&rec, 0, sizeof(rec));
  rec.event_id      = ev.event_id;
  rec.first_seen_ms = ev.committed_ms;
  rec.last_seen_ms  = ev.committed_ms;
  rec.category      = (csi_event_category_t)ev.category;
  rec.privacy       = (csi_privacy_class_t)ev.privacy;
  rec.bundled_count = ev.values.bundled_count;
  rec.values        = ev.values;
  memcpy(rec.module_id, ev.module_id, sizeof(rec.module_id));
  memcpy(rec.type_name, ev.type_name, sizeof(rec.type_name));
  return rec;
}
#endif  // FEATURE_HA_MQTT

}  // namespace

/* ── Strong overrides of csi_event.cpp's weak hooks ───────────────────── */

extern "C" void csi_event_on_id_advance(uint32_t new_id) {
  /* One NVS write per boot that allocates, plus one per STRIDE ids. */
  if (!csi_event_id_floor::must_persist(
          __atomic_load_n(&s_id_floor_stored, __ATOMIC_RELAXED), new_id)) {
    return;
  }
  persist_event_id_floor(new_id);
}

#if FEATURE_HA_MQTT
extern "C" void csi_event_on_committed(uint32_t                  event_id,
                                       const char*               module_id,
                                       const char*               type_name,
                                       csi_event_category_t      category,
                                       csi_privacy_class_t       privacy,
                                       const csi_event_values_t* values) {
  if (!values || !s_queue) return;
  /* The chokepoint already refuses rows above the ceiling; this keeps the
   * wire surface's rule visible here too, as the canary-wap does. */
  if (privacy > csi_event_get_privacy_ceiling()) return;
  CommittedEvent rec;
  rec.event_id     = event_id;
  rec.committed_ms = millis();
  copy_name(rec.module_id, module_id);
  copy_name(rec.type_name, type_name);
  rec.category     = (uint8_t)category;
  rec.privacy      = (uint8_t)privacy;
  rec.values       = *values;
  /* Never block the emitter (it may be the NimBLE host task). */
  if (xQueueSend(s_queue, &rec, 0) != pdTRUE) {
    __atomic_add_fetch(&s_dropped, 1u, __ATOMIC_RELAXED);
  }
}
#endif  // FEATURE_HA_MQTT

extern "C" void csi_event_egress_begin(void) {
  restore_event_id_floor();
#if FEATURE_HA_MQTT
  if (!s_queue) s_queue = xQueueCreate(kQueueDepth, sizeof(CommittedEvent));
  if (!s_queue) {
    Serial.println("[WARN] CSI event egress queue unavailable - events stay local");
  }
  /* The witness key signs the events body (device_signature's `event`
   * canonical, which HA's verify_event rebuilds). device_signature keeps
   * its own copy of the identity. */
  const DeviceIdentity& dev = witness_get_device();
  static const char kHex[] = "0123456789abcdef";
  char fp_hex[17];
  for (int i = 0; i < 8; ++i) {
    fp_hex[2 * i]     = kHex[(dev.pubkey_fp[i] >> 4) & 0xF];
    fp_hex[2 * i + 1] = kHex[dev.pubkey_fp[i] & 0xF];
  }
  fp_hex[16] = '\0';
  device_signature::init(dev.privkey, dev.pubkey, dev.device_id, fp_hex);

  /* The SD backfill: its delivery record from NVS, and the fingerprint
   * that marks this device's log on a card (csi_event_log.h). */
  memcpy(s_owner_fp, fp_hex, sizeof(s_owner_fp));
  uint32_t ceiling = 0;
  {
    Preferences prefs;
    if (prefs.begin(kNvsNamespace, /*readOnly=*/true)) {
      ceiling = (uint32_t)prefs.getULong(kNvsKeyDelivered, 0);
      prefs.end();
    }
  }
  s_backfill.begin(ceiling, __atomic_load_n(&s_id_floor_stored, __ATOMIC_RELAXED), s_port);
  s_dest_epoch = mqtt_destination_epoch();
#endif
}

extern "C" void csi_event_egress_pump(void) {
#if FEATURE_HA_MQTT
  if (!s_queue) return;
  const csi_event_backfill::Link link = current_link();

  /* The card, under the storage owner's rule (csi_event_log.h). */
  uint32_t log_size = 0;
  uint32_t tail_id = 0;
  switch (csi_event_log::poll(s_owner_fp, &log_size, &tail_id)) {
    case csi_event_log::Card::kOpened: s_backfill.card_open(log_size, tail_id); break;
    case csi_event_log::Card::kClosed: s_backfill.card_close(); break;
    case csi_event_log::Card::kUnchanged: break;
  }

  /* With no broker configured the rows are logged and owed to nobody, so a
   * broker configured later is not flooded with stale history; a changed
   * broker drops the backlog for the same reason the MQTT layer flushes its
   * offline queue. */
  const uint32_t epoch = mqtt_destination_epoch();
  if (!link.accepting || epoch != s_dest_epoch) {
    s_dest_epoch = epoch;
    s_backfill.not_owed(link, s_port);
  }

  const uint32_t dropped = __atomic_load_n(&s_dropped, __ATOMIC_RELAXED);
  if (dropped != s_dropped_said) {
    s_dropped_said = dropped;
    Serial.printf("[CSI] egress queue full: %lu committed event(s) not published\n",
                  (unsigned long)dropped);
  }
  CommittedEvent ev;
  for (int budget = kPumpBudget; budget > 0; --budget) {
    if (xQueueReceive(s_queue, &ev, 0) != pdTRUE) break;
    /* The per-kind tamper bridge first, as the canary-wap's csi_mqtt: HA's
     * per-type tamper sensors (SD Removed, Enclosure Open, ...) match
     * {"type": <kind>} on the tamper topic. It never waits on the card or
     * on a backlog the backfill is still sending. Not retained — an event,
     * not a state; a retained copy would re-fire HA's edge-latched sensors
     * on every HA restart. */
    char tb[128];
    if (link.accepting &&
        csi_event_wire::build_tamper_bridge_body(tb, sizeof(tb), ev.module_id,
                                                 ev.type_name, &ev.values) > 0 &&
        !boot_story_bridged_elsewhere(ev.values.state_name)) {
      mqtt_publish_tamper(tb, /*retained=*/false);
    }
    /* Then the row: onto the card, and live when nothing older waits there;
     * behind the backlog otherwise; through the offline queue when there
     * is no card (csi_event_backfill.h). */
    (void)s_backfill.commit(to_record(ev), link, s_port);
  }

  /* Backfill: rows the broker has not seen, from the card, in id order,
   * a bounded amount per pass, only after the offline queue has drained. */
  const size_t replayed = s_backfill.pass(link, s_port);
  if (replayed > 0) {
    s_replay_run += (uint32_t)replayed;
  } else if (s_replay_run > 0 && !s_backfill.pending()) {
    char detail[64];
    snprintf(detail, sizeof(detail), "%lu event(s) from the card",
             (unsigned long)s_replay_run);
    Serial.printf("[CSI] event backfill done: %s\n", detail);
    log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "MQTT event backfill done", detail);
    s_replay_run = 0;
  }
#endif
}

#else  // !FEATURE_CSI

extern "C" void csi_event_egress_begin(void) {}
extern "C" void csi_event_egress_pump(void) {}

#endif  // FEATURE_CSI
