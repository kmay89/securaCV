# mmwave_mr60 — board-agnostic MR60BHA2 radar skeleton

Minimal, board-agnostic driver wrapper for the Seeed MR60BHA2 60GHz mmWave
radar, used by the **canary-sense** firmware. Reads only the radar module's
pre-digested scalar claims over UART (presence, target count, distance,
breath/heart rate) — never raw IQ.

As of **Phase 2** the UART decoder is real: an incremental, byte-at-a-time
state machine that validates the MR60BHA2 header/data checksums and decodes the
presence, target-count, distance, breath-rate and heart-rate frames (wire
format + source URLs documented at the top of `mr60_uart.h`). The presence and
vitals FSMs are unchanged. Everything compiles and is host-tested today; the
exact float units (distance meters→cm, BPM) are marked `[BENCH]` in
`mr60_uart.h` for confirmation against real hardware.

## Modules

| File | Role | Privacy |
|------|------|---------|
| `mr60_uart.{h,cpp}`     | Incremental UART frame parser → decoded `Frame` | — |
| `mr60_presence.{h,cpp}` | Presence/count/range FSM (debounce + stall) | P0 |
| `mr60_vitals.{h,cpp}`   | Breathing/heart lock FSM | P1, build-gated |

## Layering rules (firmware/ARCHITECTURE.md)

- No `boards/` includes — **no pin numbers here**. The composition layer
  (`projects/` + `boards/`) wires the UART/I2C pins and hands this module bytes.
- No `configs/` includes — **no feature flags via header**. Thresholds arrive
  as `*Config` structs filled from `config.h` at the project layer.
- The vitals switch reaches `mr60_vitals.cpp` only as the build flag
  `-DCANARY_SENSE_VITALS=1` (set per env in
  `firmware/envs/platformio/canary-sense.ini`). A presence-only build links
  zero vitals code.

## Design invariants

- **Deadline-before-data guard**: every `tick()` checks its stall/lock deadline
  *before* trusting incoming data, so a silent radar fails the FSM safe
  (presence → `Unknown`, vitals → `Lost`) instead of freezing on the last good
  frame.
- **Wrap-safe time math**: all `millis()` comparisons use signed deltas
  `(int32_t)(now - then) >= dt`.
- **No dynamic allocation** in the hot path: the parser reassembles into a
  fixed `MR60_MAX_FRAME` buffer; FSMs are POD state.
- **Vitals suppressed unless exactly one target** (multi-person BPM ambiguity).

## Host unit testing

Every header is Arduino-free and compiles with a host C++17 toolchain. The
decoder + FSM suite lives in `firmware/tests_host/` and builds/runs with:

```
make -C firmware/tests_host
```

It builds twice — presence-only and with `-DCANARY_SENSE_VITALS=1` — under
`g++ -std=c++17 -Wall -Wextra -Werror`, and covers golden frames for every
type, byte-at-a-time vs whole-buffer equivalence, corrupted header/data
checksums, truncation + resync, garbage floods, oversized-length rejection,
unknown-type skipping, the health counters, and presence/vitals FSM
integration driven by golden frames.
