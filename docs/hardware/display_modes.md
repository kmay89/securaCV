# Display modes — one glass, five gears

The Waveshare dash (and, where noted, the watch) is too much hardware to be
only one thing. The dev playground already proved the pattern: the same
board, reflashed or latched, becomes a completely different tool — safely,
reversibly, and without a laptop. This document generalizes that from *one
special case* (the bench) into a small, deliberate **mode system**: the
architecture, the rules every mode must obey, and each gear's spec.

> **Status: Built · compile-gated · bench-pending.** The pure cores
> (registry / demo storyline / arcade QA) are **host-tested in CI**
> (`tests_host/test_mode_registry.cpp`, `test_demo_script.cpp`,
> `test_arcade_logic.cpp`); the **runtime is implemented** — the mode glue
> (`src/mode/mode_glue.cpp`), the demo/debug/arcade gears
> (`src/mode/{demo,debug,arcade}_mode.cpp`), the `main.cpp` boot branch, and
> the Settings modes doorway — and is **compile-verified by the dedicated CI
> envs** `canary-display-dash-modes` (all four gears, 4.3B) and
> `canary-display-watch-modes` (demo + debug on the watch). Every flag
> defaults **off**: the default watch/dash/emulator builds are byte-identical
> with or without this work, and per the repo's honesty rule nothing here is
> **Driven** until a bench pass on real hardware (§Waves). The browser twin
> (`canary-local/assets/mode-sim.js`, drift-locked by
> `canary-local/tests/mode.test.js`) powers the public `/modes` page.

Related: [`dev_playground_43b.md`](./dev_playground_43b.md) (the bench that
pioneered the pattern) · [`board_capability_map_43b.md`](./board_capability_map_43b.md)
(what the board can do) · [`display_peripheral_catalog.md`](./display_peripheral_catalog.md)
(what plugs into it) · [`display_platform_vision.md`](./display_platform_vision.md).

---

## Why modes at all

One household buys one wall display. But the *same hardware* is also:

- the thing a maintainer needs on the workbench (peripheral bench),
- the thing a stand at a maker faire needs (a self-running story),
- the thing support needs when "my dash is amber" (diagnostics you can
  photograph),
- the thing a factory line needs (does every touch zone and pixel work?),
- and the thing that sells the product on a shelf (an attract loop that can
  never be mistaken for a real fleet).

Today the firmware answers one of those (the bench, via `FEATURE_PLAYGROUND`
/ the `devmode` NVS latch). The rest either don't exist or would each grow
their own ad-hoc latch. The mode system is the **one** mechanism that carries
all of them — and the discipline that stops "useful tool" from becoming
"bloated gadget."

## The no-bloat contract

A mode exists only if it clears **all** of these bars. This is the list to
argue against before proposing gear number six:

1. **It's a tool with an operator story.** Someone specific (maintainer,
   demo-giver, support, factory, shopper) needs the *device itself* to do
   this, phone-free and laptop-free. "It would be cool" does not qualify.
2. **It reuses the product's organs.** Modes compose the existing fleet
   model, faces, HAL, and diagnostics — a mode that needs its own parallel
   stack is a different product, not a mode.
3. **Its logic is a pure, host-tested core.** Same split as the bench, the
   Modbus master, and the RTC math: decisions in a define-free header with a
   g++ test; the runtime is a thin shell. (The registry itself follows this
   rule — see `mode_registry.h`.)
4. **It's feature-gated, default off, byte-neutral to the emulator**, with a
   dedicated PlatformIO env so CI compile-verifies it — the proven
   `-rs485`/`-can`/`-rtc` pattern from the capability map.
5. **It can never be mistaken for, or interfere with, the product.** Every
   non-fleet mode wears a persistent on-glass banner, and the policy table
   (below) pins what it may touch. A demo can't impersonate a fleet; a bench
   can't phone home.

## The gears

