# canary.local — the Lab, built from the firmware

A local, offline **Lab** where you **meet your Canary before you meet your
Canary**: every device is a live 3D pairing card; the displays run their
*actual firmware* in the browser; guided tours and symptom-first fix-it
flows stage the device into the exact state they're describing.

The whole Lab is one **six-stage build line** — Choose · Build · Flash ·
Sense · Home · Prove — described once in `build-line.json` and rendered
everywhere from that single manifest: the adaptive shell (`lab.html`), the
isometric room, the site map, the desktop app, and the marketing site's
`/lab` page. If a page isn't on the manifest, it isn't on the line —
and `tests/build_line.test.js` fails CI the moment a committed page and
the manifest disagree. See [`build-line.README.md`](build-line.README.md).

```
canary-local/
  lab.html              THE front door — the adaptive Lab shell (source-list
                        sidebar at desktop widths, six-stage tab bar on
                        phones), rendered from build-line.json
  build-line.json       the Lab's single source of truth: six stages, every
                        bench and depth, site handoffs, redirects, and the
                        HTML documentation inventory
  site-map.html         the complete Lab map, rendered from the same manifest
  room.html             the isometric workshop (the shell's Overview iframe)
  index.html            legacy front door — redirects to lab.html (or to
                        fleet.html#… for old card deep-links)
  fleet.html            "Meet the fleet" — every device as a live 3D pairing card
  start.html            "Get started" — mission picker, 3-OS paths, one-tap-copy gates
  choose.html           "Find your Canary" — the four-question front door
  workshop.html         "The Workshop" — spec options, watch the case respond (§4c)
  catalog.html          "The Case Catalog" — browse every enclosure (§4b)
  find.html             "Find your case" — three questions → one recommended case
  boards.html           "Boards" — every board + pin flags + wiring (§4g)
  flash.html            "Flash over USB" — the real in-browser flasher
  wap.html              "First boot" — captive-portal setup, serial + MQTT (§4i)
  vision.html           "The Vision" — model load, aim card, tuning (§4k)
  eyes.html             "Through Canary eyes" — your webcam feeding the real firmware wasm
  sense.html            "The Sense" — radar school: meet it, place it, set it up
  senselab.html         the Sense's advanced depth — the radar dev bench (§4l)
  radar-dev.html        "The Proving Ground" — test a flashed Sense, live or emulated
  smoke.html            "Listen for a smoke alarm" — the WAP's ears, real firmware wasm
  dash-mic.html         "What a listening Canary hears" — the 4.3C decision core
  homeassistant.html    "The Hub" — Home Assistant on a Raspberry Pi (§4f)
  house.html            "The Canary House" — isometric home, whole fleet in place
  scenes.html           "Watch it work" — zoom into one witness doing its one honest thing
  vault.html            "The Vault" — sealed evidence + break-glass by quorum (§4j)
  operator.html         "Set up break-glass" — the Vault's operator depth (§4j·2)
  assets/
    app.js              card gallery + device sheets + guide player
    start.js            the Get Started driver (copy-gate policy + deep links, DOM-free core tested)
    house.js            iso renderer + sensing animations + visitor walk
    house-data.js       rooms + perches + walk (DOM-free, tested)
    scene3d.js          zero-dependency WebGL: procedural device bodies
    stl.js              STL → the same viewer (real printed parts)
    enclosure-lab.js    the parametric catalog, browsable (per-device tab)
    board-room.js       the Board Room: pin-flag overlay + wiring bench
    playground-sim.js   the Playground: PG1 driver port (DOM-free, tested)
    chooser.js          the needs-matcher UI
    chooser-data.js     questions + candidates + scorer (DOM-free, tested)
    guides.js           tours, fix-it flows, LED/chirp grammars (data)
    canary-local.css    Quiet Glass, on the web (palette from ui/theme.h)
  devices/
    registry.json       the device registry (one card per entry)
    enclosures.json     generated enclosure catalog (variants + .scad params)
    wiring.json         wiring harnesses (builds = permutations; signals
                        named for future live pin emulation — see §4g)
    playground.json     Waveshare 4.3B peripheral bench (generated from
                        pins.h + the dev-playground firmware — see §6)
  enclosures/preview/   coarse preview meshes for in-dev designs (rendered
                        by tools/gen_enclosures.py --render; the library's
                        own "committed STLs are print-validated" policy
                        stays intact in docs/hardware/enclosure)
  tools/gen_enclosures.py  README tables + .scad customizer → enclosures.json
  emulator/
    build.sh            firmware → WebAssembly (one artifact per flavor)
    shim/               the silicon boundary (see below)
    src/                emulator-side HAL + scenario bus
    web/emu-shell.js    the bench: power, LAN, fleet, serial, a finger
    web/bench.js        the power plane: cable, battery, switch, straps,
                        hardwired LEDs, ROM banners (DOM-free, tested)
    web/harness.html    bare oscilloscope page (dev + CI boot test)
    dist/               committed artifacts + build stamps (*.meta.json)
```

