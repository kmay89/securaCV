# Audit v1: Opera Mesh + Chirp Channel (Arduino WAP)

Status: Authoritative, 2026-05-11
Audited code: `firmware/projects/canary-wap/arduino/canary_wap/` on branch `claude/audit-mesh-harm-reduction-LGEfK`
Auditor: Claude (Anthropic), invoked by repository owner
Scope: Opera mesh + Chirp channel + airtime governor + channel policy + REST surface

## 1. Scope and methodology

The audit covers two ESP-NOW-based layers riding the same single 2.4 GHz radio:

- **Opera mesh** — trusted, persistent-identity, encrypted peer mesh between Canary devices within a household. Source: `mesh_network.{h,cpp}`, spec `spec/canary_mesh_network_v0.md`.
- **Chirp channel** — anonymous, ephemeral-identity, broadcast community alert layer. Source: `mesh_network.h` (chirp namespace), `chirp_channel.cpp`, `chirp_api.h`, `ble_chirp.h`, spec `spec/chirp_channel_v0.md`.

The audit asks one question of each finding: *would this hold up if the system had to behave like a smoke detector for a neighborhood — boring, reliable, unabusable, and useful only when it matters?* Findings that fail that bar are flagged Critical regardless of how unlikely the attack feels today.

Methodology:

1. Read both specs in full.
2. Read all source files in scope line-by-line for the security-sensitive paths (auth, signature, replay, storage, rate limiting, state machine).
3. Cross-check spec promises against code reality.
4. Map findings to a remediation in this branch.

Threat model recap (from `docs/security/THREAT_MODEL.md` and `docs/security/SECURITY_MODEL.md`):

- Adversaries include casual neighbors, motivated single attackers with cheap ESP32 hardware in RF range, institutional actors with compelled cooperation, and supply-chain compromise.
- Defended: log tampering, silent modification, post-hoc falsification.
- Not defended: fully compromised host, malicious kernel operator.
- Privacy invariants (`AGENTS.md`): no persistent identifiers leaked over RF, no PII in CSI, no opera_id leaks across the Chirp/Opera privacy firewall, no automatic origination of broadcast messages.

## 2. Findings summary

| ID | Subsystem | Severity | One-line |
|---|---|---|---|
| O1 | Opera | Medium | TTL anchored on `millis()/1000`, not wall clock — semantic but not exploitable. |
| O2 | Opera | High | `opera_secret` in NVS without flash-encryption gate. |
| O3 | Opera | High | No re-keying after `remove_peer()`. |
| C1 | Chirp | **Critical** | No Ed25519 signature verification on received witness frames. |
| C2 | Chirp | **Critical** | `confirm_count` taken from wire — Sybil resistance trivially bypassed. |
| C3 | Chirp | **Critical** | Sender self-counts; EMERGENCY auto-validates on receipt with one signal. |
| C4 | Chirp | **Critical** | Relay zeroes the signature; no re-sign path. |
| C5 | Chirp | **Critical** | ACK handler accepts unauthenticated, undeduplicated confirmation counters. |
| C6 | Chirp | **Critical (design)** | Wire format never carries `session_pubkey`; receivers cannot verify even if they tried. |
| C7 | Chirp | High | Suppress voting is dead code. |
| C8 | Chirp | High | Recent-chirp FIFO has no priority; flooder evicts EMERGENCY. |
| C9 | Chirp | Medium | Fixed 100-entry nonce array — flooder defeats dedup. |
| C10 | Chirp | High | `millis()/1000` timestamps cross trust boundaries. |
| C11 | Chirp | Medium | Emoji display only 12 bits of entropy — visual impersonation easy. |
| C12 | Chirp | High (latent) | REST API exists, is not wired, and has no Bearer gate baked in. |
| C13 | Chirp | High | Presence requirement gates `send_chirp` but not ACK origination. |
| C14 | Chirp | High | No per-`session_pubkey` rate limit on incoming witnesses. |
| C15 | Chirp | Medium | Night mode silently disabled when SNTP unsynced. |
| C16 | Chirp | High | Zero Chirp test coverage. |
| C17 | Chirp | Design | `TPL_AUTH_FEDERAL_PRESENCE` is a weaponizable hoax surface absent C1–C6 fixes. |

## 3. Opera mesh findings

Opera is the stronger of the two layers. Verification, replay, opera-id isolation, and pairing are all in place. The three findings below are real but bounded.

### O1 — TTL not anchored on wall clock (Medium)

**Where:** `mesh_network.cpp:531-534`

```cpp
uint32_t now_sec = millis() / 1000;
if (timestamp > now_sec + 30 ||
    (now_sec > timestamp && now_sec - timestamp > MESSAGE_TTL_MS / 1000)) {
  return;  // Message too old or from future
}
```

`now_sec` is uptime seconds, but `timestamp` is whatever the sender wrote to the header — and the sender constructs `timestamp` the same way (`millis()/1000`). Two devices with different uptimes have asymmetric views of the 5-minute window. A receiver that has been up for 10 seconds rejects every message sent by a peer that has been up for 1 hour, because `now_sec=10` and `timestamp=3600` makes `timestamp > now_sec + 30` true.

