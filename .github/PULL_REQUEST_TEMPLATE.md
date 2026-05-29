## What Changed

<!-- Brief description of changes -->

## Checklist

### Required
- [ ] Firmware compiles in **both** Arduino IDE and PlatformIO
- [ ] `regression_check.sh` passes with no errors
- [ ] No new hardcoded secrets or default passwords
- [ ] All new UI buttons have working API endpoints
- [ ] All new API endpoints return proper JSON with error codes

### Security Review (required for ALL changes)

> Reference: `docs/security/THREAT_MODEL.md` — The Ten Security Principles

#### Cryptographic Review
- [ ] No new cryptographic primitives introduced without justification
- [ ] No custom crypto implementations (use mbedTLS / Arduino Crypto only)
- [ ] No key material in logs, exports, SD, API responses (public keys OK)
- [ ] No downgrade paths (TLS → HTTP, signed → unsigned)
- [ ] Constant-time comparison for all secret-dependent operations

#### Privacy Review
- [ ] No new outbound network connections (Principle 2: Zero Phone-Home)
- [ ] No new identifier leaks — MAC, serial number, OUI (Principle 3)
- [ ] No raw biometric/location data stored without coarsening
- [ ] No new data collection without disclosure in `docs/security/SECURITY_MODEL.md`
- [ ] Presence detection: MACs hashed, no raw SSID storage

#### Trust Review
- [ ] No new trust assumptions introduced
- [ ] Evidence still verifiable without ERRERlabs (Principle 4)
- [ ] Device still functions fully offline
- [ ] User still has complete sovereignty over device and data (Principle 10)

#### Attack Surface Review
- [ ] No new compile-time features enabled by default
- [ ] No new network services exposed
- [ ] No new USB/Serial/JTAG interfaces in production
- [ ] Binary size delta justified
- [ ] New dependencies audited (source available, maintained, no known CVEs)

### If you touched crypto/auth:
- [ ] Token comparison uses constant-time function
- [ ] No raw key material in logs, SD, or witness chain
- [ ] Key derivation uses domain separation

### If you touched camera/GPS/SD:
- [ ] Correct XIAO ESP32S3 Sense pin definitions
- [ ] Graceful degradation if hardware absent

### If you touched web_ui.h:
- [ ] No `localStorage` / `sessionStorage` / `document.cookie`
- [ ] File size under 64KB
- [ ] Every new button has a backend handler

### If you weakened a secure default:
- [ ] Justification documented in this PR description
- [ ] Entry added to `firmware/LESSONS_LEARNED.md`
- [ ] Verified change does not affect the most vulnerable user class
- [ ] `docs/security/THREAT_MODEL.md` updated if threat model changed
- [ ] `docs/security/SECURITY_MODEL.md` updated if user-facing guarantees changed

### Lessons Learned
- [ ] If this PR fixes a bug, add an entry to `firmware/LESSONS_LEARNED.md`
- [ ] If this PR introduces a new pattern, document it in LESSONS_LEARNED
