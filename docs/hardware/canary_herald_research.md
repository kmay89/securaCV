# Canary Herald — plug-in e-paper door placard (research & design dossier)

**Status:** concept — a design, **no firmware, no bench unit**. This is the
[witness signage plate](./enclosure/canary_sign.scad) made *dynamic*: the fleet already has a static
plate that discloses the witnessing; Herald is the same plate with an e-paper face, so the door can
also carry **a note you typed from your phone**. Family-wise it is a `canary-display` sibling (a
display that shows instead of senses), not a new sensing witness.

**The one-sentence version:** a plug-in e-paper placard at your front door that says — honestly —
*this property is witnessed*, and lets you put a typed note on the door from your phone ("please
leave the package by the bench") in about a second, with no cloud, no camera, no mic, and a face
that keeps saying its piece even when the power is out.

---

## 1 · Why the door gets a display

Two jobs, both already half-built elsewhere in the repo:

- **Disclosure, upgraded.** The static sign plate exists because a privacy witness *says so plainly*
  where notice is appropriate or legally required — its three debossed lines are
  "PRIVACY WITNESS / presence sensing in use / no video is recorded or stored". That wording is the
  Herald's **resting face**, verbatim. A display adds what a debossed plate can't: the disclosure can
  stay *true* as the fleet changes (add a camera witness → the face updates), and an optional QR
  corner can point a visitor at the verify page instead of asking them to take our word for it.
- **The door note.** The oldest front-door interface is a piece of paper and tape: *back in 10
  minutes, deliveries to the side door, ring softly — baby sleeping*. Herald is that piece of paper,
  writable from the couch or the car. This is deliberately **one-way, owner → door**: it replaces
  the note, not the intercom. No mic listening for the courier's reply, no speaker, no camera in the
  placard itself.

What it is *not*: a deterrence prop. The industry's yard signs say "PROTECTED BY" a company that
often isn't even connected; the repo's posture (honest labeling, awareness not control) bans that
move. Herald never says "protected" or "monitored by" — it says what is actually true and locally
verifiable: *witnessed*. If that reads quieter than an alarm-company sign, good: it also reads true.

---

## 2 · Why e-paper — and its honest limits

For a face that lives outdoors and mostly says one thing, e-paper is not the trendy choice, it's the
correct one:

- **Readable in full sun.** Reflective, like paper — the brighter the day, the better it reads.
  Every emissive panel in the family (the dash's RGB LCD, the nightstand's ST7789) washes out
  exactly where this device lives.
- **Dark at night by physics.** Zero light pollution at the door, nothing for a neighbor to
  complain about, no "glowing rectangle" beacon. (The display family's honest-night-light work
  spends real effort making LCDs dim gracefully; e-paper gets it for free.)
- **The image survives power loss.** An unplugged e-paper panel keeps its last image indefinitely.
  A placard whose disclosure *stays up during an outage* is exactly right for this job — and the
  failure is honest: the face never goes blank-and-ambiguous, it just stops changing (§6 covers how
  we label staleness).
- **Nothing to burn in, nothing to backlight, near-zero draw between refreshes.**

The honest limits, stated up front because the repo already wrote them down:

- **Not an alert surface.** The [display market research](../research/display_market_research.md)
  line stands: e-ink is *calm and zero-light but too slow for alerting*. Alerting lives indoors on
  the dash/watch and on the [alert relay](../design/alert_relay.md). Herald renders **disclosure and
  notes** — content where a one-second update is instant and a missed update is a stale note, not a
  missed intruder.
- **"Instantly" means partial refresh.** A modern fast-partial mono panel (SSD1683/UC8176-class
  4.2", or the reTerminal E1001's 7.5") redraws a text region in **roughly 0.3–1 s** without the
  full black-white flash; a **full refresh (~2–4 s, with the flash)** runs every N partials and
  nightly to clear ghosting. So the note appears about as fast as you can look up from your phone —
  but this is a *message board* rate, and the doc never claims video-rate anything.
- **Cold slows it, and eventually stops it.** Per the
  [cold-weather envelope](./cold_weather_envelope.md): standard e-ink refreshes honestly only to
  **0 °C**; wide-temperature EPD parts reach **−15/−25 °C** as a separate panel choice. Below panel
  spec the right behavior is the e-paper superpower: **stop refreshing, keep the last face** (and
  say so indoors — §6).
- **Direct sun ages it.** Panel vendors warn against prolonged direct UV; the mounting guidance is
  a shaded door surround or porch (where doors overwhelmingly are), plus a UV-filtering window in
  the enclosure for the unlucky south-facing install.

Two honest hardware tiers, mirroring the Vision Pro/Lite split:

- **Integrated tier — Seeed reTerminal E1001** (~$79): ESP32-S3, 7.5" 800×480 mono e-paper, battery
  and enclosure included, already flagged in the [TV display design](./tv_display_design.md) as the
  architecture worth stealing (dumb panel, server renders). Fastest path to a bench unit.
- **Raw tier — XIAO ESP32-S3 + 4.2" fast-partial panel + driver board** (~$25–35 in parts): the
  fleet's own MCU family, a smaller face, our own enclosure ([§7](#7--hardware-sketch--parts)) —
  the long-term kit shape.

---

## 3 · Power — plug-in is the feature, not a compromise

E-paper draws ~nothing between refreshes, so a battery build is *possible* — and that's exactly the
trap. The battery e-ink products in the wild poll on 15-minute schedules to survive, which kills the
one thing that makes a door note delightful: **you type it, you hear the door update before you've
pocketed the phone.** Mains power buys an **always-awake radio holding an MQTT session**, so the
message path is push, not poll. The display was never the power problem; the listening was.

Three plug-in paths, in order of how often they're available at a real front door:

1. **The doorbell transformer.** Most doors already have 16–24 VAC at the frame for a chime. A small
   AC-DC buck (the commodity module every smart-doorbell retrofit uses) turns the existing wire into
   the Herald's supply — zero new wiring, and the placard mounts where the eye already looks.
2. **A flat-ribbon USB-C run** from an indoor outlet through the door or window seam — the standard
   outdoor-camera trick; the [outlet cradle](./enclosure/canary_outlet_cradle.scad) family covers
   the indoor end.
3. **PoE splitter** for the house that already ran Ethernet to the porch (rare, but then it's the
   best answer: power + wired network in one).

A small supercap or LiFePO4 buffer rides through blips; on real power loss the panel keeps its face
(§2) and the hub notices the heartbeat stop (§6). No solar: the
[solar sizing guide](./solar_power_sizing.md)'s own "right-size, not biggest" logic says a device
with a wall wire available is solar's *wrong* use case.

---

## 4 · The message path — a typed note with no new cloud

Nothing new is invented here; it's the fleet's existing owner→device write pattern pointed at a
panel:

```
phone / HA dashboard ──(LAN)──▶ HA text entity ──▶ MQTT securacv/<id>/message/set (retained)
                                                        │
                                                        ▼
                                            Herald (always-on MQTT session)
                                            partial refresh ≈ under a second
```

- **Transport:** local MQTT, the fleet spine. The command-topic idiom already exists in
  `canary-display` (`mqtt_mgr.cpp`'s exact-match own-topic latches — the nightstand's owner-set
  alarm time is the precedent for owner-authored content arriving this way). Herald adds one topic,
  `message/set`, retained so a rebooting placard re-renders the current note.
- **Authoring:** an HA MQTT `text` entity plus a handful of **canned quick-notes** ("Please leave
  the package by the bench", "Ring softly — baby sleeping", "Deliveries to the side door") exposed
  as one-tap buttons — because on a phone, at a red light, canned beats typed.
- **Off-LAN:** the owner's *own* remote path into HA (the same rule as the
  [alert relay](../design/alert_relay.md): the link points home; no vendor cloud ever carries the
  note).
- **Staleness is handled, not ignored.** A door note rots fast — "please leave the package" should
  not greet the mail carrier for three days. Every note carries a **posted-at stamp rendered on the
  face** ("posted 3:12 pm") and a default **auto-expiry (hours, owner-tunable)** back to the resting
  disclosure face. That's the ambient-display standard's "last-known state must be labeled as such"
  invariant, applied to paper.

The one genuinely new review question this device raises — the first outward-facing, owner-authored
free text on fleet glass — is answered by scope: the panel renders the owner's words *to the owner's
own door*, signed content and witness claims never render here, and the firmware treats the note as
opaque text with a stamp, never as a claim. The [standard's](../standard/AMBIENT_DISPLAY_STANDARD.md)
§2 honesty invariants bind the *system-authored* faces (disclosure, staleness, fault), which stay
firmware-controlled.

---

## 5 · What it deliberately doesn't do

- **It never advertises absence.** This is the load-bearing rule. No "we're away", no "out for
  delivery", no auto-derived "nobody home" — a placard that tells the street the house is empty is
  a burglary aid, full stop. Fleet-presence-driven auto states are **off by default**, and even
  opted-in they only ever move the wording *toward* "attended" ("someone's around — knock"), never
  toward absence. The user-facing story for the note feature says this plainly. ("Says you're
  there" is safe in exactly one direction — Herald implements that direction only.)
- **It doesn't overclaim.** No "protected", no "monitored", no alarm-company cosplay, no third-party
  brand mimicry. Resting face = the sign plate's true sentences; stronger words would fail the
  repo's own honesty bar.
- **It's not the alert surface** (§2) — no siren, no strobe, no red-flash ambitions. It may render a
  *disclosure-grade* system state ("witness offline since 2:10 pm") because that's honest signage,
  not alerting.
- **No mic, no camera, no speaker.** One-way, owner → door. The doorbell/camera lane already exists
  (the Vision doorbell enclosure); Herald composes with it instead of absorbing it.
- **No cloud rendering.** The TRMNL-style "server renders a PNG" architecture is right, but the
  server is **the hub on the LAN**, not a vendor: the hub (or the device itself for plain text)
  renders the face, and nothing about the household's notes transits anyone else's computer.

---

## 6 · Weather, cold, sun & tamper

- **Enclosure:** a gasketed evolution of the [sign plate](./enclosure/canary_sign.scad) footprint —
  panel window, sealed cable gland at the bottom edge, VHB or security-screw mount, aiming at the
  [field ratings](./enclosure/field_ratings.md) doctrine: *earn* the CER tier on the bench, don't
  assume it. The Vision weatherproof enclosure set (front/back/gasket) is the pattern to copy.
- **Cold:** wide-temp EPD panel for real winters (§2); below panel spec, refreshes pause and the
  last face persists. The *indoor* dash shows "Herald paused — too cold to refresh (last updated
  4:40 pm)"; the door face itself is its own last-known-state label, stamp included.
- **Sun:** shaded mounting guidance + UV-filter window (§2). Mono panel only for v1 — color EPD's
  minutes-long refreshes and tighter temperature windows fail the door-note job.
- **Tamper & theft:** the standard trick from [Gatekeeper](./canary_gatekeeper_research.md)/Fence
  Guard — an **LIS3DH** wake-on-motion catches the pry-off (`TamperDetected`), and the hub's
  **absence-inference** idiom catches the unplug/steal (heartbeat stops → "the placard went dark"
  poke via the alert relay). E-paper's persistence is the consolation prize: whoever walks off with
  it is carrying a sign that says what it is. No new claim vocabulary either way.

---

## 7 · Hardware sketch & parts

Raw-tier sketch (the kit shape; the integrated tier is "buy the reTerminal E1001, print the wall
mount"):

| Part | Choice | ~Cost |
|---|---|---|
| MCU | XIAO ESP32-S3 (fleet standard; WiFi + BLE, SPI to spare) | $7 |
| Panel | 4.2" mono fast-partial EPD (SSD1683/UC8176-class, wide-temp variant for cold climates) + driver board | $25–35 |
| Tamper | LIS3DH accelerometer (the Fence Guard/Gatekeeper part) | $1–2 |
| Power | doorbell-transformer AC-DC buck **or** flat-ribbon USB-C run **or** PoE splitter | $3–12 |
| Enclosure | gasketed printed placard (sign-plate footprint, panel window, cable gland) | $3 |
| **Total** | | **~$40–60** |

Wiring is deliberately boring: EPD on SPI + busy pin, LIS3DH on I2C with one interrupt line — a
fraction of the XIAO's pins, no level shifting, nothing that violates the family's
screwdriver-grade ethos.

---

## 8 · The firmware path, when it graduates

Concept-status today; the shortest consistent path later, for the record:

1. A `canary-display` flavor (`firmware/configs/canary-display/herald/`) — `CD_DEVICE_TYPE
   "canary-herald"`, `FEATURE_EPD`, most of the LCD feature set compiled out.
2. An EPD HAL (`src/hal/display_epd.cpp`) behind the existing HAL seam, so the UI layer never
   learns what a busy pin is; partial/full refresh policy lives here.
3. Board registry rows + `pins/pins.h` for the chosen board, own OTA product string, `flavors.json`
   + size guard, Arduino-twin regen — the standard four-registry drill.
4. The `message/set` command topic in `mqtt_mgr.cpp`'s existing latch pattern + an HA `text`
   entity; canned quick-notes as HA buttons.
5. Conformance claim against the [ambient display standard](../standard/AMBIENT_DISPLAY_STANDARD.md):
   **AD-Core** (honesty invariants — staleness labeling does the heavy lifting on paper) +
   **AD-Resilient** (the failure ladder maps beautifully: dead transport → stale-stamped face →
   persistent face). AD-Calm is nearly free on a panel that emits no light and animates nothing.

---

## 9 · Never let it rot & open items

- **Mostly reuse:** the sign plate's wording and footprint, the display family's MQTT command
  idiom, LIS3DH + absence-inference tamper story, alert relay for the "went dark" poke, cold/solar
  reference docs cited not re-derived. New surface is honestly small: an EPD HAL and one command
  topic.
- **No bench unit.** The real unknowns are physical: actual partial-refresh times on a wide-temp
  panel at −10 °C, ghosting accumulation outdoors, UV yellowing through the window material, and
  whether the doorbell-transformer buck stays quiet enough not to sing. All benchable with one
  panel and one winter.
- **The disclosure face needs a copy review** — it's the one surface strangers read; the sign
  plate's three lines are the starting point, a QR to the verify page is the candidate fourth.
- **Auto-expiry default** (4 h? 8 h?) wants real-life tuning — long enough for "leave the package",
  short enough that stale notes are rare. Owner-tunable regardless.
- **Adjacent, not overlapping:** the Vision **doorbell** (a camera witness that happens to be at
  the door) and the **dash** (the indoor glass). Herald is the outward face; it renders no witness
  claims and senses nothing. Noted so the three don't blur.

---

*Sources: the witness signage plate (`enclosure/canary_sign.scad`) for the disclosure wording and
footprint; [`tv_display_design.md`](./tv_display_design.md) for the reTerminal E1001/E1002 research
and the render-server architecture lesson; the
[display market research](../research/display_market_research.md) for the e-ink-is-not-alerting
line; the [cold-weather envelope](./cold_weather_envelope.md) for the 0 °C standard-EPD floor and
wide-temp −15/−25 °C parts; the [ambient display standard](../standard/AMBIENT_DISPLAY_STANDARD.md)
for the honesty invariants; [`canary_gatekeeper_research.md`](./canary_gatekeeper_research.md) for
the LIS3DH tamper + absence-inference idiom; the [alert relay](../design/alert_relay.md) for the
metadata-only poke. Fast-partial mono EPD timing (~0.3–1 s partial, 2–4 s full) is the vendor-quoted
class behavior for SSD1683/UC8176-era 4.2" panels (GoodDisplay/Waveshare); the doorbell-transformer
16–24 VAC retrofit is the established smart-doorbell power pattern.*
