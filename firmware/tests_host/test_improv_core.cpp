// Host tests for firmware/common/network/improv_core.h — the pure half of
// Improv Wi-Fi over BLE: the packet codec, the advert's service data, and the
// provisioning session's door-and-state machine. The vectors here are the
// ones ios/Tests/SecuraCVTests/ImprovWireTests.swift pins the Swift twin to,
// so the firmware and the phone cannot drift while both stay green.
//
// The rows that matter most:
//   * the WIFI_SETTINGS frame is the standard's byte for byte (the worked
//     example from the spec round-trips, checksum included);
//   * every malformed packet is refused as InvalidRpc, never partially
//     believed: a bad checksum, a data_len that disagrees, inner lengths that
//     overrun or underrun, an empty SSID, a NUL inside a credential, an SSID
//     over 32 or a password over 64 bytes (refused, never truncated);
//   * results frame their strings with the trailing checksum, and the client
//     walk reads them back; an over-budget result is refused whole;
//   * the door: no Wi-Fi of its own → Authorized with no timeout; Wi-Fi of
//     its own → AwaitingAuthorization, and credentials are NotAuthorized; a
//     tap opens it for the TTL and the TTL closes it; a join reported good
//     is Provisioned, a bad one returns to Authorized with UnableToConnect
//     and leaves the door open to retry; a join nobody reports on times out;
//   * what goes on air follows the state: Improv while the door is open or
//     a join is in flight, the beacon otherwise.

#include "../common/network/improv_core.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace canary::net::improv;

static int g_failures = 0;

// A tiny helper so the over-budget test can call parse_result with a sink
// command without declaring one on every line.
static Command& cmd_sink() {
  static Command c;
  return c;
}

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d: ", __func__, __LINE__);         \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
      ++g_failures;                                            \
    }                                                          \
  } while (0)

// ── the standard's worked example ───────────────────────────────────────────

// improv-wifi.com/ble: `01 1E 0C {MyWirelessAP} 10 {mysecurepassword} CS`.
static std::vector<uint8_t> spec_example() {
  std::vector<uint8_t> v = {0x01, 0x1E, 0x0C};
  const char* ssid = "MyWirelessAP";
  v.insert(v.end(), ssid, ssid + 12);
  v.push_back(0x10);
  const char* pw = "mysecurepassword";
  v.insert(v.end(), pw, pw + 16);
  v.push_back(checksum(v.data(), v.size()));
  return v;
}

static void the_spec_example_parses_and_rebuilds() {
  const std::vector<uint8_t> v = spec_example();
  // 2 header + 30 data (1 + 12 + 1 + 16) + 1 checksum.
  CHECK(v.size() == 33, "example frame is 33 bytes, got %zu", v.size());
  const ParsedCommand p = parse_command(v.data(), v.size());
  CHECK(p.verdict == Parse::Ok, "example parses");
  CHECK(p.command == Command::WifiSettings, "example is WIFI_SETTINGS");
  CHECK(std::strcmp(p.wifi.ssid, "MyWirelessAP") == 0, "ssid '%s'", p.wifi.ssid);
  CHECK(std::strcmp(p.wifi.password, "mysecurepassword") == 0, "password");

  uint8_t out[MAX_COMMAND_LEN];
  const size_t n = build_wifi_settings(out, sizeof(out), "MyWirelessAP", "mysecurepassword");
  CHECK(n == v.size(), "builder writes %zu bytes, example is %zu", n, v.size());
  CHECK(n == v.size() && std::memcmp(out, v.data(), n) == 0, "builder is byte-identical to the spec");
  // The exact checksum the spec's example carries (sum & 0xFF).
  CHECK(v.back() == out[n - 1], "checksum byte agrees");
}

static void an_open_network_has_an_empty_password() {
  uint8_t out[MAX_COMMAND_LEN];
  const size_t n = build_wifi_settings(out, sizeof(out), "Cafe", "");
  CHECK(n == 3 + 1 + 4 + 1, "empty password still carries its length byte (%zu)", n);
  const ParsedCommand p = parse_command(out, n);
  CHECK(p.verdict == Parse::Ok && p.wifi.password[0] == '\0', "empty password round-trips");
  CHECK(build_wifi_settings(out, sizeof(out), "Cafe", nullptr) == n, "null password is empty");
}

