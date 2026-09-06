# SecuraCV — Security Remediation Pass (2026-08)

Follows an independent findings-level review of the witness kernel
(`witnessd`) and the Privacy Witness Kernel (PWK) add-on. This pass lands the
**code-level** fixes; the architectural / hardware-custody items are tracked in
[`ENTERPRISE_CUSTODY.md`](ENTERPRISE_CUSTODY.md). Every guarantee the specs
overstated has either been fixed in code or corrected in the spec — no finding
was closed by wording alone without saying so.

## Fixed in this pass

### PWK setup wizard (`privacy_witness_kernel/`)

- **Root secret no longer disclosed.** `GET /api/status` returned the full
  add-on options — including `device_key_seed` (the sealed-log integrity secret)
  and MQTT passwords — over a port reachable on the Supervisor Docker network.
  It now returns only the non-secret fields the panel renders
  (`_public_status_options`). `serve_wizard.py`.
- **SSRF / internal port-scan oracle closed.** The device-pairing proxy
  (`_canary_request`) and the camera-reachability test (`test_camera_tcp`) now
  reject loopback / link-local / unspecified addresses and the internal
  Supervisor service names (`_is_blocked_host`), while still allowing RFC1918 LAN
  devices. The camera test returns a generic result instead of a
  refused/timeout/unreachable oracle.
- **Config-injection hardened.** Camera URLs written into `frigate.yml` are now
  scheme-allowlisted (`rtsp/rtsps/http/https`) and emitted as quoted YAML
  scalars, so a crafted value can neither select an arbitrary ffmpeg protocol
  handler nor inject YAML keys.
- Regression tests added for all three (`tests/test_serve_wizard.py`, 56 pass).

### Network listeners fail closed (`src/`)

- **Event API** refuses a non-loopback bind without protection unless
  `WITNESS_API_ALLOW_INSECURE=1` — the bearer capability token no longer crosses
  the LAN in cleartext by accident. `src/api/mod.rs`, `src/bin/witnessd.rs`,
  `src/bin/witness_api.rs`.
- **Webhook adapter** refuses a non-loopback bind without auth (or mutual TLS)
  unless `ADAPTER_WEBHOOK_ALLOW_INSECURE=1`, closing unauthenticated witness-event
  forgery. `src/bin/adapter_host.rs`. (Both mirror the break-glass server's
  existing `validate_exposure`.)

### Module seccomp sandbox (`src/module_runtime/sandbox.rs`)

- **Inherited descriptors dropped after fork** (`close_range`), so a compromised
  module can no longer exfiltrate over an inherited live socket or corrupt the
  sealed log through an inherited DB handle.
- **Newer escape hatches denied** (io_uring, memfd, handle/pidfd/mount openers),
  resolved best-effort so older libseccomp still loads the filter. Sandbox tests
  still pass (4/4).

### Timestamp anchoring honesty (`src/bin/log_anchor.rs`)

- `log_anchor verify` no longer prints "OK" for an anchor whose CMS
  countersignature was **not** checked (no `--ca`). It prints `UNVERIFIED` and
  marks the `genTime` untrusted, so a structurally-consistent but
  cryptographically-unverified anchor can't be mistaken for a trusted timestamp.

### Spec corrected to match the code

- `kernel/architecture.md` (Invariant V) and `spec/invariants.md` now state the
  **honest** break-glass guarantee: the quorum is an authorization gate with
  tamper-evident receipts, **not** a cryptographic threshold over the vault
  decryption key — a vault-file holder can decrypt without a quorum. The
  overstated module-sandbox claim ("physically cannot open files or sockets") is
  corrected to the denylist reality.

### Supply chain (`privacy_witness_kernel/Dockerfile`)

- The PWK builder pins an **exact** Rust toolchain (was the moving `stable`),
  matching the version the root `Dockerfile` builds the same crate with.

## Tracked, not closed here

