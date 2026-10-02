// Host test for canary-wap's Opera receive gates — the three pre-existing
// gaps the security re-review of the v0.4 wire registry (spec §4.5) found
// in mesh_network.cpp's handle_received_message() and closed after it.
// Host-tested only; not bench-verified; awaiting maintainer crypto review.
//
// This file reads the real mesh_network.cpp and mesh_network.h, and the PIO
// tree's mesh_session.cpp (beacon_source_scan.h; the paths are passed in,
// so every pin fails closed), and pins their text. test_mesh_address_wap
// runs the same mesh_network.cpp on host stubs, for behavior. This pins:
//
//   1. The replay counter convention, the same in both trees: the first
//      counter a sender signs is 1 (add_peer on a device that has signed
//      none; a rotation resets no counter since F95), and
//      the receiver drops counter <= last-seen with NO exemption. The old
//      gate, `counter <= rx && rx > 0`, existed so the old counter-0 first
//      frame could pass a fresh rx of 0 — and passed a counter-0 frame
//      again on every replay for as long as rx stayed 0. A model of each
//      gate, held to the pinned line, shows the hole and its closure.
//   2. No frame moves a member's address (spec §8.3). A frame whose source
//      is not the signer's bound address is dropped between the lookup and
//      verify_signature, by a read-only compare, and the receive path
//      writes no address and touches no ESP-NOW registration anywhere.
//      canary-wap used to re-point the member's address and ESP-NOW
//      registration at the source of a frame that passed signature,
//      opera_id and replay — and, before that, ahead of verify_signature,
//      where any signature did it (a keyless DoS). A verified frame does
//      not prove which radio sent it, so a replayed one passed too:
//      test_mesh_address_wap runs that against the real file. The PIO
//      session has the same source rule since F70 (it used to record the
//      source of a verified frame as the member's address, from any address
//      in its transport table), which the pin's last checks hold.
//   3. Every handler of a fixed-size struct payload takes payload_len and
//      refuses any other size, exactly (the PIO decoders' rule). The PIO
//      tree's TAMPER_ALERT is mesh_alert::PAYLOAD_LEN = 6 bytes; read as
//      this tree's 56-byte TamperAlertPayload it was 50 bytes of signature.
//      The struct mirrors here are held to the header's text, so the sizes
//      the test reasons about are the sketch's.
//
// Build: see the RX_GATES block in the Makefile.

#include "beacon_source_scan.h"
#include "mesh_alert.h"   // the PIO tree's, for its 6-byte TAMPER_ALERT length

#include <cstdint>
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
#ifndef MESH_SESSION_CPP
#error "MESH_SESSION_CPP (absolute path to the PIO mesh_session.cpp) must be defined"
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

// ── 1. The replay counter convention ────────────────────────────────────

// The gate as handle_received_message() now has it (pinned below): a
// counter at or below the last seen is a replay, whatever the last seen is.
struct RxGate {
  uint64_t last = 0;
  bool accept(uint64_t counter) {
    if (counter <= last) return false;
    last = counter;
    return true;
  }
};

// The gate as it was: the same, except that nothing is a replay while the
// last seen is still 0.
struct OldRxGate {
  uint64_t last = 0;
  bool accept(uint64_t counter) {
    if (counter <= last && last > 0) return false;
    last = counter;
    return true;
  }
};

// The sender as add_peer() and send_to_peer() now have it: the stored
// counter is the next one to sign, starting at 1, post-incremented.
struct TxCounter {
  uint64_t next = 1;
  uint64_t take() { return next++; }
};

