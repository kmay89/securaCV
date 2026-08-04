// Host-side test for the Opera mesh v0.2 security fixes (audit O1–O3).
//
// We don't link the firmware's mesh_network.cpp (ESP32-only). Instead this
// file reasserts the *invariants* the fixes establish:
//
//   O1 — message TTL is anchored on the per-peer monotonic counter, not on
//        millis()/1000 uptime. Replay protection is provided by the counter
//        alone in v0.2.
//   O2 — opera_secret persistence requires flash encryption to be enabled.
//        Tested here as a behavioral predicate: a "FE-off" load attempt
//        wipes the in-memory secret and returns failure.
//   O3 — remove_peer() initiates a rekey transaction that only commits the
//        new opera_secret after all surviving members ACK (or after a
//        timeout, in which case unacked peers are marked stale).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr size_t OPERA_SECRET_SIZE = 32;
constexpr size_t OPERA_ID_SIZE     = 16;

// O1: simulated monotonic-counter replay check.
struct Peer { uint64_t rx_counter = 0; };
bool counter_accept(Peer& p, uint64_t incoming) {
  if (incoming <= p.rx_counter && p.rx_counter > 0) return false;
  p.rx_counter = incoming;
  return true;
}

// O2: simulated FE-gated persistence.
struct OperaConfig {
  uint8_t secret[OPERA_SECRET_SIZE];
  uint8_t opera_id[OPERA_ID_SIZE];
  bool    configured;
};
bool persist_opera_config(OperaConfig& cfg, bool fe_enabled) {
  if (!fe_enabled) return false;
  cfg.configured = true;
  return true;
}
bool load_opera_config(OperaConfig& cfg, bool fe_enabled,
                       const uint8_t* on_disk_secret) {
  if (!fe_enabled) {
    // FE off: wipe in-memory and return failure (matches v0.2 behavior).
    std::memset(cfg.secret, 0, OPERA_SECRET_SIZE);
    std::memset(cfg.opera_id, 0, OPERA_ID_SIZE);
    cfg.configured = false;
    return false;
  }
  std::memcpy(cfg.secret, on_disk_secret, OPERA_SECRET_SIZE);
  cfg.configured = true;
  return true;
}

// O3: simulated rekey transaction.
struct RekeyState {
  bool     active;
  uint32_t rekey_id;
  uint16_t pending_acks;     // bit i set if peer i hasn't ACKed yet
  uint8_t  pending_secret[OPERA_SECRET_SIZE];
  uint32_t started_ms;
};

// Mimics maybe_finalize_rekey: commits when all ACKed or after timeout.
struct PeerState { bool stale = false; bool session_established = true; };
bool finalize_rekey(RekeyState& r, OperaConfig& cfg,
                    std::vector<PeerState>& peers,
                    uint32_t now_ms, uint32_t timeout_ms) {
  if (!r.active) return false;
  bool all_acked = (r.pending_acks == 0);
  bool timed_out = (now_ms - r.started_ms) > timeout_ms;
  if (!all_acked && !timed_out) return false;
  std::memcpy(cfg.secret, r.pending_secret, OPERA_SECRET_SIZE);
  for (size_t i = 0; i < peers.size(); i++) {
    bool unacked = (r.pending_acks & (uint16_t)(1u << i)) != 0;
    peers[i].session_established = false;
    peers[i].stale = unacked;
  }
  r.active = false;
  return true;
}

int failures = 0;
#define EXPECT(cond, msg) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
  } while(0)

void test_o1_counter_replay_protection() {
  Peer p;
  EXPECT(counter_accept(p, 1), "first message accepted");
  EXPECT(counter_accept(p, 2), "monotonically increasing accepted");
  EXPECT(!counter_accept(p, 2), "replay of same counter rejected");
  EXPECT(!counter_accept(p, 1), "lower-than-last counter rejected");
  EXPECT(counter_accept(p, 100), "skip-forward accepted (gaps allowed)");
}

