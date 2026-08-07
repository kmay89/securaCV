# Alert freshness & event history — the lifecycle that keeps the list alive (design)

Status: **built end to end.** The lifecycle (open → resolved, seen/unseen,
honest aging, relaunch continuity) shipped first; the seven surfaces that
turn it into something a user experiences — the all-clear, chosen snooze
durations, "while you were away", heartbeat ingestion, escalation,
local actioned-rate tuning, and per-witness rules with quiet hours — shipped
in the change that rewrote §6 into a code map. See §5 for what is code and
where. The last rung — reaching a SECOND PERSON when nobody answers — is built
too (§6), over CloudKit sharing rather than the hub relay, which is why
[`cloudkit_backend.md`](cloudkit_backend.md) §5 now carries a reversed
"never" and a §6.5 arguing it.

This doc answers one product question: **how does the Alerts surface stay
useful for years — showing what needs you and what you missed, never a museum
of stale alarms?** It is the freshness half of the notification doctrine; the
delivery half (tiers, quiet-by-default, the wake path) stays where it is.

Grep tokens: `alert lifecycle`, `needs you`, `resolvedBucket`, `seenBucket`,
`retention sweep`, `relaunch fold`, `episode collapse`, `alert fatigue`.

---

## 1. The problem, found in our own code

The Alerts tab (`ios/Sources/SecuraCV/Views/AlertsView.swift`) already kept
honest history — the `AlertLedger` records every rule-crossing with how far it
actually reached the user. But an audit of the freshness story found the tab
would rot in exactly the way the user asked us to prevent:

- **No time-based retention at all.** The ledger capped at 200 records and
  nothing else — a row from a year ago survived indefinitely, and `clear()`
  existed with no caller. The tab's answer to "what needs me?" degraded into
  "everything that ever happened."
- **"Still happening" and "over" looked identical.** A record never learned
  that its condition cleared. The "Needs you" section held every unhandled
  row forever, so urgency decayed into backlog — the precise mechanism of
  alert fatigue (clinical literature: 49–96% of interruptive alerts get
  overridden once a list stops meaning *now*).
- **Every relaunch re-notified ongoing alarms.** The news-dedupe state
  (`postedAlerts`/`ackedAlerts`) was memory-only, so a persistent condition
  re-posted after every cold start — restart spam through the one door the
  per-refresh guards didn't cover.
- **No read state, no badge.** `unhandledCount` existed but nothing rendered
  it; "did I miss something?" had no answer shorter than reading the list.
- **No per-day shape.** Binary Needs-you/Earlier meant last Tuesday read the
  same as ten minutes ago.

## 2. What the industry does (survey, August 2026)

We surveyed Ring, Google Nest/Home, Arlo, Eufy, UniFi Protect,
SimpliSafe/ADT, Wyze, Apple (HomeKit Secure Video + platform notification
machinery), the ops world (PagerDuty/Opsgenie), and the anti-nag device-state
designs (Tesla Sentry, Find My/Tile separation alerts).

**Table stakes everywhere:** per-device and per-type toggles; an
"all other motion" bucket that can be silenced separately; zones; recording
decoupled from notifying; a timeline as the recovery surface.

**What only the best do:**

| Mechanism | Best-in-class example |
|---|---|
| Episode collapse — one notification per continuous cause | Ring "Single Event Alert" (2025, paid); PagerDuty time-based grouping a decade earlier |
| One open alert per cause, repeats increment a counter | Opsgenie dedup alias — at most one open alert per key, ever |
| Severity-aware snooze — mute the noisy tier, urgent still passes | Ring motion snooze (30 min–4 h; rings and priority alerts still get through) |
| Stateful ack/resolve lifecycle; ack stops re-notification for everyone | PagerDuty/Opsgenie; ADT's tap-to-cancel texts cut false alarms >50% |
| Transition-only device-health alerts, one-shot semantics | Tesla Sentry (push only on the Alarm tier, one per transition); Find My separation alerts with trusted-location suppression |
| Heartbeat — the *absence* of a signal is itself an alert | Opsgenie heartbeat monitoring; no consumer camera vendor ships this well |
| Presence-aware suppression ("when everyone is away") | UniFi Protect geofenced notifications; Nest Home & Away Routines |
| An explicit all-clear — silence is never the "it's fine" signal | Status-page resolve broadcasts |

