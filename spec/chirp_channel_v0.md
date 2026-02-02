# Chirp Channel Protocol v0 (Community Witness Network)

Status: Draft v0.1
Intended Status: Normative
Last Updated: 2026-02-02

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

## 3. Message Types

### 3.1 Common Header

```cddl
chirp_message = {
  version: 0,
  msg_type: uint,
  session_id: bstr .size 8,       ; Ephemeral session ID
  hop_count: uint,                 ; 0 = original, max 3
  timestamp: uint,                 ; Unix timestamp (seconds)
  nonce: bstr .size 8,            ; Random nonce for dedup
  ? signature: bstr .size 64      ; Ed25519 sig (session key)
}
```

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

### 3.3 CHIRP_WITNESS (Soft Alert)

The core community alert message - **human-triggered only**:

```cddl
chirp_witness = {
  msg_type: 1,
  category: chirp_category,
  urgency: "info" / "caution" / "urgent",
  message: tstr .size (0..64),    ; Optional brief text
  confirm_count: uint,            ; How many humans confirmed (for relays)
  ttl_minutes: uint               ; How long to display (5-60)
}

chirp_category = "activity" / "utility" / "safety" / "community" / "all_clear"
```

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

### 3.4 CHIRP_ACKNOWLEDGE

Optional acknowledgment that chirp was seen (not required):

```cddl
chirp_ack = {
  msg_type: 2,
  original_nonce: bstr .size 8,   ; Nonce of chirp being ack'd
  ack_type: "seen" / "confirmed" / "resolved"
}
```

- `seen`: Device received the chirp
- `confirmed`: Human confirmed they also witness this
- `resolved`: Situation is resolved (prompts ALL_CLEAR consideration)

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

- v0.1 (2026-02-02): Initial draft

---

*"Like smoke alarms for your community - not surveillance, but safety in numbers."*
