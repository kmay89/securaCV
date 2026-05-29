# SecuraCV Canary — Threat Model

> Technical threat model for developers and security auditors.
> For the user-facing document, see `SECURITY_MODEL.md`.

---

## Design Standard

> If Moxie Marlinspike audited this device, would he find a trust
> assumption that could be exploited? If Meredith Whittaker presented
> it to a room of activists in a hostile state, could she promise them
> it won't betray them?
>
> If the answer to either question is "no," the design is not done.

---

## Target Users

- A tenant documenting a landlord who enters without permission
- A journalist protecting source meeting evidence in a hostile state
- An activist recording police presence at a protest
- A domestic abuse survivor documenting patterns
- A whistleblower who needs tamper-proof records
- A human rights observer in a conflict zone

**Every design decision must be evaluated against the most vulnerable
user on this list.**

---

## Adversary Classes

| Class | Examples | Capability | Required Resistance |
|-------|----------|------------|---------------------|
| **Casual** | Curious roommate, opposing party in civil dispute | Physical access, basic technical skills | Device-unique credentials, TLS, auth lockout |
| **Sophisticated** | Corporate adversary, determined stalker, corrupt official | Targeted attacks, social engineering, legal compulsion | No remote access surface, no cloud dependency, key isolation |
| **Institutional** | Law enforcement, intelligence agency, state actor | Compelled cooperation orders, supply chain compromise, RF surveillance, forensic analysis | Zero phone-home, no ERRERlabs-held secrets, append-only chain |
| **Systemic** | Compromised update channel, supply chain backdoor, manufacturer coercion | Silent, persistent, potentially affecting all devices | User-initiated OTA only, reproducible builds, open source |

---

## The Ten Security Principles

### 1. Keys Never Leave the Device

The Ed25519 private key is generated on the ESP32-S3's hardware RNG
and stored in NVS. It is **never**:
- Transmitted over WiFi, USB, or any interface
- Included in exports, logs, diagnostics, or crash dumps
- Derivable from any data that leaves the device
- Readable through the API, dashboard, or any debug interface
- Backed up (key loss = device re-provisioning, not key recovery)

There is no export function, backdoor, recovery key, escrow, or master key.

**Rationale:** A key that can be exported can be compelled. Court orders,
national security letters, rubber-hose attacks — if the key CAN leave
the device, an adversary with sufficient power WILL extract it.

### 2. Zero Phone-Home

The device makes **zero outbound network connections**:
- No DNS lookups
- No NTP sync (time comes from GPS)
- No telemetry, analytics, or crash reporting
- No update checks (OTA is user-initiated)
- No cloud sync, MQTT publish (off by default), or HTTP requests

The device runs a WiFi Access Point. It is a **server**, never a client.

**Rationale:** Any outbound connection reveals the device exists, creates
interceptable metadata, creates a disruptable dependency, and enables
server-side coercion.

### 3. No Identifier Leaks

- WiFi AP BSSID derived from device identity (no manufacturer OUI leak)
- No probe responses containing manufacturer information
- BLE OFF by default (binary blobs not compiled in)
- No mDNS/SSDP/UPnP beyond local AP network
- GPS coordinates use configurable coarsening
- Presence detection hashes MACs — never stores or transmits raw MACs
- SD card files contain no filesystem-level device identifiers

### 4. Evidence Is Self-Verifying

An exported evidence bundle is verifiable by anyone:
- Without contacting ERRERlabs
- Without internet access
- Without proprietary software
- Using only the public key embedded in the export

Export bundle includes: signed records, complete hash chain, device
public key, self-contained HTML+JS verification page, and written
instructions for manual checking.

**Rationale:** If verification requires ERRERlabs, then ERRERlabs can be
shut down, compelled to return false results, or compromised.

### 5. Append-Only, Tamper-Evident

```
Each record: hash(prev_record || timestamp || payload)
Each record signed: Ed25519(private_key, record_hash)
Chain verification: any party can re-hash and verify entire chain
Gap detection: missing sequence numbers are detectable
Fork detection: divergent chains from same device are detectable
```

