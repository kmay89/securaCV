/*
 * SecuraCV Canary — Bluetooth Channel Implementation
 * Version 0.1.0
 *
 * BLE (Bluetooth Low Energy) implementation using ESP32 NimBLE stack.
 * Provides secure local connectivity for mobile app integration.
 *
 * NOTE: Requires NimBLE-Arduino library. If not available, this entire
 * file compiles to empty (guarded by FEATURE_BLUETOOTH).
 */

// Feature flag - disable Bluetooth if NimBLE library is not installed
// To enable: install NimBLE-Arduino from Arduino Library Manager and set to 1
#ifndef FEATURE_BLUETOOTH
#define FEATURE_BLUETOOTH 0
#endif

#if FEATURE_BLUETOOTH

#include "bluetooth_channel.h"
#include "nvs_store.h"
#include "health_log.h"

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#include <NimBLEAdvertising.h>
#include <NimBLEScan.h>

namespace bluetooth_channel {

// ════════════════════════════════════════════════════════════════════════════
// INTERNAL STATE
// ════════════════════════════════════════════════════════════════════════════

static BluetoothState g_state = BT_DISABLED;
static bool g_initialized = false;

// BLE components
static NimBLEServer* g_server = nullptr;
static NimBLEService* g_service = nullptr;
static NimBLECharacteristic* g_status_char = nullptr;
static NimBLECharacteristic* g_command_char = nullptr;
static NimBLECharacteristic* g_notify_char = nullptr;
static NimBLEAdvertising* g_advertising = nullptr;
static NimBLEScan* g_scanner = nullptr;

// Settings (persisted to NVS)
static BluetoothSettings g_settings = {
  .enabled = false,
  .auto_advertise = true,
  .allow_pairing = true,
  .require_pin = true,
  .device_name = "SecuraCV-Canary",
  .tx_power = 3,
  .inactivity_timeout_ms = INACTIVITY_TIMEOUT_MS,
  .notify_on_connect = true
};

// Connection state
static ConnectionInfo g_connection = {0};
static PairingSession g_pairing = {0};

// Paired devices
static PairedDevice g_paired_devices[MAX_PAIRED_DEVICES];
static size_t g_paired_count = 0;

// Scan results
static ScannedDevice g_scanned_devices[MAX_SCANNED_DEVICES];
static size_t g_scanned_count = 0;
static bool g_scanning = false;
static uint32_t g_scan_start_ms = 0;
static uint32_t g_scan_duration_ms = 0;

// Statistics
static uint32_t g_total_connections = 0;
static uint32_t g_total_bytes_sent = 0;
static uint32_t g_total_bytes_received = 0;
static uint32_t g_advertising_start_ms = 0;
static uint32_t g_advertising_total_ms = 0;
static uint32_t g_connected_total_ms = 0;

// Callbacks
static ConnectionCallback g_conn_callback = nullptr;
static PairingCallback g_pair_callback = nullptr;
static ScanCallback g_scan_callback = nullptr;
static DataCallback g_data_callback = nullptr;

// NVS keys
static const char* NVS_KEY_BT_ENABLED = "bt_enabled";
static const char* NVS_KEY_BT_AUTO_ADV = "bt_auto_adv";
static const char* NVS_KEY_BT_ALLOW_PAIR = "bt_allow_pair";
static const char* NVS_KEY_BT_REQ_PIN = "bt_req_pin";
static const char* NVS_KEY_BT_NAME = "bt_name";
static const char* NVS_KEY_BT_TX_PWR = "bt_tx_pwr";
static const char* NVS_KEY_BT_TIMEOUT = "bt_timeout";
static const char* NVS_KEY_BT_PAIRED = "bt_paired";

// ════════════════════════════════════════════════════════════════════════════
// FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════════════════════

static void set_state(BluetoothState new_state);
static void load_settings();
static void save_settings();
static void load_paired_devices();
static void save_paired_devices();
static void update_status_characteristic();
static void handle_inactivity_timeout();
static void handle_scan_timeout();
static DeviceType detect_device_type(NimBLEAdvertisedDevice* device);

// ════════════════════════════════════════════════════════════════════════════
// BLE CALLBACKS
// ════════════════════════════════════════════════════════════════════════════

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    g_connection.connected = true;
    memcpy(g_connection.address, connInfo.getAddress().getNative(), BLE_ADDRESS_LENGTH);

