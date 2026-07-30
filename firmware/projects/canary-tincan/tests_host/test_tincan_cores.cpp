// Host test for the Tin Can payload cores (include/canary/tincan/): the knock
// codec, the string model, the Ring's delivery honesty, the tie ceremony's
// parent gate, the stamp table, warmer/colder, and the step duel.
//
// Builds standalone with g++ — no Arduino, no LVGL, no radio. Run in CI by the
// "Tin Can payload cores host test" step in .github/workflows/firmware.yml.
// Prints "ALL TINCAN TESTS PASSED" on success (the CI grep makes a silent pass
// impossible to fake). Build from the repo root:
//
//   g++ -std=c++17 -Wall -Wextra -I firmware/projects/canary-tincan/include
//     firmware/projects/canary-tincan/tests_host/test_tincan_cores.cpp -o t && ./t

#include "canary/tincan/duel_model.h"
#include "canary/tincan/knock_codec.h"
#include "canary/tincan/ring_policy.h"
#include "canary/tincan/stamp_set.h"
#include "canary/tincan/string_model.h"
#include "canary/tincan/tie_ceremony.h"
#include "canary/tincan/warmer_colder.h"

#include <cstdio>
#include <cstring>

using namespace canary::tincan;

static int g_fail = 0;
#define CHECK(cond, msg)                                              \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_fail++;                                                      \
    }                                                                \
  } while (0)

// ---------------------------------------------------------------------------
// Knock — the headline feature
// ---------------------------------------------------------------------------

// The rhythm a kid taps is the rhythm the other kid feels. Quantization is
// allowed to move a tap by less than half a quantum and no more.
static void test_knock_roundtrip_preserves_the_rhythm() {
  KnockCapture cap;
  cap.tap(1000);
  cap.tap(1200);   // 200 ms
  cap.tap(1280);   //  80 ms — short-short-long, the classic
  cap.tap(1900);   // 620 ms
  CHECK(cap.knock.taps == 4, "knock: four taps captured");

  uint8_t wire[KNOCK_MAX_WIRE] = {0};
  const size_t n = knock_encode(cap.knock, wire, sizeof(wire));
  CHECK(n == 4, "knock: 4 taps encode to 4 bytes");

  Knock got;
  CHECK(knock_decode(wire, n, got), "knock: decodes");
  CHECK(got.taps == 4, "knock: tap count survives");

  for (uint8_t i = 0; i + 1 < got.taps; i++) {
    const int32_t sent = (int32_t)cap.knock.gap_ms[i];
    const int32_t recv = (int32_t)got.gap_ms[i];
    const int32_t err = sent > recv ? sent - recv : recv - sent;
    CHECK(err <= KNOCK_QUANTUM_MS / 2,
          "knock: each gap survives within half a quantum");
  }
}

// A bouncing finger is one tap, not two — and it is filtered at capture, so
// the sender's own screen shows what will actually be sent.
static void test_knock_debounces_at_capture() {
  KnockCapture cap;
  CHECK(cap.tap(1000), "knock: first tap lands");
  CHECK(!cap.tap(1000 + KNOCK_MIN_GAP_MS - 1), "knock: a bounce is refused");
  CHECK(cap.knock.taps == 1, "knock: the bounce did not become a tap");
  CHECK(cap.tap(1000 + KNOCK_MIN_GAP_MS + 5), "knock: a real second tap lands");
  CHECK(cap.knock.taps == 2, "knock: two taps now");
}

static void test_knock_bounds() {
  KnockCapture cap;
  uint32_t t = 0;
  for (int i = 0; i < 20; i++) {
    cap.tap(t);
    t += 100;
  }
  CHECK(cap.knock.taps == KNOCK_MAX_TAPS, "knock: capped at the max taps");
  CHECK(cap.complete(t), "knock: a full knock reports complete");

  // Outside the window nothing more is accepted.
  KnockCapture slow;
  slow.tap(0);
  CHECK(!slow.tap(KNOCK_WINDOW_MS + 100), "knock: past the window is refused");
}

