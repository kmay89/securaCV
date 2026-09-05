'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { Router } = require('express');

const UPDATE_COOLDOWN_MS = 60 * 60 * 1000; // 1 hour
const MAX_FIRMWARE_SIZE = 16 * 1024 * 1024; // 16 MiB

// Firmware header format: magic (4 bytes) + version_major (u8) + version_minor (u8) + version_patch (u8) + reserved (1 byte)
const FIRMWARE_MAGIC = Buffer.from([0x53, 0x43, 0x56, 0x01]); // "SCV\x01"
const FIRMWARE_HEADER_SIZE = 8;

/**
 * Load the firmware signing public key from SECURACV_FW_SIGNING_PUBKEY (PEM or
 * 32-byte hex). Returns null when it is unset — there is deliberately no
 * fallback; the state argument is kept so the call signature stays stable.
 */
function loadSigningPublicKey(_state) {
  const envKey = process.env.SECURACV_FW_SIGNING_PUBKEY;
  if (envKey) {
    if (envKey.startsWith('-----BEGIN')) {
      return crypto.createPublicKey(envKey);
    }
    // Assume hex-encoded raw Ed25519 public key (32 bytes)
    const raw = Buffer.from(envKey, 'hex');
    if (raw.length !== 32) {
      throw new Error('SECURACV_FW_SIGNING_PUBKEY hex must be 32 bytes');
    }
    return crypto.createPublicKey({
      key: Buffer.concat([
        // Ed25519 SPKI prefix (DER)
        Buffer.from('302a300506032b6570032100', 'hex'),
        raw,
      ]),
      format: 'der',
      type: 'spki',
    });
  }
  // No fallback to the device's own key. That key's private half lives on
  // this very device (lib/device-state.js mints it), so checking firmware
  // against it proves only that the firmware was signed HERE — a compromised
  // device, or anyone holding its key, could mint "valid" firmware for it.
  // Unset means every update is refused (signing_key_unavailable): the honest
  // state of a build with no release key pinned.
  console.error('[SECURITY] SECURACV_FW_SIGNING_PUBKEY is not set — firmware updates are disabled until a release key is pinned');
  return null;
}

/**
 * Parse the firmware header from the beginning of the payload.
 * Returns { major, minor, patch } or null if invalid.
 */
function parseFirmwareHeader(buf) {
  if (!buf || buf.length < FIRMWARE_HEADER_SIZE) {
    return null;
  }
  if (!buf.subarray(0, 4).equals(FIRMWARE_MAGIC)) {
    return null;
  }
  return {
    major: buf[4],
    minor: buf[5],
    patch: buf[6],
  };
}

/**
 * Compare two version objects. Returns:
 *  1 if a > b, -1 if a < b, 0 if equal.
 */
function compareVersions(a, b) {
  if (a.major !== b.major) return a.major > b.major ? 1 : -1;
  if (a.minor !== b.minor) return a.minor > b.minor ? 1 : -1;
  if (a.patch !== b.patch) return a.patch > b.patch ? 1 : -1;
  return 0;
}

/**
 * Parse a semver string "MAJOR.MINOR.PATCH" into { major, minor, patch }.
 */
function parseVersionString(str) {
  const parts = (str || '').split('.').map(Number);
  if (parts.length < 2 || parts.some(isNaN)) return null;
  return { major: parts[0], minor: parts[1], patch: parts[2] || 0 };
}

/**
 * Get the path for the persistent version file.
 */
function versionFilePath(state) {
  const dataDir = state.device.keyDir || process.env.CANARY_DATA_DIR || path.join(__dirname, '..', '..', '.data');
  return path.join(dataDir, 'installed_firmware_version');
}

/**
 * Load the currently installed firmware version from persistent storage.
 */
function loadInstalledVersion(state) {
  try {
    const raw = fs.readFileSync(versionFilePath(state), 'utf8').trim();
    return parseVersionString(raw);
  } catch {
    // If no version file exists, use the device's reported version
    return parseVersionString(state.device.firmware_version);
  }
}

/**
 * Save the installed firmware version to persistent storage.
 */
function saveInstalledVersion(state, version) {
  const filePath = versionFilePath(state);
  const dir = path.dirname(filePath);
  fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(filePath, `${version.major}.${version.minor}.${version.patch}\n`, { mode: 0o600 });
}

