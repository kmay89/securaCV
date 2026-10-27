// The flagship's OTA outcome glue (firmware/canary/src/main.cpp), run for real.
//
// main.cpp's boot-health section decides when a freshly installed image is
// confirmed and when the witness chain says how the install ended. The
// functions that do it are cut verbatim out of main.cpp by cut_functions.awk
// (CANOTA_INC, the Makefile names the list) and driven here over a stand-in
// pull-OTA engine, so what runs is the shipped text:
//
//   ota_record_update_outcome()          the ONE place the outcome is witnessed
//   ota_confirm_dwell_tick()             loop()'s confirm-dwell tick
//   ota_confirm_and_witness()            the boot probes at the healthy gate
//   boot_mark_healthy(), boot_health_tick()
//   boot_health_before_deliberate_stop() serial 'x', POST /api/reboot, the
//                                        wizard's reboot, a deep sleep
//
// The stand-in keeps the engine contract the 2.4.17 train agreed
// (securacv_ota.h): the boot probes leave a passing image PENDING_VERIFY when
// a confirm dwell is set; securacv_ota_health_tick() confirms it after the
// dwell and returns true on exactly that pass; securacv_ota_confirm_now()
// confirms only an image whose probes passed and that is still pending; and
// securacv_ota_take_pending_version() holds the install's target back while
// this boot's image is pending, handing it over once (and erasing it) when
// the image is confirmed or on a boot that is not the new image (a
// rollback). A second engine mode drops the hold-back — the engine every
// fielded unit ran before 2.4.17 — and the tree must still never witness
// "applied" from an image that may yet revert.
//
// Built three times, as the envs build main.cpp: the pull engine
// (FEATURE_OTA_PULL=1: dev, release and the envs that extend them),
// FEATURE_OTA_UPDATE alone (secure, secure_ha: no engine init, no dwell,
// confirm at the probe) and neither (minimal). Each build is at gnu++11, the
// Arduino 2.0.17 core's default the canary tree compiles at.
//
// It also reads main.cpp (CANARY_MAIN_CPP) for three text pins the cut cannot
// show: securacv_ota_take_pending_version() and securacv_ota_health_tick()
// each have one caller (so no second recording site can skip the pending
// rule), and every deliberate stop persists the chain head after the gate
// and before the restart or sleep (so a witnessed outcome survives it).

#include "securacv_ota.h"
#include "health/boot_policy.h"
#include "log_level.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string.h>  // strcmp, unqualified, as main.cpp spells it
#include <vector>

#ifndef CANARY_MAIN_CPP
#error "CANARY_MAIN_CPP must name firmware/canary/src/main.cpp (the Makefile passes it)"
#endif
#ifndef CANOTA_INC
#error "CANOTA_INC must name the functions cut out of main.cpp (the Makefile passes it)"
#endif
#if !defined(FEATURE_OTA_PULL) || !defined(FEATURE_OTA_UPDATE)
#error "build with -DFEATURE_OTA_PULL=0|1 -DFEATURE_OTA_UPDATE=0|1, as the envs set them"
#endif
#if FEATURE_OTA_PULL && !FEATURE_OTA_UPDATE
#error "no env sets FEATURE_OTA_PULL without FEATURE_OTA_UPDATE"
#endif

// Any install channel: main.cpp's own guard around the probes and the record.
#define CAN_INSTALL (FEATURE_OTA_PULL || FEATURE_OTA_UPDATE)
static const int kProbeRunsAtGate = CAN_INSTALL ? 1 : 0;

static int g_failures = 0;

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d: ", __func__, __LINE__);         \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
      ++g_failures;                                            \
    }                                                          \
  } while (0)

// ── The stand-in engine ───────────────────────────────────────────────────────

// A required probe failed: the real engine marks the image invalid and
// reboots into the previous firmware, never returning. Thrown here so the
// rest of the gate does not run either.
struct RebootIntoPrevious {};

