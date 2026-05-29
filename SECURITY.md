# Security Model

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
> open Dependabot alerts are transitive and fall into the two groups below:
> Group 1 (four `rustls-webpki` alerts) has no upstream fix yet; Group 2
> (`time`) is resolved by the dependency bump in this change.

### Group 1 — `rustls-webpki 0.102.8` (1 High, 1 Moderate, 2 Low)

Four alerts, all on the same crate version: `rustls-webpki 0.102.8`, pulled
transitively by `rumqttc 0.25.1` (the MQTT-over-TLS client). The other copy
in the tree, `rustls-webpki 0.103.13` (via `rustls 0.23`), is on the current
release.

- **High** — DoS via panic on a malformed CRL `BIT STRING`. `bit_string_flags()`
  in `der.rs` underflows on content `[0x00]`, reachable through
  `BorrowedCertRevocationList::from_der()`.
- **Moderate** — `GHSA-pwjx-qhcg-rvj4`: CRLs not considered authoritative by
  distribution point due to faulty matching logic.
- **Low** — name constraints for URI names were incorrectly accepted.
- **Low** — name constraints were accepted for certificates asserting a
  wildcard name.

**Why securaCV is not exposed:** every one of these lives in CRL-revocation
or name-constraint code. The CRL paths run only when an application opts in by
passing `RevocationOptions` to `verify_for_usage()`; the name-constraint paths
run only *after* signature verification and require a misissued certificate
from a trusted CA. securaCV uses none of this — there is no `RevocationOptions`,
`CertRevocationList`, or direct `webpki` use anywhere in `src/`, and `rumqttc`
performs standard server-certificate verification only. The vulnerable code is
never invoked.

**Why it isn't simply patched:** there is no fixed `0.102.x` release
(`0.102.8` is the newest on that line), and `rumqttc 0.25.1` (the latest
published version) hard-pins `rustls-webpki = "0.102"`. A `[patch.crates-io]`
override cannot redirect it to `0.103.x` — that crosses the semver-incompatible
`0.102 → 0.103` boundary, which cargo rejects.

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

### Tracking

- Watch `rumqttc` for a release that bumps its `rustls-webpki` dependency to
  the `0.103.x` line (clears Group 1).
- When it lands, bump, drop the Group 1 entry here, and re-run the audit.
  Until then those four alerts are dismissed as "vulnerable code not in the
  execution path."

## Non-goals

The following are explicitly out of scope:

- Adding identity or biometric features
- Making guarantees optional or configurable
- Retroactively enabling new capabilities on historical data

If you need any of the above, you are building a different system.
