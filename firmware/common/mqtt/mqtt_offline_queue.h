/*
 * SecuraCV Canary — MQTT offline publish queue (pure logic)
 *
 * Bounded FIFO of publish records that could not go out because the broker
 * link was down (or the send failed). The MQTT layer pushes on a failed or
 * link-less publish and drains front-to-back once the link is up, so a
 * broker outage delays discrete messages instead of dropping them.
 *
 * What belongs here: discrete, loss-mattering messages — tamper alerts
 * (HA's edge-latching sensors and the sealed-log adapter consume each
 * one), witness/CSI events. What does not: periodic snapshots (status,
 * health, sensing) — their next tick republishes fresher truth, and a
 * replayed stale snapshot is worse than none.
 *
 * Bounds and drop policy: fixed slot count and per-slot payload capacity
 * in caller-provided storage — this header allocates nothing. When full,
 * a tamper alert outranks an event: the OLDEST event is dropped to admit
 * the new record; with no event queued, a new tamper alert drops the
 * oldest tamper alert (the newest are the ones a responder acts on), and
 * a new event is refused. So routine traffic cannot push a queued tamper
 * alert out, however fast it commits. Every drop and refusal is counted.
 * Order among the records kept is unchanged. A payload over the slot
 * capacity is refused and counted, never truncated — a truncated JSON
 * payload would parse as junk downstream.
 *
 * Pure: no Arduino, no globals, no clock. Single-task use (the MQTT loop
 * task owns push and drain both) — no locking inside. Host-tested by
 * firmware/tests_host/test_mqtt_offline_queue.cpp.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_MQTT_OFFLINE_QUEUE_H
#define SECURACV_MQTT_OFFLINE_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace mqtt_offline_queue {

/* Which topic the record replays to. The queue stores the tag, not the
 * topic string — topics are per-device and live in the MQTT layer. */
enum Kind : uint8_t {
  KIND_EVENT  = 0,
  KIND_TAMPER = 1,
};

struct Stats {
  uint32_t queued;            /* pushes accepted (lifetime) */
  uint32_t replayed;          /* records drained by pop after a publish */
  uint32_t dropped_overflow;  /* records dropped or refused on a full queue */
  uint32_t dropped_oversize;  /* pushes refused: payload over slot capacity */
  uint32_t dropped_flushed;   /* records discarded by clear() (broker changed) */
};

/* Per-slot overhead: the header the class stores ahead of each payload,
 * asserted against the real struct inside the class. The storage a caller
 * must provide for N slots of P payload bytes is N * slot_stride(P). */
constexpr size_t kSlotHeaderBytes = 4;
constexpr size_t slot_stride(size_t slot_payload) {
  return kSlotHeaderBytes + slot_payload + 1 /* NUL */;
}

class Queue {
 public:
  Queue() : m_slots(nullptr), m_slot_stride(0), m_slot_payload(0),
            m_slot_count(0), m_head(0), m_size(0), m_stats{} {}

  /* Carve `storage` into fixed slots holding up to `slot_payload` payload
   * bytes each (stored NUL-terminated). Returns false when the storage
   * cannot hold even one slot; the queue then stays inert (push refuses,
   * front reports empty). Re-init resets contents and counters. */
  bool init(void* storage, size_t storage_bytes, size_t slot_payload) {
    static_assert(sizeof(SlotHeader) == kSlotHeaderBytes,
                  "slot_stride() must match the stored header");
    m_slots = static_cast<uint8_t*>(storage);
    m_slot_payload = slot_payload;
    m_slot_stride = slot_stride(slot_payload);
    m_slot_count = (m_slots && slot_payload > 0) ? storage_bytes / m_slot_stride : 0;
    m_head = 0;
    m_size = 0;
    m_stats = Stats{};
    return m_slot_count > 0;
  }

  /* Number of slots the storage yielded (0 = inert). */
  size_t capacity() const { return m_slot_count; }
  size_t size() const { return m_size; }
  bool empty() const { return m_size == 0; }

