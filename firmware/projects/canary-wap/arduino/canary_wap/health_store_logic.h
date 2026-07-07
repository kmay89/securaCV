/**
 * @file health_store_logic.h
 * @brief Pure logic for the /HEALTH per-boot SD log: JSON-escaped line
 *        format and the boot filename scheme.
 *
 * The health log's durable tier is one append-only file per boot,
 * /HEALTH/boot_<count>.jsonl — after a crash, the previous boot's file IS
 * the forensic record of what led up to it. Old boot files are bounded by
 * datamgmt's existing /HEALTH count rotation (health logs are regenerable
 * artifacts; Invariant IV protects only /WITNESS and /CHAIN).
 *
 * Threading contract (glue lives in canary_wap.ino): log_health() runs on
 * whatever task the caller is on (loop / httpd / workers), so it never
 * touches the SD bus — it stages a fully-formatted line into a small
 * PSRAM pending ring behind a critical section, and the LOOP TASK drains
 * the ring to SD (every SD writer in this firmware is loop-task-only).
 *
 * Message and detail strings can carry peer-controlled bytes (mesh sender
 * names travel into health details), so the line builder JSON-escapes
 * them: quote, backslash, and every control byte. Escapes are atomic —
 * truncation at the buffer cap never emits half an escape sequence, so a
 * truncated line is still valid JSON.
 *
 * Hosted C++ only (test_health_store_logic.cpp pins the format). Names
 * carry an HS_ prefix; short ALL-CAPS names collide with newlib macros
 * on-target (the LINE_MAX lesson).
 */

#ifndef HEALTH_STORE_LOGIC_H
#define HEALTH_STORE_LOGIC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace health_store {

/* message[80] + detail[48], worst case fully control chars = 6 bytes of
 * escape each (\u00XX) ≈ 768 + framing. 512 keeps the pending ring small;
 * the escaper truncates cleanly when a pathological line would exceed it. */
constexpr size_t HS_LINE_MAX = 512;

/* Pending-ring depth: bursts bigger than this drop the OLDEST staged
 * lines (the newest entry is the one that explains the burst). */
constexpr size_t HS_PENDING_SLOTS = 8;

/* "/HEALTH/boot_4294967295.jsonl" = 30 chars + NUL. */
constexpr size_t HS_PATH_MAX = 40;

/**
 * JSON-escape src into dst (NUL-terminated). Escapes `"` and `\` as
 * two-byte sequences and every byte < 0x20 as \u00XX. Writing stops
 * BEFORE an escape that would not fit whole, so output is always valid
 * inside a JSON string. Returns chars written (excluding NUL).
 */
inline size_t json_escape(char* dst, size_t dst_len, const char* src) {
  if (dst_len == 0) return 0;
  size_t w = 0;
  for (const char* p = src ? src : ""; *p != '\0'; p++) {
    const unsigned char c = (unsigned char)*p;
    if (c == '"' || c == '\\') {
      if (w + 2 >= dst_len) break;
      dst[w++] = '\\';
      dst[w++] = (char)c;
    } else if (c < 0x20) {
      if (w + 6 >= dst_len) break;
      static const char* H = "0123456789abcdef";
      dst[w++] = '\\';
      dst[w++] = 'u';
      dst[w++] = '0';
      dst[w++] = '0';
      dst[w++] = H[c >> 4];
      dst[w++] = H[c & 0x0F];
    } else {
      if (w + 1 >= dst_len) break;
      dst[w++] = (char)c;
    }
  }
  dst[w] = '\0';
  return w;
}

/**
 * Build one NDJSON health line. Returns the line length (including the
 * trailing newline) or 0 when even the escaped-and-truncated form cannot
 * fit (buf too small for the framing).
 */
inline size_t line_build(char* buf, size_t buf_len, uint32_t seq,
                         uint32_t uptime_ms, const char* level_name,
                         const char* category_name, const char* message,
                         const char* detail) {
  char msg_esc[2 * 80 + 8];
  char det_esc[2 * 48 + 8];
  json_escape(msg_esc, sizeof(msg_esc), message);
  json_escape(det_esc, sizeof(det_esc), detail);

  const int n = snprintf(buf, buf_len,
                         "{\"v\":1,\"seq\":%u,\"ms\":%u,\"lvl\":\"%s\","
                         "\"cat\":\"%s\",\"msg\":\"%s\",\"detail\":\"%s\"}\n",
                         (unsigned)seq, (unsigned)uptime_ms,
                         level_name ? level_name : "?",
                         category_name ? category_name : "?", msg_esc,
                         det_esc);
  if (n <= 0 || (size_t)n >= buf_len) return 0;
  return (size_t)n;
}

/* One file per boot: crash forensics = the previous boot's complete file.
 * datamgmt's /HEALTH count rotation bounds the collection. */
inline void boot_filename(uint32_t boot_count, char* out, size_t out_len) {
  snprintf(out, out_len, "/HEALTH/boot_%u.jsonl", (unsigned)boot_count);
}

}  // namespace health_store

#endif  // HEALTH_STORE_LOGIC_H
