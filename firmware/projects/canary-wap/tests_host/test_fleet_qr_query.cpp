// GET /api/fleet/qr reads its query whole or refuses it (sweep F304).
//
// The Fleet sheet asks for a provisioning QR (SECURACV:S:<ssid>;P:<pass>;;)
// with the home network's name and password percent-encoded in the query
// (URLSearchParams). IDF's httpd_query_key_value() copies a value still
// encoded and answers ESP_ERR_HTTPD_RESULT_TRUNC with the cut copy when it
// does not fit. handle_fleet_qr() read the query into 256 bytes and each
// value into a buffer sized for its decoded form (33 and 65), and dropped the
// password's answer, so a password whose encoded form passed 64 bytes was
// drawn cut into the code, a name whose encoded form passed 32 bytes was
// answered "ssid is required", and a legal query past 255 bytes was answered
// "Missing query parameters".
//
// This runs the REAL read (fleet_qr_query.h) and the REAL handler, cut
// verbatim out of canary_wap.ino (firmware/tests_host/cut_functions.awk),
// with the real wap_json_writer.h (the home network's own bytes put back),
// over the host esp_http_server.h (stubs/receipt_grant: the query readers
// answer as IDF's do, a cut copy and ESP_ERR_HTTPD_RESULT_TRUNC when a value
// does not fit). The QR's drawing is a stand-in that keeps the payload it was
// handed. What it pins:
//   - a 22-symbol password, a 63-character passphrase, a 64-digit hex key, a
//     299-byte query (32 name bytes and 64 password bytes, every one as %XX)
//     and a name whose encoded form passes 32 bytes are drawn whole;
//   - every answer is a 400 with the documented text or a QR whose payload
//     is exactly the decoded name and password: over every password length
//     from 0 to 70 in four byte classes, and over the hand-made values a
//     reviewer would send (%00, a lone trailing '%', '+', '%3B', 65 bytes
//     decoded, a key with no '=');
//   - no 'pass' is an open network; no query, no name, an empty name, a name
//     past 32 bytes, a password past 64, a zero byte in either and a query
//     past the buffer each answer their own 400, naming the field;
//   - the home network as GET /api/wifi shows it (U+FFFD for each control
//     byte and each byte that is not UTF-8), whose text can pass 32 bytes
//     up to 96 though its own name does not, is drawn with its own bytes,
//     sent as the page sends it and every byte escaped beside the longest
//     password (a 491-byte query); the 32-byte rule runs after the
//     read-back (the F304 review), so any other name past 32 is refused.
// Host-tested; not run on a device.
//
// Build/run: make -C firmware/projects/canary-wap/tests_host run-fleet-qr-query

#include "arduino_globals.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_http_server.h"  // stubs/receipt_grant
#include "fleet_qr_query.h"
#include "wap_json_writer.h"  // wap_json::shows_as()

#ifndef FLEET_QR_HANDLER_INC
#error "FLEET_QR_HANDLER_INC must name the cut of handle_fleet_qr() (the Makefile passes it)"
#endif

namespace {
int g_checks = 0;
int g_failures = 0;
#define CHECK_MSG(c, msg)                                                 \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      ++g_failures;                                                       \
      std::printf("FAIL %s:%d: %s -- %s\n", __FILE__, __LINE__, #c,       \
                  std::string(msg).c_str());                              \
    }                                                                     \
  } while (0)
}  // namespace

// ── what the handler calls ──────────────────────────────────────────────────
struct HostHealth {
  uint32_t http_requests;
};
static HostHealth g_health;

struct WiFiCredentials {  // the sketch's, by its sizes
  char ssid[33];
  char password[65];
  bool enabled;
  bool configured;
};
static WiFiCredentials g_wifi_creds;

static std::string g_drawn;  // the payload send_qr_svg() was handed
static int g_draws = 0;
static esp_err_t send_qr_svg(httpd_req_t* req, const char* payload) {
  g_drawn = payload;
  ++g_draws;
  return httpd_resp_send(req, "<svg/>", -1);
}

#include FLEET_QR_HANDLER_INC

// ── fixtures ────────────────────────────────────────────────────────────────
// URLSearchParams' serializer (application/x-www-form-urlencoded): letters,
// digits and *-._ as they are, a space as '+', every other byte as %XX.
static std::string form_encode(const std::string& v) {
  static const char* hexd = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : v) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '*' ||
        c == '-' || c == '.' || c == '_') {
      out += (char)c;
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hexd[c >> 4];
      out += hexd[c & 15];
    }
  }
  return out;
}

