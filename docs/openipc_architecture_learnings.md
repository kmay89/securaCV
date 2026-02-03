# Architectural Learnings from OpenIPC

Status: Informational
Intended Status: Reference (Non-Normative)
Last Updated: 2026-02-03

## Purpose

This document captures **architectural patterns and design principles** observed in the
[OpenIPC](https://github.com/openipc) open-source IP camera firmware project that are
applicable to SecuraCV Canary development.

**Important:** We are not using OpenIPC code or libraries directly. We are learning from
their battle-tested approaches to embedded camera systems and applying those lessons
within SecuraCV's fundamentally different privacy-preserving architecture.

OpenIPC is MIT-licensed. If we ever directly reuse any code (unlikely given the
architecture gap), we will cite accordingly. For now, this is purely knowledge
transfer — design patterns, not implementations.

This document is **informational** and does not override normative specifications. For
binding constraints, see:
- `spec/invariants.md`
- `kernel/architecture.md`
- `firmware/ARCHITECTURE.md`

---

## 1. Context

### 1.1 What SecuraCV Canary Is

SecuraCV Canary is a **Privacy Witness Kernel** device on ESP32-S3. It:
- Outputs **semantic events**, never video/pixels
- Signs every record with **Ed25519** and chains them with **SHA256 hash chains**
- Stores keys in **NVS** with per-device hardware RNG provisioning
- Runs a **WiFi AP dashboard** for configuration and live "peek" preview
- Logs to **SD card** with tamper-evident chains
- Captures **GPS telemetry** via UART (L76K GNSS module)
- Follows the philosophy: **"The safest capabilities are the ones that don't exist"**

### 1.2 What OpenIPC Is

OpenIPC replaces closed vendor firmware on commercial IP cameras with open-source Linux
(Buildroot-based). Key sub-projects studied:
- **divinus** — Multi-platform streamer using a per-SoC HAL with runtime dynamic linking
- **smolrtsp** — Zero-copy, network-agnostic RTSP 1.0 library for embedded devices
- **ipctool** — Hardware detection/identification tool with layered HAL architecture

---

## 2. Applicable Architectural Patterns

### 2.1 Hardware Abstraction Layer (HAL) via Function Pointers

**Source pattern:** Both `divinus` and `ipctool` use function-pointer tables to abstract
hardware-specific operations. Each supported SoC family gets its own HAL implementation.
The core logic calls through function pointers, never touching hardware directly.

**ipctool structure example:**
```
Core Detection Logic
    ↓ calls through function pointers
HAL Interface (sensor_detect_fn, chip_identify_fn)
    ↓ dispatched at runtime
Platform: HiSilicon | SigmaStar | Ingenic | Goke | ...
```

**Application to SecuraCV Canary:**

SecuraCV currently targets only XIAO ESP32-S3 Sense, but should be structured to
accommodate:
- Different ESP32 variants (ESP32-S3, ESP32-C3, ESP32-C6)
- Different camera modules (OV2640 on XIAO Sense, Grove Vision AI V2 for person counting)
- Different GNSS modules (L76K now, potentially others)
- Different storage backends (SD card, SPIFFS, LittleFS)
- Different secure elements (software Ed25519 now, potentially hardware crypto in future)

**Implementation guidance:**

```c
// Define a HAL interface for each subsystem
// These are NOT OpenIPC code — they are SecuraCV-specific abstractions
// inspired by the pattern of decoupling core logic from hardware

typedef struct {
    bool (*init)(void);
    bool (*capture_frame)(uint8_t **buf, size_t *len);  // For peek only
    void (*release_frame)(uint8_t *buf);
    bool (*detect_objects)(semantic_event_t *events, size_t *count);
    void (*deinit)(void);
    const char *name;
} securacv_vision_hal_t;

typedef struct {
    bool (*init)(uint32_t baud);
    bool (*get_fix)(gps_fix_t *fix);
    bool (*is_valid)(const gps_fix_t *fix);
    float (*get_hdop)(void);
    void (*enter_sleep)(void);
    void (*wake)(void);
    const char *name;
} securacv_gnss_hal_t;

typedef struct {
    bool (*init)(void);
    bool (*write_record)(const witness_record_t *rec);
    bool (*read_record)(uint32_t seq, witness_record_t *rec);
    uint32_t (*get_record_count)(void);
    bool (*verify_chain)(uint32_t start_seq, uint32_t end_seq);
    bool (*is_healthy)(void);
    const char *name;
} securacv_storage_hal_t;

typedef struct {
    bool (*init)(void);
    bool (*generate_keypair)(uint8_t *pubkey, uint8_t *privkey);
    bool (*sign)(const uint8_t *msg, size_t msg_len,
                 const uint8_t *privkey, uint8_t *sig);
    bool (*verify)(const uint8_t *msg, size_t msg_len,
                   const uint8_t *pubkey, const uint8_t *sig);
    bool (*hash)(const uint8_t *data, size_t len, uint8_t *digest);
    bool (*self_test)(void);
    const char *name;
} securacv_crypto_hal_t;
```

**Why this matters for SecuraCV:** If you ever want a "Canary Pro" on different hardware,
or swap in a hardware secure element, or switch from software Ed25519 to a TPM — you
change one HAL implementation, not the entire firmware.

---

### 2.2 Sequential Hardware Detection Pipeline

**Source pattern:** `ipctool` follows a strict detection sequence: identify SoC first →
then detect sensor → then enumerate peripherals → then output structured report. Each
step depends on the previous step's results. The pipeline uses YAML/JSON structured
output.

**Application to SecuraCV Canary:**

Implement a **boot-time hardware validation pipeline** that runs before the witness
kernel starts creating records. This catches configuration errors, hardware failures,
and potential tampering early.

```
Boot Sequence (apply this pattern):

1. CRYPTO SELF-TEST
   - Sign + verify a known message
   - Hash a known input, compare digest
   - If FAIL → halt, blink error LED, do NOT create records

2. IDENTITY CHECK
   - Read Ed25519 keypair from NVS
   - If missing → first-boot provisioning from hardware RNG
   - Derive device fingerprint from pubkey
   - Log boot attestation record

3. STORAGE PROBE
   - Detect SD card presence
   - Verify filesystem integrity
   - Read last chain state (sequence number, prev_hash)
   - If chain state missing/corrupt → start new chain epoch, log discontinuity

4. SENSOR ENUMERATION
   - Probe camera module via I2C/CSI
   - Probe GNSS module via UART (send test command, wait for response)
   - Probe any additional sensors (tamper GPIO, temperature)
   - Record detected hardware manifest

5. NETWORK INIT
   - Start WiFi AP (for dashboard)
   - If MQTT configured → attempt connection

6. WITNESS KERNEL START
   - Begin main event loop only after all checks pass
   - First record is always a BOOT_ATTESTATION with hardware manifest
```

**Key insight from ipctool:** The structured output format matters. Every boot should
produce a machine-readable hardware manifest that gets included in the boot attestation
record. This lets an auditor verify "what hardware was this device running when it
produced these witness records?"

---

### 2.3 Zero-Copy / Minimal-Allocation Design for Constrained Environments

**Source pattern:** `smolrtsp` is explicitly designed for embedded devices. Its parser
never allocates memory or copies data — it operates on slices of the input buffer. The
library is "network-agnostic" — it doesn't care whether data comes from POSIX sockets,
libevent, or bare metal.

**Application to SecuraCV Canary:**

The ESP32-S3 has limited RAM (~512KB usable). The witness record pipeline should
minimize allocations:

```
Witness Record Pipeline (zero-copy where possible):

1. Semantic event detected (e.g., "person_count changed to 2")
2. Build payload IN-PLACE in a pre-allocated buffer
   - Don't serialize to JSON then re-parse
   - Write CBOR directly into fixed buffer
3. Hash the payload buffer (SHA256 operates on the same buffer)
4. Build chain hash from: prev_hash + payload_hash + seq + coarsened_time
5. Sign the chain hash (Ed25519 reads from same memory)
6. Write completed record to SD card
7. Optionally publish via MQTT (reads from same buffer)

Key: Steps 2-7 should operate on ONE pre-allocated record buffer
that gets reused for every record. No malloc/free per event.
```

**Transport agnosticism is also valuable:** The record serialization should be
independent of how it gets transmitted. Whether it goes to SD card, MQTT, HTTP API, or
a future BLE channel, the witness record format is the same. The output path is just a
"writer" callback — same pattern smolrtsp uses.

```c
// Transport-agnostic record output
typedef bool (*record_writer_fn)(const uint8_t *data, size_t len, void *ctx);

// Register multiple outputs
securacv_add_output(sd_card_writer, &sd_ctx);
securacv_add_output(mqtt_writer, &mqtt_ctx);
securacv_add_output(serial_writer, NULL);  // Debug
```

---

### 2.4 KISS Architecture — Single Compact Executable

**Source pattern:** Divinus explicitly adopts a "keep it simple, stupid" philosophy.
Despite supporting 10+ SoC families, it compiles to a single compact executable that
uses runtime dynamic linking (`dlopen`/`dlsym`) to load vendor-specific libraries only
when needed. It can even run from a temporary filesystem on read-only systems.

**Application to SecuraCV Canary:**

On ESP32, we don't have dynamic linking, but the principle translates:

- **Single firmware binary** — all functionality compiled in, feature-flagged at compile time
- **No runtime plugin loading** — keep the attack surface minimal
- **Feature flags, not conditional firmware builds:**

```c
// platformio.ini build_flags approach
// One firmware image, features toggled at compile time
#define FEATURE_CAMERA_PEEK    1   // Live preview for setup
#define FEATURE_WIFI_AP        1   // Configuration dashboard
#define FEATURE_MQTT_EXPORT    1   // Home Assistant integration
#define FEATURE_GPS_TELEMETRY  1   // Location attestation
#define FEATURE_SD_LOGGING     1   // Persistent record storage
#define FEATURE_SERIAL_DEBUG   0   // Disable in production
#define FEATURE_OTA_UPDATE     1   // Over-the-air firmware updates

// In code:
#if FEATURE_CAMERA_PEEK
  // Camera peek routes registered
#endif
```

- **Minimal dependencies** — don't pull in frameworks that bring 90% unused code
- **Static allocation preferred** — know your memory budget at compile time

**Privacy corollary:** In OpenIPC's world, keeping it simple means fewer bugs. In
SecuraCV's world, keeping it simple means fewer capabilities that could be misused.
Every feature flag that's OFF is an attack surface that doesn't exist.

---

### 2.5 Structured Hardware Manifest / Self-Identification

**Source pattern:** `ipctool` generates comprehensive YAML reports identifying every
component: SoC model, sensor type, flash chip, firmware version, MAC address, memory
layout. This makes it trivial to know exactly what hardware you're dealing with.

**Application to SecuraCV Canary:**

Every SecuraCV device should be able to produce a **device manifest** — a signed
attestation of its own hardware and firmware configuration. This is critical for:
- **Audit trails** — "which firmware version produced these records?"
- **Fleet management** — if you deploy multiple Canaries
- **Tamper detection** — manifest changes between boots indicate modification

```c
typedef struct {
    // Identity
    char device_id[65];        // Hex-encoded Ed25519 pubkey fingerprint
    uint8_t pubkey[32];        // Full Ed25519 public key

    // Hardware
    char soc_model[32];        // "ESP32-S3"
    char camera_module[32];    // "OV2640" or "none"
    char gnss_module[32];      // "L76K" or "none"
    uint32_t flash_size;       // Bytes
    uint32_t psram_size;       // Bytes
    bool sd_present;
    bool tamper_gpio_present;

    // Firmware
    char firmware_version[16]; // Semver: "1.2.0"
    char firmware_hash[65];    // SHA256 of firmware binary
    uint32_t build_timestamp;  // Compile time
    char build_flags[128];     // Active feature flags

    // Chain state
    uint32_t chain_epoch;      // Increments on chain break
    uint32_t boot_count;       // Total boots from NVS
    uint32_t last_sequence;    // Last record seq before this boot
} device_manifest_t;
```

This manifest gets included in every `BOOT_ATTESTATION` record and is available via
the dashboard API at `/api/manifest`.

---

### 2.6 Sensor Detection via Bus Probing (I2C/SPI/UART)

**Source pattern:** `ipctool` methodically probes I2C and SPI buses to identify
connected image sensors. It reads known register addresses and compares against a
database of sensor IDs. This approach handles the reality that datasheets lie and
vendors relabel chips.

**Application to SecuraCV Canary:**

Don't assume hardware is present — **probe and verify:**

```
Camera Detection:
1. Initialize I2C/CSI bus
2. Attempt to read OV2640 chip ID register (0x0A/0x0B → expect 0x26/0x42)
3. If no response → camera not present, disable peek feature
4. If unexpected ID → log warning, attempt generic init

GNSS Detection:
1. Open UART at 9600 baud
2. Send PMTK test command ($PMTK000*32)
3. Wait up to 2 seconds for $PMTK001 response
4. If no response → try 115200 baud (some modules ship at different rates)
5. If still no response → GNSS not present, records will have null location
6. Parse firmware version from $PMTK705 response

SD Card Detection:
1. Attempt SPI initialization on expected pins
2. Read card info (type, capacity)
3. Mount filesystem, check for existing chain state file
4. If mount fails → attempt format (with user confirmation via dashboard)
5. Write test file, read back, verify → SD healthy
```

**Key lesson:** Graceful degradation. A Canary without GPS still produces valid witness
records (just without location). A Canary without SD still streams to MQTT. A Canary
without camera still detects events (if using an external sensor like Grove Vision AI).
The witness kernel's core loop should work regardless of which peripherals are present.

---

## 3. Patterns to AVOID from OpenIPC

These are things OpenIPC does that would be **wrong for SecuraCV:**

### 3.1 Full Video Pipeline

OpenIPC's entire purpose is capturing, encoding, and streaming video. SecuraCV MUST
NEVER implement a general-purpose video pipeline. The camera is used ONLY for:
- **Peek/preview** — ephemeral live view for setup, no storage
- **Semantic extraction** — frame → object detection → event (frame immediately discarded)

This is mandated by `spec/invariants.md` Invariant I — No Raw Export by Design.

### 3.2 Linux / Full OS

OpenIPC runs Buildroot Linux. SecuraCV Canary runs bare-metal or RTOS (FreeRTOS via
ESP-IDF). A full OS is an attack surface. The constraint IS the security feature.

### 3.3 Network-Accessible Video Streams

OpenIPC exposes RTSP/HTTP video endpoints. SecuraCV MUST NEVER expose raw video over
any network interface. The peek feature uses short-lived MJPEG only on the local WiFi
AP, and it is architecturally impossible for frames to leave the device in any other
way.

### 3.4 Remote Shell / Debug Access

OpenIPC provides telnet/SSH for camera management. SecuraCV provides ONLY:
- Local WiFi AP dashboard (HTTPS preferred, HTTP acceptable on local AP)
- MQTT for event export (semantic records only, never frames)
- Serial console (debug builds only, disabled in production)

### 3.5 Firmware from Cloud

OpenIPC firmware downloads from GitHub. SecuraCV OTA updates MUST be
**cryptographically signed** with the ERRERlabs release key and verified on-device
before installation. No unsigned code execution.

---

## 4. Implementation Checklist

When working on SecuraCV Canary firmware, use this as a review checklist:

- [ ] **Does this feature follow the HAL pattern?** Can the hardware be swapped without changing core logic?
- [ ] **Does this allocate memory per-event?** Prefer pre-allocated buffers.
- [ ] **Is there a boot-time validation for this hardware?** Probe, don't assume.
- [ ] **Does the device manifest include this component?** Every hardware dependency should be in the manifest.
- [ ] **Does this feature degrade gracefully?** What happens if the hardware isn't present?
- [ ] **Is this feature-flagged?** Can it be compiled out?
- [ ] **Does this create a new capability that could be misused?** Apply the ERRERlabs test: "Would we want this capability to exist in the hands of an adversary?"
- [ ] **Is the transport agnostic?** Can records go to SD, MQTT, serial, or future transports without format changes?

---

## 5. References

- OpenIPC GitHub: https://github.com/openipc (MIT License)
- smolrtsp: Lightweight RTSP library — studied for zero-copy parsing patterns
- divinus: Multi-platform streamer — studied for HAL architecture and KISS principles
- ipctool: Hardware detection — studied for detection pipeline and manifest patterns
- ESP-IDF HAL documentation: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/hardware-abstraction.html

**Note:** No OpenIPC code has been copied into SecuraCV. These are architectural
learnings applied to a fundamentally different system with different goals and
constraints.
