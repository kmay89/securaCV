/*
 * SecuraCV Canary — USB evidence drive (firmware glue).
 *
 * Presents the Canary over USB mass storage in one of two explicit modes
 * (docs/design/usb_evidence_drive.md):
 *
 *   EVIDENCE — the SD card's sectors, READ-ONLY at the USB protocol level.
 *              Plug into any computer (or an iPhone's Files app) and the
 *              witness logs are just files. The share state machine
 *              (evidence_drive_logic) guarantees the SD has one writer.
 *   UPDATE   — a 4 MB PSRAM-backed FAT16 drop-zone ("CANARY-UPDATE").
 *              Drop a release .bin + its manifest-*.json, eject, and the
 *              pair runs the exact network-OTA verification
 *              (evidence_update_verify) before installing on reboot.
 *
 * Requires FEATURE_USB_EVIDENCE_DRIVE=1 AND a TinyUSB (USB-OTG) build
 * (ARDUINO_USB_MODE=0 — note the stock WAP profile builds hwcdc, so this
 * ships as a separate opt-in build). On every other build this header's
 * functions exist but report "not in this build".
 *
 * PHASE 2 (hardware validation) PENDING: this glue compiles only in the
 * flagged build and has not yet enumerated on real hosts. Do not enable in
 * a release profile before the checklist in the design doc runs on-device.
 */

#ifndef SECURACV_USB_EVIDENCE_DRIVE_H
#define SECURACV_USB_EVIDENCE_DRIVE_H

#include <stdint.h>

namespace usb_evidence_drive {

enum class Mode : uint8_t { OFF = 0, EVIDENCE = 1, UPDATE = 2 };

struct Config {
  const char* product;          // OTA_PRODUCT
  const char* running_version;  // FIRMWARE_VERSION
  // SD quiesce hooks, provided by the sketch (flush + close all SD files /
  // reopen). Called around EVIDENCE mode transitions.
  bool (*sd_quiesce)();
  void (*sd_resume)();
};

// One-time init (allocates the PSRAM staging volume, registers USB).
// Returns false when the build/hardware can't do MSC — callers just relay
// the status line.
bool begin(const Config& cfg);

// Main-loop pump: applies deferred USB events (eject → update scan/verify/
// install; unplug → share release) so heavy work never runs in USB task
// context. Cheap when idle.
void poll();

// Cycle OFF → EVIDENCE → UPDATE → OFF (the serial console's 'u' key).
void cycle_mode();

Mode mode();
const char* status_line(); // one line for the console/status bar

} // namespace usb_evidence_drive

#endif // SECURACV_USB_EVIDENCE_DRIVE_H
