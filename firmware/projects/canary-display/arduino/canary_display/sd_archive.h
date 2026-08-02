// Deep-archive tier for the time-machine journal — the microSD slot, lit.
//
// journal_store (LittleFS) keeps a bounded slice of history on internal
// flash; the SD card, when present, keeps the LONG story: every journal
// record appended as one JSONL line in the exact schema journal_store
// persists, so the card is readable on any laptop with nothing but a text
// editor. The card IS the export — pop it out and the household's history
// travels, no keyboard, monitor, or network required (AGENTS.md Invariant
// IV: local custody; the archive stores the same identity-minimized,
// 10-minute-bucketed records as the RAM ring, never anything richer).
//
// Same failure contract as journal_store: every entry point is
// FAILURE-TOLERANT. No card, an unmountable card, a corrupt or full card —
// the display keeps working and the RAM/LittleFS tiers are untouched. SD is
// never a dependency and can never brick a boot. A card inserted after boot
// is picked up lazily: the next appended event retries the mount (with
// backoff), so archiving starts without a power cycle.
//
// Flavor truth — why this is dash-only today: on the watch the SD slot
// shares the panel's SPI pins, and the GC9A01 is driven through Arduino_GFX's
// private bus handle (Arduino_ESP32SPI) — two masters on one set of pins with
// no shared transaction arbiter. Until panel and card ride one bus handle,
// watch SD stays off, and sd_archive.cpp refuses the flag at compile time
// there rather than shipping display corruption. The dash TF slot has its own
// data pins; only its CS/DAT3 line sits on the CH422G expander, which is
// exactly why the archive drives the slot with the S3's SDMMC host in 1-bit
// mode (GPIO-matrix routed, no CS required — DAT3 held high by the expander's
// default latch keeps the card in SD mode, per the vendor's own demos).
//
// Gated by FEATURE_SD_STORAGE — off in default builds, compile-verified by
// the dedicated `canary-display-dash-sd` env, bench-gated before the default
// flips (like FEATURE_CHIME and the journal's own PERSIST flag).
#pragma once

#include <stdint.h>

#include "journal.h"

namespace canary::fleet {

// Release the slot's DAT3/CS line and try the first mount. Failure-tolerant:
// false means "no archive until a card shows up" — callers must not treat it
// as fatal. No-op returning false when compiled out.
bool sd_archive_init();

// True when a card is mounted and the archive is accepting appends.
bool sd_archive_mounted();

// Append one record as a JSONL line (same schema as journal_store).
// Close-per-write: a power cut loses at most the in-flight line. When no
// card is mounted, quietly retries the mount on a backoff — a hot-inserted
// card starts archiving at the next event. No-op if compiled out.
void sd_archive_append(const JournalRecord& r);

// Sovereignty parity with journal_store_wipe(): erase the card's archive
// files. The home's story is the home's to forget — including the deep copy.
void sd_archive_wipe();

// Archive size on card / whole-card capacity, for an honest status line
// ("N MB archived · 32 GB card"). 0 when absent or compiled out.
uint64_t sd_archive_bytes();
uint64_t sd_archive_card_bytes();

}  // namespace canary::fleet
