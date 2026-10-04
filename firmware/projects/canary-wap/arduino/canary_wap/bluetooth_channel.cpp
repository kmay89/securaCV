/*
 * SecuraCV Canary — Bluetooth Channel Implementation
 * Version 0.1.0
 *
 * BLE (Bluetooth Low Energy) implementation using ESP32 NimBLE stack.
 * Provides secure local connectivity for mobile app integration.
 */

#include "build_config.h"

#if FEATURE_BLUETOOTH && __has_include(<NimBLEDevice.h>)

#include "bluetooth_channel.h"
#include "bt_defaults.h"
#include "loop_event_queue.h"   // F143: what the NimBLE host task reports, applied by update()
#include "loop_snapshot.h"      // F138: what the status routes show, published by update()
#include "ble_heap_guard.h"
#include "nvs_store.h"

// NimBLE headers must come before health_log.h to allow #undef of conflicting macros
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#include <NimBLEAdvertising.h>
#include <NimBLEScan.h>
#include "ble_server_dispatch.h"   // F171: the server's callbacks, shared with Opera

// Undefine NimBLE's log level macros that conflict with our LogLevel enum
#ifdef LOG_LEVEL_DEBUG
#undef LOG_LEVEL_DEBUG
#endif
#ifdef LOG_LEVEL_INFO
#undef LOG_LEVEL_INFO
#endif
#ifdef LOG_LEVEL_NOTICE
#undef LOG_LEVEL_NOTICE
#endif
#ifdef LOG_LEVEL_WARNING
#undef LOG_LEVEL_WARNING
#endif
#ifdef LOG_LEVEL_ERROR
#undef LOG_LEVEL_ERROR
#endif
#ifdef LOG_LEVEL_CRITICAL
#undef LOG_LEVEL_CRITICAL
#endif

#include "health_log.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>       // vTaskDelay: submit() waits for the loop task
#include <ctype.h>                // isxdigit: parse_address()
#include <stdarg.h>               // va_list: set_init_fail_reason()
#include "ble_ota.h"
#include "ble_presence.h"
#include "ble_console.h"
#include "ble_provision.h"
#include "ble_log_export.h"
#include "ble_witness_export.h"
#include "ble_standard_profiles.h"
#include "ota_release_key.h"

namespace bluetooth_channel {

// ════════════════════════════════════════════════════════════════════════════
// INTERNAL STATE
// ════════════════════════════════════════════════════════════════════════════

static BluetoothState g_state = BT_DISABLED;
// The loop task's: it has taken the bring-up's result (adopt_init_result(),
// sweep F167), so the settings, the paired list and the NimBLE objects below
// are the stack's. Read and written only on the loop task; another task asks
// is_initialized(), which reads g_stack_up.
static bool g_initialized = false;
// init()'s, for any task (sweep F167): the stack is up and its result handed
// over (g_bringup). Written once by init() (release), read with acquire.
static bool g_stack_up = false;
// Why the last init() attempt left the radio off, in operator language.
// Empty when initialized (or never attempted). Surfaced by the self-test
// and /api/bluetooth so "Bluetooth broken" field reports carry the cause.
// init() runs on the bring-up worker or an HTTP handler's task while the
// self-test and GET /api/bluetooth read the reason on theirs, and the old
// buffer was rewritten in place under a reader (sweep F167). Now init()
// writes a refusal whole into the next of INIT_FAIL_SLOTS buffers and then
// publishes its address (release); a reader takes the address (acquire)
// and reads text no one writes again until INIT_FAIL_SLOTS more refused
// attempts have gone by. The fixed refusals are string literals.
static const size_t INIT_FAIL_SLOTS = 4;
static char g_init_fail_text[INIT_FAIL_SLOTS][96];
static unsigned g_init_fail_slot = 0;              // init()'s, under its latch
static const char* g_init_fail_reason = "";        // the published address
// True while a caller is inside init()'s body — the bring-up worker and the
// HTTP task can both reach init() now, and NimBLE init is not reentrant.
static volatile bool g_init_in_progress = false;

// Device-info metadata for the SIG Standard Profiles (Device Information
// Service). canary_wap.ino calls set_device_metadata() before init() to
// push the firmware version + serial in. Build-target / build-profile
// names come from build_config.h macros and are baked at compile time.
static char g_meta_manufacturer[24] = "SecuraCV";
static char g_meta_model[24]        = "Canary WAP";
static char g_meta_hw_revision[24]  = HARDWARE_TARGET_NAME;
static char g_meta_fw_revision[24]  = "unknown";
static char g_meta_sw_revision[24]  = BUILD_PROFILE_NAME;
static char g_meta_serial[24]       = "";

// BLE components
static NimBLEServer* g_server = nullptr;
static NimBLEService* g_service = nullptr;
static NimBLECharacteristic* g_status_char = nullptr;
static NimBLECharacteristic* g_command_char = nullptr;
static NimBLECharacteristic* g_notify_char = nullptr;
static NimBLEAdvertising* g_advertising = nullptr;
static NimBLEScan* g_scanner = nullptr;

// Settings (persisted to NVS). The defaults: what a device holds until
// init() loads its saved settings, and what the status routes show before
// the loop task's first pass publishes (sweep F138).
static constexpr BluetoothSettings kDefaultSettings = {
  // Radio on out of the box — see bt_defaults.h. Pairing still needs an
  // explicit on-device PIN confirmation (require_pin), so this is not
  // "open by default", just "reachable by default".
  .enabled = bt_defaults::ENABLED,
  .auto_advertise = bt_defaults::AUTO_ADVERTISE,
  .allow_pairing = bt_defaults::ALLOW_PAIRING,
  .require_pin = bt_defaults::REQUIRE_PIN,
  .device_name = "SecuraCV-Canary",
  // +9 dBm: the radio max valid on ESP32-S3/C3. This is now the single source
  // of BLE TX power for the whole firmware. It defaults to +9 to preserve the
  // power the device actually ran at before — ble_manager::init() used to
  // unconditionally bump every combined build to +9 after this struct's old
  // default of +3, so +3 never took effect in practice. Devices with an
  // explicit NVS override keep that value (now correctly honored).
  .tx_power = 9,
  .inactivity_timeout_ms = INACTIVITY_TIMEOUT_MS,
  .notify_on_connect = true,
  .long_range_mode = false
};
static BluetoothSettings g_settings = kDefaultSettings;

// Connection state
static ConnectionInfo g_connection = {};
static PairingSession g_pairing = {};

// Pending Numeric-Comparison pairing — captured in onConfirmPassKey, drained
// by confirm_pairing()/reject_pairing()/cancel_pairing(). Without this, the
// only way to satisfy the host's pairing flow was to call injectConfirmPasskey
// from inside the callback itself, which forced an auto-yes that bypassed the
// MITM check. NimBLEConnInfo is a thin wrapper around ble_gap_conn_desc — safe
// to copy by value, but NimBLE-Arduino 2.x makes its default constructor
// private, so we can't keep a default-constructed static. Hold a heap copy
// instead: it's only ever copy-constructed from the live connInfo, and the
// nullptr/non-null state doubles as the "pending" flag's backing store.
static NimBLEConnInfo* g_pending_pair_info = nullptr;
static bool g_pending_pair_active = false;

// Active connection handle so update() can poll RSSI / MTU without keeping
// a reference to the callback's NimBLEConnInfo (which is per-callback scope).
// 0xFFFF = "no connection" (BLE conn handles are 0..0x0EFE in NimBLE).
static uint16_t g_connection_handle = 0xFFFF;
static uint16_t g_connection_mtu = 23;  // negotiated MTU; defaults to ATT min

// Paired devices. Each keeps its identity address (sweep F172), the key
// NimBLE's bond store holds it by; a list saved before F172 kept the
// over-the-air address, and init() rebuilds it from the bond store once.
static PairedDevice g_paired_devices[MAX_PAIRED_DEVICES];
static size_t g_paired_count = 0;
static bool g_paired_by_identity = false;   // the saved list's NVS_KEY_BT_PAIRED_ID

// What init() brings up, handed to the loop task (sweep F167). init() runs
// on the BLE bring-up worker or on an HTTP handler's task
// (bluetooth_api.h's bring_up()). It used to load the saved settings and
// the paired list straight into g_settings and g_paired_devices, set the
// state, set the NimBLE object pointers and g_initialized (a plain bool),
// and with auto-advertise on call enable() and start_advertising(), all
// while the loop task's update() drained commands, applied the NimBLE
// events (F143) and published the views (F138) from the same state. Now
// init() fills g_bringup and touches none of the loop task's state: it
// brings the stack up, keeps the objects it made and the settings it read
// for the stack (the name NimBLE advertises, the TX power and the PHY it
// set), and publishes them (g_bringup_ready, release) as its last write to
// g_bringup. The loop task takes it once, first thing in update() after
// load_saved() (adopt_init_result(): the objects, the list's rebuild, the
// channel's server callbacks, the auto-advertise). After the publish
// nothing writes g_bringup again: a later init() returns at once
// (g_stack_up).
//
// The saved settings and the paired list are not in it (the F167 review):
// they are the loop task's from its first pass (load_saved()), before any
// command runs. A hand-over that carried them overwrote, at adoption, what
// the owner's commands had done while init() ran (NimBLE's bring-up can
// take ~21 s): a Disable came back on, a name or a TX power went back, in
// RAM and in NVS.
struct Bringup {
  BluetoothSettings applied;      // the saved settings as init() read them for the stack
  NimBLEServer* server;
  NimBLEService* service;
  NimBLECharacteristic* status_char;
  NimBLECharacteristic* command_char;
  NimBLECharacteristic* notify_char;
  NimBLEAdvertising* advertising;
  NimBLEScan* scanner;
};
static Bringup g_bringup;
static bool g_bringup_ready = false;   // init() publishes (release), adopt_init_result() takes (acquire)
// The loop task's (the F167 review): update() has loaded the saved
// settings and the paired list (load_saved()).
static bool g_saved_loaded = false;
// The loop task's (the F167 review): the sketch's BLE bring-up worker is
// running (bringup_worker_started() to bringup_worker_finished(), both
// called by canary_wap.ino on the loop task). After init() returns it goes
// on registering GATT services on the same server (ble_status on DEV and
// FULL, Opera on FULL), and NimBLEAdvertising::start() starts the server
// (NimBLEServer::start(), which walks the service list it appends to), so
// no advertising starts meanwhile: a start asked for then
// (start_advertising(), or restore_radio() putting a stopped advertiser
// back) is held in g_advertise_after_bringup and made when the worker
// finishes.
static bool g_bringup_worker_running = false;
static bool g_advertise_after_bringup = false;

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

// The owner's commands on their way to the loop task (sweep F111): posted by
// submit() on esp_http_server's task (bluetooth_api.h's handlers), drained by
// update() on the loop task (run_command). A portMUX spinlock guards the
// slots; it is held only to copy a command or a result in or out, never
// while one runs.
static loop_command_ring::Ring<Command, Result, COMMAND_SLOTS, loop_command_ring::PortMuxLock>
    g_commands;

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
// Set once the saved paired list holds identity addresses (sweep F172).
static const char* NVS_KEY_BT_PAIRED_ID = "bt_paired_id";
static const char* NVS_KEY_BT_LONG_RANGE = "bt_long_range";

// ════════════════════════════════════════════════════════════════════════════
// FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════════════════════════

static void set_state(BluetoothState new_state);
static BluetoothState rest_state();
static bool init_running();
static void load_saved();
static void adopt_init_result();
static void load_settings(BluetoothSettings* out);
static void save_settings();
static void load_paired_devices(PairedDevice* out, size_t* count, bool* by_identity);
static void save_paired_devices();
static void migrate_paired_devices();
static void drop_entries_without_bond();
static NimBLEAddress paired_identity(const PairedDevice& dev);
static void update_status_characteristic();
static void handle_inactivity_timeout();
static void handle_scan_timeout();
static DeviceType detect_device_type(const NimBLEAdvertisedDevice* device);
// The owner's commands' bodies (sweep F111): they change what update() reads
// and writes, so a REST handler hands a Command to submit() and the loop task
// runs them through run_command(). Inside this file the bring-up (init()'s
// own auto-advertise), update()'s timeouts and a link's end as update()
// applies it (apply_disconnect(): the NimBLE callback only reports it, sweep
// F143) call some of them too. deinit() has no caller.
static bool enable();
static void disable();
static bool start_advertising();
static void stop_advertising();
static bool start_scan(uint32_t duration_ms);
static void stop_scan();
static void clear_scan_results();
static bool start_pairing();
static void drop_pending_pairing();
static bool answer_pending_pairing(bool accept);
static void cancel_pairing();
static bool confirm_pairing(uint32_t pin);
static bool reject_pairing();
static bool disconnect();
static bool remove_paired_device(const uint8_t* address, Refusal* refusal);
static bool clear_all_paired_devices(Refusal* refusal);
static bool set_device_trusted(const uint8_t* address, bool trusted);
static bool set_device_blocked(const uint8_t* address, bool blocked);
static bool set_settings(const BluetoothSettings& settings);
static bool set_device_name(const char* name);
static bool set_tx_power(int8_t power);

// ════════════════════════════════════════════════════════════════════════════
// BLE CALLBACKS: what the NimBLE host task reports (sweep F143)
// ════════════════════════════════════════════════════════════════════════════
//
// The callbacks below run on the NimBLE host task. Before F143 they wrote
// the connection, the pairing session and its pending Numeric-Comparison
// answer, the paired list (and saved it to NVS there), the scan results,
// the scan flag and the state, and restarted advertising, while update()
// and the owner's commands read and wrote the same on the loop task with no
// lock. The sharpest case: onConfirmPassKey() deleted and replaced the
// pending pairing while a PIN confirm or the pairing timeout on the loop
// task could be answering and deleting it.
//
// Now a callback names none of that state. It describes what happened (an
// Event, from its own arguments and millis()) and posts it to g_events
// (loop_event_queue.h); update() applies each event, in the order posted,
// on the loop task (apply_event), before the owner's commands. The pending
// pairing's heap copy travels in its event: whoever holds the pointer owns
// it, so exactly one taker answers and deletes it.
//
// The queue holds EVENT_SLOTS events. A scan result and a link's activity
// (a GATT read or write) are posted only while fewer than EVENT_LOSSY_LIMIT
// wait, so a burst of advertisements or writes never takes the room kept
// for a link's own events (up, down, a passkey, a bond). When an event
// still finds no room (the loop task stalled for that long), it is dropped
// and counted by kind (loop_event_queue.h): update() logs a link's dropped
// events as a warning and asks the stack for the link (reconcile_link(),
// sweep F169: a lost connect or disconnect would leave the state stale),
// and logs the scan results and activity the lower limit refused at debug
// level (routine when a busy room advertises through a stall). A passkey
// to confirm that finds no
// room is answered no on this task instead (as is one whose copy could not
// be allocated): the pairing fails closed, and the phone may try again.

enum EventType : uint8_t {
  BT_EV_CONNECT = 0,
  BT_EV_DISCONNECT,
  BT_EV_AUTH_COMPLETE,
  BT_EV_PASSKEY_DISPLAY,
  BT_EV_CONFIRM_PASSKEY,
  BT_EV_SCAN_RESULT,
  BT_EV_SCAN_END,
  BT_EV_ACTIVITY,
  BT_EV_STORE_FULL,      // F189: a pairing refused at its start, the bond store full
  BT_EV_STORE_OVERFLOW,  // F189: a record the bond store had no room for, not kept
};

// A link, as a server callback's NimBLEConnInfo named it. The addresses
// are the stack's own form (ble_addr_t: the type, and the bytes least
// significant first, as getBase() holds them), so the loop task rebuilds
// the same NimBLEAddress from them. NimBLE-Arduino 2.x's byte-array
// constructor takes the bytes in printed order and reverses them, so
// bytes copied out of getBase() went back through it printed backwards.
// `address` is the one on the air (getAddress()): a phone using
// resolvable private addresses shows a new one every few minutes.
// `id_address` is its identity (getIdAddress()), the address NimBLE keys
// its bond by, and the one the paired list keeps (sweep F172); the two are
// the same for a peer that uses no private address.
struct LinkEvent {
  uint16_t handle;
  ble_addr_t address;
  ble_addr_t id_address;
  bool encrypted;
  bool authenticated;
  bool bonded;
  int32_t reason;                           // BT_EV_DISCONNECT
};

struct PasskeyEvent {
  uint32_t pin;
  NimBLEConnInfo* conn;                     // BT_EV_CONFIRM_PASSKEY: the loop task's to answer and delete
};

struct ActivityEvent {
  uint32_t rx_bytes;
};

// The bond store's status (sweep F189): NimBLE's BLE_STORE_EVENT_FULL (the
// link whose pairing was refused) or BLE_STORE_EVENT_OVERFLOW (the kind of
// record not kept).
struct StoreEvent {
  uint16_t conn_handle;
  int obj_type;
};

struct Event {
  EventType type;
  uint32_t at_ms;                           // millis() when the callback ran
  union {
    LinkEvent link;
    PasskeyEvent passkey;
    ScannedDevice scan;                     // BT_EV_SCAN_RESULT: last_seen_ms is at_ms
    ActivityEvent activity;
    StoreEvent store;
  } u;
};

static const size_t EVENT_SLOTS = 24;
static const size_t EVENT_LOSSY_LIMIT = 16;

static loop_event_queue::Queue<Event, EVENT_SLOTS, loop_command_ring::PortMuxLock> g_events;

// The loop task's: a drop count last logged, and when (at most once a minute).
struct DropLog {
  uint32_t seen;
  uint32_t logged_ms;
};
static DropLog g_link_drops = {0, 0};     // a link's events (posted at the full limit)
static DropLog g_lossy_drops = {0, 0};    // scan results and GATT activity (the lower limit)
// The loop task's (sweep F189): pairings the full bond store refused, and
// records it had no room for, since boot, with their logs (as the drops'
// are: at most once a minute each).
static uint32_t g_store_full_count = 0;
static uint32_t g_store_overflow_count = 0;
static DropLog g_store_full_log = {0, 0};
static DropLog g_store_overflow_log = {0, 0};
// The loop task's: the count of a link's dropped events reconcile_link()
// last answered (sweep F169).
static uint32_t g_link_drops_reconciled = 0;

// An Event of `type`, at the callback's time, the rest zero.
static Event make_event(EventType type) {
  Event e;
  memset(&e, 0, sizeof(e));
  e.type = type;
  e.at_ms = millis();
  return e;
}

// The link a server callback names, from its own argument.
static Event link_event(EventType type, NimBLEConnInfo& connInfo) {
  Event e = make_event(type);
  e.u.link.handle = connInfo.getConnHandle();
  e.u.link.address = *connInfo.getAddress().getBase();
  e.u.link.id_address = *connInfo.getIdAddress().getBase();
  e.u.link.encrypted = connInfo.isEncrypted();
  e.u.link.authenticated = connInfo.isAuthenticated();
  e.u.link.bonded = connInfo.isBonded();
  return e;
}

// The one way an event reaches the loop task. False when the queue had no
// room for it (`limit` events waiting): it was dropped and counted.
static bool post_event(const Event& e, size_t limit = EVENT_SLOTS) {
  return g_events.post(e, limit);
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*server*/, NimBLEConnInfo& connInfo) override {
    (void)post_event(link_event(BT_EV_CONNECT, connInfo));
  }

