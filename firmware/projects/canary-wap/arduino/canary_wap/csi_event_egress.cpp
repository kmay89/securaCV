/**
 * @file csi_event_egress.cpp
 * @brief The canary-wap's committed-event egress (backlog F78). See the
 *        header for the threading and the order it keeps.
 *
 * The decisions are csi_event_backfill.h's Planner (staged copy), the same
 * the canary PIO tree's src/csi_event_egress.cpp runs; this file is the
 * glue: the commit queue, the card through csi_event_log's adapter, the
 * wire through csi_mqtt, the NVS ceiling, and the one thing the canary does
 * not need, a RAM hold for rows the card does not keep (closed bundles,
 * backlog F77), so they wait behind the backlog instead of overtaking it.
 */

#include "csi_event_egress.h"

#include "csi_event_log.h"
#include "csi_integration.h"
#include "csi_mqtt.h"

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <new>
#include <stdlib.h>
#include <string.h>

namespace csi_event_egress {

namespace {

using csi_event_backfill::AppendResult;
using csi_event_backfill::Link;
using csi_event_backfill::Route;
using csi_event_backfill::Sent;
using csi_mqtt::EventSend;

constexpr const char* SETTINGS_NS = "csi";

/* Sixteen rows: one bundler tick can close all eight bundle slots at once,
 * and an ambient module commits up to once per cooldown (wifi.channel_activity:
 * 5 s by default, 1 s at its minimum), so the queue holds a full close plus a
 * stalled pass or two. A full queue means the loop task is stuck, and then
 * dropping (counted) beats blocking the committing task, which may be the
 * NimBLE host task. */
constexpr UBaseType_t kQueueDepth = 16;
constexpr int         kPumpBudget = 8;   /* rows per loop pass */
/* Rows the card does not keep, waiting their turn in RAM. */
constexpr size_t      kHeldMax    = 8;

/* One committed row, copied out of the chokepoint's callback. */
struct Committed {
  csi_event_record_t rec;
  bool               ring;   /* a ring row: the kind the SD log keeps */
};

struct Held {
  csi_event_record_t rec;
  bool               fresh;  /* committed while the link was up: not a replay */
};

Sent to_sent(EventSend s) {
  switch (s) {
    case EventSend::kSent:        return Sent::kYes;
    case EventSend::kUnbuildable: return Sent::kNever;
    case EventSend::kNotNow:      break;
  }
  return Sent::kNotNow;
}

struct State;

/* The planner's side of the device: the card (csi_event_log's adapter),
 * the wire (csi_mqtt) and the NVS ceiling. */
class WapPort : public csi_event_backfill::Port {
 public:
  explicit WapPort(State* st) : m_st(st) {}

  bool      row_on_card = true;    /* the row being committed may go on the card */
  bool      flushing    = false;   /* commit() is handing over a row from the RAM hold */
  bool      flush_fresh = false;
  const Link* link      = nullptr;   /* route()'s link, during its commit */
  /* The commit in progress is a row whose card append failed and that will
   * wait in RAM behind older rows (or for the link): its NVS ceiling is not
   * written now (see card_append). route() clears it after the commit. */
  bool      ceiling_held = false;
  EventSend last        = EventSend::kSent;   /* the last publish this port made */

  AppendResult card_append(const char* line, size_t len) override;
  size_t card_read(uint32_t off, char* buf, size_t cap) override {
    return csi_event_log::read_at(off, buf, cap);
  }
  Sent send_live(const csi_event_record_t& rec) override;
  Sent send_backfill(const csi_event_record_t& rec, bool fresh) override;
  bool hand_to_queue(const csi_event_record_t& rec, bool deferred) override;
  bool persist_ceiling(uint32_t ceiling) override {
    /* Not written, as if NVS had refused: the planner keeps its stored
     * value and writes again before the row is handed over (flush_held's
     * commit, or the backfill send above it). */
    if (ceiling_held) return false;
    Preferences prefs;
    if (!prefs.begin(SETTINGS_NS, /*readOnly=*/false)) return false;
    const bool wrote = prefs.putULong(csi_mqtt::NVS_KEY_DELIVERED, (unsigned long)ceiling) > 0;
    prefs.end();
    return wrote;
  }

