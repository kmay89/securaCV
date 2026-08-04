# Canary Display — Trailblazer feature specs (v0.3+)

> Status: **wave 1 SHIPPED** (§1–§5: proof-on-glass, ack-sync, illumination
> ladder + presence-wake, heartbeat, chime engine), **wave 2 display-side
> SHIPPED** (§6 chirp scan, §7 v1 in-RAM story, §8 names/rooms, §9
> wellbeing line, §10 draft standard published), remaining items marked
> *bench/next* inline. Companion to [`display_ux_design.md`](./display_ux_design.md)
> (goals G1–G13), [`display_discovery_and_resilience.md`](./display_discovery_and_resilience.md),
> and the [bench bring-up runbook](./display_bench_bringup.md) (validates every
> §1–§10 acceptance criterion on real glass — incl. the router-unplugged §6 demo).
> Every feature below states its *user story*, UX, wire/protocol impact,
> privacy posture, and acceptance criteria.

---

## Wave 1 — shipped in firmware

## 1. Proof-on-Glass ("tap for proof") — `FEATURE_PROOF_QR`

**User story.** "The insurance adjuster asks how I know the garage door
opened at 2 a.m. I tap the card on the kitchen display; a QR appears; she
scans it with her own phone and can verify the Ed25519-signed chain head
herself. No app, no account, no SecuraCV server."

**UX.**
- *Dash*: tap a witness card → a modal proof sheet: device name, link
  state, trust badge, and a QR on a white card (scanners need dark-on-
  light) with the caption "Scan to verify — no account, no cloud." Tap
  anywhere to dismiss.
- *Watch*: a Proof page after the events page (tap-cycle): QR of the most
  urgent witness's chain head (first Alert-grade device, else the first).

**Payload** (QR, JSON): `{"v":1,"t":"securacv/<id>/chain","pk":"<64-hex
pubkey>","p":<raw retained chain payload>}` — the exact bytes the witness
published, plus the pinned pubkey. An independent verifier rebuilds
`securacv-canary-sig|v1|chain|<id>|<length>|<latest_hash>` and checks the
b64url signature (same canonical as `custom_components/securacv/signature.py`).
The display shows proof of what *it* pinned — if its pin disagrees with the
chain, the badge already says ✕ and the sheet says so.

**Privacy.** Nothing new leaves any device; the QR reproduces an
already-broadcast retained payload plus a public key. **Acceptance:** a
stock phone camera + 20-line python verifies a scanned chain against the
device's health-topic pubkey; QR renders ≤ 1 s after tap.

## 2. Household ack-sync — `FEATURE_ACK_SYNC`

**User story.** "The hallway sensor tripped while I carried groceries. I
long-pressed the kitchen dash; when I got upstairs, the bedside puck had
already gone quiet too. One house, one acknowledgement."

**Protocol.** Shared retained topic `securacv/fleet/ack`, payload
`{"at":<epoch_s>,"by":"<device_id>"}`. Every display publishes on a local
long-press ack (only when SNTP-synced — no guessed clocks on the wire) and
applies remote acks that are (a) newer than the last applied, (b) younger
than the ack-hold window, (c) not its own echo. **The new-alert rule is
untouched**: a fresh Alert/Tamper still cancels the (synced) ack on every
display, because each display ingests the same events.

**Privacy.** Adds one retained topic carrying a timestamp + device id — no
person data. **Acceptance:** ack on display A quiets B within 1 s; a
broker-reconnect retained replay never re-quiets a *newer* alert.

## 3. Illumination ladder + presence-wake — `FEATURE_PRESENCE_WAKE`

**User story.** "As I walk toward the front door, the dash is already
awake — it saw me coming through the hallway canary. It has no camera and
no mic of its own. At 3 a.m. it stays dark unless something is wrong."

**States** (watch, PWM; dash follows within its on/off constraint):

| State | When | Watch | Dash |
|---|---|---|---|
| **Active** | touch, fresh Notice+ event (<10 s), or unacked Alert | full | on |
| **Ambient** | otherwise, outside quiet hours | dim (~40%) | on |
| **Night** | quiet hours | red-shifted floor | off |

Presence-wake promotes Ambient→Active only. **G5 stands: nothing but an
unacked Alert/Tamper ever overrides the Night floor.** Follow-me (waking
only the display nearest the presence) needs room metadata on the wire —
specified in §8.

**Acceptance:** hallway presence event lights the watch before the user is
in front of it; no wake events during quiet hours below Alert.

## 4. The heartbeat

**User story.** "From the doorway, one soft pulse a minute tells me:
everything reachable, everything verified. Darkness never has to mean
'fine, probably.'"

**UX.** Once per 60 s, *only* when every witness is Ok/Notice **and** every
chain is Verified **and** it's daytime: the watch halo's outer ring (dash:
the header glow) swells and fades over ~1.6 s. Any lesser state = no
heartbeat — its absence is information. This is the sanctioned 4th motion
in the design language's budget (it earns its movement cryptographically).

