/**
 * @file ct_compare.h
 * @brief Constant-time byte equality, for compares where one side is secret.
 *
 * memcmp() returns at the first byte that differs, so how long it takes
 * says how many leading bytes matched. Where one side is a secret or is
 * derived from one (an HMAC tag, a pairing CONFIRM hash), that is the
 * textbook timing oracle: a party that can present guesses and time the
 * refusal can learn the expected bytes a few at a time. equal() reads
 * every byte of both buffers whatever they hold, folding the differences
 * into one accumulator, so the work it does depends on n alone.
 *
 * The compares it replaced (every accept and refuse and every log line
 * kept as it was):
 *   - the canary-wap's pairing CONFIRM check, handle_pair_confirm() in
 *     mesh_network.cpp, a memcmp(): the expected hash is SHA-256 over the
 *     pairing's X25519 session key and code (mesh_pair_crypto::confirm_hash).
 *     The canary (PIO) tree's twin already used mesh_crypto::ct_equal();
 *   - the chain backup's HMAC-SHA256 check, keyed by the device private
 *     key, a memcmp(): restore_chain() in the canary-wap's data_mgmt_api.h
 *     and datamgmt_restore_chain() in the canary (PIO) tree's
 *     securacv_data_mgmt.cpp;
 *   - the canary-wap's household RPA check, resolve_rpa_detailed() in
 *     household.cpp, an && chain that branched on each of the three bytes:
 *     the computed hash is AES-128 under an enrolled IRK (Bluetooth's ah()).
 * The survey behind them (memcmp/strcmp-family calls, ==/!= and && chains
 * on secret-named values, hand-written compare loops, over firmware/canary,
 * firmware/common and every firmware/projects tree) found the other
 * secret-dependent compares already running without an early exit
 * (api_auth.h's constant_time_compare(), securacv_auth, claim_ticket.h's
 * hex_equal(), fleet_key.h's tag_equal(), the display's csrf_matches(),
 * mesh_crypto::ct_equal(), and the ChaChaPoly checkTag() the sealed API and
 * the mesh frames use); this does not replace them. A survey is a search,
 * not a proof: its first pass missed the household chain.
 *
 * How it stays constant time as compiled, not only as written: both
 * buffers are read through volatile pointers and the differences are
 * stored into a volatile accumulator, so the compiler must make every load
 * and every store, in order, and cannot stop at the first difference or
 * skip a byte once the accumulator is nonzero. The loop runs n times; its
 * only branch is the loop's own count. The branches before it are on a
 * null pointer and on n being 0, facts about the call, not about the
 * bytes. Equal and unequal buffers take the same path to the one compare
 * of the accumulator at the end.
 *
 * Answers true when n > 0 and the n bytes at `a` equal the n bytes at `b`;
 * false when any byte differs, when n is 0 (a check of no bytes proves
 * nothing, so an empty tag is never a match), or when either pointer is
 * null. Both buffers must hold n bytes: equal() takes one length, so a
 * caller whose two lengths can differ refuses on that first (a length is
 * not secret), and nothing reads past either buffer.
 *
 * What it does not prove: anything about timing measured on a device. It
 * is host-tested for its answers (a flip of any byte answers unequal) and
 * for reading both ends of both buffers whatever they hold
 * (firmware/tests_host/test_ct_compare.cpp traps a read of each buffer's
 * first and last byte); its -O2 x86-64 assembly (GCC and clang) has no
 * branch on the bytes. The Xtensa and RISC-V builds are firmware.yml's
 * compiles and their code was not read; no timing was measured anywhere.
 *
 * Pure, Arduino-free, header-only. The canary (PIO) tree includes it from
 * firmware/common; the canary-wap sketch carries a byte-identical staged
 * copy, held by firmware/scripts/check_csi_sync.sh.
 */

#ifndef SECURACV_CT_COMPARE_H
#define SECURACV_CT_COMPARE_H

#include <stddef.h>
#include <stdint.h>

namespace ct_compare {

/** True when n > 0 and the n bytes at `a` and `b` are equal; false when
 *  any differ, when n is 0, or when either is null. Reads all n bytes of
 *  both, whatever they hold. */
[[nodiscard]] inline bool equal(const void* a, const void* b, size_t n) {
  if (a == nullptr || b == nullptr || n == 0) return false;
  const volatile uint8_t* pa = static_cast<const volatile uint8_t*>(a);
  const volatile uint8_t* pb = static_cast<const volatile uint8_t*>(b);
  volatile uint8_t diff = 0;
  for (size_t i = 0; i < n; ++i) {
    diff = static_cast<uint8_t>(diff | (pa[i] ^ pb[i]));
  }
  return diff == 0;
}

}  // namespace ct_compare

#endif /* SECURACV_CT_COMPARE_H */
