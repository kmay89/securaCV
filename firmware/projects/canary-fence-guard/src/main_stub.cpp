/*
  SecuraCV Canary Fence Guard — CONCEPT STUB (does not build, on purpose)
  ----------------------------------------------------------------------
  (c) 2026 Errer Labs / SecuraCV
  License: Apache-2.0 (repository license).

  This file is a shape, not a firmware: the intended composition of the
  future fence witness, written down so review happens before code does.
  The #error below is the honesty guard — a concept must never compile
  into something that looks shippable. Remove it only when the open
  questions in ../README.md are closed and this project gains real envs.
*/

#error "canary-fence-guard is a concept stub — see README.md; there is nothing to build yet"

#include <Arduino.h>
#include "fence_guard_requirements.h"

// Intended composition (see README.md architecture sketch):
//
//   setup():
//     - power supervisor up first (R5/R6): read cell + sun state before
//       spending anything
//     - NVS identity load / first-boot keygen (common/identity — R3)
//     - vibration sensor init, interrupt-wake armed (R1; sensor TBD)
//     - mesh transport up (R2), position broadcast off (R4)
//
//   loop():
//     - drain ISR ring buffer → feature FSM → FenceEvent (R1)
//     - debounce per R8 floors; wind never alarms alone
//     - on state change: build claim, sign over v1 canonical, advance
//       hash chain, hand to mesh transport (R3)
//     - power supervisor tick: sun/cell health transitions are claims
//       too — degrade honestly, never silently

void setup() {}
void loop() {}
