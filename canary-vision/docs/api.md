# API Reference

All endpoints are served by the Canary device on the local network over HTTP.

**Base URL:** `http://canary-a3f7.local` or `http://192.168.1.47`

**API version prefix:** `/api/v1`

## Authentication

Every `/api/*` request must include the `X-Canary-Token` header. Tokens are per-device and follow the format `cv_<device_id_suffix>_<32 hex chars>`.

```
X-Canary-Token: cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f
```

Static files (`/`, `/app.js`, `/styles.css`, `/app.webmanifest`) do **not** require authentication.

Token comparison uses `crypto.timingSafeEqual` to prevent timing attacks.

## Error Responses

All errors return JSON with `error` and `message` fields.

| Status | `error` Code | Meaning |
|--------|-------------|---------|
| 400 | `invalid_config` | Malformed or invalid config body |
| 400 | `no_firmware` | Firmware upload body was empty |
| 400 | `invalid_firmware_header` | Firmware version header missing or corrupt |
| 400 | `payload_too_large` | Firmware payload exceeds the 16 MiB maximum |
| 401 | `token_required` | `X-Canary-Token` header missing |
| 401 | `token_invalid` | Token does not match device token |
| 403 | `host_rejected` | Host header failed validation (DNS rebinding protection) |
| 403 | `signature_missing` | Firmware upload lacked the `X-Firmware-Signature` header |
| 403 | `signature_invalid` | Firmware signature failed Ed25519 verification |
| 403 | `version_downgrade` | Firmware version not strictly newer than the installed one |
| 404 | `not_found` | Endpoint or config section not found |
| 429 | `rate_limited` | General rate limit exceeded (30 req/min) |
| 429 | `auth_locked` | Too many auth failures (5 trigger an escalating lockout: 2 s, doubling to a 300 s cap) |
| 503 | `update_in_progress` | A firmware update is already running |

Rate-limited responses include a `Retry-After` header (seconds).

---

## Endpoints

### GET /api/v1/info

Returns device identity and status.

**Request:**

```bash
curl -H "X-Canary-Token: cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f" \
     http://canary-a3f7.local/api/v1/info
```

**Response (200):**

```json
{
  "device_id": "canary-a3f7",
  "name": "Front Porch",
  "model": "XIAO ESP32S3",
  "firmware_version": "0.4.1",
  "uptime_s": 86400,
  "last_seen": "2026-02-18T15:30:00.000Z",
  "wifi_rssi": -42,
  "ip": "192.168.1.47",
  "capabilities": ["camera_peek", "ota_update", "mqtt", "witness_log"]
}
```

---

### GET /api/v1/config

Returns the full device configuration across all sections.

**Request:**

```bash
curl -H "X-Canary-Token: cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f" \
     http://canary-a3f7.local/api/v1/config
```

**Response (200):**

```json
{
  "network": {
    "wifi_ssid": "HomeNetwork",
    "wifi_channel": 6,
    "mdns_enabled": true,
    "mqtt_broker": "",
    "mqtt_port": 1883,
    "mqtt_topic_prefix": "canary/a3f7"
  },
  "privacy": {
    "camera_enabled": true,
    "camera_peek_enabled": false,
    "local_processing_only": true,
    "semantic_events_only": true,
    "witness_log_enabled": true,
    "video_storage": "none",
    "auto_purge_hours": 24
  },
  "detection": {
    "motion_enabled": true,
    "motion_sensitivity": 5,
    "person_detection": true,
    "vehicle_detection": false,
    "animal_detection": false,
    "zones": [
      { "name": "front", "points": [[0,0],[100,0],[100,100],[0,100]] }
    ]
  },
  "integrations": {
    "mqtt_enabled": false,
    "home_assistant": false,
    "webhook_url": ""
  }
}
```

---

### PUT /api/v1/config

Merge-updates the full configuration. Only include sections and fields you want to change.

**Request:**

```bash
curl -X PUT \
     -H "X-Canary-Token: cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f" \
     -H "Content-Type: application/json" \
     -d '{"detection": {"motion_sensitivity": 7}}' \
     http://canary-a3f7.local/api/v1/config
```

**Response (200):**

