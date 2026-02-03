# Canary Free Signals Inventory v0

Status: Draft v0.1
Intended Status: Normative
Alignment: Civica-Aligned, SecuraCV-Compatible
Last Updated: 2026-02-02

## 1. Purpose

Canary devices observe **environmental signals** to support human situational awareness **without identifying, profiling, or predicting people**.

Canary is a **witness**, not an analyst.
It records *what changed*, not *who caused it*.

This document enumerates **signals that are available "for free"** from Canary-class hardware (ESP32-class devices) and defines **strict constraints** on how those signals may be used.

These signals enable presence intelligence in environments where optical sensing is impractical, undesirable, or invasive — such as nighttime suburban monitoring without IR illumination.

---

## 2. Core Invariants (Non-Negotiable)

All Canary signal use MUST satisfy the following invariants. These extend and reinforce the Privacy Witness Kernel invariants defined in `spec/invariants.md`.

### Invariant A — No Identity

- No persistent identifiers
- No device fingerprinting
- No MAC address storage
- No cross-day correlation of entities
- No account linkage
- No vendor inference from OUI

**Enforcement**: Signal processors MUST NOT retain any field that could identify a specific device across observations.

### Invariant B — Local First

- Signals processed locally by default
- No cloud dependency for core function
- Export is explicit, human-initiated, and reviewable
- Network transmission follows mesh protocol (`spec/canary_mesh_network_v0.md`)

**Enforcement**: Signal processing code MUST NOT initiate external network connections.

### Invariant C — Ephemeral Memory

- Raw signals expire quickly (configurable, default < 60 seconds)
- Only aggregate, non-identifying summaries may persist
- Forgetting is a feature, not a bug

**Enforcement**: Ring buffers with strict TTL; zeroization on eviction.

### Invariant D — Human-in-the-Loop

- Canary does not accuse, escalate, or act
- Canary presents signals; humans interpret meaning
- No automated notifications to external parties
- No integration with access control or enforcement systems

**Enforcement**: Event outputs are advisory only; no actuator interfaces.

### Invariant E — Physics, Not Politics

- Canary observes physical phenomena
- Canary does not infer intent, legality, or morality
- No behavioral classification
- No threat scoring

**Enforcement**: Event vocabulary is strictly physical (e.g., "rf_presence_delta", not "intruder_detected").

### Invariant F — Symmetry

- All RF sources are treated identically
- No special casing of device types, manufacturers, or assumed owners
- No "known device" vs "unknown device" distinction in event output

**Enforcement**: Device lists are internal calibration only; never affect event semantics.

**Any feature that violates these invariants MUST NOT be implemented.**

---

## 3. Signal Categories Available "For Free"

### 3.1 RF Presence Signals (BLE)

**Hardware Source**: ESP32 BLE scanner (passive)

**What is observed**:

| Signal | Description | Update Rate |
|--------|-------------|-------------|
| `device_count_delta` | Change in number of advertising devices | 1-10 Hz |
| `rssi_trend` | Aggregate RSSI movement (approaching/departing) | 1 Hz |
| `dwell_class` | Temporal stability: `transient` / `lingering` / `sustained` | On change |
| `advertising_density` | Advertisements per second (normalized) | 1 Hz |

**What this means**:

- The RF environment transitioned between "empty" and "occupied"
- Presence stabilized or departed
- Advertising density changed (more/fewer active radios nearby)

**What it MUST NOT mean**:

- Who arrived
- Why they arrived
- Whether they are authorized
- Whether they are "good" or "bad"
- What type of device they carry

**Allowed Event Phrasing**:

```json
{"event": "rf_presence_started", "signal": "ble", "count_delta": 2}
{"event": "rf_presence_sustained", "signal": "ble", "dwell_class": "lingering"}
{"event": "rf_presence_ended", "signal": "ble", "count_delta": -2}
```

**Forbidden Event Phrasing**:

