# Grove Vision AI V2 — Device Guide

The Grove Vision AI V2 (Seeed SKU 101021112) is the optical sensor behind the
`canary-vision` firmware. This guide covers what the module is, how its ports
work (including the **two USB-C ports** that trip everyone up), how to load
the initial AI model onto the module, and how to wire it to each supported
ESP32 host.

**New to the hardware? Start with the end-to-end walkthrough:**
[`canary_vision_getting_started.md`](canary_vision_getting_started.md) (unboxing → aimed and publishing).

**Companion docs:**
[`firmware/projects/canary-vision/README.md`](../../firmware/projects/canary-vision/README.md) (firmware quickstart) ·
[`bom_canary_vision.csv`](bom_canary_vision.csv) (sourcing) ·
[`docs/strategy/10-grove-vision-ai-v2-program.md`](../strategy/10-grove-vision-ai-v2-program.md) (roadmap)

---

## 1. What it is and why it fits the Canary model

The module is a self-contained vision inference computer:

| Component | Spec |
|---|---|
| Processor | Himax WiseEye2 **HX6538** — dual-core Arm Cortex-M55 @ 400 MHz |
| NPU | Arm **Ethos-U55** micro neural accelerator (+ Helium DSP/ML vector ext.) |
| Memory | 2.4 MB on-chip SRAM, 16 MB external flash (133 MHz) |
| Camera | CSI connector — OV5647 family (62°/67°/160° FOV Raspberry Pi cameras) |
| Other peripherals | PDM microphone, microSD slot, USB-C, Grove connector, XIAO socket |
| Models | Person/face/gesture detection out of the box; YOLOv5/v8, MobileNet V1/V2, EfficientNet-lite; TensorFlow & PyTorch toolchains |
| Inference cost | Person detection runs entirely on-module; host receives JSON results only |

**Privacy posture (why we use it):** image capture, processing, and model
inference all happen inside the HX6538. In event mode the ESP32 host only
ever receives *semantic results* — bounding boxes, class IDs, confidence
scores — over a 4-wire I2C bus. No pixels cross the wire, which is exactly
the boundary Invariant I (No Raw Export) wants. The firmware then coarsens
boxes further into voxel-grid claims before anything leaves the device.

> The module *can* stream raw frames over its own USB port (SenseCraft
> preview) and the AT protocol *can* return base64 JPEGs (`AT+SAMPLE`).
> `canary-vision` never issues those commands; the preview path requires
> physically plugging a computer into the module's USB-C port.

---

## 2. The two USB-C ports — which one does what

A fully assembled Canary Vision (module + stacked XIAO) has **two USB-C
ports**, and they go to **two different computers**:

```
            ┌───────────────────────────────────┐
   CSI ──── │  Grove Vision AI V2 (carrier PCB) │
  camera    │   Himax HX6538 runs the AI model  │ ◄── USB-C  ►  "MODEL PORT"
            │                                   │     (CH343 serial → HX6538)
            │   Grove connector (I2C, 0x62)     │
            │   XIAO socket ▼ (I2C + UART)      │
            │  ┌─────────────────────────────┐  │
            │  │  XIAO ESP32-C3 / ESP32-S3   │  │ ◄── USB-C  ►  "FIRMWARE PORT"
            │  │  runs canary-vision         │  │     (native USB → ESP32)
            │  └─────────────────────────────┘  │
            └───────────────────────────────────┘
```

| Task | Plug your computer into | Port belongs to |
|---|---|---|
| Load / change the AI model (SenseCraft AI) | **Module's USB-C** (on the large carrier PCB, next to the Grove connector) | Himax HX6538 |
| Live camera preview, confidence/IoU tuning | **Module's USB-C** | Himax HX6538 |
| Update the module's own (Himax) firmware | **Module's USB-C** | Himax HX6538 |
| Flash `canary-vision` (PlatformIO upload) | **XIAO's USB-C** (on the small stacked board) | ESP32 |
| Serial monitor / debug logs | **XIAO's USB-C** | ESP32 |