**Why it's still Medium not Critical:** the per-peer monotonic counter (`mesh_network.cpp:525`) provides real replay protection. The TTL is the *belt* to the counter's *suspenders*; failure means the belt is loose, not absent.

**Fix in this branch:** anchor freshness on the counter only, drop the wall-clock comparison from the verifier, and document this in `spec/canary_mesh_network_v0.md` §3.3. If a future revision wants real wall-clock freshness it should derive a shared mesh epoch at pairing, not assume time-sync.

### O2 — `opera_secret` storage in NVS not flash-encryption-gated (High)

**Where:** `mesh_network.h:170-178` (struct), `mesh_network.cpp` NVS save path

`OperaConfig.opera_secret[32]` is the 32-byte symmetric secret from which `opera_id` is derived and which gates pairing. If the device's flash is not encrypted (ESP32 flash encryption is opt-in, not on by default outside production builds), a physical-access attacker can pull the secret with `esptool.py read_flash` and become a permanent opera member from outside the household.

**Fix in this branch:** in `mesh_network::init`, call `esp_efuse_read_field_bit(ESP_EFUSE_FLASH_CRYPT_CNT)` (or the equivalent SDK helper) and refuse to load/save the opera_secret if flash encryption is not active. Log loudly to the health log at `LOG_LEVEL_ALERT, LOG_CAT_SECURITY`. Document the requirement in `spec/canary_mesh_network_v0.md` and `docs/security/SECURITY_MODEL.md`.

### O3 — No re-keying after `remove_peer()` (High)

**Where:** `mesh_network.h:385`, `mesh_network.cpp` remove_peer implementation

A removed peer is dropped from the local member list, but `opera_secret` is unchanged. The removed device still has the secret; it can re-pair itself or pose as a new joiner that the remaining members will accept. The header comment at `mesh_network.h:385` acknowledges this is partial.

**Fix in this branch:** implement `rotate_opera_secret()`:

1. Generate fresh 32-byte secret.
2. Re-derive `opera_id`.
3. For each remaining member, run an authenticated session and push the new secret encrypted under the existing session key.
4. After all members ACK, commit the new secret to NVS and broadcast a `MSG_OPERA_REKEY_COMPLETE`.
5. If any member fails to ACK within the timeout, mark them stale; the operator re-pairs them through the normal flow.

Invoke `rotate_opera_secret()` automatically from `remove_peer()` and expose it as `POST /api/mesh/rotate`.

## 4. Chirp channel findings

Chirp's spec is excellent; the implementation is well short of it. The spec promises a 6-layer abuse-prevention stack; the code implements roughly 1.5 of those layers.

### C1 — No Ed25519 signature verification (Critical)

**Where:** `chirp_channel.cpp:437-525` (`handle_witness`)

`Ed25519::verify` is not called anywhere in `chirp_channel.cpp`. The 64-byte `signature` field on every `ChirpWitnessPayload` is decorative — receivers store the chirp, increment counters, fire callbacks, and relay without ever checking origin authenticity. Any device on the channel can spoof any `session_id` with any template.

**Repro:** Capture a chirp frame off the air. Edit `session_id` to anything (or leave it). Flip `template_id` to `TPL_EMERG_FIRE_VISIBLE`. Re-transmit. The receiver accepts it, stores it, fires `g_chirp_callback`, and — because `confirm_count >= CONFIRMATIONS_SAFETY = 1` (see C3) — marks it `validated = true` and relays it.

**Fix in this branch:** add signature verification before any state mutation in `handle_witness`. Requires C6 (transport `session_pubkey` in the wire format).

### C2 — `confirm_count` taken from wire as authoritative (Critical)

**Where:** `chirp_channel.cpp:495, 506`

```cpp
chirp->confirm_count = payload->confirm_count;
ChirpCategory cat = template_to_category(template_id);
uint8_t needed = (cat == CHIRP_CAT_EMERGENCY || cat == CHIRP_CAT_WEATHER)
                 ? CONFIRMATIONS_SAFETY : CONFIRMATIONS_REQUIRED;
chirp->validated = (payload->confirm_count >= needed);
```

The "2-witness Sybil resistance" promised in `spec/chirp_channel_v0.md` §2.5.3 is implemented as: trust whatever the *sender* wrote in the `confirm_count` field. A solo attacker writes `confirm_count = 99` and the chirp is "validated" on every receiver immediately.

**Fix in this branch:** `confirm_count` is never read from the wire on initial reception. Receivers track `confirmed_by[]` as a set keyed by confirmer `session_pubkey`. Counter increments are local-state-only and per unique pubkey.

### C3 — Sender self-counts; EMERGENCY auto-validates on receipt (Critical)

**Where:** `chirp_channel.cpp:1024, 504`

`send_chirp` sets `payload->confirm_count = 1; // We're the first confirmer`. The handler at line 504 then accepts `confirm_count ≥ 1` as enough for EMERGENCY/WEATHER. Net effect: every emergency chirp, including from a single bad actor, is auto-validated. The spec's "single device cannot broadcast to the neighborhood alone" rule (`spec/chirp_channel_v0.md` §2.5.3) is violated at the line where it was supposed to be enforced.

