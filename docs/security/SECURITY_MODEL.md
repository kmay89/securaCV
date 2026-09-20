# SecuraCV Canary — Security & Privacy Model

> This document is included in every evidence export. It explains what
> the device does, what it can prove, and what its limitations are.
> It is written for a technically literate non-engineer: a journalist,
> a lawyer, a policy maker, or anyone evaluating evidence from this device.
>
> For a shorter companion focused on exports — why there is no "download
> the clip", why times are coarse, and how to handle a failed verification —
> see [Why SecuraCV exports work this way](../why_secure.md).

---

## What This Device Does

SecuraCV Canary is a witness device. It creates tamper-evident records of
events — nearby wireless devices, GPS location, environmental changes —
and chains them together cryptographically so that any alteration or
deletion is detectable.

Each record is:
1. **Hashed** into a chain (like blockchain, but on a single device)
2. **Signed** with the device's unique cryptographic key
3. **Sequenced** so gaps or missing records are visible

If any record is altered, removed, or inserted after the fact, the chain
visibly breaks and anyone checking the evidence can see the tampering.

---

## What It Can Prove

- That a specific device recorded specific events
- That events were recorded in a specific order
- That the chain has not been tampered with since creation
- That the GPS receiver reported a specific location at a specific time

## What It Cannot Prove

- That the device was physically present at the GPS-reported location
  (GPS signals can be spoofed by a sophisticated adversary)
- That the events described are "true" in an absolute sense — only that
  they were recorded by this specific device at this specific time
- The identity of the person operating the device
- That no events occurred between recorded intervals

---

## What It Collects

The device records only:

- **Timestamps** — rounded to 5-second intervals (never precise)
- **GPS coordinates** — if available, with configurable precision coarsening
- **Count of nearby WiFi devices** — not their identities
- **Device health data** — memory, storage, battery status
- **User-triggered events** — camera preview snapshots (on demand only)
- **Environmental changes** — motion state transitions (stationary/moving)
- **Coarse optical signals** (vision Canary) — occupancy as a bucket
  (none/one/two/several), a posture class (upright/ambiguous/horizontal), and a
  proximity band (far/mid/near), derived on-device from person bounding-box
  geometry. Ordinals only — never coordinates, gait, or identity

---

## What It Does NOT Collect

The device does not and cannot collect:

- WiFi network names (SSIDs) of nearby networks
- Device identifiers — MAC addresses are hashed before any storage and
  the originals are immediately discarded
- Audio or continuous video recording
- Skeletons, keypoints, gait, or body measurements — the vision Canary emits
  coarse ordinals derived from bounding boxes, never a biometric
- Exact per-person counts or occupancy histories — occupancy is a coarse
  bucket, not a running tally
- Browsing history or app data from nearby devices
- Any data from your phone when connected to the dashboard
- Usage analytics, telemetry, or crash reports
- Any information about what you do with the evidence

---

## How Privacy Is Protected

### No Phone-Home

No Canary contacts ERRERlabs, a cloud service, an analytics provider or a
crash reporter — the code to do so does not exist. A Canary WAP used as its
own WiFi Access Point makes no outbound connection at all: it is a server,
and it never speaks first. The products that join your home network open
exactly the paths disclosed in the next section and nothing else: the MQTT
broker you point them at (plain by default; CA-verified or SHA-256-pinned
TLS once you provision it, refusing to connect rather than downgrading), a
daily jittered check of a small signed update manifest on the products with
pull-OTA, and on the display line SNTP and the opt-in standalone forecast.
The third-party paths carry no identifier; the broker link carries your
device's own id and name to the broker you run, in every topic. The
forecast and the update fetch's URL policy are pinned by host tests
(`firmware/tests_host/test_wx_core.cpp`,
`firmware/common/ota/test_ota_logic.cpp`); SNTP's two hosts are literals
in the display firmware's `tz_auto.cpp`, with no test of their own.

This means:
- A WAP on its own access point is invisible to anyone watching your
  network; a networked Canary is visible exactly as its disclosed paths
  are, and no more
- No ERRERlabs server exists to be compelled: the only servers a Canary
  talks to are the broker and hub you run and the update host, which sees
  an anonymous manifest fetch
