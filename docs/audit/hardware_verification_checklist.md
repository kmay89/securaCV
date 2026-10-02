# Hardware verification checklist — Opera mesh + Chirp + Beacon

Status: Operator checklist, 2026-05-12
Audit: `docs/audit/mesh_and_chirp_audit_v1.md` §7
Closeout: `docs/audit/v0.3_closeout.md`

Every item below requires physical XIAO-ESP32S3 boards on the bench
(or equivalent ESP32-S3 dev kit with flash encryption enabled in
production builds). Host-side and lint-side gates are already green;
this is what's still hardware-bound.

When you finish a repro, drop the artifact (terminal log, BLE console
capture, MQTT trace, photo of the device LED, etc.) into
`docs/audit/repro/<finding_id>/` and tick the corresponding box here.

---

## Chirp v0.2 — two-device repro of the critical findings

These confirm that the fixes from PR #450 actually behave the way the
host tests assert they do, on real radio.

- [ ] **C1 — spoofed witness rejected**
  - Setup: two boards on the same Chirp channel, both running v0.2 firmware.
  - Pre-fix repro (artifact only): flash a v0.1 binary to the receiver and
    have an attacker board send a `CHIRP_MSG_WITNESS` with random
    `signature[64]`. Receiver accepts and surfaces the chirp.
  - Post-fix expected: with v0.2 firmware, the receiver drops the frame
    and logs `chirp: rejected witness — bad top-level signature`.
  - Artifact: `docs/audit/repro/C1/`.

- [ ] **C2 + C3 — inflated `confirm_count` ignored**
  - Setup: two boards on Chirp, no pairing.
  - Repro: attacker sends a v0.1-style witness with
    `payload->confirm_count = 99`. (Use a packet-crafter; see the wire
    format in `spec/chirp_channel_v0.md` §3.3.)
  - Post-fix expected: receiver accepts the witness (signature valid)
    but treats `confirm_count` as 0 locally; the chirp does not become
    `validated` until a real `CHIRP_MSG_ACK` arrives from a different
    `session_pubkey`.
  - Artifact: `docs/audit/repro/C2_C3/`.

- [ ] **C4 — relayed witness preserves origin signature end-to-end**
  - Setup: three boards: originator A, relayer B, verifier C, all
    within 1 hop range of each other.
  - Repro: A originates; B relays (`hop_count = 1`); C receives and
    verifies. Confirm that C accepts the relay AND that C's view of
    the chirp records `has_origin_signature = true` (visible via
    `GET /api/chirp/recent` if `chirp_api` is enabled, or via BLE
    console).
  - Post-fix expected: end-to-end origin attestation works across
    relay; downstream verifiers don't have to trust the relayer alone.
  - Artifact: `docs/audit/repro/C4/`.

- [ ] **C5 — ACK pubkey dedup**
  - Setup: three boards. Originator A sends a chirp. Confirmer B sends
    an ACK. Replay B's exact ACK frame from a different MAC.
  - Post-fix expected: receiver counts only one confirmation, not two,
    because both ACKs carry the same `confirmer_session_pubkey`.
  - Artifact: `docs/audit/repro/C5/`.

- [ ] **C9 — Bloom filter survives 1000-nonce flood**
  - Setup: two boards, attacker rapidly sends 1000 distinct-nonce
    frames (mostly with bad signatures so they're dropped at verify,
    but valid magic + version so they hit the dedup path first).
  - Post-fix expected: a legitimate chirp sent immediately afterward
    is still delivered (Bloom FPR ~0.4%, the legitimate nonce is
    not in the filter), and the receiver's free heap is unchanged
    (no malloc churn).
  - Artifact: `docs/audit/repro/C9/`.

- [ ] **C7 — community suppress vote**
  - Setup: 5 boards, all Chirp-active and in range.
  - Repro: board 1 originates a low-quality chirp. Boards 2-4 dismiss
    it within 2 minutes (calls `dismiss_chirp` which emits
    `CHIRP_MSG_SUPPRESS_VOTE`). Verify board 5 transitions the chirp
    to `suppressed = true` once it counts ≥3 unique-pubkey suppress
    votes within `SUPPRESS_WINDOW_MS` (120 s).
  - Artifact: `docs/audit/repro/C7/`.

- [ ] **Cross-reboot replay**
  - Setup: two boards, recorder + replayer.
  - Repro: capture a valid chirp frame; reboot the receiver; replay
    the frame.
  - Post-fix expected: with `time(nullptr) >= MIN_UNIX_TIME` (SNTP
    synced before replay), the frame is rejected as "outside
    freshness window" because the original timestamp is older than
    the 5-minute window. With time unsynced, frame is accepted but
    flagged `unverifiable_timestamp = true`.
  - Artifact: `docs/audit/repro/replay/`.

## Opera mesh v0.2 — three-board repro

- [ ] **O1 — counter freshness across uptimes**
  - Setup: two boards, one freshly booted (uptime ~10 s), one
    booted ≥1 hour ago.
  - Repro: have each exchange heartbeats.
  - Post-fix expected: both accept each other's frames (counter is
    the freshness mechanism; uptime difference no longer asymmetric
    rejects). Pre-v0.2 the freshly-booted device would reject the
    long-running peer's frames.
  - Artifact: `docs/audit/repro/O1/`.

- [ ] **O2 — provisioning refused when FE off**
  - Setup: one ESP32-S3 board with flash encryption explicitly NOT
    enabled (dev build).
  - Repro: attempt to pair the device into an Opera.
  - Post-fix expected: pairing flow fails; health log records
    `opera: refused to persist secret — flash encryption disabled
    (audit O2)`. NVS does not contain an opera_secret entry.
  - Artifact: `docs/audit/repro/O2/`.

- [ ] **K1 — identity-key posture is reported, and only the opt-in image refuses**
  - Setup: one ESP32-S3 board with flash encryption NOT enabled, one with
    it enabled (dev mode); the default `canary` image (`pio run -e release`)
    and an opt-in image built from the same env with
    `PLATFORMIO_BUILD_FLAGS=-DSECURACV_REQUIRE_FLASH_ENCRYPTION=1 pio run -e release`.
    The provisioning kit's `[env:secure]`
    (`firmware/provisioning/platformio_secure.ini`) sets the same flag and,
    since F42, compiles in CI (compile-only), but it is a different image —
    `partitions_secure.csv`, CSI off — so this row stays on `release`. On a
    fused board, `pio run -e secure` is also the check that its `nvs`
    partition now opens (F42 dropped the `encrypted` flag that made IDF
    refuse it): boot it and expect the opt-in refusal lines below, not an
    NVS open failure.
  - Repro: boot each combination; press `f` on the console; `GET /api/status`.
  - Expected:
    - default image, FE-off board: boot log carries
      `[WARN] Key at rest : plaintext-nvs - identity key at rest in plaintext NVS (Tier 0 default; ...)`,
      the `f` card shows `KeyAtRest : plaintext-nvs`, `/api/status` and the
      `j` manifest carry `"key_at_rest":"plaintext-nvs"`; provisioning
      succeeds.
    - default image, FE-on board: the `f` card's `FlashEnc` line reads
      `ENABLED`, but the three surfaces STILL read `plaintext-nvs` and the
      boot line is still `[WARN]`, now with `flash encryption is on but does
      not cover NVS, and NVS encryption is not active`. Flash encryption
      does not encrypt NVS and this build has no NVS encryption, so the key
      is readable from a flash dump; a board reporting `nvs-encrypted` here
      is a FAIL. Optional confirmation: read the `nvs` partition back with
      `esptool.py read_flash 0x9000 0x5000` and find the `privkey` entry in
      the clear.
    - opt-in image, FE-off board: provisioning HALTS. The log carries
      `[!!] identity key not loaded: flash encryption required by this
      build but not active` (the load is refused before NVS is read, so it
      prints whether or not a default image had already stored a key), then
      `[!!] identity key not stored: ...` with the same reason, then
      `Device provisioning failed`; NVS never gains a new `privkey` entry and
      an existing one is left as it was.
    - opt-in image, FE-on board: provisioning ALSO halts, the same two
      lines with the reason `encrypted NVS required by this build but NVS
      encryption is not active (flash encryption alone does not cover NVS)`.
      Under `framework = arduino` the opt-in image refuses on every board —
      that is the policy, not a fault.
    - fresh-unit keygen vs the battery ADC (R18a): on a board with the
      battery divider fitted and `FEATURE_POWER_MONITOR` on, erase NVS, boot
      the default image once (first-boot keygen runs
      `bootloader_random_enable()`/`_disable()` AFTER `power_start()` opened
      the ADC), note the battery mV from the `b` console card (or
      `current.voltage_mv` in `GET /api/battery/history`), then reboot
      without erasing and note it again: the two readings agree
      within normal ADC noise. A zero, pinned or wildly different first
      reading is a FAIL (the disable powered down / reset the ADC under the
      power monitor).
  - Policy under test: `firmware/common/identity/key_at_rest.h`
    (host-tested by `firmware/tests_host/test_key_at_rest.cpp`).
  - Artifact: `docs/audit/repro/K1/`.

