// Host tests for firmware/common/network/claim_ticket.h — the claim a phone
// reads over the Bluetooth setup door and spends on the home LAN for the
// pairing receipt, so the bearer token never rides Bluetooth.
//
// The rows that matter most:
//   * a mint is 32 lowercase hex characters of the 16 bytes handed in;
//   * take() spends the claim on a match only (sweep F342): a wrong, short,
//     long or empty value is refused and spends nothing, a thousand of them
//     included, and the right claim then serves, once; before, a mismatch
//     burned the claim, so one stray request from a host on the network
//     ended the phone's one-tap pairing;
//   * the spend is one compare-and-swap of the minted-at word the taker
//     loaded (spend_if_match()), so a spend whose word moved since its load
//     (a re-mint in between) is refused and leaves the new claim pending;
//   * an expired claim is refused and burned, a wrong value against it too;
//     millis()==0 is a real mint;
//   * the 49-day wrap of millis() does not expire a fresh claim;
//   * the compare runs over the whole string whatever the first wrong
//     character, and accepts the phone's upper-case spelling;
//   * a presented string of the wrong length is refused.

#include "../common/network/claim_ticket.h"

#include <cstdio>
#include <cstring>

using namespace canary::net::claim_ticket;

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

static const uint8_t kRaw[RAW_LEN] = {0x00, 0x01, 0x7f, 0x80, 0xab, 0xcd, 0xef, 0x10,
                                      0xff, 0xfe, 0x55, 0xaa, 0x12, 0x34, 0x56, 0x78};
static const char* kHex = "00017f80abcdef10fffe55aa12345678";

static void mint_is_32_lowercase_hex() {
  Ticket t{};
  mint(t, kRaw, 1000);
  CHECK(std::strlen(t.hex) == HEX_LEN, "32 characters, got %zu", std::strlen(t.hex));
  CHECK(std::strcmp(t.hex, kHex) == 0, "hex spelling: %s", t.hex);
  for (size_t i = 0; i < HEX_LEN; ++i) {
    const char c = t.hex[i];
    CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "lowercase hex only (%c)", c);
  }
  CHECK(pending(t, 1000), "pending right after the mint");
  CHECK(pending(t, 1000 + TTL_MS - 1), "pending one ms before the TTL");
  CHECK(!pending(t, 1000 + TTL_MS), "not pending at the TTL");
}

static void nothing_minted_is_nothing_to_take() {
  Ticket t{};
  CHECK(!pending(t, 5), "a zeroed ticket is not pending");
  CHECK(!take(t, kHex, 5), "nothing to take");
}

static void take_consumes_on_match() {
  Ticket t{};
  mint(t, kRaw, 1000);
  CHECK(take(t, kHex, 2000), "the right claim serves");
  CHECK(!pending(t, 2000), "and is spent");
  CHECK(t.hex[0] == '\0', "the hex is wiped");
  CHECK(!take(t, kHex, 2001), "a second take is refused");
}

static void a_mismatch_spends_nothing() {
  Ticket t{};
  mint(t, kRaw, 1000);
  char wrong[HEX_LEN + 1];
  std::strcpy(wrong, kHex);
  wrong[HEX_LEN - 1] = '9';                      // wrong in the last character
  CHECK(!take(t, wrong, 2000), "a wrong guess is refused");
  CHECK(pending(t, 2000), "and spends nothing: the claim is still pending");
  CHECK(t.minted_at_ms == 1000, "the minted-at word is untouched, got %u", (unsigned)t.minted_at_ms);
  CHECK(std::strcmp(t.hex, kHex) == 0, "the hex is untouched");
  // A host that keeps guessing: a thousand distinct wrong values, every
  // character position wrong in turn, spend nothing.
  int served = 0;
  for (int i = 0; i < 1000; ++i) {
    char g[HEX_LEN + 1];
    std::strcpy(g, kHex);
    const size_t at = (size_t)i % HEX_LEN;
    g[at] = (g[at] == '0') ? '1' : '0';
    if (i >= (int)HEX_LEN) g[(at + 7) % HEX_LEN] = (g[(at + 7) % HEX_LEN] == 'f') ? 'e' : 'f';
    if (take(t, g, 2000 + (uint32_t)i)) ++served;
  }
  CHECK(served == 0, "a wrong guess served (%d)", served);
  CHECK(pending(t, 3000), "a thousand wrong guesses spent nothing");
  CHECK(take(t, kHex, 3001), "the right claim then serves");
  CHECK(!pending(t, 3001), "and is spent");
  CHECK(!take(t, kHex, 3002), "a second take of it is refused");
}

static void expired_claim_is_refused_and_burned() {
  Ticket t{};
  mint(t, kRaw, 1000);
  CHECK(!take(t, kHex, 1000 + TTL_MS), "expired at the TTL");
  CHECK(!pending(t, 1000), "burned, whatever the clock says afterwards");
  CHECK(t.minted_at_ms == 0 && t.hex[0] == '\0', "the word cleared and the hex wiped");
  // A wrong value against an expired claim burns it too: an expired claim
  // can never be revived by a later clock, whatever was presented.
  Ticket u{};
  mint(u, kRaw, 1000);
  CHECK(!take(u, "00000000000000000000000000000000", 1000 + TTL_MS + 5), "expired and wrong: refused");
  CHECK(u.minted_at_ms == 0 && u.hex[0] == '\0', "and burned");
}

