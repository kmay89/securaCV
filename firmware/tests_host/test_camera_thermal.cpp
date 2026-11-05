/* Host tests for the camera's thermal step (repo sweep F432): the pure
 * header CameraManager::checkThermal() calls
 * (firmware/canary/lib/securacv_camera/src/camera_thermal.h), linked with
 * the shared die-temperature provider
 * (firmware/canary/lib/securacv_thermal/src/securacv_thermal.cpp) compiled
 * as is, once per chip target, over stubs/idf_tsens: the sdkconfig.h that
 * names the target (IDF defines CONFIG_IDF_TARGET_* there and nowhere on
 * the compiler's command line), the esp_idf_version.h that picks the driver
 * header, and a fake of that driver this suite drives. Three builds:
 *
 *   esp32          the classic ESP32 of env:esp32cam and env:esp32-wroom:
 *                  no sensor, the provider is its stub;
 *   esp32s3_idf4   the ESP32-S3 on ESP-IDF 4.4.7, the stock units' Arduino
 *                  2.0.17 core (driver/temp_sensor.h);
 *   esp32s3_idf5   the ESP32-S3 on ESP-IDF 5.5.4, [env:full]'s core 3
 *                  (driver/temperature_sensor.h).
 *
 * The provider installs its driver on the first read and never retries a
 * failed install, so the S3 binaries run once per driver mode
 * (--tsens=ok, --tsens=install-fails, --tsens=enable-fails).
 *
 * THE FAILURES THIS PREVENTS. On the classic ESP32 thermal_read_die_c()
 * always answers false, and checkThermal() counted each false as a sensor
 * miss: the third throttled the first PEEK stream after boot and logged
 * "Camera thermal sensor read failing" for a sensor the chip does not
 * have, while getThermalSensorOk() said true until then. resetMetrics()
 * reset the state and not the count, so the fail-safe fired on the first
 * stream only, and on a chip whose sensor really fails, a later stream ran
 * unthrottled and blind. And the provider tested CONFIG_IDF_TARGET_* before
 * anything had included sdkconfig.h, so it saw no target on any chip and
 * compiled the sensorless stub on the S3 too: every stock unit's reads were
 * misses, which the first fix would have turned from a fail-safe throttle
 * into no thermal protection at all.
 *
 * Now the provider includes sdkconfig.h first and says whether there is a
 * sensor (thermal_sensor_present()), the step reads nothing and counts
 * nothing when there is none, a reset clears the count, sensor_ok() is
 * false on a sensorless chip, and on the S3 the reads go through the
 * driver.
 *
 * It also reads the sources (argv): checkThermal() reads only through the
 * step with the provider's own adapter, resetMetrics() resets the step,
 * getThermalSensorOk() asks the provider, and the provider includes
 * sdkconfig.h before it names a sensor on the S3, S2 and C3.
 *
 * What this does not prove: ESP-IDF's real temperature-sensor driver (the
 * fakes keep its headers' shapes, from the pinned cores' own copies), the
 * camera, or anything on a device. The canary envs compile the real thing
 * in CI, the S3 driver branch for the first time; it has not run on a bench.
 *
 * Build & run: make -C firmware/tests_host
 */
#include <sdkconfig.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "camera_thermal.h"
#include "securacv_thermal.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include <esp_idf_version.h>

#include "freertos/semphr.h"   // the provider's lock, to see it balanced
#include "host_tsens.h"        // the fake driver
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
extern "C" {
#include <driver/temp_sensor.h>   // TSENS_DAC_L2, the offset the provider configures
}
#endif
#endif

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

// A provider the test drives: whether the chip has a sensor, what each read
// answers, and how many reads were asked for.
struct FakeSensor {
  bool has_sensor = true;
  bool read_ok = true;
  float temp_c = 40.0f;
  mutable int reads = 0;
  bool present() const { return has_sensor; }
  bool read(float* c) const {
    reads++;
    if (!read_ok) return false;
    *c = temp_c;
    return true;
  }
};

using camera_thermal::Step;
using camera_thermal::Tracker;

