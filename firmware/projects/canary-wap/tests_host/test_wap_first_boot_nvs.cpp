// Host tests for the canary-wap's first boot after an NVS erase and the
// "securacv" namespace (sweep F201): the REAL opens, cut verbatim out of the
// sketch (firmware/tests_host/cut_functions.awk): setup_wizard.h's init(),
// canary_wap.ino's nvs_load_key() and nvs_store_key() over the real
// nvs_store.h (NvsManager, NvsMainSession), and power_monitor.h's
// load_nvs_state(), over a fake NVS (stubs/first_boot) that models
// namespaces and counts the error line Arduino-ESP32's Preferences::begin()
// logs for each open it refuses.
//
// setup() runs them in this order: setup_wizard::init(), then
// provision_device() (the key read, then, with no key, the store that
// creates the namespace), then power_monitor::init() (load_nvs_state()).
// The source pins below hold the sketch to that order. Nothing in setup()
// opens "securacv" before setup_wizard::init(): the calls ahead of it (the
// boot banner, hw_state_init(), safe_mode_check() on "hw_state", the reset
// reason's health line) touch no "securacv" row (read from the sketch, not
// counted by this suite).
//
// So on a first boot after an erase two opens met an absent namespace and
// logged `nvs_open failed: NOT_FOUND` on a build that keeps Arduino's error
// log: the wizard's and the key read's. load_nvs_state() comes after the
// store and found the namespace, unless that boot's provisioning stored no
// key. All three now ask IDF's nvs_open() first (csi_module_settings_nvs.h's
// begin_read_only() / probe_namespace()): an absent namespace is not handed
// to Preferences, every read is its default, as the refused open made it;
// a namespace that is there, or an NVS fault, goes to Preferences as before.
//
// Against the code before F201 the first-boot test counts two error lines
// and the failed-provisioning test three; with any one of the three sites
// put back as a plain open, a test or the pin fails.
//
// And the "chirp" namespace (sweep F220): chirp_channel::init(), in setup(),
// reads the relay and urgency-filter settings (load_settings(), two
// nvs_get_u8() calls), and the loop's first pass reads the self-test chirp's
// stamp (nvs_get_u32("st_chirp_at")), each a read-only NvsSession, before
// anything writes the namespace: only a relay or filter change
// (save_settings()) and the self-test's first stamp, once the clock is set,
// do. So a first boot after an erase logged three lines, and so did every
// boot after it until the clock synced or the owner changed a Chirp
// setting. load_settings() and save_settings() are cut verbatim from
// chirp_channel.cpp and the stamp read from loop() (its block, by line),
// over the real nvs_store.h, whose NvsSession now opens read-only through
// begin_read_only(): a first boot logs none of them. Against the plain
// read-only open the chirp first-boot test counts three lines.
//
// And the key read fails closed (sweep F360, the canary's rule on this
// sketch). nvs_load_key() checked the stored key's length and then discarded
// the count Preferences::getBytes() answered; the core leaves the buffer as
// it was when that read fails, so a read that failed after the length check
// left g_device's zeroed key in use, and provisioning went on with it: the
// SoftAP came up under that key's published known answer cv-3jByJ6rHdhM2
// and records were signed with a key anyone holds. And its one false meant
// "no key": a failed session open or a length lookup other than 32 made
// provision_device() generate a keypair and store it over the old one. Now
// stored_key_length() asks IDF's nvs_open() / nvs_get_blob() first, and only
// ESP_ERR_NVS_NOT_FOUND (no namespace, or no key in it) answers Absent; any
// other error, a key that is not 32 bytes, a read that does not answer all
// 32 bytes and a key of 32 zero bytes answer Refused with the buffer zeroed.
// establish_identity_key() (provision_device()'s key block, cut verbatim)
// generates only on Absent; on Refused it sets g_device.key_refused,
// generates and writes nothing, and setup() halts on it (pinned below).
// Against the old load and the old key block, adapted only so that they
// build, the key cases fail.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Arduino.h"
#include "Preferences.h"
#include "csi_module_settings_nvs.h"
#include "device_name_rule.h"   // init() reads a stored name through it (sweep F226)
#include "nvs_store.h"

#ifndef WAP_SKETCH_DIR
#error "build with -DWAP_SKETCH_DIR=\"<the canary_wap sketch directory>\""
#endif

static int g_checks = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
    ++g_checks;                                                         \
  } while (0)

static uint32_t millis() { return 4242; }

// ── setup_wizard.h's init(), cut verbatim, over its own statics ─────────
namespace setup_wizard {
static constexpr size_t DEVICE_NAME_MAX = 32;
static bool s_first_boot = false;
static bool s_active = false;
static uint32_t s_started_ms = 0;
static char s_device_name[DEVICE_NAME_MAX + 1] = {0};
#include "test_wap_first_boot_nvs_wizard.inc"
}  // namespace setup_wizard