### 6. Privacy by Architecture, Not Policy

- BLE is not "disabled by policy" — the binary blobs are not compiled in
- Raw MACs are not "deleted after use" — they are hashed before storage
- GPS is not "anonymized in post-processing" — it is coarsened at capture
- Private keys are not "access-controlled" — there is no read interface
- Telemetry is not "opt-out" — the code to send it does not exist

Preference order: (1) Don't build it, (2) Build it so it can't leak,
(3) Give the user a physical/compile-time control.

### 7. Minimal Attack Surface

| Surface | Decision |
|---------|----------|
| Bluetooth | REMOVED at compile time (no binary blobs) |
| USB Serial | Disabled in production |
| JTAG | Disabled via eFuse |
| OTA | User-initiated only, signed binaries |
| Cloud | No outbound connections |
| mDNS | Local AP only |
| HTTP | TLS only (no plaintext fallback) |
| Camera | Preview only (evidence is metadata, not video) |

### 8. Cryptographic Minimalism

**Used:**
| Primitive | Purpose | Library |
|-----------|---------|---------|
| Ed25519 | Signatures (identity, record signing) | Arduino Crypto (rweather) |
| SHA-256 | Hashing (chain integrity, domain separation) | mbedTLS (ESP-IDF) |
| HMAC-SHA256 / HKDF | Key derivation (token generation) | mbedTLS (ESP-IDF) |
| RSA-2048 | TLS certificate (self-signed, local only) | mbedTLS (ESP-IDF) |

**Not used (and why):**
| Primitive | Reason |
|-----------|--------|
| RSA for signatures | Larger keys, slower, more implementation pitfalls |
| AES-CBC | Padding oracle attacks |
| HMAC-MD5 | MD5 is broken |
| SHA-1 | Collision attacks demonstrated |
| PBKDF2 | If password hashing needed, use Argon2id |
| Custom crypto | Never. Ever. For anything. |

**Future (when needed):**
| Primitive | Purpose |
|-----------|---------|
| X25519 | Key agreement (device pairing) |
| ChaCha20-Poly1305 | Authenticated encryption (encrypted export) |

### 9. Fail Secure, Not Fail Open

| Failure | Response |
|---------|----------|
| SD card full | Keep witnessing to RAM (short-term preservation) |
| Camera init fail | Continue all other witnessing |
| GPS no fix | Record events without location |
| Chain verify fail | Create tamper event, alert user, keep recording |
| Auth failure | Lock out with exponential backoff |
| TLS cert expired | Reject connection (no HTTP fallback) |
| Firmware corrupt | Refuse to boot (secure boot) |
| Watchdog trigger | Reboot, resume from last good state |
| NVS corruption | Generate new identity (fresh start) |

### 10. User Sovereignty

No one — not ERRERlabs, not law enforcement, not an employer — can:
- Remotely access, wipe, or disable the device
- Compel ERRERlabs to push a backdoored update
- Decrypt evidence without the device
- Forge evidence without the device's private key
- Determine if a specific person owns a specific device

The user CAN: factory reset, export evidence, change WiFi password,
physically destroy the device.

---

## Trust Boundaries