static void the_longest_legal_credential_fits_and_one_more_byte_does_not() {
  std::string ssid(SSID_MAX, 's');
  std::string pw(PASSWORD_MAX, 'p');
  uint8_t out[MAX_COMMAND_LEN];
  const size_t n = build_wifi_settings(out, sizeof(out), ssid.c_str(), pw.c_str());
  CHECK(n == MAX_COMMAND_LEN, "32 + 64 fills MAX_COMMAND_LEN exactly (%zu)", n);
  const ParsedCommand p = parse_command(out, n);
  CHECK(p.verdict == Parse::Ok, "the longest legal frame parses");
  CHECK(build_wifi_settings(out, sizeof(out), (ssid + "x").c_str(), pw.c_str()) == 0,
        "a 33-byte SSID is refused by the builder");
  CHECK(build_wifi_settings(out, sizeof(out), ssid.c_str(), (pw + "x").c_str()) == 0,
        "a 65-byte password is refused by the builder");
  CHECK(build_wifi_settings(out, sizeof(out), "", "pw") == 0, "an empty SSID is refused by the builder");
}

// ── refusals ────────────────────────────────────────────────────────────────

static void a_bad_checksum_is_refused() {
  std::vector<uint8_t> v = spec_example();
  v.back() ^= 0x01;
  const ParsedCommand p = parse_command(v.data(), v.size());
  CHECK(p.verdict == Parse::BadChecksum, "flipped checksum → BadChecksum");
  CHECK(p.command == Command::Unknown, "nothing believed from a bad frame");
  CHECK(error_for(p.verdict) == Error::InvalidRpc, "the wire word is InvalidRpc");
}

static void a_length_that_disagrees_is_refused() {
  std::vector<uint8_t> v = spec_example();
  v[1] = 0x1D;                                      // claims one byte fewer
  v.back() = checksum(v.data(), v.size() - 1);      // keep the checksum honest
  CHECK(parse_command(v.data(), v.size()).verdict == Parse::BadLength, "data_len short of the bytes");
  v = spec_example();
  v.push_back(0x00);                                // a trailing byte
  v.back() = checksum(v.data(), v.size() - 1);
  CHECK(parse_command(v.data(), v.size()).verdict == Parse::BadLength, "a trailing byte is refused");
  const uint8_t tiny[2] = {0x01, 0x00};
  CHECK(parse_command(tiny, 2).verdict == Parse::BadLength, "two bytes is not a frame");
  CHECK(parse_command(nullptr, 0).verdict == Parse::BadLength, "null is not a frame");
}

static void inner_lengths_that_do_not_add_up_are_refused() {
  // ssid_len overruns the frame.
  std::vector<uint8_t> v = {0x01, 0x05, 0x09, 'a', 'b', 'c', 0x00};
  v.push_back(checksum(v.data(), v.size()));
  CHECK(parse_command(v.data(), v.size()).verdict == Parse::Malformed, "ssid_len past the end");
  // pass_len overruns the frame.
  v = {0x01, 0x05, 0x02, 'a', 'b', 0x05, 'x'};
  v.push_back(checksum(v.data(), v.size()));
  CHECK(parse_command(v.data(), v.size()).verdict == Parse::Malformed, "pass_len past the end");
  // pass_len underruns: bytes left over before the checksum.
  v = {0x01, 0x06, 0x02, 'a', 'b', 0x01, 'x', 'y'};
  v.push_back(checksum(v.data(), v.size()));
  CHECK(parse_command(v.data(), v.size()).verdict == Parse::Malformed, "bytes left after the password");
  // No room for pass_len at all.
  v = {0x01, 0x03, 0x02, 'a', 'b'};
  v.push_back(checksum(v.data(), v.size()));
  CHECK(parse_command(v.data(), v.size()).verdict == Parse::Malformed, "no pass_len byte");
  // Empty data on a WIFI_SETTINGS.
  v = {0x01, 0x00};
  v.push_back(checksum(v.data(), v.size()));
  CHECK(parse_command(v.data(), v.size()).verdict == Parse::Malformed, "no body");
}

static void an_empty_ssid_and_a_nul_inside_are_refused() {
  std::vector<uint8_t> v = {0x01, 0x03, 0x00, 0x01, 'p'};
  v.push_back(checksum(v.data(), v.size()));
  CHECK(parse_command(v.data(), v.size()).verdict == Parse::Malformed, "empty ssid");
  v = {0x01, 0x05, 0x02, 'a', 0x00, 0x01, 'p'};
  v.push_back(checksum(v.data(), v.size()));
  CHECK(parse_command(v.data(), v.size()).verdict == Parse::Malformed, "NUL inside the ssid");
  v = {0x01, 0x05, 0x01, 'a', 0x02, 'p', 0x00};
  v.push_back(checksum(v.data(), v.size()));
  CHECK(parse_command(v.data(), v.size()).verdict == Parse::Malformed, "NUL inside the password");
}

