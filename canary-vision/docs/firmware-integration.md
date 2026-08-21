# Firmware Integration Guide

This document describes how to port the Canary Vision reference server (`device-api/`) to ESP32 firmware running on the XIAO ESP32S3 (or compatible boards).

## Reference Server vs. Firmware

The Node.js reference server in `device-api/` is a faithful model of the firmware HTTP API. It uses Express middleware to mirror the exact request/response behavior, status codes, and security checks that the ESP32 firmware must implement. The reference server is the source of truth for API behavior, and the test suite validates both.

| Concern | Reference Server | ESP32 Firmware |
|---------|-----------------|----------------|
| HTTP framework | Express 4 | ESP-IDF `httpd` or Arduino `WebServer` |
| JSON parsing | `express.json()` | ArduinoJson or cJSON |
| Crypto | Node.js `crypto` | `mbedtls` (bundled with ESP-IDF) |
| Storage | In-memory JS objects | NVS (Non-Volatile Storage) |
| mDNS | Not implemented (test only) | ESP-IDF `mdns` component |
| Static files | `express.static` | SPIFFS/LittleFS partition |

## Middleware Stack (Order Matters)

The firmware HTTP handler must execute checks in exactly this order. The reference server enforces this ordering, and the test suite verifies it (e.g., host validation runs before auth).

```
Request arrives
  |
  1. Security Headers          -- Set on EVERY response
  |
  2. Host Header Validation    -- 403 if Host not in allowlist
  |
  3. Rate Limiting             -- 429 if over threshold
  |
  4. Static File Serving       -- Serve SPA files, NO auth needed
  |                               (return here if matched)
  |
  5. PNA Preflight             -- Handle OPTIONS with PNA header, return 204
  |
  6. CORS                      -- Same-origin + trust-on-pair origins only
  |                               Handle non-PNA OPTIONS, return 204
  |
  7. JSON Body Parsing         -- Parse body for /api/* routes
  |
  8. Token Authentication      -- 401 if missing/invalid token
  |
  9. Route Handler             -- Process the API request
```

**Critical ordering rules:**
- Security headers must be set before any early return (including 403, 429).
- Host validation runs before auth. A request with a bad Host and no token must get `403 host_rejected`, not `401 token_required`.
- Static files are served before auth so the SPA can load without a token.
- PNA preflight is handled before CORS so Chrome PNA requests get the correct `Access-Control-Allow-Private-Network` header.

## Memory Constraints

The XIAO ESP32S3 has approximately 512 KB of usable RAM. Keep these budgets in mind:

| Resource | Budget | Notes |
|----------|--------|-------|
| Rate limit IP map | 64 entries max | LRU eviction, ~20 bytes per entry |
| Log ring buffer | 1000 entries max | Circular buffer, evict oldest |
| Witness chain | Last 500 records in RAM | Older records in NVS or discarded |
| Peer list | 16 peers max | Sufficient for home deployments |
| Config | ~2 KB | Four sections, shallow objects |
| HTTP request body | 4 KB max | Reject larger payloads early |
| Firmware upload | Stream to flash | Never buffer entire binary in RAM |

### Firmware Upload Handling

