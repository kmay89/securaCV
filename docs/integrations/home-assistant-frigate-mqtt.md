# Home Assistant + Frigate MQTT Integration

## 1) Overview

This guide connects **Frigate NVR → MQTT → SecuraCV (PWK) → Home Assistant** without duplicating setup details already covered elsewhere. For base installation and configuration, follow the primary docs and return here for the integration flow and verification steps:

- Home Assistant integration + add-on setup: `docs/homeassistant_setup.md`
- Frigate-specific configuration and invariants: `docs/frigate_integration.md`

This guide assumes you will keep the same MQTT broker and topic values across Frigate, the PWK add-on, and Home Assistant. The integration remains privacy-preserving because PWK only consumes Frigate’s event metadata and strips identifiers before logging. See `docs/frigate_integration.md` for the exact data contract and invariant rationale.

---

## 2) Architecture Diagram

```
[Frigate NVR]
   |  (MQTT events: frigate/events)
   v
[MQTT Broker] <-------------------------------+
   |                                          |
   |  (Frigate events -> PWK frigate_bridge)  |
   v                                          |
[SecuraCV / PWK] --(MQTT discovery + state)---+
   |
   |  (Event API)
   v
[Home Assistant]
```

---

## 3) Prerequisites

- Home Assistant running with access to **Settings → Devices & Services**.
- Frigate NVR configured and publishing events to MQTT.
- A reachable MQTT broker (e.g., HA’s built-in `core-mosquitto`).
- SecuraCV/PWK add-on or container configured for **Frigate mode**.
- A device key generated for the PWK configuration.

For exact configuration fields (topics, credentials, mode, device key, and MQTT publish settings), use the reference in `docs/homeassistant_setup.md` and `docs/frigate_integration.md` to avoid configuration drift.

### TLS Configuration (Optional)

If your MQTT broker requires TLS, enable TLS for the bridges and provide certificates as needed:

| Environment Variable | Description |
|---|---|
| `MQTT_USE_TLS` | Enable TLS (required for `mqtts://` brokers) |
| `MQTT_TLS_CA_PATH` | Path to PEM-encoded CA certificate |
| `MQTT_TLS_CLIENT_CERT_PATH` | Path to PEM-encoded client certificate (mutual TLS) |
| `MQTT_TLS_CLIENT_KEY_PATH` | Path to PEM-encoded client private key (mutual TLS) |

---

## 4) Quickstart Commands

Run these from a machine that can reach your MQTT broker and PWK runtime. Substitute hostnames, usernames, and passwords as needed.

```bash
# Generate a device key for the PWK config
openssl rand -hex 32
```

```bash
# (Optional) Watch Frigate events to confirm publishing
mosquitto_sub -h core-mosquitto -t frigate/events -v
```

```bash
# (Optional) Watch PWK MQTT discovery/state topics if enabled
mosquitto_sub -h core-mosquitto -t 'homeassistant/#' -v
mosquitto_sub -h core-mosquitto -t 'witness/#' -v
```

> Note: The exact topic prefix and discovery prefix must match your add-on configuration. See `docs/homeassistant_setup.md` and `docs/frigate_integration.md` for the default values and the full checklist.

---

## 5) Home Assistant GUI Steps

### A. Add the MQTT Integration
1. Go to **Settings → Devices & Services**.
2. Click **Add Integration**.
3. Search for and select **MQTT**.
4. Enter your MQTT broker host/port and credentials (if required).
5. Confirm it connects successfully.

### B. Listen to a Topic in Developer Tools
1. Go to **Developer Tools → MQTT**.
2. In **Listen to a topic**, enter `frigate/events`.
3. Click **Start Listening**.
4. Trigger an object detection in Frigate and confirm JSON events appear in the log output.

### C. Confirm Frigate Is Publishing
1. Stay in **Developer Tools → MQTT**.
2. If you enabled PWK MQTT publishing, listen to `witness/#` and `homeassistant/#` in separate sessions (or switch topics).
3. Confirm PWK is publishing discovery and state messages for SecuraCV entities (the broker should show retained discovery payloads and state updates).

