/**
 * @file csi_event_backfill.h
 * @brief Replaying committed csi_events from the SD event log once the MQTT
 *        broker returns (backlog F37): the delivery watermark, the replay
 *        cursor over the log, what to send next, and how much work one loop
 *        pass may do.
 *
 * Pure: no Arduino, no SD, no MQTT, no clock. The host does the I/O through
 * a Port and passes the link state and time in. Used by the canary PIO tree
 * (src/csi_event_egress.cpp over its loop-task SD adapter,
 * src/csi_event_log.cpp); the canary-wap sketch carries a staged copy
 * (check_csi_sync.sh) but still drives its own csi_event_log backfill.
 * Host-tested by firmware/tests_host/test_csi_event_backfill.cpp, which
 * replays whole outages against a model of Home Assistant's replay gate.
 *
 * The rule every choice below serves: Home Assistant's replay gate
 * (custom_components/securacv/sensor.py `_replay_gate`) refuses a signed
 * `events` body whose event_id is below the last one it verified for the
 * device. HA can only have verified an id this device handed over, so the
 * backfill never sends an id at or below the WATERMARK — the highest id
 * handed over (sent live, or buffered in the MQTT layer's offline queue) or
 * deliberately given up — and walks the log forward, so what it sends rises.
 * On a log written in id order, "forward" is "in id order"; a line whose id
 * is at or below the watermark when the walk reaches it is skipped, because
 * HA would refuse it.
 *
 * Across a reboot the watermark comes back from a CEILING in NVS, written
 * with csi_event_id_floor.h's policy (F29's shared event-id floor) before an
 * id is handed over: the ceiling is always above every id handed over, so a
 * reboot never republishes one, and skips at most kStride - 1 undelivered
 * ones. ceiling_for() also keeps the ceiling at or below the id allocator's
 * persisted floor when it can, so the ids a new boot hands out (they start
 * at that floor) are never mistaken for delivered ones.
 *
 * The commit path (commit()), in order of preference for a row that must
 * reach the broker:
 *   - on the card and nothing waiting before it: publish live now;
 *   - on the card behind rows still waiting: HELD — the backfill sends it
 *     in turn, so a new row can never overtake an older one (overtaking
 *     would raise HA's mark and get the older one refused);
 *   - not on the card (no card, append failed): the MQTT layer's
 *     publish-or-queue path, as before F37. That raises the watermark past
 *     any rows still waiting on the card: they stay on the card, but HA
 *     would now refuse them, so they are not sent.
 *
 * Backfill runs (pass()) only while the link is up; the Port's live publish
 * refuses while the MQTT offline queue still holds records, so queued
 * tamper alerts always go first. It reads at most kChunksPerPass x
 * kReadChunk bytes and sends at most kSendsPerPass events per loop pass, no
 * more often than every kSendIntervalMs; tamper-topic alerts never go
 * through it (the host publishes them at commit, whatever the backfill is
 * doing), so a long backlog cannot delay one.
 *
 * `replay`: a row committed while the link was up but held behind the
 * backlog is still news, so the backfill sends it as fresh (HA device
 * triggers fire on it); every other row it sends is a replay. The planner
 * remembers the last kFreshIds such rows; past that it says replay, the
 * conservative answer.
 */

#ifndef SECURACV_CSI_EVENT_BACKFILL_H
#define SECURACV_CSI_EVENT_BACKFILL_H

#include "csi_event.h"            /* csi_event_record_t */
#include "csi_event_id_floor.h"   /* the stride policy the ceiling reuses */
#include "csi_event_log_line.h"   /* the on-card line format */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace csi_event_backfill {

/* Bounded work per loop pass. The read budget matches the witness history
 * bridge's (docs/design/witness_history_bridge.md: at most 4 x 1 KiB per
 * pass); a whole line always fits one read. */
constexpr size_t   kReadChunk      = 1024;
constexpr size_t   kChunksPerPass  = 4;
constexpr size_t   kSendsPerPass   = 2;
/* Paces the replay (at most kSendsPerPass per interval, ~20 events/s) so a
 * long outage's backlog does not arrive at the broker as one burst. */
