# Canary Herald — plug-in e-paper kitchen placard (research & design dossier)

**Status:** concept — a design, **no firmware, no bench unit**. Family-wise a `canary-display`
sibling (a display that shows instead of senses) — the family's first **e-paper** member, the
"e-ink tile" the [ambient display standard](../standard/AMBIENT_DISPLAY_STANDARD.md) already names
as a conforming form. Drafted first as an outdoor door placard; **moved indoors** once the honest
costs of outside (cold-refresh floor, UV, chime-circuit power) stacked up against what the door
actually needed — the full reasoning and the deferred outdoor variant live in §10.

**The one-sentence version:** a plug-in e-paper placard for the kitchen counter or the fridge —
the household's taped-up note, writable from your phone ("take the chicken out of the freezer") in
about a second, sharing its face with a calm, honest fleet-status line and the morning's weather —
no glow, no camera, no mic, no cloud, and the note survives an unplug.

---

## 1 · Why the kitchen gets e-paper

The kitchen counter is the household's real message hub — it's where the taped note, the fridge
magnet, and the "DISHWASHER IS CLEAN" sticky already live. Two jobs, both squarely in the fleet's
lane:

- **The household note.** One-way, phone → counter: *dinner at 6:30 — lasagna, trash night
  tonight, call Grandma back, the dishwasher is CLEAN*. Typed from the couch, the office, or the
  grocery-store aisle; on the counter before the phone is pocketed. It replaces the sticky note,
  not the family group chat — no replies, no threads, no read receipts.
- **The calm status corner.** The display family's dash and nightstand are glass that *glows* —
  right for a hallway command surface and a bedroom that earns its light budget. The kitchen wants
  the opposite: a face that is **readable all day and invisible all night**, carrying the
  standard's honesty invariants at a glance — "all 6 Canaries reporting · ~7:10 am", and visibly
  degrading when that stops being true ([silence ≠ safety](../standard/AMBIENT_DISPLAY_STANDARD.md)).
  The hub's one retained weather blob (the `FleetSubs::WEATHER` pattern the displays already
  consume) fills the morning-glance corner for free.

This is deliberately the **AD-Calm poster child**: a surface with a zero motion budget, zero light
emission, and a worst-state-readable-in-a-second face — the standard's discipline, achieved by
physics instead of firmware restraint.

---

## 2 · Why e-paper — and its honest limits

- **Calm by physics.** No backlight, no glow, no "glowing rectangle" in a dark kitchen at 2 am.
  The nightstand line spends real engineering making an LCD honest at night; e-paper gets AD-Calm's
  night floor for free, at zero lux.
- **Readable where kitchens are bright.** Reflective like paper — the sunlit counter that washes
  out an LCD makes e-paper *easier* to read.
- **The note survives an unplug.** Cord kicked out, breaker flipped, outage: the face keeps its
  last note and stamp indefinitely. The failure mode is honest — never blank-and-ambiguous, just
  visibly stale (§5).
- **Nothing to burn in, near-zero draw between refreshes, decade-class panel life.**

The honest limits, stated up front because the repo already wrote them down:

- **Not an alert surface.** The [display market research](../research/display_market_research.md)
  line stands: e-ink is *calm and zero-light but too slow for alerting*. Alerting lives on the
  dash/watch and the [alert relay](../design/alert_relay.md). Herald renders **notes and ambient
  status** — content where a one-second update is instant and a missed update is a stale note, not
  a missed intruder.
- **"Instantly" means partial refresh.** A modern fast-partial mono panel (SSD1683/UC8176-class
  4.2", or the reTerminal E1001's 7.5") redraws a text region in **roughly 0.3–1 s** without the
  full black-white flash; a **full refresh (~2–4 s, with the flash)** runs every N partials and
  nightly to clear ghosting. Message-board rate, honestly labeled — never video-rate anything.
- **Mono for v1.** Color e-paper's minutes-long refreshes kill the typed-note job; on a mono
  panel, the standard's "color never carries meaning alone" rule is satisfied by construction —
  every state is words and glyphs.

Two honest hardware tiers, mirroring the Vision Pro/Lite split:

- **Integrated tier — Seeed reTerminal E1001** (~$79): ESP32-S3, 7.5" 800×480 mono e-paper,
  enclosure included, already flagged in the [TV display design](./tv_display_design.md) as the
  architecture worth stealing (dumb panel, server renders). Fastest path to a bench unit.
- **Raw tier — XIAO ESP32-S3 + 4.2" fast-partial panel + driver board** (~$25–35 in parts): the
  fleet's own MCU family, a smaller face, our own printed stand (§7) — the long-term kit shape.

---

## 3 · Power — plug-in is the feature, not a compromise

E-paper draws ~nothing between refreshes, so a battery build is *possible* — and that's exactly
the trap. The battery e-ink products in the wild poll on 15-minute schedules to survive, which
kills the one thing that makes the note delightful: **you type it, the counter updates before
you've pocketed the phone.** Mains power buys an **always-awake radio holding an MQTT session**,
so the message path is push, not poll. The display was never the power problem; the listening was.

