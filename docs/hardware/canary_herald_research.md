# Canary Herald — plug-in e-paper kitchen placard (research & design dossier)

**Status:** concept — a design, **no firmware, no bench unit**. Family-wise a `canary-display`
sibling (a display that shows instead of senses) — the family's first **e-paper** member, the
"e-ink tile" the [ambient display standard](../standard/AMBIENT_DISPLAY_STANDARD.md) already names
as a conforming form. Drafted first as an outdoor door placard; **moved indoors** once the honest
costs of outside stacked up against what the door actually needed — the deferred outdoor variant
lives in §12.

**The one-sentence version:** a plug-in e-paper placard for the kitchen counter or the fridge —
the household's taped-up note, writable from your phone ("take the chicken out of the freezer") in
about a second, sharing its face with a calm, honest fleet-status line and the morning's weather —
no glow, no camera, no mic, no cloud, no subscription, and the note survives an unplug.

---

## 1 · Why the kitchen gets e-paper

The kitchen counter is the household's real message hub — it's where the taped note, the fridge
magnet, and the "DISHWASHER IS CLEAN" sticky already live. Two jobs, both squarely in the fleet's
lane:

- **The household note.** One-way, phone → counter: *dinner at 6:30 — lasagna, trash night
  tonight, call Grandma back, the dishwasher is CLEAN*. Typed from the couch, the office, or the
  grocery-store aisle; on the counter before the phone is pocketed.
- **The calm status corner.** The dash and nightstand are glass that *glows* — right for a
  command surface and a bedroom. The kitchen wants the opposite: a face readable all day and
  invisible all night, carrying the standard's honesty invariants at a glance — "all 6 Canaries
  reporting · as of ~7:10 am" — and visibly degrading when that stops being true
  ([silence ≠ safety](../standard/AMBIENT_DISPLAY_STANDARD.md)).

But a kitchen display is a crowded graveyard, so before any hardware: why did everything before
it die, and what has to be different this time?

---

## 2 · The graveyard — and the sticky note that outlived it

Every product category that tried to own the kitchen's message surface failed the same few ways
(the [display market research](../research/display_market_research.md) has the receipts for the
ambient-display half):

- **The subscription treadmill.** TRMNL is $139 *plus $5/month* just to refresh faster than every
  15 minutes. Skylight/Hearth-class family calendars run $300+ with feature-gating
  subscriptions. A note on the counter that bills monthly loses to a $0.02 sticky note forever.
- **The cloud that dies.** Tidbyt's servers were the product; when the company was acquired and
  paused, every unit on every counter needed a community rescue (Tronbyt) to keep working.
  Cloud dependency is product end-of-life risk — the market research's own conclusion.
- **The counter becomes ad space.** The big-platform smart displays (Echo Show-class) rotate
  promotions and "suggestions" on their resting face. A surface the household must trust at a
  glance cannot also be somebody's ad inventory.
- **The family-CTO problem.** Calendar-syncing, account-per-person, app-per-reader products get
  configured by one parent and read by nobody. Every account added before the first useful
  glance is a family member lost.
- **The attention tax.** Glowing screens in the kitchen demand looking; notification-based
  "family apps" demand phones. Both lose to the note that just *sits there* until you walk past.

And the humble sticky note survived them all, because it has the five properties none of them
kept: **zero setup, zero learning, zero accounts, visible in place, and it never notifies
anyone.** Its actual failures are few and specific: you can't write it remotely, it never expires
(stale notes pile up), and it says nothing about the house.

**The bar, as a design contract.** Herald must lose to the sticky note *nowhere* — and beat it in
exactly four places: writable from anywhere, expires itself, always legible, and carries the
fleet's honest status line. Concretely:

1. **One minute, one person, once** — total setup cost for the household (§6).
2. **Zero seconds, zero setup, zero accounts for everyone else** — a guest, a kid, a grandparent
   reads it the way they read paper. If any reader needs an app, we've failed.
3. **No subscription — structurally.** There is nothing to subscribe *to*: the hub is the owner's,
   the render path is LAN-only, the panel draws ~nothing.
4. **No server to sunset.** The Tidbyt failure is impossible by construction, not by promise: the
   device keeps working on a LAN that never touches a vendor.
5. **The resting face sells nothing, ever.** It shows the household's own truth (status, weather,
   note) — the anti–Echo-Show.

---

## 3 · Better than a text — at exactly one job

"Just text the group chat" is the real competitor, and Herald beats it only at the **household
broadcast** job — which is precisely the job group chats are worst at:

