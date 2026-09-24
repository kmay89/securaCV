/* Host tests for the chain-state persist: the decision in
 * firmware/common/witness/chain_persist.h, and securacv_witness.cpp's own
 * glue around it (persist_chain_blob(), witness_persist_chain_state(),
 * persist_chain_if_due() and the birth stamp's witness_note_wall_clock()),
 * cut out verbatim by cut_functions.awk (the Makefile writes
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
 * once per streak. And the retry must not become its own failure: a lasting
 * one (a full partition, or a leaked NVS session whose every attempt waits
 * 2 s on the loop) is retried once per interval after the prompt retry, not
 * on every record.
 *
 * The birth stamp (witness_note_wall_clock) is the other caller that reads
 * nvs_store_u32 now. Its caller runs every loop pass, so a failed stamp must
 * not claim a birth NVS lacks, must not write the day after a flag that did
 * not land, and must wait a minute between attempts.
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
#include "identity/birth_day.h"
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

// A streak as settle() leaves it: opened by a failure at `failed_seq`, and
// `retried` once the prompt retry has failed too.
static chain_persist::Streak streak(bool failing, bool retried, uint32_t failed_seq) {
  chain_persist::Streak s{};
  s.failing = failing;
  s.retried = retried;
  s.failed_seq = failed_seq;
  return s;
}

// ── due(): the interval as before, and the retry after a failure ────────────
static void test_due_follows_the_interval_with_no_streak() {
  const chain_persist::Streak none{};
  CHECK(!chain_persist::due(9, 0, none, 10));    // gap 9: not yet
  CHECK(chain_persist::due(10, 0, none, 10));    // gap 10: due
  CHECK(chain_persist::due(25, 0, none, 10));    // gap past it: due
  CHECK(!chain_persist::due(20, 20, none, 10));  // just persisted
  // Wrap-safe: seq wrapped past 0, the gap is still 7 / 10.
  CHECK(!chain_persist::due(2u, 0xFFFFFFFBu, none, 10));
  CHECK(chain_persist::due(5u, 0xFFFFFFFBu, none, 10));
}

// Rule 2: the failure that opens a streak is retried on the next record,
// whatever the gap (a failed pre-restart persist sits far under it)...
static void test_due_retries_the_opening_failure_on_the_next_record() {
  CHECK(chain_persist::due(21, 20, streak(true, false, 20), 10));   // gap 1
  CHECK(chain_persist::due(23, 20, streak(true, false, 22), 10));  // gap 3
  CHECK(chain_persist::due(11, 0, streak(true, false, 10), 10));   // gap 11
  // Wrap-safe: the failure was at 0xFFFFFFFF, the next record is seq 0.
  CHECK(chain_persist::due(0u, 0xFFFFFFF6u, streak(true, false, 0xFFFFFFFFu), 10));
}

// ...and once that retry fails too, once per interval from the last failed
// attempt, however far behind NVS is: a lasting failure is not paid on
// every record.
static void test_due_backs_off_to_the_interval_after_the_prompt_retry() {
  const chain_persist::Streak s = streak(true, true, 11);  // failed at 10, then 11
  for (uint32_t seq = 12; seq < 21; ++seq) CHECK(!chain_persist::due(seq, 0, s, 10));
  CHECK(chain_persist::due(21, 0, s, 10));
  CHECK(chain_persist::due(40, 0, s, 10));       // overdue: still due
  // The gap to seq_persisted is not what counts while a streak is open.
  CHECK(!chain_persist::due(1000, 0, streak(true, true, 995), 10));
  // Wrap-safe: last failed at 0xFFFFFFFC, due 10 records on (seq 6).
  CHECK(!chain_persist::due(5u, 0u, streak(true, true, 0xFFFFFFFCu), 10));
  CHECK(chain_persist::due(6u, 0u, streak(true, true, 0xFFFFFFFCu), 10));
}

// ── settle(): only a landed write moves seq_persisted and counts as one ─────
static void test_settle_moves_and_counts_only_what_landed() {
  uint32_t persisted = 10, persists = 0, failures = 0;
  chain_persist::Streak s{};

  CHECK(chain_persist::settle(20, true, &persisted, &s, &persists, &failures) ==
        Say::Nothing);
  CHECK(persisted == 20 && persists == 1 && failures == 0 && !s.failing);

  // The failure: the write did not land. seq_persisted stays where NVS is,
  // and the streak opens with its prompt retry still owed.
  CHECK(chain_persist::settle(30, false, &persisted, &s, &persists, &failures) ==
        Say::Failed);
  CHECK(persisted == 20 && persists == 1 && failures == 1);
  CHECK(s.failing && !s.retried && s.failed_seq == 30);

  // Retries inside the streak: counted, not reported; the first one spends
  // the prompt retry, and each records where it tried.
  CHECK(chain_persist::settle(31, false, &persisted, &s, &persists, &failures) ==
        Say::Nothing);
  CHECK(s.failing && s.retried && s.failed_seq == 31);
  CHECK(chain_persist::settle(41, false, &persisted, &s, &persists, &failures) ==
        Say::Nothing);
  CHECK(persisted == 20 && persists == 1 && failures == 3);
  CHECK(s.failing && s.retried && s.failed_seq == 41);

  // The first write to land closes the streak, and says so once.
  CHECK(chain_persist::settle(51, true, &persisted, &s, &persists, &failures) ==
        Say::Recovered);
  CHECK(persisted == 51 && persists == 2 && failures == 3);
  CHECK(!s.failing && !s.retried);
  CHECK(chain_persist::settle(61, true, &persisted, &s, &persists, &failures) ==
        Say::Nothing);
  CHECK(persisted == 61 && persists == 3);

  // A second streak is reported again, and owes its own prompt retry.
  CHECK(chain_persist::settle(71, false, &persisted, &s, &persists, &failures) ==
        Say::Failed);
  CHECK(persisted == 61 && failures == 4 && s.failing && !s.retried && s.failed_seq == 71);
}

// ── settle() with a null argument changes nothing ───────────────────────────
static void test_settle_refuses_a_null_argument() {
  uint32_t persisted = 5, persists = 1, failures = 2;
  chain_persist::Streak s{};
  CHECK(chain_persist::settle(9, false, nullptr, &s, &persists, &failures) ==
        Say::Nothing);
  CHECK(chain_persist::settle(9, true, &persisted, nullptr, &persists, &failures) ==
        Say::Nothing);
  CHECK(chain_persist::settle(9, true, &persisted, &s, nullptr, &failures) ==
        Say::Nothing);
  CHECK(chain_persist::settle(9, false, &persisted, &s, &persists, nullptr) ==
        Say::Nothing);
  CHECK(persisted == 5 && persists == 1 && failures == 2 && !s.failing && s.failed_seq == 0);
}

// ════════════════════════════════════════════════════════════════════════════
// PART 2 — securacv_witness.cpp's own glue, over a fake NVS
// ════════════════════════════════════════════════════════════════════════════

// The cut functions' file-level state, as securacv_witness.cpp declares it.
static DeviceIdentity g_device;
static SystemHealth g_health;

// A fake NVS: the chain blob and the two birth entries. A put to a refused
// key (or any put, with refuse_all) fails the way nvs_store_* now reports it.
struct FakeNvs {
  bool refuse_all = false;
  const char* refuse_key = nullptr;
  int chain_puts = 0;
  int u32_puts = 0;
  int unexpected = 0;  // a put to any other key, or a blob of the wrong size
  bool blob_present = false;
  uint8_t blob[chain_state::BLOB_LEN] = {0};
  bool born_set = false, born_ex_set = false;
  uint32_t born = 0, born_ex = 0;
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

bool nvs_store_u32(const char* key, uint32_t val) {
  g_nvs.u32_puts++;
  if (refused(key)) return false;
  if (std::strcmp(key, NVS_KEY_BORN) == 0) {
    g_nvs.born = val;
    g_nvs.born_set = true;
  } else if (std::strcmp(key, NVS_KEY_BORN_EX) == 0) {
    g_nvs.born_ex = val;
    g_nvs.born_ex_set = true;
  } else {
    g_nvs.unexpected++;
    return false;
  }
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
  CHECK(!g_device.chain_persist_streak.failing);
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
  CHECK(g_device.chain_persist_streak.failing);
  CHECK(g_health.chain_persists == 0 && g_health.chain_persist_failures == 1);
  CHECK(g_log.count == 1 && g_log.warnings == 1);  // reported once
  CHECK(std::strstr(g_log.message, "not written to NVS") != nullptr);
  CHECK(std::strstr(g_log.detail, "seq 10;") != nullptr);

  // The next record retries at once, not an interval later; the streak is
  // not reported again.
  make_record();
  CHECK(g_nvs.chain_puts == 2);
  CHECK(g_health.chain_persist_failures == 2);
  CHECK(g_device.seq_persisted == 0 && nvs_seq() == 0);
  CHECK(g_log.count == 1);

  // That retry failed too: the next one is an interval after it, not on
  // every record (a leaked session would stall the loop 2 s per record).
  for (uint32_t i = 1; i < SD_PERSIST_INTERVAL; ++i) make_record();
  CHECK(g_nvs.chain_puts == 2);
  make_record();                                   // an interval after seq 11
  CHECK(g_nvs.chain_puts == 3);
  CHECK(g_health.chain_persist_failures == 3);
  CHECK(g_device.seq_persisted == 0 && nvs_seq() == 0);
  CHECK(g_log.count == 1);

  // NVS takes writes again: the next retry, an interval on, lands the
  // newest state.
  g_nvs.refuse_all = false;
  for (uint32_t i = 1; i < SD_PERSIST_INTERVAL; ++i) make_record();
  CHECK(g_nvs.chain_puts == 3);
  make_record();
  CHECK(g_nvs.chain_puts == 4);
  CHECK(g_device.seq == 3 * SD_PERSIST_INTERVAL + 1);
  CHECK(nvs_seq() == g_device.seq);
  CHECK(g_device.seq_persisted == g_device.seq);
  CHECK(!g_device.chain_persist_streak.failing && !g_device.chain_persist_streak.retried);
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

// ── a failure that lasts costs one attempt per interval, not one per record ─
//
// The cost a lasting failure puts on the loop: a full partition refuses
// every put, and a leaked NVS session makes each attempt wait 2 s
// (nvs_session::kSessionWaitMs) before it fails. A thousand records under
// one: the opening failure, the prompt retry, then one attempt per interval.
static void test_a_lasting_failure_is_retried_once_per_interval() {
  reset_chain(0);
  g_nvs.refuse_all = true;
  const uint32_t records = 1000;
  for (uint32_t i = 0; i < records; ++i) make_record();
  // Attempts at seq 10 (the routine one, refused), 11 (the prompt retry),
  // then 21, 31, ..., 991.
  const int expected = 2 + (int)((records - (SD_PERSIST_INTERVAL + 1)) / SD_PERSIST_INTERVAL);
  CHECK(g_nvs.chain_puts == expected);
  CHECK(g_health.chain_persist_failures == (uint32_t)expected);
  CHECK(g_health.chain_persists == 0);
  CHECK(g_device.seq_persisted == 0 && nvs_seq() == 0);
  CHECK(g_log.count == 1);                         // one streak, one line
  // It still heals within an interval of NVS taking writes again.
  g_nvs.refuse_all = false;
  for (uint32_t i = 0; i < SD_PERSIST_INTERVAL; ++i) make_record();
  CHECK(g_device.seq_persisted == nvs_seq() && !g_device.chain_persist_streak.failing);
  CHECK(g_device.seq - nvs_seq() < SD_PERSIST_INTERVAL);
  CHECK(g_log.notices == 1);
}

// ── the failure line fits its health-log slot at the largest seq ────────────
static void test_the_failure_line_fits_at_the_largest_seq() {
  reset_chain(0xFFFFFFFFu - SD_PERSIST_INTERVAL);
  g_nvs.refuse_all = true;
  for (uint32_t i = 0; i < SD_PERSIST_INTERVAL; ++i) make_record();
  CHECK(g_device.seq == 0xFFFFFFFFu && g_log.warnings == 1);
  char want[48];
  std::snprintf(want, sizeof(want), "seq 4294967295; retrying, then every %u records",
                (unsigned)SD_PERSIST_INTERVAL);
  CHECK(std::strcmp(g_log.detail, want) == 0);     // not cut short by detail[48]
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
  CHECK(!g_device.chain_persist_streak.failing && g_log.notices == 1);
}

// ── the failure, run long: never an interval without an attempt, never more ─
//
// A deterministic mix of records with NVS refusing about one write in four,
// in streaks. After every record: seq_persisted is exactly the seq NVS holds;
// no record ends an interval or more past the last write attempt (a failure
// is never left untried for an interval); inside a streak, only the prompt
// retry comes sooner than an interval after the attempt before it (a lasting
// failure is not paid per record); every attempt is counted once, on one
// side; and the health log holds one line per streak opened and one per
// streak closed. The old glue (advance and count whatever the write did)
// breaks the first on the first refused write; a retry on every record
// breaks the third.
static void test_nvs_is_retried_every_interval_and_no_more_often() {
  reset_chain(0);
  uint32_t x = 0x9E3779B9u;  // xorshift32, fixed seed: the run is reproducible
  auto next = [&x]() {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
  };
  int streaks = 0, recoveries = 0, refused_steps = 0, prompt_retries = 0;
  int prompt_in_streak = 0;
  uint32_t last_attempt = g_device.seq;  // reset_chain() wrote it
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
    if (attempted && was_failing &&
        g_device.seq - last_attempt < SD_PERSIST_INTERVAL) {
      prompt_retries++;
      CHECK(++prompt_in_streak == 1);              // one prompt retry per streak
    }
    if (attempted) last_attempt = g_device.seq;
    CHECK(g_device.seq - last_attempt < SD_PERSIST_INTERVAL);
    CHECK((uint32_t)g_nvs.chain_puts ==
          g_health.chain_persists + g_health.chain_persist_failures);
    const bool failing = g_device.chain_persist_streak.failing;
    if (attempted && failing && !was_failing) {
      streaks++;
      prompt_in_streak = 0;
    }
    if (attempted && !failing && was_failing) recoveries++;
    was_failing = failing;
    if (g_failures > 20) break;  // one broken invariant floods; stop early
  }
  CHECK(g_log.warnings == streaks && g_log.notices == recoveries);
  CHECK(g_log.count == streaks + recoveries);
  CHECK(streaks > 100 && recoveries > 100);       // the mix really failed and healed
  CHECK(refused_steps > 5000);
  CHECK(prompt_retries > 100);                    // the prompt retry really ran
  CHECK(g_health.chain_persist_failures > (uint32_t)streaks);  // retries inside streaks
  CHECK(g_nvs.unexpected == 0);
}

// ── the birth stamp: no stamp claimed that NVS lacks, and no hot retry ──────
static void test_a_failed_birth_stamp_claims_nothing_and_waits() {
  reset_chain(0);
  const uint32_t unix_s = birth::kClockFloor + 12345u;
  const uint32_t day = birth::day_of(unix_s);
  g_device.key_is_new = true;
  g_device.key_born_ms = 0;
  g_host_millis = 100000u;
  const int lines = Serial.lines;

  // The flag's write is refused: the day is not written after it, and RAM
  // claims no stamp. Reported once.
  g_nvs.refuse_key = NVS_KEY_BORN_EX;
  CHECK(!witness_note_wall_clock(unix_s));
  CHECK(!g_nvs.born_set);
  CHECK(g_device.born_day == 0 && !g_device.born_exact);
  CHECK(Serial.lines == lines + 1);
  CHECK(std::strstr(Serial.last, "not stored") != nullptr);

  // The loop calls again at once, and for the rest of the minute: no write.
  const int puts = g_nvs.u32_puts;
  g_host_millis += 1000u;
  CHECK(!witness_note_wall_clock(unix_s));
  g_host_millis += 58000u;
  CHECK(!witness_note_wall_clock(unix_s));
  CHECK(g_nvs.u32_puts == puts);

  // A minute on it tries again; this time the flag lands and the day does
  // not. Still no stamp in RAM, and the streak is not reported again.
  g_nvs.refuse_key = NVS_KEY_BORN;
  g_host_millis += 1000u;
  CHECK(!witness_note_wall_clock(unix_s));
  CHECK(g_nvs.u32_puts == puts + 2);
  CHECK(g_nvs.born_ex_set && !g_nvs.born_set);
  CHECK(g_device.born_day == 0);
  CHECK(Serial.lines == lines + 1);

  // Another minute, NVS takes both: the stamp is written and then held.
  g_nvs.refuse_key = nullptr;
  g_host_millis += 60000u;
  CHECK(witness_note_wall_clock(unix_s));
  CHECK(g_nvs.born_set && g_nvs.born == day);
  CHECK(g_nvs.born_ex_set && g_nvs.born_ex == 1u);  // made this boot, minutes ago
  CHECK(g_device.born_day == day && g_device.born_exact);
  CHECK(std::strstr(Serial.last, "[BIRTH]") != nullptr);
  const int puts_after = g_nvs.u32_puts;
  CHECK(!witness_note_wall_clock(unix_s));         // recorded: never again
  CHECK(g_nvs.u32_puts == puts_after);
}

// ── the birth stamp's wait survives millis() wrapping ───────────────────────
static void test_the_birth_retry_wait_is_wrap_safe() {
  reset_chain(0);
  const uint32_t unix_s = birth::kClockFloor + 777u;
  g_device.key_is_new = false;
  g_nvs.refuse_all = true;
  g_host_millis = 0xFFFFF000u;
  CHECK(!witness_note_wall_clock(unix_s));
  const int puts = g_nvs.u32_puts;
  g_host_millis = 0xFFFFF800u;                     // 2 s later, not yet wrapped
  CHECK(!witness_note_wall_clock(unix_s));
  CHECK(g_nvs.u32_puts == puts);                   // still waiting
  g_host_millis = 0x00000100u;                     // 4.35 s later, wrapped
  CHECK(!witness_note_wall_clock(unix_s));
  CHECK(g_nvs.u32_puts == puts);                   // still waiting
  g_nvs.refuse_all = false;
  g_host_millis = 0xFFFFF000u + 60000u;            // a minute on, wrapped
  CHECK(witness_note_wall_clock(unix_s));
  CHECK(g_nvs.born_set && !g_device.born_exact && g_nvs.born_ex == 0u);
}

int main() {
  test_due_follows_the_interval_with_no_streak();
  test_due_retries_the_opening_failure_on_the_next_record();
  test_due_backs_off_to_the_interval_after_the_prompt_retry();
  test_settle_moves_and_counts_only_what_landed();
  test_settle_refuses_a_null_argument();
  test_routine_persist_every_interval();
  test_a_failed_persist_is_retried_not_forgotten();
  test_a_lasting_failure_is_retried_once_per_interval();
  test_the_failure_line_fits_at_the_largest_seq();
  test_a_failed_direct_persist_is_retried_after_the_next_record();
  test_nvs_is_retried_every_interval_and_no_more_often();
  test_a_failed_birth_stamp_claims_nothing_and_waits();
  test_the_birth_retry_wait_is_wrap_safe();

  if (g_failures == 0) { std::printf("ALL chain-persist tests PASSED\n"); return 0; }
  std::printf("FAILED: %d assertion(s)\n", g_failures);
  return 1;
}
