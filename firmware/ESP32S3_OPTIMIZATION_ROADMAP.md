# ESP32-S3 Firmware Optimization Roadmap

**Status:** Advisory (non-normative). Findings and prioritized work items for making the
XIAO ESP32-S3 Canary firmware the most capable, most optimized witness firmware it can be —
the "Marlin standard" applied to a privacy sensor node.
**Scope:** the ACTIVE PlatformIO build at [`canary/`](canary/) (dev/release/full/…), the XIAO
ESP32-S3 **Sense** board ([`boards/xiao-esp32s3-sense/`](boards/xiao-esp32s3-sense/)), and the
shared modules under [`common/`](common/). The Arduino `canary-wap` lane and the C3/C6/display
boards are referenced only where they already prove a technique the S3 build hasn't adopted.
**Method:** a full read of every S3 subsystem (camera, vision, audio, IR, WiFi/CSI, BLE, mesh,
MQTT, storage, power/thermal, crypto/witness/OTA, web/REST) against the ESP32-S3 datasheet and
ESP-IDF driver set. Every claim below carries a `file:line` anchor so it can be checked.
**Precedence:** this document proposes changes; it does **not** override
[`ARCHITECTURE.md`](ARCHITECTURE.md), [`HARDWARE.md`](HARDWARE.md), or the privacy invariants.
Anything here that would change a config default must still go through
[`CONFIG_CHANGES.md`](CONFIG_CHANGES.md); anything that weakens a security default must still go
through [`canary/include/secure_defaults.h`](canary/include/secure_defaults.h)'s process.

> **On the privacy posture.** SecuraCV's whole thesis is *witnessing without watching*. Several
> "capabilities" the ESP32-S3 is technically great at (wake-word ASR, face/person ML, always-on
> raw audio) are deliberately *not* pursued, or are pursued only as on-device semantic gates that
> emit claims, never media. Where a hardware lever collides with that posture, it's flagged
> **[privacy-gated]** rather than recommended outright. Optimizing the firmware means making the
> *witness* better, not turning the Canary into a surveillance camera.

---

## How to read this document

Work items are tagged **P0 / P1 / P2**:

- **P0 — correctness.** Something is broken, silently disabled, or unsafe *today*. These are
  bugs, not enhancements; several features the board advertises don't actually run. Fix first.
- **P1 — high-leverage optimization.** Large, well-understood wins in capability, battery,
  latency, or security. The core of "make it the best."
- **P2 — expansion & polish.** New capabilities, peripheral headroom, and refinements that raise
  the ceiling once P0/P1 land.

Each item names the lever, the anchor, and the expected win. The consolidated priority table is
at the end.

---

## 1. The five cross-cutting levers

These touch every subsystem, so they come first. Fixing them multiplies the value of every
per-module change that follows.

### 1.1 Unify on one modern toolchain (Arduino-ESP32 core 3.x / IDF 5.x)

Today the shipping profiles are split across **two** toolchains:

- `dev` / `release` / `minimal` / `standalone` build on `espressif32 @ ^7.0.0`, whose
  `framework=arduino` package is still **Arduino 2.0.17 / IDF 4.4.7**
  ([`canary/platformio.ini`](canary/platformio.ini), `[env]` `platform =`).
- `[env:full]` pins the **pioarduino** fork `55.03.38-1` = **Arduino 3.3.8 / IDF 5.5.4**
  ([`canary/platformio.ini`](canary/platformio.ini) `[env:full]` block) because NimBLE 2.x
  requires core 3.x.

Two IDFs across shipping images means every driver-level behavior has to be reasoned about
twice, and it *forces* the firmware to keep using **legacy drivers that IDF 5.x has already
replaced** (see §3.2, §3.3): `driver/i2s.h` (audio), `driver/rmt.h` (IR), and the older
ADC path. Consolidating dev/release onto the same core-3.x/IDF-5.x platform `[env:full]` already
uses is the single highest-leverage structural change in this document, because it *unblocks*:

- the modern **`i2s_pdm` / `i2s_channel`** mic driver and **`rmt_rx`** IR driver (§3.2–3.3),
- **`esp_pm` auto light-sleep + tickless idle** for battery life (§1.4, §3.6),
- **WPA3 / PMF**, `esp_wifi_set_protocol/bandwidth/country` (§3.4),
- the **`adc_cali` / `adc_oneshot`** calibration API already partially used (§3.6),
- new **`esp_lcd`/USB-MSC/TinyUSB** composite options (§3.8, §4).

**P1.** Do it behind CI: build every flavor on the new platform, diff binary size and the
`flavors.json` matrix, keep `partitions` correct (§1.3). This is a multi-PR migration, not a
one-liner — but nearly everything else here is cheaper once it's done.

### 1.2 Put the second core and a real task model to work

The entire sensing + witness pipeline runs **inline, sequentially, on the single Arduino
`loopTask`** (core 1): `csi::process()` → `audio_process()` → `vision_process()` →
`touch_process()` → `ir_process()` → GPS → witness → web housekeeping, every loop
([`canary/src/main.cpp:1480`](canary/src/main.cpp)–1529). The **only** `xTaskCreatePinnedToCore`
in the whole canary tree is the MJPEG peek-stream worker
([`securacv_network.cpp:1753`](canary/lib/securacv_network/src/securacv_network.cpp)); core 0 runs
the WiFi/BT stack and otherwise idles. That architecture creates the recurring failure mode seen
across three separate audits: **one slow call starves everything else and threatens the 8 s task
watchdog.** Concretely, all on `loopTask`:

