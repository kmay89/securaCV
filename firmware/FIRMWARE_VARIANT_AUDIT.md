# Firmware Variant Audit (Canary Family)

Date: 2026-03-07

Purpose: identify which firmware path is currently most active and most user-friendly (especially for onboarding/UI), and flag variants at risk of rot.

## Executive Summary

- **Most user-friendly + enterprise-ready candidate (current):** `firmware/canary`
  - Modular architecture, active recent commits, integrated AP + onboarding APIs (`/api/wifi/*`, `/api/mqtt/*`), and modern dashboard UI.
- **Arduino-first compatibility path:** `firmware/projects/canary-wap/arduino/canary_wap`
  - Functionally rich and very UI-heavy, but monolithic and high maintenance cost.
- **High rot risk:** `firmware/projects/canary-wap-snapshot`
  - Snapshot mirror that duplicates the Arduino WAP tree and will drift unless explicitly frozen.
- **Specialized/adjacent stacks (not primary WAP UX path):**
  - `firmware/projects/canary-vision` (ESP32-C3 + Vision AI, MQTT-focused)
  - `firmware/projects/canary-ota` (ESP-IDF OTA subsystem)

---

## Comparison Matrix

| Firmware path | Primary role | Last activity signal | On-device UI friendliness | Onboarding readiness | Rot risk |
|---|---|---|---|---|---|
| `firmware/canary` | Main ESP32-S3 witness firmware (modular libs) | Recent feature work in 2026-02 for MQTT + WiFi STA | **High** (modern dashboard in `securacv_webui`) | **High** (WiFi scan/connect + MQTT config HTTP APIs) | Medium |
| `firmware/projects/canary-wap` | Legacy/compat WAP project (PlatformIO + Arduino sketch) | Mostly active through 2026-02, then mostly docs updates | High (large embedded dashboard) | Medium/High (rich feature set, but heavier maintenance) | Medium/High |
| `firmware/projects/canary-wap-snapshot` | Snapshot copy of WAP sketch | Frozen snapshot behavior | Medium (same copied UI) | Low (not intended as evolving source) | **High** |
| `firmware/projects/canary-vision` | Vision events + MQTT/HA integration | Active security hardening in 2026-02 | Low (no primary local onboarding UI focus) | Medium (good integration story, less local setup UX) | Medium |
| `firmware/projects/canary-ota` | OTA framework and rollback safety | Active in 2026-02 | N/A (not a WAP UX product) | N/A | Low/Medium |
| `firmware/common` + `firmware/configs` + `firmware/boards` | Shared platform layers | Mixed cadence | N/A | N/A | Medium |

---

## Evidence and Observations

### 1) `firmware/canary` is the strongest primary candidate

Why:
- Modularized into dedicated libraries (`securacv_network`, `securacv_webui`, `securacv_mqtt`, etc.) for cleaner ownership and iteration.
- Main firmware setup path boots AP + HTTP server and supports optional MQTT integration.
- Includes explicit onboarding APIs in network layer:
  - WiFi status/scan/connect/disconnect handlers.
  - MQTT status/config/disconnect handlers.
- Dashboard UI is modern, mobile-aware, and aligned with “router-like setup” expectations.

Conclusion:
- Treat `firmware/canary` as the **canonical firmware path** for enterprise onboarding/UI evolution.

### 2) `firmware/projects/canary-wap` remains useful but costly

Why:
- Good Arduino-first path and broad feature exposure.
- But sketch + embedded web UI are very large and monolithic, increasing compile and maintenance friction.
- Already flagged by regression checks for oversize `web_ui.h` and security/privacy warning patterns.

Conclusion:
- Keep as **compatibility lane**, not primary innovation lane.
- Prefer backporting from canonical `firmware/canary` only when needed.

### 3) `firmware/projects/canary-wap-snapshot` is clear rot hotspot

Why:
- Snapshot mostly duplicates the WAP Arduino code (same large UI payload hash).
- Minimal structural difference beyond file naming (`canary_wap.ino` vs `securacv_canary_wap.ino`) and README placement.
- Parallel edits here would almost certainly diverge and confuse release ownership.

Conclusion:
- Mark as **archive/frozen** and prevent feature work landing there.

### 4) `canary-vision` and `canary-ota` should stay specialized

Why:
- `canary-vision` is optimized for external inference/event flows and MQTT/HA usage, not local AP onboarding UX.
- `canary-ota` is an OTA subsystem project and should not be conflated with WAP product UX ownership.

Conclusion:
- Keep as specialized tracks with explicit scope boundaries.

---

## De-rot Plan (recommended)

1. **Declare canonical firmware owner path**
   - Set `firmware/canary` as the single source for onboarding UI/API behavior.

2. **Freeze snapshot trees**
   - Mark `firmware/projects/canary-wap-snapshot` as archived read-only in docs and CI checks.

3. **Add duplicate drift checks in CI**
   - Fail CI if snapshot folders diverge unexpectedly (or if they are edited at all without override label).

4. **Create support policy per firmware path**
   - `active`, `compatibility`, `archived`, `specialized` labels in one top-level firmware status table.

5. **Unify UX contract tests**
   - A shared onboarding acceptance test set (AP join, captive portal load, WiFi connect, MQTT config save, status confirmation).

6. **Progressive migration**
   - Gradually move Arduino WAP monolith concerns into modular `firmware/canary` libraries, reducing duplicated logic.

---

## Recommended Decision

- **Primary product track:** `firmware/canary`
- **Secondary/compatibility track:** `firmware/projects/canary-wap`
- **Archive/frozen:** `firmware/projects/canary-wap-snapshot`
- **Specialized companion tracks:** `firmware/projects/canary-vision`, `firmware/projects/canary-ota`

This structure minimizes rot while keeping Arduino-first accessibility and preserving specialized integration projects.