```json
{"event": "person_arrived"}
{"event": "homeowner_detected", "device": "iPhone"}
{"event": "suspicious_device", "vendor": "unknown"}
```

**Implementation Requirements**:

1. Scanner MUST NOT log MAC addresses to persistent storage
2. Scanner MUST NOT compute or store OUI-derived vendor information
3. Internal device tracking for deduplication MUST use ephemeral session tokens
4. Session tokens MUST rotate at least every 24 hours
5. Count deltas MUST be computed from anonymized aggregates

---

### 3.2 Wi-Fi Passive Signals (WAP)

**Hardware Source**: ESP32 Wi-Fi in promiscuous/monitor mode

**What is observed**:

| Signal | Description | Update Rate |
|--------|-------------|-------------|
| `probe_burst` | Sudden increase in probe request activity | On event |
| `rssi_impulse` | Short-lived RSSI spike in ambient | On event |
| `channel_activity` | RF energy on monitored channels | 1 Hz |

**What this means**:

- A device briefly became active nearby
- Something entered RF range and woke a radio
- Channel utilization changed

**Constraints**:

- No MAC storage
- No SSID tracking or logging
- No probe correlation across time windows
- No vendor inference
- Probe content MUST be discarded after burst detection

**Role in Presence Detection**:

Wi-Fi is an **impulse signal**:

- Good for arrival hints (devices probe on wake/movement)
- Poor for sustained presence (phones sleep, stop probing)
- Always secondary to BLE for presence duration

**Event Output**:

```json
{"event": "wifi_impulse", "type": "probe_burst", "intensity": "moderate"}
```

---

### 3.3 RF Meta-Signals (Environment Shape)

**Hardware Source**: ESP32 RF subsystem (derived)

**What is observed**:

| Signal | Description | Update Rate |
|--------|-------------|-------------|
| `noise_floor_delta` | Change in ambient RF noise | 0.1 Hz |
| `multipath_shift` | Variation in signal reflection patterns | On significant change |
| `rf_quieting` | Sudden reduction in ambient RF | On event |

**What this means**:

- Something physical changed nearby
- Large objects moved through the RF field
- Doors, vehicles, or structures altered reflections
- A source of RF interference appeared or departed

**Important Caveat**:

These signals are **coarse and ambiguous** by design.
They are context, not evidence.
They should weight other signals, not generate events alone.

**Use Case**:

- Corroborate BLE/WiFi presence signals
- Detect environmental changes when active signals are absent
- Identify "RF shadow" events (something blocked signals)

---

### 3.4 Timing Signals (Temporal Context)

**Hardware Source**: RTC, NTP (when available), internal monotonic clock

**What is observed**:

| Signal | Description | Storage |
|--------|-------------|---------|
| `event_time_bucket` | Coarse time window (per PWK invariants) | With event |
| `periodicity` | Deviation from historical patterns | Aggregate only |
| `inter_event_interval` | Time since last similar event | Ephemeral |

**What this means**:

- "This happened at an unusual time of day"
- "This normally happens but did not" (absence signal)
- "Events are occurring more/less frequently than baseline"

**Civica Rule**:

Time may be used to detect **anomaly**, never **intent**.

- Allowed: "Activity at unusual hour"
- Forbidden: "Suspicious late-night activity"

**Constraints**:

- Precise timestamps MUST NOT appear in exported events (per Invariant III of PWK)
- Time buckets follow existing 10-minute window standard
- Historical baselines MUST be stored as aggregate distributions, not event logs

---

### 3.5 Power & Electrical Context

**Hardware Source**: ADC on Vbus/battery, brownout detector

**What is observed**:

| Signal | Description | Update Rate |
|--------|-------------|-------------|
| `voltage_delta` | Change in supply voltage | 1 Hz |
| `brownout_event` | Voltage dropped below threshold | On event |
| `load_impulse` | Sudden current draw spike | On event |

**What this means**:

- Motors, tools, or large loads activated nearby
- Power instability occurred
- Possible grid or local electrical event

**Use Case**:

- Operational awareness only
- Self-health monitoring
- Corroborate physical presence (garage door, HVAC)

**Not For**:

- Forensic attribution
- Behavioral inference
- Utility monitoring

---

### 3.6 Thermal Delta Signals

**Hardware Source**: Internal temperature sensor (most ESP32 variants)

**What is observed**:

| Signal | Description | Update Rate |
|--------|-------------|-------------|
| `temp_delta` | Change in internal temperature over window | 0.1 Hz |
| `temp_trend` | Direction of temperature change | On significant change |

**What this means**:

- Environmental heating/cooling
- Possible proximity of heat sources (engines, sunlight, bodies)
- Enclosure ventilation change

**Rule**:

Only **delta** matters.
Absolute temperature MUST NOT be used for inference.
Temperature is never evidence of presence — only environmental context.

---

### 3.7 Silence & Absence Signals

**Hardware Source**: Derived from absence of expected signals

**What is observed**:

| Signal | Description | Update Rate |
|--------|-------------|-------------|
| `expected_signal_missing` | A routine signal did not occur | On detection |
| `rf_quiet_period` | Unusual absence of RF activity | On threshold |
| `pattern_break` | Historical pattern not matched | On detection |

**What this means**:

- A routine was interrupted
- The environment deviated by *absence*
- Something that usually happens did not

**Significance**:

This is one of Canary's most important signals — and the least abusable.
Absence cannot identify anyone.
Absence can only indicate deviation from baseline.

**Example Events**:

```json
{"event": "rf_quiet_anomaly", "duration_bucket": "extended", "baseline": "weekday_evening"}
{"event": "expected_pattern_missing", "pattern": "daily_presence", "deviation": "first_occurrence"}
```

---

### 3.8 Device Integrity Signals

**Hardware Source**: Boot reason register, watchdog, brownout detector

**What is observed**:

| Signal | Description | Storage |
|--------|-------------|---------|
| `boot_reason` | Why the device started | Health log |
| `watchdog_reset` | Unplanned restart occurred | Health log |
| `brownout_recovery` | Power was interrupted | Health log |
| `uptime` | Time since last boot | Ephemeral |

**Purpose**:

Trustworthiness of the witness itself.

Canary must be able to say:

> "I may have been impaired during this period."

**Use Case**:

- Evidence chain integrity
- Tamper detection (unexpected resets)
- Operational reliability metrics

---

## 4. Signal Fusion Model

### 4.1 Layered Confidence

Signals combine to increase or decrease confidence in environmental state:

```
Layer 0 (Highest Confidence):
  └─ BLE presence sustained + WiFi probe burst + Timing normal
      → "RF environment occupied, high confidence"

Layer 1 (Moderate Confidence):
  └─ BLE presence only, no WiFi corroboration
      → "RF presence detected, moderate confidence"

Layer 2 (Low Confidence):
  └─ WiFi impulse only, no BLE sustained
      → "Possible arrival, low confidence"

Layer 3 (Contextual Only):
  └─ Noise floor shift or temp delta only
      → "Environmental change, presence unknown"
```

### 4.2 Narrative Narrowing (Allowed, Limited)

Canary may support **narrative narrowing**, defined as:

> Reducing ambiguity between plausible explanations **without asserting truth**.

**Example**:

- BLE transient + WiFi probe burst + midday timing + short dwell
  → `{"narrative_hint": "delivery_like", "confidence": "plausible"}`

**Not**:

- `{"event": "delivery_occurred"}`
- `{"event": "amazon_arrived"}`
- `{"entity": "delivery_driver"}`

Narrative narrowing is always **advisory** and MUST:

1. Use hedge language ("like", "plausible", "consistent_with")
2. Never assert specific entities or identities
3. Never be used for access decisions
4. Be clearly marked as interpretive, not factual

