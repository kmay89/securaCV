# Chirp Channel Protocol v0.2 (Community Witness Network)

Status: Draft v0.2
Intended Status: Normative
Last Updated: 2026-05-11

> **v0.2 hardening summary** (see `docs/audit/mesh_and_chirp_audit_v1.md` for the
> full findings list and `docs/research/harm_reduction_prior_art.md` for the
> design anchors):
>
> - Wire format now carries `session_pubkey` (32 B) on every witness frame.
>   Receivers verify Ed25519 signatures end-to-end and reject any frame whose
>   `session_id` does not derive from the carried pubkey (closes audit C1, C6).
> - `confirm_count` is no longer transmitted; receivers track confirmations
>   locally as a set of unique confirmer `session_pubkey`s (closes audit C2, C5).
> - Initial `confirm_count = 0`. EMERGENCY/WEATHER fast-path requires the one
>   confirmation to come from a `session_pubkey` distinct from the
>   originator's (closes audit C3).
> - Relayers re-sign frames under their own session key; the original signature
>   is preserved in a `signed_origin` envelope (closes audit C4).
> - Suppress voting is now wire-format-real (`CHIRP_MSG_SUPPRESS_VOTE`), signed,
>   and counts unique pubkey dismissals (closes audit C7).
> - `TPL_AUTH_FEDERAL_PRESENCE` is removed. Such alerts must originate through
>   the new higher-trust Beacon channel (`spec/beacon_channel_v0.md`) which
>   requires two-pubkey co-signing.
> - Timestamps anchored on wall clock (`time(nullptr)`); origination refused
>   when `time(nullptr) < 1700000000` (closes audit C10, C15).
> - Storage upgraded to a priority heap by urgency; nonce dedup upgraded to
>   Bloom filter (closes audit C8, C9).
> - Presence requirement now also gates ACK origination (closes audit C13).
> - Per-pubkey rate limit on incoming witnesses (closes audit C14).
> - `PROTOCOL_VERSION` bumped from 0 to 1.
> - Companion spec: `spec/beacon_channel_v0.md` introduces the harm-reduction
>   layer (life-safety templates, NFPA-72 supervised health, CAP-aligned wire
>   fields, two-pubkey cryptographic co-signing).

## 1. Purpose and Philosophy

The Chirp Channel is an **anonymous, opt-in community alert system** that extends beyond your trusted Opera (home network) to nearby devices. Think of it like a neighborhood smoke alarm network - when something concerning happens, nearby witnesses can share soft alerts without revealing their identity.

### 1.1 Core Philosophy

This is **NOT** a surveillance system like Ring doorbells. Key differences:

| Ring/Surveillance | Chirp Channel |
|-------------------|---------------|
| Persistent identity | Ephemeral identity per session |
| Video/audio sharing | Text-only soft alerts |
| Corporate servers | Local mesh only, no internet |
| Always watching | Human-triggered only |
| Fear-driven | Community trust-driven |
| Permanent records | No history retained |

**Chirp = "Safety in numbers, not surveillance"**

### 1.2 Design Goals

1. **Human-in-the-Loop**: No automated alerts. Every chirp requires explicit human confirmation
2. **Ephemeral Identity**: New anonymous identity every session - no tracking
3. **Soft Witness**: Calm notifications, not panic alarms
4. **Opt-In Only**: Devices explicitly subscribe to receive community alerts
5. **No History**: Alerts display in real-time, then fade away
6. **Rate Limited**: One chirp per device per 5-minute window (prevents spam/hysteria)
7. **Limited Range**: Max 3 hops - your building/block, not the whole city
8. **Deniable**: Cannot prove who sent a chirp (ephemeral keys)

### 1.3 Use Cases

- "Unusual activity on my street" - soft awareness, not accusation
- "Power outage in area" - community situational awareness
- "Someone checking car doors" - heads-up without police involvement
- "Fire/smoke in building" - faster than waiting for official alarm
- "Medical emergency, need help" - community response
- "All clear" - de-escalation after concern passes

### 1.4 Non-Goals

- Identifying individuals (no video, no audio, no photos)
- Creating a permanent record of events
- Enabling vigilante behavior
- Replacing emergency services
- Tracking device locations precisely

## 2. Network Architecture

### 2.1 Two-Tier Model

