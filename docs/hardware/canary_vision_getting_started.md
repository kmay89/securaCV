# Canary Vision — Getting Started (unboxing → watching it work)

One clean path from a sealed Seeed box to a Canary Vision publishing
presence claims into Home Assistant, with the camera aimed using the
boxes-only **Aim camera** card. Every step links to the deeper reference
if something goes sideways.

**Time:** ~45 minutes. **Skill:** you can copy a file and run two commands.

**Companion docs:**
[device guide](grove_vision_ai_v2_guide.md) (ports, wiring, protocol, recovery) ·
[firmware README](../../firmware/projects/canary-vision/README.md) ·
[BOM](bom_canary_vision.csv)

---

## 0. What you need

| Item | Notes |
|---|---|
| Grove Vision AI V2 module | Seeed SKU 101021112 |
| OV5647 CSI camera | **OV5647-62** recommended; -67 / -160 also supported. Other CSI cameras render green-only |
| XIAO ESP32-C3 **or** XIAO ESP32-S3 (plain) | The host that runs canary-vision. (An ESP32-C3 DevKitM-1 + Grove cable also works — see device guide §5) |
| 2× low-profile header sets | To stack the XIAO on the module (often bundled) |
| USB-C data cable | A *data* cable — charge-only cables are the #1 "no port appears" cause |
| Computer with **Chrome or Edge** | The model loader needs WebSerial; Firefox/Safari won't work |
| An MQTT broker + Home Assistant | e.g. the Mosquitto add-on; HA with the MQTT integration configured |
| 2.4 GHz WiFi credentials | The ESP32 hosts don't do 5 GHz-only networks |

---

## 1. Unbox and assemble (10 min)

1. **Camera first.** Lift the black latch on the module's CSI connector,
   slide the ribbon in with the **contacts facing the PCB** (check both
   ends — backwards insertion is the most common "no preview" cause),
   press the latch closed. Same procedure at the camera end.
2. **Solder the headers** onto the XIAO (pins point down).
3. **Stack the XIAO into the module's socket.** Orientation is critical:
   **both USB-C ports must face the same direction.** Backwards seating
   feeds power into GPIO and can kill either board (device guide §3).
4. Note that the assembly now has **two USB-C ports** that go to two
   different chips — this trips everyone once:

   | Port | Belongs to | Used for |
   |---|---|---|
   | On the big module PCB | Himax HX6538 | loading the AI **model**, SenseCraft preview |
   | On the small XIAO | ESP32 | flashing **canary-vision**, serial logs |

---

## 2. Load the AI model — one-time, Seeed's tool (5 min)

The detection model lives in the module's own 16 MB flash and survives
power cycles and every future firmware update — you do this **once**.
We deliberately use Seeed's SenseCraft web flasher for this step (and
only this step — see the privacy note below).

1. Open **Chrome/Edge** → <https://sensecraft.seeed.cc/ai/device/local/36>
   (SenseCraft AI → Vision Workspace → Grove Vision AI V2).
2. Plug the cable into the **module's USB-C port** (the big PCB one).
   No port listed? Install the CH343 driver — device guide §7.
3. Click **Connect** → pick the serial port ("USB Single Serial").
4. **Select Model → Person Detection** → wait 1–2 min. Keep the tab
   foregrounded; backgrounding it can abort the transfer.
5. **Sanity-check in the live Preview pane**: stand back, wave — you
   should see a box around yourself with a score. This preview (raw
   frames over the USB cable) is your bench check that camera + model
   work; after this step you'll never need it again — day-to-day aiming
   uses the boxes-only card in §6.
6. Confirm the model's class list shows **person as class 0** (that's
   the firmware default; a different model → adjust later from HA, §7).
7. Disconnect the cable.

> **Privacy note:** SenseCraft's preview streams camera frames to the
> browser over the USB cable you just plugged in — a physical, one-time,
> attended operation. The deployed system never does this: the ESP32
> host only ever receives box coordinates over I2C, and nothing in our
> firmware can export a frame. Rationale: strategy doc §5
> ("SenseCraft path — considered and rejected", except the flasher).

---

## 3. Flash canary-vision (10 min)

