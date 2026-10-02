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
#include <stdlib.h>
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
 * every id handed out, and a reboot skips ids at worst and reuses none.
 *
 * One id space (backlog F46): every row, bundled or not, takes its id at
 * commit from the one allocator, which starts at kIdSpaceBase, above every
 * id an older firmware handed out. The restore is boot_floor(): it also
 * holds the floor at or above the backfill's delivery ceiling
 * (csi.evsent), so a boot whose floor writes failed while its ceiling
 * writes did not never reissues an id Home Assistant already has. No extra
 * write: the boot's first allocation is the write. */
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
  const uint32_t delivered = (uint32_t)prefs.getULong(kNvsKeyDelivered, 0);
  prefs.end();
  csi_event_set_event_id_floor(csi_event_id_floor::boot_floor(persisted, delivered));
  if (persisted > 0) __atomic_store_n(&s_id_floor_stored, persisted, __ATOMIC_RELAXED);
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

/* ── Rows that wait in RAM (backlog F103, F104) ───────────────────────────
 * The card keeps every row the pump drains, so a row waits on the card
 * behind older ones and the backfill sends it in turn. A row the card does
 * not take has nowhere to wait but RAM, and it must wait:
 *   - committed while no card log is open, when the card may hold older
 *     rows (F104): from boot until the log first opens (a boot mount that
 *     outlives its 4 s budget is adopted by a later periodic check), and
 *     from a close while rows waited on it (an SD error's lost mark, which
 *     the 30 s recheck remounts) until it opens again;
 *   - whose append failed while older rows wait on the card or in the hold,
 *     or while the link is down (F103; see hold_blocked() for the link).
 * Handed to the MQTT layer then, it would go live (or drain from the
 * offline queue, which goes before the backfill) ahead of those rows: Home
 * Assistant's mark would pass them, the watermark too, and the card's rows
 * would never be sent. Its ceiling, written before the hand-over, would
 * also read them as delivered after a reboot.
 *
 * Such a row waits in s_hold (kHeldMax, the oldest dropped first, counted)
 * and goes through the planner once nothing older waits, in id order with
 * the card's rows (EgressPort::send_held_below). Its NVS delivery ceiling is
 * written then, by the planner, never while it waits. The wait for the card
 * is bounded (csi_event_backfill::kCardWaitMs, the canary-wap's): past it the
 * held rows go, and rows still on a card that comes back later are below the
 * watermark (HA would refuse them). An ambient row is never held (csi_event.h:
 * "never persisted, drives live UI only"): one that cannot go at once is
 * dropped, so a burst of them never evicts a real event. Held rows do not
 * survive a reboot or a broker change, as the offline queue's do not: with
 * a card slot and no usable card, the rows of the first kCardWaitMs after
 * boot are lost to a reboot that comes sooner. The canary-wap's
 * egress keeps the same hold (its csi_event_egress.cpp); the one difference
 * is the canary's offline queue, which takes a held row once nothing older
 * waits and no card is open, while the link is down. Loop task only. */
constexpr size_t kHeldMax = 8;   // the canary-wap's

struct Held {
  csi_event_record_t rec;
  bool               fresh;  // committed while the link was up: not a replay
};

struct Hold {
  Held*    slots = nullptr;    // kHeldMax of them, allocated by begin()
  size_t   head = 0;
  size_t   count = 0;
  uint32_t dropped = 0;          // the oldest, dropped to make room (or no hold)
  uint32_t dropped_said = 0;     // what the log last reported