| Mode | One-liner | Network | Who it's for | Status |
|------|-----------|---------|--------------|--------|
| **fleet** | The product: the fleet face | full stack | the household | **Driven** (it's the firmware) |
| **bench** | Guided peripheral test bench ("dev mode" / sandbox) | none, ever | builders, maintainers | **Driven** (the playground; now dispatched via the registry) |
| **demo** | Scripted synthetic fleet through the *real* faces | none, ever | demos, shows, shelves, UX review | **Built · compile-gated** (`FEATURE_DEMO_MODE`); core host-tested; bench-pending |
| **debug** | On-glass diagnostics; the link is the patient | up, read-mostly | support, bench bring-up, field triage | **Built · compile-gated** (`FEATURE_DEBUG_MODE`); bench-pending |
| **arcade** | Touch/display QA that happens to be fun | none, ever | factory EOL, demo tables, burn-in | **Built · compile-gated** (`FEATURE_ARCADE`, dash-first); core host-tested; bench-pending |

### fleet — the product face

What ships. The only gear with OTA, the only gear with the watchdog armed,
the only gear that publishes. Every other mode exists in service of this one
and exits back to it. Nothing changes here.

### bench — already built, adopted as-is

The [dev playground](./dev_playground_43b.md) becomes the mode system's
first citizen, unchanged in behavior: dedicated env boots straight in;
`FEATURE_DEVMODE` builds latch in from Settings; 3-second long-press exits.
The only migration is the latch grammar: the runtime moves from the legacy
`devmode` bool to the `mode` token, and `resolve_boot_mode()` honors the old
bool forever (host-tested), so a unit latched under old firmware lands in
the bench it asked for after an update.

### demo — the story the real code tells

**The rule that makes demo mode cheap and honest: nothing is mocked
downstream of the script.** A fixed cast of four synthetic witnesses and a
~2½-minute looping storyline (`demo_script.h`) are fed through the **real**
`FleetModel` — the same `on_event`/status paths MQTT ingest uses — and
rendered by the **real** faces. If the demo looks good, the product is good;
if a UI regression lands, the demo shows it. That also makes demo mode a
standing UI soak test, not a marketing fork to maintain.

The storyline is engineered, not improvised (and host-pinned, so it stays
that way): every severity tier appears; the alarm beat holds ≥ 15 s so
hold-to-ack can be demonstrated on a live Alert; the loop **resolves to
all-quiet before wrapping** so a shelf unit looping all day never carries a
standing alarm across the seam; and every beat's severity is **drift-locked
to the real classifier** — the test runs each event name through
`fleet_model.cpp`'s `classify_event()`, so a vocabulary change can't quietly
defang the demo.

Honesty rails: a persistent **DEMO** chip on the glass (policy `banner`);
witness ids carry a reserved `demo-` prefix so they can never collide with a
real device; and the mode is network-silent — a demo unit physically cannot
join, observe, or advertise to a real fleet, because the stack is never
initialized (the bench's proven guarantee, inherited).

Uses, in order of expected mileage: sales/shows and the website video;
UX review without hardware state to arrange; enclosure/product photography;
shelf attract loop; soak-testing faces (run demo overnight, watch heap and
frame stats in the morning via debug mode).

Flavors: dash **and** watch — the script is flavor-blind because the faces
are. Optional fast-clock (script seconds ≠ wall seconds) is a wave-2+
nicety for reviewing night-mode transitions; not in the core.

### debug — the glass turns inside out

The one non-fleet gear that keeps the network **up**, because the network is
usually the thing being debugged. It renders what the firmware knows about
itself, full-screen, tap-to-page:

- **Link** — WiFi state/RSSI/channel/BSSID, IP; broker host:port, connect
  attempts, current backoff, last MQTT rc; mDNS referral state.
- **Time** — SNTP state, TZ rule + source (seed/NVS/tz_auto), RTC probe
  result when `FEATURE_RTC` lands.
- **Memory / render** — heap free/min/largest block, PSRAM, LVGL frame time.
- **Fleet, raw** — the witness table as the model sees it: id, liveness
  ages against the stale/lost deadlines, severity, badge, mute state.
- **Input** — touch crosshair with raw coordinates (the "is the touch panel
  lying" page).
- **Bus** — the live I²C census (borrowed from the bench station).
- **System** — fw version, reset reason, uptime, OTA slot, the `mode` token.

Two consumers shape the design. **Support-by-photo:** every page is
self-labeled and dense enough that "photograph the debug screen and send it"
replaces a serial log for first-line triage. **Bench bring-up:** several
steps of [`display_bench_bringup.md`](./display_bench_bringup.md) become
"open debug mode, read the page" instead of "attach a monitor."

Rails: read-mostly (it may *run* the existing diagnostics; it changes
nothing but its own paging), **no secrets on the glass** — SSID yes,
password never, key fingerprints not keys; OTA off (policy), publishes
nothing beyond the normal status heartbeat; watchdog off (a paused page is
not a hang). A `DBG1` serial grammar mirroring `PG1` is the optional
companion (same k=v discipline) for when a console *is* attached.

Note the existing web mirror + browser serial already cover the "I have a
phone/laptop" case; debug mode is deliberately the **no-second-device**
surface — the display debugging itself with its own glass.

### arcade — the QA suite wearing a costume

The honest version of "game mode": a factory/EOL test that a child can run.
One tiny game (working title **Canary Catch**: birds appear at random
positions and you tap them) is, deliberately and measurably:

- a **full-matrix touch test** — targets are placed so a completed round has
  hit every touch zone of the panel; misses and offsets are recorded,
- a **latency/readout check** — tap-to-react time per zone,
- a **display exercise** — full-screen motion and color sweeps between
  rounds (the burn-in / dead-pixel pass),
- and the demo-table magnet that keeps a booth crowded.

End of round, the score screen doubles as the QA report (zones hit, worst
latency, dropped touches) — photographable, same as debug pages. Strictly
LVGL primitives, no assets, no sound dependency; network-silent; dash-first
(the watch's 240 px round glass makes a poor arcade and a fine pass on this
wave). If this paragraph ever stops being true — if the game grows beyond
its measurement job — the no-bloat contract says cut it.

---

## The registry (implemented)

`include/canary/mode/mode_registry.h` is the source of truth; this section
is its commentary. NVS contract: one string under the existing `securacv`
namespace, key **`mode`**, values `fleet | bench | demo | debug | arcade`.
Key absent ⇒ fleet. Unknown/corrupt ⇒ fleet.

Boot resolution (`resolve_boot_mode`, host-tested):

1. A **dedicated bench build** (`FEATURE_PLAYGROUND`) boots the bench,
   always — the flash *is* the mode choice; latches are ignored. Unchanged.
2. Else the **stored token** wins *if this build carries that mode*
   (`mode_allowed` against the compiled `BuildCaps`); a mode the build lacks
   — firmware downgrade, flag removed — resolves to **fleet**. A wall
   display can never be stranded outside its product face by NVS contents.
3. Else the **legacy `devmode` bool** maps to bench (only when no token was
   ever written; a present token is the newer intent). The runtime rewrites
   the latch in the new grammar on first entry.

Policy (`mode_policy`, host-tested invariants):

| | network | OTA | watchdog | long-press exit | banner |
|---|---|---|---|---|---|
| fleet | ✅ | ✅ | ✅ | — | — |
| bench | — | — | — | ✅ | ✅ |
| demo | — | — | — | ✅ | ✅ |
| debug | ✅ | — | — | ✅ | ✅ |
| arcade | — | — | — | ✅ | ✅ |

Pinned by tests: **only fleet takes updates** (a bench/demo/debug session is
never the moment for an OTA, and a network-silent mode can't be talked into
one); **every non-fleet mode has the exit and wears the banner** (no roach
motels, no impersonation); **the watchdog arms only under fleet** (every
other gear stops the world on purpose).

## Entry & exit choreography

Uniform, and identical to what the bench already taught users:

- **In:** Settings → **modes** (the current "dev mode" row generalized to a
  short list of the modes *this build carries* — one row today, more as
  waves land). Each entry is confirm-gated, writes the token, reboots. Local,
  on-glass, deliberate — entering a mode is a reboot, never a live overlay.
- **Out:** hold the glass **3 s** → token cleared → reboot to fleet. Same
  gesture in every mode, learned once. (The bench's existing exit, adopted
  system-wide.)
- No hidden doors: no MQTT topic, no web endpoint, and no serial command may
  *enter* a mode. (A serial `mode fleet` escape in debug builds is the one
  allowed *exit* convenience, and it's optional.)

## Waves — the honest ledger

| Wave | Contents | Status |
|---|---|---|
| 0 | This spec; registry core + tests; demo storyline core + tests; CI steps. | **Done — host-verified** |
| 1 | Runtime glue: `main.cpp` branches via `boot_mode()`; the Settings row → modes list (`ModesList`/`ModeConfirm`); bench latch migrated to the token grammar (`mode_glue.cpp`, exits unified on `mode_exit_to_fleet`). | **Built** — compile-verified (`-dash-modes` / `-watch-modes` envs) |
| 2 | **demo**: `FEATURE_DEMO_MODE`; `src/mode/demo_mode.cpp` walks `DEMO_BEATS` into `the_fleet()` and renders the real faces (both flavors) under the DEMO chip (`lv_layer_top`); `DM1` serial. | **Built** — core host-tested; bench-pending |
| 3 | **debug**: `FEATURE_DEBUG_MODE`; `src/mode/debug_mode.cpp` — five tap-to-page faces (system+link / fleet raw / events / touch / i2c), non-blocking WiFi + bounded broker attempts, `DBG1` 1 Hz snapshots. | **Built** — bench-pending |
| 4 | **arcade**: `FEATURE_ARCADE` (dash); `src/mode/arcade_mode.cpp` — Canary Catch on the host-tested `arcade_logic.h` plan/stats/verdict; `ARC1` serial + the QA report screen. | **Built** — core host-tested; bench-pending |
| 5 | **The browser twin**: `canary-local/assets/mode-sim.js` (registry + storyline + latch semantics, DOM-free) drift-locked by `canary-local/tests/mode.test.js` against the Arduino mirror; carried to the website as `/modes` (gears, policy table, latch simulator, storyline player). **Plus the real thing:** the canary-local emulator page's "play the demo storyline" button walks the same drift-locked beats through the staged household into the REAL wasm firmware — signed chains and all, where the browser has Ed25519. | **Done — CI-tested** |
| 6 | **The bench session**: the executable checklist is [`board_43b_activation_bench.md` §6](./board_43b_activation_bench.md) — flash `canary-display-dash-modes`, walk doorway/latch/migration, each gear's pass signals (`DM1`/`DBG1`/`ARC1`), exits, then flip per-gear defaults where earned. | **Pending — needs hardware** |

Each wave follows the capability-map shipping reality: feature-gated default
0, dedicated env for CI compile-verification, byte-neutral to the emulator's
`dist/*.js`, pure cores host-tested first, and nothing claimed **Driven**
until it has run on a bench. When a wave lands, update this table and the
README in the same PR — the docs and the firmware must never disagree.

### Build it

```bash
# PlatformIO (compile-verification envs; CI builds these on every PR)
cd firmware/projects/canary-display
pio run -e canary-display-dash-modes     # 4.3B: fleet + all four gears
pio run -e canary-display-watch-modes    # watch: fleet + demo + debug

# Arduino CLI (the staged twin)
./setup.sh arduino modes                 # dash + every gear on the 4.3B
arduino-cli compile --profile modes
```

Enter a gear on the glass: **Settings → modes → (gear) → enter** (one-gear
builds keep the familiar "dev mode" row). Exit any gear: **hold the glass
3 s**. Serial grammars: `PG1` (bench, unchanged) · `DM1` (demo) · `DBG1`
(debug) · `ARC1` (arcade) — one `k=v` line discipline throughout.