    NimBLEAddress addr(connInfo.getAddress());
    strncpy(g_connection.name, addr.toString().c_str(), MAX_DEVICE_NAME_LEN);
    g_connection.name[MAX_DEVICE_NAME_LEN] = '\0';

    g_connection.connected_since_ms = millis();
    g_connection.last_activity_ms = millis();
    g_connection.bytes_sent = 0;
    g_connection.bytes_received = 0;

    // Update security level
    if (connInfo.isEncrypted()) {
      g_connection.security = connInfo.isAuthenticated() ? SEC_AUTHENTICATED : SEC_ENCRYPTED;
    } else {
      g_connection.security = SEC_NONE;
    }

    g_total_connections++;
    set_state(BT_CONNECTED);

    if (g_settings.notify_on_connect) {
      char detail[64];
      format_address(g_connection.address, detail);
      log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "BLE device connected", detail);
    }

    if (g_conn_callback) {
      g_conn_callback(&g_connection, true);
    }

    // Stop advertising while connected
    if (g_advertising && g_advertising->isAdvertising()) {
      g_advertising->stop();
    }
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    uint32_t connected_duration = millis() - g_connection.connected_since_ms;
    g_connected_total_ms += connected_duration;

    if (g_settings.notify_on_connect) {
      char detail[80];
      snprintf(detail, sizeof(detail), "Duration: %lus, Reason: %d",
               connected_duration / 1000, reason);
      log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "BLE device disconnected", detail);
    }

    if (g_conn_callback) {
      g_conn_callback(&g_connection, false);
    }

    memset(&g_connection, 0, sizeof(g_connection));
    set_state(BT_IDLE);

    // Resume advertising if enabled
    if (g_settings.enabled && g_settings.auto_advertise) {
      start_advertising();
    }
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    if (connInfo.isAuthenticated()) {
      g_connection.security = SEC_AUTHENTICATED;
      if (connInfo.isBonded()) {
        g_connection.security = SEC_BONDED;

        // Add to paired devices
        bool found = false;
        for (size_t i = 0; i < g_paired_count; i++) {
          if (memcmp(g_paired_devices[i].address, connInfo.getAddress().getNative(), BLE_ADDRESS_LENGTH) == 0) {
            g_paired_devices[i].last_connected_ms = millis();
            g_paired_devices[i].connection_count++;
            g_paired_devices[i].security = SEC_BONDED;
            found = true;
            break;
          }
        }

        if (!found && g_paired_count < MAX_PAIRED_DEVICES) {
          PairedDevice* dev = &g_paired_devices[g_paired_count++];
          memcpy(dev->address, connInfo.getAddress().getNative(), BLE_ADDRESS_LENGTH);
          strncpy(dev->name, g_connection.name, MAX_DEVICE_NAME_LEN);
          dev->name[MAX_DEVICE_NAME_LEN] = '\0';
          dev->paired_timestamp = millis() / 1000;
          dev->last_connected_ms = millis();
          dev->connection_count = 1;
          dev->security = SEC_BONDED;
          dev->trusted = false;
          dev->blocked = false;

          save_paired_devices();
          log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "New device paired", g_connection.name);
        }
      }

      g_pairing.state = PAIR_COMPLETE;
      if (g_pair_callback) {
        g_pair_callback(&g_pairing);
      }
    } else {
      g_pairing.state = PAIR_FAILED;
      log_health(LOG_LEVEL_WARNING, LOG_CAT_BLUETOOTH, "Pairing failed", nullptr);
      if (g_pair_callback) {
        g_pair_callback(&g_pairing);
      }
    }
  }

  uint32_t onPassKeyRequest() override {
    // Generate random 6-digit PIN
    g_pairing.pin_code = esp_random() % 900000 + 100000;
    g_pairing.state = PAIR_PIN_DISPLAYED;
    g_pairing.pin_displayed = true;

    char pin_str[16];
    snprintf(pin_str, sizeof(pin_str), "%06lu", g_pairing.pin_code);
    log_health(LOG_LEVEL_NOTICE, LOG_CAT_BLUETOOTH, "Pairing PIN displayed", pin_str);

    if (g_pair_callback) {
      g_pair_callback(&g_pairing);
    }

    return g_pairing.pin_code;
  }

  bool onConfirmPIN(uint32_t pin) override {
    g_pairing.state = PAIR_CONFIRMING;
    g_pairing.pin_code = pin;

    char pin_str[16];
    snprintf(pin_str, sizeof(pin_str), "%06lu", pin);
    log_health(LOG_LEVEL_NOTICE, LOG_CAT_BLUETOOTH, "Confirm pairing PIN", pin_str);

    if (g_pair_callback) {
      g_pair_callback(&g_pairing);
    }

    // Auto-confirm for now - in production, wait for user confirmation
    return true;
  }
};

class CharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    g_connection.last_activity_ms = millis();

    std::string value = characteristic->getValue();
    g_connection.bytes_received += value.length();
    g_total_bytes_received += value.length();

    if (g_data_callback && value.length() > 0) {
      g_data_callback((const uint8_t*)value.data(), value.length());
    }
  }

  void onRead(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    g_connection.last_activity_ms = millis();
  }
};

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(NimBLEAdvertisedDevice* device) override {
    // Check if already in list
    for (size_t i = 0; i < g_scanned_count; i++) {
      if (memcmp(g_scanned_devices[i].address, device->getAddress().getNative(), BLE_ADDRESS_LENGTH) == 0) {
        // Update existing entry
        g_scanned_devices[i].rssi = device->getRSSI();
        g_scanned_devices[i].last_seen_ms = millis();
        return;
      }
    }

    // Add new device
    if (g_scanned_count < MAX_SCANNED_DEVICES) {
      ScannedDevice* entry = &g_scanned_devices[g_scanned_count++];
      memcpy(entry->address, device->getAddress().getNative(), BLE_ADDRESS_LENGTH);

      if (device->haveName()) {
        strncpy(entry->name, device->getName().c_str(), MAX_DEVICE_NAME_LEN);
        entry->name[MAX_DEVICE_NAME_LEN] = '\0';
      } else {
        entry->name[0] = '\0';
      }

      entry->rssi = device->getRSSI();
      entry->connectable = device->isConnectable();
      entry->type = detect_device_type(device);
      entry->has_securacv_service = device->isAdvertisingService(NimBLEUUID(SERVICE_UUID));
      entry->last_seen_ms = millis();

      if (g_scan_callback) {
        g_scan_callback(entry);
      }
    }
  }

  void onScanEnd(NimBLEScanResults results) override {
    g_scanning = false;
    set_state(g_connection.connected ? BT_CONNECTED : BT_IDLE);
    log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "BLE scan complete",
               String(g_scanned_count).c_str());
  }
};

static ServerCallbacks g_server_callbacks;
static CharacteristicCallbacks g_char_callbacks;
static ScanCallbacks g_scan_callbacks;

// ════════════════════════════════════════════════════════════════════════════
// STATE MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

static void set_state(BluetoothState new_state) {
  if (g_state == new_state) return;

  BluetoothState old_state = g_state;
  g_state = new_state;

  char detail[64];
  snprintf(detail, sizeof(detail), "%s -> %s",
           state_name(old_state), state_name(new_state));
  log_health(LOG_LEVEL_DEBUG, LOG_CAT_BLUETOOTH, "BLE state change", detail);
}

// ════════════════════════════════════════════════════════════════════════════
// SETTINGS PERSISTENCE
// ════════════════════════════════════════════════════════════════════════════

static void load_settings() {
  if (!nvs_open_ro()) return;

  extern Preferences g_prefs;

  g_settings.enabled = g_prefs.getBool(NVS_KEY_BT_ENABLED, false);
  g_settings.auto_advertise = g_prefs.getBool(NVS_KEY_BT_AUTO_ADV, true);
  g_settings.allow_pairing = g_prefs.getBool(NVS_KEY_BT_ALLOW_PAIR, true);
  g_settings.require_pin = g_prefs.getBool(NVS_KEY_BT_REQ_PIN, true);
  g_settings.tx_power = g_prefs.getChar(NVS_KEY_BT_TX_PWR, 3);
  g_settings.inactivity_timeout_ms = g_prefs.getULong(NVS_KEY_BT_TIMEOUT, INACTIVITY_TIMEOUT_MS);

  size_t name_len = g_prefs.getBytesLength(NVS_KEY_BT_NAME);
  if (name_len > 0 && name_len <= MAX_DEVICE_NAME_LEN) {
    g_prefs.getBytes(NVS_KEY_BT_NAME, g_settings.device_name, name_len);
    g_settings.device_name[name_len] = '\0';
  }

  nvs_close();
}

