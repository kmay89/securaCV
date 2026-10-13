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

#include "device_name_rule.h"   // F261: the name holds no control byte (F226's test)
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

// What the device's own Bluetooth name may hold (sweep F261): 1 to
// MAX_DEVICE_NAME_LEN bytes, none of them a control byte. GET
// /api/bluetooth and GET /api/bluetooth/settings write the name through
// ArduinoJson 7.4.1, which writes a control byte other than \b \f \n \r \t
// raw, and a browser's JSON.parse refuses the answer; the radio advertises
// the same name. device_name_rule.h's test (F226, the friendly name's), at
// this name's bound: set_device_name(), a settings command naming the name,
// and POST /api/bluetooth/settings before it brings anything up all apply
// it; a stored name that fails it is not loaded (the default is). The
// scanned devices' names (ScannedDevice) are other devices' and not held to
// it.
inline bool name_storable(const char* name) {
  return device_name_rule::storable(name, MAX_DEVICE_NAME_LEN);
}
// What a refused name is answered with: the rule name_storable() applies,
// in words, its bound held to MAX_DEVICE_NAME_LEN below.
static constexpr char DEVICE_NAME_REFUSAL[] =
    "Invalid Bluetooth name: 1 to 32 bytes, no control characters";
static_assert(device_name_rule::states_bound(DEVICE_NAME_REFUSAL, MAX_DEVICE_NAME_LEN),
              "DEVICE_NAME_REFUSAL states MAX_DEVICE_NAME_LEN");

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
// and a REST handler (bluetooth_api.h's bring_up()) calls it, on
// esp_http_server's task, whenever a tap that needs the stack (Enable,
// Start Advertising, Pair, a settings POST turning Bluetooth on) finds it
// down, whatever the saved setting says (sweep F235). Nothing else here may
// run off the loop task but the readers below and submit().
//
// init() writes none of the loop task's state (sweep F167): it brings the
// stack up and publishes what it made in a hand-over; update() takes it on
// the loop task's next pass, before that pass's commands (so a handler that
// brought the stack up and then submits finds it taken), and there turns
// Bluetooth on and advertises when the settings say so. The saved settings
// and paired list are the loop task's from its first update() pass, before
// any command runs (the F167 review). is_initialized(): any task; true once
// the stack is up and the hand-over published.
//
// `scanners_held` (the sketch's fresh-unit path, provisioning_logic::
// ble_fresh_unit_start_due): bring up the server, every service and
// advertising exactly as always, but neither the active-scan settings nor
// the continuous presence sensor (ble_presence::init/start) — the radio
// duty the deferral of this whole bring-up exists to keep away from a
// phone's WPA2 handshake. They are started later by release_scanners(),
// on the loop task, when the sketch's rule says the join window is clear
// (provisioning_logic::ble_scanners_release_due). scanners_held(): any
// task; true once the loop task took such an init()'s result, until the
// release took. init() alone is init(false): the scanners started, as
// always. The hold reaches init() the way the DIS strings do
// (set_device_metadata() below): pushed before it runs, on the caller's
// task (hold_scanners_on_init()), taken once by the init() that runs
// next; init(bool) is that push and the bring-up in one call.
bool init();
void hold_scanners_on_init(bool held);
inline bool init(bool scanners_held) {
  hold_scanners_on_init(scanners_held);
  return init();
}
bool is_initialized();
void release_scanners();   // loop task; idempotent; refused (held stays true) until the stack is adopted and no owner scan runs
bool scanners_held();

// The sketch's BLE bring-up worker (canary_wap.ino), on the loop task (the
// F167 review): started before the sketch creates it, finished when its
// result is taken (the finalize stage) or it could not be created. After
// init() returns the worker goes on registering GATT services on the same
// server (ble_status, Opera), so in between the channel starts no
// advertising: a start asked for meanwhile is made when it finishes.
void bringup_worker_started();
void bringup_worker_finished();

// The sketch's whole boot bring-up is over (sweep F235's review), on the
// loop task: the last step of its finalize stage, after the worker's result
// was taken (bringup_worker_finished()) and the finalize stage's own NimBLE
// users ran (the Scout's nimble_scan_init(), which calls
// NimBLEDevice::init() when the stack is down), or right after
// bringup_worker_finished() when no worker could be created. Once per boot.
// boot_bring_up_tried(), any task: whether it has been called (release,
// acquire), whatever the bring-up's outcome. The sketch defers that
// bring-up out of the provisioning join window (BLE duty starves a phone's
// WPA2 handshake to the SoftAP; the stack's internal RAM goes to Wi-Fi
// first) and never runs it in safe mode, so false through the join window
// (at most BLE_DISCOVERY_MAX_HOLD_MS, five minutes; a fresh unit with nobody
// on its SoftAP brings it up within seconds, the setup door, scanners held),
// while the worker and the finalize stage run, and for the whole of a
// safe-mode boot. The Start Advertising and Pair handlers bring nothing up
// before it on a device saved on (bluetooth_api.h's
// waits_for_boot_bring_up()): that bring-up will, and every other
// NimBLEDevice::init() caller runs inside it (the worker's channel init()
// and, on FULL, ble_manager::init() when the channel's did not bring the
// stack up; the Scout's at finalize), so a tap's init() would run beside
// them, and init()'s latch holds only the channel's callers.
// boot_bring_up_started(), any task: whether the sketch has started that
// bring-up, bringup_worker_started() above having run (release, acquire;
// once per boot, never cleared). From there until boot_bring_up_tried()
// those two handlers answer that Bluetooth is still starting
// (bring_up_starting_error()), whatever the worker's init() answered: a
// refusal it met is not the last word (on FULL ble_manager's NimBLE init
// and the Scout's follow it, and a tap after the finalize stage runs init()
// again and names its own), and the stack is moments from its answer.
// Before it, boot_bring_up_pending_error(): nothing has started yet.
void boot_bring_up_finished();
bool boot_bring_up_tried();
bool boot_bring_up_started();
inline const char* boot_bring_up_pending_error() {
  return "Bluetooth has not started yet; it starts once Wi-Fi setup is done or within five minutes (never in safe mode)";
}

