# Beacon Channel Protocol v0 (Neighborhood Harm-Reduction Network)

Status: Draft v0.1
Intended Status: Normative
Last Updated: 2026-05-11
Companion specs: `spec/chirp_channel_v0.md` (v0.2), `spec/canary_mesh_network_v0.md` (Opera), `spec/beacon_cap_gateway_v0.md` (CAP interop, deferred implementation)
Research basis: `docs/research/harm_reduction_prior_art.md`

## 1. Purpose and operating philosophy

The Beacon Channel is a **higher-trust, narrowly-scoped, supervised-health broadcast layer** for genuine life-safety advisories within a building, block, or neighborhood. It sits next to the Chirp channel on the same ESP-NOW radio but enforces a fundamentally different trust model.

**Operating bar:** smoke-detector-grade reliability. Boring, mostly silent, unmissable when it matters, never impersonating an official alert, never abusable by a single bad actor.

**Beacon is NOT:**

- A neighborhood-watch surveillance system.
- A way to report individuals, vehicles, or "suspicious activity".
- A replacement for 911 / 999 / 112.
- An IPAWS / WEA / EAS originator.
- An entertainment, social, or "engagement" channel.

**Beacon IS:**

- A community smoke detector. A few devices in a building agree something life-safety-relevant is happening, and the rest of the building hears about it within seconds.
- A supervised-health system in the NFPA 72 sense: it knows when it is healthy and when it is not, and announces both states honestly.
- An interoperable substrate. The wire format and template set are designed so that a properly-authorized future gateway operator (NWS, building management with FEMA designation) could feed CAP alerts in, and so that local logs can be exported in a CAP-compatible form for after-action review.

