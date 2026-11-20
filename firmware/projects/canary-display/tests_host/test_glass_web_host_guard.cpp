// Host test for the display LAN API's Host guard (src/net/glass_web.cpp,
// write-guard note 2b). glass_web.cpp needs the Arduino WebServer, so this
// reads the source (path passed in as GLASS_WEB_CPP) instead of running it.
//
// Audit A2-12: a DNS-rebinding page (Host: attacker.example, re-pointed at the
// glass's LAN IP) is same-origin with the device, so it reads any route that
// does not check the Host itself. handle_glass refused it; /api/device (which
// names the home Wi-Fi network), /api/log and /api/ota/status did not. Each
// handler named here must refuse a foreign Host before its first 200.
//
// Run: make -C firmware/projects/canary-display/tests_host run-glass-web-host-guard

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef GLASS_WEB_CPP
#error "build with -DGLASS_WEB_CPP=\"<path to src/net/glass_web.cpp>\""
#endif

static int g_fail = 0;
#define CHECK(cond, msg)                                               \
  do {                                                                 \
    if (!(cond)) { std::printf("FAIL: %s\n", (msg)); ++g_fail; }       \
    else         { std::printf("ok:   %s\n", (msg)); }                 \
  } while (0)

static std::string read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// The text of `sig`'s body up to its first 200 answer: the Host guard must sit
// in that span, so a foreign Host is refused before any byte is served.
static std::string head_before_200(const std::string& src, const std::string& sig) {
  const size_t at = src.find(sig);
  if (at == std::string::npos) return std::string();
  const size_t ok = src.find("s_server->send(200", at);
  if (ok == std::string::npos) return std::string();
  return src.substr(at, ok - at);
}

int main() {
  const std::string src = read_file(GLASS_WEB_CPP);
  CHECK(!src.empty(), "glass_web.cpp read");

  const char* const guard = "if (host_is_foreign()) {";
  const char* const refusal =
      "s_server->send(403, \"application/json\", \"{\\\"ok\\\":false,\\\"err\\\":\\\"host\\\"}\");";

  struct Route { const char* sig; const char* what; };
  const Route routes[] = {
      {"void handle_device() {", "/api/device refuses a foreign Host first"},
      {"void handle_log() {", "/api/log refuses a foreign Host first"},
      {"void handle_ota_status() {", "/api/ota/status refuses a foreign Host first"},
      {"void handle_glass() {", "/api/glass refuses a foreign Host first"},
  };
  for (const Route& r : routes) {
    const std::string head = head_before_200(src, r.sig);
    CHECK(!head.empty(), r.sig);
    const size_t g = head.find(guard);
    CHECK(g != std::string::npos && head.find(refusal, g) != std::string::npos, r.what);
  }

  std::printf(g_fail == 0 ? "\nALL glass web host-guard checks PASSED\n"
                          : "\n%d FAILURE(S)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