namespace eng {
bool image_pending = false;  // otadata: this boot's image is PENDING_VERIFY
bool probes_pass = true;     // what the registered boot probes answer
bool initialized = false;    // securacv_ota_init() ran (setup(), pull builds)
uint32_t dwell_ms = 0;       // config.confirm_dwell_ms
bool holds_back = true;      // the 2.4.17 take_pending_version contract
bool armed = false;          // the probes passed and the image waits
bool dwell_started = false;
uint32_t healthy_since = 0;
char marker[SECURACV_OTA_VERSION_MAX] = {0};  // the install's target, in NVS
int probe_runs = 0;
int confirms = 0;
int takes = 0;
}  // namespace eng

extern "C" {

esp_err_t securacv_ota_boot_self_test(void) {
  ++eng::probe_runs;
  if (!eng::image_pending) return ESP_OK;
  if (!eng::probes_pass) throw RebootIntoPrevious();
  if (eng::initialized && eng::dwell_ms == 0) {  // legacy: confirm at the probe
    eng::image_pending = false;
    ++eng::confirms;
    return ESP_OK;
  }
  eng::armed = true;  // init() or the ticks decide
  eng::dwell_started = false;
  return ESP_OK;
}

bool securacv_ota_pending_verify(void) { return eng::image_pending; }

void securacv_ota_confirm_now(void) {
  if (!eng::armed) return;
  if (eng::image_pending) {
    eng::image_pending = false;
    ++eng::confirms;
  }
  eng::armed = false;
  eng::dwell_started = false;
}

bool securacv_ota_health_tick(uint32_t now_ms, bool healthy) {
  if (!eng::initialized || !eng::armed || eng::dwell_ms == 0) return false;
  if (!eng::image_pending) {
    eng::armed = false;
    return false;
  }
  if (!healthy) {
    eng::dwell_started = false;
    return false;
  }
  if (!eng::dwell_started) {
    eng::dwell_started = true;
    eng::healthy_since = now_ms;
    return false;
  }
  if ((int32_t)(now_ms - eng::healthy_since) < (int32_t)eng::dwell_ms) return false;
  eng::image_pending = false;
  eng::armed = false;
  ++eng::confirms;
  return true;
}

bool securacv_ota_take_pending_version(char* buf, size_t buf_len) {
  if (buf == NULL || buf_len == 0) return false;
  buf[0] = '\0';
  if (eng::marker[0] == '\0') return false;
  if (eng::holds_back && eng::image_pending) return false;
  std::snprintf(buf, buf_len, "%s", eng::marker);
  eng::marker[0] = '\0';
  ++eng::takes;
  return true;
}

}  // extern "C"

// ── The rest of main.cpp the cut functions call ───────────────────────────────

struct Event {
  std::string type;
  std::string ver;
};
static std::vector<Event> g_events;    // the witness chain, as far as this test sees it
static int g_counter_clears = 0;        // bootguard::mark_healthy()
static std::string g_last_serial;

void ota_witness_event(const char* type, const char* version) {
  Event e;
  e.type = type;
  e.ver = version;
  g_events.push_back(e);
}

void log_health(LogLevel, LogCategory, const char*, const char* = nullptr) {}

namespace bootguard {
bool mark_healthy() {
  ++g_counter_clears;
  return true;
}
}  // namespace bootguard

struct SerialStub {
  int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_last_serial = buf;
    return n;
  }
  void println(const char* s) { g_last_serial = s; }
};
static SerialStub Serial;

enum { WL_CONNECTED = 3, WL_DISCONNECTED = 6 };
struct WiFiStub {
  bool up = true;
  int status() const { return up ? WL_CONNECTED : WL_DISCONNECTED; }
};
static WiFiStub WiFi;

// The running image's version: switched per boot, so one build can play the
// new image and the previous one a rollback returns to.
static const char* g_running = "2.4.18";
#define FIRMWARE_VERSION g_running

