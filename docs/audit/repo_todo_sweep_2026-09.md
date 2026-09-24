# Repo-wide TODO sweep — working backlog (2026-09)

**What this is.** On 2026-09-20 a full sweep of all three repositories
(`kmay89/securaCV`, `kmay89/securacv_website`, `kmay89/securacv-homeassistant`)
cataloged every marker of incomplete work: stubs, deferred features, unchecked
checklists, placeholder data, and honest-status admissions. This file is the
**cross-session work plan** distilled from that sweep: what we are trying to
accomplish, in what order, and what each item is blocked on. Sessions (human or
AI) pick items from here, do them, and check them off — so progress survives
context resets.

**Where this sits.** Read [`IMPROVEMENT_ROADMAP.md`](../IMPROVEMENT_ROADMAP.md)
for the 2026-09-02 three-repo audit — most of its 60 items have landed, and
this file deliberately does not repeat the residuals its rows still track
(the device-package leftovers, the TLS bench passes, the platform-pin
decision). This is the 2026-09-20 picture: a marker-level sweep of what is
stubbed, deferred, or checklist-gated *now*, organized as pickable work items
rather than audit findings.

**How to work this file**

- Item IDs are stable. Never renumber; add new items at the end of a section.
- When you start an item, append `— in progress (YYYY-MM-DD)` to its line.
  When you finish, tick the box and append the PR number. Do both **in the
  same PR as the fix** so this file never lags reality.
- Line numbers below are as of the sweep date and will drift; the file paths
  and quoted phrases are the durable pointers.
- This file does not restate the repo's own ledgers — where a canonical
  open-work document already exists (see "Canonical ledgers" at the bottom),
  an item here just points into it. Fix the ledger's item, then tick both.
- Gate tags: **[code]** an AI session can do it end-to-end · **[human]** needs
  hands, hardware, an account, or money · **[decision]** needs a maintainer
  call before code is written.

---

## 0. The five unlocks (do these and whole sections light up)

These are the choke points the sweep kept hitting. Each one un-gates many
items below.

- [ ] **U1 [human] One bench session on real hardware.** Seven built firmware
  capabilities are "Built · bench-gated" (`docs/hardware/dev_playground_todo.md`,
  "the real gating work" section),
  `docs/audit/hardware_verification_checklist.md` (23 items at the sweep,
  more since) is entirely unrun, and 12 board pin maps carry "NOT yet
  validated on bench". The playground doc calls this "the thing an AI cannot
  do from CI." Un-gates: F-section flags — now also the bench half of most
  firmware items that landed in #1703 and #1704, each named in its item
  below (e.g. runbook Track D for F15, the checklist's WPA3/PMF rows for
  F16; the Beacon and Opera-mesh rows also wait on code: F30, F31, F33) —
  C6, C7, plus the sentinel/sense bench boxes (the sentinel's now include
  its Phase 1a rows, F22).
- [ ] **U2 [human] The release signing-key ceremony.** The pinned key is the
  all-zero placeholder (`desktop/flash-engine/src/release.rs`,
  `canary-local/assets/flash-core.js`), so both flashers accept firmware on
  checksum alone. See `docs/RELEASE_BUTTONS.md`. Un-gates: signed-release
  verification everywhere; the flashers' "no signed release yet" fallbacks
  retire themselves.
- [ ] **U3 [human] Apple Developer account.** The signed release workflows
  (`tvos-release.yml`, `ios-release.yml`) exit green until their enable
  variables and `APPLE_*` signing secrets exist (`tvos/README.md`, "Status").
  `ios-selfheal.yml` is *not* gated on it — PRs always get an unsigned
  simulator build with no Apple credentials. Un-gates: A8, A13, tvOS/iOS
  App Store presence, Critical Alerts entitlement request, signed builds
  that exercise the CloudKit path.
- [ ] **U4 [human] FCC authorization work + responsible-party facts.**
  `store.json` has `part15b_sdoc: false` and blank responsible-party and
  `ship_from` blocks; `docs/strategy/29-fcc-and-product-compliance-diligence.md`
  holds the checklist. Un-gates: W-section store items for radio SKUs.
- [ ] **U5 [human] Stripe + PirateShip one-time setup (~1 hour).**
  `securacv_website/store-README.md` "One-time setup". Un-gates: W1 — the two
  already-FCC-clear parts SKUs could sell today.

Two smaller one-time human acts, same flavor:

- [ ] **U6 [human] Set `MIRROR_PAT` in the monorepo secrets.** Until then the
  HACS mirror refresh is inert-but-green
  (`.github/workflows/homeassistant-mirror.yml` warns and files an issue).
  The trees drifted: after #1703, #1704 and #1718 the mirror sat 33 carried
  files behind (every refresh ran green and pushed nothing) until a hand
  resync in securacv-homeassistant#17 (2026-09-24). Until the secret is set,
  every `main` change to the carried set needs that again.
- [ ] **U7 [human] Open the staged home-assistant/brands submission.**
  `brands/home-assistant/README.md` says "not submitted"; it is the only route
  to an integration icon on HA < 2026.3.

---

## 1. Firmware (securaCV monorepo)

The canonical defect ledger is `firmware/ESP32S3_OPTIMIZATION_ROADMAP.md`
(30 prioritized items; its own §6 table governs). The sweep spot-checked the
P0 rows by source inspection; status below reflects that inspection, not the
doc's claims. ("Verified" is reserved in this repo for a signature checked
against a pinned key or a claim proven on hardware — nothing below claims it.)

### P0 — confirmed still open by source inspection