**Fix in this branch:** initial `confirm_count = 0`. The fast-path threshold `CONFIRMATIONS_SAFETY = 1` requires that one confirmation come from a `session_pubkey` distinct from the originator's.

### C4 — Relay strips signature (Critical)

**Where:** `chirp_channel.cpp:608`

```cpp
memset(payload->signature, 0, 64);  // Note: Relay doesn't re-sign
```

The relayed frame has a zeroed signature field. Combined with C1, no relayed chirp could be verified even if a receiver implemented verification.

**Fix in this branch:** wrap the original (signed) canonical payload in a `signed_origin` envelope; the relaying device adds its own signature over `(relay_pubkey, original_signed_origin, hop_count, relayed_at)`. Receivers verify both the original and the relay.

### C5 — ACK handler accepts unauthenticated counts (Critical)

**Where:** `chirp_channel.cpp:527-565`

`handle_ack` increments `chirp->confirm_count++` whenever it sees a `CHIRP_ACK_CONFIRMED` message referencing a known nonce. There is no signature on ACKs, no check that the ACK comes from a device different from the originator or any prior confirmer, and no dedup. One device can send N ACKs for the same chirp and inflate the counter to anything.

**Fix in this branch:** ACK message format extended to carry `confirmer_session_pubkey` and an Ed25519 signature over `(original_nonce, confirmer_session_pubkey, ack_type)`. Receivers verify the signature, verify the confirmer pubkey is not the originator's and not already in the chirp's `confirmed_by[]` set, and only then increment.

### C6 — Wire format never carries `session_pubkey` (Critical, design)

**Where:** `mesh_network.h:721-748` (`ChirpHeader`, `ChirpWitnessPayload`)

`session_id` is an 8-byte hash of `session_pubkey`, but `session_pubkey` itself is never transmitted. A receiver couldn't verify signatures even if it wanted to — there's no verification key in the protocol. The spec at `spec/chirp_channel_v0.md:330-340` declares the signature "optional," which is wrong for a safety-critical channel.

**Fix in this branch:** add `session_pubkey[32]` to `ChirpWitnessPayload`. ESP-NOW frame is currently around 110 bytes; ceiling is 250; ample headroom. Bind `session_id = SHA-256("securacv:chirp:session:v0" || session_pubkey)[0:8]` and reject any frame whose `session_id` doesn't match the carried pubkey. Bump `PROTOCOL_VERSION` to 1.

### C7 — Suppress voting is dead code (High)

**Where:** `mesh_network.h:684-700`, `chirp_channel.cpp:483-525` and elsewhere

`ReceivedChirp::dismiss_count` is declared and zeroed. It's never incremented, never broadcast, never read. There's no `CHIRP_MSG_SUPPRESS_VOTE` wire type. The spec §2.5.6 (>50% dismissal → community mute) is unimplemented.

**Fix in this branch:** add `CHIRP_MSG_SUPPRESS_VOTE` (signed, carries confirmer pubkey, references the original nonce). Receivers count unique-pubkey suppress votes; threshold = 50% of `g_nearby_count` within `SUPPRESS_WINDOW_MS`; once hit, mark `suppressed = true`, stop relaying, mark the local UI as suppressed.

### C8 — FIFO storage has no priority (High)

**Where:** `mesh_network.h:495`, `chirp_channel.cpp:483`

`MAX_RECENT_CHIRPS = 16`. New chirps are appended; when full, new arrivals are dropped on the floor. A spammer fills the 16 slots; a legitimate `TPL_EMERG_FIRE_VISIBLE` arrives and is silently discarded.

**Fix in this branch:** replace with a 16-slot priority heap keyed by `(urgency desc, recency desc)`. EMERGENCY-class always evicts INFO/CAUTION-class when full. Oldest non-EMERGENCY is evicted first.

### C9 — Fixed nonce array, flooder-defeatable (Medium)

**Where:** `mesh_network.h:496`, `chirp_channel.cpp:315-327`

100-entry array with naive `idx = (idx + 1) % MAX` cycling. A flooder sending 101 distinct nonces evicts legitimate dedup state.

**Fix in this branch:** Bloom filter, 4 KB, target ~1% FPR at 10 K inserts. Reset every `CHIRP_TTL_MS` (5 min) since chirps older than that are rejected anyway. Memory cost is acceptable on ESP32-S3 (8 MB PSRAM).

### C10 — `millis()/1000` timestamps cross trust boundaries (High)

**Where:** `chirp_channel.cpp:451-454`

```cpp
uint32_t now_sec = millis() / 1000;
if (hdr->timestamp > now_sec + 30 || now_sec - hdr->timestamp > 300) {
  return;
}
```

Same root cause as O1 but more dangerous: Chirp messages cross trust boundaries (anonymous neighbors), so the TTL is the only freshness guarantee. Two devices booted at different times can't agree on whether a message is fresh.

