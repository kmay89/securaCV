// Host test for canary-wap's use of the Opera mesh wire registry
// (mesh_wire.h, spec §4.5 — v0.4, awaiting crypto review, not bench-verified).
//
// The registry is the PlatformIO tree's header, staged byte-identical into
// the sketch (firmware/scripts/check_mesh_sync.sh); its own suite
// (firmware/canary/lib/securacv_mesh/test_mesh_wire.cpp) runs here against
// the staged copy (the Makefile's test_mesh_wire_staged). This file pins what
// the sketch does with it, by reading the real mesh_network.h and
// mesh_network.cpp (beacon_source_scan.h; their paths are passed in, so the
// pins fail closed):
//   1. PROTOCOL_VERSION is the registry's, and every MessageType value is an
//      initializer from mesh_wire:: — none typed by hand, none left to count
//      from the previous one (which is how 0..21 came to disagree with the
//      PIO tree's 16..29);
//   2. handle_received_message() drops any other version byte before it
//      reads the type, and reads the type from the signed header (byte 1),
//      so the PIO tree's old unsigned prefix cannot be taken for a frame;
//   3. send_to_peer() writes the header in the registry's order: version,
//      type, opera_id, sender fingerprint, counter (8 B LE), timestamp (4 B);
//   4. mesh_pair_frame's TYPE_* are the registry's pairing values, and the
//      sketch static_asserts its enum against the registry.
// The staged header itself pins the numbers (test_mesh_wire_staged).

#include "mesh_wire.h"
#include "mesh_pair_frame.h"
#include "beacon_source_scan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef MESH_NETWORK_CPP
#error "MESH_NETWORK_CPP (absolute path to mesh_network.cpp) must be defined"
#endif
#ifndef MESH_NETWORK_H
#error "MESH_NETWORK_H (absolute path to mesh_network.h) must be defined"
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

using beacon_source_scan::before;
using beacon_source_scan::block_after;
using beacon_source_scan::count;
using beacon_source_scan::function_body;
using beacon_source_scan::squeeze;

std::string load(const char* path) {
  bool ok = false;
  const std::string raw = beacon_source_scan::read_source(path, &ok);
  CHECK(ok);
  return beacon_source_scan::strip_comments(raw);
}

