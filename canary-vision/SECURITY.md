# Security Policy — Canary Vision

## Reporting a Vulnerability

If you discover a security vulnerability in Canary Vision, please report it
responsibly. **DO NOT open a public issue.**

### How to Report

1. **GitHub Security Advisory** (preferred):
   https://github.com/kmay88/securacv/security/advisories/new

2. **Email**: errerlabs@gmail.com

### What to Include

- Description of the vulnerability
- Steps to reproduce
- Potential impact
- Suggested fix (if you have one)

### Response Timeline

- **Acknowledgment**: Within 48 hours
- **Assessment**: Within 7 days
- **Fix timeline**: Depends on severity, target 30 days for critical

### Scope

In scope:
- Device API (authentication bypass, authorization flaws)
- DNS rebinding or CORS bypass
- SPA vulnerabilities (XSS, token leakage)
- Firmware update integrity bypass
- Witness chain manipulation
- Rate limiting bypass

Out of scope:
- Attacks requiring physical access to the device
- Attacks requiring existing access to the user's LAN
- Denial of service against a single device on the LAN
- Social engineering

### Security Architecture

See `docs/security.md` for the full threat model and security decisions.

### Philosophy

> The safest capabilities are the ones that don't exist.