// ── canary_wap.ino's key read, its store and its caller, cut verbatim ───
// What establish_identity_key() touches besides the cut: the members of the
// sketch's DeviceIdentity it reads and writes, and a stand-in keygen that
// counts its calls and draws a fixed key (or fails, as a boot whose
// provisioning stores no key).
struct DeviceIdentity {
  uint8_t privkey[32];
  uint8_t pubkey[32];
  uint32_t key_born_ms;
  bool key_is_new;
  bool key_refused;
  bool first_boot;
};
static DeviceIdentity g_device;
static int g_keygens = 0;
static bool g_keygen_fails = false;
static bool generate_keypair(uint8_t priv[32], uint8_t pub[32]) {
  ++g_keygens;
  if (g_keygen_fails) return false;
  std::memset(priv, 0x5a, 32);
  std::memset(pub, 0x77, 32);
  return true;
}
#include "test_wap_first_boot_nvs_key.inc"

// ── power_monitor.h's load_nvs_state(), cut verbatim ────────────────────
enum esp_reset_reason_t { ESP_RST_POWERON = 1, ESP_RST_BROWNOUT = 9 };
static esp_reset_reason_t g_reset_reason = ESP_RST_POWERON;
static esp_reset_reason_t esp_reset_reason() { return g_reset_reason; }
struct PowerState {
  uint16_t capacity_mah;
  uint32_t charge_cycles;
  uint16_t max_voltage_mv;
  uint16_t min_voltage_mv;
};
struct PowerHistory {
  uint32_t charge_cycles;
  uint32_t total_runtime_min;
  uint16_t voltage_min_mv;
  uint16_t voltage_max_mv;
  uint8_t  soc_min_pct;
  uint32_t brownout_count;
  uint32_t last_full_charge_ms;
};
namespace power_monitor {
static constexpr uint16_t CAPACITY_MAH = 3000;
static PowerState s_state = {};
static PowerHistory s_history = {};
#include "test_wap_first_boot_nvs_power.inc"
}  // namespace power_monitor

// ── chirp_channel.cpp's settings load and save, cut verbatim ────────────
namespace chirp_channel {
enum ChirpUrgency : uint8_t { CHIRP_URG_INFO = 0, CHIRP_URG_CAUTION, CHIRP_URG_URGENT };
static bool g_relay_enabled = true;                   // chirp_channel.cpp's defaults
static ChirpUrgency g_urgency_filter = CHIRP_URG_INFO;
#include "test_wap_first_boot_nvs_chirp.inc"
}  // namespace chirp_channel

// ── canary_wap.ino's self-test stamp read (in loop()), cut verbatim ─────
namespace selftest {
static uint32_t s_last_selftest_unix = 0;             // the block's own statics
static bool s_selftest_nvs_loaded = false;
static void load_stamp() {
#include "test_wap_first_boot_nvs_stamp.inc"
}
}  // namespace selftest

// ── A boot, in setup()'s order ──────────────────────────────────────────
struct BootResult {
  bool key_loaded = false;
  bool key_stored = false;
};

// A reboot: RAM gone, NVS kept. `provisions` false models a boot whose
// provisioning stored no key (establish_identity_key() returns before
// nvs_store_key() when the keypair generation fails).
static BootResult boot(bool provisions = true) {
  setup_wizard::s_first_boot = false;
  setup_wizard::s_active = false;
  setup_wizard::s_started_ms = 0;
  std::memset(setup_wizard::s_device_name, 0, sizeof(setup_wizard::s_device_name));
  std::memset(&power_monitor::s_state, 0, sizeof(power_monitor::s_state));
  power_monitor::s_state.min_voltage_mv = 0xFFFF;
  power_monitor::s_state.capacity_mah = power_monitor::CAPACITY_MAH;
  std::memset(&power_monitor::s_history, 0, sizeof(power_monitor::s_history));
  host_nvs().reset_counts();

  std::memset(&g_device, 0, sizeof(g_device));
  g_keygen_fails = !provisions;
  Serial.all.clear();

  BootResult r;
  setup_wizard::init();                          // setup()'s first "securacv" open
  const int keygens = g_keygens;
  const bool key_ready = establish_identity_key();   // provision_device()
  r.key_loaded = key_ready && g_keygens == keygens;
  r.key_stored = key_ready && g_keygens != keygens;
  power_monitor::load_nvs_state();               // power_monitor::init()
  g_keygen_fails = false;
  return r;
}

static void store_bool(const char* key, bool v) {
  HostFirstBootNvs::Row& r = host_nvs().rows[std::string("securacv/") + key];
  r.type = 'B';
  r.bytes.assign(1, v ? 1 : 0);
}
static void store_u32(const char* key, uint32_t v) {
  HostFirstBootNvs::Row& r = host_nvs().rows[std::string("securacv/") + key];
  r.type = 'u';
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  r.bytes.assign(p, p + 4);
}
static void store_string(const char* key, const char* v) {
  HostFirstBootNvs::Row& r = host_nvs().rows[std::string("securacv/") + key];
  r.type = 's';
  r.bytes.assign(v, v + std::strlen(v));
}

