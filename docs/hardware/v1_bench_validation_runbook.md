# v1 On-Device Bench Validation Runbook

> **⚠️ Not the driver sheet.** The document that sequences the whole v1 gate —
> tracks A–D, the sign-off matrix, and the tag step — is
> [`docs/V1_BENCH_TEST_RUNBOOK.md`](../V1_BENCH_TEST_RUNBOOK.md). Work from
> that one. This file stays as the **detailed per-track procedures and pass
> criteria** the driver sheet draws on (single-board bring-up, kernel operator
> smoke, 2–3 board fleet validation); earlier versions of this file called
> itself "the procedure that closes the last v1 gate," and that claim now
> lives in the driver sheet.

This runbook details the bench procedures for the v1 gate: **on-device hardware
validation** (README "Release gate", `v1-roadmap.md` "Still open"). CI proves the
code compiles and the pipelines work against fixtures; this runbook proves the same
behavior on real boards. It has three tracks:

- **Track A** — single Canary WAP: flash → provision → signed MQTT → verified-✓ in Home Assistant.
- **Track B** — kernel pipeline operator smoke (the existing 4-container manual gate).
- **Track C** — multi-canary fleet: Opera mesh pairing + Chirp exchange on 2–3 boards.

Per `firmware/PARITY_PLAN.md` (§ bench evidence), a `FEATURES.md` cell flips to ✅
**only after the matching track below passes and an artifact is attached**. Capture
artifacts as you go (see *Artifacts* at the end of each track).

Related docs: [bench bring-up (chirp/LED/button)](./bench_bringup.md) ·
[getting started](../getting_started_canary.md) ·
[ESP32-S3 toolchain](../esp32_s3_setup.md) ·
[multiple canaries](../onboarding_multiple_canaries.md) ·
[HA setup](../homeassistant_setup.md)

---

## Hardware needed

| Track | Boards | Extras |
|---|---|---|
| A | 1× Seeed XIAO ESP32-S3 Sense | microSD (high-endurance, FAT32), USB-C **data** cable; optional L76K GPS, passive buzzer |
| B | none (any Docker host) | RTSP camera or the committed mp4 fixture |
| C | 2–3× XIAO ESP32-S3 Sense | same as A per board |

A Home Assistant instance with the Mosquitto broker and the SecuraCV HACS
integration installed (Tracks A and C).

---

## Track A — single Canary WAP → HA verified-✓

**Goal:** prove the chain *flash → provision → signed event → TOFU pin → ✓ "Signature
verified" badge* on hardware, plus reboot/backfill and tamper-negative behavior.

### A1. Flash

1. Toolchain per [`esp32_s3_setup.md`](../esp32_s3_setup.md). The Arduino tree
   (`firmware/projects/canary-wap/arduino/canary_wap/`) is the source of truth;
   NimBLE-Arduino **2.x** is required (the build hard-fails on 1.x by design).
2. Select **`BUILD_PROFILE_DEV`** or **`FULL`** in `build_config.h` (MINIMAL omits
   the chirp and several test routes).
3. Build + upload; open the serial monitor at boot.

**Pass:** clean boot log; Ed25519 device key generated/persisted (health log
`crypto` entries); no watchdog resets over 10 minutes idle.

### A2. First-run provisioning

1. The device raises its AP `SecuraCV-XXXX` with a **device-unique** password
   (printed on serial at first boot — confirm it is *not* a fixed default;
   `regression_check.sh` fails on the legacy `witness2026`).
2. Join from a phone; the captive portal (`setup_wizard.h`) should redirect any
   hostname to `192.168.4.1`.
3. Enter home-WiFi credentials (or use BLE provisioning). Record the `device_id`
   (e.g. `canary-s3-AABB`) and the pubkey fingerprint from the device's `/enroll`
   page.

**Pass:** device joins WiFi STA; dashboard reachable at `canary-<name>.local`;
`/enroll` shows a 64-hex pubkey; `GET /api/selftest` reports all green.

### A3. Local witness chain

1. Let the device run ≥15 minutes with some motion in range (CSI presence).
2. Export the chain (dashboard export, or the SD `witness/` logs).
3. Verify: signatures + hash links check out (use the export-verify path the
   dashboard offers; spot-check that GPS fields, if present, are 3-decimal
   coarsened and no raw MAC appears anywhere in the export).

**Pass:** non-empty chain, verification passes, privacy fields coarse.

### A4. MQTT → Home Assistant

1. Point the device at HA's broker: `POST /api/mqtt/config`, then `/api/mqtt/test`.
2. In HA: the SecuraCV integration (MQTT mode) should discover the device on its
   first `securacv/<device_id>/status` publish and **TOFU-pin** its pubkey
   (Settings → Devices & Services → SecuraCV → Configure shows the pin).
3. Trigger presence events; watch `securacv/<device_id>/events`.

