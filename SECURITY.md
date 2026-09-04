# Security Policy

This file is the **security policy and vulnerability-reporting** entry point.
The long-form security documents live under [`docs/security/`](docs/security/):

| Document | Purpose |
|----------|---------|
| [`docs/security/SECURITY_MODEL.md`](docs/security/SECURITY_MODEL.md) | User-facing security guarantees (included in every evidence export) |
| [`docs/security/THREAT_MODEL.md`](docs/security/THREAT_MODEL.md) | The Ten Security Principles + implementation review checklist |
| [`docs/security/SECURITY-AUDIT.md`](docs/security/SECURITY-AUDIT.md) | Historical security-audit reports |
| [`spec/threat_model.md`](spec/threat_model.md) | Canonical protocol-level threat model |

This project treats the following as adversarial by default:

- Future maintainers
- Feature creep
- Accidental optimization
- Metadata correlation
- Retroactive reinterpretation of data

## Reporting issues

If you discover a way to violate an invariant, bypass an enforcement point,
or extract information the system claims is impossible to obtain:

**That is a security issue.**

### If it is exploitable against a device someone is relying on — report it privately

Use one of these, **not** a public issue:

- **[GitHub private vulnerability reporting](https://github.com/kmay89/securaCV/security/advisories/new)** — preferred; it gives us a private fork to fix in and issues the CVE if one is warranted.
- **errerlabs@gmail.com** — if you'd rather not use GitHub. Put `security` in the subject. We don't publish a PGP key today; if you need an encrypted channel, say so in a first mail with no details in it and we'll arrange one.

A public issue is a disclosure. For anything a stranger could turn around and
use against a live Canary — key extraction, an auth bypass, a way to forge a
record that verifies — filing publicly hands it to them before there is a fix.
That matters more here than in most projects: the people this system is
designed to protect are, by construction, people someone is actively trying to
watch. **Please don't do the disclosure for them.**

Everything else — a hardening idea, a doc that overstates a guarantee, a lint
that should exist, a finding against a device only you own — is welcome in the
open, and the structured form is the best place for it:

- [Security Report form](.github/ISSUE_TEMPLATE/security_report.yml)

Either way, please include:
- A minimal reproduction
- The invariant you believe is violated
- Whether the failure is compile-time or runtime

### What to expect back

This is a small project, so these are honest commitments rather than
enterprise-grade ones:

| | |
|---|---|
| **First human response** | Within 5 days. If you haven't heard anything in 10, assume the mail went astray and ping the other channel. |
| **Assessment and a plan** | Within 14 days of that first response — including "we don't consider this a vulnerability, and here's why," which you're free to disagree with publicly. |
| **Fix or a written reason it isn't fixed** | 90 days for anything exploitable. If we need longer we will say so and why, before the 90 days are up, not after. |
| **Disclosure** | Whenever you like after 90 days, with or without our agreement. Report first, coordinate if you can, but the clock is yours to run out — we would rather be embarrassed on schedule than have a real hole sit quiet. |
| **Credit** | Named in the advisory and the release notes, unless you'd rather not be. |

There is no bug bounty. We can't afford one, and saying so plainly beats
implying one exists.

### Safe harbor

If you are researching in good faith under this policy, we will not pursue or
support legal action against you, and we will say so on the record if someone
else does. Good faith means: work against **your own devices**, don't access
or exfiltrate anyone else's data, don't degrade a service anyone is depending
on, and give us the reporting window above before going public.

If a live-device test would put someone else's evidence or safety at risk,
stop and describe the attack instead — a written explanation of a plausible
break is worth more to us than a demonstration that cost somebody their
recording.

### Testing scope and rules of engagement

For structured security testing — what's in scope, what a tester is handed,
what is explicitly out of bounds, and what counts as a finding —
see [`docs/security/PENTEST_SCOPE.md`](docs/security/PENTEST_SCOPE.md).

## Known dependency advisories

Transitive dependencies occasionally carry published advisories that are
reachable in the crate but **not in any path securaCV actually executes**.
These are tracked here rather than silently dismissed, so an auditor can
verify the reasoning. Each entry records why securaCV is not exposed and
either the fix applied or why a bump is not yet possible.

> Verification done with the live crates.io index on 2026-05-29. All five
> open Dependabot alerts were transitive and are now **resolved** by the two
> dependency migrations recorded below.

### Group 1 — `rustls-webpki 0.102.8` (1 High, 1 Moderate, 2 Low) — RESOLVED

Four alerts, all on the same crate version: `rustls-webpki 0.102.8`, pulled
transitively by `rumqttc 0.25.1` (the MQTT-over-TLS client). A second copy,
`rustls-webpki 0.103.13` (via `rustls 0.23`), was already on the patched line.

- **High** — DoS via panic on a malformed CRL `BIT STRING`. `bit_string_flags()`
  in `der.rs` underflows on content `[0x00]`, reachable through
  `BorrowedCertRevocationList::from_der()`.
- **Moderate** — `GHSA-pwjx-qhcg-rvj4`: CRLs not considered authoritative by
  distribution point due to faulty matching logic.
- **Low** — name constraints for URI names were incorrectly accepted.
- **Low** — name constraints were accepted for certificates asserting a
  wildcard name.

**Why securaCV was not exposed:** every one of these lives in CRL-revocation
or name-constraint code. The CRL paths run only when an application opts in by
passing `RevocationOptions` to `verify_for_usage()`; the name-constraint paths
run only *after* signature verification and require a misissued certificate
from a trusted CA. securaCV used none of this — there is no `RevocationOptions`,
`CertRevocationList`, or direct `webpki` use anywhere in `src/`, and the MQTT
client performs standard server-certificate verification only.

**Fix applied:** there is no fixed `0.102.x` release and `rumqttc 0.25.1` (the
last release of that crate) hard-pins `rustls-webpki = "0.102"`, so the only
way to drop the vulnerable copy was to move off `rumqttc 0.25`. Migrated to its
maintained successor, **`rumqttc-next 0.33`** (re-exports `rumqttc-v5-next`),
which depends on the patched `rustls-webpki 0.103.13`. `rustls-webpki 0.102.8`
is now absent from `Cargo.lock` entirely. We were already on rumqttc's v5 API,
so this was a v5→v5 port (namespace, `MqttOptions::new`/`Broker`,
`set_keep_alive(u16)`, `ClientBuilder`, `Transport::Tls`). Build-verified with
`cargo check` across the default bins, the `adapter_host` feature set, and
`pqc-tls`.

### Group 2 — `time` stack exhaustion (Moderate) — RESOLVED

- **Bug:** parsing attacker-controlled input with the RFC 2822 format can
  exhaust the stack via deeply nested, deprecated format features. Fixed in
  `time 0.3.47`.
- **Where it came from:** `time` is a **build-time dependency only**. It was
  pulled by `liquid` (a templating engine), which is itself a
  `[build-dependencies]` of `tract-linalg` — the codegen step of the
  `tract-onnx` inference engine, behind the optional, non-default
  `backend-tract` feature.
- **Why securaCV was never exposed:** `time` never shipped in any runtime
  binary — it ran only during compilation of `tract-linalg`'s build script,
  which parses no untrusted input. (The runtime code in `src/` uses only
  `std::time`, never the `time` crate.)
- **Fix applied:** bumped `tract-onnx` `0.21 → 0.22` (stable `0.22.1`), which
  drops the old `time < 0.3.42` ceiling and lets `time` resolve to `0.3.47`.
  Verified with `cargo check --tests --features backend-tract` — the
  `backend-tract` detector (`src/detect/backends/tract.rs`) compiles unchanged
  against the new major version.

### Group 3 — `rsa 0.9.10` (RUSTSEC-2023-0071, Medium) — NOT REACHABLE, IGNORED

- **Bug:** the Marvin Attack. `rsa`'s private-key operations are not
  constant-time, so an attacker who can submit ciphertexts and measure how
  long the victim takes to process them can recover the RSA **private key**.
  CVSS 5.9 (medium).
- **Where it comes from:** exactly one crate — `c2pa` (0.90.3 when this
  analysis was written; the whole 0.90.x line behaves the same), the Content
  Credentials SDK behind the optional, non-default `c2pa-export` feature.
  `c2pa` requires `rsa ^0.9.10` as a **non-optional** dependency (it must be
  able to *verify* manifests signed with RSA-PSS by other producers), so no
  feature flag of ours drops it from `Cargo.lock`.
- **Why securaCV is not exposed:** the attack recovers an RSA private key by
  timing repeated private-key operations, and **securaCV holds no RSA private
  key at all.** The C2PA credential chain is Ed25519 end to end — the CA and
  the signing leaf are `PKCS_ED25519` certificates derived from the device
  seed, and every manifest is signed with `SigningAlg::Ed25519`
  (`src/c2pa_export.rs`). The only way `rsa` code can run is verifying a
  third-party RSA-signed manifest, which is a *public*-key operation: there
  is no secret in the process for a timing sidechannel to leak. On top of
  that, `c2pa` is compiled in only when someone opts into `c2pa-export`; it
  is not in the default feature set.
- **Fix not yet possible:** there is no fixed release on the `0.9` line. The
  constant-time rewrite lands in `rsa 0.10` (still a release candidate when
  this analysis was written), and `c2pa` still pins `rsa ^0.9.10` on its
  current `0.90.x` line (`0.90.16` in `Cargo.lock` today). Nothing we can
  bump changes the resolved version. The real fix is upstream: `c2pa`
  adopting `rsa 0.10` once it ships stable.
- **What we did instead:** the advisory is ignored explicitly and in the open,
  in [`.cargo/audit.toml`](.cargo/audit.toml), with this analysis as its
  justification. It is an entry with a name on it, not a silenced gate —
  **delete it the moment `c2pa` ships a release that resolves `rsa` past the
  advisory.** Everything else `cargo audit` reports stays fatal.

### Status

The five alerts in Groups 1 and 2 are resolved by dependency migrations (no
securaCV code was ever on a reachable path). Both vulnerable crate versions —
`rustls-webpki 0.102.8` and `time 0.3.41` — are absent from `Cargo.lock`.
Group 3 has no reachable fix and is ignored under the reasoning recorded
above. This section is retained as an audit record; remove entries once the
corresponding Dependabot alerts are closed, or — for Group 3 — once an
upstream fix lets the ignore go.

## Non-goals

The following are explicitly out of scope:

- Adding identity or biometric features
- Making guarantees optional or configurable
- Retroactively enabling new capabilities on historical data

If you need any of the above, you are building a different system.