| | Group text | Herald |
|---|---|---|
| Sender cost | pick thread, type, send — then re-send when ignored | one tap (canned), share sheet, voice line, or type once (§6) |
| Receiver cost | unlock, open app, find thread, scroll past chatter | a glance, in passing, hands full |
| Readers without a phone | excluded (kids, grandparents, guests, wet hands) | everyone in the kitchen |
| Where it's read | wherever the phone is — usually not the kitchen | exactly where the action is |
| When it's read | when sent (then forgotten) | when relevant — walking past, about to cook |
| Staleness | scrolls away, or haunts the thread forever | stamped, auto-expires (§6) |
| Side effects | the kid checking your note is now on YouTube | none — paper has no feed |
| Conversations | yes — that's what it's for | **refused, on purpose** (§8) |

The last row is the honesty line: Herald is *not* better than texting. It's better than texting
**at broadcasting one note to a place**, and it refuses every other messaging job so it can stay
better at that one.

---

## 4 · Why e-paper — and its honest limits

- **Calm by physics.** No backlight, no glow at 2 am. The nightstand line spends real engineering
  making an LCD honest at night; e-paper gets AD-Calm's night floor for free, at zero lux.
- **Readable where kitchens are bright.** Reflective like paper — the sunlit counter that washes
  out an LCD makes e-paper *easier* to read.
- **The note survives an unplug.** Cord kicked out, breaker flipped, outage: the face keeps its
  last note and stamp indefinitely. The failure mode is honest — never blank-and-ambiguous, just
  visibly stale (§7).
- **Nothing to burn in, near-zero draw between refreshes, decade-class panel life.**

The honest limits, stated up front because the repo already wrote them down:

- **Not an alert surface.** The market research line stands: e-ink is *calm and zero-light but too
  slow for alerting*. Alerting lives on the dash/watch and the
  [alert relay](../design/alert_relay.md). Herald renders **notes and ambient status** — content
  where a one-second update is instant and a missed update is a stale note, not a missed intruder.
- **"Instantly" means partial refresh.** A modern fast-partial mono panel (SSD1683/UC8176-class
  4.2", or the reTerminal E1001's 7.5") redraws a text region in **roughly 0.3–1 s** without the
  full black-white flash; a **full refresh (~2–4 s, with the flash)** runs every N partials and
  nightly to clear ghosting. Message-board rate, honestly labeled.
- **Mono for v1.** Color e-paper's minutes-long refreshes kill the typed-note job; on mono, the
  standard's "color never carries meaning alone" rule is satisfied by construction.

Two honest hardware tiers, mirroring the Vision Pro/Lite split:

- **Integrated tier — Seeed reTerminal E1001** (~$79): ESP32-S3, 7.5" 800×480 mono e-paper,
  enclosure included, already flagged in the [TV display design](./tv_display_design.md) as the
  architecture worth stealing (dumb panel, server renders). Fastest path to a bench unit.
- **Raw tier — XIAO ESP32-S3 + 4.2" fast-partial panel + driver board** (~$25–35 in parts): the
  fleet's own MCU family, a smaller face, our own printed stand (§9) — the long-term kit shape.

---

## 5 · Power — plug-in is the feature, not a compromise

E-paper draws ~nothing between refreshes, so a battery build is *possible* — and that's exactly
the trap: the battery e-ink products poll on 15-minute schedules to survive (that's what TRMNL's
subscription is *for*), which kills the one thing that makes the note delightful. Mains power
buys an **always-awake radio holding an MQTT session**: you type it, the counter updates before
you've pocketed the phone. Push, not poll. The display was never the power problem; the
listening was.

Indoors this is the easiest power story in the fleet: a **USB-C wall adapter** at the counter
(the [outlet cradle](./enclosure/canary_outlet_cradle.scad) family covers the outlet-adjacent
form), or a slim flat cable to a fridge-mounted unit. On real power loss the panel keeps its face
(§4) and the hub notices the heartbeat stop — absence-inference, surfaced as a gentle "Herald
went quiet" on the dash. No solar, no cells: the [solar guide](./solar_power_sizing.md)'s own
logic says a device an arm's reach from an outlet is solar's wrong use case.

---

## 6 · Ridiculously easy — the whole point, mechanized

Ease is the product. Every mechanism below exists to hit the §2 contract numbers: one minute of
setup for one person, zero for everyone else, and *sending faster than texting*.

**Setup — one minute, once.** First boot, the e-paper face shows a QR code (the family's
QR-onboarding motif, pointed the other way: the phone scans the placard). Scan → join → the face
prints "I'm yours — add the widget, or bookmark herald.local." Done. No account was created
anywhere in that sentence.

**Sending — every path faster than a group text, pick any:**

- **One-tap canned notes** from a home-screen widget: the household's five favorites ("Dinner's
  in the oven", "Trash night tonight", "Dishwasher is clean"). Cost: one tap. No thread to pick —
  **the address is "the house."**
- **Share sheet** from anything — a recipe, a calendar entry, a text someone *did* send you —
  straight to the counter.
