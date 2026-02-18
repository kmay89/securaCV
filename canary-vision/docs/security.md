# Security Model

Canary Vision devices sit on the home LAN, serving an HTTP API without TLS (mDNS does not support certificates). This document describes the threat model and the 10 security decisions that mitigate the resulting risks.

## Threat Model

### T1: DNS Rebinding

An attacker-controlled website tricks the browser into sending requests to `canary-a3f7.local` by returning the device's LAN IP from a crafted DNS response. The browser believes it is talking to the attacker's origin, so same-origin policy does not block the request.

**Mitigations:** Host header validation (D2), restrictive CORS (D3), Chrome PNA preflight (D4), mandatory API token (D1).

### T2: LAN Attacker

A compromised or malicious device on the same network segment can reach the Canary device directly. Since traffic is unencrypted HTTP, the attacker can observe and replay requests.

**Mitigations:** Mandatory API token (D1), rate limiting and auth lockout (D9), constant-time token comparison (D1), no fleet-wide config sync (D6).

### T3: Supply Chain / Firmware Tampering

A malicious firmware update could exfiltrate data, disable privacy controls, or backdoor the device.

**Mitigations:** Firmware signatures verified against Ed25519 public key (D7), update rate limiting (D9), witness chain provides tamper-evident event log (D8).

### T4: Lateral Movement

If one Canary device is compromised, the attacker should not be able to pivot to control the entire fleet.

**Mitigations:** No subnet scanning (D5), no fleet-wide config sync (D6), per-device tokens (D1), each device manages its own config independently.

---

## Security Decisions

### D1: Mandatory API Token

Every `/api/*` request requires a valid `X-Canary-Token` header. Tokens follow the format `cv_<device_id_suffix>_<32 hex chars>` and are unique per device.

- Missing token returns `401 token_required`.
- Invalid token returns `401 token_invalid`.
- Token comparison uses `crypto.timingSafeEqual` to prevent timing side-channels. When lengths differ, a dummy comparison against the expected token is still performed to avoid leaking length information.
- Static files (`/`, `/app.js`, `/styles.css`, `/manifest.json`) are served without authentication so the SPA can load before the user enters a token.

**Test:** `tests/security/auth.test.js`

### D2: Host Header Validation

Every request is checked against an allowlist of valid Host header values before any further processing (including auth). Only the device's own mDNS hostname and IP address are accepted. Port suffixes are stripped before comparison.

Allowed values for device `canary-a3f7`:
- `canary-a3f7.local`
- `192.168.1.47`
- `localhost`, `127.0.0.1` (dev mode only)

Any other Host value, including attacker subdomains like `canary-a3f7.local.evil.com`, returns `403 host_rejected`.

Host validation runs **before** authentication, so a DNS rebinding attack is blocked even if the attacker somehow obtains a valid token.

**Test:** `tests/security/host-validation.test.js`

### D3: Restrictive CORS

CORS headers are only set for origins that match known peers. The `Access-Control-Allow-Origin` header is **never** set to `*`. If the `Origin` header does not match a registered peer's IP or mDNS hostname, no CORS headers are added to the response.

Allowed origins are constructed from the peer list:
- `http://<peer_ip>` for each peer
- `http://<peer_device_id>.local` for each peer
- The device's own IP and mDNS hostname

Preflight `OPTIONS` requests for unknown origins receive no CORS headers, causing the browser to block the actual request.

**Test:** `tests/security/cors.test.js`

### D4: Chrome Private Network Access (PNA) Preflight

Chrome 104+ sends a PNA preflight `OPTIONS` request with `Access-Control-Request-Private-Network: true` before any page on the public internet can access a private-network resource. The device responds with:

```
Access-Control-Allow-Private-Network: true
Private-Network-Access-Name: Canary Vision (Front Porch)
Private-Network-Access-ID: AA:BB:CC:DD:EE:01
```

This gives the user a browser-level prompt identifying the device before allowing access. Non-PNA `OPTIONS` requests do not receive these headers.

**Test:** `tests/security/pna.test.js`

### D5: No Network Scanning