  const Held& front() const { return slots[head]; }
  void pop() {
    head = (head + 1) % kHeldMax;
    --count;
  }
  /* Rows arrive in id order (the commit lock orders the egress queue), so
   * the hold stays in id order. */
  void push(const csi_event_record_t& rec, bool fresh) {
    if (rec.category == CSI_CATEGORY_AMBIENT) return;
    if (!slots) {  // no memory for a hold: the row is lost, counted
      ++dropped;
      return;
    }
    if (count == kHeldMax) {
      pop();
      ++dropped;
    }
    Held& h = slots[(head + count) % kHeldMax];
    h.rec = rec;
    h.fresh = fresh;
    ++count;
  }
  void clear() {
    head = 0;
    count = 0;
  }
};
Hold     s_hold;
/* A card that may hold rows older than the ones committed now is not open
 * (see above). Set by begin() on a build with a card slot, by a close while
 * rows wait; cleared when the log opens, after kCardWaitMs, and when nothing
 * is owed. */
bool     s_card_wait = false;
uint32_t s_card_wait_since = 0;
/* The broker changed (or none is configured) before the card's log opened:
 * not_owed() can only credit rows it has seen, so it runs again at open. */
bool     s_not_owed_at_open = false;

/* The hold may not hand a row over now: rows older than it wait on the card,
 * or on a card that is not open now, or the link is down while a card is
 * open. In that last case the rows after it land on the card, and the
 * planner's hand-over would write a ceiling up to kStride ids past it, so a
 * reboot before the broker returns would read them as delivered. With no
 * card open the MQTT layer's offline queue takes it, as before F37. */
bool hold_blocked(const csi_event_backfill::Link& link) {
  return s_backfill.pending() || s_card_wait || (!link.connected && s_backfill.card_ok());
}

/* A row that is not on the card must wait now: behind the rows already in
 * the hold, or for what holds them. */
bool must_wait(const csi_event_backfill::Link& link) {
  return s_hold.count > 0 || hold_blocked(link);
}

/* A held row at or below the watermark has gone already, from the card. An
 * append that lands every byte of its line but the newline reports a failure
 * (csi_event_log::append), so its row waits in the hold, and the next append
 * writes the newline first: the line is then a whole one the walk sends. HA's
 * replay gate passes an equal id, so sending the hold's copy as well would
 * fire its triggers twice. Otherwise the watermark passes a held row only
 * where the planner already treats the rows below it as delivered: a newer
 * row never goes while one waits, not_owed() empties the hold, and a card
 * opened with no delivery record in NVS is taken as delivered up to its tail
 * (Planner::card_open), the held rows below it with the rest. */