- [ ] **K2 — chain head survives a power cut between record and persist**
  - Setup: one board on the PIO `canary` image, NO SD card (so SD-wins cannot
    mask the result); a bench supply you can cut.
  - Repro: note `chain_seq` + `chain_head` from `/api/status`; trigger a
    witness record; cut power inside the persist window (repeat ~20 times —
    the window is one NVS blob write, so most cuts land before or after it).
  - Expected: on every next boot `/api/status` reports EITHER the previous
    {`chain_seq`, `chain_head`} pair OR the new one — never the new seq with
    the old head or vice versa; the boot log carries no `[CHAIN]` mismatch
    and the `t` self-test chain verify passes. A board upgraded from a
    pre-blob image boots with its old seq/head (legacy fallback), and the
    first persist moves it to the blob; `chain_st` appears in NVS, `seq` /
    `chain` are left as they were. Re-upgrade: downgrade that board to the
    pre-blob image, create records until its `chain_seq` passes the blob's,
    then flash this image again — the boot log carries
    `[WARN] Chain: legacy seq N is ahead of chain_st seq M` and
    `/api/status` resumes at the older image's seq N, not at M.
  - Codec + source order under test: `firmware/common/witness/chain_state.h`
    (host-tested by `firmware/tests_host/test_chain_state.cpp`).
  - Artifact: `docs/audit/repro/K2/`.

- [ ] **O3 — transactional rekey on peer removal**
  - Setup: three Opera-member boards (A, B, C); A is the initiator.
  - Repro: from A, call `remove_peer(B.fingerprint)` via REST.
  - Post-fix expected:
    - A generates a new `opera_secret`.
    - A sends `MSG_OPERA_REKEY` to C, encrypted under their existing
      session key.
    - C decrypts, installs new secret, ACKs under the OLD opera_id.
    - A receives ACK, commits the new secret to NVS.
    - B (now isolated) attempts to rejoin; A and C reject because B's
      view of opera_id is stale.
  - Edge case: kill C before it can ACK. A waits 60 s, finalizes anyway,
    marks C `PEER_STALE`. C re-pairs through normal flow on next reboot.
  - Artifact: `docs/audit/repro/O3/`.

## Beacon channel v0 — three-board repro

**Blocked until two code items land** (see "Still open" in
`docs/audit/mesh_and_chirp_audit_v1.md` §9.1): the §3.3 pairing flow is a
stub, so no board can be "paired into the same beacon set" yet, and the
channel's runtime is not wired into the sketch loop, so no board receives
or ticks Beacon frames. Every row below assumes both.

- [ ] **Two-pubkey origination, happy path**
  - Setup: three boards paired into the same beacon set (A, B, C).
    `FEATURE_BEACON_CHANNEL` enabled in the build.
  - Repro: User on A calls `POST /api/beacon/originate` for
    `BCN_EMERG_FIRE_VISIBLE`. UI on B receives the cosign prompt;
    User on B confirms within 60 s.
  - Post-fix expected: A emits dual-signed `BEACON_MSG_ALERT` at
    `hop_count = 0`. C verifies both signatures, transitions to
    `BEACON_STATE_ALARM`, plays `PATTERN_BEACON` (1200/1700/2200 Hz
    sequence). HA `sensor.canary_<C>_beacon_state` flips to `Alarm`.
  - Artifact: `docs/audit/repro/beacon/happy_path/`.