static bool g_setup_done = false;
static uint32_t g_setup_done_ms = 0;
static bool g_boot_healthy = false;

// A build that never calls some of them (no engine: no tick; no OTA at all:
// no recorder) still compiles every cut function, as the -Werror harness.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include CANOTA_INC
#pragma GCC diagnostic pop

// ── Boots ──────────────────────────────────────────────────────────────────────

static const uint32_t kSetupDoneMs = 10000;
static const uint32_t kGateMs = kSetupDoneMs + bootpolicy::kDefaultHealthyDwellMs;
static const uint32_t kDwellMs = 60000;  // main.cpp's ota_cfg.confirm_dwell_ms

enum Image { kNothingInstalled, kNewImage, kRolledBack };

// Power on into `image`. The marker persists across boots (NVS); everything
// else is this boot's. setup() has run when this returns, with the engine
// initialized the way main.cpp initializes it for this build.
static void boot(Image image, const char* running, bool holds_back = true) {
  g_running = running;
  eng::image_pending = (image == kNewImage);
  eng::probes_pass = true;
  eng::initialized = FEATURE_OTA_PULL != 0;  // securacv_ota_init() in setup()
  eng::dwell_ms = FEATURE_OTA_PULL ? kDwellMs : 0;
  eng::holds_back = holds_back;
  eng::armed = false;
  eng::dwell_started = false;
  eng::probe_runs = 0;
  eng::confirms = 0;
  eng::takes = 0;
  g_counter_clears = 0;
  g_last_serial.clear();
  WiFi.up = true;
  g_boot_healthy = false;
  g_setup_done = true;  // setup() returned
  g_setup_done_ms = kSetupDoneMs;
}

// An OTA install from the image before: the engine writes the target the
// moment the boot partition flips.
static void install(const char* target) {
  std::snprintf(eng::marker, sizeof(eng::marker), "%s", target);
  g_events.clear();
}

// loop(), from `from` to `to` (inclusive) in `step` ms: the confirm-dwell
// tick, then the healthy gate, in main.cpp's order.
static void run_loop(uint32_t from, uint32_t to, uint32_t step = 250) {
  for (uint32_t t = from; t <= to; t += step) {
#if FEATURE_OTA_PULL
    ota_confirm_dwell_tick(t);
#endif
    boot_health_tick(t);
  }
}

static size_t count(const char* type) {
  size_t n = 0;
  for (size_t i = 0; i < g_events.size(); ++i) n += (g_events[i].type == type);
  return n;
}

// ── Scenarios ──────────────────────────────────────────────────────────────────

