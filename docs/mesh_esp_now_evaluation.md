# ESP-NOW Mesh — Evaluation (what we have vs. what we need)

**Status:** Evaluation / roadmap
**Last updated:** 2026-05-28
**Companion docs:** [canary_mesh_network_v0.md](../spec/canary_mesh_network_v0.md) ·
[network_coexistence.md](network_coexistence.md) ·
[esp32_mesh_sensing_design.md](esp32_mesh_sensing_design.md) ·
[audit/mesh_and_chirp_audit_v1.md](audit/mesh_and_chirp_audit_v1.md) ·
[audit/hardware_verification_checklist.md](audit/hardware_verification_checklist.md) ·
[../firmware/canary/CONSOLIDATION.md](../firmware/canary/CONSOLIDATION.md) ·
[../firmware/FEATURES.md](../firmware/FEATURES.md)

## TL;DR

The ESP-NOW Opera mesh is **more complete than the `FEATURES.md` ⚠️ implies**. The
`securacv_mesh` library is fully implemented (no stubs), host-tested in CI, and wired into
the running `firmware/canary/` firmware. The honest gap is not the code — it is that the
mesh **ships disabled in every production build**, has **never been verified on hardware**,
and is **single-transport** (ESP-NOW only) versus the three-transport resilience the spec
promises. "Correct and full" means: verify on hardware → enable in production behind the
flash-encryption gate → build the spec's fallback transports → finish the adjacent ESP-NOW
riders (Chirp/RF) → persist hub failover.

---

## What we have ✅

All claims below were verified against the source tree.

### Library — complete, not stubbed
Every module in `firmware/canary/lib/securacv_mesh/src/` has a full `.cpp` body. There are
no header-only modules and no skeleton/TODO bodies:

| Module | LOC | What it does |
|---|---|---|
| `mesh_transport.cpp` | ~590 | ESP-NOW peer table, unicast/broadcast, RSSI, SPSC recv ring, aging FSM (ACTIVE→STALE@90s→OFFLINE@5m), storm limiter |
| `mesh_session.cpp` | ~692 | Pairing↔transport bridge, opera-authenticated dispatch, per-peer replay defense, beacon/channel/election handlers |
| `mesh_crypto.cpp` | ~587 | Ed25519, ChaCha20-Poly1305 AEAD, domain-separated SHA-256, fingerprint/opera_id, constant-time compare (IDF 4.x/5.x compat) |
| `mesh_pairing.cpp` | ~459 | 5-message handshake (DISCOVER→OFFER→ACCEPT→CONFIRM→COMPLETE), 6-digit confirmation code, AEAD-wrapped secret transfer |
| `mesh_state.cpp` | ~357 | NVS persistence for opera_secret / trusted peers / replay counters, **flash-encryption gated** |
| `mesh_envelope.cpp` | ~144 | Signed wire format: header + payload + Ed25519 signature, parse + verify |
| `mesh_beacon.cpp` | ~80 | BEACON_EVENT wire format (state + label) |
| `mesh_channel_hop.cpp` | ~90 | HopTracker: airtime >50% for 60s → hop 1→6→11, 120s cooldown |
| `mesh_hub_election.cpp` | ~74 | HubMonitor: hub-absent 60s → deterministic election (lowest fingerprint) |

### Tests — real, host-buildable, in CI
Nine suites (`test_mesh_*.cpp`, ~2,900 LOC total) exercise lifecycle, peer-table bounds,
aging, crypto vectors, the full pairing handshake, replay defense, channel-hop, and hub
election. Each builds natively (`g++ -std=c++17 -DCSI_TEST_HOST_BUILD ...`) and runs in CI —
none are skipped or placeholder.

### Integration — actually running (under the feature flag)
- `firmware/canary/src/main.cpp:542-546` — `mesh_transport::init/start` +
  `mesh_session::init/start`.
- `firmware/canary/src/main.cpp:1045-1046` — `mesh_transport::process()` +
  `mesh_session::process()` each loop iteration.
- Boot restores `opera_secret`, trusted peers, and replay counters from NVS
  (`main.cpp:558-623`); successful pairing persists them back (`main.cpp:148-234`).
- `firmware/canary/src/csi_modules_integration.cpp` wires BLE-Scout→BEACON_EVENT broadcast
  (MPSC FreeRTOS queue), plus CHANNEL_LOCK and HUB_ELECTION receive handlers.

### Security posture — v0.2 hardening closed in code
The three Opera audit findings are fixed in the library:
- **O1** — message freshness anchored on a per-peer monotonic 64-bit counter (not uptime).
- **O2** — `opera_secret` provisioning/load refuses without flash encryption, logs at
  `LOG_LEVEL_ALERT`.
- **O3** — `remove_peer()` performs a transactional `opera_secret` rekey with ACK-based
  finalization.

Channel coexistence is also complete: STA-following channel policy + a 2%-of-10s airtime
governor (`mesh_channel_policy`, `airtime_governor`), with HA MQTT telemetry.

### Wire-format parity
The modular library is byte-compatible with the proven Arduino reference
(`firmware/projects/canary-wap/arduino/canary_wap/mesh_network.cpp`).

---

## What we need ❌ / ⚠️