void test_header_takes_every_value_from_the_registry() {
  const std::string h = squeeze(load(MESH_NETWORK_H));

  CHECK(count(h, "staticconstuint8_tPROTOCOL_VERSION=mesh_wire::OPERA_VERSION;") == 1);

  const std::string en = block_after(h, "enumMessageType:uint8_t");
  CHECK(!en.empty());
  struct Pin { const char* name; const char* reg; uint8_t value; };
  const Pin pins[] = {
    {"MSG_HEARTBEAT",       "HEARTBEAT",        mesh_wire::HEARTBEAT},
    {"MSG_AUTH_CHALLENGE",  "AUTH_CHALLENGE",   mesh_wire::AUTH_CHALLENGE},
    {"MSG_AUTH_RESPONSE",   "AUTH_RESPONSE",    mesh_wire::AUTH_RESPONSE},
    {"MSG_AUTH_COMPLETE",   "AUTH_COMPLETE",    mesh_wire::AUTH_COMPLETE},
    {"MSG_TAMPER_ALERT",    "TAMPER_ALERT",     mesh_wire::TAMPER_ALERT},
    {"MSG_POWER_ALERT",     "POWER_ALERT",      mesh_wire::POWER_ALERT},
    {"MSG_OFFLINE_IMMINENT","OFFLINE_IMMINENT", mesh_wire::OFFLINE_IMMINENT},
    {"MSG_PEER_LIST",       "PEER_LIST",        mesh_wire::PEER_LIST},
    {"MSG_PAIR_DISCOVER",   "PAIR_DISCOVER",    mesh_wire::PAIR_DISCOVER},
    {"MSG_PAIR_OFFER",      "PAIR_OFFER",       mesh_wire::PAIR_OFFER},
    {"MSG_PAIR_ACCEPT",     "PAIR_ACCEPT",      mesh_wire::PAIR_ACCEPT},
    {"MSG_PAIR_CONFIRM",    "PAIR_CONFIRM",     mesh_wire::PAIR_CONFIRM},
    {"MSG_PAIR_COMPLETE",   "PAIR_COMPLETE",    mesh_wire::PAIR_COMPLETE},
    {"MSG_LEAVE_OPERA",     "LEAVE_OPERA",      mesh_wire::LEAVE_OPERA},
    {"MSG_ENCRYPTED",       "ENCRYPTED",        mesh_wire::ENCRYPTED},
    {"MSG_OPERA_REKEY",     "OPERA_REKEY",      mesh_wire::OPERA_REKEY},
    {"MSG_OPERA_REKEY_ACK", "OPERA_REKEY_ACK",  mesh_wire::OPERA_REKEY_ACK},
    {"MSG_BEACON_EVENT",    "BEACON_EVENT",     mesh_wire::BEACON_EVENT},
    {"MSG_CHANNEL_LOCK",    "CHANNEL_LOCK",     mesh_wire::CHANNEL_LOCK},
    {"MSG_HUB_ELECTION",    "HUB_ELECTION",     mesh_wire::HUB_ELECTION},
  };
  size_t enumerators = 0;
  for (const Pin& p : pins) {
    const std::string init = std::string(p.name) + "=mesh_wire::" + p.reg;
    CHECK(count(en, init + ",") + count(en, init + "}") == 1);
    ++enumerators;
  }
  // Exactly those enumerators, each with an initializer: a new one that
  // counts from its neighbor, or one typed as a number, fails here.
  CHECK(count(en, "MSG_") == enumerators);
  CHECK(count(en, "=mesh_wire::") == enumerators);
  for (int d = 0; d <= 9; ++d) {
    const std::string digit = std::string("=") + (char)('0' + d);
    CHECK(count(en, digit) == 0);
  }
  // The values the two trees used to disagree on are the PIO ones now.
  CHECK(mesh_wire::TAMPER_ALERT == 18 && mesh_wire::CHANNEL_LOCK == 23 &&
        mesh_wire::HUB_ELECTION == 24 && mesh_wire::LEAVE_OPERA == 25);
  std::printf("PASS header_takes_every_value_from_the_registry\n");
}

void test_receive_path_gates_on_the_registry_version() {
  const std::string code = load(MESH_NETWORK_CPP);
  const std::string rx = squeeze(function_body(code, "handle_received_message"));
  CHECK(!rx.empty());
  // The pairing classifier runs first, on the FIRST byte, before the
  // 102-byte gate; then the version byte is read and gated; then the type
  // is read from byte 1 of the (signed) header. Nothing is read from the
  // frame before the version except by the classifier.
  const std::string classify = "mesh_pair_frame::classify(data,len,&pair_type,&pair_payload,&pair_len)";
  const std::string gate     = "if(len<102){return;}";
  const std::string version  = "uint8_tversion=data[offset++];if(version!=PROTOCOL_VERSION){return;}";
  const std::string type     = "MessageTypemsg_type=(MessageType)data[offset++];";
  CHECK(count(rx, classify) == 1);
  CHECK(count(rx, gate) == 1);
  CHECK(count(rx, version) == 1);
  CHECK(count(rx, type) == 1);
  CHECK(before(rx, classify, gate));
  CHECK(before(rx, gate, version));
  CHECK(before(rx, version, type));
  CHECK(count(rx, "size_toffset=0;") == 1);
  CHECK(before(rx, "size_toffset=0;", version));
  // The pairing range is refused inside the signed header, by the enum.
  CHECK(count(rx, "if(msg_type>=MSG_PAIR_DISCOVER&&msg_type<=MSG_PAIR_COMPLETE){return;}") == 1);
  // The signature covers everything before it.
  CHECK(count(rx, "verify_signature(peer->pubkey,data,len-SIGNATURE_SIZE,signature)") == 1);
  std::printf("PASS receive_path_gates_on_the_registry_version\n");
}