// A half-understood knock replayed on a child's wrist is worse than none, so
// decoding is total: malformed in, nothing out.
static void test_knock_decode_rejects_malformed() {
  Knock k;
  uint8_t zero[1] = {0};
  CHECK(!knock_decode(zero, 1, k), "knock: zero taps refused");

  uint8_t too_many[1] = {KNOCK_MAX_TAPS + 1};
  CHECK(!knock_decode(too_many, 1, k), "knock: over-max tap count refused");

  // Length must match the count exactly — no trailing slack to hide data in.
  uint8_t slack[5] = {3, 5, 5, 0, 0};
  CHECK(!knock_decode(slack, 5, k), "knock: trailing bytes refused");

  uint8_t bad_gap[3] = {3, 0, 5};
  CHECK(!knock_decode(bad_gap, 3, k), "knock: a zero gap is refused");

  uint8_t huge_gap[3] = {3, 64, 5};
  CHECK(!knock_decode(huge_gap, 3, k), "knock: an out-of-range gap is refused");

  CHECK(!knock_decode(nullptr, 3, k), "knock: null refused");
}

static void test_knock_playback_schedule() {
  Knock k;
  k.taps = 3;
  k.gap_ms[0] = 200;
  k.gap_ms[1] = 80;

  KnockPlayback p;
  CHECK(knock_playback(k, p), "knock: playback plan built");
  CHECK(p.pulses == 3, "knock: three pulses");
  CHECK(p.at_ms[0] == 0, "knock: first pulse at zero");
  CHECK(p.at_ms[1] == 200, "knock: second pulse after the first gap");
  CHECK(p.at_ms[2] == 280, "knock: gaps accumulate");
  CHECK(k.span_ms() == 280, "knock: span matches the schedule");
}

// ---------------------------------------------------------------------------
// String model
// ---------------------------------------------------------------------------

// The cooldown is shown to the SENDER. A block list would teach a kid to cut a
// sibling off; a silent drop would teach them the device is broken.
static void test_knock_cooldown_is_a_sender_side_brake() {
  String s;
  s.state = StringState::Taut;
  CHECK(s.may_knock(1000), "string: a fresh string may be knocked");

  s.on_knock_sent(1000);
  CHECK(!s.may_knock(1000), "string: immediately after, the brake is on");
  CHECK(s.knock_cooldown_left(1000) == KNOCK_COOLDOWN_MS,
        "string: the sender is told exactly how long is left");
  CHECK(!s.may_knock(1000 + KNOCK_COOLDOWN_MS - 1), "string: still braked");
  CHECK(s.may_knock(1000 + KNOCK_COOLDOWN_MS), "string: brake releases");
  CHECK(s.knock_cooldown_left(1000 + KNOCK_COOLDOWN_MS) == 0,
        "string: the ring empties");
}

// Both kids holding at once is the moment the product is about.
static void test_tug_both_holding() {
  String s;
  s.state = StringState::Taut;
  s.i_hold = true;
  CHECK(!s.both_holding(1000), "tug: one hand is not both");

  s.on_peer_hold(1000);
  CHECK(s.both_holding(1000), "tug: two hands");
  CHECK(string_tension(s, 1000) == 255, "tug: the string draws fully taut");

  // A hold expires on its own — never latch it waiting for a release that may
  // never come.
  CHECK(!s.both_holding(1000 + TUG_HOLD_TIMEOUT_MS + 1),
        "tug: a stale peer hold lapses");
}

// A slack string still draws something. Drawing nothing would read as "you
// have no sister", which is a different and much worse message than "she is
// out of range".
static void test_slack_still_draws() {
  String s;
  s.state = StringState::Slack;
  CHECK(string_tension(s, 0) > 0, "string: a slack string is still drawn");

  String cut;
  cut.state = StringState::Cut;
  CHECK(string_tension(cut, 0) == 0, "string: a cut string is not drawn");
}

// A cut string never comes back — not on reachability, not on anything.
static void test_cut_is_terminal() {
  String s;
  s.state = StringState::Taut;
  s.cut();
  CHECK(s.state == StringState::Cut, "string: cut");
  s.on_reachable(true);
  CHECK(s.state == StringState::Cut, "string: reachability cannot un-cut it");
  CHECK(!s.tied(), "string: a cut string is not tied");

  // Nor may an unconfirmed string become Taut just because the peer is on air.
  String p;
  p.state = StringState::Pending;
  p.on_reachable(true);
  CHECK(p.state == StringState::Pending, "string: Pending is not promoted");
}

