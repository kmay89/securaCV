# Enterprise Key Custody & Assurance — tracked work

This document is the honest home for the security properties SecuraCV **does not
yet** provide, the concrete designs to close them, and the operator controls
that already exist. It exists so an auditor sees the remaining items as *known
and designed*, not latent — and so nobody trusts a guarantee the code does not
mechanically enforce.

The kernel's threat model deliberately scopes **host/root compromise out**
(`docs/root_paradox.md`, `spec/threat_model.md §2.6`). Everything below is about
raising the bar *inside* that boundary and about the separate assurance tier the
regulated markets (banking, government/classified) require. None of it weakens
the privacy invariants — it is an additive **enterprise-custody tier**, not a
change to the consumer kernel.

---

## 1. Vault key custody — quorum is authorization, not the lock

**State today.** Break-glass enforces a genuine N-of-M threshold of trustee
signatures as an *authorization gate*, re-derived at the unseal gate and never
trusted from a stored outcome, and every attempt leaves a tamper-evident
receipt. But envelope confidentiality reduces to a **device-local master key**
(`vault/envelopes/master.key`; for pq/hybrid also `kem-mlkem768.key`) stored
beside the ciphertext. Trustee keys never enter the DEK wrap. An actor who
possesses the vault directory — stolen disk, backup, snapshot, or a compromised
host — can decrypt without any quorum. `kernel/architecture.md` (Invariant V) and
`spec/invariants.md` now state this honestly.

**Shipped now — passphrase keyguard (defense-in-depth).** Set
`SECURACV_VAULT_PASSPHRASE` and the vault master key is no longer a plaintext
file: it is wrapped under an Argon2id KEK in `master.keyguard` (an `MKG1`
container, AAD-bound to the vault's canonical directory — a deterrent against a
careless copy to a new path, not a cryptographic anti-exfiltration control),
and **no plaintext `master.key` is ever written** in this mode. The passphrase
is held only in memory, sourced from the environment, never on disk. Possession
of the vault directory alone stops being sufficient — the operator-held
passphrase is also required. This is honest defense-in-depth, **not** a
cryptographic quorum: a single holder of the passphrase can still decrypt (see
the threshold item below for the genuine-quorum end-state). The envelope crypto
is untouched, so every already-sealed v1/v2 envelope stays byte-identical, and a
legacy plaintext `master.key` keeps working unchanged (passphrase mode refuses
to run alongside a plaintext key so an operator must migrate deliberately).

**Still tracked (in order of strength):**

1. **Threshold-wrap the DEK/KEK.** Split the wrapping key across trustees with
   Shamir (a vetted crate + cross-impl KATs), or use threshold ML-KEM /
   threshold-ElGamal so decapsulation genuinely requires *n* trustee shares.
   Break-glass then becomes the cryptographic lock, not a policy check upstream
   of it. Pair it with `crypto_mode = pq/hybrid` so only *unseal* is
   quorum-locked (sealing uses the public KEM key). Highest-risk item: a sharing
   bug renders evidence permanently unopenable, so it ships on its own once the
   `MKG1` container is in the field.
   *Design now specified:* `spec/quorum_unseal_v2.md` §2 — a vault KEK split
   n-of-m with VSS-hardened Shamir (commitments in the receipt chain,
   self-describing share envelopes, in-memory reconstruction at the unseal
   gate), the identical split applied to the ML-KEM-768 seed, a two-quorum
   operational/recovery structure, and proactive resharing as a first-class
   ceremony. The standards research behind it: `PROVENANCE_INTEROP.md` §1.4.
2. **Hardware KEK for `master.keyguard`.** Seal the KEK in an HSM / TPM /
   PKCS#11 token (a new `MKG1` kind) so even the passphrase (now shipped, above)
   is not enough without the hardware. Feasible in code; needs hardware to test,
   so excluded from default CI.

**Operator control that already exists (partial, for the DB — not the vault).**
The SQLCipher database key can already be decoupled from the device identity:
set `SECURACV_DB_KEY_SEED` to an independent secret (see
`resolve_db_encryption_key` / `derive_db_encryption_key_from_secret` in `src/lib.rs`).
Keep that secret off the device (a secrets manager, TPM, or an operator-held
passphrase), and the DB-at-rest key is no longer recoverable from a file beside
the database. This does **not** yet cover the vault master key — that is item 2
above.