- [x] **F1 [code] BLE Scout never scanned in the PlatformIO build** — done:
  `setup()` now flips `ble_scout_allow_radio()` after the NimBLE stack owner
  (`ble_status_stack_begin()`, which keeps the device's own GAP name) and
  before `securacv_csi_modules_init()`'s `ble_scout_init()` completes the
  deferred scan phase. The WAP's join-window deferral guarded its
  bluetooth_channel heap guard, which this tree does not have. Roadmap item 3
  updated; a live scan against a paired beacon is U1 bench work (#1695).
  A second-look audit of the whole BLE path (same PR) found the latch was
  necessary but not sufficient, in BOTH trees: the controller's duplicate
  filter defaulted ON (an indefinite scan reported each fixed-MAC device
  once, ever — RPA phones once per ~15 min rotation), and the "NimBLE will
  auto-restart" comments were false (nothing restarted an ended scan;
  `s_running` read true forever). Fixed: `setDuplicateFilter(false)` in the
  Scout TUs and `ble_presence`, intent-tracked restart in `onScanEnd`
  (deliberate stops clear `s_running` first and stay stopped), a ~1 Hz
  tick-cadence `nimble_scan_recover()`, and the heap guard added to
  `ble_status_stack_begin()` (its own rule required it at every init site).
- [x] **F2 [code] SD glitch disabled logging until reboot** — done: the
  declared-nowhere `sd_storage_remount()` turned out to live in an unbuilt
  scaffold header nothing included (deleted, like the six the 2026-09 audit
  removed); the real fix landed in `securacv_storage` — an idle-priority
  mount worker (the canary-wap watchdog lesson), `storage_periodic_check()`
  in `loop()` (verify / background remount, 30 s cadence), a
  consecutive-write-failure threshold, live `sd_healthy`, and an MSC gate on
  every teardown/remount. Decisions are the pure, host-tested
  `common/storage/sd_mount_policy.h`. Host/compile-tested; the physical
  remove/reinsert pass is U1 bench work. Roadmap item 5 updated
  (#1694).
- [x] **F3 [code] Camera never deinits on battery** — done: `loop()` acts on
  `policy_features.camera_peek` right after `policy_process()` — battery
  modes stop any peek stream and deinit the camera (retried each pass;
  `end()` fails soft while a held frame owns the F6 lifecycle lock), and the
  policy re-allowing it re-inits eagerly so vision resumes. The roadmap's
  drain estimate (~40–60 mA) stays unmeasured — the bench number is U1 work.
  Roadmap item 4 updated (#1696).
- [x] **F4 [code] CSI probes bypass the airtime governor** — done:
  `csi_probe::Config` gained an injectable `airtime_gate` hook (the module
  stays standalone and host-testable); every probe send — unicast fan-out
  and idle broadcast — reserves through it before the driver sees the
  frame, denials are counted (`Stats::sends_denied_airtime`) and keep the
  slot cadence so a saturated window doesn't burst when it clears. The WAP
  integration wires the hook to `airtime_governor::try_reserve_routine()`
  with the ~59 B ESP-NOW framing added so tiny probe payloads aren't
  undercounted. Three new host tests cover deny/resume/ungated. The rest
  of roadmap item 6 (modem-sleep vs CSI power-save gating; wiring the
  probe into the canary PIO build at all) stays open under that item.
  (#1696) A follow-up found reconciling it on the host (the governor's ring
  lost in-window airtime above 25.6 sends a second, and an over-budget probe
  starved the heartbeat) is F51 (#1722).
- [x] **F5 [code+decision] Ed25519 private key in NVS without enforced flash
  encryption** — done (option B — maintainer to confirm), with the premise
  corrected first: flash encryption does not cover NVS, so the key is
  encrypted at rest only under NVS encryption on top of it, which the Arduino
  2.0.17 core cannot build. The posture is now decided once, in the pure
  `firmware/common/identity/key_at_rest.h` (host-tested over all 16 fact
  combinations), and self-reported: the PIO canary image reports
  `key_at_rest` as `plaintext-nvs` on every board, fused or not, in
  `/api/status`, the health export, the `f` console card, one boot line and
  the `j` self-manifest (no other tree reports it yet). Default builds never
  refuse (Tier 0, `docs/design/hardware_root_of_trust.md` §8 #1/#3/#4); an
  image built with `SECURACV_REQUIRE_FLASH_ENCRYPTION=1` refuses to load or
  persist the key unless NVS is encrypted, so under `framework = arduino` it
  refuses on every board — by design, until roadmap item 9's IDF-component
  migration. The stale claims in `mesh_state.{h,cpp}` are corrected (the O2
  gate IS enforced; it keeps the household secret off un-fused boards, it
  does not make it confidential on fused ones). `[env:secure]` is built by
  no CI job and cannot compile as written (pre-existing, recorded in
  `firmware/provisioning/platformio_secure.ini`), so bench row K1 builds the
  opt-in from a normal env. Roadmap item 8 updated; K1 is U1 work. Roadmap
  item 18's (a) and (b) landed in the same change — see F38. (#1704)
- [x] **F6 [code] Camera init/deinit vs peek-task race** — confirmed real by
  source inspection (the stream task's freeze recovery cleared `peek_active`
  then deinit+begin behind flag guards only, while the loop-task vision
  capture and two httpd handlers could call into the driver), then fixed:
  one lifecycle mutex in `CameraManager`. `captureFrame()` holds it until
  `returnFrame()` (the frame buffer is driver memory); lifecycle ops take it
  with a 2 s timeout and fail soft instead of blocking toward the 8 s task
  watchdog. Compile-tested; a live freeze repro is U1 bench work. Roadmap
  item 7 updated (#1696).
- [ ] **F38 [code+decision] Roadmap item 18's hardware key protection is
  still open** (a P1 row, listed here beside its sibling F5). Its two
  host-testable parts landed with F5 (#1704): (a) the PIO canary tree's
  first-boot identity keygen runs inside `bootloader_random_enable()` /
  `bootloader_random_disable()`, the PR #994 pattern the other three trees
  already had (the Scout key and the mesh pairing keys are generated after
  RF is up and are deliberately left bare), and a `regression_check.sh` gate
  now requires the call in all four keygen files; (b) `{seq, chain_head}`
  persist as one atomic 39-byte NVS blob sealed with a CRC-16
  (`firmware/common/witness/chain_state.h`, host-tested; the legacy pair
  stays a read-only fallback, and a legacy seq ahead of the blob's wins, so
  a re-upgrade after a downgrade does not re-sign seqs). Open, recorded in
  roadmap row 18 and not attempted: (c) a DS/HMAC-bound key — the DS route
  is RSA-only and reserved by `hardware_root_of_trust.md` §5.4 / §8 #4, and
  `key_at_rest.h` reserves its `hw-bound` label — and (d) an eFuse/RTC
  rollback anchor (design only); both need the IDF-component toolchain
  (roadmap item 9) and a bench. Also skipped: the canary-wap carry of
  `chain_state.h` (three write sites, a staged copy, a `check_csi_sync.sh`
  entry) and an entropy self-check. Bench (U1): K1's fresh-unit step (the
  keygen's `bootloader_random` pair shares the SAR ADC the battery monitor
  has already opened — does the reading survive?) and K2 (a power cut
  between record and persist never leaves a torn seq/head pair).

(Roadmap items 1 and 2 were confirmed **fixed**, and the roadmap doc now says
so — see D2 below.)

### Timeline & time

- [x] **F7 [code] Device timeline was one block deep** — done: the timeline
  now reads the 32-record witness ring the network layer already served for
  it (`/api/witness`), newest first, and `loadMoreTimeline()` really pages —
  a new `?before=<seq>` exclusive bound on the endpoint walks backward
  through the ring (still RAM-only; each slot copied under the ring lock).
  Auto-refresh pauses while the reader is paging so it can't collapse the
  list, and the renderer's type lookup gained the `type_name` key the ring
  records actually carry. Depth beyond the ring is F26 (#1694).
- [x] **F8 [code] `time_bucket` is session-relative, not wall-clock** — done:
  both trees' GPS clock sync (the one wall-clock source either has) now
  calls `csi_event_set_clock_offset_minutes()` — on the first
  `settimeofday()` and re-derived on every pass with a set clock, so the
  offset stays drift-corrected and survives `millis()` rollover. Buckets
  and quiet-hours minute-of-day are UTC-aligned; there is no timezone
  setting, so bucket 0 is UTC midnight, not the household's — that gap is
  F28. Until the first fix the old session-relative behavior remains, as
  the csi_event.cpp comment now states. (#1696)
- [x] **F9 [code] MQTT offline queue** — done: a bounded FIFO
  (`firmware/common/mqtt/mqtt_offline_queue.h`, pure, host-tested by
  `tests_host/test_mqtt_offline_queue.cpp`) buffers tamper alerts and
  events across a broker outage and replays them in order on reconnect —
  drop-oldest on overflow, oversize refused rather than truncated, fresh
  publishes join the back of a still-draining queue so order holds.
  `main.cpp`'s tamper drains gate on the new `mqtt_accepting()` instead of
  `mqtt_connected()`, so an outage no longer collapses every tamper in the
  window into the one-deep pending slot's newest value. Periodic snapshots
  (status/health/sensing) are deliberately not queued — their next tick is
  fresher truth. Two corrections to this item's own claims: the HA
  "unknown after broker restart" half was already fixed before the sweep
  (mic/update/auto states are stashed and republished on every reconnect —
  the three code comments describe that fix, not a gap), and the canary
  PIO tree turns out to have **no caller** of `mqtt_publish_event()` at
  all — event egress to HA exists only on the WAP (`csi_mqtt`, with SD
  backfill). Wiring canary event egress is F29; the queue gives that
  transport its loss bound the day it gets callers. Roadmap item 17
  updated (#1697).
- [x] **F44 [code+decision] Two 5-second time-bucket stragglers contradict the
  ten-minute floor.** The IR-TIMEBUCKET decision (option B — maintainer to
  confirm; `docs/IMPROVEMENT_ROADMAP.md` §3, "Landed in wave 4") made
  600 000 ms the floor and default of both firmwares' witness chains in
  #1704, but two canary-tree sites still offer the grid it retired. Both
  predate the decision; the fw-pins package in #1704 flagged them and
  changed neither. (1) `firmware/canary/include/secure_defaults.h:71-72`
  defines `DEFAULT_GPS_COARSENING_MS 5000` ("5000ms = 5-second buckets")
  with a `< 5000` `#error` floor at :194. Nothing reads the value, and
  nothing evaluates the floor either: no file `#include`s the header and no
  build defines `SECURACV_ENFORCE_SECURE_DEFAULTS`. Delete it, or widen it
  to 600000 with its floor. (2)
  `firmware/canary/lib/securacv_webui/src/securacv_webui.cpp:2393`: the
  Device Configuration form offers "Time Bucket (ms)" at `value="5000"`,
  `min="1000"`, `max="60000"`, and its Save posts to `/api/config`, a route
  the canary tree does not register, so the form is inert. Align it to
  canary-wap's `web_ui.h` field (value and min 600000, max 3600000) or
  delete the dead form; `regression_check.sh`'s time-bucket section holds
  only the canary-wap field to the floor today. Neither site reaches the
  chain (`canary_config.h`'s `TIME_BUCKET_MS` is 600000). The choice
  between deleting and widening, at each site, is the maintainer's.
  *Done (#1718, option delete both — maintainer to confirm):* the unused
  `DEFAULT_GPS_COARSENING_MS` and its floor are deleted, and so is the canary
  web UI's back-end-less Device Configuration form (its live Reboot button
  stays). `regression_check.sh`'s new "Privacy: canary time-bucket floor
  (Invariant III)" section fails a canary bucket constant off the ten-minute
  grid and holds any time-bucket field to min=600000.

### Mesh / fleet / beacon

- [x] **F10 [code] Five of eleven specced mesh REST endpoints are deferred** —
  done: the PIO tree now registers every mesh route its web UI sends.
  `POST /api/mesh/leave` forgets locally and sends a signed `LEAVE_OPERA` that
  can remove only its own signer; `/name` is FE-gated NVS, local only;
  `/enable` is an NVS-persisted flag (not FE-gated); and
  `GET`/`DELETE /api/mesh/alerts` sit on a new alert channel — a compact
  6-byte kind/severity/seq `TAMPER_ALERT` (`mesh_alert.h`, no free text) sent
  from the sensing witness and dispatched only after signature, opera_id and
  replay checks, with `DELETE` clearing history, not counts.
  `POST /api/mesh/remove {fingerprint}` drops the peer AND rotates
  `opera_secret` (`mesh_rekey.{h,cpp}`: an ephemeral X25519 exchange inside
  signed envelopes, the ACK sent under the old opera_id, commit on every ACK
  or at 60 s) — option B — maintainer to confirm, with a maintainer crypto
  review owed (its commit asks for one). The mutators run on the main loop
  through a one-deep request slot (409 `mesh_busy` / 503 `mesh_timeout`), and
  a dropped peer's replay counter survives as a persisted tombstone. Fixed on
  the way: the pairing initiator never registered the joiner. Option defaults
  (enable, name, leave, alert payload, DELETE) — maintainer to confirm. Spec
  v0.3; the FEATURES.md cells stay ⚠️. Host-tested; the real primitives are
  compiled only by CI's `[env:full]` leg, and nothing of this has crossed a
  radio yet (F33), so U1 Track C3 comes after that. Not wire-interoperable
  with canary-wap (`MSG_TAMPER_ALERT` 4 vs 18). (#1704)
- [x] **F11 [code] Fleet peer liveness is fabricated** — done, in two
  halves. Liveness: `mesh_session` now records the source MAC of every fully
  verified opera-authenticated frame against the sender's fingerprint
  (signature + opera_id + replay all passed, so the binding is as
  trustworthy as the frame; an unverified or replayed frame cannot rebind
  it — host-tested), and `/api/mesh/peers` joins that MAC into the live
  transport table for real `state`/`last_seen_sec`/`rssi`. A peer that
  hasn't spoken this boot honestly reads OFFLINE/never. Spec §8.3
  peer-fields note updated (#1698). Attribution: `alerts_received` read 0
  until an alert channel existed; with F10's, a `TAMPER_ALERT` counts per
  peer and opera-wide only in the verified-dispatch branch, and
  `GET /api/mesh` / `GET /api/mesh/peers` read the real numbers (per boot,
  spec §8.3). No HA attribute — the PIO tree publishes no MQTT mesh topic.
  (#1704)
- [x] **F12 [code] `ble_mesh.cpp` (canary-wap) is a stub module** — done by
  deleting the seam (option B — maintainer to confirm): `ble_mesh.{h,cpp}`
  had no includer or caller anywhere yet compiled into every WAP image, and
  its `init()` always refused. `docs/BLE_MESH_OPERA_TANDEM.md` is now
  design-only and keeps the scaffold's 14-byte frame header, so nothing is
  lost; its transport decision (options A/B/C, B recommended) stays open
  there. (#1704)
- [x] **F13 [code] Beacon gaps (canary-wap `beacon_channel`)** — done, clause
  by clause, each decision labeled "maintainer to confirm". CANCEL:
  `POST /api/beacon/cancel` originates a `BEACON_MSG_CANCEL` for the alarm
  this device holds over the two-pubkey cosign flow,
  `POST /api/beacon/cancel-solo` sends it on the BOOT-held solo path, and the
  old local mute is now `POST /api/beacon/silence` (D1 split routes; D2 a
  CANCEL charges the originator's 5/24 h bucket like an ALERT; D3 a strict
  cosigner gate; D4 the originator adopts its own frame without a charge).
  Four defects that would have sunk any CANCEL were fixed with it — the
  emitted header was hardcoded to ALERT, the originator never entered ALARM
  for its own frame, the cosigner had no msg_type gate, and the cosigner never
  held the alarm it co-signed — and the decisions live in Arduino-free,
  host-tested headers (`beacon_wire.h`, `beacon_cancel_policy.h`). COSIGN: the
  "unencrypted broadcast" premise was wrong — COSIGN has been X25519 +
  ChaCha20-Poly1305 since #454; the stale ledgers are corrected, the routing
  fields are now bound as AEAD associated data (`beacon_cosign_aad.h`), and an
  all-zero X25519 secret is refused (option B). Gateway: deferred by decision
  (option 2) — nothing built, two host tests pin that gateway trust grants
  nothing, and the milestone is F32. The docs now also say the solo path's
  BOOT gate stops a remote API caller, not a holder of one member's key. The
  only real compile is CI's `FEATURE_BEACON_CHANNEL=1` arduino-cli leg in
  `firmware.yml`, and none of it runs on a board yet: the §3.3 pairing flow is
  a stub (F30) and the loop is not wired (F31), so the checklist's "CANCEL
  propagates" row is U1 after both. Still open in
  `docs/audit/mesh_and_chirp_audit_v1.md`: the CANCEL reference is the
  unsigned header nonce (a wire-format change), and rate state is not rebuilt
  from the audit log on boot. (#1703)
- [x] **F14 [code] Staged mesh PRs referenced in headers never landed** —
  done: the stale 2a/2b/2g/2h/2i/4b/5c-4/5c-5 forward references in
  `mesh_envelope.h`, `mesh_session.h`, `mesh_transport.h` and `library.json`
  now describe what exists, and two false claims are corrected — the
  envelope is layout-parallel to canary-wap's, not wire-compatible (PIO
  version 1 vs Opera 0, different type numbering), and the outbound counter
  is RAM-only (F33). The live canary-wap pair-frame bug they recorded is
  fixed (option B — maintainer to confirm): pairing frames went out as raw
  structs under 102 bytes, which the signed-header gate dropped, so
  WAP-to-WAP pairing could never complete; they now carry a 1-byte type
  prefix and are classified first by the pure `mesh_pair_frame.h`
  (host-tested, canary-wap `tests_host/test_mesh_pair_frame.cpp`), and the
  unauthenticated in-header pairing branch is gone. Host-tested only; bench
  is U1 Track C2, where F33's key defect should surface as mismatched
  codes. The follow-ups are tracked here (F33) rather than as issues.
  (#1704)
- [ ] **F30 [code+decision] Beacon §3.3 pairing flow.** Nothing can add a
  member to a beacon set: build the `PAIR_OFFER` wire type, an ephemeral
  X25519 exchange with a 6-digit confirmation, the FE-gated beacon-set write
  with `has_x25519_pubkey`, and `spec/beacon_cap_gateway_v0.md` §2.2's cap
  of three non-revoked gateway entries enforced at the add. Design-review
  sized, its own PR. Until it lands no board can pair, so F13's encrypted
  two-device path (ALERT and CANCEL) is unreachable.
- [ ] **F31 [code+decision] The Beacon runtime loop is not wired.** Wire
  `beacon_channel` into `canary_wap` behind `FEATURE_BEACON_CHANNEL`:
  `init()` beside `chirp_channel::init()`, `update()` in the loop,
  `dispatch_espnow_message()` beside the chirp forward in
  `mesh_network.cpp`, the alarm/state callbacks routed to MQTT
  `beacon.state` and `PATTERN_BEACON` audio, and `set_enabled()` behind an
  explicit, persisted user opt-in — it has no surface today, and enabling
  at boot would switch on a life-safety broadcast (plus its daily
  fingerprint self-test broadcast) with no user choice. Prerequisite: a
  COSIGN_REQ frame is 310 B and the shared ESP-NOW receive path drops
  anything over 250 B, so either shrink
  `BeaconCosignRequestPayload.ciphertext[160]` to the 72 B canonical (a wire
  change: a 222 B frame, and the size pin in
  `test_beacon_cancel_origination.cpp` moves) or widen `mesh_network`'s
  buffer. The handler-budget part already landed with F13. Two policy calls
  ride with it: whether a CANCEL is exempt from the originator's 5/24 h
  bucket (spec §8; today an originator on its fifth origination cannot
  cancel its own alarm), and what a two-device CANCEL does when the chosen
  cosigner silenced the alarm locally or missed the ALERT (today it refuses
  silently, the deterministic pick keeps choosing it, and each retry spends
  a bucket slot).
- [ ] **F32 [decision+human] CAP-gateway attestation milestone.** Deferred
  by decision in F13; `spec/beacon_cap_gateway_v0.md` §6 sets the gates in
  order: the trust-root decision (§5 question 1), a separately named
  firmware build, a per-deployment legal review with an operator identity,
  the gateway pairing UX and §2.2 cap (with F30), then code. Until then a
  gateway-trust entry is a plain cosigner, pinned by
  `test_gateway_trust_confers_no_privilege` and
  `test_source_grants_gateway_trust_nothing`.
- [x] **F33 [code] The PIO Opera mesh has not yet carried a frame over a
  radio.** Found while landing F10/F14 and not fixed there; each needs its
  own change and a bench (U1 Tracks C2/C3). (1) `mesh_transport`'s peer
  table is never populated on a device — no `add_peer` caller outside host
  tests — so every inbound frame drops as `recv_dropped_no_peer` and
  `broadcast()` reaches nobody: pairing, alerts, leave and rekey included.
  (2) Pairing runs X25519 over Ed25519-generated keys in BOTH trees, so the
  two sides derive different secrets on a device and the 6-digit codes
  cannot match (PIO: use `mesh_crypto::x25519_generate_keypair`; the WAP
  needs a clamped Curve25519 keygen; crypto review either way). (3) The PIO
  outbound counter is RAM-only while receivers persist theirs, so a
  rebooted sender's frames drop as replays until it catches up. (4) The PIO
  tree has no way to create an opera. (5) The four pairing handlers still
  call into the session from the httpd task (F10's mutators already moved
  to the loop's request slot). (6) Two removals from two devices inside the
  60 s window can split the opera (the spec's `REVOCATION_GRACE_MS`
  deny-list is in neither tree). (7) The web UI renders an alert's
  receiver-uptime `timestamp_ms` as a wall-clock time. FEATURES.md's "Mesh
  network (Opera / ESP-NOW)" row still reads ✅ for canary-wap although (2)
  and (6) hold there too; `features_dashboard_guard` refuses a downgrade, so
  lowering that cell is a maintainer's edit.
  *Done in code (#1718):* all seven parts. Nothing has run on two radios yet.
  - (2) Pairing ephemerals are clamped X25519 keys in both trees. The PIO
    host X25519 is now a real RFC 7748 ladder, and a host test shows two
    independent keypairs agree on the secret and the code (crypto review —
    maintainer to confirm).
  - (1) The transport peer table fills from pairing and NVS `peer_macs`.
  - (3) The outbound counter reserves ahead in NVS (`mesh_out_ctr`), so no
    counter is signed twice and a rebooted sender is not dropped as a
    replay.
  - (5) The four pairing routes run on the loop's request slot.
  - (6) The §5.6 seven-day deny-list is in both trees. PIO also converges
    two concurrent removals: a settle window, lower-fingerprint precedence
    and OFFER propagation (crypto review — maintainer to confirm). Since
    27bd7ea, a removal reaches NVS and the health log once, not once per
    resent OFFER.
  - (7) The web UI shows an alert's age.
  - (4) `pair/start` founds an opera on a PIO device that holds none, on the
    existing route and gates.

  FEATURES.md's mesh cell is untouched (the maintainer's call). Bench (U1):
  Tracks C2/C3, whose rows the runbook now carries. Found here and recorded
  as F48 and F49.

### Network surface & provisioning

- [ ] **F15 [code] No TLS on the canary.local HTTP/peek surface.** The one
  gap `firmware/PARITY_PLAN.md` lists as shared by *both* trees (❌ / ❌ at
  the sweep; the dashboard reads ⚠️ / ⚠️ since #1691). Landed for the
  canary tree's dev builds in #1704 (option (b) — maintainer to confirm):
  self-signed ECDSA P-256 HTTPS on 443 serving the whole route table, the
  certificate generated on the device once and kept under the WAP's NVS
  keys, plus a port-80 server that keeps the six connectivity probes and
  307-redirects the rest (307, not the WAP's 301: a cached permanent
  redirect would strand a factory-reset device's plain-HTTP setup). The
  decisions and the redirect builder live in the pure, host-tested
  `common/network/tls_policy.h`; TLS is skipped during first-boot setup, the
  peek stream runs in-handler over TLS, and `/api/status` reports
  `tls_enabled` / `tls_cert_fp` / `tls_mode_reason` (a core missing a
  capability builds HTTP-only and says why). `FEATURE_HTTPS=1` is on in
  `[env:dev]` (inherited by dev_ha, usb-onboard and full, and restated in
  full) and off in release and the board envs; the compile is CI's (the
  dev and full legs; `dev_ha` inherits the flag but no workflow builds it,
  `firmware/flavors.json` `build_envs`), and FEATURES.md reads ⚠️ for
  canary (PIO).
  Still open: turning it on in release/release_ha, the maintainer's call
  once the size-guard delta is read (option (c), flasher-provisioned
  certificates, is the fallback if the 2.0.17 core lacks x509write);
  canary-wap, whose dashboard cell reads ⚠️ since #1691: a runtime opt-in,
  not the build's posture — compile-gated on `esp_https_server.h`, served
  after setup once the on-device certificate loads, and plain HTTP during
  setup and on a start failure, logged (`firmware/FEATURES.md`, the
  canary-wap HTTPS note; PARITY_PLAN's shared TLS line); the canary's mDNS
  TXT record, which does not yet advertise TLS, so a discovery client cannot
  tell HTTPS is on; and the bench, U1 runbook Track D, D1–D5.
- [x] **F16 [code] WPA3/PMF + per-device AP password** on the WAP join path —
  done (option (b) — maintainer to confirm): both trees now ask for WPA2/WPA3
  transition on the SoftAP with PMF capable and never required, and for PMF
  capable on the STA, through one pure header
  (`common/network/ap_security_policy.h`, host-tested; the WAP's staged copy
  is held byte-identical by `check_ap_security_sync.sh` in CI). SoftAP SAE
  exists only on IDF 5 cores, so the 2.0.17-core builds (canary
  dev/release/board envs) stay WPA2 and say so in `ap_auth` /
  `ap_auth_reason`. The per-device AP password already existed in both trees —
  the item's "hardcoded AP password" premise was stale; the `pre_build.py`
  tripwire matches no source today (and, unverified without PlatformIO, the
  canary tree's `extra_scripts` line sits under `[platformio]` with a path to
  a nonexistent `<repo>/scripts/pre_build.py`, so the script appears never to
  run there). Widening the canary's 8-character AP password is a separate
  maintainer decision (it needs an NVS derivation-version marker so
  provisioned devices keep theirs). New FEATURES.md row, ⚠️ in both trees;
  roadmap item 21 updated; four bench boxes in
  `docs/audit/hardware_verification_checklist.md` (U1). (#1704)
- [x] **F17 [code] `provision_core.h` exists in two byte-identical copies** —
  done, the include flipped: the display's `provision.cpp` / `onboard_ui.cpp`
  now include `network/provision_core.h` straight from `common/` (the
  `-I ../../common` every display env already carries), the display copy is
  deleted, and `check_provision_core_sync.sh` plus its CI step are retired.
  The Arduino sketch stays self-contained — `setup.sh regen` stages the
  canonical header flat next to the sketch (same committed-copy pattern as
  `wifi_join_policy.h`), covered by the existing sketch-sync CI gate. The
  design doc and the four comments that described the pinned pair now
  describe the single copy. This is only the include flip the sync script's
  header promised — the display's full migration to the shared portal
  remains Phase-4-last per `docs/design/onboarding_shared_module.md`.
  (#1698)
- [x] **F18 [code] `DEVICE_CHIP_ID="placeholder"` fallback** — done, with the
  real defect being the neighboring `"unknown"` fallbacks on the live path:
  the fleet manifest is keyed by MAC, and an unreadable device used to be
  written as MAC `unknown` — a row no fleet tool can match to hardware, and
  a second such device collides with the first. `get_device_info` now
  refuses (exit 1, esptool's output echoed) when the MAC cannot be read.
  The chip-ID fallback is honest instead of fabricated: ESP32-S3
  legitimately reports no chip ID, so it prints "none (… the MAC is the
  identity)". The `"placeholder"` literal survives only inside the labeled
  `--dry-run` branch, which writes nothing. (#1698)
- [x] **F19 [code+decision] `FEATURE_TAMPER_GPIO` is defined but never
  consumed** — done (option B — maintainer to confirm; the pin is U1
  bench-unvalidated): the flag now has a consumer in both trees, a pure
  debounce-and-hold contact FSM (`common/csi/src/contact_tamper.h`: 5
  samples AND 300 ms, wrap-safe, host-tested) feeding a new `enclosure` kind
  into the existing tamper chokepoint on CLOSED→OPEN only. The touch-mode
  alternative was not taken: touch is S3- and PIO-only, senses a capacitive
  release rather than a magnet leaving, and the BOM already sells the reed
  switch (SW2). The canary's unread `TAMPER_GPIO 2` gave way to the board
  map's `TAMPER_PIN_DEFAULT` 4, held equal in both trees by a new
  `check_board_registry.py` check, and `canary_config.h` refuses the GPIO4
  touch collision (move touch with `-DTOUCH_PIN_NUM=5`). HA sees an open
  lid through the health's `enclosure_open` and F29's tamper-topic bridge.
  The flag stays 0 in every shipped profile — canary `[env:full]` and WAP
  FULL are flasher-offered — so CI builds it with the flag on only in a
  canary dev rebuild and the WAP Beacon leg, and the catalog's pin status stays
  "planned". Bench (U1): pin, polarity (the NC reed held open by the
  magnet) and the debounce policy. (Re-scope recorded in #1699.) (#1704)
- [x] **F39 [code] The canary's OTA deploy scripts cannot deploy.**
  `firmware/canary/scripts/ota_deploy.py` and `ota_deploy.sh` POST the image
  to `/api/ota` with no `Authorization` header, and `handle_ota` answers
  `auth_gate()` first (`securacv_network.cpp`), so every device refuses
  both scripts (the bearer check fails, or a 503 before a token is
  provisioned). Pre-existing — found by the fw-netsurface package in
  #1704, not introduced there. Take the bearer the way the other operator
  tools do (an env var or a prompt, never an argv literal), and with F15
  on, speak HTTPS to 443 with the device's pinned `tls_cert_fp`.
  *Done (#1718):* both scripts take the bearer from `CANARY_TOKEN` or a
  no-echo prompt (never argv, never printed; the shell passes it to curl on
  stdin), and with `CANARY_TLS_FP` speak HTTPS to 443 only after the presented
  certificate's DER SHA-256 equals the pin; an unpinned TLS device is refused
  with its fingerprint printed, and an HTTP-only build keeps plain HTTP.
  `test_ota_deploy.py` (17 cases against a fake Canary) runs in the Mesh +
  Scout host-test job, with shellcheck. The pin comes from the operator (the
  recovery kit's `tls_cert_fp`), not from `/api/status`, which would hand the
  token to an unverified peer. Bench (U1): a real push both ways.
- [x] **F40 [code+decision] The canary tree's pre-build tripwire never
  runs.** `firmware/canary/platformio.ini` sets
  `extra_scripts = pre:../../scripts/pre_build.py` under `[platformio]`,
  where PlatformIO does not read `extra_scripts` (it is an `[env]` option),
  and the path resolves to a nonexistent `<repo>/scripts/pre_build.py` (the
  script is `firmware/scripts/pre_build.py`). Wiring it as written would
  not work either: the script reads `__file__`, which is not defined when
  SCons runs an extra script, and a dry run of its checks against today's tree
  blocks the build on a comment ("No localStorage/cookies." at
  `canary-wap/.../web_ui.h:2707`) that `regression_check.sh`'s version of
  the same check skips. The CI guard it duplicates
  (`regression_check.sh`, the "Regression Guards" job) does run. Either
  delete the dead line and the script (the docstring and `common.ini`'s
  note point at the wrong path too), or port it: `[env]` placement, a
  correct path, `env.subst("$PROJECT_DIR")` in place of `__file__`, and the
  comment filter. Maintainer to choose.
  *Done (#1718, option delete — maintainer to confirm):* the dead line and
  `firmware/scripts/pre_build.py` are removed, and every pointer to it now
  names `regression_check.sh`, whose new "Build: PlatformIO extra_scripts"
  section fails an `extra_scripts` outside `[env]` or one that points at a
  missing file.
- [x] **F52 [code] The canary's NVS sessions were not serialized across
  tasks.** Found by the wave-7 security scout (its finding 4, not this
  file's F4). `NvsManager` (`firmware/canary/lib/securacv_crypto`) was one
  shared `Preferences` handle with a bare open flag. Three tasks open
  sessions on it: the loop (MQTT reload, witness chain persist, the birth
  stamp, factory reset), the httpd task serving the API (MQTT status, config
  and CA, Wi-Fi connect and disconnect, the reboot's chain persist) and the
  pull-OTA task (the pre-reboot chain persist). A session ending on one task
  closed the other task's handle. A status poll during a reload could read
  the broker host as empty and leave MQTT off until the next reprovision; a
  disturbed credential-carry read could make a same-host save drop the
  stored broker password. A write racing that `end()` landed nothing while
  `nvs_store_bytes` still returned true (F55), and that includes the atomic
  chain blob.
  *Done (#1722):* a recursive FreeRTOS mutex is held from `begin()` to the
  matching `end()`, and a depth count makes nested sessions on one task
  close only at the outermost `end()`. An `end()` from a task with no
  session is a no-op. Each wait is bounded (2 s, under the 8 s loop
  watchdog) and fails soft the way F6's camera lock does. No caller
  changed. The depth arithmetic lives in `nvs_session_depth.h` and is
  host-tested (`test_nvs_session_depth`), and `test_nvs_manager_lock` runs
  `securacv_crypto.cpp`'s own `begin()`/`end()`, cut out by the Makefile,
  against a fake recursive mutex. The FreeRTOS mutex is compile-tested by
  CI's canary envs; it has not run on a bench. Bench (U1): the NvsManager
  rows in `docs/audit/hardware_verification_checklist.md`. canary-wap keeps
  its own copy of the class: F53.
- [ ] **F53 [code] canary-wap's NvsManager copy has the same shared handle.**
  `firmware/projects/canary-wap/arduino/canary_wap/nvs_store.h` carries its
  own header-only `NvsManager` with the bare open flag F52 removes from the
  canary. The WAP opens sessions from `canary_wap.ino` (its esp_http_server
  routes among them), `vault_snapshot.cpp` (`set_pubkey_hex`,
  `clear_pubkey`) and `bluetooth_channel.cpp`, and it runs several FreeRTOS
  tasks. Trace every caller to its task. If more than one task holds a
  session, port F52: move its `nvs_session_depth.h` to `firmware/common/`,
  stage it for the sketch and add the lock. Compiled by the WAP PlatformIO
  and Arduino CLI legs. Found in the wave-7 reconcile; not traced to a live
  overlap.
- [ ] **F55 [code] The canary's NVS writes report success whatever the
  write did.** `nvs_store_bytes` and `nvs_store_u32`
  (`firmware/canary/lib/securacv_crypto/src/securacv_crypto.cpp`) return
  true once the session opens, whatever `putBytes` / `putUInt` returned, and
  `witness_persist_chain_state()` (`securacv_witness.cpp`) does not read
  `persist_chain_blob()`'s result anyway: it advances `seq_persisted`
  regardless. So any NVS write failure, not only the cross-task race F52
  closes, silently drops the atomic `{seq, chain_head}` blob F38 (b) added
  (#1704), and nothing counts or logs it.
  `ScvNetworkManager::saveCredentials()`
  (`firmware/canary/lib/securacv_network/src/securacv_network.cpp`) has the
  same shape: it ignores what `putBytes` / `putBool` returned, marks the
  credentials configured, logs 'WiFi credentials saved' and returns true,
  and `handle_wifi_connect` ignores even a false, so a Wi-Fi save the API
  reported as done can be silently dropped. Return what the put wrote, leave
  `seq_persisted` behind when the chain persist fails so the next interval
  retries, count the failure beside `chain_persists`, and have the Wi-Fi
  route answer an error when the save fails. The helpers are CI-compiled
  only; a pure retry rule could be host-tested. Found reconciling F52.
- [x] **F56 [code] The canary's provisioning receipt did not ask the Host,
  and a page load under a foreign Host could spend the BOOT tap.** Found by
  the wave-7 security-docs review. `GET /api/provisioning-receipt`
  (`firmware/canary/lib/securacv_network/src/securacv_network.cpp`) is
  gated by a bearer or one BOOT tap rather than `auth_gate`, so #1691's
  Host check did not cover it. On the page path the Host was read in
  `send_html_with_token`, after `page_token_inject` had already taken the
  tap.
  *Done (#1722):* the receipt route refuses a foreign Host like every other
  token-bearing route (`403 {"error":"host"}`, before the bearer or the
  tap; the SoftAP is exempt, as for `auth_gate`). Its handler serves only
  on an explicit bearer or tap verdict and refuses every other one. A page
  load under a foreign Host no longer spends the tap.
  `provisioning_gate.h`'s `receipt_decide` and `page_token_decide` carry
  the order and are host-tested. `check_route_security.py`'s
  `check_host_first` checks that the glue hands them the Host, obeys the
  receipt verdict, and keeps `host_is_foreign` as reviewed. Route audit:
  every other API route runs `auth_gate`, and the public routes (`/`,
  `/setup`, six probes, the wildcard fallback, the port-80 redirect) hand
  out the token only through the page decision. The glue is syntax-checked
  against the core 3 headers and built by CI only. Not bench-tested.
  Hardening TODOs: apply the same Host-first order to the WAP sketch's
  receipt route, and consider refusing browser cross-site requests before
  either tap path takes the tap.

### Parity & sub-projects

- [ ] **F20 [code] Arduino↔PlatformIO parity debt.** Canonical ledgers:
  `firmware/FEATURES.md` (the ❌/⚠️ dashboard) and
  `firmware/canary/CONSOLIDATION.md` (gap inventory, 16 rows since F29 added
  one; phase 3 half done, phases 5–8 unstarted). Gap #11 is done (option D —
  maintainer to confirm), its premise corrected: the canary never had an
  unauthenticated receipt route — it had none at all; the real exposure was
  `GET /` and `GET /setup` handing the bearer token to any home-LAN caller.
  Now a short BOOT tap opens a 30 s gate
  (`common/network/provisioning_gate.h`, host-tested) that admits exactly one
  consumer — one `GET /api/provisioning-receipt` (the WAP's shape) or one page
  load — and the pages carry the token only for first-boot setup, a
  bearer-authenticated request, a SoftAP peer or a spent tap; a bare LAN load
  gets an unlock banner instead. A fail-open found on the way is closed (with
  no credential provisioned, `checkOptional` accepted a bare `Bearer` header
  against the empty token), and
  `firmware/canary/scripts/check_route_security.py` now fails CI on any route
  that reaches no credential gate and is not on its public allowlist. The
  dashboard's canary gate row had read ✅ before any canary code existed; it
  reads ⚠️ until the U1 bench pass (one tap → one receipt, a second GET → 403)
  (#1704). Carrying over the WAP's session-cookie model, so a LAN reload stops
  needing a tap, is a Phase 6 follow-up. Next: gap #10 (`hardware_state` safe
  mode), then the unstarted phases.
- [ ] **F21 [code] canary-wap enterprise readiness** —
  `firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md`. §1
  (security/privacy) is done. Reconciled in #1704: every tick carries its
  evidence pointer, boxes that something else superseded say so, and the
  code-sized items landed — a Good / Needs attention / Action required
  status strip from a host-tested verdict on `/api/status` (wording: option
  "the checklist's own three labels" — maintainer to confirm),
  `regression_check.sh --strict` (zero warnings in both modes, reviewed
  allowlists that fail when stale; a release refuses on it, which meets the
  security/privacy acceptance criterion), the offline-queue outage
  assertion, and `integrations/ha_frigate_mqtt/up.sh`. The status-strip box
  and its parent, "Simple status language", are ticked (#1704): the strip
  is CI-compiled (the Arduino CLI and PlatformIO canary-wap builds), not yet
  seen on a device, and its label wording is still maintainer to confirm.
  Still open: the pinning box, which needs the WAP's Arduino-CLI CI rows to
  pin the core (a maintainer release decision, `firmware/PLATFORMS.md`); the
  four boxes that are their own change, now F34; and the U1 / human rest —
  the under-10-minute unbox and the Docker + Frigate end-to-end run (the two
  acceptance criteria still unmet), the per-env boot-time budget and a live
  broker-down run.
- [ ] **F22 [code+human] canary-sentinel is Phase 0** — Phase 1a landed
  (#1703), compile-gated in CI and NOT released: canary-sense's
  network/witness path carried and pinned by `check_sentinel_net_sync.sh`
  (whole files but for named product regions), a new signed `sentinel`
  canonical covering every coarse field it publishes (one golden vector
  shared by firmware and HA), MQTT events + retained state, HA discovery,
  signed pull-OTA with one product per preset env (all declared
  unpublished), the setup portal and mDNS; HA dispatches the new event
  dialect and maps the device to modality "other". Decisions — the 1a/1b
  split, copy-and-pin over a `common/net` promotion, the "other" modality,
  the per-preset OTA names — maintainer to confirm. Open: Phase 1b (the
  WiFi-RF / CSI / BLE channels and the fleet-link beacon on the C6 radio) is
  bench-bound and not built, and the README's bench checklist — 10 boxes
  now, with Phase 1a's three — is U1.
- [x] **F23 [code] canary-ota reference project lags the main trees** — done
  via the label option (its README already declared the engine promoted to
  `common/ota/`; lifting a frozen demo to parity would duplicate the
  production engine it points at). The README now opens with "teaching
  sample, not the production OTA path", states plainly that the harness
  verifies SHA256 only and must never ship devices or face untrusted
  update sources, and the stale "Phase 3 (Future)" / "Next Steps" open
  checklists are rewritten to say where every item actually landed
  (signatures + anti-rollback in `common/ota/`, the 24 h check timer in
  the canary loop, the MQTT update entity in `securacv_mqtt`). The
  `YOUR_WIFI_SSID` sdkconfig placeholders are a demo user's labeled
  edit-me fields, not shipped credentials — stated in the README rather
  than "fixed". (#1699)
- [x] **F24 [code] Emulator wave 2: first-boot captive-portal theater** —
  done: canary-display's real `net/provision.cpp` now compiles into all five
  display flavors against three shims at the silicon line — a `WebServer`
  the page's phone dials, a `WiFiUDP` socket for the captive DNS, and a
  radio with a SoftAP, an async scan and a STA join resolved against a
  staged LAN — so every route, DNS answer and verdict is the firmware's. On
  the fleet page, "meet the bird again" boots a factory-fresh unit, and a
  phone (`assets/onboard-phone.js`) joins its SoftAP, follows the captive
  redirect, frames `PORTAL_HTML` in a sandboxed srcdoc and stands in for the
  portal's inline script (the Lab's CSP forbids it) against `/scan`,
  `/join` and `/status`; every other boot stays preseeded. The framed page
  is pinned by the new `tools/gen_display_portal.py` →
  `devices/display_portal.json` (run before `gen_csp.py`), and
  `tests/onboard.test.js` / `tests/onboard_probe.mjs` gate it in
  `canary-local.yml`. Decision: the real `provision.cpp` plus a phone that
  stands in only for the portal script — maintainer to confirm. The
  chirp-fallback and live-pins waves stay roadmap in
  `canary-local/README.md` §6. (#1703)
- [x] **F25 [decision] Adopt SD tamper narration on the canary tree** —
  decided and done (option B — maintainer to confirm; a vocabulary decision
  under AGENTS.md rule 5): the canary now feeds the integrity watcher its
  storage lane's live 3-state (`sd_mount_policy::sd_state_for_tamper`, with
  a latch that tells write-failure loss from removal), so `sd_error` /
  `sd_remove` narrate here exactly as on canary-wap. The per-host kinds live
  in a new gated `spec/witness_dictionary.json` list that
  `lint_dictionary_sync.py` holds equal to the module's literals, HA's
  per-type sensors, every narration copy and each host's live feed. The
  canary's MQTT health gains `sd_mounted` (sent only after a card has
  mounted this boot; a present-but-failing card is not called removed).
  Bench (U1): a live card pull and a write failure on a canary base.
  (#1704)
- [x] **F26 [code+decision] Timeline history deeper than the witness ring.**
  F7 pages the 32-record RAM ring; the full history sits in
  `/WITNESS/records.jsonl` on the SD card, and the HTTP task never touches
  SD (the `handle_witness` contract). Decided (option B, staged —
  maintainer to confirm) and stage 1 landed (#1704): the pure, host-tested
  backward JSONL walker `firmware/common/witness/witness_history.h` (any
  chunk size, torn and corrupt lines skipped and counted, an untrusted
  O(page) resume hint re-checked before use, chain linkage checked — not
  signatures) and the bridge's design,
  `docs/design/witness_history_bridge.md`. Stage 2 — the loop-task bridge,
  the `handle_witness` extension and the timeline UI — is designed and NOT
  built: F35. Until then deep history remains the export/unseal tools' job.
  *Done (#1718):* stage 2 is F35; the design doc now reads both stages built,
  no bench pass (option B, staged — maintainer to confirm).
- [x] **F27 [code+decision] The canary tree has no Scout pairing surface** —
  done (option B — maintainer to confirm), with the premise corrected: nothing
  anywhere ever called `ble_scout_pair()` — the WAP's "PR 5c setup UI" never
  landed, so neither tree had a pair-time MAC producer. The canary now pairs
  by proximity: `POST /api/scout/pair/start` arms a window of at most 60 s
  with a label, and the first advert from an unpaired device at or above
  -45 dBm (the default) is paired inside the NimBLE scan callback, so the raw
  MAC never leaves it — the API and UI see only the keyed `hashed_id` and the
  label (window FSM `ble_scout_pairing.h`, host-tested). The registry persists
  as a versioned NVS blob written on the loop task; one lock serializes
  registry, presence tracker and window across the HTTP, NimBLE and loop
  tasks; five `FEATURE_BLE_SCAN` routes and a "Paired beacons" card. Known
  limit, documented: the owner's phone, another Canary's fleet-link advert or
  a stranger's phone held close can win the window instead, so tags are the
  reliable choice. The WAP carries the module but no route. The compile is
  CI's (`[env:full]`); a live pair is U1; the tightenings are F36. (#1704)
- [x] **F28 [code+decision] No timezone setting — day-aligned features run on
  UTC** — done (option A — maintainer to confirm): a household time zone,
  stored as a POSIX rule in NVS on the canary and canary-wap, seeded at
  setup from the phone's own IANA zone through one shared table
  (`firmware/common/time/tz_rule.h`, lifted from the display, which now
  reads it too) and applied with `setenv` + `tzset` (never `configTzTime`:
  the WAP has no SNTP). The CSI day offset, the quiet-hours minute of day,
  the WAP's NFPA-72 waking-hours gate and Chirp night mode follow household
  time once a zone is set; with none set the result is UTC, byte-for-byte
  as before (host-tested against the old formula). Both `/api/settings`
  surfaces take `tz` or `tz_iana`; an unknown zone is refused by name, and
  only rules that pass a strict POSIX grammar newlib reads to the end are
  accepted (a zone with summer time must name both change dates). The
  WAP's seed rides the companion wizard's join (its captive page is JS-free
  by design). Bench (U1): quiet hours across a real local midnight and a
  DST transition on a running device. Optional follow-ups, not items: the
  BLE provisioning path (`ble_provision`) carries no zone, and a coarse
  GPS-longitude seed could cover a device whose setup never sent one. (#1704)
- [x] **F29 [code+decision] The canary PIO tree publishes no events topic** —
  done (the csi_mqtt shape, persistence option (a) — maintainer to
  confirm): the canary now publishes its committed csi_events on
  `securacv/{id}/events` in canary-wap's exact body, built by one shared
  header both hosts call (`common/csi/src/csi_event_wire.h`, byte-identical
  to the old WAP builder for signed bodies; `signed` is now true only when a
  signature rides the body), signed with the witness key through
  `device_signature`, plus the SD/enclosure bridge to the tamper topic. Its
  MQTT health carries `public_key` so HA pins the key on first sight
  (`MQTT_BUFFER_SIZE` 1024 → 1280); the event-id floor is shared and
  host-tested in both trees, so a short boot can no longer reuse ids HA's
  replay gate would refuse; and the F9 offline queue now evicts events
  before any tamper alert (a body built while the link is down says
  `"replay":true`, which nothing in HA reads yet). pytest checks a
  canary-shaped signed body against a pinned key; the device glue's compile
  is CI's, and end-to-end delivery from a canary base is U1. Not done: the
  SD event log and reconnect backfill (F37) and MQTT-discovery triggers for
  the canary's events topic. (#1704)
- [ ] **F34 [code+decision] canary-wap enterprise boxes that are their own
  change** (from F21; each names itself in `ENTERPRISE_READINESS_TODO.md`).
  Two wait on a maintainer call: the guided factory reset — whether
  `POST /api/factory-reset` may exist at all (recommended: yes, but only
  behind the Bearer credential AND a BOOT-tap `provisioning_gate_take()`,
  with a two-step confirmation in the companion PWA; a remote credential
  alone must never wipe a witness) — and the setup profile presets — what
  "Frigate bridge mode" means (device-side or hub-side) before the three
  `/api/mqtt/config` presets and their plain-language tradeoff copy can be
  written. Two are plain code: the kernel's backend audit trail (a
  `docs/security/backend_audit.md` row per detector backend and a
  `cargo test` that every non-pure-Rust backend is feature-gated), and a
  firmware-side MQTT contract fixture (the WAP's `csi_mqtt.cpp` discovery /
  retained templates checked against the HA parsers in pytest, with QoS
  written down as the delivery bound).
- [x] **F35 [code] Build the timeline history bridge (F26 stage 2).** The
  loop-task SD read bridge `docs/design/witness_history_bridge.md` specifies
  over F26's walker: one outstanding request (a second gets
  `503 history_busy`), a 3 s bounded wait with a generation counter so a
  late completion never answers the next request, at most 4 × 1 KiB read
  per loop pass, `handle_witness` extended rather than a new route, a
  byte-offset resume hint refused when it points past the file, and rows
  badged "from card, chain-linked" — never the ring's "Verified", since no
  per-row signature is checked on the loop. Proof is CI's canary compile
  plus U1 with a card holding more than 32 records.
  *Done (#1718):* the bridge is built as specified, as a pure header
  (`firmware/common/witness/witness_history_bridge.h`, host-tested): one
  request slot (503 `history_busy`), a generation counter so a late walk never
  answers the next request, a 3 s bounded wait (504 `history_timeout`), at
  most 4 × 1 KiB per loop pass, a past-EOF hint refused before any read.
  `handle_witness` is extended (no new route); the web UI's Load More pages
  into the card, rows badged "from card, chain-linked", never "Verified".
  CI-compiled on #1718 (897182d: the canary matrix and its OTA-slot size
  guards); bench (U1): a card holding more than 32 records.
- [ ] **F36 [code+human] Tighten Scout proximity pairing (F27 follow-up).**
  The window pairs the first single qualifying advert. Two tightenings need
  real advert data first (U1): an N-advert confirmation (several qualifying
  adverts from one `hashed_id` before it pairs; N depends on real advert
  rates), and a fleet-link / Chirp payload filter so a neighboring Canary's
  advert cannot win the window (it needs the advert payload passed into
  `ble_scout_on_advert()`, which today takes only MAC, RSSI and time). The
  WAP carries the same pairing module and could adopt the route after.
- [x] **F37 [code] Canary SD event log + reconnect backfill (F29 follow-up,
  "F29b" in #1704's commits).** F29 chose persistence option (a): a
  committed event outlives a broker outage only as far as the 12-slot
  offline queue holds it. Port canary-wap's SD event log and its
  watermark-driven backfill (`csi_event_log.{h,cpp}`, SD.h-bound today) to
  this tree through an adapter that honors its loop-task single-writer SD
  rule, then replay from the card once the broker returns
  (`firmware/canary/CONSOLIDATION.md` gap row 16). Bench (U1): an outage
  longer than the queue.
  *Done (#1718):* with a card in, the canary logs every committed csi_event
  to `/EVENTS/today.ndjson` in the canary-wap's line format. Both trees now
  marshal and parse it through one header,
  `common/csi/src/csi_event_log_line.h`, which also refuses torn and glued
  lines. Once the broker returns and the offline queue has drained, the
  canary replays what the broker has not seen, in id order and a bounded
  amount per loop pass. The rules are the pure
  `common/csi/src/csi_event_backfill.h`, host-tested against a model of Home
  Assistant's replay gate:
  - no id at or below the highest one already handed over is sent;
  - a new row never overtakes an older one waiting on the card;
  - the watermark survives reboots as an NVS ceiling capped at the
    allocator's floor;
  - queued tamper alerts go first, and the tamper bridge publishes at
    commit.

  The card is touched only from the loop task, and only while storage
  reports it mounted with no mount in flight. A log is used only when
  `/EVENTS/owner` names this device's witness key, and the canary-wap now
  leaves such a card alone in turn. The failed-read give-up counts
  consecutive failures, and a whole line resets it. No broker, or a changed
  broker, drops the backlog. `firmware/scripts/check_event_egress_order.py`
  (33 self-test mutations) holds the firmware glue to what the host test
  models. Compile beyond the 2.0.17 syntax harness is CI's. Bench (U1): the
  F37 rows in `hardware_verification_checklist.md`. Found here and recorded
  as F46 and F47.
- [x] **F41 [code] canary-wap's MQTT health does not carry its tamper
  state.** `csi_mqtt::publish_health()` sends heap, uptime and the battery;
  `enclosure_open` and `sd_mounted` reach only the HTTP `/api/health`
  document. HA's per-type WAP tamper sensors set from F29's tamper-topic
  bridge are therefore re-cleared by the next health publish. Pre-existing;
  found by the fw-events package in #1704. Carry both fields in the MQTT
  health (the canary tree already does) or stop the health from clearing a
  tamper-type sensor. F29's "MQTT-discovery triggers for the canary events
  topic" clause is the same surface and could land with it.
  *Done (#1718):* `csi_mqtt::publish_health()` carries `sd_mounted` (once a
  card has mounted this boot) and `enclosure_open` (once the contact is
  adopted) in the canary tree's names; the worst-case body is 323 bytes,
  inside the 384-byte buffer. HA needed no change:
  `tests/test_wap_tamper_health.py` reproduces the re-clear against the old
  firmware's keys. Still open (maintainer's call, in
  `docs/homeassistant_setup.md`): the WAP health carries no level for
  `sd_error`, watchdog, power-loss, unexpected-reboot or `tamper_detected`, so
  those sensors still clear at the next publish. The MQTT-discovery triggers
  clause of F29 was not attempted. Bench (U1): a lid opening and an SD pull
  held across two publishes.
- [x] **F42 [code] The Tier-1 secure build is not buildable as written.**
  `firmware/provisioning/platformio_secure.ini` `[env:secure]` replaces
  the canary `[env]` build flags instead of extending `${env.build_flags}`,
  so it loses the include paths the witness and crypto sources need, and no
  CI job builds it (the file says so; fw-keys recorded it in #1704 rather
  than fixing it). `partitions_secure.csv` also flags the `nvs` partition
  `encrypted`; ESP-IDF's NVS is protected by NVS encryption (`nvs_keys`),
  not by flash encryption, so on a fused board that flag leaves NVS
  unreadable. Fix both, and add a compile-only CI leg so it cannot rot
  again. Bench (U1): K1/K2 of the hardware checklist on a fused board. The
  key-at-rest decision itself is F38.
  *Done (#1718):* both secure envs extend `${env.build_flags}` and resolve
  through `firmware/canary/platformio.ini`'s `extra_configs`;
  `partitions_secure.csv` drops `encrypted` from `nvs` (ESP-IDF 4.4.7: "The
  nvs partition cannot be encrypted"); `main.cpp`'s always-used includes moved
  above the `FEATURE_CSI` gate (which also fixes `[env:minimal]`).
  `firmware.yml`'s canary leg compiles both envs, and `regression_check.sh`
  refuses an encrypted `nvs` row. The key still sits in plaintext NVS under
  `framework = arduino` (F5, F38). Bench (U1): K1/K2 on a fused board.
- [x] **F43 [code] The dash display's join screen draws its caption over
  the QR.** On the 800×480 dash flavor, the first-boot join scene's "or join
  … password" caption crosses the QR code's lower edge (`onboard_ui`
  layout). Seen in the emulator's preview of the real firmware by the
  fw-emu-portal package (#1703), so it is visible without hardware; a phone
  may still read the code, but the layout is wrong. Re-check every display
  flavor's join scene once fixed.
  *Done (#1718):* the Join scene is stacked from the panel size and the
  labels' line heights by a pure header (`include/canary/ui/onboard_layout.h`)
  — centered on rectangular glass, inside the chord band on round glass, and
  on a short window only the QR canvas shrinks. The re-check of every emulated
  flavor found the round watch (network-name line 7 px inside the card) and
  the AMOLED 2.41 (captions overlapping by 2 px) had the same defect; the same
  stack fixes both. Guards: `tests_host/test_onboard_layout.cpp` (every
  display env's panel) and a framebuffer check in `onboard_probe.mjs`; the
  emulator dist is rebuilt. Bench (U1): the panels themselves, and a phone
  scanning the watch's smaller card.

- [x] **F45 [code] The nightstand join screen cuts off the only text way in.**
  On the 172 px nightstand (and the C6 and the nightlight at 180 px), the
  joined credentials line "SecuraCV-XXXX  •  <password>" (172 px of text in a
  156 px row) and the stuck-phone hint (207 px) end in an ellipsis. When the
  QR fails, that text is the only way to join, so this glass dead-ends.
  Pre-existing; found by F43's re-check of every flavor (#1718). Split small
  rectangular glass the way round glass already is (network name on one
  line, password or hint on the next) when the joined line is wider than
  its row, through the same `onboard_layout.h` stack and its host test.
  *Done (#1718):* `join_lines()` keeps the joined line only where it fits its
  row. It measures the way LVGL lays a line out, kerning included, from a
  generated metrics header (`gen_montserrat_metrics.py`, `--check`ed in CI).
  Otherwise it splits the line into the name, then `pass  <key>`. Each row
  tries shorter forms before a smaller face, and none ever ends in an
  ellipsis. The name and key stay on the glass with and without the QR or
  the stuck-phone hint, and the hint gets a row of its own. On the round
  watch that row is the title's band, a visible change. Three more cuts
  were fixed by the same measure. The host test runs every display env with
  both type ladders, using the widest name and key the minting alphabet can
  make. `onboard_probe.mjs` fails on an ellipsis and reads the firmware's
  own labels, the hint included (4242eb7). The emulator dist is rebuilt.
  Bench (U1): real 172/180 px glass. Found here: F50.
- [ ] **F46 [code] The CSI bundler's event ids live outside the chokepoint
  id space.** Found by F37 (#1718). Rows that pass through the CSI bundler
  (presence, and the system.integrity tampers, since they carry a state)
  take ids from `common/csi/src/csi_bundler.cpp`'s own space, 0x80000000
  upward. No floor covers that space, it restarts every boot, and it
  commits in bundle-close order. Home Assistant's replay gate therefore
  already refuses many canary events live: the chokepoint ids after any
  bundle, and every bundle id after a reboot until the new boot passes the
  old ids. F29's "no reused ids" holds only for chokepoint ids. The backfill
  mirrors the gate and skips those rows rather than send ids it would
  refuse, so F37 does little across reboots until there is one id space.
  Also, the first bundled row handed over moves `csi.evsent` and the
  watermark into the bundler's space for good, so on that device the
  backfill never sends a chokepoint-id row again
  (`test_one_bundled_row_moves_the_watermark_for_good` pins today's
  behavior). Recommended: allocate bundle ids from the chokepoint allocator
  at commit time, in both trees and the open-row display. The fix must
  reset or migrate `csi.evsent` and Home Assistant's stored mark.
- [ ] **F47 [code] canary-wap's backfill watermark lives in RAM.** Found by
  F37 (#1718). Its first reconnect after every boot replays up to 64 ids
  Home Assistant refuses. It could adopt the canary's
  `common/csi/src/csi_event_backfill.h` (NVS ceiling, id-floor cap). A
  smaller one found in the same review: when an unbuildable row is the
  last whole line before a power cut's torn tail, the canary's walk stays
  pending and re-reads the fragment about once per loop pass until the next
  committed row seals it. Nothing is lost, and the next row goes out.
- [ ] **F48 [code+decision] canary-wap's mesh crypto and its interop with the
  PIO tree.** Found by F33 (#1718). canary-wap's AUTH exchange still runs
  X25519 over long-term Ed25519 keys, the bug class F33 part 2 fixed for
  pairing, and its rotation encrypts under those session keys. canary-wap
  also HKDFs the pairing key where the PIO tree and spec §5.3 use it raw,
  and it numbers pairing frames differently, so the two trees cannot pair
  with each other. Concurrent removals on canary-wap cannot converge without
  a wire change: `MSG_OPERA_REKEY` names no removed device and has no
  announcement phase. Crypto review and a wire decision first, then code in
  both trees and a cross-tree host test.
- [ ] **F49 [code] Mesh leftovers from F33.** (1) The canary's health-log
  list passes `millis()` to `formatTimestamp`, the same uptime-as-time-of-day
  rendering F33 part 7 fixed for alerts. (2) The joiner side's
  `CodeReadyCallback` never fires: the code arrives on `SEND_ACCEPT`, and
  dispatch reports only `NOTIFY_CODE_READY`. The web UI reads the code from
  `GET /api/mesh`, so no user sees it today. (3) There is no radio-MAC
  learning from opera frames, so a changed MAC means a re-pair. (4) Some
  PIO residual splits remain: both initiators already handed out, a mutual
  removal, or a lost ACK. A random-loss probe split 3 of 60 runs at 5%
  frame loss (spec §5.6 states it).
- [ ] **F50 [code] The display's other join hints still cut on narrow glass.**
  Found by F45 (#1718). The Fail-stage hints from `join_failure_hint` measure
  175-219 px at 12 px ("your router may be out of addresses" is 219), so
  they are cut on the round watch's 142 px band and on the 156/164 px
  portrait rows. The PhoneJoined hint ("no page? open 192.168.4.1") is
  182 px under Heirloom on those rows. Carry them through `join_lines()` /
  `fit_line()` with narrow forms. Also, on the round watch's no-QR path,
  the title band appears to overlap the top of the bird, inferred from the
  numbers only. The emulator always renders the QR, so it was not seen.
- [x] **F51 [code] The airtime governor's window lost sends above 25.6 a
  second, and a saturating probe starved the heartbeat.** Found reconciling
  F4 (#1696) on the host. `airtime_governor.cpp`'s 256-slot ring held sends,
  not time, so a caller reserving faster than 256 per 10 s window overwrote
  in-window airtime and the 2 % cap silently stopped holding (200 Hz × 16 B:
  allowed in full, read 0.82 %, true 6.41 %). F4's probe gate held only
  because its framed 75 B frame (792 µs) clears the 781 µs-per-slot line by
  1.4 %; a 3 % cap did not (12.7 % true, 2.0 % read). And a probe asking
  for more than the cap took every microsecond the window freed: the 30 s
  heartbeat and 60 s chirp presence were refused 9 of 9 times in 180 s at
  160 frames/s. On the DEV profile, or a boot where the mesh did not
  initialize (safe mode, ESP-NOW refused), nothing initialized the
  governor, so every probe reservation passed.
  *Done (#1722):* the ring holds 100 ms buckets, so nothing in the window is
  ever overwritten; the window reads 10.0–10.1 s, erring toward denial, and
  a reader whose clock trails the newest send (the MQTT telemetry) still
  counts every send before its time. The probe's gate moved to
  `probe_airtime.h`: it starts no frame once the governor's window reads
  1.60 % (one peer at 20 Hz stays steady; the probe stops within one
  792 µs frame of the line, so about 0.39 % stays for the heartbeat,
  presence and the Beacon self-test), and it brings the governor up on a
  boot without the mesh. Host-tested (`test_mesh_coexistence`,
  `test_csi_probe_airtime`, which pins the ceiling at exactly 1.60 % from
  both sides); the device build is CI's (firmware.yml's WAP Arduino legs).
  Latent on shipped devices: the WAP's probe table is empty, so it
  broadcasts at 10 Hz, ~0.8 % by the governor's estimate. Still open:
  framing is added only for the probe (F54); the 1.60 % ceiling is also the
  Beacon's `airtime_saturated` trouble line (`beacon_channel.cpp`,
  > 160 x100), so the two move together or not at all; the 32-slot Beacon
  telemetry ring keeps the per-send shape (it gates nothing); with a filled
  probe table an over-budget probe arrives in bursts (host-measured about
  1.3 s of frames per 10 s window at 8 peers), which is roadmap section 5
  step 5's call (`aggregate_cap_hz`); and no bench has checked the
  192 µs + 8 µs/B estimate against real air (U1).
- [ ] **F54 [code] The airtime governor charges the mesh, chirp and beacon
  callers for their payload only.** Found reconciling F4 and F51. The
  probe's gate adds the ~59 B of ESP-NOW MAC and action-frame framing
  (`PROBE_FRAME_OVERHEAD_BYTES` in `probe_airtime.h`); every other
  caller passes header plus payload. The mesh also charges a heartbeat,
  tamper or power broadcast once, while `broadcast_message` unicasts it, its
  64 B signature included, to every connected peer (`mesh_network.cpp`; only
  offline-imminent multiplies by the peer count). So the window under-reads
  every caller but the probe. — in progress (2026-09-23)

---

## 2. Apps (desktop Flasher, Lab, iOS, tvOS)

- [x] **A1 [code] tvOS has no timeline view.** iOS has the parity-proven
  `TimelineScrubView` + `viewer/timeline_core.js`; `tvos/` has zero
  timeline/scrub code. Port the shared core to the Wall. *Done:* the model
  itself ships to tvOS, not a copy — `ios/Shared/TimelineScrub.swift` (with
  `EventVocabulary.swift` and `AlertRecord.swift`) carries the
  SecuraCV-Parity marker and compiles into the Wall (`lint_apple_parity`
  now covers 12 files), and its new `records(fromSealedPayloads:)` is a
  line-for-line port of the viewer's `normalizeEnvelope`, pinned by a new
  `normalization` section in `viewer/fixtures/timeline/scrub_parity.json`
  (`gen_timeline_parity.mjs`, asserted in JS and Swift). It also corrected
  a premise three places carried: sealed-log entries do carry time — every
  payload embeds its coarse `time_bucket`. The Wall
  (`WallTimelineView.swift`) draws the day shape only from a log it walked
  against its pinned key (A2) and clears it with every other verdict; the
  Siri Remote moves through lit buckets, and every "when" is a bucket range
  in the TV's clock, never an instant — the details strip prints each
  record's own sealed bucket, so +5:45 zones and 25-hour days read right.
  The iPhone's view still labels cells from the local-midnight grid: A17.
  All Swift is CI-only (tvos.yml "Witness Wall (simulator build + unit
  tests)", ios-selfheal.yml "selfheal"). (#1704)
- [x] **A2 [code+decision] tvOS verification is structurally dark.**
  `WallCanary.swift` hard-codes `allVerified = false`; no pairing ceremony
  pins a key and the TV sends no sealed-log token (`tvos/README.md`). Design
  the pairing/token flow (decision), then wire it (code). *Done:* option (a)
  — maintainer to confirm: a new long-lived, route-scoped viewer token. The
  operator mints it once (`witness_api mint-viewer-token --label <room>
  [--base-url <url>]`, or `entrypoint.sh mint-viewer-token` in the Docker
  sidecar; `revoke-viewer-token <id>` takes effect on the next request).
  The kernel keeps only its sha256 in `viewer_tokens.json` (0600, read per
  request), compares in constant time, and honors it on exactly one route,
  `GET /api/sealed-log`, served by the same writer as the capability path;
  anywhere else it is a bad token that counts toward the auth lockout, and
  a viewer read never clears that lockout. The mint prints a one-line
  pairing receipt with the kernel's current verifying key; the Apple TV's
  settings take it, keep token and pin as one Keychain item per source,
  and send the bearer only to that source, never across a redirect. The
  Wall reads "Verified through <this TV's receipt time>" only when the
  pinned key signed a walk that passed and checked at least one signature
  (an empty tail reads "Paired · nothing sealed to check"); a log signed
  by any other key is an alarm, a refused token a warning, and
  `allVerified` lights for the verified standing alone. The docs say what
  "Verified" does not prove — that the served tail is current or complete;
  a signed head in the document is named as roadmap work in
  `tvos/discovery/DISCOVERY.md`. The kernel half has Rust tests and a
  mint/read/revoke step in the sidecar e2e (docker-sidecar.yml, CI-only);
  all Swift is CI-only (tvos.yml), and Keychain custody on a signed Apple
  TV is a human pass. (#1704)
- [x] **A3 [decision] Wall-reachable sidecar bind/port** —
  `tvos/discovery/DISCOVERY.md` "a bind and port decision not yet made";
  Docker-sidecar users cannot reach the Wall at all. *Done:* option (3) —
  maintainer to confirm: an explicit opt-in. `SECURACV_API_BIND=loopback|all`
  (default loopback) in the sidecar entrypoint; `all` binds `0.0.0.0:8799`
  and exports the kernel's cleartext acknowledgment with it (the two flip
  together, or the kernel refuses to start). The image still `EXPOSE`s
  nothing: the owner adds the port mapping, which both compose quickstarts
  carry as comments, and `docs/frigate_integration.md` walks through it.
  A render test and a LAN-mode step in the sidecar e2e (docker-sidecar.yml,
  CI-only) cover it. (#1704)
- [x] **A4 [code] Lab native serial is a Phase-2 stub** — the seam's named
  content is done: `list_serial_ports()` is real (the `serialport` crate,
  field-for-field the Flasher's `PortDto` so a port picker written against
  either app reads the other's answer unchanged; registered on desktop and
  mobile handlers; `cargo check`/`clippy`/tests green; `libudev-dev` added
  to the Lab's Linux release deps, which would otherwise have broken the
  next release build). The capability stays honest: `serial` remains false
  — it means "native flash path present", and that path (the Flasher's
  espflash sidecar) is A14 below — while the new `serial_list: true`
  advertises what does exist. `notifications:false` is untouched (its own
  feature, not this seam). (#1700)
- [ ] **A5 [code] Lab mDNS + BLE discovery** (HTTP-poll only today) and the
  menubar companion with the signed timeline — `desktop-lab/README.md`
  Roadmap items 2–3. *Partial — mDNS and the companion landed (#1704):*
  `desktop-lab/src-tauri/src/fleet.rs` is a desktop-only twin of the
  Flasher's `fleet_scan` (browse only; `desktop_parity` holds
  `SERVICE_TYPE`, `FleetSighting`, `fleet_scan` and `scan_blocking` equal),
  and both apps' Witness Walls now feed browsed boards to the `/api/fleet`
  poll after the kernel addresses (a board answers with a one-board
  self-report, so trying it first would replace the kernel's fleet),
  saying "N Canaries announced on this network — none serves the fleet
  document yet" when boards announce but nothing serves it; `MOBILE.md` no
  longer claims iOS mDNS/BLE discovery. The companion (`companion.rs`) is a
  Rust-driven tray ("N of M online") with native notifications on fleet
  changes — coarse words from the fleet's own report only, never
  "verified", never sealed-log or wellbeing content, and silence never
  notifies — so the `notifications` capability is now true on desktop. On
  macOS it keeps running after the window closes; on Linux it runs while
  the Lab is open (the .deb now depends on libayatana-appindicator3-1, and
  a missing tray library no longer crashes the launch). A browse that
  finds a real Canary, the tray on a real Linux desktop and macOS menu bar,
  and notifications from an unsigned macOS bundle are human passes.
  *Still open:* BLE discovery (skipped: it needs a Mac, for the Info.plist
  Bluetooth entry, and a BlueZ desktop to validate; the shared beacon
  vector fixture and btleplug seam are their own PR) and the companion's
  signed timeline (skipped: it needs a viewer token and a pinned key, A2's
  pairing — without them the only honest render is the unverified fleet
  report the companion already gives).
- [x] **A6 [code] iOS Unseal screen is an empty placeholder**
  (`ios/Sources/SecuraCV/Views/KeysView.swift`); the crypto exists repo-side
  in `tools/unseal_snapshot.py`. Build the import + decrypt flow. *Done:*
  the Keys tab's Unseal screen (`UnsealView.swift`) runs the whole loop. It
  creates the X25519 snapshot key on the phone (Keychain, ThisDeviceOnly,
  no export path), hands only the public half to a paired canary-wap
  (`POST /api/vault/key`), shows per Canary whether it holds this phone's
  key, lists the sealed frames, and pulls and opens them; it also opens a
  `.svlt` from Files, Mail or AirDrop (`com.securacv.svlt` is exported and
  claimed). A frame is shown once, full screen, with its trigger and
  ten-minute bucket, and discarded on Done, on disappear and when the app
  leaves the foreground — never written to disk, Photos, the pasteboard or
  iCloud; there is no share button. `SnapshotVault.swift` ports the format
  to CryptoKit, pinned byte for byte to the Python reference by a new
  generated fixture (`tools/fixtures/vault/svlt_parity.json` from
  `tools/gen_svlt_parity.py`, `--check` in firmware.yml): the Swift tests
  open it and re-seal it to identical bytes. A draft rebuilt the AAD from
  parsed fields, which would have let a flipped reserved header byte open;
  the file's own header bytes are authenticated, and the Python and Swift
  tests both pin that case. "Sealed snapshot" and the kernel's quorum vault
  are kept apart in the copy, the RFC's guardrail, the iOS README and the
  glossary. All Swift is CI-only (ios-selfheal.yml "selfheal"); the
  on-device pass is A15. (#1703)
- [x] **A7 [code+decision] Secure-Enclave-backed key custody on iOS** —
  named a roadmap item in `ios/README.md`; today keys are Keychain
  generic-password items. *Done:* option (1) — maintainer to confirm. The
  Secure Enclave holds only P-256 keys, so it wraps the X25519 snapshot key
  rather than holding it (`EnclaveCustody.swift`: a versioned envelope,
  ephemeral P-256 ECDH → HKDF-SHA256 → AES-GCM), and only an Enclave key
  created with `.userPresence` can unwrap the Keychain item. Wrapping, key
  creation and the in-place migration of an existing raw key never prompt;
  an unseal runs every public-key check first (a file sealed to another
  key never asks), then asks for Face ID, Touch ID or the passcode once.
  Without an Enclave or a passcode, and on the simulator, a software
  wrapper of the same envelope is used and the Keys tab labels the custody
  as such; the custody kind is the Keychain record's first byte, so it
  cannot disagree with the blob. Removing the passcode disables an
  Enclave-wrapped key, and the docs say so. Only the snapshot key is
  wrapped: tokens stay plain Keychain items by design (they are read on
  every poll). After the Codex review on #1703, `Keychain.set` updates an
  existing item in place instead of deleting it first, so a failed wrap
  during the migration keeps the raw key instead of erasing its only copy.
  CI type-checks the Enclave branch but can run only the software one (the
  simulator has no Secure Enclave); the device pass is A15. (#1703)
- [ ] **A8 [human-gated by U3] Exercise the CloudKit household/away path.**
  `HouseholdShare.swift` degrades to a no-op in unsigned builds, so CI has
  never touched iCloud. Needs a signed build (U3), then a test pass.
- [ ] **A9 [human] Windows enablement.** The hub-io raw-disk write backend is
  complete but staged off pending a VM/hardware pass
  (`desktop/hub-io/src/write.rs`, "STAGED — NOT YET ENABLED"), and
  `secret_store.rs`'s Windows branch has never been compiled by CI.
- [x] **A10 [code] Linux secret-store backend** — currently "none": profile
  passwords and bearer tokens land in a plaintext prefs file
  (`desktop/src-tauri/src/secret_store.rs`). The frontend discloses it; fix it
  anyway (libsecret/keyring). *Done:* option 1 — maintainer to confirm. The
  Flasher reaches the freedesktop Secret Service through keyring 3's
  synchronous backend with keyring's own `crypto-rust` feature, which is
  what makes the session encrypted (a draft that set the feature on the
  backend crate directly got a plain one). No new `-sys` crate or build
  package — libdbus-1 was already linked; the .deb now names
  `libdbus-1-3` as a runtime dependency. The backend is probed once per
  launch and fails closed to "none" on a headless or minimal desktop, so
  the consent note never names a store that isn't there, and store calls
  run on tauri's blocking pool because a locked keyring's unlock prompt
  waits until answered. Secrets a no-store session left in prefs move
  into the keyring on the next launch that finds one, in one pass beside
  the launch restore, never holding it up (a locked keyring asks once, as
  `desktop/INSTALL.md` now says). The live Secret Service branch and the
  macOS/Windows branches have no CI; a pass on a Linux desktop with
  gnome-keyring or KWallet is human work. (#1704)
- [x] **A11 [code] Native Flasher hardcodes what the browser derives.** (#1701)
  `canary-local/tests/desktop_parity.test.js` header: chip tables, USB IDs and
  the release host are literals in the native app, kept in sync only by CI.
  Make native read the embedded catalog; delete the matching assertions.
  *Done:* `chip_table()`/`canonical_chip()`, `release_origin()` (lib.rs) and
  `usb_ids()` (we2.rs) now derive from `EMBEDDED_CATALOG`; origin and USB ids
  fail closed like `manifest_url_allowed`, while chip naming stays broader
  than the catalog (the chip token is extracted from espflash's own output,
  catalog spellings win by exact match, non-catalog ESP32 variants keep
  their real names) so the catalog-independent rescue/local-file operations
  keep working — a Codex review caught that a catalog-only table misnamed
  ESP32-S2/C2/H2 as bare ESP32, which would have defeated the flash chip
  guard. The three parity assertions were rewritten rather
  than deleted — they now guard that the derivation stays (no retyped copy
  creeps back) and that the catalog fields native parses stay well-formed,
  since native fails closed silently on a malformed field. `MODEL_ADDR` and
  `DEV_FLASH_MANIFEST_URL` stay deliberate constants, still diffed by the
  test.
- [x] **A12 [code] Desktop Flasher lacks the eFuse-read diagnostic** the
  browser flasher has (espflash has no fuse-read; the parity test currently
  forces a "browser-only" disclosure). Needs an espflash upstream check or a
  raw-command implementation — investigate, then either implement or record
  why not beside the disclosure.
  *Investigated, not implemented (#1718):* the pinned espflash 3.3.0 CLI has
  no register, eFuse or security-info read (`board-info` prints no security
  field; the library declares `GET_SECURITY_INFO` but never sends it). The
  reason is recorded beside the parity test's browser-only disclosure and tied
  to the pin. espflash 4.x's `board-info` prints part of it (not
  `SECURE_VERSION` or `DIS_DOWNLOAD_MANUAL_ENCRYPT`), so the item reopens with
  an espflash bump, which needs a bench flash per board.
- [ ] **A13 [human-gated by U3/certs] macOS signing/notarization** — both Mac
  apps ship unsigned until `ENABLE_MACOS_SIGNING` + certs exist
  (`desktop-lab/README.md`, `desktop/INSTALL.md`).
- [x] **A14 [code] Port the Flasher's native flash engine to the Lab.** The
  A4 seam now lists ports; actually flashing needs the Flasher's `espflash`
  sidecar pipeline brought into `desktop-lab` (bundle the sidecar per
  target triple in tauri.conf + release workflow — read
  `.github/RELEASE_LESSONS.md` first, this is app-bundling work — port the
  flash/monitor commands from `desktop/src-tauri`, then give the Lab's
  flash page the native path behind `native_capabilities().serial`, which
  flips true only then). The Lab wraps the browser flasher's pages, and
  Tauri's webview has no Web Serial — this is "the stated reason the
  native Lab exists" from A4, sized as its own PR.
  *Done:* option (c) — maintainer to confirm: one shared engine rather
  than a text twin. Step 0 gave the Lab crate a PR compile/lint/test job
  (`desktop-lab-check.yml`), so the rest goes red on the PR, not the
  release. `desktop/flash-engine` is a new tauri-free crate holding the
  Flasher's whole flash path — catalog guards, release size/SHA-256/
  signature checks, NVS provisioning, the change map, intake checks,
  espflash argv, the write pipeline and the boot-receipt serial monitor —
  behind a two-method seam (an event sink and the app's espflash spawn).
  The Flasher is re-pointed at it with the same command names, arguments,
  events, error strings and argv; its tests moved with the code, none lost,
  plus an in-memory-host suite for the pipeline. The Lab gains the
  Flasher's seven flash commands as thin wrappers held equal by text in
  `desktop_parity`, with espflash spawned from Rust only (no shell grant)
  and killed on a normal quit. The Lab release bundles the Flasher's
  pinned, sha256-checked espflash 3.3.0 (a test holds the pins equal, and
  both workflows now assert the macOS universal binary has both slices),
  with `externalBin` scoped to the macOS and Linux configs so the iPad
  shell still builds; the .deb installs the udev/ModemManager rule under
  its own path (`61-securacv-lab.rules`), since dpkg refuses two packages
  owning one file. The Flash page mounts a native bench
  (`canary-local/assets/flash-native.js`) with the Flasher's diagnostics
  verbatim and the browser's own provisioning form.
  `native_capabilities().serial` is true on macOS and Linux only while the
  bundled espflash is really there; an app that can't flash shows an
  in-app card saying USB flashing isn't available on this device, never
  the website's get-Chrome card. After review, the engine reads a
  webview-supplied safety copy only through a validated path (an
  absolute, canonical `.bin` regular file, size-capped) in both apps, and
  the bench's success line credits the release-signature check to the
  app, never to the chip.
  Every Lab surface that says what it fetches now says the Flash page asks
  which signed firmware is published once a board is connected, and
  downloads it when Flash is pressed. The bench leaves to the Flasher, and
  says so on its card: the safety copy and change map, room presets, the
  minted API token and fleet book, the rescue bench, local-file install,
  the Vision module burn, and reaping espflash after a force quit. Human
  work: a real flash and boot receipt from the Lab on macOS and Linux, one
  Flasher flash after the refactor, and a "Desktop app release" run with
  publish:false before any Lab release. The Lab's RELEASE_NOTES section
  lands with its next version bump. (#1704)
- [ ] **A15 [human] On-device pass for the iPhone snapshot flow (A6, A7).**
  CI builds unsigned and runs the simulator, which has no Secure Enclave,
  so neither item has run on a phone. On a physical iPhone with a passcode:
  create a key and see "Secure Enclave" custody; one Face ID or passcode
  prompt per unseal, and canceling it leaves no frame; a file sealed to
  another key raises no prompt; forgetting the key deletes the Enclave
  key; removing the passcode disables the key, as documented. Then a real
  canary-wap seal followed by an unseal on the phone, a `.svlt` opened from
  Files or Mail, and the app-switcher snapshot blank while a frame is up.
- [x] **A16 [code] Mirror the Wall's `canary.local:8799` probe on the other
  walls.** A2 added it to the Apple TV's well-known candidates only; the
  desktop Flasher's wall, the Lab's `witness-host.js` / `tv-emulator.js`
  and the website's `tv/app.js` do not probe it yet. The website test pins
  its two lists against each other, so this is a cross-repo change.
  *Done (#1718, website #203):* every wall — the Flasher, the Lab host and
  companion defaults, and the website's `tv/app.js` / `js/tv-emulator.js`
  (re-vendored into both apps) — probes `canary.local:8099`, `:8799`, then
  `canary.local`, in the Apple TV's order; `desktop_parity.test.js` and a new
  test pin them all to the Swift literal, and the website test pins its two
  lists. Not done (maintainer's call): probing a provisioned or typed host on
  `:8799`.
- [x] **A17 [code] The iPhone's timeline labels cells from the local grid.**
  `ios/Sources/SecuraCV/Views/Components/TimelineScrubView.swift` (~:47,
  the day's audio-graph clock) prints `dayT0 + cell × bucket`, the rule the
  Wall had until its review fix: in zones whose offset is not a multiple of
  the bucket (+5:45, +5:30) and on 25-hour days that is not the record's
  sealed bucket. Port the Wall's rule — print each record's own sealed
  bucket (`WallTimeline.sealedBuckets`).
  *Done (#1718):* `TimelineCellBuckets` in `TimelineScrubView.swift` labels a
  lit cell with its records' own buckets (oldest first) and only an empty cell
  with its slot; tap/drag now scroll to the cell's newest record.
  `TimelineCellBucketsTests.swift` covers +5:45, +5:30 and a 25-hour day.
  Wording says "own bucket", not "sealed" (the ribbon's alert notebook makes
  no sealing claim). Swift is CI-only (iOS self-heal job).
- [ ] **A18 [code+decision] The Wall cannot walk an add-on or root-image
  install.** The Home Assistant add-on honors `/config/viewer_tokens.json`
  but has no control that mints a viewer token (running `witness_api`
  inside the add-on needs its config and seed, which the Supervisor offers
  no path to), and the root `witnessd` container image ships no
  `witness_api` binary. Both are documented limits: those installs show
  the Wall the roll-call, never a walk. Decide where minting lives, then
  build it.
- [ ] **A19 [decision] The kernel's auth lockout also closes `/api/fleet`
  and `/health`.** The per-address lockout is checked before routing, so a
  tokenless poller that trips it locks itself out of the roll-call too. The
  Wall now asks a refusing source once per session and stays clear of it;
  other tokenless pollers still can lock themselves out. Decide whether the
  lockout should cover the unauthenticated routes.
- [x] **A20 [code] A bundled espflash that cannot run reads as "unknown"
  in both flashers.** When the sidecar is present but cannot execute (the
  wrong CPU, a missing loader), `identify()` in the Flasher
  (`desktop/src/app.js`) and in the Lab (`canary-local/assets/flash-native.js`)
  reads the spawn failure as `unknown` and coaches download mode; only the
  Details text names the cause. Give the spawn failure its own kind and
  words in both frontends (two flashers, two frontends — CLAUDE.md). From
  the rust-flash package's open items (#1704).
  *Done (#1718):* a new `engine` kind in both frontends' error classifier
  (`desktop/src/app.js`, `canary-local/assets/flash-native.js`), checked
  first: "The app's flash engine couldn't start", with the system's reason
  (`spawnReason()`) and no driver or download-mode coaching. `desktop_parity`
  builds the errors from the Rust that produces them. A real spawn failure has
  not been seen on a machine (bench).
- [x] **A21 [code] The espflash pins do not mark either app as changed.** The
  bundled espflash's version and sha256 live only in
  `desktop-release.yml` and `desktop-flasher-release.yml`, and neither app's
  `.github/release-targets.yml` watch names them, so a pin bump alone
  releases nothing. One pins file both workflows read and both watches name
  closes it (`.github/RELEASE_LESSONS.md` 2026-09-23 (b) records the lesson).
  *Done (#1718):* `.github/espflash-pins.env` holds the version and the three
  sha256 pins once; both release workflows load it with a parse-don't-source
  step that fails on a malformed line, and both apps' `release-targets.yml`
  watches name it. `desktop_parity` refuses a pin inside either workflow. The
  load step has not yet run on a release runner — a build-only dispatch of
  each workflow proves it.
- [x] **A22 [code] The desktop Flasher's crate fails clippy and fmt, and no
  PR job runs either.** `desktop/src-tauri` fails
  `cargo clippy --all-targets -- -D warnings` on two findings, both in files
  #1704 did not change: `whoami.rs`'s `decode_hex` trips
  `manual_is_multiple_of`, and `sscma.rs`'s `clamp_threshold` is dead
  outside its own tests (use it or delete it). `cargo fmt --check` fails in
  `fleet.rs`, `hub.rs`, `lib.rs` and `whoami.rs`, all of it older than
  #1704. The Lab crate (`desktop-lab-check.yml`) and
  `hub-core`/`hub-io`/`flash-engine` (the `desktop-hub-core.yml` matrix)
  run `cargo fmt --check` and clippy with `-D warnings`; the Flasher's
  `tauri-crate-check` job runs only `cargo check` and `cargo test`, so the
  next Flasher change meets the same red lints and no PR job says so. Fix
  the two findings, format the crate in a commit of its own, then add the
  Lab job's Format check and Clippy steps to `tauri-crate-check`. From the
  rust-apps and rust-flash packages' open items (#1704).
  *Done (#1718):* `desktop/src-tauri` is `cargo fmt`- and `clippy -D
  warnings`-clean on rustc 1.98.1 (`is_multiple_of`; the dead
  `clamp_threshold` and its test removed); `desktop-hub-core.yml`'s `cargo
  check (src-tauri)` job now runs a format check and clippy.
- [x] **A23 [code] The hub's provisioning executor and host runner accept
  paths they cannot mean.** `canary-local/tools/hub_seed_apply.py`'s
  `under_root()` joined a plan's `requires_files` under `--files-root` with
  `..` segments intact, so `/ssl/../../etc/passwd` was looked up outside the
  root and reported present. A step's `dest` and `source` were never
  checked, so a plan path could name a file outside the root it is joined
  under. `canary-local/tools/hub_host_provision.sh` spliced
  `SECURACV_HOST_SSL_DIR` into `docker -v <src>:/ssl:ro` after only a `-d`
  test, so a `:` changed the mount spec and a relative value became a named
  volume. Hygiene, not a boundary: the plan rides the same read-only mount
  as the executor. The Flasher embeds both
  (`desktop/hub-io/src/provision.rs`), but its release watch
  (`.github/release-targets.yml`) did not name them. From the wave-7
  security sweep (its path-hygiene findings, not this file's F7 or F8).
  *Done (#1722):* the executor refuses such a plan before it observes
  anything, naming the step, key and path (exit 2). Every step is checked,
  whether or not its feature is enabled. The runner mounts only an absolute
  directory with no `:`, and states the rule beside the override. The
  bundle's two pins moved (executor, host runner). The Flasher's watch
  names the five embedded files, and `test_release_plan.py` holds every
  file a desktop app's path-linked crates embed, and each such crate, to
  its watch. That test runs at PR time only on a `.github` change until
  `workflows-lint.yml`'s path lists name the desktop crates. Host-tested;
  not run on a hub.
- [x] **A24 [code] Both apps' Witness Wall emulator still reads a silent
  `online` as present.** Found by W20 (website #199), which fixed the
  canonical `js/tv-emulator.js` and `witness-wall.html` after the apps last
  vendored them. `desktop/src/witness/tv-emulator.js` and
  `canary-local/witness/tv-emulator.js` still write `online: … !== false` at
  :416, :436 and :490, against `tvos/discovery/DISCOVERY.md` ("`online`
  defaults to `false`"), and both `witness.html` copies still say the real
  fleet brings "chain health". `PROVENANCE.txt` records 963aa8f6… and
  0ff5397e…, but the website's main hashes 425e9c80… and 0d34d6fa….
  `scripts/check_witness_emulator_sync.sh` compares only the two app copies,
  so nothing fails. Re-run `scripts/vendor_witness_emulator.sh` against a
  website checkout at or after a8b5e1f and commit both copies.
  *Done (#1720):* re-vendored from the website at a8b5e1f with
  `scripts/vendor_witness_emulator.sh`. Both `PROVENANCE.txt` files record
  the new hashes (`witness-wall.html` 0d34d6fa…, `js/tv-emulator.js`
  425e9c80…); `highlight.js` and `demo-fleet.json` are byte-unchanged. The
  three fleet-row `online` derivations (appear, witness:fleet, connect) read
  `online === true`. Both `witness.html` copies replace "chain health" with
  the website's presence-only paragraph. `check_witness_emulator_sync.sh`
  still compares only the two app copies with each other, so
  `canary_local.test.js` now replays `fleet_contract_vectors.json` through
  those three derivations in both vendored copies and holds every other
  `online:` the emulator writes to a bare literal: copies that lag the
  contract fail CI. Reaching users still needs the Flasher and Lab version
  bumps (`docs/RELEASE_BUTTONS.md`). Left, website-side: the poll path adds
  a newcomer as `online: true`, the tile toast says "online · chain intact"
  for an offline tile, and the paragraph's 8799 instruction does not work
  inside either app.

---

## 3. Home Assistant (monorepo `custom_components/` + HACS mirror)

- [x] **HA1 [code] Watch persistence.** Voice-started watches live in
  `hass.data` and die on restart — the one spoken promise the code can't keep
  (`custom_components/securacv/intent.py`, storage note). Use a
  `homeassistant.helpers.storage.Store`. *Done:* `watch_runtime.py` mirrors
  the roster to one domain-level Store (`.storage/securacv_watches`, v1)
  and restores it once per HA instance in `async_setup_entry`, before the
  MQTT subscribe can feed it. Saves are throttled, not debounced, so a busy
  event stream cannot keep a watch off disk: a clean restart keeps every
  watch, and a crash or power cut loses at most the last few seconds of
  changes — the docs say exactly that. A watch that ended while the hub was
  down is announced by the first tick, not dropped. The restore is bounded
  (`MAX_WATCHES`; a row the engine could never have built, such as a
  ten-year span, is dropped with a warning), and a store that fails to
  read is never written over: saves stay off, the actions say why they
  refuse, and a reload retries. Diagnostics report `watch_count` only. No
  gate runs a real Store or a 2024.x core; the tests model HA's
  `helpers/storage.py` as read at the 2024.4.1 and 2025.9.0 tags. (#1703)
- [x] **HA2 [code] Wire entity translations.** `strings.json` declares 9
  entity keys but no entity sets `_attr_translation_key` — the translations
  are dead and entity names are hardcoded English. Wire them, extend coverage
  to the other ~21 entity classes, then translations beyond `en.json` become
  possible. *Done:* every entity class in `sensor.py` and `binary_sensor.py`
  sets `_attr_translation_key` and none sets `_attr_name`. `strings.json`
  declares all 40 keys (14 sensor, 26 binary_sensor) with today's exact
  English names, so every rendered name is byte-identical (the doc-driven
  `canary-local/devices/homeassistant.json` did not move), and
  `translations/en.json` is an identical copy; the one correction is
  `kernel_online`, now "Online", which is what users already saw.
  `tests/test_entity_translations.py` keeps the two files identical,
  forbids `_attr_name`, rejects undeclared or dead keys and pins the 40
  names. Decision (keep every rendered name byte-identical, fix
  `kernel_online`, leave the kernel device's double "SecuraCV" prefix as a
  follow-up — HA12) — maintainer to confirm. Another language is now a
  `translations/<lang>.json` away; none is written. hassfest checks the
  JSON shape in CI. (#1703)
- [x] **HA3 [code] Stop swallowing malformed MQTT payloads silently.** Two
  `except TypeError: pass` sites in `binary_sensor.py` (~:664, ~:961) — add
  debug-level logs so a firmware field-type regression is visible. *Done:*
  both sites (the tamper-type sensor's health handler and the
  mesh-connected sensor's handler) log at DEBUG with the device id, the
  ignored topic and the traceback; state is untouched, as before. Debug,
  not warning: an untrusted broker must not be able to spam the log. Two
  caplog tests in `test_mqtt_payload_hardening.py` cover them. `sensor.py`'s
  three `(TypeError, ValueError)` sites return early on purpose and were
  left as they are. (#1703)
- [x] **HA4 [code] Timeline card: say when history is unavailable.**
  `www/securacv-timeline-card.js` falls back to current-state rows when the
  recorder is off, looking like "nothing happened". Render an explicit notice.
  *Done:* a new exported pure helper, `timelineStatus()`, drives a notice
  above the list whenever the history read failed — "Event history is
  unavailable (…), so this shows each sensor's current event only — not
  what happened over the last {h}h." — also when fallback rows exist, and
  an empty line that claims no time window. The card records where its
  rows came from and drops a failed read's notice when its config changes
  or its event entities go away. Node tests cover the helper and, in a
  fake-DOM sandbox, the element; `docs/lovelace_timeline.md` states the
  fallback. (#1703)
- [x] **HA5 [decision] Service surface.** The integration registers zero
  services — everything is intents + options flow. Decide whether
  pin/rotate/unpin/start-watch should also be services for automations.
  *Done:* option C — maintainer to confirm: watch actions only.
  `services.py` registers `securacv.start_watch`, `securacv.end_watch` (by
  id, or by label with case, spacing and a leading article ignored; an
  unknown or ambiguous label is refused, never guessed; an early end is
  announced like an expiry) and `securacv.list_watches` (response only),
  once from a new `async_setup`, with `hass` bound at registration — a
  `ServiceCall` has no `.hass` before HA 2025.1, which is inside
  `hacs.json`'s range. Voice and the actions share one start path
  (`watch_runtime.async_start_watch`). Every refusal is a
  `ServiceValidationError`: before the HA1 restore, when no loaded entry
  runs the watch tick (added after the Codex review on #1703), and for a
  duration it cannot read ("48 hours" is refused, not taken as 14 days).
  Pin, rotate and unpin stay options-flow forms: an automation-callable
  rotate would make "on mismatch, rotate to the received key" a one-line
  way to trust a re-flashed or impersonating device
  (`docs/device_trust.md`, "Why pin, rotate and unpin are not actions").
  The voice answer at the watch cap now names `securacv.end_watch` instead
  of a dashboard control that doesn't exist. `tests/test_services.py` pins
  the three names; hassfest checks `services.yaml` and the new `services`
  section of `strings.json` in CI only. Verify-now and export are still
  not actions (strategy/11 Phase 1), and the refusals are not yet
  translatable (HA9). (#1703)
- [x] **HA6 [code] Guard `FUTURE_TRANSPORTS`/`FUTURE_TAMPER_TYPES` in-package.**
  Today only `scripts/lint_feature_flags.sh` keeps them out of the advertised
  sets; add a unit test beside the constants. *Done:* `const.py` gains
  `ALL_TAMPER_TYPES` (mirroring `ALL_TRANSPORTS`), and `binary_sensor.py`
  creates its per-type tamper and transport entities by iterating the
  `ALL_*` lists through module-level tables (no entity, name or unique_id
  changed). `tests/test_feature_flags.py` proves each FUTURE/ALL pair
  disjoint and complete — every `TRANSPORT_*`/`TAMPER_*` constant in
  exactly one list — and the tables keyed by exactly the advertised lists.
  The shell lint stays as a second opinion: its check B now covers tamper
  types too, fails on an unparseable block instead of passing vacuously,
  and refuses a parsed token that is not a `const.py` constant.
  `docs/feature-flags.md` updated. (#1703)
- [x] **HA7 [code] Give `integrations/ha_frigate_mqtt/TASKS.md` a header**
  saying whether it is a tick-as-you-go operator runbook or an unexecuted
  plan; 26 unchecked boxes are currently ambiguous. If runbook: retitle
  RUNBOOK.md. *Done:* it is a runbook, so `TASKS.md` became
  `integrations/ha_frigate_mqtt/RUNBOOK.md`, with a status header: tick as
  you go for one bring-up and never commit the ticks; the expected outputs
  are derived from `docker-compose.yml`, `README.md` and
  `verify_pipeline.sh` as of 2026-09-22, not recorded from a live run.
  Decision (retitle, add the header and fix the mismatches in the same
  change) — maintainer to confirm. The mismatches fixed: the mandatory
  MQTT password step, four services with their real container names, the
  verify script's three real checks and their output, and no Home
  Assistant check in that script (a separate GUI step now). The first
  bring-up step also failed as written, in the runbook and the stack
  README alike — Compose interpolates every service on load, so `.env`
  must exist before the first `docker compose run`; both now write `.env`
  first, and `tests/test_bringup_order.sh` (in lint.yml) keeps that order.
  The walkthrough (`docs/integrations/home-assistant-frigate-mqtt.md`)
  links the runbook, and its checklist is corrected. A dated live run is
  HA13. (#1703)
- [ ] **HA8 [human-gated by U1] Ship a working example camera** for the
  Frigate config once a real camera is on the bench (today: one placeholder
  camera, `demo`, on an RTSP path nothing serves — Frigate starts it and
  logs ffmpeg errors for it, so the config parses but never detects).
- [x] **HA9 [code] Make the watch actions' refusals translatable.** HA5's
  `securacv.*` actions raise `ServiceValidationError` with plain-English
  messages and no `translation_key`, so a non-English install reads
  English errors (strategy/11's action-exceptions row reads ⚠️ for it).
  Give each refusal a key in an `exceptions` section of `strings.json`
  (copied to `translations/en.json`) and raise with the translation
  domain, key and placeholders.
  *Done (#1718):* all nine refusals the watch actions raise go through
  `services._refusal()` with `translation_domain`, `translation_key` and
  `translation_placeholders`; the keys live in a new `exceptions` section of
  `strings.json`, `translations/en.json` is a byte-identical copy, and the
  English is unchanged word for word. `tests/test_exception_translations.py`
  scans the package: every raise carries a literal key and the keys raised
  equal the keys declared. strategy/11's action-exceptions row reads ✅; its
  exception-translations row stays ⚠️ until `__init__.py`'s three
  `UpdateFailed` messages are translated. hassfest is CI's.
- [x] **HA10 [code] The watch roster points at a dashboard that doesn't
  exist.** `watches.speak_roster` ends its summary of four or more watches
  with "The dashboard has the rest.", and no dashboard lists watches. Point
  it at `securacv.list_watches` (HA5) instead, and update
  `tests/test_watches.py`, which pins the current sentence.
  *Done (#1718):* the summary now ends "The securacv.list_watches action has
  the rest." A new test holds every `securacv.<action>` named in `watches.py`,
  `intent.py` and `voice.py` to a registered action.
- [x] **HA11 [code] Watch notifications double the article and say "1
  days".** Every watch label starts with "the "
  (`watch_runtime._make_label`), and `watches.speak_ending` /
  `speak_fired` write "The {label} watch", so a notification reads "The
  the gate canary watch ended after 1 days" — the plural is wrong too. Fix
  the speech in `watches.py` and pin both cases in a test.
  *Done (#1718):* every `speak_*` function says the label's article once
  (`_label`/`_the` helpers) and counts in the singular for one (`_count`) —
  including `_duration_phrase`'s "1 minutes". Pinned in
  `tests/test_watches.py`. Left: an early end still rounds up to "after 1
  day"; the notification title shows a stored doubled label as stored.
- [ ] **HA12 [decision] The kernel device's double "SecuraCV" prefix.** HA2
  kept "SecuraCV Last Event" and "SecuraCV Adapter Host" (keys
  `kernel_last_event`, `adapter_stats`) byte-identical. Dropping the prefix
  changes their friendly names, new-install entity ids,
  `docs/homeassistant_setup.md` and the data `gen_homeassistant.py`
  generates from it (`canary-local/devices/homeassistant.json`). Decide,
  then rename in `strings.json` and `translations/en.json` together (the
  names test pins them).
- [ ] **HA13 [human] Record a live bring-up in the Frigate runbook.**
  `integrations/ha_frigate_mqtt/RUNBOOK.md`'s header says its expected
  outputs are derived, not observed (HA7 was done with no Docker daemon),
  and its "Settings → Devices & services → MQTT → Configure" path comes
  from Home Assistant's documentation, not a click-through. Run the stack
  once and replace the derived samples with a dated live run; the header
  keeps saying "derived" until then. The verify script's first check needs
  a live detection, so this pairs with HA8.
- [ ] **HA14 [code] `/enroll` is named as every Canary's key source, but only
  canary-wap serves it.** `docs/homeassistant_setup.md` (Step 6: "the
  fingerprint + pubkey hex are on each device's `/enroll` page"),
  `docs/device_trust.md`'s manual pinning and the carried
  `custom_components/securacv/strings.json` ("Read each device's fingerprint
  and pubkey hex from its /enroll page") send operators to `/enroll`. Only
  `canary_wap.ino`'s `start_http_server()` registers it; the headless MQTT
  variants that compile the canonical copy never do
  (`firmware/common/identity/device_signature.h` says so), and the flagship
  and the displays have no such route. Say where each product's fingerprint
  can be read out of band, or add the route. Change the three texts
  together, and carry `strings.json` to the mirror. Line refs at `main`
  80b6cd3f: `docs/homeassistant_setup.md` :475-477, `docs/device_trust.md`
  :97-99 (and :119, :222), `strings.json` and `translations/en.json` :88 and
  :97, the `__init__.py` docstring :845 and `device_trust.py` :14; the
  routes are `canary_wap.ino` :8413-8416, and `device_signature.h` :290-293
  says canary-sense and canary-vision never register them. Found in the
  wave-7 mirror reconcile. The HACS store page
  (securacv-homeassistant#17) names only the options-flow path, and no key
  source, until this lands.
- [x] **HA15 [code] The HomeKit Bridge recipe's include list failed Home
  Assistant's config check and skipped the Canary's own Motion sensor.**
  `docs/integrations/apple-home-homekit-bridge.md` §4 put
  `binary_sensor.*_occupancy` under `include_entities`, which Home Assistant
  validates with `cv.entity_ids` (`core.valid_entity_id`) and so refuses.
  The same list bridged only the kernel's `binary_sensor.pwk_*_motion`, not
  the integration's per-Canary Motion sensor (`binary_sensor.py`
  `SecuraCVCanaryMotionSensor`, `device_class: motion`), which exists for
  exactly this bridge. Found in the wave-7 mirror reconcile.
  *Done (#1722):* the occupancy glob moved to `include_entity_globs`, and
  `binary_sensor.securacv_canary_*_motion` joined it. The page says that
  glob also matches each Canary's Unexpected Motion tamper sensor, which no
  firmware signal drives, and that `exclude_entities` keeps it out. The
  kernel's `pwk_*_motion` and the WAP's `*_smoke_alarm` / `*_co_alarm`
  lines left the list, because the tree does not set those ids (HA16); the
  page tells the reader to add the ids their install shows. The schema and
  the naming were read from Home Assistant core's source, not run in a
  live Home Assistant. The HACS store page (securacv-homeassistant#17)
  gives the same Canary glob as its example without the Unexpected Motion
  caveat; that page is mirror-owned, so the caveat is the mirror's to add.
- [ ] **HA16 [code] The kernel's and the WAP's Home Assistant entity ids are
  documented but never set.** `src/bin/event_mqtt_bridge.rs` documents
  `binary_sensor.pwk_<zone>_motion`, `binary_sensor.pwk_chain_problem`,
  `sensor.pwk_last_event` and the rest of the `pwk_*` family (its header,
  :10-15), and `docs/homeassistant_setup.md` (:934-936, :980),
  `homeassistant/lovelace/securacv-dashboard.yaml` and the HomeKit recipe
  use those ids. canary-wap's acoustic sensors are documented as
  `binary_sensor.<id>_smoke_alarm` / `_co_alarm` (`gen_wap.py` :586-591 and
  the recipe's §1 table). Neither publisher sets an entity id: the kernel's
  discovery configs (`HaBinarySensorConfig` :190-205 and its siblings) and
  canary-wap's `publish_one_discovery` (`csi_mqtt.cpp`) send a name, a
  unique id and a device, and no `default_entity_id`. Home Assistant core's
  `components/mqtt/entity.py` sets `_attr_has_entity_name = True` and takes
  an id only from `default_entity_id`, so by that source a new install
  names them `binary_sensor.privacy_witness_kernel_pwk_<zone>_motion`
  (device "Privacy Witness Kernel", entity "PWK <zone> Motion") and
  `binary_sensor.canary_<id>_smoke_alarm_heard` / `_co_alarm_heard` (device
  "Canary <id>"). Read from source, not seen in a running Home Assistant.
  HA15 took the three unverified lines out of the recipe's filter. Fix: set
  `default_entity_id` to the documented ids in both publishers (the WAP
  half is a firmware change), check a live install, then put the lines
  back in the recipe; or re-document the ids from a live install. Found in
  the review of this sweep's wave-8 ledger.
- [ ] *(Mirror repo itself: no code work. It is byte-identical again as of
  securacv-homeassistant#17 (2026-09-24), which resynced the 33 carried
  files #1703, #1704 and #1718 had moved. The same PR brought the store
  page's watch-actions, key-pinning, broker-TLS and Apple Home sentences,
  and a `lint_readme.py` overclaim check that reads a hard-wrapped claim as
  one and refuses "encrypted by default". Its health items are U6 and U7
  above, plus the three monorepo-fixture tests its CI deselects, which is
  by design. A few more tests skip themselves there because they read
  firmware sources the mirror does not carry.)*

---

## 4. Website (`kmay89/securacv_website`)

- [ ] **W1 [code, gated by U5] Wire buy links for the two FCC-clear SKUs**
  (`wap-parts`, `vision-parts` in `store.json`) once Stripe exists. Everything
  else stays waitlist-only until U4.
- [x] **W2 [code] Refresh the stale `docs/roadmap.md`** — done in website
  PR #200: the `/fleet` TODO section became a Shipped entry (route,
  `fleet[]` off the `j` manifest, shared `js/serial.js`,
  `tests/fleet-facts.test.mjs`), and the witness note now records that the
  `/witness` route is the One Witness explainer, so the unbuilt burst-signal
  idea needs its own route.
- [x] **W3 [code] Showroom AR button is hardcoded to the Doorbell** — done:
  the button follows the selected product. Each Showroom product declares its
  model and its ledger figure in `js/showroom-products.mjs` (`ar:`), one pure
  function (`arButtonState()` in `js/showroom-ar.mjs`) turns the product and
  the carried `/scad/cad-dims.json` into the button's state, and
  `showProduct()` rewrites the `data-ar-*` attributes on every switch through
  `syncArButton()`; the static default is now the boot product, the Watch. The
  roadmap's click-time exporter plan was dropped, with its four reasons, in
  `docs/render-roadmap.md` (no vendored exporter, the site CSP's `connect-src`
  has no `blob:`, Scene Viewer needs a public URL, and an export would bypass
  the ledger pins every committed `.glb` carries). `tests/ar-dims.test.mjs`
  runs the real `syncArButton()` through every product switch, so dropping the
  call fails the suite (website PR #201).
- [x] **W4 [code] Dash and Combo have live Showroom products but no `.glb`** —
  done: `scripts/make-canary-dash-glb.mjs` and
  `scripts/make-canary-combo-glb.mjs` build both on the shared
  `scripts/lib/glb.mjs` builder from the carried ledger (envelope, seams and,
  for the Dash, the panel knobs `panel_l` / `panel_w` / `glass_t`); the case
  numbers the ledger does not carry (the Dash's bezel face and lip, the
  Combo's face positions) are named once in each generator's CAD table.
  Materials come from the role vocabulary and every stacked part sits one EPS
  proud; the Dash's glass is drawn as the bezel's view window, which is what
  the case shows. Both are on `/view-in-room`, in the Render Lab and behind
  the Showroom's AR button, byte-gated in `site-checks.yml` and refreshed by
  the Update-everything fixer, and a new models test fails if any
  `make-*-glb.mjs` is missing from a list a model must be registered in. Their
  AR copy says prototype, still in development, measured off the
  in-development CAD with no print file committed. Upstream first (#1703): the
  Dash's figure is now its printed case measured off its own CAD (118.1 × 78 ×
  25.6 mm with dock pads and screw lobes, where the ledger carried the vendor
  panel's box — option "retire body_mm and derive", maintainer to confirm);
  the Combo got its first fleet figure, measured the same way (86.4 × 73.6 ×
  26.38 mm, one seam at 24.38 — option (1), maintainer to confirm), and reads
  prototype because a figure measured off one in-development case now cites
  only that case's catalog variants (maintainer to confirm); and
  `canary_dash_display.scad` and `canary_combo.scad` joined `REFERENCE_SCADS`,
  so the site carries both sources sha256-pinned. Only software-rasterizer
  previews were looked at: how the two read on a GPU or in phone AR is not
  proven. What the work surfaced and did not fix is W17. (website PR #202;
  upstream #1703)
- [ ] **W5 [code] Blender hero bake** — milestone 1 landed (website PR #201):
  `blender/bake_hero.py` produces output now. On Blender 4.0 it died at
  `transform_apply`, its weighted-normal modifier was inert without auto
  smooth, and an STL in the enclosure frame would have exported lying on its
  back; all three are fixed, and it takes repeated `--in FILE [--face-down]
  --at Z` parts. A new wrapper, `scripts/bake-hero.mjs`, places every part
  from `scad/cad-dims.json` (no typed millimeters; a literal sweep enforces
  it) and checks the assembly before Blender runs and again after. The first
  hero, `models/hero/canary-vision.glb` (the assembled XIAO indoor Vision,
  about 7k triangles), matches the ledger's assembled envelope;
  `tests/hero-models.test.mjs` gates it (heroes are test-gated, not
  byte-gated), and no page loads it yet. Heroes get their own `models/hero/`
  path because the old promise that a bake "drops in at the same path" could
  never pass the byte gate; the promise is retired in the docs and the
  `/view-in-room` copy. Baking needs Blender 4.0+ with numpy (tested on 4.0.2)
  on a workstation; CI never runs Blender. What remains (the AO bake,
  colorways and a page opt-in) is milestone 2: W12.
- [ ] **W6 [code, paired with U4/U5 work] Supplier quotes are placeholders** —
  5 rows in `suppliers.json` marked `"status": "placeholder"`. When real RFQs
  land, also invert `tests/quote-compare.edge.test.mjs` ("honesty flag" test
  currently asserts placeholders exist — it will fail the moment they don't).
- [x] **W7 [code] Claims-audit remediation** — done, all four rows. The "no
  one can" absolutes now name the mechanism (the private key, the quorum, the
  chain) on How It Works, the Vault, the homepage's who-it's-for card and the
  Vault tagline in `js/site.js` (`llms.txt` / `llms-full.txt` regenerated);
  the ecosystem and request pages say the donation fund *will* land with a
  fiscal host not yet chosen and that nothing takes money today; the "All
  rights reserved" footers now carry the Apache-2.0 line `about.html` already
  used (and `engine.html`'s "© SecuraCV" reads "© Errer Labs"); and the
  homepage mock's biometric line reads `SUBJECT_04` under an "illustrative,
  not a real person or benchmark" label. Each fix has a guard pattern in
  `tests/legal-claims.test.mjs`, and `docs/claims-audit-2026-07.md` gained a
  dated status block naming what stays open (W15, W16). (website PR #201)
- [ ] **W8 [code] IA deferred editorial pass** (`docs/ia-review-2026-07.md`) —
  three of the four moved (website PR #201). *Availability vocabulary*, done:
  Available / Maker build / Preview / Concept are defined once in the glossary
  (`/glossary#availability`), mapped onto the fleet ladder on `/figures`, and
  lead the badges on Compare and the concept pages; the store's own banner now
  says "Walkthrough", so "Preview" means one thing, and
  `tests/availability.test.mjs` reads `store.json`, so a badge says
  "available" only when the store sells that product — the U5 store opening
  flips the test instead of breaking it. *One owner per message*, done:
  Witness, About, Watch Over and Industry trim their mechanism essays to a
  short intro that links into How It Works. *Homepage reduction*, mechanical
  half: the stub pointers, `#compare` and `#how` are cut, and the data-flow
  diagram, the Lab promo and "What's next" fold into the sections that own
  them, one row per move in `docs/ia-homepage-2026-09.md`. Still open, founder
  calls both: the editorial half (W14; the homepage is nine sections, not
  five, until it lands) and "Witness" as the hero noun (W15).
- [ ] **W9 [code] `render-roadmap.md` quality items** — (a) and (b) landed
  (website PR #201): spot-varnish masks on a part-space UV set (the Vision's
  gloss halos are derived from its ring and pipe radii, and a test measures
  their margins on the built geometry) and a micro-wear roughness map on the
  glossy faces. (c) is decided, not built: the roadmap now prefers a WebGL2
  path tracer (`three-gpu-pathtracer` on `three-mesh-bvh`, lazily imported on
  opt-in) to a WebGPU rewrite. Building it, and judging the micro-wear map on
  a GPU, is W13.
- [ ] **W10 [human] Tizen TV package has `REPLACEME` app IDs**
  (`tv/tizen/config.xml`) — needs Samsung Seller Office issuance before any
  submission.
- [ ] **W11 [code] Advisory Claude review workflow is inert** if
  `ANTHROPIC_API_KEY` is unset — the code half is done in both repos. Each
  `claude-review.yml` header now states the intended state (on for every
  same-repo PR), what happens without the secret, the observed state (as of
  2026-09-22 no run has produced a review) and the fork and Dependabot
  caveats, and a skip is now a `::warning::` plus a job-summary block, so a
  green check is not mistaken for a review; the website's `AGENTS.md` workflow
  row and both repos' `MAINTAINERS.md` say the reviewer runs once the secret
  is set (website PR #201; monorepo #1703). The new warning has not been seen
  on a real run yet (CI only). What remains is the human half: set
  `ANTHROPIC_API_KEY` under Settings → Secrets and variables → Actions in both
  repos (and under Dependabot secrets if bot PRs should be reviewed). Until
  then a green "Canary Reviewer" check means nothing was reviewed.
- [x] **W12 [code] Blender hero bake, milestone 2** (W5 landed milestone 1).
  Written up in `docs/render-roadmap.md` ("the hero's baked AO and a page
  opt-in (milestone 2)") and not built: a scripted Cycles-CPU AO bake exported
  as a same-origin PNG that `scripts/bake-hero.mjs` patches in by relative URI
  (the hero test already requires the relative URI), colorway variants
  (`KHR_materials_variants` from the colorway registry), and an opt-in "hero
  render" toggle on `/view-in-room` beside the procedural default, which stays
  the default (the hero keeps the SCAD origin, not the procedural models'
  centered one). Needs Blender 4.0+ with numpy on a workstation. CI never runs
  Blender, so a ledger move of more than 1 mm turns
  `tests/hero-models.test.mjs` red until someone re-bakes.
  *Done (website #203):* `blender/bake_hero.py --bake-ao` runs a Cycles-CPU
  AO bake over one atlas across both parts (512 px, 256 samples, 40 mm rays;
  57 s here, a 77 KB PNG). `scripts/bake-hero.mjs` patches the PNG into
  `models/hero/canary-vision.glb` by relative URI and adds the carried
  registry's colorways as `KHR_materials_variants`. `/view-in-room` gains an
  opt-in "Hero render" toggle, off by default, that places the hero's
  hotspots about its own origin and names the lens opening, since the hero
  draws no camera. `tests/hero-models.test.mjs` grew from 10 to 17 tests
  (18 with the hotspot-label guard). A colorway carry needs no Blender:
  "Update everything" runs `bake-hero.mjs --patch-only`. Not judged on a GPU
  and not tried in AR on a phone. The roadmap says so, and W13 keeps the GPU
  half.
- [ ] **W13 [code, needs a GPU session] Render quality — W9's GPU half.** (1)
  Cinematic mode: the route is decided in `docs/render-roadmap.md` (a WebGL2
  path tracer, `three-gpu-pathtracer` on `three-mesh-bvh`, lazily imported on
  opt-in, not a WebGPU rewrite); building it needs a GPU session and a
  bundle-size review. (2) The micro-wear roughness map spans G 241–254 (14
  levels) and is "not yet judged on a GPU": choose its amplitude, and whether
  to normalize the peak to G = 255, in the Render Lab — a textures test pins
  the range the roadmap states, so move the map and the roadmap together.
- [ ] **W14 [code+decision] Homepage reduction, the editorial half** (W8's
  remainder; founder items 1 and 3 in `docs/ia-homepage-2026-09.md` "Open —
  human items"). Relocate the timeline (`.timeline-section`, "Same camera.
  Same question. Different futures.") to `witness.html` or cut it, then
  rewrite the hero description and `#witness-pitch` to the review's order:
  hero → three-part product → what leaves the device → four paths → proof.
  Before the timeline can be cut, "rules apply forward only" needs an owner
  off the homepage: only the timeline's Day-180 row says it today, and its
  natural home (`how-it-works#constraints`) is headed "Three intentional
  constraints", so a fourth is a copy decision. Fifteen tests read
  `index.html`; the manifest lists them. Then prune the CSS families the
  mechanical cut left unused.
- [ ] **W15 [decision] The CANARY trademark exposure, and "Witness" as the
  hero noun.** The claims audit's one structural item, still open after W7:
  CANARY is a live U.S. registered trademark for home-security cameras, the
  options are a clearance opinion, repositioning "canary" as a metaphor, or a
  rename, and the audit says not to scale store sales until it is settled
  (`docs/claims-audit-2026-07.md`), which bears on U5/W1. The IA
  review's hedge, W8's item (d), is to build the public hierarchy around
  *witness* and keep *Canary* as the affectionate product name; renaming
  surfaces is a founder decision, and the dormant hero B variant
  (`data-hero="b"`, "Three things. One witness.", `js/exp-hero.js`
  `ROLLOUT = 0`) is the cheapest place to try it (open item 2 in
  `docs/ia-homepage-2026-09.md`).
- [ ] **W16 [code+human] The claims-audit rows W7 did not cover**, listed in
  `docs/claims-audit-2026-07.md`'s "Status 2026-09-22" block: "architecturally
  incapable" (`compare.html`); the "nothing phones home / anywhere" family
  (M2: `download.html`, `lab.html`, `compare.html` and `watch-over.html`'s
  hero fine print — W8's trim removed only the second of watch-over's two);
  the recording-consent notice (H3); and a contact mailing address and role
  email plus the privacy policy's date (M4 / L3), which need facts only a
  human has. Give each fix a guard in `tests/legal-claims.test.mjs`, as W7
  did.
  *Code half done (website #203):*
  - "Architecturally incapable" names what is missing.
  - The phone-home family and its siblings ("LAN-only", "local-only",
    "never online") are gone from the site. The FAQ names each routine
    trip online and the iPhone companion's iCloud default.
  - A recording-consent notice, pinned verbatim, is on the store and on
    First Flights.
  - `tests/legal-claims.test.mjs` scans each surface as flowing text, so an
    aside, a tag, a line break or a no-break-space entity cannot split a
    phrase.

  Still open (human): the contact mailing address, a role email and the
  privacy policy's date (M4/L3). Recorded for the same founder pass: the
  privacy policy says the site fetches nothing external, while every page
  loads Google Fonts. The audit's monorepo follow-up list (the Lab's
  "nothing here phones anywhere" footers, `hatch.json`'s motto,
  `docs/FAQ.md`, `docs/getting_started_canary.md`) and Compare's unsourced
  "auto-deletes in 24 h" cell are the wave-4 claims-followup package.
  `docs/FAQ.md`'s phone-home answer now names the Canaries' disclosed
  outbound paths and the desktop apps' update fetch (#1722): the broker, the
  signed update check, the display's SNTP and its opt-in forecast, linked
  to `SECURITY_MODEL.md`'s list. It still opens with "No.", which website
  #203 ruled out for the site's FAQ, so it stays on that follow-up list
  with `docs/getting_started_canary.md`, the Lab's footers and
  `hatch.json`'s motto.
- [x] **W17 [code] Model and copy drift found while building W4, not fixed
  there** (website PR #202). (1) `scripts/make-canary-glb.mjs` draws the
  Vision's lens barrel and glass under a solid Ø13 accent disc, so the lens
  renders as a flat yellow disc. (2) `scripts/make-canary-wap-glb.mjs` puts
  its four screw heads 0.5 mm inside the solid lid, so they never show; a
  models-test guard could require every non-body part to show at least one cap
  not buried in another solid. (3) `js/lab.js` still captions hatching
  enclosures "render-verified, print validation pending" (the site reserves
  the word for signatures; the AR page's new guard does not reach `lab.js`).
  (4) The Showroom's Watch tagline says "screwed drum" (v0.2 is a snap bezel),
  and its Combo procedural parts put the lux aperture and light pipe at x 8.5
  / 34.5 and the screws at ±39 where the CAD says 1.6 / 27.6 and ±38.5. (5)
  The Combo AR model's lens and radome positions are hand-kept CAD values
  until C15 carries the measured features. A model edit regenerates and
  commits its `.glb` in the same change.
  *Done (website #203):* (1) the Vision lens is a nested EPS ladder, no longer
  a flat disc; (2) the WAP's screw heads sit proud of the lid, and a new
  models-test guard (every non-shell part keeps a visible cap) also found and
  fixed the doorbell faceplate hiding its lens bezel, grille, vents and LED
  ring; (3) `js/lab.js` says render-checked, and a site-wide copy sweep
  refuses "verified" for renders, meshes and prints; (4) the Showroom's Watch
  is a snap bezel and the Combo's face parts sit on the CAD's numbers; (5)
  closes with C15. The three changed models are regenerated byte-for-byte.
  Noticed, not changed: the Showroom Combo's vboard/vcam at x −22.1 where the
  CAD's `v_cx` is −21.6.
- [x] **W18 [code] The Lab download page says the app fetches only its update
  manifest** (`download.html`, "Local-first, always": "talks only to your own
  devices … The one thing it fetches for itself is its own update manifest,
  from the project's public GitHub releases"). Since A14 (#1704), once a board
  is connected the Lab's Flash page asks GitHub which signed firmware is
  published, and pressing Flash downloads that image. The Lab's own surfaces
  say so now (`desktop-lab/README.md`, `desktop-lab/INSTALL.md` and the
  bundle's `longDescription` in `tauri.conf.json`); the download page is
  hand-written, so no carry fixes it. No Lab release carries A14 yet (the
  version is still 0.2.4; the bump is a release decision), so land the fix
  before or with that release: name both fetches, both from the project's
  GitHub releases. The same section's "(and, soon, reliable USB flashing)"
  ages with that release; no Lab flash has run on real hardware yet (A14's
  human work), so say what the app does, not that it is proven. Not one of
  the claims audit's M2 rows (`download.html` :6, :9, :124, which W16 holds),
  but when W16 scopes those it should name the same two fetches, not the
  audit's device wording ("the only outbound traffic is a signed update
  check …"), which predates A14. A new website PR from origin/main, with a
  guard in `tests/legal-claims.test.mjs` that fails on "the one thing it
  fetches".
  *Done (website #203):* the "Local-first, always" block says the Lab reaches
  the internet for two things only, both from the project's public GitHub
  releases: its update manifest and the firmware for its Flash page, checked
  against the pinned release key before a byte is written. The USB-flashing
  sentence stays future tense until a published Lab release carries A14
  (review option A — maintainer to confirm); `legal-claims.test.mjs` bans the
  old sentences. Rewrite it in the present tense with the A14 Lab release.
- [x] **W19 [code] The website glossary's "The Vault" conflates sealed
  snapshots with the quorum Vault** (`scripts/lib/glossary.mjs`, rendered
  into `glossary.html` and `llms-full.txt`): "The sealed store where raw
  snapshots live. Sealed means no one — including you, including us — can
  open it alone." The monorepo glossary has kept the two apart since
  ios-unseal (#1703): the Vault is the kernel's sealed store of raw frames
  that break-glass opens by quorum (Invariant V), and a sealed snapshot is
  "Not the Vault", one canary-wap frame encrypted to one person's X25519 key
  and opened by that key's holder alone, so the website's sentence is false
  for it. The sentence also keeps the "no one can … alone" absolute W7
  rewrote elsewhere (the Vault tagline in `js/site.js` now reads "Footage
  that takes a quorum to open."), and W7's guard misses it because the
  em-dash aside splits "no one" from "can open". Rewrite the entry to the
  monorepo's split (with a "Sealed snapshot" term beside it if the site wants
  one), regenerate (`node scripts/make-glossary.mjs`, then
  `node scripts/make-llms-txt.mjs`), and widen the guard in
  `tests/legal-claims.test.mjs` to read through the aside (it scans the
  generated `glossary.html`; `scripts/` is out of its scope by design). A new
  website PR from origin/main.
  *Done (website #203):* "The Vault" is the witness kernel's sealed store of
  raw frames, opened only through break-glass by a quorum of trustees; a new
  "Sealed snapshot" term is one Canary WAP frame sealed to one person's key,
  opened on the holder's own device, not the Vault and no quorum.
  `glossary.html`, `llms-full.txt` and `llms.txt` are regenerated, and the
  tests hold the split.
- [x] **W20 [code] The website's Walls read a silent `online` as present, and
  its pages carried stale status copy.** Found by the wave-7 site docs-drift
  scout. `tv/app.js`'s `parseFleet` defaulted a missing `online` to true,
  and `js/tv-emulator.js` did the same at three sites. That is against the
  fleet contract the Apple TV and the Rust core follow
  (`tvos/discovery/DISCOVERY.md`: a silent row is not claimed present).
  *Done (website #199):* both read `online === true`. `tests/tv-wall.test.mjs`
  replays a verbatim copy of `fleet_contract_vectors.json` through
  `parseFleet` and pins the copy's sha256. `witness-wall.html` and
  `tv/README.md` say the real Wall is presence-only against every shipping
  source. The download page offers the Flasher, on its own `flasher-v` train
  with the broker-encryption select and the fuse-read difference, and says
  Windows is not built. Stale status copy is corrected on `docs/LAYOUT.md`,
  `witness.html`, `docs/roadmap.md`, `lab.html`, `linux.html`,
  `engine.html`, `compare.html` and `apple-tv.html`. The glossary defines
  the companion app. Left: A24 and W21.
- [ ] **W21 [code] The website's copy of the fleet contract vectors is
  hand-carried.** W20 copied
  `tvos/witness-core/tests/fixtures/fleet_contract_vectors.json` into the
  website's `tests/fixtures/`. `tests/tv-wall.test.mjs` pins it by sha256
  and tells a human to re-copy it. Add it to `scripts/carry_to_site.py`'s
  verifier carry, with a PROVENANCE line and a
  `scripts/tests/test_carry_to_site.py` case, so the weekly carry job
  refreshes it. Then retire the hand pin in the website's test. The same
  website follow-up can fix a stale pointer: `tests/legal-claims.test.mjs`'s
  phone-home comment cites `SECURITY_MODEL.md`'s old heading "The display
  line's disclosed exceptions", path 1, which is now "The networked
  products' disclosed outbound paths", path 3. The monorepo half landed in
  #1720, and website #207 added the file to the carry job's `CARRY_PATHS`
  and fixed that pointer; what is left is retiring the hand pin once the
  first carry PR has run. — in progress (2026-09-23)

---

## 5. Hardware, CAD, print files (monorepo)

The canonical ledger is `docs/hardware/enclosure/AUDIT_2026_09.md` ("Open —
major, by theme") — work its themes, then tick here.

- [ ] **C1 [code] Parametric-UX debt, cheapest first** (audit §"Parametric
  UX": 705 of 1704 parameters showed no help, 174 of them because written help
  was mechanically discarded) — wave 1 landed (#1703): every shared-help line
  outside the 7" case is split one knob per line (116 knobs on 39 lines in 14
  dev and tool files; each group comment became one help per knob citing its
  own fact, `deviates:` reasons stay on their knob's line, and no help was
  invented for a knob the comment did not describe), and the unambiguous
  comments written above a knob are summarized onto it, the paragraph kept.
  Most of the audit's above-line comments turned out to finish the previous
  knob's help and were left alone. Knobs without help, recounted with the
  builder's own parser: 681 → 536. No geometry moved: the evaluated CSG trees
  of all 75 touched parts are byte-identical to base (the full render is
  `enclosure.yml`'s). Guard: `lint_design_lang.py` fails a line with two knobs
  and a trailing help, with the 7" case's four lines held in a shrink-only
  `HELP_LINE_DEBT`; the rule is `DESIGN_RULES.md` §10. What remains (the 7"
  case's lines, help for the knobs that never had any, ranges, one two-stud
  group name, presets) is C10.
- [x] **C2 [code] Naming collisions across case files** — done: each of
  `usb_w`, `vm_*`, `skirt_t` and `clip_w` now means one thing, the minority
  meanings renamed at every use site, comments and assert strings included.
  The display cases' USB shell is `usb_shell_w` / `usb_shell_h`; the watch
  station's side slot is `usb_slot_w` / `usb_slot_h` and its bezel snap finger
  `finger_t`; the wear clip's belt-clip width is `leaf_w`; the Sense's and its
  gang plate's radar carrier is `radar_l` / `radar_w` / `radar_front_h` (their
  derived `vm_cx` / `vm_cy` / `vm_standoff` too), and the Sense manifest's
  `cad.params` follow, `gen_cad_params.py` writing zero bytes. No geometry
  moved. Guard: `lint_design_lang.py`'s `KNOB_MEANINGS` fails a listed knob
  whose help says the other meaning and names the name to use;
  `DESIGN_RULES.md` §10 carries the table. The website carried the renames in
  website PR #202; one stale comment is left for C13. (#1703)
- [ ] **C3 [code] Lid-rib proportions + hardware counts** — the hardware half
  landed (#1703): `docs/hardware/enclosure/gen_hardware.py` reads each
  committed set's `HARDWARE` echo off the CAD (13 sets: the `render.sh`
  presets, the WAP shield render and one `screw_insert` set per case family)
  into `hardware.json` and joins the fasteners to the BOM CSVs by each row's
  own description; its `--check` runs in `enclosure.yml` and
  `scripts/regen_cad.py` and fails on new drift, or on a listed drift that
  stops happening. Its first run found 12 disagreements with the BOMs, listed
  in `hardware.json` and left for the CSV owner: C12. The rib half has its
  probe and no rib change: a fit-check family could not see a case's variables
  (`use<>` imports modules, never variables), so the probe reads each case's
  own PLS-4 bound instead, and the eight headroom numbers sit in
  `DESIGN_RULES.md`'s "Lid rib proportions" table, held to the CAD by the same
  `--check`. The call is C11 (evidence recorded, no rib changed — maintainer
  to confirm). Also still open here: carrying per-set hardware into
  `build.json`, with a third `assembly.test.js` leg.
- [ ] **C4 [decision] The C6 `brass_h` disagreement** — registry says 5.0, the
  file prints the C3's measured 3.0; `devices/README.md` says owning either
  number would bless it. A maintainer measures and decides.
- [x] **C5 [code] Draw the Watch Station figure from its parts** — done
  (option "draw from the CAD's seated measurement" — maintainer to confirm):
  `gen_assembled_dims.py` gained a `device.canary-display-watch` row (the drum
  plus the bezel seated at `drum_h`, read from `bezel()`'s own nub datum, so
  no `.scad` changed), and `gen_figures.mjs` / `gen_device_glbs.mjs` a third
  envelope source, `assembled: true` → `dims_source` "assembled-cad" (refused
  without a row; one envelope source per figure). The Watch is 49 × 49 × 23.19
  mm with its seam at 21 and stays a prototype on the ladder; `massing.mjs`
  now draws it from the measured envelope, seam and face aperture instead of
  typed numbers, and a knob edit turns `gen_assembled_dims.py --check` red
  until regenerated (shown on a scratch copy: `disc_t` 5.0 → 5.5 reads 23.69
  against 23.19). The website carried it in website PR #202, where the Watch
  model now rides the 21 mm seam. With it came a shared `scad_probe.py` and a
  Lab preview-mesh gate in `enclosure.yml`
  (`gen_enclosures.py --check-previews`), whose first run found two rotted
  preview meshes, now re-rendered (#1703).
- [ ] **C6 [human] Caliper passes for the board registry** — 2 of 9
  `BRD_REGISTRY` rows are measured; `ws147`/`ws169` (which drive three
  display cases) are vendor-drawing rungs; ~2,460 `MEASURE` tags across 26
  `.scad` files. Pairs with U1.
- [ ] **C7 [human] Print-validate the in-development enclosures** — only 4 of
  ~35 printable designs have committed STLs
  (`docs/hardware/enclosure/README.md` "In development" lists 22). Print,
  fit-check, then commit per the regen order in CLAUDE.md
  (`scripts/regen_cad.py`). Every environmental rating in `field_ratings.md`
  is also still "untested" — that protocol needs real units too.
- [x] **C8 [code] Kiri:Moto slicer is not vendored** — closed as intended
  (option B — maintainer to confirm): the engine stays unvendored. The blocker
  is not the vendor step but `SharedArrayBuffer`, which needs a
  cross-origin-isolated Lab (COOP/COEP through a `coi-serviceworker` shim,
  since GitHub Pages cannot set the headers) for one soft number, so it is
  revisited only with a COOP/COEP decision for the whole Lab;
  `canary-local/assets/vendor/kiri/README.md` now opens with that. The slice
  button's fallback note speaks to the person printing ("Exact toolpath time
  needs the optional slicer engine, which this copy of the Lab doesn't include
  — the modeled estimate stands."), and the vendor pointer moved to
  `console.info`; `slicer.test.js` pins both, and `kiri_slice_probe.mjs`
  asserts the new note in CI only (`canary-local.yml`). (#1703)
- [x] **C9 [code] Nightstand-line 3D figures reuse the Dash mesh** — done
  (option "route the display line through the generated figure GLBs" —
  maintainer to confirm): `scene3d.js`'s `buildFromFigure()` draws each
  display from its committed fleet-figure model, painted by material role with
  the lit face as the live-glass screen plane; all seven registry displays
  route through it, and the hand meshes (`buildWatchStation` / `buildDash` and
  their typed numbers) are gone. `app.js`'s silent WAP fallback became
  `builderFor()`: an idea stands in as a ghost, a built figure with no model
  as its envelope slab plus a console warning, never another device's body.
  Tests: `canary-local/tests/scene_figures.test.js`; the real-GPU render probe
  runs in CI only (`canary-local.yml`). Three display manifests have no
  registry card (Nightstand C6, Nightlight C3, Nightstand 7), so nothing draws
  them yet (C13). (#1703)
- [ ] **C10 [code] C1's remaining waves.** Wave 1 (#1703) moved the help that
  was already written; the rest is writing it. Help for the 536 knobs that
  have none, the released four first (their help carries to the website
  builder's Advanced accordion); `[min:step:max]` ranges where a range is
  known; one group name for the two-stud interface; presets for the doorbell
  and the Sense, modeled on `canary_wap_enclosure.scad`'s `preset` with
  defaults unchanged so no STL moves. Also the 7" case
  (`canary_s3_lcd7.scad`): its four shared-help lines (9 knobs) and its
  above-line comments were left out of wave 1 by instruction and sit in
  `lint_design_lang.py`'s `HELP_LINE_DEBT`; `gen_stamp`'s digest ignores
  comments and whitespace, so splitting them needs no STAMP_REV bump — delete
  the debt entries with the split. The Dash's and the J-box's `usb_w` /
  `usb_h` still share a line (no help to lose). Mind the Sense's `usb_h`: its
  help is only a `[4:0.5:8]` range, and `gen_enclosures.py` reads a range only
  at the start of a comment, so wave 1 left it alone rather than risk its
  slider. `DESIGN_RULES.md` §11 "Customizer help text" names the first four.
  *Wave 2 done (#1718, website #203):*
  - The 7" case's shared-help lines are split and `HELP_LINE_DEBT` is
    empty. The Dash's and the J-box's `usb_w`/`usb_h` are split.
  - Every knob in the released four (WAP, Vision, doorbell, Sense) has help
    read off the geometry that uses it, except the option-list selectors.
  - Stated ranges sit on their knob's line, and `gen_enclosures.py` reads a
    range where the builder does (43 knobs had been a slider in one and raw
    text in the other).
  - One group name, "Stud/keyhole interface", across 16 files, held by a
    new `lint_design_lang.py` rule.
  - Presets for the doorbell (`doorbell_weather`) and the Sense
    (`sense_wall`, `sense_ceiling`), with defaults unchanged (preset
    contents and the group name: maintainer to confirm). A new check
    refuses a preset that overrides a control the builder does not gray
    out. It caught the WAP's and the Vision's presets leaving `opt_weep`
    live.
  - Knobs without help: 535 → 347 of 1743. The CSG of every touched part
    is byte-identical to the base.

  Still open: the 336 development-case knobs without help, and a
  maintainer call. The OpenSCAD 2021.01 desktop Customizer makes a slider
  or dropdown only for a bare bracket comment, so the house `help // [range]`
  form gives plain boxes there (`DESIGN_RULES.md` §11). The J-box's pigtail
  `usb_w`/`usb_h` needs a name of its own, and multi-line group headers are
  still invisible to the Lab's parser.
- [ ] **C11 [decision] Raise the lid ribs, or leave them** (C3's rib half; the
  audit's "Ribs proportioned backwards"). The evidence is recorded and no rib
  changed: `DESIGN_RULES.md` §11's "Lid rib proportions" table gives the
  headroom each committed preset leaves over its tallest component, read off
  the CAD by `gen_hardware.py` and held there by its `--check`. Four presets
  are pinned at zero slack (doorbell, Sense, Vision DevKit indoor, WAP
  battery_full) and four have 0.35–3.58 mm, so no per-file `lid_rib_h` can
  rise without growing `cav_extra` (which moves the released envelopes and
  every figure and AR model after them) or becoming preset-derived, and the
  WAP's rib doubles as the battery hold-down (`batt_hold`). The headroom is
  bounded by each preset's tallest component, which is not a geometric
  clearance proof; a true rib-versus-component probe needs per-case
  component-envelope modules in the released case files. The table's
  candidate is a bool `lid_rib_fill`: the rib grows to the preset's own slack,
  the WAP's battery sets excluded, the four pinned presets stay
  byte-identical, only the slack presets' lids and fronts move (their
  envelopes do not), shipped through `scripts/regen_cad.py --previews`. A
  maintainer decides, per case.
- [ ] **C12 [human] The 12 BOM disagreements `gen_hardware.py` found.** Its
  first run joined each committed preset's fasteners to the BOM CSVs and
  listed what disagrees in `docs/hardware/enclosure/hardware.json`
  (`bom_drift`): two short quantities in `bom_canary_vision.csv` (the doorbell
  echoes 10 M2 screws against SCR5's 8, and 7 inserts against INS1's 5) and
  ten fasteners no row bills (mostly #6 wall screws; the Sense's M2 pan
  self-taps and its hinge bolt; the WAP weather base's M3 wall screws and its
  shield's replacement lid screws). The generator never edits a CSV: someone
  who knows the build decides which side is right, fixes the CSV (or the CAD),
  and shrinks `KNOWN_DRIFT`, whose `--check` fails when a listed entry stops
  happening. Same pass: `bom_canary_display.csv`'s D-ENC1 still describes the
  Dash back's retired keyholes and M4 pair.
- [x] **C13 [code] Lab display-line leftovers from #1703.** (1)
  `canary-local/devices/registry.json` lags the display manifests:
  `canary-display-nightstand-c6`, `canary-display-nightstand7` and
  `canary-display-nightlight-c3` have no registry card (the C3's differs from
  the registry's `canary-nightlight`), so nothing draws them in 3D, and the C6
  and C3 pocket cases the manifests now home there show on no Lab device page;
  rerun the chooser and board-identity tests after aligning. (2)
  `canary-fence-guard` is an idea still drawn as a solid procedural body
  (`buildFenceGuard`), where every other idea renders as a ghost. (3)
  `real-shapes.js`'s `realDash` seat constants were derived for a 16 mm body
  and the old 8.4 mm back mesh (the preview back is 9.0 now); re-derive them
  from `total_t`. (4) `canary-local/tools/figures/massing.mjs` comments still
  call the 1.47" S3 stick's USB shell `usb_w` / `usb_h`, the names C2 gave to
  the wall opening.
  *Done (#1718, website #203):* (1) the Nightstand C6 and the Nightstand 7
  get registry cards (no twins, so the flashers' links do not move); the C6
  draws a new sketch figure of its pocket case, the Nightstand 7 draws the
  Dash 7's slab through a new `figures.json` `manifests` join, and the C3
  manifest names its existing card with a new `lab.card` key rather than
  adding a second one (option "lab.card join" — maintainer to confirm).
  `lint_device_manifests.py` now refuses a claimed case whose home is no
  registry card, and the chooser, board-identity and figure tests pass on the
  aligned registry. Every display card's Web row said "first-boot portal
  only", but every flavor serves the glass mirror on :80 after provisioning;
  all nine now say so, and a test ties the row to the firmware. (2) The Fence
  Guard draws as a ghost like every other idea. (3) `realDash`'s seat is
  re-derived from `total_t` and the 9.0 mm back, and a test holds it to the
  ledger's assembled row. (4) `massing.mjs` names the shell
  `usb_shell_w` / `usb_shell_h`. Also: `scene_figures.test.js`,
  `body_dims.test.js` and `device_models.test.js` were run by no workflow;
  `canary-local.yml` now runs them, and `canary_local.test.js` fails any test
  file there that no `node --test` command names. The website carries the
  C6 figure in `scad/cad-dims.json`. Open: the C6 manifest's `figure` (a
  firmware change with a dist rebuild), and the Dash stand's fin, which
  overlaps the back's two lower dock pads as modeled.
- [x] **C14 [code] The CAD regen order runs `gen_figures` before a file it
  reads.** `scripts/regen_cad.py` (and CLAUDE.md's order) runs
  `gen_figures.mjs` at step 6 and `gen_enclosures.py` at step 11, but
  `gen_figures.mjs` reads `canary-local/devices/catalog.json`, which
  `gen_enclosures.py` writes, as catalog evidence, so a `cad.enclosure_sets`
  or README-table edit needs `gen_enclosures.py` first and then
  `gen_figures.mjs`. The order is meant to follow what each generator reads;
  fix it in the script and wherever the order is stated (CLAUDE.md,
  `gen_cad_params.py`'s `REGEN_ORDER`) together. A related hazard:
  `gen_enclosures.py`'s catalog globs `docs/hardware/enclosure/*.scad`, so
  running it while `gen_assembled_dims.py` has its `.tmp_assembled_*.scad` on
  disk adds a bogus product.
  *Done (#1718):* `gen_enclosures.py` now runs before `gen_figures.mjs`, and
  the order moved together in `scripts/regen_cad.py`, `gen_cad_params.py`'s
  `REGEN_ORDER`, CLAUDE.md and the enclosure README. A test fails any step
  that reads (by quoted path) a file a later step writes. The catalog skips
  scratch `.scad` files (`.tmp_probe_*` and any dot- or tmp-named file), and
  a test builds the catalog with strays on disk and gets the same bytes.
- [x] **C15 [code] Carry the Combo's measured face features to the website.**
  `gen_assembled_dims.py` records the Combo's lens aperture and radome window
  as face `features` (the case's own `lens_x` / `lens_y` and `rad_cx` /
  `rad_cy`, re-evaluated by `--check`) and the fleet figure draws them there,
  but neither `figures.json` nor the site's `scad/cad-dims.json` carries them,
  so the site's Combo model places them by hand (W17). Carry them as an
  additive `features_mm` key (`distill_cad_dims` plus the `NEW_FIG` pin in
  `test_gen_builder_manifest_site`), then have `make-canary-combo-glb.mjs`
  read them and the models test pin them. Separately, the Combo itself has
  no fit-check module and no committed STLs: its seat is read from its own
  datums and no closing gate proves it, so it stays a prototype until C7
  prints it.
  *Done (#1718, website #203):* `distill_cad_dims()` carries each figure's
  measured face features as an additive `features_mm` (verbatim, unrounded)
  and refuses a malformed, stale or off-face record;
  `test_gen_builder_manifest_site` pins the key and each refusal. The
  website's `make-canary-combo-glb.mjs` places the lens and radome window from
  it through `cad-dims.mjs`, and the models test holds them there.

---

## 6. Documentation hygiene (small, do alongside other work)

- [x] **D1 [code] `docs/hardware/enclosure/AUDIT_2026_09.md` "Open — blocking"
  section retracts itself** — done: retitled "Blocking — found by this audit,
  all four since fixed", with the fix dates in the intro and the entries kept
  as the proof record (#1693).
- [x] **D2 [code] `firmware/ESP32S3_OPTIMIZATION_ROADMAP.md` items 1 and 2 are
  fixed in source but were still listed open** — done: §2 items 1–2 and their
  §6 table rows now read (fixed); the remaining follow-up (a sensor-side small
  capture for frames above XGA) stays named in item 1 (#1693).
- [x] **D3 [code] `docs/review/01-flag-report.md` F-12** — no change needed:
  the report's minors status block already carries "F-12 ✅ Resolved
  (#673/#706)" (checked in #1693).
- [x] **D4 [code+decision] Carry the Watch Station and Dash assembly steps
  into the catalog README** — done (land now with a dev caveat — maintainer to
  confirm): `docs/hardware/enclosure/README.md` has an `## Assembly` block for
  the Watch station (v0.2 snap bezel, pry notch) and the Dashboard display
  case (dock pads, wall cradle), steps written from the `.scad`, each opening
  with "Render- and mesh-checked, not print-validated" as prose outside the
  numbered list. `gen_enclosures.py` carries that caveat into `build.json` as
  its own `caveat` field, which the workshop and the Assemble tab show beside
  the steps, and now refuses an Assembly block it cannot attribute or a second
  one for the same device; every Watch and Dash `readmeStep` points at its
  README row, and `assembly.test.js` pins the join by index and by content.
  When C7 prints a unit, removing the caveat paragraph is the whole follow-up
  (#1703).
- [x] **D5 [code] The enclosure audit does not say what landed in #1703.**
  `docs/hardware/enclosure/AUDIT_2026_09.md`'s "Open — major, by theme"
  paragraphs carry no note newer than 2026-09-08: "Naming collisions" (fixed —
  C2), "Parametric UX" (wave 1 — C1; 681 → 536 knobs without help), "Assembly
  and service" ("hardware counts remain open" — counted now by
  `gen_hardware.py`, with 12 BOM disagreements listed, C12) and "Ribs
  proportioned backwards" (the headroom evidence is recorded; the call is
  C11). Add a dated note to each in the audit's own italic style — this file's
  rule is to fix the canonical ledger's item, then tick both. *Done:* each of
  the four, and "Stiffening" (whose "lid-rib proportions remain open" needed
  the same C11 pointer), carries a 2026-09-23 note in the audit's style
  naming what #1703 landed and the item that holds the rest. (#1704)
- [x] **D6 [code] "Verified" where nothing was verified.** The enclosure
  README's "In development" lead said the designs were "render- and
  mesh-verified", which breaks the project's rule that "verified" means a
  signature checked against a pinned key; D4's new assembly caveats already
  say "checked". *Done:* that line, `docs/hardware/README.md`'s Vision Pro
  mount row and `canary_vision_pro_recamera.md` now say "mesh-checked".
  (#1704)
- [x] **D7 [code] The firmware and Lab READMEs trail the trees.**
  `firmware/README.md` (an architecture tree showing only two boards and two
  `common/` directories, `rf_presence/` and `camera/`, that do not exist; a
  build-target table without the four newer projects; a canary env table
  without `usb-onboard`, the reach ports or the secure pair, and silent on
  which envs CI builds; the display called PlatformIO-only; FULL + OTA said
  to need 16 MB; a "single supported" fleet dashboard), the display README
  ("Two hardware flavors", a v0.1 status banner, no Location page, the SD
  deep archive called dash-only), the WAP README (an API table with a route
  that does not exist (`/api/health`), a non-route (`/api/witness/export`)
  and two wrong methods, five routes missing, an example that gets a 401,
  no `usbdrive` row), canary-ota's two-consumer note and
  `VARIANT_POLICY.md`'s canary-ota row ("the ACTIVE variants"),
  `devices/README.md` (three glTF models; the Sentinel manifests' absent
  keys and unbuilt envs) and `canary-local/README.md` ("no fetches outside
  the page's own directory", a "CI-generated" SBOM, a flash.html line
  without broker encryption).
  *Done (#1722):* each rewritten against the tree with its status words,
  counts pointed at `flavors.json` / `devices/` rather than typed;
  `gen_wap.py` regenerates to zero diff.
- [x] **D8 [code] The security docs have not caught up with the flagship's
  page-token gate and HTTPS.** #1691's rewrite, written against an older
  `main`, still says the flagship's page carries its API token to whoever
  can load `GET /` or `/setup` and that no button press releases the
  dashboard (`docs/security/SECURITY_MODEL.md`, "Who Can Access Your
  Data"). #1704 had already made a set-up flagship withhold the token on the
  home LAN (first-boot wizard, bearer, SoftAP peer or one BOOT tap only) and
  serve self-signed HTTPS on 443 in its dev and full builds, and #1691 added
  the `Host` guard beside that policy. `THREAT_MODEL.md`'s Scenario 2 and
  Definition of Done item 7 say "the flagship's port 80" without that
  qualifier, `scripts/tests/test_docs_claims.py`'s ban reasons repeat the old
  posture, and `firmware/scripts/regression_check.sh` labels its display
  egress paths by the retired "disclosed exception" numbering (they are
  outbound paths 3 to 5 now).
  *Done (#1720):* `SECURITY_MODEL.md`, `THREAT_MODEL.md` Scenario 2 and
  DoD 7, and `test_docs_claims.py`'s reasons now say what ships. The token
  rides the page for the first-boot wizard, and after setup only for a
  bearer request, a request over the flagship's own access point, or one
  page load that spends a BOOT tap. The `FEATURE_HTTPS` builds serve
  self-signed HTTPS on 443 from the first boot after setup; CI compiles
  `dev` and `full`, and no workflow builds `dev_ha` or `usb-onboard`.
  #1691's Host guard sits beside that policy. The test bans the two retired
  sentences and a third that its review caught before merge (all four
  HTTPS builds called CI-compiled). `regression_check.sh`'s
  disclosed-outbound labels cite the renumbered paths 3-5. Release images
  stay plain HTTP on port 80, and none of the HTTPS builds has run on
  hardware. No behavior change.
- [x] **D9 [code] The Lab's Vision card says fw 2.2.0.**
  `canary-local/devices/registry.json`'s canary-vision `status` reads
  "released — print-validated enclosure, fw 2.2.0", while the registry's own
  `fw_train` and `firmware/projects/canary-vision/include/canary/version.h`
  name the current train (2.4.15 on 7446893). The Lab's Specs tab prints
  that status. Found by the wave-7 site scout. Say the train or drop the
  version, then rerun the Lab's registry tests.
  *Done (#1720):* the Vision card's status reads "released —
  print-validated enclosure" and names no version. The train is `fw_train`,
  which `canary_local.test.js` already holds to `version.h`. A new test
  there fails when any card's status names an x.y.z other than `fw_train`,
  or an x.y after fw/v/firmware, and the test checks its own scanner on
  eight spellings. `registry.json` is hand-authored: every generator reads
  it and none writes it. `gen_vision.py` embeds the status, so
  `vision.json` is regenerated; every other registry reader reproduces its
  committed bytes. The fix reaches the Lab's Specs tab on Pages at the next
  deploy, and the desktop Lab at its next release.

---

## 7. CI hygiene (monorepo workflows)

Workflow debt no section above owns: path filters, interpreter pins and the
host-test list. The rules these items apply are `.github/CI.md`'s.

- [x] **CI1 [code] CI hygiene the 2026-09 waves left.** The hub plan's
  docs-prose gate walked every `docs/**/*.md` but ran only in
  `canary-local.yml`, whose filter names a dozen docs, so a PR touching
  only other docs never ran it. `canary-local.yml`'s two path lists missed
  files its logic tests read, several of them opened by #1703/#1704 (the
  durable fix is CI2). `ios-selfheal.yml`'s PR compile did not fire on the
  files outside `ios/` that its XCTests read by `#filePath`. Every explicit
  `python-version` pin (six on 7446893) gave no reason, although R9 asks
  for one. hub-core's test matrix built its crates with no cargo cache, and
  jobs that run `node` used the runner image's node instead of setting one
  up.
  *Done (#1722):* the hub plan's docs-prose gate moves to lint.yml
  (`scripts/tests/test_hub_plan_prose.py`). canary-local.yml's filter now
  covers the 23 files its logic tests opened outside it (counting
  `desktop/src/models/` as one). Seven of those gaps were opened by
  #1703/#1704, among them device_models.test.js, which newly reads
  `desktop/src/models` and `fig3d.js`. ios-selfheal.yml fires on the 15
  out-of-tree files its XCTests read by `#filePath`. The six explicit
  Python pins either say why (three stay on 3.11) or read pyproject, and
  the reason half of R9 is machine-checked. hub-core's test matrix has
  rust-cache. Every job that runs node sets it up. The five that ran the
  image's node are now on 22, and the three node-20 release-path jobs are
  named in CI.md as the known exceptions. docs/ci.md no longer types a
  check count. The cache is proven when a second push restores it, and the
  scheduled and release-only jobs on their next run.
- [ ] **CI2 [code] canary-local.yml's path filter is hand-kept, and it
  drifts.** Found in the wave-7 reconcile and its review. On 7446893, 23
  files its logic tests open were outside it, 7 of them opened by
  #1703/#1704, and #1722 lists them all. The reconcile's string scan found
  19; a trace found the other 4, which the tests build with join(). Still
  outside after #1722:
  - 27 existence probes: 6 `boards/vendor/*.step.gz`, 15 docs/hardware
    research and reCamera docs, the fence-guard README, `docs/README.md`,
    `docs/device_trust.md`, `docs/firmware_ota.md`,
    `docs/lovelace_timeline.md` and `LICENSE`.
  - What the job's drift-step generators read: `gen_flash.py` opens 128
    files under `firmware/canary/**`, plus `firmware-release.yml`, `LICENSE`
    and `platformio_secure.ini`; `gen_operator.py` reads
    `docs/design/vault_operator_ux_v1_1.md`; `gen_figures.mjs --check` reads
    the canary-ota and canary-sentinel `platformio.ini`.
  - `tvos/witness-core/tests/fixtures/fleet_contract_vectors.json`, which
    `canary_local.test.js` has read for A24's replay since #1720 (the trace
    above ran without it).
  - New readers still arriving: open PR #1702 adds one (the src-tauri glob
    already covers it).

  The fix is a gate that traces every logic-tests command, the generator
  drift steps included:
  - Node runs under a `--require` preload that records every repo path
    passed to fs.* and every module load. The preload calls
    `module.syncBuiltinESMExports()` so ESM imports are seen.
  - Python runs under `sys.addaudithook` ('open').
  - It fails on any content read outside the workflow's pull_request paths
    (R6 keeps push equal), naming the test, the file and the line to add.
  - It reports existence probes, or allowlists each with a reason.
  - It runs in the logic-tests job, so a new reader fails in the PR that
    adds it.
  - Allowlist: the job's own outputs and tmp dirs.

  The same idea applies to ios-selfheal.yml's `#filePath` readers.
- [ ] **CI3 [code] One host-test list.** On 7446893, `firmware.yml`'s Mesh +
  Scout host-test job compiles 34 tests_host sources inline (canary-display
  14, canary-wap 17, canary-tincan 2, canary-companion 1). 19 of those tests
  are built nowhere else (canary-wap 14, canary-display 5), and 15 duplicate
  a Makefile rule (canary-wap 3, canary-display 9, canary-tincan 2,
  canary-companion 1). The tincan and companion Makefiles are never run by
  CI: `make -C` names only the canary-wap, canary-display and
  `firmware/tests_host` ones. Every host test main added in #1703-#1718 went
  into a Makefile. Fix: move the 19 into their Makefiles with per-rule
  `-Werror` (main's canary-wap pattern), and delete the inline steps. The
  witness-page step keeps `gen_witness_page_v1.py --check` as a trimmed
  step. CI runs `make -C` for all five host-test Makefiles. Add
  `scripts/tests/test_host_test_lists.py` so the list cannot fork again
  (the same idea as the `MESH_TESTS` guard in that job).
  — in progress (2026-09-23)

---

## Canonical ledgers (do not duplicate — point here)

| Ledger | Governs |
|---|---|
| [`IMPROVEMENT_ROADMAP.md`](../IMPROVEMENT_ROADMAP.md) | The 2026-09-02 audit's open residuals (device package, TLS bench passes, platform pins) |
| `firmware/ESP32S3_OPTIMIZATION_ROADMAP.md` | The 30 prioritized firmware defects (8 P0) |
| `firmware/FEATURES.md` | Arduino↔PIO parity dashboard |
| `firmware/canary/CONSOLIDATION.md` | Consolidation phases + gap inventory |
| `firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md` | WAP enterprise checklist |
| `docs/hardware/dev_playground_todo.md` | Bench-gated capability list (U1) |
| `docs/audit/hardware_verification_checklist.md` | The hardware verification cases (U1) |
| `docs/hardware/enclosure/AUDIT_2026_09.md` | Enclosure open themes (C1–C3, C10–C12) |
| `docs/RELEASE_BUTTONS.md` | Release/ship operations (U2) |
| `securacv_website/docs/render-roadmap.md` | 3D/AR quality roadmap (W3, W4, W5, W9, W12, W13) |
| `securacv_website/docs/roadmap.md` | Website roadmap, coming-soon surfaces (refreshed by W2, website PR #200) |

**Provenance.** Findings confirmed by source inspection during the
2026-09-20 sweep (five parallel
audits over the three repos; classic TODO-marker counts were near zero — this
project records debt as honest-status prose and checklists, which is what this
file indexes). Items listed here were spot-checked against source at sweep
time; re-read the source before building on a line.