```
┌─────────────────────────────────────────────────────────────┐
│                    CHIRP CHANNEL (Outer Ring)               │
│        Anonymous • Ephemeral • Community • 3-hop range      │
│                                                             │
│   ┌───────────────────────────────────────────────────┐     │
│   │              OPERA (Inner Ring)                   │     │
│   │    Trusted • Persistent Identity • Home Network   │     │
│   │                                                   │     │
│   │   [Canary A] ←──────→ [Canary B]                 │     │
│   │       ↑                    ↑                      │     │
│   │       └────────────────────┘                      │     │
│   └───────────────────────────────────────────────────┘     │
│                           ↕                                 │
│   [Neighbor1]  [Neighbor2]  [Building3]  [Campus4]         │
│      (anon)       (anon)       (anon)       (anon)         │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 Transport

Chirp Channel uses the same ESP-NOW transport as Opera, but with different message handling:

- **Broadcast Only**: All chirps sent to broadcast address
- **No Pairing Required**: Devices discover each other via broadcast
- **Separate Channel**: Uses WiFi channel 6 (Opera uses channel 1)
- **Range**: ~250m per hop, max 750m with 3 hops

### 2.3 Identity Model

**Ephemeral Session Identity**:
```
session_seed = random_bytes(32)
session_keypair = Ed25519_Generate(session_seed)
session_id = SHA-256("securacv:chirp:session:v0" || session_pubkey)[0:8]
```

- New identity generated on each enable/reboot
- Identity not linked to device Ed25519 key (privacy firewall)
- Session ID is 8-byte pseudonym (displayed as emoji sequence for humans)
- Identity discarded when chirp channel disabled

**Emoji Display** (for session identification):
```
emoji_index = session_id[i] % 16
emoji_set = ["🐦", "🌳", "🏠", "🌙", "⭐", "🌸", "🍃", "💧",
             "🔔", "🎵", "🌈", "☀️", "🌻", "🐝", "🦋", "🍀"]
display = emoji_set[session_id[0]] + emoji_set[session_id[1]] + emoji_set[session_id[2]]
// Example: "🐦🌳⭐" - easy to remember, hard to track
```

## 2.5 Abuse Prevention Design

### 2.5.1 Threat Model

Assume attackers who want to:
- Flood with spam to make the system unusable
- Send offensive/disgusting content
- Create false panic ("gunman!" when there's none)
- Coordinate attacks with multiple devices
- Drive-by attack (show up, spam, leave)
- Desensitize users with false alarms (cry wolf)

### 2.5.2 Philosophy: Witness Authority, Not Neighbors

**Critical Design Principle**: This system exists to **witness power**, not to surveil people.

**What this is NOT**:
- Neighborhood watch (leads to racial profiling)
- "Suspicious person" reporting (weaponizes neighbors)
- Ring doorbell culture (surveillance capitalism)
- Policing each other

**What this IS**:
- Community smoke alarm for serious emergencies
- Collective witness when authority arrives unexpectedly
- Mutual aid coordination during crises
- "Hey, something big is happening, stay safe"

### 2.5.3 Defense: Structured Messages (Emergency-Focused)

**Critical Design Decision**: Messages use **predefined templates** focused on **emergencies and authority presence**, not individual behavior.

**Message Templates** (exhaustive list):

| Category | Template ID | Display Text |
|----------|-------------|--------------|
| **Authority Presence** | | |
| | `AUTH_POLICE_ACTIVITY` | "police activity in area" |
| | `AUTH_HEAVY_RESPONSE` | "heavy law enforcement response" |
| | `AUTH_ROAD_BLOCKED_LE` | "road blocked by law enforcement" |
| | `AUTH_HELICOPTER` | "helicopter circling area" |
| | ~~`AUTH_FEDERAL_PRESENCE`~~ | **Removed in v0.2** — life-safety advisories about federal/agency presence are weaponizable as hoaxes in a low-trust soft-alert channel. If a deployment genuinely needs this signal, it must originate through the higher-trust Beacon channel (`spec/beacon_channel_v0.md`) which requires two-pubkey cryptographic co-signing. |
| **Infrastructure** | | |
| | `INFRA_POWER_OUT` | "power outage" |
| | `INFRA_WATER_ISSUE` | "water service disruption" |
| | `INFRA_GAS_SMELL` | "gas smell - evacuate?" |
| | `INFRA_INTERNET_DOWN` | "internet outage in area" |
| | `INFRA_ROAD_CLOSED` | "road closed or blocked" |
| **Emergency** | | |
| | `EMERG_FIRE_VISIBLE` | "fire or smoke visible" |
| | `EMERG_MEDICAL_SCENE` | "medical emergency scene" |
| | `EMERG_MULTIPLE_AMBULANCE` | "multiple ambulances responding" |
| | `EMERG_EVACUATION` | "evacuation in progress" |
| | `EMERG_SHELTER_IN_PLACE` | "shelter in place advisory" |
| **Weather** | | |
| | `WX_SEVERE_WARNING` | "severe weather warning" |
| | `WX_TORNADO` | "tornado warning" |
| | `WX_FLOOD` | "flooding reported" |
| | `WX_LIGHTNING_CLOSE` | "dangerous lightning nearby" |
| **Mutual Aid** | | |
| | `AID_WELFARE_CHECK` | "neighbor may need help" |
| | `AID_SUPPLIES_NEEDED` | "supplies needed in area" |
| | `AID_OFFERING_HELP` | "offering assistance" |
| **All Clear** | | |
| | `CLR_RESOLVED` | "situation resolved" |
| | `CLR_SAFE` | "area appears safe now" |
| | `CLR_FALSE_ALARM` | "false alarm" |

**What's deliberately MISSING** (and why):

| Excluded Template | Why Excluded |
|-------------------|--------------|
| "Suspicious person" | Racial profiling vector |
| "Unfamiliar vehicle" | Leads to harassment |
| "Someone at door" | Normal activity |
| "Person taking photos" | Legal activity, often targeted at minorities |
| "Loitering" | Criminalization of existence |
| "Unknown person in area" | Everyone was unknown once |

**The Test**: Before adding any template, ask:
1. Could this be used to target someone based on race/appearance?
2. Is this about an individual's behavior or a community emergency?
3. Would sending this make the community safer or more paranoid?

If the answer to #1 is "yes" or #2 is "individual" or #3 is "paranoid" - **don't add it**.

**Detail Slots** (minimal, factual):
- `AUTH_*`: Optional "[many vehicles]" indicator
- `EMERG_*`: Optional "[ongoing]" or "[contained]" status
- NO descriptions of people. Ever.

### 2.5.3 Defense: Witness Requirement (Sybil Resistance) — v0.2

**Key Insight**: A single device should not be able to broadcast to the neighborhood alone.

**Rule**: Chirps only propagate beyond hop 0 after **2 independent confirmations from distinct `session_pubkey`s**.

```
Device A sends chirp (hop 0) ──broadcast──→
  Device B sees same thing, confirms ──→ (1 confirmation, pubkey_B added to confirmed_by)
  Device C sees same thing, confirms ──→ (2 confirmations, pubkey_C added to confirmed_by)
  NOW the chirp propagates to hop 1+ ──relay──→