constexpr uint32_t kSendIntervalMs = 100;
/* Rows committed while the link was up that the planner remembers as fresh. */
constexpr size_t   kFreshIds       = 8;
/* Consecutive failed card reads before the walk gives up on this card. */
constexpr uint8_t  kReadFailLimit  = 3;
/* How much of the log's tail the host reads to find its last id. */
constexpr size_t   kTailRead       = 1024;

static_assert(kReadChunk >= csi_event_log_line::kLineMax,
              "one read must hold a whole line");

/* The NVS ceiling to write before handing `id` over: above `id` (the
 * guarantee), kStride past it (the write cadence), but no higher than the
 * allocator's persisted floor when `id` came from below that floor — the
 * next boot's ids start at the floor, and a ceiling above it would read
 * them as already delivered. An id at or above the floor (the bundler's
 * ids, which no floor covers, or one handed out after a failed floor write)
 * gets the plain stride. */
inline uint32_t ceiling_for(uint32_t id, uint32_t id_floor) {
  const uint32_t c = csi_event_id_floor::floor_for(id);
  return (id_floor > id && id_floor < c) ? id_floor : c;
}

/* The id of the last whole, well-formed line in buf[0..n) — a read of the
 * log's tail — or 0 when there is none. Only '\n'-terminated lines count: a
 * torn fragment after the last newline is ignored, and so is a partial line
 * at the start of the buffer (it does not start a record). */
inline uint32_t last_line_id(const char* buf, size_t n) {
  if (!buf) return 0;
  size_t end = n;
  while (end > 0 && buf[end - 1] != '\n') --end;  /* drop a torn fragment */
  while (end > 0) {
    const size_t nl = end - 1;                     /* this line's '\n' */
    size_t start = nl;
    while (start > 0 && buf[start - 1] != '\n') --start;
    const size_t len = nl - start;
    if (len > 0 && len < csi_event_log_line::kLineMax) {
      char line[csi_event_log_line::kLineMax];
      memcpy(line, buf + start, len);
      line[len] = '\0';
      csi_event_record_t rec;
      if (csi_event_log_line::parse(line, &rec)) return rec.event_id;
    }
    end = start;
  }
  return 0;
}

/* Whose log is on this card. The log's own lines carry no device id (the
 * format is the canary-wap's), so the canary keeps its witness-key
 * fingerprint in a sibling file and uses a log only when that file names
 * it: the backfill signs what it replays with THIS device's key, and
 * another device's history must never go out under it. */
enum class Owner : uint8_t {
  kOurs,     /* the owner file names this device: use the log */
  kClaim,    /* no log yet: write the owner file, then use it */
  kForeign,  /* a log this device cannot claim: leave the card's log alone */
};

/* `recorded` is the owner file's content ("" when absent; trailing
 * whitespace ignored), `ours` this device's fingerprint. */
