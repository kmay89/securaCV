# bitchat protocol review — what to borrow for Beacon/Chirp alerts

Status: Working brief, 2026-06-10
Purpose: Review the bitchat BLE mesh messenger (github.com/permissionlesstech/bitchat,
public domain / Unlicense) and identify implementation ideas worth borrowing for our
harm-reduction **alert** channels (Beacon, Chirp). bitchat is a chat application; we
are not building chat. The interesting parts are its transport, flooding, dedup,
privacy, and resilience mechanics — all of which transfer to template-only alerts.

Companion docs: `spec/beacon_channel_v0.md`, `spec/chirp_channel_v0.md`,
`docs/research/harm_reduction_prior_art.md`, `docs/mesh_esp_now_evaluation.md`
(roadmap items G3/G4).

## 1. What bitchat is

bitchat (initiated by Jack Dorsey, maintained by Permissionless Technologies) is a
serverless messenger with two transports:

1. **Bluetooth LE mesh** — multi-hop relay (TTL up to 7 hops), no internet, every
   peer is a relay. Devices act as BLE central and peripheral simultaneously.
2. **Nostr** — internet fallback over ~290 public relays, NIP-17 gift-wrapped DMs,
   geohash-scoped location channels with ephemeral per-geohash identities.

Identity is keypair-only (no accounts, no phone numbers): a Curve25519 Noise static
key plus an Ed25519 signing key, fingerprinted as `SHA-256(static_pubkey)` for
out-of-band verification. Mesh payloads are end-to-end encrypted with
`Noise_XX_25519_ChaChaPoly_SHA256`. Implementation is Swift (iOS/macOS), with an
Android sibling project.

### 1.1 Protocol mechanics relevant to us

| Mechanism | bitchat design |
|---|---|
| Packet format | 13-byte fixed header (version, type, TTL, u64 timestamp, flags, payload length) + 8-byte sender ID + optional 8-byte recipient ID (`0xFF…FF` = broadcast) + payload + optional Ed25519 signature |
| Propagation | TTL-bounded flooding; each peer decrements TTL, rebroadcasts to all peers except the one it received from; processes-but-does-not-forward at TTL 0 |
| Dedup | Time-windowed Bloom filter on packet IDs (`OptimizedBloomFilter`); no false negatives, occasional false-positive drops absorbed by gossip redundancy |
| Traffic analysis resistance | All packets PKCS#7-padded to the next standard block size (256/512/1024/2048 B) so observers cannot infer message type or length; timing randomization on transmissions |
| Relay confidentiality | Relays forward opaque Noise ciphertext; only the endpoint can decrypt |
| Reliability | `DeliveryAck` / `ReadReceipt` packets + sender-side retry service that re-transmits when no ack arrives in a window; store-and-forward caching for temporarily offline peers |
| Fragmentation | `fragmentStart` / `fragmentContinue` / `fragmentEnd` message types for payloads exceeding BLE MTU |
| DoS resistance | `NoiseRateLimiter` throttles repeated handshake attempts per peer |
| Battery | Adaptive battery modes — BLE scan/advertise duty cycle scales with battery level and charge state |
| Panic affordance | Triple-tap emergency wipe of all local state |
| Compression | LZ4 on payloads |

## 2. Where we already match or exceed bitchat

Several bitchat mechanisms validate decisions we already made independently — no
action needed, but worth recording as convergent evolution:

- **Bloom-filter dedup**: Chirp v0.2 (audit C9) and Beacon §8 already use Bloom
  dedup on nonces. bitchat's "no false negatives, gossip absorbs false positives"
  rationale is the same one our audit used.
- **Hop-bounded flooding**: our 3-hop cap is *tighter* than bitchat's 7 — correct
  for us, since neighborhood scope is a harm-reduction feature (alerts should not
  propagate city-wide).
- **Ephemeral identity**: Chirp's per-session Ed25519 keys are stronger than
  bitchat's persistent mesh identity. bitchat only goes ephemeral for geohash
  channels; Chirp is ephemeral always.
- **Keypair-only identity, no accounts**: identical philosophy.
- **Rate limiting per peer**: Chirp v0.2 C14 and Beacon's per-pubkey budgets cover
  what `NoiseRateLimiter` covers.