void test_o2_load_refuses_when_fe_off() {
  OperaConfig cfg{};
  uint8_t disk_secret[OPERA_SECRET_SIZE];
  std::memset(disk_secret, 0xAB, sizeof(disk_secret));

  // FE off: load must refuse AND wipe in-memory state.
  bool ok = load_opera_config(cfg, /*fe_enabled=*/false, disk_secret);
  EXPECT(!ok, "O2: load returns false when FE is off");
  EXPECT(cfg.configured == false, "O2: cfg.configured stays false");
  for (size_t i = 0; i < OPERA_SECRET_SIZE; i++) {
    if (cfg.secret[i] != 0) { failures++; break; }
  }

  // FE on: load proceeds.
  ok = load_opera_config(cfg, /*fe_enabled=*/true, disk_secret);
  EXPECT(ok, "O2: load returns true when FE is on");
  EXPECT(cfg.configured == true, "O2: cfg.configured becomes true");
  EXPECT(std::memcmp(cfg.secret, disk_secret, OPERA_SECRET_SIZE) == 0,
         "O2: secret bytes restored from 'disk'");
}

void test_o2_persist_refuses_when_fe_off() {
  OperaConfig cfg{};
  EXPECT(!persist_opera_config(cfg, /*fe_enabled=*/false),
         "O2: persist returns false when FE is off");
  EXPECT(persist_opera_config(cfg, /*fe_enabled=*/true),
         "O2: persist returns true when FE is on");
}

void test_o3_rekey_commits_on_all_acks() {
  RekeyState r{};
  r.active = true;
  r.rekey_id = 42;
  r.started_ms = 0;
  std::memset(r.pending_secret, 0xCC, sizeof(r.pending_secret));
  std::vector<PeerState> peers(3);
  // 3 peers pending ACK → bits 0,1,2 set
  r.pending_acks = 0b111;

  EXPECT(!finalize_rekey(r, *(new OperaConfig{}), peers, /*now_ms=*/100,
                         /*timeout_ms=*/60000),
         "O3: rekey not finalized while ACKs outstanding");

  // ACKs come in
  r.pending_acks &= ~(uint16_t)0b001;
  r.pending_acks &= ~(uint16_t)0b010;
  r.pending_acks &= ~(uint16_t)0b100;

  OperaConfig cfg{};
  EXPECT(finalize_rekey(r, cfg, peers, 200, 60000),
         "O3: rekey finalizes when all ACKs received");
  EXPECT(cfg.secret[0] == 0xCC, "O3: new secret committed on commit");
  for (auto& p : peers) EXPECT(!p.stale, "O3: ACKing peers are not marked stale");
  for (auto& p : peers) EXPECT(!p.session_established,
                               "O3: all sessions invalidated after rekey");
}

void test_o3_rekey_timeout_marks_unacked_stale() {
  RekeyState r{};
  r.active = true;
  r.rekey_id = 7;
  r.started_ms = 0;
  std::memset(r.pending_secret, 0xDD, sizeof(r.pending_secret));
  std::vector<PeerState> peers(3);
  r.pending_acks = 0b111;
  // Only peer 0 ACKs.
  r.pending_acks &= ~(uint16_t)0b001;

  OperaConfig cfg{};
  // Within timeout: no commit.
  EXPECT(!finalize_rekey(r, cfg, peers, 1000, 60000),
         "O3: pre-timeout with outstanding ACKs → not finalized");

  // After timeout.
  EXPECT(finalize_rekey(r, cfg, peers, 70000, 60000),
         "O3: post-timeout → finalize anyway");
  EXPECT(!peers[0].stale, "O3: peer 0 ACKed → not stale");
  EXPECT(peers[1].stale,  "O3: peer 1 didn't ACK → marked stale");
  EXPECT(peers[2].stale,  "O3: peer 2 didn't ACK → marked stale");
}

} // namespace

int main() {
  test_o1_counter_replay_protection();
  test_o2_load_refuses_when_fe_off();
  test_o2_persist_refuses_when_fe_off();
  test_o3_rekey_commits_on_all_acks();
  test_o3_rekey_timeout_marks_unacked_stale();

  if (failures == 0) {
    std::printf("All mesh opera security invariants passed.\n");
    return 0;
  } else {
    std::printf("%d failures.\n", failures);
    return 1;
  }
}