Rules of thumb:

- **Model work → module port. Firmware work → XIAO port.** Neither port can
  do the other's job: the module port cannot see the ESP32, and the XIAO
  port cannot reach the Himax flash.
- You never need both cables at once for normal operation. Powering either
  port powers the stacked pair.
- If a "device not recognized" error appears on the module port, install
  the CH343 USB-serial driver (§7). The XIAO port needs no driver
  (native USB CDC).

---

## 3. Ports and interfaces reference

### Grove connector (I2C) — the 4-pin expansion port

The Grove socket on the module carries **I2C only**. The module is an I2C
*peripheral* at address **`0x62`**, 100 kHz or 400 kHz (the SSCMA library
defaults to 400 kHz).

| Grove wire | Signal | ESP32-C3 DevKit | XIAO ESP32-C3 | XIAO ESP32-S3 |
|---|---|---|---|---|
| Yellow | SCL | GPIO5 | GPIO7 (D5) | GPIO6 (D5) |
| White | SDA | GPIO4 | GPIO6 (D4) | GPIO5 (D4) |
| Red | VCC (3.3–5 V) | 3V3 | 3V3 | 3V3 |
| Black | GND | GND | GND | GND |

These pin assignments are authoritative in each board's
`firmware/boards/<board-id>/pins/pins.h`; `vision_mgr.cpp` passes them to
`Wire.begin()` explicitly.

### XIAO socket (stacking header)

The module has a female socket matching the XIAO footprint. A XIAO seated
there gets **both I2C and UART** with zero wiring. Orientation matters:
**the XIAO's USB-C must face the same direction as the module's USB-C** —
plugging it in backwards feeds power into GPIO and can kill either board.

### UART (alternative transport)

The module also speaks the same protocol over UART at a fixed **921600
baud** (header pins / XIAO socket). `canary-vision` uses I2C; UART is the
fallback if your host's I2C is occupied. Image transfer over UART is slow —
another reason event-only I2C is the right default.

### CSI camera connector

Flat-flex cable to an OV5647 camera (OV5647-62 recommended; -67 and -160
also have drivers). Mind the contact orientation on both ends — backwards
insertion is the most common "no preview" cause. Other CSI cameras may
enumerate but render green-only (no ISP driver), which degrades accuracy.

### SD slot and PDM microphone

Present on the module but **unused** by `canary-vision`: writing JPEGs to SD
(`AT+ACTION="save_jpeg()"`) and audio capture both violate the no-raw-export
posture. Leave the SD slot empty in deployments.

---

## 4. Loading the initial AI model (SenseCraft AI)

The module ships with a bootloader and usually a default firmware, but you
choose and load the **detection model** once before first deployment. The
model lives in the module's 16 MB flash and **persists across power cycles
and host reflashes** — you do not repeat this when you update canary-vision.

> **Prefer the Lab's own flasher.** The canary.local flash page
> (`canary-local/flash.html`) carries a module flow that burns the pinned
> person-detection model over WebSerial from our own page — SHA-256-verified
> against the release manifest (`manifest-vision-model.json`), followed by an
> AT handshake proof and an optional live bench preview with the TSCORE/TIOU
> sliders. No vendor account, no model catalog to misnavigate. The SenseCraft
> steps below remain the documented vendor fallback and work identically.

1. **Use Chrome or Edge** (the flasher needs WebSerial; Firefox/Safari won't work).
2. Plug your computer into the **module's USB-C port** (the carrier PCB one —
   see §2). The XIAO can stay stacked; just make sure the cable is in the
   module's port.
3. Open the SenseCraft AI Vision Workspace:
   <https://sensecraft.seeed.cc/ai/device/local/36>
   (or SenseCraft AI → Models → Workspace → Grove Vision AI V2).
4. Click **Connect** (top left) and pick the module's serial port
   (shows as **USB Single Serial** / CH343).
