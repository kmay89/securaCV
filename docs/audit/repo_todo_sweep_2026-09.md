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
  (`.github/workflows/homeassistant-mirror.yml` warns and files an issue). The
  trees drifted: after #1703, #1704 and #1718 the mirror sat 33 carried files
  behind (every refresh ran green and pushed nothing) until a hand resync in
  securacv-homeassistant#17 (2026-09-24). HA14 moved the carried
  `custom_components/securacv` files again in #1725 (and F55 one carried
  test), resynced in securacv-homeassistant#19, and HA18, HA17 and HA22 moved
  them again in #1727, resynced by hand in securacv-homeassistant#20
  (2026-10-01). #1761 added one carried test (F46's
  `tests/test_replay_one_id_space.py`), carried by hand in
  securacv-homeassistant#21 (2026-10-02). #1762 changes three carried tests in
  wave 11 (`tests/test_canary_health_trust.py` and
  `tests/test_wap_tamper_health.py` for F82 and F109,
  `tests/test_signature.py` for F130; no integration code); their carry
  follows its merge. Wave 12 of #1762 changes carried integration code and
  tests (HA24): `binary_sensor.py`, `const.py`, `health_metrics.py`,
  `sensor.py`, `strings.json`, `translations/en.json`,
  `tests/test_egress_health.py` (new; its four firmware-reading tests skip in
  the mirror), `tests/test_entity_translations.py`,
  `tests/test_mqtt_lifecycle.py` and `tests/test_wap_tamper_health.py` (one
  comment); `conftest.py` and `brand/` are unchanged, no mirror-owned file
  needs a change (`README.md` names no single health entity), and the wave's
  other changes (the Vision's HA recipes and dashboard, the Lab tooling) touch
  no carried file. A mirror-shaped run (the mirror's three deselects) gave 448
  passed and 20 skipped here, on this tree. Their carry follows its merge too.
  Wave 14 of #1762 changes no carried file (F203's discovery test lives in the
  monorepo's `scripts/tests`), and neither does wave 15 (HA16 changes the
  WAP firmware, the kernel bridge, the kernel wizard and docs; nothing under
  `custom_components/securacv` moves since `41d53091`). Until the secret is
  set, every `main` change to the carried set needs that again.
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
  *Found by F201 (#1762):* wiring `beacon_channel::init()` makes its
  `load_beacon_set()` and `load_audit_log()` open `beacon` read-only before
  anything writes it, on a flash-encrypted board's first boot after an NVS
  erase; route both through `csi_module_settings_nvs::begin_read_only()` in
  the same change. `ensure_x25519_keypair()`'s read cannot come first: every
  caller needs a beacon-set entry, which only `load_beacon_set()` fills from
  NVS.
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
- [x] **F129 [code] The WAP setup wizard's close-out link opens a host no WAP
  advertises.** `companion_pwa.h`'s `updateMdnsLinkFromDevice` fetches
  `/api/status` and links `http://<device_id, lowercased>.local/`
  (`canary-s3-4dc2.local`), but `generate_mdns_hostname` advertises
  `canary-<name>`, or `canary-<4 hex>` when unnamed (A29). `/api/status` is
  `handle_status_auth` too, so a fetch without the API token fails and the
  static `canary.local` link stays. `/api/device-info` and the rename response
  already carry `mdns_host`. Link the advertised host, then drop
  `scripts/tests/test_wap_name_examples.py`'s exemption for the comment that
  describes the current code. A firmware change: compile-tested by CI. Found
  by A35 (#1762).
  *Done (#1762):* `updateMdnsLinkFromDevice` reads the public
  `/api/device-info`, whose `mdns_host` is `g_device.mdns_hostname` (the label
  `MDNS.begin` registers), through a new pure `WizardLogic.closeOutHost` that
  takes one RFC 1123 label and nothing else; on an error, a blip or an
  unusable host the static `canary.local` link stays. The old link read
  `/api/status`. Without a session cookie that route answered 401 and the link
  stayed on `canary.local`, which only the Canary holding the catch-all claim
  answers, so on a network with several Canaries it can open another one.
  After the recovery-kit save (a BOOT tap, then Save), which issues a
  `cv_session` cookie that `api_auth_check` accepts before any token, "Run
  again" re-prepared the links from the device id, lowercased, a host no
  Canary advertises. The rename response was not needed: it is token-gated,
  and the wizard holds no API token. The wizard's own flow names no device, so
  its close-out link is the unnamed host, `canary-<4 hex>.local`
  (`canary-7916.local` for the test key); a device that already has a stored
  name links `canary-<name>.local`, since `generate_mdns_hostname` prefers it.
  `sanitizeMdnsHostname`, which only turned the device id into a host, is
  gone, and the comments name the advertised host and both old paths.
  `web_assets_gz.h` is regenerated (`gen_web_assets_gz.py --check` passes);
  `check_csi_sync.sh`, the microcopy lint and `check_route_security.py` pass.
  `wizard_logic.test.js` unit-tests `closeOutHost` and runs the shipped
  `updateMdnsLinkFromDevice`, with any page helper it calls, against a fake
  device that answers as `canary_wap.ino`'s routes do (a 401 on `/api/status`
  without a token or the session cookie, `handle_status`'s device id with the
  cookie, `handle_device_info`'s JSON on the public route), pinned to the .ino
  and `api_auth.h`; on the old page six of its tests fail, the cookie case on
  the device-id host. `scripts/tests/test_wap_name_examples.py`'s last source
  exemption is dropped, and the test fails on the old comment. A firmware
  change: host-tested, compile-tested by CI, not bench-tested.

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
  *Corrected (#1762):* this said about 16 years (183,960 a day) and about
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
  *Since F81 (#1763, #1762):* the canary commits one row per closed bundle
  (at most ten minutes long, every observation in `bundled`, the span in
  `duration_sec`) instead of one per refresh, as the canary-wap does. So once
  closed bundles reach the ring, the daily summary's active and quiet counts
  count bundles: a state held for three hours is about 18 rows, one per
  10-minute window, and core.presence opens at most six bundles in each of
  its ceiling's hours (F90; a sliding hour can hold more rows, F132). The
  summary would have to join consecutive same-state rows (or sum
  `duration_sec`) to count periods, and a row's span can take in a brief other
  state (a return within two minutes joins the open bundle). On the canary the
  inject latch and the Today sheet do not apply: it calls no
  `csi_event_inject` and serves no `/api/events/today`. On the canary-wap, a
  closed bundle put in the ring would set `g_ring_has_live` like any live row.
  *Since F78 (#1762):* on the canary-wap a closed bundle that cannot go out at
  once waits in the egress's 8-row RAM hold and merges into the backfill by
  id, so a short broker outage no longer loses it. It is still never on the
  card, so a reboot, or more than eight rows waiting, still does.
  *Since F93 (#1762):* the daily summary emits no row on either tree, before
  or after that change: nothing calls `meta_daily_summary_set_clock()`, so its
  `tick()` returns before the 23:55 check (F121). Its counts matter here only
  once F121 is fixed.
  *Since F121 (#1762, wave 14):* the daily summary emits one row per synced
  local date at 23:55. It still counts committed ring rows, so until closed
  bundles reach the ring its active and quiet counts read 0
  (`test_csi_daily_summary.cpp` and `test_wap_daily_summary.cpp` pin `a0 q0
  x0` after a day of presence changes, which this item would update), and its
  note reaches no published surface (F219).
- [x] **F78 [code] canary-wap's live publish overtakes its own backlog.** On
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
  *Done (#1762):* the canary-wap's egress runs `csi_event_backfill.h`'s
  Planner, as the canary's does, in a new sketch-local
  `csi_event_egress.{h,cpp}`. The commit hook (`csi_integration.cpp`) only
  copies the row into a 16-deep FreeRTOS queue: it never blocks (a full queue
  drops and counts), never publishes, and never touches the card or the
  watermark, so nothing runs under the commit lock on the NimBLE host task.
  `csi_mqtt::loop()`'s pump, on the loop task, is the one writer: each row's
  tamper bridge first, then the row, live only when nothing older is owed.
  Card rows wait on the card, and the backfill walks them in id order, two per
  pass, until caught up (no longer 64 per CONNECTED). Rows the card does not
  keep wait in an 8-row RAM hold (oldest dropped first, counted) and merge
  into the walk by id: closed bundles (never on this card, F77), rows whose
  append failed, and every row while no card is open. "Older" includes a card
  that is not open but may hold older rows: from boot until its log first
  opens (a slow mount), and after it closes with rows waiting (an SD error's
  remount), for at most 45 s (`kCardWaitMs`); a card that comes back later has
  its rows skipped. An ambient row is never held: one that cannot go out at
  once is dropped, counted. With no broker configured, or after the broker
  changes (host, port, user or topic prefix: `csi_mqtt::destination_epoch()`),
  the waiting rows are owed to nobody, as on the canary. The NVS delivery
  ceiling is written on a hand-over, never for a row that will wait in RAM
  behind older ones (a failed card append's write is held back in `WapPort`
  until the row goes); the walk's own hand-over still writes it up to
  `kStride` ids ahead (F47's trade). `MQTT_EVENT_CONNECTED` flags nothing now.
  `csi_event_log` keeps the reload and dismissals and gains the planner's card
  adapter (`poll`, `append_line` reporting the retention cut, `read_at`,
  torn-tail sealing); `iterate_since` and `append` are gone; a pulled card now
  resets the reconcile latch, so a reinserted card reconciles a failed
  rewrite. A dismissal line never cuts the log and is never replayed; one the
  log cannot take yet (no open log, or a log at its cap, including a cap an
  earlier dismissal in the same flush reached) stays queued in RAM, up to
  eight, until it can, and a reboot drops it. Also fixed on the way: the old
  live path wrote the delivery ceiling before every publish attempt, connected
  or not, so a reboot during an outage skipped the whole card backlog.
  `tests_host/test_wap_event_egress.cpp` runs the real egress, SD log and CSI
  library against a model of HA's replay gate (45 scenarios, 132 checks).
  Built on the extraction commit (13c862c: the old logic moved unchanged,
  except that it sends the tamper bridge when the events body does not build),
  its reconnect-window, commit-inside-the-walk and reboot-in-an-outage
  scenarios fail 9 checks. Built on the first fix (3fc1f93), 20 checks fail
  (the late-card, closed-card, broker-change, ambient, reinserted-card and
  delivery-ceiling scenarios), and 8 in the dismiss suite (the waiting
  dismissals). A 66-mutation sweep of the egress and the card adapter: 61 fail
  a host suite, one (the RAM flush moved before the walk, a one-pass delay)
  fails `check_wap_event_egress.py`, and four are equivalent (listed in the
  PR). `firmware/scripts/check_wap_event_egress.py` (49 self-test mutations,
  run by `check_csi_sync.sh`) holds what the host build cannot compile: the
  hook (only `on_committed`, after the privacy gate), one pump in
  `csi_mqtt::loop()` and one `begin()` in `csi_integration::init` sketch-wide,
  the boot order (floor and egress before the modules, F83), the esp_mqtt
  handler and the broker epoch. Behavior changes beyond the fix: a broker
  configured later, or a changed one, is not sent the backlog; with no card, a
  short outage delivers up to 8 rows (was: lost); rows committed in the first
  45 s after boot wait for a card that may still mount; an ambient row that
  cannot go out at once is dropped. Residual: RAM-held rows and waiting
  dismissals do not survive a reboot (for closed bundles that is F77's call);
  a loop-task stall past 16 commits now drops rows from the card too.
  Host-tested; the ESP32 compile is CI's; not bench-tested (U1: the five F78
  rows in `hardware_verification_checklist.md`). Found here: F103-F106.
  *Since F103's review (#1762):* a failed append is not proof the row is off
  the card. A short write that landed all but the newline was sealed by the
  next append, the walk sent the card's copy, and the hold sent its own (HA's
  replay gate passes an equal id, so its triggers fired twice). The
  canary-wap's egress now drops a held row at or below the watermark
  (`State::front_delivered()`), as the canary's does; two
  `test_wap_event_egress` scenarios fail on the code before.
- [ ] **F79 [code+decision] Below the backfill's bound, the SD event log is
  trusted input.** Both backfills (the canary's
  `csi_event_backfill::Planner`, and the canary-wap's, which runs the same
  Planner since F78 in #1762; its `iterate_since` is gone) refuse a
  card line at or above the allocator's next id (F46). They sign and send
  whatever a line below that bound says: its content, and its id, including
  an id a reboot skipped or one the allocator had passed by the time the
  walk read it. A forged line written ahead of real rows also moves the
  watermark past them, so the real rows behind it are never delivered
  (review probes on the real planner). Options: a per-line MAC under a
  device key, or replaying only rows the witness chain vouches for. Not
  tracked in the roadmap or the gaps ledger. Found by F46's review (#1761).
  *Since #1762:* both egresses drop a row waiting in their RAM hold at or
  below the watermark (F103's review), so a forged line also drops the held
  rows at or below its id, where they used to be sent and refused by HA's
  replay gate (all but an equal id).
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
  *Done (#1763):* `csi_bundler_admit()` now names a merge
  (`CSI_BUNDLER_MERGED`: rolled into a bundle still open after admit's own
  expiry, no new row) apart from an opening (`CSI_BUNDLER_BUFFERED`), and
  `csi_event_emit` refunds the ceiling slot on a merge only;
  `csi_bundler_has_open()` stays, as a diagnostic. On both trees (the
  canary-wap copies are synced). `firmware/tests_host/test_csi_bundler_ceiling.cpp`
  runs the probe on the real library with a test clock (`CSI_TEST_CLOCK`):
  the emit every 121 s commits 144 rows a day (714 before), no hour holds
  more than the ceiling, and refreshes of an open bundle still spend
  nothing. Not seen on a device (U1).
  *Also (#1762):* #1762 fixed the same item in parallel and merged onto
  #1763's mechanism (its own rename of `CSI_BUNDLER_BUFFERED` and removal of
  `has_open()` were dropped in the merge). It adds three tests to
  `test_csi_bundler_ceiling.cpp` (folded in from its own ceiling test when
  the two merged), on a day anchored where the module's counter is created:
  the 121 s probe commits exactly 144 rows, so each of the counter's hours
  still gets its six; a 60 s refresh beside one new state every 10 minutes,
  whose bundle closes on its 10-minute window inside a refresh's admit,
  commits exactly 144 (286 before, 143 of them the refreshed state); and
  thirty merges into one open bundle leave five of the six slots for new
  states.
  Correction to #1763's text above: "no hour holds more than the ceiling" is
  true of the ceiling's own hours, not of every 60 minutes. The counter keeps
  six 10-minute buckets (the current one and the five before it) and bounds
  the openings in them; a row commits when its bundle closes, two to ten
  minutes after it opened. So a sliding 60 minutes of committed rows can
  hold more than six: 7 in the 121 s probe (with or without the final
  flush's row), 8 in the window probe (two of them the final flush's rows,
  which close two bundles early; 6 without them), and 12, twice the
  ceiling, when six openings fall at the end of one bucket and six at the
  start of the bucket that pushes it out, fifty minutes later
  (`test_a_sliding_hour_can_hold_twice_the_ceiling`, which pins it). Each
  probe's day still holds 144. Not a regression: the 121 s and window probes
  committed 714 and 286 rows a day before F80. Filed as F132; the test's "+1"
  comments now name this cause, not the final flush.
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
  F46 and F82 are corrected). Bench rows: the F80/F81 section of
  `hardware_verification_checklist.md` (U1). Host-tested; the ESP32 compiles
  are CI's; not bench-verified. Found here: F90, which #1763 filed too, and
  F132 (in the review of the merge with #1763).
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
  *Done (#1763):* the canary calls `csi_bundler_tick()` once per main
  loop (`securacv_csi_modules_tick()`), as the canary-wap does, so a bundle
  closes for its window or its quiet gap only and a refresh merges. The
  tick sits outside the CSI power and heap gates: the feature callback
  stops while they skip `csi::process()`, and an open bundle must still
  close on time then (Codex review on #1763). Both trees now spend one ceiling
  slot per bundle, which F90 follows up. Built for the canary, not run on
  one (U1).
  *Also (#1762), with F80:* #1762 fixed the same item in parallel and merged
  onto #1763's bridge function and placement. The call runs before
  `csi_event_egress_pump()`, so a bundle that closes is published in the same
  pass, and a bundle opened before CSI is shed still commits on its window or
  quiet gap.
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
    holds it: one call, under no `#if` (#1763's placement; #1762 had put it
    under `#if FEATURE_CSI`, and the merge kept #1763's), a top-level
    statement no gate controls, nothing leaving the loop before it, before
    the egress pump, and a feed that closes nothing (16 mutations refused).
    CI compiles it. The bare call costs nothing with CSI off: no module
    registers there (`securacv_csi_modules_init()` runs only under
    `FEATURE_CSI`, after `csi::init`), so nothing opens and the tick scans
    eight empty slots. A `system.integrity` tamper never waits for the tick:
    it seals its own key with `csi_bundler_flush_key()` at emit. The rule
    forbids every wrapper so that no later gate can strand an open bundle.
    In a host probe of the real bridge and modules, an hour of loop passes
    with the tamper feed running and no init registered no module and opened
    no bundle; after init, with no tick at all, every tamper row committed
    at its emit and no pass saw a `system.integrity` bundle open.
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
  PlatformIO builds are CI's; not bench-verified (U1). Found here: F90 (which
  #1763 filed too), F91, F92 and F93.
- [ ] **F82 [code+decision] Nothing warns before the event-id space runs
  out.** The allocator has 2^30 ids from 0xC0000000 (F46), about 16.5 years at
  the most a device can commit (since F80; recomputed in #1762). At
  exhaustion ids restart at 1, and each later boot first reissues
  0xFFFFFFFF. Home Assistant then refuses the device from then on, and
  nothing on the device says so. Add a health or diagnostic flag once the
  allocator passes `kHoldLimit`
  (0xF0000000), and decide the recovery (a re-pin plus a reset of the floor
  and `csi.evsent`). Found by F46's review (#1761).
  *Partly done (#1762): the warning.* Both trees' MQTT health carries
  `event_id_space_low`, from `csi_event_id_floor::space_low()` of the
  allocator's next id: true from the moment it reaches `kHoldLimit` (2^28 ids
  before the wrap: about four years at the most a device can commit, about 19
  at the shipped defaults), and after a wrap (ids below `kIdSpaceBase`, then
  the 0xFFFFFFFF each later boot reissues). The canary reads it through
  `csi_event_egress_id_space_low()`, the canary-wap in
  `csi_mqtt::publish_health()` (its worst-case body is 350 of 384 bytes).
  `test_csi_event_id_floor.cpp` pins the predicate across a modeled run to the
  wrap and the boot after it; `test_canary_event_egress.cpp` drives the real
  allocator to `kHoldLimit`, through a reboot and a wrap; the canary-wap's
  `test_mqtt_reinit.cpp` checks the published body; each fails with the flag
  reverted. The Lab's WAP health example carries the key (`gen_wap.py`, then
  `gen_homeassistant.py`). Home Assistant shows it as the Event ID Space Low
  binary sensor since HA24 (#1762, wave 12). Still open: the recovery (a
  re-pin plus a reset of the floor and the delivery ceiling) is the decision
  this item asks for; nothing here resets anything. Host-tested; the compiles
  are CI's; not bench-tested (U1: the F82 row in
  `hardware_verification_checklist.md`, which names each device's keys: the
  canary's floor and ceiling are `securacv`/`csi.evid` and `csi.evsent`, the
  canary-wap's `csi`/`ev.next` and `csi.evsent`).
- [x] **F83 [code] The canary-wap can commit an event before its id floor
  is restored.** `csi_integration::init` calls `register_v1_modules()`
  before `apply_event_id_floor_from_nvs()`. `ble_scout_init()` emits
  `initialized("failed")` when `ble_scout_key_init()` fails, and that
  commit's floor write (with `g_id_floor_stored` still 0) overwrites the
  persisted floor before the restore reads it. Only `boot_floor`'s hold at
  `csi.evsent` then limits the reissued ids. It is rare (it needs a
  key-store failure) and older than F46. Restore the floor first, as the
  canary does (`csi_event_egress_begin`). Found by F46 (#1761).
  *Done (#1763):* `csi_integration::init` restores the floor before
  `register_v1_modules()`. Built, not run on a device (U1).
  *Also (#1762), as hardening: the premise was false.* #1762 reordered the
  same two calls in parallel. On #1761's base nothing commits between
  `register_v1_modules()` and `apply_event_id_floor_from_nvs()`:
  `ble_scout_init()`'s `emit_initialized("failed")` carries a state_name, so
  `csi_event_emit` hands it to the bundler, which opens a bundle
  (`CSI_BUNDLER_BUFFERED`): no event id, no `csi_event_on_id_advance`, no NVS
  floor write. The bundle commits when it closes in `loop()`, after the
  restore, with an id from the restored floor. A host probe on the real staged
  sources (`ble_scout.cpp`, `ble_scout_state.cpp`, `ble_scan.cpp`,
  `csi_event.cpp`, `csi_module.cpp`, `csi_bundler.cpp`, the key store failing)
  counted 0 id advances and 0 commits during init, and both reviews confirmed
  it independently on every boot path: `csi_module_register` runs no module
  init, the mesh handlers installed there only log, no bundle slot is open at
  boot for an admit to evict, and ble.scout's NimBLE-task emits start only
  after `ble_bringup_finalize`. The reorder landed anyway because that verdict
  leaned on one emit staying state-bearing and nothing committed held it: the
  tests review showed that dropping the state_name from the "failed" emit
  would reopen F83 with every gate green. #1762 also starts the egress
  (`csi_event_egress::begin()`, F78) after the floor and before
  `register_v1_modules()`, as the canary restores its floor in
  `csi_event_egress_begin()` before its modules; the SD log reload, which
  needs the module manifests, stays after (and F93 runs the modules'
  `init()` after them). `check_wap_event_egress.py`'s boot-order rule
  requires that order (self-test mutations), and it fails on the
  `csi_integration.cpp` before F83. No device behavior changes today, so no
  host test can fail before it. The ESP32 compile is CI's; not bench-tested.
- [x] **F103 [code] On the canary, a failed card append during an outage moves
  the delivery ceiling past the backlog.** On the canary, a row whose card
  append fails while a backlog waits moves the NVS delivery ceiling past that
  backlog. `Planner::commit`'s not-on-card route writes the ceiling
  (`persist_for`) before `hand_to_queue`; the canary then sends the row live
  or into its MQTT offline queue (the planner header's documented trade), so a
  reboot before the backlog drains skips the rows still on the card. The
  canary-wap no longer does (its `WapPort` declines that one write while the
  row will wait in RAM, and writes it when the row goes, #1762), but the
  canary's offline queue publishes later without asking the planner, so the
  same hold there needs the queue's drain to write the ceiling first. Fix it
  in the canonical planner or the canary's port, with a scenario in
  `test_csi_event_backfill.cpp` (a failed append during an outage, then a
  reboot: every card row still arrives). Found by F78 (#1762).
  *Done (#1762), in the canary's port, as on the canary-wap:* the planner is
  unchanged. When a row's card append fails while it must wait (older rows on
  the card or in the RAM hold F104 added, or the link down with the card open),
  `firmware/canary/src/csi_event_egress.cpp` declines the not-on-card route's
  ceiling write and keeps the row in the hold instead of handing it over, so
  the watermark does not move. The hold's flush commits it again through the
  planner once nothing older waits, which writes the ceiling first. So the
  MQTT offline queue, which publishes later without asking the planner, is
  never given a row that older rows wait ahead of, and its drain needs no
  ceiling write. Holding the ceiling for the queue's drain to write, as the
  item proposed, would not have been enough: the queue drains before the
  backfill by design (queued tamper alerts first), so the row would still
  overtake the backlog. Correction to the item: the loss did not need a
  reboot. A host probe on the base planner (eight rows held on the card in an
  outage, one failed append, the broker back) delivered only the failed row
  and the two after it: the watermark had moved to the failed row, and the
  walk skipped the backlog. As on the canary-wap, a failed append that opens
  an outage waits too, since a hand-over's ceiling runs `kStride` ids past the
  row, over the card rows after it.
  Review found that a failed append is not proof the row is off the card. A
  short write that lands all but the newline is sealed by the next append; the
  walk then sent the card's copy, and the held copy went too (HA's replay gate
  passes an equal id, so its triggers fired twice). The canary-wap had the same
  defect since F78. Both egresses now drop a held row at or below the
  planner's watermark (the canary's `delivered_from_card()`, the canary-wap's
  `State::front_delivered()`). Each device has one scenario per path out of
  the hold, and each fails before that fix (both fake cards gain
  `SD.short_by_next`). The review's 300-seed fuzz of the canary egress went
  from 22 seeds delivering an id twice to none.
  `firmware/tests_host/test_canary_event_egress.cpp` (new) compiles the
  canary's real egress and SD adapter (a RAM card) with the real chokepoint,
  planner and offline queue, against a model of `securacv_mqtt.cpp`'s event
  surfaces, the storage manager and HA's replay gate, and checks every
  hand-over against the NVS ceiling. Seven F103 scenarios fail on the egress
  with only F104's fix: a failed append in an outage then a reboot (every card
  row arrives), the same with no reboot, mid-backfill, behind a held row, one
  that opens an outage, one behind a held row with the link down, and one in
  the pass a late card opens. Residual: the failed row itself, held in RAM, is
  lost on a reboot or a broker change, as an offline-queue row is.
  Host-tested; the PlatformIO compile is CI's; not bench-tested (U1: the
  F103/F104 rows in `hardware_verification_checklist.md`). Found here: F107.
- [x] **F104 [code+decision] On the canary, a row committed while its card is
  not open overtakes the card's backlog.** On the canary, a row committed
  while its card is not open overtakes the card's backlog.
  `Planner::card_close()` keeps no note that rows were waiting, and
  `Planner::commit` routes a row as not on the card whenever the card is
  closed: live when the link is up (the ceiling and the watermark move past
  the card's rows), or into the offline queue, whose drain raises HA's mark
  past them. When the card reopens, `card_open` finds its tail at or below the
  watermark and parks the walk at the end, so the rows still on it are never
  sent. `test_csi_event_backfill.cpp`'s `test_card_lost_while_rows_wait` pins
  that loss as the planner's rule. The canary-wap now holds such rows in RAM
  behind a card that may hold older ones, from boot until its log first opens
  and after a close with rows waiting, for at most 45 s (`kCardWaitMs`,
  #1762); the canary's pump (`csi_event_egress_pump`) has no counterpart.
  Decide whether the canary takes the same bounded wait in its port (with a
  scenario: a card closed mid-backlog, a commit, the card back) or states the
  loss as the trade. On the canary this is from reading the planner and the
  pump, not a probe. Found by F78's review (#1762).
  *Done (#1762): the bounded wait, in the canary's port.* Parity was
  possible: the canary's storage manager re-probes a lost or absent card every
  30 s and gives the boot mount 4 s, as the canary-wap's does, so the same
  45 s covers one remount. `kCardWaitMs` moved into `csi_event_backfill.h`
  (`csi_event_backfill::kCardWaitMs`, re-staged), and the canary-wap's egress
  header aliases it, so both egresses read one value. On a build with a card
  slot, from boot until the card's log first opens, and after it closes while
  rows wait on it, a row committed with no log open waits in an 8-row RAM hold
  (heap, PSRAM first; the oldest dropped first, logged) instead of going live
  or into the offline queue. Held rows go through the planner in id order with
  the card's rows (`send_held_below()` before each walked row; `send_live()`
  refuses while the hold is not empty), and their NVS ceiling is written only
  then. With a card open and the link down they wait for the link (a
  hand-over's ceiling would cover the card rows after them); with no card open
  the offline queue takes them, as before. Past 45 s, timed from boot or from
  the pass that saw the close, the card is given up: the held rows go, and
  rows on a card that comes back later are skipped, as on the canary-wap.
  Ambient rows are never held (they are dropped). A broker change drops the
  hold and ends the wait. A change seen before a late card opens is applied
  when it opens (the canary-wap's `not_owed_at_open`), and one made with the
  card open is not applied again at a remount. `test_card_lost_while_rows_wait`
  now pins only the planner's side once the host has given the card up (the
  planner is unchanged: `pending()` is false while the card is closed), and
  says so.
  In `test_canary_event_egress.cpp`, six F104 scenarios fail on the egress
  before the fix: a card closed mid-backlog, a commit, the card back (nothing
  skipped); a card that mounts late; the wait is bounded; a held row writes no
  ceiling over the card rows after it; ambient rows; a broker change before
  the open. Review added scenarios for the wait's timing (a close a minute
  after boot, a card out for one 30 s recheck, the wait ending exactly
  `kCardWaitMs` after it began, at boot and after a close) and for the hold's
  own rules (a close with nothing owed does not wait; the broker-change rules
  above; held rows keep their replay flags; a held row's failed publish is
  retried; a refused flush keeps its rows; overflow drops the oldest; a row
  routed in the pass the wait ends waits behind the hold). The suite has 65
  checks. Of 51 mutations of the canary egress, 49 fail it; the two that pass
  change nothing a host can see (a failed append that takes the hold when
  nothing waits, which the same pass flushes; the flush run before the
  backfill pass, a pass later in the same order). `check_event_egress_order.py`
  follows the pump into `route()` (the tamper bridge still before it; the pump
  never commits a row itself), allows the broker-change `if` one term beyond
  its two (`opened_unowed`), holds the card wait's expiry to leaving nothing
  early, and refuses six more mutations (57 in all with F93's rule 8).
  Behavior changes: on a canary with an SD slot whose card is absent, foreign
  or slow to mount, rows committed in the first 45 s after boot reach Home
  Assistant up to 45 s late, or not at all if the canary reboots or its broker
  changes first. Such a canary in a boot loop shorter than 45 s delivers no
  events-topic row (tamper alerts, the boot's power verdict among them, do not
  wait). Ambient rows from those 45 s are dropped. The same applies while a
  card is out with rows waiting. Host-tested; the PlatformIO compile is CI's;
  not bench-tested (U1). Found here: F108 and F109.
- [x] **F107 [code] Nothing holds `securacv_mqtt.cpp`'s publish-or-queue
  order.** While the canary's MQTT offline queue still drains,
  `publish_or_queue()` puts a new event at the back of it instead of sending it
  live, so it cannot overtake older queued rows. The canary's held-row flush
  relies on that (F104: a held row handed over with the link up while older
  queued rows drain), and so does every not-on-card hand-over. Both host tests
  model the branch (`test_csi_event_backfill.cpp`'s World and
  `test_canary_event_egress.cpp`'s transcription) rather than compile it, and
  `check_event_egress_order.py` rule 1 holds only
  `mqtt_publish_event_live()`'s refusal, so deleting the branch keeps every
  gate green. Add a rule (the `link_up && !s_offline_q.empty()` push before the
  live send) with a mutation. Found by F103 and F104 (#1762).
  *Done (#1762):* the order is one shared function, compiled by the host
  tests. `firmware/common/mqtt/mqtt_offline_queue.h` gains
  `publish_or_queue()`: while the link is up and the queue still holds
  records, a new record joins its back; one the queue refuses (an over-size
  payload, or an event a full queue of tamper alerts refuses) goes live; with
  the link down it is queued. `securacv_mqtt.cpp`'s `publish_or_queue()` is
  now one call to it, for its event and tamper surfaces.
  `test_canary_event_egress.cpp` and `test_csi_event_backfill.cpp` drop their
  transcriptions and compile it (the backfill test's World now runs the real
  12-slot queue; its copy had refused an event that a full queue of tamper
  alerts refuses, which the firmware sends live), and
  `test_mqtt_offline_queue.cpp` pins the order directly. Two new egress
  scenarios route a row, and flush a held row, in the pass the queue still
  drains an outage; with the branch removed from the header, or moved after
  the live send, 5 egress checks and the queue test fail.
  `check_event_egress_order.py` rule 9 holds the header's branch shape and
  `securacv_mqtt.cpp`'s delegation (the wrapper is the one statement
  `return mqtt_offline_queue::publish_or_queue(s_offline_q, s_mqtt.connected(), ...)`,
  and `mqtt_publish_event()` and `mqtt_publish_tamper()` call neither
  `s_mqtt.publish(` nor `s_offline_q.push(`), with 13 self-test mutations (70
  in all); it fails on the `securacv_mqtt.cpp` before. Rule 1 still holds the
  live send's refusal while the queue drains, and both tests still model
  `mqtt_publish_event_live()` and `mqtt_loop()`'s drain. No device behavior
  changes. Host-tested; the PlatformIO compile is CI's; not bench-tested.
- [ ] **F108 [decision] Both egresses wait 45 s for a card at every boot, even
  with none in.** A canary or canary-wap with an SD slot and no card (or
  another device's card) holds every row committed in the first 45 s after
  boot in RAM, and drops ambient rows from those seconds, because a card might
  still mount with older rows on it (F78, F104). Those rows arrive up to 45 s
  late, or not at all if the device reboots or its broker changes first, so
  such a device in a boot loop shorter than 45 s delivers no events-topic row
  (the tamper topic and the boot power verdict are unaffected). The boot probe
  often knows better: the storage manager's boot mount either found no card or
  is still running (`sd_mount_policy.h`). Decide whether a probe that finished
  and found no card ends the wait at once, at the cost of the rows of a card
  seated in the first 30 s, and weigh the boot-loop case. Found by F104
  (#1762).
- [x] **F109 [code] The canary's event egress has no counters a host can
  read.** Rows dropped from its RAM hold reach only a Serial line, ambient
  rows it could not hold are dropped uncounted, and the planner's Stats (live,
  held, queued, replayed, skipped, untrusted, read give-ups) are never
  surfaced, while the canary-wap exposes `csi_event_egress::stats()`. Put them
  in the MQTT health or `/api/diagnostics` so a bench run (and a field report)
  can see what the egress dropped. Found by F104 (#1762).
  *Done (#1762), in the MQTT health:* `csi_event_egress_stats()`
  (`firmware/canary/src/csi_event_egress.h`) returns the canary-wap's
  `csi_event_egress::Stats` shape field for field: `dropped` (commits the full
  egress queue refused), `held_dropped` (rows the RAM hold dropped, oldest
  first), `ambient_dropped` (ambient rows that had to wait, now counted),
  `unsent_dropped` (rows no card kept that the MQTT layer refused with nothing
  to keep them: an offline queue with no memory, or one full of tamper alerts
  while the link is down; `route()` used to discard that `Route::kUnsent`),
  and the planner's `live`, `held`, `queued`, `replayed`, `skipped`,
  `untrusted`, `unsendable`, `truncated_unsent` and `read_giveups`. The
  canary-wap's Stats gains `unsent_dropped` too, counting the rows whose body
  never builds where it drops one (a refused row waits in its hold).
  `mqtt_publish_health_update()` carries them as a `csi_event_egress` object
  (the planner's under `planner`), read on the loop task, where the pump
  writes them, beside an `offline_queue` object with the MQTT offline queue's
  `dropped_overflow`, `dropped_oversize` and `dropped_flushed`
  (`mqtt_offline_queue_stats()`): with no card, an outage longer than the
  queue's 12 slots evicts the oldest events there, after the egress counted
  them `queued`. They start over at every boot. `/api/diagnostics` does not
  carry them: it runs on the httpd task. The worst-case health packet grows to
  1745 B, so `MQTT_BUFFER_SIZE` goes from 1280 to 1792 (+512 B of heap). The
  counters describe paths, not a per-row ledger: `docs/csi_developer_api.md`
  defines each one and lists the rows none of them counts (a held row sent
  ahead of a card row, rows a broker change or a given-up card wait abandons,
  a held row whose card copy went), and `held` minus `replayed` is not what is
  owed. In `test_canary_event_egress.cpp`, `test_egress_counts_what_it_did`,
  `test_card_less_losses_are_counted` (an inert offline queue, a queue full of
  tamper alerts, a 14-row outage the queue overflows) and checks in the
  ambient, hold-overflow and failed-append scenarios fail with the counting,
  the hold exclusion or the accessor reverted; the canary-wap's
  `test_unbuildable_rows_are_skipped_not_stalled` fails with any of its three
  counts reverted; `test_canary_health_trust.py` holds the worst case to the
  buffer (it fails at 1664), each egress key to the field of the same name in
  the canary-wap's struct and the planner's, and each `offline_queue` key to
  the queue's Stats, and `python.yml` now runs it when the firmware files it
  reads change. Docs: `csi_developer_api.md`, the HA setup guide, a bench row
  in `hardware_verification_checklist.md`. Neither the canary-wap's counters
  nor Home Assistant read any of it yet (F149, HA24); both do since wave 12
  (#1762). Host-tested; the PlatformIO and Arduino compiles are CI's; not
  bench-tested (U1).
- [x] **F149 [code] The canary-wap's egress counters reach no surface.**
  `csi_event_egress::stats()` (`dropped`, `held_dropped`, `ambient_dropped`,
  `unsent_dropped`, the planner's Stats) is read only by
  `test_wap_event_egress.cpp`, while the canary publishes the same counters in
  its MQTT health since F109 (#1762). The canary-wap's health body has 34 of
  its 384 bytes spare at worst, so the thirteen counters (a 341-byte object at
  their widest) need a larger body, a topic of their own or its local
  diagnostics. Found by F109 (#1762).
  *Done (#1762), on a retained topic of its own and in `GET
  /api/diagnostics`:* `csi_event_egress::stats_json()` (`csi_event_egress.h`)
  spells the thirteen counters as one object in the names the canary's MQTT
  health uses for its `csi_event_egress` object (the planner's nested as
  `planner`; 319 bytes at the widest; static asserts on both `Stats` sizes
  fail the build until a new counter is spelled there). MQTT:
  `csi_mqtt::publish_egress()` publishes
  `{"firmware_version":…,"uptime":…,"csi_event_egress":{…}}`, retained, on
  `{prefix}/{device_id}/egress`, as the statement right after
  `publish_health()` in the 60 s health block, on the loop task, where the
  pump writes the counters. The version and uptime are the ones the health
  body it follows carried (nothing goes out before the boot's first health
  body), so a reader can tell this boot's counters from a retained body an
  earlier boot, or a firmware with no egress topic after a rollback, left on
  the broker. The body is up to 405 bytes, in a 448-byte buffer
  (`kEgressBodyMax`). Not in the health body: it has 34 of its 384 bytes spare
  at the worst case, and a topic of its own leaves the body, buffer and key
  list the trust-on-first-use hook and every health consumer read as they
  were. Like health, the topic is unsigned and not in the reconnect burst, and
  its bytes count in the outbound-byte total. `GET /api/diagnostics`
  (token-gated, `FEATURE_SYS_MONITOR` builds; no page in the sketch calls it)
  carries the object as `csi_event_egress`, `null` before the loop task's
  first pump, built by `wap_diagnostics.h` (pure, host-tested): 649 bytes at
  the widest, in the route's 768-byte `kJsonMax` buffer (it was 512). The
  route runs on the httpd task, so `pump()` publishes its counters through
  `loop_snapshot.h` as its last statement every pass, and `read_stats()` gives
  any task a whole copy at most one pass old. Tests: in
  `test_wap_event_egress.cpp`, `test_other_tasks_read_what_the_pump_published`
  and `test_the_counters_spell_the_canarys_names`; in `test_mqtt_reinit.cpp`,
  `the_egress_counters_ride_a_retained_topic_of_their_own` (nothing before a
  health, the exact body, the next health's uptime, the retain flag, the
  widest body, nothing while the link is down; with F150's empty-NVS test
  beside it the suite is 23 tests and 343 checks in this tree); and
  `test_wap_diagnostics.cpp` (26 checks: every key, the object unquoted,
  `null` before the first pump, the widest body inside `kJsonMax`, a buffer
  one byte short refused whole). Each fails with its piece reverted.
  `check_wap_event_egress.py` rule 12 (21 self-test mutations, 83 in all)
  counts call sites on squashed code and holds which task reads what, the
  topic's cadence (`csi_mqtt::publish_egress(` once, as the statement right
  after `publish_health(...)` in the one health block inside `if
  (csi_mqtt::connected())`), and the handler's reader, builder and buffer; the
  check fails on the sources before. Docs: `csi_developer_api.md`,
  `homeassistant_setup.md`, `device_trust.md`, the WAP README, `csi_mqtt.h`,
  the Lab's WAP topic list (`gen_wap.py`, then `gen_csp.py --check`, then
  `gen_homeassistant.py`) and a bench row in
  `hardware_verification_checklist.md`. Host-tested; the canary-wap's
  PlatformIO and Arduino compiles are CI's; not bench-tested (U1). Found here:
  F179 and F180.
- [x] **F179 [code] The canary's `GET /api/diagnostics` does not carry its
  egress counters.** F109 kept them to its MQTT health because the route runs
  on the HTTP server's task. Since F149 the canary-wap's route reads them
  through a copy its loop task publishes each pass (`loop_snapshot.h`), and
  the canary could do the same, so a bench run without a broker would see
  them. Found by F149 (#1762).
  *Done (#1762):* the canary's token-gated `GET /api/diagnostics`
  (FEATURE_DIAGNOSTICS builds) carries `csi_event_egress`, the thirteen
  counters its MQTT health carries, under the health's names, `null` before
  the loop's first pass and on a build with no egress.
  `csi_event_egress_pump()` publishes a whole copy of
  `csi_event_egress_stats()` as its last step every pass, under a FreeRTOS
  spinlock (an unchanged copy takes no lock), and
  `csi_event_egress_read_stats()` copies it out on any task: at most one pass
  old, never torn (the canary-wap's `loop_snapshot.h` Value<T>, written out in
  `csi_event_egress.cpp`, since that header is the sketch's).
  `csi_event_egress_stats_json()` spells it (319 bytes at the widest; static
  asserts on both Stats sizes fail the build until a new counter is spelled).
  The route, in `lib/securacv_network`, reaches it through
  `include/csi_event_egress_diagnostics.h`, since no library here includes a
  `src/` header, and now builds its body with the pure
  `lib/securacv_diagnostics/src/diagnostics_json.h`: every key it had (`heap`,
  `sd`, `selftest`, `system`), byte for byte (compared against the old
  snprintf chain on four snapshots), then `csi_event_egress`. The old chain
  appended with `pos += snprintf(...)`, which past the buffer would have
  handed the next call a wrapped size, and dropped self-test rows to stay
  inside it; the builder holds the widest body (1352 bytes) inside the route's
  2048-byte buffer and refuses one that does not fit whole (500
  `diagnostics_too_large`). No `offline_queue` object (the health carries it).
  Tests: `test_canary_event_egress.cpp`'s
  `test_other_tasks_read_what_the_pump_published` and
  `test_the_diagnostics_object_is_refused_whole`, and the new
  `test_canary_diagnostics.cpp` (27 checks: the spelling with every counter
  distinct, each of the thirteen names `main.cpp`'s health publish uses
  carrying that field's counter, the widest object and body, and the handler
  held to the builder, its buffers and the published copy, against seven
  mutations). With `publish_stats()` taken out of the pump 10 checks fail,
  moved to the pump's start 5; the base handler, two swapped counters or a
  renamed health key each fail `test_canary_diagnostics`. Docs:
  `csi_developer_api.md` (the route's keys and where the counters come from),
  `homeassistant_setup.md`, and a bench row in
  `hardware_verification_checklist.md` ("The canary's diagnostics carry its
  egress counters (F179)"). Host-tested; the PlatformIO compiles are CI's; not
  bench-tested (U1).
- [x] **F180 [code] canary-wap's `csi_event_egress::watermark()` names a
  diagnostics reader it does not have.** Its header says "The delivery
  watermark (diagnostics and host tests)", but only
  `test_wap_event_egress.cpp` calls it; `GET /api/diagnostics` carries the
  counters (F149), not the watermark. Add the watermark to the copy the pump
  publishes and to the route's builder (`wap_diagnostics.h`), or correct the
  comment. Found by F149 (#1762).
  *Done (#1762), by correcting the comment:* `csi_event_egress::watermark()`'s
  header now says what it is (the highest event id handed to the MQTT layer or
  given up, restored at boot to just under the NVS ceiling, so after a reboot
  up to kStride ids past the last row handed over), that it is the loop
  task's, and that only the host tests read it: no route or topic carries it,
  and `GET /api/diagnostics` and the `egress` topic carry the counters. Not
  added to the published copy and the route: nothing reads it today, its
  meaning after a reboot is not "what Home Assistant has", and a
  canary-wap-only key would break the one-object-on-both-devices shape F149
  and F179 give the diagnostics (if a bench run needs it, it belongs in both
  devices' diagnostics). Comment only; no behavior changed, so no test.
- [ ] **F105 [code+decision] Ambient rows reach the SD event log and the
  `events` topic.** Ambient rows reach the SD event log and the `events`
  topic, though `csi_event.h` says `CSI_CATEGORY_AMBIENT` is "never persisted,
  drives live UI only". Both trees' egresses log and publish every committed
  ring row the privacy ceiling passes, and `wifi.channel_activity` commits
  ambient ring rows, up to one per cooldown (5 s by default, 1 s at its
  minimum). A host probe on the canary-wap's egress commits one ambient emit
  and finds 230 bytes on the card and one `events` publish; the hook before
  F78 did the same. Since #1762 the canary-wap never holds an ambient row in
  RAM (one that cannot go out at once is dropped, counted), and since F104
  (#1762) neither does the canary (one that would wait in its hold is
  dropped, uncounted: F109), but with a card in they are still logged and
  replayed on both. Decide whether ambient rows go to the
  card and the broker; if not, filter them in both egresses, which also keeps
  the canary-wap's 16-row commit queue far from full. Found by F78 (#1762).
- [x] **F106 [code] The canary-wap's `csi_mqtt::init()` destroys the esp_mqtt
  client under a concurrent publish.** The canary-wap's `csi_mqtt::init()`
  destroys the esp_mqtt client under a concurrent publish. A config POST or
  `POST /api/mqtt/test` runs `init()` on the httpd task, which calls
  `teardown_client()` (`esp_mqtt_client_stop`, `esp_mqtt_client_destroy`, then
  `s_client = nullptr`), while the loop task's egress (or any other publish in
  `csi_mqtt.cpp`) can be inside `publish_raw()` with the old handle, which
  only null-checks it. Pre-existing; F78 narrowed it to one publishing task
  for events, and `csi_mqtt.h` now says so. Hand a runtime re-init to the loop
  task (`csi_mqtt::loop()`), or guard the client with a lock, and hold it with
  a static check. Found by F78's review (#1762).
  *Done (#1762):* the esp_mqtt client is the loop task's, and the loop task
  never stops one. Any task can call `csi_mqtt::request_reinit()`, which bumps
  a request counter. `csi_mqtt::loop()` serves the request ahead of the egress
  pump, without waiting: it detaches the open client, and a one-shot worker
  task (`retire_task`, 6 KB stack) runs `esp_mqtt_client_stop` and
  `esp_mqtt_client_destroy`. Detaching sets `s_client`, now atomic, to
  `nullptr`, so no loop publish can reach the old client; the event handler
  ignores every event whose client is not `s_client`, and subscribes on its
  own `e->client`. A later pass opens the new client once the worker reports
  the old one gone. The review found the first version stopping the client on
  the loop task: `esp_mqtt_client_stop` takes the client's API lock, which the
  esp_mqtt task holds across a whole connect attempt (`network_timeout_ms`,
  10 s by default, read from esp-mqtt's master branch; the pinned core's copy
  was not read). In the review's host model, "Test & save" against an
  unreachable broker held the loop task about 9.9 s, past its 8 s panic
  watchdog. A worker that cannot be created is retried on the next pass; the
  client is never stopped inline. One open serves every request made before
  its NVS read began, including one made while the old client is still
  stopping; a request made during the open waits for the next re-init. The
  config POST asks for a re-init and waits up to 2 s, so the page's status
  refresh sees the new client, and answers `{"ok":true}` as before (the save
  stands either way). `POST /api/mqtt/test` asks, then waits for the re-init
  and the connect within the 4 s it always waited, reads `s_connected` only
  after the re-init, and keeps its answer's fields; while an old client is
  still stopping against a dead broker it answers `ok:false` within those 4 s.
  The QR hub provision only asks. `setup()` gives the bridge its identity
  first (`csi_mqtt::set_identity`), so that request is served with the
  identity even when the HTTP server, and the boot `init()` in it, never
  started. A second `init()` asks instead of stopping. The OTA settings
  handler published the auto-update switch from the httpd task;
  `set_update_auto_state()` replaces `publish_update_auto_state()` and caches
  the state for `loop()` to publish. `retire_task` logs how long each stop
  took.
  Pinned by `test_mqtt_reinit.cpp` (14 tests, 211 checks), which compiles the
  real `csi_mqtt.cpp` over a fake esp_mqtt that records each call's task,
  notices a client destroyed under a publish, and can make the stop hold its
  caller for 9940 ms; every loop pass is timed against a 1 s budget. Each fix
  reverted fails it: the reviewed code (stop on the loop task) fails 11 of the
  14 tests; the config POST tearing the client down in place (the code before
  F106) destroys the client of a loop-task publish in flight; the test handler
  doing the same runs its stop on the httpd task; the auto-update state
  published in place publishes from the httpd task; without the handler's
  client check a detached client's late connect sets `connected()`; with every
  request marked served a Save made during the open gets the old settings;
  with the re-init after the pump the pump misses the new destination's epoch
  for one pass. `check_wap_loop_commands.py` (below, F96) holds it in the
  source: `esp_mqtt_client_stop` only in `retire_task()`;
  `esp_mqtt_client_destroy` only there and on `open_client()`'s own unstarted
  client; init and start only in `open_client()`, which stores `s_client`
  before the start; `s_client` written only by `open_client()` and
  `detach_client()`; `serve_reinit()` returning while a client retires and
  running before the pump; the handler's client check first. No HTTP handler
  may name `init`, an `esp_mqtt_client_*` call, a write of `s_client`, a
  lifecycle step, `csi_mqtt::loop`, `mesh_network::update` or a publish; the
  boot `init()` is held to one call, and the QR scanner to requesting.
  `test_mqtt_identity.cpp`'s HA20 pins follow the identity to
  `set_identity()`, and `check_wap_event_egress.py`'s broker-epoch rule (F78)
  reads `open_client()`, where the epoch bump now lives. Residual: `GET /api/mqtt/config` still reads the
  transport name and last error while a re-init may be writing them; both are
  constants, so a torn read is a stale word. The loop task's own publishes can
  still block inside esp-mqtt (F112). Host-tested; the Arduino and PlatformIO
  compiles are CI's; not bench-tested (U1: the F96/F106 rows in
  `hardware_verification_checklist.md`, "Test & save" against an unreachable
  broker IP included). Found here: F112.
  *Since F112 (#1762):* every client's network timeout is 2 s (esp-mqtt now
  read at the commit the pinned core uses, 6af4446), so the retiring stop
  waits out at most a 2 s connect step plus esp_mqtt's reconnect wait of up to
  5 s, and a loop-task publish over a stalled link waits one 2 s timeout, not
  10 s (what is left is F142).
- [x] **F112 [code] canary-wap's loop-task MQTT publishes can block inside
  esp-mqtt past the watchdog.** Every `csi_mqtt` publish (the event egress,
  health, status, counts, mesh) calls `esp_mqtt_client_publish` on the loop
  task. When the client is connected, that call takes the client's API lock
  and writes the socket through `esp_mqtt_write`, that is `esp_transport_write`
  with `network_timeout_ms`, 10 s by default (`csi_mqtt.cpp` does not set
  `cfg.network.timeout_ms`). If Wi-Fi or the broker stalls with the TCP send
  buffer full, one publish could hold the loop task longer than its 8 s panic
  watchdog. Pre-existing: these publishes always ran on the loop task; F106
  took only the client's stop off it. Options: a `network.timeout_ms` well
  under the watchdog, or `esp_mqtt_client_enqueue` (the esp_mqtt task sends)
  for the loop's publishes. From esp-mqtt's source (master branch, as F106's
  review read it), not from the pinned core's copy; not probed. Found by
  F106's review (#1762).
  *Done (#1762):* `open_client()` sets `cfg.network.timeout_ms` to
  `csi_mqtt::kNetworkTimeoutMs` (2000 ms) on every client, in the IDF 5 nested
  config the file already builds. Correction to the item: canary-wap compiles
  only on Arduino-ESP32 3.x (pioarduino 55.03.38-1, that is 3.3.8 on ESP-IDF
  5.5.4; the Arduino CLI legs install the latest 3.x), so there is no core 2.x
  `network_timeout_ms` form to set; and esp-mqtt was read at the commit
  ESP-IDF 5.5.4 pins (6af4446), not master, where the path is as the item
  describes. A publish takes the client's API lock and writes through
  `esp_mqtt_write()`, whose every `esp_transport_write()` gets this timeout; a
  write that sends nothing in it fails and aborts the connection (DISCONNECTED
  dispatched on the publishing task, which clears the bridge's link), and
  later publishes return at `publish_raw()`'s gate; the esp_mqtt task holds
  the same lock across its own socket operations with the same timeout (a
  ping, a resend, the rest of a long incoming message). So a link that stops
  costs a loop pass one timeout. The review found a wait the timeout does not
  bound: esp_mqtt dispatches CONNECTED with the API lock held, and the
  bridge's handler set the link up and then sent its reconnect burst under
  that lock (the status, the discovery configs, the cached states and the
  subscribes: 34 publishes, about 16.5 KB, and 3 subscribes in the host
  build), so a loop publish that came meanwhile waited for the whole burst,
  bounded only by the link's throughput. The handler now announces the link
  (`s_connected`) only once the burst is sent, unless a DISCONNECTED (the
  burst's own failed write) or the loop task's detach came meanwhile; the
  burst's own publishes pass the gate as the esp_mqtt task (`s_burst_task`).
  The connect itself (the TCP or TLS connect, the CONNECT write, the CONNACK
  wait) is not a wait the loop meets: an abort clears the link before
  esp_mqtt's reconnect delay, which it waits with the lock released, and
  `refresh_connection_after_ms` is not set. `canary_wap.ino` static_asserts
  `kNetworkOpsBudget` (3: an esp_mqtt operation a publish waits behind that
  ends just inside the timeout, the publish's own write, and room for the rest
  of the pass) times `kNetworkTimeoutMs` under `WATCHDOG_TIMEOUT_SEC`.
  `esp_mqtt_client_enqueue` was not taken: it takes the same lock, so it would
  not bound the wait behind the esp_mqtt task, and the event egress (F78)
  reads `publish()`'s -1 as "not sent, keep the row", where enqueue at QoS 0
  succeeds into an outbox the esp_mqtt task may later drop. Behavior changes:
  a socket operation that makes no progress for 2 s now aborts the connection
  where it took 10 s, including a publish that finds the TCP send buffer full
  during a backfill or the reconnect burst; an abort makes the broker publish
  the will, so Home Assistant shows the board unavailable through esp_mqtt's
  10 s reconnect wait and the connect, and QoS 0 event rows already handed to
  the socket are lost with the watermark past them (older than F112, which
  shortens its trigger: F147); a broker that takes more than 2 s to answer one
  step of a connect fails that attempt and esp_mqtt retries 10 s later, which
  the Home Assistant Mosquitto add-on can meet on a login its Supervisor has
  not cached (F148); F106's retiring stop waits out at most a 2 s connect step
  instead of 10 s, plus esp_mqtt's reconnect wait of up to 5 s; the link
  counts as up (and `POST /api/mqtt/test` as connected) once the reconnect
  burst is sent rather than as it starts, and a loop-task publish during the
  burst is refused (the egress keeps the row for a later pass). The
  `csi_mqtt.cpp` note that said publishes are posted to the esp_mqtt task's
  queue is corrected. Pinned by `test_mqtt_reinit.cpp`'s six F112 tests (21
  tests and 316 checks in all, with F82's health test) over a fake that
  follows that write path and holds the API lock across the esp_mqtt task's
  dispatches: every client carries the timeout; over a stalled link a busy
  loop pass (`loop()`, status, health, counts, chain, chirp state) waits out
  one timeout and then nothing; a publish behind the esp_mqtt task's stalled
  write waits one timeout; a stall under the timeout keeps the connection; a
  loop pass in the middle of the CONNECTED burst over a slow link (250 ms a
  write, the burst itself over 8 s) waits for no lock; a burst cut short by
  its own failed write leaves the link down. With the config line removed,
  four fail (the 10 s default held the pass 10 s); with the previous handler,
  the burst test fails (the pass waited 10250 ms for the lock); with the
  announcement made unconditional, the cut-short test fails.
  `check_wap_loop_commands.py` rule M1 holds the config line before
  `esp_mqtt_client_init()`, the constants (a budget of at least 3, the product
  under the watchdog) and the sketch's `static_assert`. Residual: one 2 s
  timeout on a stalled link, over LESSONS' 1 s rule, and a link that trickles
  bytes restarts it on every write (F142). From esp-mqtt's source and a host
  model; not probed on a device. Host-tested; the Arduino and PlatformIO
  compiles are CI's; not bench-tested (U1: the F112 rows in
  `hardware_verification_checklist.md`). Found here: F142, F147 and F148.
- [ ] **F142 [code] A canary-wap loop-task MQTT publish can still wait out a
  network timeout, or longer on a trickling link.** Since F112 a link that
  stops costs a loop pass one `kNetworkTimeoutMs` (2 s) before the connection
  aborts, which is over LESSONS_LEARNED's "anything that can block >1 s does
  not belong on the loop task". A link that makes a little progress just
  inside the timeout restarts it on every partial write (`esp_mqtt_write()`
  loops until the message is out), so a long publish (a discovery config, a
  backfill row) or a pass of several can wait longer, bounded by throughput,
  not by the timeout. The reconnect burst no longer holds the loop (F112's
  review), and the connect is not a wait the loop meets. A publishing worker
  (the loop task hands rows to it and reads results back, keeping F78's
  per-row outcome) would take esp_mqtt off the loop task. From esp-mqtt's
  source at the IDF 5.5.4 pin and a host model; not probed. Found by F112
  (#1762).
- [ ] **F147 [code+decision] canary-wap event rows sent at QoS 0 can be lost
  at a connection abort with the egress watermark already past them.**
  `publish_raw()` reports a row sent when `esp_mqtt_client_publish()` returns
  a message id, which at QoS 0 means the bytes reached lwIP's send buffer, and
  the egress counts it delivered (`csi_event_egress.cpp`'s `to_sent()`: kSent
  is kYes) and moves on. When the link then stalls, the next publish that
  finds the buffer full waits out the network timeout and aborts the
  connection, and the rows still in the buffer are gone (the broker also
  publishes the will). F112 shortened the trigger from 10 s to 2 s; the loss
  path is older. QoS 1 for event rows with the watermark moved on the PUBACK
  (`MQTT_EVENT_PUBLISHED`), or any watermark that moves only on the broker's
  acknowledgment, would close it; either changes F78's per-row delivery
  accounting and the outbox's RAM use, hence the decision. From esp-mqtt's
  source and F112's review; not probed (U1: the stalled-broker row counts
  them). Found by F112's review (#1762).
- [ ] **F148 [decision] canary-wap's 2 s MQTT network timeout is also the
  whole CONNACK wait.** esp-mqtt's `esp_mqtt_connect()` waits
  `network_timeout_ms` in total for the broker's CONNACK. Home Assistant's
  Mosquitto add-on validates a login its Supervisor has not cached through
  Core, which its auth.py says takes 1-2 s, so the first connect after an
  add-on or Supervisor restart (or a password change) can fail once and retry
  10 s later (the retry is a cache hit). `kNetworkTimeoutMs` could be raised
  to 2500 ms and still keep `kNetworkOpsBudget` (3) times it under the 8 s
  watchdog, at the cost of a longer stall on the loop task; or left at 2 s and
  the one retry accepted. Decide, and say so in `csi_mqtt.h`. From source (the
  CONNACK wait at esp-mqtt 6af4446; auth.py only in the review's saved copy,
  not upstream); not probed. Found by F112's review (#1762).
- [ ] **F90 [code+decision] An hour of unbroken presence fills a 6/hour
  ceiling by itself.** A bundle closes at its 10-minute window even while
  the room stays occupied, and the next refresh opens a new one: a new row,
  which spends a ceiling slot (F80 made that true on every path). So
  sustained presence opens six bundles an hour, `core.presence`'s whole
  ceiling (6), and a real state change after that is dropped at the
  ceiling until the oldest slot rotates out. core.presence refreshes every
  state, empty included, so after about an hour in any one state the ceiling
  is full of that state's own rows, and every refresh of the held state is
  refused too, because the ceiling is checked before the bundler runs. On
  the canary's real bridge (host, fake clock; holds of one to one and a third
  hours in 7 s steps) the transition out waits up to about ten minutes, 7 to
  602 s depending on when a slot ages out, and every later row of the held
  state carries one observation and 0 s. The canary-wap already did this
  before F80 (its loop tick closes the bundle before the next refresh admits,
  so its reopens were already counted; on the old library, 2 of 17 clock
  phases admitted a transition at once after two hours in one state). The
  canary did worse before F81 (every refresh spent a slot).
  Options: a window rollover of a bundle still inside its quiet gap counts
  as the same presence (a continuation); or the ceiling reserves room for a
  change of state; or `core.presence`'s ceiling rises above the window rate
  (12/hour, a privacy-contract change); and, whichever is chosen, refuse at a
  full ceiling only an emit that needs a slot, decided from the admit's
  outcome as the refund now is, and extend `test_csi_bundler_ceiling.cpp`.
  Filed by both PRs: found doing F80 (#1763), and by F80 and F81 (#1762).
- [ ] **F132 [code+decision] The hourly ceiling's hour is six 10-minute
  buckets, so a sliding 60 minutes can hold twice the ceiling.**
  `csi_event.cpp`'s per-module counter keeps six 10-minute buckets, the
  current one and the five before it, and refuses an emit when they already
  hold the ceiling. Any six consecutive buckets therefore hold at most the
  ceiling's openings, but a sliding 60 minutes does not: six openings at the
  end of one bucket and six more once that bucket rotates out are twelve in
  about fifty minutes. Rows add their own lag, since a bundled row commits
  when its bundle closes, two to ten minutes after the opening a bucket
  counted. On the library (host, `CSI_TEST_CLOCK`) one sliding hour of
  committed rows held 7 in F80's 121 s probe (none of them a flush's), 8 in
  the window probe (two of them its final flush's) and 12 at a bucket edge,
  while each day held 144. Both trees run the same library. The counter's
  comment said "sliding 60-minute window" and F80's Done text "no hour holds
  more than the ceiling"; the comment is corrected, F80 carries the
  correction, and `test_csi_bundler_ceiling.cpp` pins the bucket-edge 12
  (`test_a_sliding_hour_can_hold_twice_the_ceiling`). The ceiling is
  privacy-relevant (F90 calls raising it a privacy-contract change), so
  decide what "per hour" promises. Options: count a true sliding hour of
  openings (each module keeps its last N opening times, N the ceiling, which
  the override can raise to 255), or say in the module docs and the privacy
  contract that the bound is per six-bucket hour and up to twice that in any
  60 minutes. Either way rows still commit at their bundle's close, with a
  lag of two to ten minutes that varies per row, so even a sliding-hour
  bound on openings lets committed rows bunch; only a count taken at commit
  time bounds rows per 60 minutes, and a closing bundle cannot be refused
  without losing it. F90's fix (who gets a slot at a full ceiling) has to be
  decided with this one. Not a regression: before F80 the 121 s and window
  probes committed 714 and 286 rows a day. Found by the review of the #1763
  merge into #1762.
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
  rows. Found reviewing F81 (#1762).
  *Since F93 (#1762):* a cooldown the canary-wap's Tuning Lab stored (30 to
  3600 s) applies from boot too, and one under two minutes lets a repeat merge
  into the open bundle; at the 600 s default nothing merges, as above.
- [ ] **F92 [code+decision] An open bundle dies with a reboot.** Bundles live
  in RAM until they close, up to 10 minutes after they open, and no planned
  restart (`deliberate_restart_now()` for `POST /api/reboot`, an OTA, a
  settings restart) closes them first, in either tree. On the canary the
  commit hooks only queue for the loop's egress pump, so a flush before a
  restart would also need the pump to drain before the card or the broker sees
  the row. Until F81 the canary lost at most one CSI window; now it loses up
  to ten minutes of any state-bearing row (a presence state, breathing, an
  anomaly row's two minutes), as the canary-wap always did. Only
  `system.integrity` seals its row at commit. Found by F81 (#1762).
  *Since F103 and F104 (#1762):* a restart also loses the rows waiting in
  either egress's RAM hold (up to eight), on the canary as on the canary-wap,
  so a flush before a planned restart would need that hold drained too.
- [x] **F93 [code] Neither tree calls a CSI module's `init()` at boot.**
  `csi_module_register()` only records the module. The canary's
  `securacv_csi_modules_init()` never calls `init`, and the canary-wap calls
  it only in `reinit_module()`, from three HTTP handlers
  (`handle_settings_post`, `handle_calibrate_apply`,
  `handle_tune_post_coefficients`). So core.presence's preset, sensitivity,
  thresholds and pet mode, and every other module's NVS settings
  (`anomaly.baseline`'s cooldown among them), take effect on the canary-wap
  only after a settings change in that boot, and never on the canary, whose
  `csi_module_settings_*` overrides nothing reads at boot. Read from source;
  not checked on a device. Found reading F81's bridge (#1762).
  *Done (#1762):* both trees run every registered CSI module's `init()` once
  at boot, with its stored settings, after the event-id floor and the events
  egress and before the first tick. The library's new `csi_module_init_all()`
  runs each module's `init()` once (latched per module), and
  `csi_module_tick_all()` ticks no module whose boot init has not run, so
  `csi_module.h`'s promise ("init() is called exactly once before any
  tick()") holds by construction: a host that forgets the call gets a dead
  pipeline, not ignored settings. On the canary, `securacv_csi_modules_init()`
  calls it last, through `init_modules_from_nvs()`: after `setup()`'s
  `csi_event_egress_begin()` restored the floor, and before the features
  callback. On the canary-wap, `csi_integration::init()` calls
  `csi_settings_nvs_init_modules()` right after `register_v1_modules()`, so
  after the floor and the egress (F83), and before the HAL installs its
  callback.
  Both trees read settings by one rule,
  `firmware/common/csi/src/csi_module_settings_nvs.h` (staged): namespace
  "csi", the canary-wap's short-key table, and typed reads. The canary's copy
  of the table lacked its `core.privacy_ceiling` row. An unmapped key, an
  absent row or a namespace that will not open reads as the caller's default.
  The settings handle `csi_module.h` always described is now a read session: a
  boot reads every module's settings through one read-only handle (16 mapped
  rows among core.presence, core.breathing and anomaly.baseline), so a
  namespace that will not open costs one attempt, not one per setting. A NULL
  handle, which the canary-wap's `reinit_module()` uses after a settings
  change, still opens a handle per key. The canary-wap's overrides moved out of
  `csi_integration.cpp` into a sketch-local `csi_settings_nvs.cpp` (+ `.h`),
  which a host suite compiles.
  What each `init()` does, checked: core.presence, core.breathing and
  anomaly.baseline read their settings and reset their state;
  wifi.channel_activity reads three keys no tree maps, so it gets its defaults
  (F122); core.activity_ribbon sets its bucket start to now;
  meta.daily_summary, core.multilink_fusion and meta.empty_room_baseline reset
  their state; meta.quiet_hours, ble.events, system.integrity,
  acoustic.events, vault.events and ble.scout have empty `init()`s. None
  emits, allocates or arms a timer. With nothing stored, every `init()` leaves
  the values the statics already held, except the ribbon, whose first
  15-minute bucket now starts when the modules initialize, not at power-on.
  So nothing today leans on F46/F83's order. `csi_module.h` now says `init()`
  must not emit, and both host tests pin it.
  Tests: `firmware/tests_host/test_csi_module_boot.cpp` (10 tests: the
  canary's real bridge, the real library and modules, a fake NVS across
  modeled reboots) and `firmware/projects/canary-wap/tests_host/test_wap_module_boot.cpp`
  (8 tests: the sketch's real readers and boot function with the staged
  library and modules; it reads `register_v1_modules()` so the list it boots
  is the device's, and checks that the three feature-gated modules it leaves
  out have empty `init()` bodies). They show that a stored preset,
  sensitivity, direct threshold, pet mode and anomaly cooldown apply from the
  boot's first windows; that a stored threshold wins over the preset and the
  sensitivity, at boot as at once (on the canary-wap with the Tuning Lab's
  reset rows, read from `TUNE_COEFFS`; a precedence older than F93 and not
  changed here); that a boot opens NVS once and reads 16 rows, and a second
  call reads none; that a missing namespace costs one attempt and leaves the
  defaults; that the boot init commits nothing and opens no bundle, and the
  boot's first row takes the restored floor; that no tick comes before
  `init()` and no `init()` runs twice; and that a settings change on the
  canary-wap still applies at once. Against the code before F93, 8 of the
  canary's 10 tests and 5 of the canary-wap's 8 fail. The rest guard the new
  path (both commit tests, the canary's key-map test, the canary-wap's
  missing-namespace test and source pin). Handing `csi_module_init_all()` no
  session fails the once-per-boot and missing-namespace tests on each tree; an
  `init()` that emits fails the commit tests; each tree's threshold-precedence
  test fails, case by case, with `on_init()` made to let the preset win.
  Static checks: `check_wap_event_egress.py`'s boot-order rule (3) and
  `check_event_egress_order.py`'s new rule 8 hold each tree's call, its place
  (top level, after the registrations, before the first tick), its single
  caller, and the tree's one `csi_module_init_all()` call inside the boot
  function, handed the session. They add 11 and 14 mutations, and both fail on
  the sources before F93. The canary's floor-before-modules order was held by
  nothing until rule 8. The common/csi multilink and empty-room tests now run
  the boot init their comments assumed `register()` ran.
  What changes on a device: on the canary-wap, a preset, sensitivity,
  calibrated threshold, pet mode, shimmer setting, breathing lock or anomaly
  coefficient saved in an earlier boot applies from the first window of every
  boot. Before, it applied only after a change in that boot: presence after any
  `/api/settings` POST or calibration apply, breathing and anomaly after a
  Tuning Lab change to that module. Two limits predate F93 and are unchanged
  by it, and `docs/csi_developer_api.md` says both: a stored threshold
  (`cp.mt`, `cp.at`, `cp.bt`) wins over the preset and the sensitivity, which
  set only those reads' default, and the calibration's apply, the Tuning Lab's
  per-row reset and Reset all, and a bundle import all store those rows, so on
  such a board a saved preset changes nothing, now at boot as it already did
  within a boot, while `GET /api/settings` keeps reporting it (F127); and the
  Tuning Lab's three Quiet Hours knobs belong to no module, so they apply from
  the next boot or at once through an `/api/settings` Quiet Hours change,
  never from the Tuning Lab POST itself (F128). On the canary nothing writes
  these rows, so a freshly flashed canary behaves as before; a board whose NVS
  holds rows a canary-wap image stored now applies them. A canary logs one
  Preferences `nvs_open failed: NOT_FOUND` line at boot, for the namespace it
  never created (F125). Host-tested; the ESP32 compiles are CI's; not
  bench-tested (U1: the F93 section in `hardware_verification_checklist.md`;
  its preset row needs a board with no stored presence threshold, or NVS
  erased first, and a fourth row records the threshold winning over the preset
  after a reboot). Found here: F121-F128 and D11.
  *Since F123, F125 and F128 (#1762):* a Tuning Lab or bundle Quiet Hours
  change applies at once, the Lab declares the device's own 23:00 to 07:00
  default, and a clean canary boot logs no NVS error.
- [x] **F121 [code] meta.daily_summary never emits.** Nothing in either tree
  calls `meta_daily_summary_set_clock()`, so the module's clock stays at its
  0xffff sentinel and its `tick()` returns before the 23:55 check. No
  `daily_summary` row is ever committed, though `docs/csi_modules.md` lists it
  as one row per day. Feed it the household minute of day from each tree's
  loop (the chokepoint already keeps the F28 clock offset), or drop the claim.
  From reading both trees: there is no caller of the setter, and F93's boot
  `init()` only re-arms the sentinel. Not probed on a device. See F77. Found by
  F93 (#1762).
  *Done (#1762):* both trees feed `meta.daily_summary` the household minute of
  day and the local date it falls on, so one `daily_summary` row commits per
  local date, at 23:55 local time. The loop pass that already derives the
  chokepoint's F28 clock offset hands the module both: the canary's
  `updateCsiClockOffset()` (`main.cpp`, through a new plain-typed bridge call,
  `securacv_csi_modules_set_clock(minute, local_date)`, so `main.cpp` still
  includes no module header) and the canary-wap's `update_csi_clock_offset()`.
  The date is a key that changes when the local date does (from `localtime_r`;
  UTC's if that fails, as the minute falls back). Both updaters run on every
  loop pass with a set clock (GPS is either tree's one clock source), and each
  refuses an unsynced clock itself (below the trees' existing 1700000000
  floor), so a device whose clock never synced commits no summary (its uptime
  minute would otherwise reach 23:55 a day after boot). The module
  (`meta_daily_summary.{h,cpp}`, staged byte-identical, `check_csi_sync.sh`
  green) holds the local date whose row is done instead of a flag let go
  before 00:30: a clock stepped back inside the window, a DST fall-back over
  23:55 or a zone moved west across midnight commits no second row for that
  date, and every new date owes its row however the clock reached it (a night
  with no CSI windows, a DST spring-forward at midnight, a zone moved east
  after the row, a GPS step past midnight). A first clock since `init()` that
  already reads 23:55 to 23:59 commits none for that date, since that boot
  cannot know whether the one before it did. What it cannot cover, said in the
  module header and the docs row: a date on which 23:55 never happens has no
  row; a clock stepped back more than a date can summarize an earlier date
  again (the latch is RAM); and the row still fires on a module tick, so a
  device whose CSI windows are starved from 23:55 to 23:59 commits none that
  day. What the row carries was checked against `docs/csi_modules.md` and the
  readers: a P0 event row whose `bundled` counts the committed ring rows it
  walked and whose `note` reads `a<A> q<Q> x<X>`. Closed bundles never reach
  the ring (F77), so on a device it reads `a0 q0 x0`, and no published surface
  carries `note` (F219). On MQTT it is `"type":"daily_summary"` with
  `"event_type":"unknown"`, which Home Assistant's Last event sensor shows
  (HA28). The doc was the wrong side: `docs/csi_modules.md` claimed active
  minutes, a longest quiet stretch and an anomaly count, and now says what the
  row is. canary-wap Quiet Hours spanning 23:55 hold the row like any
  non-anomaly row, against the chokepoint's own comment (F218). Tests:
  `firmware/tests_host/test_csi_daily_summary.cpp` (72 checks) and
  `firmware/projects/canary-wap/tests_host/test_wap_daily_summary.cpp` (81),
  each cutting its tree's updater verbatim out of the device source and
  running it over the real library and modules: one row per synced day at
  23:55, no second after a reboot at 23:57, none in fifty unsynced hours, one
  per date across Havana's spring-forward and Santiago's fall-back, eastward
  and westward zone moves and a clock step, a New York day dated in local
  time, source pins on the per-pass call, and the Quiet Hours hold. Each fix
  reverted (the feed, the floor guard, the date key, the first-clock rule, a
  UTC minute or date) fails named cases. Bench row: "Daily summary at 23:55
  (F121)" in `hardware_verification_checklist.md`. Host-tested; the ESP32
  compiles are CI's; not bench-tested (U1). Found here: F218, F219, HA28 and
  A48.
- [ ] **F218 [decision] Quiet Hours hold the daily summary, against the
  chokepoint's own comment.** On the canary-wap a Quiet Hours window spanning
  23:55 (the device default, 23:00 to 07:00, once turned on) holds
  `meta.daily_summary`'s row like any non-anomaly row: no `daily_summary`
  commits, and the morning's `held_summary` counts it as one more held row
  (`test_wap_daily_summary.cpp`'s
  `test_quiet_hours_over_2355_hold_the_summary`). `csi_event.cpp`'s step-4b
  comment (both trees) says every non-anomaly event from a non-meta module is
  suppressed, but the code exempts only `meta.quiet_hours`
  (`is_qh_summary_module`), so the daily summary, and
  `meta.empty_room_baseline`'s `baseline_status` should one emit inside the
  window, are held. Decide between the comment (let every meta module, or the
  summary alone, pass the gate as `meta.quiet_hours`' own summary does) and
  the code (keep them held and correct the comment), or move the summary to
  the window's close. F121 left both as they stand. Found by F121 (#1762).
- [ ] **F219 [code+decision] The daily summary's counts reach no reader.**
  `meta.daily_summary` puts its counts in `note` (`a<A> q<Q> x<X>`), which no
  published surface carries (the MQTT events body, the card line and
  `/api/events/today` leave `note` out), and they count committed ring rows,
  which closed bundles never reach (F77), so on a device they read `a0 q0 x0`.
  The wire shows only `"type":"daily_summary"` and `bundled` (rows walked,
  earlier summaries among them). Decide what the summary reports (periods or
  minutes per state, which needs F77's bundles, or a `duration_sec` sum) and
  where a reader sees it (a wire field, Home Assistant attributes, the Today
  sheet). Found by F121 (#1762).
- [ ] **F122 [code+decision] wifi.channel_activity's three tunables have no
  NVS row.** `wifi_channel_activity.cpp` documents `spike_ratio`,
  `min_activity` and `cooldown_sec` as NVS-backed via the Tuning Lab, but
  neither tree's key map (now `csi_module_settings_nvs.h`) has their keys, and
  the Tuning Lab lists none, so they always read their defaults (pinned in
  `test_csi_module_boot.cpp`). Add rows and Tuning Lab entries, or drop the
  claim. Found by F93 (#1762).
- [x] **F123 [code] The Tuning Lab's Quiet Hours defaults disagree with the
  device's.** In the canary-wap's `csi_integration.cpp`, `TUNE_COEFFS`
  declares `core.quiet_hours.start_min` 0 and `end_min` 480, while
  `apply_quiet_hours_from_nvs()` and `GET /api/settings` default to 1380 and
  420 (23:00 to 07:00). On a device that never stored them,
  `/api/tune/coefficients` and the preset bundle report 00:00 to 08:00, and
  loading that bundle back stores those values, which stay inert while Quiet
  Hours is off. From reading; not probed. Found by F93 (#1762).
  *Done (#1762):* one default, and one set of rows. The canary-wap's
  `csi_settings_nvs.h` holds the default: `kQuietHoursDefaultEnabled` (false),
  `kQuietHoursDefaultStartMin` (23 * 60) and `kQuietHoursDefaultEndMin` (7 *
  60). The Tuning Lab's table declares those values; it moved, with the Lab's
  POST, from `csi_integration.cpp` to a sketch-local `csi_tune_lab.cpp` that a
  host suite compiles. The chokepoint's boot apply and `GET /api/settings`
  read Quiet Hours through one reader, `read_quiet_hours()`
  (`csi_settings_nvs.cpp`), by the shared key map's `qh.*` rows, and
  `POST /api/settings` stores them through `store_quiet_hours_from_settings()`
  beside it, by the same map (it wrote `qh.en` / `qh.start` / `qh.end` by hand
  in `csi_integration.cpp`, which no host suite compiles; the parse moved
  unchanged). So a device that never stored them shows the Lab, and exports in
  its bundle, Quiet Hours off from 23:00 to 07:00, the Lab's reset buttons
  store that window, and what the dashboard saves is what the Lab, the boot
  and `GET /api/settings` read. The dashboard's own first values already
  matched, and a test now pins them to the constants. Tests
  (`firmware/projects/canary-wap/tests_host/test_wap_tune_lab.cpp`, 315
  checks): the Lab declares the device default and reports it for a store with
  no rows; an owner who turns Quiet Hours on in the Lab and resets start and
  end holds a row at 23:30 and not one at 07:30 after a reboot; a bundle
  import keeps the end at 07:00; the dashboard starts from the same window;
  the dashboard's store writes the rows the reader, the boot and the Lab read,
  and reads the Lab's; the rows keep their stored names (devices hold their
  window under them); and mutation-checked source pins hold
  `GET /api/settings` to `read_quiet_hours(prefs)`, its POST to the store, to
  counting it as a write and to one apply after NVS closes, and
  `csi_integration.cpp` to defining no table, reader or apply of its own. With
  the table's old 0 / 480 restored, the default, reset and bundle tests fail;
  with the store writing a row under another name, the round trip fails; with
  the key map's row renamed in both copies, the stored-name test fails; with
  the POST's apply deleted, or `csi_integration.cpp` as it stood before the
  store moved, the pins fail. `docs/csi_developer_api.md`'s
  `GET /api/settings` example shows 1380 / 420 (it showed 0 / 480, and an `ok`
  field the handler never sends). Host-tested; the ESP32 compiles are CI's;
  not bench-tested (U1: `hardware_verification_checklist.md`, "Tuning Lab
  Quiet Hours (F123, F128)", including a dashboard-and-Lab row). Found here:
  F151.
- [ ] **F124 [decision] The canary reads CSI module settings that nothing on it
  can write.** Since F93 the canary applies stored `csi` rows at boot by the
  canary-wap's rule, but it has no `/api/settings`, calibration or Tuning Lab
  surface, so its owner cannot set a preset, sensitivity, threshold or pet
  mode; only a canary-wap image on the same board could have stored them.
  Decide whether the canary gets a settings surface (and which keys), or says
  plainly that its CSI modules run on defaults. Found by F93 (#1762).
- [x] **F125 [code] A canary logs one NVS error at every boot.** F93's boot
  init opens the `csi` namespace read-only once, and nothing on a canary
  creates it. Arduino-ESP32's `Preferences::begin()` logs `nvs_open failed:
  NOT_FOUND` at error level, which the release envs keep
  (`CORE_DEBUG_LEVEL=1`). Probe for the namespace without the log (IDF's
  `nvs_open` directly), or create it once, so a clean boot logs no error.
  Found by F93 (#1762).
  *Done (#1762):* a clean canary boot logs no NVS error. The shared read
  session's `begin()` (`firmware/common/csi/src/csi_module_settings_nvs.h`,
  staged into the canary-wap sketch) asks IDF's `nvs_open()` first
  (`probe_namespace()`): a namespace it reports as `ESP_ERR_NVS_NOT_FOUND` is
  never handed to `Preferences::begin()`, so nothing is logged, and every read
  of that boot is its default, as the refused open made it. A namespace that
  is there is opened and closed by the probe, then opened by Preferences as
  before; any other answer still tries the Preferences open, so a real NVS
  fault keeps its error line. The same open runs in the canary-wap's boot init
  (`csi_settings_nvs_init_modules()`), so its first boot after an erase gets
  the same quiet probe, and a canary-wap that ever saved a setting behaves as
  before. That IDF's `nvs_open()` answers NOT_FOUND without an error-level log
  is from IDF's source as read, not seen on a board. The host fakes now model
  NVS namespaces (a read-only open of one never created fails and counts one
  Preferences error log) and IDF's `nvs_open()`. Tests:
  `test_a_clean_canary_boot_logs_no_nvs_error`
  (`firmware/tests_host/test_csi_module_boot.cpp`) and
  `test_a_never_written_namespace_opens_nothing_at_boot`
  (`firmware/projects/canary-wap/tests_host/test_wap_module_boot.cpp`): no
  error log and no Preferences open for a namespace never written, and one
  open, no log, once a canary-wap image wrote a row. Both fail with `begin()`
  going straight to Preferences (one open, one error line).
  `test_an_nvs_fault_costs_the_boot_one_open` (each tree) keeps a fault's one
  open and one error line. The canary-wap's other read-only opens of `csi` at
  boot still log on a first boot after an erase (F150). Host-tested; the ESP32
  compiles are CI's; not bench-tested (U1: the F93 section's canary row in
  `hardware_verification_checklist.md` now expects no `nvs_open failed` line).
- [ ] **F126 [decision] The securacv-csi library changed a contract without a
  version.** Since F93, `csi_module_tick_all()` ticks no module until
  `csi_module_init_all()` has run its `init()`, so a third-party sketch that
  registers and ticks modules without that call gets no ticks.
  `library.json` and `library.properties` still say 0.4.0, and the committed
  firmware SBOM pins it. Decide whether to bump the library version and
  regenerate `sbom/sbom-firmware.cdx.json`. Found by F93 (#1762).
- [ ] **F127 [code+decision] The dashboard's preset and sensitivity do nothing
  once a threshold row exists.** `core.presence`'s `init()` reads `cp.mt`,
  `cp.at` and `cp.bt` over the preset and sensitivity baseline, which it
  passes only as those reads' default. The canary-wap stores those rows in
  several places: the calibration's apply writes all three, and so do the
  Tuning Lab's per-row reset and Reset all (each stores its `TUNE_COEFFS`
  default as a row) and a bundle import (`POST /api/tune/preset`, every
  coefficient). After any of these, a preset or slider change applies nowhere,
  at boot or at once, while `GET /api/settings` still reports it and the
  dashboard still shows it. Decide the precedence. Options: a preset or
  sensitivity change clears the direct rows; a reset deletes the row instead
  of storing the default; or the dashboard says that calibrated thresholds
  override it. Pinned as it stands by `test_a_stored_threshold_wins_over_the_saved_preset`
  (canary-wap) and `test_a_stored_threshold_wins_over_the_stored_preset`
  (canary), which a fix would update. Found reviewing F93 (#1762).
  *Since F166 (#1762, wave 13):* the calibration status and the Tuning Lab
  report the thresholds the module runs, and the status says whether they are
  stored, from the preset or mixed (`current_source`, which this item's third
  option could show). So a bundle exported and loaded back stores the
  thresholds the device ran rather than the balanced ones, but they still win
  over a later preset change, and the Lab's per-row reset and Reset all still
  store the balanced default as a row.
- [x] **F128 [code] A Quiet Hours change from the Tuning Lab applies only after
  a reboot.** `TUNE_COEFFS` gives `core.quiet_hours.enabled`, `start_min` and
  `end_min` an empty reinit module, and `handle_tune_post_coefficients()`
  (also the bundle import) never calls `apply_quiet_hours_from_nvs()`, which
  runs only from `handle_settings_post()` on a Quiet Hours change and from
  `register_v1_modules()` at boot. So a Tuning Lab or bundle Quiet Hours change
  is stored and reported but not applied until the next boot or an
  `/api/settings` Quiet Hours POST. Call `apply_quiet_hours_from_nvs()` when a
  `core.quiet_hours.*` knob changes, with a host or static pin.
  `docs/csi_developer_api.md` now states the current behavior. From reading;
  not probed. Found reviewing F93 (#1762).
  *Done (#1762):* a Quiet Hours knob stored through the Tuning Lab, or by a
  bundle import (the same handler), reaches the chokepoint at once. The Lab's
  table records what each knob applies (`TuneApply`: one module's re-init, or
  Quiet Hours), and the POST's store-and-apply, `tune_post()`
  (`csi_tune_lab.cpp`), stores the body, closes NVS, re-runs each touched
  module's `init()` once through `csi_integration.cpp`'s `reinit_module()`,
  and calls `apply_quiet_hours_from_nvs()` (now in `csi_settings_nvs.cpp`)
  once when a `core.quiet_hours.*` knob was stored.
  `handle_tune_post_coefficients()` hands its body to
  `tune_post(body, reinit_module)` and answers. Where the applies run was
  checked: only the mesh, Chirp and Bluetooth commands and the MQTT re-init
  run on the loop task (F96, F106 and, in this wave, F111), while
  `/api/settings`, the calibration's apply and the Lab apply on the HTTP
  server task, where `csi_event_set_quiet_window()` is documented as a state
  update (the next emit on the loop task flushes any held summary). So the Lab
  applies there too, as `/api/settings` does. Tests (`test_wap_tune_lab.cpp`):
  at 12:00 a Lab change to 11:30-12:30 holds the next row with no reboot and
  no module re-init; moving the start past now releases it behind a
  `held_summary` of the held rows; turning it off in the Lab flushes the
  summary; a bundle carrying every knob re-runs each module's `init()` once
  and applies its Quiet Hours; every knob applies to its own group; NVS that
  will not open stores and applies nothing; unknown keys change nothing;
  values clamp. With `tune_post()` not re-applying Quiet Hours, the
  apply-at-once and bundle tests fail. Source pins, checked against 13
  in-memory mutations, hold the POST handler to
  `tune_post(body, reinit_module)`, the bundle import to that handler,
  `reinit_module()` to the test's model, the boot to its one apply, and
  `POST /api/settings` to its one Quiet Hours apply after NVS closes.
  `docs/csi_developer_api.md` and the Lab's footer (`tune_ui.h`) say a Quiet
  Hours knob applies at once. Host-tested; the ESP32 compiles are CI's; not
  bench-tested (U1: the checklist section named under F123).
- [x] **F150 [code] A canary-wap's first boot after an NVS erase logs several
  `nvs_open failed: NOT_FOUND` errors.** From reading:
  `csi_integration::init()` reads the event-id floor, the events egress its
  delivery ceiling (`csi_event_egress.cpp`'s `begin()`),
  `register_v1_modules()` the Quiet Hours, then the privacy ceiling, and the
  HAL config its transmitter filter, and `canary_wap.ino` one more, each
  through a read-only `Preferences::begin("csi")`, before anything writes the
  namespace (the first event's id-floor write creates it). F125 made only the
  modules' boot session quiet (`csi_module_settings_nvs::probe_namespace()`);
  the others could use the same probe, or the boot could create the namespace
  once. Not counted on a device. Found by F125 (#1762).
  *Premise corrected (#1762, wave 12):* that boot logs two of these lines, not
  six: the event-id floor's read and the events egress's ceiling read.
  `csi_event_egress::begin()`, finding no ceiling record, stores one
  (`csi_event_backfill.h`'s `restore()`), which creates the namespace before
  `register_v1_modules()` and the other reads run. And the lines exist only on
  a build that keeps Arduino's error log (the `canary-wap-debug` env, or an
  arduino-cli build at Core Debug Level Error or above): the release image is
  built at Core Debug Level None (`canary-wap-default` inherits
  `CORE_DEBUG_LEVEL=0`), where Arduino-ESP32's `log_e` compiles to nothing, so
  it printed none before F150 either.
  *Done (#1762):* by the probe, not an early create.
  `csi_module_settings_nvs.h` (shared with the canary, staged byte-identical;
  `check_csi_sync.sh` green) gains `begin_read_only()`, F125's
  `probe_namespace()` in front of `Preferences::begin()`, which answers false
  with no open and no line for an absent namespace, as the refused open
  answered, so every caller's result is unchanged. Every read-only open of
  `csi` in the sketch uses it, 13 call sites: the floor read (now
  `read_event_id_floor_rows()` in `csi_settings_nvs.cpp`, so a suite runs it;
  `check_csi_sync.sh`'s floor rule follows it), the egress ceiling, Quiet
  Hours, the privacy ceiling, the filter, the time zone, GET /api/settings,
  the calibration status, the Tuning Lab's two GETs, the MQTT bridge's
  settings and CA reads, and the zone id. An early create would write flash
  once on such a boot and change what the floor read answers; the probe leaves
  both as they were (that boot still prints `[EVT-LOG] event-id floor not
  readable from NVS`, F165). Tests: `test_wap_module_boot.cpp`
  (`begin_read_only()` on absent, present and faulted NVS; `init()`'s reads in
  order on empty NVS, with and without the egress's record, no error line
  either way; a source pin holding every read-only open of `csi` in every
  NVS-touching sketch source to the helper, each of the 13 put back as a plain
  open and caught), `test_wap_event_egress.cpp` (the ceiling read on empty NVS
  logs nothing, the planner's first record creates the namespace) and
  `test_mqtt_reinit.cpp` (the bridge's settings read on empty NVS logs nothing
  and loads its defaults). The egress and MQTT NVS fakes now model namespaces
  and IDF's `nvs_open()`. With the probe taken out of the helper all three
  suites fail; with any one call site reverted a test or the pin fails. That
  IDF's `nvs_open()` answers NOT_FOUND without an error-level line is from
  IDF's source as read (F125). Host-tested; the ESP32 compiles are CI's; not
  bench-tested (U1: "canary-wap first boot after an NVS erase (F150)" in
  `hardware_verification_checklist.md`, on a `canary-wap-debug` build). Found
  here: F164 and F165.
- [x] **F151 [code] canary-wap's dashboard and calibration write the presence
  module's settings rows by literal key.** `handle_settings_post()` stores
  `"cp.pet_mode"`, `"cp.preset"` and `"cp.sens"`, and
  `handle_calibrate_apply()` `"cp.mt"` / `"cp.at"` / `"cp.bt"`, by name in
  `csi_integration.cpp`, which no host suite compiles, while `core.presence`
  reads them through the shared key map (`csi_module_settings_nvs.h`).
  `test_wap_module_boot.cpp` holds the map to those names, but nothing holds
  the handlers' literals to the map: renaming the POST's `"cp.sens"` to
  `"cp.sen"` leaves the canary-wap host suites, `check_csi_sync.sh` and
  `regression_check.sh` green (probed), and on a device the slider would then
  save a value no module reads. Route the writes through `nvs_key_for()` in a
  host-tested store, as F123 did for Quiet Hours
  (`store_quiet_hours_from_settings()`), or pin them. Found by F123's review
  (#1762).
  *Done (#1762):* premise re-probed on an export of 7f45142: with the POST's
  `"cp.sens"` renamed `"cp.sen"`, every canary-wap host suite,
  `check_csi_sync.sh`, `regression_check.sh`, `check_wap_event_egress.py` and
  `check_wap_loop_commands.py` stayed green. The stores and their readers
  moved to `csi_settings_nvs.cpp`, by `nvs_key_for()`, as F123 did for Quiet
  Hours: `store_presence_from_settings()` and `read_presence_settings()` (the
  dashboard's pet mode, preset and sensitivity), `store_presence_thresholds()`
  and `read_presence_thresholds()` (the calibration's apply and its status's
  `current`, whose fallback is now `presence_threshold_defaults()`, the
  reader's own defaults, not a literal of its own), and the privacy ceiling's
  `cp.pc`, the same class spelled by hand in three places
  (`store_privacy_ceiling_from_settings()`, `read_privacy_ceiling()`, and
  `apply_privacy_ceiling_from_nvs()`, moved from `csi_integration.cpp`). Every
  default is the handler's. Tests (`test_wap_tune_lab.cpp`, four new): what
  the dashboard and the calibration store is what core.presence, the Tuning
  Lab and GET read, under the names devices hold (`cp.pet_mode`, `cp.preset`,
  `cp.sens`, `cp.mt`, `cp.at`, `cp.bt`, `cp.pc`); each of the dashboard's own
  one-key bodies is a stored write by itself (in review, three of the four
  could drop their flag with every suite green); clamps, unknown values and
  keys inside other keys behave as before; any stored ceiling but 1 or 2
  applies P0. Source pins hold GET, the POST, the calibration's apply and its
  status to the stores and readers, each value under its own name (28
  in-memory mutations, among them the item's probe in its current form and the
  review's four), and a sketch-wide pin refuses any string literal starting
  with a key-map prefix (`cp.`, `cb.`, `qh.`, `ab.`) in every NVS-touching
  sketch source but the map. The exact probe cannot be applied to the new
  handler (the literal is gone): applied to the base handler, `"cp.sen"` fails
  both pins, and each of 20 mutations of the stores and handlers fails a test.
  `docs/csi_developer_api.md` names the rows. Host-tested; the ESP32 compiles
  are CI's; not bench-tested (U1: "canary-wap dashboard presence settings and
  calibration (F151)" in `hardware_verification_checklist.md`). Found here:
  F166.
- [x] **F164 [code] canary-wap's first boot after an NVS erase still logs
  `nvs_open failed: NOT_FOUND` for the mesh namespace, on a build that keeps
  Arduino's error log.** From reading: `mesh_network::init()` opens `mesh`
  read-only for the opera config and the deny-list (on a board with flash
  encryption), the last-seen tombstones and the send-counter record, and the
  sketch's `load_replay_counters()` once more, before anything writes it, and
  Arduino-ESP32's `Preferences::begin()` logs each failed open at error level:
  up to five lines (three without flash encryption) on a `canary-wap-debug`
  build or an arduino-cli build at Core Debug Level Error or above; the
  release image (Core Debug Level None) compiles them out. A device with no
  opera that never turns the mesh on keeps no `mesh` namespace only until the
  sketch's replay save first runs (every five minutes of uptime, and before a
  reboot through the API), which opens it read-write even with no peers and so
  creates it; such a build logs them at each boot before its first five
  minutes up or its first API reboot, not at every boot (corrected by F164's
  review, #1762, wave 13). `csi_module_settings_nvs::begin_read_only()`
  (F150) takes any namespace and would quiet them; the mesh_net host stub's
  `begin()` succeeds on any namespace, so no harness counts these lines today.
  Not counted on a device. Found by F150 (#1762).
  *Done (#1762):* a canary-wap boot that finds no `mesh` namespace logs no
  `nvs_open failed` line for it. Those boots are the first after an NVS erase
  and any later one before the sketch's replay save first opens the namespace
  read-write: that save runs every five minutes of uptime (`canary_wap.ino`'s
  `REPLAY_SAVE_INTERVAL_MS`) and before a reboot through the API (the
  pre-reboot hook), whatever the mesh's state, and with no peers
  `save_replay_counters()` still opens `mesh` read-write, which creates it.
  Every boot after that finds it, and logged nothing before F164 either (the
  item's "at every boot" is corrected in place). Every read-only open of
  `mesh` in `mesh_network.cpp`, six sites (`load_opera_config()`,
  `load_peers()`, `load_revocations()`, `load_rx_tombstones()`,
  `load_tx_reservations()` and `load_replay_counters()`, which the sketch
  calls right after `init()`), goes through F150's
  `csi_module_settings_nvs::begin_read_only(prefs, NVS_NS)`: an absent
  namespace is not handed to `Preferences::begin()`, so every read returns its
  default, as the refused open made it; a namespace that is there, or an NVS
  fault, goes to Preferences as before. Nothing else in the mesh changes. The
  `stubs/mesh_net` Preferences now models NVS namespaces (a read-write open or
  a stored key creates one; only clearing the whole store removes it), refuses
  a read-only open of an absent namespace and counts its error line in
  `host_sim::nvs_error_logs`, refuses a second `begin()` on a begun handle as
  Arduino does, and gains an `nvs.h` answering the probe from the same store;
  the address, liveness, command and Chirp suites on it pass on the old mesh
  code too. Tests (`test_mesh_liveness_wap.cpp`):
  `a_first_boot_with_no_mesh_namespace_logs_nothing` (no error line, no
  Preferences open, with flash encryption on and off, nothing created),
  `a_device_up_five_minutes_holds_the_mesh_namespace` (the reach: after the
  five-minute or the pre-reboot save the namespace exists, empty, and the next
  boot opens it with no line),
  `a_boot_that_finds_the_mesh_namespace_opens_it_as_before`,
  `an_nvs_fault_at_boot_keeps_its_error_lines` and
  `every_read_only_open_of_mesh_is_the_quiet_one` (a source pin; each of the
  six put back as a plain open is caught). On the base `mesh_network.cpp` the
  harness counts five error lines (three without flash encryption), the item's
  count, and the first test, the reach test's first-boot check and the pin
  fail. The F150 pin in `test_wap_module_boot.cpp` now counts only the quiet
  opens of `csi`. That IDF's `nvs_open()` answers NOT_FOUND without an
  error-level line is from IDF's source as read (F125). Docs: a bench row in
  `hardware_verification_checklist.md` ("canary-wap boot with no mesh
  namespace (F164)", on a `canary-wap-debug` build, the second boot a power
  cycle inside the first five minutes). Host-tested; the ESP32 compiles are
  CI's; not bench-tested (U1). Found here: F201.
- [ ] **F165 [decision] canary-wap's first boot after an NVS erase says its
  event-id floor is unreadable and skips the card log's reload.**
  `apply_event_id_floor_from_nvs()` reports NVS unread when the `csi`
  namespace does not exist yet (the events egress creates it right after), so
  `csi_integration::init()` prints `[EVT-LOG] event-id floor not readable from
  NVS - the log is not reloaded this boot`, a fault's wording on a device that
  is only new, and does not arm the Today ring's reload from the card; the
  next boot reloads it. Decide whether an absent namespace is "nothing stored"
  (arm the reload; `csi_event_inject` refuses every row at or above the
  restored floor, so only rows below the one id space would load) or keep the
  answer and reword the line. F150 kept the answer as it was. From reading.
  Found by F150 (#1762).
- [x] **F166 [code] canary-wap's calibration status reports the balanced
  thresholds as current whatever the preset.** `handle_calibrate_status()`
  shows `current` as the stored `core.presence.*_threshold` rows, or
  `presence_threshold_defaults()` (35 / 75 / 30) when none is stored, while
  core.presence then runs its preset and sensitivity baseline (sensitive 25 /
  60 / 20, quiet 50 / 90 / 40, each moved by up to 20 by the slider), so on a
  device that saved a non-balanced preset and stores no threshold the
  dashboard's before/after shows thresholds the module does not use. Report
  the baseline the module derives (from `read_presence_settings()`), or say
  the value is a default. Related to F127's precedence decision. From reading
  core_presence.cpp's `on_init()`. Found by F151 (#1762).
  *Done (#1762):* the calibration status (`GET /api/csi/calibrate/status`, a
  ready run) reports as `current` the thresholds core.presence runs, and says
  where they come from. The baseline derivation moved out of `on_init()` into
  `core_presence_baseline_thresholds(preset, sensitivity)`
  (`firmware/common/csi/src/core_presence.{h,cpp}`, staged byte-identical,
  `check_csi_sync.sh` green; the preset rows keep their text, which
  `canary-local/tools/gen_wap.py` reads, so `wap.json` is unchanged), which
  `on_init()` still passes as each threshold row's default. The canary-wap's
  `read_presence_thresholds_in_use()` (`csi_settings_nvs.cpp`) reads each row
  as `init()` does (a row of another type counts as absent, as it does for the
  module), else takes the baseline from `read_presence_settings()`;
  `presence_thresholds_in_use_unread()` answers what `init()` runs when NVS
  does not open. The status reports those numbers and adds `"current_source"`:
  `"stored"` (all three rows), `"preset"` (none) or `"mixed"`.
  `read_presence_thresholds()` and `presence_threshold_defaults()`, whose
  balanced fallback was the bug, are gone. The precedence (a stored row wins
  over a later preset or slider change) is unchanged and still F127's
  decision; the dashboard renders the numbers, not `current_source`. The
  status reads NVS, not the module's runtime state, so a dismissal's one-point
  nudge (until the next `init()`) does not show, as before. Tests:
  `test_wap_tune_lab.cpp`'s
  `the_calibration_status_reports_the_thresholds_in_use` and
  `test_wap_module_boot.cpp`'s
  `test_the_calibration_status_reports_what_the_module_runs` (for six ways the
  rows can stand, the booted module reads one below the reported motion
  threshold as "empty" and the threshold itself as "subtle"); both fail with
  the reader ignoring the stored preset (the old answer: 35 where "sensitive"
  runs 25) or counting every row as stored. The Tuning Lab had the same bug
  (found by F166's review): `GET /api/tune/coefficients` and the bundle export
  `GET /api/tune/preset` answered an absent `cp.mt` / `cp.at` / `cp.bt` with
  `TUNE_COEFFS`' balanced 35 / 75 / 30, so on a device set to "sensitive" with
  no threshold row the Lab's sliders said 35 / 75 / 30 while the status said
  25 / 60 / 20, and a bundle exported and loaded back stored 35 / 75 / 30 as
  rows, which then won over the preset. `tune_read_value()`
  (`csi_tune_lab.cpp`) now answers those three with
  `read_presence_thresholds_in_use()`, and every other knob as before; their
  declared `default` stays the balanced numbers (the Lab's reset stores it as
  a row, and changing that is F127's). `the_lab_reports_the_thresholds_in_use`
  covers NVS not open, each preset and two slider positions, a stored row, a
  row of another type and the export/import round trip (25 / 60 / 20 before
  and after); it fails with the new line taken out. Two new pins hold both GET
  handlers to `tune_read_value()`. Docs: `csi_developer_api.md` and two bench
  rows in `hardware_verification_checklist.md` (the calibration's
  before/after, and the Lab's values with a bundle round trip). Host-tested;
  the ESP32 compiles are CI's; not bench-tested (U1).
- [x] **F201 [code] canary-wap's first boot after an NVS erase may still log
  `nvs_open failed: NOT_FOUND` for the `beacon` and `securacv` namespaces, on
  a build that keeps Arduino's error log.** From reading, not counted:
  `beacon_channel.cpp` opens `beacon` read-only on a board with flash
  encryption (`load_beacon_set()` and `load_audit_log()` from its init,
  `ensure_x25519_keypair()` on first use) before anything writes it, and
  `setup_wizard.h`'s `init()` and `power_monitor.h`'s `load_nvs_state()` open
  `securacv` read-only, which may come before the first write of that
  namespace on such a boot (not traced). F150 and F164 quieted `csi` and
  `mesh` with `csi_module_settings_nvs::begin_read_only()`, which takes any
  namespace; the `beacon` opens would need the harness work F164 did for
  `mesh` (its stub's `begin()` succeeds on any namespace). Found by F164
  (#1762).
  *Premise corrected (#1762):* the `beacon` half cannot happen on a device
  today. Nothing in the sketch calls `beacon_channel::init()`, `set_enabled()`
  or `dispatch_espnow_message()` (F31), so `load_beacon_set()` and
  `load_audit_log()` never run. Every path to `ensure_x25519_keypair()` needs
  `g_enabled` or a cosign request only the undispatched receive path sets, and
  also a beacon-set entry, which only `load_beacon_set()` fills from NVS, so
  its read could not come before the namespace's first write even with the
  runtime wired. When F31 wires `init()`, route the set's and the audit log's
  reads through `csi_module_settings_nvs::begin_read_only()` (noted on F31).
  `beacon_channel.cpp` is unchanged.
  *Done (#1762), the `securacv` half:* traced from `setup()`, a first boot
  after an NVS erase reaches two read-only opens of `securacv` before
  `provision_device()`'s key store creates it: `setup_wizard::init()` and the
  key read in `nvs_load_key()` (an `NvsMainSession` the item did not name).
  `power_monitor`'s `load_nvs_state()` runs after the store, so it meets an
  absent namespace only on a boot whose provisioning stored no key. Each
  logged `nvs_open failed: NOT_FOUND` on a build that keeps Arduino's error
  log. All three now ask IDF's `nvs_open()` first, as F150 and F164 did for
  `csi` and `mesh`: the wizard and the battery read through
  `csi_module_settings_nvs::begin_read_only(prefs, "securacv")`, the key read
  through `probe_namespace(NVS_MAIN_NS)` ahead of its session. An absent
  namespace opens nothing and logs nothing, and every read is its default, as
  before; a present namespace or an NVS fault goes to Preferences as before.
  `firmware/projects/canary-wap/tests_host/test_wap_first_boot_nvs.cpp` (54
  checks) cuts the three opens and `nvs_store_key()` verbatim from the sketch
  and runs them in `setup()`'s order over the real `nvs_store.h` and a new
  `stubs/first_boot` NVS that models namespaces and counts the error line
  `Preferences::begin()` logs for each refused open: a first boot logs no line
  and the next boot loads the key it stored; a provisioned boot reads as
  before; a boot that stored no key logs nothing; an NVS fault keeps all four
  lines; source pins hold `setup()`'s order and each quiet open. Against the
  old code the first boot counts two lines and the no-key boot three. That
  IDF's `nvs_open()` answers NOT_FOUND without an error-level line is from
  IDF's source as read (F125). Bench row: "canary-wap first boot after an NVS
  erase: the securacv namespace (F201)". Host-tested; the ESP32 compiles are
  CI's; not bench-tested (U1). Found here: F220.
- [x] **F220 [code] canary-wap's first boot after an NVS erase may log
  `nvs_open failed: NOT_FOUND` for the `chirp` namespace.**
  `chirp_channel.cpp`'s `load_settings()` (from `chirp_channel::init()` in
  `setup()`) reads `chirp_relay` and `chirp_filter` through `nvs_store.h`'s
  `nvs_get_u8()`, each a read-only `NvsSession` on `chirp`, before anything
  writes it (only `save_settings()` and the self-test's `nvs_set_u32()` do),
  and the loop's self-test cadence reads `st_chirp_at` the same way until the
  first stamp is stored; on a build that keeps Arduino's error log each
  refused open logs a line. Route them through the quiet probe (in
  `NvsSession`'s read-only open or the convenience readers), with a harness
  that counts the line, as F201's `stubs/first_boot` does. From reading, not
  counted. Found by F201 (#1762).
  *Done (#1762):* the premise held, counted: on a first boot after an NVS
  erase, `load_settings()` (two `nvs_get_u8()`) and the loop's first-pass
  stamp read (`nvs_get_u32("st_chirp_at")`) open `chirp` read-only before
  anything writes it: three refused opens, three `nvs_open failed: NOT_FOUND`
  lines on a build that keeps Arduino's error log, and the same on every boot
  until the clock was set or the owner changed a Chirp setting. `NvsSession`'s
  read-only open (`nvs_store.h`) now goes through
  `csi_module_settings_nvs::begin_read_only()` (F150, F201): IDF's
  `nvs_open()` first; an absent namespace opens nothing, logs nothing and
  every read is a miss; a present namespace or an NVS fault goes to
  Preferences as before. The fix is in the session, so every read-only reader
  of `chirp` is covered and `canary_wap.ino` is unchanged.
  `test_wap_first_boot_nvs.cpp` (93 checks, 39 new) cuts `load_settings()`,
  `save_settings()` and the stamp's block verbatim and runs them over the real
  `nvs_store.h`: a first boot and a second clockless boot open nothing and log
  no line; after the stamp the next boot opens three, logs none and reads it;
  an NVS fault keeps its three lines. With the plain open put back the first
  boot counts three lines. `stubs/nvs_store` gains the `nvs.h` the header now
  reaches (it models no empty namespace, from the review). That IDF's
  `nvs_open()` answers NOT_FOUND without an error-level line is from IDF's
  source as read (F125). Bench row "canary-wap first boot after an NVS erase:
  the chirp namespace (F220)". Host-tested; the ESP32 compiles are CI's; not
  bench-tested (U1). Found here, by the review: F236 (spec §11.2's NVS table).
- [ ] **F236 [code] Chirp spec §11.2's NVS table is not the canary-wap's.** It
  names `chirp_enabled`, `chirp_notify_sound` and `chirp_urgency_filter`,
  which nothing stores, and leaves out `chirp_filter` and the self-test stamp
  `st_chirp_at`; the canary-wap stores `chirp_relay`, `chirp_filter` and
  `st_chirp_at` in namespace `chirp`. Give the namespace and the keys as
  stored. From code and the spec; pre-existing. Found by F220's review
  (#1762).
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
  *Found by F95 (#1762):* the AUTH exchange's fix must also change its
  payloads, not only its key agreement. `AUTH_RESPONSE` (160 B) makes a 262 B
  signed frame against ESP-NOW's 250 B, so `send_to_peer` refuses it
  (host-probed, and pinned by `test_mesh_liveness_wap`'s
  `no_session_opens_and_a_removal_splits_the_opera`). Two more for the crypto
  review, from code: `handle_auth_challenge` derives a session for the member
  whose key the payload names, not for the envelope's signer, and
  `handle_auth_response` never checks `challenge_sig`; its `opera_proof` is a
  fixed signature over the `opera_id`, the same in every exchange. Until this
  lands, a canary-wap removal splits the opera (F95, F114).
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
  *Done (#1762):* the PIO session takes a member's opera frame only from the
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
  *Done (#1762):* each member's send counter is reserved ahead in NVS, the PIO
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
  `remove_peer` reached the send path from the REST handler's task; since F96
  (#1762) it runs on the loop task, and the handle stays.
  Counting stays per member (one counter per sender is F72's option). A rekey
  reset the counters to 1, below the stored reservation and under a new opera
  key; since F95's counter fix (#1762) a rotation keeps every counter.
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
  harness). Since F76 (#1762) heartbeats go to every member whatever its
  state, so they no longer skip it. Since F71 (#1762) a boot resumes every
  member one past the highest send-counter reservation stored for any of them,
  so a reboot levels the per-destination counters and the gap restarts from
  nothing. Between boots it grows with the traffic one member gets and another
  does not, as before. Since #1761 only a radio copying the member's own
  address can deliver such a frame (ESP-NOW does not authenticate a source).
  It is dispatched and pushes the receiver's last-seen ahead, which silences
  the member until its counter catches up (host-probed). A host probe (#1762):
  one frame replayed across members cost the member one genuine frame after a
  reboot and two with no reboot, where resuming each member from its own
  reservation (F71's first cut) cost up to a block (1025). Options:
  - put the destination fingerprint in the signed bytes (a wire change under
    spec §4.5's registry, with F48);
  - use one outbound counter per sender, as on the PIO tree. That makes any
    unheard frame fresh at every member instead.
  See THREAT_MODEL "Still open on canary-wap". Found by the canary-wap half
  of the F49 part 3 withdrawal (#1761).
  *Since F99 (#1762):* a canary-wap device starts a new member one past the
  highest counter it can have signed, so a pairing in a steady opera leaves the
  new member level with the busiest member (pinned by `test_mesh_liveness_wap`'s
  `a_new_member_starts_level_with_the_busiest_member`; F99's first version
  started at the highest reservation, a block further ahead, and one replayed
  frame cost a member 984 heartbeats). A boot still levels the counters, but a
  re-pair between boots can start the gap wide: after a busy member is removed
  and re-paired, its counter starts as far ahead of the quieter members' as its
  traffic was, until the next boot. Host-probed on #1762: A
  sent B 3072 frames and C 5, removed B (C re-paired after the F95 split), and
  re-paired B; A's next counters were 3073 to B and 9 to C, so one A-to-B frame
  replayed at C from A's address drops A's next 3065 frames at C, about 25
  hours of heartbeats. One counter per sender or a destination in the signed
  bytes closes it.
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
  *Done (#1762):* the partner is now added first. The initiator's
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
  *Done (#1762):* `ensure_broadcast_peer()` registers FF:FF:FF:FF:FF:FF with
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
  *Done (#1762):* the initiator keeps a verified CONFIRM from the pairing
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
  *Done (#1762):* `update()` sends the heartbeat in MESH_CONNECTING as well as
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
  gap. Found by F75's review (#1762).
  *Correction (#1762):* the PIO side is no longer only read from code. It is
  host-probed there too, the same on wave 9's code and after F97: the
  initiator's own CONFIRM, fed back to it from the joiner's address, returns
  SEND_COMPLETE and PAIRED, and the joiner, whose owner has not confirmed,
  drops that COMPLETE (pinned since F97 by
  `the_joiner_takes_a_complete_only_after_its_owner_confirms`, which since the
  F97 review also delivers the CONFIRM sent in front of it). Spec §5.2 and
  THREAT_MODEL say so. Still open.
- [ ] **F115 [code+decision] The 6-digit pairing code has no commitment, so a
  relay that swaps both ephemeral keys can grind it.** Nothing commits either
  side's ephemeral before the other side's is sent. The relay answers the
  joiner first with its own OFFER (the joiner shows its code and stops its
  DISCOVER), DISCOVERs the initiator as a joiner, and searches ephemerals
  against the initiator's OFFER until the initiator's code equals the
  joiner's; it then opens the COMPLETE and holds the `opera_secret`.
  Host-probed on canary-wap's real handlers in F98-F100's review and rerun on
  its branch: about 1.5 million X25519 tries, under 3 minutes on one host
  core, and the search splits across cores, against the 2-minute pairing
  timeout; both screens showed 312463, both owners confirmed, and the relay
  read A's `opera_secret`. The relay presented both devices' own long-term
  keys, so this is not F69's key substitution. Fix with a commitment to one
  side's ephemeral before it sees the other's (the joiner's DISCOVER could
  carry a hash of its ephemeral, revealed in its ACCEPT; Bluetooth's numeric
  comparison commits to a nonce that way) or a much longer code, together with
  F69's transcript binding: a wire change on both trees (F48). The PlatformIO
  tree derives its code the same way, with OFFER then ACCEPT in the same order
  (read from code, not probed). The threat model's claim that a swapped
  ephemeral shows different codes "by construction" and spec §11.1 item 5
  were corrected in #1762. Found by F98-F100's review (#1762).
- [~] **F95 [code] canary-wap never opens an AUTH session, so removing a
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
  counters until they climb back. Found while fixing F71-F76 (#1762).
  *Partly done (#1762); the main item stays open and needs F48 first.*
  Starting the AUTH exchange does not fix it as the exchange stands
  (host-probed, and pinned by `test_mesh_liveness_wap`'s
  `no_session_opens_and_a_removal_splits_the_opera`, which fails the day it
  stops being true):
  - the `AUTH_RESPONSE` payload is 160 B, so its signed frame is 262 B against
    the 250 B an ESP-NOW frame carries, and `send_to_peer` refuses it. A
    challenged member marks a session the challenger never gets;
  - were it sent, the two keys would still differ: the exchange runs X25519
    over the long-term Ed25519 keys (F48's bug class), so every
    `MSG_OPERA_REKEY` would fail to decrypt and the rotation would commit at
    its 60 s timeout with the same split.
  Both are wire or derivation changes, F48's (added there). Until then a
  removal moves the remover alone to a new `opera_id`; every survivor keeps the
  old one, keeps trusting the removed device, and must re-pair with the
  remover. Spec §3.1 and §5.6 and the threat model now say so (the §5.6 caveat
  does not hold on canary-wap). Whether to rotate at all meanwhile is F114.
  The related counter gap is fixed: a rotation no longer resets any counter
  (tx 1, rx 0, on both sides), as the PIO tree's rotation keeps its own. The
  reset bought nothing (frames are signed with the long-term key, and the
  `opera_id` in the signed bytes kills a frame from before the rotation) and
  cost two things. A survivor the rotation did not reach (every one, today)
  dropped the remover's frames after the re-pair that rejoined it, until they
  climbed back (host-probed). And the last-seen reset was in RAM only, so a
  reboot before the 5-minute `replay_ctrs` save restored the old value with
  the same effect. Pinned by
  `a_survivor_re_paired_after_a_rotation_hears_the_remover_at_once` and
  `a_rotation_that_reaches_a_member_keeps_every_counter` (the test gives both
  sides one session key, standing in for a working AUTH exchange); each of the
  four resets put back alone fails one of them. This supersedes F71's Done
  text on the rekey (corrected there). Host-tested only; the Arduino compile
  is CI's; not bench-verified (U1).
- [ ] **F114 [decision] Should a canary-wap removal rotate while no survivor
  can be told?** Until F48 no member holds an AUTH session (F95), so
  `remove_peer`'s rotation reaches no one: it only moves the remover to a new
  `opera_id` and splits it from every survivor, which must re-pair with it. The
  rotation buys nothing there today: the remover drops the removed device by
  its key either way, and the survivors keep trusting it either way. The
  routine way to hit it is not a hostile device: since F98 a device that comes
  back at its old radio address with a new key (an NVS erase or a reflash) is
  refused by every member that still holds its old entry, and removing that
  entry at each of them moves each to an `opera_id` of its own, so one
  reflashed device can scatter the whole opera. Options: keep rotating (the
  §5.6 flow, ready for F48); or skip the rotation, and log it, when no
  survivor holds a session, which keeps the opera together until F48. Found by
  F95 and F98's review (#1762).
- [x] **F96 [code] canary-wap's mesh REST handlers run on the HTTP server's
  task.** `handle_mesh_*` in `canary_wap.ino` call `remove_peer`,
  `leave_opera`, `start_pairing_*`, `cancel_pairing` and `confirm_pairing`
  straight from esp_http_server's task, while `update()` reads and writes the
  same peer table and pairing state, and the one `g_prefs` NVS object, on the
  loop task. `mesh_network.cpp`'s `g_rekey` note says a serializer moves
  `remove_peer` onto the main task, but no serializer exists: the `_auth`
  wrappers only check the bearer token. Found from code; not probed, since the
  host harness is single-threaded. #1762 keeps its own new work off this path:
  F75's COMPLETE goes out from `update()`, and F71's reservation opens its own
  NVS handle (its refusal-log statics can still race, benignly). Fix: queue
  the mesh commands to the loop task. Found while fixing F71 and F75 (#1762).
  *Done (#1762):* the owner commands are internal to `mesh_network.cpp`:
  `set_enabled`, `remove_peer`, `set_opera_name`, `leave_opera`,
  `start_pairing_initiator`, `start_pairing_joiner`, `cancel_pairing`,
  `confirm_pairing` and `clear_alerts` are `static` and gone from
  `mesh_network.h`, so nothing outside the file can call them. The nine
  changing `handle_mesh_*` handlers (`pair/start`, `pair/join`,
  `pair/confirm`, `pair/cancel`, `leave`, `name`, `enable`, `remove`, alerts
  `DELETE`) validate their bodies as before and hand a `Command` to
  `mesh_network::submit()`, which posts it to a four-slot ring
  (`loop_command_ring.h`, a portMUX spinlock held only to copy a command or a
  result) and waits up to 2 s. `update()` drains the ring first on every pass,
  before its early return, so a disabled mesh (which is what a fresh device
  is) still takes `enable` and `pair/start`. A command the loop task has not
  started within the 2 s is withdrawn and never runs; one it has started is
  waited for and answered with its own result. A command that did not run
  answers with the PlatformIO tree's codes for the same two cases: 409
  `mesh_busy` (four already waiting) and 503 `mesh_timeout`. Every other answer
  is unchanged, and both web UI paths read only the body. POST /api/reboot and
  the safe-mode retry ran the pre-reboot hook on the httpd task, and it called
  `save_replay_counters()` there, touching the peer table and the one
  `g_prefs` handle. It now calls `save_replay_counters_before_reboot()`, which
  saves in place only on the loop task (the one `init()` recorded) and
  otherwise hands `MESH_CMD_SAVE_REPLAY` to `submit()`; a save the loop task
  does not start within 2 s does not run, and the last 5-minute save stands.
  The `g_rekey` note no longer promises a serializer. The same move covers the
  state F99 added in this PR: `remove_peer`'s survivor reservations are now
  written on the loop task too. Behavior changes beyond the fix: a mesh request
  sent while the device boots, before `loop()` runs, answers `mesh_timeout`;
  and `http_send_error` now takes its status line from `http_status_line.h`
  (host-tested), which adds 409 and 503 (before, every code but 400, 404 and
  500 went out as 400), so the audio self-test's 409 and the BLE chirp send's
  503 now carry their own status lines too, with the bodies unchanged. Not
  changed: `GET /api/mesh`, `/peers` and `/alerts` still read from the httpd
  task (F110).
  *Since F110 (#1762):* they read a view the loop task publishes at the end of
  every pass and after each owner command it runs.
  Pinned by `test_mesh_commands_wap.cpp` (11 tests, 192 checks) on the #1761
  mesh harness, whose stubs count every NVS write and ESP-NOW call made while
  the test plays the HTTP server's task. Each fix reverted fails it: with
  `submit()` running the command in place (the old handlers' behavior), six
  tests fail; with the drain after `update()`'s early return, three fail
  (including a pairing driven over REST from a fresh, disabled device); with
  the replay save back in place on any task, the reboot-save test fails on an
  NVS write from the HTTP task; with the busy/timeout mapping swapped, two
  fail; with the status table back to its 400/404/500 arms, three fail.
  `test_loop_command_ring.cpp` (66 checks) runs six requester threads and one
  loop thread, including a run with a one-tick timeout in which thousands of
  withdrawals race the drain, and is clean under `make tsan-loop-ring`. It
  covers post order across slot reuse and the ticket wrap, a full ring,
  results collected once, withdrawal, a started command waited for, no
  withdrawn command run, and no slot lost; six mutations of the header each
  fail it. `firmware/scripts/check_wap_loop_commands.py` (53 self-test
  mutations, run by `regression_check.sh`) holds in the source the handlers,
  the internal linkage, the drain, `submit()`, `http_send_error`'s table, the
  replay save's hand-over and F106's MQTT rules. Host-tested; the Arduino and
  PlatformIO compiles are CI's; not bench-tested (U1: the F96/F106 rows in
  `hardware_verification_checklist.md`, a dashboard reboot included). Found
  here: F110 and F111.
- [x] **F110 [code] canary-wap's mesh status routes read the loop task's state
  from the httpd task.** `GET /api/mesh`, `/api/mesh/peers` and
  `/api/mesh/alerts` still read `g_peers`, `g_pairing` and the alert history
  while `mesh_network::update()` writes them; F96 moved only the routes that
  change state and the pre-reboot replay save. The arrays are fixed and the
  history is never freed, so the worst is a torn snapshot (a member's name
  read mid-shift after a removal, a pairing code read while `cancel_pairing`
  wipes it), not a freed pointer. A read command through the same ring, or a
  snapshot the loop task publishes each pass, would make them consistent. From
  code; not probed. Found by F96 (#1762).
  *Done (#1762):* a snapshot the loop task publishes (a read command would
  have queued every page refresh behind the owner commands, with 409 or 503
  when the loop is busy). `loop_snapshot.h` (new) holds `Value<T>`, a whole
  copy of what the loop task last published (a publish of the same bytes takes
  no lock, so publishing every pass costs a compare), and `Log<E, N>`, a
  bounded log read one record per critical section, started over when the loop
  task changes it between two copies and copied under one hold after four such
  starts, so a read is never torn and always ends; the lock is
  `loop_command_ring.h`'s `PortMuxLock`. `update()` publishes a `StatusView`
  (the status, the opera's name, its enabled and has-opera flags, the pairing
  code while it is shown, and each member as the peer list shows it: name,
  fingerprint, state, RSSI, alert count, last seen; no key) at the end of
  every pass and before its early return; `run_command()` publishes after each
  owner command it runs, before the drain posts the result, so a GET right
  after a POST's answer shows what the POST did (the dashboard reloads the
  peer list right after a removal, and the opera panel after cancel, rename,
  enable and leave); and `init()` publishes the first, since the HTTP server
  starts before it. The alert history is now a `Log` that `store_alert()` and
  `clear_alerts()` change under its lock. `handle_mesh_status` and
  `handle_mesh_peers` read `mesh_network::read_status()`, and
  `handle_mesh_alerts` reads `read_alerts()` into a 3 KB heap copy (a refused
  allocation answers 500 `out_of_memory`). Every response shape is unchanged
  and the dashboard reads the same fields; uptime is still counted at the
  read. None of the three waits for the loop task, so they never answer
  `mesh_busy` or `mesh_timeout`. Cost: 760 B of internal SRAM for the view
  (the host's layout), and a 760 B compare per publish when nothing it shows
  changed. Pinned by `test_loop_snapshot.cpp` (50 checks: interleavings driven
  by a lock that runs the loop task's step between a reader's critical
  sections, and one writer against four reader threads that must see it
  mid-run; clean under `make tsan-loop-snapshot`; six header mutations each
  fail it on a multi-core host, and the two unlocked ones fail under
  ThreadSanitizer on one CPU too) and seven `test_mesh_commands_wap` tests (18
  in the suite): `a_status_read_is_the_last_published_pass`,
  `a_disabled_mesh_publishes_its_state`,
  `the_pairing_code_shows_only_while_it_is_displayed`,
  `a_status_read_counts_uptime_and_touches_nothing`,
  `the_alerts_read_is_the_history_in_storage_order`,
  `a_read_right_after_a_post_shows_what_it_did` and
  `the_peer_list_shows_each_members_heard_state`. Each piece of the wiring
  reverted alone fails one (a read of the live state, no publish at the end of
  a pass, none in `run_command()`, none in `init()`, the code never shown, the
  uptime not recounted, each of the four member fields the list shows,
  `has_opera` taken from `enabled`). The publish at `update()`'s early return
  is redundant since `run_command()` publishes each command: nothing else
  changes the view between the drain and that return, so no test fails without
  it, and only `check_wap_loop_commands.py` rule 9 holds it. Rule 9 (17
  self-test mutations; 123 in all with F96's, F106's, F111's and F112's rules)
  holds that no HTTP handler in the sketch names a live mesh reader
  (`get_status`, `get_peer`, `get_peer_count`, `get_alerts`,
  `get_opera_config`, `get_pairing_session`, `is_enabled`, `has_opera` and the
  rest), that the three routes each read the view once, that `update()`
  publishes before its every return and at its end and `run_command()` before
  its one return, that the view and the log are published, read, appended,
  cleared and reached only where they should be, and that the two readers name
  no live state; the rule sees a handler's own body, not what its callees
  read. The harness is one thread, so no host test showed a torn read on the
  old code; the evidence for consistency is the threaded snapshot test and the
  review's two-thread probe of the real `mesh_network.cpp` (torn reads on the
  base, none here). Spec §8.1 says so. Host-tested; the Arduino compile is
  CI's; not bench-tested (U1: the F110 row in
  `hardware_verification_checklist.md`). Found here: F138, with F111.
- [x] **F111 [code] canary-wap's Chirp REST handlers change chirp_channel's
  state from the httpd task.** `chirp_api.h`'s handlers call
  `chirp_channel::enable`, `disable`, `send_chirp`, `confirm_chirp`,
  `dismiss_chirp`, `mute`, `unmute`, `set_relay_enabled` and
  `set_urgency_filter` on esp_http_server's task, while
  `chirp_channel::update()` reads and writes the same cooldowns, pending and
  recent chirps and relay state on the loop task (`canary_wap.ino` calls it
  right after `mesh_network::update()`), and `chirp_channel.cpp` takes no
  lock. F96's shape fits: make the mutators internal and hand a command to the
  loop task (`loop_command_ring.h` is reusable). From code; not probed. The
  Bluetooth channel's REST handlers (`bluetooth_api.h`: `enable`, `disable`,
  `cancel_pairing`, `confirm_pairing`) were not examined for the same pattern.
  Found by F96 (#1762).
  *Done (#1762):* the Chirp and Bluetooth owner commands run on the loop task.
  Chirp: `enable`, `disable`, `send_chirp`, `confirm_chirp`, `dismiss_chirp`,
  `mute`, `unmute`, `set_relay_enabled` and `set_urgency_filter` are `static`
  in `chirp_channel.cpp` and gone from `mesh_network.h` (with the uncalled
  `deinit`, `send_all_clear` and `clear_chirps`). The nine POST handlers in
  `chirp_api.h` validate their bodies as before, hand a
  `chirp_channel::Command` to `chirp_channel::submit()` (a four-slot
  `loop_command_ring.h`, as F96 did for the mesh), wait up to 2 s, and answer
  from the `Result` the loop task read right after the command ran: the new
  session's emoji, a refused send's reason (checked in the order the handler
  always used), the relay and filter settings as they stand.
  `chirp_channel::update()` drains the ring before its disabled-channel early
  return, so enable still works on a fresh device, and the sketch's `loop()`
  calls it every pass. Bluetooth (examined, as the item asked): the four named
  handlers race, since a PIN confirm on the httpd task and `update()`'s
  pairing-timeout `cancel_pairing()` on the loop task could both find the
  pending Numeric Comparison pairing and both answer and delete it, and so do
  the other fifteen changing handlers (advertising, scans, reject, disconnect,
  the paired devices, settings, name, TX power) against `update()`'s scan
  timeout and inactivity disconnect. All nineteen now submit
  `bluetooth_channel::Command`s the same way, and the nineteen mutators (with
  the uncalled `deinit`) are internal to `bluetooth_channel.cpp`. A command
  never brings the NimBLE stack up, since its init can block past the loop
  watchdog: `enable()` no longer calls `init()`, and the enable, advertise and
  pair handlers call it on their own task first (`bring_up()`, where
  `enable()` called it before). A settings POST carries a mask of the fields
  it names, applied to the settings the loop task holds. A command that did
  not run answers 409 `chirp_busy` / `bluetooth_busy` or 503 `chirp_timeout` /
  `bluetooth_timeout` in the routes' JSON body shape; every other answer keeps
  its shape. Behavior change beyond the fix: a Chirp or Bluetooth POST sent
  while the device boots, before `loop()` runs, answers the timeout. Pinned by
  `test_chirp_commands_wap.cpp` (8 tests, 151 checks; the real
  `chirp_channel.cpp` over the mesh_net stubs, with the test's own wall clock)
  and `test_bluetooth_commands_wap.cpp` (12 tests, 235 checks; the real
  `bluetooth_channel.cpp` over a NimBLE-Arduino stand-in, `stubs/bt`, that
  records each radio, bond, passkey and NVS call with its task): no such call
  comes from the handler's side, nothing moves before the loop task's turn, a
  PIN confirm, reject or cancel answers the pending pairing once on the loop
  task (the timeout's own pass included), withdrawn, busy and post order hold
  through the real `submit()` and `update()`, and what each command carries
  reaches the channel (an urgent send's urgency, detail and TTL in the signed
  witness frame; a settings POST naming only the filter or only the relay;
  each of the nine Bluetooth settings fields alone; untrust and unblock;
  clear-all; Start Advertising and Pair turning Bluetooth on). Each fix
  reverted fails them: `submit()` running the command in place (the old
  handlers) fails 8 of 8 Chirp and 11 of 12 Bluetooth tests; the drain after
  the early return fails 7 Chirp and 3 Bluetooth tests; `enable()` calling
  `init()` again fails 1; settings applied whole fails 2.
  `firmware/scripts/check_wap_loop_commands.py` (rules C1-C4: 48 new self-test
  mutations, 53 with F112's rule M1, 123 in all with F110's rule 9; run by
  `regression_check.sh`)
  holds the internal linkage, every caller inside each .cpp, the drains,
  `submit()`, `bluetooth_channel::init(`'s two callers, and the handlers'
  side, which no host test compiles (they build their answers with
  ArduinoJson): each changing handler submits once and answers every wait but
  kDone with `send_not_run()` (409/503) right after it; it submits the command
  its route names (the ack's "confirmed" confirms, "resolved" dismisses) with
  each field that command consumes filled once from the request; the settings
  POSTs fill and name each field in its own block; nothing is answered from
  live channel state after the submit; the enable, advertise and pair handlers
  call `bring_up()` before they submit; and the sketch's `loop()` calls each
  channel's `update()` as its own top-level statement with no return before
  it. Not changed: the GET routes still read the loop task's state from the
  httpd task (F138), and the NimBLE host task's callbacks still write the
  Bluetooth state (F143). Host-tested; the Arduino and PlatformIO compiles are
  CI's; not bench-tested (U1: the F111 rows in
  `hardware_verification_checklist.md`). Found here: F138 (with F110) and
  F143-F146.
- [x] **F138 [code] canary-wap's Chirp and Bluetooth status routes read the
  loop task's state from the httpd task.** `GET /api/chirp`,
  `/api/chirp/nearby` and `/api/chirp/recent` (`chirp_api.h`, through
  `chirp_channel::get_status`, `get_recent_chirps`, `get_nearby_devices` and
  the rest) read the session, cooldowns and the recent and nearby tables while
  `chirp_channel::update()` (and `prune_old_chirps()`) rewrite them on the
  loop task, and `GET /api/bluetooth`, `/scan/results`, `/paired` and
  `/settings` (`bluetooth_api.h`: `get_status`, `get_scanned_devices`,
  `get_paired_devices`, `get_settings`) read state the loop task and the
  NimBLE host task write. The tables are allocated once (Chirp, PSRAM) or
  static (Bluetooth), so the worst is a torn snapshot, not a freed pointer.
  F111 moved only the mutators; F110's shape fits the reads (a view the loop
  task publishes, `loop_snapshot.h`). From code; not probed. Found by F110 and
  by F111 (#1762), which each filed it; merged here.
  *Done (#1762), both halves:* its Chirp half and its Bluetooth half landed
  separately in this wave and are merged here; both lean on F110's
  `loop_snapshot.h` and both extend `check_wap_loop_commands.py`. Every GET
  route of the two channels reads a view the loop task publishes, never the
  live state, and never waits for the loop task. Chirp: `GET /api/chirp`,
  `/nearby` and `/recent` read `chirp_channel.cpp`'s published `StatusView`,
  `NearbyTable` and `RecentTable` (no key or signature). The status is
  published at the end of every `update()` pass (also before its
  disabled-channel return), after each owner command in `run_command()`,
  before the drain posts the result, so a read right after a POST's answer
  shows it, and from `init()`; the tables are rebuilt only when a chirp frame,
  the 30-second prune, an owner command or `init()` changed them, so an idle
  pass builds no table and reads no PSRAM (`publish_view()` clears the flag
  once, right after its guard). `read_status()` counts the cooldown and mute
  left, the presence requirement, the wall clock, night mode and `can_send` at
  the read, as the live readers did. The two table copies (1284 + 836 bytes)
  and the 1284-byte scratch the next one is built in are one 3404-byte block
  that `init()` allocates with `csi_large_calloc()` beside the tables they
  copy (PSRAM where the board has it), held through
  `loop_snapshot::AttachedValue`, new: `Value`'s publish and whole read with
  the bytes in storage the loop task attaches and the lock on-die. So the view
  adds about 0.1 KB of internal DRAM, not the 2.1 KB two static copies would
  have taken back from the PSRAM diet's budget for the BLE stack, and
  `ram_audit.yml`'s DRAM guard fails if either view symbol reaches 256 bytes
  in internal DRAM. Each nearby or recent GET copies its table to the heap and
  frees it right after the serialize (ArduinoJson 7 keeps a `const char`
  array, the copy's emoji, by pointer until then). Bluetooth: `GET
  /api/bluetooth`, `/scan/results`, `/paired` and `/settings` read a status
  view carrying the settings, the scan list and the paired list, published at
  the end of every `update()` pass (before its disabled return too) and after
  each owner command, before the drain posts the result. `read_status()`
  counts the advertising and connected times to the read; before the first
  pass the readers answer the boot state (`kDefaultSettings`, which
  `g_settings` now starts from). The live readers are gone from
  `bluetooth_channel.h`. The Bluetooth views are static `Value` copies, about
  1.5 KB of internal SRAM (F177). Every route of both channels answers the
  keys it always did; beyond F146's `clock_unsynced`, a nearby device's emoji
  is at most 30 bytes in the view (a 31-byte one was read past its field). A
  scratch harness (ArduinoJson is not in the repo) built the four Bluetooth
  handlers over ArduinoJson 7.4.1 against the base and the new sources and
  found identical JSON; built before the F143 review's fix, it showed the
  connection and paired names printed backwards. Pinned by
  `test_chirp_commands_wap.cpp` (20 tests, 598 checks; 12 new, over real
  presence, witness and confirmation frames, among them
  `an_idle_pass_does_not_rebuild_the_tables`), `test_loop_snapshot.cpp`
  (`AttachedValue`: four threads read whole, clean under TSAN) and five
  `test_bluetooth_commands_wap.cpp` tests
  (`a_route_reads_the_last_published_pass`,
  `a_read_right_after_a_post_shows_what_it_did`,
  `reads_before_the_first_pass_show_the_boot_state`,
  `the_status_read_counts_the_times_to_now`,
  `each_view_field_reaches_the_route`), plus the threaded Bluetooth test's
  reads while the loop task publishes. 46 of 47 single reverts of
  `chirp_channel.cpp` fail a test; the survivor, the publish at the disabled
  channel's return, is redundant (`run_command()` publishes each command), so
  only rule CV3 holds it. `check_wap_loop_commands.py` gains rules CV1-CV7 and
  BV3: no HTTP handler names a live reader, each GET handler reads its view
  once, each view is published and read in one place, every path to the Chirp
  tables marks them, each route answers exactly its old keys and (CV7, since
  `chirp_api.h` is not host-compiled) sets each key from its own field and
  frees each copy only after the serialize; BV3 also holds that the
  dashboard's Bluetooth panel reads no key its route does not send. The values
  the Bluetooth handlers set are compiled by no host test. With BV1-BV3 (48
  self-test mutations) and CV1-CV7 (46), the check now refuses 217 mutations
  in this tree, run by `regression_check.sh`. No host test shows a torn read
  on the old code; the evidence for whole copies is `test_loop_snapshot.cpp`'s
  threads. Spec §8.1 says so. Host-tested; the Arduino and PlatformIO compiles
  and the RAM audit's ELF run are CI's; not bench-tested (U1: the F138 rows of
  the Bluetooth and Chirp sections in `hardware_verification_checklist.md`,
  the Chirp row also expecting the boot log's internal heap within about 150
  bytes of a build before F138). Found here: F170 and F177.
- [x] **F143 [code] canary-wap's Bluetooth state is still written by the
  NimBLE host task.** `bluetooth_channel.cpp`'s callbacks (`onConnect`,
  `onDisconnect`, `onAuthenticationComplete`, `onPassKeyDisplay`,
  `onConfirmPassKey`, `onResult`, `onScanEnd`) run on the NimBLE host task and
  write `g_connection`, `g_pairing`, `g_paired_devices` (and save them to
  NVS), `g_scanned_devices`, `g_scanning` and `g_state`, and `onDisconnect`
  calls `start_advertising()`, while the loop task's `update()` and its
  commands read and write the same with no lock. F111 moved only the httpd
  task off this state. The sharpest case: `onConfirmPassKey` deletes and
  replaces `g_pending_pair_info` while the loop task's pairing timeout or a
  confirm command may be answering and deleting it. An atomic exchange of the
  pending pointer (one taker deletes it), or callbacks that post events for
  `update()` to apply, would close it. From code; not probed. Found by F111
  (#1762).
  *Done (#1762):* the item's second remedy, callbacks that post events for
  `update()` to apply. The NimBLE host task's callbacks (`onConnect`,
  `onDisconnect`, `onAuthenticationComplete`, `onPassKeyDisplay`,
  `onConfirmPassKey`, `onResult`, `onScanEnd`, and the GATT
  `onWrite`/`onRead`, which wrote the link's activity) name none of the
  channel's state: each builds an `Event` from its own arguments and
  `millis()` and posts it to a bounded FIFO (`loop_event_queue.h`, new), whose
  portMUX spinlock is held only to copy one event in or out. `update()`
  applies the events first on every pass: before the owner's commands, so a
  PIN confirm finds the passkey the stack just asked about, and before its
  disabled early return, so a link that ends after Bluetooth is off still
  ends. The state, the paired list and its NVS save, the scan list, the
  advertising restart, the connection-parameter and PHY requests, the presence
  sensor's calls and the health-log lines are all the loop task's. The pending
  Numeric-Comparison copy travels in its event, and each answer is tied to its
  link (the review's blocking finding): `apply_disconnect()` lets the copy of
  a link that ended go with no answer, and the owner's confirm, the wrong-PIN
  no, reject, cancel and the pairing timeout all answer through
  `answer_pending_pairing()`, which injects only when the stack's own record
  of the handle (`getPeerInfoByHandle`) still names the same peer. So a lost
  disconnect event or a reused handle never takes another phone's yes. The
  link event carries the stack's `ble_addr_t` whole: the first version rebuilt
  the address through NimBLE-Arduino 2.x's byte-array constructor, which
  reverses the bytes, so names printed backwards, and the host stand-in, whose
  constructor did not reverse, hid it (it now reverses as 2.5.0's does). The
  queue holds 24 events; scan results and GATT activity post only while fewer
  than 16 wait, so a burst never takes the room kept for a link's own events.
  Drops are counted by kind: a link's log the warning `BLE link events dropped
  (queue full)`, the others a debug line, each at most once a minute. A
  passkey to confirm that finds no room is answered no on the NimBLE task, so
  that pairing fails closed. Behavior changes beyond the fix: what a callback
  reports takes effect on the next loop pass; a link that ends after Bluetooth
  was turned off leaves the state disabled (it read idle); a scan's end is
  applied once. The queue takes about 1.4 KB of internal SRAM. Scope: the
  link, passkey and bond events reach the channel only where its server
  callbacks are installed, the DEV profile; on the default FULL profile
  `ble_opera::init()` replaces them on the shared server (F171; since
  F171, #1762, wave 13, they reach it on every profile). The GATT
  activity and scan events are the channel's on both profiles. Pinned by
  `test_loop_event_queue.cpp` (106 checks; three producers and a consumer on
  real threads, clean under `make tsan-loop-events`) and
  `test_bluetooth_commands_wap.cpp` (38 tests, 663 checks in this tree), among
  them `the_pending_pairing_has_one_owner`, `a_pairing_ends_with_its_link`,
  `a_reused_handle_never_takes_the_old_yes`,
  `an_answer_goes_only_to_its_own_link`, `a_link_is_named_by_its_address`,
  `gatt_activity_never_takes_the_links_room`, `a_busy_room_is_no_warning`, one
  test per field family of the events, and
  `threads_callbacks_loop_and_commands`: the NimBLE host task, the loop task
  and the HTTP server's on three threads, clean under `make tsan-bt-commands`
  (local, not CI's), which draws ThreadSanitizer race reports with the
  callbacks applying their events in place. 41 mutations of the channel and
  the queue each fail the suite or the static check.
  `check_wap_loop_commands.py` rule BV2 (23 self-test mutations) holds that
  the callbacks name no file global but the queue and the data hook and call
  nothing of the file but their helpers, that GATT activity and scan results
  post with `EVENT_LOSSY_LIMIT` and a link's own callbacks never do, and that
  `update()` consumes the events once, before its first return and the command
  drain. Not changed: `init()` still writes the channel's state off the loop
  task (F167). Host-tested; the Arduino and PlatformIO compiles are CI's; not
  bench-tested (U1: the F143 row in `hardware_verification_checklist.md`, on a
  DEV-profile build). Found here: F167-F169; by its review: F171-F173.
- [x] **F144 [code] `POST /api/bluetooth/settings` never turns Bluetooth on or
  off.** `bluetooth_channel::set_settings()` assigns `g_settings` and then
  tests `g_settings.enabled && !is_enabled()` and
  `!g_settings.enabled && is_enabled()`, but `is_enabled()` reads the
  `g_settings.enabled` just assigned, so both branches are dead:
  `enabled:false` is stored and advertising, scans and links keep running
  until the next boot (and `enabled:true` does not enable). Pre-existing; F111
  kept the behavior (the command applies the named fields and calls the same
  `set_settings()`). From code; the Bluetooth host harness can now pin a fix.
  Found by F111 (#1762).
  *Done (#1762):* the premise holds. `set_settings()` now reads `was_enabled`
  before it assigns the new settings, and from it turns Bluetooth off as
  `BT_CMD_DISABLE` does (a pairing in progress ended, advertising and a user
  scan stopped, a link dropped, the state disabled, the setting saved) and on
  as `BT_CMD_ENABLE` does (the state idle from disabled, the setting saved;
  advertising starts with Start Advertising, as after an enable). F111's rule
  holds: no command brings the NimBLE stack up. When the POST says `"enabled":
  true`, `handle_bluetooth_settings_set` brings the stack up on its own task
  first (`bring_up()`, as the enable handler does), `run_command()` refuses a
  settings command that would turn Bluetooth on while `init()` has not run
  (`BT_REFUSED_NOT_ENABLED`, none of its fields applied), and the handler
  answers a failed bring-up or that refusal with the init error (before, such
  a POST saved `enabled: true` and answered success). From the F143 review:
  turning Bluetooth off (POST /disable or the settings' `enabled: false`) ends
  a pairing first, answering a Numeric Comparison awaiting the owner no while
  its link is still up; before, it stayed pending and on show. The dashboard
  never sends `enabled` to this route (its switch uses /enable and /disable),
  so the fix reaches API clients. Dropping a link and ending a phone's pairing
  hold where the channel's server callbacks run, the DEV profile (F171; every
  profile since F171, #1762, wave 13).
  Pinned by `test_bluetooth_commands_wap.cpp`'s
  `settings_enabled_false_turns_bluetooth_off` and
  `settings_enabled_true_turns_bluetooth_on_as_enable_does` (both fail with
  the old `set_settings()` body) and `turning_bluetooth_off_ends_a_pairing`,
  and by `check_wap_loop_commands.py` rule BV1 (5 self-test mutations) on the
  handler's side, which no host test compiles: the bring-up before the
  submit, the answer to the refusal, and `set_settings()` deciding from
  `was_enabled`, never `is_enabled()`. Host-tested; the Arduino and PlatformIO
  compiles are CI's; not bench-tested (U1: the F144 row in
  `hardware_verification_checklist.md`).
- [ ] **F145 [decision] canary-wap's fresh-device Bluetooth TX power is +3
  dBm, not the +9 its settings comment promises.** `bluetooth_channel.cpp`'s
  `g_settings` initializer sets `.tx_power = 9` with a comment that +9 is the
  power devices always ran at, but `load_settings()` reads
  `nvs->getChar("bt_tx_pwr", 3)`, so on a device with no stored key the boot
  sets +3. Decide which is meant, make both say it, and pin it
  (`test_bluetooth_commands_wap.cpp` can). Found by F111's Bluetooth harness
  (#1762).
- [x] **F167 [code] canary-wap's Bluetooth `init()` still writes the channel's
  state off the loop task.** `init()` runs on the BLE bring-up worker or on an
  HTTP handler's task (`bring_up()`, F111), and there loads the saved settings
  and paired list into `g_settings` and `g_paired_devices`, sets the state,
  writes `g_init_fail_reason` (which GET /api/bluetooth reads live), and with
  auto-advertise on calls `enable()` and `start_advertising()`, while the loop
  task's `update()` drains commands, applies the NimBLE events (F143) and
  publishes the views (F138) from the same state; `g_initialized` is a plain
  bool. A loaded result handed to the loop task (or `init()` run as a
  command's follow-up once the stack is up) would close it. From code; not
  probed. Found by F143 (#1762).
  *Done (#1762):* the premise held. The saved settings and the paired list are
  now the loop task's from its first pass: `load_saved()`, the first statement
  of `update()`, runs before any command or event, whether or not the stack
  ever comes up. `init()` (on the bring-up worker or an HTTP handler's task)
  reads the saved settings only for what the stack needs (the advertised name,
  the TX power, the PHY), creates the NimBLE objects into a hand-over
  (`g_bringup`), attaches the server-callback dispatcher
  (`ble_server_dispatch::attach()`, so a server event before the hand-over
  never meets NimBLE's defaults), publishes the hand-over with a release store
  and only then sets `g_stack_up`. The loop task takes it right after
  `load_saved()` (`adopt_init_result()`, an acquire): the pointers; the state
  from `rest_state()` (a device saved with Bluetooth off now reads `disabled`
  after the bring-up, where it read `idle`); a TX power or PHY the owner
  changed while `init()` ran, re-applied; the list's rebuild from the bond
  store; the dispatcher's pairing owner (`ble_server_dispatch::set_owner()`,
  an atomic store, not a second `NimBLEServer::setCallbacks()` from the loop
  task); and auto-advertise. The review found the first version adopting the
  saved settings whole, so a Disable, a name, a TX power or the auto-advertise
  switch sent during the bring-up (about 21 s on a device) came back undone in
  RAM and NVS. Loading on the loop task also closes the older case of a
  command run before the stack was up saving the compiled defaults over the
  stored settings. Remove and Clear all, whose bonds are NimBLE's, are refused
  until the stack is up (`Bluetooth is not up yet; nothing was removed` and
  `... cleared`). The review also found the auto-advertise running while the
  sketch's bring-up worker still registered ble_status and Opera services on
  the same server: `canary_wap.ino` now calls
  `bluetooth_channel::bringup_worker_started()` before it creates the worker
  and `bringup_worker_finished()` when the worker is done (or its create
  fails), both on the loop task, and in between every advertising start of the
  channel's (auto-advertise, pairing mode, Start Advertising, a link's end,
  Remove's restore) is held and made when the worker finishes; a stop
  withdraws it. `g_initialized` is loop-only; `is_initialized()` is the
  acquire load of `g_stack_up`; the refusal text is written whole into the
  next of four slots and published by an atomic pointer, so
  `init_fail_reason()` never reads a torn string. TSAN also found
  `ble_standard_profiles.h`'s battery level written and read across tasks; it
  is atomic now. Pinned by `test_bluetooth_commands_wap.cpp`'s
  `the_bring_up_hands_its_result_to_the_loop_task`,
  `a_refused_bring_up_publishes_its_reason_whole`,
  `commands_while_init_runs_are_kept`, `remove_and_clear_wait_for_the_stack`,
  `the_advertising_waits_for_the_bring_up_worker`,
  `the_bring_up_leaves_the_state_of_what_runs` and
  `threads_bring_up_loop_and_reads` (the worker, the loop and a reader on
  threads, `make tsan-bt-commands`): each fix reverted alone fails one of
  them, and `init()` applying its own result races under TSAN.
  `check_wap_loop_commands.py` rules BV4 (extended), BD1 (`set_owner()`) and
  the new BV7 (the hold and the sketch's two calls) hold the shapes.
  Host-tested (TSAN) over a NimBLE stand-in; the Arduino and PlatformIO
  compiles are CI's; not bench-tested (U1: the F167 rows). Found here: F210.
- [ ] **F168 [decision] A phone that asks to pair while the owner has not
  started pairing mode is refused on the next pass, by accident.**
  `onConfirmPassKey` (now its event, F143) sets the session to confirming
  without touching `started_ms`, so `update()`'s pairing timeout measures from
  the last owner-started pairing (or boot), and whenever that was 60 s or more
  ago it cancels at once, answering no on the phone's link. Pre-existing, kept
  by F143; `test_bluetooth_commands_wap.cpp`'s full-queue and security tests
  start pairing mode first for that reason. Decide whether a pairing outside
  pairing mode is refused on purpose (then refuse it by name, with a test) or
  gets its own 60 s. Since F171 (#1762, wave 13) the same holds on the FULL
  profile. Found by F143 (#1762).
- [x] **F169 [code] A dropped Bluetooth link event leaves the connection state
  stale.** F143's event queue keeps 8 of its 24 slots for a link's own events,
  but if the loop task stalls long enough for those to fill, the rest are
  dropped (logged as `BLE link events dropped (queue full)`). A dropped
  disconnect leaves `g_connection.connected` true, so advertising does not
  resume until the next link's events or a reboot. A pending pairing's answer
  is safe: it checks the stack's own record of the link. After a drop, the
  loop task could ask the stack the same way for the connection
  (`getPeerInfoByHandle`, or `ble_gap_conn_rssi()` answering
  `BLE_HS_ENOTCONN`) and apply the disconnect itself. From code; the host
  tests show only that the reserve holds a link's events through a burst of
  scan results, writes or reads. Found by F143 (#1762).
  *Done (#1762):* the item's remedy. When the count of a link's dropped events
  (`dropped_reserved()`) has moved, `update()` calls `reconcile_link()` once,
  after it applies the events and before the owner's commands: a recorded link
  the stack no longer holds on its handle, or holds for another address
  (`getPeerInfoByHandle`), is ended as its disconnect would have been (a
  warning, `BLE link gone, its end dropped: ended from the stack's record`);
  then, with no link recorded, a link the stack holds (`getPeerDevices`) is
  recorded as its connect would have been (`BLE link up, its start dropped:
  recorded from the stack's record`). A pass with no new drop asks the stack
  nothing. A disconnect now ends the record only when it names the recorded
  link (handle and over-the-air address), so a second link (F188) or a stale
  report never ends the link recorded in its place. Pinned by
  `a_dropped_disconnect_heals`,
  `a_dropped_connect_is_recorded_from_the_stack`,
  `only_the_recorded_links_end_ends_it` and, from the review (three mutants
  survived the first tests), `a_reconcile_leaves_a_live_record_alone` (the
  stand-in counts the stack calls); 7 mutations each fail the suite. Rule BV2
  lets `reconcile_link()` apply a link's start and end and holds its call to
  `update()`, once, between the consume and the drain (5 self-test mutations).
  Host-tested (`make tsan-bt-commands` is clean but does not exercise a drop);
  the compiles are CI's; not bench-tested (U1: the F169-F170 row).
- [x] **F170 [code] canary-wap's Bluetooth state and advertising flag disagree
  after a scan ends.** `stop_scan()` and a scan's end set the state idle (or
  connected) whatever it was, so when a disconnect restarted advertising
  during a scan, GET /api/bluetooth then reads `"state": "idle"` with
  `"advertising": true` (and `start_scan()` sets scanning while advertising
  goes on). Pre-existing, the same on the base and now (seen in F138's
  response comparison). Found by F138 (#1762).
  *Done (#1762):* the premise holds. `rest_state()` names what still runs:
  disabled when Bluetooth is off, then a link, a scan, pairing mode and
  advertising, in that order, else idle. `stop_scan()` (the owner's, the scan
  timeout's and a bond delete's), a scan's end, a pairing's cancel (the
  owner's and the 60 s timeout's) and a link's end set it; the starts still
  set their own. So a scan that ends while advertising reads `"state":
  "advertising"`, a scan in pairing mode ends back in `pairing`, a scan
  stopped while connected reads `connected`, and a link that ends during a
  scan reads `scanning`. On FULL, Opera's onDisconnect restarts advertising
  before the loop task applies the end, so once F171 let the channel see
  disconnects the state would have read idle beside `"advertising": true`
  after every one; it reads advertising. One existing test pinned the old idle
  after a scan and now expects advertising. Pinned by
  `the_state_after_a_scan_is_what_runs`,
  `the_state_after_pairing_or_a_link_is_what_runs`,
  `a_link_ends_on_full_into_advertising` and
  `a_remove_during_a_scan_ends_the_scan_first`; reverting each of the four
  call sites fails a test, and 6 `rest_state()` mutations each fail the suite.
  Not changed: pairing mode started with advertising off reads advertising
  (F190). Host-tested; the compiles are CI's; not bench-tested (U1: the
  F169-F170 row). Found here: F190.
- [x] **F171 [code] FULL-profile canary-wap: `ble_opera::init()` replaces
  `bluetooth_channel`'s server callbacks, so on shipping builds a phone's
  pairing is accepted with no owner confirm.** `build_config.h` makes FULL the
  default profile and sets `FEATURE_BLUETOOTH` and `FEATURE_BLE`;
  `ble_config.h` sets `FEATURE_BLE_OPERA 1`. The BLE bring-up worker runs
  `bluetooth_channel::init()`, which calls
  `setCallbacks(&g_server_callbacks)`, and then `ble_manager::init()`, which
  reaches `ble_opera::init()`: it takes the same singleton
  `NimBLEDevice::createServer()` and calls `setCallbacks(&g_serverCallbacks)`,
  and NimBLEServer keeps one pointer. `OperaServerCallbacks` overrides only
  `onConnect` and `onDisconnect`. So on FULL builds the channel never sees a
  link, a passkey or a bond, and NimBLE-Arduino 2.5.0's default
  `onConfirmPassKey` injects yes (`NimBLEServer.cpp:1154-1157`): the owner's
  on-device PIN confirm (`require_pin`) is bypassed, and MITM protection with
  it; its default `onPassKeyDisplay` returns 123456. Also dead on FULL: the
  dashboard's connection card and PIN box, the paired list, the inactivity
  timeout, POST /disconnect, and turning Bluetooth off dropping a link. The
  channel's GATT and scan callbacks still run. Fix: forward Opera's two
  callbacks to the channel's, or install one dispatcher that calls both, and
  add a host test that builds both modules' bring-up over the stand-in.
  Pre-existing; from code (`build_config.h:63, :247-248`; `ble_config.h:63`;
  `canary_wap.ino:11588, :11632`; `ble_opera.h:85-108, :259-264`; the
  NimBLE-Arduino 2.5.0 source); not probed on a device. Found by F143's review
  (#1762).
  *Done (#1762):* the premise holds: NimBLE-Arduino 2.3.8 and 2.5.0 both keep
  one `m_pServerCallbacks` per server, and both default `onConfirmPassKey` to
  yes. New `ble_server_dispatch.h`: one `Dispatcher` is the server's only
  callbacks object. `bluetooth_channel::init()` and `ble_opera::init()` hand
  theirs to `ble_server_dispatch::install()` with a role instead of calling
  `setCallbacks()`, in whichever order they run (the boot worker runs the
  channel's first; a REST handler's `bring_up()` can run it after Opera's).
  The pairing channel (`kPairing`) gets every callback the 2.3.8 floor
  declares (connect, disconnect, MTU, passkey display, Numeric Comparison,
  authentication, identity, connection parameters, PHY); Opera (`kLink`) gets
  onConnect and onDisconnect only, never the security callbacks its object
  would answer with the library's defaults. With no pairing owner (before the
  channel is up) a Numeric Comparison is answered no and the passkey shown is
  a random one, not 123456; a build without the channel pairs by Just Works
  under NimBLE's defaults, so no Numeric Comparison reaches anyone there.
  `onPassKeyEntry` is not overridden: 2.3.8 does not declare it, and the WAP's
  DISPLAY_YESNO capability never asks this side to type one. The dispatcher is
  installed with deleteCallbacks false. What the owner's confirm covers, from
  NimBLE 2.3.8's pairing tables (read, not probed): every Numeric Comparison,
  which is what a phone offering a display and a yes/no gets. A DisplayOnly or
  NoInputNoOutput initiator (and on legacy pairing a DisplayYesNo one) pairs
  by Just Works with no owner (unauthenticated: the channel neither lists the
  bond nor gives the link the authenticated characteristics, but NimBLE stores
  it), and a keyboard-only initiator by Passkey Entry with the digits the PIN
  box shows, in or out of pairing mode (F191). F171 also closed FULL's Passkey
  Entry with the library's fixed 123456, which gave anyone who knew it an
  authenticated bond. Follow-ons, because the channel now sees FULL's every
  link: the channel's connect no longer stops the shared advertiser while
  Opera is registered (`ble_server_dispatch::link_observed()`), so FULL keeps
  Opera's fleet-link beacon on the air through a phone's link (DEV still stops
  advertising while connected); from the review, only the recorded link's
  events set the connection card's security and the pairing's state (another
  link's bonded encryption had marked the recorded link bonded), and the
  inactivity timeout drops only the recorded link (it called `disconnect()`,
  which drops every link on the server, Opera's GATT clients and a BLE OTA or
  provisioning session among them). The owner's Disconnect (POST /disconnect)
  still drops every link on the server, stated and pinned. Behavior change on
  FULL beyond the fix: the owner's PIN confirm, the PIN box, the connection
  card, the paired list, the inactivity timeout, POST /disconnect and turning
  Bluetooth off dropping a link work there for the first time. Pinned by
  `test_bluetooth_commands_wap.cpp`'s
  `full_profile_the_owner_answers_every_pairing`,
  `full_profile_either_init_order`, `no_pairing_owner_fails_closed`,
  `another_links_encryption_leaves_the_record` and
  `full_profile_the_timeout_ends_the_recorded_link_only`, which build both
  inits (Opera's real header) over the stand-in, whose server callbacks are
  NimBLE-Arduino 2.5.0's with its defaults: with the old `setCallbacks()`
  calls the first fails at the library's yes, the second at Opera never
  counting a link, the third at the library's 123456; reverting the
  recorded-link check or the timeout's reach fails the last two; 18 mutations
  (12 of the dispatcher, 2 of the advertising guard, 4 of the recorded-link
  and timeout code) each fail the suite. `check_wap_loop_commands.py` rule BD1
  (12 self-test mutations) allows a `setCallbacks(` outside `install()` only
  with an object of a `NimBLECharacteristicCallbacks` class (followed through
  any number of derivations), so a server callbacks object of any derived
  class, a pointer variable and `nullptr` / `NULL` / `0` (each puts NimBLE's
  default yes back; the review found the first version passed them) are
  refused; it holds `install()` to deleteCallbacks false and each owner to one
  install with its own role, and rule C2 no longer lets the inactivity timeout
  call `disconnect()`. `docs/security/THREAT_MODEL.md` (the fail-secure
  table's pairing row, naming what asks no owner) and the hardware checklist
  (the Bluetooth rows run on FULL; a new F171 row with the timeout's reach)
  say the same; `firmware/LESSONS_LEARNED.md` has the lesson. Host-tested; the
  Arduino and PlatformIO compiles are CI's; not bench-tested (U1: the F171 row
  in `hardware_verification_checklist.md`). Found here: F187, F188 and F191.
- [x] **F172 [code] canary-wap's Bluetooth "Remove" leaves the phone's bond in
  NimBLE.** `remove_paired_device()` deletes `NimBLEAddress(address,
  addr_type)`, built from the bytes the paired list keeps. Those are the
  native order (least significant first, from `getBase()`), and NimBLE-Arduino
  2.x's byte-array constructor reverses them, so `ble_gap_unpair()` is asked
  for a different address and fails unnoticed: the phone keeps its bond,
  reconnects encrypted without pairing again (the dashboard tells the owner it
  "will need to pair again"), and `apply_auth_complete()` adds it back to the
  list. Byte order is not the whole fix: the list keeps
  `NimBLEConnInfo::getAddress()`, the over-the-air address, but NimBLE keys a
  bond by the identity address (`getIdAddress()`), which differs for a phone
  using resolvable private addresses (most do). Keep the identity address and
  delete by the stack's form (`NimBLEAddress(ble_addr_t)`). Separately,
  `format_address()` prints the native bytes least significant first, so GET
  /paired's and the connection's `address` fields read backwards against the
  phone's own address (and against `connection.name`); `parse_address()`
  round-trips that string, so DELETE /paired still finds its entry.
  Pre-existing. The host stand-in now records `deleteBond`'s argument, and
  `test_bluetooth_commands_wap.cpp` asserts only its address type, so a test
  can hold the fix when it lands. From a host probe against 2.5.0's
  constructor; not bench-tested. Found by F143's review (#1762).
  *Done (#1762):* the premise holds, and it was worse than the item said.
  NimBLE's `ble_gap_unpair()` (ble_gap.c, the same in NimBLE-Arduino 2.3.8 and
  2.5.0) answers the store's error for an address it holds no bond for, and
  for a bond that carries the peer's IRK (a phone using private addresses
  hands its IRK over) it answers `BLE_HS_EBUSY` and keeps the bond while the
  advertiser or a discovery runs, which on the WAP is nearly always (the
  presence scan never ends, advertising is on by default, Opera's beacon on
  FULL); `remove_paired_device()` and `clear_all_paired_devices()` threw the
  answer away. A link's event now carries `getIdAddress()` beside
  `getAddress()`, both as the stack's `ble_addr_t`; `apply_auth_complete()`
  keeps the identity address and its type in the paired list and finds an
  existing entry by it (a phone back on a new private address is the same
  entry, its count up). Remove deletes `NimBLEAddress(ble_addr_t)` with the
  stored type, on the loop task, with the radio quiet for the call (from the
  review): the owner's scan ends (`stop_scan()`), the presence scan pauses
  (`ble_presence::pause_for_user_scan()`), the shared advertiser stops
  (Opera's beacon with it), and both come back after; `ble_gap_unpair()` also
  ends a live link to that address. Clear-all does the same once for every
  bond. A bond the stack keeps anyway (another task restarted the advertiser
  inside the call: Opera's onConnect, a chirp) keeps its entry, nothing is
  saved, the health log says `Paired device not removed: the stack kept its
  bond`, and the command answers `BT_REFUSED_BOND_KEPT`, which DELETE
  /api/bluetooth/paired sends as `The phone's bond was not removed; try again`
  (Clear-all: `Not every phone's bond was removed; try again`); the
  dashboard's buttons ignore the message, and their reload still lists the
  phone. An entry no bond backs is removed. A list saved before is rebuilt
  once, the first time the stack is up (NVS key `bt_paired_id`, written by
  every save, marks a list of identity addresses): an entry that names a bond
  keeps its fields and takes the bond's type, a bond with no entry is added,
  an entry that names no bond is dropped, and the health log says `Paired list
  rebuilt from the bond store` with the three counts; a fresh device writes
  nothing. An old private-address entry cannot be matched to its bond (that
  needs the phone's IRK), so its trust and block flags start over.
  `format_address()` prints the stored bytes most significant first and
  `parse_address()` reads them back the same way, so GET /paired's `address`
  (and the connection's and the scan list's) reads as the phone shows its own,
  and DELETE /paired, trust and block still find the entry a GET printed;
  `parse_address()` also refuses a non-hex field. No health-log line names a
  peer's address (the review): `New device paired` had logged the phone's
  stable identity address, and it and `BLE device connected` now carry no
  detail. The stand-in models a resolvable private address over an identity, a
  bond store keyed by identity, and `deleteBond()` as `ble_gap_unpair()`
  answers; before the review it deleted every bond and answered true, which is
  how the build stage's Remove passed. Pinned by
  `remove_forgets_the_bond_by_its_identity`,
  `a_bond_the_stack_keeps_keeps_its_entry`,
  `a_remove_during_a_scan_ends_the_scan_first`,
  `full_profile_remove_brings_the_beacon_back`,
  `an_address_prints_most_significant_first_and_round_trips` and
  `an_old_paired_list_is_rebuilt_from_the_bond_store`: with the review's
  stand-in the build stage's code fails the first and the rebuild's Remove;
  reverting each part fails a test, and 12 delete, rebuild and refusal
  mutations each fail the suite. Host-tested; the compiles are CI's; not
  bench-tested (U1: the F172 row). Found here: F189.
- [x] **F173 [code] A Bluetooth link that comes up just after Bluetooth is
  turned off stays up.** `disable()` stops advertising and drops the link it
  knows of. A phone whose connection was already in flight connects anyway;
  its connect event is applied (F143), and since `update()` returns early
  while the channel is disabled, nothing drops it: no inactivity timeout and
  no advertising check. GET /api/bluetooth reads `"state": "connected"` with
  `"enabled": false` until the phone leaves. `apply_connect()` could refuse a
  link while disabled (disconnect its handle, record nothing). Pre-existing,
  the same on the base, which wrote the connection from the callback. Seen by
  F143's review in a harness probe, not on a device (#1762).
  *Done (#1762):* the premise holds (the harness showed a connect applied
  while disabled reading `"state": "connected"` with `"enabled": false`).
  `apply_connect()` refuses a link while Bluetooth is off: it drops the handle
  (`NimBLEServer::disconnect`), records nothing and logs `BLE link refused:
  Bluetooth is off`; the link's end leaves the state disabled. A Numeric
  Comparison asked while Bluetooth is off is answered no on the loop task
  (`BLE pairing refused: Bluetooth is off`), since no pairing timeout runs
  while disabled. The reach on FULL, stated after the F171 review: the channel
  sees every link there, so Bluetooth off refuses every BLE link, whatever
  service it came for (Opera's GATT clients, BLE OTA, provisioning), including
  each one Opera's advertising lets in while the channel is off (F187). Pinned
  by `a_link_while_bluetooth_is_off_is_refused` (DEV wiring) and
  `full_profile_off_refuses_every_link` (FULL wiring: Opera counts the client,
  the channel drops it); reverting the refusal or the passkey's no fails them,
  and 4 mutations of the refusal each fail the suite. Host-tested; the
  compiles are CI's; not bench-tested (U1: the F173 row). Found here: F187.
- [ ] **F177 [decision] canary-wap's Bluetooth status views sit in internal
  DRAM.** The views F138's Bluetooth half publishes (`bluetooth_channel.cpp`'s
  `g_status_view`, `g_scan_view` with up to 16 scanned devices,
  `g_paired_view` with up to 8 paired ones) are static `loop_snapshot::Value`
  copies in internal DRAM, the internal heap the PSRAM diet freed for the BLE
  stack, about 1.5 KB, and `ram_audit.yml` names none of them. Since #1762
  `loop_snapshot.h` has `AttachedValue` (the bytes in a PSRAM block the loop
  task attaches, the lock on-die), which the Chirp tables use. Decide per
  view, from its measured size, whether it moves to a PSRAM block (a paired
  view that carries key material stays on-die, as mesh `g_peers` does), and
  add any that stays static and reaches 256 bytes to the RAM audit's guard.
  Found by F138's review (#1762).
- [ ] **F187 [decision] FULL-profile canary-wap: Opera's advertising and Chirp
  ignore the Bluetooth switch.** `disable()` (POST /disable, or the settings'
  `"enabled": false`) stops the shared NimBLE advertiser, but on FULL Opera's
  onConnect and onDisconnect (`ble_opera.h`) and `ble_chirp.h`'s
  `restoreOperaAdvertising()` after every chirp (the heartbeat chirp comes
  every 5 minutes) start it again, connectable, whatever the pairing channel's
  `enabled` says. Since F173 each link that advertising lets in while
  Bluetooth is off is dropped at once, whatever service it came for (on FULL
  no BLE client can stay connected while Bluetooth is off), so a phone or
  Canary set to reconnect can loop connect-and-drop. Decide whether "Bluetooth
  off" on FULL silences Opera's discovery advertising and the chirps, or the
  web UI says BLE Discovery stays on and which services Bluetooth off refuses.
  From code and the host harness (`full_profile_off_refuses_every_link`); not
  probed. Found by F173 (#1762).
- [ ] **F188 [decision] FULL-profile canary-wap: a second BLE client can
  connect while one is linked, and the pairing channel tracks only the
  newest.** Opera re-advertises (connectable) after every connect, and since
  F171 the channel leaves that advertising on through a link on FULL (stopping
  it would take the fleet-link beacon off the air), so with NimBLE-Arduino's
  default of 3 connections a second phone, Canary or app session connects
  beside the first. The channel records only the newest link, whatever service
  it came for: the status and connection card describe that one; its
  inactivity timeout (5 minutes by default) counts only the channel's own
  characteristics' activity (Opera's, BLE OTA's, provisioning's, the console's
  and the log export's post none), so a recorded OTA, provisioning or Opera
  session idle on the channel's characteristics for that long is dropped; and
  a second phone's Numeric Comparison replaces the first's on show. Since the
  F171 review another link's encryption no longer labels the recorded one,
  only the recorded link's end clears it (F169), and the timeout drops only
  it; the owner's Disconnect drops every link. Decide whether FULL keeps one
  link (a non-connectable beacon while linked, or Opera not re-advertising) or
  the channel tracks every link and counts every service's activity. From code
  and the host harness; not probed. Found by F171 (#1762).
- [x] **F189 [code] canary-wap's paired list outgrows NimBLE's bond store.**
  `MAX_PAIRED_DEVICES` is 8, but NimBLE-Arduino's default
  `CONFIG_BT_NIMBLE_MAX_BONDS` is 3 (no build overrides it), and its
  store-full callback (`ble_store_util_status_rr`, installed by
  `NimBLEDevice::init()`) calls `ble_gap_unpair_oldest_peer()`, which goes
  through `ble_gap_unpair()`'s busy guard: a fourth phone's pairing evicts the
  oldest bond while GET /api/bluetooth/paired keeps listing it (F172's rebuild
  drops such an entry only on the first boot after the update), or, when that
  oldest bond carries an IRK while the WAP advertises or scans (nearly
  always), fails to store the new one. Raise the bond store to the list's size
  (or cap the list at it), or drop an entry when its bond is evicted, and make
  the store-full path quiet the radio as Remove does. From code (nimconfig.h,
  NimBLEDevice.cpp, ble_gap.c, 2.3.8); not probed. Found by F172 (#1762).
  *Done (#1762):* the premise held (a probe over the stand-in before the fix
  listed 4 phones over 3 bonds). NimBLE-Arduino 2.3.8 and 2.5.0 (read from
  source, not compiled here) keep `CONFIG_BT_NIMBLE_MAX_BONDS` = 3, check the
  store at a Pairing Request (counting active pairings as bonds) and again at
  the write, and their default store-status answer
  (`ble_store_util_status_rr`) unpaired the oldest bond or, when NimBLE's busy
  guard refused that, failed the new pairing. `init()` now installs the
  channel's own answer,
  `NimBLEDevice::setDeviceCallbacks(&g_store_callbacks)`:
  `BLE_STORE_EVENT_FULL` for a peer the store already holds answers 0 (its
  record is rewritten, so a known phone still re-pairs), and for anyone else
  `BLE_HS_ESTORE_CAP`, so the new phone's pairing fails and nothing is
  deleted. `BLE_STORE_EVENT_OVERFLOW` is refused the same way, a bonded
  phone's CCCD record included: NimBLE's default made room in the 8-record
  CCCD store by unpairing another listed phone's bond; now no bond goes, the
  record is not kept, and the phone's subscription write is answered with an
  ATT error (the subscription holds for that link only). Both post lossy
  events the loop task counts and logs at most once a minute (`BLE pairing
  refused: the bond store is full`, `BLE bond store full: a record was not
  kept`), and a refused pairing on the recorded link reads failed. The
  store-full path deletes nothing, so it cannot fail half-way or need the
  radio quiet. The list now fits the store by content: `apply_auth_complete()`
  lists a new phone only when `NimBLEDevice::isBonded(identity)` (else `Paired
  device not listed: the bond store did not keep it`), and every boot's
  adoption drops listed entries without a bond (F172's rebuild did that only
  on the first boot after the update). The stand-in models both stores, the
  FULL event at a Pairing Request, the OVERFLOW event at the write and
  NimBLE's default evictions. Pinned by
  `a_full_bond_store_refuses_a_new_pairing`,
  `the_store_never_evicts_a_listed_bond`,
  `the_paired_list_follows_the_bond_store_at_boot` and
  `a_full_cccd_store_evicts_no_bond` (each fails with one fix reverted) and
  rule BV5. `MAX_PAIRED_DEVICES` stays 8, the bond store 3 and the CCCD store
  8 (F207, F208). Host-tested over a NimBLE stand-in; the compiles are CI's;
  not bench-tested (U1: the F189 rows). Found here: F207-F209.
- [x] **F190 [code] canary-wap's Bluetooth pairing mode reads "advertising"
  when it has to start advertising.** `start_pairing()` sets `BT_PAIRING`,
  then its `start_advertising()` sets `BT_ADVERTISING` whenever it starts the
  advertiser (the owner had stopped it), so GET /api/bluetooth hides pairing
  mode until it ends. `start_advertising()` could set the state from
  `rest_state()` (F170), as the ends now do. From code; not probed. Found by
  F170 (#1762).
  *Done (#1762):* the premise held. `start_advertising()` now sets the state
  from `rest_state()`, not `BT_ADVERTISING`, so pairing mode started with the
  advertiser stopped (or from Bluetooth off, which Pair turns on) reads
  `pairing` in GET /api/bluetooth until it ends, then `advertising` (F170's
  order: disabled, connected, scanning, pairing, advertising, idle); Start
  Advertising during a scan reads `scanning`. Pinned by
  `test_bluetooth_commands_wap.cpp`'s
  `pairing_mode_reads_pairing_when_it_starts_advertising`, which fails with
  `BT_ADVERTISING` put back. Host-tested; the Arduino and PlatformIO compiles
  are CI's; not bench-tested (U1: the F190 row in
  `hardware_verification_checklist.md`).
- [ ] **F191 [code+decision] canary-wap Bluetooth pairs outside Numeric
  Comparison with no owner.** A DisplayOnly or NoInputNoOutput initiator (or a
  legacy DisplayYesNo one) pairs Just Works (NimBLE 2.3.8's
  `ble_sm_sc_resp_ioa` and `ble_sm_lgcy_resp_ioa` for the WAP's DISPLAY_YESNO;
  `BLE_SM_LVL` 0 and SC-only off refuse neither): the channel does not list
  the bond and its link does not get the authenticated characteristics, but
  NimBLE keeps the bond in its 3-bond store (and its store-full path acts on
  it, F189). A keyboard-only initiator gets Passkey Entry: the WAP shows the
  passkey and writes it to the health log (`Pairing PIN displayed`, the six
  digits), and neither pairing mode nor `allow_pairing` gates it, so whoever
  reads the dashboard's PIN box or the log can complete an authenticated,
  listed bond. Decide whether to refuse both, for example by deleting an
  unauthenticated bond and dropping its link in `apply_auth_complete()` (with
  the radio quiet, as Remove does), by refusing a passkey display outside
  pairing mode, and by keeping the digits out of the health log. From NimBLE
  2.3.8 source and the host harness; not probed. Found by F171's security
  review (#1762).
- [ ] **F207 [decision] canary-wap Bluetooth: NimBLE's bond store holds 3
  phones and the paired list 8.** NimBLE-Arduino 2.3.8 and 2.5.0 keep
  `CONFIG_BT_NIMBLE_MAX_BONDS` = 3 (the nimconfig.h default; no build
  overrides it) while `MAX_PAIRED_DEVICES` is 8. Since F189 a fourth phone is
  refused while three bonds are held (it used to evict the oldest), so five of
  the list's slots can never fill and the dashboard promises a list it cannot
  keep. A `#define` in the sketch does not reach the library's compile (the
  Arduino IDE builds libraries separately; a PlatformIO `build_flags` -D
  does), and each bond costs store RAM and NVS. Decide: raise it through build
  flags on the PlatformIO envs (and say what an Arduino IDE build keeps), or
  cap the list and the dashboard at 3. From NimBLE source and the host
  harness; not probed. Found by F189 (#1762).
- [ ] **F208 [decision] canary-wap Bluetooth: NimBLE's CCCD store may be too
  small for three bonded phones' subscriptions.** `CONFIG_BT_NIMBLE_MAX_CCCDS`
  keeps 8 records (the nimconfig.h default in NimBLE-Arduino 2.3.8 and 2.5.0;
  no build overrides it). NimBLE persists one per bonded phone per
  characteristic it subscribes to, and the sketch has 12
  `NIMBLE_PROPERTY::NOTIFY` sites, so three bonded phones subscribing to three
  each make nine. Since F189 a full CCCD store refuses the record: the phone's
  subscription write is answered with an ATT error and the subscription holds
  for that link only (NimBLE's default unpaired another phone's bond instead).
  Decide: raise it with the bond store (F207) through PlatformIO build flags,
  or keep the refusal and say in the dashboard that a phone's subscriptions
  may not survive a reconnect. From NimBLE source and the host harness; not
  probed. Found by F189's review (#1762).
- [ ] **F209 [code+decision] canary-wap Bluetooth: unlisted Just Works bonds
  can fill the bond store and lock the owner out.** A DisplayOnly or
  NoInputNoOutput phone pairs by Just Works with no owner asked (F191), and
  NimBLE stores the bond though the channel never lists it. Before F189 such
  bonds pushed the owner's bond out of the 3-bond store; since F189 three of
  them fill it, and the owner's next pairing is refused until Clear all
  (`DELETE /api/bluetooth/paired/all`, which deletes every bond the stack
  keeps, listed or not). Remove cannot reach them (it takes a listed address),
  and the dashboard does not show them. Fix with F191 (delete an
  unauthenticated bond and drop its link in `apply_auth_complete()`, the radio
  quiet as Remove does), or let a full store evict only a bond the list does
  not hold, or show the store's count beside the list. From code and the host
  harness; not probed. Found by F189 (#1762).
- [x] **F210 [code] canary-wap Bluetooth: `is_enabled()` is read off the loop
  task.** `bluetooth_api.h`'s Start Advertising and Pair handlers call
  `bluetooth_channel::is_enabled()`, a plain read of the loop task's
  `g_settings.enabled`, on the HTTP server's task, while the loop task's
  commands (and, since F167, its first pass's `load_saved()`) write it: a data
  race by the C++ memory model, which TSAN has not reported because the
  threaded test does not read it. The handlers use it only to decide whether
  to call `bring_up()`, whose own test is the atomic `g_stack_up`, so a stale
  read costs a redundant `bring_up()` or a refusal the owner can retry. Read
  it from the published status view (`loop_snapshot`, F138) or store it
  atomically. From code; not probed. Found by F167 (#1762).
  *Done (#1762):* the premise held. With the handlers' read put into the
  threaded test as `is_enabled()`, ThreadSanitizer reports a race against the
  loop task's first-pass `load_saved()`, 5 runs of 5. The Start Advertising
  and Pair handlers now read the new `bluetooth_channel::read_enabled()`, the
  settings the last pass published (`read_settings().enabled`, F138's view).
  `is_enabled()` is `static` in `bluetooth_channel.cpp`, called only by the
  loop task's `run_command()`, and gone from the header. A read a pass stale
  costs a `bring_up()` that finds the stack up, or a refusal the owner
  retries; before the first pass both reads answer the boot default.
  `test_bluetooth_commands_wap.cpp`'s new
  `the_handlers_read_enabled_from_the_view` holds the boot state, the saved
  "off" after the first pass, the old value in the middle of a command and the
  command's value after its pass; it fails with `read_enabled()` put back to
  the live flag. `make tsan-bt-commands` is clean; with the live read, the
  threaded test run alone (`./test_bluetooth_commands_wap_tsan
  threads_bring_up_loop_and_reads`) reports the race, 5 of 5. That target is
  local: CI holds F210 through the single-thread test and rule BV8. BV8 in
  `check_wap_loop_commands.py` pins `read_enabled()` to `return
  read_settings().enabled;` and `is_enabled()` to one `static` definition
  called only from `run_command()`; BV3 and C3 name the new reader (8 new
  mutations, 326 refused on the integrated tree, where the suite runs 71 tests
  and 1356 checks). Host-tested over a NimBLE stand-in; the Arduino and
  PlatformIO compiles are CI's; not bench-tested (behavior unchanged, no bench
  row). Found here, by the review: F235 (a refused boot bring-up is never
  retried through these two handlers while the setting says on; not caused by
  this change).
- [ ] **F235 [code] canary-wap Bluetooth: Start Advertising and Pair never
  bring the stack up while the setting says on.** `bluetooth_api.h`'s two
  handlers call `bring_up()` only when `read_enabled()` (before F210,
  `is_enabled()`) says off. On a device whose boot bring-up failed or the heap
  guard refused, with the saved setting on (the default), each tap answers a
  failed start (ok false, no refusal), the stack stays down, and a retry
  answers the same; only POST `/api/bluetooth/enable`, or a settings POST
  naming `enabled`, brings it up. `check_wap_loop_commands.py`'s C3 says the
  guard exists so that a device whose boot bring-up failed can still turn
  Bluetooth on, which holds only for a device saved off. Call `bring_up()`
  whenever `is_initialized()` is false (answering "initializing" while the
  worker runs), or answer the refusal by name, and correct C3's wording. From
  a host probe on the real `bluetooth_channel.cpp`, at #1762's base and head.
  Found by F210's review (#1762).
- [x] **F146 [code] A canary-wap Chirp send refused for an unsynced clock
  answers `cooldown`.** `can_send_chirp()` is false both in the cooldown and
  while `time()` is below `MIN_UNIX_TIME`, and the send's refusal (checked in
  the handler's long-standing order, kept by F111) names the second case
  `cooldown` with 0 seconds left. A refusal of its own (`clock_unsynced`)
  would tell the owner to wait for time sync, not for a cooldown.
  Pre-existing; `test_chirp_commands_wap.cpp`'s `a_refused_send_names_why`
  pins today's answer. Found by F111 (#1762).
  *Done (#1762):* the premise holds, and the status route and the dashboard
  told the same story. `run_command()`'s send refusal now splits where
  `can_send_chirp()` does: after the channel off and the presence requirement,
  the cooldown (with its time left), then a wall clock not set yet
  (`SEND_REFUSED_CLOCK_UNSYNCED`), then night mode, still the handler's
  long-standing order. `POST /api/chirp/send` answers each refusal from
  `send_refusal_error()` and `send_refusal_message()` (`mesh_network.h`), so a
  send before the clock is set answers
  `{"success":false,"error":"clock_unsynced","message":"Waiting for the clock
  to be set from GPS time before sending"}` (GPS is the sketch's one clock
  source: it has no SNTP). The other four answers keep their strings and
  shape, and only the cooldown carries `cooldown_remaining_sec` and
  `cooldown_tier`. `GET /api/chirp` named no reason for the case, and the
  dashboard's Chirp card said Ready with Send on: `cannot_send_reason` now
  names `clock_unsynced` after `disabled`, `cooldown` and `presence_required`
  (`chirp_channel::cannot_send_reason()`), and the card
  (`WebUiLogic.chirpSendGate()` in `web_ui.h`; `web_assets_gz.h` regenerated)
  says "Waiting for GPS time..." with Send off. Pinned by
  `test_chirp_commands_wap.cpp`'s `a_refused_send_names_why`,
  `a_status_read_counts_time_at_the_read` and
  `cannot_send_reason_names_the_clock`, and `web_ui_logic.test.js`'s
  `chirpSendGate` suite (4 tests); each piece reverted alone fails one.
  `chirp_api.h` is not host-compiled (ArduinoJson is not on the host), so
  `check_wap_loop_commands.py` rule CV7 holds the handlers' use of the
  lookups: the send's answer is exactly `CHIRP_SEND_ANSWER` (error and message
  from the two lookups, the cooldown's fields only for a cooldown), and the
  status route's `cannot_send_reason` comes only from one
  `cannot_send_reason(v)`. Spec §8.1 lists the refusals in order. Not closed,
  pre-existing (the review's probes): the cooldown is a state, so a send
  drained in the pass after a cooldown ran out, or in its last second, still
  answers `cooldown` with 0 s and the card says Ready for it, and a mute
  during a cooldown ends it (F178). Host-tested and statically held; the
  Arduino compile is CI's; not bench-tested (U1: the F146 row in
  `hardware_verification_checklist.md`). Found here: F174-F176, and by its
  review F178.
- [x] **F174 [code] canary-wap `POST /api/chirp/confirm` answers `not_found`
  for a confirmation refused for the presence requirement or an unset clock.**
  `confirm_chirp()` returns false for all of them (presence not met, clock not
  set, no such chirp, the device's own chirp), and `handle_chirp_confirm`
  answers each `{"error":"not_found","message":"Chirp not found or already
  dismissed"}`; `/api/chirp/ack` with "confirmed" answers a bare
  `success:false`. A dismiss marks the chirp locally whatever the clock, but
  sends its signed suppress vote only with presence met and the clock set, and
  says nothing when it does not. F146's shape fits: a refusal in the `Result`,
  named by the handler. Found by F146 (#1762).
  *Done (#1762):* the premise holds: a scratch harness of the real handlers
  over ArduinoJson 7.4.1 (the library is not in the repo) answered every
  refused confirm `200` `not_found` and the ack's "confirmed" a bare `200
  {"success":false}`, and showed worse: the confirm refusal's 85 bytes went
  out of a 64-byte buffer, which `serializeJson()` fills without a terminator,
  so `httpd_resp_sendstr()` sent cut JSON and the stack bytes after it (the
  mute route's 101-byte `invalid_duration` refusal the same).
  `confirm_chirp()` reports why it refused into the command `Result` (a
  `ConfirmRefusal`), in the order it checks: the channel off (new, first, as
  for a send), the presence requirement, the wall clock, then the chirp (none
  with that nonce, or this device's own). `send_confirm_answer()` in
  `chirp_api.h` answers each from host-tested lookups in `mesh_network.h`:
  `chirp_disabled`, `presence_required`, `clock_unsynced` and `own_chirp` as
  409, `not_found` as 404, never 403 (the dashboard reads a 403 as a bad
  token); `/api/chirp/ack` with "confirmed" answers the same. A dismiss
  reports `vote_sent` and, when its signed suppress vote stayed home, why
  (`vote_error` `presence_required` or `clock_unsynced`, with "Dismissed on
  this device only: ..."), and a dismiss of a chirp that is not there (the
  dismiss route and the ack's "resolved") answers 404 `not_found` where it
  answered `200 {"success":false}`. The dashboard shows either answer under
  the Community Activity list (`WebUiLogic.chirpActionNote`, `role=status`; it
  threw the answer away; `web_assets_gz.h` regenerated). The mute route's
  buffer is 160 bytes. Pinned by `test_chirp_commands_wap.cpp`'s
  `a_refused_confirm_names_why` and `a_dismiss_says_whether_its_vote_went`
  (and the named refusals in `every_command_runs_on_the_loop_task`; 14 single
  reverts each fail one), by `web_ui_logic.test.js`'s `chirpActionNote` suite
  (4 tests, all failing on the old page), and by `check_wap_loop_commands.py`
  rules CV9 (the two answers, and that the confirm, dismiss and ack routes
  answer through them right after the not-run guard), CV10 (no buffer under
  160 bytes for a `chirp_api.h` answer with a message; the longest is 161
  bytes, in CV9's 256) and CV11 (the dashboard glue). Spec §8.1 says so.
  Host-tested, with the handlers' JSON checked only in that scratch harness
  (`chirp_api.h` is not compiled on the host); the Arduino compile is CI's;
  not bench-tested (U1: the two F174 rows in
  `hardware_verification_checklist.md`). Found here: F195 and F196.
- [ ] **F175 [decision] A canary-wap with no GPS fix can never originate on
  Chirp.** The sketch has no SNTP; GPS is its one wall-clock source (F8, F28),
  and a send, a confirmation and a suppress vote all wait for `time()` to pass
  `MIN_UNIX_TIME`, while night mode reads true (the conservative answer).
  Since F146 the owner is told so (`clock_unsynced`), but a board without the
  GPS module, or indoors, stays receive-only. Decide whether Chirp takes a
  time from somewhere else (SNTP over the uplink, the hub, the phone in the
  companion wizard, as F28 takes the zone) or the Chirp page says up front
  that sending needs GPS. Found by F146 (#1762).
- [x] **F176 [code] The PlatformIO canary's dashboard has a Community (Chirp)
  panel whose routes nothing in its tree serves.**
  `firmware/canary/lib/securacv_webui/src/securacv_webui.cpp`'s page calls
  `GET /api/chirp`, `/api/chirp/recent`, `POST /api/chirp/send` and the rest,
  but no file under `firmware/canary` or `firmware/common` registers an
  `/api/chirp` route (`firmware/common/chirp/` holds only a header). From a
  grep; not run on a device. Serve them or hide the panel. Found by F146
  (#1762).
  *Done (#1762):* hidden until a route answers; no Chirp server built. The
  premise holds: no file under `firmware/canary` or `firmware/common`
  registers an `/api/chirp` route. The Community nav button starts hidden, and
  the page load's one `GET /api/chirp` shows it only when the answer is a
  Chirp status (it carries `state`), so on today's firmware the 404 (which
  `api()` returns as `{ok: false, error}`) keeps it hidden and nothing polls
  Chirp again; a firmware that serves the routes gets the tab back with no
  page change. The 5 s panel poll also asks for Chirp only once a status has
  answered (`chirpServed`).
  `firmware/tests_host/test_canary_community_panel.test.js` (5 cases, run by
  the tests_host Makefile) lifts the code out of the raw string: the markup
  starts hidden, a 404 keeps it hidden with no further request, a status shows
  it and loads the recent list, the poll asks nothing while no route has
  answered, and a scan of `firmware/canary` and `firmware/common` fails the
  day an `/api/chirp` route is registered, so the gate can go then. Three
  cases fail on the page before this change; five mutations each fail it. The
  Bluetooth tab has the same gap (F198). Host-tested (node); not run on a
  device (U1: the F176 row in `hardware_verification_checklist.md`). Found
  here: F198.
- [x] **F178 [code] canary-wap Chirp's send cooldown is a state, so a mute
  ends it and a send just after it says `cooldown` with 0 s.** `mute()` sets
  CHIRP_MUTED over CHIRP_COOLDOWN, `update()` expires only CHIRP_COOLDOWN,
  `unmute()` and the mute's timeout return to CHIRP_ACTIVE, and
  `can_send_chirp()` and the send command check only the state: muting after a
  send allows the next send at once (host harness: a second send 1 s after the
  first goes out, tier 2, at 7f45142 and in #1762), against spec 2.5.4's
  escalating cooldowns. And `update()` drains commands before its
  COOLDOWN-to-ACTIVE flip, so a send drained in the pass after a cooldown ran
  out (or in its last second) answers `cooldown` with `cooldown_remaining_sec`
  0, `GET /api/chirp` reads `cannot_send_reason` `cooldown` with 0 s, and
  `chirpSendGate()` says Ready with Send on for it: the symptom F146 removed
  for the clock, in a narrow window. Keep the cooldown as its own timer
  (`can_send_chirp()` checks the last send against the tier's cooldown), not a
  state the mute overwrites, and have the card turn Send off for any
  `can_send: false`. Pre-existing; `test_chirp_commands_wap.cpp`'s
  mute-in-cooldown tests compare the view to the live readers and pass
  whichever way this goes. Found by F146's review (#1762), which asked for it
  to be filed.
  *Done (#1762):* the premise holds: on the base, through the real handlers in
  a scratch ArduinoJson harness, a send one second after the first, muted in
  between, went out at tier 2; a send drained as the timer ran out answered
  `cooldown` with `cooldown_remaining_sec` 0, and `GET /api/chirp` 5 ms before
  the end read `cooldown` with 0 s. The cooldown is a timer
  (`cooldown_left_ms()`: the tier's cooldown counted from the last send, none
  at tier 0), and every gate reads it: `send_gate()` (what `can_send_chirp()`
  answers and the send refuses on), `read_status()` (at the read, so the route
  says it is over the moment it is) and `cannot_send_reason()` (`cooldown`
  exactly while the timer runs, muted or not). A refused send names the check
  that refused it, read once: `send_chirp()` reports `send_gate()`'s refusal,
  with the timer reading that gate used, or night from its own check.
  `run_command()` used to re-read the gate after the send, so a gate that
  opened in between was answered with no reason or `cooldown` with 0 s; found
  in review, and the presence, clock and 06:00 edges, which predate this PR,
  are fixed with it. `g_state` never holds `CHIRP_COOLDOWN`: `shown_state()`
  makes an active channel read `cooldown` while the timer runs. The route's
  `state` and the MQTT `chirp_state` read as before, except after a send made
  while muted: that read `cooldown`, then `active` with the presence beacon
  saying listening while the mute still dropped every chirp, and it now reads
  `muted` until the mute ends. A mute shows `muted` over a running cooldown,
  and an unmute or the mute's timeout reads `cooldown` again. The beacon still
  says not listening during a cooldown (kept and pinned; F194). `update()` no
  longer ends a cooldown after its drain. `cooldown_remaining_sec`, in the
  status and in a send refused for the cooldown, is rounded up
  (`seconds_left()`), so the last second reads 1, not 0. The Chirp card turns
  Send off for any `can_send` that is not true (`WebUiLogic.chirpSendGate`;
  `web_assets_gz.h` regenerated). Pinned by `test_chirp_commands_wap.cpp`'s
  `a_mute_does_not_end_the_cooldown` (the mute, the unmute, a 15-minute mute
  timing out inside tier 3's hour), `a_send_while_muted_stays_muted`,
  `a_send_just_after_the_cooldown_goes_out` and `a_send_at_an_edge_names_why`
  (each `millis()` call moves the uptime clock 1 ms on and each `time()` call
  the wall clock 1 s on, so every edge is reproducible);
  `the_pass_publishes_what_it_changed`, `cannot_send_reason_names_the_clock`
  and `every_command_runs_on_the_loop_task` moved to the timer. The new tests
  fail against the base `chirp_channel.cpp`, the muted-send test with F178
  reverted, and each edge of the edge test against the code before the review
  fix; single reverts in the new code each fail at least one test.
  `web_ui_logic.test.js` adds 2 `chirpSendGate` tests (both fail with the new
  branch removed). `check_wap_loop_commands.py` rule CV8 holds that only
  `shown_state()` and `state_name()` name `CHIRP_COOLDOWN` in
  `chirp_channel.cpp`, and CV7 pins the rounding in both routes. Spec §2.5.4,
  §7.1, §7.2 and §8.1 say so. Host-tested; the Arduino compile is CI's; not
  bench-tested (U1: the two F178 rows in
  `hardware_verification_checklist.md`). Found here: F192, F193 and F194.
- [x] **F192 [code] canary-wap Chirp: a mute on a disabled channel turns it on
  with no session.** `mute()` sets CHIRP_MUTED whatever the state, so `POST
  /api/chirp/mute` on a channel that is off makes `is_enabled()` true: `GET
  /api/chirp` reads `muted`, then `active` once the mute runs out, with an
  empty session emoji, and a later `POST /api/chirp/enable` answers success
  with an empty emoji, because `enable()` returns at once for any state but
  DISABLED; no session is made until a disable and an enable, or a reboot. On
  a device the passes would then send presence beacons with an all-zero
  session id (not probed: the host stub's ESP-NOW refused the frames). Refuse
  a mute (and an unmute) on a channel that is off, or have `enable()` check
  for a session. From a scratch harness of the real handlers, the same before
  this wave's changes and after them. Found by F178 (#1762).
  *Done (#1762):* the premise held: in a scratch harness of the real handlers
  over ArduinoJson 7.4.1, `POST /api/chirp/mute` on a channel that was off
  answered success, `GET /api/chirp` then read `muted` and then `active` with
  an empty session emoji, and `POST /api/chirp/enable` answered success with
  an empty emoji. A mute or an unmute on a channel that is off is now refused
  by name: the state stays as it was, no mute frame goes out under the
  all-zero session id, and `enable()` stays the one way on, so a later enable
  always makes a session. `mute()` and `unmute()` report a `MuteRefusal` (the
  channel off first, then a duration other than 15, 30, 60 or 120), and both
  routes answer through `send_mute_answer()` (`chirp_api.h`): `409
  {"success":false,"error":"chirp_disabled","message":"Chirp channel is not
  enabled"}`; `invalid_duration` keeps its 200 (the statuses are F195's
  decision). Found with it and fixed: `disable()` did not end a running mute,
  so a mute, a disable and an enable gave a new session that read `active`
  with `"muted":true` while every chirp was still dropped; `disable()` now
  clears it. The dashboard's mute and unmute buttons show the answer under the
  Community Activity list (`WebUiLogic.chirpActionNote`; `web_assets_gz.h`
  regenerated). Pinned by `test_chirp_commands_wap.cpp`'s
  `a_mute_needs_a_channel_that_is_on` and `a_disable_ends_the_mute`, with both
  refusal messages pinned word for word (29 tests, 1033 checks);
  `web_ui_logic.test.js`'s mute and unmute suite (4 tests) fails 4 of 4 on the
  old page; `check_wap_loop_commands.py` rule CV12 and CV11's glue for both
  buttons (318 mutations in the integrated check). Spec §7.1, §7.2 and §8.1
  say so. Host-tested, the handlers' JSON checked only in the scratch harness;
  the Arduino and PlatformIO compiles are CI's; not bench-tested (U1: the two
  F192 rows).
- [ ] **F193 [code+decision] canary-wap Chirp's cooldown tiers reset 24 hours
  after the first chirp of a run, not after 24 hours without one.**
  `reset_cooldown_if_stale()` clears the tiers 24 hours after
  `first_chirp_today_ms` whatever was sent since, where spec 2.5.4 says
  "Reset: 24 hours of no chirps", and the reset also ends a running cooldown
  (a 4-hour one started 22 hours into the run is over 2 hours later).
  Separately `enable()` zeroes the tiers and they live in RAM, so a disable
  and an enable, or a reboot, costs the 10-minute presence requirement instead
  of the tier's 15 minutes, 1 hour or 4 hours. Decide which reset the spec
  means and whether the tiers survive a re-enable and a reboot (a new session
  is a new identity on the wire; the tiers are local). From code. Found by
  F178 (#1762).
- [x] **F194 [code] canary-wap Chirp's presence beacon says "not listening"
  while the send cooldown runs.** `send_presence()` sets `listening` from what
  the state reads as, and an active channel in its cooldown reads
  CHIRP_COOLDOWN, but such a device still takes chirps (`handle_witness()`
  drops them only while muted), so neighbors' nearby lists show it as not
  listening. F178 kept the wire as it was (`a_mute_does_not_end_the_cooldown`
  pins it); setting the flag from the mute alone would fix it. From code.
  Found by F178 (#1762).
  *Done (#1762):* the premise held (the scratch harness: the beacon after a
  send said `listening` 0). `send_presence()` now sets the flag from the mute
  alone, `is_muted()`, the same test `handle_witness()` calls for its drop, so
  the flag and what the device takes cannot drift: listening through the send
  cooldown, not while a mute runs, and listening again when the mute ends with
  the cooldown still running. `a_mute_does_not_end_the_cooldown`, which pinned
  the old wire, now expects listening on the pass the mute ran out on; the new
  `the_beacon_says_listening_through_a_cooldown` checks each beacon against a
  real witness frame taken or dropped in the same state. Both fail with the
  old flag, and a flag always 1 fails `a_send_while_muted_stays_muted`. Spec
  §3.2 now says the canary-wap sends exactly "accepting community chirps".
  Host-tested; the compile is CI's; not bench-tested (U1: the F194 row).
- [ ] **F195 [decision] canary-wap Chirp's POST routes answer refusals with
  different statuses.** Since F174 a refused confirm answers 409 or 404, while
  a refused send (F146's `chirp_disabled`, `presence_required`, `cooldown`,
  `clock_unsynced`, `night_restricted`), a send with an unknown template, a
  refused mute (`invalid_duration`) and a failed enable still answer 200 with
  `success:false`. Decide whether every Chirp refusal carries a status (the
  dashboard reads the body either way, and must never get a 403, which it
  takes for a bad token). Found by F174 (#1762).
- [x] **F196 [code] canary-wap: other REST answers may outgrow the char
  buffers they are serialized into.** The Chirp confirm and mute refusals did
  (F174): `serializeJson()` fills a char array without a terminator when the
  answer does not fit, and `httpd_resp_sendstr()` then sends whatever follows
  on the stack. Rule CV10 holds only `chirp_api.h`; a crude scan of the other
  `*_api.h` files and `canary_wap.ino` for the same shape found nothing, but
  their answers were not measured. Measure each fixed buffer against its
  longest answer, or serialize to the length `measureJson()` gives. Found by
  F174 (#1762).
  *Done (#1762), in two halves, merged here (the Chirp package's and the
  Bluetooth package's):* the premise held, and it is wider than the item says:
  the sized form `serializeJson(doc, buf, n)` also terminates only an answer
  shorter than n (ArduinoJson 7.4.1), so a full heap buffer is the same bug.
  Measured in a scratch harness of the real handlers over ArduinoJson 7.4.1:
  `GET /api/chirp/nearby` with 32 neighbors whose unsigned presence beacons
  carry 30-byte emoji of `"` is 3864 bytes against its 3072-byte stack buffer
  (3078 bytes went out, the last 6 from the stack), and `GET
  /api/chirp/recent` with sixteen ordinary chirps is 4764 bytes against its
  `malloc(4096)` (4098 bytes went out, followed by the heap, and the dashboard
  showed no alerts). The nearby, recent and templates lists and `GET
  /api/rf/status` (two `const char*` fields no fixed buffer can be measured
  against; not shown to overflow) now serialize to `measureJson()`'s length on
  the heap, and the Chirp enable answer's buffer is 160 (its longest answer is
  137 bytes; today's is 57). Every Bluetooth answer now goes through
  `send_doc()` (`bluetooth_api.h`), which reserves a `String` to
  `measureJson() + 1` and answers the fixed allocation error when the
  reservation fails; no Bluetooth answer had gone out cut (the closest, the
  init error, at most 112 of 128), and the harness gave byte-identical answers before
  and after. The rest was measured and kept: the sketch's other serializations
  write a `String` or a `File`, except the fleet scan's cache (F211), and the
  21 fixed buffers left fit their longest answers (the closest, the mute
  answer, 100 of 160). The new `firmware/scripts/check_wap_json_answers.py`
  (rules J1-J4, run by `regression_check.sh`) holds it: a `String` or `File`
  grows; a heap buffer of `measureJson()+1` bytes, or a fixed one behind a
  `measureJson()` refusal that returns, needs no bound; every other fixed
  buffer must exceed the longest answer the check computes from the document's
  own statements; and what it cannot bound fails. On the integrated tree it
  reads 86 `serializeJson()` calls in 11 files, `bluetooth_api.h` included
  (the Chirp package left that file out until the Bluetooth half landed;
  integration dropped the exclusion), measures 21 fixed buffers and refuses 29
  mutations; on the base tree it fails 8 sites, 3 of them in
  `bluetooth_api.h` (the Chirp package counted 5 with that file left out). `check_wap_loop_commands.py`
  rule BV6 keeps every `serializeJson()` of `bluetooth_api.h` inside
  `send_doc()` (the base file fails it at ten sites), and CV7's
  free-after-serialize pins follow the measured form. The check is a static
  reader, not a compiler, so a shape nobody tried may still pass. The
  `snprintf`-built answers in `canary_wap.ino` are another shape and were not
  measured (F212). Spec §8.1 says so. Statically checked and harness-measured;
  the compiles are CI's; not bench-tested (U1: the two F196 rows; the Chirp
  row's setup takes at least six sender boards). Found here: F211-F213.
- [x] **F211 [code] canary-wap: a cut fleet-scan cache answers no canaries.**
  `fleet_scan_task()` (`canary_wap.ino`) keeps at most 8 mDNS adverts, but
  each of their seven TXT values can carry up to 255 bytes (and `"` or `\`
  escape to two), so eight adverts with long values outgrow
  `FLEET_SCAN_CACHE_SIZE` (2560): the cache is cut (terminated by hand),
  `handle_fleet_scan()`'s `deserializeJson()` fails, and `GET /api/fleet/scan`
  answers an empty `canaries` list, so the Fleet sheet shows none, whichever
  device on the LAN sent the long values. Keep the adverts that fit
  (`measureJson()` per entry). `check_wap_json_answers.py` names the site in
  `EXEMPT`. From code; not probed. Found by F196 (#1762).
  *Done (#1762):* the premise held, shown in a scratch harness over
  ArduinoJson 7.4.1 with the old task's code: eight adverts whose 255-byte TXT
  values hold `"` and `\` made a 4675-byte list, 2560 bytes were kept,
  `deserializeJson()` answered IncompleteInput and the route listed no other
  Canary. The cache is now written by `fleet_scan_cache.h` on a new pure
  writer, `wap_json_writer.h`. `fleet_scan_cache::fill()` is the task's whole
  browse: it measures every result's row, keeps the shortest rows that fit
  together (at most eight; of two equal rows, the first browsed) and writes
  them in browse order, each whole, with the closing `]}` reserved from the
  start. A long advert costs only its own row, wherever mDNS answers it; with
  more than eight Canaries the eight shortest rows are listed, not the first
  eight answered. A control byte other than `\b`, `\f`, `\n`, `\r` and `\t` is
  written as `�`: `handle_fleet_scan()` re-serializes the cache with
  ArduinoJson, which writes such a byte raw, and the browser's JSON.parse
  refuses it (in a scratch harness of that read, 27 of 257 route answers
  failed with the first F211 header and none now).
  `tests_host/test_fleet_scan_cache.cpp` (582 checks) runs the real header at
  the sketch's `FLEET_SCAN_CACHE_SIZE`: the item's eight adverts keep rows
  {2,4,6,7}, the four shortest; one long advert at each of eight positions
  beside seven Canaries keeps the seven; 400 seeded browses match an oracle
  and a brute force over every subset. A `fill()` that keeps rows greedily in
  browse order fails 428 of its checks, one bounded at the first eight results
  432, a cache that keeps `\u00XX` 402. `check_wap_json_answers.py` drops its
  `EXEMPT` table; rule J4 holds `fleet_scan_task()` to `fill()` (the count
  read once and handed on unchanged, no `add()` of its own, nothing else
  writing the staging buffer; 10 mutations). Host-tested and harness-measured;
  the Arduino and PlatformIO compiles are CI's; not bench-tested (U1: the F211
  row). The device's own name still reaches the route's self row raw (F226),
  and the list still takes every advert on trust (F228).
- [x] **F212 [code] canary-wap: the sketch's `snprintf`-built JSON answers are
  neither measured nor escaped.** `snprintf()` terminates, so a too-long
  answer is cut (unparseable) rather than followed by memory, but
  `http_send_error()` (128 bytes), `peek_gate_refuse()` (160), the BLE chirp
  send answer (128) and `handle_device_info()` (768) in `canary_wap.ino` do
  not check its length, and `handle_device_info()` writes the device name with
  `%s`: `set_device_name()` checks only the length, so a name holding `"` or
  `\` would break that answer (whether the routes in front of it refuse those
  was not checked). Measure them (or extend `check_wap_json_answers.py` to
  them) and escape what a person typed. From a scan; not probed. Found by F196
  (#1762).
  *Done (#1762):* the premise held. The rename routes (POST
  `/api/wifi/connect`'s `device_name`, POST `/api/device-name`) refuse neither
  `"` nor `\`, and in a scratch harness over ArduinoJson 7.4.1 a name such as
  `kitchen "north"` made GET `/api/device-info` fail `deserializeJson()`; the
  device's own pages read that route. It and the provisioning receipt are now
  built by `identity_json.h` on `wap_json_writer.h`: every string goes through
  the escaper, and each answer is measured first and written into a heap
  buffer of exactly that length, never cut, with the old bytes for every
  ordinary input; the receipt is wiped with `secure_zero()` before it is freed
  (no rule pins the wipe). Every writer call is qualified (`wap_json::...`):
  the first version's unqualified `boolean(...)` was ambiguous next to
  Arduino.h's `typedef bool boolean;`, so neither ESP32 build would have
  compiled the sketch, while the host suites passed. Both new suites now
  include `tests_host/arduino_globals.h` (arduino-esp32 3.3.8's Arduino.h
  names) first, and with the first version's headers the identity suite no
  longer compiles. The sketch's other `snprintf()` answers were measured and
  kept: `http_send_error()` (longest 44 of 128), `peek_gate_refuse()` (80 of
  160), the BLE chirp send (61 of 128) and the mic mute (43 of 80) fit; the
  rest test `snprintf()`'s length and refuse, or compose behind one length
  flag; none writes a string a person typed. `check_wap_json_answers.py` gains
  J5 (each `snprintf()` answer measured, every `%s` escaped by construction)
  and J6 (the identity answers at their measured length). J5 and J6 hold 15
  mutations and J4 10 (F211); the check refuses 53 in all (on the integrated
  tree: 85 `serializeJson()` calls, 21 fixed buffers, 13 `snprintf()` answer
  pieces). `tests_host/test_identity_json.cpp`
  (96 checks) holds the old bytes for ordinary input and gives back a name
  holding `"` and `\` as typed; writing the name raw again fails 13 checks,
  the AP SSID raw 5 (the other fields are device-made, and no test holds their
  escape). Host-tested and statically checked; the compiles are CI's; not
  bench-tested (U1: the F212 row). Found here: F227 (what J4 to J6 do not
  read), F229 (the other host suites lack Arduino.h's names) and, by the
  review, F230 (the writer's copy trusts the length it measured).
- [x] **F213 [code] canary-wap Chirp: a presence beacon's emoji is stored as
  sent.** `handle_presence()` copies the payload's 31 bytes into the nearby
  table, and presence frames are unsigned, so any device in ESP-NOW range puts
  any bytes (quotes, control characters, markup) into every neighbor's `GET
  /api/chirp/nearby`. The route serializes them whole since F196, and no page
  renders the list today, but the spec gives the field as an emoji string
  (§3.2), and a witness's sender emoji is derived from its session id
  (`generate_emoji_string()`), never taken from the frame. Derive the nearby
  emoji the same way, or refuse bytes outside the emoji set. From code. Found
  by F196 (#1762).
  *Done (#1762):* the premise held. `handle_presence()` now sets a nearby
  row's emoji with `generate_emoji_string()` of the beacon header's
  `session_id`, the id the row is keyed on, as `handle_witness()` derives a
  chirp's sender emoji; the beacon's own `emoji` field is never read. Derived
  rather than filtered: spec §2.3 makes the emoji the display of the session
  id, and a filter would still let a beacon show an emoji unrelated to its id.
  Every byte now comes from `EMOJI_SET`, and a canary-wap beacon carries its
  own session's display, so an honest neighbor shows what it sends. The beacon
  stays unsigned: a device in range still chooses the session id it claims,
  and with it which five emoji its row shows, but no other byte.
  `test_chirp_commands_wap.cpp`'s new `a_beacon_emoji_is_its_session_display`
  sends forged fields (quotes, backslashes, control bytes 0x01 to 0x1D and
  DEL, markup, `</script>`, a full 31-byte field with no terminator) and
  requires each row, in the view and the live table, to be its session's
  display; it fails with the old copy put back (the review sent all 256 byte
  values: every row derived). On the integrated tree the suite runs 30 tests
  and 1088 checks. Spec §3.2 and §8.1 say so; bench row "canary-wap Chirp: a
  neighbor's emoji is its session's display (F213)". Host-tested; the compile
  is CI's; not bench-tested (U1). Found here: F231 (the spec's three emoji)
  and F232-F234 (what unsigned presence and suppress votes still steer).
- [ ] **F226 [code] canary-wap: a control byte in the device name breaks two
  ArduinoJson answers.** The rename routes (POST `/api/wifi/connect`'s
  `device_name`, POST `/api/device-name`) take any byte but NUL, and
  ArduinoJson 7.4.1 writes a control byte other than `\b`, `\f`, `\n`, `\r`
  and `\t` raw, which a browser's JSON.parse refuses. POST `/api/device-name`
  answers the new name through ArduinoJson, and GET `/api/fleet/scan` puts it
  in its self row: in a scratch harness of `handle_fleet_scan()`, a name
  holding 0x01 made the route's answer unparseable, and the Fleet sheet then
  says it could not reach the device (`/api/device-info` escapes the name
  since F212; the scan's other rows write such a byte as U+FFFD since F211).
  Refuse control bytes in a name at the rename routes or in
  `setup_wizard::set_device_name()`, or write those answers with
  `wap_json_writer.h`. Harness-measured and from code; not probed on a device.
  Found by F211's review (#1762).
- [ ] **F227 [code] canary-wap: `check_wap_json_answers.py`'s J4 to J6 leave
  gaps.** J5 reads only the `snprintf()` answers a handler sends: the JSON
  texts `canary_wap.ino` builds that are not answers (the pairing QR payload,
  the two BLE witness values, the health log's dropped-lines note, the MQTT
  sensing body) and the builders outside the answer files (`sys_monitor.h`'s
  `get_json()`, `power_monitor.h`, `hardware_state.h`, `api_auth.h`'s 429
  answer, `csi_integration.cpp`, `csi_mqtt.cpp`) are outside its scope. Within
  it, three shapes pass (a `["%s"]` answer, an unsized `sprintf()`, a `%s`
  answer sent through a local helper such as `send_json()`), and J4 and J6
  check that statements are present, not their order or that nothing else
  names the buffer (a `free()` moved above the send, a deleted
  `secure_zero()`, a `strcat()` after the build all pass). None of these
  shapes exists today. Extend the rules (refuse `sprintf()` in answer files,
  treat each `(httpd_req_t*, const char*)` helper as a sink, require J6's
  order), or record why each case is safe. From a scan and the review's
  mutations; not probed. Found by F212 and its review (#1762), merged into one
  item.
- [ ] **F228 [decision] canary-wap: the fleet scan lists every
  `_securacv._tcp` advert as a Canary.** Any device on the LAN can advertise
  the service with any TXT values, and adverts are unsigned. GET
  `/api/fleet/scan` keeps the shortest adverts that fit (F211), so a long
  advert can no longer push a Canary out, but short adverts nobody owns win a
  place ahead of a real Canary whose row is longer: eight short ones fill the
  list, and the Fleet sheet cannot tell them apart. Decide whether the list
  prefers adverts it can verify (a device id the fleet knows, a signed TXT
  field) or marks rows as unverified. From code. Found by F211 (#1762).
- [ ] **F229 [code] canary-wap tests_host: the other pure-header suites
  compile without Arduino.h's global names.** F212's first `identity_json.h`
  passed every host test and could not compile in the sketch, whose
  translation unit opens with Arduino.h (`typedef bool boolean;`, `byte`,
  `word`, the pin and bit macros). `tests_host/arduino_globals.h` now carries
  arduino-esp32 3.3.8's names, but only `test_fleet_scan_cache.cpp` and
  `test_identity_json.cpp` include it. Include it first in every suite that
  compiles a header the sketch includes (`wap_diagnostics.h`,
  `mqtt_identity.h`, `loop_snapshot.h` and the rest), or compile each such
  header once after it in a Makefile target. From code. Found by F212's review
  (#1762).
- [ ] **F230 [code] canary-wap: `wap_json_writer.h`'s escaped write trusts a
  length it measured earlier.** `escaped()` checks room for the string's
  measured length, then copies up to the string's NUL without bounding the
  copy by that length, and `room()` subtracts in a way that wraps once the
  length passes the cap. `handle_device_info()` measures and then writes in
  two passes over live globals, and the loop task writes
  `g_wifi_status.ap_auth`; had it grown between the passes, the write would
  overrun the exactly sized heap buffer, where the old bounded `snprintf()`
  gave a cut answer. Practically unreachable today (the loop rewrites the same
  label, and the name changes only on the API task). Bound the copy by the
  length checked and stop on overflow, write `room()` without the wrapping
  subtraction, or copy the globals before measuring. From code. Found by
  F212's review (#1762).
- [ ] **F241 [code] canary-wap saves an MQTT topic prefix of 32 characters but
  reads back the default.** `/api/mqtt` accepts and stores `MAX_PREFIX_LEN`
  (32) characters (the settings page's input has no `maxlength`), and
  `config_load`'s `Preferences::getString(key, buf, MAX_PREFIX_LEN)` refuses a
  stored string whose length with its NUL exceeds that, so the device falls
  back to `securacv` without a word. `stubs/mqtt/Preferences.h` truncates
  instead, so no host test can see it; the host, user and password fields are
  read the same way. Refuse a prefix longer than `MAX_PREFIX_LEN - 1` in the
  handler (or read with `sizeof(out->prefix)`), check the other fields, and
  make the stub refuse as the core does. From code and arduino-esp32's
  `Preferences.cpp`; pre-existing. Found by HA16's review (#1762).
- [ ] **F231 [code] canary-wap Chirp: the spec describes a three-emoji display
  the canary-wap renders as five.** `generate_emoji_string()` renders five
  emoji from the session id (up to 30 bytes in the 31-byte field), while spec
  §2.3's pseudocode builds three and §3.2 gives the beacon's `emoji` as `tstr
  .size (3..12)`. Bring §2.3 and §3.2 to five (and the field to what is sent),
  or say which is meant. From code and the spec. Found by F213 (#1762).
- [ ] **F232 [code+decision] canary-wap Chirp: unsigned presence beacons fill
  the nearby table and steer two gates.** `handle_presence()` adds a row for
  any session id a beacon claims, up to `MAX_NEARBY_CACHE` (32), and refuses
  new rows while full; a row lives three minutes after its last beacon. The
  table gates confirmations (`nearby_has_pubkey_with_presence()`: a confirmer
  with no row is dropped) and sets the community suppression threshold
  (`g_nearby_count / 2`, at least 3). So one device in range beaconing 32
  made-up session ids keeps real neighbors out of the table (their
  confirmations refused, so no chirp validates) and turns community
  suppression off (F233: from 18 rows on, no chirp can be suppressed). Decide
  what a presence row should cost (a beacon signed with the session key, as
  witnesses are; a per-sender-address cap; evicting the stalest row rather
  than refusing the newest) and whether the threshold should count only rows a
  signed frame confirmed. From code; the suppression effect host-probed, the
  confirmation effect not probed. Found by F213 (#1762).
- [ ] **F233 [code] canary-wap Chirp: with 18 or more nearby rows, community
  suppression is unreachable.** `handle_suppress_vote()` keeps at most
  `MAX_CONFIRMERS_PER_CHIRP` (8) voters per chirp, but the threshold is
  `g_nearby_count / 2` (at least 3), which is 9 or more from 18 rows on.
  Host-probed with real signed votes (a scratch test beside
  `test_chirp_commands_wap.cpp`, not committed): 16 and 17 rows suppress at
  the 8th vote; 18, 20 and 32 rows hold `suppress_count` at 8 and never
  suppress. Unsigned beacons reach it from one device (F232), and so do 18
  real neighbors with no attacker. Below the cap the code also differs from
  spec §2.5.6, which asks for more than 50% of `nearby_count` where the code
  takes at least half rounded down (7 rows suppress at 3 votes), and
  `SUPPRESS_WINDOW_MS` (120 s) is defined in `mesh_network.h` and read
  nowhere. Cap the threshold at the voter set or size the set to the table,
  and bring the comparison and the window to the spec (or the spec to the
  code). Found by F213's review (#1762).
- [ ] **F234 [code+decision] canary-wap Chirp: a suppress vote counts from any
  session key, with no nearby row.** `handle_suppress_vote()` checks the
  vote's signature and that the header's session id is the voter key's, but
  not that the voter has a presence row (a confirmation must), and a session
  key costs nothing to make. Host-probed (the same scratch test): votes signed
  by fresh keys suppress a witnessed `TPL_EMERG_FIRE_VISIBLE` chirp at a
  receiver with 0 or 6 rows at the third vote and with 17 rows at the eighth;
  a suppressed chirp leaves the recent table at the next prune (30 s), so `GET
  /api/chirp/recent` stops showing it. So one device in range can hide any
  chirp, an emergency template included, at every receiver in range with fewer
  than 18 rows, against spec §2.5.6's "legitimate content won't get
  mass-dismissed". Decide who may vote (a presence row, as for confirmations;
  a row a signed frame confirmed; F232's per-address cost) and whether an
  emergency template can be voted down at all. From code and a host probe.
  Found by F213's review (#1762).
- [x] **F97 [code] The PIO pairing has F75's deadlock.**
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
  by F75 (#1762).
  *Done (#1762):* the premise holds and is wider. With frames delivered as
  they are sent, no order completed. The joiner's owner first timed out both
  sides, as above. The initiator's owner first had its CONFIRM dropped by the
  joiner, which then waited for it in `AWAITING_CONFIRM_PEER` and dropped the
  COMPLETE, so the initiator reported PAIRED, and registered, bound and
  persisted a member that never joined, while the joiner timed out
  (host-probed on wave 9's code; the existing tests confirmed both owners before
  either CONFIRM crossed). canary-wap's F75 fix is carried over in
  `mesh_pairing`, with one difference:
  - a CONFIRM counts only from the partner's address and only once the code is
    shown (`AWAITING_CONFIRM` or later; before the ACCEPT the session key is
    all zero);
  - the initiator keeps a verified early CONFIRM
    (`PairingContext::peer_confirmed`) and completes at its owner's
    `confirm_code` (the confirm runs from the REST slot on the main loop, and
    the same `process()` reports PAIRED);
  - the joiner checks the initiator's CONFIRM in either order and takes the
    COMPLETE in `AWAITING_CONFIRM_PEER` or `AWAITING_COMPLETE`, that is once
    its own owner confirmed; only a joiner takes one;
  - a wrong hash from the partner's address ends the pairing in either order
    (one arriving before the owner's confirm used to be dropped);
  - the difference: every PIO COMPLETE goes out with the initiator's own
    CONFIRM immediately in front of it (`Action::leading_confirm`, built
    before the session key is wiped and sent by the session as its own frame),
    the reverse of what this item proposed. An updated joiner does not need
    it; a joiner on the pre-F97 firmware does. With the COMPLETE alone, an
    updated initiator reported PAIRED while that joiner dropped the COMPLETE,
    in both orders (review finding, host-probed against wave 9's real
    handlers); with the CONFIRM in front both sides reach PAIRED in both
    orders. canary-wap sends the COMPLETE alone because its receive buffer
    holds one frame; this tree's ring holds eight.
  Pinned by eight `test_mesh_pairing` tests and three `test_mesh_session`
  tests. With `mesh_pairing.cpp` at wave 9's code these fail:
  `the_joiners_owner_may_confirm_first`,
  `the_initiators_owner_may_confirm_first`,
  `a_bad_confirm_from_the_partner_ends_the_pairing_in_either_order`,
  `the_joiner_takes_a_complete_only_after_its_owner_confirms`,
  `pairing_over_the_air_joiner_confirms_first` (as initiator, through the REST
  slot: exactly two frames, the CONFIRM then the COMPLETE) and
  `pairing_over_the_air_initiator_confirms_first` (as joiner). The older-joiner
  case, `a_pre_f97_joiner_completes_in_either_order` (a joiner on wave 9's
  rules) and `pairing_over_the_air_with_a_pre_f97_joiner` (through the
  session's sends), fails on the dropped COMPLETE with the leading CONFIRM not
  built, and the session test with it built but not sent. The guards fail by
  mutation: `a_confirm_from_another_address_does_not_count` with the address
  check removed, `a_confirm_before_the_code_is_shown_does_not_count` with the
  state check removed (the initiator then seals the opera_secret under the
  all-zero key), `the_initiator_still_waits_for_the_joiners_confirm` when the
  owner's confirm completes without the joiner's CONFIRM, and the joiner test
  with the role check removed or a COMPLETE taken before the owner's confirm.
  Not closed: the reflected CONFIRM (F94, host-probed on this tree too, the
  same before and after), and the reverse mix, a pre-F97 initiator with an
  updated joiner (F117). Spec §5.2 and THREAT_MODEL are updated. Host-tested
  only; the `[env:full]` compile is CI's; not bench-verified (U1). Found here:
  F117.
- [x] **F117 [code] PIO: a pre-F97 initiator pairs an updated joiner only when
  the initiator's owner confirms first.** The pre-F97 initiator drops a
  CONFIRM that arrives before its own owner confirmed, and the updated joiner
  sends its CONFIRM once. So when the joiner's owner confirms first, both sides
  wait and time out, and neither keeps the other: the same as between two
  pre-F97 devices, and no false success (host-probed with wave 9's real
  handlers against wave 10's). The other mix, an updated initiator with a
  pre-F97 joiner, completes in both orders since the F97 review (the CONFIRM
  in front of every COMPLETE). Fix, if mixed opera members matter until the
  field updates: the updated joiner re-sends its CONFIRM when it reads the
  initiator's CONFIRM in `AWAITING_CONFIRM_PEER`; a pre-F97 initiator then
  answers with the COMPLETE, and an updated one, already PAIRED, drops it. Or
  say in the pairing guide to confirm on the Canary already in the opera
  first. Found fixing F97's review (#1762).
  *Done (#1762):* reproduced with the real pre-F97 code: c6a305b's
  `mesh_pairing.{h,cpp}` compiled into a scratch probe beside this tree's
  library (namespace renamed), frames crossing as bytes, both sides ticked on
  one clock. On wave 10's code a pre-F97 initiator with an updated joiner, the
  joiner's owner first, timed out on both sides, as two pre-F97 devices do;
  the other seven role, version and order combinations behaved as the item
  said. Fixed as proposed, bounded: the updated joiner arms a re-send of its
  CONFIRM when it reads the initiator's CONFIRM in `AWAITING_CONFIRM_PEER`
  (moving to `AWAITING_COMPLETE`); that CONFIRM says the initiator's owner has
  confirmed, so a pre-F97 initiator is waiting for exactly this. `tick()`
  sends it `CONFIRM_RESEND_FIRST_MS` (1 s) later, then every
  `CONFIRM_RESEND_INTERVAL_MS` (2 s), at most `CONFIRM_RESEND_MAX` (3) times,
  only to the partner and only while no COMPLETE has come. The session's
  receive path now stamps pairing frames with the last `process()` time (it
  passed 0, which nothing read until now). On the probe the mixed pair now
  completes in both orders, with the owners' confirms 0 s, 5 s, 60 s and 240 s
  apart, and every other combination, two updated devices included, sends the
  same frames as on wave 10's code (one joiner CONFIRM, one COMPLETE): an
  updated initiator's COMPLETE follows its CONFIRM at once, and a late copy
  would reach an initiator already PAIRED, which drops it. Pinned by
  `test_a_pre_f97_initiator_completes_in_either_order` (a model of the pre-F97
  initiator, as F97's tests model the old joiner),
  `test_the_joiners_confirm_resend_is_bounded` (three copies at 1, 3 and 5 s,
  all to the partner; none from `AWAITING_CONFIRM_PEER` or from an initiator),
  `test_two_updated_devices_resend_nothing` (both orders, ticked every 50 ms)
  and the session's `test_pairing_over_the_air_with_a_pre_f97_initiator` (both
  orders on the session's clock, and an updated initiator whose COMPLETE lands
  a loop pass after its CONFIRM). They fail by mutation: the re-send not armed
  (both sides time out, as before), unbounded, due at once, and the receive
  stamped 0. Spec §5.2 and THREAT_MODEL are updated. Host-tested only; the
  `[env:full]` compile is CI's; not bench-verified (U1). Found here: F134.
- [x] **F98 [code] canary-wap's `add_peer` takes a new member at an address
  another member holds.** Only the re-pair path (`rebind_peer`) refuses such
  an address; a new key at it is appended. The two entries then share one
  ESP-NOW registration, and `remove_peer` on either deletes it for both.
  Host-probed with the #1761 harness: a new key at C's address was added
  (three members). Removing it dropped C's registration, and A's next
  heartbeat to C was not sent. Fix: refuse it as the re-pair does (one
  address, one member), or keep the registration while any entry holds the
  address. F102 is the PIO tree's case of one address stored for two members.
  Found while fixing F71-F76 (#1762).
  *Done (#1762):* `add_peer` refuses a new key at an address another member
  holds, as `rebind_peer` does (one address, one member), so a pairing from a
  copied member address fails through F73's path on either side. Both
  refusals log their own WARNING ("another member holds that radio address;
  remove it first"), since `fail_pairing`'s line cannot say which refusal it
  was. Their routine case is a device whose NVS was erased or that was
  reflashed: it keeps its radio address and comes back with a new key, and
  each member that still holds its old entry refuses it until that entry is
  removed. On canary-wap each such removal splits that member from the opera
  until F48 (F95), so every member that held the old entry ends up on an
  `opera_id` of its own and the others must re-pair with it (whether to rotate
  at all meanwhile is F114). Before F98 the new key was added beside the old
  entry, at the same address. NVS an older firmware wrote can still hold two
  members at one address: `remove_peer` and `rebind_peer` now drop an ESP-NOW
  registration only when no other member holds the address (`release_mac`),
  so removing or moving one no longer strands the other. Pinned by
  `test_mesh_address_wap`'s `a_new_key_at_another_members_address_is_refused`,
  `removing_a_member_keeps_an_address_another_member_holds` and
  `a_re_pair_away_from_a_shared_address_keeps_it_for_the_other`, and
  `test_mesh_liveness_wap`'s
  `an_initiator_refuses_a_new_member_at_another_members_address`,
  `a_joiner_refuses_a_new_initiator_at_another_members_address` and
  `a_device_back_with_a_new_key_rejoins_once_its_old_entry_is_removed`
  (refused, logged, old entry removed, re-paired, split from the others). Each
  fails with its fix reverted, and the log checks fail with either log call
  removed. `test_mesh_rx_gates_wap`'s text pins follow. Spec §5.2 and §8.3 and
  the threat model say so. Host-tested only; the Arduino compile is CI's; not
  bench-verified (U1).
- [x] **F99 [code] After a one-sided removal and a re-pair, canary-wap's
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
  highest reservation, as a boot does since F71. Found by F71 (#1762).
  *Done (#1762):* the second option, in a stronger form; the first cannot
  work as written (a re-pair is possible only once the deny-list entry has
  expired, and the kept reservation would expire with it). `add_peer` starts a
  new member one past the highest counter this device can have signed to
  anyone (`g_tx_high_signed`: every counter `send_to_peer` spent since the
  boot, above the highest reservation the boot read back; 1 on a device that
  has signed none), and records it covered up to that counter, so a reboot
  before its first frame resumes above it too. A removal holds every
  survivor's reservation to it before the save, so the record still covers the
  removed member's counters when its entry goes; with no member left the
  record is not rewritten, and `load_tx_reservations` now reads it with no
  member loaded too (`init()` calls it when no opera is loaded). That also
  covers a re-pair after leaving the opera, and a device whose opera was not
  loaded because flash encryption is off. The review found the first version
  starting past the highest reservation, up to a block above anything signed,
  which widened F72's gap by up to a block at every pairing (host-probed: one
  A-to-J frame replayed at B cost B 984 of A's heartbeats); a new member now
  starts level with the busiest member (see F72). Premise host-probed on the
  base first (B dropped A's frames 1..5 and heard 6). Pinned by six
  `test_mesh_liveness_wap` tests, each failing with its piece reverted:
  `a_re_paired_removed_member_hears_its_remover_at_once`,
  `a_removed_members_reservation_outlives_it_and_a_reboot`,
  `a_device_re_paired_after_its_partner_held_no_one_hears_it`,
  `a_device_whose_opera_was_not_loaded_re_pairs_above_its_counters`,
  `a_reboot_before_the_first_send_keeps_a_re_paired_member_above` and
  `a_new_member_starts_level_with_the_busiest_member`. Not covered: NVS from
  before F71 with no member left and no record, where a new member starts at
  1, as on a fresh device, since the two cannot be told apart. Spec §3.3 and
  §12.3 and the threat model say so. Host-tested only; the Arduino compile is
  CI's; not bench-verified (U1). Found here: F113.
- [x] **F113 [code] canary-wap's `leave_opera` saves an all-zero opera that the
  next boot loads as configured.** `leave_opera` zeroes `g_opera_config` and
  persists it, so NVS holds a 16-byte zero `opera_id` and a 32-byte zero
  `opera_secret`, and `load_opera_config` counts those as configured (it
  checks the lengths only). Host-probed with the #1762 harness: after a leave
  and a reboot the device holds that opera (`MESH_DISABLED`, since `enabled`
  was zeroed too). Once enabled, `start_pairing_initiator` keeps it instead of
  founding one; the joiner derives its `opera_id` from the zero secret while
  the initiator keeps the stored zero id, and the joiner drops every frame of
  the initiator's. Fix: remove the keys on a leave (or store `configured`),
  and refuse an all-zero secret at load. Found while fixing F99 (#1762).
  *Done (#1762):* premise host-probed on the base first: after a leave and a
  reboot the device held the zero opera (`MESH_DISABLED`); turned on and
  started as an initiator it kept it and sealed its zero secret to the joiner,
  whose `opera_id` then differed, and the joiner did not hear it.
  `persist_opera_config()` now writes the id and secret only while an opera is
  configured and removes both keys otherwise, which covers the leave and a
  turn-on or rename after it (`set_enabled` and `set_opera_name` save the
  config too, and wrote the zeros again); `load_opera_config()` refuses an
  all-zero id or an all-zero secret, so NVS an older firmware's leave wrote is
  not loaded either (logged once, at that boot) and the next pairing
  overwrites it. A leave still turns the mesh off, as before. Pinned by three
  `test_mesh_liveness_wap` tests:
  `a_device_that_left_founds_a_new_opera_after_a_reboot` and
  `a_setting_saved_after_a_leave_stores_no_opera` fail with the key removal
  reverted, and `an_empty_opera_older_firmware_stored_is_not_loaded` (an
  all-zero pair, a zero id with a real secret, a real id with a zero secret)
  fails with the load refusal reverted. Spec §5.4 and §12.3 and the threat
  model say so. Host-tested only; the Arduino compile is CI's; not
  bench-verified (U1: the F113 row in `hardware_verification_checklist.md`).
  Found here: F137.
- [x] **F100 [code] A canary-wap pairing COMPLETE is sent once.** Neither side
  retransmits it, and the initiator does not check the send. So a COMPLETE
  lost on the air, or refused by the storm gate, leaves the initiator holding
  a member that never joined while the joiner times out. From code; not
  probed. Found while fixing F71-F76 (#1762).
  *Done (#1762):* the initiator keeps the COMPLETE it sent (the frame already
  on the air; the pairing key is still wiped), and `update()` sends it again
  every 2 s until the joiner is heard, for at most `PAIRING_TIMEOUT_MS` after
  the first send, which outlasts the joiner's own wait: at most 60 copies of a
  61-byte frame. Nothing on the wire acknowledges a COMPLETE, so "heard" means
  any verified fresh frame of the joiner's, judged against its last-seen
  counter when the COMPLETE went out (a joiner that took the COMPLETE sends
  its heartbeat within 30 s, F76; a re-pair's joiner, heard before, still gets
  copies). The copies stop early if the joiner is no longer a member or the
  opera rotated or was left. The send is checked: copies are counted, and a
  window that ends unanswered is logged once, as a COMPLETE that could not be
  sent when no copy went out. The pairing callback still reports success at
  the first COMPLETE, and one COMPLETE is kept at a time (a second pairing
  completing inside the window replaces it). The copies can reach the same
  joiner's next pairing, and `handle_pair_complete` ended a pairing on any
  COMPLETE it could not open, from any address (pre-existing: 61 bytes from
  any radio canceled a confirmed pairing). It now drops it and waits. Not
  covered: a replayed old frame of a re-added member counts as heard (F116).
  Pinned by nine `test_mesh_liveness_wap` tests:
  - `a_lost_complete_is_sent_again_until_the_joiner_is_heard`,
    `a_lost_complete_on_a_re_pair_is_sent_again` (the last-seen baseline),
    `an_unanswered_complete_stops_at_the_pairing_timeout_and_says_so`,
    `a_complete_the_storm_gate_refused_goes_out_when_it_reopens`,
    `a_complete_that_never_went_out_is_logged_as_that` and
    `a_complete_first_refused_then_sent_is_logged_as_unanswered` (the copy
    count), each failing with the resend or its piece reverted;
  - `a_complete_that_does_not_open_does_not_end_the_pairing` and
    `an_earlier_pairings_complete_does_not_end_a_later_one`, failing with the
    cancel put back;
  - `the_complete_is_not_sent_again_once_it_is_not_the_operas`, which pins the
    resend's stop conditions (the code before it sent no copy).
  Spec §5.2 and §8.3 say so. Host-tested only; the Arduino compile is CI's;
  not bench-verified (U1). Found here: F115 (by the review) and F116.
  *Since F116 (#1762):* a replayed old frame of a member re-added into the
  same opera no longer counts as heard: the re-add starts at the member's
  last-seen tombstone. One it signed after its removal still does (F140).
- [x] **F116 [code] canary-wap starts a re-added member's last-seen counter at
  0.** After removing the last member (no survivor, so nothing rotates and the
  `opera_id` stays) or after a leave, a recorded frame of that member,
  replayed from its address at the re-pair, is taken once, and it counts as the
  joiner heard, so it ends F100's COMPLETE resend early (host-probed in F100's
  review; the effect is a failed pairing, which a jammer achieves anyway).
  With NVS an older firmware wrote, or the open address-copy class (F72), any
  not-yet-heard genuine frame of the joiner can stop the resend the same way.
  The receive side is older than F100: `add_peer` has always started rx at 0,
  and F99 fixed only the send side. Keep a last-seen tombstone per removed
  fingerprint, as the PlatformIO tree does for a forgotten peer
  (`mesh_session.cpp`), and restore it at the re-add. Found by F100's review
  (#1762).
  *Done (#1762):* premise host-probed on the base first: A had heard B up to
  counter 5, removed it (its last member) and re-paired; A's last-seen for B
  was 0, B's recorded frame 1, replayed from B's address, ended A's COMPLETE
  resend, and B, which had lost the COMPLETE, was still waiting. A member
  dropped from the table (`remove_peer`, `leave_opera`) now leaves its
  last-seen counter as a tombstone, kept by fingerprint and by the `opera_id`
  it was dropped from (the PlatformIO tree keeps its tombstones by fingerprint
  alone, F139): at most eight, the oldest dropped first, in NVS `rx_tombs` (32
  B each, one write per removal or leave that changes them; not gated on flash
  encryption, like `replay_ctrs`). `add_peer` starts a member re-added into
  that opera there, on the initiator's side and on the joiner's (which adds
  its initiator under the opera it is joining, derived from the COMPLETE's
  secret, not the one it still holds), and `load_peers` does at boot (the
  re-added member's counter is not in `replay_ctrs` until the next 5-minute
  save). In any other opera the member's old frames drop at the `opera_id`
  check, so no tombstone applies there: a removal with a survivor rotates the
  opera at once (F95), and a first version that restored the tombstone
  whatever the opera kept a member whose counters restart out of the rotated
  opera for nothing (review, probed). A member never heard leaves none. The
  re-add does not consume the tombstone: the member's next removal raises it
  if the member was heard above it, and releases it if not. That release keeps
  a legitimately reflashed device from being shut out for good. One whose NVS
  was erased has a new key, so a new fingerprint, and no tombstone applies
  (pinned); one reflashed with its NVS kept resumes its send counters above
  everything it signed (F71), so above its tombstone (pinned); one that kept
  its key but lost its `tx_ctrs` record restarts at 1, below its tombstone, so
  it is not heard after a re-pair into the same opera, and removing it again
  before it is heard releases the tombstone; the next re-pair (after the 7-day
  deny-list grace) starts it at 0 and it is heard (pinned), which reopens the
  window for that member. Every member on firmware from before F71 is in that
  class (it starts its counters at 1 at every boot and every add): after a
  removal and a re-pair into the same opera it stays unheard until it is
  updated or released that way, so update it first. Pinned by ten
  `test_mesh_liveness_wap` tests:
  `a_replayed_frame_does_not_end_a_re_added_members_complete_resend`,
  `a_removed_members_last_seen_outlives_a_reboot`,
  `a_device_that_left_keeps_its_members_last_seen` (the joiner's side) and
  `a_device_that_left_keeps_its_members_last_seen_across_a_boot`,
  `a_tombstone_is_not_restored_in_a_rotated_opera`,
  `a_joiner_restores_a_tombstone_only_in_the_opera_it_joins`,
  `the_tombstones_are_bounded_and_the_oldest_goes`,
  `a_tombstone_is_raised_by_what_was_heard_since_the_re_add`,
  `a_device_whose_counters_went_back_is_released_by_a_second_removal` (the key
  is removed, and a boot does not bring the tombstone back) and
  `a_reflashed_device_with_a_new_key_is_heard_at_once` (a guard; it held
  before too). Each piece reverted alone fails at least one of them: the
  restore in `add_peer`, the floor in `load_peers`, the retire in
  `remove_peer` and in `leave_opera`, the release, the eviction, the raise,
  the boot read, the NVS write at a removal and at a leave, the empty set's
  key removed rather than written empty, the opera in the lookup, the joiner's
  lookup under the opera it joins, the leave's retire before the id is
  cleared, the `opera_id` stored and persisted. The harness's Preferences stub
  now refuses a zero-length `putBytes`, as Arduino-ESP32 does, and
  `test_mesh_rx_gates_wap`'s source pin follows. Not covered: NVS from before
  F116 holds no tombstone, so a member dropped under older firmware re-adds at
  0; a frame the member signed after its removal (it does not know of it, and
  goes on signing above its tombstone) is fresh at a re-pair into the same
  opera and still ends the COMPLETE resend (F140), as do a genuine frame it
  sent another member that this device has not heard (F72) and one heard since
  the last 5-minute last-seen save before a reboot; on a board without flash
  encryption a boot loads no opera and no member (spec §5.5), so the members
  it held leave no tombstone and a re-pair into the same opera starts them at
  0 (F141); a ninth drop evicts the oldest tombstone, whose window reopens at
  its next re-add. Spec §3.3, §4.2, §5.2 and §12.3 and the threat model say
  so. Host-tested only; the Arduino compile is CI's; not bench-verified (U1:
  the F116 row in `hardware_verification_checklist.md`). Found here: F139,
  F140 and F141.
- [x] **F137 [code] canary-wap leaves a removed member's key, radio address
  and name in NVS.** `persist_peers()` writes `peer_0`..`peer_<n-1>` and
  `peer_cnt` and never removes a key, so after a removal the last slot's old
  `peer_<n>` stays, and after a leave every `peer_<i>` does (only `peer_cnt`
  drops to 0). Host-probed on #1762's harness: after A removed C, `peer_1` was
  still in A's NVS; after A left, `peer_0` was. Nothing loads them
  (`load_peers` reads `peer_cnt` entries), but they name the household's
  former members on the flash, unencrypted even on a fused board (spec §5.5).
  Fix: remove `peer_<i>` for every i at or above the new count in
  `persist_peers()`. Found while fixing F113 (#1762).
  *Done (#1762):* premise host-probed on 7f45142 first (the new tests run
  against it): after a removal of the member in the last slot, that `peer_<i>`
  stayed in NVS, and after a leave every `peer_<i>` did. `persist_peers()` now
  writes the live slots in order, then `peer_cnt`, and only when both are
  stored removes every `peer_<i>` at or above the count, asking `isKey()`
  first (Arduino-ESP32's `Preferences::remove()` of an absent key logs an
  error-level line). The order is the review's: a full NVS refuses a set and
  still erases, so with the count written first a refused count stood over
  removed slots, and a boot loaded the removed member back, lost a survivor
  and loaded an all-zero member (host-probed at 65a159c, the build before the
  review). A save now stops at the first refused slot and answers whether the
  list was stored, and `load_peers()` loads no slot that does not read whole.
  A boot also removes what older firmware left, since only a membership change
  saved the list: the slots above the stored count and, with flash encryption
  on and no opera to load, every slot an older leave kept, saved as a leave
  saves now; each logs once and a second boot finds nothing. On a board with
  flash encryption off the stored members stay until its next membership
  change: what such a boot does with them is F141's decision. The same leak,
  by fingerprint, was in the two counter records: `replay_ctrs` kept a removed
  member's entry until the sketch's next 5-minute save, and `tx_ctrs` kept
  every former member's once none was left. `remove_peer()` and
  `leave_opera()` now save `replay_ctrs` at once (removed when no member is
  left), and with no member `tx_ctrs` is one entry under an all-zero
  fingerprint holding `g_tx_high_signed`, which the reader takes as before
  (F99). What a dropped member leaves on purpose, by fingerprint only: its
  deny-list entry (spec §5.6, seven days, flash-encryption gated) when it was
  removed, and its F116 last-seen tombstone when it was heard. The mesh_net
  Preferences stub follows Arduino-ESP32's `remove()` and `isKey()`
  (Preferences.cpp is byte-identical from 3.3.8, PlatformIO's pin, to 3.3.12)
  and can refuse the writes to chosen keys, as a partition that fills part way
  through a save does. Tests (`test_mesh_liveness_wap`):
  `a_removed_member_leaves_no_slot_behind`,
  `a_device_that_left_holds_no_member`,
  `removing_the_last_member_keeps_only_the_counter_floor`,
  `a_refused_save_leaves_a_list_a_boot_reads_whole`,
  `an_unreadable_slot_is_not_loaded_as_a_member`,
  `a_boot_removes_the_slots_an_older_removal_left`,
  `a_boot_removes_the_members_an_older_leave_left` and
  `a_board_without_flash_encryption_keeps_its_stored_members`, plus a check in
  `test_mesh_address_wap`'s fold test. All but the last fail on 7f45142 (the
  last holds what F137 leaves to F141), and each of twenty one-piece mutations
  fails at least one. Spec §3.3 and §12.3, the threat model and two U1 rows in
  `hardware_verification_checklist.md` say so. Host-tested only; the Arduino
  compile is CI's; not bench-verified (U1).
- [ ] **F139 [decision] The PlatformIO tree's last-seen tombstones have no
  release and no opera scope.** `mesh_session.cpp` keeps a dropped peer's
  tombstone by fingerprint alone, re-applies it whenever that key is
  registered again, whatever the opera (after a rotation too, where the peer's
  old frames already fail the `opera_id` check), and parks its counter again
  at the next drop. So a device whose outbound counter went back while it kept
  its key (`mesh_out_ctr` lost, the identity key kept) drops at that tombstone
  after every re-pair until eight newer drops evict it. canary-wap since F116
  keeps a tombstone per fingerprint and `opera_id`, restores it only at a
  re-add into that opera, and releases it at a removal of a member not heard
  above it since its re-add, which reopens the window for that member. Decide
  whether the PIO tree does the same (either or both), or both trees keep
  their tombstones for good. Read from code, not probed. Found by F116
  (#1762).
- [ ] **F140 [code+decision] A removed member's frames signed after its
  removal still end canary-wap's COMPLETE resend at a re-pair into the same
  opera.** A removal is one-sided: the removed device keeps the opera and goes
  on signing heartbeats to its remover, above the last-seen tombstone the
  remover kept (F116), until its own owner removes the remover. One of those,
  recorded and replayed from its address at a re-pair into the same opera (a
  removal of the last member rotates nothing), is fresh and counts as the
  joiner heard, so the initiator stops resending the COMPLETE while the
  joiner, which lost it, times out. Host-probed by F116's review on the real
  `mesh_network.cpp` (a heartbeat at counter 6 above tombstone 5, and the gap
  frame of two owners who removed each other a minute apart, each ended the
  resend; in the second, B failed to `MESH_NO_OPERA` while A held B). Fix
  shape: end the resend only on a frame that proves the joiner holds this
  COMPLETE (an acknowledgement under the pairing session key, a wire change),
  or on a frame signed under the joiner's new pairing; the decision is the
  wire change. Found by F116's review (#1762).
- [ ] **F141 [decision] canary-wap on a board without flash encryption drops
  every member at every boot, keeps no tombstone for them, and does not
  implement spec §5.5 items 1-2.** `load_opera_config()` refuses on an FE-off
  board, so `init()` never calls `load_peers()`: every member leaves the table
  at every boot without passing through `retire_rx()`, and the next 5-minute
  save with no member removes `replay_ctrs`. A re-pair into the same opera (as
  a joiner of a member that kept it) then starts each member at 0, and its
  recorded frames are fresh again (F116's review, probed on the real code).
  Nothing in canary-wap gates pairing on flash encryption
  (`flash_encryption_enabled()` is read only in the opera-config and deny-list
  persist and load), unlike §5.5's items 1 (refuse to create an opera) and 2
  (refuse to consume a `PAIR_COMPLETE`). Decide whether canary-wap implements
  §5.5 items 1-2, retires the stored members' last-seen counters as tombstones
  at an FE-off boot (from `peer_<i>` and `replay_ctrs`, both unencrypted
  already), or both, or whether the spec changes. Read from code; spec §4.2
  and §12.3 and the threat model state the gap since #1762. Found by F116's
  review (#1762).
  *Since F137 (#1762, wave 12):* a canary-wap boot removes the member slots an
  older firmware left only with flash encryption on. On a board without it the
  stored members (`peer_<i>`, plaintext keys and radio addresses, and their
  fingerprints in `tx_ctrs` and, until the next 5-minute save, `replay_ctrs`)
  stay until the board's next membership change, on purpose, because one of
  this item's options reads them to retire each member's last-seen counter as
  a tombstone. Whatever is decided here also decides whether such a boot
  removes them afterwards (`test_mesh_liveness_wap`'s
  `a_board_without_flash_encryption_keeps_its_stored_members` pins today's
  answer).
- [x] **F101 [code] PIO opera broadcasts reach a running pairing's partner and
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
  only those. Found while fixing F70 (#1762).
  *Done (#1762):* reproduced on wave 9's code: `send_tamper_alert` returned
  true and `leave_opera` reported `notified`, each with only the outsider's
  copy sent; beacon events, channel locks and hub elections did the same. The
  PIO session now sends every opera frame through `send_to_members`: one
  unicast to each trusted peer's bound `radio_mac`, and nowhere else. That
  covers tamper alerts, beacon events, channel locks, hub elections, LEAVE and
  the rekey OFFER, plus a rekey unicast's fallback when its destination is
  unbound. Each sender's return and `leave_opera`'s `notified` count only the
  sends the transport took. A member with no binding is sent nothing; since
  F70 it is heard from nowhere. `mesh_transport::broadcast()` is unchanged and
  the session no longer uses it. Pinned by
  `test_opera_sends_with_only_a_pairing_partner_reach_nobody` and
  `test_opera_sends_reach_bound_members_only` (the four senders that report a
  result, the rekey OFFER's retransmit and the LEAVE), both failing on the
  code before; the rekey assertion also fails with only the rekey sender
  reverted. `test_opera_sends_count_only_what_the_transport_took` refuses one
  member's sends, then both: the senders report sent only while one copy was
  taken, and `leave_opera` reports not notified when none was; it fails when
  `send_to_members` counts attempts (mutation). Eight existing tests that
  hand-added a transport address as a send target, and the `boot_device`
  helper, now bind a member there; `test_rest_request_slot` also keeps a bare
  transport address, which pins the REST LEAVE's `clear_peers()` (it fails
  with that call deleted). Spec §8.3 (with leave's `notified`) and
  THREAT_MODEL are updated. Host-tested only; the `[env:full]` compile is
  CI's; not bench-verified (U1).
- [x] **F102 [code] PIO: a pairing whose radio-MAC bind the session refused is
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
  Found in F70's review (#1762).
  *Done (#1762):* both halves of the fix, each enough for the probe on its
  own:
  - the session reports its bind. `NOTIFY_PAIRED` runs the PairedCallback,
    then `end_pair_contact` (which now returns the bind's result), then a new
    `PairedPeerBoundCallback(fp, mac, bound)` (`set_paired_peer_bound_callback`).
    `main.cpp` persists the address from there (`on_mesh_paired_peer_bound`),
    only when bound, and logs a refused bind by fingerprint as a health
    WARNING. `register_paired_peer` no longer touches the address;
  - `peer_mac_blob::upsert` refuses an address another fingerprint holds,
    leaving the blob untouched, so `save_peer_mac` does too.
  A refused bind keeps the member's previous binding in RAM and NVS; a re-pair
  the session does bind is stored in place. Pinned by:
  - `test_paired_peer_bound_reports_the_bind`: true for a new member; false
    for a re-pair from another member's address and for a member the callback
    could not register; always after the PairedCallback;
  - `test_refused_repair_bind_is_not_persisted_across_reboot`: the item's probe
    end to end, with a stand-in for `main.cpp`'s wiring on a fake NVS. After
    the reboot, C is heard from its own address and J from its old one;
  - `test_successful_repair_is_persisted_across_reboot`: J re-pairs from a
    free address (a swapped radio), then again from it; both report bound,
    NVS holds J there in one entry, and after the reboot J is heard there and
    not at its old address. It fails if a re-pair of an already-trusted member
    reads as refused (mutation);
  - a new case in `test_mesh_state`'s `test_peer_mac_blob`, which fails on the
    old upsert;
  - `scripts/tests/test_canary_mesh_pair_bind_wiring.py`, a source pin on
    `main.cpp`'s wiring with self-test mutations (no host build compiles
    `main.cpp`). It fails on wave 9's `main.cpp`.
  With the address saved whatever the bind and the old upsert (the code
  before), the two refused-bind session tests fail; with either half alone,
  the reboot test passes. A test-only hook,
  `mesh_transport::test::reset_storm_limiter`, keeps the session suite's tests
  out of each other's send history. Spec §8.3 and §12.3 and THREAT_MODEL are
  updated. Host-tested only; the `[env:full]` compile is CI's; not
  bench-verified (U1). Found here: F118-F120.
- [x] **F118 [code] A PIO pairing whose radio-MAC bind is refused still
  completes and trusts the partner.** The initiator seals the opera_secret
  into its COMPLETE before anything is bound. On both sides, `main.cpp`'s
  PairedCallback then registers the partner's key and persists it, and only
  after that does `end_pair_contact` try the bind. So a partner the session
  cannot bind (an address another member holds, or a full transport table)
  becomes a trusted member that is heard from nowhere (F70) and sent nothing
  (F101), and it holds a slot until removed. Since F102 its address stays out
  of NVS and the refusal is logged. Spec §5.2 says a device that cannot hold
  its partner fails the pairing; canary-wap does that since F73, adding the
  joiner before anything is sent. Fix: check the bind before the COMPLETE
  (initiator) and before installing the secret (joiner), and fail the pairing
  on a refusal. From code and the F102 tests; not probed further. Found while
  fixing F102 (#1762).
  *Done (#1762):* the premise holds: on wave 10's code a re-pair of member J
  from member C's address, and a new member while eight were trusted, both
  completed (each side NOTIFY_PAIRED, the COMPLETE sealed and sent), and only
  then was the bind, or the register, refused;
  `test_paired_peer_bound_reports_the_bind` pinned exactly that. Now
  `mesh_pairing` takes a `PartnerGate` at `start_initiator`/`start_joiner`,
  asked at this side's owner's confirm before any CONFIRM goes out (both
  roles), on the initiator again in `initiator_complete` before it seals, and
  on the joiner again in `joiner_handle_complete` before it opens the
  COMPLETE. A refusal is NOTIFY_FAILED with `FailReason::PARTNER_REFUSED`, and
  nothing is sent, sealed, opened or stored; every NOTIFY_FAILED now carries a
  reason (canceled, timeout, bad_confirm, bad_complete, crypto,
  partner_refused). The session hands every pairing
  `mesh_session::can_hold_partner`, which refuses what `register_trusted_peer`
  and `bind_peer_mac` would: a deny-listed key, a new member for a full opera,
  an address another member is bound to, a broadcast, group or zero address,
  no transport room. The FailedCallback now takes the reason and the partner's
  fingerprint; `main.cpp`'s `on_mesh_pairing_failed` logs a refusal as a
  health WARNING ("Opera pairing refused", by fingerprint);
  `confirm_pairing_code` returns false when the confirm ended the pairing, and
  `POST /api/mesh/pair/confirm` answers `409 partner_refused`. One difference
  from canary-wap's F73: the check also runs at the confirm, so a refusing
  joiner sends no CONFIRM and its initiator never seals the secret to it or
  keeps it (it times out). Correction to the item: the full-transport-table
  case is reached only by a joiner whose own ACCEPT could not be sent for want
  of room (its initiator never shows a code); an initiator's partner, and a
  joiner's that got its ACCEPT out, already sits in the table as the pair
  contact. Pinned by four `test_mesh_pairing` tests
  (`test_a_refused_partner_fails_at_the_owners_confirm`, both roles;
  `test_the_initiator_seals_nothing_to_a_refused_partner`, both orders;
  `test_the_joiner_opens_nothing_from_a_refused_partner`;
  `test_every_failure_says_why`) and three `test_mesh_session` tests:
  `test_a_partner_the_initiator_cannot_hold_fails_the_pairing` (a re-pair from
  another member's address and a new member for a full opera, both confirm
  orders, through the REST slot: nothing sent to the partner, neither paired
  callback, the fake NVS unchanged, J and C still heard),
  `test_the_initiator_asks_again_before_it_seals` and
  `test_a_joiner_that_cannot_hold_its_initiator_fails_the_pairing` (at the
  confirm and at the COMPLETE). `test_can_hold_partner_refusals` asks the gate
  directly, one refusal at a time beside a control the same setup admits, and
  `test_cancel_pairing_fires_failed_callback` pins the reason and a null
  partner when none was named. The `scripts/tests` wiring pin holds
  `on_mesh_pairing_failed` by its exact form (it returns early, with no health
  log, for every reason but PARTNER_REFUSED, then logs the refusal once,
  unguarded, naming the partner) and holds it installed. F102's
  `test_paired_peer_bound_reports_the_bind` now expects those two cases to
  fail the pairing; the bound callback's `false` stays pinned for a
  PairedCallback that registered nothing. Each check fails by mutation: the
  gate off in the session (either role), off in `confirm_code`,
  `initiator_complete` or `joiner_handle_complete`, and `can_hold_partner`
  without any one of its five checks (until the review found it, removing the
  deny-list, transport-room or unicast check left every suite green); the
  pin's self-tests show it failing on the refusal test inverted, a return
  before the log, a guarded log, and the other reasons logged. Spec §5.2 and
  §8.3 and THREAT_MODEL are updated. Host-tested only; the `[env:full]`
  compile is CI's; not bench-verified (U1). Found here: F133, F135 and F136.
- [x] **F119 [code] PIO: a `peer_macs` blob written before F102 can hold one
  address under two fingerprints.** The boot restore binds entries in blob
  order, so the first takes the address and the second is refused; in F102's
  scenario, the member that owns the address stays unheard. F102 stops new
  duplicates (only bound addresses are saved, and `upsert` refuses a held one)
  but does not repair a stored one, and the blob cannot tell which entry is
  right (an in-place update keeps the first pairing's position). Options: at
  boot, bind neither entry of a shared address and log it, so both members
  re-pair; or drop both entries. From code; not probed. Found while fixing
  F102 (#1762).
  *Done (#1762):* both entries are dropped, and neither is bound.
  `mesh_session::restore_peer_macs` classifies every stored entry before it
  binds any; two registered fingerprints' entries at one address are SHARED,
  and `main.cpp`'s boot restore drops both from NVS and logs the address as a
  health WARNING by fingerprint, so both members are heard from nowhere until
  each re-pairs. Why not keep them unbound: the other entry would still hold
  the address in NVS, and `peer_mac_blob::upsert` refuses an address another
  fingerprint holds (F102), so the owner's re-pair would be bound for that
  boot and not stored, and it would be unheard again after the next reboot;
  dropping both lets the re-pairs be stored, and logs once. Pinned by
  `test_boot_restore_binds_neither_member_of_a_shared_address`: the blob
  F102's scenario left (J's entry first, then C's, both at C's address); after
  a reboot neither is heard from that address, nobody holds it in the
  transport table and the blob is empty; each member re-pairs and is stored,
  and after another reboot C is heard at its address and J at X. It fails with
  the SHARED classification removed, which is blob-order binding (the code
  before). Which entries go is
  `mesh_session::stored_mac_must_drop(verdict, peers_loaded)`, true exactly
  for SHARED and UNTRUSTED when the pubkey list was read;
  `test_stored_mac_must_drop_truth_table` holds all eight cases. `main.cpp`'s
  loop asks it and nothing else, and the `scripts/tests` pin holds that loop,
  the register block before `restore_peer_macs` and the SHARED branch's health
  log by their exact form, with no `bind_peer_mac` of its own; its self-tests
  show it failing on every inverted or neutralized form the review found
  passing (a drop of every bound entry, `continue` turned into `{}`, `true`
  for `peers_loaded`, the pubkeys registered only when the read failed). Spec
  §8.3 and §12.3 and THREAT_MODEL are updated. Host-tested only; the
  `[env:full]` compile is CI's; not bench-verified (U1).
- [x] **F120 [code] PIO: a `peer_macs` entry whose fingerprint is no longer
  trusted is never pruned, and since F102 it blocks storing that address for
  any other member.** `remove_trusted_peer` drops a member's entry only best
  effort, and the boot restore binds nothing for an untrusted fingerprint but
  leaves the entry stored. So after a failed NVS removal, a member that pairs
  from that address is bound for the boot but not stored (`save_peer_mac`
  refuses, and `main.cpp` warns); after a reboot it is heard from nowhere, and
  a re-pair from that address hits the same refusal. Fix: at boot, drop
  entries whose fingerprint is not a registered peer. From code; not probed.
  Found while fixing F102 (#1762).
  *Done (#1762):* at boot `restore_peer_macs` calls an entry whose fingerprint
  is not a registered peer UNTRUSTED and never binds it, and `main.cpp` drops
  it from NVS (a Serial line), but only when `load_trusted_peers` succeeded: a
  failed read registers nobody, and every entry would look untrusted. A
  deny-listed key whose pubkey is still in NVS is not registered (F33 part 6),
  so its entry goes too. An untrusted entry at a member's address does not
  make the member's entry SHARED (F119): a non-member cannot own it. Pinned by
  `test_boot_restore_drops_entries_of_peers_no_longer_trusted`: a removed
  member's entry at one address and another's at a member's address are
  dropped and the member bound; with the stale entry kept, a new member's
  address there could not be stored (upsert refuses it); after the drop a new
  member pairs from that address, is stored, and is heard there after a
  reboot; a boot that could not read the pubkey list drops nothing. It fails
  with UNTRUSTED kept (as REFUSED) and with an untrusted entry counted as
  sharing; `test_stored_mac_must_drop_truth_table` holds the drop decision for
  every verdict and both values of `peers_loaded`,
  `test_boot_restore_keeps_a_refused_entry` keeps a member's entry that the
  bind refuses (REFUSED, as before), and the `scripts/tests` pin (27 cases)
  holds `main.cpp`'s loop to that decision by its exact form. Not closed, as
  the item says: between a failed removal and the next boot a stale entry
  still blocks storing its address; the next boot repairs it. Spec §8.3 and
  §12.3 and THREAT_MODEL are updated. Host-tested only; the `[env:full]`
  compile is CI's; not bench-verified (U1).
- [x] **F133 [code] The PIO web UI reports a failed pairing as complete on the
  initiator.** `startPairingPolling()` (`securacv_webui.cpp`) reads `ACTIVE`
  or `CONNECTING` from `GET /api/mesh` as "Pairing complete", and an initiator
  that was already in an opera returns to exactly those states after any
  failure: a timeout, a refusal at the seal (F118), a cancel. Since F118 a
  refusal at the confirm answers `409 partner_refused`, which the confirm
  button's alert shows, and every refusal is in the health log; the status
  poll still cannot tell. Fix: report the last pairing's outcome
  (`mesh_session::pairing_fail_reason()`, added by F118) in `GET /api/mesh`
  and read it in the poll. From code; not probed. Found by F118 (#1762).
  *Done (#1762):* reproduced: wave 11's page, driven in Chromium against a
  mocked API (scratch probe), alerted "Successfully joined opera!" on a Canary
  already ACTIVE in an opera whose pairing timed out. `GET /api/mesh` now ends
  with three fields, after every older one (the body without them is the old
  body byte for byte): `pairing_seq` (pairings started since boot; 0: none),
  `pairing_result` (`none`, `running`, `paired`, `failed`; an initiator reads
  `running` from its COMPLETE until its NOTIFY_PAIRED has registered the
  member) and `pairing_fail_reason` (F118's reason, `none` unless failed).
  POST `pair/start` and `pair/join` answer the `pairing_seq` they started. The
  handler's buffer is `mesh_api::STATUS_JSON_CAP`, 640 bytes: the widest body
  is 517, past the 512 it had. `startPairingPolling()` reads the fields
  through `pairingVerdict()`: a failure is reported as one, with its reason in
  words; a success once, in the words of the side this Canary played; a body
  about another pairing (another number, or 0 after a restart) ends the poll
  without claiming either; a body without the fields claims neither. Three
  fixes in the same poll: a 000000 code is shown (the old truthiness test hid
  it), the confirm button is not offered again after this owner's confirm, and
  a confirm answered `409 partner_refused` says so once and stops the poll.
  Pinned by `test_mesh_session`'s
  `test_build_mesh_status_json_reports_the_last_pairing`,
  `test_status_json_fits_worst_case` and
  `test_get_mesh_tells_each_pairing_outcome` (a timeout, a REST cancel, a
  refusal at the confirm, a success reading `running` until its NOTIFY_PAIRED,
  a joiner's success, a start and a join refused while a pairing runs naming
  no number and leaving the count alone, a reboot starting the count again);
  by `firmware/tests_host/test_canary_mesh_pairing_poll.test.js` (9 cases, run
  by firmware.yml's mesh host-test step), whose first case fails on the old
  page with "Successfully joined opera!"; and by
  `scripts/tests/test_canary_mesh_status_wiring.py`, a source pin of the
  handlers, which only the `[env:full]` build compiles (all five of its rules
  fail on the handler at 7f45142). `build_mesh_status_json`'s report parameter
  has no default, so a handler call without it does not compile. Each check
  fails by mutation (nine in the C++, seven in the page; one more page mutant
  is equivalent). The fields are read from the HTTP task without a lock, as
  the rest of this tree's status is (F161). *Since F161 (#1762, wave 13):*
  they come, with the rest of the body, from a view the main loop publishes
  after each pass and each request it runs. Spec §8.3 and THREAT_MODEL are
  updated. Host-tested, and the page in Chromium against a mocked API; the
  `[env:full]` compile is CI's; not bench-verified (U1). Found here: F161 and
  F163.
- [x] **F134 [code] PIO: a lost `PAIR_COMPLETE` leaves the initiator holding a
  member that never joined.** This is canary-wap's F100 case. The PIO
  initiator sends its COMPLETE once, behind its own CONFIRM, and reports
  PAIRED, so `main.cpp` registers, binds and stores the joiner. If the
  COMPLETE is lost on the air, the joiner's re-sent CONFIRMs (F117) reach an
  initiator already PAIRED, which drops them, and the joiner fails at its
  timeout. Host-probed on the pure library in scratch: the initiator notified
  PAIRED, the joiner re-sent its CONFIRM three times, nothing answered, and it
  stayed in `AWAITING_COMPLETE` to its timeout. Fix: canary-wap's F100 shape
  (send the COMPLETE again until the joiner's first verified frame, for at
  most the pairing timeout), which means keeping the sealed frame after the
  session key is wiped. Found by F117 (#1762).
  *Done (#1762):* reproduced: a scratch interop probe ran wave 11's
  `mesh_pairing` as the initiator against itself, f89e7e5 (wave 11 before
  F117), ba1892e (wave 10) and the pre-F97 state machine of 4e77f0d as
  joiners, frames crossing as bytes on one clock, both orders, the owners'
  confirms 0 s to 4 minutes apart. With the first COMPLETE (or it and the
  CONFIRM in front of it) lost, the initiator reported PAIRED in all 16
  combinations and the joiner timed out. canary-wap's F100 shape, in the pure
  library: the initiator keeps the two frames it sent, its leading CONFIRM and
  the sealed COMPLETE, byte for byte (`PairingContext::kept_*`; the session
  key stays wiped and nothing is sealed again), and `tick()` sends both again
  every `COMPLETE_RESEND_INTERVAL_MS` (2 s), to the partner only, for at most
  `COMPLETE_RESEND_WINDOW_MS` (the 5-minute pairing timeout) after the first
  send: at most 149 copies. `stop_complete_resend()` ends them and wipes the
  frames. The session calls it when the member is heard (its first verified
  fresh frame under this opera, from its own address, after the address,
  signature, opera and counter checks) and, before each pairing tick, when the
  member is no longer trusted and bound at the address it paired from
  (removed, left, rotated out, a failed bind, bound elsewhere) or this device
  no longer holds the opera the COMPLETE carried; a new pairing ends them with
  its context. Every copy carries the leading CONFIRM, so a pre-F97 joiner
  that lost both frames completes too. Decided: a joiner's re-sent CONFIRM
  reaching a PAIRED initiator still prompts nothing (the next copy is at most
  2 s away, and the CONFIRM authenticates nothing a radio in range could not
  replay or reflect, F94); pinned. On the probe with the change, an updated
  initiator pairs every joiner (updated, pre-F117, wave 10, pre-F97) in both
  orders with no loss, the COMPLETE lost, or both frames lost; an older
  initiator with a loss still fails as before, which only updating the
  initiator repairs. Differences from canary-wap: PIO members send nothing on
  a timer, so a joiner that took the COMPLETE is usually not heard and the
  copies run their whole window (F162); a window's end is not logged; and the
  copies' sends are not checked or counted, as no PIO pairing send is. A side
  effect on F94: a joiner whose owner confirms after the initiator completed
  on its own reflected CONFIRM, inside the window, now takes a copy and joins;
  one whose owner never confirms still leaves the initiator holding a member
  that never joined. Not changed (the protocol review's probe; the same before
  F134): a member that re-pairs into the opera it already holds (F136's path)
  and sends an ordinary frame before the first copy is due ends the copies, so
  with its first COMPLETE lost its pairing still times out. Pinned by six
  `test_mesh_pairing` tests (among them
  `test_a_lost_complete_is_sent_again_until_the_joiner_takes_it`,
  `test_the_complete_copies_are_bounded`,
  `test_a_lost_complete_reaches_a_pre_f97_joiner` and
  `test_frames_reaching_a_paired_initiator_send_the_secret_nowhere_else`) and
  five `test_mesh_session` tests (among them
  `test_complete_copies_stop_when_they_can_no_longer_help`,
  `test_only_the_joiners_own_fresh_frame_ends_the_copies`, where a forged,
  relayed, other-opera or replayed frame under the joiner's fingerprint ends
  nothing, since the joiner's key goes out in clear in its DISCOVER, and
  `test_a_leave_or_removal_in_a_copys_pass_sends_no_copy`). Each piece fails
  by mutation; across F133-F135, 30 C++ mutants, all killed. Spec §5.2 and
  THREAT_MODEL are updated. Host-tested only; the `[env:full]` compile is
  CI's; not bench-verified (U1). Found here: F162 and F163.
- [x] **F135 [code] PIO: `mesh_pairing::cancel()` ends a pairing that already
  ended.** It fails every state but IDLE. The REST `pair/cancel` runs at the
  start of `process()`, before the pairing tick, so one that lands after the
  initiator's COMPLETE went out (the joiner's CONFIRM arrived in the transport
  pass just before) turns the PAIRED context into FAILED: the joiner has the
  secret, but NOTIFY_PAIRED never fires, so the initiator never registers,
  binds or stores it. On a FAILED context it fires NOTIFY_FAILED, and the
  FailedCallback, a second time. Host-probed on the pure library in scratch:
  `cancel()` after SEND_COMPLETE returned NOTIFY_FAILED (canceled) and the
  next tick NONE; on FAILED it returned NOTIFY_FAILED again. Fix: make
  `cancel()` a no-op on PAIRED and FAILED. Found while fixing F118 (#1762).
  *Done (#1762):* reproduced by the new tests on wave 11's code: `cancel()`
  after the initiator's SEND_COMPLETE returned NOTIFY_FAILED (canceled) and
  the deferred NOTIFY_PAIRED never came, so through the session's REST slot
  the FailedCallback ran in place of the PairedCallback and nothing was stored
  while the joiner opened the COMPLETE; on FAILED it returned NOTIFY_FAILED
  again and overwrote the reason with `canceled`. `mesh_pairing::cancel()` now
  ends only a running pairing: it is a no-op (NONE) on IDLE, PAIRED and
  FAILED. A cancel during F134's COMPLETE copies leaves them running: the
  pairing is over and, with its first COMPLETE lost, the copies are the
  joiner's only way in. `pair/cancel` still answers `{ok: true}`. Pinned by
  `test_mesh_pairing`'s
  `test_a_cancel_after_the_complete_leaves_the_pairing_paired` (both orders,
  the COMPLETE delivered and lost: the copies still run after the cancel and
  the joiner opens the next one),
  `test_a_cancel_on_a_failed_pairing_reports_nothing_again` (four reasons,
  each kept) and `test_a_cancel_with_nothing_running_does_nothing` (a
  regression pin), and `test_mesh_session`'s
  `test_a_cancel_after_the_complete_still_reports_paired` (through the REST
  slot: the PairedCallback runs, the member is registered, bound and stored,
  no FailedCallback; a second cancel leaves the copies going) and
  `test_a_cancel_on_a_failed_pairing_fires_nothing_again`. With the old
  `cancel()` both pairs fail; with PAIRED or FAILED left out of the no-op its
  pair fails; and a cancel that wipes the kept frames, or a session cancel
  that stops the copies first, fails the lost-COMPLETE branches. Spec §5.2 and
  THREAT_MODEL say so. Host-tested only; the `[env:full]` compile is CI's; not
  bench-verified (U1).
- [ ] **F136 [code+decision] PIO: `pair/join` on a Canary already in an opera
  keeps the old opera's members.** `execute_request`'s PAIR_JOIN does not
  check `s_opera_id_set`, and the joiner's PairedCallback (`main.cpp`)
  installs the new opera_secret over the old one while every old trusted peer
  stays registered, bound and stored. Their frames then fail the opera_id
  check, so they stay listed as members that are never heard, and they count
  against the new opera's eight slots (F118's joiner refusals are reached this
  way). Decide whether a join requires leaving first (refuse PAIR_JOIN with an
  opera, as the web UI's join button suggests) or replaces the opera (clear
  the old members and their addresses). From code; not probed. Found while
  fixing F118 (#1762).
- [x] **F161 [code] PIO: `GET /api/mesh` and `/api/mesh/peers` read
  `mesh_session`'s state from the HTTP task.** canary-wap's F110 counterpart.
  `handle_mesh_status` and `handle_mesh_peers` (`securacv_network.cpp`) read
  the pairing state and code, F133's pairing number, outcome and reason, the
  opera name and the trusted-peer links while the main loop's `process()`
  writes them, with no lock and no published view. Each 32-bit value is read
  whole, but one body can mix two loop passes (a pairing number with the
  outcome of a pairing started between the reads, a name read mid-rename).
  canary-wap reads a view its loop publishes (`loop_snapshot.h`, F110). From
  code; not probed. Found by F133 (#1762).
  *Done (#1762):* a view the main loop publishes, F110's shape through F110's
  header. `loop_snapshot.h` is shared, not ported: the PIO lib's copy
  (`firmware/canary/lib/securacv_mesh/src/loop_snapshot.h`) is canonical,
  `check_mesh_sync.sh` holds canary-wap's byte-identical, and each tree brings
  its own lock (on the device a portMUX critical section, the one
  `securacv_witness` and `ble_scout` take; on the host a `std::mutex`).
  `mesh_session` builds a `StatusView` (the enable switch, the opera's id and
  name, the pairing state, number, outcome and reason, the code only while
  `GET /api/mesh` shows it, `peers_total`, `peers_online`, `alerts_received`,
  and each trusted member by fingerprint with its alerts and, once heard from
  its binding, its transport entry's state, RSSI and last-seen time; no key)
  and publishes it (`publish_status()`): at the end of every `process()` pass
  and before its early return, in the drain after each REST request it ran and
  before the result is posted (so a GET right after a POST's answer shows the
  POST), in `deinit()`, and once from `main.cpp`'s `setup()` after the mesh
  restore, since the HTTP server is up before the first loop pass. Before any
  publish a read answers the state before `init()`, which is also what
  `init()` leaves. `handle_mesh_status` and `handle_mesh_peers` copy the view
  (`read_status()`) and read nothing else of the session or the transport: the
  status body is `mesh_api::build_mesh_status_json_from_view`, and the peers
  join moved out of the handler into the host-tested
  `mesh_api::peer_views_from_status`, the members still the persisted pubkeys
  (F199) and the age still counted at the read. Every field of both bodies is
  unchanged; after a pass they equal the old handlers' bodies byte for byte.
  Neither route waits for the main loop. Cost: 240 B of internal SRAM for the
  view plus its lock, and per pass a build of the view and a 240 B compare
  (the lock is taken only when the view changed). Pinned by eight new
  `test_mesh_session` tests:
  `test_a_read_before_any_publish_is_the_state_before_init`,
  `test_a_status_read_is_the_last_published_pass`,
  `test_a_read_right_after_a_post_shows_what_it_did`,
  `test_a_disabled_session_still_publishes_each_pass`,
  `test_deinit_publishes_the_wiped_session` (no byte of a cleared name
  copied), `test_the_view_holds_the_code_only_while_it_is_shown`,
  `test_the_peer_list_joins_each_member_from_the_view` (a member unheard past
  the transport's 5-minute threshold reads OFFLINE with its real age and RSSI,
  never CONNECTED) and the two-thread
  `test_status_reads_stay_whole_while_the_main_loop_runs`;
  `test_get_mesh_tells_each_pairing_outcome` now builds its body as the
  handler does. With `read_status()` answering the live state the last-pass
  test fails 3/3 and the two-thread test 5/5; each publish point removed alone
  fails a test; of 30 mutations of the view, the join and the builder, 29 are
  killed and the survivor is equivalent; the suite is clean under
  ThreadSanitizer, which flags the view with its lock removed.
  `firmware/scripts/check_canary_mesh_status.py` (25 self-test mutations, run
  by `regression_check.sh`) holds that no canary HTTP handler names a live
  session or transport reader (one allowance, `handle_mesh_alerts`'
  `get_alerts`: F197), that the two routes read the view once and the peers
  route joins it, where `mesh_session.cpp` publishes and reads, and
  `main.cpp`'s one publish in `setup()`;
  `scripts/tests/test_canary_mesh_status_wiring.py` pins the status handler's
  view read and its one from-view builder call. The two handlers, compiled
  verbatim on the host against stubs in scratch, answer the expected bodies;
  the `[env:full]` compile is CI's. Spec §8.1 says so. Host-tested; not
  bench-tested (U1: the F161 row in `hardware_verification_checklist.md`).
  Found here: F197 and F199.
- [ ] **F162 [code+decision] PIO: a joiner sends nothing after it pairs, so
  its initiator never hears it.** F134's COMPLETE copies end when the joiner
  is heard, and PIO members send no frame on a timer, so after a pairing that
  went well the copies run their whole 5-minute window (149 copies of two
  frames, which the joiner drops), the initiator's peer list shows the new
  member OFFLINE ("never"), and both sides read CONNECTING until one of them
  sends an opera frame (a tamper alert, a beacon event). canary-wap's members
  announce themselves (F76). Decide whether a PIO member sends a signed frame
  after it pairs, once or on a timer; the registry's HEARTBEAT (16) is one a
  PIO receiver already verifies and drops, marking its sender heard. From code
  and the F134 tests; not probed on a radio. Found by F134 (#1762).
- [x] **F163 [code] The kernel's mesh pairing wizard waits for both Canaries
  to read ACTIVE.** `privacy_witness_kernel/wizard/index.html`'s
  `meshConfirm()` polls `/api/mesh/status` (the Canary's `GET /api/mesh`)
  every 2 s for 60 s for both `state`s to be ACTIVE, and says "Pairing did not
  complete" otherwise. On the PIO tree ACTIVE needs a peer heard this boot,
  and a new member hears no one until someone sends an opera frame (F162), so
  a finished pairing reads as not completed. It can read `pairing_seq` and
  `pairing_result` now (F133). From code; not probed. Found by F133 and F134
  (#1762).
  *Done (#1762):* the wizard keeps the pairing number each Canary's
  `pair/start` or `pair/join` answered (F133; the add-on proxy passes the
  answers and the status bodies through unchanged) and reads that pairing's
  `pairing_result` from `GET /api/mesh`: both `paired` is a success whatever
  `state` reads, and the done screen adds that the two may not show each other
  connected until each hears from the other (F162); either `failed` stops the
  wait at once and names that Canary (existing or new) and the reason in words
  (a timeout, a cancel, `partner_refused`, `bad_confirm`, `bad_complete`,
  `crypto`, else the reason named); a body about another pairing (another
  number, or 0 after a restart) stops the wait claiming neither. Each of
  those, and either wait's timeout, cancels (best effort) a side whose latest
  read shows the wizard's pairing number still `running`, so the retry is not
  refused until that Canary's own 5-minute timeout (a Canary runs one pairing
  at a time, so that cancel ends no one else's); a side that finished is left
  as it finished, a timeout acts only on its final poll's reads, and a Canary
  that reports no number is not canceled. A Canary whose answer carries no
  number (from before F133, or a canary-wap, whose routes have no
  `pairing_seq`) is read the old way, done once it reads ACTIVE, also beside
  one that reports its outcome; a 0 is no number. The code-wait poll
  (`meshPollForCodes`) ends early the same way (a failure, or a pairing both
  owners confirmed on the Canaries themselves). The wizard's JS had no test:
  `privacy_witness_kernel/tests/test_wizard_mesh_pairing.test.js` (11 cases;
  `pwk-wizard-tests.yml` runs it) lifts the page's mesh section out by literal
  markers and drives it against a scripted proxy and an immediate `delay()`.
  Nine cases fail on the page before this change (the two that pass pin the
  old ACTIVE path), and 24 mutations of the new code each fail it. Not
  changed: a pre-F133 PlatformIO Canary still reads as not completed after a
  finished pairing (it reports no outcome and reads ACTIVE only once it hears
  a member, F162); a rejected confirmation, a confirm that fails on the
  network and five unreadable polls still leave the other side running (F200).
  Host-tested (node); not run against a Canary (U1: the F163 row in
  `hardware_verification_checklist.md`). Found here: F200.
  *Since F200 (#1762, wave 14):* a rejected confirmation, a confirm that fails
  on the network and five unreadable polls now cancel a side still running the
  wizard's pairing, from a fresh read of each side, and nothing beside a side
  that reports the pairing paired. A timeout still acts only on its final
  poll's reads (F215).
- [x] **F197 [code] PIO `GET /api/mesh/alerts` reads the alert history from
  the HTTP task.** `handle_mesh_alerts` copies `mesh_session::get_alerts()`
  (the 16-record ring `store_alert()` writes on the main loop's receive path
  and `clear_alerts()` empties through the request slot) in place, so a body
  can hold a record half overwritten by a newer alert or straddle a clear.
  F161 moved only `GET /api/mesh` and `/peers` to the published view;
  canary-wap's alerts route reads a `loop_snapshot::Log` its loop task changes
  under its lock (F110), and the PIO tree now has that header (the route's
  newest-first order has to survive the move: the `Log` reads in storage
  order). `check_canary_mesh_status.py` allows exactly this read (`ALLOWED`)
  until it moves. From code; not probed. Found by F161 (#1762).
  *Done (#1762):* the alert history is a `loop_snapshot::Log` in
  `mesh_session.cpp` with its own lock beside the status view's (a portMUX
  critical section on the device, a `std::mutex` on the host); the shared
  header is unchanged, so `check_mesh_sync.sh` still holds canary-wap's copy
  byte-identical. The receive path builds each record whole and appends it
  under the lock (the oldest overwritten once 16 are held); `clear_alerts()`
  (the REST DELETE, run by the main loop's request drain) and `reset_alerts()`
  (`deinit()`, `leave_opera()`) clear it under the lock. `read_alerts()` (new;
  `get_alerts()` is gone) copies the whole log on any task, every record whole
  and all from one moment, without waiting for the main loop. Each record
  carries the order it was stored in (`AlertEntry::seq`), so `read_alerts()`
  still puts the newest first after a wrap and keeps the newest `cap`.
  `handle_mesh_alerts` reads it once; every field and the body are unchanged.
  Cost: about 90 B more static RAM and a 384 B stack copy per request. Pinned
  by `test_mesh_session`'s two-thread
  `test_alert_reads_stay_whole_while_the_main_loop_stores_and_clears` and new
  checks in `test_alert_ring_wraps_newest_first`: against the old in-place
  read the two-thread test fails 5 of 5 runs and ThreadSanitizer reports the
  race, and a local ThreadSanitizer build of the suite is clean now. CI builds
  it without one, where a host lock that does nothing usually still passes, so
  `check_canary_mesh_status.py` holds the rest: its one allowance is gone,
  rule 6 places every use of the log (appended only on the receive path,
  cleared only by the two clearers, read only by `read_alerts()`), and rule 7
  holds both locks' shape and the log's one writer (57 self-test mutations; 14
  errors on the old sources). Spec §8.1 says so. Host-tested; the `[env:full]`
  compile is CI's; not bench-tested (U1: the F197 row).
- [x] **F198 [code] The PlatformIO dashboard's Bluetooth tab calls routes
  nothing in its tree serves.** `securacv_webui.cpp` calls `/api/bluetooth`
  and eleven routes under it (power, scan start and results, pair start and
  cancel, paired devices, trust, forget all, disconnect, name, settings),
  three of them at every page load (`/api/bluetooth`, `/settings`, `/paired`)
  and `/api/bluetooth` every 5 s on that tab, but no file under
  `firmware/canary` or `firmware/common` registers an `/api/bluetooth` route
  (a grep of every `.uri =`). The same grep finds no route for the page's
  `POST /api/logs/rotate`, `GET /api/wifi` and `POST /api/wifi/forget` (the
  server has `/api/logs/*/ack`, `/api/logs/ack-all` and `/api/wifi/status`,
  `/scan`, `/connect`, `/disconnect`). F176's shape fits: hide what nothing
  serves until it answers, or point the calls at routes that exist. From a
  grep; not run on a device. Found by F176 (#1762).
  *Done (#1762):* the premise held, and was wider than counted: nothing under
  `firmware/canary` or `firmware/common` serves `/api/bluetooth` or any of the
  fifteen routes the page calls under it (the item counted eleven), `GET
  /api/wifi`, `POST /api/wifi/forget` or `POST /api/logs/rotate`. Bluetooth
  takes F176's shape: the tab starts hidden, the page load's one `GET
  /api/bluetooth` shows it only for a Bluetooth status and only then loads the
  settings and the paired list, the 5 s poll asks nothing until one has
  answered (`btServed`), and unlocking with a token asks once more while none
  has (the Community probe, F176, now does the same). The Wi-Fi card uses the
  routes that do the job: it reads `GET /api/wifi/status` (the AP row from
  `ap_active`, the home row "Saved", a saved network read from `state`), and
  since that route counts against the device-wide request limit where the old
  404 did not, it polls only while Settings is open. Forget posts
  `/api/wifi/disconnect`, which clears the saved network, so it shows only
  while there is no link, and Disconnect's confirmation now says it forgets
  the network, as the route always did. Rotate Old Logs is removed: no route
  does its job. `firmware/tests_host/test_canary_dashboard_routes.test.js` (14
  cases, run by the tests_host Makefile) lifts the code from the page's raw
  string and holds the tab's gate, the Wi-Fi card's states, Forget's route,
  Disconnect's text and the Settings-only poll, and scans every route the page
  names against a registration, matched as `httpd_uri_match_wildcard` matches;
  7 of the first 9 cases fail on the old page, and 30 mutations each fail it.
  The scan names one gap rather than fixing it (`KNOWN_UNMATCHED`): F214.
  Host-tested (node); not run on a device (U1: the F198 row). Found here: F214
  and F216.
- [ ] **F199 [code+decision] PIO `GET /api/mesh/peers` lists its members from
  NVS, so a board without flash encryption answers 500 `load_failed`, and the
  list can disagree with `GET /api/mesh`'s `peers_total`.** The rows are the
  persisted pubkeys (`mesh_state::load_trusted_peers`, flash-encryption
  gated), while `peers_total` and `peers_online` count the session's trusted
  table; on a dev board without flash encryption every peers read fails, and
  after an NVS save or removal that failed the two disagree until a reboot.
  F161 kept the NVS membership so the response would not change; the published
  view holds every member the session trusts, so the rows could come from it.
  Decide whether the list is the durable set or the session's. From code; not
  probed. Found by F161 (#1762).
- [x] **F200 [code] The kernel wizard leaves a Canary pairing when the other
  rejects the confirmation or stops answering.** `meshConfirm()` stops on a
  rejected or failed `pair/confirm` ("The ... Canary rejected the
  confirmation", "Network error during confirm"), and both of the wizard's
  waits stop after five unreadable polls, all without canceling a Canary still
  running the wizard's pairing; a Canary starts a pairing only when none runs,
  so the retry those messages invite is refused until that pairing's 5-minute
  timeout (a power pull after the codes show reaches the first path). F163's
  review fix cancels on a failure, on a body about another pairing and at
  either wait's timeout, each from a read that carries the wizard's pairing
  number; these paths have no such read, so a fix reads each side's status
  once and cancels a side that reports the wizard's number still running. From
  code; not run against a Canary. Found by F163's review (#1762).
  *Done (#1762):* `meshCancelLeftRunning()` reads each Canary's status once,
  each in its own try, and cancels through F163's `meshCancelRunning()` a side
  that reports the wizard's pairing number still `running`. It leaves alone a
  side that cannot be read, reports no number, finished or is on another
  pairing, and everything while either side reports this pairing `paired`:
  that side has had both confirms, so a partner still running is completing,
  and canceling it would leave a member that never joined. It runs before the
  message, with the status line saying both Canaries are being checked, on a
  confirmation either Canary rejects, on a confirm that fails on the network,
  and on each wait's five-unreadable-polls exit. A failed confirm answer does
  not prove the confirm failed, so on those two paths, when one side reports
  this pairing paired and the other paired or running, the wizard waits in the
  confirm wait as for any other answer. Each Start Pairing begins an attempt
  and Cancel ends it; the waits, the confirm and the cleanup carry their
  attempt and stop at their next await once it is gone, so a cleanup still
  waiting on a slow read cannot cancel a pairing started after Cancel. The
  retry those messages invite now finishes in the page too: the confirm button
  is ready again at the codes (it stayed "Confirming..."), Start is ready
  after a success or Cancel (it stayed "Starting..."), and the wait's line is
  reset. Not changed: a timeout still acts only on its final poll's reads and
  still cancels a side running beside a paired one, as F163 pinned (F215).
  `privacy_witness_kernel/tests/test_wizard_mesh_pairing.test.js` (23 cases)
  adds the F200 paths, each cancel before its message, a lost confirm beside a
  paired side, Cancel during a cleanup's read and a retry reaching a ready
  confirm; 11 of the 23 fail on the page before this change, and 38 mutations
  of the new code each fail it (four survivors are equivalent). Host-tested
  (node); not run against a Canary (U1: the F200 row, which the F163 row now
  points to). Found here: F215 and F217.
- [x] **F214 [code] PIO and canary-wap `POST /api/logs/<seq>/ack` may match no
  route.** Both trees register `/api/logs/*/ack` on servers whose
  `uri_match_fn` is `httpd_uri_match_wildcard`, which (ESP-IDF's httpd_uri.c)
  treats `*` as a wildcard only as a template's last character, or before a
  final `?`, and as a literal anywhere else. So a request for
  `/api/logs/42/ack` would match nothing and answer 404, and each dashboard's
  per-entry Acknowledge would fail while `ack-all` works.
  `firmware/FEATURES.md` lists the route as working on both. Register `POST
  /api/logs/*` and check for the `/ack` suffix in the handler, or match with a
  custom function. `test_canary_dashboard_routes.test.js` names it in
  `KNOWN_UNMATCHED`. From the matcher's published source; not run on a device.
  Found by F198 (#1762).
  *Done (#1762):* the premise held, asked of IDF's own matcher: against both
  trees' route tables `POST /api/logs/42/ack` answered 404 on canary-wap and
  405 on the PlatformIO canary (its GET `/*` fallback matches the path), while
  ack-all worked. The old template matched only its own literal text, so no
  per-entry request ever reached the old handler. Both trees now register
  `POST /api/logs/*` after every other `POST /api/logs/...` route: httpd
  answers with the first registration that matches and refuses a later
  template an earlier one already matches, so that order keeps ack-all (and
  canary-wap's rotate) reachable. The handler reads the rest of the path with
  `log_ack_seq_from_uri()` (the same function in `securacv_network.cpp` and
  `canary_wap.ino`): decimal digits that fit a uint32_t, then `/ack`, then the
  end of the path. Anything else answers 404 with httpd's default body (IDF
  5.x: "Nothing matches the given URI"; IDF 4.4: "This URI does not exist")
  and acknowledges nothing. Side effect: an unregistered GET under
  `/api/logs/` (canary-wap's documented `/api/logs/unacked`) answers 405
  rather than 404. `firmware/tests_host/idf_uri_route_oracle.c` holds
  `httpd_uri_match_wildcard()` copied verbatim from ESP-IDF release/v5.5 (the
  function is byte-identical in v4.4.7 and v5.3.2) and httpd's first-match
  registration loop, pinned by a hash of its text.
  `test_dashboard_route_match.test.js` (10 cases) reads both trees' route
  tables in registration order (73 PlatformIO and 192 canary-wap
  registrations, every `#if` branch), finds no registration refused, holds
  every request both dashboards make to the route it means (the PlatformIO
  page's gated Chirp and Bluetooth calls excepted), and runs each dashboard's
  per-entry Acknowledge URL through its own tree's parser, cut from the
  firmware (`test_log_ack_route --parse`). `test_log_ack_route.cpp` (229
  checks) runs both trees' parser and handler. The old registrations fail 6 of
  the 10 route cases, the new template registered ahead of ack-all 8, the old
  handler body 112 of the 229 checks, and three wrong page URLs and a dropped
  call each fail the Acknowledge case on both trees. `firmware/FEATURES.md`'s
  row now reads as host-tested and CI-compiled, not bench-tested. All rerun
  green on the integrated tree. Host-tested; the compiles are CI's; not run on
  a device (U1: the F214/F217 row). Found here: F237-F240.
- [ ] **F215 [code+decision] The kernel wizard's timeouts still leave or end a
  pairing on old reads.** F163 pinned that a timeout acts only on its final
  poll's reads: after up to 59 good polls and one that fails on either side, a
  side still running keeps its pairing until its own 5-minute timeout and
  refuses the retry (the confirm wait also counts no not-ok read toward its
  five, so a Canary answering errors reaches this exit too). And a
  confirm-wait timeout cancels a side still running beside one that reports
  the pairing paired, a joiner whose opera key never landed in 60 s, which
  F200 declines to do at its own exits. F200's `meshCancelLeftRunning()` (a
  fresh read of each side, canceling only this pairing's number and nothing
  beside a paired side) would serve both. Decide whether a timeout reads once
  more, and whether it spares a side running beside a paired one. From code
  and the F163 and F200 tests; not run against a Canary. Found by F200
  (#1762).
- [x] **F216 [code] The PlatformIO dashboard's Bluetooth settings load has an
  always-false guard.** `loadBtSettings()` (`securacv_webui.cpp`) returns
  early on `!data.enabled === undefined`, which is never true, so an error
  body would fill the settings form with blanks. Nothing reaches it on today's
  firmware (no `/api/bluetooth` route, and the tab is gated since F198); fix
  it with the routes, or drop the panel if `bluetooth_mgr.h` stays
  header-only. From code. Found by F198 (#1762).
  *Done (#1762):* `loadBtSettings()` now returns unless the body carries a
  boolean `enabled` (canary-wap's settings GET answers one), so an error body
  (a 404, a 401, `{success: false}`) leaves the form as it was; the old guard,
  `!data.enabled === undefined`, was never true. The load still runs only
  after a Bluetooth status has shown the tab (F198). Nothing in the PlatformIO
  tree serves `/api/bluetooth`, so nothing reaches this code on today's
  firmware; whether `bluetooth_mgr.h` stays header-only stays with F198's
  gate. `test_canary_dashboard_routes.test.js` (16 cases) gains one: five
  non-settings answers leave every field as it was, and a settings body still
  fills the form; it fails on the old guard. No generated web asset embeds
  this page. Host-tested (node); unreachable on a device until a firmware
  serves the route.
- [x] **F217 [code] The kernel wizard's Back during Start Pairing leaves the
  attempt running out of sight.** The form's Back (`closeMeshWizard()`) stays
  clickable while `meshStartPairing()` awaits `pair/start` and `pair/join`; it
  hides the screen but ends nothing, so the start goes on, its wait polls on a
  hidden screen (up to two minutes) and, if the codes show, stops there with
  both Canaries in PAIRING_CONFIRM until their 5-minute timeout. Nothing ends
  the attempt, so a reopened wizard's Start stays "Starting..." until a
  reload. Make Back during a start end the attempt (Cancel's path) or disable
  it while the start runs. From code (F200's attempt rule stops stale work
  once another attempt begins, but nothing begins one here); not run against a
  Canary. Found by F200's review (#1762).
  *Done (#1762):* Back waits for the start rather than ending it. While
  `meshStartPairing()` awaits `pair/start` and `pair/join`, the form's Back is
  disabled and `closeMeshWizard()` does nothing (`meshStarting`); Back is
  disabled before the flag goes up, and a `finally` enables it again on every
  way out. A failure leaves the form with Back working; a success hands the
  screen to the progress view, whose Cancel ends the attempt as before. Back
  does not take Cancel's path because it cannot end what the start is about to
  begin: the start's request may still be on its way to a Canary, so a cancel
  sent at Back can arrive first, and one sent once the start answers would
  break F200's rule that nothing is sent for an ended attempt. The cost: an
  unreachable Canary holds Back for the start's requests, normally bounded by
  the add-on's 10 s per-request timeout; a start the add-on never answers
  needs a page reload.
  `privacy_witness_kernel/tests/test_wizard_mesh_pairing.test.js` (28 cases)
  now runs the page against a stub DOM that answers null for an id the markup
  does not declare, and adds five cases: Back held while either request runs;
  every way out leaves Back working; the item's case ends with a reopened
  wizard's Start reaching its codes; every mesh lookup names a declared id,
  and Back's id is the button that runs `closeMeshWizard()`; a page whose Back
  lost its id still closes after the start throws. The page before this change
  fails 5 of the 28, and with Back's id removed all 28 fail. Whether Back
  stays disabled through a failed start's own cancels is not pinned (from the
  review). Host-tested (node); not run against a Canary (U1: the F214/F217
  row).
- [ ] **F237 [code] canary-wap's `wap_server.h` lists eight routes nothing
  registers.** Its endpoint comment names GET `/api/health`, `/api/identity`,
  `/api/witness/<seq>`, `/api/witness/stats`, `/api/logs/unacked`, `/api/gps`,
  `/api/time` and `/api/export/download`. None is in the route table
  `firmware/tests_host/dashboard_route_tables.js` reads (the oracle answers
  404 or 405 for each), and neither dashboard calls them. Correct the list to
  the table, or hold it to the table in `test_dashboard_route_match.test.js`.
  Found by F214 (#1762).
- [ ] **F238 [code] The route-match test reads only the two main dashboards.**
  `test_dashboard_route_match.test.js` holds the requests of the PlatformIO
  dashboard (`securacv_webui.cpp`) and canary-wap's `web_ui.h`. canary-wap's
  other pages (the companion PWA, the Tuning Lab, and the MQTT and Sense pages
  `csi_integration.cpp` serves) and the PlatformIO setup page call routes it
  does not check, and it reads every `#if` branch as one table, so a call to a
  feature-gated route counts as served in a build without the feature. Extend
  `pageRequests()` to those pages (their raw strings use other delimiters) and
  name any gap it finds. Found by F214 (#1762).
- [ ] **F239 [code] The PlatformIO probe routes' comment misstates the
  matcher.** `securacv_network.cpp` says `kProbePaths` carry a trailing `*`
  because "the wildcard matcher compares the FULL uri" and a probe may carry a
  query. esp_http_server hands the matcher only the path, so an exact template
  already matches a probe with a query, and the `*` also takes paths such as
  `/gen_204x`. Nothing misroutes, because `handle_captive_probe` checks the
  path again. Correct the comment, or drop the `*` and the second check
  together. From the IDF source F214 copied. Found by F214 (#1762).
- [ ] **F240 [code] `firmware/FEATURES.md`'s REST API table keeps a stale
  PlatformIO (canary-wap/) column.** The 2026-09-05 pass collapsed the
  dashboard table's two canary-wap columns because the PlatformIO lane builds
  the Arduino sketch through `src_dir`; the REST API table still has a
  "PlatformIO (canary-wap/)" column reading not-served for routes that lane
  serves (`GET /api/chain`, `GET /api/logs`, `POST /api/logs/ack-all`, and
  more). Collapse it into the WAP column as the dashboard table did. Found by
  F214 (#1762).
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
- [x] **F84 [code] The dash's onboarding scene lines run through its 300 px
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
  *Done (#1762):* the wide glass follows the small glass's rule (F65) inside
  its own halo. Every centered title and body goes through `fit_line()` (a
  network name through `name_line()`) at `scene_line_w()`'s width, inside the
  300 px ring's inner chord (`wide_ring()`, `kWideRingD`, now in
  `onboard_layout.h`), in the small glass's faces: the title in the
  Character's body face (24 px, 28 under Heirloom), the body in its caption
  face (16, 20), each stepping down to the default Character's face. No title
  face could fit: the chord is about 284 px at a title's latitude, and at 36
  px only "Hello.", "Joining", "You're in." and "No address" are that narrow.
  So on the dash, dash7 and nightstand7 the two shorter forms show at both
  ladders ("Check your phone", "No address"): F86's trade, now on the 800x480
  glass too (F86's note). Every other line reads whole in its own face, but a
  network name wider than the body's chord (286 px at the default type, 284
  under Heirloom) now shows its head and tail around "...":
  "Basement-Mesh-Extender-Office-5G" (296 px at 16) shows as
  "Basement-Mesh-E...nder-Office-5G" at both types, where the panel's old 784
  px row read it whole. No title box overlaps its body's any more (y 211..237
  over 245..262 at the default type; Hello 215..241 over 247..264). The Join
  title keeps the 36 px title face on its row above the ring, fitted to the
  panel's 784 px. The bird's square and its one wide seat (`kWideBirdPx`,
  `kWideBirdOff`) move into the header too, with `bird_seat()` (F89). In
  native LVGL 8.4 renders of the dash every scene line now sits within x
  257..542, inside the ring; the Success hop's box reaches y 91 at its apex
  (the ring's top edge is at 90), but none of its drawn pixels reach the
  stroke. `test_onboard_layout` runs its scene and halo checks on the wide
  glass too: every line whole inside the chord, the shorter forms pinned
  there, the Join title on its row, and the Join rows and the bird's seat,
  over its breath, clear of the stroke. A new check on every glass refuses a
  scene whose title box overlaps its body's. With the old faces it fails 108
  checks (54 lines that cannot read whole, 54 overlaps). `test_onboard_scenes`
  gains a wide build (the dash config): the real onboard_ui.cpp on the three
  wide envs with both ladders, QR or not. Against the old onboard_ui.cpp it
  fails 330 checks (186 lines across the halo's stroke, 144 title/body
  overlaps). `montserrat_metrics.h` now carries the title face (36 px).
  `test_onboard_layout` also runs every check on the panels main.cpp turns
  before `provision_run()`, report-only (counted and printed, never failing):
  10 do not hold on each portrait 480x800 dash glass and 26 on the landscape
  320x180 nightlight (F156, F157). Not held on wide glass: the QR card's
  rounded corners, which reach past the 300 px ring (F155). Host-tested, and
  measured in a native LVGL 8.4 harness (not in CI); the ESP32 builds are
  CI's; not bench-tested. The emulator dist moves for the five display
  flavors: it is rebuilt in this PR by CI's pinned emsdk after this ledger
  lands, and it was not rebuilt or run here.
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
  *Also (#1762):* the same trade now holds on the 800x480 glass (the dash
  line, dash7, nightstand7). F84 fits the scene lines inside the 300 px halo's
  chord (284 px at a title's latitude and 286 at the body's; 282 and 284 under
  Heirloom) in the small glass's faces (titles 24 px, 28 under Heirloom;
  bodies 16, 20). So "Check your phone" and "No address" show there at both
  types, and a network name wider than the body's chord
  ("Basement-Mesh-Extender-Office-5G", 296 px at 16) shows its head and tail
  around "..." where the panel's old 784 px row read it whole. One more option
  on that glass: sizing its halo with `halo_ring()` from its Join stack, as
  the small glass does, gives a 312 px ring at y 84 (306 at y 82 under
  Heirloom), between the Join title's row (to y 82; 80) and the credentials
  row (from y 398; 390), with a 296 px chord at a title (290 under Heirloom).
  There "Nice - check your phone" (295 px at 24) reads whole at the default
  type; under Heirloom (348 px at 28, 295 in the floor face) it does not, and
  "No address from the router" (331 px at 24) does not at either.
  `test_onboard_layout` pins where each shorter form shows on the wide glass
  too, so any change updates it.
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
- [x] **F88 [code] The top edge cuts the landscape nightlight's
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
  *Done (#1762):* the splash's seats move into
  `include/canary/ui/splash_layout.h` (the bird's square, its offsets, the
  wordmark's, the bubble's width, offset and frame). `seat()` keeps every seat
  the splash had wherever the canvas holds the bird with its highest hop:
  `kHopReach`, 16 px, which is canary_mark's 12 px apex at the temperament's
  1.25 ceiling plus LVGL's overshoot swing. Where it does not, the seat drops
  just enough: clamped to the canvas, not scaled. A bird that moved sits lower
  than the centered speech bubble expects, so the bubble then hangs from just
  under it (its top fixed, growing down). Only the landscape nightlight's
  first meeting moves: the bird's top at y 16 (its highest hop to 0), the
  bubble from y 84. In a native LVGL 8.4 run the bird now draws at y 3..81
  through its intro (it drew at y -25..53), and the bubble at y 84..153
  (84..177 under Heirloom) on the 180 px canvas; every other canvas draws as
  before. Two host tests hold it on every canvas the splash runs on: each
  display env's panel, and the panel turned for the two flavors main.cpp
  rotates before `splash_play()` (the nightlight's and the 7" and dash
  glass's, read off main.cpp). `tests_host/test_splash_layout.cpp` holds the
  header: both splashes' bird over its hop and breath on the canvas, only that
  one seat moving, and the hung bubble's tallest form (with any pseudonym: 86
  px, 94 under Heirloom) on the canvas and clear of the bird. The tallest
  bubble comes from a port of LVGL 8.4's label wrap
  (`tests_host/lv_txt_wrap.h`), held to 24 bubble heights the real LVGL drew
  in the native harness; `kHopReach` is derived from canary_mark.cpp and
  LVGL's overshoot curve. With the old fixed seat it fails 4 checks (the
  320x180 canvas at both ladders). `tests_host/test_splash_scenes.cpp` holds
  splash.cpp to the header: it compiles the real splash.cpp and
  canary_mark.cpp against the host tests' fake LVGL (three builds: the watch,
  nightstand and dash configs), plays the whole first meeting and hello again
  on every canvas of the build's glass with both ladders and the hop at its
  ceiling, and at every refresh reads where the bird and the bubble land. The
  bird must breathe down to exactly the seat `seat()` names and hop no higher
  than `kHopReach`, on the canvas; the bubble must sit where `seat()` puts it
  (hung or centered) in the height its typed line wraps to, inside the
  canvas's height and clear of a moved bird; its tallest must be
  test_splash_layout's measure. Inverting the hung branch, re-aligning the
  bird to its old seat or the bubble to the center after the branch, another
  bubble pad or label inset, and the pre-F88 splash.cpp each fail it. Not
  held, only printed: the bubble's tail, aligned once to the empty bubble
  (F158); on the AMOLED the centered bubble rising into its bird (F159); and
  on the 172 and 180 px wide portrait glass the 196 px bubble overhanging the
  canvas by 12 and 8 px a side (F160). Host-tested and measured natively (the
  harness is not in CI); the ESP32 builds are CI's; not bench-tested, and no
  rotated nightlight has shown it. The emulator dist moves (it is rebuilt in
  this PR by CI's pinned emsdk after this ledger lands, not here); no emulator
  flavor runs the 320x180 canvas, so its splashes should draw as before.
- [x] **F89 [code] No emulator probe reads where the bird is drawn.** F64's
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
  *Done (#1762):* the emulator exports `emu_onboard_seat()` (the shell's
  `onboardSeat()`): the box `onboard_layout.h`'s `bird_seat()` names for the
  glass, its faces and the scene on it, computed by the firmware through
  `onboard_ui_bird_seat()` (the same call `onboard_ui.cpp` seats the bird
  with), and its breath (`kBirdBreath`). `bird_perch.mjs` gains `birdOnSeat()`
  (one drawn box from `markBox()`: the seat's size, at its x, within the
  breath of its y) and `breathOnSeat()`, which holds a scene's run of reads to
  the seat exactly. One read cannot: a bird up to twice the breath off its
  seat can read within it. canary_mark's eased breath rounds down, so across a
  whole swing the drawn top must reach the seat less the breath exactly and
  the seat plus the breath less 1 px, and never pass either end: a bird 1 px
  off fails wherever the reads fall, and a run that misses a swing fails too.
  `onboard_probe.mjs` now reads the Hello scene, on an emulated clock slowed
  tenfold from the first-boot serial line, from the bird's first breath to the
  scene's end (2.6 s), and PhoneJoined for 4 s (a whole swing at the slowest
  temperament). Every read is held by `birdPerch` (on the glass, clear of
  every line) and each run by `breathOnSeat`. An idle flourish can hop the
  bird during the PhoneJoined reads, so a run that caught a hop
  (`flourishHop()`: a read 8 px or more above the seat) is read again, at most
  twice more; a bird misplaced by less than 6 px is never taken for a hop. No
  second table of seats lives in the page, and a dist built before the binding
  fails the probe instead of passing it. `onboard.test.js` models LVGL 8.4's
  breath (lv_bezier3's ease-in-out with lv_anim's rounding) and holds
  `breathOnSeat` at eight phases: on the seat it passes, at the slowest breath
  too; 1 or 3 px off either way it fails; a short run, a still bird, a bird
  aside and a seat that moves fail. It holds `flourishHop` both ways and the
  wiring from the binding to the probe. With the span rule disabled, or with
  bounds alone (the reads within the breath), it fails. Run locally in
  Chromium against the committed dist, which predates the binding, the probe
  fails every flavor at Hello ("this emulator dist has no emu_onboard_seat").
  With a scratch shim standing in the binding (the header's seats at the
  default Character), the walk passes all five flavors and fleet.html, and
  every Hello and PhoneJoined run reaches exactly its seat less 2 px and at
  least its seat plus 1: the watch y 34..38 (seat 36), the nightstand 74..78
  (76), the touch169 54..58 (56), the AMOLED 214..218 (216), the dash 102..106
  (104), each over about 700 Hello reads and 75 PhoneJoined reads. With the
  shim's seat moved 1 px either way (the watch, the dash), 1 px down (the
  touch169) or 3 px (the watch), the probe fails. `boot_probe.mjs` passes all
  five on the committed dist. The dist is rebuilt in this PR by CI's pinned
  emsdk after this ledger lands; the rebuilt dist must pass the whole probe,
  and it was not run against it here. Not seen on real glass (U1).
- [ ] **F155 [code+decision] The 800x480 glass's QR card corners reach past
  its 300 px halo.** The dash line's Join card (232 px, 10 px corners) reaches
  159.9 px from the halo's center (163.5 under Heirloom, where the Join stack
  centers the card 5 px above the ring's center), past the ring's 147..150 px
  stroke, so the white card paints over the ring at its four diagonals. F66
  trimmed the touch169's canvas for the same reason; here even the floor
  canvas that keeps the join code's 7 px pitch (203 px) reaches about 156 px,
  and a ring large enough to clear the corners (about 330 px) would cross the
  Join title and credentials rows. Either accept it or recompose the wide Join
  scene. `test_onboard_layout` prints the reach on wide glass, and neither it
  nor `test_onboard_scenes` holds it there. Found by F84 (#1762).
- [x] **F156 [code] A portrait dash runs its onboarding on 480x800 with Join
  rows wider than the panel.** A dash or 7" glass whose saved rotation is
  portrait wears it before the splash and the onboarding
  (`lvgl_port_set_rotation()` in setup()), so after
  `forget_wifi_credentials()`, or when `wifi_wants_setup()` reopens setup, the
  first-boot scenes run on a 480x800 canvas. There the Join title "Scan with
  your phone camera" is 553 px in the 36 px title face on a 464 px row, so it
  now ends in an ellipsis (before F84 it was content-sized and centered, and
  the panel's edges cut both ends). The credentials line (up to 655 px, 773
  under Heirloom) and the stuck-phone hint (513, 644) are content-sized and
  cut by the edges. The other scenes' lines are fitted and fit.
  `test_onboard_layout` now runs its checks on the turned panel report-only:
  10 do not hold on each of the dash, dash7 and nightstand7.
  `test_onboard_scenes` does not run the turned panel. Found by F84 (#1762).
  *Done (#1762):* on the 480x800 canvas a dash or 7" glass turned portrait
  runs, every onboarding line fits its 464 px row at both ladders. The wide
  Join title has a shorter form in fewer of its own words, in the same 36 px
  title face: "Scan with your phone" (404 px; the whole line is 553) and, when
  no code rendered, "Join this network" (`wide_join_title()` returns both
  forms and `fit_line()` picks). The worded credentials line keeps one row
  wherever the panel's row holds it in the label face, which every 800 px
  glass does for any name and key the unit can mint, and otherwise splits the
  way small glass's does (`wide_join_lines()`): the name on the credentials
  row over `password <key>`, both in the label face, and a standing hint on a
  note row the wide glass now has too (`wide_join()`). provision.cpp hands the
  wide stuck-phone hint a shorter form, "on your phone, forget this network -
  then scan again" (427 px at 16), which the portrait glass shows at both
  ladders; the 800 px glass keeps the whole hint. The coach lines are fitted
  the same way. Every wide row is now a label as wide as the panel's row, so
  on the 800x480 glass the same words sit on the same rows (a line of odd
  width 1 px left of where its content-sized label put it).
  `test_onboard_layout` runs every check on the turned panels and holds them
  (report-only before: 10 failed on each of the dash, dash7 and nightstand7).
  `test_onboard_scenes` runs the real onboard_ui.cpp on the turned panel in
  its dash build and, on every glass, holds what it draws: each credentials,
  hint and note row on the stack's row at the width `fit_row()` gives it, at
  least as tall as its face's line, in its role's face and color, and the Join
  title and rows exactly as onboard_layout.h says for the panel, so the 800 px
  glass's whole title and whole hint are held on the drawn screen. Each piece
  fails by mutation, among them the title's shorter form reverted (36 scene
  checks), the old content-sized rows (66), the split key row in the caption
  face (36) and the title's forms handed in the other order (36). In a native
  LVGL 8.4 harness (not in CI) on 480x800 the old tree cut the Join title and
  drew the credentials and hint lines past both edges; the new tree draws no
  line cut or off the glass at either ladder, QR or not. Not held there, as on
  the 800x480 glass: the QR card's corners past the ring (F155) and the
  Success hop's box reaching the ring's top at its apex. Host-tested and
  measured natively; the ESP32 builds are CI's; not bench-tested. The emulator
  dist moves for the five display flavors (rebuilt in this PR by CI's pinned
  emsdk); no emulator flavor runs a turned panel (F184). Found here: F184.
- [x] **F157 [code] A landscape nightlight runs its onboarding on 320x180,
  where the small-glass layout does not fit.** The nightlight wears its saved
  rotation (its own NVS key; `lvgl_port_set_panel_rotation()` in setup())
  before `provision_run()`, which runs on placeholder credentials and again
  from loop() when `wifi_wants_setup()` reopens setup (a renamed network, a
  mistyped saved password, a 5 GHz-only network, before the unit has ever
  joined since power-on). On the 320x180 canvas `small_join()` gives a 124 px
  halo at y 22 (118 at y 26 under Heirloom). Nine scene lines per ladder
  cannot read whole ("Let's get you connected." is 148 px in the floor face on
  a 96 px line), the QR card's corners reach 69.4 px from the halo's center
  past its stroke (from 59 px), the note row runs off the glass (y 178..193),
  and the bird's seat (its top at y 6) crosses the stroke. In native LVGL 8.4
  seven lines are drawn cut, 42 of the bird's pixels sit on the halo's stroke,
  and the Success hop lifts its box to y -7, so the top edge cuts its head
  (F88's defect, in the onboarding). `test_onboard_layout` now runs its checks
  on the turned panel report-only: 26 do not hold. `test_onboard_scenes` does
  not run it. Found by F84's review (#1762).
  *Done (#1762):* landscape small glass composes the scenes sideways
  (`land_join()` in onboard_layout.h; `small_join()` hands it any small glass
  wider than it is tall). On the 320x180 nightlight the halo stands at the
  panel's right, 2 px from its edge (x 168..318, y 15..165), the smallest even
  ring that holds the QR card at its floor canvas (88 px, a 104 px card, 3 px
  a module as on every small glass), with the card's corners 2 px inside its
  stroke. The bird sits at the halo's center in every scene, inside the stroke
  over its breath and its hop. The text takes a 158 px column from x 8 to 2 px
  short of the halo: every scene line is fitted to it, the block centered, the
  Join title at the scenes' title latitude and the credentials, hint and note
  rows under the lowest body. The halo, the card and the bird ride a new x
  offset on the ring (`Ring::x`); the rows and the lines ride the column
  (`Column`, onboard_ui.cpp's `fit_row()`). Every line reads whole, in the
  words and shorter forms of the 172 and 180 px portrait glass. No glass a
  display ships is landscape small glass, and each keeps its composition
  (pinned). `test_onboard_layout` holds the turned panel (26 checks had
  failed, report-only), with landscape checks of its own and the Success hop's
  reach on the glass on every glass; `test_onboard_scenes` runs onboard_ui.cpp
  on it in its nightstand build and holds each row it draws on its seat in the
  column, in its face and color, and the Join words exactly as
  onboard_layout.h says. Reverting the landscape composition fails 34 layout
  and 122 scene checks. In the native LVGL 8.4 harness on 320x180 the old tree
  drew 7 lines cut per run, the bird across the halo's stroke in 5 scenes and
  off the glass in Success, and the card past the stroke; the new tree none,
  at both ladders, QR or not. Host-tested and measured natively; the ESP32
  builds are CI's; not bench-tested, no rotated nightlight has shown it, and
  no phone has scanned the 88 px code. The emulator dist moves (no flavor runs
  the 320x180 canvas, F184).
- [x] **F158 [code] The splash's speech-bubble tail is aligned once, to the
  empty bubble.** splash.cpp aligns the tail (`LV_ALIGN_OUT_TOP_MID`, +5) when
  the bubble first shows, before any line is typed. A centered bubble then
  grows both ways as its lines wrap, and the tail stays put, inside the grown
  bubble: in native LVGL 8.4 the round watch's tail top sits at y 99 inside a
  bubble at y 90..159, and the same happens on every canvas whose bubble is
  centered. Only the hung bubble (the landscape nightlight's, F88) keeps its
  top edge, and its tail with it. Re-align the tail after each line, or anchor
  the bubble's top. Neither splash test reads the tail. Found by F88 (#1762).
  *Correction (#1762, wave 12):* only the LVGL 9.5 builds (the dash line,
  dash7, nightstand7, nightstand-c6) draw the tail at all. LVGL 8.4 (the
  watch, nightstand-s3, touch169, amoled241, nightlight-c3 and every emulator
  flavor) refuses the turned, rounded square's alpha layer
  (`lv_draw_sw_layer_create` with `LV_COLOR_SCREEN_TRANSP` 0), so the 8.4
  numbers above are the tail object's box, not drawn pixels, and those glasses
  showed no tail before or after; the premise holds on the 9.5 builds, where
  the old tail was drawn over the bubble's first line.
  *Done (#1762):* splash.cpp aligns the tail again whenever the typed line
  changes (`lv_obj_align_to()` lays the screen out first, so the tail object
  lands on the bubble's top edge as it stands), and sets the label only when
  its text changes (it was set every frame, redrawing an unchanged line).
  `splash_layout.h` names the tail (`kTailSide`, `kTailInset`). On the 9.5
  builds the tail now stands on the bubble's edge under the bird; before, it
  was drawn over the bubble's first line (a radius-0 native emulation of a
  drawing build; the 9.5 draw itself is read from source, not run). The 8.4
  glass and every emulator flavor draw no tail, before this change and after
  (F185). `test_splash_scenes` reads the tail object's box at every refresh
  (up with the bubble, centered on it, `kTailInset` into its top edge; nothing
  is drawn there) and holds the bubble's line set only when its text changes;
  aligning the tail once, as before, or setting the line at every frame fails
  it in each of its round, small-glass and wide builds. In the native LVGL 8.4
  harness the old tree's tail object sat off the bubble's top edge in 497 to
  979 of 1407 drawn frames per canvas, by up to 54 px; the new tree's never.
  Host-tested and measured natively; not bench-tested. The emulator dist moves
  for the five display flavors and shows no tail, as before. Found here: F185.
- [ ] **F159 [code+decision] On the AMOLED the first meeting's speech bubble
  covers the bird's body.** The 450x600 AMOLED runs splash.cpp's small-glass
  composition (a 64 px bird at CENTER -70 over a bubble centered at +5) with
  the big type ladder's label face (20 px, 24 under Heirloom), so its bubble
  is taller: in native LVGL 8.4 the tallest is 110 px (157 under Heirloom; 132
  and 184 with a wide-lettered pseudonym), and its top rises 14 px into the
  bird's box at the default type and 37 under Heirloom (25 and 51 with the
  wide-lettered pseudonym, as `test_splash_scenes` draws it). The bird's drawn
  body ends about 10 px above its box, so under Heirloom more than 20 px of it
  are behind the bubble. The other small glass overlap a few pixels of box,
  below the drawn bird. `test_splash_layout` and `test_splash_scenes` print
  these and do not hold them. Seat the bird from the tallest bubble where the
  canvas has room, or give the AMOLED its own seats. Found by F88 (#1762).
- [x] **F160 [code] The first meeting's 196 px speech bubble is wider than the
  172 and 180 px portrait glass.** splash.cpp's small-glass bubble is 196 px
  wide (`kSmallGlass.bubble_w`), drawn for the round watch's 240 px disc, and
  centered on every small-glass canvas. On the 172x320 nightstand (s3 and c6)
  it overhangs each side of the canvas by 12 px and on the 180x320 nightlight
  by 8 px, so the bubble's border and rounded corners are off the glass and it
  reads as a band, not a bubble; on the 172 px glass its 172 px text column
  also starts 1 px off the left edge. `test_splash_scenes` (compiling the real
  splash.cpp against the host tests' fake LVGL) prints the overhang and does
  not hold it. Size the bubble from the canvas (the canvas less a margin,
  capped at 196 px) and re-measure its tallest form, or accept it. Found by
  F88's fixer (#1762).
  *Done (#1762):* the bubble is as wide as its family asks and no wider than
  the canvas less 4 px a side (`splash_layout.h`'s `bubble_w()`,
  `kBubbleMargin`): 164 px on the 172x320 nightstand (s3 and c6) and 172 on
  the 180x320 nightlight; every other canvas keeps its width. The margin is 4
  px, not the rows' 8, because at 8 the 180 px glass would wrap one more line.
  The tallest form is re-measured at those widths and held to what a native
  LVGL 8.4 harness drew: the 180 px glass keeps its heights (86 px, 94 under
  Heirloom), and the 172 px glass keeps 86 at the default type but under
  Heirloom wraps one more line with the widest-lettered pseudonym (112 px; 94
  with "2222..." and with each of 40 random pseudonyms), every line still on
  the canvas. `test_splash_scenes` holds the bubble's width and its overhang
  to zero on every canvas (it printed 12 and 8 px); `test_splash_layout` sizes
  the bubble per canvas, pins where it narrows and where its tallest grows,
  and holds the wrap model to the four new native heights. Restoring the 196
  px width fails 21 scene checks in the small-glass build, and `bubble_w()`
  returning the family's width fails 13 layout and 9 scene checks. In the
  native harness the bubble now spans x 4..167 and 4..175 (it spanned -12..183
  and -8..187). Printed, not held: under Heirloom with the widest-lettered
  pseudonym the 172 px glass's tallest bubble now reaches 15 px into the
  bird's box (6 before) and hides 86 of the bird's drawn pixels (none before,
  none with a typical pseudonym; F183). Host-tested and measured natively; not
  bench-tested. The emulator dist moves for the five display flavors. Found
  here: F183.
- [ ] **F183 [decision] The 172 px glass's narrowed splash bubble reaches
  further into the bird under Heirloom.** F160 narrows the first meeting's
  speech bubble to 164 px on the 172x320 nightstand (s3 and c6). Under
  Heirloom its tallest form, "Not that kind of bird. Call me %s." with the
  widest-lettered pseudonym ("WWWW..."), wraps one more line (112 px), so the
  centered bubble's top reaches 15 px into the bird's 64 px box (6 before),
  and in a native LVGL 8.4 harness it hides 86 of the bird's drawn pixels
  (none before). A typical pseudonym does not: "2222..." and each of 40 random
  pseudonyms keep the bubble at 94 px, hiding none. On nightstand-c6, the only
  172 px build that draws the tail (LVGL 9.5), the tail stands 7 px higher
  still and covers about 74 more (a radius-0 native emulation). The 180 px
  glass and the default type are unchanged. Either accept it, hang the bubble
  under the bird on that canvas, or settle it with F159's choice for the
  AMOLED (seat the bird from the tallest bubble), which would settle both.
  `test_splash_layout` and `test_splash_scenes` print it. Found by F160
  (#1762).
- [x] **F184 [code] No emulator flavor runs a turned panel.** The onboarding
  and the splash on the 480x800 portrait dash glass and the 320x180 landscape
  nightlight are held only on the host (`test_onboard_layout`,
  `test_onboard_scenes` and the splash tests, against fake_lvgl) and were
  measured in a native LVGL 8.4 harness that is not in CI; `onboard_probe.mjs`
  and `boot_probe.mjs` (and `bird_perch.mjs` through them) run the five
  flavors' native panels only. A harness parameter that applies a saved
  rotation before the first boot, the way `main.cpp` does, would let the
  probes read the turned scenes off the framebuffer: no line cut or off the
  glass, the bird on `bird_seat()`'s seat. Found by F156 and F157 (#1762).
  *Done (#1762): the turned dash glass here; its splash in a browser and the
  landscape nightlight in F206 and F204.* The emulator runs a turned panel,
  and the onboarding probe walks it. On LVGL 8.4, the emulator's pin,
  `lvgl_port_set_rotation()` used to change only its own logical size, so a
  saved portrait rotation laid a 480x800 face out on LVGL's 800x480 canvas.
  Its 8.4 dash branch now turns the display through v8's API (sw_rotate and
  `lv_disp_set_rotation()` with the 9.x branch's quarter turns, the screen
  invalidated and the touch layer told, touching the driver only on a change),
  and `fill_indev_data()` re-encodes the fed pointer sample on both majors
  (`test_display_settings` holds `rotation_to_lvgl_indev()` against v8's
  arithmetic). No shipped env builds the dash on LVGL 8. The emulator's
  display HAL holds its framebuffer in the frame the glass's viewer reads: it
  reads the turn off the refreshing display's driver, maps each native pixel
  back through the inverse of `lv_refr.c`'s `draw_buf_rotate`, reports the
  turned size and re-announces the glass's shape. `emu_preset_rotation()`
  stages a saved rotation before power-on, through the glass settings store
  after every preseed and before `setup()`, so `main.cpp` wears it from the
  first frame; the harness takes `?rotation=0..3` and logs every shape the
  firmware announces, and the shell refuses to boot a dist without the binding
  rather than boot it unturned. `emu_screen_arcs()` reports every arc as the
  circle `lv_arc` draws, and `emu_onboard_join()` reports the halo's and the
  QR card's boxes as `onboard_layout.h`'s stack seats them, through the new
  read-only `onboard_ui_join_layout()`, which `test_onboard_scenes` holds on
  every glass, scene, ladder and QR state. On every walk, native and turned,
  `onboard_probe.mjs` reads Hello, Join with and without the stuck-phone hint,
  PhoneJoined, both failures, Connecting after the wrong-key and the right-key
  joins, Success until the screen goes, and the face the boot ends on: every
  line on the glass, none cut as the label's own text says, and each settled
  line inked inside the box the firmware reports (`tests/onboard_glass.mjs`:
  `linesOnGlass`, `linesCut`, `linesInked`); the bird on the glass and, where
  it sits, clear of the lines (through Success's hop only its on-glass half,
  `birdOnGlass`); the join QR upright, its finder patterns at top-left,
  top-right and bottom-left (`qrUpright`: a glass drawn mirrored, flipped or
  upside down moves one to bottom-right); and every frame the firmware drew on
  one glass (`framesOnGlass`). The turned walk (`dash@portrait`, the panel and
  turn read from build.sh's pin map and `glass_settings.h`) also holds that
  the firmware's first frame was already 480x800, so the splash ran turned;
  that on both Join reads the QR card and the halo stand where the layout
  seats them (`cardAtLayout`, `haloAtLayout`); that the halo's stroke is inked
  where its circle says, with nothing 3 px outside it (`haloInked`); and that
  the card stands over the halo's center with its sides inside the stroke
  (`cardInHalo`). Its corners reach past the 800 px glass's ring (159.9 px
  from the center against a 147 px inner edge, F155) and are printed, not
  held. `onboard.test.js` pins `setup()`'s order: the rotation is worn once,
  after `lvgl_port_init()` and before `splash_play(` and `provision_run(`.
  `emulator/test/glass_turn.sh` builds the real `lvgl_port.cpp` and the real
  HAL against LVGL 8.4 with g++: at every quarter turn the glass equals the
  scene rendered unturned at the logical size, pixel for pixel; the HAL
  announces each change of shape; a landscape boot leaves the driver
  untouched; and a fed touch comes back out of LVGL's pointer where it went
  in. Against the old `lvgl_port.cpp` it fails 6 checks. CI runs it right
  after the third-party cache, before any step that reads the committed dist,
  so a stale dist cannot skip it. In a native full boot outside CI (build.sh's
  whole dash TU list, the real `main.cpp` included, built with g++), the
  probe's own holds pass at both portrait turns and unturned, at both ladders,
  and nine breaks each fail them (the old holds missed six). On the committed
  dist, which predates the bindings, the five native walks with every new read
  pass, as do `fleet.html`, `boot_probe.mjs` and `csp_probe.mjs`;
  `dash@portrait` fails at once ("this emulator dist has no
  emu_preset_rotation"). The emulator dist moves for the five display flavors:
  CI's pinned emsdk rebuilds it in this PR after this ledger lands, the
  rebuilt dist must pass the whole probe, `dash@portrait` included, and none
  of it was run against a rebuilt dist here. Until then the onboard probe on
  the committed artifacts goes red in `canary-local.yml`'s wasm job and that
  job's later steps are skipped (`glass_turn.sh` runs before them). Still
  open: no emulator flavor builds the 320x180 landscape nightlight, so only
  the portrait dash is walked turned (F204), and no browser probe reads the
  turned splash (F206). The emulator's turned glass is LVGL 8.4's software
  rotation; the shipped dash builds run LVGL 9.5, which does not turn what it
  flushes (F205). Not bench-tested. Found here: F204-F206 and A47; the display
  flavors' missing native stand-in is merged into A43.
  *Done (#1762, wave 14):* the rest of it is F204 (the emulator builds
  the nightlight and walks it landscape) and F206 (the turned splash read in a
  browser, laid out and inked, and turned boots in `boot_probe.mjs`). F206 is
  done, and `dash@portrait` passes on the committed dist, which CI rebuilt
  after wave 13. F204 is done: CI's pinned emsdk built the nightlight bundle
  (`9f636001`), and on that dist, in local Chromium, `boot_probe.mjs` boots
  `nightlight@landscape` at 320x180 with the bird on the glass and
  `onboard_probe.mjs` walks it with every turned-walk hold. F205 is done too:
  the LVGL 9.5 dash builds now turn each flush in `lvgl_port.cpp`'s
  `flush_cb`, the same quarter
  turns LVGL 8.4's sw_rotate draws in the emulator, so the emulator's turned
  glass shows what the shipped builds draw, as host-tested
  (`test_lvgl_port_turn`, `test_display_settings`) and as a native LVGL 9.5
  run outside CI showed (no CI job rendered with LVGL 9.5 then; since wave 15
  the wasm job ends with `glass_turn_lvgl9.sh` and `check_lvgl9_quotes.py`,
  F225, not yet run in CI). CI ran the
  same probes green on the commit that landed the manifest claim
  (`41d53091`: 72 checks, the wasm job's boot, onboard, csp and render probes
  included). Not bench-tested.
- [ ] **F185 [code+decision] The LVGL 8.4 glass's speech bubble has no tail.**
  splash.cpp turns its 12 px tail square 45 degrees with rounded corners; LVGL
  8.4 renders a turned object through a transform layer, a rounded one needs
  that layer's alpha, and `lv_draw_sw_layer_create` refuses an alpha layer
  while `LV_COLOR_SCREEN_TRANSP` is 0 (the display's lv_conf). So the watch,
  nightstand-s3, touch169, AMOLED and nightlight-c3, and every emulator
  flavor, draw no tail (a native LVGL 8.4 check: 0 px turned with radius 2, 60
  px turned with radius 0, 48 px unturned); only the 9.5 builds (the dash
  line, nightstand-c6) draw it. Where it is drawn, the square turns about its
  top-left corner (the default pivot), so in a radius-0 emulation the diamond
  stands about 5 px left of the bubble's center line and reaches 10 px into
  the bubble. Either draw it without a transform (a triangle), make it
  drawable on 8.4 (radius 0 passes natively; a centered pivot would center
  it), or drop it on 8.4. `test_splash_scenes` holds the tail object's box,
  not what is drawn. Found by F158's review (#1762). Since wave 15 the wasm
  job ends with `glass_turn_lvgl9.sh` (F225; not yet run in CI), which
  renders the tail's styles through the real LVGL 9.5.0; natively they draw a
  145 px diamond hung from the square's top-left corner, as this item says.
  `splash.cpp`'s own tail is not rendered there yet (F246).
- [x] **F204 [code] No emulator flavor builds the nightlight.**
  `canary-local/emulator/build.sh` builds the watch, dash, nightstand,
  touch169 and amoled241 glasses; none defines `CD_NIGHTLIGHT`, so the
  nightlight's hardware rotation (`display_set_rotation()`'s MADCTL table,
  `lvgl_port_set_panel_rotation()`) never runs in the emulator, and the
  320x180 landscape onboarding F157 composes is held only on the host and in a
  native harness outside CI. A nightlight flavor (its config and 180x320
  pins), the emulator HAL's `display_set_rotation()` and
  `emu_preset_rotation()` staging the nightlight's own NVS key would let
  `onboard_probe.mjs` add a landscape nightlight entry to its `TURNED` table,
  where the turned walk's holds would apply: the card and halo at the layout's
  seats (`onboard_ui_join_layout()` already returns the halo's x offset beside
  the text) and the QR card's corners inside the stroke (`cardInHalo`'s
  `corners` option, which F157's 2 px clearance passes). Found by F184
  (#1762).
  *Done (#1762): built by CI's pinned emsdk, claimed by the manifest, and
  walked in a browser.*
  `canary-local/emulator/build.sh` builds a nightlight flavor (`./build.sh
  nightlight`, in `all` and the allowlist, `createCanaryEmuNightlight`): the
  C3-LCD-1.47's pin map (180x320), the nightlight config (`CD_NIGHTLIGHT` over
  the nightstand flavor) and `CD_LEAN_BUILD`, as its env sets them; the env's
  ESP-NOW and pair-demo flags are not mirrored. The emulator's display HAL
  implements `display_set_rotation()` the way the board turns it, in hardware:
  the framebuffer takes the logical frame's shape, the page is told the new
  shape, and every flush lands where LVGL drew it. What the MADCTL table does
  to the panel's RAM stays `display_1in47.cpp`'s, to be bench-verified.
  `imu_init()` answers that no IMU is there. `emu_preset_rotation()` stages
  the nightlight's rotation in its own `scv-nl` key, so `main.cpp` wears it
  before the splash. `tests/turned_glass.mjs` reads the turned glasses from
  the sources, and `onboard_probe.mjs` and `boot_probe.mjs` walk
  `nightlight@landscape` (320x180) beside `dash@portrait` with every
  turned-walk hold, the card's corners held inside the halo's stroke
  (`cardInHalo`'s `corners`, F157's 2 px); `haloInked` now accepts an outside
  pixel that falls off the glass. `emulator/test/glass_turn.sh` builds the
  real `lvgl_port.cpp` and HAL a second time in the nightlight config: at
  every quarter turn the glass equals the scene rendered at the logical size,
  pixel for pixel, and each change of shape is announced (47 checks). The dist
  drift check now names a bundle `build.sh all` builds that the committed dist
  lacks (the old step passed an untracked file). In a native full boot outside
  CI the probe's holds pass at both landscape turns and unturned, corners held
  (69.4 px from the halo's center against a 72 px inner edge); without the
  `scv-nl` staging the boot comes up 180x320 and fails them. CI's pinned emsdk
  then built `dist/canary-display-nightlight.js` (`9f636001`); the five
  existing display bundles did not move (the Vision core did, for this wave's
  Vision fixes). `devices/canary-display-nightlight-c3/device.json` claims it
  (`"emulator": {"flavor": "nightlight"}`), and `lint_device_manifests.py`
  passes. On the rebuilt dist, in local Chromium: `boot_probe.mjs` passes all
  eight boots, `nightlight@landscape` at 320x180 with two shapes announced and
  the bird (93x93) on the glass; `onboard_probe.mjs` passes the nightlight's
  native walk and `nightlight@landscape` (the glass 320x180 from the first
  frame, the splash read whole, the QR card and halo where the layout seats
  them); `csp_probe.mjs` loads all 32 pages, the nightlight's harness among
  them, with no violation; and `render_probe.mjs` passes. CI ran the same
  probes green on the commit that landed the claim (`41d53091`: 72 checks,
  the wasm job's boot, onboard, csp and render probes included). The Lab
  offered no nightlight twin then (A54, done in wave 15). Not bench-tested.
  Found here: F222 and A54.
- [x] **F205 [code] On LVGL 9.5 the dash glass's rotation does not turn what
  it draws.** LVGL 9.5's `lv_display_set_rotation()` swaps the logical
  resolution and "will not perform the actual rotation" (its docs,
  main-modules/display/rotation.rst): in partial render mode `refr_area()`
  renders logical areas and `call_flush_cb()` hands them on as they are, and
  the in-tree drivers turn them with `lv_draw_sw_rotate()` in their flush.
  `lvgl_port.cpp`'s 9.x `flush_cb` blits `px_map` at the area unturned, and
  matrix rotation is off. In a native LVGL 9.5 run of the real `lvgl_port.cpp`
  (dash config, a recording `Arduino_GFX`), after
  `lvgl_port_set_rotation(ROT_PORTRAIT)` the logical canvas is 480x800 and the
  flushes span x 0..479, y 0..799 into the 800x480 RGB framebuffer, so a
  portrait dash, dash7 or nightstand7 most likely shows the turned face's top
  480 rows unturned in the panel's left 480 columns and loses the rest (read
  from source and that native run; not bench-tested). The emulator (LVGL 8.4,
  sw_rotate since F184) shows what the firmware intends, not this. A fix
  rotates each flushed area in `flush_cb` (`lv_display_rotate_area()` and
  `lv_draw_sw_rotate()`, the docs' partial-mode example) or enables matrix
  rotation; `rotation_map_touch()` and its host oracle
  (`test_display_settings.cpp`'s `forward()`, 90 degrees to (W-1-ly, lx)) must
  then follow the quarter turn drawn, and they assume the opposite of LVGL's
  own (`lv_display_rotate_area()` and LVGL 8's `draw_buf_rotate`, 90 degrees
  to (ly, H-1-lx)). Found by F184 (#1762).
  *Done (#1762):* on LVGL 9.5 the dash glass turns what it draws, and a tap
  lands on what was drawn under it. A native LVGL 9.5.0 run of the real
  `lvgl_port.cpp` (dash config, a recording `Arduino_GFX`; outside CI)
  confirmed the premise before the fix: after
  `lvgl_port_set_rotation(ROT_PORTRAIT)` 153,600 flushed pixels fell off the
  800x480 panel and as many native pixels were never repainted. It also found
  one more defect: at Landscape, Flipped the face was drawn upright while
  `rotation_map_touch()` turned every tap a half turn. `flush_cb` now turns
  each flushed area the way LVGL's rotation docs do for partial mode
  (`lv_display_rotate_area()` for where it lands, `lv_draw_sw_rotate()` for
  its pixels) through a turn buffer the size of the draw buffer, allocated
  beside it in `lvgl_port_init()`: 128,000 B more PSRAM on every dash-family
  board (the dash, its variants, dash7 and nightstand7, all with 8 MB PSRAM).
  The turn buffer is asked of PSRAM first, and of internal RAM only when the
  draw buffer itself fell back there (25,600 B), so a PSRAM glass never takes
  128,000 B of internal RAM for a turn. Matrix rotation was not an option:
  LVGL 9.5 accepts it in direct and full render modes only. A glass whose turn
  buffer cannot be allocated refuses a turn, logs it and keeps LVGL, the
  canvas and the touch layer landscape. `rotation_map_touch()` assumed the
  opposite turn to LVGL's at both portraits; its two portrait branches are
  swapped, so it now inverts the turn both majors draw and equals LVGL's own
  `lv_display_rotate_point()`. By that arithmetic Portrait puts the face's top
  along the panel's native left edge (the panel turned a quarter clockwise);
  `docs/hardware/display_settings.md` says so. LVGL 8.4 draws the same quarter
  turns, and the 8.4 code `lvgl_port.cpp` compiles is unchanged. After the fix
  the native run reads all four turns pixel for pixel equal to the LVGL-turned
  reference, with every raw tap on the pixel drawn under it, with PSRAM,
  without it and with the turn refused. `test_display_settings`'s oracle now
  quotes LVGL 9.5's `lv_display_rotate_area` and LVGL 8.4's `draw_buf_rotate`
  and holds `rotation_map_touch()` against `lv_display_rotate_point` (the old
  header fails 12,156 checks). The new `test_lvgl_port_turn` compiles the real
  `lvgl_port.cpp` against `fake_lvgl9/lvgl.h` (LVGL 9.5's display layer, its
  rotation code quoted from v9.5.0) and a recording `Arduino_GFX` and plays
  LVGL's partial render mode: at every quarter turn LVGL gets the right turn,
  no flush leaves the panel, every native pixel shows the logical pixel LVGL's
  turn puts there, a raw touch at each of the 384,000 pixels lands on the
  pixel drawn there, and a heap fake holds which heap each buffer comes from,
  the refusal included (1956 checks; 141 fail against the base tree, and ten
  mutations of the new port each fail it). The emulator dist does not move:
  every changed line of `lvgl_port.cpp` is in its LVGL 9 branch, and the
  emulator's objects are identical before and after. No CI job ran the real
  LVGL 9.5 renderer then; the quotes in `fake_lvgl9` were checked against the
  library only by the scratch native runs (since wave 15 the wasm job ends
  with `glass_turn_lvgl9.sh`, which renders the real port through LVGL 9.5.0,
  and `check_lvgl9_quotes.py`, which holds the quotes to the library; F225,
  not yet run in CI). Not done:
  `main.cpp` picks the face from the saved setting, not from the turn the
  port wore (F223, done in wave 15).
  Host-tested, and run natively against LVGL 9.5.0 outside CI; the ESP32
  builds and `pio check` are CI's. Not bench-tested: no turned dash, dash7 or
  nightstand7 has shown a face, the GT911's axes are unconfirmed against the
  panel's, and the turn's cost per flush on an S3 is unmeasured (F224). Found
  here: F223-F225.
- [x] **F206 [code] No browser probe reads the turned glass's splash, and
  `boot_probe.mjs` boots no turned glass.** `onboard_probe.mjs`'s
  `dash@portrait` walk boots through the first-meeting splash on 480x800, but
  its reads start at the first-boot line; `framesOnGlass` holds only that the
  splash's first frame was already turned, so the splash's lines and bird are
  read in no browser, and `boot_probe.mjs` boots the five native panels only.
  The splash on 480x800 is held on the host (`test_splash_layout`,
  `test_splash_scenes`) and was measured in a native full boot outside CI (653
  reads at both portrait turns and both ladders, no line off the glass or cut,
  the bird on the glass). Reading the splash in the turned walk, on a clock
  slowed from power-on, and a turned boot in `boot_probe.mjs` with its face's
  bird read by `birdPerch` would hold both off the framebuffer. Found by F184
  (#1762).
  *Done (#1762):* on every turned walk `onboard_probe.mjs` reads the splash
  from the firmware's first frame to the first-boot line, on a clock slowed to
  half speed from power-on (the harness's new `?timescale=`, set before
  power-on): every read on the turned glass, every line on it and none cut,
  and the bird clear of every line (`birdPerch`); the run must have seen the
  bird and each of the ten lines `kHello` types, whole (`splashCoverage`, the
  lines read from `story_scripts.h`). Each whole line is also read off the
  canvas, the first time it shows at full strength, with ink inside the box
  the firmware reports (`linesInk`), and every line must have been seen inked
  (`splashInk`). `boot_probe.mjs` boots each turned glass after the native
  ones (`dash@portrait`, and `nightlight@landscape` once its bundle is
  committed, F204): the turned size, every frame on it (`framesOnGlass`), and
  the face's bird on stage (`birdOnStage`; `birdPerch` alone passed a face
  with none) and clear of every line. On the committed dist `dash@portrait`
  passes both: 1797 splash reads in the final run, the bird in all of them,
  all ten lines seen whole and each inked in its box. A copy of the probe
  whose canvas reads are turned upside down fails it, and a turned boot whose
  bird reads as absent fails `birdOnStage`. In a native full boot outside CI
  the same reads pass on the portrait dash and the landscape nightlight, and a
  bubble hung 60 px low or pushed off the glass fails them. The ink read
  catches a splash drawn elsewhere or not at all, not a wrong glyph. Both
  probes read their source files through a call `probe_server.test.js` can see
  (dd38ce6e); on the integrated tree `boot_probe.mjs --flavor dash` boots both
  glasses, the turned one with its bird on stage. Run in local Chromium on the
  committed dist; not bench-tested.
- [x] **F222 [code] A runtime turn leaves the nightlight's companion off the
  glass.** On the first update after a turn rebuilds the face,
  `canary_mark_mood()`'s on-stage `lv_anim_del(s_bird, nullptr)` deletes
  `nightlight_ui_tumble()`'s translate animations, so the bird stays at the
  tumble's start offset (x=386 on 320x180 after 0->1, x=-118 on 180x320 after
  1->0, y=-131 after 1->3, measured in a native boot of build.sh's nightlight
  TUs calling `nightlight_request_rotation()` at 7 s; with that one delete
  skipped the bird lands at 226,29). The emulator HAL's side of the runtime
  turn is right (the shape announced, the framebuffer reshaped, the face
  rebuilt in landscape). Delete only the mark's own animations (by exec
  callback), or start the tumble after the first mood, and add a native check
  of a runtime turn. Most likely on hardware too (the same LVGL 8.4 calls);
  not bench-tested. Found by F204's review (#1762).
  *Done (#1762):* a runtime turn seats the nightlight's companion on its
  perch. `canary_mark_mood()` no longer deletes every animation on the bird:
  it stops only the mark's own motion (`stop_motion()`: the breath and the hop
  by their exec callbacks, and the wing it owns), so a host's animation on the
  bird, the tumble `nightlight_ui_tumble()` starts right after a turn rebuilds
  the face, runs to its end through the first mood, a later mood change and a
  hide. Picked over starting the tumble after the first mood, which would
  still let a mood change inside the 700 ms tumble end it. The new
  `canary-local/emulator/test/runtime_turn.sh` builds build.sh's whole
  nightlight TU list (the real `main.cpp`, the emulator's sources, LVGL 8.4)
  with g++ and boots it natively on a virtual clock: a boot at each rotation
  gives the perch, and a boot turned at 7 s from every rotation to every other
  through `nightlight_request_rotation()` (the mailbox the app's picker
  writes) must announce the new shape, show the companion tumbling, then
  breathe exactly on that perch (x 226, y 27..31 on 320x180; x 42, y 200..204
  on 180x320). On the old `canary_mark.cpp` all 12 turns fail, off the glass
  on 8 (x 386 after 0 to 1, x -118 after 1 to 0, y -133..-129 after 1 to 3;
  the item's -131 is mid-breath) and over the clock on the other 4; with the
  fix all 72 checks pass, and two runs print the same bytes.
  `canary-local.yml`'s wasm job runs it right after `glass_turn.sh`, bounded
  at ten minutes; it has no `if:`, so it runs whenever the steps before it
  pass, and none of them reads the dist. `onboard.test.js` holds it to
  build.sh's nightlight TU lists, to its defines (derived from build.sh with
  bash for that flavor and required word for word in `runtime_turn.sh`'s
  compiles), language flags, third-party pins and the mailbox; seventeen
  mutations each fail it. `test_canary_mark_seat` (fake_lvgl) holds which
  animations a mood change stops: the host's slide survives the first mood, a
  mood change mid-slide and a hide; after Happy and a greeting, a mood change
  leaves one breath on the bird and nothing on the wing (6 checks fail on the
  old file). The emulator dist moves for all six display flavors
  (`canary_mark.cpp` is in each). On the committed dist, which predates this
  change, `boot_probe.mjs` passed its boots and `onboard_probe.mjs --flavor
  nightlight` both walks in the package's run (they hold the unturned paths,
  not the fix); a native full boot of the dash draws all 27 dumps identical
  before and after. Most likely the same on the C3 board (the same LVGL 8.4
  calls); not bench-tested. Found here: F242-F244.
- [x] **F223 [code] The dash face follows the saved rotation, not the turn the
  port wore.** `main.cpp`'s `dash_is_portrait()` reads `settings().rotation`.
  When `lvgl_port_set_rotation()` refuses a turn because the glass has no turn
  buffer (F205), the portrait column `portrait7_ui` is still built, on the
  800x480 canvas. That happens when, at boot, a PSRAM glass's 128,000 B PSRAM
  request for the turn buffer is refused, or when a glass whose draw buffer
  fell back to internal RAM has a 25,600 B request refused by both PSRAM and
  internal RAM. Reading `rotation_is_portrait(lvgl_port_rotation())` there
  would keep the face with the glass. The emulator compiles `main.cpp`, so the
  dist moves for the dash flavor. Found by F205 (#1762).
  *Done (#1762):* the dash face follows the turn the port wore. The choice
  moved into `canary/ui/dash_face.h` (`dash_face_portrait()`:
  `rotation_is_portrait(lvgl_port_rotation())`), and `main.cpp`'s
  `dash_is_portrait()` returns it, so the face build, its updates, touch
  routing and the rebuild tracker follow the glass: a glass whose turn buffer
  was refused stays landscape and builds the landscape poster on its 800x480
  canvas. `lvgl_port_rotation()` already reported the turn worn; its comment
  and `lvgl_port.h` now say the face follows it. `test_lvgl_port_turn` plays
  `main.cpp`'s boot for every saved rotation with the turn buffer granted,
  refused by PSRAM and refused by both heaps, and holds the face to the canvas
  (1984 checks, rerun green on the integrated tree; with the old read 8 fail,
  the two portraits under both refusals). `onboard.test.js` pins `main.cpp` to
  the header and to reading the saved rotation only to hand it to the port (it
  finds those reads under two spellings, from the review). The emulator's LVGL
  8.4 port never refuses a turn, so its dash draws what it drew (a native full
  boot gives all 27 dumps identical); the dist moves for the dash flavor,
  which compiles `main.cpp`'s dash branch. Host-tested; the ESP32 builds and
  `pio check` are CI's; no turned dash glass has been seen (F224). Found here,
  by the review: F245 (Settings and the app still show the saved turn on a
  glass that refused it).
- [ ] **F224 [human] No turned dash glass has been confirmed on real glass.**
  F205 turns LVGL 9.5's flushes and maps touch by arithmetic held on the host
  and in a native LVGL 9.5 run outside CI. On a dash, dash7 or nightstand7,
  confirm three things: that Portrait shows the face with its top along the
  panel's native left edge and Portrait, Flipped along the right; that a tap
  lands under the finger at all four turns (the GT911's axes against the
  panel's); and what a full-screen turned repaint costs (the portable
  `lv_draw_sw_rotate` loops read and write PSRAM column by column, 384,000
  pixels per full frame). If it is slow, the code options are turning straight
  into the RGB framebuffer (the second partial-mode example in LVGL's rotation
  docs, with the cache write-back Arduino_GFX does) or a smaller internal-RAM
  turn buffer. No row in `hardware_verification_checklist.md` names it yet.
  Found by F205 (#1762).
- [x] **F225 [code] No CI job runs the real LVGL 9.5 renderer.** The display's
  LVGL 9.5 builds (the dash family, nightstand-c6) are what ship, but every
  host and browser check of what they draw runs fake LVGL or LVGL 8.4
  (`glass_turn.sh`, the emulator). `test_lvgl_port_turn` holds `lvgl_port.cpp`
  against LVGL 9.5's rotation code quoted into `fake_lvgl9/lvgl.h`, and
  nothing re-checks those quotes against the library. A sibling of
  `glass_turn.sh` would close that gap: fetch LVGL v9.5.0 the way
  `glass_turn.sh` fetches 8.4 and render a scene through the real port at
  every quarter turn, compared with a plain display of the logical size
  (F205's native run did exactly that, outside CI). It would also hold the
  paths only 9.5 takes, such as the splash tail F185 says only 9.5 draws.
  Found by F205 (#1762).
  *Done (#1762):* CI is wired to render the dash glass through the real LVGL
  9.5, and its wasm job first ran the step on 5755ce2b: 169 checks passed, the
  quote check with them, after the onboard probe's red step (A63).
  `canary-local/emulator/test/glass_turn_lvgl9.sh`, `glass_turn.sh`'s sibling,
  reads the release from `sketch.yaml`'s `dash-core3` profile (`lvgl (9.5.0)`,
  the pin the core-3 Arduino dash and modes releases install) and refuses to
  run when another profile names a different 9.x, when the PlatformIO dash env
  does not ask for `^9.5.0` with the display's `lv_conf.h`, or when the
  Arduino sketch's `lv_conf.h` copy differs. It fetches v9.5.0 into the
  gitignored `emulator/test/third_party/lvgl`, builds it with the display's
  own `lv_conf.h`, and links the real `lvgl_port.cpp` (dash config, LVGL 9
  branch) with a recording `Arduino_GFX` over the native 800x480 framebuffer.
  `glass_turn_lvgl9_test.cpp` renders a scene (text in three faces, a bordered
  rounded card, a rounded-cap arc, a QR code, a translucent band, the splash's
  turned tail square) at every quarter turn and holds: LVGL gets the turn
  `glass_settings.h` names; nothing flushed falls off the panel, and every
  native pixel equals the same scene on a plain display of the logical size
  under the turn the port draws; the turn's own refresh repaints the whole
  panel; a partial update (areas right of logical x 0 and narrower than the
  canvas) leaves the panel equal to the changed scene rendered whole, and
  putting the objects back leaves the scene again; every native pixel maps
  through `rotation_map_touch()` to the logical pixel drawn there; a glass
  whose turn buffer was refused stays landscape (169 checks). With F205's turn
  removed it fails 22 checks; a source stride read from the canvas width fails
  8, and so does an area placed as if it began at logical x 0 (both passed
  before the partial pass). `check_lvgl9_quotes.py` holds
  `fake_lvgl9/lvgl.h`'s quoted functions token for token to the fetched
  source, and LVGL's own RGB565 dispatch and stride default, the facts the
  fake's stand-ins assume (the stand-ins themselves are held by
  `test_lvgl_port_turn`); its `--self-test` requires eleven drifts and a
  missing function to fail. On 9.5.0 the tail's styles draw a 145 px diamond
  hung from the square's top-left corner, the library's half of F185;
  `splash.cpp`'s own tail is not rendered through 9.5 (F246).
  `canary-local.yml`'s wasm job ends with it, after its own cache keyed on
  `sketch.yaml`, and both steps run whether the steps before them passed or
  failed (`success() || failure()`); `glass_turn_lvgl9.test.js` pins that and
  the pin. It runs natively on a 64-bit host, not on an ESP32. The PlatformIO
  dash7, nightstand7 and nightstand-c6 rows ask for `^9.5.0`, which admits
  v9.6.0 (F247), and nightstand-c6's 9.x path is rendered by no check (F248).
  The emulator dist does not move. Not run in CI yet (a cold cache clones
  about 267 MB); not bench-tested. Found here: F246-F248.
- [ ] **F242 [code] The nightlight's tumble never wears its Startle.**
  `nightlight_ui_tumble()` calls `canary_mark_react(CanaryReact::Startle)` on
  the bird `nightlight_ui_create()` has just built, whose mood stays Hidden
  until the next `nightlight_ui_update()`; `canary_mark_react()` refuses a
  hidden bird, so no runtime turn shows the Startle the face's comment
  promises. Firing it after the first mood (a pending flag the update
  consumes, still refused asleep and rationed) would give the tumble its
  reaction, held by a host test of the order and by `runtime_turn.sh`. Read
  from source. Found by F222 (#1762).
- [ ] **F243 [code] No browser probe turns a running glass.**
  `runtime_turn.sh` holds the sources natively, but the dist's nightlight
  bundle is never turned at runtime in Chromium: `emu_preset_rotation()`
  refuses after power-on and the harness has no binding that writes the
  rotation mailbox. An `emu_request_rotation()` binding and a `boot_probe.mjs`
  step that turns `nightlight@landscape` back to portrait and reads
  `birdOnStage` once the tumble is over would hold the bytes the Lab serves.
  Found by F222 (#1762).
- [ ] **F244 [code] A hop the canary mark starts over a running breath fights
  it for the bird's y.** `start_hop()` starts `s_bob` with `hop_cb` while the
  breath runs with `bob_cb`; LVGL's `lv_anim_start()` replaces only an
  animation with the same var and exec callback, so both run, and LVGL 8.4
  inserts the newer one at the head of its list and runs the list from the
  head (`lv_anim.c`), so each time the breath's value changes it overwrites
  the hop's y for that frame. The idle flourish's hop, Startle and Joyful all
  start a hop over a breath. Read from source, not measured; stopping the
  breath before a hop (or the hop before `hop_done`'s breath) would settle it,
  held by a fake_lvgl test of the animations a hop leaves running. Found by
  F222 (#1762).
- [ ] **F245 [code+decision] A refused turn is still shown as the glass's
  orientation.** When `lvgl_port_set_rotation()` refuses a turn for want of a
  turn buffer (F205), the face stays landscape (F223), but Settings >
  Orientation (`settings_ui.cpp`'s `build_display()`) checks the saved
  rotation under "The layout follows the turn.", and the settings GET
  (`glass_web.cpp`) reports it to the app; the only signal is the serial line
  "No rotation buffer, staying landscape". Decide whether to keep the saved
  choice and say on the glass that it could not be worn, or to show the turn
  the port wore (`lvgl_port_rotation()`), then hold it with a host test of the
  Display page with the turn buffer refused. From code. Found by F223's review
  (#1762).
- [ ] **F246 [code] `splash.cpp`'s speech-bubble tail is not rendered through
  the real LVGL 9.5.** `glass_turn_lvgl9.sh` draws the tail's styles (12 px,
  radius 2, border 1, turned 450) and holds a 145 px diamond hung from the
  box's top-left corner on 9.5.0, but `splash.cpp`'s own tail, aligned under
  the bird on each canvas (the dash's 800x480 and 480x800, nightstand-c6's
  172x320), is held only as an object box against fake LVGL
  (`test_splash_scenes`). Compiling `splash.cpp` and `canary_mark.cpp` into
  the harness (a Preferences stand-in, the splash clock driven to each beat)
  would read the tail off the frame and show where F185's centered pivot or
  triangle would land. Found by F225 (#1762).
- [ ] **F247 [code+decision] The PlatformIO LVGL 9 envs float past the release
  CI renders.** `canary-display.ini` asks for `lvgl/lvgl@^9.5.0` in the dash
  env (dash7 and nightstand7 inherit it) and in nightstand-c6; a caret admits
  any 9.x, and v9.6.0 is tagged, so those three releases (built by `pio run`)
  most likely build LVGL 9.6 while the dash and modes releases (arduino-cli,
  9.5.0) and every check build 9.5.0 (the PlatformIO registry was not
  reachable to confirm). In a run outside CI the same `lvgl_port.cpp` passed
  the harness's checks (115, before its partial-update pass) on 9.6.0, with
  deprecation warnings; 9.6 also moves `lv_version.h` and
  `lv_draw_sw_utils.c`, so `glass_turn_lvgl9.sh` and `check_lvgl9_quotes.py`
  stop (exit 2) on a bump until they learn the new layout. Pin the PlatformIO
  rows exactly (9.5.0, as `sketch.yaml` does) or render both releases. Found
  by F225 (#1762).
- [ ] **F248 [code] nightstand-c6's LVGL 9.5 path is rendered by no check.**
  `glass_turn_lvgl9.sh` builds the dash config only; the C6 nightstand
  compiles `lvgl_port.cpp`'s 9.x branch as the nightstand flavor (a lean
  `lv_conf` without the 36 and 48 px faces, a static internal draw buffer, no
  turn) and is held only by its ESP32 build. A second build of the harness in
  that config, with a scene that skips the 36 px face, would hold its flushes,
  whole and partial, against a plain 172x320 display. Found by F225 (#1762).
- [ ] **F250 [code] `io/orientation.h` describes `Orient::R90` two ways.** Its
  classifier picks R90 when gravity pulls toward the glass's right edge, the
  device turned clockwise, and R90's own comment says "turned clockwise", but
  the same comment says "onto its left edge", and the enum's header says the
  value is how far the face must rotate clockwise; both describe the opposite
  turn. `display_1in47.cpp`'s landscape MADCTL bytes are also not yet
  bench-verified. The Lab's 3D case (A56) turns by the classifier's reading.
  Correct the comments, and check the bytes on a bench. Found by A56 (#1762).
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
- [x] **F130 [code] canary-vision's `dwell_ended` publishes `dwell_ms` 0.**
  `PresenceFSM::tick` clears `dwelling_` before the snapshot that
  `publish_event_json` reads, so no event row reports how long the dwell
  lasted; `dwell_started` reads 0 too, since `dwell_start_ms_` is set on its
  own tick. `visit_ms` has a latch (`last_visit_ms_`) for the leave-side
  events; the dwell has none, so only the heartbeat's state row ever carries a
  running dwell. HA's `custom_components/securacv/tests/test_signature.py`
  fixture shows a Vision `dwell_started` with `dwell_ms` 5000, a value no
  device sends. Latch the dwell length for `dwell_ended`, or document
  `dwell_ms` as heartbeat-only and correct the fixture. A firmware change:
  compile-tested by CI. Found by A37's review (#1762).
  *Done (#1762):* latched. `PresenceFSM` keeps `ended_dwell_ms_`, the length
  of the dwell the latest tick ended, on the running dwell's clock
  (`now_ms - dwell_start_ms_`). `tick()` zeroes it first, so only the tick
  that emits `dwell_ended` reports it (with any heartbeat before the next
  frame), and `snapshot()` returns it as `dwell_ms` when not dwelling. So
  `dwell_ended` and the state row sent with it carry the dwell's length,
  counted to the frame that declared the person gone, so it includes the lost
  timeout (`lost_ms`: 1.5 s by default, 4 s in the `litter_box` preset, up to
  60 s), as `visit_ms` always has; `presence_ended` and `interaction_likely`
  carry 0. `dwell_started` stays 0, which is honest: the dwell starts on that
  frame. `firmware/projects/canary-vision/README.md` gains a table of what
  `presence_ms`, `dwell_ms`, `visit_ms` (events rows only; the state row has
  no such key) and `voxel` mean on each row, and says both lengths include the
  lost timeout. HA's `test_signature.py` fixture, a Vision `dwell_started`
  with reason `dwell` and `dwell_ms` 5000, is now a `dwell_ended` (`dwell_ms` 6600)
  and an `interaction_likely` (reason `dwell_then_left`, `visit_ms`
  16700), keyed and valued as `publish_event_json` writes them; both verify.
  `homeassistant/automations/securacv_vision_presence.yaml`'s lingering alert
  printed `dwell_ms` on `dwell_started` and `interaction_likely`, which always
  carry 0, so it always said "dwell 0s"; it now prints how long the person has
  been in view (`presence_ms`) or how long the visit lasted (`visit_ms`), held
  by `scripts/tests/test_vision_automation_clocks.py`, which fails on the old
  recipe. A new host suite,
  `firmware/tests_host/test_vision_presence_fsm.cpp`, links `presence_fsm.cpp`
  and `voxel_tracker.cpp` verbatim, fails on the old FSM at the `dwell_ended`
  assertion, and holds both lengths past the last sighting plus the lost
  timeout. `gen_vision.py` pins the new snapshot formula and latch, and the
  Vision pane's note no longer says `dwell_ms` is 0 on every event row. The
  emulator's Vision core compiles `presence_fsm.cpp`, so the dist moves (see
  A39). A firmware change: host-tested, compile-tested by CI, not
  bench-tested. Found here: F153, F154 and HA26.
- [x] **F152 [code] canary-vision emits `interaction_likely` after almost
  every visit but the first.** `PresenceFSM::tick` never resets its
  `VoxelTracker` at `presence_started` (only `PresenceFSM::reset()` does, at
  boot), so `stable_enter_ms()` keeps the time an earlier visit's cell
  settled. On the second frame of any later visit
  `now_ms - voxel_tracker_.stable_enter_ms() >= ZONE_INTERACTION_MS` holds,
  and the visit ends in `interaction_likely` (`zone_interaction_then_left`)
  however short it was. A probe of the tree's `presence_fsm.cpp` at 100 ms
  frames shows a fresh FSM's 1 s visit ending without it, and after one 4 s
  visit both a 1 s revisit and a 0.5 s pass in another cell ending with it.
  The Vision lingering alert and the litter-box visit-completed recipe page on
  that event. Reset the tracker (or its enter time) at `presence_started`, and
  decide what a new visit's `presence_started` voxel names (today the previous
  visit's cell, documented and pinned by `test_vision_core_bindings.cpp` since
  A39, which a fix must update). Found by A39's review (#1762).
  *Done (#1762):* `PresenceFSM::tick` resets its `VoxelTracker` on the frame
  that starts a visit, before that frame's update, so the first sighting seeds
  the settled cell and the interaction clock (`stable_enter_ms()`) starts with
  the visit: a visit qualifies for `zone_interaction_then_left` only by
  staying in one settled cell for `ZONE_INTERACTION_MS` of its own time. A new
  visit's `presence_started` voxel names the cell the person was first seen in
  on that visit, as the first visit after boot always did; the rows between
  visits still name where the last visit settled. A qualified visit seen again
  on the frame right after its `presence_ended` (the one frame its
  `interaction_likely` goes out on, which that sighting turns into the next
  visit's `presence_started`) keeps its report: before F152 the next
  fragment's stale clock usually sent one late, with the fragment's
  `visit_ms`, and the reset alone sent none (found in review); the FSM now
  owes it and sends `interaction_likely` with the ended visit's reason and
  `visit_ms` on the frame after that `presence_started`, inside
  `INTERACTION_AFTER_LEAVE_WINDOW_MS` (that row is the new visit's frame:
  present, its confidence and cell). The Vision README, the device guide's
  litter-box reading, the bindings comment and the pane note (`gen_vision.py`,
  `vision.json`) say so, and `gen_vision.py` dies unless the tracker is reset
  in exactly those two places.
  `firmware/tests_host/test_vision_presence_fsm.cpp` (both builds) drives a
  fresh 1 s visit, a 1 s revisit and a 0.5 s pass in another cell after a 4 s
  visit, a long settled visit after short ones, and five back-to-back cases;
  the revisit, pass, zone-window and four back-to-back tests fail on the FSM
  before them. A39's pin in `test_vision_core_bindings.cpp` now holds the
  opposite through the Lab's ABI, as does a back-to-back case there, and
  `vision.test.js` gains both on the WASM core. The emulator dist moves
  (`dist/canary-vision-core.js` only) and is rebuilt in this PR by CI's pinned
  emsdk after this ledger lands; the rebuilt dist was not run here. Until then
  `vision.test.js`'s F152 test fails on the committed dist and passes on this
  tree's sources with `LAB_CORES=native` (A40, which closes the opt-in this
  item's package left open): run on the integrated tree, `vision.test.js`,
  `eyes.test.js` and `audio.test.js` pass 46 of 47 on the dist and 47 of 47
  natively. A firmware change: host-tested, compile-tested by CI, not
  bench-tested.
- [ ] **F153 [decision] Should canary-vision's dwell and visit lengths end at
  the last sighting?** `dwell_ended`'s `dwell_ms` (`now_ms - dwell_start_ms_`)
  and `visit_ms` (`now_ms - presence_start_ms_`) are taken on the frame that
  declares the person gone, so both include the lost timeout: 1.5 s by
  default, 4 s in the `litter_box` preset, up to 60 s. Taking them from
  `last_seen_ms_` would report how long the person was seen; today they report
  how long the device held them present, and the README says so since F130. A
  change moves the device rows, the Lab core (the emulator dist),
  `test_vision_presence_fsm.cpp`, `test_vision_core_bindings.cpp` and the HA
  recipes that print `visit_ms`. Found by F130's review (#1762).
- [x] **F154 [code] canary-vision's `DWELL_END_GRACE_MS` does not do what the
  README's FSM diagram says.** With a grace longer than the lost timeout,
  `PresenceFSM::tick` clears `dwelling_` silently and emits `presence_ended`
  on the same tick, so the dwell ends with no `dwell_ended` and its length is
  never reported; the diagram's
  `Dwelling --> Present: (optional) dwell_end_grace` is a path the code does
  not take. The shipped grace is 0, so no device does this today. Make the
  grace extend the dwell past the lost timeout and still emit `dwell_ended`,
  or drop the knob and the diagram edge. Found by F130 (#1762).
  *Done (#1762):* made to do what the diagram says rather than dropped, as
  the brief asked for host tests with a nonzero grace. A dweller is held
  present and dwelling until unseen for longer than the lost timeout or
  `DWELL_END_GRACE_MS`, whichever is longer: a dweller seen again within the
  grace keeps the dwell with no event, and once it has passed `dwell_ended`
  fires with the dwell's length (which then includes the grace),
  `presence_ended` follows on the next frame if nobody is seen on it, and
  `interaction_likely` (`dwell_then_left`) after it. A stay that never
  dwelled is still let go at the lost timeout, and a lost timeout longer than
  the grace governs. The
  grace stays compile-time and 0 in every shipped build, so no device changes;
  `include/canary/config.h` takes `-DVISION_DWELL_END_GRACE_MS=<ms>`, and
  `firmware/tests_host` builds `test_vision_presence_fsm.cpp` a second time
  with 4000 ms (held past the lost timeout then `dwell_ended` with its length,
  back within the grace, dwellers only, a longer lost timeout governs); the
  first two fail on the old FSM, which sent `presence_ended` at the lost
  timeout and no `dwell_ended`. The README's diagram names each edge's event,
  takes Dwelling to Present on unseen > max(lost_timeout, dwell_end_grace)
  with `dwell_ended`, and drops the direct Dwelling to Idle edge, which the
  code no longer takes (it took it with a grace longer than the lost timeout:
  the defect). The flavor config's `CONFIG_DWELL_END_GRACE_MS` is still read by
  nothing (F182). Not changed: a dweller seen on the frame between
  `dwell_ended` and `presence_ended` starts a second dwell in the same visit
  (F186, found by the review, as old as the FSM). Host-tested, compile-tested
  by CI, not bench-tested. Found here: F182 and F186.
- [ ] **F131 [code+decision] The Sense count follows every radar frame, so a
  room that empties records an `occupancy_changed` to 0 while still
  Present.** `mr60_presence.cpp` sets `count_` from each presence frame
  (`has_target ? bucket_of(...) : Zero`) while the state waits out its
  debounce and clear timers, and `main.cpp`'s `drive_fsms` records
  `occupancy_changed` on any count change while Present. A g++ probe of the
  real `mr60_presence.cpp` shows it: the first empty frame after Present
  reports `count_changed` with the state still Present, so the device signs
  `occupancy_changed` with occupants "0" and presence "present" 1.5 s before
  `presence_cleared`, and a target that drops out for one frame signs two
  occupancy events. The same tracking publishes a clear state row counting one
  occupant during the debounce window. The comment says occupancy is "only
  newsworthy while someone is here". Decide whether the count should follow
  the debounced state; the Lab's radar lab keeps the count until Clear and its
  MQTT note names the omission. A firmware change: host-testable against
  `mr60_presence.cpp`, compile-tested by CI. Found by A30's review (#1762).
- [x] **F181 [code] Discovery's device object truncates for long device ids,
  which makes every entity's config invalid.** `publish_discovery` builds
  `devObj` (256 bytes in canary-vision and canary-sense, 384 in
  canary-sentinel) with the device id twice plus the model string, and the NVS
  id is accepted up to 47 characters (`runtime_config.h` `device_id[48]`), so
  the object is cut mid-JSON from a 46-character id on the Vision, a
  44-character id on the Sense default build and a 39-character id on its
  wellbeing build (model "Canary Sense Wellbeing (XIAO ESP32-C6 + MR60BHA2)");
  every discovery payload that embeds it is then invalid JSON, which Home
  Assistant ignores. Measured by formatting the real format strings, not on a
  device. Size the buffers from the longest id and model, or refuse an id that
  cannot fit when it is provisioned.
  `scripts/tests/test_ha_discovery_binary_sensors.py` cuts `devObj` at its
  declared size the way snprintf does, so it holds the payload buffers around
  it, not this. Found by HA25 (#1762).
  *Done (#1762):* sized, not refused, so an id the flasher and NVS already
  accept stays valid. Each product's `src/ha/ha_discovery.cpp` names its
  device object format (`kDeviceObjectFormat`) and derives
  `kDeviceObjectMost`, the most the object can take: the format without its
  five `%s`, the longest id `runtime_config.h` accepts (47 characters) twice,
  that build's manufacturer, model and firmware version, and the terminator.
  `devObj` is 320 bytes on canary-vision and canary-sense (worst 259 and 273,
  the latter on the wellbeing build) and stays 384 on canary-sentinel (worst
  293, the Perimeter Demo model), each with a `static_assert` against
  `kDeviceObjectMost`, so a flavor whose model would overrun it fails its own
  build (checked on the host with g++ against every flavor's config: the
  assert fires one byte below each product's worst case and holds at it).
  `scripts/tests/test_ha_discovery_binary_sensors.py` formats `devObj` and
  `availObj` whole, with their real format strings, for a 47-character id and
  every flavor's model, parses them and holds each to its declared size (on
  the old sizes three subtests fail: 259, 263 and 273 bytes needed against
  256), and formats every announcement whose arguments are the id, a topic
  (read from `topics.h`) or those objects (15, 16 and 15 announcements),
  holding each to its buffer and to valid JSON; the tightest is the Vision's
  Uptime sensor, 19 bytes to spare in its 768. The table-driven number
  entities and the Vision's watch profile select are not formatted there
  (F203). The availability object needs at most 200 of its 256. Measured by
  formatting the real format strings and by the compile-time assert on the
  host, not on a device; the three firmware compiles are CI's; not
  bench-tested. Found here: F203.
- [ ] **F182 [code+decision] canary-vision's flavor config names interaction
  settings nothing reads.** `firmware/configs/canary-vision/default/config.h`
  defines `CONFIG_DWELL_END_GRACE_MS`,
  `CONFIG_INTERACTION_AFTER_LEAVE_WINDOW_MS` and `CONFIG_ZONE_INTERACTION_MS`
  beside the settings they describe, but the firmware reads the project
  `include/canary/config.h`'s own constants, so editing the flavor file
  changes nothing (only `CONFIG_LOST_TIMEOUT_MS` and `CONFIG_DWELL_START_MS`
  have a reader, `gen_flash.py`, and those seed values duplicate the project's
  too). Decide which file owns them, then wire the project constants to the
  flavor macros or drop the dead ones. Found by F154 (#1762).
- [x] **F186 [code] A Vision dweller seen on the frame between `dwell_ended`
  and `presence_ended` starts a second dwell in the same visit, so the
  lingering alert pages twice.** After `dwell_ended`, `PresenceFSM::tick`
  clears `dwelling_` and leaves `presence_` set until the next frame ends the
  stay. A sighting on that frame keeps the visit and, the visit being older
  than `DWELL_START_MS`, takes the `dwell_started` branch again
  (`presence_fsm.cpp`). A scratch probe linking the real FSM, with the
  shipped grace of 0, shows it (probe P8, on the probe's clock):
  `dwell_started` at 11.0 s, `dwell_ended` at 13.5 s, a second
  `dwell_started` at 13.6 s and a second `dwell_ended` at 15.6 s, then one
  `presence_ended` and one `interaction_likely`.
  `homeassistant/automations/securacv_vision_presence.yaml` pages "Lingering
  detected" on each `dwell_started`. As old as the FSM: the same probe on the
  base gives the same events. A nonzero grace (F154) leaves the same one-frame
  gap. Fix: let a sighting on that frame either resume the ended dwell or
  end the visit first, and hold the choice with a host test. Not probed on a
  device. Found by F154's review (#1762).
  *Done (#1762):* the visit ends first. The frame after `dwell_ended` now
  sends `presence_ended` whatever it shows (`PresenceFSM`'s `leave_owed_`, set
  by `dwell_ended`), and a sighting on it is held and opens the next visit on
  the frame after: `presence_started` there, then the ended stay's owed
  `interaction_likely` (`dwell_then_left`, F152's path) on the frame after
  that, the same events a return one frame later always gave. Chosen over
  resuming the ended dwell, which would have left a `dwell_ended` row whose
  `dwell_ms` named an end that did not happen and sent a second `dwell_ended`
  for the same dwell: ending the visit keeps one dwell per stay and every
  `dwell_ended`'s `dwell_ms` the final length of its dwell, and treats a
  dweller unseen past the lost timeout (or the grace) as gone, as a walk-by
  always was. When the next frame is empty, the held sighting opens the visit
  (presence true with that frame's confidence, 0, and the sighting's cell,
  held through the lost timeout counted from the sighting); when the next
  frame has a sighting of its own, that sighting opens it and the held one is
  dropped. The new visit's clocks start on the frame that sends its
  `presence_started`, one frame after the sighting (F153 asks whether lengths
  should follow sightings). `reset()` clears the owed `presence_ended` and the
  held sighting with everything else. Probe P8 now reads `dwell_started` 11.0
  s, `dwell_ended` 13.5 s, `presence_ended` 13.6 s, `presence_started` 13.7 s,
  `interaction_likely` 13.8 s, `presence_ended` 15.6 s. The visit boundaries
  move into `open_visit` and `end_visit`, so the source pins in
  `gen_vision.py`, `vision.test.js` and `test_vision_dashboard_voxel.py`
  follow the new form with the same intent (`vision.json` unchanged).
  `test_vision_presence_fsm.cpp` drops the test that pinned the second dwell
  and, in both builds (grace 0 and 4000 ms), gains P8, the one-frame case, the
  back-for-good case (a second dwell only in the next stay), a gap sighting
  followed by one in another cell, `reset()` right after `dwell_ended` and
  right after a gap sighting, and a table of every way back after a dwell (80
  walks with grace 0, 180 with the grace). Every one fails on the FSM before
  it, each run alone in both builds. Ten mutations of the fix each fail one of
  them; three of those (the held branch's guard forced true, either new
  `reset()` line dropped) were found by review and passed the first version of
  these tests, and in the Lab, which resets its core on every scene change and
  camera start, they would have shown a cell the person had left, a
  `presence_ended` with no visit open, or a phantom visit at cell -1,-1.
  `test_vision_core_bindings.cpp` drives P8, the gap-then-elsewhere case and
  `vision_emu_reset` at both points through the Lab core's ABI. The README's
  FSM diagram gains a Leaving state, and its text says what that frame does
  and which sighting opens the next visit. HA's Presence sensor and the fleet
  beacon now go off and on at that boundary, as for any new visit. The
  emulator dist moves (`dist/canary-vision-core.js` only): CI's pinned emsdk
  rebuilds it in this PR after this ledger lands, and the rebuilt dist was not
  run here. Until then `vision.test.js`'s F186 test fails on the committed
  dist (31 of 32, rerun on the integrated tree) and passes with
  `LAB_CORES=native` (32 of 32); `eyes.test.js` and `audio.test.js` pass both
  ways, and `native_cores.test.js`'s opt-in parity names the stale dist at
  Vision tick 77. A firmware change: host-tested, compile-tested by CI, not
  bench-tested. Found here: F202.
- [x] **F202 [code] A Vision dwell whose subject is last seen on the
  `dwell_started` frame reports no `dwell_then_left`.** `PresenceFSM::tick`
  returns on the frame that sends `dwell_started` before `if (dwelling_)
  dwell_latch_ = true;`, and the latch is set only on later sighted frames, so
  a subject gone right after the dwell starts leaves it false. A probe linking
  the tree's FSM (the same before F186), 100 ms frames, seen until the
  `dwell_started` frame: settled in one cell, the visit sends `dwell_started`,
  `dwell_ended`, `presence_ended` and `interaction_likely` with reason
  `zone_interaction_then_left`; with its settled cell changing every few
  frames it sends no `interaction_likely` at all, although it dwelled and
  `dwell_ended` fired; seen one frame longer, it reports `dwell_then_left`.
  Latch the dwell where it starts (or read the ended dwell at the leave) and
  hold it in `test_vision_presence_fsm.cpp`; the lingering alert and the
  litter-box visit-completed recipe page on that event. A firmware change:
  host-testable, moves the emulator dist. Found by F186 (#1762).
  *Done (#1762):* latched where it starts. `PresenceFSM::tick` sets
  `dwell_latch_` in the branch that sends `dwell_started`, so a stay that
  dwelled reports `interaction_likely` (`dwell_then_left`) however soon after
  the dwell starts the person is last seen; the per-frame latch it replaces is
  gone (with the latch set at the start it could add nothing). The item's
  probe reproduced on the FSM before the fix, in both builds.
  `test_vision_presence_fsm.cpp` (both builds, grace 0 and 4000 ms) drives 100
  ms frames, settled and moving, last seen on the `dwell_started` frame and
  one and two frames after (`dwell_then_left`) and on the frame before (no
  dwell, so the zone rule alone); the cases last seen on the `dwell_started`
  frame fail on the old FSM. `test_vision_core_bindings.cpp` drives both walks
  through the Lab core's ABI and `vision.test.js` on the WASM core; both fail
  before the fix. The Vision README says a visit counts as dwelled from the
  frame that sends `dwell_started`, and the sandbox's linger blurb now holds
  for a person who leaves on that frame too (`vision.json` unchanged). The
  emulator dist moves (`dist/canary-vision-core.js` only): CI's pinned emsdk
  rebuilds it in this PR after this ledger lands, and the rebuilt dist was not
  run here. Until then `vision.test.js`'s F202 and A42 tests fail on the
  committed dist (32 of 34, rerun on the integrated tree) and pass with
  `LAB_CORES=native` (34 of 34); `native_cores.test.js`'s opt-in tick-for-tick
  parity, run by hand, names the stale dist at Vision tick 333. A firmware
  change: host-tested, compile-tested by CI, not bench-tested.
- [x] **F203 [code] The discovery fit test does not format the table-driven
  announcements.** `scripts/tests/test_ha_discovery_binary_sensors.py` holds
  every fixed announcement of canary-vision, canary-sense and canary-sentinel
  to its buffer for a 47-character id (F181), but the Vision's and the Sense's
  number entities and the Vision's watch profile select take their fields from
  tables inside `publish_discovery` that the test does not parse. Those check
  their own `snprintf` result and skip a payload that does not fit, so an
  entity would go missing from Home Assistant with only a serial log line.
  Parse the tables (or move them where the test can read them) and hold those
  payloads too. Found by F181 (#1762).
  *Done (#1762):* parsed. `scripts/tests/test_ha_discovery_binary_sensors.py`
  reads `NumberEnt numbers[]` (canary-vision, canary-sense) and
  `WATCH_PROFILES[]` (`detect_profiles.h`) as the compiler does: member order,
  each row's literals, `topics.*`, named integer constants, and a flavor's
  `#if defined(X) && X` rows only where that flavor defines X (the Sense's six
  vitals numbers on its wellbeing build only). It formats every number row,
  and the select with its options, for the 47-character id and every flavor's
  model, parses each, checks its unique_id, model, bounds and options, and
  holds it to its buffers (the Vision's `unitField[64]` and `options[192]`);
  every table site the fixed-announcement test leaves out must be formatted
  here. A self-test shrinks each buffer to one byte under its longest payload
  and requires the check to name it, and another requires five edits it cannot
  follow to be refused by name. Nothing was cut: the tightest is the Sense
  wellbeing build's Presence debounce number, 931 bytes with its terminator in
  1280, and the Vision's options take 29 of 192. The old test passed a Vision
  number buffer shrunk to 880 bytes, its options buffer to 28 and the Sense's
  number buffer to 921; each fails now. Test-only, host-tested; no firmware
  byte and no carried Home Assistant file changes.
- [x] **F221 [code] A Vision box with a negative width and a negative height
  reads a proximity.** `classify_posture` reads a non-positive side as
  unknown, but `sample_from_boxes` hands `classify_proximity` the area `w *
  h`, which is positive when both sides are negative, so such a box reads
  near, mid or far (a -100 by -100 box reads mid) while its posture reads
  unknown. The device's SSCMA boxes have unsigned fields, so only the Lab's
  sandbox and the core's ABI can send one, and both builds agree on it; A42's
  oracle tests (`test_vision_detection_pipeline.cpp` and the grid in
  `vision.test.js`'s A42 test) pin today's behavior. Make a non-positive side
  read proximity unknown too, and move both oracles with it (a firmware change
  that moves the emulator dist). Found by A42 (#1762).
  *Done (#1762):* `detection_pipeline.h`'s `area_of` gives a box with a side
  that is not positive no area (0), so `classify_proximity` reads it unknown,
  as `classify_posture` reads its posture; a positive box reads its area as
  before. Both oracles move with it: the host suite's `__int128` oracle reads
  a non-positive side as unknown, and a new host test drives ten such boxes
  and `area_of` itself; `vision.test.js`'s A42 grid reads it unknown too and
  gains two fixed rows (-100 by -100, which read mid, and -2000000000 by -100,
  which read near), and a named F221 test drives seven such boxes and three
  positive ones through the Vision core. Each fails with `area_of` put back to
  the bare product. Only the Lab's sandbox and the core's ABI can send a
  negative side; the device's SSCMA sides are unsigned, so no device reading
  changes. The emulator dist moves (`dist/canary-vision-core.js` only). Until
  CI's pinned-emsdk rebuild, `vision.test.js`'s A42 and F221 tests fail on the
  committed dist (33 of 35, rerun on the integrated tree) and pass with
  `LAB_CORES=native` (35 of 35; with eyes and audio, 51 of 51);
  `native_cores.test.js`'s opt-in out-of-range scenario, run by hand under
  `LAB_CORES=native`, names the stale dist at its first both-negative box.
  After the rebuild the dist step must show `vision.test.js` 35 of 35, and
  both opt-in Vision parity scenarios must pass by hand. A firmware change:
  host-tested, compile-tested by CI, not bench-tested. Found here, by the
  review: F249 (an incremental host build misses a pipeline change).
- [ ] **F249 [code] An incremental host build misses a `detection_pipeline.h`
  change in the Vision core bindings test.** `firmware/tests_host/Makefile`'s
  `VISCORE_HDR` lists the FSM headers, `config.h`, the shim headers and the
  emscripten stub, but `vision_core_bindings.cpp` also includes
  `detection_pipeline.h` and `optical_features.h`, so after a pipeline edit
  `make` rebuilds only `test_vision_detection_pipeline` and reuses a stale
  `test_vision_core_bindings`. CI builds fresh, so it is unaffected. Add
  `$(VISION_PIPE_HDR)` to `VISCORE_HDR`. Pre-existing. Found by F221's review
  (#1762).

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
  *Done (#1762):* the Vision page's MQTT pane no longer hand-writes a payload.
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
  *Done (#1762):* `gen_homeassistant.py` builds the "5 · Meet the fleet" step
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
  *Done (#1762):* a new `canary-local/tools/_pseudonym.py` derives the
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
  *Done (#1762):* decided: every WAP identity example on the page is the repo
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
  advertises; it found 16 problems in the docs as they stood. Page data, docs
  and one `wap.js` line (the unnamed host's note), host-tested; no firmware
  changed. Found here: A35 (two firmware
  comments and a test fixture) and D10 (the docs call the suffix unique).
- [x] **A30 [code] The WAP and Sense Lab pages' sandbox scenes publish
  abbreviated payloads.** `gen_wap.py`'s and `gen_sense.py`'s `SANDBOX`
  entries publish only the fields a scene changes. Events look like
  `{"event_type":"motion","state":"motion","motion":74}`, with no
  v/alg/fp/sig, and chain gets `{"length":+1}`, which is not JSON. `wap-ui.js`
  pushes them into a pane whose note says its payloads are "the exact strings
  csi_mqtt.cpp publishes", and the chain one replaces the retained chain row.
  `fingerprint_examples.test.js`'s envelope rule exempts `.sandbox[…]` for now
  (A27). Lay each scene's fields over the topic's full example, as the Vision
  pane's `vizEventPayload` does (A26), then drop the exemption. Found by A26
  and A27 (#1762).
  *Done (#1762):* every sandbox publish in the WAP and Sense page data is now
  its topic's whole payload. Each scene names the fields it sets (`set`) and
  the counters it moves (`advance`); `gen_wap.py` and `gen_sense.py` lay them
  over the topic's example, key order kept, and refuse a field the payload
  does not have. `wap-ui.js`'s `scenePayload` lays the same fields over the
  topic as it then stands, so a second click takes the next event id and chain
  length. A publish is retained by its topic's own flag (the pane used to
  retain anything but `events`), and the retained snapshot lands each topic as
  it stands when its turn comes, so a card tapped on an offline bench is no
  longer undone by the example. Every WAP scene publishes counts then chain,
  as the loop does when `records_created` moves: a committed row, an acoustic
  detection and a mute each make a witness record. A moved head's hash is
  elided. The WAP's events rows are `core.presence` `presence_changed` rows.
  Each presence scene's event word and console line are its row's state, and
  `gen_wap.py` refuses scores that `derive_target_state`, read from
  `core_presence.cpp` at the default balanced preset, would land in another
  state. `motion`, which the scenes used to publish, is not a state, and the
  sit scene had said `subtle` over a `quiet` row. The dashboard sketch's pill
  is the scene's own, in the getting-started guide's vocabulary, and each
  blurb names both words (A38). The silent-panic scene is gone: the canary-wap
  has no touch pad and no WAP source emits `silent_panic` (it is
  firmware/canary's `securacv_touch`). `mic/state` and the Sense identify echo
  publish the bare word the firmware writes. `gen_sense.py` keys the Sense
  state (with and without the vitals block), events and chain examples from
  `mqtt_mgr.cpp`, `main.cpp` and `witness.cpp`. The Sense radar lab now
  publishes the state row when `main.cpp` would: on a presence, count or
  vitals change, on a build switch, and on every heartbeat (`heartbeat_ms`, so
  a range band that moves alone waits for it), each laid over the build's
  state example as the row last stood. The lights scene sets only its lux,
  over the row as it stands, and walks someone in first if the room is empty.
  The lab's stall drops the count with the link, as `mr60_presence.cpp` does.
  Both panes' notes take the non-retained topics from each topic's flag (the
  WAP's names events alone, though tamper is live-only too) and say what is
  elided and staged: the WAP sandbox publishes a presence row at the click
  rather than when its bundle closes, keeps the example's timestamp, and omits
  the `acoustic.events` row the device bundles beside each alarm and mute; the
  Sense lab leaves out the `occupancy_changed` to 0 occupants the device
  records as a room empties (F131), and keeps the example's clocks. Neither
  sandbox lede claims the exact MQTT. `fingerprint_examples.test.js` drops the
  `.sandbox[…]` exemption, and its WAP and Sense fp rules read the sandbox. A
  shared fake DOM (`tests/fixtures/fake_dom.js`, with listeners, a no-op
  canvas and a test clock) lets `wap.test.js` and `sense.test.js` drive both
  panes; `sense.test.js` plays every lab-driven scene through the real radar
  lab, pane and cards and holds each published row to `sense.json`'s. Each new
  test fails on the code before it, shown by restoring the old file, and eight
  mutations of the Sense state-row rules are each caught. Lab page data and
  scripts only, host-tested; the browser probes are CI's, and the emulator
  dist does not move. Found here: A38 and F131.
- [x] **A31 [code] The Sense Lab page's MQTT pane hand-writes its live rows.**
  `sense-ui.js`'s `labevent` handler writes an events row with no v, alg, sig
  or bucket_uptime_s, and a chain row
  `{"v":1,"length":…,"latest_hash":"…","alg":"ed25519","sig":"…"}` with no fp,
  which HA's `signature.py` reads as unsigned. A26's script policy passes the
  file, because its one fp is the interpolated, rule-held `fp_example`: the
  policy reads the values that are there, not the fields that are missing.
  Build both rows from `sense.json`'s topic payloads, as the Vision pane now
  does. Found by A26 (#1762).
  *Done (#1762), with A30:* `sense-ui.js` builds the lab's live rows from
  `sense.json`. `senseEventPayload` lays the lab's event, presence, occupants
  and range over the events example (every key in `record_event_now`'s order,
  the envelope and `bucket_uptime_s` included) at seq = chain length + 1.
  `senseChainPayload` republishes the chain example at that seq, its hash
  elided and its fp kept, so HA's `signature.py` sees a signed envelope. The
  script spells no fp of its own any more; `fingerprint_examples.test.js`'s
  script walk now asserts that, and takes `guides.js`'s exempted forgery as
  its sign of life. `bucket_uptime_s` stays the example's 0: the Sense page has
  no device clock, and 0 is the bucket for the example's first ten minutes of
  uptime (312 s). Before the bench connects the pane publishes nothing, as on
  the device: a lab event still advances the chain (`record_event_now` chains
  first and returns while MQTT is down), and the retained snapshot on connect
  lands the chain and the state as they stand. `sense.test.js` unit-tests both
  builders, drives the pane through two lab events and two pushed scenes, and
  holds an offline event to the chain only. On the old `sense-ui.js` two of
  its tests fail, and the offline test fails on A31's first cut. Host-tested.
- [x] **A32 [code] The WAP Lab page's events example is a subset of the wire
  body.** `gen_wap.py`'s TOPICS `events` payload carries event_id, event_type,
  state, motion, breathing, signed and the envelope. `csi_event_wire.h` writes
  keys it lacks, among them module, category, privacy, timestamp, zone,
  confidence, duration_sec and replay. `gen_wap.py` holds each topic's suffix
  to `csi_mqtt.cpp` but not its payload keys. Key the WAP examples from the
  wire builder, the way `gen_vision.py` now keys the Vision pane (A26). Found
  by A27 (#1762).
  *Done (#1762):* `gen_wap.py` keys every topic's example from the code that
  publishes it, in order, or refuses to write: the events and tamper bodies
  from `csi_event_wire.h` (the builder both trees publish through); status,
  chain and counts (their signed branches), health, mesh, chirp and beacon
  from `csi_mqtt.cpp`; sensing from the `.ino`'s snprintf; and update/state
  from `ota_publish_update_state`'s JsonDocument. `update/auto` and
  `mic/state` are pinned bare strings. Keys a publisher writes only sometimes
  (health's `sd_mounted` and `enclosure_open`, a release's url and summary)
  may appear only in their place. The events example is now a whole
  `core.presence` `presence_changed` row: its seventeen body keys and the
  envelope, state `active` (the old `motion` is not a state core.presence
  emits), event_type the state, category `event`, privacy `p0`, and an
  event_id in `kIdSpaceBase`'s space (the old 1234 is below every id a WAP
  hands out since F46). The generator refuses an events example whose module,
  type, state, category or privacy no WAP module emits, or whose scores
  `derive_target_state` would land in another state. The health example gains
  `"sd_mounted":true`, which the FULL build the page's boot log narrates sends
  once a card has mounted (F41); `enclosure_open` stays out, since
  FEATURE_TAMPER_GPIO is off in every profile. The Hub page prints health
  verbatim, so `homeassistant.json` was regenerated after `wap.json`;
  `gen_csp.py --check` is unchanged. `wap.test.js` parses the same formats
  itself and holds every example to them. On the old `wap.json` its three new
  tests fail, and the generator refuses the old events example, a `motion`
  state and a reordered health. Page data only, host-tested.
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
  page's demo. Found by A27 (#1762).
  *Since A36 (#1762):* the Hub demo shows the WAP's own discovery entities,
  so the demo is per product. The guide's table is still this item's call.
- [ ] **A34 [decision] The Sense Lab page's fingerprint is not the repo test
  key's.** The WAP page, the Hub page and the Vision pane show the seed-0x42
  key (fp `7916ca487912fa1b`). The Sense page shows `b7e2c49a11f03d5c`, which
  no derivation produces, though canary-sense derives its fp the same way.
  Decide whether every Lab key example is the test key's. If so, move
  `gen_sense.py`'s `EX_FP` (and an elided health key) to it and add the Sense
  page to `fingerprint_examples.test.js`'s test-key check. Found by A26
  (#1762).
- [x] **A35 [code] Two firmware comments and a test fixture show WAP names no
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
  review (#1762).
  *Done (#1762):* `csi_dashboard_html.h`'s device-identity comment and
  `cbor.h`'s writer example now show `canary-s3-4dC2`, and `flash.test.js`'s
  redaction fixture logs `AP started: SecuraCV-4dC2`. The dashboard comment
  sits inside the served script, so `web_assets_gz.h` is regenerated
  (`gen_web_assets_gz.py --check` passes); nothing else embeds the header, and
  `check_csi_sync.sh`'s staged-copy check, `lint_dictionary_sync.py` and the
  microcopy lint pass. A device serves the corrected comment after the next
  firmware build, which CI compiles; it was not built here.
  `scripts/tests/test_wap_name_examples.py` now also reads every line of the
  WAP sketch, `firmware/common/`, `canary-local/`, `custom_components/` and
  `tools/` sources, comments and strings alike. The display tree is not read:
  its SoftAP is `SecuraCV-%.4s` of its own token, another product's recipe. On
  wave 9's tree the test fails on the three lines and on
  `fingerprint_examples.test.js`'s history comment, which now describes the
  old pair without spelling it. One line keeps a WAP-shaped host as a named
  exemption, which fails if it goes dead: `companion_pwa.h`'s comment
  `(canary-s3-XXXX.local)`. It is true to `updateMdnsLinkFromDevice`, which
  builds the wizard's close-out link from the device id, a host no WAP
  advertises, so the code is filed as a bug (F129) instead of the comment
  being corrected.
  *Since F129 (#1762):* the exemption is gone. `companion_pwa.h`'s comment
  names the host the wizard now links, and the source walk runs with no
  exemption.
- [x] **A36 [code] The Hub page's demo shows entities no WAP announces.** The
  demo now names the fleet step's WAP (`canary-s3-4dC2`), but it still shows
  Die Temperature, SD Card Healthy and Tamper Detected from the setup guide's
  firmware/canary entity set. `csi_mqtt.cpp`'s discovery table announces none
  of them, and the printed health line carries no die temperature or SD state.
  Give the demo a per-product entity set, or drop those three from the WAP
  demo (see A33). Found by A27's review (#1762).
  *Done (#1762):* the Hub page's demo card is the fleet step's WAP as its own
  MQTT discovery announces it. `gen_homeassistant.py` draws nine entities from
  `wap.json`'s discovery table by object_id (Witness Records, Chain Length,
  Online, Uptime, Signal Strength, Smoke Alarm Heard, CO Alarm Heard,
  Microphone Mute, Firmware), with the table's name, component and unit, and
  refuses one the WAP does not announce. Die Temperature, SD Card Healthy and
  Tamper Detected are gone, and so is Chain Valid, the integration's own
  entity. Initial values are the states the entities' templates read off the
  fleet step's lines (312, 312 blocks, 312 s, -58 dBm). The card is named as
  `csi_mqtt.cpp`'s device block names it (`Canary canary-s3-4dC2`), the
  discovery ribbon shows the WAP's config topics
  (`homeassistant/<component>/canary_<id>/<object_id>/config`), the drill's
  timeline line names `smoke_alarm_t3`, and the note says the integration's own
  entities belong to the device it registers (`SecuraCV Canary <id>`) and are
  not drawn. `hub-ha-ui.js` also counted "Witness Count" up from a hard-coded
  1284 under a card showing 312; the count and chain now start from the
  printed values and move only on a witness record (a mute, a detection, the
  liveness tick), not when the device appears or an alarm clears. The setup
  guide's table is untouched (A33 stays open). `homeassistant.test.js` holds
  each entity to the WAP's table and to `csi_mqtt.cpp`, and drives
  `buildHaDemo` on the fake DOM through discovery, the liveness tick, the drill
  and two flips of the mic switch: the alert row names the drill's word, the
  trigger turns on, and Witness Records, Chain Length and the chain status
  move by one on the detection and on each mute, and not on the clear. On the
  old `hub-ha-ui.js` the play-through fails, on the old `homeassistant.json`
  three tests fail, and four mutations of the drill and mic paths that passed
  the first cut's suite now fail it. Page data and script, host-tested.
- [x] **A37 [code] The Vision pane's clocks and coarse features do not follow
  the sandbox.** `vizEventPayload` moves presence, voxel, box, occupancy mask
  and the chain length. `ts_ms`, `presence_ms`, `dwell_ms` and `visit_ms` keep
  the example's values, so a `dwell_started` publishes `dwell_ms` 0, and
  posture and proximity stay `upright`/`mid` for any box while someone is
  present. Derive them from the sandbox's clock and box, or have the pane say
  which fields are illustrative. Found by A26's review (#1762).
  *Done (#1762):* the Vision pane takes its clocks and coarse features from
  the sandbox, as `publish_event_json` takes them from the FSM snapshot on the
  tick that emits the event. `presence_ms` comes from the firmware core's FSM,
  and posture, proximity, the person count's occupancy bucket and the
  occupied-cell mask from its detection pipeline; the committed WASM core
  already returned all of them. `ts_ms` is the clock the core saw plus the
  rows' own `ts_ms` (`pane.clock.t0_ms`, 41250), `bucket_uptime_s` its
  10-minute bucket, and `visit_ms` the last stay, latched at `presence_ended`
  from the events the pane sees (the core does not return `last_visit_ms_`).
  The state row sent on the same tick carries the same clocks, `uptime_s`
  included. `dwell_ms` is 0 on every event row, as the device sends it:
  `PresenceFSM` sets `dwell_start_ms_` on the tick that emits `dwell_started`
  and clears `dwelling_` before `dwell_ended`'s snapshot, and no other event
  is dwelling. Only the heartbeat's state row carries a running dwell, and the
  pane does not stage the heartbeat. The pane's note says so, and names the
  two values that stay illustrative: the voxel is the frame's cell (the device
  publishes its tracker's settled cell, which the core does not return, A39),
  and a moved head's hash is elided. `gen_vision.py` pins the snapshot
  formulas, the two dwell branches, the heartbeat publish, the core's JSON
  fields and `optical_features.h`'s occupancy buckets. `vision.test.js` drives
  `buildMqtt` with `VisionSim` on the real core through the linger scene. It
  holds every event's clocks, posture, proximity and mask to the snapshot and
  to `optical_features.h`'s thresholds, every event's `dwell_ms` to 0, and
  `dwell_ended` to being sent while present with the stay so far as
  `presence_ms`. On the old `vision-ui.js` that test and the overlay unit test
  fail, and a pane faking a running dwell fails it now. No core change, so the
  emulator dist does not move. Host-tested; the browser probe is CI's. Found
  here: F130 (the device's `dwell_ended` reports `dwell_ms` 0) and A39.
  *Since F130 and A39 (#1762):* `dwell_ended` carries the dwell it closed
  (counted to the frame that declared the person gone), so `dwell_ms` is no
  longer 0 on every event row, and the pane takes the voxel (the tracker's
  settled cell) and `visit_ms` from the core; its note names only the elided
  chain hash as illustrative.
- [ ] **A38 [decision] The WAP Lab page shows sensing cards and a pill
  vocabulary the canary-wap does not have.** `gen_wap.py`'s dashboard cards
  Touch (silent panic, enclosure tamper), Appliance activity (IR) and Thermal
  drift, and its pills (Quiet / Presence / Motion / Active / Offline), come
  from `docs/getting_started_canary.md` §3 and §6-8. The canary-wap sketch has
  no touch pad, IR receiver or die-temperature drift code, and its dashboard
  names its states Empty, Subtle motion, Quiet, Active and Together
  (`csi_dashboard_html.h`). The two vocabularies collide: the firmware's
  `quiet` is a still, breathing person, the guide's Quiet an empty room. The
  sandbox's presence scenes now say the firmware's word in their console line
  and event, keep the guide's pill as an explicit per-scene field, and name
  both in each blurb (A30). The boot log's runtime table (`WEVT` rows, one in
  state `motion`, which `core.presence` never emits) is not a table the sketch
  prints. Decide whether that guide describes the WAP or firmware/canary, then
  trim the page's cards, pills and table to the WAP's. See A33. Found by A30
  (#1762).
- [x] **A39 [code] The Vision firmware core does not return the voxel tracker's
  settled cell or `last_visit_ms_`.** The Lab's Vision pane shows the frame's
  cell (and says so) and latches `visit_ms` from the events it sees (A37).
  Returning both from `canary-local/emulator/vision/vision_core_bindings.cpp`'s
  tick JSON moves the emulator dist (CI's pinned-emsdk rebuild); then the pane
  can drop its note and its own latch. Found by A37 (#1762).
  *Done (#1762):* `vision_core_bindings.cpp`'s tick JSON returns the FSM
  snapshot's `visit_ms` (`last_visit_ms_`) and `voxel` (`r`, `c`, `rows`,
  `cols`: the tracker's settled cell) in its `fsm` object. The Vision pane
  reads both: `vizEventPayload` takes each event's voxel and `visit_ms` from
  the core, the pane's own visit latch is gone, and its note names one
  illustrative value, the elided chain hash. The settled cell is not the
  frame's: it moves only once the person has been seen away from it three
  times in a row, and it keeps the last cell once the frame is empty, so
  `presence_ended` names where the person was. It is reset only by
  `PresenceFSM::reset()`, at boot, so a later visit's `presence_started`, and
  its frames until the new cell settles, name the previous visit's cell: the
  Vision README, the pane note, the bindings comment and the Lab's Voxel
  entity description (which said "the occupied cell") say so, `gen_vision.py`
  dies if the tracker gains another reset site, and
  `test_vision_core_bindings.cpp` pins it with a second visit. Before anyone
  is seen it is the tracker's reset `Voxel{-1,-1,0,0}`; `gen_vision.py`'s idle
  state example said rows 3 and cols 3 and now says 0, pinned to
  `voxel_tracker.cpp`. HA's Vision dashboard said the voxel sensor reads -1,-1
  when nobody is present, so after a first visit its card showed the last cell
  as live for good; it now reads whether anyone is in frame from the
  confidence sensor on the same state row and says "No person in frame" once
  the frame is empty, held by `scripts/tests/test_vision_dashboard_voxel.py`,
  which fails on the old card. A new host suite,
  `firmware/tests_host/test_vision_core_bindings.cpp`, builds the core
  natively from the five sources `build.sh`'s vision flavor hands em++ (with
  the emulator's Arduino shim and a one-macro
  `stubs/emscripten/emscripten.h`), holds its JSON to the device's values, and
  fails on the old bindings; `firmware.yml` now also runs on a
  `canary-local/emulator/vision/**` or `shim/**` edit. `vision.test.js` holds
  the committed dist's `fsm` keys to the bindings source, so a stale dist
  fails there by name; holds every event and state row the pane publishes to
  the core's voxel, `visit_ms` and `dwell_ms`; and holds the pane note and the
  Voxel entity description to the firmware lines they stand on. The emulator
  dist moves (the vision-core flavor, `dist/canary-vision-core.js`): it is
  rebuilt in this PR by CI's pinned emsdk after this ledger lands, and it was
  not rebuilt or run here. Until then two `vision.test.js` tests fail on the
  committed dist, one naming it; against this tree's sources built natively
  and served to the tests in place of the dist, all 29 passed, and on the base
  sources the same two failed. The pane does not tolerate the old dist: the
  two ship together. Lab page, page data, HA dashboard and emulator ABI:
  host-tested; the browser probe is CI's. Found here: F152, HA25 and A40.
  *Since F152 and HA25 (#1762, wave 12):* each visit starts its own voxel
  tracker, so a later visit's `presence_started` names the cell it began in:
  the carry-over this item documented and pinned is gone
  (`test_vision_core_bindings.cpp` now pins the opposite, and the README, pane
  note, bindings comment and `gen_vision.py` say so). HA's Vision dashboard
  voxel card reads the Presence binary sensor, which can now turn on, instead
  of the confidence sensor; while Presence is unknown (Vision firmware without
  HA25) it falls back to this item's confidence gate and says so, and it no
  longer prints "Nobody seen since the device started." when the voxel sensor
  has no value (this item pinned that render for an unavailable sensor; the
  card now says the Vision has not reported).
- [x] **A40 [code] The page tests that drive a WebAssembly core can only run
  the committed dist.** A change to a core's sources cannot be proven
  page-side until CI's pinned-emsdk rebuild lands, and an agent without emsdk
  sees those tests fail on the old dist. A39 proved `vision.test.js` and
  `eyes.test.js` against the tree's Vision sources by building the core
  natively (the five `build.sh` sources with the emulator shim and a stub
  `emscripten.h`, as `firmware/tests_host/test_vision_core_bindings.cpp` now
  links them) and serving it to the tests in place of
  `dist/canary-vision-core.js` over synchronous pipes; that harness stayed in
  a scratch directory. A committed opt-in (an environment variable the tests
  read) would let anyone run the Vision page tests against the sources. Found
  by A39 (#1762).
  *Done (#1762):* `vision.test.js`, `eyes.test.js` and `audio.test.js` take
  their core from `canary-local/tests/native/cores.js`. Unset (or
  `LAB_CORES=dist`) it returns the committed `emulator/dist/<core>.js`, the
  module they required before, so the Lab's and CI's default runs are
  unchanged; any other value is refused. `LAB_CORES=native` reads `build.sh`'s
  flavor block for the core (the source array its `for src in` loop walks, the
  flags and include paths its `em++ -c` line passes, and the link line's
  runtime methods), compiles those sources with g++ (or `$CXX`) beside a
  one-line `<emscripten.h>` and `core_server.cpp`, and hands the test a
  factory with the dist's shape: `cwrap` (which, like the dist's, returns a
  non-string export's raw number), plus `ccall` and the `HEAP` views where the
  dist exports them. The reader refuses by name what it cannot follow rather
  than skip it: any write to an array or variable the build reads other than
  its one literal (an append, a reassignment, an element), a compile line
  other than exactly the one it knows, `EMCC_CFLAGS`, an unquoted glob and a
  backquoted command. The server's export table is generated from the sources'
  `EMSCRIPTEN_KEEPALIVE` signatures, and a type a wasm call cannot carry is
  refused by name. Calls stay synchronous: a worker thread owns the pipe and
  the test's thread waits on a SharedArrayBuffer; a core that dies fails its
  call, and one that stops answering is killed after 30 s. These three are the
  only Node tests that require a dist core; the display flavors are booted
  only in Chromium (A43), and the Vision, eyes and audio browser probes still
  load the dist (A41). `native_cores.test.js` holds the loader in the default
  run with no compiler: what unset returns, the `build.sh` reading and each of
  its refusals on edited copies of `build.sh`, every export and runtime method
  the dist has, `cwrap`/`ccall` return conversion as read off the committed
  audio bundle, and the pipe against a stand-in core that dies, answers twice
  or hangs. Under the opt-in it also drives each native core next to its dist,
  call for call, on fixed scenarios (3000 Vision ticks; the audio cadences
  plus noise). Agreement covers only those calls: the native build is 64-bit
  g++ where the dist is wasm32 and emscripten's clang (A42). A bindings
  mutation and an audio tone-gate mutation each fail the opt-in run while the
  default run stays green. `canary-local.yml`'s logic job runs
  `native_cores.test.js` with the other suites, then runs the three tests
  again with `LAB_CORES=native` on the runner's g++ (no emsdk, about 10 s),
  whenever the dist step ran, red or green, so a stale dist shows as red above
  and green below. This PR shows it: run on the integrated tree before CI's
  dist rebuild, the default run fails F152's test on the old dist (46 of 47),
  the opt-in run passes all 47 of the three tests, and the opt-in parity test
  names the stale dist at its 22nd Vision tick. `vision.test.js`'s stale-dist
  message names the opt-in. Documented in
  `canary-local/tests/native/README.md` and the Lab README's testing list.
  Test tooling, host-tested; the emulator dist does not move for it; the CI
  step's first run is CI's. Found here: A41-A43.
- [x] **A41 [code] The Lab's browser probes run only the committed Vision and
  audio cores.** `vision_probe.mjs`, `eyes_probe.mjs` and `audio_probe.mjs`
  load `emulator/dist/canary-vision-core.js` and `canary-wap-audio.js` through
  each page's `<script>` tag, so `LAB_CORES=native` (A40) does not reach them,
  and a core change is proven in Chromium only after CI's pinned-emsdk
  rebuild. The probe server could serve a stand-in factory that forwards each
  call to the natively built core (`canary-local/tests/native/`) over
  synchronous requests. Found by A40 (#1762).
  *Done (#1762):* `vision_probe.mjs`, `eyes_probe.mjs` and `audio_probe.mjs`
  take `LAB_CORES`. Unset (or `dist`) nothing changes:
  `tests/native/probe_cores.js`'s `probeCores` returns null and the probe
  serves the committed bytes. Under `LAB_CORES=native` it builds the probe's
  core with `cores.js` (A40) before Chromium starts, and the probe's server
  answers the core's dist URL with `tests/native/core_standin.js`, under every
  spelling of that URL the server would read as the file, and refuses any
  other method on it, so a native run never serves the committed core. The
  stand-in is a factory of the dist's export name and module shape (`cwrap`,
  `ccall`, `UTF8ToString` and the `HEAP` views where the dist has them); its
  every call is a synchronous same-origin `XMLHttpRequest` to the probe
  server, which runs it on the native core through `cores.js`'s own module, so
  the return conversions and the refusals (`cwrap`'s at `cwrap` time) are one
  implementation for the Node tests and the probes. A pointer export's window
  (the audio frame) is mirrored in a heap the stand-in keeps in the page,
  carried to the core before each call and back after it; the heap grows with
  the old buffer detached, as wasm memory growth detaches it. The pages are
  served as committed: the stand-in is a `'self'` script and its requests are
  `'self'` connects, with no eval and no inline code. Two allowances: each
  core call is a request, so `vision.html` (which calls its core every
  animation frame) never reaches Playwright's `networkidle`, and the native
  probes wait instead for 500 ms of quiet not counting the core's own
  requests; and a native run fails unless every core it asked for had its
  stand-in both served and called, and prints the call count when it passes.
  Run locally three times each way, each probe passed on the dist and
  natively. A scratch rename of `presence_started` in `presence_fsm.cpp`
  failed the native Vision and eyes probes while their dist runs passed, and
  removing the audio tone gate in `securacv_audio.cpp` failed the native audio
  probe while its dist run passed. `native_cores.test.js` holds the bridge in
  the default run with no compiler and no Chromium: the stand-in as served, in
  a `vm` context over `fake_core.js`; the HTTP handling, raw URL spellings
  included; the no-silent-fallback guard (served but never called, called but
  never served, one core of two); the quiet wait, over a fake page; the
  stand-in's needs against the pages' policy; that every probe opening a core
  page takes the bridge before its file lookup; and the CI wiring. Twenty-one
  mutations of the stand-in, the bridge and the probe wiring each fail one of
  those tests. `canary-local.yml`'s wasm job runs each of the three probes
  again under `LAB_CORES=native` right after its dist step, whenever that step
  ran, red or green, with the runner's g++ and no emsdk. An earlier red step
  in that job skips both modes: in #1762 the native probes first run after the
  dist rebuild, since until then the onboard probe goes red at
  `dash@portrait` before them (F184). A native pass in
  Chromium says what one in Node says: this tree's sources pass, built by
  64-bit g++ rather than wasm32 clang (A42 still applies). Documented in
  `tests/native/README.md` and the Lab README's testing list. Test tooling,
  host-tested and run in local Chromium; the emulator dist does not move; the
  CI steps' first run is CI's.
- [x] **A42 [code] The native Lab cores are not the wasm build, and disagree
  with it on out-of-range input.** `canary-local/tests/native/cores.js` (A40)
  compiles the Vision and audio cores with the host compiler for its 64-bit
  ABI; the dist is wasm32 built by emscripten's clang. On this tree a Vision
  box two billion pixels wide reads proximity `unknown` on the dist and `near`
  natively, because `(long)w*h` overflows only in wasm32. Its voxel row
  differs too: a signed overflow in the cell mapping is undefined, and g++
  resolves it differently from clang (`CXX=clang++` matches the dist's row).
  Building with `-m32` where a 32-bit multilib is installed would close the
  `long`-width part only; undefined behavior can still split between
  compilers, and fixing the overflows in the sources (clamping the box before
  multiplying) is the real cure. `native_cores.test.js` under
  `LAB_CORES=native` catches a difference only on the paths its scenarios
  drive. Found by A40's review (#1762).
  *Done (#1762):* fixed in the sources, so the builds compute the same thing.
  `detection_pipeline.h`'s `point_to_cell` takes a box's center as `int64_t`
  at both call sites and multiplies and clamps there; the area is `area_of(w,
  h)`, `(int64_t)w * h`; in `optical_features.h`, `classify_posture` compares
  in `int64_t`, and `classify_proximity` takes `int64_t` areas, counts an area
  past the frame's as 100% and scales a frame too large to multiply by 100. A
  box whose int math did not overflow reads as before (the 3000-tick
  native-vs-dist parity scenario passes with this change alone); the item's
  box, two billion pixels wide from the frame's corner, now reads near and
  horizontal in the last column on every build. Only the Lab's sandbox and the
  core's ABI can send a box that reaches the center, cell and posture
  overflows; the device's SSCMA boxes have uint16 fields (Seeed_Arduino_SSCMA
  v1.0.3), which could reach only the area overflows in the ESP32's 32-bit
  `long`, and a 240x240 model does not return a box that large.
  `test_vision_detection_pipeline.cpp` holds 104,976 boxes built from the int
  range's ends to an `__int128` oracle, and the Makefile builds that suite a
  second time under `-fsanitize=undefined` (CI's firmware host job now needs
  gcc's UBSan runtime; its first run there is CI's);
  `test_optical_features.cpp` holds the classifiers at `int64_t` extremes;
  both fail before the fix. The posture or area multiply put back in `long`
  passes every 64-bit host test, as a 64-bit `long` must, so `vision.test.js`
  gains a firmware-wasm test on the dist CI runs: eight boxes, one per
  overflow site, and a 6,561-box grid, each held to a BigInt oracle that reads
  its thresholds from `optical_features.h`. Once CI's rebuilt dist carries
  this change, that test is the CI gate for the 32-bit `long` half; nothing
  shows that half from the sources on a PR before a rebuild (A49). A scratch
  clang wasm32 build disagreed with g++ on 68,130 of the 104,976 boxes before
  the fix and on none after. `native_cores.test.js` gains an opt-in
  out-of-range scenario, but no CI job runs that file under `LAB_CORES=native`
  (A50). The emulator dist moves (`dist/canary-vision-core.js` only): until CI
  rebuilds it, `vision.test.js`'s A42 test fails on the committed dist at the
  item's box and passes with `LAB_CORES=native`; after the rebuild it must
  pass on the dist in CI, and both opt-in Vision parity scenarios must pass
  when run by hand. Host-tested; the firmware compile is CI's; not
  bench-tested. Found here: F221, A49 and A50.
- [ ] **A43 [decision] The display flavors have no stand-in built from the
  tree's sources.** A display firmware change that a Chromium probe checks
  (`boot_probe.mjs`, `onboard_probe.mjs`, `bench_probe.mjs`, `csp_probe.mjs`)
  is proven only after CI rebuilds `dist/canary-display-*.js`, which links the
  firmware, LVGL and the browser shims into one page under Asyncify; A40's
  pipe cannot stand in for a page. Decide whether a host build of the display
  firmware's page-checked logic is worth having, or whether CI's rebuild stays
  the only path. Found by A40 (#1762).
  *Merged here (#1762, wave 13), from F184's review:* a native stand-in is
  cheap. F184's review built `build.sh`'s whole dash translation-unit list
  natively (the real `main.cpp`, the emulator's `src/` and `shim/`, LVGL 8.4,
  Ed25519) with g++ in seconds; a driver standing in for the page (the EM_JS
  calls as C functions, a virtual clock) walked the first-boot portal in about
  2 s per run and evaluated the probe's own holds on dumped framebuffers and
  bindings, catching each of nine mutations. Committed and run in the page
  logic tests job beside `LAB_CORES=native`, it would prove the display
  sources on every PR, stale dist or not. Until then F184's turned walk
  (`dash@portrait`) fails on the committed dist until CI's rebuild, and its
  reads ran only in that scratch harness.
- [x] **A44 [code] `boot_probe.mjs`'s ready wait can be refused by the harness
  page's CSP.** It waits with a string predicate
  (`page.waitForFunction("window.__ready === true", ...)`), which Playwright
  evaluates through `eval` in the page, and harness.html's policy (`script-src
  'self' 'wasm-unsafe-eval'`) refuses that. Locally, against the committed
  dist, two of three runs failed one flavor each with "EvalError: Refused to
  evaluate a string as JavaScript because 'unsafe-eval' is not an allowed
  source of script" (the dash, then the AMOLED; every other flavor booted),
  and the third passed all five: an intermittent harness failure, not the
  firmware's. A function predicate (`() => window.__ready === true`), which
  Playwright passes without `eval`, would most likely remove it; check
  `onboard_probe.mjs` and the other probes for the same string form. Found
  while fixing F156-F160 (#1762).
  *Done (#1762):* the premise held, and the mechanism is Playwright's:
  `waitForFunctionExpression` (server/frames.js, read at 1.56.1) re-evaluates
  a string predicate through `globalThis.eval` on every animation frame in the
  page, while a function predicate is evaluated once, inside the DevTools
  call, which the page's policy does not govern, and is then only called. So
  the string form failed whenever `window.__ready` was still false at the
  first poll, that is, whenever the wasm boot outlasted the page's load event.
  `boot_probe.mjs` and `csp_probe.mjs` now wait with `() => window.__ready ===
  true` (in `csp_probe.mjs` the harness targets had waited on the same string
  through `t.ready`, where a refused eval would also have counted as the
  page's own violation). Every other `waitForFunction`, and every
  `page.evaluate`, in `canary-local/tests` already passed a function. With
  every wasm compile delayed 2.5 s by an init script, the string form failed
  all five flavors in both probes with that EvalError and the function form
  passed all five; on the committed dist the fixed `boot_probe.mjs` passed
  three runs of five flavors and `csp_probe.mjs` three runs of 31 pages (the
  unfixed `boot_probe.mjs` also passed three local runs: the race needs a boot
  slower than the load event). `csp.test.js` now refuses a `waitForFunction`
  predicate it cannot follow to a function within the probe's own file: a
  string at the call, through any parentheses; a name the file gives a string,
  directly or through another name; a wrapper's parameter whose calls in the
  file pass one (as in `operator_probe.mjs`'s `wait`); and, since it cannot
  see what they hold, a name the file never writes (an import, a loop
  variable), a parameter of an unnamed callback, a wrapper the file never
  calls and a call it cannot cut into arguments. A second test holds the scan
  to 24 refused and 11 accepted fixtures, and eleven mutations of the scan
  each fail it. The rule fails on the old two probes and on the review's
  wrapper and parenthesized-string forms, which the first version let through.
  What it cannot follow is a value a written name gets some other way, such as
  a ternary's arm or a call's return. Test tooling; the emulator dist does not
  move. Found here: A45 and A46.
- [x] **A45 [code] Twelve probe waits pass their options as the predicate's
  argument.** `page.waitForFunction(fn, arg, options)` takes its options
  third, and `boardroom_probe.mjs` (lines 84, 91, 137, 153), `eyes_probe.mjs`
  (129), `kiri_slice_probe.mjs` (80, 111) and `workshop_probe.mjs` (83, 122,
  131, 162, 214) pass `{ timeout: N }` second, so Playwright hands the object
  to the predicate and waits its 30 s default instead: kiri's slice wait is
  cut from the stated 40 s to 30 s, and the others wait up to six times longer
  than they say before failing. Moving each to `null, { timeout: N }` shortens
  most of those waits to the stated value, which may expose real slowness on
  CI's runners, so each value should be checked against a CI run as it moves;
  `csp.test.js`'s `waitForFunction` scan (A44) could then hold the argument
  order too. Found while fixing A44 (#1762).
  *Done (#1762):* the premise held: with the options second, Playwright handed
  `{ timeout: N }` to the predicate and waited its own 30 s (a scratch page
  timed out at 30007 ms with `{ timeout: 1000 }` second, at 1006 ms with it
  third). All twelve waits now pass `null, { timeout: N }`, in
  `boardroom_probe.mjs`, `eyes_probe.mjs`, `kiri_slice_probe.mjs` and
  `workshop_probe.mjs`. The stated values stay: timed here over five runs of
  each probe, the longest of the twelve took 327 ms against 15 s (677 ms with
  every process pinned to one core), and CI's last green wasm job took these
  four probes 11 s, 1 s, 3 s and 5 s, so none is tight. Kiri's slice wait was
  timed only on the engine-absent path; the vendored-engine path has not run
  here. `csp.test.js`'s `waitForFunction` scan (A44) now also holds where the
  options go: every wait states them third, as an object of the keys
  Playwright reads there, naming its timeout; a one- or two-argument call is
  refused, and so is a misspelled key, a number, null, an object with no
  timeout, a computed key or spread after the timeout, and what the scan
  cannot see. On the tree before the fix the rule names exactly the twelve
  waits; a second test holds it to 27 refused and 15 accepted fixtures, and 22
  mutations of the scan each fail one of the two tests. The JavaScript reader
  the scans share moved to `canary-local/tests/js_scan.js`. Test tooling, run
  locally in Chromium; the emulator dist does not move; the CI run is CI's.
- [x] **A46 [code] Twelve browser probes still turn a request URL into a
  filesystem path.** `audio_probe.mjs`, `csp_probe.mjs`, `eyes_probe.mjs`,
  `flash_probe.mjs`, `kiri_slice_probe.mjs`, `operator_probe.mjs`,
  `radar_dev_probe.mjs`, `sense_probe.mjs`, `senselab_probe.mjs`,
  `vault_probe.mjs`, `vision_probe.mjs` and `wap_probe.mjs` serve the
  repository with resolve-then-check (`resolve(join(ROOT, rel))` and a
  `startsWith(ROOT + sep)` test), the shape CodeQL's path-injection query
  flagged twice on #1737; `probe_server.mjs` was written to replace it (the
  tree indexed once, a request a Map lookup) and only `render_probe.mjs` and
  `scene_lifecycle_probe.mjs` use it. Moving the twelve onto
  `indexTree`/`lookup` keeps the probes loopback-only and taint-free. Found
  while wiring A41 (#1762).
  *Done (#1762):* the premise held, and the probes were not loopback-only
  either: each called `listen(0)`, which bound 0.0.0.0, so while one ran the
  whole working tree, `.git` included, was served to the network. The twelve
  now index the tree once with `probe_server.mjs`'s `indexTree` and answer a
  request with `lookup()`, as `render_probe.mjs` and
  `scene_lifecycle_probe.mjs` do (fourteen probes in all), keeping their own
  routes in front (the A41 bridge first in vision, eyes and audio). They and
  the allowlist probes `bench`, `boardroom` and `workshop` (which keep their
  own `Map`, built before their server starts) bind 127.0.0.1 and open their
  pages there. `boot_probe.mjs` and `onboard_probe.mjs`, allowlists too, still
  listen on every interface: another package owned them this wave (A51). With
  a preload logging every request, all fifteen probes answered the same
  requests with the same status and byte count before and after, vision, eyes
  and audio both ways. A path outside the index is now a 404, and the old
  `dir/` to `index.html` fallback is gone (no probe asked for a directory).
  The new `canary-local/tests/probe_server.test.js`, wired into the page logic
  tests, scans every file in `canary-local/tests` that creates a server, says
  `listen` or answers a page's requests, and holds each to five rules: `fs`
  and `path` named only in ways the scan sees called; every handler found;
  every filesystem read in a handler gives only the index's answer, from a map
  declared once at top level before its server and changed only before it;
  nothing derived from the request reaches a path function or a filesystem
  read or write; and every `listen` a readable `.listen(...)` on 127.0.0.1 or
  ::1. The boot and onboard probes are excused from that last rule only, by
  name (`LOOPBACK_PENDING`). A fixture test holds it to 74 refused and 14
  accepted servers (50 of the 74 passed the first version), every one of the
  reviewers' 27 bypass shapes is refused, and 71 mutations of the scan, of
  `lookup()` and of `js_scan.js`'s `codeOnly()` each fail one of the tests.
  What it cannot follow is listed in its header: another module's function a
  handler hands the request (the A41 bridge's `handle()`, which
  `native_cores.test.js` tests), a class's method, a global's method, and a
  value from outside any handler it knows. Integration showed it working: wave
  14's turned-glass probes handed `readFile` itself to a helper, which the
  scan refuses since it cannot see where that reader is called; they now pass
  `(file, enc) => readFile(file, enc)` (dd38ce6e), and `probe_server.test.js`
  passes 4 of 4 on the integrated tree. Test tooling, run locally in Chromium;
  the emulator dist does not move; the CI run is CI's. Found here: A51-A53.
- [x] **A47 [code] The Lab's 3D dash shows a turned glass's canvas on its
  landscape screen.** Since F184 the emulator's canvas turns with the glass
  (480x800 for a portrait dash), and the Lab textures its 3D screen from that
  canvas (`ctx.scene.src = glass` in `canary-local/assets/app.js`), so a
  visitor who turns the dash from its on-glass Settings gets the portrait
  canvas on the model's landscape screen, the model unturned (before F184 that
  choice laid a portrait face out on an 800x480 canvas). Not run in the Lab
  here. Turn the model with the glass, or keep the Lab's dash landscape. Found
  by F184 (#1762).
  *Done (#1762): the dash's case turns with its glass and leaves its stand.*
  `canary-local/assets/scene3d.js`'s `glassTurn()` turns only for a canvas
  that is the panel's own shape turned on its side exactly (`app.js` hands the
  scene the device card's pin-map glass): a 480x800 canvas on the dash's
  800x480 panel turns a quarter turn, and the HTML default 300x150, or any
  other shape, turns nothing. Each builder that can turn names its body's pose
  (`scene.turnPose`) and marks the parts that hold it (`stand`); turned, the
  body is seated face-on and turned a quarter turn clockwise with the holders
  left out, `turnedShadow()` grounds it, and the screen samples the canvas
  turned the other way, so the glass reads upright at its own proportions. The
  stand is left out rather than turned with the case: it is cut for the
  landscape case, and the wall cradle's dock features form a landscape
  rectangle, so no printed part holds the case portrait (C16); keeping the
  dash landscape was the other choice. `real_shapes.test.js` and
  `scene_figures.test.js` hold it on the real Dash (the stand left out, the
  case turned centered and upright, 78 x 118.1, its glass face-on and its
  shadow under its foot; the 300x150 and 0x0 canvases on every twin's glass);
  eight mutants and the reviewed code each fail them. `render_probe.mjs` draws
  the sheet's real dash in Chromium: the stand (known by its mesh) draws no
  pixel turned and does unturned, three marks on a 480x800 canvas read upright
  (face-on 0.65 against the canvas's 0.67), and the case stands centered on
  its shadow. Only the turn is held, not its direction (A56). Drawn in
  headless Chromium; not looked at in the Lab page by hand. Found here: A55,
  A56 and C16.
- [x] **A48 [code] The iOS timeline says no device sets the CSI clock
  offset.** `ios/Sources/SecuraCV/Wire/WapEvents.swift` anchors row dates on
  the claim that absolute buckets are boot-relative on every device because
  nothing calls the clock-offset setter in production. Both trees call
  `csi_event_set_clock_offset_minutes()` on every loop pass with a GPS-synced
  clock (F28), so on such a device `time_bucket` is household time. Correct
  the comment, and decide whether a synced device's bucket can date rows. From
  reading during F121 (#1762).
  *Done (#1762):* `ios/Sources/SecuraCV/Wire/WapEvents.swift`'s header and
  `anchoredDates` doc, and `WapEventsTests.swift`'s matching comment, now say
  what the firmware does: both trees set the CSI clock offset on every loop
  pass once the system clock is set (`update_csi_clock_offset` in
  `canary_wap.ino`, `updateCsiClockOffset` in `firmware/canary/src/main.cpp`;
  GPS is the only wall-clock source in either tree), from the household zone's
  minute of day (F28). So a bucket is household time on a device with a GPS
  fix and boot-relative on one without, and a row does not say which. The
  deltas `anchoredDates` uses are a true age only while the offset holds
  still, and it moves at the first clock set (any size), a DST change (an
  hour), a household zone set or changed (the difference between the two
  zones' UTC offsets) and a drift correction (a minute). Between those, each
  pass floors local time and uptime to the whole minute on boundaries that do
  not line up, so a row near a 10-minute boundary can land one bucket either
  side. Comment-only: no code changed and nothing compiled (no Swift toolchain
  here). Whether a synced device's bucket may date rows is A57.
- [x] **A49 [code] Commit a wasm32 build of the Vision pipeline that needs no
  emsdk.** The host's clang 18 with wasm-ld builds `detection_pipeline.h` for
  wasm32, freestanding (two stub headers, no libc), and node runs the module.
  A42's scratch harness compared it with g++ and clang++ on 104,976 extreme
  boxes: 68,130 differed before the fix and none after, and it caught the two
  A42 mutants no 64-bit host test can see (the posture or area multiply put
  back in `long`). Today only `vision.test.js`'s A42 test catches those, and
  only on a dist CI has rebuilt from the mutated sources. Committed beside
  `LAB_CORES=native`, and run where clang and wasm-ld are installed (skipping
  by name where they are not), this build would show the 32-bit `long`
  behavior from the sources on the PR, before any dist rebuild. It is not
  emscripten's clang, so it settles the `long`-width question, not every
  difference between the compilers. Found by A42 (#1762).
  *Done (#1762):* committed as `canary-local/tests/native/wasm32.js`,
  `wasm32/pipeline_probe.cpp` (`detection_pipeline.h` behind a small C ABI)
  and `vision_wasm32.test.js`. The probe is built twice from the flags
  `build.sh` hands em++ for the Vision core (read through `cores.js`'s
  `buildPlan`): with the host's clang for `wasm32-unknown-unknown`, linked by
  `wasm-ld`, freestanding (no libc; `wasm32/stubs` holds a `cstddef` and a
  `string.h`, and `wasm-ld` refuses any undefined symbol), and with g++ for
  the host. The test checks that the wasm32 module imports nothing and has a
  32-bit `long`, holds every field of the two builds' answers to each other on
  the 104,976 boxes `test_vision_detection_pipeline.cpp` holds the host build
  to exact arithmetic on (its `V[]` read from that suite; `canary-local.yml`
  now watches the file), and puts each of A42's two `long` multiplies back in
  a scratch copy of the header, requiring the wasm32 build to catch it and the
  host build to miss it. Proved on the real tree: the posture products in
  `long` fail the grid test on 17,820 boxes and the area multiply on 16,200,
  while every Vision host suite (UBSan included) and `vision.test.js` under
  `LAB_CORES=native` stay green; on the pre-A42 headers the builds disagree on
  68,130 boxes. The area mutant's spelling is tied to F221's form of `area_of`
  (from the review). The three tests take under a second and passed on the
  integrated tree here (clang 18, `wasm-ld-18`). Without the tools they skip
  and name them; `VISION_WASM32=require` makes that a failure, and the page
  logic job runs the file in its own step with it set, whenever the dist step
  ran (the runner image installs `lld-<n>` beside each `clang-<n>`; that CI
  run is CI's). It is the host's clang, not emscripten's, and not the dist: it
  shows what the sources compute with a 32-bit `long` on the host suite's
  extreme-box grid, not every way two compilers can differ. The emulator dist
  does not move for it. `native_cores.test.js` under `LAB_CORES=native` stays
  opt-in (A50). Found here: A58 (whole cores as wasm32).
- [ ] **A50 [code+decision] Run `native_cores.test.js` under
  `LAB_CORES=native` in CI.** `canary-local.yml` runs that file only in its
  default mode, where its Vision and audio parity scenarios skip; its
  `LAB_CORES=native` step runs only `vision.test.js`, `eyes.test.js` and
  `audio.test.js`. So the scenarios that compare the native build with the
  committed dist call for call (the tick-for-tick ones and A42's out-of-range
  one) run only by hand. A42's review found that this left the 32-bit `long`
  half unguarded in CI until `vision.test.js` gained its own extreme-box test.
  Adding the file to that step (about 8 s) would show a stale or divergent
  dist on every PR, at the first differing call; a core change would then go
  red in a second place until CI's dist rebuild, as `vision.test.js` already
  does. Decide whether that second red is worth it, and if so add the file to
  the step. Found by A42's review (#1762).
- [x] **A51 [code] `boot_probe.mjs` and `onboard_probe.mjs` still listen on
  every interface.** Both serve an allowlist built before their server starts,
  so no request becomes a path, but both call `listen(0)`, which binds
  0.0.0.0, so while they run their allowlisted directories are served to the
  network; every other probe server binds 127.0.0.1 since A46.
  `canary-local/tests/probe_server.test.js` excuses the two from its loopback
  rule by name (`LOOPBACK_PENDING`) only because another package owned them in
  wave 14. Bind each to 127.0.0.1, open its pages there, and empty
  `LOOPBACK_PENDING`. Found by A46 (#1762).
  *Done (#1762):* the premise held: both called `listen(0)`, which bound
  0.0.0.0. `boot_probe.mjs` and `onboard_probe.mjs` now listen on 127.0.0.1
  and open their pages there, and `probe_server.test.js`'s `LOOPBACK_PENDING`
  and its excuse are gone, so every probe server in `canary-local/tests` is
  held to loopback by one rule. A new test reopens each real file's listen and
  must see the scan refuse it; run on the old files it names both `listen(0)`.
  Nothing holds the page host yet (no probe names `localhost` today; from the
  review). Both probes passed in local Chromium before and after, apart from
  an intermittent nightlight read (A63). Test tooling only. Found here: A63.
- [x] **A52 [code] The allowlist probes' servers end the probe on an
  undecodable request.** `bench_probe.mjs`, `boardroom_probe.mjs`,
  `workshop_probe.mjs`, `boot_probe.mjs` and `onboard_probe.mjs` call
  `decodeURIComponent` on the request path outside any `try` in an async
  handler, so a malformed escape (`/%E0`) throws `URIError` and Node ends the
  process with exit 1 instead of answering 404 (checked on a scratch copy of
  the handler); `probe_server.mjs`'s `lookup()` returns null for the same
  request. Only the probe's own browser can reach the first three now
  (loopback), so it takes a page bug to trigger, and the probe then dies
  without saying which request. Found by A46 (#1762).
  *Done (#1762):* the premise held: with a preload requesting `/%E0`, all five
  old probes exited 1 on `URIError`. The five now answer through
  `probe_server.mjs`'s `lookup()`, whose one decode sits in its own `try`, so
  an undecodable path is a 404, and with the same preload each answered `/%E0`
  with 404 on 127.0.0.1 and passed. A raw request carrying a fragment now gets
  404 where four of them gave 200; a browser never sends one.
  `probe_server.test.js` gains rule 5: a `decodeURI` or `decodeURIComponent`
  call, or a `new URL(...)` built from the request, must sit inside a `try`
  whose `catch` holds no literal `throw`, with no function between them (17
  refused and 6 accepted fixtures; each real allowlist probe turned back to
  the bare decode is refused; `lookup()` is fed six malformed escapes; 15
  scratch mutants caught). It does not follow a rethrow through a helper,
  `Promise.reject`, or an aliased `URL` constructor (from the review). Test
  tooling only. Found here: A62.
- [ ] **A53 [decision] Whether the Lab's source scans should parse
  JavaScript.** Three static rules over the probes (A44's string predicates
  and A45's options in `csp.test.js`, A46's request paths in
  `probe_server.test.js`) read source through `canary-local/tests/js_scan.js`,
  a bracket-and-comma reader, and each refuses by name what it cannot follow
  or lists it in its header. A46's scan grew to about 530 lines of
  reader-level data flow (and 220 lines of fixtures) after review. A vendored
  parser (acorn is one file of about 100 KB, no build step) would let them
  follow values exactly and shrink that; the cost is a vendored dependency in
  a no-build tree and rewriting three scans with their fixtures and mutation
  sets. Found by A46 (#1762).
- [x] **A54 [code] The Lab shows no nightlight twin.** CI's rebuild has
  committed `dist/canary-display-nightlight.js` (F204), and only the probes
  boot it:
  the registry's `canary-nightlight` card has no `emulator` or bench block, so
  `fleet.html` offers no "Try it", the bench knows no nightlight and
  `gen_flash.py` links no twin. Add the registry's emulator block (module,
  factory `createCanaryEmuNightlight`) and a bench block from the
  C3-LCD-1.47's facts (its power path and the panel's backlight cap, which the
  emulator does not model), then regenerate in CLAUDE.md's order
  (`gen_flash.py` after the dist). Found by F204 (#1762).
  *Done (#1762):* the premise held. The registry's `canary-nightlight` card
  names its emulator (`emulator/dist/canary-display-nightlight.js`,
  `createCanaryEmuNightlight`), so `fleet.html#canary-nightlight` offers Try
  it and Bench, booting the flavor CI built. Its bench block is the
  C3-LCD-1.47's, each fact citing a file that says it (`bench.test.js` fails
  when a file stops saying its quote): USB-C the only power and the serial
  port, no battery and no charger IC; BOOT on GPIO9 and RST; the panel's 50%
  backlight cap (`CD_BL_MAX_PCT`), stated as the board's cap that the twin
  does not apply. A bench block that names no `power.battery` is now a board
  with no battery path (this one and the Nightstand stick): `BenchPower`
  refuses to fit a cell, the page draws the battery chip inert ("none on this
  board"), and the troubleshooter leaves out every step that stages a battery,
  so pulling the cable drops the rail. In review, one click on the old chip
  fitted a battery the board has not got. The bench lists no lights (C17).
  `gen_flash.py` now links a product to its twin through the manifest's
  `lab.card`; regenerated, the Nightlight's flash entry offers the twin.
  `bench_probe.mjs` opens the Nightlight's card: Try it shows the ring, the
  boot log says the 50% cap, the battery chip is inert, the troubleshooter
  offers no ride-through, and pulling USB drops the rail until the firmware
  reboots. Passed in local Chromium; its battery checks fail with the
  pre-review files. Not bench-tested. Found here: A59 (the S3 ROM banner),
  C17, A60 and A61.
- [ ] **A55 [code] The Lab's 3D Touch 1.69 has a landscape lit face and a
  portrait glass.** The Lab's touch169 figure draws its screen plane 36.1 x
  28.1 (wider than tall) while the glass it textures from it is 240x280
  (taller than wide, the pin map's panel), so the Lab stretches the touch169's
  canvas across a plane of the other shape; A47's turn turns only a canvas
  that is its panel exactly swapped, so it never turns the touch169, rightly
  (measured from the screen part the Lab's builder makes; not looked at in the
  Lab by hand). Either the figure's plane follows the panel (the figure
  generator, then CLAUDE.md's regen order) or the scene fits the canvas to the
  plane it is given. Found by A47 (#1762).
- [x] **A56 [code] The Lab's turned 3D dash turns the same way for both
  portrait turns.** A47's `glassTurn()` reads only the canvas's shape, so a
  dash turned portrait the other way (rotation 3) gets its case turned
  clockwise as for 1: the glass reads upright either way (the canvas is the
  glass as its viewer reads it), but the case stands with its cable side where
  the other turn puts it. Handing the scene the turn the emulator's HAL reads
  off LVGL's driver would turn the case the way the visitor did. Found by A47
  (#1762).
  *Done (#1762):* the premise held, and an upside-down glass (rotation 2)
  turned nothing at all. The emulator's display HAL now sends, with every
  glass shape, the quarter turns the glass is worn at (`js_display_ready(w, h,
  round, turn)`): LVGL's software rotation on the dash, the panel's hardware
  turn on the nightlight. `emu-shell.js` carries it, `app.js` hands it to the
  scene, and the harness logs it. `scene3d.js` turns the case that many
  quarter turns clockwise and samples the canvas turned as many back; a turn
  the canvas's shape contradicts turns nothing, and a dist that announces no
  turn keeps A47's shape-only read. In `render_probe.mjs` the real Dash turned
  1, 3 and 2 stands out of its stand with its cable side left, right and on
  top, and the glass upright. `scene_figures.test.js` and
  `real_shapes.test.js` hold the same and fail on the old scene, and
  `scene_figures.test.js` pins the nightlight HAL storing the whole turn. Once
  the dist announces turns, `boot_probe.mjs` checks the announced turn against
  the saved rotation, booting each turned glass both ways round (`bootTurns`:
  the dash at `ROT_PORTRAIT` and `ROT_PORTRAIT_INV`, the nightlight at
  `Orient::R90` and `R270`); with the harness made to announce 1 for every
  side turn, the two counterclockwise boots fail. On the committed dist all
  ten boots pass with the turn unsaid. The HAL change moves the dist for all
  six display flavors; it was not rebuilt here. The announcements were seen
  natively (a scratch HAL build with LVGL 8.4), not in a browser. On the
  nightlight, turn 1 follows the firmware's own reading (`io/orientation.h`);
  whether the panel draws that way round still needs a bench check. Not looked
  at in the Lab page by hand. Found here: F250 and, by the review, A64.
- [ ] **A57 [decision] May the iOS timeline date a WAP row from its bucket
  when the device's clock is GPS-set?** `GET /api/events/today` rows carry
  only `time_bucket`; on a WAP with a GPS fix that is the household's
  10-minute bucket of the day (F28, A48), on one without it counts from boot,
  and nothing on the wire says which, so `WapEvents.swift`'s `anchoredDates`
  reads deltas only. The deltas also break across an offset change inside one
  boot's ring (the first fix, a DST change, a household zone set or changed, a
  drift correction), and the per-pass recompute can put any row one bucket out
  at a 10-minute boundary. Options: (a) keep deltas only and say so in the
  timeline; (b) have the firmware say per row (or per boot, with an offset
  epoch) whether the offset was applied, so the app can date synced rows by
  their bucket and split a page where the offset moved, a flag that adds no
  finer time than Invariant III's bucket; (c) anchor only on rows from a
  synced device. Found in A48 (#1762).
- [ ] **A58 [code+decision] The source-level wasm32 check covers the Vision
  box pipeline alone.** `vision_wasm32.test.js` (A49) builds
  `detection_pipeline.h` freestanding; the rest of the Vision core
  (`presence_fsm.cpp`, `voxel_tracker.cpp`, `detect_config.cpp`, the bindings'
  JSON) and the WAP audio core need libc and the emulator's Arduino shim
  (`snprintf`; the audio bindings' `millis()` returns `unsigned long`, 64 bits
  natively and 32 in wasm32 and on the ESP32), which a freestanding clang
  build lacks. A wasi-libc sysroot (wasi-sdk) could build whole cores for
  wasm32 and run `native_cores.test.js`'s call-for-call scenarios between that
  build and the g++ one on every PR, with no emsdk. Decide whether the 32-bit
  check should cover whole cores, and at what CI cost. Found by A49 (#1762).
- [ ] **A59 [code] The Lab's bench prints the ESP32-S3's ROM banner for every
  board.** `canary-local/emulator/web/bench.js`'s `romBanner()` writes
  `ESP-ROM:esp32s3-20210327` and the S3's strap story (GPIO0 for download)
  whatever the card, so the Nightlight's twin, an ESP32-C3 with BOOT on GPIO9,
  prints an S3 banner at power-on, reset and download mode. No C3 ROM banner
  is on disk to quote; one has to be captured from a C3, or each bench block
  has to name its chip's banner. Found by A54 (#1762).
- [ ] **A60 [decision] Whether a bench draws a slide switch and a battery
  fast-forward for a board without a battery path.**
  `canary-local/assets/app.js`'s bench view always makes an ON/OFF switch chip
  from `power.switch` and a "battery time x60" knob, so the Nightlight's bench
  (USB-C its only power) and the Nightstand stick's show a switch labeled "(no
  battery path)" or "(not fitted)" that toggles and gates nothing, and a
  fast-forward with no cell to speed. The battery chip itself is inert there
  since A54. Either a bench block may say it has no switch and the view draws
  neither, or the labeled no-op controls stay. Found by A54 (#1762).
- [ ] **A61 [code] Model the Nightlight's backlight cap in the emulator's
  display HAL.** The board's HAL clips the backlight at `CD_BL_MAX_PCT` (50%,
  the nightlight config) and the firmware prints that cap at boot, but
  `canary-local/emulator/src/emu_hal_display.cpp` sets the level as asked, so
  the twin's glow and its bench backlight row can read brighter than the
  glass. The bench says so for now. Applying the cap where the board does
  would make the twin match; it moves the dist. Found by A54 (#1762).
- [ ] **A62 [code] `fleet.html` turns a malformed deep link into a page
  error.** `canary-local/assets/app.js` reads its card with a bare
  `decodeURIComponent(location.hash.slice(1))`, so `fleet.html#%E0` throws
  `URIError` once the cards have rendered, instead of quietly opening nothing.
  Decode inside a `try`, as `probe_server.mjs`'s `lookup()` does. Found by A52
  (#1762).
- [x] **A63 [code] `onboard_probe.mjs`'s nightlight walk fails a read now and
  then.** In 31 local walks of `--flavor nightlight` (20 on the wave-15 Lab
  files, 11 with the base `harness.js`, `emu-shell.js` and
  `onboard_probe.mjs`), nine failed one check. Five were the landscape splash
  ("9 canvas reads, never inked whole at full strength"), seen with both sets
  of files. Four were the phone-joined scene's ink ("3 line(s) the firmware
  draws show no ink where it says they are"), three on the landscape glass and
  one on the unturned one, all on the wave-15 files; that split is not settled
  (an exact-test p of about 0.15), and no path from the change to it was
  found. A read that races the frame it checks would fail this way, and CI's
  wasm job runs the same walk. Find what each read waits on and make it wait
  for the frame it reads. Found by A51 (#1762).
  *Done (#1762):* both were reads that did not wait for the frame they read,
  and neither came from the wave-15 change. The labels are the firmware's
  object tree between two passes of its loop; the canvas is the last frame
  LVGL flushed. LVGL 8.4 runs its display refresh before its animations in
  one `lv_timer_handler()` pass (`lv_timer_create()` puts each new timer at
  the head of the list, and the refresh timer is made after the animation
  timer), so a fade's step lands after its pass's frame. The phone-joined
  check comes right after `stepTime(5000)`, which finishes the scene's 260
  ms fade in one such step: a trace of every turn of the page after the step
  read all three lines at full opacity with no ink on the canvas for about
  30 ms, until the next frame inked them. The splash's read waited for two
  flushes past the read, or 2 s; the glass is still during a line's hold, so
  the second flush could be the next beat's. On "Dark means all is well. If
  I glow, look at me." and "Add another of me and we compare notes." it came
  about 1.6 s on, against holds of 1.9 and 1.7 s of wall time on the
  half-speed clock. CI's wasm job failed that way on 5755ce2b, on the second
  line. `inkOnFrame()` now reads the labels, waits for the next frame (300 ms
  with none means nothing was left to draw), and reads the labels and the
  pixels in one turn. `linesSettled()` (`onboard_glass.mjs`) holds the frame
  only to the lines that drew whole before it. A line that came whole after
  it, or a held line still dark, is read again, and the fifth read is judged
  as it stands. `holdLines` and the splash both read this way.
  `onboard.test.js` holds `linesSettled` and pins both reads (the pins fail
  on the old probe). With the fix, 20 of 20 local nightlight walks passed (8
  unturned, 12 landscape), and a mutant whose "phone" lines never ink still
  fails, on the join scene's stuck-phone hint. Run in local Chromium on the
  CI-rebuilt dist; not bench-tested.
- [ ] **A64 [code] `glass_turn_test.cpp` does not read the turn the HAL
  announces.** `canary-local/emulator/test/glass_turn_test.cpp` counts
  `js_display_ready` calls, but its stand-in drops the fourth argument, so the
  native glass test holds the shape of each turned glass and not the turn A56
  sends with it. Record the turn and require 1, 3 and 2 for the dash's
  `ROT_PORTRAIT`, `ROT_PORTRAIT_INV` and `ROT_LANDSCAPE_INV`, and 1 and 3 for
  the nightlight's `Orient::R90` and `R270`. Today `scene_figures.test.js`
  pins the HAL's source for this, and `boot_probe.mjs` holds it in a browser
  only once the dist is rebuilt. Found by A56's review (#1762).

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
  *Partly done (#1762):* both publishers now ask Home Assistant for the
  documented ids with `default_entity_id`, and the kernel wizard's dashboard
  names the zone ids the bridge asks for. Read from Home Assistant core's
  source (`components/mqtt/` at 2025.9.0, 2025.10.0, 2026.3.0, 2026.4.0 and
  2026.9.0): the option is a full entity id, `<domain>.<object id>`,
  abbreviated `def_ent_id`, added in 2025.10.0 beside the older `object_id`
  (removed in 2026.4.0). Home Assistant applies it when it first registers an
  entity, so one already in its registry keeps its id (a deleted entity that
  comes back takes the new one), and a release before 2025.10 drops the key
  without a word. canary-wap (`csi_mqtt.cpp`): every discovery config (the 24
  table entities, the firmware update entity, the auto-update and mic mute
  switches) carries `<component>.<device id slug>_<object_id>`, the slug being
  what Home Assistant's slugify makes of the id (`canary-s3-4dC2` becomes
  `canary_s3_4dc2`), so `binary_sensor.<id>_smoke_alarm`, `_co_alarm` and
  `switch.<id>_mic_mute` are asked for; unique ids are unchanged. Two WAP ids
  that differ only in letter case slug alike, and Home Assistant gives the
  second `_2` (from the review). `tests_host/test_ha_discovery_ids.cpp` (909
  checks) drives the real `csi_mqtt.cpp` through a connect burst and holds all
  27 configs to a strict JSON reader, the documented id, one `def_ent_id`
  each, the unchanged unique id and the 768-byte body buffer at the longest
  identity: the presence config is the longest, 762 bytes there and 654 at a
  real 14-character id. On the code before, 185 checks fail (135 configs with
  no `def_ent_id`, 50 documented ids no config asks for); a comma lost after
  the new member fails 120. `gen_wap.py` pins the firmware's id format (its
  sandbox `ha` lines are typed by hand). Kernel
  (`src/bin/event_mqtt_bridge.rs`): the sensor, binary sensor and button
  configs carry `default_entity_id` (`sensor.pwk_<zone>_events`,
  `binary_sensor.pwk_<zone>_motion`, `sensor.pwk_last_event`,
  `sensor.pwk_daily_digest`, `binary_sensor.pwk_chain_problem`,
  `button.pwk_verify_now`); two unit tests hold every config to its id, domain
  and unique id, and three tests fail with the key left out (`cargo test --bin
  event_mqtt_bridge`, 15 tests, run in the package's build; clippy is CI's).
  The kernel wizard's `generateDashboard` built zone ids from the digest's raw
  ids (`zone:front_door` gave `sensor.pwk_zone_front_door_events`); it now
  drops the `zone:` prefix and slugs as the bridge does, held by
  `test_wizard_dashboard_ids.test.js` (3 cases, run by `pwk-wizard-tests.yml`;
  two fail on the page before). The docs say what the key buys and what it
  does not, and the mesh and Chirp/Beacon ids drop the doubled `canary_`
  (`docs/audit/v0.3_closeout.md` keeps its form as the dated v0.3 record).
  Left: no running Home Assistant has confirmed the new ids, so the three
  lines HA15 took out of the HomeKit recipe's filter stay out until a live
  install (2025.10 or later, entities registered fresh) shows them. The WAP's
  Arduino compile is CI's; nothing ran on a unit or in a Home Assistant. Found
  here: HA29 (Home Assistant before 2025.10), HA30 (a Template Sensor example)
  and, by the review, F241 (an MQTT prefix of 32 characters).
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
- [x] **HA24 [code] Home Assistant reads neither the event-id space warning
  nor the egress counters.** Since #1762 both devices' MQTT health carries
  `event_id_space_low` and the canary's carries `csi_event_egress` and
  `offline_queue` (sweep F82, F109), but the integration's health sensor keeps
  a fixed attribute set. So a device nearing the wrap raises nothing in Home
  Assistant, and a canary losing rows (`unsent_dropped`,
  `offline_queue.dropped_overflow`) reaches no attribute. Add a diagnostic
  binary sensor or a repair issue for the flag (naming the re-pin once F82's
  recovery is decided) and the counters as health attributes, then the HACS
  mirror's resync (U6). Found by F82 and F109 (#1762).
  *Done (#1762):* Home Assistant reads both. An **Event ID Space Low** binary
  sensor (`event_id_space_low`; device class problem, diagnostic) follows the
  health flag: on for `true`, off for `false`, unknown when a health publish
  lacks it. Discovery creates it on the first health publish that carries the
  flag, so only the canary base and the canary-wap grow one, and a device that
  never sends it (canary-sense, older firmware) gets no sensor stuck at
  unknown. A binary sensor rather than a repair issue: the integration's
  device flags (SD Replacement Recommended, the tamper types) are binary
  sensors, its one repair issue is about its own MQTT setup, and a repair
  would have nothing to offer while F82's recovery is undecided; the sensor's
  docs say only that the flag warns, and name no recovery. The Health sensor
  carries `csi_event_egress` (the planner's counters nested) and
  `offline_queue` as attributes, keeping only the counters `const.py` names
  when they are non-negative integers. The canary base's come from its health;
  the canary-wap's from its retained `egress` topic (F149), which the Health
  sensor also subscribes to. Those start over at every boot and the topic is
  retained, so a canary-wap's show only while the body pairs with its latest
  health (`egress_topic_pairs()`: the same `firmware_version`, an `uptime` no
  later than the health's): a body an earlier boot left, or one newer firmware
  left before a rollback, is not shown as current, live or after a Home
  Assistant restart. An empty retained publish clears it, and the topic never
  overrides a canary base's own counters. One window remains: if a canary-wap
  reboots on the same firmware and its first health is no earlier in its boot
  than the previous boot's last egress, those older counters show until the
  new boot's egress publish, which follows in the same loop pass.
  `tests/test_egress_health.py` (21 tests) covers the helpers, both devices'
  paths in either order, pairing across a rollback, a restart and a
  same-firmware reboot, the empty clear, junk on either topic, both sensors'
  subscriptions and discovery; in the monorepo four of them also hold
  `const.py`'s names to the canary-wap's `Stats`, `stats_json()` and
  `publish_egress()`, and to the canary's `main.cpp` objects (they skip in the
  mirror). Each mutation tried fails it, among them pairing always true, the
  version or the uptime ignored, no empty clear, discovery without the flag
  gate and the flag sensor subscribed to another topic. `strings.json` and
  `translations/en.json` name the sensor; mypy and ruff pass. Docs:
  `homeassistant_setup.md`, `csi_developer_api.md`, the Lab's HA page data
  (`gen_homeassistant.py`) and a bench row in
  `hardware_verification_checklist.md`. The HACS mirror needs its resync (U6).
  Host-tested against the integration's stubs, not a running Home Assistant;
  hassfest is CI's (`validate.yml`); not bench-tested (U1). Found here: HA27.
- [x] **HA25 [code] The Presence and Dwelling binary sensors canary-vision
  announces over MQTT discovery (and canary-sense's and canary-sentinel's
  Presence) cannot turn on.** Their `value_template` is
  `{{ value_json.presence | default(false) }}` (`dwelling` for Dwelling) over
  the state row, where the field is a JSON boolean, so the template renders
  `True` or `False`. Home Assistant's MQTT binary sensor compares the rendered
  text to `payload_on` `"true"` and `payload_off` `"false"` with plain
  equality (`homeassistant/components/mqtt/binary_sensor.py`;
  `helpers/template` does not lowercase it), logs "No matching payload found"
  and leaves the entity unknown. Read from Home Assistant's source and checked
  with jinja2, not reproduced in a running Home Assistant. Render the strings
  (`{{ 'true' if value_json.presence else 'false' }}`) or set
  `payload_on`/`payload_off` to `True`/`False`, pin it with a render test, and
  then the Vision dashboard's voxel card can gate on Presence (A39 gated it on
  the confidence sensor instead). The integration's own occupancy sensor reads
  the boolean itself and is not affected. A firmware change in three products'
  discovery. Found by A39's review (#1762).
  *Done (#1762):* every binary sensor the three products announce renders its
  payload strings, `{{ 'true' if value_json.<field> | default(false) else
  'false' }}`: canary-vision's Presence and Dwelling, canary-sense's Presence
  and Breathing confirmed (`CANARY_SENSE_VITALS` builds), and
  canary-sentinel's Presence, Anomaly and Channel blinded (the last three had
  the same template and the same defect; the Sense's Radar link problem
  already rendered strings). Strings rather than `payload_on`/`payload_off`
  `True`/`False`: the comparison then does not depend on how the engine prints
  a Python bool, the payloads keep the rows' JSON spelling, and with
  `default(false)` inside the test a row without the field reads off with no
  template warning. canary-vision's two payload buffers grow from 768 to 1024
  bytes, as the Sense's and Sentinel's are: at 768 the longer template would
  cut the Presence JSON off from a 40-character device id (the old template
  already did from 44). `scripts/tests/test_ha_discovery_binary_sensors.py`
  extracts every binary_sensor announcement from the three `ha_discovery.cpp`
  files, holds each to the field it is named for and that field to a JSON
  boolean on its product's state row, renders each template in Home
  Assistant's own template environment (`scripts/tests/_ha_jinja.py`: the
  sandbox, LoggingUndefined and the round/int/float filters of Home Assistant
  2025.4.4's `helpers/template.py`, on jinja2 3.1.6) and compares it as its
  MQTT binary sensor does. Every sensor but Radar link problem (exempt by
  object id, not by reading its template) must read a row without its field as
  off with no warning, and every announcement, built for a 47-character id
  with each flavor's model and its device objects cut at their declared sizes,
  must fit its buffer. On the base templates 22 checks fail. jinja2 was
  installed in no CI Python job, so the repo's template render tests skipped
  in CI; lint.yml's scripts/tests step now installs `jinja2==3.1.6` (Home
  Assistant's own pin). The Vision dashboard's voxel card gates on Presence:
  the confidence gate A39 used said nobody was there on rows where the device
  still held someone present. While Presence is neither on nor off (Vision
  firmware without this fix, since the dashboard YAML is copied separately
  from firmware updates) the card falls back to that confidence gate and says
  so, with a hint to update the firmware, and while the voxel sensor has no
  value it says the Vision has not reported; `test_vision_dashboard_voxel.py`
  renders each case. The Lab's Vision page called Presence "motion class" and
  Dwelling "occupancy class"; it now names what discovery announces
  (occupancy; no class), pinned by `vision.test.js`. No carried HA file
  changed. Not reproduced in a running Home Assistant; the three firmware
  compiles are CI's; not bench-tested. Found here: F181.
- [x] **HA26 [code] The Vision interaction alert always reports "confidence
  0%".** `homeassistant/automations/securacv_vision_presence.yaml` prints the
  event's `confidence` on `interaction_likely`, which fires after
  `presence_ended`, when the FSM's confidence is 0 (no box in frame). Drop it
  for that event, or say what it measures. Found by F130 (#1762).
  *Done (#1762):* dropped for that event. `securacv_vision_presence.yaml`
  prints the confidence for `dwell_started` only, which is sent from a frame
  with the person in it, and `interaction_likely` prints how long the visit
  lasted. The litter-box visit-completed alert (`securacv_litterbox.yaml`),
  which pages only on `interaction_likely`, printed the same confidence and no
  longer does. That confidence is the best box score of the frame the event
  was sent from, after the visit has ended: 0 on the empty frame after
  `presence_ended`, or, when someone is seen on that very frame (F152), the
  next visit's box; never the visit it reports.
  `scripts/tests/test_vision_automation_clocks.py` renders both messages for
  the rows the device sends in Home Assistant's template environment
  (`_ha_jinja.py`, with HA's own `round` and `int`), holds that neither prints
  a confidence on `interaction_likely` (including a row carrying the next
  visit's 91), and pins the firmware lines behind it; three of its tests fail
  on the old recipes. `test_vision_presence_fsm.cpp` holds the usual
  `interaction_likely` row to confidence 0 and the late one to the next
  visit's frame. A commit in this work (2e1849d) also added `| int` to the
  litter-box seconds and says they "now print as a whole number": Home
  Assistant's `round` already returns an int at precision 0, so they always
  printed that way there, and the `| int` is dropped again.
- [ ] **HA27 [decision] Home Assistant records the Health sensor's counters on
  every change.** The recorder stores each Health sensor state write with its
  attributes. `uptime_seconds` already makes every health publish a new
  attributes row, and since HA24 a canary-wap's retained `egress` topic writes
  the sensor a second time each health cycle, with its thirteen counters.
  Decide whether the counters (and the uptime) go in the entity's
  `_unrecorded_attributes`, which drops their history. Found by HA24 (#1762).
- [ ] **HA28 [code+decision] Home Assistant's Last event sensor reads
  `unknown` for every stateless row.** `csi_event_wire.h` writes the row's
  state as `event_type`, and a stateless row (`meta.daily_summary`'s
  `daily_summary`, `meta.quiet_hours`' `held_summary`) has none, so the canary
  sensor's state turns `unknown` at each; since F121 that is every night at
  23:55 on every canary and canary-wap with a synced clock. Decide whether a
  stateless row's `event_type` falls back to its type (a wire change; the
  signed canonical carries `state`, not `event_type`) or Home Assistant reads
  `type` for such rows. Found by F121 (#1762).
- [ ] **HA29 [code+decision] Home Assistant before 2025.10 still names the
  WAP's and the kernel's MQTT entities from the device and entity names.**
  `default_entity_id` (HA16) is dropped there by the discovery schemas'
  `REMOVE_EXTRA`; those releases read `object_id` (an object id with no
  domain). 2025.10 through 2026.3 accept both, and 2026.4 removed `object_id`.
  The HACS integration's floor is Home Assistant 2024.4.1 (`hacs.json`).
  Sending `obj_id` as well would give 2024.4 to 2025.9 the documented ids too,
  at about 40 more bytes per WAP config, and the presence (762) and mic mute
  (754) configs are within 14 bytes of their 768-byte buffers at the longest
  identity, so both buffers (`publish_one_discovery`'s and
  `publish_mic_discovery`'s) would have to grow (`test_ha_discovery_ids.cpp`
  holds them). Decide between sending both and saying in the docs that the ids
  need 2025.10. Found in HA16 and its review (#1762).
- [ ] **HA30 [code] `docs/homeassistant_setup.md`'s Template Sensor example
  reads an entity no publisher creates.** It reads
  `sensor.pwk_boundary_crossing_count`; the kernel bridge makes
  `sensor.pwk_<zone>_events`, `sensor.pwk_last_event` and
  `sensor.pwk_daily_digest`. Point the example at a real entity or drop it.
  Its slug example also names a zone the kernel refuses (`Front Door`;
  `validate_zone_id` takes only `zone:` and `[a-z0-9_-]`, so `zone:back-gate`
  becomes `back_gate`). Found in HA16 and its review (#1762).
- [ ] *(Mirror repo itself: no code work. It was byte-identical again as of
  securacv-homeassistant#17 (2026-09-24), which resynced the 33 carried
  files #1703, #1704 and #1718 had moved. The same PR brought the store
  page's watch-actions, key-pinning, broker-TLS and Apple Home sentences,
  and a `lint_readme.py` overclaim check that reads a hard-wrapped claim as
  one and refuses "encrypted by default". PR #1725's carried files
  followed in securacv-homeassistant#19 and PR #1727's in #20 (2026-10-01),
  byte-identical again (U6). PR #1761 added one carried test (F46's
  `tests/test_replay_one_id_space.py`), carried by hand in
  securacv-homeassistant#21 (2026-10-02). Its health
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
- [ ] **C16 [decision] No printed part holds the Dash portrait.**
  `glass_settings.h` offers the dash glass turned portrait (`ROT_PORTRAIT`,
  "for a wall column or a tall bedside face"), but
  `canary_dash_display.scad`'s desk stand is cut for the landscape case (the
  90-degree plug's channel under the USB-C in the case's bottom wall, the
  screw lobes standing on the pedestal outside the lip) and its wall cradle's
  four dock features sit at ±38 x ±16, a landscape rectangle; turned a
  quarter, neither takes the case. The Lab draws a turned dash standing alone
  for that reason (A47). Decide: a portrait mount (a cradle with a second dock
  pattern, or a stand cut for the case on its side), or the portrait setting's
  copy saying it needs one. Found by A47's review (#1762).
- [ ] **C17 [decision] Which lights the C3-LCD-1.47 has.**
  `docs/hardware/enclosure/canary_c3_lcd147.scad` cuts its light band for an
  onboard RGB LED and keeps the USB end lit for a charge LED, while
  `firmware/boards/waveshare-esp32c3-lcd147/pins/pins.h` (`HAS_RGBLED 0`, no
  WS2812) and the board README (no WS2812, no battery charger IC) describe a
  board with neither. The Lab's Nightlight bench lists no lights until a look
  at a board settles which file is wrong, and that file needs correcting.
  Found by A54 (#1762).

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
  unique. Found by A29's review (#1762).
- [x] **D11 [code] `docs/csi_developer_api.md` calls the Tuning Lab's bundle
  "signed" and part of the "witness-chain export format".** It is neither.
  `handle_tune_get_preset()` streams a flat, unsigned JSON object of every
  coefficient's value, and `POST /api/tune/preset` is
  `handle_tune_post_coefficients()`, which checks no signature. Reword the two
  table rows and the sentence under them, or sign the bundle. From reading;
  not probed. Found reviewing F93 (#1762).
  *Done (#1762):* reworded; nothing is signed. The two
  `docs/csi_developer_api.md` rows now say `GET /api/tune/preset` exports
  every knob's current value as one flat, unsigned JSON object (a download
  named `tuning-preset.json`) and `POST /api/tune/preset` imports such an
  object through the same handler as `POST /api/tune/coefficients`, which
  checks no signature. The sentence under them now says the bundle is that
  object, one `"<full_key>": value` pair per knob, not signed and not part of
  the witness chain or its export; that an import checks only that each key
  names a knob and its value is a number (or true / false), clamping it to the
  knob's range; and that the Lab saves it as a downloaded file and loads it
  from a file the owner picks, the device keeping no copy. Read from
  `handle_tune_get_preset()`, `tune_post()` and `tune_ui.h`. The Lab's own
  footer now calls the file plain, unsigned JSON.

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
- [x] **CI5 [code] The Docker sidecar e2e reads a logged line as missing.**
  On e9c594a0 (#1762) "Build and e2e test" failed with "no
  SECURACV_API_BIND=all startup notice in the LAN-mode sidecar's log", and
  the log it dumped next held that notice. `docker/sidecar/ci_e2e.sh` runs
  under `set -o pipefail` and checked the log with `docker logs | grep -q`.
  grep exits at its first match, `docker logs` is still writing the rest of
  the log into the pipe and dies of SIGPIPE (141), and pipefail reports the
  pipeline as failed. The longer the log runs past the match, the likelier
  the miss. Its two loop checks had the same trap and were saved only by
  retrying. Found on #1762.
  *Done (#1762):* `logs_have()` reads the log into a variable and greps the
  copy; `running()` asks `docker inspect` for `State.Running` instead of
  piping `docker ps` into `grep -q`; the retained-discovery check captures
  `mosquitto_sub`'s message and tests that it is not empty. With a stub
  `docker` that writes 200000 lines after the notice, the old pipeline
  returns 141, `logs_have` finds the notice, and a line that is not there
  still reads as missing. `bash -n` and shellcheck are clean. CI-only: the
  e2e runs in docker-sidecar.yml.

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
