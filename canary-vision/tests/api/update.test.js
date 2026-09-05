'use strict';

const { describe, it, before, after } = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { startServer, TEST_TOKEN } = require('../helpers/start-server');
const { createClient, request } = require('../helpers/test-client');

const TOKEN = TEST_TOKEN;

// Firmware header: magic "SCV\x01" + major + minor + patch + reserved
const FIRMWARE_MAGIC = Buffer.from([0x53, 0x43, 0x56, 0x01]);

function buildFirmware(major, minor, patch) {
  const header = Buffer.alloc(8);
  FIRMWARE_MAGIC.copy(header, 0);
  header[4] = major;
  header[5] = minor;
  header[6] = patch;
  header[7] = 0; // reserved
  const body = crypto.randomBytes(64); // simulated firmware body
  return Buffer.concat([header, body]);
}

function signFirmware(firmware, privateKey) {
  return crypto.sign(null, firmware, privateKey);
}

describe('Update API', () => {
  let server, client, signingKeyPair, tmpDir;

  before(async () => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'update-test-'));
    signingKeyPair = crypto.generateKeyPairSync('ed25519');
    const pubKeyRaw = signingKeyPair.publicKey.export({ type: 'spki', format: 'der' });
    const rawPubKey = pubKeyRaw.subarray(pubKeyRaw.length - 32);
    process.env.SECURACV_FW_SIGNING_PUBKEY = rawPubKey.toString('hex');
    process.env.CANARY_DATA_DIR = tmpDir;

    server = await startServer({ devMode: true });
    client = createClient(server.url, TOKEN);
    server.state.device.firmware_version = '0.1.0';
  });

  after(async () => {
    delete process.env.SECURACV_FW_SIGNING_PUBKEY;
    delete process.env.CANARY_DATA_DIR;
    await server.close();
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it('GET /api/v1/update/check reports the running version and offers nothing it cannot vouch for', async () => {
    const res = await client.get('/api/v1/update/check');
    assert.equal(res.status, 200);
    assert.ok(res.json.current_version);
    // The reference server has no update feed. It used to answer with a
    // hard-coded "0.4.2 is available" plus a made-up sha256 and signature;
    // a caller must never see a signed-looking update nobody signed.
    assert.equal(res.json.update_available, false);
    assert.equal(res.json.available_version, null);
    assert.equal(res.json.sha256, null);
    assert.equal(res.json.signature, null);
  });

  it('POST /api/v1/update rejects missing signature', async () => {
    const firmware = buildFirmware(1, 0, 0);
    const res = await request(server.url + '/api/v1/update', {
      method: 'POST',
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        'Content-Type': 'application/octet-stream',
      },
      body: firmware,
    });
    assert.equal(res.status, 403);
    assert.equal(res.json.error, 'signature_missing');
  });

  it('POST /api/v1/update rejects invalid signature', async () => {
    const firmware = buildFirmware(1, 0, 0);
    const badSig = crypto.randomBytes(64).toString('hex');
    const res = await request(server.url + '/api/v1/update', {
      method: 'POST',
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        'Content-Type': 'application/octet-stream',
        'X-Firmware-Signature': badSig,
      },
      body: firmware,
    });
    assert.equal(res.status, 403);
    assert.equal(res.json.error, 'signature_invalid');
  });

  it('POST /api/v1/update accepts valid signature with newer version', async () => {
    const firmware = buildFirmware(1, 0, 0);
    const sig = signFirmware(firmware, signingKeyPair.privateKey).toString('hex');
    const res = await request(server.url + '/api/v1/update', {
      method: 'POST',
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        'Content-Type': 'application/octet-stream',
        'X-Firmware-Signature': sig,
      },
      body: firmware,
    });
    assert.equal(res.status, 200);
    assert.equal(res.json.ok, true);
    assert.equal(res.json.version, '1.0.0');

    // Wait for simulated update to complete and reset cooldown
    await new Promise((resolve) => setTimeout(resolve, 200));
    server.state.setLastUpdateTime(0);
  });

  it('POST /api/v1/update rejects truncated payload', async () => {
    const firmware = buildFirmware(2, 0, 0);
    const sig = signFirmware(firmware, signingKeyPair.privateKey).toString('hex');
    // Send only partial data (truncated)
    const truncated = firmware.subarray(0, 4);
    const res = await request(server.url + '/api/v1/update', {
      method: 'POST',
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        'Content-Type': 'application/octet-stream',
        'X-Firmware-Signature': sig,
      },
      body: truncated,
    });
    // Truncated payload won't match signature
    assert.equal(res.status, 403);
    assert.equal(res.json.error, 'signature_invalid');
  });
});

