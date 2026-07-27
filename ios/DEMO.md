# Demo & test the iPhone app — Simulator, your iPhone, and fast iteration

How to get SecuraCV running for a demo or a test pass, with no Canary
hardware required. Everything here happens **on a Mac** (the app never
compiles in the Linux dev container — CI and your Mac are the compilers).

## TL;DR

| I want to… | Do this |
| --- | --- |
| See it in the Simulator | `cd ios && scripts/heal.sh generate && open SecuraCV.xcodeproj`, pick an iPhone simulator, ⌘R |
| Build + test headless | `cd ios && scripts/heal.sh build` |
| Run it on my iPhone | Same as Simulator, but select your phone as the destination — signing notes below |
| Iterate on a screen fast | Open any file in `Sources/SecuraCV/Views/`, ⌥⌘↩ for the Preview canvas |
| Demo without hardware | Demo mode — it's automatic in the Simulator; on a phone, Fleet tab → ⋯ → **Demo fleet** |

## Demo mode (the seeded fleet)

The Simulator has no Bluetooth and usually no Canaries on its Wi-Fi, so a
plain build demos as an empty shell. **Demo mode** (`App/DemoFleet.swift`)
seeds five sample Canaries — a verified front door with a person event, a
garage on battery, a nursery `canary-sense` with a breathing lock, a signed
vision witness, and a BLE-only mailbox — plus a day of coarse-bucketed
timeline events, flowing through the *same* `Witness`/`TimelineEvent`
primitives the live transports produce. There is no separate demo rendering
path to drift.

- **In the Simulator** it turns itself on when nothing is paired.
- **Anywhere** it can be toggled: Fleet tab → ⋯ menu → **Demo fleet**, the
  "Turn off" button on the Today banner, or forced with the `-SecuraCVDemo`
  launch argument / `SECURACV_DEMO=1` environment (handy in schemes and CI).
- **It joins, never replaces**: pair a real Canary mid-demo and it appears
  next to the samples (demo ids are `demo-`-namespaced, so no collisions).
- **It can't cry wolf** (held by `DemoFleetTests`): sample data never
  exceeds `.notice` severity, never wears a failed badge or tamper, and so
  can never trigger a notification or the alarm colors. The banner on Today
  makes sure sample data never passes as real.
- **It can't mask a real alarm either**: the demo feeds the "provably
  alive" heartbeat only while *no* real device is paired. The moment a real
  fleet exists, only real confirmations count — sample data must never hold
  the dead-man's-switch open — and any demo-fed verification is revoked.

## Simulator on your Mac

```sh
cd ios
brew install xcodegen          # once
scripts/heal.sh generate && open SecuraCV.xcodeproj
```

Pick any iPhone simulator and ⌘R — the demo fleet is on by default. The
headless equivalent (`scripts/heal.sh build`) regenerates, builds, and runs
the tests on the newest iPhone simulator installed (override with
`SECURACV_SIM="iPhone 16 Pro"` if you want a specific one).

A useful extra: the Simulator shares your Mac's network, so a **real Canary
on your Wi-Fi is discoverable and pairable from inside the Simulator** —
mDNS discovery, HTTP pairing, and chain verification all work. Only BLE is
Simulator-impossible.

## Your iPhone

1. Plug the phone in (or use Wi-Fi debugging), select it as the run
   destination, ⌘R.
2. **Signing team**: automatic signing needs your team. Either set it once so
   it survives project regeneration:
   ```sh
   APPLE_DEVELOPMENT_TEAM=YOURTEAMID scripts/heal.sh generate
   ```
   (writes git-ignored `ios/local.yml`, which `project.yml` merges on every
   regenerate), or pick the team in Xcode → target → Signing & Capabilities —
   but that manual pick is wiped each time the project regenerates, so
   `local.yml` is the way.
3. First install: on the phone, Settings → General → VPN & Device
   Management → trust your developer certificate.

**Why Debug builds sign at all:** the full entitlements include **Critical
Alerts**, which Apple grants per-app on request — automatic signing fails
for any team without the grant. Debug builds therefore use
`Support/SecuraCV.dev.entitlements` (everything except Critical Alerts; the
app already falls back to time-sensitive notifications), while Release keeps
the full set. If you're on a **free** Apple ID (no paid membership), push,
HomeKit, and iCloud entitlements won't sign either — use the Simulator, or
temporarily point `CODE_SIGN_ENTITLEMENTS` at a stripped copy.

TestFlight / App Store distribution is a different pipeline entirely:
`.github/workflows/ios-release.yml` and `desktop-lab/MOBILE.md`.

## Fast iteration (and the "web emulator" question)

The tempting idea — mirror the app in a browser to iterate faster — is one
this repo has already paid for once: the two flashers are two frontends, and
every diagnostic has to be built twice or half the users keep the vague
version. A web replica of a native SwiftUI app would be that, permanently,
while never exercising the real code (CoreBluetooth, Keychain, ActivityKit —
the reasons this app is native).

The native toolchain already has the fast loop:

- **Xcode Previews** — every main view has a `#Preview` seeded from
  `DemoFleet.previewStore()`. Open a view file, ⌥⌘↩, and the canvas
  live-reloads as you type: sub-second, no build-and-run, no hardware, real
  rendering. This *is* the "web emulator" experience, minus the drift.
- **Demo mode in the Simulator** covers whole-app flows (tabs, navigation,
  pull-to-refresh, the Test Alert path).

If a shareable no-Mac walkthrough is ever needed (e.g. on the website), it
should be screenshots/screen-recordings of the real app, not a second
implementation.

## CI

`ios-selfheal.yml` now also runs on every PR touching `ios/**` — the same
regenerate + build + test as the nightly, unsigned, on a Simulator. PR runs
always build (no Apple credentials needed); the `ENABLE_IOS_BUILD=true`
repo-variable gate applies only to the scheduled nightly, so it never nags
before iOS is enabled.
