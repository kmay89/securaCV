# API Authentication Reference

## Overview

All protected SecuraCV Canary API endpoints require Bearer token authentication via the
`Authorization` HTTP header. Tokens are derived deterministically from the device's Ed25519
private key using HKDF-style HMAC-SHA256, meaning the same device always produces the same
token — no separate secret storage is needed beyond the existing keypair.

## Token Format

```
cv_<32 base62 characters>
```

- Prefix: `cv_` (constant, for easy identification)
- Body: 32 characters from `[A-Za-z0-9]` (base62)
- Total length: 35 characters
- Entropy: ~190 bits (32 * log2(62))
- Example: `cv_7kX9mN2pQrS5tU8vW0xY1zA3bC4dE6f`

## Token Derivation

Two-step HKDF-style process using HMAC-SHA256 with domain separation:

```
Step 1 (Extract):
  intermediate_key = HMAC-SHA256(
    key  = Ed25519_private_key[0..31],
    data = "securacv:token-key-derive:v1"
  )

Step 2 (Expand):
  token_hash = HMAC-SHA256(
    key  = intermediate_key,
    data = "securacv:api-token:v1" || WiFi_MAC_address
  )
```

The token body is then produced by base62-encoding `token_hash` with rejection sampling
(threshold 248 = 62 * 4) to eliminate modular bias.

### Why Two Steps?

Domain separation ensures the private key is never used directly in the token context.
If the token derivation were ever compromised, the intermediate key leaks — not the
Ed25519 signing key. This limits blast radius.

## Authentication Flow

### Making Authenticated Requests

Include the token in the `Authorization` header:

```http
GET /api/status HTTP/1.1
Host: 192.168.4.1
Authorization: Bearer cv_7kX9mN2pQrS5tU8vW0xY1zA3bC4dE6f
```

### Response Codes

| Code | Meaning | Action |
|------|---------|--------|
| 200  | Authenticated successfully | Process response |
| 401  | Missing or malformed Authorization header | Include `Authorization: Bearer <token>` |
| 403  | Invalid token | Check token value |
| 429  | Too many failed attempts (locked out) | Wait for `Retry-After` seconds |

### 401 Response

```json
{
  "error": "unauthorized",
  "hint": "Include 'Authorization: Bearer cv_...' header"
}
```

Includes `WWW-Authenticate: Bearer realm="securacv"` header.

### 403 Response

```json
{
  "error": "forbidden"
}
```

No additional detail is provided to avoid information leakage.

### 429 Response

```json
{
  "error": "too_many_requests",
  "retry_after_s": 32
}
```

Includes `Retry-After` header with seconds until lockout expires.

## Brute-Force Mitigation

Failed authentication triggers exponential backoff:

| Attempt | Lockout Duration |
|---------|-----------------|
| 1       | 2 seconds       |
| 2       | 4 seconds       |
| 3       | 8 seconds       |
| 4       | 16 seconds      |
| 5       | 32 seconds      |
| 6+      | 5 minutes (cap) |

The failure counter resets after 1 minute of no failures.

### Timing Attack Mitigation

Token comparison uses constant-time byte comparison (`volatile` accumulator XOR pattern)
to prevent timing side-channel attacks. All comparisons take the same duration regardless
of where the mismatch occurs.

## Endpoint Classification

### Public Endpoints (No Auth)

| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Dashboard HTML (shows auth modal) |
| GET | `/api/device-info` | Non-sensitive device metadata |
| GET | `/api/provisioning-receipt` | Token retrieval (requires BOOT button press) |

### Protected Endpoints (Bearer Token Required)

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | Device status |
| GET | `/api/health` | System health + auth stats |
| GET | `/api/identity` | Device identity (pubkey, fingerprint) |
| GET | `/api/chain` | Hash chain state |
| GET | `/api/witness` | Witness records (paginated) |
| GET | `/api/witness/:seq` | Specific witness record |
| GET | `/api/witness/stats` | Witness statistics |
| GET | `/api/logs` | Health logs (paginated) |
| GET | `/api/logs/unacked` | Unacknowledged logs |
| POST | `/api/logs/:seq/ack` | Acknowledge log entry |
| POST | `/api/logs/ack-all` | Acknowledge all logs to level |
| GET | `/api/gps` | GPS status |
| GET | `/api/time` | Time sync status |
| POST | `/api/export` | Create export bundle |
| GET | `/api/export/download` | Download export |
| GET | `/api/config` | Current configuration |
| POST | `/api/config` | Update configuration |
| POST | `/api/reboot` | Reboot device |
| GET | `/api/mesh` | Mesh status |
| GET | `/api/mesh/peers` | Mesh peer list |
| POST | `/api/mesh/*` | All mesh actions |
| GET | `/api/chirp` | Chirp status |
| POST | `/api/chirp/*` | All chirp actions |

## Token Security Properties

1. **Never stored on SD card** — Token lives only in NVS (encrypted flash) and RAM
2. **Never in URLs** — Transmitted only via `Authorization` header
3. **Never in witness chain** — Not included in any signed record
4. **Redacted in logs** — Only first 8 characters shown (e.g., `cv_7kX9****`)
5. **No localStorage/cookies** — Dashboard stores token only in JS variable (lost on tab close)
6. **Deterministic** — Derived from keypair, so lost tokens can be re-derived on the device
7. **Device-unique** — MAC address is mixed into derivation, so no two devices share a token

## Auth Stats (Health Endpoint)

The `/api/health` endpoint includes authentication statistics:

```json
{
  "auth": {
    "total_successes": 42,
    "total_failures": 3,
    "consecutive_failures": 0,
    "locked_out": false
  }
}
```

When locked out:

```json
{
  "auth": {
    "total_successes": 42,
    "total_failures": 8,
    "consecutive_failures": 5,
    "locked_out": true,
    "lockout_remaining_s": 27
  }
}
```