static void test_string_book_caps() {
  StringBook b;
  for (size_t i = 0; i < TINCAN_MAX_STRINGS; i++) {
    String* s = b.alloc();
    CHECK(s != nullptr, "book: slot available");
    s->state = StringState::Taut;
    s->session_id = (uint16_t)(100 + i);
  }
  CHECK(b.full(), "book: full at the cap");
  CHECK(b.alloc() == nullptr, "book: refuses beyond the cap");
  CHECK(b.count_taut() == TINCAN_MAX_STRINGS, "book: counts taut strings");
  CHECK(b.find_by_session(103) != nullptr, "book: finds by session");
  CHECK(b.find_by_session(999) == nullptr, "book: misses cleanly");
}

// ---------------------------------------------------------------------------
// Ring — the honesty that matters most
// ---------------------------------------------------------------------------

// There is no "Sent". Handing a frame to a radio is not something to show a
// parent, because they will read it as "delivered".
static void test_ring_delivery_states() {
  RingAttempt a;
  a.ring = Ring::AnswerMe;
  a.sent_ms = 1000;

  CHECK(a.state(1000) == DeliveryState::InFlight, "ring: starts in flight");
  CHECK(a.should_retry(1000), "ring: retries while in flight");

  a.on_receipt(1500);
  CHECK(a.state(1500) == DeliveryState::DeliveredNoAck,
        "ring: a receipt without a tap is its own state");
  CHECK(!a.should_retry(1500), "ring: stops retrying once it landed");

  a.on_ack(1800);
  CHECK(a.state(1800) == DeliveryState::DeliveredAcked, "ring: acked");
  CHECK(a.failure(1800) == NotDeliveredReason::None, "ring: no failure reason");
}

// A Ring expires. One landing forty minutes late, after a parent has already
// walked out to find their kid, is worse than one that never landed.
static void test_ring_expires_and_says_why() {
  RingAttempt a;
  a.sent_ms = 0;
  const uint32_t late = RING_EXPIRY_MS + 1;

  CHECK(a.expired(late), "ring: expires on schedule");
  CHECK(a.state(late) == DeliveryState::NotDelivered, "ring: not delivered");
  CHECK(a.failure(late) == NotDeliveredReason::Expired,
        "ring: a NotDelivered state always carries a reason");
  CHECK(!a.should_retry(late), "ring: stops retrying past the window");

  // A receipt arriving after expiry does not resurrect it.
  a.on_receipt(late + 100);
  CHECK(a.state(late + 200) == DeliveryState::NotDelivered,
        "ring: a late receipt does not count");

  // An explicit give-up carries the reason the parent can act on.
  RingAttempt b;
  b.sent_ms = 0;
  b.on_give_up(NotDeliveredReason::StringSlack);
  CHECK(b.state(10) == DeliveryState::NotDelivered, "ring: gave up");
  CHECK(b.failure(10) == NotDeliveredReason::StringSlack,
        "ring: says 'the string was slack', not just 'failed'");
  CHECK(std::strcmp(not_delivered_reason_token(b.failure(10)),
                    "string-slack") == 0,
        "ring: the reason has a stable token");
}

// A give-up must not rewrite history for a Ring that already landed.
static void test_ring_give_up_cannot_unland() {
  RingAttempt a;
  a.sent_ms = 0;
  a.on_receipt(100);
  a.on_give_up(NotDeliveredReason::NoRoute);
  CHECK(a.state(200) == DeliveryState::DeliveredNoAck,
        "ring: a late give-up does not erase a real delivery");
}

// The Ring is the ONLY thing that may break quiet, and AllClear — reassurance
// — deliberately is not an interrupt.
static void test_ring_quiet_override() {
  CHECK(ring_should_wake(Ring::ComeInside, true),
        "ring: come-inside breaks quiet");
  CHECK(ring_should_wake(Ring::AnswerMe, true), "ring: answer-me breaks quiet");
  CHECK(!ring_should_wake(Ring::AllClear, true),
        "ring: reassurance does not interrupt");
  CHECK(ring_should_wake(Ring::AllClear, false),
        "ring: reassurance still shows when not quiet");

  CHECK(ring_needs_ack(Ring::AnswerMe), "ring: answer-me needs a tap");
  CHECK(!ring_needs_ack(Ring::Dinner),
        "ring: everything else does not, so the ack keeps its meaning");

  CHECK(ring_valid(0) && ring_valid((uint8_t)Ring::Count - 1),
        "ring: valid ids accepted");
  CHECK(!ring_valid((uint8_t)Ring::Count), "ring: unknown id refused");
}