```json
{
  "ok": true,
  "config": { "...full config after merge..." }
}
```

`privacy.camera_peek_enabled` is **immutable via the API** (Invariant I: No Raw Export). If a config update includes it, the key is stripped, the rest of the update is applied normally, and the response lists the rejected key:

```json
{
  "ok": true,
  "config": { "..." },
  "rejected_immutable": ["camera_peek_enabled"]
}
```

There is no HTTP endpoint that can change it — enabling camera peek requires physical interaction with the device (the firmware's physical button mechanism).

**Validation rules:**

| Field | Constraint |
|-------|-----------|
| `privacy.auto_purge_hours` | Must be one of: 1, 6, 12, 24, 48, 168 |
| `detection.motion_sensitivity` | Integer 1-10 |
| `network.mqtt_port` | Integer 1-65535 |
| `network.wifi_ssid` | Non-empty string |

---

### GET /api/v1/config/:section

Returns a single configuration section.

**Valid sections:** `network`, `privacy`, `detection`, `integrations`

**Request:**

```bash
curl -H "X-Canary-Token: cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f" \
     http://canary-a3f7.local/api/v1/config/privacy
```

**Response (200):**

```json
{
  "camera_enabled": true,
  "camera_peek_enabled": false,
  "local_processing_only": true,
  "semantic_events_only": true,
  "witness_log_enabled": true,
  "video_storage": "none",
  "auto_purge_hours": 24
}
```

**Response (404):** Unknown section.

---

### PUT /api/v1/config/:section

Merge-updates a single configuration section. Body fields are merged into that section only.

**Request:**

```bash
curl -X PUT \
     -H "X-Canary-Token: cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f" \
     -H "Content-Type: application/json" \
     -d '{"motion_sensitivity": 8}' \
     http://canary-a3f7.local/api/v1/config/detection
```

**Response (200):**

```json
{
  "ok": true,
  "config": { "...full config after merge..." }
}
```

---

### GET /api/v1/logs

Returns device log entries.

**Query parameters:**

| Param | Default | Range | Description |
|-------|---------|-------|-------------|
| `tail` | 100 | 1-500 | Number of most recent entries to return |
| `level` | (all) | INFO, DEBUG, WARN, ERROR | Filter by log level |

**Request:**

```bash
curl -H "X-Canary-Token: cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f" \
     "http://canary-a3f7.local/api/v1/logs?tail=50&level=WARN"
```

**Response (200):**

```json
{
  "logs": [
    {
      "ts": "2026-02-18T14:20:00.000Z",
      "level": "WARN",
      "msg": "Reboot requested"
    }
  ]
}
```

---

### GET /api/v1/witness

Returns entries from the tamper-evident witness hash chain.

Each record is linked to its predecessor by `prev_hash`, forming an append-only chain. Records are signed with Ed25519.

**Query parameters:**

| Param | Default | Range | Description |
|-------|---------|-------|-------------|
| `last` | 20 | 1-100 | Number of most recent records to return |

**Request:**

```bash
curl -H "X-Canary-Token: cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f" \
     "http://canary-a3f7.local/api/v1/witness?last=5"
```

**Response (200):**

```json
{
  "records": [
    {
      "seq": 4801,
      "hash": "a1b2c3...64 hex chars",
      "prev_hash": "000000...64 zeros (genesis) or previous hash",
      "timestamp": "2026-02-18T14:10:00.000Z",
      "event_type": "person_detected",
      "zone": "front",
      "signature": "ed25519 signature hex",
      "time_source": "device_clock",
      "thumbnail": "data:image/x-portable-graymap;base64,..."
    }
  ]
}
```

When the device has a fresh GPS fix, `time_source` is `gps_utc` and the record additionally carries `gps_timestamp`, `gps_fix_quality`, `gps_satellites`, and `gps_fix_age_ms`.

**Hash computation:** `SHA256("${seq}:${prev_hash}:${timestamp}:${event_type}:${zone}:${time_source}:${gps_timestamp}")`

- `time_source` is `gps_utc` or `device_clock` (a record without one hashes as `device_clock`).
- `gps_timestamp` is the empty string when the record has no `gps_timestamp` field, so a `device_clock` record's hash input ends in `:device_clock:`.

This is the same formula the device itself embeds in `GET /api/v1/witness/export` under `verification_instructions`.

**Event types:** `motion_detected`, `person_detected`, `vehicle_detected`, `animal_detected`, `object_removed`, `contact_changed`, `presence_restricted`, `acoustic_impulse`

---

### GET /api/v1/peers

Returns the list of known peer Canary devices.

**Request:**

```bash
curl -H "X-Canary-Token: cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f" \
     http://canary-a3f7.local/api/v1/peers
```

**Response (200):**

```json
{
  "peers": [
    {
      "device_id": "canary-b1c2",
      "name": "Garage",
      "ip": "192.168.1.103",
      "last_seen": "2026-02-18T14:20:00Z"
    },
    {
      "device_id": "canary-d4e5",
      "name": "Back Yard",
      "ip": "192.168.1.110",
      "last_seen": "2026-02-18T14:18:00Z"
    }
  ]
}
```

---

### POST /api/v1/reboot

Requests a device reboot.

**Rate limit:** 1 reboot per 5 minutes (enforced separately from the general rate limit).

**Request:**

```bash
curl -X POST \
     -H "X-Canary-Token: cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f" \
     http://canary-a3f7.local/api/v1/reboot
```

**Response (200):**

```json
{
  "ok": true,
  "message": "Rebooting in 2 seconds"
}
```

**Response (429):** Reboot rate limit exceeded. Includes `Retry-After` header.

---

### GET /api/v1/update/check

Reports the running firmware version. The reference server has no update feed
to consult, and says so — `update_available` is `false` and the digest and
signature fields are `null` rather than placeholders. (It used to advertise a
hard-coded "0.4.2" with a made-up sha256 and signature.) Updates arrive by
`POST /api/v1/update`.

**Request:**

```bash
curl -H "X-Canary-Token: cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f" \
     http://canary-a3f7.local/api/v1/update/check
```

**Response (200):**

```json
{
  "current_version": "0.4.1",
  "available_version": null,
  "update_available": false,
  "changelog": null,
  "sha256": null,
  "signature": null,
  "requires_usb": false,
  "note": "no update feed is configured on this device; updates arrive by POST /api/v1/update"
}
```

---

### POST /api/v1/update

Uploads and applies a firmware update.

**Rate limit:** 1 update per hour.

**Body:** the raw firmware binary (`application/octet-stream`) — **not** multipart/form-data. Maximum 16 MiB.

**Required header:** `X-Firmware-Signature` — a detached Ed25519 signature over the entire firmware payload, hex-encoded (64 bytes, 128 hex characters). Verified against the configured signing public key (`SECURACV_FW_SIGNING_PUBKEY`, PEM or 32-byte hex). There is no fallback: with the key unset every update is refused with `signing_key_unavailable`. The device's own key is never a stand-in for the release key — its private half lives on the device, so "signed by the device" would prove nothing about who built the firmware.

**Firmware layout:** the binary must begin with an 8-byte header — magic `SCV\x01` (bytes `0x53 0x43 0x56 0x01`), then `version_major`, `version_minor`, `version_patch` (one byte each), then 1 reserved byte. The version must be **strictly newer** than the installed firmware (anti-downgrade).

**Request:**

```bash
curl -X POST \
     -H "X-Canary-Token: cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f" \
     -H "X-Firmware-Signature: <128 hex chars>" \
     -H "Content-Type: application/octet-stream" \
     --data-binary @firmware-0.4.2.bin \
     http://canary-a3f7.local/api/v1/update
```

**Response (200):**

```json
{
  "ok": true,
  "message": "Updating firmware. Device will reboot.",
  "version": "0.4.2"
}
```

**Response (400):** `no_firmware` (empty body), `payload_too_large` (over 16 MiB), or `invalid_firmware_header` (missing/corrupt `SCV\x01` header).

**Response (403):** `signature_missing`, `signature_invalid`, or `version_downgrade` (version not strictly newer than the installed one).

**Response (429):** Update rate limit exceeded.

**Response (500):** `signing_key_unavailable` — no firmware signing key configured.

**Response (503):** An update is already in progress.