```

**Confirmation Flow**:
1. Original chirp broadcasts locally (hop 0, ~250m range). Originator does NOT
   count itself; initial `confirm_count = 0`.
2. Nearby devices see it in their "pending" feed.
3. If a human on device B also witnesses the event, they tap "I see this too".
4. Device B emits a `CHIRP_MSG_ACK` carrying B's `session_pubkey` and an Ed25519
   signature over `(original_nonce, B.session_pubkey, ACK_CONFIRMED)`.
5. Every receiver verifies the ACK's signature, verifies `B.session_pubkey` is
   not the originator's pubkey, verifies `B.session_pubkey` is not already in
   the local `confirmed_by[]` set for this chirp, and only then increments.
6. After threshold confirmations (2 for most templates, 1 for safety templates
   from a pubkey ≠ originator), the chirp becomes "validated" and relays
   propagate it.
7. Unvalidated chirps expire after 5 minutes.

**Why This Works**:
- The `confirm_count` is no longer carried on the wire as a sender-controlled
  field. It is local state, derived only from cryptographically authenticated
  ACK messages with unique pubkeys.
- Single bad actor cannot inflate the count by claiming N witnesses — every
  claim requires a fresh signed ACK from a fresh pubkey.
- Coordinated attack needs 3+ devices in same area, each holding a distinct
  session_pubkey and each willing to sign an ACK. Cost is real.
- Real events naturally get confirmed by multiple witnesses.

**Exception**: Safety-class templates (EMERGENCY, WEATHER) require only 1
confirmation for faster propagation, but that one confirmation MUST come from
a `session_pubkey` distinct from the originator's, and the originator does not
count itself.

**Presence requirement also applies to ACK origination**: a device must have
been broadcasting presence beacons for `PRESENCE_REQUIRED_MS` (10 min) before
its ACKs are accepted as confirmations. Drive-by devices cannot immediately
push chirps over the validation threshold.

### 2.5.4 Defense: Escalating Cooldowns

Instead of fixed 5-minute cooldown:

```
Chirp 1: 5 minute cooldown
Chirp 2: 15 minute cooldown
Chirp 3: 1 hour cooldown
Chirp 4+: 4 hour cooldown