- **Where we exceed bitchat**: template-only payloads (no free-text abuse surface),
  two-pubkey co-signing, NFPA-72 supervised health, CAP alignment, witness
  confirmation thresholds, suppress voting. bitchat has no equivalent of any of
  these — they are chat-irrelevant but alert-essential.

## 3. What to borrow

Ordered by value-for-effort. Each item names the spec section it would amend.

### B1. Fixed-size frame padding (traffic-analysis resistance)

bitchat pads every packet to a standard block size so an RF observer cannot infer
message type or content from ciphertext length. Our frames are currently
size-distinguishable: a `CHIRP_PRESENCE` (~60 B), a `CHIRP_WITNESS` (~110 B), a
relayed witness with `signed_origin` (~206 B), an ACK, and a suppress vote all have
different lengths, and Beacon alert (217 B) vs selftest (~100 B) differ too. A
passive observer who cannot break Ed25519 can still learn *"a witness alert was
just originated near me"* — and template categories with distinctive detail slots
may leak through length variation as templates evolve.

**Proposal:** pad all Chirp and Beacon frames to a single fixed size (e.g. 248 B,
just under the ESP-NOW 250 B ceiling) with random fill + 2-byte true-length
trailer. Cost: a few hundred extra bytes of airtime per frame — well inside the
airtime governor budget for channels that emit a handful of frames per day. This
makes a chirp indistinguishable from a presence beacon on the air, which directly
strengthens the §6.4 plausible-deniability guarantee.

Amends: `spec/chirp_channel_v0.md` §3.1, `spec/beacon_channel_v0.md` §5.

### B2. Relay timing jitter (originator anonymity)

bitchat randomizes transmission timing to resist traffic analysis. Our flooding
relays currently re-broadcast as fast as the loop allows, which means an observer
with two receivers can identify the **origin device** of a chirp by who transmitted
first (and by RSSI). For Chirp — whose entire identity model exists so that
chirps cannot be attributed — first-transmitter timing is the most practical
deanonymization attack, and it defeats the ephemeral-key firewall entirely.

**Proposal:** add a uniform random relay delay (e.g. 50–500 ms) before any hop ≥ 1
re-broadcast, and a small random pre-send delay (e.g. 0–200 ms) at hop 0 measured
from the human hold-to-send release. Beacon relays should use a *shorter* jitter
window (e.g. 20–100 ms) since latency matters more there and Beacon identity is
persistent anyway — jitter there is for collision avoidance, not anonymity.

Amends: `spec/chirp_channel_v0.md` §5, `spec/beacon_channel_v0.md` §8 (relay row).

### B3. Late-joiner delivery: store-and-forward + active-alarm re-announce

bitchat caches messages for temporarily offline peers and retries until a
`DeliveryAck` arrives. The alert-shaped version of this problem: **a device that
boots, rejoins, or was out of range while an alarm went out never learns about
it.** A smoke detector that misses the fire because it was rebooting fails its
operating bar. Currently a Beacon alert is transmitted once at origination; a
receiver that joins two minutes later sees nothing until `expires`, yet the alarm
is still active.

**Proposal (Beacon):** while a device holds an active `Alarm` (or `Trouble`-worthy
`Cancel`) whose `expires` is in the future, it re-broadcasts the *original
dual-signed frame, byte-identical* every `BEACON_REANNOUNCE_INTERVAL_MS` (suggest
60 s ± jitter, routed through `try_reserve_routine()` so re-announces never starve
urgent traffic). Receivers that don't yet hold the alarm accept it through the
normal §7.1 pipeline (the dual signatures still verify — no new trust surface).
Cancel frames are re-announced on the same schedule until the original alarm's
`expires`, preserving the "cancellation as prominent as the alert" invariant for
late joiners too.

Two existing §7.1 pipeline rules need explicit carve-outs for byte-identical
re-announces, because both stop working once the alarm outlives them:

- **Dedup beyond the Bloom window.** The Bloom filter only remembers 5 minutes
  of nonces (§8), but alarms persist until `expires`. A re-announce arriving
  after eviction would pass step 3 and be fully reprocessed — duplicate audit
  log entries, repeated `PATTERN_BEACON` audio, repeated UI triggers. Receivers
  must therefore *also* dedup against the active-alarm table (RAM, keyed by
  nonce): a frame whose nonce matches a held active alarm is treated as a
  liveness refresh — update a `last_reannounce_seen` timestamp, write nothing
  to the audit log, re-trigger nothing.
- **Freshness exemption.** Step 4 drops frames whose `effective` is outside
  `now ± 300 s`. A byte-identical re-announce carries the *original*
  origination time, so after 5 minutes every re-announce would be rejected as
  stale — silently defeating the whole mechanism. The freshness rule must be
  amended to also accept frames where `effective ≤ now < expires` (the alarm
  is simply still active); replay of long-dead alerts stays blocked because
  `expires` has passed.

**Proposal (Chirp):** same pattern, but only for validated safety-class chirps,
re-announced by *relays that hold them* at most once per 60 s until `ttl_minutes`
elapses. This formalizes the store-and-forward item already on the Chirp roadmap.

Amends: `spec/beacon_channel_v0.md` §7/§8, `spec/chirp_channel_v0.md` §5.

### B4. The BLE fallback blueprint (roadmap G4) — and phone reach

bitchat is the most complete open-source reference implementation of exactly what
our unbuilt BLE fallback (`spec/canary_mesh_network_v0.md` §2.2 transport 3,
roadmap G4) needs: simultaneous central+peripheral roles, a single GATT service
with a characteristic for mesh frames, MTU-aware fragmentation
(start/continue/end), connection management under iOS/Android background limits,
and adaptive duty cycling. Being Unlicense/public domain, we can lift designs (and
code, if ever useful) without license friction.

Two distinct wins here:

1. **Device-to-device resilience:** when ESP-NOW is jammed or the 2.4 GHz channel
   is saturated, Beacon frames (217 B, fits in a fragmented GATT write, or in 2–3
   extended advertisements) can fall back to BLE. Beacon `Trouble` already fires
   on airtime saturation; BLE fallback turns that Trouble state into degraded
   operation instead of an outage.
2. **Reach without hardware:** the harm-reduction payoff. Today an alert reaches
   only people who own a Canary. A phone app (or PWA + Web Bluetooth) that speaks
   the Beacon BLE framing in **receive-only** mode would let anyone in the
   building hear "fire or smoke visible" or "shelter in place" with zero hardware.
   Receive-only is the right v0 scope: phones get life-safety reach, but
   origination stays gated on paired, co-signing, supervised devices — so the
   abuse-prevention stack is untouched.

bitchat's specific lessons to take: their fragmentation triple maps cleanly onto
frames that exceed BLE's practical MTU; their adaptive battery modes (scan/
advertise duty cycle scaling with battery state) matter for any battery-powered
Canary variant; and their experience is that 7-hop BLE flooding works on commodity
phones — our 3-hop budget is comfortably inside that envelope.

Amends: `docs/mesh_esp_now_evaluation.md` G4; new spec `beacon_ble_transport_v0.md`
when scheduled.

### B5. Noise-style discipline for the co-sign channel

`BEACON_COSIGN_REQUEST`/`RESPONSE` (Beacon §6.3) currently specifies "fresh
ChaCha20-Poly1305 session key via X25519 ECDH" for Beacon-paired-only devices.
That is an ad-hoc construction; bitchat uses the formally analyzed
`Noise_XX_25519_ChaChaPoly_SHA256` pattern, which adds mutual authentication,
forward secrecy, and a specified nonce + sliding-window replay regime for free.
The co-sign channel is the single most security-critical link in Beacon — it is
exactly where a MITM would substitute a different canonical to trick a cosigner
into signing the wrong alert.

**Proposal:** specify the co-sign channel as Noise **KK** (both static pubkeys
already known from the beacon set — one round trip, mutual auth, forward secrecy)
rather than XX, using the same `25519_ChaChaPoly_SHA256` suite. Adopt bitchat's
incrementing-nonce + sliding-window replay rule verbatim. This is a small spec
change with an outsized assurance gain, and embedded Noise implementations exist
for ESP32.

Amends: `spec/beacon_channel_v0.md` §6.3.

