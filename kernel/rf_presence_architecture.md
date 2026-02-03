# Canary RF Presence — Architecture

Status: Draft v0.1
Intended Status: Normative (Internal Constitution)
Last Updated: 2026-02-02

This document defines the architectural boundaries for RF-based presence detection in Canary devices.
It complements `kernel/architecture.md` (the witnessd constitution) with constraints specific to passive RF observation.

RF presence detection enables situational awareness in environments where optical sensing is impractical, undesirable, or invasive — such as nighttime monitoring without IR illumination, or privacy-respecting perimeter awareness.

---

## 0. Binding References

This architecture MUST enforce:

- `spec/invariants.md` (I–VII)
- `spec/canary_free_signals_v0.md` (Invariants A–F)
- `spec/canary_mesh_network_v0.md` (for multi-device coordination)

Where this document names a boundary, it is a boundary **in code**, not convention.

---

## 1. Component Isolation Diagram

```
                    (LOCAL ONLY — Canary Device)
┌─────────────────────────────────────────────────────────────────┐
│                        RF Presence Subsystem                     │
│                      (Canary Firmware TCB)                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐       │
│  │  BLE Scanner │    │ WiFi Monitor │    │  Env Sensors │       │
│  │   (passive)  │    │  (passive)   │    │ (temp,power) │       │
│  └──────┬───────┘    └──────┬───────┘    └──────┬───────┘       │
│         │                   │                   │               │
│         └─────────┬─────────┴───────────────────┘               │
│                   │                                              │
│                   ▼                                              │
│         ┌─────────────────────┐                                  │
│         │   Signal Anonymizer  │  ← MAC stripped, tokens rotated │
│         │   (Privacy Barrier)  │                                 │
│         └──────────┬──────────┘                                  │
│                    │                                             │
│                    ▼                                             │
│         ┌─────────────────────┐                                  │
│         │   RF Observation     │  ← Aggregates only              │
│         │   Ring Buffer        │    (count, RSSI, density)       │
│         └──────────┬──────────┘                                  │
│                    │                                             │
│                    ▼                                             │
│         ┌─────────────────────┐                                  │
│         │   RF Presence FSM    │  ← State machine                │
│         │   (State Engine)     │    (empty/impulse/presence/dwell)│
│         └──────────┬──────────┘                                  │
│                    │                                             │
│                    ▼                                             │
│         ┌─────────────────────┐                                  │
│         │  Signal Fusion       │  ← Multi-signal correlation     │
│         │  (Confidence Layer)  │    (BLE + WiFi + Env)           │
│         └──────────┬──────────┘                                  │
│                    │                                             │
│                    ▼                                             │
│         ┌─────────────────────┐                                  │
│         │  Event Contract      │  ← Vocabulary enforcement       │
│         │  Enforcer            │    (rejects forbidden fields)   │
│         └──────────┬──────────┘                                  │
│                    │                                             │
│                    ▼                                             │
│              RF Events (API)                                     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
        │                                    │
        │                                    │
        ▼                                    ▼
   Local Display                      Mesh Network
   (Status LED, Web UI)               (Opera Protocol)
```

### Isolation Walls (must hold)

1. **Privacy Barrier**: MAC addresses, device names, and OUI data MUST NOT cross from Scanner into Observation Buffer
2. **No Raw Scan Export**: Raw scan results MUST NOT be exposed via any API
3. **Aggregate Only**: Only anonymized aggregates (count, RSSI statistics) may persist
4. **Event Vocabulary Lock**: Only allowed event types may exit the Event Contract Enforcer

---

## 2. Data Flow Rules (RF Signal Discipline)

### 2.1 Where raw RF data may exist

Raw RF data (MAC addresses, device names, SSIDs, probe contents) may exist ONLY:

- Transiently in scanner hardware buffers
- Briefly in the Signal Anonymizer during token derivation

Raw RF data MUST NOT:

- Be written to NVS or any persistent storage
- Be exposed via any API endpoint
- Be logged
- Be transmitted over mesh network
- Cross the Privacy Barrier in identifiable form

### 2.2 Signal Anonymizer (Privacy Barrier)

The Signal Anonymizer is the **single choke point** for all RF identity data.

```
Input (forbidden to persist):          Output (allowed to persist):
├─ MAC address [6 bytes]        →      ├─ Session token [4 bytes]
├─ Device name [32 bytes]       →      │   (rotates every SESSION_ROTATE_MS)
├─ OUI/Vendor [3 bytes]         →      │
└─ Probe SSIDs [variable]       →      └─ (discarded)

Token derivation:
  session_key = HKDF("canary:session:v0", device_secret, session_epoch)
  token = HMAC-SHA256(session_key, mac_address)[0:4]

Rotation:
  session_epoch increments every SESSION_ROTATE_MS (default 4 hours)
  Old tokens become invalid, cannot correlate across sessions
```