  void onDisconnect(NimBLEServer* /*server*/, NimBLEConnInfo& connInfo, int reason) override {
    Event e = link_event(BT_EV_DISCONNECT, connInfo);
    e.u.link.reason = reason;
    (void)post_event(e);
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    (void)post_event(link_event(BT_EV_AUTH_COMPLETE, connInfo));
  }

  uint32_t onPassKeyDisplay() override {
    // NimBLE 2.x: Return passkey to display (generate random 6-digit)
    // Use rejection sampling to avoid modulo bias
    uint32_t passkey;
    do {
      passkey = esp_random();
    } while (passkey >= (UINT32_MAX - (UINT32_MAX % 1000000)));
    passkey %= 1000000;
    Event e = make_event(BT_EV_PASSKEY_DISPLAY);
    e.u.passkey.pin = passkey;
    // Dropped (no room): the owner never sees the digits and the pairing
    // fails at the phone, closed.
    (void)post_event(e);
    return passkey;
  }

  void onConfirmPassKey(NimBLEConnInfo& connInfo, uint32_t pin) override {
    // BLE Numeric Comparison: the peer and we both saw `pin` derived from the
    // ECDH handshake. The user must visually verify the same six digits show
    // on both screens before we tell NimBLE to accept — this is the bit that
    // closes Man-In-The-Middle. The conn info goes to the loop task in the
    // event (a heap copy: NimBLE-Arduino 2.x makes NimBLEConnInfo's default
    // constructor private), which surfaces the PIN to the SPA; the owner's
    // confirm, reject or cancel, or the pairing timeout, answers it there.
    // Intentionally NO injectConfirmPasskey(..., true) here.
    Event e = make_event(BT_EV_CONFIRM_PASSKEY);
    e.u.passkey.pin = pin;
    // ESP32 Arduino builds run with exceptions off: a failed `new` is null.
    e.u.passkey.conn = new NimBLEConnInfo(connInfo);
    if (e.u.passkey.conn == nullptr || !post_event(e)) {
      // Not handed over: answered no here, so NimBLE does not wait on an
      // answer nobody can give. Fails closed.
      delete e.u.passkey.conn;
      NimBLEDevice::injectConfirmPasskey(connInfo, false);
    }
  }
};

class CharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& /*connInfo*/) override {
    std::string value = characteristic->getValue();
    Event e = make_event(BT_EV_ACTIVITY);
    e.u.activity.rx_bytes = (uint32_t)value.length();
    (void)post_event(e, EVENT_LOSSY_LIMIT);

    // The data hook runs here, on the NimBLE host task, as it always did
    // (nothing sets it today).
    if (g_data_callback && value.length() > 0) {
      g_data_callback((const uint8_t*)value.data(), value.length());
    }
  }

  void onRead(NimBLECharacteristic* /*characteristic*/, NimBLEConnInfo& /*connInfo*/) override {
    (void)post_event(make_event(BT_EV_ACTIVITY), EVENT_LOSSY_LIMIT);
  }
};

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    Event e = make_event(BT_EV_SCAN_RESULT);
    ScannedDevice& entry = e.u.scan;
    memcpy(entry.address, device->getAddress().getBase()->val, BLE_ADDRESS_LENGTH);
    if (device->haveName()) {
      strncpy(entry.name, device->getName().c_str(), MAX_DEVICE_NAME_LEN);
      entry.name[MAX_DEVICE_NAME_LEN] = '\0';
    } else {
      entry.name[0] = '\0';
    }
    entry.rssi = device->getRSSI();
    entry.connectable = device->isConnectable();
    entry.type = detect_device_type(device);
    entry.has_securacv_service = device->isAdvertisingService(NimBLEUUID(SERVICE_UUID));
    entry.last_seen_ms = e.at_ms;
    (void)post_event(e, EVENT_LOSSY_LIMIT);
  }

  void onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) override {
    (void)post_event(make_event(BT_EV_SCAN_END));
  }
};

// The bond store's status (sweep F189), on the NimBLE host task. NimBLE
// keeps CONFIG_BT_NIMBLE_MAX_BONDS bonds (nimconfig.h's default, 3: no WAP
// build overrides it, and a define in the sketch does not reach the
// library's own compile), where the paired list keeps MAX_PAIRED_DEVICES
// (8). Its default answer, ble_store_util_status_rr() (NimBLE-Arduino 2.3.8
// and 2.5.0, ble_store_util.c), lets a pairing start on a full store and,
// when its bond then finds no room, unpairs the oldest bond
// (ble_gap_unpair_oldest_peer()) through ble_gap_unpair()'s busy guard: the
// oldest phone lost its bond while the list kept it, or, its bond carrying
// an IRK while the WAP advertised or scanned (nearly always), the new bond
// was not stored and the list took it anyway. Here no bond is ever evicted
// behind the owner:
//   - BLE_STORE_EVENT_FULL (ble_sm_chk_store_overflow(), at the phone's
//     Pairing Request, before any key; it counts the pairings under way
//     too): refused, unless the peer on that link is one the store already
//     holds (a phone that lost its keys pairs again over its own record).
//     The owner removes a paired phone to make room.
//   - BLE_STORE_EVENT_OVERFLOW (ble_store_write(), a record with no room
//     after all: a bond whose pairing was let start as a held peer but that
//     handed over a new identity, an address record, or a bonded phone's
//     CCCD, a subscription kept across links, whose store holds
//     CONFIG_BT_NIMBLE_MAX_CCCDS = 8 records where the sketch has a dozen
//     characteristics to subscribe to): not kept. NimBLE's default made
//     room for a CCCD by unpairing the oldest bond but the writing phone's
//     (ble_gap_unpair_oldest_except()), another listed phone's, behind the
//     owner, as for a bond. Refused, a subscription's record is not kept:
//     on the write path ble_gatts_clt_cfg_access() returns the store's
//     error as the access's, so the phone's CCCD Write Request is answered
//     with an ATT error (ble_att_svr_write(), NimBLE-Arduino 2.5.0), while
//     the subscription is already set in RAM and holds for that link only;
//     the other writes of a CCCD (the stack's own, at a reconnect or a
//     notify while disconnected) ignore the error.
// So the store-full path calls no ble_gap_unpair() and cannot fail on its
// busy guard. Each posts its event (the log is the loop task's) under the
// lossy limit: a stranger's repeated pairing attempts never take the room
// kept for a link's own events.
class StoreCallbacks : public NimBLEDeviceCallbacks {
  int onStoreStatus(struct ble_store_status_event* event, void* /*arg*/) override {
    if (event->event_code == BLE_STORE_EVENT_FULL) {
      NimBLEServer* server = NimBLEDevice::getServer();
      if (server != nullptr) {
        const NimBLEConnInfo peer = server->getPeerInfoByHandle(event->full.conn_handle);
        if (peer.getConnHandle() == event->full.conn_handle && NimBLEDevice::isBonded(peer.getIdAddress())) {
          return 0;   // over its own record: no room needed
        }
      }
      Event e = make_event(BT_EV_STORE_FULL);
      e.u.store.conn_handle = event->full.conn_handle;
      e.u.store.obj_type = event->full.obj_type;
      (void)post_event(e, EVENT_LOSSY_LIMIT);
      return BLE_HS_ESTORE_CAP;
    }
    if (event->event_code == BLE_STORE_EVENT_OVERFLOW) {
      Event e = make_event(BT_EV_STORE_OVERFLOW);
      e.u.store.obj_type = event->overflow.obj_type;
      (void)post_event(e, EVENT_LOSSY_LIMIT);
      return BLE_HS_ESTORE_CAP;
    }
    return BLE_HS_EUNKNOWN;
  }
};

static ServerCallbacks g_server_callbacks;
static CharacteristicCallbacks g_char_callbacks;
static ScanCallbacks g_scan_callbacks;
static StoreCallbacks g_store_callbacks;

// ── The loop task applies them ─────────────────────────────────────────

