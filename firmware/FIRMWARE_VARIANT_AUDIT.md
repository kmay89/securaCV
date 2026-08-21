# Firmware Variant Audit (Canary Family)

**Last updated:** 2026-08-20 (refreshed canary-vision + canary-sense onboarding readiness: both now raise the shared setup portal from `firmware/common/network/setup_portal` when unprovisioned or recovery-stuck, alongside flasher NVS credential seeding)
**Original audit date:** 2026-03-07
**Scope:** per-variant risk analysis. For lifecycle labels see [VARIANT_POLICY.md](VARIANT_POLICY.md). For per-feature parity see [FEATURES.md](FEATURES.md).

Purpose: identify which firmware path is currently most active and most user-friendly (especially for onboarding/UI), and flag variants at risk of rot.

## Executive Summary

- **Canonical / ACTIVE path:** [`firmware/canary`](./canary)
  - Modular architecture, active recent commits, integrated AP + onboarding APIs (`/api/wifi/*`, `/api/mqtt/*`), and modern dashboard UI.
- **COMPATIBILITY (Arduino-first) path:** [`firmware/projects/canary-wap/arduino/canary_wap`](./projects/canary-wap/arduino/canary_wap)
  - Functionally rich and very UI-heavy, but monolithic and high maintenance cost.
- **REMOVED:** `firmware/projects/_archive/canary-wap-snapshot` (deleted 2026-05-29)
  - Archived 2026-02-20, then removed from the tree on 2026-05-29; history remains in git. Security fixes (removed fallback AP password, `SECURACV_RELEASE_BUILD` guards) had been backported before archival, so no residual liability remained at deletion.
- **SPECIALIZED tracks (not primary WAP UX path):**
  - [`firmware/projects/canary-vision`](./projects/canary-vision) (ESP32-C3 + Vision AI, MQTT-focused)
  - [`firmware/projects/canary-sense`](./projects/canary-sense) (XIAO ESP32-C6 + MR60BHA2 radar, MQTT-focused)
  - [`firmware/projects/canary-ota`](./projects/canary-ota) (OTA A/B subsystem)

---

## Comparison Matrix

