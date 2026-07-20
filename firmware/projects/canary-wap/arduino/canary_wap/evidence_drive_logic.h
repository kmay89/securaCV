/*
 * SecuraCV Canary — Evidence Drive core logic (host-testable, no Arduino).
 *
 * Two pure pieces of the USB evidence drive (see
 * docs/design/usb_evidence_drive.md):
 *
 *  1. share_state: the single-writer state machine that decides who owns the
 *     SD card — firmware (NORMAL) or the USB host (SHARED, read-only). The
 *     rule it enforces: the SD has exactly one writer, ever, and witness
 *     records are never dropped — they buffer in a bounded ring while the
 *     host is looking, and ring pressure forces the share to end rather
 *     than lose a record.
 *
 *  2. fat16: formatter + parser for the PSRAM-backed "CANARY-UPDATE"
 *     staging volume. We format the volume ourselves, the host drops a
 *     signed update file on it, and we parse our own format back — the
 *     only FAT structures ever trusted are ones this module wrote or can
 *     fully bound-check. Nothing here touches flash or the SD.
 *
 * The USB/TinyUSB/SD glue lives in usb_evidence_drive.{h,cpp} (firmware
 * only, FEATURE_USB_EVIDENCE_DRIVE); this file must compile hosted for
 * tests_host/test_evidence_drive_logic.cpp.
 */

#ifndef SECURACV_EVIDENCE_DRIVE_LOGIC_H
#define SECURACV_EVIDENCE_DRIVE_LOGIC_H

#include <stdint.h>
#include <stddef.h>

namespace evidence_drive {

// ════════════════════════════════════════════════════════════════════════════
// 1. Share-mode state machine (who owns the SD card)
// ════════════════════════════════════════════════════════════════════════════

enum class ShareState : uint8_t {
  NORMAL = 0,   // firmware owns the SD; MSC evidence LUN reports not-ready
  SHARED = 1,   // host owns the SD read-only; firmware buffers records in RAM
};

enum class ShareEvent : uint8_t {
  SHARE_REQUEST,   // user asked (console 'u' / web UI) to expose the drive
  SHARE_RELEASE,   // user asked to stop sharing
  HOST_EJECT,      // host issued MSC eject / stop-unit
  USB_UNPLUG,      // cable gone
  RING_PRESSURE,   // witness ring is nearly full — durability outranks host
};

struct ShareStatus {
  ShareState state = ShareState::NORMAL;
  // Bookkeeping the firmware glue reports into the health log.
  uint32_t shares_started = 0;
  uint32_t shares_ended = 0;
  uint32_t forced_ends = 0;      // ended by RING_PRESSURE, not by the user/host
  uint32_t buffered_records = 0; // currently waiting in the RAM ring
  uint32_t buffered_peak = 0;
};

// Transition table. Returns true when the event caused a state change; the
// glue uses the transition (not the event) to flush/close/reopen SD files.
// begin_share must only succeed once the glue confirms SD files are flushed
// and closed — hence the explicit `sd_quiesced` gate.
bool share_apply(ShareStatus& st, ShareEvent ev, bool sd_quiesced);

// Ring accounting (glue owns the actual buffer; logic owns the policy).
// Returns the event the glue must feed back into share_apply when the ring
// crosses the pressure threshold (records buffered >= capacity - headroom).
static const uint32_t SHARE_RING_HEADROOM = 4;
bool share_note_buffered(ShareStatus& st, uint32_t ring_capacity);
void share_note_drained(ShareStatus& st);

// ════════════════════════════════════════════════════════════════════════════
// 2. FAT16 staging volume ("CANARY-UPDATE", PSRAM-backed)
// ════════════════════════════════════════════════════════════════════════════
//
// Fixed geometry, chosen so every canary-wap OTA-slot image fits and the
// whole volume lives comfortably in the S3's 8 MB PSRAM:
//   512 B sectors · 1 sector/cluster · 4 MB total
//   [ boot | FAT×2 (32 sectors each) | root dir (32 sectors, 512 entries) | data ]

static const uint32_t FAT_SECTOR_SIZE   = 512;
static const uint32_t FAT_TOTAL_SECTORS = 8192;              // 4 MB
static const uint32_t FAT_SECTORS_PER_FAT = 32;              // 8192 clusters × 2 B
static const uint32_t FAT_ROOT_ENTRIES  = 512;
static const uint32_t FAT_ROOT_SECTORS  = (FAT_ROOT_ENTRIES * 32) / FAT_SECTOR_SIZE; // 32
static const uint32_t FAT_FIRST_DATA_SECTOR =
    1 + 2 * FAT_SECTORS_PER_FAT + FAT_ROOT_SECTORS;          // 97
static const uint32_t FAT_DATA_CLUSTERS =
    FAT_TOTAL_SECTORS - FAT_FIRST_DATA_SECTOR;               // usable clusters
static const uint32_t FAT_VOLUME_BYTES  = FAT_TOTAL_SECTORS * FAT_SECTOR_SIZE;

// Largest update file the volume can stage (all data clusters).
static const uint32_t FAT_MAX_FILE_BYTES = FAT_DATA_CLUSTERS * FAT_SECTOR_SIZE;

// Format `buf` (must be FAT_VOLUME_BYTES) as an empty CANARY-UPDATE volume
// carrying a README.TXT that tells the human what to drop here.
void fat16_format(uint8_t* buf);

// One parsed root-directory file.
struct FatFile {
  char name[13];        // "CANARY~1.BIN" style 8.3, NUL-terminated
  uint32_t size = 0;
  uint16_t first_cluster = 0;
  bool size_matches_chain = false; // FAT chain length covers `size`
};

// Scan the root directory for real files (skips volume label, long-name
// entries, deleted entries, directories). Returns count found, capped at
// `max_out`. Never reads outside `buf`.
size_t fat16_list_root(const uint8_t* buf, FatFile* out, size_t max_out);

// The update-drop policy: exactly one image (*.BIN) AND exactly one signed
// per-variant manifest (*.JSO — hosts shorten "manifest-….json" to an 8.3
// alias with a three-letter extension), both chain-consistent and within
// size bounds. A bare .bin is NOT self-verifying — its Ed25519 signature
// lives in the manifest — so the pair is mandatory. Returns true and fills
// both outs when a unique pair exists. `reason` (optional, cap
// `reason_cap`) gets a short human explanation on failure — it becomes
// RESULT.TXT on the volume.
static const uint32_t FAT_MAX_MANIFEST_BYTES = 16 * 1024;
bool fat16_find_update(const uint8_t* buf, FatFile* bin_out, FatFile* manifest_out,
                       char* reason, size_t reason_cap);

// Copy the file's bytes (following the FAT chain) into `dst` (cap
// `dst_cap`). Returns bytes copied, or 0 on any inconsistency. Bounds are
// enforced on every cluster hop; loops in a corrupted chain terminate.
uint32_t fat16_read_file(const uint8_t* buf, const FatFile& f,
                         uint8_t* dst, uint32_t dst_cap);

// Write (or replace) RESULT.TXT in the volume root with `text` — how the
// firmware answers a drop ("verified … installing" / why it refused).
// Single-cluster payload (≤512 B), allocated from the tail of the data
// region so it never collides with host-written files. Returns false only
// when the root directory is full.
bool fat16_write_result(uint8_t* buf, const char* text);

} // namespace evidence_drive

#endif // SECURACV_EVIDENCE_DRIVE_LOGIC_H