**Pass:** entities appear for the device; the timeline card / event sensors show
**✓ "Signature verified"** (not "Signed (unverified)") — this proves the firmware's
`securacv-canary-sig` canonical signing and HA's `signature.py` verifier agree on
real hardware.

### A5. Resilience negatives (all must behave, not just the happy path)

| Test | Expected |
|---|---|
| Reboot the device mid-session | On reconnect, `csi_mqtt` backfills missed events from the SD event log (no gap in event_ids in HA) |
| Pull power abruptly | `power_loss` tamper path: last-gasp/early-boot tamper event after restart |
| Publish a doctored payload from a laptop using the device's topics (wrong key) | HA marks **"Verification failed"** / trust mismatch notification — never a silent ✓ |
| Pull the SD card while running | `sd_remove` tamper event over MQTT |
| Stale wall clock (cold boot, no NTP yet) | Early events marked/queued per firmware policy; no crash |

### Artifacts (attach to the PARITY_PLAN evidence table)
Serial boot log, `/api/selftest` JSON, exported chain + verify output, HA
screenshots of the TOFU pin and a ✓-verified event row, and the failed-verify
screenshot from the doctored-payload test.

---

## Track B — kernel pipeline operator smoke (existing gate)

Run the documented manual gate against the live 4-container stack:

```bash
cd integrations/ha_frigate_mqtt
./verify_pipeline.sh   # must exit 0
```

See [`docs/integrations/home-assistant-frigate-mqtt.md`](../integrations/home-assistant-frigate-mqtt.md).
With a real RTSP camera configured in Frigate, additionally confirm one
camera-originated event lands in the sealed log and `log_verify` passes against
the encrypted DB.

**Pass:** `verify_pipeline.sh` exits 0; `log_verify` green on the produced DB.

**Artifacts:** script output, `log_verify` output.

---

## Track C — multi-canary fleet (2–3 boards)

> 🖨️ Flashing several boards in sequence? The printable
> [provisioning dock](./enclosure/canary_dock.scad) holds a numbered row of
> XIAOs USB-up next to a hub, so board ↔ identity bookkeeping stays straight.

**Goal:** move Opera mesh + Chirp from "code-complete, bench-gated ⚠️" to ✅.
Prerequisite: each board individually passes Track A steps A1–A3.

### C1. Flash-encryption gate (security precondition)

On a board **without** flash encryption enabled, attempt mesh provisioning.

**Pass:** the device **refuses** to provision the opera secret (audit O2 behavior).
Then enable flash encryption on all fleet boards and proceed.

### C2. Opera pairing

1. Board 1: `POST /api/mesh/enable`, then `/api/mesh/pair/start`.
2. Board 2: `/api/mesh/pair/join`; confirm on both (`/api/mesh/pair/confirm`).
3. `GET /api/mesh/peers` on each — both list the other with fresh heartbeats.

**Pass:** pairing completes (Ed25519 challenge-response + X25519 session per
`spec/canary_mesh_network_v0.md`); peers show *online*; heartbeats keep peers
fresh for ≥30 minutes; pulling one board's power flips it to *stale* → *offline*
on the survivor within the spec thresholds (90 s / 300 s).

### C3. Mesh behavior

| Test | Expected |
|---|---|
| Witness/alert propagation | Event on board 1 raises a mesh alert on board 2 (`/api/mesh/alerts`) |
| Peer removal | `/api/mesh/remove` on board 1 → opera secret rotates; removed board can no longer rejoin without re-pairing (audit O3) |
| Tamper auto-revoke | Tamper alert from a paired member → survivor marks it `REVOKED` (v0.5 behavior) |
| Third board joins | 3-node opera stable; no crosstalk with a second, separately-paired opera (opera_id isolation) |

### C4. Chirp exchange

1. With ≥2 boards in radio range (chirp does not require opera membership),
   originate a templated chirp on board 1.
2. Confirm board 2 receives, displays the 5-emoji session identity, and its ACK
   reaches board 1 (unique-pubkey confirmation count increments once).

**Pass:** signed end-to-end (C1 audit); replayed packet is deduped (nonce cache);
a flood from one session hits the per-pubkey rate limit without starving
EMERGENCY/WEATHER priority storage; wall-clock-less board refuses origination.

### C5. Fleet → HA

With all boards on MQTT (Track A4 each): HA shows one device per `device_id`,
each with its own TOFU pin, and per-device timelines stay independent and
verified.

### Artifacts
`/api/mesh/peers` JSON from each board at each stage, serial logs of the
pairing ceremony and secret rotation, chirp send/receive serial excerpts,
HA multi-device screenshot. On success, update the `FEATURES.md` mesh/chirp
cells and the PARITY_PLAN evidence table in the same PR.

---

## Exit criteria (v1 tag)

All three tracks pass with artifacts attached. Then, per the README release
gate: flip the README status from `v1-rc`, move `CHANGELOG.md` `[1.0.0]` out of
*Unreleased*, and cut the tag.
