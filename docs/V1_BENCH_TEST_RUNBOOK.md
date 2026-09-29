# SecuraCV — v1 On-Device Bench-Test Runbook

> **Purpose:** the single driver sheet for the one remaining v1 gate — *on-device hardware
> validation*. Everything documented is already green in CI; this runbook takes it onto real
> hardware. It **sequences and references** the detailed procedures that already live in the repo
> rather than duplicating them — chiefly
> [`hardware/v1_bench_validation_runbook.md`](hardware/v1_bench_validation_runbook.md), whose
> per-track pass criteria and artifact lists apply to Tracks A–C here — and gives you one
> results matrix to sign off.
>
> **Companion:** [`V1_LAUNCH_REVIEW.md`](V1_LAUNCH_REVIEW.md) (the why) · this doc is the how.
> **Closes:** the "on-device validation pending" blocker in `v1-roadmap.md` and issue **#610**.

## How to use this
Work the five tracks top to bottom. Each step lists the **command/reference**, the **expected
result**, and the **artifact** to capture. File artifacts under `docs/audit/repro/<track>/` (the
dirs #610 expects to be filled). When every row in the §5 matrix is ✅ with an artifact, flip the
README badge `v1-rc → v1.0`, date the `CHANGELOG.md [1.0.0]` entry, and bump the crate version
in `Cargo.toml` to `1.0.0` (the tag step).

---

## 0. Pre-flight — what you need

| Item | Qty | Notes |
|---|---|---|
| XIAO ESP32-S3 (Sense) | **≥ 3** | 3 boards are needed for the Opera mesh / Beacon three-board repro; 2 minimum for Chirp |
| Boards with **flash encryption** enabled | ≥ 2 | Required for mesh (the `opera_secret` in NVS is only confidential under FE) |
| USB-C cables, powered hub | — | For simultaneous multi-board flashing + serial |
| Raspberry Pi 4/5 or x86 box w/ Home Assistant OS | 1 | Operator stack host (Track A) |
| RTSP IP camera (or the committed mp4 fixture over RTSP) | 1 | Track A |
| Host tooling | — | `pio` (PlatformIO), `idf.py` (for FE provisioning), `python3`, `mosquitto-clients`, `docker`/`docker-compose` |

Serial monitor at **115200 baud**. Keep a notebook (or `tee` the serial logs) — the serial
console is the primary artifact source for the firmware tracks.

---

## Track A — Operator pipeline (kernel ↔ Frigate ↔ Home Assistant)

Proves the flagship path end-to-end on real infrastructure. (CI already proves the bridge + log
+ verify hermetically; this is the live-stack confirmation.)

| # | Step | Command / reference | Expected | Artifact |
|---|---|---|---|---|
| A1 | Stand up the 4-container stack | `integrations/ha_frigate_mqtt/` `docker compose up -d` (see `docs/integrations/home-assistant-frigate-mqtt.md`) | Mosquitto + Frigate + HA + SecuraCV all healthy | `docker compose ps` output |
| A2 | Operator smoke check | `integrations/ha_frigate_mqtt/verify_pipeline.sh` | Exit `0`; Frigate publishes, `frigate_bridge` ingests ≥1 event, encrypted `witness.db` non-empty | script stdout |
| A3 | Verify the live log cryptographically | `cargo run --bin log_verify -- --db <witness.db>` (with the deployment's DB key seed) | Hash chain + Ed25519 signatures **valid** | verify output |
| A4 | Real RTSP camera (not the fixture) | point Frigate at a live camera; trigger a detection | Event appears in HA as a **verified ✓** witness row | HA screenshot |
| A5 | Tamper proof, live | mutate one row in the DB, re-run `log_verify` | Verification **fails** at the tampered link | before/after output |

> A4's HA screenshot is also the **"show, don't tell"** marketing asset the launch review calls
> for — capture a clean one.

---

## Track B — Canary device bring-up (single, then multi)

> First time touching the hardware? Do the parts/peripheral smoke in
> [`hardware/bench_bringup.md`](hardware/bench_bringup.md) (board + buzzer + LED + BOOT button)
> before this track — it proves the physical chirp/LED/button work in ~60 seconds. This track
> picks up at firmware bring-up and assumes a board that already powers on.

| # | Step | Command / reference | Expected | Artifact |
|---|---|---|---|---|
| B1 | Confirm virgin device | `firmware/provisioning/verify_device.py --port /dev/ttyACM0 --expect-virgin` | eFuses report unprovisioned | tool output |
| B2 | Flash dev firmware | `cd firmware/canary && pio run -e dev -t upload` | Build + flash OK; auto-provisions keys on first boot | serial: device fingerprint |
| B3 | First-boot onboarding | follow `docs/getting_started_canary.md` (join `SecuraCV-XXXX` AP → `http://canary.local`) | Captive portal → dashboard; joins home WiFi | dashboard screenshot |
| B4 | Witness chain on-device | trigger an event; open the dashboard chain view; `GET /api/v1/witness/verify` | Chain links + signatures valid; no gaps | verify JSON |
| B5 | Privacy invariants on the wire | inspect MQTT `securacv/<id>/#` + API payloads | **No raw MAC**, **no >3 dp GPS**; pseudonymous `device_id` only | captured payloads |
| B6 | Multi-canary + Identify | flash board #2; `docs/onboarding_multiple_canaries.md` | Unique `canary-<name>.local`; Identify blinks/chirps the right board | short video |
| B7 | HA MQTT discovery | with the HA integration installed | Both Canaries auto-discover; sensors + signature verification populate | HA devices screenshot |

---

## Track C — Mesh + Chirp + Beacon (the "canaries talk without HA" proof) — closes #610

Mesh persists its `opera_secret` only on **flash-encryption-enabled** boards built from
`[env:full]`. The static guard added this cycle (`firmware/scripts/regression_check.sh` → "Mesh
secret persistence is FE-gated") asserts the FE check stays in the persistence layer; this track
proves on hardware that **no mesh secret lands in unencrypted NVS**.

> **As-built nuance / open design question.** On an FE-off board the persistence layer refuses to
> write the secret, but `on_pairing_succeeded` (`firmware/canary/src/main.cpp`) **deliberately
> still runs the live session in RAM for the current boot** (it just won't survive a reboot). So
> "fail-closed" today means *no secret persisted*, **not** *no live session*. Whether live
> activation should also refuse on FE-off boards is an open #610 decision for the maintainers — C2
> below verifies the as-built behavior, not an assumed hard refusal.

| # | Step | Command / reference | Expected | Artifact → `docs/audit/repro/` |
|---|---|---|---|---|
| C0 | Production-provision FE boards | `firmware/provisioning/` → `./generate_keys.sh` then `./provision_canary.sh --port … --phase 2 --dry-run` then without `--dry-run` (uses `platformio_secure.ini` / `sdkconfig.defaults.secure` / `partitions_secure.csv`) | Secure Boot v2 + Flash Encryption burned; `verify_device.py` confirms | `O2/` |
| C1 | Build the mesh image | `pio run -e full -t upload` on ≥3 FE boards | `[OK] Mesh layer active (mesh_transport + mesh_session)` in serial | `O3/` setup log |
| C2 | **FE secret-at-rest gate** | pair on a **non-FE** board, then dump NVS | serial shows `active for this boot but NVS persist failed`; **NVS contains no `opera_secret`** (O2); session does **not** survive reboot | `O2/` (NVS dump + serial) |
| C3 | Chirp v0.2 two-device repro | `docs/audit/hardware_verification_checklist.md` → *Chirp v0.2* | findings reproduce as documented | `chirp/` |
| C4 | Opera mesh v0.2 three-board repro (O1/O2/O3) | same checklist → *Opera mesh v0.2* | O1 counter freshness, O2 FE-gated provisioning, O3 transactional rekey all pass | `O1/ O2/ O3/` |
| C5 | Beacon channel v0 three-board repro | same checklist → *Beacon channel v0* | beacon discovery / tamper-revoke as documented | `beacon/` |
| C6 | Cross-reboot **replay** defense | capture a valid frame → reboot receiver → replay (SNTP-synced) | replayed frame **rejected** | `replay/` |
| C7 | Non-impersonation contract | checklist → *Non-impersonation contract* | holds on-device | `repro/` |
| C8 | Close out | tick `docs/audit/v0.3_closeout.md` §"How to verify on-device"; advance `firmware/FEATURES.md` mesh row to ✅; flip `FEATURE_MESH_NETWORK=1` only in the FE-meeting prod env | #610 checkboxes complete | updated docs |

---

## Track D — Firmware TLS handshake (HTTPS:443)

`canary-wap` starts HTTPS on **443** (`httpd_ssl_start`) with an HTTP→443 redirect when
`SECURACV_HAS_HTTPS_SERVER` (ESP-IDF `CONFIG_ESP_HTTPS_SERVER_ENABLE`) and a provisioned cert are
present. Confirm the v1 image actually runs it.

| # | Step | Expected | Artifact |
|---|---|---|---|
| D1 | Build the v1 image with the HTTPS config enabled + a provisioned/generated cert. `canary (PIO)`: `pio run -e dev` or `-e full` (`FEATURE_HTTPS=1`; the first TLS boot generates an ECDSA P-256 certificate, serial `[TLS] Certificate generation took … ms` then `[TLS] Cert fingerprint: …` — record the ms figure, the docs carry no measured keygen time yet); `release` does not enable it yet | serial: `[HTTPS] Server started on port 443`; `GET /api/status` → `tls_enabled: true` (else `tls_mode_reason` says why) | serial log |
| D2 | `curl -k https://canary-<name>.local/api/v1/info` | 200 with the API payload over TLS | curl output |
| D3 | `curl -I http://canary-<name>.local/` | redirect to HTTPS (captive portal exempt) | curl output |
| D4 | Compare presented cert fingerprint to the serial-logged `Cert fingerprint` | match | both values |
| D5 | `canary (PIO)` only: open the dashboard over `https://`, start the camera peek | MJPEG plays over TLS (it streams in the handler, so other TLS requests wait while it runs — note how the dashboard feels) | screen recording or notes |

---

## Track E — Boot recovery (A/B rollback, crash-loop safe mode)

The one track where *compile-tested* is not an acceptable resting state: these
features exist to behave correctly when the firmware is broken, so the test is
to break it on purpose. `canary (PIO)` on a XIAO ESP32-S3; the bad image comes
from the bench-only `SCV_BENCH_CRASH_AFTER_MS` flag (no env sets it), which
panics that many ms into `loop()` — before the 30 s healthy gate. Background:
`firmware_ota.md` ("Where each property holds today") and
`firmware/common/health/boot_policy.h`.

| # | Step | Expected | Artifact → `docs/audit/repro/boot/` |
|---|---|---|---|
| E1 | Flash a good `dev` image over USB; watch serial for 40 s | `Boot attempt 1 of 4 before safe mode`, then `Boot healthy (loop stable)` about 30 s after `WITNESS DEVICE READY` | serial log |
| E2 | Build a bad image: `PLATFORMIO_BUILD_FLAGS="-DSCV_BENCH_CRASH_AFTER_MS=5000" pio run -e dev`. Install it **over OTA** onto the E1 board — the dev push endpoint, or a signed pull manifest (which needs a version above E1's, or the anti-rollback floor refuses it) | New image boots `(new image, pending confirmation)`, panics at ~5 s; the **next** boot is the E1 image again; after its healthy gate, `fw_update_rolled_back` is in the witness chain | serial log + chain excerpt |
| E3 | Same bad image, but flash it **over USB** (a confirmed image — nothing to roll back to) | Boots 1–3 panic; boot 4 prints the `SAFE MODE` card; Wi-Fi AP never comes up | serial log |
| E4 | In safe mode: `c`, then `n` | `Canceled.`; still in safe mode | serial log |
| E5 | In safe mode: hold BOOT 2 s | `Counter cleared. Restarting normally...`; boots normally (then crash-loops again, since the image is still bad) | serial log |
| E6 | Flash the good E1 build over USB while in safe mode | Boots normally at `Boot attempt 1` — a different build starts the count over | serial log |
| E7 | On the good image, pull power within 10 s of boot, four boots in a row | Safe mode on the 4th boot; documents that the counter counts power cuts, which is intended (a brownout loop is a crash loop) | serial log |

---

## 5. Sign-off matrix (the v1 gate)

| Track | Item | Status | Artifact |
|---|---|:---:|---|
| A | Live operator pipeline + live verify + live tamper | ☐ | |
| A | Verified-✓ timeline screenshot (marketing asset) | ☐ | |
| B | Single Canary bring-up + on-device chain verify | ☐ | |
| B | Privacy on the wire (no MAC / no fine GPS) | ☐ | |
| B | Multi-canary + Identify + HA discovery | ☐ | |
| C | Mesh active on FE boards; **fail-closed on non-FE** | ☐ | |
| C | #610: O1 / O2 / O3 / replay / non-impersonation | ☐ | |
| C | `v0.3_closeout.md` ticked; FEATURES.md mesh ✅ | ☐ | |
| D | HTTPS:443 + redirect + cert match | ☐ | |
| E | Bad OTA image reverts to the previous image (E2) | ☐ | |
| E | Confirmed crash loop stops in safe mode; every escape works (E3–E6) | ☐ | |

**Exit criteria → tag v1.0:** every row ✅ with an artifact, all CI checks green, then run the
tag step (badge `v1-rc → v1.0`, `CHANGELOG [1.0.0]` dated, crate version → `1.0.0`).
Anything that fails on hardware comes back as a code fix (CI-gated) before re-running its track —
the loop has a terminal state and this matrix is how you drive it there.
