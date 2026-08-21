# Canary Vision

Privacy-preserving fleet management for SecuraCV Canary ESP32 camera devices.

Canary Vision provides a device-hosted API server and companion SPA for managing ESP32-based security cameras on your local network. All processing stays local. No cloud. No tracking. No subnet scanning.

## Quick Start

```bash
# Install dependencies
npm install

# Start the development server
npm run dev

# Run the full test suite
npm test
```

The dev server starts at `http://localhost:3000` with `devMode` enabled (localhost accepted as a valid Host header).

### Other Test Commands

```bash
npm run test:security   # Security-specific tests only
npm run test:api        # API endpoint tests only
npm run test:spa        # SPA compliance tests only
```

Requires **Node.js >= 20.0.0**.

## Project Structure

```
canary-vision/
  device-api/           # Express reference server (mirrors ESP32 firmware API)
    server.js           # App factory and middleware stack
    middleware/
      security-headers.js   # CSP, X-Frame-Options, etc.
      host-validation.js    # DNS rebinding protection
      rate-limit.js         # Per-IP rate limiting + auth lockout
      pna.js                # Chrome Private Network Access preflight
      cors.js               # Same-origin + trust-on-pair CORS (no peer allowlist)
      auth.js               # X-Canary-Token validation
    routes/
      info.js           # GET /api/v1/info
      config.js         # GET/PUT /api/v1/config (full + per-section)
      logs.js           # GET /api/v1/logs
      witness.js        # GET /api/v1/witness
      peers.js          # GET /api/v1/peers
      reboot.js         # POST /api/v1/reboot
      update.js         # GET /api/v1/update/check, POST /api/v1/update
    lib/
      device-state.js   # In-memory device state, config defaults, witness chain
      token.js          # Constant-time token comparison
      witness-chain.js  # SHA-256 hash chain with Ed25519 signatures
  spa/                  # Device-hosted vanilla JS companion app (no build step)
    index.html
    app.js
    styles.css
    app.webmanifest
  tests/
    helpers/
      start-server.js   # Test server bootstrap
      test-client.js    # Minimal HTTP client with Host header control
    security/           # 9 security decision tests (D1-D6, D9, D10, headers)
    api/                # 7 API endpoint tests
    spa/                # 3 SPA compliance tests (CSP, routing, token storage)
  docs/                 # Documentation
```

## Architecture

### Device API

Each Canary device runs an HTTP server on the LAN. The reference implementation in `device-api/` uses Express and faithfully models the ESP32 firmware API. The SPA is served as static files from the same server.

**Middleware stack (order matters):**

1. Security headers (all responses)
2. Host header validation (DNS rebinding protection)
3. Rate limiting (per-IP, with auth failure lockout)
4. Static file serving (no auth required)
5. Private Network Access preflight
6. CORS (same-origin + trust-on-pair; no peer allowlist)
7. JSON body parsing (`/api/*` only)
8. Token authentication (`/api/*` only)
9. Route handlers

### Companion SPA

The SPA is a vanilla JavaScript application with no frameworks, no build step, and no external dependencies. It runs entirely in the browser, uses `localStorage` for per-device token storage, and only communicates with private-network addresses. It complies with the strict Content Security Policy: no inline scripts, no `eval`, no `innerHTML`, no tracking.

### Fleet Discovery

Devices are discovered via mDNS (`_securacv._tcp`), peer list exchange, manual entry, or provisioning receipt import. **No subnet scanning.** See [docs/discovery.md](docs/discovery.md).

### Witness Chain

Each detected event (motion, person, vehicle, animal, object removal, contact change, restricted-zone presence, acoustic impulse) is recorded in a tamper-evident hash chain. Records include a sequence number, SHA-256 hash of the chain data, link to the previous hash, and an Ed25519 signature. See [docs/api.md](docs/api.md#get-apiv1witness).

### Security

Eleven security decisions (D1-D11) govern the system design. All are enforced by tests. See [docs/security.md](docs/security.md).

## Device Identity

- **Device ID:** `canary-a3f7`
- **mDNS hostname:** `canary-a3f7.local`
- **API token format:** `cv_<device_id_suffix>_<32 hex chars>`
- **Auth header:** `X-Canary-Token`

## License

MIT -- see [LICENSE](LICENSE).
