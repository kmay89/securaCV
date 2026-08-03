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
                           contract), WristCache, BuildInfo
  Sources/
    SecuraCV/App/          entry, FleetStore (the one observable)
    SecuraCV/Model/        Witness, witness-chain, FleetRollup (mirror fleet_model.h + api.md)
    SecuraCV/Transport/    Discovery (mDNS), DeviceAPI (HTTP), BLEConsole (CoreBluetooth)
    SecuraCV/Security/     Keychain, DeviceStore, ChainVerifier (Ed25519 on-device)
    SecuraCV/Cloud/        CloudSync (CloudKit private DB — the user's own iCloud)
    SecuraCV/Alerts/       AlertCenter (interruption levels), Heartbeat (provably-alive)
    SecuraCV/Native/       LiveActivity, WatchLink (WCSession → wrist), HomeKitBridge, MediaRoute
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
- **The character earns its moments.** The one standard Canary
  (`brands/logo_512x512.png`, staged by `make_brand_assets.py` — never
  redrawn) appears in the calm places, breathing gently (still, under Reduce
  Motion), and chirps only when the alert path proves itself end-to-end.
  Bundle tests fail CI if the chirp or the mascot ever fall out of the app.

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