void test_send_path_writes_the_registry_header() {
  const std::string code = load(MESH_NETWORK_CPP);
  const std::string tx = squeeze(function_body(code, "send_to_peer"));
  CHECK(!tx.empty());
  const char* order[] = {
    "msg[offset++]=PROTOCOL_VERSION;",
    "msg[offset++]=(uint8_t)type;",
    "memcpy(msg+offset,g_opera_config.opera_id,OPERA_ID_SIZE);offset+=OPERA_ID_SIZE;",
    "memcpy(msg+offset,g_device_fingerprint,FINGERPRINT_SIZE);offset+=FINGERPRINT_SIZE;",
    "for(inti=0;i<8;i++){msg[offset++]=(counter>>(i*8))&0xFF;}",
    "memcpy(msg+offset,&timestamp,4);offset+=4;",
    "sign_message(g_device_privkey,msg,offset,signature);",
  };
  for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
    CHECK(count(tx, order[i]) == 1);
    if (i > 0) CHECK(before(tx, order[i - 1], order[i]));
  }
  // The wire header is 38 bytes and the frame minimum 102 — the PIO
  // envelope's HEADER_LEN and MIN_FRAME_LEN.
  const std::string sq = squeeze(code);
  CHECK(count(sq, "staticconstexprsize_tWIRE_HEADER_BYTES=2+OPERA_ID_SIZE+FINGERPRINT_SIZE+8+4;") == 1);
  CHECK(count(sq, "static_assert(WIRE_HEADER_BYTES+SIGNATURE_SIZE==102,") == 1);
  // And the sketch pins its enum to the registry at compile time.
  CHECK(count(sq, "static_assert(PROTOCOL_VERSION==mesh_wire::OPERA_VERSION,") == 1);
  CHECK(count(sq, "static_assert(MSG_TAMPER_ALERT==mesh_wire::TAMPER_ALERT,") == 1);
  CHECK(count(sq, "static_assert(MSG_LEAVE_OPERA==mesh_wire::LEAVE_OPERA,") == 1);
  std::printf("PASS send_path_writes_the_registry_header\n");
}

void test_pair_frame_types_are_the_registry_values() {
  CHECK(mesh_pair_frame::TYPE_DISCOVER == mesh_wire::PAIR_DISCOVER);
  CHECK(mesh_pair_frame::TYPE_OFFER    == mesh_wire::PAIR_OFFER);
  CHECK(mesh_pair_frame::TYPE_ACCEPT   == mesh_wire::PAIR_ACCEPT);
  CHECK(mesh_pair_frame::TYPE_CONFIRM  == mesh_wire::PAIR_CONFIRM);
  CHECK(mesh_pair_frame::TYPE_COMPLETE == mesh_wire::PAIR_COMPLETE);
  for (int b = 0; b < 256; ++b) {
    CHECK((mesh_pair_frame::payload_len_for((uint8_t)b) != 0) ==
          mesh_wire::is_pairing_type((uint8_t)b));
  }
  // A frame starting with the version byte is never a pairing frame, at
  // any length a pairing frame may have.
  uint8_t frame[mesh_pair_frame::MAX_FRAME_LEN] = {0};
  frame[0] = mesh_wire::OPERA_VERSION;
  const size_t lens[] = {59, 99, 33, 61};
  for (size_t len : lens) {
    uint8_t t = 0; const uint8_t* p = nullptr; size_t pl = 0;
    CHECK(!mesh_pair_frame::classify(frame, len, &t, &p, &pl));
  }
  std::printf("PASS pair_frame_types_are_the_registry_values\n");
}

}  // namespace

int main() {
  test_header_takes_every_value_from_the_registry();
  test_receive_path_gates_on_the_registry_version();
  test_send_path_writes_the_registry_header();
  test_pair_frame_types_are_the_registry_values();
  std::printf("\nALL mesh wire registry (canary-wap) tests PASSED (%d checks)\n", g_checks);
  return 0;
}
