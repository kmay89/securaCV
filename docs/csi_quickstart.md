# CSI Quickstart

Two paths on one page. Pick the one that matches who you are.

---

## I just got a Canary and want it to work (5 minutes)

The "grandma path." No code, no terminal, no cables beyond USB-C for the
first power-up.

1. **Plug it in.** USB-C from the device to a phone charger, the wall, or
   a computer. The status LED breathes when it's running.
2. **Open the SecuraCV app on your phone.** Tap **Set up a new device**.
3. **Scan the QR code on the device or the box.** The app fills in the
   pairing token and the device's home network for you.
4. **Type your home WiFi password.** One screen. The phone hands the
   credentials to the device over Bluetooth so they never go anywhere
   else.
5. **The dashboard opens automatically** at `canary.local`. Watch the
   pearlescent orb at the centre of the screen — it should already be
   gently pulsing if anyone is in the room.
6. **Step out for one minute** when the dashboard suggests it. The device
   uses that minute to learn what your empty room looks like; afterwards,
   the orb settles when you're away and lights up when someone arrives.

That's it. No account, no cloud, no app to keep running. The app is just
the setup wizard — the device runs by itself.

---

## I'm a developer and want to hack on it (5 minutes)

The "open-source-developer path." A laptop, an ESP32-S3 board, and a
serial monitor.

```bash
# 1. Clone the repo and install the CSI library.
git clone https://github.com/kmay89/securacv.git
cd securacv
ln -s "$PWD/firmware/common/csi" \
      "$HOME/Documents/Arduino/libraries/csi"

# 2. Edit MY_SSID / MY_PASSWORD in the minimal sketch.
$EDITOR firmware/examples/csi_minimal/csi_minimal.ino

# 3. Build and flash.
arduino-cli compile --fqbn esp32:esp32:esp32s3 \
    --libraries firmware/common \
    firmware/examples/csi_minimal
arduino-cli upload  --fqbn esp32:esp32:esp32s3 \
    --port /dev/ttyACM0 \
    firmware/examples/csi_minimal

# 4. Watch the serial output.
arduino-cli monitor --port /dev/ttyACM0 -c baudrate=115200
```

You should see one line per second:

```
t=4   motion=12   breathing= 3   rssi=-42dBm   frames=18
```

Wave your hand near the device — the `motion` column rises. Sit still in
front of it for 20 seconds and the `breathing` column gently locks onto a
value. That's WiFi sensing on a $15 board, no camera, no cloud.

### Where to go next

| You want to... | Look at |
| --- | --- |
| Subscribe from a laptop in Python | `firmware/examples/csi_pc_listener/listener.py` |
| Add your own sensing behavior | `firmware/examples/modules/` and `docs/csi_modules.md` |
| Wire CSI into a different host firmware | `firmware/common/csi/README.md` |
| Understand the privacy contract | `firmware/common/csi/csi_event.h` and `spec/event_contract.md` |
| See what events the dashboard renders | `docs/csi_developer_api.md` |
| Tune coefficients live without recompiling | `http://canary.local/tune` (after Phase 4 lands) |

### Compatibility

| Chip | Supported | Notes |
| --- | --- | --- |
| ESP32-S3 | ✅ Primary | XIAO ESP32-S3 Sense is the reference board. |
| ESP32 | ✅ | HT20 only. |
| ESP32-C3 | ✅ | HT20 only. |
| ESP32-C6 | ✅ | First chip to expose 802.11bf-style sounding fields. |
| ESP32-S2 | ⚠️ | No CSI in stock IDF builds. |
| ESP8266 | ❌ | No CSI support. |

### Honest limits

- **One room is the gold standard.** Same-room motion and breathing
  detection work well on a single device.
- **Through one interior wall**, motion is reliable; breathing claims
  degrade to "subtle motion."
- **Through floors** is hit-or-miss — it depends on whether your floors
  are wood-frame (often works) or concrete (rarely).
- **Multi-body** detection counts to "more than one." Exact people
  counting needs MIMO or a second node, which a single ESP32-S3
  doesn't have.
- **Pets** trigger motion, not presence. The Pet Mode toggle uses the
  fact that small pets breathe outside the human Goertzel band, so
  sustained breathing only fires for people.

The dashboard's `?` affordance opens a "What it can / can't see" sheet
that shows users the same seams.