**Sibling surface:** the display firmware also serves its own tiny
steady-state page from the device (`src/net/glass_web.cpp` — live
mirror, 3D model, help, master settings, browser serial monitor; PR
#903). That page is what answers on your LAN; *this* page is the
repo-level teaching bench with the full firmware in wasm — learn here,
then meet the same ideas on the device's own mirror.

Serve the repo root with any static server and open `/canary-local/`:

```bash
python3 -m http.server -d /path/to/securaCV 8000
# → http://localhost:8000/canary-local/
```

Nothing phones anywhere: no CDN, no fonts, no analytics, no fetches
outside the page's own directory. Invariant IV extends to the docs.

---

## 1. The core decision: the emulator IS the firmware

The display emulator is **not a re-implementation** — it is
`firmware/projects/canary-display` compiled to WebAssembly:

- `src/main.cpp` runs **verbatim**: the same `setup()`/`loop()`, the same
  gesture policy (tap = page, long-press = acknowledge, modal ownership),
  the same brightness ladder and quiet-hours veto, the same MQTT
  reconnect backoff.
- The LVGL faces (`glance_ui`, `dash_ui`, `settings_ui`, `commission_ui`,
  `splash`, `canary_mark`) compile unmodified against LVGL v8.4.0 — the
  same pin as `envs/platformio/canary-display.ini`.
- The care/fleet layers (`bird_mood`, `care_glue`, `fleet_model`,
  `glass_settings`, journal, mutes) are the firmware's own bytes — they
  were already "pure logic, host-testable" by design; this page is that
  design promise, cashed.
- `net/mqtt_mgr.cpp` — the fleet dispatcher, TOFU pin store, and Ed25519
  chain verification — compiles for real, against the same
  rweather/Crypto library the device links. **"✓ verified" in the
  browser is a real signature verification**, not theater: the page's
  simulated witnesses sign chain heads with WebCrypto Ed25519 keys and
  the wasm firmware verifies them against pins it took on first contact.

What is *replaced* is exactly the silicon boundary, one shim per wire:

| Real thing | Shim | Where |
|---|---|---|
| GC9A01 / RGB panel via Arduino_GFX | RGB565 flush → RGBA framebuffer → canvas/3D texture | `src/emu_hal_display.cpp` |
| CST816S / GT911 touch | pointer events, polled by the firmware's own gesture machine | same |
| LEDC backlight PWM (day 8-bit, night 13-bit) | brightness of the on-page glass + 3D glow | same |
| LEDC piezo tone | Web Audio | `shim/Arduino.h` + app |
| NVS (Preferences) | in-memory map, mirrored to JS; preseed/wipe = provisioned vs first-boot; survives emulated reboots | `src/emu_support.cpp` |
| millis()/time() | virtual clock — scenario can scale (×60 staleness demos) or stage the local hour | same (+ `--wrap=time`) |
| WiFi STA supervisor | scenario switch with the firmware's honest semantics (boot timeout → boot completes and the loop owns the retry, never a reboot; an outage on a link that once worked → reboot after `WIFI_OUTAGE_REBOOT_MS`) | `src/emu_net.cpp` |
| PubSubClient socket | scenario broker: retained rows, LWT, wildcard replay | `src/emu_mqtt.cpp` |
| mDNS discovery / broker gossip | scenario referral (teaches the self-healing rebind) | `src/emu_net.cpp` |
| esp_random / mbedtls SHA-256 | page entropy (seedable) / vetted compact SHA-256 | shims |

Two firmware TUs are excluded outright (`hal/display_*.cpp` — they *are*
the silicon) and four are stubbed with honest signage (`ota_mgr` declines
installs, `chirp_scan` reports no radio, `provision` ships provisioned —
see §5 Roadmap).

**Drift is structurally impossible** at the logic layer: there is no
second implementation to drift. If a PR changes the mood engine or the
halo layout, the next `build.sh` run ships that change to the page,
and CI fails if the firmware stops compiling against the shim boundary.

## 2. The device registry — one card per device, forever

`devices/registry.json` is the page's only source of device knowledge.
One entry = one pairing card. An entry with an `emulator` block gets live
firmware behind its glass; witness entries (no glass) get decoder cards
(LED grammar, chirp meanings, joining paths) instead.

Adding a future Canary = adding one registry entry + one procedural body
in `scene3d.js` (dimensions from its enclosure `.scad` — the same
millimeters, so the card is the printed thing). If it has a screen, add a
`build.sh` flavor wiring its config/pins dirs; the shim layer is shared.

## 3. Versioning: cards for every firmware version

The family versions in lockstep (`CANARY_FW_VERSION`, one tag per
release train — see `docs/firmware_ota.md`). The page rides the same
train:

- Every artifact carries a build stamp (`dist/*.meta.json`: flavor, fw
  version, git sha, LVGL + ArduinoJson pins). The page footer and the
  registry's `fw_train` say which firmware you're touching.
- A release build (`fw-v*` tag) can publish per-version artifacts as
  `dist/canary-display-<flavor>@<version>.js` release assets; a registry
  entry's `emulator.module` may point at any of them. That is the whole
  per-version story — the page is static, the artifact names the version,
  and old versions keep working because they're just files.
- The working-tree build (this directory) always teaches the checked-out
  code: emulator, docs, and firmware move in one commit.

## 4. The teaching architecture

Three doors into the same live device, all driven by one scenario API
(`emu-shell.js`):

- **Tour** — the storyboard: halo → living canary → staleness (×60
  time-lapse) → gestures → proof QR → night floor → add-a-canary →
  honesty banners. Every step *stages* the device; the copy narrates
  what the glass is actually doing.
- **Fix it** — symptom-first flows ("screen looks dark", "witness
  stale/lost", "broker unreachable", "no ✓ / FAILED", "code won't
  scan"). Each step has a *show me on the glass* button that reproduces
  the state, plus an **On the real device** callout where the page and
  the bench diverge. The failure catalog comes from the repo's own
  docs (`canary_qr_onboarding.md`, `getting_started_canary.md`,
  bench runbooks) — including the count-coded LED grammar (groups of
  2/3/4/5) and the chirp vocabulary.
- **Try it** — free play: link switches, time controls, staged events,
  tamper, household ack-sync from a "sibling display".
- **Bench** — the physical test bench: the layer the firmware *can't*
  see, modeled where it lives (outside the silicon boundary, in
  `emulator/web/bench.js`). Pull the USB cable mid-frame, remove or
  brown out the battery, flip the board's ON/OFF slide switch (it gates
  the battery path only — its documented job), hold BOOT through RESET
  and park the mask ROM in download mode, then recover. The hardwired
  lights (PWR on the rail, CHG/DONE on the charge chip, the watch's
  batteryless-CHG flicker) answer only to physics — no firmware here or
  on your desk can turn them off, and the bench says so to your face.
  Rail transitions print the ESP32-S3 ROM's verbatim reset banners
  (staged — the ROM is the one program we can't compile to wasm;
  everything after the banner is the real firmware), NVS rides through
  every power event like the flash it is, and a live diagnostics pane
  (power source, boot stage, uptime, backlight duty, flush count, link,
  MQTT session, NVS keys) plus symptom-first debug flows ("a red light
  is always on", "waiting for download", "it died when I unplugged it")
  make it a troubleshooting mode for the real bench. Hardware facts per
  board come from the registry's `bench` block, never from code.

Plus **Wire** (the same USB-CDC boot log and MQTT traffic a bench
shows — because it's the same firmware) and **Specs** (registry facts +
deep links into the repo).

## 4b. Choosing and housing: the front door and the enclosure lab

**`choose.html` — Find your Canary.** Four questions (placement, needs,
hard privacy lines, power) score a curated candidate list of
device + enclosure pairs (`assets/chooser-data.js`, DOM-free, tested).
The scorer's promises are pinned by tests: a "no cameras" answer removes
every camera device unconditionally; outdoor placement only ever
recommends sealed sets; every recommendation carries its true status —
released (print-validated, shipping) vs in-development, said to your
face. Matches link onward: the device's live card, the printable STLs,
the configurator, the BOM. The chooser also accepts deep-link pre-fills
(`choose.html#place=door&want=see,prove…`) so other pages can hand it a
half-answered quiz.

**`house.html` — The Canary House.** An isometric cutaway home with the
whole fleet perched where it belongs, each device animating the way it
actually senses (camera cone, WiFi-field ripples, radar arcs, breathing
wave, display glow) and a "walk a visitor through" mode whose witness
feed shows the ONLY thing the fleet ever emits: small signed claims.
Perches toggle on/off to size a real fleet; every perch is a chooser
candidate (`assets/house-data.js` imports `chooser-data.js`, so titles
and statuses cannot drift), and every perch deep-links back into the
chooser with its answers pre-filled. Promises pinned by
`tests/house.test.js`.

**The Enclosure tab** (every device sheet). The enclosure library *is* a
set of parametric OpenSCAD configurators; the lab is their showroom:
variant pills (from the catalog README's own tables), the real printed
parts spinning in the same WebGL viewer the device cards use (STL parsed
in ~100 lines, `assets/stl.js`), true dimensions from each mesh's
bounding box, and the configurator's full parameter map — groups, enums,
ranges, comments — parsed straight from the `.scad` customizer
annotations by `tools/gen_enclosures.py`. In-development designs render
from coarse preview meshes clearly marked as such; the library's
"committed STLs are print-validated" policy is untouched.

**Print guide (in the Enclosure tab).** Pick your filament color and the
parts wear it; flip to the print guide and they sit on a gridded build
plate exactly as modeled (the .scad sources put z=0 on the plate in the
documented orientation — "prints face-down", "open-face-up"). A layer
slider scrubs a TRUE cross-section of the mesh at 0.2 mm (plane-triangle
slicing, `assets/print-guide.js`, Node-tested); an overhang toggle tints
faces steeper than 45° that would need support (these parts are designed
not to); the settings card quotes the catalog's own guidance per part
(TPU for gaskets, face-down lids, the fit coupon first). A guide, not a
slicer — nothing is an invented toolpath.

**Build it (every device sheet).** BOM with required/optional totals and
per-part sourcing straight from `docs/hardware/bom_*.csv`, assembly steps
from the enclosure catalog's own §Assembly, and the CI-generated
CycloneDX SBOM explained and linked — all emitted into
`devices/build.json` by the same generator, drift-gated, so "how to
build it" can never silently rot.

Live parameter re-rendering in the browser (openscad-wasm) is a wave-2
hook: the lab's data model already carries everything it needs (the full
parameter schema per configurator); what's missing is only the ~15 MB
evaluator, deliberately not shipped by default.

## 4c. The Workshop: spec it like a car, build it like a kit

`workshop.html` is the production journey — Configure → Print → Gather →
Assemble → Flash → Your build — and its rule is that every pane is
someone else's maintained file, re-emitted as `devices/workshop.json` by
the same generator and drift-gated in CI:

| The page shows | Parsed from |
|---|---|
| option checkboxes + consequences ("LiPo → battery bay, enlarges the case") | the `.scad`'s own customizer comments |
| packages with real outer dimensions | the enclosure README's preset table |
| each package's case parts | the variant tables (committed, print-validated STLs) |
| option → parts links (`opt_gps` → `M1 L76K`) | `docs/hardware/bom_*.csv` rows — **verified at generation**: a renamed RefDes fails the build |
| option → firmware links (`opt_gps` → `FEATURE_GNSS`) | `firmware/configs/*/config.h` — same verification |
| reference builds with prices | the BOM's own `# … (REF+REF)` summary rows |
| drill templates (print at 100%, 20 mm calibration square) | `template_*.svg`, rendered from `canary_templates_2d.scad` |

The honesty ribbon on the 3D viewport is the design's heart: tick a
combination that matches a curated preset and you're looking at exactly
your case (with its rendered dimensions); deviate and the ribbon says
"custom combo — showing nearest preset", while the **OpenSCAD parameter
set** download always encodes your exact choices (named after the scad,
so OpenSCAD's customizer picks it up as a preset automatically). Two
audiences, one page: "Just dreaming" hides RefDes/flag chips and
formulas; "I'm building it" shows the whole sheet. Chooser results and
device sheets deep-link in (`workshop.html#<device-id>`).

`tests/workshop.test.js` pins the promises (every package resolves to a
real set, every ref/flag link is live, the wap presets carry the
README's own option story, mesh volume math is exact on a reference
cube); `tests/workshop_probe.mjs` drives the real page in CI — packages
render, the ribbon flips, the checklist speaks BOM, every stage walks.

## 4d. The Board tab: the vendor's actual CAD, rendered offline

Every device sheet with a modeled board gets a **Board** tab — the off-the-shelf
board it runs on, as the vendor's own CAD, spinning in the same WebGL viewer the
printed enclosure parts use. Not an illustration: Seeed's published STEP, real
soldermask + gold pads + shield + connectors, true to the millimeters they drew.

The pipeline mirrors the enclosure lab's "generated from source, drift-gated"
rule, one tier deeper:

| Stage | What | Where |
|---|---|---|
| Source | vendor STEP (open-hardware CAD), or a procedural builder for boards with no vendor CAD | `boards/vendor/*.step` (+ provenance README) |
| Generate | STEP → committed GLB (cascadio) or code → GLB (procedural), mount parts filtered, materials baked | `canary-local/tools/gen_boards.py` ← `boards/boards.config.json` |
| Artifact | committed mesh the page loads | `canary-local/boards/*.glb` |
| Facts | dims · triangles · materials · pinout · provenance | `canary-local/devices/boards.json` |
| Loader | GLB → scene3d parts, ~180 lines, zero deps | `canary-local/assets/glb.js` |
| View | orbit + honesty ribbon + firmware pinout | `canary-local/assets/board-lab.js` |

The honesty guarantee: `boards.json`'s geometry facts are **recomputed from the
committed GLB by the page's own loader** (`tools/glb_facts.mjs` → `assets/glb.js`),
and `tests/boards.test.js` re-derives them in CI — so the numbers on the card can
never drift from the mesh the browser shows. Heavy STEP→GLB tessellation is a
local authoring step (`pip install cascadio trimesh`); CI verifies the committed
outputs with node only. Committed GLBs are not byte-drift-gated (tessellation
varies by cascadio build, exactly as preview STLs vary by openscad build); the
JSON is.

Boards today: the XIAO ESP32-S3 Sense + L76K GNSS (WAP), the plain XIAO
ESP32-S3 and Round Display for XIAO (Watch), Grove Vision AI V2 (Vision), the
Raspberry Pi 5 (the Home Assistant hub — Board Room only, no device tab), and
the Waveshare ESP32-S3-Touch-LCD-4.3B (Dash). The Waveshare has no vendor CAD,
so it lands as a **dimensional model reverse-engineered** — built procedurally by
`gen_boards.py` (rounded shell + boolean-cut screen/IO via `shapely`+`manifold3d`),
its proportions taken from a published community shell for the 5″ sibling used as
a dimensional reference only (attributed, not redistributed — see
`boards/vendor/README.md`) and its terminal silk from the firmware board notes.
Clearly labeled as such in its provenance and the viewer ribbon (see §4g).

## 4e. The Assemble tab: the build, from the real parts

Every device with a choreographed build gets an **Assemble** tab — a LEGO-style
guide that reads the device two ways:

- **Exploded** — scrub a slider from *together* to *exploded* and the whole
  device fans apart along its assembly axis, dashed leader lines tracing each
  part back to where it seats.
- **Step by step** — a player where each part flies into place along its
  insertion axis with a little ease-out "click", the camera follows the action,
  a progress rail tracks where you are, and the caption is the enclosure
  catalog's own §Assembly sentence. Autoplay, prev/next, keyboard arrows.

Every part on the stage is the real thing: the printed enclosure STLs
(`stl.js`), the vendor board GLB (`glb.js`), and the fasteners/battery/magnet
built procedurally to the BOM's sizes (`assembly.js`). Only the *choreography* —
seated transforms, explode/insert vectors, camera, which part appears when — is
authored, in `devices/assembly.json`. `tests/assembly.test.js` gates the honesty:
every part resolves to a file that exists, every quoted step maps to a real
build.json README step, and every claimed quantity matches the BOM (a `×4`
screws badge can't drift from the BOM's four). The ribbon says so to your face:
*"every part is the real thing… the choreography is staged."*

| Piece | File |
|---|---|
| Engine — explode, step player, camera tween, procedural fasteners | `canary-local/assets/assembly.js` |
| The tab — exploded slider, step player, callouts, progress rail | `canary-local/assets/assembly-lab.js` |
| Choreography (authored, validated) | `canary-local/devices/assembly.json` |
| Honesty gate | `canary-local/tests/assembly.test.js` |

Today: the Canary WAP weather + battery build. Vision and the Watch follow —
the engine is device-agnostic; they need only their `assembly.json` entry.

## 4f. The Hub: Home Assistant on a Raspberry Pi, the same way

`homeassistant.html` extends the teaching bench past the Canaries to the
place they converge: **why** a hub at all (N independent witnesses → one
wall, one verified timeline, automations with teeth), the **hardware**
(the assembly engine from §4e staging a Raspberry Pi 4 at its published
dimensions into a deliberately generic vented case), the **software** (a
bench terminal — the emulator idea translated for the CLI: you can't
`dd` a card from a web page, so it replays the real commands with
recorded transcripts, versions substituted live), and the **payoff** (a
faithful *sketch* of Home Assistant — labeled as such, unlike the wasm
emulator which is the real firmware — where MQTT discovery lands entity
by entity, the mic-mute switch signs into the chain, and a smoke-alarm
drill fires the alert blueprint into a phone notification).

Anti-rot, same rules as everything here — nothing written twice:

| Fact on the page | Source of truth |
|---|---|
| Entity names, MQTT topics | `docs/homeassistant_setup.md` (parsed) |
| Integration version, min HA | `custom_components/securacv/manifest.json`, `hacs.json` |
| Firmware train | `devices/registry.json` |
| HA OS / Core versions | weekly snapshot from `version.home-assistant.io` |

`tools/gen_homeassistant.py` regenerates `devices/homeassistant.json`
(drift-gated in `canary-local.yml`); the scheduled
`homeassistant-freshness.yml` workflow refreshes the upstream snapshot
weekly and opens a PR when it moved. Self-healing posture: a dead feed
keeps the previous snapshot verbatim (no diff, no PR, never a broken
page), and the page computes its snapshot's age out loud — past 120 days
it turns amber and names the workflow to go check.
`tests/homeassistant.test.js` is the honesty gate: demo entities must be
ones the doc promises, terminal templates must expand clean, every 3D
part must resolve, every step must stage something.

| Piece | File |
|---|---|
| The page | `canary-local/homeassistant.html` + `assets/hub.js` |
| Raspberry Pi + case, procedurally | `canary-local/assets/hub-parts.js` |
| Bench terminal (core is DOM-free, tested) | `canary-local/assets/hub-term.js` |
| The Home Assistant sketch | `canary-local/assets/hub-ha-ui.js` |
| Generated data | `canary-local/devices/homeassistant.json` |
| Generator | `canary-local/tools/gen_homeassistant.py` |
| Honesty gate | `canary-local/tests/homeassistant.test.js` |

## 4g. The Board Room: ECAD viewer + wiring bench (`boards.html`)

The Enclosure Lab's electronic sibling — a standalone page where **every**
board in the catalog is browsable, not just per-device tabs, and the pins
speak for themselves. Two modes:

- **the board** — the vendor GLB with 3D **pin flags** hung off the real pad
  geometry, synced both ways with the firmware pin-map table (hover a row,
  the flag lights; hover a flag, the row lights). An *every pad* toggle shows
  the full castellation map, not just the Canary-used pins. `planned` pins
  wear dashed flags.
- **wire it** — a LEGO-instruction harness: schematic peripherals (piezo,
  reed, GNSS, WS2812, divider, LiPo) arranged around the board on a virtual
  bench, every wire a colored curve that **lands on the exact castellated pad
  the firmware config names**, walked step by step with the classic mistakes
  called out (UART crossover, DIN vs DOUT, power connects last, polarity).

The load-bearing honesty rule: **pin anchors are measured on the committed
mesh, never eyeballed.** `tools/pin_anchors.mjs` clusters each GLB's
pad-color islands (union-find on a 0.5 mm vertex grid) and prints their
centers; the anchors committed into `boards.config.json` → `boards.json` come
from those islands, and `tests/boardroom.test.js` gates that every anchor sits
inside the mesh's own bbox. Rows whose feature couldn't be confidently located
on the vendor mesh (e.g. the Vision board's Grove socket) carry **no** anchor
and render table-only — a flag is a claim.

| Piece | File |
|---|---|
| The page | `canary-local/boards.html` |
| The room — pills, flags overlay, wire bench, step player | `canary-local/assets/board-room.js` |
| 3D→CSS projection for the flag overlay | `assets/scene3d.js` (`DeviceScene.project`) |
| Pin anchors + full pad maps (authored from mesh islands) | `boards/boards.config.json` → `devices/boards.json` |
| Anchor authoring aid | `canary-local/tools/pin_anchors.mjs` |
| Wiring harnesses (schema v1) | `canary-local/devices/wiring.json` |
| Honesty gates | `tests/boardroom.test.js` + `tests/boardroom_probe.mjs` |

`wiring.json` is deliberately data-shaped for what comes next:

- **Assembly permutations** — `builds` is an array per device, not a single
  harness: field vs bedside vs battery-less loadouts are new entries, no new
  code, and the Workshop's option flags (`FEATURE_GNSS`, battery bay…) map
  1:1 onto which peripherals a build carries. A future Workshop hook can
  select the matching build the way it already selects the matching STL.
- **Live pin emulation** — every firmware-facing connection carries
  `signal`/`dir` (`chirp`, `tamper`, `gnss_uart`, `vbat`…) matching the
  feature names in `firmware/configs`. That is the binding point for the
  emulator: the same scenario bus that feeds the display emulator's serial
  and MQTT (`emulator/src/emu_bus.h`) can publish GPIO state per signal, and
  the Board Room subscribes — pin flags glow when the firmware drives them,
  the buzzer wire pulses while a chirp plays. Pads first, signals named now,
  wasm bridge later; nothing in the schema needs to change.

## 4h. Testing the Board Room

`tests/boardroom.test.js` (node, CI): every anchor inside its mesh bbox;
every wiring build references a real board/peripheral/pin/color/step; wires
may not promise more than the firmware config (a `planned` pinout row forces
a `planned` wire); every step wires something. `tests/boardroom_probe.mjs`
(headless Chromium, CI): loads the page, checks the mesh lands in the scene,
the flags and table speak, the every-pad toggle works, the harness renders
one line per connection, and the step player walks.

## 4i. The WAP: first boot, from scratch (`wap.html`)

The Hub teaches the place the fleet converges; **The WAP teaches the first
five minutes of a single device.** The Canary WAP is the WiFi-CSI witness on
the XIAO ESP32-S3 Sense — and it is, literally, a **W**ireless **A**ccess
**P**oint: unprovisioned, it brings up a `SecuraCV-XXXX` network with a
device-unique WPA2 password and a captive portal, and you set it up from a
phone. `wap.html` stages that whole experience from the firmware's own bytes:

- **The board** — the same vendor CAD the Board Room shows
  (`seeed_xiao_esp32s3_sense.glb`), reused wholesale (`board-lab.js`).
- **The serial console** — the real boot banner and the ordered `setup()` log,
  streamed on *power on*; the `[WIFI] AP started:` line is the cue the phone
  waits for.
- **The phone** — a staged handset that catches the SoftAP live, then renders
  the firmware's **actual** `CAPTIVE_PORTAL_HTML` (verbatim, in a sandboxed
  iframe) and walks the five-step setup wizard to *online*.
- **The dashboard + MQTT** — once it joins, a labeled *sketch* of the
  on-device dashboard (pills/gauges from the getting-started guide) beside an
  MQTT-explorer view whose retained topics, payloads and all 24 Home Assistant
  discovery entities are the exact strings `csi_mqtt.cpp` publishes.
- **The sandbox** — wave, sit, leave, fire a T3 smoke / T4 CO cadence, hold the
  panic pad, mute the mic; each drives the pill, writes a witness record on the
  serial console, and publishes the MQTT the firmware would — all four surfaces
  moving together through one small bus.

Anti-rot, same rule as everything here — nothing written twice:

| Fact on the page | Source of truth |
|---|---|
| SSID `SecuraCV-XXXX`, `cv-…` password, `192.168.4.1`, NVS keys, timeouts | `arduino/canary_wap/{wap_server.h,setup_wizard.h,canary_wap.ino}` |
| The captive landing page (rendered verbatim) | `arduino/canary_wap/setup_page_html.h` |
| Setup routes + the five wizard steps | `canary_wap.ino` + `companion_pwa.h` |
| Boot banner + `setup()` log lines | `boot_banner.cpp` + `canary_wap.ino` |
| MQTT prefix/topics/payloads + 24 HA entities | `csi_mqtt.cpp` |
| Sensing pills + dashboard cards | `docs/getting_started_canary.md` |
| Firmware train · board · name | `devices/registry.json`, `devices/boards.json` |

`tools/gen_wap.py` regenerates `devices/wap.json` and **`sys.exit`s if any of
those literals moved** (the same posture as `gen_homeassistant.py`); the drift
gate in `canary-local.yml` re-runs it and `git diff --exit-code`s. The captive
page is the firmware's own HTML rendered verbatim; the dashboard is a *sketch*,
labeled as one to your face (the wasm display emulator is the real firmware —
this is not).

| Piece | File |
|---|---|
| The page | `canary-local/wap.html` + `assets/wap.js` |
| Phone/captive/wizard · dashboard sketch · MQTT explorer | `canary-local/assets/wap-ui.js` |
| The board + 3D reuse | `assets/board-lab.js`, `assets/scene3d.js`, `assets/glb.js` |
| Generated data | `canary-local/devices/wap.json` |
| Generator | `canary-local/tools/gen_wap.py` |
| Honesty gates | `tests/wap.test.js` + `tests/wap_probe.mjs` |

## 4j. The Vault: sealed evidence & break-glass by quorum (`vault.html`)

The other pages teach a device; **The Vault teaches three words** — *vault*,
*sealed*, *quorum* — with the docs and the code standing right behind every
claim. It's the answer to "what happens to the rare frame that actually
matters?": nothing is captured unless you armed it, what is captured is
**sealed** so the device can't read it back, and getting it out again takes a
**quorum** — no one alone.

- **Vault** — write-only escrow. The device holds only the operator's *public*
  X25519 key; everything is off by default; files stay local. The fail-closed
  capture decision table is the firmware's own (`vault_logic::capture_decision`).
- **Sealed** — a scrubbable five-step walk of the real construction (ephemeral
  X25519 → HKDF-SHA256, info `securacv/vault/seal/v1` → ChaCha20-Poly1305), the
  **byte-exact 64-byte `SVLT` header** (which is the AEAD associated data), and a
  *flip a header byte* toggle that shows the tag failing. The key derivation runs
  for real in WebCrypto where supported; the AEAD step is labeled as illustrated.
- **Quorum** — the interactive centerpiece, and **not theater**: it generates the
  trustees' Ed25519 keys in your browser, signs each approval with the kernel's
  exact domain separation (`securacv:pwk:trustee-approval:v2`), and counts
  *distinct* valid signatures exactly like `count_valid_distinct_approvals`. Meet
  the threshold and a single-use token unseals the evidence; try to force it with
  too few approvals, reuse one key for two slots, or re-open a spent token, and it
  refuses — in front of you. Every decision lands in a signed receipt log.

Anti-rot, same rule as everything here:

| Fact on the page | Source of truth |
|---|---|
| Quorum bounds, the `distinct ≥ n` grant rule, token fields | `src/break_glass/core.rs` |
| The three `:v2` signing domains | `src/crypto/signatures.rs` |
| Kernel envelope (`VLT2`, ChaCha20-Poly1305, DEK-wrap) | `src/vault/{crypto,format}.rs`; wired in `src/bin/witnessd.rs` |
| Device `.svlt` seal (info string, `SVLT` magic, ring bound, decisions) | firmware `vault_snapshot.cpp` / `vault_logic.h` + `docs/sealed_snapshot_vault.md` |
| Invariant I (No Raw Export) + V (Break-Glass by Quorum) | `spec/invariants.md` |

`tools/gen_vault.py` regenerates `devices/vault.json` and `sys.exit`s if any of
those literals moved; the drift gate re-runs it and `git diff --exit-code`s.
`tests/vault.test.js` re-derives the honesty (constants, domains, magic and
invariants must still exist in source) **and runs a real Ed25519 approval
round-trip** so the demo's quorum math is pinned; `tests/vault_probe.mjs` drives
the page in headless Chromium — seal walk, tamper toggle, and break-glass to a
real 2-of-3 with all the guardrails.

| Piece | File |
|---|---|
| The page | `canary-local/vault.html` + `assets/vault.js` |
| Seal walk + real-Ed25519 quorum demo (DOM-free cores exported) | `canary-local/assets/vault-ui.js` |
| Generated data | `canary-local/devices/vault.json` |
| Generator | `canary-local/tools/gen_vault.py` |
| Honesty gates | `tests/vault.test.js` + `tests/vault_probe.mjs` |

## 4j·2. The Operator's Bench: set up break-glass (`operator.html`)

The Vault teaches what a vault, a seal and a quorum *are*; **The Operator's Bench
teaches how you stand one up** — the four commands that just shipped in
`src/break_glass/cli.rs`: `init` → `trustee enroll` → `drill` → `doctor`. It's the
"from zero to a rehearsed quorum" companion.

- **The four commands** as cards — what each does, its flags, straight from the CLI.
- **The setup ceremony** is the interactive centerpiece: step through
  `init 2-of-3` → enroll three trustees (import a key, or *mint* one written
  `0600`) → `drill` → `doctor`, and watch a live "vault state" panel track the
  code's real behavior — the committed policy stays a **draft** below the
  threshold, goes **live as 2-of-2** the instant it's valid, then **strengthens to
  2-of-3**. The terminal output is the real binary's, recorded.
- **What `doctor` checks** and **what `drill` rehearses** (a throwaway sandbox that
  burns a single-use token) — spelled out, honest about the plaintext-key warning.

Anti-rot, same rule as everything here:

| Fact on the page | Source of truth |
|---|---|
| The four commands, their flags, the draft-state store, every recorded message | `src/break_glass/cli.rs` |
| `MAX_TRUSTEES`, the empty-roster rejection, the `distinct ≥ n` grant rule | `src/break_glass/core.rs` |
| The guided-setup narrative | `docs/operator_guide.md`, `docs/design/vault_operator_ux_v1_1.md` |

`tools/gen_operator.py` regenerates `devices/operator.json` and `sys.exit`s if any
command, flag or message moved; the drift gate re-runs it and `git diff
--exit-code`s. `tests/operator.test.js` re-derives the honesty (the CLI surface and
the load-bearing messages must still exist in source, and the ceremony's policy
transitions are checked); `tests/operator_probe.mjs` drives the page in headless
Chromium — the whole ceremony, confirming the policy goes draft → live 2-of-2 →
2-of-3 and the drill/doctor output renders.

| Piece | File |
|---|---|
| The page | `canary-local/operator.html` + `assets/operator.js` |
| Generated data | `canary-local/devices/operator.json` |
| Generator | `canary-local/tools/gen_operator.py` |
| Honesty gates | `tests/operator.test.js` + `tests/operator_probe.mjs` |

## 4k. The Vision: first watch (`vision.html`)

The WAP teaches a device that sets itself up; **The Vision teaches a device
you set up once, correctly.** The Canary Vision is the optical witness —
person detection runs on the Grove Vision AI V2 module's own NPU (Himax
HX6538 + Ethos-U55) and the ESP32 host only ever hears boxes over I2C.
`vision.html` stages its whole life:

- **The board** — the vendor CAD (`seeed_grove_vision_ai_v2.glb`), reused
  wholesale from the Board Room, plus the module spec table from the device
  guide.
- **The two ports** — a hands-on picker for the gotcha that trips everyone:
  two USB-C ports, two different computers; a wrong-port click explains *why*
  it physically can't work.
- **The model load** — Seeed's SenseCraft workspace staged click for click
  (Connect → USB Single Serial → Person Detection → upload → preview), ending
  in a live preview pane. The scene is cartoon actors, and says so — but the
  staged sensor boxes feed **the compiled Canary Vision firmware core**:
  production class/score filtering, primary-box selection, optical features,
  voxel tracking, NVS-backed tuning and the presence FSM. IoU/NMS remains on
  the labeled SSCMA-module side of that boundary, where it exists in hardware.
- **The host flash + serial console** — the README's own quickstart commands
  with one-tap copy, then the staged boot log; `Grove Vision AI ID=` non-zero
  is the lesson.
- **MQTT + HA** — every topic from `topics.h`, all 19 discovery entities from
  `ha_discovery.cpp`, the retained `cfg/state` and the signed events.
- **The Aim card** — the boxes-only channel, drawn the way the Lovelace card
  draws it: a wireframe box and a voxel cell on black. The exact `aim`
  payload streams in a ticker, key for key from `main.cpp`.
- **Placement + the sandbox** — use-case presets (entry, living room,
  hallway, workshop, office) that land real numbers on the same four
  NVS-backed sliders HA exposes; scenario buttons (walk, linger, leave, the
  cat, two people, the TV) drive one shared sim whose events ripple across
  the serial console, the MQTT explorer, the aim card and the event log.

Anti-rot, same rule as everything here — nothing written twice:

| Fact on the page | Source of truth |
|---|---|
| Thresholds, voxel grid, frame, cadences, aim timing | `include/canary/config.h` + `detect_config.h` |
| Every MQTT topic | `include/canary/topics.h` |
| The HA discovery entities (all 19) | `src/ha/ha_discovery.cpp` |
| Class/score filter, best box, optical features + voxel math | `include/canary/vision/detection_pipeline.h` (compiled into both ESP32 and wasm) |
| Runtime score/target/lost/dwell clamps | `src/detect_config.cpp` (the real implementation is linked into wasm) |
| The event vocabulary, voxel stability + FSM order | `src/state/{voxel_tracker,presence_fsm}.cpp` (linked into wasm) |
| The aim payload keys | `src/main.cpp` |
| Boot banner + log lines | `firmware/common/boot/boot_banner.cpp` + net managers |
| Ports, wiring, SenseCraft steps, recovery, symptoms | `docs/hardware/grove_vision_ai_v2_guide.md` + `canary_vision_getting_started.md` |
| Host envs + quickstart + tuning table | `firmware/projects/canary-vision/README.md` |

`tools/gen_vision.py` regenerates `devices/vision.json` and **`sys.exit`s if
any of those literals moved**; the drift gate in `canary-local.yml` re-runs it
and `git diff --exit-code`s. The preview scene is staged and labeled as such,
but there is no JavaScript copy of Canary decision logic: `build.sh vision`
compiles the production pipeline, detect config, voxel tracker and FSM into
`dist/canary-vision-core.js`. The page refuses to start when its generated
data differs from that runtime contract. CI is triggered by every
`firmware/projects/canary-vision/**` change, executes behavior tests against
the committed wasm, rebuilds it with pinned Emscripten 6.0.3, byte-diffs the
artifact, and runs the complete page again in Chromium.

| Piece | File |
|---|---|
| The page | `canary-local/vision.html` + `assets/vision.js` |
| Ports picker · SenseCraft stage · sim · aim card · sandbox | `canary-local/assets/vision-ui.js` |
| Production wasm boundary | `emulator/vision/` + `emulator/web/vision-core.js` |
| The board + 3D reuse | `assets/board-lab.js`, `assets/scene3d.js`, `assets/glb.js` |
| Generated data | `canary-local/devices/vision.json` |
| Generator | `canary-local/tools/gen_vision.py` |
| Honesty gates | `tests/vision.test.js` + `tests/vision_probe.mjs` |

### The module flasher (the real one, on `flash.html`)

The staged SenseCraft walkthrough above teaches the vendor path — but the
flash page now carries SecuraCV's **own module flow**: burn the pinned
person-detection model into the Grove Vision AI V2 over WebSerial, from our
page, with zero choices to make. The engine (`assets/we2-core.js`) is a
clean-room, fully-tested mirror of the module's ROM-bootloader protocol —
XMODEM/CRC-16 at 921600, the burn-address preamble block, the reboot
prompt — the same wire Seeed's open-source flasher speaks. The model asset
rides the release train (`.github/workflows/vision-model-release.yml` →
`manifest-vision-model.json`, SHA-256 pinned, MIT with attribution in the
firmware NOTICE), and after burning, the flow proves it: AT handshake, our
model card stored on-device (`AT+INFO`), one test inference, and an optional
live bench preview with the on-module TSCORE/TIOU sliders.

| Piece | File |
|---|---|
| Engine (DOM-free: CRC, XMODEM, preamble, state machine, AT parser) | `assets/we2-core.js` |
| WebSerial transport + the flow UI | `assets/we2-flash.js` (mounted from `flash.js`) |
| Catalog block + doc drift-gate | `tools/gen_flash.py` → `devices/flash.json` `we2_module` |
| Release asset pipeline | `.github/workflows/vision-model-release.yml` |
| Honesty gate (scripted fake bootloader, byte-level) | `tests/we2.test.js` |
## 4l. The Sense Lab: the radar pipeline, on the bench (`senselab.html`)

`sense.html` (radar school) teaches an owner to meet, place and provision the
device; **the Sense Lab is its engineering sibling — the dev bench** — because
the MR60BHA2 radar accepts no configuration commands at all (verified against
the vendor library and both ESPHome components), the only knobs that exist
are *where you put it* and *how the host firmware judges its claims*. The
page stages exactly that:

- **The hardware, all the way down** — the ADT6101P (2T2R, on-module
  Cortex-M3 running the whole vitals pipeline), the five-frame UART grammar
  with live checksums, the `[BENCH]` assumptions quoted from `mr60_uart.h`,
  and the frames the witness *refuses* to decode (phase waveforms, point
  cloud) arriving and being counted `unknown` in front of you.
- **The placement bench** — drag a person through top-down and side views;
  the 80°×80° sector, the near/mid range bands (the live CS_* knobs) and the
  1.5 m vitals envelope are the device's published geometry; the quality
  meter's U-curve (sweet spot ≈ 0.7 m), orientation projection and
  interference penalties are grounded in the vital-sign radar literature —
  every number parsed from `docs/hardware/mr60bha2_radar_notes.md`.
- **The pipeline** — real wire bytes stream through **line-for-line JS ports
  of the firmware's parser and FSMs** (pinned in CI to the same behaviors
  `firmware/tests_host/test_mr60_uart.cpp` pins), across the privacy
  chokepoint (watch distance and BPM be read and dropped), into
  Ed25519-signed witness events over the real v1 `sense` canonical.
- **Canary Cards** — the standardized widget-card layer
  (`docs/standard/CANARY_CARDS.md`, renderer `assets/canary-cards.js`):
  one HA-discovery entity, one card, on every surface — including the
  honest "provably absent" cards a presence-only build renders for BPM.
- **On the glass** — boot the real canary-display firmware (wasm) and this
  witness joins its fleet through the display's own MQTT dispatcher.
- **The power lab** — rails calibrated to Seeed's published 0.8 W kit
  figure; move the levers (modem sleep, heartbeat, LED) and watch
  claims-per-joule and the sensing share of the heat budget.

| Piece | File |
|---|---|
| The page | `canary-local/senselab.html` + `assets/senselab.js` |
| DOM-free cores — protocol, parser + FSM ports, physics, power (tested) | `canary-local/assets/sense-sim.js` |
| The surfaces — stage, console, knobs, power lab, glass bridge | `canary-local/assets/senselab-ui.js` |
| Canary Cards — schema, validator, renderer (tested) | `canary-local/assets/canary-cards.js` + `docs/standard/CANARY_CARDS.md` |
| Generated data | `canary-local/devices/senselab.json` |
| Generator (drift-gates firmware + hardware notes) | `canary-local/tools/gen_senselab.py` |
| Honesty gates | `tests/senselab.test.js` + `tests/senselab_probe.mjs` |

## 5. Where this lives (repo → Pages → securacv.com)

Three tiers, no lock-in, one source of truth:

1. **The repo** — everything here works from a checkout over any static
   server, fully offline. Versioned with the firmware it teaches.
2. **GitHub Pages** — `.github/workflows/pages.yml` publishes
   `canary-local/` (+ the enclosure files it references) on every main
   push. Enable once: Settings → Pages → Source → "GitHub Actions".
3. **securacv.com** — the marketing site adopts it whenever ready,
   either by pointing a custom domain/subdomain at Pages
   (e.g. `local.securacv.com`, one CNAME) or by copying the built site
   folder onto any host — the pages are self-contained and fetch
   nothing external, so they inherit the site's privacy posture by
   construction. The marketing page links "Find your Canary" as its
   how-to-pick tool; the repo remains where it's built and reviewed.

## 6. Roadmap (scenario waves)

- **Wave 2 — first-boot theater**: shim `WebServer`/`DNSServer` so
  `provision.cpp` compiles too, with a simulated phone sheet for the
  SoftAP captive-portal walk (`provision_core.h` is already pure).
- **Wave 2 — chirp fallback**: scripted BLE chirp injection while the
  broker is dark (the "burglar cut the internet" demo).
- **On-device serving**: the artifacts are single-file by design
  (`-sSINGLE_FILE`); gzipped they fit the WAP's embedded-assets pattern
  (`gen_web_assets_gz.py`) so the real `canary.local` can one day serve
  this page from flash. Until then it lives in the repo and any static
  host.
- **Settings/commissioning deep-links**: guide steps that open the
  settings surface or mint a commissioning QR directly (the firmware
  entry points exist; the tour currently narrates the gestures).
- **Board Room — build permutations**: more `wiring.json` builds per
  device (bedside/no-battery/mobile loadouts), Vision + Watch harnesses
  (the Watch's is a seating, not a wiring — the XIAO drops into the
  socket bars the mesh already locates), and a Workshop hook that picks
  the build matching the ticked options.
- **Board Room — live pins**: bridge the emulator scenario bus to the
  wiring `signal` names so flags glow and wires pulse when the firmware
  actually drives them (see §4g); the schema already carries
  `signal`/`dir` so this is an emulator-side export plus a subscriber.
  A first cut of the *live-text* half of this now ships as the
  **Playground** (`devices/playground.json` + `assets/playground-sim.js`,
  generated from the Waveshare 4.3B `pins.h` and the dev-playground
  firmware by `tools/gen_playground.py`, drift-gated by
  `tests/playground.test.js`): a per-peripheral bring-up bench whose
  `PG1` serial output is a line-for-line port of the firmware's own
  playground driver. The website renders it in 3D
  (`securacv_website/playground.html`); wiring flags/pulses on the
  emulator GPIO bus remain the follow-up.

## 7. Building

```bash
cd canary-local/emulator
./build.sh all         # requires the pinned Emscripten SDK 6.0.3
./build.sh vision      # rebuild only the Canary Vision production core
```

Pinned third-party (fetched once into `third_party/`, gitignored):
LVGL v8.4.0 · rweather/arduinolibs (Crypto) · ArduinoJson v7.4.1.
Artifacts in `dist/` are committed so the page works from a checkout
without a toolchain; CI installs the same pinned compiler, rebuilds them and
fails on byte drift.

## 8. Testing

- `emulator/web/harness.html` — bare boot bench (also the CI probe).
- `tests/canary_local.test.js` — Node tests for the DOM-free logic:
  the witness signing canonical is pinned against `trust.cpp`'s locked
  format (and a WebCrypto round-trip verifies a real signature over
  it), LED cadence translation covers every documented grammar row, and
  registry entries are checked against the committed artifacts' build
  stamps and the firmware tree's `CANARY_FW_VERSION`.
- `tests/bench.test.js` — the power plane's truth table, pinned:
  rail up ⇔ USB ∨ (battery ∧ switch ∧ charge>0); the switch gates only
  the battery; straps are sampled only at reset (BOOT low → download
  mode, even through `ESP.restart()`); LEDs follow the rail/charger and
  never the firmware; brownout at 0 %; the ROM banners' exact text; and
  every `BENCH_FIXES` flow stages cleanly. Registry `bench` blocks are
  validated (drivers name real wires, every LED carries its honesty
  note, witnesses carry no bench).
- `tests/boot_probe.mjs` + CI (`.github/workflows/canary-local.yml`):
  rebuilds both flavors from the tree, boots the watch in headless
  Chromium, and asserts the framebuffer flushed, the boot banner sang,
  MQTT round-tripped, TOFU pinning fired, and no page errors.
- `tests/bench_probe.mjs` — drives the real page's Bench tab in headless
  Chromium: USB pull with battery ride-through (the firmware never
  notices), switch-off rail death (honest-dark glass + serial), power
  restore (ROM banner, then a true re-boot), BOOT+RESET into download
  mode and back.
- `tests/wap.test.js` + `tests/wap_probe.mjs` — the WAP bench (§4i): the
  honesty test pins every SSID, route, MQTT topic, HA entity, boot line and
  wizard label to its firmware source and exercises the DOM-free serial/MQTT
  cores; the probe walks first boot in headless Chromium (power on → the phone
  catches the SoftAP → the firmware's captive HTML → the wizard reaches online
  → retained MQTT + all 24 discovery entities land → a smoke cadence alarms).
- `tests/vault.test.js` + `tests/vault_probe.mjs` — the Vault explainer (§4j):
  the honesty test pins the quorum constants, the three signing domains, the
  `VLT2`/`SVLT` magics and Invariants I & V to their source **and runs a real
  Ed25519 approval round-trip**; the probe drives the seal walk, the tamper
  toggle, and a real 2-of-3 break-glass with every guardrail (forge → denied,
  single-use → refused, reused key can't fill two slots).