- TFLite `Invoke()` blocks **~500 ms** ([`securacv_vision.cpp:433`](canary/lib/securacv_vision/src/securacv_vision.cpp)) — long enough to overflow the 160 ms audio DMA ring mid-cadence.
- `MDNS.queryService()` blocks **~1 s every 30 s** ([`securacv_network.cpp:495`](canary/lib/securacv_network/src/securacv_network.cpp)).
- `WiFi.scanNetworks(false,…)` blocks **~4 s** on the httpd task ([`securacv_network.cpp:2124`](canary/lib/securacv_network/src/securacv_network.cpp)).
- `PubSubClient.connect()` blocks up to **~15 s** with no `setSocketTimeout` ([`securacv_mqtt.cpp`](canary/lib/securacv_mqtt/src/securacv_mqtt.cpp)).
- Every witness record does a synchronous Ed25519 sign+verify (§3.7) **and** a synchronous SD `open/write/close` (§3.5) on the loop.

**P1.** Introduce a small, explicit task model: a **core-0-pinned "sensing" task** for DSP-heavy
work (vision decode/Invoke, audio DSP, CSI feature extraction) fed by FreeRTOS queues, and a
**"durability" task** that owns SD + NVS writes (§3.5, §3.7). Keep `loopTask` for orchestration
and the watchdog pump. The pattern already exists in-tree — the CSI→mesh hop uses an MPSC queue
([`csi_modules_integration.cpp:185`](canary/src/csi_modules_integration.cpp)) — so this is
generalizing a known-good approach, not inventing one. Win: bounded loop latency, no watchdog
thrash, sensing that keeps running while the network blocks.

### 1.3 Reclaim the flash and give the chain a durable home

The canonical dev/release table [`canary/partitions_ota.csv`](canary/partitions_ota.csv) uses
only **4 MB of the 8 MB** chip: 2 × 1.9 MB A/B app slots + a 192 KB `spiffs` partition — and
**nothing mounts that `spiffs` partition** (no LittleFS/FFat `begin()` anywhere; confirmed in the
storage audit). So half the flash is dark, and the one data partition that exists is unused.
Meanwhile `[env:full]` has to switch to arduino's `default_8MB` table (2 × 3.2 MB, no custom
data partition) because FULL doesn't fit a 1.9 MB slot — which is why
[`PARTITIONS.md`](PARTITIONS.md) has to maintain a whole "which table for which deployment"
matrix.

**P1.** Design **one canonical 8 MB table** for all non-secure profiles:

- 2 × ~3.0–3.2 MB A/B app slots (fits FULL *and* the smaller profiles, ends the matrix),
- a dedicated **`witness_log` LittleFS/FAT data partition** that survives OTA and SD removal —
  a flash-durable mirror of the chain head + recent records, so a device with no SD card (or a
  card that just failed, §3.5) still keeps a tamper-evident local record,
- a small **`nvs_keys`** partition so NVS encryption (§3.7) can be turned on without re-carving
  flash later.

This directly serves the product promise ("verifiable after the fact") on the majority of
deployments that today depend entirely on a removable FAT card for durability. Note the migration
constraint from PARTITIONS.md: changing an installed device's table needs a USB reflash, so this
ships as a deliberate version boundary.

### 1.4 Turn on real power management (`esp_pm`) — the battery lever

The power-policy engine *already contains* the DFS + auto-light-sleep call
(`esp_pm_configure` with `light_sleep_enable`,
[`securacv_power_policy.cpp:157`](canary/lib/securacv_power_policy/src/securacv_power_policy.cpp))
— but it's wrapped in `#if CONFIG_PM_ENABLE`, and **`CONFIG_PM_ENABLE` is never set**, so the
compiled path is the `#else` branch: a plain `setCpuFrequencyMhz()`. The firmware advertises
dynamic frequency scaling and tickless idle and actually runs **locked-frequency, no light
sleep.** (This ties to §5: `sdkconfig.defaults` appears not to be wired into the Arduino build at
all — see the verification note there — so even the CONFIG lines that *look* set may be inert.)

