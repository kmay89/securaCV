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
  resync in securacv-homeassistant#17 (2026-09-24). HA14 moved the carried
  `custom_components/securacv` files again in #1725 (and F55 one carried
  test), resynced in securacv-homeassistant#19, and HA18, HA17 and HA22
  moved them again in #1727, resynced by hand in securacv-homeassistant#20
  (2026-10-01). #1761 adds one carried test (F46's
  `tests/test_replay_one_id_space.py`), which waits for the next resync.
  Until the secret is set, every `main` change to the carried set needs
  that again.
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
  canary-wap HTTPS note; PARITY_PLAN's shared TLS line); and the bench, U1
  runbook Track D, D1–D5.
  *mDNS TLS advertisement done (#1757):* the canary's `_securacv._tcp`
  record now carries a `tls` TXT ("1" when the HTTPS server actually came
  up, "0" otherwise — its absence means firmware predating this, not plain
  by choice) and a `secure_port` TXT when live, so a discovery client (the
  Lab, the Flasher) can tell HTTPS is on without probing. The decision is
  the pure, host-tested `tls_policy::mdns_tls_advert`; `begin()` announces
  before the server exists (tls=0) and `startHttpServer()` re-announces once
  TLS is up, which the STA_GOT_IP re-announce then carries to the home-WiFi
  interface. The plain `http`/`securacv` services keep advertising 80 (it
  307-redirects) for a client that cannot do TLS. Host-tested
  (`test_tls_policy`), canary `[env:full]` compiles. Not bench-verified (U1).
- [ ] **F62 [code+decision] Consume the mDNS TLS advert in the desktop
  clients.** Found by the #1757 review: the canary now advertises `tls` /
  `secure_port` over mDNS (F15), but no in-repo discovery client reads them
  — the Flasher's `desktop/src-tauri/src/fleet.rs` serializes neither into
  `FleetSighting`, and `desktop/src/app.js` still builds an `http://` URL
  from the port-80 SRV record (which 307-redirects to a self-signed 443 the
  default reqwest trust policy then rejects). Closing the loop needs: the two
  fields carried through `FleetSighting` in BOTH the Flasher and the Lab's
  twin (`desktop-lab/src-tauri/src/fleet.rs`, held equal by
  `desktop_parity.test.js`); the frontend transport decision to prefer
  `https://<secure_port>` when `tls="1"`; and a client TLS-trust model for
  the self-signed cert — a TOFU pin store or an explicit accept-with-pin
  flow (security-sensitive, a maintainer decision). Pre-existing gap, not a
  regression; deferred from #1757 to keep that PR to the advertisement half.
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
- [x] **F53 [code] canary-wap's NvsManager copy has the same shared handle.**
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
  *Done (#1725):* traced, and more than one task holds a session: the loop,
  the API httpd task, the NimBLE host task (a new bond's pairing record, BLE
  Wi-Fi provisioning), the Bluetooth bring-up task and the QR-scan task.
  F52's lock is ported: `nvs_session_depth.h` moved to
  `firmware/common/storage/` (the canary includes it from there, unchanged)
  and is staged next to the sketch (`setup.sh arduino`, held byte-identical
  by `check_csi_sync.sh`), and `nvs_store.h`'s `NvsManager` holds the same
  bounded recursive mutex from `begin()` to the matching `end()`, with the
  2 s wait static_asserted under the loop's watchdog in `canary_wap.ino`.
  The vault's five sessions that never ended, a lockout under the lock, now
  end. Host-tested on the real header (`test_nvs_store_lock`, twelve mutants
  caught), and `test_nvs_session_balance` is a textual scan of the sketch
  that fails on a block that opens a session and never ends it, or that
  returns inside one without ending it (a session ended in only one branch,
  or left by a `goto`, gets past it: F60). The glue compiles only in CI's
  WAP PlatformIO and Arduino CLI legs, and the bench rows in
  `hardware_verification_checklist.md` are open.
- [x] **F55 [code] The canary's NVS writes report success whatever the
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
  *Done (#1725):* `nvs_store_u32` / `nvs_store_bytes` return true only when
  the put wrote the whole value, and `witness_persist_chain_state()` settles
  every write through `common/witness/chain_persist.h`: only a write that
  landed moves `seq_persisted` and counts in `chain_persists`; one that did
  not is counted in the new `chain_persist_failures` (MQTT health, beside
  `chain_persists`, the only place that counter is exposed), leaves
  `seq_persisted` where it was, and is retried on the next record, then once
  per interval while the failure lasts (so a full partition or a leaked NVS
  session's 2 s wait is not paid on every record); a failure streak is
  logged once when it opens and once when a write lands again. The genesis
  write at boot takes the same path. The helpers' other callers now read the
  result: the birth stamp no longer writes its day after a flag that did not
  land, claims no stamp NVS lacks and retries a minute later, and a boot
  count that did not land says so. Host-tested on the firmware's own code
  (`test_nvs_store_result.cpp`, `test_chain_persist.cpp`: the helpers and
  the witness glue cut out verbatim; the old bodies fail 9 and 38
  assertions, a retry on every record 28); compiled by CI's canary envs on
  #1725; no bench pass. Still open: `nvs_store_key()` has the same unread put,
  but an honest result there halts provisioning (F58, maintainer to choose);
  canary-wap's own copies of these helpers and its `persist_chain_state()`
  carry the same defect (F59); the canary's other direct NVS put calls (64 in
  15 files, a few of which already read their result) were not audited (F61).
  The Wi-Fi save this item names is one of them and is unchanged:
  `ScvNetworkManager::saveCredentials()` still ignores its puts, and
  `handle_wifi_connect` ignores its result; F61 starts there.
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
  receipt route (F57), and consider refusing browser cross-site requests
  before either tap path takes the tap.
- [ ] **F57 [decision] canary-wap's receipt route and bearer-gated API do not
  refuse a foreign Host.** F56 gave the canary's receipt the Host-first order
  its `auth_gate` routes already had; on the WAP, neither the receipt nor the
  bearer check (`api_auth.h`) reads the Host. `handle_provisioning_receipt`
  (`canary_wap.ino`) serves on a bearer (`api_auth_check_optional`) or takes
  the BOOT tap (`provisioning_gate_take()`) without reading the Host, and
  nothing under `firmware/projects/canary-wap/` includes
  `firmware/common/network/host_guard.h`. #1722 left the WAP out, recording
  that a port needs a decision on the captive portal. The follow-up proposed
  with F56's fix has the canary's shape: stage `host_guard.h`, refuse a
  foreign Host first with the SoftAP exempt, and extend the WAP's own
  `check_route_security.py`. That decision comes first. The canary exempts
  every request from its own AP subnet (`host_is_foreign`'s `from_ap_subnet`),
  but in standalone mode the WAP's AP is the product (`canary_wap.ino` says so
  where it restarts the captive DNS), so the same exemption would exempt every
  client it has. And the WAP runs its captive DNS for the whole life of the
  AP, pointing every name at itself, so captive-portal assistants arrive under
  foreign Host names; its `/` redirect and `request_host_is_direct` (the
  wizard's `/api/wifi/pair-token`) already tell those apart from the owner's
  browser by `canary.local` / `192.168.4.1`. Decide which interface and which
  names the WAP trusts, then port it with its route checker. Found in the
  wave-7 receipt review (F56).
- [ ] **F58 [decision] The canary's identity-key store still reports success
  whatever the write did.** `nvs_store_key()`
  (`firmware/canary/lib/securacv_crypto/src/securacv_crypto.cpp`) ignores
  `putBytes`' result; F55 left it alone on purpose. Its caller,
  `witness_provision_device()`, stops provisioning on false ("provisioning
  cannot continue"), so an honest result there halts a device on a full NVS
  instead of booting it with a key the next boot will not find (a new device
  id and a broken Home Assistant pin). Maintainer to choose: halt, or boot
  with a loud, counted ephemeral identity. Found doing F55.
- [x] **F59 [code] canary-wap's NVS writes report success whatever the write
  did.** (#1749) `canary_wap.ino`'s `nvs_store_key` / `nvs_store_u32` /
  `nvs_store_bytes` and `tls_store_to_nvs` return true once the session
  opens, `persist_chain_state()` advances `seq_persisted` and
  `chain_persists` regardless, and `note_wall_clock()` writes the birth pair
  unread. The chain persist also still writes the two-entry seq/chain pair
  F38 (b) retired on the canary, in two sessions, and the reboot's persist
  on the httpd task can interleave with the loop's (F53's lock serializes
  each session, not the pair). Port F55:
  `firmware/common/witness/chain_persist.h` is shared-ready (a staged copy
  and a sync check for the sketch). The WAP's provisioning also stops when
  its key store returns false, so an honest `nvs_store_key` there is F58's
  choice too. Found doing F55 and F53.
  *Done:* `nvs_store_u32` / `nvs_store_bytes` / `nvs_store_token` /
  `tls_store_to_nvs` and `wifi_save_credentials` now answer true only when
  every put landed (the Wi-Fi connect route answers an error on a failed
  save — the WAP twin of F61's named start); `note_wall_clock` is the
  canary's honest version (no half-stamp, one report, minute retry);
  `persist_chain_state()` writes chain_state.h's single atomic blob under
  `chain_st` and settles through chain_persist.h (streaks counted in the
  new `g_health.chain_persist_failures`, retried per its rules), which also
  retires the reboot handler's inline pair — the httpd/loop interleave has
  no two-entry window left to tear; boot resumes via `chain_state::choose()`
  (legacy pair read-only). Both headers are staged copies held by
  check_csi_sync.sh. `nvs_store_key` deliberately keeps ignoring its put —
  F58's pending halt-vs-ephemeral call, same posture as the canary's.
- [x] **F60 [code] canary-wap's NVS session-balance check is textual.**
  (#1749)
  F53's `test_nvs_session_balance`
  (`firmware/projects/canary-wap/tests_host/`) reads the sketch's sources as
  text: it fails on a block that opens a session and never ends it, or that
  returns inside one without ending it. A session ended in only one branch,
  or left by a `break` or `goto`, gets past it, and so does a session opened
  through a pointer or a second name. Under F53's lock a session that never
  ends shuts every other task out (each waits 2 s, then fails soft). Close
  the gap with an RAII session guard in `nvs_store.h` or a real control-flow
  check. Found doing F53.
  *Done:* `NvsMainSession` (nvs_store.h) is the RAII guard — constructor is
  begin(), destructor the end() on every path — and all 25 session sites in
  the sketch (canary_wap.ino, bluetooth_channel.cpp, vault_snapshot.cpp)
  plus the nvs_store:: helpers use it; the dead `nvs_open_rw` /
  `nvs_open_ro` / no-arg `nvs_close` wrappers are deleted. The balance
  test's second edition enforces the rule that makes the RAII sound and IS
  textually decidable: no sketch source outside nvs_store.h names
  NvsManager at all (comments/strings stripped; ESP-IDF's own
  `nvs_close(handle)` exempt), and the guard's begin-in-ctor/end-in-dtor
  lines are pinned. test_nvs_store_lock runs the guard's scenarios on the
  real header (scope close, nested depth, cross-task fail-soft owing no
  end).
- [x] **F61 [code] The canary's other NVS puts are unaudited for a write that
  did not land.** (#1753) F55 made the chain-state helpers honest; the 64 direct
  `Preferences` / `NvsManager` put calls in 15 files of `firmware/canary`
  (outside `securacv_crypto.cpp`) were out of its scope, and a few already
  read their result (`csi_event_egress.cpp`, `mesh_state.cpp`). Start with the
  Wi-Fi save F55 named: `ScvNetworkManager::saveCredentials()`
  (`firmware/canary/lib/securacv_network/src/securacv_network.cpp`) ignores
  what `putBytes` / `putBool` returned, marks the credentials configured, logs
  'WiFi credentials saved' and returns true, and `handle_wifi_connect` ignores
  even a false, so a Wi-Fi save the API reported as done can be silently
  dropped; have the route answer an error when the save fails. Then audit each
  of the rest for a state it claims after a put NVS refused. Found doing F55.
  *Done:* every direct put site audited (putChar included — the item's 64
  under-counted by missing it). Seven claimed state over a refused put, now
  honest: `saveCredentials()` answers true only when every entry landed
  (WARNING otherwise, the WAP's F59 wording) and `handle_wifi_connect`
  snapshots the manager's credentials, restores them on a failed save and
  answers `ok:false` instead of "Connecting..." — setup stays incomplete and
  the retry tick cannot connect with credentials the answer called unsaved
  (the WAP route's Codex rollback, ported); `audio_save_mute_intent` returns
  the put's verdict (the route's `"persisted"` field was already wired to it,
  so the mic-privacy claim is now real) and WARNs for the result-ignoring
  MQTT path; the thermal watchdog's `save_nvs()` clears `s_dirty` only when
  all eleven entries landed, so a failed history save retries next cadence
  instead of going silently final; the camera's orientation print and the
  vision config print say NOT saved on a refused put (vision's return was
  already honest, its log was not); `setup_set_device_name` and
  `setup_set_tz` keep the live RAM state they really applied but log a
  WARNING naming the failed persist instead of "updated"/"set". Audited and
  left as-is, with reasons: mesh_state.cpp (all ten sites), ble_scout.cpp,
  ble_scout_key.cpp, mqtt's `write_credentials`/TLS writes and auth's bearer
  persist already read every result; power's cycle/brownout/history saves,
  canary_power_events.h's lineage log + heartbeat, and diagnostics'
  self-verifying `test_nvs` probe are best-effort telemetry that claims
  nothing and retries on its own cadence; `setup_mark_complete` /
  `setup_check_timeout` fail toward re-entering setup, which is fail-safe.
  Not bench-verified on hardware (U1).

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
- [x] **F46 [code] The CSI bundler's event ids live outside the chokepoint
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
  *Done (#1761):* one event-id space, in both trees. Every committed
  csi_event, bundled or direct, takes its id when it commits, from
  csi_event.cpp's one allocator (`commit_row()`). An open bundle has no
  event id, only a handle in [0x80000000, 0xC0000000). That handle is what
  `csi_event_emit` returns for a buffered emit, and the `id` of an
  `"open":1` row on the canary-wap's `/api/events/today`.
  `csi_event_inject` refuses a card line in that range, so no restored row
  shares an open row's id. The allocator starts at 0xC0000000
  (`csi_event_id_floor.h` `kIdSpaceBase`) on every device, above every id
  an older firmware handed out. So an upgraded device's next id is above
  Home Assistant's stored mark, and nothing is reset: not `csi.evsent`, not
  HA's mark. HA is unchanged; `tests/test_replay_one_id_space.py` drives
  its real gate across restarts. The space holds 2^30 ids. That is about
  16.5 years at the most a device can commit (178,200 a day: the uncapped
  `wifi.channel_activity` at one row a second and the other 15 modules at
  the 255/hour override), and about 75 years at the shipped defaults.
  *Corrected (#<W9>):* this said about 16 years (183,960 a day) and about
  65 years. Those figures counted 5,760 a day of bundles reopened on a
  refunded refresh, a leak F80 closed. Under a boot every 5 s it lasts
  about 17 years, at the NVS write rate F29 accepted. At exhaustion ids
  restart at 1 and HA refuses the device, with nothing on the device to say
  so (F82). `test_csi_event_id_floor.cpp` pins the numbers. Both hosts
  restore `boot_floor(floor, csi.evsent)`: held at or above the delivery
  ceiling, never past `kHoldLimit` (0xF0000000). Both backfills restore
  their watermark with `csi_event_backfill::restore()`, which treats a
  ceiling `boot_floor` did not follow as no record. So a device whose
  ceiling an older firmware pushed to the top for a forged card line
  resumes its backfill once HA is re-pinned. A recursive commit lock spans
  allocate-and-hooks, because ble.scout commits from the NimBLE host task
  in both trees; `firmware/scripts/check_csi_commit_order.py` (13
  mutations, run by `check_csi_sync.sh`) holds that shape.
  `csi_event_log_line.h` reads id/first/last as uint32 (strtol saturated at
  2147483647 on the ESP32) and refuses a sign or an overflow. Neither
  backfill (the canary's planner, the canary-wap's `iterate_since`) sends
  or credits a card line at or above the allocator's next id, so a forged
  id cannot push HA's mark or the id floor toward the wrap. Below that
  bound the SD log is still trusted input (F79).
  `test_one_bundled_row_moves_the_watermark_for_good` is replaced by
  `test_upgrade_from_a_bundler_space_ceiling`. `test_csi_event_id_space.cpp`
  runs the real library across interleaved bundled and direct rows,
  out-of-order closes and reboots; all six of its tests fail on the old
  library. The forged-id, handle-range and poisoned-ceiling tests each fail
  on the code before their fix. Residual: HA refuses a device whose mark an
  older firmware pushed near the top of the space until the owner re-pins
  it. The HACS mirror carries the new HA test on its next resync (U6);
  mirror PR securacv-homeassistant#21, open for #1727's files, does not
  carry it. The emulator dist does not move. Host-tested; the ESP32
  compile is CI's; not bench-verified (U1: the F46 rows in
  `hardware_verification_checklist.md`). Found here: F77-F83 and HA23.
- [x] **F47 [code] canary-wap's backfill watermark lives in RAM.** (#1754)
  Found by
  F37 (#1718). Its first reconnect after every boot replays up to 64 ids
  Home Assistant refuses. It could adopt the canary's
  `common/csi/src/csi_event_backfill.h` (NVS ceiling, id-floor cap). A
  smaller one found in the same review: when an unbuildable row is the
  last whole line before a power cut's torn tail, the canary's walk stays
  pending and re-reads the fragment about once per loop pass until the next
  committed row seals it. Nothing is lost, and the next row goes out.
  *Done:* csi_mqtt.cpp adopts the header's ceiling rule over its own
  iterate_since backfill: `persist_delivered_ceiling()` (must_persist
  cadence, `ceiling_for`'s id-floor cap via the new
  `csi_integration::event_id_floor_stored()`, same `csi.evsent` key as the
  canary's egress) runs in `publish_and_advance` BEFORE the id is handed
  over, and `init()` restores the watermark from the ceiling — max()'d, so
  a config-POST re-init never moves it back — with Planner::begin's
  first-boot rule (everything below the restored id floor treated as
  delivered, the record written then). The torn-tail re-read is fixed in
  the canonical Planner: the walk parks on a torn tail (`m_torn_size`) and
  reads again only when the log grows, cleared on card open/close and
  retention cuts; `test_torn_tail_is_parked_not_reread_every_pass`
  reproduces the reboot-then-spin scenario and pins zero reads while the
  log stands still (432 checks). Staged WAP copy re-synced. Not
  bench-verified on hardware (U1).
- [ ] **F77 [code+decision] Closed bundles never reach the event ring.**
  `csi_bundler.cpp` commits a closed bundle through
  `csi_event_commit_bundle_()`, which runs the commit hooks but not
  `persist_to_ring()` (F46 did not change this). So a presence or tamper
  row leaves `/api/events/today` the moment its bundle closes. It is never
  on the canary-wap's SD log either, because that hook appends only what
  `csi_event_find` sees, so the WAP's backfill never replays it. The daily
  summary misses it too: `meta_daily_summary.cpp` (both trees) counts its
  active and quiet periods from `csi_event_recent()`, and those are
  core.presence states, which always go through the bundler. Three places
  say otherwise: the 2.4.15 release notes ("Closed bundles reach the event
  ring, so `/api/events/today` and the daily summary see them"), the WAP
  dashboard's comment ("the dismiss appears when the bundle commits",
  `csi_dashboard_html.h`) and `docs/csi_developer_api.md`'s example of a
  committed bundled row. A fourth, the roadmap's Wi-Fi sensing row ("closed
  bundles reach the event ring"), is corrected in #1761. In a host probe,
  one buffered emit then a flush leaves `csi_event_recent` returning 0. F46
  removed the old reason (ids from an unpersisted space). Putting closed
  bundles in the ring changes four things together: the Today sheet, the
  daily summary's counts, the ring's live-row latch for `csi_event_inject`,
  and what the WAP replays. Decide them together. Found by F46 (#1761).
  *Since F81 (#<W9>):* the canary commits one row per closed bundle (at most
  ten minutes long, every observation in `bundled`, the span in
  `duration_sec`) instead of one per refresh, as the canary-wap does. So once
  closed bundles reach the ring, the daily summary's active and quiet counts
  count bundles: a state held for three hours is about 18 rows, one per
  10-minute window, and core.presence commits at most six an hour (F90). The
  summary would have to join consecutive same-state rows (or sum
  `duration_sec`) to count periods, and a row's span can take in a brief other
  state (a return within two minutes joins the open bundle). On the canary the
  inject latch and the Today sheet do not apply: it calls no
  `csi_event_inject` and serves no `/api/events/today`. On the canary-wap, a
  closed bundle put in the ring would set `g_ring_has_live` like any live row.
- [ ] **F78 [code] canary-wap's live publish overtakes its own backlog.** On
  reconnect, `MQTT_EVENT_CONNECTED` sets `s_connected` on the esp_mqtt task
  and only flags the backfill for the loop task. A row committed before
  `csi_mqtt::loop()` drains it goes out live through `publish_and_advance`,
  which moves `s_last_published_event_id` past every unsent row.
  `iterate_since` then skips those rows, and HA would refuse them anyway.
  The canary's planner holds new rows behind the backlog for exactly this
  reason (F37); the WAP does not. Also, the live path runs on whichever
  task commits (the NimBLE host task for a ble.scout close, now under the
  chokepoint's commit lock), while the backfill runs on the loop task, and
  both write `s_last_published_event_id` and `s_delivered_ceiling` with no
  lock between them. Adopting `csi_event_backfill::Planner` on the WAP, as
  F47 suggested, would fix both. Found by F46 (#1761).
- [ ] **F79 [code+decision] Below the backfill's bound, the SD event log is
  trusted input.** Both backfills (the canary's
  `csi_event_backfill::Planner`, the canary-wap's `iterate_since`) refuse a
  card line at or above the allocator's next id (F46). They sign and send
  whatever a line below that bound says: its content, and its id, including
  an id a reboot skipped or one the allocator had passed by the time the
  walk read it. A forged line written ahead of real rows also moves the
  watermark past them, so the real rows behind it are never delivered
  (review probes on the real planner). Options: a per-line MAC under a
  device key, or replaying only rows the witness chain vouches for. Not
  tracked in the roadmap or the gaps ledger. Found by F46's review (#1761).
- [x] **F80 [code] A refresh refund can reopen a bundle without spending
  the hourly ceiling.** `csi_event_emit` refunds an emit's ceiling slot
  when `csi_bundler_has_open()` finds its key. But `csi_bundler_admit()`
  then expires that slot (a gap of at least `CSI_BUNDLER_MAX_GAP_MS`, or
  the 10-minute window) and opens a new bundle, which commits as a row the
  ceiling never counted. In a host probe on the real library, one
  state-bearing emit every 121 s commits 714 rows a day under a 6/hour
  ceiling (144 allowed). The canary hides it by flushing every bundle every
  window (F81); the canary-wap only ticks the bundler, so it is exposed.
  F46's id-space headroom counts the leak. Fix: decide the refund from
  admit's outcome (merged or opened), not from `has_open()` before it.
  Found by F46's review (#1761).
  *Done (#<W9>):* the ceiling is spent by what the admit did.
  `csi_bundler_admit()` reports `CSI_BUNDLER_OPENED` (a new bundle, a future
  row: the emit keeps its slot) or `CSI_BUNDLER_MERGED` (rolled into the
  bundle that was still open: the slot goes back), decided under its slot lock
  after overdue bundles expire. `csi_bundler_has_open()` is gone, and
  `CSI_BUNDLER_BUFFERED` is renamed `CSI_BUNDLER_OPENED`.
  `firmware/tests_host/test_csi_bundle_ceiling.cpp` drives the real library
  under a fake clock. One emit every 121 s with nothing ticking commits 144
  rows a day under a 6/hour ceiling (714 before). A 60 s refresh beside one
  new state every 10 minutes commits 144 (286 before). A refresh that merges
  still gives its slot back.
  Correction to the item: the canary-wap was less exposed than it says. A host
  that ticks every loop pass mostly closes an overdue bundle before the next
  emit can reopen it: with a 1 s tick the 121 s probe commits 144 a day on the
  old library too. So the canary-wap leaked only when an emit reached the
  expiry in the same pass as its tick (a window-rollover refresh, or an
  arrival on the NimBLE task). The canary, once it ticks (F81), runs its
  window's emits before the tick and would have refunded every 10-minute
  reopen, which is why the two land together.
  With the leak gone F46's headroom loses its refund term: at most 178,200
  rows a day, about 16.5 years (6,025 days), and about 75 years at the shipped
  defaults (`csi_event_id_floor.h`, pinned in `test_csi_event_id_floor.cpp`;
  F46 and F82 are corrected). The canary-wap's staged copies are re-staged.
  Host-tested; the ESP32 compiles are CI's; not bench-verified (U1: the
  F80/F81 rows in `hardware_verification_checklist.md`). Found here: F90.
- [x] **F81 [code] The canary flushes every open bundle on every CSI
  window.** `firmware/canary/src/csi_modules_integration.cpp` calls
  `csi_event_flush_bundles()` (close all) after each module tick. Its
  comment says it drains bundles "whose 10-minute window has elapsed",
  which is what `csi_bundler_tick()` does, and the canary-wap calls that.
  So the canary never refreshes an open bundle: each core.presence refresh
  commits a new row and spends the hourly ceiling. The 2.4.15 release
  notes say "Same-state refreshes no longer spend the hourly ceiling"; on
  the canary they still do. Switch to
  `csi_bundler_tick()` together with F80, since the flush is what hides
  that leak on the canary. Found by F46 (#1761).
  *Done (#<W9>), with F80:* the canary's `securacv_csi_modules_feed()` closes
  no bundle. `securacv_csi_modules_tick()`, new in the bridge, runs
  `csi_bundler_tick()`, and `main.cpp`'s `loop()` calls it once per pass,
  outside the CSI power and degrade gates and before
  `csi_event_egress_pump()`, where the canary-wap's loop ticks. So a bundle
  opened before CSI is shed still commits on its window or quiet gap, and its
  row goes out in the same pass.
  - `firmware/tests_host/test_csi_modules_integration.cpp` builds the canary's
    real bridge over the real library and modules (stubbed Arduino,
    Preferences and csi_hal hooks, a fake clock) and plays a stand-in for
    `main.cpp`'s loop. A refresh inside the window commits no row and spends
    no ceiling (the old bridge: 6 rows in 590 s, then 0 of 5 new states
    admitted). A bundle commits once, on its quiet gap or its window, with
    every observation and its span, and it commits while CSI is shed. All four
    tests fail on the old bridge. They pin the bridge's split, not
    `main.cpp`'s call.
  - `main.cpp`'s call is not host-tested: no suite compiles `main.cpp`.
    `firmware/scripts/check_csi_bundle_tick.py`, run by `check_csi_sync.sh`,
    holds it: one call, directly under `#if FEATURE_CSI`, a top-level
    statement no gate controls, nothing leaving the loop before it, before the
    egress pump, and a feed that closes nothing (16 mutations refused). CI
    compiles it.
  - The events body's `bundled` is now the row's own count on every path on
    both trees (`csi_event_wire::bundled_on_wire()`, pinned in
    `test_csi_event_wire.cpp`). The canary's live and queued bodies said 1, as
    the canary-wap's live body always had. A direct row's replay body now says
    1 where it said 0. `bundled` is not in the signed canonical, so no
    signature moves.
  What changes downstream: every canary row that names a state, except a
  `system.integrity` tamper (which closes its own key), now commits when its
  bundle closes, two to ten minutes after its first observation. An
  `anomaly.baseline` row commits two minutes after its motion, where it used
  to commit within a second (F91), and a row is lost if the device restarts
  first (F92). Its `timestamp` is the close, rows reach Home Assistant in the
  order their bundles close, and a return to a state within two minutes joins
  its open bundle, so a span can take in a brief other state. In Home
  Assistant the canary's last-event sensor (whose value is the body's
  `event_type`, the state name), its timestamp attribute, the voice brief and
  the watches follow that. In a host probe (active 0-100 s, empty 100-130 s,
  active 130-330 s, then empty) the rows were `empty` at 242 s (bundled 3),
  `active` at 432 s (bundled 10, 310 s, spanning the empty stretch) and
  `empty` at 932 s. So the sensor read `empty` while the room was active and
  `active` for about 500 s after it emptied. Before F81 it stuck instead, once
  the ceiling ran out after about three minutes. The motion and occupancy
  sensors map none of core.presence's state names.
  What leaned on the flush, checked: the canary serves no `/api/events/today`
  and calls no `csi_event_inject`, so the Today sheet and the inject latch do
  not apply there. The daily summary counts ring rows, and bundled rows never
  reach the ring (F77), so it is unchanged. Ids still reach the hooks in
  commit order on the loop task (`check_event_egress_order.py`,
  `check_csi_commit_order.py`). The `csi_event_flush_bundles()` doc claimed
  callers no tree has and is corrected; after F81 no firmware calls it. Docs:
  `csi_developer_api.md`, `csi_modules.md`, the F37 and F46 bench rows, a new
  F80/F81 bench section with an anomaly-latency row, and LESSONS_LEARNED. The
  2.4.15 note "same-state refreshes no longer spend the hourly ceiling" now
  holds on the canary too, inside a bundle's ten minutes. Host-tested; the
  PlatformIO builds are CI's; not bench-verified (U1). Found here: F90, F91,
  F92 and F93.
- [ ] **F82 [code+decision] Nothing warns before the event-id space runs
  out.** The allocator has 2^30 ids from 0xC0000000 (F46), about 16.5 years at
  the most a device can commit (since F80, #<W9>). At exhaustion ids restart
  at 1, and each later boot first reissues 0xFFFFFFFF. Home Assistant then
  refuses the device from then on, and nothing on the device says so. Add a
  health or diagnostic flag once the allocator passes `kHoldLimit`
  (0xF0000000), and decide the recovery (a re-pin plus a reset of the floor
  and `csi.evsent`). Found by F46's review (#1761).
- [ ] **F83 [code] The canary-wap can commit an event before its id floor
  is restored.** `csi_integration::init` calls `register_v1_modules()`
  before `apply_event_id_floor_from_nvs()`. `ble_scout_init()` emits
  `initialized("failed")` when `ble_scout_key_init()` fails, and that
  commit's floor write (with `g_id_floor_stored` still 0) overwrites the
  persisted floor before the restore reads it. Only `boot_floor`'s hold at
  `csi.evsent` then limits the reissued ids. It is rare (it needs a
  key-store failure) and older than F46. Restore the floor first, as the
  canary does (`csi_event_egress_begin`). Found by F46 (#1761).
- [ ] **F90 [decision] A held state fills core.presence's hourly ceiling.**
  Every bundle reopening is a row the ceiling counts (F80), and a held state
  reopens its bundle every 10 minutes: 6 rows an hour, all of core.presence's
  6/hour ceiling. core.presence refreshes every state, empty included, so
  after about an hour in any one state the ceiling is full of that state's own
  rows. Its next transition is refused until a slot ages out, and so is every
  refresh of the held state, because the ceiling is checked before the bundler
  runs. On the canary's real bridge (host, fake clock; holds of one to one and
  a third hours in 7 s steps) the transition out waits up to about ten
  minutes, 7 to 602 s depending on when a slot ages out, and every later row
  of the held state carries one observation and 0 s. The canary-wap behaved
  the same before F80: its loop ticks before the window's emits, so its
  reopens were already counted (on the old library, 2 of 17 clock phases
  admitted a transition at once after two hours in one state).
  Options: a ceiling above the window rate (12/hour for core.presence, a
  privacy-contract change); counting a same-key window reopen as a
  continuation; and, whichever is chosen, refusing at a full ceiling only an
  emit that needs a slot, decided from the admit's outcome as the refund now
  is. Found by F80 and F81 (#<W9>).
- [ ] **F91 [decision] Anomaly rows wait in an open bundle.** Every
  non-ambient row with a state goes through the bundler, ANOMALY rows
  included, and commits only when its bundle closes. `anomaly.baseline`'s
  cooldown is 600 s by default, so nothing ever merges into its bundle:
  bundling adds two minutes of RAM-only exposure and nothing else. On the
  canary since F81 an `unusual_motion` row commits two minutes after the
  motion (host, the canary's real bridge: detected at 71 s, committed at 191
  s; before F81, at 71 s), and a restart inside those two minutes loses it
  (F92). The canary has no open-bundle view; the canary-wap's
  `/api/events/today` snapshot shows a live alarm before its bundle closes.
  ANOMALY rows bypass quiet hours because they are urgent (`csi_event.cpp`
  step 4b), and `system.integrity` already seals its own key for exactly this
  reason (`tamper_events_module.cpp` `emit_kind`).
  Options: seal ANOMALY-category rows at commit on both trees, with
  `csi_bundler_flush_key()` after the emit as `system.integrity` does, or by
  having the bundler return COMMIT for them; or keep the bundle and say so.
  The same two-minute-plus wait applies to `core.breathing`,
  `core.multilink_fusion`'s `motion_confirmed`, `meta.empty_room_baseline`
  and, on BLE builds, `ble.scout` arrivals and departures, which are EVENT
  rows. Found reviewing F81 (#<W9>).
- [ ] **F92 [code+decision] An open bundle dies with a reboot.** Bundles live
  in RAM until they close, up to 10 minutes after they open, and no planned
  restart (`deliberate_restart_now()` for `POST /api/reboot`, an OTA, a
  settings restart) closes them first, in either tree. On the canary the
  commit hooks only queue for the loop's egress pump, so a flush before a
  restart would also need the pump to drain before the card or the broker sees
  the row. Until F81 the canary lost at most one CSI window; now it loses up
  to ten minutes of any state-bearing row (a presence state, breathing, an
  anomaly row's two minutes), as the canary-wap always did. Only
  `system.integrity` seals its row at commit. Found by F81 (#<W9>).
- [ ] **F93 [code] Neither tree calls a CSI module's `init()` at boot.**
  `csi_module_register()` only records the module. The canary's
  `securacv_csi_modules_init()` never calls `init`, and the canary-wap calls
  it only in `reinit_module()`, from three HTTP handlers
  (`handle_settings_post`, `handle_calibrate_apply`,
  `handle_tune_post_coefficients`). So core.presence's preset, sensitivity,
  thresholds and pet mode, and every other module's NVS settings
  (`anomaly.baseline`'s cooldown among them), take effect on the canary-wap
  only after a settings change in that boot, and never on the canary, whose
  `csi_module_settings_*` overrides nothing reads at boot. Read from source;
  not checked on a device. Found reading F81's bridge (#<W9>).
- [ ] **F48 [code+decision] canary-wap's mesh crypto and its interop with the
  PIO tree.** Found by F33 (#1718). canary-wap's AUTH exchange still runs
  X25519 over long-term Ed25519 keys, the bug class F33 part 2 fixed for
  pairing, and its rotation encrypts under those session keys. canary-wap
  also HKDFs the pairing key where the PIO tree and spec §5.3 use it raw,
  and its pairing payload structs differ from the PIO tree's, so the two
  trees cannot pair with each other (the outer frame and the type-byte
  numbering they also disagreed on are reconciled by spec §4.5's
  `mesh_wire.h` — host-tested, crypto review pending; the payloads and
  the derivation are what remain). Concurrent removals on canary-wap cannot converge without
  a wire change: `MSG_OPERA_REKEY` names no removed device and has no
  announcement phase. Crypto review and a wire decision first, then code in
  both trees and a cross-tree host test.
- [~] **F49 [code] Mesh leftovers from F33.** (1) The canary's health-log
  list passes `millis()` to `formatTimestamp`, the same uptime-as-time-of-day
  rendering F33 part 7 fixed for alerts. (2) The joiner side's
  `CodeReadyCallback` never fires: the code arrives on `SEND_ACCEPT`, and
  dispatch reports only `NOTIFY_CODE_READY`. The web UI reads the code from
  `GET /api/mesh`, so no user sees it today. (3) There is no radio-MAC
  learning from opera frames, so a changed MAC means a re-pair. (4) Some
  PIO residual splits remain: both initiators already handed out, a mutual
  removal, or a lost ACK. A random-loss probe split 3 of 60 runs at 5%
  frame loss (spec §5.6 states it).
  *Done (#1756), parts 1-2; part 3 withdrawn (#1761):*
  (1) `GET /api/logs` now carries `uptime_ms` (handle_logs) and the log list
  renders each entry's `timestamp_ms` as an age against it (`formatLogAge`,
  shared with `formatAlertAge`) instead of `new Date(...)` — the made-up
  time of day is gone. New host test `test_canary_health_logs.test.js`
  (lifted-and-stubbed, Date poisoned, u32-wrap pinned) in the Makefile and
  firmware.yml's node step.
  (2) `dispatch_action` now fires the `CodeReadyCallback` on the joiner's
  `SEND_ACCEPT` too (its code-derivation beat — there is no separate
  `NOTIFY_CODE_READY` on that side), with the same code `pairing_confirmation_code()`
  reports. Pinned by `test_joiner_offer_surfaces_code_with_accept`.
  (3) **Withdrawn (#1761).** #1756 sent an opera envelope from an address
  the transport did not hold through the full verify (signature + opera_id
  + strict counter). On a pass it re-bound the signer's transport binding
  to that address (`bind_peer_mac`) and persisted it
  (`PeerMacLearnedCallback`, then `main.cpp` `save_peer_mac`). That verify
  does not establish an address:
  - the envelope signs no source or destination;
  - the PIO sender spends one outbound counter across every destination;
  - frames go out unencrypted (`encrypt = false`).
  So any genuine member frame the receiver had not heard passed the verify
  when an outsider re-sent it from its own address. That covers a missed
  broadcast, a rekey frame unicast to another member, or, after the
  receiver reboots, a frame heard since its last 5-minute counter save.
  A host probe against main (c104f56) showed the receiver:
  - moving B's binding to the outsider and dropping B's real address;
  - sending its next rotation OFFER to the outsider alone (the 60 s commit
    then forgot B when B stayed silent);
  - answering a replayed `REKEY_OFFER` with its `REKEY_ACCEPT` to the
    outsider.
  #1756's own test replayed only a frame the receiver had already heard,
  which the counter stops anyway.
  Now:
  - the unknown-sender hook takes pairing frames only again, so opera
    frames from unbound addresses drop as `recv_dropped_no_peer` before any
    verify;
  - `on_opera_frame` is void again, with no `via_unknown`;
  - the callback and `main.cpp`'s save of it are gone;
  - a changed radio MAC means a re-pair, which re-binds an already-trusted
    device.
  Kept: `bind_peer_mac`'s add-before-remove order (#1756 review).
  Pinned by four tests, each failing on #1756's code:
  - `test_unheard_broadcast_replayed_from_a_new_address_moves_nothing`
  - `test_unheard_rekey_offer_replayed_from_a_new_address_moves_nothing`
  - `test_bound_peer_new_address_is_dropped_not_learned` (#1756's test,
    rewritten)
  - `test_repair_moves_a_trusted_peers_address`
  Spec §8.3 now says a verified frame MUST NOT bind an address. How a
  changed MAC could be learned
  safely is F68.
  The review of the withdrawal found pre-existing limits, and they are
  documented here, not fixed:
  - the re-pair it points to does not authenticate the long-term key (F69);
  - on the PIO tree a re-pair cannot start with eight members bound,
    because the transport table has no slot for the new address;
  - the address a verified frame is recorded under can be a pairing
    partner's, with no spoofing (F70).
  Spec §11.1 items 4 and 5 are now marked partial. Host-tested only; not
  bench-verified (U1).
  canary-wap's half (#1761). canary-wap's `handle_received_message`
  re-pointed a member's `mac_addr`, and its ESP-NOW registration, at the
  source of any frame that passed `opera_id`, signature and its per-peer
  counter. A host probe ran the real `mesh_network.cpp` on new host stubs
  (`tests_host/stubs/mesh_net`, `mesh_net_sim.h`: several simulated devices,
  frames carried as bytes). It showed:
  - a frame B sent A that A missed, re-sent from an outsider's own address,
    moved B to the outsider. A dropped B's address from its ESP-NOW list and
    sent its frames for B there until B's next frame (its 30 s heartbeat)
    arrived;
  - canary-wap counts per destination and the envelope names none. So a
    frame B sent C did the same whenever B's counter for C ran ahead of A's
    last-seen for B. A also kept that counter, so B's own frames dropped as
    replays and B stayed bound to the outsider until its counter for A
    passed it;
  - after a power cut, a frame A heard since its last 5-minute counter save
    did the same;
  - from C's copied address, B's next real frame deleted C's ESP-NOW
    registration, so A could no longer reach C.
  Now a frame whose source is not the signer's own bound address drops
  before `verify_signature`. It spends no counter, reaches no handler and
  counts in `auth_failures`. No signed frame writes an address. That is
  stricter than the PIO tree (F70).
  The re-pair path could not re-bind either: `add_peer` appended a second
  entry for a key it already held, which no lookup reached, and a full opera
  refused it. It now re-binds the existing entry:
  - the new address is registered first;
  - an address another member holds is refused;
  - counters, name and state are kept;
  - the move is logged as a health WARNING.
  A changed address needs a re-pair with each member that holds it.
  canary-wap keeps its key in NVS, so a swapped module joins as a new
  member; an NVS image moved to another board, or a relay (F69), reaches the
  re-bind. A full canary-wap opera (16) still takes a re-pair (host-tested
  through `add_peer`, not a full pairing). A duplicate entry the old
  `add_peer` saved is folded into one at boot. It takes the later pairing's
  address unless another member holds it, and the fold is logged.
  Since a pairing now binds addresses, the review ran the pairing handlers
  the same way and found two paths that needed no owner:
  - a joiner took COMPLETE before its owner confirmed. Whoever answered its
    DISCOVER first replaced its opera, or, with the opera_secret and a
    member's public key, re-bound that member;
  - an initiator kept a finished pairing's keys until the 2-minute timeout,
    so a radio that overheard its OFFER got the opera_secret sealed to it
    (pre-existing).
  Both are closed as on the PIO state machine. The joiner takes the first
  OFFER, and a COMPLETE only after its owner confirmed. The initiator takes
  one ACCEPT, from where its OFFER went, and wipes the pairing after
  COMPLETE.
  Pinned by `test_mesh_address_wap` (19 tests against the real file; 15 fail
  on 89a4c56) and by `test_mesh_rx_gates_wap`'s
  `no_frame_moves_a_members_address` (fails on 89a4c56). Spec §8.3, §4.5's
  table, §3.3, §11.1 item 5, THREAT_MODEL and LESSONS_LEARNED are updated.
  Still open on canary-wap: a radio copying a member's own address can
  deliver that member's unheard frames, including ones sent to other
  members. They are dispatched and silence the member until its counter
  catches up (host-probed). Host-tested only; the Arduino compile is CI's;
  not bench-verified (U1).
  All 13 mesh C++
  suites + the webui node tests + the full firmware host suite pass; canary
  `[env:full]` compiles. **Part 4 (PIO residual splits) is left open — it
  rides F48's cross-tree wire decision (a mutual-removal convergence needs a
  `MSG_OPERA_REKEY` wire change), not something to land alone.** Not
  bench-verified on hardware (U1).
- [ ] **F68 [decision] How may a mesh peer's changed radio MAC be learned
  safely?** F49 part 3 (#1756) learned it from any verified opera frame,
  and it was withdrawn (#1761): the envelope signs no address and one
  counter serves every destination, so a replayed frame the receiver had
  never heard re-pointed a member at an outsider's radio. Today a changed
  MAC (a swapped module, a new locally-administered address) means a
  re-pair on the PIO tree, and on canary-wap too since #1761. Options:
  (a) **A signed self-asserted MAC.** The sender puts its own radio
  address in the signed bytes (a payload field or a header field under the
  shared `mesh_wire.h` registry). A receiver binds an address only when the
  frame names it and it equals the frame's source. This is a cross-tree
  wire change, so it is F48's territory. It stops a replay from the
  outsider's own address. It does not stop ESP-NOW source spoofing, but a
  spoofer can then only point the member at the member's own named
  address.
  (b) **A challenge the new address must answer.** A frame from a new
  address for a trusted fingerprint triggers a fresh nonce, and the new
  address must return it signed with the peer's key before any re-bind.
  That defeats replay, but costs new message types (also a wire change), a
  round trip, and a rate limit so strangers cannot make the device sign
  and send on demand.
  (c) **Keep re-pairing.** No wire change, but a re-pair is only as strong
  as pairing. Pairing binds whatever long-term key the DISCOVER or OFFER
  carried, and the 6-digit code covers only the ephemeral exchange. So an
  outsider relaying an owner-run pairing from its own address can re-point
  an already-trusted member at its own radio and persist it, or get its
  own key trusted (F69). On the PIO tree a re-pair also cannot start with
  eight members bound. Choosing (c) means F69 lands first.
  canary-wap's side: it dropped its verified-frame re-bind (#1761), and it
  re-pairs like the PIO tree, one member at a time. Whichever option is
  chosen is carried to both trees. Found by the adversarial
  review of #1756.

- [ ] **F69 [code+decision] Pairing does not authenticate the
  long-term keys.** On both trees the 6-digit code and the CONFIRM hash
  are derived from the ephemeral X25519 session key only.
  - PIO initiator: takes `ctx.peer_pubkey` from the plaintext DISCOVER and
    ignores the ACCEPT's `device_pubkey`.
  - PIO joiner: takes the OFFER's `device_pubkey`.
  - The long-term key signs nothing in the exchange.
  - canary-wap derives its code the same way (`mesh_pair_crypto`). Since
    #1761 its re-pair re-binds a member it already holds. So a relayed
    pairing whose codes match, claiming a member's key, re-binds that member
    to the relay's radio there too, until another re-pair (host-probed);
    before, it added an unreachable duplicate entry. On canary-wap the relay
    is the main way to reach that re-bind: its key lives in NVS, so a
    swapped module joins as a new member. The move is logged as a health
    WARNING. canary-wap's joiner needed no relay at all before #1761, since
    it took COMPLETE before its owner confirmed; #1761 closed that.
  The review of #1761 host-probed the PIO tree. An outsider relays an
  owner-run pairing (A and a new device D) from its own address, no
  spoofing, without touching the ephemeral keys:
  - both screens show the same code;
  - claiming B's key made A re-bind trusted member B to the outsider's
    radio, and main.cpp would persist it, so B's own frames then drop
    until another re-pair;
  - claiming its own key got it trusted, and it then signed a REKEY_OFFER
    that removed B.
  The results were identical before #1756, on c104f56 and on #1761. Spec
  §11.1 item 5 ("Man-in-the-Middle: visual confirmation codes") is now
  marked partial.
  Decide:
  - either bind both long-term public keys into the code and the CONFIRM
    hash, or sign the transcript with each side's long-term key. Either is
    a cross-tree wire change with canary-wap (F48's registry) and needs
    maintainer crypto review;
  - whether a pairing that presents an already-trusted fingerprint should
    re-bind silently. Today the only sign is main.cpp's WARN "Peer pubkey
    persisted but mesh_session register failed (table full or already
    registered)".

- [x] **F70 [code] Record a verified frame's source only from the signer's
  own binding.** `on_opera_frame` records the source of every verified
  frame as the signer's link (`TrustedPeer::mac`). Any address in the
  transport table qualifies, and two need nothing from the signer:
  - while a pairing runs, the partner's address is in the table
    (`ensure_pair_contact`), so an outsider that answers the pairing from
    its own radio can replay a member's unheard frame there with no
    spoofing until the pairing ends (host-probed with the receiver as
    initiator);
  - a radio copying another bound member's address can do the same
    (ESP-NOW does not authenticate a source).
  `send_rekey_frame` unicasts the signer's rekey replies to that link, and
  `forget_peer` removes it from the transport table. So the outsider gets
  the receiver's `REKEY_ACCEPT` (probed), and in the copy case a later
  removal of the signer strands the copied member. This is pre-existing:
  the same before #1756.
  Fix, sketched by the #1761 review and not built:
  - record the link only when the source equals the signer's `radio_mac`,
    or drop opera frames whose source is only the pair contact;
  - have `send_rekey_frame` and `forget_peer` use `radio_mac`.
  This churns tests that inject from hand-added addresses. Documented in
  THREAT_MODEL "Opera mesh", spec §8.3 (peer fields) and the PeerLink doc
  in mesh_session.h.
  canary-wap does not have this: since #1761 it accepts a member's frame
  only from that member's own bound address and records nothing from a
  frame's source.
  *Done (#<W9>):* the PIO session takes a member's opera frame only from the
  member's own bound radio MAC (`radio_mac`). `on_opera_frame` compares the
  source after the fingerprint lookup and before the signature check. So a
  frame from any other address in the transport table (a running pairing's
  partner, another member's copied address) spends no counter, reaches no
  handler and records nothing. A member with no binding (no `peer_macs` entry
  at boot, or a pairing whose bind was refused) is heard from nowhere until a
  pairing binds one. That is canary-wap's rule since #1761, and the stricter
  of the two fixes sketched above. Recording the link only from `radio_mac`
  would still have dispatched the replayed frame, so a `REKEY_OFFER` from the
  pairing partner's address would still revoke a member and join the rotation.
  The frame-recorded address (`TrustedPeer::mac`) is gone:
  - `send_rekey_frame` unicasts to `radio_mac`;
  - `forget_peer` removes only `radio_mac`;
  - `get_peer_links` and `online_peer_count` report `radio_mac` once a
    verified frame has arrived from it, and reset when `bind_peer_mac` moves
    the binding (so after a re-pair the status view no longer shows the old
    address).
  No path binds or moves an address from a frame. Pinned by four new
  `test_mesh_session` tests, each failing on #1761's code (7a0b0db):
  - `test_pair_contact_replay_records_nothing_and_gets_no_accept`, as
    initiator and as joiner (on 7a0b0db the outsider got the `REKEY_ACCEPT`
    and became the member's link);
  - `test_copied_member_address_moves_no_link`;
  - `test_forgetting_a_peer_drops_only_its_own_address` (on 7a0b0db the other
    member's address left the table);
  - `test_unbound_member_frame_is_never_taken`, which also fails with the
    check reduced to "only once bound".
  `test_peer_link_mac_binding` also sends a replayed frame and one of another
  opera from a new binding, and holds that they mark and spend nothing. That
  converts the base's replay assertion, which the first cut had dropped; it
  fails if `heard` is set before the replay or `opera_id` check. canary-wap's
  `test_mesh_rx_gates_wap` pins the PIO receive order to the new rule. Tests
  that injected a member's frames from hand-added addresses now bind the
  sender first, so each refusal is still the check it names. Spec §8.3 and
  §4.5's table, THREAT_MODEL "Opera mesh", the PeerLink doc and the
  `/api/mesh/peers` comments are updated. Still open, as on canary-wap: a
  radio copying the member's own address gets the member's unheard frames
  dispatched (it moves no address, and on this tree the member's later frames
  count above it); closing that takes a wire change (F68, F48). The 13 mesh
  suites, the firmware and canary-wap host suites and the canary-local node
  tests pass. Host-tested only; the `[env:full]` compile is CI's; not
  bench-verified (U1). Found here: F101 and F102.
- [x] **F71 [code] canary-wap's mesh send counters restart at 1 on every
  boot.** `load_peers` sets each member's `msg_counter_tx` to 1, while
  receivers keep and persist their last-seen. So after a canary-wap reboots,
  every member drops its frames as replays until its counter for that member
  climbs back past what the member last saw: one heartbeat per 30 s, for as
  many frames as it sent that member before. Host-probed with the #1761
  harness: after five heard heartbeats and a reboot, the sender's frames
  1..5 dropped and 6 was heard. The PIO tree fixed the same thing in F33
  part 3 (`mesh_out_ctr` reserve-ahead). canary-wap needs it per member, or
  one counter (see the next item). It also decides how soon a member is
  heard after a re-pair, if the member rebooted to change its address
  (`test_mesh_address_wap`'s re-pair test pins that frames 1..3 drop). Spec
  §3.3 now states it. Found by the canary-wap half of the F49 part 3
  withdrawal (#1761).
  *Done (#<W9>):* each member's send counter is reserved ahead in NVS, the PIO
  tree's F33 part 3 done per member. Before `send_to_peer` signs the first
  counter above a member's stored reservation, `reserve_tx_counter` stores a
  new one 1024 ahead, and that one write covers every member past its
  reservation. The store is NVS `tx_ctrs`: one record of an 8 B fingerprint
  and a u64 per member, written by every reservation and by `persist_peers`,
  and not gated on flash encryption (like `replay_ctrs`). A boot
  (`load_tx_reservations`, run before `fold_duplicate_peers`) resumes every
  member one past the highest reservation stored for any of them. That also
  levels the per-member counters, so F72's cross-member gap restarts from
  nothing at each boot instead of growing by up to a block.
  - The first boot after this update finds members and no record. It resumes
    every member above 2^40, which no boot of the older firmware reached, so
    that boot is heard at once too. The floor rests on an argument, not a
    measurement: reaching 2^40 to one member takes 10,000 frames a second for
    over three years.
  - An unreadable record is logged and resumes above 2^48. A second unreadable
    record would resume below the device's own history, and its members would
    drop its frames until each counter climbed back (spec §3.3).
  - A crash between a reservation and its frame leaves a gap of unused
    counters, never a counter signed twice under one opera key.
  - When NVS refuses a reservation, the frame is refused too, and that is
    logged at most once per 5 minutes while the refusals last.
  - `send_to_peer` spends no counter while the storm gate holds
    (`storm_paused`).
  Wear: one write (at most 256 B) per 1024 counters spent to a member. At the
  30 s heartbeat the members' counters cross a block together, about 3 writes
  a day for a whole opera (a full opera of 16 took 3 in a simulated day). Out
  of step it is under 45 a day, against the 288 a day the sketch's 5-minute
  last-seen save already makes. A flood costs about one write per 5 minutes
  (at most 101 counters per 31 s). These are arithmetic and host counts, not
  measured flash wear. The record has its own Preferences handle, because
  `remove_peer` reaches the send path from the REST handler's task (F96).
  Counting stays per member (one counter per sender is F72's option). A rekey
  still resets the counters to 1, below the stored reservation and under a new
  opera key.
  Pinned by nine `test_mesh_liveness_wap` tests, each failing with its change
  reverted: `a_rebooted_member_is_heard_at_once`,
  `no_counter_is_signed_twice_across_reboots`,
  `the_reservation_costs_one_write_per_block`,
  `a_reservation_that_cannot_be_stored_refuses_the_frame`,
  `a_damaged_reservation_record_is_logged_and_replaced`,
  `a_boot_aligns_every_members_counter`,
  `the_first_boot_after_the_update_is_heard_at_once`,
  `a_fold_on_the_first_boot_keeps_the_floor` and
  `a_flood_spends_no_counter_while_the_storm_gate_holds`.
  `test_mesh_address_wap`'s re-pair test pinned that A drops a rebooted B's
  frames 1..3; it now pins that A hears B's first frame after the re-pair
  (counter 1025). Spec §3.3 and §12.3 say this, the first boot and the
  unreadable record included. Host-tested only; the Arduino compile is CI's;
  not bench-verified (U1). Found here: F99.
- [ ] **F72 [decision] canary-wap counts per destination, but its envelope
  names no destination.** A receiver judges a frame its member sent to another
  member against its own last-seen for that member. Such a frame is fresh
  whenever the member's counter for the other member ran ahead. That happens,
  for example, while the receiver sits at `PEER_UNKNOWN` in the member's
  table, which the alerts and the Beacon, channel-lock and hub-election sends
  skip; a fresh pairing leaves the partner there (host-probed with the #1761
  harness). Since F76 (#<W9>) heartbeats go to every member whatever its
  state, so they no longer skip it. Since F71 (#<W9>) a boot resumes every
  member one past the highest send-counter reservation stored for any of them,
  so a reboot levels the per-destination counters and the gap restarts from
  nothing. Between boots it grows with the traffic one member gets and another
  does not, as before. Since #1761 only a radio copying the member's own
  address can deliver such a frame (ESP-NOW does not authenticate a source).
  It is dispatched and pushes the receiver's last-seen ahead, which silences
  the member until its counter catches up (host-probed). A host probe (#<W9>):
  one frame replayed across members cost the member one genuine frame after a
  reboot and two with no reboot, where resuming each member from its own
  reservation (F71's first cut) cost up to a block (1025). Options:
  - put the destination fingerprint in the signed bytes (a wire change under
    spec §4.5's registry, with F48);
  - use one outbound counter per sender, as on the PIO tree. That makes any
    unheard frame fresh at every member instead.
  See THREAT_MODEL "Still open on canary-wap". Found by the canary-wap half
  of the F49 part 3 withdrawal (#1761).
- [x] **F73 [code] canary-wap's pairing reports success when `add_peer`
  refused the partner.** `handle_pair_confirm` (initiator) and
  `handle_pair_complete` (joiner) ignore `add_peer`'s result. In each of
  these cases the handler still persists the list, sets `MESH_ACTIVE` and
  fires the pairing callback with success:
  - a deny-listed key;
  - a full opera (16) for a new member;
  - since #1761, a re-pair onto an address another member holds, or one
    ESP-NOW cannot register (its list holds 20).
  It should return the failure to the owner instead. Found while fixing the
  re-pair (#1761), from code.
  *Done (#<W9>):* the partner is now added first. The initiator's
  `initiator_complete` calls `add_peer` before it seals the opera_secret, and
  the joiner's `handle_pair_complete` calls it before it installs the new
  opera. On a refusal, `fail_pairing`:
  - wipes the pairing, and sends and stores nothing;
  - returns the opera to CONNECTING or NO_OPERA, as `cancel_pairing` does;
  - logs a health WARNING;
  - fires the pairing callback with the role and `success = false`.
  A refusing joiner keeps its opera in RAM and in NVS, and wipes the decrypted
  secret. The other side cannot know, since there is no wire NACK: a refusing
  initiator sends no COMPLETE, so its joiner times out, and a refusing joiner
  has already been added by its initiator. No sketch code sets a pairing
  callback, so an owner sees the failure in `GET /api/mesh` and in the health
  log. Since F75 the initiator completes from `update()` on the loop task, so
  a refusal after a REST confirm shows on the next loop pass, not in the HTTP
  response. Pinned through the real handlers by five `test_mesh_liveness_wap`
  tests, each failing with the fix reverted: an initiator with a full opera
  (in both confirm orders), an initiator whose partner was removed
  mid-pairing, an initiator re-pairing onto another member's address, a joiner
  with a full opera, and a joiner whose initiator was removed mid-pairing.
  Correction to the item: its ESP-NOW-full case (a list of 20) cannot be
  reached through the pairing handlers, because the DISCOVER/OFFER step has
  already registered the partner's address (`esp_now_add_peer` in the
  DISCOVER and OFFER handlers). It stays covered at `add_peer`
  (`test_mesh_address_wap`). Spec §5.2 still lists that address among the
  refusals, with no note that a pairing cannot reach it. Host-tested only;
  the Arduino compile is CI's; not bench-verified (U1).
- [x] **F74 [code] canary-wap's mesh does not keep the ESP-NOW broadcast
  peer registered.** `send_pair_frame(BROADCAST_ADDR, MSG_PAIR_DISCOVER,
  ...)` relies on another module's registration. `csi_probe::init` adds it
  once; the WAP brings the CSI active probe up whenever csi_hal runs.
  `chirp_channel`'s and `beacon_channel`'s broadcasts add it before each
  send. `mesh_network`'s channel-change listener deletes it, and only chirp
  and beacon add it back. `esp_now_send` refuses an address that is not in
  the peer list (ESP_ERR_ESPNOW_NOT_FOUND). So after a channel change, with
  Chirp off (its default) and no Beacon broadcast since, neither a joiner's
  DISCOVER nor the CSI probe's own broadcast is sent. Whether the first
  boot's channel poll lands before or after `csi_probe::init` is a timing
  question, not established. In the #1761 host sim the first `poll_radio()`
  dropped the registration this way. Found from code and the sim; not
  bench-verified. Fix: register the broadcast peer where the DISCOVER is
  sent, and have the listener re-add it instead of only deleting it.
  *Done (#<W9>):* `ensure_broadcast_peer()` registers FF:FF:FF:FF:FF:FF with
  the settings Chirp, Beacon and the CSI probe use (channel 0, unencrypted),
  and takes `ESP_ERR_ESPNOW_EXIST` as registered. The pairing DISCOVER calls
  it. The channel-change listener now drops the peer and re-adds it, instead
  of only dropping it. Chirp, Beacon and the CSI probe add the peer only when
  it is missing, so their sends are unchanged. `mesh_net_sim.h`'s `boot()` no
  longer registers broadcast for every device: it stood in for those modules
  and hid this. Pinned by
  `a_discover_goes_out_with_no_broadcast_peer_registered` and
  `a_channel_change_re_adds_the_broadcast_peer` (`test_mesh_liveness_wap`);
  both fail on the old code. `docs/network_coexistence.md` said the listener
  re-registered the peer; now it does. Host-tested only; the Arduino compile
  is CI's; not bench-verified (U1).
- [x] **F75 [code] A canary-wap pairing completes only if the initiator's
  owner confirms the code first.** `handle_pair_confirm` acts on the
  joiner's CONFIRM only when this device's own `code_confirmed` is already
  set, ignores it otherwise, and neither side sends its CONFIRM twice. So if
  the joiner's owner confirms first, the initiator never sends COMPLETE, and
  both sides time out after 2 minutes. Spec §5.2 asks the owner to confirm
  on both devices, in no order. Host-probed with the #1761 harness: the same
  on 89a4c56 and after #1761. The PlatformIO tree's state machine also drops
  a peer's CONFIRM that arrives before its owner's (`either_handle_confirm`
  acts only in AWAITING_CONFIRM_PEER); whether it re-sends was not checked.
  Fix: keep an early CONFIRM and act on it at the owner's confirm, or
  re-send CONFIRM until COMPLETE arrives. Found by the canary-wap half of
  the F49 part 3 withdrawal (#1761).
  *Done (#<W9>):* the initiator keeps a verified CONFIRM from the pairing
  partner (`PairingSession::peer_confirmed`) and completes once its own owner
  confirms. The COMPLETE goes out from `update()` (`initiator_step`) on the
  loop task, not from `confirm_pairing()`, which the REST handler calls on the
  HTTP server's task. When the joiner confirmed first, the initiator sends
  COMPLETE alone: a CONFIRM just before it could take the joiner's one-frame
  receive buffer and get the COMPLETE dropped. A CONFIRM now counts only from
  the partner's address, and only once the code is shown: before the ACCEPT
  the session key is all zero, so anyone can compute that CONFIRM.
  `peer_confirmed` is also cleared wherever the partner or the keys change.
  Before this fix a CONFIRM was taken from any address, and once the owner had
  confirmed, a wrong hash from any radio ended the pairing. A wrong hash from
  the partner's own address still ends it, now in either order. Every #1761
  guard stays: the first OFFER only, COMPLETE only after the joiner's owner
  confirmed, one ACCEPT from the OFFER's address, and a wipe after COMPLETE.
  Pinned by five `test_mesh_liveness_wap` tests:
  - `the_joiners_owner_may_confirm_first`, failing with the fix reverted;
  - `a_confirm_from_another_address_does_not_count` and
    `a_bad_confirm_from_another_address_does_not_end_the_pairing`, failing
    with the fix reverted and with only the address check removed;
  - `a_confirm_before_the_code_is_shown_does_not_count`, failing with the
    state check removed;
  - `the_initiator_still_waits_for_the_joiners_confirm`, which pins the
    unchanged initiator-first order.
  Not closed: the address is not authenticated and the CONFIRM hash is
  role-free, so the initiator's own CONFIRM, reflected from the joiner's
  address, still counts. That gap was there before F75; it is F94, and spec
  §5.2 and the threat model say so. Spec §5.2 says either order. The item's
  open question is answered: the PIO state machine does not re-send its
  CONFIRM, so it has the same deadlock (host-probed; F97). Host-tested only;
  the Arduino compile is CI's; not bench-verified (U1).
- [x] **F76 [code] canary-wap's opera heartbeats only after it hears a
  member, and two cases leave it nothing to hear.** update() sends a
  heartbeat only in MESH_ACTIVE, and `broadcast_message` skips members
  below PEER_CONNECTED. The two cases:
  - after a fresh WAP-to-WAP pairing, each side holds the other at
    PEER_UNKNOWN. Host-probed with the #1761 harness: neither side sent
    the other a frame in 10 simulated minutes, on 89a4c56 and after #1761;
  - after a reboot of every member, init() leaves the opera at
    MESH_CONNECTING, and ACTIVE needs a member heard. Host-probed by the
    #1761 review: three members booted from NVS stayed CONNECTING and sent
    nothing for 2 minutes.
  A Beacon event, a channel lock or a hub election (sent to OFFLINE members
  too) would start it on a device. Not bench-verified. Found by the
  canary-wap half of the F49 part 3 withdrawal (#1761).
  *Done (#<W9>):* `update()` sends the heartbeat in MESH_CONNECTING as well as
  MESH_ACTIVE, and `send_heartbeat` sends it to every member whatever its
  state, charging the airtime governor one frame per member
  (`test_mesh_coexistence`'s pin follows). The cadence is unchanged: one frame
  per member per 30 s. The every-member half covers a third case the item
  missed: two members, each ACTIVE through others and holding each other at
  PEER_UNKNOWN (what a pairing between them leaves), never heard each other.
  The alerts and the Beacon, channel-lock and hub-election sends still reach
  PEER_CONNECTED and later only (F72 is narrowed to match). Pinned by five
  `test_mesh_liveness_wap` tests, each failing with the fix reverted, and each
  half of the fix alone fails its own cases:
  - `a_fresh_pairing_is_heard_both_ways`;
  - `an_opera_whose_members_all_rebooted_comes_back`;
  - `two_active_members_that_never_heard_each_other_do`;
  - `the_announce_keeps_the_heartbeat_cadence`: 20-21 frames per member in 10
    simulated minutes, and a configured opera with no members, in
    MESH_CONNECTING, sends nothing;
  - `an_opera_nobody_answers_keeps_the_heartbeat_cadence`: a device alone
    stays MESH_CONNECTING for 10 minutes and sends each member 20-21 frames;
    it fails on a 5 s announce.
  `test_mesh_address_wap`'s per-destination counter test now runs B's counter
  ahead with Beacon events, which still skip a PEER_UNKNOWN member. Spec §7.1
  states it. Host-tested only; the Arduino compile is CI's; not bench-verified
  (U1).
- [ ] **F94 [code+decision] A pairing CONFIRM can be reflected, on both
  trees.** The CONFIRM hash is SHA256(DOMAIN_PAIR_CONFIRM, session_key ||
  code) in both directions (canary-wap's `confirm_pairing` and
  `handle_pair_confirm`; the PIO tree's `compute_confirmation_hash`). So a
  radio that re-sends the initiator's own CONFIRM to it from the joiner's
  address, which ESP-NOW does not authenticate, passes it off as the joiner's.
  Host-probed on canary-wap, initiator-first order: the initiator sent
  COMPLETE, added the joiner and reported success, though the joiner's owner
  never confirmed, and the joiner then dropped the COMPLETE. The result was
  the same before F75, and F75's partner-address check does not stop it. The
  harm is an opera entry for a device that never joined, and a pairing
  reported to the initiator's owner as a success. Fix: bind the hash to the
  sender's role (a separate domain per role). That is a wire change,
  coordinated with the PIO tree (F48); spec §5.2 and THREAT_MODEL name the
  gap. Found by F75's review (#<W9>).
- [ ] **F95 [code] canary-wap never opens an AUTH session, so removing a
  member splits the opera.** Nothing sends `MSG_AUTH_CHALLENGE`: the handlers
  exist, but no code sends one. So `session_established` is never true, and
  `remove_peer`'s rotation sends `MSG_OPERA_REKEY` to no member. With no ACK
  pending, `maybe_finalize_rekey` commits the new secret at once, resets every
  member's counters, and marks them AUTHENTICATING or STALE. The survivors
  keep the old `opera_id`, and from then on each side drops the other's
  frames. Host-probed with the #1761 harness: after 10 simulated minutes of a
  working three-member opera, 0 of 6 entries held a session. A removed C, sent
  B no REKEY, and changed its own `opera_id` at once; three minutes later B
  held A as STALE and A held B at AUTHENTICATING. F48 covers how the AUTH key
  is derived; this item is that nothing starts the exchange. Until it is
  fixed, every survivor must re-pair after any removal on canary-wap. Related,
  from code and not probed: a rotation resets a member's last-seen to 0 in RAM
  only, and `replay_ctrs` keeps the old value until the next 5-minute save, so
  a receiver that reboots in between would drop the sender's restarted
  counters until they climb back. Found while fixing F71-F76 (#<W9>).
- [ ] **F96 [code] canary-wap's mesh REST handlers run on the HTTP server's
  task.** `handle_mesh_*` in `canary_wap.ino` call `remove_peer`,
  `leave_opera`, `start_pairing_*`, `cancel_pairing` and `confirm_pairing`
  straight from esp_http_server's task, while `update()` reads and writes the
  same peer table and pairing state, and the one `g_prefs` NVS object, on the
  loop task. `mesh_network.cpp`'s `g_rekey` note says a serializer moves
  `remove_peer` onto the main task, but no serializer exists: the `_auth`
  wrappers only check the bearer token. Found from code; not probed, since the
  host harness is single-threaded. #<W9> keeps its own new work off this path:
  F75's COMPLETE goes out from `update()`, and F71's reservation opens its own
  NVS handle (its refusal-log statics can still race, benignly). Fix: queue
  the mesh commands to the loop task. Found while fixing F71 and F75 (#<W9>).
- [ ] **F97 [code] The PIO pairing has F75's deadlock.**
  `either_handle_confirm` acts only in `AWAITING_CONFIRM_PEER`, so it drops a
  peer's CONFIRM that arrives before this device's owner confirms, and
  `confirm_code` sends its CONFIRM once. Host-probed with
  `test_mesh_pairing.cpp`'s helpers, the joiner's owner confirming first:
  - the initiator dropped the joiner's CONFIRM;
  - the initiator's own CONFIRM left the joiner waiting for a COMPLETE;
  - `tick()` re-sent nothing;
  - both sides returned NOTIFY_FAILED at the 5-minute timeout.
  canary-wap's fix (F75) carries over: keep the early CONFIRM, but only once
  the code is shown and only from the partner's address, act on it at the
  owner's confirm, and send the COMPLETE with no CONFIRM in front of it. Found
  by F75 (#<W9>).
- [ ] **F98 [code] canary-wap's `add_peer` takes a new member at an address
  another member holds.** Only the re-pair path (`rebind_peer`) refuses such
  an address; a new key at it is appended. The two entries then share one
  ESP-NOW registration, and `remove_peer` on either deletes it for both.
  Host-probed with the #1761 harness: a new key at C's address was added
  (three members). Removing it dropped C's registration, and A's next
  heartbeat to C was not sent. Fix: refuse it as the re-pair does (one
  address, one member), or keep the registration while any entry holds the
  address. F102 is the PIO tree's case of one address stored for two members.
  Found while fixing F71-F76 (#<W9>).
- [ ] **F99 [code] After a one-sided removal and a re-pair, canary-wap's
  re-paired member drops the remover's frames until the remover's counter
  climbs back.** `remove_peer` drops the entry on the remover only, and the
  removed device keeps its last-seen counter for the remover. After the 7-day
  grace, a re-pair re-binds the entry on the removed device's side, which
  keeps that counter. The remover adds the device as a new member, whose send
  counter starts at 1, since F71's record holds current members only.
  Host-probed with the #1761 harness: B had heard five of A's frames; after A
  removed B and they re-paired, B dropped A's frames 1..5 and heard 6. Fix
  options: keep a removed member's reservation in the record until its
  deny-list entry expires, or start a new member's counter one past the
  highest reservation, as a boot does since F71. Found by F71 (#<W9>).
- [ ] **F100 [code] A canary-wap pairing COMPLETE is sent once.** Neither side
  retransmits it, and the initiator does not check the send. So a COMPLETE
  lost on the air, or refused by the storm gate, leaves the initiator holding
  a member that never joined while the joiner times out. From code; not
  probed. Found while fixing F71-F76 (#<W9>).
- [ ] **F101 [code] PIO opera broadcasts reach a running pairing's partner and
  count it as delivered.** `mesh_transport::broadcast()` sends to every
  address in the transport table. While a pairing runs, that includes the
  partner's address (`ensure_pair_contact`), and an outsider gets its address
  there by answering the pairing. So the opera's tamper alerts, beacon events,
  channel locks, hub elections and a LEAVE are unicast to a non-member, and
  the return values count it. A scratch host probe ran with no members and an
  outsider answering the pairing: `send_tamper_alert` returned true and
  `leave_opera` reported `notified: true`, each with only the outsider's copy
  sent (the same on 7a0b0db). The frames are signed, not encrypted, so they
  disclose nothing a radio in range could not overhear; the harm is a false
  "sent" or `notified`. Fix: send opera broadcasts to the trusted peers' bound
  radio MACs only (or have the transport skip the pair contact), and count
  only those. Found while fixing F70 (#<W9>).
- [ ] **F102 [code] PIO: a pairing whose radio-MAC bind the session refused is
  still persisted, and the next boot binds it.** `dispatch_action`'s
  `NOTIFY_PAIRED` runs the PairedCallback (`main.cpp`'s
  `register_paired_peer`, which calls `save_peer_mac`) before
  `end_pair_contact` calls `bind_peer_mac`. `bind_peer_mac` refuses an address
  another member holds, but `peer_mac_blob::upsert` accepts it, and the boot
  restore binds `peer_macs` entries in blob order. A host probe in F70's
  review: member J, paired first at X, re-paired from member C's address. The
  session kept J at no new address, but NVS recorded J at C's address. After a
  reboot J took that address, C's bind was refused, and C's frames from its
  own address were dropped: C was not heard at all. Since F70 that is the
  outcome; before, C's frames were taken through J's binding. Reaching it
  takes a confirmed pairing that presents an already-trusted key from a copied
  address (F69's unauthenticated re-pair). Fix: persist the address only after
  the bind succeeds (`end_pair_contact` reports it, or the callback runs after
  the bind), and have `upsert` refuse an address another fingerprint holds.
  Found in F70's review (#<W9>).
- [x] **F50 [code] The display's other join hints still cut on narrow glass.**
  (#1755, #1727) Found by F45 (#1718). The Fail-stage hints from `join_failure_hint` measure
  175-219 px at 12 px ("your router may be out of addresses" is 219), so
  they are cut on the round watch's 142 px band and on the 156/164 px
  portrait rows. The PhoneJoined hint ("no page? open 192.168.4.1") is
  182 px under Heirloom on those rows. Carry them through `join_lines()` /
  `fit_line()` with narrow forms. Also, on the round watch's no-QR path,
  the title band appears to overlap the top of the bird, inferred from the
  numbers only. The emulator always renders the QR, so it was not seen.
  *Done, twice (#1755, then #1727), and merged into one:* two sessions
  fixed this in parallel. #1755 merged first; merging `main` into #1727
  kept #1727's coach line and #1755's bird seat. The coach line (#1727):
  the scenes without credentials (PhoneJoined, Fail) give
  their hint both rows the credentials leave. `onboardlayout::hint_lines()`
  tries the whole hint on the hint row, then over both rows, then a narrow
  form (`join_failure_hint_narrow()`, new in `wifi_join_policy.h`, one per
  failure; each still names the fix and keeps the hint's "may" and "try"),
  in the row's own face before the floor face. A two-row break goes after a
  clause ("no page?" over "open 192.168.4.1"), else where the halves are
  most even, and never inside "2.4 GHz". On every display env and ladder the
  fixes and the "no page?" hint now read whole, on one row or two, and
  `test_onboard_layout` requires it; the narrow forms are a pinned rung that
  no glass reaches today, and each fits the hint row alone. Round and
  portrait glass share one "no page?" hint (the round watch showed only the
  address). The test runs every Fail hint and the "no page?" hint on every
  display env with both ladders and LVGL's own metrics. The old one-row
  behavior fails 39 of its row checks: 37 are cuts the old tree showed, and
  2 are the round watch's new, longer hint. `onboard_probe.mjs` reads the
  whole "no page?" hint and the whole wrong-key and absent-network fixes off
  the emulator's glass word for word. With the old dist it fails on the
  watch (the address without "no page?") and on the nightstand ("passwords
  are case..."). #1755 had fitted the coach line to the hint row alone
  (`fit_line`: the full form, the narrow form, then the floor face) with
  its own narrow wording, and the merge replaced both. That wording
  dropped the NoAddress hint's "may" ("router out of addresses") and the
  catch-all's "try", which `test_wifi_join_policy`'s hedge check (#1727)
  would fail. The bird (#1755): on small glass the Join scene seats it at the
  hidden card's center (`join_bird_top`), which the stack keeps clear of
  the title and the credentials, and `canary_mark_rebase()` re-arms the
  base capture when the seat moves; the layout test guards the seat on
  every glass. The second observation holds at the layout constants but
  not on the glass. At the constants, the bird's head (from y 40) sits
  under the round watch's no-QR title band (30..48, or 30..52 under
  Heirloom), an overlap of 8 px (12). But canary_mark records the bird's
  base before LVGL's first layout pass, so the bird rides the panel center
  (y 98..137 on 240 px). That clears the band but puts the bird behind the
  other scenes' titles. #1755's rebase re-reads the base the same way, and
  after the Join scene that moves the bird off the glass: in the emulator,
  the round watch's PhoneJoined scene draws no bird at all on the merged
  tree, where 8f40cd8 (just before #1755) drew it behind the title (F64).
  The band's own title is cut under Heirloom ("On your phone", 154
  px on 142; F65). The onboarding docs said the hint comes after 9 s; they
  now say 4 s after the phone joins. Host-tested; the ESP32 builds are
  CI's; not bench-tested. The emulator dist is rebuilt from the merged
  tree, and `onboard_probe.mjs` passes on it for all five display flavors
  and fleet.html (a local Chromium run). Found here: F64, F65 and F66.
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
  1.60 % (one peer at 20 Hz, with an Opera of one, stays steady; the probe
  stops within one 792 µs frame of the line, so about 0.39 % stays for the
  heartbeat, presence and the Beacon self-test), and it brings the governor up
  on a boot without the mesh. Host-tested (`test_mesh_coexistence`,
  `test_csi_probe_airtime`, which pins the ceiling at exactly 1.60 % from
  both sides); the device build is CI's (firmware.yml's WAP Arduino legs).
  Latent on shipped devices: the WAP's probe table is empty, so it
  broadcasts at 10 Hz, ~0.8 % by the governor's estimate. Still open: the
  framing and the mesh's per-peer fan-out are F54's (#1725); the 1.60 %
  ceiling is also the Beacon's `airtime_saturated` trouble line
  (`beacon_channel.cpp`, > 160 x100), so the two move together or not at
  all; the 32-slot Beacon telemetry ring keeps the per-send shape (it gates
  nothing); with a filled probe table an over-budget probe arrives in bursts
  (host-measured about 1.3 s of frames per 10 s window at 8 peers), which is
  roadmap section 5 step 5's call (`aggregate_cap_hz`); and no bench has
  checked the 192 µs + 8 µs/B estimate against real air (U1).
- [x] **F54 [code] The airtime governor charges the mesh, chirp and beacon
  callers for their payload only.** Found reconciling F4 and F51. The
  probe's gate adds the ~59 B of ESP-NOW MAC and action-frame framing
  (`PROBE_FRAME_OVERHEAD_BYTES` in `probe_airtime.h`); every other
  caller passes header plus payload. The mesh also charges a heartbeat,
  tamper or power broadcast once, while `broadcast_message` unicasts it, its
  64 B signature included, to every connected peer (`mesh_network.cpp`; only
  offline-imminent multiplies by the peer count). So the window under-reads
  every caller but the probe.
  *Done (#1725):* the governor adds a ~59 B ESP-NOW framing allowance to
  every frame itself (`ESPNOW_FRAME_OVERHEAD_BYTES` in
  `airtime_governor.h`). The allowance is conservative: Espressif documents
  43 B of fixed fields for an unencrypted frame, and every sender here is
  unencrypted. `probe_airtime.h` no longer adds its own, so nothing is
  counted twice. `try_reserve_routine`, `force_reserve_urgent` and
  `force_reserve_beacon` take a defaulted frame count, charged as frames ×
  the one-frame estimate; 0 frames charges nothing, and a routine
  reservation of 0 frames is never denied. `mesh_network.cpp` charges one
  signed frame per peer: the 38 B header `send_to_peer` writes, the payload
  and the 64 B signature (`sizeof(MessageHeader)` is 48 and never sent). The
  heartbeat, tamper and power alerts go to each peer `broadcast_message`
  reaches (connected, stale, offline or alerting), and offline-imminent to
  each known peer. The chirp and Beacon broadcasts are one frame each and
  gain only the framing. The probe's 1.60 % ceiling is unchanged.
  Re-derived, a heartbeat to a full Opera of 16 (25 216 µs), a chirp
  presence and a Beacon self-test still fit in the 0.39 % it leaves, and one
  paired peer at 20 Hz (an Opera of one) still holds steady. With more mesh
  peers, a heartbeat now holds that probe off for up to about 1.6 s and
  keeps the window over the Beacon's 1.60 % line for up to about 1.5 s (16
  peers), once every 30 s. This is latent while the WAP probes no peer.
  Host-tested (`test_mesh_coexistence`, re-baselined to the framed numbers,
  with a fan-out case and a source pin on `mesh_network.cpp`;
  `test_csi_probe_airtime`, with full-Opera cases); compile-tested by CI's
  WAP Arduino leg. The figures are the governor's estimate, not a
  measurement of the air. Still open: every other mesh send reserves nothing
  (Beacon event, channel lock, hub election, rekey and its ACK, auth,
  pairing, leave-opera).
- [ ] **F63 [code] canary-sense and canary-sentinel answer no serial `j`, so
  the in-browser flasher's identity card cannot show them.** HA17 (#1727)
  gave both a boot line with the full key, not the `j` self-manifest
  canary-vision answers (`firmware/common/attest/self_manifest.h`:
  `device_id` / `pubkey` / `pubkey_fp`), which is what the identity card
  reads. Sentinel reads no serial input, and Sense's serial input belongs
  to the tuning console, which has no identity command, so `j` needs a new
  input path on both. Which port that input would arrive on is F67's
  question, so settle F67 first. Found doing HA17.
- [x] **F64 [code] The onboarding bird never sits where its host placed
  it.** (#1760, #1761) canary_mark_mood() records the bird's base with lv_obj_get_x/y at
  its first on-stage mood, and the breath and the poses then write that base
  back as the style offset. onboard_ui_create() reaches that mood (Hello,
  Idle) before LVGL's first layout pass, so the base reads 0. The bird then
  rides its alignment anchor with its offset lost. On the 240 px watch that
  is the panel center, y 98..137 (±2 as it breathes), instead of CENTER -64
  (y 36..75). Reproduced with LVGL 8.4.0 and the display's lv_conf, and seen
  in the emulator. On every small glass it sits behind "Hello." / "Let's get
  you connected.", "Nice - check your phone" / "a setup page is opening",
  "Joining" and "You're in.". Where a host's first mood comes after a layout
  pass instead, the base is the laid-out position. Under any alignment other
  than TOP_LEFT, that is not the style offset either (inferred from LVGL's
  semantics, not run). splash, dash_ui, nightstand7_ui, glance_ui,
  portrait_ui, portrait7_ui and nightlight_ui all align their birds (CENTER,
  LEFT_MID, TOP_MID, BOTTOM_MID, RIGHT_MID). Record the base as the host's
  style offset (lv_obj_get_style_x/y), then re-place the onboarding bird. At
  its constants the round watch's no-QR title band (30..48 px, 30..52 under
  Heirloom) overlaps the bird's head (from y 40), so that path needs a new
  position (F50's second observation, #1727). #1755 gave it one: the Join
  scene aligns the bird to the hidden card's seat (`join_bird_top`, y 94 on
  the round watch, which the layout test holds clear of the band) and calls
  `canary_mark_rebase()`. But the rebase only re-arms the same
  lv_obj_get_x/y capture, which the next mood takes right after the align
  and before a layout pass. It reads the bird's last laid-out position, and
  pose_rest() and the breath write that back as the style offset under the
  new alignment, so every scene change moves the bird by where it last
  stood. Seen in the emulator: the round watch's PhoneJoined scene draws
  no bird at all on the tree that merged #1755 (by the arithmetic it is
  past the glass's right edge), where 8f40cd8, just before #1755, drew it
  at the panel center behind the title. The fix here covers both; check
  that the drawn bird sits where each scene places it. Found by F50
  (#1727).
  *Done (#1760):* `record_base()` reads the bird's style offset
  (`lv_obj_get_style_x/y`), the number the host's `lv_obj_align` wrote and
  the poses write back, instead of the laid-out position; the wing and eye
  reads that offset their own last pose do the same. The emulator exports
  `emu_mark_box()` (the shell's `markBox()`), and a shared probe check
  (`canary-local/tests/bird_perch.mjs`) fails a shown bird that is off the
  glass or over a line of text: `boot_probe` runs it on every flavor's
  face, `onboard_probe` on the PhoneJoined scene, which must show the bird.
  On the rebuilt dist both pass for all five flavors (local Chromium run),
  and the screenshots show the round watch's PhoneJoined bird above its
  title (none was drawn before), the watch face's bird inside the ring
  above the clock (it sat on the ring), and the portrait face's bird at
  its `TOP_MID V(22)` seat. Not seen on real glass (U1).
  *Also (#1761), built independently with host tests:* canary_mark records the
  bird's base as the host's style offset from its anchor
  (`lv_obj_get_style_x/y`, the same call on LVGL 8 and 9). That is the one
  coordinate every pose, breath and hop writes back, so the bird is drawn
  where its host aligned it, under any anchor, whether its first mood comes
  before LVGL's first layout pass or after. `canary_mark_rebase()` needed no
  change of its own: it re-arms the same capture, which now reads the offset
  the align just wrote. A native harness (LVGL 8.4.0 with the display's
  lv_conf, the real faces linked, the bird's drawn box read after every
  refresh) measured every face's bird off its seat before the fix:
  - the onboarding bird sat at the panel's center (y 98..102 instead of 36
    on the round watch, 62..66 px low on every small glass), then walked
    another anchor-distance off the glass at each re-seat (PhoneJoined at
    x 300, y 194; Success at x 700);
  - the first-meeting splash bird sat at x 176 on the 240 px disc (x 704 on
    the dash), and the hello-again splash 29..44 px low (53..68 on the 7");
  - the glance bird sat at the disc's top edge, or at x 200 when it came on
    stage after a layout pass;
  - the dash bird sat at x 0, the 7" portrait column's at y 0 over the
    clock instead of 360, and the nightstand7's 18 px left;
  - the portrait faces' birds sat 17..43 px high, or 32..124 px right;
  - the nightlight's sat 20..24 px low, or off the 320 px glass; in its
    landscape composition (RIGHT_MID) 13..17 px low, or at x 453, past the
    glass, after a layout pass.

  Now each bird draws at its anchor plus its offset (dx 0, dy within the
  2 px breath or the hop), and the round watch's no-QR Join bird draws at
  `join_bird_top` (y 94). One seat that was never right now shows: the
  landscape nightlight's first-meeting splash bird sits at its coded CENTER
  -70, y -12 on the 180 px canvas, so the top edge cuts its head. Before,
  it sat whole but at x 256 (F88). `test_canary_mark_seat` compiles the
  real canary_mark.cpp against a model of LVGL 8's position rules
  (`tests_host/fake_lvgl/`) that reproduces those numbers. It holds the
  drawn box to the placement before and after a layout pass, under every
  anchor and across the onboarding's re-seats; the old file fails 29 of
  its checks. `test_onboard_scenes` holds the onboarding bird at the seat
  `onboard_layout.h` names, in every scene, on every small-glass env with
  both ladders; against the old canary_mark.cpp it fails 72 checks in its
  round build and 362 in its rectangular one. Host-tested, and measured
  natively (the harness is not in CI); the ESP32 builds are CI's; not
  bench-tested. #1761 keeps #1760's `record_base()` and its wing and eye
  reads on merging main; the emulator dist is rebuilt in this PR by CI's
  pinned emsdk. Found here: F88, F89.
- [x] **F65 [code] The onboarding's scene titles and bodies are cut on small
  glass.** Only the Join scene's credentials rows and the coach line are
  fitted (F45, F50). The titles and bodies keep LV_LABEL_LONG_DOT at a fixed
  width: the round watch's title band stays 142 px after the Join scene (138
  px in Hello), and on portrait glass every row is the panel less 16 px.
  Measured with test_onboard_layout's LVGL metrics, these are every cut on a
  display env. At the default ladder (titles 16 px, bodies 12 px): "Nice -
  check your phone" (197) and "No address from the router" (221) on the
  round watch and the 156/164 px nightstand and nightlight, and "Network not
  found" (154) on the round watch. Under Heirloom (titles 20 px, bodies 14
  px), on the round watch: the no-QR Join title "On your phone" (154, shown
  as "On your..." whenever the QR does not render), "Nice - check your
  phone" (246), "Network not found" (194), "Wrong password" (175), "No
  address from the router" (277), "Couldn't connect" (176) and "That didn't
  work" (167). Under Heirloom on the 156/164 px portrait rows: the same six
  later titles, and the bodies "Let's get you connected." (179), "a setup
  page is opening" (176), "try again on your phone" (176) and "looking for
  your canaries" (180). Under Heirloom on the 224 px touch169: "Nice - check
  your phone" and "No address from the router". The amoled241's Heirloom
  faces (28/20 px) are not in montserrat_metrics.h, so it was not measured;
  scaled from the 20 px widths, nothing reaches its 434 px. The Connecting
  body is the network name clipped to 28 characters, so a long name can be
  cut on any small glass. rf_fit_center() re-fits the centered labels on
  round glass. fit_line()'s ladder (shorter forms, then the floor face) with
  shorter copy covers the rest, and test_onboard_layout can hold each line
  the way it holds the coach line. Found by F50 (#1727).
  *Done (#1761):* on small glass, every scene's title and body is fitted
  at its own latitude through `fit_line()`'s ladder: the Character's face,
  then a shorter form, then the default Character's face. The width is
  `onboardlayout::scene_line_w()`: the disc's chord on round glass; on
  rectangular glass the panel less its pads, and no wider than the halo's
  inner chord. The copy moved into `onboard_layout.h` (`scene_copy()`,
  `join_title()`), where the host tests measure it, and the glass copy
  audits now read that file. Two lines have a shorter form, made of fewer
  of their own words (a test holds this): "Nice - check your phone" becomes
  "Check your phone", and the Fail reason "No address from the router"
  becomes "No address" (`join_failure_label_narrow()`, new in
  `wifi_join_policy.h`, returns the label itself for the other reasons;
  provision.cpp hands it over with the Fail stage, and the fix under the
  title still names the router). "No address" shows on every small glass
  except the round watch at the default type; "Check your phone" shows on
  the same glass except also the AMOLED at the default type. Three reasons
  put them there:
  - on the 172/180x320 portrait glass, no face holds the whole line in
    156/164 px;
  - on the touch169 (both types) and the AMOLED, the old full-width rows
    (224 and 434 px) held the whole line, but the lines now sit inside the
    halo (F66), whose chord gives a title 160 px on the touch169 (154 under
    Heirloom) and 314 px on the AMOLED (308). That is a decision, F86;
  - on the round watch under Heirloom, the ladder reaches the shorter form
    in Heirloom's face before it tries the whole line in the default face,
    where it would fit (221 px on 222). That is a decision too, F87.

  Every other line reads whole, stepping down to the default face under
  Heirloom where it must. A long network name in the Connecting body is
  shown whole in either face. If it does not fit, `name_line()` keeps its
  head and its tail around "..." in the floor face: the 32-byte
  "Basement-Mesh-Extender-Office-5G" reads "Basement-M...-Office-5G" on the
  172 px nightstand. The tail stays because that is where a household's
  networks differ ("-5G", "-EXT") and where the 2.4 GHz band shows, the
  first thing a not-found failure asks about. The old `%.28s` clip counted
  bytes and could split a UTF-8 character. The dash fits the name the same
  way (a 32-byte name ran past its 800 px under Heirloom); its titles and
  bodies stay content-sized and unmeasured (F84). `test_onboard_layout`
  runs every line of every scene (each Fail reason, both Join titles, a
  typical name and the widest) on every small-glass env (the round watch,
  the portrait glass, the touch169, the AMOLED) with both ladders, and pins
  where the two shorter forms show. Without the shorter forms it fails 33
  checks, 18 of them a line that would be cut. `test_onboard_scenes`
  compiles the real onboard_ui.cpp against `tests_host/fake_lvgl/` and
  drives every scene the way provision.cpp does, on the same envs: every
  label's text must fit the label in the font it carries and say one of
  the scene's forms. A scene line set without `fit_line()` fails it 160
  times on the rectangular glass and 6 on the round watch; dropping the
  Fail reason's narrow form, 20 times; removing the shorter forms, 52. In
  the native LVGL 8.4 harness, across the watch, nightstand, nightlight,
  touch169 and AMOLED scenes, the old tree drew 7 cut lines at the default
  ladder and 22 under Heirloom; the new tree draws none.
  `onboard_probe.mjs` now also fails on an ellipsis in each flavor's
  PhoneJoined scene and in its wrong-key and absent-network Fail scenes. It
  reads the committed dist, which is rebuilt in this PR by CI's pinned
  emsdk; it was not run locally. Host-tested; the ESP32 builds are CI's;
  not bench-tested.
- [x] **F66 [code] The onboarding halo ring runs through the low text rows
  on rectangular small glass.** onboard_ui.cpp draws one 236 px ring,
  centered, on every small glass. On the 172/180x320 portrait glass, its
  bottom arc (y 275..278 at the center) runs through the hint row
  (269..284). The upper row of a split coach line crosses it at both ends
  ("your router may be", y 259..268). On the 240x280 touch169 the arc (y
  255..258 at the center) runs through the hint row (239..254), and under
  Heirloom through both rows of a split fix. The Join scene's split
  credentials have crossed it the same way since F45. The text is drawn over
  the arc, not cut. The round watch's rows sit inside its ring. Size the
  ring from the panel's short side on rectangular glass, or keep it clear of
  the Join stack's rows, and have test_onboard_layout hold the rows inside
  the ring. Found by F50 (#1727), from the layout constants and seen in
  native LVGL 8.4 renders of onboard_ui.cpp.
  *Done (#1761):* on rectangular small glass the halo now fills the band
  between the Join scene's title row and its credentials row, kMinGap
  clear of each (`onboardlayout::small_join()`, `halo_ring()`), concentric
  with the QR card the way the dash's halo is with its card. Its sizes at
  the default ladder (Heirloom in brackets):
  - 196 px (192) from y 56 on the 172/180x320 portrait glass, wider than
    the panel, so it runs off the sides, as the 236 px ring always did;
  - 176 px (172) from y 46 on the 240x280 touch169;
  - 328 px (322) on the AMOLED, whose centered titles had crossed the
    236 px ring too.

  No row of the stack can reach the halo: not the credentials, hint and
  note rows, nor the coach lines that take them in PhoneJoined and Fail.
  Every centered line is fitted inside it (F65's `scene_line_w()`). That is
  why the touch169 and the AMOLED now show the two shorter forms where
  their old full-width rows held the whole lines (F86). On the touch169,
  the 128 px QR card's 10 px corners reached 86.4 px from the shared
  center, in the 176 px ring's stroke (85..88) and past it under Heirloom.
  `small_join()` trims that canvas two px at a time until the card's
  rounded corners are kMinGap inside the ring, the trade `join_stack()`
  already makes for air. The canvas ends at 106 px (104 under Heirloom),
  still 3 px per module, so the join code keeps its pitch and LVGL fills
  the canvas with a lower QR version. The rows and the ring do not move,
  and no other glass is trimmed. In native LVGL 8.4 renders the card's
  white inside the stroke band went from 20 px (128 under Heirloom) to 0.
  No phone has scanned the smaller card yet. The round watch keeps its
  236 px rim ring, with every row inside it. A full-width row there is
  fitted to the disc's chord at the ring's inner edge, so its box corners
  come within a pixel of the stroke; rectangular glass keeps 2 px. The
  scenes' bird seat (`scene_bird_top()`) stays inside the halo: CENTER -64
  as before, except 1 px lower on the touch169 under Heirloom.
  `test_onboard_layout` checks, on every small-glass env with both ladders,
  that the halo's stroke stays clear of every row of every scene, the Join
  title row, the QR card's rounded corners and the bird's two seats (with
  the breath). With the old ring it fails 40 checks, 26 of them a row or a
  seat across the stroke; without the card trim it fails 4.
  `test_onboard_scenes` holds onboard_ui.cpp to this: the arc it draws is
  the one `small_join()` places, and the labels, the card and the bird's
  breath stay clear of the stroke. On the rectangular glass, sizing the
  ring at 236 fails it 640 times, aligning the bird CENTER 28 times, and
  leaving the card untrimmed 72 times. Not held: on the touch169 the
  Success hop's apex reaches the halo's top arc (F85), and the dash's
  300 px halo still runs through its wider titles, bodies and long network
  names (F84). Host-tested; the ESP32 builds are CI's; not bench-tested.
  The emulator dist is rebuilt in this PR by CI's pinned emsdk.
- [ ] **F84 [code] The dash's onboarding scene lines run through its 300 px
  halo.** On the 800x480 wide glass (the dash, dash7 and nightstand7
  envs), the scenes' titles and bodies are content-sized and centered on a
  300 px halo. In native LVGL 8.4 renders:
  - "No address from the router" in the 36 px title face spans x 150..650
    at y 204..243, where the ring's sides are near x 254 and 546;
  - "Let's get you connected." (x 252..547) and the body "looking for your
    canaries" (x 251..549) touch the ring's sides;
  - a long network name in the Connecting body (now its head and tail
    around "...", up to 770 px) runs through both sides;
  - the Hello and Success title and body boxes overlap each other by 4 px.

  F66 placed the small glass's halo. The wide branch keeps its 300 px ring
  and unfitted lines, and no host test measures them. Fit the wide lines
  inside the ring's chord, or size the ring from them, and have
  `test_onboard_layout`'s `check_wide_rows` and a wide build of
  `test_onboard_scenes` hold it. Found by F66 (#1761).
- [ ] **F85 [decision] On the touch169 the Success hop rises into the
  halo's top arc.** The onboarding bird's scene seat stays inside the halo
  (F66, `scene_bird_top`), but the one earned hop puts its head into the
  arc's stroke at the apex on the 240x280 touch169. The hop is 12 px at the
  default Character, at most 14 for a shipped one and 15 under
  canary_mark's clamp of the temperament, plus the overshoot path's brief
  swing past that. The seat is at y 56 and the halo starts at 46. In the
  native harness the bird's top reaches y 43 at the apex, with 30 of its
  pixels on the stroke (18 under Heirloom). Either accept it (the hop
  crosses the halo for a moment) or seat the bird lower on that glass so
  the whole hop stays inside. Found by F66 (#1761).
- [ ] **F86 [decision] On the touch169 and the AMOLED: the halo's chord or
  the whole line.** F66 fits every centered onboarding line inside the
  halo's inner chord on rectangular small glass. That narrows a title from
  the 224 px row it had to 160 px (154 under Heirloom) on the 240x280
  touch169, and from 434 px to 314 (308) on the 450x600 AMOLED. So the
  touch169 now shows "Check your phone" and "No address" at both types,
  and the AMOLED shows "No address" (and "Check your phone" under
  Heirloom). The old rows held "Nice - check your phone" and "No address
  from the router" whole. It is the same truth in fewer words, but a
  450 px panel now drops "from the router" to keep a decorative ring's
  chord. Either keep it (the halo frames the scene's words), or on those
  two glass let the centered lines take the panel's row and size or drop
  the ring to clear them. `test_onboard_layout` pins where each shorter
  form shows, so any change updates it. See also F87. Found by F66
  (#1761).
- [ ] **F87 [decision] A title's ladder: the Character's face or the whole
  words.** `fit_line()` tries a line's forms in the Character's face first
  (the whole line, then its shorter form), and only then in the default
  Character's face; F45's rows and F50's coach lines use the same order.
  So under Heirloom the round watch shows "No address", though "No address
  from the router" fits its 222 px line in the default face (221 px), and
  the AMOLED shows "Check your phone", though "Nice - check your phone"
  fits its 308 px in the default face (295). For a title, the whole words
  may matter more than the face. Either keep one ladder for every line, or
  try a title's whole form in the floor face before its shorter form.
  Found by F65 (#1761).
- [ ] **F88 [code] The top edge cuts the landscape nightlight's
  first-meeting splash bird.** `splash.cpp` seats the first-meeting bird
  (64 px on small glass) at CENTER -70 (`INTRO_BIRD_Y`), a seat for a
  canvas far taller than the nightlight's 320x180 landscape one. There the
  bird sits at y -12, and in a native LVGL 8.4 run it drew from y -25 to
  -10 through its intro, with its head off the glass. Before F64 it sat
  whole, but at x 256 (the old base read), so the seat was never right on
  that canvas. Only a first meeting with a saved landscape rotation
  reaches it: no met-before flag while the rotation is kept in the
  nightlight's own NVS namespace, which takes a partial wipe. Clamp the
  first-meeting seat to the canvas, or scale it with the height. Found by
  F64's review (#1761).
- [ ] **F89 [code] No emulator probe reads where the bird is drawn.** F64's
  bird was seen in the emulator, but nothing there checks where it sits:
  `onboard_probe.mjs` reads the labels and the QR card, not the bird. A
  framebuffer read of the mark's canary yellow (0xFFD44F, which nothing
  else on a first boot paints) in the Hello and PhoneJoined scenes could
  hold the bird's box to the seat each flavor's layout names, the way
  `test_onboard_scenes` holds it on the host. Found by F64 (#1761).
  *Partly done (#1760):* the emulator exports the mark's drawn box
  (`emu_mark_box()`, the shell's `markBox()`), and
  `canary-local/tests/bird_perch.mjs` fails a shown bird that is off the glass
  or over a line of text: `boot_probe` runs it on every flavor's face, and
  `onboard_probe` on the PhoneJoined scene, which must show the bird. Left:
  the Hello scene, and holding the box to the seat each flavor's
  `onboard_layout.h` names rather than only on the glass and clear of text.
- [ ] **F67 [code] The C6 builds most likely send `Serial` to UART0 on the
  radar's pins, not to USB.** `firmware/envs/platformio/canary-sense.ini`
  :78 and `canary-sentinel.ini` :67 add `-UARDUINO_USB_CDC_ON_BOOT` (so do
  `canary-display.ini` :700, for the C6 nightstand, and `common.ini` :76,
  for `common_esp32c3`). PlatformIO's `ProcessFlags` appends every `-U`
  after the `-D` list, and its `CPPDEFINES` removal compares the bare name
  with a `(name, value)` tuple. So the flag cancels the board's
  `-DARDUINO_USB_CDC_ON_BOOT=1` (`seeed_xiao_esp32c6.json` in pioarduino
  55.03.38) instead of being skipped. Arduino-ESP32 3.3.8's
  `HardwareSerial.h` (:426-440) then defines the macro as 0 and maps
  `Serial` to `Serial0`, which is UART0, on RX GPIO17 / TX GPIO16 on the C6
  (:160, :180). `HWCDC.h` (:110-115) declares `HWCDCSerial`, the USB-C
  port, only when CDC-on-boot is 1. Both XIAO C6 `pins.h` files put the
  radar's UART1 on TX16/RX17 ('UART0 stays on USB-CDC console'), and
  Sense's `main.cpp` calls `Serial.begin()` (:677) before
  `RadarSerial.begin()` (:735). So the Sense boot log, its tuning console
  and the `Ed25519 pubkey` line HA17 added most likely go to header pins
  D6/D7, and UART0 and UART1 contend for the radar's pins.
  `scripts/lint_usb_console.py` and RELEASE_LESSONS (q) state the opposite
  rule for C3/C6 ('they provide `Serial` on their own, and the S3 flag
  prevents it'), so no gate notices. This is read from the installed core
  headers and PlatformIO's source, not seen on a bench. The C3 envs on core
  2.0.x were not checked. Fix, after a bench read on a XIAO C6 (U1): settle
  which flag the C6 (and C3) builds need, then change the envs, the release
  FQBNs and `lint_usb_console.py`'s rule together, and drop the port caveat
  from `docs/device_trust.md` and Step 6 of `docs/homeassistant_setup.md`.
  Found reviewing HA17 (#1727), which filed it as F62; renumbered when
  `main`'s F62 (the desktop clients' mDNS TLS advert, #1757) merged first.

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
- [x] **A12 [code] Desktop Flasher lacks the eFuse-read diagnostic** (#1702) the
  browser flasher has (espflash has no fuse-read; the parity test currently
  forces a "browser-only" disclosure). Needs an espflash upstream check or a
  raw-command implementation — investigate, then either implement or record
  why not beside the disclosure.
  *Done — implemented natively (#1702), after #1718 recorded the upstream
  check: espflash v3.3.0 (the pinned sidecar) and v4.3.0 (latest) were both
  downloaded and their command lists enumerated — neither has a fuse-read or
  read-reg command (`board-info` prints no security field; the library
  declares `GET_SECURITY_INFO` but never sends it). So
  `desktop/src-tauri/src/efuse.rs` speaks the read-only sliver of the ROM
  serial protocol itself (SLIP + SYNC + READ_REG, over the `serialport`
  crate the WE2 flasher already uses — the flash engine stays
  espflash-the-CLI): reset into the ROM (classic and USB-Serial/JTAG
  sequences, verbatim from the vendored esptool-js), read the six block-0
  words, hard-reset back. The decode is a port of `intake.js` — same fields,
  bits, widths, three-state clean/touched/active, same user-facing copy —
  and the desktop-parity test now pins the two tables against each other
  (mutation-tested: a one-bit drift fails), plus the native `EFUSE_BASE`
  pins against the vendored esptool-js bundle. A probe that can't reach the
  ROM reports "not checked", never "clean". NOT bench-verified on hardware
  (U1): the protocol and reset sequences are host-tested against the
  vendored implementation, not proven on a board.
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
- [x] **A25 [code] canary-local's WAP page shows an 8-digit example
  fingerprint.** (#1760, #1761) `canary-local/tools/gen_wap.py`'s TOPICS examples for
  `events`, `chain` and `counts` carry `"fp":"7f3a9c21"`. The envelope `fp`
  is 16 hex digits (8 bytes of `pubkey_fp`), and since HA20 a canary-wap
  sends it in lowercase. The examples elide the signature with an ellipsis,
  but the fp reads as a whole value. Give them a 16-digit lowercase example
  (regenerate `wap.json`, then `gen_csp.py` if the page hash moves). Found
  in HA20 (#1727).
  *Done (#1760):* the envelope examples carry `7f3a9c21b04e6d58`, and the
  boot log's `Public key fingerprint:` line carries the same 16 digits in
  `hex_to_str`'s capitals (it prints `g_device.fingerprint_hex`, which HA20
  left in capitals). `wap.json` regenerated; the CSP did not move.
  *Then (#1761), replacing #1760's example:* `gen_wap.py`'s `events`,
  `chain` and `counts` examples
  now carry `"fp":"7916ca487912fa1b"`, and its `[PROV]` boot line reads
  `Public key fingerprint: 7916CA487912FA1B`. Both are the repo's Ed25519
  test key (seed 0x42 × 32, public key `2152f8d1…81db12`). The WAP's
  `tests_host/test_mqtt_identity.cpp` (`kTestFp`) uses the same key, and so
  do Home Assistant's `tests/test_fingerprint_case.py` fixtures (`WAP_FP`,
  `WAP_EVENT`, `WAP_CHAIN`, `WAP_COUNTS`), so the page, the firmware test
  and HA show one fingerprint. The boot line was a fourth 8-digit example.
  It prints `g_device.fingerprint_hex`, which `hex_to_str` still spells in
  capitals (HA20 moved only the two MQTT strings), so it shows 16 capitals
  while the envelope `fp` is 16 lowercase. The generator now pins
  `mqtt_identity.h`'s lowercase alphabet, the encoder call, `hex_to_str`'s
  capitals and the `[PROV]` printf, and refuses to write when one of them
  moves. The health `public_key` example stays elided (`…`). The other Lab
  generators' examples already match their products: the Sense's `fp`
  (`witness.cpp`, 16 lowercase), the Operator page's `short_fp` (`cli.rs`,
  16 lowercase) and its trustee `--public-key 0123…` (elided;
  `hex::decode` takes either case). The Vision, Sense Lab, Vault and Home
  Assistant generators emit no such example. Two pages show one that this
  item's gate cannot see: the Vision page's hand-written MQTT pane
  (`assets/vision-ui.js`) shows a health `public_key` of `ed25519:…`, which
  the Vision firmware does not send (A26), and the Home Assistant page's
  WAP chain line has no `fp` at all, so HA's verifier would read it as
  unsigned (A27). A new `canary-local/tests/fingerprint_examples.test.js`
  runs in `canary-local.yml`'s logic tests. It walks every generated
  `canary-local/devices/*.json` (not the pages' hand-written scripts) and
  finds each fp, fingerprint, pubkey and public_key example: a property, a
  key in an example payload, a console line's value or a CLI flag. Each
  example must match exactly one rule, each rule's length and case are
  pinned to the source that prints it, and an example no rule covers
  fails. The test also derives the key from the seed with Node's Ed25519
  and holds the WAP page and `test_mqtt_identity.cpp` to it. On the old
  `wap.json`, 2 of its 5 tests fail (the four 8-digit examples); all 5
  pass on the new one. Every file it reads is inside the workflow's path
  filter. `wap.json` is regenerated, and `gen_csp.py --check` is unchanged
  because the captive page did not move. The emulator dist does not move.
  Page data only, host-tested; no firmware changed. The corrected examples
  reach users with the next Lab and Flasher release. Found here: A26-A29.
- [x] **A26 [code] The Vision Lab page's simulated MQTT pane does not match
  the firmware's publishes.** `canary-local/assets/vision-ui.js`
  hand-writes its health row as `{"fw":…,"public_key":"ed25519:…"}`.
  canary-vision's `publish_health_retained` (`src/net/mqtt_mgr.cpp`) sends
  `battery`, `battery_present`, `memory_free`, `uptime`,
  `firmware_version` and a `public_key` of 64 lowercase hex digits with no
  prefix. Its chain row, `{"length":1,"head":"…"}`, is not
  `publish_chain_retained`'s envelope either (`v`, `length`,
  `latest_hash`, `algorithm`, `alg`, `fp`, `sig`). Give the rows the
  firmware's keys with an elided bare `public_key`, or move the pane's
  payloads into `gen_vision.py`'s data, so that
  `fingerprint_examples.test.js` (which reads only the generated
  `devices/*.json`) holds them. Then decide whether that test should also
  read the pages' hand-written scripts. If it does, it needs a policy for
  template values like `sense-ui.js`'s `${data.device.fp_example}`. Found
  by A25's review (#1761).
  *Done (#<W9>):* the Vision page's MQTT pane no longer hand-writes a payload.
  `gen_vision.py` emits `mqtt.pane`: the rows the pane fills on connect
  (status, cfg/state, state, health, chain, aim/state) and an events template.
  Each is keyed exactly as the snprintf format that publishes it, nested keys
  and order included, or the generator refuses to write. The formats are
  `mqtt_mgr.cpp`'s `publish_status_retained`, `publish_detect_cfg_retained`,
  `publish_state_retained`, `publish_health_retained` and the signed branch of
  `publish_chain_retained`, `main.cpp`'s `publish_event_json` and
  `witness.cpp`'s `sign_event_envelope`. Health now carries a bare, elided
  `public_key` (`2152f8d1…`), chain the
  v/length/latest_hash/algorithm/alg/fp/sig envelope, cfg/state its
  `profile_label`, and aim/state a bare `OFF`. The fp and key are the repo
  test key's (seed 0x42 × 32); canary-vision derives its fp the WAP's way
  (`witness.cpp`'s `DOMAIN_FINGERPRINT`), so a Vision holding that key prints
  `7916ca487912fa1b`. `vision-ui.js` renders those rows and lays the sandbox's
  values over them. An event puts its reason right after the event name, as
  the firmware does, sets `occ_mask` to the box's own cell, and advances the
  chain row's length, eliding its moved head. A tuning slider republishes
  cfg/state whole, aim toggles a bare ON/OFF, and a reconnect republishes the
  retained rows as they stand.
  The decision on page scripts: `fingerprint_examples.test.js` now reads
  `canary-local/assets/*.js` and the Lab's HTML pages too, under a stricter
  policy than the JSON. A payload field a script writes (an object key, bare
  or in any quote, JSON inside a string literal included, or a property
  assignment) spells no fp, fingerprint, pubkey or public_key value of its
  own. Its value may only interpolate `${data.<path>}`, where the path, in the
  page's `devices/<page>.json`, is an example the JSON walk already holds to
  exactly one rule. `sense-ui.js`'s `${data.device.fp_example}` passes; a
  literal, a bare `…` or any other expression fails. Plain declarations are
  not read (flash.js's broker-certificate placeholder). One value is a named
  exemption: `guides.js`'s tamper drill publishes a forged chain head with fp
  `0000000000000000`, which the display's `evaluate_chain` does not read (the
  drill fails on the sig). The test pins that call and fails on a dead
  exemption.
  `vision.test.js` holds every pane row to the firmware's keys, unit-tests the
  overlay, and drives `buildMqtt` on a fake DOM through connect, events, a
  slider, aim and a reconnect. On the old `vision-ui.js` the script check
  fails on `"ed25519:…"`, written as a JSON string or as an object literal; on
  the old `vision.json` four Vision tests and two fingerprint tests fail. Lab
  page data and script only, host-tested. The page was browser-checked locally
  with `vision_probe.mjs`, which CI also runs. No firmware changed and the
  emulator dist does not move. The pane reaches users with the next Lab and
  Flasher release. Found here: A30 (the sandbox scenes' deltas), A31 (the
  Sense pane's hand-written live rows), A34 (the Sense page's fingerprint) and
  A37 (the Vision pane's fixed clocks).
- [x] **A27 [code] The Home Assistant Lab page's "real topic contract" is
  not the WAP's.** `gen_homeassistant.py`'s "5 · Meet the fleet" step
  shows a WAP chain line,
  `{"length":1284,"latest_hash":"9f2c…","sig":"ed25519:…"}`. It has no
  `v`, `alg` or `fp`, and its sig is not base64url.
  `custom_components/securacv/signature.py`'s `_verify_with_kind` reads it
  as unsigned ("Payload missing sig/fp/alg fields"), yet the page's note
  says it is the line the integration verifies against the pinned key. The
  step's `availability`, `status` and `health` lines also carry topics and
  keys no WAP publishes: `csi_mqtt.cpp` sends
  `{"online":true,"device_type":"canary-wap"}` on `status`, and its
  `health` carries `public_key`, HA's TOFU anchor. Generate these lines
  from `gen_wap.py`'s `TOPICS`, with the same test key and fp
  `7916ca487912fa1b`. Then extend `fingerprint_examples.test.js` to require
  an `fp` in every `events`, `chain` and `counts` example, since it checks
  only the fps that are present. Found by A25's review (#1761).
  *Done (#<W9>):* `gen_homeassistant.py` builds the "5 · Meet the fleet" step
  from `devices/wap.json`, so `gen_wap.py` runs first. It prints the WAP's
  retained `status`, `health`, `chain` and `counts` verbatim from
  `gen_wap.py`'s TOPICS, under the test key's device id
  (`securacv/canary-s3-4dC2/…`, fp `7916ca487912fa1b`). The step's
  `mosquitto_sub` subscribes to exactly those four topics: the WAP retains
  eleven, so the old bare `securacv/#` with `-C 4` printed whichever four the
  broker handed over first. The availability topic and the status and health
  keys no WAP publishes are gone. The note says only what the integration
  does. It pins `health`'s `public_key` on first sight (`__init__.py`'s
  `_async_health_for_tofu`); `chain` and `counts` carry the v/alg/fp/sig
  envelope it checks against that pin (`signature.py`'s `_extract_envelope`
  and `_verify_with_kind`); a line without the envelope reads as unsigned. The
  hash, the key and the sigs are elided, and the note says so. The generator
  pins those literals in both integration files and refuses to write when one
  moves; both files are now in `canary-local.yml`'s path filters. The demo
  below names the same device (`SecuraCV Canary canary-s3-4dC2`) and reads its
  Witness Count and Uptime from the counts and health lines, and the test
  holds those values, the device name and the drill's notification to the
  lines.
  `fingerprint_examples.test.js` requires v 1, alg `ed25519`, a string fp and
  a base64url sig (86 characters whole, per `SIG_B64URL_LEN`, or a shorter
  prefix elided with `…`) in every events, chain and counts example in the
  generated JSON, both topic-contract entries and `<prefix>/<id>/<topic> {…}`
  wire lines. It holds the fleet lines to `wap.json`'s payloads byte for byte,
  and its WAP fp and key rules cover the Hub page's lines. A sandbox scene's
  publishes are exempt for now (A30). Because the Hub page now reads
  `wap.json`, `canary-local.yml` checks "WAP data drift" before "Hub data
  drift", the WAP error names both generators, and
  `scripts/tests/test_lab_drift_order.py` requires every drift step to follow
  the pages its generator reads. On the old `homeassistant.json` two tests
  fail: its chain line has no v, alg or fp, and its command is the bare
  `securacv/#`. Page data and CI step order only, host-tested. No integration
  code changed, so there is nothing to carry to the HACS mirror. Found here:
  A30, A32 (the WAP events example), A33 (the setup guide's topic table) and
  A36 (the demo's entities).
- [x] **A28 [code] The Sense and Vision Lab pages show their Hardware ID as
  hex.** `gen_sense.py`'s `EX_HWID` (`9f41c2d8a06be375`) and
  `gen_vision.py`'s `EX_HEX` (`b3f2a9c41d5e`, 12 characters) are hex. But
  `device_pseudonym::device_id_hex`
  (`firmware/common/identity/device_pseudonym.h`) renders 16 characters of
  the 54-character unambiguous alphabet, which has no `0` or `1`, so
  neither is a value a unit can print. The Sense page's `EX_HOST`
  (`canary-sense-001-b7e2c4`) borrows the fingerprint's first six digits;
  `mdns_mgr.cpp`'s `make_hostname` appends the pseudonym's first six
  characters instead, without lowercasing them. Give each example the
  pseudonym's alphabet and length, and derive the host from it. Then
  extend `fingerprint_examples.test.js`'s approach (find every example,
  hold it to a rule pinned to the source) to the pseudonym. Found by A25
  (#1761).
  *Done (#<W9>):* a new `canary-local/tools/_pseudonym.py` derives the
  pseudonym by `device_pseudonym.h`'s own construction: SHA-256 of
  `canary:device-id:v1:` and the salt, rendered as 16 characters of the
  54-character alphabet by rejection sampling. It also derives `make_hostname`
  (the id cut to 23 bytes, `_`, space and `.` hyphenated, then the pseudonym's
  first six characters, case kept) and the MQTT client id
  (`securacv-<id>-<pseudonym>`). It reads each recipe from the source and
  refuses when one moves. The examples use the two salts the shared header's
  host test uses (0x11 × 32 and 0x22 × 32). The Sense page's console now
  prints the MQTT scene `main.cpp` prints between the fleet advert and the
  first connect, `Hardware ID 88XFJv8Ztmy3dr4R` among its rows; the generator
  pins the whole block and its place. The Sense host is
  `canary-sense-001-88XFJv`, on its `[MDNS]` line too. The Vision console's
  Hardware ID line shows `fH3dtqtBGrpRDhAN`. Both pages' MQTT connect lines
  name the client id the firmware prints
  (`securacv-canary_sense_001-88XFJv8Ztmy3dr4R`), not the bare device id. The
  registry cards for both products said `canary-<fp>.local`; they now say
  `<device-id>-<pseudonym>.local`, like the other cards.
  `fingerprint_examples.test.js` derives the pseudonym again from the same
  salts with Node's SHA-256, pins the construction and each project's
  `make_hostname`, client-id and `Hardware ID` lines, and holds every hwid,
  every `Hardware ID` console line (one required on each page) and every
  `Connecting … as` line to the derivation. Every `.local` host a generated
  page names must be a fixed name, a template or the host its product derives;
  every registry card of the display, Sense, Vision and WAP families must name
  the template its firmware composes; and no template may build a host from
  the fp or the MAC. On the old data the pseudonym, host and template tests
  fail, and `sense.test.js` fails on the old Sense console, which had no
  Hardware ID line. Page data and one page script only, host-tested. The Sense
  page was browser-checked locally with `sense_probe.mjs`, which CI also runs.
  No firmware changed.
- [x] **A29 [code] The WAP's mDNS host examples do not have its fallback
  hostname's shape.** With no friendly name set, `generate_mdns_hostname`
  (`canary_wap.ino`) advertises `canary-%02x%02x` of `pubkey_fp[0..1]`:
  four lowercase hex digits, not the device id's unambiguous suffix. The
  Lab page (`gen_wap.py`'s `EX_MDNS`, `canary-ab7k.local`, beside device id
  `canary-s3-ab7k`) and the docs (`firmware/projects/canary-wap/README.md`'s
  `canary-ab7k.local`; `firmware/projects/canary-wap/arduino/canary_wap/README.md`
  and `docs/homeassistant_setup.md`'s `http://canary-s3-ab7k.local`) show,
  as the unnamed device's host, a name only a friendly name could produce:
  `k` is not hex, and the fallback never carries `-s3-`. The page's SSID
  (`SecuraCV-AB7K`) and device id (`canary-s3-ab7k`) also spell one suffix
  in two cases, which one device cannot, because both come from the same
  `unambiguous_suffix16`. Decide whether the page's identity examples
  should all come from the repo test key (device id `canary-s3-4dC2`, HA's
  `DEVICE_ID`; `SecuraCV-4dC2`; and `canary-7916.local` beside the
  `7916ca487912fa1b` the page now shows), then correct the docs to the
  fallback's real shape. Found by A25 (#1761).
  *Done (#<W9>):* decided: every WAP identity example on the page is the repo
  test key's. `gen_wap.py` derives them from `pubkey_fp[0..1]` of the fp the
  page already shows, by the firmware's recipe read from `canary_wap.ino` (the
  alphabet, the base-54 loop, the S3 `DEVICE_ID_PREFIX`, `generate_device_id`
  and `generate_ap_ssid` both encoding those two bytes, and the
  `canary-%02x%02x` fallback), and refuses when one moves. The names are
  device id `canary-s3-4dC2`, SSID `SecuraCV-4dC2` (one suffix, one case) and
  unnamed host `canary-7916.local`. The AP password stays illustrative: it is
  HMAC-derived from the private key, a credential rather than an identity, and
  no host test pins that derivation. The page's "Reach it" row says the host
  is the unnamed one (`canary-<name>.local` once named), and the registry card
  adds `(unnamed: canary-<4 hex>.local)`; the test holds both.
  `fingerprint_examples.test.js` derives the three names again from the seed,
  checks the arithmetic by hand (0x7916 = 30998 = 2 + 34·54 + 10·54², so
  `4dC2`), and holds every WAP device id, SSID and four-hex host in the
  generated JSON to them. On the old `wap.json` it fails on all six id and
  SSID examples and on `canary-ab7k.local`.
  The docs: `unambiguous_suffix16` encodes 16 bits, so a WAP suffix's fourth
  character is always `2`, and AB7K, AABB, CCDD and a3f7 are suffixes no
  Canary prints. Ten docs now show the test key's names and the unnamed host's
  real shape, `canary-` and the first two bytes of the key fingerprint in four
  lowercase hex digits (e.g. `canary-7916.local`): the WAP README, the sketch
  README, `homeassistant_setup.md`, `getting_started_canary.md`,
  `onboarding_multiple_canaries.md`, `onboarding_workflow_evaluation.md`,
  `manual_test_plan_captive_portal.md`, `v1_bench_validation_runbook.md`,
  `homeassistant_automations.yaml` and `tools/serial-monitor/README.md`. Seven
  of them derived the host from the MAC or the device id. Two said the host is
  "shown in the boot banner". It is not, but at the default log level the
  health log echoes `[INFO/NETWORK] mDNS started | canary-7916` to serial at
  boot, and the three docs that tell a reader where to find the host now point
  at that line. A display board README's `<device-id>.local` is now
  `<device-id>-<pseudonym>.local`. `scripts/tests/test_wap_name_examples.py`,
  in lint.yml's unfiltered run, builds the 65,536 suffixes the recipe can
  produce and holds every doc under `docs/` (except the dated audits),
  `tools/`, `firmware/` and `canary-local/` to them and to the hosts a WAP
  advertises; it found 16 problems in the docs as they stood. Page data and
  docs only, host-tested; no firmware changed. Found here: A35 (two firmware
  comments and a test fixture) and D10 (the docs call the suffix unique).
- [ ] **A30 [code] The WAP and Sense Lab pages' sandbox scenes publish
  abbreviated payloads.** `gen_wap.py`'s and `gen_sense.py`'s `SANDBOX`
  entries publish only the fields a scene changes. Events look like
  `{"event_type":"motion","state":"motion","motion":74}`, with no
  v/alg/fp/sig, and chain gets `{"length":+1}`, which is not JSON. `wap-ui.js`
  pushes them into a pane whose note says its payloads are "the exact strings
  csi_mqtt.cpp publishes", and the chain one replaces the retained chain row.
  `fingerprint_examples.test.js`'s envelope rule exempts `.sandbox[…]` for now
  (A27). Lay each scene's fields over the topic's full example, as the Vision
  pane's `vizEventPayload` does (A26), then drop the exemption. Found by A26
  and A27 (#<W9>).
- [ ] **A31 [code] The Sense Lab page's MQTT pane hand-writes its live rows.**
  `sense-ui.js`'s `labevent` handler writes an events row with no v, alg, sig
  or bucket_uptime_s, and a chain row
  `{"v":1,"length":…,"latest_hash":"…","alg":"ed25519","sig":"…"}` with no fp,
  which HA's `signature.py` reads as unsigned. A26's script policy passes the
  file, because its one fp is the interpolated, rule-held `fp_example`: the
  policy reads the values that are there, not the fields that are missing.
  Build both rows from `sense.json`'s topic payloads, as the Vision pane now
  does. Found by A26 (#<W9>).
- [ ] **A32 [code] The WAP Lab page's events example is a subset of the wire
  body.** `gen_wap.py`'s TOPICS `events` payload carries event_id, event_type,
  state, motion, breathing, signed and the envelope. `csi_event_wire.h` writes
  keys it lacks, among them module, category, privacy, timestamp, zone,
  confidence, duration_sec and replay. `gen_wap.py` holds each topic's suffix
  to `csi_mqtt.cpp` but not its payload keys. Key the WAP examples from the
  wire builder, the way `gen_vision.py` now keys the Vision pane (A26). Found
  by A27 (#<W9>).
- [ ] **A33 [decision] `docs/homeassistant_setup.md`'s MQTT Topic Reference
  reads as one product's contract, and it is not the WAP's.** It names
  `securacv/{device_id}/availability` (an online/offline LWT), which
  `firmware/canary/lib/securacv_mqtt` publishes and canary-wap does not; the
  WAP's LWT and online flag ride `status`. It describes status as "Device
  state, GPS, chain sequence". The Hub page lists that table as the topic
  contract (`gen_homeassistant.py` parses it into `topics`). Step 2 of the
  same guide says each Canary always advertises its unique hostname, but
  firmware/canary's `securacv_network.cpp` advertises the constant `canary`
  and relies on RFC 6762 renaming. Decide whether the table is per product or
  the common subset, and say which; A36 is the same question for the Hub
  page's demo. Found by A27 (#<W9>).
- [ ] **A34 [decision] The Sense Lab page's fingerprint is not the repo test
  key's.** The WAP page, the Hub page and the Vision pane show the seed-0x42
  key (fp `7916ca487912fa1b`). The Sense page shows `b7e2c49a11f03d5c`, which
  no derivation produces, though canary-sense derives its fp the same way.
  Decide whether every Lab key example is the test key's. If so, move
  `gen_sense.py`'s `EX_FP` (and an elided health key) to it and add the Sense
  page to `fingerprint_examples.test.js`'s test-key check. Found by A26
  (#<W9>).
- [ ] **A35 [code] Two firmware comments and a test fixture show WAP names no
  Canary prints.**
  `firmware/projects/canary-wap/arduino/canary_wap/csi_dashboard_html.h:3365`
  (a comment inside the dashboard's served script) and
  `firmware/common/encoding/cbor.h:332` show `canary-s3-AB7K`, and
  `canary-local/tests/flash.test.js:1330`'s redaction fixture logs `AP
  started: SecuraCV-7fA3`. A suffix's fourth character is always `2` (A29).
  The two firmware files were left alone because a comment edit there means a
  firmware rebuild, and `scripts/tests/test_wap_name_examples.py` reads docs
  only. Change them to `canary-s3-4dC2` / `SecuraCV-4dC2` when those files are
  next touched, and widen the lint to comments if wanted. Found by A29's
  review (#<W9>).
- [ ] **A36 [code] The Hub page's demo shows entities no WAP announces.** The
  demo now names the fleet step's WAP (`canary-s3-4dC2`), but it still shows
  Die Temperature, SD Card Healthy and Tamper Detected from the setup guide's
  firmware/canary entity set. `csi_mqtt.cpp`'s discovery table announces none
  of them, and the printed health line carries no die temperature or SD state.
  Give the demo a per-product entity set, or drop those three from the WAP
  demo (see A33). Found by A27's review (#<W9>).
- [ ] **A37 [code] The Vision pane's clocks and coarse features do not follow
  the sandbox.** `vizEventPayload` moves presence, voxel, box, occupancy mask
  and the chain length. `ts_ms`, `presence_ms`, `dwell_ms` and `visit_ms` keep
  the example's values, so a `dwell_started` publishes `dwell_ms` 0, and
  posture and proximity stay `upright`/`mid` for any box while someone is
  present. Derive them from the sandbox's clock and box, or have the pane say
  which fields are illustrative. Found by A26's review (#<W9>).

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
- [x] **HA14 [code] `/enroll` is named as every Canary's key source, but only
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
  *Done (#1725):* `docs/homeassistant_setup.md` Step 6,
  `docs/device_trust.md` and the options-flow text in `strings.json` /
  `translations/en.json` now say where each product's key can be read out of
  band, as read from source, in a new "Where each product shows its key"
  table. The table covers each product: canary-wap's unauthenticated
  `/enroll` page and `/api/device/enroll`, plus `/api/device-info`
  (fingerprint) and bearer-gated `/api/status`; USB serial `i` / `j` on the
  `firmware/canary` build; serial `j` on canary-vision. canary-sense and
  canary-sentinel show only a boot-log fingerprint (their setup portal shows
  no key), so owners compare it with `pinned_fingerprint` on the Health
  sensor, ignoring case. A canary-display has no key. The integration
  comments that said HA fetches `/api/device/enroll` to TOFU-pin were wrong;
  they are fixed, and so is the same comment in `canary_wap.ino`. The pin
  form's error no longer demands lowercase. `tests/test_key_source_copy.py`
  checks each clause: `/enroll` must name canary-wap and nothing wider. It
  also holds the product list to `firmware/flavors.json`. Read from source,
  not checked on a bench. Left: a full-key source on Sense and Sentinel
  (HA17); canary-wap's uppercase `fp` (HA18); the mirror carry. That carry
  includes the store-page sentence, which still tells every hostile-broker
  owner to pin by hand until it lands.
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
- [x] **HA17 [code] canary-sense and canary-sentinel show no full public key
  out of band.** Manual pinning needs the 64-hex key, but both devices print
  only `Ed25519 ready  fp=<fingerprint>`, once at boot (in the init of
  `firmware/projects/canary-{sense,sentinel}/src/witness.cpp`). Their only
  web page is the shared setup portal
  (`firmware/common/network/setup_portal.cpp`: `/`, `/scan`, `/join`,
  `/status`), on their own setup network at first boot and when a join keeps
  failing, and it shows no key. Sense's serial input feeds only the tuning
  console, which has no identity command, and Sentinel reads no USB serial
  at all. Owners can use the fingerprint to check a TOFU pin but cannot set
  one. Two candidate surfaces: the `j` self-manifest canary-vision already
  answers (`firmware/common/attest/self_manifest.h`: `device_id` / `pubkey`
  / `pubkey_fp`), or a key card on the setup portal. The portal is shared
  with every product that compiles it, and it runs on an open-to-nearby
  setup network, so decide that one deliberately. Host-test the routing or
  the card. Then update together the Sense and Sentinel rows of "Where each
  product shows its key" in `docs/device_trust.md`, Step 6 of
  `docs/homeassistant_setup.md`, and the options-flow text in `strings.json`
  / `translations/en.json`;
  `tests/test_key_source_copy.py::test_pin_step_offers_no_source_for_a_fingerprint_only_product`
  must change with them. The in-browser flasher's identity card works for
  them once they answer `j`. Found in HA14.
  *Done (#1727):* canary-sense and canary-sentinel now print their full
  public key at boot, from the first firmware release after 2.4.15.
  `witness.cpp`'s init prints `Ed25519 pubkey <64 hex>` right after
  `Ed25519 ready  fp=<fingerprint>`, from `device_signature::pubkey_hex()`.
  That is lowercase, the same string the health publish carries as
  `public_key`, and 79 characters, so it does not wrap at 80 columns. The
  `Device ID` line later in the same boot log gives the pin form's other
  field. Neither surface the item suggested was used. Sentinel reads no
  serial input, and Sense's serial input belongs to the tuning console, so
  `j` would need a new input path on both. The setup portal is shared by
  every product (`firmware/common/network/setup_portal.cpp`) and is up only
  at first boot and while joining keeps failing, so it would widen scope
  and still not be there when an owner goes to pin. One correction to this
  item's own claims: the portal's setup network is not open to anyone
  nearby. It is a WPA2 network with a per-unit password, minted once and
  kept, and it takes one station at a time (`setup_portal.cpp`'s
  `AP_PASS_LEN` and `WiFi.softAP(..., /*max_conn=*/1)`). The line is carried
  into canary-sentinel's `witness.cpp` (`check_sentinel_net_sync.sh`).
  Updated together: the Sense and Sentinel rows of "Where each product
  shows its key", manual pinning, rotation and the pre-TOFU threat bullet
  in `docs/device_trust.md`; Step 6 of `docs/homeassistant_setup.md`; and
  the options flow in `strings.json` / `translations/en.json`. All of them
  name the release floor ("a firmware release after 2.4.15"; 2.4.15 and
  older print only the fingerprint) and say "serial console", not USB. In
  `tests/test_key_source_copy.py`, the fingerprint-only pin test became
  `test_pin_step_names_a_source_for_every_signing_product`, and three tests
  are new: `test_boot_line_the_copy_names_is_printed_by_the_firmware` holds
  the named line to both `witness.cpp` inits;
  `test_boot_line_copy_names_the_release_it_ships_after` holds the release
  floor; `test_pin_step_names_the_device_id_line_the_firmware_prints` holds
  the `Device ID` line to both `main.cpp` files. The firmware halves skip in
  the HACS mirror, and `python.yml`'s path filter lists the five files they
  read. Two related fixes: canary-wap's `/enroll` page now tells the reader
  to paste the full key, not the fingerprint, within its 2048-byte buffer,
  which two `static_assert`s check in the host build too. And
  `docs/flasher_profiles_fleet_book.md` names `/api/device/enroll?nonce=`
  instead of a `/enroll.json` route that does not exist. Compile-tested by
  CI and host-probed against stubs, not bench-tested. Left open: which port
  carries the line. The C6 builds undefine `ARDUINO_USB_CDC_ON_BOOT`, which
  with Arduino-ESP32 3.3.8 maps `Serial` to UART0 on the radar's pins
  rather than USB (F67). Also left: serial `j` on Sense and Sentinel, which
  the in-browser flasher's identity card needs (F63); a bench read of the
  line on a Sense (Sentinel has not run on hardware); the line on the Sense
  teaching page's staged boot log. The HACS mirror's resync of the changed
  integration files follows (U6).
- [x] **HA18 [code] Home Assistant read canary-wap's signed publishes as a
  key mismatch (fingerprint case).** `canary_wap.ino`'s `hex_to_str`
  writes capitals, so `g_device.fingerprint_hex` is uppercase. That string is
  the `fp` in every signed chain / event / counts envelope, through
  `device_signature::init` and `csi_mqtt`, and the health `public_key` is
  uppercase too. HA derives the pinned fingerprint in lowercase
  (`device_trust.fingerprint_from_pubkey_hex`), and
  `signature._verify_with_kind` compares `pinned.fingerprint_hex != fp`
  exactly. So after TOFU, every WAP publish should read `mismatch`
  ("Fingerprint changed without rotation"). A host probe reproduces it: an
  uppercase fp gives mismatch, and the same fp lowercased gives ok. Not
  observed on a bench. The `firmware/canary` build already lowercases its
  envelope `fp` (`csi_event_egress.cpp`). Fix it on the HA side by comparing
  case-insensitively, stored pins included, with a test that uses a
  WAP-shaped envelope. Also decide whether the WAP should emit lowercase:
  its TLS CN, the provisioning receipt and the flashers' fleet-book
  `expectedFp` all read the same string. When it lands, remove the
  canary-wap caveat HA14 added to `docs/homeassistant_setup.md` Step 6 and
  to `docs/device_trust.md` "How to verify" step 3, which both expect
  `verified: true` and matching fingerprints. Found in HA14.
  *Done (#1727):* Confirmed, then fixed in Home Assistant. A host probe
  compiled the WAP's own `hex_to_str`, `compute_fingerprint` and
  `generate_device_id` (lifted verbatim from `canary_wap.ino`) with
  `device_signature.cpp` and `csi_event_wire.h`. It built the WAP's health,
  chain, counts and events publishes for the repo's test key and fed them
  through the TOFU hook and the sensor handlers. After TOFU all three signed
  topics read `mismatch`, with one key-mismatch notification. The same
  bodies with `fp` lowercased read `ok`. The integration now lowercases hex
  before it compares or stores it (`device_trust.normalize_hex`): the
  envelope `fp` in `signature._verify_with_kind` and every verdict field;
  the mismatch-notice dedup key; the health `public_key` before TOFU; every
  key `TrustStore.async_pin` stores (the pin form already lowercased); pins
  the old hook stored in capitals, healed on load with their replay
  counters kept; and the Health sensor's `public_key` attribute.
  Diagnostics reads the store, so it was already lowercase. The `fp` only
  picks the key, and the signature is still checked against it: tests pin
  that a different key, or a tampered body, in capitals still reads
  `mismatch`. `tests/test_fingerprint_case.py` carries the WAP's publishes
  byte for byte and runs them in capitals and in lowercase, and checks that
  the TOFU log line shows the key in lowercase. 8 of its 11 tests fail on
  the old code. Its firmware cross-check holds the fixture to `hex_to_str`
  at every `csi_mqtt::init` call, and it skips in the HACS mirror. HA14's
  canary-wap caveats in `docs/homeassistant_setup.md` Step 6 and in
  `docs/device_trust.md` "How to verify" step 3 are removed. HA18 leaves
  the WAP firmware alone: deployed units send capitals, and HA accepts
  both; HA20 (#1727) makes the WAP send lowercase too. The sweep of other
  consumers found they already ignore case: the desktop Flasher's whoami
  check (`eq_ignore_ascii_case`), the kernel's fleet peers (it lowercases
  the health key before it pins or compares it, and never reads `fp`), the
  iOS app (it derives its own lowercase fp and lowercases before suffix
  matches), both flashers' certificate mint (it lowercases; now pinned in
  `desktop_parity.test.js`), the witness verifier (it compares bytes),
  `canary-local/tools` (no fp compares), and the firmware readers (the
  canary-display's key pin, `firmware/canary`'s mesh API, the WAP's own mesh
  and beacon endpoints), `firmware/provisioning/create_manifest.py` and
  `tools/verify_witness_log.py`, which parse hex to bytes or lowercase it.
  tvOS pins kernel keys, not device fingerprints. The canary-display's
  fleet model did not ignore case (HA19, done in the same PR). The review
  of the sweep also found, from source, that the WAP's BLE Device Info
  characteristic read its id through a pointer to a deleted task's stack.
  That is not a case bug (HA21, done in the same PR). Reproduced and fixed
  on a host, not seen on a bench. The HACS mirror's resync of the changed
  integration files follows (U6).
- [x] **HA19 [code] A canary-display filed a canary-wap's BLE beacons and
  chirps under a ghost row (fingerprint case).** `FleetModel::on_chain`
  (`firmware/projects/canary-display/include/canary/fleet/fleet_model.h`)
  stored the chain envelope's `fp` as sent, and a canary-wap sends it in
  capitals (`canary_wap.ino`'s `hex_to_str`). Every `fp4` the model is
  handed is lowercase (`beacon_parse.h`'s `beacon_fp4_from_mfg`,
  `chirp_scan.cpp`), and `fp_suffix_match` and the ghost retirement compare
  exactly. The result: a WAP's own fleet beacon or chirp never landed on
  its MQTT row and made an `SCV-xxxx` twin instead; a ghost made by an
  early chirp was never retired; and a tap on the WAP's row sent
  `fleet_link_request` a capital suffix that `find_target_addr` (`strncmp`
  against the lowercase `fp4`) could never match. A host probe with the
  display's own model and beacon parser reproduced the first two, and with
  the fp lowercased they matched; the third is read from `fleet_link.cpp`'s
  source. Found in HA18.
  *Done (#1727):* `on_chain` stores the fp in lowercase (`copy_hex_lower`)
  and takes the ghost suffix from the stored copy. A new case in
  `tests_host/test_fleet_beacon_model.cpp` fails on the old model and passes
  on the new one, and the whole display host suite passes. `setup.sh regen`
  restaged the sketch's `fleet_model.h`. `canary-local/emulator/build.sh`
  compiles the fleet model and the MQTT manager that feeds it, so the
  emulator dist is rebuilt in this PR by CI's pinned emsdk, in the same
  rebuild as the Quiet Hours wheels (roadmap row 17). Host-tested; the
  device builds are CI's; not seen on a bench.
- [x] **HA20 [code] canary-wap: send its MQTT fingerprint and health key in
  lowercase.** Every other build writes both in lowercase: `firmware/canary`'s
  `csi_event_egress.cpp` and `main.cpp`, and each `witness.cpp` and
  `mqtt_mgr.cpp` on Sense, Sentinel and Vision. The WAP alone writes
  capitals through `hex_to_str`, which caused HA18 and HA19. Home Assistant
  (HA18) and the canary-display's fleet model (HA19) now accept both and
  must keep doing so while capital-spelling units are deployed, so this is
  consistency, not a fix; HA19 had to land first, and it has (#1727). Scope
  it to `g_device.fingerprint_hex` and the `pubkey_hex` handed to
  `csi_mqtt::init` at both of its calls (at boot, and again after a QR hub
  provision), not to `hex_to_str` itself: that one encoder also spells the
  TLS certificate fingerprint, the receipts, `/api/status`, serial `i`, the
  BLE witness export and the BLE Opera Device Info `id` (HA21).
  `fingerprint_hex` also feeds the CN of a newly generated TLS certificate
  (stored certificates keep theirs), the receipt's `pubkey_fp`,
  `/api/device-info`, `/enroll` and `/api/device/enroll`'s
  `fingerprint_hex`, the BLE console metadata and the BLE DIS serial number
  (`bluetooth_channel::set_device_metadata`). `/api/mesh/peers`'
  `fingerprint` is spelled by its own `%02X` and stays in capitals unless
  this item widens its scope. No reader of any of these in the repo
  compares it exactly (HA18's sweep). The BLE name suffix (`SCV-XXXX`) is
  built separately at BLE bring-up and stays in capitals, which is what the
  iPhone's provisional name shows.
  `custom_components/securacv/tests/test_fingerprint_case.py`'s firmware
  cross-check requires both strings to come from `hex_to_str`, at every
  `csi_mqtt::init` call, and fails on purpose when they stop: point it at
  the new encoder and keep both spellings running. Host-test the two
  strings, then update `docs/device_trust.md`'s note on which WAP surfaces
  print capitals. Found in HA18 (#1727).
  *Done (#1727):* A new pure header beside the sketch, `mqtt_identity.h`,
  spells the two MQTT strings in lowercase (`hex_lower`). Its
  `fingerprint_hex` and `public_key_hex` take fixed-size arrays, so a wrong
  buffer or byte array does not compile. Both `csi_mqtt::init` calls take
  `mqtt_identity::public_key_hex`: the one at boot (`register_api_routes`)
  and the one after a QR hub provision (`qr_scan_task_fn`). The one
  `device_signature::init` call takes `mqtt_identity::fingerprint_hex` of
  `g_device.pubkey_fp`. The decision, from the code: `hex_to_str` and
  `g_device.fingerprint_hex` stay in capitals. device_signature's cached
  fingerprint is the only source of the envelope `fp` (the chain and counts
  bodies and the events Signer in `csi_mqtt.cpp`). It is also what `/enroll`
  and `/api/device/enroll` print, so that card now prints the fingerprint in
  lowercase, beside the key it already printed in lowercase
  (device_signature's own encoder). Lowercasing `g_device.fingerprint_hex`
  was wider than the item needed. The TLS CN, the receipt,
  `/api/device-info`, `/api/status`, serial `i`, the BLE console metadata
  and DIS serial, the BLE witness export, the BLE Opera Device Info `id` and
  `/api/mesh/peers` keep their capitals. A fresh grep of the readers found
  none that compares these strings exactly. Home Assistant runs them through
  `normalize_hex`. The canary-display's fleet model lowercases them (HA19),
  and its key pin compares bytes. The kernel's fleet peers lowercase the key
  and never read `fp`. The desktop whoami reads the card's `pubkey_hex` and
  compares fingerprints with `eq_ignore_ascii_case`. HA and the display
  still accept both spellings. A new `tests_host/test_mqtt_identity.cpp` (26
  checks, in `make`'s `run`) covers three things. First, the encoder, on the
  repo test key and on all 256 byte values, with the key and fingerprint
  derived by OpenSSL. Second, a signed events body built through the real
  `device_signature.cpp` and `csi_event_wire.h`, with OpenSSL's
  deterministic Ed25519 as the signer; it is byte-identical to the lowercase
  `WAP_EVENT` Home Assistant runs, signature included. Third, source pins on
  `canary_wap.ino` and `csi_mqtt.cpp`: the encoder feeds both init calls,
  every published fp comes from `device_signature::fingerprint_hex()`, and
  the health key comes only from `csi_mqtt::init`'s copy. Two pins fail
  against the old sketch. `test_fingerprint_case.py`'s firmware cross-check
  now points at `mqtt_identity.h`. It still runs both spellings, and it
  checks that the lowercase run is the newer WAP's bytes. It fails on the
  old sketch, and the old version of it fails on the new sketch, as
  designed. `python.yml`'s path filter gains the header. The hex-case note
  and "Checking a TOFU pin" in `docs/device_trust.md` now name the capital
  spelling as firmware 2.4.15 and older. So do
  `docs/homeassistant_setup.md`, the integration's comments and the
  `/enroll` comment in `device_signature.cpp`. The emulator dist does not
  move: `canary-local/emulator/build.sh` compiles only the WAP's
  `securacv_audio.cpp`. Host-tested. The sketch compile is CI's. Not seen on
  a bench. The HACS mirror's resync of the changed integration files follows
  (sweep U6). Found here: A25.
- [x] **HA21 [code] canary-wap: the BLE Device Info characteristic read its
  device id from a deleted task's stack.** `ble_bringup_task`
  (`canary_wap.ino`) fills a stack array, `ble_device_id_hex[20]`, with
  `hex_to_str` of `pubkey_fp` and hands it to `ble_manager::init`.
  `ble_manager.h` copied it into its static `g_deviceIdHex`, but then passed
  the caller's pointer, not the copy, to `ble_opera::init` and
  `ble_chirp::init`, and `ble_opera.h` keeps that pointer
  (`g_deviceIdHash = deviceIdHash;`). The task then deletes itself
  (`vTaskDelete(NULL)`). From then on the loop's `ble_manager::update()`
  calls `ble_opera::update()` every 5 s, and its
  `updateDeviceInfoCharacteristic()` formatted `"id":"%s"` from memory the
  task no longer owns, so what the GATT Device Info characteristic
  (`...6002`, `docs/ble_protocol.md`) showed after bring-up was undefined.
  `ble_chirp` keeps the pointer too but reads it only inside `init`. Not a
  case bug; the `id` is one of the capital-spelled surfaces HA20 lists.
  Found in HA18's review.
  *Done (#1727):* `ble_manager.h` passes `g_deviceIdHex`, the copy it
  already made and never used (a zero-initialized static filled by
  `strncpy` with size - 1, so always terminated), to both inits. Read from
  source only: not compiled here (the WAP compile is CI's), not run, not
  seen on a bench. The emulator dist does not move:
  `canary-local/emulator/build.sh` compiles only the WAP's
  `securacv_audio.cpp`, which includes none of the BLE headers.
- [x] **HA22 [code] A health `public_key` with whitespace in it passes the
  TOFU hook's hex check and fails inside the pin task.**
  `_async_health_for_tofu` (`custom_components/securacv/__init__.py`)
  accepts any 64-character `public_key` that `bytes.fromhex` decodes, and
  `bytes.fromhex` skips ASCII whitespace. So a 64-character string holding
  spaces passes, decodes to fewer than 32 bytes, and
  `fingerprint_from_pubkey_hex` raises `ValueError` inside the scheduled
  pin task. No pin is made, but Home Assistant logs an unhandled task
  exception. Fix: require every character to be a hex digit (or the
  decoded key to be 32 bytes) before the pin task is scheduled, with a
  test. Pre-existing: the hook did the same before HA18. Found in HA18's
  review (#1727). *Done (#1727):* the hook requires exactly 64 hex digits
  after lowercasing, before the pin task is scheduled.
  `test_fingerprint_case.py` gains four 64-character non-keys; the two
  whitespace forms fail on the old hook and pass after. Host-tested.
- [ ] **HA23 [decision] Home Assistant accepts an event whose id equals its
  mark.** `_replay_gate` (`sensor.py`) refuses only an id below the last
  verified one. An equal counter passes on purpose: a broker re-delivers
  the retained last event, and the chain and counts republish unchanged
  while idle. So when a boot's NVS writes all fail and the device reissues
  an id, HA accepts the second, different body under it (a review history
  run through the real gate). This was true before F46, which does not
  make it worse. Decide whether an equal id should pass only with an
  identical body. Found by F46's review (#1761).
- [ ] *(Mirror repo itself: no code work. It was byte-identical again as of
  securacv-homeassistant#17 (2026-09-24), which resynced the 33 carried
  files #1703, #1704 and #1718 had moved. The same PR brought the store
  page's watch-actions, key-pinning, broker-TLS and Apple Home sentences,
  and a `lint_readme.py` overclaim check that reads a hard-wrapped claim as
  one and refuses "encrypted by default". PR #1725's carried files
  followed in securacv-homeassistant#19 and PR #1727's in #20 (2026-10-01),
  byte-identical again (U6). PR #1761 adds one carried test (F46's
  `tests/test_replay_one_id_space.py`), which waits for the next resync.
  Its health
  items are U6 and U7 above, plus the three monorepo-fixture tests its CI
  deselects, which is by design. A few more tests skip themselves there
  because they read firmware sources the mirror does not carry.)*

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
- [x] **W21 [code] The website's copy of the fleet contract vectors is
  hand-carried.** (website #216) W20 copied
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
  first carry PR has run.
  *Done (website #216):* the carry, run from `main` (4507279), added the
  vectors to `tv/vendor/PROVENANCE.txt` with the sha256 the hand pin held
  (the file itself was byte-identical); the provenance test now requires
  that pin, and the hand pin and its test are gone. 705 site tests pass.

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
- [ ] **D10 [decision] The docs call a WAP's four-character suffix unique to
  the board.** `getting_started_canary.md` says "The four characters at the
  end are unique to your Canary", and `onboarding_multiple_canaries.md` says
  the last four characters "are unique to this board". The suffix encodes 16
  bits of the key fingerprint, and its fourth character is always `2` (A29),
  so only three vary. Two Canaries share a suffix with a 1-in-65,536 chance
  per pair; in a fleet of about 300 the odds that two share one are about
  even. Decide whether the copy should say the suffix tells your Canaries
  apart in practice, and that the full device id and fingerprint are what is
  unique. Found by A29's review (#<W9>).

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
- [x] **CI2 [code] canary-local.yml's path filter is hand-kept, and it
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
  *Done (#1725):* the logic-tests job's three test steps run with two read
  recorders armed (`scripts/path_filter_reads/`). A `--require` preload
  records the paths each node process opens to read, stats, lists, resolves
  or only checks for existence. A `sitecustomize` `sys.addaudithook` hook
  records the paths each python3 process opens and lists. CPython audits no
  stat or exists call, so a python3 existence check is not seen. Each half
  arms the other in the processes it starts, and a generator a test spawns
  is charged to that test. The step after them runs
  `scripts/check_path_filter_reads.py` on the same recordings, so no suite
  runs twice. It matches every read against `on.pull_request.paths` with
  GitHub's pattern rules. A miss fails naming the test, the file, the test's
  line (a static ES `import` has none) and the line to add to both lists
  (R6). It also fails on a `node --test <file>` or `unittest discover` suite
  that left no record of its own. Its first run found 29 reads outside the
  filter, all listed now: two content reads
  (`firmware/canary/include/canary/runtime_config.h` in desktop_parity,
  `tvos/witness-core/tests/fixtures/fleet_contract_vectors.json` in
  canary_local) and 27 existence checks. The allowlist is empty: a `.pyc`
  read is charged to its source, which keeps `scripts/bom_pricing.py`
  checked. Not recorded: shell reads (the Witness Wall step's `cmp`, git),
  children with a replaced environment or python3 -I/-E/-S, the drift-step
  generators (194 paths outside the filter, measured on CI2's branch, 187
  of them `gen_flash.py`'s, nearly all under `firmware/canary/**`) and
  every other filtered workflow (CI4).
- [x] **CI3 [code] One host-test list.** On 7446893, `firmware.yml`'s Mesh +
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
  *Done (#1725):* the 19 are named rules in their Makefiles with the inline
  steps' exact flags and per-rule `-Werror` (canary-wap 14, hooked in by
  prerequisite as `run-sketch-logic`; canary-display 5, as `run-io-cores`),
  and the 12 canary-wap and canary-display duplicate rules took `-Werror`
  too. Every suite an inline step used to grep keeps that marker check as
  well as its exit code (`run_marked`, in all four project Makefiles;
  `test_audio_cadence` keeps its own lowercase marker), and its output still
  reaches the log through `tee` as the suite writes it. Of the 33 inline
  steps (34 compiles), 32 are gone and the witness-page step keeps only
  `gen_witness_page_v1.py --check`. The tincan and companion Makefiles got
  `make -C` steps, and the display list runs in the fleet-link job's
  existing `make -C`. `scripts/tests/test_host_test_lists.py` reads each
  directory's expanded `make` plan, so the shared `run:` lists and the
  prerequisite hooks count the same way. A run counts only when its failure
  reaches make: no `-` prefix, `.IGNORE` or `-i`, and nothing like `|| true`
  after it. The test also fails on a C or C++ compile of a tests_host source
  in any workflow step or composite action (by path, glob, shell variable,
  or a relative name under a working-directory or `cd`), and on a tests_host
  Makefile that no step in a pull-request workflow runs whole. It is red on
  the tree before this change and green on it. Host-tested: all five
  Makefiles pass locally, and a failing check in each of the 19 moved suites
  turns `make` red.
- [ ] **CI4 [decision] The path-filter read gate hears only
  canary-local.yml's logic tests.** CI2 (#1725) records what that job's three
  test steps read. The job's drift-step generators run unarmed: measured
  on CI2's branch, they read 194 paths outside the filter, 187 of them
  `gen_flash.py`'s (all but two under `firmware/canary/**`; the two are
  `.github/workflows/firmware-release.yml` and
  `firmware/provisioning/platformio_secure.ini`), plus `gen_figures.mjs`'s
  6 existence probes and one `gen_operator.py` read. Arming them is a policy
  call: `firmware/canary/**` in the filter would run the workflow's wasm
  build-and-boot job (a 45-minute timeout) on every flagship firmware PR. No
  other filtered workflow is recorded either, for example
  `ios-selfheal.yml`'s XCTests, whose `#filePath` reads CI1 listed by hand
  (the recorders hear node and python3 only). Decide which drift steps to
  arm, at what cost, and how the other filtered workflows' readers are
  heard. Found doing CI2.

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
