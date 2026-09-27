# Fleet semantics — what "online", "verified" and "the mesh" mean, precisely

**Status:** descriptive, not aspirational. This page states what the
multi-device parts of SecuraCV do — membership, liveness, ordering, trust,
relay, scale — with the code behind each statement, and says **not built**
where a spec describes more than the tree contains. The specs keep their
maturity labels ([`spec/README.md`](../spec/README.md)); this page is the
index-level answer to "which of it runs?". The single-device fault story is
in [`failure_semantics.md`](failure_semantics.md) and, once it lands, the
companion page `FAULT_MODEL.md`; the roll-call wire contract is
[`tvos/discovery/DISCOVERY.md`](../tvos/discovery/DISCOVERY.md), whose §
"what the fleet contract says honestly" this page condenses and extends.

## 1. The short version

- There is **no fleet-wide state**. Four membership views exist, none is
  authoritative, and none reconciles with another (§2).
- **"Online" means different things on different surfaces.** The hub's
  roll-call claims a peer present only on a fresh, signed, chain-advancing
  publish; the display's glass credits any traffic, signed or not (§3).
- **Trust is trust-on-first-use, kept in three stores that never sync.**
  A signature proves who asserted a claim, never that the claim is true,
  and the broker remains the trust boundary (§4).
- **There is no cross-device ordering, clock, correlation or consensus** —
  by design for correlation (Invariant II), and simply not built for the
  rest (§5).
- **Relay is specified for Opera and Beacon and built for neither.** Chirp
  relays, with a soft-accept the code itself marks as a v0.2 limitation
  (§6).
- **Every scale figure is a cap or an estimate, not a measurement** (§7).

## 2. Membership: four views, no reconciliation

| View | Where | How an id gets in | How it leaves | Authenticated? |
|---|---|---|---|---|
| Hub roll-call (`GET /api/fleet`) | `src/fleet_peers.rs`, fed by `event_mqtt_bridge --fleet-peers-path` | any id seen on `securacv/+/{availability,status,health,chain,state,meta}` | at the cap (64), the never-proven then least-recently-heard id is displaced; an unpinned id is forgotten after 30 days; a pinned id never expires by the clock | the `chain` publish is signature-checked against a TOFU pin; everything else is not |
| Display glass | `canary-display/.../fleet_model.h` | any MQTT traffic, an mDNS `_securacv._tcp` advert, or a BLE/ESP-NOW/UDP beacon | `stale` after 180 s of silence, `lost` after 600 s (raises an alert) | no: "any traffic proves liveness", and the mDNS input is commented `UNAUTHENTICATED LAN input` |
| Device roster | `firmware/common/fleet_link/fleet_roster.h` | a beacon carrying a 16-bit fingerprint suffix | not heard for 120 s (16 entries) | no: the beacon has no signature, nonce or timestamp, and the header says "never a verified trust claim" |
| Opera (WAP household mesh) | `canary-wap/.../mesh_network.{h,cpp}` | pairing (pubkey pinned) | stale at 90 s, offline at 300 s, 16 peers | yes: per-message Ed25519 and a per-peer monotonic counter |

The hub never speaks MQTT itself: the kernel re-reads the bridge's summary
file on every request, and the file is written atomically. Two bridges on
one broker keep two files and can pin two different keys for one id; a Wall
that probes `canary.local:8099`, then `:8799`, "keeps the first that
answers", so a board answering first stands in for the kernel's whole fleet
(DISCOVERY.md). None of this is coordination; it is four independent
observers.

**Duplicate identity is a configuration hazard, not a detected fault.** The
default ids are shared across units (`canary_vision_001`,
`canary_sense_001`, marked "change per unit"). Two units with the default id
collide on the MQTT topics: the bridge pins the first key, the second key
that signs puts the id into a sticky `degraded` verdict that only deleting
the summary file clears, Home Assistant raises a key-mismatch notification,
and both units stop reading `online`. mDNS hostnames are salted against
this; topics are not.

## 3. Liveness: a failure detector with one fixed window

The hub's roll-call is the strictest surface. A peer is `online: true` only
when a **live** (not broker-retained) `chain` publish carried an Ed25519
signature that verified against the pinned key, **and** its chain length
advanced past the last one the bridge verified, **and** it arrived within
`FLEET_PEER_RECENT_SECS` (180 s), with no LWT `offline` since
(`fleet_peers.rs`). There is no suspicion level and no adaptive timeout:
one window, then a boolean. Because a Canary signs per sealed record and
not on a timer, a healthy but quiet Canary reads `online: false`, which the
contract defines as "not claimed present", never "claimed absent". A
retained `chain` publish can pin a key and set the chain verdict but never
advances presence; a retained `status: online` on the display, by contrast,
sets `Link::Online` on reconnect, so a dead device shows online there for up
to 180 s, then stale, then lost.

