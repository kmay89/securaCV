// Canary Vision's module link (src/vision/vision_mgr.cpp, compiled verbatim
// against a scripted Grove Vision AI module, stubs/vision_mgr/): what the
// boot line, the `j` proof record and the sampling loop say about the module.
//
// Pinned here (sweep F384):
//   * module_id() is 1 when the module answered (the library's begin() and
//     then a fresh NAME? round trip) and 0 when it did not. It was the
//     library's cached id accessor cast to int, the address of its id buffer:
//     non-zero with no module at all, so the desktop Flasher's boot-line proof
//     passed a dead module. (On a 64-bit host that cast does not even compile.)
//   * the boot line keeps its exact format, `Grove Vision AI ID=%d`, with
//     only 1 or 0 in it, and the ERROR line above it exactly when the value
//     is 0 (gen_vision.py stages both; the desktop's monitor parses both);
//   * the firmware never calls the id accessor (SSCMA::ID), cached or not:
//     the module's own id is treated as a per-unit identifier;
//   * the round trip is fresh (name(false)), and a begin() that failed skips
//     it, so a dead module does not also cost NAME?'s 3 s timeout;
//   * sample() reads invoke()'s int as the status code it is (CMD_OK == 0).
//     Stored in a bool it read backwards: a successful invoke that found no
//     box returned no sample, so the presence FSM never saw an empty frame and
//     could not time a person out, and a failed invoke returned a sample
//     built from the previous frame's boxes, a person who was no longer seen.
//
// Host-tested only: the stub answers the way Seeed_Arduino_SSCMA 1.0.3's
// source says the library does; nothing here has run against a module.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Arduino.h"
#include "Seeed_Arduino_SSCMA.h"
#include "Wire.h"
#include "pins.h"

#include "canary/config.h"
#include "canary/detect_config.h"
#include "canary/vision/vision_mgr.h"