| # | Gap | State | Evidence |
|---|---|---|---|
| **G1** | **Ships disabled in production** | `FEATURE_MESH_NETWORK=0` in `dev`/`release`/`minimal` (and inheriting `dev_ha`/`release_ha`); `=1` only in `[env:full]` | `firmware/canary/platformio.ini` |
| **G2** | **No on-device hardware verification** | O1/O2/O3 + cross-reboot replay repro boxes all unchecked; `docs/audit/repro/{O1,O2,O3,replay}/` empty | `docs/audit/hardware_verification_checklist.md` |
| **G3** | **WiFi-AP-bridge fallback unbuilt** | Spec §2.2 transport #2; conformance marked "P — ESP-NOW only" | `spec/canary_mesh_network_v0.md`, `docs/audit/mesh_and_chirp_audit_v1.md` §6 |
| **G4** | **BLE fallback for heartbeats unbuilt** | Spec §2.2 transport #3; explicitly listed "not solved yet" | `docs/network_coexistence.md` |
| **G5** | **Per-device certs vs shared `opera_secret`** | "Stream C step 2"; not begun | `docs/network_coexistence.md` |
| **G6** | **Chirp channel body header-only** (shares ESP-NOW transport) | Deferred to Phase 4b | `CONSOLIDATION.md` gap #9 |
| **G7** | **RF presence detection header-only** | Deferred to Phase 4b | `CONSOLIDATION.md` gap #8 |
| **G8** | **Hub-failover NVS persistence not wired** | In-RAM election works; elected hub not persisted | `firmware/FEATURES.md` (hub failover ⚠️, canary PIO) |
| **G9** | **Stale comment** says `mesh_session::init()` is not called | It *is* called (`main.cpp:544`) | `csi_modules_integration.cpp:156-162` |
| **G10** | **"Add another Canary" pairing wizard** absent from HA add-on UI | Not implemented | `docs/network_coexistence.md` |

### Spec-conformance snapshot (`spec/canary_mesh_network_v0.md`)

| Spec area | Status |
|---|---|
| §2.2 ESP-NOW primary transport | ✅ implemented |
| §2.2 WiFi-AP bridge (secondary) | ❌ not implemented (G3) |
| §2.2 BLE fallback (tertiary) | ❌ not implemented (G4) |
| §3.1 Ed25519 device auth | ✅ |
| §3.2 X25519 + HKDF + ChaCha20-Poly1305 session | ✅ |
| §3.3 Replay (per-peer monotonic counter) | ✅ (O1 closed) |
| §4 message types (heartbeat/auth/alerts/peer-list) | ✅ |
| §5 pairing (6-digit confirmation) | ✅ |
| §5.5 flash-encryption gate | ✅ in code; ❌ unverified on hardware (G2) |
| §5.6 peer removal + rekey | ✅ in code; ❌ unverified on hardware (G2) |
| §8 REST API endpoints | ✅ (gated) |

---

## Roadmap to "correct and full"

Ordered by value × safety. Each item is a self-contained PR.

1. **PR-1 (G2) — On-device hardware verification.** Run O1/O2/O3 + cross-reboot replay
   repros on two `[env:full]` devices; capture artifacts into `docs/audit/repro/`; tick the
   checklist and update `docs/audit/v0.3_closeout.md`. **Blocks production enablement.**
2. **PR-2 (G1) — Production enablement.** After PR-1: flip `FEATURE_MESH_NETWORK=1` in
   production envs that satisfy the flash-encryption precondition; confirm the FE gate fails
   closed on non-FE boards; add a `regression_check.sh` assertion that mesh only enables with
   FE; advance `FEATURES.md` mesh row toward ✅.
3. **PR-3 (G8) — Hub-failover NVS persistence.** Persist elected-hub fingerprint via
   `mesh_state` (mirror `mesh_state.cpp:46-200`); restore on boot; add a host test.
4. **PR-4 (G3) — WiFi-AP-bridge fallback** (spec §2.2 #2) behind the `mesh_transport` send
   abstraction, gated by the airtime governor.
5. **PR-5 (G4) — BLE heartbeat fallback** (spec §2.2 #3): demote routine HEARTBEATs to BLE
   GATT under channel-conflict spikes; keep tamper/power/offline on ESP-NOW. Behind
   `FEATURE_BLE` (default off per CVE-2025-27840).
6. **PR-6 (G6) — Chirp channel body** — port `canary_wap/chirp_channel.cpp` into the modular
   lib (Phase 4b), carrying the C1–C17 audit fixes; host tests for signature/dedup/rate-limit.
7. **PR-7 (G7) — RF presence detection** — port `canary_wap/rf_presence.{h,cpp}` (Phase 4b).
8. **PR-8 (G10) — "Add another Canary" wizard** in the HA add-on, signed by an existing
   trustee Canary, over the spec §8 pairing endpoints.
9. **PR-9 (G5) — Per-device certificates** — replace the shared `opera_secret` trust model
   ("Stream C step 2"); design-first, likely its own spec revision.

> PRs 6/7 (Chirp/RF) are independent of 4/5 (fallback transports) and may be reordered by
> user priority.

---

## How to verify
- **Library/tests:** build any suite per its file header
  (`g++ -std=c++17 -DCSI_TEST_HOST_BUILD firmware/canary/lib/securacv_mesh/test_mesh_*.cpp`)
  and `pio run -e full` for the device build with mesh enabled.
- **Hardware (PR-1):** follow `docs/audit/hardware_verification_checklist.md`; artifacts land
  in `docs/audit/repro/`.
- **Regression gate:** `firmware/scripts/regression_check.sh` must stay green for every PR.
