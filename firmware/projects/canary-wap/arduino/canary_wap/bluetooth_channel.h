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
#include <string.h>

#include "loop_command_ring.h"  // F111: the owner's commands handed to the loop task

namespace bluetooth_channel {

// ════════════════════════════════════════════════════════════════════════════
// CONFIGURATION
// ════════════════════════════════════════════════════════════════════════════

// BLE limits
static const size_t BLE_ADDRESS_LENGTH = 6;       // Bluetooth MAC address length
static const size_t BLE_ADDRESS_STR_LEN = 18;     // "XX:XX:XX:XX:XX:XX\0"
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
  uint8_t address[BLE_ADDRESS_LENGTH];      // MAC address
  uint8_t address_type;                     // BLE address type (public/random)
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
  uint8_t address[BLE_ADDRESS_LENGTH];                       // MAC address
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
  uint8_t address[BLE_ADDRESS_LENGTH];
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
  uint16_t mtu;                             // negotiated ATT MTU (23..247)
  uint8_t battery_pct;                      // SIG Battery Service value
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
  // Long-range mode: request LE Coded PHY (S=8) on every new connection.
  // ~4× range vs 1M PHY at ~125 kbps throughput (vs 1 Mbps). Discovery is
  // unaffected — peers still scan and connect on 1M; the PHY update is
  // negotiated post-connection. Disable for higher throughput sessions.
  bool long_range_mode;
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

// Initialization. init() brings the NimBLE stack up, which can block its
// caller past the loop task's 8 s watchdog (canary_wap.ino's BLE bring-up
// note), so it never runs on the loop task: the bring-up worker calls it,
// and a REST handler (bluetooth_api.h) calls it, on esp_http_server's task,
// when the owner turns Bluetooth on before it is up. Nothing else here may
// run off the loop task but the readers below and submit().
//
// init() writes none of the loop task's state (sweep F167): it brings the
// stack up, loads the saved settings and paired list into a hand-over, and
// publishes it; update() takes it on the loop task's next pass, before that
// pass's commands (so a handler that brought the stack up and then submits
// finds it taken), and there turns Bluetooth on and advertises when the
// settings say so. is_initialized(): any task; true once the stack is up
// and the hand-over published.
bool init();
bool is_initialized();

// Why the last init() attempt left the radio off ("" when initialized or
// never attempted). Any task. Each refusal is written whole before its
// address is published (sweep F167), and a text a caller holds is not
// written again until several later refusals have gone by: safe to hold
// for an answer's length.
const char* init_fail_reason();

// Push device metadata into the BLE Device Information Service. Optional;
// must be called BEFORE init() to take effect (init reads the cached
// strings when registering DIS). Strings are copied — caller-owned
// memory not retained. If you skip this, defaults populate the DIS
// (manufacturer="SecuraCV", model="Canary WAP", fw="unknown", etc.).
void set_device_metadata(const char* fw_revision, const char* serial);

// Two flags another task may read live. is_enabled(): whether the setting
// says on (a REST handler decides from it whether to bring the stack up
// first, F111). is_advertising(): NimBLE's own advertising state, which it
// keeps under its own lock (the self-test reads it). What the status routes
// show is read from the view below (read_status, read_settings, read_scan,
// read_paired), never live (sweep F138).
bool is_enabled();
bool is_advertising();

// ──────────────────────────────────────────────────────────────────────────
// The owner's commands (sweep F111)
// ──────────────────────────────────────────────────────────────────────────
//
// What the owner asks for over REST (bluetooth_api.h): turn Bluetooth on or
// off, advertise, scan, pair (start, confirm the PIN, reject, cancel),
// disconnect, manage the paired devices, change the settings, the name and
// the TX power. The REST handlers run on esp_http_server's task, and what
// these change (the pairing session and the pending Numeric-Comparison
// pairing it deletes, the scan, the connection, the settings and their NVS
// keys, the state) is also update()'s, on the loop task: its pairing
// timeout cancels the pairing, its scan timeout stops the scan, its
// inactivity check disconnects. Before F111 a confirm on the HTTP task and
// the timeout's cancel on the loop task could both find the pending pairing
// and both delete it. So the functions that do these are internal to
// bluetooth_channel.cpp, and a handler hands a Command to submit() instead:
// submit() posts it to a ring of COMMAND_SLOTS (loop_command_ring.h, as the
// mesh's and Chirp's submit() do) that update() drains first thing on every
// pass (a disabled or not-yet-started channel included, so BT_CMD_ENABLE
// can turn it on), and waits for its Result.
//
// A command never brings the stack up (that is init()'s, off the loop
// task): BT_CMD_ENABLE, the auto-enable of ADVERTISE_START and PAIR_START,
// and a BT_CMD_SETTINGS that turns Bluetooth on (sweep F144) fail with
// BT_REFUSED_NOT_ENABLED when init() has not run.
//
// The wait is bounded for a command the loop task has not started: after
// timeout_ms it is withdrawn and never runs. kDone: it ran, and *result is
// what it did. kBusy (every slot taken) and kWithdrawn: it did not run and
// will not, and *result is zeroed; the handler answers not_run_status() /
// not_run_error(): 409 bluetooth_busy and 503 bluetooth_timeout. Never call
// submit() from the loop task: it would wait for itself.
//
// The NimBLE host task's callbacks (a connect, a passkey to confirm, a bond,
// a scan result) do not write it either (sweep F143): each posts an event
// that update() applies on the loop task, before the commands
// (bluetooth_channel.cpp, "BLE CALLBACKS").

enum CommandType : uint8_t {
  BT_CMD_ENABLE = 0,
  BT_CMD_DISABLE,
  BT_CMD_ADVERTISE_START,  // turns Bluetooth on first when it is off
  BT_CMD_ADVERTISE_STOP,
  BT_CMD_SCAN_START,       // duration_ms
  BT_CMD_SCAN_STOP,
  BT_CMD_SCAN_CLEAR,
  BT_CMD_PAIR_START,       // turns Bluetooth on first when it is off
  BT_CMD_PAIR_CANCEL,
  BT_CMD_PAIR_CONFIRM,     // pin
  BT_CMD_PAIR_REJECT,
  BT_CMD_DISCONNECT,
  BT_CMD_PAIRED_REMOVE,    // address
  BT_CMD_PAIRED_CLEAR,
  BT_CMD_PAIRED_TRUST,     // address, flag: trusted
  BT_CMD_PAIRED_BLOCK,     // address, flag: blocked
  BT_CMD_SETTINGS,         // the fields set_mask names, from settings; enabled
                           // turns Bluetooth on or off as ENABLE / DISABLE do
  BT_CMD_NAME,             // name
  BT_CMD_POWER,            // power
};

// BT_CMD_SETTINGS: which fields of Command::settings the POST named. The
// loop task applies them to the settings it holds, so two posts do not
// undo each other's other fields.
enum SettingsField : uint16_t {
  BT_SET_ENABLED           = 1u << 0,
  BT_SET_AUTO_ADVERTISE    = 1u << 1,
  BT_SET_ALLOW_PAIRING     = 1u << 2,
  BT_SET_REQUIRE_PIN       = 1u << 3,
  BT_SET_DEVICE_NAME       = 1u << 4,
  BT_SET_TX_POWER          = 1u << 5,
  BT_SET_INACTIVITY        = 1u << 6,
  BT_SET_NOTIFY_ON_CONNECT = 1u << 7,
  BT_SET_LONG_RANGE        = 1u << 8,
};

struct Command {
  CommandType       type;
  bool              flag;
  int8_t            power;
  uint8_t           address[BLE_ADDRESS_LENGTH];
  uint32_t          pin;
  uint32_t          duration_ms;
  // One past MAX_DEVICE_NAME_LEN, so a name too long to keep stays too
  // long here, and set_device_name() refuses it as it always did.
  char              name[MAX_DEVICE_NAME_LEN + 2];
  uint16_t          set_mask;
  BluetoothSettings settings;
};

// Why ENABLE, ADVERTISE_START, PAIR_START or a SETTINGS that turns
// Bluetooth on did not run, or why PAIRED_REMOVE / PAIRED_CLEAR left a
// phone's bond.
enum Refusal : uint8_t {
  BT_REFUSED_NONE = 0,     // it ran (ok says how it went)
  BT_REFUSED_NOT_ENABLED,  // Bluetooth off and not brought up (init() has not run);
                           // a refused SETTINGS applied none of its fields
  BT_REFUSED_CONNECTED,    // ADVERTISE_START: a device is connected
  BT_REFUSED_BOND_KEPT,    // PAIRED_REMOVE / PAIRED_CLEAR: NimBLE kept a bond
                           // (ble_gap_unpair() busy); its entry stays listed
};

// What a command did, as the loop task saw it right after the command ran.
struct Result {
  bool    ok;              // the command's own answer (DISABLE, the stops, CANCEL, CLEAR: true)
  Refusal refusal;
  bool    allow_pairing;   // PAIR_START: the setting when it was refused
};

// A command of `type` with every other field zero.
inline Command make_command(CommandType type) {
  Command cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = type;
  return cmd;
}

static const size_t   COMMAND_SLOTS   = 4;
static const uint32_t COMMAND_WAIT_MS = 2000;   // for the loop task to start it
static const uint32_t COMMAND_POLL_MS = 5;

loop_command_ring::Wait submit(const Command& cmd, Result* result,
                               uint32_t timeout_ms = COMMAND_WAIT_MS);

// The REST answer to a command that did not run (any Wait but kDone).
inline int not_run_status(loop_command_ring::Wait w) {
  return w == loop_command_ring::Wait::kBusy ? 409 : 503;
}
inline const char* not_run_error(loop_command_ring::Wait w) {
  return w == loop_command_ring::Wait::kBusy ? "bluetooth_busy" : "bluetooth_timeout";
}

// ──────────────────────────────────────────────────────────────────────────
// What the status routes show (sweep F138)
// ──────────────────────────────────────────────────────────────────────────
//
// GET /api/bluetooth, /scan/results, /paired and /settings run on
// esp_http_server's task. What they show is the loop task's: update(), the
// owner's commands and the NimBLE host task's events, which update()
// applies (F143), change it. Read in place, one answer could mix two
// passes: a paired device's name read mid-shift after a removal, a scan
// list read while a result is added, a pairing PIN read while a cancel
// wipes it. update() publishes what the routes show at the end of every
// pass (its early return included) and after each owner command it runs
// (before the command's handler answers, so a GET right after a POST shows
// what it did); the readers below copy the last one whole
// (loop_snapshot.h) and never wait for the loop task. Before the first
// pass publishes, they answer what the channel holds at boot: Bluetooth
// disabled, the default settings, no devices.

// The scan list, as GET /api/bluetooth/scan/results shows it.
struct ScanView {
  bool scanning;
  uint8_t count;
  ScannedDevice devices[MAX_SCANNED_DEVICES];
};

// The paired devices, as GET /api/bluetooth/paired shows them.
struct PairedView {
  uint8_t count;
  PairedDevice devices[MAX_PAIRED_DEVICES];
};

// Any task. The status the last pass published, its advertising and
// connected times counted to the read (as get_status() counted them).
void read_status(BluetoothStatus* out);
// Any task. The settings the last pass published.
BluetoothSettings read_settings();
// Any task. The scan list and the paired devices the last pass published.
void read_scan(ScanView* out);
void read_paired(PairedView* out);

// Status
const char* state_name(BluetoothState state);

// Callbacks
void set_connection_callback(ConnectionCallback cb);
void set_pairing_callback(PairingCallback cb);
void set_scan_callback(ScanCallback cb);
void set_data_callback(DataCallback cb);

// Update (call from loop). Applies what the NimBLE host task reported, then
// runs the owner's commands (above), first on every pass, a disabled or
// not-yet-started channel's included.
void update();

// Utilities. An address's bytes are kept as the stack keeps them, least
// significant first; format_address() prints them most significant first
// ("AB:89:67:45:23:01", as the phone shows its own), and parse_address()
// reads that form back into the same bytes, refusing anything else
// (sweep F172).
void format_address(const uint8_t* addr, char* out);
bool parse_address(const char* str, uint8_t* out);
const char* device_type_name(DeviceType type);
const char* security_level_name(SecurityLevel level);
const char* pairing_state_name(PairingState state);

// Coarse distance estimate from RSSI using the log-distance path-loss model:
//   d = 10 ^ ((tx_ref_dbm - rssi) / (10 * n))
// where tx_ref_dbm is the device's expected RSSI at 1 m and n is the path-loss
// exponent (free space ~2.0; indoor ~2.5–3.5). We pick n=2.5 as a household
// default. This is *coarse* — BLE RSSI is noisy and varies with antenna
// orientation, body absorption, and multipath. Treat the result as a
// "near / nearby / far" hint, not survey-grade distance. Matches the way the
// Apple Find My UI presents distance.
//
// rssi_dbm:    measured RSSI (negative dBm; less negative = closer)
// tx_ref_dbm:  RSSI at 1m for this peer (default -59 dBm, typical for BLE 0dBm)
// returns:     estimated meters, clamped to [0.1, 100.0]; 0 means unknown.
float estimate_distance_m(int8_t rssi_dbm, int8_t tx_ref_dbm = -59);

// Friendly bucket label ("near", "nearby", "far", "very far") suitable for UI.
const char* distance_label(float meters);

} // namespace bluetooth_channel

#endif // SECURACV_BLUETOOTH_CHANNEL_H