// Every byte as %XX: the longest a value can be sent.
static std::string all_escaped(const std::string& v) {
  static const char* hexd = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : v) {
    out += '%';
    out += hexd[c >> 4];
    out += hexd[c & 15];
  }
  return out;
}

struct Answer {
  std::string status;
  std::string text;
  std::string payload;  // "" unless a QR was drawn
};

static Answer get(const std::string& query) {
  httpd_req_t req;
  req.query = query;
  g_drawn.clear();
  g_draws = 0;
  handle_fleet_qr(&req);
  Answer a;
  a.status = req.status;
  a.text = req.resp;
  a.payload = g_draws == 1 ? g_drawn : "";
  return a;
}

static std::string qr(const std::string& ssid, const std::string& pass) {
  return "SECURACV:S:" + ssid + ";P:" + pass + ";;";
}

static void drawn_whole(const std::string& what, const std::string& query, const std::string& ssid,
                        const std::string& pass) {
  const Answer a = get(query);
  CHECK_MSG(a.status == "200 OK" && a.payload == qr(ssid, pass),
            what + " (query " + std::to_string(query.size()) + " bytes): " + a.status + " \"" +
                a.text + "\", drawn \"" + a.payload + "\"");
}

static void refused(const std::string& what, const std::string& query, const std::string& text) {
  const Answer a = get(query);
  CHECK_MSG(a.status == "400 Bad Request" && a.text == text && a.payload.empty(),
            what + ": " + a.status + " \"" + a.text + "\", drawn \"" + a.payload +
                "\" (expected 400 \"" + text + "\")");
}

// ── the values the old handler cut or refused ───────────────────────────────
static void the_old_cuts_are_drawn_whole() {
  std::memset(&g_wifi_creds, 0, sizeof(g_wifi_creds));
  // 22 symbols, each %XX: 66 encoded bytes.
  const std::string symbols = "!\"#$%&'()+,/:<=>?@[]^`";
  CHECK_MSG(symbols.size() == 22 && form_encode(symbols).size() == 66, "the symbol fixture");
  drawn_whole("a 22-symbol password", "ssid=Home&pass=" + form_encode(symbols), "Home", symbols);

  // A 63-character passphrase with a few symbols in it.
  std::string phrase = "correct horse battery staple, said the parrot! (twice) #42";
  while (phrase.size() < 63) phrase += "x";
  CHECK_MSG(phrase.size() == 63 && form_encode(phrase).size() > 64, "the passphrase fixture");
  drawn_whole("a 63-character passphrase", "ssid=Home&pass=" + form_encode(phrase), "Home", phrase);

  // A WPA2 key as 64 hex digits.
  std::string psk;
  for (int i = 0; i < 64; i++) psk += "0123456789abcdef"[i % 16];
  drawn_whole("a 64-digit hex key", "ssid=Home&pass=" + psk, "Home", psk);

  // The longest legal query: 32 name bytes and 64 password bytes, every one
  // as %XX: "ssid=" + 96 + "&pass=" + 192 = 299 bytes.
  std::string name;
  while (name.size() < 32) name += "\xC3\xA9";  // "é", 16 of them
  const std::string pw(64, '!');
  const std::string longest = "ssid=" + all_escaped(name) + "&pass=" + all_escaped(pw);
  CHECK_MSG(longest.size() == 299, "the longest query is " + std::to_string(longest.size()));
  drawn_whole("the longest legal query", longest, name, pw);

  // A name whose encoded form passes 32 bytes.
  const std::string strasse = "Wohnzimmer Stra\xC3\x9F" "e \xC3\x9C 2,4";
  CHECK_MSG(strasse.size() <= 32 && form_encode(strasse).size() > 32, "the name fixture");
  drawn_whole("a name whose encoded form passes 32 bytes", "ssid=" + form_encode(strasse) + "&pass=pw",
              strasse, "pw");
}

// ── every answer is a documented 400 or a whole QR ──────────────────────────
static void every_length_is_whole_or_refused() {
  std::memset(&g_wifi_creds, 0, sizeof(g_wifi_creds));
  struct Class {
    const char* what;
    char c;
  };
  for (const Class& k : {Class{"letters", 'a'}, Class{"spaces", ' '}, Class{"symbols", '!'},
                         Class{"non-ASCII bytes", '\xE9'}}) {
    for (size_t n = 0; n <= 70; n++) {
      const std::string pass(n, k.c);
      const std::string what = std::string("a password of ") + std::to_string(n) + " " + k.what;
      for (const std::string& enc : {form_encode(pass), all_escaped(pass)}) {
        const std::string q = "ssid=Home&pass=" + enc;
        if (n <= 64) {
          drawn_whole(what, q, "Home", pass);
        } else {
          refused(what, q, "Password is too long");
        }
      }
    }
    // Past 32 bytes every name is refused as too long: by the handler up to
    // the 96 bytes the read takes (the longest shown form of a 32-byte
    // name), and by the read past them, sent plain or cut by the reader.
    for (size_t n = 1; n <= 100; n++) {
      const std::string ssid(n, k.c);
      const std::string what = std::string("a name of ") + std::to_string(n) + " " + k.what;
      for (const std::string& enc : {form_encode(ssid), all_escaped(ssid)}) {
        const std::string q = "ssid=" + enc + "&pass=pw";
        if (n <= 32) {
          drawn_whole(what, q, ssid, "pw");
        } else {
          refused(what, q, "Network name is too long");
        }
      }
    }
  }
}

