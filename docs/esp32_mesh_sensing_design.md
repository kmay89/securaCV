# ESP32-S3 Mesh Sensing Design — WiFi CSI + BLE Scout + Single-Node

**Status:** Largely implemented — the proposed modules (multi-link fusion, empty-room calibration, sensing watchdog, channel-hop coordination) ship and are tracked in [`firmware/FEATURES.md`](../firmware/FEATURES.md); on-device hardware verification remains open (#610). Phasing aligned with [`firmware/canary/CONSOLIDATION.md`](../firmware/canary/CONSOLIDATION.md) Phase 4–5.
**Owner:** firmware maintainers
**Related:** [`spec/canary_free_signals_v0.md`](../spec/canary_free_signals_v0.md), [`spec/canary_mesh_network_v0.md`](../spec/canary_mesh_network_v0.md), [`docs/csi_modules.md`](csi_modules.md), [`docs/BLE_MESH_OPERA_TANDEM.md`](BLE_MESH_OPERA_TANDEM.md)

## Context

SecuraCV already ships a working ESP32-S3 single-node WiFi CSI pipeline ([`firmware/common/csi/`](../firmware/common/csi/)) with privacy-enforced presence, breathing, activity-ribbon, and anomaly modules, and a working ESP-NOW mesh transport (1540 LOC in `canary-wap`). The two have never been wired together. Today CSI quality depends on whatever ambient WiFi traffic happens to exist, the existing mesh code lives in the about-to-be-archived Arduino sketch instead of the modular canary build (Phase 4 gap in `firmware/canary/CONSOLIDATION.md`), and there is no answer for "what if I want to track my phone in a room" beyond the CSI heuristics. ESPresence and ESP-CSI-Tool show the cheap-hardware ceiling: ESP-NOW-driven active probing turns CSI from "best effort" into "deterministic 50 Hz" and is the single biggest reliability win available; BLE RSSI tracking of *known* beacons (phones, watches, tags) is a complementary, well-understood capability that fits naturally on a separate node.

This design delivers a reliable, useful 2–8-node mesh sensing v1 by porting and extending what already works, with no new outbound network surface and the existing privacy chokepoint kept intact.

## Architecture

### Node roles (chosen at provisioning, persisted in NVS)

ESP32-S3 has a single shared 2.4 GHz radio. Continuous CSI + continuous BLE scan corrupts breathing/motion detection through coex gaps. We sidestep the problem with role specialization:

| Role | Radio | Purpose | Power |
|---|---|---|---|
| **Sensor** (default) | WiFi only, BLE compile-disabled | Capture CSI + send 50 Hz ESP-NOW probes to peers | Mains (~100 mA) |
| **Scout** (optional) | BLE scan only, WiFi STA for mesh uplink only | ESPresense-style RSSI tracking of paired beacons | Mains; lower-duty BLE-only variant battery-feasible |
| **Hub** (>=1 required) | WiFi STA + ESP-NOW | Channel coordinator, evidence aggregator, dashboard host | Mains |

Hub is functionally a Sensor + coordinator and uses the same firmware image — role is selected at first boot.

### Mesh transport: ESP-NOW only

Reuses existing implementation at [`firmware/projects/canary-wap/arduino/canary_wap/mesh_network.cpp`](../firmware/projects/canary-wap/arduino/canary_wap/mesh_network.cpp). Ed25519 device auth, ChaCha20-Poly1305 payload encryption, 6-char pairing code, replay counters, max 8 peers, 250-byte fragmentation. We **port** (do not copy) this into a new `firmware/canary/lib/securacv_mesh/` lib per `CONSOLIDATION.md` Phase 4. Same channel as CSI by construction, sub-10 ms link latency, fail-secure (no outbound).

### The reliability lever: deterministic CSI via active probe

Today CSI capture quality is governed by whatever beacons + STA traffic happen to fly by. The new design makes every paired Sensor send ESP-NOW unicasts to every paired peer at a peer-count-aware effective rate. The fixed knob is an **aggregate cap** (default 200 Hz total Tx, comfortably under the 2 % airtime governor introduced in #442); the per-peer rate is `min(20 Hz, aggregate_cap / max(1, peer_count))`. With 1 peer that's 20 Hz; with 8 peers it's 25 Hz capped to 20 = 20 Hz per peer for 160 Hz aggregate. 20 Hz matches the existing CSI HAL rate-limiter ceiling so we never waste airtime on frames the receiver will drop. Each frame triggers a CSI callback on the receiver with a **known sender** on a **pinned channel**, so the 1 Hz feature window (20 frames target) becomes deterministic instead of best-effort. This alone is the single biggest accuracy and stability improvement available on this hardware.

Routine probe traffic is gated by `airtime_governor::try_reserve_routine()` (introduced in #442); urgent traffic (alerts) bypasses the cap via `force_reserve_urgent()`. The probe is routine.

### Channel pinning

At pairing, Hub scans 2.4 GHz channels 1/6/11, picks the cleanest, and broadcasts a `MSG_CHANNEL_LOCK` to peers. All nodes WiFi-STA to the Hub's WiFi (so beacons reinforce the lock — see #442's STA-following channel policy) and ESP-NOW probes go on that same channel. HT20 only — HT40 in 2.4 is too crowded and the second 20 MHz adds nothing over a typical home. Channel utilization is measured via Clear Channel Assessment (CCA) busy time exposed by `esp_wifi_sta_get_ap_info()` + the airtime governor's own counters; if CCA busy >50 % for 60 s, Hub proposes a coordinated hop. The hop itself is a state-mutating operation guarded by the same RTC mutex the channel-policy module already uses (#442), so an interrupt mid-hop cannot leave peers split across channels.

### Multi-link fusion

Each Sensor emits its 1 Hz `csi_features_t` over ESP-NOW to peers (32 bytes + small header — well under MTU). Hub maintains an n×n link table of recent feature vectors. New module `core.multilink_fusion` (in `firmware/common/csi/src/`) emits:

- **2-link confirmation gate**: only escalate to `confirmed` confidence if ≥2 independent links agree within a 3-window sliding overlap. Eliminates the single-link false-positive class (HVAC shimmer, neighbor WiFi spike).
- **Motion direction**: cross-product of per-link RSSI deltas gives a coarse "which side of the room" estimate without learning a room model.
- **Breathing fusion**: median BPM across links with agreement, drops the link with widest disagreement (rejects one bad link cleanly).

Single-link fallback: if only one link is healthy, the existing single-node modules keep working unchanged — there is no fusion regression risk.

### Empty-room auto-calibration

At pairing complete + every 24 h during a quiet-hours window (the `meta.quiet_hours` module already detects these): record a 10-min baseline of feature vectors per link, store the per-link mean+stddev in NVS, and subtract at runtime. Solves the "moves the couch and now everything trips" failure mode without learning a room model.

### BLE Scout (feature-flagged, OFF by default)

A separate role for dedicated nodes that **do not** do CSI. New lib `firmware/canary/lib/securacv_ble_scan/` gated behind `FEATURE_BLE_SCAN=1` (default 0). Per [`firmware/LESSONS_LEARNED.md`](../firmware/LESSONS_LEARNED.md) §"BLE adds proprietary binary blobs", BLE stays compile-time opt-in and CVE-2025-27840 is acknowledged in the user-facing role description.

- Passive scan only — never advertises (matches "no outbound" principle).
- Tracks user-paired beacons by hashed MAC (HMAC-SHA256 with a per-device key, never the raw MAC — same hygiene as the existing chain).
- Emits one event per beacon-arrived / beacon-left per room, rate-limited to 1/min per beacon.
- Kalman-filtered RSSI → coarse "room = this Scout's room" decision. No trilateration in v1 (multipath makes the math unreliable in homes).
- Mesh-broadcasts events to Hub for aggregation.

Out of scope for v1: BLE 5.1 AoA (ESP32-S3 doesn't support CTE), BLE Mesh transport, scanning of unknown devices (privacy + utility both lose).

### Privacy enforcement

The existing P0/P1/P2 chokepoint (`csi_event.cpp`, `csi_event_invariants_test.cpp`) gates every emission. We add three invariants:

1. Peer MAC never enters a witness chain payload (already true of CSI; extend to ESP-NOW receive path).
2. Beacon MAC is hashed before any persistence (Scout role only).
3. Per-link RSSI is bucketed to int8 before broadcast (same scaling as existing RSSI stats).

The host-build conformance test (`csi_event_invariants_test.cpp`) is extended with three assertions covering the above.

### Power

- **Sensor**: mains-only. ~100 mA continuous, dominated by radio-on for CSI. Document this clearly. The XIAO ESP32-S3 Sense reference board is USB-powered already.
- **Scout**: ~15–25 mA average with duty-cycled BLE (200 ms scan / 1.8 s sleep). Weeks on a small battery; mains preferred.
- **Hub**: mains. Adds the dashboard, channel coordination, multi-link fusion compute (~50 KB heap).
- No "battery-CSI" mode in v1. Documented limitation, not a feature gap.

### Error handling and failure modes

Every one of these is a real failure we will see, not a hypothetical:

| Failure | Detection | Recovery |
|---|---|---|
| 0 CSI frames for 5 s **(Sensor + Hub roles only — Scout has no CSI)** | New CSI-watchdog timer in `csi_hal` | Gentle: toggle `esp_wifi_set_csi(false/true)`. Escalation (full `esp_wifi_stop`/`start`) is the integration layer's call via the watchdog callback, guarded by the same single-flight mutex as `mesh_channel_policy` so concurrent recovery + channel hop cannot race. |
| AP roam / channel change | Beacon channel != pinned channel (uses #442's STA-following policy) | Pause probes; rejoin on new channel; rebaseline |
| DFS event (5 GHz neighbor, edge case) | RSSI cliff + frame drop spike | Same as channel-change |
| Peer churn (node power-cycled) | Heartbeat absent >30 s | Mark peer SUSPECT; expire at 5 min; fall back to single-link |
| Hub disappears | No `MSG_HEARTBEAT` from Hub for 60 s | Sensors continue logging locally to SD; promote a deterministic backup-Hub by lowest `device_id` among reachable peers (works in the 2-node case too — the surviving sensor with the lower id becomes Hub). |
| Multipath shimmer | RSSI swing >8 dB without spectrum-shaped Doppler | Reject as "non-human"; do not advance presence state |
| Mesh storm (broadcast loop) | `esp_now_send` callback rate exceeds `peer_count × probe_hz × 1.5` (i.e. 50 % over expected) | Rate-limit + log; pause for 30 s |

Hub failover is the only intentionally new state machine; the rest extend existing recovery paths.

## What changes — file by file

### New files
- `firmware/canary/lib/securacv_mesh/` — port from `firmware/projects/canary-wap/arduino/canary_wap/mesh_network.{h,cpp}`; add probe scheduler + role state. Library skeleton (`library.json`, `src/mesh_network.{h,cpp}`, `src/mesh_probe.{h,cpp}`). Phase 4 of `CONSOLIDATION.md`.
- `firmware/canary/lib/securacv_ble_scan/` — new, feature-flagged. Reuses NimBLE 2.x already pulled in by `[env:full]` in `platformio.ini`.
- `firmware/common/csi/src/core_multilink_fusion.{h,cpp}` — new module following the `csi_module.h` contract. Registers alongside `core_presence`, declares allowed fields, runs through the existing privacy chokepoint.
- `firmware/common/csi/src/csi_probe.{h,cpp}` — active-probe transmitter (50 Hz unicast ESP-NOW). Owned by the CSI lib so probe cadence is tied to feature-window cadence.
- `firmware/common/csi/tests/multilink_fusion_test.cpp` — host x86 test, same harness as existing `csi_event_invariants_test.cpp`.
- `firmware/common/network/tests/mesh_probe_test.cpp` — host test for the probe scheduler.

### Modified files
- `firmware/common/csi/src/csi_hal.{h,cpp}` — add `csi_set_channel_lock(uint8_t channel)`, a `csi_watchdog_tick()` hook, and the active-probe send entry point (a one-liner that calls into `csi_probe`).
- `firmware/common/csi/src/csi_types.h` — add `CSI_FIELD_LINK_ID`, `CSI_FIELD_FUSION_AGREE_COUNT` for the new module's allow-list; the int8 32-byte feature contract is unchanged.
- `firmware/projects/canary-wap/arduino/canary_wap/mesh_network.h` — add `mesh_set_role()`, `mesh_set_channel_lock()`, `mesh_broadcast_features()`, `MESH_MSG_CSI_FEATURES`, `MESH_MSG_CHANNEL_LOCK`, `MESH_MSG_BEACON_EVENT`. Backwards compatible (additive only).
- `firmware/canary/platformio.ini` — flip `FEATURE_MESH_NETWORK=1` in `[env:dev]` and `[env:release]`; add `FEATURE_BLE_SCAN=0` flag; add `securacv_mesh` and (gated) `securacv_ble_scan` to lib deps.
- `firmware/canary/sdkconfig.defaults` — leave `CONFIG_BT_ENABLED=n` as-is; the BLE Scout build sets the override in its own env in `platformio.ini` only.
- `firmware/canary/CONSOLIDATION.md` — expand Phase 4 to spell out the probe + fusion deliverables (currently only says "fill in stub bodies").
- `firmware/FEATURES.md` — add rows for "Active probe", "Multi-link fusion", "BLE beacon scout", with status flips per phase.
- `firmware/common/csi/csi_event_invariants_test.cpp` — extend with the three new privacy assertions. (File lives at the package root, not under `src/`.)

### Reused (no changes)
- `firmware/common/csi/src/csi_features.{h,cpp}` — 32-byte int8 vector is already the right interchange format for fusion.
- `firmware/common/csi/src/csi_event.{h,cpp}` — privacy chokepoint already enforces field allow-lists.
- `firmware/common/csi/src/csi_bundler.{h,cpp}` — 10-min sliding window collapse works identically for fused events.
- `firmware/common/csi/src/core_presence.cpp`, `core_breathing.cpp`, `core_activity_ribbon.cpp`, `anomaly_baseline.cpp`, `meta_quiet_hours.cpp` — single-link behavior unchanged; fusion module sits above them.
- ESP-NOW peer state machine, Ed25519 auth, ChaCha20-Poly1305 — all carry over from `mesh_network.cpp` port.

## Verification

End-to-end testing (in order):

1. **Single-node regression** — `pio run -e dev && pio test -e dev`. Existing CSI tests (`csi_event_invariants_test.cpp`, `csi_witness_roundtrip_test.cpp`) stay green. Boots, captures, emits events with no peers — fusion module is dormant.
2. **Two-node probe deterministic** — flash two Sensors, pair, point at empty room. Confirm `frames_in_window` is consistently >=45 (vs. previous ambient-dependent 5–20). 10-minute soak: zero CSI watchdog trips.
3. **Two-node motion** — walk through line-of-sight at 1 m/s. Expect `presence_changed` to `active` within 2 s on both nodes, fusion event `motion_confirmed` within 3 s.
4. **Two-node breathing** — seated subject, 3 m line-of-sight, 5-min run. BPM agreement within 2 BPM across links, fusion median stable within ±1 BPM after 60 s.
5. **Three-node geometry** — add Hub, place in triangle. Walk the perimeter. Per-link RSSI deltas should sketch the path direction in the Hub log within ~5 dB resolution.
6. **CSI watchdog** — kill peer's probe (yank power). Surviving node detects, restarts WiFi, recovers <10 s. Logged to health chain.
7. **Channel hop** — saturate the pinned channel with a separate AP transmitter. Hub proposes hop; all peers follow; CSI resumes <30 s.
8. **Hub failover** — power-cycle Hub. Two Sensors agree on backup-Hub election within 60 s; logs continue locally.
9. **BLE Scout (only with `[env:full]` build)** — pair a phone as a known beacon, walk between two Scout rooms. Room-attribution event within 5 s of stable-RSSI threshold.
10. **Privacy conformance** — runnable from the repo root:
    ```bash
    g++ -std=c++17 -DCSI_TEST_HOST_BUILD \
        firmware/common/csi/csi_event_invariants_test.cpp \
        firmware/common/csi/src/csi_event.cpp \
        firmware/common/csi/src/csi_module.cpp \
        firmware/common/csi/src/csi_bundler.cpp \
        firmware/common/csi/src/csi_witness_payload.cpp \
        firmware/common/csi/src/ble_events_module.cpp \
        firmware/common/csi/src/meta_quiet_hours.cpp \
        -I firmware/common/csi/src -o /tmp/csi_invariants && /tmp/csi_invariants
    ```
    Passes including the three new assertions: (a) no peer MAC in any persisted CSI feature payload, (b) Scout beacon MAC is hashed before any event emission, (c) per-link RSSI is bucketed int8 before mesh broadcast.

A successful v1 is: tests 1–8 reliable across three separate room geometries, and `regression_check.sh` stays green.

## Out of scope for this design

- BLE Mesh transport (use ESP-NOW)
- BLE 5.1 AoA / CTE (hardware doesn't support)
- 5 GHz operation (chip is 2.4 GHz only)
- Battery-powered Sensor role (radio-on cost makes this dishonest)
- >8-node deployments (existing peer table cap; revisit in v2)
- Cloud uplink (architectural principle; no change)
- Camera/CV fusion (separate roadmap)
- Learned room models / neural CSI classifiers (defer until 2–8 nodes work reliably with the rule-based fusion)

## Phasing for review-sized PRs

1. **PR 1: probe + channel lock** — `csi_probe`, `csi_set_channel_lock`, CSI watchdog. Single-node test rig benefits immediately.
2. **PR 2: mesh port** — `securacv_mesh` lib, equivalent to `CONSOLIDATION.md` Phase 4, no new behavior.
3. **PR 3: feature broadcast + fusion** — `core_multilink_fusion`, `MESH_MSG_CSI_FEATURES`, 2-link confirmation gate.
4. **PR 4: Hub coordination** — channel-hop proposal, Hub failover election, empty-room baseline calibration.
5. **PR 5: BLE Scout** — `securacv_ble_scan`, `[env:scout]`, beacon-event mesh broadcast. Feature-flag default OFF.
6. **PR 6: privacy conformance** — extended invariants test, `FEATURES.md` flip, dashboard surface for the new event types.

Each PR is independently revertable and ships with its own tests.