// The seam the spend is built on: a spend whose minted-at word moved since
// the taker loaded it (here, a re-mint in between) loses its compare-and-
// swap. The new claim stays pending, and its own take serves.
static void a_spend_whose_stamp_changed_is_refused() {
  Ticket t{};
  mint(t, kRaw, 1000);
  const uint32_t seen_old = __atomic_load_n(&t.minted_at_ms, __ATOMIC_ACQUIRE);
  uint8_t other[RAW_LEN];
  for (size_t i = 0; i < RAW_LEN; ++i) other[i] = (uint8_t)(0xA0 + i);
  mint(t, other, 2000);
  char new_hex[HEX_LEN + 1];
  std::strcpy(new_hex, t.hex);
  CHECK(!spend_if_match(t, seen_old, new_hex, 2100), "a spend of the old word is refused");
  CHECK(pending(t, 2100), "and the new claim is still pending");
  CHECK(std::strcmp(t.hex, new_hex) == 0, "its hex untouched");
  CHECK(!spend_if_match(t, 0, new_hex, 2100), "a word of 0 is nothing to spend");
  CHECK(pending(t, 2100), "still pending");
  CHECK(take(t, new_hex, 2200), "take(new hex) serves");
  CHECK(!pending(t, 2200), "and spends it");
  // An old word past its TTL, seen after a re-mint, burns nothing either:
  // the burn is a compare-and-swap of the word it read.
  Ticket u{};
  mint(u, kRaw, 1000);
  const uint32_t stale = u.minted_at_ms;
  mint(u, other, 1000 + TTL_MS + 10);
  CHECK(!spend_if_match(u, stale, kHex, 1000 + TTL_MS + 20), "the stale word's spend is refused");
  CHECK(pending(u, 1000 + TTL_MS + 20), "and the fresh claim is not burned");
}

static void millis_zero_is_a_real_mint() {
  Ticket t{};
  mint(t, kRaw, 0);
  CHECK(t.minted_at_ms == 1, "millis()==0 stored as 1, got %u", (unsigned)t.minted_at_ms);
  CHECK(pending(t, 10), "pending");
  CHECK(take(t, kHex, 10), "and takeable");
}

static void wrap_is_not_expiry() {
  Ticket t{};
  const uint32_t near_wrap = 0xFFFFFFF0u;
  mint(t, kRaw, near_wrap);
  CHECK(pending(t, near_wrap + 60000), "a minute across the wrap: pending");
  CHECK(!pending(t, near_wrap + TTL_MS), "and expired at the TTL across the wrap");
  CHECK(take(t, kHex, near_wrap + 100), "takeable across the wrap");
}

static void the_compare_accepts_upper_case_and_refuses_other_lengths() {
  Ticket t{};
  mint(t, kRaw, 1000);
  char upper[HEX_LEN + 1];
  for (size_t i = 0; i <= HEX_LEN; ++i) {
    const char c = kHex[i];
    upper[i] = (c >= 'a' && c <= 'f') ? (char)(c - 'a' + 'A') : c;
  }
  CHECK(hex_equal(kHex, upper), "the phone's upper-case spelling matches");
  CHECK(!hex_equal(kHex, "00017f80abcdef10fffe55aa1234567"), "31 characters: refused");
  CHECK(!hex_equal(kHex, "00017f80abcdef10fffe55aa123456789"), "33 characters: refused");
  CHECK(!hex_equal(kHex, ""), "empty: refused");
  CHECK(!hex_equal(kHex, nullptr), "null: refused");
  CHECK(!take(t, "", 2000), "an empty presentation is refused");
  CHECK(pending(t, 2000), "and leaves the claim pending");
  CHECK(!take(t, "00017f80abcdef10fffe55aa1234567", 2000), "31 characters: refused by take");
  CHECK(!take(t, "00017f80abcdef10fffe55aa123456789", 2000), "33 characters: refused by take");
  CHECK(!take(t, nullptr, 2000), "null: refused by take");
  CHECK(pending(t, 2000), "and none of them spent the claim");
  CHECK(take(t, upper, 2001), "the phone's upper-case spelling serves");
}

static void a_fresh_mint_replaces_an_outstanding_claim() {
  Ticket t{};
  mint(t, kRaw, 1000);
  uint8_t other[RAW_LEN];
  for (size_t i = 0; i < RAW_LEN; ++i) other[i] = (uint8_t)(0xA0 + i);
  mint(t, other, 2000);
  CHECK(!take(t, kHex, 2500), "the old claim is gone");
  CHECK(pending(t, 2500), "and the new one stays pending after that wrong value");
}

static void wipe_leaves_nothing() {
  Ticket t{};
  mint(t, kRaw, 1000);
  wipe(t);
  CHECK(!pending(t, 1000) && t.hex[0] == '\0', "wiped");
}

int main() {
  mint_is_32_lowercase_hex();
  nothing_minted_is_nothing_to_take();
  take_consumes_on_match();
  a_mismatch_spends_nothing();
  expired_claim_is_refused_and_burned();
  a_spend_whose_stamp_changed_is_refused();
  millis_zero_is_a_real_mint();
  wrap_is_not_expiry();
  the_compare_accepts_upper_case_and_refuses_other_lengths();
  a_fresh_mint_replaces_an_outstanding_claim();
  wipe_leaves_nothing();

  if (g_failures) {
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
  }
  std::printf("ALL claim_ticket TESTS PASSED\n");
  return 0;
}
