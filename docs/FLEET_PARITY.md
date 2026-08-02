# Parity by architecture — one edit reaches the whole fleet

A SecuraCV capability that every Canary is supposed to share — a wire format, a
status shape, a beacon layout — must be **impossible to ship on one board and
forget on another.** Copy-paste across firmwares is how a fleet silently
fragments: a reviewer approves the change on `canary-wap`, the same behaviour
never lands on `canary-display`, and months later the two answer the same
request with two different shapes and nothing caught it.

The rule: **a fleet-wide behaviour lives in exactly one place, and each board
carries only the thin glue that feeds it local state.** A change to the
behaviour is a change to that one file — never a sweep across boards.

## The shape of the pattern

1. **One canonical, pure core in `firmware/common/`.** Board-agnostic, no
   Arduino / no heap, so it compiles and is *unit-tested on the host* with
   `g++`. This is the single source of truth for the behaviour.
2. **PlatformIO trees include it directly** via `-I firmware/common` — no copy,
   they build the canonical file.
3. **Arduino sketches carry a copy that a guard keeps honest.** Either a
   committed byte-copy checked by a `firmware/scripts/check_*_sync.sh` guard, or
   a copy staged by the project's `setup.sh regen` and checked by
   `check_display_arduino_sync.sh`. Drift fails CI, not the field.
4. **A host test pins the contract** in `firmware/tests_host/` so the shape
   (and its safety properties — escaping, buffer bounds) can't regress. CI runs
   `make -C firmware/tests_host`.
5. **The per-board glue is tiny and local.** Each board fills a small struct
   from its own state and calls the shared builder. Capability gates
   (`HAS_WIFI` × `FEATURE_HTTP_SERVER`, cross-checked in
   `common/core/feature_sanity.h`) decide *where* the glue is wired; the core
   itself is pure data and compiles everywhere.

## Worked examples in the tree

- **`/api/fleet` self-report** — the coarse fleet presence/health JSON every
  networked Canary answers (the contract in
  [`../tvos/discovery/DISCOVERY.md`](../tvos/discovery/DISCOVERY.md), read by the
  Witness Wall emulator and the Flasher's post-flash LAN discovery).
  - Core: `firmware/common/fleet_selfreport/fleet_selfreport.h` — a clamp-safe,
    JSON-escaping, header-only builder.
  - Test: `firmware/tests_host/test_fleet_selfreport.cpp` (shape, escaping,
    buffer bounds, hub compose). Writing this test caught a real null-`out`
    crash in the integer writer before it ever reached a board.
  - Glue: `canary-wap` (ESP-IDF `esp_http_server`) and `canary-display`
    (Arduino `WebServer`) each gather `{name, product, online, chain}` from
    local state and call the builder — **two server styles, byte-identical
    output.**
  - Guards: `check_fleet_selfreport_sync.sh` (wap byte-copy) +
    `check_display_arduino_sync.sh` (display regen).

- **Fleet-link presence beacon** — the byte-exact BLE advertisement shared
  between a Canary and a display. Core:
  `firmware/common/fleet_link/fleet_beacon.h`; guard:
  `check_fleet_beacon_sync.sh`; tested next to the canary PIO build.

- **Device self-manifest** — the machine-readable JSON emitted over serial.
  Core: `firmware/common/attest/self_manifest.h`; test:
  `test_self_manifest.cpp`.

## Consumers outside the firmware (the iPhone app)

A fleet-wide contract has readers that aren't boards. The iPhone app is one, and
it follows the same discipline one layer out:

- **`ios/Sources/SecuraCV/Wire/`** holds *pure* ports of the canonical headers —
  `FleetBeacon.swift` (from `common/fleet_link/fleet_beacon.h`) and
  `FleetSelfReport.swift` (from `common/fleet_selfreport/fleet_selfreport.h`).
  Foundation only: no CoreBluetooth, no URLSession, no SwiftUI, so they are
  host-testable exactly like `firmware/tests_host/`. The transports
  (`Transport/BLEConsole.swift`, `Transport/DeviceAPI.swift`) are thin glue on
  top, and `App/FleetMerge.swift` — also pure — folds the tiers together.
- **The guard is the test, not a byte-copy.** `check_*_sync.sh` can keep two C
  files identical; it cannot keep a Swift file identical to a C header. So the
  Swift tests reuse the firmware's own vectors: `FleetBeaconTests` carries the
  values from `common/fleet_link/test_fleet_beacon.cpp`, and
  `FleetSelfReportTests` decodes JSON literals that are the verbatim stdout of
  `fleet_selfreport_build()` compiled with g++. Change the wire, and the C test
  changes — these must change with it or they fail.
- **Tier discipline.** The app reads three surfaces and they are not equal: the
  BLE presence beacon and `GET /api/fleet` are unauthenticated and coarse, while
  `GET /api/v1/witness` is authenticated *and* locally verified. Only the last
  may set a trust badge. `FleetMerge` states the rules and `FleetMergeTests`
  enforces them — a device *claiming* `"chain":"ok"` must never render as
  verified.

The **desktop apps** (Flasher + Lab) are two more readers, held together the
same way:

- **One canonical emulator** (the website repo's Witness Wall) is stamped into
  both apps by `scripts/vendor_witness_emulator.sh` — byte-identical copies at
  `desktop/src/witness/` and `canary-local/witness/`, guarded by
  `scripts/check_witness_emulator_sync.sh` in CI.
- **One discovery command**: `witness_discover` exists verbatim in both Rust
  shells (`desktop/src-tauri`, `desktop-lab/src-tauri`) and probes the same
  `GET /api/fleet` contract; the witness-wall test in
  `canary-local/tests/desktop_parity.test.js` fails the build if either app
  loses the command, stops posting `witness:fleet`, or lets the emulator
  copies drift.
- **Per-surface flavor stays host-side**: `?profile=` / `witness:profile`
  pins a surface to its use case (home / business / apartment) without
  forking the emulator.

**When you add a fleet-wide capability, add the app-side reader in the same
change** — a contract with a firmware writer and no reader on the surface users
actually look at has only half shipped.

## Adding the next fleet-wide capability

1. Write the pure core in `firmware/common/<area>/` (no Arduino types; guard any
   device-only path behind `#if defined(ARDUINO)`).
2. Write the host test in `firmware/tests_host/`, add a Makefile target, and run
   it locally — this is the part you can fully verify off-device.
3. Wire the glue into each HTTP/BLE/serial surface that should expose it, gated
   on the relevant capability flags so boards that can't serve it simply don't.
4. If an Arduino sketch needs a copy, add a `check_*_sync.sh` guard (or extend
   `setup.sh` + the regen guard) and a CI step, mirroring the beacon.
5. Update the contract doc (e.g. `DISCOVERY.md`) so the shape has a written home.

Do this and "apply it to the whole fleet" stops being a checklist you can fail —
it's what the architecture does on its own.
