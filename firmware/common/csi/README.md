# SecuraCV CSI

Privacy-preserving WiFi Channel State Information sensing for ESP32-S3.

This library turns a single `$15` ESP32-S3 board into a camera-free room sensor
that detects motion, breathing rhythm, and presence — without storing or
transmitting MAC addresses, raw I/Q samples, or precise timestamps.

It powers the SecuraCV Canary product, and is published as a stand-alone
Arduino / PlatformIO library so any maker or researcher can drop it into their
own sketch.

## What you get

| Layer | What it does | File |
| --- | --- | --- |
| `csi_hal` | Wraps `esp_wifi_set_csi_rx_cb()`. Lock-free ring buffer between the WiFi task and the main loop. **Scrubs MAC / BSSID / FCS at the interrupt boundary** before any data is buffered, and canonicalizes each frame to 52 L-LTF tones on the way in. | `csi_hal.{h,cpp}` |
| `csi_features` | Aggregates ~20 Hz CSI frames into one 32-dim `int8` feature vector per 1 s window. Subcarrier amplitude variance × 8, phase-difference Doppler × 4, breathing Goertzel bins 0.10–0.45 Hz × 8 over a cross-window envelope (gain-invariant band shares, resampled onto a fixed 1 Hz grid from each window's close time), RSSI stats × 4, frame-health × 4. | `csi_features.{h,cpp}` |
| `csi_types` | Privacy invariants, capability flags, the `csi_features_t` contract. | `csi_types.h` |
| `csi_subcarriers` | Reduces every frame — non-HT (128 B) or HT (256 B), 20 or 40 MHz — to the same 52 L-LTF data+pilot tones in frequency order, so a window never mixes tone counts and the twelve null tones stay out of the AGC mean. Header-only; detects the tone ordering from the frame's own null tones. | `csi_subcarriers.h` |
| `csi_idf_compat` | The one place the ESP-IDF driver-config field names live: legacy `lltf_en`/`htltf_en`/… on ESP32/S3/C3, the `acquire_csi_*` bitfields on C6/C5/C61. Compile-tested against four IDF struct shapes. | `csi_idf_compat.h` |
| `csi_traffic` | Frame supply for a solo device: pings the gateway at the HAL's target rate so every echo reply is a CSI frame addressed to us (a device cannot receive its own ESP-NOW probes). Same remedy as espressif/esp-csi's router examples. Policy host-tested. | `csi_traffic.{h,cpp}` |
| `csi_module` | Tiny module-interface for layered sensing: `init` / `tick(features)` / `emit_event` / `on_event_dismissed` / `deinit`. The expansion path. | `csi_module.h` |
| `csi_event` | The single privacy chokepoint. Tags every event with a class (`P0`/`P1`/`P2`), coarsens timestamps at emit time, strips fields not on the module's allow-list, and routes through the optional witness chain. | `csi_event.{h,cpp}` |
| `csi_bundler` | Same-state events within a 10-minute sliding window collapse into one row with a duration. Stops the dashboard from flickering. | `csi_bundler.{h,cpp}` |
| v1 modules | `core.presence`, `core.breathing`, `core.activity_ribbon`, `meta.daily_summary`, `anomaly.baseline`. | `core_*.{h,cpp}`, `meta_*.{h,cpp}`, `anomaly_baseline.{h,cpp}` |

## Privacy invariants (enforced at runtime, not in docs)

1. The source MAC and BSSID are scrubbed from the raw frame before it enters
   any buffer that outlives a single ISR callback.
2. Only aggregated, non-identifying features cross the public interface.
3. No subcarrier sample is ever exported.
4. Features are bucketed to `int8` so fine-grained side channels are lost.
5. No per-frame timestamp is exported — only a coarse 10-minute bucket index
   matching the SecuraCV event contract.
6. Every emitted event carries a privacy class. `P0` is contract-conformant by
   construction; `P1` is opt-in; `P2` never leaves the device.
7. A `conformance_check_no_mac_in_buffers()` helper scans the ring buffer for
   anything resembling a MAC address — used by the SecuraCV test suite.

## 30-second quick start

```cpp
#include <csi_hal.h>

void on_window(const csi_features_t* f) {
  // Each window is 1 second. f->v[0..7] is amplitude variance per band,
  // f->v[8..11] is phase-Doppler, f->v[12..19] is the breathing FFT, etc.
  Serial.printf("motion=%d breathing=%d frames=%u\n",
    (int)f->v[8], (int)f->v[14], f->frames_in_window);
}

void setup() {
  Serial.begin(115200);
  WiFi.begin();                    // any STA association is fine
  csi_hal::Config cfg = csi_hal::Config::defaults();
  csi_hal::init(cfg);
  csi_hal::set_features_callback(on_window);
  csi_hal::start();                // safe before WiFi is up; will defer
}

void loop() {
  csi_hal::process();              // pumps the ring; emits one window/sec
}
```

A complete example lives in `firmware/examples/csi_minimal/csi_minimal.ino`.

## Hardware compatibility

| Chip | Supported | Notes |
| --- | --- | --- |
| ESP32-S3 | ✅ Primary | XIAO ESP32-S3 Sense is the SecuraCV reference board. |
| ESP32 (original) | ✅ | HT20 only on most variants. |
| ESP32-C3 | ✅ | HT20 only. |
| ESP32-C6 | ⚠️ compiles, bench-unverified | ESP-IDF 5.1+ gives the C6 (and C5/C61) a different `wifi_csi_config_t` — the `wifi_csi_acquire_config_t` bitfields — so the HAL as first written did not compile there at all, whatever the table said. `csi_idf_compat.h` now fills whichever shape the target exposes and is compile-tested against every IDF revision of both (`tests_host/test_csi_idf_compat.cpp`); the frame path is the same 52-tone L-LTF canonical set as every other part. **No C6 has run this HAL yet** — treat it as untested until a bench log says otherwise. HE-LTF (Wi-Fi 6) acquisition is deliberately left off: its tone count differs per PPDU type and needs its own map. No ESP32 exposes IEEE 802.11bf sounding in ESP-IDF; `CSI_CAP_SOUNDING_11BF` stays reserved. |
| ESP32-S2 | ⚠️ | No CSI in stock IDF builds. |
| ESP8266 | ❌ | No CSI support. |

## Building

### PlatformIO

Add to `platformio.ini`:

```ini
lib_deps =
  https://github.com/kmay89/securacv.git#main
build_flags =
  -I${PROJECT_DIR}/../../common/csi
```

(The SecuraCV monorepo wires this for you in `firmware/envs/platformio/canary-wap.ini`.)

### Arduino CLI

```bash
arduino-cli compile --fqbn esp32:esp32:esp32s3 \
    --libraries firmware/common \
    arduino/canary_wap
```

### Arduino IDE

Symlink (or copy) `firmware/common/csi/` into your Arduino libraries folder
(`~/Documents/Arduino/libraries/csi/` on macOS / Linux).

## Modular sensing pipeline

If you want more than the raw features, register modules. Each module declares
its privacy class and event allow-list up front, and the runtime enforces both
on emit. See `csi_module.h` for the contract and `firmware/examples/modules/`
for a stub example.

The five modules listed above are the v1 set; `docs/csi_modules.md` covers
the events each one emits, the tunables they expose, and how to add your
own alongside them.

## Captive-portal pairing helper (host-side)

If you're embedding the library into a product that wants the same
"join AP → scan QR → onboarded" flow that SecuraCV ships, the
`csi_integration` namespace exposes a small RAM-only pairing-token store
your captive-portal handler can reach for:

```cpp
char hex[csi_integration::PAIR_TOKEN_HEX_LEN + 1];
if (csi_integration::pair_token_issue(hex, sizeof(hex))) {
  // Embed `hex` in a QR pointing at /companion?token=<hex>
}
// Later, the companion endpoint validates / consumes:
if (csi_integration::pair_token_consume(received_hex)) {
  // legitimate handoff; proceed with provisioning
}
```

Tokens are 32 random bytes (`esp_fill_random()`), expire after 10 minutes,
and are single-use. The store is intentionally NOT a security boundary
— the AP password is — but it makes the QR handoff legible to the PWA
and prevents stale URLs from accidentally re-running provisioning.

The complete reference implementation (setup page, QR rendering with
the vendored Nayuki encoder, companion-PWA 4-card wizard) lives under
`firmware/projects/canary-wap/arduino/canary_wap/`.

## License

MIT — see `LICENSE`.