Indoors, this is the easiest power story in the fleet: a **USB-C wall adapter** at the counter
(the [outlet cradle](./enclosure/canary_outlet_cradle.scad) family already covers the
outlet-adjacent form), or a slim flat cable to a fridge-mounted unit. A small buffer rides through
blips; on real power loss the panel keeps its face (§2) and the hub notices the heartbeat stop —
the standard absence-inference idiom, surfaced as a gentle "Herald went quiet" on the dash rather
than an alarm. No solar, no cells, no charging chemistry — the
[solar sizing guide](./solar_power_sizing.md)'s own logic says a device an arm's reach from an
outlet is solar's wrong use case.

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
  alarm time is the precedent for owner-authored content arriving this way). Herald adds one
  topic, `message/set`, retained so a rebooting placard re-renders the current note.
- **Authoring:** an HA MQTT `text` entity plus a handful of **canned quick-notes** ("Dinner's in
  the oven", "Trash night tonight", "Dishwasher is clean") exposed as one-tap buttons — because on
  a phone, in a checkout line, canned beats typed.
- **Off-LAN:** the owner's *own* remote path into HA (the same rule as the
  [alert relay](../design/alert_relay.md): the link points home; no vendor cloud ever carries the
  note).
- **Staleness is handled, not ignored.** A note rots — "dinner at 6:30" should not greet anyone
  at breakfast. Every note carries a **posted-at stamp rendered on the face** and a default
  **auto-expiry (hours, owner-tunable)** back to the resting face. That's the ambient-display
  standard's "last-known state must be labeled as such" invariant, applied to paper. The stamp is
  **coarsened to the fleet's 10-minute buckets** ("posted ~3:10 pm") per the metadata-minimization
  invariant (Invariant III, `AGENTS.md`) — an interior face leaks less than an exterior one, but
  the kernel's own timestamp discipline is the house style, and the staleness job needs nothing
  finer.

