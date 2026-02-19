# Security Model

## Threat Model

The SecuraCV Canary is a privacy-preserving witness device that operates as a local
WiFi Access Point. The primary threats are:

### In-Scope Threats

| Threat | Mitigation |
|--------|-----------|
| **Unauthorized API access** | Bearer token authentication on all protected endpoints |
| **Token brute-force** | Exponential backoff (2s → 5min cap), ~190-bit token entropy |
| **Timing side-channel** | Constant-time token comparison (volatile XOR accumulator) |
| **Network eavesdropping** | TLS 1.2+ encryption on port 443 |
| **Token interception via URL** | Token sent only in Authorization header, never in URLs |
| **Token persistence in browser** | Token stored only in JS variable (no localStorage/cookies) |
| **Multi-client attacks** | AP limited to 1 concurrent client |
| **Credential reuse across devices** | Device-unique AP password and API token |
| **Remote provisioning** | Physical BOOT button required for token retrieval |
| **Token leakage in logs** | Token redacted to first 8 characters in all serial output |
| **Key domain confusion** | Two-step HKDF with domain separation strings |
| **Modular bias in token encoding** | Rejection sampling in base62 encoding |

### Out-of-Scope Threats

| Threat | Rationale |
|--------|-----------|
| **Physical chip extraction** | Device is physically accessible by design; NVS encryption helps but determined attackers with physical access are not the threat model |
| **Internet-based attacks** | Device operates in AP mode only, never connects to the internet |
| **Supply chain attacks** | Firmware is open-source and verifiable |
| **Side-channel on Ed25519 signing** | Arduino Crypto library handles this; not modified |

## Defense-in-Depth Layers

```
Layer 1: Physical Isolation
  └── WiFi AP mode only (no internet gateway)
  └── Max 1 AP client (no multi-client attacks)

Layer 2: Transport Encryption
  └── TLS 1.2+ (self-signed RSA-2048 certificate)
  └── HTTP port 80 → HTTPS 301 redirect
  └── Certificate persisted across reboots (NVS)

Layer 3: Authentication
  └── Bearer token via Authorization header
  └── Constant-time comparison
  └── Exponential backoff on failures
  └── Token never in URLs, never in cookies

Layer 4: Physical Gate
  └── Provisioning receipt requires BOOT button press
  └── Factory reset requires long BOOT press (3+ seconds)
  └── Mesh pairing requires physical confirmation

Layer 5: Credential Isolation
  └── Device-unique AP password (derived from fingerprint)
  └── Device-unique API token (derived from keypair + MAC)
  └── Two-step HKDF domain separation
  └── Token never on SD card, never in witness chain

Layer 6: Monitoring
  └── Auth stats in health endpoint
  └── Failed attempt logging (SCV_CAT_AUTH)
  └── Lockout state visible via API
```

## Cryptographic Components

### Ed25519 Keypair

- Generated at first boot using Arduino Crypto library
- Stored in NVS (ESP32 Non-Volatile Storage)
- Used for witness record signing (primary purpose)
- Private key material used as HKDF input key (secondary purpose)

### HMAC-SHA256 (Token Derivation)

- Implementation: mbedtls (bundled with ESP-IDF)
- Used in two-step HKDF-style derivation
- Domain separation prevents key confusion between token and signing contexts

### RSA-2048 (TLS Certificate)

- Generated at first boot using mbedtls
- Self-signed X.509 certificate (CN=SecuraCV Canary)
- 30-year validity window (2020–2050, local-only device without reliable RTC at boot)
- Stored in NVS as DER-encoded blobs
- SHA-256 fingerprint available via `/api/device-info`

### Base62 Encoding

- Character set: `A-Za-z0-9` (62 characters)
- Rejection sampling with threshold 248 (= 62 * 4)
- Eliminates the ~1.6% modular bias of naive `byte % 62`
- Produces uniform distribution across all 62 characters

## Token Security Properties

| Property | Implementation |
|----------|---------------|
| Entropy | ~190 bits (32 base62 characters) |
| Deterministic | Same keypair always produces same token |
| Unique per device | MAC address mixed into derivation |
| Domain-separated | Two-step HKDF with version strings |
| Timing-safe comparison | Volatile XOR accumulator, constant time |
| Redacted in logs | Only `cv_XXXXXXXX****` shown |
| Not persisted in browser | JS variable only, cleared on tab close |
| Not in URLs | Authorization header only |
| Not on SD card | NVS only |
| Not in witness chain | Excluded from all signed records |

## Rate Limiting

### Authentication Rate Limiting

- Exponential backoff per-device (single AP client)
- Base: 2 seconds, doubling per failure
- Cap: 5 minutes (300 seconds)
- Reset: 1 minute of no failures

### General Rate Limiting

- 120 requests per minute (all endpoints)
- 30 POST actions per minute (state-changing endpoints)
- Per-client IP tracking (effectively per-session given 1 client max)

## CORS Policy

- Same-origin only (no `Access-Control-Allow-Origin` header set)
- No cross-origin requests permitted
- Dashboard served from same origin as API

## Security Headers

Protected endpoints include:
- `Cache-Control: no-store` (prevent caching of sensitive data)
- `X-Content-Type-Options: nosniff`
- Content-Type explicitly set on all responses
