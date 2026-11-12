# Firmware Variant Parity Plan — canary (ACTIVE) ⇄ canary-wap (Arduino)

> **Decision (founder, this cycle):** do **not** pick one tree — bring **both** the ACTIVE
> modular tree (`firmware/canary/`, PlatformIO) and the `canary-wap` Arduino tree to **full
> bidirectional feature parity**, and ship both as supported v1 images.
>
> **Source of truth:** the CI-guarded dashboard in [`FEATURES.md`](FEATURES.md)
> (`.github/workflows/features-dashboard-guard.yml` blocks any ✅→⚠️/❌ regression without an issue
> ref). This plan is the *closure program* for the cells where the two trees disagree today.
>
> **Companion:** [`../docs/v1-roadmap.md`](../docs/v1-roadmap.md) (decisions on record) ·
> [`../docs/V1_BENCH_TEST_RUNBOOK.md`](../docs/V1_BENCH_TEST_RUNBOOK.md) (on-device proof) ·
> [`VARIANT_POLICY.md`](VARIANT_POLICY.md) + [`FIRMWARE_VARIANT_AUDIT.md`](FIRMWARE_VARIANT_AUDIT.md)
> (lifecycle + de-rot plan — note its item #6 recommends *convergence*, which §2 reconciles with
> this *parity* decision).
>
> **Honest framing:** this is a **multi-PR program**, not a one-shot. Several gaps are behavioral
> (mesh, BLE, RF, acoustic) and can only be *closed* — i.e. rated ✅ — once proven on hardware
> (see the bench runbook). Pre-bench, we land the code so it builds CI-green; the dashboard cell
> flips to ✅ when the bench test backs it.

> **Reconciliation note (2026-06-10, signed pull-OTA):** the OTA gap closed
> in BOTH trees at once via the shared engine at `firmware/common/ota/`
> (HTTPS manifest pull, Ed25519 release signature, A/B rollback, HA `update`
> entity, local-server option). canary consumes it through
> `build_src_filter`; canary-wap carries committed copies guarded by
> `firmware/scripts/check_ota_sync.sh` — the same committed-copy pattern as
> CSI and the boot banner. The release public key header is part of the
> sync set so one release signature serves pull-OTA on both trees AND the
> WAP's BLE OTA. End-to-end flow: `docs/firmware_ota.md`.
>
> **Rollback note (2026-09-29, restated 2026-11, sweep D23):** the
> dashboard reads "OTA A/B with rollback safety" ✅ for both trees, so §1
> no longer lists it and its §4 row's "Dashboard ✅" cell is ticked. The
> code is there — the WAP links the shared engine, and its
> `verifyRollbackLater()` override is live because the Arduino core's own
> precompiled `sdkconfig` enables the bootloader rollback config (the
> engine now `#error`s on a core that does not). The §4 row's
> "Bench-proven" cell stays open, because that column is the closure rule:
> nobody has yet reverted a bad image on either tree. The bench procedure
> is `docs/V1_BENCH_TEST_RUNBOOK.md` Track E (written against canary (PIO);
> the WAP confirms at the end of `setup()`, so the same bad-image flash
> applies).

---

## 1. Where the two trees disagree today