// The receiving half of "late is not a weaker kind of on-time".
static void test_ring_receiver_drops_stale() {
  CHECK(ring_should_render(1000), "ring: a fresh Ring renders");
  CHECK(!ring_should_render(RING_EXPIRY_MS + 1),
        "ring: a Ring older than its own window is not shown at all");
}

// ---------------------------------------------------------------------------
// Tie ceremony — the stranger gate
// ---------------------------------------------------------------------------

// Without a parent-opened window the tie path is not reachable. This is what
// makes "you cannot tie a string in a playground" structural.
static void test_tie_requires_a_parent_window() {
  TieCeremony c;
  CHECK(!c.may_tie(1000), "tie: no window, no tie");
  CHECK(!c.begin_exchange(1000, true), "tie: exchange refused with no window");
  CHECK(c.failure == TieFailure::NoWindow, "tie: and it says why");

  TieCeremony d;
  d.open_window(1000);
  CHECK(d.may_tie(1000), "tie: a parent opened the gate");
  CHECK(d.may_tie(1000 + TIE_WINDOW_MS - 1), "tie: still open");
  CHECK(!d.may_tie(1000 + TIE_WINDOW_MS), "tie: the window closes on time");

  d.step(1000 + TIE_WINDOW_MS + 1);
  CHECK(d.stage == TieStage::Failed, "tie: an expired window fails the ceremony");
  CHECK(d.failure == TieFailure::WindowExpired, "tie: reason recorded");
}

// The knot is what makes a man-in-the-middle visible to a six-year-old.
static void test_knot_derivation_and_mismatch() {
  uint8_t sas_a[4] = {0xAB, 0xCD, 0x00, 0x00};
  uint8_t sas_b[4] = {0xAB, 0xCD, 0xFF, 0xFF};
  Knot ka, kb;
  CHECK(knot_from_secret(sas_a, sizeof(sas_a), ka), "knot: derives");
  CHECK(knot_from_secret(sas_b, sizeof(sas_b), kb), "knot: derives");
  CHECK(ka.equals(kb), "knot: same first two bytes give the same four glyphs");

  CHECK(ka.glyph[0] == 0xA && ka.glyph[1] == 0xB, "knot: nibbles split in order");
  CHECK(ka.glyph[2] == 0xC && ka.glyph[3] == 0xD, "knot: second byte too");
  for (size_t i = 0; i < KNOT_GLYPHS; i++) {
    CHECK(ka.glyph[i] < KNOT_ALPHABET, "knot: every glyph is in the alphabet");
  }

  uint8_t different[2] = {0x12, 0x34};
  Knot kc;
  knot_from_secret(different, sizeof(different), kc);
  CHECK(!ka.equals(kc), "knot: different secrets give different knots");

  uint8_t tiny[1] = {0x01};
  Knot kd;
  CHECK(!knot_from_secret(tiny, sizeof(tiny), kd),
        "knot: too little entropy is a derivation bug, not a short knot");

  // A mismatch ends the ceremony — no retry to grind against.
  TieCeremony c;
  c.open_window(0);
  c.begin_exchange(0, true);
  c.present_knot(ka, 0);
  CHECK(!c.knots_match(false), "tie: a human said the knots differ");
  CHECK(c.stage == TieStage::Failed, "tie: mismatch ends it");
  CHECK(c.failure == TieFailure::KnotMismatch, "tie: reason is the mismatch");
}

