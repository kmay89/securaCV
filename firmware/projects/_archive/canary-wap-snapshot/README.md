# Canary WAP Snapshot — ARCHIVED (FROZEN 2026-02-20)

> **Status:** ARCHIVED. Do not edit. Do not build in CI. Kept for historical
> reference only.
>
> **Active Arduino WAP path:** `firmware/projects/canary-wap/arduino/canary_wap/`
> **Active modular path:** `firmware/canary/` (PlatformIO, preferred)

## Why this is archived

This directory was a frozen reference snapshot of the Arduino `canary_wap`
sketch as it existed on **2026-02-20**. Active development continued in
`firmware/projects/canary-wap/arduino/canary_wap/`, and by 2026-03-09 the two
trees had diverged enough that this snapshot represented outdated and
security-weaker code (notably, it retained the removed `"witness2026"`
fallback AP password and lacked the `SECURACV_RELEASE_BUILD` fail-closed
guards).

## What changed when archiving

Two targeted security backports were applied before freezing, then the tree
was moved to `firmware/projects/_archive/` and made unbuildable by default:

1. Removed the static `"witness2026"` AP password from
   `snapshot/canary_wap/wap_server.h` (the constant was dead code).
2. Neutralized `AP_PASSWORD_DEFAULT` in `snapshot/canary_wap/securacv_canary_wap.ino`
   (now empty string; no functional fallback). Boot will fail closed if
   provisioning has not produced a device-unique password.
3. Added `SECURACV_RELEASE_BUILD` guards to `snapshot/canary_wap/build_config.h`
   mirroring the active canary-wap tree.
4. Added a compile-time `#error` in `build_config.h` that blocks accidental
   builds. If you genuinely need to reproduce the historical binary for an
   audit, pass `-DSECURACV_ALLOW_ARCHIVED_BUILD=1`.

## Rules for this archive

- **No new commits** that change functionality. Edits under
  `firmware/projects/_archive/**` are rejected by CI unless the commit
  message contains `[archive-edit]` (used only for rare follow-up security
  backports).
- If you need to port behavior, copy it into the active tree at
  `firmware/projects/canary-wap/` or `firmware/canary/`. Do not resurrect
  this tree.
- The historical documentation below is preserved verbatim for reference,
  but the claims about "fastest builds" and active build paths no longer
  apply.

---

## Historical (pre-archival) content

The rest of this document is a verbatim copy of the original README as it
existed at freeze time. Links and instructions are historical; do not follow
them for new work.

---

This directory captures a **frozen snapshot** of the existing Arduino sketch so
we can iterate toward the firmware architecture without breaking the current
working demo. The files under `snapshot/` are intentionally **not wired into any
build system**; they serve as a reference baseline only.

## BLE Discovery (Opera/Chirp/Nearby)

The Canary firmware includes an optional BLE Discovery subsystem controlled by the
`FEATURE_BLE` compile flag (enabled in FULL profile). It adds three BLE subsystems:

- **Opera**: BLE server/advertising — Canary broadcasts its presence via custom GATT service
- **Chirp**: Connectionless broadcast alerts between Canary devices
- **Nearby**: BLE scanner that discovers other Canaries and tracks proximity (RSSI)

**New files**: `ble_config.h`, `ble_opera.h`, `ble_chirp.h`, `ble_nearby.h`, `ble_manager.h`

**API endpoints**: `GET /api/ble/status`, `GET /api/nearby`, `POST /api/chirp/send`

**Library dependency**: NimBLE-Arduino by h2zero (install via Arduino Library Manager)

**Antenna requirement**: The XIAO ESP32S3 has a dedicated IPEX BLE antenna connector — it
must be installed for BLE to work. If absent, the firmware gracefully degrades without BLE.

**Security note**: BLE can be compiled out entirely by setting `FEATURE_BLE=0`. When disabled,
all BLE binary blobs are removed from the build, reducing the trust surface.

See `docs/ble_protocol.md` for the full protocol specification.

## API Security (v2.1.0+)

The Canary WAP implements defense-in-depth API security:

- **TLS 1.2+** — All connections encrypted via self-signed RSA-2048 certificate (port 443)
- **Bearer token auth** — All protected API endpoints require `Authorization: Bearer cv_...` header
- **HKDF token derivation** — API token derived deterministically from Ed25519 keypair via two-step HMAC-SHA256
- **Device-unique credentials** — Both AP password and API token are unique per device
- **Constant-time comparison** — Prevents timing side-channel attacks on token validation
- **Exponential backoff** — Failed auth attempts trigger 2s→5min escalating lockout
- **Physical provisioning gate** — BOOT button press required to retrieve token via API
- **Single client AP** — Max 1 WiFi client for security isolation