// The first boot after an erase: no line, nothing opened before the store,
// the namespace made by the store, and every reader's first-boot answer.
static int test_a_first_boot_after_an_erase_logs_no_securacv_line() {
  host_nvs().clear();
  const BootResult r = boot();
  CHECK(host_nvs().error_logs == 0);
  CHECK(host_nvs().opens == 2);                  // the store, then load_nvs_state()
  CHECK(host_nvs().probes == 3);                 // wizard, key read, load_nvs_state()
  CHECK(!r.key_loaded);
  CHECK(r.key_stored);
  CHECK(host_nvs().has_namespace("securacv"));
  CHECK(setup_wizard::s_first_boot);
  CHECK(setup_wizard::s_active);
  CHECK(setup_wizard::s_started_ms == 4242);
  CHECK(setup_wizard::s_device_name[0] == '\0');
  CHECK(power_monitor::s_state.capacity_mah == power_monitor::CAPACITY_MAH);
  CHECK(power_monitor::s_state.min_voltage_mv == 0xFFFF);
  CHECK(power_monitor::s_history.soc_min_pct == 100);

  // The next boot finds the key it stored, and still logs nothing.
  const BootResult second = boot();
  CHECK(second.key_loaded);
  CHECK(!second.key_stored);
  CHECK(host_nvs().error_logs == 0);
  CHECK(setup_wizard::s_first_boot);             // setup_ok is not stored yet
  return 0;
}

// A provisioned device reads as before: the wizard's setup_ok and name, the
// key, the battery history; one Preferences open each, no line.
static int test_a_provisioned_boot_reads_as_before() {
  host_nvs().clear();
  store_bool("setup_ok", true);
  store_string("dev_name", "Porch");
  store_u32("batt_cycles", 7);
  {
    HostFirstBootNvs::Row& k = host_nvs().rows["securacv/privkey"];
    k.type = 'b';
    k.bytes.assign(32, 0x11);
  }
  const BootResult r = boot();
  CHECK(host_nvs().error_logs == 0);
  CHECK(host_nvs().opens == 3);
  CHECK(r.key_loaded);
  CHECK(!setup_wizard::s_first_boot);
  CHECK(!setup_wizard::s_active);
  CHECK(std::strcmp(setup_wizard::s_device_name, "Porch") == 0);
  CHECK(power_monitor::s_state.charge_cycles == 7);
  CHECK(power_monitor::s_history.charge_cycles == 7);
  return 0;
}

// A boot whose provisioning stored no key reaches load_nvs_state() with no
// namespace: it opens nothing, logs nothing and reads every default, as the
// refused open did. Before F201 this boot logged three lines.
static int test_a_boot_that_stored_no_key_logs_nothing() {
  host_nvs().clear();
  const BootResult r = boot(/*provisions=*/false);
  CHECK(!r.key_loaded);
  CHECK(!r.key_stored);
  CHECK(host_nvs().error_logs == 0);
  CHECK(host_nvs().opens == 0);
  CHECK(!host_nvs().has_namespace("securacv"));
  CHECK(power_monitor::s_state.capacity_mah == power_monitor::CAPACITY_MAH);
  CHECK(power_monitor::s_history.brownout_count == 0);

  // A brownout reset still counts: the write creates the namespace.
  g_reset_reason = ESP_RST_BROWNOUT;
  host_nvs().reset_counts();
  power_monitor::load_nvs_state();
  g_reset_reason = ESP_RST_POWERON;
  CHECK(host_nvs().error_logs == 0);
  CHECK(power_monitor::s_history.brownout_count == 1);
  CHECK(host_nvs().has_namespace("securacv"));
  return 0;
}

// An NVS fault is not a missing namespace: the wizard's and the power
// monitor's opens still go to Preferences and keep their error line, as
// before. The key read asks IDF, which names the fault, and refuses it
// (sweep F360): no Preferences open, so no line of its own, and no keygen or
// store after it. Before F360 the key read's open and the store's each
// logged a line too (four), and a keypair was generated.
static int test_an_nvs_fault_keeps_its_lines() {
  host_nvs().clear();
  host_nvs().fail_begin = true;
  const int keygens = g_keygens;
  const BootResult r = boot();
  CHECK(!r.key_loaded);
  CHECK(!r.key_stored);
  CHECK(host_nvs().error_logs == 2);             // wizard, power
  CHECK(g_device.key_refused);
  CHECK(g_keygens == keygens);
  CHECK(Serial.all.find("[!!] identity key not loaded: NVS could not say whether one is stored "
                        "(ESP_ERR_NVS_NOT_INITIALIZED)\n") != std::string::npos);
  CHECK(setup_wizard::s_first_boot);
  host_nvs().fail_begin = false;
  return 0;
}

// ── The key read fails closed (F360) ────────────────────────────────────
static const uint8_t kStoredKey = 0x11;