static void save_settings() {
  if (!nvs_open_rw()) return;

  extern Preferences g_prefs;

  g_prefs.putBool(NVS_KEY_BT_ENABLED, g_settings.enabled);
  g_prefs.putBool(NVS_KEY_BT_AUTO_ADV, g_settings.auto_advertise);
  g_prefs.putBool(NVS_KEY_BT_ALLOW_PAIR, g_settings.allow_pairing);
  g_prefs.putBool(NVS_KEY_BT_REQ_PIN, g_settings.require_pin);
  g_prefs.putChar(NVS_KEY_BT_TX_PWR, g_settings.tx_power);
  g_prefs.putULong(NVS_KEY_BT_TIMEOUT, g_settings.inactivity_timeout_ms);
  g_prefs.putBytes(NVS_KEY_BT_NAME, g_settings.device_name, strlen(g_settings.device_name));

  nvs_close();
}

static void load_paired_devices() {
  if (!nvs_open_ro()) return;

  extern Preferences g_prefs;

  size_t data_len = g_prefs.getBytesLength(NVS_KEY_BT_PAIRED);
  if (data_len > 0 && data_len <= sizeof(g_paired_devices)) {
    g_prefs.getBytes(NVS_KEY_BT_PAIRED, g_paired_devices, data_len);
    g_paired_count = data_len / sizeof(PairedDevice);
  }

  nvs_close();
}

static void save_paired_devices() {
  if (!nvs_open_rw()) return;

  extern Preferences g_prefs;

  g_prefs.putBytes(NVS_KEY_BT_PAIRED, g_paired_devices, g_paired_count * sizeof(PairedDevice));

  nvs_close();
}

// ════════════════════════════════════════════════════════════════════════════
// HELPER FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

static void update_status_characteristic() {
  if (!g_status_char) return;

  // Build status JSON (compact)
  char status[MAX_SERVICE_DATA_LEN];
  snprintf(status, sizeof(status), "{\"s\":%d,\"c\":%d}",
           (int)g_state, g_connection.connected ? 1 : 0);

  g_status_char->setValue((uint8_t*)status, strlen(status));

  if (g_connection.connected) {
    g_status_char->notify();
    g_connection.bytes_sent += strlen(status);
    g_total_bytes_sent += strlen(status);
  }
}

static void handle_inactivity_timeout() {
  if (!g_connection.connected) return;
  if (g_settings.inactivity_timeout_ms == 0) return;

  uint32_t inactive_ms = millis() - g_connection.last_activity_ms;
  if (inactive_ms >= g_settings.inactivity_timeout_ms) {
    log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "Disconnecting due to inactivity", nullptr);
    disconnect();
  }
}

static void handle_scan_timeout() {
  if (!g_scanning) return;

  if (g_scan_duration_ms > 0 && millis() - g_scan_start_ms >= g_scan_duration_ms) {
    stop_scan();
  }
}

static DeviceType detect_device_type(NimBLEAdvertisedDevice* device) {
  // Check for SecuraCV service first
  if (device->isAdvertisingService(NimBLEUUID(SERVICE_UUID))) {
    return DEV_SECURACV;
  }

  // Try to detect by appearance or name patterns
  if (device->haveAppearance()) {
    uint16_t appearance = device->getAppearance();
    if (appearance >= 0x0040 && appearance <= 0x007F) return DEV_PHONE;
    if (appearance >= 0x0080 && appearance <= 0x00BF) return DEV_COMPUTER;
    if (appearance >= 0x00C0 && appearance <= 0x00FF) return DEV_WEARABLE;
  }

  if (device->haveName()) {
    String name = device->getName().c_str();
    name.toLowerCase();
    if (name.indexOf("iphone") >= 0 || name.indexOf("android") >= 0 ||
        name.indexOf("pixel") >= 0 || name.indexOf("samsung") >= 0 ||
        name.indexOf("galaxy") >= 0) {
      return DEV_PHONE;
    }
    if (name.indexOf("ipad") >= 0 || name.indexOf("tablet") >= 0) {
      return DEV_TABLET;
    }
    if (name.indexOf("macbook") >= 0 || name.indexOf("laptop") >= 0 ||
        name.indexOf("desktop") >= 0) {
      return DEV_COMPUTER;
    }
    if (name.indexOf("watch") >= 0 || name.indexOf("band") >= 0 ||
        name.indexOf("fitbit") >= 0) {
      return DEV_WEARABLE;
    }
  }

  return DEV_UNKNOWN;
}

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API IMPLEMENTATION
// ════════════════════════════════════════════════════════════════════════════