**P1.** Enable `CONFIG_PM_ENABLE=y` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` on the core-3.x
platform, set a sensible `min_freq_mhz` (40–80) per power mode, and let the existing policy code
drive it. With WiFi `MIN_MODEM` and DTIM-aligned light sleep, **idle draw drops from ~40–60 mA to
low-single-digit mA while keeping the WiFi/MQTT association alive** — no teardown, no cold
reconnect. This is a far better default than the current deep-sleep-and-cold-reconnect duty cycle
(§3.6), which can net *negative* savings once the reconnect handshake is counted.

### 1.5 Protect the key at rest and in hardware (the "crypto signing everything" lever)

The product's spine is Ed25519-signed, hash-chained records. But the **device private key sits in
plaintext NVS** ([`securacv_crypto.cpp:306`](canary/lib/securacv_crypto/src/securacv_crypto.cpp))
with **no flash encryption, no NVS encryption, and no secure boot** in the default build
([`canary/sdkconfig.defaults`](canary/sdkconfig.defaults)). Physical read of the flash → key
extraction → the attacker can forge the entire chain. "Keys never leave the device"
([`secure_defaults.h`](canary/include/secure_defaults.h) Principle 1) is enforced only against the
*software* export path, not against at-rest confidentiality. The ESP32-S3 has the exact hardware
to fix this and **none of it is used** (grep: zero `esp_ds_*` / `esp_hmac_*` / `esp_efuse_*` in
`canary/`). See §3.7 for the staged plan (NVS encryption now → flash encryption + secure boot v2
→ HMAC/DS-peripheral key wrapping). This is a P0/P1 split: NVS encryption is a quick P1 win; the
plaintext key is a P0-severity exposure that the roadmap must not leave implicit.

---

## 2. Fix these now — the P0 correctness list

These are not enhancements. Each is a feature the firmware *claims* but does not deliver, or an
unsafe behavior, verified during the audit.

1. **Vision Layers 2 & 3 never run at the default resolution.** `begin()` selects **XGA
   (1024×768)** ([`securacv_camera.cpp:148`](canary/lib/securacv_camera/src/securacv_camera.cpp)),
   but `decode_and_downsample` hard-fails when `width×height > 640×480`
   ([`securacv_vision.cpp:139`](canary/lib/securacv_vision/src/securacv_vision.cpp)). So block-motion,
   scene-tamper, object-removal, **and the TFLite person detector all silently no-op** — only the
   Layer-1 JPEG-size heuristic ever executes. Fix: give the vision path its own small
   GRAYSCALE/RGB565 capture, or drop the running framesize to ≤VGA when `VISION_DETECT` is on.
   *This is the single biggest "advertised but dead" defect.*

2. **A "never sleeps" build still deep-sleeps.** The deep-sleep block is gated by
   `#if FEATURE_POWER_POLICY` ([`main.cpp:1574`](canary/src/main.cpp)), **not** by
   `FEATURE_DEEP_SLEEP` — despite the comment at `main.cpp:197` and the `lowpower.h` docs claiming
   the latter gates it. With the default `FEATURE_POWER_POLICY=1` + `FEATURE_DEEP_SLEEP=0`, a
   `CRITICAL_BATTERY` event ([`main.cpp:571`](canary/src/main.cpp)) or `PMODE_SHUTDOWN` **will
   deep-sleep the device.** Fix: add the real `#if FEATURE_DEEP_SLEEP` guard around the sleep entry.

3. **BLE Scout ships as a no-op in the PlatformIO build.** `ble_scout_allow_radio()` is only ever
   called from the `canary-wap` `.ino` — **never from `canary/src`** — so `s_radio_allowed` stays
   false and the passive scanner never starts
   ([`ble_scout.cpp:231`](canary/lib/securacv_ble_scan/src/ble_scout.cpp)). In `[env:full]`,
   `FEATURE_BLE_SCAN=1` builds the registry/tracker/roster but **nothing scans**, so room
   attribution and the fleet roster are inert. Fix: call `ble_scout_allow_radio()` +
   re-init after the provisioning join window, mirroring the WAP.

4. **The camera burns battery it doesn't need to.** `camera_init()` runs unconditionally at boot
   ([`main.cpp:871`](canary/src/main.cpp)); every battery power mode sets
   `policy_features.camera_peek=false`, but that flag is **only read for a status print**
   ([`main.cpp:2659`](canary/src/main.cpp)) — `esp_camera_deinit()` / `CameraManager::end()` is
   never called. The OV3660/OV2640 stays clocked at 20 MHz XCLK, **~40–60 mA continuously on
   battery.** Fix: act on `camera_peek=false` → deinit (and re-init on demand); `end()`/`reinit()`
   already exist ([`securacv_camera.cpp:207`](canary/lib/securacv_camera/src/securacv_camera.cpp)).

5. **SD card glitch permanently disables logging.** `sd_storage_remount()` is *declared* but
   **unimplemented** ([`common/storage/storage.h:190`](common/storage/storage.h)); a single
   transient card failure disables witness persistence until reboot, with no re-init path. Fix:
   implement remount + a periodic mount-health poll.

6. **CSI collapses on battery and on lone devices.** `battery_normal` keeps `csi=true` while
   forcing `WIFI_PS_MIN_MODEM` ([`securacv_power_policy.cpp:73`](canary/lib/securacv_power_policy/src/securacv_power_policy.cpp)),
   and the boot path enables modem sleep on battery ([`main.cpp:1311`](canary/src/main.cpp)).
   Modem sleep drops the asynchronous RX frames CSI depends on → frame yield collapses → the 5 s
   CSI watchdog just toggles the radio every 10 s without fixing the cause. Worse, the in-tree
   **active CSI probe (`csi_probe.cpp`) is never wired into the canary build**, so a device with
   no associated STA and `AP_MAX_CONNECTIONS=1` sees almost no frames. Fix: gate power-save on
   `csi_hal::is_running()` (force `WIFI_PS_NONE` when CSI is live) **and** start the existing probe.

