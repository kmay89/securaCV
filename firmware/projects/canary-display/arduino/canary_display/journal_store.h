// Persistence backend for the time-machine journal (spec §7).
//
// LittleFS on internal flash — deliberately NOT the SD card: SD on the watch
// shares the SPI bus with the panel (arbitration hazard) and on the dash sits
// behind the CH422G expander (no native CS). Internal flash sidesteps both and
// works identically on either flavor, mounting the standard `spiffs` data
// partition already present in the default 8MB/16MB tables.
//
// Every entry point is FAILURE-TOLERANT: if the filesystem won't mount, the
// journal simply runs RAM-only for the session and the review UI still works —
// persistence is the layer that survives reboots, never a hard dependency and
// never something that can brick a boot. Gated by FEATURE_TIME_MACHINE_PERSIST
// (bench-validated before shipping enabled, exactly like FEATURE_CHIME).
#pragma once

#include <stdint.h>

#include "journal.h"

namespace canary::fleet {

// Mount the store. Returns false (RAM-only) on any failure — callers must not
// treat that as fatal. No-op returning false when persistence is compiled out.
bool journal_store_init();

// Reload persisted records into the RAM ring (the_journal()) at boot, newest
// last so the ring ends up newest-first. No-op if unmounted / compiled out.
void journal_store_load();

// Append one record durably, rotating to stay under the flash budget. No-op if
// unmounted / compiled out. Cheap: one line, with a rare bounded rewrite.
void journal_store_append(const JournalRecord& r);

// Sovereignty: erase all persisted history. The home's story is the home's to
// forget. No-op if unmounted / compiled out.
void journal_store_wipe();

// Bytes currently on flash (for an honest "N days · X KB, tap to erase" line).
// 0 if unmounted / compiled out.
uint32_t journal_store_bytes();

}  // namespace canary::fleet