### 2.3 RF Observation Buffer

The observation buffer stores ONLY aggregated, anonymized data:

```cpp
struct RfObservation {
  uint32_t timestamp_ms;       // Internal only, not exported
  uint8_t  ble_device_count;   // Unique tokens in window
  int8_t   ble_rssi_max;       // Strongest signal
  int8_t   ble_rssi_mean;      // Average signal
  uint8_t  ble_adv_density;    // Advertisements/second (0-255)
  uint8_t  wifi_probe_bursts;  // Probe burst count in window
  int8_t   wifi_rssi_peak;     // Peak probe RSSI
  uint8_t  env_flags;          // Environmental signals (temp delta, power)
};
```

Buffer constraints:

| Property | Value | Rationale |
|----------|-------|-----------|
| Max entries | 64 | Bounded memory |
| Entry TTL | 60 seconds | Ephemeral by design |
| Eviction | Zeroize on overwrite | Minimize exposure |

### 2.4 Session Token Lifecycle

```
Boot
  │
  ├──[Load session_epoch from NVS or generate new]
  │
  ▼
Active Session
  │
  ├──[Derive tokens for incoming MACs]
  ├──[Use tokens for deduplication only]
  ├──[Tokens never leave device]
  │
  ├──[SESSION_ROTATE_MS elapsed]────────────────┐
  │                                              │
  ▼                                              │
Rotate Session                                   │
  │                                              │
  ├──[Increment session_epoch]                   │
  ├──[Save new epoch to NVS]                     │
  ├──[Clear token→count mappings]    ◄───────────┘
  ├──[Old tokens now meaningless]
  │
  ▼
New Session (tokens uncorrelatable with previous)
```

---

## 3. Trust Boundaries

### 3.1 RF Subsystem TCB (trusted)

The RF presence TCB includes:

- Signal Anonymizer (token derivation, rotation)
- Observation Buffer management
- RF Presence FSM (state transitions)
- Event Contract Enforcer (vocabulary validation)

### 3.2 Scanners (partially trusted)

BLE and WiFi scanners are treated as potentially leaky:

- They MAY access raw RF data
- They MUST hand off to Anonymizer immediately
- They MUST NOT log, cache, or export raw data
- They MUST NOT make network calls

### 3.3 Signal Fusion (trusted, constrained)

The fusion layer:

- MAY combine multiple signal sources
- MAY apply temporal pattern detection
- MUST NOT introduce identity signals
- MUST NOT predict behavior or intent
- MUST output only allowed event vocabulary

### 3.4 External Interfaces (untrusted)

Web UI, mesh network, and mobile apps:

- Can request current RF presence state
- Can request historical event stream (events only, not raw)
- Cannot access raw scan data
- Cannot access session tokens

---

## 4. Enforcement Map (Where Each Invariant Lives)

### Signal Invariant A — No Identity

Enforced by:

- `SignalAnonymizer` strips MAC before buffer insertion
- Session tokens rotate, preventing cross-session correlation
- No OUI/vendor derivation code exists
- Event Contract rejects device identifiers

### Signal Invariant B — Local First

Enforced by:

- No cloud endpoints defined
- Mesh transmission uses Opera protocol (local only)
- Export requires explicit user action

### Signal Invariant C — Ephemeral Memory

Enforced by:

- 60-second TTL on observation buffer
- 4-hour session token rotation
- Zeroization on buffer eviction

### Signal Invariant D — Human-in-the-Loop

Enforced by:

- No automated external notifications
- Events are advisory only
- No actuator interfaces (no door locks, no alarms)

### Signal Invariant E — Physics, Not Politics

Enforced by:

- Event vocabulary is physical ("rf_presence_started", not "intruder")
- No threat scoring
- No behavioral classification

### Signal Invariant F — Symmetry

Enforced by:

- All RF sources processed identically
- No "known device" special casing in events
- Trust lists are internal calibration only

---

## 5. RF Presence FSM Architecture

### 5.1 State Definitions

```
┌─────────────┐
│  RF_EMPTY   │  No significant RF presence
└──────┬──────┘
       │
       │ [ble_count > 0] OR [wifi_probe_burst]
       ▼
┌─────────────┐
│ RF_IMPULSE  │  Brief signal detected, awaiting confirmation
└──────┬──────┘
       │
       │ [sustained > PRESENCE_THRESHOLD_MS]
       ▼
┌─────────────┐
│ RF_PRESENCE │  Confirmed RF activity
└──────┬──────┘
       │
       │ [sustained > DWELL_THRESHOLD_MS]
       ▼
┌─────────────┐
│ RF_DWELLING │  Stable, sustained presence
└──────┬──────┘
       │
       │ [count_delta < 0] AND [rssi_declining]
       ▼
┌─────────────┐
│ RF_DEPARTING│  Signals weakening, count dropping
└──────┬──────┘
       │
       │ [count = 0] OR [timeout > LOST_TIMEOUT_MS]
       ▼
┌─────────────┐
│  RF_EMPTY   │  Return to baseline
└─────────────┘
```

