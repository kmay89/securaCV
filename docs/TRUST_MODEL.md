# Trust model — who may talk to whom, and on what evidence

**Scope:** every networked Canary (`firmware/canary`, canary-wap, canary-vision,
canary-sense, canary-sentinel, canary-display), the hub (Home Assistant plus
the kernel), and the Witness Wall (the Apple TV app). Written in the firmware
parity program from what the code does (`FIRMWARE_MAP.md` §7 is the
file-by-file evidence); where a thing is designed but not yet on a radio, this
page says so. The per-target cells are in [`PARITY.md`](PARITY.md).

Vocabulary ([`GLOSSARY.md`](GLOSSARY.md)): **Verified** means an Ed25519
signature checked against a **pinned** key, nothing looser. **Presence** is
unsigned ("I am here"). **Paired** is defined in §3 below and nowhere else.

---

## 1. Identities

| Who | Identity | Where the private half lives | Who pins the public half |
|---|---|---|---|
| A witnessing Canary (canary, WAP, vision, sense, sentinel) | one Ed25519 keypair, generated on first boot from the hardware RNG; fingerprint = `SHA256("securacv:pubkey:fingerprint" ‖ pubkey)[0:8]` | plaintext NVS, namespace `securacv`, key `privkey` (Tier 0 of `common/identity/key_at_rest.h`; every shipped build reports `creds_at_rest: "plaintext-nvs"` in `GET /api/status`) | Home Assistant (TOFU from the first `health` publish, or a manual pin), the display's `cvtrust` store, the bridge's `fleet_peers.json` — three stores that never exchange pins |
| A display | none — it signs nothing; it verifies peers' `chain` publishes against its own TOFU pins | — | — |
| The hub | the Mosquitto `canary` login (one shared credential the installer mints), the kernel's viewer token | the Pi | nobody: a node trusts the broker it was pointed at |
| The Witness Wall | none | — | it validates the mDNS `host` it dials is private, and expires sources after 30 days |

The identity key is the device's birth certificate. That is why the 10-second
factory reset (`common/io/reset_hold.h`, `onboarding_fsm.h`) clears credentials
and settings and **keeps the key**: a reset node rejoins as the same device
every pin knows; the hold sheds the key only at the opt-in 30-second stage the
PIO canary alone enables, with a serial warning first. An install in either
flasher sheds it too, on every Canary that keeps one: its factory image starts
the settings region over, so the next boot makes a new key.

## 2. What is signed, what is not

**Signed (Verified):** the `chain`, `events`, `counts`, `sense` and `sentinel`
MQTT publishes, over the locked canonical `securacv-canary-sig|v1|<kind>|…`
(`common/identity/device_signature.h`; HA's `signature.py` and the display's
`trust.cpp` rebuild the same bytes). Every OTA image and manifest
(`OTA.md`).

**Unsigned (Presence):** `health` (which carries the public key the pins come
from — hence TOFU), `status`, `availability`, `state`, every MQTT command
topic, the fleet-link beacon on BLE / ESP-NOW / UDP, the `_securacv._tcp`
advert, `GET /api/fleet` and `GET /api/status`.

**Authenticated with a shared key (Paired, §3):** the peer protocol's
heartbeats and events (`common/network/peer_protocol.h`).

## 3. Pairing — the household fleet key

The program brief asks that only paired nodes and the Wall accept each other.
Below the WAP's Opera mesh (which has never paired on a radio and does not
interoperate with the PIO canary's), nothing in the tree authenticated a peer.
The peer protocol does, with one shared key per household:

```
fleet_key = HKDF-SHA256(salt = "securacv:fleet-key:v1",
                        ikm  = wifi_ssid ‖ 0x00 ‖ wifi_password,
                        info = "peer")                      (common/network/fleet_key.h)
```

- **What "paired" means.** Every node that was handed the household Wi-Fi by
  its owner derives the same 32 bytes and tags every frame with
  `HMAC-SHA256(fleet_key, frame)[0:16]`. A stranger in radio range, a guest on
  the guest network, a neighbor's Canary — none can forge a heartbeat or an
  event, and none is admitted to a node's peer table. Pairing therefore
  happens in the onboarding step the owner already performs, with no second
  ceremony; it is exactly as strong as the Wi-Fi password and no stronger.
