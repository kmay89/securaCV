// fleet_qr_query.h — GET /api/fleet/qr's query, read whole or refused (sweep F304).
//
// The Fleet sheet asks for a provisioning QR (SECURACV:S:<ssid>;P:<pass>;;)
// with the home network's name and password in the query string, percent-
// encoded by the page (URLSearchParams). IDF's httpd_query_key_value() copies
// a value still encoded, and answers ESP_ERR_HTTPD_RESULT_TRUNC with the cut
// copy when it does not fit. The handler used to size its buffers for the
// decoded forms (33 and 65 bytes) and read the query into 256, so:
//   - a password whose encoded form passed 64 bytes (22 symbols, each %XX,
//     or a 63-character passphrase with a few) was drawn cut into the code:
//     the 'pass' answer was never read, and the scanning Canary was handed a
//     password the network does not take;
//   - a name whose encoded form passed 32 bytes was answered "ssid is
//     required";
//   - a legal query past 255 bytes ("ssid=" + 96 + "&pass=" + 192 is 299)
//     was answered "Missing query parameters".
// Now the query and both values are read into buffers sized for the encoded
// forms, every answer is checked, and a value is refused, naming its field,
// when it is cut, when it decodes past what it may hold, or when it decodes
// to a zero byte (%00), which would end the C string the QR is drawn from.
// A password may hold 64 bytes (a 63-character passphrase or a 64-digit hex
// key). A name is read at up to 96 bytes, the longest text the Fleet sheet
// can be shown a 32-byte name as (GET /api/wifi shows each control byte, and
// each byte that is not UTF-8, as U+FFFD's three, F262), so the handler can
// put the home network's own bytes back first and then hold the name to 32
// bytes (name_fits()). No 'pass' is an open network (""). Every refusal is a
// 400 whose text is refusal()'s; the handler then runs its own rules (the
// home network's bytes put back, the 32-byte name, the ';' refusal) on what
// this read.
//
// Pure apart from the three esp_http_server.h readers it calls, so
// tests_host/test_fleet_qr_query.cpp runs it over the host stand-in
// (tests_host/stubs/receipt_grant/esp_http_server.h). Host-tested; not run on
// a device.
#ifndef CANARY_WAP_FLEET_QR_QUERY_H
#define CANARY_WAP_FLEET_QR_QUERY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_http_server.h"

