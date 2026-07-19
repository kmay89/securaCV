# canary.local — the family's guide, built from the firmware

A local, offline page where you **meet your Canary before you meet your
Canary**: every device is a live 3D pairing card; the displays run their
*actual firmware* in the browser; guided tours and symptom-first fix-it
flows stage the device into the exact state they're describing.

```
canary-local/
  index.html            the page (vanilla JS, no frameworks, no build step)
  choose.html           "Find your Canary" — the four-question front door
  assets/
    app.js              card gallery + device sheets + guide player
    scene3d.js          zero-dependency WebGL: procedural device bodies
    stl.js              STL → the same viewer (real printed parts)
    enclosure-lab.js    the parametric catalog, browsable (per-device tab)
    chooser.js          the needs-matcher UI
    chooser-data.js     questions + candidates + scorer (DOM-free, tested)
    guides.js           tours, fix-it flows, LED/chirp grammars (data)
    canary-local.css    Quiet Glass, on the web (palette from ui/theme.h)
  devices/
    registry.json       the device registry (one card per entry)
    enclosures.json     generated enclosure catalog (variants + .scad params)
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
| WiFi STA supervisor | scenario switch with the firmware's honest semantics (boot timeout → reboot; 5-min outage → reboot) | `src/emu_net.cpp` |
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
the configurator, the BOM.

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
soldermask + gold pads + shield + connectors, true to the millimetres they drew.

The pipeline mirrors the enclosure lab's "generated from source, drift-gated"
rule, one tier deeper:

| Stage | What | Where |
|---|---|---|
| Source | vendor STEP (open-hardware CAD) | `boards/vendor/*.step` (+ provenance README) |
| Generate | STEP → committed GLB (cascadio), mount parts filtered, materials baked | `canary-local/tools/gen_boards.py` ← `boards/boards.config.json` |
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

Boards today: XIAO ESP32-S3 Sense (WAP), Grove Vision AI V2 (Vision), Round
Display for XIAO (Watch). Boards without vendor CAD — e.g. the Waveshare
4.3B-BOX — will land as dimensional models reverse-engineered from the datasheet
+ photographs, clearly labelled as such.

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

## 7. Building

```bash
cd canary-local/emulator
./build.sh all         # needs emcc (apt install emscripten, or emsdk)
```

Pinned third-party (fetched once into `third_party/`, gitignored):
LVGL v8.4.0 · rweather/arduinolibs (Crypto) · ArduinoJson v7.4.1.
Artifacts in `dist/` are committed so the page works from a checkout
without a toolchain; CI rebuilds them and fails on drift.

## 8. Testing

- `emulator/web/harness.html` — bare boot bench (also the CI probe).
- `tests/canary_local.test.js` — Node tests for the DOM-free logic:
  the witness signing canonical is pinned against `trust.cpp`'s locked
  format (and a WebCrypto round-trip verifies a real signature over
  it), LED cadence translation covers every documented grammar row, and
  registry entries are checked against the committed artifacts' build
  stamps and the firmware tree's `CANARY_FW_VERSION`.
- `tests/boot_probe.mjs` + CI (`.github/workflows/canary-local.yml`):
  rebuilds both flavors from the tree, boots the watch in headless
  Chromium, and asserts the framebuffer flushed, the boot banner sang,
  MQTT round-tripped, TOFU pinning fired, and no page errors.