**What everyone gets wrong**, which is our opening:

- Push is treated as the system of record — best-effort APNs/FCM with no
  missed-alert reconciliation, so silence is indistinguishable from "nothing
  happened."
- No ack concept in consumer apps: ten household members each triage the
  same event.
- The anti-fatigue fixes are sold back as subscriptions (Ring's episode
  collapse is Premium; Wyze's 5-minute cooldown is the free tier's *penalty*;
  Nest free keeps events 3 hours). Fatigue caused by the vendor, fixed for a
  fee.
- Old alerts either nag forever (offline loops) or vanish silently with no
  summary.

Local-first flips the economics: our collapse, retention, and lifecycle are
state machines over data the user already owns — free forever by
construction, which is the brand.

## 3. The doctrine (five rules of freshness)

Extends the delivery doctrine in `apple_watch_and_notifications.md` §1; same
spirit, applied to *time*:

1. **Urgency is about the present.** A row may claim "Needs you" only while
   its condition is live *and* unhandled. A condition that cleared on its own
   files itself into history — urgency never becomes a backlog chore.
2. **"Still happening" and "over" must never look the same.** Every surface
   that shows an alert shows which it is. This is the same honesty rule as
   "we told you" vs. "we couldn't tell you," pointed at time.
3. **Alerts are perishable; history is durable; neither impersonates the
   other.** Settled history the user has *seen* ages out after 30 days.
   What the user has never seen is never deleted by time — "we discarded the
   thing you missed" is the one failure a witness product may not commit.
   (Bounded by the cap either way.)
4. **A repeat reopens the whole lifecycle.** A condition that returns is
   news: unhandled again, unseen again, open again. An old acknowledgment
   never covers a new occurrence.
5. **Seen and handled are different questions.** Glancing at the list clears
   the badge and the dots; it acknowledges nothing. Acknowledging stops the
   urgency; it doesn't pretend the row was read everywhere.

## 4. The lifecycle model

One record per condition (the existing collapse key), now with a full state
loop:

```
            note()                    resolve()
  (new) ──────────────▶ OPEN ────────────────────▶ RESOLVED
            ▲            │  ack/mute → handled        │ seen + 30 days
            │            │  (stays open)              ▼
            └────────────┴──────── note() again   swept (or user removes)
                       "it came back = news"
```

- **Open → resolved** is decided by `FleetStore` at the same moment a calmed
  witness leaves the news-dedupe ledgers — "would notify again" and "shown as
  over" are one decision, so the tab and the notifications can never drift.
- **Seen** is stamped when the user leaves the Alerts tab (on the way out,
  not in, so rows don't reshuffle under the reader's thumb). The app badge
  *is* the unseen count.
- **Resolution never touches handling.** A cleared-while-you-were-out row
  still says nobody looked — that is the "you missed this" recovery surface,
  found at the top of history wearing its unseen dot.
- All lifecycle timestamps are 10-minute buckets (Invariant III), like every
  other time this record has ever known.

## 5. What shipped with this doc (code map)