bool init() {
  if (g_initialized) return true;

  set_state(BT_INITIALIZING);
  log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "Initializing BLE", nullptr);

  // Load settings
  load_settings();
  load_paired_devices();

  // Initialize NimBLE
  NimBLEDevice::init(g_settings.device_name);
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);  // +3 dBm

  // Set security
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, MITM, SC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO);

  // Create server
  g_server = NimBLEDevice::createServer();
  g_server->setCallbacks(&g_server_callbacks);

  // Create service
  g_service = g_server->createService(SERVICE_UUID);

  // Create characteristics
  g_status_char = g_service->createCharacteristic(
    STATUS_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  g_status_char->setCallbacks(&g_char_callbacks);

  g_command_char = g_service->createCharacteristic(
    COMMAND_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  g_command_char->setCallbacks(&g_char_callbacks);

  g_notify_char = g_service->createCharacteristic(
    NOTIFY_CHAR_UUID,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::INDICATE
  );

  // Start service
  g_service->start();

  // Set up advertising
  g_advertising = NimBLEDevice::getAdvertising();
  g_advertising->addServiceUUID(SERVICE_UUID);
  g_advertising->setScanResponse(true);
  g_advertising->setMinPreferred(0x06);

  // Set up scanner
  g_scanner = NimBLEDevice::getScan();
  g_scanner->setScanCallbacks(&g_scan_callbacks);
  g_scanner->setActiveScan(true);
  g_scanner->setInterval(SCAN_INTERVAL_MS);
  g_scanner->setWindow(SCAN_WINDOW_MS);

  g_initialized = true;
  set_state(BT_IDLE);

  log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "BLE initialized", g_settings.device_name);

  // Auto-start advertising if enabled
  if (g_settings.enabled && g_settings.auto_advertise) {
    enable();
    start_advertising();
  }

  return true;
}

void deinit() {
  if (!g_initialized) return;

  stop_advertising();
  stop_scan();
  disconnect();

  NimBLEDevice::deinit(true);

  g_server = nullptr;
  g_service = nullptr;
  g_status_char = nullptr;
  g_command_char = nullptr;
  g_notify_char = nullptr;
  g_advertising = nullptr;
  g_scanner = nullptr;

  g_initialized = false;
  set_state(BT_DISABLED);

  log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "BLE deinitialized", nullptr);
}

bool is_initialized() {
  return g_initialized;
}

bool enable() {
  if (!g_initialized) {
    if (!init()) return false;
  }

  g_settings.enabled = true;
  save_settings();

  if (g_state == BT_DISABLED) {
    set_state(BT_IDLE);
  }

  log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "BLE enabled", nullptr);
  return true;
}

void disable() {
  stop_advertising();
  stop_scan();
  disconnect();

  g_settings.enabled = false;
  save_settings();
  set_state(BT_DISABLED);

  log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "BLE disabled", nullptr);
}

bool is_enabled() {
  return g_settings.enabled;
}

bool start_advertising() {
  if (!g_initialized || !g_settings.enabled) return false;
  if (g_connection.connected) return false;  // Can't advertise while connected

  if (g_advertising && !g_advertising->isAdvertising()) {
    g_advertising->start();
    g_advertising_start_ms = millis();
    set_state(BT_ADVERTISING);
    log_health(LOG_LEVEL_DEBUG, LOG_CAT_BLUETOOTH, "BLE advertising started", nullptr);
  }

  return true;
}