Reset: 24 hours of no chirps
```

**Why This Works**:
- First chirp is easy (legitimate use)
- Spammer hits diminishing returns fast
- Max 4-5 chirps per day per device
- Legitimate users rarely need >2 chirps/day

### 2.5.5 Defense: Presence Requirement (Anti-Drive-By)

Devices must broadcast presence for **10 minutes** before they can send chirps.

```
New device joins area:
  t=0: Start broadcasting presence
  t=0-10min: Can receive chirps, cannot send
  t=10min+: Can send chirps

Device leaves and returns:
  Must re-establish 10-minute presence
```

**Why This Works**:
- Drive-by spammer can't immediately attack
- Encourages devices to be stable community members
- Attacker would need to "camp" for 10 min before each attack
- Presence beacons are passive (don't require human)

### 2.5.6 Defense: Community Mute Propagation — v0.2

If multiple devices quickly dismiss a chirp, a signed "suppress vote" propagates.

**Wire format** (`CHIRP_MSG_SUPPRESS_VOTE`):

```
chirp_suppress_vote = {
  msg_type: 4,
  original_nonce: bstr .size 8,           ; nonce of chirp being suppressed
  voter_session_pubkey: bstr .size 32,
  signature: bstr .size 64,               ; Ed25519 over canonical
}
```

The canonical signed input is `"securacv:chirp:suppress:v0" || original_nonce
|| voter_session_pubkey`.

**Counting rule**: every receiver maintains a `suppressed_by[]` set per chirp,
keyed by `voter_session_pubkey`. Duplicate votes from the same pubkey count
once. The receiver tracks `nearby_count = |g_nearby_devices|`.

**Threshold**: a chirp becomes `suppressed = true` when
`|suppressed_by| / nearby_count > 50%` and `|suppressed_by| >= 3` (small
absolute floor to prevent suppression in very-small networks), within
`SUPPRESS_WINDOW_MS` (default 120 s) of the chirp's `received_ms`.

**Behavior when suppressed**:
- The chirp is hidden from the receiver's UI.
- The receiver no longer relays the chirp.
- The receiver does not echo further suppress votes (they're not needed).
- The suppression itself is logged to the health log but does not generate
  further wire traffic.

**Why This Works**:
- Community self-moderates without central authority.
- Bad content gets filtered organically.
- Privacy preserved (only `session_pubkey` in suppress vote, not a stable
  device identifier).
- Legitimate content won't get mass-dismissed (50% threshold + 3-vote floor).

### 2.5.7 Defense: Time-Based Restrictions

Different rules at different times:

| Time | Restrictions |
|------|--------------|
| 6am-10pm | Normal operation |
| 10pm-6am | Safety templates only, requires 2 confirmations |

**Why This Works**:
- Night-time attacks (when people are sleeping) are limited
- Legitimate late-night alerts (fire, medical) still work
- Reduces "prank" window

### 2.5.8 Summary: Abuse Prevention Stack

```
Layer 1: Structured templates (no offensive content possible)
Layer 2: Witness requirement (no solo broadcast)
Layer 3: Escalating cooldowns (diminishing spam returns)
Layer 4: Presence requirement (no drive-by)
Layer 5: Community mute (organic moderation)
Layer 6: Time restrictions (reduced night attacks)
```

**Threat Actor Analysis**:

| Attack | Blocked By |
|--------|------------|
| Spam flooding | Escalating cooldowns (max ~5/day) |
| Offensive text | Structured templates (impossible) |
| False "gunman" | No such template exists |
| Coordinated spam | Witness requirement (need 3+ liars) |
| Drive-by attack | 10-min presence requirement |
| Cry wolf | Community mute + escalating cooldowns |
| Night harassment | Time restrictions |

## 3. Message Types

### 3.1 Common Header — v0.2

```cddl
chirp_message = {
  magic: 0xC4,
  version: 1,                         ; v0.2 wire format
  msg_type: uint,
  session_id: bstr .size 8,           ; SHA-256("securacv:chirp:session:v0" || session_pubkey)[0:8]
  hop_count: uint,                    ; 0 = original, max 3
  timestamp: uint,                    ; UNIX wall-clock seconds (NOT millis()/1000); originators MUST refuse to send if time(nullptr) < 1700000000
  nonce: bstr .size 8                 ; Random nonce for dedup
}
```

The signature field is no longer carried on the header — each message type
carries its own signature, scoped to the message-type-specific signed input
(see §3.3). Receivers MUST verify every witness, ACK, suppress vote, and mute
broadcast cryptographically; unsigned or invalid-signature frames are dropped.

The originating `session_pubkey` (32 bytes) is carried in each signed message
payload. Receivers MUST recompute `session_id` from the carried `session_pubkey`
and drop the frame if it does not match.

### 3.2 CHIRP_PRESENCE (Discovery)

Periodic broadcast to discover nearby chirp-enabled devices:

```cddl
chirp_presence = {
  msg_type: 0,
  emoji: tstr .size (3..12),      ; Emoji display string
  listening: bool,                 ; Accepting community chirps
  last_chirp_age_min: uint / null ; Minutes since last chirp sent
}
```

- Sent every 60 seconds when chirp channel enabled
- No sensitive information shared
- Allows UI to show "X devices nearby"

### 3.3 CHIRP_WITNESS (Soft Alert) — v0.2

The core community alert message — **human-triggered only**, **template-only**,
**signed end-to-end**:

```cddl
chirp_witness = {
  msg_type: 1,
  template_id: uint,                  ; ChirpTemplate enum (no free text)
  detail_slot: uint,                  ; ChirpDetailSlot enum (constrained)
  urgency: "info" / "caution" / "urgent",
  ttl_minutes: uint,                  ; How long to display (5-60)
  session_pubkey: bstr .size 32,      ; Originator's session pubkey (32 B)
  signature: bstr .size 64,           ; Ed25519 over canonical
  ? signed_origin: signed_origin_envelope   ; Present only on relays (hop_count > 0)
}

