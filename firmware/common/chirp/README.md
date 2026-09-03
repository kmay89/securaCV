# Chirp Channel — API consolidation index

**Status (2026-06-09):** the chirp-channel **API** is consolidated and documented here.
The remaining work is the **ACTIVE body port** (a `securacv_chirp` library under
`firmware/canary/lib/` implementing the canonical header) — tracked as a parity gap in
[`../../PARITY_PLAN.md`](../../PARITY_PLAN.md). No implementation body lives in `firmware/common/`
or `firmware/canary/` yet.

## Why this index exists

"Chirp" names a few related-but-distinct surfaces across the trees. This file records which is
which, so the eventual ACTIVE body port targets the right contract instead of re-deciding the API.

| Surface | Path | Role | Canonical? |
|---|---|---|:---:|
| **`chirp_channel.h`** | `firmware/common/chirp/chirp_channel.h` | **The canonical C-ABI chirp-channel API** (Community Witness Network: ephemeral identity, structured templates, ≤3-hop relay, cooldowns). Consumed by canary-wap (PIO) `firmware/projects/canary-wap/src/main.cpp`. | ✅ |
| `chirp_channel::` (C++) | `firmware/projects/canary-wap/arduino/canary_wap/mesh_network.h` (decls) + `chirp_channel.cpp` (~1480 LOC body) | **Reference implementation** of the same feature, in a C++ namespace embedded in the Arduino lane's mesh header. v0.2 wire format (`PROTOCOL_VERSION = 1`, MAGIC `0xC4`). | reference impl |
| `chirp_api::` | `firmware/projects/canary-wap/arduino/canary_wap/chirp_api.h` | HTTP/REST handlers **above** the chirp API (web endpoints) — a layer, not a competing core API. | — |
| `ble_chirp` | `firmware/projects/canary-wap/arduino/canary_wap/ble_chirp.h` | A **separate** BLE-advertisement broadcast-alert feature (different transport). Shares only the word "chirp". | — (unrelated) |

Spec: [`spec/chirp_channel_v0.md`](../../../spec/chirp_channel_v0.md) ·
audit: [`docs/audit/mesh_and_chirp_audit_v1.md`](../../../docs/audit/mesh_and_chirp_audit_v1.md)

## Dependency-adaptation map for the ACTIVE body port

When the body is ported into `firmware/canary/lib/securacv_chirp/`, the Arduino reference impl's
dependencies map to ACTIVE equivalents as follows (so the port is dependency-adaptation, not a
rewrite):

| Arduino dependency | ACTIVE equivalent | Notes |
|---|---|---|
| `esp_now_send()` / peer table (`<esp_now.h>`) | `mesh_transport::send_raw(mac, data, len)` (`securacv_mesh`) | `send_raw` broadcasts to `FF:FF:FF:FF:FF:FF` without peer registration. |
| ESP-NOW recv callback | register chirp's dispatcher on `mesh_transport::set_recv_callback` in `firmware/canary/src/main.cpp` under `#if FEATURE_CHIRP` | mirrors how `mesh_session` receives. |
| `Ed25519::generate/derive/sign/verify` (`<Ed25519.h>`) | `crypto_generate_keypair()` / `crypto_sign()` / `crypto_verify()` (`securacv_crypto`) | `generate`+`derive` collapse into one keypair call. |
| `mbedtls_sha256_*` (`<mbedtls/sha256.h>`) | `sha256_domain(domain, data, n, out)` (`securacv_crypto`) | domain-separated hash wrapper. |
| `nvs_get_u8` / `nvs_set_u8` (`nvs_store.h`) | `NvsManager` (`securacv_crypto`) or `hal_nvs_*` (`hal/hal_storage.h`) | persists `relay_enabled`, `urgency_filter`. |
| `health_log()` + `SCV_LOG_*` / `SCV_CAT_*` (`health_log.h`) | `log_health()` in `firmware/canary/lib/securacv_witness/src/securacv_witness.h` (levels from `log_level.h`); add a chirp category | integration hook wired in `firmware/canary/src/main.cpp`. |
| `airtime_governor::try_reserve_routine()` (`airtime_governor.h`) | **no ACTIVE equivalent yet** | thin shim returning `true` for slice 1, or port the governor in a later slice. |
| `esp_fill_random()`, `millis()`, `time()`, `localtime()` | same (ESP-IDF / Arduino built-ins) | — |
| `FEATURE_CHIRP` | already `-DFEATURE_CHIRP=1` in `firmware/canary/platformio.ini` `[env:full]` (currently inert) | PlatformIO LDF auto-discovers `lib/securacv_chirp/`. |

The only dependency without an ACTIVE equivalent is the **airtime governor** (one call site) — shim
or port it in the body PR.