static bool same_store(const HostFirstBootNvs& a, const HostFirstBootNvs& b) {
  if (a.created != b.created || a.rows.size() != b.rows.size()) return false;
  for (auto ia = a.rows.begin(), ib = b.rows.begin(); ia != a.rows.end(); ++ia, ++ib) {
    if (ia->first != ib->first || ia->second.type != ib->second.type ||
        ia->second.bytes != ib->second.bytes) {
      return false;
    }
  }
  return true;
}
static bool all_bytes(const uint8_t* p, uint8_t v) {
  for (size_t i = 0; i < 32; i++) {
    if (p[i] != v) return false;
  }
  return true;
}
static void store_key(size_t len, uint8_t v) {
  HostFirstBootNvs::Row& k = host_nvs().rows["securacv/privkey"];
  k.type = 'b';
  k.bytes.assign(len, v);
}
static bool printed(const char* line) { return Serial.all.find(line) != std::string::npos; }

// Nothing open after the load: IDF's handle closed, the session ended.
static int check_idle() {
  CHECK(host_nvs().handle_ns.empty());
  CHECK(!NvsManager::instance().isOpen());
  return 0;
}

// The boot a refusal leaves: no key in RAM, nothing generated, NVS as it
// was, the flag setup() halts on, and the two lines that say why.
static int check_refused(const HostFirstBootNvs& before, const char* why) {
  CHECK(g_device.key_refused);
  CHECK(all_bytes(g_device.privkey, 0));
  CHECK(!g_device.key_is_new && !g_device.first_boot);
  CHECK(same_store(before, host_nvs()));
  CHECK(printed(why));
  CHECK(printed("[!!] Stored identity key not read: not replacing it, provisioning cannot "
                "continue (see the identity key line above)\n"));
  CHECK(!printed("[PROV] Generating Ed25519 keypair"));
  CHECK(!printed("[PROV] Loaded existing keypair from NVS"));
  return check_idle();
}

// One establish_identity_key() as provision_device() runs it. The key buffer
// starts as bytes nobody chose (the device's is a zeroed static; the load
// must not depend on that), so every refusal's zeroed buffer is the load's
// own.
static bool establish() {
  std::memset(&g_device, 0, sizeof(g_device));
  std::memset(g_device.privkey, 0xEE, sizeof(g_device.privkey));
  Serial.all.clear();
  host_nvs().reset_counts();
  return establish_identity_key();
}

// A stored key loads unchanged, and nothing is generated or written.
static int test_a_stored_key_loads_unchanged() {
  host_nvs().clear();
  store_key(32, kStoredKey);
  const HostFirstBootNvs before = host_nvs();
  const int keygens = g_keygens;
  CHECK(establish());
  CHECK(all_bytes(g_device.privkey, kStoredKey));
  CHECK(!g_device.key_refused && !g_device.key_is_new && !g_device.first_boot);
  CHECK(g_keygens == keygens);
  CHECK(same_store(before, host_nvs()));
  CHECK(printed("[PROV] Loaded existing keypair from NVS\n"));
  CHECK(host_nvs().error_logs == 0);
  CHECK(host_nvs().blob_reads == 1);
  return check_idle();
}

// The namespace is there and holds no key (a boot whose provisioning
// stored none, after its brownout count made the namespace): Absent, so a
// key is generated and stored, as on a first boot.
static int test_a_namespace_with_no_key_generates_one() {
  host_nvs().clear();
  store_u32("batt_brown", 1);
  const int keygens = g_keygens;
  CHECK(establish());
  CHECK(g_keygens == keygens + 1);
  CHECK(!g_device.key_refused && g_device.key_is_new && g_device.first_boot);
  CHECK(g_device.key_born_ms == 4242);
  CHECK(all_bytes(g_device.privkey, 0x5a));
  CHECK(host_nvs().rows["securacv/privkey"].bytes == std::vector<uint8_t>(32, 0x5a));
  CHECK(printed("[PROV] Keypair stored in NVS\n"));
  CHECK(host_nvs().error_logs == 0);
  return check_idle();
}

// The filed case: the length lookup answers 32, the read itself fails and
// leaves the buffer as it was. Before, the load answered true over the
// zeroed key.
static int test_a_read_that_fails_after_the_length_check_is_refused() {
  host_nvs().clear();
  store_key(32, kStoredKey);
  host_nvs().read = HostFirstBootNvs::Read::Fails;
  const HostFirstBootNvs before = host_nvs();
  const int keygens = g_keygens;
  CHECK(!establish());
  CHECK(g_keygens == keygens);
  CHECK(check_refused(before,
                      "[!!] identity key not loaded: the stored key could not be read "
                      "(0 of 32 bytes)\n") == 0);
  host_nvs().read = HostFirstBootNvs::Read::Ok;
  return 0;
}

