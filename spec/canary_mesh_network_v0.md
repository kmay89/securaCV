# Canary Mesh Network Protocol v0.4 (Opera Protocol)

Status: Draft v0.4
Intended Status: Normative
Last Updated: 2026-10-01

> **v0.4 summary — one outer frame for both trees (§4.5).** The PlatformIO
> mesh and canary-wap numbered the type byte of their outer frame
> independently (`TAMPER_ALERT` 18 vs `MSG_TAMPER_ALERT = 4`; canary-wap's
> `CHANNEL_LOCK` 20 and `HUB_ELECTION` 21 sat on the PIO values of
> `OFFLINE_IMMINENT` and `WITNESS_RECORD`), used different version bytes (1
> vs 0), and the PIO tree put an unsigned copy of the type ahead of its
> envelope. §4.5 now states **one registry** (`mesh_wire.h`, canonical in
> the PIO library and staged byte-identical into the canary-wap sketch) and
> **one outer frame** — the signed envelope, version byte first — and both
> trees' enums take their values from it. Nothing below the envelope
> changed: several payloads and the pairing exchange still differ (§4.5
> table), so the trees still cannot pair with, and therefore cannot verify
> a frame from, each other. **Implemented and host-tested; awaiting
> maintainer crypto review; not bench-verified** (nothing of it has crossed
> a radio — U1 Track C2). See §14.

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

> **Implementation status (v0.2): relay is not built.** `mesh_network.cpp`
> carries no relay path and no hop count; a frame reaches exactly the peers
> in ESP-NOW range of its sender, one radio hop. "Fully connected" therefore
> means "every peer the radio can hear", and the hop limit above bounds a
> mechanism that does not yet exist. See `docs/FLEET_SEMANTICS.md` §6.
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

> **Implementation status, canary-wap (sweep F95, open; host-probed, not
> bench-verified).** Nothing sends `AUTH_CHALLENGE`, so no member ever holds
> a session, and the exchange cannot complete as it stands if one is sent.
> Its `AUTH_RESPONSE` payload is 160 B (64 + 32 + 64), so the signed frame
> is 262 B (38 B header + payload + 64 B signature) against the 250 B an
> ESP-NOW frame carries, and `send_to_peer` refuses it: the responder marks
> a session the challenger never gets. And step 4 runs X25519 over the
> long-term **Ed25519** keys (§5.3's bug class), so the two sides would
> derive different keys even if the response went out. Fixing either is a
> wire or derivation change (F48). The §5.6 rotation encrypts under these
> session keys, so on canary-wap it reaches no member (§5.6). The
> PlatformIO tree has no AUTH exchange: its frames are signed only.

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
  expectations. The **sender's** counter must survive a reboot too, or the
  receivers drop its frames as replays until it climbs back past what they
  remember (and a counter must never be signed twice). v0.3 (F33, PlatformIO
  tree): the sender reserves ahead — before it uses the first counter above
  its persisted high-water mark it persists a new mark 1024 counters ahead
  (NVS `mesh_out_ctr`, §12.3), and at boot it resumes above the persisted
  mark. NVS is written once per 1024 frames; a crash anywhere, including
  between a reservation and its first frame, costs an unused gap (receivers
  need only "higher"), never a reuse; a reservation that cannot be persisted
  refuses the frame. canary-wap keeps one `msg_counter_tx` per member (its
  counters are per destination) and, since F71 (host-tested, not
  bench-verified), reserves each the same way: before it uses the first
  counter above a member's stored reservation it stores one 1024 ahead (NVS
  `tx_ctrs`, one record of 8 B fingerprint + u64 per member, §12.3; one
  write covers every member past its reservation), and a boot resumes
  every member one past the highest reservation stored for any of them.
  So a boot also brings the members' counters level; the gap between them
  below (open) then grows only with the traffic one member gets and
  another does not, until the next boot. A reservation NVS refuses refuses
  the frame and is logged, at most once per 5 minutes while the refusals
  last. `send_to_peer` spends no counter while the storm gate holds, so
  even a flood costs about one write per 5 minutes (at most 101 counters
  per 31 s). Until F71 the counters restarted at 1 at every boot, so a
  rebooted canary-wap's frames dropped as replays at every member that had
  heard it, until its counter for that member climbed back. **The first
  boot after the update** finds members stored and no record (the record
  is written with the member list since F71, so that means NVS from an
  older firmware) and resumes every member above 2^40, which no boot of the
  older firmware reached, so that boot is heard at once too. A record that
  is there but unreadable is logged and every member resumes above 2^48,
  above anything signed since the update; a device that has already
  resumed from that floor once resumes below its own history at a second
  unreadable record, and its members drop its frames until each counter
  climbs back past the one they last heard. Since F95's counter fix
  (host-tested) its rotation (§5.6) resets no counter, as the PlatformIO
  tree's rotation keeps its own: it used to set a member's counters back
  (send 1, last-seen 0) for the new session, which bought nothing (frames
  are signed with the long-term key, and the `opera_id` in the signed bytes
  already kills a frame from before the rotation). It cost a survivor the
  rotation did not reach (every one, today) the remover's frames after the
  re-pair that rejoined it, until they climbed back; and since the
  last-seen reset was in RAM only, a member that rebooted before its
  5-minute `replay_ctrs` save restored the old value with the same effect.
  **A new member** (F99, host-tested) starts one past the highest
  counter the device can have signed to anyone, a removed member's
  included: every counter spent since the boot, above the highest
  reservation the boot read back (1 on a device that has signed none). A
  device this one removed or left keeps its last-seen counter for it, and
  a re-pair re-binds this one there with its counters, so a counter
  restarted at 1 dropped there until it climbed back. The new member is
  recorded as covered up to that counter, so a reboot before its first
  frame resumes above it too. A removal holds every survivor's reservation
  to it before the record is rewritten, the record is not rewritten when
  no member is left, and a boot reads it with no member loaded too (an
  opera emptied, or not loaded because flash encryption is off). Past what
  was signed, not past what was reserved: a reservation runs up to a block
  ahead, and F99 first started a new member there, a block ahead of every
  other member's counter until the next boot, which widened the gap below.
  A new member now starts level with the busiest member; after a busy
  member is removed it starts as far ahead of the others as that member's
  traffic was, until the next boot levels them. **A re-added member's
  last-seen** (F116, host-tested) starts where this device left it: a
  member dropped by a removal or a leave leaves its last-seen counter as a
  tombstone (§4.2, §12.3), and `add_peer` (initiator and joiner side) and a
  boot start the re-added member there. It used to start at 0, so in an
  opera whose id had not changed (removing the last member rotates
  nothing; a re-pair into the same opera after a leave) every frame the
  member had signed before was fresh once more, and one replayed at the
  re-pair counted as the joiner heard and ended the COMPLETE resend (§5.2).
  The envelope names no
  destination, so a receiver judges a frame its sender addressed to
  another member by its own last-seen counter for that sender (open).
  **Counter convention, both trees (v0.4 follow-up):** the first counter a
  sender signs is **1** (the PIO tree hands out `s_outbound_counter + 1`
  from 0; canary-wap's `add_peer` starts `msg_counter_tx` at 1 on a device
  that has signed no counter, and one past the highest it can have signed
  otherwise, F99), the receiver's last-seen starts at 0, and the
  gate is strict — `counter <= last_seen` is a replay, whatever `last_seen`
  is. canary-wap's gate used to carry an exemption (`&& last_seen > 0`) so
  its old counter-0 first frame could pass, and that exemption let a
  counter-0 frame pass again on every replay for as long as the last-seen
  stayed 0; it is gone, and no counter-0 frame is ever fresh (§4.5, the
  table below the registry).
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

