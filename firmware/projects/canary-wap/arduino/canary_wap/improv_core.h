/**
 * @file improv_core.h
 * @brief The pure half of Improv Wi-Fi over BLE: the packet codec, the
 *        service-data byte layout, and the provisioning session's state
 *        machine. No Arduino, no NimBLE, no heap.
 *
 * Improv Wi-Fi (improv-wifi.com, the open standard Home Assistant, ESPHome and
 * the improv-wifi web SDK speak) is how a phone hands a brand-new Canary its
 * Wi-Fi over Bluetooth with one tap: the device advertises the Improv service,
 * the phone shows a card, the person taps it, the credentials cross, and the
 * device answers with its own verdict and the address it can now be reached
 * at. Every Canary family that carries a radio uses this one file for the
 * bytes and the rules; the NimBLE glue beside it (improv_ble.cpp) is the thin
 * part, and the per-family code is thinner still. The iOS twin is
 * ios/Shared/ImprovWire.swift, pinned to the same vectors by
 * firmware/tests_host/test_improv_core.cpp and ImprovWireTests.swift.
 *
 * WIRE CONTRACT (the standard's, restated once so a reader needs no browser):
 *
 *   Service        00467768-6228-2272-4663-277478268000
 *   Current state  ...8001  READ | NOTIFY   one byte, State below
 *   Error state    ...8002  READ | NOTIFY   one byte, Error below
 *   RPC command    ...8003  WRITE           the phone's request, framed below
 *   RPC result     ...8004  READ | NOTIFY   the device's answer, framed below
 *   Capabilities   ...8005  READ            one byte, CAP_* bits
 *
 *   Service data in the advert, under the 16-bit UUID 0x4677 (6 bytes):
 *     [0] current state   [1] capabilities   [2..5] reserved, zero
 *   What SecuraCV puts on air (improv_ble.cpp and the WAP's Opera own the
 *   bytes; this is the shape): the fleet presence beacon stays the PRIMARY
 *   advert at all times, so a display's passive roster scan keeps hearing
 *   a device whether or not its door is open. While the door is open the
 *   primary gains the flags AD, FLEET_BEACON_FLAG_SETUP_OPEN in the beacon
 *   and a short "<Family>-XXXX" name, the device is connectable, and the
 *   SCAN RESPONSE carries the 128-bit service UUID plus this service data —
 *   iOS, Android and Chrome each merge the scan response into the
 *   advertisement a service filter matches, so a filtered scan still finds
 *   it. Door shut: the beacon alone, no scan response, and on a board with
 *   no other service, not connectable.
 *
 *   RPC command   [cmd][data_len][data ...][checksum]
 *   WIFI_SETTINGS data: [ssid_len][ssid bytes][pass_len][pass bytes]
 *   RPC result    [cmd][data_len][len_1][str_1] ... [len_n][str_n][checksum]
 *   checksum = the low 8 bits of the sum of every byte before it.
 *
 *   A WIFI_SETTINGS result carries URLs the device can now be reached at
 *   (first one is where a client should go; empty string when there is
 *   none). A GET_WIFI_NETWORKS answer is one result per network, three
 *   strings each (ssid, rssi as decimal text, auth name), closed by one
 *   empty result. A GET_DEVICE_INFO result is four strings: firmware name,
 *   firmware version, hardware, device name.
 *
 * THE SESSION (what "authorized" means here):
 *
 *   The standard leaves "authorization" to the device: a button press, or
 *   nothing at all. SecuraCV's rule, in one sentence: the door is open for
 *   a device NOBODY OWNS YET, and for an owner's own tap — never for a
 *   device that has an owner and a bad day.
 *
 *   - NO CREDENTIALS STORED (first boot, or after a factory reset): the
 *     door opens on its own for a bounded first-boot window
 *     (Timing::first_boot_window_ms, half an hour by default; a power cycle
 *     re-arms it; a compile-time 0 makes the device tap-only). Long enough
 *     to plug in three Canaries and open the app later; short enough that
 *     a unit forgotten in a drawer is not claimable from the street for
 *     the rest of its life.
 *   - CREDENTIALS STORED: the door is shut, whatever the network is doing.
 *     A saved network that stopped working raises the recovery portal on
 *     the SoftAP side (that door has a key printed on the unit) — it does
 *     NOT open this one. A device that has an owner is not re-pointable
 *     at a stranger's network because the router rebooted; the first
 *     version of this file got that wrong, and the one place it was wrong
 *     was the one place it mattered.
 *   - A PHYSICAL TAP (where the board has a button the firmware reads)
 *     opens the door for Timing::tap_ttl_ms, on a device with or without
 *     credentials — the owner's own act, the same trust the BOOT-tap
 *     receipt already expresses.
 *
 *   While the door is open, three bounds keep a stranger in radio range
 *   from doing much with it: accepted WIFI_SETTINGS writes are rate-limited
 *   (Timing::wifi_settings_cooldown_ms) and capped per open door
 *   (Timing::wifi_settings_cap — reaching it shuts the door until a tap or
 *   a power cycle), and a connected client that sends nothing for
 *   Timing::idle_disconnect_ms is dropped so it cannot park on the single
 *   link. Encryption is the glue's job (improv_ble.cpp: LE Secure
 *   Connections Just Works, checked both by the characteristic's
 *   properties and again in the write handler).
 *
 * All time math is wrap-safe unsigned-delta (now - then), per the firmware
 * idiom for millis() arithmetic.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace canary {
namespace net {
namespace improv {

// ── UUIDs ──────────────────────────────────────────────────────────────────

inline constexpr const char* SERVICE_UUID       = "00467768-6228-2272-4663-277478268000";
inline constexpr const char* CURRENT_STATE_UUID = "00467768-6228-2272-4663-277478268001";
inline constexpr const char* ERROR_STATE_UUID   = "00467768-6228-2272-4663-277478268002";
inline constexpr const char* RPC_COMMAND_UUID   = "00467768-6228-2272-4663-277478268003";
inline constexpr const char* RPC_RESULT_UUID    = "00467768-6228-2272-4663-277478268004";
inline constexpr const char* CAPABILITIES_UUID  = "00467768-6228-2272-4663-277478268005";

/// The 16-bit UUID the advert's service data rides under.
inline constexpr uint16_t SERVICE_DATA_UUID16 = 0x4677;
inline constexpr size_t   SERVICE_DATA_LEN    = 6;

// ── Vocabulary ─────────────────────────────────────────────────────────────

enum class State : uint8_t {
  Stopped               = 0x00,  // the service is registered but not on offer
  AwaitingAuthorization = 0x01,  // a physical interaction opens the door
  Authorized            = 0x02,  // credentials are accepted
  Provisioning          = 0x03,  // credentials received, the join is in flight
  Provisioned           = 0x04,  // the join worked
};

enum class Error : uint8_t {
  None            = 0x00,
  InvalidRpc      = 0x01,  // a packet that is not a packet (length, checksum, shape)
  UnknownRpc      = 0x02,  // a command this device does not speak
  UnableToConnect = 0x03,  // the join failed
  NotAuthorized   = 0x04,  // credentials sent while the door was shut
  BadHostname     = 0x05,
  Unknown         = 0xFF,
};

enum class Command : uint8_t {
  Unknown         = 0x00,
  WifiSettings    = 0x01,
  Identify        = 0x02,
  GetDeviceInfo   = 0x03,
  GetWifiNetworks = 0x04,
  Hostname        = 0x05,
  DeviceName      = 0x06,
  GetNetworkState = 0x07,
};

/// Capability bits (the advert's byte [1] and the characteristic ...8005).
inline constexpr uint8_t CAP_IDENTIFY    = 0x01;
inline constexpr uint8_t CAP_DEVICE_INFO = 0x02;
inline constexpr uint8_t CAP_SCAN_WIFI   = 0x04;
inline constexpr uint8_t CAP_HOSTNAME    = 0x08;

/// WPA2's own bounds. Longer is never a real home credential, so a longer
/// one is refused, never truncated (a truncated password persisted as the
/// one the person typed is the worst outcome).
inline constexpr size_t SSID_MAX     = 32;
inline constexpr size_t PASSWORD_MAX = 64;

/// The largest command a client can legitimately write: header (2) +
/// ssid_len (1) + 32 + pass_len (1) + 64 + checksum (1).
inline constexpr size_t MAX_COMMAND_LEN = 2 + 1 + SSID_MAX + 1 + PASSWORD_MAX + 1;
/// The largest result: header (2) + 255 data + checksum (1).
inline constexpr size_t MAX_RESULT_LEN = 258;

// ── Checksum ───────────────────────────────────────────────────────────────

/// The low 8 bits of the sum of `n` bytes — the standard's trailing byte.
inline uint8_t checksum(const uint8_t* b, size_t n) {
  uint32_t sum = 0;
  for (size_t i = 0; i < n; ++i) sum += b[i];
  return (uint8_t)(sum & 0xFF);
}

// ── Command parsing ────────────────────────────────────────────────────────

struct WifiSettings {
  char ssid[SSID_MAX + 1];
  char password[PASSWORD_MAX + 1];
};

/// Why a command packet was or was not accepted. Everything but Ok maps to
/// Error::InvalidRpc on the wire (error_for below): the standard has one
/// word for "not a packet", and a device that names which byte was wrong
/// is a device helping someone probe it.
enum class Parse : uint8_t {
  Ok,
  BadLength,    // shorter than a frame, or data_len disagrees with the bytes
  BadChecksum,
  Malformed,    // a WIFI_SETTINGS body whose inner lengths do not add up, or
                // an empty SSID, or a NUL inside a credential
  TooLong,      // SSID over 32 or password over 64 bytes
};

struct ParsedCommand {
  Command command;
  Parse verdict;
  WifiSettings wifi;   // filled only for WifiSettings with verdict Ok
};

/// Zero a credential buffer in a way the optimizer keeps.
inline void wipe(void* p, size_t n) {
  volatile uint8_t* b = (volatile uint8_t*)p;
  while (n--) *b++ = 0;
}

/// Parse one RPC command packet exactly as written (checksum included).
///
/// Strict on purpose: `data_len` must account for every byte between the
/// header and the checksum, and a WIFI_SETTINGS body must consume its data
/// exactly. The reference implementation tolerates trailing bytes; this one
/// does not, because the only packets that carry them are not from a client
/// that speaks the standard.
inline ParsedCommand parse_command(const uint8_t* data, size_t len) {
  ParsedCommand out;
  memset(&out, 0, sizeof(out));
  out.command = Command::Unknown;
  out.verdict = Parse::BadLength;
  if (data == nullptr || len < 3) return out;
  const size_t data_len = data[1];
  if (data_len != len - 3) return out;
  if (checksum(data, len - 1) != data[len - 1]) {
    out.verdict = Parse::BadChecksum;
    return out;
  }
  out.command = (Command)data[0];
  if (out.command != Command::WifiSettings) {
    out.verdict = Parse::Ok;
    return out;
  }
  // [2] ssid_len, [3 ..] ssid, then pass_len, pass — and nothing after.
  out.verdict = Parse::Malformed;
  if (data_len < 2) return out;
  const size_t ssid_len = data[2];
  const size_t ssid_start = 3;
  const size_t ssid_end = ssid_start + ssid_len;          // index of pass_len
  if (ssid_end >= len - 1) return out;                     // no room for pass_len
  const size_t pass_len = data[ssid_end];
  const size_t pass_start = ssid_end + 1;
  const size_t pass_end = pass_start + pass_len;
  if (pass_end != len - 1) return out;                     // must land on the checksum
  if (ssid_len == 0) return out;
  if (ssid_len > SSID_MAX || pass_len > PASSWORD_MAX) {
    out.verdict = Parse::TooLong;
    return out;
  }
  for (size_t i = ssid_start; i < ssid_end; ++i) if (data[i] == 0) return out;
  for (size_t i = pass_start; i < pass_end; ++i) if (data[i] == 0) return out;
  memcpy(out.wifi.ssid, data + ssid_start, ssid_len);
  out.wifi.ssid[ssid_len] = '\0';
  memcpy(out.wifi.password, data + pass_start, pass_len);
  out.wifi.password[pass_len] = '\0';
  out.verdict = Parse::Ok;
  return out;
}

/// The error byte a refused packet earns.
inline Error error_for(Parse p) {
  return p == Parse::Ok ? Error::None : Error::InvalidRpc;
}

// ── Building ───────────────────────────────────────────────────────────────

/// Encode one RPC command (the client side; the firmware uses it only in
/// tests, the Swift twin uses it for real). Returns bytes written, 0 if it
/// does not fit or a string is over 255 bytes.
inline size_t build_command(uint8_t* out, size_t cap, Command cmd,
                            const uint8_t* data, size_t data_len) {
  if (out == nullptr || data_len > 255 || cap < data_len + 3) return 0;
  out[0] = (uint8_t)cmd;
  out[1] = (uint8_t)data_len;
  if (data_len) memcpy(out + 2, data, data_len);
  out[data_len + 2] = checksum(out, data_len + 2);
  return data_len + 3;
}

/// Encode a WIFI_SETTINGS command from an SSID and a password.
inline size_t build_wifi_settings(uint8_t* out, size_t cap,
                                  const char* ssid, const char* password) {
  if (ssid == nullptr) return 0;
  const size_t sl = strlen(ssid);
  const size_t pl = password ? strlen(password) : 0;
  if (sl == 0 || sl > SSID_MAX || pl > PASSWORD_MAX) return 0;
  uint8_t body[1 + SSID_MAX + 1 + PASSWORD_MAX];
  size_t n = 0;
  body[n++] = (uint8_t)sl;
  memcpy(body + n, ssid, sl); n += sl;
  body[n++] = (uint8_t)pl;
  if (pl) { memcpy(body + n, password, pl); n += pl; }
  const size_t written = build_command(out, cap, Command::WifiSettings, body, n);
  wipe(body, sizeof(body));
  return written;
}

/// Encode one RPC result: `count` strings, each length-prefixed. Returns
/// bytes written, 0 if the strings do not fit the 255-byte data budget.
inline size_t build_result(uint8_t* out, size_t cap, Command cmd,
                           const char* const* strings, size_t count) {
  if (out == nullptr || cap < 3) return 0;
  size_t pos = 2;
  for (size_t i = 0; i < count; ++i) {
    const char* s = strings[i] ? strings[i] : "";
    const size_t sl = strlen(s);
    if (sl > 255) return 0;
    if (pos + 1 + sl > 2 + 255 || pos + 1 + sl + 1 > cap) return 0;
    out[pos++] = (uint8_t)sl;
    if (sl) memcpy(out + pos, s, sl);
    pos += sl;
  }
  out[0] = (uint8_t)cmd;
  out[1] = (uint8_t)(pos - 2);
  out[pos] = checksum(out, pos);
  return pos + 1;
}

/// The six service-data bytes the advert carries under 0x4677.
inline void build_service_data(uint8_t out[SERVICE_DATA_LEN], State state, uint8_t caps) {
  out[0] = (uint8_t)state;
  out[1] = caps;
  out[2] = out[3] = out[4] = out[5] = 0;
}

// ── Result parsing (the client side; pinned here so the Swift twin and the
//    tests agree on the shape) ───────────────────────────────────────────────

/// Walk one result packet's strings. `on_string(index, ptr, len)` is called
/// per string; returns the count, or -1 when the packet is not a result.
template <typename Fn>
inline int parse_result(const uint8_t* data, size_t len, Command& cmd, Fn&& on_string) {
  if (data == nullptr || len < 3) return -1;
  if ((size_t)data[1] != len - 3) return -1;
  if (checksum(data, len - 1) != data[len - 1]) return -1;
  cmd = (Command)data[0];
  size_t pos = 2;
  const size_t end = len - 1;
  int count = 0;
  while (pos < end) {
    const size_t sl = data[pos++];
    if (pos + sl > end) return -1;
    on_string(count, (const char*)(data + pos), sl);
    pos += sl;
    ++count;
  }
  return count;
}

// ── The session ────────────────────────────────────────────────────────────

struct Timing {
  /// A physical tap keeps the door open this long (the standard suggests a
  /// minute).
  uint32_t tap_ttl_ms = 60000;
  /// A join the device never reports on reads as failed after this long —
  /// the setup portal's own wizard timeout, so both doors agree.
  uint32_t provisioning_timeout_ms = 30000;
  /// A device with no stored credentials opens the door for this long after
  /// power-on (and again after a factory reset wipes them at runtime).
  /// 0 = never on its own: tap-only.
  uint32_t first_boot_window_ms = 30u * 60u * 1000u;
  /// Two accepted WIFI_SETTINGS writes are at least this far apart; a
  /// client that writes faster is told InvalidRpc and nothing is started.
  uint32_t wifi_settings_cooldown_ms = 3000;
  /// WIFI_SETTINGS attempts admitted per open door (accepted or malformed);
  /// the attempt past the cap shuts the door until a tap or a power cycle.
  uint8_t  wifi_settings_cap = 10;
  /// A connected client that has sent nothing for this long is dropped —
  /// long enough to find a password, short enough that a stranger cannot
  /// hold the single link against the owner.
  uint32_t idle_disconnect_ms = 180000;
  /// After the join succeeded the link is dropped after this long, so the
  /// device goes back to its beacon (the phone has read its verdict and,
  /// on a WAP, the claim by then).
  uint32_t provisioned_linger_ms = 20000;
};

/// Why the door is open — the session reports it so a log line can say.
enum class Door : uint8_t {
  Shut,           // credentials stored (or the window ran out): a tap opens it
  NoCredentials,  // nothing stored: open for the first-boot window
  Tap,            // a physical tap: open for Timing::tap_ttl_ms
};

/// 0 is the "never" sentinel in every timestamp below, so a clock reading
/// of exactly 0 is stored as 1 — one millisecond lost, never an event.
inline uint32_t stamp(uint32_t now_ms) { return now_ms == 0 ? 1u : now_ms; }

struct Session {
  State state = State::Stopped;
  Error error = Error::None;
  Door  door  = Door::Shut;
  /// The last word the glue gave on credentials (begin / set_no_credentials).
  bool no_credentials = false;
  uint32_t tap_at_ms = 0;
  /// When the first-boot window began; 0 = none.
  uint32_t window_at_ms = 0;
  /// The window ran out or the cap shut it: no reopening without a tap or
  /// a power cycle (a factory reset re-arms, since it wipes credentials).
  bool window_spent = false;
  uint32_t provisioning_at_ms = 0;
  uint32_t provisioned_at_ms = 0;
  /// Rate limit bookkeeping for the current open door.
  uint32_t last_wifi_settings_ms = 0;   // 0 = none yet this door
  uint8_t  wifi_settings_count = 0;
  /// The one link: up, and when it last asked for anything.
  bool     link_up = false;
  uint32_t last_activity_ms = 0;
  /// Set by a join result; cleared when the door reopens. Lets the glue
  /// keep the service in Provisioned long enough for the phone to read the
  /// result, then fall back to the beacon on its own terms.
  bool provisioned = false;
};

inline bool door_open(const Session& s) { return s.door != Door::Shut; }

namespace detail {
inline void open_no_credentials_door(Session& s) {
  s.door = Door::NoCredentials;
  s.provisioned = false;
  s.error = Error::None;
  s.last_wifi_settings_ms = 0;
  s.wifi_settings_count = 0;
  if (s.state != State::Provisioning) s.state = State::Authorized;
}
inline void shut_door(Session& s) {
  s.door = Door::Shut;
  if (s.state == State::Authorized) s.state = State::AwaitingAuthorization;
}
}  // namespace detail

/// Begin: with no stored credentials the first-boot window opens the door
/// (unless the window is 0: tap-only); with credentials the device awaits
/// a tap.
///
/// `window_used_ms` is how much of the window earlier boots already spent:
/// a board whose firmware restarts itself (a wizard's idle restart, a
/// watchdog) must not re-arm the window on every restart — "re-armed by a
/// power cycle" means a power cycle, so the glue carries the spent time
/// across software resets (RTC memory, cleared on power-on) and hands it
/// in here. A window already spent begins shut and spent. The stamp is
/// back-dated so session_tick's one rule still decides the expiry.
inline void session_begin(Session& s, bool no_credentials, uint32_t now_ms,
                          const Timing& t = Timing{}, uint32_t window_used_ms = 0) {
  s = Session{};
  s.no_credentials = no_credentials;
  s.state = State::AwaitingAuthorization;
  if (no_credentials && t.first_boot_window_ms != 0) {
    if (window_used_ms >= t.first_boot_window_ms) {
      s.window_spent = true;
      return;
    }
    s.window_at_ms = stamp(now_ms - window_used_ms);
    detail::open_no_credentials_door(s);
  }
}

/// How much of the first-boot window this session has spent so far — what
/// the glue stores across a software reset. 0 when the window is not
/// running; the whole window once it is spent.
inline uint32_t session_window_used_ms(const Session& s, uint32_t now_ms, const Timing& t) {
  if (s.window_spent) return t.first_boot_window_ms;
  if (s.door != Door::NoCredentials || s.window_at_ms == 0) return 0;
  const uint32_t used = (uint32_t)(now_ms - s.window_at_ms);
  return used > t.first_boot_window_ms ? t.first_boot_window_ms : used;
}

/// The glue's word on credentials, every pass. Credentials appearing (the
/// join persisted them) shuts the NoCredentials door; the state stays
/// Provisioned or Provisioning for the phone to read. Credentials vanishing
/// at runtime (a factory reset) re-arms the window from now — that is the
/// owner's own act. A recovery portal is NOT "no credentials": the caller
/// must pass the stored-credentials fact, never the portal's.
inline void session_set_no_credentials(Session& s, bool no_credentials, uint32_t now_ms,
                                       const Timing& t = Timing{}) {
  const bool wiped = no_credentials && !s.no_credentials;
  s.no_credentials = no_credentials;
  if (no_credentials) {
    if (wiped && t.first_boot_window_ms != 0) {
      s.window_at_ms = stamp(now_ms);
      s.window_spent = false;
    }
    if (s.door != Door::NoCredentials && s.door != Door::Tap &&
        !s.window_spent && s.window_at_ms != 0) {
      detail::open_no_credentials_door(s);
    }
    return;
  }
  if (s.door == Door::NoCredentials) {
    // A tap in flight keeps its own TTL; otherwise the door shuts unless a
    // join is being reported on.
    detail::shut_door(s);
  }
}

/// A physical tap: the door opens for the TTL (no effect while the
/// NoCredentials door is already open — that one has its own window).
inline void session_tap(Session& s, uint32_t now_ms) {
  if (s.door == Door::NoCredentials) return;
  s.door = Door::Tap;
  s.tap_at_ms = stamp(now_ms);
  s.error = Error::None;
  s.provisioned = false;
  s.last_wifi_settings_ms = 0;
  s.wifi_settings_count = 0;
  if (s.state != State::Provisioning) s.state = State::Authorized;
}

namespace detail {
/// Count one WIFI_SETTINGS attempt against the open door. Returns false
/// when the attempt is past the cap — the door is shut then.
inline bool count_attempt(Session& s, uint32_t now_ms, const Timing& t) {
  if (s.wifi_settings_count >= t.wifi_settings_cap) {
    if (s.door == Door::NoCredentials) s.window_spent = true;
    s.door = Door::Shut;
    if (s.state != State::Provisioning) s.state = State::AwaitingAuthorization;
    return false;
  }
  ++s.wifi_settings_count;
  s.last_wifi_settings_ms = stamp(now_ms);
  return true;
}
}  // namespace detail

/// Credentials arrived. Returns the error to publish (None means the join
/// is now in flight and the glue should start it).
inline Error session_on_wifi_settings(Session& s, uint32_t now_ms,
                                      const Timing& t = Timing{}) {
  if (!door_open(s) || s.state == State::AwaitingAuthorization) {
    s.error = Error::NotAuthorized;
    return s.error;
  }
  if (s.state == State::Provisioning) {
    // One join at a time; the standard has no word for "busy", and a second
    // write mid-join is a client that did not wait. Refuse without
    // disturbing the join in flight.
    s.error = Error::InvalidRpc;
    return s.error;
  }
  if (s.last_wifi_settings_ms != 0 &&
      (uint32_t)(now_ms - s.last_wifi_settings_ms) < t.wifi_settings_cooldown_ms) {
    // Too soon after the last attempt: refused, not counted, nothing started.
    s.error = Error::InvalidRpc;
    return s.error;
  }
  if (!detail::count_attempt(s, now_ms, t)) {
    s.error = Error::NotAuthorized;
    return s.error;
  }
  s.state = State::Provisioning;
  s.provisioning_at_ms = now_ms;
  s.error = Error::None;
  s.last_activity_ms = stamp(now_ms);
  return Error::None;
}

/// A WIFI_SETTINGS frame that did not parse (Malformed / TooLong) still
/// counts against the open door's cap — a flood of garbage is an attempt
/// too — and is answered InvalidRpc like any bad packet.
inline Error session_on_malformed_wifi_settings(Session& s, uint32_t now_ms,
                                                const Timing& t = Timing{}) {
  if (door_open(s) && s.state != State::Provisioning) detail::count_attempt(s, now_ms, t);
  s.error = Error::InvalidRpc;
  return s.error;
}

/// The join's verdict. Success → Provisioned; failure → back to Authorized
/// (the door is still open — the person can retry with the right password)
/// with UnableToConnect on the error characteristic.
inline void session_on_join_result(Session& s, bool joined, uint32_t now_ms) {
  if (s.state != State::Provisioning) return;
  if (joined) {
    s.state = State::Provisioned;
    s.provisioned = true;
    s.provisioned_at_ms = stamp(now_ms);
    s.error = Error::None;
  } else {
    s.state = door_open(s) ? State::Authorized : State::AwaitingAuthorization;
    s.error = Error::UnableToConnect;
  }
}

/// A packet that was not a command: the error to publish (state unchanged).
inline Error session_on_bad_packet(Session& s, Parse verdict) {
  s.error = error_for(verdict);
  return s.error;
}

/// A command this device does not speak.
inline Error session_on_unknown_command(Session& s) {
  s.error = Error::UnknownRpc;
  return s.error;
}

// ── The one link ───────────────────────────────────────────────────────────

/// A client connected / disconnected (the glue's server callbacks).
inline void session_link_up(Session& s, uint32_t now_ms) {
  s.link_up = true;
  s.last_activity_ms = stamp(now_ms);
}
inline void session_link_down(Session& s) { s.link_up = false; }

/// Any command the link sent (accepted or not) is activity.
inline void session_touch(Session& s, uint32_t now_ms) { s.last_activity_ms = stamp(now_ms); }

/// True when the glue should drop the connected client: it has been silent
/// for the idle bound, or the join is done and the linger has passed. Never
/// mid-join (the verdict is owed to it).
inline bool idle_disconnect_due(const Session& s, uint32_t now_ms, const Timing& t) {
  if (!s.link_up) return false;
  if (s.state == State::Provisioning) return false;
  if (s.state == State::Provisioned && s.provisioned_at_ms != 0) {
    return (uint32_t)(now_ms - s.provisioned_at_ms) >= t.provisioned_linger_ms;
  }
  return s.last_activity_ms != 0 &&
         (uint32_t)(now_ms - s.last_activity_ms) >= t.idle_disconnect_ms;
}

/// Time passes: the first-boot window and the tap TTL expire, and a join
/// nobody reported on fails. Returns true when the state changed (the glue
/// republishes).
inline bool session_tick(Session& s, uint32_t now_ms, const Timing& t) {
  bool changed = false;
  if (s.door == Door::NoCredentials && t.first_boot_window_ms != 0 && s.window_at_ms != 0 &&
      (uint32_t)(now_ms - s.window_at_ms) >= t.first_boot_window_ms) {
    s.window_spent = true;
    const State before = s.state;
    detail::shut_door(s);
    changed = s.state != before;
  }
  if (s.door == Door::Tap &&
      (uint32_t)(now_ms - s.tap_at_ms) >= t.tap_ttl_ms) {
    const State before = s.state;
    detail::shut_door(s);
    changed = changed || s.state != before;
  }
  if (s.state == State::Provisioning &&
      (uint32_t)(now_ms - s.provisioning_at_ms) >= t.provisioning_timeout_ms) {
    session_on_join_result(s, false, now_ms);
    changed = true;
  }
  return changed;
}

// ── What goes on air ───────────────────────────────────────────────────────

enum class Advert : uint8_t {
  Beacon,  // the fleet presence beacon is the primary advert (steady state)
  Improv,  // the Improv service UUID + service data are the primary advert
};

/// Improv takes the primary advert exactly while the door is open or a join
/// is being reported on; a provisioned device goes back to the beacon so
/// the fleet keeps hearing it, and so a stranger's scanner sees nothing to
/// provision. (While a client is connected the question is moot — a
/// connected peripheral does not advertise.)
inline Advert advert_for(const Session& s) {
  switch (s.state) {
    case State::Authorized:
    case State::Provisioning:
      return Advert::Improv;
    case State::AwaitingAuthorization:
      // A shut door is nothing to offer; the beacon says "I am here, I am
      // on Wi-Fi" instead. (With the door open this state is never held.)
      return Advert::Beacon;
    case State::Provisioned:
    case State::Stopped:
    default:
      return Advert::Beacon;
  }
}

}  // namespace improv
}  // namespace net
}  // namespace canary