static void hand_made_values() {
  std::memset(&g_wifi_creds, 0, sizeof(g_wifi_creds));
  // A zero byte would end the string the QR is drawn from: refused, named.
  refused("a password holding %00", "ssid=Home&pass=abc%00def", "Password can't contain a zero byte");
  refused("a name holding %00", "ssid=Ho%00me&pass=x", "Network name can't contain a zero byte");
  // A '%' not followed by two hex digits is itself, as the form decoding
  // reads it: drawn whole, the '%' kept.
  drawn_whole("a lone trailing '%'", "ssid=Home&pass=abc%", "Home", "abc%");
  drawn_whole("a '%' and one hex digit", "ssid=Home&pass=abc%4", "Home", "abc%4");
  drawn_whole("a '%' and a non-hex pair", "ssid=Home&pass=50%zz", "Home", "50%zz");
  // '+' is a space; "%2B" is a '+'.
  drawn_whole("a '+'", "ssid=My+Home&pass=a+b", "My Home", "a b");
  drawn_whole("an encoded '+'", "ssid=Home&pass=a%2Bb", "Home", "a+b");
  // '%3B' decodes to ';', which the payload cannot carry: the ';' rule.
  refused("a password holding %3B", "ssid=Home&pass=a%3Bb",
          "Network names/passwords containing ';' can't be QR-provisioned yet");
  refused("a name holding %3B", "ssid=Ho%3Bme&pass=x",
          "Network names/passwords containing ';' can't be QR-provisioned yet");
  // 65 bytes decoded, sent plain (fits the read, decodes too long) and
  // escaped (cut by the read).
  refused("a 65-byte password sent plain", "ssid=Home&pass=" + std::string(65, 'p'),
          "Password is too long");
  refused("a 65-byte password escaped", "ssid=Home&pass=" + all_escaped(std::string(65, 'p')),
          "Password is too long");
  // An open network: no 'pass' at all, or an empty one.
  drawn_whole("no pass (an open network)", "ssid=Cafe", "Cafe", "");
  drawn_whole("an empty pass", "ssid=Cafe&pass=", "Cafe", "");
  drawn_whole("pass first", "pass=pw&ssid=Cafe", "Cafe", "pw");
  // The name.
  refused("no query", "", "Missing query parameters");
  refused("no ssid", "pass=pw", "ssid is required");
  refused("an empty ssid", "ssid=&pass=pw", "ssid is required");
  refused("an ssid key with no '='", "ssid&pass=pw", "ssid is required");
  refused("a 33-byte name sent plain", "ssid=" + std::string(33, 'n'), "Network name is too long");
  // A query past the buffer is refused by its length, never read cut.
  refused("a query past the buffer", "ssid=Home&pass=pw&pad=" + std::string(500, 'z'),
          "Query is too long");
  drawn_whole("a query one byte under the buffer",
              "ssid=Home&pass=pw&pad=" + std::string(fleet_qr::kQueryCap - 1 - 22, 'z'), "Home", "pw");
}

// The Fleet sheet fills the name in from GET /api/wifi, which shows the home
// network's control bytes, and its bytes that are not UTF-8, as U+FFFD
// (F262): the code carries the network's own bytes, whole. The shown form of
// a name of 32 bytes or fewer can pass 32 bytes (up to 96, every byte three)
// and is still the home network: the read takes it and the 32-byte rule
// runs after the read-back (the F304 review; before, such a name was
// refused as too long, and on main as "ssid is required").
static std::string shown_as(const std::string& raw) {
  std::string out;
  for (unsigned char c : raw) {
    if (c < 0x20 || c >= 0x80) {
      out += "\xEF\xBF\xBD";  // a control byte, or a lone byte that is not UTF-8
    } else {
      out += (char)c;
    }
  }
  return out;
}

