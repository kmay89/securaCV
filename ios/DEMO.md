# Demo & test the iPhone app — Simulator, your iPhone, TestFlight, fast iteration

How to get SecuraCV running for a demo or a test pass, with no Canary
hardware required. Everything here happens **on a Mac** (the app never
compiles in the Linux dev container — CI and your Mac are the compilers).

## TL;DR

| I want to… | Do this |
| --- | --- |
| See it in the Simulator | `cd ios && scripts/heal.sh generate && open SecuraCV.xcodeproj`, pick an iPhone simulator, ⌘R |
| Build + test headless | `cd ios && scripts/heal.sh build` |
| Run it on my iPhone | Same as Simulator, but select your phone as the destination — signing notes below |
| **The production app on my iPhone** | TestFlight — one-time setup + one Actions button, see below |
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

Pick an **iPad simulator** instead to see the iPad shape: the same four
surfaces behind a persistent sidebar rather than a tab bar, with the fleet's
worst-severity pip on the Fleet row. Rotate or drag into Split View and the
root adapts with the size class.

A useful extra: the Simulator shares your Mac's network, so a **real Canary
on your Wi-Fi is discoverable and pairable from inside the Simulator** —
mDNS discovery, HTTP pairing, and chain verification all work. Only BLE is
Simulator-impossible.

### The watch app in the Simulator

The wrist app is a target in the same project (no separate checkout, no
separate version — it ships inside the iPhone `.ipa`). To see it:

1. In Xcode's scheme picker choose **SecuraCVWatch**, and pick a watch
   simulator that is *paired* to an iPhone simulator (Xcode's default watch
   sims are; Devices & Simulators shows the pairing).
2. ⌘R. Run the **SecuraCV** scheme on the paired iPhone simulator too — the
   moment the phone app is up, WatchConnectivity carries the demo fleet to
   the wrist, banner and all ("Sample data — pair a Canary from your
   iPhone"). The watch renders whatever the phone believes; it has no demo
   switch of its own, because it has no state of its own.
3. Complications: long-press the watch face in the simulator → edit → add
   the **SecuraCV Fleet** complication. It renders from the last snapshot the
   watch app cached, so open the watch app once first.

On real hardware it's the same story: install on the iPhone (TestFlight or
⌘R), and the Watch app appears in the Watch app's "Available Apps" list —
it installs from the phone, never separately.

## Your iPhone

1. Plug the phone in (or use Wi-Fi debugging), select it as the run
   destination, ⌘R.
2. **Signing team**: automatic signing needs your team. Either set it once so
   it survives project regeneration:
   ```sh
   APPLE_DEVELOPMENT_TEAM=YOURTEAMID scripts/heal.sh generate
   ```
   (writes git-ignored `ios/local.yml`, which `heal.sh` merges into the spec
   on every regenerate), or pick the team in Xcode → target → Signing & Capabilities —
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

An Xcode dev build stops launching ~7 days (free Apple ID) or ~1 year (paid)
after install, and untethers from your Mac either way. For the app that
*lives* on your phone and updates itself, use TestFlight — next section.

## The production app on your iPhone — TestFlight

The repo already has the whole pipeline (`.github/workflows/ios-release.yml`:
archive → sign → upload to App Store Connect → tag `ios-v*`). It needs a
one-time setup, then it's one button per release.

**One-time setup (~30 minutes, all outside this repo):**

1. **Apple Developer Program** membership ($99/yr) on your Apple ID —
   TestFlight does not work on the free tier.