signed_origin_envelope = {
  origin_pubkey: bstr .size 32,
  origin_signature: bstr .size 64,
}
```

Canonical signed input (originator):
```
canonical = "securacv:chirp:witness:v0"
         || nonce(8) || template_id(1) || detail_slot(1)
         || urgency(1) || ttl_minutes(1) || timestamp(4)
         || session_pubkey(32)
```

The `confirm_count` field from v0.1 is **removed from the wire**. Confirmation
state is local-only and derived from `CHIRP_MSG_ACK` messages with unique
`session_pubkey`s.

Category is no longer transmitted explicitly — it is derived from
`template_id >> 4` (high nibble of template ID indexes the category enum).

On relay (hop_count > 0):
- The relaying device sets `signed_origin = { origin_pubkey, origin_signature }`
  preserving the original signer's identity.
- The relaying device re-signs the canonical under its own session key, placing
  its `session_pubkey` and `signature` in the top-level fields.
- Receivers verify both signatures and that `origin_pubkey != session_pubkey`.

**Categories**:
- `activity`: Unusual activity observed (not accusation, just awareness)
- `utility`: Power outage, water issue, internet down
- `safety`: Fire, medical, urgent safety concern
- `community`: Community event, lost pet, general notice
- `all_clear`: Situation resolved, de-escalation

**Urgency Levels**:
- `info`: FYI, no action needed (blue indicator)
- `caution`: Heads up, be aware (yellow indicator)
- `urgent`: Important, pay attention (orange indicator - NOT red/panic)

### 3.4 CHIRP_ACKNOWLEDGE — v0.2

Acknowledgment that chirp was seen and (optionally) human-witnessed. Signed.

```cddl
chirp_ack = {
  msg_type: 2,
  original_nonce: bstr .size 8,         ; Nonce of chirp being ack'd
  ack_type: "seen" / "confirmed" / "resolved",
  confirmer_session_pubkey: bstr .size 32,
  signature: bstr .size 64,             ; Ed25519 over canonical
}
```

Canonical signed input:
```
canonical = "securacv:chirp:ack:v0"
         || original_nonce(8) || ack_type(1) || confirmer_session_pubkey(32)