// A read that wrote the buffer and then refused (IDF fills it before its
// CRC check), and a short read: refused, and the buffer zeroed again.
static int test_a_garbled_or_short_read_is_refused_and_zeroed() {
  for (HostFirstBootNvs::Read how : {HostFirstBootNvs::Read::Garbles, HostFirstBootNvs::Read::Short}) {
    host_nvs().clear();
    store_key(32, kStoredKey);
    host_nvs().read = how;
    const HostFirstBootNvs before = host_nvs();
    const int keygens = g_keygens;
    CHECK(!establish());
    CHECK(g_keygens == keygens);
    CHECK(check_refused(before, how == HostFirstBootNvs::Read::Short
                                    ? "the stored key could not be read (31 of 32 bytes)\n"
                                    : "the stored key could not be read (0 of 32 bytes)\n") == 0);
  }
  host_nvs().read = HostFirstBootNvs::Read::Ok;
  return 0;
}

// The length lookup fails (a fault, or "privkey" stored as another type).
// Before, getBytesLength()'s 0 read as "no key": a keypair was generated
// and stored over the identity.
static int test_a_failed_length_lookup_is_refused() {
  host_nvs().clear();
  store_key(32, kStoredKey);
  host_nvs().length_fault = ESP_FAIL;
  HostFirstBootNvs before = host_nvs();
  int keygens = g_keygens;
  CHECK(!establish());
  CHECK(g_keygens == keygens);
  CHECK(check_refused(before,
                      "[!!] identity key not loaded: NVS could not say whether one is stored "
                      "(ESP_FAIL)\n") == 0);
  host_nvs().length_fault = ESP_OK;

  host_nvs().clear();
  store_u32("privkey", 7);
  before = host_nvs();
  keygens = g_keygens;
  CHECK(!establish());
  CHECK(g_keygens == keygens);
  CHECK(check_refused(before,
                      "[!!] identity key not loaded: NVS could not say whether one is stored "
                      "(ESP_ERR_NVS_TYPE_MISMATCH)\n") == 0);
  return 0;
}

// A stored key of the wrong length is refused, not replaced.
static int test_a_wrong_length_key_is_refused() {
  for (size_t len : {size_t(16), size_t(31), size_t(33), size_t(64)}) {
    host_nvs().clear();
    store_key(len, kStoredKey);
    const HostFirstBootNvs before = host_nvs();
    const int keygens = g_keygens;
    CHECK(!establish());
    CHECK(g_keygens == keygens);
    char why[96];
    std::snprintf(why, sizeof(why),
                  "[!!] identity key not loaded: the stored key is %u bytes, not 32\n",
                  (unsigned)len);
    CHECK(check_refused(before, why) == 0);
  }
  return 0;
}

// The probe found the key but the session would not open (the shared
// handle's lock wait timed out, or NVS failed between the two). Before,
// that read as "no key" and a keypair was generated.
static int test_a_session_that_will_not_open_is_refused() {
  host_nvs().clear();
  store_key(32, kStoredKey);
  host_nvs().prefs_begin_fails = true;
  const HostFirstBootNvs before = host_nvs();
  const int keygens = g_keygens;
  CHECK(!establish());
  CHECK(g_keygens == keygens);
  CHECK(check_refused(before,
                      "[!!] identity key not loaded: the stored key could not be read "
                      "(0 of 32 bytes)\n") == 0);
  host_nvs().prefs_begin_fails = false;
  return 0;
}

// A stored key of 32 zero bytes is the zeroed buffer's, the one key whose
// every derivation is public: refused.
static int test_a_zero_key_is_refused() {
  host_nvs().clear();
  store_key(32, 0);
  const HostFirstBootNvs before = host_nvs();
  const int keygens = g_keygens;
  CHECK(!establish());
  CHECK(g_keygens == keygens);
  CHECK(check_refused(before, "[!!] identity key not loaded: the stored key is 32 zero bytes\n") ==
        0);
  return 0;
}

// Only all 32 zero bytes are refused: a key with one byte set, at either
// end, is a key.
static int test_a_key_with_one_byte_set_loads() {
  for (size_t at : {size_t(0), size_t(31)}) {
    host_nvs().clear();
    store_key(32, 0);
    host_nvs().rows["securacv/privkey"].bytes[at] = 0x01;
    CHECK(establish());
    CHECK(!g_device.key_refused);
    CHECK(g_device.privkey[at] == 0x01);
    CHECK(check_idle() == 0);
  }
  return 0;
}

// The next boot that can read the key loads it: the refusal kept it.
static int test_a_refused_boot_keeps_the_key_for_the_next() {
  host_nvs().clear();
  store_key(32, kStoredKey);
  host_nvs().read = HostFirstBootNvs::Read::Fails;
  CHECK(!establish());
  host_nvs().read = HostFirstBootNvs::Read::Ok;
  CHECK(establish());
  CHECK(all_bytes(g_device.privkey, kStoredKey));
  CHECK(!g_device.key_refused);
  return check_idle();
}

