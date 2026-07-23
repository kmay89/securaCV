# Canary Vision site security review — 2026-07-22

## Scope

Reviewed the local-first Canary Vision web surface and device API reference implementation:

- Express middleware ordering and route authentication.
- SPA storage, CSP posture, and private-network request model.
- Configuration mutation logic.
- Witness-event export and webhook integration paths.

## Threat model summary

The web UI is intentionally unauthenticated for static assets so a user can load the SPA before pairing. All API routes except the BOOT-gated provisioning receipt require the per-device `X-Canary-Token`. Host validation runs before auth to reduce DNS-rebinding exposure, CORS is same-origin by default with trust-on-pair enrollment, and witness exports are privacy-coarsened before canonical envelope export.

## Finding CV-SITE-2026-07-22-01: webhook SSRF / remote witness-event exfiltration

**Severity:** High

**Issue:** `integrations.webhook_url` was stored as a free-form URL and `sendWebhook()` selected HTTP or HTTPS transport from the parsed protocol. A user with API-token access, a compromised browser session, or any future config-import path could point the device at public Internet, loopback, or link-local endpoints. Because webhook delivery is performed by the device process and includes witness event data, this created both an SSRF sink and a way to bypass the project's local-ownership expectation.

**Fix shipped in this PR:** Add a shared webhook URL validator used both when accepting config updates and immediately before dispatch. The validator accepts only `http`/`https`, rejects credentials, strips fragments before persistence, rejects loopback and link-local targets, and limits allowed hosts to RFC1918 LAN IPv4 addresses and `.local` names for local automation systems.

**Residual risk:** Hostnames ending in `.local` depend on the local mDNS resolver and LAN trust. This is acceptable for the intended Home Assistant-class integration, but firmware should mirror the same allowlist and avoid general DNS for webhook targets.

## Additional observations

- Middleware ordering follows the documented architecture: security headers, Host validation, rate limit, static serving, PNA/CORS, JSON parsing, BOOT-gated provisioning, then API token authentication.
- Static SPA assets are intentionally unauthenticated, but CSP, `nosniff`, frame denial, no-referrer, and restrictive permissions-policy headers are set globally.
- Config updates reject unknown keys and immutable privacy keys, reducing typo-driven weaker defaults and API-enabled raw-media paths.
- Witness envelope export validates the raw chain before coarsening and refuses forbidden precise fields in the canonical envelope.

## Follow-up plan

1. Mirror the webhook validator in firmware and document it in the firmware integration guide.
2. Add a deployment-mode test that starts the server with `devMode: false` and verifies loopback Host and dev-only simulate/provision shortcuts stay unavailable.
3. Add dependency-audit CI for the Node workspace (`npm audit --audit-level=high`) and keep the existing `cargo` checks for the Rust kernel.
4. Review desktop Tauri capabilities in a separate pass, especially filesystem and shell/plugin permissions.