static void over_spec_credentials_are_refused_not_truncated() {
  std::vector<uint8_t> v = {0x01, 0x00, 33};
  for (int i = 0; i < 33; ++i) v.push_back('s');
  v.push_back(0x00);
  v[1] = (uint8_t)(v.size() - 2);
  v.push_back(checksum(v.data(), v.size()));
  const ParsedCommand p = parse_command(v.data(), v.size());
  CHECK(p.verdict == Parse::TooLong, "33-byte ssid → TooLong");
  CHECK(p.wifi.ssid[0] == '\0', "nothing copied from a refused frame");

  v = {0x01, 0x00, 0x01, 'a', 65};
  for (int i = 0; i < 65; ++i) v.push_back('p');
  v[1] = (uint8_t)(v.size() - 2);
  v.push_back(checksum(v.data(), v.size()));
  CHECK(parse_command(v.data(), v.size()).verdict == Parse::TooLong, "65-byte password → TooLong");
  CHECK(error_for(Parse::TooLong) == Error::InvalidRpc, "TooLong is InvalidRpc on the wire");
}

static void other_commands_parse_as_themselves() {
  uint8_t out[8];
  size_t n = build_command(out, sizeof(out), Command::Identify, nullptr, 0);
  CHECK(n == 3 && out[0] == 0x02 && out[1] == 0x00 && out[2] == 0x02, "identify is 02 00 02");
  CHECK(parse_command(out, n).command == Command::Identify, "identify parses");
  n = build_command(out, sizeof(out), Command::GetDeviceInfo, nullptr, 0);
  CHECK(parse_command(out, n).command == Command::GetDeviceInfo, "device info parses");
  n = build_command(out, sizeof(out), Command::GetWifiNetworks, nullptr, 0);
  CHECK(parse_command(out, n).command == Command::GetWifiNetworks, "networks parses");
  const uint8_t odd[3] = {0x7F, 0x00, 0x7F};
  const ParsedCommand p = parse_command(odd, 3);
  CHECK(p.verdict == Parse::Ok && p.command == (Command)0x7F, "an unknown command is parsed, then refused by the session");
  CHECK(build_command(out, 2, Command::Identify, nullptr, 0) == 0, "no room → 0");
}

// ── results ─────────────────────────────────────────────────────────────────

static void a_result_frames_its_strings() {
  uint8_t out[MAX_RESULT_LEN];
  const char* urls[] = {"http://canary-sense-ab12.local/", ""};
  const size_t n = build_result(out, sizeof(out), Command::WifiSettings, urls, 2);
  const size_t url_len = std::strlen(urls[0]);
  CHECK(n == 2 + 1 + url_len + 1 + 1, "frame = header + (len,url) + (len,'') + checksum (%zu)", n);
  CHECK(out[0] == 0x01, "result names its command");
  CHECK(out[1] == 1 + url_len + 1, "data_len covers both strings");
  CHECK(out[2] == url_len, "first string length");
  CHECK(out[3 + url_len] == 0, "second string is empty");
  CHECK(out[n - 1] == checksum(out, n - 1), "checksum closes the frame");

  Command cmd = Command::Unknown;
  std::vector<std::string> got;
  const int count = parse_result(out, n, cmd, [&](int, const char* s, size_t l) {
    got.emplace_back(s, l);
  });
  CHECK(count == 2 && cmd == Command::WifiSettings, "walk returns both strings");
  CHECK(got.size() == 2 && got[0] == urls[0] && got[1].empty(), "strings read back");

  // The empty result that closes a GET_WIFI_NETWORKS answer.
  const size_t e = build_result(out, sizeof(out), Command::GetWifiNetworks, nullptr, 0);
  CHECK(e == 3 && out[0] == 0x04 && out[1] == 0x00 && out[2] == 0x04, "empty result is 04 00 04");
  CHECK(parse_result(out, e, cmd, [](int, const char*, size_t) {}) == 0, "empty result walks as zero strings");
}

