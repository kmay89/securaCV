# mmwave_mr60 — board-agnostic MR60BHA2 radar skeleton

Minimal, board-agnostic driver wrapper for the Seeed MR60BHA2 60GHz mmWave
radar, used by the **canary-sense** firmware. Reads only the radar module's
pre-digested scalar claims over UART (presence, target count, distance,
breath/heart rate) — never raw IQ.

This is the **Phase 0 skeleton**: stable public interfaces, real FSM logic, and
a wire-shaped UART parser whose per-type field decode is a documented TODO
(see `mr60_uart.cpp`). It compiles and is host-testable today.

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

Every header is Arduino-free and compiles with a host C++17 toolchain:

```
g++ -std=c++17 -DCANARY_SENSE_VITALS \
    -I firmware/common/sensors/mmwave_mr60 \
    firmware/common/sensors/mmwave_mr60/*.cpp <your_test>.cpp -o /tmp/mr60_test
```

Drop `-DCANARY_SENSE_VITALS` to verify the presence-only build excludes vitals.
A `firmware/tests_host/` suite lands in Phase 2 (design doc roadmap).