The `POST /api/v1/update` endpoint receives the firmware binary as the raw request body (`application/octet-stream`), with a detached Ed25519 signature over the whole payload in the `X-Firmware-Signature` header and an 8-byte `SCV\x01` version header at the start of the binary (see [api.md](api.md#post-apiv1update)). On the ESP32, stream the upload directly to the OTA partition rather than buffering it in RAM (verify the signature incrementally or from the written partition before setting the boot partition). Use the ESP-IDF OTA API:

```c
esp_ota_handle_t ota_handle;
const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);

// In the HTTP receive callback:
esp_ota_write(ota_handle, chunk_data, chunk_len);

// After all data received:
esp_ota_end(ota_handle);
esp_ota_set_boot_partition(update_partition);
esp_restart();
```

## NVS Storage

Use the ESP-IDF Non-Volatile Storage (NVS) library to persist device identity, tokens, keys, and configuration across reboots.

### NVS Namespace Layout

```
Namespace: "canary"

Key                     Type      Description
----                    ----      -----------
device_id               str       e.g., "canary-a3f7"
device_name             str       e.g., "Front Porch"
api_token               str       e.g., "cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f"
ed25519_priv            blob      64-byte Ed25519 private key
ed25519_pub             blob      32-byte Ed25519 public key
fw_verify_pub           blob      32-byte Ed25519 firmware verification public key
witness_seq             u32       Current sequence number
witness_last_hash       str       64-char hex hash of last witness record

Namespace: "config"

Key                     Type      Description
----                    ----      -----------
network                 str       JSON blob of network config
privacy                 str       JSON blob of privacy config
detection               str       JSON blob of detection config
integrations            str       JSON blob of integrations config

Namespace: "peers"

Key                     Type      Description
----                    ----      -----------
count                   u8        Number of stored peers
peer_0                  str       JSON blob of peer 0
peer_1                  str       JSON blob of peer 1
...
```

### Token Storage

The API token is generated once during provisioning and stored in NVS. It must never be logged, included in API responses, or transmitted over the network. The reference server stores it in `state.device.api_token` and only uses it for comparison in `lib/token.js`.

```c
// Read token from NVS
char api_token[64];
size_t token_len = sizeof(api_token);
nvs_get_str(nvs_handle, "api_token", api_token, &token_len);

// Constant-time comparison (mbedtls)
#include "mbedtls/constant_time.h"
int result = mbedtls_ct_memcmp(provided_token, api_token, token_len);
```

### Ed25519 Key Storage

The witness chain signing key pair is generated once during provisioning and stored in NVS. The private key never leaves NVS -- it is read into a stack buffer, used to sign a hash, and immediately zeroed.

```c
// Sign a witness hash
unsigned char priv_key[64];
size_t key_len = sizeof(priv_key);
nvs_get_blob(nvs_handle, "ed25519_priv", priv_key, &key_len);

// ... sign with mbedtls_pk_sign ...

// Zero the key buffer
mbedtls_platform_zeroize(priv_key, sizeof(priv_key));
```

## Security Headers on ESP32

Set these headers on every HTTP response, including error responses:

```c
httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
httpd_resp_set_hdr(req, "Cache-Control", "no-store");
httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
httpd_resp_set_hdr(req, "Content-Security-Policy",
    "default-src 'self'; script-src 'self'; style-src 'self'; "
    "img-src 'self' data:; connect-src 'self' http://*.local http://192.168.* "
    "http://10.* http://172.16.* ... http://172.31.*");
```

The CSP `connect-src` string is ~300 bytes. Store it as a `const char[]` in flash (PROGMEM/DRAM) rather than constructing it dynamically.

## Host Validation on ESP32

Extract the `Host` header, strip any port suffix, and compare against:

1. The device's mDNS hostname (e.g., `canary-a3f7.local`)
2. The device's current IP address (refreshed on WiFi reconnect)

Reject all other values with `403`. Do this check **before** token validation.

## mDNS Registration

At boot, after WiFi connects:

```c
mdns_init();
mdns_hostname_set("canary-a3f7");
mdns_instance_name_set("canary-a3f7");

mdns_service_add(NULL, "_securacv", "_tcp", 80, NULL, 0);

mdns_txt_item_t txt[] = {
    {"device_id", "canary-a3f7"},
    {"name", "Front Porch"},
    {"model", "XIAO ESP32S3"},
    {"fw", "0.4.1"}
};
mdns_service_txt_set("_securacv", "_tcp", txt, 4);
```

To discover peers, periodically browse for `_securacv._tcp`:

```c
mdns_result_t *results = NULL;
mdns_query_ptr("_securacv", "_tcp", 3000, 16, &results);
// Walk results, add to peer list, free with mdns_query_results_free()
```

## SPA Hosting

Store the SPA files (`index.html`, `app.js`, `styles.css`, `app.webmanifest`) in a SPIFFS or LittleFS partition. Serve them from the root path before the auth middleware runs.

Partition table entry:

```
# Name,   Type, SubType, Offset,   Size
spiffs,   data, spiffs,  0x210000, 0x100000
```

The SPA is approximately 40 KB total (26 KB JS + 9 KB CSS + HTML + manifest), well within a 1 MB SPIFFS partition.

## Witness Chain on ESP32

Use `mbedtls_sha256` for hashing and `mbedtls_pk_sign` (with `MBEDTLS_PK_ED25519` if available, or a TweetNaCl port) for Ed25519 signing.

Hash input format: `"${seq}:${prev_hash}:${timestamp}:${event_type}:${zone}:${time_source}:${gps_timestamp}"` — `time_source` defaults to `device_clock` and `gps_timestamp` to the empty string when the record has no GPS fix, matching the reference server's `lib/witness-chain.js` and the `verification_instructions` in `GET /api/v1/witness/export`.

Store `witness_seq` and `witness_last_hash` in NVS after each new record so the chain survives reboots. Keep the most recent 500 records in a RAM ring buffer for API queries; older records can be persisted to SPIFFS or discarded.

## Provisioning Flow

1. Device boots for the first time with no NVS data.
2. Generates Ed25519 key pair, stores in NVS.
3. Generates API token (`cv_<id_suffix>_<32 random hex>`), stores in NVS.
4. Starts WiFi AP for initial configuration (SSID, password).
5. After WiFi connects, registers mDNS and starts the HTTP server.
6. Outputs provisioning receipt (JSON with `device_id`, `base_url`, `token`) via serial or a one-time HTTP endpoint.
7. User imports receipt into the SPA.