## 2. Relationship to Chirp and Opera

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                         BEACON CHANNEL  (this spec)                          │
│   Persistent device pubkey identity • Multi-pubkey co-signed origination     │
│   ~13 life-safety templates only • NFPA-72 supervised health • CAP-aligned   │
│                                                                              │
│        ┌──────────────────────────────────────────────────────────┐          │
│        │                    CHIRP CHANNEL                         │          │
│        │   Ephemeral session identity • Anonymous • Soft alerts   │          │
│        │   ~25 templates incl. authority/utility/aid • 3-hop      │          │
│        │                                                          │          │
│        │     ┌──────────────────────────────────────────┐         │          │
│        │     │              OPERA MESH                  │         │          │
│        │     │   Trusted • Encrypted • Household-scope  │         │          │
│        │     └──────────────────────────────────────────┘         │          │
│        └──────────────────────────────────────────────────────────┘          │
└──────────────────────────────────────────────────────────────────────────────┘
```

| Dimension | Opera | Chirp | Beacon |
|---|---|---|---|
| Identity | Persistent device Ed25519 | Ephemeral session Ed25519 | Persistent device Ed25519 |
| Trust model | Household-shared `opera_secret` | None (anonymous) | Building-shared `beacon_set` of paired device pubkeys |
| Origination | Single device | Single device + ≥2 witness ACKs to relay | **≥2 device pubkeys co-sign at hop 0** |
| Magic byte | (carried in opera_id) | `0xC4` | `0xB1` |
| Templates | None (control plane) | ~25, broad | ~13, life-safety only |
| Wire size | Variable | ~110 B | ~250 B (signed twice) |
| Range | Household RF | 3 hops, ~750 m | 3 hops, ~750 m |
| Channel | Follows STA | Follows STA | Follows STA |
| Airtime class | Routine + urgent | Routine + urgent | **Always urgent** |
| Persistence | Always-on | Opt-in | Always-on once paired |
| Audio | None | `PATTERN_ALERT` (2 kHz, 200 ms) | `PATTERN_BEACON` (3-tone, ≤600 ms, ≠ WEA) |
| UI color | n/a | Blue/yellow/orange | Orange/amber (never red) |
| State surface | `MeshState` enum | `ChirpState` enum | **NFPA-72: `Normal/Trouble/Alarm/Supervisory`** |

Critically: the three channels share the radio (via `mesh_channel_policy`) and the airtime budget (via `airtime_governor`), but they share **no** keys, **no** identities, **no** opera_id leakage. A device's Beacon participation does not reveal its Opera membership; a device's Chirp activity is cryptographically unlinkable to its Beacon device pubkey.

## 3. Identity and trust model

### 3.1 Beacon device identity

A device's Beacon identity is its persistent Ed25519 device pubkey — **the same pubkey used by Opera and witness records**. This is a deliberate choice:

- Reuses the existing key-management infrastructure (NVS, eFuse-derived flash encryption, key generation at first boot).
- Allows Beacon receivers to also be Opera members (a typical deployment: 1–3 Canaries in your home are Opera peers AND members of the building Beacon set).
- Provides a single auditable identity per device for safety-critical claims.

The device pubkey is **public** by design — it's announced during pairing and is the verification key for every Beacon frame the device originates. Treating it as a secret is incorrect; treating it as PII is also incorrect (it's a pseudonymous identifier, no more linkable to a human than a MAC address, and it never travels with location).

### 3.2 Beacon set

A **beacon set** is a list of trusted device pubkeys that may co-sign or originate Beacon frames. Each device maintains its own beacon set independently. Sets typically overlap heavily within a building.

Beacon set entries:

```
beacon_set_entry = {
  device_pubkey:   bstr .size 32,
  device_name:     tstr .size 24,        ; user-friendly name, local only
  paired_at:       uint,                 ; unix timestamp (seconds)
  fingerprint:     bstr .size 8,         ; SHA-256(device_pubkey)[0:8]
  trust_level:     uint,                 ; 0=cosigner, 1=gateway, 2=revoked
  last_selftest:   uint / null,          ; unix timestamp of last selftest heard
}
```

Maximum set size: 32 entries per device. Persisted to NVS, gated behind flash encryption (see `spec/canary_mesh_network_v0.md` Opera secret rules).

### 3.3 Pairing

Beacon pairing uses the same visual-confirmation flow as Opera pairing (`spec/canary_mesh_network_v0.md` §2.5), but produces a **limited** trust relationship:

- No `opera_secret` is exchanged.
- No `opera_id` is established.
- Both devices simply add the other's device pubkey to their beacon set with `trust_level = 0` (cosigner).

A Beacon-paired neighbor cannot read Opera traffic, cannot derive `opera_id`, cannot become an Opera member. The privacy boundary is intentional and absolute.

Pairing flow:

1. Initiator presses "Add neighbor" in UI; device enters `BEACON_PAIR_INIT`.
2. Joiner presses "Join neighbor" on their device; enters `BEACON_PAIR_JOIN`.
3. Ephemeral X25519 ECDH; 6-digit confirmation code derived from session key.
4. Both users confirm the code matches.
5. Each device sends a signed `BEACON_PAIR_OFFER` containing its device pubkey.
6. Each device verifies the offer's signature and adds the other's pubkey to its beacon set.
7. Pairing complete in ≤120 s or aborts.

### 3.4 Revocation

Any device can unilaterally remove another device from its own beacon set. Removal is local — no broadcast, no propagation. The removed device's Beacon frames stop being accepted by this device.

To revoke globally across a building, every cosigner must remove the pubkey from their own set. The system does not promise consistent revocation across the set; it promises that no single device's revocation can disable a healthy member.

A `trust_level = 2` (revoked) state is supported: the pubkey is kept locally for logging purposes ("this revoked device tried to originate") but never accepted as a signer.

### 3.5 Gateway pubkeys (forward-looking)

Reserved: pubkeys may carry `trust_level = 1` (gateway). A gateway-trust pubkey may originate solo (no co-signer required) but only if the frame carries a verifiable second signature from the upstream feed (e.g., CAP XML-DSig). Receivers display gateway-originated frames with a clear "from gateway X" badge and **never** at higher urgency than community-originated frames. This is fully specified in `spec/beacon_cap_gateway_v0.md`; no implementation in v0.

## 4. Templates (life-safety only)

Beacon's template set is deliberately **narrower** than Chirp's. Templates are restricted to genuine life-safety advisories where false alarms are tolerable but bursts of false alarms would erode trust.

| Code | Template ID | Display text | CAP category | CAP responseType | Default urgency | Default severity |
|---|---|---|---|---|---|---|
| 0x20 | `BCN_EMERG_FIRE_VISIBLE` | "fire or smoke visible" | Fire | Evacuate | Immediate | Severe |
| 0x21 | `BCN_EMERG_MEDICAL_SCENE` | "medical emergency scene" | Health | Monitor | Expected | Moderate |
| 0x22 | `BCN_EMERG_MULTIPLE_AMBULANCE` | "multiple ambulances responding" | Health | Monitor | Expected | Moderate |
| 0x23 | `BCN_EMERG_EVACUATION` | "evacuation in progress" | Safety | Evacuate | Immediate | Severe |
| 0x24 | `BCN_EMERG_SHELTER_IN_PLACE` | "shelter in place advisory" | Safety | Shelter | Immediate | Severe |
| 0x12 | `BCN_INFRA_GAS_SMELL` | "gas smell, possible leak" | Infra | Evacuate | Immediate | Severe |
| 0x10 | `BCN_INFRA_POWER_OUT` | "extended power outage" | Infra | Monitor | Future | Minor |
| 0x30 | `BCN_WX_SEVERE_WARNING` | "severe weather warning" | Met | Monitor | Expected | Severe |
| 0x31 | `BCN_WX_TORNADO` | "tornado warning" | Met | Shelter | Immediate | Extreme |
| 0x32 | `BCN_WX_FLOOD` | "flooding reported" | Met | Avoid | Expected | Severe |
| 0x80 | `BCN_CLR_RESOLVED` | "situation resolved" | Safety | AllClear | Past | Minor |
| 0x81 | `BCN_CLR_SAFE` | "area appears safe now" | Safety | AllClear | Past | Minor |
| 0x82 | `BCN_CLR_FALSE_ALARM` | "false alarm" | Safety | AllClear | Past | Minor |

**Excluded by design** (and the reason for each):

| Excluded | Reason |
|---|---|
| Any "authority presence" / "police activity" / "federal presence" | Higher-trust origination ≠ higher-trust attestation about contested events. Lives in Chirp (lower trust) only. |
| "Suspicious person", "unfamiliar vehicle", any individual description | Anti-profiling invariant — same rule as Chirp. |
| Mutual aid templates ("supplies needed", "welfare check") | Life-safety scope only. Coordination belongs in Chirp. |
| Internet outage, road closures, water disruption | Not life-safety. Belongs in Chirp. |
| Missing persons, lost pets, AMBER-style alerts | Not in scope; would require identifying details we will not carry. |

The template set is part of the wire format; adding or removing a template requires a `BEACON_PROTOCOL_VERSION` bump.

## 5. Wire format

All Beacon frames begin with `magic = 0xB1` and `version`. Beacon does not use CBOR — it uses packed binary structs with explicit field order for size and parse predictability.

### 5.1 Header (24 bytes)

```c
struct BeaconHeader {
  uint8_t  magic;              // 0xB1
  uint8_t  version;            // BEACON_PROTOCOL_VERSION (initially 1)
  uint8_t  msg_type;           // BeaconMsgType
  uint8_t  hop_count;          // 0..3
  uint8_t  flags;              // bit 0: is_exercise, bit 1: is_test, bits 2-7: reserved
  uint8_t  reserved;
  uint16_t payload_len;        // network byte order
  uint8_t  nonce[16];          // random per origination, preserved across relays
};
```

### 5.2 Canonical alert payload (signed twice)

```c
struct BeaconAlertCanonical {
  uint64_t effective;          // unix timestamp seconds; origination time
  uint64_t expires;            // unix timestamp seconds; effective + ttl
  uint8_t  template_id;        // BeaconTemplate
  uint8_t  msg_type;           // BeaconMsgType: Alert | Update | Cancel | Exercise
  uint8_t  urgency;            // CAP: Immediate=0 | Expected=1 | Future=2 | Past=3 | Unknown=4
  uint8_t  severity;           // CAP: Extreme=0 | Severe=1 | Moderate=2 | Minor=3 | Unknown=4
  uint8_t  certainty;          // CAP: Observed=0 | Likely=1 | Possible=2 | Unlikely=3 | Unknown=4
  uint8_t  scope;              // always 2 (Private)
  uint8_t  detail_slot;        // BeaconDetailSlot (constrained, no free text)
  uint8_t  ref_canceled_nonce[16];  // for Cancel/Update msgType; zeroed otherwise
  uint8_t  originator_pubkey[32];
  uint8_t  cosigner_pubkey[32];
};
```

Signed message construction:

```
canonical_bytes = "securacv:beacon:canonical:v0" || BeaconAlertCanonical (packed)

