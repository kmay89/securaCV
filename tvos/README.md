# SecuraCV on Apple TV — the Witness Wall (setup)

The native tvOS app: a calm, shared, always-on view of your fleet's **verified
record** — not a video wall. The what-and-why lives in
[`../docs/tvos/README.md`](../docs/tvos/README.md); the self-heal / self-publish /
self-update design is in [`../docs/tvos/AUTOPIPELINE.md`](../docs/tvos/AUTOPIPELINE.md).

> **Status: design + pipeline scaffold, not yet built.** The release pipeline
> ([`.github/workflows/tvos-release.yml`](../.github/workflows/tvos-release.yml))
> is wired and passes the CI policy checks, but it **no-ops** until you enable
> it — a push of a `tvos-v*` tag today prints a helpful message and exits
> green. The first real build needs **your Apple Developer account**; that
> can't come from CI or this repo alone. This doc is the exact checklist, and
> it's the same shape as [`../desktop-lab/MOBILE.md`](../desktop-lab/MOBILE.md).

## Intended layout

```
tvos/
├── README.md                     ← you are here
├── WitnessWall/                  ← the SwiftUI tvOS app (Xcode project)
├── witness-core/                 ← thin crate wrapping the shared Rust verifier
│                                    for the aarch64-apple-tvos target (FFI to Swift)
└── scripts/
    ├── build-witness-core.sh     ← builds witness-core for Apple TV
    └── release-tvos.sh           ← xcodebuild archive + export + upload to ASC
```

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

## Run it locally (once the app exists)

```sh
# build the shared verifier for Apple TV
bash scripts/build-witness-core.sh
# then open WitnessWall/ in Xcode and run on the tvOS Simulator or an Apple TV
```
