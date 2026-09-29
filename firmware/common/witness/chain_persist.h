/**
 * @file chain_persist.h
 * @brief When the chain state is written to NVS, and what a write that did
 *        not land leaves behind.
 *
 * THE PROBLEM THIS SOLVES (repo sweep F55). The canary caches {seq, chain
 * head} in NVS as chain_state.h's blob: every SD_PERSIST_INTERVAL records, at
 * boot for a genesis head, and before each deliberate restart. The write
 * helper returned true once its NVS session opened, whatever the put wrote,
 * and the persist did not read the result anyway: it moved `seq_persisted` up
 * to `seq` and counted a persist regardless. So a write that NVS refused (a
 * full partition, a flash error) or that never started (the session wait ran
 * out) was forgotten. Nothing counted or logged it, and nothing tried again
 * until the gap reached the interval once more, while a power cut in that
 * window resumed the chain from an older head.
 *
 * THE RULES.
 *   1. Only a write that landed moves `seq_persisted` and counts as a
 *      persist. A failed one is counted as a failure and leaves
 *      `seq_persisted` where it was.
 *   2. With no failure streak open, a persist is due when the gap
 *      `seq - seq_persisted` reaches the interval, as before. While a
 *      streak is open, the failure is retried, not forgotten: once on the
 *      next record after the failure that opened it, whatever the gap (a
 *      failed genesis or pre-restart write sits far under the interval),
 *      then once per interval, counted from the last failed attempt. The
 *      subtractions are unsigned, so a seq that wrapped past 0 still reads
 *      as the gap it is.
 *   3. A failure streak is reported once, at the failure that opens it,
 *      and once more when a write lands again. A retry that fails inside
 *      an open streak is counted, not reported.
 *
 * WHY RETRIES BACK OFF TO THE INTERVAL. A failure that lasts is not free to
 * retry. A full NVS partition refuses every put, and a session another task
 * leaked (F52's lock) makes every attempt wait out the session wait,
 * nvs_session::kSessionWaitMs = 2 s, on the loop before it fails. The
 * periodic record comes about once a second (RECORD_INTERVAL_MS), so a retry
 * on every record would stall the loop 2 s per record for as long as the
 * leak lasted, and multiply the refused writes by the interval. After the
 * one prompt retry, an open streak costs one attempt per interval: the
 * cadence the routine persist already had before F55, now with the attempt
 * counted, reported and held behind instead of forgotten. The price is the
 * lag: while a streak lasts, NVS holds the head of the last write that
 * landed, however many intervals back, and a power cut resumes from it
 * (SD-wins reconciles when a card is present). A retry on every record
 * would not shorten that while the failure lasts; it would only land the
 * first write after the failure clears sooner, by fewer than an interval of
 * records.
 *
 * Pure hosted C++ (no Arduino/ESP-IDF includes). test_chain_persist.cpp
 * tests these rules on the host, and runs securacv_witness.cpp's own persist
 * glue, cut out verbatim, over a fake NVS. Canonical source:
 * firmware/common/witness/chain_persist.h; consumer: the PIO canary tree
 * (securacv_witness.cpp, via -I ../common).
 */

#ifndef WITNESS_CHAIN_PERSIST_H
#define WITNESS_CHAIN_PERSIST_H

#include <stdint.h>

namespace chain_persist {

/**
 * The open failure streak, if any. Zero-initialized means no streak: a
 * DeviceIdentity starts that way. Only settle() writes it.
 */
struct Streak {
  bool     failing;     ///< the last attempt did not land (a streak is open)
  bool     retried;     ///< the prompt retry after the opening failure has run, and failed
  uint32_t failed_seq;  ///< the seq the last failed attempt tried to write
};

/**
 * Is a persist due after a record (rule 2)? `interval` is the records
 * between routine persists, and between retries once the prompt one has
 * failed.
 */
inline bool due(uint32_t seq, uint32_t seq_persisted, const Streak& streak,
                uint32_t interval) {
  if (!streak.failing) return (uint32_t)(seq - seq_persisted) >= interval;
  const uint32_t wait = streak.retried ? interval : 1u;
  return (uint32_t)(seq - streak.failed_seq) >= wait;
}

/** What one settled attempt asks the caller to report (rule 3). */
enum class Say : uint8_t {
  Nothing,    ///< a write landed with no streak open, or a retry failed inside one
  Failed,     ///< the failure that opens a streak
  Recovered,  ///< the first write to land after a streak
};

/**
 * Settle one attempt to write the chain state as of `seq`. `wrote` is true
 * only when the whole blob landed. Moves `*seq_persisted` to `seq` and
 * counts `*persists` only on a write that landed, which also closes an open
 * streak; otherwise counts `*failures`, leaves `*seq_persisted` alone
 * (rule 1) and records the attempt in `*streak` for due(). A null argument
 * changes nothing and reports nothing.
 */
inline Say settle(uint32_t seq, bool wrote, uint32_t* seq_persisted, Streak* streak,
                  uint32_t* persists, uint32_t* failures) {
  if (!seq_persisted || !streak || !persists || !failures) return Say::Nothing;
  if (wrote) {
    *seq_persisted = seq;
    ++*persists;
    const bool was_failing = streak->failing;
    *streak = Streak{};
    return was_failing ? Say::Recovered : Say::Nothing;
  }
  ++*failures;
  streak->failed_seq = seq;
  if (streak->failing) {
    streak->retried = true;
    return Say::Nothing;
  }
  streak->failing = true;
  streak->retried = false;
  return Say::Failed;
}

}  // namespace chain_persist

#endif  // WITNESS_CHAIN_PERSIST_H
