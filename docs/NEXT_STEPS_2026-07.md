# Next steps — what the last ~100 PRs built, and what they left open

*Written 2026-07-24, from the merged history `#1080 → #1226` in this repo and
`#89 → #111` in [`securacv_website`](https://github.com/kmay89/securacv_website).*

This is a **map of the open edges**, not a wish list. Everything called "open"
below was checked against the code on `main`, not just read off a roadmap —
where a roadmap and the tree disagree, the tree wins and the disagreement is
recorded in §4.

Two rules this document inherits: a group of Canaries is a **fleet**, never the
bird-word (see [`CLAUDE.md`](../CLAUDE.md)); and status is tiered
**`compile-tested → verified`**, where *verified* means real hardware.

---

## 1 · What the last ~100 PRs actually did

Eight tracks, in rough order of how much of the window they consumed.

| Track | What landed | Representative PRs |
|---|---|---|
| **Flasher, Lab & desktop apps** | Browser + native flashing, the Raspberry Pi hub writer (disk enumeration → typed write plan → write-authorization gate), Web Bluetooth reachability check, post-flash LAN discovery, the Witness Wall embedded in the Flasher | `#1082–#1093`, `#1103–#1124`, `#1135`, `#1146`, `#1150`, `#1173`, `#1187–#1195`, `#1203`, `#1208–#1216`, `#1219`, `#1223` |
| **Displays & the Nightstand Line** | Five "gears" mode architecture, the 4.3B/4.3C board catalog, Canary Voice sound engine, ES7210 mic bring-up + wake-on-sound, the portrait face + WS2812 ambient beacon, the look engine | `#1091`, `#1142`, `#1151`, `#1204–#1206`, `#1210`, `#1213`, `#1215`, `#1222` |
| **Self-\* / trust** | Pure crash-loop + safe-mode boot policy, the ASCII fleet view, the `n` console command, `fleet[]` in the self-manifest, break-glass `init`/`enroll`/`drill`, hub-core safety typing | `#1080–#1081`, `#1085`, `#1090`, `#1097`, `#1112`, `#1118–#1122` |
| **Fleet contract & the big screen** | The tvOS Witness Wall, the fleet discovery contract + runnable reference kernel, the host-app embed API, and `GET /api/fleet` served identically by every networked board | `#1179`, `#1192`, `#1194`, `#1211`, `#1216`, `#1226` |
| **Sensing** | Sentinel multi-sensor fusion (Phase 0), GNSS RMC trust fix, ambient unattributed CSI glow, vehicle DBC signal profiles, Ranger Doppler witness, coarse mover-class | `#1147`, `#1150`, `#1154–#1155`, `#1163`, `#1165`, `#1184`, `#1186`, `#1190` |
| **Release & CI plumbing** | One-click whole-pipeline release, one-button "release firmware if changed", the OTA key ceremony script, bounded/cached CI jobs, the advisory review-threads gate | `#1141`, `#1145`, `#1159`, `#1188–#1189`, `#1199`, `#1214`, `#1219–#1221`, `#1224` |
| **Strategy, legal & commercial** | Trademark policy + grants registry, licensing structure, `TERMS.md` + liability posture, the shipping/fulfillment and Pro Cam channel docs, the concept catalog (Poolwatch, Curbwatch, Feeder, Clime, Hearth, Chore, Gatekeeper) | `#1156`, `#1164`, `#1172`, `#1175`, `#1177`, `#1183`, `#1196–#1197`, `#1200`, `#1202`, `#1207`, `#1218` |
| **Website** | Witness Wall (skins, real fleet discovery, XSS fix, embed API), store payments, the 3D/AR model pipeline + Render Lab, `/verify`, `/industry`, the company trust pages | website `#89–#111` |

**The through-line.** The window was dominated by *getting a device from box to
bench to wall* — flashing, bring-up, and making a just-flashed Canary visible.
The architecture matured alongside it: pure host-tested cores in `common/` with
thin per-board glue, CI-pinned formats ahead of device wiring, and sync guards
against copy-paste drift. `docs/FLEET_PARITY.md` (`#1226`) names that doctrine.

**The counterweight.** Almost none of it is bench-validated. The Nightstand
Line says so plainly — "the landed flavors are *compile-tested*; nothing is
bench-validated" — and that caveat generalizes across the window.

---

## 2 · What they left open

Ordered by consequence, not by effort.

### P0 — the A/B rollback safety net is written but inert in shipping builds

The highest-stakes gap in the window, and it is invisible from the outside
because the engine looks done.

`firmware/common/ota/src/securacv_ota.cpp` overrides the Arduino core's weak
`verifyRollbackLater()` so a freshly-flashed image stays `PENDING_VERIFY` until
it passes its post-flash self-tests — a crash, hang, or brownout before
confirmation reverts to the previous image. That override is `#if`-guarded:

```
#if defined(CONFIG_APP_ROLLBACK_ENABLE) || defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
```

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is set in exactly one place in the
tree — `firmware/projects/canary-ota/sdkconfig.defaults` and
`.production`. The shipping Arduino/PlatformIO builds (`canary`, `canary-wap`,
`canary-display`) never set it, so the core auto-confirms every image on first
boot and **the revert net compiles out**. A bad OTA on a shipping Canary today
is not caught by A/B rollback.

Also still unwired from the same track: the crash-loop counter into the boot
path, and the safe-mode console. The *decision layer* for both is done and
host-tested (`firmware/common/health/boot_policy.h`,
`firmware/tests_host/test_boot_policy.cpp`, `#1085`) — only the boot-path
wiring is missing.

**Do:** enable the bootloader rollback config in the shipping builds, wire the
crash-loop counter, land the safe-mode console — then validate the whole thing
on hardware by deliberately flashing a failing image. This is the one item on
this list where "compile-tested" is not an acceptable resting state, because
the feature's entire purpose is to behave correctly when things go wrong.

### P1 — the Nightstand Line's tail

> **Since closed (2026-08-21):** three of the four deferred items below have
> landed since this snapshot was written. The original text is kept as the
> record; each close-out is noted inline, verified against the tree.

`#1210` (portrait face + beacon) and `#1222` (look engine) landed the firmware.
Four things are honestly deferred in
[`docs/hardware/display_nightstand_line.md`](hardware/display_nightstand_line.md) §7:

- **The emulator + Lab slice.** No nightstand or dash7 entries exist in
  `canary-local/emulator/build.sh` or `canary-local/registry.json` (verified:
  zero matches). Needs `createCanaryEmuNightstand` / `…Dash7`, the committed
  `dist/*.js` + `.meta.json`, the `fleet.html` script tags, and — the concrete
  blocker — a **172×320 portrait sizing case** in `buildDisplaySheet()`, which
  today hardcodes `dev.glass.round ? "232px" : "464px"`
  (`canary-local/assets/app.js:286`). Requires an Emscripten session to rebuild
  the committed wasm the `canary_local` tests assert on.
  *Since closed:* the slice landed — `build.sh` builds `nightstand` and
  `touch169` flavors, `emulator/dist/canary-display-nightstand.{js,meta.json}`
  is committed, and the registry (which lives at
  `canary-local/devices/registry.json`, not the path this snapshot cites)
  wires `createCanaryEmuNightstand`; twins now load on demand from the
  registry's module path rather than `fleet.html` script tags, and the Dash 7
  rides the dash build's `createCanaryEmuDash`. Residual: `buildDisplaySheet()`
  still hardcodes the round/landscape widths (`app.js`, now ~line 289).
- **The C6 build** (`canary-display-nightstand-c6`). Toolchain-blocked, not
  design-blocked: the C6 needs arduino-esp32 3.x, while the display graphics
  stack is pinned to `GFX@1.4.9`, the last core-2.x-compatible release. The
  gating work is a **core-3.x display base** — `GFX@^1.5.0` plus an LVGL/NimBLE
  3.x audit. That upgrade unblocks more than the C6; it is worth scheduling as
  its own piece rather than smuggling it into a feature PR.
  *Since closed:* the core-3.x display base landed and
  `[env:canary-display-nightstand-c6]` exists in
  `firmware/envs/platformio/canary-display.ini` (extends
  `canary_display_c6_core3`; listed in `firmware/flavors.json`).
- **Portrait-native modal polish.** The shared modals render in the watch's
  240-wide style on the 172-wide glass — functional, slightly overflowing.
- **BOOT-button input.** The 1.47" boards have no touch, so the button is the
  peek / summon-nightlight / acknowledge surface. Unwired.
  *Since closed:* wired — `canary-display`'s `main.cpp` reads
  `BOOT_BUTTON_PIN` through the `canary/io/boot_button.h` short/double/long
  classifier ("tap = peek, double = lantern, hold = acknowledge").

Plus the look-engine follow-ons `#1222` previewed but did not ship: the on-glass
info carousel, the settings-surface scene picker, an MQTT look topic, and the
BOOT-button scene cycle. The engine already produces everything they need —
these are glue, and they depend on the BOOT-button item above.

### P1 — the fleet contract has no aggregator

`#1226` made every *networked board* answer `GET /api/fleet` for itself.
[`tvos/discovery/DISCOVERY.md`](../tvos/discovery/DISCOVERY.md) names the Rust
hub/kernel as "the natural aggregator home" — a device answering for itself is
a fleet of one, and the Witness Wall's premise is the whole fleet on one screen.
The shared builder already exposes the open/append/close shape a hub needs to
list peers. This is the smallest remaining step between "a Canary appears on
the wall" and "your fleet appears on the wall."

### P2 — the website's render ladder

[`docs/render-roadmap.md`](https://github.com/kmay89/securacv_website/blob/main/docs/render-roadmap.md)
carries six explicit TODOs, self-ranked. Highest value and most uniquely ours:
**label / decal placement**. Then spot-varnish / UV-spot masks, a
roughness/micro-wear variation map, an opt-in WebGPU cinematic mode,
"view what's on screen" AR for the Showroom, and a Blender hero bake from the
real STL as the quality ceiling.

The ground rules that bit us there are already written down and CI-enforced
(no coplanar surfaces, outward normals, plain glTF only, real-world metric
scale). Keep them in front of any new geometry work.

### P2 — hardware validation, as a scheduled activity

Spread across the window and never collected in one place:

- Nightstand: RGB/ST7789 timings, the CH422G bit map, the ST7789 34-px offset,
  the S3 BGR/HSPI quirks, WS2812 color order.
- The `n` console command wants a real-terminal smoke test.
- The nightstand-s3 PSRAM mode carries an explicit *"VERIFY on the bench"*
  comment in `firmware/envs/platformio/canary-display.ini`.
- The P0 rollback work above needs a deliberate bad-image flash.

**Do:** one bench session against
[`docs/V1_BENCH_TEST_RUNBOOK.md`](V1_BENCH_TEST_RUNBOOK.md), extended to cover
the new boards, that promotes items from *compile-tested* to *verified*. The
tiering language already exists; what's missing is the session.

### P3 — carried, deliberately

Not forgotten, just not now: the C6 mesh/chirp gaps deferred to Phase 4b
(`docs/mesh_esp_now_evaluation.md` G6/G7), `SensorDisagreement` and
`FirmwareIntegrity` failure semantics (correctly withheld until the mechanisms
exist — emitting them without one would be fabrication), the
`ENTERPRISE_READINESS_TODO` items, and the website's held "Witness"
(unattributed Wi-Fi burst) idea, which is blocked on firmware.

---

## 3 · A sequence

Three waves. Each is independently shippable; nothing later depends on
something earlier being perfect.

**Wave 1 — arm the safety net.** The P0 rollback work end to end: config in the
shipping builds, crash-loop counter wired, safe-mode console, then the
bad-image bench test. Pair it with the bench session (P2), since both need
hardware on the desk and the runbook open.

**Wave 2 — finish the Nightstand, aggregate the fleet.** The Emscripten session
for the emulator slice (it unblocks the Lab, the flasher preview, and the
`canary_local` tests together), the BOOT-button and the look-engine glue that
depends on it, and the hub-side `/api/fleet` aggregator. The core-3.x display
base is the long pole here — start it early or explicitly defer the C6 again.

**Wave 3 — render and polish.** Label/decal placement first (highest value per
the roadmap's own ranking), then the portrait modal pass, then the rest of the
render ladder as appetite allows.

---

## 4 · Roadmap drift found while writing this

Both roadmaps **under-report** what shipped. That is the safer direction to be
wrong in, but it costs real planning time — the work looks open when it isn't.

- [`docs/design/self_star_roadmap.md`](design/self_star_roadmap.md) TODO 1 says
  the manifest `fleet[]` field is "still to wire." It shipped in `#1112`:
  `firmware/common/attest/self_manifest.h` declares `const Peer* fleet` /
  `fleet_count` and emits the `fleet` key. The doc also lists the website
  `/fleet` page as an open surface.
- The website's `docs/roadmap.md` still carries **"TODO — `/fleet` page (coming
  soon)"** with an approach section. That page is live and complete: `fleet.html`
  is routed in `_redirects`, the WebSerial logic was factored into the shared
  `js/serial.js` helper exactly as the TODO prescribed, `js/fleet.js` renders it,
  and `tests/fleet-facts.test.mjs` is the anti-rot test the TODO asked for.

**Do:** move both entries to their "Shipped" sections in the same pass, and note
the residual — a real-terminal smoke test for `n` — where it belongs, under
hardware validation rather than as unfinished feature work.

---

## Related

- [Parity by architecture](FLEET_PARITY.md) — the doctrine `#1226` established
- [Self-\* roadmap (design)](design/self_star_roadmap.md) — TODO 1 / TODO 2 detail
- [The Lab & Flasher experience (design)](design/flasher_experience.md) — the phase plan the flasher track follows
- [The Nightstand Line](hardware/display_nightstand_line.md) — §7 landed vs. staged
- [V1 bench test runbook](V1_BENCH_TEST_RUNBOOK.md) — the hardware-validation spine
- [Release process & channels](RELEASE_PROCESS.md)
