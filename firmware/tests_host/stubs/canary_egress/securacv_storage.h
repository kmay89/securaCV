/* Stand-in for the canary's securacv_storage.h: the four calls the SD event
 * log adapter (canary/src/csi_event_log.cpp) makes. The test defines them
 * over its model of the storage manager: a card that is mounted or not, a
 * background mount in flight, the mount generation (a remount is a new one)
 * and the write-failure count that marks a card lost. Same signatures as the
 * product's. */
#ifndef STUB_CANARY_EGRESS_SECURACV_STORAGE_H
#define STUB_CANARY_EGRESS_SECURACV_STORAGE_H
#include <stdint.h>
bool storage_is_mounted();
bool storage_mount_in_flight();
uint32_t storage_mount_generation();
void storage_note_write_failure();
void storage_note_write_success();
#endif
