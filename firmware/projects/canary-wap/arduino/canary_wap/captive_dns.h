/*
 * SecuraCV Canary WAP — Captive-portal DNS response builder
 *
 * Pure, Arduino-free packet logic split out of setup_wizard.h so it can be
 * unit-tested on the host (see tests_host/test_captive_dns.cpp). The firmware
 * glue (WiFiUDP read/write, WiFi.softAPIP()) stays in setup_wizard.h and calls
 * build_response() here.
 *
 * The redirector points every A-record lookup at the AP IP so canary.local
 * (and any typed hostname) lands on the device, and returns NODATA (NOERROR,
 * zero answers) for every other QTYPE — AAAA (28), HTTPS/SVCB (65), etc. —
 * so clients fall straight back to their IPv4 lookup instead of stalling on a
 * malformed answer. See firmware/LESSONS_LEARNED.md, "Networking & Captive
 * Portal".
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_CAPTIVE_DNS_H
#define SECURACV_CAPTIVE_DNS_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace captive_dns {

static constexpr uint16_t QTYPE_A = 0x0001;   // IPv4 address record

// Build a captive-portal DNS response for `query` (length `qlen`).
//
// A-record (type 1) questions are answered with `ip` (4 bytes, the AP
// address); every other QTYPE gets NOERROR with ANCOUNT=0 (NODATA). Writes the
// response into `out` (capacity `out_cap`) and returns its length, or 0 if the
// query should be dropped: shorter than a DNS header, already a response
// (QR=1), no question (QDCOUNT<1), or no room in `out`.
inline size_t build_response(const uint8_t* query, size_t qlen,
                             const uint8_t ip[4],
                             uint8_t* out, size_t out_cap) {
  if (qlen < 12) return 0;                       // smaller than a DNS header
  if (query[2] & 0x80) return 0;                 // QR=1 → already a response
  uint16_t qdcount = ((uint16_t)query[4] << 8) | query[5];
  if (qdcount < 1) return 0;                     // nothing to answer
  if (qlen + 16 > out_cap) return 0;             // no room for header+answer

  // Walk the first QNAME to its QTYPE. Query QNAMEs aren't compressed, so each
  // label is [len][bytes], terminated by a zero-length label. The loop only
  // dereferences query[q] while q < qlen, and a label whose length overshoots
  // the packet simply leaves q >= qlen — caught by the bounds check below, so
  // there's no out-of-bounds read regardless of the label bytes.
  size_t q = 12;
  while (q < qlen && query[q] != 0x00) {
    q += (size_t)query[q] + 1;
  }
  // Require the 0x00 terminator plus a full QTYPE(2)+QCLASS(2) within bounds.
  // A truncated or malformed question (overshooting label, or a packet that
  // ends before QTYPE/QCLASS) is dropped rather than half-parsed.
  if (q + 4 >= qlen) return 0;
  uint16_t qtype = ((uint16_t)query[q + 1] << 8) | query[q + 2];

  memcpy(out, query, qlen);
  out[2] = 0x84 | (query[2] & 0x01);             // QR=1, AA=1, preserve RD
  out[3] = 0x00;                                 // RA=0, RCODE=0 (NOERROR)
  out[6] = 0x00;                                 // ANCOUNT high byte
  out[8] = 0x00; out[9]  = 0x00;                 // NSCOUNT
  out[10] = 0x00; out[11] = 0x00;                // ARCOUNT

  size_t pos = qlen;
  if (qtype == QTYPE_A) {
    out[7] = 0x01;                               // ANCOUNT = 1
    out[pos++] = 0xC0; out[pos++] = 0x0C;        // NAME -> offset 12 (pointer)
    out[pos++] = 0x00; out[pos++] = 0x01;        // TYPE A
    out[pos++] = 0x00; out[pos++] = 0x01;        // CLASS IN
    out[pos++] = 0x00; out[pos++] = 0x00;
    out[pos++] = 0x00; out[pos++] = 0x3C;        // TTL 60s
    out[pos++] = 0x00; out[pos++] = 0x04;        // RDLENGTH 4
    out[pos++] = ip[0]; out[pos++] = ip[1];
    out[pos++] = ip[2]; out[pos++] = ip[3];
  } else {
    out[7] = 0x00;                               // ANCOUNT = 0 (NODATA)
  }
  return pos;
}

}  // namespace captive_dns

#endif /* SECURACV_CAPTIVE_DNS_H */