sig_originator = Ed25519_Sign(originator_privkey, canonical_bytes)
sig_cosigner   = Ed25519_Sign(cosigner_privkey,   canonical_bytes)
```

The full alert frame on the wire:

```
[BeaconHeader (24 B)]
[BeaconAlertCanonical (~130 B)]
[sig_originator (64 B)]
[sig_cosigner   (64 B)]
```

Total ~282 B. With careful packing the payload fits within 250 B by:
- Reducing pubkey size to 24 B (truncated SHA-256(pubkey)[0:24]) since pubkeys are already in the beacon set
- OR using a 2-byte beacon-set index in lieu of pubkeys for the canonical body
- OR splitting into two frames: a signed announce + a signed cosign reference

**Decision:** use beacon-set fingerprint (16 bytes = SHA-256(pubkey)[0:16]) in the canonical instead of full pubkey. Receivers look up the full pubkey from the local beacon set. Saves 32 bytes; total ~250 B fits within the ESP-NOW ceiling.

Updated canonical:

```c
struct BeaconAlertCanonical {
  uint64_t effective;
  uint64_t expires;
  uint8_t  template_id;
  uint8_t  msg_type;
  uint8_t  urgency;
  uint8_t  severity;
  uint8_t  certainty;
  uint8_t  scope;              // 2 (Private)
  uint8_t  detail_slot;
  uint8_t  ref_canceled_nonce[16];
  uint8_t  originator_fp[16];  // SHA-256(originator_pubkey)[0:16]
  uint8_t  cosigner_fp[16];    // SHA-256(cosigner_pubkey)[0:16]
};
```

Total alert frame: 24 (header) + 65 (canonical, packed) + 64 + 64 = 217 B. Comfortably fits.

### 5.3 Other message types

```c
enum BeaconMsgType : uint8_t {
  BEACON_MSG_ALERT       = 0,  // dual-signed alert
  BEACON_MSG_UPDATE      = 1,  // dual-signed amendment of an active alert
  BEACON_MSG_CANCEL      = 2,  // dual-signed all-clear
  BEACON_MSG_EXERCISE    = 3,  // dual-signed drill
  BEACON_MSG_SELFTEST_OK = 4,  // self-signed daily health beacon (single signature)
  BEACON_MSG_PAIR_OFFER  = 5,  // self-signed pairing handshake
  BEACON_MSG_REVOKE      = 6,  // self-signed local revocation announcement
};
```

#### BEACON_MSG_SELFTEST_OK

Daily health beacon, **single signature** (the device about itself). Routed through `airtime_governor::try_reserve_routine()` (not urgent — self-test never preempts user traffic).

```c
struct BeaconSelfTestPayload {
  uint64_t timestamp;
  uint32_t uptime_sec;
  uint16_t free_heap_kb;
  uint8_t  key_self_test_ok;        // 1 if Ed25519 sign/verify round-trip OK
  uint8_t  beacon_set_size;
  uint16_t reserved;
  uint8_t  device_fp[16];           // own pubkey fingerprint
  uint8_t  signature[64];           // Ed25519_Sign(device_privkey, canonical)
};
```

Receivers maintain `last_selftest_seen[device_fp] = timestamp`. Absence for >36 h transitions the receiver to `Trouble` state, naming the missing device in MQTT telemetry.

### 5.4 Wire format invariants (CI-enforced)

The lint script (`scripts/lint_no_impersonation.sh`) and the host tests enforce:

- `magic == 0xB1` on every frame.
- `version == BEACON_PROTOCOL_VERSION` (currently 1).
- `scope == 2` (Private) on every alert.
- Both signatures verify against pubkeys in the local beacon set.
- `originator_fp != cosigner_fp`.
- For `BEACON_MSG_CANCEL` and `BEACON_MSG_UPDATE`, `ref_canceled_nonce != 0`.
- For `BEACON_MSG_EXERCISE`, `flags & 1 == 1` AND `status` in derived CAP export is `Test` or the export carries `msgType=Exercise`.

## 6. Origination — the two-pubkey rule

Beacon origination requires **two distinct device pubkeys** to sign the canonical message before any frame is emitted at hop 0. There is no on-the-wire "single signature acceptable" state. A receiver that gets a frame with only one signature (or two signatures from the same pubkey) drops it on the floor and logs a security event.

### 6.1 Standard two-device flow (multi-device household, paired building set)

1. User A holds-to-send on Device A: e.g. "fire or smoke visible." Device A:
   - Constructs `BeaconAlertCanonical` with `certainty = Likely`, the chosen template, `effective = now()`, `expires = now() + ttl`.
   - Signs canonical with its own privkey → `sig_originator`.
   - Picks a candidate cosigner — first paired device in beacon set with `last_seen < BEACON_COSIGN_FRESHNESS_MS` (default: 10 min).
   - Sends a local `BEACON_COSIGN_REQUEST` to the candidate (Opera-encrypted if both are in same Opera, otherwise Beacon-set-encrypted via ChaCha20-Poly1305 keyed by ECDH).
2. Device B receives the request. The UI prompts User B: "Device A is reporting *fire or smoke visible* — do you also see it? Hold to confirm." This prompt is calm, blue, no flashing.
3. User B holds-to-confirm within `BEACON_COSIGN_WINDOW_MS` (default: 60 s). Device B:
   - Verifies `sig_originator` against Device A's pubkey from local beacon set.
   - Verifies template ID and severity are within Device B's allowed list.
   - Signs the same canonical → `sig_cosigner`.
   - Sends `BEACON_COSIGN_RESPONSE` back to Device A containing only `sig_cosigner`.
4. Device A receives the response, verifies `sig_cosigner` against Device B's pubkey, assembles the full Beacon frame, and emits at hop 0 onto the broadcast channel.
5. Receivers verify both signatures, both fingerprints are in their local beacon set, neither is revoked, both have `last_selftest_seen` within 36 h, then transition to `Alarm` state and play `PATTERN_BEACON`.

If User B does not confirm within 60 s, the origination expires silently. No alert is broadcast. The originator's UI returns to normal with "no cosigner available — try again or escalate."

### 6.2 Solo-degraded flow (single-device household)

When the device has no paired neighbor in its beacon set (or none with `last_seen` within freshness window), the operator can still originate a Beacon via the **physical BOOT button cosigner** pattern:

1. User holds the BOOT button down.
2. While holding BOOT, user presses hold-to-send in the UI for 2 seconds.
3. Device treats the simultaneous BOOT-press as the cosigner authorization. The cosigner signature is generated using a derived `boot_key` from the device's eFuse-protected unique ID, distinct from the regular device key (this prevents a software-only attacker from forging the BOOT cosignature).
4. The canonical is constructed with `certainty = Observed` (not `Likely`, not `Confirmed`) and the `cosigner_fp` is the BOOT-key fingerprint.
5. Receivers that recognize the device as a member of their beacon set will display the alert but with a "single-device origination" badge and at one notch lower urgency than a dual-device-co-signed Beacon.

This makes the trust gradient visible: receivers can downweight solo Beacons without rejecting them.

### 6.3 Co-sign request encryption

The `BEACON_COSIGN_REQUEST` and `BEACON_COSIGN_RESPONSE` messages are device-to-device, not broadcast. They MUST be encrypted to prevent a third party from substituting a different canonical (and thus tricking a cosigner into signing the wrong thing).

If both devices are in the same Opera: use the existing Opera session key.
If they are only Beacon-paired: derive a fresh ChaCha20-Poly1305 session key via X25519 ECDH between device pubkeys.

The cosigner MUST decrypt, parse the canonical, display it to the user, and only sign after explicit confirmation. The cosigner UI MUST display:
- The originator's device name (from local beacon set entry).
- The full template text.
- The severity and urgency.
- A "decline" button as visible as the "confirm" button.

### 6.4 Anti-coercion guard

The cosign request includes the originator's `boot_button_state` (whether the BOOT button is currently pressed). If a cosigner receives a request where the originator claims `boot_button_state = 0` but the receiver's UI sees signs of duress (e.g., very fast repeated requests), the receiver MAY refuse silently. This is a soft mitigation — the real defense against coercion is operational, not cryptographic.

## 7. Reception and state surface

### 7.1 Validation pipeline

Every received Beacon frame goes through:

1. `magic == 0xB1`? Drop if not.
2. `version` recognized? Drop if not.
3. Bloom-filter dedup on `nonce`? Drop if duplicate.
4. `effective` within `time(nullptr) ± BEACON_FRESHNESS_S` (default ±300 s)? Drop if not. If `time(nullptr) < 1700000000` (unsynced), accept but flag `unverifiable_timestamp = true`.
5. Both `originator_fp` and `cosigner_fp` in local beacon set with `trust_level != revoked`? Drop if not.
6. `originator_fp != cosigner_fp`? Drop if equal.
7. `Ed25519::verify(sig_originator, originator_pubkey, canonical)` succeeds? Drop if not.
8. `Ed25519::verify(sig_cosigner, cosigner_pubkey, canonical)` succeeds? Drop if not.
9. Both signers have `last_selftest_seen < 36h` ago? Drop if not.
10. Per-pubkey rate limit not exceeded? Drop if exceeded.
11. **Accept.** Store in audit log. Update state. Trigger UI and audio.

### 7.2 NFPA-72 state machine

The publicly visible state surface is the four-state NFPA enum, published to HA as `sensor.canary_<id>_beacon_state`:

```
NORMAL       — no active alarms, no trouble conditions, no supervisory
TROUBLE      — operationally degraded:
                 - time unsynced
                 - airtime governor sustained >80% of cap
                 - a paired beacon set member has missing selftest >36h
                 - own key self-test fails
                 - beacon_set empty (no co-signers paired)
