# The Care Wave — what the display learned from every product it replaces

Wave 4 of the Canary Display ("considerate + caring"). Each section below is
a design decision traced to the market research that motivated it: a sweep of
baby-monitor parent units, security-panel displays, ambient smart displays,
and aging-in-place monitoring products (2025–2026), synthesized against what
the display already shipped. The recurring lesson: **the market's most hated
behaviors are all attention failures** — waking people for maintenance,
hiding state to be polite, or going quiet without saying so. Every feature
here is an attention policy, not a widget.

Feature flags: `FEATURE_CARE` (§1–§3, §5–§8), `FEATURE_RHYTHM` (§4).
Pure logic lives in `include/canary/care/{attention,rhythm}.h` (host-tested);
wiring in `src/care/care_glue.cpp`; per-witness state in the fleet model.

---

## §1 The clock is the idle face (watch)

When all is quiet and the clock is valid, the watch's halo hero is the
**time**, with `all quiet · N canaries` beneath it. Every comparable ambient
display converges on clock-as-idle-anchor (Nest Hub, Apple StandBy, IQ Panel
screensaver) because it earns the glance: people check the time 20× a day,
and each check absorbs the security state peripherally — calm technology's
"inform from the periphery" done literally. Any Warn+ state reclaims the
hero for the severity word; the small clock returns at the bottom.

## §2 The attention policy (`care/attention.h`)

The single most emotionally charged complaint across EVERY product category
researched is the 2 a.m. maintenance chirp — SimpliSafe's un-silenceable
"no link to dispatch," Honeywell's low-battery beeps, baby monitors'
mandatory out-of-range triple-beep. The policy, stated once, host-tested:

- **Tier 1** (unacked Alert/Tamper) is the only sound allowed during quiet
  hours. It re-voices until acknowledged, **ramping soft → mid → full**
  across voicings (`chime_play(c, ramp)`): wake gently, escalate honestly.
  No product ships a ramp; the complaint pairs it answers ("siren scared
  everyone" / "alarm too quiet to wake me") are both in the research.
- **Tier 2** (Warn: stale witness, low battery) sounds by day. At night it
  is **suppressed into the overnight ledger** — no sound, full visual
  honesty, nothing lost.
- **All-clear** only sounds by day, once, on resolution.

## §3 The overnight ledger + morning summary

What quiet hours silenced, the morning says: *"While you slept: 2 notices"*
on the watch's badge line and the dash's day line, with rows deduplicated
per witness ("Kitchen · went quiet"). The household acknowledge clears it —
reading the summary IS the ack. This is the answer to the "what did I miss"
anxiety that makes people disable night modes entirely.

## §4 The rhythm line (`care/rhythm.h`, `FEATURE_RHYTHM`)

The strongest reassurance UX in aging-in-place research is Alarm.com
Wellness' **green/yellow/red versus the home's own learned baseline** — and
since Alexa Together's shutdown (May 2025) stranded the consumer slot, nobody
pairs it with an in-home ambient display. The display now learns one honest
signal on-device: **first stir** — the first Notice-class activity between
03:00 and noon, EWMA across days (3-day learning period, empty days skipped).

- `Morning rhythm ✓ · first stir 06:42` — the home moved, all is routine
- `Quiet past the usual wake (06:45)` — worth a glance (usual + 45 min)
- `Still quiet — well past usual (06:45)` — worth a call (usual + 2 h);
  the "long lie" case the entire elder-care category exists to catch

It is a wellbeing **surface**, never an alarm: no sound, no event, no
publish. The baseline persists in NVS and never leaves the device.

## §5 Escalation on no-ack

No passive-sensor product does multi-step escalation (notify the sibling if
the primary doesn't answer); it exists only in dead-man-switch apps. The
display closes the loop with what it already has: a Tier-1 condition
unacknowledged for `CD_ESCALATE_UNACKED_MS` (default 15 min) publishes ONE
non-retained `securacv/fleet/escalation` event (`{at, by, worst, witness}`).
A household automation decides who else to wake — the display only reports
that nobody here answered. Ack or resolution resets the episode.

## §6 Per-witness mute + Roll Call

**Mute** is the security panel's zone bypass, done honestly. Long-press ON a
witness (watch: its detail page; dash: its card) mutes it until morning
(next `CD_QUIET_END_HOUR`, persisted to NVS so a power blip can't resurrect
the nag — or forget the bypass). Muted witnesses stay visible — hairline
arc/spine, "muted" state — because silent forgotten bypasses are how real
alarms get missed. **Tamper and a failed chain verify punch through a mute
at full severity**: mute quiets nags, it never un-knows an attack. A muted
witness's Alert does not cancel a standing household ack; its Tamper does.

**Roll Call** is the IQ-Panel-grade walk test no ambient display ships:
per-witness last-word age, battery, and self-reported WiFi RSSI (watch: its
own page; dash: tap the headline). Rows light green as each canary answers —
walk the house and watch them report.

**§6b Witness detail sheet** (dash): tapping a Roll Call row opens one
canary's whole story — its isometric figure at poster size (the same
committed-CAD drawing every other surface uses, `fleet_figures_art.h`;
an unresolvable wire type shows no figure, never a guess) beside every
honest field the fleet model holds: state + age (+ muted), signal word,
battery, room feel, a card-bearing witness's claim strip, chain verdict +
length, which bands carry it right now (`wifi + mesh + ble`, or "your
hub"), and the last event. The sheet stays live while open, like Roll Call
itself. The signed-chain QR stays one tap deeper ("prove it"), so the
spec §1 cryptography is unchanged — the sheet adds the story, not a new
trust surface.

## §7 The transparency page

Research consensus: people accept monitoring whose shape they can SEE, and
elders in particular want a mirror of "what does this thing say about me" —
validated in the literature, implemented nowhere. The display carries it on
the glass itself (watch: last page; dash: tap the footer):

> Watches: N canaries, via your broker only ·
> Speaks: a liveness heartbeat and household acks ·
> Keeps: N events on this device — erasable in History ·
> Never: cloud, camera, microphone, or your MAC

The dash sheet also carries the live network truth — which WiFi, the signal
as a word, and the glass's own `.local` address (composed by the same
`canary/net/hostname.h` recipe mDNS registers, so it always resolves) —
because "what network am I on" belongs where the honesty lives. And it hosts
**cleaning mode** ("wipe the glass": 30 s touch lockout with countdown) — a
wet cloth must never acknowledge an alarm.

## §8 Small courtesies from the same research

- **Ack attribution**: "acked · kitchen-dash" — the who-disarmed audit line
  panels keep, riding the ack-sync payload's existing `by` field.
- **Room comfort**: temp/humidity on detail lines when a witness publishes
  it (parent-unit table stakes; display renders, never measures).
- **Emergency contact** (`CD_EMERGENCY_CONTACT`, secrets.h): during an
  unacked Tier-1 the dash footer says who to call — a panel dispatches, a
  witness display informs the person standing in front of it.
- **Glance-first wake**: waking the watch from the dark always lands on the
  halo's one big fact, never mid-rotation — the distance-tiered rendering
  pattern (Nest Hub, Echo Adaptive Content), keyed on the wake edge.

## Deliberately not built

Camera feeds on the glass (against the witness philosophy; video is the
least-accepted modality in the care literature), voice/TTS, widget grids,
arming/entry-delay UX (this is a witness, not an alarm panel), cloud
weather, and rhythm-verdict *sounds* (reassurance must never nag). An
ambient-light sensor (Nest Ambient EQ is the category's most-praised
feature) is a BOM note for the next hardware rev, not firmware.