static void a_network_row_is_three_strings() {
  uint8_t out[MAX_RESULT_LEN];
  const char* row[] = {"Home", "-61", "WPA2"};
  const size_t n = build_result(out, sizeof(out), Command::GetWifiNetworks, row, 3);
  Command cmd;
  std::vector<std::string> got;
  parse_result(out, n, cmd, [&](int, const char* s, size_t l) { got.emplace_back(s, l); });
  CHECK(got.size() == 3 && got[0] == "Home" && got[1] == "-61" && got[2] == "WPA2", "ssid, rssi, auth");
}

static void an_over_budget_result_is_refused_whole() {
  uint8_t out[MAX_RESULT_LEN];
  std::string big(200, 'u');
  const char* two[] = {big.c_str(), big.c_str()};
  CHECK(build_result(out, sizeof(out), Command::WifiSettings, two, 2) == 0, "402 bytes of data → 0");
  std::string edge(253, 'u');                      // 1 + 253 = 254 ≤ 255
  const char* one[] = {edge.c_str()};
  CHECK(build_result(out, sizeof(out), Command::WifiSettings, one, 1) == 2 + 254 + 1, "254 bytes of data fits");
  std::string over(255, 'u');                      // 1 + 255 = 256 > 255
  const char* too[] = {over.c_str()};
  CHECK(build_result(out, sizeof(out), Command::WifiSettings, too, 1) == 0, "256 bytes of data → 0");
  CHECK(build_result(out, 10, Command::WifiSettings, one, 1) == 0, "a small buffer → 0");
  CHECK(parse_result(out, 2, cmd_sink(), [](int, const char*, size_t) {}) == -1, "two bytes is not a result");
}

static void a_corrupt_result_is_not_walked() {
  uint8_t out[MAX_RESULT_LEN];
  const char* one[] = {"abc"};
  const size_t n = build_result(out, sizeof(out), Command::WifiSettings, one, 1);
  Command cmd;
  out[2] = 9;                                      // inner length past the end
  out[n - 1] = checksum(out, n - 1);
  CHECK(parse_result(out, n, cmd, [](int, const char*, size_t) {}) == -1, "inner length overrun → -1");
  out[2] = 3;
  out[n - 1] ^= 0xFF;
  CHECK(parse_result(out, n, cmd, [](int, const char*, size_t) {}) == -1, "bad checksum → -1");
}

static void service_data_is_state_caps_and_four_zeros() {
  uint8_t sd[SERVICE_DATA_LEN] = {9, 9, 9, 9, 9, 9};
  build_service_data(sd, State::Authorized, CAP_IDENTIFY | CAP_SCAN_WIFI);
  CHECK(sd[0] == 0x02, "state byte");
  CHECK(sd[1] == 0x05, "caps byte");
  CHECK(sd[2] == 0 && sd[3] == 0 && sd[4] == 0 && sd[5] == 0, "reserved bytes are zero");
  CHECK(SERVICE_DATA_UUID16 == 0x4677, "service data UUID is 0x4677");
}

// ── the session ─────────────────────────────────────────────────────────────

static void no_wifi_opens_the_door_with_no_timeout() {
  Session s;
  session_begin(s, /*no_wifi=*/true);
  CHECK(s.state == State::Authorized, "first boot is Authorized");
  CHECK(s.door == Door::NoWifi, "door reason is NoWifi");
  CHECK(advert_for(s) == Advert::Improv, "Improv is on air");
  Timing t;
  CHECK(!session_tick(s, 10 * 60 * 1000, t), "ten minutes later: unchanged");
  CHECK(s.state == State::Authorized, "still Authorized — no timeout on the NoWifi door");
  CHECK(session_on_wifi_settings(s, 1000) == Error::None, "credentials are accepted");
  CHECK(s.state == State::Provisioning, "join in flight");
  CHECK(advert_for(s) == Advert::Improv, "Improv stays on air through the join");
  session_on_join_result(s, true);
  CHECK(s.state == State::Provisioned && s.error == Error::None, "joined → Provisioned");
  CHECK(s.provisioned, "provisioned flag set");
  CHECK(advert_for(s) == Advert::Beacon, "a provisioned device goes back to the beacon");
}