// The hold is measured from the LATER of the two presses, so one kid pressing
// early cannot shorten the shared three seconds.
static void test_tie_confirm_is_two_handed() {
  TieCeremony c;
  c.open_window(0);
  c.begin_exchange(0, true);
  Knot k;
  uint8_t sas[2] = {0x11, 0x22};
  knot_from_secret(sas, sizeof(sas), k);
  c.present_knot(k, 0);
  c.knots_match(true);

  c.hold_start(1000);
  CHECK(!c.confirmed(1000 + TIE_CONFIRM_HOLD_MS),
        "tie: one kid holding is never enough");

  c.peer_hold_start(1500);
  CHECK(!c.confirmed(1000 + TIE_CONFIRM_HOLD_MS),
        "tie: the clock runs from the later press, not the earlier");
  CHECK(c.confirmed(1500 + TIE_CONFIRM_HOLD_MS),
        "tie: three seconds together confirms");

  CHECK(c.step(1500 + TIE_CONFIRM_HOLD_MS) == TieStage::Tied, "tie: tied");

  // Presses too far apart are not "together".
  TieCeremony d;
  d.open_window(0);
  d.begin_exchange(0, true);
  d.present_knot(k, 0);
  d.knots_match(true);
  d.hold_start(0);
  d.peer_hold_start(TIE_CONFIRM_SKEW_MS + 100);
  CHECK(!d.confirmed(TIE_CONFIRM_SKEW_MS + 100 + TIE_CONFIRM_HOLD_MS),
        "tie: presses too far apart are not simultaneous");

  // A released hold breaks the confirm.
  TieCeremony e;
  e.open_window(0);
  e.begin_exchange(0, true);
  e.present_knot(k, 0);
  e.knots_match(true);
  e.hold_start(0);
  e.peer_hold_start(0);
  e.hold_release();
  CHECK(!e.confirmed(TIE_CONFIRM_HOLD_MS), "tie: letting go breaks it");
}

static void test_tie_refuses_when_full() {
  TieCeremony c;
  c.open_window(0);
  CHECK(!c.begin_exchange(0, /*have_slot=*/false), "tie: refuses with no slot");
  CHECK(c.failure == TieFailure::NoSlot, "tie: reason is the full table");
}

// ---------------------------------------------------------------------------
// Stamps
// ---------------------------------------------------------------------------

// Ids are a wire contract. A reordered table would silently turn one child's
// "sandwich" into another's "bedtime" across a firmware version boundary.
static void test_stamp_table_matches_the_enum() {
  const StampDef* t = stamp_table();
  for (size_t i = 0; i < stamp_count(); i++) {
    CHECK((uint8_t)t[i].id == (uint8_t)i, "stamp: table order matches the enum");
    CHECK(t[i].token != nullptr && t[i].token[0] != '\0',
          "stamp: every stamp has a token");
  }
  CHECK(stamp_count() == 16, "stamp: sixteen, and only sixteen");
  CHECK(std::strcmp(stamp_token(Stamp::Sandwich), "sandwich") == 0,
        "stamp: token lookup");
}

// An unknown stamp is refused, not drawn as a placeholder. A future firmware's
// stamp 16 must not make today's watch pretend it received something.
static void test_stamp_wire() {
  uint8_t b[1];
  CHECK(stamp_encode(Stamp::Heart, b, 1) == 1, "stamp: encodes to one byte");
  Stamp got;
  CHECK(stamp_decode(b, 1, got) && got == Stamp::Heart, "stamp: round-trips");

  uint8_t unknown[1] = {(uint8_t)Stamp::Count};
  CHECK(!stamp_decode(unknown, 1, got), "stamp: unknown id refused");

  uint8_t two[2] = {0, 0};
  CHECK(!stamp_decode(two, 2, got), "stamp: wrong length refused");
  CHECK(stamp_encode(Stamp::Count, b, 1) == 0, "stamp: refuses to encode Count");
}

// ---------------------------------------------------------------------------
// Warmer / colder
// ---------------------------------------------------------------------------