static void apply_connect(const Event& e) {
  const LinkEvent& link = e.u.link;
  // Bluetooth is off (sweep F173). disable() stops advertising and drops
  // the link it knows of, but a phone whose connection was already in
  // flight (or, on the FULL profile, one Opera's advertising let in) comes
  // up anyway, and nothing would ever drop it: update() returns early while
  // the channel is disabled, so no inactivity timeout runs. Refused: the
  // link is dropped and nothing of it is recorded (its end, when the stack
  // reports it, finds no connection and leaves the state disabled).
  if (!g_settings.enabled) {
    if (g_server) {
      g_server->disconnect(link.handle);
    }
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE link refused: Bluetooth is off", nullptr);
    return;
  }
  g_connection.connected = true;
  g_connection_handle = link.handle;
  memcpy(g_connection.address, link.address.val, BLE_ADDRESS_LENGTH);

  // The address as NimBLE prints the phone's own (the ble_addr_t
  // constructor keeps the stack's byte order; see LinkEvent).
  const NimBLEAddress addr(link.address);
  strncpy(g_connection.name, addr.toString().c_str(), MAX_DEVICE_NAME_LEN);
  g_connection.name[MAX_DEVICE_NAME_LEN] = '\0';

  g_connection.connected_since_ms = e.at_ms;
  g_connection.last_activity_ms = e.at_ms;
  g_connection.bytes_sent = 0;
  g_connection.bytes_received = 0;

  // Request a faster connection interval. Units: 1.25 ms for interval,
  // 10 ms for supervision timeout. The (24, 40, 0, 400) range is inside
  // Apple's accepted band (min 15 ms, range >= 15 ms, timeout >= 2 s)
  // so iOS won't reject and renegotiate to 30 ms+. Android typically
  // honors the request directly. Falls back silently to the default
  // 30-ms interval if the peer refuses. (Asked from the loop task since
  // F143, a pass after the link came up; a link already gone refuses it.)
  if (g_server) {
    g_server->updateConnParams(link.handle, 24, 40, 0, 400);
  }

  // If long-range mode is on, request a PHY switch to LE Coded S=8.
  // BLE_HCI_LE_PHY_CODED_PREF_MASK = 0x04, S=8 option = 0x0002. Peer can
  // refuse and we keep 1M — there's no downside to trying.
  if (g_settings.long_range_mode) {
    ble_gap_set_prefered_le_phy(link.handle, 0x04, 0x04, 0x0002);
  }

  // Update security level
  if (link.encrypted) {
    g_connection.security = link.authenticated ? SEC_AUTHENTICATED : SEC_ENCRYPTED;
  } else {
    g_connection.security = SEC_NONE;
  }

  g_total_connections++;
  set_state(BT_CONNECTED);

  // Tell the presence sensor to drop to reduced-duty so the live console
  // link gets more radio time. ble_presence stops/restarts the scanner
  // with the new parameters internally.
  ble_presence::notify_console_connected(true);

  // No address in the line (the F172 review): the health log is durable
  // (the SD card's /HEALTH, the BLE log export), and a peer's address is an
  // identifier the no-identity invariant keeps out of it (spec
  // canary_free_signals_v0.md, Invariant A). Since F171 this line runs for
  // every link on the FULL profile too.
  if (g_settings.notify_on_connect) {
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE device connected", nullptr);
  }

  if (g_conn_callback) {
    g_conn_callback(&g_connection, true);
  }

  // Stop advertising while connected, unless another owner of the server's
  // links advertises on the same advertiser for its own reasons: Opera, on
  // the FULL profile, keeps the fleet-link beacon on the air and starts
  // advertising again after every connect. Before the dispatcher (sweep
  // F171) this callback never ran on FULL, so no FULL build stopped the
  // advertiser for a phone's link; stopping it here would take the beacon
  // the displays read off the air for the whole link.
  if (g_advertising && g_advertising->isAdvertising() && !ble_server_dispatch::link_observed()) {
    g_advertising->stop();
  }
}

// Whether `link` (an ended link's event) is the connection the channel
// records: the same handle and the same over-the-air address. A handle is
// reused, so the handle alone could name the next link.
static bool is_recorded_link(const LinkEvent& link) {
  return g_connection.connected && g_connection_handle == link.handle &&
         memcmp(g_connection.address, link.address.val, BLE_ADDRESS_LENGTH) == 0;
}

static void apply_disconnect(const Event& e) {
  // A pairing awaiting the owner's answer on this link ends with it: the
  // stack ended that pairing when the link went, and the copy must not
  // outlive it, or a later confirm of its six digits would answer yes on
  // whatever link takes the handle next (a phone whose own digits were
  // never shown). No answer is given, not even a no: the link it belonged
  // to is gone, and whatever holds the handle by this pass (the same phone
  // back already, pairing afresh) is a pairing of its own.
  if (g_pending_pair_info != nullptr &&
      g_pending_pair_info->getConnHandle() == e.u.link.handle) {
    drop_pending_pairing();
    g_pairing.state = PAIR_FAILED;
    g_pairing.pin_displayed = false;       // its digits mean nothing now
    g_pairing.pin_code = 0;
    g_pairing.user_confirmed = false;
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "Pairing link lost before confirmation", nullptr);
    if (g_pair_callback) {
      g_pair_callback(&g_pairing);
    }
  }

  // The end of a link that is not the one recorded (sweep F169): a second
  // link that came up while one was recorded (Opera's advertising, on the
  // FULL profile, lets one in), or the stack's own report of a link the
  // loop task already ended when it found the link gone (reconcile_link()).
  // The recorded connection is another link's and stays.
  if (g_connection.connected && !is_recorded_link(e.u.link)) {
    return;
  }

  // A link whose connect event was dropped (a full queue) adds no time.
  uint32_t connected_duration = 0;
  if (g_connection.connected) {
    connected_duration = e.at_ms - g_connection.connected_since_ms;
    g_connected_total_ms += connected_duration;
  }

  if (g_settings.notify_on_connect) {
    char detail[80];
    snprintf(detail, sizeof(detail), "Duration: %lus, Reason: %d",
             (unsigned long)(connected_duration / 1000), (int)e.u.link.reason);
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE device disconnected", detail);
  }

  if (g_conn_callback) {
    g_conn_callback(&g_connection, false);
  }

  memset(&g_connection, 0, sizeof(g_connection));
  g_connection_handle = 0xFFFF;
  g_connection_mtu = 23;

  // Restore the presence sensor's normal duty cycle now that the radio
  // doesn't need to favor a live link.
  ble_presence::notify_console_connected(false);

  // Resume advertising if enabled
  if (g_settings.enabled && g_settings.auto_advertise) {
    start_advertising();
  }
  // The state from what still runs (sweep F170): disabled when the link
  // ended after Bluetooth was turned off (POST /disable, or the settings'
  // "enabled": false, which drop it); otherwise a scan, pairing mode or
  // advertising that goes on. On the FULL profile Opera's onDisconnect has
  // usually restarted advertising before this pass, and the state read
  // idle with "advertising": true.
  set_state(rest_state());
}

static void apply_auth_complete(const Event& e) {
  const LinkEvent& link = e.u.link;
  // The connection card and the pairing's state are the recorded link's
  // (the F171 review): on FULL a second link can be up beside it (Opera
  // advertises through a link), and its encryption must not mark the
  // recorded one bonded or end its pairing. A bond is a bond whichever link
  // made it, so the paired list takes it either way, by its identity.
  const bool recorded = is_recorded_link(link);
  if (link.authenticated) {
    if (recorded) {
      g_connection.security = link.bonded ? SEC_BONDED : SEC_AUTHENTICATED;
    }
    if (link.bonded) {

      // Add to paired devices, by the identity address the bond is keyed by
      // (sweep F172): the over-the-air address of a phone using resolvable
      // private addresses changes every few minutes and names no bond, so
      // a Remove could never forget it.
      const NimBLEAddress identity(link.id_address);
      const std::string identity_str = identity.toString();
      bool found = false;
      for (size_t i = 0; i < g_paired_count; i++) {
        if (memcmp(g_paired_devices[i].address, link.id_address.val, BLE_ADDRESS_LENGTH) == 0) {
          g_paired_devices[i].last_connected_ms = e.at_ms;
          g_paired_devices[i].connection_count++;
          g_paired_devices[i].security = SEC_BONDED;
          found = true;
          break;
        }
      }

      // Only a bond the store holds (sweep F189): NimBLE reports the link
      // bonded whether or not the bond was stored (a record the full store
      // did not keep, StoreCallbacks), and an entry no bond backs is one no
      // phone can use and Remove cannot forget. So the list never outgrows
      // the store.
      if (!found && !NimBLEDevice::isBonded(identity)) {
        log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
                   "Paired device not listed: the bond store did not keep it", nullptr);
      } else if (!found && g_paired_count < MAX_PAIRED_DEVICES) {
        PairedDevice* dev = &g_paired_devices[g_paired_count++];
        memcpy(dev->address, link.id_address.val, BLE_ADDRESS_LENGTH);
        dev->address_type = link.id_address.type;
        strncpy(dev->name, identity_str.c_str(), MAX_DEVICE_NAME_LEN);
        dev->name[MAX_DEVICE_NAME_LEN] = '\0';
        dev->paired_timestamp = e.at_ms / 1000;
        dev->last_connected_ms = e.at_ms;
        dev->connection_count = 1;
        dev->security = SEC_BONDED;
        dev->trusted = false;
        dev->blocked = false;

        save_paired_devices();
        // No address in the line (the F172 review): the identity address is
        // the phone's stable one (on most phones the public Bluetooth
        // address), and the health log is durable. The paired list keeps it,
        // where Remove needs it.
        log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "New device paired", nullptr);
      }
    }

    if (recorded) {
      g_pairing.state = PAIR_COMPLETE;
      if (g_pair_callback) {
        g_pair_callback(&g_pairing);
      }
    }
  } else {
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH, "Pairing failed", nullptr);
    if (recorded) {
      g_pairing.state = PAIR_FAILED;
      if (g_pair_callback) {
        g_pair_callback(&g_pairing);
      }
    }
  }
}

static void apply_passkey_display(const Event& e) {
  g_pairing.pin_code = e.u.passkey.pin;
  g_pairing.state = PAIR_PIN_DISPLAYED;
  g_pairing.pin_displayed = true;

  char pin_str[16];
  snprintf(pin_str, sizeof(pin_str), "%06lu", (unsigned long)e.u.passkey.pin);
  log_health(SCV_LOG_NOTICE, SCV_CAT_BLUETOOTH, "Pairing PIN displayed", pin_str);

  if (g_pair_callback) {
    g_pair_callback(&g_pairing);
  }
}

static void apply_confirm_passkey(const Event& e) {
  // Bluetooth is off (sweep F173): the link is being refused, and no pairing
  // may wait for a yes meanwhile (update() returns early while disabled, so
  // no pairing timeout would answer it). Answered no, and its copy deleted.
  if (!g_settings.enabled) {
    NimBLEDevice::injectConfirmPasskey(*e.u.passkey.conn, false);
    delete e.u.passkey.conn;
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE pairing refused: Bluetooth is off", nullptr);
    return;
  }
  // The event's copy is the loop task's now. One left from an earlier
  // passkey that nobody answered is replaced, as it always was (NimBLE
  // times that attempt out).
  delete g_pending_pair_info;
  g_pending_pair_info = e.u.passkey.conn;
  g_pending_pair_active = true;

  g_pairing.state = PAIR_CONFIRMING;
  g_pairing.pin_code = e.u.passkey.pin;
  g_pairing.pin_displayed = true;
  g_pairing.user_confirmed = false;

  char pin_str[16];
  snprintf(pin_str, sizeof(pin_str), "%06lu", (unsigned long)e.u.passkey.pin);
  log_health(SCV_LOG_NOTICE, SCV_CAT_BLUETOOTH,
             "BLE pairing PIN — awaiting user confirmation", pin_str);

  if (g_pair_callback) {
    g_pair_callback(&g_pairing);
  }
  // The pairing timeout (PAIRING_TIMEOUT_MS) rejects it if the user doesn't
  // respond.
}

static void apply_scan_result(const Event& e) {
  const ScannedDevice& seen = e.u.scan;
  // Check if already in list
  for (size_t i = 0; i < g_scanned_count; i++) {
    if (memcmp(g_scanned_devices[i].address, seen.address, BLE_ADDRESS_LENGTH) == 0) {
      // Update existing entry
      g_scanned_devices[i].rssi = seen.rssi;
      g_scanned_devices[i].last_seen_ms = seen.last_seen_ms;
      return;
    }
  }

  // Add new device
  if (g_scanned_count < MAX_SCANNED_DEVICES) {
    ScannedDevice* entry = &g_scanned_devices[g_scanned_count++];
    *entry = seen;
    if (g_scan_callback) {
      g_scan_callback(entry);
    }
  }
}

static void apply_scan_end(const Event& /*e*/) {
  // Once per scan: stop_scan() (the owner's, or update()'s scan timeout)
  // may have ended it on this task already.
  if (!g_scanning) return;
  g_scanning = false;
  set_state(rest_state());   // F170: advertising, a link or pairing mode may go on
  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE scan complete",
             String(g_scanned_count).c_str());
  // Hand the radio back to the always-on presence loop. Safe to call
  // even when the user-triggered scan reached its natural duration
  // rather than going through stop_scan().
  ble_presence::resume_continuous_scan();
}

static void apply_activity(const Event& e) {
  g_connection.last_activity_ms = e.at_ms;
  g_connection.bytes_received += e.u.activity.rx_bytes;
  g_total_bytes_received += e.u.activity.rx_bytes;
}

// A pairing the full bond store refused at its start (sweep F189): counted
// for the health log (update(), at most once a minute), and, on the
// recorded link, the owner's pairing session reads failed, as a failed
// authentication's does.
static void apply_store_full(const Event& e) {
  g_store_full_count++;
  if (g_connection.connected && g_connection_handle == e.u.store.conn_handle &&
      g_pairing.state != PAIR_NONE && g_pairing.state != PAIR_COMPLETE) {
    g_pairing.state = PAIR_FAILED;
    if (g_pair_callback) {
      g_pair_callback(&g_pairing);
    }
  }
}