  /* Enqueue one payload. On a full queue the oldest EVENT makes room;
   * with none queued, a tamper alert displaces the oldest tamper alert and
   * an event is refused (all counted in dropped_overflow). Refuses
   * (counted) a payload over the slot capacity, and an inert queue. */
  bool push(Kind kind, bool retained, const char* payload) {
    if (m_slot_count == 0 || payload == nullptr) return false;
    const size_t len = strlen(payload);
    if (len > m_slot_payload) {
      m_stats.dropped_oversize++;
      return false;
    }
    if (m_size == m_slot_count) {
      size_t victim = 0;  /* logical index; 0 = oldest */
      if (!oldest_of(KIND_EVENT, &victim) && kind != KIND_TAMPER) {
        m_stats.dropped_overflow++;  /* never displace a tamper alert */
        return false;
      }
      remove_at(victim);
      m_stats.dropped_overflow++;
    }
    uint8_t* slot = slot_at((m_head + m_size) % m_slot_count);
    SlotHeader hdr;
    hdr.len = static_cast<uint16_t>(len);
    hdr.kind = static_cast<uint8_t>(kind);
    hdr.retained = retained ? 1 : 0;
    memcpy(slot, &hdr, sizeof(hdr));
    memcpy(slot + sizeof(hdr), payload, len);
    slot[sizeof(hdr) + len] = '\0';
    m_size++;
    m_stats.queued++;
    return true;
  }

  /* Peek the oldest record without removing it, so the caller can attempt
   * the publish and only pop on success — a failed replay keeps the
   * record for the next drain pass. The returned pointer is valid until
   * the next push/pop/init. */
  bool front(Kind* kind, bool* retained, const char** payload) const {
    if (m_size == 0) return false;
    const uint8_t* slot = slot_at(m_head);
    SlotHeader hdr;
    memcpy(&hdr, slot, sizeof(hdr));
    if (kind)     *kind = static_cast<Kind>(hdr.kind);
    if (retained) *retained = hdr.retained != 0;
    if (payload)  *payload = reinterpret_cast<const char*>(slot + sizeof(hdr));
    return true;
  }

  /* Remove the oldest record (after a successful replay). */
  void pop_front() {
    if (m_size == 0) return;
    m_head = (m_head + 1) % m_slot_count;
    m_size--;
    m_stats.replayed++;
  }

  /* Discard everything queued, keeping storage, capacity and lifetime
   * counters. For when the records' destination is gone: a queue filled
   * during an outage against broker A must not drain to a reprovisioned
   * broker B — stale security signals are not the new endpoint's to see.
   * Returns how many records were discarded (also counted in
   * stats().dropped_flushed). */
  size_t clear() {
    const size_t discarded = m_size;
    m_head = 0;
    m_size = 0;
    m_stats.dropped_flushed += static_cast<uint32_t>(discarded);
    return discarded;
  }

  const Stats& stats() const { return m_stats; }

 private:
  struct SlotHeader {
    uint16_t len;
    uint8_t  kind;
    uint8_t  retained;
  };

  uint8_t* slot_at(size_t index) const { return m_slots + index * m_slot_stride; }

  /* Physical slot of the record `logical` places behind the oldest. */
  size_t phys(size_t logical) const { return (m_head + logical) % m_slot_count; }

  /* Logical index of the oldest queued record of `kind`, if any. */
  bool oldest_of(Kind kind, size_t* logical) const {
    for (size_t i = 0; i < m_size; ++i) {
      SlotHeader hdr;
      memcpy(&hdr, slot_at(phys(i)), sizeof(hdr));
      if (hdr.kind == static_cast<uint8_t>(kind)) {
        *logical = i;
        return true;
      }
    }
    return false;
  }

  /* Drop the record at `logical`, keeping the others in order: the
   * records older than it each move one slot toward the tail, then the
   * head advances past the freed slot. Rare (only on a full queue), and
   * at most slot_count-1 slot copies. */
  void remove_at(size_t logical) {
    for (size_t i = logical; i > 0; --i) {
      memcpy(slot_at(phys(i)), slot_at(phys(i - 1)), m_slot_stride);
    }
    m_head = (m_head + 1) % m_slot_count;
    m_size--;
  }

  uint8_t* m_slots;
  size_t   m_slot_stride;
  size_t   m_slot_payload;
  size_t   m_slot_count;
  size_t   m_head;
  size_t   m_size;
  Stats    m_stats;
};

}  /* namespace mqtt_offline_queue */

#endif  /* SECURACV_MQTT_OFFLINE_QUEUE_H */