// ════════════════════════════════════════════════════════════════════════════
// The provider on a chip with no sensor (the esp32 build).
// ════════════════════════════════════════════════════════════════════════════

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
static void test_a_chip_with_no_sensor_has_none() {
  CHECK(!thermal_sensor_present());
  float c = 12.5f;
  CHECK(!thermal_read_die_c(&c));
  CHECK(c == 12.5f);                     // left untouched
  CHECK(!thermal_read_die_c(nullptr));
}

// The camera's own adapter over that provider, through as many checks as a
// long stream makes: nothing read, nothing missed, never throttled.
static void test_a_sensorless_chip_never_throttles() {
  Tracker t = camera_thermal::initial();
  const camera_thermal::DieSensor die{};
  for (int i = 0; i < 1000; i++) {
    const Step s = camera_thermal::check(t, die);
    CHECK(!s.changed && !s.fail_safe && !s.recovered);
  }
  CHECK(t.state == THERMAL_NORMAL && t.fail_count == 0 && t.die_temp_c == 0);
  CHECK(!camera_thermal::sensor_ok(t, thermal_sensor_present()));   // nothing to vouch for
  CHECK(camera_thermal::frame_delay_ms(t.state, 40) == 40);
}
#endif

// The step over a counting provider that says it has no sensor: not one read
// is asked for, and a stream's reset changes nothing.
static void test_a_provider_with_no_sensor_is_never_read() {
  Tracker u = camera_thermal::initial();
  FakeSensor none;
  none.has_sensor = false;
  none.read_ok = false;
  for (int stream = 0; stream < 5; stream++) {
    camera_thermal::reset(u);
    for (int i = 0; i < 50; i++) {
      const Step s = camera_thermal::check(u, none);
      CHECK(s.prev == THERMAL_NORMAL && !s.changed && !s.fail_safe);
    }
  }
  CHECK(none.reads == 0);
  CHECK(u.state == THERMAL_NORMAL && u.fail_count == 0);
  CHECK(!camera_thermal::sensor_ok(u, none.present()));
}

// ════════════════════════════════════════════════════════════════════════════
// The provider on the ESP32-S3 (the esp32s3_idf4 and esp32s3_idf5 builds).
// ════════════════════════════════════════════════════════════════════════════

#if defined(CONFIG_IDF_TARGET_ESP32S3)
// What the fake driver does when the provider installs it, from --tsens=.
enum class DriverMode { Ok, InstallFails, EnableFails };
static DriverMode g_mode = DriverMode::Ok;

static void test_the_s3_provider_names_its_sensor() {
  CHECK(thermal_sensor_present());
}

// The driver installs on the first read and stays installed: one install,
// one enable, a read per call, the lock taken and given each time (the 50 ms
// wait), a read the driver refuses a miss with the output untouched, and a
// null output refused before anything is asked of the driver.
static void test_the_s3_provider_reads_through_the_driver() {
  HostTsens& d = host_tsens();
  CHECK(d.installs == 0 && d.reads == 0);
  CHECK(!thermal_read_die_c(nullptr));
  CHECK(d.installs == 0 && d.reads == 0);

  float c = 12.5f;
  d.celsius = 47.25f;
  CHECK(thermal_read_die_c(&c));
  CHECK(c == 47.25f);
  CHECK(d.installs == 1 && d.enables == 1 && d.reads == 1 && d.enabled);
  d.celsius = 61.0f;
  CHECK(thermal_read_die_c(&c) && c == 61.0f);
  CHECK(d.installs == 1 && d.enables == 1 && d.reads == 2);   // installed once

  d.read_fails = true;
  c = 33.0f;
  CHECK(!thermal_read_die_c(&c));
  CHECK(c == 33.0f);
  CHECK(d.installs == 1 && d.reads == 3);
  d.read_fails = false;
  CHECK(thermal_read_die_c(&c) && c == 61.0f);
  CHECK(d.reads == 4 && d.uninstalls == 0 && d.disables == 0);

  CHECK(g_host_tsens_mutexes_created == 1 && g_host_tsens_null_handle_calls == 0);
  const HostTsensMutex& m = g_host_tsens_mutexes[0];
  CHECK(m.count == 0 && m.takes == 4 && m.gives == 4);
  CHECK(m.failed_takes == 0 && m.bad_gives == 0 && m.last_wait == 50);

  // The configuration: IDF 5's 20..100 °C band; IDF 4.4's L2 offset, divider 6.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  CHECK(d.range_min == 20 && d.range_max == 100);
#else
  CHECK(d.dac_offset == (int)TSENS_DAC_L2 && d.clk_div == 6);
#endif
}

