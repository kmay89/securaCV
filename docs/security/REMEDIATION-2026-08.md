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