static void a_device_on_its_own_wifi_waits_for_a_tap() {
  Session s;
  session_begin(s, /*no_wifi=*/false);
  CHECK(s.state == State::AwaitingAuthorization, "installed device awaits authorization");
  CHECK(s.door == Door::Shut, "door shut");
  CHECK(advert_for(s) == Advert::Beacon, "nothing to provision on air");
  CHECK(session_on_wifi_settings(s, 1000) == Error::NotAuthorized, "credentials refused");
  CHECK(s.state == State::AwaitingAuthorization, "state unchanged by the refusal");
  CHECK(s.error == Error::NotAuthorized, "error published");

  Timing t;
  session_tap(s, 5000);
  CHECK(s.state == State::Authorized && s.door == Door::Tap, "a tap opens the door");
  CHECK(s.error == Error::None, "a tap clears the last error");
  CHECK(advert_for(s) == Advert::Improv, "Improv on air while the tap holds");
  CHECK(!session_tick(s, 5000 + t.tap_ttl_ms - 1, t), "just inside the TTL: open");
  CHECK(session_tick(s, 5000 + t.tap_ttl_ms, t), "at the TTL: changed");
  CHECK(s.state == State::AwaitingAuthorization && s.door == Door::Shut, "the TTL shuts the door");
  CHECK(advert_for(s) == Advert::Beacon, "beacon again");
}

static void a_tap_at_millis_zero_still_counts() {
  Session s;
  session_begin(s, false);
  session_tap(s, 0);
  CHECK(s.tap_at_ms == 1, "millis()==0 is stored as 1");
  Timing t;
  CHECK(!session_tick(s, 100, t) && s.state == State::Authorized, "open at 100 ms");
}

static void the_tap_ttl_survives_millis_wrap() {
  Session s;
  session_begin(s, false);
  const uint32_t near_wrap = 0xFFFFFFFFu - 10000;
  session_tap(s, near_wrap);
  Timing t;
  CHECK(!session_tick(s, near_wrap + 30000, t), "30 s across the wrap: still open");
  CHECK(s.state == State::Authorized, "open");
  CHECK(session_tick(s, near_wrap + t.tap_ttl_ms + 1, t), "TTL across the wrap: shut");
}

static void a_failed_join_leaves_the_door_open_to_retry() {
  Session s;
  session_begin(s, true);
  CHECK(session_on_wifi_settings(s, 1000) == Error::None, "first try");
  session_on_join_result(s, false);
  CHECK(s.state == State::Authorized, "back to Authorized");
  CHECK(s.error == Error::UnableToConnect, "UnableToConnect published");
  CHECK(!s.provisioned, "not provisioned");
  CHECK(session_on_wifi_settings(s, 2000) == Error::None, "second try accepted");
  CHECK(s.error == Error::None, "a new attempt clears the error");
}

static void a_failed_join_under_a_tap_returns_to_awaiting_when_the_ttl_passed() {
  Session s;
  session_begin(s, false);
  session_tap(s, 1000);
  Timing t;
  // Credentials land 45 s into the minute the tap bought, so the TTL runs
  // out while the join (30 s budget) is still in flight.
  CHECK(session_on_wifi_settings(s, 1000 + 45000) == Error::None, "accepted under the tap");
  // The TTL passes mid-join: the door shuts but the join is not disturbed.
  CHECK(!session_tick(s, 1000 + t.tap_ttl_ms + 1, t), "no state change mid-join");
  CHECK(s.state == State::Provisioning && s.door == Door::Shut, "join continues, door shut");
  session_on_join_result(s, false);
  CHECK(s.state == State::AwaitingAuthorization, "failed with the door shut → awaiting");
  CHECK(s.error == Error::UnableToConnect, "error says why");
}

static void a_join_nobody_reports_on_times_out() {
  Session s;
  session_begin(s, true);
  Timing t;
  CHECK(session_on_wifi_settings(s, 1000) == Error::None, "accepted");
  CHECK(!session_tick(s, 1000 + t.provisioning_timeout_ms - 1, t), "inside the timeout");
  CHECK(session_tick(s, 1000 + t.provisioning_timeout_ms, t), "at the timeout: changed");
  CHECK(s.state == State::Authorized && s.error == Error::UnableToConnect, "timed out → UnableToConnect");
}

static void a_second_write_mid_join_is_refused_without_disturbing_it() {
  Session s;
  session_begin(s, true);
  CHECK(session_on_wifi_settings(s, 1000) == Error::None, "first");
  CHECK(session_on_wifi_settings(s, 1500) == Error::InvalidRpc, "second refused");
  CHECK(s.state == State::Provisioning && s.provisioning_at_ms == 1000, "first join untouched");
  session_on_join_result(s, true);
  CHECK(s.state == State::Provisioned, "and it completes");
}

