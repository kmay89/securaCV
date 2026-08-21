// Onboarding pure helpers — dependency-free and host-testable.
//
// Everything here is string/byte math with no Arduino, no WiFi, no LVGL, so
// the host harness can pin the behaviors that would otherwise only fail on a
// stranger's phone at setup time: the WIFI: join-QR escaping (a home SSID can
// legally contain ; , : " and backslashes), the JSON escaping the scan list
// rides to the portal, and the unbiased alphabet rendering behind the
// per-session AP password. The I/O half lives in provision.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace canary::net {

// Render `n` random bytes as a password over the project's unambiguous
// alphabet (no 0/O/o, 1/I/i/l/L — same set as device_pseudonym, so every
// human-facing identifier obeys the same "safe to read off a screen" rule).
// Rejection-samples to stay unbiased; falls back deterministically on the
// (astronomically unlikely) shortfall so the output is always full length.
// Writes out_len-1 chars + NUL. Returns false on bad args.
inline bool render_password(const uint8_t* random_bytes, size_t n,
                            char* out, size_t out_len) {
  static const char kAlpha[] =
      "23456789ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz";
  constexpr size_t kAlphaLen = sizeof(kAlpha) - 1;          // 54
  constexpr unsigned kLimit = 216;                          // 54*4, unbiased
  if (random_bytes == nullptr || out == nullptr || out_len < 2) return false;
  const size_t want = out_len - 1;
  size_t produced = 0;
  for (size_t i = 0; i < n && produced < want; i++) {
    if (random_bytes[i] < kLimit) out[produced++] = kAlpha[random_bytes[i] % kAlphaLen];
  }
  while (produced < want) {                                 // deterministic top-up
    out[produced] = kAlpha[produced % kAlphaLen];
    produced++;
  }
  out[want] = '\0';
  return true;
}