void test_replay_gate_is_strict_and_the_first_counter_is_one() {
  const std::string code = load(MESH_NETWORK_CPP);
  const std::string sq   = squeeze(code);

  // The sender: add_peer starts at 1 on a device that has reserved no
  // counter (F99: one past its highest reservation otherwise), and so does
  // load_peers before the reservations are read. A rotation resets no
  // counter since F95 (the rekey-apply branch of handle_received_message
  // and maybe_finalize_rekey set them back to tx 1, rx 0; they carry on
  // now, as on the PIO tree, and test_mesh_liveness_wap runs it).
  // No reset to 0 remains anywhere in the file.
  // Since F99 a new member starts one past the highest reservation the
  // device stored for anyone: 1 on a device that has stored none
  // (g_tx_high_reserved starts at 0; test_mesh_liveness_wap runs both).
  const std::string add = squeeze(function_body(code, "add_peer"));
  CHECK(!add.empty());
  CHECK(count(add, "peer->msg_counter_tx=(g_tx_high_reserved==UINT64_MAX)?0:g_tx_high_reserved+1;") == 1);
  CHECK(count(sq, "staticuint64_tg_tx_high_reserved=0;") == 1);
  // Peers restored from NVS take the same convention (Codex P1 on #1752):
  // a static-zeroed tx would sign counter 0 and the strict gate drops it.
  const std::string ld = squeeze(function_body(code, "load_peers"));
  CHECK(count(ld, "g_peers[i].msg_counter_tx=1;") == 1);
  CHECK(count(ld, "g_peers[i].msg_counter_rx=0;") == 1);
  CHECK(count(add, "peer->msg_counter_rx=0;") == 1);
  CHECK(count(sq, "msg_counter_tx=0;") == 0);
  CHECK(count(sq, "msg_counter_tx=1;") == 1);  // load_peers
  const std::string fin = squeeze(function_body(code, "maybe_finalize_rekey"));
  CHECK(!fin.empty());
  CHECK(count(fin, "msg_counter_tx=") == 0);
  CHECK(count(fin, "msg_counter_rx=") == 0);
  const std::string tx = squeeze(function_body(code, "send_to_peer"));
  CHECK(!tx.empty());
  CHECK(count(tx, "uint64_tcounter=peer->msg_counter_tx++;") == 1);  // signs the stored value: 1 first

  // The receiver: strict, no exemption, the counter recorded right after.
  const std::string rx = squeeze(function_body(code, "handle_received_message"));
  CHECK(!rx.empty());
  const std::string gate = "if(counter<=peer->msg_counter_rx){return;}peer->msg_counter_rx=counter;";
  CHECK(count(rx, gate) == 1);
  CHECK(count(rx, "msg_counter_rx>0") == 0);
  CHECK(count(sq, "msg_counter_rx>0") == 0);
  CHECK(count(rx, "&&peer->msg_counter_rx") == 0);
  // The rekey-apply branch leaves the counters alone (F95): the only
  // write to one in the receive path is the replay gate's record.
  CHECK(count(rx, "peer->msg_counter_tx=") == 0);
  CHECK(count(rx, "peer->msg_counter_rx=") == 1);

  // The PIO tree says the same thing: its outbound counter starts at 0 and
  // the first one it hands out is +1, and its receive gate is `<=` with no
  // exemption.
  const std::string pio = load(MESH_SESSION_CPP);
  const std::string psq = squeeze(pio);
  CHECK(count(psq, "staticuint64_ts_outbound_counter=0;") == 1);
  const std::string next = squeeze(function_body(pio, "next_outbound_counter"));
  CHECK(!next.empty());
  CHECK(count(next, "constuint64_tnext=s_outbound_counter+1;") == 1);
  const std::string prx = squeeze(function_body(pio, "on_opera_frame"));
  CHECK(!prx.empty());
  // on_opera_frame is void again: #1756 (F49 part 3) made it return bool
  // for the unknown-sender hook, and that hook no longer takes an opera
  // frame. The strict `<=` and the record-right-after are what this pin
  // guards, through both shapes.
  CHECK(count(prx, "if(hdr.counter<=peer->last_counter)return;peer->last_counter=hdr.counter;") == 1);

  // The models, held to those lines by the pins above.
  {
    TxCounter sender;
    RxGate    receiver;
    CHECK(sender.take() == 1);                 // the first counter signed is 1 ...
    CHECK(receiver.accept(1));                 // ... and a fresh receiver takes it, no exemption needed
    CHECK(!receiver.accept(1));                // its replay is dropped
    CHECK(!receiver.accept(0));                // and 0 is never a fresh counter
    CHECK(receiver.accept(sender.take()));     // 2
    CHECK(receiver.accept(100));               // gaps are fine (a lost frame)
    CHECK(!receiver.accept(99));
  }
  {
    RxGate fresh;
    CHECK(!fresh.accept(0));                   // even against a fresh receiver
    CHECK(!fresh.accept(0));
    CHECK(fresh.last == 0);
  }
  {
    // The hole the old gate had: a counter-0 frame (the old first frame of
    // every peer, so one every peer had signed) replayed for as long as the
    // receiver's last-seen stayed 0 — which recording 0 kept it at.
    OldRxGate old;
    CHECK(old.accept(0));
    CHECK(old.accept(0));
    CHECK(old.accept(0));
    CHECK(old.last == 0);
    // And under the old sender convention (first counter 0) the strict gate
    // would have dropped every peer's first frame — which is why the sender
    // moved to 1 in the same change, not the gate alone.
    RxGate strict;
    CHECK(!strict.accept(0));
  }
  std::printf("PASS replay_gate_is_strict_and_the_first_counter_is_one\n");
}