7. **Camera init/deinit races the peek-stream task.** Vision guards with `if (isPeekActive())
   return`, but the stream task's freeze-recovery sets `peek_active=false` *before*
   `deinit()`+`begin()` ([`securacv_camera.cpp:643`](canary/lib/securacv_camera/src/securacv_camera.cpp)),
   so a main-loop `esp_camera_fb_get()` can hit a half-initialized driver → crash/UB. Fix: a single
   owning task or a mutex around all `esp_camera_*` lifecycle vs capture (subsumed by §1.2).

8. **Stale/false in-code claims to correct while touching these.** The auth header says "wiring
   happens in Phase 2" but auth **is** wired (`auth_gate` on ~91 of 98 handlers,
   [`securacv_network.cpp:757`](canary/lib/securacv_network/src/securacv_network.cpp)); the
   `bluetooth_mgr.h` pairing/bonding manager is **orphaned dead code** (`ble_debug_beacon` was
   removed in the repo audit cleanup);
   the "single main-loop task" thread-safety comments on the Scout tracker/roster are **wrong**
   (the advert callback runs in the NimBLE host task — a real data race the moment item 3 is
   fixed). Clean these up so the next reader isn't misled.

---

## 3. Per-subsystem findings

### 3.1 Camera (`securacv_camera`) — sensor you're not fully using

Current: `CameraManager` singleton, JPEG, tiered init (XGA/q10/fb2/PSRAM → VGA → QVGA/DRAM),
PID auto-detect (OV2640/OV3660/OV5640), 6 tuning presets, MJPEG peek stream on its own task,
thermal governor (70/80 °C) and a 10 s freeze watchdog.

Untapped / issues beyond the P0s above:
- **Fixed 20 MHz XCLK, never tuned or gated** ([`securacv_camera.cpp:111`](canary/lib/securacv_camera/src/securacv_camera.cpp)).
  OV3660/OV5640 tolerate other clocks; lowering cuts EMI + self-heat (the code itself notes
  streaming adds 10–20 °C, [`main.cpp:1533`](canary/src/main.cpp)), raising lifts frame rate.
- **No sensor SCCB standby.** With PWDN not wired on the Sense, software standby (SCCB sleep
  register) is the *only* way to idle the sensor short of full deinit — unused. **[P1]**
- **No OV5640 tuning branch** and an OV2640-centric AEC clamp
  ([`securacv_camera.cpp:392`](canary/lib/securacv_camera/src/securacv_camera.cpp)); the PID map
  mixes real and guessed IDs. Wrong exposure/color on two of the three sensors the board ships. **[P1]**
- **Software JPEG decode every vision frame** instead of asking the sensor for a small
  RGB565/grayscale via DCW/binning — free downscale left on the table. **[P1]**
- ~~`firmware/common/camera/camera_mgr.h` is a **second, unused camera implementation**~~ —
  deleted in the repo audit cleanup; `securacv_camera` is the only camera stack. **[done]**

### 3.2 Audio (`securacv_audio`) — legacy driver, scalar DSP

Current: PDM 16 kHz mono, DC-removed RMS + one 3.4 kHz band-pass biquad tone gate, two-stage
NFPA-72 (T3 smoke) / UL-2034 (T4 CO) cadence matchers, optional knock/doorbell/glass transients,
hard mute with witness audit, raw samples wiped every frame. Solid privacy hygiene.

- **Legacy I2S driver** `driver/i2s.h` / `i2s_read`
  ([`securacv_audio.cpp:56`](canary/lib/securacv_audio/src/securacv_audio.cpp)) — removed/relocated
  in IDF 5.x. Migrate to `driver/i2s_pdm.h` + `i2s_channel_read` (gains built-in HPF/gain, channel
  event callbacks). **[P1, unblocked by §1.1]**
- **Scalar DSP** — the biquad + sum-of-squares are per-sample float MACs
  ([`securacv_audio.cpp:339`](canary/lib/securacv_audio/src/securacv_audio.cpp)); **esp-dsp SIMD is
  unused anywhere** in the tree. `dsps_biquad_f32_ae32` / `dsps_dotprod_f32` cut this several-fold,
  and `dsps_fft2r` would enable a real spectral gate (stronger alarm-vs-noise discrimination than a
  single biquad). **[P1]**