// Escape one string field for the WIFI: QR payload (the format phones' camera
// apps parse to auto-join a network). Special characters \ ; , : " must be
// backslash-escaped per the de-facto spec — an SSID like `Bob's;Cafe` must
// not terminate the field early or the phone joins the wrong network.
// Returns chars written (excl. NUL), or 0 if it didn't fit (never truncates:
// a half-escaped credential is worse than none).
inline size_t wifi_qr_escape(const char* in, char* out, size_t cap) {
  if (in == nullptr || out == nullptr || cap == 0) return 0;
  size_t o = 0;
  for (size_t i = 0; in[i] != '\0'; i++) {
    const char c = in[i];
    const bool esc = (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"');
    if (o + (esc ? 2u : 1u) + 1 > cap) { out[0] = '\0'; return 0; }
    if (esc) out[o++] = '\\';
    out[o++] = c;
  }
  out[o] = '\0';
  return o;
}

// Build the complete join payload: WIFI:T:WPA;S:<ssid>;P:<pass>;;
// (T:nopass + no P: field when the password is empty — open network.)
// Returns length, or 0 if it didn't fit.
inline size_t wifi_qr_payload(const char* ssid, const char* pass,
                              char* out, size_t cap) {
  if (ssid == nullptr || out == nullptr || cap == 0) return 0;
  char essid[96];
  char epass[144];
  if (wifi_qr_escape(ssid, essid, sizeof(essid)) == 0 && ssid[0] != '\0') return 0;
  const bool open_net = (pass == nullptr || pass[0] == '\0');
  if (!open_net && wifi_qr_escape(pass, epass, sizeof(epass)) == 0) return 0;

  size_t o = 0;
  auto put = [&](const char* s) -> bool {
    for (size_t i = 0; s[i] != '\0'; i++) {
      if (o + 2 > cap) return false;
      out[o++] = s[i];
    }
    return true;
  };
  const bool ok = open_net
      ? (put("WIFI:T:nopass;S:") && put(essid) && put(";;"))
      : (put("WIFI:T:WPA;S:") && put(essid) && put(";P:") && put(epass) && put(";;"));
  if (!ok) { out[0] = '\0'; return 0; }
  out[o] = '\0';
  return o;
}

// Escape a string into a JSON string literal body (no surrounding quotes).
// Handles " \ and control characters; the scan list travels to the portal as
// JSON and the page inserts SSIDs with textContent, so this is the single
// point where a hostile SSID (they broadcast; anyone can name one) must be
// neutralized. Returns chars written, or 0 if it didn't fit.
inline size_t json_escape(const char* in, char* out, size_t cap) {
  static const char kHex[] = "0123456789abcdef";
  if (in == nullptr || out == nullptr || cap == 0) return 0;
  size_t o = 0;
  for (size_t i = 0; in[i] != '\0'; i++) {
    const unsigned char c = (unsigned char)in[i];
    size_t need;
    if (c == '"' || c == '\\') need = 2;
    else if (c < 0x20) need = 6;                 // \u00XX
    else need = 1;
    if (o + need + 1 > cap) { out[0] = '\0'; return 0; }
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
    else if (c < 0x20) {
      out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
      out[o++] = kHex[(c >> 4) & 0xF]; out[o++] = kHex[c & 0xF];
    } else out[o++] = (char)c;
  }
  out[o] = '\0';
  return o;
}

// ── Captive DNS response builder ──────────────────────────────────────────
// Faithful port of the policy proven in canary-wap's captive_dns.h (see
// LESSONS_LEARNED §captive-portal): answer the redirect A-record ONLY for
// QTYPE=A; every other type (AAAA=28, HTTPS/SVCB=65) gets NOERROR with zero
// answers (NODATA). Android Chrome fires parallel AAAA/HTTPS lookups — a
// naive responder (like the stock DNSServer) answers those with an A record,
// which is malformed, and the client stalls instead of falling back to IPv4.
//
// Builds the response in `out` from the raw query. Returns response length,
// or 0 to drop the packet (malformed / truncated / already a response).
inline size_t dns_build_response(const uint8_t* query, size_t qlen,
                                 const uint8_t ip[4],
                                 uint8_t* out, size_t out_cap) {
  if (query == nullptr || ip == nullptr || out == nullptr) return 0;
  if (qlen < 12 || qlen > 512 || out_cap < qlen + 16) return 0;
  if (query[2] & 0x80) return 0;                    // QR=1: a response — drop
  const unsigned qdcount = ((unsigned)query[4] << 8) | query[5];
  if (qdcount == 0) return 0;

  // Walk the first QNAME (labels are never compressed in queries) to find
  // QTYPE. Bounds-check every step — this is unauthenticated LAN input.
  size_t q = 12;
  while (q < qlen && query[q] != 0) {
    const size_t label = query[q];
    if (label > 63 || q + 1 + label >= qlen) return 0;
    q += 1 + label;
  }
  if (q + 4 >= qlen) return 0;                      // truncated QTYPE/QCLASS
  const unsigned qtype = ((unsigned)query[q + 1] << 8) | query[q + 2];
  const size_t question_end = q + 5;                // past NUL + QTYPE + QCLASS

  // Header + question echoed back; QR=1, AA=1, RD preserved, RA=0, RCODE=0.
  for (size_t i = 0; i < question_end; i++) out[i] = query[i];
  out[2] = (uint8_t)(0x84 | (query[2] & 0x01));
  out[3] = 0x00;
  out[4] = 0x00; out[5] = 0x01;                     // QDCOUNT=1 (answer 1st only)
  out[6] = 0x00;                                    // ANCOUNT high
  out[8] = 0x00; out[9] = 0x00;                     // NSCOUNT
  out[10] = 0x00; out[11] = 0x00;                   // ARCOUNT

  if (qtype != 0x0001) {                            // not A: NODATA
    out[7] = 0x00;
    return question_end;
  }
  out[7] = 0x01;                                    // ANCOUNT=1
  size_t o = question_end;
  const uint8_t answer[] = {0xC0, 0x0C,             // name ptr -> question
                            0x00, 0x01, 0x00, 0x01, // TYPE A, CLASS IN
                            0x00, 0x00, 0x00, 0x3C, // TTL 60 s
                            0x00, 0x04};            // RDLENGTH 4
  for (uint8_t b : answer) out[o++] = b;
  out[o++] = ip[0]; out[o++] = ip[1]; out[o++] = ip[2]; out[o++] = ip[3];
  return o;
}

// ── Per-OS captive-probe policy ───────────────────────────────────────────
// Same hybrid strategy the WAP wizard proved out: Apple probes get the
// portal page (deliberately NOT the "Success" token, so the Captive Network
// Assistant sheet pops and stays up); Android gets a real 204 (validates the
// AP, no cellular fallback); Windows gets the exact NCSI bodies.
enum class Probe : uint8_t { None, ApplePortal, Android204, WindowsNcsi, WindowsConnect };

// Match on the path component only (cache-busting queries still route).
inline bool path_is(const char* uri, const char* path) {
  if (uri == nullptr || path == nullptr) return false;
  size_t i = 0;
  for (; path[i] != '\0'; i++) {
    if (uri[i] != path[i]) return false;
  }
  return uri[i] == '\0' || uri[i] == '?';
}

inline Probe classify_probe(const char* uri) {
  if (path_is(uri, "/hotspot-detect.html") ||
      path_is(uri, "/library/test/success.html")) return Probe::ApplePortal;
  if (path_is(uri, "/generate_204") || path_is(uri, "/gen_204")) return Probe::Android204;
  if (path_is(uri, "/ncsi.txt")) return Probe::WindowsNcsi;
  if (path_is(uri, "/connecttest.txt")) return Probe::WindowsConnect;
  return Probe::None;
}

}  // namespace canary::net