// ── The "chirp" namespace (F220) ────────────────────────────────────────
// A boot's chirp reads, in their order: setup()'s chirp_channel::init()
// (load_settings()), then loop()'s first pass (the self-test stamp). RAM
// gone, NVS kept.
static void chirp_boot() {
  chirp_channel::g_relay_enabled = true;
  chirp_channel::g_urgency_filter = chirp_channel::CHIRP_URG_INFO;
  selftest::s_last_selftest_unix = 0;
  selftest::s_selftest_nvs_loaded = false;
  host_nvs().reset_counts();
  chirp_channel::load_settings();
  selftest::load_stamp();
}

// The first boot after an erase: nothing opened, no line, every default; a
// boot whose clock never synced writes nothing, so the next is as quiet;
// the self-test's first stamp makes the namespace, and the boot after reads
// it with three opens and still no line.
static int test_a_first_boot_logs_no_chirp_line() {
  host_nvs().clear();
  chirp_boot();
  CHECK(host_nvs().error_logs == 0);
  CHECK(host_nvs().opens == 0);
  CHECK(host_nvs().probes == 3);                 // relay, filter, stamp
  CHECK(!host_nvs().has_namespace("chirp"));
  CHECK(chirp_channel::g_relay_enabled);
  CHECK(chirp_channel::g_urgency_filter == chirp_channel::CHIRP_URG_INFO);
  CHECK(selftest::s_selftest_nvs_loaded && selftest::s_last_selftest_unix == 0);

  chirp_boot();                                  // no clock yet: nothing was written
  CHECK(host_nvs().error_logs == 0 && host_nvs().opens == 0);

  CHECK(nvs_set_u32("st_chirp_at", 1760000000u));   // the loop's first stamp, clock set
  CHECK(host_nvs().has_namespace("chirp"));
  chirp_boot();
  CHECK(host_nvs().error_logs == 0);
  CHECK(host_nvs().opens == 3);
  CHECK(selftest::s_last_selftest_unix == 1760000000u);
  CHECK(chirp_channel::g_relay_enabled);         // the settings' keys are absent: defaults
  CHECK(chirp_channel::g_urgency_filter == chirp_channel::CHIRP_URG_INFO);
  return 0;
}

// A relay or filter change writes the namespace; the next boot reads both
// back, as before.
static int test_stored_chirp_settings_read_as_before() {
  host_nvs().clear();
  chirp_channel::g_relay_enabled = false;
  chirp_channel::g_urgency_filter = chirp_channel::CHIRP_URG_URGENT;
  chirp_channel::save_settings();
  CHECK(host_nvs().has_namespace("chirp"));
  chirp_boot();
  CHECK(host_nvs().error_logs == 0);
  CHECK(host_nvs().opens == 3);
  CHECK(!chirp_channel::g_relay_enabled);
  CHECK(chirp_channel::g_urgency_filter == chirp_channel::CHIRP_URG_URGENT);
  CHECK(selftest::s_last_selftest_unix == 0);    // no stamp stored: the clock seeds it
  return 0;
}

// An NVS fault still goes to Preferences and keeps its three lines.
static int test_an_nvs_fault_keeps_its_chirp_lines() {
  host_nvs().clear();
  host_nvs().fail_begin = true;
  chirp_boot();
  CHECK(host_nvs().error_logs == 3);
  CHECK(chirp_channel::g_relay_enabled);
  CHECK(selftest::s_selftest_nvs_loaded && selftest::s_last_selftest_unix == 0);
  host_nvs().fail_begin = false;
  return 0;
}

// ── Source pins: the order these tests run in is setup()'s ──────────────
static std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
static std::string body_of(const std::string& src, const std::string& sig) {
  const size_t at = src.find("\n" + sig);
  if (at == std::string::npos) return "";
  const size_t end = src.find("\n}\n", at);
  if (end == std::string::npos) return "";
  return src.substr(at, end - at);
}

