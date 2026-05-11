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

}  /* namespace mesh_pairing */
