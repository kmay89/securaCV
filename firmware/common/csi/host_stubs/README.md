# Host stubs for compiling `csi_hal.cpp` on a PC

`csi_hal.cpp` is the ESP-IDF backend: it includes `<Arduino.h>` and
`<esp_wifi.h>`. These headers stand in for them so the *real* HAL — the
callback, the ring, the transmitter filter — compiles with plain `g++` and
the test (`../csi_hal_transmitter_filter_test.cpp`) can drive it through the
same driver surface the device uses. Every function declared here is
**defined by the test**, which is how the test controls the clock, what
`esp_wifi_sta_get_ap_info()` answers, and which callback got registered.

Only the members the HAL touches are modeled. Nothing here is compiled into
a device build: the Arduino and PlatformIO manifests build `src/` only, and
`check_csi_sync.sh` stages `src/` only.
