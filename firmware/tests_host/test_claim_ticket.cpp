// Host tests for firmware/common/network/claim_ticket.h — the claim a phone
// reads over the Bluetooth setup door and spends on the home LAN for the
// pairing receipt, so the bearer token never rides Bluetooth.
//
// The rows that matter most:
//   * a mint is 32 lowercase hex characters of the 16 bytes handed in;
//   * take() consumes the claim on a match AND on a mismatch (one guess per
//     join), and a second take of anything is refused;
//   * an expired claim is refused and burned; millis()==0 is a real mint;
//   * the 49-day wrap of millis() does not expire a fresh claim;
//   * the compare runs over the whole string whatever the first wrong
//     character, and accepts the phone's upper-case spelling;
//   * a presented string of the wrong length is refused (and still burns).

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

static void take_consumes_on_mismatch() {
  Ticket t{};
  mint(t, kRaw, 1000);
  char wrong[HEX_LEN + 1];
  std::strcpy(wrong, kHex);
  wrong[HEX_LEN - 1] = '9';                      // wrong in the last character
  CHECK(!take(t, wrong, 2000), "a wrong guess is refused");
  CHECK(!pending(t, 2000), "and burns the claim");
  CHECK(!take(t, kHex, 2001), "the right claim no longer serves — one guess per join");
}

static void expired_claim_is_refused_and_burned() {
  Ticket t{};
  mint(t, kRaw, 1000);
  CHECK(!take(t, kHex, 1000 + TTL_MS), "expired at the TTL");
  CHECK(!pending(t, 1000), "burned, whatever the clock says afterwards");
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
  CHECK(!pending(t, 2000), "and still burns the claim");
}

static void a_fresh_mint_replaces_an_outstanding_claim() {
  Ticket t{};
  mint(t, kRaw, 1000);
  uint8_t other[RAW_LEN];
  for (size_t i = 0; i < RAW_LEN; ++i) other[i] = (uint8_t)(0xA0 + i);
  mint(t, other, 2000);
  CHECK(!take(t, kHex, 2500), "the old claim is gone");
  CHECK(!pending(t, 2500), "and the new one was burned by that wrong guess");
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
  take_consumes_on_mismatch();
  expired_claim_is_refused_and_burned();
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