 private:
  /* Rows in the RAM hold older than `id` go first (one already delivered
   * from the card is dropped: State::front_delivered). False = one could
   * not go now (it stays, and so does `id`). */
  bool send_held_below(uint32_t id);
  State* m_st;
};

/* Everything the pump owns. Loop task only. On the heap (PSRAM when there
 * is one), not in .bss: the planner's read buffer and the RAM hold are
 * ~2.7 KB, and the full build's internal-DRAM .bss is the tight budget. */
struct State {
  csi_event_backfill::Planner planner;
  WapPort  port;
  Held     held[kHeldMax] = {};  /* zeroed: no slot is read before push() fills it */
  size_t   held_head = 0;
  size_t   held_count = 0;
  uint32_t held_dropped = 0;
  uint32_t held_dropped_said = 0;
  uint32_t ambient_dropped = 0;
  uint32_t unsent_dropped = 0;   /* not on the card, body never built: gone (stats()) */
  uint32_t dropped_said = 0;
  uint32_t replay_run = 0;   /* rows replayed in the current backlog */
  /* A card that may hold rows older than the ones committed now is not open:
   * from boot until the card's log first opens, and from a close while rows
   * waited on it until it opens again. Rows the card does not keep wait in
   * RAM meanwhile, for at most kCardWaitMs (card_wait_since). The planner
   * cannot see it: pending() is false while the card is closed. */
  bool     card_wait = true;
  uint32_t card_wait_since = 0;
  /* The broker the backlog is owed to (csi_mqtt::destination_epoch). */
  uint32_t dest_epoch = 0;
  /* Nothing on the card was owed when it had not opened yet: apply that
   * when it opens (not_owed() can only credit rows it has seen). */
  bool     not_owed_at_open = false;

  State() : port(this) {}