The two tables below are the rows of the [`FEATURES.md` dashboard](FEATURES.md#feature-parity-dashboard)
where its **canary (PIO)** column (ACTIVE here) and its **canary-wap** column (Arduino here)
differ, leaving out a row with a ➖ in either column: ➖ means the capability is out of that
tree's scope, so there is nothing to port. Group A holds the rows where the Arduino cell is ahead
and Group B those where the ACTIVE cell is (✅ over ⚠️ over ❌). A row whose two cells are equal is
not listed, whatever they read: both ⚠️ or both ❌ is a gap the trees share, not a parity item (TLS,
after the tables, is the shared gap this plan tracks). Each row names its dashboard row (the
capability text whole, or the text before its closing parenthetical) and carries that row's two
cells. The dashboard stays the source: `scripts/tests/test_parity_plan.py` (`lint.yml`'s unittest
step) fails when a row here, its cells or its group no longer match it, or when a dashboard row
the two trees differ on is missing, so a PR that flips one of those cells restates this list in
the same commit.

> **Status reconciliation (2026-06-09):** a spot-audit against current code found that
> **MQTT publish + HA Discovery is already present in the Arduino tree**
> (`firmware/projects/canary-wap/arduino/canary_wap/csi_mqtt.cpp` — `publish_discovery()` for HA
> entities + device triggers, `#include`d and called from `canary_wap.ino`, so it compiles and
> links in the green Arduino CLI build; the HA integration it feeds is validated by `hassfest`).
> Its closure bar is "compile + hassfest" (no bench), so the `FEATURES.md` dashboard cell and the
> §4 scorecard row were flipped to ✅. The mesh / chirp / RF / BLE cells were re-checked and left
> as-is — their ⚠️/❌ correctly reflect "code-present-but-bench-pending" (mesh) or "header-only /
> absent in ACTIVE" (chirp/RF/BLE), per Step 0. The remaining genuinely-open *code* gap in ACTIVE
> is the **chirp + RF-presence `.cpp` bodies** (bodies live only in the Arduino tree; the inert
> `[env:full]` `FEATURE_CHIRP`/`FEATURE_RF_PRESENCE` flags that stood in for them were removed on
> 2026-10-09, and each port adds its flag with its body).

> **Chirp API consolidation (2026-06-09):** the chirp **API** was consolidated as a precursor to
> the body port. `firmware/common/chirp/chirp_channel.h` is ratified as the **single canonical
> C-ABI chirp-channel API** (no build includes it yet: the canary-wap PIO lane that was said to
> consume it was the deleted `src/` scaffold; firmware/tests_host syntax-checks it, and canary-wap's
> `test_chirp_protocol_invariants.cpp` holds its values to the reference's, sweep F272); the Arduino
> `chirp_channel::` C++ namespace (in that lane's `mesh_network.h` + `chirp_channel.cpp`) is the
> reference implementation to adapt, and `chirp_api.h` (HTTP layer) / `ble_chirp.h` (separate BLE
> feature) are adjacent surfaces, not competing core APIs. The catalog + the
> dependency-adaptation map for the future `securacv_chirp` body live in
> [`../firmware/common/chirp/README.md`](common/chirp/README.md). **API only — no body landed
> yet**, so the dashboard/scorecard chirp cells stay ⚠️/☐.

### Group A — Arduino has it, ACTIVE is partial/absent → **port INTO ACTIVE**

| Capability | ACTIVE | Arduino | Closure is… |
|---|:---:|:---:|---|
| SD storage (append-only, `/WITNESS` `/HEALTH` `/CHAIN` `/EXPORT`) | ⚠️ | ✅ | code (the `/HEALTH` writer, [`canary/CONSOLIDATION.md`](canary/CONSOLIDATION.md) gap #17) + bench (SD card); `/EXPORT` rides the export row |
| Web UI (embedded PROGMEM dashboard) | ⚠️ | ✅ | code (compile + `selftest-ui`) |
| Camera peek (MJPEG stream, no frame storage) | ⚠️ | ✅ | code + **bench** (needs camera) |
| Mesh network (Opera / ESP-NOW) | ⚠️ | ✅ | wired already; **bench** (#610) |
| Mesh RSSI from ESP-NOW radio | ⚠️ | ✅ | code + bench |
| BLE discovery (Opera/Chirp/Nearby) | ❌ | ✅ | code + bench |
| RF presence detection | ❌ | ✅ | code + bench |
| Hub failover election | ⚠️ | ✅ | code + **multi-board bench** |
| Bluetooth setup door — Improv Wi-Fi over BLE | ❌ | ✅ | code + a decision (which envs carry it) + **bench** (U1, a phone): sweep F251 |
| Witness record export to /EXPORT/ | ⚠️ | ✅ | a decision first (sweep F476: port the canary-wap's staging write, or drop the directory and restate the row), then code + bench (SD card) |
| Chirp channel (broadcast beacon) | ⚠️ | ✅ | code + **two-board bench** |
| mDNS fleet advert (`_securacv._tcp`, canonical TXT schema incl. `dt`/`role` + broker gossip) | ⚠️ | ✅ | code (the canonical TXT keys, the note under the dashboard) + bench |
| Remote identify blink (HA `Identify` button on MQTT variants; HTTP `/api/identify` on WAP) | ❌ | ✅ | code (compile + hassfest) + bench (the blink) |
| Provisioning gate | ⚠️ | ✅ | **bench** (U1: one tap, one receipt); the code landed and CI-compiles |

### Group B — ACTIVE has it, Arduino partial/absent → **port INTO Arduino**

| Capability | ACTIVE | Arduino | Closure is… |
|---|:---:|:---:|---|
| BLE Scout pairing surface (proximity window, no MAC on the API, NVS-persisted registry) | ⚠️ | ❌ | code + bench (a beacon tag); the ACTIVE cell is ⚠️ as well |
| Capacitive touch (silent panic / enclosure tamper) | ✅ | ❌ | code + bench |
| Native deep-sleep HAL | ✅ | ❌ | code + bench |
| IR appliance activity (RMT NEC/RC5/Sony, salted-hash buckets) | ✅ | ❌ | code + bench |
| Internal temp drift tamper (ESP32-S3 die sensor) | ✅ | ❌ | code + bench |
| Sensing events signed into Ed25519 witness chain (T3/T4/panic/tamper/temp) | ✅ | ❌ | code + host test |
| Home Assistant MQTT auto-discovery for sensing entities (11 entities) | ✅ | ❌ | code (compile + hassfest) |
| Sensing dashboard panel (gauges + acoustic + touch + IR + temp + power) | ✅ | ❌ | code + `selftest-ui` |
| SD endurance wear estimate | ✅ | ❌ | code + host test |
| WiFi power save (modem sleep on battery, TX power control) | ✅ | ❌ | code + bench |
| WiFi auto-reconnect with exponential backoff | ✅ | ❌ | code (compile) |
| MQTT broker TLS — CA-verified or SHA-256-pinned socket, fail-closed, lab opt-in warns every connect | ✅ | ⚠️ | code + bench (a TLS broker): the CA mode is there; the pin and lab modes are refused, since esp_mqtt has no fingerprint hook and the pinned core builds esp-tls without its insecure mode ([per-variant audit](../docs/FIRMWARE_VARIANT_AUDIT.md)) |

**At parity on the dashboard, so not listed:** MQTT publish + HA Discovery (✅ / ✅ since the
2026-06-09 reconciliation above; its bar was compile + hassfest), OTA A/B with rollback safety
(the rollback note above) and Acoustic alarm-cadence detection (T3 smoke / T4 CO) (`FEATURES.md`'s
canary-wap acoustic note). The last two left Group B in sweep D23: the dashboard reads them ✅ for
both trees, and their §4 rows keep the "Bench-proven" cell open. The bench runbook's Track E is
the bad-image test; it has no acoustic track yet.

### Shared gap (neither tree, not a parity item but on the v1 path)
- **TLS (HTTPS self-signed): ⚠️ / ⚠️ in the dashboard** (canary (PIO) since #1704, the F15
  paragraph below; canary-wap since #1691). The `canary-wap` sketch's `httpd_ssl_start`-on-443
  path is a runtime state, not the build's posture: compile-gated on `esp_https_server.h`
  (`SECURACV_HAS_HTTPS_SERVER`), served after first-boot setup once `init_tls_cert()` loads or
  generates the device certificate (no stock core can generate one: the correction below), and
  plain HTTP during setup and again when the HTTPS server
  fails to start (logged) — `FEATURES.md`'s canary-wap HTTPS note. Neither tree has run it on
  hardware: proving it is the shared TLS gap (launch review §2) and Track D in the bench
  runbook — do it once and mirror into both trees.
- *2026-09 (F15, option (b) — maintainer to confirm):* the `canary (PIO)` tree now has the same
  shape — `httpd_ssl` on 443 serving every route, a port-80 server that keeps the six OS
  connectivity probes and 307-redirects the rest, TLS skipped during first-boot setup — with an
  on-device **ECDSA P-256** self-signed certificate (the WAP's CN / validity / serial / NVS
  layout; a keygen expected to be far shorter than RSA-2048's 30-60 s — untimed until Track D D1
  records the serial `[TLS] Certificate generation took … ms` line). `FEATURE_HTTPS=1` in `[env:dev]` and
  `[env:full]` only, so CI compiles it on IDF 4.4 and 5.5; release stays off until the
  size-guard log gives the slot delta. Never run on hardware: the dashboard cell is ⚠️, not ✅.
  Fallback if the dev leg shows the 2.0.17 core lacks x509write: option (c), certificate + key
  provisioned by the two flashers' NVS builders (no on-device keygen).
- *Correction 2026-10-22 (F15, F337):* neither tree has generated the certificate on a device.
  Both generators are gated on `__has_include("mbedtls/x509write_crt.h")`, a header mbedTLS has
  never shipped (its write API is in `mbedtls/x509_crt.h`, behind `MBEDTLS_X509_CRT_WRITE_C`;
  checked on arduino-esp32 2.0.17 and PlatformIO's framework-arduinoespressif32 3.3.8 and its
  libs), so every pinned core compiles it out, and nothing else stores a certificate. A stock
  WAP serves plain HTTP after setup, and the `canary (PIO)` `[env:dev]` / `[env:full]` builds
  serve HTTP and say so in `tls_mode_reason` ("no stored certificate and this core cannot
  generate one"). The CI compile above is real; the HTTPS it compiles does not run. Track D's D1
  records the HTTP-only reason until F15's guard is fixed.

---

## 2. The architectural reality that sets the effort

Parity is **not** copy-paste in either direction, because the two trees are structured oppositely:

- **ACTIVE is modular** (`firmware/canary/lib/securacv_*`, composed in `src/main.cpp` via
  `FEATURE_*` flags). Porting an Arduino capability INTO ACTIVE means **refactoring the monolithic
  logic into a `securacv_*` library** with a HAL-clean interface — higher up-front cost, but it's
  the long-term home (per `VARIANT_POLICY.md`, ACTIVE is the lane meant to eventually supersede
  COMPATIBILITY).
- **Arduino is one ~2,700-line sketch** under a binary-size budget (the firmware CI fails over the
  3 MB ceiling). Porting ACTIVE capabilities INTO it **grows the sketch** — watch the size gate,
  and prefer `#if FEATURE_*` gating so variants can drop weight.
- **The dashboard guard is a ratchet:** never let a ✅ cell regress; new work only moves cells
  ⚠️/❌ → ✅, and only once the capability is genuinely functional (compile-only ≠ ✅ for
  behavioral features — keep those ⚠️ until the bench runbook backs them).

> **Strategic note to keep visible:** maintaining two trees at parity is an **ongoing** cost, not
> a one-time push. Parity is the right v1 bridge (ship both, lose no capability), but the
> `VARIANT_POLICY` end-state is ACTIVE-supersedes-COMPATIBILITY. Worth revisiting post-v1 whether
> to converge rather than mirror forever.

---

## 3. Sequenced closure program

**Step 0 — Baseline & scorecard (pre-bench, safe, do first).**
- Re-audit the Group-A cells against current code (mesh is now *wired* in
  `main.cpp` — its ⚠️ reflects "unproven on hardware," not "absent"; don't pre-flip to ✅).
- Stand up the parity scorecard (§4) and link it from `FEATURES.md`.

**Tranche A1 — ACTIVE, compile-closeable (no hardware to land CI-green):**
Web UI, WiFi auto-reconnect-style robustness, and the mesh/chirp/RF/BLE *code presence* (build
under `[env:full]`). Each lands behind its `FEATURE_*` flag and must pass the PlatformIO +
host-test jobs. Behavioral cells stay ⚠️ pending Tranche-bench.

**Tranche B1 — Arduino, compile-closeable:**
MQTT publish + HA Discovery **(✅ already landed — `csi_mqtt.cpp`, see the 2026-06-09 note in §1)**,
WiFi auto-reconnect, sensing dashboard panel, capacitive touch / IR / temp-tamper / deep-sleep
scaffolding. Land behind `#if FEATURE_*`; verify the size gate and `hassfest`/`selftest-ui` jobs.

**Tranche A2 / B2 — bench-gated behavior:**
Everything marked "**bench**" above (mesh, RSSI, BLE discovery, RF presence, hub failover, chirp,
camera peek, acoustic, OTA rollback, power save). Land the code in A1/B1; **flip the dashboard
cell to ✅ only after the matching bench-runbook track passes** and an artifact is captured.

**Shared — TLS:** execute runbook Track D once; mirror the enablement into both trees.

---

## 4. Parity scorecard (fill as cells converge)

| Direction | Capability | Code landed (CI-green) | Bench-proven | Dashboard ✅ |
|---|---|:---:|:---:|:---:|
| →ACTIVE | Web UI dashboard | ☐ | n/a | ☐ |
| →ACTIVE | Camera peek (MJPEG) | ☐ | ☐ | ☐ |
| →ACTIVE | Mesh network | ☐ (wired) | ☐ (#610) | ☐ |
| →ACTIVE | Mesh RSSI | ☐ | ☐ | ☐ |
| →ACTIVE | BLE discovery | ☐ | ☐ | ☐ |
| →ACTIVE | RF presence | ☐ | ☐ | ☐ |
| →ACTIVE | Hub failover | ☐ | ☐ | ☐ |
| →ACTIVE | Chirp channel | ☐ | ☐ | ☐ |
| →ACTIVE | SD storage (`/HEALTH` writer) | ☐ (gap #17) | ☐ | ☐ |
| →ACTIVE | Bluetooth setup door | ☐ (sweep F251) | ☐ | ☐ |
| →ACTIVE | Witness export to `/EXPORT` | ☐ (sweep F476 decides first) | ☐ | ☐ |
| →ACTIVE | mDNS fleet advert (canonical TXT) | ☐ | ☐ | ☐ |
| →ACTIVE | Remote identify | ☐ | ☐ | ☐ |
| →ACTIVE | Provisioning gate | ☑ (CI-compiled, host-tested; sweep F354) | ☐ (U1) | ☐ |
| →Arduino | MQTT + HA Discovery | ☑ | n/a | ☑ |
| →Arduino | OTA A/B rollback | ☑ (shared engine; rollback note above) | ☐ | ☑ |
| →Arduino | Acoustic T3/T4 | ☑ (`securacv_audio`; `FEATURES.md`'s canary-wap acoustic note) | ☐ | ☑ |
| →Arduino | Capacitive touch | ☐ | ☐ | ☐ |
| →Arduino | Deep-sleep HAL | ☐ | ☐ | ☐ |
| →Arduino | IR appliance | ☐ | ☐ | ☐ |
| →Arduino | Internal temp tamper | ☐ | ☐ | ☐ |
| →Arduino | Sensing→chain signing | ☐ | ☐ | ☐ |
| →Arduino | Sensing HA discovery (11) | ☐ | ☐ | ☐ |
| →Arduino | Sensing dashboard panel | ☐ | n/a | ☐ |
| →Arduino | WiFi power save | ☐ | ☐ | ☐ |
| →Arduino | WiFi auto-reconnect | ☐ | n/a | ☐ |
| →Arduino | BLE Scout pairing surface | ☐ | ☐ | ☐ |
| →Arduino | SD endurance wear estimate | ☐ | n/a | ☐ |
| →Arduino | MQTT broker TLS (pin and lab modes) | ☐ (CA mode landed) | ☐ | ☐ |
| shared | TLS (HTTPS) enabled | ☐ ACTIVE: code in dev/full (2026-09, F15) — compiles, but no stock core generates the certificate (the 2026-10-22 correction above), so a green `firmware.yml` run does not tick it · ☐ WAP default builds | ☐ | ☐ |

**Exit:** every row ✅ in `FEATURES.md` for both `canary (PIO)` and `canary-wap (Arduino)` columns,
with the bench-gated rows backed by a runbook artifact.
