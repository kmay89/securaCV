#!/bin/bash
set -e

echo "=== Canary Vision Test Suite ==="
echo ""

# Install deps
cd "$(dirname "$0")/.."
npm install --workspaces

# Run security tests
echo "── Security Tests ──"
node --test tests/security/auth.test.js
node --test tests/security/host-validation.test.js
node --test tests/security/cors.test.js
node --test tests/security/pna.test.js
node --test tests/security/no-scan.test.js
node --test tests/security/no-fleet-sync.test.js
node --test tests/security/rate-limit.test.js
node --test tests/security/camera-peek.test.js
node --test tests/security/security-headers.test.js

# Run API tests
echo ""
echo "── API Tests ──"
node --test tests/api/info.test.js
node --test tests/api/config.test.js
node --test tests/api/logs.test.js
node --test tests/api/witness.test.js
node --test tests/api/witness-simulate.test.js
node --test tests/api/envelope-bridge.test.js
node --test tests/api/peers.test.js
node --test tests/api/reboot.test.js
node --test tests/api/update.test.js
node --test tests/api/provision.test.js
node --test tests/api/webhook.test.js

# Run SPA tests
echo ""
echo "── SPA Tests ──"
node --test tests/spa/csp-compliance.test.js
node --test tests/spa/routing.test.js
node --test tests/spa/token-storage.test.js
node --test tests/spa/peer-discovery.test.js
node --test tests/spa/device-types.test.js
node --test tests/spa/magic-pairing.test.js
node --test tests/spa/session-expiry.test.js
node --test tests/spa/verified-timeline.test.js

echo ""
echo "=== ALL TESTS PASSED ==="
