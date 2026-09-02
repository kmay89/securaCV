# Wi-Fi sensing (CSI): the open-source landscape and what we took from it

**Status (2026-09).** A survey of every open-source Channel State Information
stack worth reading, measured against what `firmware/common/csi` already does,
and the concrete changes it produced. Read this before touching the CSI HAL or
proposing "just use library X" — most of the well-known tools solve a different
problem (bulk raw-I/Q capture for offline research) than a Canary does (privacy-
bounded, on-device, no raw samples ever leave the chip).

## 1. The landscape

| Project | What it is | Hardware | License | Useful to us because | Not a drop-in because |
|---|---|---|---|---|---|
| [espressif/esp-csi](https://github.com/espressif/esp-csi) — `examples/get-started/*` | Espressif's reference CSI capture: `csi_recv`, `csi_send`, `csi_recv_router` | every ESP32 (ESP32 / S2 / C3 / S3 / C5 / C6 / C61) | Apache-2.0 | Canonical per-target `wifi_csi_config_t` code (the C5/C6 `acquire_csi_*` shape vs the legacy `lltf_en` shape); the router example pings the gateway to make frames and keeps **only the L-LTF tones** "for router compatibility" | Prints raw I/Q to a serial port for a PC to analyze — the exact thing our privacy barrier forbids |
| esp-csi — **`esp-radar`** component (Component Registry `esp-radar >= 0.3.0`) | Espressif's on-device human-motion detector | ESP32 family, IDF ≥ 4.4.1 | Apache-2.0 | Two dimensionless metrics — `waveform_wander` (slow drift = someone there) and `waveform_jitter` (fast change = movement) — plus an **on-site training step** (`esp_radar_train_start/stop` → `someone_threshold`, `move_threshold`) and a decoder config (`ltf_type`, `sub_carrier_step_size`, `outliers_threshold`) | Binary component; filters by MAC (`filter_mac`/`filter_dmac`) and logs per-packet detail we would have to scrub |
| esp-csi — **`esp_wifi_sensing`** component (`>= 0.1.1`, IDF ≥ 5.4) | Newer state machine: ACTIVE/INACTIVE presence with `motion_sensitivity`, `active_jitter_min`, enter/exit levels, `RESET_BASELINE`, `esp_wifi_sensing_fsm_ping_router_start()` | ESP32 family | Apache-2.0 | Confirms the two design choices we made independently: a **router-ping frame supply** and **empty-room baselining** are how Espressif ships it too | IDF 5.4+ component with a Web Serial monitor; not an Arduino library |
| [StevenMHernandez/ESP32-CSI-Tool](https://github.com/StevenMHernandez/ESP32-CSI-Tool) | Active (STA/AP) and passive CSI capture to CSV/SD | ESP32, IDF v4.3 | MIT (research citation requested) | The clearest statement of the L-LTF layout (128 bytes = 64 tones × [imag, real] int8) and of `channel_filter_en = 0` for tone independence | Research capture tool; no on-device inference |
| [Gi-z/CSIKit](https://github.com/Gi-z/CSIKit) | Python parsing + visualization for Atheros, Intel, Nexmon, ESP32, FeitCSI, PicoScenes formats | host | MIT | A ready-made ESP32 CSI reader for the **Lab's** offline test vectors and for `firmware/examples/csi_pc_listener` | Host-side only |
| [nexmon_csi](https://github.com/seemoo-lab/nexmon_csi) | Firmware patches exposing CSI on Broadcom chips (Raspberry Pi 3B+/4, some routers), 256 tones at 80 MHz | Broadcom | GPL-2.0 (nexmon) | A **hub-side** sensor on the Raspberry Pi hub would see far more tones than any ESP32 | GPL + a patched Wi-Fi firmware on the hub's only radio; noisier than the commercial NIC tools |
| [PicoScenes](https://ps.zdns.cn/) | Research platform, Wi-Fi 4–7, up to 320 MHz / 1024 tones, Intel AX200/AX210, Qualcomm, SDRs | PC NICs | proprietary-ish (free for research) | The reference for what "good" CSI looks like when validating our features | Not a product component |
| FeitCSI | CSI extraction on Intel AX2xx via a patched iwlwifi | Linux + Intel | GPL | Same hub-side story as nexmon, on newer silicon | GPL, kernel patch |
| IEEE 802.11bf (2025) | The Wi-Fi sensing amendment: sounding frames, sensing-by-proxy | none shipped | — | Our `CSI_CAP_SOUNDING_11BF` bit is reserved for it | **No ESP32 exposes 11bf sounding in ESP-IDF** as of IDF 5.5 / 6.0 — the bit stays unset everywhere |

Two research surveys are worth the hour if you want the theory: the tutorial
["Hands-on Wireless Sensing with Wi-Fi"](https://arxiv.org/abs/2206.09532)
and the generalizability survey at
[arXiv 2503.08008](https://arxiv.org/abs/2503.08008) (why thresholds trained
in one room drift in another — the reason `meta.empty_room_baseline` exists).

## 2. What was actually wrong in our stack, and what changed

Reading the landscape against `csi_hal.cpp` / `securacv_csi.cpp` surfaced
three defects. All three are fixed on this branch; the first two are covered by
host tests, the third by a host-tested policy plus a device path copied from
esp-csi's own examples.

1. **The HAL did not compile on the chips the README listed.** ESP-IDF 5.1+
   typedefs `wifi_csi_config_t` to `wifi_csi_acquire_config_t` on the C6 / C5
   / C61 (bitfields `enable`, `acquire_csi_legacy`, `acquire_csi_ht20`, …), and
   both HALs filled the legacy `lltf_en` / `htltf_en` fields unconditionally.
   The README's "ESP32-C6 ✅" was a false statement (AGENTS.md rule 4). Fix:
   [`csi_idf_compat.h`](../firmware/common/csi/src/csi_idf_compat.h) fills
   whichever shape the target exposes and is compile-tested against four IDF
   struct revisions (`tests_host/test_csi_idf_compat.cpp`). The docs now say
   "compiles, bench-unverified" — because it is.
2. **Frames of different lengths were mixed inside one window.** With
   `lltf_en + htltf_en + ltf_merge_en`, a non-HT frame (every router beacon)
   is 64 tones and an HT frame is 128 (L-LTF + HT-LTF). The HAL copied
   `len / 2` pairs and `csi_features` locked the tone count on the first frame
   of the window — so a window that opened on an HT frame read the following
   beacons' missing half as zeros (bands 4..7 → large fake "motion"), and one
   that opened on a beacon truncated the HT frames. The twelve null tones (DC
   and guards) also sat inside the AGC mean. Fix:
   [`csi_subcarriers.h`](../firmware/common/csi/src/csi_subcarriers.h) reduces
   every frame to the **52 L-LTF data+pilot tones in frequency order** — the
   same tone set esp-csi's router example keeps, and the exact synthetic
   channel `test_csi_features.cpp` already modeled. The tone ordering (FFT
   order for 20 MHz, linear for 40 MHz) is cross-checked against the frame's
   own null tones, so a driver revision that reorders the buffer degrades to a
   detectable condition rather than a silent one.
3. **A solo device was starved of frames.** CSI is measured on frames a
   device *receives*; `csi_probe`'s ESP-NOW broadcasts light up *other*
   Canaries, and a device cannot hear itself. On home Wi-Fi a single Canary
   lived on the access point's ~10 beacons/s against a 20-frame window, so
   every window was "degraded" and the breathing ring filled at half speed.
   Fix: [`csi_traffic`](../firmware/common/csi/src/csi_traffic.h) pings the
   gateway at the HAL's target rate (esp-csi's `csi_recv_router`, esp-radar
   and `esp_wifi_sensing` all do exactly this); each echo reply is a frame
   addressed to us. The start/stop policy is host-tested; the device path is
   the lwIP `esp_ping` session API with a one-byte payload. It runs only while
   `csi_hal` runs and the power gate allows CSI.

## 3. What we deliberately did not take

- **Raw-sample export** (every capture tool). Privacy invariant 3: no
  subcarrier sample ever leaves the device. The Lab gets synthetic vectors.
- **MAC filtering** (`esp-radar`'s `filter_mac`). Invariant F (symmetry) and
  the scrub barrier forbid the HAL from knowing which station a frame came
  from; the router-ping supply gives us a deterministic source without it.
- **HE-LTF acquisition on the C6.** Wi-Fi 6 long training fields carry ~242
  tones at 20 MHz, but the count differs per PPDU type (SU / MU / DCM /
  beamformed), which is the mixed-count problem of §2.2 all over again. It
  needs its own tone map and a bench before it is turned on.
- **A hub-side Broadcom/Intel sensor** (nexmon, FeitCSI). GPL firmware
  patches on the hub's only radio, for a capability the Canary already has.

## 4. What to do next, in order

1. **Bench the C6 path.** One XIAO ESP32-C6 running `firmware/examples/csi_minimal`
   with the new shim, a serial log showing `frames=` non-zero, and the table
   rows in `firmware/common/csi/README.md` / `docs/csi_quickstart.md` flip from
   "bench-unverified" to "verified". Nothing else should change.
2. **Adopt esp-radar's two metrics as first-class features.** `waveform_wander`
   (slow drift of the per-tone amplitude vector against a baseline) and
   `waveform_jitter` (frame-to-frame change) map onto our `v[0..7]` and
   `v[8..11]` bands but are scalar, dimensionless, and have a published
   calibration recipe (`train_start` in an empty room → thresholds). Adding
   them to the reserved `v[28..31]` slots keeps the 32-byte contract and gives
   `core.presence` a second opinion that is already field-proven.
3. **Make the Lab's radar model the firmware's.** `canary-local/assets/radar-emu.js`
   models the mmWave Sense product; nothing in the Lab runs the CSI feature
   extractor. Compiling `csi_features.cpp` + `csi_subcarriers.h` to WASM (the
   emulator already does this for the display firmware) and feeding it the
   host tests' synthetic channels would let the WAP bench show real motion /
   breathing numbers instead of a canned animation.
4. **Publish the offline vectors.** A small set of recorded, already-reduced
   52-tone windows (no MACs, no raw frames — they are what the HAL emits) so
   `csi_pc_listener`, CSIKit users, and the emulator all test against the same
   truth.

## 5. Honest limits that did not change

- One room is the gold standard; CSI does not see through a closed door well.
- Breathing needs a still subject and ≥ 24 one-second windows before any bin
  reports; that is physics, not a tunable.
- A 40 MHz router still yields 52 tones per frame here (the primary channel's
  L-LTF), not 108; the HT-LTF path that would give more is exactly what §2.2
  removed from the window for consistency.
