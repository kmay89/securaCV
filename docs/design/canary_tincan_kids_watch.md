# Design: the Tin Can — a kids' wrist Canary on the Waveshare AMOLED 2.06

**Status:** design / RFC — **no code yet.** Written to be argued with before a
line is committed, per the [`lan_baby_monitor`](lan_baby_monitor.md) /
[`alert_relay`](alert_relay.md) precedent.
· **Date:** 2026-07-30 · **Owner:** TBD
· **Target board:** Waveshare **ESP32-S3-Touch-AMOLED-2.06** ("the watch board")

> *"Design a firmware idea around the watch board and make it completely for
> kids. Research the tin can concept and what we could pull off between
> siblings on the same LAN. Scope it to age and zero liability, make it fun
> for kids, and let a parent send a 'come inside' alert on the LAN."*

**The one-sentence version:** two kids' watches **tie a string** — a
parent-witnessed, LAN-only pairing — and after that they can *knock*, *tug*,
*stamp* and *doodle* at each other with no voice, no text, no location and no
internet; the parent gets exactly one privileged message, **the Ring**
("come inside"), which is honest about whether it arrived.

The design's spine is a refusal: **the Tin Can carries no speech.** That is
what makes it fun (kids invent codes), what makes it cheap to defend
(there is no content to moderate, record, or leak), and what keeps it inside
[Invariant I](../../spec/invariants.md) instead of asking for a carve-out the
way the baby monitor did.

---

## 1 · The hardware, honestly

The board is a wearable-shaped ESP32-S3 dev board — a watch *body*, not a
watch *product*.

