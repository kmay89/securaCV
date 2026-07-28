// Host tests for the serial tuning console (common/console/tuning_console.h)
// — the line-based knob console the canary-sense flasher bench speaks over
// USB serial. No Arduino, no framework: plain assert(), like the rest of
// this suite.
//
// What is pinned here:
//   * the command vocabulary: help / ? / cfg / get / set / reset / stream / raw
//   * clamping through the knob table (and the "clamped from" reply)
//   * strict number parsing (no "12x", no "-3", no overflow wrap)
//   * the one-line `[cfg] name=value ... stream=N raw=N` snapshot the
//     flasher parses to populate its sliders
//   * take_changed() semantics (fires once per applied change, not per line)
//   * CRLF tolerance, blank lines, unknown commands/knobs, line overflow

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "console/tuning_console.h"

using securacv::console::Knob;
using securacv::console::TuningConsole;

// ---- captured output --------------------------------------------------------

static char g_out[8192];
static size_t g_out_len = 0;

static void out_reset() { g_out_len = 0; g_out[0] = '\0'; }

static void out_write(const char* s) {
  const size_t n = strlen(s);
  if (g_out_len + n + 1 >= sizeof(g_out)) return;
  memcpy(g_out + g_out_len, s, n + 1);
  g_out_len += n;
}

static bool out_contains(const char* needle) {
  return strstr(g_out, needle) != nullptr;
}

// ---- fake knobs -------------------------------------------------------------

static uint32_t g_debounce = 300;
static uint32_t g_near = 150;

static uint32_t fk_get_debounce() { return g_debounce; }
static bool fk_set_debounce(uint32_t v) {
  if (v > 3000u) v = 3000u;
  if (g_debounce == v) return false;
  g_debounce = v;
  return true;
}

static uint32_t fk_get_near() { return g_near; }
static bool fk_set_near(uint32_t v) {
  if (v < 50u) v = 50u;
  if (v > 400u) v = 400u;
  if (g_near == v) return false;
  g_near = v;
  return true;
}

static const Knob KNOBS[] = {
    {"debounce", "ms", "sustained target before 'present'",
     0, 3000, 300, fk_get_debounce, fk_set_debounce},
    {"near", "cm", "near band edge",
     50, 400, 150, fk_get_near, fk_set_near},
};

static TuningConsole make_console() {
  g_debounce = 300;
  g_near = 150;
  out_reset();
  TuningConsole c;
  c.begin(KNOBS, sizeof(KNOBS) / sizeof(KNOBS[0]), out_write);
  return c;
}

// ---- tests ------------------------------------------------------------------

static void test_defaults() {
  TuningConsole c = make_console();
  assert(c.stream_period_ms() == TuningConsole::STREAM_DEFAULT_MS);
  assert(!c.raw_enabled());
  assert(!c.take_changed());
  printf("PASS defaults\n");
}

static void test_cfg_snapshot_line() {
  TuningConsole c = make_console();
  c.feed("cfg\n");
  assert(out_contains("[cfg] debounce=300 near=150 stream=1000 raw=0"));
  printf("PASS cfg_snapshot_line\n");
}

static void test_help_lists_knobs() {
  TuningConsole c = make_console();
  c.feed("help\n");
  assert(out_contains("[tune] commands:"));
  assert(out_contains("debounce"));
  assert(out_contains("0..3000 ms"));
  assert(out_contains("near"));
  assert(out_contains("50..400 cm"));
  out_reset();
  c.feed("?\n");
  assert(out_contains("[tune] commands:"));
  printf("PASS help_lists_knobs\n");
}

static void test_set_applies_and_flags() {
  TuningConsole c = make_console();
  c.feed("set debounce 500\n");
  assert(g_debounce == 500);
  assert(out_contains("[tune] ok debounce=500"));
  // The refreshed snapshot rides every set reply.
  assert(out_contains("[cfg] debounce=500 near=150"));
  assert(c.take_changed());
  assert(!c.take_changed());  // drained
  printf("PASS set_applies_and_flags\n");
}

static void test_set_clamps_and_says_so() {
  TuningConsole c = make_console();
  c.feed("set near 9999\n");
  assert(g_near == 400);
  assert(out_contains("clamped from 9999"));
  assert(out_contains("range 50..400"));
  assert(c.take_changed());
  printf("PASS set_clamps_and_says_so\n");
}

static void test_set_same_value_no_change_flag() {
  TuningConsole c = make_console();
  c.feed("set debounce 300\n");
  assert(out_contains("(unchanged)"));
  assert(!c.take_changed());
  printf("PASS set_same_value_no_change_flag\n");
}