**Acceptance:** pulse absent whenever any witness is unverified, stale, or
worse; absent at night.

## 5. Sound identity — Canary Voice — `FEATURE_CHIME`

**User story.** "A dead sensor battery must never sound like an intruder"
(Owlet's two-tone lesson) — and resolution deserves a sound, so silence
keeps meaning "nothing new." Beyond safety, the sound should be *ours*: a
family of acoustic signatures a household learns the way it learns a
marque's startup chime, elegant enough to live with and never annoying.

**The engine.** One passive piezo, one LEDC channel — monophonic square
wave. The quality is in what we do *with* it, all non-blocking and rendered
a few ms at a time so a note can **breathe** (shaped attack/release — the
de-click that makes it an instrument, not a beeper), **chirp** (glissando —
pitch glides across a note), and **trill** (warble — a fast vibrato, the
bird it's named for). The pure score + audio math live in
`voice_score.h` and are host-tested (`test_voice_score.cpp`); the LEDC
streamer is `hal/chime.cpp`.

**The palette.** Every pleasant voice is drawn from a warm **major
pentatonic** (C6–E7) — a scale with no clashing intervals, so nothing can
sound harsh — and shares one timbre, so the family reads as one identity.
The alert alone stays *outside* that scale, on its frozen IEC pitches.

| Voice | Trigger | Shape | Category / quiet hours |
|---|---|---|---|
| **Alarm** (Tier 1) | Alert/Tamper edge; re-voices every 30 s until ack | 10 fast pulses, alt 2.6/3.1 kHz (frozen IEC 60601-1-8) | Alert — **sounds**, keeps an audible floor at every volume |
| **Warn** (Tier 2) | Warn edge (witness lost, chain failed) | 3 slow 1.8 kHz pulses | Notice — silent |
| **AllClear** | worst falls from ≥Warn to quiet | falling two-tone 1.3→0.9 kHz | Notice — silent |
| **Sunrise** | wake alarm (nightstand wave) | two rising warbled notes | Wake — sounds through the night |
| **Boot** | power-on | rising warbled chirp up an octave ("the canary wakes") | Notice |
| **JoinSuccess** | onboarding joined the fleet | bright pentatonic arpeggio to the octave | Notice |
| **AckConfirm** | long-press acknowledged | warm perfect-fifth rise that resolves | Interaction |
| **PageTurn** | paging a witness (watch) | two light rising pips | Interaction |
| **MuteOn / MuteOff** | per-witness mute toggled | mirrored fall / rise | Interaction |
| **Tap / Heartbeat** | (available; not auto-wired — a per-minute chirp would nag) | soft pip / low swell | Interaction / Ambient |

**Volume that makes sense.** One knob, 0–4 (Off, Soft, Low, Medium, Full),
persisted in NVS (`scv-voice`). Category **ceilings** mean a UI blip is
never as loud as a fault; **night** silences the Interaction and Ambient
categories entirely and attenuates Notice, while Wake and Alert sound
through; the **Tier-1 alert keeps a floor at every volume, including Off** —
the one sound allowed to break the night can't be dialed to nothing.
Interaction tones are the *optional* half of the grammar (a single toggle;
default on). The alarm's soft→full **ramp** is unchanged (wake gently,
escalate honestly).

Engine ships compiled and CI-covered; `FEATURE_CHIME` stays 0 until the
piezo pad is populated (`BUZZER_PIN`: watch GPIO1/D0, dash GPIO6 — both
VERIFY at bench). **Acceptance:** tiers audibly distinct at 5 m; a dead
battery never reads as a break-in; every note de-clicks; the all-clear
plays exactly once per resolution; interactions and mode changes are silent
at night.

**Clearance + preview.** Every signature is cleared as original and
unencumbered, and provably *not* confusable with a regulated life-safety
cadence (fire T3 / CO T4) — the audit and its CI-checked guard live in
[`display_sound_clearance.md`](./display_sound_clearance.md). You can *hear*
the whole palette before the piezo pad exists in the browser preview
(`canary-local/voice/`), which is **generated from `voice_score.h`** and
CI-gated so it can never drift from the firmware.

---

## Wave 2 — shipped display-side (bench items marked)

## 6. Off-grid resilience — Chirp scan + mesh relay — `FEATURE_CHIRP_SCAN`

**Shipped: passive scan.** NimBLE passive scan for the Canaries'
connectionless Chirp adverts (mfr id `0xFFFF`, 17-byte payload:
type/timestamp/chain-hash/fingerprint-suffix — `docs/ble_protocol.md` §5).
The scanner runs *only while the broker link is down* (4 s bursts every
20 s — WiFi/BLE coexistence stays polite) and stops the moment MQTT
returns. Chirps are matched to known witnesses by fingerprint suffix
(the last 4 hex chars, same as the canary's `SCV-XXXX` BLE name; unknown
suffixes surface as `SCV-XXXX` pseudo witnesses so a chirping stranger is never
silently dropped); ALERT/TAMPER chirps raise real fleet events labeled
"(chirp)" — honestly coarser trust, no Ed25519 on chirps — and all types
refresh liveness. Per-witness/per-kind 60 s dedupe absorbs re-broadcasts.
Demo acceptance stands: *unplug the router; tamper still reaches the
bedside puck in <10 s.*

**BLE 5 long range — `FEATURE_BLE5_SCAN` (compiled, bench-gated).** The
scanner can arm the BLE 5 extended scanner so the Canaries' **Coded-PHY
(LE Long Range)** chirps are heard too — roughly **4× the range** of legacy
1M, which is the difference between the far-corner/garage Canary's tamper
reaching the glass with the router cut and not. The chirp wire format is
unchanged (BLE 5 buys range, not a new payload); the flag widens the
passive-scan dwell and raises the BLE heap gate (`ble_gate.h`), since the
extended/coded scanner carries more controller RAM (the reboot-loop lesson
is unforgiving off-grid). It ships **compiled + CI-built (the
`canary-display-dash-ble5` env) but disabled**, exactly like the chime —
turning it into *actual* Coded-PHY reception also needs the NimBLE build's
extended advertising (`CONFIG_BT_NIMBLE_EXT_ADV`) and a Canary transmitting
on the Coded PHY, so a coexistence + range soak on real radios is the last
gate before the default flag flips.

**Bench/next:** re-chirp relay (ESP-NOW/BLE, 1-hop cap) once two displays
are on the wall; the Coded-PHY radio soak above.

## 7. Semantic time machine — `FEATURE_TIME_MACHINE`

**Shipped: the verifiable journal + review UI.** Beyond the v1 histogram
("Past 24h · 14 events · worst: warn"), the fleet model now feeds a
proof-carrying journal (`fleet/journal.h`, host-tested): each event is
recorded epoch-stamped with the **verbatim signed chain head that was live
when it fired** — so the Proof-on-Glass QR works on a week-old event
exactly as on a live one. History never becomes hearsay. The **dash** gets
a full history modal (tap the "Past 24h" line → a wall-clock log; tap any
row → its signed chain as a QR; a two-tap "erase all" honors sovereignty).
The **watch** gets a read-only HISTORY page in the tap-cycle — the durable,
verdict-carrying story. Both surfaces render only records with a known
clock (no guessed timeline).

Cross-reboot durability is **`FEATURE_TIME_MACHINE_PERSIST`** (LittleFS on
internal flash — no SD SPI-bus arbitration; failure-tolerant, so a failed
mount degrades to RAM-only, never a bad boot). It ships **compiled + host-
tested but disabled**, flipped on after a flash-write validation at bench,
exactly like `FEATURE_CHIME`. **Bench/next:** deeper-than-RAM scrubbing
from flash and an explicit retention window (default 30 days).

## 8. Rooms & follow-me (protocol addition)

**Shipped: names + rooms on glass.** Retained `securacv/<id>/meta`
`{"name":"Kitchen","room":"kitchen"}` (published from HA/companion app or
`mosquitto_pub -r`) now flows through the fleet model; both displays
render friendly names everywhere (cards, hero, events, proof sheet), with
`Name · room` on the detail lines. Backwards compatible: absent meta =
device ids, exactly today's behavior. **Bench/next:** room-scoped
presence-wake/follow-me and per-room quiet hours (needs the display to
know *its own* room — onboarding question).

## 9. Wellbeing face (aging-in-place)

**Shipped: wellbeing line.** canary-sense wellbeing builds publish a
P1-gated `breathing_locked` on the state topic; the fleet model now
carries it and both displays append "breathing ✓ / —" to the witness
detail line — radar-only, no camera/mic in the home, no cloud. The field
only renders for witnesses that actually publish it (consent by
construction: no wellbeing build, no wellbeing line). **Bench/next:** the
dedicated caregiver page ("Kitchen active this morning ✓ · overnight
breathing steady ✓") and an explicit consent flag in meta (§8).

## 10. The open standard

**Shipped: draft v0.1 published** at
[`docs/standard/AMBIENT_DISPLAY_STANDARD.md`](../standard/AMBIENT_DISPLAY_STANDARD.md):
conformance levels (AD-Core honesty invariants, AD-Calm attention/light/
sound, AD-Resilient failure ladder, AD-Verified proof-on-glass), a
14-point self-assessment checklist, the reference wire vocabulary, and a
porting guide pointing at the host-testable fleet model. Community ports
(Guition panels, e-ink, Tidbyt-class) grow the ecosystem; SecuraCV owns
the reference implementation and the trust layer. Pitch alignment: Calm
Tech Institute certification criteria. **Next:** iterate the draft in the
open via issues/PRs.

## Anti-roadmap (unchanged, load-bearing)

No live video on displays · no cloud AI · no ads · no subscriptions · no
microphones or cameras on any display. Every "no" above is a documented
competitor failure mode and the moat around everything else in this file.