void stop_advertising() {
  if (g_advertising && g_advertising->isAdvertising()) {
    g_advertising->stop();
    g_advertising_total_ms += millis() - g_advertising_start_ms;
    if (g_state == BT_ADVERTISING) {
      set_state(BT_IDLE);
    }
    log_health(LOG_LEVEL_DEBUG, LOG_CAT_BLUETOOTH, "BLE advertising stopped", nullptr);
  }
}

bool is_advertising() {
  return g_advertising && g_advertising->isAdvertising();
}

bool start_scan(uint32_t duration_ms) {
  if (!g_initialized || !g_settings.enabled) return false;
  if (g_scanning) return false;

  // Clear previous results
  clear_scan_results();

  g_scan_start_ms = millis();
  g_scan_duration_ms = duration_ms;
  g_scanning = true;
  set_state(BT_SCANNING);

  // Start scan (non-blocking with callback)
  g_scanner->start(duration_ms / 1000, false);

  log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "BLE scan started",
             String(duration_ms / 1000).c_str());
  return true;
}

void stop_scan() {
  if (g_scanner && g_scanning) {
    g_scanner->stop();
    g_scanning = false;
    if (g_state == BT_SCANNING) {
      set_state(g_connection.connected ? BT_CONNECTED : BT_IDLE);
    }
    log_health(LOG_LEVEL_DEBUG, LOG_CAT_BLUETOOTH, "BLE scan stopped", nullptr);
  }
}

bool is_scanning() {
  return g_scanning;
}

const ScannedDevice* get_scanned_devices(size_t* count) {
  if (count) *count = g_scanned_count;
  return g_scanned_devices;
}

void clear_scan_results() {
  memset(g_scanned_devices, 0, sizeof(g_scanned_devices));
  g_scanned_count = 0;
}

bool start_pairing() {
  if (!g_initialized || !g_settings.enabled) return false;
  if (!g_settings.allow_pairing) return false;

  g_pairing.state = PAIR_INITIATED;
  g_pairing.started_ms = millis();
  g_pairing.pin_displayed = false;
  g_pairing.user_confirmed = false;

  set_state(BT_PAIRING);

  // Make sure we're advertising
  start_advertising();

  log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "Pairing mode started", nullptr);

  if (g_pair_callback) {
    g_pair_callback(&g_pairing);
  }

  return true;
}

void cancel_pairing() {
  if (g_pairing.state == PAIR_NONE) return;

  g_pairing.state = PAIR_NONE;
  memset(&g_pairing, 0, sizeof(g_pairing));

  if (g_state == BT_PAIRING) {
    set_state(g_connection.connected ? BT_CONNECTED : BT_IDLE);
  }

  log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "Pairing cancelled", nullptr);
}

bool confirm_pairing(uint32_t pin) {
  if (g_pairing.state != PAIR_CONFIRMING) return false;

  if (pin == g_pairing.pin_code) {
    g_pairing.user_confirmed = true;
    return true;
  }

  return false;
}

bool reject_pairing() {
  if (g_pairing.state == PAIR_NONE) return false;

  g_pairing.state = PAIR_FAILED;
  log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "Pairing rejected", nullptr);

  if (g_pair_callback) {
    g_pair_callback(&g_pairing);
  }

  cancel_pairing();
  return true;
}

PairingState get_pairing_state() {
  return g_pairing.state;
}

uint32_t get_pairing_pin() {
  return g_pairing.pin_code;
}

bool disconnect() {
  if (!g_connection.connected) return false;

  if (g_server) {
    // Disconnect all clients
    for (auto& client : g_server->getPeerDevices()) {
      g_server->disconnect(client);
    }
  }

  return true;
}

bool is_connected() {
  return g_connection.connected;
}

const ConnectionInfo* get_connection_info() {
  return &g_connection;
}

const PairedDevice* get_paired_devices(size_t* count) {
  if (count) *count = g_paired_count;
  return g_paired_devices;
}

bool remove_paired_device(const uint8_t* address) {
  for (size_t i = 0; i < g_paired_count; i++) {
    if (memcmp(g_paired_devices[i].address, address, BLE_ADDRESS_LENGTH) == 0) {
      // Shift remaining devices
      for (size_t j = i; j < g_paired_count - 1; j++) {
        g_paired_devices[j] = g_paired_devices[j + 1];
      }
      g_paired_count--;
      memset(&g_paired_devices[g_paired_count], 0, sizeof(PairedDevice));

      // Remove from NimBLE bond storage
      NimBLEDevice::deleteBond(NimBLEAddress(address));

      save_paired_devices();
      log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "Paired device removed", nullptr);
      return true;
    }
  }
  return false;
}

