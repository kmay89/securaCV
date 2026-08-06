# SecuraCV on Apple TV — the Witness Wall (setup)

The native tvOS app: a calm, shared, always-on view of your fleet's **verified
record** — not a video wall. The what-and-why lives in
[`../docs/tvos/README.md`](../docs/tvos/README.md); the self-heal / self-publish /
self-update design is in [`../docs/tvos/AUTOPIPELINE.md`](../docs/tvos/AUTOPIPELINE.md).

> **The Apple TV also stands watch.** Beyond drawing the verified record, the
> Wall can be the household's *resident*: turn on "Stand watch" and this Apple
> TV posts the coarse away wake — into your own iCloud, one class word, no
> device name or time — when a Canary goes dark or a chain stops verifying.
> That closes the hole in the away path, because the phone that left the house
> is the one device that can no longer notice anything. Opt-in, off until you
> ask, and honest about its limit: tvOS pauses an app that is not on screen,
> so the watch runs while the Wall is the app on the TV. See
> [`ResidentWatch.swift`](WitnessWall/Sources/WitnessWall/ResidentWatch.swift).
>
> **Status: built and continuously tested; the App Store upload is waiting on
> an Apple Developer account.** The app and its verification core are real and
> are exercised on every PR by
> [`.github/workflows/tvos.yml`](../.github/workflows/tvos.yml) — the Rust core
> (including the check that pins its chain math to the kernel's own fixtures)
> and a full SwiftUI build + unit-test run on the **tvOS Simulator**, with no
> signing and no secrets. What still no-ops is only the *signed upload*
> ([`tvos-release.yml`](../.github/workflows/tvos-release.yml)): a `tvos-v*`
> tag today prints a helpful message and exits green until you enable it,
> because signing needs **your Apple Developer account** and that can't come
> from CI or this repo alone. The checklist below is the whole remaining gap,
> and it's the same shape as [`../desktop-lab/MOBILE.md`](../desktop-lab/MOBILE.md).

## What makes this the best at what it does

We surveyed the field (Aug 2026): Ring has **no** Apple TV app and the one
approved third-party viewer stacks a second subscription on a Ring plan;
Nest's TV story is cloud-only and its history tier took a 25% price hike;
eufy/Wyze/Tapo/Reolink have no TV app at all; Apple's own Home tab is
live-view-only with **no history on tvOS**; and the one good first-party app
(UniFi Protect) still demands cloud SSO login and $200+ hardware. Three
categories are simply **empty**, and this app sits in all three:

1. **Integrity you can see.** No shipping product on any TV platform renders
   cryptographic proof that the record is intact. The Wall's "verified ✓" is
   the Rust core actually verifying the kernel's chain — pinned by CI to the
   kernel's own test vectors, never a hardcoded string.
2. **Zero-typing, zero-cloud setup.** Turn it on: the Wall probes the same
   well-known LAN addresses the desktop Flasher and Lab probe
   (`canary.local`), finds the fleet by itself, and remembers it. No account,
   no subscription, nothing leaves the room — and tvOS's own constraints
   (storage the OS may purge, foreground-only apps) match the architecture:
   the TV never holds the record, it witnesses, displays, and proves.
3. **Built for the rooms TVs are actually in.** One wall, three profiles —
   the living-room **Home** wall, the behind-the-counter **Business** board
   (denser, attention-first), the **Apartment** peephole (the door leads) —
   plus four skins. A profile changes layout and emphasis, never the data,
   so no mode can claim what another wouldn't.

The honesty rules are tests, not intentions: stale data is announced in the
loudest element on screen, a hub that vanishes can never keep drawing a green
fleet, and a squatted `canary.local` that answers with a login page is
skipped, not trusted (`Tests/WitnessWallTests/`).

## Layout

```
tvos/
├── README.md                     ← you are here
├── WitnessWall/                  ← the SwiftUI tvOS app
│   ├── project.yml               ← XcodeGen spec; the .xcodeproj is GENERATED,
│   │                               never committed (same rule as ios/project.yml)
│   ├── Sources/WitnessWall/      ← the app: FFI wrapper, fleet client, state, views
│   ├── Support/                  ← Info.plist + the module map for the C header
│   └── Tests/WitnessWallTests/   ← unit tests, run on the simulator in CI
├── witness-core/                 ← the chain math, compiled for Apple TV
│   ├── src/                      ← verify + fleet parsing + the C ABI
│   ├── include/                  ← the hand-written C header Swift imports
│   └── tests/vectors.rs          ← pins the math to the KERNEL'S OWN fixtures
└── scripts/
    ├── build-witness-core.sh     ← builds + stages the core (device + simulator)
    ├── stamp_build.sh            ← build identity for the About/Health panel
    ├── make_app_icon.py          ← generates the icon: our standard Canary on
    │                               a birdfeeder, across the 3 parallax layers
    ├── check_app_icon.py         ← CI gate: every image, at Apple's exact sizes
    └── release-tvos.sh           ← xcodebuild archive + export + upload to ASC
```

### Why there is a second implementation of the chain math

`tvos/witness-core` does not import the kernel crate — it *can't*: the kernel is
welded to `rusqlite`/sqlcipher and GStreamer, none of which cross-compile to
`aarch64-apple-tvos`. So it follows the precedent the offline JavaScript
verifier already set (`viewer/verify_core.js`): re-implement the **pinned
bytes**, then prove byte-identity in CI against
`tests/fixtures/envelope/domain_separation_vectors.json` — the same fixtures
`src/crypto/signatures.rs` checks itself against. That check
(`tvos/witness-core/tests/vectors.rs`) runs on every PR *and* again on the
release path. If it ever fails, the TV is wrong. Never "fix" it by regenerating
the vectors from this crate — that deletes the only thing keeping the two in
agreement.

The point, same as iOS: **reuse, don't rebuild.** The chain-verification logic
is the workspace's existing Rust core, compiled for Apple TV — never a second
implementation that could disagree with the kernel.

## What you need (one time)

- A **Mac** with **Xcode** (+ Command Line Tools) and the **tvOS SDK**.
- An **Apple Developer account** — the $99/yr Program is required for
  TestFlight / the App Store (the free tier runs the tvOS Simulator only).
- Rust with the Apple TV core target (tier-3, so it builds with the std source):
  ```sh
  rustup toolchain install nightly
  rustup component add rust-src --toolchain nightly
  rustup target add aarch64-apple-tvos --toolchain nightly   # or -Z build-std
  ```

## Turn the pipeline on

The workflow stays a no-op until **both** are true — mirroring the iOS gate:

1. Set the repo **variable** `ENABLE_TVOS_BUILD=true`
   (Settings → Secrets and variables → Actions → Variables).
2. Add the Apple signing **secrets** (Settings → Secrets and variables →
   Actions → Secrets):

   | Secret | What it is |
   |---|---|
   | `APPLE_API_KEY_BASE64` | The App Store Connect `.p8` key, base64-encoded |
   | `APPLE_API_KEY` | The key ID (the `AuthKey_<ID>.p8` ID) |
   | `APPLE_API_ISSUER` | The App Store Connect issuer ID |
   | `APPLE_DEVELOPMENT_TEAM` | Your Apple Developer team ID |

Then ship the way everything else ships:

```sh
git tag tvos-v0.1.0 && git push origin tvos-v0.1.0
```

CI builds the witness core for `aarch64-apple-tvos`, builds + signs the app,
and uploads it to App Store Connect. From there Apple's phased release carries
it to every Apple TV — nothing to sideload. See
[`../docs/tvos/AUTOPIPELINE.md`](../docs/tvos/AUTOPIPELINE.md) for the full
never-rot mapping.

## Run it locally

```sh
# 1. build + stage the verification core (device and simulator slices)
bash scripts/build-witness-core.sh
#    …or just the simulator, which is all you need to run on a Mac:
TVOS_SLICES=simulator bash scripts/build-witness-core.sh

# 2. generate the Xcode project from project.yml (never commit the .xcodeproj)
brew install xcodegen                 # once
cd WitnessWall && xcodegen generate

# 3. open it, then run on the tvOS Simulator or a real Apple TV
open WitnessWall.xcodeproj
```

No Mac handy? The part most worth testing runs anywhere:

```sh
cd witness-core && cargo test          # chain math + the kernel-agreement vectors
```

Point the running app at a fleet with the reference kernel below
(`python3 tvos/discovery/mock-kernel.py`, then enter `http://localhost:8099`).

## Get it onto a real Apple TV, fast

The least-headache runbook — from a 2-minute AirPlay demo (no account) to an
on-device Xcode build (free Apple ID) to TestFlight — is
**[`RUN_ON_APPLE_TV.md`](RUN_ON_APPLE_TV.md)**.

## Find your real Canaries from a browser

The tvOS app and the web emulator both show your actual fleet when it's
reachable, over one tiny CORS'd endpoint (`GET /api/fleet`). The contract, the
browser rules that decide where it can work, and a **runnable reference kernel**
you can point the emulator at live are in
**[`discovery/DISCOVERY.md`](discovery/DISCOVERY.md)**:

```sh
python3 tvos/discovery/mock-kernel.py     # then Connect the emulator to it
```

## Put it inside the Flasher & the Lab

The emulator is one self-contained web view, so it drops into both Tauri apps —
including the "flash a Canary and watch it appear on the wall" moment in the
Flasher. The integration plan (and why it needs a macOS/Tauri build to verify
before shipping) is in **[`EMBED_IN_APPS.md`](EMBED_IN_APPS.md)**.

## Trademarks

Apple, Apple TV, tvOS, Siri, Mac, iPhone, iPad, HomeKit, AirPlay, Xcode, and
TestFlight are trademarks of Apple Inc., registered in the U.S. and other
countries and regions. App Store and App Store Connect are service marks of
Apple Inc. SecuraCV is an independent project by Errer Labs and is **not
affiliated with, endorsed, sponsored, or certified by Apple Inc.** References to
Apple products are nominative — for identification and interoperability only.
