// Host test for the display's two fieldbus transports and their caller (F253):
// the REAL src/playground/playground.cpp, src/io/rs485.cpp and
// src/io/can_bus.cpp, compiled against fake_fieldbus/ (a recording Serial1,
// a scripted ESP-IDF TWAI driver, a fake clock, an empty I2C bus) with the
// 4.3B board pins and the dash config, as the envs that compile them do.
//
// Built three ways by this directory's Makefile, one per PlatformIO env that
// compiles the playground:
//   test_fieldbus_glue_inline  FEATURE_PLAYGROUND       (canary-display-playground;
//                                                        dash-b / dash-modes alike)
//   test_fieldbus_glue_rs485   FEATURE_DEVMODE + RS485  (canary-display-dash-rs485)
//   test_fieldbus_glue_can     FEATURE_DEVMODE + CAN    (canary-display-dash-can)
// Every build plays the same station scenarios and is held to ONE wire trace:
// the request bytes, the UART and TWAI configuration, the reply window, the
// test frame, the drain and the PG1 lines. The two driver builds also check
// that the station went through canary::io (the driver's own state is up
// after a tap, and its bring-up line is on the console once), and drive the
// driver's API directly: argument refusals, return codes, frame packing both
// ways, bitrate presets. Each scenario runs in its own forked process, so the
// playground's and the drivers' file-scope state starts fresh, under a 20 s
// alarm, so a mutant that never returns fails instead of hanging.
//
// Prints "ALL FIELDBUS GLUE TESTS PASSED (<build>)" on success.

#include <Arduino.h>
#include <driver/twai.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "pins.h"
#include "canary/hal/display.h"
#include "canary/log.h"
#include "canary/playground/playground.h"
#include "canary/playground/playground_ui.h"
#include "canary/ui/lvgl_port.h"
#include "boot/boot_banner.h"
#if defined(FEATURE_DEVMODE) && FEATURE_DEVMODE
#include "canary/mode/mode_glue.h"
#endif
#if defined(FEATURE_RS485) && FEATURE_RS485
#include "canary/io/modbus_rtu.h"
#include "canary/io/rs485.h"
#endif
#if defined(FEATURE_CAN) && FEATURE_CAN
#include "canary/io/can_bus.h"
#endif

#if defined(FEATURE_RS485) && FEATURE_RS485
#define BUILD_NAME "rs485"
#elif defined(FEATURE_CAN) && FEATURE_CAN
#define BUILD_NAME "can"
#else
#define BUILD_NAME "inline"
#endif

// ── What playground.cpp links against, stubbed ──────────────────────────────

canary::LogSink canary::g_log_sink = nullptr;

namespace canary::hal {
bool display_init() { return false; }  // no glass: the UI calls stay out
void backlight_set(uint8_t) {}
TouchSample touch_read() { return {}; }
bool expander_read_inputs(uint8_t*) { return false; }
bool expander_od_set(uint8_t, bool) { return true; }
}  // namespace canary::hal
namespace canary::ui {
bool lvgl_port_init() { return false; }
}  // namespace canary::ui
namespace canary::playground {
void playground_ui_create() {}
void playground_ui_update(const PgState&) {}
void playground_ui_handle_tap(int16_t, int16_t) {}
}  // namespace canary::playground
#if defined(FEATURE_DEVMODE) && FEATURE_DEVMODE
namespace canary::mode {
void mode_exit_to_fleet() { _exit(3); }  // a long-press is never played here
}  // namespace canary::mode
#endif
extern "C" {
void boot_separator(void) {}
void boot_blank(void) {}
void boot_line(const char*) {}
void boot_kv(const char*, const char*) {}
void boot_kvf(const char*, const char*, ...) {}
void boot_scene_ready(const char*, const char*, const char*) {}
}

namespace pg = canary::playground;

// ── Checks and the per-scenario runner ──────────────────────────────────────

static int g_fail = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      g_fail++;                                                        \
    }                                                                  \
  } while (0)

static int run(const char* name, void (*fn)()) {
  std::fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0) {
    alarm(20);
    fn();
    std::fflush(stdout);
    _exit(g_fail ? 1 : 0);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  const bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", name);
  if (WIFSIGNALED(st)) std::printf("    (killed by signal %d)\n", WTERMSIG(st));
  return ok ? 0 : 1;
}

// ── Console readers ─────────────────────────────────────────────────────────

