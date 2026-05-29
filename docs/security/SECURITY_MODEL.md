# SecuraCV Canary — Security & Privacy Model

> This document is included in every evidence export. It explains what
> the device does, what it can prove, and what its limitations are.
> It is written for a technically literate non-engineer: a journalist,
> a lawyer, a policy maker, or anyone evaluating evidence from this device.

---

## What This Device Does

SecuraCV Canary is a witness device. It creates tamper-proof records of
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

---

## What It Does NOT Collect

The device does not and cannot collect:

- WiFi network names (SSIDs) of nearby networks
- Device identifiers — MAC addresses are hashed before any storage and
  the originals are immediately discarded
- Audio or continuous video recording
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
the device, an adversary with sufficient power WILL extract it. The
only defense is making extraction impossible by design.

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

No custom cryptographic implementations are used. All primitives come
from established, audited libraries.

---

## Tamper Evidence

The witness chain is append-only:

- **Records can be added** — each new record extends the chain
- **Records cannot be modified** — changing any record breaks the chain
- **Records cannot be deleted** — missing sequence numbers are detectable
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
Mitigations include: no outbound network connections (a compromised blob
has nowhere to send data), secure boot (firmware is verified before
execution), and flash encryption (firmware cannot be trivially extracted).

### Physical Access
Physical possession of the device enables forensic analysis of the
SD card and flash storage. Secure boot and flash encryption raise the
difficulty but cannot guarantee protection against a state-level
adversary with unlimited physical access time.

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