// A record the full bond store did not keep (sweep F189): counted for the
// health log.
static void apply_store_overflow(const Event& /*e*/) {
  g_store_overflow_count++;
}

// After a link's event was dropped (sweep F169). The queue keeps room for a
// link's own events, but a loop task stalled long enough fills that too,
// and a dropped disconnect left the connection recorded (no advertising
// again, no inactivity timeout to drop it) until the next link's events or
// a reboot; a dropped connect left a live link unrecorded. So when the
// count of a link's dropped events has moved, the loop task asks the stack
// itself, once: a recorded link the stack no longer holds on its handle
// (or holds for another address) ended, and is ended here as its event
// would have; then, with no link recorded, a link the stack holds is
// recorded as its connect would have. A pending pairing's answer was
// already safe (answer_pending_pairing() asks the stack the same way). The
// stack's own report of such a link, if it comes after all, finds another
// link or none recorded (apply_disconnect()).
static void reconcile_link() {
  const uint32_t dropped = g_events.dropped_reserved();
  if (dropped == g_link_drops_reconciled) return;
  g_link_drops_reconciled = dropped;
  if (g_server == nullptr) return;
  if (g_connection.connected) {
    const NimBLEConnInfo live = g_server->getPeerInfoByHandle(g_connection_handle);
    if (live.getConnHandle() != g_connection_handle ||
        memcmp(live.getAddress().getBase()->val, g_connection.address, BLE_ADDRESS_LENGTH) != 0) {
      Event end = make_event(BT_EV_DISCONNECT);
      end.u.link.handle = g_connection_handle;
      memcpy(end.u.link.address.val, g_connection.address, BLE_ADDRESS_LENGTH);
      end.u.link.reason = -1;            // the stack's reason was in the dropped event
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
                 "BLE link gone, its end dropped: ended from the stack's record", nullptr);
      apply_disconnect(end);
    }
  }
  if (!g_connection.connected) {
    for (const uint16_t handle : g_server->getPeerDevices()) {
      NimBLEConnInfo live = g_server->getPeerInfoByHandle(handle);
      if (live.getConnHandle() != handle) continue;
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
                 "BLE link up, its start dropped: recorded from the stack's record", nullptr);
      apply_connect(link_event(BT_EV_CONNECT, live));
      break;
    }
  }
}

// One event, on the loop task (update()'s consume of g_events).
static void apply_event(const Event& e) {
  switch (e.type) {
    case BT_EV_CONNECT:         apply_connect(e); break;
    case BT_EV_DISCONNECT:      apply_disconnect(e); break;
    case BT_EV_AUTH_COMPLETE:   apply_auth_complete(e); break;
    case BT_EV_PASSKEY_DISPLAY: apply_passkey_display(e); break;
    case BT_EV_CONFIRM_PASSKEY: apply_confirm_passkey(e); break;
    case BT_EV_SCAN_RESULT:     apply_scan_result(e); break;
    case BT_EV_SCAN_END:        apply_scan_end(e); break;
    case BT_EV_ACTIVITY:        apply_activity(e); break;
    case BT_EV_STORE_FULL:      apply_store_full(e); break;
    case BT_EV_STORE_OVERFLOW:  apply_store_overflow(e); break;
  }
}


// ════════════════════════════════════════════════════════════════════════════
// STATE MANAGEMENT
// ════════════════════════════════════════════════════════════════════════════

// ESP32 NimBLE accepts BLE TX power in the range [-12, +9] dBm. Anything
// outside that band makes setPower fail (or, worse, behave silently in
// unsupported ranges on some IDF versions). Clamp at every boundary
// — NVS load, settings POST — so a corrupted persisted value or a
// validation-skipping API caller can't reach the radio with garbage.
static int8_t clamp_tx_power(int8_t v) {
  if (v < -12) return -12;
  if (v >  9)  return  9;
  return v;
}

// The state from what is running (sweep F170), for when the activity the
// state named ends (a scan, pairing mode, a link): disabled when Bluetooth
// is off, then a link, a scan, pairing mode and advertising, in that
// order, else idle. Before, each of those ends set idle (or connected)
// whatever went on, so a scan that ended while advertising read
// "state": "idle" with "advertising": true. The starts of a link, a scan
// and pairing mode still set their own state; advertising's start sets
// this one (sweep F190), since pairing mode starts advertising itself.
static BluetoothState rest_state() {
  if (!g_settings.enabled) return BT_DISABLED;
  if (g_connection.connected) return BT_CONNECTED;
  if (g_scanning) return BT_SCANNING;
  if (g_pairing.state == PAIR_INITIATED || g_pairing.state == PAIR_PIN_DISPLAYED ||
      g_pairing.state == PAIR_CONFIRMING) {
    return BT_PAIRING;
  }
  if (g_advertising && g_advertising->isAdvertising()) return BT_ADVERTISING;
  return BT_IDLE;
}

static void set_state(BluetoothState new_state) {
  if (g_state == new_state) return;

  BluetoothState old_state = g_state;
  g_state = new_state;

  char detail[64];
  snprintf(detail, sizeof(detail), "%s -> %s",
           state_name(old_state), state_name(new_state));
  log_health(SCV_LOG_DEBUG, SCV_CAT_BLUETOOTH, "BLE state change", detail);
}

// ════════════════════════════════════════════════════════════════════════════
// SETTINGS PERSISTENCE
// ════════════════════════════════════════════════════════════════════════════

// Into `out`, which holds the values a key the device never saved keeps
// (its callers start it from kDefaultSettings): load_saved()'s, on the loop
// task, and init()'s, on its own, into its hand-over (the settings it gives
// the stack, sweep F167). It names none of the channel's state.
static void load_settings(BluetoothSettings* out) {
  NvsMainSession nvs(true);
  if (!nvs.isOpen()) return;

  // Defaults (used when the device has never saved BT settings) come from
  // bt_defaults.h so the first-boot state matches the struct initializer.
  out->enabled = nvs->getBool(NVS_KEY_BT_ENABLED, bt_defaults::ENABLED);
  out->auto_advertise = nvs->getBool(NVS_KEY_BT_AUTO_ADV, bt_defaults::AUTO_ADVERTISE);
  out->allow_pairing = nvs->getBool(NVS_KEY_BT_ALLOW_PAIR, bt_defaults::ALLOW_PAIRING);
  out->require_pin = nvs->getBool(NVS_KEY_BT_REQ_PIN, bt_defaults::REQUIRE_PIN);
  out->tx_power = clamp_tx_power(nvs->getChar(NVS_KEY_BT_TX_PWR, 3));
  out->inactivity_timeout_ms = nvs->getULong(NVS_KEY_BT_TIMEOUT, INACTIVITY_TIMEOUT_MS);
  out->long_range_mode = nvs->getBool(NVS_KEY_BT_LONG_RANGE, bt_defaults::LONG_RANGE);

  size_t name_len = nvs->getBytesLength(NVS_KEY_BT_NAME);
  if (name_len > 0 && name_len <= MAX_DEVICE_NAME_LEN) {
    nvs->getBytes(NVS_KEY_BT_NAME, out->device_name, name_len);
    out->device_name[name_len] = '\0';
  }

}

static void save_settings() {
  NvsMainSession nvs(false);
  if (!nvs.isOpen()) return;

  nvs->putBool(NVS_KEY_BT_ENABLED, g_settings.enabled);
  nvs->putBool(NVS_KEY_BT_AUTO_ADV, g_settings.auto_advertise);
  nvs->putBool(NVS_KEY_BT_ALLOW_PAIR, g_settings.allow_pairing);
  nvs->putBool(NVS_KEY_BT_REQ_PIN, g_settings.require_pin);
  nvs->putChar(NVS_KEY_BT_TX_PWR, g_settings.tx_power);
  nvs->putULong(NVS_KEY_BT_TIMEOUT, g_settings.inactivity_timeout_ms);
  nvs->putBool(NVS_KEY_BT_LONG_RANGE, g_settings.long_range_mode);
  nvs->putBytes(NVS_KEY_BT_NAME, g_settings.device_name, strlen(g_settings.device_name));

}

// Into `out` (MAX_PAIRED_DEVICES entries), `count` and `by_identity`.
// load_saved()'s, on the loop task (the F167 review); like load_settings()
// it names none of the channel's state.
static void load_paired_devices(PairedDevice* out, size_t* count, bool* by_identity) {
  NvsMainSession nvs(true);
  if (!nvs.isOpen()) return;

  size_t data_len = nvs->getBytesLength(NVS_KEY_BT_PAIRED);
  if (data_len > 0 && data_len <= MAX_PAIRED_DEVICES * sizeof(PairedDevice)) {
    nvs->getBytes(NVS_KEY_BT_PAIRED, out, data_len);
    *count = data_len / sizeof(PairedDevice);
  }
  *by_identity = nvs->getBool(NVS_KEY_BT_PAIRED_ID, false);

}

static void save_paired_devices() {
  NvsMainSession nvs(false);
  if (!nvs.isOpen()) return;

  nvs->putBytes(NVS_KEY_BT_PAIRED, g_paired_devices, g_paired_count * sizeof(PairedDevice));
  // Every list saved from here on holds identity addresses (apply_auth_complete).
  nvs->putBool(NVS_KEY_BT_PAIRED_ID, true);
  g_paired_by_identity = true;

}

// A paired list saved before sweep F172 kept each phone's over-the-air
// address, which for a phone using resolvable private addresses (most do)
// is one it has long since dropped, and names no bond: Remove asked NimBLE
// to forget an address it had never stored. Once, the first time the stack
// is up with such a list (init(), after NimBLEDevice::init(): the bond
// store is NimBLE's), the list is rebuilt from the bond store, which keys
// each bond by the phone's identity address. An entry that names a bond
// keeps what it says (name, counts, trust, block); a bond with no entry is
// added under its identity address; an entry that names no bond (an old
// over-the-air address, or a bond NimBLE no longer has) is dropped, as no
// phone can use it. The rebuilt list is saved with NVS_KEY_BT_PAIRED_ID, so
// this runs once. An over-the-air entry cannot be matched to its bond (that
// needs the phone's IRK, which NimBLE-Arduino does not hand out), so its
// trust and block flags start over under the identity entry.
static void migrate_paired_devices() {
  if (g_paired_by_identity) return;
  const int bonds = NimBLEDevice::getNumBonds();
  // A device with no list and no bond (a fresh one) has nothing to rebuild,
  // and nothing is written: its first pairing saves the list, marked.
  if (g_paired_count == 0 && bonds <= 0) return;
  PairedDevice rebuilt[MAX_PAIRED_DEVICES];
  memset(rebuilt, 0, sizeof(rebuilt));
  size_t count = 0;
  size_t kept = 0;
  for (int b = 0; b < bonds && count < MAX_PAIRED_DEVICES; b++) {
    const NimBLEAddress identity = NimBLEDevice::getBondedAddress(b);
    const ble_addr_t* id = identity.getBase();
    PairedDevice* dev = &rebuilt[count++];
    bool found = false;
    for (size_t i = 0; i < g_paired_count; i++) {
      if (memcmp(g_paired_devices[i].address, id->val, BLE_ADDRESS_LENGTH) == 0) {
        *dev = g_paired_devices[i];
        found = true;
        kept++;
        break;
      }
    }
    if (!found) {
      memcpy(dev->address, id->val, BLE_ADDRESS_LENGTH);
      strncpy(dev->name, identity.toString().c_str(), MAX_DEVICE_NAME_LEN);
      dev->name[MAX_DEVICE_NAME_LEN] = '\0';
      dev->security = SEC_BONDED;
    }
    dev->address_type = id->type;
  }
  const size_t dropped = g_paired_count > kept ? g_paired_count - kept : 0;
  memcpy(g_paired_devices, rebuilt, sizeof(g_paired_devices));
  g_paired_count = count;
  save_paired_devices();

  char detail[64];
  snprintf(detail, sizeof(detail), "%u kept, %u added, %u dropped", (unsigned)kept,
           (unsigned)(count - kept), (unsigned)dropped);
  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "Paired list rebuilt from the bond store", detail);
}

// The paired list follows the bond store (sweep F189): each boot, once the
// loop task holds the stack (adopt_init_result()), an entry whose bond the
// store no longer holds is dropped, and the list saved when one was. Since
// F189 the store evicts no bond (StoreCallbacks) and only Remove and Clear
// delete one, keeping the list in step; this heals a list saved while the
// store's default evicted bonds behind it (F172's rebuild ran once, on the
// first boot after it). A list that matches the store saves nothing.
static void drop_entries_without_bond() {
  size_t kept = 0;
  for (size_t i = 0; i < g_paired_count; i++) {
    if (NimBLEDevice::isBonded(paired_identity(g_paired_devices[i]))) {
      if (kept != i) g_paired_devices[kept] = g_paired_devices[i];
      kept++;
    }
  }
  if (kept == g_paired_count) return;
  const size_t dropped = g_paired_count - kept;
  for (size_t i = kept; i < MAX_PAIRED_DEVICES; i++) {
    memset(&g_paired_devices[i], 0, sizeof(PairedDevice));
  }
  g_paired_count = kept;
  save_paired_devices();
  char detail[24];
  snprintf(detail, sizeof(detail), "%u dropped", (unsigned)dropped);
  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "Paired list: entries without a bond dropped", detail);
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
    log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "Disconnecting due to inactivity", nullptr);
    // The recorded link only, the one whose activity this measured (the
    // F171 review). disconnect() drops every link on the server, and since
    // F171 the channel sees the FULL profile's links too: a second link
    // beside the recorded one (Opera's GATT client, a BLE OTA or
    // provisioning session, whose characteristics post no activity here)
    // was cut with it. On DEV no second link comes up (the channel stops
    // advertising while connected), so nothing changes there.
    if (g_server) {
      g_server->disconnect(g_connection_handle);
    }
  }
}