```

Receivers MUST:
1. Verify the signature.
2. Reject if `confirmer_session_pubkey` equals the original chirp's
   `session_pubkey` (the originator cannot self-confirm).
3. Reject if `confirmer_session_pubkey` is already in the local `confirmed_by[]`
   set for this `original_nonce`.
4. Reject if the confirmer's pubkey has not been seen in `g_nearby_devices` for
   at least `PRESENCE_REQUIRED_MS` (drive-by ACKs do not count).
5. Otherwise, add the pubkey to `confirmed_by[]`. If the chirp's confirmation
   threshold is now met, mark `validated = true` and (if `relay_enabled`)
   relay the chirp.

- `seen`: device received the chirp; used for diagnostic counts only, does not
  count as a witness confirmation.
- `confirmed`: human confirmed they also witness this; counts as a witness
  confirmation if it passes all checks above.
- `resolved`: user signals the situation is resolved; treated as a local
  dismissal and may contribute to suppress voting.

### 3.5 CHIRP_MUTE (Leave Me Alone)

Temporary opt-out without disabling:

```cddl
chirp_mute = {
  msg_type: 3,
  duration_minutes: uint,         ; 15, 30, 60, or 120
  reason: "busy" / "sleeping" / "away" / null
}
```

## 4. Human-in-the-Loop Flow

### 4.1 Sending a Chirp

**Critical**: Chirps are NEVER sent automatically. The flow is:

1. **Device detects something** (motion, tamper, user observation)
2. **User is prompted**: "Share with nearby community?"
3. **User selects category and urgency** from simple picker
4. **User optionally adds brief message** (64 chars max)
5. **User confirms send** with deliberate action (hold button 2 sec)
6. **Chirp broadcasts** to nearby devices

```
┌─────────────────────────────────────────┐
│         SHARE WITH COMMUNITY?           │
│                                         │
│  🐦🌳⭐ (your session)                   │
│                                         │
│  ┌─────────────────────────────────┐   │
│  │ What's happening?               │   │
│  │ ○ Unusual activity              │   │
│  │ ○ Utility issue                 │   │
│  │ ○ Safety concern                │   │
│  │ ○ Community notice              │   │
│  └─────────────────────────────────┘   │
│                                         │
│  How urgent?                            │
│  [Info] [Caution] [Urgent]             │
│                                         │
│  Brief message (optional):              │
│  [_______________________________]      │
│                                         │
│  [ Cancel ]    [====Hold to Send====]  │
│                                         │
│  ⚠️ This will notify ~12 nearby devices │
└─────────────────────────────────────────┘
```

### 4.2 Receiving a Chirp

When chirp received, display is **calm and informative**, not alarming:

```
┌─────────────────────────────────────────┐
│  🐝🌸💧 shared nearby:                   │
│                                         │
│  📍 Activity • Caution                  │
│  "someone checking car doors"           │
│                                         │
│  ⏱️ 2 min ago • 🔄 3 relays              │
│                                         │
│  [ Dismiss ]  [ I see it too ]          │
└─────────────────────────────────────────┘
```

### 4.3 Rate Limiting

To prevent spam and hysteria:

- **Per-device**: Max 1 chirp per 5 minutes
- **Per-category**: Max 1 chirp per category per 15 minutes
- **Relay limit**: Max 2 relays per received chirp
- **Cooldown display**: "You can chirp again in 3:24"

## 5. Relay and Propagation

### 5.1 Hop Behavior

```
Original device (hop 0) ──broadcast──→
  Nearby devices (receive, hop 1) ──relay──→
    Further devices (receive, hop 2) ──relay──→
      Edge devices (receive, hop 3) ──STOP──
```

- **Hop 0**: Original chirp, signed by session key
- **Hop 1-3**: Relayed chirp, original signature preserved
- **Max 3 hops**: Prevents city-wide propagation

### 5.2 Relay Decision

Devices relay chirps if:
1. Hop count < 3
2. Haven't relayed this nonce before
3. User has "relay enabled" setting (default: on)
4. Message is < 5 minutes old

Devices do NOT relay if:
1. Hop count >= 3
2. Already relayed this nonce
3. Rate limited (max 10 relays per minute)
4. User has muted chirps

### 5.3 Deduplication

```
recent_nonces = sliding_window(last_100_nonces)
if (chirp.nonce in recent_nonces) {
  drop_duplicate()
} else {
  recent_nonces.add(chirp.nonce)
  process_chirp()
}
```

## 6. Privacy Guarantees

### 6.1 What IS Shared

- Ephemeral session ID (unlinkable to device)
- Category and urgency level
- Optional 64-char message
- Hop count and timestamp

### 6.2 What is NOT Shared

- Device Ed25519 key (identity firewall)
- Opera membership (home network info)
- Precise location (only "nearby" via hop count)
- Device name or any persistent identifier
- Historical chirp patterns (no storage)
- Audio, video, or images (text only)

### 6.3 Identity Firewall

The Chirp Channel uses a **completely separate identity** from Opera:

```
Opera Identity (persistent):
  device_key = Ed25519 device key
  opera_id = SHA-256(opera_secret)

Chirp Identity (ephemeral):
  session_seed = random_bytes(32)  // NOT derived from device_key
  session_key = Ed25519_Generate(session_seed)
  session_id = SHA-256(session_key)

