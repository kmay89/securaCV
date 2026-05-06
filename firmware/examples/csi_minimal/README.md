# csi_minimal

The smallest useful WiFi CSI sketch on ESP32-S3 — about 80 lines of code,
no dashboard, no event pipeline, no dependencies beyond the SecuraCV CSI
library and the Arduino-ESP32 core.

```
                    +---------------------+
                    |  ESP32-S3 / -C3 / .. |
   2.4 GHz WiFi --> |  esp_wifi CSI cb     |
                    |        |             |
                    |  csi_hal (this lib)  |
                    |        |             |
                    |  csi_features        |  --->  Serial @ 115200
                    +---------------------+         t=4  motion=12  breathing=3  rssi=-42dBm  frames=18
```

## Quick start (3 commands)

```bash
# 1. Install the library so the IDE / CLI can find it.
ln -s "$PWD/../../common/csi" ~/Documents/Arduino/libraries/csi   # macOS / Linux

# 2. Edit MY_SSID / MY_PASSWORD at the top of csi_minimal.ino, then build.
arduino-cli compile --fqbn esp32:esp32:esp32s3 --libraries ../../common .

# 3. Flash and watch the serial output.
arduino-cli upload  --fqbn esp32:esp32:esp32s3 --port /dev/ttyACM0 .
arduino-cli monitor --port /dev/ttyACM0 -c baudrate=115200
```

If the `motion` column rises when you wave your hand within a few metres of
the board, CSI is working. Sit still and the `breathing` column should
gently lock onto a value in the 0.15–0.45 Hz band.

## What's actually happening

- `csi_hal::init()` registers an ESP-IDF callback for CSI frames and sets up
  a small lock-free ring buffer between the WiFi task and your `loop()`.
- For each incoming frame, the source MAC, BSSID, and FCS are scrubbed
  inside the callback before any data is buffered. (See
  `csi_hal::conformance_check_no_mac_in_buffers()`.)
- `csi_features` aggregates the ~20 frames per second into one 32-dim
  `int8` feature vector per 1-second window and invokes your callback.

The 32-dim layout is documented in `csi_features.h`. The two numbers this
sketch prints (`motion`, `breathing`) are simple averages of the
phase-Doppler and breathing-FFT bands. Real applications layer the
`csi_module` / `csi_event` pipeline on top — see `csi_pc_listener` and
`firmware/examples/modules/` for the next steps.

## License

MIT — see `firmware/common/csi/LICENSE`.
