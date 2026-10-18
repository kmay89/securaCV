// canary-local/emulator/test/request_rotation_test.cpp — the binding that
// turns a running glass (sweep F243), natively. onboard.test.js builds it
// with g++ against the emulator's REAL src/emu_main.cpp (its main() renamed,
// so this driver owns the process; never called) and, in the nightlight
// config, the firmware's REAL src/care/nightlight.cpp — the rotation mailbox
// the app's orientation picker writes (glass_web.cpp) and main.cpp's loop
// drains (nightlight_take_rotation_request) into its one apply path. The
// emulator's Preferences are an in-memory stand-in here that counts writes.
//
// It holds emu_request_rotation():
//   * before power-on it refuses (0) and writes nothing: a unit that is off
//     has no loop to drain the mailbox, and emu_preset_rotation stages a
//     saved rotation for the boot instead;
//   * after power-on each of 0..3 is queued (1) into the very mailbox the
//     loop drains, once, and a later ask before the drain replaces it, as the
//     picker's does;
//   * anything outside 0..3 is refused (0) and leaves the mailbox as it was;
//   * it writes no flash: the turn itself is the apply path's to save;
//   * on a flavor with no mailbox (the dash config) it answers -1, before
//     power-on and after.
// runtime_turn.sh then holds what the loop does with the mailbox (F222): a
// glass turned at runtime seats its companion where a boot there does. This
// proves the sources build.sh hands em++, built with g++, not the dist's
// bytes; boot_probe.mjs turns the dist's nightlight once the dist carries the
// binding.
//
// Prints "ALL REQUEST ROTATION TESTS PASSED" on success.
#include <Arduino.h>
#include <Preferences.h>
#include <config.h>
#include <stdio.h>
#include <string.h>

#ifdef CD_NIGHTLIGHT
#include "canary/care/nightlight_glue.h"
#else
#include "canary/glass_settings.h"
#endif

extern "C" {
void emu_power_on(void);
int emu_preset_rotation(int);
int emu_request_rotation(int);
}

namespace {
int g_fail = 0;
int g_checks = 0;
int g_flash_writes = 0;
}  // namespace

#define CHECK(cond, ...)                                    \
  do {                                                      \
    g_checks++;                                             \
    if (!(cond)) {                                          \
      g_fail++;                                             \
      printf("  FAIL (%s:%d): ", __FILE__, __LINE__);       \
      printf(__VA_ARGS__);                                  \
      printf("\n");                                         \
    }                                                       \
  } while (0)

// ── What emu_main.cpp links against, standing in for the rest ───────────
void setup() {}
void loop() {}
extern "C" void emscripten_sleep(unsigned int) {}
extern "C" void emu_native_em_js(const char*) {}
extern "C" uint32_t millis(void) { return 0; }

// The emulator's Preferences (emu_support.cpp), in memory: empty flash, and
// every write counted.
bool Preferences::begin(const char* ns, bool) {
  strncpy(ns_, ns, sizeof(ns_) - 1);
  ns_[sizeof(ns_) - 1] = 0;
  return true;
}
void Preferences::end() {}
uint8_t Preferences::getUChar(const char*, uint8_t def) { return def; }
size_t Preferences::putUChar(const char*, uint8_t) {
  g_flash_writes++;
  return 1;
}

#ifndef CD_NIGHTLIGHT
// emu_main.cpp's save_rotation() reaches the glass settings on this flavor;
// nothing here calls it (no rotation is staged), so these only link.
namespace canary::glass {
namespace {
Settings s_settings;
}
void settings_init() {}
Settings& settings_mut() { return s_settings; }
void settings_mark_dirty() {}
void settings_loop(uint32_t) {}
}  // namespace canary::glass
#endif

int main() {
#ifdef CD_NIGHTLIGHT
  using canary::care::nightlight_take_rotation_request;
  printf("nightlight config: emu_request_rotation writes the rotation mailbox\n");
  CHECK(nightlight_take_rotation_request() == -1, "the mailbox starts empty");

  // Off: refused, nothing written.
  for (int r = 0; r < 4; ++r) {
    const int ans = emu_request_rotation(r);
    CHECK(ans == 0, "emu_request_rotation(%d) before power-on answered %d, not 0", r, ans);
  }
  CHECK(nightlight_take_rotation_request() == -1, "a refused ask before power-on reached the mailbox");
  CHECK(emu_preset_rotation(1) == 1, "emu_preset_rotation(1) before power-on is still the way to stage a turn");

  emu_power_on();
  CHECK(emu_preset_rotation(1) == 0, "emu_preset_rotation refuses a running unit (the boot it staged for has begun)");

  // On: every quarter turn queued into the mailbox the loop drains, once.
  for (int r = 0; r < 4; ++r) {
    const int ans = emu_request_rotation(r);
    CHECK(ans == 1, "emu_request_rotation(%d) after power-on answered %d, not 1", r, ans);
    const int got = nightlight_take_rotation_request();
    CHECK(got == r, "asked for rotation %d, the loop drains %d", r, got);
    CHECK(nightlight_take_rotation_request() == -1, "rotation %d drained twice", r);
  }

  // A second ask before the loop drains replaces the first, as the picker's.
  CHECK(emu_request_rotation(1) == 1 && emu_request_rotation(3) == 1, "two asks queued");
  CHECK(nightlight_take_rotation_request() == 3, "the later ask is the one the loop drains");

  // Out of range: refused, and the mailbox keeps what it held.
  CHECK(emu_request_rotation(2) == 1, "rotation 2 queued");
  for (int bad : {-1, 4, 5, 255, 256, -256}) {
    const int ans = emu_request_rotation(bad);
    CHECK(ans == 0, "emu_request_rotation(%d) answered %d, not 0", bad, ans);
  }
  CHECK(nightlight_take_rotation_request() == 2, "a refused ask disturbed the queued rotation");
  CHECK(nightlight_take_rotation_request() == -1, "the mailbox empties on the drain");

  CHECK(g_flash_writes == 0, "emu_request_rotation wrote flash (%d writes): saving the turn is the apply path's",
        g_flash_writes);
#else
  printf("no-mailbox config: emu_request_rotation answers -1\n");
  for (int r : {0, 1, 2, 3, -1, 4}) {
    const int ans = emu_request_rotation(r);
    CHECK(ans == -1, "emu_request_rotation(%d) before power-on answered %d, not -1", r, ans);
  }
  emu_power_on();
  for (int r : {0, 1, 2, 3, -1, 4}) {
    const int ans = emu_request_rotation(r);
    CHECK(ans == -1, "emu_request_rotation(%d) answered %d, not -1", r, ans);
  }
  CHECK(g_flash_writes == 0, "emu_request_rotation wrote flash (%d writes)", g_flash_writes);
#endif

  printf("%d checks, %d failed\n", g_checks, g_fail);
  if (g_fail) {
    printf("REQUEST ROTATION TEST FAILED\n");
    return 1;
  }
  printf("ALL REQUEST ROTATION TESTS PASSED\n");
  return 0;
}