Owner-authored free text on fleet glass is still a first, and the scoping rule from the original
draft stands: the panel renders the owner's words *inside the owner's own kitchen*; signed content
and witness claims never render as notes; the firmware treats a note as opaque text with a stamp,
never as a claim. The [standard's](../standard/AMBIENT_DISPLAY_STANDARD.md) §2 honesty invariants
bind the *system-authored* face regions (status line, staleness, fault), which stay
firmware-controlled.

---

## 5 · The resting face — status that stays honest

When no note is posted, Herald is the kitchen's quiet truth line, AD-Core rules applied to paper:

| Face region | Content | Honesty rule it carries |
|---|---|---|
| Status line | "All 6 Canaries reporting · as of ~7:10 am" | **Silence ≠ safety**: a witness gone quiet degrades the line by deadline ("5 of 6 reporting — Garage quiet since ~6:50 am"), words and glyphs, no color needed |
| Weather corner | Hub's one retained forecast blob (`FleetSubs::WEATHER`) | Data the displays already consume; no new fetch path, nothing leaves the LAN |
| Note area | Resting: date, or the household's chosen line | Owner's surface, clearly separated from system-authored regions |
| Stamp | "as of ~HH:MM" on every render, 10-minute buckets | Last-known state is labeled as such — on a panel that *holds* an image, the stamp is what makes persistence honest rather than misleading |

A dead transport banners the face within one render cycle ("hub unreachable since ~7:20 am") —
that's AD-Core §2.1, and it's the difference between this and a picture frame.

---

## 6 · What it deliberately doesn't do

- **It's not the alert surface** (§2) — no flashing, no siren, no red ambitions. Alert-grade
  events belong to the dash, the watch, and the phone poke. Herald may *state* a fault ("Garage
  quiet since ~6:50 am") because that's honest signage, not alerting.
- **No mic, no camera, no speaker.** One-way, phone → counter. It replaces the sticky note, not
  the intercom, and it adds zero sensing surface to the kitchen.
- **No cloud rendering.** The TRMNL-style "server renders the face" architecture is right, but the
  server is **the hub on the LAN**, not a vendor: the hub (or the device itself for plain text)
  renders, and nothing about the household's notes transits anyone else's computer.
- **No engagement mechanics.** No threads, no reactions, no per-person targeting, no read
  receipts. The moment a message board grows those, it competes with the group chat and loses; a
  placard that does one thing stays glanceable.
- **Timestamps stay coarse** (§4) — the kernel's 10-minute discipline, even indoors.

---

## 7 · Hardware sketch & parts

Raw-tier sketch (the kit shape; the integrated tier is "buy the reTerminal E1001, print the
stand"):

| Part | Choice | ~Cost |
|---|---|---|
| MCU | XIAO ESP32-S3 (fleet standard; WiFi + BLE, SPI to spare) | $7 |
| Panel | 4.2" mono fast-partial EPD (SSD1683/UC8176-class) + driver board | $25–35 |
| Power | USB-C wall adapter + cable (or slim flat cable for the fridge mount) | $3–8 |
| Mount | printed counter easel **or** magnet fridge plate **or** wall plate | $2–3 |
| **Total** | | **~$40–55** |

Wiring is deliberately boring: EPD on SPI + busy pin — a fraction of the XIAO's pins, no level
shifting, nothing that violates the family's screwdriver-grade ethos. The three mounts share one
printed body (the [sign plate](./enclosure/canary_sign.scad) footprint family) with swappable
backs.

---

## 8 · The firmware path, when it graduates

Concept-status today; the shortest consistent path later, for the record:

1. A `canary-display` flavor (`firmware/configs/canary-display/herald/`) — `CD_DEVICE_TYPE
   "canary-herald"`, `FEATURE_EPD`, most of the LCD feature set compiled out.
2. An EPD HAL (`src/hal/display_epd.cpp`) behind the existing HAL seam, so the UI layer never
   learns what a busy pin is; partial/full refresh policy lives here.
3. Board registry rows + `pins/pins.h` for the chosen board, own OTA product string,
   `flavors.json` + size guard, Arduino-twin regen — the standard four-registry drill.
4. The `message/set` command topic in `mqtt_mgr.cpp`'s existing latch pattern + an HA `text`
   entity; canned quick-notes as HA buttons; the fleet-status line reuses the fleet model the
   displays already carry.
5. Conformance claim against the [ambient display standard](../standard/AMBIENT_DISPLAY_STANDARD.md):
   **AD-Core** (the §5 face is the honesty invariants rendered literally) + **AD-Calm** (free by
   physics: zero light, zero motion) + **AD-Resilient** (dead transport → bannered, stale-stamped,
   persistent face).

---

## 9 · Never let it rot & open items

- **Mostly reuse:** the display family's MQTT command idiom and fleet model, the retained-weather
  pattern, absence-inference for "Herald went quiet", the sign-plate enclosure footprint, alert
  relay untouched. New surface is honestly small: an EPD HAL and one command topic.
- **No bench unit.** The real unknowns: actual partial-refresh times and ghosting cadence on the
  candidate panels, the render split (hub-rendered bitmap vs. on-device text layout — the
  TRMNL-architecture question §2 defers), and whether the status line + weather + note fit a 4.2"
  face legibly or the 7.5" panel is the honest minimum.
- **Auto-expiry default** (4 h? 8 h?) wants real-life tuning — long enough for "dinner at 6:30",
  short enough that stale notes are rare. Owner-tunable regardless.
- **Adjacent, not overlapping:** the **dash** (glowing command glass, alerts, touch), the
  **nightstand** (bedroom light discipline), the **TV/Witness Wall** (big-screen). Herald is the
  no-glow, no-touch, paper corner of the same family. Noted so they don't blur.

---

## 10 · Postscript — the outdoor door-placard variant (deferred, not dead)

Herald was first drafted as an **outdoor door placard**: the static
[witness signage plate](./enclosure/canary_sign.scad) made dynamic — resting face = the sign's
honest disclosure lines ("PRIVACY WITNESS / presence sensing in use / no video is recorded or
stored" — *witnessed*, never "protected"), plus notes to visitors ("please leave the package by
the bench"). Moved indoors because the outdoor bill is real and the door's actual need is mostly
met by the static plate:

- **Cold:** standard e-ink refreshes only to 0 °C; wide-temp parts to −15/−25 °C
  ([cold-weather envelope](./cold_weather_envelope.md)).
- **Sun:** prolonged UV ages panels; unshaded doors need filtered windows.
- **Power:** the doorbell pair is a series leg through the chime — continuous draw needs a chime
  bypass/power kit or transformer-side tap; "no new wire runs," not "zero install."
- **The absence rule:** an exterior face must **never advertise absence** — no "back in 10
  minutes," ever — enforced in layers (system never authors absence; authoring-side nudge with
  deliberate override; the last word stays the owner's). This rule is the outdoor variant's
  load-bearing design constraint and comes back with it.

If the outdoor pull proves real (waitlist signal, or a jurisdiction where dynamic disclosure
earns its keep), it returns as a **weatherized flavor of the same firmware** — same panel class,
same `message/set` topic, plus the gasketed enclosure
([field ratings](./enclosure/field_ratings.md) discipline), wide-temp panel, and an LIS3DH
pry-off/`TamperDetected` story (the [Gatekeeper](./canary_gatekeeper_research.md) part). The
indoor build risks none of that and proves the whole message path first.

---

*Sources: [`tv_display_design.md`](./tv_display_design.md) for the reTerminal E1001/E1002 research
and the render-server architecture lesson; the
[display market research](../research/display_market_research.md) for the e-ink-is-not-alerting
line; the [ambient display standard](../standard/AMBIENT_DISPLAY_STANDARD.md) for the honesty
invariants and the "e-ink tile" conformance form; `AGENTS.md` Invariant III for the 10-minute
timestamp buckets; the [cold-weather envelope](./cold_weather_envelope.md) and
[`canary_gatekeeper_research.md`](./canary_gatekeeper_research.md) for the deferred outdoor
variant's constraints; the [alert relay](../design/alert_relay.md) for where alerting actually
lives. Fast-partial mono EPD timing (~0.3–1 s partial, 2–4 s full) is the vendor-quoted class
behavior for SSD1683/UC8176-era 4.2" panels (GoodDisplay/Waveshare).*