namespace fleet_qr {

constexpr size_t kSsidMax = 32;   // bytes: 802.11's longest network name
constexpr size_t kPassMax = 64;   // bytes: a 63-character passphrase or a 64-digit hex key
// The longest text a page shows a kSsidMax-byte name as: every byte
// U+FFFD's three (wap_json_writer.h's shows_as()). Read at that length so
// the home network's shown form reaches the handler's read-back.
constexpr size_t kSsidShownMax = 3 * kSsidMax;
// Each value as the page sends it at its longest, every byte as "%XX", and
// a NUL: 289 and 193 bytes.
constexpr size_t kSsidEncCap = 3 * kSsidShownMax + 1;
constexpr size_t kPassEncCap = 3 * kPassMax + 1;
// The query buffer. IDF refuses a request line whose URI passes
// CONFIG_HTTPD_MAX_URI_LEN (512 by default) before any handler runs, so no
// query that reaches the handler is longer; the longest the page sends is
// 491 bytes ("ssid=" + 288 + "&pass=" + 192; 505 with "/api/fleet/qr?",
// inside IDF's 512). One that does not fit is refused by length, never read
// cut.
constexpr size_t kQueryCap = 512;
static_assert(5 + (kSsidEncCap - 1) + 6 + (kPassEncCap - 1) < kQueryCap,
              "the longest query the page sends must fit the query buffer");

// ssid holds what the page sent, up to kSsidShownMax bytes, until the
// handler's read-back and name_fits() have run.
struct Credentials {
  char ssid[kSsidShownMax + 1];
  char pass[kPassMax + 1];
};

enum class Read : uint8_t {
  Ok,
  NoQuery,          // no query string at all
  QueryTooLong,     // longer than kQueryCap - 1
  QueryUnreadable,  // the reader answered anything but ESP_OK
  NoSsid,           // no 'ssid', or an empty one
  SsidTooLong,      // cut by the reader, more than kSsidShownMax bytes decoded, or
                    // more than kSsidMax once read back (name_fits())
  SsidUnreadable,   // the reader answered anything else
  SsidHasNul,       // decodes to a zero byte
  PassTooLong,      // cut by the reader, or more than kPassMax bytes decoded
  PassUnreadable,   // the reader answered anything else
  PassHasNul,       // decodes to a zero byte
};

// The text the route sends with each refusal's 400.
inline const char* refusal(Read r) {
  switch (r) {
    case Read::Ok: return "";
    case Read::NoQuery: return "Missing query parameters";
    case Read::QueryTooLong: return "Query is too long";
    case Read::QueryUnreadable: return "Query could not be read";
    case Read::NoSsid: return "ssid is required";
    case Read::SsidTooLong: return "Network name is too long";
    case Read::SsidUnreadable: return "Network name could not be read";
    case Read::SsidHasNul: return "Network name can't contain a zero byte";
    case Read::PassTooLong: return "Password is too long";
    case Read::PassUnreadable: return "Password could not be read";
    case Read::PassHasNul: return "Password can't contain a zero byte";
  }
  return "Bad request";
}

// application/x-www-form-urlencoded decoding, in place: '+' is a space,
// "%XX" is that byte, and a '%' not followed by two hex digits is itself.
// Answers the decoded length, which counts any zero byte a "%00" wrote
// (strlen() stops at it). Safe in place: the decoded form is never longer.
inline size_t url_decode_inplace(char* s) {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  char* w = s;
  for (const char* r = s; *r;) {
    if (*r == '%' && hex(r[1]) >= 0 && hex(r[2]) >= 0) {
      *w++ = (char)((hex(r[1]) << 4) | hex(r[2]));
      r += 3;
    } else if (*r == '+') {
      *w++ = ' ';
      r++;
    } else {
      *w++ = *r++;
    }
  }
  *w = '\0';
  return (size_t)(w - s);
}

// One value: read encoded into `enc` (cap bytes), then decoded and held to
// `max` bytes. NOT_FOUND is passed back for the caller to judge.
enum class Value : uint8_t { Ok, Missing, TooLong, Unreadable, HasNul };

inline Value read_value(const char* qs, const char* key, char* enc, size_t cap, size_t max,
                        size_t* len) {
  *len = 0;
  enc[0] = '\0';
  const esp_err_t e = httpd_query_key_value(qs, key, enc, cap);
  if (e == ESP_ERR_NOT_FOUND) {
    enc[0] = '\0';
    return Value::Missing;
  }
  if (e == ESP_ERR_HTTPD_RESULT_TRUNC) return Value::TooLong;
  if (e != ESP_OK) return Value::Unreadable;
  const size_t n = url_decode_inplace(enc);
  if (n > max) return Value::TooLong;
  if (strlen(enc) != n) return Value::HasNul;
  *len = n;
  return Value::Ok;
}

// The query's two values into *out, whole, or a refusal. *out is all zeros
// unless the answer is Ok.
inline Read read_query(const char* qs, Credentials* out) {
  memset(out, 0, sizeof(*out));
  char ssid_enc[kSsidEncCap];
  char pass_enc[kPassEncCap];
  size_t ssid_n = 0;
  size_t pass_n = 0;
  switch (read_value(qs, "ssid", ssid_enc, sizeof(ssid_enc), kSsidShownMax, &ssid_n)) {
    case Value::Ok: break;
    case Value::Missing: return Read::NoSsid;
    case Value::TooLong: return Read::SsidTooLong;
    case Value::Unreadable: return Read::SsidUnreadable;
    case Value::HasNul: return Read::SsidHasNul;
  }
  if (ssid_n == 0) return Read::NoSsid;
  switch (read_value(qs, "pass", pass_enc, sizeof(pass_enc), kPassMax, &pass_n)) {
    case Value::Ok: break;
    case Value::Missing: break;  // an open network: no password
    case Value::TooLong: return Read::PassTooLong;
    case Value::Unreadable: return Read::PassUnreadable;
    case Value::HasNul: return Read::PassHasNul;
  }
  memcpy(out->ssid, ssid_enc, ssid_n + 1);
  memcpy(out->pass, pass_enc, pass_n + 1);
  return Read::Ok;
}

// The request's query, read whole, then its two values (read_query()).
inline Read read_request(httpd_req_t* req, Credentials* out) {
  memset(out, 0, sizeof(*out));
  const size_t len = httpd_req_get_url_query_len(req);
  if (len == 0) return Read::NoQuery;
  if (len >= kQueryCap) return Read::QueryTooLong;
  char qs[kQueryCap];
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) return Read::QueryUnreadable;
  return read_query(qs, out);
}

// After the handler's read-back: the name the code will carry is at most
// kSsidMax bytes. A posted text past that is not the home network's shown
// form, and no network has such a name.
inline bool name_fits(const Credentials& c) {
  return strlen(c.ssid) <= kSsidMax;
}

}  // namespace fleet_qr

#endif  // CANARY_WAP_FLEET_QR_QUERY_H
