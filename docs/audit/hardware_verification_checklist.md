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

Read the results knowing one thing the backfill cannot change: rows that go
through the CSI bundler (presence, and `system.integrity` tampers, which
carry a state) take ids from the bundler's own space — 0x80000000 upward,
restarting every boot, covered by no floor — and commit when their bundle
closes, not in id order. Home Assistant's replay gate refuses a row whose
id is below one it already verified, live or not, and the backfill skips
exactly those rows. So judge "whole" against the rows HA could accept; a
`replay` verdict on a LIVE row is that pre-existing id-space problem, not
the backfill.

- [ ] **An outage longer than the offline queue arrives whole**
  - Setup: an HA-enabled canary image (`release_ha`) with a card in,
    paired to Home Assistant; the MQTT broker on a host you can stop.
  - Repro: stop the broker; commit more than 12 events (presence changes
    in front of the sensor); restart the broker.
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
    verdict on any backfilled body (at most nine of its rows may be missing
    — the NVS ceiling's stride). Post-reboot rows from the bundler restart
    below the pre-reboot ids, so HA refuses them live and the backfill does
    not send them — the id-space problem above, recorded as an open item.
  - Artifact: `docs/audit/repro/F37/reboot/`.
- [ ] **Another device's card is left alone**
  - Setup: a card taken from a canary-wap (or another canary).
  - Expected: the health log says `SD event log belongs to another device -
    not used`; the card's `/EVENTS` is unchanged afterwards; events still
    publish live.
  - Artifact: `docs/audit/repro/F37/foreign-card/`.

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

---

When every box above has a corresponding artifact in
`docs/audit/repro/`, update `docs/audit/v0.3_closeout.md` § "How to verify
on-device" to say "complete (see `docs/audit/repro/`)" and tick the two
final `[ ]` lines remaining in the audit doc sign-off checklists.
