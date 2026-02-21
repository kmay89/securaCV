# SecuraCV — Security Audit Report (v2)

**Date:** 2026-02-21
**Revision:** 2 — deep hardening pass (research-grade)
**Scope:** Full-stack audit — Rust kernel (every `.rs` file), firmware (C/C++), Node.js device API, SPA, Docker, CI/CD, supply chain, cryptographic correctness, injection surfaces, side channels
**Methodology:** Line-by-line manual audit of every source file, dependency review, threat modeling against OWASP Top 10, STRIDE, and privacy-specific threat models

---

## Executive Summary

SecuraCV demonstrates **exceptional security engineering** for a project at this stage. The seven kernel invariants are enforced by construction. Cryptographic primitives are modern (Ed25519, ChaCha20-Poly1305, optional post-quantum Dilithium2/Kyber768). The Rust type system prevents entire classes of memory-safety vulnerabilities. The break-glass quorum system is correctly implemented with duplicate-approval protection, approval count caps, and receipt chaining.

This second audit pass focused on **subtle side-channel attacks, OOM/DoS vectors, information leakage, and supply chain integrity** — the class of issues that distinguish deployable infrastructure from research prototypes.

**All findings have been remediated in this commit.**

---

## Phase 2 Findings & Remediations

### HIGH — Fixed

| # | Finding | Location | Fix Applied |
|---|---------|----------|-------------|
| H-5 | **Timing oracle in Rust API token validation** — `CapabilityTokenManager::validate` used `!=` on `[u8; 32]`, which short-circuits on the first differing byte. An attacker measuring response times could extract the token byte-by-byte over ~256×32 = 8192 requests. | `src/api/mod.rs:130` | Replaced with `subtle::ConstantTimeEq::ct_eq()`. Added `subtle = "2.5"` as direct dependency (already a transitive dep via `ed25519-dalek`). |
| H-6 | **Sandbox IPC payload unbounded allocation (OOM DoS)** — Parent process reads a `u64` length from the child sandbox via pipe and allocates `vec![0u8; len]`. A corrupted/malicious child could send `u64::MAX` as the length, causing immediate OOM crash of the kernel process. | `src/module_runtime/sandbox.rs:204` | Added 16 MiB cap (`MAX_SANDBOX_PAYLOAD`). Rejects with error and cleans up child process on overflow. |
| H-7 | **API token logged to stderr on rotation without token_path** — When `WITNESS_API_TOKEN_PATH` is not set and the token rotates, the new token hex was logged via `log::warn!`. Log output can be captured by log aggregators, systemd journal, container runtimes, and process monitoring tools. | `src/api/mod.rs:373-374` | Removed token value from log. Now logs a warning that the token rotated but is not accessible without the path configured. |
| H-8 | **OTA cert verification bypass has no release-build guard** — `SECURACV_OTA_SKIP_CERT_VERIFY` disables TLS certificate verification for OTA downloads. If accidentally set in a production build, firmware updates are vulnerable to MITM interception and replacement. The macro was only gated by developer discipline. | `firmware/projects/canary-ota/components/securacv_ota/securacv_ota.c` | Added `#error` compile-time guard: cert-skip now requires `SECURACV_DEBUG_BUILD` to be defined. Production builds will fail to compile if cert-skip is set. Added `SECURACV_DEBUG_BUILD=1` to dev/test platformio environments. |

### MEDIUM — Fixed

| # | Finding | Location | Fix Applied |
|---|---------|----------|-------------|
| M-3 | **SQL identifier injection surface in `ensure_columns`** — Table and column names are interpolated into DDL via `format!()`. While current callers pass hardcoded string literals, the function signature accepts any `&str`, creating a latent injection surface if ever called with dynamic input. | `src/storage.rs:136` | Added `validate_sql_identifier()` guard: rejects empty, >64 char, digit-leading, or non-`[a-zA-Z0-9_]` identifiers before interpolation. |
| M-4 | **Raw MAC address exposed in PNA response header** — `Private-Network-Access-ID` header contained the device's actual MAC address, enabling device fingerprinting and tracking across browser sessions. | `canary-vision/device-api/middleware/pna.js:13` | Replaced with HMAC-SHA256 truncation of the MAC. Produces a stable 16-hex-char identifier (browsers can still distinguish devices) without leaking the real hardware address. |
| M-5 | **Missing Permissions-Policy header** — Both the Rust API and Node.js device API lacked `Permissions-Policy`, leaving browser features (camera, microphone, geolocation) available to injected scripts via XSS. | `src/api/mod.rs`, `canary-vision/device-api/middleware/security-headers.js` | Added `Permissions-Policy: camera=(), microphone=(), geolocation=()` to both APIs. |
| M-6 | **Dockerfile used `USER witness` (name-based) instead of numeric UID** — Container runtimes that don't propagate `/etc/passwd` into the container could fail to resolve the username. | `Dockerfile` | Changed to `USER 1001:1001`. Added `HEALTHCHECK` instruction. |

