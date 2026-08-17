# SecuraCV — native iOS companion

The **living-with-it** app for your fleet: at-a-glance trust, smoke-alarm-grade
alerts, and on-device key custody. Not a flasher (that's `desktop/`), not a
camera app (there are no pixels to show) — a **witness console**. This is the
build of the design in [`docs/design/iphone_companion_app.md`](../docs/design/iphone_companion_app.md).

> **Status:** foundation. Real, buildable native SwiftUI — the four surfaces,
> the device transports, every Apple-native integration, and the embedded
> Apple Watch app (see "On your wrist" below) are wired. It
> **builds and signs on a Mac / in CI with an Apple Developer account** (see
> `desktop-lab/MOBILE.md` — that requirement can't come from this repo alone).
> It is *not* compiled in the Linux dev container; the gated CI is the compiler.

## Why native (not the Tauri shell)

The desktop apps wrap the web Lab in a WebView, which is right for *reusing* the
Lab. This app instead goes native because it needs things a WebView fights:
**Dynamic Island / Live Activities**, **CoreBluetooth** (iOS Safari has no Web
Bluetooth — the device's own PWA has to send people to Bluefy; native erases
that), **HomeKit**, **Secure Enclave** key custody, **AirPlay** + mic/speaker,
and reliable **background notifications**. These are the exact capabilities the
`desktop-lab` `native_capabilities()` seam marks as "Phase 2." This is Phase 2.

## Anti-rot: how it "just works forever"

- **The project is generated, never committed.** `project.yml` (XcodeGen) is the
  one source of truth; `scripts/heal.sh` regenerates the `.xcodeproj` on every
  build and every night. Same idea as the desktop app's `build.rs` re-embedding
  the one canonical catalog so no committed copy drifts.
- **The device describes, the app renders.** Settings come from the Canary's own
  `/api/v1/config` schema; state mirrors `fleet_model.h`; unknown fields are
  forward-compatible. New firmware features light up **without an App Store
  update**.
- **Frozen OS primitives only** — Network.framework, CoreBluetooth, Keychain /
  Secure Enclave, CloudKit, UserNotifications, ActivityKit, CryptoKit. The
  load-bearing frameworks are a decade stable; the churn is at the fashionable
  edges we avoid.
- **Nightly self-heal** (`.github/workflows/ios-selfheal.yml`) rebuilds + tests
  against current contracts so it can't rot silently between releases.

## Layout

```
ios/
  project.yml              XcodeGen spec — the single source of truth (edit this, never a .pbxproj)
  Shared/                  compiled into app + widgets + watch: fleet enums, Theme,
                           FleetActivityAttributes, WristSnapshot (the phone→watch
                           contract), WristCache, GlanceAnswer (the one spoken
                           sentence), BuildInfo
  Sources/
    SecuraCV/App/          entry, FleetStore (the one observable)
    SecuraCV/Model/        Witness, witness-chain, FleetRollup (mirror fleet_model.h + api.md)
    SecuraCV/Transport/    Discovery (mDNS), DeviceAPI (HTTP), BLEConsole (CoreBluetooth)
    SecuraCV/Security/     Keychain, DeviceStore, ChainVerifier (Ed25519 on-device)
    SecuraCV/Cloud/        CloudSync (CloudKit private DB — the user's own iCloud)
    SecuraCV/Alerts/       AlertCenter (interruption levels), Heartbeat (provably-alive)
    SecuraCV/Native/       LiveActivity, WatchLink (WCSession → wrist), HomeKitBridge,
                           MediaRoute, FleetIntents (Siri / Shortcuts / Action button)
    SecuraCV/Views/        Today / Fleet / Alerts / Keys + Pair + DeviceDetail
    SecuraCVWidgets/       Dynamic Island / Live Activity UI
    SecuraCVNotificationService/  NSE: shape the content-free wake into a shown alert
    SecuraCVWatch/         SecuraCV on your wrist: WristStore + 3 screens (glance/heartbeat/about)
    SecuraCVWatchWidgets/  watch complications + Smart Stack card (read the watch-local cache)
  Tests/SecuraCVTests/     ChainVerifier + model/push-discipline + wrist-contract tests
  Support/                 Info.plists, entitlements (incl. Watch-*.plist / Watch*.entitlements)
  Assets.xcassets/         iPhone app icon + Canary mascot — generated, committed
  WatchAssets.xcassets/    watch app icon + Canary mascot  — same generators, same contract
  Sounds/                  the canary's chirp (make_chirp.py, generated + committed)
  scripts/                 heal.sh (regen+build+test+embed proof), stamp_build.sh,
                           make_app_icon.py / check_app_icon.py (icons + their CI gate),
                           make_brand_assets.py (mascot), make_chirp.py (the one sound)
```

## The polish doctrine

Four rules keep "beautiful" from decaying into "busy":

- **One vocabulary of meaning.** `Shared/EventVocabulary.swift` mirrors the
  witness dictionary (same ids, same sentences as the Home Assistant card —
  `scripts/lint_dictionary_sync.py` fails CI on drift), speaks the device
  dialect, and renders *unknown* event types as readable words with a calm
  default — a new sensor lights up here without an app update, never as a
  blank row (the anti-rot bet, applied to copy).
- **The hive.** At a handful of Canaries the Fleet tab becomes a honeycomb
  (`Views/Components/Honeycomb.swift`, pure host-tested geometry): quiet
  cells wear soft rings, the one that needs you is the only saturated one,
  and a dashed "+" cell keeps the next Canary's place visibly open. At ten+
  the list gains rooms and search — scale must feel calmer, not louder.
- **Buzz discipline.** Every haptic and the one sound go through
  `Shared/FeedbackPolicy.swift` (host-tested): escalations are felt once at
  the crossing, the all-clear once on the way back, a wrist- or phone-started
  path test answers in the hand that asked — and an ordinary week produces
  **zero** haptics. The chirp plays as a system sound, so the silent switch
  wins.
- **Repeats rest; escalations pierce.** The condition ledger stops the same
  alarm re-posting, but a dog pacing the porch flips a Canary between two
  status lines all evening — and every flip used to buzz. `RepeatGovernor`
  (host-tested) gives the first alert of a burst the full interruption and
  each repeat a doubling rest (120 s → 30 min ceiling; a calm half hour makes
  the next alert news again). A repeat that gets WORSE pierces immediately;
  tamper and a failed chain are never governed — the same punch-through
  contract mutes honor. Rested repeats still land in the history with the
  honest reason, and the same verdict gates the away wake, so an iPad across
  town rests exactly when the pocket does. When several Canaries cross in one
  pass (the router died), `AlertStorm` collapses them into ONE summary naming
  the count and the worst thing — the ledger keeps every per-Canary record.
  And the 3am action is on the notification itself: **Mute until morning**,
  the same always-expiring `MuteDuration` every other surface uses.
- **~5-second LAN alerts, and a measured claim.** The 5-second liveness
  sentinel also watches each Canary's witness-chain head (`HeadWatch`,
  host-tested — first sight is a baseline, any movement including backwards
  is news): the moment a head moves, the full refresh runs NOW instead of
  waiting out the 20-second cycle. Tamper heard over a BLE console NOTIFY
  re-evaluates immediately — sub-second, the fastest transport the fleet
  has. And the Test Alert now runs a stopwatch: the provably-alive card
  shows "posted and accepted in _n_ ms", a number it measured, never a vibe
  (non-negotiable #4).
- **The fleet moves to new Wi-Fi as one flow, pilot-first.** Fleet tab →
  Options → "Update fleet Wi-Fi…". The staged plan (`FleetWiFiRollout`,
  host-tested) sends ONE pilot Canary first and touches nothing else until
  the pilot actually answers on the new network — a typo'd password strands
  one device (which keeps its rescues), never the fleet. Each device rides
  the transport it can actually use right now: HTTP (`/api/wifi/connect`)
  when it's answering, the firmware's bonded BLE provisioning service when
  the password already changed under it, and the display family is named
  hands-on up front (its credentials live in its own first-boot portal).
  Every verdict on the sheet is a device that actually came back — not a
  hope. The same honesty landed under the glass settings sheet as an undo:
  the sheet snapshots every knob at open, and "Undo changes" replays the
  snapshot through the ordinary write path (`SettingsRevert`, host-tested).
- **The character earns its moments.** The one standard Canary
  (`brands/logo_512x512.png`, staged by `make_brand_assets.py` — never
  redrawn) appears in the calm places, breathing gently (still, under Reduce
  Motion), and chirps only when the alert path proves itself end-to-end.
  Bundle tests fail CI if the chirp or the mascot ever fall out of the app.
- **The character is an engine, not a sticker.** `Shared/CanaryMood.swift`
  is a faithful Swift mirror of the display firmware's mood engine
  (`firmware/projects/canary-display/.../bird_mood.h` — the source of
  truth; change it first, keep every constant equal, the tests pin the
  math). Anxiety rises instantly with real trouble and decays one point
  per quiet hour; consecutive clean days build trust; the face ladder is
  `Hidden / Asleep / Calm / Worried / Distressed` with `Searching`/`Calling`
  postures that name WHO the bird is looking for. The phone computes the
  mood from live fleet truth (`CanaryMoodKeeper`, persisted — the bird has
  no amnesia), ships it in the wrist snapshot, and `Shared/CanaryActor`
  performs it with transform choreography over the one canonical mascot —
  squash-and-stretch anchored at the feet, stillness under Reduce Motion.
  The honesty rules travel with the engine: every face maps 1:1 to state a
  log line can name, and during a real unacknowledged alarm the bird is
  `Hidden` — never cute during a real alarm; the instruments own the stage.
- **The Alerts tab is a list of alerts.** It used to be a rules editor
  wearing that name, so the app kept no history at all and the one question
  people open it with — *did something need me, and did it reach me?* — had
  no answer anywhere. Now `AlertLedger` records every condition that crossed
  a rule (coarse 10-minute buckets only, Invariant III), collapses repeats
  into one row with a count so a flapping Canary can't bury the list, and
  stores **how far each one actually got**: on Wi-Fi, away, or not delivered
  *with the reason*. "We told you" and "we couldn't tell you" must never
  look the same. Rules moved behind one plainly-worded button; the calm
  canary holds the empty state, because a quiet fleet is the product
  working. The wrist gets the same list (`AlertsListView`, the watch's
  fourth screen) over capped, additive-optional snapshot rows.
- **Away alerts are real, and honest about their limit.** `AwayPush` carries
  the contentless wake over the user's **own iCloud** — a
  `CKQuerySubscription` on their private database, so Apple's push service
  does the delivery and SecuraCV runs no server, holds no APNs key, and
  keeps no device-token registry. The wake carries a coarse class and
  nothing else (`Shared/WakePayload.swift` — pure Foundation, the one file
  the NSE compiles, so a reviewer can check that claim by reading it); the
  phone writes the words itself. The limit is stated in the UI rather than
  hidden: something has to be **home** to post the wake, exactly as HomeKit
  needs a hub, and `AwayReach` says so instead of leaving "Anywhere" looking
  like a promise. See the design doc §5 for why this replaced the hosted
  relay — which stays easy to add, since the receiver already decodes both
  payload shapes.
- **The CloudKit schema is a command, not a ritual.**
  `scripts/cloudkit_schema.sh check` / `promote` inspects the development
  schema and deploys it to production. It exists because the failure it
  guards is silent: CloudKit auto-creates record types in development on
  first write, production does not, and a production build missing
  `WitnessWake` throws no error and warns nobody — away alerts just never
  arrive. The script refuses to promote a schema that lacks the type
  (deploying nothing while looking like success is the trap), and warns
  when create-time isn't queryable, which is what `sweepOldWakes` needs.

- **First-class ecosystem citizenship.** Alerts are actionable (Ack /
  Mute 1 hour, mirrored to the wrist by the system) with relevance-ranked
  summaries; a real **Focus filter** (`FleetFocusFilter`) lets each Focus
  choose "only life-safety" right inside iOS's Focus settings — and critical
  still passes, because Focus quiets the everyday, never the smoke alarm
  (tested). Mute is durable (`MuteLedger`, host-tested) and works from the
  notification, the phone detail screen, or the watch — tamper punch-through
  preserved everywhere by construction. The fleet glance also ships as
  iPhone **Lock Screen / Home Screen widgets** (Shared/FleetGlanceViews —
  the same views the watch complications render, over the same snapshot),
  and the Live Activity stale-dates so the island never presents old truth
  as current.
- **The island is an episode, not wallpaper.** `Shared/IslandPolicy.swift`
  (host-tested, FeedbackPolicy's twin) decides when the Live Activity may
  exist at all: a condition at warn or above, the dead-man's-switch talking,
  or a path test in flight. It lingers a few minutes on the all-clear, then
  leaves the stage — an ordinary quiet day puts *nothing* in the status bar,
  the ambient twin of "an ordinary week produces zero haptics." The
  always-there glance for people who want one is the Lock Screen / Home
  Screen widget — ground the user chose to give it, not ground the app
  squats on.

## On your iPad

Same binary, same release, same truth — `TARGETED_DEVICE_FAMILY "1,2"` means
the iPad ships in every `ios-v*` release automatically, with iPad
orientations, multi-scene support, and no full-screen requirement (Split
View / Stage Manager just work). What changes is the idiom, at exactly one
decision point: `RootView` reads the horizontal size class and gives regular
widths a persistent **sidebar** (`SidebarRootView`, with the fleet's
worst-severity pip visible before a single tap) while compact widths keep
the tab bar. Both idioms render the same `AppSection` list and the same four
views — designed *for* iPad never means a second implementation (the
two-flashers lesson applies to layouts too). Content that would sprawl on a
13″ canvas self-constrains instead (`TodayView`'s readable-width column).

## On your wrist

The watch app is a **target in this same project** (`SecuraCVWatch` +
`SecuraCVWatchWidgets` in `project.yml`), embedded in the iPhone `.ipa` — it
inherits `MARKETING_VERSION`, ships on the same `ios-v*` tag, and CI proves
the embed on every PR (`heal.sh`) and every release (the `.ipa` check). It
cannot version-drift from the phone, by construction.

Sync is one shared Codable payload, `Shared/WristSnapshot.swift`, pushed by
`Native/WatchLink.swift` over WatchConnectivity's `updateApplicationContext`
(latest-state-wins, delivered even while the watch sleeps) and adopted by the
watch's `WristStore` under a tested revision rule. The watch app parks each
snapshot in a watch-local app group so the complications render without
waking anything. The phone's `FleetStore` stays the only source of truth: the
wrist remembers, orders, and requests — it never invents state. Design of
record: [`docs/design/apple_watch_and_notifications.md`](../docs/design/apple_watch_and_notifications.md).

## Ask, don't open — Siri, Shortcuts, Spotlight, the Action button

The way a daily app stays wanted instead of resented: most days you never
open it, because the platform asks on your behalf. The app exports exactly
four verbs as App Intents (`Sources/SecuraCV/Native/FleetIntents.swift`),
live in Siri, the Shortcuts app, Spotlight, and the Action button picker the
moment the app is installed — zero setup, no in-app configuration screen to
rot:

- **Check the Fleet** — the one honest answer, spoken from the same glance
  cache the widgets render. No app launch, no radio, instant. "All's well —
  5 of 5 healthy."
- **Test Alert Path** — the alert self-test from anywhere a Shortcut can
  run: posts a real notification and confirms iOS accepted it, the same
  green check the provably-alive card lights. Opens the app on purpose: the
  proof should be seen on the card and heard in the chirp, not narrated.
  The spoken verdict claims only what the test proved — "alerts can reach
  this device," never a device→relay round trip it didn't make.
- **Quiet Hour** — every paired Canary muted for an hour in one verb, and
  the confirmation names, every time, what a mute can never silence: tamper
  and signature failures still come through (the same
  `Witness.effectiveSeverity` punch-through as every other mute path).
- **Resume Alerts** — the symmetric verb, so quiet is never a trap.

This is also the flexibility story with no scope creep: "quiet the fleet
when my mow-the-lawn timer starts," "check the fleet when I arrive home,"
"Action button = test my alerts" are all *Shortcuts automations the user
composes*, not settings we ship. The OS owns the combinatorics; the app
contributes four verbs and stays simple.

The honesty rules travel into the spoken answer
(`Shared/GlanceAnswer.swift`, host-tested): an old snapshot says its age
("As of 40 minutes ago: …"), a quiet fleet with a dark delivery heartbeat
answers with both facts, and sample data says "Sample data" in the sentence
itself. Intents that only read answer from the cache and never launch
anything; intents that act prefer the live store and fall back to the same
durable ledgers the store folds at launch — one behavior, cold or warm.

## Build it (on a Mac)

```sh
cd ios
brew install xcodegen
scripts/heal.sh build      # regenerate, build, and test on a simulator
# or open it:
scripts/heal.sh generate && open SecuraCV.xcodeproj
```

**Demoing or testing it — Simulator, your own iPhone, Xcode Previews, demo
mode (the seeded fleet)?** See [`DEMO.md`](DEMO.md). Short version: the
Simulator opens straight into a sample fleet, and
`APPLE_DEVELOPMENT_TEAM=YOURTEAMID scripts/heal.sh generate` makes device
signing survive regeneration.

Signing (device / TestFlight / App Store) uses the **same** Apple secrets as the
desktop mobile pipeline (`ENABLE_IOS_BUILD`, `APPLE_DEVELOPMENT_TEAM`,
`APPLE_API_ISSUER`/`APPLE_API_KEY`/`APPLE_API_KEY_BASE64`, …). See
`.github/workflows/ios-release.yml` and `desktop-lab/MOBILE.md`.

## What it will never do (invariant guardrails)

No live video wall, no face/plate/"who was that" search, no precise timestamps
on an event, no SecuraCV-hosted footage, no solo vault unseal. An app that
*can't* do these is one nobody has to trust us not to do. See the RFC §7.
