/*
 * SecuraCV Canary — Mesh pairing — Implementation (PR 2d slice)
 *
 * Only the deterministic, side-effect-free pieces. PR 2e wires these
 * into a full state machine + integration with mesh_transport.
 */

#include "mesh_pairing.h"

namespace mesh_pairing {

uint32_t compute_confirmation_code(const uint8_t session_key[SESSION_KEY_LEN]) {
  if (session_key == nullptr) return 0;

  uint8_t code_hash[mesh_crypto::SHA256_OUT_LEN];
  mesh_crypto::sha256_domain(DOMAIN_PAIR_CONFIRM,
                             session_key, SESSION_KEY_LEN,
                             code_hash);

  /* canary-wap mesh_network.cpp:800:
   *   code = ((h[0] << 16) | (h[1] << 8) | h[2]) % 1000000
   * Exact same byte order and modulus so both lanes display the same
   * 6 digits for the same session_key. */
  const uint32_t top24 = ((uint32_t)code_hash[0] << 16) |
                         ((uint32_t)code_hash[1] << 8)  |
                         ((uint32_t)code_hash[2]);
  return top24 % CONFIRMATION_CODE_MODULUS;
}

void compute_confirmation_hash(const uint8_t session_key[SESSION_KEY_LEN],
                               uint32_t      code,
                               uint8_t       out[mesh_crypto::SHA256_OUT_LEN]) {
  if (session_key == nullptr || out == nullptr) return;

  /* Wire-compat with canary-wap mesh_network.cpp:856-860 / 1448-1450:
   * the hash input is session_key (32 bytes) followed by the
   * confirmation_code stored as a 4-byte little-endian uint32. We
   * construct the LE bytes explicitly so this is correct regardless
   * of host endianness (the test build is x86 LE; the device is
   * Xtensa LE; the wire is fixed LE). */
  uint8_t buf[SESSION_KEY_LEN + 4];
  for (size_t i = 0; i < SESSION_KEY_LEN; ++i) buf[i] = session_key[i];
  buf[SESSION_KEY_LEN + 0] = (uint8_t)(code         & 0xFF);
  buf[SESSION_KEY_LEN + 1] = (uint8_t)((code >> 8)  & 0xFF);
  buf[SESSION_KEY_LEN + 2] = (uint8_t)((code >> 16) & 0xFF);
  buf[SESSION_KEY_LEN + 3] = (uint8_t)((code >> 24) & 0xFF);

  mesh_crypto::sha256_domain(DOMAIN_PAIR_CONFIRM, buf, sizeof(buf), out);
}

}  /* namespace mesh_pairing */
