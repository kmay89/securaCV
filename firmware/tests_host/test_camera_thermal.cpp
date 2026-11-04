/* Host tests for the camera's thermal step (repo sweep F432): the pure
 * header CameraManager::checkThermal() calls
 * (firmware/canary/lib/securacv_camera/src/camera_thermal.h), linked with
 * the host build of the shared die-temperature provider
 * (firmware/canary/lib/securacv_thermal/src/securacv_thermal.cpp, compiled
 * as is: no CONFIG_IDF_TARGET_ESP32S3/S2/C3, so SECURACV_HAVE_TSENS is 0).
 * Built twice: as the host, and with -DCONFIG_IDF_TARGET_ESP32=1, the
 * target IDF defines for the classic ESP32 of env:esp32cam and
 * env:esp32-wroom, which must read as no sensor too.
 *
 * THE FAILURE THIS PREVENTS. On the classic ESP32 thermal_read_die_c()
 * always answers false, and checkThermal() counted each false as a sensor
 * miss: the third throttled the first PEEK stream after boot and logged
 * "Camera thermal sensor read failing" for a sensor the chip does not
 * have, while getThermalSensorOk() said true until then. resetMetrics()
 * reset the state and not the count, so the fail-safe fired on the first
 * stream only, and on a chip whose sensor really fails, a later stream ran
 * unthrottled and blind.
 *
 * Now the provider says whether there is a sensor (thermal_sensor_present()),
 * the step reads nothing and counts nothing when there is none, a reset
 * clears the count, and sensor_ok() is false on a sensorless chip.
 *
 * It also reads the sources (argv): checkThermal() reads only through the
 * step with the provider's own adapter, resetMetrics() resets the step,
 * getThermalSensorOk() asks the provider, and the provider names a sensor
 * only on the S3, S2 and C3.
 *
 * What this does not prove: the ESP-IDF temperature-sensor driver, the
 * camera, or anything on a device. The esp32cam and canary envs compile the
 * real thing in CI; it has not run on a bench.
 *
 * Build & run: make -C firmware/tests_host
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "camera_thermal.h"
#include "securacv_thermal.h"

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
// The provider's host build, and a chip with no sensor.
// ════════════════════════════════════════════════════════════════════════════

static void test_the_host_provider_has_no_sensor() {
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

  // The same with a counting provider: not one read is asked for, and a
  // stream's reset changes nothing.
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
  test_the_host_provider_has_no_sensor();
  test_a_sensorless_chip_never_throttles();
  test_three_misses_throttle_once();
  test_a_reset_clears_the_count();
  test_the_hysteresis_holds();
  test_the_frame_delay_follows_the_state();
  std::vector<std::string> files(argv + 1, argv + argc);
  CHECK(files.size() == 4);
  test_the_sources(files);

#if defined(CONFIG_IDF_TARGET_ESP32)
  const char* build = "classic ESP32 target";
#else
  const char* build = "host";
#endif
  if (g_failures) {
    std::printf("test_camera_thermal (%s): %d of %d checks FAILED\n", build, g_failures, g_checks);
    return 1;
  }
  std::printf("test_camera_thermal (%s): OK, %d checks\n", build, g_checks);
  return 0;
}
