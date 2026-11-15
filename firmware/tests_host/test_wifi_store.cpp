// Host tests for firmware/common/network/wifi_store.h — the saved-networks
// ring: add/dedupe/evict, remove, the pick rule, a blob that parses as
// "no store" when it is torn, and the joined bit (FLAG_JOINED) that a join
// alone sets and the blob carries across a power cut.

#include "../common/network/wifi_store.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace canary::net::wifi_store;

static int g_failures = 0;

#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (!(cond)) {                                             \
      std::printf("FAIL %s:%d: ", __func__, __LINE__);         \
      std::printf(__VA_ARGS__);                                \
      std::printf("\n");                                       \
      ++g_failures;                                            \
    }                                                          \
  } while (0)

static void the_blob_has_a_fixed_size() {
  CHECK(ENTRY_BYTES == 100, "33 + 65 + fails + flags");
  CHECK(BLOB_BYTES == 412, "8 header + 4*100 + crc");
  CHECK(MAX_NETWORKS == 4, "four slots");
}

static void add_dedupes_and_marks_last_good() {
  Store s; init(s);
  CHECK(s.count == 0, "empty");
  CHECK(find(s, "Home") < 0, "nothing yet");
  CHECK(add(s, "Home", "hunter2"), "add");
  CHECK(s.count == 1 && s.last_good == 0, "one, last good");
  CHECK(add(s, "Work", ""), "open network saved");
  CHECK(s.count == 2 && s.last_good == 1, "two, work last good");
  note_result(s, 0, false); note_result(s, 0, false);
  CHECK(s.nets[0].fails == 2, "two failures on Home");
  CHECK(add(s, "Home", "newpass"), "re-add Home");
  CHECK(s.count == 2, "deduped");
  CHECK(!std::strcmp(s.nets[0].pass, "newpass"), "password replaced");
  CHECK(s.nets[0].fails == 0 && s.last_good == 0, "fails cleared, Home last good");
  CHECK(!add(s, std::string(33, 's').c_str(), "x"), "33-byte ssid refused");
  CHECK(!add(s, "", "x"), "empty ssid refused");
  CHECK(!add(s, "X", std::string(65, 'p').c_str()), "65-byte password refused");
  CHECK(s.count == 2, "refusals change nothing");
}

static void a_fifth_network_evicts_the_most_failing_non_last_good() {
  Store s; init(s);
  add(s, "A", "a"); add(s, "B", "b"); add(s, "C", "c"); add(s, "D", "d");
  CHECK(s.count == 4 && s.last_good == 3, "full, D last good");
  note_result(s, 1, false); note_result(s, 1, false); note_result(s, 1, false);  // B fails most
  note_result(s, 3, false); note_result(s, 3, false); note_result(s, 3, false); note_result(s, 3, false);
  // D fails even more but is last_good: protected.
  CHECK(add(s, "E", "e"), "add a fifth");
  CHECK(s.count == 4, "still four");
  CHECK(find(s, "B") < 0, "B evicted");
  CHECK(find(s, "D") >= 0, "last good kept");
  CHECK(find(s, "E") == 1, "E took B's slot");
  CHECK(s.last_good == 1, "E is last good");
}

static void remove_shifts_and_keeps_last_good_honest() {
  Store s; init(s);
  add(s, "A", "a"); add(s, "B", "b"); add(s, "C", "c");
  note_result(s, 2, true);
  CHECK(s.last_good == 2, "C last good");
  CHECK(remove(s, "A"), "remove A");
  CHECK(s.count == 2 && find(s, "B") == 0 && find(s, "C") == 1, "shifted");
  CHECK(s.last_good == 1, "last good followed C");
  CHECK(remove(s, "C"), "remove the last good");
  CHECK(s.last_good == 0, "falls back to slot 0");
  CHECK(!remove(s, "Nope"), "unknown: false");
  CHECK(remove(s, "B"), "remove B");
  CHECK(s.count == 0 && s.last_good == 0, "empty again");
  CHECK(s.nets[0].ssid[0] == '\0', "slot cleared");
}