static void test_set_rejects_junk_numbers() {
  TuningConsole c = make_console();
  c.feed("set debounce 12x\n");
  assert(g_debounce == 300);
  assert(out_contains("not a whole number"));
  out_reset();
  c.feed("set debounce -3\n");
  assert(g_debounce == 300);
  assert(out_contains("not a whole number"));
  out_reset();
  c.feed("set debounce 99999999999999999999\n");  // would wrap uint32
  assert(g_debounce == 300);
  assert(out_contains("not a whole number"));
  assert(!c.take_changed());
  printf("PASS set_rejects_junk_numbers\n");
}

static void test_set_unknown_knob() {
  TuningConsole c = make_console();
  c.feed("set flux 42\n");
  assert(out_contains("unknown knob 'flux'"));
  out_reset();
  c.feed("set\n");
  assert(out_contains("usage: set <knob> <value>"));
  printf("PASS set_unknown_knob\n");
}

static void test_reset_restores_defaults() {
  TuningConsole c = make_console();
  c.feed("set debounce 500\nset near 200\n");
  assert(c.take_changed());
  out_reset();
  c.feed("reset\n");
  assert(g_debounce == 300 && g_near == 150);
  assert(out_contains("[tune] ok defaults restored"));
  assert(c.take_changed());
  out_reset();
  c.feed("reset\n");  // already at defaults
  assert(out_contains("already there"));
  assert(!c.take_changed());
  printf("PASS reset_restores_defaults\n");
}

static void test_stream_control() {
  TuningConsole c = make_console();
  c.feed("stream off\n");
  assert(c.stream_period_ms() == 0);
  c.feed("stream on\n");
  assert(c.stream_period_ms() == TuningConsole::STREAM_DEFAULT_MS);
  c.feed("stream 500\n");
  assert(c.stream_period_ms() == 500);
  c.feed("stream 1\n");  // below floor
  assert(c.stream_period_ms() == TuningConsole::STREAM_MIN_MS);
  c.feed("stream 999999\n");  // above ceiling
  assert(c.stream_period_ms() == TuningConsole::STREAM_MAX_MS);
  out_reset();
  c.feed("stream banana\n");
  assert(out_contains("usage: stream on|off|<ms>"));
  // stream changes are session state, not knob changes
  assert(!c.take_changed());
  printf("PASS stream_control\n");
}

static void test_raw_control() {
  TuningConsole c = make_console();
  c.feed("raw on\n");
  assert(c.raw_enabled());
  c.feed("raw off\n");
  assert(!c.raw_enabled());
  out_reset();
  c.feed("raw\n");
  assert(out_contains("usage: raw on|off"));
  assert(!c.take_changed());
  printf("PASS raw_control\n");
}

static void test_case_insensitive_and_crlf() {
  TuningConsole c = make_console();
  c.feed("SET Debounce 700\r\n");
  assert(g_debounce == 700);
  out_reset();
  c.feed("CFG\r\n");
  assert(out_contains("[cfg] debounce=700"));
  printf("PASS case_insensitive_and_crlf\n");
}

static void test_blank_and_whitespace_lines_ignored() {
  TuningConsole c = make_console();
  c.feed("\n\r\n   \n\t\n");
  assert(g_out_len == 0);
  printf("PASS blank_and_whitespace_lines_ignored\n");
}

static void test_unknown_command() {
  TuningConsole c = make_console();
  c.feed("frobnicate\n");
  assert(out_contains("unknown command 'frobnicate'"));
  assert(out_contains("try 'help'"));
  printf("PASS unknown_command\n");
}

static void test_overflow_line_discarded() {
  TuningConsole c = make_console();
  // A paste far beyond MAX_LINE must not execute anything and must not
  // corrupt the next command.
  for (int i = 0; i < 500; i++) c.feed('a');
  c.feed('\n');
  assert(out_contains("line too long"));
  assert(g_debounce == 300);
  out_reset();
  c.feed("set debounce 400\n");
  assert(g_debounce == 400);
  printf("PASS overflow_line_discarded\n");
}

static void test_extra_whitespace_tokenizing() {
  TuningConsole c = make_console();
  c.feed("  set   debounce\t900  \n");
  assert(g_debounce == 900);
  printf("PASS extra_whitespace_tokenizing\n");
}

int main() {
  test_defaults();
  test_cfg_snapshot_line();
  test_help_lists_knobs();
  test_set_applies_and_flags();
  test_set_clamps_and_says_so();
  test_set_same_value_no_change_flag();
  test_set_rejects_junk_numbers();
  test_set_unknown_knob();
  test_reset_restores_defaults();
  test_stream_control();
  test_raw_control();
  test_case_insensitive_and_crlf();
  test_blank_and_whitespace_lines_ignored();
  test_unknown_command();
  test_overflow_line_discarded();
  test_extra_whitespace_tokenizing();
  printf("ALL TUNING CONSOLE TESTS PASSED\n");
  return 0;
}