  const Held& front() const { return held[held_head]; }
  void pop() {
    held_head = (held_head + 1) % kHeldMax;
    --held_count;
  }
  /* The oldest held row is at or below the watermark: it went already, from
   * the card. A short write that lands every byte of the line but its
   * newline reports a failed append, so its row waits here, and the next
   * append writes the newline first: the line is then a whole one the walk
   * sends. HA's replay gate passes an equal id, so sending this copy as
   * well would fire its triggers twice (found by sweep F103's review).
   * Otherwise the watermark passes a held row only where the planner
   * already treats the rows below it as delivered: a newer row never goes
   * while one waits, not_owed() empties the hold, and a card opened with no
   * delivery record in NVS is taken as delivered up to its tail. */
  bool front_delivered() const { return front().rec.event_id <= planner.watermark(); }
  /* Rows arrive in id order (the commit lock orders the queue), so the
   * hold stays in id order; past kHeldMax the oldest goes. An ambient row is
   * never held: csi_event.h's contract for CSI_CATEGORY_AMBIENT is "never
   * persisted, drives live UI only", so one that cannot go out now is
   * dropped (counted), and a run of them can never evict a real event. */
  void push(const csi_event_record_t& rec, bool fresh) {
    if (rec.category == CSI_CATEGORY_AMBIENT) {
      ++ambient_dropped;
      return;
    }
    if (held_count == kHeldMax) {
      pop();
      ++held_dropped;
    }
    Held& h = held[(held_head + held_count) % kHeldMax];
    h.rec = rec;
    h.fresh = fresh;
    ++held_count;
  }
  void clear() {
    held_head = 0;
    held_count = 0;
  }
};

State*        g_state = nullptr;
/* Written once by begin() (loop task), read by on_committed() from any
 * task: published with release/acquire. */
QueueHandle_t s_queue = nullptr;
uint32_t      s_dropped = 0;   /* atomic add from the committing task */

AppendResult WapPort::card_append(const char* line, size_t len) {
  /* A row the card does not keep (a closed bundle, or a row from the RAM
   * hold): refused here, so the planner takes its not-on-card route. */
  if (!row_on_card) return AppendResult{false, m_st->planner.log_size(), 0};
  const AppendResult r = csi_event_log::append_line(line, len);
  /* The append failed, so the planner takes its not-on-card route, which
   * writes the NVS ceiling for this row before hand_to_queue(). When the row
   * is going to wait in RAM behind rows still on the card or in RAM, or for
   * the link (hand_to_queue's own test), that ceiling would cover the rows
   * on the card, and a reboot before they go would skip every one of them.
   * Hold it back; it is written when the row is actually handed over. */
  if (!r.ok && link && link->accepting) {
    ceiling_held = !link->connected || m_st->planner.pending() || m_st->held_count > 0;
  }
  return r;
}

bool WapPort::send_held_below(uint32_t id) {
  while (m_st->held_count > 0 && m_st->front().rec.event_id < id) {
    if (m_st->front_delivered()) {
      m_st->pop();
      continue;
    }
    const Held& h = m_st->front();
    /* The planner wrote the ceiling for `id` before this send, and every
     * held id is below it, so NVS already covers them. */
    const EventSend sent =
        csi_mqtt::publish_event_row(h.rec, /*bundled=*/1, /*replay=*/!h.fresh);
    if (sent == EventSend::kNotNow) return false;
    if (sent == EventSend::kUnbuildable) ++m_st->unsent_dropped;
    m_st->pop();   /* sent, or a body that can never build: gone either way */
  }
  return true;
}

/* A row committed just now, on the card, nothing older on the card: it
 * still waits while older rows wait in RAM (they go first, in send_backfill,
 * once the backfill reaches it). */
Sent WapPort::send_live(const csi_event_record_t& rec) {
  if (m_st->held_count > 0) return Sent::kNotNow;
  last = csi_mqtt::publish_event_row(rec, /*bundled=*/1, /*replay=*/false);
  return to_sent(last);
}

/* A row from the card, in id order. Rows waiting in RAM below it go first.
 * A dismissal line is the owner's local record (csi_event_log.h), never
 * replayed. */
Sent WapPort::send_backfill(const csi_event_record_t& rec, bool fresh) {
  if (rec.values.dismissed != 0) return Sent::kNever;
  if (!send_held_below(rec.event_id)) return Sent::kNotNow;
  last = csi_mqtt::publish_event_row(rec, rec.bundled_count, /*replay=*/!fresh);
  return to_sent(last);
}

/* A row that is not on the card. From the RAM hold (flushing): publish it.
 * Otherwise (its append failed, or nothing was waiting when it was routed
 * here): live when nothing older waits and the link is up; else it waits
 * in RAM behind the older rows instead of overtaking them, and the planner
 * is told it was not handed over (so the watermark does not move). */
bool WapPort::hand_to_queue(const csi_event_record_t& rec, bool deferred) {
  if (flushing) {
    last = csi_mqtt::publish_event_row(rec, /*bundled=*/1, /*replay=*/!flush_fresh);
    return last == EventSend::kSent;
  }
  if (!deferred && !m_st->planner.pending() && m_st->held_count == 0) {
    last = csi_mqtt::publish_event_row(rec, /*bundled=*/1, /*replay=*/false);
    if (last == EventSend::kSent) return true;
    if (last == EventSend::kUnbuildable) {
      ++m_st->unsent_dropped;   /* never builds: gone, counted */
      return false;
    }
  }
  m_st->push(rec, /*fresh=*/!deferred);
  return false;
}

Link current_link() {
  Link link;
  link.accepting = csi_mqtt::accepting();
  link.connected = csi_mqtt::connected();
  link.id_floor  = csi_integration::event_id_floor_stored();
  link.now_ms    = (uint32_t)millis();
  /* Every id this device handed out is below the allocator's next one, so
   * a card line at or above it is not ours: the planner never sends or
   * credits it (backlog F46). */
  link.id_next   = csi_event_get_next_event_id();
  return link;
}

/* One dequeued row. A row the card keeps goes to the planner (on the card;
 * live when nothing older waits, held on the card otherwise). A row it does
 * not keep waits in RAM while anything older waits (on the card, on a card
 * that is not open now, or in RAM) or the link is down, and otherwise goes
 * to the planner's not-on-card route (live now). */
void route(const Committed& ev, const Link& link) {
  State& st = *g_state;
  const bool card_row = ev.ring && st.planner.card_ok();
  if (!card_row && link.accepting &&
      (st.planner.pending() || st.card_wait || st.held_count > 0 || !link.connected)) {
    st.push(ev.rec, /*fresh=*/link.connected);
    return;
  }
  st.port.row_on_card = card_row;
  st.port.link = &link;
  (void)st.planner.commit(ev.rec, link, st.port);
  st.port.link = nullptr;
  st.port.row_on_card = true;
  st.port.ceiling_held = false;
}

/* Rows in the RAM hold, once nothing older waits on the card (or on a card
 * that is not open now): through the planner, so each is under the NVS
 * ceiling before it goes and the watermark follows it. One already
 * delivered from the card is dropped (State::front_delivered). */
void flush_held(const Link& link) {
  State& st = *g_state;
  while (st.held_count > 0 && link.accepting && link.connected && !st.planner.pending() &&
         !st.card_wait) {
    if (st.front_delivered()) {
      st.pop();
      continue;
    }
    const Held h = st.front();
    st.port.row_on_card = false;
    st.port.flushing = true;
    st.port.flush_fresh = h.fresh;
    const Route r = st.planner.commit(h.rec, link, st.port);
    st.port.flushing = false;
    st.port.row_on_card = true;
    if (r != Route::kQueued && st.port.last != EventSend::kUnbuildable) break;  /* next pass */
    if (st.port.last == EventSend::kUnbuildable) ++st.unsent_dropped;
    st.pop();
  }
}

void* egress_alloc(size_t n) {
#if defined(ARDUINO)
  void* p = ps_malloc(n);
  return p ? p : malloc(n);
#else
  return malloc(n);
#endif
}

}  // namespace

void begin() {
  if (g_state) return;
  void* mem = egress_alloc(sizeof(State));
  if (!mem) {
    Serial.println("[EVT] event egress unavailable (no memory) - events stay local");
    return;
  }
  g_state = new (mem) State();
  if (!__atomic_load_n(&s_queue, __ATOMIC_ACQUIRE)) {
    QueueHandle_t q = xQueueCreate(kQueueDepth, sizeof(Committed));
    if (!q) {
      Serial.println("[EVT] event egress queue unavailable - events stay local");
    }
    __atomic_store_n(&s_queue, q, __ATOMIC_RELEASE);
  }
  /* The delivery record: Planner::begin restores the watermark from the
   * ceiling NVS holds (csi_event_backfill::restore: no ceiling, or one the
   * allocator did not follow, is no record, and everything below the
   * restored id floor counts as delivered). */
  uint32_t ceiling = 0;
  {
    Preferences prefs;
    if (prefs.begin(SETTINGS_NS, /*readOnly=*/true)) {
      ceiling = (uint32_t)prefs.getULong(csi_mqtt::NVS_KEY_DELIVERED, 0);
      prefs.end();
    }
  }
  g_state->planner.begin(ceiling, csi_integration::event_id_floor_stored(), g_state->port);
  /* Until the card's log opens this boot, rows the card does not keep wait
   * for it (it may hold older ones): a slow card mounts after boot
   * (hardware_state.h "still probing"). */
  g_state->card_wait = true;
  g_state->card_wait_since = (uint32_t)millis();
  g_state->dest_epoch = csi_mqtt::destination_epoch();
}

void on_committed(uint32_t                  event_id,
                  const char*               module_id,
                  const char*               type_name,
                  csi_event_category_t      category,
                  csi_privacy_class_t       privacy,
                  const csi_event_values_t* values) {
  QueueHandle_t q = __atomic_load_n(&s_queue, __ATOMIC_ACQUIRE);
  if (!values || !q) return;
  Committed c;
  memset(&c, 0, sizeof(c));
  /* A ring row (stateless and ambient commits) is the record the SD log
   * keeps: the ring's copy, so the line matches what csi_event_recent
   * returns (first/last seen, bundled count). csi_event_find takes the ring
   * lock, which the chokepoint's lock order allows under the commit lock.
   * Logged as the original: a dismissal that reached the ring row first is
   * its own later line (csi_event_log.h, queue_dismissal). A closed bundle
   * never enters the ring (backlog F77): its copy is built from the commit. */
  if (csi_event_find(event_id, &c.rec)) {
    c.ring = true;
    c.rec.values.dismissed = 0;
  } else {
    c.ring = false;
    c.rec.event_id      = event_id;
    c.rec.first_seen_ms = (uint32_t)millis();
    c.rec.last_seen_ms  = c.rec.first_seen_ms;
    c.rec.category      = category;
    c.rec.privacy       = privacy;
    c.rec.bundled_count = values->bundled_count;
    c.rec.values        = *values;
    strncpy(c.rec.module_id, module_id ? module_id : "?", CSI_EVENT_NAME_MAX - 1);
    strncpy(c.rec.type_name, type_name ? type_name : "?", CSI_EVENT_NAME_MAX - 1);
  }
  /* Never block the committing task (it may be the NimBLE host task). */
  if (xQueueSend(q, &c, 0) != pdTRUE) {
    __atomic_add_fetch(&s_dropped, 1u, __ATOMIC_RELAXED);
  }
}

void pump() {
  QueueHandle_t q = __atomic_load_n(&s_queue, __ATOMIC_ACQUIRE);
  if (!g_state || !q) return;
  State& st = *g_state;
  const Link link = current_link();

  /* The card, loop task only (csi_event_log.h). */
  uint32_t log_size = 0;
  uint32_t tail_id = 0;
  switch (csi_event_log::poll(&log_size, &tail_id)) {
    case csi_event_log::CardChange::kOpened:
      st.planner.card_open(log_size, tail_id, link);
      st.card_wait = false;
      if (st.not_owed_at_open) {
        st.not_owed_at_open = false;
        st.planner.not_owed(link, st.port);
      }
      break;
    case csi_event_log::CardChange::kClosed:
      /* Rows still waiting on the card are owed when it comes back (an SD
       * error's remount, hardware_state.h, is up to SD_RECHECK_INTERVAL_MS
       * away): rows the card does not keep wait in RAM behind them. */
      if (st.planner.pending()) {
        st.card_wait = true;
        st.card_wait_since = link.now_ms;
      }
      st.planner.card_close();
      break;
    case csi_event_log::CardChange::kUnchanged: break;
  }
  /* Bounded: a card that does not open within kCardWaitMs is given up on,
   * and the rows in RAM go (the rows on it, if it comes back later, are
   * then below the watermark: HA would refuse them). */
  if (st.card_wait && (uint32_t)(link.now_ms - st.card_wait_since) >= kCardWaitMs) {
    st.card_wait = false;
    if (st.held_count > 0) {
      Serial.printf("[EVT] event log card not open after %lu s: %u event(s) waiting in RAM go out\n",
                    (unsigned long)(kCardWaitMs / 1000), (unsigned)st.held_count);
    }
  }

  /* No broker configured: the rows are logged and owed to nobody, so a
   * broker configured later is not sent stale history; a changed broker
   * drops the backlog for the same reason (the canary's rule: what waited
   * for broker A is not broker B's to see). */
  const uint32_t epoch = csi_mqtt::destination_epoch();
  if (!link.accepting || epoch != st.dest_epoch) {
    st.dest_epoch = epoch;
    st.planner.not_owed(link, st.port);
    if (!st.planner.card_ok()) st.not_owed_at_open = true;
    st.clear();
    st.card_wait = false;   /* nothing older is owed any more */
  }

  const uint32_t dropped = __atomic_load_n(&s_dropped, __ATOMIC_RELAXED);
  if (dropped != st.dropped_said) {
    st.dropped_said = dropped;
    Serial.printf("[EVT] egress queue full: %lu committed event(s) not logged or published\n",
                  (unsigned long)dropped);
  }
  if (st.held_dropped != st.held_dropped_said) {
    st.held_dropped_said = st.held_dropped;
    Serial.printf("[EVT] %lu event(s) the card could not keep dropped from the RAM hold\n",
                  (unsigned long)st.held_dropped);
  }

  Committed ev;
  for (int budget = kPumpBudget; budget > 0; --budget) {
    if (xQueueReceive(q, &ev, 0) != pdTRUE) break;
    /* The per-kind tamper bridge first: HA's tamper sensors match
     * {"type": <kind>} on the tamper topic, and a tamper alert never waits
     * on the card or on a backlog. Not retained. */
    (void)csi_mqtt::publish_tamper_bridge(ev.rec.module_id, ev.rec.type_name, &ev.rec.values);
    /* Then the row, in its turn. */
    route(ev, link);
  }

  /* Backfill: rows the broker has not seen, from the card, in id order, a
   * bounded amount per pass; then rows waiting in RAM, once nothing older
   * waits on the card. */
  const size_t replayed = st.planner.pass(link, st.port);
  flush_held(link);
  if (replayed > 0) {
    st.replay_run += (uint32_t)replayed;
  } else if (st.replay_run > 0 && !st.planner.pending()) {
    Serial.printf("[EVT] event backfill done: %lu event(s) from the card\n",
                  (unsigned long)st.replay_run);
    st.replay_run = 0;
  }
}

uint32_t watermark() { return g_state ? g_state->planner.watermark() : 0; }

Stats stats() {
  Stats s = {};
  s.dropped = __atomic_load_n(&s_dropped, __ATOMIC_RELAXED);
  if (g_state) {
    s.held_dropped = g_state->held_dropped;
    s.ambient_dropped = g_state->ambient_dropped;
    s.unsent_dropped = g_state->unsent_dropped;
    s.planner = g_state->planner.stats();
  }
  return s;
}

#ifdef CSI_TEST_HOST_BUILD
void test_reset() {
  if (g_state) {
    g_state->~State();
    free(g_state);
    g_state = nullptr;
  }
  if (s_queue) {
    vQueueDelete(s_queue);
    s_queue = nullptr;
  }
  s_dropped = 0;
}
#endif

}  // namespace csi_event_egress