### 5.2 Signal Weights

```
Signal Type          Weight    Role
─────────────────────────────────────────
BLE sustained        1.0       Primary presence indicator
BLE transient        0.3       Arrival hint
WiFi probe burst     0.5       Arrival trigger, short-lived
WiFi quiet           0.2       Departure hint
Noise floor shift    0.1       Environmental context only
Temp delta           0.05      Environmental context only
Silence/absence      0.8       Pattern deviation (high value)
```

### 5.3 Confidence Calculation

```
presence_confidence =
  (ble_sustained * 1.0) +
  (ble_transient * 0.3) +
  (wifi_burst * 0.5) +
  (noise_shift * 0.1) +
  (temp_delta * 0.05)

confidence_class:
  >= 0.8  → "high"
  >= 0.5  → "moderate"
  >= 0.2  → "low"
  <  0.2  → "uncertain"
```

---

## 6. Forbidden Implementations (PR Rejection List)

The following are forbidden and will be rejected:

1. **MAC address logging** — Any code that persists MAC addresses
2. **Vendor fingerprinting** — OUI lookups or device type inference
3. **Device names in events** — Bluetooth names in exported data
4. **Cross-session correlation** — Tokens valid beyond rotation window
5. **Behavioral prediction** — "User is likely to..." outputs
6. **Threat scoring** — Risk levels, suspicion scores
7. **Known device exclusion** — Treating "your phone" differently in events
8. **Cloud telemetry** — Sending any RF data to remote endpoints
9. **SSID tracking** — Storing or correlating probe SSIDs
10. **Movement tracking** — Building trajectories from RSSI

If you need one of these, you are building a surveillance system, not a witness.

---

## 7. Module Interface (for future extensibility)

If RF signal processing is modularized in the future:

### 7.1 Allowed Module Inputs

```cpp
struct RfSignalView {
  // Aggregates only — no identifiers
  uint8_t  device_count;
  int8_t   rssi_statistics[4];  // min, max, mean, stddev
  uint8_t  signal_density;
  uint32_t presence_duration_ms;
  uint8_t  confidence_class;    // 0-3 (uncertain to high)

  // Temporal context
  uint8_t  time_bucket;         // Coarse, per PWK invariant
  bool     is_baseline_deviation;
};
```

### 7.2 Forbidden Module Inputs

```cpp
// NEVER ALLOWED in module interface
struct ForbiddenRfData {
  uint8_t  mac_address[6];      // FORBIDDEN
  char     device_name[32];     // FORBIDDEN
  char     ssid[32];            // FORBIDDEN
  uint8_t  oui[3];              // FORBIDDEN
  uint32_t precise_timestamp;   // FORBIDDEN
};
```

### 7.3 Allowed Module Outputs

```cpp
struct RfEventCandidate {
  const char* event_name;       // From allowed vocabulary only
  uint8_t     confidence;       // 0-100
  const char* signal_source;    // "ble", "wifi", "fused"
  int8_t      count_delta;      // Change in presence count
  const char* narrative_hint;   // Optional, hedge language only
};
```

---

## 8. Conformance Tests (Required)

A conforming implementation MUST pass:

1. **No MAC Persistence Test**: NVS dump contains no MAC addresses
2. **Token Rotation Test**: Tokens become invalid after rotation
3. **No Cross-Correlation Test**: Same device, different sessions → different tokens
4. **Event Vocabulary Test**: Only allowed event names emitted
5. **No Identity Field Test**: Events contain no device identifiers
6. **Aggregate Only Test**: Buffer contains only aggregates
7. **Ephemeral Buffer Test**: Entries expire within TTL

---

## 9. Integration with Vision Presence

When both RF and Vision presence are available:

### 9.1 Fusion Rules

```
RF + Vision Agreement:
  → Highest confidence events

RF only (night mode, no IR):
  → RF events with "rf_only" marker

Vision only (RF disabled):
  → Vision events, RF fields null

Disagreement (RF presence, no vision):
  → Both events emitted, "uncorrelated" marker
  → Human interprets meaning
```

### 9.2 No Cross-Correlation of Identity

RF presence MUST NOT be used to identify the person seen by vision.
Vision presence MUST NOT be used to identify the device seen by RF.

These are independent observations of the environment, not linked profiles.

---

## 10. Summary

The RF Presence subsystem extends Canary's witness capability to non-optical domains.

It observes the RF environment.
It notices when that environment changes.
It forgets the details.
It does not identify who or what caused the change.

The restraint is the architecture.