static void handle_scan_timeout() {
  if (!g_scanning) return;

  if (g_scan_duration_ms > 0 && millis() - g_scan_start_ms >= g_scan_duration_ms) {
    stop_scan();
  }
}

static DeviceType detect_device_type(const NimBLEAdvertisedDevice* device) {
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

// A refusal of the attempt under way, for any task (sweep F167): written
// whole into the next slot, then its address published. init()'s alone,
// under its latch. nullptr clears it.
static void set_init_fail_reason(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_init_fail_reason(const char* fmt, ...) {
  if (fmt == nullptr) {
    __atomic_store_n(&g_init_fail_reason, (const char*)"", __ATOMIC_RELEASE);
    return;
  }
  g_init_fail_slot = (g_init_fail_slot + 1) % INIT_FAIL_SLOTS;
  char* text = g_init_fail_text[g_init_fail_slot];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(g_init_fail_text[0]), fmt, args);
  va_end(args);
  __atomic_store_n(&g_init_fail_reason, (const char*)text, __ATOMIC_RELEASE);
}

bool init() {
  if (__atomic_load_n(&g_stack_up, __ATOMIC_ACQUIRE)) return true;

  // Concurrency latch: init() can now be entered from the boot bring-up
  // worker task AND the HTTP task (a user tapping Enable/Advertise/Pair
  // during the bring-up window has the handler bring the stack up, F111).
  // NimBLE init is not reentrant, so exactly one caller may run the body;
  // the loser backs off with false and the handler answers with the init
  // error, while GET /api/bluetooth reads "initializing" (the loop task's
  // view, from init_running()). Cleared on every exit path (RAII).
  bool expected = false;
  if (!__atomic_compare_exchange_n(&g_init_in_progress, &expected, true,
                                   false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return false;
  }
  struct InitLatchClear {
    ~InitLatchClear() {
      __atomic_store_n(&g_init_in_progress, false, __ATOMIC_RELEASE);
    }
  } latch_clear;
  // Another caller brought it up between the first look and the latch.
  if (__atomic_load_n(&g_stack_up, __ATOMIC_ACQUIRE)) return true;

  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "Initializing BLE", nullptr);

  // The saved settings the stack starts with (its name, TX power and PHY),
  // read into the hand-over on this task (sweep F167), never into the loop
  // task's g_settings: the loop task loaded those at its first pass
  // (load_saved()) and its commands change them meanwhile. A command that
  // changes the TX power or the PHY while init() runs is applied to the
  // stack when the loop task takes the hand-over (adopt_init_result()).
  Bringup& b = g_bringup;
  memset(&b, 0, sizeof(b));
  b.applied = kDefaultSettings;
  load_settings(&b.applied);

  // Initialize NimBLE. This is the single NimBLEDevice::init() owner for the
  // firmware: it runs before ble_manager::init() in setup() and owns the GAP
  // device name, TX power, MTU and security config. NimBLE 2.x init() returns
  // false when the controller/host stack can't come up (BT compiled out, radio
  // unavailable, or a coexistence/heap failure). Previously the result was
  // ignored and the code marched on to createServer() — which then returned
  // null and crashed on the first server->... deref. Treat a failed init as a
  // hard failure so the caller degrades gracefully (the documented contract).
  // Fail closed on low memory BEFORE bringing the controller up. The malloc
  // failure inside NimBLEDevice::init() asserts and panics rather than
  // returning false, so the graceful-false path below never runs on a
  // no-PSRAM build — the device just boot-loops. Skip the stack instead and
  // leave the radio off; the panel reports Bluetooth idle, the AP stays up.
  // Only gate when the stack ISN'T already up: another module (e.g. the CSI
  // BLE Scout via csi_integration::init) may have paid the controller
  // allocation earlier, in which case NimBLEDevice::init() below is a no-op and
  // refusing here would needlessly kill the pairing/GATT server.
  if (!NimBLEDevice::isInitialized()) {
    size_t largest = 0;
    size_t total   = 0;
    if (!ble_heap_guard::can_init(&largest, &total)) {
      char detail[96];
      snprintf(detail, sizeof(detail),
               "largest block %uB (need %lu), total free %uB (need %lu)",
               (unsigned)largest, bt_defaults::MIN_INIT_FREE_BLOCK,
               (unsigned)total, bt_defaults::MIN_INIT_TOTAL_FREE);
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
                 "BLE not started: insufficient heap", detail);
      set_init_fail_reason(
          "Not started: internal RAM too low (largest block %u KB/%lu KB, free %u KB/%lu KB)",
          (unsigned)(largest / 1024), bt_defaults::MIN_INIT_FREE_BLOCK / 1024,
          (unsigned)(total / 1024), bt_defaults::MIN_INIT_TOTAL_FREE / 1024);
      return false;
    }
  }
  if (!NimBLEDevice::init(b.applied.device_name)) {
    log_health(SCV_LOG_ERROR, SCV_CAT_BLUETOOTH, "NimBLE init failed", nullptr);
    set_init_fail_reason("%s", "NimBLE stack init failed (controller/host bring-up)");
    return false;
  }
  // The bond store's status is the channel's to answer (sweep F189): a full
  // store refuses a new pairing instead of evicting a bond behind the owner
  // (StoreCallbacks). Before any server exists, so before any pairing.
  NimBLEDevice::setDeviceCallbacks(&g_store_callbacks);
  // NimBLE 2.x takes the dBm value directly (int8_t). Don't pass the
  // ESP_PWR_LVL_* enum here — those values are indexes (e.g. P3 == 7), not
  // dBm, and would set the radio to a different power than intended. This is
  // where the boot's TX power is set (set_tx_power(), the owner's, on the loop
  // task, is the other); ble_manager no longer overrides it (it used to bump
  // every combined build to +9 dBm, ignoring this NVS setting).
  NimBLEDevice::setPower(b.applied.tx_power);

  // Bump default ATT MTU to 247 (244-byte payload). The default is 23
  // (20-byte payload), which fragments every JSON status read into 3+ ATT
  // packets and roughly triples connection-event time on the radio. 247 is
  // the largest a single LE Data Length Extension packet carries without
  // additional fragmentation; both iOS and modern Android accept it.
  NimBLEDevice::setMTU(247);

  // Long-range mode: declare a preference for LE Coded PHY on new
  // connections. The actual PHY upgrade happens after the link is up,
  // via a PHY update request in onConnect — discovery still uses 1M.
  // Bit masks: 0x01 = 1M, 0x02 = 2M, 0x04 = Coded.
  if (b.applied.long_range_mode) {
    NimBLEDevice::setDefaultPhy(0x04, 0x04);
  } else {
    NimBLEDevice::setDefaultPhy(0x01 | 0x02, 0x01 | 0x02);
  }

  // Set security
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, MITM, SC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO);

  // Create server. createServer() returns null if the host stack rejected the
  // request (out of GATT resources / not initialized). Bail out cleanly rather
  // than dereferencing null on the createService() line below, and release the
  // stack we just brought up so a later retry starts from a clean slate.
  NimBLEServer* server = NimBLEDevice::createServer();
  if (!server) {
    log_health(SCV_LOG_ERROR, SCV_CAT_BLUETOOTH, "NimBLE createServer null", nullptr);
    set_init_fail_reason("%s", "NimBLE createServer failed (host stack rejected)");
    NimBLEDevice::deinit(true);
    return false;
  }
  // NimBLE keeps one set of server callbacks per server, and on the FULL
  // profile Opera (ble_opera.h) wants that server's too. Both go through the
  // one dispatcher (ble_server_dispatch.h, sweep F171). Here it only goes on
  // the server, with no pairing owner yet: until the loop task takes this
  // bring-up (adopt_init_result(), sweep F167) and hands it the channel's
  // callbacks, a Numeric Comparison is answered no and the passkey shown is
  // one nobody sees, never NimBLE's default yes and 123456. So no event of
  // the channel's server callbacks reaches the loop task before it holds
  // the state they apply to.
  ble_server_dispatch::attach(server);

  // Create service
  NimBLEService* service = server->createService(SERVICE_UUID);

  // Create characteristics
  NimBLECharacteristic* status_char = service->createCharacteristic(
    STATUS_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  status_char->setCallbacks(&g_char_callbacks);

  NimBLECharacteristic* command_char = service->createCharacteristic(
    COMMAND_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  command_char->setCallbacks(&g_char_callbacks);

  NimBLECharacteristic* notify_char = service->createCharacteristic(
    NOTIFY_CHAR_UUID,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::INDICATE
  );

  // Start service
  service->start();

  // Register the OTA service on the same NimBLE server. It exposes its own
  // GATT service UUID so peers can discover and skip it independently of
  // the primary control surface, and so a half-completed OTA can't disturb
  // the control characteristics.
  ble_ota::init(server, SECURACV_OTA_RELEASE_PUBKEY);

  // Offline console: read-only JSON snapshot of device state. Requires a
  // bonded link (the characteristic carries READ_ENC + READ_AUTHEN), so
  // a paired phone can pull `/api/status`-shaped data even when WiFi is
  // down.
  ble_console::init(server);

  // BLE WiFi provisioning: scan + creds + state characteristics so a
  // paired phone can configure home WiFi without the captive AP. The
  // creds characteristic is write-only and rate-limited; reads of the
  // others require READ_ENC + READ_AUTHEN.
  ble_provision::init(server);

  // Read-only health-log export. Bonded peers can paginate through the
  // ring buffer over BLE for forensic recovery / on-site triage when
  // canary.local is unreachable.
  ble_log_export::init(server);

  // Read-only witness-chain export. Surfaces the chain head + last
  // signed record so a paired phone can independently verify the
  // device's claimed chain state with the device's pubkey, no WiFi
  // path required.
  ble_witness_export::init(server);

  // SIG Standard Profiles — Device Information Service (manufacturer,
  // model, fw/hw/sw revision, serial), Battery Service, GAP Appearance.
  // Without these the device shows as "Unknown" in the iOS / Android
  // BT pane; with them it gets a camera icon and proper metadata. The
  // serial defaults to the BLE local address if set_device_metadata()
  // wasn't called before init() — never empty in the on-air payload.
  {
    const char* serial_for_dis = g_meta_serial;
    char serial_fallback[18] = {0};
    if (serial_for_dis[0] == '\0') {
      NimBLEAddress addr = NimBLEDevice::getAddress();
      snprintf(serial_fallback, sizeof(serial_fallback), "%s", addr.toString().c_str());
      serial_for_dis = serial_fallback;
    }
    ble_standard_profiles::register_all(
      server,
      g_meta_manufacturer,
      g_meta_model,
      serial_for_dis,
      g_meta_fw_revision,
      g_meta_hw_revision,
      g_meta_sw_revision
    );
  }

  // Set up advertising
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  // Note: setScanResponse(bool) is deprecated in NimBLE 2.x
  // Scan response is automatically enabled when service UUIDs are added
  // Connection params are managed internally by NimBLE

  // Set up scanner
  NimBLEScan* scanner = NimBLEDevice::getScan();
  scanner->setScanCallbacks(&g_scan_callbacks);
  scanner->setActiveScan(true);
  scanner->setInterval(SCAN_INTERVAL_MS);
  scanner->setWindow(SCAN_WINDOW_MS);

  // Bring up the always-on presence sensor. The user-triggered scan in
  // start_scan() preempts this; resume_continuous_scan() unwinds the swap
  // when the user scan ends. ble_presence is read-only (no advertise, no
  // connect, just listen) so it adds no attack surface beyond what we
  // already have for legitimate scan results. (Before the hand-over below,
  // so the loop task, which pauses and resumes it, finds it up.)
  ble_presence::init();
  ble_presence::start();

  // Hand the result to the loop task (sweep F167): the objects, then the
  // publish, the last write to g_bringup; then the flag other tasks read.
  // The auto-advertise and the paired list's rebuild are the loop task's
  // (adopt_init_result()), on its next pass, which runs before any command
  // a handler submits after this returns; the advertising itself waits for
  // the sketch's bring-up worker, if it is running, to finish registering
  // its services (start_advertising(), the F167 review).
  b.server = server;
  b.service = service;
  b.status_char = status_char;
  b.command_char = command_char;
  b.notify_char = notify_char;
  b.advertising = advertising;
  b.scanner = scanner;
  __atomic_store_n(&g_bringup_ready, true, __ATOMIC_RELEASE);
  set_init_fail_reason(nullptr);
  __atomic_store_n(&g_stack_up, true, __ATOMIC_RELEASE);

  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE initialized", b.applied.device_name);
  return true;
}

// Whether init() is under way on some task (its latch is held): the loop
// task's view reads "initializing" meanwhile, as GET /api/bluetooth did
// when init() set the state itself.
static bool init_running() {
  return __atomic_load_n(&g_init_in_progress, __ATOMIC_ACQUIRE);
}

// The loop task, first in every update() pass (the F167 review): the saved
// settings and the paired list, loaded once, before any command or event
// of the pass. They are the loop task's from here on whether or not the
// stack ever comes up, so a command run before it is up (or while init()
// runs on another task) changes the settings the device saved, never the
// compiled defaults, and saves what it changed and nothing else. Before,
// they were loaded by init(): first into g_settings on init()'s own task,
// then (F167) into its hand-over, which overwrote at adoption whatever the
// owner's commands had done meanwhile; and a command run before init()
// saved the defaults over the stored settings.
static void load_saved() {
  if (g_saved_loaded) return;
  g_saved_loaded = true;
  BluetoothSettings saved = kDefaultSettings;
  load_settings(&saved);
  g_settings = saved;
  load_paired_devices(g_paired_devices, &g_paired_count, &g_paired_by_identity);
}

// The loop task, in every update() pass after load_saved() (sweep F167):
// takes the bring-up's result once it is published, and does there what
// init() used to do on its own task: the NimBLE objects become the loop
// task's; a TX power or PHY the owner changed while init() ran reaches the
// stack (init() set the saved ones); a list saved before F172 is rebuilt
// from the bond store (it needs the stack up, which it now is), and the
// list follows the store (F189); the channel's server callbacks become the
// dispatcher's pairing owner, so the stack's events reach this task from
// here on and never before; and with auto-advertise on, Bluetooth turns on
// and advertises (once the sketch's bring-up worker has finished
// registering its services, start_advertising()). The settings and the
// list stay as the loop task holds them (the F167 review). Before any
// command of the pass, so a handler that brought the stack up
// (bring_up()) and then submitted finds it taken.
static void adopt_init_result() {
  if (g_initialized || !__atomic_load_n(&g_bringup_ready, __ATOMIC_ACQUIRE)) return;
  const Bringup& b = g_bringup;
  g_server = b.server;
  g_service = b.service;
  g_status_char = b.status_char;
  g_command_char = b.command_char;
  g_notify_char = b.notify_char;
  g_advertising = b.advertising;
  g_scanner = b.scanner;
  g_initialized = true;
  // The state from what runs (F170): disabled when Bluetooth is saved off,
  // idle otherwise (it read idle either way before F167).
  set_state(rest_state());

  // What the owner changed while init() ran, on the stack init() set up
  // with the saved values (the name NimBLE advertises takes effect at the
  // next boot, as a name change always has).
  if (g_settings.tx_power != b.applied.tx_power) {
    NimBLEDevice::setPower(g_settings.tx_power);
  }
  if (g_settings.long_range_mode != b.applied.long_range_mode) {
    if (g_settings.long_range_mode) {
      NimBLEDevice::setDefaultPhy(0x04, 0x04);
    } else {
      NimBLEDevice::setDefaultPhy(0x01 | 0x02, 0x01 | 0x02);
    }
  }

  // A paired list saved before sweep F172 is rebuilt from the bond store
  // (once; it needs the stack up). Then the list follows the store (F189).
  migrate_paired_devices();
  drop_entries_without_bond();

  // init() already put the dispatcher on the server (attach()); this only
  // names the channel its pairing owner, an atomic store the NimBLE host
  // task reads. Not install(): that calls NimBLEServer::setCallbacks()
  // again, from this task, while on FULL the bring-up worker's Opera init
  // calls it on its own (the F167 review).
  ble_server_dispatch::set_owner(ble_server_dispatch::kPairing, &g_server_callbacks);

  // Auto-start advertising if enabled. NOTE: init() is only ever called
  // AFTER the provisioning join window has cleared (the loop's
  // ble_discovery_start_if_due gate — the whole BLE bring-up is deferred
  // there for internal-RAM budgeting), or by the owner's own tap, so
  // transmitting immediately is safe. While the sketch's bring-up worker
  // still registers services, start_advertising() holds the start until it
  // finishes (the F167 review).
  if (g_settings.enabled && g_settings.auto_advertise) {
    enable();
    start_advertising();
  }
}

// The sketch's BLE bring-up worker (canary_wap.ino), told by the sketch on
// the loop task (the F167 review): see g_bringup_worker_running.
void bringup_worker_started() {
  g_bringup_worker_running = true;
}

void bringup_worker_finished() {
  g_bringup_worker_running = false;
  if (g_advertise_after_bringup) {
    g_advertise_after_bringup = false;
    start_advertising();
  }
}

[[maybe_unused]] static void deinit() {
  if (!g_initialized) return;

  // Tear down the presence sensor before NimBLE goes away so its scanner
  // pointer doesn't dangle.
  ble_presence::deinit();

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
  __atomic_store_n(&g_bringup_ready, false, __ATOMIC_RELEASE);
  __atomic_store_n(&g_stack_up, false, __ATOMIC_RELEASE);
  set_state(BT_DISABLED);

  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE deinitialized", nullptr);
}

// Any task: whether the stack is up and handed over (sweep F167). The loop
// task takes it on its next pass (g_initialized, its own).
bool is_initialized() {
  return __atomic_load_n(&g_stack_up, __ATOMIC_ACQUIRE);
}

// Any task: the published refusal (sweep F167), whole.
const char* init_fail_reason() {
  return __atomic_load_n(&g_init_fail_reason, __ATOMIC_ACQUIRE);
}

void set_device_metadata(const char* fw_revision, const char* serial) {
  // Caller hands ownership of the strings — we copy. Empty / nullptr
  // leaves the existing default in place. Must be invoked BEFORE init();
  // init() reads these into the DIS characteristics during service
  // creation, after which they're cached on the BLE stack and changes
  // here have no effect until the next reinit.
  if (fw_revision && fw_revision[0]) {
    strncpy(g_meta_fw_revision, fw_revision, sizeof(g_meta_fw_revision) - 1);
    g_meta_fw_revision[sizeof(g_meta_fw_revision) - 1] = '\0';
  }
  if (serial && serial[0]) {
    strncpy(g_meta_serial, serial, sizeof(g_meta_serial) - 1);
    g_meta_serial[sizeof(g_meta_serial) - 1] = '\0';
  }
}

static bool enable() {
  // Never brings the stack up: NimBLE init can block past the loop task's
  // watchdog, and this runs there (BT_CMD_ENABLE). init() calls this once it
  // is up; a REST handler calls init() first, on its own task (sweep F111).
  if (!g_initialized) return false;

  g_settings.enabled = true;
  save_settings();

  if (g_state == BT_DISABLED) {
    set_state(BT_IDLE);
  }

  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE enabled", nullptr);
  return true;
}

static void disable() {
  // A pairing in progress ends first: a Numeric Comparison awaiting the
  // owner is answered no while its link is still up (it was left pending,
  // and showing, while update() returned early for the disabled channel).
  cancel_pairing();
  stop_advertising();
  stop_scan();
  disconnect();

  g_settings.enabled = false;
  save_settings();
  set_state(BT_DISABLED);

  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE disabled", nullptr);
}

// The loop task's (run_command()). Another task reads read_enabled(), the
// settings the last pass published (sweep F210).
static bool is_enabled() {
  return g_settings.enabled;
}

static bool start_advertising() {
  if (!g_initialized || !g_settings.enabled) return false;
  if (g_connection.connected) return false;  // Can't advertise while connected

  // The sketch's bring-up worker is still registering GATT services on the
  // server NimBLEAdvertising::start() would start (NimBLEServer::start()
  // walks the service list the worker appends to, from another task): the
  // start is held, and made when it finishes (bringup_worker_finished(), the
  // F167 review). Answered as started: it follows within a loop pass or
  // two, and pairing mode reads pairing meanwhile.
  if (g_bringup_worker_running) {
    g_advertise_after_bringup = true;
    return true;
  }

  if (g_advertising && !g_advertising->isAdvertising()) {
    g_advertising->start();
    g_advertising_start_ms = millis();
    // The state from what runs (sweep F190), not BT_ADVERTISING: pairing
    // mode starts the advertiser when the owner had stopped it, and setting
    // advertising here hid pairing mode from GET /api/bluetooth until it
    // ended. So pairing mode reads pairing, a scan scanning, a link
    // connected, and advertising alone reads advertising (rest_state()'s
    // order, F170).
    set_state(rest_state());
    log_health(SCV_LOG_DEBUG, SCV_CAT_BLUETOOTH, "BLE advertising started", nullptr);
  }

  return true;
}

static void stop_advertising() {
  g_advertise_after_bringup = false;   // a start held for the bring-up worker, withdrawn
  if (g_advertising && g_advertising->isAdvertising()) {
    g_advertising->stop();
    g_advertising_total_ms += millis() - g_advertising_start_ms;
    if (g_state == BT_ADVERTISING) {
      set_state(BT_IDLE);
    }
    log_health(SCV_LOG_DEBUG, SCV_CAT_BLUETOOTH, "BLE advertising stopped", nullptr);
  }
}

bool is_advertising() {
  // Any task (the self-test): NimBLE's own state, through its own object
  // once the stack is up, not the loop task's g_advertising (sweep F167).
  return is_initialized() && NimBLEDevice::getAdvertising()->isAdvertising();
}

static bool start_scan(uint32_t duration_ms) {
  if (!g_initialized || !g_settings.enabled) return false;
  if (g_scanning) return false;

  // Hand the radio over from the always-on presence sensor. NimBLE has one
  // scanner singleton — we pause continuous mode, swap our own callbacks
  // and aggressive duty cycle in via setActiveScan + the existing
  // g_scan_callbacks, then resume continuous after the user scan ends.
  ble_presence::pause_for_user_scan();
  g_scanner->setScanCallbacks(&g_scan_callbacks);
  g_scanner->setActiveScan(true);  // request scan responses for richer UI
  g_scanner->setInterval(SCAN_INTERVAL_MS);
  g_scanner->setWindow(SCAN_WINDOW_MS);

  // Clear previous results
  clear_scan_results();

  g_scan_start_ms = millis();
  g_scan_duration_ms = duration_ms;
  g_scanning = true;
  set_state(BT_SCANNING);

  // Start scan (non-blocking with callback). NimBLE-Arduino 2.x takes the
  // scan duration in MILLISECONDS (it was seconds in 1.x), so pass duration_ms
  // directly — dividing by 1000 here made the controller stop scanning ~1000x
  // too early while g_scanning stayed true until the manual timeout.
  g_scanner->start(duration_ms, false);

  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "BLE scan started",
             String(duration_ms / 1000).c_str());
  return true;
}