// ── 2. No frame moves a member's address ────────────────────────────────

void test_no_frame_moves_a_members_address() {
  const std::string code = load(MESH_NETWORK_CPP);
  const std::string rx   = squeeze(function_body(code, "handle_received_message"));
  CHECK(!rx.empty());

  const std::string lookup  = "OperaPeer*peer=find_peer_by_fingerprint(sender_fp);";
  const std::string unknown = "if(!peer){g_auth_failures++;return;}";
  const std::string source  = "if(memcmp(peer->mac_addr,mac,6)!=0){g_auth_failures++;return;}";
  const std::string verify  = "if(!verify_signature(peer->pubkey,data,len-SIGNATURE_SIZE,signature)){g_auth_failures++;return;}";
  const std::string replay  = "if(counter<=peer->msg_counter_rx){return;}";
  CHECK(count(rx, lookup) == 1);
  CHECK(count(rx, source) == 1);
  CHECK(count(rx, verify) == 1);
  CHECK(count(rx, replay) == 1);
  // Order: lookup, source, verify, replay. `before` fails if a second copy
  // of the later text is inserted ahead of the earlier one.
  CHECK(before(rx, lookup, source));
  CHECK(before(rx, source, verify));
  CHECK(before(rx, verify, replay));
  // Between the lookup and the verify there is the unknown-peer drop and
  // the source drop, and nothing else: no write through `peer->`, no
  // ESP-NOW call.
  {
    const size_t a = rx.find(lookup) + lookup.size();
    const size_t b = rx.find(verify);
    CHECK(a <= b);
    CHECK(rx.substr(a, b - a) == unknown + source);
  }
  // The receive path writes no address and registers or drops no ESP-NOW
  // peer — not after the checks either, where the re-bind used to be.
  CHECK(count(rx, "memcpy(peer->mac_addr,") == 0);
  CHECK(count(rx, "mac_addr,mac") == 1);           // the source compare only
  CHECK(count(rx, "esp_now_add_peer(") == 0);
  CHECK(count(rx, "esp_now_del_peer(") == 0);
  // Where a member's address changes outside a first add: a re-pair
  // (add_peer -> rebind_peer), new address registered before the old one is
  // dropped, one address per member. (The other is load_peers folding a
  // duplicate entry an older firmware's re-pair saved; test_mesh_address_wap
  // runs it.)
  const std::string add = squeeze(function_body(code, "add_peer"));
  CHECK(count(add, "returnrebind_peer(&g_peers[i],mac);") == 1);
  // Since F98 a new key at an address another member holds is refused too,
  // ahead of the append (test_mesh_address_wap runs it).
  CHECK(count(add, "if(other_holder_of(mac,nullptr)!=nullptr){returnfalse;}") == 1);
  CHECK(before(add, "returnrebind_peer(&g_peers[i],mac);", "if(other_holder_of(mac,nullptr)!=nullptr){returnfalse;}"));
  CHECK(before(add, "if(other_holder_of(mac,nullptr)!=nullptr){returnfalse;}", "esp_now_add_peer(&peer_info)"));
  const std::string rb = squeeze(function_body(code, "rebind_peer"));
  CHECK(!rb.empty());
  CHECK(count(rb, "if(other_holder_of(mac,peer)!=nullptr){returnfalse;}") == 1);
  // The old address is released (its registration dropped unless another
  // member an older firmware stored there still holds it, F98) after the
  // new one is registered, and before the member moves.
  CHECK(before(rb, "esp_now_add_peer(&peer_info)", "release_mac(peer->mac_addr,peer);"));
  CHECK(before(rb, "release_mac(peer->mac_addr,peer);", "memcpy(peer->mac_addr,mac,6);"));
  // The PIO session has the same rule since F70: a member's frame is taken
  // only from the member's own binding (radio_mac), compared between its
  // lookup and its signature check, and after its replay gate the frame
  // only marks the member heard. Until F70 it recorded the frame's source
  // as the member's address (peer->mac), from any address in its transport
  // table. Its frames from an address the table does not hold are dropped
  // before this function runs, and nothing here writes an address (spec
  // §8.3).
  const std::string pio = load(MESH_SESSION_CPP);
  const std::string prx = squeeze(function_body(pio, "on_opera_frame"));
  CHECK(!prx.empty());
  const std::string pio_lookup = "TrustedPeer*peer=find_trusted_peer(sender_fp_in_frame);";
  const std::string pio_source =
      "if(!peer->radio_mac_set||memcmp(mac,peer->radio_mac,mesh_transport::MESH_TRANSPORT_MAC_LEN)!=0){return;}";
  const std::string pio_verify = "mesh_envelope::parse_and_verify(";
  const std::string pio_replay = "if(hdr.counter<=peer->last_counter)return;";
  const std::string pio_heard  = "peer->heard=true;";
  CHECK(count(prx, pio_lookup) == 1);
  CHECK(count(prx, pio_source) == 1);
  CHECK(count(prx, pio_heard) == 1);
  CHECK(before(prx, pio_lookup, pio_source));
  CHECK(before(prx, pio_source, pio_verify));
  CHECK(before(prx, pio_verify, pio_replay));
  CHECK(before(prx, pio_replay, pio_heard));
  CHECK(count(prx, "memcpy(") == 0);
  CHECK(count(prx, "bind_peer_mac(") == 0);
  CHECK(count(prx, "mesh_transport::add_peer(") == 0);
  CHECK(count(prx, "mesh_transport::remove_peer(") == 0);
  std::printf("PASS no_frame_moves_a_members_address\n");
}