// The normal update: nothing is witnessed while the image can still revert;
// the pass that confirms it witnesses "applied", once.
static void an_update_is_witnessed_applied_once_it_is_confirmed() {
  install("2.4.18");
  boot(kNewImage, "2.4.18");
  run_loop(kSetupDoneMs, kGateMs - 250);
  CHECK(eng::probe_runs == 0, "the probes ran before the healthy gate");
  CHECK(g_events.empty(), "an event before the gate");
  run_loop(kGateMs, kGateMs);
  CHECK(eng::probe_runs == kProbeRunsAtGate, "the gate ran the probes %d times", eng::probe_runs);
  CHECK(g_counter_clears == 1, "the gate did not clear the crash-loop counter");
#if FEATURE_OTA_PULL
  CHECK(eng::image_pending, "the pull build confirmed at the probe, skipping the dwell");
  CHECK(g_events.empty(), "\"%s\" witnessed while the image was still pending",
        g_events.empty() ? "" : g_events[0].type.c_str());
  CHECK(eng::marker[0] != '\0', "the target was consumed while the image was pending");
  run_loop(kGateMs + 250, kGateMs + kDwellMs - 250);
  CHECK(eng::image_pending && g_events.empty(), "confirmed or witnessed inside the dwell");
  run_loop(kGateMs + kDwellMs, kGateMs + kDwellMs + 1000);
#endif
#if CAN_INSTALL
  CHECK(!eng::image_pending, "the image was never confirmed");
  CHECK(eng::confirms == 1, "confirmed %d times", eng::confirms);
  CHECK(g_events.size() == 1 && count("fw_update_applied") == 1,
        "expected one fw_update_applied, got %zu event(s)", g_events.size());
  CHECK(g_events.size() == 1 && g_events[0].ver == "2.4.18", "applied carries the wrong version");
  CHECK(eng::marker[0] == '\0', "the target was not consumed");
  // Nothing more for the rest of the boot, nor at a deliberate restart.
  run_loop(kGateMs + kDwellMs + 1250, kGateMs + kDwellMs + 120000, 1000);
  boot_health_before_deliberate_stop();
  CHECK(g_events.size() == 1, "witnessed again after the confirm (%zu events)", g_events.size());
  CHECK(eng::confirms == 1, "confirmed again");
#else
  CHECK(eng::probe_runs == 0 && g_events.empty() && eng::image_pending,
        "a build with no install channel touched the engine");
  run_loop(kGateMs + 250, kGateMs + kDwellMs * 2, 1000);
  boot_health_before_deliberate_stop();
  CHECK(count("fw_update_applied") == 0 && eng::confirms == 0,
        "a build with no install channel confirmed or witnessed an update");
#endif
}

// The dwell fails (the home link never holds) and the unit restarts: the
// bootloader boots the previous image, which finds the target still there and
// witnesses the rollback. Before 2.4.17 the first boot of the new image took
// the target and said "applied", and the rollback went unrecorded.
static void a_failed_dwell_reads_as_a_rollback_not_an_update() {
#if FEATURE_OTA_PULL
  install("2.4.18");
  boot(kNewImage, "2.4.18");
  WiFi.up = false;
  run_loop(kSetupDoneMs, kGateMs + kDwellMs * 3, 500);
  CHECK(eng::probe_runs == 1, "the gate did not run the probes");
  CHECK(eng::image_pending, "confirmed without the home link");
  CHECK(g_events.empty(), "witnessed an image that is about to revert");
  CHECK(std::strcmp(eng::marker, "2.4.18") == 0, "the target did not survive the pending boot");
  // A crash, a watchdog, a power cut: no deliberate stop. The bootloader
  // reverts the still-pending image.
  boot(kRolledBack, "2.4.17");
  run_loop(kSetupDoneMs, kGateMs);
  CHECK(g_events.size() == 1 && count("fw_update_rolled_back") == 1,
        "expected one fw_update_rolled_back, got %zu event(s)", g_events.size());
  CHECK(g_events.size() == 1 && g_events[0].ver == "2.4.18",
        "the rollback names the target that failed");
  CHECK(eng::marker[0] == '\0', "the target was not consumed by the rollback boot");
#endif
}

// The bootloader reverted an install (any reason): the previous image
// witnesses it at its healthy gate, in every build that can install.
static void a_rollback_boot_is_witnessed_at_its_gate() {
  install("2.4.18");
  boot(kRolledBack, "2.4.17");
  run_loop(kSetupDoneMs, kGateMs + 1000);
#if CAN_INSTALL
  CHECK(g_events.size() == 1 && count("fw_update_rolled_back") == 1,
        "expected one fw_update_rolled_back, got %zu event(s)", g_events.size());
  CHECK(eng::confirms == 0, "a rollback boot confirmed something");
#else
  CHECK(g_events.empty(), "a build with no install channel witnessed an outcome");
#endif
}