---

## 2. Tamper-evidence — bind the log's length and root externally

**State today.** The hash chain proves *interior* integrity (edits, reordering,
mid-log deletion are all caught). It does not, by itself, bind its own **length
or head**: truncating the newest events, restoring an earlier whole-DB snapshot,
or wiping the log entirely can still verify as internally consistent. The
default verifier also takes its identity anchor from the DB under audit, so a
"valid" verdict is only as strong as an out-of-band pin of the device key.

**Design to close it:**

- **Signed external high-water-mark.** Persist a signed, monotonic
  `(count, head_hash)` outside the DB (an append-only external log, or the
  RFC-3161 TSA anchor), and have `run_full_verify` fail closed when the current
  head/count is behind it. Closes tail-truncation, whole-file rollback, and the
  wiped-log case.
  *Design now specified:* `spec/quorum_unseal_v2.md` §4 — implement the
  high-water-mark as transparency-ecosystem witnessing: an RFC 9162 Merkle
  tree over the sealed hashes (receipt-chain heads as leaves = the
  cross-binding item below), C2SP checkpoint notes, fleet-internal witness
  cosigning as the default, and one fleet-level aggregate checkpoint anchored
  outward (TSA + OpenTimestamps). See `PROVENANCE_INTEROP.md` §1.5.
- **Fold anchor verification into `run_full_verify`** and require the CMS
  countersignature to be checked in-boundary (see §4). `log_anchor verify` no
  longer prints "OK" for a cryptographically unverified anchor — it prints
  `UNVERIFIED` — but anchor checking is still a separate command; folding it in
  makes truncation-past-an-anchor a hard verify failure.
- **Cross-bind the receipt chains.** Anchor the break-glass and export receipt
  chain heads into the sealed-event chain (or a signed manifest of all three
  heads) so the Invariant-V/IV audit trail cannot be truncated independently.
- **Out-of-band verify key.** Require published/escrowed device-key material for
  any *evidentiary* verdict; label a self-anchored result "self-consistent,
  identity unverified" rather than "valid".

---

## 3. Module isolation — invert the denylist, cover the compat ABI

**State today (hardened in this pass).** The seccomp filter now also closes the
newer escape hatches (io_uring, memfd, handle/pidfd/mount openers), and the
forked worker drops every inherited descriptor except the result pipe, so an
inherited DB handle or live socket can no longer be reused for exfiltration.

**Residual, tracked:** the filter is still a *default-allow denylist* bound to
the native ABI. To make "modules cannot open files or sockets" mechanically
true, invert it to an **allowlist** (`KillProcess` default + an explicit allow
set validated against the real inference workload) and cover the 32-bit compat
ABI. Separately, keep the signing/DB keys out of the process that forks the
sandbox worker (spawn the worker before keys are loaded, or run modules in a
key-free process) so a code-exec bug in the child never sees key pages.

---

## 4. Transport — in-process TLS on the token surfaces

**State today (hardened in this pass).** The Event API and the webhook adapter
now **fail closed** on a non-loopback bind without protection, matching the
break-glass server's `validate_exposure`. The API refuses an off-loopback bind
unless `WITNESS_API_ALLOW_INSECURE=1`; the webhook refuses a non-loopback bind
without auth (or mutual TLS) unless `ADAPTER_WEBHOOK_ALLOW_INSECURE=1`. This
closes the *accidental plaintext-on-LAN* exposure.

**Residual, tracked:** neither the Event API nor the break-glass HTTP server
terminates TLS **in-process** — cert/key on break-glass attests an external
terminator. Wire `rustls` into both accept loops (mirror the webhook module) so
"provide a cert/key" actually encrypts the socket, and verify the CMS
countersignature of RFC-3161 anchors inside Rust (not only via an optional
external `openssl ts -verify`).

### 4a. PWK wizard — authenticate the mutating endpoints

**State today.** The PWK setup wizard (`privacy_witness_kernel/serve_wizard.py`)
binds `0.0.0.0:8788` and is served to the user through the Home Assistant
Supervisor **ingress** proxy (which authenticates the user). Its `do_POST`
handlers — `/api/save` (writes config and mints `device_key_seed`),
`/api/restart-ha`, `/api/test-camera`, and the `/api/mesh/pair/*` device
proxies — carry **no auth check of their own**, so any process that can reach
the container's port directly on the Supervisor Docker network (a malicious
sibling add-on, or SSRF from one) can invoke them, bypassing ingress. The
2026-08 pass already minimized secret *disclosure* on `/api/status` and closed
the SSRF/loopback and YAML-injection vectors on this surface; the mutating
endpoints are the remaining item.