describe('Update API — Anti-Downgrade', () => {
  let server, signingKeyPair, tmpDir;

  before(async () => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'downgrade-test-'));
    signingKeyPair = crypto.generateKeyPairSync('ed25519');
    const pubKeyRaw = signingKeyPair.publicKey.export({ type: 'spki', format: 'der' });
    const rawPubKey = pubKeyRaw.subarray(pubKeyRaw.length - 32);
    process.env.SECURACV_FW_SIGNING_PUBKEY = rawPubKey.toString('hex');
    process.env.CANARY_DATA_DIR = tmpDir;

    server = await startServer({ devMode: true });
    server.state.device.firmware_version = '1.0.0';
  });

  after(async () => {
    delete process.env.SECURACV_FW_SIGNING_PUBKEY;
    delete process.env.CANARY_DATA_DIR;
    await server.close();
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it('rejects same-version firmware update', async () => {
    const firmware = buildFirmware(1, 0, 0);
    const sig = signFirmware(firmware, signingKeyPair.privateKey).toString('hex');
    const res = await request(server.url + '/api/v1/update', {
      method: 'POST',
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        'Content-Type': 'application/octet-stream',
        'X-Firmware-Signature': sig,
      },
      body: firmware,
    });
    assert.equal(res.status, 403);
    assert.equal(res.json.error, 'version_downgrade');
  });

  it('rejects downgrade firmware update', async () => {
    const firmware = buildFirmware(0, 9, 0);
    const sig = signFirmware(firmware, signingKeyPair.privateKey).toString('hex');
    const res = await request(server.url + '/api/v1/update', {
      method: 'POST',
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        'Content-Type': 'application/octet-stream',
        'X-Firmware-Signature': sig,
      },
      body: firmware,
    });
    assert.equal(res.status, 403);
    assert.equal(res.json.error, 'version_downgrade');
  });

  it('accepts upgrade firmware update', async () => {
    const firmware = buildFirmware(2, 0, 0);
    const sig = signFirmware(firmware, signingKeyPair.privateKey).toString('hex');
    const res = await request(server.url + '/api/v1/update', {
      method: 'POST',
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        'Content-Type': 'application/octet-stream',
        'X-Firmware-Signature': sig,
      },
      body: firmware,
    });
    assert.equal(res.status, 200);
    assert.equal(res.json.ok, true);
    assert.equal(res.json.version, '2.0.0');

    // Wait for simulated update to complete and reset cooldown
    await new Promise((resolve) => setTimeout(resolve, 200));
    server.state.setLastUpdateTime(0);
  });

  it('rejects firmware with corrupt version header', async () => {
    // Build firmware without proper magic bytes
    const badFirmware = crypto.randomBytes(72);
    const sig = signFirmware(badFirmware, signingKeyPair.privateKey).toString('hex');
    const res = await request(server.url + '/api/v1/update', {
      method: 'POST',
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        'Content-Type': 'application/octet-stream',
        'X-Firmware-Signature': sig,
      },
      body: badFirmware,
    });
    assert.equal(res.status, 400);
    assert.equal(res.json.error, 'invalid_firmware_header');
  });
});

describe('Update API — no release key pinned', () => {
  let server, tmpDir;

  before(async () => {
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'nokey-test-'));
    // The route reads the release key at construction; make sure none is set.
    delete process.env.SECURACV_FW_SIGNING_PUBKEY;
    process.env.CANARY_DATA_DIR = tmpDir;
    server = await startServer({ devMode: true });
    server.state.device.firmware_version = '0.1.0';
  });

  after(async () => {
    delete process.env.CANARY_DATA_DIR;
    await server.close();
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it('refuses firmware signed by the device\'s own key — that key is not a release key', async () => {
    // Pre-fix the route fell back to state.publicKey when the env var was
    // unset, so firmware signed with the device's own private key (which the
    // device itself holds) installed as "verified". Nothing about who built
    // the firmware is proven by that, so it is refused outright.
    const firmware = buildFirmware(1, 0, 0);
    const sig = signFirmware(firmware, server.state.privateKey).toString('hex');
    const res = await request(server.url + '/api/v1/update', {
      method: 'POST',
      headers: {
        Host: '127.0.0.1',
        'X-Canary-Token': TOKEN,
        'Content-Type': 'application/octet-stream',
        'X-Firmware-Signature': sig,
      },
      body: firmware,
    });
    assert.equal(res.status, 500);
    assert.equal(res.json.error, 'signing_key_unavailable');
  });
});