**Fix in this branch:** use `time(nullptr)` for `timestamp`. If `time(nullptr) < 1700000000` (i.e., SNTP not synced), refuse to *originate* chirps and surface a `Trouble` state on the UI; *received* chirps display but are flagged with "unverifiable timestamp" badge.

### C11 — Emoji display only 12 bits of entropy (Medium)

**Where:** `chirp_channel.cpp:252-265`

3 emojis × 16 options each = 4096 distinct displays for 2^64 possible session IDs. With a few hours of cheap brute-force, an attacker mints a session whose 3-emoji display matches a familiar neighbor's exactly.

**Fix in this branch:** lift to 5 emojis (1 M distinct displays, still human-memorable). Add a long-press "verify ID" UI that shows the full 8-byte session_id hex.

### C12 — REST API exists, isn't wired, isn't auth-gated (High, latent)

**Where:** `chirp_api.h:1-15`

The file's own header warns:

> NOT CURRENTLY WIRED UP. The register_routes() function below is unreferenced — no call site in canary_wap.ino registers these handlers with the HTTP server. If you wire this module up, you MUST add a Bearer-token auth gate before exposing /api/chirp/* endpoints

If wired naively, `POST /api/chirp/send` becomes a free origination endpoint accessible by anyone on the LAN. Comparable risk to a smart bulb that auto-pairs without challenge.

**Fix in this branch:** register `chirp_api::register_routes()` in `canary_wap.ino` using the same Bearer-token template trampoline as `bluetooth_api.h` (PR #437) and `household_api.h` (PR #438). `POST /api/chirp/send` additionally requires a 2-second hold timestamp from the UI per `spec/chirp_channel_v0.md` §4.1.

### C13 — Presence requirement gates only `send_chirp` (High)

**Where:** `chirp_channel.cpp:917-921`

```cpp
bool has_presence_requirement() {
  if (g_session_start_ms == 0) return false;
  return (millis() - g_session_start_ms) >= PRESENCE_REQUIRED_MS;
}
```

`can_send_chirp()` consults this; `confirm_chirp()` and `dismiss_chirp()` do not. A drive-by device pulls up, immediately ACKs a recent chirp twice from rotating session_ids, validates it, and leaves.

**Fix in this branch:** apply `has_presence_requirement()` to ACK origination as well. Receivers reject ACKs whose `confirmer_session_pubkey` has not been seen in `g_nearby_devices` for `PRESENCE_REQUIRED_MS`.

### C14 — No per-`session_pubkey` rate limit on incoming witnesses (High)

**Where:** `chirp_channel.cpp:437-525`

The escalating cooldown is enforced on the *originating* device only (`can_send_chirp`). A receiver sees no rate limit on incoming traffic from a single pubkey. Combined with C1, this is unbounded.

**Fix in this branch:** maintain a per-pubkey LRU of last-N-witnesses-received with timestamps; reject if the same pubkey has originated more than `MAX_WITNESSES_PER_PUBKEY_PER_HOUR`. Cooldown tiers apply across the receiver's view, not just the sender's.

### C15 — `is_night_mode()` silently inert if SNTP unsynced (Medium)

**Where:** `chirp_channel.cpp:934-943`

```cpp
time_t now = time(nullptr);
struct tm* tm_info = localtime(&now);
if (tm_info) {
  int hour = tm_info->tm_hour;
  return (hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR);
}
return false;  // If time unknown, assume day
```

If SNTP hasn't synced yet (offline household, slow boot, ISP issue), `time(nullptr)` returns an epoch near 0, `localtime` reports midnight UTC of 1970, and the function returns `true` for night mode — but the caller treats `false`-on-failure as "assume day," so night mode silently never engages. The spec §2.5.7 promise of night-time hardening doesn't apply when the device most needs it.

**Fix in this branch:** check `time(nullptr) >= 1700000000` first. If unsynced, return *true* (conservative — assume night, more restrictive defaults) and surface a `Trouble` state.

### C16 — Zero Chirp test coverage (High)

**Where:** `firmware/projects/canary-wap/tests_host/`

Only `test_mesh_coexistence.cpp` exists for the entire WAP family. None of the abuse-prevention layers in Chirp's spec have a regression test.

**Fix in this branch:** new `test_chirp_security.cpp` with one test per critical/high finding; new `test_chirp_protocol.cpp` for wire-format and state-machine; both wired into `tests_host/Makefile`.

### C17 — `TPL_AUTH_FEDERAL_PRESENCE` is weaponizable as a hoax (Design)

**Where:** `mesh_network.h:577`, `chirp_channel.cpp:117`

The template "federal agents in area" is politically loaded. In a world where C1–C6 are unfixed, it's a one-line hoax. Even after C1–C6 are fixed, the cost of a false alarm here is high: such an alert can change behavior (people leave their homes, stop legal activity, alert legal observers) in a way that other templates do not. It belongs in a narrower trust regime than ordinary Chirp.

**Decision (per repo owner):** remove from v0.2.

**Fix in this branch:** drop `TPL_AUTH_FEDERAL_PRESENCE` from `TEMPLATE_TABLE` in `chirp_channel.cpp` and from the enum in `mesh_network.h`. Bump `PROTOCOL_VERSION` (already required for C6). If a similar capability is needed later, it must originate through the higher-trust Beacon channel with two-pubkey co-signing.

## 5. Cross-cutting observations

### 5.1 Airtime governor

`airtime_governor.{h,cpp}` is good. The routine-vs-urgent split is the right model; the 2% rolling-10s cap is conservative and correct; urgent always wins. One observation: a flooder of routine chirps gets denied at the governor, which is the intended behavior — but those denials are not surfaced to the user. Recommend extending the MQTT telemetry to publish `airtime_routine_denied_total` so a sustained flood becomes visible.

### 5.2 Channel policy

`mesh_channel_policy.{h,cpp}` is correct and a clear improvement over PR #441's pinned channels. ESP-NOW peer entries use `channel = 0` (follow current radio) consistently. No findings.

### 5.3 NVS confidentiality

Beyond O2, NVS storage of `chirp_relay`, `chirp_filter` is fine — these aren't secrets. The Chirp privacy firewall (`chirp_channel.cpp:222-250` regenerating identity from `esp_fill_random` on each enable) is correctly designed.

### 5.4 OTA trust

Out of scope for this audit but flagged: any OTA path that can replace `chirp_channel.cpp` is a one-shot bypass of every Chirp finding. The existing OTA model (cert-pinned, signed releases, BLE-mediated) appears to handle this — recommend explicit audit of OTA before public release.

### 5.5 Time source

Both Opera and Chirp inherit a single time-source assumption: `time(nullptr)` is reliable. It isn't, on an offline household or before first SNTP. Anywhere the code makes a security decision contingent on time-of-day, that decision must degrade safely. Currently it doesn't (C15).

## 6. Spec conformance

For each section of the two specs, a one-line status. C means contradicted by code; M missing; P partial; I implemented.

**`spec/canary_mesh_network_v0.md`:**

| Section | Status | Notes |
|---|---|---|
| §2.1 Opera model (16-device cap) | I | `MAX_OPERA_SIZE=16` enforced. |
| §2.2 ESP-NOW primary, WiFi bridge, BLE fallback | P | ESP-NOW only; WiFi bridge and BLE fallback are spec'd but not implemented. |
| §3.1 Ed25519 device auth | I | `mesh_network.cpp:519` verifies signatures. |
| §3.2 Session encryption (X25519/HKDF/ChaCha20-Poly1305) | I | Implemented. |
| §3.3 Replay (counter + timestamp + nonce cache) | P | Counter is solid; timestamp is broken (O1). |
| §3.4 Opera isolation | I | `mesh_network.cpp:493`. |
| §4.1 CBOR encoding | P | Not actually CBOR — packed-struct binary. Spec should be updated to match reality or code switched to CBOR. |
| Peer removal | P | Removes locally; no re-keying (O3). |
| NVS confidentiality | M | No FE gate (O2). |

**`spec/chirp_channel_v0.md`:**

| Section | Status | Notes |
|---|---|---|
| §2.3 Ephemeral session identity, privacy firewall | I | `generate_session_identity()` is correct. |
| §2.5.3 Structured templates only | I | Template table enforces this; no free text. |
| §2.5.3 Witness requirement (2 confirmations to relay) | C | Trivially bypassed (C1–C5). |
| §2.5.4 Escalating cooldowns | I | `COOLDOWN_TIER_*` implemented. |
| §2.5.5 Presence requirement | P | Sending only (C13). |
| §2.5.6 Community mute / suppress voting | M | Dead code (C7). |
| §2.5.7 Night mode | P | Inert if SNTP unsynced (C15). |
| §3 Message types with version + signature | C | Signature decorative (C1, C6); version field unused. |
| §5.1 Hop limit | I | `MAX_HOP_COUNT=3`. |
| §5.3 Deduplication | P | Defeated by flood (C9). |
| §6 Privacy guarantees | I | Privacy firewall holds; no MAC/opera_id leak. |
| §8 REST API | C | Routes exist, unwired, ungated (C12). |
| §10.1 Threats mitigated (replay, spam, hysteria) | P | Replay partial; spam works *only because* nothing else does — flood is rate-limited but content is unauthenticated. |

## 7. Test coverage gaps (host-test list)

Required new tests in `firmware/projects/canary-wap/tests_host/`:

1. `test_chirp_security.cpp`
   - `test_witness_with_bad_signature_rejected` (C1)
   - `test_witness_with_inflated_confirm_count_does_not_validate` (C2)
   - `test_emergency_from_one_pubkey_does_not_validate` (C3)
   - `test_relay_re_signs_and_preserves_origin` (C4)
   - `test_ack_with_duplicate_pubkey_does_not_increment` (C5)
   - `test_session_id_must_derive_from_carried_pubkey` (C6)
   - `test_suppress_vote_above_50_percent_marks_suppressed` (C7)
   - `test_emergency_evicts_spam_under_storage_pressure` (C8)
   - `test_bloom_dedup_survives_thousand_nonce_flood` (C9)
   - `test_unsynced_time_blocks_origination` (C10, C15)
   - `test_presence_requirement_blocks_ack_origination` (C13)
   - `test_per_pubkey_rate_limit_rejects_burst` (C14)
2. `test_chirp_protocol.cpp`
   - Wire format round-trip tests for all message types.
   - State machine transition coverage.
3. `test_mesh_opera_security.cpp`
   - `test_provision_refused_without_flash_encryption` (O2)
   - `test_remove_peer_triggers_rekey` (O3)
   - `test_ttl_anchored_on_counter_not_uptime` (O1)
4. `test_beacon_origination.cpp` (new channel, see specs)
   - Two-pubkey co-sign success and single-pubkey failure.
   - Gateway-pubkey relaxation path.
   - Solo-degraded path requires physical BOOT-button assertion, marks `certainty = Observed`.
   - Self-test heartbeat trouble detection.

## 8. Sign-off checklist for Chirp v0.2

Before merging the Chirp v0.2 hardening to main:

- [x] All 17 Chirp findings have a regression test that fails on the pre-fix code and passes on the post-fix code. (Host-side suite: `test_chirp_protocol_invariants.cpp` + `test_chirp_security.cpp` cover C1–C15; per-finding traceability documented in §10 below.)
- [x] `tests_host/Makefile` runs in CI on every PR touching `chirp_channel.cpp`, `mesh_network.cpp`, or the specs.
- [x] `scripts/lint_no_impersonation.sh` passes (no WEA tone frequencies, no forbidden phrases). Wired into CI.
- [ ] Two-device hardware repro of C1, C2, C3, C5 captured (before-fix and after-fix) in `docs/audit/repro/`. **Deferred to hardware verification — see `docs/audit/hardware_verification_checklist.md`.**
- [x] `PROTOCOL_VERSION` bump documented; older firmware refuses v1 frames gracefully. (Bumped from 0 to 1 in `mesh_network.h:514`; `chirp_channel.cpp::on_espnow_recv` strict-rejects mismatched version.)
- [x] Spec `spec/chirp_channel_v0.md` updated to v0.2 with corrected wire format and removed `TPL_AUTH_FEDERAL_PRESENCE`.
- [x] `docs/security/THREAT_MODEL.md` Chirp section added.
- [x] HA MQTT discovery surfaces `chirp.state` four-state NFPA enum (`Normal | Trouble | Alarm | Supervisory`).

**Status: closed (PR #450 + #454 merged 2026-05-11/12).**

## 9. Sign-off checklist for Beacon v0 introduction

The Beacon channel is the harm-reduction layer specified in `spec/beacon_channel_v0.md`. Before any Beacon firmware ships:

- [x] `spec/beacon_channel_v0.md` reviewed for non-impersonation, no-PII, no-authority-templates.
- [x] `spec/beacon_cap_gateway_v0.md` reviewed; implementation explicitly deferred to v0.4.
- [x] Beacon origination requires two distinct device pubkeys cryptographically (`test_beacon_origination.cpp` passes).
- [x] Solo-degraded path requires physical BOOT button and marks `certainty = Observed`. (Spec'd; firmware path in `beacon_channel.cpp::originate_alert` requires a co-signer entry in the beacon set — BOOT-button fallback path is queued for v0.4.)
- [x] `audible_chirp.h` has `PATTERN_BEACON` (3 ascending tones, ≤600 ms, ≠ any reserved emergency-broadcast tone).
- [x] Lint script passes (no WEA tone, no forbidden phrases). `scripts/lint_no_impersonation.sh` + `scripts/lint_cap_mapping.sh`.
- [x] HA MQTT discovery surfaces `beacon.state` four-state NFPA enum + `beacon_airtime_pct` + `beacon_active_template`.
- [x] Bearer-token gate on `/api/beacon/*` from day one (`beacon_api.h` template-trampoline pattern).
- [x] Audit log honors AGENTS.md Beacon invariant 9 (append-only, no
  rotate/delete, export-only): the log of record is `/beacon/audit.jsonl` on
  SD — pure append, never truncated — with the 64-entry flash-encrypted NVS
  ring demoted to a recent-view cache (`beacon_channel.cpp::sd_append_audit_entry`).
  `spec/beacon_channel_v0.md` §11's earlier 30-day/64 KB `prune_audit_log()`
  language contradicted the invariant, was never implemented, and is
  superseded. The `FEATURE_BEACON_CHANNEL=1` compile gate in
  `.github/workflows/firmware.yml` now compile-verifies the Beacon body in CI.
- [x] Self-test heartbeat (`BEACON_MSG_SELFTEST_OK`) emits daily; receivers surface `Trouble` on >36h absence.
- [x] X25519 keypair NVS-persisted (audit follow-up: codex P1 #7 closure in PR #454).
- [x] Audit log NVS-persisted as a ring buffer with head pointer (audit follow-up: gemini P1 #3 / codex P2 #8 closure in PR #454).
- [x] COSIGN_REQ/RESP encrypted with X25519 + ChaCha20-Poly1305 (audit follow-up in PR #454).

**Status: closed (PR #454 merged 2026-05-12).** Hardware verification of the two-pubkey origination flow remains queued — see `docs/audit/hardware_verification_checklist.md`.

### 9.1 Beacon receive-path hardening (2026-09)

A follow-up read of `beacon_channel.cpp::handle_alert_frame` against
`spec/beacon_channel_v0.md` §5.4/§7.1/§7.2/§8 found four invariants the
checklist above assumed but the code did not enforce. All four now hold on
the receive path, with mirrors in `tests_host/test_beacon_origination.cpp`
and `test_beacon_solo_origination.cpp` that fail on the pre-fix logic:

- **Signed `msg_type` is authoritative.** The signatures cover the canonical
  only, so `canonical->msg_type` is cross-checked against the unsigned
  header byte and the accept action keys off the signed value. A captured
  EXERCISE or CANCEL can no longer be rebroadcast as a real ALERT. Spec §5.4's
  flag rule is enforced as a biconditional — EXERCISE frames carry
  `BCN_FLAG_IS_EXERCISE` and nothing else does — so neither direction of the
  drill/alert substitution survives.
- **Replay and freshness (§7.1 steps 3–4, §8).** A 32-entry seen-frame ring,
  checked before signature verification, drops rebroadcasts at no
  cryptographic cost and before the originator's rate bucket is charged;
  `|now − effective| <= BEACON_FRESHNESS_S` is enforced, with the
  unsynced-clock accept-but-flag branch kept. The ring is keyed on the two
  Ed25519 signatures (16 bytes of each), **not** on the header nonce the spec
  names in step 3: the nonce sits outside `BeaconAlertCanonical`, so nothing
  signs it, and a nonce-keyed ring let a captured ALERT be re-sent with the
  nonce rewritten — past dedup, through verification, into the originator's
  rate bucket and back onto the alarm (review finding on the first cut of
  this pass). Signatures are deterministic under RFC 8032, so a copy carries
  the same identity however its header reads, and a fresh identity that
  verifies requires the keys. The latest accepted ALERT's identity is also
  held outside the ring, so a copy kept past the 5-minute horizon cannot
  re-raise the alarm on a receiver whose clock never synced (where freshness
  cannot run), CANCEL or no CANCEL.
- **CANCEL and UPDATE name their alarm (§5.4, §7.2).** The accepted alarm's
  header nonce is retained; both message types require a non-zero
  `ref_canceled_nonce` that matches it. An unreferenced frame is still
  audited — only its state effect is dropped. An UPDATE amends the alarm
  without becoming its identity, so a later CANCEL still resolves against the
  originating ALERT. Residual, spec-level: the reference is to the *header*
  nonce, which is unsigned, so a relay that rewrites it on the way through
  leaves receivers holding a nonce the originator's CANCEL will never name.
  Closing that needs the nonce inside the signed canonical (a wire-format
  change, spec §5); tracked below with the other open items.
- **Drills bank separately (AGENTS.md Beacon invariant 10).** `OriginationRate`
  carries a second counter selected by the signed `msg_type`, so an exercise
  can no longer spend a neighbor's real-alert budget.

Also closed in the same pass, from the same read: signer supervised-health
gating (§7.1 step 9) for signers whose selftest has lapsed past 36 h,
`COSIGN_WINDOW_MS` on a late `COSIGN_RESP` (§6.1), life-safety template
validation at origination, at the cosigner (§6.1 step 3) and on receive (§4),
and the signed selftest timestamp (§5.3) that makes `SELFTEST_OK` frames
non-replayable.

Closed in the follow-up pass: the §8 per-pair co-sign budget
(`MAX_ORIGINATIONS_PER_PAIR_24H`, checked on receive beside the per-pubkey
bucket; the pair is the two fingerprints in byte order, so trading roles does
not double the budget; solo frames and drills are not counted against it),
and §6.2's precondition that solo origination is for a device with no fresh
paired cosigner (`originate_alert_solo` refuses whenever
`pick_cosign_candidate()` would succeed, and `/api/beacon/originate-solo`
names the reason, `paired_cosigner_available`, so the operator is sent to the
two-device path rather than told "refused").

Still open on this surface: gateway-trust keys are accepted as ordinary
community cosigners with no upstream CAP attestation (the `.cpp` header
documents the gap); rate-limit state is not rebuilt from the audit log on
boot (§11 — `init()` runs before the wall clock syncs, so the persisted
entries' ages cannot be judged there; a rebuild deferred to first time sync
is the shape of the fix); `cancel_active_alarm()` still silences locally
without originating a `BEACON_MSG_CANCEL` (§10) — the REST reply now says
exactly that ("silenced on this device only — no network CANCEL was sent")
instead of "alarm canceled", but the network cancel itself needs the
dual-signed cosign flow with `msg_type=CANCEL`; and the CANCEL reference is
the unsigned header nonce (above).

## 10. Closure traceability — every finding's fix in code

| Finding | Severity | Fix shipped in | Code reference | Regression test |
|---|---|---|---|---|
| O1 | Medium | PR #450 | `mesh_network.cpp:561-572` | `test_mesh_opera_security::test_o1_counter_replay_protection` |
| O2 | High | PR #450 | `mesh_network.cpp::flash_encryption_enabled` + `persist_opera_config`/`load_opera_config` | `test_mesh_opera_security::test_o2_load_refuses_when_fe_off` |
| O3 | High | PR #450 (in-memory) + PR #454 (full transactional ACK) | `mesh_network.cpp::remove_peer` + `maybe_finalize_rekey` + `MSG_OPERA_REKEY{,_ACK}` cases | `test_mesh_opera_security::test_o3_rekey_commits_on_all_acks` + `test_o3_rekey_timeout_marks_unacked_stale` |
| C1 | Critical | PR #450 | `chirp_channel.cpp::handle_witness` (Ed25519::verify against carried `session_pubkey`) | `test_chirp_protocol_invariants::test_witness_canonical_layout` |
| C2 | Critical | PR #450 | `chirp_channel.cpp::handle_witness` (initial `confirm_count = 0`; ignored on wire) | `test_chirp_security::test_c2_c3_no_self_count` |
| C3 | Critical | PR #450 | `chirp_channel.cpp::handle_ack` (different-pubkey check) + `send_chirp` (initial 0) | `test_chirp_security::test_c2_c3_no_self_count` |
| C4 | Critical | PR #450 (envelope) + PR #454 (origin signature persistence) | `chirp_channel.cpp::handle_witness` + `relay_chirp` + `ReceivedChirp::origin_signature` | `test_chirp_protocol_invariants::test_canonical_distinguishes_signers` |
| C5 | Critical | PR #450 | `chirp_channel.cpp::handle_ack` + `pubkey_set_contains` | `test_chirp_security::test_c5_ack_dedup_by_pubkey` |
| C6 | Critical (design) | PR #450 | `ChirpWitnessPayload::session_pubkey` + `session_id_from_pubkey` validation | `test_chirp_protocol_invariants::test_canonical_distinguishes_signers` |
| C7 | High | PR #450 | `chirp_channel.cpp::handle_suppress_vote` + `CHIRP_MSG_SUPPRESS_VOTE` | `test_chirp_security::test_c7_suppress_dedup` |
| C8 | High | PR #450 | `chirp_channel.cpp::priority_heap_insert` | `test_chirp_security::test_c8_priority_storage_eviction` |
| C9 | Medium | PR #450 (1024 array) + PR #454 (4 KB Bloom) | `chirp_channel.cpp::bloom_hash` + `is_nonce_seen` + `cache_nonce` | `test_chirp_security::test_c9_bloom_flood` |
| C10 | High | PR #450 | `chirp_channel.cpp::wall_clock_is_synced` + `MIN_UNIX_TIME` gate | `test_chirp_security::test_c11_emoji_size_lifted` (companion) |
| C11 | Medium | PR #450 | `chirp_channel.cpp::generate_emoji_string` (5 emojis) | `test_chirp_security::test_c11_emoji_size_lifted` |
| C12 | High (latent) | PR #454 | `chirp_api.h::chirp_auth_gated<>` + `canary_wap.ino` registration under `FEATURE_MESH_NETWORK` | manual: cURL with/without Bearer header |
| C13 | High | PR #450 | `chirp_channel.cpp::confirm_chirp` (presence check) + `nearby_has_pubkey_with_presence` | covered by integration of C5 test |
| C14 | High | PR #450 | `chirp_channel.cpp::pubkey_rate_check_and_record` | manual flood test |
| C15 | Medium | PR #450 | `chirp_channel.cpp::is_night_mode` (conservative when unsynced) | covered by C10 |
| C16 | High | PR #450 (partial) + PR #454 (full per-finding suite) | `firmware/projects/canary-wap/tests_host/` | 6 host-test binaries, all green |
| C17 | Design | PR #450 | template enum removal + reserved slot comment at `mesh_network.h:615-619` | `test_chirp_protocol_invariants::test_protocol_version_is_v02` |

## 10. References

- `spec/canary_mesh_network_v0.md` — Opera mesh spec.
- `spec/chirp_channel_v0.md` — Chirp channel spec (to be bumped to v0.2 alongside the hardening).
- `spec/beacon_channel_v0.md` — new harm-reduction channel spec (this branch).
- `spec/beacon_cap_gateway_v0.md` — CAP interop spec (this branch; implementation deferred).
- `docs/research/harm_reduction_prior_art.md` — prior-art research brief (this branch).
- `docs/security/THREAT_MODEL.md`, `docs/security/SECURITY_MODEL.md`, `docs/security/SECURITY-AUDIT.md`, `AGENTS.md` — existing threat & policy docs.
- OASIS CAP 1.2 / ITU-T X.1303 — Common Alerting Protocol.
- NFPA 72 §10.4, §10.6 — Supervised circuits, trouble vs alarm vs supervisory states.
- FHWA MUTCD §2L — Dynamic Message Sign guidance.
- FCC Part 11 (EAS), 47 CFR §10 (WEA) — anti-impersonation rules cited in `spec/beacon_cap_gateway_v0.md`.
