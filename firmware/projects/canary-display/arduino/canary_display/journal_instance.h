// Project-wide time-machine journal singleton (spec §7), sized by the flavor
// config. Mirrors fleet_instance.h: everything takes it by reference so the
// Journal template itself stays dependency-free and host-testable.
#pragma once

#include "canary/config.h"
#include "canary/fleet/journal.h"

namespace canary::fleet {

using EventJournal = Journal<CD_JOURNAL_CAP>;

EventJournal& the_journal();

// Boot the time machine: mount persistence (if enabled), reload recent history
// into RAM, and wire the fleet's event sink so every new event is journaled
// (and persisted). Call once after the fleet exists. Degrades to RAM-only if
// the filesystem won't mount — never fatal.
void journal_begin();

// Sovereignty: erase all history, RAM ring and flash alike.
void journal_wipe_all();

}  // namespace canary::fleet