// The camera's own adapter over the S3 provider: the hysteresis runs on what
// the driver answers, a refused read counts as a miss, and the fail-safe and
// its recovery are reported.
static void test_the_s3_camera_step_reads_the_die() {
  HostTsens& d = host_tsens();
  Tracker t = camera_thermal::initial();
  const camera_thermal::DieSensor die{};
  d.celsius = 45.0f;
  Step s = camera_thermal::check(t, die);
  CHECK(!s.changed && t.state == THERMAL_NORMAL && t.die_temp_c == 45);
  CHECK(camera_thermal::sensor_ok(t, thermal_sensor_present()));
  d.celsius = 72.0f;
  s = camera_thermal::check(t, die);
  CHECK(s.changed && t.state == THERMAL_THROTTLED && t.die_temp_c == 72);
  CHECK(camera_thermal::frame_delay_ms(t.state, 40) == 120);
  d.celsius = 81.0f;
  s = camera_thermal::check(t, die);
  CHECK(s.changed && t.state == THERMAL_PAUSED);
  d.celsius = 60.0f;
  camera_thermal::check(t, die);
  CHECK(t.state == THERMAL_THROTTLED);   // 60 is under 75: paused to throttled
  camera_thermal::check(t, die);
  CHECK(t.state == THERMAL_NORMAL);      // and under 65: throttled to normal

  d.read_fails = true;
  int fail_safes = 0;
  for (int i = 0; i < 3; i++) fail_safes += camera_thermal::check(t, die).fail_safe ? 1 : 0;
  CHECK(fail_safes == 1 && t.state == THERMAL_THROTTLED);
  CHECK(!camera_thermal::sensor_ok(t, thermal_sensor_present()));
  d.read_fails = false;
  s = camera_thermal::check(t, die);
  CHECK(s.recovered && t.state == THERMAL_NORMAL && t.die_temp_c == 60);
  CHECK(camera_thermal::sensor_ok(t, thermal_sensor_present()));
}

// The chip has a sensor and its driver will not install (or enable): the
// provider gives up after one attempt, every read is a miss with the output
// untouched, and the camera fails safe on every stream, not only the first
// after boot.
static void test_a_driver_that_will_not_start_fails_safe_every_stream() {
  HostTsens& d = host_tsens();
  CHECK(thermal_sensor_present());   // the chip has one; it is the driver that failed
  float c = 12.5f;
  for (int i = 0; i < 5; i++) {
    CHECK(!thermal_read_die_c(&c));
    CHECK(c == 12.5f);
  }
  CHECK(d.installs == 1);   // not retried on every read
  CHECK(!d.enabled && d.reads == 0);
  if (g_mode == DriverMode::EnableFails) {
    CHECK(d.enables == 1);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    CHECK(d.uninstalls == 1 && !d.installed);   // the failed install is torn down
#endif
  } else {
    CHECK(d.enables == 0);
  }

  Tracker t = camera_thermal::initial();
  const camera_thermal::DieSensor die{};
  for (int stream = 0; stream < 3; stream++) {
    camera_thermal::reset(t);
    int fail_safes = 0;
    for (int i = 0; i < 10; i++) fail_safes += camera_thermal::check(t, die).fail_safe ? 1 : 0;
    CHECK(fail_safes == 1 && t.state == THERMAL_THROTTLED);
    CHECK(!camera_thermal::sensor_ok(t, thermal_sensor_present()));
  }
  CHECK(d.installs == 1 && d.reads == 0);

  // The lock was taken and given for every miss.
  CHECK(g_host_tsens_mutexes_created == 1);
  const HostTsensMutex& m = g_host_tsens_mutexes[0];
  CHECK(m.count == 0 && m.takes == 35 && m.gives == 35 && m.bad_gives == 0);
}
#endif