- **A voice line** ("note the house: dinner at 6:30") via the phone's assistant shortcut → HA.
- **Typed**, from the HA text entity or the device's own local page — the long way, still one
  field and no recipient-picking.
- **Works without HA:** the device's local web page (the `glass_web` pattern) covers a
  hub-less household on the LAN; HA adds the widget/voice/share conveniences and the owner's own
  remote path when away. The fleet rule holds either way: no vendor cloud ever carries a note.

**Receiving — zero anything.** A glance, in place, hands full. That asymmetry — sender does one
tap, *every* receiver pays nothing — is what no app-based family product ever achieved, because
apps must charge the reader (an install, an account, a notification) to exist.

**Clearing — the crumple gesture.** One physical button on the placard: **"got it."** The person
who takes the chicken out taps it; the note clears back to the resting face, and the retained
topic updates so the sender's HA quietly shows "cleared ~4:10 pm." One button, household-level,
anonymous — the e-paper equivalent of crumpling the note. (What it is *not*: read receipts —
§8.)

**The transport underneath** is the fleet's existing owner→device write pattern, nothing new:

```
widget / share sheet / voice / HA ──(LAN)──▶ MQTT securacv/<id>/message/set (retained)
                                                  │                    ▲
                                                  ▼                    │ cleared/expired
                                      Herald (always-on MQTT session) ─┘  ("got it" button)
                                      partial refresh ≈ under a second
```

The command-topic idiom already exists in `canary-display` (`mqtt_mgr.cpp`'s exact-match
own-topic latches — the nightstand's owner-set alarm time is the precedent for owner-authored
content). Retained, so a rebooting placard re-renders the current note.

**Staleness is handled, not ignored.** A note rots — "dinner at 6:30" should not greet anyone at
breakfast. Every note carries a **posted-at stamp on the face** and a default **auto-expiry
(hours, owner-tunable)**. The stamp is **coarsened to the fleet's 10-minute buckets** ("posted
~3:10 pm") per Invariant III (`AGENTS.md`) — the kernel's own timestamp discipline, and the
staleness job needs nothing finer.