static void stop_scan() {
  if (g_scanner && g_scanning) {
    g_scanner->stop();
    g_scanning = false;
    if (g_state == BT_SCANNING) {
      set_state(rest_state());   // F170: advertising, a link or pairing mode may go on
    }
    log_health(SCV_LOG_DEBUG, SCV_CAT_BLUETOOTH, "BLE scan stopped", nullptr);
    // Hand the radio back to the always-on presence loop.
    ble_presence::resume_continuous_scan();
  }
}

static void clear_scan_results() {
  memset(g_scanned_devices, 0, sizeof(g_scanned_devices));
  g_scanned_count = 0;
}

static bool start_pairing() {
  if (!g_initialized || !g_settings.enabled) return false;
  if (!g_settings.allow_pairing) return false;

  g_pairing.state = PAIR_INITIATED;
  g_pairing.started_ms = millis();
  g_pairing.pin_displayed = false;
  g_pairing.user_confirmed = false;

  set_state(BT_PAIRING);

  // Make sure we're advertising
  start_advertising();

  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "Pairing mode started", nullptr);

  if (g_pair_callback) {
    g_pair_callback(&g_pairing);
  }

  return true;
}

// Whether the link a pending Numeric Comparison belongs to is still up: the
// stack's own record of its handle (ble_gap_conn_find) names the same peer.
// A link whose disconnect never reached the loop task (a full event queue)
// fails it, and so does a new link that took the handle.
static bool pending_link_is_up() {
  if (g_pending_pair_info == nullptr || g_server == nullptr) return false;
  const uint16_t handle = g_pending_pair_info->getConnHandle();
  const NimBLEConnInfo live = g_server->getPeerInfoByHandle(handle);
  return live.getConnHandle() == handle &&
         live.getAddress() == g_pending_pair_info->getAddress();
}

// The pending Numeric Comparison let go unanswered (its link ended).
static void drop_pending_pairing() {
  delete g_pending_pair_info;
  g_pending_pair_info = nullptr;
  g_pending_pair_active = false;
}

// The one way the pending Numeric Comparison is answered: `accept` goes to
// its link when that link is still up (pending_link_is_up()), never to
// another; then the copy is deleted. True when the answer was given.
static bool answer_pending_pairing(bool accept) {
  bool answered = false;
  if (g_pending_pair_info != nullptr && pending_link_is_up()) {
    NimBLEDevice::injectConfirmPasskey(*g_pending_pair_info, accept);
    answered = true;
  }
  drop_pending_pairing();
  return answered;
}

static void cancel_pairing() {
  // Drain any pending Numeric-Comparison so NimBLE doesn't sit indefinitely
  // waiting on injectConfirmPasskey. A reject closes the bond attempt cleanly.
  if (g_pending_pair_active || g_pending_pair_info) {
    (void)answer_pending_pairing(false);
  }
  if (g_pairing.state == PAIR_NONE) return;

  g_pairing.state = PAIR_NONE;
  memset(&g_pairing, 0, sizeof(g_pairing));

  if (g_state == BT_PAIRING) {
    set_state(rest_state());   // F170: start_pairing() started advertising, which goes on
  }

  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "Pairing canceled", nullptr);
}

static bool confirm_pairing(uint32_t pin) {
  if (g_pairing.state != PAIR_CONFIRMING) return false;
  if (!g_pending_pair_active) return false;
  // The heap copy can be null if the `new` in onConfirmPassKey failed (ESP32
  // Arduino builds run with exceptions off, so operator new returns nullptr
  // rather than throwing). Bail before dereferencing — we have nothing to
  // inject and NimBLE will time the bond attempt out on its own.
  if (!g_pending_pair_info) {
    g_pending_pair_active = false;
    return false;
  }

  // The SPA sends back the same six digits we showed it. A mismatch means
  // either a bug in the SPA, a stale request, or an active attacker trying
  // to coerce a yes — all warrant a reject.
  if (pin != g_pairing.pin_code) {
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "Pairing PIN mismatch — rejecting", nullptr);
    (void)answer_pending_pairing(false);
    g_pairing.state = PAIR_FAILED;
    if (g_pair_callback) g_pair_callback(&g_pairing);
    return false;
  }

  // The yes goes only to the link the six digits came from: when that link
  // is gone, or another took its handle, nothing is answered and the
  // confirm fails (the new link's own digits come in their own event).
  if (!answer_pending_pairing(true)) {
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "Pairing link gone — confirmation not sent", nullptr);
    g_pairing.state = PAIR_FAILED;
    if (g_pair_callback) g_pair_callback(&g_pairing);
    return false;
  }
  g_pairing.user_confirmed = true;
  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH,
             "Pairing PIN confirmed by user", nullptr);
  return true;
}

