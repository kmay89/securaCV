# ESP32-S3 Power Resilience (Brownout) — Canary

Companion to [`esp32s3_ble_wap_audit.md`](./esp32s3_ble_wap_audit.md), finding **F7**.
For supply/cable sizing, battery selection, consumption estimates, and the
battery-health telemetry reference, see the full
[power & battery guide](./hardware/esp32s3_power_battery_guide.md).

## Why this matters

The Canary WAP runs several radio roles on one 2.4 GHz front-end (WiFi AP/STA,
BLE advertise/scan, ESP-NOW mesh) plus, on the Sense board, a camera. When WiFi
and BLE transmit in the same window the instantaneous draw spikes to roughly
**400–700 mA**. If the 3.3 V rail can't service that spike, the SoC's brownout
detector resets the chip.

The failure is deceptive: a brownout reset looks **exactly** like a firmware
fault. Symptoms you'll see in the field:

- "BLE init failed" / "WiFi AP failed to start" in the boot log — because the
  reset interrupts bring-up part-way.
- A **boot loop** that trips `safe_mode_check()` (3 reboots in 60 s), which then
  disables the optional peripherals — so the device looks "broken" and BLE/WiFi
  are off, masking the real cause (power).

## What the firmware does

At boot, right after the safe-mode check, `setup()` logs the cause of the last
reset via `esp_reset_reason()`
(`firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino`):

| Reset reason | Logged as | Level |
|---|---|---|
| `ESP_RST_BROWNOUT` | "Last reset: brownout (supply voltage sag)" + 3.3 V hint | `ERROR` |
| `ESP_RST_PANIC` | "Last reset: panic (firmware crash)" | `WARNING` |
| `ESP_RST_*_WDT` | "Last reset: watchdog timeout" | `WARNING` |

So a brownout shows up as a distinct `SCV_CAT_SYSTEM` health-log entry pointing
at the power supply, not at BLE/WiFi. The full reset reason is also exposed in
sys_monitor's status JSON (`reset_reason`) for the dashboard / fleet manager.

> The modular `canary/` build (`firmware/canary/src/main.cpp`) can adopt the
> same boot-time `esp_reset_reason()` log; it currently surfaces the reason only
> via sys_monitor. Tracked as a follow-up.

## Hardware recommendations (field units)

1. **Supply:** a clean **≥ 500 mA** 3.3 V source. A flaky USB cable / weak
   port is the most common brownout cause — try a known-good cable + 5 V/1 A+
   supply before suspecting firmware.
2. **Bulk decoupling:** a **470–1000 µF** low-ESR electrolytic across 3V3/GND
   close to the module to absorb the TX spike, in parallel with a **0.1 µF**
   ceramic for high-frequency noise.
3. **Battery units:** ensure the LiPo + regulator can deliver the burst current;
   the XIAO ESP32-S3's onboard LDO (~600 mA class) is marginal under AP+STA+BLE
   +camera. Reducing concurrent radio load helps — see the F4 AP-drop behavior
   in the audit doc, which keeps the device in the lower-power STA+BLE state once
   it has joined home WiFi.
4. **TX power:** BLE TX power is configurable (`bt_tx_pwr` in NVS, default +9 dBm);
   lowering it trims peak draw at the cost of range if a unit is power-constrained.

## Verifying

- Trigger a deliberate brownout (briefly load/sag the supply, or undervolt) and
  confirm the next boot logs the `ERROR` brownout entry rather than a BLE/WiFi
  failure.
- Check the dashboard status JSON `reset_reason` field after the event.