inline Owner owner_verdict(bool log_exists, const char* recorded, const char* ours) {
  if (!ours || !ours[0]) return Owner::kForeign;   /* nothing to bind to */
  const size_t n = strlen(ours);
  bool match = recorded && strncmp(recorded, ours, n) == 0;
  if (match) {
    for (const char* p = recorded + n; *p; ++p) {
      if (*p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') { match = false; break; }
    }
  }
  if (match) return Owner::kOurs;
  return log_exists ? Owner::kForeign : Owner::kClaim;
}

/* What one append did. */
struct AppendResult {
  bool     ok;    /* the whole line landed */
  uint32_t size;  /* the log's size afterwards, whatever happened */
  uint32_t cut;   /* bytes a retention truncation dropped from the head first */
};

/* A live publish attempt's outcome. */
enum class Sent : uint8_t {
  kYes,     /* the broker link took it */
  kNotNow,  /* link down, offline queue still draining, or the send failed */
  kNever,   /* this record cannot be published at all (its body does not build) */
};

/* The host's side: card I/O (loop task, under the storage owner's rule),
 * the MQTT surfaces and the NVS ceiling. */
class Port {
 public:
  virtual ~Port() {}
  /* Append one line (with its '\n') to the log. */
  virtual AppendResult card_append(const char* line, size_t len) = 0;
  /* Read up to `cap` bytes at byte offset `off`; the count read, 0 on error. */
  virtual size_t card_read(uint32_t off, char* buf, size_t cap) = 0;
  /* Publish a row committed just now, live only (never buffered). */
  virtual Sent send_live(const csi_event_record_t& rec) = 0;
  /* Publish a row from the card, live only; `fresh` = committed while the
   * link was up (so not a replay). */
  virtual Sent send_backfill(const csi_event_record_t& rec, bool fresh) = 0;
  /* Hand a row that is not on the card to the MQTT layer's publish-or-queue
   * path; `deferred` = built while the link is down. True = sent or buffered. */
  virtual bool hand_to_queue(const csi_event_record_t& rec, bool deferred) = 0;
  /* Write the delivery ceiling to NVS. True = written. */
  virtual bool persist_ceiling(uint32_t ceiling) = 0;
};

/* The link as the host sees it this pass. */
struct Link {
  bool     accepting;  /* a broker is configured (events are owed to it) */
  bool     connected;  /* the broker link is up now */
  uint32_t id_floor;   /* the event-id floor NVS holds now (0 = none) */
  uint32_t now_ms;     /* wraps; only differences are used */
};

enum class Route : uint8_t {
  kLive,     /* published live now */
  kHeld,     /* on the card; the backfill sends it in turn */
  kQueued,   /* not on the card; handed to the MQTT layer */
  kUnsent,   /* not on the card, and the MQTT layer refused it */
  kNotOwed,  /* no broker configured: logged, owed to nobody */
};

struct Stats {
  uint32_t live;
  uint32_t held;
  uint32_t queued;
  uint32_t replayed;
  uint32_t skipped;            /* lines passed over: delivered, torn or foreign to the format */
  uint32_t unsendable;         /* lines whose body would not build */
  uint32_t truncated_unsent;   /* retention truncations that dropped rows still waiting */
  uint32_t read_giveups;       /* walks abandoned after kReadFailLimit failed reads */
};

class Planner {
 public:
  Planner() : m_buf() { reset(); }

  void reset() {
    m_through = 0;
    m_stored = 0;
    m_card_ok = false;
    m_size = 0;
    m_scan_off = 0;
    m_card_max = 0;
    m_read_fails = 0;
    m_parked = false;
    m_sent_before = false;
    m_last_send_ms = 0;
    memset(m_fresh, 0, sizeof(m_fresh));
    m_fresh_next = 0;
    m_stats = Stats{};
  }

  /* Boot. `nvs_ceiling` is the persisted delivery ceiling (0 = never
   * written), `id_floor` the event-id floor restored at boot (0 = none). */
  void begin(uint32_t nvs_ceiling, uint32_t id_floor, Port& port) {
    reset();
    m_stored = nvs_ceiling;
    if (nvs_ceiling != 0) {
      m_through = nvs_ceiling - 1;
      return;
    }
    /* No delivery record: the first boot of this firmware on this device.
     * Every id handed out before this boot is treated as delivered (an
     * earlier firmware may have published it, and HA would refuse it
     * again), and the record starts here — written now, so that rows held
     * on the card this boot are still owed after a reboot instead of
     * falling under the same assumption. A ceiling is never 0. */
    m_through = (id_floor != 0) ? id_floor - 1 : 0;
    if (port.persist_ceiling(m_through + 1)) m_stored = m_through + 1;
  }

  /* The host opened this card's log: `size` bytes, last id `tail_id`. */
  void card_open(uint32_t size, uint32_t tail_id) {
    m_card_ok = true;
    m_size = size;
    m_card_max = tail_id;
    m_read_fails = 0;
    m_parked = false;
    /* A log with no delivery record behind it (NVS lost its ceiling) is
     * treated as delivered, for the same reason as begin(). */
    if (m_stored == 0 && tail_id > m_through) m_through = tail_id;
    m_scan_off = (tail_id > m_through) ? 0 : size;
  }

  /* The card left (or its log became unusable). */
  void card_close() {
    m_card_ok = false;
    m_parked = false;
  }

  /* Nothing on the card is owed any more: no broker is configured, or the
   * broker changed (the same rule that flushes the MQTT offline queue —
   * what waited for broker A is not broker B's to see). The watermark
   * moves past every logged row, persisted, so neither a later broker nor
   * a reboot gets the backlog. */
  void not_owed(const Link& link, Port& port) {
    if (m_card_max > m_through) {
      persist_for(m_card_max, link.id_floor, port);
      m_through = m_card_max;
    }
    m_scan_off = m_size;
    m_parked = false;
  }

  /* Rows are waiting on the card for the backfill. */
  bool pending() const {
    return m_card_ok && m_scan_off < m_size && m_card_max > m_through;
  }

  /* One committed row, on the loop task: log it, then route it. */
  Route commit(const csi_event_record_t& rec, const Link& link, Port& port) {
    const bool waiting = pending();
    bool on_card = false;
    uint32_t line_end = 0;
    if (m_card_ok) {
      /* m_buf doubles as the line buffer: commit() and pass() never run
       * at once (both are the host's loop task). */
      const size_t n = csi_event_log_line::marshal(&rec, m_buf, sizeof(m_buf));
      if (n > 0) {
        const AppendResult r = port.card_append(m_buf, n);
        if (r.cut > 0) on_cut(r.cut);
        m_size = r.size;
        if (r.ok) {
          on_card = true;
          line_end = r.size;
          if (rec.event_id > m_card_max) m_card_max = rec.event_id;
        }
      }
    }

    if (!link.accepting) {
      persist_for(rec.event_id, link.id_floor, port);
      advance(rec.event_id);
      if (on_card && !waiting) m_scan_off = line_end;
      return Route::kNotOwed;
    }

    if (on_card) {
      if (!waiting && link.connected) {
        persist_for(rec.event_id, link.id_floor, port);
        if (port.send_live(rec) == Sent::kYes) {
          advance(rec.event_id);
          m_scan_off = line_end;  /* caught up, past this line */
          m_stats.live++;
          return Route::kLive;
        }
      }
      if (link.connected) note_fresh(rec.event_id);
      m_stats.held++;
      return Route::kHeld;
    }

    persist_for(rec.event_id, link.id_floor, port);
    if (port.hand_to_queue(rec, /*deferred=*/!link.connected)) {
      advance(rec.event_id);
      m_stats.queued++;
      return Route::kQueued;
    }
    return Route::kUnsent;
  }

  /* One loop pass of backfill. Returns how many rows it sent. */
  size_t pass(const Link& link, Port& port) {
    if (!link.accepting || !link.connected || !pending()) return 0;
    const bool may_send = !m_sent_before ||
        (uint32_t)(link.now_ms - m_last_send_ms) >= kSendIntervalMs;
    /* Parked on a row it will send, inside the pacing interval: nothing
     * to read until the interval is up. */
    if (m_parked && !may_send) return 0;
    m_parked = false;
    size_t sent = 0;
    for (size_t chunk = 0; chunk < kChunksPerPass && pending(); ++chunk) {
      const uint32_t left = m_size - m_scan_off;
      const size_t want = (left < kReadChunk) ? (size_t)left : kReadChunk;
      const size_t got = port.card_read(m_scan_off, m_buf, want);
      if (got == 0 || got > want) {
        note_read_fail();
        break;
      }
      size_t pos = 0;
      while (pos < got) {
        const char* nl = static_cast<const char*>(memchr(m_buf + pos, '\n', got - pos));
        if (!nl) break;
        const size_t len = (size_t)(nl - (m_buf + pos));
        const uint32_t step = (uint32_t)(len + 1);
        m_buf[pos + len] = '\0';
        csi_event_record_t rec;
        if (len >= csi_event_log_line::kLineMax ||
            !csi_event_log_line::parse(m_buf + pos, &rec) ||
            rec.event_id <= m_through) {
          m_scan_off += step;
          pos += step;
          m_stats.skipped++;
          continue;
        }
        if (!may_send || sent >= kSendsPerPass) {
          m_parked = true;
          return finish(sent, link);
        }
        persist_for(rec.event_id, link.id_floor, port);
        const Sent s = port.send_backfill(rec, is_fresh(rec.event_id));
        if (s == Sent::kNotNow) {
          m_parked = true;
          return finish(sent, link);
        }
        forget_fresh(rec.event_id);
        m_scan_off += step;
        pos += step;
        if (s == Sent::kNever) {
          m_stats.unsendable++;
          continue;
        }
        advance(rec.event_id);
        sent++;
        m_stats.replayed++;
      }
      if (pos == 0) {
        /* No line break in this read. At the end of the file it is a torn
         * last line (the host seals it with the next append): stop. A short
         * read mid-file is a failed read: retry next pass. A full read with
         * no line break cannot be a record (a line is under kLineMax): it
         * is a damaged run — step over it and resync at the next '\n'. */
        if ((uint32_t)(m_scan_off + got) >= m_size) break;
        if (got < kReadChunk) {
          note_read_fail();
          break;
        }
        m_scan_off += (uint32_t)got;
        m_stats.skipped++;
      }
      m_read_fails = 0;
    }
    return finish(sent, link);
  }

  uint32_t watermark() const { return m_through; }
  uint32_t stored_ceiling() const { return m_stored; }
  uint32_t scan_offset() const { return m_scan_off; }
  uint32_t log_size() const { return m_size; }
  bool card_ok() const { return m_card_ok; }
  const Stats& stats() const { return m_stats; }

 private:
  void advance(uint32_t id) {
    if (id > m_through) m_through = id;
  }

  /* Before an id is handed over, NVS must already hold a ceiling above it
   * (csi_event_id_floor's policy). A failed write leaves m_stored unchanged
   * so the next hand-over tries again; the row still goes out — delivery
   * cannot wait on flash, the same trade the id floor makes. */
  void persist_for(uint32_t id, uint32_t id_floor, Port& port) {
    if (!csi_event_id_floor::must_persist(m_stored, id)) return;
    const uint32_t c = ceiling_for(id, id_floor);
    if (port.persist_ceiling(c)) m_stored = c;
  }

  /* kReadFailLimit failed reads in a row: leave the rest on the card (a
   * remount re-reads it) rather than hold new rows behind it forever. */
  void note_read_fail() {
    if (++m_read_fails < kReadFailLimit) return;
    m_scan_off = m_size;
    m_read_fails = 0;
    m_parked = false;
    m_stats.read_giveups++;
  }

  /* A retention truncation dropped `cut` bytes from the head of the log. */
  void on_cut(uint32_t cut) {
    m_parked = false;
    if (m_scan_off >= cut) {
      m_scan_off -= cut;
    } else {
      if (pending()) m_stats.truncated_unsent++;
      m_scan_off = 0;
    }
    m_size = (m_size > cut) ? m_size - cut : 0;
  }

  void note_fresh(uint32_t id) {
    m_fresh[m_fresh_next] = id;
    m_fresh_next = (m_fresh_next + 1) % kFreshIds;
  }
  bool is_fresh(uint32_t id) const {
    for (size_t i = 0; i < kFreshIds; ++i) {
      if (m_fresh[i] == id) return id != 0;
    }
    return false;
  }
  void forget_fresh(uint32_t id) {
    for (size_t i = 0; i < kFreshIds; ++i) {
      if (m_fresh[i] == id) m_fresh[i] = 0;
    }
  }

  size_t finish(size_t sent, const Link& link) {
    if (sent > 0) {
      m_sent_before = true;
      m_last_send_ms = link.now_ms;
    }
    return sent;
  }

  uint32_t m_through;      /* the watermark */
  uint32_t m_stored;       /* the ceiling NVS holds (0 = none) */
  bool     m_card_ok;
  uint32_t m_size;         /* the log's size in bytes */
  uint32_t m_scan_off;     /* where the next unread line starts */
  uint32_t m_card_max;     /* highest id known on the card */
  uint8_t  m_read_fails;
  bool     m_parked;       /* the walk stopped on a row it will send */
  bool     m_sent_before;
  uint32_t m_last_send_ms;
  uint32_t m_fresh[kFreshIds];
  size_t   m_fresh_next;
  Stats    m_stats;
  char     m_buf[kReadChunk + 1];  /* one card read, or one line to append */
};

}  // namespace csi_event_backfill

#endif  // SECURACV_CSI_EVENT_BACKFILL_H