5. Click **Select Model** and pick the model. For canary-vision use
   **Person Detection** (the room-presence default). For a litter-box
   witness pick a **cat / animal detection** model instead — the host
   firmware adapts at runtime via the litter_box watch profile (§9), no
   rebuild.
6. Wait 1–2 minutes for the upload. **Stay on the tab** — backgrounding it
   can abort the transfer.
7. Verify in the live **Preview** pane that detections appear, and note two
   tuning sliders:
   - **Confidence** — minimum score to report a detection. The firmware
     applies its own threshold too (`SCORE_MIN` in `config.h`, default 70).
   - **IoU** — box-overlap threshold for de-duplication.
8. Check the model's **class list** in the workspace. canary-vision assumes
   the subject is **class 0** (`PERSON_TARGET` in `config.h` seeds the first
   boot). If your model numbers its classes differently, set the **class
   index** at runtime — the device's "Person class index" number entity in
   Home Assistant (or `securacv/<id>/cfg/target/set`) — no rebuild needed.
9. Disconnect. Done — the module now runs this model autonomously on boot.

> **Heads-up:** while a computer is connected to the module's USB port doing
> live preview, I2C results to the host are not delivered concurrently — the
> module does one job at a time. Unplug the preview before bench-testing the
> ESP32 event path.
>
> **After deployment, don't come back here to aim.** The firmware has a
> boxes-only *Aim assist* channel (HA switch + `securacv/<id>/aim` topic)
> and the dashboard ships an *Aim camera* card that draws the live bounding
> box + voxel grid over the device's normal local MQTT path — no laptop on
> the module port, no pixels. See the getting-started guide §6.

### Restoring factory module firmware