| Piece | Where |
|---|---|
| `resolvedBucket` / `seenBucket` + `isOpen` / `needsYou` / `isUnseen` (additive optional — old ledgers decode unchanged) | `ios/Shared/AlertRecord.swift` |
| `resolve` / `markSeen` / `remove` / `retentionSweep` (30-day `retention`, unseen and open rows exempt) / `unseenCount` | `ios/Sources/SecuraCV/Model/AlertLedger.swift` |
| Relaunch fold: rebuild `postedAlerts`/`ackedAlerts` from open records so an alarm that outlives the app never re-posts as fresh news | `AlertLedger.foldOpenAlerts` + `FleetStore.init` |
| Resolution wired at the dedupe-prune site; badge sync | `FleetStore.evaluateAlerts()` / `syncBadge()` |
| App-icon badge = unseen count (deduped writes) | `AlertCenter.setBadge` |
| Day-sectioned history ("Earlier today" / "Yesterday" / dates), Ongoing/Cleared chips, unseen dots, settled-row removal, clear-history (settled rows only — a live alarm can be acked or muted, never made to vanish) with an honest "the sealed witness logs stay" dialog, seen-marking on tab exit | `ios/Sources/SecuraCV/Views/AlertsView.swift` + `AlertHistory.daySections` |
| Wrist parity: cap-aware needs-you-first rows (`AlertHistory.wristRows` — a live alarm never falls off the 12-row cap behind settled history), additive-optional `resolved` flag, "Cleared" chip on the watch | `WristSnapshot+App.swift`, `Shared/WristSnapshot.swift`, `SecuraCVWatch/Views/AlertsListView.swift` |
| Lifecycle tests (resolution, reopen, sweep exemptions, fold, day grouping, wrist cap) | `ios/Tests/SecuraCVTests/AlertHistoryTests.swift` |

## 5b. The freshness surfaces (the §6 roadmap, now code)

| Piece | Where |
|---|---|
| **All-clear as a ledger event.** Resolutions of the last day become Today entries ("Front Porch — Gone dark, now clear"), derived from the ledger at every fold so they can't drift from the tab, and digest-tier by construction — there is no code path from here to a notification. Wears the *unverified* badge: it is the phone's own observation, never one of the fleet's signed records. | `AlertFreshness.allClearEvents` → `FleetStore.refreshOnce()` |
| **Snooze with chosen durations.** 1 hour / until tonight / until morning, on the phone rows, the device screen, and the wrist. The menu is time-aware (`offered(at:)`) so "until tonight" at 10pm — which would mean *tomorrow* night — is never offered; the type has no untimed case, so an endless mute is unrepresentable rather than merely discouraged. Rows and the device screen say when the quiet ends. | `ios/Shared/AlertSnooze.swift`, `FleetStore.mute(_:duration:)`, `AlertRecord.mutedUntil`, `WristSync.muteDurationKey` |
| **"While you were away."** Unseen rows ARE the diff since the user last looked, so the tab leads with one honest sentence — "3 things happened while you were away — 1 still needs you" — instead of leaving recovery to scrolling. Computed on entry, gone on exit. | `AlertFreshness.awaySummary` → `AlertsView` |
| **Heartbeat ingestion.** A Canary answering feeds the beat, and so does any real alert iOS accepted. `lastVerified`/`lastBeat` persist, so a cold start no longer throws away a verification the user earned. | `Heartbeat`, `FleetBeat.heard`, `FleetStore.refreshOnce()` |
| **Escalation, top tier only.** One extra buzz for an unanswered tamper/integrity alarm, never below that tier, at most once per occurrence (the stamp is in the ledger, so "once" outlives a relaunch), and never at all once acknowledged. | `EscalationPolicy`, `FleetStore.escalateIfUnanswered`, `AlertRecord.escalatedBucket` |
| **Actioned-rate self-tuning, locally.** Two integers per severity on this phone count acks against mutes/dismissals; at ~90% dismissed over a real sample the rules sheet OFFERS a demotion. It never applies one, "keep them" is remembered per class, and taking the offer forgets the evidence so a re-armed rule is judged on what happens next. The offer names — and switches off — **every** rule that covers the class, because the shipped rules overlap on an alarm and turning off one would leave the next one pushing exactly what the button promised to stop. Tamper is never offered at any dismissal rate: the smoke alarm is not tunable. | `AlertTuning`, `AlertRule.pushing(for:in:)`, `AlertRulesSheet` |
| **Per-witness rules + quiet hours.** A floor per Canary (everything armed / only serious / only tamper) and a wall-clock quiet window. Both narrow; neither silences: `WitnessPushFloor` has no "never" rung and `QuietHours.silences` cannot return true for `.critical`. A held alert says which of the two held it. | `WitnessAlertPrefs`, `QuietHours`, `AlertCenter.level(for:awayFromHome:now:)` |
| Rule persistence (what the user arms now survives relaunch, folded onto the shipped rules so improved wording still reaches everyone) | `AlertRule.merge(stored:into:)`, `AlertCenter.init(defaults:)` |
| Policy tests (escalation rationing and the bucket-debt wait, quiet-hours midnight wrap and the critical exemption, floor ladder, tuning thresholds, rule merge) | `AlertPolicyTests.swift` |
| Freshness + heartbeat tests (all-clear derivation, away line, mute/escalation stamps, reopen clearing both, old ledgers decode, listening window, coalescing) | `AlertFreshnessTests.swift`, `HeartbeatTests.swift`, `AlertSnoozeTests.swift` |

