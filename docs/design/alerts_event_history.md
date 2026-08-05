# Alert freshness & event history — the lifecycle that keeps the list alive (design)

Status: **core built, roadmap scoped.** The alert lifecycle this doc specifies
(open → resolved, seen/unseen, honest aging, relaunch continuity) shipped with
this doc in `ios/` — see §5 for exactly what is code and where. The rest (§6)
is design with named dependencies, most of them already owned by
[`apple_watch_and_notifications.md`](apple_watch_and_notifications.md) phases
N0/R1 and [`alert_relay.md`](alert_relay.md).

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

## 6. Roadmap (design, with owners)

Ordered by leverage-per-risk; each names its industry precedent and its
dependency.

1. **All-clear as a ledger event.** When an open record resolves, the row
   already flips to "Cleared"; add the *transition* to the Today timeline
   ("Front Porch back online") as a digest-tier entry — never a push. The
   status-page lesson: silence must not be the only "it's fine" signal.
   No dependency; next slice of this doc.
2. **Snooze with chosen durations.** Mute is fixed at 1 hour on every path
   today. Offer 1 h / until tonight / until morning, per witness and
   fleet-wide, always expiring, never an untimed off (untimed mute is how
   cameras become decorative — Ring's moon button got this right). Tamper
   punch-through preserved by construction (`Witness.effectiveSeverity`).
3. **"While you were away" reconciliation.** On foreground, diff the ledger
   against the last-seen stamp and lead the tab with one line — "3 things
   happened, 1 still needs you." The industry's recovery story is "hope the
   user scrolls"; ours can be explicit because the ledger is durable state
   and notifications are only its projection (the PagerDuty model).
4. **Heartbeat ingestion.** `Heartbeat.recordBeat()` still has no device
   signal feeding it — the dead-man's-switch can only go green from a manual
   test, and resets to unknown on relaunch. Wire the fleet's liveness into
   beats, persist `lastVerified`, and the "provably alive" card becomes true
   continuously. (Owned by `apple_watch_and_notifications.md` N0 + the
   relay-terminus question in `iphone_companion_app.md` §10.7.)
5. **Escalation, top tier only.** An unacked tamper/integrity alarm re-alerts
   once after a short window, then reaches the second household member —
   never applied below the top tier; rationing escalation is what keeps it
   meaning something (PagerDuty; `iphone_companion_app.md` §5b rule 5).
   Depends on the relay (R1) for the second-person leg.
6. **Actioned-rate self-tuning, locally.** Count per-class ack/dismiss rates
   on the phone; when a class is dismissed ~95% of the time, *offer* its
   demotion to digest ("you dismiss almost all of these — stop pushing
   them?"). No vendor ships this; for us it is a UserDefaults counter and
   one sheet, and the data never leaves the device (Invariant IV).
7. **Per-witness rules + quiet hours.** `AlertRule` is global-by-severity
   today; the doctrine doc already promises per-witness reach and
   quiet hours (Digest/Important only — Critical is the smoke alarm).

Deliberately **not** adopted, with reasons: familiar-face suppression (Nest's
best anti-fatigue feature is an identity substrate — Invariant II forbids the
mechanism, not just the misuse); cloud retention ladders (our retention is a
product decision, not a price sheet); AI notification text (the vocabulary is
the dictionary, deterministic and CI-locked).

## 7. Invariant notes

- Every lifecycle timestamp is a coarse 10-minute bucket (III). Resolution
  and seen stamps included — nothing here learns a second.
- The ledger, the counters, and any future actioned-rate data live on the
  phone (IV). Nothing in this design adds a wire format; the wrist's
  `resolved` flag rides the existing additive-optional snapshot contract.
- Clearing history removes the phone's notebook only; the sealed witness
  chain on the devices is untouched, and the UI says so in the confirmation
  dialog (rule 4: never oversell, never let a delete look bigger than it is).
