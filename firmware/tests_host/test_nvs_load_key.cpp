/* Host tests for the flagship's identity-key load (repo sweep F360), run on
 * the firmware's own code: nvs_load_key(), stored_key_length(),
 * nvs_store_key(), key_at_rest_facts() and the NvsManager session and blob
 * calls under them, cut out of
 * firmware/canary/lib/securacv_crypto/src/securacv_crypto.cpp, and
 * establish_identity_key(), cut out of
 * firmware/canary/lib/securacv_witness/src/securacv_witness.cpp, all verbatim
 * by cut_functions.awk (the Makefile writes build/nvs_load_key.inc), over a
 * fake NVS (stubs/nvs_key) that Preferences and IDF's nvs_open() /
 * nvs_get_blob() read alike, and a stand-in keygen.
 *
 * THE FAILURE THIS PREVENTS. nvs_load_key() checked the stored key's length,
 * then read it with Preferences::getBytes() and discarded the count it
 * answered. The Arduino core leaves the buffer as it was when that read
 * fails, so an NVS error between the two reads ran the boot on the zeroed
 * DeviceIdentity key: a signing key anyone holds, whose setup-network key is
 * the published known answer cv-3jByJ6rHdhM2 (test_ap_password.cpp), and
 * from which, on a unit with no stored token, the API token is derived too.
 * And every false meant "no key":
 * a failed open or a failed length lookup fell through to a new keypair,
 * stored over the identity the unit had.
 *
 * Now the load answers Loaded, Absent (IDF says nothing is stored: no
 * namespace, or no key in it) or Refused (the buffer zeroed), and
 * establish_identity_key() generates only on Absent. Refused stops
 * provisioning, so setup() halts (pinned below), and NVS is left byte for
 * byte as it was; the next boot that can read the key loads it.
 *
 * Built twice. As is, the default image. With
 * -DSECURACV_REQUIRE_FLASH_ENCRYPTION=1, the opt-in image, whose policy
 * (common/identity/key_at_rest.h) refuses every load and store on this tree:
 * there the load is Refused before NVS is read, and no key is generated.
 *
 * It also reads the canary's sources (argv): witness_provision_device()
 * returns false on establish_identity_key() before it derives anything from
 * the key, setup() runs canary_pe::on_boot() (which makes the namespace)
 * before it provisions and halts on that false, the one call of
 * nvs_load_key() is establish_identity_key()'s, and only securacv_crypto.cpp's
 * load, probe and store name the key's NVS entry.
 *
 * What this does not prove: ESP-IDF's NVS or Arduino's Preferences
 * themselves (the fakes answer as their 2.0.17 sources read), or anything on
 * a device. The canary envs compile the real thing in CI; it has not run on
 * a bench.
 *
 * Build & run: make -C firmware/tests_host (the cut needs the Makefile).
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Arduino.h"           // stubs/nvs_key: a Serial that keeps every line
#include "nvs.h"               // stubs/nvs_key: IDF's NVS over the Preferences fake's store
#include "securacv_crypto.h"   // the real KeyLoad, NvsManager and declarations
#include "securacv_witness.h"  // the real DeviceIdentity
#include "canary_config.h"     // NVS_MAIN_NS, NVS_KEY_PRIV, SECURACV_REQUIRE_FLASH_ENCRYPTION
#include "identity/key_at_rest.h"

static int g_checks = 0;
static int g_failures = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    g_checks++;                                                          \
    if (!(cond)) {                                                       \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

// NvsManager's constructor is private and instance() is its only way in; the
// firmware's is a one-line static local, and so is this one.
NvsManager& NvsManager::instance() {
  static NvsManager s_instance;
  return s_instance;
}

// securacv_witness.cpp's own file-scope identity.
static DeviceIdentity g_device;

// The keygen establish_identity_key() calls: a fixed, non-zero key, so a test
// can tell a generated key from a stored one, and a refusal on demand.
static int g_keygens = 0;
static bool g_keygen_fails = false;
static uint8_t generated_byte(size_t i) { return (uint8_t)(0xC0 ^ (i * 5)); }
bool crypto_generate_keypair(uint8_t priv[32], uint8_t pub[32]) {
  g_keygens++;
  if (g_keygen_fails) return false;
  for (size_t i = 0; i < 32; i++) {
    priv[i] = generated_byte(i);
    pub[i] = (uint8_t)~generated_byte(i);
  }
  return true;
}

#include "nvs_load_key.inc"

// ── the world each test starts from ─────────────────────────────────────────

static std::vector<uint8_t> good_key() {
  std::vector<uint8_t> k(32);
  for (size_t i = 0; i < 32; i++) k[i] = (uint8_t)(i * 7 + 3);
  return k;
}
// Both unused in the opt-in build, which reads and generates no key.
[[maybe_unused]] static std::vector<uint8_t> generated_key() {
  std::vector<uint8_t> k(32);
  for (size_t i = 0; i < 32; i++) k[i] = generated_byte(i);
  return k;
}

static void fresh_world() {
  host_key_nvs() = HostKeyNvs();
  host_key_nvs().ns = NVS_MAIN_NS;
  Serial.lines.clear();
  g_device = DeviceIdentity();
  g_keygens = 0;
  g_keygen_fails = false;
  g_host_millis = 4242;
}

static void store(const std::vector<uint8_t>& key) {
  host_key_nvs().ns_exists = true;
  host_key_nvs().blobs[NVS_KEY_PRIV] = key;
}

[[maybe_unused]] static bool equals(const uint8_t* a, const std::vector<uint8_t>& b) {
  return b.size() == 32 && std::memcmp(a, b.data(), 32) == 0;
}
static bool all_zero(const uint8_t* a) {
  for (size_t i = 0; i < 32; i++)
    if (a[i] != 0) return false;
  return true;
}
static bool printed(const char* needle) {
  for (const std::string& l : Serial.lines)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

// After every call: no NVS session, the lock free, no handle left open, and
// nothing the fakes count as a fault.
static void check_idle_and_clean() {
  const HostKeyNvs& n = host_key_nvs();
  CHECK(!NvsManager::instance().isOpen());
  CHECK(g_host_mutexes[0].count == 0 && g_host_mutexes[0].owner == -1);
  CHECK(g_host_mutexes[0].bad_gives == 0);
  CHECK(n.prefs_open == 0);
  CHECK(n.idf_open_handles == 0 && n.idf_opens >= n.idf_closes);
  CHECK(n.idf_rw_opens == 0);      // the load writes nothing through IDF
  CHECK(n.double_begins == 0);
  CHECK(n.wrong_namespace == 0);
}

// A refused boot: establish_identity_key() answers false, nothing was
// generated or written, the store is what it was, the identity buffer holds
// no key, and the two lines say why.
static void check_refused_boot(const HostKeyNvs& before, const char* reason) {
  const int rw_before = host_key_nvs().rw_begins;
  const int puts_before = host_key_nvs().puts;
  Serial.lines.clear();
  CHECK(!establish_identity_key());
  CHECK(g_keygens == 0);
  CHECK(host_key_nvs().rw_begins == rw_before);
  CHECK(host_key_nvs().puts == puts_before);
  CHECK(host_key_nvs().same_store(before));
  CHECK(all_zero(g_device.privkey));
  CHECK(!g_device.key_is_new);
  CHECK(printed(reason));
  CHECK(printed("[!!] Identity key not used: no new key is made in its place"));
  CHECK(!printed("Generating new keypair"));
  check_idle_and_clean();
}

#if SECURACV_REQUIRE_FLASH_ENCRYPTION

// ════════════════════════════════════════════════════════════════════════════
// The opt-in image: the policy refuses every load, so nothing is read,
// generated or written, stored key or none.
// ════════════════════════════════════════════════════════════════════════════

static const char kPolicyLine[] =
    "[!!] identity key not loaded: flash encryption required by this build but not active";

static void test_the_policy_refuses_a_stored_key_before_reading_it() {
  fresh_world();
  store(good_key());
  const HostKeyNvs before = host_key_nvs();
  uint8_t priv[32];
  std::memset(priv, 0xEE, sizeof(priv));
  CHECK(nvs_load_key(priv) == KeyLoad::Refused);
  CHECK(all_zero(priv));
  CHECK(host_key_nvs().idf_opens == 0 && host_key_nvs().reads == 0);
  CHECK(printed(kPolicyLine));
  check_idle_and_clean();
  check_refused_boot(before, kPolicyLine);
}

// Before F360 this answered "no key": a keypair was generated, and only the
// store's own refusal kept it out of NVS.
static void test_the_policy_refuses_with_no_key_and_generates_none() {
  fresh_world();
  const HostKeyNvs before = host_key_nvs();
  uint8_t priv[32];
  CHECK(nvs_load_key(priv) == KeyLoad::Refused);
  CHECK(all_zero(priv));
  check_refused_boot(before, kPolicyLine);
  CHECK(!host_key_nvs().ns_exists && host_key_nvs().blobs.empty());
}

#else

// ════════════════════════════════════════════════════════════════════════════
// The default image.
// ════════════════════════════════════════════════════════════════════════════

// ── a first boot: nothing stored, so a key is generated and stored ──────────
//
// No namespace at all, which setup() never reaches: canary_pe::on_boot()
// runs first on every boot and opens the namespace read-write, so a real
// first boot meets a namespace with no key (the next case). This is the
// fault-free erase with nothing in front of the load: IDF says nothing is
// stored, and the load answers Absent without a Preferences open.
static void test_a_first_boot_generates_and_stores_a_key() {
  fresh_world();
  uint8_t priv[32];
  std::memset(priv, 0xEE, sizeof(priv));
  CHECK(nvs_load_key(priv) == KeyLoad::Absent);
  CHECK(all_zero(priv));
  CHECK(host_key_nvs().failed_begins == 0 && host_key_nvs().ro_begins == 0);
  check_idle_and_clean();

  Serial.lines.clear();
  CHECK(establish_identity_key());
  CHECK(g_keygens == 1);
  CHECK(host_key_nvs().ns_exists);
  CHECK(host_key_nvs().blobs[NVS_KEY_PRIV] == generated_key());
  CHECK(equals(g_device.privkey, generated_key()));
  CHECK(g_device.key_is_new && g_device.key_born_ms == 4242);
  CHECK(printed("[..] Generating new keypair..."));
  CHECK(printed("[OK] New keypair generated and stored"));
  CHECK(host_key_nvs().failed_begins == 0);
  check_idle_and_clean();

  // The next boot loads that key, and generates nothing.
  g_device = DeviceIdentity();
  Serial.lines.clear();
  CHECK(establish_identity_key());
  CHECK(g_keygens == 1);
  CHECK(equals(g_device.privkey, generated_key()));
  CHECK(!g_device.key_is_new);
  CHECK(printed("[OK] Loaded existing keypair from NVS"));
  check_idle_and_clean();
}

// The namespace is there but holds no key: a real first boot (on_boot()
// made the namespace), or the 30 s BOOT wipe (it clears the namespace's keys
// and leaves it). Absent too, so a key is generated, and IDF's lookup logs
// nothing where Preferences' getBytesLength() logged "nvs_get_blob len fail:
// privkey NOT_FOUND".
static void test_a_namespace_with_no_key_generates_one() {
  fresh_world();
  host_key_nvs().ns_exists = true;
  host_key_nvs().blobs["boots"] = {1, 0, 0, 0};
  uint8_t priv[32];
  CHECK(nvs_load_key(priv) == KeyLoad::Absent);
  CHECK(all_zero(priv));
  CHECK(host_key_nvs().ro_begins == 0 && host_key_nvs().failed_begins == 0);   // no Preferences read
  check_idle_and_clean();
  CHECK(establish_identity_key());
  CHECK(g_keygens == 1);
  CHECK(host_key_nvs().blobs[NVS_KEY_PRIV] == generated_key());
  CHECK(host_key_nvs().blobs["boots"] == std::vector<uint8_t>({1, 0, 0, 0}));
  check_idle_and_clean();
}

// ── a good stored key loads unchanged, and nothing is written ───────────────
static void test_a_good_stored_key_loads_unchanged() {
  fresh_world();
  store(good_key());
  const HostKeyNvs before = host_key_nvs();
  uint8_t priv[32];
  CHECK(nvs_load_key(priv) == KeyLoad::Loaded);
  CHECK(equals(priv, good_key()));
  check_idle_and_clean();

  Serial.lines.clear();
  CHECK(establish_identity_key());
  CHECK(equals(g_device.privkey, good_key()));
  CHECK(g_keygens == 0 && host_key_nvs().puts == 0 && host_key_nvs().rw_begins == 0);
  CHECK(host_key_nvs().same_store(before));
  CHECK(!g_device.key_is_new);
  CHECK(Serial.lines.size() == 1 && printed("[OK] Loaded existing keypair from NVS"));
  check_idle_and_clean();
}

// ── THE F360 CASE: the read fails after the length lookup said 32 ───────────
//
// Preferences::getBytes() answers 0 and leaves the buffer as it was. Before,
// the load answered true over the zeroed buffer, and the boot ran on the
// all-zero key. Now it is Refused, the boot stops, and this firmware writes
// nothing, so a fault that leaves the entry in place (Read::Fails: a flash
// read error, say) leaves the key for the next boot that can read it. (IDF
// itself erases an entry whose data CRC fails, inside that read; this fake
// does not model that.)
static void test_a_failed_read_after_the_length_check_is_refused() {
  fresh_world();
  store(good_key());
  host_key_nvs().read = HostKeyNvs::Read::Fails;
  const HostKeyNvs before = host_key_nvs();
  uint8_t priv[32];
  std::memset(priv, 0, sizeof(priv));  // the zeroed DeviceIdentity key
  CHECK(nvs_load_key(priv) == KeyLoad::Refused);
  CHECK(all_zero(priv));
  CHECK(host_key_nvs().reads == 1);   // the length lookup passed; the read itself failed
  CHECK(printed("[!!] identity key not loaded: the stored key could not be read (0 of 32 bytes)"));
  check_idle_and_clean();

  check_refused_boot(before,
                     "[!!] identity key not loaded: the stored key could not be read (0 of 32 bytes)");

  // The entry the fault left is there for the next boot.
  host_key_nvs().clear_faults();
  g_device = DeviceIdentity();
  CHECK(establish_identity_key());
  CHECK(equals(g_device.privkey, good_key()));
  CHECK(g_keygens == 0 && host_key_nvs().puts == 0);
  check_idle_and_clean();
}

// A read that wrote into the buffer and then failed (IDF fills it before its
// data CRC check refuses) leaves no part of those bytes behind.
static void test_a_garbled_read_is_refused_and_zeroed() {
  fresh_world();
  store(good_key());
  host_key_nvs().read = HostKeyNvs::Read::Garbles;
  const HostKeyNvs before = host_key_nvs();
  uint8_t priv[32];
  CHECK(nvs_load_key(priv) == KeyLoad::Refused);
  CHECK(all_zero(priv));
  check_idle_and_clean();
  check_refused_boot(before, "the stored key could not be read (0 of 32 bytes)");
}

// A read that returned fewer than 32 bytes is not the key.
static void test_a_short_read_is_refused() {
  fresh_world();
  store(good_key());
  host_key_nvs().read = HostKeyNvs::Read::Short;
  const HostKeyNvs before = host_key_nvs();
  uint8_t priv[32];
  CHECK(nvs_load_key(priv) == KeyLoad::Refused);
  CHECK(all_zero(priv));
  check_idle_and_clean();
  check_refused_boot(before, "the stored key could not be read (31 of 32 bytes)");
}

// ── a stored key of 32 zero bytes is refused ───────────────────────────────
static void test_an_all_zero_stored_key_is_refused() {
  fresh_world();
  store(std::vector<uint8_t>(32, 0));
  const HostKeyNvs before = host_key_nvs();
  uint8_t priv[32];
  CHECK(nvs_load_key(priv) == KeyLoad::Refused);
  CHECK(all_zero(priv));
  check_idle_and_clean();
  check_refused_boot(before, "[!!] identity key not loaded: the stored key is 32 zero bytes");
}

// One non-zero byte anywhere is a key the load hands back.
static void test_a_key_with_one_non_zero_byte_loads() {
  for (size_t at : {(size_t)0, (size_t)17, (size_t)31}) {
    fresh_world();
    std::vector<uint8_t> k(32, 0);
    k[at] = 0x01;
    store(k);
    uint8_t priv[32];
    CHECK(nvs_load_key(priv) == KeyLoad::Loaded);
    CHECK(equals(priv, k));
    check_idle_and_clean();
  }
}

// ── a stored key of the wrong length is refused, not replaced ──────────────
//
// Before, any length but 32 answered false, and a new keypair was stored
// over it.
static void test_a_wrong_length_key_is_refused_not_replaced() {
  for (size_t len : {(size_t)1, (size_t)16, (size_t)31, (size_t)33, (size_t)64}) {
    fresh_world();
    store(std::vector<uint8_t>(len, 0x5A));
    const HostKeyNvs before = host_key_nvs();
    uint8_t priv[32];
    CHECK(nvs_load_key(priv) == KeyLoad::Refused);
    CHECK(all_zero(priv));
    CHECK(host_key_nvs().reads == 0);
    check_idle_and_clean();
    char line[96];
    std::snprintf(line, sizeof(line), "the stored key is %u bytes, not 32", (unsigned)len);
    check_refused_boot(before, line);
  }
}

// ── NVS that cannot say whether a key is stored is not "no key" ─────────────
//
// A read-only open that fails with a fault (here no memory for the handle)
// while a write would still land: before, the load's false led to a new
// keypair, and the store wrote it over the old identity.
static void test_a_failed_open_is_refused_not_replaced() {
  for (esp_err_t fault : {ESP_ERR_NO_MEM, ESP_ERR_NVS_NOT_INITIALIZED, ESP_FAIL}) {
    fresh_world();
    store(good_key());
    host_key_nvs().ro_open_fault = fault;
    const HostKeyNvs before = host_key_nvs();
    uint8_t priv[32];
    CHECK(nvs_load_key(priv) == KeyLoad::Refused);
    CHECK(all_zero(priv));
    check_idle_and_clean();
    char line[128];
    std::snprintf(line, sizeof(line),
                  "[!!] identity key not loaded: NVS could not say whether one is stored (%s)",
                  esp_err_to_name(fault));
    check_refused_boot(before, line);
  }
  // The same fault with no namespace at all is still a fault: IDF did not
  // say NOT_FOUND, so the load cannot know nothing is stored.
  fresh_world();
  host_key_nvs().ro_open_fault = ESP_ERR_NO_MEM;
  const HostKeyNvs before = host_key_nvs();
  check_refused_boot(before, "NVS could not say whether one is stored (ESP_ERR_NO_MEM)");
}

// The length lookup failing (a flash read error, or a "privkey" stored as
// another type): getBytesLength() answers 0 for it, exactly as for a missing
// key, which is why the load asks IDF.
static void test_a_failed_length_lookup_is_refused_not_replaced() {
  for (esp_err_t fault : {ESP_FAIL, ESP_ERR_NVS_TYPE_MISMATCH, ESP_ERR_NVS_INVALID_HANDLE}) {
    fresh_world();
    store(good_key());
    host_key_nvs().length_fault = fault;
    const HostKeyNvs before = host_key_nvs();
    uint8_t priv[32];
    CHECK(nvs_load_key(priv) == KeyLoad::Refused);
    CHECK(all_zero(priv));
    check_idle_and_clean();
    check_refused_boot(before, "NVS could not say whether one is stored");
  }
}

// IDF says a 32-byte key is stored, and the Preferences session will not
// open: Refused, and nothing is generated.
static void test_a_session_that_will_not_open_is_refused() {
  fresh_world();
  store(good_key());
  host_key_nvs().begin_fails = true;
  const HostKeyNvs before = host_key_nvs();
  uint8_t priv[32];
  CHECK(nvs_load_key(priv) == KeyLoad::Refused);
  CHECK(all_zero(priv));
  CHECK(host_key_nvs().reads == 0);
  check_idle_and_clean();
  check_refused_boot(before, "the stored key could not be read (0 of 32 bytes)");
}

// Every refusal leaves the caller's buffer zeroed, whatever it held.
static void test_every_refusal_zeroes_a_dirty_buffer() {
  for (int c = 0; c < 6; c++) {
    fresh_world();
    store(c == 3 ? std::vector<uint8_t>(32, 0) : good_key());
    if (c == 0) host_key_nvs().read = HostKeyNvs::Read::Fails;
    if (c == 1) host_key_nvs().read = HostKeyNvs::Read::Garbles;
    if (c == 2) host_key_nvs().read = HostKeyNvs::Read::Short;
    if (c == 4) host_key_nvs().ro_open_fault = ESP_ERR_NO_MEM;
    if (c == 5) host_key_nvs().length_fault = ESP_FAIL;
    uint8_t priv[32];
    std::memset(priv, 0xEE, sizeof(priv));
    CHECK(nvs_load_key(priv) == KeyLoad::Refused);
    CHECK(all_zero(priv));
    check_idle_and_clean();
  }
}

// ── what a first boot already did when the keygen or the store fails ───────
static void test_a_failed_keygen_or_store_writes_no_key() {
  fresh_world();
  g_keygen_fails = true;
  CHECK(!establish_identity_key());
  CHECK(g_keygens == 1 && host_key_nvs().puts == 0 && host_key_nvs().blobs.empty());
  CHECK(printed("[!!] Keypair generation failed"));
  check_idle_and_clean();

  fresh_world();
  host_key_nvs().begin_fails = true;   // no namespace yet, and no session opens
  CHECK(!establish_identity_key());
  CHECK(g_keygens == 1 && host_key_nvs().blobs.empty());
  CHECK(printed("[!!] Failed to store keypair"));
  check_idle_and_clean();
}

#endif  // SECURACV_REQUIRE_FLASH_ENCRYPTION

// ════════════════════════════════════════════════════════════════════════════
// The sources around the cut: the halt, and no second road to the key.
// ════════════════════════════════════════════════════════════════════════════

static std::string read_file(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}
static bool ends_with(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}
static std::string body_of(const std::string& src, const std::string& head) {
  const size_t at = src.find(head);
  if (at == std::string::npos) return "";
  const size_t end = src.find("\n}\n", at);
  return end == std::string::npos ? "" : src.substr(at, end - at + 3);
}
static size_t count_of(const std::string& s, const std::string& needle) {
  size_t n = 0;
  for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) n++;
  return n;
}
// A line that is code, not a comment (the sources' comments name these
// functions in prose).
static bool is_code(const std::string& line) {
  const size_t first = line.find_first_not_of(" \t");
  if (first == std::string::npos) return false;
  return line.compare(first, 2, "//") != 0 && line.compare(first, 1, "*") != 0 &&
         line.compare(first, 2, "/*") != 0;
}

static void test_the_sources_halt_and_keep_one_road(const std::vector<std::string>& files) {
  std::string witness, main_cpp;
  for (const std::string& f : files) {
    if (ends_with(f, "lib/securacv_witness/src/securacv_witness.cpp")) witness = read_file(f);
    if (ends_with(f, "canary/src/main.cpp")) main_cpp = read_file(f);
  }
  CHECK(!witness.empty() && !main_cpp.empty());

  // witness_provision_device() stops on a refused key before it names,
  // derives or stores anything from it, and reaches the key only through
  // establish_identity_key().
  const std::string prov = body_of(witness, "\nbool witness_provision_device() {\n");
  CHECK(!prov.empty());
  const size_t stop = prov.find("\n  if (!establish_identity_key()) return false;\n");
  CHECK(stop != std::string::npos);
  CHECK(count_of(prov, "establish_identity_key()") == 1);
  for (const char* later : {"crypto_print_key_at_rest(", "Ed25519::derivePublicKey(",
                            "generate_device_id(", "nvs_load_u32(", "nvs_store_u32(",
                            "auth_load_or_derive(", "g_device.initialized = true"}) {
    const size_t at = prov.find(later);
    CHECK(at != std::string::npos && at > stop);
  }
  CHECK(prov.find("nvs_load_key(") == std::string::npos);
  CHECK(prov.find("crypto_generate_keypair(") == std::string::npos);
  CHECK(prov.find("nvs_store_key(") == std::string::npos);

  // setup() runs canary_pe::on_boot(), whose read-write open makes the
  // namespace, before it provisions: a first boot's key read meets a
  // namespace with no key in it.
  const size_t pe_boot = main_cpp.find("\n  canary_pe::on_boot();\n");
  CHECK(pe_boot != std::string::npos &&
        pe_boot < main_cpp.find("  if (!witness_provision_device()) {\n"));

  // setup() halts on that false, and provisions once.
  CHECK(main_cpp.find("  if (!witness_provision_device()) {\n"
                      "    Serial.println(\"[!!] Device provisioning failed - HALTING\");\n"
                      "    while (true) { delay(1000); }\n"
                      "  }\n") != std::string::npos);
  CHECK(count_of(main_cpp, "witness_provision_device(") == 1);

  // One call of nvs_load_key() in the canary tree, establish_identity_key()'s,
  // and only securacv_crypto.cpp's probe, load and store name the key's NVS
  // entry (canary_config.h defines it; reset_scope_keys.h lists it as kept).
  const std::string est = body_of(witness, "\nstatic bool establish_identity_key() {\n");
  CHECK(count_of(est, "nvs_load_key(g_device.privkey)") == 1);
  int load_calls = 0, crypto_names = 0, unread = 0;
  for (const std::string& f : files) {
    const std::string src = read_file(f);
    if (src.empty()) {
      std::printf("  could not read %s\n", f.c_str());
      unread++;
    }
    std::istringstream lines(src);
    std::string line;
    while (std::getline(lines, line)) {
      if (!is_code(line)) continue;
      if (line.find("nvs_load_key(") != std::string::npos &&
          line.find("KeyLoad nvs_load_key(uint8_t priv[32])") == std::string::npos) {
        load_calls++;
        CHECK(ends_with(f, "lib/securacv_witness/src/securacv_witness.cpp"));
      }
      if (line.find("NVS_KEY_PRIV") != std::string::npos ||
          line.find("\"privkey\"") != std::string::npos) {
        if (ends_with(f, "lib/securacv_crypto/src/securacv_crypto.cpp")) {
          crypto_names++;
        } else {
          const bool allowed = ends_with(f, "canary/include/canary_config.h") ||
                               ends_with(f, "canary/include/reset_scope_keys.h");
          if (!allowed) std::printf("  NVS_KEY_PRIV named in %s: %s\n", f.c_str(), line.c_str());
          CHECK(allowed);
        }
      }
    }
  }
  CHECK(unread == 0 && files.size() > 50);
  CHECK(load_calls == 1);
  CHECK(crypto_names == 3);  // stored_key_length(), nvs_load_key(), nvs_store_key()
}

int main(int argc, char** argv) {
#if SECURACV_REQUIRE_FLASH_ENCRYPTION
  test_the_policy_refuses_a_stored_key_before_reading_it();
  test_the_policy_refuses_with_no_key_and_generates_none();
  const char* build = "opt-in image (SECURACV_REQUIRE_FLASH_ENCRYPTION=1)";
#else
  test_a_first_boot_generates_and_stores_a_key();
  test_a_namespace_with_no_key_generates_one();
  test_a_good_stored_key_loads_unchanged();
  test_a_failed_read_after_the_length_check_is_refused();
  test_a_garbled_read_is_refused_and_zeroed();
  test_a_short_read_is_refused();
  test_an_all_zero_stored_key_is_refused();
  test_a_key_with_one_non_zero_byte_loads();
  test_a_wrong_length_key_is_refused_not_replaced();
  test_a_failed_open_is_refused_not_replaced();
  test_a_failed_length_lookup_is_refused_not_replaced();
  test_a_session_that_will_not_open_is_refused();
  test_every_refusal_zeroes_a_dirty_buffer();
  test_a_failed_keygen_or_store_writes_no_key();
  const char* build = "default image";
#endif
  std::vector<std::string> files(argv + 1, argv + argc);
  CHECK(!files.empty());
  test_the_sources_halt_and_keep_one_road(files);

  if (g_failures) {
    std::printf("test_nvs_load_key (%s): %d of %d checks FAILED\n", build, g_failures, g_checks);
    return 1;
  }
  std::printf("test_nvs_load_key (%s): OK, %d checks\n", build, g_checks);
  return 0;
}
