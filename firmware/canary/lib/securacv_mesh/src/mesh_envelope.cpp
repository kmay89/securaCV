/*
 * SecuraCV Canary — Mesh authenticated envelope — Implementation
 *
 * Pure functions; no globals; no I/O. Encodes/decodes the wire format,
 * delegates Ed25519 sign/verify to mesh_crypto.
 */

#include "mesh_envelope.h"

#include <string.h>

namespace mesh_envelope {

/* ──────────────────────────────────────────────────────────────────────────
 * INTERNAL LE I/O
 * ────────────────────────────────────────────────────────────────────────── */

namespace {

inline void write_u64_le(uint8_t* dst, uint64_t v) {
  for (int i = 0; i < 8; ++i) dst[i] = (uint8_t)((v >> (i * 8)) & 0xFF);
}

inline uint64_t read_u64_le(const uint8_t* src) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= ((uint64_t)src[i]) << (i * 8);
  return v;
}

inline void write_u32_le(uint8_t* dst, uint32_t v) {
  for (int i = 0; i < 4; ++i) dst[i] = (uint8_t)((v >> (i * 8)) & 0xFF);
}

inline uint32_t read_u32_le(const uint8_t* src) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= ((uint32_t)src[i]) << (i * 8);
  return v;
}

/* Encode header into the first HEADER_LEN bytes of dst. Forces
 * version=PROTOCOL_VERSION regardless of header.version. */
inline void encode_header(uint8_t* dst, const Header& h) {
  size_t off = 0;
  dst[off++] = PROTOCOL_VERSION;
  dst[off++] = h.msg_type;
  memcpy(dst + off, h.opera_id,  OPERA_ID_LEN);     off += OPERA_ID_LEN;
  memcpy(dst + off, h.sender_fp, FINGERPRINT_LEN);  off += FINGERPRINT_LEN;
  write_u64_le(dst + off, h.counter);                off += COUNTER_LEN;
  write_u32_le(dst + off, h.timestamp);              off += TIMESTAMP_LEN;
  (void)off;
}

/* Decode the first HEADER_LEN bytes of src into *out. Returns false if
 * the version byte is not PROTOCOL_VERSION. */
inline bool decode_header(const uint8_t* src, Header* out) {
  size_t off = 0;
  out->version = src[off++];
  if (out->version != PROTOCOL_VERSION) return false;
  out->msg_type = src[off++];
  memcpy(out->opera_id,  src + off, OPERA_ID_LEN);    off += OPERA_ID_LEN;
  memcpy(out->sender_fp, src + off, FINGERPRINT_LEN); off += FINGERPRINT_LEN;
  out->counter   = read_u64_le(src + off);            off += COUNTER_LEN;
  out->timestamp = read_u32_le(src + off);            off += TIMESTAMP_LEN;
  (void)off;
  return true;
}

}  /* namespace */

/* ──────────────────────────────────────────────────────────────────────────
 * SERIALIZE
 * ────────────────────────────────────────────────────────────────────────── */

size_t serialize_signed(const Header&  header,
                        const uint8_t* payload, size_t payload_len,
                        const uint8_t  signer_privkey[mesh_crypto::PRIVKEY_LEN],
                        const uint8_t  signer_pubkey [mesh_crypto::PUBKEY_LEN],
                        uint8_t* out_buf, size_t out_buf_cap) {
  if (out_buf == nullptr) return 0;
  if (signer_privkey == nullptr || signer_pubkey == nullptr) return 0;
  if (payload_len > MAX_PAYLOAD_LEN) return 0;
  if (payload == nullptr && payload_len > 0) return 0;

  const size_t total = HEADER_LEN + payload_len + SIGNATURE_LEN;
  if (out_buf_cap < total) return 0;

  /* Layout: [header || payload || signature]. */
  encode_header(out_buf, header);
  if (payload_len > 0) {
    memcpy(out_buf + HEADER_LEN, payload, payload_len);
  }

  /* Sign over (header || payload) — NOT the signature slot.
   * mesh_crypto::ed25519_sign already applies domain-separated hashing
   * (DOMAIN_MESSAGE) before the Ed25519 primitive. */
  const bool sig_ok = mesh_crypto::ed25519_sign(
      signer_privkey, signer_pubkey,
      out_buf, HEADER_LEN + payload_len,
      out_buf + HEADER_LEN + payload_len);
  if (!sig_ok) return 0;

  return total;
}

/* ──────────────────────────────────────────────────────────────────────────
 * PARSE + VERIFY
 * ────────────────────────────────────────────────────────────────────────── */

bool parse_and_verify(const uint8_t* frame, size_t frame_len,
                      const uint8_t  peer_pubkey[mesh_crypto::PUBKEY_LEN],
                      Header*        out_header,
                      const uint8_t** out_payload, size_t* out_payload_len) {
  if (frame == nullptr || peer_pubkey == nullptr) return false;
  if (out_header == nullptr || out_payload == nullptr || out_payload_len == nullptr) {
    return false;
  }
  if (frame_len < MIN_FRAME_LEN) return false;
  if (frame_len > MAX_FRAME_LEN) return false;

  /* Decode header (also validates version). */
  if (!decode_header(frame, out_header)) return false;

  const size_t payload_len = frame_len - HEADER_LEN - SIGNATURE_LEN;
  const uint8_t* signature = frame + HEADER_LEN + payload_len;

  /* Verify the signature over (header || payload). Must succeed before
   * we expose the payload to the caller — a tampered frame should
   * never reach an upper-layer handler. */
  if (!mesh_crypto::ed25519_verify(peer_pubkey,
                                   frame, HEADER_LEN + payload_len,
                                   signature)) {
    return false;
  }

  *out_payload     = (payload_len > 0) ? (frame + HEADER_LEN) : nullptr;
  *out_payload_len = payload_len;
  return true;
}

}  /* namespace mesh_envelope */