- [ ] **CANCEL propagates (and the originator adopts its own frames)**
  - Setup: continue from the happy path — A, B and C all in
    `BEACON_STATE_ALARM` for A's alert.
  - Check first: A itself shows `Alarm` (`GET /api/beacon` on A,
    HA `sensor.canary_<A>_beacon_state`) — the originator adopts its own
    ALERT at hop 0; before the CANCEL pass it stayed `Normal`. B, the
    cosigner, shows `Alarm` too — it resolves its own fingerprint to its own
    key (spec §7.1 step 5); before the review follow-up it dropped the frame
    it had co-signed, stayed `Normal`, and refused the CANCEL below.
  - Repro: on A, `POST /api/beacon/cancel` with `{"reason":"false_alarm"}`.
    B's UI shows the cosign prompt for the all-clear; B confirms.
  - Expected: A emits a dual-signed `BEACON_MSG_CANCEL`
    (`template_id = 0x82`, header `msg_type = 2`) naming the alarm's nonce.
    A, B and C each move to `BEACON_STATE_SUPERVISORY`; each audit log
    gains the CANCEL (A's at `hop_count = 0`).
  - Two-device variant: with only A and B paired (each in the other's set,
    no C), the same CANCEL completes — B is A's only candidate, B holds the
    alarm, B confirms, and both leave `Alarm`.
  - Negative: on a fourth board D that never received the ALERT, a
    COSIGN_REQ for that CANCEL is refused before its user is asked (health
    log: "COSIGN_REQ refused").
  - Solo variant: a single board holding a solo alarm, BOOT held,
    `POST /api/beacon/cancel-solo` → a receiver with the board in its set
    leaves `Alarm`. `POST /api/beacon/silence` on any board changes only that
    board.
  - Artifact: `docs/audit/repro/beacon/cancel_propagates/`.

- [ ] **Single-signature reject**
  - Setup: same as above.
  - Repro: craft a `BEACON_MSG_ALERT` frame with only `sig_originator`
    populated and `sig_cosigner` zeroed.
  - Post-fix expected: receiver rejects at the signature-verify gate;
    no state transition.
  - Artifact: `docs/audit/repro/beacon/single_sig_reject/`.

- [ ] **Supervised-health Trouble on missing self-test**
  - Setup: three boards. Stop the self-test heartbeat on one
    (`AT command` or pull power).
  - Repro: wait 36 hours.
  - Post-fix expected: the other two boards transition from
    `BEACON_STATE_NORMAL` to `BEACON_STATE_TROUBLE` with
    `BCN_TROUBLE_NEIGHBOR_SELFTEST_GAP` in the trouble mask. HA
    sensor reflects the change.
  - Artifact: `docs/audit/repro/beacon/selftest_trouble/`.

- [ ] **X25519 keypair persistence across reboot**
  - Setup: two paired boards (A, B). FE enabled.
  - Repro: note A's `fingerprint` in `GET /api/beacon/set` on B (the
    route lists fingerprints, names and trust levels — not X25519 keys, and
    it should not grow one for this check). Complete one COSIGN_REQ→RESP
    exchange (A originates, B confirms). Reboot A. Originate again from A.
  - Post-fix expected: A's fingerprint on B is unchanged, and the
    post-reboot COSIGN_REQ→RESP exchange completes without re-pairing — B
    can only decrypt A's request if A kept the X25519 keypair B stored at
    pairing.
  - Pre-fix (v0.3 before PR #454): the pubkey would change every
    reboot, breaking cosign decrypt at B.
  - Artifact: `docs/audit/repro/beacon/x25519_persistence/`.

- [ ] **Solo origination — BOOT button held (v0.4)**
  - Setup: one board, FE enabled, Beacon enabled. No paired
    beacon-set neighbor (so the dual-pubkey path is unavailable).
  - Repro: hold the BOOT button down; `POST /api/beacon/originate-solo`
    with `template_id=0x20` (`BCN_EMERG_FIRE_VISIBLE`) and
    `severity="Severe"`. While still holding BOOT.
  - Post-fix expected: device emits `BEACON_MSG_ALERT` with
    `flags & BCN_FLAG_SOLO_ORIGIN`, `certainty=Observed`,
    `originator_fp == cosigner_fp`. A second (receiver) board with
    the originator in its beacon set accepts the frame and surfaces
    a "solo origination" badge in the HA `beacon_active_template`
    sensor.
  - Artifact: `docs/audit/repro/beacon/solo_happy/`.

- [ ] **Solo origination — BOOT button NOT held (v0.4)**
  - Setup: same as above.
  - Repro: release the BOOT button before calling `originate-solo`.
  - Post-fix expected: `POST /api/beacon/originate-solo` returns
    400 with `{"error":"boot_button_not_held"}`. No frame is
    broadcast (verify with a packet capture or by checking the
    receiver board's audit log is unchanged).
  - Artifact: `docs/audit/repro/beacon/solo_no_button/`.

- [ ] **Solo origination — certainty=Likely tampering rejected (v0.4)**
  - Setup: one originator board + one receiver board.
  - Repro: craft a `BEACON_MSG_ALERT` with `BCN_FLAG_SOLO_ORIGIN`
    set but `certainty=Likely` (not Observed). Send to receiver.
  - Post-fix expected: receiver drops the frame and logs
    `beacon: rejected solo frame — certainty != Observed`.
    Receiver's `beacon_active_template` does not change.
  - Artifact: `docs/audit/repro/beacon/solo_certainty_tamper/`.

- [ ] **Auto-revoke on tamper (v0.5)**
  - Setup: three boards. A and B paired into the same Opera mesh AND
    paired into each other's beacon set. C is the test stimulus.
  - Repro: open A's enclosure (or whatever triggers its tamper sensor).
    A broadcasts `MSG_TAMPER_ALERT`. B receives it.
  - Post-fix expected: B's `beacon_channel::on_peer_tampered(A.pubkey)`
    fires from `handle_tamper_alert`. Verify via
    `GET /api/beacon/set` on B that A's entry now shows
    `trust_level: "revoked"`. Verify that any subsequent Beacon frame
    from A (originated as a happy-path test) is dropped at the trust
    check on B with log line
    `beacon: paired neighbor revoked on tamper alert (v0.5 auto-revoke)`.
  - Recovery: physically inspect A; if the tamper was malicious, replace
    or reflash; if it was operator error (opened for maintenance),
    re-pair A through the standard pairing flow.
  - Artifact: `docs/audit/repro/beacon/auto_revoke/`.

- [ ] **Audible self-test cadence (NFPA 72 §14, v0.5)**
  - Setup: one board with passive buzzer on the chirp GPIO. Audio
    capture device. Set the system clock forward 30 days (`date -s` or
    a flashed NVS field) to fast-forward the self-test schedule.
  - Repro: let the device run; observe.
  - Post-fix expected: a single 1500 Hz, 80 ms beep plays once per
    30 days, only during 06:00–22:00 local time. The health log
    records `self-test chirp played (NFPA-72 supervised)` each
    occurrence. Spectrogram confirms NO reserved emergency-broadcast
    frequencies are touched at any point.
  - Negative cases to verify:
    - Set clock to 02:00 local; advance 30 days. No chirp plays
      (night-mode suppression). After 06:00, chirp plays on next loop.
    - Disable SNTP; ensure no chirp plays (unsynced suppression).
    - Trigger an active Beacon alarm and then advance 30 days. The
      self-test is suppressed during alarm state (don't compete with
      an emergency).
  - Artifact: `docs/audit/repro/beacon/selftest_cadence/`.

- [ ] **Audit log persistence across reboot + rotation**
  - Setup: one board, FE enabled.
  - Repro: emit ≥65 Beacon ALERTs (one above `AUDIT_LOG_MAX`),
    capture the audit log via `GET /api/beacon/audit?offset=0&limit=32`,
    reboot, capture again.
  - Post-fix expected: the most recent 64 entries are present in both
    captures, in oldest-first order. The pre-rotation log on reload
    matches the in-RAM state byte-for-byte. Chain-hash continuity
    holds.
  - Pre-fix: the on-disk content would be inconsistent with RAM after
    rotation because the `memmove` shuffled in-memory entries but only
    one slot was persisted.
  - Artifact: `docs/audit/repro/beacon/audit_persistence/`.

## Non-impersonation contract — on-device verification

- [ ] **Buzzer pattern frequency confirmation**
  - Setup: one board with a passive buzzer connected to the chirp
    GPIO. Audio capture device.
  - Repro: trigger `PATTERN_BEACON` via `POST /api/beacon/selftest`
    or by entering `BEACON_STATE_ALARM`.
  - Post-fix expected: spectrogram shows three sequential tones at
    1200 / 1700 / 2200 Hz, total duration ≤600 ms. **NO** tones at
    853 Hz or 960 Hz at any point. **NO** 8-second sustained
    attention signal of any kind.
  - Artifact: `docs/audit/repro/non_impersonation/audio_capture.wav`
    + spectrogram screenshot.

## MQTT/HA discovery — on-deployment verification

- [ ] **`chirp.state` + `beacon.state` discoverable in HA**
  - Setup: one board + MQTT broker + Home Assistant instance.
  - Repro: pair the device, wait one 30 s publish cycle.
  - Post-fix expected: HA's Settings → Devices → SecuraCV Canary
    shows the four new sensors:
    - `sensor.canary_<id>_chirp_state` (Normal/Trouble/Alarm/Supervisory)
    - `sensor.canary_<id>_beacon_state` (Normal/Trouble/Alarm/Supervisory)
    - `sensor.canary_<id>_beacon_airtime_pct` (%)
    - `sensor.canary_<id>_beacon_active_template` (string)
  - Artifact: `docs/audit/repro/ha/screenshots/`.

- [ ] **Alarm triggers HA automation**
  - Setup: HA automation: `state_changes -> sensor.canary_<id>_beacon_state
    becomes "Alarm"`.
  - Repro: trigger a beacon alarm via the happy-path test above.
  - Post-fix expected: HA automation fires within one 30 s publish cycle.

## Canary base SD event log + reconnect backfill (F37) — on-device verification

Code: `firmware/canary/src/csi_event_egress.cpp` over the loop-task adapter
`src/csi_event_log.cpp`; the rules are
`firmware/common/csi/src/csi_event_backfill.h` (host-tested against a model
of Home Assistant's replay gate). Owner: U1.

Since sweep F46 every row, the bundled ones (presence, and
`system.integrity` tampers, which carry a state) included, takes its event
id when it commits, from one allocator that starts at 0xC0000000, so the
log is in id order and ids keep rising across reboots. On firmware from
before F46, bundled rows took ids from the bundler's own space (0x80000000
upward, restarting every boot) and HA refused many of them, and the
chokepoint rows after them, live. A `replay` verdict on any row from this
firmware, live or backfilled, is now a finding.

- [ ] **An outage longer than the offline queue arrives whole**
  - Setup: an HA-enabled canary image (`release_ha`) with a card in,
    paired to Home Assistant; the MQTT broker on a host you can stop.
  - Repro: stop the broker; commit more than 12 events (presence changes
    in front of the sensor); restart the broker. Since sweep F81 a presence
    row commits when its bundle closes, two minutes after the state's last
    refresh or ten minutes after it opened, and `core.presence` opens at
    most six bundles in each of its ceiling's hours (six 10-minute buckets,
    sweep F132), so keep the broker down for a few hours, or count the
    other modules' rows too (the serial log's commit lines).
  - Expected: the serial log shows `[EVT-LOG] /EVENTS/today.ndjson open`
    at boot and `[CSI] event backfill done: N event(s) from the card`
    after the reconnect; HA's event history holds the outage's rows in id
    order, well past the 12 the offline queue could hold; no backfilled
    body gets a `replay` verdict; and the bodies committed while the broker
    was down carry `"replay":true`.
  - Artifact: `docs/audit/repro/F37/outage/`.
