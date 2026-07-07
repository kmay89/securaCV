# Canary Display — Trailblazer feature specs (v0.3+)

> Status: **wave 1 SHIPPED** (§1–§5: proof-on-glass, ack-sync, illumination
> ladder + presence-wake, heartbeat, chime engine), **wave 2+ SPECIFIED**
> (§6–§10). Companion to [`display_ux_design.md`](./display_ux_design.md)
> (goals G1–G13) and [`display_discovery_and_resilience.md`](./display_discovery_and_resilience.md).
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

## 5. Sound identity — chime engine — `FEATURE_CHIME`

**User story.** "A dead sensor battery must never sound like an intruder"
(Owlet's two-tone lesson) — and resolution deserves a sound, so silence
keeps meaning "nothing new."

**Grammar** (IEC 60601-1-8-informed; LEDC tone, non-blocking):

| Tier | Trigger | Pattern | Quiet hours |
|---|---|---|---|
| 1 | Alert/Tamper rising edge; re-voices every 30 s until ack | 10 fast pulses, alternating 2.6/3.1 kHz | **sounds** (the one exception) |
| 2 | Warn rising edge (witness lost, chain failed) | 3 slow pulses, 1.8 kHz | silent |
| all-clear | worst falls from ≥Warn to quiet | falling two-tone 1.3→0.9 kHz | silent |

Engine ships compiled and CI-covered; `FEATURE_CHIME` stays 0 until the
piezo pad is populated (`BUZZER_PIN`: watch GPIO1/D0, dash GPIO6 — both
VERIFY at bench). **Acceptance:** tiers audibly distinct at 5 m; mode
changes silent; all-clear plays exactly once per resolution.

---

## Wave 2+ — specified, next PRs / bench

## 6. Off-grid resilience — Chirp scan + mesh relay

*Own PR (BLE adds real size/coexistence risk).* Passive NimBLE scan for the
Canaries' connectionless Chirp adverts (mfr id `0xFFFF`, 17-byte payload:
type/timestamp/chain-hash/device-id — `docs/ble_protocol.md` §5). When the
broker link is down, chirps keep liveness/tamper flowing into the fleet
model, badged "via chirp" (coarser trust — no Ed25519 on chirps). Demo
acceptance: *unplug the router; tamper still reaches the bedside puck in
<10 s.* Phase 2: displays re-chirp as ESP-NOW/BLE relays (mains-powered
wall nodes are ideal repeaters), capped at 1 hop to avoid storms.

## 7. Semantic time machine

MicroSD event journal on the watch (slot exists) / flash ring on dash;
dash gains a scrub bar: "yesterday: 14 events, all verified, quiet
00:00–06:12." The story of the home, no video — the archive is the same
signed envelopes, so **history is verifiable too** (proof QR works on past
events). Retention default 30 days, user-wipeable (sovereignty).

## 8. Rooms & follow-me (protocol addition)

Retained `securacv/<id>/meta` `{"name":"Kitchen","room":"kitchen"}`
published from HA/companion app. Displays render friendly names (today:
device ids), group by room, and scope presence-wake/follow-me to their own
room. Also unlocks per-room quiet hours. Backwards compatible: absent meta
= today's behavior.

## 9. Wellbeing face (aging-in-place)

canary-sense wellbeing builds already publish a P1-gated breathing lock on
the state topic. A watch flavor page for the family caregiver: "Kitchen
active this morning ✓ · overnight breathing steady ✓" — radar-only, no
camera/mic in the home, no cloud, green-halo reassurance. Requires: state-
topic wellbeing fields into the fleet model + a consent flag in meta (§8).
This is the market where privacy-by-construction is the *requirement*,
not a preference.

## 10. The open standard

Publish "Quiet Glass" + the fleet model + signed-status vocabulary as
**the open ambient-security-display spec**: docs + host-testable reference
model + conformance checklist (glance ≤1 s, silence-≠-safety deadlines,
honest trust badges, night floor, motion budget). Community ports (Guition
panels, e-ink, Tidbyt-class) grow the ecosystem; SecuraCV owns the
reference implementation and the trust layer. Pitch alignment: Calm Tech
Institute certification criteria (attention/periphery/light/sound).

## Anti-roadmap (unchanged, load-bearing)

No live video on displays · no cloud AI · no ads · no subscriptions · no
microphones or cameras on any display. Every "no" above is a documented
competitor failure mode and the moat around everything else in this file.
