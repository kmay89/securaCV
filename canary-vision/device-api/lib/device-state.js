'use strict';

const crypto = require('node:crypto');

const DEVICE = {
  device_id: 'canary-a3f7',
  name: 'Front Porch',
  model: 'XIAO ESP32S3',
  firmware_version: '0.4.1',
  ip: '192.168.1.47',
  mac: 'AA:BB:CC:DD:EE:01',
  mdns_hostname: 'canary-a3f7.local',
  api_token: 'cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f',
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
    last_seen: '2026-02-18T14:20:00Z',
  },
  {
    device_id: 'canary-d4e5',
    name: 'Back Yard',
    ip: '192.168.1.110',
    last_seen: '2026-02-18T14:18:00Z',
  },
];

function createDeviceState(overrides = {}) {
  const device = { ...DEVICE, ...overrides };

  // Generate Ed25519 keypair for witness signing
  const { publicKey, privateKey } = crypto.generateKeyPairSync('ed25519');

  const startTime = Date.now();

  // Deep clone config
  const config = JSON.parse(JSON.stringify(DEFAULT_CONFIG));

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

  function addWitnessRecord(eventType, zone) {
    witnessSeq++;
    const prevHash = witnessRecords.length > 0
      ? witnessRecords[witnessRecords.length - 1].hash
      : '0'.repeat(64);

    const timestamp = new Date().toISOString();
    const data = `${witnessSeq}:${prevHash}:${timestamp}:${eventType}:${zone}`;
    const hash = crypto.createHash('sha256').update(data).digest('hex');
    const signature = crypto.sign(null, Buffer.from(hash), privateKey).toString('hex');

    const record = {
      seq: witnessSeq,
      hash,
      prev_hash: prevHash,
      timestamp,
      event_type: eventType,
      zone,
      signature,
    };

    witnessRecords.push(record);
    addLog('INFO', `Witness: ${eventType} in ${zone} (seq ${witnessSeq})`);
    return record;
  }

  // Seed some initial data
  addLog('INFO', 'Device booted');
  addLog('INFO', 'WiFi connected to HomeNetwork');
  addLog('INFO', 'mDNS registered: canary-a3f7.local');
  addLog('DEBUG', 'Motion detection initialized');
  addLog('INFO', 'Peer discovered: canary-b1c2 (Garage)');
  addLog('INFO', 'Peer discovered: canary-d4e5 (Back Yard)');

  // Seed witness records
  addWitnessRecord('person_detected', 'front');
  addWitnessRecord('motion_detected', 'front');
  addWitnessRecord('person_detected', 'front');
  addWitnessRecord('vehicle_detected', 'front');
  addWitnessRecord('motion_detected', 'front');
  addWitnessRecord('person_detected', 'front');
  addWitnessRecord('motion_detected', 'front');
  addWitnessRecord('animal_detected', 'front');
  addWitnessRecord('person_detected', 'front');
  addWitnessRecord('motion_detected', 'front');

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
    peers: JSON.parse(JSON.stringify(overrides.peers || DEFAULT_PEERS)),
    addLog,
    addWitnessRecord,
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
