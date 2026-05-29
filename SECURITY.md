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

Please report it with:
- A minimal reproduction
- The invariant you believe is violated
- Whether the failure is compile-time or runtime

Use the GitHub issue form for structured reports:
- [Security Report](.github/ISSUE_TEMPLATE/security_report.yml)

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

### Status

All five alerts above are resolved by dependency migrations (no securaCV code
was ever on a reachable path). Both vulnerable crate versions —
`rustls-webpki 0.102.8` and `time 0.3.41` — are absent from `Cargo.lock`. This
section is retained as an audit record; remove entries once the corresponding
Dependabot alerts are closed.

## Non-goals

The following are explicitly out of scope:

- Adding identity or biometric features
- Making guarantees optional or configurable
- Retroactively enabling new capabilities on historical data

If you need any of the above, you are building a different system.
