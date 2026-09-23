// Host test for canary-wap's use of the Opera revocation deny-list (spec §5.6
// REVOCATION_GRACE_MS) — F33 part 6.
//
// The list itself is the PlatformIO tree's mesh_revocation module, staged
// byte-identical into the sketch (firmware/scripts/check_mesh_sync.sh); its
// behavior suite (firmware/canary/lib/securacv_mesh/test_mesh_revocation.cpp)
// also runs here against the staged copy (the Makefile's
// test_mesh_revocation_staged). This file pins what the sketch does with it,
// by reading the real mesh_network.cpp (beacon_source_scan.h; its path is
// passed in, so the pins fail closed):
//   1. remove_peer() deny-lists the removed fingerprint — copied before the
//      peer table shifts over it — and persists the list;
//   2. handle_pair_discover() refuses a deny-listed joiner before it makes a
//      key or sends an OFFER, and handle_pair_offer() a deny-listed
//      initiator before it records anything;
//   3. add_peer() refuses a deny-listed pubkey;
//   4. init() restores the list (before the peers load), update() expires it
//      every pass and re-persists it every 5 minutes;
//   5. persistence is flash-encryption gated, like the peer list.
// And, on the staged copy: a removal is refused for exactly
// REVOCATION_GRACE_MS (7 days).

#include "mesh_revocation.h"
#include "beacon_source_scan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef MESH_NETWORK_CPP
#error "MESH_NETWORK_CPP (absolute path to mesh_network.cpp) must be defined"
#endif

namespace {

int g_checks = 0;
#define CHECK(c)                                                          \
  do {                                                                    \
    ++g_checks;                                                           \
    if (!(c)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
      std::exit(1);                                                       \
    }                                                                     \
  } while (0)

using beacon_source_scan::count;
using beacon_source_scan::function_body;
using beacon_source_scan::squeeze;

void test_staged_grace() {
  mesh_revocation::List l;
  mesh_revocation::init(l);
  const uint8_t fp[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(mesh_revocation::add(l, fp, 500));
  CHECK(mesh_revocation::contains(l, fp, 500 + mesh_revocation::REVOCATION_GRACE_MS - 1));
  CHECK(!mesh_revocation::contains(l, fp, 500 + mesh_revocation::REVOCATION_GRACE_MS));
  CHECK(mesh_revocation::REVOCATION_GRACE_MS == 7u * 24u * 3600u * 1000u);
  std::printf("PASS staged_grace\n");
}

void test_mesh_network_uses_the_list() {
  bool ok = false;
  const std::string raw = beacon_source_scan::read_source(MESH_NETWORK_CPP, &ok);
  CHECK(ok);
  const std::string code = beacon_source_scan::strip_comments(raw);

  const std::string rm = squeeze(function_body(code, "remove_peer"));
  CHECK(!rm.empty());
  CHECK(count(rm, "memcpy(removed_fp,g_peers[i].fingerprint,FINGERPRINT_SIZE);") == 1);
  CHECK(count(rm, "mesh_revocation::add(g_revoked,removed_fp,millis());persist_revocations();") == 1);
  CHECK(rm.find("memcpy(removed_fp,") < rm.find("g_peers[j]=g_peers[j+1];"));

  const std::string disc = squeeze(function_body(code, "handle_pair_discover"));
  const std::string refuse_disc = "if(is_revoked_pubkey(discover->pubkey)){return;}";
  CHECK(count(disc, refuse_disc) == 1);
  CHECK(disc.find(refuse_disc) < disc.find("generate_keypair("));
  CHECK(disc.find(refuse_disc) < disc.find("send_pair_frame("));

  const std::string offer = squeeze(function_body(code, "handle_pair_offer"));
  const std::string refuse_offer = "if(is_revoked_pubkey(offer->device_pubkey)){return;}";
  CHECK(count(offer, refuse_offer) == 1);
  CHECK(offer.find(refuse_offer) < offer.find("memcpy(g_pairing.peer_pubkey,"));

  const std::string add = squeeze(function_body(code, "add_peer"));
  CHECK(count(add, "if(is_revoked_pubkey(pubkey)){returnfalse;}") == 1);

  const std::string init = squeeze(function_body(code, "init"));
  CHECK(count(init, "load_revocations();") == 1);
  CHECK(init.find("load_revocations();") < init.find("load_peers();"));

  const std::string upd = squeeze(function_body(code, "update"));
  CHECK(count(upd, "mesh_revocation::expire(g_revoked,now);") == 1);
  CHECK(count(upd, "persist_revocations();") == 1);
  CHECK(count(upd, ">=300000u") == 1);

  const std::string save = squeeze(function_body(code, "persist_revocations"));
  const std::string load = squeeze(function_body(code, "load_revocations"));
  CHECK(count(save, "if(!flash_encryption_enabled())return;") == 1);
  CHECK(count(load, "if(!flash_encryption_enabled())return;") == 1);
  CHECK(count(load, "mesh_revocation::decode(g_revoked,blob,got,millis())") == 1);

  const std::string isr = squeeze(function_body(code, "is_revoked_pubkey"));
  CHECK(count(isr, "returnmesh_revocation::contains(g_revoked,fp,millis());") == 1);
  std::printf("PASS mesh_network_uses_the_list\n");
}

}  // namespace

int main() {
  test_staged_grace();
  test_mesh_network_uses_the_list();
  std::printf("\nALL mesh revocation (canary-wap) tests PASSED (%d checks)\n", g_checks);
  return 0;
}