// No cryptographic link between identities
```

### 6.4 Plausible Deniability

Because session keys are ephemeral and unlinked:
- Cannot prove a specific device sent a chirp
- Cannot track chirp history across sessions
- Cannot correlate chirps to home network membership

## 7. State Machine

### 7.1 Chirp Channel States

```
CHIRP_DISABLED       → Feature disabled (default)
CHIRP_INITIALIZING   → Generating session identity
CHIRP_LISTENING      → Receiving chirps, not sending presence
CHIRP_ACTIVE         → Full participation (presence + listening)
CHIRP_MUTED          → Temporarily ignoring chirps
CHIRP_COOLDOWN       → Rate limited, cannot send new chirp
```

### 7.2 Transitions

```
DISABLED ──enable()──→ INITIALIZING
INITIALIZING ──ready()──→ LISTENING
LISTENING ──join_active()──→ ACTIVE
ACTIVE ──mute(duration)──→ MUTED
MUTED ──unmute/timeout──→ ACTIVE
ACTIVE ──send_chirp()──→ COOLDOWN
COOLDOWN ──timeout(5min)──→ ACTIVE
* ──disable()──→ DISABLED
```

## 8. API Endpoints

### 8.1 REST API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/chirp` | GET | Chirp channel status |
| `/api/chirp/enable` | POST | Enable chirp channel |
| `/api/chirp/disable` | POST | Disable chirp channel |
| `/api/chirp/nearby` | GET | Count of nearby chirp devices |
| `/api/chirp/recent` | GET | Recent chirps (last 30 min) |
| `/api/chirp/send` | POST | Send new chirp (human confirmed) |
| `/api/chirp/ack` | POST | Acknowledge a chirp |
| `/api/chirp/mute` | POST | Mute for duration |

### 8.2 Response Formats

```json
// GET /api/chirp
{
  "state": "ACTIVE",
  "session_emoji": "🐦🌳⭐",
  "nearby_count": 12,
  "recent_chirps": 2,
  "last_chirp_sent": null,
  "cooldown_remaining_sec": 0,
  "relay_enabled": true
}

// GET /api/chirp/recent
{
  "chirps": [
    {
      "emoji": "🐝🌸💧",
      "category": "activity",
      "urgency": "caution",
      "message": "someone checking car doors",
      "age_sec": 120,
      "hop_count": 1,
      "acks": 3
    }
  ]
}

// POST /api/chirp/send
{
  "category": "activity",
  "urgency": "caution",
  "message": "someone checking car doors",
  "ttl_minutes": 15
}
```

## 9. UI Requirements

### 9.1 Chirp Panel

The web UI MUST include a "Community" panel showing:

1. **Chirp Status**: Enabled/Disabled toggle with session emoji
2. **Nearby Count**: "12 devices nearby" (anonymous count only)
3. **Recent Feed**: Live feed of recent chirps (last 30 min)
4. **Send Chirp**: Category/urgency picker with hold-to-send
5. **Mute Controls**: Quick mute buttons (15m, 30m, 1h, 2h)

### 9.2 Design Language