static int test_the_boot_runs_these_opens_in_this_order() {
  const std::string dir = WAP_SKETCH_DIR;
  const std::string ino = read_file(dir + "/canary_wap.ino");
  const std::string wizard = read_file(dir + "/setup_wizard.h");
  const std::string power = read_file(dir + "/power_monitor.h");
  CHECK(!ino.empty() && !wizard.empty() && !power.empty());

  const std::string setup = body_of(ino, "void setup() {");
  CHECK(!setup.empty());
  const size_t w = setup.find("\n  setup_wizard::init();");
  const size_t p = setup.find("if (!provision_device())");
  const size_t m = setup.find("power_monitor::init(");
  CHECK(w != std::string::npos && p != std::string::npos && m != std::string::npos);
  CHECK(w < p && p < m);
  const std::string wizard_call = "\n  setup_wizard::init();";
  CHECK(setup.find("setup_wizard::init()", w + wizard_call.size()) == std::string::npos);
  // Nothing ahead of the wizard in setup() names the namespace or a session.
  const std::string ahead = setup.substr(0, w);
  CHECK(ahead.find("securacv\"") == std::string::npos);
  CHECK(ahead.find("NvsMainSession") == std::string::npos);
  CHECK(ahead.find("nvs_") == std::string::npos);

  // provision_device() establishes the key, which reads before it stores.
  const std::string prov = body_of(ino, "static bool provision_device() {");
  CHECK(prov.find("if (!establish_identity_key()) return false;") != std::string::npos);
  const std::string est = body_of(ino, "static bool establish_identity_key() {");
  const size_t rd = est.find("nvs_load_key(");
  const size_t wr = est.find("nvs_store_key(");
  CHECK(rd != std::string::npos && wr != std::string::npos && rd < wr);

  // power_monitor::init() reads its state through load_nvs_state().
  const std::string pinit = body_of(power, "inline void init(");
  CHECK(pinit.find("load_nvs_state();") != std::string::npos);

  // Each of the three opens is the quiet one (F201); none is a plain
  // read-only begin() of "securacv" any more.
  CHECK(body_of(wizard, "inline bool init() {")
            .find("csi_module_settings_nvs::begin_read_only(prefs, \"securacv\")") !=
        std::string::npos);
  CHECK(body_of(power, "inline void load_nvs_state() {")
            .find("csi_module_settings_nvs::begin_read_only(prefs, \"securacv\")") !=
        std::string::npos);
  const std::string load_key = body_of(ino, "static KeyLoad nvs_load_key(uint8_t priv[32]) {");
  const size_t probe = load_key.find("stored_key_length(&stored)");
  const size_t session = load_key.find("NvsMainSession nvs(true);");
  CHECK(probe != std::string::npos && session != std::string::npos && probe < session);
  const std::string length = body_of(ino, "static esp_err_t stored_key_length(size_t* len) {");
  CHECK(length.find("nvs_open(NVS_MAIN_NS, NVS_READONLY, &h)") != std::string::npos);
  CHECK(length.find("nvs_close(h);") != std::string::npos);
  CHECK(wizard.find("begin(\"securacv\", true)") == std::string::npos);
  CHECK(power.find("begin(\"securacv\", true)") == std::string::npos);
  return 0;
}

// Comments become spaces (newlines kept); string and character literals are
// kept, so a "//" inside one is not a comment.
static std::string strip_comments(const std::string& s) {
  std::string out = s;
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '"' || c == '\'') {
      size_t j = i + 1;
      while (j < s.size() && s[j] != c && s[j] != '\n') j += (s[j] == '\\') ? 2 : 1;
      i = j + 1;
    } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      while (i < s.size() && s[i] != '\n') out[i++] = ' ';
    } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
      size_t j = s.find("*/", i + 2);
      j = (j == std::string::npos) ? s.size() : j + 2;
      for (; i < j; i++) {
        if (out[i] != '\n') out[i] = ' ';
      }
    } else {
      i++;
    }
  }
  return out;
}
static size_t count_of(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) n++;
  return n;
}

// The key read's refusal (F360) stops the boot: setup()'s provisioning
// failure block halts on g_device.key_refused before anything else in it,
// and provision_device() returns on a refused key before it derives or names
// anything from it or marks the identity ready. The one call of
// nvs_load_key() is establish_identity_key()'s, and only the key's probe,
// load and store name its NVS entry.
static int test_a_refused_key_halts_the_boot() {
  const std::string dir = WAP_SKETCH_DIR;
  const std::string ino = strip_comments(read_file(dir + "/canary_wap.ino"));
  CHECK(!ino.empty());

  const std::string setup = body_of(ino, "void setup() {");
  const size_t fail = setup.find("if (!provision_device()) {");
  CHECK(fail != std::string::npos);
  const std::string fail_head = "if (!provision_device()) {";
  CHECK(setup.find("provision_device()", fail + fail_head.size()) == std::string::npos);
  const size_t end = setup.find("pinMode(BOOT_BUTTON_GPIO", fail);
  CHECK(end != std::string::npos);
  const std::string block = setup.substr(fail, end - fail);
  const size_t halt = block.find("if (g_device.key_refused) {");
  const size_t loop = block.find("while (true) { delay(1000); }");
  CHECK(halt != std::string::npos && loop != std::string::npos && halt < loop);
  CHECK(block.find('}', halt) > loop);           // the loop is inside the if's block
  CHECK(block.find("Serial.println();") > loop);  // the block's first statement
  CHECK(block.find("log_health(") > loop);

  const std::string prov = body_of(ino, "static bool provision_device() {");
  const size_t est = prov.find("if (!establish_identity_key()) return false;");
  CHECK(est != std::string::npos);
  for (const char* later : {"Ed25519::derivePublicKey(", "compute_fingerprint(", "generate_device_id(",
                            "derive_api_token(", "derive_ap_password(", "g_device.initialized = true;"}) {
    const size_t at = prov.find(later);
    CHECK(at != std::string::npos && est < at);
  }

  CHECK(count_of(ino, "nvs_load_key(") == 2);    // its definition and establish_identity_key()'s call
  CHECK(body_of(ino, "static bool establish_identity_key() {").find("nvs_load_key(g_device.privkey)") !=
        std::string::npos);
  const size_t in_bodies =
      count_of(body_of(ino, "static esp_err_t stored_key_length(size_t* len) {"), "NVS_KEY_PRIV") +
      count_of(body_of(ino, "static KeyLoad nvs_load_key(uint8_t priv[32]) {"), "NVS_KEY_PRIV") +
      count_of(body_of(ino, "static bool nvs_store_key(const uint8_t priv[32]) {"), "NVS_KEY_PRIV");
  CHECK(in_bodies == 3);
  CHECK(count_of(ino, "NVS_KEY_PRIV") == in_bodies + 1);   // and its definition
  CHECK(count_of(ino, "\"privkey\"") == 1);
  return 0;
}

