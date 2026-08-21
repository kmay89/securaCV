# Security Model

Canary Vision devices sit on the home LAN, serving an HTTP API without TLS (mDNS does not support certificates). This document describes the threat model and the 11 security decisions that mitigate the resulting risks.

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
- Static files (`/`, `/app.js`, `/styles.css`, `/app.webmanifest`) are served without authentication so the SPA can load before the user enters a token.

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

CORS is same-origin plus trust-on-pair. The `Access-Control-Allow-Origin` header is **never** set to `*`, and peer devices are **not** allowlisted just for being peers: a compromised peer could otherwise make cross-origin authenticated requests to every other device in the mesh (a lateral-movement vector).

CORS headers are set only for:

- the device's own origin: `http://<device_ip>` and `http://<mdns_hostname>`;
- origins enrolled by **trust-on-pair**: when a physical BOOT-button press releases the provisioning receipt, the origin that received the receipt is recorded as durably allowed. The press already authorizes handing out the API token itself, so trusting the recipient origin is strictly weaker;
- any private-network origin, but **only** for `/api/provisioning-receipt` — that endpoint is unauthenticated by design, physically gated by the BOOT button, and one-shot, so its browser response is useless without a press.

Every other origin — including a known peer's IP or mDNS hostname — gets no CORS headers, and preflight `OPTIONS` requests from such origins get none either, causing the browser to block the actual request.

**Test:** `tests/security/cors.test.js` (asserts peer origins receive no CORS headers)

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

Firmware updates are uploaded to `POST /api/v1/update` as a raw binary body (`application/octet-stream`) with a detached Ed25519 signature over the whole payload in the `X-Firmware-Signature` header (hex-encoded, 64 bytes). The device verifies the signature against the configured signing public key before applying the update, parses an 8-byte `SCV\x01` version header from the binary, and rejects any version that is not strictly newer than the installed one (anti-downgrade). `GET /api/v1/update/check` additionally advertises the available version with its SHA-256 hash and signature.

### D8: Tamper-Evident Witness Chain

Every detection event (motion, person, vehicle, animal, object removal, contact change, restricted-zone presence, acoustic impulse) is appended to an Ed25519-signed hash chain. Each record contains:

- `seq`: monotonically increasing sequence number
- `hash`: `SHA256("${seq}:${prev_hash}:${timestamp}:${event_type}:${zone}:${time_source}:${gps_timestamp}")`
- `prev_hash`: hash of the previous record (or 64 zeros for genesis)
- `signature`: Ed25519 signature of the hash
- `timestamp`, `event_type`, `zone`
- `time_source`: `gps_utc` (satellite-derived UTC) or `device_clock`; a record without one hashes as `device_clock`
- `gps_timestamp`: present only on `gps_utc` records; hashes as the empty string when absent

The hash formula here matches the one the device embeds in its own `GET /api/v1/witness/export` `verification_instructions` — that endpoint is the executable statement of the contract.

Any tampering (deletion, modification, insertion) breaks the chain. Third parties can verify integrity by walking the chain and checking hashes and signatures.

### D9: Rate Limiting

Three rate-limiting mechanisms protect the device:

| Limit | Threshold | Window | Lockout |
|-------|-----------|--------|---------|
| General requests | 30 per IP | 60 seconds | N/A |
| Auth failures | 5 per IP | failures retained for 300 seconds | Exponential: 2 s for the first lockout, doubling each subsequent lockout, capped at 300 s |
| Reboot | 1 | 5 minutes | Until cooldown expires |
| Firmware update | 1 | 1 hour | Until cooldown expires |

The auth lockout mirrors the firmware's exponential backoff (`DEFAULT_AUTH_LOCKOUT_BASE_SEC=2`, `DEFAULT_AUTH_LOCKOUT_CAP_SEC=300`): the fifth failure triggers a lockout of `min(2 × 2^(lockouts−1), 300)` seconds, and a successful authentication resets the escalation.

The IP tracking map uses LRU eviction with a maximum of 64 entries to bound memory usage on the ESP32.

All 429 responses include a `Retry-After` header.

**Test:** `tests/security/rate-limit.test.js`

### D10: Camera Peek Immutable via API

The `camera_peek_enabled` setting defaults to `false` and **cannot be changed through the API at all** (Invariant I: No Raw Export). A config update that includes it does not fail outright: the key is stripped, the rest of the update applies normally, and the response carries `rejected_immutable: ["camera_peek_enabled"]` so clients can tell the user the toggle did not take. There is no HTTP endpoint that can flip it — enabling camera peek requires physical interaction with the device (the firmware's physical button mechanism).

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
| `Content-Security-Policy` | `default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self' http://*.local http://192.168.* http://10.* http://172.16-31.*` |
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


### D11 — Webhook SSRF and remote-exfiltration guard

Webhook delivery is opt-in, but the configured `integrations.webhook_url` is still treated as a potential SSRF sink because witness events are posted by the device process. The API now validates webhook targets at configuration time and again immediately before dispatch:

- only `http` and `https` URLs are accepted;
- URL credentials and fragments are rejected/stripped;
- public Internet hosts, loopback (`localhost`, `127.0.0.0/8`), and link-local metadata ranges are rejected;
- accepted destinations are limited to RFC1918 LAN IPv4 addresses and `.local` mDNS names for local automation systems.

This preserves the local-ownership model while still allowing Home Assistant-style LAN webhooks.

**Tests:** `tests/api/config.test.js`, `tests/security/webhook-url.test.js`
