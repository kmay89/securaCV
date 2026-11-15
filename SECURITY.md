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

### The website is in scope too

`securacv.com` names this file as its policy
(`/.well-known/security.txt`), because the website's own repository is not
public and a policy nobody can open is not a policy. Report a website issue
through the same two private channels above. The site is static — no
accounts, no credentials, no server-side code — so the interesting surface is
content injection, its Content Security Policy and its response headers; the
same applies to the in-browser Lab this repository publishes on GitHub Pages
(`canary-local/`).

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
- **Where it comes from:** exactly one crate — `c2pa`, the Content
  Credentials SDK behind the optional, non-default `c2pa-export` feature.
  Through 0.90.x (0.90.3 when this analysis was written) `c2pa` required
  `rsa ^0.9.10` itself, as a **non-optional** dependency. From 0.91.0 its
  crypto lives in a crate of its own, `c2pa_raw_crypto` (0.1.2 in
  `Cargo.lock`), whose `rust_native_crypto` feature requires `rsa ^0.9.10`;
  that feature is the pure-Rust backend `c2pa-export` selects, and the only
  other backend `c2pa` offers is OpenSSL's. Either way `rsa` is there so
  `c2pa` can *verify* manifests signed with RSA-PSS by other producers, and
  no feature flag of ours drops it from `Cargo.lock`.
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
  this analysis was written, and at `0.10.0-rc.18` when `c2pa` 0.91.0 was
  taken), and `c2pa_raw_crypto` 0.1.2, which `c2pa` 0.91.0 requires, still
  asks for `rsa ^0.9.10`. Nothing we can bump changes the resolved version.
  The real fix is upstream: `c2pa`'s crypto crate adopting `rsa 0.10` once
  it ships stable.
- **What we did instead:** the advisory is ignored explicitly and in the open,
  in [`.cargo/audit.toml`](.cargo/audit.toml), with this analysis as its
  justification. It is an entry with a name on it, not a silenced gate —
  **delete it the moment `c2pa` ships a release that resolves `rsa` past the
  advisory.** Everything else `cargo audit` reports stays fatal.

### Group 4 — `zip` extraction (RUSTSEC-2026-0367, -0368, -0369, -0429) — FIXED everywhere

- **Bug:** three advisories published 2026-10 against every `zip` release
  before 9.0.0, all in extraction: `ZipArchive::extract` creates symbolic
  links whose targets lie outside the destination (0368), `extract()` applies
  Unix permissions through symlinks without confining the target (0369), and
  extraction keeps SUID/SGID bits (0367). Each needs an attacker-made archive
  that the program then extracts.
