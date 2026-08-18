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
custody; the sealed-log external signed high-water-mark and mandatory
out-of-band verify key; in-process TLS for the API and break-glass; the seccomp
allowlist inversion + compat ABI; an Argon2id (`v2`) seed scheme; base-image
digest pinning; the `cargo audit` `--deny` triage; and the regulated-market
assurance tier (FIPS / PKI / RBAC / SIEM / compliance mapping).

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
[`ENTERPRISE_CUSTODY.md`](ENTERPRISE_CUSTODY.md) §8 and
[`quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md):

- **PWK wizard mutating endpoints are unauthenticated** on the
  Supervisor-Docker-reachable port. The correct fix depends on the Home
  Assistant ingress-auth model (require the ingress-authenticated path, or a
  bearer token on mutating requests) and needs live add-on testing; a
  half-verified auth change risks locking the wizard out or providing false
  assurance. Designed in ENTERPRISE_CUSTODY §8.
- **Break-glass HTTP server availability**: a pre-auth slowloris can stall the
  single-threaded server, and the per-peer lockout map is unbounded (and
  collapses to the proxy IP behind a terminator). Availability hardening
  (accept/read timeouts, a bounded lockout map) — tracked, not a
  confidentiality or integrity gap.
- **Defense-in-depth zeroization**: raw-frame cleartext on some seal *error*
  paths and the pre-roll drain gating are belt-and-suspenders items on top of
  the enforced token gate.

## Verification (this pass)

`cargo test --lib` (342 tests) passes, including new tests for the policy gate,
WYSIWYS approval, the audit-path quorum re-derivation and its
fabricated-era rejection, and the per-envelope receipt binding;
`tools/test_unseal_snapshot.py` and the PWK wizard pytest suite pass; `cargo
clippy --all-targets` and `cargo doc` are clean.
