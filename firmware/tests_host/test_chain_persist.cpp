/* Host tests for the chain-state persist: the decision in
 * firmware/common/witness/chain_persist.h, and securacv_witness.cpp's own
 * glue around it (persist_chain_blob(), witness_persist_chain_state() and
 * persist_chain_if_due()), cut out verbatim by cut_functions.awk (the Makefile writes
 * build/chain_persist_glue.inc) and compiled over a fake NVS.
 *
 * THE FAILURE THIS PREVENTS (repo sweep F55). witness_persist_chain_state()
 * did not read persist_chain_blob()'s result: it moved seq_persisted up to
 * seq and counted a persist whatever the write did. A write that did not
 * land was forgotten. Nothing counted or logged it, the next attempt waited
 * a whole interval, and in that window NVS held an older head than the
 * device believed it did. The suite is organized by that failure: a failed
 * write must leave seq_persisted where NVS is, be counted apart from the
 * writes that landed, be tried again after the next record, and be reported
 * once per streak.
 *
 * What this does not prove: the NVS itself, the record path around
 * persist_chain_if_due() (the Makefile checks witness_create_record_gps()
 * still calls it), the genesis and boot-count writes in
 * witness_provision_device(), or anything on a device. The canary envs
 * compile the real thing in CI; it has not run on a bench.
 *
 * Build & run: make -C firmware/tests_host (the cut needs the Makefile).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "securacv_witness.h"  // the real DeviceIdentity / SystemHealth, over stubs/witness_glue
#include "canary_config.h"
#include "witness/chain_persist.h"
#include "witness/chain_state.h"

static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

using chain_persist::Say;

// ════════════════════════════════════════════════════════════════════════════
// PART 1 — the decision (chain_persist.h)
// ════════════════════════════════════════════════════════════════════════════

// ── due(): the interval as before, and the retry after a failure ────────────
static void test_due_follows_the_interval_and_the_streak() {
  CHECK(!chain_persist::due(9, 0, false, 10));    // gap 9: not yet
  CHECK(chain_persist::due(10, 0, false, 10));    // gap 10: due
  CHECK(chain_persist::due(25, 0, false, 10));    // gap past it: due
  CHECK(!chain_persist::due(20, 20, false, 10));  // just persisted
  // Rule 2: a failed attempt is retried after the next record, whatever
  // the gap, even one far under the interval (a failed pre-restart persist).
  CHECK(chain_persist::due(21, 20, true, 10));
  CHECK(chain_persist::due(20, 20, true, 10));
  // Wrap-safe: seq wrapped past 0, the gap is still 7 / 10.
  CHECK(!chain_persist::due(2u, 0xFFFFFFFBu, false, 10));
  CHECK(chain_persist::due(5u, 0xFFFFFFFBu, false, 10));
}

// ── settle(): only a landed write moves seq_persisted and counts as one ─────
static void test_settle_moves_and_counts_only_what_landed() {
  uint32_t persisted = 10, persists = 0, failures = 0;
  bool failing = false;

  CHECK(chain_persist::settle(20, true, &persisted, &failing, &persists, &failures) ==
        Say::Nothing);
  CHECK(persisted == 20 && persists == 1 && failures == 0 && !failing);

  // The failure: the write did not land. seq_persisted stays where NVS is.
  CHECK(chain_persist::settle(30, false, &persisted, &failing, &persists, &failures) ==
        Say::Failed);
  CHECK(persisted == 20 && persists == 1 && failures == 1 && failing);

  // Retries inside the streak: counted, not reported.
  CHECK(chain_persist::settle(31, false, &persisted, &failing, &persists, &failures) ==
        Say::Nothing);
  CHECK(chain_persist::settle(32, false, &persisted, &failing, &persists, &failures) ==
        Say::Nothing);
  CHECK(persisted == 20 && persists == 1 && failures == 3 && failing);

  // The first write to land closes the streak, and says so once.
  CHECK(chain_persist::settle(33, true, &persisted, &failing, &persists, &failures) ==
        Say::Recovered);
  CHECK(persisted == 33 && persists == 2 && failures == 3 && !failing);
  CHECK(chain_persist::settle(43, true, &persisted, &failing, &persists, &failures) ==
        Say::Nothing);
  CHECK(persisted == 43 && persists == 3);

  // A second streak is reported again.
  CHECK(chain_persist::settle(53, false, &persisted, &failing, &persists, &failures) ==
        Say::Failed);
  CHECK(persisted == 43 && failures == 4 && failing);
}

// ── settle() with a null argument changes nothing ───────────────────────────
static void test_settle_refuses_a_null_argument() {
  uint32_t persisted = 5, persists = 1, failures = 2;
  bool failing = false;
  CHECK(chain_persist::settle(9, false, nullptr, &failing, &persists, &failures) ==
        Say::Nothing);
  CHECK(chain_persist::settle(9, true, &persisted, nullptr, &persists, &failures) ==
        Say::Nothing);
  CHECK(chain_persist::settle(9, true, &persisted, &failing, nullptr, &failures) ==
        Say::Nothing);
  CHECK(chain_persist::settle(9, false, &persisted, &failing, &persists, nullptr) ==
        Say::Nothing);
  CHECK(persisted == 5 && persists == 1 && failures == 2 && !failing);
}

// ════════════════════════════════════════════════════════════════════════════
// PART 2 — securacv_witness.cpp's own glue, over a fake NVS
// ════════════════════════════════════════════════════════════════════════════

// The cut functions' file-level state, as securacv_witness.cpp declares it.
static DeviceIdentity g_device;
static SystemHealth g_health;

// A fake NVS: the chain blob. A put to a refused key (or any put, with
// refuse_all) fails the way nvs_store_bytes now reports it.
struct FakeNvs {
  bool refuse_all = false;
  const char* refuse_key = nullptr;
  int chain_puts = 0;
  int unexpected = 0;  // a put to any other key, or a blob of the wrong size
  bool blob_present = false;
  uint8_t blob[chain_state::BLOB_LEN] = {0};
};
static FakeNvs g_nvs;

static bool refused(const char* key) {
  return g_nvs.refuse_all ||
         (g_nvs.refuse_key != nullptr && std::strcmp(key, g_nvs.refuse_key) == 0);
}

bool nvs_store_bytes(const char* key, const uint8_t* data, size_t len) {
  if (std::strcmp(key, NVS_KEY_CHAINST) != 0 || len != chain_state::BLOB_LEN) {
    g_nvs.unexpected++;
    return false;
  }
  g_nvs.chain_puts++;
  if (refused(key)) return false;
  std::memcpy(g_nvs.blob, data, len);
  g_nvs.blob_present = true;
  return true;
}

// The health log: what the glue reported, and how often.
struct Reported {
  int count = 0;
  int warnings = 0;
  int notices = 0;
  char message[80] = {0};
  char detail[48] = {0};
};
static Reported g_log;

void log_health(LogLevel level, LogCategory category, const char* message,
                const char* detail) {
  g_log.count++;
  if (level == LOG_LEVEL_WARNING) g_log.warnings++;
  if (level == LOG_LEVEL_NOTICE) g_log.notices++;
  CHECK(category == LOG_CAT_STORAGE);
  std::snprintf(g_log.message, sizeof(g_log.message), "%s", message ? message : "");
  std::snprintf(g_log.detail, sizeof(g_log.detail), "%s", detail ? detail : "");
}

// securacv_witness.cpp's own functions.
#include "chain_persist_glue.inc"

// The seq NVS holds, decoded as the next boot would.
static uint32_t nvs_seq() {
  uint32_t seq = 0xDEADBEEFu;
  uint8_t head[chain_state::HEAD_LEN];
  CHECK(g_nvs.blob_present);
  CHECK(chain_state::decode(g_nvs.blob, sizeof(g_nvs.blob), &seq, head));
  return seq;
}

// One record, as witness_create_record_gps() ends one: the chain advances,
// then the persist runs if due.
static void make_record() {
  g_device.seq++;
  g_device.chain_head[0] = (uint8_t)g_device.seq;
  persist_chain_if_due();
}

static void reset_chain(uint32_t seq) {
  g_device = DeviceIdentity();
  g_health = SystemHealth();
  g_nvs = FakeNvs();
  g_log = Reported();
  g_device.seq = seq;
  g_device.seq_persisted = seq;
  witness_persist_chain_state();  // the chain state NVS starts from
  CHECK(nvs_seq() == seq);
  g_health = SystemHealth();
  g_nvs.chain_puts = 0;
}

// ── a routine persist: every SD_PERSIST_INTERVAL records, as before ─────────
static void test_routine_persist_every_interval() {
  reset_chain(0);
  for (uint32_t i = 1; i < SD_PERSIST_INTERVAL; ++i) make_record();
  CHECK(g_nvs.chain_puts == 0);                    // not due yet
  make_record();
  CHECK(g_nvs.chain_puts == 1);
  CHECK(nvs_seq() == SD_PERSIST_INTERVAL);
  CHECK(g_device.seq_persisted == SD_PERSIST_INTERVAL);
  CHECK(g_health.chain_persists == 1 && g_health.chain_persist_failures == 0);
  CHECK(!g_device.chain_persist_failing);
  CHECK(g_log.count == 0);                         // a landed write says nothing
  CHECK(g_nvs.unexpected == 0);
}

// ── the failure: a write that did not land is held, counted, retried ────────
static void test_a_failed_persist_is_retried_not_forgotten() {
  reset_chain(0);
  for (uint32_t i = 1; i < SD_PERSIST_INTERVAL; ++i) make_record();
  g_nvs.refuse_all = true;
  make_record();                                   // due at seq 10: refused
  CHECK(g_nvs.chain_puts == 1);
  CHECK(nvs_seq() == 0);                           // NVS still holds seq 0...
  CHECK(g_device.seq_persisted == 0);              // ...and the device says so
  CHECK(g_device.chain_persist_failing);
  CHECK(g_health.chain_persists == 0 && g_health.chain_persist_failures == 1);
  CHECK(g_log.count == 1 && g_log.warnings == 1);  // reported once
  CHECK(std::strstr(g_log.message, "not written to NVS") != nullptr);
  CHECK(std::strstr(g_log.detail, "seq 10") != nullptr);

  // The next record retries at once, not an interval later; the streak is
  // not reported again.
  make_record();
  make_record();
  CHECK(g_nvs.chain_puts == 3);
  CHECK(g_health.chain_persist_failures == 3);
  CHECK(g_device.seq_persisted == 0 && nvs_seq() == 0);
  CHECK(g_log.count == 1);

  // NVS takes writes again: the next record lands the newest state.
  g_nvs.refuse_all = false;
  make_record();
  CHECK(g_nvs.chain_puts == 4);
  CHECK(nvs_seq() == SD_PERSIST_INTERVAL + 3);
  CHECK(g_device.seq_persisted == SD_PERSIST_INTERVAL + 3);
  CHECK(!g_device.chain_persist_failing);
  CHECK(g_health.chain_persists == 1 && g_health.chain_persist_failures == 3);
  CHECK(g_log.count == 2 && g_log.notices == 1);   // the recovery, once
  CHECK(std::strstr(g_log.message, "written to NVS again") != nullptr);
  CHECK(std::strstr(g_log.detail, "3 failed") != nullptr);

  // ...and the routine interval resumes from there.
  for (uint32_t i = 1; i < SD_PERSIST_INTERVAL; ++i) make_record();
  CHECK(g_nvs.chain_puts == 4);
  make_record();
  CHECK(g_nvs.chain_puts == 5 && g_log.count == 2);
  CHECK(g_nvs.unexpected == 0);
}

// ── a failed persist outside the interval (before a restart) is retried too ─
static void test_a_failed_direct_persist_is_retried_after_the_next_record() {
  reset_chain(20);
  make_record();
  make_record();                                   // seq 22, gap 2: not due
  CHECK(g_nvs.chain_puts == 0);
  g_nvs.refuse_all = true;
  witness_persist_chain_state();                   // the reboot / OTA / sleep path
  CHECK(g_device.seq_persisted == 20 && nvs_seq() == 20);
  CHECK(g_health.chain_persist_failures == 1 && g_log.warnings == 1);
  g_nvs.refuse_all = false;
  make_record();                                   // gap 3, but a retry is owed
  CHECK(g_nvs.chain_puts == 2);
  CHECK(nvs_seq() == 23 && g_device.seq_persisted == 23);
  CHECK(!g_device.chain_persist_failing && g_log.notices == 1);
}

// ── the failure, run long: NVS never lags a full interval unnoticed ─────────
//
// A deterministic mix of records with NVS refusing about one write in four,
// in streaks. After every record: seq_persisted is exactly the seq NVS holds;
// a record that leaves NVS an interval or more behind the chain has just
// tried to write; every attempt is counted once, on one side; and the health
// log holds one line per streak opened and one per streak closed. The old
// glue (advance and count whatever the write did) breaks the first two on
// the first refused write.
static void test_nvs_never_lags_an_interval_without_a_retry() {
  reset_chain(0);
  uint32_t x = 0x9E3779B9u;  // xorshift32, fixed seed: the run is reproducible
  auto next = [&x]() {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
  };
  int streaks = 0, recoveries = 0, refused_steps = 0;
  bool was_failing = false;
  for (int step = 0; step < 50000; ++step) {
    const uint32_t r = next();
    // Change the NVS's mood now and then, so refusals come in streaks.
    if ((r % 16u) == 0) g_nvs.refuse_all = ((r >> 4) % 4u) == 0;
    if (g_nvs.refuse_all) refused_steps++;
    const int puts_before = g_nvs.chain_puts;
    make_record();
    const bool attempted = g_nvs.chain_puts != puts_before;
    CHECK(g_nvs.chain_puts <= puts_before + 1);    // at most one write per record

    CHECK(g_device.seq_persisted == nvs_seq());
    if (g_device.seq - nvs_seq() >= SD_PERSIST_INTERVAL) CHECK(attempted);
    CHECK((uint32_t)g_nvs.chain_puts ==
          g_health.chain_persists + g_health.chain_persist_failures);
    if (attempted && g_device.chain_persist_failing && !was_failing) streaks++;
    if (attempted && !g_device.chain_persist_failing && was_failing) recoveries++;
    was_failing = g_device.chain_persist_failing;
    if (g_failures > 20) break;  // one broken invariant floods; stop early
  }
  CHECK(g_log.warnings == streaks && g_log.notices == recoveries);
  CHECK(g_log.count == streaks + recoveries);
  CHECK(streaks > 100 && recoveries > 100);       // the mix really failed and healed
  CHECK(refused_steps > 5000);
  CHECK(g_health.chain_persist_failures > (uint32_t)streaks);  // retries inside streaks
  CHECK(g_nvs.unexpected == 0);
}

int main() {
  test_due_follows_the_interval_and_the_streak();
  test_settle_moves_and_counts_only_what_landed();
  test_settle_refuses_a_null_argument();
  test_routine_persist_every_interval();
  test_a_failed_persist_is_retried_not_forgotten();
  test_a_failed_direct_persist_is_retried_after_the_next_record();
  test_nvs_never_lags_an_interval_without_a_retry();

  if (g_failures == 0) { std::printf("ALL chain-persist tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