// The refusal that defines the feature: a fingerprint is keyed by household
// Canary fp4s and there is no field an AP identifier could live in.
static void test_fingerprint_holds_only_household_anchors() {
  CHECK(sizeof(AnchorReading) == sizeof(uint16_t) + sizeof(int16_t),
        "hunt: an anchor is exactly an fp4 and an RSSI — no room for a BSSID");

  Fingerprint f;
  CHECK(!f.observe(0, -50), "hunt: fp4 zero is the no-such-node sentinel");
  CHECK(f.count == 0, "hunt: and it was not recorded");

  for (int i = 1; i <= (int)HUNT_MAX_ANCHORS; i++) {
    CHECK(f.observe((uint16_t)i, -50), "hunt: anchors accepted up to the cap");
  }
  CHECK(!f.observe(999, -50), "hunt: the cap holds");
  CHECK(f.count == HUNT_MAX_ANCHORS, "hunt: capped");

  // Re-observing updates rather than duplicating.
  CHECK(f.observe(1, -80), "hunt: re-observe accepted");
  CHECK(f.count == HUNT_MAX_ANCHORS, "hunt: no duplicate slot");
  CHECK(f.get(1)->rssi == -80, "hunt: the reading updated");
}

static void test_hunt_heat_and_honesty() {
  Fingerprint hider, seeker;
  hider.observe(0xA1, -40);
  hider.observe(0xA2, -60);
  hider.observe(0xA3, -75);

  // Too little overlap: say "unknown" rather than let a kid chase noise.
  seeker.observe(0xA1, -40);
  int32_t db = 0;
  size_t overlap = 0;
  CHECK(!hunt_distance_db(hider, seeker, db, overlap),
        "hunt: one anchor in common is not enough to be honest");
  CHECK(overlap == 1, "hunt: overlap reported anyway");

  // Standing where the hider stood.
  Fingerprint same = hider;
  CHECK(hunt_distance_db(hider, same, db, overlap), "hunt: full overlap");
  CHECK(db == 0, "hunt: identical readings are zero apart");
  CHECK(hunt_heat(db, false) == HuntHeat::Found, "hunt: found");

  // Far away.
  Fingerprint far;
  far.observe(0xA1, -90);
  far.observe(0xA2, -30);
  far.observe(0xA3, -40);
  CHECK(hunt_distance_db(hider, far, db, overlap), "hunt: overlap");
  CHECK(hunt_heat(db, false) == HuntHeat::Cold, "hunt: cold");
}

// Kids stop moving the instant they win; a readout that immediately un-wins
// them is cruel.
static void test_hunt_hysteresis() {
  const int32_t wobble = (HUNT_FOUND_DB + HUNT_FOUND_RELEASE_DB) / 2;
  CHECK(hunt_heat(wobble, /*was_found=*/true) == HuntHeat::Found,
        "hunt: a small wobble does not un-win");
  CHECK(hunt_heat(wobble, /*was_found=*/false) != HuntHeat::Found,
        "hunt: but you cannot win by wobbling into it");
}

// A fingerprint is game-scoped: end() destroys it, so it can never become a
// reusable correlation token.
static void test_hunt_round_is_destroyed_at_the_end() {
  Fingerprint hider;
  hider.observe(0xA1, -40);
  hider.observe(0xA2, -60);

  Hunt h;
  h.begin(hider);
  CHECK(h.active, "hunt: round running");
  CHECK(h.target.count == 2, "hunt: snapshot held for the round");

  h.end();
  CHECK(!h.active, "hunt: round over");
  CHECK(h.target.count == 0, "hunt: the snapshot is destroyed, not just parked");
  CHECK(h.heat == HuntHeat::Unknown, "hunt: verdict cleared too");
}

static void test_hunt_wire() {
  Fingerprint f;
  f.observe(0xBEEF, -55);
  f.observe(0x1234, -70);

  uint8_t buf[HUNT_WIRE_MAX] = {0};
  const size_t n = hunt_encode(f, buf, sizeof(buf));
  CHECK(n == 1 + 2 * 3, "hunt: three bytes per anchor");

  Fingerprint got;
  CHECK(hunt_decode(buf, n, got), "hunt: decodes");
  CHECK(got.count == 2, "hunt: anchor count survives");
  CHECK(got.get(0xBEEF) && got.get(0xBEEF)->rssi == -55, "hunt: reading survives");
  CHECK(got.get(0x1234) && got.get(0x1234)->rssi == -70, "hunt: and the other");

  uint8_t bad[2] = {5, 0};
  CHECK(!hunt_decode(bad, 2, got), "hunt: length must match the count");

  uint8_t over[1] = {HUNT_MAX_ANCHORS + 1};
  CHECK(!hunt_decode(over, 1, got), "hunt: over-cap count refused");
}

