# canary.local — the family's guide, built from the firmware

A local, offline page where you **meet your Canary before you meet your
Canary**: every device is a live 3D pairing card; the displays run their
*actual firmware* in the browser; guided tours and symptom-first fix-it
flows stage the device into the exact state they're describing.

```
canary-local/
  index.html            the page (vanilla JS, no frameworks, no build step)
  assets/
    app.js              card gallery + device sheets + guide player
    scene3d.js          zero-dependency WebGL: procedural device bodies
    guides.js           tours, fix-it flows, LED/chirp grammars (data)
    canary-local.css    Quiet Glass, on the web (palette from ui/theme.h)
  devices/registry.json the device registry (one card per entry)
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

## 5. Roadmap (scenario waves)

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

## 6. Building

```bash
cd canary-local/emulator
./build.sh all         # needs emcc (apt install emscripten, or emsdk)
```

Pinned third-party (fetched once into `third_party/`, gitignored):
LVGL v8.4.0 · rweather/arduinolibs (Crypto) · ArduinoJson v7.4.1.
Artifacts in `dist/` are committed so the page works from a checkout
without a toolchain; CI rebuilds them and fails on drift.

## 7. Testing

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
