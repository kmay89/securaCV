# iCloud as the backend we don't have (design)

**Status:** shipped, with one deployment step that has to be pressed by hand and
one open decision (§6.3). This is the canonical account of *why a local-first
privacy project has a cloud container at all*, what may and may not go in it,
which parts of Apple's CloudKit Console we use, and what the user gets out of
the arrangement.

**The one-sentence version:** SecuraCV runs no server, so the two things that
genuinely have to follow you between devices — **your fleet list** and a
**content-free "something needs you" wake** — ride your *own* iCloud private
database, which we cannot read, which needs no account of ours, and which
costs you nothing.

Companions: the [iPhone companion RFC](iphone_companion_app.md) §4–§5 set the
rule this doc implements; the [alert relay RFC](alert_relay.md) is the
vendor-neutral version of the same idea for the hub; [Apple Home &
the fleet](apple_home_integration.md) argues the identical timing residual for
HomeKit and is the precedent §6.1 leans on; [reaching your fleet from
away](../away_access.md) is the *other* remote problem (looking at the hub on
demand) and is deliberately solved a different way.

---

## 1 · Why a local-first product has a cloud container at all

Three problems cannot be solved on the LAN, and pretending otherwise ships a
product that lies in its own copy:

| Problem | Why the LAN can't | What we do |
|---|---|---|
| **Your second device.** You pair a Canary on your iPhone; your iPad knows nothing about it. | Two devices that are never on at the same time never meet. | The fleet list syncs through **your** iCloud private database. |
| **Alerts when you're not home.** | iOS will not keep a socket open in your pocket across town. Instant push on iOS must arrive through APNs — that is a platform fact, not a design choice. | A CloudKit subscription makes **Apple's** push service wake your devices. |
| **A device that's home has to be the one that notices.** | Nothing on your phone can see a Canary go dark from another city. | A resident device (phone on Wi-Fi, or better an always-on Apple TV / Mac / docked iPad) writes the wake. Same shape as a HomeKit home hub, and `AwayReach` says so plainly when nothing is home. |

Two answers were available and both were refused:

- **A SecuraCV server.** It would work, and it would create the thing this
  project exists to not create: a box we operate that knows which households
  have Canaries, when they alert, and how to reach them. There is no
  configuration of that box that is safer than not having it.
- **A hosted relay we run** (the ntfy-shaped option in
  [`alert_relay.md`](alert_relay.md)). Better, but it still holds a device-token
  registry — so "this household got some alerts" becomes learnable by whoever
  holds the registry, including whoever compromises it.

Riding the user's own iCloud beats both, and the reason is structural rather
than aspirational: **there is no third party in the path who is not already the
user's.** We hold no APNs key. We keep no device-token registry. There is no
server of ours to compromise because there is no server of ours.

---

## 2 · What is actually in the container

One container — `iCloud.com.securacv.witness`, named in exactly one place
(`ios/Sources/SecuraCV/Cloud/CloudContainer.swift`) and cross-checked against
both entitlements files by `scripts/lint_cloudkit_container.py`. **Private
database only.** No public database, no shared database, no zones (§5).

Two record types. This is the entire list, and the linter above fails the build
if a third appears without being declared.

### `WitnessWake` — the away alert

| Field | Value | Why it is the minimum |
|---|---|---|
| `sev` | one of `tamper` / `integrity` / `offline` / `pattern` | Four values. The receiving extension needs *some* class to pick the right sentence and interruption level — `tamper` earns the smoke-alarm treatment, the others do not. |
| *(nothing else)* | — | No device name, no zone, no event content, and no timestamp we write. CloudKit stamps every record with a precise creation date of its own, which we do not control and cannot switch off — §6.4 says what that costs. It never reaches the notification. |