// ---------------------------------------------------------------------------
// Step duel
// ---------------------------------------------------------------------------

// Yesterday is gone at midnight. There is no streak to break.
static void test_duel_resets_daily_with_no_history() {
  Duel d;
  d.roll_day(100);
  d.primed = true;
  d.last_credit_ms = 0;
  const uint32_t mine = d.credit(500, 1000);
  d.on_peer_steps(mine > 0 ? mine - 1 : 0);
  CHECK(d.my_steps > 0, "duel: steps credited");
  CHECK(d.standing() == DuelStanding::Ahead, "duel: ahead");

  CHECK(d.roll_day(101), "duel: a new day rolls over");
  CHECK(d.my_steps == 0 && d.peer_steps == 0, "duel: both sides reset");
  CHECK(!d.peer_reported, "duel: and the peer's report is forgotten");
  CHECK(d.standing() == DuelStanding::Unknown,
        "duel: silence is 'unknown', never an implied win");
  CHECK(d.gap() == 0, "duel: no invented lead against silence");
  CHECK(!d.roll_day(101), "duel: the same day does not roll again");
}

// The first batch after boot or a wake spans an unknown window, so crediting it
// at full value would reward being switched off.
static void test_duel_first_batch_is_dropped() {
  Duel d;
  CHECK(d.credit(5000, 1000) == 0, "duel: the first batch is not credited");
  CHECK(d.my_steps == 0, "duel: nothing banked");
}

// The cheat is admitted, not policed — but the rate ceiling keeps the number
// from becoming absurd enough to make the game feel fake.
static void test_duel_rate_and_cap() {
  Duel d;
  d.credit(0, 0);  // prime
  const uint32_t credited = d.credit(10000, 1000);  // one second of shaking
  CHECK(credited <= DUEL_MAX_STEPS_PER_SEC,
        "duel: a shaken wrist is flattened to the rate ceiling");
  CHECK(d.my_steps == credited, "duel: only the credited number is banked");

  Duel c;
  c.primed = true;
  c.my_steps = DUEL_DAILY_CAP;
  c.last_credit_ms = 0;
  CHECK(c.credit(100, 10000) == 0, "duel: nothing credited past the daily cap");
  CHECK(c.capped(), "duel: and the UI can say so");

  // Slow walking must not vanish to integer division on short polls.
  Duel slow;
  slow.primed = true;
  slow.last_credit_ms = 0;
  CHECK(slow.credit(1, 10) == 1, "duel: a single step on a short poll counts");
}

static void test_duel_peer_report_is_clamped() {
  Duel d;
  d.on_peer_steps(DUEL_DAILY_CAP * 10);
  CHECK(d.peer_steps == DUEL_DAILY_CAP,
        "duel: a peer cannot report an impossible number");
}

int main() {
  std::printf("Tin Can payload core host tests\n");

  test_knock_roundtrip_preserves_the_rhythm();
  test_knock_debounces_at_capture();
  test_knock_bounds();
  test_knock_decode_rejects_malformed();
  test_knock_playback_schedule();

  test_knock_cooldown_is_a_sender_side_brake();
  test_tug_both_holding();
  test_slack_still_draws();
  test_cut_is_terminal();
  test_string_book_caps();

  test_ring_delivery_states();
  test_ring_expires_and_says_why();
  test_ring_give_up_cannot_unland();
  test_ring_quiet_override();
  test_ring_receiver_drops_stale();

  test_tie_requires_a_parent_window();
  test_knot_derivation_and_mismatch();
  test_tie_confirm_is_two_handed();
  test_tie_refuses_when_full();

  test_stamp_table_matches_the_enum();
  test_stamp_wire();

  test_fingerprint_holds_only_household_anchors();
  test_hunt_heat_and_honesty();
  test_hunt_hysteresis();
  test_hunt_round_is_destroyed_at_the_end();
  test_hunt_wire();

  test_duel_resets_daily_with_no_history();
  test_duel_first_batch_is_dropped();
  test_duel_rate_and_cap();
  test_duel_peer_report_is_clamped();

  if (g_fail) {
    std::printf("\n%d TINCAN TEST(S) FAILED\n", g_fail);
    return 1;
  }
  std::printf("\nALL TINCAN TESTS PASSED\n");
  return 0;
}
