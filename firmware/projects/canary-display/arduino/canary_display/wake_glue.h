// include/canary/care/wake_glue.h — wake-alarm device glue (nightstand
// wave). The pure state machine lives in wake_alarm.h; this wires it to
// NVS (survives reboots — an alarm that can be lost is no alarm), the MQTT
// config topic, the chime, and the backlight sunrise.
#pragma once
#include <stdint.h>

namespace canary::care {

void wake_alarm_init();                                   // NVS restore
void wake_alarm_on_config(const char* payload, unsigned len);
void wake_alarm_loop(uint32_t now_ms);                    // chirps by phase
// Backlight override during the sunrise ramp: -1 = none, else a
// gamma-corrected 0..255 level (perceptually linear dawn).
int  wake_alarm_backlight();
// True when a tap was consumed to dismiss a live alarm.
bool wake_alarm_tap();

}  // namespace canary::care
