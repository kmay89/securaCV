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

The device makes **zero outbound network connections**. It does not
contact any server — not ERRERlabs, not any cloud service, not any
analytics provider. It runs its own WiFi Access Point and acts as a
server. It never speaks first.

This means:
- No one can detect the device exists by monitoring network traffic
- No server can be compelled to reveal device data
- The device works identically with or without internet nearby

#### The display line's disclosed exceptions

The Canary displays (Dash / Nightstand / Watch Station) join the home
WiFi to render the fleet, and carry exactly four disclosed outbound
paths — all anonymous queries, none carrying identifiers, and none
required for the device to function:

1. **Time (SNTP)** — always on when networked: UTC from two public time
   sources (`pool.ntp.org`, `time.nist.gov`). This is what keeps a
   bedside clock honest.
2. **Timezone lookup** — compile-time opt-in only (`CD_TZ_WEB_LOOKUP` in
   `secrets.h`); off in every shipped image. Without it the zone comes
   from configuration or the app.
3. **Standalone weather** — the one *opt-in* path: runtime opt-in, off
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
   test (`tests_host/test_wx_core.cpp`) so it cannot quietly grow an
   identifier. The device never serves or republishes the stored grid
   point. Honest status: closing the network path also closed the only
   way a location was ever stored (the phone app posted `wx_loc`), and
   this firmware has no on-glass location entry yet — so on a fresh
   device the second gate stays unsatisfied and the fetcher stays idle
   until one lands. A grid point stored by an earlier build keeps working;
   a settings reset clears it. Compile-tested, not yet bench-tested.
4. **Signed update checks** — a daily, jittered HTTPS GET of a small
   signed JSON manifest from the release host (`docs/firmware_ota.md`;
   the desktop Flasher can also ask the glass to run one over the LAN).
   No identifiers ride on it, and nothing is installed until the manifest
   verifies against the pinned Ed25519 release key. No setting turns this
   on or off from the network, and it carries no location. Disclosed on
   the glass's own network page (`docs/hardware/display_settings.md`).

### No Tracking Identifiers

- The WiFi network name ("Canary-XXXX") reveals no manufacturer identity,
  serial number, or information linking back to ERRERlabs or the owner
- Bluetooth is disabled at compile time (the code to enable it does not
  exist in standard firmware)
- No service discovery broadcasts beyond the device's own WiFi network
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

Only someone with **all three** of the following:

1. **Physical proximity** to the device (WiFi range, approximately 30 meters)
2. **The device's WiFi password** (unique per device, set during provisioning)
3. **The API access code** (if enabled — displayed only via physical button press)

All communication between your phone/computer and the device is
encrypted with TLS (the same encryption used by banks and secure websites).

---

## Who Cannot Access Your Data

| Who | Why Not |
|-----|---------|
| **ERRERlabs** (the manufacturer) | We have no remote access capability. No backdoor exists. |
| **Law enforcement** (without the physical device) | The device makes no network connections. There is no server to subpoena. |
| **Network observers** | The device creates no outbound traffic to intercept. |
| **Other WiFi users** | Each device has a unique, randomly derived password. |
| **Remote attackers** | No internet connection, no exposed services, TLS on all local traffic. |
| **ERRERlabs under court order** | We cannot comply because we have nothing — no keys, no data, no access. |

---

## Cryptographic Design

The device uses well-vetted, standard cryptographic primitives:

| Purpose | Algorithm | Library |
|---------|-----------|---------|
| Device identity & record signing | Ed25519 | Arduino Crypto (Rhys Weatherley) |
| Chain integrity & domain separation | SHA-256 | mbedTLS (ESP-IDF) |
| API token derivation | HMAC-SHA256 / HKDF | mbedTLS (ESP-IDF) |
| Transport encryption | TLS 1.2+ (RSA-2048) | mbedTLS (ESP-IDF) |
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
The device is built on the Espressif ESP32-S3 microcontroller.
Espressif is a Chinese semiconductor company. The WiFi firmware contains
proprietary binary blobs from Espressif. A sufficiently resourced
state-level adversary could theoretically compromise these blobs.
The mitigation that always applies is architectural: the device makes
**no outbound network connections**, so a compromised blob has nowhere
to send anything. Secure boot and flash encryption are additional
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

The ESP32-S3 supports Secure Boot and flash encryption, which together
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
documenting patterns, whistleblowers who need tamper-proof records.

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