bool clear_all_paired_devices() {
  // Clear local storage
  memset(g_paired_devices, 0, sizeof(g_paired_devices));
  g_paired_count = 0;

  // Clear NimBLE bond storage
  int bond_count = NimBLEDevice::getNumBonds();
  for (int i = bond_count - 1; i >= 0; i--) {
    NimBLEDevice::deleteBond(NimBLEDevice::getBondedAddress(i));
  }

  save_paired_devices();
  log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "All paired devices cleared", nullptr);
  return true;
}

bool set_device_trusted(const uint8_t* address, bool trusted) {
  for (size_t i = 0; i < g_paired_count; i++) {
    if (memcmp(g_paired_devices[i].address, address, BLE_ADDRESS_LENGTH) == 0) {
      g_paired_devices[i].trusted = trusted;
      save_paired_devices();
      return true;
    }
  }
  return false;
}

bool set_device_blocked(const uint8_t* address, bool blocked) {
  for (size_t i = 0; i < g_paired_count; i++) {
    if (memcmp(g_paired_devices[i].address, address, BLE_ADDRESS_LENGTH) == 0) {
      g_paired_devices[i].blocked = blocked;
      save_paired_devices();
      return true;
    }
  }
  return false;
}

BluetoothSettings get_settings() {
  return g_settings;
}

bool set_settings(const BluetoothSettings& settings) {
  g_settings = settings;
  save_settings();

  // Apply changes
  if (g_settings.enabled && !is_enabled()) {
    enable();
  } else if (!g_settings.enabled && is_enabled()) {
    disable();
  }

  // Update device name if changed
  if (g_initialized) {
    // NimBLE doesn't support changing name after init without reinit
  }

  return true;
}

bool set_device_name(const char* name) {
  if (!name || strlen(name) == 0 || strlen(name) > MAX_DEVICE_NAME_LEN) {
    return false;
  }

  strncpy(g_settings.device_name, name, MAX_DEVICE_NAME_LEN);
  g_settings.device_name[MAX_DEVICE_NAME_LEN] = '\0';
  save_settings();

  log_health(LOG_LEVEL_INFO, LOG_CAT_BLUETOOTH, "Device name changed", name);
  return true;
}

bool set_tx_power(int8_t power) {
  if (power < -12 || power > 9) return false;

  g_settings.tx_power = power;
  save_settings();

  if (g_initialized) {
    // Map to ESP power levels
    esp_power_level_t level = ESP_PWR_LVL_P3;
    if (power <= -12) level = ESP_PWR_LVL_N12;
    else if (power <= -9) level = ESP_PWR_LVL_N9;
    else if (power <= -6) level = ESP_PWR_LVL_N6;
    else if (power <= -3) level = ESP_PWR_LVL_N3;
    else if (power <= 0) level = ESP_PWR_LVL_N0;
    else if (power <= 3) level = ESP_PWR_LVL_P3;
    else if (power <= 6) level = ESP_PWR_LVL_P6;
    else level = ESP_PWR_LVL_P9;

    NimBLEDevice::setPower(level);
  }

  return true;
}

BluetoothStatus get_status() {
  BluetoothStatus status = {0};

  status.state = g_state;
  status.enabled = g_settings.enabled;
  status.advertising = is_advertising();
  status.scanning = g_scanning;
  status.connected = g_connection.connected;

  strncpy(status.device_name, g_settings.device_name, MAX_DEVICE_NAME_LEN);
  status.device_name[MAX_DEVICE_NAME_LEN] = '\0';

  // Get local address
  if (g_initialized) {
    NimBLEAddress addr = NimBLEDevice::getAddress();
    strncpy(status.local_address, addr.toString().c_str(), 17);
    status.local_address[17] = '\0';
  }

  status.tx_power = g_settings.tx_power;
  status.paired_count = g_paired_count;
  status.scanned_count = g_scanned_count;
  status.connection = g_connection;
  status.pairing = g_pairing;

  status.total_connections = g_total_connections;
  status.total_bytes_sent = g_total_bytes_sent;
  status.total_bytes_received = g_total_bytes_received;
  status.advertising_time_ms = g_advertising_total_ms;
  if (is_advertising()) {
    status.advertising_time_ms += millis() - g_advertising_start_ms;
  }
  status.connected_time_ms = g_connected_total_ms;
  if (g_connection.connected) {
    status.connected_time_ms += millis() - g_connection.connected_since_ms;
  }

  return status;
}

