# canary-wap — the brief for this tree

Loads when you touch a file here; the root `AGENTS.md` still applies, and so
does [`firmware/AGENTS.md`](../../AGENTS.md).

## Reading this tree

- The Arduino sketch under `arduino/canary_wap/` is the canonical source (its
  `platformio.ini` points `src_dir` at it). `canary_wap.ino` is 685 KB,
  `web_ui.h` 286 KB, `mesh_network.cpp` and `bluetooth_channel.cpp` ~150 KB
  each: search for the symbol, then read a line range. A whole read is refused
  by the read guard and costs more than the task.
- `web_assets_gz.h` is generated from `web_ui.h` / `csi_dashboard_html.h` /
  `companion_pwa.h` by `gen_web_assets_gz.py` (`--check` in CI); edit the
  source page, then regenerate.
- The copies of `firmware/common/*` staged in the sketch are byte-identical by
  CI (`firmware/scripts/check_*_sync.sh`): edit `common/` first.
- Host tests: `tests_host/` (the WAP's own) beside `firmware/tests_host/`.
  `firmware/scripts/check_wap_*.py` are the route, loop-command and
  JSON-answer gates.

## Beacon Channel Invariants (v0.2 — must be enforced)

The Beacon channel (`spec/beacon_channel_v0.md`) is the project's
harm-reduction broadcast layer. It carries life-safety advisories with
smoke-detector-grade reliability requirements. Any agent modifying Beacon
firmware (`firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.{h,cpp}`)
or the Chirp v0.2 channel (`chirp_channel.cpp` there, declared in that
folder's `mesh_network.h`; `firmware/common/chirp/chirp_channel.h` is the
C-ABI API the PlatformIO port is to implement, its values held to this
reference by canary-wap's `tests_host/test_chirp_protocol_invariants.cpp`)
MUST observe:

1. **No automatic origination.** Beacons are originated only on explicit
   user action. Sensors (CSI, motion, audio) may prompt a human; sensors
   must not originate.
2. **Two-pubkey rule.** Every Beacon ALERT/UPDATE/CANCEL frame requires two
   distinct Ed25519 signatures from two distinct device pubkeys, both in
   the local beacon set with `trust_level != REVOKED`. Do not introduce a
   "trusted device skips co-sign" shortcut.

   **Solo-degraded exception (spec §6.2):** the single-device origination
   path is permitted ONLY when the physical BOOT button on the device is
   held at the moment of origination (real-time GPIO check), the frame
   carries `BCN_FLAG_SOLO_ORIGIN` in its header, and `certainty` is
   `Observed`. The BOOT hold, like the spec's "no fresh paired cosigner"
   rule, is enforced on the originating device only; no receiver can see
   either. The solo rules a receiver MUST check before accepting are the
   ones on the wire: the `BCN_FLAG_SOLO_ORIGIN` flag,
   `certainty` = `Observed`, `originator_fp` == `cosigner_fp`, and both
   signature slots (one Ed25519 signature, copied) against the
   originator's non-revoked beacon-set entry, so the UI can show solo
   frames downweighted a notch. The BOOT check therefore stops a caller of
   that device's REST API who is not at the device, not a holder of its
   Ed25519 key, who can sign a solo frame on any radio in range (spec §6.2
   security note, §14.2; revocation is the recourse). Do NOT add a
   software-only path that bypasses the BOOT GPIO check. *(Reworded
   2026-09 to match the spec §6.2 security note; maintainer to confirm.)*
3. **No red, no reserved emergency-broadcast tones, no reserved phrasing.**
   See `spec/beacon_cap_gateway_v0.md` §4. The
   `scripts/lint_no_impersonation.sh` CI lint enforces this; do not bypass.
4. **No PII on the wire.** No descriptions of people, no license plates, no
   GPS, no MAC addresses, no `opera_id`, no household identifiers.
   Templates only.
5. **`scope = BCN_SCOPE_PRIVATE` always.** Never `Public`, never
   `Restricted`. Lint-enforced.
6. **No restoration of `TPL_AUTH_FEDERAL_PRESENCE`** or any
   "authority/government/federal presence" template in Chirp or Beacon.
7. **No persistence of Chirp session keys.** Session keys are ephemeral by
   design — that's the privacy firewall between Chirp and Opera.
8. **NVS writes of `opera_secret` and Beacon `beacon_set` require flash
   encryption.** Both code paths refuse on FE-off devices. Do not weaken.
9. **The Beacon audit log is append-only and chain-hashed.** Do not add a
   "rotate" or "delete" operation; export-only.
10. **Drills (`msgType = Exercise`) are wire-format-distinct from real
    alerts.** Do not merge their counters or rate-limit buckets.
11. **Tamper-flagged neighbors auto-revoke (v0.5).** When a paired
    Beacon-set member sends an Opera tamper alert (`MSG_TAMPER_ALERT`),
    `beacon_channel::on_peer_tampered(device_pubkey)` automatically
    transitions that entry to `trust_level = REVOKED`. Do not introduce
    a manual-only path or add a "trust them anyway, fail loud later"
    shortcut. Recovery is operational: physically inspect the device,
    then re-pair through the standard flow.
12. **Audible self-test is NFPA-72 §14 cadence (v0.5).** The
    `PATTERN_SELFTEST_OK` chirp plays monthly during waking hours
    (06:00–22:00 local, wall clock synced) only. Do not increase the
    cadence below monthly and do not lift the night-mode suppression —
    a 3 AM beep is a worse failure mode than a missing chirp.

Modifying any of the above requires updating
`docs/audit/mesh_and_chirp_audit_v1.md` and `docs/security/THREAT_MODEL.md` in the same
PR.
