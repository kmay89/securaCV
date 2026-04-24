# Firmware Variant Audit (Canary Family)

**Last updated:** 2026-04-17
**Original audit date:** 2026-03-07
**Scope:** per-variant risk analysis. For lifecycle labels see [VARIANT_POLICY.md](VARIANT_POLICY.md). For per-feature parity see [FEATURES.md](FEATURES.md).

Purpose: identify which firmware path is currently most active and most user-friendly (especially for onboarding/UI), and flag variants at risk of rot.

## Executive Summary

- **Canonical / ACTIVE path:** [`firmware/canary`](./canary)
  - Modular architecture, active recent commits, integrated AP + onboarding APIs (`/api/wifi/*`, `/api/mqtt/*`), and modern dashboard UI.
- **COMPATIBILITY (Arduino-first) path:** [`firmware/projects/canary-wap/arduino/canary_wap`](./projects/canary-wap/arduino/canary_wap)
  - Functionally rich and very UI-heavy, but monolithic and high maintenance cost.
- **ARCHIVED:** [`firmware/projects/_archive/canary-wap-snapshot`](./projects/_archive/canary-wap-snapshot)
  - Frozen 2026-02-20. Protected by a build-time `#error` and by the `archive-guard` CI job. Security fixes (removed fallback AP password, `SECURACV_RELEASE_BUILD` guards) were backported before archival so the frozen tree is not a residual liability.
- **SPECIALIZED tracks (not primary WAP UX path):**
  - [`firmware/projects/canary-vision`](./projects/canary-vision) (ESP32-C3 + Vision AI, MQTT-focused)
  - [`firmware/projects/canary-ota`](./projects/canary-ota) (OTA A/B subsystem)

---

## Comparison Matrix

| Firmware path | Lifecycle | Primary role | Last activity signal | On-device UI friendliness | Onboarding readiness | Rot risk |
|---|---|---|---|---|---|---|
| `firmware/canary` | ACTIVE | Main ESP32-S3 witness firmware (modular libs) | Recent feature work in 2026-02 for MQTT + WiFi STA, plus storage counter implementation 2026-04 | **High** (modern dashboard in `securacv_webui`) | **High** (WiFi scan/connect + MQTT config HTTP APIs) | Low |
| `firmware/projects/canary-wap` | COMPATIBILITY | Arduino-IDE-first WAP sketch (PlatformIO + Arduino) | Security hardening + real ESP-NOW RSSI landed 2026-04 | High (large embedded dashboard) | Medium/High (rich feature set, heavier maintenance) | Medium |
| `firmware/projects/_archive/canary-wap-snapshot` | ARCHIVED | Frozen reference of WAP sketch | Frozen 2026-02-20 by policy | Medium (same copied UI) | Low (not intended as evolving source) | **Contained** (build-gated + CI archive-guard) |
| `firmware/projects/canary-vision` | SPECIALIZED | Vision events + MQTT/HA integration | Active security hardening in 2026-02 | Low (no primary local onboarding UI focus) | Medium (good integration story, less local setup UX) | Medium |
| `firmware/projects/canary-ota` | SPECIALIZED | OTA framework and rollback safety | Active in 2026-02 | N/A (not a WAP UX product) | N/A | Low/Medium |
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

### 3) `firmware/projects/_archive/canary-wap-snapshot` is contained

Why:
- Previously a duplicate of the WAP Arduino code with the same large UI payload, at high risk of silent drift.
- Now sits under `projects/_archive/` and is:
  - gated at compile time by `#error "archived, define SECURACV_ALLOW_ARCHIVED_BUILD to override"`
  - hardened pre-archival: the dead fallback AP password `"witness2026"` has been removed and `SECURACV_RELEASE_BUILD` guards mirror the ACTIVE WAP tree
  - protected by the `archive-guard` job in `.github/workflows/firmware.yml`, which fails on any edit to `firmware/projects/_archive/**` without `[archive-edit]` in the PR title/body or commit message.

Conclusion:
- **Frozen and auditable.** No further maintenance expected per [VARIANT_POLICY.md](VARIANT_POLICY.md).

### 4) `canary-vision` and `canary-ota` remain SPECIALIZED

Why:
- `canary-vision` is optimized for external inference/event flows and MQTT/HA usage, not local AP onboarding UX.
- `canary-ota` is an OTA subsystem project and should not be conflated with WAP product UX ownership.

Conclusion:
- Keep as specialized tracks with explicit scope boundaries.

---

## De-rot Plan (status)

| Action | Status |
|---|---|
| 1. Declare canonical firmware owner path (`firmware/canary`) | ✅ Codified in [VARIANT_POLICY.md](VARIANT_POLICY.md) |
| 2. Freeze snapshot tree | ✅ Moved to `projects/_archive/` with build-time `#error` |
| 3. Add duplicate-drift / archive-edit CI guard | ✅ `archive-guard` job in `.github/workflows/firmware.yml` |
| 4. Create support policy per firmware path | ✅ [VARIANT_POLICY.md](VARIANT_POLICY.md) |
| 5. Unify UX contract tests (onboarding acceptance) | ⏳ Planned (tracked separately) |
| 6. Progressive migration of Arduino WAP monolith → modular `firmware/canary` | ⏳ In progress; P3 per audit plan |

---

## Recommended Decision

- **Primary product track:** `firmware/canary` (ACTIVE)
- **Secondary/compatibility track:** `firmware/projects/canary-wap` (COMPATIBILITY)
- **Archive/frozen:** `firmware/projects/_archive/canary-wap-snapshot` (ARCHIVED)
- **Specialized companion tracks:** `firmware/projects/canary-vision`, `firmware/projects/canary-ota` (SPECIALIZED)

This structure minimizes rot while keeping Arduino-first accessibility and preserving specialized integration projects. Lifecycle transitions are now governed by [VARIANT_POLICY.md](VARIANT_POLICY.md); feature parity is tracked in [FEATURES.md](FEATURES.md).
