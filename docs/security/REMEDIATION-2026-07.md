# SecuraCV — Security & Supply-Chain Remediation Sweep (2026-07)

**Date:** 2026-07-20
**Scope:** GitHub code-scanning (CodeQL) alerts, the open engineering-issue
backlog, and the doc-12 supply-chain hardening track (#924).
**Branch of record:** `claude/code-scanning-vulnerabilities-vmh7ap` — each item
below landed as its own focused, CI-verified pull request.

## Summary

Starting from the open CodeQL alert list, this sweep triaged every alert to a
clean state, then worked the solvable engineering issues one verified PR at a
time, and finally advanced the no-secrets-required slice of the #924
supply-chain track. **Nine PRs merged.** The items that remain open are blocked
on physical hardware, or on signing keys / audit-policy decisions that only the
maintainer can supply — they are listed under **Pending** with the specific
blocker for each. Nothing below is blocked on engineering effort that could be
completed and verified from CI.

## Done

### Code scanning (CodeQL)

All open code-scanning alerts were triaged to a clean state:

- Genuine findings were **fixed in code** — see the firmware and kernel PRs
  below (notably the first-boot entropy and reconnect-jitter fixes).
- Non-applicable findings were **dismissed with written justification** through
  the code-scanning API, so each dismissal carries a one-line rationale rather
  than a bare "won't fix."

CodeQL runs green on `main` across all analyzed languages (actions, c-cpp,
javascript-typescript, python, rust).

### Merged PRs

| PR | Issue | What & why |
|----|-------|-----------|
| #987 | #922 | **OTA-engine sync guard.** `firmware/scripts/check_ota_sync.sh` plus a `firmware.yml` step fail CI if the two Arduino OTA engine copies drift, so the sense/vision sketches cannot silently diverge. |
| #989 | #919 | **Poison-tolerant adapter locks + outage sealing.** Adapters recover a poisoned `Mutex` (`unwrap_or_else(\|p\| p.into_inner())`) instead of cascading a panic, and seal a `GapMissingData` record on an adapter outage so a gap is provable rather than silent. The trait is `cfg`-gated to stay dead-code-free under `-D warnings`. |
| #990 | #918 | **Boot-time chain verification + liveness watchdog.** `witnessd` verifies the sealed-log tail at startup and drops to a recorded safe mode on mismatch; a watchdog task tracks liveness using `saturating_duration_since`. |
| #991 | #924 | **cargo-deny gate.** `deny.toml` + `audit.yml` enforce license / bans / advisories / sources on every push, using the sparse-registry URL. |
| #992 | #923 | **Property tests.** proptest coverage for `TimeBucket` coarsening (never below `MIN_BUCKET_SIZE_S`) and hash-chain append/verify round-trips. |
| #993 | #921 (F4) | **Reconnect jitter + MQTT socket timeout (firmware).** WiFi/MQTT reconnect backoff gets `esp_random()`-derived jitter to avoid a synchronized-reconnect thundering herd across the fleet; `PubSubClient::setSocketTimeout(5 s)` bounds a stuck broker socket. |
| #994 | #921 (F3) | **First-boot entropy.** The one-time identity keygen is wrapped in `bootloader_random_enable()/_disable()` so the RNG is properly seeded before WiFi/BT bring the RF entropy source online (ESP-IDF's documented early-entropy pattern). |
| #995 | #924 | **Docker base images pinned by digest.** All four `FROM` lines pinned to multi-arch OCI-index digests; a `docker` Dependabot ecosystem keeps the pins fresh and catches base-image security rebuilds. |
| #999 | #924 | **SBOMs attached to releases.** `sbom.yml` gains a `release: published` trigger and a least-privilege `attach-to-release` job that promotes the Rust / Node / firmware CycloneDX SBOMs from a 90-day CI artifact to permanent release assets. |

### Issue housekeeping

- **#932 closed as a duplicate of #933** (which remains the surviving tracker).

## Pending — and why

These are open on purpose. Each is blocked on something only the maintainer can
provide.

### Hardware verification

- **#921 — on-device entropy verification (C3 / C6 / S3).** The entropy *code*
  is fixed and merged (#993, #994) and CI-compiled, but confirming RNG quality
  and reconnect behavior on each physical SoC needs the boards on a bench. This
  is left to the maintainer's hardware verification pass.

### #924 supply-chain — remaining sub-items

The no-secrets pieces of #924 are done (cargo-deny #991, digest pins +
Dependabot #995, SBOM-to-releases #999). What remains is exactly the part that
depends on your keys and release decisions:

- **SLSA provenance** — needs the release build wired to emit signed provenance
  attestations (a build-identity decision plus attestation storage).
- **cosign signatures** — needs your signing key, and a key-management choice
  (keyless / OIDC vs. a managed KMS key).
- **cargo-vet** — needs an audit-policy decision: which trusted orgs / imports
  to seed the audit set with. A wrong initial policy is worse than none.
- **installer tag + checksum** — needs coupling to the release process
  (publish a pinned installer tag and checksum alongside the release
  artifacts).

## Verification notes

- Every PR was merged only after its full CI matrix went green. For the
  firmware changes (not locally compilable in the remediation environment), the
  PlatformIO / Arduino / static-analysis builds in CI are the compile-check
  backstop, and on-hardware behavior is explicitly deferred to #921's bench
  pass.
- The workflow / CI changes (#991, #995, #999) are gated by
  `workflows-lint.yml` (actionlint + shellcheck on every `run:` block); the
  container-image builds validate the Docker digest pins; and a `generate-sbom`
  run plus the release-gated `attach-to-release` job validate the SBOM path.