### B6. Fragmentation reserve (CAP gateway future-proofing)

Beacon alert frames (217 B) sit close to the ESP-NOW 250 B ceiling. The deferred
CAP gateway (`spec/beacon_cap_gateway_v0.md`) will want to carry things a fixed
canonical can't (gateway XML-DSig evidence, multi-area scopes). Rather than invent
fragmentation later under pressure, reserve bitchat's three-type scheme now:
`BEACON_MSG_FRAG_START` / `FRAG_CONT` / `FRAG_END` message-type codes, with
reassembly bounded (suggest ≤4 fragments, ≤1 s reassembly window, drop on gap).
Costs three enum values today; saves a wire-format break later.

One caveat bitchat's connection-oriented GATT transport doesn't face: ESP-NOW
broadcasts have no MAC-layer acks or retries, so naive drop-on-gap reassembly
compounds loss fast (at 10% per-frame loss, a 4-fragment message fails ~34% of
the time). The eventual implementation should pair the fragment types with
simple erasure coding (e.g. one XOR parity fragment tolerating any single loss)
or carousel retransmission of the fragment set — a decision for the CAP-gateway
implementation, not for the enum reservation.

Amends: `spec/beacon_channel_v0.md` §5.3 enum reservation only — no v0
implementation.

## 4. What NOT to borrow (and why)

| bitchat feature | Why we reject it |
|---|---|
| Free-text messaging | The whole point of our template-only design. Free text is the abuse surface (profiling, panic, harassment) that templates structurally eliminate. Alerts, not chat. |
| Internet/Nostr fallback | Violates local-ownership invariant. Our alerts must work — and *only* work — within physical radio range of the community they concern. An internet bridge is also a deanonymization and subpoena surface. The sanctioned interop path remains the authorized CAP gateway, not public relays. |
| 7-hop TTL | Neighborhood scope is deliberate. 3 hops ≈ 750 m is a building/block, which is the unit of mutual aid. |
| Social layer (nicknames, petnames, verification UI, mentions) | Beacon's paired device names cover the legitimate need. Anything more invites the engagement dynamics (Ring/Nextdoor culture) the philosophy sections explicitly reject. |
| Triple-tap panic wipe | Right instinct, wrong layer for us. Chirp already retains nothing (ephemeral keys, no history). For Beacon, wiping the beacon set would silently destroy smoke-detector reliability for the *whole building* — a panic affordance that degrades neighbors' life-safety is a footgun. Device seizure is already handled by flash encryption + the witness kernel's break-glass model. |
| LZ4 compression | Our frames are ≤250 B packed structs; compression adds code-size and a decompression-bomb surface for zero practical gain. |
| Delivery/read receipts per recipient | Per-recipient acks on a broadcast alert channel invert the privacy model (they enumerate who is listening). Our witness-confirmation ACKs serve the alert-shaped version of this need. |

## 5. Suggested sequencing

1. **B2 relay jitter** — small firmware change, closes the most practical Chirp
   deanonymization attack. Do first.
2. **B1 fixed-size padding** — wire-format change; bundle with the next
   `PROTOCOL_VERSION` / `BEACON_PROTOCOL_VERSION` bump.
3. **B3 active-alarm re-announce** — firmware + spec; biggest reliability win for
   Beacon's smoke-detector operating bar.
4. **B5 Noise KK co-sign channel** — spec now, implement with the next Beacon
   firmware iteration.
5. **B6 fragmentation enum reservation** — one-line spec edit, do with B1's bump.
6. **B4 BLE fallback + receive-only phone reach** — largest effort, largest
   harm-reduction payoff; track as the G4 roadmap item with bitchat as the named
   reference implementation.

## 6. Sources

- bitchat repository: https://github.com/permissionlesstech/bitchat (Unlicense)
- bitchat technical whitepaper: `WHITEPAPER.md` in the repository (packet format,
  TTL/flooding rules, Bloom dedup, Noise XX layering, PKCS#7-style padding,
  fragmentation, delivery acks, rate limiting)
- Noise Protocol Framework: https://noiseprotocol.org (XX and KK handshake
  patterns, `25519_ChaChaPoly_SHA256` suite)
