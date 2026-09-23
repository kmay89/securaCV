/*
 * SecuraCV Canary — SD event log adapter (backlog F37)
 *
 * The card half of the committed-event backfill: /EVENTS/today.ndjson in
 * the canary-wap's line format (common/csi/src/csi_event_log_line.h, shared
 * byte-for-byte), so a tool reads either device's card the same way. The
 * decisions — watermark, replay cursor, what to send next, work per pass —
 * are the pure common/csi/src/csi_event_backfill.h; this file only moves
 * bytes.
 *
 * Threading: LOOP TASK ONLY. This tree's SD rule (securacv_storage.h) is
 * that the Arduino loopTask is the single writer of all SD state, and that
 * nothing touches SD.* while a background mount attempt runs. Every entry
 * point here is called from csi_event_egress_pump() on the loop task, and
 * poll() refuses the card unless storage_is_mounted() and no mount is in
 * flight. append() and read_at() ask again on every call, because a run of
 * failed writes marks the card lost mid-pass. Nothing else in this file
 * opens the card. Committed rows reach the pump through csi_event_egress's
 * FreeRTOS queue, so a commit on the NimBLE host task never gets here
 * directly.
 *
 * Ownership: the log's lines carry no device id, so the directory keeps
 * /EVENTS/owner, one line naming this device's witness-key fingerprint. A
 * log whose owner file does not name this device (another device's card, a
 * canary-wap card, a card from before a factory reset) is left untouched
 * and unused — the backfill signs what it replays with this device's key,
 * and another device's history must never go out under it. The witness log
 * refuses such a card for the same reason (securacv_witness.cpp's fork
 * guard).
 *
 * Crash model (the canary-wap's csi_event_log.cpp, ported): FILE_APPEND and
 * close per line, so a power cut loses at most the line in flight; a torn
 * last line is sealed with '\n' before the next append, and the parser
 * refuses it. Retention: past kMaxBytes the oldest quarter is dropped
 * through a .tmp copy and a rename, reconciled on the next mount if a crash
 * interrupts it.
 */

#ifndef SECURACV_CANARY_CSI_EVENT_LOG_H
#define SECURACV_CANARY_CSI_EVENT_LOG_H

#include <stddef.h>
#include <stdint.h>

#include "csi_event_backfill.h"

namespace csi_event_log {

/* The canary-wap's cap: 256 KB is weeks of a normal home's events. */
constexpr uint32_t kMaxBytes = 256u * 1024u;

enum class Card : uint8_t {
  kUnchanged,  /* same state as the last poll */
  kOpened,     /* a (new) card's log is usable: size and tail id reported */
  kClosed,     /* the log stopped being usable (card gone, lost, refused) */
};

/* Re-evaluate the card; call once per pump pass, before any other entry
 * point. `owner_fp` is this device's fingerprint (hex). On kOpened, `*size`
 * and `*tail_id` describe the log (0/0 when it does not exist yet). */
Card poll(const char* owner_fp, uint32_t* size, uint32_t* tail_id);

/* Append one line (with its '\n'). Only after poll() reported the log open. */
csi_event_backfill::AppendResult append(const char* line, size_t len);

/* Read up to `cap` bytes at `off`; the count read, 0 on error. */
size_t read_at(uint32_t off, char* buf, size_t cap);

}  // namespace csi_event_log

#endif  // SECURACV_CANARY_CSI_EVENT_LOG_H
