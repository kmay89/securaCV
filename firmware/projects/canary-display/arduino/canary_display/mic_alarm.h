// Acoustic alarm listener — the mic-bearing dash's ONLY use of its mics
// (FEATURE_MIC_ALARM, Waveshare 4.3C, docs/hardware/display_mic_variant.md).
//
// What this is: the runtime shell around the pure, host-tested mic core
// (canary/io/mic_logic.h). It captures audio frames over I2S from the
// ES7210 mic front end, reduces each ~20 ms frame to ONE RMS scalar,
// ZEROES the sample buffer immediately (the WAP's privacy-barrier design),
// and feeds the loud/quiet edge stream to the cadence detector. Only the
// two regulated alarm grammars come out: smoke T3 -> "acoustic_smoke_alarm",
// CO T4 -> "acoustic_co_alarm" — UNSIGNED local events (a display holds no
// signing key), Sev::Alert on the glass.
//
// The contract, enforced by the Gate (host-tested invariants):
//   - OFF by default; armed only from Settings -> microphone (NVS,
//     namespace "scv-mic"). A reboot keeps the household's choice.
//   - Unset pins hard-block listening: AUDIO_PIN_I2S_* ship -1 until the
//     bench fills them from the vendor schematic — until then the mics are
//     provably un-driven (no driver install ever happens).
//   - You ALWAYS know: the amber "● MIC" chip on the glass is lit exactly
//     while the capture driver is installed — the same gate action starts
//     the driver and draws the chip, and there is no second flag to lie.
//   - Hard mute: disarming UNINSTALLS the I2S driver (pins released), the
//     verifiable mute — never a software flag over a live stream.
//
// Serial grammar (MIC1, PG1-style): HELLO on begin (state + pins), EVT on
// arm/disarm/start/stop/detection, SNAP 1 Hz with the live RMS while
// running — so "is it on?" is answerable from the console too.

#ifndef CANARY_IO_MIC_ALARM_H
#define CANARY_IO_MIC_ALARM_H

#include <stdint.h>

namespace canary {
namespace io {

// All no-ops unless FEATURE_MIC_ALARM && HAS_MICROPHONE (the TU is empty).
void mic_begin(const char* self_id, bool display_ok);
void mic_loop(uint32_t now_ms);

void mic_set_armed(bool armed);  // Settings toggle; persists to NVS
bool mic_armed();
bool mic_listening();            // == driver installed == chip lit
bool mic_pins_ok();              // false until AUDIO_PIN_I2S_* leave -1
uint16_t mic_level();            // last frame RMS (debug page)

// Sensitivity preset (quiet / standard / noisy — see mic_logic.h). Sets the
// noise-floor-relative thresholds for the room; persists to NVS. Detection
// cadence is standards-fixed and not affected. Index clamps to a valid
// preset; the name is for the Settings row and the debug page.
void mic_set_sensitivity(uint8_t index);
uint8_t mic_sensitivity();        // current preset index
const char* mic_sensitivity_name();

}  // namespace io
}  // namespace canary

#endif  // CANARY_IO_MIC_ALARM_H