// Why the last init() attempt left the radio off ("" when initialized or
// never attempted). Any task. Each refusal is written whole before its
// address is published (sweep F167), and a text a caller holds is not
// written again until several later refusals have gone by: safe to hold
// for an answer's length.
const char* init_fail_reason();

// Any task, after a tap's bring_up() answered false (sweep F235): whether
// that was another task's bring-up, not a refusal. init() admits one caller
// at a time and turns the others away at once, so a tap made while the boot
// worker's init() runs gets false with nothing refused; true then, and also
// when that bring-up has finished with the stack up since. These routes are
// served by one esp_http_server task, so no other tap is inside init() then.
// False means init() refused, and init_fail_reason() names
// why (the heap guard, NimBLE's init, the server). The tap answers
// bring_up_starting_error() with a 503 for the first and the refusal for
// the second; it submits nothing either way.
bool bring_up_starting();
inline const char* bring_up_starting_error() {
  return "Bluetooth is still starting; try again in a moment";
}

// Push device metadata into the BLE Device Information Service. Optional;
// must be called BEFORE init() to take effect (init reads the cached
// strings when registering DIS). Strings are copied — caller-owned
// memory not retained. If you skip this, defaults populate the DIS
// (manufacturer="SecuraCV", model="Canary WAP", fw="unknown", etc.).
void set_device_metadata(const char* fw_revision, const char* serial);

// The one flag another task may read live: NimBLE's own advertising state,
// which it keeps under its own lock (the self-test reads it). What the
// status routes show is read from the view below (read_status,
// read_settings, read_scan, read_paired), never live (sweep F138), and so is
// whether Bluetooth is on (read_enabled(), sweep F210: the setting is the
// loop task's, which its commands and its first pass's load write).
bool is_advertising();

// The setup door's security profile (ble_improv.h): true when applied.
// The decision is provisioning_logic::security_profile_for's (host-tested):
// shut restores Numeric Comparison always; open applies Just Works unless a
// pairing is pending or in flight, or a connected link is authenticated or
// bonded — then refused (false), and the caller asks again next pass. The
// Just Works profile is never applied under a bonded session. Loop task
// only.
bool set_setup_door(bool open);
bool setup_door_open();

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
// Bluetooth on did not run, why PAIRED_REMOVE / PAIRED_CLEAR left a
// phone's bond, or why a name was refused (F261).
enum Refusal : uint8_t {
  BT_REFUSED_NONE = 0,     // it ran (ok says how it went)
  BT_REFUSED_NOT_ENABLED,  // Bluetooth off and not brought up (init() has not run);
                           // a refused SETTINGS applied none of its fields
  BT_REFUSED_CONNECTED,    // ADVERTISE_START: a device is connected
  BT_REFUSED_BOND_KEPT,    // PAIRED_REMOVE / PAIRED_CLEAR: NimBLE kept a bond
                           // (ble_gap_unpair() busy); its entry stays listed
  BT_REFUSED_NOT_UP,       // PAIRED_REMOVE / PAIRED_CLEAR: the stack is not up, so
                           // its bond store cannot be reached; nothing changed
  BT_REFUSED_BAD_NAME,     // NAME, or a SETTINGS naming the name: not name_storable()
                           // (sweep F261); a refused SETTINGS applied none of its fields
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
// Any task. Whether the settings the last pass published say on (sweep
// F210: the setting is the loop task's flag, which its commands and its
// first pass's load of the saved settings write, so another task reads it
// here, never in place). The Start Advertising and Pair handlers decided
// from it whether to bring the stack up before they submit, which left a
// device saved on whose boot bring-up failed or was refused with the stack
// down for good; whether Bluetooth is saved on says nothing about whether
// the stack is up, and since sweep F235 bring_up() asks is_initialized().
// They read it only to leave the stack to the sketch's boot bring-up before
// it is over, on a device saved on (bluetooth_api.h's
// waits_for_boot_bring_up(), F235's review); before the first pass it
// answers the boot default, on.
bool read_enabled();
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