**Colors** (calming, not alarming):
- Info: Soft blue (#5B9BD5)
- Caution: Warm yellow (#F4B942)
- Urgent: Soft orange (#E67E22) - NOT red

**Animations**:
- New chirp: Gentle fade-in, not sudden pop
- Acknowledge: Subtle pulse
- Muted: Grayed out with timer

### 9.3 Notification Style

Chirp notifications should be **informative, not alarming**:

✅ Good: "🐝🌸💧 shared: activity nearby"
❌ Bad: "⚠️ ALERT: SUSPICIOUS ACTIVITY DETECTED"

✅ Good: "heads up from your area"
❌ Bad: "SECURITY THREAT IN YOUR NEIGHBORHOOD"

## 10. Security Considerations

### 10.1 Threats Mitigated

1. **Spam/Abuse**: Rate limiting (1 per 5 min)
2. **Tracking**: Ephemeral session identity
3. **Hysteria**: Human confirmation required, calm UI
4. **Replay**: Nonce deduplication, 5-minute TTL
5. **Impersonation**: Ed25519 session signatures
6. **Range abuse**: 3-hop limit

### 10.2 Threats Not Mitigated

1. **False Reports**: Human judgment required (community self-policing)
2. **Radio Jamming**: Physical layer attack
3. **Social Engineering**: Malicious but believable chirps
4. **Collusion**: Multiple devices coordinating false reports

### 10.3 Trust Model

The Chirp Channel operates on **community trust**:
- Anyone can participate (no permission needed)
- Reputation is not tracked (ephemeral identity)
- False reports are self-limiting (rate limits)
- Community interprets context (soft alerts, not accusations)

## 11. Configuration

### 11.1 Default Settings

```json
{
  "chirp_enabled": false,          // Off by default
  "relay_enabled": true,           // Relay others' chirps
  "presence_broadcast": true,      // Send presence beacons
  "notification_sound": true,      // Audio for incoming chirps
  "urgency_filter": "info",        // Show all urgency levels
  "max_display_minutes": 30,       // Keep chirps visible
  "auto_dismiss_info": true        // Auto-dismiss info after 5 min
}
```

### 11.2 NVS Storage

```
NVS Key              | Size    | Description
---------------------|---------|---------------------------
chirp_enabled        | 1 byte  | Feature flag
chirp_relay          | 1 byte  | Relay enabled
chirp_notify_sound   | 1 byte  | Sound enabled
chirp_urgency_filter | 1 byte  | Min urgency to show
// Session identity NOT stored (regenerated each boot)
```

## 12. Implementation Notes

### 12.1 ESP-NOW Configuration for Chirp

```c
// Chirp uses different channel from Opera
#define CHIRP_CHANNEL 6

// Chirp always broadcasts (no peer list)
uint8_t CHIRP_BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Message prefix to identify chirp vs opera
#define CHIRP_MAGIC 0xC4  // 'Ch' for Chirp
#define OPERA_MAGIC 0x0A  // 'Op' for Opera
```

### 12.2 Session Identity Generation

```c
void generate_chirp_session() {
  // Generate fresh random seed (NOT from device key)
  esp_fill_random(session_seed, 32);

  // Derive session keypair
  Ed25519::generateKeyPair(session_pubkey, session_privkey, session_seed);

  // Compute session ID
  SHA256 hash;
  hash.update("securacv:chirp:session:v0", 26);
  hash.update(session_pubkey, 32);
  hash.finalize(session_id);  // Use first 8 bytes

  // Generate emoji display
  generate_emoji_string(session_id, emoji_display);
}
```

### 12.3 Memory Budget

| Component | Max Size |
|-----------|----------|
| Session identity | 128 bytes |
| Recent chirps | 16 * 128 = 2048 bytes |
| Nonce cache | 100 * 8 = 800 bytes |
| Presence cache | 32 * 16 = 512 bytes |
| **Total** | ~3.5 KB |

## 13. Relationship to Opera

### 13.1 Independence

Chirp Channel operates **independently** from Opera:
- Different identity system (ephemeral vs persistent)
- Different channel (6 vs 1)
- Different trust model (community vs trusted)
- Can use Chirp without Opera, and vice versa

### 13.2 Optional Integration

When both enabled, Opera can optionally:
- Forward chirps to Opera members (with consent)
- Display chirps on all home devices
- Aggregate nearby counts across Opera

### 13.3 Privacy Boundary

Opera devices MUST NOT:
- Correlate chirp session with Opera identity
- Share Opera membership info via Chirp
- Log chirp activity linked to device identity

## 14. Glossary

- **Chirp**: A community alert message sent via the Chirp Channel
- **Session**: A temporary identity that lasts until disable/reboot
- **Emoji ID**: Human-readable 3-emoji representation of session
- **Hop**: One relay transmission (max 3)
- **Presence**: Periodic beacon showing device is chirp-enabled
- **Relay**: Forwarding someone else's chirp to extend range
- **Mute**: Temporary opt-out of receiving chirps

## 15. Changelog

- v0.2 (2026-05-11): Hardening per `docs/audit/mesh_and_chirp_audit_v1.md` —
  end-to-end signature verification (C1, C4, C6); Sybil-resistant confirmation
  via unique-pubkey set tracking and wire-format removal of `confirm_count`
  (C2, C3, C5); signed suppress voting wired (C7); priority storage by
  urgency (C8); Bloom-filter dedup (C9); wall-clock-anchored timestamps with
  conservative behavior when unsynced (C10, C15); 5-emoji session display
  (C11); REST API Bearer-gated (C12); presence requirement on ACK origination
  (C13); per-pubkey rate limit on incoming witnesses (C14); `TPL_AUTH_FEDERAL_PRESENCE`
  removed (C17); host tests added (C16). `PROTOCOL_VERSION` bumped from 0 to 1.
- v0.1 (2026-02-02): Initial draft

---

*"Like smoke alarms for your community - not surveillance, but safety in numbers."*
