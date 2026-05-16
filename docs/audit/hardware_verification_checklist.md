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
  - Repro: capture A's `x25519_pubkey` via `GET /api/beacon/set` on B.
    Reboot A. After reboot, query again.
  - Post-fix expected: A's `x25519_pubkey` is identical to pre-reboot.
    A successful COSIGN_REQ→RESP exchange completes after reboot
    without re-pairing.
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

---

When every box above has a corresponding artifact in
`docs/audit/repro/`, update `docs/audit/v0.3_closeout.md` § "How to verify
on-device" to say "complete (see `docs/audit/repro/`)" and tick the two
final `[ ]` lines remaining in the audit doc sign-off checklists.
