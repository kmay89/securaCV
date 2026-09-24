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
 *   2. While the last attempt has failed, a persist is due after the next
 *      record whatever the gap, so the failure is retried, not forgotten.
 *      Otherwise it is due when the gap `seq - seq_persisted` reaches the
 *      interval, as before. The subtraction is unsigned, so a seq that
 *      wrapped past 0 still reads as the gap it is.
 *   3. A failure streak is reported once, at the failure that opens it,
 *      and once more when a write lands again. A retry that fails inside
 *      an open streak is counted, not reported.
 *
 * Retries run once per record while the streak lasts, so a lasting failure
 * (a full NVS partition) costs one refused write per record, not one per
 * interval. That is the price of retrying on the next record; the
 * alternative, waiting out another interval, is the gap this header closes.
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
 * Is a persist due after a record? `failing` is true while the last attempt
 * did not land (rule 2); `interval` is the records between routine persists.
 */
inline bool due(uint32_t seq, uint32_t seq_persisted, bool failing, uint32_t interval) {
  return failing || (uint32_t)(seq - seq_persisted) >= interval;
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
 * counts `*persists` only on a write that landed; otherwise counts
 * `*failures` and leaves `*seq_persisted` alone (rule 1). `*failing` is the
 * open streak, kept for due() and for the next call. A null argument changes
 * nothing and reports nothing.
 */
inline Say settle(uint32_t seq, bool wrote, uint32_t* seq_persisted, bool* failing,
                  uint32_t* persists, uint32_t* failures) {
  if (!seq_persisted || !failing || !persists || !failures) return Say::Nothing;
  if (wrote) {
    *seq_persisted = seq;
    ++*persists;
    if (!*failing) return Say::Nothing;
    *failing = false;
    return Say::Recovered;
  }
  ++*failures;
  if (*failing) return Say::Nothing;
  *failing = true;
  return Say::Failed;
}

}  // namespace chain_persist

#endif  // WITNESS_CHAIN_PERSIST_H