function updateRoutes(state) {
  const router = Router();

  // Load signing key once at startup
  let signingPubKey;
  try {
    signingPubKey = loadSigningPublicKey(state);
  } catch (err) {
    console.error(`[SECURITY] Failed to load firmware signing key: ${err.message}`);
    // Routes will reject all updates if key is unavailable
  }

  router.get('/api/v1/update/check', (req, res) => {
    // The reference server has no update feed to ask. It used to answer with a
    // hard-coded "0.4.2 is available" plus a made-up sha256 and signature —
    // shipped code telling every caller an update existed and looked signed.
    // Say what is true: the running version, and that nothing is on offer here.
    res.json({
      current_version: state.device.firmware_version,
      available_version: null,
      update_available: false,
      changelog: null,
      sha256: null,
      signature: null,
      requires_usb: false,
      note: 'no update feed is configured on this device; updates arrive by POST /api/v1/update',
    });
  });

  router.post('/api/v1/update', (req, res) => {
    // Check for update in progress
    if (state.getUpdateInProgress()) {
      return res.status(503).json({
        error: 'update_in_progress',
        message: 'An update is already in progress',
      });
    }

    // Verify signing key is available
    if (!signingPubKey) {
      return res.status(500).json({
        error: 'signing_key_unavailable',
        message: 'Firmware signing key not configured',
      });
    }

    // Rate limit: 1 per hour
    const now = Date.now();
    if (state.getLastUpdateTime() && (now - state.getLastUpdateTime()) < UPDATE_COOLDOWN_MS) {
      const retryAfter = Math.ceil((state.getLastUpdateTime() + UPDATE_COOLDOWN_MS - now) / 1000);
      res.setHeader('Retry-After', String(retryAfter));
      return res.status(429).json({
        error: 'rate_limited',
        message: 'Update rate limit exceeded. Try again later.',
        retry_after: retryAfter,
      });
    }

    // Extract the detached Ed25519 signature from the header
    const signatureHex = req.headers['x-firmware-signature'];
    if (!signatureHex) {
      return res.status(403).json({
        error: 'signature_missing',
        message: 'Firmware signature is required',
      });
    }

    const signatureBytes = Buffer.from(signatureHex, 'hex');
    if (signatureBytes.length !== 64) {
      return res.status(403).json({
        error: 'signature_invalid',
        message: 'Firmware signature verification failed',
      });
    }

    // Collect the full firmware payload
    const chunks = [];
    let totalSize = 0;

    req.on('data', (chunk) => {
      totalSize += chunk.length;
      if (totalSize > MAX_FIRMWARE_SIZE) {
        // Payload too large — destroy the stream
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });

    req.on('end', () => {
      if (totalSize > MAX_FIRMWARE_SIZE) {
        return res.status(400).json({
          error: 'payload_too_large',
          message: 'Firmware payload exceeds maximum size',
        });
      }

      if (totalSize === 0) {
        return res.status(400).json({
          error: 'no_firmware',
          message: 'No firmware file provided',
        });
      }

      const firmwarePayload = Buffer.concat(chunks);

      // SECURITY: Verify Ed25519 signature of the firmware binary
      let signatureValid;
      try {
        signatureValid = crypto.verify(
          null, // Ed25519 does not use a separate hash algorithm
          firmwarePayload,
          signingPubKey,
          signatureBytes,
        );
      } catch {
        signatureValid = false;
      }

      if (!signatureValid) {
        // SECURITY: Do NOT leak which byte or offset failed
        state.addLog('WARN', 'SECURITY: Firmware signature verification failed — update rejected');
        return res.status(403).json({
          error: 'signature_invalid',
          message: 'Firmware signature verification failed',
        });
      }

      // Parse firmware header for version info (anti-downgrade)
      const fwVersion = parseFirmwareHeader(firmwarePayload);
      if (!fwVersion) {
        state.addLog('WARN', 'SECURITY: Firmware header missing or corrupt — update rejected');
        return res.status(400).json({
          error: 'invalid_firmware_header',
          message: 'Firmware version header is missing or corrupt',
        });
      }

      // Anti-downgrade check
      const installedVersion = loadInstalledVersion(state);
      if (installedVersion) {
        const cmp = compareVersions(fwVersion, installedVersion);
        if (cmp <= 0) {
          const fwStr = `${fwVersion.major}.${fwVersion.minor}.${fwVersion.patch}`;
          const instStr = `${installedVersion.major}.${installedVersion.minor}.${installedVersion.patch}`;
          state.addLog(
            'WARN',
            `SECURITY: Firmware downgrade attempt blocked: ${fwStr} <= ${instStr}`,
          );
          return res.status(403).json({
            error: 'version_downgrade',
            message: 'Firmware version must be strictly newer than installed version',
          });
        }
      }

      // Signature valid + version newer — accept the update
      state.setUpdateInProgress(true);
      state.setLastUpdateTime(now);

      const versionStr = `${fwVersion.major}.${fwVersion.minor}.${fwVersion.patch}`;
      state.addLog('WARN', `Firmware update started: v${versionStr}`);

      // Persist the new version
      saveInstalledVersion(state, fwVersion);

      // Simulate update completing after a short delay
      setTimeout(() => {
        state.setUpdateInProgress(false);
        state.device.firmware_version = versionStr;
        state.addLog('INFO', `Firmware update completed: v${versionStr}`);
      }, 100);

      res.json({
        ok: true,
        message: 'Updating firmware. Device will reboot.',
        version: versionStr,
      });
    });
  });

  return router;
}

// Exports for testing
module.exports = updateRoutes;
module.exports._test = {
  parseFirmwareHeader,
  compareVersions,
  parseVersionString,
  FIRMWARE_MAGIC,
  FIRMWARE_HEADER_SIZE,
  loadSigningPublicKey,
};