| Firmware path | Lifecycle | Primary role | Last activity signal | On-device UI friendliness | Onboarding readiness | Rot risk |
|---|---|---|---|---|---|---|
| `firmware/canary` | ACTIVE | Main ESP32-S3 witness firmware (modular libs) | Recent feature work in 2026-02 for MQTT + WiFi STA, plus storage counter implementation 2026-04 | **High** (modern dashboard in `securacv_webui`) | **High** (WiFi scan/connect + MQTT config HTTP APIs) | Low |
| `firmware/projects/canary-wap` | COMPATIBILITY | Arduino-IDE-first WAP sketch (PlatformIO + Arduino) | Security hardening + real ESP-NOW RSSI landed 2026-04 | High (large embedded dashboard) | Medium/High (rich feature set, heavier maintenance) | Medium |
| `firmware/projects/_archive/canary-wap-snapshot` _(removed 2026-05-29)_ | REMOVED | Frozen reference of WAP sketch | Archived 2026-02-20, deleted 2026-05-29 | n/a | n/a | **Removed** (history in git) |
| `firmware/projects/canary-vision` | SPECIALIZED | Vision events + MQTT/HA integration | 2026-07: S3-tree robustness parity (supervised WiFi STA, heap degradation, HA diagnostics) + mDNS fleet advert + HA identify button | Low (no primary local onboarding UI focus) | Medium/High (flasher NVS seeding + the shared setup portal — `SecuraCV-XXXX` SoftAP + captive join wizard when unprovisioned or a saved join keeps failing fixably; no full local dashboard) | Low/Medium (actively tracked to the S3 tree) |
| `firmware/projects/canary-sense` | SPECIALIZED | 60GHz mmWave radar presence witness (MR60BHA2 / XIAO ESP32-C6): signed witness chain + MQTT/HA integration | New 2026-07 (Phase 2: full net/witness/OTA stack + mDNS fleet advert; compile/CI-verified, bench validation pending) | Low (no local onboarding UI by design) | Medium/High (HA discovery + identify button; flasher NVS seeding + the shared setup portal for the no-credentials and recovery cases — sensing continues under it) | Medium until bench-validated |
| `firmware/projects/canary-ota` | SPECIALIZED | OTA framework and rollback safety | Active in 2026-02 | N/A (not a WAP UX product) | N/A | Low/Medium |
| `firmware/projects/canary-display` | SPECIALIZED | Fleet status displays (watch puck + 4.3" dash): MQTT subscribe, TOFU + Ed25519 chain verify, glance UI | New 2026-07 (v0.1, compile/CI-verified; bench validation pending) | **High** (the product IS an on-device UI) | Low (flashes with compiled secrets; provisioning parity on roadmap) | Medium until bench-validated |
| `firmware/common` + `firmware/configs` + `firmware/boards` | shared | Shared platform layers | Mixed cadence | N/A | N/A | Low |

---

## Evidence and Observations

### 1) `firmware/canary` is the ACTIVE primary path

Why:
- Modularized into dedicated libraries (`securacv_network`, `securacv_webui`, `securacv_mqtt`, `securacv_storage`, etc.) for cleaner ownership and iteration.
- Main firmware setup path boots AP + HTTP server and supports optional MQTT integration.
- Includes explicit onboarding APIs in network layer:
  - WiFi status/scan/connect/disconnect handlers.
  - MQTT status/config/disconnect handlers.
- Dashboard UI is modern, mobile-aware, and aligned with "router-like setup" expectations.
- Storage status counters (`witness_count`, `health_count`, `unacked_count`) are now populated from live filesystem enumeration + witness-layer truth (previously TODO, resolved 2026-04).

Conclusion:
- **Canonical firmware path** for enterprise onboarding/UI evolution.

### 2) `firmware/projects/canary-wap` is COMPATIBILITY

Why:
- Good Arduino-first path and broad feature exposure.
- Sketch + embedded web UI are very large and monolithic, increasing compile and maintenance friction.
- Already flagged by regression checks for oversize `web_ui.h` and security/privacy warning patterns — these are guarded by the `regression_check.sh` CI job.
- SD flush on shutdown (previously TODO) now forces a FAT sync via a sentinel file before `SD.end()`, resolved 2026-04.
- ESP-NOW RSSI (previously hard-coded `-50`) is now sourced from `esp_now_recv_info_t::rx_ctrl->rssi` via `chirp_channel::dispatch_espnow_message`, resolved 2026-04.

Conclusion:
- Keep as **compatibility lane**, not primary innovation lane.
- Prefer backporting from `firmware/canary` only when needed.

### 3) `firmware/projects/_archive/canary-wap-snapshot` was removed

Why:
- Previously a duplicate of the WAP Arduino code with the same large UI payload, at high risk of silent drift.
- It was first contained under `projects/_archive/`: gated at compile time by `#error "archived, define SECURACV_ALLOW_ARCHIVED_BUILD to override"`, hardened pre-archival (the dead fallback AP password `"witness2026"` removed and `SECURACV_RELEASE_BUILD` guards mirrored from the ACTIVE WAP tree), and protected by the `archive-guard` CI job.
- On 2026-05-29 it was deleted outright. Its full history remains recoverable from git, and the `archive-guard` job in `.github/workflows/firmware.yml` stays in place to govern any future archived trees.

Conclusion:
- **Removed; history in git.** No further maintenance applies. The WAP UX it captured lives on in the COMPATIBILITY tree (`firmware/projects/canary-wap/`).

### 4) `canary-vision` and `canary-ota` remain SPECIALIZED

Why:
- `canary-vision` is optimized for external inference/event flows and MQTT/HA usage, not local AP onboarding UX. (Since 2026-08 it does carry the shared setup portal for provisioning and recovery — that is a safety net, not a product UI.)
- `canary-ota` is an OTA subsystem project and should not be conflated with WAP product UX ownership.

Conclusion:
- Keep as specialized tracks with explicit scope boundaries.

---

## De-rot Plan (status)

| Action | Status |
|---|---|
| 1. Declare canonical firmware owner path (`firmware/canary`) | ✅ Codified in [VARIANT_POLICY.md](VARIANT_POLICY.md) |
| 2. Freeze snapshot tree | ✅ Archived 2026-02-20 (`projects/_archive/`, build-time `#error`); tree removed 2026-05-29 (history in git) |
| 3. Add duplicate-drift / archive-edit CI guard | ✅ `archive-guard` job in `.github/workflows/firmware.yml` |
| 4. Create support policy per firmware path | ✅ [VARIANT_POLICY.md](VARIANT_POLICY.md) |
| 5. Unify UX contract tests (onboarding acceptance) | ⏳ Planned (tracked separately) |
| 6. Progressive migration of Arduino WAP monolith → modular `firmware/canary` | ⏳ In progress; P3 per audit plan |

---

## Recommended Decision

- **Primary product track:** `firmware/canary` (ACTIVE)
- **Secondary/compatibility track:** `firmware/projects/canary-wap` (COMPATIBILITY)
- **Removed:** `firmware/projects/_archive/canary-wap-snapshot` (archived 2026-02-20, deleted 2026-05-29 — history in git)
- **Specialized companion tracks:** `firmware/projects/canary-vision`, `firmware/projects/canary-ota` (SPECIALIZED)

This structure minimizes rot while keeping Arduino-first accessibility and preserving specialized integration projects. Lifecycle transitions are now governed by [VARIANT_POLICY.md](VARIANT_POLICY.md); feature parity is tracked in [FEATURES.md](FEATURES.md).