See [`ENTERPRISE_CUSTODY.md`](ENTERPRISE_CUSTODY.md): threshold/HSM vault key
custody *(the `SECURACV_VAULT_PASSPHRASE` keyguard shipped 2026-08-21 in
#1565 — defense-in-depth, not the threshold; §1 records what remains)*; the
sealed-log external signed high-water-mark and mandatory out-of-band verify
key *(closed in the 2026-08-23 pass below — the mark shipped as
`SECURACV_HWM_PATH`; the key is not mandated, but a run without one is
labeled `self-consistent; identity unverified` instead of `valid`)*;
in-process TLS for the API and break-glass *(closed in the 2026-08-31 pass
below, as the `api-tls` feature)*; the seccomp allowlist inversion + compat
ABI; an Argon2id seed scheme *(closed 2026-08-21 in #1565 — recorded in
ENTERPRISE_CUSTODY §5; it shipped as the opt-in `seed-argon2id:v1:` device-seed
format, not as a `v2` of the legacy form; the strength check is applied by
`break_glass init` on a new database since 2026-09-06, other provisioning
flows still tracked)*; base-image digest pinning; the
`cargo audit` `--deny` triage; and the regulated-market assurance tier
(FIPS / PKI / RBAC / SIEM / compliance mapping).

## Verification

`cargo check --lib` and the full `cargo test --lib` suite pass; the sandbox
tests pass with `libseccomp` linked; `adapter_host` builds with its adapter
feature set; the wizard pytest suite passes (56 tests).

---

# Provenance & quorum-hardening pass (2026-08-18)

A follow-on pass driven by a standards review (courts/evidence law, medical
data integrity, C2PA, key ceremonies, transparency logs, and the frontier-AI
adversary — see [`PROVENANCE_INTEROP.md`](PROVENANCE_INTEROP.md)) plus an
adversarial re-audit of the break-glass quorum, `witnessd`, the vault crypto,
and the PWK add-on. The quorum + unseal target design is
[`../../spec/quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md).

## Quorum & unseal — fixed in this pass

- **Quorum-gated policy mutation (Invariant V).** Changing a *live* quorum
  policy (roster, threshold, or `vault.crypto_mode`) now requires
  current-quorum approvals under a dedicated signature domain
  (`DOMAIN_POLICY_CHANGE_APPROVAL`, disjoint from unlock approvals); the
  bootstrap on an empty database is the only device-key-only path, and it is
  labeled as such. Every accepted change appends a chained, device-signed
  **policy-change history** record (`break_glass policy propose | approve |
  set --approvals | history`). Previously one actor holding the device key
  seed could replace the roster and satisfy "quorum" with keys they minted.
  `src/break_glass/core.rs`, `src/lib.rs`, `src/break_glass/cli.rs`,
  `spec/break_glass.md`.
  - **History is authenticated before it is trusted** (closing two review
    findings on the first cut): the authorizing approvals' commitment is bound
    *inside* the signed record, and the era resolver
    (`verify::load_authenticated_policy_eras`) verifies the full chain — hash
    linkage, device signature, approvals-commitment binding, and each
    non-bootstrap transition's prior-era N-of-M consent — before any era is
    used as ground truth. A device-key holder can sign a fabricated row but
    cannot manufacture the prior trustees' consent, so a forged era can no
    longer launder a forged receipt. (Residual host-attacker "delete the
    ledger and re-bootstrap" is the ENTERPRISE_CUSTODY §2 external-anchor
    boundary.)
- **WYSIWYS trustee approval.** `break_glass request --output-request` emits a
  full request-context file; `approve --request <file>` recomputes the request
  hash locally, displays every decoded field, and refuses a file whose fields
  do not hash to its claimed value. Bare-hash signing remains only as a loudly
  warned fallback. Neutralizes deepfake "sign this hash" calls and the
  Bybit-style blind-signing swap. Same treatment for policy-change proposals.
- **Audit-path quorum re-derivation (verifier differential closed).** The
  `break_glass receipts` CLI audit had **no** quorum floor, and `verify.rs`
  (log_verify / API `/verify`) *skipped* re-derivation on any policy-era
  mismatch — so a forged `Granted` receipt carrying an arbitrary
  `policy_commitment`, or an under-quorum one on the current policy, audited as
  VALID. Both now share `verify::verify_receipt_quorum`, which resolves the
  receipt's era against the signed policy-change history and re-derives the
  quorum against that historical policy's own roster and threshold; an era that
  matches no real policy (when history is present) is a forgery and fails.
  Legacy databases with no history preserve the prior lenient behavior (the era
  cannot be disproven), so old deployments do not false-alarm. `src/verify.rs`,
  `src/break_glass/cli.rs`.
- **Per-envelope binding of trustee consent (Invariant V).** The unseal gate
  bound the token to its envelope/ruleset but never checked that the *receipt*
  it referenced was for the same envelope — so a genuine, fully-approved
  `Granted` receipt for envelope A could be paired with a token for envelope B
  and unseal B, reusing trustee consent across disclosures.
  `break_glass_receipt_outcome_for_verifier` now binds the receipt's
  `vault_envelope_id` and `ruleset_hash` to the token's before the quorum
  re-derivation. `src/lib.rs` (+ every seal/unseal closure threads the
  envelope/ruleset through).

## witnessd / vault / CLI — fixed in this pass

- **Sensitive file writes hardened.** The break-glass token, a freshly minted
  trustee signing key, and unsealed raw media were written with
  `create+truncate`, which follows a pre-planted symlink and ignores the
  requested `0600` when the path already exists. A shared `write_secret_file`
  now opens with `O_NOFOLLOW` and re-applies `0600` after open; the unsealed
  plaintext is zeroized after the write. `src/break_glass/cli.rs`.
- **Raw-media plaintext zeroized on seal error paths.** `seal_bytes` only
  scrubbed the caller's cleartext buffer on success; the "envelope already
  exists" early return and a `seal_v2` failure left it in memory. It now
  zeroizes on every exit. `src/vault/mod.rs`.
- **Break-glass lockout map bounded.** The per-peer auth-failure table grew one
  entry per source IP forever; it now prunes settled entries and caps at 4096
  (a spoofed-source flood can no longer grow it without limit). An untracked IP
  beyond the cap is simply not rate-limited that round — never wrongly locked.
  `src/break_glass/server.rs`.
- **Python unseal output permissions.** `tools/unseal_snapshot.py` wrote the
  decrypted snapshot with the default umask (world-readable) and, under
  `--force`, wrote the new private key into a pre-existing (possibly laxer)
  file before `chmod`. The output is now created `0600` atomically with
  `O_NOFOLLOW`; `--force` unlinks then `O_EXCL`-creates the key.

## PWK add-on — fixed in this pass

- **SSRF loopback-guard parser differential.** `_is_blocked_host` consulted
  only Python's strict `ipaddress` parser, which rejects non-canonical numeric
  IPv4 forms (`127.1`, `0`, octal, hex, decimal) that the OS socket resolver
  nonetheless maps to loopback — letting them through as "hostnames." The guard
  now normalizes any `inet_aton`-accepted form to canonical dotted-quad and
  re-classifies. `privacy_witness_kernel/serve_wizard.py`.
- **YAML injection via the MQTT broker host.** `_write_frigate_config` emitted
  the wizard-supplied `broker_host` into `frigate.yml` unquoted (the camera URL
  was already a quoted scalar); a value with a newline could inject keys. It is
  now a single-line, single-quoted YAML scalar.
- Regression tests added for all three
  (`privacy_witness_kernel/tests/test_serve_wizard.py`).

## Tracked, not closed here (this pass)

Recorded honestly rather than half-fixed; designs in
[`ENTERPRISE_CUSTODY.md`](ENTERPRISE_CUSTODY.md) §4a and
[`quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md):

- **PWK wizard mutating endpoints are unauthenticated** on the
  Supervisor-Docker-reachable port. The correct fix depends on the Home
  Assistant ingress-auth model (require the ingress-authenticated path, or a
  bearer token on mutating requests) and needs live add-on testing; a
  half-verified auth change risks locking the wizard out or providing false
  assurance. Designed in ENTERPRISE_CUSTODY §4a.
- **Break-glass HTTP server availability**: a pre-auth slowloris can stall the
  single-threaded server, and the per-peer lockout still collapses to the proxy
  IP behind a TLS terminator (it keys on the peer socket address). The lockout
  map is now bounded (above); accept/read timeouts and a forwarded-for-aware
  key remain tracked availability items — not a confidentiality or integrity
  gap.
- **Audit tooling on encrypted databases**: `break_glass receipts` opens the
  kernel DB without the SQLCipher key, so it reads only an unencrypted DB; the
  keyed read path (`open_kernel_db_keyed`, already used by `doctor`) should be
  threaded through. Low severity — it cannot leak an encrypted DB's contents,
  only fail to read them. *(Closed in the 2026-09-06 pass below: `receipts`
  and `unseal` both take `--device-key-seed` / `--db-key` now.)*
- **Pre-roll buffer drain gating**: the in-memory pre-event ring drain is
  belt-and-suspenders on top of the enforced token gate at
  `export_for_vault`.

## Verification (this pass)

`cargo test --lib` (342 tests) passes, including new tests for the policy gate,
WYSIWYS approval, the audit-path quorum re-derivation and its
fabricated-era rejection, and the per-envelope receipt binding;
`tools/test_unseal_snapshot.py` and the PWK wizard pytest suite pass; `cargo
clippy --all-targets` and `cargo doc` are clean.

---

# Sealed-log high-water-mark + honest verdict labeling (2026-08-23)

Closes the two `ENTERPRISE_CUSTODY.md` §2 items the interior hash chain cannot
reach on its own: it proves no event was edited, reordered, or deleted
*mid-log*, but it does not bind the log's **length or head**, and a self-anchored
verdict does not prove *whose* log it is.

## Fixed in this pass

### Signed external high-water-mark (`src/log/high_water_mark.rs`, new)
- A device-signed, monotonic `(seq, head)` mark in an `SCVHWM01` container kept
  **outside** the database. `seq` is the newest sealed-event id at write time.
  The signature is domain-separated (`securacv:pwk:sealed-log-high-water:v1`)
  over every field, magic and versions included, so no cross-context or
  downgrade replay.
- **Writer** (`SqliteSealedLogStore`): opt-in via `SECURACV_HWM_PATH`; the store
  advances the mark after each append, *after* the commit and best-effort (a
  failed mark write logs and never blocks a witnessing append — the mark only
  moves forward, so a lag can never cause a false verify failure). A lower `seq`
  at advance time is the benign out-of-order sibling-writer case (a no-op, not
  an alarm); a same-`seq`/different-head write is real corruption and errors.
- **Verifier** (`run_full_verify_with_high_water_mark`, `log_verify
  --high-water-mark`): after the chain verifies, it checks the mark's signer is a
  genesis-anchored lineage key (same rule as the checkpoint signer), verifies the
  signature, and **fails closed** with `FailedLedger::HighWaterMark` /
  `FailureKind::HighWaterRegression` when the live log is behind the mark. The
  live high is the newest **real signed row** (`MAX(id)`), reconciled with the
  signed, already-verified checkpoint when retention has emptied the live table
  — never the writable `sqlite_sequence` counter, so a no-signing-key actor
  cannot inflate a truncated log past the mark, and a legitimate full-retention
  prune (empty table + checkpoint) is not mistaken for a wipe. Covers the
  tail-truncation, whole-file rollback, and wiped-log cases.

### Honest verdict labeling (B3, `src/verify_runner.rs`)
- `VerifyReport` now carries `identity_verified` and a `verdict` string. A run
  with no out-of-band key is labeled `"self-consistent; identity unverified"`
  (it proves internal consistency, not identity); an operator-supplied /
  escrowed verifying key upgrades it to `"valid"`; a failed chain is
  `"tampered"`. `log_verify`'s human summary states which, and points operators
  at `--public-key` / `--device-key-seed` for an evidentiary verdict.
- Additive only: `chain_valid` is unchanged and remains the machine-readable
  pass/fail; existing report consumers see new keys, not changed ones. When
  `SECURACV_HWM_PATH` is unset, the sealed-log write path is byte-identical to
  before.

## Honest scope
The mark is device-signed, so a holder of the signing key forges both the log
and the mark — the key/host-compromise case the threat model scopes out
(`spec/threat_model.md` §2.6). It closes the *no-key* rollback/truncation/wipe.
A rollback that restores the mark file together with an old DB still needs the
mark on append-only/external media (or the TSA anchor) to defeat — the
transparency-witness end-state tracked in `spec/quorum_unseal_v2.md` §4.

## Tracked, not closed here
The transparency-ecosystem witnessing end-state (RFC 9162 Merkle tree, C2SP
checkpoint notes, fleet witness cosigning, TSA/OpenTimestamps aggregate), folding
anchor verification into `run_full_verify`, and cross-binding the receipt chain
heads — all in `ENTERPRISE_CUSTODY.md` §2.

## Adversarial review (folded in before merge)
Two independent skeptical passes over the diff converged on the same defect from
opposite sides, both now fixed and regression-tested:
- **Trusting `sqlite_sequence` let a no-signing-key actor mask a truncation.**
  An actor with the DB key could `DELETE` the newest rows and inflate the
  `sqlite_sequence` counter above the mark, so the live "seq" read past the mark
  and skipped the head anchor. Fixed by reading the live high from `MAX(id)` of
  real signed rows only; a new test inflates the counter and confirms the
  truncation is still caught.
- **A legitimate full-retention prune false-failed as a wipe.** Pruning that
  empties the live table (leaving a signed checkpoint) reported no head, which
  the old gate read as a wipe. Fixed by reconciling the empty-table case against
  the already-verified checkpoint head; a new test exercises a full prune and
  confirms verify passes.
- **That reconciliation trusted the checkpoint's unsigned cutoff (2026-09
  follow-up).** The checkpoint signed only its head, so an actor with DB write
  access could wipe the table, replay an old signed checkpoint, and inflate its
  `cutoff_event_id` past the mark. Checkpoints now sign
  `SHA256(tag ‖ chain_head_hash ‖ le64(cutoff_event_id))`
  (`log::checkpoint_message`); every verifier accepts the legacy bare-head
  form, but over an empty table a legacy cutoff is trusted only when the
  checkpoint head equals the mark's head — otherwise a prune cannot be told
  from a wipe and verify fails closed, saying exactly that. Tests cover the
  inflated bound cutoff, the legacy-at-the-mark pass, and the replayed legacy
  checkpoint.