**Design to close it.** Verify that a mutating request actually arrived through
the authenticated ingress path rather than directly on the port — the robust
form is to require the Supervisor-injected ingress session (HA sets an
`X-Ingress-Path`/session the add-on can validate against `SUPERVISOR_TOKEN`),
and/or require a per-install wizard bearer token minted at first run and handed
to the browser only through the ingress-authenticated page. Fail closed on a
direct-port request. This needs live add-on testing (a wrong cut either locks
the wizard out or gives false assurance), which is why it is tracked here rather
than patched blind. Binding to loopback is not sufficient on its own — the
Supervisor reaches the add-on over the Docker network, not loopback.

---

## 5. Seed strength

`signing_key_from_seed` derives the device key with a single unsalted SHA-256
gated on a 32-character minimum. Auto-generated seeds are full-entropy OS random
and safe; a human-supplied passphrase that merely clears 32 characters is
enumerable offline. Changing the derivation would invalidate every existing
device identity, so the path is a **versioned** scheme: add an Argon2id
(memory-hard) KDF for a new `v2` seed format, keep `v1` (SHA-256) for existing
devices, and steer operators to full-entropy seeds
(`openssl rand -hex 32`) meanwhile.

---

## 6. Regulated-market assurance tier

These are procurement requirements for banking / government / classified, absent
by design in a privacy-first consumer kernel. They belong in a **separate tier**,
not bolted onto the invariants (the kernel's non-queryable, local-ownership,
host-out-of-scope philosophy is structurally at odds with SOC-2 / 800-53
audit-centralization).

| Control | Status | Path |
|---|---|---|
| FIPS 140-2/3 validated crypto | Missing | Build `rustls` on `aws-lc-rs` FIPS mode; validated module for the sealing/signing path |
| HSM / secure-element key custody | Missing on host | §1 item 2 |
| PKI / mutual-TLS device identity | Missing | CA + certificate lifecycle + revocation; SPIFFE/x.509 |
| RBAC + least privilege | Missing | Role model (operator / auditor / admin) atop the quorum |
| Audit export to SIEM (CEF/syslog/OTLP) | Absent by design | Opt-in, tier-only export that does not violate non-queryability |
| Tamper detection + response (host) | Firmware only | Host-side key zeroization on tamper |
| Formal compliance mapping | None | SSP + SOC-2 / ISO 27001 / NIST 800-53 / CC / CJIS mapping |

---

## 7. Advisory gate — triage toward `--deny`

The `cargo audit` job currently fails only on *vulnerabilities*, not on
informational advisories. Making it `--deny unmaintained --deny unsound --deny
yanked` is the goal, but it must be a **deliberate triage**, not a blind flip:
the tree carries the advisories below, and `.cargo/audit.toml` policy requires a
written reachability analysis per ignore (never ignore just to quiet the gate).
Flip the gate only after each is either bumped or given a real justification:

| Advisory | Crate | Class | Note |
|---|---|---|---|
| RUSTSEC-2026-0161 | `pqcrypto-traits` | unmaintained | trait defs only; PQ crates are behind non-default `pqc-*` features |
| RUSTSEC-2026-0162 | `proc-macro-error` | unmaintained | build-time proc-macro helper; not in the runtime binary |
| RUSTSEC-2024-0370 | `rustls-pemfile` | unmaintained | small stable PEM parser; TLS-feature builds only |
| RUSTSEC-2025-0134 | `memmap2` | unsound | confirm the pulling path and whether the aliasing pattern is reachable |
| RUSTSEC-2026-0186 | `spin` | yanked | prefer a lockfile bump to a maintained version |

Toolchain/image supply-chain items handled in this pass and remaining:
the PWK builder now pins an exact Rust toolchain (was the moving `stable`).
Still open: pin `BUILD_FROM`/`alpine:3.18` by `@sha256:` digest (mirror the root
`Dockerfile`) and add a sha256 check of the rustup installer and the cargo-chef
tarball.

---

*This file is the canonical tracker; link findings here rather than restating
them. Update it as items land.*