// Serial 'x', POST /api/reboot or the wizard's reboot before the healthy gate
// opened: the code chose to stop, so the probes run now and a passing image
// is confirmed — not reverted by the restart — and witnessed before it.
static void a_deliberate_restart_inside_the_gate_confirms() {
  install("2.4.18");
  boot(kNewImage, "2.4.18");
  run_loop(kSetupDoneMs, kSetupDoneMs + 5000);
  boot_health_before_deliberate_stop();
  CHECK(g_boot_healthy, "the stop did not open the healthy gate");
  CHECK(g_counter_clears == 1, "the stop did not clear the crash-loop counter");
  CHECK(g_last_serial.find("deliberate restart or sleep") != std::string::npos,
        "the gate did not say why it opened: \"%s\"", g_last_serial.c_str());
  CHECK(eng::probe_runs == kProbeRunsAtGate, "the stop ran the probes %d times", eng::probe_runs);
#if CAN_INSTALL
  CHECK(!eng::image_pending, "a deliberate restart left the image pending, so it reverts");
  CHECK(eng::confirms == 1, "confirmed %d times", eng::confirms);
  CHECK(g_events.size() == 1 && count("fw_update_applied") == 1,
        "expected one fw_update_applied before the restart, got %zu event(s)", g_events.size());
#else
  CHECK(eng::image_pending && g_events.empty(), "a build with no install channel touched the engine");
#endif
}

// The same stop inside the pull engine's confirm dwell (the gate opened, the
// minute of health has not passed): 2.4.17's fix — before it, this restart
// reverted the update.
static void a_deliberate_restart_inside_the_dwell_confirms() {
#if FEATURE_OTA_PULL
  install("2.4.18");
  boot(kNewImage, "2.4.18");
  run_loop(kSetupDoneMs, kGateMs + kDwellMs / 2);
  CHECK(eng::image_pending && g_events.empty(), "premise: inside the dwell, still pending");
  boot_health_before_deliberate_stop();
  CHECK(!eng::image_pending, "a deliberate restart inside the dwell left the image pending");
  CHECK(eng::confirms == 1, "confirmed %d times", eng::confirms);
  CHECK(g_events.size() == 1 && count("fw_update_applied") == 1,
        "expected one fw_update_applied, got %zu event(s)", g_events.size());
  CHECK(g_counter_clears == 1, "the gate ran twice");
  CHECK(eng::probe_runs == 1, "the probes ran twice");
  // Had the restart not happened, the dwell must not confirm or witness again.
  run_loop(kGateMs + kDwellMs / 2 + 250, kGateMs + kDwellMs * 2);
  CHECK(eng::confirms == 1 && g_events.size() == 1, "the dwell confirmed or witnessed again");
#endif
}

// A sleep or restart taken before setup() returned (no probes have run): the
// confirm's precondition does not hold, so nothing is confirmed or witnessed,
// and the target stays for the boot that knows the outcome.
static void a_stop_before_setup_returned_confirms_nothing() {
  install("2.4.18");
  boot(kNewImage, "2.4.18");
  g_setup_done = false;
  boot_health_before_deliberate_stop();
  CHECK(!g_boot_healthy, "the gate opened before setup() returned");
  CHECK(eng::probe_runs == 0, "the probes ran before setup() returned");
  CHECK(eng::image_pending && eng::confirms == 0, "confirmed an image whose probes never ran");
  CHECK(g_events.empty(), "witnessed an outcome before setup() returned");
  CHECK(std::strcmp(eng::marker, "2.4.18") == 0, "the target was consumed");
  // Nor on the rollback boot: the outcome is the gate's to witness, after
  // setup() brought the chain up, not a stop's that came first.
  boot(kRolledBack, "2.4.17");
  g_setup_done = false;
  boot_health_before_deliberate_stop();
  CHECK(g_events.empty(), "witnessed a rollback before setup() returned");
  CHECK(std::strcmp(eng::marker, "2.4.18") == 0, "the target was consumed before setup() returned");
}