Also hardened: the concurrent-writer mark advance no longer raises false
"regression" alarms (benign out-of-order no-op), and the honest-scope docs now
state the best-effort-lag, durability (`synchronous=full`), and lockstep-rollback
residuals plainly.

## Verification (this pass)
`cargo test --lib` (373 tests) passes, including new high-water-mark codec /
monotonicity / live-head tests and verify-runner tests for the pass,
live-behind-mark, tail-truncation, full-retention-prune, inflated-counter,
untrusted-signer, honest-label, and end-to-end writer→verify paths; `cargo
clippy --lib --bins -- -D warnings` and `cargo fmt --check` clean (default and
`pqc-signatures`); spelling and docs-index lints pass; the kernel-status-grid
generator produces no diff.

---

# In-process TLS pass (2026-08-31)

Scope: close the ENTERPRISE_CUSTODY §4 residual — "provide a cert/key" on the
token-bearing HTTP surfaces now encrypts the socket instead of attesting that
something else does.

## Fixed in this pass

- **New `api-tls` feature: in-process rustls termination for the Event API and
  the break-glass server.** Both accept loops serve every connection through a
  `rustls::StreamOwned` session when TLS material is configured (the same
  arrangement as the webhook adapter's `adapter-webhook-tls` module). PEM
  parsing and `ServerConfig` construction happen at spawn/bind time, so bad
  material fails startup, not the first connection.
- **Exposure gates key off TLS being ACTIVE, never off config presence.** The
  Event API's non-loopback refusal now admits a bind when in-process TLS
  actually wraps the socket; a plaintext bind still requires
  `WITNESS_API_ALLOW_INSECURE=1`. On a build without the feature, configuring
  cert/key on the Event API is a **startup error** — material the build cannot
  terminate must not look like protection. Break-glass keeps its bind-time
  refusal; without the feature its cert/key remain an explicit attestation of
  an external terminator (now with a startup warning saying exactly that).
- **A hostile connection cannot wedge the single-threaded loops.** Every
  socket operation — the lazy rustls handshake included — runs through a
  `DeadlineStream` that enforces the per-op inactivity timeout (2s API / 5s
  break-glass) AND an absolute 30s wall-clock budget for the whole
  connection. A per-op timeout alone is reset forever by a peer dripping one
  byte per interval (slowloris) — a pre-existing gap on the plaintext request
  path that in-process TLS would have widened with one more drip-able phase;
  both are closed by the budget, which also bounds a peer draining a large
  response one byte at a time. The break-glass server additionally gains the
  write timeout the Event API already had. (Raised independently by review;
  regression-tested with a spent-budget unit test.)
- **Operator wiring.** witnessd and witness_api read `WITNESS_API_TLS_CERT` /
  `WITNESS_API_TLS_KEY` (paths to PEM files, both-or-neither);
  `break_glass_serve` keeps its `--tls-cert`/`--tls-key` flags and now
  advertises `https://` when termination is in-process. The container image's
  health probe follows the TLS configuration (https when a cert is named).
- **Key-material hygiene.** `ApiTlsConfig` scrubs its PEM buffers on drop
  (zeroize) and redacts the key from `Debug` formatting; TLS sessions send
  `close_notify` on teardown so strict clients see an orderly closure.

## Honest scope

- Server-authentication only: client auth on these surfaces remains the
  rotating capability token. mTLS (as the webhook module already offers) would
  be a further tier.
- The RFC-3161 CMS countersignature check in Rust stays tracked in
  `ENTERPRISE_CUSTODY.md` §4 — this pass is transport only.
- Deliberate asymmetry, signed off: on a non-`api-tls` build the Event API
  hard-refuses configured TLS material, while break-glass keeps its
  pre-existing attestation contract (routable bind allowed with cert/key as
  the operator's claim that an external terminator fronts it, warned at
  startup). Changing break-glass to refuse would break existing front-proxy
  deployments; the honest posture is the warning plus this record.

## Verification (this pass)

`cargo test --lib` passes on the default build (including the new
refuse-material-without-feature and non-loopback-refusal tests) and with
`--features api-tls` (389 tests, including end-to-end handshakes: a rustls
client reads `/health` and the break-glass console over the encrypted session,
and a plaintext client gets no HTTP from a TLS listener). `cargo clippy
--all-targets -- -D warnings` clean on default, `api-tls`, and the pqc combo;
`cargo fmt --check` clean. CI gains `Clippy (api-tls)` + `Test (api-tls)`
steps in the optional-feature build gate.

## Ceremony runbook pass (2026-09-06)

Writing [`CEREMONY_RUNBOOK.md`](CEREMONY_RUNBOOK.md) over the shipped commands
meant running every one of them as a ceremony would. What that surfaced:

### Fixed in this pass

- **`break_glass receipts` and `break_glass unseal` could not open a real
  database.** Every kernel-created database is SQLCipher-encrypted
  (`Kernel::open` always keys the connection), but both commands opened it
  unkeyed and failed with "file is not a database" on any deployment. Both
  now take `--device-key-seed` (env `DEVICE_KEY_SEED`) or `--db-key` (env
  `SECURACV_DB_KEY`) exactly like `log_verify`, `log_anchor`, and
  `court_export`, and an unkeyed open of an encrypted file fails with a
  message naming the flag. Regression test:
  `receipts_audit_opens_the_encrypted_kernel_database_with_the_seed`.
- **Operator guide described `trustee enroll` committing the policy at `n`
  and "strengthening" on each further enrollment.** The code commits only
  when the roster is complete (`m` enrolled); earlier enrollments edit the
  draft only. The guide, the Lab description, and the Lab's Operator's Bench
  transcript (`gen_operator.py` → `devices/operator.json`, with its test
  `operator.test.js`, which had asserted the wrong behavior) now say what the
  code does: one commit line, at the complete roster.
- **The relying-party verification card and the Observer role handed out the
  device signing seed.** `policy history` and `policy show` required
  `--device-key-seed` and opened a full `Kernel` (which also writes schema and
  backfill rows), so the only way an observer could run them was to hold the
  seed that signs every ledger. Both now take `--db-key` / `SECURACV_DB_KEY`
  (and `history` a pinned `--public-key-file`) and open the database
  read-only — `SQLITE_OPEN_READONLY`, so a mistyped `--db` fails instead of
  creating an empty database and no audit connection can write (test:
  `db_key_derivation_opens_the_database_without_the_seed` asserts a refused
  write and unchanged file bytes); a new `break_glass db-key` derives the
  database key from the seed so the operator can hand a verifier that lesser
  credential. The card and the roles table no longer mention
  `DEVICE_KEY_SEED` for a verifier.
- **`break_glass receipts` printed plain `VALID` under a key read out of the
  database it was auditing.** It, and `policy history`, now print where the
  verifying key came from (`Verifying key: pinned out of band` vs `read from
  the audited database — … self-consistent; identity unverified`) and suffix
  the summary line the same way `log_verify` labels that situation.
- **`break_glass policy history` verified rows under the current device key
  only.** It now verifies each row against the genesis-anchored lineage like
  `receipts` and `log_verify`, so a legitimate rotation no longer turns every
  earlier row INVALID (regression test:
  `policy_history_verifies_rows_signed_across_a_device_key_rotation`).
- **`break_glass init` accepted a bare passphrase as the seed of a new
  device.** It now applies `validate_new_seed_strength` unless the database
  already carries a pinned identity under that seed — path existence alone
  is not enough, since a pre-created empty file would otherwise be
  provisioned with the weak seed (review finding on #1660).
- **`court_export`'s own `VERIFICATION.md` told the recipient to run
  `openssl ts -verify … -in anchors/<token>.tsr` without `-token_in`, on
  files that are bare DER TimeStampTokens** — openssl rejects that with an
  ASN.1 tag error, so the one step the design delegates to an independent
  implementation did not work as printed. The kit now names those files
  `.der`, prints `-token_in` and says why, and the end-to-end test runs the
  printed line against a throwaway TSA and requires `Verification: OK` (the
  earlier test only checked the string). `log_anchor verify` also no longer
  claims "in chain history" for `digest` anchors, where that check is
  skipped by design.
- Runbook and CPS quoted outputs and commands corrected against source: the
  `✔ <stage> (<elapsed>)` marker, which binaries take `--ui`, the drill's
  `Result: DRILL PASSED …` line, `doctor` not being a vault-touching tool,
  `log_anchor import` labeling (never refusing), no anchor being cross-bound
  into `log_verify`, the `openssl ts -verify` forms for a `.tsr` versus a bare
  token, the checkpoint record's fields, and CPS status badges for rehearsal
  cadence and network security.

### Tracked, not closed here (this pass)

Every item below is labeled **GAP** in the runbook where a ceremony step
needs it, with the procedural stand-in the runbook uses meanwhile.

- **No operator command for device-key rotation or database re-keying.**
  `Kernel::rotate_device_identity` and `rekey_database_file` are library
  APIs; nothing in `src/bin`, the HA add-on, or `scripts/` calls them. A
  rotation ceremony is therefore not scriptable today.
- **No keyguard migration or passphrase rotation.** `SECURACV_VAULT_PASSPHRASE`
  wraps a *fresh* master key; there is no command to wrap an existing
  plaintext `master.key` or to change the passphrase, and `break_glass doctor`
  reports only `master.key` (it does not recognize `master.keyguard`).
- **No trustee key-generation / public-key helper** for a trustee who mints
  their own key; deriving the public hex from a self-minted seed needs an
  external Ed25519 tool.
- **The served console writes no per-request log**; the durable trace is the
  receipt row and the consumed-token row. A ceremony transcript must capture
  the HTTP replies itself.
- **`break_glass request --db` is accepted and ignored** — the request never
  consults the roster; a roster mismatch surfaces only at `authorize`.
- **No anchor is cross-bound into `run_full_verify` / `log_verify`.** Anchors
  of either kind are checked only by `log_anchor verify` (imprint, chain
  membership for `chain_head`, countersignature with `--ca`); a `valid`
  ledger verdict says nothing about the anchors table
  (`quorum_unseal_v2.md` §4).
- **`witnessd` first run and the HA add-on wizard do not apply
  `validate_new_seed_strength`**; only `break_glass init` does.

### Honest scope

Nothing in this pass changes a cryptographic guarantee. The receipts/unseal
fix restores an audit and an unseal path that were unusable on encrypted
databases; the rest is documentation that now says which steps of a custody
ceremony the code enforces and which are human rules — the same distinction
[`../../spec/quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md) §3.4 draws
between the ceremony *mode* (design) and the ceremony *procedure* (runbook).