// ── 3. Struct payloads are length-checked ───────────────────────────────

// Mirrors of mesh_network.h's fixed-size payload structs, field for field;
// each is held to the header's text below, so sizeof here is sizeof there
// (same ABI rules on the host and on Xtensa: natural alignment, no packing).
struct HeartbeatPayload {
  uint8_t status;
  uint32_t uptime_sec;
  uint8_t peer_count;
  uint8_t battery_pct;
};
struct TamperAlertPayload {
  uint8_t alert_type;
  uint8_t severity;
  uint32_t witness_seq;
  char detail[48];
};
struct PowerAlertPayload {
  uint8_t alert_type;
  uint16_t voltage_mv;
  uint16_t estimated_runtime_sec;
};
struct OfflineImminentPayload {
  uint8_t reason;
  uint32_t final_seq;
  uint8_t final_chain_hash[8];
};

// The gate every struct handler now applies, exact.
bool struct_gate(size_t payload_len, size_t struct_len) {
  return payload_len == struct_len;
}

void pin_struct(const std::string& h, const char* name, const char* fields) {
  const std::string body = block_after(h, std::string("struct") + name);
  CHECK(!body.empty());
  CHECK(body == std::string("{") + fields + "}");
}

void test_struct_handlers_check_the_payload_length() {
  const std::string code = load(MESH_NETWORK_CPP);
  const std::string h    = squeeze(load(MESH_NETWORK_H));

  // The mirrors are the header's structs.
  pin_struct(h, "HeartbeatPayload",
             "uint8_tstatus;uint32_tuptime_sec;uint8_tpeer_count;uint8_tbattery_pct;");
  pin_struct(h, "TamperAlertPayload",
             "uint8_talert_type;uint8_tseverity;uint32_twitness_seq;chardetail[48];");
  pin_struct(h, "PowerAlertPayload",
             "uint8_talert_type;uint16_tvoltage_mv;uint16_testimated_runtime_sec;");
  pin_struct(h, "OfflineImminentPayload",
             "uint8_treason;uint32_tfinal_seq;uint8_tfinal_chain_hash[8];");
  CHECK(count(h, "#pragmapack") == 0);
  CHECK(count(h, "__attribute__((packed))") == 0);
  static_assert(sizeof(TamperAlertPayload) == 56, "1+1+(pad 2)+4+48");
  static_assert(sizeof(HeartbeatPayload) == 12, "1+(pad 3)+4+1+1+(pad 2)");
  static_assert(sizeof(PowerAlertPayload) == 6, "1+(pad 1)+2+2");
  static_assert(sizeof(OfflineImminentPayload) == 16, "1+(pad 3)+4+8");

  // The dispatch hands every struct handler the payload length.
  const std::string rx = squeeze(function_body(code, "handle_received_message"));
  CHECK(!rx.empty());
  const char* calls[] = {
    "handle_heartbeat(peer,payload,payload_len);",
    "handle_auth_challenge(mac,payload,payload_len);",
    "handle_auth_response(peer,payload,payload_len);",
    "handle_tamper_alert(peer,payload,payload_len);",
    "handle_power_alert(peer,payload,payload_len);",
    "handle_offline_imminent(peer,payload,payload_len);",
  };
  for (const char* c : calls) CHECK(count(rx, c) == 1);
  // No handler is called without it.
  CHECK(count(rx, "handle_heartbeat(peer,payload);") == 0);
  CHECK(count(rx, "handle_tamper_alert(peer,payload);") == 0);
  CHECK(count(rx, ",payload);") == 0);
  // The two inline rekey cases gate before they cast.
  const std::string rk_gate = "if(payload_len!=sizeof(OperaRekeyPayload))break;";
  const std::string rk_cast = "constOperaRekeyPayload*rk=(constOperaRekeyPayload*)payload;";
  const std::string ak_gate = "if(payload_len!=sizeof(OperaRekeyAckPayload))break;";
  const std::string ak_cast = "constOperaRekeyAckPayload*ack=(constOperaRekeyAckPayload*)payload;";
  CHECK(count(rx, rk_gate) == 1 && count(rx, rk_cast) == 1 && before(rx, rk_gate, rk_cast));
  CHECK(count(rx, ak_gate) == 1 && count(rx, ak_cast) == 1 && before(rx, ak_gate, ak_cast));

  // Each handler refuses any other length before it casts.
  struct Handler { const char* fn; const char* type; const char* var; };
  const Handler handlers[] = {
    {"handle_heartbeat",        "HeartbeatPayload",       "hb"},
    {"handle_auth_challenge",   "AuthChallengePayload",   "challenge"},
    {"handle_auth_response",    "AuthResponsePayload",    "response"},
    {"handle_tamper_alert",     "TamperAlertPayload",     "alert"},
    {"handle_power_alert",      "PowerAlertPayload",      "alert"},
    {"handle_offline_imminent", "OfflineImminentPayload", "alert"},
  };
  for (const Handler& hd : handlers) {
    const std::string body = squeeze(function_body(code, hd.fn));
    CHECK(!body.empty());
    const std::string gate = std::string("if(payload_len!=sizeof(") + hd.type + "))return;";
    const std::string cast = std::string("const") + hd.type + "*" + hd.var + "=(const" + hd.type + "*)payload;";
    CHECK(count(body, gate) == 1);
    CHECK(count(body, cast) == 1);
    CHECK(before(body, gate, cast));
  }
  // And the definitions take the length (a declaration without it would
  // not compile against these calls, but pin the shape anyway).
  const std::string sq = squeeze(code);
  for (const Handler& hd : handlers) {
    const std::string decl = std::string("staticvoid") + hd.fn + "(";
    CHECK(count(sq, decl) == 2);  // forward declaration + definition
    CHECK(count(sq, decl + "OperaPeer*peer,constuint8_t*payload,size_tpayload_len)") == 2 ||
          count(sq, decl + "constuint8_t*mac,constuint8_t*payload,size_tpayload_len)") == 2);
  }

  // The gate, on the lengths that matter: the PIO tree's 6-byte
  // TAMPER_ALERT (mesh_alert.h, spec §4.3) is not this tree's 56-byte
  // struct — reading it as one took 50 bytes past the payload, out of the
  // signature that follows it.
  static_assert(mesh_alert::PAYLOAD_LEN == 6, "the PIO template-only TAMPER_ALERT");
  CHECK(sizeof(TamperAlertPayload) - mesh_alert::PAYLOAD_LEN == 50);
  CHECK(!struct_gate(mesh_alert::PAYLOAD_LEN, sizeof(TamperAlertPayload)));
  CHECK(struct_gate(sizeof(TamperAlertPayload), sizeof(TamperAlertPayload)));
  CHECK(!struct_gate(sizeof(TamperAlertPayload) - 1, sizeof(TamperAlertPayload)));
  CHECK(!struct_gate(sizeof(TamperAlertPayload) + 1, sizeof(TamperAlertPayload)));   // exact, as PIO decodes
  CHECK(!struct_gate(0, sizeof(TamperAlertPayload)));
  // A zero-length payload (the minimum 102-byte frame) reaches no struct
  // handler: every struct is at least one byte.
  CHECK(!struct_gate(0, sizeof(HeartbeatPayload)));
  CHECK(!struct_gate(0, sizeof(PowerAlertPayload)));
  CHECK(!struct_gate(0, sizeof(OfflineImminentPayload)));
  std::printf("PASS struct_handlers_check_the_payload_length\n");
}

}  // namespace

int main() {
  test_replay_gate_is_strict_and_the_first_counter_is_one();
  test_no_frame_moves_a_members_address();
  test_struct_handlers_check_the_payload_length();
  std::printf("\nALL mesh receive gate (canary-wap) tests PASSED (%d checks)\n", g_checks);
  return 0;
}