What the window does **not** give you: the chain canonical binds no nonce or
timestamp, so a broker publisher can replay a captured signed publish. The
length-must-advance rule bounds that to one window per chain advance the
bridge did not itself see. A liveness challenge (the firmware's `whoami`
canonical) exists on the device side and nothing drives it from the bridge.

## 4. Trust: TOFU, three stores, the broker as the boundary

- **Device keys.** Ed25519, generated at first boot, kept in NVS. `chain`,
  `events` and `counts` are signed; `health` (which carries the public key)
  and `status` are not (`device_trust.md`).
- **Pin stores.** Home Assistant (`device_trust.py`), the display's NVS (24
  pins, `trust.cpp`), and the bridge's summary file each pin on first sight
  and never exchange pins. Manual out-of-band pinning exists only in Home
  Assistant. Provisioning (`gen_hub_provision_bundle.py`) distributes no
  Canary keys and no trust anchors.
- **Rotation and revocation.** A re-flashed Canary surfaces as a key
  mismatch: a notification in Home Assistant, a sticky `degraded` in the
  bridge, a mismatched pin on the display. There is no revocation list;
  Home Assistant accepts a mismatched payload and marks it unverified
  ("warn-loudly-accept"); Opera keeps a 7-day deny-list; Beacon revocation
  is local to one device.
- **What the kernel verifies.** The kernel checks a Canary's signature only
  for the `chain` publish in the roll-call. Events ingested through the
  `mqtt_sensor` adapter are kernel-signed at ingest with attestation
  `adapter`; the device's own signature is not checked there (see
  `FAULT_MODEL.md` §2.3).
- **The honest upshot**, replacing an older sentence in `device_trust.md`:
  a hostile broker cannot *forge* a signed publish for a pinned Canary, but
  it can replay one (bounded as in §3), can invent ids and sign for them
  with its own key before anyone pins the real one, and can hold a real id
  in `degraded`. Signatures bound authorship. They do not move the trust
  boundary off the broker, and the code says so in its own words
  (`fleet_peers.rs`, module comment).

## 5. Ordering, correlation and consensus: none, and why

- **No cross-device ordering.** There are no Lamport, vector or hybrid
  logical clocks anywhere. The only per-source sequence is each device's
  chain length, and the event contract forbids sequence numbers that imply
  continuity across buckets (`spec/event_contract.md` §7). The hub's log
  is ordered by hub append order and stamped with the hub's receive-time
  bucket; a device's own bucket is uptime-based and not read.
- **No cross-device correlation — by design.** Correlation tokens are
  per-process, per-bucket random keys, zeroized on rotation, emitted only by
  the in-process camera pipeline, and the contract requires them to be
  incomparable across devices (`spec/event_contract.md` §"correlation";
  Invariant II). Adapters set none. Two Canaries mapped to one zone in one
  hub bucket **collapse into one sealed event** through the adapter host's
  `(adapter, kind, zone, hint)` dedup — lossy suppression, not fusion.
- **No consensus.** `FailureType::SensorDisagreement` is never emitted
  because no multi-sensor consensus layer exists (`failure_semantics.md`).
  Sentinel fuses channels inside one device; `core_multilink_fusion`'s peer
  ingest and the federated-baseline sharing have no production caller
  (`docs/csi_modules.md`, the 2026-07 firmware audit).
- **Hub election (WAP CSI channel coordination)** picks the lowest
  fingerprint among the peers *this* node currently sees in `CONNECTED`,
  `STALE` or `ALERT` state, with no term or epoch (`csi_integration.cpp`).
  A partition therefore yields one coordinator per side, and the two issue
  conflicting `CHANNEL_LOCK` frames until the partition heals. The
  PlatformIO tree persists whatever `HUB_ELECTED` fingerprint a peer
  announces; its header says "peers verify the election is deterministic",
  and no code performs that check (`csi_modules_integration.cpp`,
  `mesh_session.h`). The feature matrix's checkmark for this row means
  "the frames are sent and acted on", not "the election is safe under
  partition".

## 6. Relay: specified twice, built once

