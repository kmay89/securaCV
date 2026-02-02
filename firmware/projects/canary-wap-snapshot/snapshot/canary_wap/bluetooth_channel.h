/*
 * SecuraCV Canary — Bluetooth Channel
 * Version 0.1.0
 *
 * BLE (Bluetooth Low Energy) interface for mobile app connectivity.
 * Enables secure local device management and monitoring.
 *
 * Security Properties:
 * - Secure pairing with PIN confirmation
 * - Device whitelist for trusted connections
 * - Auto-disconnect on inactivity
 * - No sensitive data over BLE (status only)
 *
 * Features:
 * - Device status broadcasting
 * - Paired device management
 * - Scan for nearby BLE devices
 * - Connection status monitoring
 * - Device name configuration
 */

#ifndef SECURACV_BLUETOOTH_CHANNEL_H
#define SECURACV_BLUETOOTH_CHANNEL_H

#include <Arduino.h>
#include "log_level.h"

namespace bluetooth_channel {

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

// BLE limits
static const size_t MAX_PAIRED_DEVICES = 8;
static const size_t MAX_SCANNED_DEVICES = 16;
static const size_t MAX_DEVICE_NAME_LEN = 32;
static const size_t MAX_SERVICE_DATA_LEN = 20;

// Timing (milliseconds)
static const uint32_t ADVERTISING_INTERVAL_MS = 500;
static const uint32_t SCAN_DURATION_MS = 10000;
static const uint32_t SCAN_INTERVAL_MS = 80;
static const uint32_t SCAN_WINDOW_MS = 40;
static const uint32_t CONNECTION_TIMEOUT_MS = 30000;
static const uint32_t INACTIVITY_TIMEOUT_MS = 300000;  // 5 minutes
static const uint32_t STATUS_UPDATE_INTERVAL_MS = 1000;
static const uint32_t PAIRING_TIMEOUT_MS = 60000;

// BLE UUIDs (SecuraCV custom service)
static const char* SERVICE_UUID = "8fc1ceca-b162-4401-9607-c8ac21383e90";
static const char* STATUS_CHAR_UUID = "8fc1cecb-b162-4401-9607-c8ac21383e90";
static const char* COMMAND_CHAR_UUID = "8fc1cecc-b162-4401-9607-c8ac21383e90";
static const char* NOTIFY_CHAR_UUID = "8fc1cecd-b162-4401-9607-c8ac21383e90";

// ════════════════════════════════════════════════════════════════════════════
// ENUMS
// ════════════════════════════════════════════════════════════════════════════

// Bluetooth state
enum BluetoothState : uint8_t {
  BT_DISABLED = 0,        // Bluetooth off
  BT_INITIALIZING,        // Starting BLE stack
  BT_IDLE,                // Ready but not advertising
  BT_ADVERTISING,         // Broadcasting presence
  BT_SCANNING,            // Scanning for devices
  BT_PAIRING,             // Pairing mode active
  BT_CONNECTED,           // Device connected
  BT_ERROR                // Fatal error
};

// Scan result type
enum DeviceType : uint8_t {
  DEV_UNKNOWN = 0,
  DEV_PHONE,
  DEV_TABLET,
  DEV_COMPUTER,
  DEV_WEARABLE,
  DEV_SECURACV,           // Another SecuraCV device
  DEV_OTHER
};

// Connection security level
enum SecurityLevel : uint8_t {
  SEC_NONE = 0,
  SEC_ENCRYPTED,
  SEC_AUTHENTICATED,
  SEC_BONDED
};

// Pairing state
enum PairingState : uint8_t {
  PAIR_NONE = 0,
  PAIR_INITIATED,
  PAIR_PIN_DISPLAYED,
  PAIR_CONFIRMING,
  PAIR_COMPLETE,
  PAIR_FAILED
};

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

// Paired device record
struct PairedDevice {
  uint8_t address[6];                       // MAC address
  char name[MAX_DEVICE_NAME_LEN + 1];       // Device name
  uint32_t paired_timestamp;                // When paired (epoch)
  uint32_t last_connected_ms;               // Last connection time
  uint32_t connection_count;                // Total connections
  SecurityLevel security;                   // Security level achieved
  bool trusted;                             // In trusted whitelist
  bool blocked;                             // Blocked device
};

// Scanned device entry
struct ScannedDevice {
  uint8_t address[6];                       // MAC address
  char name[MAX_DEVICE_NAME_LEN + 1];       // Device name (if available)
  int8_t rssi;                              // Signal strength
  DeviceType type;                          // Detected device type
  bool connectable;                         // Can connect to this device
  bool has_securacv_service;                // Is SecuraCV device
  uint32_t last_seen_ms;                    // Last seen timestamp
};

// Current connection info
struct ConnectionInfo {
  bool connected;
  uint8_t address[6];
  char name[MAX_DEVICE_NAME_LEN + 1];
  int8_t rssi;
  SecurityLevel security;
  uint32_t connected_since_ms;
  uint32_t last_activity_ms;
  uint32_t bytes_sent;
  uint32_t bytes_received;
};

// Pairing session
struct PairingSession {
  PairingState state;
  uint8_t peer_address[6];
  char peer_name[MAX_DEVICE_NAME_LEN + 1];
  uint32_t pin_code;                        // 6-digit PIN
  uint32_t started_ms;
  bool pin_displayed;
  bool user_confirmed;
};

// Bluetooth status (for API)
struct BluetoothStatus {
  BluetoothState state;
  bool enabled;
  bool advertising;
  bool scanning;
  bool connected;
  char device_name[MAX_DEVICE_NAME_LEN + 1];
  char local_address[18];                   // "XX:XX:XX:XX:XX:XX"
  int8_t tx_power;                          // dBm
  uint8_t paired_count;
  uint8_t scanned_count;
  ConnectionInfo connection;
  PairingSession pairing;