static bool reject_pairing() {
  if (g_pending_pair_active || g_pending_pair_info) {
    (void)answer_pending_pairing(false);
  }
  if (g_pairing.state == PAIR_NONE) return false;

  g_pairing.state = PAIR_FAILED;
  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "Pairing rejected", nullptr);

  if (g_pair_callback) {
    g_pair_callback(&g_pairing);
  }

  cancel_pairing();
  return true;
}

static bool disconnect() {
  if (!g_connection.connected) return false;

  if (g_server) {
    // Disconnect all clients
    for (auto& client : g_server->getPeerDevices()) {
      g_server->disconnect(client);
    }
  }

  return true;
}

// A paired entry's identity address in the stack's own form (sweep F172):
// the list keeps the bytes as getBase()->val holds them and the type beside
// them, so NimBLEAddress(ble_addr_t) names the key NimBLE's bond store
// holds. (The byte-array constructor reverses the bytes: it takes the
// printed order.)
static NimBLEAddress paired_identity(const PairedDevice& dev) {
  ble_addr_t id;
  id.type = dev.address_type;
  memcpy(id.val, dev.address, BLE_ADDRESS_LENGTH);
  return NimBLEAddress(id);
}

// NimBLE will not forget a bond that carries the phone's identity resolving
// key (IRK) while it advertises or scans: ble_gap_unpair() (ble_gap.c, the
// same in NimBLE-Arduino 2.3.8 and 2.5.0) answers BLE_HS_EBUSY and keeps
// the bond, since the IRK must leave the controller's resolving list, which
// cannot change meanwhile. A phone that uses resolvable private addresses
// (most do) hands its IRK over, and the WAP nearly always does one or the
// other: the presence sensor scans without end (ble_presence), advertising
// is on by default, and on the FULL profile Opera's fleet-link beacon is on
// the air. So a bond is forgotten with the radio quiet for the call (the
// F172 review: Remove answered ok, dropped the entry and left the bond, and
// the phone came back encrypted with no owner asked). Quieting it ends the
// owner's scan (stop_scan(), which hands the scanner back to the presence
// loop), pauses the presence loop and stops the shared advertiser, Opera's
// beacon with it; restore_radio() starts the advertiser again if it was on
// and resumes the presence loop. Both on the loop task, inside one command.
struct QuietRadio {
  bool advertising;   // the shared advertiser was on: started again after
};

static QuietRadio quiet_radio() {
  QuietRadio q = {false};
  if (g_scanning) {
    stop_scan();
  }
  ble_presence::pause_for_user_scan();
  if (g_advertising && g_advertising->isAdvertising()) {
    g_advertising->stop();
    q.advertising = true;
  }
  return q;
}

static void restore_radio(const QuietRadio& q) {
  if (q.advertising && g_advertising && !g_advertising->isAdvertising()) {
    // Held while the sketch's bring-up worker registers services, as
    // start_advertising() holds its own (the F167 review).
    if (g_bringup_worker_running) {
      g_advertise_after_bringup = true;
    } else {
      g_advertising->start();
    }
  }
  ble_presence::resume_continuous_scan();
}

// Forgets the bond NimBLE keeps under `identity`, with the radio quiet. True
// when no bond is left under it: deleted now, or there was none (an entry a
// bond never backed, which ble_gap_unpair() answers with the store's
// error). False when the stack kept it (an advertiser or a scan another
// task started again meanwhile: Opera's onConnect, a chirp). ble_gap_unpair()
// also ends a live link to that address; its disconnect arrives as usual.
static bool forget_bond(const NimBLEAddress& identity) {
  const QuietRadio q = quiet_radio();
  const bool deleted = NimBLEDevice::deleteBond(identity);
  restore_radio(q);
  return deleted || !NimBLEDevice::isBonded(identity);
}

static bool remove_paired_device(const uint8_t* address, Refusal* refusal) {
  // The bond store is NimBLE's, reachable once the stack is up (the F167
  // review: the list is loaded at the loop task's first pass, so before the
  // stack is up an entry is found whose bond no call can reach yet, and
  // forget_bond() would read the unreachable bond as gone). Nothing changes.
  if (!g_initialized) {
    *refusal = BT_REFUSED_NOT_UP;
    return false;
  }
  for (size_t i = 0; i < g_paired_count; i++) {
    if (memcmp(g_paired_devices[i].address, address, BLE_ADDRESS_LENGTH) == 0) {
      // Forget the bond first (sweep F172), by the identity address the list
      // keeps. When the stack keeps it, the entry stays (the phone could
      // still come back without pairing, and the list says so) and the owner
      // is told; nothing is saved.
      if (!forget_bond(paired_identity(g_paired_devices[i]))) {
        log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
                   "Paired device not removed: the stack kept its bond", nullptr);
        *refusal = BT_REFUSED_BOND_KEPT;
        return false;
      }

      // Shift remaining devices
      for (size_t j = i; j < g_paired_count - 1; j++) {
        g_paired_devices[j] = g_paired_devices[j + 1];
      }
      g_paired_count--;
      memset(&g_paired_devices[g_paired_count], 0, sizeof(PairedDevice));

      save_paired_devices();
      log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "Paired device removed", nullptr);
      return true;
    }
  }
  return false;
}

static bool clear_all_paired_devices(Refusal* refusal) {
  // As Remove (the F167 review): with the stack down no bond can be
  // deleted, and every entry would read unbonded and be dropped, saving an
  // empty list over the bonds NimBLE still keeps. Nothing changes.
  if (!g_initialized) {
    *refusal = BT_REFUSED_NOT_UP;
    return false;
  }
  // Every bond NimBLE keeps, with the radio quiet once for all of them (see
  // quiet_radio()).
  const QuietRadio q = quiet_radio();
  for (int i = NimBLEDevice::getNumBonds() - 1; i >= 0; i--) {
    NimBLEDevice::deleteBond(NimBLEDevice::getBondedAddress(i));
  }
  restore_radio(q);

  // The entries of bonds the stack kept stay listed; the rest go.
  size_t kept = 0;
  for (size_t i = 0; i < g_paired_count; i++) {
    if (NimBLEDevice::isBonded(paired_identity(g_paired_devices[i]))) {
      g_paired_devices[kept++] = g_paired_devices[i];
    }
  }
  for (size_t i = kept; i < MAX_PAIRED_DEVICES; i++) {
    memset(&g_paired_devices[i], 0, sizeof(PairedDevice));
  }
  g_paired_count = kept;
  save_paired_devices();

  const int left = NimBLEDevice::getNumBonds();
  if (left > 0) {
    log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH,
               "Paired devices not all cleared: the stack kept bonds", String(left).c_str());
    *refusal = BT_REFUSED_BOND_KEPT;
    return false;
  }
  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "All paired devices cleared", nullptr);
  return true;
}

static bool set_device_trusted(const uint8_t* address, bool trusted) {
  for (size_t i = 0; i < g_paired_count; i++) {
    if (memcmp(g_paired_devices[i].address, address, BLE_ADDRESS_LENGTH) == 0) {
      g_paired_devices[i].trusted = trusted;
      save_paired_devices();
      return true;
    }
  }
  return false;
}

static bool set_device_blocked(const uint8_t* address, bool blocked) {
  for (size_t i = 0; i < g_paired_count; i++) {
    if (memcmp(g_paired_devices[i].address, address, BLE_ADDRESS_LENGTH) == 0) {
      g_paired_devices[i].blocked = blocked;
      save_paired_devices();
      return true;
    }
  }
  return false;
}

static bool set_settings(const BluetoothSettings& settings) {
  // Whether Bluetooth was on, read before the assignment (sweep F144): the
  // old test compared the new value with is_enabled() after it, which read
  // the value just assigned, so neither branch ever ran and enabled:false
  // left advertising, scans and links running until the next boot.
  const bool was_enabled = g_settings.enabled;
  g_settings = settings;
  // The settings struct comes from a JSON POST — the API parser doesn't
  // range-check tx_power, so clamp here before it goes to NVS or the radio.
  g_settings.tx_power = clamp_tx_power(g_settings.tx_power);

  // Apply the change of state the way BT_CMD_ENABLE and BT_CMD_DISABLE do;
  // each saves the settings. enable() never brings the stack up: a POST
  // that turns Bluetooth on has the handler do that first (bring_up()), and
  // run_command() refuses it while the stack is down.
  if (g_settings.enabled && !was_enabled) {
    if (!enable()) save_settings();
  } else if (!g_settings.enabled && was_enabled) {
    disable();
  } else {
    save_settings();
  }

  // Update device name if changed
  if (g_initialized) {
    // NimBLE doesn't support changing name after init without reinit
  }

  return true;
}

static bool set_device_name(const char* name) {
  if (!name || strlen(name) == 0 || strlen(name) > MAX_DEVICE_NAME_LEN) {
    return false;
  }

  strncpy(g_settings.device_name, name, MAX_DEVICE_NAME_LEN);
  g_settings.device_name[MAX_DEVICE_NAME_LEN] = '\0';
  save_settings();

  log_health(SCV_LOG_INFO, SCV_CAT_BLUETOOTH, "Device name changed", name);
  return true;
}

static bool set_tx_power(int8_t power) {
  if (power < -12 || power > 9) return false;

  g_settings.tx_power = power;
  save_settings();

  if (g_initialized) {
    // NimBLE 2.x: setPower takes the dBm value directly. The validated
    // [-12, +9] range maps 1:1 onto the supported ESP32 power levels.
    NimBLEDevice::setPower(power);
  }

  return true;
}

// ════════════════════════════════════════════════════════════════════════════
// WHAT THE STATUS ROUTES SHOW (sweep F138)
// ════════════════════════════════════════════════════════════════════════════
//
// GET /api/bluetooth, /scan/results, /paired and /settings (bluetooth_api.h)
// run on esp_http_server's task and read get_status(), the scan and paired
// tables and the settings in place, while the loop task wrote them (and,
// before F143, the NimBLE host task). Now the loop task publishes copies:
// publish_views() at the end of every update() pass (its early return
// included) and after each owner command (run_command(), before the drain
// posts the result the handler answers from); the readers copy the last
// one whole (loop_snapshot.h: a publish of unchanged bytes takes no lock).
// Only the loop task publishes; init() (the bring-up worker's, or a
// handler's bring_up()) never does.

// The status, as GET /api/bluetooth shows it, and the settings.
struct StatusView {
  BluetoothStatus status;            // advertising_time_ms, connected_time_ms: the runs that ended
  uint32_t advertising_since_ms;     // when the current run started (status.advertising)
  BluetoothSettings settings;
};

static loop_snapshot::Value<StatusView, loop_command_ring::PortMuxLock> g_status_view;
static loop_snapshot::Value<ScanView, loop_command_ring::PortMuxLock> g_scan_view;
static loop_snapshot::Value<PairedView, loop_command_ring::PortMuxLock> g_paired_view;
// The stack's own address, read once it is up (it is the public address,
// fixed: no own-address type is set). The loop task's.
static char g_local_address[18] = "";

// Each built zeroed (padding included) so a pass that changed nothing
// compares equal and takes no lock.
static void publish_status_view() {
  StatusView v;
  memset(&v, 0, sizeof(v));
  BluetoothStatus& status = v.status;
  // "initializing" while init() runs on its own task and its result is not
  // taken yet (sweep F167: init() no longer sets the state itself).
  status.state = (!g_initialized && init_running()) ? BT_INITIALIZING : g_state;
  status.enabled = g_settings.enabled;
  status.advertising = is_advertising();
  status.scanning = g_scanning;
  status.connected = g_connection.connected;

  strncpy(status.device_name, g_settings.device_name, MAX_DEVICE_NAME_LEN);
  status.device_name[MAX_DEVICE_NAME_LEN] = '\0';

  // Get local address
  if (g_initialized) {
    if (g_local_address[0] == '\0') {
      NimBLEAddress addr = NimBLEDevice::getAddress();
      strncpy(g_local_address, addr.toString().c_str(), 17);
      g_local_address[17] = '\0';
    }
    memcpy(status.local_address, g_local_address, sizeof(status.local_address));
  }

  status.tx_power = g_settings.tx_power;
  status.mtu = g_connection_mtu;
  // Mirror the SIG Battery Service value into /api/bluetooth so the SPA
  // can render it next to the other BT stats. The standard-profiles
  // module owns the canonical value (which a future battery sense
  // driver can update via set_battery_level); we just read.
  status.battery_pct = ble_standard_profiles::get_battery_level();
  status.paired_count = g_paired_count;
  status.scanned_count = g_scanned_count;
  memcpy(&status.connection, &g_connection, sizeof(status.connection));
  memcpy(&status.pairing, &g_pairing, sizeof(status.pairing));

  status.total_connections = g_total_connections;
  status.total_bytes_sent = g_total_bytes_sent;
  status.total_bytes_received = g_total_bytes_received;
  status.advertising_time_ms = g_advertising_total_ms;
  status.connected_time_ms = g_connected_total_ms;
  v.advertising_since_ms = g_advertising_start_ms;
  memcpy(&v.settings, &g_settings, sizeof(v.settings));
  (void)g_status_view.publish(v);
}

static void publish_scan_view() {
  ScanView v;
  memset(&v, 0, sizeof(v));
  v.scanning = g_scanning;
  v.count = (uint8_t)g_scanned_count;
  memcpy(v.devices, g_scanned_devices, g_scanned_count * sizeof(ScannedDevice));
  (void)g_scan_view.publish(v);
}

static void publish_paired_view() {
  PairedView v;
  memset(&v, 0, sizeof(v));
  v.count = (uint8_t)g_paired_count;
  memcpy(v.devices, g_paired_devices, g_paired_count * sizeof(PairedDevice));
  (void)g_paired_view.publish(v);
}