static void pick_prefers_what_the_radio_can_see() {
  Store s; init(s);
  CHECK(pick(s, nullptr, nullptr, 0, -1) == -1, "empty store: nothing to pick");
  add(s, "Home", "h"); add(s, "Work", "w"); add(s, "Cafe", "c");
  note_result(s, 0, true);  // Home last good
  const char* seen[] = {"Neighbor", "Work", "Cafe"};
  const int32_t rssi[] = {-30, -70, -55};
  CHECK(pick(s, seen, rssi, 3, -1) == 2, "Cafe is the strongest VISIBLE saved network");
  const char* seen2[] = {"Work", "Home"};
  const int32_t rssi2[] = {-60, -60};
  CHECK(pick(s, seen2, rssi2, 2, -1) == 0, "tie goes to last good");
  const char* seen3[] = {"Neighbor"};
  const int32_t rssi3[] = {-30};
  CHECK(pick(s, seen3, rssi3, 1, -1) == 0, "nothing saved visible, fresh: last good");
  CHECK(pick(s, nullptr, nullptr, 0, 0) == 1, "no scan, after slot 0: slot 1");
  CHECK(pick(s, nullptr, nullptr, 0, 2) == 0, "round-robin wraps");
  CHECK(pick(s, nullptr, nullptr, 0, 7) == 0, "a stale previous reads as fresh");
}

static void the_blob_round_trips_and_a_torn_one_is_no_store() {
  Store s; init(s);
  add(s, "Home", "hunter2"); add(s, "Bob's;Cafe", ""); note_result(s, 0, false); note_result(s, 1, true);
  uint8_t blob[BLOB_BYTES];
  CHECK(serialize(s, blob, sizeof(blob)) == BLOB_BYTES, "full blob");
  CHECK(blob[0] == 'S' && blob[1] == 'C' && blob[2] == 'V' && blob[3] == 'W', "magic");
  CHECK(blob[4] == VERSION && blob[5] == 2 && blob[6] == 1, "header");
  uint8_t small[100];
  CHECK(serialize(s, small, sizeof(small)) == 0, "too small: 0");

  Store back;
  CHECK(parse(blob, sizeof(blob), back), "parses");
  CHECK(back.count == 2 && back.last_good == 1, "header back");
  CHECK(!std::strcmp(back.nets[0].ssid, "Home") && !std::strcmp(back.nets[0].pass, "hunter2"), "entry 0");
  CHECK(back.nets[0].fails == 1, "fails back");
  CHECK(!std::strcmp(back.nets[1].ssid, "Bob's;Cafe") && back.nets[1].pass[0] == '\0', "entry 1 (open)");

  uint8_t torn[BLOB_BYTES];
  std::memcpy(torn, blob, sizeof(torn));
  torn[20] ^= 0x01;
  CHECK(!parse(torn, sizeof(torn), back), "a flipped byte fails the CRC");
  CHECK(back.count == 0, "and leaves an empty store");
  std::memcpy(torn, blob, sizeof(torn));
  torn[0] = 'X';
  CHECK(!parse(torn, sizeof(torn), back), "wrong magic");
  std::memcpy(torn, blob, sizeof(torn));
  torn[4] = 9;
  CHECK(!parse(torn, sizeof(torn), back), "unknown version (crc aside)");
  CHECK(!parse(blob, sizeof(blob) - 1, back), "short buffer");
  CHECK(!parse(nullptr, sizeof(blob), back), "null");
  // Zero bytes (an erased NVS blob) is no store, not a crash.
  uint8_t zeros[BLOB_BYTES] = {0};
  CHECK(!parse(zeros, sizeof(zeros), back), "all zeros");
}

static void legacy_credentials_fold_in_once() {
  Store s; init(s);
  CHECK(import_legacy(s, "Home", "hunter2"), "imported");
  CHECK(s.count == 1 && s.last_good == 0, "one network");
  CHECK(!import_legacy(s, "Home", "hunter2"), "second import is a no-op");
  CHECK(!import_legacy(s, "", "x"), "an empty legacy ssid is nothing");
  CHECK(!import_legacy(s, nullptr, "x"), "null too");
}