### The one honesty problem this raised

Feeding fleet liveness into the heartbeat nearly made the card lie. "Delivery
verified" is a claim about **notifications reaching this phone**; a Canary
answering on the LAN proves the *fleet* is up and proves nothing about APNs.
So a beat carries its source (`WristBeatSource`), and the copy splits with it:
a check-in says "your fleet checked in", only an accepted delivery says
"delivery verified". The wrist gets the same split over an additive-optional
field, so the watch can never overstate what the phone told it.

The dead-man's-switch grew three guards. Two protect against crying wolf — a
false "your fleet went dark" is how a real one gets ignored — and one against
the opposite failure, staying green through a real outage:

0. **Only the fleet's silence counts.** The dark verdict reads
   `lastFleetCheckIn`, never `lastVerified`. Sharing one timestamp meant the
   notification *about* a Canary going dark refreshed the very timer that was
   supposed to be running out: the switch would have sat green while the
   fleet was gone. Found in review, and the reason the two clocks are now
   separate fields rather than one.
1. **Silence only counts while we were listening.** The app stops its radios
   in the background by design, so backgrounded time is not evidence.
   `noteListening()` restarts the window at every foreground.
2. **Silence only counts if something was expected.** With nothing paired
   there is no beat to miss, so the card says "not yet verified" instead of
   alarming about a fleet the user hasn't bought yet.

Two more honesty rules fell out of the same review, both worth stating because
they generalize:

- **A tolerant decoder degrades to the WEAKER claim.** `WristBeatSource`
  decodes an unknown future value as a fleet check-in, not a verified
  delivery — everywhere else in this codebase a tolerant enum clamps toward
  the *safe* reading, and for a claim about the user's safety net "safe"
  means promising less.
- **A live failure outlives a stale success.** Persisting verifications gave
  the card something old to fall back on, which meant "Test failed:
  notifications are off" was overwritten by "Delivery verified 3 days ago" on
  the very next refresh. A failed verdict now stands until the path actually
  works again — and a Canary checking in does not clear it, because it
  doesn't fix what broke.
- **Quiet hours are enforced where they are known.** Focus can be published
  through, because iOS enforces it on the receiving device; quiet hours
  cannot, because the notification extension that turns a wake into a banner
  has never heard of the setting. So the wake is held here, before it leaves
  — otherwise "quiet hours" would silence this phone and buzz the user's iPad
  at 3am.

And because the fleet answers every 20 seconds, beats coalesce (one a minute)
and the wire's copy is floored to 5 minutes — otherwise every refresh would
wake the widgets and the watch to say the same thing. Flooring can only make
a beat look *older* than it is, which is the honest direction for a liveness
claim to round.

## 6. The last rung: somebody who is not the owner

Built. If a top-tier alarm goes unanswered past the escalation window, a
person the owner invited is told.

The mechanism is CloudKit sharing rather than the hub relay
([`alert_relay.md`](alert_relay.md) §3), because for an Apple household it
needs no server, no account, and no third party: the owner invites people
through Apple's own sharing sheet, and their devices subscribe to one shared
zone. The hub relay remains the answer for reaching somebody who isn't on
Apple hardware, and for channels the owner picks themselves.

