// Host tests for common/mqtt/mqtt_offline_queue.h — the bounded FIFO that
// holds discrete MQTT publishes (tamper alerts, events) across a broker
// outage. Pins the drop policy (oldest out, never truncate; a tamper alert
// outranks an event, so a burst of events cannot push a queued tamper
// out), the peek-then-pop drain contract (a failed replay keeps the
// record), FIFO order across wrap and across a mid-queue eviction, the
// inert behavior of a queue whose storage never arrived, and the
// publish-or-queue order the canary's MQTT layer runs (publish_or_queue():
// while the queue still drains, a new record joins its back, backlog F107).
//
// Build/run: make -C firmware/tests_host (the CI "host tests" job).

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "../common/mqtt/mqtt_offline_queue.h"

using mqtt_offline_queue::Kind;
using mqtt_offline_queue::Queue;

static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

int main() {
  // Storage for 4 slots of 32 payload bytes. Stride is header(4) + 32 + NUL.
  const size_t kPayload = 32;
  const size_t kStride = 4 + kPayload + 1;
  uint8_t storage[4 * kStride];

  // ── init carves the expected slot count; too-small storage is inert ──
  Queue q;
  CHECK(q.init(storage, sizeof(storage), kPayload));
  CHECK(q.capacity() == 4);
  CHECK(q.empty());

  Queue inert;
  CHECK(!inert.init(nullptr, 0, kPayload));
  CHECK(inert.capacity() == 0);
  CHECK(!inert.push(mqtt_offline_queue::KIND_EVENT, false, "x"));
  const char* p = nullptr;
  CHECK(!inert.front(nullptr, nullptr, &p));
  inert.pop_front();  // must not crash on empty/inert

  uint8_t tiny[3];
  Queue too_small;
  CHECK(!too_small.init(tiny, sizeof(tiny), kPayload));

  // ── push/front/pop round-trips payload, kind and retained flag ──
  CHECK(q.push(mqtt_offline_queue::KIND_TAMPER, true, "{\"state\":\"on\"}"));
  Kind kind = mqtt_offline_queue::KIND_EVENT;
  bool retained = false;
  CHECK(q.front(&kind, &retained, &p));
  CHECK(kind == mqtt_offline_queue::KIND_TAMPER);
  CHECK(retained);
  CHECK(std::strcmp(p, "{\"state\":\"on\"}") == 0);
  CHECK(q.size() == 1);

  // A failed replay pops nothing: front again returns the same record.
  const char* p2 = nullptr;
  CHECK(q.front(nullptr, nullptr, &p2));
  CHECK(std::strcmp(p2, "{\"state\":\"on\"}") == 0);
  q.pop_front();
  CHECK(q.empty());
  CHECK(q.stats().replayed == 1);

  // ── oversize payload is refused and counted, never truncated ──
  std::string big(kPayload + 1, 'a');
  CHECK(!q.push(mqtt_offline_queue::KIND_EVENT, false, big.c_str()));
  CHECK(q.stats().dropped_oversize == 1);
  CHECK(q.empty());
  std::string exact(kPayload, 'b');  // exactly at capacity fits
  CHECK(q.push(mqtt_offline_queue::KIND_EVENT, false, exact.c_str()));
  CHECK(q.front(nullptr, nullptr, &p));
  CHECK(exact == p);
  q.pop_front();

  // ── overflow drops the OLDEST; FIFO order holds across wrap ──
  CHECK(q.push(mqtt_offline_queue::KIND_EVENT, false, "e1"));
  CHECK(q.push(mqtt_offline_queue::KIND_EVENT, false, "e2"));
  CHECK(q.push(mqtt_offline_queue::KIND_EVENT, false, "e3"));
  CHECK(q.push(mqtt_offline_queue::KIND_EVENT, false, "e4"));
  CHECK(q.size() == 4);
  CHECK(q.push(mqtt_offline_queue::KIND_EVENT, false, "e5"));  // drops e1
  CHECK(q.size() == 4);
  CHECK(q.stats().dropped_overflow == 1);
  const char* expect[] = {"e2", "e3", "e4", "e5"};
  for (const char* want : expect) {
    CHECK(q.front(nullptr, nullptr, &p));
    CHECK(std::strcmp(p, want) == 0);
    q.pop_front();
  }
  CHECK(q.empty());

  // ── interleaved push/pop wraps the ring without corrupting order ──
  for (int round = 0; round < 3; ++round) {
    char a[8], b[8];
    std::snprintf(a, sizeof(a), "r%da", round);
    std::snprintf(b, sizeof(b), "r%db", round);
    CHECK(q.push(mqtt_offline_queue::KIND_TAMPER, false, a));
    CHECK(q.push(mqtt_offline_queue::KIND_EVENT, true, b));
    CHECK(q.front(&kind, &retained, &p));
    CHECK(std::strcmp(p, a) == 0);
    CHECK(kind == mqtt_offline_queue::KIND_TAMPER);
    CHECK(!retained);
    q.pop_front();
    CHECK(q.front(&kind, &retained, &p));
    CHECK(std::strcmp(p, b) == 0);
    CHECK(kind == mqtt_offline_queue::KIND_EVENT);
    CHECK(retained);
    q.pop_front();
  }
  CHECK(q.empty());

  // ── clear() discards contents, keeps capacity, counts the flush ──
  CHECK(q.push(mqtt_offline_queue::KIND_TAMPER, true, "old-broker-1"));
  CHECK(q.push(mqtt_offline_queue::KIND_EVENT, false, "old-broker-2"));
  CHECK(q.clear() == 2);
  CHECK(q.empty());
  CHECK(q.capacity() == 4);
  CHECK(q.stats().dropped_flushed == 2);
  CHECK(q.clear() == 0);  // idempotent on empty
  CHECK(q.stats().dropped_flushed == 2);
  // The queue still works after a flush (new broker's records).
  CHECK(q.push(mqtt_offline_queue::KIND_EVENT, false, "new-broker"));
  CHECK(q.front(nullptr, nullptr, &p));
  CHECK(std::strcmp(p, "new-broker") == 0);
  q.pop_front();

  // ── a tamper alert outranks an event on a full queue ──
  // The canary's egress publishes every committed csi_event through this
  // queue during a broker outage; routine rows must not evict a tamper.
  {
    uint8_t st4[4 * kStride];
    Queue t;
    CHECK(t.init(st4, sizeof(st4), kPayload));
    // Move the head off slot 0 first, so evictions cross the ring's wrap.
    CHECK(t.push(mqtt_offline_queue::KIND_EVENT, false, "pre1"));
    CHECK(t.push(mqtt_offline_queue::KIND_EVENT, false, "pre2"));
    CHECK(t.push(mqtt_offline_queue::KIND_EVENT, false, "pre3"));
    t.pop_front();
    t.pop_front();
    t.pop_front();
    CHECK(t.push(mqtt_offline_queue::KIND_TAMPER, false, "T1"));
    for (int i = 0; i < 20; ++i) {  // a burst far past capacity
      char e[8];
      std::snprintf(e, sizeof(e), "E%d", i);
      CHECK(t.push(mqtt_offline_queue::KIND_EVENT, false, e));
    }
    CHECK(t.size() == 4);
    CHECK(t.stats().dropped_overflow == 17);
    const char* want_burst[] = {"T1", "E17", "E18", "E19"};
    for (const char* want : want_burst) {
      CHECK(t.front(nullptr, nullptr, &p));
      CHECK(std::strcmp(p, want) == 0);
      t.pop_front();
    }
    CHECK(t.empty());

    // Mixed queue: the OLDEST event goes, everything else keeps its order,
    // kind and retained flag.
    CHECK(t.push(mqtt_offline_queue::KIND_EVENT, false, "E1"));
    CHECK(t.push(mqtt_offline_queue::KIND_TAMPER, true, "T1"));
    CHECK(t.push(mqtt_offline_queue::KIND_EVENT, false, "E2"));
    CHECK(t.push(mqtt_offline_queue::KIND_TAMPER, false, "T2"));
    CHECK(t.push(mqtt_offline_queue::KIND_TAMPER, false, "T3"));  // drops E1
    CHECK(t.push(mqtt_offline_queue::KIND_TAMPER, true, "T4"));   // drops E2
    // Only tamper alerts left: a new event is refused, not admitted...
    CHECK(!t.push(mqtt_offline_queue::KIND_EVENT, false, "E3"));
    CHECK(t.size() == 4);
    // ...and a new tamper alert displaces the oldest tamper alert.
    CHECK(t.push(mqtt_offline_queue::KIND_TAMPER, false, "T5"));  // drops T1
    CHECK(t.stats().dropped_overflow == 17 + 4);
    const char* want_mixed[] = {"T2", "T3", "T4", "T5"};
    const bool want_retained[] = {false, false, true, false};
    for (int i = 0; i < 4; ++i) {
      CHECK(t.front(&kind, &retained, &p));
      CHECK(kind == mqtt_offline_queue::KIND_TAMPER);
      CHECK(retained == want_retained[i]);
      CHECK(std::strcmp(p, want_mixed[i]) == 0);
      t.pop_front();
    }
    CHECK(t.empty());

    // An event in the middle goes; the tamper alerts around it stay put.
    CHECK(t.push(mqtt_offline_queue::KIND_TAMPER, false, "A"));
    CHECK(t.push(mqtt_offline_queue::KIND_TAMPER, false, "B"));
    CHECK(t.push(mqtt_offline_queue::KIND_EVENT, false, "e"));
    CHECK(t.push(mqtt_offline_queue::KIND_TAMPER, false, "C"));
    CHECK(t.push(mqtt_offline_queue::KIND_EVENT, false, "f"));  // drops e
    const char* want_mid[] = {"A", "B", "C", "f"};
    for (const char* want : want_mid) {
      CHECK(t.front(nullptr, nullptr, &p));
      CHECK(std::strcmp(p, want) == 0);
      t.pop_front();
    }
    CHECK(t.empty());
  }

  // The review's case at the canary's real geometry (MQTT_OFFLINE_SLOTS 12
  // x MQTT_OFFLINE_SLOT_BYTES 512): one queued SD alert, then a dozen
  // committed events while the broker is gone. The alert is still first.
  {
    const size_t kSlots = 12, kSlotBytes = 512;
    static uint8_t big_storage[12 * (4 + 512 + 1)];
    Queue c;
    CHECK(c.init(big_storage, sizeof(big_storage), kSlotBytes));
    CHECK(c.capacity() == kSlots);
    CHECK(c.push(mqtt_offline_queue::KIND_TAMPER, false,
                 "{\"type\":\"sd_remove\",\"severity\":\"tamper\"}"));
    for (int i = 0; i < 12; ++i) {
      CHECK(c.push(mqtt_offline_queue::KIND_EVENT, false, "{\"event_id\":1}"));
    }
    CHECK(c.stats().dropped_overflow == 1);
    CHECK(c.front(&kind, nullptr, &p));
    CHECK(kind == mqtt_offline_queue::KIND_TAMPER);
    CHECK(std::strstr(p, "sd_remove") != nullptr);
  }

  // ── a long broker outage never stalls the producer ──
  // ENTERPRISE_READINESS_TODO §6 "Operational failover": buffering must not
  // block witness generation. The queue's half of that promise: with the
  // broker down and NOTHING draining, every fitting push is accepted at once
  // (it never waits for, or refuses on, a consumer), the queue stays at its
  // bound, each overflow is counted, and what survives is the newest records
  // in order. (The chain itself never touches this queue — the MQTT layer is
  // its only caller.)
  {
    Queue outage;
    CHECK(outage.init(storage, sizeof(storage), kPayload));
    const uint32_t kPushes = 10000;
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < kPushes; ++i) {
      char rec[16];
      std::snprintf(rec, sizeof(rec), "ev%u", (unsigned)i);
      if (outage.push(mqtt_offline_queue::KIND_EVENT, false, rec)) ++accepted;
      CHECK(outage.size() <= outage.capacity());
    }
    CHECK(accepted == kPushes);
    CHECK(outage.size() == outage.capacity());
    CHECK(outage.stats().queued == kPushes);
    CHECK(outage.stats().dropped_overflow == kPushes - outage.capacity());
    CHECK(outage.stats().replayed == 0);
    for (uint32_t i = kPushes - (uint32_t)outage.capacity(); i < kPushes; ++i) {
      char want[16];
      std::snprintf(want, sizeof(want), "ev%u", (unsigned)i);
      CHECK(outage.front(nullptr, nullptr, &p));
      CHECK(std::strcmp(p, want) == 0);
      outage.pop_front();
    }
    CHECK(outage.empty());
  }

  // ── publish_or_queue(): the order a discrete record goes out in ──
  // Backlog F107: while the queue still drains an outage, a new record
  // joins its back instead of going live, so it never overtakes an older
  // queued event (Home Assistant's replay gate would refuse that one) or a
  // queued tamper alert. securacv_mqtt.cpp's events and tamper surfaces run
  // exactly this function; the canary's egress relies on it for every row
  // it hands over off the card.
  {
    Queue o;
    CHECK(o.init(storage, sizeof(storage), kPayload));
    std::string sent;          // what went out live, in order
    int sends = 0;
    int ensures = 0;
    bool send_ok = true;
    auto publish = [&](bool link_up, Kind k, const char* payload) {
      return mqtt_offline_queue::publish_or_queue(
          o, link_up, k, false, payload,
          [&]() {
            ++sends;
            if (send_ok) sent += std::string(payload) + ";";
            return send_ok;
          },
          [&]() { ++ensures; });
    };
    auto drain = [&]() {
      std::string out;
      const char* rec = nullptr;
      while (o.front(nullptr, nullptr, &rec)) {
        out += std::string(rec) + ";";
        o.pop_front();
      }
      return out;
    };

    // Link up, nothing queued: live, the queue untouched.
    CHECK(publish(true, mqtt_offline_queue::KIND_EVENT, "e1"));
    CHECK(sent == "e1;" && o.empty() && ensures == 0);

    // Link down: queued, never sent.
    sent.clear();
    sends = 0;
    CHECK(publish(false, mqtt_offline_queue::KIND_EVENT, "e2"));
    CHECK(publish(false, mqtt_offline_queue::KIND_TAMPER, "t3"));
    CHECK(sends == 0 && o.size() == 2 && ensures == 2);

    // The link is back and the outage still drains: a new event and a new
    // tamper alert join the back, behind e2 and t3, and nothing goes live.
    CHECK(publish(true, mqtt_offline_queue::KIND_EVENT, "e4"));
    CHECK(publish(true, mqtt_offline_queue::KIND_TAMPER, "t5"));
    CHECK(sends == 0 && sent.empty());
    CHECK(drain() == "e2;t3;e4;t5;");

    // Link up, nothing queued, the send fails: queued for the drain.
    send_ok = false;
    CHECK(publish(true, mqtt_offline_queue::KIND_EVENT, "e6"));
    CHECK(sends == 1 && o.size() == 1);
    send_ok = true;
    // ...and the next record waits behind it rather than overtaking it.
    CHECK(publish(true, mqtt_offline_queue::KIND_EVENT, "e7"));
    CHECK(sends == 1 && drain() == "e6;e7;");

    // What the queue refuses goes live while the link is up (delivery beats
    // ordering there): a payload over the slot size, and an event a full
    // queue of tamper alerts refuses.
    sends = 0;
    sent.clear();
    CHECK(publish(false, mqtt_offline_queue::KIND_EVENT, "e8"));
    const std::string oversize(kPayload + 1, 'x');
    CHECK(publish(true, mqtt_offline_queue::KIND_EVENT, oversize.c_str()));
    CHECK(sends == 1 && sent == oversize + ";");
    CHECK(drain() == "e8;");
    for (int i = 0; i < 4; ++i) CHECK(publish(false, mqtt_offline_queue::KIND_TAMPER, "t"));
    CHECK(o.size() == o.capacity());
    sent.clear();
    CHECK(publish(true, mqtt_offline_queue::KIND_EVENT, "e9"));
    CHECK(sent == "e9;" && o.size() == o.capacity());
    // With the link down the same event is refused, so the caller re-arms.
    CHECK(!publish(false, mqtt_offline_queue::KIND_EVENT, "e10"));
    CHECK(drain() == "t;t;t;t;");

    // No payload: nothing sent, nothing queued.
    sends = 0;
    ensures = 0;
    CHECK(!publish(true, mqtt_offline_queue::KIND_EVENT, nullptr));
    CHECK(sends == 0 && ensures == 0 && o.empty());

    // An inert queue (its storage never arrived): the storage hook runs, the
    // push refuses, and the caller is told.
    Queue none;
    CHECK(!mqtt_offline_queue::publish_or_queue(
        none, false, mqtt_offline_queue::KIND_EVENT, false, "e11",
        []() { return true; }, [&]() { ++ensures; }));
    CHECK(ensures == 1);
  }

  // ── re-init resets contents and counters ──
  CHECK(q.push(mqtt_offline_queue::KIND_EVENT, false, "stale"));
  CHECK(q.init(storage, sizeof(storage), kPayload));
  CHECK(q.empty());
  CHECK(q.stats().queued == 0 && q.stats().replayed == 0 &&
        q.stats().dropped_overflow == 0 && q.stats().dropped_oversize == 0 &&
        q.stats().dropped_flushed == 0);

  std::printf("OK test_mqtt_offline_queue (%d checks)\n", g_checks);
  return 0;
}
