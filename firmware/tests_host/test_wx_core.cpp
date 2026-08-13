// test_wx_core.cpp — the standalone-weather request/blob contract, pinned.
//
// The request shape IS the privacy disclosure: an anonymous forecast query
// over a coarse coordinate, nothing else. These tests fail the build if the
// URL ever grows an identifier-shaped field, if the coordinate sharpens
// past the 0.1° grid, or if the combined-location codec can smuggle an
// out-of-range coordinate into the store.

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "canary/net/wx_core.h"
#include "canary/glass_settings.h"

static int g_fail = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
      g_fail++;                                        \
    }                                                  \
  } while (0)

using namespace canary::net;
using namespace canary::glass;

int main() {
  char url[320];

  // The URL is exactly the disclosed shape: coarse coordinate + field names.
  wx_url_path(374, -1224, url, sizeof(url));
  CHECK(strstr(url, "latitude=37.4") && strstr(url, "longitude=-122.4"),
        "one-decimal coordinate, sign carried: %s", url);
  CHECK(strstr(url, "forecast_days=2") && strstr(url, "timezone=auto"),
        "today + tomorrow, zone resolved server-side from the same coord");
  // Nothing identifier-shaped, ever. The words themselves must not appear.
  for (const char* bad : {"key", "token", "id=", "mac", "uuid", "serial"}) {
    CHECK(strstr(url, bad) == nullptr, "URL grew '%s': %s", bad, url);
  }
  // Southern/eastern hemisphere signs survive.
  wx_url_path(-338, 1512, url, sizeof(url));
  CHECK(strstr(url, "latitude=-33.8") && strstr(url, "longitude=151.2"),
        "negative latitude prints correctly: %s", url);

  // WMO code words: the plain vocabulary, unknown degrades to absent.
  CHECK(strcmp(wx_code_word(0), "clear") == 0, "0 = clear");
  CHECK(strcmp(wx_code_word(2), "some clouds") == 0, "2 = some clouds");
  CHECK(strcmp(wx_code_word(63), "rain") == 0, "63 = rain");
  CHECK(strcmp(wx_code_word(95), "thunderstorm") == 0, "95 = thunderstorm");
  CHECK(wx_code_word(42)[0] == '\0', "unknown code renders as nothing");

  // The blob is the hub's own retained shape, absent fields truly absent.
  char blob[256];
  wx_blob_build(blob, sizeof(blob), 214, 240, 130, 20, "some clouds", 260,
                140, 10, "clear", 1752854400u);
  CHECK(strstr(blob, "\"t_now\":21.4") && strstr(blob, "\"hi\":24.0"),
        "temps are x10-rendered: %s", blob);
  CHECK(strstr(blob, "\"hi2\":26.0") && strstr(blob, "\"rain2\":10"),
        "tomorrow rides the same blob: %s", blob);
  CHECK(strstr(blob, "\"ts\":1752854400"), "freshness stamp present");
  wx_blob_build(blob, sizeof(blob), -10000, 240, 130, -1, "", -10000, -10000,
                -1, "", 7u);
  CHECK(strstr(blob, "t_now") == nullptr && strstr(blob, "rain") == nullptr,
        "absent fields are absent, not zero: %s", blob);
  CHECK(strstr(blob, "\"hi\":24.0") && strstr(blob, "\"ts\":7}"),
        "present fields still render: %s", blob);
  // Negative temperatures keep their sign and tenths.
  wx_blob_build(blob, sizeof(blob), -55, -10, -152, 0, "snow", -10000, -10000,
                -1, "", 1u);
  CHECK(strstr(blob, "\"t_now\":-5.5") && strstr(blob, "\"lo\":-15.2"),
        "negative x10 temps render honestly: %s", blob);

  // The combined-location codec round-trips the whole valid grid and
  // refuses everything outside it (half a coordinate can never be stored).
  int16_t la, lo;
  CHECK(wx_loc_decode(wx_loc_encode(374, -1224), &la, &lo) && la == 374 &&
            lo == -1224,
        "round-trip");
  CHECK(wx_loc_decode(wx_loc_encode(-900, -1800), &la, &lo) && la == -900 &&
            lo == -1800,
        "grid corner min");
  CHECK(wx_loc_decode(wx_loc_encode(900, 1800), &la, &lo) && la == 900 &&
            lo == 1800,
        "grid corner max");
  CHECK(!wx_loc_decode(-1, &la, &lo), "negative refused");
  CHECK(!wx_loc_decode(WX_LOC_MAX + 1, &la, &lo), "past the grid refused");

  if (g_fail) {
    std::printf("test_wx_core: %d FAILURE(S)\n", g_fail);
    return 1;
  }
  std::printf("test_wx_core: all checks passed\n");
  return 0;
}
