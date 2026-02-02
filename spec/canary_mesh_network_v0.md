# Canary Mesh Network Protocol v0 (Opera Protocol)

Status: Draft v0.1
Intended Status: Normative
Last Updated: 2026-02-02

## 1. Purpose and Scope

This specification defines a secure mesh network protocol for SecuraCV Canary devices, enabling an "opera" of canaries to communicate and protect each other. When one canary is tampered with or loses power, it broadcasts alerts to its opera members, providing redundant evidence of security events.

### 1.1 Design Goals

1. **Mutual Protection**: Canaries alert each other of tamper/power events before going offline
2. **Network Isolation**: Separate operas for different owners (your devices vs neighbor's devices)
3. **Anti-Spoofing**: Cryptographic authentication prevents unauthorized devices from joining
4. **Privacy Preservation**: Minimal metadata exposure, no raw media transmission
5. **Resilience**: Works with WiFi, ESP-NOW, or BLE; survives partial network failures

### 1.2 Non-Goals

- Raw media transmission (violates PWK invariants)
- Centralized coordination (single point of failure)
- Internet connectivity requirement (local mesh only)
- Real-time streaming between devices

## 2. Network Architecture

### 2.1 Opera Model

A **opera** is a group of canaries that trust each other and share alerts. Each opera has:

- **Opera ID**: 16-byte cryptographic identifier derived from the opera secret
- **Opera Secret**: 32-byte shared secret established during pairing
- **Members**: List of authenticated device public keys

```
OperaID = SHA-256("securacv:opera:id:v0" || opera_secret)[0:16]
```

### 2.2 Transport Layers

The protocol supports multiple transports, used in priority order:

1. **ESP-NOW** (Primary): Direct peer-to-peer at 250m range, 1Mbps, encrypted
2. **WiFi AP Bridge**: Devices on same home network can relay messages
3. **BLE Beacon** (Fallback): Short-range emergency broadcasts

Each transport provides the same logical message interface.

### 2.3 Network Topology

- **Fully Connected Mesh**: Every device maintains connections to all opera members
- **Hop Limit**: Maximum 3 hops for relayed messages (prevents amplification)
- **Heartbeat**: Devices ping every 30 seconds to maintain presence
- **Max Opera Size**: 16 devices (prevents resource exhaustion)

## 3. Security Model

### 3.1 Device Authentication

Devices authenticate using their Ed25519 device keys (same keys used for witness records):

```
challenge = random_bytes(32)
response = Ed25519_Sign(device_privkey, "securacv:mesh:auth:v0" || challenge || opera_id)
```

Authentication flow:
1. Initiator sends `AUTH_CHALLENGE` with random 32-byte nonce
2. Responder signs nonce with device key, returns `AUTH_RESPONSE`
3. Initiator verifies signature against known opera member public key
4. Both parties derive session key using X25519 ECDH

### 3.2 Session Encryption

After authentication, all messages are encrypted:

```
session_key = HKDF-SHA256(
  "securacv:mesh:session:v0",
  X25519(local_privkey, peer_pubkey),
  32
)
message_key = HKDF-SHA256(session_key, message_counter, 32)
ciphertext = ChaCha20-Poly1305(message_key, nonce, plaintext)
```

### 3.3 Replay Prevention

- **Message Counter**: Monotonic 64-bit counter per peer session
- **Timestamp Validation**: Messages rejected if >5 minutes old
- **Nonce Tracking**: Last 64 nonces cached to detect replays

### 3.4 Opera Isolation

Devices MUST verify opera membership before accepting messages:

1. Verify sender's public key is in local opera member list
2. Verify opera ID matches local opera
3. Reject messages from unknown devices (prevents neighbor interference)

## 4. Message Types

### 4.1 Wire Format

All messages use deterministic CBOR encoding:

```cddl
mesh_message = {
  version: 0,
  opera_id: bstr .size 16,
  sender_fp: bstr .size 8,      ; Sender's pubkey fingerprint
  msg_type: tstr,
  counter: uint,
  timestamp: uint,               ; Unix timestamp (seconds)
  payload: any,
  signature: bstr .size 64
}
```

### 4.2 Control Messages

#### HEARTBEAT
Periodic presence announcement (every 30 seconds):
```cddl
heartbeat_payload = {
  status: "online" / "low_battery" / "warning",
  uptime_sec: uint,
  peer_count: uint,
  battery_pct: uint / null
}
```

#### AUTH_CHALLENGE
Initiate authentication:
```cddl
auth_challenge_payload = {
  nonce: bstr .size 32,
  pubkey: bstr .size 32
}
```

#### AUTH_RESPONSE
Complete authentication:
```cddl
auth_response_payload = {
  challenge_sig: bstr .size 64,
  pubkey: bstr .size 32,
  opera_proof: bstr .size 64     ; Signs opera_id with device key
}
```

### 4.3 Alert Messages

#### TAMPER_ALERT
Broadcast when tamper is detected:
```cddl
tamper_alert_payload = {
  alert_type: "tamper" / "motion" / "breach",
  severity: uint,                ; 0-7 matching LogLevel
  witness_seq: uint / null,      ; Sequence of related witness record
  detail: tstr .size (0..64)
}
```

#### POWER_ALERT
Broadcast on power loss detection:
```cldl
power_alert_payload = {
  alert_type: "power_loss" / "low_voltage" / "battery_critical",
  voltage_mv: uint / null,
  estimated_runtime_sec: uint / null
}
```

#### OFFLINE_IMMINENT
Final broadcast before expected shutdown:
```cddl
offline_imminent_payload = {
  reason: "power_loss" / "tamper" / "reboot" / "shutdown",
  final_seq: uint,              ; Last witness sequence number
  final_chain_hash: bstr .size 8  ; First 8 bytes of chain head
}
```

### 4.4 Sync Messages

#### PEER_LIST
Share known opera members:
```cddl
peer_list_payload = {
  peers: [* peer_entry]
}
peer_entry = {
  pubkey_fp: bstr .size 8,
  last_seen: uint,
  status: tstr
}
```

## 5. Pairing Protocol

### 5.1 Overview

Pairing adds a new device to an existing opera (or creates a new opera). The process requires physical proximity and user confirmation to prevent unauthorized joins.

### 5.2 Pairing Flow

1. **Initiator** (existing opera member) enters "pairing mode" via UI
2. **Joiner** (new device) enters "join opera" mode via UI
3. Devices discover each other via ESP-NOW broadcast or WiFi scan
4. **Visual Verification**: Both devices display 6-digit code derived from session
5. User confirms codes match on both devices
6. Opera secret is securely transferred to joiner
7. Joiner's public key is added to all opera members

### 5.3 Pairing Security

```
pairing_session_key = X25519(initiator_ephemeral, joiner_ephemeral)
confirmation_code = SHA-256("securacv:pair:confirm:v0" || pairing_session_key)[0:3]
display_code = decimal(confirmation_code) % 1000000  ; 6 digits
```

After confirmation:
```
encrypted_opera_secret = ChaCha20-Poly1305(
  pairing_session_key,
  nonce,
  opera_secret || opera_member_list
)
```

### 5.4 Creating a New Opera

If no opera exists, the first device generates:
```
opera_secret = random_bytes(32)
opera_id = SHA-256("securacv:opera:id:v0" || opera_secret)[0:16]
```

## 6. Alert Propagation

### 6.1 Broadcast Behavior

When a canary detects a critical event:

1. Immediately broadcast `TAMPER_ALERT` or `POWER_ALERT` to all peers
2. If power is failing, broadcast `OFFLINE_IMMINENT` as final message
3. Messages are relayed by other opera members (max 3 hops)
4. Receiving devices store alert in local log with sender attribution

### 6.2 Alert Priority

| Alert Type | Priority | Relay Immediately |
|------------|----------|-------------------|
| OFFLINE_IMMINENT | 0 (highest) | Yes |
| POWER_ALERT | 1 | Yes |
| TAMPER_ALERT | 2 | Yes |
| HEARTBEAT | 3 | No |

### 6.3 Persistence

Received alerts are stored in the health log with:
- `LOG_LEVEL_ALERT` or `LOG_LEVEL_TAMPER`
- `LOG_CAT_NETWORK`
- Source device fingerprint in detail field

## 7. State Machine

### 7.1 Mesh States

```
MESH_DISABLED      → Feature disabled
MESH_INITIALIZING  → Loading opera config, starting transports
MESH_NO_FLOCK      → No opera configured, awaiting pairing
MESH_CONNECTING    → Attempting to reach opera members
MESH_ACTIVE        → Connected to one or more peers
MESH_PAIRING       → In pairing mode (initiator or joiner)
MESH_ERROR         → Fatal error, requires restart
```

### 7.2 Peer States

```
PEER_UNKNOWN       → Never contacted
PEER_AUTHENTICATING → Auth in progress
PEER_CONNECTED     → Authenticated and communicating
PEER_STALE         → No heartbeat for 90 seconds
PEER_OFFLINE       → No heartbeat for 5 minutes
PEER_ALERT         → Received alert from this peer
```

## 8. API Endpoints

### 8.1 REST API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/mesh` | GET | Mesh status and peer list |
| `/api/mesh/peers` | GET | Detailed peer information |
| `/api/mesh/alerts` | GET | Recent mesh alerts |
| `/api/mesh/pair/start` | POST | Enter pairing mode (initiator) |
| `/api/mesh/pair/join` | POST | Enter join mode (joiner) |
| `/api/mesh/pair/confirm` | POST | Confirm pairing code |
| `/api/mesh/pair/cancel` | POST | Cancel pairing |
| `/api/mesh/leave` | POST | Leave current opera |
| `/api/mesh/remove/:fp` | POST | Remove peer from opera |

### 8.2 Response Formats

```json
// GET /api/mesh
{
  "state": "ACTIVE",
  "opera_id": "a1b2c3d4e5f6...",
  "opera_name": "Home Canaries",
  "peer_count": 3,
  "peers_online": 2,
  "peers_offline": 1,
  "last_alert": null,
  "uptime_sec": 3600
}

// GET /api/mesh/peers
{
  "peers": [
    {
      "fingerprint": "AB12CD34",
      "name": "Front Door",
      "state": "CONNECTED",
      "last_seen_sec": 15,
      "rssi": -45,
      "alerts_received": 0
    }
  ]
}
```

## 9. UI Requirements

### 9.1 Mesh Panel

The web UI MUST include a "Opera" panel showing:

1. **Opera Status**: Active/Inactive, opera name, member count
2. **Peer Grid**: Visual representation of each peer with status indicator
3. **Alert Feed**: Recent alerts from opera members
4. **Pairing Controls**: Buttons to start/join pairing, confirmation dialog

### 9.2 Status Indicators

| Peer State | Color | Animation |
|------------|-------|-----------|
| Connected | Green | Steady |
| Stale | Yellow | Slow pulse |
| Offline | Red | None |
| Alert | Red | Fast pulse |
| Authenticating | Blue | Spinner |

## 10. Resource Constraints

### 10.1 Memory Budget

| Component | Max Size |
|-----------|----------|
| Peer list | 16 * 64 = 1024 bytes |
| Session keys | 16 * 64 = 1024 bytes |
| Message buffer | 2048 bytes |
| Alert history | 32 * 128 = 4096 bytes |
| **Total** | ~8 KB |

### 10.2 Network Budget

| Message | Frequency | Size |
|---------|-----------|------|
| Heartbeat | 30 sec | ~64 bytes |
| Alert | On event | ~128 bytes |
| Auth | On connect | ~256 bytes |

## 11. Security Considerations

### 11.1 Threats Mitigated

1. **Neighbor Interference**: Opera ID isolation prevents cross-talk
2. **Replay Attacks**: Message counters and timestamp validation
3. **Spoofing**: Ed25519 signatures on all messages
4. **Eavesdropping**: ChaCha20-Poly1305 encryption
5. **Man-in-the-Middle**: Visual confirmation codes during pairing
6. **Resource Exhaustion**: Max opera size, rate limiting

### 11.2 Threats Not Mitigated

1. **Physical Compromise**: Attacker with device access can extract opera secret
2. **Targeted Jamming**: Radio-level attacks can block communication
3. **Social Engineering**: User may accept malicious device into opera

### 11.3 Failure Modes

1. **Single Device Offline**: Other devices continue normally
2. **All Peers Offline**: Device operates independently, alerts queued
3. **Opera Secret Compromise**: Must create new opera, re-pair all devices

## 12. Implementation Notes

### 12.1 ESP-NOW Configuration

```c
// Broadcast address for discovery
uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ESP-NOW channel (matches WiFi AP channel)
#define ESPNOW_CHANNEL 1

// Encryption uses ESP-NOW PMK + LMK derived from opera secret
// PMK (Primary Master Key): opera_secret[0:16]
// LMK (Local Master Key): device-specific, derived per peer
```

### 12.2 Power Loss Detection

Canaries MUST detect imminent power loss and broadcast alerts:

1. Monitor Vbus/battery voltage via ADC
2. Trigger alert when voltage drops below threshold
3. Use supercapacitor/battery to send final messages
4. Target: 500ms to broadcast after power cut detected

### 12.3 NVS Storage

```
NVS Key          | Size    | Description
-----------------|---------|---------------------------
mesh_enabled     | 1 byte  | Feature flag
mesh_opera_id    | 16 bytes| Current opera ID
mesh_opera_secret| 32 bytes| Opera encryption secret
mesh_opera_name  | 32 bytes| User-friendly opera name
mesh_peers       | 1024 B  | Serialized peer list
mesh_peer_count  | 1 byte  | Number of peers
```

## 13. Conformance

An implementation conforms to this specification if it:

1. Implements all REQUIRED message types (HEARTBEAT, AUTH_*, TAMPER_ALERT, POWER_ALERT, OFFLINE_IMMINENT)
2. Validates all signatures before accepting messages
3. Enforces opera isolation (rejects messages from non-members)
4. Implements visual confirmation codes for pairing
5. Stores received alerts in the health log
6. Respects resource constraints (max opera size, message limits)

## 14. Changelog

- v0.1 (2026-02-02): Initial draft
