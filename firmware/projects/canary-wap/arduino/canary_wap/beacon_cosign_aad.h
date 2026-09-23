/*
 * SecuraCV Canary — Beacon COSIGN envelope associated data (host-testable)
 *
 * Arduino-free: beacon_wire.h + string.h only. COSIGN_REQ and COSIGN_RESP
 * bodies are ChaCha20-Poly1305 under a key derived from X25519 between the
 * two devices (spec/beacon_channel_v0.md §6.3, beacon_channel.cpp
 * cosign_encrypt/cosign_decrypt). The ciphertext has always been
 * authenticated; the routing fields that travel beside it in clear were not.
 * A flipped `accept` byte on a COSIGN_RESP turned a valid cosign into a
 * discarded one (a jam-equivalent denial — the other direction already
 * failed signature verification), and the COSIGN_REQ's fingerprints and
 * ciphertext length sat outside the tag.
 *
 * These builders produce the exact bytes both ends bind as associated data,
 * so the Poly1305 tag now covers them. Sender and receiver call the same
 * function, so they cannot disagree about the layout, and the leading
 * msg_type byte keeps a REQ's associated data from ever equaling a RESP's.
 * Multi-byte fields are serialized explicitly little-endian.
 *
 * Also here: the all-zero shared-secret check. X25519 against a low-order
 * peer key yields an all-zero secret — a session key an attacker knows.
 * ecdh_session_key refuses it explicitly rather than rely on what the
 * Curve25519 library's eval() promises. And secure_zero, which wipes the
 * shared secret and the session key on every exit path of ecdh_session_key,
 * cosign_encrypt and cosign_decrypt: a plain memset of a buffer that is about
 * to go out of scope is a dead store the optimizer may delete (it does, at
 * -O2 and -Os).
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_BEACON_COSIGN_AAD_H
#define SECURACV_BEACON_COSIGN_AAD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "beacon_wire.h"

namespace beacon_cosign_aad {

// msg_type || originator_fp || candidate_cosigner_fp || ciphertext_len (LE16)
static const size_t REQ_AAD_LEN = 1 + 2 * beacon_channel::DEVICE_FP_SIZE + 2;
// msg_type || originator_fp || cosigner_fp || accept
static const size_t RESP_AAD_LEN = 1 + 2 * beacon_channel::DEVICE_FP_SIZE + 1;

inline void cosign_req_aad(uint8_t out[REQ_AAD_LEN],
                           const uint8_t originator_fp[beacon_channel::DEVICE_FP_SIZE],
                           const uint8_t candidate_cosigner_fp[beacon_channel::DEVICE_FP_SIZE],
                           uint16_t ciphertext_len) {
  size_t i = 0;
  out[i++] = beacon_channel::BEACON_MSG_COSIGN_REQ;
  memcpy(out + i, originator_fp, beacon_channel::DEVICE_FP_SIZE);
  i += beacon_channel::DEVICE_FP_SIZE;
  memcpy(out + i, candidate_cosigner_fp, beacon_channel::DEVICE_FP_SIZE);
  i += beacon_channel::DEVICE_FP_SIZE;
  out[i++] = (uint8_t)(ciphertext_len & 0xFF);
  out[i++] = (uint8_t)(ciphertext_len >> 8);
}

inline void cosign_resp_aad(uint8_t out[RESP_AAD_LEN],
                            const uint8_t originator_fp[beacon_channel::DEVICE_FP_SIZE],
                            const uint8_t cosigner_fp[beacon_channel::DEVICE_FP_SIZE],
                            uint8_t accept) {
  size_t i = 0;
  out[i++] = beacon_channel::BEACON_MSG_COSIGN_RESP;
  memcpy(out + i, originator_fp, beacon_channel::DEVICE_FP_SIZE);
  i += beacon_channel::DEVICE_FP_SIZE;
  memcpy(out + i, cosigner_fp, beacon_channel::DEVICE_FP_SIZE);
  i += beacon_channel::DEVICE_FP_SIZE;
  out[i++] = accept;
}

// True when every byte of the 32-byte X25519 shared secret is zero. Reads
// every byte whatever it finds (no early exit), so the time taken does not
// depend on the secret.
inline bool shared_secret_is_zero(const uint8_t shared[32]) {
  uint8_t acc = 0;
  for (size_t i = 0; i < 32; i++) acc |= shared[i];
  return acc == 0;
}

// Zero `n` bytes at `p` in a way the optimizer keeps. Each store goes through
// a volatile pointer, and the empty asm with a "memory" clobber stops the
// compiler from assuming the bytes are dead afterward. Same technique as
// device_pseudonym.h's secure_zero and rf_presence.cpp's secure_wipe.
inline void secure_zero(void* p, size_t n) {
  volatile uint8_t* vp = static_cast<volatile uint8_t*>(p);
  while (n--) *vp++ = 0;
  __asm__ __volatile__("" ::: "memory");
}

}  // namespace beacon_cosign_aad

#endif  // SECURACV_BEACON_COSIGN_AAD_H
