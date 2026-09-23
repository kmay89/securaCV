/*
 * SecuraCV Canary — Timeline history from the card (repo sweep F35)
 *
 * The loop-task SD read bridge behind GET /api/witness's card pages: the
 * httpd task posts one request and waits (bounded); the Arduino loop task —
 * the single owner of all SD state (securacv_storage.h) — walks
 * /WITNESS/records.jsonl backward a few reads per pass and hands the page
 * back. Every decision (the request slot, the generation counter, the read
 * budget, the resume-hint rules, the linkage a page reports) is the pure,
 * host-tested firmware/common/witness/witness_history_bridge.h; this file
 * supplies the SD calls, the task delay and the one slot.
 * Design: docs/design/witness_history_bridge.md.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_WITNESS_HISTORY_H
#define SECURACV_WITNESS_HISTORY_H

#include <stdint.h>
#include "canary_config.h"

#if FEATURE_SD_STORAGE

#include "witness/witness_history_bridge.h"

enum class WitnessHistoryWait : uint8_t {
  PAGE    = 0,  // *page is valid until witness_history_release(*gen)
  BUSY    = 1,  // another request is outstanding: 503 history_busy
  TIMEOUT = 2,  // no page after WAIT_MS by the clock: 504 history_timeout (the loop drops it)
};

// httpd task. Post `req` and wait for its page until
// witness_history_bridge::WAIT_MS have passed on millis() (the wait gives up
// at its first poll past that, one 10 ms step at most). On PAGE, build the
// answer from *page, then call
// witness_history_release(*gen) — the slot stays claimed until then.
WitnessHistoryWait witness_history_request(const witness_history_bridge::Request& req,
                                           const witness_history_bridge::Response** page,
                                           uint32_t* gen);
void witness_history_release(uint32_t gen);

// Loop task, once per loop(): at most READS_PER_PASS reads of READ_LEN bytes,
// and nothing touches SD while no request is pending, no card is mounted or a
// mount is in flight.
void witness_history_service();

#endif  // FEATURE_SD_STORAGE

#endif  // SECURACV_WITNESS_HISTORY_H