static void the_home_network_is_put_back() {
  std::memset(&g_wifi_creds, 0, sizeof(g_wifi_creds));
  std::strcpy(g_wifi_creds.ssid, "Home\x01Net");
  g_wifi_creds.configured = true;
  const std::string shown = "Home\xEF\xBF\xBDNet";  // U+FFFD for the control byte
  drawn_whole("the home network as GET /api/wifi showed it", "ssid=" + form_encode(shown) + "&pass=pw",
              "Home\x01Net", "pw");

  struct Home {
    const char* what;
    std::string raw;
  };
  const std::string pw64(64, '!');
  for (const Home& h : {Home{"30 letters and 2 control bytes", std::string(30, 'H') + "\x01\x02"},
                        Home{"28 letters and 4 control bytes", std::string(28, 'H') + "\x01\x02\x03\x04"},
                        Home{"26 letters and 3 control bytes", std::string(26, 'H') + "\x1f\x1e\x1d"},
                        Home{"32 control bytes", std::string(32, '\x01')},
                        Home{"32 bytes that are not UTF-8", std::string(32, '\xFF')}}) {
    std::memset(&g_wifi_creds, 0, sizeof(g_wifi_creds));
    std::memcpy(g_wifi_creds.ssid, h.raw.data(), h.raw.size());
    g_wifi_creds.configured = true;
    const std::string sh = shown_as(h.raw);
    CHECK_MSG(h.raw.size() <= 32 && sh.size() > 32, std::string(h.what) + ": the fixture");
    const std::string what = std::string("the home network (") + h.what + ", shown as " +
                             std::to_string(sh.size()) + " bytes)";
    drawn_whole(what + ", sent as the page sends it", "ssid=" + form_encode(sh) + "&pass=pw", h.raw, "pw");
    // Every byte escaped, with the longest password escaped too: up to
    // "ssid=" + 288 + "&pass=" + 192 = 491 bytes, the longest query the page
    // sends.
    drawn_whole(what + ", every byte escaped, the longest password",
                "ssid=" + all_escaped(sh) + "&pass=" + all_escaped(pw64), h.raw, pw64);
    // The same text with one U+FFFD written as a letter is not the home
    // network, and too long for any other.
    std::string other = sh;
    other.replace(other.find("\xEF\xBF\xBD"), 3, "X");
    if (other.size() > 32) {
      refused(what + " with one U+FFFD as a letter", "ssid=" + form_encode(other) + "&pass=pw",
              "Network name is too long");
    }
  }
  std::memset(&g_wifi_creds, 0, sizeof(g_wifi_creds));
}

// The read on its own: each answer's text, and *out zeroed on a refusal.
static void the_read_answers() {
  struct Case {
    fleet_qr::Read r;
    const char* text;
  };
  using R = fleet_qr::Read;
  for (const Case& c : {Case{R::NoQuery, "Missing query parameters"},
                        Case{R::QueryTooLong, "Query is too long"},
                        Case{R::QueryUnreadable, "Query could not be read"},
                        Case{R::NoSsid, "ssid is required"},
                        Case{R::SsidTooLong, "Network name is too long"},
                        Case{R::SsidUnreadable, "Network name could not be read"},
                        Case{R::SsidHasNul, "Network name can't contain a zero byte"},
                        Case{R::PassTooLong, "Password is too long"},
                        Case{R::PassUnreadable, "Password could not be read"},
                        Case{R::PassHasNul, "Password can't contain a zero byte"}}) {
    CHECK_MSG(std::string(fleet_qr::refusal(c.r)) == c.text,
              std::string("refusal(): ") + fleet_qr::refusal(c.r));
  }
  fleet_qr::Credentials cred;
  std::memset(&cred, 0x5a, sizeof(cred));
  CHECK_MSG(fleet_qr::read_query("ssid=Home&pass=a%00", &cred) == R::PassHasNul &&
                cred.ssid[0] == '\0' && cred.pass[0] == '\0',
            "a refused read left bytes in *out");
  char zero[] = "a%00b";
  CHECK_MSG(fleet_qr::url_decode_inplace(zero) == 3 && std::strlen(zero) == 1,
            "url_decode_inplace() does not count the zero byte");
}

int main() {
  the_old_cuts_are_drawn_whole();
  every_length_is_whole_or_refused();
  hand_made_values();
  the_home_network_is_put_back();
  the_read_answers();
  if (g_failures) {
    std::printf("test_fleet_qr_query: %d of %d checks FAILED\n", g_failures, g_checks);
    return 1;
  }
  std::printf("test_fleet_qr_query: ALL %d CHECKS PASSED (GET /api/fleet/qr: every value drawn "
              "whole or refused by name)\n",
              g_checks);
  return 0;
}
