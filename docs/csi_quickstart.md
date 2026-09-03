# CSI Quickstart

Two paths on one page. Pick the one that matches who you are.

---

## I just got a Canary and want it to work (5 minutes)

The "grandma path." No app to install, no account to create. The device
ships with a captive-portal setup page that walks the phone through it.

1. **Plug it in.** USB-C from the device to a phone charger, the wall, or
   a computer. The status LED breathes when it's running.
2. **Open Wi-Fi on your phone and pick `SecuraCV-XXXX`.** Your phone will
   pop up a "Sign in to network" sheet automatically — that's the
   captive-portal setup page the device serves.
3. **Scan the big QR code on the page** with your phone's camera.
   (Camera not working? There's a "Tap here to set up by hand" link
   under the code that opens the same setup page in your browser.)
4. **Pick your home Wi-Fi and type the password.** One screen each.
   The credentials go directly to the device — they never leave your
   home.
5. **Switch your phone back to your home Wi-Fi** and tap the
   "Open canary.local" button. The pearlescent dashboard opens, and
   the orb at the center should already be gently pulsing if anyone is
   in the room.
6. **Step out for one minute** when the dashboard suggests it. The device
   uses that minute to learn what your empty room looks like; afterwards,
   the orb settles when you're away and lights up when someone arrives.

That's it. No account, no cloud, no app to keep running. The setup page
is served by the device itself; once your home Wi-Fi is configured, the
captive-portal AP turns into a quiet fallback for re-pairing later.

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
| Understand the privacy contract | `firmware/common/csi/src/csi_event.h` and `spec/event_contract.md` |
| See what events the dashboard renders | `docs/csi_developer_api.md` |
| Tune coefficients live without recompiling | `http://canary.local/tune` (long-press the device-id chip in the topbar to reveal it, or append `?tune=1` to any dashboard URL) |
| Inspect the captive-portal pairing flow | `firmware/projects/canary-wap/arduino/canary_wap/setup_page_html.h` and `handle_captive_portal` in `canary_wap.ino` |
| See the one-shot pairing token contract | `csi_integration::pair_token_*` in `csi_integration.h` |

### Compatibility

| Chip | Supported | Notes |
| --- | --- | --- |
| ESP32-S3 | ✅ Primary | XIAO ESP32-S3 Sense is the reference board. |
| ESP32 | ✅ | HT20 only. |
| ESP32-C3 | ✅ | HT20 only. |
| ESP32-C6 | ⚠️ compiles, bench-unverified | ESP-IDF 5.1+ gives the C6 (and C5/C61) a different `wifi_csi_config_t` — the `wifi_csi_acquire_config_t` bitfields — so the HAL as first written did not compile there at all, whatever the table said. `csi_idf_compat.h` now fills whichever shape the target exposes and is compile-tested against every IDF revision of both (`tests_host/test_csi_idf_compat.cpp`); the frame path is the same 52-tone L-LTF canonical set as every other part. **No C6 has run this HAL yet** — treat it as untested until a bench log says otherwise. HE-LTF (Wi-Fi 6) acquisition is deliberately left off: its tone count differs per PPDU type and needs its own map. No ESP32 exposes IEEE 802.11bf sounding in ESP-IDF; `CSI_CAP_SOUNDING_11BF` stays reserved. |
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
- **The breathing rate is host-tested, not bench-tested.** The envelope
  is gain-invariant (band shares of the AGC-normalized frame) and is
  resampled onto a fixed 1 Hz grid from each window's close timestamp,
  so it survives the receiver's per-packet AGC and a loop that runs
  early, late or stalls — on synthetic frames. No device has produced a
  bench log for the breathing path yet; `csi_stats_t`'s `windows_held`,
  `windows_merged` and `window_period_ms` show how far a real loop's
  cadence was from the grid.

The dashboard's `?` affordance opens a "What it can / can't see" sheet
that shows users the same seams.
