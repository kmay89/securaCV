/*
 * SecuraCV Canary — BLE Standard SIG Profiles
 *
 * Registers the SIG-assigned services that turn a custom-only BLE
 * peripheral into something iOS / Android / desktop OSes describe
 * properly. Without this, the device shows up as an opaque "Unknown
 * Device" with no metadata. With it, the system Bluetooth pane shows
 * manufacturer + model + firmware + serial and renders a camera icon
 * next to the name — the polish that distinguishes a hardware device
 * from a generic GATT server.
 *
 *   Service                 UUID    Characteristics
 *   ────────────────────── ────── ────────────────────────────────────
 *   Device Information     0x180A  Manufacturer (0x2A29), Model (0x2A24),
 *                                  Serial (0x2A25), Firmware Rev (0x2A26),
 *                                  Hardware Rev (0x2A27), Software Rev
 *                                  (0x2A28)
 *   Battery                0x180F  Battery Level (0x2A19) 0–100 %
 *
 * GAP Appearance is set to 0x0541 (Digital Still Camera). iOS uses
 * this to pick the icon shown next to our device name in the Bluetooth
 * settings pane. Other values from the Core Spec assigned-numbers
 * document can be substituted; 0x0541 is a reasonable default for a
 * sensor-with-camera.
 *
 * Header-only because the characteristics carry static text — no state
 * to keep, no callbacks to dispatch. The battery-level helper is the
 * one exception (small mutable state behind a setter).
 */

#ifndef SECURACV_BLE_STANDARD_PROFILES_H
#define SECURACV_BLE_STANDARD_PROFILES_H

#include "build_config.h"

#if FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)

#include <NimBLEDevice.h>
#include <NimBLEServer.h>

namespace ble_standard_profiles {

// SIG-assigned 16-bit UUIDs. NimBLEUUID accepts the 16-bit form and
// expands to the Bluetooth Base UUID internally.
static constexpr uint16_t SVC_DEVICE_INFORMATION = 0x180A;
static constexpr uint16_t SVC_BATTERY            = 0x180F;

static constexpr uint16_t CHR_MANUFACTURER_NAME  = 0x2A29;
static constexpr uint16_t CHR_MODEL_NUMBER       = 0x2A24;
static constexpr uint16_t CHR_SERIAL_NUMBER      = 0x2A25;
static constexpr uint16_t CHR_FIRMWARE_REVISION  = 0x2A26;
static constexpr uint16_t CHR_HARDWARE_REVISION  = 0x2A27;
static constexpr uint16_t CHR_SOFTWARE_REVISION  = 0x2A28;
static constexpr uint16_t CHR_BATTERY_LEVEL      = 0x2A19;

// Digital Still Camera. Renders a camera icon in the iOS Bluetooth
// pane. Swap for 0x0080 (Generic Computer) or 0x0540 (Generic Camera)
// if a specific host doesn't recognise the subtype.
static constexpr uint16_t GAP_APPEARANCE_CAMERA  = 0x0541;

// Battery-level characteristic — module-internal so set_battery_level
// can update it after init. nullptr until register_battery runs.
static NimBLECharacteristic* g_battery_char = nullptr;
static uint8_t               g_battery_level_pct = 100;

// Build a single SIG service that exposes the static device-info strings.
// Each characteristic is read-only, no security flags — DIS is meant to
// be discoverable pre-pairing so the peer's stack can render the device
// before the user taps Pair.
inline void register_dis(NimBLEServer* server,
                         const char* manufacturer,
                         const char* model,
                         const char* serial,
                         const char* fw_revision,
                         const char* hw_revision,
                         const char* sw_revision) {
  NimBLEService* dis = server->createService(NimBLEUUID((uint16_t)SVC_DEVICE_INFORMATION));
  if (!dis) return;
  // Characteristic order doesn't matter to clients — they discover by UUID.
  // Each setValue carries a single static UTF-8 string; NimBLE caches it.
  auto add = [&](uint16_t uuid, const char* val) {
    NimBLECharacteristic* c = dis->createCharacteristic(NimBLEUUID(uuid),
                                                         NIMBLE_PROPERTY::READ);
    if (c && val) c->setValue(val);
  };
  add(CHR_MANUFACTURER_NAME, manufacturer);
  add(CHR_MODEL_NUMBER,      model);
  add(CHR_SERIAL_NUMBER,     serial);
  add(CHR_FIRMWARE_REVISION, fw_revision);
  add(CHR_HARDWARE_REVISION, hw_revision);
  add(CHR_SOFTWARE_REVISION, sw_revision);
  dis->start();
}

inline void register_battery(NimBLEServer* server, uint8_t initial_pct) {
  NimBLEService* bas = server->createService(NimBLEUUID((uint16_t)SVC_BATTERY));
  if (!bas) return;
  g_battery_level_pct = initial_pct > 100 ? 100 : initial_pct;
  g_battery_char = bas->createCharacteristic(
    NimBLEUUID((uint16_t)CHR_BATTERY_LEVEL),
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  if (g_battery_char) {
    g_battery_char->setValue(&g_battery_level_pct, 1);
  }
  bas->start();
}

// Update + notify subscribed clients. No-op until register_battery has run.
inline void set_battery_level(uint8_t pct) {
  if (pct > 100) pct = 100;
  g_battery_level_pct = pct;
  if (g_battery_char) {
    g_battery_char->setValue(&g_battery_level_pct, 1);
    g_battery_char->notify();
  }
}

inline uint8_t get_battery_level() { return g_battery_level_pct; }

// Single entry point — call after the main GATT service has been started
// so this lives next to it on the same NimBLE server.
inline void register_all(NimBLEServer* server,
                         const char* manufacturer,
                         const char* model,
                         const char* serial,
                         const char* fw_revision,
                         const char* hw_revision,
                         const char* sw_revision,
                         uint16_t appearance = GAP_APPEARANCE_CAMERA,
                         uint8_t initial_battery_pct = 100) {
  if (!server) return;
  // GAP appearance is a global property on NimBLEDevice (advertised in
  // the GAP service that NimBLE auto-creates), not a service of its own.
  NimBLEDevice::setAppearance(appearance);
  register_dis(server, manufacturer, model, serial,
               fw_revision, hw_revision, sw_revision);
  register_battery(server, initial_battery_pct);
}

}  // namespace ble_standard_profiles

#endif  // FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)

#endif  // SECURACV_BLE_STANDARD_PROFILES_H