| Channel | Spec says | Code does |
|---|---|---|
| Opera (`spec/canary_mesh_network_v0.md` §2.3, §6.1) | "Messages are relayed by other opera members (max 3 hops)"; "Fully Connected Mesh" | **no relay code** in `mesh_network.cpp`; only the ESP-NOW transport is built (the spec's own v0.2 status note says the WiFi-AP and BLE transports are not), so "fully connected" is one radio hop, and an alert reaches exactly the peers in range |
| Beacon (`spec/beacon_channel_v0.md` §2 table, §8) | "3 hops, ~750 m"; hop limit 3; relay rate limit 5/min | `MAX_HOP_COUNT` and `MAX_RELAYS_PER_MINUTE` are defined in `beacon_wire.h` and **never read**; `hop_count` is set to 0 on origination and copied into the audit entry on receipt; **nothing relays** |
| Chirp (`spec/chirp_channel_v0.md`) | 3 hops, relay after 2 confirming ACKs | built: relays after ACKs from distinct session keys, 10/min, Bloom-deduplicated on the nonce |

Two Chirp facts a reader of the spec would not guess from it, both stated
in the code: a relayed frame whose origin signature is zero is
**soft-accepted** on the relayer's signature alone ("v0.2 limitation",
`chirp_channel.cpp`), so at hop ≥ 1 the claimed origin key is
unauthenticated; and the spec's "Sybil resistance" (a presence seen for ten
minutes before an ACK counts) is not implemented — the ACK check tests only
that the session id is in the nearby table, and `NearbyDevice` has no
first-seen field. Beacon's receive path has one more: when the receiver's
clock is unsynced the freshness branch is empty ("accept but flag" —
nothing is flagged), so a captured frame can be re-accepted after the
32-slot dedup ring rolls or the device reboots — and a reboot also zeroes
the per-key and per-pair 24 h budgets (`init()` clears the rate tables;
`load_audit_log()` restores the audit entries, not the budgets), so the
budget does not bound the replay across reboots. Auto-revoke on a peer's
tamper report (`AGENTS.md`, Beacon rule 11)
is unreachable from a WAP peer today because `broadcast_tamper_alert` has
no caller.

None of the above is hidden by the specs' maturity labels — all three are
🟡 draft — but "draft" says the wire format may change, not "the relay is
missing", and this table says the second.

## 7. Scale: caps and estimates, no measurements

| Component | Bound | Kind |
|---|---|---|
| Hub roll-call | 64 ids (`FLEET_PEER_MAX`) | cap |
| Display pins | 24 (`trust.cpp`) | cap |
| Device roster | 16 (`fleet_roster.h`) | cap |
| Opera | 16 peers; 8 trusted peers in the PlatformIO tree | cap |
| Multilink fusion | 7 peers | cap (dormant) |
| Beacon set | 32 keys | cap |
| Chirp nearby cache | 32, pruned every 3 min, no eviction when full | cap |
| Airtime per device | governor's estimate of framing and fan-out | estimate, per device, no cross-device coordination |

No benchmark covers fleet, MQTT or broker behavior (`BENCHMARKS.md`, "what
this does not cover"), and the feature matrix itself marks multi-device
discovery "needs verification of scalability". `network_coexistence.md`
used to answer "can I put eight Canaries on one network?" with a
percentage; it now answers with the per-device cap and says the aggregate
has not been measured.

## 8. What is not claimed

- No fleet-wide membership, no authoritative roster, no reconciliation
  between the four views.
- No liveness proof: `online` is "claimed present within one window",
  replayable for one window per missed chain advance.
- No key distribution, no revocation list, no pairing-grade pins outside
  Opera.
- No cross-device ordering, correlation, fusion or consensus; no
  partition-safe election.
- No relay on Opera or Beacon; no Sybil resistance on Chirp.
- No multi-node, partition, split-brain or two-hub test. The fixtures in
  `tvos/witness-core/tests/fixtures/fleet_contract_vectors.json` pin the
  wire shape and parse semantics of one observer; `fleet_peers.rs`'s unit
  tests pin replay, second-key, cap-eviction and clock-jump behavior of one
  bridge. Opera's F33 fixes "have not run on radios" (its spec §12).
- No measured scale figure.

## 9. Where each row is pinned

| Claim | Test or gate |
|---|---|
| Roll-call: replay bounded to one window; second signing key is sticky; cap eviction order; clock-jump pass skipped; retained never advances presence | `src/fleet_peers.rs` unit tests |
| Wire shape of `GET /api/fleet`; silence is not absence | `src/api/mod.rs` fleet tests; `fleet_contract_vectors.json` (replayed by the Wall's `parseFleet`) |
| Display deadlines and beacon model | `test_fleet_beacon_model`, `test_espnow_peer`, `test_beacon_parse` (display host tests) |
| Device roster expiry | `firmware/common/fleet_link` host tests |
| Opera per-peer counter, pairing | `securacv_mesh` host tests; WAP mesh tests (source-text pins, not linked code, for the Beacon receive path — `beacon_source_scan.h` says so) |
| Break-glass quorum counts distinct trustee keys, binds bucket and ruleset, replay ledger durable | `src/break_glass/core.rs` tests, `consumed_break_glass_tokens` tests in `src/lib.rs` |
| No relay on Opera/Beacon, no election verification | **no test; established by reading the code** (this page's §5–6 cite the lines) |
