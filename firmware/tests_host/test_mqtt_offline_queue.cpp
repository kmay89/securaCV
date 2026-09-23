// Host tests for common/mqtt/mqtt_offline_queue.h — the bounded FIFO that
// holds discrete MQTT publishes (tamper alerts, events) across a broker
// outage. Pins the drop policy (oldest out, never truncate), the
// peek-then-pop drain contract (a failed replay keeps the record), FIFO
// order across wrap, and the inert behavior of a queue whose storage never
// arrived.
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
