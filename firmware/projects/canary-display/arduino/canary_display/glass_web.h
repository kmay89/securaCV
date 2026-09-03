// include/canary/net/glass_web.h — the display, mirrored to your phone.
//
// A tiny steady-state web server on the display itself: one self-contained
// page (no internet, no external assets — everything ships in flash) that
// shows a LIVE emulation of the glass, an interactive 3D model of the
// device you can spin with a finger, a help guide, and the master
// settings. The WAP's fleet page already links every display's hostname —
// this module is what answers.
//
// Posture: LAN-only, same trust stance as the sensor dashboard. The
// settings it exposes are the glass's own comfort controls (brightness,
// quiet hours, palette); witness data stays read-only, and the two keys
// that would open a network path or store a location (wx_direct, wx_loc)
// are refused from the network entirely — on-glass only, see
// net/settings_policy.h. Nothing here ever leaves the home network.
#pragma once
#include <stdint.h>
#include "fleet_instance.h"
#include "canary_mark.h"

namespace canary::net {

// Start the server (call once, after provisioning is settled). Registers
// the _http._tcp mDNS service so phones and the WAP fleet page find it.
void glass_web_init();

// Pump HTTP (loop task). Cheap when idle.
void glass_web_tick(uint32_t now_ms);

// Refresh the live snapshot the mirror serves. Called from render() with
// the same state the glass itself just drew — the mirror can never
// disagree with the wall.
void glass_web_publish(const canary::fleet::Fleet& fleet, uint32_t now_ms,
                       bool night, bool time_valid, int clock_hh,
                       int clock_mm, bool wifi_ok, bool mqtt_ok,
                       canary::ui::CanaryMood bird);

}  // namespace canary::net
