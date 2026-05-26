'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

const DEVICE = {
  device_id: 'canary-a3f7',
  name: 'Front Porch',
  model: 'XIAO ESP32S3',
  firmware_version: '0.4.1',
  ip: '192.168.1.47',
  mac: 'AA:BB:CC:DD:EE:01',
  mdns_hostname: 'canary-a3f7.local',
};

const DEFAULT_CONFIG = {
  network: {
    wifi_ssid: 'HomeNetwork',
    wifi_channel: 6,
    mdns_enabled: true,
    mqtt_broker: '',
    mqtt_port: 1883,
    mqtt_topic_prefix: 'canary/a3f7',
  },
  privacy: {
    camera_enabled: true,
    camera_peek_enabled: false,
    local_processing_only: true,
    semantic_events_only: true,
    witness_log_enabled: true,
    video_storage: 'none',
    auto_purge_hours: 24,
  },
  detection: {
    motion_enabled: true,
    motion_sensitivity: 5,
    person_detection: true,
    vehicle_detection: false,
    animal_detection: false,
    zones: [
      { name: 'front', points: [[0, 0], [100, 0], [100, 100], [0, 100]] },
    ],
    suppression: {
      enabled: true,
      cooldown_seconds: 300,
      session_timeout_seconds: 60,
      motion_cooldown_seconds: 600,
    },
  },
  integrations: {
    mqtt_enabled: false,
    home_assistant: false,
    webhook_url: '',
  },
};

const DEFAULT_PEERS = [
  {
    device_id: 'canary-b1c2',
    name: 'Garage',
    ip: '192.168.1.103',
    mdns_hostname: 'canary-b1c2.local',
    last_seen: '2026-02-18T14:20:00Z',
  },
  {
    device_id: 'canary-d4e5',
    name: 'Back Yard',
    ip: '192.168.1.110',
    mdns_hostname: 'canary-d4e5.local',
    last_seen: '2026-02-18T14:18:00Z',
  },
];

function generateToken(deviceId) {
  const suffix = deviceId.split('-').pop() || 'xxxx';
  return 'cv_' + suffix + '_' + crypto.randomBytes(18).toString('hex');
}

function loadOrGenerateKeypair(keyDir) {
  if (keyDir) {
    const privPath = path.join(keyDir, 'ed25519.key');
    const pubPath = path.join(keyDir, 'ed25519.pub');
    try {
      const privateKey = crypto.createPrivateKey(fs.readFileSync(privPath));
      const publicKey = crypto.createPublicKey(fs.readFileSync(pubPath));
      return { publicKey, privateKey };
    } catch {
      // Key files don't exist yet — generate and persist
      const pair = crypto.generateKeyPairSync('ed25519');
      fs.mkdirSync(keyDir, { recursive: true });
      fs.writeFileSync(privPath, pair.privateKey.export({ type: 'pkcs8', format: 'pem' }), { mode: 0o600 });
      fs.writeFileSync(pubPath, pair.publicKey.export({ type: 'spki', format: 'pem' }));
      return pair;
    }
  }
  // No keyDir: ephemeral keys (tests / in-memory only)
  return crypto.generateKeyPairSync('ed25519');
}

