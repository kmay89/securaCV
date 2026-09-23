# Canary Mesh Network Protocol v0.3 (Opera Protocol)

Status: Draft v0.3
Intended Status: Normative
Last Updated: 2026-09-23

> **v0.3 summary** (PlatformIO tree, `firmware/canary/lib/securacv_mesh`):
> `LEAVE_OPERA` (§4.2), a compact template-only `TAMPER_ALERT` payload
> (§4.3), the REST surface the web UIs already call — leave, name,
> enable, alerts GET/DELETE — reconciled in §8.1/§8.3, and `remove` with an
> ephemeral-X25519 `opera_secret` rotation (§4.2 `REKEY_*`, §5.6 PIO;
> **crypto review and bench pass pending**). See §14.

> **v0.2 hardening summary** (see `docs/audit/mesh_and_chirp_audit_v1.md`
> findings O1–O3):
>
> - Message freshness is anchored on the **per-peer monotonic counter**, not
>   on `millis()/1000` uptime seconds (closes audit O1). The timestamp field
>   in the header is retained for diagnostic purposes only and is no longer
>   security-bearing. The counter is monotonic per peer and persisted across
>   reboots (LRU-evicted but never decreased).
> - `opera_secret` provisioning and NVS storage now require **flash encryption
>   to be enabled** (`esp_efuse_read_field_bit(ESP_EFUSE_FLASH_CRYPT_CNT)`).
>   Devices without flash encryption refuse to provision an Opera, refuse to
>   load any previously-stored opera_secret, and log loudly to the health
>   log at `LOG_LEVEL_ALERT, LOG_CAT_SECURITY` (closes audit O2).
> - `remove_peer()` now **automatically rotates `opera_secret`** and re-pushes
>   the new secret to the remaining members under their existing session
>   keys (closes audit O3). A removed device no longer retains cryptographic
>   capability to rejoin without explicit re-pairing.

## 1. Purpose and Scope

This specification defines a secure mesh network protocol for SecuraCV Canary devices, enabling an "opera" of canaries to communicate and protect each other. When one canary is tampered with or loses power, it broadcasts alerts to its opera members, providing redundant evidence of security events.

### 1.1 Design Goals

