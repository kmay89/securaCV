# Canary WAP — Lab descriptor (not compiled)

> **No build compiles this directory.** No PlatformIO env or `arduino-cli`
> invocation puts `firmware/configs/canary-wap/` on an include path, and the
> sketch includes no `config.h`. Nothing in `config.h` here configures a
> device. (Audit, 2026-10-09.)

Where the WAP is actually configured:

| What | Where |
|------|-------|
| Hardware target + build profile, every `FEATURE_*` flag | [`build_config.h`](../../../projects/canary-wap/arduino/canary_wap/build_config.h) (the `BUILD_PROFILE_MINIMAL` / `_DEV` / `_FULL` blocks) |
| Pins (SD, GPS, camera) | [`canary_wap.ino`](../../../projects/canary-wap/arduino/canary_wap/canary_wap.ino) |
| Which profile each PlatformIO env builds | [`envs/platformio/canary-wap.ini`](../../../envs/platformio/canary-wap.ini) (`canary-wap-default`: FULL; `canary-wap-mobile`: DEV) |
| AP SSID prefix, channel, HTTP/HTTPS ports | [`wap_server.h`](../../../projects/canary-wap/arduino/canary_wap/wap_server.h) |

Why the file still exists: two Lab generators read it.
`canary-local/tools/gen_enclosures.py` lists its `FEATURE_*` lines as the
Workshop's WAP firmware flags, and `canary-local/tools/figures/gen_figures.mjs`
maps its `CONFIG_DEVICE_TYPE` to the WAP figure. Both should read
`build_config.h` (the WAP publishes `device_type` `"canary"`, not
`"canary_wap"`); once they do, this directory can be deleted.