If the module's firmware is damaged or you need a clean slate:
[factory flasher bundle](https://files.seeedstudio.com/wiki/grove-vision-ai-v2/res/Vision_AI_Module_V2_factory_flasher.zip),
or re-flash via the SenseCraft process page. For a corrupted *bootloader*,
see §7.

---

## 5. Wiring options per host

### Option A — stacked XIAO (recommended; zero wiring)

Solder the header pins onto a XIAO ESP32-C3 or XIAO ESP32-S3 and seat it in
the module's socket, **USB-C ports facing the same way**. I2C and UART are
both connected through the socket. This is the form factor the
`canary-vision-xiao-c3` / `canary-vision-xiao-s3` build environments target.

### Option B — Grove cable to any host

4-pin Grove cable from the module's Grove socket to the host's I2C pins
(table in §3). Works with the ESP32-C3 DevKitM-1 (`canary-vision-default`
env), a XIAO on an expansion base, or any 3.3 V I2C-capable host. I2C only.

### Option C — discrete jumpers

SCL→SCL, SDA→SDA, 3V3→VCC, GND→GND. Same electrical path as Option B; used
for bench bring-up and for the bootloader-recovery procedure (§7).

---

## 6. Flashing canary-vision firmware (host side)

Always through the **XIAO's / DevKit's USB-C**, never the module's:

```bash
cd firmware/projects/canary-vision
cp secrets/secrets.example.h secrets/secrets.h   # then fill WiFi + MQTT
pio run -e canary-vision-xiao-c3 -t upload       # XIAO ESP32-C3
pio run -e canary-vision-xiao-s3 -t upload       # XIAO ESP32-S3
pio run -e canary-vision-default -t upload       # ESP32-C3 DevKitM-1
pio device monitor                               # 115200 baud
```

On boot the monitor prints a `Grove Vision AI ID=...` line — a non-zero ID
confirms the I2C link to the module is alive.

---

## 7. Recovery & troubleshooting

### CH343 driver (module port not recognized)

- Windows: [CH343SER.EXE](https://files.seeedstudio.com/wiki/grove-vision-ai-v2/res/CH343SER.EXE)
- macOS: [CH34xSER_MAC.ZIP](https://files.seeedstudio.com/wiki/grove-vision-ai-v2/res/CH341SER_MAC.ZIP)
- Linux — add a udev rule instead of a driver:

  ```bash
  echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="55d3", MODE:="0666"' \
    | sudo tee /etc/udev/rules.d/99-grove-vision-ai.rules
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```

### Boot / Reset buttons (on the module)

- **BootLoader mode:** hold **Boot**, plug in USB, release. (Or, while
  connected: hold Boot, tap Reset.) Needed for stubborn flashing sessions.
- **Reset:** taps the HX6538 if preview freezes or results stop.

### Bootloader recovery over I2C (bricked module)

The module's bootloader can be restored *through the host MCU*: flash the
`we2_iic_bootloader_recover` example from the
[Seeed_Arduino_SSCMA](https://github.com/Seeed-Studio/Seeed_Arduino_SSCMA)
library onto the ESP32, connect the module over I2C, open the serial
monitor, and press Enter when the device is detected. Hold the module's
BOOT button while connecting power; expect to need several attempts
(3–10 is normal per Seeed).

### Common symptoms

| Symptom | Likely cause / fix |
|---|---|
| `Grove Vision AI ID=0` or init failure | I2C wiring/pins wrong (check board table §3), Grove cable on wrong socket, or module in bootloader mode (tap Reset) |
| Preview works, ESP32 gets nothing | Preview computer still attached to module port — the module won't serve both (§4 note) |
| Detections for everything except person | Wrong model loaded, or `PERSON_TARGET` ≠ the model's person class index |
| Green-tinted preview | Unsupported CSI camera (no color pipeline) — use OV5647 family |
| No serial output on XIAO ESP32-C3 after upload | Re-open the monitor after pressing Reset (C3 USB Serial/JTAG re-enumerates) |
| Module dead after stacking XIAO | XIAO seated backwards — see orientation warning §3; inspect for damage |

---

## 8. Protocol reference (for firmware developers)

The module runs Seeed's **SSCMA-Micro** firmware speaking an AT-style
protocol over I2C/UART/USB
([spec](https://github.com/Seeed-Studio/SSCMA-Micro/blob/dev/docs/protocol/at_protocol.md)).
The `Seeed_Arduino_SSCMA` library (pinned at v1.0.3 in
`firmware/envs/platformio/canary-vision.ini`) wraps it:

- `AI.begin(&Wire, addr=0x62, …, clock=400000)` — init (call
  `Wire.begin(I2C_PIN_SDA, I2C_PIN_SCL)` first).
- `AI.invoke(times, filter, show)` — run inference; `filter=true` only
  replies when results change; `show=false` suppresses image data.
- `AI.boxes()` → `{x, y, w, h, score, target}` (box center coords, score
  0–100, target = class index).
- `AI.classes()` / `AI.points()` — classification / keypoint model outputs.
- `AI.perf()` — preprocess/inference/postprocess times (ms).

Raw protocol shape (what's on the wire):
`AT+INVOKE=1,0,1\r` → `\r{"type":1,"name":"INVOKE","code":0,"data":{"boxes":[[x,y,w,h,score,target]],"perf":[...]}}\n`.

Commands canary-vision deliberately does **not** use: `AT+SAMPLE` (returns
base64 JPEG frames), `AT+ACTION="save_jpeg()"` (writes frames to SD),
`AT+WIFI`/`AT+MQTTSERVER` (the module's own SenseCraft cloud/MQTT uplink —
our events go through the host firmware's local MQTT instead).

---

## 9. Watch profiles (room presence · litter box)

One firmware, several jobs. The host firmware ships **watch profiles** —
per-use-case tuning presets selected at runtime from Home Assistant (the
device's **Watch profile** select) or over MQTT
(`securacv/<id>/cfg/profile/set`, key or label):

| Profile | Model to load (§4) | Preset (score / lost / dwell) | Beacon class |
|---|---|---|---|
| `room_presence` (default) | Person Detection | 70 % / 1500 ms / 10000 ms | person |
| `litter_box` | a cat / animal detection model | 60 % / 4000 ms / 8000 ms | animal |

Selecting a profile applies its preset to the four runtime detection
settings in one step; each stays individually tunable afterward, and
re-selecting the profile restores the preset. The presets live in
`firmware/projects/canary-vision/include/canary/detect_profiles.h`, with a
host test pinning them inside the tunable bounds.

**What a profile does NOT change:** the event vocabulary
(`presence_started` / `dwell_started` / `dwell_ended` / `presence_ended` /
`interaction_likely`), the signed witness envelope, and the privacy posture
are identical across profiles — a litter-box visit is the same
presence→dwell→leave shape a room walk-through is, read against a
different subject. No new claim types (Invariant VI), no pixels on the
wire either way.

### Litter box setup, end to end

1. Load a cat/animal detection model on the module (§4 — the module's own
   USB-C port, one time).
2. Flash/keep the normal canary-vision host firmware (§6) and let it join
   your broker.
3. In HA, set **Watch profile → Litter box**, then set the class index to
   the model's cat class (single-class models are usually `0`).
4. Placement: litter boxes live in already-lit spaces, so no low-light
   heroics are needed — mount with the whole box in frame from 0.5–2 m,
   aim **across** the box rather than into a lamp or window, and use the
   Aim card to verify the box fills a known voxel cell.
5. Install the litter recipes:
   [`homeassistant/automations/securacv_litterbox.yaml`](../../homeassistant/automations/securacv_litterbox.yaml)
   (visit-completed notification + a no-visit-in-24h wellness alert) and
   [`homeassistant/lovelace/securacv-litterbox-dashboard.yaml`](../../homeassistant/lovelace/securacv-litterbox-dashboard.yaml).

How the events read for a litter box: `presence_started` = cat arrived,
`dwell_started` = a real visit (stayed past 8 s, not a walk-by sniff),
`interaction_likely` (reason `dwell_then_left` or
`zone_interaction_then_left`) = **visit completed**, `presence_ended` =
cat gone. The lost timeout is deliberately long because a digging cat
drops detection frames constantly — a short timeout fragments one visit
into five.

For a room-presence witness nothing changes: the default profile is the
firmware you already know, and the existing
`securacv_vision_presence.yaml` automations now skip events published by a
litter_box-profile device so both can share a broker without cross-firing.

---

## 10. Vendor resources

- [Product wiki](https://wiki.seeedstudio.com/grove_vision_ai_v2/) ·
  [XIAO demo projects](https://wiki.seeedstudio.com/grove_vision_ai_v2_demo/) ·
  [Software support / model loading](https://wiki.seeedstudio.com/grove_vision_ai_v2_software_support/) ·
  [AT command guide](https://wiki.seeedstudio.com/grove_vision_ai_v2_at/) ·
  [Supported cameras](https://wiki.seeedstudio.com/Grove-vision-ai-v2-camera-supported/)
- [HX6538 datasheet (PDF)](https://files.seeedstudio.com/wiki/grove-vision-ai-v2/HX6538_datasheet.pdf) ·
  [Circuit diagram (PDF)](https://files.seeedstudio.com/wiki/grove-vision-ai-v2/Grove_Vision_AI_Module_V2_Circuit_Diagram.pdf)
- [Himax WE2 SDK](https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2) ·
  [SSCMA-Micro firmware](https://github.com/Seeed-Studio/SSCMA-Micro) ·
  [Seeed_Arduino_SSCMA library](https://github.com/Seeed-Studio/Seeed_Arduino_SSCMA)

> **Note on Seeed's Home Assistant tutorial:** Seeed's
> [Application for HomeAssistant](https://wiki.seeedstudio.com/sensecraft-ai/application/application-for-homeassistant/)
> path routes device data through the SenseCraft adapter firmware + HACS
> plugin (and the module's own MQTT/WiFi stack). We intentionally do *not*
> use it: canary-vision publishes directly to your local broker with our
> own schema, signing, and HA MQTT Discovery — no vendor account, no cloud
> dependency, and the privacy chokepoint stays in our firmware. See the
> program doc (§ "SenseCraft path — considered and rejected") for the full
> rationale.
