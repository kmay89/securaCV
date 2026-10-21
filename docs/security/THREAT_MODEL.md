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
| USB Serial | Enabled in release images, not disabled: the published ESP32-S3 builds set `CDCOnBoot=cdc`, so `Serial` is the USB console (the C3 and C6 boards' USB-Serial/JTAG provides it on its own), and `scripts/lint_usb_console.py` holds every publishing workflow to the bench builds. The console takes the serial commands, and the WAP prints its provisioning receipt there at every boot, the bearer token and the SoftAP password included. Reading it takes the cable in the unit's USB port, which is physical access: it is the owner's way back to the token when no client of theirs can win the BOOT tap (F342) |
| JTAG | Disabled via eFuse |
| OTA | Owner-initiated by default: the device checks a signed manifest daily and installs nothing; Install is a button, and the per-device Auto Update switch is an explicit owner opt-in (off by default). Every install verifies the Ed25519 release signature and the version floor first. The WAP's BLE OTA (protocol v2) puts product and version under the release signature and enforces the same anti-rollback floor as the pull path; a downgrade or a legacy v1 header needs the owner's BOOT-button break-glass and is logged as a bypass (`docs/firmware_ota.md`) |
| Cloud | None. The outbound paths are the four in Principle 2 — broker, signed-manifest check, SNTP, opt-in forecast — to hosts the owner chose |
| mDNS | Local network only: the device's own AP, or the home LAN once joined (`_securacv._tcp`) |
| Setup Wi-Fi (SoftAP) | Device-unique passphrase, max 1 client. Since 2026-09 (F16) both firmware trees ask the driver for WPA2/WPA3 transition with PMF capable (never required, so a WPA2-only phone still joins) and fall back to WPA2-PSK where the core lacks SoftAP SAE — expected on the IDF 4.4 core the `canary (PIO)` dev/release builds use — reporting what is on the air as `ap_auth` in `/api/wifi/status` and `/api/status` (WAP: `/api/wifi`, `/api/device-info`). CI-compiled (#1704), never run on hardware. Both trees' passphrase is `cv-` + 12 characters (15), an HMAC of the device private key; the `canary (PIO)` one was `cv-` plus 5 characters derived from the published key fingerprint (8 in all) until sweep F350, which moved every unit's key once, on update, with no marker keeping the old one (host-tested; not run on a device) |
| HTTP | Plaintext on the LAN by default (token-authenticated); TLS is an owner opt-in on the WAP (`tls_enabled`) and the kernel (`api-tls` feature) — not "TLS only". The `canary (PIO)` dev/full builds (`FEATURE_HTTPS=1`, 2026-09) serve a self-signed ECDSA P-256 certificate on 443 from the first boot after setup and redirect port 80 there; release builds stay plaintext until the size budget is read, and a build or core that cannot do TLS falls back to plaintext and says why in `/api/status` `tls_mode_reason` — CI-compiled (#1704), never run on hardware. The WAP's dashboard session cookie (`cv_session`: `HttpOnly`, `SameSite=Strict`, 24 h) carries `Secure` whenever it is set over the HTTPS server, so the browser never sends it to the port-80 redirect in the clear; the first-boot setup and a failed HTTPS start serve plain HTTP and set it without `Secure`, since a browser drops a `Secure` cookie that an http page sets (host-tested, not run in a browser against a device). The iPhone app pins the receipt's `tls_cert_fp` (`PinnedTrustDelegate`), refuses an https device it cannot check, and offers a plain-http credential push only behind a disclosure the owner switches on |
| WAP provisioning receipt (`GET /api/provisioning-receipt`) | The one answer that carries the WAP's bearer token, its SoftAP password and its TLS pin. Decided in this order: a valid bearer (the `Authorization` header alone, under the per-address lockout, F343); then the Bluetooth setup door's claim, spent only on a match (a wrong, short or truncated `?claim=` changes nothing, an expired one is burned; F342); then the BOOT tap, served only to the sole address that asked for the receipt from 10 s before the tap onward, so a second address closes the tap and an asker gets `409 tap_contended` while an address other than it asked within the last 10 s, up to the closed tap's 30 s (it ends 10 s after the other device stops; under a host that keeps polling, the poller's own asks end it 10 s after the owner's page stopped, the owner's retry hears `403`, and the next tap is closed again); the `409`'s hint says to start the request first and keep it asking, then press BOOT, never a bare re-tap, and the `403`'s hint and button, the dashboard's and the companion's `409` text (the click, or the tap on Save, before the press), the dashboard's timeout line and the companion's steps give the same order, and both pages say to press only once the device has answered one of their asks with `403` (F342). The WAP asks no Host question on this route yet (the ledger's F344). The asker is the socket peer (`request_peer_key()`, `getpeername`), never a header. Residuals: a tap no client of the owner's was asking for (an accidental tap, a page or `curl` started after it, a tap pressed per a recipe or an older page that says to press first, BLE OTA break-glass) goes to the first asker; hosts behind one address (NAT, an ARP spoofer, a page in the owner's own browser) are one asker; a host that keeps the one web server busy for more than 10 s before the tap can hide the owner's client; a dual-stack browser would count as two askers if the station interface ever enabled IPv6 (it does not today). A host that asks for the receipt at least once every 10 s (polling included) makes every tap the owner's client is already asking for refuse, loudly (a bare re-tap with no client of the owner's asking goes to that host, the first residual); the owner's ways out are the recovery kit's token and the USB serial console (the receipt with the token prints at every boot), which open nothing to anyone else, and last, holding BOOT 10 s, then setting up again from the iPhone through the setup door (the token survives the hold; the hold clears the saved Wi-Fi, the broker login and the name; a stray `?claim=` no longer spends the phone's claim). The hold reopens the first-boot setup door for 30 minutes, and the door goes to whichever Bluetooth client connects first: a client that connects before the phone (a host on the network that is also in radio range, say) can join the Canary to a network it is on, spend the claim minted for its own link (a claim is served before the tap's rule is asked) and keep the token, which nothing rotates today. So the hold is for a host known to be on the network only, with the phone ready, and the phone ends paired unless something in radio range reached the door first. Host-tested; not run on a device |
| MQTT broker link | Plain by default — the broker password crosses the LAN in the clear until the owner provisions TLS, and on the flagship's release images the provisioning request itself (`POST /api/mqtt/config`) rides the plain device API. CA-verified or SHA-256-pinned TLS per product, refusing to connect on an incomplete setup; the lab mode is chosen by name and warns on every connect. Per-variant table: `docs/FIRMWARE_VARIANT_AUDIT.md`. Compile-tested, host-tested, not bench-tested against a TLS broker. The WAP's stored broker password is write-only (its `GET` answers `password_set`), and since sweep F289's third road (#1835) a `POST /api/mqtt/config` from the dashboard's session alone, which the landing page mints for any host on the LAN, that moves the broker (its host, port or TLS mode, or a CA sent) while a password is stored and the body names none is refused `403 password_needed` before anything is saved; the bearer, or the password typed again, moves it (`move_needs_password()`). So the session cannot point the bridge at a listener of its own and have the reconnect hand over the user and password that way (host-tested, not bench-tested; the flagship's own `POST /api/mqtt/config` refuses a carry to a new host or port from any caller, `credential_carry`). Not covered by that gate: a LAN host that spoofs the configured broker's name (mDNS, DNS or ARP) still receives the password from a plain-mode bridge with no change to the config, and the CA-verified mode is the defense. Also not covered: the WAP's camera QR provisioning re-point (`qr_scan_task_fn`). An SCV1 code that names a hub host moves the broker host and port and keeps the stored user and password, so the next connect sends them to the host the code names. It is scanned while a scan runs that a pair token started (the landing page hands one to any caller, and `GET /api/pair/token` mints one for the dashboard's session), or during the unprovisioned boot scan, and the code has to be held in the camera's view: accepted as proximity-bound in the ledger's F289, with F333 keeping the smallest fix |
| Fleet roll-call (`GET /api/fleet`) | The one open read on the hub: rate-limited, no token. It serves the kernel's own row and — when `api.fleet_peers_path` is set — each Canary the MQTT bridge heard, in the contract's coarse words only (name, online, chain verdict, product, and presence/occupants/breathing while proven online); never an event, a zone or key material. The origin allow-list stops other websites' scripts, not a client that can reach the port, so the port is open only where the product needs it: the SecuraCV app ships its 8799 host port disabled (the integration reaches the kernel over the Supervisor's internal network); the Docker sidecar binds every interface and publishes 8799 from the first start so Home Assistant can pair (`SECURACV_API_BIND=all`, the default, which also exports the kernel's cleartext acknowledgment — never relaxing the token on any other route; `loopback` is the opt-out), and there the roll-call serves only the kernel's own row unless `SECURACV_FLEET_ROLLCALL=1` — who is home is the owner's to opt into, never a side effect of pairing. What the roll-call says is bounded by the MQTT broker, not proven past it: a peer with publish rights can replay a captured signed publish (held to one window per missed chain advance), invent ids, or put a real id into `degraded` — see `tvos/discovery/DISCOVERY.md`. The summary file behind it is `0600` and size-bounded |
| Viewer credential (`GET /api/sealed-log`, the Witness Wall) | A second, narrower bearer credential beside the rotating capability token, because a TV cannot re-read a token file: minted by the operator (`witness_api mint-viewer-token`, printed once as a pairing receipt that also carries the kernel's verifying key for the Wall to pin), long-lived until revoked by id, and honored on exactly one route — the non-queryable, size-capped, signed sealed-log tail (Invariant VII bounds what it reads). Presented on any other path or method it is an invalid token that counts toward the per-address lockout, and a good viewer read never clears that count (only a capability-token success does), so the narrower credential cannot reset the lockout that guards the wider one; `?token=` is refused for it as for every token. At rest only its sha256 (`viewer_tokens.json`, `0600`, beside the capability token), compared in constant time and re-read per request, so a revocation lands on the next poll. Whoever holds it reads the coarse sealed record (event types, zones, 10-minute buckets) — the reason it is a credential and not an open read like the roll-call — and over plaintext HTTP it can be sniffed on the LAN like the capability token; TLS stays the owner's opt-in (`api-tls`). What the Wall's "Verified" proves is *authorship* — every served entry signed by the pinned key and linked from the served anchor — not that the tail is *current* or *complete*: the served `checkpoint_head` is unsigned and the document carries no signed time or high-water mark, so a captured genuine document replayed later, or a genuine one cut short at either end, still walks clean against the pin, and the Wall keeps no last-walked head across polls. A signed head (or high-water mark) in the document is the roadmap item that closes it; not built |
| Pairing (`POST /api/pair`, the Docker sidecar) | The one unauthenticated write: it redeems the 6-digit code `docker exec securacv pair` printed (once, on that terminal; never in the daemon log) for a paired bearer. Bounded three ways: the code lives 600 s and works once; a per-code budget of five attempts, each wrong guess spent durably before the refusal is sent and the fifth burning the code; and every refusal — wrong, absent, burned, expired, pairing off — the identical `401 invalid_pairing_code`, each a strike toward the per-address lockout (a success does not clear the strikes; only the first authenticated request with the new token does). Compared in constant time; the kernel stores only the code's sha256. Malformed input is a `400` and not a strike. Known limit: behind Docker Desktop's userland proxy or any reverse proxy every client shares one source address, so a guesser's lockout also holds Home Assistant's polling at `429` |
| Paired bearer (Home Assistant on the sidecar) | 32 random bytes, 64 hex on the wire, sha256 at rest in `paired_tokens.json` (`0600`), re-read per request so a revocation lands on the next call — `DELETE /api/pair/<id>` with the capability token (any id) or with a paired token for its own id only (`403 forbidden` otherwise), `docker exec securacv unpair <id>`, or removing the integration entry. Same rights as the capability token on every route; told apart by its receipts (`auth_mode: paired`, the token id) and the self-revoke rule. **Plaintext on the LAN until TLS lands for the sidecar** — the paired token crosses on every request, as sniffable as the capability or viewer token; the compose quickstarts say so beside the port, and the follow-up (an `api-tls` sidecar whose certificate fingerprint `pair` prints and Home Assistant pins) is tracked. The app has no equivalent exposure: its token file is read over the Supervisor's internal network |
| Kernel announcement (MQTT, `witness/<id>/announce`) | Retained on the owner's own broker: the kernel's id, its public verifying key, where its API listens (`scheme`, `bind`, `port`; a `host` only when the operator sets `SECURACV_ANNOUNCE_HOST` — a network identifier, redacted in Home Assistant diagnostics), the pairing posture, a bucket-start timestamp, and an Ed25519 signature by the kernel's device key over the canonical bytes, obtained from the token-gated `POST /api/sign-announce` because the publisher holds no key. Never a token, a code, the seed, a chain length or a hash. A pinned integration refuses an announcement the pinned key did not sign (`announce_signature_invalid`) and a key that disagrees with the authenticated identity (`announce_key_mismatch` → Repairs `tofu_mismatch`); the pin itself comes only from `/api/pair` or `/api/identity`, never from the announcement. Fleet-internal reconciliation under Invariant III, like the Canaries' retained status topics: the broker's access control is the boundary. Cleared (empty retained payload) on a clean shutdown |
| Home Assistant integration surface | No unauthenticated path to any coarse record: nothing under `/config/www` or `/local`. Evidence packs live under `<config>/securacv/evidence` (`0700` / `0600`) and are served only by an authenticated `HomeAssistantView` (`/api/securacv/evidence/<8 hex>`) through `async_sign_path` links valid 24 hours; each pack is a receipted disclosure on the kernel. Diagnostics are projections (scheme and port, a four-hex kernel-id prefix, eight-character fingerprints, counts and flags) swept by `async_redact_data` over tokens, the token id, hosts, URLs, device ids, keys, hashes, entry ids, nonces, IPs, MACs, SSIDs, names, coordinates and the seed. The integration's claim on the broker (`witness/claim/<id>`) carries a random nonce and a bucket start, never an entry id. Actions never mutate trust; the kernel's key is rotated only through the Repairs fix flow after the person compares fingerprints |
| The SecuraCV app's Supervisor posture | No `hassio_role`: the app reaches only its own options, the shared MQTT service, the add-on info reads its preflight needs, the discovery endpoint and Home Assistant's own API; "Install it for me" is a my.home-assistant.io link; the one Core restart is `POST /core/api/services/homeassistant/restart` on the person's click. The integration it carries is copied only on that explicit click (first install) or, on app start, into a copy bearing the app's own marker and a matching sha256 manifest (temp dir + atomic rename, `cp -RL`, a symlinked destination refused) — never an unmarked or HACS copy, never a downgrade, never followed by an automatic restart (a persistent notification asks). A custom AppArmor profile confines each process to the paths it writes; the panel answers only the Supervisor's ingress proxy and loopback and masks the device key and every broker password |
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
| Auth failure | Lock out the failing source address with exponential backoff, per address on the kernel and on both Canary firmwares (the WAP and the flagship run one table, `firmware/common/network/auth_backoff.h`: at most 60 s per address, and a shared overflow slot bounds the addresses beyond its 8-address table; a host's wrong bearers hold only that host's address, F343, F340). On the Canaries a request with no credential is no strike (the kernel counts a missing token as one, `src/api/mod.rs`), and a BOOT tap (or the WAP's setup-door claim) that hands an address the token clears that address's own slot, never the shared overflow (F341) |
| TLS client cannot verify the WAP's self-signed certificate | The iPhone app pins the receipt's `tls_cert_fp` and refuses an https Canary whose certificate does not match or that has no pin on record (`DeviceAPI.swift`, `PinnedTrustDelegate`); browsers show their self-signed warning. No API asks the client for a certificate — the kernel's rustls config is `with_no_client_auth()` and the WAP's `httpd_ssl_config_t` carries only the server certificate and key |
| HTTPS server fails to start on the WAP | Serves plain HTTP and logs `[HTTPS] Server start FAILED — falling back to HTTP`; setup mode is HTTP-only by design (both stated, never silent) |
| Firmware corrupt | Refuse to boot **on the opt-in secure-provisioning tier only** (secure boot is NOT enabled on default builds — see [Scope of this principle](#scope-of-this-principle)); the default tier relies on the OTA signature check before the image is written |
| A phone asks to pair with the WAP over BLE | A Numeric Comparison (what a phone that shows six digits and a yes/no gets, as current phones do) is answered yes only by the owner's confirm of the six digits in the web UI (`require_pin`; the phone's link and digits must still be the ones shown), on every profile that compiles the pairing channel; with no pairing channel to ask, it is answered no. Until F171 (2026-10) the default FULL profile's Opera module replaced the channel's NimBLE server callbacks, NimBLE-Arduino's default answered every Numeric Comparison yes, with no owner and no MITM check, and a Passkey Entry took the library's fixed 123456: one dispatcher (`ble_server_dispatch.h`) now gives the security callbacks to the channel alone. Not every pairing asks the owner (NimBLE 2.3.8's pairing tables, read from source): a DisplayOnly or NoInputNoOutput initiator, and on legacy pairing a DisplayYesNo one, pairs by Just Works, with no owner and no MITM protection (the channel neither lists that bond nor gives its link the authenticated characteristics, but NimBLE stores the bond). NimBLE-Arduino keeps 3 bonds by default and, when a new one found the store full, unpaired the oldest (when its busy guard allowed); since F189 (2026-10) the channel refuses a new phone's pairing that finds the store full (the phone's pairing fails; a phone the store holds still re-pairs) and never deletes a bond to make room (nor for a bonded phone's subscription record, whose 8-record store NimBLE's default made room in by unpairing another phone, the F189 review), so such a stranger's bonds can no longer push the owner's out, but three of them fill the store and the owner's next pairing is refused until Clear all (`DELETE /api/bluetooth/paired/all`, which deletes every bond the stack keeps; the health log says `BLE pairing refused: the bond store is full`); a keyboard-only initiator pairs by Passkey Entry, which needs the six digits the WAP shows in the PIN box and writes to the health log, in or out of pairing mode (filed in the 2026-09 sweep). A build without the pairing channel pairs by Just Works under NimBLE's defaults. Host-tested over a NimBLE stand-in (both modules' inits); not bench-tested |
| The owner removes a paired phone | Its bond is deleted from NimBLE's store under the phone's identity address (the key NimBLE keeps it by), with the radio quiet for the call: NimBLE refuses to forget a bond that carries the phone's IRK (a phone using private addresses hands its IRK over) while it advertises or scans (`ble_gap_unpair()`, `BLE_HS_EBUSY`), so the loop task ends the owner's scan, pauses the presence scan and stops the shared advertiser (Opera's beacon with it, on FULL) for the delete and starts them again after. When the stack keeps the bond anyway (another task restarted the advertiser in that window), the phone stays listed and the owner is told to try again; only a phone whose bond is gone must pair again. Until F172 the delete named a byte-reversed over-the-air address, and until the F172 review the stack's refusal went unnoticed and Remove answered ok: either way the phone kept its bond and came back encrypted with no owner asked. A link that comes up while Bluetooth is off is dropped (F173). Host-tested over a stand-in that answers as `ble_gap_unpair()` does; not bench-tested |
| Watchdog trigger | Reboot, resume from last good state |
| NVS corruption | A key NVS does not hold (never stored, or wiped) is a fresh start: a new identity. On the flagship (`firmware/canary`) and the canary-wap, a key IDF reports as stored that cannot be read, is not 32 bytes or is 32 zero bytes, or an NVS that cannot say whether one is stored, halts the boot instead: no key is generated, and nothing is written to the identity key or the chain state (sweep F360). A fault in the read that leaves the entry in place (a session that will not open, a flash read error while IDF copies the data) leaves the key for a later boot that can read it. Corruption is IDF's to meet first, and the firmware sees only its answer (read from IDF v4.4.7, v5.5.1 and v5.5.4's source). An entry whose data checksum fails IDF erases in the read it refuses, so that boot halts: on IDF 4.4 (every flagship env but `env:full`) the blob's index survives and each later boot halts until the 30 s BOOT hold at power-on wipes the identity; on IDF 5.5 (`env:full` and the canary-wap) the next boot's NVS init erases the orphaned index, so that boot finds no key and makes a new identity. Some faults make a new identity with no halt, because IDF answers "not found": a data chunk whose header fails IDF's check, on IDF 5.5 (on 4.4 it halts every boot, as above); an index entry whose header fails it, which IDF erases before the key is read (when it loads the page, or in the probe's own lookup); and a flash read error in IDF's own lookup of the key, which sets that page aside for the boot and leaves the old entry on it. All of it is today's behavior, not a decision; what holds throughout is that the firmware never writes over a key IDF still reports as stored. The canary-wap's BOOT hold keeps the identity (it has no wipe stage), so a clean install (full erase) in either flasher is its way to a new identity on purpose, halted or not. Before F360 a read that failed after the length check, before IDF copied anything into the buffer, ran the boot on the zero key, whose setup-network key is the published `cv-3jByJ6rHdhM2` (on a release canary-wap, the SoftAP came up under it and records were signed with it), and a failed open or length lookup replaced the stored identity. Host-tested; not run on a device. canary-sense, canary-vision and canary-sentinel never sign with the zero key, but still replace a stored key they fail to read with a new one (Home Assistant shows a changed fingerprint; sweep F362) |

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
Adversary must know the AP password: device-unique and computable from
nothing the device publishes. On the WAP and the flagship it is an HMAC of
the device's private key under its own domain label; on the headless setup
portals and the display it is a random key minted once and kept. (Until
sweep F350 the flagship's was derived from its public-key fingerprint,
which the device publishes: the setup network's SSID spells two of its
bytes and every signed MQTT event body carries all eight. Anyone who had
seen the fingerprint could compute it. canary-wap had moved to the private
key earlier.) Even if connected: API auth required (on the flagship a peer
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

### Scenario 11: Adversary on the LAN of a Docker sidecar

**Attack:** A host on the same network as a Docker sidecar — whose port 8799
is published by default — tries to read the witness log, pair itself, or
steal Home Assistant's credential.

**Result:** Without a token the host gets `/health` and the kernel's own
roll-call row, nothing else; every data route refuses. Guessing the pairing
code is bounded to five tries per code against a million codes inside ten
minutes, every refusal is the same `401`, and the strikes lock the address
out; a code that was never issued cannot be redeemed at all. What the host
*can* do today is read Home Assistant's paired token off the wire, because
the sidecar serves plain HTTP — the stated exposure until TLS with a pinned
fingerprint lands for the sidecar — and then read the coarse sealed record
(event types, zones, ten-minute windows; never a face, a plate or a frame)
until the owner revokes that token (`docker exec securacv unpair <id>`).
It can also replay a captured announcement, which a pinned Home Assistant
ignores unless the signature verifies, and cannot forge one without the
kernel's key.

**PASS if:** No credential is minted without the owner's code; the sealed
record is never readable without a credential; the plaintext exposure is
stated, bounded to coarse claims, and revocable.

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
- A factory reset before the seizure does not destroy the data, on either
  tier. The canary-wap's 10 s BOOT hold clears the saved Wi-Fi, the broker
  login and the device name, and keeps the identity key, the chain state,
  the witness records and the health log on SD, the API token, and the
  Bluetooth bonds: each bonded phone's keys stay in NVS, as readable from
  flash on the default tier as the rest of it, and a bonded peer keeps the
  lane that changes the WAP's Wi-Fi (removing the paired phones,
  `DELETE /api/bluetooth/paired` one at a time or
  `/api/bluetooth/paired/all`, is what deletes them). No hold sheds its
  key (a clean install, a full erase in either flasher, is its way to a new
  identity). The `firmware/canary` flagship's 10 s hold keeps the
  identity key and the chain state too; only its further stage, BOOT held
  on to 30 s, sheds them, by clearing its NVS namespace. That clear does not
  remove the key's bytes: NVS only marks each entry erased, and the bytes
  stay in flash until NVS erases that page to reuse it, so on the default
  tier a full erase in either flasher is what removes the key. Neither hold
  erases the SD card.

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
Additionally, exponential backoff limits brute-force attempts. On the
kernel and on both Canary firmwares it is per source address; on the
Canaries (the WAP and the flagship share `auth_backoff.h`) at most nine
backoffs run at once (8 tracked addresses and a shared overflow), so a host
with many addresses gets about 30 guesses a minute between reboots. The
table lives in RAM, and on the WAP any landing session can reboot the unit
(`POST /api/reboot`, `POST /api/safe-mode/retry`), which empties it: a
guesser that reboots the WAP after every few seconds of guessing gets about
37 a minute if the unit is back on the LAN 45 s later, about 100 at 10 s,
about 155 at 5 s (modeled over `auth_backoff.h` in
`firmware/tests_host/test_auth_backoff.cpp`; the WAP's boot time is not
measured; [`magic_pairing.md`](../design/magic_pairing.md) §3.7). The
flagship's reboot needs the bearer. On both, the token's entropy is the
bound across addresses (F343, F340): about 184 bits for a
WAP token derived since its 64-byte encoder (32 random characters of a
54-character alphabet); about 155 on average for the flagship's, whose
encoder takes 32 HMAC bytes, so about 27 of its characters are random and
the rest a fixed top-up; and about 116 on average for a WAP token kept in
NVS from the first, 24-byte encoder.

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
5. All security-sensitive defaults are hardened where they are set:
   `firmware/canary/include/secure_defaults.h` maps each default to the
   file that decides it, and `regression_check.sh` ("Security: hardened
   defaults") holds the BLE, MQTT and SoftAP-client ones to their values.
   No Canary image requires TLS today: the release image serves HTTP until
   `FEATURE_HTTPS` fits its slot
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
- A signature proves who signed a frame, not which radio sent it or whom it
  was for. The envelope signs no source or destination address, a
  PlatformIO sender spends one outbound counter across every destination,
  and frames go out unencrypted (peers registered with `encrypt = false`).
  So a member's genuine frame that a receiver has not heard yet — a missed
  broadcast, a rotation frame unicast to another member — can be recorded
  and re-sent from any radio in range and still pass signature, `opera_id`
  and counter. The PlatformIO tree therefore takes no radio address from a
  frame: an opera frame from an address its transport table does not hold
  is dropped before verification, and so, since F70, is a member's frame
  from any address but the member's own binding (the bullet on where a
  verified frame's source is recorded, below); a member's address is bound
  only by a completed pairing (or restored from NVS at boot), so a changed
  radio MAC means a re-pair — with the limits in the bullet on the pairing
  code below. F49 part 3 (#1756) briefly re-bound and persisted a member's
  address on such a frame, which let an outsider re-point the member at
  its own radio (host probe: the receiver's next rotation sent its OFFER
  to the outsider, and the 60 s commit dropped the member when it stayed
  silent); withdrawn,
  spec §8.3. canary-wap re-pointed a member's address, and its ESP-NOW
  registration, at the source of any frame that passed its checks; a host
  probe against its real `mesh_network.cpp` showed that a frame the
  receiver missed, a frame the member sent to another member (canary-wap
  counts per destination), or one heard before a power cut moved the member
  to an outsider's radio, and a copied member address made the receiver
  drop another member's ESP-NOW registration. Since 2026-10-01 it drops a
  frame whose source is not the signer's own bound address before
  verification, and a re-pair re-binds a member it already holds (logged).
  A changed address therefore needs a re-pair with each member that holds
  it, where one frame used to move it everywhere; an entry an older
  firmware's re-pair duplicated is folded into one at boot (logged).
- **canary-wap's pairing waits for its owners (2026-10-01).** Once a
  re-pair could re-bind a member, the pairing handlers bound addresses,
  and pairing frames are unsigned. Two ways to drive them with no owner
  action were host-probed and closed. A joiner took COMPLETE as soon as it
  showed its code, before its owner confirmed: an outsider that answered
  its DISCOVER first finished the pairing, replacing the joiner's opera
  or, holding the `opera_secret` and presenting a member's public key,
  re-binding that member to its own radio. And an initiator kept a
  finished pairing's ephemeral key and confirmed code until the 2-minute
  timeout, so a radio that overheard its OFFER could send its own ACCEPT
  and CONFIRM and get the `opera_secret` sealed to it in a COMPLETE anyone
  in range can read (pre-existing). Now, as on the PlatformIO tree, the
  joiner takes the first OFFER and a COMPLETE only after its owner
  confirmed, and the initiator takes one ACCEPT, from where its OFFER
  went, and wipes the pairing once COMPLETE is sent. Since F75 the owners
  may confirm in either order (the initiator keeps a joiner's early
  CONFIRM), and a CONFIRM counts only from the pairing partner's address,
  and only once the code is shown: the joiner's genuine CONFIRM re-sent
  from another radio no longer completes a pairing, and a wrong hash from
  another radio no longer cancels one. The address check does not stop a
  reflection (open): the CONFIRM hash is the same in both directions, so
  the initiator's own CONFIRM, re-sent to it from the joiner's address,
  counts as the joiner's, and the initiator then holds a joiner whose owner
  never confirmed (the joiner drops the COMPLETE; host-probed, the same
  before F75, and on the PlatformIO tree, which computes the same hash,
  the same before and after F97). A hash bound to the sender's role would
  stop it (a wire change). Since F73 a pairing whose partner the device cannot hold (a
  deny-listed key, a full opera, an address another member holds) fails
  and is logged: the initiator adds the joiner before anything is sent, so
  it no longer seals the `opera_secret` to a partner it then refuses, and a
  refusing joiner keeps its own opera. The PlatformIO tree does the same
  since F118, and earlier: the check runs at the owner's confirm, before
  any CONFIRM goes out, so a joiner that cannot hold its initiator never
  gets the secret sealed to it (canary-wap's joiner refuses at the
  COMPLETE, after its initiator has added it). Since F98 that includes a new key at
  an address another member holds, which `add_peer` used to append: the two
  entries shared one ESP-NOW registration, and removing either stranded the
  other. Such a refusal is logged on its own line. Its routine case is a
  device whose NVS was erased or that was reflashed: it keeps its radio
  address with a new key, and each member that holds its old entry refuses
  it until that entry is removed, which on canary-wap splits that member
  from the opera until F48 (the rotation below). Since F100 the initiator sends its COMPLETE again every 2 s until
  it hears the joiner, for at most the 2-minute pairing timeout, and logs
  one never answered (it went once, unchecked); the copies are the frame
  already on the air, and the pairing key stays wiped. A joiner drops a
  COMPLETE it cannot open under its pairing key instead of ending the
  pairing, which any radio could do with 61 bytes from any address.
  Host-tested only.
- **Still open on canary-wap: a radio copying a member's own address.**
  ESP-NOW does not authenticate a source, so a radio that copies member
  B's bound address passes canary-wap's address check. It can deliver B's
  not-yet-heard frames: ones B sent to other members whose counter is above
  the receiver's last-seen for B (B's counters are per destination and the
  envelope names none), or, after a power cut, frames heard since the last
  5-minute counter save. They are dispatched, and the receiver's last-seen
  for B moves up, so B's own frames drop as replays until B's counter for
  the receiver catches up. No address moves (host-probed; open). A
  destination in the signed bytes would stop the cross-member case (a wire
  change); the power-cut window is the counter-save cadence. Since F71 a
  rebooted canary-wap no longer restarts its per-member send counters at 1:
  each is reserved ahead in NVS, and a boot resumes every member above the
  highest counter it reserved for any of them (spec §3.3), and no counter
  is signed twice. A rotation keeps the counters (since F95's counter fix;
  it used to restart them at 1, and a member that had not switched, or
  rebooted before its 5-minute last-seen save, dropped the restarted
  frames), and a new member starts above every counter the device can
  have signed, a removed member's included (F99: a device re-paired after a
  removal or a leave kept its last-seen counter and dropped a counter that
  restarted at 1). It starts level with the busiest member, not at the
  reservation a block ahead of it, which would have widened the
  cross-member gap above by up to a block at every pairing; after a busy
  member is removed the new one starts as far ahead of the others as that
  member's traffic was, until the next boot. The first boot after the update finds no record and resumes above
  2^40, which no older boot reached, so it is heard at once too. An
  unreadable record resumes above 2^48, above anything signed since the
  update; a second unreadable record resumes below the device's own
  history, and its members drop its frames until each counter climbs back.
  A boot also brings every member's counter level, so the cross-member gap
  above starts again from nothing at each boot rather than growing by up
  to a reservation block (host-tested).
- **A re-added member's old frames stay replays on canary-wap (F116).**
  A member a canary-wap dropped (removed, or left behind at its own leave)
  used to take its last-seen counter with it, and a re-pair started it at
  0. In an opera whose id had not changed (removing the last member
  rotates nothing; a re-pair into the same opera after a leave) every
  frame the member had signed before, alerts included, was fresh once
  more, and one replayed from its address at the re-pair counted as the
  joiner heard and ended the COMPLETE resend (host-probed). The PlatformIO
  tree already kept such counters as tombstones; canary-wap now does too,
  in NVS (`rx_tombs`), each for the opera the member was dropped from, and
  a re-add into that opera on either side of the pairing, or a boot,
  starts the member there. In any other opera the old frames drop at the
  `opera_id` check (a removal with a survivor rotates the opera at once),
  so no tombstone applies there. Limits: eight at most, so a ninth drop
  evicts the oldest, whose window reopens at its next re-add; and a
  removal of a member not heard above the tombstone its re-add restored
  releases it, so the next re-add starts at 0 again. That release is the
  owner's way back for a device whose own counters went back while it kept
  its key (its send-counter record lost), which would otherwise drop at its
  tombstone for good; it reopens the window for that member. Every member
  on firmware from before F71 is in that class: it starts its counters at
  1 at every boot and every add, so after a removal and a re-pair into the
  same opera it stays unheard until it is updated (F71's first boot starts
  it above) or removed again and re-paired after the deny-list grace.
  Update it first. Not covered: a frame the member signed after its
  removal (the removal is one-sided, so it goes on signing above its
  tombstone) is fresh at the re-pair, and one replayed there still ends
  the COMPLETE resend; a frame the member sent another member and this
  device has not heard is still fresh here (the cross-member gap above,
  open), and so is one heard since the last 5-minute last-seen save before
  a reboot; and on a board without flash encryption a boot loads no opera
  and no member, so the members it held then leave no tombstone and a
  re-pair into the same opera starts them at 0. Host-tested only.
- **The 6-digit pairing code does not cover the long-term keys**
  (pre-existing; found in the review of the F49 part 3 withdrawal). The
  code and the CONFIRM hash are derived from the ephemeral X25519 session
  key alone, and the long-term Ed25519 key a pairing binds is taken as the
  DISCOVER (joiner to initiator) or OFFER (initiator to joiner) carried it;
  that key signs nothing in the exchange. So an outsider that relays an
  owner-run pairing between the two devices, from its own address and
  without touching the ephemeral keys, gets matching codes on both screens
  while choosing the key the initiator records (the joiner takes the
  OFFER's key the same way; that side was not probed). Claiming an
  already-trusted member's key re-binds that member to the outsider's radio
  and persists it, and the member's own frames then drop as coming from an
  unbound address until another re-pair, which runs the same exchange.
  (From an address no other member holds: since F118 the PlatformIO tree
  fails a re-pair from an address another member holds at the owner's
  confirm; from F102 until then it completed, refused the bind and
  persisted nothing; before F102 it persisted the address anyway, below.)
  Claiming its own key makes the outsider a trusted member, able to sign a
  rotation that removes a real one. Host-probed on the PlatformIO tree,
  with the same results before #1756, on #1756 and after the withdrawal.
  canary-wap derives its code the same way. Since its re-pair re-binds a
  member it already holds (2026-10-01), a relay claiming a member's key
  re-binds that member to the relay's radio there too (host-probed;
  before, the relay added a duplicate entry no lookup reached); the move
  is logged as a health WARNING, the owner's only sign. What the code
  does bind is the ephemeral exchange, and only weakly. A relay that swaps
  one ephemeral key shows different codes on the two screens, but the code
  has 10^6 values and nothing commits either side's ephemeral before the
  other side's is sent, so a relay that swaps both picks its second
  ephemeral after it has seen the first and grinds it until the two codes
  match. Both owners then confirm the same code, and the relay opens the
  COMPLETE and holds the `opera_secret` (host-probed on canary-wap against
  its real pairing handlers, 2026-10-02: about 1.5 million X25519 tries,
  under 3 minutes on one host core, and the search splits across cores,
  against a 2-minute pairing timeout; the relay presented both devices' own
  long-term keys, so this is not the key substitution above). The
  PlatformIO tree derives its code the same way, in the same OFFER and
  ACCEPT order (read from code, not probed). Fixing both needs a
  commitment to one side's ephemeral before it sees the other's (as
  Bluetooth's numeric comparison commits to a nonce) or a much longer
  code, together with both long-term keys in the code and the CONFIRM
  hash, or a transcript signed with them: a wire change on both trees,
  open. On the PlatformIO tree the re-pair also
  cannot start while eight members are bound: the transport table has no
  slot for the new address, so the pairing's replies cannot be sent until
  a member leaves or is removed (host-probed).
- **Where a verified frame's source is recorded: closed on the PlatformIO
  tree (F70).** Until F70 the PlatformIO receiver took a member's verified
  frame from any address in its transport table and recorded that address
  as the member's (its liveness link), and the table holds more than the
  member's own binding. While a pairing runs, the partner's address is in
  it, so an outsider that answered the pairing from its own address could
  deliver a member's not-yet-heard frame there with no spoofing until the
  pairing ended (as initiator or as joiner); and because ESP-NOW does not
  authenticate the source address, a radio that copied another bound
  member's address could do the same. No binding moved, but the receiver
  sent its rekey replies to the member at the recorded address (host
  probe: a replayed `REKEY_OFFER` from the pairing partner's address got
  the receiver's `REKEY_ACCEPT` sent to the outsider), and a later removal
  of the member took that address out of the transport table, stranding
  the member whose address was copied. The same before #1756. Now a
  member's frame from any address but its own binding is dropped before
  verification, as on canary-wap: it spends no counter, reaches no handler
  and records nothing. A member with no binding at all (none restored at
  boot, or a pairing whose bind was refused) has no address its frames
  are taken from until a pairing binds one. The rekey unicasts go to the binding, forgetting a
  member removes only the binding, and the liveness link is the binding
  once a verified frame has arrived from it. What remains is a radio that
  copies the member's *own* address: it still gets the member's
  not-yet-heard frames dispatched, as on canary-wap. Nothing can tell it
  apart, it moves no address, and on this tree the member's later frames
  still count above it (one counter per sender). Host-tested
  (`test_mesh_session`, four tests that fail on the code before F70); not
  bench-verified.
- **PlatformIO pairing and sends (F97, F101, F102; host-tested only).**
  Three gaps the F70 work found, closed:
  - *Either confirm order.* Until F97 the PlatformIO pairing took a
    CONFIRM only after its own owner had confirmed and sent its own once,
    so no order completed with frames delivered as sent. Worse than a
    timeout, an initiator confirmed first reported success and held a
    member whose joiner had dropped the COMPLETE (host-probed). canary-wap's
    F75 rules now hold here: a CONFIRM counts only from the partner's
    address and once the code is shown (before it, the all-zero session
    key makes the hash anyone's), the initiator keeps a joiner's early
    CONFIRM, and the joiner takes the COMPLETE only after its own owner
    confirmed. Every PlatformIO COMPLETE goes out with the initiator's own
    CONFIRM in front of it, so a joiner on firmware before F97 completes
    too, in either order; without it, an updated initiator would report
    success with such a joiner, which drops the COMPLETE (host-probed). A
    pre-F97 initiator with an updated joiner completed only when the
    initiator's owner confirmed first; otherwise both timed out and
    neither kept the other. Since F117 the updated joiner re-sends its
    CONFIRM, at most three times and only to its partner, once it has read
    the initiator's CONFIRM after its own owner confirmed, and that pair
    completes in both orders (host-probed against the pre-F97 library);
    the copies carry the hash its first CONFIRM already put on the air. The
    reflection above stays open.
  - *Opera sends to members only.* Until F101 every opera sender (tamper
    alert, beacon event, channel lock, hub election, LEAVE, rekey OFFER)
    went to every address in the transport table, which, while a pairing
    runs, includes the partner's: an outsider answering a pairing got them
    all, and they counted as sent (with no member, a tamper alert reported
    sent and a leave reported `notified`; host-probed). The frames are
    signed, not encrypted, so this disclosed nothing a radio in range could
    not overhear; the harm was a false "sent". Now each goes to the
    members' bound radio MACs only, and only those sends count.
  - *An address persisted only once bound.* Until F102 `main.cpp` persisted
    a just-paired member's address before the session's bind, which refuses
    an address another member holds, and the boot restore binds stored
    addresses in order. So a re-pair of member J relayed from member C's
    copied address (the relay above) was written as J's address, and after
    a reboot J held it and C, refused its own, was not heard at all
    (host-probed). The address is now persisted from a callback that runs
    after the bind, only when it took, and NVS refuses an address another
    fingerprint holds; a refused bind is logged by fingerprint and leaves
    the member's previous binding as it was.
  Pinned by host tests in `test_mesh_pairing`, `test_mesh_session` and
  `test_mesh_state` that fail on the code before each fix, and a source pin
  on `main.cpp`'s wiring; the canary build is CI's; not bench-verified.
- **PlatformIO refusals and stored addresses (F118-F120; host-tested
  only).**
  - *A partner the device cannot hold fails the pairing (F118).* Until
    F118 the PlatformIO initiator sealed the `opera_secret` before anything
    checked the partner, both sides reported success, and the partner's
    key was registered and stored; only the address bind then failed. A
    re-pair relayed from another member's copied address (the relay above)
    thus left a trusted member heard from nowhere, sent nothing and holding
    a slot. Now the pairing asks first — at the owner's confirm, again
    before the seal and before the joiner opens the secret — and a refusal
    sends, seals and stores nothing, answers the confirm `409
    partner_refused` and is logged by fingerprint. The relay can still
    claim a member's key from an address no member holds (above; F69).
  - *Stored addresses at boot (F119, F120).* A `peer_macs` blob written
    before F102 can hold one address under two fingerprints, and the boot
    restore bound in stored order, which in F102's scenario gave the address
    to the member that did not own it. Now neither entry is bound, both are
    dropped and the shared address is logged, so both members re-pair. An
    entry whose fingerprint is no longer a member is dropped too: kept, it
    held its address in NVS for good, so the next member to pair from that
    address was unheard after every reboot. Nothing is dropped when the
    pubkey list could not be read.
  Pinned by host tests in `test_mesh_pairing` and `test_mesh_session`
  that fail with each check removed (each of the partner gate's refusals,
  each place it is asked, the boot restore's drop decision for every
  verdict), and the `main.cpp` source pin, which holds its restore loop and
  its refusal log to their exact form; the canary build is CI's; not
  bench-verified.
- **PlatformIO pairing endings (F133-F135; host-tested only).**
  - *A lost COMPLETE (F134).* Nothing acknowledges a COMPLETE, and until
    F134 the initiator sent it once and reported success, so one lost on
    the air left it holding a member that never joined, the same harm as
    the reflection above. Now it sends the frames it already put on the
    air (its CONFIRM and the sealed COMPLETE, byte for byte; the pairing
    key stays wiped and nothing is sealed again) every 2 s, to the
    partner's address only, for at most the 5-minute pairing timeout, and
    stops when it hears the joiner (a verified fresh frame from the address
    it paired from: its fingerprint is public, so a forged, relayed or
    replayed frame bearing it ends nothing), when the joiner is no longer a
    member bound where it paired from, or when the opera is left or
    rotated. The
    copies disclose nothing the first send did not: the same ciphertext,
    to the same address. No frame redirects them or brings one early: a
    CONFIRM reaching a completed initiator, the joiner's, replayed or
    reflected, is dropped (host-tested, with third-party, replayed and
    reflected pairing frames of every type). A joiner still takes a copy
    only after its own owner confirmed; so the reflection above now ends in
    a pairing when the joiner's owner confirms within the window, and still
    leaves the initiator holding a member that never joined when that owner
    does not. An initiator on firmware before F134 keeps the old harm. The
    cost, until F162: this tree's members sent nothing on a timer, so the
    copies usually ran their whole window, at most 149 copies of two
    frames. Since F162 a member sends one signed HEARTBEAT right after it
    pairs, so the joiner's ends the initiator's copies within one pass; they
    run on only while the first COMPLETE is lost (until the joiner opens a
    copy and its heartbeat follows) or when that heartbeat cannot reach the
    initiator (F320). Since F324 the joiner heartbeats every 30 s ± 3 s, so
    a lost paired heartbeat costs about one more interval of copies (about
    15 copies of two frames), not the whole window; the window runs out
    only against a joiner that sends nothing in it.
  - *A cancel after the end (F135).* A `pair/cancel` that landed after the
    initiator's COMPLETE went out used to fail the completed pairing: the
    joiner held the `opera_secret` while the initiator never trusted or
    stored it. A cancel now ends only a running pairing, and leaves a
    completed one's COMPLETE copies running.
  - *The pairing screen (F133).* The web UI called any pairing that ended
    in `ACTIVE` or `CONNECTING` complete, a timeout, a refusal (above) or a
    cancel included. `GET /api/mesh` now reports the last pairing's number,
    outcome and failure reason, and the page reports a failure as one.
  Pinned by host tests in `test_mesh_pairing` and `test_mesh_session`, a
  node test of the page's poll and a source pin of the handlers' wiring
  (`scripts/tests/test_canary_mesh_status_wiring.py`), each failing on the
  code before its fix and by mutation; an interop probe in scratch ran the pairing library
  against the pre-F117, wave-10 and pre-F97 libraries; the canary build is
  CI's; not bench-verified.
- **PlatformIO membership: a join needs a leave, a member says it paired
  and keeps saying so, the peers list is the session's (F136, F162, F199,
  F324; host-tested only).**
  - *A join on a Canary already in an opera is refused (F136).*
    `pair/join` with an opera held answers `409 opera_exists` before a
    pairing number is taken or any member, key or secret is touched; until
    F136 the join installed the new `opera_secret` over the old one while
    every old member stayed registered, bound and stored, listed but never
    heard and counting against the eight slots. The cost: a Canary cannot
    pair with a second member of the opera it holds, so a changed radio
    address is Leave then Join (F319). canary-wap's join is unchanged
    (F318).
  - *A member announces itself, and keeps doing so (F162, F324).* One
    signed `HEARTBEAT` (registry 16, empty payload) to bound members only,
    right after NOTIFY_PAIRED and once on the first pass after a boot that
    bound a member (F162), and since F324 every 30 s ± 3 s while the
    session runs with an opera, every heartbeat re-arming the deadline;
    never to a pairing partner that is not a member, and nothing signed or
    counter spent with no member bound. A receiver handles it as any
    verified opera frame; it discloses nothing an opera frame does not
    (the opera id, the sender's fingerprint, a counter, an uptime stamp),
    and all of that is already on the air in every opera frame.
    - *Accepted: a periodic radio emission.* A member's MAC is now on the
      air every interval, which marks a device as a Canary in an opera to
      anyone listening on its channel. That adds little to the RF pattern
      of the only build with the mesh: `[env:full]` already broadcasts the
      BLE status beacon continuously, and no released image builds the
      mesh. The owner who wants no periodic frame turns the mesh off
      (`POST /api/mesh/enable` with `false`): a stopped session sends
      nothing and drops its deadline.
    - *Liveness counts verified frames only (F324).* A member's peer row
      (`state`, `last_seen_sec`, `rssi`) and `peers_online` come from its
      last frame that passed the signature, opera and counter checks
      (`verified_seen_ms`), as on canary-wap. Until F324 they were the
      transport entry's, which any frame from the address refreshed before
      verification, and ESP-NOW does not authenticate a source: a 3-byte
      junk frame and a replay from a quiet member's MAC once a minute held
      it `CONNECTED` and `peers_online` 1 for ten minutes (host-probed). A
      heartbeat alone would have made that easy to aim (the member's MAC on
      the air every 30 s), so an intruder who pulled a Canary could have
      held it `CONNECTED` with a spare ESP32. Now junk, a forgery (a
      flipped signature byte, or a frame signed under another opera) and a
      replay of a frame the receiver has heard refresh nothing
      (host-tested), and a member that is gone reads `STALE` within about
      90-100 s and `OFFLINE` within about 5 minutes while that is all a
      radio copying its address sends.
      An age past 2^31 ms is held there, so a member gone longer than the
      32-bit clock can count does not read `CONNECTED` again when it wraps.
    - *Not covered: a frame the receiver did not hear.* The counter check
      is a high-water mark, so any genuine frame of the member's that the
      receiver did not hear verifies whenever it arrives from the member's
      own address, and refreshes its liveness: a recorder that sends such
      frames one under every 90 s holds a member that is gone `CONNECTED`
      about 90 s per frame, and then it reads `STALE` and `OFFLINE`. There
      is no fixed bound, after a reboot or otherwise: the hold is the size
      of the recorder's stock, and a radio near the member comes to hold
      one three ways, and a fourth, after a pairing, is narrowed but not
      closed (the last below):
      - *Frames the member sent while the receiver was not listening.* A
        member heartbeats each bound member's address every interval
        whether or not that member is there (spec §7.1), so every heartbeat
        it sends while the receiver is off, booting, out of range or on
        another channel (the softAP's channel 1 before its station rejoins
        its router, F320) is one the receiver never heard, and a passive
        recorder near the member keeps it with no jamming. They build up at
        one per about 30 s the receiver is away, and relayed they hold a
        gone member `CONNECTED` about three times as long as the receiver
        was away (2.6 to 3.3 times by the jitter's range; one hour down
        held it about 2.9 hours in the host test). Before F324 a member sent
        nothing on a timer, so a downtime left no frames to keep. A breaker
        trip on the receiver's circuit or a slow router rejoin leaves a
        small stock with no intent at all; an intruder who unplugs the
        receiver, pulls the member and plugs the receiver back in builds a
        large one.
      - *After a receiver reboot, every frame it heard since its last
        5-minute replay-counter save* is fresh again (spec §8.3; the F116
        bullet above says the same of canary-wap's own save): at most 12
        heartbeats one save interval holds at the shortest spacing (more if
        the member sent other frames in it), about 18 minutes, on top of
        what the member sent while the receiver was down. A receiver that
        reboots more often than every 5 minutes never advances its saved
        counter, so the same stock serves on every boot. A save before the
        deliberate restarts would narrow this to unclean ones (a power cut,
        a crash); it is not made.
      - *A jammer's stock.* A jammer that keeps the member's frames from the
        receiver and sends them late holds it `CONNECTED` for as long as its
        stock lasts, after the member is gone too, with no `STALE` between;
        a jammer that only blocks leaves it `STALE` within about 90 s.
      - *Narrowed, not closed: after a pairing, every frame the member
        signed under the opera before it.* The new receiver never heard any
        of them, and held no saved counter for the member until its first
        frame there. A receiver paired on the bench and unplugged within 5
        minutes to be mounted restored no counter for the member, and every
        frame the member had signed (since F324, one heartbeat to each other
        member every 30 s) held a gone member `CONNECTED` again: 240
        recorded frames, 356 minutes (host-probed). Now the counter of the
        member's first frame that verifies there is persisted at once
        (mesh_session's first-counter handler, main.cpp
        `on_mesh_first_counter`; also after a re-pair from the member's
        replay tombstone, and after a boot that restored no counter for it,
        a saved 0 included, since a save before the member is heard writes
        it at 0 and no frame carries 0), and once more
        `FIRST_COUNTER_RESAVE_MS` (66 s) later if it has moved. A save that
        fails (NVS refused the write) is tried again rather than taken as
        done: the first on the member's next frame that verifies, the
        second 66 s after the pass that failed. The Codex review of #1839
        found both taken as done, so a failed first save waited for the
        second (or, with the counter unmoved by then, for the 5-minute
        save) and a failed second save for the 5-minute save. The first
        frame that verifies need not be the member's newest: the counter
        check is a high-water mark, so any older genuine frame of the
        member's passes too, and a radio copying the member's address can
        land one while the receiver holds the member registered and unheard.
        That window runs until the member's own first frame lands: after a
        pairing, milliseconds normally, up to about one heartbeat interval
        (33 s) when the first COMPLETE or the paired heartbeat is lost,
        which a jammer can force; after a boot that restored no counter for
        the member, until the receiver hears the member's next heartbeat,
        which also waits for the receiver's station to join its router (the
        softAP's channel 1 until then, F320). The first save then holds the
        older counter, and the member's own frames raise it in RAM only. The
        second save holds the member's own counter unless every frame of the
        member's own was kept from the receiver through those 66 s: lost,
        jammed, or, after a boot, unheard by a station that had not yet
        joined its router. So what stays open is three reboots. One before the
        member's first frame lands (or, when NVS refused that save, before
        its retry on the member's next frame, or a 5-minute save, lands)
        finds nothing newer saved for it (after a first pairing, no counter
        at all), so everything it signed before is fresh again, and nothing
        need be landed at the pairing (host-tested: 132 of 132 relayed frames read
        `CONNECTED`; with that save refused and the reboot before its
        retry, 133 of 133); after a pairing that window is the one above,
        milliseconds unless the first COMPLETE or the paired heartbeat was
        lost or NVS refused the first save, which holds it open until a
        retry lands: on the member's next verified frame, normally its next
        heartbeat 27 to 33 s later, about 33 s more for each refused write.
        No pass retries it, so when the member's own frames are kept from
        the receiver after the refusal (lost, or jammed), it stays open
        until the member's next verified frame or the next 5-minute save
        that lands (main.cpp saves the counters every 300 s), as every
        pairing's did before F324 (host-tested: 290 s of passes with the
        member quiet ran no retry, and a reboot then read 133 of 133
        `CONNECTED`; with a 5-minute save landed before it, none).
        One before a save of the member's newer counter lands (the second
        save, 66 s after the first, and, when NVS refused it, 66 s after each
        refused try; or a 5-minute save, when one lands first), and, when the
        member's own frames were kept out through the 66 s, one before the
        next 5-minute save,
        restore the older counter, and everything the member signed after
        it is fresh again (host-tested: a reboot before the second save let
        132 relayed frames hold a gone member `CONNECTED` at every read;
        after it, at none, also when one heartbeat of the member's own was
        lost and when the millis() clock wrapped between the two saves; a
        refused second save leaves the older counter saved until its retry
        lands 66 s later). Those two take a recording of the
        member's earlier frames, a radio copying its address at the pairing
        or the boot, and that reboot.
      One frame the receiver hears from the member itself makes every older
      one a replay. Nothing on the receive side tells a late frame from a
      fresh one: the signed header carries the sender's own clock, which no
      receiver checks, and closing this needs a freshness check against
      that clock or a wire change. Host-tested
      (`test_a_frame_the_receiver_has_not_heard_still_counts_as_live`, its
      reboot, jammer and downtime legs,
      `test_a_members_first_counter_is_saved_at_once`,
      `test_an_older_first_frame_is_saved_over_once_more`,
      `test_a_failed_first_counter_save_is_tried_again` and
      `test_a_failed_second_save_is_tried_again_a_deadline_later`); not run
      on a device.
    - *The replay window of a missed frame, bounded conditionally.* A frame
      of a member's that the receiver missed stays dispatchable from a radio
      copying the member's own address (spec §8.3) only until the receiver
      hears the member's next frame: on a working link its next heartbeat,
      about 33 s or less. A jammer that keeps the member's frames from the
      receiver keeps it open; if it also sends them late, the member reads
      `CONNECTED` throughout (the bullet above). Not a flat 33 s bound.
    - *Cost, from code.* One Ed25519 signature per interval per sender, and
      at most 8 heartbeat verifies per interval per receiver (one per bound
      member; `MAX_TRUSTED_PEERS` is 8), on the main loop; one outbound
      counter per heartbeat (a reservation write per 1024, about 2.8 a
      day); the 5-minute replay-counter save now writes counters that move,
      and a member's first counter adds at most two saves that land per
      registration (a pairing, or a boot that restored no counter for the
      member): its first frame's, and one 66 s later if the counter moved;
      past those two no frame adds another; these writes happen only on
      flash-encrypted boards. A save that fails is tried again on the
      member's next verified frame, or for the second save 66 s later,
      never on the next pass. On a board without flash encryption every
      replay-counter save is refused before any write, so the first-counter
      save is tried on each of the member's verified frames and writes
      nothing: a counter snapshot per frame (the session's half host-tested,
      with a handler that fails on every call:
      `test_a_failed_first_counter_save_is_tried_again` and
      `test_a_failed_second_save_is_tried_again_a_deadline_later`). On a
      flash-encrypted board whose NVS keeps refusing, each verified frame
      of a member whose first save has not landed is one refused write
      attempt (`Preferences::begin` and `putBytes` of the replay blob) on
      top of the verify the frame already cost: normally one a heartbeat,
      but a radio copying the member's address with a recording of the
      member's frames this receiver has not heard sets the pace, as fast
      as the main loop verifies what the transport queues (8 frames at a
      time), bounded by the frames in the recording, since each verifies
      once. It needs a board whose NVS already refuses the write; whether
      a refused write wears flash was not checked on hardware. The
      transport's 100-frame-a-second storm gate is untouched. The same cadence canary-wap pays (F76).
  - *The peers list is the session's (F199).* `GET /api/mesh/peers` builds
    its rows from the view the main loop published, one per member the
    session holds, and reads no NVS on the HTTP task; a stored key the
    session does not hold is not a member anything hears, so the rows
    always agree with `peers_total`.
  Pinned by host tests in `test_mesh_session` and `test_mesh_pairing`, by
  `check_canary_mesh_status.py` and the handlers' wiring tests (the
  first-counter save's in `scripts/tests/test_canary_mesh_first_counter_wiring.py`);
  the canary build is CI's; not bench-verified (the F136, F162, F199, F215 and F324 section
  of `docs/audit/hardware_verification_checklist.md`).
- **A canary-wap that left its opera stored an all-zero one (F113).** Its
  leave saved the zeroed opera config as it stood, the next boot loaded the
  zero `opera_id` and `opera_secret` as an opera, and the next pairing it
  started kept that opera and sealed the zero secret, which anyone can
  know, to the joiner (whose `opera_id`, derived from it, then differed
  from the initiator's stored zero one, so the two never heard each
  other; host-probed). The id and secret are now removed while no opera
  is configured, and a boot loads no all-zero id or secret, so NVS an
  older firmware's leave wrote is refused too. Host-tested only.
- **canary-wap kept its former members in NVS (F137).** Its member list is
  `peer_cnt` and `peer_0`..`peer_<n-1>`, each a public key, radio address
  and name, and a save removed no slot: after a removal the freed slot
  stayed, and after a leave every one did (host-probed), plaintext on the
  flash (next bullet), though nothing loaded them. The two counter
  records named them by fingerprint as well (`replay_ctrs` until the next
  5-minute save, `tx_ctrs` for good once no member was left). A save now
  removes every slot at or above the count, the last-seen record is saved
  at each removal and leave, and the send-counter record with no member
  left keeps its one counter under no fingerprint. The save stores the
  live slots and the count before it removes anything, so a full NVS
  that refuses a write leaves a list a boot reads whole (it used to leave
  a count over removed slots: the removed member back, a survivor lost,
  an all-zero member). A device updated with an older firmware's leftovers
  loses them at its first boot: the slots above the count, and, with
  flash encryption on and no opera, every slot an older leave kept. On an
  FE-off board the stored members stay until the next membership change
  (no opera loads there, so nothing reads them; sweep F141 decides what a
  boot does with them). What a dropped member leaves on purpose, by
  fingerprint only: its deny-list entry (spec §5.6, seven days,
  flash-encryption gated) when it was removed, and its last-seen
  tombstone (F116) when it was heard. Host-tested only.
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
  - **canary-wap: the rotation reaches no member (sweep F95, open;
    host-probed).** Its `MSG_OPERA_REKEY` is sealed under a per-member AUTH
    session key, and nothing opens a session: no code sends
    `AUTH_CHALLENGE`, and the exchange cannot complete as it stands (its
    `AUTH_RESPONSE` makes a 262 B signed frame against ESP-NOW's 250 B, and
    it runs X25519 over the long-term Ed25519 keys, so the two sides' keys
    would not agree; spec §3.1). So a removal moves the remover alone to a
    new `opera_id`; every survivor stays on the old one, split from the
    remover until it re-pairs with it, and keeps trusting the removed
    device, which only the remover now drops. The §5.6 caveat that the
    removed device cannot impersonate a member after the rotation does not
    hold on canary-wap yet. The fix is a wire and derivation change, with
    the crypto review (F48).
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
  - One outer frame for both trees (spec §4.5, `mesh_wire.h` staged
    byte-identical): the type byte of a signed frame used to mean different
    messages on the two trees (`TAMPER_ALERT` 18 vs 4; canary-wap's
    `CHANNEL_LOCK`/`HUB_ELECTION` on the PIO values of `OFFLINE_IMMINENT`/
    `WITNESS_RECORD`), so a validly signed frame could have been read as a
    different message on the other side. Both now take the version byte
    (1) and every type from one registry, the envelope is the frame (the
    PIO tree's unsigned type prefix is gone — one fewer unauthenticated
    byte, dispatch already keyed on the signed type), and a pairing type
    can never equal a version byte. What remains: the payloads of
    `TAMPER_ALERT` and the pairing exchange differ, so the trees still
    cannot pair, and a frame is only ever verified against a pinned peer
    key. This is a wire break with no negotiation: a pre-v0.4 and a
    post-v0.4 build of either tree drop each other's frames, so an opera
    is updated as a whole and re-paired (spec §4.5 "Compatibility").
    Host-tested (a frame built canary-wap's way verifies on the PIO
    session; version-0 and prefixed shapes are dropped); **maintainer
    crypto review pending; not bench-verified.**

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