// ════════════════════════════════════════════════════════════════════════════
// A chip with a sensor: the miss counter, the reset, the hysteresis.
// ════════════════════════════════════════════════════════════════════════════

static void test_three_misses_throttle_once() {
  Tracker t = camera_thermal::initial();
  FakeSensor die;
  die.temp_c = 50.0f;
  CHECK(!camera_thermal::check(t, die).changed);   // a good read first
  CHECK(t.die_temp_c == 50 && camera_thermal::sensor_ok(t, true));

  die.read_ok = false;
  for (int miss = 1; miss <= 2; miss++) {
    const Step s = camera_thermal::check(t, die);
    CHECK(!s.fail_safe && !s.changed);
    CHECK(t.state == THERMAL_NORMAL && t.fail_count == miss);
    CHECK(camera_thermal::sensor_ok(t, true));
    CHECK(t.die_temp_c == 50);   // a miss keeps the last reading
  }
  Step s = camera_thermal::check(t, die);
  CHECK(s.fail_safe && s.changed && s.prev == THERMAL_NORMAL);
  CHECK(t.state == THERMAL_THROTTLED && t.fail_count == THERMAL_FAIL_SAFE_COUNT);
  CHECK(!camera_thermal::sensor_ok(t, true));
  // Later misses in the same episode report nothing more.
  for (int i = 0; i < 20; i++) {
    s = camera_thermal::check(t, die);
    CHECK(!s.fail_safe && !s.changed && !s.recovered);
  }
  CHECK(t.state == THERMAL_THROTTLED && t.fail_count == THERMAL_FAIL_SAFE_COUNT);
  CHECK(die.reads == 24);

  // The next good read ends the episode, once, and the hysteresis resumes.
  die.read_ok = true;
  die.temp_c = 50.0f;
  s = camera_thermal::check(t, die);
  CHECK(s.recovered && s.changed && t.state == THERMAL_NORMAL && t.fail_count == 0);
  CHECK(camera_thermal::sensor_ok(t, true));
  s = camera_thermal::check(t, die);
  CHECK(!s.recovered && !s.changed);

  // The fail-safe throttles only from normal: a paused camera stays paused.
  Tracker hot = camera_thermal::initial();
  die.temp_c = 85.0f;
  camera_thermal::check(hot, die);
  CHECK(hot.state == THERMAL_PAUSED);
  die.read_ok = false;
  for (int i = 0; i < 3; i++) s = camera_thermal::check(hot, die);
  CHECK(s.fail_safe && !s.changed && hot.state == THERMAL_PAUSED);
}

// resetMetrics() at each stream start: the count goes with the state, so a
// failing sensor fails safe on every stream, and two misses before a reset
// do not count toward the next stream's third.
static void test_a_reset_clears_the_count() {
  Tracker t = camera_thermal::initial();
  FakeSensor die;
  die.read_ok = false;
  camera_thermal::check(t, die);
  camera_thermal::check(t, die);
  CHECK(t.fail_count == 2);
  camera_thermal::reset(t);
  CHECK(t.state == THERMAL_NORMAL && t.fail_count == 0);
  Step s = camera_thermal::check(t, die);
  CHECK(!s.fail_safe && t.state == THERMAL_NORMAL && t.fail_count == 1);

  for (int stream = 0; stream < 3; stream++) {
    camera_thermal::reset(t);
    int fail_safes = 0;
    for (int i = 0; i < 10; i++) fail_safes += camera_thermal::check(t, die).fail_safe ? 1 : 0;
    CHECK(fail_safes == 1);
    CHECK(t.state == THERMAL_THROTTLED);
  }
}