- Every product keeps witnessing without internet; only the disclosed
  paths go quiet

#### The networked products' disclosed outbound paths

Every product that joins a home network — the displays (Dash / Nightstand /
Watch Station) to render the fleet, the sense and vision Canaries and the
`firmware/canary` flagship to publish, a WAP once you join it to your WiFi —
carries only the outbound paths listed here. The third-party paths carry no
identifier (the broker link carries your device's own id and name, to the
broker you run) and none is required for the device to witness. A new
outbound path is a change to this list and to `THREAT_MODEL.md` Principle 2,
with a host test pinning its request shape the way
`firmware/tests_host/test_wx_core.cpp` pins the forecast.

1. **The MQTT broker you chose** — sense, vision, the flagship and the
   WAP's bridge publish to it; the displays subscribe. The socket is plain
   by default and TLS once you provision it — verified against a CA you
   supply on every product, pinned to the broker certificate's SHA-256
   fingerprint on the display line (not the plain-only nightstand-c6),
   sense, vision and the flagship; an incomplete setup refuses to connect
   rather than falling back ([`docs/FIRMWARE_VARIANT_AUDIT.md`](../FIRMWARE_VARIANT_AUDIT.md)).
   Compile-tested by CI, decision host-tested, not bench-tested against a
   TLS broker.
2. **Signed update checks** — on every product with pull-OTA (the
   flagship, WAP, vision, sense and the display line): a daily, jittered
   HTTPS GET of a small signed JSON manifest from the release host
   (`docs/firmware_ota.md`; the desktop Flasher can also ask the glass to
   run one over the LAN). No identifiers ride on it, and nothing is
   installed until the manifest verifies against the pinned Ed25519
   release key and you press Install — or turn on the per-device Auto
   Update switch, which is off by default. No setting turns the check on
   or off from the network, and it carries no location. Disclosed on the
   glass's own network page (`docs/hardware/display_settings.md`).

The display line adds three of its own:

3. **Time (SNTP)** — always on when networked: UTC from two public time
   sources (`pool.ntp.org`, `time.nist.gov`). This is what keeps a
   bedside clock honest. (The flagship and the WAP take their time from
   GPS; sense and vision have no clock source of their own — no GPS, no
   SNTP — and sense stamps `ts_ms` from uptime; the displays have no
   GPS, so they sync.)
4. **Timezone lookup** — compile-time opt-in only (`CD_TZ_WEB_LOOKUP` in
   `secrets.h`); off in every shipped image. Without it the zone comes
   from configuration or the app.
