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
  "the real gating work" section), the 23-item
  `docs/audit/hardware_verification_checklist.md` is entirely unrun, and 12
  board pin maps carry "NOT yet validated on bench". The playground doc calls
  this "the thing an AI cannot do from CI." Un-gates: F-section flags,
  C6, C7, plus the sentinel/sense bench boxes.
- [ ] **U2 [human] The release signing-key ceremony.** The pinned key is the
  all-zero placeholder (`desktop/src-tauri/src/release.rs`,
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
  The trees are byte-identical today; nothing guarantees they stay so.
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
  (#1696)
- [ ] **F5 [code+decision] Ed25519 private key in NVS without enforced flash
  encryption.** `firmware/canary/lib/securacv_mesh/src/mesh_state.h` ("audit-O2
  deferred work"). Decide the enforcement posture, then implement. Roadmap
  item 8.
- [x] **F6 [code] Camera init/deinit vs peek-task race** — confirmed real by
  source inspection (the stream task's freeze recovery cleared `peek_active`
  then deinit+begin behind flag guards only, while the loop-task vision
  capture and two httpd handlers could call into the driver), then fixed:
  one lifecycle mutex in `CameraManager`. `captureFrame()` holds it until
  `returnFrame()` (the frame buffer is driver memory); lifecycle ops take it
  with a 2 s timeout and fail soft instead of blocking toward the 8 s task
  watchdog. Compile-tested; a live freeze repro is U1 bench work. Roadmap
  item 7 updated (#1696).

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

### Mesh / fleet / beacon

- [ ] **F10 [code] Five of eleven specced mesh REST endpoints are deferred**
  (remove/leave/name/enable/alerts-DELETE) —
  `firmware/canary/lib/securacv_network/src/securacv_network.cpp`, per
  `spec/canary_mesh_network_v0.md` §8. Scoped 2026-09-21: `remove` is
  gated on porting the WAP's atomic `opera_secret` rekey transaction
  (`MSG_OPERA_REKEY` + ACK state machine, mesh_network.cpp:163+) into the
  PIO mesh layer, whose envelope has no rekey message type at all —
  shipping remove-without-rekey is the security misrepresentation §8.3
  explicitly refuses. That port is a design-review-sized change (new wire
  type, per-peer ACK tracking, commit/rollback, NVS re-persist,
  trusted-peer re-registration) — treat it as its own PR with maintainer
  eyes on the crypto, not a sweep item. `alerts` needs an alert message
  type dispatched first (TAMPER_ALERT is reserved but unimplemented in
  the PIO envelope). `leave` (local forget + best-effort notify) is
  implementable without rekey — the leaver discards its own secret.
- [x] **F11 [code] Fleet peer liveness is fabricated** — the liveness half is
  done: `mesh_session` now records the source MAC of every fully verified
  opera-authenticated frame against the sender's fingerprint (signature +
  opera_id + replay all passed, so the binding is as trustworthy as the
  frame; an unverified or replayed frame cannot rebind it — host-tested),
  and `/api/mesh/peers` joins that MAC into the live transport table for
  real `state`/`last_seen_sec`/`rssi`. A peer that hasn't spoken this boot
  honestly reads OFFLINE/never. `alerts_received` stays 0 and stays honest:
  the PIO envelope reserves TAMPER_ALERT/POWER_ALERT but nothing sends or
  dispatches them yet — counting attribution lands with the alert channel
  (F10's alerts endpoint). Spec §8.3 peer-fields note updated. (#1698)
- [ ] **F12 [code] `ble_mesh.cpp` (canary-wap) is a stub module** — every
  publish returns `false` with a "transport not wired" log. Decide whether to
  build the transport or delete the seam.
- [ ] **F13 [code] Beacon gaps (canary-wap `beacon_channel`):** CAP-gateway
  upstream-signature path specified but not implemented; `BEACON_MSG_CANCEL`
  not built (paired devices stay in ALARM until expiry); COSIGN_REQ pairing
  channel is an unencrypted broadcast. Tracked in the file header as "v0.3".
- [ ] **F14 [code] Staged mesh PRs referenced in headers never landed** —
  `mesh_envelope.h` / `mesh_session.h` name PR 2g/2h/2i (peer table, replay
  counters, NVS persistence) and record a live canary-wap pair-frame bug.
  Re-scope these into real issues or land them.

### Network surface & provisioning

- [ ] **F15 [code] No TLS on the canary.local HTTP/peek surface.** The one
  gap `firmware/PARITY_PLAN.md` marks ❌ in *both* trees.
- [ ] **F16 [code] WPA3/PMF + per-device AP password** on the WAP join path —
  roadmap item 21; `pre_build.py` already warns on the hardcoded AP password.
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
- [ ] **F19 [code+decision] `FEATURE_TAMPER_GPIO` is defined but never
  consumed** by canary-wap firmware (status "planned" in
  `boards/boards.config.json` and the canary-local device JSON). Re-scoped
  2026-09-21 as [code+decision]: the device catalog itself frames it as a
  choice — "needs firmware support, OR use securacv_touch enclosure-tamper
  mode" (`canary-local/devices/build.json`), and the default tamper pin
  (GPIO4) is shared with the touch default. Decide which mechanism is
  canonical for reed/hall enclosure tamper (a new GPIO consumer module
  feeding the tamper event path, or retiring the flag in favor of the
  touch mode); the pin maps are also U1 bench-unvalidated, so the wiring
  half is bench-gated either way. (Re-scope recorded in #1699.)

### Parity & sub-projects

- [ ] **F20 [code] Arduino↔PlatformIO parity debt.** Canonical ledgers:
  `firmware/FEATURES.md` (the ❌/⚠️ dashboard) and
  `firmware/canary/CONSOLIDATION.md` (15-row gap inventory; phases 3, 5–8
  unstarted; gap #11, the unauthenticated provisioning-receipt endpoint, is
  marked security-High — do that one first).
- [ ] **F21 [code] canary-wap enterprise readiness** —
  `firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md`, 18 unchecked
  boxes incl. three unmet acceptance criteria. §1 (security/privacy) is done.
- [ ] **F22 [code+human] canary-sentinel is Phase 0** — Phase 1 wiring plus 7
  bench boxes (`firmware/projects/canary-sentinel/README.md`). Bench half is U1.
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
- [ ] **F24 [code] Emulator wave 2: first-boot captive-portal theater.**
  `canary-local/emulator/src/emu_net.cpp` hardcodes `provision_needed() =
  false`; the most important first-run UX is unemulated. Roadmapped in
  `canary-local/README.md` §6, with the chirp-fallback and live-pins waves.
- [ ] **F25 [decision] Adopt SD tamper narration on the canary tree.** F2
  gave the canary tree a real SD hot-swap machine, but the integrity
  watcher still feeds pinned ABSENT (the `src/main.cpp` tamper-narration
  comment points here). Wiring the live state in would add the
  `sd_error`/`sd_remove` event kinds to this host's vocabulary — a
  dictionary decision (AGENTS.md rule 5), not a data feed; canary-wap
  remains the only host narrating SD stories until it is made.
- [ ] **F26 [code+decision] Timeline history deeper than the witness ring.**
  F7 pages the 32-record RAM ring; the full history sits in
  `/WITNESS/records.jsonl` on the SD card, and the HTTP task never touches
  SD (the `handle_witness` contract). Serving older pages means a loop-task
  SD read bridge (request queue + bounded chunked reads of the JSONL tail,
  parsing via `witness_store`) — design the bridge before writing it. Until
  then deep history remains the export/unseal tools' job.
- [ ] **F27 [code+decision] The canary tree has no Scout pairing surface.**
  `ble_scout_pair()` has zero callers in the PIO tree — the WAP's setup UI
  (PR 5c) is the only pair-time MAC producer anywhere — so even with the
  scan fixed (F1 + the #1695 audit), paired-beacon room attribution cannot
  be configured on a canary build; only the unpaired consumers (fleet
  roster) exercise the scan. Decide the surface (an `/api` pair endpoint on
  the canary web UI, or WAP-only by design), then wire it.
- [ ] **F28 [code+decision] No timezone setting — day-aligned features run on
  UTC.** F8 aligned `time_bucket` and the quiet-hours minute-of-day to the
  wall clock, but the only clock either tree has is GPS UTC, so "midnight"
  and a user's "22:00–07:00" quiet window are UTC, not household local
  time. Decide where the timezone lives (a settings field + NVS on each
  host, or derived from the paired app's locale at pairing time), then
  thread it into the offset both `updateCsiClockOffset` /
  `update_csi_clock_offset` helpers compute. The WAP's NFPA-72
  waking-hours check reads the same clock and has the same skew.
- [ ] **F29 [code+decision] The canary PIO tree publishes no events topic.**
  The HA integration's event entities consume `securacv/{id}/events` (two
  dialects: the WAP's `csi_mqtt` and canary-sense's radar witness), but
  the canary tree's `mqtt_publish_event()` has zero callers — a PIO canary
  shows no event stream in HA even though it compiles the same csi_event
  chokepoint the WAP feeds from. Decide whether the canary adopts
  csi_mqtt-style event egress and with what persistence (the WAP pairs it
  with SD-backed backfill and a watermark), then wire it. The F9 offline
  queue already bounds transport loss the day callers exist.

---

## 2. Apps (desktop Flasher, Lab, iOS, tvOS)

- [ ] **A1 [code] tvOS has no timeline view.** iOS has the parity-proven
  `TimelineScrubView` + `viewer/timeline_core.js`; `tvos/` has zero
  timeline/scrub code. Port the shared core to the Wall.
- [ ] **A2 [code+decision] tvOS verification is structurally dark.**
  `WallCanary.swift` hard-codes `allVerified = false`; no pairing ceremony
  pins a key and the TV sends no sealed-log token (`tvos/README.md`). Design
  the pairing/token flow (decision), then wire it (code).
- [ ] **A3 [decision] Wall-reachable sidecar bind/port** —
  `tvos/discovery/DISCOVERY.md` "a bind and port decision not yet made";
  Docker-sidecar users cannot reach the Wall at all.
- [ ] **A4 [code] Lab native serial is a Phase-2 stub.** The real
  `list_serial_ports()` sits in a comment above the stub in
  `desktop-lab/src-tauri/src/lib.rs`; `serial:false`, `notifications:false`.
  This is the stated reason the native Lab exists.
- [ ] **A5 [code] Lab mDNS + BLE discovery** (HTTP-poll only today) and the
  menubar companion with the signed timeline — `desktop-lab/README.md`
  Roadmap items 2–3.
- [ ] **A6 [code] iOS Unseal screen is an empty placeholder**
  (`ios/Sources/SecuraCV/Views/KeysView.swift`); the crypto exists repo-side
  in `tools/unseal_snapshot.py`. Build the import + decrypt flow.
- [ ] **A7 [code+decision] Secure-Enclave-backed key custody on iOS** —
  named a roadmap item in `ios/README.md`; today keys are Keychain
  generic-password items.
- [ ] **A8 [human-gated by U3] Exercise the CloudKit household/away path.**
  `HouseholdShare.swift` degrades to a no-op in unsigned builds, so CI has
  never touched iCloud. Needs a signed build (U3), then a test pass.
- [ ] **A9 [human] Windows enablement.** The hub-io raw-disk write backend is
  complete but staged off pending a VM/hardware pass
  (`desktop/hub-io/src/write.rs`, "STAGED — NOT YET ENABLED"), and
  `secret_store.rs`'s Windows branch has never been compiled by CI.
- [ ] **A10 [code] Linux secret-store backend** — currently "none": profile
  passwords and bearer tokens land in a plaintext prefs file
  (`desktop/src-tauri/src/secret_store.rs`). The frontend discloses it; fix it
  anyway (libsecret/keyring).
- [ ] **A11 [code] Native Flasher hardcodes what the browser derives.**
  `canary-local/tests/desktop_parity.test.js` header: chip tables, USB IDs and
  the release host are literals in the native app, kept in sync only by CI.
  Make native read the embedded catalog; delete the matching assertions.
- [ ] **A12 [code] Desktop Flasher lacks the eFuse-read diagnostic** the
  browser flasher has (espflash has no fuse-read; the parity test currently
  forces a "browser-only" disclosure). Needs an espflash upstream check or a
  raw-command implementation — investigate, then either implement or record
  why not beside the disclosure.
- [ ] **A13 [human-gated by U3/certs] macOS signing/notarization** — both Mac
  apps ship unsigned until `ENABLE_MACOS_SIGNING` + certs exist
  (`desktop-lab/README.md`, `desktop/INSTALL.md`).

---

## 3. Home Assistant (monorepo `custom_components/` + HACS mirror)

- [ ] **HA1 [code] Watch persistence.** Voice-started watches live in
  `hass.data` and die on restart — the one spoken promise the code can't keep
  (`custom_components/securacv/intent.py`, storage note). Use a
  `homeassistant.helpers.storage.Store`.
- [ ] **HA2 [code] Wire entity translations.** `strings.json` declares 9
  entity keys but no entity sets `_attr_translation_key` — the translations
  are dead and entity names are hardcoded English. Wire them, extend coverage
  to the other ~21 entity classes, then translations beyond `en.json` become
  possible.
- [ ] **HA3 [code] Stop swallowing malformed MQTT payloads silently.** Two
  `except TypeError: pass` sites in `binary_sensor.py` (~:664, ~:961) — add
  debug-level logs so a firmware field-type regression is visible.
- [ ] **HA4 [code] Timeline card: say when history is unavailable.**
  `www/securacv-timeline-card.js` falls back to current-state rows when the
  recorder is off, looking like "nothing happened". Render an explicit notice.
- [ ] **HA5 [decision] Service surface.** The integration registers zero
  services — everything is intents + options flow. Decide whether
  pin/rotate/unpin/start-watch should also be services for automations.
- [ ] **HA6 [code] Guard `FUTURE_TRANSPORTS`/`FUTURE_TAMPER_TYPES` in-package.**
  Today only `scripts/lint_feature_flags.sh` keeps them out of the advertised
  sets; add a unit test beside the constants.
- [ ] **HA7 [code] Give `integrations/ha_frigate_mqtt/TASKS.md` a header**
  saying whether it is a tick-as-you-go operator runbook or an unexecuted
  plan; 26 unchecked boxes are currently ambiguous. If runbook: retitle
  RUNBOOK.md.
- [ ] **HA8 [human-gated by U1] Ship a working example camera** for the
  Frigate config once a real camera is on the bench (today: one disabled
  example so the config parses).
- [ ] *(Mirror repo itself: no code work — it is byte-identical today. Its
  health items are U6 and U7 above, plus the three monorepo-fixture tests its
  CI deselects, which is by design.)*

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
- [ ] **W3 [code] Showroom AR button is hardcoded to the Doorbell**
  (`showroom.html`, `data-ar-model`). Make it follow the selected product —
  the plan is already written at `docs/render-roadmap.md` "view what's on
  screen AR" (mind the mm→m warning there).
- [ ] **W4 [code] Dash and Combo have live Showroom products but no `.glb`**
  (no AR, no view-in-room). Add generators on the shared `scripts/lib/glb.mjs`
  builder; envelopes come from the carried CAD ledger, never hand-typed.
- [ ] **W5 [code] Blender hero bake** — all 5 shipped GLBs are procedural
  stand-ins; `blender/bake_hero.py` exists but has never produced output, and
  the AO bake has "one manual step" left (`blender/README.md`).
- [ ] **W6 [code, paired with U4/U5 work] Supplier quotes are placeholders** —
  5 rows in `suppliers.json` marked `"status": "placeholder"`. When real RFQs
  land, also invert `tests/quote-compare.edge.test.mjs` ("honesty flag" test
  currently asserts placeholders exist — it will fail the moment they don't).
- [ ] **W7 [code] Claims-audit remediation** —
  `docs/claims-audit-2026-07.md` open items (soften "no one can forge" claims,
  fiscal-host, "All rights reserved" drop, illustrative-token label).
- [ ] **W8 [code] IA deferred editorial pass** — `docs/ia-review-2026-07.md`
  four items (homepage reduction, one-owner-per-message trim, sitewide
  availability vocabulary — the last "belongs with a store-opening pass", so
  pair with U5).
- [ ] **W9 [code] `render-roadmap.md` quality items** (spot-varnish masks,
  roughness variation, WebGPU cinematic mode) — nice-to-have, after W3–W5.
- [ ] **W10 [human] Tizen TV package has `REPLACEME` app IDs**
  (`tv/tizen/config.xml`) — needs Samsung Seller Office issuance before any
  submission.
- [ ] **W11 [code] Advisory Claude review workflow is inert** if
  `ANTHROPIC_API_KEY` is unset (`.github/workflows/claude-review.yml` skips
  green) — either set the secret (human) or note the intended state in the
  workflow header.

---

## 5. Hardware, CAD, print files (monorepo)

The canonical ledger is `docs/hardware/enclosure/AUDIT_2026_09.md` ("Open —
major, by theme") — work its themes, then tick here.

- [ ] **C1 [code] Parametric-UX debt, cheapest first:** 174 of 1704 Customizer
  parameters have help text that is written but mechanically discarded —
  restore it; then chip at the 705 with none, the 14 names for the two-stud
  interface group, and presets for the released cases (audit §"Parametric UX").
- [ ] **C2 [code] Naming collisions across case files** (`usb_w`, `vm_*`,
  `skirt_t`, `clip_w` mean different things in different files) — audit's
  "transferred skill actively wrong" theme.
- [ ] **C3 [code] Lid-rib proportions + hardware counts** — audit themes;
  ribs need a per-case clearance probe, and nothing in the catalog counts
  hardware (the one header BOM already drifted).
- [ ] **C4 [decision] The C6 `brass_h` disagreement** — registry says 5.0, the
  file prints the C3's measured 3.0; `devices/README.md` says owning either
  number would bless it. A maintainer measures and decides.
- [ ] **C5 [code] Draw the Watch Station figure from its parts** — today it
  is a hand-authored sketch in `canary-local/tools/figures/massing.mjs`, so
  the first Watch knob edit desyncs the website's carried copy.
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
- [ ] **C8 [code] Kiri:Moto slicer is not vendored** — the Enclosure Lab's
  slice button degrades to a model-based estimate
  (`canary-local/assets/enclosure-lab.js`); vendor the engine or keep the
  honest note deliberately (then mark this item closed-as-intended).
- [ ] **C9 [code] Nightstand-line 3D figures reuse the Dash mesh**
  (`canary-local/assets/scene3d.js` "dedicated meshes are follow-up work").

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
- [ ] **D4 [code+decision] Carry the Watch Station and Dash assembly steps
  into the catalog README.** `canary-local/devices/assembly.json` authors
  their steps with every `readmeStep: null` ("not yet in the catalog
  README"). Carrying them is a three-part join: author the `## Assembly`
  steps in the two devices' `docs/hardware/enclosure/README.md` sections,
  carry them into `build.json`'s readme-step list, and point each
  `steps[].readmeStep` at its row (`canary-local/tests/assembly.test.js`
  gates the join). Both designs are in-development and not print-validated,
  so decide first whether the README steps land now with a dev caveat or
  wait for print validation (C7).

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
| `docs/audit/hardware_verification_checklist.md` | The 23 hardware verification cases (U1) |
| `docs/hardware/enclosure/AUDIT_2026_09.md` | Enclosure open themes (C1–C3) |
| `docs/RELEASE_BUTTONS.md` | Release/ship operations (U2) |
| `securacv_website/docs/render-roadmap.md` | 3D/AR quality roadmap (W3, W9) |
| `securacv_website/docs/roadmap.md` | Website roadmap (stale — W2) |

**Provenance.** Findings confirmed by source inspection during the
2026-09-20 sweep (five parallel
audits over the three repos; classic TODO-marker counts were near zero — this
project records debt as honest-status prose and checklists, which is what this
file indexes). Items listed here were spot-checked against source at sweep
time; re-read the source before building on a line.