---

## 5. Explicit Non-Goals (Hard Stops)

Canary MUST NOT be used to:

| Forbidden Use | Rationale |
|---------------|-----------|
| Build movement histories | Violates Invariant A (No Identity) |
| Create social graphs | Violates Invariant A |
| Detect specific organizations or agencies | Violates Invariant E (Physics, Not Politics) |
| Warn of raids or enforcement actions | Violates Invariant D and E |
| Enable evasion of lawful process | Violates Invariant E |
| Rank people by risk | Violates Invariant A and F |
| Predict behavior | Violates Invariant E |
| Fuse data across properties without consent | Violates Invariant B |
| Centralize intelligence | Violates Invariant B (Local First) |
| Distinguish "authorized" from "unauthorized" | Violates Invariant F (Symmetry) |

If a feature sounds like **analysis**, **prediction**, or **targeting**, it is out of scope.

---

## 6. Comparison: Canary vs. Surveillance Systems

### 6.1 Why This Is Not Palantir

| Palantir Model | Canary Model |
|----------------|--------------|
| Centralizes data | Local by default |
| Persists identity | Forgets aggressively |
| Correlates across domains | Refuses cross-correlation |
| Optimizes for prediction | Avoids prediction |
| Serves power asymmetrically | Preserves symmetry |
| Claims to know | Refuses to overclaim |

> **Palantir tries to know.**
> **Canary tries not to overclaim.**

### 6.2 Why This Is Not a Ring Doorbell

| Ring Model | Canary Model |
|------------|--------------|
| Cloud-dependent | Local-first |
| Continuous recording | Event-only, ephemeral |
| Facial recognition optional | Identity prohibited |
| Law enforcement portal | No external access |
| Vendor data mining | No data exfiltration |

---

## 7. Design Principles (Civica)

> **If a signal can be abused when aggregated, it must remain unaggregated.**

> **If a conclusion feels convenient, it is probably forbidden.**

> **The restraint is the product.**

---

## 8. RF Presence FSM (State Machine)

The RF Presence Finite State Machine parallels the vision-based presence FSM but operates on RF signals instead of visual detection.

### 8.1 States

```
RF_EMPTY          → No RF presence detected
RF_IMPULSE        → Brief signal (probe burst, transient BLE)
RF_PRESENCE       → Sustained RF activity detected
RF_DWELLING       → RF presence stable for threshold duration
RF_DEPARTING      → Signal strength declining, count dropping
```

### 8.2 Transitions

```
RF_EMPTY ──[ble_count > 0]──────────────────→ RF_IMPULSE
RF_EMPTY ──[wifi_probe_burst]───────────────→ RF_IMPULSE

RF_IMPULSE ──[sustained > PRESENCE_MS]──────→ RF_PRESENCE
RF_IMPULSE ──[timeout < IMPULSE_TIMEOUT]────→ RF_EMPTY

RF_PRESENCE ──[sustained > DWELL_MS]────────→ RF_DWELLING
RF_PRESENCE ──[count_delta < 0, rssi_down]──→ RF_DEPARTING
RF_PRESENCE ──[timeout > LOST_MS]───────────→ RF_EMPTY

RF_DWELLING ──[count_delta < 0]─────────────→ RF_DEPARTING
RF_DWELLING ──[timeout > LOST_MS]───────────→ RF_EMPTY

RF_DEPARTING ──[count = 0, timeout]─────────→ RF_EMPTY
RF_DEPARTING ──[count stable]───────────────→ RF_PRESENCE
```

### 8.3 Timing Constants (Configurable)

| Constant | Default | Description |
|----------|---------|-------------|
| `IMPULSE_TIMEOUT_MS` | 5000 | Max duration of impulse state |
| `PRESENCE_THRESHOLD_MS` | 10000 | Time to confirm presence |
| `DWELL_THRESHOLD_MS` | 60000 | Time to confirm dwelling |
| `LOST_TIMEOUT_MS` | 30000 | Time before declaring empty |
| `DEPARTING_CONFIRM_MS` | 15000 | Time to confirm departure |