- **The kernel's `Cargo.lock`: fixed.** `zip` came from `c2pa` (the optional
  `c2pa-export` feature) at 8.6.0. `c2pa` 0.91.3 requires `zip ^9.0.4`, so
  the lockfile took `c2pa` 0.91.3 and `zip` 9.0.5 (#1802); it now carries
  `c2pa` 0.91.6, whose `c2pa_raw_crypto` 0.2 moves to `elliptic-curve` 0.14
  and so takes the unfixable `crypto-bigint` 0.5 line off the tree
  (RUSTSEC-2026-0387, the 2026-10-26 audit). `c2pa` 0.91.x declares
  Rust 1.96; CI builds on current stable.
- **The Flasher's and the Lab's `Cargo.lock`
  (`desktop/src-tauri`, `desktop-lab/src-tauri`): not reachable.** `zip`
  4.6.1 comes from exactly one crate, `tauri-plugin-updater`, as an optional,
  **Windows-only** dependency of its `zip` feature. Its one use is
  `extract_zip()`, which unpacks a Windows update archive into a fresh
  temporary directory to find the `.exe` or `.msi` inside. Both apps reach it
  only through `Update::download()` followed by `Update::install()` on the
  bytes `download()` returned (`desktop/src-tauri/src/lib.rs`
  `install_update`, `desktop-lab/src-tauri/src/self_update.rs`), and
  `download()` refuses any archive whose minisign signature does not verify
  against the public key pinned in each app's `tauri.conf.json` before it
  returns a byte. No webview grant reaches the updater (both capability files
  say so). So the archive `zip` opens is one we signed: an attacker who could
  make it would hold the release signing key, and with that key could ship any
  installer at all, which the extraction flaws add nothing to. On macOS and
  Linux the crate is not compiled in.
- **Fixed for the desktop apps on 2026-11-02.** `tauri-plugin-updater`
  2.14.1 requires `zip ^9`, so both lockfiles took the updater bump and `zip`
  9.2.2 (`cargo update -p tauri-plugin-updater` in each crate; the crate
  requirement `tauri-plugin-updater = "2"` was already loose enough). That
  also clears RUSTSEC-2026-0429 (7.5 high, published 2026-10-22: `extract`
  aborts the process on a symlink entry with a large declared size — the same
  signed-archive-only path as the three above), which the 2026-11-02
  scheduled audit was the first to fail on, since it was never on the ignore
  list. The three ignore entries are gone from
  [`.cargo/audit.toml`](.cargo/audit.toml); every lockfile's audit now reads
  only the Group 3 entry. The reachability analysis above is kept as the
  record of why the ignore was sound while it stood.

### Status

The five alerts in Groups 1 and 2 are resolved by dependency migrations (no
securaCV code was ever on a reachable path). Both vulnerable crate versions —
`rustls-webpki 0.102.8` and `time 0.3.41` — are absent from `Cargo.lock`.
Group 3 has no reachable fix and is ignored under the reasoning recorded
above. Group 4 is fixed by bumps in every lockfile that carried `zip`, and its
ignore entries are gone. This section is retained as an audit record; remove entries once the
corresponding Dependabot alerts are closed, or — for Group 3 — once an
upstream fix lets the ignore go.

### Group 5 — SQLite WAL page-size read (CVE-2026-119375; RUSTSEC-2026-0502, earlier RUSTSEC-2026-0459) — RESOLVED

- **Bug:** CVE-2026-119375. SQLite before 3.54.0 can return uninitialized
  page-cache memory when a process opens a database together with a sibling
  `-wal` file whose page size differs from the database's. A crafted pair of
  files makes ordinary queries return stale bytes from the process's memory.
  The vector is local and the victim has to open the files. CVSS 4.0: 6.8.
- **Where it came from:** `rusqlite`'s `bundled-sqlcipher` feature, which
  the kernel needs for its encrypted database. `libsqlite3-sys` 0.39.1
  built SQLCipher from `sqlcipher-src`, pinned to `=419.0.6`: SQLCipher
  4.19.0 on SQLite 3.53.4. The plain bundled SQLite in that release was
  already 3.54.1; the SQLCipher build was not.
- **The two advisory ids:** RUSTSEC-2026-0459 was keyed on the
  `libsqlite3-sys` version, so `cargo audit` passed from the `rusqlite` 0.41
  upgrade on while the SQLCipher build still carried SQLite 3.53.4 (this
  section said so at the time). RUSTSEC-2026-0502 names `sqlcipher-src`
  itself, at the 419.0.6 the root `Cargo.lock` carried, and failed the audit
  on that entry. The lockfile now carries 420.0.0.
- **Fix:** `rusqlite` 0.43.0, on `libsqlite3-sys` 0.40.0, which pins
  `sqlcipher-src` `=420.0.0`: SQLCipher 4.20.0 on SQLite 3.54.1. Checked
  against the live crates.io index on 2026-11-15: 0.40.0 is the first
  `libsqlite3-sys` release that selects `sqlcipher-src` 420 (0.39.0 and
  0.39.1 pin `=419.0.6`), and `rusqlite` 0.42.0 and 0.43.0 are the releases
  that use it. Cipher defaults are unchanged: checked once by hand on
  2026-11-15, by diffing the SQLCipher part of the amalgamation between
  4.19.0 and 4.20.0 (only the version number, a wasm-only guard and the
  error-path exits of `cipher_migrate` differ) and by comparing
  `PRAGMA cipher_settings` and `cipher_default_settings` from both builds.
- **What keeps it fixed:** the unit test
  `tests::linked_sqlite_is_at_least_3_54_0` (`src/lib.rs`) asks
  `sqlite_version()` through a kernel connection and fails below 3.54.0, so
  the check is on the library the kernel runs, not on a crate version. It was
  run once by hand against the old lockfile (2026-11-15) and failed there on
  3.53.4, as it should. Beside it,
  `tests::kernel_database_cipher_settings_are_pinned` pins the SQLCipher
  defaults a kernel database is written with, as far as SQLCipher reports
  them: the six rows of `PRAGMA cipher_settings`, plus the cipher, the
  iteration count that derives the HMAC key, the page-number byte order in
  the HMAC and the HMAC salt mask. A release that changes one of those fails
  it; it passed against both the old and the new lockfile (by hand,
  2026-11-15). Not pinned, because no pragma reports them: the 16-byte salt
  header and the key and IV sizes. A release that changed one of those would
  pass the test, and no standing test opens a file written by another
  SQLCipher version.
- **One fixture database, opened across the version boundary:** checked
  once by hand on 2026-11-15, not by a standing test. The conformance
  fixture database written by the old build passed the new `log_verify`
  (VALID), and one written by the new build passed the old `log_verify`
  (VALID). The fixture holds one sealed event, one break-glass grant and its
  redemption, and one export receipt. `log_verify` was the only program
  that opened the other version's file. Not covered: a pending `-wal`
  written by the other version (the fixture is in WAL journal mode, but no
  `-wal` file was left beside it), the TSA and OpenTimestamps anchor tables
  and their readers (absent from the fixture), checkpoint rows (the table
  was empty), clock-provenance records (there were none), a rotated device
  key, a database rekeyed with `rekey_database_file`, a database migrated
  from plaintext, and every other reader (`witnessd`, `export_verify`,
  `court_export`, `log_anchor`, `break_glass`).
- **Builds made before this change remain exposed.** Every kernel binary
  built with an earlier `rusqlite` (SQLite 3.53.4 under `rusqlite` 0.41,
  3.39.4 before it) carries the bug. For those builds the advice stands:
  only a process that opens a database, with its `-wal`, that a less trusted
  party could have written is exposed. `witnessd` and the bridges open their
  own database in their own data directory; whoever can plant a `-wal` there
  can already rewrite the log's files. The verifier and export commands
  (`log_verify`, `export_verify`, `court_export`, `log_anchor`,
  `break_glass`) open the database they are pointed at and hold key material
  while they do, so the exposed case is running one of them on a database
  directory received from someone else. With such a build, check received
  evidence as an envelope (`envelope_verify`, the offline viewer), which
  reads JSON and never opens SQLite, and do not open a received database
  that arrives with a `-wal` file beside it.

### Group 6 — `quinn-udp` receive loop (RUSTSEC-2026-0538) — RESOLVED

- **Advisory:** RUSTSEC-2026-0538, "Receive loop can spin after a truncated
  datagram", against `quinn-udp` 0.5.15; patched in 0.5.16. Nothing beyond
  that title is described here.
- **Where it came from:** the lockfiles of the Flasher
  (`desktop/src-tauri`), the Lab app (`desktop-lab/src-tauri`) and the flash
  engine (`desktop/flash-engine`). `quinn-udp` sits under `quinn` 0.11.11,
  an optional dependency of `reqwest` 0.12.28 that only `reqwest`'s `http3`
  feature (its HTTP/3 client over QUIC) turns on. `reqwest` 0.12.28 is
  reached from `flash-engine` and, in the two apps, also from the app crate
  itself (`securacv-flasher` in `desktop/src-tauri`, `securacv-lab` in
  `desktop-lab/src-tauri`). `tauri` 2.12.5 and `tauri-plugin-updater`
  2.14.1 depend on `reqwest` 0.13.4 instead, which has no `quinn` edge in
  these lockfiles.
- **Compiled in? No.** No crate in the three graphs turns on `http3`; the
  lockfiles list `quinn` because `reqwest`'s `__rustls-ring` feature names
  `quinn?/ring`, a reference that does not enable the dependency.
  `cargo tree --locked --target all -i quinn-udp` prints nothing in each of
  the three crates, with default features and with `--all-features`
  (2026-11-15). The audit reads the lockfile, so it failed all the same.
- **Fix:** `quinn-udp` 0.5.16 in all three lockfiles. Only that package
  entry (version and checksum) changed; each lockfile passes
  `cargo metadata --locked`.

## Non-goals

The following are explicitly out of scope:

- Adding identity or biometric features
- Making guarantees optional or configurable
- Retroactively enabling new capabilities on historical data

If you need any of the above, you are building a different system.