- [ ] **A reboot inside the outage republishes nothing**
  - Setup: as above.
  - Repro: stop the broker, commit several events, power-cycle the canary,
    commit a few more, restart the broker.
  - Expected: the pre-reboot backlog arrives in id order with no `replay`
    verdict on any backfilled body (at most ten of its rows may be missing
    — the NVS ceiling's stride). The post-reboot rows, presence included,
    have higher ids and arrive after it, none refused. A presence bundle
    still open at the power cycle never commits (bundles live in RAM until
    they close, sweep F81): that state is missing, not refused.
  - Artifact: `docs/audit/repro/F37/reboot/`.
- [ ] **Another device's card is left alone**
  - Setup: a card taken from a canary-wap (or another canary).
  - Expected: the health log says `SD event log belongs to another device -
    not used`; the card's `/EVENTS` is unchanged afterwards; events still
    publish live.
  - Artifact: `docs/audit/repro/F37/foreign-card/`.
- [ ] **A canary's card in a canary-wap is left alone**
  - Setup: a card the canary base has written its event log to (it holds
    `/EVENTS/owner`); a canary-wap on firmware carrying the owner rule.
  - Expected: the canary-wap's serial log says `[EVT-LOG] /EVENTS belongs to
    a canary base (owner file) - event log off for this card`; after events
    commit on the canary-wap, the card's `/EVENTS` is unchanged; back in the
    canary, the backfill sends none of the canary-wap's rows.
  - Artifact: `docs/audit/repro/F37/canary-card-in-wap/`.

## canary-wap event egress (F78) — on-device verification

Code: `firmware/projects/canary-wap/arduino/canary_wap/csi_event_egress.cpp`
(`csi_event_backfill.h`'s planner, a commit queue, a RAM hold for rows the
card does not keep) over `csi_event_log.cpp`; `csi_mqtt::loop()` pumps it on
the loop task. Host-tested
(`firmware/projects/canary-wap/tests_host/test_wap_event_egress.cpp`) and held
by `firmware/scripts/check_wap_event_egress.py`; the NimBLE host task
committing while the loop task backfills is not something a host test can
run. Compile is CI's. Owner: U1.

- [ ] **A row committed in the reconnect window waits behind the backlog**
  - Setup: a canary-wap with a card in, paired to Home Assistant; the MQTT
    broker on a host you can stop.
  - Repro: stop the broker; commit several events (walk in front of the
    sensor, so presence bundles close too); start the broker and keep
    walking while it reconnects.
  - Expected: the serial log shows `[EVT] event backfill done: N event(s)
    from the card`; HA's event history holds the outage's rows and the ones
    committed during the reconnect, each once, in id order; HA shows no
    `replay` verdict; no `[EVT] egress queue full` line.
  - Artifact: `docs/audit/repro/F78/reconnect-window/`.
- [ ] **A ble.scout close during the backfill stays in order**
  - Setup: as above, with a paired Scout beacon.
  - Repro: stop the broker, commit a backlog, start the broker, and take the
    beacon out of range while the backlog drains (a departure commits on the
    NimBLE host task).
  - Expected: the departure arrives after the backlog, once; no `replay`
    verdict; no watchdog or stack fault on the NimBLE host task.
  - Artifact: `docs/audit/repro/F78/scout-close/`.
- [ ] **A reboot during an outage keeps the card's backlog owed**
  - Setup: a canary-wap with a card in, paired to Home Assistant.
  - Repro: stop the broker; commit several events; power-cycle the
    canary-wap; start the broker once it is back up.
  - Expected: HA receives every row of the outage once, in id order, with
    `"replay":true`, and no `replay` verdict. (Before F78 every commit
    during the outage wrote the delivery ceiling past the backlog, and
    this sent nothing.)
  - Artifact: `docs/audit/repro/F78/reboot-in-outage/`.
- [ ] **A short outage with no card loses no row**
  - Setup: no card in; paired to HA; up more than 45 s since boot (until
    then rows wait for a card that may still mount).
  - Repro: stop the broker; commit up to eight events (presence, a tamper);
    start the broker.
  - Expected: all of them arrive in id order with `"replay":true`; past
    eight, the serial log names how many the RAM hold dropped, oldest first.
    Ambient `wifi.channel_activity` rows from the outage are not held and
    do not arrive (they are live-UI only), and never push out an event.
  - Artifact: `docs/audit/repro/F78/no-card/`.
- [ ] **A card that mounts late, or remounts mid-backfill, keeps its
  backlog first**
  - Setup: a card in with rows still owed from an outage; paired to HA.
  - Repro: (a) power-cycle with a slow card (or one seated after boot) and
    commit a presence event before the card mounts; (b) during a backfill,
    force an SD error (a card that briefly loses contact) so the log closes
    and remounts, and commit an event meanwhile.
  - Expected: in both, HA receives the card's owed rows first, then the new
    event, each once, in id order, no `replay` verdict. If the card is not
    back within 45 s the serial log says `event log card not open after 45
    s` and the waiting events go out without it.
  - Artifact: `docs/audit/repro/F78/late-card/`.

## Canary base: rows the card cannot take (F103, F104) — on-device verification

Code: `firmware/canary/src/csi_event_egress.cpp` (a RAM hold of eight rows,
the canary-wap's 45 s card wait, `csi_event_backfill::kCardWaitMs`).
Host-tested on the real egress and SD adapter
(`firmware/tests_host/test_canary_event_egress.cpp`); the PlatformIO compile
is CI's. Owner: U1. The serial lines below are the egress's own.

- [ ] **A failed card append during an outage keeps the backlog owed**
  - Setup: an HA-enabled canary (`release_ha`) with a card in, paired to
    Home Assistant; the broker on a host you can stop; a card that starts
    refusing writes on cue (one filled to capacity, or a worn card known to
    fail writes).
  - Repro: stop the broker; commit several events; make the next append
    fail (fill the card) and commit one more; (a) start the broker; (b)
    repeat, but power-cycle the canary before starting the broker.
  - Expected: (a) HA receives the outage's card rows, then the row whose
    append failed, each once, in id order, no `replay` verdict; (b) the
    card rows arrive after the reboot, at most ten of the outage's first
    rows missing (the NVS ceiling's stride, as in F37's reboot row), and
    the failed row, held in RAM, is lost. Before F103 the failed row went
    first and HA never received the card's rows, reboot or not.
  - Artifact: `docs/audit/repro/F103/failed-append/`.
- [ ] **A card that leaves mid-backlog and comes back within 45 s**
  - Setup: as above, with an ordinary card.
  - Repro: stop the broker; commit a backlog; start the broker; while the
    backfill runs, pull the card and reseat it within 30 s (the storage
    manager's recheck remounts it), committing an event while it is out.
  - Expected: HA receives the whole backlog, then the event committed while
    the card was out, each once, in id order. If the card stays out past
    45 s the serial log says `[CSI] event log card not open after 45 s: N
    event(s) waiting in RAM go out`, the waiting event goes, and the rows
    left on the card are not sent when it returns.
  - Artifact: `docs/audit/repro/F104/card-out/`.
- [ ] **A card that mounts late at boot keeps its backlog first**
  - Setup: a card with rows still owed from an outage (stop the broker,
    commit, power off).
  - Repro: power on with the card out and the broker up; once an event
    commits (the serial log's commit lines; since sweep F81 a presence row
    commits two to ten minutes after its state began, so this needs a row
    from another module, or several tries), seat the card. The storage
    manager re-probes every 30 s, and the wait is 45 s from boot, so the
    event must commit early and the card go in at once.
  - Expected: HA receives the card's owed rows first, then the new event,
    each once, in id order.
  - Artifact: `docs/audit/repro/F104/late-card/`.
- [ ] **No card: the first 45 s after boot**
  - Setup: no card; paired to HA.
  - Repro: watch the serial log for an event that commits in the first
    45 s after boot (since sweep F81 a presence row commits when its bundle
    closes, two to ten minutes after the state began, so this may take a
    few boots, or a module whose rows commit at once).
  - Expected: it reaches HA about 45 s after boot (it waits for a card that
    may still mount); events after that go at once. An ambient
    `wifi.channel_activity` row from those 45 s does not arrive. Then
    press reset within 45 s of such a commit line: that event never
    arrives (it waited in RAM), while the tamper topic's boot verdict
    still does.
  - Artifact: `docs/audit/repro/F104/no-card/`.

## The egress's counters and the event-id warning (F109, F82) — on-device verification

Code: `firmware/canary/src/csi_event_egress.cpp` (`csi_event_egress_stats()`,
`csi_event_egress_id_space_low()`), carried by `main.cpp`'s
`mqtt_publish_health_update()` as `csi_event_egress` and
`event_id_space_low`; the canary-wap's `csi_mqtt::publish_health()` carries
`event_id_space_low` too. The flag is `csi_event_id_floor::space_low()` of
the allocator's next id. Host-tested (`test_canary_event_egress.cpp`,
`test_csi_event_id_floor.cpp`, the canary-wap's `test_mqtt_reinit.cpp`, and
`test_canary_health_trust.py` for the canary's worst-case packet against its
1664 B MQTT buffer); the compiles are CI's. Owner: U1.

- [ ] **The canary's health counts what its egress did**
  - Setup: an HA-enabled canary (`release_ha`) with a card in, paired to
    Home Assistant; `mosquitto_sub -v -t 'securacv/+/health'` on the broker
    host, which you can stop.
  - Repro: commit a few events with the broker up; stop the broker, commit
    a few more, start it again and wait for `[CSI] event backfill done`;
    then wait for the next health publish (once a minute).
  - Expected: the health body holds a `csi_event_egress` object whose
    `planner.live` counts the first rows, `planner.held` and
    `planner.replayed` the outage's, and whose `dropped`, `held_dropped` and
    `ambient_dropped` match any drop lines the serial log printed (0 when
    none). The publish arrives whole (no missing health while it is the
    largest yet).
  - Artifact: `docs/audit/repro/F109/health-counters/`.
- [ ] **Both devices warn before the event-id space runs out**
  - Setup: a canary and a canary-wap you can write NVS on (a bench unit
    whose Home Assistant entry you will re-pin afterwards).
  - Repro: write the `securacv` namespace's `csi.evid` to `4026531840`
    (0xF0000000) and reboot; watch the health topic. Then write it back
    below that and reboot again.
  - Expected: `"event_id_space_low":true` from the first health publish
    after the first reboot, and `false` after the second; the device's
    events after the first reboot carry ids at or above 4026531840.
  - Artifact: `docs/audit/repro/F82/id-space-low/`.

## canary-wap loop-task ownership (F96, F106) — on-device verification

Code: `firmware/projects/canary-wap/arduino/canary_wap/mesh_network.cpp`
(`submit()` posts a mesh owner command to a four-slot
`loop_command_ring.h`; `update()` drains it first on every pass; the
pre-reboot replay save is one more command from the httpd task) and
`csi_mqtt.cpp` (`request_reinit()`; `csi_mqtt::loop()` serves the re-init:
it detaches the old client, a one-shot `mqtt_retire` task stops and destroys
it, and a later pass opens the new one). The `handle_mesh_*` handlers, the
MQTT config and test handlers and the QR scanner hand their work to the loop
task instead of doing it on their own.
Host-tested (`tests_host/test_loop_command_ring.cpp`,
`test_mesh_commands_wap.cpp`, `test_mqtt_reinit.cpp`) and held by
`firmware/scripts/check_wap_loop_commands.py`; two real tasks on two cores
are not something a host test can run. Compile is CI's. Owner: U1.

- [ ] **Pairing, removal and the other mesh routes still answer as before**
  - Setup: two canary-wap boards on this firmware, each with the web UI
    open.
  - Repro: pair them from the web UI (Create Opera on one, Join on the
    other), confirming the code on the joiner first; rename the opera;
    clear the alerts; turn the mesh off and on again; remove the other
    board; leave the opera.
  - Expected: every step answers as it did before (`{"ok":true}`, or the
    same 400 errors for a refused step); no `mesh_busy` (409) or
    `mesh_timeout` (503) in normal use; the removal starts the rekey (the
    serial log's `opera: rekey transaction started after peer removal`).
  - Artifact: `docs/audit/repro/F96/rest-routes/`.
- [ ] **A busy loop answers `mesh_timeout`, and the command does not run**
  - Setup: one board, mesh on.
  - Repro: while the loop task is held (an SD card remount, or a debug
    build with a deliberate 3 s stall in `loop()`), send
    `POST /api/mesh/name` with a new name.
  - Expected: `503 {"ok":false,"error":"mesh_timeout"}` within about 2 s;
    after the stall, `GET /api/mesh` still shows the old name.
  - Artifact: `docs/audit/repro/F96/timeout/`.
- [ ] **Saving and testing the broker under publish load**
  - Setup: a board paired to Home Assistant's broker, events committing
    (walk in front of the sensor), the `/mqtt` page open.
  - Repro: press Save and Test connection repeatedly, about once a second
    for a minute, changing the topic prefix back and forth.
  - Expected: no reboot, Guru Meditation or task watchdog; the page shows
    `Saved.` and `Reached the broker (plain)` (or the TLS transport); HA
    keeps receiving events between the reconnects; the serial log shows one
    `[MQTT] bridge started` per re-init (presses that land while one waits
    share it).
  - Artifact: `docs/audit/repro/F106/save-under-load/`.
- [ ] **"Test & save" with an unreachable broker IP does not reboot the board**
  - Setup: one board with the companion page open; a broker address on the
    LAN that nothing answers (an unused IP, so the TCP connect times out
    rather than being refused).
  - Repro: enter that IP and press Test & save; while the first attempt is
    still connecting, press it again two or three times.
  - Expected: no `task_wdt` / Guru Meditation and no reboot (uptime keeps
    counting, and the rapid-reboot counter does not move); the config save
    answers within about 2 s and the test within about 4 s (`ok:false`);
    in the serial log each `[MQTT] bridge started` follows a
    `[MQTT] previous client stopped after N ms` line, where N can reach
    several seconds against that IP while the loop keeps running (the stop
    waits out the rest of a connect attempt, which the client's network
    timeout bounds at 2 s since F112, 10 s before, and then esp_mqtt's task
    noticing the stop, up to half its 10 s reconnect wait; from esp-mqtt's
    source). Then enter the real broker and press Test & save once:
    `Reached the broker`.
  - Artifact: `docs/audit/repro/F106/unreachable-broker/`.
- [ ] **A reboot from the dashboard still saves the mesh's replay counters**
  - Setup: two paired canary-wap boards exchanging heartbeats for a few
    minutes.
  - Repro: press Reboot on one (POST /api/reboot); repeat with the safe-mode
    Retry button if a board is in safe mode.
  - Expected: the board answers `Rebooting...` and restarts within about
    3 s; after it boots, the pair keeps exchanging frames (the peer stays
    `connected`), with no fault on either board.
  - Artifact: `docs/audit/repro/F96/reboot-save/`.
- [ ] **A QR hub provision still joins the fleet**
  - Setup: an unprovisioned canary-wap with a camera; a canary-display
    showing its provisioning QR with a hub.
  - Repro: scan the code.
  - Expected: the canary-wap joins Wi-Fi and comes up on the hub's broker
    (its `status` topic says online) without a reboot, and the display
    celebrates it.
  - Artifact: `docs/audit/repro/F106/qr-hub/`.

## canary-wap mesh status reads and membership (F110, F113, F116) — on-device verification

Code: `firmware/projects/canary-wap/arduino/canary_wap/mesh_network.cpp`
(`publish_view()` at the end of every `update()` pass and after each owner
command, `read_status()` and
`read_alerts()` for the status routes through `loop_snapshot.h`;
`persist_opera_config()` / `load_opera_config()` for a leave; `retire_rx()`
and the `rx_tombs` NVS record for a re-added member) and the three status
handlers in `canary_wap.ino`. Host-tested (`tests_host/test_loop_snapshot.cpp`,
`test_mesh_commands_wap.cpp`, `test_mesh_liveness_wap.cpp`) and held by
`firmware/scripts/check_wap_loop_commands.py`. Compile is CI's. Owner: U1.

- [ ] **The Opera page reads as before while the opera is busy**
  - Setup: two paired canary-wap boards, the web UI's Opera page open on
    one, the csi dashboard's Fleet sheet open on the other.
  - Repro: start a new pairing and watch the code appear; cancel it; remove
    and re-pair the other board; trigger a tamper alert on it a few times
    (open its case) and clear the alerts.
  - Expected: every page refresh answers (no `mesh_busy` or `mesh_timeout`
    on a GET), the code shows while it is on screen and is gone as soon as
    the cancel answers (not only at the next refresh), a removed member is
    gone from the list the page reloads right after the removal, the peer
    list and counts agree with each other on every refresh, and the alert
    list shows each alert whole.
  - Artifact: `docs/audit/repro/F110/status-routes/`.
- [ ] **A board that left its opera founds a new one after a reboot**
  - Setup: two paired boards.
  - Repro: on one, Leave the opera; reboot it; turn the mesh on, Create
    Opera and pair a third board (or the same one) with it.
  - Expected: after the reboot the Opera page shows no opera (`has_opera`
    false); the new pairing completes and both boards show each other
    connected within about a minute.
  - Artifact: `docs/audit/repro/F113/leave-reboot/`.
- [ ] **A board removed and re-paired is heard at once, and a reflashed one too**
  - Setup: two paired boards, A and B, exchanging heartbeats for a few
    minutes.
  - Repro: on A, remove B (its only member); wait out the 7-day deny-list
    grace (a debug build with a short grace, or leave A powered for 7
    days); re-pair A and B. Then erase B's flash, reflash it, and pair it
    with A again (removing B's old entry on A first). Then, with a third
    board C paired too, remove B while C stays (the opera rotates), wait
    out the grace and re-pair B; if a board on a release from before F71
    is at hand, repeat that last step with B on it.
  - Expected: after each re-pair B shows connected on A within about a
    minute, and A's serial log shows no `pairing COMPLETE never answered`.
  - Artifact: `docs/audit/repro/F116/re-pair/`.

## canary-wap Chirp and Bluetooth commands, MQTT network timeout (F111, F112) — on-device verification

Code: `firmware/projects/canary-wap/arduino/canary_wap/chirp_channel.cpp`
and `bluetooth_channel.cpp` (`submit()` posts an owner command to a
four-slot `loop_command_ring.h`; each channel's `update()` drains it first
on every pass), `chirp_api.h` and `bluetooth_api.h` (the POST handlers
submit and answer from the result; a Bluetooth handler that turns
Bluetooth on calls `init()` on its own task first), and `csi_mqtt.cpp`
(`open_client()` sets the client's `network.timeout_ms` to
`kNetworkTimeoutMs`, 2 s; the link is announced to the loop task only after
the CONNECTED handler's burst, which esp_mqtt sends under the client's API
lock). Host-tested (`tests_host/test_chirp_commands_wap.cpp`,
`test_bluetooth_commands_wap.cpp` over a NimBLE stand-in,
`test_mqtt_reinit.cpp` over a fake esp_mqtt) and held by
`firmware/scripts/check_wap_loop_commands.py`; the real radio stacks, two
tasks on two cores and a real stalled TCP link are not something a host
test can run. Compile is CI's. Owner: U1.

- [ ] **The Chirp routes still answer as before**
  - Setup: one canary-wap with the web UI open on Community > Chirp; a
    synced clock.
  - Repro: turn Chirp on and off and on again; wait ten minutes; send a
    template; send another at once; mute 30 minutes, unmute; untick
    "relay"; with a second board nearby on Chirp, confirm and dismiss one of
    its chirps.
  - Expected: each step answers as before (`success:true` with the session
    emoji on enable; the second send `cooldown` with its seconds; mute,
    unmute and settings `success:true`, settings showing `relay_enabled:
    false`); no `chirp_busy` (409) or `chirp_timeout` (503) in normal use.
  - Artifact: `docs/audit/repro/F111/chirp-routes/`.
- [ ] **Bluetooth pairing from the web UI, the PIN confirmed near the timeout**
  - Setup: one canary-wap; a phone with nRF Connect (or the companion app).
  - Repro: Bluetooth > Pair; connect from the phone; when the six digits
    show on both screens, confirm in the web UI (once promptly; once about
    58 s after pairing mode started, so the confirm meets the 60 s pairing
    timeout). Then Disconnect, Clear scan results, trust / block / remove
    the paired phone, rename the device, set TX power to -6, Disable and
    Enable.
  - Expected: no Guru Meditation, heap-poisoning abort or watchdog reset
    (the near-timeout confirm used to race the timeout's cancel); each
    route answers as before; the phone's bond is removed with the device.
  - Artifact: `docs/audit/repro/F111/bt-pairing/`.
- [ ] **Turning Bluetooth on before the bring-up still works, off the loop task**
  - Setup: a canary-wap just provisioned onto Wi-Fi (the BLE bring-up is
    deferred until the join window clears).
  - Repro: within that window, press Bluetooth > Enable (or Pair) in the
    web UI.
  - Expected: `Bluetooth enabled` (or the init-failed reason the stack
    gives) within a few seconds; no `task_wdt` on `loopTask` and no
    reboot (the stack comes up on the HTTP request's task, as before).
  - Artifact: `docs/audit/repro/F111/bt-early-enable/`.
- [ ] **A stalled broker link does not trip the loop watchdog**
  - Setup: a board connected to Home Assistant's broker, events committing.
  - Repro: block the broker's traffic without closing the TCP connection
    (an iptables DROP on the broker host for the board's IP, or pull the
    broker host's network cable), keep walking in front of the sensor for
    two minutes, then restore the link.
  - Expected: no `task_wdt` / Guru Meditation and no reboot; the serial log
    shows `[MQTT] disconnected (will retry)` once the link gives out: about
    2 s after the publish that finds the TCP send buffer full, or when the
    60 s keepalive goes unanswered, whichever comes first; HA shows the
    board unavailable (the broker publishes its will) until the reconnect.
    Events committed after the link failed reach HA after the reconnect
    (from the card or the RAM hold, per F78), except QoS 0 rows the board
    had already handed to the socket before the abort: those count as sent
    and may be missing. Count them (compare the board's event ids with HA's
    history for the window) and note the number.
  - Artifact: `docs/audit/repro/F112/stalled-broker/`.
- [ ] **A reconnect over a slow link does not trip the loop watchdog**
  - Setup: a board connected to Home Assistant's broker with discovery on;
    the broker host can shape the board's traffic (for example
    `tc qdisc add dev <if> root netem rate 16kbit` for its IP).
  - Repro: shape the link, restart the broker (or the board's Wi-Fi) so the
    board reconnects and sends its status, discovery configs and cached
    states over the slow link; keep walking in front of the sensor while it
    does; then remove the shaping.
  - Expected: no `task_wdt` / Guru Meditation and no reboot while
    `[MQTT] connected` is followed by the burst; the HA entities come back;
    events committed during the burst reach HA after it (the egress waits
    for the link the bridge announces once the burst is sent).
  - Artifact: `docs/audit/repro/F112/slow-reconnect/`.

## One event-id space (F46) — on-device verification

Code: `firmware/common/csi/src/csi_event.cpp` (one allocator, ids taken at
commit under a recursive commit lock), `csi_bundler.cpp` (open bundles carry
handles), `csi_event_id_floor.h` (`kIdSpaceBase`, `boot_floor`), the canary's
`src/csi_event_egress.cpp` and the canary-wap's `csi_integration.cpp` /
`csi_event_log.cpp`. Host-tested
(`firmware/tests_host/test_csi_event_id_space.cpp` on the real library,
`test_csi_event_backfill.cpp`, `test_csi_event_id_floor.cpp`) and held by
`firmware/scripts/check_csi_commit_order.py`; the commit lock across the
loop task and the NimBLE host task is not something a host test can run.
Owner: U1.

- [ ] **An upgraded device keeps reaching Home Assistant**
  - Setup: a canary (`release_ha`) and a canary-wap, each paired to Home
    Assistant on firmware from before F46, each having published a presence
    row (so HA's mark sits at 0x80000000 or above).
  - Repro: flash this firmware without erasing NVS; walk in front of each
    sensor, and let it commit a few other events too.
  - Expected: every body's `event_id` is 3221225472 (0xC0000000) or above,
    rising in arrival order; HA shows no `replay` verdict; nothing was reset
    in HA or on the device.
  - Artifact: `docs/audit/repro/F46/upgrade/`.
- [ ] **A bundle that closes after a reboot is accepted**
  - Setup: as above, on this firmware.
  - Repro: start presence, power-cycle mid-presence, let presence start and
    end again.
  - Expected: the post-reboot rows' ids are above every pre-reboot id; no
    `replay` verdict. The presence bundle open at the power cycle is never
    committed, on either device (sweep F81 made the canary bundle like the
    canary-wap).
  - Artifact: `docs/audit/repro/F46/reboot-bundle/`.
- [ ] **A Scout arrival and a loop-task commit at once stay in order**
  - Setup: a canary with a paired BLE Scout beacon and an open presence
    bundle near its quiet-gap close.
  - Repro: bring the beacon in range as the presence bundle closes, a few
    times; then repeat with the broker slowed or blocked (a firewall rule
    that drops its packets), so a publish holds the commit lock.
  - Expected: the `events` bodies' ids rise in publish order; no `replay`
    verdict; no watchdog reset and no BLE supervision drop (the commit lock
    is held across the commit hooks on both tasks, the canary-wap's MQTT
    publish included).
  - Artifact: `docs/audit/repro/F46/scout-race/`.
- [ ] **Open rows carry handles**
  - Setup: a canary-wap on this firmware.
  - Repro: `GET /api/events/today` while presence is ongoing.
  - Expected: the `"open":1` row's `id` is in [2147483648, 3221225472);
    the dashboard shows it as happening now, with no dismiss button.
  - Artifact: `docs/audit/repro/F46/open-row/`.

## Bundled presence rows and the hourly ceiling (F80, F81) — on-device verification

Code: `firmware/common/csi/src/csi_event.cpp` (the ceiling spends a slot by
what `csi_bundler_admit()` did: an opening keeps it, a merge gives it back),
`csi_bundler.cpp`, and the canary's `src/csi_modules_integration.cpp` +
`src/main.cpp` (`securacv_csi_modules_tick()` once per loop, outside the CSI
power gates, instead of a flush after every window), and
`csi_event_wire.h` (`bundled` is the row's own count on every path).
Host-tested on the real library (`firmware/tests_host/test_csi_bundler_ceiling.cpp`),
on the canary's real bridge (`test_csi_modules_integration.cpp`, a stand-in
for `main.cpp`'s loop) and on the wire builder (`test_csi_event_wire.cpp`);
`main.cpp`'s call is held by `firmware/scripts/check_csi_bundle_tick.py` and
compiled by CI; not run on a device. Owner: U1.

- [ ] **A steady presence state is one row per bundle on the canary**
  - Setup: a canary (`release_ha`) paired to Home Assistant, with a card in.
  - Repro: move in front of the sensor for fifteen minutes, then leave the
    room for five.
  - Expected: no `events` body per refresh. One `core.presence` body about
    ten minutes after the state began, with `"bundled"` above 1 and
    `"duration_sec"` near 540; the next about two minutes after you leave,
    `"bundled"` above 1. The card's `/EVENTS/today.ndjson` holds the same
    rows. Before F81 every refresh (5 s, 20 s, then each minute) was its own
    body with `"bundled":1` and `"duration_sec":0`, and the sixth spent the
    hour's ceiling.
  - Artifact: `docs/audit/repro/F81/steady-presence/`.
- [ ] **A transition inside the first hour is not held back**
  - Setup: as above, and a canary-wap beside it.
  - Repro: twenty minutes in front of both sensors, then leave.
  - Expected: the canary-wap's `GET /api/events/today` shows the new state
    open within a few seconds of leaving; on both devices the new state's
    row commits when its bundle closes. Known limit, not a finding: after
    about an hour in ONE state the six-an-hour ceiling is full of that
    state's own rows, and the transition then waits up to about ten
    minutes for a slot (host-measured: 7 to 602 s, depending on when a slot
    ages out; record the delay you see).
  - Artifact: `docs/audit/repro/F81/transition/`.
- [ ] **An anomaly row waits for its bundle on the canary**
  - Setup: a canary (`release_ha`) paired to Home Assistant, the room empty
    and quiet for a few minutes (`anomaly.baseline` learns the quiet).
  - Repro: walk through the room once, briefly; note the time.
  - Expected: the `anomaly.baseline` `unusual_motion` body reaches Home
    Assistant about two minutes after the motion (its bundle closes on the
    quiet gap; its ten-minute cooldown means nothing merges into it), with
    `"bundled":1`, where before F81 it arrived within a second. Known
    behavior, handed up as a decision, not a finding: every state-bearing
    row but a `system.integrity` tamper now waits for its bundle, and a
    restart inside those two minutes loses it.
  - Artifact: `docs/audit/repro/F81/anomaly-latency/`.
- [ ] **A bundle commits while CSI is shed**
  - Setup: a canary with `FEATURE_POWER_POLICY` on a battery near the
    battery-saver threshold (battery saver and low power shed CSI;
    battery-normal does not).
  - Repro: start presence; let the policy enter battery saver (the health
    log's "Power policy: mode changed" entry names it); wait three minutes.
  - Expected: the open presence bundle commits about two minutes after its
    last observation, with no CSI window arriving (the loop's tick, not the
    feed, closes it).
  - Artifact: `docs/audit/repro/F81/csi-shed/`.

## CSI modules' saved settings at boot (F93) — on-device verification

Code: `firmware/common/csi/src/csi_module.cpp` (`csi_module_init_all()`
runs each registered module's `init()` once; `csi_module_tick_all()` ticks
none before it), `csi_module_settings_nvs.h` (the key map and NVS read rule
both trees share, with one read-only handle for a boot), the canary's
`src/csi_modules_integration.cpp` (`init_modules_from_nvs()`, at the end of
`securacv_csi_modules_init()`) and the canary-wap's `csi_integration.cpp`
(`init()` calls `csi_settings_nvs_init_modules()` right after
`register_v1_modules()`) and `csi_settings_nvs.cpp` (its readers and that
boot init). Host-tested on the canary's real
bridge (`firmware/tests_host/test_csi_module_boot.cpp`) and on the
canary-wap's real readers with the staged library and modules
(`firmware/projects/canary-wap/tests_host/test_wap_module_boot.cpp`); the
boot order is held by `check_event_egress_order.py` (rule 8) and
`check_wap_event_egress.py` (rule 3) and compiled by CI; not run on a
device. Owner: U1.

- [ ] **A saved preset applies from the first minute after a reboot**
  - Setup: a canary-wap on this firmware, dashboard open, on a board with
    no stored presence threshold (no `cp.mt`, `cp.at` or `cp.bt` row in
    the `csi` namespace): never calibrated, no Tuning Lab threshold
    change, per-row reset, "Reset all" or bundle import. Each of those
    stores the thresholds, and a stored threshold wins over the preset and
    the sensitivity (at boot and at once, before F93 as after it), so on
    such a board this row cannot pass. Erase NVS first when unsure.
  - Repro: set the preset to "sensitive" (or the sensitivity slider to
    100); `POST /api/reboot`; when it is back, make a small motion in
    front of it (a hand wave at a few meters) within the first minute.
  - Expected: `GET /api/events/today` shows a `subtle` (or stronger) open
    row for it. Then set "quiet", reboot, repeat: the same motion stays
    `empty`. Before F93 both boots ran on the balanced default until a
    setting was changed.
  - Artifact: `docs/audit/repro/F93/preset-after-reboot/`.
- [ ] **A stored threshold still wins over the preset after a reboot**
  - Setup: the board from the row above, then a calibration applied
    (`POST /api/csi/calibrate/apply`) or the Tuning Lab's "Reset all".
  - Repro: set the preset to "sensitive"; reboot; repeat the small motion.
  - Expected: the board reads it by the stored thresholds (after "Reset
    all", the balanced 35 / 75 / 30: the motion that read `subtle` above
    stays `empty`), while `GET /api/settings` still reports
    `"preset": "sensitive"`. Host-pinned by
    `test_a_stored_threshold_wins_over_the_saved_preset`
    (`test_wap_module_boot.cpp`). Not an F93 change; recorded so a
    calibrated board is not read as F93 failing.
  - Artifact: `docs/audit/repro/F93/threshold-over-preset/`.
- [ ] **A Tuning Lab cooldown applies after a reboot**
  - Setup: as above, `/tune` open.
  - Repro: set `anomaly.baseline.cooldown_sec` to 30; reboot; leave the
    room quiet for two minutes; walk through twice, about 40 s apart.
  - Expected: two `unusual_motion` observations (one row, `"bundled":2`,
    or two rows); with the default 600 s, one.
  - Artifact: `docs/audit/repro/F93/cooldown-after-reboot/`.
- [ ] **A canary with nothing stored behaves as before**
  - Setup: a canary (`release_ha`) on this firmware, freshly flashed (no
    canary-wap image ever stored settings on the board; nothing on the
    canary writes them).
  - Expected: presence and anomaly rows as on the previous firmware; the
    boot log shows CSI armed as before, plus one Preferences
    `nvs_open failed: NOT_FOUND` error line when the modules initialize
    (the boot's one read-only open of the `csi` namespace, which nothing on
    the canary creates; host-tested as one attempt, not one per setting).
    The other difference by design: the activity ribbon's first 15-minute
    bucket starts when the modules initialize in `setup()`, not at
    power-on.
  - Artifact: `docs/audit/repro/F93/canary-defaults/`.

## SoftAP WPA2/WPA3 transition + PMF (F16) — on-device verification

Code: `firmware/common/network/ap_security_policy.h` (host-tested), applied
after every `WiFi.softAP()` in both trees. Owner: U1.

- [ ] **WPA3 phone joins; the device says so**
  - Setup: a `canary (PIO)` `full` image (IDF 5.5) and a canary-wap image;
    a phone that supports WPA3.
  - Expected: the phone joins `SecuraCV-XXXX`; `GET /api/wifi/status`
    (canary) / `GET /api/wifi` (WAP) shows `ap_auth: "wpa2-wpa3"`. If it
    shows `"wpa2"`, record `ap_auth_reason` (a core without SoftAP SAE, or
    a driver refusal) — that is the finding.
  - Artifact: `docs/audit/repro/F16/wpa3-join/`.
- [ ] **WPA2-only client still joins**
  - Setup: same images; a laptop or phone forced to WPA2.
  - Expected: it joins and loads the dashboard (PMF is capable, never
    required).
  - Artifact: `docs/audit/repro/F16/wpa2-join/`.
- [ ] **2.0.17-core builds report WPA2 honestly**
  - Setup: a `canary (PIO)` `dev` or `release` image.
  - Expected: `ap_auth: "wpa2"` with `ap_auth_reason` naming the missing
    SoftAP SAE; any client joins as before.
  - Artifact: `docs/audit/repro/F16/idf44-fallback/`.
- [ ] **STA PMF**
  - Setup: join the Canary to a PMF-capable router.
  - Expected: `sta_pmf: true`; the association is stable.
  - Artifact: `docs/audit/repro/F16/sta-pmf/`.

## Canary NvsManager session lock — on-device verification

Code: `NvsManager` in `firmware/canary/lib/securacv_crypto/src/securacv_crypto.cpp`,
which holds a recursive FreeRTOS mutex from `begin()` to the matching
`end()`. The session arithmetic is `nvs_session_depth.h`, host-tested by
`firmware/tests_host/test_nvs_session_depth.cpp` and, on the real
`begin()`/`end()` over a fake mutex, `test_nvs_manager_lock.cpp`. The loop,
the httpd task serving the API and the pull-OTA task all open sessions on
the one settings handle, and before the lock a session ending on one task
closed it under another. These rows check the real mutex on a board, which
nothing on the host can do. Owner: U1.

In every row, the serial log must not show `[NVS] session wait timed out`.
That line means a task waited 2 s for another's session and gave up.

- [ ] **Status polls during an MQTT reprovision keep MQTT configured**
  - Setup: an HA-enabled canary image (`release_ha`) joined to Wi-Fi and
    paired to a broker; a laptop on the LAN with the API token.
  - Repro: poll `GET /api/mqtt/status` about every 100 ms. Meanwhile send
    `POST /api/mqtt/config` several times with the same host and no
    password (the credential carry keeps the stored one), then once with a
    new host and its password.
  - Expected: no status response lacks `host` or answers
    `"configured": false`. After each save MQTT reconnects to the saved
    broker, and after the same-host saves it still authenticates with the
    stored password.
  - Artifact: `docs/audit/repro/nvs-lock/mqtt-reprovision/`.
- [ ] **A reboot during a status poll keeps the chain head**
  - Setup: the `release_ha` image as above (`/api/mqtt/status` is an HA
    route); a GPS fix, so records are being written.
  - Repro: poll `GET /api/status` and `GET /api/mqtt/status`. Note the last
    `chain_seq` that `/api/status` answers, then send `POST /api/reboot`.
  - Expected: the boot log's `[OK] Chain seq: N` is at least that
    `chain_seq`. The reboot's chain persist landed, so the chain does not
    resume from an older head.
  - Artifact: `docs/audit/repro/nvs-lock/reboot/`.
- [ ] **A pull-OTA install under polling keeps the chain head**
  - Setup: the `release_ha` image, with an update manifest that offers a
    newer build; a GPS fix.
  - Repro: poll `GET /api/status` and `GET /api/mqtt/status`, then start
    the install with `POST /api/ota/install`. The pull-OTA task persists
    the chain before its reboot.
  - Expected: the install completes and the device boots the new image.
    Its `[OK] Chain seq: N` is at least the last `chain_seq` read before
    the restart.
  - Artifact: `docs/audit/repro/nvs-lock/pull-ota/`.

## Canary WAP NvsManager session lock (F53) — on-device verification

Code: the header-only `NvsManager` in
`firmware/projects/canary-wap/arduino/canary_wap/nvs_store.h`, which holds a
recursive FreeRTOS mutex from `begin()` to the matching `end()`, with the
canary's session arithmetic (`firmware/common/storage/nvs_session_depth.h`,
staged next to the sketch). Host-tested by
`firmware/projects/canary-wap/tests_host/test_nvs_store_lock.cpp` (the real
header over a fake mutex) and `test_nvs_session_balance.cpp` (a textual
scan of the sketch that fails on a block that opens a session and never
ends it, or that returns inside one without ending it; a session ended in
only one branch, or left by a `goto`, gets past it). Five tasks open
sessions on the one settings handle: the loop, the httpd task serving the
API, the NimBLE host task, the Bluetooth bring-up task and the QR-scan task.
These rows check the real mutex on a board, which nothing on the host can
do. Owner: U1.

In every row, the serial log must not show `[NVS] session wait timed out`.
That line means a task waited 2 s for another's session and gave up; on the
WAP it would most likely mean a session somewhere never ended.

- [ ] **API writes during chain persists keep the chain head**
  - Setup: a FULL image on a XIAO ESP32-S3 Sense on USB power, at the
    default record interval; a laptop on the LAN with the API token. No GPS
    fix is needed: the loop writes a record every second
    (`RECORD_INTERVAL_MS`), fix or no fix, and persists the chain every 10
    of them (`SD_PERSIST_INTERVAL`). On battery the power policy stretches
    that cadence.
  - Repro: send `POST /api/bluetooth/power`, alternating two TX powers,
    about every 100 ms for a few minutes. Note the last `chain_seq` that
    `GET /api/status` answers, then send `POST /api/reboot`.
  - Expected: the boot log's `[PROV] Chain seq: N` is at least that
    `chain_seq`, and after the boot `GET /api/bluetooth/settings` answers the
    last power sent.
  - Artifact: `docs/audit/repro/nvs-lock-wap/api-writes/`.
- [ ] **A BLE bond during API writes keeps its pairing record**
  - Setup: the same image; a phone that can bond over BLE.
  - Repro: while the laptop repeats the `POST /api/bluetooth/power` loop,
    start pairing (`POST /api/bluetooth/pair/start`) and bond the phone. The
    NimBLE host task saves the pairing record. Reboot.
  - Expected: `GET /api/bluetooth/paired` lists the phone after the reboot.
  - Artifact: `docs/audit/repro/nvs-lock-wap/ble-bond/`.
- [ ] **The vault's key and config calls release the store**
  - Setup: the same image (the vault is compiled in with the camera and the
    PDM mic), on USB power as in the first row; a vault recipient public
    key.
  - Repro: `POST /api/vault/key` with the key, `POST /api/vault/config`
    turning `t3_smoke` on, then `DELETE /api/vault/key`. Wait a minute after
    each (the loop keeps persisting the chain meanwhile), then reboot as in
    the first row.
  - Expected: `GET /api/vault/status` answers `has_key: true` after the
    first call and `has_key: false` with every trigger off after the last;
    it answers the same after the reboot, and `[PROV] Chain seq: N` is at
    least the last `chain_seq` read. Before F53 these calls left their
    sessions open, which the lock would have turned into this row's timeout
    line.
  - Artifact: `docs/audit/repro/nvs-lock-wap/vault/`.

---

When every box above has a corresponding artifact in
`docs/audit/repro/`, update `docs/audit/v0.3_closeout.md` § "How to verify
on-device" to say "complete (see `docs/audit/repro/`)" and tick the two
final `[ ]` lines remaining in the audit doc sign-off checklists.