function createDeviceState(overrides = {}) {
  const device = { ...DEVICE, ...overrides };

  // Generate a unique API token if none was provided via overrides or env
  if (!device.api_token) {
    device.api_token = process.env.CANARY_TOKEN || generateToken(device.device_id);
  }

  // Load or generate Ed25519 keypair for witness signing.
  // When keyDir is provided, persists the keypair to disk so the witness
  // chain remains externally verifiable across restarts.
  const { publicKey, privateKey } = loadOrGenerateKeypair(overrides.keyDir);

  const startTime = Date.now();

  // Deep clone config
  const config = structuredClone(DEFAULT_CONFIG);

  // Pending physical confirmations
  const pendingPhysicalConfirm = new Set();

  // Witness chain
  const witnessRecords = [];
  let witnessSeq = 4800;

  // Logs
  const logs = [];

  // Reboot tracking
  let lastRebootTime = 0;

  // Update tracking
  let updateInProgress = false;
  let lastUpdateTime = 0;

  function addLog(level, msg) {
    logs.push({
      ts: new Date().toISOString(),
      level,
      msg,
    });
    // Keep last 1000
    if (logs.length > 1000) logs.shift();
  }

  // Webhook callback (set by server after dispatcher is created)
  let onWitnessRecord = null;

  // GPS state (optional — null when no GPS module present)
  let gpsState = null;

  // Event suppression engine
  const activeSessions = new Map();
  let sessionIdCounter = 0;

  function getSuppressionConfig() {
    return config.detection.suppression || {};
  }

  function getCooldownForType(eventType) {
    const supp = getSuppressionConfig();
    if (eventType === 'motion_detected') {
      return (supp.motion_cooldown_seconds || 600) * 1000;
    }
    return (supp.cooldown_seconds || 300) * 1000;
  }

  function getSessionTimeout() {
    const supp = getSuppressionConfig();
    return (supp.session_timeout_seconds || 60) * 1000;
  }

  function sessionKey(eventType, zone) {
    return eventType + ':' + zone;
  }

  function tryEmitEvent(eventType, zone) {
    const supp = getSuppressionConfig();
    if (!supp.enabled) {
      return addWitnessRecord(eventType, zone);
    }

    const now = Date.now();
    const key = sessionKey(eventType, zone);
    const existing = activeSessions.get(key);

    if (existing) {
      const sessionAge = now - existing.lastSeen;
      const cooldown = getCooldownForType(eventType);

      if (sessionAge < getSessionTimeout()) {
        existing.lastSeen = now;
        existing.suppressedCount++;
        addLog('DEBUG', `Suppressed ${eventType} in ${zone} (session ${existing.sessionId}, ${existing.suppressedCount} suppressed)`);
        return null;
      }

      if (sessionAge < cooldown) {
        existing.lastSeen = now;
        existing.suppressedCount++;
        addLog('DEBUG', `Cooldown: ${eventType} in ${zone} (${Math.round((cooldown - sessionAge) / 1000)}s remaining)`);
        return null;
      }

      // Session expired + cooldown elapsed — close old session, start new one
      const closedSession = existing;
      activeSessions.delete(key);
      if (closedSession.suppressedCount > 0) {
        addLog('INFO', `Session ${closedSession.sessionId} ended: ${closedSession.suppressedCount} events suppressed`);
      }
    }

    // Start new activity session
    sessionIdCounter++;
    const session = {
      sessionId: 'sess_' + sessionIdCounter,
      eventType,
      zone,
      startedAt: now,
      lastSeen: now,
      suppressedCount: 0,
    };
    activeSessions.set(key, session);

    const record = addWitnessRecord(eventType, zone);
    if (record) {
      record.activity_session = session.sessionId;
    }
    return record;
  }

  function generateMockEdgeThumbnail(eventType, seq) {
    const W = 256, H = 192;
    const pixels = Buffer.alloc(W * H);
    const seed = seq * 7 + 13;

    // Simulate scene edges: horizontal lines for ground/horizon
    for (let x = 0; x < W; x++) {
      const horizonY = 90 + ((seed + x * 3) % 20) - 10;
      if (horizonY >= 0 && horizonY < H) pixels[horizonY * W + x] = 80 + (x % 40);
    }

    // Vertical edges for doorway/wall structure
    const wallX1 = 40 + (seed % 30);
    const wallX2 = W - 40 - (seed % 30);
    for (let y = 20; y < H - 20; y++) {
      if (wallX1 < W) pixels[y * W + wallX1] = 60 + (y % 30);
      if (wallX2 < W) pixels[y * W + wallX2] = 60 + (y % 30);
    }

    // Object silhouette based on event type
    let objX, objY, objW, objH;
    if (eventType === 'person_detected') {
      objW = 30 + (seed % 15); objH = 70 + (seed % 20);
      objX = 80 + (seed % 80); objY = H - objH - 30;
      for (let a = 0; a < 360; a += 10) {
        const px = Math.round(objX + objW / 2 + 8 * Math.cos(a * Math.PI / 180));
        const py = Math.round(objY - 5 + 8 * Math.sin(a * Math.PI / 180));
        if (px >= 0 && px < W && py >= 0 && py < H) pixels[py * W + px] = 180;
      }
    } else if (eventType === 'vehicle_detected') {
      objW = 80 + (seed % 30); objH = 40 + (seed % 15);
      objX = 50 + (seed % 60); objY = H - objH - 25;
    } else if (eventType === 'animal_detected') {
      objW = 35 + (seed % 15); objH = 25 + (seed % 10);
      objX = 90 + (seed % 70); objY = H - objH - 20;
    } else {
      objW = 20; objH = 20;
      objX = 100 + (seed % 50); objY = 80 + (seed % 40);
    }

    for (let x = objX; x < Math.min(objX + objW, W); x++) {
      if (objY >= 0 && objY < H) pixels[objY * W + x] = 220;
      const botY = Math.min(objY + objH, H - 1);
      pixels[botY * W + x] = 220;
    }
    for (let y = objY; y < Math.min(objY + objH, H); y++) {
      if (objX >= 0 && objX < W) pixels[y * W + objX] = 220;
      const rightX = Math.min(objX + objW, W - 1);
      pixels[y * W + rightX] = 220;
    }

    for (let y = objY + 2; y < Math.min(objY + objH - 2, H); y++) {
      for (let x = objX + 2; x < Math.min(objX + objW - 2, W); x++) {
        const noise = ((x * 7 + y * 13 + seed) % 100);
        if (noise < 15) pixels[y * W + x] = 100 + (noise * 8);
      }
    }

    const header = `P5\n${W} ${H}\n255\n`;
    const pgm = Buffer.concat([Buffer.from(header), pixels]);
    return 'data:image/x-portable-graymap;base64,' + pgm.toString('base64');
  }

  function addWitnessRecord(eventType, zone) {
    witnessSeq++;
    const prevHash = witnessRecords.length > 0
      ? witnessRecords[witnessRecords.length - 1].hash
      : '0'.repeat(64);

    const timestamp = new Date().toISOString();
    const data = `${witnessSeq}:${prevHash}:${timestamp}:${eventType}:${zone}`;
    const hash = crypto.createHash('sha256').update(data).digest('hex');
    const signature = crypto.sign(undefined, Buffer.from(hash), privateKey).toString('hex');

    const record = {
      seq: witnessSeq,
      hash,
      prev_hash: prevHash,
      timestamp,
      event_type: eventType,
      zone,
      signature,
      time_source: 'device_clock',
      thumbnail: generateMockEdgeThumbnail(eventType, witnessSeq),
    };

    if (gpsState && gpsState.fix_age_ms < 30000) {
      record.time_source = 'gps_utc';
      record.gps_timestamp = gpsState.utc;
      record.gps_fix_quality = gpsState.fix_quality;
      record.gps_satellites = gpsState.satellites;
      record.gps_fix_age_ms = gpsState.fix_age_ms;
    }

    witnessRecords.push(record);
    addLog('INFO', `Witness: ${eventType} in ${zone} (seq ${witnessSeq})`);

    if (onWitnessRecord) {
      onWitnessRecord(record);
    }

    return record;
  }

  // Seed some initial data
  addLog('INFO', 'Device booted');
  addLog('INFO', 'WiFi connected to HomeNetwork');
  addLog('INFO', 'mDNS registered: canary-a3f7.local');
  addLog('DEBUG', 'Motion detection initialized');
  addLog('INFO', 'Peer discovered: canary-b1c2 (Garage)');
  addLog('INFO', 'Peer discovered: canary-d4e5 (Back Yard)');

  // Seed witness records with activity sessions
  const seedRecord = (type, zone, sessionId) => {
    const r = addWitnessRecord(type, zone);
    if (r && sessionId) r.activity_session = sessionId;
    return r;
  };
  seedRecord('person_detected', 'front', 'sess_demo_1');
  seedRecord('motion_detected', 'front', 'sess_demo_1');
  seedRecord('person_detected', 'front', 'sess_demo_1');
  seedRecord('vehicle_detected', 'front', 'sess_demo_2');
  seedRecord('motion_detected', 'front', 'sess_demo_2');
  seedRecord('person_detected', 'front', 'sess_demo_3');
  seedRecord('motion_detected', 'front', 'sess_demo_3');
  seedRecord('animal_detected', 'front', 'sess_demo_4');
  seedRecord('person_detected', 'front', 'sess_demo_5');
  seedRecord('motion_detected', 'front', 'sess_demo_5');

  return {
    device,
    config,
    pendingPhysicalConfirm,
    witnessRecords,
    logs,
    publicKey,
    privateKey,
    startTime,
    lastRebootTime,
    updateInProgress,
    lastUpdateTime,
    peers: structuredClone(overrides.peers || DEFAULT_PEERS),
    addLog,
    addWitnessRecord,
    tryEmitEvent,
    getActiveSessions() { return Array.from(activeSessions.values()); },
    setOnWitnessRecord(cb) { onWitnessRecord = cb; },
    setGpsState(state) { gpsState = state; },
    getGpsState() { return gpsState; },
    getUptime() {
      return Math.floor((Date.now() - startTime) / 1000);
    },
    getRebootTime() {
      return lastRebootTime;
    },
    setRebootTime(t) {
      lastRebootTime = t;
    },
    getUpdateInProgress() {
      return updateInProgress;
    },
    setUpdateInProgress(v) {
      updateInProgress = v;
    },
    getLastUpdateTime() {
      return lastUpdateTime;
    },
    setLastUpdateTime(t) {
      lastUpdateTime = t;
    },
  };
}

module.exports = { createDeviceState, DEVICE, DEFAULT_CONFIG, DEFAULT_PEERS };