BluetoothState get_state() {
  return g_state;
}

const char* state_name(BluetoothState state) {
  switch (state) {
    case BT_DISABLED:      return "disabled";
    case BT_INITIALIZING:  return "initializing";
    case BT_IDLE:          return "idle";
    case BT_ADVERTISING:   return "advertising";
    case BT_SCANNING:      return "scanning";
    case BT_PAIRING:       return "pairing";
    case BT_CONNECTED:     return "connected";
    case BT_ERROR:         return "error";
    default:               return "unknown";
  }
}

void set_connection_callback(ConnectionCallback cb) {
  g_conn_callback = cb;
}

void set_pairing_callback(PairingCallback cb) {
  g_pair_callback = cb;
}

void set_scan_callback(ScanCallback cb) {
  g_scan_callback = cb;
}

void set_data_callback(DataCallback cb) {
  g_data_callback = cb;
}

void update() {
  if (!g_initialized || !g_settings.enabled) return;

  static uint32_t last_status_update = 0;
  uint32_t now = millis();

  // Update status characteristic periodically
  if (g_connection.connected && now - last_status_update >= STATUS_UPDATE_INTERVAL_MS) {
    last_status_update = now;
    update_status_characteristic();

    // Update RSSI
    // Note: NimBLE doesn't provide direct RSSI access for connections
  }

  // Check inactivity timeout
  handle_inactivity_timeout();

  // Check scan timeout
  handle_scan_timeout();

  // Check pairing timeout
  if (g_pairing.state != PAIR_NONE && g_pairing.state != PAIR_COMPLETE) {
    if (now - g_pairing.started_ms >= PAIRING_TIMEOUT_MS) {
      log_health(LOG_LEVEL_WARNING, LOG_CAT_BLUETOOTH, "Pairing timeout", nullptr);
      cancel_pairing();
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// UTILITIES
// ════════════════════════════════════════════════════════════════════════════

void format_address(const uint8_t* addr, char* out) {
  snprintf(out, BLE_ADDRESS_STR_LEN, "%02X:%02X:%02X:%02X:%02X:%02X",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

bool parse_address(const char* str, uint8_t* out) {
  if (strlen(str) != 17) return false;

  unsigned int bytes[6];
  if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
             &bytes[0], &bytes[1], &bytes[2],
             &bytes[3], &bytes[4], &bytes[5]) != 6) {
    return false;
  }

  for (int i = 0; i < 6; i++) {
    out[i] = (uint8_t)bytes[i];
  }
  return true;
}

const char* device_type_name(DeviceType type) {
  switch (type) {
    case DEV_UNKNOWN:   return "unknown";
    case DEV_PHONE:     return "phone";
    case DEV_TABLET:    return "tablet";
    case DEV_COMPUTER:  return "computer";
    case DEV_WEARABLE:  return "wearable";
    case DEV_SECURACV:  return "securacv";
    case DEV_OTHER:     return "other";
    default:            return "unknown";
  }
}

const char* security_level_name(SecurityLevel level) {
  switch (level) {
    case SEC_NONE:          return "none";
    case SEC_ENCRYPTED:     return "encrypted";
    case SEC_AUTHENTICATED: return "authenticated";
    case SEC_BONDED:        return "bonded";
    default:                return "unknown";
  }
}

const char* pairing_state_name(PairingState state) {
  switch (state) {
    case PAIR_NONE:          return "none";
    case PAIR_INITIATED:     return "initiated";
    case PAIR_PIN_DISPLAYED: return "pin_displayed";
    case PAIR_CONFIRMING:    return "confirming";
    case PAIR_COMPLETE:      return "complete";
    case PAIR_FAILED:        return "failed";
    default:                 return "unknown";
  }
}

} // namespace bluetooth_channel

#endif // FEATURE_BLUETOOTH