```
┌─────────────────────────────────────────────────────────┐
│                   TRUSTED BOUNDARY                       │
│                                                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  ESP32-S3    │  │  Ed25519     │  │  Witness     │  │
│  │  Hardware RNG│  │  Private Key │  │  Chain       │  │
│  │  (key gen)   │  │  (NVS only)  │  │  (append)    │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│                                                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  Secure Boot │  │  Flash       │  │  GPS Time    │  │
│  │  (firmware   │  │  Encryption  │  │  Source      │  │
│  │   verify)    │  │  (data at    │  │  (no NTP)    │  │
│  │              │  │   rest)      │  │              │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│                                                          │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│              ACKNOWLEDGED TRUST ASSUMPTIONS               │
│                                                          │
│  • Espressif ESP32-S3 silicon (Chinese manufacturer)     │
│  • Espressif WiFi binary blobs (closed-source)           │
│  • ESP32 ROM bootloader (Espressif, not modifiable)      │
│  • mbedTLS implementation (bundled with ESP-IDF)         │
│  • Arduino Crypto library (rweather, open-source)        │
│  • GPS satellite constellation (U.S. DoD operated)       │
│                                                          │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│                  UNTRUSTED / ADVERSARIAL                  │
│                                                          │
│  • All network traffic (even on device's own AP)         │
│  • All API clients (authenticate before trust)           │
│  • ERRERlabs (no special access by design)               │
│  • The user's phone/laptop (zero trust beyond TLS+auth)  │
│  • Physical environment (tamper detection, not prevent)   │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

---

## Attack Scenarios (Red Team)

### Scenario 1: Government demands evidence from ERRERlabs

**Attack:** Court order or national security letter compelling ERRERlabs
to produce user evidence.

**Result:** ERRERlabs has nothing. No keys, no data, no device identifiers,
no purchase records linking devices to users. Cannot comply even if
compelled.

**PASS if:** ERRERlabs literally cannot comply.

### Scenario 2: Adversary on same WiFi network

**Attack:** Adversary in WiFi range attempts to access device.

**Result:** Device runs its own AP — it is not on a shared network.
Adversary must know the AP password (device-unique, derived from
fingerprint). Even if connected: TLS encryption required, API auth
required, exponential backoff on failures.

**PASS if:** Network proximity alone grants no access.

### Scenario 3: Adversary obtains evidence export

**Attack:** Adversary acquires an exported evidence bundle.

**Result:** Export contains only public key and signed records. No private
key, no WiFi password, no device credentials. Adversary can read and
verify the evidence but cannot forge new evidence or compromise the device.

**PASS if:** Export leaks no secrets that compromise future evidence.

### Scenario 4: Adversary physically seizes device

**Attack:** Device is confiscated by adversary with forensic capability.

**Result:**
- Secure boot prevents firmware replacement
- Flash encryption prevents data extraction without device cooperation
- Factory reset destroys all data (user can trigger before seizure)

**LIMITATION:** A sufficiently resourced adversary with physical access
can potentially bypass ESP32 secure boot (active area of research).
SD card contents are readable if not encrypted at application layer.

**PARTIAL PASS:** Documented limitation. Future mitigations: application-layer
SD card encryption, RISC-V open silicon.

### Scenario 5: ERRERlabs pushes malicious firmware

**Attack:** ERRERlabs (voluntarily or under coercion) pushes a backdoored
firmware update.

**Result:**
- OTA updates are user-initiated, never automatic
- Update binary must be signed
- User can refuse any update with no consequence
- User can build from source and compare binary hash

**PASS if:** ERRERlabs cannot silently update any device.

### Scenario 6: Evidence forgery

**Attack:** Adversary attempts to create false evidence records.

**Result:** Requires Ed25519 private key (never leaves device) AND
re-hashing/re-signing every subsequent record in the chain.

**PASS if:** Forgery requires physical device possession AND key extraction.

### Scenario 7: Evidence deletion

**Attack:** Adversary deletes records from the chain.

**Result:** Chain gaps are detectable (missing sequence numbers). Even if
SD card is wiped, previously exported evidence remains valid and shows
the original chain state.

**PASS if:** Deletion is always detectable.

### Scenario 8: Supply chain compromise

**Attack:** Backdoored ESP32-S3 chips in manufacturing.

**Result:** Acknowledged risk. Mitigated by:
- No outbound connections (compromised chip has no exfiltration channel)
- Secure boot (firmware integrity verified)
- Flash encryption (data at rest protected)
- Future: open-source RISC-V silicon

**DOCUMENTED LIMITATION:** Disclosed to users in SECURITY_MODEL.md.

### Scenario 9: Correlation attack via WiFi AP

**Attack:** Adversary uses device's WiFi SSID to track owner across locations.

**Result:** SSID format "Canary-XXXX" reveals no manufacturer, serial, or
user identity. However, a persistent SSID is correlatable if observed in
multiple locations.

**Mitigation:** SSID suffix derived from device identity (not sequential).
Future: randomized SSID rotation with owner-only discovery.

**PARTIAL PASS:** Noted as area for improvement.

### Scenario 10: Timing side-channel on authentication

**Attack:** Adversary measures response time to infer token characters.

**Result:** Token comparison uses constant-time function (volatile XOR
accumulator). Response time is independent of how many characters match.
Additionally, exponential backoff limits brute-force attempts.

**PASS if:** Auth timing is constant regardless of input.

---

## Supply Chain Integrity

### Firmware Provenance
- All firmware built from public source (github.com/kmay89/securaCV)
- CI builds use pinned dependencies and deterministic toolchain
- Release binaries include SHA-256 hashes in release notes
- Users can build from source and compare binary hash
- Binary blobs: ESP32 ROM bootloader (Espressif) and WiFi firmware only

### Dependency Policy
- Minimize dependencies: Crypto lib, ArduinoJson, ESP32 Arduino Core
- Every dependency must have source available
- Pin exact versions in platformio.ini
- No dependency auto-updates
- Review changelogs before upgrading

### Hardware Trust
- ESP32-S3 manufactured by Espressif (China-based) — acknowledged boundary
- Mitigations: no outbound connections, secure boot, flash encryption,
  eFuse locks, no Espressif cloud services used
- Future: RISC-V based designs with open-source silicon

---

## Implementation Review Checklist

For every code change, verify:

### Cryptographic Review
- [ ] No new cryptographic primitives without justification
- [ ] No custom crypto implementations
- [ ] No key material in logs, exports, or API responses (except public keys)
- [ ] No downgrade paths (TLS to HTTP, signed to unsigned)
- [ ] Constant-time comparison for all secret-dependent operations

### Privacy Review
- [ ] No new outbound network connections
- [ ] No new identifier leaks (MAC, serial, OUI)
- [ ] No raw biometric/location data stored without coarsening
- [ ] No new data collection without user-visible disclosure
- [ ] Presence detection: verify MAC hashing, verify no SSID storage

### Trust Review
- [ ] No new trust assumptions introduced
- [ ] Evidence still verifiable without ERRERlabs
- [ ] Device still functions fully offline
- [ ] User still has complete sovereignty over device and data

### Attack Surface Review
- [ ] No new compile-time features enabled by default
- [ ] No new network services exposed
- [ ] No new USB/Serial/JTAG interfaces
- [ ] Binary size delta justified
- [ ] New dependencies audited (source available, maintained, no known CVEs)

---

## Weakening a Secure Default

To change any security-hardened default, a developer must:

1. Document the justification in the commit message
2. Add an entry to `firmware/LESSONS_LEARNED.md`
3. Get explicit approval referencing this threat model
4. Verify the change does not affect the most vulnerable user class
5. Update this document if the threat model changes

---

## Definition of Done (Security)

1. `SECURITY_MODEL.md` exists and is included in every evidence export
2. No outbound network connections exist in any code path
3. Ed25519 private key has no read/export interface of any kind
4. All cryptographic operations use vetted libraries (no custom crypto)
5. All security-sensitive defaults are hardened (see `secure_defaults.h`)
6. BLE binary blobs are not compiled in by default
7. TLS is required for all API access (no HTTP fallback)
8. Evidence is verifiable offline without any ERRERlabs service
9. Regression checks enforce all ten principles automatically
10. The transparency document passes the "would Moxie sign this?" test

---

## Mesh Layers (v0.2 — added 2026-05-11)

Three layered networks share the device's single 2.4 GHz radio. Each has a
distinct trust model, distinct cryptographic primitives, and distinct UI
treatment. Full audit: `docs/audit/mesh_and_chirp_audit_v1.md`.

### Opera mesh (household, trusted)

- Persistent device Ed25519 identity, shared `opera_secret` (32 B) symmetric
  among household devices.
- All frames Ed25519-signed; per-peer monotonic counter for replay
  protection (v0.2: wall-clock TTL retired per audit O1 — the counter is the
  authoritative freshness mechanism).
- `opera_secret` storage requires flash encryption enabled
  (eFuse `FLASH_CRYPT_CNT > 0`); load/save paths refuse on FE-off devices
  and log loudly (v0.2 audit O2).
- Peer removal auto-rotates `opera_secret` and invalidates existing sessions
  (v0.2 audit O3).

### Chirp channel (anonymous, community, soft-alert)

- Ephemeral session Ed25519 identity, regenerated on every enable.
- Cryptographic privacy firewall: session keys are NOT derived from device
  identity; chirp activity is unlinkable to Opera membership.
- v0.2 hardening:
  - End-to-end signature verification on every witness/ACK/suppress-vote
    (audit C1, C4, C5, C6).
  - `confirm_count` removed from wire; receivers track unique-pubkey
    confirmation sets locally (audit C2, C3).
  - Signed suppress voting wired (audit C7).
  - Wall-clock-anchored timestamps; origination refused when SNTP
    unsynced (audit C10, C15).
  - Per-pubkey rate-limit on incoming witnesses (audit C14).
  - REST API (`/api/chirp/*`) Bearer-token-gated when wired into
    `canary_wap.ino` (audit C12).
- Templates only, no free text. No "suspicious person", no "unfamiliar
  vehicle", no individual descriptions. `TPL_AUTH_FEDERAL_PRESENCE` removed
  in v0.2 (audit C17).

### Beacon channel (NEW v0.2 — supervised harm-reduction)

- Persistent device Ed25519 identity (same key as Opera/witness records).
- **Two-pubkey cryptographic co-signing on origination.** Every Beacon frame
  carries two Ed25519 signatures from two distinct device pubkeys. No
  single compromised device can originate a Beacon. Spec:
  `spec/beacon_channel_v0.md`.
- Narrow life-safety template set (~13 templates). No authority/government
  templates, no mutual-aid templates.
- CAP-aligned wire fields. Always `scope = Private`. Never IPAWS/WEA/EAS.
- NFPA-72-style supervised health state surface: `Normal | Trouble | Alarm |
  Supervisory`. Daily `BEACON_MSG_SELFTEST_OK` heartbeat.
- Append-only chain-hashed audit log of every Beacon received.
- CAP gateway interop (inbound and outbound) specified
  (`spec/beacon_cap_gateway_v0.md`) but not implemented in v0.

### Non-impersonation contract

CI-enforced via `scripts/lint_no_impersonation.sh`. The lint fails the build
if reserved emergency-broadcast phrases, the reserved two-tone audio pair,
or pure red as a primary alert color appear in any alert/chirp/beacon
firmware or UI source.

### Threats covered by mesh layers

| Threat | Opera | Chirp | Beacon |
|---|---|---|---|
| Spoofed origination | yes (Ed25519) | yes (Ed25519, v0.2) | yes (dual Ed25519) |
| Replay | yes (counter) | yes (1024-entry dedup + freshness) | yes (dedup + freshness) |
| Sybil flood | yes (opera_id isolation) | yes (per-pubkey rate + unique-pubkey set) | yes (two-pubkey rule + audit) |
| Physical extraction of opera_secret | yes (FE gate) | n/a | n/a |
| Compromised device originates fake alarm | n/a | partial (suppress vote) | yes (two-pubkey rule) |
| Reserved-tone or reserved-phrase impersonation | n/a | yes (lint) | yes (lint, color, frequencies) |
| Hawaii-style operator error | n/a | n/a | yes (two-person rule, msgType=Exercise distinct from Alert) |

### Outstanding work (tracked)

- Beacon REST endpoints (`/api/beacon/*`) not yet wired into `canary_wap.ino`
  (deferred; gated behind `FEATURE_BEACON_CHANNEL`).
- Full transactional opera_secret rekey ACK protocol (v0.2 implements the
  minimum-correct in-memory rotation; spec §5.6 documents the full flow for
  v0.3).
- Beacon pairing transport encryption.
- Bloom-filter nonce dedup for Chirp.
- Persistent origin signature across Chirp relays.