Now the **XIAO's USB-C port** (the small board). You need
[PlatformIO](https://platformio.org/install/cli) (`pip install platformio`).

```bash
git clone https://github.com/kmay89/securaCV.git
cd securaCV/firmware/projects/canary-vision

cp secrets/secrets.example.h secrets/secrets.h
# edit secrets/secrets.h: WiFi SSID/pass, MQTT broker host/port/user/pass

pio run -e canary-vision-xiao-c3 -t upload   # XIAO ESP32-C3
# or: pio run -e canary-vision-xiao-s3 -t upload   # XIAO ESP32-S3
pio device monitor                            # 115200 baud
```

Your first USB flash with real secrets seeds the device's NVS; the
signed OTA updates it installs later inherit that setup automatically.

**What you should see** on the monitor: the boot banner, then

- `Grove Vision AI ID=...` — **non-zero** means the I2C link to the
  module is alive (zero → §8),
- `Connected IP=... RSSI=...` — WiFi up,
- `MQTT ... Connected.` and a burst of `[DISC]` lines — the device just
  self-registered in Home Assistant.

---

## 4. Watch it appear in Home Assistant (2 min)

No YAML needed — the firmware registers everything via MQTT Discovery:

**Settings → Devices & Services → MQTT** → device
**"SecuraCV Canary Vision \<device_id\>"** (default id `canary_vision_001`).

You get: `Presence` + `Dwelling` binary sensors, `Confidence`, `Voxel`,
`Last event`, `Uptime`, WiFi RSSI + free-heap diagnostics, a `Firmware`
update entity with Install button, an `Auto Update` switch, an
**`Aim assist`** switch, and four runtime tuning numbers (§7).

Walk in front of the camera: `Presence` flips on; linger ~10 s and
`Dwelling` follows.

---

## 5. Add the dashboard (3 min)

Import
[`homeassistant/lovelace/securacv-vision-dashboard.yaml`](../../homeassistant/lovelace/securacv-vision-dashboard.yaml)
(Settings → Dashboards → ⋮ → Raw configuration editor, or a new
dashboard From YAML) and replace every `DEVICE_ID` with yours. It ships
three views: **Live** (presence, voxel grid, confidence, the Aim camera
card), **Tuning**, and **Firmware**.

The SecuraCV integration auto-serves the custom cards
(`custom:securacv-aim-card`, `custom:securacv-timeline-card`) — no
manual frontend-resource step. If you use MQTT discovery only (no
SecuraCV integration installed), add
`custom_components/securacv/www/securacv-aim-card.js` as a Lovelace
resource by hand.

---

## 6. Aim the camera — boxes, never pixels (5 min)

This is the in-place replacement for SenseCraft's preview: mount the
device where it will live, then watch **where detection boxes land**
without any frame ever leaving the device.

1. On the dashboard's Live view, press **Start aiming** on the
   *Aim camera* card (it flips the device's `Aim assist` switch; the
   live stream needs an **HA admin** user).
2. The firmware now streams box coordinates + scores at ~5 Hz on
   `securacv/<device_id>/aim` (local MQTT, non-retained, no pixels).
   The card draws the wireframe box, the score, and highlights the
   voxel cell — the coarse claim the witness will actually publish.
3. Walk the space: adjust camera tilt until people appear where you
   expect across the zone you care about, with scores comfortably above
   your threshold (default 70).
4. Press **Stop aiming** — or walk away; it turns itself off after
   10 minutes and it's off by default on every boot.

> Why not just re-open SenseCraft? That needs a laptop physically on the
> module's USB port, pauses I2C events to the host while connected, and
> streams raw frames. The aim card works over the device's normal local
> MQTT path, after mounting, from the couch.

---

## 7. Tune it (optional, live — no rebuild)

Dashboard → **Tuning** view (or the device's Configuration section).
All four persist in NVS across reboots and OTA updates:

| Setting | Default | When to change |
|---|---|---|
| Score threshold | 70 % | False positives → raise; missed detections → lower (watch scores in the aim card) |
| Lost timeout | 1500 ms | How long silence means "person left" |
| Dwell start | 10 s | Sustained presence before "dwelling" |
| Person class index | 0 | Only after loading a non-default model in SenseCraft |

---

## 8. If something doesn't work

| Symptom | Fix |
|---|---|
| No serial port for the module | CH343 driver / data cable — device guide §7 |
| `Grove Vision AI ID=0` | I2C link: XIAO seated fully? Correct env for your board? Module in bootloader mode (tap Reset)? |
| Preview worked, HA gets nothing | Unplug the SenseCraft laptop from the module port — it pauses I2C results (device guide §4) |
| Boxes on everything but people | Wrong model, or person ≠ class 0 → fix the class index in Tuning |
| Green-tinted preview | Non-OV5647 camera — swap it |
| Aim card says "needs an HA admin user" | The live MQTT stream uses HA's websocket MQTT API — sign in as an admin (or aim once as admin during setup) |
| Device online but no entities | MQTT integration not configured in HA, or broker credentials wrong in `secrets.h` |

Deeper recovery (bricked module bootloader, factory firmware restore):
device guide §7.

---

## 9. What you now have

A witness that detects people entirely on-device, publishes only coarse
claims (presence, dwell, a 3×3 voxel cell, confidence) to **your** MQTT
broker, self-updates only from Ed25519-signed releases when you press
Install (or opt into auto-update), and never stores or exports a frame.
Next steps: alert automations
([`securacv_vision_presence.yaml`](../../homeassistant/automations/securacv_vision_presence.yaml)),
and the radar sibling for camera-free rooms
([canary-sense](../../firmware/projects/canary-sense/README.md)).
