# Design: the LAN baby monitor — DEFERRED

**Status:** research complete, **deliberately out of scope for now** — no
code. This doc exists so the "no" is argued honestly and recorded, per the
[`canary-fence-guard`](../../firmware/projects/canary-fence-guard/README.md)
/ [Hub network witness](hub_network_witness.md) precedent: when we revisit,
we start from this argument instead of re-litigating it from memory.
· **Date:** 2026-07-25 · **Owner:** TBD

> *"Look up what we could do for baby monitoring [over] LAN."*

Short answer: **we researched it, and we are not building it now.** The
part of a baby monitor that is commodity (all-night live audio/video on the
LAN) is the part our constitution forbids and the open-source world already
does well. The part that is genuinely ours (care without a camera in the
room) is already shipped or positioned under the wellbeing line and doesn't
need a new product to exist. The rest of this doc is the evidence.

---

## 1. What a baby monitor actually is

The incumbent parent units (Infant Optics DXR-8 Pro, eufy SpaceView, VTech,
HelloBaby — surveyed in
[`display_market_research.md` §4](../research/display_market_research.md))
set the bar the category is judged by:

- **Continuous, all-night raw audio + video**, glanceable at the bedside.
- **Sub-second latency** — local-FHSS units run <0.2 s; WiFi/cloud rivals
  at 10–20 s are the ones parents return.
- **Absence is an alarm**: link loss must beep within seconds, every time.
  (We already adopted this semantics for displays —
  [`display_discovery_and_resilience.md`](../hardware/display_discovery_and_resilience.md).)
- **Two-way talkback** to soothe without entering the room.

Every one of those is table stakes. A baby monitor that does three of the
four is not a smaller baby monitor; it is a returned one. That framing is
what makes the scope call clear.

## 2. Why this is beyond our scope for now

### 2.1 The core feature is the thing our invariants exist to forbid

[Invariant I](../../spec/invariants.md) — *No Raw Export by Design* — says
the kernel MUST NOT expose APIs that **stream, mirror, or replay** raw
media. The single tolerated live path, camera **Peek**, is defended in
[`openipc_architecture_learnings.md`](../openipc_architecture_learnings.md)
precisely because it is *ephemeral, session-scoped, setup-shaped*: "short-lived
MJPEG only on the local WiFi, no storage, architecturally impossible for
frames to leave the device any other way."

A baby monitor inverts that: continuous overnight streaming *is the
product*. Shipping it means either (a) formally amending the invariant with
a "live care view" carve-out — a constitutional change, not a feature PR —
or (b) shipping something that contradicts the promise on the front page of
the README. Neither is a side quest. Until someone writes and defends that
carve-out, the honest answer is no.

### 2.2 Audio streaming is not unimplemented — it is anti-implemented

The mic pipeline
([`securacv_audio.h`](../../firmware/projects/canary-wap/arduino/canary_wap/securacv_audio.h),
[`display_mic_variant.md`](../hardware/display_mic_variant.md)) reduces
16 kHz capture to at most three scalars per 20 ms window and **zeroes the
sample buffer inside the callback**. "No audio can be streamed" is not a
missing feature — it is one of the strongest honesty claims we make, and
it's load-bearing in how the mic variant was cleared to exist at all.
Nursery audio would require a second capture path built to bypass that
barrier. That's a conformance break with blast radius across every page
where we say "the mic cannot record," and it is not worth one product line.

### 2.3 The hardware can't hold up its half yet

- Only **canary-wap** has camera + mic + HTTP server. Its peek stream is
  **single-viewer** (second client → 409), auto-standbys the camera after
  5 min idle, and streaming steps the ESP32-S3 die **10–20 °C in minutes**
  ([`esp32s3_thermal_review.md`](../esp32s3_thermal_review.md)) — an
  all-night session in an enclosed, nursery-warm case is untested territory.
- **No Canary has a working speaker.** The display's mic variant never
  initializes its speaker output, deliberately
  ([`display_mic_variant.md`](../hardware/display_mic_variant.md)); the
  only audio out anywhere is a piezo chirp. Talkback — table stakes, per §1 —
  means new hardware (an I²S amp), a new transport, and a new privacy
  argument for an *inbound* audio channel into a child's room.
- **No parent unit exists.** The TV emulator's "two-way ready" PiP is a
  scripted demo toast (`desktop/src/witness/tv-emulator.js`), not code.

### 2.4 It is a safety-adjacent product, and half of one is worse than none

Parents treat a baby monitor as safety equipment even when the box says
otherwise. The category's failure lore is exactly what our research files
already catalog — Miku's brick-and-paywall, Nanit's subscription-gated
activation, Cubo's hundred-notification nights
([`display_market_research.md` §4](../research/display_market_research.md)).
Entering it invites the comparison on latency, link-loss behavior, and
all-night reliability from day one, plus a "not a medical device" liability
posture we have so far only had to hold for the wellbeing radar
(see the framing on the website's watch-over page). We should enter that
category on purpose, resourced for it — or not at all.

### 2.5 The commodity part is already solved outside; the honest part is already ours

Anyone who wants LAN video of a crib tonight has excellent local-only
options: Frigate + go2rtc WebRTC (with local YAMNet **cry detection**
built in), OpenBabyMonitor, BabyGuard on a Pi. We would be building an
undifferentiated, worse version of that — on weaker silicon — while
spending our actual differentiator to do it.

Meanwhile the thing only we say with a straight face is already in the
line: **Canary Sense over the crib** — 60 GHz presence and bedside
breathing, no camera, no mic, vitals suppressed unless exactly one person
is in range. The Lab's house demo already places it on the nursery dresser
("a baby monitor with no camera to point"), the gallery already sells
"Nursery Sense in soft sage," and the Care/Nightstand waves already encode
the alerting and sleep-comfort craft the category taught us
([`display_care_wave.md`](../hardware/display_care_wave.md),
[`display_nightstand.md`](../hardware/display_nightstand.md)). The care
value ships without the camera, which is the brand.

## 3. What this deferral does *not* close off

Small, invariant-clean pieces that can land independently, whenever
prioritized — none of them constitute "a baby monitor":

1. **A nursery blueprint for Canary Sense** — crib presence, breathing
   confidence, settled/stirring/awake coarse states, temp/humidity against
   the sleep-science bands the Nightstand already encodes. Pure config +
   docs, alongside
   [`canary_sense_wellbeing.md`](../blueprints/canary_sense_wellbeing.md).
2. **A cry-*cadence* template** in the acoustic-events module, beside
   T3/T4 — envelope rhythm only, emitting a coarse claim, buffer-zeroing
   untouched. Dictionary-first per FR-13 (a new event type edits
   [`witness_dictionary.json`](../../spec/witness_dictionary.json) before
   any code).
3. **Frigate audio-label mapping** — for households already running
   Frigate, mapping its local cry/scream audio labels into witness events
   through the existing bridge.
4. **Peek stays exactly what it is** — a setup tool.

## 4. Revisit triggers

Reopen this doc — don't restart the debate — when any of these change:

- A written, accepted **Invariant I carve-out** for session-scoped live
  care viewing exists (the constitutional work in §2.1).
- A Canary or display ships with a **real speaker path**, making talkback
  physically possible.
- The peek stream is reworked for **multi-viewer + long sessions** with the
  thermal story tested in an enclosure.
- The community asks for it loudly — the website's ideas board is the
  intake; a top-voted "nursery watch" idea is a genuine signal.

Until then: this was looked at properly, and the answer is *not now*.