### 8.4 Events Emitted

| Event | Trigger | Contains |
|-------|---------|----------|
| `rf_impulse` | Transition to RF_IMPULSE | signal_type, intensity |
| `rf_presence_started` | Transition to RF_PRESENCE | count_delta, confidence |
| `rf_dwell_started` | Transition to RF_DWELLING | dwell_class |
| `rf_departing` | Transition to RF_DEPARTING | rssi_trend |
| `rf_presence_ended` | Transition to RF_EMPTY | duration_bucket |

---

## 9. Implementation Notes

### 9.1 BLE Scanner Privacy Mode

The BLE scanner MUST operate in "privacy mode":

```cpp
// Conceptual — actual implementation in firmware
struct RfObservation {
  uint32_t timestamp_ms;       // Ephemeral, not logged
  uint8_t  device_count;       // Aggregate only
  int8_t   rssi_max;           // No device association
  int8_t   rssi_mean;          // Aggregate
  uint8_t  adv_per_second;     // Density metric
  // NO: mac_address, device_name, oui, vendor
};
```

### 9.2 Session Token Rotation

For internal deduplication (counting unique devices within a window):

```cpp
// Tokens are derived from MAC but not reversible
// and rotate every SESSION_ROTATE_MS (default 4 hours)
uint32_t derive_session_token(const uint8_t mac[6], uint32_t session_epoch) {
  // HMAC-based derivation with rotating key
  // Token is valid only within current session
  // Cannot be correlated across sessions
}
```

### 9.3 Memory Layout

| Buffer | Size | TTL | Purpose |
|--------|------|-----|---------|
| `rf_observation_ring` | 64 entries | 60s | Raw signal aggregates |
| `session_token_map` | 32 entries | 4h | Deduplication within session |
| `presence_state` | 1 entry | N/A | Current FSM state |
| `baseline_histogram` | 24 entries | 7d | Hourly presence baseline |

### 9.4 NVS Storage Keys

```
NVS Key                | Size    | Description
-----------------------|---------|---------------------------
rf_presence_enabled    | 1 byte  | Feature flag
rf_presence_thresholds | 20 bytes| Timing constants
rf_baseline_histogram  | 96 bytes| Aggregate hourly baselines
rf_session_epoch       | 4 bytes | Current session rotation
```

---

## 10. Conformance

An implementation conforms to this specification if it:

1. Implements all signal categories marked as REQUIRED (§3.1, §3.7, §3.8)
2. Enforces all Core Invariants (§2) in code, not configuration
3. Never stores MAC addresses, device names, or OUI data persistently
4. Implements session token rotation with maximum 24-hour lifetime
5. Uses coarse time buckets for all exported events
6. Produces only the allowed event vocabulary
7. Passes the RF Privacy Conformance Test Suite (when available)

Failure to meet any invariant constitutes non-conformance.

---

## 11. Relationship to PWK Invariants

This specification extends and reinforces:

| PWK Invariant | Free Signals Extension |
|---------------|------------------------|
| I — No Raw Export | RF aggregates only, never raw scan data |
| II — No Identity Substrate | Rotating session tokens, no stable IDs |
| III — Metadata Minimization | Coarse time buckets, aggregate counts |
| IV — Local Ownership | Local processing, no cloud dependency |
| V — Break-Glass | Not applicable (no sealed evidence from RF) |
| VI — No Retroactive Expansion | Baseline histograms bound to collection epoch |
| VII — Non-Queryability | No search by device, only sequential review |

---

## 12. Closing

Canary is intentionally *boring*.

It listens.
It notices.
It records environmental deltas.
It forgets the details.
It does not judge.

That restraint is the product.

---

## 13. Changelog

- v0.1 (2026-02-02): Initial draft
