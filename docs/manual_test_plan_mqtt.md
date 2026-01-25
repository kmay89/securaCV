# Manual Test Plan: Frigate → PWK → Home Assistant (MQTT)

## Purpose
Validate the Frigate → MQTT → `frigate_bridge` → PWK event log → `event_mqtt_bridge` → Home Assistant flow, including discovery and state topics plus expected entity IDs.

## References (Expected Topics & Entities)
Use these documented defaults when validating:

- **Frigate event topic:** `frigate/events` (Frigate publishes detections here).【F:docs/frigate_integration.md†L37-L53】
- **PWK MQTT publish prefixes:**
  - `mqtt_publish.topic_prefix`: `witness` (state topics).【F:docs/frigate_integration.md†L147-L173】
  - `mqtt_publish.discovery_prefix`: `homeassistant` (HA discovery).【F:docs/frigate_integration.md†L147-L173】
- **PWK state topics (examples):**
  - `witness/status` (availability)
  - `witness/last_event`
  - `witness/zone/<name>/count`
  - `witness/zone/<name>/motion`
  - `witness/zone/<name>/event`
  - `witness/events` (firehose).【F:docs/homeassistant_setup.md†L296-L314】
- **Auto-created entity IDs (per zone + global):**
  - `sensor.pwk_<zone>_events`
  - `binary_sensor.pwk_<zone>_motion`
  - `sensor.pwk_last_event`.【F:docs/homeassistant_setup.md†L266-L277】

## Preconditions
- Frigate is running and publishing MQTT events to `frigate/events`.
- PWK add-on is configured in **frigate** mode with the same broker.
- MQTT broker is reachable by both PWK and Home Assistant.
- `mqtt_publish.enabled: true` so `event_mqtt_bridge` publishes discovery + state topics.

## Test Steps

### 1) Verify Frigate publishes to MQTT
**Action**
- Subscribe to the Frigate event topic (example):
  - `frigate/events`

**Expected**
- New detection events appear on `frigate/events` in JSON payloads (topic name matches the documented default).【F:docs/frigate_integration.md†L37-L53】

### 2) Verify `frigate_bridge` subscribes and logs events
**Action**
- Ensure `frigate_bridge` is running (PWK add-on in `mode: "frigate"`).
- Generate a Frigate detection event (e.g., walk through a camera zone).

**Expected**
- `frigate_bridge` receives the event from `frigate/events` and logs a sanitized PWK event (no object IDs or raw media). The event is stored in the PWK log and should later surface via `event_mqtt_bridge` in Step 3.

### 3) Verify `event_mqtt_bridge` publishes discovery + state topics
**Action**
- Start `event_mqtt_bridge` (daemon mode if applicable).
- Subscribe to discovery and state topics using the documented prefixes:
  - Discovery: `homeassistant/#`
  - State: `witness/#`

**Expected**
- Discovery config topics appear under `homeassistant/...` (discovery prefix).【F:docs/frigate_integration.md†L147-L173】
- State topics publish under the `witness` prefix, including:
  - `witness/status` (online/offline)
  - `witness/last_event`
  - `witness/zone/<name>/count`
  - `witness/zone/<name>/motion`
  - `witness/zone/<name>/event`
  - `witness/events` (firehose).【F:docs/homeassistant_setup.md†L296-L314】

### 4) Verify Home Assistant entities appear
**Action**
- Open Home Assistant → **Settings → Devices & Services → MQTT** (or entity list).
- Search for the expected entity IDs.

**Expected**
- Entities auto-created via MQTT Discovery:
  - `sensor.pwk_<zone>_events`
  - `binary_sensor.pwk_<zone>_motion`
  - `sensor.pwk_last_event`.【F:docs/homeassistant_setup.md†L266-L277】
- `sensor.pwk_last_event` updates when new events are logged (from Step 2).

## Notes / Troubleshooting
- If discovery entities do not appear, confirm `mqtt_publish.discovery_prefix` and `mqtt_publish.topic_prefix` are aligned with the expected values (`homeassistant` + `witness`).【F:docs/frigate_integration.md†L147-L173】
- If state topics are missing, verify `mqtt_publish.enabled: true` and the broker connection details.