1. **Mutual Protection**: Canaries alert each other of tamper/power events before going offline
2. **Network Isolation**: Separate operas for different owners (your devices vs neighbor's devices)
3. **Anti-Spoofing**: Cryptographic authentication prevents unauthorized devices from joining
4. **Privacy Preservation**: Minimal metadata exposure, no raw media transmission
5. **Resilience**: Works with WiFi, ESP-NOW, or BLE; survives partial network failures
   *(design goal — see the implementation-status note under §2.2: v0.2 ships ESP-NOW only)*

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

> **Implementation status (v0.2): only ESP-NOW (1) is implemented.** The WiFi-AP
> bridge (2) and BLE fallback (3) are specified here but **not yet built** — they are
> tracked as roadmap items G3/G4 in
> [`docs/mesh_esp_now_evaluation.md`](../docs/mesh_esp_now_evaluation.md). Until they
> ship, mesh resilience relies on ESP-NOW alone.

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

### 3.3 Replay Prevention — v0.2

- **Message Counter** (authoritative): Monotonic 64-bit counter per peer.
  Receivers reject any message with `counter <= last_seen_counter_for_peer`.
  Counter state is persisted to NVS so reboots do not reset the receiver's
  expectations.
- **Nonce Tracking**: Last 64 nonces cached to detect concurrent duplicates.
- **Timestamp field**: Retained in the wire format for diagnostic and
  debugging purposes. **Not security-bearing in v0.2.** Earlier revisions
  used `millis()/1000` here, which gave uptime seconds rather than wall-clock
  time, causing receivers to reject legitimate messages from peers with
  different uptimes (audit O1). Receivers MAY log unusual timestamp gaps but
  MUST NOT reject solely on timestamp.

Future revisions MAY add wall-clock-anchored freshness if a shared mesh
epoch is derived at pairing; v0.2 does not.

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

#### LEAVE_OPERA — v0.3
Sent once by a member that is leaving (`POST /api/mesh/leave`), signed under
the opera it is leaving, with no payload:
```cddl
leave_opera_payload = nil        ; PlatformIO: zero-length payload, msg_type 25
```
A receiver that verifies it — signature against the sender's pinned pubkey,
`opera_id`, per-peer counter — removes the **signer's own** trust entry and
nothing else, so a peer can only ever remove itself. It needs no rekey: the
leaver discards its own `opera_secret`, and the survivors' opera is unchanged.
(Removing *another* device is §5.6 and does rotate the secret.) Like every
opera frame it is replay-protected by the counter; the one residual case is a
receiver that has since re-registered the same device into the same,
un-rotated opera (registration restarts that peer's counter at 0), where a
recorded LEAVE can drop the device's entry again — never add one.

#### REKEY_OFFER / REKEY_ACCEPT / REKEY_SECRET / REKEY_ACK — v0.3 (PlatformIO)
The `opera_secret` rotation that `remove` runs (§5.6, PlatformIO subsection).
All four ride opera-authenticated envelopes under the **current** `opera_id`
(msg_type 26–29); multi-byte integers are little-endian:
```cddl
rekey_offer  = [ rekey_id: u32, eph_pub: bstr .size 32, removed_fp: bstr .size 8 ]  ; 44 B, broadcast
rekey_accept = [ rekey_id: u32, eph_pub: bstr .size 32 ]                             ; 36 B, to the initiator
rekey_secret = [ rekey_id: u32, nonce: bstr .size 12,
                 ciphertext: bstr .size 32, tag: bstr .size 16 ]                    ; 64 B, to one survivor
rekey_ack    = [ rekey_id: u32 ]                                                     ;  4 B, to the initiator
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

**PlatformIO tree (v0.3) — compact, template-only payload.** The PIO sender
(`mesh_session::send_tamper_alert`, envelope msg_type 18) carries a fixed
6-byte payload (`mesh_alert.h`): `kind` u8 (0 `enclosure_tamper`, 1
`temp_drift`, 2 `camera_tamper` — the dictionary's firmware tamper `kind`
vocabulary), `severity` u8 (0–7, LogLevel), `witness_seq` u32 LE (0 = none).
There is **no free-text `detail` on the wire**: a receiver renders the kind's
template name and never shows sender-authored text. Any other length, or a
severity above 7, is dropped. canary-wap sends its own struct (with a 64-byte
`detail`) under its own outer type (`MSG_TAMPER_ALERT = 4`), so the two trees
do not exchange alerts — see §8.3.

#### POWER_ALERT
Broadcast on power loss detection:
```cddl
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

### 5.5 Flash Encryption Requirement — v0.2

Provisioning a new Opera, joining an existing Opera, and loading a stored
`opera_secret` from NVS at boot all require **flash encryption to be
enabled** on the device. The firmware checks
`esp_efuse_read_field_bit(ESP_EFUSE_FLASH_CRYPT_CNT)` (or equivalent SDK
helper); if not set, the relevant code paths:

1. Refuse to create an opera (return `MESH_ERROR_NO_FLASH_ENCRYPTION`).
2. Refuse to consume a `PAIR_COMPLETE` payload.
3. Refuse to load a previously-stored `opera_secret` from NVS — instead,
   wipe the entry, mark the device as `MESH_NO_OPERA`, and log loudly:
   `health_log(LOG_LEVEL_ALERT, LOG_CAT_SECURITY, "opera: refused to load — flash encryption disabled")`.

Rationale: `opera_secret` is the symmetric secret that gates pairing and
opera_id derivation. On an unencrypted flash, a physical-access attacker
can extract the secret with `esptool.py read_flash` and become a permanent
opera member. The FE gate eliminates this attack at the cost of refusing to
work on dev boards without FE — which is the correct trade-off for a
production safety system.

### 5.6 Peer Removal and Re-keying — v0.2

`remove_peer(fingerprint)` MUST automatically rotate `opera_secret` and
re-distribute the new secret to remaining members:

1. Generate fresh `new_opera_secret = random_bytes(32)`.
2. Derive `new_opera_id = SHA-256("securacv:opera:id:v0" || new_opera_secret)[0:16]`.
3. Remove the targeted peer from the local member list.
4. For each remaining member, send a `MSG_OPERA_REKEY` containing
   `new_opera_secret` encrypted under that member's existing session key.
5. Wait up to 60 s for each member to ACK with `MSG_OPERA_REKEY_ACK`.
6. Once all surviving members have ACKed, commit `new_opera_secret` and
   `new_opera_id` to NVS. Increment the on-disk rotation counter.
7. Any member that fails to ACK is marked `PEER_STALE` and rejoined via
   normal pairing.

The removed device's pubkey is recorded in a local revocation list and
refused acceptance into future pairing flows for `REVOCATION_GRACE_MS`
(default 7 days), even by a freshly-rotated opera.

Caveat: the removed device, while it still has the *old* `opera_secret`,
cannot impersonate a current member because the surviving members no longer
accept frames carrying the old `opera_id` after rotation. The old
`opera_secret` is forensically useful (for log decryption) but operationally
inert.

#### PlatformIO tree — ephemeral rotation (v0.3, F10-rekey option B)

> **Status: implemented and host-tested; maintainer crypto review and the
> U1 Track C3 bench pass are both still required before this counts as
> shipped.**

The PlatformIO mesh keeps **no per-peer session keys** (its frames are
Ed25519-signed only, and the pairing X25519 key is wiped at `PAIRED`), so
step 4 above cannot encrypt "under that member's existing session key". It
runs an ephemeral exchange per rotation instead
(`firmware/canary/lib/securacv_mesh/src/mesh_rekey.{h,cpp}`), every message
inside a signed opera envelope so each ephemeral public key is
authenticated by the sender's long-term Ed25519 key:

1. The initiator generates `new_opera_secret = random_bytes(32)` and an
   ephemeral X25519 keypair, drops the removed peer (trust entry and radio
   address), and broadcasts `REKEY_OFFER {rekey_id, eph_pub_i, removed_fp}`.
   The removed device ignores an OFFER that names it.
2. Each survivor answers `REKEY_ACCEPT {rekey_id, eph_pub_s}` with its own
   ephemeral keypair.
3. For each ACCEPT the initiator derives
   `k = SHA-256("securacv:opera:rekey:key:v0" || X25519(eph_i, eph_pub_s) ||
   rekey_id || eph_pub_i || eph_pub_s)` and sends that survivor
   `REKEY_SECRET` = ChaCha20-Poly1305 under `k`, random 96-bit nonce,
   AAD `rekey_id || initiator_fp || survivor_fp`.
4. The survivor decrypts, sends `REKEY_ACK {rekey_id}` **under the old
   `opera_id`, before switching** (an ACK under the new id would fail the
   initiator's `opera_id` check — the ordering canary-wap learned), then
   installs the new secret, drops `removed_fp` and re-persists.
5. The initiator commits when every survivor has ACKed, or at 60 s; a
   survivor that did not ACK is dropped and must re-pair (canary-wap's
   accepted trade-off). A survivor that never gets its SECRET aborts at its
   own 60 s mark and keeps the old secret.

On a switch the outbound counter is **kept**: receivers track the per-peer
counter by fingerprint, not by `opera_id`, so resetting it would get the
next frames dropped as replays. The new secret is re-persisted through the
flash-encryption gate (§5.5); if that save is refused, the old secret is
cleared rather than left for the next boot. The ephemeral keys come from a
dedicated X25519 generator (`mesh_crypto::x25519_generate_keypair`, RFC 7748
clamping) — not the Ed25519 generator pairing uses.

Deliberate limits: one rotation at a time per device (a second `remove` is
refused, a survivor ignores a second OFFER); two users removing peers from
two devices inside the same 60 s window can split the household between two
new secrets, and the losing side re-pairs. **The `REVOCATION_GRACE_MS`
deny-list above is not implemented in either tree** — a removed device can
be re-paired by a user who walks it through pairing again.

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
MESH_DISABLED        → Feature disabled
MESH_INITIALIZING    → Loading opera config, starting transports
MESH_NO_OPERA        → No opera configured, awaiting pairing
MESH_CONNECTING      → Attempting to reach opera members
MESH_ACTIVE          → Connected to one or more peers
MESH_PAIRING_INIT    → In pairing mode as initiator (existing member)
MESH_PAIRING_JOIN    → In pairing mode as joiner (new device)
MESH_PAIRING_CONFIRM → Awaiting user confirmation of pairing code
MESH_ERROR           → Fatal error, requires restart
```

### 7.2 Peer States

```
PEER_UNKNOWN        → Never contacted
PEER_DISCOVERED     → Found via broadcast, not yet authenticated
PEER_AUTHENTICATING → Auth handshake in progress
PEER_CONNECTED      → Authenticated and actively communicating
PEER_STALE          → No heartbeat for 90 seconds
PEER_OFFLINE        → No heartbeat for 5 minutes
PEER_ALERT          → Received alert from this peer
PEER_REMOVED        → Removed from opera (pending deletion)
```

## 8. API Endpoints

### 8.1 REST API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/mesh` | GET | Mesh status and peer list |
| `/api/mesh/peers` | GET | Detailed peer information |
| `/api/mesh/alerts` | GET | Recent mesh alerts |
| `/api/mesh/alerts` | DELETE | Clear the alert history |
| `/api/mesh/pair/start` | POST | Enter pairing mode (initiator) |
| `/api/mesh/pair/join` | POST | Enter join mode (joiner) |
| `/api/mesh/pair/confirm` | POST | Confirm pairing code |
| `/api/mesh/pair/cancel` | POST | Cancel pairing |
| `/api/mesh/leave` | POST | Leave current opera |
| `/api/mesh/name` | POST | Rename the opera, body `{"name": "..."}` |
| `/api/mesh/enable` | POST | Mesh on/off, body `{"enabled": true\|false}` |
| `/api/mesh/remove` | POST | Remove a peer from the opera, body `{"fingerprint": "<16 hex>"}` |

v0.3: this table now lists the routes both web UIs call and the canary-wap
server registers (`securacv_webui.cpp` and `web_ui.h` send `remove` with a
`{fingerprint}` body, not the `/remove/:fp` path v0.2 listed; `enable`,
`name` and the alerts `DELETE` were missing).

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

### 8.3 v0 Implementation Notes (PR-8, F10)

The REST implementation in the PlatformIO tree (`firmware/canary`, gated on
`FEATURE_MESH_NETWORK`, compiled in CI by `[env:full]`) diverges from
§8.1/§8.2 as described here; the normative tables describe the target shape.

**Endpoints implemented (12 registrations):** `GET /api/mesh`,
`GET /api/mesh/peers`, `POST /api/mesh/pair/{start,join,confirm,cancel}`
(PR-8), `POST /api/mesh/leave`, `POST /api/mesh/name`,
`POST /api/mesh/enable`, `GET /api/mesh/alerts`, `DELETE /api/mesh/alerts`
(F10), and `POST /api/mesh/remove` (F10-rekey, below). All require a valid
`Authorization: Bearer` token
and pass through the same rate limiter as the rest of the REST API. Like the
PR-8 pairing handlers, they call into `mesh_session` from the HTTP server's
task rather than marshaling onto the main loop the module's send contract
names — a known posture, not a new one.

**`remove` (F10-rekey — crypto review and bench pending):** body
`{"fingerprint": "<16 hex>"}`, the string `GET /api/mesh/peers` emits. It
never drops a peer without rotating: §5.6 requires that removing a peer
rotate `opera_secret` and hand the new one to the survivors, and a `remove`
that only edited the local table would leave the removed device a working
secret — so the PIO route starts the §5.6 PlatformIO rotation first and
forgets the peer only if the rotation started. Responses: `{ok, rekey:
"started"}` (survivors are being re-keyed; the commit lands within 60 s),
`{ok, rekey: "committed"}` (nobody left to tell — rotated locally at once),
`persisted` for the removed peer's NVS entry; errors `unknown_peer` (404),
`rekey_in_flight` (409 — one rotation at a time; `enable {false}` is refused
with the same code while one runs), `no_opera`, `mesh_disabled`,
`invalid_fingerprint`. canary-wap's rotation is its own (per-peer session
keys, `MSG_OPERA_REKEY`); the two trees do not rotate each other.

**`leave`:** the device signs a `LEAVE_OPERA` (§4.2) under the opera it is
leaving and broadcasts it (best effort — the response carries
`notified: true|false`), then forgets everything opera-scoped: the secret,
the trusted peers, the replay counters, the elected hub and the name, in RAM
and in NVS (`persisted: false` if an NVS clear failed). Survivors that verify
the frame drop the leaver's trust entry, in RAM and in NVS. No rekey (§4.2).

**`name`:** 1–32 printable-ASCII bytes, refused with `no_opera` when the
device holds no opera. **Local only** — it renames this device's label for
the opera and is not propagated to peers. Persisted to NVS `opera_name`
through the flash-encryption gate (§12.3); on an FE-off board the rename
holds until reboot and the response says `persisted: false`.

**`enable`:** body `{"enabled": bool}`. Disabling stops the session — no
send, no receive dispatch, a pairing in flight is canceled — without
leaving the opera; `GET /api/mesh` then reports `state: "DISABLED"`, and
`pair/start` / `pair/join` answer `mesh_disabled`. The choice is persisted
to NVS `mesh_enabled` (a preference, not flash-encryption gated) and applied
at boot.

**Alerts:** the PIO tree now **sends** `TAMPER_ALERT` (§4.3) — a device's
own enclosure, temperature-drift or camera tamper witness record queues one
to the opera, drained on the main loop — and **receives** it: a frame that
passes signature, `opera_id` and replay checks and decodes is counted
against the sender (`alerts_received` per peer and opera-wide), kept in a
16-entry RAM history, and written to the health log at `LOG_LEVEL_ALERT`,
`LOG_CAT_NETWORK` with the sender's fingerprint (§6.3). `GET /api/mesh/alerts`
returns `{ok, count, alerts:[{timestamp_ms, type, severity, sender_fp,
sender_name, detail, witness_seq}]}` newest first: `type` is `"TAMPER"`,
`detail` is the kind's template name, `timestamp_ms` is the receiver's uptime
at receipt (the canary-wap basis too), and `sender_name` is `""` until a
peer-metadata store exists. `DELETE` clears the history; the counters keep
counting. Counters and history are **per boot** — not persisted. Relay
(§6.1 step 3), `POWER_ALERT` and `OFFLINE_IMMINENT` are not implemented.
**Not wire-interoperable with canary-wap:** the two trees number the outer
frame type differently (PIO 18 vs WAP `MSG_TAMPER_ALERT = 4`) and carry
different payloads; nothing here delivers alerts across trees (the WAP's own
`broadcast_tamper_alert` has no caller, so it sends none either).

**`GET /api/mesh` field set:** the implementation emits the exact fields the
canary web UI consumes — `ok, state, opera_id, opera_name, has_opera,
enabled, peers_total, peers_online, alerts_received, pairing_code` — rather
than the illustrative shape in §8.2. `pairing_code` is included **only** when
`state == "PAIRING_CONFIRM"`; it is never present in any other state so the
6-digit confirmation value (§5.3) is not exposed before the out-of-band
visual-match step.

**Peer fields:** `fingerprint` is derived from the persisted trusted-peer
public keys (`mesh_crypto::fingerprint`). `name` is best-effort and may be
empty (the UI falls back to "Unknown Device") — a placeholder pending a
peer-metadata store. `alerts_received` is the §8.2 per-peer count of
verified `TAMPER_ALERT` frames this boot. `state`/`last_seen_sec`/`rssi`
are real joins against the ESP-NOW transport peer table:
`mesh_session` records the source MAC of every **fully verified**
opera-authenticated frame against the sender's fingerprint (signature,
opera_id and replay checks all passed, so the MAC provably spoke for the
fingerprint at that instant), and the handler joins that MAC into the
transport table's liveness. A trusted peer that has not sent a verified
frame this boot — or whose MAC has aged out of the transport table —
reports the OFFLINE/never defaults; the binding refreshes on the peer's
next verified frame, so an address change heals itself.

**Add-on → device bridge:** the Home Assistant "Add another Canary" wizard
(`privacy_witness_kernel/serve_wizard.py` + `wizard/index.html`) forwards
pairing calls to each Canary's REST API. The device address and bearer token
are entered transiently in the wizard form and passed per-request; they are
**not** persisted to add-on config. The bearer token is transmitted over
plaintext HTTP on the assumed-trusted LAN and is never logged.

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

The PlatformIO tree (`mesh_state.cpp`, NVS namespace `securacv`) stores:
`opera_secret` (32 B), `trusted_peers` (up to 8 × 32 B pubkeys),
`replay_ctrs` (per-peer counters), `elected_hub` (8 B) and — v0.3 —
`opera_name` (up to 32 B), all behind the flash-encryption gate (§5.5), plus
`mesh_enabled` (1 B), which is **not** gated: it is a preference, and gating
it would make "off" silently revert to "on" at every reboot of an FE-off
board. The `opera_id` is not stored; it is derived from the secret at boot.

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
- v0.2 (2026-05-11): audit O1–O3 hardening (counter-anchored freshness,
  flash-encryption gate, rekey on removal) — summary at the top.
- v0.3 (2026-09-23): `LEAVE_OPERA` (§4.2); the PlatformIO compact
  `TAMPER_ALERT` payload, sent and received (§4.3, §8.3); §8.1 reconciled
  with the routes the web UIs call (`remove` takes a `{fingerprint}` body;
  `enable`, `name`, alerts `DELETE` added); PIO REST leave/name/enable/alerts
  (§8.3); `opera_name` and `mesh_enabled` NVS keys (§12.3); PIO
  `POST /api/mesh/remove` with an ephemeral-X25519 `opera_secret` rotation,
  `REKEY_OFFER/ACCEPT/SECRET/ACK` (§4.2, §5.6 PlatformIO subsection) — crypto
  review and bench pass pending; the §5.6 revocation deny-list is stated as
  not implemented.