// The 70/80 °C hysteresis, recovering 5 °C below each entry.
static void test_the_hysteresis_holds() {
  struct Step_ {
    float temp_c;
    ThermalState want;
  };
  const Step_ walk[] = {
      {69, THERMAL_NORMAL},    {70, THERMAL_THROTTLED}, {66, THERMAL_THROTTLED},
      {65, THERMAL_THROTTLED}, {64, THERMAL_NORMAL},    {79, THERMAL_THROTTLED},
      {80, THERMAL_PAUSED},    {76, THERMAL_PAUSED},    {75, THERMAL_PAUSED},
      {74, THERMAL_THROTTLED}, {80, THERMAL_PAUSED},    {40, THERMAL_THROTTLED},
      {40, THERMAL_NORMAL},    {85, THERMAL_PAUSED},    {74.9f, THERMAL_THROTTLED},
  };
  Tracker t = camera_thermal::initial();
  FakeSensor die;
  ThermalState was = THERMAL_NORMAL;
  for (const Step_& w : walk) {
    die.temp_c = w.temp_c;
    const Step s = camera_thermal::check(t, die);
    if (t.state != w.want)
      std::printf("  at %.1f C: state %d, want %d\n", (double)w.temp_c, (int)t.state, (int)w.want);
    CHECK(t.state == w.want);
    CHECK(s.prev == was && s.changed == (was != w.want));
    CHECK(!s.fail_safe && !s.recovered);
    was = t.state;
  }
  CHECK(THERMAL_THROTTLE_TEMP_C == 70 && THERMAL_PAUSE_TEMP_C == 80 && THERMAL_RECOVER_MARGIN_C == 5);
}

static void test_the_frame_delay_follows_the_state() {
  CHECK(camera_thermal::frame_delay_ms(THERMAL_NORMAL, 40) == 40);
  CHECK(camera_thermal::frame_delay_ms(THERMAL_NORMAL, 333) == 333);
  CHECK(camera_thermal::frame_delay_ms(THERMAL_PAUSED, 40) == 500);
  CHECK(camera_thermal::frame_delay_ms(THERMAL_THROTTLED, 20) == 120);
  CHECK(camera_thermal::frame_delay_ms(THERMAL_THROTTLED, 40) == 120);
  CHECK(camera_thermal::frame_delay_ms(THERMAL_THROTTLED, 100) == 300);
  CHECK(camera_thermal::frame_delay_ms(THERMAL_THROTTLED, 200) == 500);
  CHECK(camera_thermal::frame_delay_ms(THERMAL_THROTTLED, 500) == 500);
}

// ════════════════════════════════════════════════════════════════════════════
// The sources around the step.
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

static void test_the_sources(const std::vector<std::string>& files) {
  std::string cam_cpp, cam_h, step_h, provider;
  for (const std::string& f : files) {
    if (ends_with(f, "securacv_camera/src/securacv_camera.cpp")) cam_cpp = read_file(f);
    if (ends_with(f, "securacv_camera/src/securacv_camera.h")) cam_h = read_file(f);
    if (ends_with(f, "securacv_camera/src/camera_thermal.h")) step_h = read_file(f);
    if (ends_with(f, "securacv_thermal/src/securacv_thermal.cpp")) provider = read_file(f);
  }
  CHECK(!cam_cpp.empty() && !cam_h.empty() && !step_h.empty() && !provider.empty());

  // checkThermal() reads only through the step, with the provider's adapter,
  // and paces and reports from what the step answered.
  const std::string check = body_of(cam_cpp, "\nvoid CameraManager::checkThermal() {\n");
  CHECK(count_of(check, "camera_thermal::check(m_thermal, camera_thermal::DieSensor{})") == 1);
  CHECK(check.find("thermal_read_die_c(") == std::string::npos);
  CHECK(check.find("m_frame_delay_ms = camera_thermal::frame_delay_ms(m_thermal.state, "
                   "m_user_frame_delay_ms);") != std::string::npos);
  CHECK(check.find("if (step.fail_safe) {") != std::string::npos);
  CHECK(check.find("if (step.recovered) {") != std::string::npos);
  CHECK(check.find("if (step.changed) {") != std::string::npos);
  CHECK(cam_cpp.find("thermal_read_die_c(") == std::string::npos);   // no second read site
  CHECK(cam_cpp.find("m_thermal.fail_count") == std::string::npos);  // the count is the step's

  // The adapter is the provider's two calls.
  CHECK(step_h.find("  bool present() const { return thermal_sensor_present(); }\n") != std::string::npos);
  CHECK(step_h.find("  bool read(float* c) const { return thermal_read_die_c(c); }\n") != std::string::npos);

  // Each stream starts from the step's reset.
  const std::string reset = body_of(cam_cpp, "\nvoid CameraManager::resetMetrics() {\n");
  CHECK(count_of(reset, "camera_thermal::reset(m_thermal);") == 1);

  // The status reads the provider, not the count alone.
  CHECK(cam_h.find("return camera_thermal::sensor_ok(m_thermal, thermal_sensor_present());") !=
        std::string::npos);
  CHECK(cam_h.find("#include \"camera_thermal.h\"") != std::string::npos);

  // The provider asks sdkconfig.h which chip this is before it tests the
  // target. IDF defines CONFIG_IDF_TARGET_* in that header and nowhere on
  // the compiler's command line, and nothing else the provider includes
  // reaches it; without the include it saw no target on any chip, and the
  // S3 compiled the sensorless stub (the builds of this suite would pass on
  // it too: their sdkconfig.h is on the include path, as the core's is, and
  // only an include reads it).
  const size_t sdkconfig_at = provider.find("#include <sdkconfig.h>\n");
  const size_t target_at = provider.find("#if defined(CONFIG_IDF_TARGET_ESP32S3)");
  CHECK(sdkconfig_at != std::string::npos && target_at != std::string::npos);
  CHECK(sdkconfig_at < target_at);
  CHECK(count_of(provider, "#include <sdkconfig.h>\n") == 1);
  CHECK(provider.find("#include") < sdkconfig_at &&
        provider.find("#include \"securacv_thermal.h\"\n") < sdkconfig_at);   // its own header, then this

  // The provider names a sensor on the S3, S2 and C3 alone, and present()
  // is that answer.
  CHECK(provider.find("#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2) || \\\n"
                      "    defined(CONFIG_IDF_TARGET_ESP32C3)\n"
                      "  #define SECURACV_HAVE_TSENS 1\n") != std::string::npos);
  CHECK(count_of(provider, "#define SECURACV_HAVE_TSENS 1") == 1);
  CHECK(provider.find("extern \"C\" bool thermal_sensor_present(void) {\n"
                      "  return SECURACV_HAVE_TSENS != 0;\n}\n") != std::string::npos);
}