static std::vector<std::string> console_lines() {
  std::vector<std::string> out;
  std::string cur;
  for (char c : Serial.text) {
    if (c == '\n') {
      if (!cur.empty() && cur.back() == '\r') cur.pop_back();
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// The PG1 event lines, each as "<station> <body>" (the timestamp dropped).
static std::vector<std::string> evts() {
  std::vector<std::string> out;
  for (const std::string& l : console_lines()) {
    if (l.rfind("PG1 ", 0) != 0) continue;
    const size_t at = l.find(" EVT ");
    if (at != std::string::npos) out.push_back(l.substr(at + 5));
  }
  return out;
}

static int lines_starting(const char* prefix) {
  int n = 0;
  for (const std::string& l : console_lines()) {
    if (l.rfind(prefix, 0) == 0) n++;
  }
  return n;
}

[[maybe_unused]] static bool has_line(const char* line) {
  for (const std::string& l : console_lines()) {
    if (l == line) return true;
  }
  return false;
}

// ── An independent CRC-16/MODBUS (bitwise; the core's is not used here) ────

static uint16_t crc_modbus(const std::vector<uint8_t>& b) {
  uint16_t crc = 0xFFFF;
  for (uint8_t x : b) {
    crc ^= x;
    for (int i = 0; i < 8; i++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}

static std::vector<uint8_t> with_crc(std::vector<uint8_t> b) {
  const uint16_t c = crc_modbus(b);
  b.push_back((uint8_t)(c & 0xFF));
  b.push_back((uint8_t)(c >> 8));
  return b;
}

// The read of holding register 0 of slave 1, as the cataloged example frame.
static const std::vector<uint8_t> kProbeRequest = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x84, 0x0A};

// A Modbus slave on the far end: answers every well-formed read-holding frame
// addressed to `slave` with `values`, `after_ms` after the request went out.
static void slave_answers(uint8_t slave, const std::vector<uint16_t>& values, uint32_t after_ms) {
  Serial1.on_frame = [slave, values, after_ms](const std::vector<uint8_t>& req) {
    if (req.size() != 8 || req[0] != slave || req[1] != 0x03) return;
    std::vector<uint8_t> body = {slave, 0x03, (uint8_t)(values.size() * 2)};
    for (uint16_t v : values) {
      body.push_back((uint8_t)(v >> 8));
      body.push_back((uint8_t)(v & 0xFF));
    }
    Serial1.inject(with_crc(body), after_ms);
  };
}

static uint64_t now_ms() { return fake::now_us / 1000; }

// ── RS-485 station scenarios (every build) ──────────────────────────────────

static void rs485_probe_reads_register() {
  slave_answers(1, {0x1234}, 20);
  pg::action_rs485_probe();
  CHECK(Serial1.begins == 1);
  CHECK(Serial1.baud == RS485_BAUD_DEFAULT);
  CHECK(Serial1.baud == 9600);
  CHECK(Serial1.config == SERIAL_8N1);
  CHECK(Serial1.rx_pin == RS485_PIN_RX);
  CHECK(Serial1.tx_pin == RS485_PIN_TX);
  CHECK(Serial1.rx_pin == 43 && Serial1.tx_pin == 44);
  CHECK(Serial1.tx == kProbeRequest);
  CHECK(Serial1.flushes == 1);
  CHECK(!Serial1.ops.empty() && Serial1.ops.front() == "begin");
  const auto e = evts();
  CHECK(e.size() == 1 && e[0] == "rs485 slave=1 reg=0 val=4660 crc=ok");
  const auto& s = pg::state().rs485;
  CHECK(s.ready);
  CHECK(s.polls == 1);
  CHECK(s.replies == 1);
  CHECK(s.last_val == 0x1234);
  CHECK(s.last_ok);

  // A second tap: the UART is not brought up again, the same frame goes out.
  slave_answers(1, {0x0001}, 5);
  pg::action_rs485_probe();
  CHECK(Serial1.begins == 1);
  std::vector<uint8_t> twice = kProbeRequest;
  twice.insert(twice.end(), kProbeRequest.begin(), kProbeRequest.end());
  CHECK(Serial1.tx == twice);
  const auto e2 = evts();
  CHECK(e2.size() == 2 && e2[1] == "rs485 slave=1 reg=0 val=1 crc=ok");
  CHECK(s.polls == 2 && s.replies == 2 && s.last_val == 1);

#if defined(FEATURE_RS485) && FEATURE_RS485
  // The station went through the driver: its state is up, and its bring-up
  // line is on the console once.
  CHECK(canary::io::rs485_ready());
  CHECK(lines_starting("[RS485] ") == 1);
  CHECK(has_line("[RS485] up on A/B (TX=44 RX=43) @ 9600 8N1"));
#else
  CHECK(lines_starting("[RS485] ") == 0);
#endif
}

static void rs485_quiet_bus_waits_the_window() {
  const uint64_t t0 = now_ms();
  pg::action_rs485_probe();
  const uint64_t waited = now_ms() - t0;
  CHECK(waited >= 300);
  CHECK(waited < 303);
  const auto e = evts();
  CHECK(e.size() == 1 && e[0] == "rs485 slave=1 reply=none");
  const auto& s = pg::state().rs485;
  CHECK(s.polls == 1 && s.replies == 0 && !s.last_ok);
  CHECK(Serial1.tx == kProbeRequest);
}

static void rs485_reply_inside_window() {
  slave_answers(1, {0x00FF}, 250);
  const uint64_t t0 = now_ms();
  pg::action_rs485_probe();
  // The probe returns once the reply is in, not at the end of its window: a
  // wait that always ran the full 300 ms would hold the playground loop that
  // long on every answered tap.
  const uint64_t took = now_ms() - t0;
  CHECK(took >= 250);
  CHECK(took < 255);
  const auto e = evts();
  CHECK(e.size() == 1 && e[0] == "rs485 slave=1 reg=0 val=255 crc=ok");
}

static void rs485_reply_after_window() {
  slave_answers(1, {0x00FF}, 350);
  pg::action_rs485_probe();
  const auto e = evts();
  CHECK(e.size() == 1 && e[0] == "rs485 slave=1 reply=none");
  CHECK(!pg::state().rs485.last_ok);
}

static void rs485_stale_bytes_drained_first() {
  // A late reply from an earlier exchange is already waiting on the line.
  Serial1.inject(with_crc({0x01, 0x03, 0x02, 0xBE, 0xEF}), 0);
  slave_answers(1, {0x0042}, 20);
  pg::action_rs485_probe();
  const auto e = evts();
  CHECK(e.size() == 1 && e[0] == "rs485 slave=1 reg=0 val=66 crc=ok");
  int reads_before_write = 0;
  for (const std::string& op : Serial1.ops) {
    if (op == "write") break;
    if (op == "read") reads_before_write++;
  }
  CHECK(reads_before_write == 7);
}

static void rs485_bad_replies_are_no_reply() {
  // Corrupt CRC.
  Serial1.on_frame = [](const std::vector<uint8_t>&) {
    std::vector<uint8_t> r = with_crc({0x01, 0x03, 0x02, 0x12, 0x34});
    r.back() ^= 0x01;
    Serial1.inject(r, 10);
  };
  pg::action_rs485_probe();
  // Another slave answering.
  Serial1.on_frame = [](const std::vector<uint8_t>&) {
    Serial1.inject(with_crc({0x02, 0x03, 0x02, 0x12, 0x34}), 10);
  };
  pg::action_rs485_probe();
  const auto e = evts();
  CHECK(e.size() == 2);
  for (const std::string& l : e) CHECK(l == "rs485 slave=1 reply=none");
  const auto& s = pg::state().rs485;
  CHECK(s.polls == 2 && s.replies == 0 && !s.last_ok);
}

// F291: a Modbus exception (fn | 0x80, code, CRC: five bytes, two fewer than
// the normal one-register reply) is decoded as soon as it is in, and the
// station says which exception it was. The old wait held out for seven bytes,
// sat through the whole 300 ms window and said "reply=none".
static void rs485_exception_returns_at_once() {
  Serial1.on_frame = [](const std::vector<uint8_t>&) {
    Serial1.inject(with_crc({0x01, 0x83, 0x02}), 10);  // illegal data address
  };
  const uint64_t t0 = now_ms();
  pg::action_rs485_probe();
  const uint64_t took = now_ms() - t0;
  CHECK(took >= 10);
  CHECK(took < 15);  // well before the 300 ms window
  auto e = evts();
  CHECK(e.size() == 1 && e[0] == "rs485 slave=1 exception=2");
  const auto& s = pg::state().rs485;
  CHECK(s.polls == 1 && s.replies == 0 && !s.last_ok);
  CHECK(Serial1.tx == kProbeRequest);

  // Another code, arriving a byte at a time: the wait ends on the fifth.
  Serial1.on_frame = [](const std::vector<uint8_t>&) {
    const std::vector<uint8_t> b = with_crc({0x01, 0x83, 0x04});  // device failure
    for (size_t i = 0; i < b.size(); i++) Serial1.inject({b[i]}, (uint32_t)(5 + 5 * i));
  };
  const uint64_t t1 = now_ms();
  pg::action_rs485_probe();
  const uint64_t took1 = now_ms() - t1;
  CHECK(took1 >= 25);
  CHECK(took1 < 30);
  e = evts();
  CHECK(e.size() == 2 && e[1] == "rs485 slave=1 exception=4");

  // A good read after an exception reads as it always did.
  slave_answers(1, {0x0007}, 5);
  pg::action_rs485_probe();
  e = evts();
  CHECK(e.size() == 3 && e[2] == "rs485 slave=1 reg=0 val=7 crc=ok");
  CHECK(s.polls == 3 && s.replies == 1 && s.last_ok && s.last_val == 7);
}

// An exception that is not whole and good is not one: it waits out the window
// and is no reply, as a cut-short normal reply is.
static void rs485_bad_exceptions_wait_the_window() {
  struct Case {
    const char* what;
    std::vector<uint8_t> bytes;
  };
  std::vector<uint8_t> bad_crc = with_crc({0x01, 0x83, 0x02});
  bad_crc.back() ^= 0x01;
  std::vector<uint8_t> bad_code = with_crc({0x01, 0x83, 0x02});
  bad_code[2] = 0x03;
  std::vector<uint8_t> cut = with_crc({0x01, 0x83, 0x02});
  cut.resize(4);
  const std::vector<Case> cases = {
      {"corrupted CRC", bad_crc},
      {"code corrupted", bad_code},
      {"cut short", cut},
      {"another slave's", with_crc({0x02, 0x83, 0x02})},
      {"an exception to another function", with_crc({0x01, 0x84, 0x02})},
  };
  size_t n = 0;
  for (const Case& c : cases) {
    const std::vector<uint8_t> bytes = c.bytes;
    Serial1.on_frame = [bytes](const std::vector<uint8_t>&) { Serial1.inject(bytes, 10); };
    const uint64_t t0 = now_ms();
    pg::action_rs485_probe();
    const uint64_t waited = now_ms() - t0;
    n++;
    if (waited < 300 || waited >= 303) {
      std::printf("    (%s exception: returned after %llu ms)\n", c.what, (unsigned long long)waited);
    }
    CHECK(waited >= 300);
    CHECK(waited < 303);
    const auto e = evts();
    CHECK(e.size() == n && e.back() == "rs485 slave=1 reply=none");
  }
  const auto& s = pg::state().rs485;
  CHECK(s.polls == n && s.replies == 0 && !s.last_ok);
}

// A normal reply is never taken for an exception, even one whose first five
// bytes carry a good CRC (its value is the CRC of its own header) and whose
// last two bytes come later: the probe waits for all seven and reads the value.
static void rs485_normal_reply_is_not_an_exception() {
  const uint16_t hc = crc_modbus({0x01, 0x03, 0x02});
  const uint16_t value = (uint16_t)(((hc & 0xFF) << 8) | (hc >> 8));  // big-endian on the wire
  const std::vector<uint8_t> whole = with_crc({0x01, 0x03, 0x02, (uint8_t)(hc & 0xFF), (uint8_t)(hc >> 8)});
  CHECK(crc_modbus(std::vector<uint8_t>(whole.begin(), whole.begin() + 3)) ==
        (uint16_t)(whole[3] | (whole[4] << 8)));  // the head alone is CRC-good
  Serial1.on_frame = [whole](const std::vector<uint8_t>&) {
    Serial1.inject(std::vector<uint8_t>(whole.begin(), whole.begin() + 5), 20);
    Serial1.inject(std::vector<uint8_t>(whole.begin() + 5, whole.end()), 40);
  };
  const uint64_t t0 = now_ms();
  pg::action_rs485_probe();
  const uint64_t took = now_ms() - t0;
  CHECK(took >= 40);
  CHECK(took < 45);
  char want[64];
  std::snprintf(want, sizeof(want), "rs485 slave=1 reg=0 val=%u crc=ok", (unsigned)value);
  const auto e = evts();
  CHECK(e.size() == 1 && e[0] == want);
  const auto& s = pg::state().rs485;
  CHECK(s.replies == 1 && s.last_ok && s.last_val == value);
}

// ── CAN station scenarios (every build) ─────────────────────────────────────

static void expect_test_frame(const fake_twai::Sent& s) {
  CHECK(s.msg.identifier == 0x100);
  CHECK(s.msg.extd == 0);
  CHECK(s.msg.rtr == 0);
  CHECK(s.msg.ss == 0);
  CHECK(s.msg.self == 0);
  CHECK(s.msg.dlc_non_comp == 0);
  CHECK(s.msg.data_length_code == 8);
  for (int i = 0; i < 8; i++) CHECK(s.msg.data[i] == 0xC0 + i);
  CHECK(s.ticks == 50);
}

static void can_send_brings_up_and_transmits() {
  pg::action_can_send();
  CHECK(fake_twai::installs == 1);
  CHECK(fake_twai::g.mode == TWAI_MODE_NORMAL);
  CHECK(fake_twai::g.tx_io == CAN_PIN_TX && fake_twai::g.rx_io == CAN_PIN_RX);
  CHECK(fake_twai::g.tx_io == 15 && fake_twai::g.rx_io == 16);
  CHECK(fake_twai::t.brp == 8);  // the 500 kbit/s preset
  CHECK(fake_twai::f.acceptance_code == 0);
  CHECK(fake_twai::f.acceptance_mask == 0xFFFFFFFF);
  CHECK(fake_twai::f.single_filter);
  CHECK(fake_twai::starts == 1);
  CHECK(fake_twai::sent.size() == 1);
  if (fake_twai::sent.size() == 1) expect_test_frame(fake_twai::sent[0]);
  auto e = evts();
  CHECK(e.size() == 1 && e[0] == "can tx id=0x100 dlc=8 ok");
  CHECK(pg::state().can.ready);
  CHECK(pg::state().can.tx == 1);

  pg::action_can_send();
  CHECK(fake_twai::installs == 1 && fake_twai::starts == 1);
  CHECK(fake_twai::sent.size() == 2);
  if (fake_twai::sent.size() == 2) expect_test_frame(fake_twai::sent[1]);
  CHECK(pg::state().can.tx == 2);

#if defined(FEATURE_CAN) && FEATURE_CAN
  CHECK(canary::io::can_ready());
  CHECK(lines_starting("[CAN] ") == 1);
  CHECK(has_line("[CAN] up on H/L (TX=15 RX=16) @ 500000 bit/s"));
#else
  CHECK(lines_starting("[CAN] ") == 0);
#endif
}

static void can_send_failures() {
  pg::action_can_send();  // up, one frame out
  fake_twai::transmit_answer = ESP_ERR_TIMEOUT;
  pg::action_can_send();
  fake_twai::transmit_answer = ESP_FAIL;
  pg::action_can_send();
  const auto e = evts();
  CHECK(e.size() == 3);
  if (e.size() == 3) {
    CHECK(e[0] == "can tx id=0x100 dlc=8 ok");
    CHECK(e[1] == "can tx=fail (bus quiet? need another node/ack)");
    CHECK(e[2] == "can tx=fail (bus quiet? need another node/ack)");
  }
  CHECK(pg::state().can.tx == 1);
  CHECK(fake_twai::sent.size() == 3);
}

static void can_install_refused_then_retried() {
  fake_twai::install_answer = ESP_FAIL;
  pg::action_can_send();
  CHECK(fake_twai::installs == 1 && fake_twai::starts == 0);
  CHECK(fake_twai::sent.empty());
  CHECK(!pg::state().can.ready);
  auto e = evts();
  CHECK(e.size() == 1 && e[0] == "can tx=fail driver");
  fake_twai::install_answer = ESP_OK;
  pg::action_can_send();
  CHECK(fake_twai::installs == 2 && fake_twai::starts == 1);
  CHECK(fake_twai::sent.size() == 1);
  e = evts();
  CHECK(e.size() == 2 && e[1] == "can tx id=0x100 dlc=8 ok");
  CHECK(pg::state().can.ready);
#if defined(FEATURE_CAN) && FEATURE_CAN
  CHECK(has_line("[CAN] driver install failed"));
#endif
}

static void can_start_refused_uninstalls() {
  fake_twai::start_answer = ESP_FAIL;
  pg::action_can_send();
  CHECK(fake_twai::installs == 1 && fake_twai::starts == 1);
  CHECK(fake_twai::uninstalls == 1 && !fake_twai::installed);
  CHECK(fake_twai::sent.empty());
  CHECK(!pg::state().can.ready);
  auto e = evts();
  CHECK(e.size() == 1 && e[0] == "can tx=fail driver");
  fake_twai::start_answer = ESP_OK;
  pg::action_can_send();
  CHECK(fake_twai::installs == 2 && fake_twai::starts == 2);
  CHECK(fake_twai::sent.size() == 1);
#if defined(FEATURE_CAN) && FEATURE_CAN
  CHECK(has_line("[CAN] start failed"));
#endif
}

static twai_message_t msg(uint32_t id, bool extd, bool rtr, uint8_t dlc, uint8_t first) {
  twai_message_t m = {};
  m.extd = extd ? 1 : 0;
  m.rtr = rtr ? 1 : 0;
  m.identifier = id;
  m.data_length_code = dlc;
  for (int i = 0; i < 8; i++) m.data[i] = (uint8_t)(first + i);
  return m;
}

// One playground pass, a drain interval after the last.
static void pass() {
  fake::now_us += 100 * 1000;
  pg::playground_loop();
}

static void can_drain_logs_frames() {
  // Nothing is read before the controller is up.
  fake_twai::inbox.push_back(msg(0x101, false, false, 8, 0xA0));
  pass();
  CHECK(fake_twai::receives == 0);
  fake_twai::inbox.clear();

  pg::action_can_send();
  Serial.text.clear();
  fake_twai::inbox.push_back(msg(0x101, false, false, 8, 0xA0));
  fake_twai::inbox.push_back(msg(0x18FF50E5, true, true, 8, 0x00));
  twai_message_t odd = msg(0x7FF, false, false, 12, 0x01);  // a non-compliant DLC
  odd.dlc_non_comp = 1;
  fake_twai::inbox.push_back(odd);
  for (int i = 0; i < 7; i++) fake_twai::inbox.push_back(msg(0x200 + i, false, false, 1, (uint8_t)i));
  CHECK(fake_twai::inbox.size() == 10);

  pass();
  auto e = evts();
  CHECK(e.size() == 8);  // eight frames a pass, no more
  if (e.size() == 8) {
    CHECK(e[0] == "can rx id=0x101 ext=0 rtr=0 dlc=8 data=A0 A1 A2 A3 A4 A5 A6 A7");
    CHECK(e[1] == "can rx id=0x18FF50E5 ext=1 rtr=1 dlc=8 remote");
    CHECK(e[2] == "can rx id=0x7FF ext=0 rtr=0 dlc=8 data=01 02 03 04 05 06 07 08");
    CHECK(e[3] == "can rx id=0x200 ext=0 rtr=0 dlc=1 data=00");
    CHECK(e[7] == "can rx id=0x204 ext=0 rtr=0 dlc=1 data=04");
  }
  CHECK(fake_twai::receives == 8);
  CHECK(pg::state().can.rx == 8);
  CHECK(pg::state().can.last_id == 0x204);

  pass();
  e = evts();
  CHECK(e.size() == 10);
  if (e.size() == 10) CHECK(e[9] == "can rx id=0x206 ext=0 rtr=0 dlc=1 data=06");
  CHECK(fake_twai::receives == 11);  // two frames, then the empty queue
  CHECK(pg::state().can.rx == 10);
  CHECK(pg::state().can.last_id == 0x206);
  for (TickType_t t : fake_twai::receive_ticks) CHECK(t == 0);  // never waits
}

static void can_bus_off_is_not_recovered() {
  pg::action_can_send();
  fake_twai::state = TWAI_STATE_BUS_OFF;
  pg::action_can_send();
  for (int i = 0; i < 20; i++) pass();
  pg::action_can_send();
  const auto e = evts();
  CHECK(e.size() == 3);
  if (e.size() == 3) {
    CHECK(e[1] == "can tx=fail (bus quiet? need another node/ack)");
    CHECK(e[2] == "can tx=fail (bus quiet? need another node/ack)");
  }
  CHECK(fake_twai::recoveries == 0);
  CHECK(fake_twai::starts == 1);
  CHECK(fake_twai::state == TWAI_STATE_BUS_OFF);
  CHECK(pg::state().can.tx == 1);
}

// ── The RS-485 driver's API (dash-rs485 build) ──────────────────────────────

#if defined(FEATURE_RS485) && FEATURE_RS485
namespace io = canary::io;
namespace mb = canary::io::modbus;

static void rs485_driver_refuses_before_begin() {
  uint16_t r[1] = {0};
  CHECK(io::rs485_read_holding(1, 0, 1, r, 1, 100) == io::RS485_ERR_NOT_READY);
  CHECK(!io::rs485_ready());
  CHECK(Serial1.begins == 0 && Serial1.tx.empty());
}

static void rs485_driver_begin() {
  CHECK(io::rs485_begin(19200));
  CHECK(io::rs485_ready());
  CHECK(Serial1.begins == 1 && Serial1.baud == 19200 && Serial1.config == SERIAL_8N1);
  CHECK(Serial1.rx_pin == 43 && Serial1.tx_pin == 44);
  CHECK(has_line("[RS485] up on A/B (TX=44 RX=43) @ 19200 8N1"));
  CHECK(io::rs485_begin(9600));  // idempotent: no second bring-up
  CHECK(Serial1.begins == 1 && Serial1.baud == 19200);
}

static void rs485_driver_begin_default_baud() {
  CHECK(io::rs485_begin(0));
  CHECK(Serial1.baud == RS485_BAUD_DEFAULT);
  CHECK(has_line("[RS485] up on A/B (TX=44 RX=43) @ 9600 8N1"));
}

static void rs485_driver_refuses_bad_arguments() {
  io::rs485_begin(0);
  uint16_t r[200] = {0};
  CHECK(io::rs485_read_holding(1, 0, 1, nullptr, 1, 100) == io::RS485_ERR_ARG);
  CHECK(io::rs485_read_holding(1, 0, 0, r, 1, 100) == io::RS485_ERR_ARG);
  CHECK(io::rs485_read_holding(1, 0, 2, r, 1, 100) == io::RS485_ERR_ARG);
  CHECK(io::rs485_read_holding(1, 0, 126, r, 200, 100) == io::RS485_ERR_ARG);
  CHECK(Serial1.tx.empty());  // nothing went out
  CHECK(Serial1.flushes == 0);
}

static void rs485_driver_reads_several_registers() {
  io::rs485_begin(0);
  slave_answers(7, {0x0102, 0x0304, 0xFFFF}, 30);
  uint16_t r[3] = {0, 0, 0};
  const uint64_t t0 = now_ms();
  CHECK(io::rs485_read_holding(7, 0x0010, 3, r, 3, 200) == 3);
  // The call returns once the 11-byte reply is in (30 ms), not at its 200 ms
  // timeout.
  const uint64_t took = now_ms() - t0;
  CHECK(took >= 30);
  CHECK(took < 35);
  CHECK(Serial1.tx == with_crc({0x07, 0x03, 0x00, 0x10, 0x00, 0x03}));
  CHECK(r[0] == 0x0102 && r[1] == 0x0304 && r[2] == 0xFFFF);
}

static void rs485_driver_honors_its_timeout() {
  io::rs485_begin(0);
  uint16_t r[1] = {0};
  const uint64_t t0 = now_ms();
  const uint64_t d0 = fake::delay_calls;
  CHECK(io::rs485_read_holding(1, 0, 1, r, 1, 50) == io::RS485_ERR_TIMEOUT);
  const uint64_t waited = now_ms() - t0;
  CHECK(waited >= 50 && waited < 53);
  CHECK(fake::delay_calls > d0);  // it yields while it waits
}

static void rs485_driver_reports_parse_faults() {
  io::rs485_begin(0);
  uint16_t r[1] = {0};
  Serial1.on_frame = [](const std::vector<uint8_t>&) {
    std::vector<uint8_t> b = with_crc({0x01, 0x03, 0x02, 0x12, 0x34});
    b[3] ^= 0x10;
    Serial1.inject(b, 5);
  };
  CHECK(io::rs485_read_holding(1, 0, 1, r, 1, 100) == mb::ERR_CRC);
  Serial1.on_frame = [](const std::vector<uint8_t>&) {
    Serial1.inject(with_crc({0x09, 0x03, 0x02, 0x12, 0x34}), 5);
  };
  CHECK(io::rs485_read_holding(1, 0, 1, r, 1, 100) == mb::ERR_ADDR);
  // A reply cut short never completes: the exchange times out.
  Serial1.on_frame = [](const std::vector<uint8_t>&) {
    std::vector<uint8_t> b = with_crc({0x01, 0x03, 0x02, 0x12, 0x34});
    b.resize(4);
    Serial1.inject(b, 5);
  };
  CHECK(io::rs485_read_holding(1, 0, 1, r, 1, 100) == io::RS485_ERR_TIMEOUT);
}

// F291 at the driver: an exception returns ERR_EXCEPTION and its code as soon
// as its five bytes are in, for a read of any length; the code is 0 after any
// other outcome, and a caller may leave it out.
static void rs485_driver_reports_exception_at_once() {
  uint8_t code = 0xEE;
  uint16_t r[3] = {0, 0, 0};
  CHECK(io::rs485_read_holding(1, 0, 1, r, 3, 100, &code) == io::RS485_ERR_NOT_READY);
  CHECK(code == 0);
  io::rs485_begin(0);

  Serial1.on_frame = [](const std::vector<uint8_t>&) {
    Serial1.inject(with_crc({0x07, 0x83, 0x02}), 8);
  };
  code = 0xEE;
  uint64_t t0 = now_ms();
  CHECK(io::rs485_read_holding(7, 0x0010, 3, r, 3, 200, &code) == mb::ERR_EXCEPTION);
  uint64_t took = now_ms() - t0;
  CHECK(took >= 8 && took < 12);  // not the 200 ms timeout
  CHECK(code == 0x02);
  CHECK(r[0] == 0 && r[1] == 0 && r[2] == 0);  // nothing written

  Serial1.on_frame = [](const std::vector<uint8_t>&) {
    Serial1.inject(with_crc({0x07, 0x83, 0x0B}), 3);
  };
  t0 = now_ms();
  CHECK(io::rs485_read_holding(7, 0, 1, r, 1, 200) == mb::ERR_EXCEPTION);  // no code pointer
  took = now_ms() - t0;
  CHECK(took >= 3 && took < 7);

  slave_answers(7, {0x0102}, 4);
  code = 0xEE;
  CHECK(io::rs485_read_holding(7, 0, 1, r, 1, 100, &code) == 1);
  CHECK(code == 0 && r[0] == 0x0102);
  Serial1.on_frame = nullptr;
  code = 0xEE;
  CHECK(io::rs485_read_holding(7, 0, 1, r, 1, 20, &code) == io::RS485_ERR_TIMEOUT);
  CHECK(code == 0);
}
#endif

// ── The CAN driver's API (dash-can build) ───────────────────────────────────

#if defined(FEATURE_CAN) && FEATURE_CAN
namespace io = canary::io;
namespace canf = canary::io::can;

static void can_driver_refuses_unsupported_bitrate() {
  CHECK(!io::can_begin(100000));
  CHECK(!io::can_ready());
  CHECK(fake_twai::installs == 0);
  CHECK(has_line("[CAN] refused: unsupported bitrate (use 125k/250k/500k/1M)"));
}

static void can_driver_preset(uint32_t bitrate, uint32_t brp) {
  CHECK(io::can_begin(bitrate));
  CHECK(fake_twai::t.brp == brp);
  CHECK(fake_twai::g.mode == TWAI_MODE_NORMAL);
  char line[64];
  std::snprintf(line, sizeof(line), "[CAN] up on H/L (TX=15 RX=16) @ %lu bit/s", (unsigned long)bitrate);
  CHECK(has_line(line));
}
static void can_driver_125k() { can_driver_preset(canf::BITRATE_125K, 32); }
static void can_driver_250k() { can_driver_preset(canf::BITRATE_250K, 16); }
static void can_driver_1m() { can_driver_preset(canf::BITRATE_1M, 4); }

static canf::Frame frame(uint32_t id, bool ext, bool rtr, uint8_t dlc) {
  canf::Frame f = {};
  f.id = id;
  f.extended = ext;
  f.rtr = rtr;
  f.dlc = dlc;
  for (int i = 0; i < 8; i++) f.data[i] = (uint8_t)(0x10 + i);
  return f;
}

static void can_driver_transmit() {
  CHECK(io::can_transmit(frame(0x100, false, false, 8), 10) == io::CAN_ERR_NOT_READY);
  CHECK(fake_twai::sent.empty());
  CHECK(io::can_begin(canf::BITRATE_500K));
  CHECK(io::can_transmit(frame(0x800, false, false, 1), 10) == io::CAN_ERR_ARG);
  CHECK(io::can_transmit(frame(0x20000000, true, false, 1), 10) == io::CAN_ERR_ARG);
  CHECK(io::can_transmit(frame(0x100, false, false, 9), 10) == io::CAN_ERR_ARG);
  CHECK(fake_twai::sent.empty());

  CHECK(io::can_transmit(frame(0x1ABCDEF5, true, false, 3), 25) == io::CAN_OK);
  CHECK(io::can_transmit(frame(0x123, false, true, 2), 0) == io::CAN_OK);
  CHECK(fake_twai::sent.size() == 2);
  if (fake_twai::sent.size() == 2) {
    const twai_message_t& a = fake_twai::sent[0].msg;
    CHECK(a.extd == 1 && a.rtr == 0 && a.identifier == 0x1ABCDEF5);
    CHECK(a.data_length_code == 3);
    CHECK(a.data[0] == 0x10 && a.data[1] == 0x11 && a.data[2] == 0x12);
    CHECK(a.data[3] == 0);  // nothing past the DLC
    CHECK(fake_twai::sent[0].ticks == 25);
    const twai_message_t& b = fake_twai::sent[1].msg;
    CHECK(b.extd == 0 && b.rtr == 1 && b.identifier == 0x123 && b.data_length_code == 2);
    CHECK(fake_twai::sent[1].ticks == 0);
  }
  fake_twai::transmit_answer = ESP_ERR_TIMEOUT;
  CHECK(io::can_transmit(frame(0x100, false, false, 0), 10) == io::CAN_ERR_TIMEOUT);
  fake_twai::transmit_answer = ESP_FAIL;
  CHECK(io::can_transmit(frame(0x100, false, false, 0), 10) == io::CAN_ERR_BUS);
}

static void can_driver_receive() {
  canf::Frame f = {};
  CHECK(io::can_receive(&f, 0) == io::CAN_ERR_NOT_READY);
  CHECK(io::can_begin(canf::BITRATE_500K));
  CHECK(io::can_receive(nullptr, 0) == io::CAN_ERR_ARG);
  CHECK(io::can_receive(&f, 7) == io::CAN_ERR_TIMEOUT);
  CHECK(!fake_twai::receive_ticks.empty() && fake_twai::receive_ticks.back() == 7);

  fake_twai::inbox.push_back(msg(0x0ABCDEF, true, false, 12, 0x30));
  CHECK(io::can_receive(&f, 0) == io::CAN_OK);
  CHECK(f.id == 0x0ABCDEF && f.extended && !f.rtr && f.dlc == 8);
  CHECK(f.data[0] == 0x30 && f.data[7] == 0x37);
  fake_twai::inbox.push_back(msg(0x055, false, true, 4, 0));
  CHECK(io::can_receive(&f, 0) == io::CAN_OK);
  CHECK(f.id == 0x055 && !f.extended && f.rtr && f.dlc == 4);

  fake_twai::installed = false;  // the driver went away under it
  CHECK(io::can_receive(&f, 0) == io::CAN_ERR_BUS);
}
#endif

int main() {
  int fails = 0;
  std::printf("test_fieldbus_glue (%s build)\n", BUILD_NAME);
  fails += run("rs485 probe reads holding register 0 of slave 1", rs485_probe_reads_register);
  fails += run("rs485 quiet bus waits out the 300 ms window", rs485_quiet_bus_waits_the_window);
  fails += run("rs485 reply at 250 ms is read", rs485_reply_inside_window);
  fails += run("rs485 reply at 350 ms is no reply", rs485_reply_after_window);
  fails += run("rs485 stale bytes drained before the request", rs485_stale_bytes_drained_first);
  fails += run("rs485 bad CRC and another slave's reply are no reply", rs485_bad_replies_are_no_reply);
  fails += run("rs485 exception returns at once and is named", rs485_exception_returns_at_once);
  fails += run("rs485 bad, cut-short or foreign exception waits the window", rs485_bad_exceptions_wait_the_window);
  fails += run("rs485 normal reply with a CRC-good head is not an exception", rs485_normal_reply_is_not_an_exception);
  fails += run("can send brings TWAI up once and sends the test frame", can_send_brings_up_and_transmits);
  fails += run("can send failures say so", can_send_failures);
  fails += run("can refused install fails the tap and retries on the next", can_install_refused_then_retried);
  fails += run("can refused start uninstalls and retries", can_start_refused_uninstalls);
  fails += run("can drain: eight frames a pass, unpacked and logged", can_drain_logs_frames);
  fails += run("can bus-off is not recovered", can_bus_off_is_not_recovered);
#if defined(FEATURE_RS485) && FEATURE_RS485
  fails += run("rs485 driver refuses before begin", rs485_driver_refuses_before_begin);
  fails += run("rs485 driver begin is idempotent", rs485_driver_begin);
  fails += run("rs485 driver begin(0) takes the board default", rs485_driver_begin_default_baud);
  fails += run("rs485 driver refuses bad arguments", rs485_driver_refuses_bad_arguments);
  fails += run("rs485 driver reads several registers", rs485_driver_reads_several_registers);
  fails += run("rs485 driver honors its timeout and yields", rs485_driver_honors_its_timeout);
  fails += run("rs485 driver reports parse faults and a cut-short reply", rs485_driver_reports_parse_faults);
  fails += run("rs485 driver reports an exception and its code at once", rs485_driver_reports_exception_at_once);
#endif
#if defined(FEATURE_CAN) && FEATURE_CAN
  fails += run("can driver refuses an unsupported bitrate", can_driver_refuses_unsupported_bitrate);
  fails += run("can driver 125k preset", can_driver_125k);
  fails += run("can driver 250k preset", can_driver_250k);
  fails += run("can driver 1M preset", can_driver_1m);
  fails += run("can driver transmit packs frames and maps results", can_driver_transmit);
  fails += run("can driver receive unpacks frames and maps results", can_driver_receive);
#endif
  if (fails) {
    std::printf("%d FIELDBUS GLUE SCENARIO(S) FAILED (%s)\n", fails, BUILD_NAME);
    return 1;
  }
  std::printf("ALL FIELDBUS GLUE TESTS PASSED (%s)\n", BUILD_NAME);
  return 0;
}
