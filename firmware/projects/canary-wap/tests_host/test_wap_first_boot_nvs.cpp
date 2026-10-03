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
// Build/run: make -C firmware/projects/canary-wap/tests_host run

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "Arduino.h"
#include "Preferences.h"
#include "csi_module_settings_nvs.h"
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

// ── canary_wap.ino's key read and store, cut verbatim ───────────────────
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

// ── A boot, in setup()'s order ──────────────────────────────────────────
struct BootResult {
  bool key_loaded = false;
  bool key_stored = false;
};

// A reboot: RAM gone, NVS kept. `provisions` false models a boot whose
// provisioning stored no key (provision_device() returns before
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

  BootResult r;
  setup_wizard::init();                          // setup()'s first "securacv" open
  uint8_t priv[32];
  r.key_loaded = nvs_load_key(priv);             // provision_device()
  if (!r.key_loaded && provisions) {
    std::memset(priv, 0x5a, sizeof(priv));       // generate_keypair()
    r.key_stored = nvs_store_key(priv);
  }
  power_monitor::load_nvs_state();               // power_monitor::init()
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

// An NVS fault is not a missing namespace: every open still goes to
// Preferences and keeps its error line, as before.
static int test_an_nvs_fault_keeps_its_lines() {
  host_nvs().clear();
  host_nvs().fail_begin = true;
  const BootResult r = boot();
  CHECK(!r.key_loaded);
  CHECK(!r.key_stored);
  CHECK(host_nvs().error_logs == 4);             // wizard, key read, key store, power
  CHECK(setup_wizard::s_first_boot);
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

  // provision_device() reads the key before it stores one.
  const std::string prov = body_of(ino, "static bool provision_device() {");
  const size_t rd = prov.find("nvs_load_key(");
  const size_t wr = prov.find("nvs_store_key(");
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
  const std::string load_key = body_of(ino, "static bool nvs_load_key(uint8_t priv[32]) {");
  const size_t probe = load_key.find("probe_namespace(NVS_MAIN_NS)");
  const size_t session = load_key.find("NvsMainSession nvs(true);");
  CHECK(probe != std::string::npos && session != std::string::npos && probe < session);
  CHECK(wizard.find("begin(\"securacv\", true)") == std::string::npos);
  CHECK(power.find("begin(\"securacv\", true)") == std::string::npos);
  return 0;
}

int main() {
  // nvs_store.h names every namespace the sketch uses; this suite opens one.
  (void)NVS_CHIRP_NS;
  (void)NVS_MESH_NS;
  int rc = 0;
  rc |= test_a_first_boot_after_an_erase_logs_no_securacv_line();
  rc |= test_a_provisioned_boot_reads_as_before();
  rc |= test_a_boot_that_stored_no_key_logs_nothing();
  rc |= test_an_nvs_fault_keeps_its_lines();
  rc |= test_the_boot_runs_these_opens_in_this_order();
  if (rc != 0) {
    std::fprintf(stderr, "test_wap_first_boot_nvs: FAILED\n");
    return 1;
  }
  std::printf("test_wap_first_boot_nvs: ALL %d CHECKS PASSED\n", g_checks);
  return 0;
}
