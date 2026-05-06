# Writing a third-party CSI module

A **module** is a small agent that consumes the SecuraCV CSI feature stream
and emits domain events through the privacy chokepoint. The interface is
intentionally minimal so you can add a new sensing behavior without touching
SecuraCV core.

This directory contains one stub module (`stub_door_opens.{h,cpp}`) that
shows every required moving part:

- A manifest declaring the event types and their per-field allow-list.
- A privacy class for each event type (`P0` / `P1` / `P2`).
- A `tick()` function that runs once per CSI window.
- A `dismiss` handler so user feedback (the dashboard's `That was nothing`
  swipe) can nudge thresholds locally.
- Lifecycle: `init()` reads NVS-backed settings; `deinit()` is reversible.

## Why the manifest matters

The chokepoint enforces the manifest at runtime: any field a module tries
to publish that isn't on its allow-list is silently zeroed before the event
is persisted, exported, or shown to the dashboard. Privacy is a runtime
gate, not a documentation promise. The fuzzer at
`firmware/common/csi/csi_event_invariants_test.cpp` proves it. (The
test lives at the library root, not under `src/`, so arduino-cli's 1.5
recursive-compile of `src/` skips it; it builds standalone for CI.)

## Adding your module to a build

1. Drop `your_module.{h,cpp}` next to this README (or, if you're
   contributing it back to SecuraCV, into `firmware/common/csi/src/`
   alongside the other v1 modules so arduino-cli's 1.5-format recursive
   compile picks it up).
2. Register at boot:

   ```cpp
   #include "stub_door_opens.h"

   void register_csi_modules() {
     csi_module_register(stub_door_opens_module());
   }
   ```

3. Drive the runtime from the CSI features callback:

   ```cpp
   csi_set_features_callback([](const csi_features_t* f, void*) {
     csi_module_tick_all(f);
   }, nullptr);
   ```

That's it. The bundler, ceiling, witness chain, and stream surfaces all
keep working without further wiring.

## Privacy class quick guide

| Class | When to pick it | What can carry |
| --- | --- | --- |
| `P0` | Always-on. Aggregate counts and bucketed scalars only. | state name, confidence, duration, time bucket, motion/breathing scores (0..100), bundle counts. |
| `P1` | Opt-in. Still anonymous but more detailed. Emits only when the host has raised the privacy ceiling via the dashboard. | the above + numeric estimates (e.g. BPM). |
| `P2` | Power-user / developer disclosure. Never persists to the witness chain or leaves the device. | the above + the raw 32-dim feature vector. The Tuning Lab lives here. |

If you're not sure, pick `P0` and keep your event payload small.