// A required probe fails at a deliberate stop: the engine reverts (it never
// returns), so nothing after the probes runs — no confirm, no record, no
// counter clear — and the previous image witnesses the rollback.
static void a_failed_probe_at_a_stop_reverts_and_confirms_nothing() {
#if CAN_INSTALL
  install("2.4.18");
  boot(kNewImage, "2.4.18");
  eng::probes_pass = false;
  bool rebooted = false;
  try {
    boot_health_before_deliberate_stop();
  } catch (const RebootIntoPrevious&) {
    rebooted = true;
  }
  CHECK(rebooted, "a failed required probe did not revert");
  CHECK(eng::confirms == 0 && eng::image_pending, "confirmed an image that failed its probe");
  CHECK(g_events.empty(), "witnessed an image that failed its probe");
  CHECK(g_counter_clears == 0, "cleared the crash-loop counter for an image that failed its probe");
  boot(kRolledBack, "2.4.17");
  run_loop(kSetupDoneMs, kGateMs);
  CHECK(g_events.size() == 1 && count("fw_update_rolled_back") == 1,
        "the previous image did not witness the rollback");
#endif
}

// Against the engine every fielded unit ran before 2.4.17 (no hold-back: the
// target is handed over even while the image is pending), the tree itself
// still never witnesses "applied" from a pending image.
static void without_the_hold_back_the_tree_still_waits() {
#if FEATURE_OTA_PULL
  install("2.4.18");
  boot(kNewImage, "2.4.18", /*holds_back=*/false);
  run_loop(kSetupDoneMs, kGateMs + kDwellMs / 2);
  CHECK(eng::image_pending, "premise: inside the dwell, still pending");
  CHECK(eng::takes == 0, "took the target while the image was pending");
  CHECK(g_events.empty(), "witnessed while the image was pending");
  run_loop(kGateMs + kDwellMs / 2 + 250, kGateMs + kDwellMs * 2);
  CHECK(g_events.size() == 1 && count("fw_update_applied") == 1,
        "expected one fw_update_applied, got %zu event(s)", g_events.size());
#endif
}

// No install: the gate runs the probes (a no-op on a confirmed image), clears
// the counter and witnesses nothing; a deliberate stop confirms nothing.
static void a_boot_with_no_install_witnesses_nothing() {
  eng::marker[0] = '\0';
  g_events.clear();
  boot(kNothingInstalled, "2.4.17");
  run_loop(kSetupDoneMs, kGateMs + kDwellMs * 2, 1000);
  boot_health_before_deliberate_stop();
  CHECK(g_events.empty(), "witnessed an outcome with no install");
  CHECK(eng::confirms == 0, "confirmed with no install");
  CHECK(g_counter_clears == 1, "the gate cleared the counter %d times", g_counter_clears);
}

// ── Text pins over main.cpp ───────────────────────────────────────────────────

