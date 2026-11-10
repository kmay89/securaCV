# SecuraCV v1 — what gates the tag

v1 means **everything documented works end-to-end** on a first-try install,
`cargo test` passes, and no doc claims more than the code does. It needs both
pillars: privacy-preserving detection and tamper-evident artifacts. This is
the one definition; `CHANGELOG.md` `[1.0.0]` and the README status follow it.

## The gate

**On-device hardware validation.** Everything below is green in CI and
unproven on real ESP32 boards. Run
[`V1_BENCH_TEST_RUNBOOK.md`](V1_BENCH_TEST_RUNBOOK.md) (tracks A–E, issue
#610; per-track procedures in
[`hardware/v1_bench_validation_runbook.md`](hardware/v1_bench_validation_runbook.md)).
When its sign-off matrix is all ✅, tag: README badge `v1-rc → v1.0`, date
the `CHANGELOG.md` `[1.0.0]` entry, set the crate version in `Cargo.toml` to
`1.0.0`.

Nothing else blocks the tag. The rest of the open work is in
[`audit/repo_todo_sweep_2026-09.md`](audit/repo_todo_sweep_2026-09.md); its
section 0 lists the acts only a person can do. One of them deserves a look
before shipping even though the definition above does not name it: until the
release signing-key ceremony (U2 there) both flashers accept firmware on a
checksum alone.

## What is in v1

| Capability | Status | Where |
|---|---|---|
| Frame isolation | Works | `RawFrame` is type-enforced; `spec/invariants.md` |
| Hash-chained, Ed25519-signed log | Works | `log_verify`; `tamper_demo` shows a modified log failing |
| Event contract | Works | allowlist enforced |
| Break-glass | Works | n-of-m trustee quorum, policy storage, approval flow |
| Detection | Works; motion-only by default | `DetectorBackend` + `BackendRegistry`. A stock build reports motion, not classified objects, and `witnessd` warns so at startup. Object detection needs a `--features backend-tract` build and `detect.backend = "tract"` (`detect.tract_model` is optional; the pinned model loads when it is unset). With Frigate, detection is Frigate's job |
| Video ingest | Works, feature-gated | file (`ingest-file-ffmpeg`) and RTSP (`rtsp-ffmpeg`) each have an end-to-end CI roundtrip; the GStreamer backend shares the interface and is not separately exercised |
| Frigate → Home Assistant over MQTT | Works | `frigate_mqtt_e2e` and the `frigate-mqtt-e2e` job; the four-container stack (`verify_pipeline.sh`) is a manual smoke check |
| Vault sealing | Wired, opt-in | needs a valid `BREAK_GLASS_SEAL_TOKEN`; `witnessd` says at startup whether sealing is on |
| Device key | Software-held | OS-RNG seed kept 0600; rotation and DB re-key are operator commands ([`db_key_rotation.md`](db_key_rotation.md)) |
| Sandboxing | Partial, by design | seccomp for adapters and the per-frame detection child (Linux). Detection backends are an audited boundary, not an enforced one ([`security/THREAT_MODEL.md`](security/THREAT_MODEL.md)) |
| Firmware privacy | Works | no raw MAC or precise GPS on any API; `regression_check.sh` fails on either |
| Firmware OTA | Works; canary-sense partial | signed pull-updates with A/B rollback ([`firmware_ota.md`](firmware_ota.md)); the ESP32-C6 pass rides the bench gate |

## Known gaps v1 ships with

- **`export_verify` after a key rotation.** Given the current seed it fails
  its lineage check, and its C2PA anchor belongs to one epoch. Needs a
  maintainer decision ([`db_key_rotation.md`](db_key_rotation.md#verifier-clis)).
- **Break-glass grants in an offline envelope.** The offline verifiers check
  the receipt chain and the approvals commitment, not the trustee signatures
  or the quorum; the kernel re-derives those against its database. The full
  fix is a wire-format revision (issue #1580).
- **Hardware-backed keys** (TPM / secure element): needs hardware.

## Not in v1

- GPU acceleration (`ort` backend) and WASM sandboxing
- Encrypted-vault setup UX + hardware-backed keys (v1.1) — sealing itself is wired
- Remote attestation
- Performance guarantees: there is a harness, and no number is published ([`BENCHMARKS.md`](BENCHMARKS.md))
- Face or plate detection: forbidden by design, in every version

## Decisions on record

- **Both firmware trees ship**, held at parity ([`../firmware/PARITY_PLAN.md`](../firmware/PARITY_PLAN.md)). Converging the Arduino sketch into `firmware/canary` is a post-v1 question.
- **No bundled detection model.** The direct-ingest path has a one-command, hash-checked fetch (`scripts/fetch_detection_model.sh`).