// The chirp reads (F220): setup() calls chirp_channel::init(), which loads
// the settings, before loop() runs; the stamp read the cut took is loop()'s;
// NvsSession's read-only open is the quiet one, and the convenience readers
// open read-only through it; no sketch source opens "chirp" read-only any
// other way.
static int test_the_chirp_reads_are_the_quiet_ones() {
  const std::string dir = WAP_SKETCH_DIR;
  const std::string ino = read_file(dir + "/canary_wap.ino");
  const std::string chirp = read_file(dir + "/chirp_channel.cpp");
  const std::string store = read_file(dir + "/nvs_store.h");
  CHECK(!ino.empty() && !chirp.empty() && !store.empty());
  CHECK(body_of(ino, "void setup() {").find("chirp_channel::init()") != std::string::npos);
  CHECK(body_of(chirp, "bool init() {").find("\n  load_settings();") != std::string::npos);
  CHECK(body_of(ino, "void loop() {").find("nvs_get_u32(\"st_chirp_at\", &persisted)") !=
        std::string::npos);
  const size_t cls = store.find("\nclass NvsSession {");
  const size_t cls_end = store.find("\n};\n", cls);
  CHECK(cls != std::string::npos && cls_end != std::string::npos);
  const std::string session = store.substr(cls, cls_end - cls);
  CHECK(session.find("if (readOnly) return csi_module_settings_nvs::begin_read_only(m_prefs, ns);") !=
        std::string::npos);
  CHECK(session.find("m_open = m_prefs.begin(") == std::string::npos);
  CHECK(body_of(store, "inline bool nvs_get_u8(const char* key, uint8_t* out_val) {")
            .find("NvsSession nvs(NVS_CHIRP_NS, true);") != std::string::npos);
  CHECK(body_of(store, "inline bool nvs_get_u32(const char* key, uint32_t* out_val) {")
            .find("NvsSession nvs(NVS_CHIRP_NS, true);") != std::string::npos);
  for (const char* plain : {"begin(NVS_CHIRP_NS, true)", "begin(\"chirp\", true)"}) {
    CHECK(ino.find(plain) == std::string::npos);
    CHECK(chirp.find(plain) == std::string::npos);
    CHECK(store.find(plain) == std::string::npos);
  }
  return 0;
}

int main() {
  // nvs_store.h names every namespace the sketch uses; this suite opens two.
  (void)NVS_MESH_NS;
  int rc = 0;
  rc |= test_a_first_boot_after_an_erase_logs_no_securacv_line();
  rc |= test_a_provisioned_boot_reads_as_before();
  rc |= test_a_boot_that_stored_no_key_logs_nothing();
  rc |= test_an_nvs_fault_keeps_its_lines();
  rc |= test_a_stored_key_loads_unchanged();
  rc |= test_a_namespace_with_no_key_generates_one();
  rc |= test_a_read_that_fails_after_the_length_check_is_refused();
  rc |= test_a_garbled_or_short_read_is_refused_and_zeroed();
  rc |= test_a_failed_length_lookup_is_refused();
  rc |= test_a_wrong_length_key_is_refused();
  rc |= test_a_session_that_will_not_open_is_refused();
  rc |= test_a_zero_key_is_refused();
  rc |= test_a_key_with_one_byte_set_loads();
  rc |= test_a_refused_boot_keeps_the_key_for_the_next();
  rc |= test_the_boot_runs_these_opens_in_this_order();
  rc |= test_a_refused_key_halts_the_boot();
  rc |= test_a_first_boot_logs_no_chirp_line();
  rc |= test_stored_chirp_settings_read_as_before();
  rc |= test_an_nvs_fault_keeps_its_chirp_lines();
  rc |= test_the_chirp_reads_are_the_quiet_ones();
  if (rc != 0) {
    std::fprintf(stderr, "test_wap_first_boot_nvs: FAILED\n");
    return 1;
  }
  std::printf("test_wap_first_boot_nvs: ALL %d CHECKS PASSED\n", g_checks);
  return 0;
}
