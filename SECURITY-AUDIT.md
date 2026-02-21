# SecuraCV — Security Audit Report

**Date:** 2026-02-21
**Scope:** Full-stack red team audit — Rust kernel, firmware (C/C++), Node.js device API, Docker infrastructure, CI/CD, secrets management, structural integrity
**Methodology:** Automated deep-scan of every source file, configuration, dependency, and route handler in the repository

---

## Executive Summary

SecuraCV demonstrates **strong security engineering fundamentals**. The seven kernel invariants are enforced by construction, not policy. Cryptographic primitives are modern (Ed25519, ChaCha20-Poly1305, optional post-quantum). The Rust type system prevents entire classes of memory-safety vulnerabilities. Input validation is thorough, error responses are generic, and timing-safe comparisons are used throughout.

**No critical code-level vulnerabilities were found in the kernel, API, or firmware authentication paths.**

The issues identified are **infrastructure and deployment hardening gaps** — the kind that distinguish a strong prototype from enterprise-grade, 501(c)(3)-trustworthy production software. All critical and high-priority findings have been remediated in this commit.

---

## Findings & Remediations

### CRITICAL — Fixed

| # | Finding | Location | Fix Applied |
|---|---------|----------|-------------|
| C-1 | **MQTT broker allowed anonymous access** — any device on the network could publish/subscribe to all topics, including witness events and Home Assistant discovery | `integrations/ha_frigate_mqtt/mosquitto.conf` | Disabled `allow_anonymous`, added `password_file` directive, documented TLS setup |
| C-2 | **DEVICE_KEY_SEED exposed as environment variable** — container environment is visible via `docker inspect`, process listing, and crash dumps | `integrations/ha_frigate_mqtt/docker-compose.yml` | Migrated to Docker secrets (`/run/secrets/device_key_seed`), seed loaded at runtime from mounted secret file |

### HIGH — Fixed

| # | Finding | Location | Fix Applied |
|---|---------|----------|-------------|
| H-1 | **API token logged to stdout in dev mode** — token visible in terminal scrollback, CI output, container logs | `canary-vision/device-api/server.js:88` | Token now written to file with `0o600` permissions instead of printed |
| H-2 | **Docker container runs as root** — unnecessary privilege escalation risk if container is compromised | `Dockerfile` | Added `witness` system user/group, `USER witness` directive before `ENTRYPOINT` |
| H-3 | **CI secrets committed to repo** — `secrets.ci.h` with placeholder credentials in version control | `firmware/projects/canary-vision/secrets/secrets.ci.h` | Deleted file. CI workflow now generates header dynamically in the build step |
| H-4 | **No automated secret scanning in CI** — relied solely on `.gitignore` to prevent credential leaks | `.github/workflows/` | Added `secrets-scan.yml` workflow with trufflehog and pattern-based detection |

### MEDIUM — Fixed

| # | Finding | Location | Fix Applied |
|---|---------|----------|-------------|
| M-1 | **Docker Compose ports bound to 0.0.0.0** — MQTT (1883), Frigate (5000), Home Assistant (8123) exposed to all network interfaces | `integrations/ha_frigate_mqtt/docker-compose.yml` | All ports now bound to `127.0.0.1` |
| M-2 | **`.device_key_seed` not in .gitignore** — new Docker secrets file could be accidentally committed | `.gitignore` | Added `.device_key_seed` to exclusion list |

---

## Verified Secure — No Action Required

These areas were audited and found to meet or exceed enterprise security standards:

### Kernel (Rust)

- **Memory safety**: Rust ownership model eliminates buffer overflows, use-after-free, and data races by construction
- **Minimal `unsafe`**: Single occurrence in `module_runtime/sandbox.rs` for syscall sandboxing — necessary and properly isolated
- **Zeroization**: `zeroize` crate used for all sensitive key material
- **Cryptography**: Ed25519 (dalek), ChaCha20-Poly1305, SHA-256, optional PQC (Dilithium2, Kyber768) — all current, well-audited implementations
- **Database**: Parameterized queries via rusqlite, file permissions enforced at 0o600
- **Build profile**: Release builds strip symbols, use LTO, `panic = "abort"` (reduces attack surface)

### API Server

- **Attack surface**: Only 3 read-only endpoints (`/health`, `/events`, `/events/latest`) — minimal by design
- **Authentication**: 32-byte cryptographically random tokens with 10-minute rotation
- **Rate limiting**: Per-IP exponential backoff (2s → 300s cap) after 5 failed attempts
- **Token handling**: Bearer header and `X-Witness-Token` supported; query parameter explicitly rejected to prevent URL logging
- **Loopback enforcement**: Rejects non-loopback clients when bound to loopback address
- **Security headers**: `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`
- **Error responses**: Generic messages only, no stack traces or internal state leaked
- **Request size limit**: 8192 bytes enforced