### D. Add a Dashboard Card for SecuraCV State
1. Go to **Overview → Edit Dashboard → Add Card**.
2. Choose **Entities** (or **Sensor**).
3. Select the SecuraCV entities created via MQTT discovery (e.g., zone state or event count sensors).
4. Save the card and verify live updates after new Frigate events.

### E. View Home Assistant Logs
1. Go to **Settings → System → Logs**.
2. Filter for **MQTT**, **SecuraCV**, or **Frigate** to see connection and entity update entries.

---

## 6) Verify Pipeline

From the repository root (or wherever your verification script lives), run:

```bash
./verify_pipeline.sh
```

**Success looks like**: the script completes with exit code `0` and prints a summary indicating the MQTT → PWK → HA pipeline is healthy (e.g., all required topics observed and at least one SecuraCV state update received). If the script reports missing topics or connection failures, use the troubleshooting list below to resolve them.

---

## 7) Troubleshooting (Common Issues)

1. **MQTT integration fails to connect in HA**
   - Verify the broker host/port and credentials match Frigate and PWK settings.
   - Confirm the broker is reachable from Home Assistant.

2. **No messages appear on `frigate/events` in Developer Tools**
   - Ensure Frigate is configured to publish MQTT and is actively detecting events.
   - Validate the topic in Frigate matches the subscription (`frigate/events` by default).

3. **PWK starts but logs show it cannot subscribe to Frigate topics**
   - Check `frigate.mqtt_host`, `frigate.mqtt_port`, and credentials in the PWK config.
   - Confirm the broker allows the PWK client to subscribe.

4. **No SecuraCV entities appear in HA after enabling MQTT discovery**
   - Ensure `mqtt_publish.enabled: true` in the PWK config.
   - Confirm the `mqtt_publish.discovery_prefix` matches Home Assistant’s discovery prefix (default `homeassistant`).

5. **SecuraCV sensors exist but never update**
   - Verify PWK is receiving Frigate events and publishing to `mqtt_publish.topic_prefix`.
   - Confirm the MQTT broker isn’t dropping retained or QoS 1 messages.

6. **Frigate events are present but PWK logs show skipped events**
   - Validate `frigate.labels`, `frigate.min_confidence`, and `frigate.cameras` filters.
   - Check for Frigate `false_positive` flags that are excluded by design.

7. **Home Assistant dashboard card shows `unknown` or `unavailable`**
   - Confirm the MQTT discovery payloads are retained by the broker.
   - Check HA logs for MQTT availability or entity registration warnings.

8. **Multiple brokers or duplicated topics**
   - Ensure Frigate, PWK, and HA all point to the same broker and consistent topic prefixes.

9. **MQTT authentication errors**
   - Re-check usernames/passwords in Frigate, PWK, and the HA MQTT integration.
   - Ensure the broker user is authorized to publish and subscribe to the required topics.

10. **Events appear delayed or missing after HA restart**
    - Confirm the broker persists retained discovery topics and that HA reconnects after restart.
    - Verify the PWK add-on is running and publishing LWT availability status.

11. **verify_pipeline.sh reports missing topics**
    - Confirm you can see Frigate events in **Developer Tools → MQTT**.
    - Verify the PWK MQTT publish settings match the broker and prefixes in `docs/homeassistant_setup.md` and `docs/frigate_integration.md`.

12. **Frigate + PWK integration works but the PWK sealed log is empty**
    - Confirm the PWK database path and permissions for the add-on/container.
    - Use the `log_verify` tool described in `log_verify_README.md` if you need integrity checks.

---

## 8) Next Steps

- Review and align your configuration with the authoritative setup docs:
  - `docs/homeassistant_setup.md` (install + add-on config)
  - `docs/frigate_integration.md` (Frigate mode constraints and MQTT options)
- Add a dedicated dashboard view for SecuraCV sensors and zone states.
- If you need deeper verification, run `log_verify` as documented in `log_verify_README.md`.