bool delivered_from_card(const Held& h) {
  return h.rec.event_id <= s_backfill.watermark();
}

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
  /* flush_held() is handing a row from the hold to the planner. */
  bool flushing = false;
  bool flush_fresh = false;
  bool flush_unbuildable = false;  // that row's body did not build
  /* route()'s link, during its commit. */
  const csi_event_backfill::Link* link = nullptr;
  /* The row being committed failed its card append and must wait in the
   * hold (backlog F103): its NVS ceiling is not written now. route() clears
   * it after the commit. */
  bool ceiling_held = false;

  csi_event_backfill::AppendResult card_append(const char* line, size_t len) override {
    /* A row from the hold never goes on the card late: the log is written
     * in id order, and rows after it may be there already. Refused here,
     * so the planner takes its not-on-card route. */
    if (flushing) return csi_event_backfill::AppendResult{false, s_backfill.log_size(), 0};
    const csi_event_backfill::AppendResult r = csi_event_log::append(line, len);
    /* The append failed, so the planner takes its not-on-card route, which
     * writes the NVS ceiling for this row before hand_to_queue(). When the
     * row must wait (rows older than it wait on the card or in the hold, or
     * the link is down with the card open: must_wait()), that ceiling would
     * cover the rows on the card, and a reboot before they go would skip
     * every one of them. Hold it back; the planner writes it when the row
     * goes (flush_held()). */
    if (!r.ok && link && link->accepting) ceiling_held = must_wait(*link);
    return r;
  }
  size_t card_read(uint32_t off, char* buf, size_t cap) override {
    return csi_event_log::read_at(off, buf, cap);
  }
  /* A row committed just now: F29's live body, not a replay. Its count is
   * the row's own (a closed bundle's roll-ins; 1 for a direct row, which
   * csi_event_wire::bundled_on_wire() makes of a 0), the same count the
   * card line and its replay carry (sweep F81 made canary rows bundles).
   * Rows waiting in the hold are older: they go first, from send_backfill()
   * once the walk reaches this row. */
  csi_event_backfill::Sent send_live(const csi_event_record_t& rec) override {
    if (s_hold.count > 0) return csi_event_backfill::Sent::kNotNow;
    const size_t n = build_body(m_body, sizeof(m_body), rec, rec.bundled_count, /*replay=*/false);
    if (n == 0) return csi_event_backfill::Sent::kNever;
    return mqtt_publish_event_live(m_body) ? csi_event_backfill::Sent::kYes
                                           : csi_event_backfill::Sent::kNotNow;
  }
  /* A row from the card: the canary-wap's backfill body (the logged bundle
   * count, the committed time), a replay unless it was committed while the
   * link was up. Rows waiting in the hold below it go first. */
  csi_event_backfill::Sent send_backfill(const csi_event_record_t& rec, bool fresh) override {
    if (!send_held_below(rec.event_id)) return csi_event_backfill::Sent::kNotNow;
    const size_t n = build_body(m_body, sizeof(m_body), rec, rec.bundled_count, !fresh);
    if (n == 0) return csi_event_backfill::Sent::kNever;
    return mqtt_publish_event_live(m_body) ? csi_event_backfill::Sent::kYes
                                           : csi_event_backfill::Sent::kNotNow;
  }
  /* Not on the card: F29's path — live, or into the MQTT layer's offline
   * queue with `"replay":true` when built while the link is down. The
   * row's own count, as send_live(). A row from the hold is a replay unless
   * it was committed while the link was up and goes out live. The offline
   * queue drains later without asking the planner, so it is only ever given
   * a row whose ceiling may be written now: never one older rows wait
   * ahead of (route(), and ceiling_held below). */
  bool hand_to_queue(const csi_event_record_t& rec, bool deferred) override {
    if (flushing) {
      const size_t n = build_body(m_body, sizeof(m_body), rec, rec.bundled_count,
                                  deferred || !flush_fresh);
      flush_unbuildable = (n == 0);
      return n > 0 && mqtt_publish_event(m_body);
    }
    /* A row whose append failed while it must wait (card_append): into the
     * hold, behind the older rows, and not handed over, so the planner moves
     * neither the watermark nor the ceiling (backlog F103). Before, it went
     * live or into the offline queue, which drains before the backfill: Home
     * Assistant's mark passed the card's rows, the watermark too, and they
     * were never sent, reboot or not. */
    if (ceiling_held) {
      s_hold.push(rec, /*fresh=*/!deferred);
      return false;
    }
    const size_t n = build_body(m_body, sizeof(m_body), rec, rec.bundled_count, deferred);
    return n > 0 && mqtt_publish_event(m_body);
  }
  bool persist_ceiling(uint32_t ceiling) override {
    /* Not written, as if NVS had refused: the planner keeps the value it
     * holds and writes again before the row is handed over. */
    if (ceiling_held) return false;
    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) return false;
    const bool wrote = prefs.putULong(kNvsKeyDelivered, (unsigned long)ceiling) > 0;
    prefs.end();
    return wrote;
  }

 private:
  /* Rows in the hold older than `id` go first, live (the planner wrote the
   * ceiling for `id` before this send, and every held id is below it, so NVS
   * already covers them). False = one could not go now: it stays, and so
   * does the card row. A held row whose body never builds is dropped, and so
   * is one already delivered (delivered_from_card()). */
  bool send_held_below(uint32_t id) {
    while (s_hold.count > 0 && s_hold.front().rec.event_id < id) {
      const Held& h = s_hold.front();
      if (delivered_from_card(h)) {
        s_hold.pop();
        continue;
      }
      const size_t n = build_body(m_body, sizeof(m_body), h.rec, h.rec.bundled_count, !h.fresh);
      if (n > 0 && !mqtt_publish_event_live(m_body)) return false;
      s_hold.pop();
    }
    return true;
  }

  char m_body[768];  // static storage via the static port below, not the loop stack
};
EgressPort s_port;