// vision_mgr.cpp reads the runtime detection settings; the NVS-backed loader
// (detect_config.cpp) is not linked, so the seeds config.h compiles in are
// the settings, as on a factory-fresh unit.
namespace canary::cfg {
const DetectConfig& detect() {
  static const DetectConfig d{(uint8_t)PERSON_TARGET, (uint8_t)SCORE_MIN,
                              LOST_TIMEOUT_MS, DWELL_START_MS, 0};
  return d;
}
}  // namespace canary::cfg

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg)                                                       \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      ++g_fails;                                                               \
      std::printf("  FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);          \
    }                                                                          \
  } while (0)

static const char kErrorLine[] = "ERROR: Grove Vision AI V2 not responding";

// A fresh boot of the module link: a new script, an empty console, init().
static void boot(bool begin_ok, bool name_answers) {
  sscma_script() = SscmaScript{};
  sscma_script().begin_ok = begin_ok;
  sscma_script().name_answers = name_answers;
  Serial.out.clear();
  stub_delay_total_ms() = 0;
  canary::vision::init();
}

static bool printed(const char* s) { return Serial.out.find(s) != std::string::npos; }

static boxes_t person_box(uint8_t score) {
  boxes_t b{};
  b.x = 120;
  b.y = 120;
  b.w = 60;
  b.h = 120;
  b.score = score;
  b.target = (uint8_t)PERSON_TARGET;
  return b;
}

static void test_a_module_that_answers() {
  std::printf("-- a module that answers: ID=1, ready, no id query from the firmware\n");
  boot(/*begin_ok=*/true, /*name_answers=*/true);
  CHECK(canary::vision::module_id() == 1, "module_id is 1, never the id buffer's address");
  CHECK(canary::vision::module_ok(), "module_ok");
  CHECK(canary::vision::ready(), "ready to sample");
  CHECK(printed("[I2C]\nGrove Vision AI ID=1\n"), "the boot line, its exact format, value 1");
  CHECK(!printed(kErrorLine), "no ERROR line for a module that answered");
  CHECK(sscma_script().begin_calls == 1, "begin() once");
  CHECK(sscma_script().name_calls == 1, "one NAME? round trip after begin()");
  CHECK(sscma_script().name_cached_calls == 0, "the round trip is fresh: name(false), not the cache");
  CHECK(sscma_script().id_calls == 0, "the firmware never calls the id accessor");
  CHECK(Wire.sda == I2C_PIN_SDA && Wire.scl == I2C_PIN_SCL, "the bus opens on the board's own pins");
}

static void test_a_dead_module() {
  std::printf("-- a dead module: ID=0, the ERROR line, no NAME? timeout\n");
  boot(/*begin_ok=*/false, /*name_answers=*/false);
  CHECK(canary::vision::module_id() == 0, "module_id is 0 when begin() failed");
  CHECK(!canary::vision::module_ok(), "not module_ok");
  CHECK(!canary::vision::ready(), "not ready");
  CHECK(printed("[I2C]\nERROR: Grove Vision AI V2 not responding"), "the ERROR line, under [I2C]");
  CHECK(printed(" not responding — check wiring/model (see docs/hardware/grove_vision_ai_v2_guide.md)\n"
                "Grove Vision AI ID=0\n"),
        "then the boot line, value 0, right below it");
  CHECK(sscma_script().name_calls == 0, "a failed begin() skips the NAME? round trip (no 3 s timeout)");
  CHECK(sscma_script().id_calls == 0, "the firmware never calls the id accessor");

  VisionSample vs{};
  CHECK(!canary::vision::sample(vs), "a dead module yields no sample");
  CHECK(sscma_script().invoke_calls == 0, "and is never asked to invoke");
}

static void test_begin_ok_but_the_round_trip_fails() {
  std::printf("-- begin() answered but the fresh NAME? did not: ID=0\n");
  boot(/*begin_ok=*/true, /*name_answers=*/false);
  CHECK(canary::vision::module_id() == 0, "module_id is 0 when the fresh round trip failed");
  CHECK(!canary::vision::module_ok(), "not module_ok");
  CHECK(!canary::vision::ready(), "not ready");
  CHECK(printed(kErrorLine), "the ERROR line");
  CHECK(printed("Grove Vision AI ID=0\n"), "the boot line, value 0");
  CHECK(sscma_script().name_calls == 1 && sscma_script().name_cached_calls == 0,
        "the round trip was asked, freshly");
  CHECK(sscma_script().id_calls == 0, "the firmware never calls the id accessor");
}

static void test_only_ever_one_or_zero() {
  std::printf("-- module_id is only ever 1 or 0, and agrees with module_ok\n");
  const bool cases[][2] = {{true, true}, {true, false}, {false, true}, {false, false}};
  for (const auto& c : cases) {
    boot(c[0], c[1]);
    const int id = canary::vision::module_id();
    CHECK(id == 0 || id == 1, "module_id is 1 or 0");
    CHECK((id == 1) == canary::vision::module_ok(), "module_id and module_ok are one verdict");
    CHECK((id == 1) == (c[0] && c[1]), "1 only when begin() and the round trip both answered");
    char line[64];
    std::snprintf(line, sizeof(line), "Grove Vision AI ID=%d\n", id);
    CHECK(printed(line), "the boot line prints the same value");
    CHECK(printed(kErrorLine) == (id == 0), "the ERROR line exactly when the value is 0");
  }
}

static void test_invoke_status_is_read_as_a_status() {
  std::printf("-- sample(): invoke()'s int is a status code, CMD_OK == 0\n");
  boot(true, true);
  VisionSample vs{};

  // A successful invoke with a person in frame: a sample with the person.
  sscma_script().invoke_rc = CMD_OK;
  sscma_script().frame = {person_box(90)};
  CHECK(canary::vision::sample(vs), "a successful invoke with a person yields a sample");
  CHECK(vs.person_now && vs.person_count == 1, "the person is in it");

  // A successful invoke that found no box: a sample, empty. The presence FSM
  // needs this frame to time the person out (before the fix: no sample).
  sscma_script().frame.clear();
  vs = VisionSample{};
  vs.person_now = true;
  CHECK(canary::vision::sample(vs), "a successful invoke that found no box still yields a sample");
  CHECK(!vs.person_now && vs.person_count == 0, "an empty one");

  // A person again, then a failed invoke: the library's boxes still hold the
  // person, and no sample may be built from them (before the fix: a sample
  // with the person who was no longer seen).
  sscma_script().frame = {person_box(90)};
  CHECK(canary::vision::sample(vs) && vs.person_now, "the person is back");
  sscma_script().invoke_rc = CMD_ETIMEDOUT;
  vs = VisionSample{};
  CHECK(!canary::vision::sample(vs), "a timed-out invoke yields no sample, whatever the boxes hold");
  CHECK(!vs.person_now, "and the stale person is not reported");

  sscma_script().invoke_rc = CMD_EIO;
  sscma_script().frame.clear();
  CHECK(!canary::vision::sample(vs), "a failed invoke with no boxes yields no sample either");

  CHECK(sscma_script().id_calls == 0, "sampling never calls the id accessor");
}

int main() {
  std::printf("== canary-vision module link (vision_mgr.cpp) ==\n");
  test_a_module_that_answers();
  test_a_dead_module();
  test_begin_ok_but_the_round_trip_fails();
  test_only_ever_one_or_zero();
  test_invoke_status_is_read_as_a_status();
  std::printf("%d checks, %d failures\n", g_checks, g_fails);
  if (g_fails) {
    std::printf("VISION MODULE LINK FAILED\n");
    return EXIT_FAILURE;
  }
  std::printf("vision module link ok\n");
  return EXIT_SUCCESS;
}
