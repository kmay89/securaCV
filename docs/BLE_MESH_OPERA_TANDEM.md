# BLE Mesh + Opera Mesh Tandem — Design

**Status:** v1 design, scaffolding committed in `firmware/projects/canary-wap/arduino/canary_wap/ble_mesh.{h,cpp}`. Transport implementation pending review of options A/B/C below.

## Why two mesh layers

Opera and BLE Mesh solve different problems and want different transports.

| Concern | Opera mesh | BLE Mesh |
|---|---|---|
| Role | Witness **data plane** | Household **control plane** |
| Transport | WiFi mesh (existing) | BLE radio |
| Reach | Up to whole-network (LAN) | ~10–30 m per hop, household-scoped |
| Payload | Full Ed25519-signed witness records, hash chain replication, evidence | Compact identity / state updates: bond IRKs, chain heads, presence beacons |
| Throughput need | 10s–100s of KB / event | <100 B / event |
| Latency tolerance | Seconds OK | Sub-second wanted (UX: phone walks in → nearest Canary recognizes immediately) |
| Trust model | Each device has its own per-household keypair, mutual-auth on join | Symmetric household key (NetKey) shared across canaries — control-plane only |

Trying to put control-plane updates on Opera works, but:
- Opera requires WiFi association — a freshly-flashed Canary in pairing mode can't receive any IRK sync until it joins WiFi.
- BLE Mesh works peer-to-peer over BLE in the absence of WiFi infrastructure.
- The latency budget for "phone X paired on Canary A, recognized by Canary B before user notices" is ~1 s. WiFi mesh hops with 5-second propagation are too slow.

Conversely, putting full witness records on BLE Mesh wastes radio time — they're signed evidence, not control state, and rely on the WAP's WiFi reach.

## What syncs over BLE Mesh

Three message types. Each ≤ 16-byte encrypted payload over a household-scoped NetKey + AppKey.

### 1. `MSG_CHAIN_HEAD_HEARTBEAT` (every ~30 s per Canary)

```c
struct __attribute__((packed)) ChainHeadHeartbeat {
  uint32_t sender_id;       // last 4 bytes of pubkey fingerprint
  uint32_t chain_height;
  uint8_t  chain_head[8];   // truncated SHA-256 of latest record
};  // 16 bytes
```

Lets every Canary in the household see whether peers are caught up. If A sees B at height 102 but A is at 104, A can offer to sync via Opera.

### 2. `MSG_BOND_ADVERTISE` (on new pairing, then refreshed every ~5 min)

```c
struct __attribute__((packed)) BondAdvertise {
  uint32_t sender_id;
  uint8_t  irk[16];         // peer's Identity Resolving Key
  // (no name — name is sensitive; user enters once on the canary they paired)
};  // 20 bytes — needs one fragment if BLE Mesh segmentation is on,
    // or one extended-adv frame.
```

When user pairs phone X on Canary A, A broadcasts X's IRK over the household NetKey. Canary B receives, drops it into its `household` IRK store (Phase 4 module — `household.h::add_irk_for_household()`). Next time X comes within range of B, B sees the RPA and resolves it via the household IRK without prompting the user a second time. This is the "pair once per household" UX.

### 3. `MSG_FAMILIAR_BLOOM_DELTA` (every ~1 h, when local Bloom changed)

```c
struct __attribute__((packed)) FamiliarBloomDelta {
  uint32_t sender_id;
  uint8_t  bloom_delta[12]; // sub-bits of the familiar Bloom filter
};  // 16 bytes
```

The federated mesh module (Phase 9) already does Bloom-share aggregation over Opera. Mirroring lightweight deltas over BLE Mesh lets two Canaries on the same household but different VLANs stay synced even when WiFi is partitioned.

## Crypto

- **Household NetKey (16 B)** + **AppKey (16 B)** — derived once during multi-device onboarding (when user adds a second Canary to an existing household).
- Each message: AES-CCM with the AppKey, AAD = `(sender_id, msg_type, seq)`.
- Replay protection: 32-bit `seq` per sender, household-wide. Out-of-order tolerated up to ±64 (small replay window).
- The household key never leaves the device cleartext; it lives in NVS, encrypted by the device's secure-element-backed key (where available).

## Transport options

Three viable paths, each with their own footprint:

### Option A — formal ESP-BLE-MESH (Bluetooth SIG standard)

- Pro: standards-compliant, interop with Mesh Provisioning, tooling exists.
- Con: requires `CONFIG_BLE_MESH=y` + `CONFIG_BLE_MESH_USE_NIMBLE=y` in sdkconfig — these aren't set in the stock Arduino-ESP32 BLE build, so this is a switch from the bundled NimBLE library to the IDF NimBLE+Mesh build. Roughly 80 KB extra flash.
- Effort: 2–3 days. Provisioning, NetKey/AppKey distribution, custom Vendor Model.

### Option B — BLE 5 extended advertising as transport

- Pro: stays on the same NimBLE library we just bumped (PR #327), no sdkconfig change. Reuses the BLE radio we already have running for pairing.
- Con: not technically "BLE Mesh." Each device just advertises an encrypted blob; peers passively scan. No explicit forwarding (so devices > 1 hop apart need a relay), no provisioning ceremony.
- Effort: 1–2 days. Custom encrypted ad payload, periodic broadcast scheduler, scan callback.

### Option C — Opera-as-transport for control plane

- Pro: zero additional radio config, leverages the work already in Phase 9 (`federated::*`).
- Con: requires WiFi association, breaks the "works without infrastructure" property, doesn't help in the BLE-only freshly-paired case.
- Effort: 0.5 day. Just routes the three message types over Opera.

## Recommendation

Start with **Option B** to ship household sync without a stack swap, and revisit Option A only if standards compliance becomes a requirement. Option C as a secondary fallback for cases where two canaries can see each other via WiFi but not BLE.

## Non-goals (deliberately deferred)

- General BLE Mesh provisioning of arbitrary third-party Mesh devices. We're a closed household.
- Lighting / sensor models from the BLE Mesh spec. Not our use case.
- Cross-household mesh. Each household runs its own NetKey; no inter-household messaging.