---

## Phase 1 Findings (Previously Fixed — Verified Still Intact)

### CRITICAL — Fixed (verified)

| # | Finding | Status |
|---|---------|--------|
| C-1 | MQTT broker anonymous access | Fixed: `allow_anonymous false` + password file |
| C-2 | DEVICE_KEY_SEED in environment variables | Fixed: Docker secrets |

### HIGH — Fixed (verified)

| # | Finding | Status |
|---|---------|--------|
| H-1 | API token logged to stdout (Node.js) | Fixed: written to file with 0o600 |
| H-2 | Docker container ran as root | Fixed: non-root user |
| H-3 | CI secrets committed to repo | Fixed: deleted, generated dynamically |
| H-4 | No secret scanning in CI | Fixed: TruffleHog workflow |

---

## Verified Secure — Full Audit Detail

### Kernel Cryptographic Correctness

- **Ed25519 signatures**: Domain-separated via `DOMAIN_*` constants (sealed log, break-glass receipt, export receipt, break-glass token). Prevents cross-domain signature confusion attacks.
- **Signature verification**: Uses `ed25519-dalek` `Verifier::verify()` — no custom verification code.
- **Post-quantum hybrid**: Optional Dilithium2 signatures stored alongside Ed25519, enabling future migration without breaking existing chains.
- **Vault encryption**: ChaCha20-Poly1305 with random nonce per envelope. V2 format includes length-prefixed AAD to prevent ambiguity. V1 AAD concatenation is documented and preserved for backwards compatibility.
- **Key derivation**: Device signing key derived from seed via SHA-256 with a domain separator. Seed is validated to not be the MVP placeholder value.
- **Ephemeral correlation tokens**: Per-bucket HMAC keys with `Zeroizing<[u8; 32]>` wrapper. Keys are destroyed on bucket rotation. `export_key_for_test_only()` is gated by `#[cfg(test)]`.

### Break-Glass Quorum System

- **Duplicate approval rejection**: Same trustee counted only once via `HashSet` deduplication.
- **Unknown trustee detection**: Approvals from non-policy trustees trigger explicit denial (not silent ignore).
- **Approval count cap**: `MAX_APPROVALS = 64` prevents DoS via excessive approval processing.
- **Trustee count cap**: `MAX_TRUSTEES = 32` prevents DoS via excessive policy size.
- **Token single-use**: `consumed` flag prevents token replay. Checked in `assert_token_valid`.
- **Receipt chaining**: Break-glass receipts form a hash chain with Ed25519 + optional PQ signatures.
- **Receipt hash binding**: Token is bound to receipt hash after authorization, preventing receipt substitution.

### Sandbox Security (seccomp)

- **Denylist coverage**: 41 syscalls blocked (filesystem, network, directory ops). Covers `openat2`, `faccessat2`, and other newer syscalls.
- **PR_SET_NO_NEW_PRIVS**: Set before filter installation — prevents privilege escalation.
- **libseccomp**: Uses portable syscall-name mapping (works across x86_64 and aarch64).
- **Non-Linux fallback**: Returns conformance error instead of silently running unsandboxed.

### Sealed Log Integrity

- **Hash chain**: SHA-256 chain with `prev_hash → entry_hash` linking. Genesis is `[0; 32]`.
- **Tagged serialization**: `SealedLogRecord` uses `#[serde(tag = "record_type")]` to prevent type confusion. Legacy untagged format supported for backward compatibility.
- **Reprocess guard**: `ReprocessGuard::assert_same_ruleset` prevents applying new rulesets to historical data.
- **Failure semantics**: Storage and crypto failures produce explicit `FailureEvent` records. Graceful degradation chain: sealed log → conformance_alarms table → stderr.
- **Retention checkpoints**: Pruning creates signed checkpoint entries preserving chain continuity.

### Node.js Device API

