# SecuraCV — native iOS companion

The **living-with-it** app for your fleet: at-a-glance trust, smoke-alarm-grade
alerts, and on-device key custody. Not a flasher (that's `desktop/`), not a
camera app (there are no pixels to show) — a **witness console**. This is the
build of the design in [`docs/design/iphone_companion_app.md`](../docs/design/iphone_companion_app.md).

> **Status:** foundation. Real, buildable native SwiftUI — the four surfaces,
> the device transports, and every Apple-native integration are wired. It
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
  Shared/                  FleetActivityAttributes — compiled into app + widget
  Sources/
    SecuraCV/App/          entry, Theme, FleetStore (the one observable)
    SecuraCV/Model/        Witness, enums, witness-chain (mirror fleet_model.h + api.md)
    SecuraCV/Transport/    Discovery (mDNS), DeviceAPI (HTTP), BLEConsole (CoreBluetooth)
    SecuraCV/Security/     Keychain, DeviceStore, ChainVerifier (Ed25519 on-device)
    SecuraCV/Cloud/        CloudSync (CloudKit private DB — the user's own iCloud)
    SecuraCV/Alerts/       AlertCenter (interruption levels), Heartbeat (provably-alive)
    SecuraCV/Native/       LiveActivity, HomeKitBridge, MediaRoute (AirPlay/mic/speaker)
    SecuraCV/Views/        Today / Fleet / Alerts / Keys + Pair + DeviceDetail
    SecuraCVWidgets/       Dynamic Island / Live Activity UI
    SecuraCVNotificationService/  NSE: shape the content-free wake into a shown alert
  Tests/SecuraCVTests/     ChainVerifier + model/push-discipline tests
  Support/                 Info.plists, entitlements
  scripts/                 heal.sh (regen+build+test), stamp_build.sh (build identity)
```

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
