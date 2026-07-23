// Debug mode runtime (FEATURE_DEBUG_MODE, docs/hardware/display_modes.md
// §debug): the glass turns inside out. Tap-to-page, full-screen diagnostics
// rendered from what the firmware already knows about itself — link, memory,
// the fleet model raw, a touch crosshair, the I2C census, system facts.
//
// The one non-fleet gear with the network UP (mode_policy(Debug).network):
// the link is usually the thing being diagnosed, so WiFi comes up
// (non-blocking — a dead AP is a *finding*, never a reboot loop) and the
// broker is attempted on a fixed cadence; with it, the retained fleet
// repopulates and the Fleet page shows the model as it really is.
//
// Rails: read-mostly; no OTA; no secrets on the glass (SSID yes, password
// never, key fingerprints not keys); DBG1 serial snapshots mirror the PG1
// grammar. Every page is self-labeled and photographable — "send a photo of
// the debug screen" is the support story. Hold 3 s to exit.

#ifndef CANARY_MODE_DEBUG_MODE_H
#define CANARY_MODE_DEBUG_MODE_H

namespace canary {
namespace mode {

void debug_mode_setup();
void debug_mode_loop();

}  // namespace mode
}  // namespace canary

#endif  // CANARY_MODE_DEBUG_MODE_H