- **Mic runs 100% duty** — no coarse RMS wake-gate to duty-cycle capture (vision has a duty cycle;
  audio doesn't). A cheap energy pre-gate saves CPU/battery when the room is quiet. **[P2]**
- **[privacy-gated]** esp-sr AFE (noise suppression/AGC) would harden cadence detection; WakeNet/
  MultiNet are out of scope by posture — flag, don't adopt silently.

### 3.3 IR (`securacv_ir`) — legacy driver

NEC/Sony/RC5 decode on RMT RX with per-session salted 4-bit privacy buckets — nicely scoped. Uses
**legacy `driver/rmt.h`** ([`securacv_ir.cpp:22`](canary/lib/securacv_ir/src/securacv_ir.cpp));
migrate to `driver/rmt_rx.h` on core 3.x. **[P1, unblocked by §1.1]**

### 3.4 WiFi / mesh / MQTT (`securacv_network`, `securacv_mesh`, `securacv_mqtt`)

Current: boots `WIFI_AP_STA`, tears AP down to STA after an 8 s grace (single-radio coex dance),
exp-backoff reconnect, mDNS `_securacv._tcp`, ESP-NOW mesh with **app-layer Ed25519 +
ChaCha20-Poly1305** (transport itself unencrypted, `encrypt=false`), MQTT via PubSubClient.

Untapped / issues:
- **PHY is never pinned.** Nothing calls `esp_wifi_set_protocol` / `set_bandwidth` /
  `set_country` anywhere in the canary build — pure driver defaults. For CSI this is a real
  problem: an HT40 association or rate renegotiation changes the subcarrier count and destabilizes
  the fixed 32-dim feature vector. Pin `11bgn` + `HT20` + country at init. **[P1]**
- **Slow reconnects that disrupt sensing.** `WiFi.begin(ssid,pass)` with no cached BSSID/channel/
  static-IP does a full scan + DHCP every time
  ([`securacv_network.cpp:405`](canary/lib/securacv_network/src/securacv_network.cpp)); the scan
  sweeps channels and disrupts CSI/ESP-NOW mid-capture. Cache BSSID/channel/IP in NVS →
  `WiFi.config()` + `WiFi.begin(…,channel,bssid)` for **sub-300 ms** reconnect and no sweep. **[P1]**
- **`network_set_tx_power()` exists but is never called at boot** — Seeed's weak-antenna
  recommendation goes unused; set it at init. **[P1]**
- **MQTT is plaintext, QoS 0, blocking, with no offline queue** — witness/tamper events emitted
  during a broker/WiFi blip are **dropped, not replayed**
  ([`securacv_mqtt.cpp`](canary/lib/securacv_mqtt/src/securacv_mqtt.cpp)). Add `setSocketTimeout`,
  a bounded offline queue for security-critical events, and optional `WiFiClientSecure`+CA. **[P1]**
- **ESP-NOW uses the default rate and unauthenticated broadcast pairing.**
  `esp_wifi_config_espnow_rate()` (pin a robust low/LR rate) improves mesh range/reliability;
  app-layer AEAD already covers confidentiality. **[P2]**
- **WPA3-Personal + PMF** on STA and SoftAP. (The AP password is already per-device — the shared
  legacy fallback macro was deleted from [`canary_config.h`](canary/include/canary_config.h) and
  `begin()` now requires an explicit credential.)
  The IDF OTA sub-project already sets `pmf_cfg`; the main firmware doesn't. **[P1, unblocked by §1.1]**
- **De-block the loop** — async `WiFi.scanNetworks(true,…)`, throttle/offload `MDNS.queryService`,
  move MQTT to its own task (subsumed by §1.2). **[P1]**
- **[future] FTM ranging** (`esp_wifi_ftm_*`, S3 initiator/responder) → inter-Canary distance to
  feed `core_multilink_fusion`. **[P2]**

### 3.5 Storage / SD (`securacv_storage`, `securacv_data_mgmt`, witness store)

Current: Arduino `SD.h` over FSPI SPI2, FAT, append-only `/WITNESS/records.jsonl`, hourly
`/CHAIN/backup.bin`, NVS chain-head cache every 10 records, SD-wins boot reconciliation (verified
with signature — genuinely good design).

- **SD SPI runs at 4 MHz** ([`canary_config.h:336`](canary/include/canary_config.h); the `pins.h`
  `SD_SPI_FREQ_*` are dead duplicates). On the short XIAO-Sense traces **20–25 MHz is reliable →
  ~5× throughput** and proportionally shorter loop stalls. Lowest-effort high-impact change in the
  whole document (one constant, keep the 1 MHz init fallback). **[P1]**
- **Per-record `open`/`write`/`close` on `loopTask`, no `flush` anywhere** — FAT directory re-read
  every record, tens-of-ms stalls ([`securacv_witness.cpp:266`](canary/lib/securacv_witness/src/securacv_witness.cpp)).
  Move to the durability task (§1.2) with a persistent handle + periodic flush. **[P1]**
- **`/CHAIN/backup.bin` truncates in place** ([`securacv_data_mgmt.cpp:322`](canary/lib/securacv_data_mgmt/src/securacv_data_mgmt.cpp)) —
  not power-loss-safe. Use temp-write + flush + atomic rename. **[P1]**
- **No SPI mutex** (single-task-by-convention, unenforced); **`/HEALTH` rotation churns an empty
  directory every 5 min** (no writer ever creates a health file); **`SDStatus` error counters are
  never incremented**. Wire up or delete. **[P2]**
- **CID/CSD/`cardType` never read** — wear is a pure write-volume estimate vs a hard-coded 32 TBW;
  real card identity would also catch counterfeit/worn cards. **[P2]**
- **The idle 192 KB `spiffs` partition** should become a LittleFS chain-head journal (§1.3),
  giving card-independent, wear-leveled power-loss safety for `seq`/`chain_head`. **[P1]**

### 3.6 Power / battery / thermal (`securacv_power*`, `securacv_lowpower`, `securacv_thermal*`)

Current: ADC1 on GPIO1 with **calibration present** (curve-fitting on IDF5 / `esp_adc_cal` on
IDF4), 16-deep median filter, divider autodetect, voltage-curve SoC, trend-based charge inference
(no readable CHRG pin on XIAO), 6-mode battery policy, passive thermal watchdog (die sensor),
camera as the sole thermal actuator.

Beyond §1.4 (`esp_pm`) and the P0s (deep-sleep gate, camera deinit):
- **Deep sleep tears down WiFi/MQTT ungracefully** — no `esp_wifi_stop`/MQTT DISCONNECT before
  `esp_deep_sleep_start` ([`securacv_lowpower.cpp:213`](canary/lib/securacv_lowpower/src/securacv_lowpower.cpp));
  the 55 s-sleep + full cold reconnect cycle likely nets *negative* savings. Prefer light-sleep
  duty cycling (§1.4); if deep sleep is used, tear down cleanly first. **[P1]**
- **No `gpio_hold`/`rtc_gpio_hold` and no PSRAM power-down before deep sleep** → floating XCLK/
  SD-CS/LED lines leak and can corrupt SD; deep-sleep floor is far above the µA ideal. **[P1]**
- **ULP RISC-V coprocessor entirely unused** — `lowpower_arm_wake_ulp()` exists with **zero
  callers**. A ULP program sampling VBAT + watching the tamper GPIO during deep sleep would give
  true always-on witnessing at µA and wake only on a threshold/tamper crossing. **[P2, headline
  capability]**
- **Wake-on-ext0/ext1 unused** — the tamper switch and a charger-status line aren't wired to wake.
  **[P2]**
- **No RTC-memory state preservation** (`RTC_DATA_ATTR` unused) — every wake is a cold boot that
  re-detects the divider and reloads NVS. Preserving SoC/trend/chain-head enables fast-reconnect
  duty cycling. **[P2]**
- **Voltage-only SoC has no IR-drop/load compensation** — camera/WiFi load steps make SoC jump and
  risk mode flapping near thresholds (partly masked by hysteresis). **[P2]**
- **Doc/HW fix:** `securacv_power.h:6` says "TP4056"; the board charger is **BQ25101** (100 mA) —
  different full-detect/charge-time math (3000 mAh at 100 mA ≈ 30 h). **[P2]**

### 3.7 Crypto / witness / OTA / identity (`securacv_crypto`, `securacv_witness`, `common/ota`)

Current: rweather Ed25519 (software; S3 has no SHA-512 accel so no HW path exists regardless),
mbedtls SHA-256 (**HW-accelerated** — good), domain-separated hash chain, verify-after-sign, A/B
OTA with PENDING_VERIFY self-test + rollback, HTTPS+cert-bundle manifest pull, software NVS
anti-rollback floor.

Beyond §1.5:
- **Weak first-boot entropy.** `esp_fill_random` is called during provisioning early in `setup()`
  before RF is up, with no `bootloader_random_enable()` and no entropy self-check
  ([`securacv_crypto.cpp:136`](canary/lib/securacv_crypto/src/securacv_crypto.cpp)) — risk of
  predictable keys on fresh units. Seed hardware entropy before keygen, gate provisioning on a
  check. **[P1]**
- **Chain-head persistence is non-atomic** — `seq` then `chain_head` as two separate NVS writes
  ([`securacv_witness.cpp:227`](canary/lib/securacv_witness/src/securacv_witness.cpp)); a power cut
  between them leaves them inconsistent (recoverable via SD-wins **only if a card is present**).
  Persist as one blob in one commit (or double-buffer with a generation counter). **[P1]**
- **No device-side rollback detection** — without secure boot / an eFuse or RTC monotonic anchor,
  an attacker who rewrites NVS can rewind `seq`/`chain_head`; the device re-signs the fork with its
  own key and only an external verifier holding an earlier copy notices. Anchor a monotonic counter
  in eFuse/RTC. **[P1]**
- **DS + HMAC peripherals unused** — the biggest key-protection lever: HMAC-downstream mode wraps
  the Ed25519 key so it never sits readable in NVS (or switch the device key to RSA and sign in the
  DS peripheral — the explicit Ed25519-vs-RSA tradeoff, document it). **[P1]**
- **Signing on the loop hot path** — record create (sign + verify-after-sign) runs synchronously
  from `loopTask`; route through the durability task (§1.2). **[P1]**
- **`time_bucket` is per-boot uptime, not wall-clock** ([`securacv_witness.cpp:127`](canary/lib/securacv_witness/src/securacv_witness.cpp)) —
  collides across boots; ordering rests entirely on `seq`. Bind real GPS/NTP time where available. **[P2]**
- **Co-signing is design-only** — [`spec/co_signing.md`](../spec/co_signing.md) fully specifies
  `PWK/Endorsement/v1` but **nothing is implemented in firmware**. Cross-device attestation
  (a Canary endorsing a neighbor's chain head) is a marquee capability left on paper. **[P2]**

### 3.8 Web / REST / onboarding (`securacv_network` httpd, `securacv_auth`, `securacv_usb_onboard`)

Current: **IDF-native `esp_http_server`** (not ESPAsyncWebServer — the right choice), ~46 REST
endpoints, bearer-token auth with constant-time compare + exp-backoff lockout **wired on ~91 of 98
handlers**, rate limiting, token injected into the embedded SPA via a chunked placeholder.
Genuinely solid. Gaps:

- **No TLS anywhere** (no `esp_https_server`/`httpd_ssl`) — all REST + the MJPEG peek stream are
  plaintext on the LAN; a self-signed pinned cert (as the archived WAP snapshot had) closes it. **[P1]**
- **Auth coverage is good — the MJPEG stream *is* gated.** `handle_peek_stream` calls `auth_gate`
  first ([`securacv_network.cpp:1700`](canary/lib/securacv_network/src/securacv_network.cpp)), so the
  peek stream is **not** an open privacy hole. The main intentionally-ungated handler is `handle_ui`
  (it serves the SPA and carries the token). Worth a periodic sweep that no *new* handler is added
  without `auth_gate`, but there is no open endpoint today. **[P2 — hygiene]**
- **AP password is exactly 8 chars** (`"cv-"` + 5), the WPA2 floor — ~28.7 bits of entropy. Widen
  to 10–12 chars from the same fingerprint for headroom. **[P2]**
- **Untapped UX:** Improv-WiFi / WebUSB provisioning, SSE/WebSocket event streams instead of poll,
  an OpenAPI description of the REST surface, USB-MSC evidence-drive (the `usb-onboard` env already
  proves the TinyUSB composite path). **[P2]**

---

## 4. Pins, headroom & expandability

The user asked specifically about free pins and peripherals. On the XIAO ESP32-S3 **Sense**, the
built-ins consume most GPIOs: the camera DVP bus (GPIO10–18, 38–48), the PDM mic (41/42), the
microSD (CS 21 + SPI on the D8/D9/D10 pads = GPIO7/8/9), and VBAT sense (GPIO1). GPIO26–37 carry
the flash/**octal** PSRAM bus and must never be routed
([`boards/xiao-esp32s3-sense/pins/pins.h`](boards/xiao-esp32s3-sense/pins/pins.h)). That leaves a
realistically small but real user surface on the header:

- **The I2C bus on D4/D5 (GPIO5/6)** — the Grove/expansion path. This is where the fleet grows:
  BH1750 lux (already supported on canary-sense), BME280/SHT4x environment, a PIR or SCD4x CO₂, a
  small SSD1306/SH1107 status OLED, an RTC (PCF8563/DS3231) to give witness records a **real
  wall-clock** (fixing §3.7's uptime-only `time_bucket`), or a QMI8658/LIS3DH **IMU for a true
  motion/tamper/knock sensor** that's far more robust than die-temp drift. A clean, documented
  **I2C sensor-adapter contract** (the repo already has `spec/sensor_adapter_contract_v0.md`) turns
  each of these into a *data* contribution, Marlin-style, not a code fork. **[P2, high capability]**
- **D6/D7 (GPIO43/44)** default to the GPS UART; when GPS is unused they're a spare UART for RS485/
  Modbus, a second GNSS, or a serial sensor.
- **The tamper input (GPIO2/D1)** and **EXT_LED (GPIO3/D2)** — the intended enclosure-tamper and
  status-LED pins, currently defined but the LED path is unused (and must never drive GPIO21, which
  is the SD CS — see the LESSONS_LEARNED trap).
- **Capacitive touch** — the S3 touch controller is already used for silent-panic/tamper
  (`FEATURE_TOUCH`); more pads = more silent gestures.

The honest constraint: this board is I/O-poor once the camera+mic+SD are populated. The
**expandability story is the I2C bus + the fleet mesh**, not a pile of spare GPIO. The biggest
"new capability" wins are an **RTC** (trustworthy timestamps) and an **IMU** (real tamper/impact
sensing) on I2C, plus finishing the **co-signing mesh** (§3.7) so capability scales by *adding
Canaries* rather than by adding pins to one.

---

## 5. Verify-before-acting notes

Kept honest and separate — a few high-impact claims depend on build wiring that should be
confirmed against a real CI build log before anyone acts loudly on them:

- **Is `canary/sdkconfig.defaults` actually applied?** Under `framework=arduino` the IDF libs are
  **prebuilt**, and `platformio.ini` doesn't reference the file via a pioarduino `custom_sdkconfig`
  hook. If it's inert, then the `CONFIG_BT_ENABLED=n`, SPIRAM-80M, CSI-enable, and HTTPD-limit
  lines have **no effect** on the canary build, and CSI works only because the prebuilt libs
  already enable it. This changes *how* §1.4 (`esp_pm`) and the secure-config lines get applied
  (they'd need the pioarduino custom-sdkconfig path, which is another reason to do §1.1 first).
  **Check a verbose build for whether the file is consumed.**
- **Confirm the two ungated HTTP handlers** (`handle_ui`, `/api/peek/stream`) before shipping —
  §3.8 item 2.
- **SD 20 MHz** is reliable on the reference wiring; validate on hardware with the specific card
  mix before raising the default, keeping the slow-init fallback ladder.
- Everything tagged **[unblocked by §1.1]** presumes the core-3.x migration; on the legacy 2.0.17
  platform those driver/API moves don't exist.

---

## 6. Consolidated priority table

| # | Item | Tag | Subsystem | Anchor | Expected win |
|---|------|-----|-----------|--------|--------------|
| 1 | Vision runs only Layer 1 at XGA (2/3 dead) | **P0** | Vision | `securacv_vision.cpp:139` | Restores motion/tamper/person detection |
| 2 | "Never sleeps" build still deep-sleeps | **P0** | Power | `main.cpp:1574` | Correctness/safety on marginal cells |
| 3 | BLE Scout never scans in PIO build | **P0** | BLE | `ble_scout.cpp:231` | Room attribution + fleet roster actually work |
| 4 | Camera never deinits on battery | **P0** | Camera/Power | `main.cpp:2659` | ~40–60 mA saved on battery |
| 5 | SD glitch disables logging until reboot | **P0** | Storage | `storage.h:190` | Durable logging survives transient faults |
| 6 | CSI dies under modem-sleep; probe unwired | **P0** | WiFi/CSI | `power_policy.cpp:73` | Reliable CSI on battery + lone devices |
| 7 | Camera init/deinit races peek task | **P0** | Camera | `securacv_camera.cpp:643` | Removes a crash vector |
| 8 | Plaintext private key in NVS | **P0→P1** | Crypto | `securacv_crypto.cpp:306` | NVS-enc now; flash-enc+secure-boot next |
| 9 | Unify on core-3.x / IDF-5.x toolchain | **P1** | Build | `platformio.ini` | Unblocks §3.2–3.4, §1.4, WPA3, new drivers |
| 10 | Dual-core task model (sensing + durability) | **P1** | Core | `main.cpp:1480` | Bounded loop latency, no WDT thrash |
| 11 | One 8 MB partition table + `witness_log` | **P1** | Flash | `partitions_ota.csv` | Ends the table matrix; card-independent durability |
| 12 | Enable `esp_pm` auto light-sleep | **P1** | Power | `power_policy.cpp:157` | Idle ~40→~3 mA, association kept |
| 13 | SD SPI 4 → 20 MHz | **P1** | Storage | `canary_config.h:336` | ~5× write throughput (one constant) |
| 14 | Off-loop SD writes + flush + atomic backup | **P1** | Storage | `securacv_witness.cpp:266` | No loop stalls; power-loss safety |
| 15 | Pin WiFi PHY (protocol/BW/country) + TX power | **P1** | WiFi/CSI | `securacv_network.cpp` | Stable CSI vector, correct regulatory/range |
| 16 | Fast reconnect (cached BSSID/channel/IP) | **P1** | WiFi | `securacv_network.cpp:405` | <300 ms reconnect, no CSI-disrupting sweep |
| 17 | MQTT: socket timeout + offline queue + TLS | **P1** | MQTT | `securacv_mqtt.cpp` | No dropped events; encrypted transport |
| 18 | HW key protection (HMAC/DS peripheral) + entropy seed + atomic chain head | **P1** | Crypto | `securacv_crypto.cpp:136` | Real at-rest + anti-forgery guarantees |
| 19 | Migrate audio→`i2s_pdm`, IR→`rmt_rx` | **P1** | Audio/IR | `securacv_audio.cpp:56` | Forward-compat; built-in HPF/callbacks |
| 20 | esp-dsp / esp-nn for audio DSP + TFLite | **P1** | Audio/Vision | `securacv_audio.cpp:339` | Several-fold DSP; ~500→~60 ms Invoke |
| 21 | WPA3/PMF + per-device AP password | **P1** | WiFi | `canary_config.h:276` | Closes plaintext-AP + shared-secret exposure |
| 22 | TLS on the HTTP/peek surface | **P1** | Web | `securacv_network.cpp` | Encrypted LAN API + stream |
| 23 | Camera SCCB standby + XCLK gating/tuning | **P1** | Camera | `securacv_camera.cpp:111` | Lower idle draw + self-heat; OV5640 headroom |
| 24 | OV5640/OV3660 tuning parity + PID map fix | **P1** | Camera | `securacv_camera.cpp:392` | Correct image on shipped sensors |
| 25 | Graceful sleep teardown + `gpio_hold` + PSRAM down | **P1** | Power | `securacv_lowpower.cpp:213` | µA deep-sleep floor; no SD corruption |
| 26 | ULP RISC-V VBAT + tamper watcher | **P2** | Power | `securacv_lowpower.cpp:185` | Always-on witnessing at µA |
| 27 | I2C expandability: RTC (wall-clock) + IMU (tamper) | **P2** | Sensors/Pins | `pins.h` I2C D4/D5 | Trustworthy timestamps; robust tamper |
| 28 | Implement co-signing (`PWK/Endorsement/v1`) | **P2** | Crypto/Mesh | `spec/co_signing.md` | Cross-device attestation |
| 29 | ESP-NOW rate config / LR; FTM ranging | **P2** | Mesh | `mesh_transport.cpp` | Mesh range/reliability; fusion localization |
| 30 | Dead-code cleanup + stale-comment fixes | **P2** | All | §2 item 8 | Correct the record for the next reader |

---

*Generated as an engineering audit of the `firmware/canary` S3 build. Findings are anchored to
source; items in §5 depend on build-wiring that should be confirmed against a CI build log before
acting. Nothing here changes a shipped config default — that still goes through
[`CONFIG_CHANGES.md`](CONFIG_CHANGES.md) and the `secure_defaults.h` review process.*