Owner-authored free text on fleet glass is still a first, and the scoping rule stands: the panel
renders the owner's words *inside the owner's own kitchen*; witness claims never render as notes;
the firmware treats a note as opaque text with a stamp, never as a claim. The
[standard's](../standard/AMBIENT_DISPLAY_STANDARD.md) §2 honesty invariants bind the
*system-authored* face regions, which stay firmware-controlled.

---

## 7 · The resting face — status that stays honest

When no note is posted, Herald is the kitchen's quiet truth line, AD-Core rules applied to paper:

| Face region | Content | Honesty rule it carries |
|---|---|---|
| Status line | "All 6 Canaries reporting · as of ~7:10 am" | **Silence ≠ safety**: a witness gone quiet degrades the line by deadline ("5 of 6 reporting — Garage quiet since ~6:50 am"), words and glyphs, no color needed |
| Weather corner | Hub's one retained forecast blob (`FleetSubs::WEATHER`) | Data the displays already consume; nothing leaves the LAN |
| Note area | Resting: date, or the household's chosen line | Owner's surface, clearly separated from system-authored regions |
| Stamp | "as of ~HH:MM" on every render, 10-minute buckets | Last-known state labeled as such — on a panel that *holds* an image, the stamp is what makes persistence honest |

A dead transport banners the face within one render cycle ("hub unreachable since ~7:20 am") —
AD-Core §2.1, and the difference between this and a picture frame.

---

## 8 · What it deliberately doesn't do

- **It's not the alert surface** (§4) — no flashing, no siren, no red ambitions. Herald may
  *state* a fault ("Garage quiet since ~6:50 am") because that's honest signage, not alerting.
- **No mic, no camera, no speaker.** One-way, phone → counter. It replaces the sticky note, not
  the intercom, and adds zero sensing surface to the kitchen.
- **No cloud rendering.** The TRMNL-style "server renders the face" architecture is right, but
  the server is **the hub on the LAN**, not a vendor.
- **No engagement mechanics.** No threads, no reactions, no per-person targeting, no read
  receipts — the "got it" button is household-level and anonymous by design (who tapped it is
  deliberately unknowable). The moment a message board grows conversation features, it competes
  with the group chat and loses (§3); a placard that does one thing stays glanceable.
- **No ads, no upsell, no "suggestions" — structurally.** The resting face is rendered by the
  owner's hub from the owner's data. There is no channel by which anyone else's content could
  reach it.
- **Timestamps stay coarse** (§6) — the kernel's 10-minute discipline, even indoors.

---

## 9 · Hardware sketch & parts

Raw-tier sketch (the kit shape; the integrated tier is "buy the reTerminal E1001, print the
stand"):

| Part | Choice | ~Cost |
|---|---|---|
| MCU | XIAO ESP32-S3 (fleet standard; WiFi + BLE, SPI to spare) | $7 |
| Panel | 4.2" mono fast-partial EPD (SSD1683/UC8176-class) + driver board | $25–35 |
| "Got it" button | one panel-mount momentary switch, debounced in firmware | $1 |
| Power | USB-C wall adapter + cable (or slim flat cable for the fridge mount) | $3–8 |
| Mount | printed counter easel **or** magnet fridge plate **or** wall plate | $2–3 |
| **Total** | | **~$40–55** |

Wiring is deliberately boring: EPD on SPI + busy pin, one GPIO for the button — a fraction of the
XIAO's pins, nothing that violates the family's screwdriver-grade ethos. The three mounts share
one printed body (the [sign plate](./enclosure/canary_sign.scad) footprint family) with swappable
backs.

---

## 10 · The firmware path, when it graduates

Concept-status today; the shortest consistent path later, for the record:

1. A `canary-display` flavor (`firmware/configs/canary-display/herald/`) — `CD_DEVICE_TYPE
   "canary-herald"`, `FEATURE_EPD`, most of the LCD feature set compiled out.
2. An EPD HAL (`src/hal/display_epd.cpp`) behind the existing HAL seam; partial/full refresh
   policy lives there. The "got it" button reuses the family's existing debounced-input pattern.
3. Board registry rows + `pins/pins.h`, own OTA product string, `flavors.json` + size guard,
   Arduino-twin regen — the standard four-registry drill.
4. The `message/set` command topic in `mqtt_mgr.cpp`'s existing latch pattern + an HA `text`
   entity; canned quick-notes as HA buttons; QR onboarding face at first boot; the fleet-status
   line reuses the fleet model the displays already carry.
5. Conformance claim against the [ambient display standard](../standard/AMBIENT_DISPLAY_STANDARD.md):
   **AD-Core** (the §7 face is the honesty invariants rendered literally) + **AD-Calm** (free by
   physics) + **AD-Resilient** (dead transport → bannered, stale-stamped, persistent face).

---

## 11 · Never let it rot & open items

- **Mostly reuse:** the display family's MQTT command idiom and fleet model, the retained-weather
  pattern, absence-inference for "Herald went quiet", the QR-onboarding motif, the sign-plate
  enclosure footprint. New surface is honestly small: an EPD HAL, one command topic, one button.
- **No bench unit.** The real unknowns: actual partial-refresh times and ghosting cadence on the
  candidate panels; the render split (hub-rendered bitmap vs. on-device text layout — the
  TRMNL-architecture question §4 defers); whether status + weather + note fit a 4.2" face legibly
  or the 7.5" panel is the honest minimum; and the §2 contract numbers measured for real — the
  one-minute setup and one-tap send are **claims to bench, not vibes**: a usability pass
  (the [display usability protocol](./display_usability_protocol.md) pattern) with a stopwatch
  and someone who didn't build it.
- **Auto-expiry default** (4 h? 8 h?) wants real-life tuning. Owner-tunable regardless.
- **An optional third resting line** (next calendar event, via an owner-written HA template)
  is tempting and cheap — but it's the first step down the Skylight path, so it stays an opt-in
  open question, not a default.
- **Adjacent, not overlapping:** the **dash** (glowing command glass, alerts, touch), the
  **nightstand** (bedroom light discipline), the **TV/Witness Wall** (big-screen). Herald is the
  no-glow, no-touch, paper corner of the same family.

---

## 12 · Postscript — the outdoor door-placard variant (deferred, not dead)

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

*Sources: the [display market research](../research/display_market_research.md) for the TRMNL
pricing/refresh trade, the Tidbyt acquisition-pause and Tronbyt community rescue, and the
e-ink-is-not-alerting line; [`tv_display_design.md`](./tv_display_design.md) for the reTerminal
E1001/E1002 research and the render-server architecture lesson; the
[ambient display standard](../standard/AMBIENT_DISPLAY_STANDARD.md) for the honesty invariants
and the "e-ink tile" conformance form; `AGENTS.md` Invariant III for the 10-minute timestamp
buckets; the [cold-weather envelope](./cold_weather_envelope.md) and
[`canary_gatekeeper_research.md`](./canary_gatekeeper_research.md) for the deferred outdoor
variant; the [alert relay](../design/alert_relay.md) for where alerting actually lives.
Skylight/Hearth-class subscription calendars and Echo Show-class resting-face promotions are the
widely documented category behavior the §2 contract is written against. Fast-partial mono EPD
timing (~0.3–1 s partial, 2–4 s full) is the vendor-quoted class behavior for SSD1683/UC8176-era
4.2" panels (GoodDisplay/Waveshare).*