The device API does **not** expose any `/api/v1/scan`, `/api/v1/discover`, or similar endpoint. The SPA source code contains no subnet scanning patterns: no IP iteration, no broadcast address references, no `probe` or `subnet` keywords.

Discovery is passive only -- via mDNS announcements, peer list exchange, manual entry, or receipt import. See [discovery.md](discovery.md).

**Test:** `tests/security/no-scan.test.js`

### D6: No Fleet-Wide Config Sync

There is no endpoint to push configuration or firmware updates across the fleet. No `/api/v1/fleet/config`, `/api/v1/config/broadcast`, `/api/v1/fleet/update`, or `/api/v1/peers/update` endpoints exist. A `PUT /api/v1/config` call affects only the device it is sent to.

This limits the blast radius of a single compromised device. To update fleet configuration, the SPA must make individual requests to each device.

**Test:** `tests/security/no-fleet-sync.test.js`

### D7: Signed Firmware Updates

Firmware update files include a SHA-256 hash and an Ed25519 signature. The `GET /api/v1/update/check` endpoint returns both. The device verifies the signature against a built-in public key before applying the update. Updates are uploaded as `multipart/form-data` via `POST /api/v1/update`.

### D8: Tamper-Evident Witness Chain

Every detection event (motion, person, vehicle, animal) is appended to an Ed25519-signed hash chain. Each record contains:

- `seq`: monotonically increasing sequence number
- `hash`: `SHA256("${seq}:${prev_hash}:${timestamp}:${event_type}:${zone}")`
- `prev_hash`: hash of the previous record (or 64 zeros for genesis)
- `signature`: Ed25519 signature of the hash
- `timestamp`, `event_type`, `zone`

Any tampering (deletion, modification, insertion) breaks the chain. Third parties can verify integrity by walking the chain and checking hashes and signatures.

### D9: Rate Limiting

Three rate-limiting mechanisms protect the device:

| Limit | Threshold | Window | Lockout |
|-------|-----------|--------|---------|
| General requests | 30 per IP | 60 seconds | N/A |
| Auth failures | 5 per IP | 60 seconds | 60-second full lockout |
| Reboot | 1 | 5 minutes | Until cooldown expires |
| Firmware update | 1 | 1 hour | Until cooldown expires |

The IP tracking map uses LRU eviction with a maximum of 64 entries to bound memory usage on the ESP32.

All 429 responses include a `Retry-After` header.

**Test:** `tests/security/rate-limit.test.js`

### D10: Camera Peek Default Off

The `camera_peek_enabled` setting defaults to `false`. Attempting to set it to `true` via the API does **not** immediately enable it. Instead, the response returns `pending_physical_confirm: ["camera_peek_enabled"]`, and the setting remains `false` until a physical button press on the device confirms the change (simulated via `POST /api/v1/confirm` in the reference server).

Additional privacy defaults:
- `video_storage`: `"none"`
- `semantic_events_only`: `true` (only event metadata, no raw frames)
- `local_processing_only`: `true`

**Test:** `tests/security/camera-peek.test.js`

---

## Security Headers

All responses (including static files) include:

| Header | Value |
|--------|-------|
| `X-Content-Type-Options` | `nosniff` |
| `X-Frame-Options` | `DENY` |
| `Content-Security-Policy` | `default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self' http://*.local http://192.168.* http://10.* http://172.16-31.*` |
| `Cache-Control` | `no-store` |
| `Referrer-Policy` | `no-referrer` |

The CSP `connect-src` directive allows the SPA to reach other Canary devices on the LAN (mDNS `.local` and RFC 1918 addresses) while blocking connections to the public internet.

`X-Powered-By` is disabled.

**Test:** `tests/security/security-headers.test.js`

---

## SPA Compliance

The SPA enforces additional client-side security constraints, verified by tests:

- No inline `<script>` tags or `on*` event handler attributes in HTML
- No `eval()`, `new Function()`, or `document.write` in JavaScript
- No `.innerHTML` usage (prevents DOM XSS)
- No third-party tracking or analytics libraries
- Tokens stored per-device in `localStorage` under the `canary_devices` key
- All API requests validated as private-network URLs before sending

**Tests:** `tests/spa/csp-compliance.test.js`, `tests/spa/token-storage.test.js`
