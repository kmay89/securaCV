# Canary WAP "mobile" — Lab descriptor (not compiled)

> **No build compiles this directory**, so none of the power overrides in
> `config.h` (record interval, mesh heartbeat, RF sampling, BLE advertising
> and TX power) reaches a device. See [`../default/README.md`](../default/README.md).

What `pio run -e canary-wap-mobile` actually builds is the sketch's
`BUILD_PROFILE_DEV` ([`envs/platformio/canary-wap.ini`](../../../envs/platformio/canary-wap.ini),
[`build_config.h`](../../../projects/canary-wap/arduino/canary_wap/build_config.h)):
Wi-Fi, HTTP and SD, without the camera, the Opera mesh or Opera/Chirp/Nearby
BLE discovery. No battery-life figure has been measured for it.