static void a_provisioned_device_refuses_until_the_door_reopens() {
  Session s;
  session_begin(s, true);
  session_on_wifi_settings(s, 1000);
  session_on_join_result(s, true);
  // The portal tore down: the device has Wi-Fi now.
  session_set_no_wifi(s, false, 2000);
  CHECK(s.door == Door::Shut, "door shuts when Wi-Fi arrives");
  CHECK(s.state == State::Provisioned, "state stays Provisioned for the phone to read");
  CHECK(session_on_wifi_settings(s, 3000) == Error::NotAuthorized, "a stranger's write is refused");
  CHECK(advert_for(s) == Advert::Beacon, "beacon on air");
  // Later the saved network stops working and the recovery portal rises.
  session_set_no_wifi(s, true, 4000);
  CHECK(s.state == State::Authorized && s.door == Door::NoWifi, "recovery reopens the door");
  CHECK(!s.provisioned && s.error == Error::None, "provisioned flag and error cleared");
  CHECK(session_on_wifi_settings(s, 5000) == Error::None, "credentials accepted again");
}

static void wifi_arriving_mid_join_does_not_disturb_the_join() {
  Session s;
  session_begin(s, true);
  session_on_wifi_settings(s, 1000);
  session_set_no_wifi(s, false, 1500);   // the join itself brought Wi-Fi up
  CHECK(s.state == State::Provisioning, "join still in flight");
  session_on_join_result(s, true);
  CHECK(s.state == State::Provisioned, "then Provisioned");
}

static void a_tap_while_the_nowifi_door_is_open_changes_nothing() {
  Session s;
  session_begin(s, true);
  session_tap(s, 1000);
  CHECK(s.door == Door::NoWifi, "the NoWifi door keeps its reason");
  Timing t;
  CHECK(!session_tick(s, 1000 + t.tap_ttl_ms + 1, t), "and never times out");
}

static void bad_packets_and_unknown_commands_publish_their_word() {
  Session s;
  session_begin(s, true);
  CHECK(session_on_bad_packet(s, Parse::BadChecksum) == Error::InvalidRpc, "bad packet → InvalidRpc");
  CHECK(s.state == State::Authorized, "state untouched");
  CHECK(session_on_unknown_command(s) == Error::UnknownRpc, "unknown → UnknownRpc");
  CHECK(s.state == State::Authorized, "state untouched");
}

static void a_stopped_session_offers_nothing() {
  Session s;
  CHECK(s.state == State::Stopped, "default is Stopped");
  CHECK(advert_for(s) == Advert::Beacon, "beacon");
  CHECK(session_on_wifi_settings(s, 1) == Error::NotAuthorized, "refused");
}

int main() {
  the_spec_example_parses_and_rebuilds();
  an_open_network_has_an_empty_password();
  the_longest_legal_credential_fits_and_one_more_byte_does_not();
  a_bad_checksum_is_refused();
  a_length_that_disagrees_is_refused();
  inner_lengths_that_do_not_add_up_are_refused();
  an_empty_ssid_and_a_nul_inside_are_refused();
  over_spec_credentials_are_refused_not_truncated();
  other_commands_parse_as_themselves();
  a_result_frames_its_strings();
  a_network_row_is_three_strings();
  an_over_budget_result_is_refused_whole();
  a_corrupt_result_is_not_walked();
  service_data_is_state_caps_and_four_zeros();
  no_wifi_opens_the_door_with_no_timeout();
  a_device_on_its_own_wifi_waits_for_a_tap();
  a_tap_at_millis_zero_still_counts();
  the_tap_ttl_survives_millis_wrap();
  a_failed_join_leaves_the_door_open_to_retry();
  a_failed_join_under_a_tap_returns_to_awaiting_when_the_ttl_passed();
  a_join_nobody_reports_on_times_out();
  a_second_write_mid_join_is_refused_without_disturbing_it();
  a_provisioned_device_refuses_until_the_door_reopens();
  wifi_arriving_mid_join_does_not_disturb_the_join();
  a_tap_while_the_nowifi_door_is_open_changes_nothing();
  bad_packets_and_unknown_commands_publish_their_word();
  a_stopped_session_offers_nothing();

  if (g_failures) {
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
  }
  std::printf("ALL improv_core TESTS PASSED\n");
  return 0;
}