| Piece | Where |
|---|---|
| The rationing (only an escalation, only the top tier — asked independently of the caller that already passed `EscalationPolicy`), the roster's arithmetic, and every sentence anyone reads | `ios/Sources/SecuraCV/Cloud/HouseholdRelay.swift` |
| Zone + share lifecycle, publishing an escalation, the participant's own subscription, the sweep | `ios/Sources/SecuraCV/Cloud/HouseholdShare.swift` |
| Invite / roster / revoke, and what an invited person is told they are accepting | `ios/Sources/SecuraCV/Views/HouseholdSheet.swift` |
| The invitation's landing pad (`userDidAcceptCloudKitShareWith`) and `CKSharingSupported` | `SecuraCVApp.swift`, `ios/Support/Info.plist` |
| Wired at the end of the escalation, gated twice | `FleetStore.escalateIfUnanswered` |
| Tests: the rationing, the roster's refusal to count an invitation as a person, the zone boundary, and the copy's silence | `HouseholdRelayTests.swift` |

**Three properties worth keeping.** *One:* the shared zone holds one record
type and nothing else, so "a household member can't see your fleet" is
CloudKit's access control rather than our filtering — the moment anything else
is written there, the promise becomes a claim. *Two:* the participant's push
is worded by the participant's own device, because shared-database
subscriptions carry no record fields; nothing about the alert, not even its
sentence, crosses the wire. *Three:* the owner's quiet hours, Focus and
away-reach rules are deliberately **not** consulted — those govern whether
*this* user is interrupted, and a household member is a different person with
their own phone and their own settings.

This reverses a documented "never" in
[`cloudkit_backend.md`](cloudkit_backend.md) §5 (no shared database). The
reversal is argued in that doc's new §6.5 rather than performed quietly; the
old row's real justification was "not needed for anything above", and this is
the thing that needed it.

**Still not built, deliberately:** an ack that travels back from the household.
It would matter if escalation could fire twice, and it can't — the ledger's
stamp makes it at most once per occurrence — so the owner's app has nothing
left to stop. A household member who wants to *do* something opens their own
phone and calls the owner, which is what actually happens at 3am anyway.

Two smaller things left as they are:

- **Notification actions stay "Acknowledge" and "Mute 1 hour".** Durations
  live in the app, not on the lock screen: "until morning" tapped at 9am is a
  22-hour silence, and a notification action can't show what it would cost the
  way a menu can.
- **The wrist's swipe-to-mute is still one hour.** The duration menu is on the
  witness screen where there is room to read it; a swipe on a 40mm screen
  should do the safest thing.

Deliberately **not** adopted, with reasons: familiar-face suppression (Nest's
best anti-fatigue feature is an identity substrate — Invariant II forbids the
mechanism, not just the misuse); cloud retention ladders (our retention is a
product decision, not a price sheet); AI notification text (the vocabulary is
the dictionary, deterministic and CI-locked).

## 7. Invariant notes

- Every lifecycle timestamp is a coarse 10-minute bucket (III). Resolution,
  seen, mute-expiry and escalation stamps included — nothing here learns a
  second. (Heartbeat times are the documented exception the wrist contract
  already carries: link-health times keep operational precision, and the
  wire's copy is floored to 5 minutes anyway.)
- The ledger, the mute ledger, the per-witness floors and the actioned-rate
  counters all live on the phone (IV). The tuning counters are the only new
  behavioral data in the app and they are two integers per severity — no
  model, no upload, no profile, and nothing that records what an alert was
  about. Nothing in this design adds a wire format: the wrist's `resolved`
  flag, `lastBeatAt`/`beatSourceRaw`, and the mute-duration key all ride the
  existing additive-optional contract.
- Clearing history removes the phone's notebook only; the sealed witness
  chain on the devices is untouched, and the UI says so in the confirmation
  dialog (rule 4: never oversell, never let a delete look bigger than it is).
