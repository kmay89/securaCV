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
- A whistleblower who needs tamper-evident records
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
| **Systemic** | Compromised update channel, supply chain backdoor, manufacturer coercion | Silent, persistent, potentially affecting all devices | Owner-controlled signed OTA (Auto Update an explicit opt-in), reproducible builds, open source |

---

## The Ten Security Principles

### 1. Keys Never Leave the Device

The Ed25519 private key is generated on the ESP32-family chip's hardware RNG
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

#### Scope of this principle

Every item above is a statement about
*software* paths. It is not a claim that the key survives an adversary who
takes the device to a bench: flash encryption is an opt-in tier, so on a
default Canary the key is recoverable by reading the flash directly. What
that does and does not buy an attacker — forward forgery yes, rewriting
anchored history no — is stated in
[`SECURITY_MODEL.md`](SECURITY_MODEL.md#physical-extraction-and-the-flash-encryption-default).
Reviewers should hold this principle to the software boundary and treat
physical extraction as the separately-documented trade it is.

### 2. Zero Phone-Home

**Nothing outbound that is not disclosed and named.** No Canary
contacts ERRERlabs, a cloud, an analytics or crash-report service, and a
WAP used as its own access point opens no outbound socket at all: it runs
a WiFi Access Point and is a **server**, never a client. A product on a
home network opens exactly these paths, and nothing else:

- **The MQTT broker the owner points it at** (sense, vision, the
  `firmware/canary` flagship, the WAP's bridge) — plain by default;
  CA-verified or SHA-256-pinned TLS when provisioned, refusing to connect
  on an incomplete setup rather than downgrading
  (`firmware/common/network/mqtt_transport_logic.h`, host-tested)
- **A daily, jittered signed-manifest check** on every product with
  pull-OTA — an anonymous HTTPS GET that installs nothing; Install is a
  button and the per-device Auto Update switch an explicit opt-in
  (`docs/firmware_ota.md`; the fetch's URL policy is pinned by
  `firmware/common/ota/test_ota_logic.cpp`)
- **SNTP** on the display line — the flagship and the WAP take time from
  GPS; sense and vision have no clock source of their own (no GPS, no
  SNTP; sense stamps `ts_ms` from uptime); the displays have no GPS.
  Two host literals in the display firmware's `tz_auto.cpp`
  (`pool.ntp.org`, `time.nist.gov`), with no host test of their own
- **The opt-in standalone forecast** on the two 7" displays, switched on
  at the glass only and pinned by `firmware/tests_host/test_wx_core.cpp`

DNS resolves only those hosts. There is no telemetry, no cloud sync and no
other HTTP request. The user-facing list, with each path's test, is
[`SECURITY_MODEL.md`](SECURITY_MODEL.md#the-networked-products-disclosed-outbound-paths).

**Rationale:** Any outbound connection reveals the device exists, creates
interceptable metadata, creates a disruptable dependency, and enables
server-side coercion — so each one is a listed, reviewed exception, never a
default.

**Review rule:** a new outbound path is a change to this list and to the
matching entry in `SECURITY_MODEL.md`, with a host test pinning its request
shape as `test_wx_core.cpp` does for the forecast.
`firmware/scripts/regression_check.sh` greps the trees for a new client
socket and points here.

### 3. No Identifier Leaks

- The AP's name ("Canary-XXXX") carries no manufacturer identity or
  serial. Its BSSID is the radio's factory MAC and carries Espressif's
  OUI: nothing in the firmware sets a derived or random address, so the
  radio discloses the chip vendor, as every ESP32 does, and nothing more
- No probe responses containing manufacturer information
- BLE advertising carries the device's own name and the SecuraCV service
  UUID; the fleet beacon and Chirp add a manufacturer-data field under
  the Bluetooth SIG's reserved test id `0xFFFF` holding type, flags,
  battery, health, chain height and the two fingerprint bytes already in
  the name (`fleet_beacon.h`) — never a serial. The advertising address
  is the chip's public BLE address (Espressif's OUI), the same disclosure
  as the Wi-Fi radio's factory MAC. BLE is compiled into the WAP's FULL
  and DEV profiles (MINIMAL compiles it out); the `firmware/canary`
  flagship builds it out of `release` / `release_ha`, and its `full` env
  compiles the Scout scanner and the GATT status service
- mDNS (`_securacv._tcp`) only on the network the device is on — its own
  AP, or the LAN it was joined to; no SSDP/UPnP
- The CSI HAL holds one identifier, the BSSID of the router it is
  associated with, only to keep other transmitters out of its window; it
  is compared in place, never copied into a slot, stat, log line or wire
  format, and wiped on `deinit()`
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

- Broker TLS is not "recommended" — an incomplete TLS setup refuses to
  connect; there is no fallback to plain to be talked into
- Raw MACs are not "deleted after use" — they are hashed before storage
- GPS is not "anonymized in post-processing" — it is coarsened at capture
- Private keys are not "access-controlled" — there is no read interface
- Telemetry is not "opt-out" — the code to send it does not exist

Preference order: (1) Don't build it, (2) Build it so it can't leak,
(3) Give the user a physical/compile-time control.

### 7. Minimal Attack Surface

| Surface | Decision |
|---------|----------|
| Bluetooth | BLE (NimBLE) in the WAP's FULL profile (and, reduced to the pairing channel — with the BLE OTA and the presence listener that ride it — and the status service, in DEV; MINIMAL compiles it out), gated on `HW_HAS_BLE`: owner pairing / provisioning, the GATT status service, signed BLE OTA v2, the Scout scanner that attributes rooms to the owner's own paired beacons by hashed MAC, and two passive scanners — BLE presence (listen-only, feeding the same presence pipeline as the WiFi one; no MAC, OUI or name kept from the scanner) and Nearby discovery of other Canaries by service UUID (everything else counted only). Nothing classes or tracks other people's devices. The S3 / C3 the BLE-bearing builds run on have no Classic (BR/EDR) radio; the classic-ESP32 boards have one and no build compiles a stack for it. The `firmware/canary` flagship's `release` / `release_ha` images have `FEATURE_BLE_STATUS=0` / `FEATURE_BLE_SCAN=0`; its `full` env compiles the Scout scanner and the GATT status service (`securacv_ble_scan`, `securacv_ble_status`), the only BLE code in that tree |
| USB Serial | Disabled in production |
| JTAG | Disabled via eFuse |
| OTA | Owner-initiated by default: the device checks a signed manifest daily and installs nothing; Install is a button, and the per-device Auto Update switch is an explicit owner opt-in (off by default). Every install verifies the Ed25519 release signature and the version floor first. The WAP's BLE OTA (protocol v2) puts product and version under the release signature and enforces the same anti-rollback floor as the pull path; a downgrade or a legacy v1 header needs the owner's BOOT-button break-glass and is logged as a bypass (`docs/firmware_ota.md`) |
| Cloud | None. The outbound paths are the four in Principle 2 — broker, signed-manifest check, SNTP, opt-in forecast — to hosts the owner chose |
| mDNS | Local network only: the device's own AP, or the home LAN once joined (`_securacv._tcp`) |
| Setup Wi-Fi (SoftAP) | Device-unique passphrase, max 1 client. Since 2026-09 (F16) both firmware trees ask the driver for WPA2/WPA3 transition with PMF capable (never required, so a WPA2-only phone still joins) and fall back to WPA2-PSK where the core lacks SoftAP SAE — expected on the IDF 4.4 core the `canary (PIO)` dev/release builds use — reporting what is on the air as `ap_auth` in `/api/wifi/status` and `/api/status` (WAP: `/api/wifi`, `/api/device-info`). CI-compiled (#1704), never run on hardware. The `canary (PIO)` passphrase is 8 characters, the WPA2 floor; widening it is a separate decision because it is re-derived every boot and would change every provisioned device's password |
| HTTP | Plaintext on the LAN by default (token-authenticated); TLS is an owner opt-in on the WAP (`tls_enabled`) and the kernel (`api-tls` feature) — not "TLS only". The `canary (PIO)` dev/full builds (`FEATURE_HTTPS=1`, 2026-09) serve a self-signed ECDSA P-256 certificate on 443 from the first boot after setup and redirect port 80 there; release builds stay plaintext until the size budget is read, and a build or core that cannot do TLS falls back to plaintext and says why in `/api/status` `tls_mode_reason` — CI-compiled (#1704), never run on hardware. The iPhone app pins the receipt's `tls_cert_fp` (`PinnedTrustDelegate`), refuses an https device it cannot check, and offers a plain-http credential push only behind a disclosure the owner switches on |
| MQTT broker link | Plain by default — the broker password crosses the LAN in the clear until the owner provisions TLS, and on the flagship's release images the provisioning request itself (`POST /api/mqtt/config`) rides the plain device API. CA-verified or SHA-256-pinned TLS per product, refusing to connect on an incomplete setup; the lab mode is chosen by name and warns on every connect. Per-variant table: `docs/FIRMWARE_VARIANT_AUDIT.md`. Compile-tested, host-tested, not bench-tested against a TLS broker |
| Fleet roll-call (`GET /api/fleet`) | The one open read on the hub: rate-limited, no token. It serves the kernel's own row and — when `api.fleet_peers_path` is set — each Canary the MQTT bridge heard, in the contract's coarse words only (name, online, chain verdict, product, and presence/occupants/breathing while proven online); never an event, a zone or key material. The origin allow-list stops other websites' scripts, not a client that can reach the port, so the port stays loopback by default and is the owner's to expose (the add-on's disabled 8799 host port; the Docker sidecar's `SECURACV_API_BIND=all` plus a port mapping the image does not `EXPOSE` — the switch also exports the kernel's cleartext acknowledgment, which never relaxes the token on any other route). What the roll-call says is bounded by the MQTT broker, not proven past it: a peer with publish rights can replay a captured signed publish (held to one window per missed chain advance), invent ids, or put a real id into `degraded` — see `tvos/discovery/DISCOVERY.md`. The summary file behind it is `0600` and size-bounded |
| Viewer credential (`GET /api/sealed-log`, the Witness Wall) | A second, narrower bearer credential beside the rotating capability token, because a TV cannot re-read a token file: minted by the operator (`witness_api mint-viewer-token`, printed once as a pairing receipt that also carries the kernel's verifying key for the Wall to pin), long-lived until revoked by id, and honored on exactly one route — the non-queryable, size-capped, signed sealed-log tail (Invariant VII bounds what it reads). Presented on any other path or method it is an invalid token that counts toward the per-address lockout, and a good viewer read never clears that count (only a capability-token success does), so the narrower credential cannot reset the lockout that guards the wider one; `?token=` is refused for it as for every token. At rest only its sha256 (`viewer_tokens.json`, `0600`, beside the capability token), compared in constant time and re-read per request, so a revocation lands on the next poll. Whoever holds it reads the coarse sealed record (event types, zones, 10-minute buckets) — the reason it is a credential and not an open read like the roll-call — and over plaintext HTTP it can be sniffed on the LAN like the capability token; TLS stays the owner's opt-in (`api-tls`). What the Wall's "Verified" proves is *authorship* — every served entry signed by the pinned key and linked from the served anchor — not that the tail is *current* or *complete*: the served `checkpoint_head` is unsigned and the document carries no signed time or high-water mark, so a captured genuine document replayed later, or a genuine one cut short at either end, still walks clean against the pin, and the Wall keeps no last-walked head across polls. A signed head (or high-water mark) in the document is the roadmap item that closes it; not built |
| Camera | Preview only (evidence is metadata, not video) |

### 8. Cryptographic Minimalism

**Used:**
| Primitive | Purpose | Library |
|-----------|---------|---------|
| Ed25519 | Signatures (identity, record signing) | Arduino Crypto (rweather) |
| SHA-256 | Hashing (chain integrity, domain separation) | mbedTLS (ESP-IDF) |
| HMAC-SHA256 / HKDF | Key derivation (token generation) | mbedTLS (ESP-IDF) |
| RSA-2048 | TLS certificate (self-signed, local only) — canary-wap | mbedTLS (ESP-IDF) |
| ECDSA P-256 | TLS certificate (self-signed, local only) — `canary (PIO)` `FEATURE_HTTPS` builds | mbedTLS (ESP-IDF) |

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
| TLS client cannot verify the WAP's self-signed certificate | The iPhone app pins the receipt's `tls_cert_fp` and refuses an https Canary whose certificate does not match or that has no pin on record (`DeviceAPI.swift`, `PinnedTrustDelegate`); browsers show their self-signed warning. No API asks the client for a certificate — the kernel's rustls config is `with_no_client_auth()` and the WAP's `httpd_ssl_config_t` carries only the server certificate and key |
| HTTPS server fails to start on the WAP | Serves plain HTTP and logs `[HTTPS] Server start FAILED — falling back to HTTP`; setup mode is HTTP-only by design (both stated, never silent) |
| Firmware corrupt | Refuse to boot **on the opt-in secure-provisioning tier only** (secure boot is NOT enabled on default builds — see [Scope of this principle](#scope-of-this-principle)); the default tier relies on the OTA signature check before the image is written |
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
│  │  ESP32 family│  │  Ed25519     │  │  Witness     │  │
│  │  Hardware RNG│  │  Private Key │  │  Chain       │  │
│  │  (key gen)   │  │  (NVS only)  │  │  (append)    │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│                                                          │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  Secure Boot │  │  Flash       │  │  GPS Time    │  │
│  │  (firmware   │  │  Encryption  │  │  (SNTP on    │  │
│  │   verify)    │  │  (data at    │  │  glass only) │  │
│  │              │  │   rest)      │  │              │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│                                                          │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│              ACKNOWLEDGED TRUST ASSUMPTIONS               │
│                                                          │
│  • Espressif ESP32-family silicon (Chinese manufacturer) │
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

### Audit Boundary vs Security Boundary

The hardware/firmware boundaries above describe *where data is trusted*. The
kernel software adds a second, orthogonal distinction that auditors must keep
straight: an **audit boundary** is a code contract you must *manually verify*,
whereas a **security boundary** is *mechanically enforced* and fails closed even
against a malicious actor on the wrong side of it. Conflating the two is the most
common way to misread this codebase's privacy guarantees.

**Audit boundaries** are the out-of-TCB producer surfaces. They run *outside* the
kernel's trusted computing base and are treated by the threat model as careless or
malicious; nothing about the trait *prevents* a misbehaving implementation from
retaining or exporting raw bytes — conformance is established by manual review,
not by the type system or the runtime:

| Audit boundary | Where | Contract that MUST be hand-audited |
|----------------|-------|-------------------------------------|
| `detect::backend::DetectorBackend` | [`src/detect/backend.rs`](../../src/detect/backend.rs) | Receives raw pixels; must not store or export them beyond the `detect` call. |
| `InferenceView` → backend handoff | [`src/frame.rs`](../../src/frame.rs) (`run_detector`) | Forwards pixels to the configured backend; the view restricts, but cannot enforce, downstream handling. |
| `adapter::SensorAdapter` | [`src/adapter/mod.rs`](../../src/adapter/mod.rs) | Untrusted producer of vendor-neutral `Claim`s; must never retain raw media or emit identity / precise time / precise location. |

**The security boundary** is, and remains, the three fail-closed gates inside
`Kernel::append_event_checked` — the single choke point every producer (modules,
adapters, the Frigate bridge) passes through. These are enforced in code and record
a `FailureEvent` on rejection:

1. **Event-type allowlist** — a producer (via its `ModuleDescriptor`) may emit only
   its declared `EventType`s; anything else is rejected.
2. **Contract Enforcer** — confidence bounds, 10-minute time-bucket coarsening,
   strict `^zone:[a-z0-9_-]{1,64}$` zone allowlist, correlation-token constraints.
3. **Zone policy** — operator-designated sensitive zones are rejected.

The practical consequence: an audit-boundary component adds **breadth of producers,
never new query surface or new privilege**. A malicious backend or adapter can
produce *garbage claims*, but as an out-of-TCB producer it reaches the log **only**
through `append_event_checked`, so every such claim still passes all three gates —
and the adapter host (`AdapterHost`) exposes no lower-level write that skips them.

This is a property of the **producer** boundary, not an absolute property of the
`Kernel` type. The Kernel is itself the TCB, and it does have lower-level append
methods — e.g. `append_event_with_failure_semantics` (which `append_event_checked`
calls as its final write step, and which the failure-event path reuses) takes an
already-built `Event` and does **not** re-run the gates. Those are trusted-base APIs:
reachable only by code that already holds a `&mut Kernel` and can mint arbitrary
`Event`s — code that is *inside* the boundary by definition — never a bypass exposed
to a producer. The three gates defend the line where untrusted input crosses into the
kernel; they are not, and do not claim to be, a sandbox around the trusted kernel
itself (that is what `CapabilityBoundaryRuntime` and the optional seccomp sandbox are
for).

**Hardening an audit boundary into a security one (optional).** The realistic attack
surface in an adapter is parsing attacker-controlled bytes (MQTT payloads, webhook
bodies, NVR JSON). With the `adapter-sandbox` feature and `with_sandbox(true)`, that
parse step runs in a forked **seccomp** sandbox that physically cannot open files or
sockets (`adapter::sandbox::parse_in_sandbox`), upgrading the parse step from an
audit boundary toward a real security boundary. It is opt-in for adapters; module
sandboxing via `CapabilityBoundaryRuntime` is mandatory, not optional.

> Canonical sources: [`spec/sensor_adapter_contract_v0.md`](../../spec/sensor_adapter_contract_v0.md) §4,
> [`kernel/architecture.md`](../../kernel/architecture.md) ("the adapter trait is an audit boundary;
> the Contract Enforcer is the security boundary"), and the per-trait `# Audit Boundary` doc comments
> in the code referenced above.

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
fingerprint). Even if connected: API auth required (on the flagship a peer
on its own AP is handed the page token, so there the AP password is the
whole boundary), exponential backoff on failures, and HTTPS once setup has
completed (plain HTTP during setup, on the flagship's release images — its
dev and full builds redirect port 80 to HTTPS from the first boot after
setup — and on the display line's LAN page).

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
- **Default tier: FAIL.** Secure boot and flash encryption are an opt-in
  secure-provisioning build (`firmware/provisioning/sdkconfig.defaults.secure`
  ships them commented out; so does `canary-ota/sdkconfig.production`). On a
  default Canary the flash — identity key included — is readable on a bench.
  What that buys an attacker (forward forgery yes, rewriting anchored history
  no) is stated in [SECURITY_MODEL.md](SECURITY_MODEL.md#physical-extraction-and-the-flash-encryption-default).
- **Secure tier: PARTIAL PASS.** Secure boot prevents firmware replacement;
  flash encryption prevents data extraction without device cooperation.
- Factory reset destroys all data (user can trigger before seizure) — both tiers.

**LIMITATION:** A sufficiently resourced adversary with physical access
can potentially bypass ESP32 secure boot (active area of research).
SD card contents are readable if not encrypted at application layer.

**PARTIAL PASS:** Documented limitation. Future mitigations: application-layer
SD card encryption, RISC-V open silicon.

### Scenario 5: ERRERlabs pushes malicious firmware

**Attack:** ERRERlabs (voluntarily or under coercion) pushes a backdoored
firmware update.

**Result:**
- OTA is owner-initiated by default: the device checks a signed manifest
  daily and installs nothing; Install is a button, and the per-device Auto
  Update switch is an explicit opt-in (off by default)
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
- Secure boot (firmware integrity verified) — opt-in secure-provisioning tier only
- Flash encryption (data at rest protected) — opt-in secure-provisioning tier only
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
- Mitigations: no outbound connections, no Espressif cloud services used;
  secure boot, flash encryption and eFuse locks on the opt-in
  secure-provisioning tier (NOT on default builds)
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
2. Every outbound path is one of the disclosed set in Principle 2, each
   named with its host test or its lack of one; `regression_check.sh`
   greps for a new client socket
3. Ed25519 private key has no read/export interface of any kind
4. All cryptographic operations use vetted libraries (no custom crypto)
5. All security-sensitive defaults are hardened (see `secure_defaults.h`)
6. BLE is compiled only where a profile names it (the WAP's FULL and DEV
   profiles, gated on `HW_HAS_BLE`; the flagship's `full` env, Scout
   scanner and status service only); its adverts carry the device's own
   name and fleet-beacon state, never a serial or anything about other
   people, and its two passive scanners (presence, Nearby) keep no MAC,
   OUI or name from other people's devices
7. The WAP serves HTTPS after setup, and the flagship's dev and full builds
   from the first boot after setup; plain HTTP is a stated posture (setup
   mode and start failure on the WAP, both logged; the flagship's release
   images, and its dev and full builds during setup and on a TLS start
   failure, named in `tls_mode_reason`; the displays' LAN page), never a
   silent downgrade of a TLS session
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
  and log loudly (v0.2 audit O2). That keeps the secret off un-fused
  boards; it does **not** make it confidential at rest on fused ones.
  Flash encryption does not cover NVS (ESP-IDF encrypts only the app,
  OTA-data and NVS-key partitions), so on an FE-on board the persisted
  `opera_secret`, trusted peers, replay counters and hub election are
  still plaintext on the flash chip. They would be ciphertext only under
  NVS encryption, which is not available under `framework = arduino`
  (roadmap item 9 in `firmware/ESP32S3_OPTIMIZATION_ROADMAP.md`).
- The device's **own identity key** is deliberately not gated the same way:
  it exposes only this device, and Tier 0 of the
  [root-of-trust ladder](../design/hardware_root_of_trust.md) keeps it in
  NVS by decision (§8 #1/#3/#4). The PIO canary image (`firmware/canary`)
  reports the posture live as `key_at_rest` (`plaintext-nvs` /
  `nvs-encrypted` / `nvs-encrypted+secure-boot`) in `/api/status` and the
  self-manifest — `plaintext-nvs` on every board today, fused or not, for
  the reason above; canary-wap and the other trees do not report it yet.
  Only images built with `SECURACV_REQUIRE_FLASH_ENCRYPTION=1` (Tier 3+)
  refuse to store or load it, and they refuse unless NVS is actually
  encrypted — so, under `framework = arduino`, on every board. The
  decision is written once in `firmware/common/identity/key_at_rest.h`.
- Peer removal auto-rotates `opera_secret` and invalidates existing sessions
  (v0.2 audit O3; on canary-wap that is now spec v0.3's transactional flow,
  #454, and on the PlatformIO tree the variant below — "Outstanding work"
  has what neither has proven yet).
  - PlatformIO tree (spec §5.6 PlatformIO subsection, `mesh_rekey.{h,cpp}`):
    no per-peer session keys exist there, so the rotation runs an
    ephemeral X25519 exchange per removal inside signed envelopes, the ACK
    going out under the old `opera_id` before a survivor switches; a
    survivor that misses the 60 s window is dropped and re-pairs. There,
    `opera_id` is a cleartext header field and `opera_secret` buys nothing
    else, so the removed device is shut out by each survivor
    **unregistering its pubkey**, not by the new secret. Since F33 a device
    unregisters it as soon as it verifies an OFFER naming it, so only a
    survivor that missed every copy of the OFFER for the whole window keeps
    trusting the removed device, indefinitely and with no signal. The
    rotation's own gain is that every pre-removal frame is dead to the
    survivors that switched, across a re-pair too. Two removals made on two
    devices at once converge on one secret (F33: a settle window before any
    secret goes out, the lower initiator fingerprint wins, every OFFER's
    removal holds on every device that hears it); an initiator that already
    handed out its secret before hearing the other OFFER, or two devices
    removing each other, can still split the household, and a host
    random-loss probe still split a few percent of runs. Host-tested;
    **maintainer crypto review and the U1 Track C3 bench pass are pending**.
  - The §5.6 `REVOCATION_GRACE_MS` deny-list (F33, both trees,
    `mesh_revocation.{h,cpp}` staged byte-identical): a removed device's
    pairing attempts are refused for 7 days, persisted FE-gated with the
    grace left (time powered off does not count down). The PlatformIO tree
    also deny-lists the device named by every verified OFFER; canary-wap
    records only removals made on itself, because its rotation message does
    not name the removed device, and two concurrent removals on canary-wap
    devices can still split that household (a wire change). Host-tested;
    crypto review and bench pending.

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
- **Two-pubkey cryptographic co-signing on origination.** A standard Beacon
  ALERT/UPDATE/CANCEL frame carries two Ed25519 signatures from two distinct
  paired device pubkeys. The exception is the solo-degraded path (spec §6.2):
  a frame flagged `BCN_FLAG_SOLO_ORIGIN`, with `certainty = Observed` and
  originator == cosigner, carries one key's signature in both slots. Those
  three are the only solo rules a receiver checks — they are what is on the
  wire — and receivers accept such a frame from any non-revoked set member,
  shown downweighted. The BOOT-button hold and the "no fresh paired
  cosigner" rule are enforced by the originating firmware, not on receive.
  So they stop a caller of the device's REST API who is not physically at
  the device. They do not stop anyone holding that device's Ed25519 private
  key (readable from flash on the default tier — Scenario 4 above): with it,
  a solo ALERT, or a solo CANCEL naming a live alarm's clear-text header
  nonce, can be signed on any ESP32 in radio range, and every set member in
  range accepts it. Nor do they stop software running on the device itself.
  Recovery is revoking the key (spec §3.4; listed in spec §14.2 "Not
  mitigated"). Spec: `spec/beacon_channel_v0.md`.
- Receivers accept a frame only when both signers resolve to a non-revoked
  beacon-set member — or to the receiving device itself: a set holds peers
  only, so the device that co-signed an alarm resolves its own fingerprint to
  its own pubkey and holds that alarm, which is what lets it cosign the
  alarm's CANCEL (spec §6.5, §7.1 step 5). That slot is still verified
  against the device's own key, so only a frame it really signed gets
  through, and two distinct fingerprints are still required.
- Co-sign requests and responses (`COSIGN_REQ`/`COSIGN_RESP`) are encrypted
  to the peer — X25519 ECDH, a domain-labeled SHA-256 key, ChaCha20-Poly1305
  — with the clear routing fields (fingerprints, length, the `accept` byte)
  bound as associated data and an all-zero shared secret refused (spec §6.3).
- Narrow life-safety template set (~13 templates). No authority/government
  templates, no mutual-aid templates.
- CAP-aligned wire fields. Always `scope = Private`. Never IPAWS/WEA/EAS.
- NFPA-72-style supervised health state surface: `Normal | Trouble | Alarm |
  Supervisory`. Daily `BEACON_MSG_SELFTEST_OK` heartbeat.
- Append-only chain-hashed audit log of every Beacon received. Two storage
  tiers (AGENTS.md *Beacon channel invariants* item 9): the log of record is
  `/beacon/audit.jsonl` on SD — pure append, never pruned/truncated/rotated,
  tamper-evident via the embedded signatures and chain hashes — with a
  64-entry NVS ring as the bounded recent-view cache. The ring is written
  only on a board with flash encryption on (canary-wap's
  `beacon_channel.cpp` uses the same gate as O2; an un-fused board keeps
  the ring in RAM only), and it is plaintext NVS there too: flash
  encryption does not cover NVS, only NVS encryption would, and that is
  not available under `framework = arduino` (the O2 bullet in the Opera
  mesh section above). The
  chain head spans every entry ever appended, so continuity stays provable
  past the ring boundary; SD-less devices keep chaining and raise a one-time
  `STORAGE` health warning.
- CAP gateway interop (inbound and outbound) specified
  (`spec/beacon_cap_gateway_v0.md`) and deferred by decision. Until its human
  gates are met (trust root, separately named build, per-deployment legal
  review — gateway spec §6), a gateway-trust key is an ordinary two-pubkey
  signer with no extra privilege; two host tests pin that, one of them
  against the real receive path's source.

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
| Physical extraction of opera_secret | partial (the FE gate keeps it off un-fused boards; NVS is not encrypted, so a fused board still holds it in plaintext) | n/a | n/a |
| Compromised device originates fake alarm | n/a | partial (suppress vote) | yes (two-pubkey rule) |
| Reserved-tone or reserved-phrase impersonation | n/a | yes (lint) | yes (lint, color, frequencies) |
| Hawaii-style operator error | n/a | n/a | yes (two-person rule, msgType=Exercise distinct from Alert) |

### Outstanding work (tracked)

- Beacon channel runtime not wired into the `canary_wap` loop: the REST
  routes are registered (behind `FEATURE_BEACON_CHANNEL`, Bearer-gated), but
  nothing calls `beacon_channel::init()`, `set_enabled()`, `update()` or
  `dispatch_espnow_message()`, and the shared ESP-NOW receive path forwards
  to Chirp only. Wiring it needs an explicit user opt-in and a `COSIGN_REQ`
  small enough for the 250-byte receive buffer (today 310 bytes).
- `opera_secret` rotation on peer removal is implemented in both firmware
  trees. canary-wap runs spec §5.6's
  transactional flow (#454: the new secret under each survivor's session
  key, the ACK sent under the old `opera_id`, commit on every ACK or at
  60 s, an unacked peer marked `PEER_STALE`); the PlatformIO tree runs its
  own ephemeral-X25519 variant over signed envelopes (#1704, the Opera
  section above). Neither has run on a radio: the PlatformIO rotation is
  host-tested two-sided (`test_mesh_rekey`), and canary-wap's host test
  restates the commit rule on a mirror, not the sketch's own code. Open: the PlatformIO
  rotation's maintainer crypto review; the bench passes (the checklist's O3
  row; `docs/hardware/v1_bench_validation_runbook.md` Track C3); the §5.6
  `REVOCATION_GRACE_MS` deny-list (F33: in both trees, host-tested, the
  Opera section above), and convergence of two concurrent removals on
  canary-wap, which needs a wire change; and a pairing that
  completes on a device, which both rotations need first — #1704 found that
  both trees ran pairing's X25519 on Ed25519-generated keys (fixed by F33:
  clamped X25519 ephemerals in both trees, host-tested against a real X25519
  — crypto review pending) and that the PlatformIO transport's peer table was
  never populated outside host tests (fixed by F33: each trusted peer's radio
  MAC is bound from pairing and NVS, spec §8.3; host-tested, not yet seen on
  a bench). canary-wap's AUTH session keys still run X25519 over the
  long-term Ed25519 keys (spec §5.3 note).
- Beacon pairing flow (spec §3.3: `PAIR_OFFER`, ephemeral X25519 +
  confirmation code) is a stub. Nothing writes a beacon-set entry or a peer's
  X25519 key, so the two-device co-sign path — whose transport is encrypted —
  has no reachable input on any device.
- Bloom-filter nonce dedup for Chirp.
- Persistent origin signature across Chirp relays.