  // Statistics
  uint32_t total_connections;
  uint32_t total_bytes_sent;
  uint32_t total_bytes_received;
  uint32_t advertising_time_ms;
  uint32_t connected_time_ms;
};

// Bluetooth settings (persisted to NVS)
struct BluetoothSettings {
  bool enabled;
  bool auto_advertise;                      // Start advertising on boot
  bool allow_pairing;                       // Accept new pairings
  bool require_pin;                         // Require PIN for pairing
  char device_name[MAX_DEVICE_NAME_LEN + 1];
  int8_t tx_power;                          // Transmit power (-12 to +9 dBm)
  uint32_t inactivity_timeout_ms;           // Auto-disconnect timeout
  bool notify_on_connect;                   // Log connection events
};

// ════════════════════════════════════════════════════════════════════════════
// CALLBACKS
// ════════════════════════════════════════════════════════════════════════════

// Connection state changed callback
typedef void (*ConnectionCallback)(const ConnectionInfo* conn, bool connected);

// Pairing state changed callback
typedef void (*PairingCallback)(const PairingSession* session);

// Scan result callback
typedef void (*ScanCallback)(const ScannedDevice* device);

// Data received callback
typedef void (*DataCallback)(const uint8_t* data, size_t len);

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

// Initialization
bool init();
void deinit();
bool is_initialized();

// Enable/disable
bool enable();
void disable();
bool is_enabled();

// Advertising
bool start_advertising();
void stop_advertising();
bool is_advertising();

// Scanning
bool start_scan(uint32_t duration_ms = SCAN_DURATION_MS);
void stop_scan();
bool is_scanning();
const ScannedDevice* get_scanned_devices(size_t* count);
void clear_scan_results();

// Pairing
bool start_pairing();
void cancel_pairing();
bool confirm_pairing(uint32_t pin);
bool reject_pairing();
PairingState get_pairing_state();
uint32_t get_pairing_pin();

// Connection management
bool disconnect();
bool is_connected();
const ConnectionInfo* get_connection_info();

// Paired devices
const PairedDevice* get_paired_devices(size_t* count);
bool remove_paired_device(const uint8_t* address);
bool clear_all_paired_devices();
bool set_device_trusted(const uint8_t* address, bool trusted);
bool set_device_blocked(const uint8_t* address, bool blocked);

// Settings
BluetoothSettings get_settings();
bool set_settings(const BluetoothSettings& settings);
bool set_device_name(const char* name);
bool set_tx_power(int8_t power);

// Status
BluetoothStatus get_status();
BluetoothState get_state();
const char* state_name(BluetoothState state);

// Callbacks
void set_connection_callback(ConnectionCallback cb);
void set_pairing_callback(PairingCallback cb);
void set_scan_callback(ScanCallback cb);
void set_data_callback(DataCallback cb);

// Update (call from loop)
void update();

// Utilities
void format_address(const uint8_t* addr, char* out);
bool parse_address(const char* str, uint8_t* out);
const char* device_type_name(DeviceType type);
const char* security_level_name(SecurityLevel level);
const char* pairing_state_name(PairingState state);

} // namespace bluetooth_channel

#endif // SECURACV_BLUETOOTH_CHANNEL_H
