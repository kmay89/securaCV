/**
 * @file csi_witness_payload.h
 * @brief Build the canonical witness-chain payload string for a CSI event.
 *
 * The host (canary-wap) wraps every committed event in a witness-chain
 * record. The record's signed body is an ASCII payload string. This
 * helper builds that string in a host-buildable, dependency-free way
 * so the privacy-invariants fuzzer can assert the format on every CI
 * run — without dragging Arduino headers into the host build.
 *
 * Format (one line, ASCII, fields space-separated):
 *
 *   csi <module> <type> <category> <state> <conf>
 *       m=<n> b=<n> bpm=<n> d=<n> bk=<n>
 *       kv=<kernel_version> rs=<ruleset_id> zn=<zone_id>
 *
 * The trailing kv / rs / zn fields satisfy spec/event_contract.md §2's
 * mandatory metadata: every event MUST carry the firmware version that
 * produced it, the ruleset it was scored against, and the zone it
 * fired in.
 *
 * Returns the number of bytes written (not counting the NUL), or -1 if
 * the buffer was too small. The output is always NUL-terminated when
 * the call returns >= 0.
 */
#ifndef SECURACV_CSI_WITNESS_PAYLOAD_H
#define SECURACV_CSI_WITNESS_PAYLOAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int csi_witness_build_payload(char*       buf,
                              size_t      buflen,
                              const char* module_id,
                              const char* type_name,
                              uint8_t     category,
                              const char* state_name,
                              const char* confidence,
                              uint8_t     motion_score,
                              uint8_t     breathing_score,
                              uint8_t     bpm,
                              uint16_t    duration_sec,
                              uint8_t     time_bucket,
                              const char* kernel_version,
                              const char* ruleset_id,
                              const char* zone_id);

#ifdef __cplusplus
}
#endif

#endif  /* SECURACV_CSI_WITNESS_PAYLOAD_H */