| Part | What's on it | What we do with it |
|---|---|---|
| MCU | ESP32-S3R8, dual LX7 @ 240 MHz, 8 MB PSRAM, 32 MB flash | plenty; the OTA A/B story from [`canary-ota`](../../firmware/projects/canary-ota) fits with room to spare |
| Display | 2.06" AMOLED **410 × 502**, QSPI, CO5300 driver | the whole UI. AMOLED = true black, so a mostly-black kid face costs almost nothing |
| Touch | **FT3168** @ 0x38 — the store page says CST9220, every vendor sketch drives an FT3168 ([resolved](#hardware-notes-settled-at-registration)) | taps, holds, one-finger doodle |
| IMU | QMI8658 6-axis | step duel, wake-on-raise, knock-by-wrist-tap |
| RTC | PCF85063 | the clock keeps time with the radio off |
| PMU | AXP2101 + Li-po | battery telemetry, the sleep budget in §8 |
| Audio | ES8311 codec, ES7210 AEC, **dual digital mics** | **deliberately never initialized.** See §3.1 |
| Radio | 2.4 GHz Wi-Fi + BLE 5 | ESP-NOW strings, LAN Ring, BLE tie ceremony |
| Breakout | 1 × I²C, 1 × UART, USB pads | where the missing part goes ↓ |

### 1.1 The one thing the board does not have

**There is no vibration motor.** The Waveshare sample tree drives display,
touch, IMU, RTC, PMU, SD and the audio codec — no haptic driver, and the
schematic has no LRA/ERM.

This is load-bearing, not a nitpick. The entire emotional core of a tin can is
that it is *quiet and private*: a tug you feel and nobody else notices. A
watch that can only answer with sound and light is a watch that gets confiscated
in a classroom and is useless under a blanket at 8 p.m. So:

- **Wave 0 adds haptics.** A **DRV2605L + LRA** on the exposed I²C port is the
  minimum viable Tin Can. It is a two-wire add-on, it has a mature driver, and
  its effect library is exactly the vocabulary §5 needs (sharp click, ramp-up,
  double-tick).
- **The degraded fallback is named, not hidden.** With no motor a knock is a
  full-screen AMOLED flash, and — where an output transducer is actually
  present — the
  [`chime`](../../firmware/projects/canary-display/include/canary/hal/chime.h)
  voice. The board does have an audio output path (see below), but whether a
  transducer is fitted varies by revision, so the firmware **probes rather than
  assumes**: a device that claims it can be heard and can't is exactly the
  surprise that leaves a kid waiting for an answer that already arrived. The
  boot screen says *"no buzzer fitted — knocks will be seen, not felt."* Honest
  degradation is the house style
  ([`failure_semantics.md`](../failure_semantics.md)).

### Hardware notes settled at registration

Two of §12's open questions were answered by reading the vendor tree
([waveshareteam/ESP32-S3-Touch-AMOLED-2.06](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06))
rather than the store page, while transcribing the pin map for
[`waveshare-esp32s3-amoled206`](../../firmware/boards/waveshare-esp32s3-amoled206/README.md):

- **Touch is FT3168 @ 0x38.** Waveshare's store copy says CST9220; every sketch
  in the vendor tree drives an FT3168 through `Arduino_DriveBus`. Vendor code
  beats vendor marketing — but this is still the first thing to confirm on real
  hardware, because a wrong touch driver is a dead watch, not a degraded one.
- **There is an audio output path.** `08_ES8311.ino` plays PCM through
  `i2s.setPins(41, 45, 40, 42, 16)` after driving GPIO46 high — a power-amp
  enable. So the codec can make sound; §12.5's "no speaker fitted" was too
  strong. What remains genuinely open is whether a *transducer* is populated on
  a given revision, which is why the runtime probes.

Still unresolved and still the thing that matters most: **there is no haptic
motor anywhere in the vendor tree**, and no LRA/ERM on the schematic.

### 1.2 Board registry

New entry `waveshare-esp32s3-amoled206`, tier **compile-tested** until a bench
pass, `pio_board = esp32-s3-devkitc-1` with an 8 MB octal PSRAM / 32 MB flash
board JSON. Baseline caps: `HAS_CAMERA 0`, `HAS_MICROPHONE 0` *(present on the
board, refused by this firmware — see §3.1; the flag describes what the
firmware may touch)*, `HAS_SD_CARD 1`, `HAS_PSRAM 1`, `HAS_USB_CDC 1`,
`HAS_WIFI 1`, `HAS_BLE 1`, plus `HAS_HAPTIC` (0 or 1 by build) and
`HAS_AMOLED 1`. Registry rules per
[`check_board_registry.py`](../../firmware/scripts/check_board_registry.py).

---

## 2 · Research: what a tin can telephone actually teaches

A tin-can telephone is two cans joined by a string. Speaking into one can makes
its base act as a diaphragm; the vibration becomes a **longitudinal wave in the
string** — a travelling variation in tension — and the far can's base replays
it. It only works if the string is **taut**. Slack string, no call. Sound moves
through the solid better than through air, which is why a whisper carries
further down a string than across a room.

That is a toy, and it is also a surprisingly complete specification. Five
properties fall out of it, and every one of them is a feature we want:

| Tin can property | What it becomes here |
|---|---|
| **The string is physical.** You can see it, hold it, cut it. | A **string** is an explicit, visible, parent-witnessed pairing between exactly two watches. It appears on-screen as a drawn line. Untying it is one gesture and it is instant. |
| **It only works taut.** | Range and link state are *the product*, not an error dialog. The line on screen is drawn **taut** when the peer is reachable and **slack** when it isn't. A kid learns "she's out of range" from a picture, in a second, without reading a word. |
| **Point to point.** One string, two cans. No party line. | Strings are 1:1 and few (cap: 6). This is not a social network, has no group chat, no broadcast, and therefore no pile-on. |
| **Anyone can cut it.** | Either kid can cut a string. So can a parent. Nothing negotiates, nothing appeals, nothing is stored about it. |
| **It carries vibration, not language.** | The wire format carries **rhythm, pressure and pictograms** — never speech and never free text. §3.1. |

The last row is the whole design. A tin can does not have a vocabulary; it has
a *feel*. Two kids on a real tin can spend the first ten minutes shouting and
the next hour tapping the can and giggling — because a shared secret code you
invented is better than a phone call. We are building the second hour.

### 2.1 Prior art, and where it leaves a gap

- **Relay** (screen-free kid "phone", push-to-talk over 4G, GPS) — proved the
  category and the parent appetite. It is a carrier product with a subscription,
  a cloud, voice recordings and location. Every one of those is a liability
  surface we are declining.
- **Yoto / Toniebox** — proved that "no screen, no ads, no mic, no camera" is a
  *selling point* to parents, not a compromise. Yoto markets the absences.
  We market the absences too, and we have the receipts to back them
  ([`BRAND.md`](../BRAND.md): *can't*, not *won't*).
- **Walkie-talkies** — the actual thing siblings already use. They're loud,
  they're one-to-many, they don't work between floors of some houses, and they
  broadcast to any stranger on the channel.

The gap: **nothing does quiet, private, house-scoped, zero-account sibling
signalling.** That's the Tin Can.

---

## 3 · The scope call: what this is, and the things it will never be

### 3.1 The refusal list

This section is the design. Everything else is decoration on top of it. Each
line is a *structural* refusal — the code to do the thing is absent, not
disabled.

1. **No voice. Ever.** The mics and codec are never initialized; the audio
   capture path is not compiled in. Same posture, same reason as
   [`display_mic_variant.md`](../hardware/display_mic_variant.md) and
   [`lan_baby_monitor.md` §2.2](lan_baby_monitor.md) — *"no audio can be
   streamed"* is one of the strongest honesty claims the project makes, and a
   kids' walkie-talkie is exactly the product that would spend it. Consequence:
   **we never call it a walkie-talkie**, in copy, in the store, or in a review
   pitch.
2. **No location.** No GPS, no cell, no Wi-Fi geolocation lookup, no
   "where is my kid" screen, no breadcrumb history. The warmer/colder game
   (§6.5) is a similarity score between two RSSI fingerprints that never
   becomes a coordinate, never leaves the pair, and is never stored.
3. **No camera.**
4. **No free text and no keyboard.** Nothing on this device requires reading.
   The one open-ended channel (Doodle, §5.4) is ephemeral by construction and
   can be switched off per-watch by a parent.
5. **No strangers.** A string can only be tied inside a **tie window** opened
   from the household hub on the home LAN (§5.6). There are no invite codes, no
   directory, no discovery of non-household watches, and no way to tie a string
   in a school playground.
6. **No account, no login, no PII.** No name, no birthday, no photo, no email.
   A kid's whole identity is a bird colour and a three-glyph mark they pick on
   first boot. Nothing about a child is transmitted to us because there is no
   "us" in the data path.
7. **No cloud.** The firmware has no internet client. Not "we don't use one" —
   there isn't one linked in. This is also the COPPA answer (§9.1).
8. **No biometrics off-wrist.** Step counts are compared as a single integer
   inside a string and are never stored, exported, or aggregated. The 2025
   COPPA amendments made biometric identifiers personal information; we simply
   don't hold any.
9. **No punishing engagement mechanics.** No streaks that break, no guilt-face
   when a kid didn't move, no ranking against other households, no
   notification spam. The bird's honesty rule from
   [`bird_mood.h`](../../firmware/projects/canary-display/include/canary/care/bird_mood.h)
   — *"no random sadness for variety, no cheerful mask over a degraded system"*
   — extends to: **no sadness about the child at all.** The bird reacts to the
   string, never to the kid's behaviour.
10. **Not a safety device, not a medical device, not a locator, not a phone.**
    This is stated on the box, on the boot screen, and in the parent app's
    first-run — not buried in terms. §9.2.

### 3.2 Age scoping

| Band | Posture |
|---|---|
| **Under 6** | Knock, Tug, Stamps only. Doodle off by default. No duels (a step contest for a four-year-old is just a way to lose a watch). |
| **6–11 — the target** | Everything. This is the band that can't be trusted with a phone, can already keep a secret code, and finds "I buzzed her from the treehouse" genuinely magic. |
| **12+** | Don't chase it. They want a phone and they will get one. A Tin Can survives here only as a household object (the Ring still reaches them). |

The bands are a **parent app setting**, not an age the device asks for. The
device never learns a birthday.

### 3.3 The hard gate before anyone sells one

This design is buildable today as a **maker project**. It is **not** a
children's product until a lab says so, and the doc should stop anyone from
blurring the two:

- **CPSIA** third-party testing applies to consumer products designed
  primarily for children 12 and under; **ASTM F963** is the US toy safety
  standard, plus small-parts (16 CFR 1500.18).
- **Battery containment.** Reese's Law and the CPSC rule that followed it make
  child-resistant battery compartments a legal requirement for coin/button
  cells. A bare Li-po strapped to a child's wrist is the single largest
  physical risk in this entire design and it is not covered by any firmware
  decision.
- **The strap must break away.** Anything worn on a child around playground
  equipment needs a breakaway/tear-away failure mode.
- **FCC / CE** for an intentional radiator, plus SAR posture for a worn device.

Until those exist: ship it as a **kit for the maker's own household**, say so,
and do not put the word "kids" on a store listing. This paragraph is the
liability firewall, and it is cheaper than the alternative.
*(Not legal advice — this is the homework list to hand to counsel, per
[`LEGAL.md`](../LEGAL.md).)*

---

## 4 · What the kid sees

One glass, mostly black (AMOLED), four faces reachable by swipe. No menus, no
settings a child can wreck, no text a child must read.

```
        ┌─────────────┐   swipe →   ┌─────────────┐
        │   THE FACE  │             │  THE STRINGS│
        │    9:42     │             │  ●───── Mya │  taut
        │   🐦 bird   │             │  ●╌╌╌╌╌ Sam │  slack
        │  ▓▓▓▓░ batt │             └─────────────┘
        └─────────────┘                    ↓ tap a string
              ↑ swipe                ┌─────────────┐
        ┌─────────────┐             │  THE CAN    │
        │  THE QUEST  │             │  hold=tug   │
        │  today's    │             │  tap=knock  │
        │  one thing  │             │  ○ stamps   │
        └─────────────┘             └─────────────┘
```

- **The Face** — clock, the kid's canary, battery, and the state of every
  string as a tiny row of taut/slack lines. Reuses
  [`character.h`](../../firmware/projects/canary-display/include/canary/ui/character.h)
  (the Character ring gives us kid palettes for free — Neon and Aqua already
  exist) and `bird_mood.h`.
- **The Strings** — one row per tied string, drawn as an actual line: taut when
  reachable, sagging and dashed when not. No "last seen 14:02". A picture.
- **The Can** — the per-string screen. Hold to tug, tap to knock, ring of
  stamps around the edge, doodle pad in the middle.
- **The Quest** — at most **one** thing per day (§6.4). Never a list, never a
  backlog, never a red badge.

**No roach motels**, same rule as the display's mode registry: every screen is
one long-press from the Face. Nothing on this watch can trap a child —
including the parent's own settings (§9.3).

---

## 5 · The string: protocol and vocabulary

### 5.1 Transport

**ESP-NOW is primary.** Connectionless, ~ms latency, no AP association, and —
critically for a house — it keeps working when the router reboots.

**What we reuse is the plumbing, not the wire format.** Be precise about this,
because the existing path cannot carry a string:
[`espnow_peer.h`](../../firmware/projects/canary-display/include/canary/net/espnow_peer.h)
is a **receive-only observer** by design — the dash has no signing identity, so
"it never transmits" is a stated honesty rule, not an omission — and
[`espnow_peer_logic.h`](../../firmware/projects/canary-display/include/canary/net/espnow_peer_logic.h)
accepts *only* an exact **11-byte** (`BEACON_MFG_LEN`) fleet-presence beacon and
decodes it into the fleet model. The paired transmit side doesn't exist yet at
all ([`board_capability_map_43b.md` §"Remaining to go live"](../hardware/board_capability_map_43b.md)).

So the Tin Can needs, as new work in wave 1–2:

- a **second ESP-NOW frame type** — variable length, authenticated, and
  discriminated from the presence beacon before either parser runs, so a string
  frame can never be mistaken for a witness observation or vice versa;
- a **transmit API** (`string_send`) alongside the existing receive drain — the
  watch *does* mint its own per-string keys, so unlike the dash it has something
  to say.

What genuinely carries over: the ESP-NOW bring-up and channel-follow code, the
"reject foreign traffic before it reaches any model" discipline, and the channel
and airtime governance already written for the WAP mesh
(`mesh_channel_policy.h`, `airtime_governor.h`). The fleet presence beacon stays
exactly as it is — the Tin Can listens to it for §6.5, and never extends it.

**LAN/MQTT is the fallback and the parent path.** When both watches are
associated to the house Wi-Fi but out of ESP-NOW earshot, strings ride
`mqtt_mgr` on household topics. The **Ring** (§7) always rides this path,
because it originates on the hub.

**There is no third path.** If neither works, the string is slack and the UI
says so. We never invent a cloud fallback to make the picture look better.

### 5.2 Knock — the headline

Press-and-hold the glass (or tap the watch body — the IMU picks it up), tap a
rhythm, release. The far watch replays that rhythm **exactly** as haptic
pulses.

- Up to **8 taps** inside a 2 s window; inter-onset intervals quantized to
  40 ms. About 10 bytes on the wire.
- The far watch is **silent** (haptic only) unless the kid has sound on.
- A knock has no meaning we assign. Two siblings will invent
  *three-short = "come to my room"* within a week and it will be **theirs**.
  That is the product. The firmware must never offer a "knock dictionary."
- **Zero moderation surface.** A rhythm is not language. There is nothing to
  filter, nothing to log, nothing that can be a slur.

### 5.3 Tug — presence

Hold a finger on the Can screen. The far watch feels a sustained ramp for as
long as you hold, and the string on both screens pulls visibly taut.

**If both kids hold at once**, both watches lock into a shared pulse and the
line glows. That is the emotional centre of the whole device: two children in
different rooms, each feeling the other holding on. It costs a boolean each way
and it is the thing they'll show their grandmother.

### 5.4 Stamps and Doodle

- **Stamps** — a fixed ring of 16 pictograms drawn in the Canary line style
  (bird, ball, sandwich, moon, "?", heart, bricks, controller, 5, …). Stable
  wire ids in a `stamp_set.h` table; each carries a glyph, a haptic effect and
  an optional chime phrase. A fixed vocabulary means no typing, **no reading
  age requirement**, and no scalable way to be cruel.
- **Doodle** — a 3-second finger scribble replayed stroke-for-stroke on the far
  watch, then gone. **Never written to flash**, never forwarded, never
  screenshot-able. This is the one open channel, so it is the one with the
  guardrails: only inside a tied string, non-persistent by construction, and
  a per-watch parent off switch.

### 5.5 Envelope

Every string owns its own key. One frame shape, versioned, small:

```
[ ver | string_id(2) | dir(1) | ctr(8) | kind(1) | payload(≤32) | tag(16) ]
              AEAD (ChaCha20-Poly1305 or AES-GCM via mbedtls)
```

- Key: X25519 at tie time → HKDF → **two directional keys**, `K_AB` and `K_BA`,
  derived with distinct info strings. Roles A and B are assigned by
  lexicographic order of the two X25519 public keys, so both ends agree without
  negotiating. The project already carries Ed25519 on-device for chain verify,
  so the crypto posture and the review habits exist.
- **Nonce discipline — the thing to get right.** A single shared key plus a
  per-endpoint counter would put the *same* key/nonce pair on the first frame
  each watch sends, and nonce reuse destroys both confidentiality and
  authenticity under ChaCha20-Poly1305 and AES-GCM alike. Two defences, both
  required: the directional keys above mean the two send streams never share a
  key at all, and the nonce is constructed as `dir ‖ ctr` (never random, never
  implicit) so it is recoverable from the frame and unique within a stream.
- `ctr` is **64-bit**, monotonic, and **persisted to NVS before first use of a
  value** — a reboot must never rewind it. It is also the replay defence: a
  sliding window rejects anything not strictly ahead, so a recorded knock can't
  be replayed at 2 a.m. A counter that would wrap, or that cannot be persisted,
  forces a re-tie rather than a rollover.
- **The parent Ring is signed by the household key** and is the only frame kind
  a watch will accept from something that is not a tied peer.
- Off-string frames are dropped before they reach any model — the same
  "foreign traffic is rejected here" discipline `espnow_peer_logic.h` already
  applies.

### 5.6 Tying the string — the ceremony, and the consent gate

1. A parent opens a **tie window** (2 minutes) from the app, the kitchen dash,
   or Home Assistant. **Without an open window, no watch will tie anything.**
   The window is a household-hub action on the home LAN, which is what makes
   "you cannot do this at school" structural rather than a policy.
2. The two kids hold their watches face to face. Each shows a **knot**: four
   glyphs derived from the freshly-agreed shared secret (a short authentication
   string — the same idea as a Signal safety number, drawn as pictures because
   the users are six).
3. The knots match → **both kids press and hold for three seconds, together.**
   Two-person, deliberate, physical, and memorable. Kids remember a ceremony;
   they do not remember a settings toggle.
4. Cap of **6 strings**. Untie is one gesture from either end, and the parent
   can cut any string from the app.

---

## 6 · Fun that survives the refusal list

The constraint set above deletes every lazy idea (chat, voice notes, photos,
"find my friend"). What's left has to be genuinely better, or kids take the
watch off after a week.

### 6.1 The bird is theirs
Reuse `bird_mood.h` verbatim in structure, repoint the inputs. The kid's canary
perks up when a string goes taut, dozes at night, ruffles when the string has
been slack all afternoon. **It never reacts to the child** — not to steps, not
to chores, not to a missed quest. It reacts to *the connection*. That's
affection without manipulation, and it keeps the Pwnagotchi honesty rule
intact.

### 6.2 Knock codes
See §5.2. The single highest fun-per-byte feature in the design, and it exists
*because* speech was refused.

### 6.3 Step duel
IMU steps, sibling vs sibling, resets at midnight, one integer each way. The
firmware **says out loud that shaking your wrist counts** — a cheat you can see
is a game; a cheat detector you can't see is an argument with a parent. Cap the
rate, don't police it.

### 6.4 One quest a day
The parent app can set at most one: *"be outside before dinner", "feed the
cat"*. It lands as a stamp, the kid taps it done, the parent gets a check with
a time. One. Never a list. There is no backlog, no streak, and no penalty for
skipping — which is precisely what makes it feel like a treasure map instead of
a chore app.

### 6.5 Warmer / colder — the LAN as a treasure map

The genuinely novel one, and it works *only* because the household already has
a fleet of Canaries beaconing.

A **hider** watch stands somewhere and records a fingerprint: the RSSI of each
**household Canary** it can hear, keyed by the `fp4` those Canaries already
beacon. A **seeker** watch continuously compares its own fingerprint to that one
and shows **warmer / colder** — a bird that gets excited, a line that tightens,
a haptic that quickens.

- **Household Canaries only — never Wi-Fi APs.** The obvious implementation
  ("every AP it can hear") is the wrong one and must be refused in the wire
  contract, not just in review: an RSSI vector keyed by BSSIDs — or by *stable
  hashes* of them — is precisely the input that commercial Wi-Fi geolocation
  databases consume. Emitting one would manufacture reusable location metadata
  on a device whose headline claim is that it has none, and would break
  **Invariant III (Metadata Minimization)** — *"zone IDs are local only,
  correlation tokens are single-use"* ([`AGENTS.md`](../../AGENTS.md)).
  Household `fp4`s are already household-scoped, already coarsened, and already
  ours.
- It is a **similarity score**, not a position. No trilateration, no map, no
  coordinates, ever — because the moment it produces a coordinate it becomes a
  child locator and inherits that entire category's liability.
- The fingerprint is **game-scoped**: it lives only for the round, only inside
  the string, and is discarded when the round ends. It is never stored, never
  reused across games, and never leaves the pair.
- It reuses `chirp_scan.h` and the beacon RSSI machinery we already ship.
- Every extra Canary a household owns makes the game *better*. That is the
  nicest incentive alignment in the product line.

### 6.6 The passive-aggressive delight
Two watches in the same room, both taut, both idle, and their birds start
mirroring each other's idle flourishes. Nobody asked for it. It's four lines of
code and it's the thing a kid notices on day three.

---

## 7 · The parent side: the Ring

One privileged message. Not a chat.

**Sources:** the iPhone companion ([`iphone_companion_app.md`](iphone_companion_app.md)),
the 4.3" kitchen dash (a physical, satisfying button), or a Home Assistant
automation/`adapter_host` script.

**Vocabulary — fixed, six entries:**
`Come inside` · `Dinner` · `Bedtime` · `Come find me` · `Answer me` · `All clear`

**Behaviour:**

- Full-screen on the kid's watch, a distinct haptic pattern that belongs to no
  other message, and a chime phrase from the existing
  [Canary Voice](../../firmware/projects/canary-display/include/canary/hal/voice_score.h)
  grammar — deliberately not reusable by a sibling. The Ring is the one thing
  **Quiet cannot silence**, matching the chime engine's existing rule that the
  Tier-1 alert keeps an audible floor at every volume.
- **`Answer me` requires an ack.** The kid taps; the parent sees a check and a
  time. That's the entire feature: an acknowledged poke. Not a location, not a
  status, not a live view.
- **Honest delivery.** The parent's UI shows exactly three states —
  **delivered & acked** / **delivered, not acked** / **not delivered**. It never
  says "sent" and lets a parent infer the rest. If the watch is out of range,
  off, or flat, the app says *"not delivered — the string was slack."*

That last bullet is the most important sentence in the section. A parent who
learns that this always works will build a safety habit on a best-effort LAN
message, and the day it silently fails is the day this design hurt someone.
The absence-is-an-alarm semantics the displays already use
([`display_discovery_and_resilience.md`](../hardware/display_discovery_and_resilience.md))
apply here in the parent's direction: **the app is loud about not knowing.**

---

## 8 · Power, wear, and the boring reality

A 410 × 502 AMOLED plus Wi-Fi is not a two-week watch. Be honest early:

- **Target: a full school day** — roughly 7 a.m. to 7 p.m. on a ~500 mAh cell,
  charged on a bedside dock nightly. Say "charge it like a toothbrush."
- AMOLED + a mostly-black face means the Face costs little; the panel is off
  most of the time anyway (wake-on-raise via QMI8658).
- Radio duty is the real budget. ESP-NOW listen windows are cheap; a periodic
  wake schedule (~1 Hz listen, longer when every string has been slack for an
  hour) is the tuning knob. The RTC keeps the clock with the radio down.
- **Bedtime is a power feature.** Strings go quiet at parent-set quiet hours
  (reuse `rhythm.h` / `suncalc.h`); the Ring still lands.
- Thermals are not a concern at this duty cycle, but the thermal watchdog from
  the ACTIVE tree comes along anyway — this thing touches skin.

---

## 9 · Liability posture, argued

### 9.1 Why COPPA mostly does not reach this

COPPA binds an **operator** of a website or online service — including
connected toys — that **collects** personal information from children. The
2025 amendments (compliance by 22 April 2026) widened "personal information"
to include biometric identifiers and added separate consent for disclosure to
advertisers.

The Tin Can's answer is structural, not procedural: **there is no operator and
no collection.** No account, no server, no telemetry, no internet client
compiled into the firmware, no PII on the device to begin with (§3.1.6). The
FTC's audio-recording carve-out — that a voice file may be collected without
consent if it is destroyed promptly — is a carve-out we don't even need,
because there is no audio path at all.

What we still owe: a plain-language parent-facing privacy page that says all of
the above in words a non-lawyer can check, and the discipline never to add a
"just one small analytics ping."

### 9.2 The disclaimers, and where they live

*"Not a safety device. Not a phone. Not a locator. Works only inside your home
network. Messages may not arrive."* — on the box, on the boot screen (once, on
first boot, dismissible), and in the parent app's first-run. Not in a EULA.

### 9.3 Nothing here is a control device

A parent can silence a watch, cut a string, and turn Doodle off. A parent
**cannot** lock a child out of the plain watch face, cannot read a child's
knocks, doodles or stamps (they are not stored anywhere to read), and cannot
see where a child is (there is no such data). School mode silences; it never
traps — the no-roach-motels rule is a child-safety rule here, not just a UX
one.

This is a deliberate product decision with a legal dividend: a device that
cannot monitor a child is a device that cannot be subpoenaed for a child's
messages, cannot leak them, and cannot be turned into a coercion tool inside a
troubled household.

---

## 10 · What goes wrong

| Risk | Mitigation |
|---|---|
| **Sibling knock-spam** | Per-string rate limit with a visible cooldown on the *sender's* screen ("the string is still buzzing"). No mute wars, no block lists — a cooldown is legible to a six-year-old; a block is not. |
| **Doodle is a bullying channel** | Sibling-scoped by construction, ephemeral, parent off switch. It is also the first feature to cut if bench use says otherwise. |
| **Step duel → arguments** | Daily reset, no history, and the firmware admits shaking counts (§6.3). |
| **Parent treats the Ring as safety** | §7's three delivery states, plus the boot-screen disclaimer. If bench testing shows parents still over-trust it, add a monthly "this only works at home" reminder. |
| **A watch is lost with keys on it** | Per-string keys, no household master secret on a kid's watch, and nothing personal on the device to find. **Revocation is not instant, and the UI must not pretend it is:** the app cannot erase a lost watch's NVS, and the two watches can still reach each other over router-independent ESP-NOW. So a cut is a **signed revocation** the *surviving* peer must receive and persist (it then refuses the string's keys and shows the string as cut); until that ack lands, the app shows the cut as **pending**, exactly like the Ring's three delivery states in §7. A revocation the surviving watch has persisted is permanent — it survives reboot and cannot be un-cut by a replayed frame. |
| **RF congestion** with several watches + a fleet | Reuse the WAP airtime governor and channel policy. Strings are tiny and bursty; the fleet beacons are the bigger consumer. |
| **The board is not a kid product** | §3.3. This is the one that stops a launch, and it should. |
| **Scope creep to voice** | The refusal in §3.1 is a *constitutional* line here, same standing as Invariant I. Adding voice is a new product with a new argument, not a feature PR. |

---

## 11 · Build plan

| Wave | Deliverable | Gate |
|---|---|---|
| **0 — bring-up** | Board registry entry + pin header; LVGL on the CO5300 at 410 × 502; confirm the touch controller part; DRV2605L + LRA on the I²C port; battery telemetry from the AXP2101 | it lights up, it buzzes, it survives a day |
| **1 — pure cores, host-tested** | `string_model.h` (tied/taut/slack, + persisted revocation), `knock_codec.h`, `tincan_wire.h` (frame discrimination, header parse, **directional nonce construction**, monotonic-counter replay window), `tie_ceremony.h` (role assignment + the knot derivation), `ring_policy.h` (priority, ack, delivery states), `stamp_set.h`, `warmer_colder.h` (`fp4`-keyed only — a test asserts no AP identifier can enter a fingerprint), `duel_model.h` — all in `tests_host/`, zero Arduino | CI green with no board attached |
| **2 — one string** | The new ESP-NOW string frame type **and its transmit path** (§5.1), tie ceremony, knock + tug, taut/slack drawn honestly | two kids in one house, one week, no instructions |
| **3 — the Ring** | Hub → watch over MQTT, ack path, the three delivery states in the app and on the dash | a parent can call kids to dinner and knows whether it landed |
| **4 — the fun** | Stamps, Doodle, quest-a-day, step duel, warmer/colder | the watch is still on the wrist after a month |
| **5 — the shell** | Strap, enclosure ([`canary-local/enclosures`](../../canary-local/enclosures)), battery containment, and the §3.3 certification homework | nobody says "kids" in a store listing before this |

**Code layout:** a new SPECIALIZED tree `firmware/projects/canary-tincan/`
under [`VARIANT_POLICY.md`](../../firmware/VARIANT_POLICY.md) — its scope
(paired kid messaging) is genuinely different from canary-display's
(fleet rendering), the way canary-sense's is. Wave 0 promotes the shared
organs — `fleet_model`, `bird_mood`, `character`/`theme`, `lvgl_port`,
`chime`/`voice_score`, `espnow_peer`, `beacon_parse`, `mqtt_mgr`, `wifi_mgr`,
`provision` — from canary-display into `firmware/common/` rather than forking
them, per the tree's existing anti-drift habit.

---

## 12 · Open questions

1. **LRA vs ERM.** An LRA gives a crisper, more "tap-like" knock; an ERM is
   cheaper and more forgiving mechanically. Bench both before Wave 2 — the
   knock is the product, and a mushy knock is a dead product.
2. **Does Doodle survive contact with real siblings?** It's the one channel
   with an open payload. Ship it off-by-default in Wave 4 and let a household
   turn it on.
3. **Is the tie window enough of a stranger gate,** or does a string also need
   a per-household key so two watches from different houses can never tie even
   inside a window? (Leaning: yes, add it — it's cheap.)
4. ~~**Should the Ring reach a kid who has walked out of range and back?**~~
   **Settled: it expires after 5 minutes**, and the parent is told it expired.
   Implemented both ways in
   [`ring_policy.h`](../../firmware/projects/canary-tincan/include/canary/tincan/ring_policy.h):
   the sender stops waiting, *and* a receiving watch refuses to render a Ring
   older than its own window. Late is not a weaker kind of on-time.
5. **Sound on the watch at all?** Partly settled — the board *can* make sound
   (see the hardware notes in §1.1); what is open is whether we want it to. A
   silent-only device is purer, and quieter in a classroom.
6. **Do we want the mics physically absent** on any household build — a
   cut-trace or DNP note in the enclosure docs — so "no voice" is provable by
   looking, not just by reading our source?

---

## References

- Waveshare **ESP32-S3-Touch-AMOLED-2.06** — [product page](https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm),
  [wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06),
  [sample tree](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06)
  (peripheral list; confirms no haptic motor)
- [Tin-can telephone](https://en.wikipedia.org/wiki/Tin-can_telephone) — the
  taut-string/longitudinal-wave mechanism behind §2
- FTC — [Complying with COPPA FAQ](https://www.ftc.gov/business-guidance/resources/complying-coppa-frequently-asked-questions),
  [Children's Privacy](https://www.ftc.gov/business-guidance/privacy-security/childrens-privacy),
  [audio-recording enforcement policy statement](https://www.ftc.gov/system/files/documents/public_statements/1266473/coppa_policy_statement_audiorecordings.pdf),
  [2025 Rule amendments (Federal Register)](https://www.federalregister.gov/documents/2025/04/22/2025-05904/childrens-online-privacy-protection-rule)
- CRS — [Smart Toys and COPPA](https://www.congress.gov/crs_external_products/LSB/PDF/LSB10051/LSB10051.4.pdf)
- Prior art: [Relay](https://www.amazon.com/Relay-Screenless-Smartphone-Nationwide-Contract/dp/B07HPGBK4N) (screen-free kid PTT, 4G + GPS),
  [Yoto](https://us.yotoplay.com/) (screen-free, "no cameras, no mics, no ads")
- In-tree: [`lan_baby_monitor.md`](lan_baby_monitor.md) ·
  [`alert_relay.md`](alert_relay.md) ·
  [`iphone_companion_app.md`](iphone_companion_app.md) ·
  [`display_modes.md`](../hardware/display_modes.md) ·
  [`VARIANT_POLICY.md`](../../firmware/VARIANT_POLICY.md) ·
  [`LEGAL.md`](../LEGAL.md) · [`BRAND.md`](../BRAND.md)