// The fleet's "ever online" across a power cut. It was RAM only, so a Canary
// restarted by the same blackout as its router read the router's two minutes
// of boot as a first boot with a credential problem, and raised setup.
static void the_joined_bit_is_set_only_by_a_join() {
  Store s; init(s);
  CHECK(!any_joined(s), "an empty ring has never joined");

  // A seeded pair (a flasher's, a compiled secret's) has not joined: a wrong
  // one must still open setup.
  CHECK(import_legacy(s, "Seeded", "typed-once"), "seeded");
  CHECK(s.nets[0].flags == 0 && !any_joined(s), "a seeded pair has not joined");
  note_result(s, 0, false); note_result(s, 0, false); note_result(s, 0, false);
  CHECK(!any_joined(s), "a failed join is not a join");

  note_result(s, 0, true);
  CHECK((s.nets[0].flags & FLAG_JOINED) != 0 && any_joined(s), "a join sets it");
  note_result(s, 0, false); note_result(s, 0, false);
  CHECK(any_joined(s), "a later failure does not forget that it worked");

  // add() is the network that just worked (the portal rule): joined.
  Store t; init(t);
  CHECK(add(t, "Home", "hunter2") && any_joined(t), "a network that just worked has joined");
  CHECK(add(t, "Home", "newpass") && (t.nets[0].flags & FLAG_JOINED) != 0, "re-added: still joined");

  // The bit belongs to its network: it moves with it and leaves with it.
  CHECK(import_legacy(t, "Seeded", "typed-once"), "a seeded second network");
  CHECK((t.nets[0].flags & FLAG_JOINED) != 0 && t.nets[1].flags == 0, "one joined, one seeded");
  CHECK(remove(t, "Home"), "remove the joined one");
  CHECK(t.count == 1 && !std::strcmp(t.nets[0].ssid, "Seeded") && !any_joined(t),
        "what is left has never joined");

  // Slots past count are not networks.
  Store u; init(u);
  u.nets[2].flags = FLAG_JOINED;
  CHECK(!any_joined(u), "a stale slot is not a saved network");
  note_result(u, 2, true);
  CHECK(!any_joined(u), "and a verdict for it is ignored");
}

static void the_joined_bit_survives_the_blob_and_the_format_is_unchanged() {
  CHECK(VERSION == 1 && ENTRY_BYTES == 100 && BLOB_BYTES == 412,
        "the bit rides in the byte the blob always carried: no format bump");

  Store s; init(s);
  import_legacy(s, "Seeded", "typed-once");
  add(s, "Home", "hunter2");
  uint8_t blob[BLOB_BYTES];
  CHECK(serialize(s, blob, sizeof(blob)) == BLOB_BYTES, "blob out");
  Store back;
  CHECK(parse(blob, sizeof(blob), back), "blob in (the reboot)");
  CHECK(back.count == 2, "both back");
  CHECK(back.nets[0].flags == 0, "the seeded one is still never-joined");
  CHECK(back.nets[1].flags == FLAG_JOINED, "the joined one is still joined");
  CHECK(any_joined(back), "ever online, after the power cut");

  // A ring that never joined says so after the reboot too.
  Store n; init(n);
  import_legacy(n, "Seeded", "typed-once");
  note_result(n, 0, false);
  CHECK(serialize(n, blob, sizeof(blob)) == BLOB_BYTES && parse(blob, sizeof(blob), back),
        "never-joined round trip");
  CHECK(!any_joined(back), "never joined, after the power cut");

  // A blob written before the bit existed carries 0 there: it reads as never
  // joined until the next association sets it, and it still parses.
  Store old; init(old);
  add(old, "Home", "hunter2");
  old.nets[0].flags = 0;   // what 2.4.19 and earlier wrote
  CHECK(serialize(old, blob, sizeof(blob)) == BLOB_BYTES && parse(blob, sizeof(blob), back),
        "an older blob parses");
  CHECK(back.count == 1 && !any_joined(back), "and reads as never joined");
  note_result(back, 0, true);
  CHECK(any_joined(back), "until the next join");
}

int main() {
  the_blob_has_a_fixed_size();
  add_dedupes_and_marks_last_good();
  a_fifth_network_evicts_the_most_failing_non_last_good();
  remove_shifts_and_keeps_last_good_honest();
  pick_prefers_what_the_radio_can_see();
  the_blob_round_trips_and_a_torn_one_is_no_store();
  legacy_credentials_fold_in_once();
  the_joined_bit_is_set_only_by_a_join();
  the_joined_bit_survives_the_blob_and_the_format_is_unchanged();
  if (g_failures) {
    std::printf("test_wifi_store: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_wifi_store: all passed\n");
  return 0;
}