- **Middleware ordering**: Security headers → host validation → rate limiting → static files → PNA → CORS → JSON parsing → auth → routes. Correct and defense-in-depth.
- **CORS**: Peer device origins explicitly rejected (`corsMiddleware` blocks cross-origin API access).
- **Rate limiting**: Per-IP with exponential backoff and auth-failure escalation.
- **Trust proxy**: Explicitly set to `false` — prevents IP spoofing via `X-Forwarded-For`.
- **CSP**: Self-only with private-network `connect-src` allowlist for device discovery.
- **Immutable privacy keys**: `camera_peek_enabled` cannot be modified via config API.
- **Input validation**: Motion sensitivity (1-10), MQTT port (1-65535), SSID non-empty. Config changes bounded.
- **Reboot rate limiting**: 5-minute minimum interval prevents reboot DoS.

### Firmware Authentication

- **Constant-time comparison**: `volatile uint8_t` XOR accumulator pattern. Correct implementation.
- **Exponential backoff**: 2^n × 2000ms base, capped at 300s. Failure window auto-resets after 60s clean.
- **Token redaction**: Only 8-char prefix logged, never full token.
- **Optional auth**: `api_auth_check_optional` for provisioning-receipt endpoint returns bool without sending error response.
- **Buffer bounds**: Auth header capped at 128 bytes, preventing stack overflow.

### Supply Chain

- **`subtle` (newly added)**: v2.5 — well-audited constant-time primitives crate, already a transitive dependency via `curve25519-dalek` → `ed25519-dalek`. Zero new attack surface.
- **Vendored `indicatif`**: Local path dependency, source in `vendor/indicatif`. Not fetched from registry at build time.
- **TruffleHog CI**: Scans for verified secrets on every PR to main.
- **Cargo.lock committed**: Reproducible builds.

---

## Remaining Recommendations (Not Blocking)

| Priority | Recommendation | Rationale |
|----------|---------------|-----------|
| Medium | **Database encryption at rest** (SQLCipher) | Defense-in-depth if device storage is physically compromised |
| Medium | **SBOM generation** in CI (`cargo-sbom`) | Supply chain transparency for auditors |
| Medium | **Firmware signature verification** in Node.js update route | Currently stubbed (`routes/update.js:68`) — reference server only |
| Medium | **Firmware downgrade protection** | OTA accepts any version newer than minimum; no anti-rollback to older-but-valid versions |
| Low | **TLS certificate auto-generation** for local API | Self-signed cert fallback to reduce plaintext friction |
| Low | **Token expiration in sessionStorage** | SPA tokens survive page refresh indefinitely (cleared on tab close) |
| Low | **Signed commits/tags** enforcement in CI | Chain of custody for release artifacts |

---

## Alignment with Design Goals

This audit was conducted at the stakes described: **public infrastructure for dignity**.

- **"Witnessing is not watching"** — Every export path is gated by break-glass quorum. No API endpoint leaks raw media. Correlation tokens are ephemeral (keys destroyed per time bucket). Zone policy suppresses sensitive areas. The seven invariants hold under adversarial analysis.

- **No backdoors by construction** — The timing oracle fix (H-5) was the most subtle finding: a `!=` operator on byte arrays is safe in most contexts but creates a measurable side channel when the comparison controls authentication. The fix uses `subtle::ConstantTimeEq`, the same primitive used by TLS libraries and SSH implementations.

- **Fail-closed everywhere** — The sandbox OOM fix (H-6) ensures a corrupted child process cannot crash the parent kernel. The OTA cert-skip guard (H-8) ensures a build misconfiguration cannot silently disable TLS verification. The SQL identifier validation (M-3) ensures a latent injection surface cannot become exploitable.

- **No bloat** — Every fix is minimal. `subtle` was already a transitive dependency. The PNA MAC hash uses only Node's built-in `crypto`. The SQL identifier check is 12 lines. No new frameworks, no new abstractions.

---

## Files Modified in This Hardening Pass

| File | Change |
|------|--------|
| `Cargo.toml` | Added `subtle = "2.5"` for constant-time comparison |
| `src/api/mod.rs` | Constant-time token validation, removed token logging, added `Permissions-Policy` header |
| `src/module_runtime/sandbox.rs` | 16 MiB cap on IPC payload length |
| `src/storage.rs` | SQL identifier validation in `ensure_columns` |
| `canary-vision/device-api/middleware/pna.js` | Hashed MAC address in PNA ID header |
| `canary-vision/device-api/middleware/security-headers.js` | Added `Permissions-Policy` header |
| `canary-vision/spa/app.js` | Security annotation for HTTP LAN access pattern |
| `firmware/projects/canary-ota/components/securacv_ota/securacv_ota.c` | Compile-time guard against cert-skip in release builds |
| `firmware/projects/canary-ota/platformio.ini` | Added `SECURACV_DEBUG_BUILD` to dev/test environments |
| `Dockerfile` | Numeric UID, healthcheck |
| `SECURITY-AUDIT.md` | This report (v2) |