The lock screen shows a deliberately generic line ("Your Canaries — Something
needs your attention"). The real sentence is composed **on the phone**, from the
user's own data, by the notification service extension. A stolen phone's lock
screen never says which Canary or what happened.
Vocabulary and copy live in one short file, `ios/Shared/WakePayload.swift`, so a
reviewer can check that claim by reading it.

### `EscalationWake` — the one thing another person can see

Lives in the `HouseholdEscalations` zone, which is the only shared thing in
this container. Same single field as the wake above (`sev`), same absence of
everything else — and written only when a top-tier alarm has already gone
unanswered by the owner. Participants are read-only, the zone is the access
boundary, and §6.5 argues the whole trade (including why §5's old "never a
shared database" row was reversed rather than quietly ignored).

### `PairedDevice` — the fleet list

| Field | Value |
|---|---|
| `name` | the name *you* gave the Canary |
| `deviceType` | which product it is |
| `baseURL` | its LAN address |
| `pairedAt` | when you paired it |

**No secrets.** Per-device tokens and pinned keys are in the Keychain,
device-bound, and never come here — that split is enforced by where the code
lives (`Security/Keychain.swift` vs `Cloud/CloudSync.swift`), not by
remembering.

The local mirror is the source of truth; iCloud is a convenience layer over it.
Both call sites deliberately swallow their errors, because a failed sync must
never stall the local alert already reaching the person in the room. That is the
right trade — and it is also exactly why §4 exists.

---

## 3 · The CloudKit Console, tile by tile

The console offers six things. Two earn their setup; the rest are noise for this
project, and knowing *why* is worth more than the verdict.

| Tile | Verdict | Why |
|---|---|---|
| **CloudKit Database** | **Use — this is the one.** | Schema management lives here, and §4 is a real deployment step with a silent failure behind it. Also worth knowing: the browser can only ever show **your own** account's private records. You cannot look at a user's data from this console. That is Invariant IV as infrastructure rather than as a sentence in a privacy policy. |
| **Push Notifications** | **Use, read-only.** | Delivery metrics turn "away alerts feel flaky" into a number, and today that question has no answer at all. **Do not** set up a push key or a token registry as a delivery path — that reintroduces exactly the third party §1 refused. Metrics in; nothing out. |
| **Dashboards** | **Later, small.** | Once production has traffic, one dashboard on container error rate and push delivery is worth having. Pointless before the schema ships, because there is nothing to chart. |
| **Identity & Trust** (Private Access Tokens / Privacy Relay) | **No.** | PATs let *a server* distinguish real clients from bots without fingerprinting them. We have no backend, and the website is static with `connect-src 'self'`. Good idea, wrong architecture — there is nothing to attach it to. |
| **Discussion Forums** | n/a | An external link. |
| **Documentation** | n/a | An external link. |

One thing to check while in the Push Notifications tile: both entitlements files
hardcode `aps-environment: development`. The release workflow uses automatic
signing, which normally rewrites that key from the distribution profile, so it
is very probably fine — but the delivery metrics are where you would find out
that TestFlight builds are talking to the sandbox. See
[`APPLE_SIGNING.md`](../APPLE_SIGNING.md).

---

## 4 · Deploying the schema — the step that silently doesn't happen

**CloudKit invents record types and indexes on first write in DEVELOPMENT.
Production refuses to.** Types and indexes have to be promoted deliberately.

That asymmetry is a trap, because the failure is silent in both directions:
production rejects a save carrying a field the schema doesn't define, and
rejects a query against a field with no queryable index. Combine that with the
error-swallowing in §2 and a half-deployed schema produces **no error at launch,
no warning, and nothing the user can see** — the feature is simply dead in the
shipped app.

So it is a command, not a memory:

```sh
ios/scripts/cloudkit_schema.sh check      # exports dev, reports every gap, exits non-zero
ios/scripts/cloudkit_schema.sh promote    # verifies first, refuses to ship a dead feature
```

What production needs, and what breaks without it:

| Record type | Needs | If it's missing |
|---|---|---|
| `WitnessWake` | the type, the `sev` field | Away alerts never arrive — the write is rejected, so no push is ever sent. |
| `WitnessWake` | `createdTimestamp` **Queryable** *(advised)* | Alerts still work, but the day-old sweep fails and spent wakes accumulate in the user's iCloud. |
| `PairedDevice` | the type, all four fields | The fleet never syncs. |
| `PairedDevice` | `recordName` **Queryable** *(required)* | `CloudSync.pull()` uses a match-all predicate, which production refuses without an index. The error is swallowed, so a second device reads empty and looks like "you have no devices" — forever. |

**Two gates, because this already went wrong once.** The first version of
`cloudkit_schema.sh` knew only about `WitnessWake`, while `CloudSync` had been
writing and querying `PairedDevice` since the day it was written. Nothing caught
it, because nothing was looking. Now:

1. `cloudkit_schema.sh` reads a **requirements table** — one row per record
   type, listing its fields and the indexes its queries depend on — instead of
   hardcoding one type in one grep.
2. `scripts/lint_cloudkit_container.py` cross-checks that table against the
   Swift sources on **every push**, in both directions: a record type the app
   uses but the table doesn't list fails the build, and so does a table row no
   Swift file names. It also reads the whole `RECORD TYPE … );` stanza rather
   than a fixed twenty-line window, which under-reads a type with more fields
   than the window and reports a gap that isn't there.

This is the repo's standing pattern: *two things that must agree, typed twice,
get a gate rather than a promise.*

---

## 5 · What will never go in the container

Not "what we haven't gotten to" — what is refused, with the reason:

| Never | Why |
|---|---|
| **Footage, frames, thumbnails, embeddings** | Invariant I. Raw media never leaves the device during normal operation, and reversible derivatives count as raw media. There is no encryption story that makes this acceptable, because the objection isn't confidentiality — it's custody. |
| **Event content** — what happened, in which zone, at what time | Invariant III. A wake says *a kind of trouble*, never the trouble. |
| **Event times, at any precision** | Invariant III. No record carries a time we wrote *about an event*. The log buckets events to 10-minute windows, and none of that is put in a record at all. This row is narrower than it looks, though — two timestamps reach iCloud regardless, one of them event-correlated. They are named and costed in §6.4 rather than hidden behind this line. |
| **Per-device tokens, pinned keys, the vault private key** | Keychain, device-bound. The sealed-snapshot promise is that only the phone can open a snapshot; a synced key softens that to "any of your phones," which may be an acceptable *opt-in* but is never a default and never silent. |
| **A public database** | A public database is a database we could read. Nothing here needs one. |
| ~~**A shared database**~~ — **reversed, deliberately; see §6.5** | This row used to read "a shared one is a sharing mechanism we would then have to secure. Neither is needed for anything above." The second sentence was the real argument, and it stopped being true the day the product needed a *second person* to be told when nobody answers an alarm. The first sentence still stands as a cost, and §6.5 pays it explicitly: one zone, one record type, read-only participants, and nothing in it worth stealing. A reversal like this belongs in the open — if a later change wants to put anything else in that zone, it has to argue with §6.5 first. |
| **An APNs key or a device-token registry of ours** | The whole point of §1. Holding one recreates the server we refused, and "this household got some alerts" becomes learnable by whoever holds it. |
| **Analytics, telemetry, crash-adjacent user data** | There is no telemetry anywhere in this project ([FAQ](../FAQ.md)); a container is not a loophole in that. |

---

## 6 · The residuals, argued instead of waved away

A privacy claim that only lists its strengths isn't a privacy claim. Three
things are genuinely imperfect here.

### 6.1 The timing residual (Invariant III)

Invariant III says the system cannot *"vary network behavior in proportion to
event occurrence unless explicitly configured for cover traffic."* A wake is
written **when something happens**, so its existence is an event-correlated
network signal. The content is a four-value class; the *timing* is not covered
by the content being tiny.

Who learns what: a network observer of the household uplink sees that traffic
left when it left. Apple sees a record appear in a private database at a time.
Neither learns which Canary, which zone, or what happened — but "something
happened at roughly T" is real and must be stated rather than asserted away.

This is the same residual [`apple_home_integration.md`](apple_home_integration.md)
argues at length for HomeKit, and it is the same one
[`alert_relay.md`](alert_relay.md) accepts with eyes open for any relay. Two
honest observations bound it:

- It is **the price of instant away alerts on iOS**, and it is unavoidable for
  *any* mechanism, ours or a competitor's. The alternative is not a quieter
  alert; it is no alert.
- It is **opt-in and revocable.** Away alerts are off until the user arms an
  "Anywhere" rule, and `AwayPush.disable()` deletes the subscription outright.
  A user who wants zero event-correlated traffic can have exactly that, and
  keeps every local alert.

The Apple Home projection solves its version of this with a **metronome** — it
publishes every tick whether or not anything happened, which is Invariant III's
own named conforming path (cover traffic). The away wake does **not** do this
today, and it is a fair question whether it should (§9).

### 6.2 `sev` travels in plaintext, and has to

CloudKit can encrypt a field so that Apple cannot read it
(`CKRecord.encryptedValues` — keys derived from the user's iCloud Keychain).
`sev` cannot use it. Apple's push service has to read the field to put it in the
notification payload (`desiredKeys`), and it cannot read what it cannot decrypt.
Encrypting `sev` would mean the phone receives only the generic line and the
away alert stays vague forever — which is the failure the away path was built to
fix.

So: Apple can see a four-value class on a record in a private database. Not
which device, not which room, not what happened. That is the trade, stated
plainly.

### 6.3 `PairedDevice` is plaintext today — open decision

This one has no such excuse. `CloudSync` writes `name`, `deviceType`, `baseURL`
and `pairedAt` as **standard fields**, which means they are encrypted in transit
and at rest with keys Apple holds (unless the user has Advanced Data Protection
switched on). `name` is a name the user chose — "Nursery", "Back Door" — and
`baseURL` is a LAN address. Neither is footage and neither is an identifier of a
person, but neither needs to be readable by anyone either.

Nothing forces plaintext here: `pull()` fetches with a match-all predicate and
never filters on a field, so moving all four into `encryptedValues` costs no
functionality. The constraint is that encrypted fields cannot be queried,
sorted, or indexed — which this code does not do.

**Recommendation: encrypt them.** It is cheapest to do before there is
production data to migrate, and it upgrades the claim from *"SecuraCV cannot
read your fleet list"* (true today) to *"nobody but you can"* (not true today).
Tracked in §9.

### 6.4 Two precise timestamps reach iCloud, and one of them is event-correlated

An earlier draft of this document claimed that nothing finer than a 10-minute
bucket ever leaves for a cloud. That was false, and it was the exact failure
mode this document is supposed to be an antidote to — a privacy claim written
from the intent rather than from the code. Both timestamps, stated plainly:

**`PairedDevice.pairedAt` — a full-precision `Date`, written by us.**
`CloudSync.push()` stores it as-is. It is not an event time: it says when you
paired a Canary, once, at install. But it is precise, we chose to write it, and
"we only ever emit coarse times" was not a thing we could say.

Coarsening it is not free, which is worth knowing before someone tries.
`pairedAt` is the **last-writer-wins key** for cross-device merge
(`DeviceStore.mergeFromCloud`, which keeps the existing record when
`existing.pairedAt >= ref.pairedAt`). Bucket it to 10 minutes and two edits
inside one bucket compare equal, so the incoming one is silently dropped — a
correctness bug traded for a privacy gain that is small, since the leak is one
install-time moment per device rather than anything about your day. The real fix
is a separate monotonic merge counter, at which point `pairedAt` can be
coarsened or dropped from the record entirely. Queued as §9.5, next to the
`encryptedValues` change it should land with.

**CloudKit's per-record creation date — precise, ours to neither set nor
remove.** Every record gets one. For `PairedDevice` this is another install-time
moment and uninteresting. For `WitnessWake` it is not: **the creation date of a
wake is, to within seconds, the time the event happened**, sitting in the user's
private database until the sweep clears it (under 24 hours). That is a genuinely
precise event time in a cloud, and no wording makes it otherwise.

What bounds it, honestly:

- It is in the **user's own** private database. Custody is theirs; SecuraCV
  cannot read it (Invariant IV holds).
- It never reaches the notification, so nothing on a lock screen exposes it.
- It says *when*, never *what* or *which* — there is no zone, no device, and
  four possible values of `sev` next to it.
- `sweepOldWakes` deletes it within a day. That is a retention bound, not
  encryption, and it is worth exactly what a retention bound is worth: the
  window is short, and during the window the timestamp exists.
- It only exists at all if the user armed an "Anywhere" rule, and
  `AwayPush.disable()` removes the whole path.

This is the same residual as §6.1 seen from the storage side rather than the
network side, and it has the same answer: it is the price of instant away alerts
on iOS, it is opt-in, and it is stated rather than asserted away. A metronome
(§9.2) would blunt the *network* signal and would not touch this one — only
encrypting the record's own metadata would, which CloudKit does not offer.

*A note on wording, because a privacy project pays for imprecision:* the
companion RFC's phrase "Apple end-to-end protects it" was doing more work than
standard CloudKit fields support. **"SecuraCV has no server in the loop and
cannot read any of it"** is exact and is the claim that matters. This doc is
the canonical statement of the finer point.

### 6.5 A shared zone exists now, and it is the only thing anyone else can see

The escalation ladder ends at a person who is not the owner: if a top-tier
alarm goes unanswered, somebody the owner invited is told. On Apple devices
the honest mechanism for that is CloudKit sharing, which means this container
now has a **shared** database — the thing §5 used to refuse. What follows is
the argument, because a reversal that isn't argued is just a slide.

**What was built.** One custom zone, `HouseholdEscalations`, holding one
record type (`EscalationWake`) carrying one field — the same coarse `sev`
class the ordinary wake carries. The owner shares the **zone**, invitations go
out through Apple's own sharing sheet, and participants are **read-only**.

**Why a zone share rather than a record share.** The zone is the access
boundary, so "a household member cannot see your fleet, your Canary names,
your history, or your ordinary alerts" is a fact about CloudKit's access
control rather than a promise about our filtering. Ordinary wakes keep going
to the owner's default zone, which nobody is invited to. The rule that keeps
this true is short enough to enforce in review: **nothing but escalations may
ever be written into that zone.**

**What a participant actually receives.** A push whose words their own device
wrote. Shared-database subscriptions carry no per-record fields, so the
sentence comes from the subscription the participant's app created locally
("Nobody answered — an alarm on a fleet you help watch hasn't been answered").
Even the wording of a household alert never crosses the wire.

**The residuals, in the same spirit as §6.1–6.4:**

- **The participant learns that an alarm here went unanswered, and roughly
  when.** That is the entire point of telling them, and it is the same
  timing residual as §6.4 pointed at a second person instead of at Apple.
  They learn no *what*, no *which*, and no *where*.
- **The owner and the participant learn each other's Apple ID handles.** That
  is inherent to inviting a specific human, it is what the sharing sheet
  shows before anyone taps accept, and it stays on device.
- **A compromised participant account gains a zone containing "an alarm
  wasn't answered" and nothing else.** This is the direct answer to §5's
  surviving worry ("a sharing mechanism we would then have to secure"): the
  mechanism is Apple's, and what it guards is worth approximately nothing.
- **Escalation records are swept within a day**, same bound and same worth as
  `sweepOldWakes`.
- **It only exists if the owner invited someone**, and deleting the share
  revokes everyone at once, with no per-person bookkeeping of ours to get
  wrong.

**What this does not become.** Not a household account, not shared fleet
access, not a second person who can see or acknowledge anything. If a later
feature wants those, it is a new design with a new argument — not an extra
field on this record.

---

## 7 · How the user wins

Concretely, not in the abstract:

- **No account, no password, no profile.** You never type an email to use the
  app. There is nothing of yours at our end to breach, because there is no
  "our end."
- **No subscription, ever.** iCloud storage cost is a handful of records
  measured in bytes — inside anyone's free tier, including the 5 GB one.
- **Your second device just has your fleet.** Sign in, and the Canaries are
  there. No re-pairing, no export file, no QR dance.
- **Away alerts with no relay to register with and no vendor to trust.** Setup
  is "be signed into iCloud", which you already are.
- **The lock screen stays quiet about your house.** A stolen or shoulder-surfed
  phone shows "Something needs your attention." The specifics are composed
  behind the lock, from your own data.
- **It degrades honestly.** Not signed into iCloud? The app is fully functional
  locally; iCloud is convenience, never a gate. Nothing at home to notice an
  event? `AwayReach` says so in a sentence you can act on, instead of pretending
  a reach it doesn't have.
- **Opt-out is as real as opt-in.** Deleting the subscription ends the away path
  outright. Signing out of iCloud ends the sync. Local alerts keep working
  through both.
- **If SecuraCV disappears tomorrow, nothing of yours disappears with it.** We
  hold nothing, so there is nothing to shut down, sell, or lose. Your records
  are in your account and your log is on your hardware.

---

## 8 · Failure modes, and where each one is said out loud

The rule is that no failure here is allowed to be silent to the *user*, even
when the API is silent to the *code*.

| Symptom | Cause | Where it surfaces |
|---|---|---|
| Away alerts never arrive | `WitnessWake` or `sev` not in the production schema | `cloudkit_schema.sh check` names it; the Alerts tab states the actual reason rather than failing silently |
| Second device shows no Canaries | `PairedDevice` type or its `recordName` index not promoted | `cloudkit_schema.sh check` names it (this is the gap §4 was extended to catch) |
| Old wakes pile up in iCloud | `createdTimestamp` not Queryable | `cloudkit_schema.sh check`, as an advisory |
| "Sign in to iCloud to get alerts when you're away." | No usable iCloud account | `AwayReach.unavailable`, verbatim in the UI |
| "iOS couldn't register this iPhone for alerts." | APNs registration failed | `AwayPush.noteRegistrationFailure` |
| Nothing at home to notice an event | No resident device on the LAN | `AwayReach`, stated as the honest limit rather than papered over |
| The app aborts on launch in CI | `CKContainer.default()` with no entitlement — an ObjC exception Swift cannot catch | `scripts/lint_cloudkit_container.py` bans the call outright; `CloudContainer.swift` carries the full account |

---

## 9 · Open decisions and revisit triggers

1. **Encrypt `PairedDevice` fields** (§6.3). Recommended, cheap now, expensive
   after production data exists. Needs an `encryptedValues` change in
   `CloudSync` plus a matching schema promotion.
2. **A metronome for the away wake** (§6.1). Invariant III names cover traffic
   as its own conforming path, and the Apple Home projection already implements
   one. The trade is battery and iCloud writes against a genuinely quieter
   timing profile. Revisit if a user's threat model includes an observer of
   their uplink.
3. **`aps-environment` in the distribution build** (§3). Verify against real
   delivery metrics once production has traffic; correct the entitlements if
   automatic signing is not rewriting it.
4. **A third record type, or a new field on an existing one.** Do not add either
   without updating the requirements table — CI will stop you, which is the
   point — and without arguing it against §5 here first.
5. **A monotonic merge counter for `PairedDevice`** (§6.4), so `pairedAt` stops
   being load-bearing for conflict resolution and can be coarsened or dropped
   from the record. Same `CloudSync` write path as decision 1; they should land
   together, since both need one schema promotion rather than two.