int main(int argc, char** argv) {
  std::vector<std::string> files;
  std::string mode = "ok";
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a.compare(0, 8, "--tsens=") == 0) mode = a.substr(8);
    else files.push_back(a);
  }

#if defined(CONFIG_IDF_TARGET_ESP32S3)
  if (mode == "ok") g_mode = DriverMode::Ok;
  else if (mode == "install-fails") g_mode = DriverMode::InstallFails;
  else if (mode == "enable-fails") g_mode = DriverMode::EnableFails;
  else { std::printf("test_camera_thermal: unknown --tsens=%s\n", mode.c_str()); return 2; }
  host_tsens().install_fails = g_mode == DriverMode::InstallFails;
  host_tsens().enable_fails = g_mode == DriverMode::EnableFails;
  char build[96];
  std::snprintf(build, sizeof(build), "%s, IDF %d.%d.%d, driver %s", CONFIG_IDF_TARGET,
                ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH, mode.c_str());

  test_the_s3_provider_names_its_sensor();
  if (g_mode == DriverMode::Ok) {
    test_the_s3_provider_reads_through_the_driver();
    test_the_s3_camera_step_reads_the_die();
  } else {
    test_a_driver_that_will_not_start_fails_safe_every_stream();
  }
#else
  if (mode != "ok") { std::printf("test_camera_thermal: --tsens= is for the S3 builds\n"); return 2; }
  const char* build = CONFIG_IDF_TARGET ", no sensor";
  test_a_chip_with_no_sensor_has_none();
  test_a_sensorless_chip_never_throttles();
#endif
  test_a_provider_with_no_sensor_is_never_read();
  test_three_misses_throttle_once();
  test_a_reset_clears_the_count();
  test_the_hysteresis_holds();
  test_the_frame_delay_follows_the_state();
  CHECK(files.size() == 4);
  test_the_sources(files);

  if (g_failures) {
    std::printf("test_camera_thermal (%s): %d of %d checks FAILED\n", build, g_failures, g_checks);
    return 1;
  }
  std::printf("test_camera_thermal (%s): OK, %d checks\n", build, g_checks);
  return 0;
}