### Canary Vision Device API (Node.js)

- **Token generation**: Ed25519-based with `crypto.timingSafeEqual()` for constant-time comparison
- **Middleware stack**: Properly ordered — security headers → host validation → rate limiting → auth
- **CSP**: Restrictive Content-Security-Policy appropriate for local device operation
- **Host validation**: Validates Host header against device mDNS hostname and IP
- **`x-powered-by` disabled**: Framework fingerprinting prevented

### Firmware (C/C++)

- **Secure defaults**: `DEFAULT_TLS_REQUIRED=1`, `AUTH_MAX_FAILURES=5`, exponential backoff
- **Constant-time auth**: Hand-rolled volatile comparison in `api_auth.h`
- **Hardware RNG**: Device keys generated on-device, never exported
- **Secrets management**: `.gitignore` properly excludes `secrets.h` at all levels, allows `.example.h` templates

### Seven Kernel Invariants (Verified by Code and Tests)

1. **No Raw Export** — No API path streams raw media; pre-event buffer is transient and break-glass gated
2. **No Identity Substrate** — No face embeddings, plate strings, or global identifiers stored
3. **Metadata Minimization** — Coarse time buckets, local zone IDs
4. **Local Ownership** — All logs stored locally, no remote indexing
5. **Break-Glass by Quorum** — N-of-M trustee approval with receipt logging and approval caps
6. **No Retroactive Expansion** — New rulesets cannot reprocess old data
7. **Non-Queryable** — No bulk search or identity selectors

### Structural Integrity

- **No significant code duplication** — apparent duplications are intentional (feature-gated backends, platform-specific implementations, frozen snapshot baselines)
- **No circular dependencies** — all dependency flow is unidirectional
- **Consistent naming conventions** — `snake_case` (Rust), `camelCase` (JS), `PascalCase` (structs)
- **Clean module boundaries** — crypto vs vault vs storage vs ingest vs detect properly separated

---

## Remaining Recommendations (Not Blocking)

These are hardening suggestions for the roadmap, not blockers:

| Priority | Recommendation | Rationale |
|----------|---------------|-----------|
| Medium | **Database encryption at rest** (SQLCipher or equivalent) | Defense-in-depth if device storage is physically compromised |
| Medium | **SBOM generation** in CI | Supply chain transparency for enterprise adopters and auditors |
| Medium | **Windows file permission handling** for token files | Current `0o600` enforcement is Unix-only |
| Low | **TLS certificate auto-generation** for API | Self-signed cert as fallback when no cert is provided, reducing friction |
| Low | **Feature gate documentation** | Matrix showing which features enable which backends and dependencies |
| Low | **Signed commits/tags** enforcement in CI | Chain of custody for release artifacts |

---

## Alignment with Design Goals

This audit was conducted with explicit attention to the project's stated values:

- **"Witnessing is not watching"** — The kernel's seven invariants are enforced by code, not configuration. This audit verified that no export path bypasses break-glass, no identity substrate exists in any code path, and metadata minimization is applied consistently.

- **501(c)(3) trust standard** — The fixes applied here (authenticated MQTT, non-root containers, secret scanning, no committed credentials) bring the deployment infrastructure up to the same rigor as the kernel itself.

- **No bloat** — Every fix is minimal and targeted. No new frameworks, no unnecessary abstractions, no feature flags for things that should simply be secure by default.

- **Structural coherence** — The codebase has no meaningful duplication. The separation between kernel, firmware, device API, and integration layers is architecturally sound.

---

## Files Modified in This Audit

| File | Change |
|------|--------|
| `integrations/ha_frigate_mqtt/mosquitto.conf` | Disabled anonymous access, added password file, documented TLS |
| `integrations/ha_frigate_mqtt/docker-compose.yml` | Docker secrets for DEVICE_KEY_SEED, localhost-bound ports, MQTT auth volume |
| `integrations/ha_frigate_mqtt/README.md` | Updated setup instructions for authenticated MQTT and Docker secrets |
| `Dockerfile` | Added non-root `witness` user |
| `canary-vision/device-api/server.js` | Token written to file instead of stdout |
| `.github/workflows/firmware_canary_vision_ci.yml` | Generate secrets header dynamically instead of copying committed file |
| `.github/workflows/secrets-scan.yml` | New: automated secret scanning in CI |
| `.gitignore` | Added `.device_key_seed` exclusion |
| `firmware/projects/canary-vision/secrets/secrets.ci.h` | Deleted (no longer needed) |
