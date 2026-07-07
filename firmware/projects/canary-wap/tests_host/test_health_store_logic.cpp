/* Host tests for health_store_logic.h — the /HEALTH per-boot jsonl line
 * format (with JSON escaping of peer-influenced strings) and the boot
 * filename scheme. Build & run (CI: firmware.yml host tests):
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror \
 *       -I firmware/projects/canary-wap/arduino/canary_wap \
 *       firmware/projects/canary-wap/tests_host/test_health_store_logic.cpp \
 *       -o /tmp/test_health_store_logic && /tmp/test_health_store_logic
 */

#include <cstdio>
#include <cstring>

#include "health_store_logic.h"

using namespace health_store;

static int g_failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      g_failures++;                                                    \
    }                                                                  \
  } while (0)

static void test_json_escape() {
  char out[64];

  CHECK(json_escape(out, sizeof(out), "plain ascii") == 11);
  CHECK(strcmp(out, "plain ascii") == 0);

  /* Quote and backslash — the injection-relevant pair (mesh peer names
   * travel into health details). */
  json_escape(out, sizeof(out), "a\"b\\c");
  CHECK(strcmp(out, "a\\\"b\\\\c") == 0);

  /* Control bytes become \u00XX. */
  json_escape(out, sizeof(out), "x\ny\tz");
  CHECK(strcmp(out, "x\\u000ay\\u0009z") == 0);

  /* NULL src is an empty string, not a crash. */
  CHECK(json_escape(out, sizeof(out), nullptr) == 0);
  CHECK(out[0] == '\0');

  /* Truncation is atomic: a two-byte escape never gets half-written. */
  char tiny[4];
  json_escape(tiny, sizeof(tiny), "ab\"cd");
  CHECK(strcmp(tiny, "ab") == 0); /* the \" needs 2 chars; only 1 left */

  /* High-bit bytes (UTF-8 payloads) pass through untouched. */
  json_escape(out, sizeof(out), "caf\xc3\xa9");
  CHECK(strcmp(out, "caf\xc3\xa9") == 0);
}

static void test_line_build() {
  char line[HS_LINE_MAX];
  const size_t n = line_build(line, sizeof(line), 42, 123456, "WARNING",
                              "STORAGE", "SD card removed", "slot 1");
  CHECK(n > 0);
  CHECK(line[n - 1] == '\n');
  CHECK(strcmp(line,
               "{\"v\":1,\"seq\":42,\"ms\":123456,\"lvl\":\"WARNING\","
               "\"cat\":\"STORAGE\",\"msg\":\"SD card removed\","
               "\"detail\":\"slot 1\"}\n") == 0);

  /* A hostile detail (quotes + newline from a peer name) stays inside
   * its JSON string. */
  const size_t m = line_build(line, sizeof(line), 1, 2, "INFO", "MESH",
                              "Opera alert received",
                              "From \"evil\nname\": tamper");
  CHECK(m > 0);
  CHECK(strstr(line, "From \\\"evil\\u000aname\\\": tamper") != NULL);
  /* Exactly one newline — the terminator; the embedded one was escaped. */
  CHECK(strchr(line, '\n') == line + m - 1);

  /* NULL message/detail degrade to empty fields, not crashes. */
  CHECK(line_build(line, sizeof(line), 1, 2, "INFO", "SYSTEM", nullptr,
                   nullptr) > 0);
  CHECK(strstr(line, "\"msg\":\"\"") != NULL);

  /* A buffer too small for the framing refuses. */
  char tiny[32];
  CHECK(line_build(tiny, sizeof(tiny), 1, 2, "INFO", "SYSTEM", "x", "y") ==
        0);
}

static void test_boot_filename() {
  char path[HS_PATH_MAX];
  boot_filename(42, path, sizeof(path));
  CHECK(strcmp(path, "/HEALTH/boot_42.jsonl") == 0);
  boot_filename(4294967295u, path, sizeof(path));
  CHECK(strcmp(path, "/HEALTH/boot_4294967295.jsonl") == 0);
}

int main() {
  test_json_escape();
  test_line_build();
  test_boot_filename();

  if (g_failures != 0) {
    std::printf("%d CHECK(S) FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL health_store TESTS PASSED\n");
  return 0;
}