static std::string read_text(const char* path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Comments out, string and character literals kept (the same reader as
// test_provisioning_gate.cpp's).
static std::string strip_comments(const std::string& t) {
  std::string out;
  enum { CODE, LINE, BLOCK, STR, CHR } st = CODE;
  for (size_t i = 0; i < t.size(); ++i) {
    const char c = t[i];
    const char n = i + 1 < t.size() ? t[i + 1] : '\0';
    switch (st) {
      case CODE:
        if (c == '/' && n == '/') { st = LINE; ++i; }
        else if (c == '/' && n == '*') { st = BLOCK; ++i; }
        else { out += c; if (c == '"') st = STR; else if (c == '\'') st = CHR; }
        break;
      case LINE:
        if (c == '\n') { out += c; st = CODE; }
        break;
      case BLOCK:
        if (c == '*' && n == '/') { st = CODE; ++i; }
        else out += (c == '\n') ? '\n' : ' ';
        break;
      case STR:
      case CHR:
        out += c;
        if (c == '\\' && i + 1 < t.size()) out += t[++i];
        else if ((st == STR && c == '"') || (st == CHR && c == '\'')) st = CODE;
        break;
    }
  }
  return out;
}

static size_t occurrences(const std::string& s, const std::string& needle) {
  size_t n = 0;
  for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) ++n;
  return n;
}

static void main_cpp_has_one_recording_site_and_persists_after_every_stop() {
  const std::string src = strip_comments(read_text(CANARY_MAIN_CPP));
  CHECK(!src.empty(), "could not read %s", CANARY_MAIN_CPP);
  if (src.empty()) return;

  // One take of the target, in the recorder; one health tick, in the dwell
  // tick. A second site could witness from a pending image or skip it.
  CHECK(occurrences(src, "securacv_ota_take_pending_version(") == 1,
        "securacv_ota_take_pending_version() has %zu call sites, want 1 (ota_record_update_outcome)",
        occurrences(src, "securacv_ota_take_pending_version("));
  CHECK(occurrences(src, "securacv_ota_health_tick(") == 1,
        "securacv_ota_health_tick() has %zu call sites, want 1 (ota_confirm_dwell_tick)",
        occurrences(src, "securacv_ota_health_tick("));
  CHECK(occurrences(src, "ota_confirm_dwell_tick(millis());") == 1,
        "loop() does not run the confirm-dwell tick exactly once");
  CHECK(occurrences(src, "ota_cfg.confirm_dwell_ms = " + std::to_string(kDwellMs) + ";") == 1,
        "main.cpp's confirm dwell is no longer %u ms: update kDwellMs here", (unsigned)kDwellMs);

  // Every deliberate stop: the gate, then the chain head persisted, then the
  // restart or the sleep — so an outcome the gate witnessed is in the head the
  // next boot resumes from.
  const std::string call = "boot_health_before_deliberate_stop();";  // a call, not the definition
  size_t calls = 0;
  for (size_t at = src.find(call); at != std::string::npos; at = src.find(call, at + 1)) {
    ++calls;
    size_t stop = std::string::npos;
    const char* stops[] = {"ESP.restart()", "lowpower_enter_deep_sleep()"};
    for (size_t i = 0; i < 2; ++i) {
      const size_t s = src.find(stops[i], at);
      if (s < stop) stop = s;
    }
    const size_t persist = src.find("witness_persist_chain_state();", at);
    const size_t line = std::count(src.begin(), src.begin() + at, '\n') + 1;
    CHECK(stop != std::string::npos, "the stop at main.cpp:%zu never restarts or sleeps", line);
    CHECK(persist != std::string::npos && persist < stop,
          "the stop at main.cpp:%zu restarts or sleeps without persisting the chain head after the gate",
          line);
  }
  CHECK(calls == 3, "boot_health_before_deliberate_stop() has %zu callers, want 3 "
        "(deliberate_restart_now, the wizard's reboot, the deep sleep): pin each new one", calls);
}

int main() {
  an_update_is_witnessed_applied_once_it_is_confirmed();
  a_failed_dwell_reads_as_a_rollback_not_an_update();
  a_rollback_boot_is_witnessed_at_its_gate();
  a_deliberate_restart_inside_the_gate_confirms();
  a_deliberate_restart_inside_the_dwell_confirms();
  a_stop_before_setup_returned_confirms_nothing();
  a_failed_probe_at_a_stop_reverts_and_confirms_nothing();
  without_the_hold_back_the_tree_still_waits();
  a_boot_with_no_install_witnesses_nothing();
  main_cpp_has_one_recording_site_and_persists_after_every_stop();

  const char* build = FEATURE_OTA_PULL ? "pull engine" : FEATURE_OTA_UPDATE ? "update only" : "no OTA";
  if (g_failures) {
    std::printf("test_canary_ota_outcome (%s): %d failure(s)\n", build, g_failures);
    return 1;
  }
  std::printf("test_canary_ota_outcome (%s): all checks passed\n", build);
  return 0;
}