ALARM        — at least one active, unsuppressed, dual-signature-verified Beacon
                 with urgency=Immediate or severity>=Severe
SUPERVISORY  — informational degradation:
                 - muted
                 - urgency filter raised
                 - pairing in progress
                 - last alarm cleared within last hour (post-alarm reflective)
```

Transitions are explicit:

```
NORMAL → ALARM        on accept of qualifying Beacon
ALARM → SUPERVISORY   on accept of CANCEL referencing the active alarm,
                      OR on alarm's `expires` reached
SUPERVISORY → NORMAL  after BEACON_POST_ALARM_REFLECT_MS (default 1h)
NORMAL → TROUBLE      on any trouble condition becoming true
TROUBLE → NORMAL      when all trouble conditions resolved
ALARM dominates TROUBLE in display (you should know there's a fire even if the radio is congested)
```

### 7.3 Audible response

`PATTERN_BEACON` is defined in `audible_chirp.h`. It is:

- Three ascending tones: 1200 Hz, 1700 Hz, 2200 Hz, each ~150 ms with ~50 ms gap.
- Total duration ~600 ms.
- **Distinct from `PATTERN_ALERT`** (the Chirp soft-alert pattern).
- **Not** the WEA two-tone (853 Hz + 960 Hz) — these frequencies are reserved.
- **Not** the EAS attention signal (853 Hz + 960 Hz, 8 s) — also reserved.

The lint script enforces non-WEA frequencies in any pattern named `*BEACON*` or `*ALERT*`.

### 7.4 Visual response

- `Alarm` displays with the template text, severity, and a single calming amber (#E67E22) accent color. **Never red.** Background remains the normal UI background; the alarm is presented as an attention-deserving block, not a screen takeover.
- `Cancel` for an active alarm transitions immediately to `Supervisory`. The cancellation is as prominent as the original alarm.
- `Trouble` displays a small yellow indicator with the specific cause ("neighbor Cairn missing selftest", "time unsynced", "airtime saturated").
- `Supervisory` displays a small grey indicator with the cause.

## 8. Rate limiting and abuse prevention

The two-pubkey origination requirement is the primary defense. Layered on top:

| Layer | Mechanism | Default value |
|---|---|---|
| Origination | Two distinct pubkeys must co-sign | Hardwired |
| Per-pubkey limit | Max Beacons originated by one pubkey per 24h | 5 |
| Per-pubkey-pair limit | Max Beacons co-signed by the same pair per 24h | 8 |
| Cosign window | Cosigner has N seconds to respond to a request | 60 s |
| Cosign freshness | Cosigner must have been seen in last N seconds | 600 s (10 min) |
| Bloom dedup | Last 5 minutes of nonces | 4 KB Bloom |
| Suppress vote | None for Beacon (high-trust origination, not voted) | n/a |
| Hop limit | Max relay hops | 3 |
| Relay rate limit | Max relays per minute | 5 |
| Self-test cadence | Daily | 24 h |
| Airtime class | Always urgent | `force_reserve_urgent` |

**Why no suppress voting on Beacon:** Beacon origination is already gated by cryptographic co-signing. Suppress voting on top would create a path where a small majority could silence a legitimate alarm, which is the opposite of smoke-detector reliability. False alarms are addressed by the originators sending a `CANCEL` (`BCN_CLR_FALSE_ALARM`) — the same way the fire service handles them.

## 9. State machine (firmware)

```
BEACON_DISABLED        — feature off
  ↓ enable()
BEACON_INITIALIZING    — load beacon set from NVS, generate session
  ↓ ready
BEACON_NORMAL          — operating, healthy
  ↕ trouble conditions
BEACON_TROUBLE         — operating, degraded
  ↓ accept qualifying alarm
BEACON_ALARM           — active alarm
  ↓ cancel / expires
BEACON_SUPERVISORY     — post-alarm reflective period
  ↓ reflect timeout
BEACON_NORMAL

BEACON_PAIR_INIT       — initiator
BEACON_PAIR_JOIN       — joiner
BEACON_PAIR_CONFIRM    — awaiting user code confirmation
* → BEACON_PAIRED      — pairing complete
* → BEACON_PAIR_FAILED → BEACON_NORMAL (on timeout or cancel)
```

## 10. REST API

All endpoints Bearer-token-gated identically to `/api/mesh/*` and `/api/bluetooth/*`. Hold-to-send actions also require the UI's hold timestamp.

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/beacon` | GET | Status (state, beacon_set_size, active_alarm, trouble_reasons) |
| `/api/beacon/set` | GET | List beacon set entries (no full pubkeys, only fingerprints + names) |
| `/api/beacon/pair/start` | POST | Enter pair-init mode |
| `/api/beacon/pair/join` | POST | Enter pair-join mode |
| `/api/beacon/pair/confirm` | POST | Confirm pair code |
| `/api/beacon/pair/cancel` | POST | Abort pairing |
| `/api/beacon/revoke` | POST | Set a beacon-set entry to `trust_level = 2` (revoked) |
| `/api/beacon/originate` | POST | Begin two-pubkey origination flow (template_id, urgency, severity) |
| `/api/beacon/cosign` | POST | Confirm a pending cosign request (originator_fp, decision) |
| `/api/beacon/cancel` | POST | Originate a `BEACON_MSG_CANCEL` for the current active alarm |
| `/api/beacon/active` | GET | Active alarms and active cosign requests |
| `/api/beacon/audit` | GET | Audit log (last 30 days, signed, exportable) |
| `/api/beacon/selftest` | POST | Force a `BEACON_MSG_SELFTEST_OK` emission (mostly for tests) |

## 11. Persistence

| Storage | Where | Encrypted? |
|---|---|---|
| Beacon set (pubkeys, names, last_seen) | NVS | Yes — requires flash encryption (same gate as Opera secret) |
| Audit log (received + originated) | Append-only flash file, chain-hashed like witness records | Yes (FE) |
| Per-pubkey rate-limit state | RAM only, rebuilt from audit log on boot | n/a |
| Active alarm state | RAM only | n/a |
| Last-selftest-seen map | RAM only | n/a |

Maximum audit log: 30 days, capped at ~64 KB. Older entries pruned by `prune_audit_log()` daily.

## 12. Configuration

Defaults baked into firmware; selected values surfaced through `/api/beacon` for the UI:

```c
namespace beacon_channel {
  static const uint8_t  PROTOCOL_VERSION              = 1;
  static const uint8_t  BEACON_MAGIC                  = 0xB1;
  static const uint8_t  MAX_BEACON_SET                = 32;
  static const uint8_t  MAX_HOP_COUNT                 = 3;
  static const uint32_t COSIGN_WINDOW_MS              = 60000;
  static const uint32_t COSIGN_FRESHNESS_MS           = 600000;
  static const uint32_t BEACON_FRESHNESS_S            = 300;
  static const uint32_t SELFTEST_INTERVAL_MS          = 86400000; // 24 h
  static const uint32_t SELFTEST_MISSING_MS           = 129600000; // 36 h
  static const uint8_t  MAX_ORIGINATIONS_PER_PUBKEY_24H = 5;
  static const uint8_t  MAX_ORIGINATIONS_PER_PAIR_24H  = 8;
  static const uint8_t  MAX_RELAYS_PER_MINUTE         = 5;
  static const uint32_t POST_ALARM_REFLECT_MS         = 3600000; // 1 h
  static const uint32_t MIN_UNIX_TIME                 = 1700000000;
}
```

## 13. Privacy guarantees

What Beacon shares on the wire:
- Originator and cosigner device fingerprints (16 bytes each).
- Template ID, urgency, severity, certainty, msg_type.
- Effective/expires timestamps.
- Random nonce.
- Detail slot (constrained enum).

What Beacon never shares:
- Full device pubkeys after pairing (only fingerprints on the wire).
- `opera_id` or any household identifier.
- Chirp `session_id` (privacy firewall preserved).
- Location, PII, person descriptions, MAC addresses, BSSID.
- Free text. Templates only.

## 14. Threats considered and not considered

### 14.1 Mitigated

| Threat | Mitigation |
|---|---|
| Single bad actor originates fake alarm | Two-pubkey co-signing |
| Compromised device floods alerts | Per-pubkey rate limit, suppress not needed because false alarms self-cancel and other devices in the set can revoke |
| Replay of past Beacons | Bloom dedup + `effective` freshness window |
| Beacon set membership leakage | Fingerprints, not pubkeys, on the wire; co-signer name only in local UI |
| Confusion with WEA / EAS / IPAWS | Distinct frequencies, distinct phrasing, distinct UI color, distinct CAP `scope=Private` |
| Hawaii-style operator error | Two-person rule, `msgType=Exercise` distinct from Alert, cancel as prominent as origination |
| Drive-by neighbor pairing | Visual confirmation code at pairing, 120 s timeout |
| Coerced origination | Cosigner must independently confirm; cosign requires explicit hold; refusal is unanimous-equivalent (one refusal prevents origination) |

### 14.2 Not mitigated

| Threat | Why not |
|---|---|
| Fully compromised host firmware | Beyond scope of any protocol — `THREAT_MODEL.md` explicitly out of scope |
| Coordinated compromise of two paired devices | Acknowledged — co-signing is a force multiplier, not a magic shield. Recovery is operational (revoke the compromised pubkeys). |
| Radio jamming | Physical-layer attack, outside protocol scope. Beacon falls into `Trouble` if airtime saturated. |
| Long-range RF triangulation of beacon-set members | Acknowledged. Fingerprints are persistent identifiers; this is the same trade-off the existing Opera mesh accepts. |
| Insider abuse of `BCN_CLR_FALSE_ALARM` to suppress real alarms | Insiders are by definition cosigners; revocation is the recourse. Audit log is signed and retains the history. |

## 15. Future work

- **Threshold signatures (FROST or similar)** to make co-signing scale beyond pairs without bloating the wire format.
- **Gateway operator implementation** per `spec/beacon_cap_gateway_v0.md`.
- **Audible self-test** following NFPA 72 §14 cadence (monthly chirp confirming the device is healthy).
- **Hardware tamper integration** — a tamper alert on a Beacon-paired device automatically downgrades it to `trust_level = 2` in the local beacon set.

## 16. Changelog

- v0.1 (2026-05-11): Initial draft.

---

*"A cairn is a deliberately-stacked pile of stones placed by passersby as a navigation aid in mountains. Anonymous, community-built, only useful, never abusable for individual targeting. Each one is small; together they form a trail."*