5. **Standalone weather** — the one *opt-in* path: runtime opt-in, off
   by default, and gated three ways (`firmware/.../net/wx_direct.h`): the
   owner must switch it on **on the glass itself**, a coarse location must
   be stored, and **no hub may ever have been configured**. A home with a
   hub keeps the hub as its single egress point — the fetcher never
   becomes a fallback when that hub is down. The switch is a hand on the
   glass, not a network call: the display's LAN write API (`POST /api/set`)
   refuses `wx_direct` and `wx_loc` for every caller, token or not, with
   `403 {"ok":false,"err":"on_glass_only"}` (one host-tested table,
   `net/settings_policy.h`), so a host on the home WiFi cannot flip the
   device's one opt-in outbound path or plant a location for it. `GET
   /api/settings` reports the opt-in's on/off to any caller, and whether a
   location is stored only to callers that are not cross-site. The query
   is an anonymous HTTPS forecast request (Open-Meteo, pinned root CA) over
   a 0.1° grid point (~11 km); the exact request shape is pinned by a host
   test (`firmware/tests_host/test_wx_core.cpp`) so it cannot quietly grow an
   identifier. The device never serves or republishes the stored grid
   point. The location is entered **on the glass itself**: the two 7"
   flavors that carry the standalone forecast (`dash7`, `nightstand7`)
   have a Location page under settings → weather — hemisphere, degrees
   and tenths wheels per axis, committed by one explicit *Use This
   Location* — whose helpers can only produce a point on the 0.1° grid
   inside the range the loader accepts (host-tested,
   `tests_host/test_display_settings.cpp`). There is no place-name lookup:
   entering a location makes no network request at all. **The stored cell
   is displayed on the glass** — on the Weather page's Location row and as
   the page's live caption — and that is a deliberate in-room disclosure:
   anyone who can stand at the glass and open its settings can read the
   ~11 km cell, exactly as they could read the forecast it produces. The
   disclosure stops at the glass: the LAN page and `GET /api/settings`
   still never carry the grid point, only whether one is stored. Closing
   the network path closed the phone app's `wx_loc` post for good; a grid
   point stored by an earlier build keeps working, *Forget Location* and a
   settings reset both clear it. Compile-tested by CI on the two 7" builds,
   wheel helpers host-tested, not yet bench-tested.

### No Tracking Identifiers

- The WiFi network name ("Canary-XXXX") reveals no manufacturer identity,
  serial number, or information linking back to ERRERlabs or the owner.
  The radio's hardware address is a separate disclosure: nothing in the
  firmware sets a derived or random MAC, so the access point's BSSID and
  the BLE advertising address are the chip's factory addresses and carry
  Espressif's OUI, as every ESP32's do — they say which vendor made the
  radio, not which device or owner this is
- BLE (NimBLE) is compiled into the shipped Canary WAP build
  (`BUILD_PROFILE_FULL`) for owner pairing and provisioning, the GATT
  status service, signed BLE firmware updates, the Scout scanner that
  attributes rooms to the owner's own paired beacons by hashed MAC, and
  two passive scanners: BLE presence, a listen-only feed into the same
  presence pipeline as the WiFi one that keeps no MAC, OUI or name from
  the scanner, and Nearby discovery, which recognizes other Canaries by
  the SecuraCV service UUID and only counts everything else. The MINIMAL
  profile compiles it out; DEV keeps the pairing channel (with the BLE
  OTA and the presence listener that ride it) and the status service.
  The `firmware/canary` flagship's `release` and `release_ha` images
  build it out; its `full` env compiles the Scout scanner and the GATT
  status service (`securacv_ble_scan`, `securacv_ble_status`), the only
  BLE code in that tree. Its advertisements carry the device's own name
  and the SecuraCV service UUID; the fleet beacon and Chirp add a
  manufacturer-data field under the Bluetooth SIG's reserved test id
  `0xFFFF` — type, flags, battery, health, chain height and the two
  fingerprint bytes already in the device's name (`fleet_beacon.h`) —
  never a serial. Nothing classes or tracks other people's devices. The
  S3 / C3 the BLE-bearing builds run on have no Classic (BR/EDR) radio;
  the classic-ESP32 boards have one and no build compiles a stack for it
- Service discovery (mDNS `_securacv._tcp`) is advertised only on the
  network the device is on — its own access point, or the home network you
  joined it to — never beyond it
- MAC addresses from nearby devices are cryptographically hashed — the
  device counts nearby devices without knowing or storing who they are

### Keys Never Leave the Device

The device's cryptographic identity (an Ed25519 private key) is:
- Generated on the device's own hardware random number generator
- Stored only on the device's internal flash memory
- Never transmitted over any interface (WiFi, USB, or otherwise)
- Not backed up, escrowed, or recoverable by anyone — including ERRERlabs

There is no "export private key" function. There is no backdoor, master
key, or recovery mechanism. If the device is lost or destroyed, the
evidence it already exported remains verifiable, but new evidence
requires a new device with a new identity.

**Why no backup?** Because a key that can be exported can be compelled.
A court order, a warrant, or physical coercion — if the key CAN leave
the device, an adversary with sufficient power WILL extract it. So there
is no software path that will hand it over: no API, no export, no
diagnostic, no debug interface, and nothing to compel us to hand over
because we never had it.

**The limit of that promise.** It is a statement about software. Someone
who physically takes the device and reads the flash chip directly is a
different adversary, and on a default-configured Canary they can recover
the key. That is a deliberate trade — the alternative permanently bricks
devices whose owners lose a key — and it is spelled out, with what it does
and doesn't let an attacker do, under
[Physical extraction and the flash-encryption default](#physical-extraction-and-the-flash-encryption-default).

---

## Who Can Access Your Data

Whoever can reach the device's network port. That boundary is the thing to
understand, product by product, because there is no second factor behind
it: no button press releases the dashboard, and the device cannot tell one
host on its network from another.

**On the device's own access point** — the Canary WAP, the `firmware/canary`
flagship, the headless products' setup portals — the boundary is the AP
password, unique per device. Anyone holding it is inside.

**On your home network, the boundary is your network.** The flagship serves
its dashboard and API as plain HTTP on port 80 on every interface, access
point and home network alike, and the page itself carries the API token to
whoever can load `GET /` or `/setup`. That token is a defense against
drive-by *web pages* — a page from another origin cannot read it — not
against a *host* on the same network, which can load the page and read the
token in one request. The
WAP's sensing dashboard has the same shape with a different mechanism: its
landing page mints a one-tap pairing link for whoever loads it, and the tap
becomes a 24-hour session cookie; the WAP's bearer token itself is handed
out only in the provisioning receipt (its gate opens for thirty seconds
after a short BOOT tap) or on the serial console. The displays treat the
LAN as their trust boundary by design — no bearer credential at all, a
per-boot CSRF token that keeps cross-site pages away from writes, and the
on-glass-only settings above that no network caller can flip.

**Encryption in transit is an opt-in, not the default.** The Canary WAP
serves HTTPS on port 443 with a certificate minted on the device once setup
has completed and the certificate loads; during first-boot setup, and when
the HTTPS server fails to start, it serves plain HTTP and says so on the
serial log. The flagship's dashboard, the displays' LAN page and the headless
products' setup portals are plain HTTP on your own network. The hub's API has
the same shape: plain on loopback by default, TLS through the kernel's
`api-tls` build feature. The iPhone app pins the WAP certificate fingerprint
carried in the pairing receipt and QR (`tls_cert_fp`), refuses an https
Canary it cannot check, and offers a plain-http credential push only when
nothing encrypted reaches the device and only behind a disclosure you switch
on. Compile-tested by CI; whether a given unit is serving HTTPS is on its
serial log and in its pairing receipt.

**One link is yours to encrypt: the MQTT broker.** A Canary that publishes
to a broker opens that socket in plain text by default, so the broker
username and password cross your LAN unencrypted until you provision TLS —
verified against a CA you supply, or pinned to the broker certificate's
SHA-256 fingerprint (which products support which is in
[`docs/FIRMWARE_VARIANT_AUDIT.md`](../FIRMWARE_VARIANT_AUDIT.md)). An
incomplete TLS setup refuses to connect rather than quietly downgrading, and
the unverified "lab" mode exists only as a mode chosen by name that warns on
every connect. On the flagship, provisioning the broker password over
`POST /api/mqtt/config` crosses the LAN in the clear like the rest of the
device API. This is compile-tested, host-tested, not yet bench-tested.

**One read on the hub is open on purpose: the fleet roll-call**
(`GET /api/fleet`). It answers anyone who can reach the kernel's port with
the coarse words the Witness Wall paints — each Canary's name, whether it is
online, its chain verdict and product, and the presence / occupants /
breathing words while it is proven online — never an event, a zone or a key.
That is why the kernel listens on loopback by default and the port is yours
to expose; the origin allow-list only stops other websites' scripts, not a
device on your LAN. And "online" there is a verified, chain-advancing
signature seen within three minutes — stronger than a heartbeat, but not a
liveness proof against someone who can publish on your MQTT broker
(`tvos/discovery/DISCOVERY.md` spells out the difference).

---

## Who Cannot Access Your Data

| Who | Why Not |
|-----|---------|
| **ERRERlabs** (the manufacturer) | We have no remote access capability. No backdoor exists. |
| **Law enforcement** (without the physical device) | There is no ERRERlabs server to subpoena. A Canary talks only to the broker and hub you run and to the update host, which sees an anonymous manifest fetch; the evidence lives on the device and on your hub. |
| **Network observers** | They see the disclosed paths and nothing else: a broker session (readable on the wire until you provision TLS), a daily signed-manifest fetch and, on the display line, SNTP. No path carries an identifier beyond what your own broker login already is. |
| **Other WiFi users** | Each device has a unique, randomly derived password. |
| **Remote attackers** | Nothing listens beyond your network and nothing is exposed to the internet; the outbound paths are client-initiated to hosts you chose. Encryption on the device's own API is an opt-in — the WAP's HTTPS after setup, the kernel's `api-tls` — and the MQTT broker link is TLS only once you provision it (plain by default). |
| **ERRERlabs under court order** | We cannot comply because we have nothing — no keys, no data, no access. |

---

## Cryptographic Design

The device uses well-vetted, standard cryptographic primitives:

| Purpose | Algorithm | Library |
|---------|-----------|---------|
| Device identity & record signing | Ed25519 | Arduino Crypto (Rhys Weatherley) |
| Chain integrity & domain separation | SHA-256 | mbedTLS (ESP-IDF) |
| API token derivation | HMAC-SHA256 / HKDF | mbedTLS (ESP-IDF) |
| Transport encryption (opt-in: the WAP's HTTPS after setup; the kernel's `api-tls` feature) | TLS 1.2+ (RSA-2048 self-signed on the WAP) | mbedTLS (ESP-IDF); rustls (kernel) |
| Broker link (MQTT over TLS, when provisioned) | TLS 1.2+; CA chain verification on all five products; SHA-256 certificate pin and a named unverified lab mode on canary-display (not the plain-only nightstand-c6), canary-sense, canary-vision and the `firmware/canary` flagship; canary-wap is CA-only (esp_mqtt has no pin hook). Compile-tested, host-tested, not bench-tested | mbedTLS via WiFiClientSecure (display / sense / vision / flagship) and esp-tls (canary-wap) |
| At-rest event database (kernel) | SQLCipher (AES-256), key derived from the device seed | SQLCipher via rusqlite |

No custom cryptographic implementations are used. All primitives come
from established, audited libraries.

---

## Tamper Evidence

The witness chain is append-only:

- **Records can be added** — each new record extends the chain
- **Records cannot be modified undetectably** — changing any record breaks the chain
- **Records cannot be deleted undetectably** — missing sequence numbers are detectable
- **The chain break itself is evidence** — tampering is visible to anyone
  who checks

To forge a record, an adversary would need:
1. The device's Ed25519 private key (which never leaves the device)
2. To re-hash and re-sign every subsequent record in the chain

To hide a deletion, an adversary would need to re-sign the entire chain
from the deletion point forward — which again requires the private key.

---

## Evidence Verification

Evidence exported from this device can be verified by **anyone**,
**anywhere**, without contacting ERRERlabs or any other service:

### Option 1: Built-in Verifier
Open the included `verification.html` file in any web browser.
It runs entirely offline — no internet connection required.

### Option 2: Online Verifier
Visit verify.securacv.com and upload the export bundle.

### Option 3: Manual Verification
Using any Ed25519 implementation:
1. The device's public key is in `device_identity.json`
2. Each record's Ed25519 signature can be checked independently
3. The SHA-256 hash chain can be recomputed from the first record
4. Sequence numbers can be checked for gaps

**No ERRERlabs service, account, or software is required for
verification.** If ERRERlabs ceased to exist tomorrow, all previously
exported evidence would remain fully verifiable.

---

## Known Limitations

We believe transparency about limitations is as important as describing
capabilities. The following are known constraints:

### Hardware Trust Boundary
The Canaries are built on Espressif ESP32-family microcontrollers (S3,
C3, C6 and the classic ESP32 — `firmware/boards/boards.json`).
Espressif is a Chinese semiconductor company. The WiFi firmware contains
proprietary binary blobs from Espressif. A sufficiently resourced
state-level adversary could theoretically compromise these blobs.
The mitigation that always applies is architectural: the firmware opens
no undisclosed outbound path — the few it has are named above, two of
them pinned by host tests — so a compromised blob would have to
originate traffic of its own, on a network whose expected traffic is
short and known, and a WAP on its own access point gives it no route
out at all. Secure boot and flash encryption are additional
mitigations, but they are **off unless you turn them on** — see
[Physical extraction and the flash-encryption default](#physical-extraction-and-the-flash-encryption-default).

### Physical Access
Physical possession of the device enables forensic analysis of the
SD card and flash storage. On a device in its default configuration this
includes recovering the device's private key; the section below states
exactly what that does and does not let an attacker do.

### GPS Spoofing
The device cannot independently verify GPS signals. A sophisticated
adversary with GPS spoofing equipment could cause the device to record
false location data. The device records what its GPS receiver reports,
not objective ground truth.

### Clock Accuracy
Timestamps come from GPS satellites when a fix is available. Without
GPS fix, the device uses its internal clock, which may drift. All
timestamps are coarsened to 5-second buckets regardless of source.

### Evidence Scope
The device records metadata about events, not comprehensive multimedia
evidence. It proves that *something was recorded at a time and place*,
not necessarily *what happened* in full detail.

### Physical extraction and the flash-encryption default

**The honest statement: on a Canary in its default configuration, someone
who takes the device away and opens it up can read the private key out of
the flash chip.**

This is a deliberate default, not an oversight, and the trade is worth
understanding because it is the one place where "keys never leave the
device" needs an asterisk.

ESP32-family chips support Secure Boot and flash encryption, which together
make the flash contents unreadable and stop unsigned firmware running.
They are also **irreversible**: they are burned into one-time fuses. A
device with them enabled and a lost key is a brick, permanently, with no
recovery path — not for you, and not for us. We decided that a default
that can permanently destroy an owner's device is the wrong default for a
device people are supposed to be able to keep, repair, and re-flash. So
[the tiered design](../design/hardware_root_of_trust.md) puts the
reversible protections in the default path and leaves the irreversible
lockdown as an explicit, key-backup-enforced opt-in. The settings are
staged and commented in `firmware/provisioning/sdkconfig.defaults.secure`.

**What key recovery gets an attacker.** They can sign new records as that
device. From that point on, a chain they produce is cryptographically
indistinguishable from one the real device produced.

**What it does not get them.** It does not rewrite the past. The log is
hash-chained, so altering an existing record breaks every record after it;
and where chain heads have been anchored to an external timestamp
authority (`log_anchor`, RFC 3161), the history up to each anchor is
pinned by a signature that is not the device's and that key theft does not
provide. Forging *forward* from a stolen key is achievable. Forging
*backward* past an anchor means defeating the timestamp authority as well.
It also does not decrypt sealed material, and that holds on both sides
for the same structural reason — the decryption key is somewhere the
device isn't. Sealed snapshots on the device are encrypted to an
**operator-held X25519 public key whose private half never touches the
device at all** (see [`../sealed_snapshot_vault.md`](../sealed_snapshot_vault.md)),
so there is nothing on the flash to recover that would open them. Vault
material on the hub is protected by a separate hub-held key under the
break-glass policy. Neither is the device identity key, so stealing the
identity key opens neither.

**It is also detectable, after the fact.** A cloned identity produces two
divergent chains from the same device key. Two chains that share a prefix
and then disagree is not something the real device can do, so if both ever
reach a verifier, the clone is provable.

**If your threat model includes device seizure**, this is the case the
opt-in tiers exist for. Turn them on deliberately, back up the key first,
and understand that you are trading recoverability for extraction
resistance. Read [`../design/hardware_root_of_trust.md`](../design/hardware_root_of_trust.md)
before you burn a fuse — that decision cannot be walked back.

**If your threat model is an intruder, a landlord, or a domestic abuser
who does not take the device to a lab**, the default is the right one for
you, and the tamper-evidence properties are unaffected.

---

## The Design Standard

This device is designed for people whose safety may depend on it:
tenants documenting unauthorized entry, journalists protecting source
meetings, activists recording police presence, domestic abuse survivors
documenting patterns, whistleblowers who need tamper-evident records.

Every design decision is evaluated against the most vulnerable user.
If the device fails for a journalist in a hostile state, it does not
matter that it works for an insurance claim.

The security model is based on **architecture, not promises**:

```
Promise-based security:       "We won't look at your data."
Architecture-based security:  "We CAN'T look at your data."

SecuraCV Canary implements the latter.
```

---

## Open Source

The firmware source code is publicly available at:
https://github.com/kmay89/securaCV

Anyone can:
- Audit the code for backdoors or vulnerabilities
- Build the firmware from source and compare it to what ships on the device
- Verify that the security claims in this document match the implementation
- Fork the project and modify it for their own needs

---

## Contact

To report a security vulnerability:
https://github.com/kmay89/securaCV/issues (use the Security Report template)

This document is versioned alongside the firmware. The version that
matches your device's firmware is the authoritative reference.