- **An open network has no secret**, so no key is derived and the peer link
  stays **off** with one logged warning and `peer: "off"` in `/api/status`.
  The protocol never runs unauthenticated — that is the brief's "explicit
  degradation" and `PARITY.md` marks such a node ⚠️. The same holds when the
  key's HMAC fails (on the device, an allocation failure inside mbedTLS): no
  key is derived rather than an all-zero one, and a frame whose tag the HMAC
  could not compute is neither sent nor accepted (sweep F350's review).
- **Rotation.** A node honors a replacement key (32 random bytes) stored
  under NVS `fleet_key`: a stored key wins over the derived one, and a key
  change empties the peer table (a new key is a new fleet). What this program
  ships is the firmware half only. The hub half is deliberately **not** a
  service: the HA integration's rule is that services never mutate trust
  (`docs/device_trust.md`, "Why pin, rotate and unpin are not actions"), so
  handing out a fleet key belongs beside pinning in its human-only options
  flow, and that flow is future work (`PARITY.md` marks hub-driven rotation
  ⚠️). Until then a household rotates the way it always could: change the
  Wi-Fi password, and every node re-derives on its next join.
- **Replay.** Each frame carries the sender's boot-relative uptime and a
  per-boot sequence number; a receiver accepts a frame only when the sequence
  advances, or when the sender's uptime fell (it rebooted). A captured
  heartbeat cannot be replayed later, and a captured event cannot be
  re-injected.
- **What the key does not do.** It does not identify a node (the fingerprint
  in the frame is self-asserted, like the beacon's), does not encrypt (every
  payload is the ObjectClass vocabulary and coarse state, by Invariant II/III
  there is nothing in it worth hiding), and does not bind to the witness
  identity key. A node that leaves the household (sold, moved) keeps deriving
  the old key until it is reset or re-provisioned; the household rotates.

## 4. Coordination is not trust

The hub-less coordinator (`peer_protocol.h`: oldest uptime wins, tie → lowest
fingerprint; any node on its hub makes the hub the coordinator for everyone)
governs only what the hub otherwise would: who answers the roll-call for the
fleet and who drives a staged OTA (`OTA.md`). It never grants trust: a
coordinator cannot make a node accept a key, a pin, an image or a command it
would not accept from anyone else. Election is deterministic from the shared
table, so there is no round of messages to attack — a node that lies about
its uptime can only make itself the coordinator, which gives it nothing.

## 5. What an attacker can do (and what this program changed)

| Attacker | Before this program | Now |
|---|---|---|
| **In radio range, no credentials** | spoof the presence beacon for any fingerprint suffix; appear in displays' fleet models | the same (presence is unsigned by design) — but cannot speak the peer protocol, so is never a peer, never coordinates, never carries an event |
| **On the LAN, no credentials** (a guest device) | read every `/api/fleet`; POST `/api/ota` on a display; rebind an uncredentialed display's broker by mDNS referral | read `/api/fleet` and `/api/status` (both coarse by construction: no MAC, no key, no broker address, no coordinates); the display routes unchanged (see `OTA.md` for the signed-upload rule); still cannot forge a peer frame |
| **Knows the Wi-Fi password** (a household member, an ex-member, a leaked password) | everything above | can derive the fleet key: forge heartbeats and events, become coordinator. The remedy is the household's: change the Wi-Fi password (every node re-derives on its next join) or rotate from the hub |
| **Holds the broker login** (sniffed off plain 1883, or an insider) | pre-pin a key for any device id (TOFU), replay a signed publish within a window, invent ids, trigger `install` / `identify` / mic commands, bounded by the release key and version floor | the same. This is the largest remaining gap: per-device broker credentials and ACLs are not in this program (the hub's installer mints one shared login); the fleet key is not reachable from the broker, so a broker-only attacker still cannot forge a peer frame |
| **Holds the OTA release key** | ship any image to any node | the same — the key is offline and never in the repo (`OTA.md`) |

Things that were true before and remain true: a node never refuses to boot
over its key's storage tier (`key_at_rest.h` rule 1); no MAC, stable id or
timestamp finer than the ten-minute bucket crosses the wire in any of the
above (Invariants II/III); the first-boot 30-minute Improv window and the
BOOT-tap minute are the only times a node takes Wi-Fi from a phone.

## 6. Where each rule is enforced and tested

| Rule | Code | Test |
|---|---|---|
| fleet key derivation, open-network refusal, tag | `common/network/fleet_key.h` | `firmware/tests_host/test_fleet_key.cpp` (RFC 4231 / 5869 vectors) |
| frame tag, replay window, table, election, hub hand-off | `common/network/peer_protocol.h` | `test_peer_protocol.cpp` |
| a failed HMAC derives no key, sends no frame and accepts none | `common/network/fleet_key.h`, `peer_protocol.h` | `test_fleet_key_hmac_fail.cpp`, built over the mbedTLS and OpenSSL branches |
| socket, arming, disarming on an open network | `common/network/peer_link.cpp` | compiled on sense / vision / sentinel; bench-verified: no (PARITY.md E2E plan) |
| reset keeps the identity | `common/io/reset_hold.h` + each board's reset glue | `test_reset_hold.cpp`; the key list each board clears is in its `main.cpp` |
| signatures, canonical bytes | `common/identity/device_signature.{h,cpp}` | `test_device_signature.cpp`, HA `tests/test_signature.py` |
| OTA signatures and floors | `common/ota/src/securacv_ota.cpp` | `test_ota_logic.cpp`, `test_ota_release.py` |
