// include/canary/ui/commission_ui.h — "add a canary": the display mints a
// provisioning QR (SCV1 grammar, firmware/common/provision_qr) carrying its
// own Wi-Fi credentials, the hub address, and a short-lived fast-track
// token. A camera canary scans the glass and joins; the moment ANY new
// witness appears while this surface is open, it celebrates and closes —
// which also blesses the phone-made WIFI: code path and the captive-portal
// path, because the celebration keys on the join, not the transport.
//
// Modal-surface contract identical to settings_ui: main.cpp routes every
// tap here while open, long-press exits, idle times out, a live unacked
// alert closes it instantly, and the backlight pins to full (a camera needs
// a bright code).
#pragma once
#include <stdint.h>

namespace canary::ui {

void commission_ui_open();
void commission_ui_close();
bool commission_ui_active();

// Tap routing (panel pixels). Only called while active.
void commission_ui_handle_tap(int16_t x, int16_t y);

// Housekeeping from loop(): countdown, auto re-mint at expiry, the joined
// celebration (fleet_count vs the count at open), idle/urgent close.
void commission_ui_tick(uint32_t now_ms, int fleet_count, bool urgent);

// The currently minted fast-track token ('' when the surface is closed).
// The scanner-side wave will match this against a joining witness's hello
// for auto-blessing; today the join celebration keys on fleet count.
const char* commission_ui_token();

}  // namespace canary::ui