// The loop task: update()'s passes and each owner command (run_command()).
static void publish_views() {
  publish_status_view();
  publish_scan_view();
  publish_paired_view();
}

void read_status(BluetoothStatus* out) {
  StatusView v;
  if (!g_status_view.read(&v)) {
    // Before the first pass publishes: what the channel holds at boot.
    memset(&v, 0, sizeof(v));
    v.status.state = BT_DISABLED;
    v.status.enabled = kDefaultSettings.enabled;
    memcpy(v.status.device_name, kDefaultSettings.device_name, sizeof(v.status.device_name));
    v.status.tx_power = kDefaultSettings.tx_power;
    v.status.mtu = 23;
    v.status.battery_pct = ble_standard_profiles::get_battery_level();
  }
  *out = v.status;
  // The runs going on, counted to now (get_status() counted them at the read).
  const uint32_t now = millis();
  if (out->advertising) {
    out->advertising_time_ms += now - v.advertising_since_ms;
  }
  if (out->connected) {
    out->connected_time_ms += now - out->connection.connected_since_ms;
  }
}

BluetoothSettings read_settings() {
  StatusView v;
  if (!g_status_view.read(&v)) return kDefaultSettings;
  return v.settings;
}

bool read_enabled() {
  return read_settings().enabled;
}

void read_scan(ScanView* out) {
  if (!g_scan_view.read(out)) memset(out, 0, sizeof(*out));
}

void read_paired(PairedView* out) {
  if (!g_paired_view.read(out)) memset(out, 0, sizeof(*out));
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

// One owner command, on the loop task (update()'s drain of g_commands). The
// Result is read here, right after the command, so a REST answer describes
// the state the command left.
static Result run_command(const Command& cmd) {
  Result r;
  memset(&r, 0, sizeof(r));
  switch (cmd.type) {
    case BT_CMD_ENABLE:
      r.ok = enable();
      if (!r.ok) r.refusal = BT_REFUSED_NOT_ENABLED;
      break;
    case BT_CMD_DISABLE:
      disable();
      r.ok = true;
      break;
    case BT_CMD_ADVERTISE_START:
      // Auto-enable: "Start Advertising" is unambiguous user intent.
      if (!is_enabled() && !enable()) {
        r.refusal = BT_REFUSED_NOT_ENABLED;
        break;
      }
      if (g_connection.connected) {
        r.refusal = BT_REFUSED_CONNECTED;
        break;
      }
      r.ok = start_advertising();
      break;
    case BT_CMD_ADVERTISE_STOP:
      stop_advertising();
      r.ok = true;
      break;
    case BT_CMD_SCAN_START:
      r.ok = start_scan(cmd.duration_ms);
      break;
    case BT_CMD_SCAN_STOP:
      stop_scan();
      r.ok = true;
      break;
    case BT_CMD_SCAN_CLEAR:
      clear_scan_results();
      r.ok = true;
      break;
    case BT_CMD_PAIR_START:
      if (!is_enabled() && !enable()) {
        r.refusal = BT_REFUSED_NOT_ENABLED;
        break;
      }
      r.ok = start_pairing();
      r.allow_pairing = g_settings.allow_pairing;
      break;
    case BT_CMD_PAIR_CANCEL:
      cancel_pairing();
      r.ok = true;
      break;
    case BT_CMD_PAIR_CONFIRM:
      r.ok = confirm_pairing(cmd.pin);
      break;
    case BT_CMD_PAIR_REJECT:
      r.ok = reject_pairing();
      break;
    case BT_CMD_DISCONNECT:
      r.ok = disconnect();
      break;
    case BT_CMD_PAIRED_REMOVE:
      r.ok = remove_paired_device(cmd.address, &r.refusal);
      break;
    case BT_CMD_PAIRED_CLEAR:
      r.ok = clear_all_paired_devices(&r.refusal);
      break;
    case BT_CMD_PAIRED_TRUST:
      r.ok = set_device_trusted(cmd.address, cmd.flag);
      break;
    case BT_CMD_PAIRED_BLOCK:
      r.ok = set_device_blocked(cmd.address, cmd.flag);
      break;
    case BT_CMD_SETTINGS: {
      // The fields the POST named, over the settings as they stand here.
      BluetoothSettings s = g_settings;
      const BluetoothSettings& in = cmd.settings;
      if (cmd.set_mask & BT_SET_ENABLED) s.enabled = in.enabled;
      if (cmd.set_mask & BT_SET_AUTO_ADVERTISE) s.auto_advertise = in.auto_advertise;
      if (cmd.set_mask & BT_SET_ALLOW_PAIRING) s.allow_pairing = in.allow_pairing;
      if (cmd.set_mask & BT_SET_REQUIRE_PIN) s.require_pin = in.require_pin;
      if (cmd.set_mask & BT_SET_DEVICE_NAME) {
        memcpy(s.device_name, in.device_name, sizeof(s.device_name));
        s.device_name[MAX_DEVICE_NAME_LEN] = '\0';
      }
      if (cmd.set_mask & BT_SET_TX_POWER) s.tx_power = in.tx_power;
      if (cmd.set_mask & BT_SET_INACTIVITY) s.inactivity_timeout_ms = in.inactivity_timeout_ms;
      if (cmd.set_mask & BT_SET_NOTIFY_ON_CONNECT) s.notify_on_connect = in.notify_on_connect;
      if (cmd.set_mask & BT_SET_LONG_RANGE) s.long_range_mode = in.long_range_mode;
      // Turning Bluetooth on is an enable (sweep F144), and a command never
      // brings the stack up: refused, with nothing applied, as BT_CMD_ENABLE
      // is, while init() has not run.
      if (s.enabled && !g_settings.enabled && !g_initialized) {
        r.refusal = BT_REFUSED_NOT_ENABLED;
        break;
      }
      r.ok = set_settings(s);
      break;
    }
    case BT_CMD_NAME: {
      char name[sizeof(cmd.name)];
      memcpy(name, cmd.name, sizeof(name));
      name[sizeof(name) - 1] = '\0';
      r.ok = set_device_name(name);
      break;
    }
    case BT_CMD_POWER:
      r.ok = set_tx_power(cmd.power);
      break;
  }
  publish_views();   // F138: what this command did, before its handler answers
  return r;
}

loop_command_ring::Wait submit(const Command& cmd, Result* result, uint32_t timeout_ms) {
  Result r;
  memset(&r, 0, sizeof(r));
  const loop_command_ring::Wait w = loop_command_ring::submit(
      g_commands, cmd, &r, timeout_ms, COMMAND_POLL_MS,
      []() { return (uint32_t)millis(); },
      [](uint32_t ms) {
        const TickType_t ticks = pdMS_TO_TICKS(ms);
        vTaskDelay(ticks > 0 ? ticks : 1);
      });
  if (w != loop_command_ring::Wait::kDone) memset(&r, 0, sizeof(r));
  if (result != nullptr) *result = r;
  return w;
}

// A drop count in the health log when it moved, at most once a minute.
static void log_drops(uint32_t dropped, DropLog& log, LogLevel level, const char* message,
                      uint32_t now) {
  if (dropped == log.seen) return;
  if (log.logged_ms != 0 && now - log.logged_ms < 60000) return;
  char detail[48];
  snprintf(detail, sizeof(detail), "%lu since boot", (unsigned long)dropped);
  log_health(level, SCV_CAT_BLUETOOTH, message, detail);
  log.seen = dropped;
  log.logged_ms = now != 0 ? now : 1;
}

void update() {
  // The saved settings and paired list, on the first pass (the F167
  // review), then the bring-up's result, once init() has published it
  // (sweep F167): the stack's objects become this task's before anything
  // below reads them, and the channel's server callbacks reach the stack
  // only from here on.
  load_saved();
  adopt_init_result();
  // What the NimBLE host task reported since the last pass (sweep F143),
  // then the owner's commands (F111), both before the early return below:
  // a link that ends after Bluetooth is turned off still ends here, and a
  // disabled channel still runs BT_CMD_ENABLE. The events come first so a
  // command acts on the radio's latest state (a PIN confirm finds the
  // passkey the stack just asked about).
  g_events.consume(apply_event);
  reconcile_link();   // F169: what a dropped link event left stale
  g_commands.drain(run_command);

  // Events a full queue refused, in the health log at most once a minute
  // each: a link's as a warning (reconcile_link() above has asked the stack
  // for the link), scan results and GATT activity at debug level (a busy
  // room through a stall).
  uint32_t now = millis();
  log_drops(g_events.dropped_reserved(), g_link_drops, SCV_LOG_WARNING,
            "BLE link events dropped (queue full)", now);
  log_drops(g_events.dropped_limited(), g_lossy_drops, SCV_LOG_DEBUG,
            "BLE scan/activity events dropped (queue full)", now);
  // The bond store's refusals (sweep F189), at most once a minute each.
  log_drops(g_store_full_count, g_store_full_log, SCV_LOG_WARNING,
            "BLE pairing refused: the bond store is full", now);
  log_drops(g_store_overflow_count, g_store_overflow_log, SCV_LOG_WARNING,
            "BLE bond store full: a record was not kept", now);

  if (!g_initialized || !g_settings.enabled) {
    publish_views();   // F138: a disabled channel's pass, as the routes show it
    return;
  }

  static uint32_t last_status_update = 0;

  // Refresh the offline-console snapshot. Internally throttled to its own
  // SNAPSHOT_PERIOD_MS so calling on every iteration is cheap; only sends
  // a notification when the JSON bytes actually changed.
  ble_console::tick();

  // Drain async WiFi-scan completion + mirror connect outcome into the
  // provisioning STATE characteristic.
  ble_provision::tick();

  // Refresh the log-export HEAD so subscribers see new entries land
  // without having to poll. Internally throttled, no-op when the ring
  // hasn't changed.
  ble_log_export::tick();

  // Refresh the witness-chain head + last record. Same throttle pattern.
  ble_witness_export::tick();

  // Update status characteristic periodically
  if (g_connection.connected && now - last_status_update >= STATUS_UPDATE_INTERVAL_MS) {
    last_status_update = now;
    update_status_characteristic();

    // Refresh live RSSI + negotiated MTU for the API surface. The host call
    // ble_gap_conn_rssi() reads the most recent received-signal strength on
    // the active link — works regardless of whether the link is encrypted.
    if (g_connection_handle != 0xFFFF) {
      int8_t rssi = 0;
      if (ble_gap_conn_rssi(g_connection_handle, &rssi) == 0) {
        g_connection.rssi = rssi;
      }
      uint16_t mtu = ble_att_mtu(g_connection_handle);
      if (mtu >= 23) g_connection_mtu = mtu;
    }
  }

  // Check inactivity timeout
  handle_inactivity_timeout();

  // Check scan timeout
  handle_scan_timeout();

  // Check pairing timeout
  if (g_pairing.state != PAIR_NONE && g_pairing.state != PAIR_COMPLETE) {
    if (now - g_pairing.started_ms >= PAIRING_TIMEOUT_MS) {
      log_health(SCV_LOG_WARNING, SCV_CAT_BLUETOOTH, "Pairing timeout", nullptr);
      cancel_pairing();
    }
  }

  // F138: the status routes see this pass whole, from here until the next.
  publish_views();
}

// ════════════════════════════════════════════════════════════════════════════
// UTILITIES
// ════════════════════════════════════════════════════════════════════════════

// The lists keep an address's bytes as the stack does, least significant
// first (getBase()->val); a Bluetooth address is printed most significant
// first, as the phone shows its own and NimBLE's toString() prints it. So
// the bytes go out in reverse (sweep F172: they went out in stored order,
// backwards against the phone and against connection.name), and
// parse_address() takes them back in reverse, so an address a GET route
// printed finds its entry again (DELETE /api/bluetooth/paired, trust,
// block).
void format_address(const uint8_t* addr, char* out) {
  snprintf(out, BLE_ADDRESS_STR_LEN, "%02X:%02X:%02X:%02X:%02X:%02X",
           addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

bool parse_address(const char* str, uint8_t* out) {
  if (strlen(str) != 17) return false;
  // "XX:XX:XX:XX:XX:XX" exactly: sscanf's %02x stops at the first non-hex
  // character and still counts the field, so "...:0G" read as "...:00".
  for (int i = 0; i < 17; i++) {
    const bool colon = (i % 3) == 2;
    if (colon ? str[i] != ':' : !isxdigit((unsigned char)str[i])) return false;
  }

  unsigned int bytes[6];
  if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
             &bytes[0], &bytes[1], &bytes[2],
             &bytes[3], &bytes[4], &bytes[5]) != 6) {
    return false;
  }

  for (int i = 0; i < 6; i++) {
    out[5 - i] = (uint8_t)bytes[i];   // printed most significant first (format_address)
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

float estimate_distance_m(int8_t rssi_dbm, int8_t tx_ref_dbm) {
  // RSSI of 0 from NimBLE means "no measurement available" — surface that
  // explicitly so the UI can render "—" instead of a bogus number.
  if (rssi_dbm == 0) return 0.0f;
  // Path-loss exponent. 2.0 = free space; 3.0 = light obstruction; we use 2.5
  // as a household default. Tuned by observation, not regulation — the value
  // shifts the absolute number but not the relative ordering of devices.
  const float n = 2.5f;
  float exponent = ((float)tx_ref_dbm - (float)rssi_dbm) / (10.0f * n);
  float d = powf(10.0f, exponent);
  if (d < 0.1f) d = 0.1f;
  if (d > 100.0f) d = 100.0f;
  return d;
}

const char* distance_label(float meters) {
  if (meters <= 0.0f)  return "unknown";
  if (meters <= 1.0f)  return "right here";
  if (meters <= 3.0f)  return "near";
  if (meters <= 8.0f)  return "nearby";
  if (meters <= 20.0f) return "in range";
  return "far";
}

} // namespace bluetooth_channel

#endif // FEATURE_BLUETOOTH && NimBLEDevice.h available