> **Implementation status (v0.4): neither tree encodes CBOR.** Both send a
> fixed binary header in this field order — version (1 B), type (1 B),
> opera_id (16 B), sender fingerprint (8 B), counter (u64 LE), timestamp
> (u32 LE) — then the payload and a 64-byte Ed25519 signature over
> everything before it. The version byte is 1 and the type byte is a
> number from the §4.5 registry (not a text string). §4.5 is the normative
> statement of the frame as built; the CDDL above describes the intended
> self-describing form and is retained as such.

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
leave_opera_payload = nil        ; zero-length payload, type byte 25 in both trees (§4.5)
```
A receiver that verifies it — signature against the sender's pinned pubkey,
`opera_id`, per-peer counter — removes the **signer's own** trust entry and
nothing else, so a peer can only ever remove itself. It needs no rekey: the
leaver discards its own `opera_secret`, and the survivors' opera is unchanged.
(Removing *another* device is §5.6 and does rotate the secret.) Like every
opera frame it is replay-protected by the counter, and that protection
outlives the trust entry: in the PlatformIO tree a receiver that drops a
peer — on its LEAVE, a removal or a rotation — keeps the peer's last
counter as a **tombstone** (persisted in `replay_ctrs` with the live
counters, §12.3), and re-registering the same device re-applies it. So a
re-pair into the same, un-rotated opera does not re-open the window:
everything the device signed before it left — the LEAVE, its alerts,
beacon events, a `REKEY_OFFER` — stays a replay. For the same reason the
leaver keeps its own outbound counter across the leave, so its frames after
a re-pair continue above the tombstone its peers hold. (Before the fix,
registration restarted the counter at 0 and each of those recorded frames
verified once more — review finding, fw-mesh.)
canary-wap acts on no `LEAVE_OPERA` it receives (the member stays until the
owner removes it), but since F116 (host-tested) it keeps the same kind of
tombstone for every member it drops itself, at a removal (§5.6) or at its
own leave: at most eight, the oldest dropped first, under NVS `rx_tombs`
(§12.3), applied by `add_peer` on either side of the re-pair and at boot.
A device whose own counters went back while it kept its key (its
`tx_ctrs` record lost, §3.3) would drop at its tombstone for good, so a
removal of a member not heard above the tombstone its re-add restored
releases it, and the next re-pair starts it at 0, as every re-add did
before F116: the owner's way out, at the cost of reopening the window for
that member. A device whose NVS was erased has a new key, so no tombstone
applies; one that kept its NVS resumes its counters above everything it
signed (§3.3).

#### REKEY_OFFER / REKEY_ACCEPT / REKEY_SECRET / REKEY_ACK — v0.3 (PlatformIO)
The `opera_secret` rotation that `remove` runs (§5.6, PlatformIO subsection).
All four ride opera-authenticated envelopes under the **current** `opera_id`
(type bytes 26–29, §4.5); multi-byte integers are little-endian:
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
(`mesh_session::send_tamper_alert`, type byte 18 — since v0.4 the same byte
canary-wap's `MSG_TAMPER_ALERT` carries, §4.5) carries a fixed
6-byte payload (`mesh_alert.h`): `kind` u8 (0 `enclosure_tamper`, 1
`temp_drift`, 2 `camera_tamper` — the dictionary's firmware tamper `kind`
vocabulary), `severity` u8 (0–7, LogLevel), `witness_seq` u32 LE (0 = none).
There is **no free-text `detail` on the wire**: a receiver renders the kind's
template name and never shows sender-authored text. Any other length, or a
severity above 7, is dropped. canary-wap sends its own 54-byte struct (with a
48-byte `detail`) under the same type byte, so the two trees still do not
exchange alerts — a PIO receiver drops the canary-wap payload on length, and
a canary-wap receiver would read the 6-byte one as a truncated struct. The
payload, not the type byte, is what remains to reconcile (§4.5 table, §8.3).

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

### 4.5 Wire Type Registry and Outer Frame — v0.4

> **Status: implemented in both trees and host-tested; awaiting maintainer
> crypto review; not bench-verified** (no frame of either tree has crossed a
> radio, U1 Track C2). The registry is
> `firmware/canary/lib/securacv_mesh/src/mesh_wire.h`, canonical, staged
> byte-identical into `firmware/projects/canary-wap/arduino/canary_wap/`
> (`firmware/scripts/check_mesh_sync.sh`). Both trees' enums
> (`mesh_envelope::MsgType`, `mesh_session::MsgType`, `mesh_pairing::MsgType`;
> `mesh_network::MessageType`, `mesh_pair_frame::TYPE_*`) take their values
> from it, and the header's own `static_assert`s hold the invariants below
> wherever it is compiled. Host tests: `test_mesh_wire.cpp` (the numbers,
> run in both trees), `test_mesh_envelope.cpp`, `test_mesh_session.cpp`
> (`test_outer_frame_is_the_registry_frame`), canary-wap's
> `test_mesh_wire_wap.cpp` and `test_mesh_pair_frame.cpp`; the receive
> gates below the frame (the counter convention, the MAC binding, the
> struct-payload lengths — the v0.4 review's three pre-existing canary-wap
> findings, closed after it): `test_mesh_rx_gates_wap.cpp`, which pins the
> same lines in both trees. Since 2026-10-01 the MAC binding's row is "no
> frame binds an address" (§8.3), and canary-wap's `test_mesh_address_wap`
> runs it against the real `mesh_network.cpp`.

**The outer frame.** One byte, one meaning, both trees. A receiver
classifies a frame by its **first byte**:

| First byte | Frame | Layout |
|---|---|---|
| `1` (`mesh_wire::OPERA_VERSION`; `mesh_envelope::OPERA_VERSION` and canary-wap's `mesh_network::PROTOCOL_VERSION` take it) | opera-authenticated | `[version 1][type 16..255][opera_id 16][sender_fp 8][counter u64 LE][timestamp u32 LE][payload][Ed25519 signature 64]` — the signature is over every byte before it, hashed under `"securacv:mesh:message:v0"` (`mesh_crypto::DOMAIN_MESSAGE`) in both trees; the type is read from this **signed** header, never from anything in front of it |
| `8..12` | pre-membership pairing, unsigned | `[type][the raw pairing payload struct]` — there is no peer key to verify against yet; the pairing state machine's own checks authenticate the exchange (§5) |
| anything else | dropped | `0` (canary-wap's version until v0.4), `2..7`, `13..15`, a stale version, the PIO tree's pre-v0.4 unsigned type prefix, a Chirp (`0xC4`) or Beacon (`0xB1`) magic |

The pairing block is `8..12` (canary-wap's numbering) and not the PIO tree's
old `0..4` because a pairing type must never equal a version byte: `0` was
canary-wap's version and `1` is the registry's, so a receiver that keys on
the first byte — both do — could have taken one frame for the other. The
registry's `static_assert`s keep `0`, `1`, `0xC4` and `0xB1` out of every
block. The PIO tree's old prefix byte was an unsigned copy of the signed
type ahead of the envelope; dropping it removed one unauthenticated byte
from the frame and made the two layouts identical — no check was weakened,
since dispatch there already keyed on the signed type.

**The registry.**

| Byte | Name | Payload compatibility across the trees (v0.4) |
|---|---|---|
| 0–7 | reserved — never assign | `0` and `1` are version bytes |
| 8 | `PAIR_DISCOVER` | structs differ (PIO `mesh_pairing`, WAP `PairDiscoverPayload`); cannot pair across trees |
| 9 | `PAIR_OFFER` | same |
| 10 | `PAIR_ACCEPT` | same |
| 11 | `PAIR_CONFIRM` | same |
| 12 | `PAIR_COMPLETE` | same; canary-wap also HKDFs the pairing key (§5.3) |
| 13–15 | reserved — never assign | |
| 16 | `HEARTBEAT` | PIO does not send; WAP struct |
| 17 | `CSI_FEATURES` | PIO only |
| 18 | `TAMPER_ALERT` | **differs**: PIO 6-byte template-only (§4.3), WAP 56-byte struct with free text (`TamperAlertPayload`: 1 + 1 + 2 pad + 4 + 48, sent as `sizeof`) — and since the v0.4 follow-up the WAP refuses any other length, so the PIO 6 bytes are dropped rather than read as the struct |
| 19 | `POWER_ALERT` | WAP only |
| 20 | `OFFLINE_IMMINENT` | WAP only |
| 21 | `WITNESS_RECORD` | PIO only (reserved) |
| 22 | `BEACON_EVENT` | **same** — `mesh_beacon` is staged byte-identical |
| 23 | `CHANNEL_LOCK` | **same** — `mesh_channel_hop` staged |
| 24 | `HUB_ELECTION` | **same** — `mesh_hub_election` staged |
| 25 | `LEAVE_OPERA` | **same** — empty |
| 26–29 | `REKEY_OFFER` / `ACCEPT` / `SECRET` / `ACK` | PIO only (§5.6 PlatformIO subsection) |
| 30–36 | `AUTH_CHALLENGE`, `AUTH_RESPONSE`, `AUTH_COMPLETE`, `PEER_LIST`, `ENCRYPTED`, `OPERA_REKEY`, `OPERA_REKEY_ACK` | canary-wap's per-peer session layer (§3.1, §5.6 v0.2); PIO drops as unknown |
| 37–255 | reserved | |

**What this buys, and what it does not.** A type byte now means the same
message on both sides, and a frame of one tree parses as a frame on the
other: the session test builds a frame byte by byte the way canary-wap's
`send_to_peer` writes it and the PIO session verifies and dispatches it (a
`LEAVE_OPERA`), drops its replay, and drops the version-0 and prefixed
shapes. But a frame is only verified against a **pinned peer key**, and the
two trees cannot pair with each other (the pairing structs and key
derivation differ), so **no cross-tree frame can be verified on a radio
today**; the four payloads marked *same* would verify once pairing is
reconciled, `TAMPER_ALERT` would not until its payload is. A verified frame
of a type the receiver has no handler for (a canary-wap session-layer type
at a PIO receiver) is dropped **after** it advances the sender's replay
counter — it is a genuine frame from that peer — which the session test
states explicitly.

**What remains below the envelope, and what is already the same there.**
The registry settles the bytes of the outer frame; these are the receive
rules under it, tree by tree (v0.4 follow-up, after the review's three
canary-wap findings; host-tested, not bench-verified, awaiting crypto
review):

| Rule | PIO (`mesh_session`) | canary-wap (`mesh_network`) | Across the trees |
|---|---|---|---|
| Counter convention (§3.3) | first counter signed is 1; receiver's last-seen starts at 0; `counter <= last` dropped, no exemption | **same** since the follow-up — `msg_counter_tx` starts at 1 in `add_peer` on a device that has signed none (one past the highest it can have signed otherwise, F99; a rotation resets no counter since F95); the gate is `counter <= msg_counter_rx`, the old `&& rx > 0` exemption gone | **same**: a counter-0 frame is never fresh at either receiver |
| Where a member's address comes from (§8.3) | a completed pairing (persisted only once the session bound it, F102), or NVS `peer_macs` at boot; a member's opera frame from any address but its own bound one drops before verification (F70; from an address the transport table does not hold it always did), and a frame binds, moves or records no address. Until F70 a verified frame's source, which could be any address in that table, was recorded as the member's address and used for its rekey replies (the §8.3 peer-fields note) | a completed pairing (`add_peer`; a re-pair re-binds a member already held, logged; the joiner completes only after its owner confirmed), or NVS at boot (an older firmware's duplicate entry folded into one); since 2026-10-01 a frame from any address but the signer's own bound one drops before verification. It used to re-point the member and its ESP-NOW registration at the source of a frame that passed signature, `opera_id` and replay, and before the v0.4 follow-up it did so ahead of the signature (a frame with a member's public `sender_fp` + `opera_id` and any signature: a keyless denial of service) | **same rule**, source check included since F70: a member's frame is taken only from its own bound address, and no frame binds an address |
| Fixed-size payloads | decoders take the length and refuse any other, exactly (`mesh_alert`, `mesh_beacon`, …; `LEAVE_OPERA` must be empty) | **same** since the follow-up: every struct handler (`HEARTBEAT`, `AUTH_*`, `TAMPER_ALERT`, `POWER_ALERT`, `OFFLINE_IMMINENT`, `OPERA_REKEY[_ACK]`) refuses `payload_len != sizeof(struct)`; `BEACON_EVENT`, `CHANNEL_LOCK`, `HUB_ELECTION` already decoded through the staged modules | **same rule**; the encodings still differ where the registry table says so |
| Payload encodings, pairing exchange | | | **differ** — the registry table above, §4.3, §5.3, §8.3 |

**Compatibility — this is a wire break, stated plainly.** A canary-wap
built before v0.4 and one built after drop each other's Opera frames
(version 0 vs 1), and would read each other's type bytes differently if
they did not; a PIO Canary before and after v0.4 likewise (the prefix
byte, and pairing `0..4` vs `8..12`). There is no version negotiation and
no dual-decode: a mixed opera does not degrade, it goes silent. The
project has no record of an opera formed on a radio in either tree (U1
Track C2 is open), which is why v0.4 renumbers rather than adds a second
decode path — but that is a statement about what has been tested, not a
proof about every flashed device. A device that has stored an opera from a
pre-v0.4 build must be re-paired after the update, and every member of an
opera must be updated together.

## 5. Pairing Protocol

### 5.1 Overview

Pairing adds a new device to an existing opera (or creates a new opera). The process requires physical proximity and user confirmation to prevent unauthorized joins.

### 5.2 Pairing Flow

1. **Initiator** (existing opera member) enters "pairing mode" via UI
2. **Joiner** (new device) enters "join opera" mode via UI
3. Devices discover each other via ESP-NOW broadcast or WiFi scan
4. **Visual Verification**: Both devices display 6-digit code derived from session
5. User confirms codes match on both devices, in either order
6. Opera secret is securely transferred to joiner
7. Joiner's public key is added to all opera members

canary-wap (F75, host-tested, not bench-verified): the initiator keeps a
joiner's `PAIR_CONFIRM` that arrives before its own owner confirms (verified,
only from the pairing partner's address, and only once the code is shown:
before the ACCEPT the session key is all zero, so anyone can compute that
hash) and sends `PAIR_COMPLETE` when its owner does, without a `PAIR_CONFIRM` of its own first: two frames back to
back can meet the joiner's one-frame receive buffer, and the COMPLETE would be
the one dropped. Until F75 it dropped such a CONFIRM, nothing re-sends one,
and a pairing whose joiner was confirmed first timed out. A CONFIRM from any
other address counts for nothing on either side. The address is not
authenticated, though, and the CONFIRM hash is the same in both directions,
so the initiator's own CONFIRM, re-sent to it from the joiner's address,
counts as the joiner's: the initiator completes and holds a joiner whose
owner never confirmed, and the joiner drops the COMPLETE (host-probed on
canary-wap, the same before F75, and on the PlatformIO tree, which computes
the same role-free hash, the same before and after F97; open: a hash bound
to the sender's role is a wire change).

PlatformIO tree (F97, host-tested, not bench-verified): the same rules.
Until F97 `either_handle_confirm` acted only in `AWAITING_CONFIRM_PEER`,
after this device's own owner had confirmed, so a CONFIRM that arrived
earlier was dropped, and neither side sends its CONFIRM twice. No order
completed with frames delivered as they are sent: a joiner confirmed first
left both sides at the 5-minute timeout, and an initiator confirmed first
had its CONFIRM dropped by the joiner, which then waited for it and dropped
the `PAIR_COMPLETE` too, so the initiator reported the pairing done and held
a member that never joined, while the joiner timed out (host-probed, both
orders). Now a CONFIRM counts only from the pairing partner's address and
only once the code is shown (`AWAITING_CONFIRM` or later); the initiator
keeps a joiner's early CONFIRM and completes at its own owner's confirm; the
joiner checks the initiator's CONFIRM in either order and takes the
`PAIR_COMPLETE` once its own owner has confirmed, which needs nothing else
from the initiator (only the session key opens it); and a wrong hash from
the partner's address ends the pairing in either order.

One difference from canary-wap: the PlatformIO initiator sends its own
`PAIR_CONFIRM` immediately in front of every `PAIR_COMPLETE`, in both
orders. An updated joiner does not need it. A joiner on firmware before F97
does: it reads a CONFIRM only after its own owner confirmed and takes a
COMPLETE only after such a CONFIRM. With the COMPLETE alone, an updated
initiator reported the pairing done and kept the member while that joiner
dropped the COMPLETE, in both orders; with the CONFIRM in front, the pair
completes in both orders (host-probed against the pre-F97 code). Two frames
back to back are safe on this tree, whose transport ring holds eight
received frames; canary-wap's one-frame buffer is why it sends the COMPLETE
alone.

The other mix (F117, host-tested, not bench-verified): an initiator on
firmware before F97 drops a CONFIRM that arrives before its own owner
confirms, and answers only one it reads after that, with a `PAIR_COMPLETE`
alone. Until F117 the updated joiner sent its CONFIRM once, so when the
joiner's owner confirmed first both sides timed out, as two pre-F97 devices
did, and neither kept the other (host-probed with the pre-F97 library
compiled beside this one). Now the joiner sends its CONFIRM again once it
has read the initiator's CONFIRM after its own owner confirmed, which says
the initiator's owner has confirmed too: 1 s after that, then every 2 s, at
most three times, only to the pairing partner and only while no COMPLETE
has come. The pre-F97 initiator answers the copy with the COMPLETE, and the
pair completes in both orders. An updated initiator sends its COMPLETE right
behind its CONFIRM, so two updated devices exchange the same frames as
before (host-probed, both orders, the owners' confirms 0 s to 4 minutes
apart); a copy that did go out would reach an initiator already done, which
drops it.

A device that cannot hold its partner fails the pairing: a deny-listed key
(§5.6), a new member for a full opera, a re-pair onto an address another
member holds, a new member at such an address (canary-wap since F98: one
address, one member; its `add_peer` used to append one, and the two
entries shared one ESP-NOW registration that removing either deleted for
both), or an address its radio cannot register. canary-wap logs a refusal
because another member holds the address on a line of its own, which names
the way through: remove that member first. The routine case is a device
whose NVS was erased or that was reflashed: it keeps its radio address and
comes back with a new key, so each member that still holds its old entry
refuses it until that entry is removed, and on canary-wap each such removal
splits that member from the opera until F48 (§5.6, F95; host-tested).
PlatformIO tree (F118, host-tested, not bench-verified): the session asks
whether it can hold the partner — the same refusals its trusted-peer table
and its address binding make: a deny-listed key, a new member for a full
opera (8), an address another member is bound to, a broadcast, group or
zero address, no room in the transport table — at this side's owner's confirm,
before any `PAIR_CONFIRM` goes out (both roles); on the initiator again
before it seals the `opera_secret` into the `PAIR_COMPLETE`; on the joiner
again before it opens one. A refusal ends the pairing: nothing is sent,
sealed, opened or stored, a refusal at the confirm answers the confirm
request with `409 partner_refused`, and every refusal goes to the health
log with the partner's fingerprint. A
joiner that refuses at its confirm sends no CONFIRM, so its initiator never
seals the secret to it and times out; unlike canary-wap, the initiator does
not end up holding a member that refused. Until F118 the PlatformIO
initiator sealed the secret first, both sides reported success and stored
the partner's key, and only then did the address bind fail: a trusted
member heard from nowhere and sent nothing, holding a slot until removed.
The transport-table case is reached only by a joiner whose own
`PAIR_ACCEPT` could not be sent for want of that room (its initiator never
shows a code): an initiator's partner, and any joiner's that got its
ACCEPT out, already has its address in the table for the pairing's
replies.
canary-wap (F73, host-tested): the initiator adds the joiner before anything is sent, so on a
refusal no `PAIR_COMPLETE` goes out and its joiner times out; a joiner that
refuses the initiator keeps the opera it had, in RAM and in NVS. Either way
nothing is stored, the device is not `MESH_ACTIVE`, the refusal is logged and
the pairing callback reports failure. A joiner's refusal comes after its
initiator has added it; the initiator cannot know. Until F73 both handlers
ignored the refusal, persisted, went `MESH_ACTIVE` and reported success, and
the initiator sealed the `opera_secret` to a partner it then did not hold.

canary-wap (F100, host-tested, not bench-verified): the initiator sends its
`PAIR_COMPLETE` again every 2 s until it hears the joiner (the joiner's
first verified frame: nothing on the wire acknowledges a COMPLETE, and a
joiner that took it sends its heartbeat within 30 s, §7.1), for at most the 2-minute
pairing timeout after the first send, which outlasts the joiner's own wait.
It stops early if the joiner is no longer a member or the opera rotated or
was left, and a window that ends unanswered is logged once, as a COMPLETE
that could not be sent if no copy went out. The copies are the frame
already sent; the pairing key stays wiped. Until F100 the COMPLETE went
once and its send was not checked, so one lost on the air, or refused by
the storm gate, left the initiator holding a member that never joined while
the joiner timed out. The pairing still reports success at the first
COMPLETE. On the joiner, a COMPLETE that does not open under its pairing
key is dropped and the pairing goes on (to its timeout, if no good one
comes): it used to end the pairing, from any address, so any radio could
cancel a confirmed pairing, and the copies of an earlier pairing's
COMPLETE would end the same joiner's next pairing with that initiator.
"Heard" is judged against the joiner's last-seen counter when the COMPLETE
went out, which for a joiner this device dropped before is its tombstone
since F116 (§3.3, §4.2), not 0: a frame it signed before, replayed from its
address at the re-pair, no longer ends the copies. A genuine frame it sent
another member and this device has not heard still can (the envelope names
no destination, F72), as can one heard since this device's last 5-minute
last-seen save before a reboot.

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

**Ephemeral keys (v0.3, F33 — crypto review pending, maintainer to
confirm).** `initiator_ephemeral` and `joiner_ephemeral` are X25519 keys:
32 random bytes clamped per RFC 7748 §5 (the low three bits cleared, bit 255
cleared, bit 254 set), with the public key the clamped scalar times the base
point. Until F33 both trees generated them with the **Ed25519** generator and
ran X25519 over the result; an Ed25519 public key is an Edwards point derived
from SHA-512 of the seed, not the seed times the X25519 base point, so the two
sides derived different session keys and the codes could not match on a
device. The PlatformIO tree now uses `mesh_crypto::x25519_generate_keypair`
(and its host tests run a real X25519, the RFC 7748 ladder, rather than a shim
that agreed whatever the keys were); canary-wap uses
`mesh_pair_crypto::generate_keypair` (`mesh_pair_crypto.h`). Two divergences
remain, both open: canary-wap feeds the X25519 output through HKDF-SHA256
(`"securacv:mesh:session:v0"`) before the code and the AEAD, where the formula
above and the PlatformIO tree use it directly — so the two trees still would
not show the same code to each other (since v0.4 they number the pairing
frames alike, `8..12`, §4.5, but the payload structs differ); and
canary-wap's AUTH exchange (§3.1) still runs X25519
over the long-term Ed25519 identity keys, the same class of bug, in the
per-peer session keys its §5.6 rekey encrypts under. Neither tree has paired
on a radio yet (U1 Track C2).

### 5.4 Creating a New Opera

If no opera exists, the first device generates:
```
opera_secret = random_bytes(32)
opera_id = SHA-256("securacv:opera:id:v0" || opera_secret)[0:16]
```

Both trees found an opera on `POST /api/mesh/pair/start` when the device
holds none, and then start the initiator pairing with it. canary-wap names it
`"My Canary Opera"` (or the request's `name`) and keeps it in RAM if the
flash-encryption-gated save is refused. The PlatformIO tree (v0.3, F33) does
the same on the same route, behind the gates that route already had (bearer
token, rate limit, flash encryption), with the same default name (it takes
no `name`; `POST /api/mesh/name` renames), but fails closed: the
secret is drawn on the main loop and persisted before anything uses it, and
if it cannot be persisted no opera is created (`opera_not_persisted`) — a
secret the founding device forgot at its next reboot would strand every
device that joined it. It never replaces an opera the device already holds
(`opera_exists`). §8.3 has the route.

canary-wap stores an opera only while it has one (F113, host-tested): a
leave, and a turn on or rename after it, removes the `opera_id` and
`opera_sec` keys, and a boot does not load an all-zero id or secret. Its
leave used to store the zeroed config as it stood, the next boot loaded
that as an opera, and `pair/start` then kept it instead of founding one:
the joiner derived its `opera_id` from the zero secret it was sent, the
initiator kept the stored zero id, and each dropped the other's frames. NVS
an older firmware's leave wrote is refused the same way (logged once at
that boot) and overwritten at the next pairing.

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
opera member. The FE gate keeps the secret off un-fused boards, at the cost
of refusing to work on dev boards without FE — which is the correct
trade-off for a production safety system. It does not eliminate the attack
on a fused board: flash encryption does not cover NVS (ESP-IDF encrypts
only the app, OTA-data and NVS-key partitions), so a stored `opera_secret`
is still plaintext on the flash chip there. Only NVS encryption protects
NVS at rest, and it is not available under `framework = arduino`, which
both mesh trees (canary-wap and the PlatformIO canary) build on (roadmap
item 9 in `firmware/ESP32S3_OPTIMIZATION_ROADMAP.md`). That is why §11.2
still lists physical compromise as not mitigated;
`docs/security/THREAT_MODEL.md` (the Opera mesh section) says the same.

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
(default 7 days), even by a freshly-rotated opera. (Implemented in both
trees since v0.3, F33 — "The revocation deny-list" at the end of this
section; crypto review and bench pass pending.)

Caveat: the removed device, while it still has the *old* `opera_secret`,
cannot impersonate a current member because the surviving members no longer
accept frames carrying the old `opera_id` after rotation. The old
`opera_secret` is forensically useful (for log decryption) but operationally
inert.

**canary-wap today (sweep F95, open; host-probed, not bench-verified).**
Step 4 reaches no member: nothing opens a session (§3.1), so no member
holds a session key and `MSG_OPERA_REKEY` goes to none. With no ACK
pending the remover commits at once and moves alone to the new
`opera_id`; every survivor stays on the old one, keeps trusting the removed
device (the message would not have named it anyway), and is split from the
remover until it re-pairs with it. Opening the session as the exchange
stands does not help (§3.1: the response does not fit a frame, and the keys
would not agree); that is F48's wire and derivation change. So the caveat
above does not hold on canary-wap yet: the removed device stays trusted by
every survivor, and only the remover drops it. Since F95's counter fix a
rotation keeps every counter (§3.3), so the re-pair that rejoins a survivor
is heard at once.

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
   AAD `rekey_id || initiator_fp || survivor_fp`. Since v0.3 (F33) no
   `REKEY_SECRET` goes out before `REKEY_SETTLE_MS` (6 s, one OFFER
   retransmit period plus a second) after the start; ACCEPTs that arrive
   sooner are held and answered when the window closes (concurrent
   removals, below).
4. The survivor decrypts, sends `REKEY_ACK {rekey_id}` **under the old
   `opera_id`, before switching** (an ACK under the new id would fail the
   initiator's `opera_id` check — the ordering canary-wap learned), then
   installs the new secret, drops `removed_fp` and re-persists.
5. The initiator commits when every survivor has ACKed, or at 60 s; a
   survivor that did not ACK is dropped and must re-pair (canary-wap's
   accepted trade-off). Until then it re-broadcasts the same OFFER every
   5 s, which heals a lost OFFER, ACCEPT or SECRET (the survivor answers,
   or repeats its ACCEPT and draws a fresh SECRET). A lost ACK does not
   heal: that survivor has already switched and drops old-`opera_id`
   frames, so it holds the new secret but the initiator drops it. A
   survivor that never gets its SECRET aborts at its own 60 s mark and
   keeps the old secret (since F33 it has already forgotten the removed
   device — it did so on the OFFER, below).

On a switch the outbound counter is **kept**: receivers track the per-peer
counter by fingerprint, not by `opera_id`, so resetting it would get the
next frames dropped as replays. On commit each device first drops from NVS
every peer the rotation dropped (on the initiator: the removed device and
any survivor that did not ACK; on a survivor: the removed device), and only
then re-persists the new secret through the flash-encryption gate (§5.5):
a power cut between the writes leaves the old secret without the dropped
peer, never the new secret beside it. If a removal or the save is refused,
the old secret is cleared rather than left for the next boot. The ephemeral keys come from a
dedicated X25519 generator (`mesh_crypto::x25519_generate_keypair`, RFC 7748
clamping) — the generator pairing uses too since F33 (§5.3).

**What the rotation does and does not buy here.** In this tree
`opera_secret` has one use besides being handed on at pairing — deriving
`opera_id` — and `opera_id` travels in cleartext in every frame header;
frames are authenticated by the sender's Ed25519 key alone. So the step that actually excludes the removed
device is each survivor **unregistering its pubkey** (at install, and on
the initiator at `remove`), not the new secret: the removed device can
copy the new `opera_id` off the air, and it stays accepted by any survivor
that did not unregister it. Since v0.3 (F33) a device unregisters and
deny-lists the removed device as soon as it verifies an OFFER naming it, so
a survivor that answered the OFFER but aborted without its SECRET no longer
keeps it; what remains is a survivor that missed every copy of the OFFER for
the whole 60 s window. Such a survivor trusts the removed device
indefinitely and gets no signal that it was itself dropped. What the rotation does buy: every frame signed before the
removal carries the old `opera_id` and is dead to every survivor that
switched — also across a later re-pair of the removed device — and a
survivor that missed the rotation is visibly split onto the old id instead
of silently in step.
The §5.6 caveat above ("cannot impersonate a current member because the
surviving members no longer accept frames carrying the old `opera_id`")
therefore describes canary-wap's session-key design, not this tree.

**Concurrent removals (v0.3, F33 — crypto review and U1 Track C3 bench
pass pending).** One rotation at a time per device still holds (a second
`remove` is refused while one runs). Two users removing peers from two
devices inside the same 60 s window used to split the household between two
new secrets; the PlatformIO tree now converges them on one:

- **Settle.** No SECRET before `REKEY_SETTLE_MS` (step 3), so two initiators
  that start close together each hear the other's OFFER, or its retransmit,
  while neither has handed anything out.
- **Precedence.** Of two concurrent rotations the one whose initiator's
  fingerprint is lower (bytewise) wins. An initiator that hears a preceding
  OFFER before it has handed out a secret yields: its rotation ends
  uncommitted and it answers the winner as a survivor. A survivor still
  waiting for its SECRET switches to a preceding OFFER, and drops a rotation
  whose initiator another OFFER names as removed. An OFFER that names this
  device as removed ends this device's own rotation.
- **Propagation.** Every device that verifies an OFFER deny-lists and
  forgets its `removed_fp` — also one it cannot join because a rotation of
  its own is running — and a running rotation drops that device from its
  survivors. So neither rotation hands either removed device the new
  secret.
- **Re-announce.** A yielded initiator starts its removal again once the
  winning rotation is over, so a winner that never heard the first OFFER
  still drops that device.

What remains, stated rather than hidden: two initiators that have both
handed out a secret before hearing each other (every copy of both OFFERs
lost for a whole settle window) still split, and the losing side re-pairs;
two devices that remove *each other* can end on two secrets by arrival
order; and a lost ACK still drops a survivor (step 5). The host simulation
(`test_mesh_rekey.cpp`, `test_two_removals_converge`) converges ten
orderings of two removals, each with scheduled losses, on one secret; it
models no random radio loss, and a random-loss probe run while writing it
(not committed) still split a few percent of runs at 5 % frame loss, more at
higher loss. None of it has run on radios.

canary-wap cannot converge concurrent removals without a wire change: its
`MSG_OPERA_REKEY` carries the new secret directly, with no announcement
phase to settle in and no field naming the removed device, so neither
precedence nor propagation has anything to act on. Two removals from two
canary-wap devices inside one window can still split that household. It
gets the deny-list only (below).

**The revocation deny-list (v0.3, F33 — both trees; crypto review and bench
pass pending).** `mesh_revocation.{h,cpp}` in the PlatformIO mesh library,
staged byte-identical into the canary-wap sketch
(`firmware/scripts/check_mesh_sync.sh`): at most 8 fingerprints, each
refused for `REVOCATION_GRACE_MS` = 7 days; a full list evicts the entry
with the least grace left, so the removal just made always fits. Time is
uptime, like every mesh timeout; the list is persisted as
(fingerprint, remaining ms) — at each removal and every 5 minutes while it
holds anything — and restored with that remaining time from the next boot's
clock, so time powered off does not count down (a device that is off for a
day denies for a day longer — the conservative direction). Persisted behind
the flash-encryption gate like the peer list (§5.5, §12.3); on an FE-off
board it lasts until reboot.

- PlatformIO: records the peer removed on this device and the `removed_fp`
  of every verified OFFER it hears (propagation, above); refuses a
  deny-listed device's pairing DISCOVER / OFFER before the pairing state
  machine sees it, and refuses it as a trusted peer. The list survives
  leaving the opera.
- canary-wap: records the peer removed on this device; refuses it in the
  pairing handlers (DISCOVER, OFFER) and in `add_peer`. It cannot learn of
  a removal made on another member (the rotation does not name the device).

## 6. Alert Propagation

### 6.1 Broadcast Behavior

When a canary detects a critical event:

1. Immediately broadcast `TAMPER_ALERT` or `POWER_ALERT` to all peers
2. If power is failing, broadcast `OFFLINE_IMMINENT` as final message
3. Messages are relayed by other opera members (max 3 hops) — **specified,
   not built (v0.2)**: today an alert is heard only by peers in direct
   range of the sender
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

canary-wap (F76, host-tested, not bench-verified) sends its heartbeat
(§4.2) every 30 s in `MESH_CONNECTING` as well as `MESH_ACTIVE`, to every
member whatever its peer state (§7.2): that is how an opera that has heard no
member gets heard. It used to send it only in `MESH_ACTIVE`, which needs a
member heard, and only to members at `PEER_CONNECTED` or later, so an opera
whose members had all rebooted, or a fresh pairing (each side holds the other
at `PEER_UNKNOWN`), sent nothing at all. Its alerts and its Beacon,
channel-lock and hub-election sends still go to members at `PEER_CONNECTED`
or later only. (The PlatformIO tree sends no heartbeat.)

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

**canary-wap: the changing routes run on the loop task (sweep F96).** The
PlatformIO tree's rule (§8.3) holds on canary-wap too: `pair/start`,
`pair/join`, `pair/confirm`, `pair/cancel`, `leave`, `name`, `enable`,
`remove` and the alerts `DELETE` no longer change the peer table, the
pairing session, the opera config (or its NVS handle) or the alert history
from the HTTP server's task, where they raced `mesh_network::update()`. The
handler validates its body and hands one command to a four-slot ring that
`update()` drains on the loop task before anything else (a disabled mesh
included, so `enable` can turn it back on), then waits up to 2 s for the
loop task to start it. Two errors follow, with the PlatformIO tree's codes:
`mesh_busy` (409, four requests already waiting) and `mesh_timeout` (503,
the loop task did not start it in time; it was withdrawn and did not run).
A command the loop task has started is waited for and answered with its
own result, so every other answer is unchanged. A request sent while the
device is still booting, before its loop runs, gets `mesh_timeout`. The
replay counters that `POST /api/reboot` and the safe-mode retry save
before the restart go the same way (one more command through the same
ring). The loop task writes them; a save it does not start within 2 s does
not run, and the periodic save (every 5 minutes) is the last one on flash.
Host-tested (`test_mesh_commands_wap.cpp`, `test_loop_command_ring.cpp`);
the Arduino compile is CI's; not bench-tested.

**canary-wap: the status routes read what the loop task published (sweep
F110).** `GET /api/mesh`, `/api/mesh/peers` and `/api/mesh/alerts` read the
peer table, the pairing session, the opera config and the alert history
from the HTTP server's task while `update()` wrote them, so one response
could mix two passes (a member's name read mid-shift after a removal, a
pairing code read while a cancel wiped it). `update()` now publishes a view
at the end of every pass (its early return included; `init()` publishes the
first, since the HTTP server starts before it): the status, the opera's
name and enabled flag, the pairing code while it is shown, and each member
as the peer list shows it, with no key. `/api/mesh` and `/peers` read a
whole copy of the last one; `/alerts` reads the alert history whole and
from one moment, as the loop task changes it under a lock
(`loop_snapshot.h`). Neither waits for the loop task, so these three never
answer `mesh_busy` or `mesh_timeout`; their responses are unchanged
(uptime is counted at the read), and a refused allocation for the alerts
copy answers 500 `out_of_memory`. Host-tested (`test_loop_snapshot.cpp`,
under real threads and ThreadSanitizer too, and `test_mesh_commands_wap.cpp`);
the Arduino compile is CI's; not bench-tested.

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
and pass through the same rate limiter as the rest of the REST API. The
five F10 mutations — `leave`, `name`, `enable`, alerts `DELETE`, `remove` —
and, since v0.3 (F33), the four pairing routes — `pair/start`, `pair/join`,
`pair/confirm`, `pair/cancel` — run on the main loop that owns
`mesh_session`'s state, not on the HTTP server's task: each handler
validates its body, hands one request to a one-deep slot and waits (at most
about 3 s, twice) for the main loop's next `mesh_session::process()` to
execute it (a direct call from the handler file does not compile). Two
extra errors follow from that: `mesh_busy` (409 — another mesh request
holds the slot) and `mesh_timeout` (503 — the main loop did not reach it in
time; the request was withdrawn and did not run, or it was abandoned while
running and its result discarded). A late result never answers a later
request: the slot holds one request at a time and discards an abandoned
one's result before it frees. The pairing routes' `mesh_disabled` and
`rekey_in_flight` refusals are now decided on the main loop, after the
handler's own `no_flash_encryption` check. `pair/confirm` answers
`partner_refused` (409, F118) when this device cannot hold the partner
(§5.2): the confirm ended the pairing and sent nothing; any other refusal
of a confirm is still `confirm_failed` (400).

**`pair/start` founds an opera (v0.3, F33):** with no opera secret in NVS
the route no longer answers `no_opera`: the main loop founds one (§5.4) —
it draws the secret, has main.cpp persist it (NVS `opera_secret`, with the
default name `"My Canary Opera"` in `opera_name`, best effort), installs it
and starts the initiator pairing, answering `{ok, created: true, state:
"PAIRING_INIT"}` (`created: false` when it added to an existing opera).
Errors, besides the others above: `opera_not_persisted` (500 — the secret
could not be stored; nothing was created), `opera_exists` (409 — the
session holds an opera NVS did not return, e.g. a join that finished
meanwhile; it is never replaced, and a retry adds to it), and
`pair_start_failed` (400 — a pairing is already running, and nothing was
created; or, rarely, the pairing's key generation failed after the opera
was created and persisted — `GET /api/mesh` then shows it). No new route: the web UI's "Create Opera" button and the Home
Assistant wizard's "Add another Canary" already call it.

**`remove` (F10-rekey — crypto review and bench pending):** body
`{"fingerprint": "<16 hex>"}`, the string `GET /api/mesh/peers` emits. It
never drops a peer without rotating: §5.6 requires that removing a peer
rotate `opera_secret` and hand the new one to the survivors — so the PIO
route starts the §5.6 PlatformIO rotation first and forgets the peer only
if the rotation started. (In this tree the exclusion itself is every
survivor unregistering the removed pubkey; the rotation is what makes the
removed device's earlier frames dead — §5.6, "What the rotation does and
does not buy here".) Responses: `{ok, rekey:
"started"}` (survivors are being re-keyed; the commit lands within 60 s),
`{ok, rekey: "committed"}` (nobody left to tell — rotated locally at once),
`persisted` for the removed peer's NVS entry; errors `unknown_peer` (404),
`rekey_in_flight` (409 — one rotation at a time; `enable {false}`, `leave`,
`pair/start` and `pair/join` are refused with the same code while one runs,
since each would strand or split it), `pairing_in_progress` (409 — a
pairing in flight would hand its joiner the secret being retired),
`no_opera`, `mesh_disabled`, `invalid_fingerprint`. canary-wap's rotation is its own (per-peer session
keys, `MSG_OPERA_REKEY`); the two trees do not rotate each other.

**`leave`:** the device signs a `LEAVE_OPERA` (§4.2) under the opera it is
leaving and sends it to every member's bound radio MAC (best effort — the
response carries `notified: true|false`, true only if a member took it; a
running pairing's partner does not count, F101), then forgets everything
opera-scoped: the secret,
the trusted peers, the elected hub and the name, in RAM and in NVS
(`persisted: false` if an NVS clear failed). It keeps only replay-defense
state: the peers' last counters (as tombstones, in RAM and in NVS
`replay_ctrs`) and its own outbound counter, so a later re-pair into the
same opera can neither be fed the peers' old frames nor have its own new
frames dropped (§4.2). Survivors that verify the frame drop the leaver's
trust entry, in RAM and in NVS, and keep its counter as a tombstone. No
rekey (§4.2).

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
returns `{ok, count, uptime_ms, alerts:[{timestamp_ms, type, severity,
sender_fp, sender_name, detail, witness_seq}]}` newest first: `type` is
`"TAMPER"`, `detail` is the kind's template name, `timestamp_ms` is the
receiver's uptime at receipt (the canary-wap basis too) — not a wall-clock
time — and `sender_name` is `""` until a peer-metadata store exists.
`uptime_ms` (v0.3, F33) is the receiver's uptime when the response was
built, so a client can show each alert's age (`uptime_ms − timestamp_ms`,
modulo 2³² across the `millis()` wrap); the PIO web UI shows "received … ago"
and, without `uptime_ms`, no time at all — until F33 it rendered the uptime
as a time of day. canary-wap's web UI shows no alert time. `DELETE` clears the history; the counters keep
counting. Counters and history are **per boot** — not persisted. Relay
(§6.1 step 3), `POWER_ALERT` and `OFFLINE_IMMINENT` are not implemented.
**Still not interoperable with canary-wap at the payload:** since v0.4 the
two trees agree on the outer frame and the type byte (18 in both, §4.5),
but they carry different `TAMPER_ALERT` payloads (6-byte template-only vs
54-byte struct); nothing here delivers alerts across trees (the WAP's own
`broadcast_tamper_alert` has no caller, so it sends none either), and the
trees cannot pair with each other yet, which any cross-tree verification
needs first.

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
`mesh_session` reports each peer's bound radio MAC once a **fully
verified** opera-authenticated frame (signature, opera_id and replay checks
all passed) has arrived from it, and the handler joins that MAC into the
transport table's liveness. The checks prove who signed a frame, not which
radio sent it — the envelope signs no address (the F49 part 3 note below)
— so a member's frame is taken only from that member's own binding (F70):
from any other address, even one the transport table holds, it is dropped
before verification, spends no counter and reaches no handler, and no frame
binds, moves or records an address. A member with no binding (none restored
from `peer_macs` at boot, an entry the boot restore dropped as shared, F119,
or a pairing whose bind was refused) has no
address its frames are taken from until a pairing binds one. Until F70 the PlatformIO session took
a member's verified frame from any address in that table and recorded
that address as the member's, and the table holds more than the member's
own: while a pairing runs, the partner's (an outsider that answers the
pairing from its own radio needs no spoofing), and every other member's (a
radio can copy one; ESP-NOW does not authenticate a source). Either could
deliver a member's not-yet-heard frame and become its recorded address,
which the session also used as the destination of its rekey unicasts to
the member and took out of the transport table when it forgot the member.
That sent the receiver's `REKEY_ACCEPT` to the outsider, and a later
removal of the member could strand the member whose address was copied
(host-probed, the same before #1756). The rekey unicasts now go to the
binding, and forgetting a member removes only the binding. A radio copying
the member's *own* address still gets the member's not-yet-heard frames
dispatched, as on canary-wap: nothing tells it apart, and it moves no
address (a member's later frames still count above it, since a PlatformIO
sender uses one counter, §3.3). A trusted peer that has not sent a verified
frame this boot — or since a re-pair moved its binding, or whose MAC has
aged out of the transport table — reports the OFFLINE/never defaults, and
`peers_online` in `GET /api/mesh` counts only peers heard this boot (v0.3,
F33).

**The transport peer table (v0.3, F33 part 1).** The ESP-NOW transport
delivers frames only from MACs in its peer table and `broadcast()` sends only
to them; before F33 nothing on a device filled it (only host tests did), so
every inbound frame was dropped as `recv_dropped_no_peer` and every
broadcast — pairing replies, alerts, leave, rekey — reached nobody.
`mesh_session` now keeps it in step with the trusted peers: each peer's
radio MAC is learned when a pairing completes (the address the partner
paired from), persisted in NVS `peer_macs` (§12.3) once the session has
bound it (F102, below) and bound again at boot;
while a pairing runs, the partner's MAC is added for the unicast replies
and pairing frames from a MAC not in the table reach the pairing state
machine (nothing else from an unknown MAC does); a peer dropped by a
verified `LEAVE_OPERA`, a removal or a rotation leaves the table with it,
and a pairing that ends without a new member removes the partner's MAC
again. A finished pairing (paired, canceled or timed out) no longer
blocks the next one — until F33 the first pairing a device ran was its last
until a reboot. Host-tested (`test_mesh_session`, `test_mesh_transport`,
`test_mesh_state`); not yet run on two radios (U1 Track C2).

**Opera sends reach bound members only (F101).** Every opera sender —
`TAMPER_ALERT`, `BEACON_EVENT`, `CHANNEL_LOCK`, `HUB_ELECTION`,
`LEAVE_OPERA`, the rekey OFFER — unicasts the signed frame to each trusted
peer's bound radio MAC and to no other address, and its result (the
sender's `true`, leave's `notified`) counts only those sends. Until F101
they used the transport's `broadcast()`, which sends to every address in
the table, and while a pairing runs the table holds the partner's address,
which an outsider gets there by answering the pairing from its own radio:
with no member at all, a tamper alert reported sent and a leave reported
`notified`, each with only the outsider's copy sent (host-probed). The
frames are signed, not encrypted (§11.1 item 4), so the outsider learned
nothing a radio in range could not overhear; the harm was the false
"sent". A member with no binding is sent nothing, as it is heard from
nowhere. Host-tested; not bench-verified.

**A pairing's address is persisted only once it is bound (F102).** When a
pairing completes, the session runs the paired callback (which registers
the member), binds the member to the address it paired from, and then
reports that bind through a second callback, from which `main.cpp`
persists the address, and only when the bind took. `bind_peer_mac` refuses
an address another member holds, a full transport table, or a member the
callback could not register; NVS `peer_macs` likewise refuses an address
another fingerprint holds (one address, one member, §12.3). Until F102
`main.cpp` persisted the address from the paired callback, before the
bind, and the boot restore binds `peer_macs` in stored order: a re-pair of
member J from member C's address (which the unauthenticated re-pair of
§11.1 item 5 lets a relay present) left the session holding J at its old
address and NVS holding J at C's, so after a reboot J took C's address,
C's bind was refused, and C was not heard at all (host-probed). A refused
bind now keeps the member's previous binding in RAM and NVS (a new member
has none, and is heard from nowhere until it pairs from a free address),
and is logged by fingerprint. Host-tested (`test_mesh_session`,
`test_mesh_state`, and a source pin on `main.cpp`'s wiring in
`scripts/tests`); `main.cpp` is compiled by CI only; not bench-verified.
Since F118 the refusals the session can foresee (an address another member
holds, a full opera) end the pairing before it completes (§5.2), so the
bind's report is left to what the session cannot foresee (the paired
callback not registering the member, the radio driver refusing the
address).

**The boot restore drops shared and stale entries (F119, F120).** At boot
`main.cpp` registers the stored pubkeys and hands the stored `peer_macs`
entries to `mesh_session::restore_peer_macs`, which classifies every entry
before it binds any:
- an entry whose fingerprint is not a registered peer — a member removed
  while its entry stayed (the NVS removal is best effort), a deny-listed
  key — is never bound and is dropped from NVS (F120). Kept, it held its
  address in `peer_macs` for good, and since F102 `peer_macs` refuses an
  address another fingerprint holds, so a member pairing from it was bound
  for the boot and not stored: unheard after every reboot;
- two registered fingerprints' entries at one address — a blob written
  before F102, when a re-pair from another member's address was stored, in
  place, so the blob's order says nothing about which entry is right — are
  both left unbound and both dropped, and the shared address goes to the
  health log (F119). Both members are then heard from nowhere until each
  re-pairs, and the re-pairs are stored. Binding in stored order, as before,
  gave the address to the first entry, which in F102's scenario was the
  member that did not own it, and the owner was not heard at all. Keeping
  both entries unbound would not have let the owner's re-pair be stored (the
  other entry still holds the address), so after the next reboot the owner
  would be unheard again;
- every other entry is bound as before.
Entries are dropped only when the pubkey list was read: a failed read
registers nobody, and every entry would look stale. Which entries go is
`mesh_session::stored_mac_must_drop(verdict, peers_loaded)` (true exactly for
the two kinds above, and only when the list was read), and `main.cpp` drops
what it says and nothing else. Host-tested (`test_mesh_session`, every
verdict against both values of `peers_loaded`; the `scripts/tests` source pin
holds `main.cpp`'s restore to its exact form); not bench-verified.

**No address learning from opera frames (v0.3, F49 part 3 — withdrawn).**
A trusted peer's radio MAC can change without a new identity (a module
swapped onto the same device, a new locally-administered address). #1756
healed that from the frames themselves: the transport's unknown-sender path
handed an opera envelope from an address the table did not hold to
`mesh_session`, and a frame that passed the full receive verification —
signature under the sender fingerprint's key, `opera_id` match, strict
counter — from a peer bound elsewhere re-bound the transport table to the
new address and persisted it (NVS `peer_macs`). That verification does not
establish an address. The envelope (§4.5) signs version, type, `opera_id`,
`sender_fp`, counter, timestamp and payload — no source address and no
destination — and a PlatformIO sender spends one outbound counter across
every destination (§3.3). So a genuine frame the receiver has not heard
yet — a broadcast it missed, a rotation frame (§4.2 `REKEY_*`) the sender
unicast to another member, or, once the receiver reboots, one it heard
after its last replay-counter save — passes every check from whatever
address delivers it. The PlatformIO tree's frames are signed, not sealed, and its
transport registers peers with `encrypt = false`, so anyone in range can
record one and deliver it from their own radio. A host probe against the
merged code did exactly that: the receiver moved the member's binding to
the outsider's address, its next rotation's OFFER went to the outsider
alone and, the member sending nothing in that window, the 60 s commit
dropped it; a replayed `REKEY_OFFER` sent the receiver's `REKEY_ACCEPT` to
the outsider. The member's next
genuine frame would have moved the binding back, but on a quiet opera
(there is no heartbeat on this tree) that can be a long wait.

So the relearn is withdrawn. A verified opera frame is **not** proof of
its sender's radio address, and an implementation MUST NOT bind or re-bind
a peer's address from one. The PlatformIO tree drops every opera frame from
an address its transport table does not hold, before any verification
(`recv_dropped_no_peer`, as before #1756), and, since F70, a member's frame
from any address but the member's own binding (the peer-fields note above),
so a frame cannot move a binding and spends no counter; a peer's address is bound only when a pairing
completes (the address the partner paired from — re-pairing a device that
is already trusted re-binds it) and restored from NVS `peer_macs` at boot.
A changed radio MAC means a re-pair, with two limits. The pairing does not
authenticate the long-term key it binds (§11.1): the 6-digit code and the
CONFIRM hash cover only the ephemeral X25519 exchange, and the key is
taken as the DISCOVER or OFFER carried it, so an outsider relaying an
owner-run pairing from its own address gets matching codes on both
screens while choosing that key. Claiming a trusted member's key re-binds
the member to the outsider's radio and persists it (from an address no
other member holds: since F118 a re-pair from one another member holds
fails at the owner's confirm, and from F102 until then it was refused the
bind and nothing was persisted); claiming its own gets it trusted
(host-probed on the PlatformIO tree, the same before #1756; open, a wire
change). The code binds even the ephemeral exchange only weakly (§11.1):
a relay that swaps both ephemeral keys can grind its second one until the
codes match, and then reads the `opera_secret` in the COMPLETE
(host-probed on canary-wap; open). And on the PlatformIO tree, with eight members
bound the transport table has no slot for the new address, so the
pairing's replies cannot be sent and the re-pair cannot start until a
member leaves or is removed (host-probed). Learning a new address safely
needs either a wire change (the address signed into a payload, under the §4.5
registry) or a challenge the new address must answer with the peer's key;
that is an open decision, not built. **canary-wap conforms (2026-10-01),
with the source rule the PlatformIO tree took in F70:** its
`handle_received_message` drops a frame whose source is not the signer's
own bound address before the signature check, so it spends no counter and
reaches no handler. No signed opera frame writes an
address there any more; only a completed pairing does. Before, it
re-pointed a member's address, and
its ESP-NOW registration, at the source of any frame that passed `opera_id`,
signature and replay. A host probe against the real `mesh_network.cpp`
showed how far that reached there. A frame the receiver missed, re-sent
from an outsider's radio, moved the member there, and the receiver's frames
for the member went to the outsider until the member's next frame arrived
(its 30 s heartbeat, while it is `MESH_ACTIVE`). canary-wap's counters are
per destination and the envelope names none, so a frame the member sent to
*another* member did the same whenever the member's counter for that member
ran ahead (for example while the receiver sat at `PEER_UNKNOWN` in the
member's table, which broadcasts skip; the probe set that state directly).
The receiver then also took that counter as its last-seen, so the member's
own frames dropped as replays and the binding stayed on the outsider until
the member's counter climbed past it. After a power cut, frames heard since
the last 5-minute counter save did the same. And a copied member address
made the member's next real frame delete that other member's ESP-NOW
registration, so the receiver could no longer reach it. Like the
PlatformIO tree since F70 (the peer-fields note above), canary-wap accepts a
verified frame only from the signer's own address.
A changed radio MAC means a re-pair there too, with each member that holds
the device: one pairing re-binds one member's entry, where a frame used to
move it at every member at once. The member is then heard once its counter
for that member passes the last one heard; a member that rebooted to
change its address resumes its counters above every one it signed (§3.3,
F71), so its first frame after the re-pair is heard.
canary-wap keeps its identity key in the device's NVS, so a swapped module
comes back with a new key and joins as a new member. So does a device whose
NVS was erased or that was reflashed, but at its old radio address: since
F98 a member that still holds its old entry there refuses it until that
entry is removed (the refusal is logged as such, §5.2), and on canary-wap
each such removal splits that member from the opera until F48 (§5.6, F95;
host-tested). Before F98 the new key was added beside the old entry, at the
same address. What reaches a re-bind
is an NVS image moved to another board, or the relay below. Its `add_peer`
now re-binds a member it already holds to the address the pairing
completed from (new address registered first; an address another member
holds refused; counters, name and state kept), and logs the move. Since
F98 a new key at an address another member holds is refused too (one
address, one member), and a removal or a re-pair away from an address
drops its ESP-NOW registration only when no other member holds it (NVS an
older firmware wrote can hold two members at one address). It used
to append a second entry that no lookup reached, and a full opera refused
it. An entry an older firmware duplicated that way is folded into one at
boot, at the later pairing's address unless another member holds it, and
the fold is logged. Because a pairing binds addresses, canary-wap's
pairing handlers now keep the PlatformIO state machine's order: the joiner
takes the first `PAIR_OFFER` only, and `PAIR_COMPLETE` only after its owner
confirmed the code; the initiator takes one `PAIR_ACCEPT`, from the address
its OFFER went to, before it shows a code, and wipes the pairing once its
COMPLETE is sent. Before, the joiner took a COMPLETE as soon as it showed a
code, so whoever answered its DISCOVER first finished the pairing with no
owner on that side. Without the `opera_secret` that replaced the joiner's
opera; with it and a member's public key, once a re-pair re-binds, it
re-bound that member to its own radio (both host-probed; the second only
on the change's own re-pair step, since before it the joiner added an
unreachable duplicate). And the initiator kept a finished
pairing until the 2-minute timeout, so a radio that overheard its OFFER
could send its own ACCEPT and CONFIRM and have the `opera_secret` sealed to
it in a COMPLETE anyone in range can read (host-probed; it predates the
change). The pairing limit above applies too: canary-wap's code covers only
the ephemeral exchange, and a relay of an owner-run pairing, its codes
matching on both screens, that claims a member's key re-binds that member
to the relay's radio until another re-pair (host-probed on canary-wap
after this change; before it, the relay only added an unreachable
duplicate). Unlike the PlatformIO tree's eight-member limit, a full
canary-wap opera (16) still takes a re-pair, since ESP-NOW's list has 20
entries (host-tested through `add_peer`, not a full pairing). Its initiator
used to act on the joiner's CONFIRM only once its own owner had confirmed,
and nothing is sent twice, so a pairing whose joiner was confirmed first
timed out (host-probed); since F75 the owners confirm in either order, and
a CONFIRM counts only from the pairing partner's address (§5.2), and since
F100 the initiator sends its COMPLETE again until it hears the joiner
(§5.2). What remains, open: ESP-NOW does not authenticate a source,
so a radio copying a member's own address still delivers that member's
not-yet-heard frames — including ones sent to other members — and they are
dispatched and move the receiver's last-seen counter, silencing the member
until its counter catches up (no address moves; host-probed). A
destination in the signed bytes would stop the cross-member case (a wire
change). Host-tested
(`test_bound_peer_new_address_is_dropped_not_learned`,
`test_unheard_broadcast_replayed_from_a_new_address_moves_nothing`,
`test_unheard_rekey_offer_replayed_from_a_new_address_moves_nothing`,
`test_repair_moves_a_trusted_peers_address`; canary-wap:
`test_mesh_address_wap`, against the real `mesh_network.cpp` on host stubs,
and `test_mesh_rx_gates_wap`); not yet run on two radios
(U1 Track C2).

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
4. **Eavesdropping**: ChaCha20-Poly1305 seals the `opera_secret` a pairing
   or a rotation hands over. Partial: the PlatformIO tree's opera frames
   themselves are signed, not encrypted (§8.3), so anyone in range reads
   their payloads.
5. **Man-in-the-Middle**: Visual confirmation codes during pairing.
   Partial, on two counts. The code and the CONFIRM hash are derived from
   the ephemeral X25519 session key only, so they do not cover the
   long-term public key the DISCOVER or OFFER carries, which signs nothing
   in the exchange. A relay that swaps it can get its own key trusted, or
   re-bind an already-trusted member to its radio (§8.3; host-probed on the
   PlatformIO tree; on canary-wap, which derives the code the same way,
   host-probed for a relay claiming a member's key). And they bind the
   ephemeral exchange only weakly. A relay that swaps one ephemeral key
   shows different codes on the two screens, but the code has 10^6 values
   and nothing commits either side's ephemeral before the other side's is
   sent, so a relay that swaps both picks its second ephemeral after it has
   seen the first, and grinds it until the two codes match. It then opens
   the `PAIR_COMPLETE` and holds the `opera_secret`. Host-probed on
   canary-wap against its real pairing handlers (2026-10-02): about 1.5
   million X25519 tries, under 3 minutes on one host core, and the search
   splits across cores, against a 2-minute pairing timeout; the relay presented
   both devices' own long-term keys, so this is not the key substitution
   above. The PlatformIO tree derives its code the same way and its OFFER
   and ACCEPT come in the same order (read from code, not probed). Closing
   both needs a commitment to one side's ephemeral before it sees the
   other's (Bluetooth's numeric comparison commits to a nonce that way) or
   a much longer code, together with both long-term keys in the code and
   the CONFIRM hash, or a transcript signed with them: a wire change on
   both trees, open.
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
`replay_ctrs` (up to 16 × (8 B fingerprint + 8 B counter): the trusted
peers' counters and — v0.3 — the tombstones of dropped peers, §4.2),
`elected_hub` (8 B) and — v0.3 —
`opera_name` (up to 32 B), `peer_macs` (up to 8 × (8 B fingerprint +
6 B radio MAC), F33; one entry per fingerprint and, since F102, one
fingerprint per address; at boot an entry for a fingerprint that is not a
member, and both entries of an address two members' entries hold, are
dropped, F119/F120, §8.3) and `mesh_revoked` (the §5.6 deny-list: up to 8 ×
(8 B fingerprint + 4 B remaining ms, little-endian), F33), all behind the
flash-encryption gate (§5.5), plus
`mesh_enabled` (1 B), which is **not** gated: it is a preference, and gating
it would make "off" silently revert to "on" at every reboot of an FE-off
board — and `mesh_out_ctr` (u64, F33, §3.3), the outbound counter's
reserve-ahead high-water mark, not gated either: a count, not a secret, and
gating it would restart the counter at every reboot of an FE-off board. The `opera_id` is not stored; it is derived from the secret at boot.
canary-wap (NVS namespace `mesh`) stores the same deny-list blob under
`revoked` (F33), behind its flash-encryption gate, and — F71 — `tx_ctrs`
(up to 16 × (8 B fingerprint + u64)), each member's send-counter
reservation (§3.3; 0 for a member nothing was signed to yet on a device
that has reserved none, written with the member list too, so members
stored with no record mean NVS from an older firmware; since F99 a new
member is recorded as covered up to the highest counter the device can
have signed, a removal holds every survivor's reservation to that counter
before the rewrite, the record is not rewritten when no member is left, and
a boot reads it with no member loaded too), not gated, like the last-seen counters it keeps under
`replay_ctrs`: counts, not secrets, and a gate would restart the counters
at every boot of an FE-off board. Since F116 it also stores `rx_tombs` (up
to 8 × (8 B fingerprint + u64), oldest first), the last-seen counters of
members it dropped (§4.2), written at each removal or leave that changes
them and removed when none is left; not gated either, for the same reason.
Its `opera_id` and `opera_sec` keys are there only while it holds an opera
(F113, §5.4).

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
  not implemented; PIO replay tombstones — a dropped peer's counter survives
  its re-pair, and the leaver keeps its outbound counter (§4.2, §8.3, §12.3).
- v0.3, F33 (2026-09-23; maintainer crypto review and the U1 Track C2/C3
  bench passes pending — none of it has run on radios): pairing ephemerals
  are clamped X25519 keys in both trees (§5.3); the PIO transport peer table
  is filled from pairing and NVS `peer_macs` (§8.3, §12.3); the PIO outbound
  counter reserves ahead in `mesh_out_ctr` (§3.3, §12.3); the four PIO
  pairing routes run on the main loop's request slot (§8.3); the §5.6
  revocation deny-list in both trees (`mesh_revoked`, `revoked`, §12.3) and
  convergence of two concurrent removals in the PIO tree (§5.6); PIO
  `GET /api/mesh/alerts` adds `uptime_ms`, and the web UI shows an alert's
  age instead of a made-up time of day (§8.3); PIO `pair/start` founds an
  opera when the device holds none (§5.4, §8.3).
- v0.4 (2026-09-29; **implemented, awaiting maintainer crypto review; not
  bench-verified**): one wire type registry and one outer frame for both
  trees (§4.5, `mesh_wire.h`, staged and sync-guarded): the signed envelope
  is the frame, version byte 1 in both (canary-wap was 0), the type byte is
  the PIO numbering (`TAMPER_ALERT` 18, `LEAVE_OPERA` 25, rekey 26–29;
  canary-wap's `CHANNEL_LOCK`/`HUB_ELECTION` move from 20/21 to 23/24 and
  its session-layer types to 30–36), the pairing prefix is `8..12` in both
  (the PIO tree was `0..4`), and the PIO tree's unsigned type byte ahead of
  the envelope is gone. §4.1 states that neither tree encodes CBOR. The
  §4.3/§5.3/§8.3 interop notes now name the payloads and the pairing
  exchange, not the numbering, as what keeps the trees apart.
- v0.4 follow-up (2026-09-30; **host-tested only, not bench-verified,
  awaiting maintainer crypto review**): the security re-review of v0.4
  approved the registry and found three pre-existing canary-wap receive
  gaps, closed here — the replay counter convention is the PIO tree's
  (first counter 1, strict gate, no `rx == 0` exemption: a counter-0 frame
  had replayed indefinitely while the receiver's last-seen was 0), the
  source MAC is bound only after the signature (it was bound before it), and
  every struct-payload handler refuses any length but its struct's (a PIO
  6-byte `TAMPER_ALERT` had been read as the 56-byte struct, into the
  signature) — §3.3, §4.5's table below the registry,
  `test_mesh_rx_gates_wap.cpp`. And one fragility: the PIO envelope's
  version constant is `mesh_envelope::OPERA_VERSION` (was
  `PROTOCOL_VERSION`, which `canary_config.h` #defines as a string, so the
  envelope could not be included from the canary sketch);
  `test_mesh_wire.cpp` compiles both headers under that macro.
- v0.4 follow-up, F49 part 3 withdrawn (2026-10-01; **host-tested only,
  not bench-verified**): a verified opera frame no longer teaches the
  PlatformIO tree a peer's radio address. #1756 re-bound (and persisted) a
  trusted peer's MAC on a verified frame from an unbound address; the
  envelope signs no address and one counter serves every destination, so
  an outsider replaying a frame the receiver had not heard re-pointed the
  member at its own radio. Opera frames from unbound addresses drop again,
  a changed radio MAC means a re-pair, and the spec now says a verified
  frame MUST NOT bind an address (§8.3); canary-wap does not conform yet.
  The review of the withdrawal found two pre-existing limits, now stated
  rather than fixed: a pairing does not authenticate the long-term key it
  binds, so §11.1 items 4 and 5 are marked partial (item 4 because the
  PlatformIO frames are not encrypted); and the address a verified frame is
  recorded under can be a pairing partner's or a copied one (§8.3 peer
  fields).
- v0.4 follow-up, canary-wap conforms to §8.3 (2026-10-01; **host-tested only,
  not bench-verified**): canary-wap no longer re-points a member's address
  at the source of a verified frame. A frame from any address but the
  signer's bound one drops before verification, and a re-pair re-binds a
  member already held (it used to add an unreachable duplicate), with the
  move logged. A host probe showed the old re-bind reached further there
  than a missed frame, because canary-wap counts per destination. §3.3 now
  also says that a rebooted canary-wap sender is dropped as a replay until
  its counters climb back (open). Since a pairing binds addresses there,
  canary-wap's pairing handlers keep the PlatformIO order: the joiner takes
  the first OFFER and a COMPLETE only after its owner confirmed; the
  initiator takes one ACCEPT, from where its OFFER went, and wipes the
  pairing after COMPLETE. A duplicate entry an older firmware's re-pair
  saved is folded into one at boot.
- v0.4 follow-up, canary-wap liveness (2026-10-01; **host-tested only, not
  bench-verified**; sweep F71, F73–F76): a rebooted canary-wap's frames are
  heard at once — each member's send counter is reserved ahead in NVS
  `tx_ctrs`, a boot resumes every member above the highest reservation, and
  the first boot after the update, which finds no record, above 2^40
  (§3.3, §12.3); the owners confirm a pairing in either order, and a
  CONFIRM counts only from the pairing partner's address and only once the
  code is shown (§5.2, §8.3); a pairing whose partner the device cannot
  hold fails, sending and storing nothing, instead of reporting success
  (§5.2); the pairing DISCOVER registers the ESP-NOW broadcast peer itself,
  and a channel change re-adds it rather than deleting it; and an opera with
  members it has not heard sends its heartbeat while `MESH_CONNECTING` too,
  to every member whatever its state, at the same 30 s cadence (§7.1).
- v0.4 follow-up, canary-wap membership (2026-10-02; **host-tested only, not
  bench-verified**; sweep F95, F98, F99, F100): a removal's rotation reaches
  no member, because nothing opens an AUTH session and the exchange cannot
  complete as it stands (its response is a 262 B frame, and its keys would
  not agree; §3.1, §5.6; open, F48); a rotation keeps every counter, so the
  re-pair that rejoins a survivor, or a reboot before the last-seen save, is
  heard at once (§3.3); a new member starts one past the highest send
  counter the device can have signed, so a device re-paired after a removal
  or a leave hears it at once (§3.3, §12.3); a new key at an address another
  member holds is refused (§5.2, §8.3); and the initiator sends its
  `PAIR_COMPLETE` again until it hears the joiner, for at most the pairing
  timeout, while a joiner drops a COMPLETE it cannot open instead of ending
  its pairing (§5.2).
- v0.4 follow-up, PlatformIO pairing and sends (2026-10-02; **host-tested
  only, not bench-verified**; sweep F97, F101, F102): the PlatformIO owners
  confirm a pairing in either order, with canary-wap's F75 rules — a CONFIRM
  counts only from the partner's address and once the code is shown, the
  initiator keeps a joiner's early CONFIRM, and the joiner takes the COMPLETE
  once its own owner confirmed (§5.2; until F97 no order completed with
  frames delivered as sent); unlike canary-wap, every PlatformIO COMPLETE
  goes out with the initiator's CONFIRM in front of it, so a joiner on the
  older firmware completes too (§5.2); the opera senders unicast
  to the members' bound radio MACs only, and count only those (§8.3); and a
  pairing's address is persisted only once the session bound it, with
  `peer_macs` refusing an address another fingerprint holds (§8.3, §12.3).
- v0.4 follow-up, PlatformIO pairing refusals and stored addresses
  (2026-10-02; **host-tested only, not bench-verified**; sweep F117-F120):
  a PlatformIO device that cannot hold its pairing partner fails the
  pairing at its owner's confirm, and again before the initiator seals or
  the joiner opens the `opera_secret`, with nothing sent or stored (§5.2);
  an updated PlatformIO joiner re-sends its CONFIRM, at most three times,
  once it has read the initiator's, so an initiator on the pre-F97 firmware
  completes in both orders (§5.2); and the boot restore drops a `peer_macs`
  entry whose fingerprint is not a member and both entries of an address
  two members' entries hold, binding neither (§8.3, §12.3).