csi_event_backfill::Link current_link() {
  csi_event_backfill::Link link;
  link.accepting = mqtt_accepting();
  link.connected = mqtt_connected();
  link.id_floor  = __atomic_load_n(&s_id_floor_stored, __ATOMIC_RELAXED);
  link.now_ms    = millis();
  /* Every id this device handed out is below the allocator's next one, so a
   * card line at or above it is not ours: the planner never sends or credits
   * it (backlog F46). */
  link.id_next   = csi_event_get_next_event_id();
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

/* One dequeued row. With the card's log open the planner logs it, then
 * sends it live or holds it on the card behind the backlog; if its append
 * fails it waits in the hold when it must (EgressPort::card_append, backlog
 * F103). With no log open it waits in the hold while anything older waits
 * (backlog F104), and otherwise takes the planner's not-on-card route: the
 * MQTT layer's publish-or-queue path, its ceiling written first. */
void route(const csi_event_record_t& rec, const csi_event_backfill::Link& link) {
  if (!s_backfill.card_ok() && link.accepting && must_wait(link)) {
    s_hold.push(rec, /*fresh=*/link.connected);
    return;
  }
  s_port.link = &link;
  (void)s_backfill.commit(rec, link, s_port);
  s_port.link = nullptr;
  s_port.ceiling_held = false;
}

/* Rows in the hold, once nothing older waits (and, with a card open, the
 * link is up): through the planner, so each is under the NVS ceiling before
 * it is handed over and the watermark follows it. A row the MQTT layer
 * refuses stays for the next pass; one whose body never builds is dropped,
 * and so is one already delivered from the card (delivered_from_card()). */
void flush_held(const csi_event_backfill::Link& link) {
  while (s_hold.count > 0 && link.accepting && !hold_blocked(link)) {
    const Held h = s_hold.front();
    if (delivered_from_card(h)) {
      s_hold.pop();
      continue;
    }
    s_port.flushing = true;
    s_port.flush_fresh = h.fresh;
    s_port.flush_unbuildable = false;
    const csi_event_backfill::Route r = s_backfill.commit(h.rec, link, s_port);
    s_port.flushing = false;
    if (r != csi_event_backfill::Route::kQueued && !s_port.flush_unbuildable) break;
    s_hold.pop();
  }
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

  /* The hold for rows that must wait in RAM (backlog F104): PSRAM first,
   * heap otherwise, as the MQTT layer's offline queue. Without it a row
   * that must wait is dropped, counted. */
  if (!s_hold.slots) {
    void* mem = psramFound() ? ps_malloc(kHeldMax * sizeof(Held)) : nullptr;
    if (!mem) mem = malloc(kHeldMax * sizeof(Held));
    s_hold.slots = static_cast<Held*>(mem);
    if (!s_hold.slots) {
      Serial.println("[WARN] CSI event egress hold unavailable - rows that must wait are dropped");
    }
  }
#if FEATURE_SD_STORAGE
  /* Until the card's log first opens this boot, rows wait for it: it may
   * hold older ones, and the boot mount can be adopted after setup(). */
  s_card_wait = true;
  s_card_wait_since = millis();
#endif
#endif
}

extern "C" void csi_event_egress_pump(void) {
#if FEATURE_HA_MQTT
  if (!s_queue) return;
  const csi_event_backfill::Link link = current_link();

  /* The card, under the storage owner's rule (csi_event_log.h). */
  uint32_t log_size = 0;
  uint32_t tail_id = 0;
  bool opened_unowed = false;   // nothing was owed when this log had not opened yet
  switch (csi_event_log::poll(s_owner_fp, &log_size, &tail_id)) {
    case csi_event_log::Card::kOpened:
      s_backfill.card_open(log_size, tail_id, link);
      s_card_wait = false;
      opened_unowed = s_not_owed_at_open;
      break;
    case csi_event_log::Card::kClosed:
      /* Rows still waiting on the card are owed when it comes back (the
       * storage manager remounts a lost card on its 30 s recheck): rows
       * committed meanwhile wait in the hold behind them (backlog F104). */
      if (s_backfill.pending()) {
        s_card_wait = true;
        s_card_wait_since = link.now_ms;
      }
      s_backfill.card_close();
      break;
    case csi_event_log::Card::kUnchanged: break;
  }
  /* Bounded: a card that does not open within kCardWaitMs is given up on,
   * and the rows in the hold go (the rows on it, if it comes back later,
   * are then below the watermark: HA would refuse them). */
  if (s_card_wait &&
      (uint32_t)(link.now_ms - s_card_wait_since) >= csi_event_backfill::kCardWaitMs) {
    s_card_wait = false;
    if (s_hold.count > 0) {
      Serial.printf("[CSI] event log card not open after %lu s: %u event(s) waiting in RAM go out\n",
                    (unsigned long)(csi_event_backfill::kCardWaitMs / 1000),
                    (unsigned)s_hold.count);
    }
  }

  /* With no broker configured the rows are logged and owed to nobody, so a
   * broker configured later is not flooded with stale history; a changed
   * broker drops the backlog for the same reason the MQTT layer flushes its
   * offline queue. not_owed() credits only rows it has seen, so a change
   * made before the card's log opened is applied again when it opens. What
   * waited in the hold, or for the card, is owed to nobody either. */
  const uint32_t epoch = mqtt_destination_epoch();
  if (!link.accepting || epoch != s_dest_epoch || opened_unowed) {
    s_dest_epoch = epoch;
    s_backfill.not_owed(link, s_port);
    s_not_owed_at_open = !s_backfill.card_ok();
    s_hold.clear();
    s_card_wait = false;
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
     * behind the backlog otherwise; with no card open, in the hold while
     * anything older waits, else through the offline queue (route()). */
    route(to_record(ev), link);
  }

  /* Backfill: rows the broker has not seen, from the card, in id order,
   * a bounded amount per pass, only after the offline queue has drained;
   * then rows in the hold, once nothing older waits. */
  const size_t replayed = s_backfill.pass(link, s_port);
  flush_held(link);
  if (s_hold.dropped != s_hold.dropped_said) {
    s_hold.dropped_said = s_hold.dropped;
    Serial.printf("[CSI] %lu event(s) dropped from the RAM hold\n",
                  (unsigned long)s_hold.dropped);
  }
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

#ifdef CSI_TEST_HOST_BUILD
extern "C" void csi_event_egress_test_reset(void) {
  __atomic_store_n(&s_id_floor_stored, 0u, __ATOMIC_RELAXED);
#if FEATURE_HA_MQTT
  if (s_queue) {
    vQueueDelete(s_queue);
    s_queue = nullptr;
  }
  s_dropped = 0;
  s_dropped_said = 0;
  s_backfill.reset();
  s_dest_epoch = 0;
  s_owner_fp[0] = '\0';
  s_replay_run = 0;
  s_hold.clear();
  s_hold.dropped = 0;
  s_hold.dropped_said = 0;
  s_card_wait = false;
  s_card_wait_since = 0;
  s_not_owed_at_open = false;
#endif
}
#endif

#else  // !FEATURE_CSI

extern "C" void csi_event_egress_begin(void) {}
extern "C" void csi_event_egress_pump(void) {}

#endif  // FEATURE_CSI