2. **App record**: App Store Connect → Apps → **+** → New App → platform iOS,
   bundle ID `com.securacv.witness` (register the ID at
   developer.apple.com → Identifiers first if it's not offered). Name it
   SecuraCV. Nothing else needs filling in for TestFlight.
3. **CI signing credentials** — the same four secrets the whole repo's Apple
   pipeline shares (full walkthrough in
   [`desktop-lab/MOBILE.md`](../desktop-lab/MOBILE.md) § CI). In the repo's
   Settings → Secrets and variables → Actions:
   - Variable `ENABLE_IOS_BUILD` = `true`
   - Secret `APPLE_DEVELOPMENT_TEAM` — your 10-char Team ID
   - Secret `APPLE_API_ISSUER` — App Store Connect API issuer UUID
   - Secret `APPLE_API_KEY` — the API key ID (10 chars)
   - Secret `APPLE_API_KEY_BASE64` — the `.p8` contents, base64-encoded
   (Create the key at App Store Connect → Users and Access → Integrations →
   App Store Connect API, role **App Manager**.)
   Plus the persistent signing identity — per-run cloud certificates orphan
   themselves on throwaway runners (RELEASE_LESSONS 2026-07-28 (e)): in
   Xcode → Settings → Accounts → Manage Certificates → **+** create both
   *Apple Development* and *Apple Distribution*; in Keychain Access → My
   Certificates select both → File → Export Items → one `.p12` with a
   password; then
   - Secret `APPLE_CERTIFICATE` — the `.p12`, base64-encoded
   - Secret `APPLE_CERTIFICATE_PASSWORD` — its password

   ⚠️ `APPLE_CERTIFICATE` is **repo-wide** — the macOS desktop pipeline
   reads the same secret for its *Developer ID Application* identity
   (`desktop/SIGNING.md`). If that's already set up (or when you set it up
   later), export ONE combined `.p12` containing every identity — Developer
   ID Application + Apple Development + Apple Distribution — rather than
   replacing the secret with a phone-only bundle. Each pipeline picks its
   own identity out of the bundle; nothing conflicts.
4. **Register one device on the team** — CI archives sign with a development
   profile, and Apple refuses to mint one for a team with *zero* registered
   devices (RELEASE_LESSONS 2026-07-28 (d)). Plug your iPhone into your Mac →
   Finder → click the phone → click the subtitle line under its name until
   the **UDID** shows → right-click it → Copy UDID. Then
   developer.apple.com → Certificates, Identifiers & Profiles → Devices →
   **+** → paste. One-time; any one device unblocks every future archive.
5. **TestFlight** app from the App Store on your iPhone, signed into the same
   Apple ID.

**Each release (one button):**

1. Actions → **iOS release** → Run workflow with
   `export_method = app-store-connect`, `publish = true`. Leave
   `critical_alerts` **off** until Apple has granted that entitlement
   (request it at developer.apple.com; the app falls back to time-sensitive
   alerts meanwhile — flipping it early just fails signing).
2. ~15 min later the build appears in App Store Connect → TestFlight. Under
   Internal Testing, add a group with your Apple ID once; every future build
   auto-notifies your phone and installs/updates through the TestFlight app.
3. Each publish needs a `MARKETING_VERSION` bump in `ios/project.yml` — the
   workflow refuses to re-ship a version whose `ios-v*` tag already exists
   (that's it working, not breaking; see `docs/RELEASE_BUTTONS.md`).

A dry run first is house style (RELEASE_LESSONS principle 3): run the same
button with `publish = false` and `export_method = app-store-connect`, and
confirm the `.ipa` artifact appears before doing the real one. (Use
app-store-connect even for the dry run — `release-testing` is an ad-hoc
export, which requires registered devices on the team; the app-store path
needs none.)

The app icon App Store Connect requires is generated + committed
(`scripts/make_app_icon.py` → `Assets.xcassets`, same contract as the tvOS
icon); `Info.plist` declares exempt-only encryption
(`ITSAppUsesNonExemptEncryption=false`) so TestFlight builds don't park on
the export-compliance questionnaire.

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

The same run covers the wrist: building the SecuraCV scheme builds the
embedded watch app + complications, `heal.sh` fails if they didn't land
inside `SecuraCV.app` (an un-embedded watch app is otherwise silent), the
icon gate (`scripts/check_app_icon.py`) asserts both app-icon catalogs
structurally, and the wrist contract (`Shared/WristSnapshot.swift`) is
unit-tested in `SecuraCVTests`. The release workflow re-proves the embed on
the exported `.ipa` before validate/upload.
