# SecuraCV Lab on iPhone & iPad (Tauri v2 mobile)

The whole point of the [capability-layer](README.md) design: **iOS and iPadOS
reuse the exact same frontend as the web Lab and the desktop app** — the
`canary-local` build line, the isometric room, the six-stage adaptive nav
(which becomes a **bottom tab bar on iPhone** and a **sidebar on iPad**). We
just wrap it in a Tauri v2 mobile shell. One frontend, four platforms.

> **Status: scaffolded, not yet built.** Everything below is wired, but the
> first real build needs **your Apple Developer account** (signing +
> provisioning) — that can't be done from CI or this repo alone. This doc is
> the exact checklist.

---

## What you need (one time)

- A **Mac** with **Xcode** (+ Command Line Tools) and an **iOS Simulator**.
- An **Apple Developer account** (free tier runs the Simulator and a 7-day
  device build; the $99/yr Program is required for TestFlight / the App Store).
- Rust with the iOS targets:
  ```sh
  rustup target add aarch64-apple-ios aarch64-apple-ios-sim x86_64-apple-ios
  ```

## Run it locally

From `desktop-lab/`:

```sh
npm install
npm run ios:init     # tauri ios init — generates the Xcode project (src-tauri/gen/apple)
npm run ios:dev      # build + run in the Simulator (or a plugged-in device)
```

`ios:init` scaffolds `src-tauri/gen/apple/` (the Xcode project). It's generated
from `tauri.conf.json` + `Cargo.toml`, so it's reproducible — commit it or
regenerate in CI (the workflow does the latter).

## Ship a build

```sh
npm run ios:build    # tauri ios build — produces an .ipa
```

For a **signed** build (device / TestFlight), set your team + let Tauri sign:

```sh
export APPLE_DEVELOPMENT_TEAM=XXXXXXXXXX   # your 10-char Team ID
npm run ios:build -- --export-method app-store-connect
```

Add to `src-tauri/tauri.conf.json` when you're ready to pin the deployment
target (optional; defaults are sane):

```jsonc
"bundle": {
  "iOS": { "minimumSystemVersion": "14.0" }   // iPhone 6s and up
}
```

(The bundle **identifier** `com.securacv.lab` already works for iOS.)

---

## The capability layer on iOS — native where it can, honest where it can't

iOS is the strictest surface, and the layer already accounts for it (see the
[cross-ecosystem plan](../desktop-lab/README.md) and the capability seam in
`src-tauri/src/lib.rs`):

| Capability | iOS |
|---|---|
| **USB flashing** | ❌ Apple blocks generic USB-serial. The **Flash bench is hidden on iOS**; it points to the desktop app instead. |
| **Discovery (mDNS / BLE)** | ✅ native (CoreBluetooth / Bonjour) — live fleet on your phone. |
| **Notifications** | ✅ native — alerts on signed events. |
| **Sealed local storage** | ✅ Keychain-backed. |

Nothing pretends to work that can't. The frontend reads the capability seam and
hides/redirects the pieces iOS won't allow.

---

## CI

[`.github/workflows/desktop-mobile-release.yml`](../.github/workflows/desktop-mobile-release.yml)
runs an iOS build on a macOS runner **on manual dispatch only** (so it never
blocks other CI). It's gated behind the `ENABLE_IOS_BUILD` repository variable
and the Apple signing secrets — until those are set, the job no-ops with a clear
message rather than failing. Set them once your provisioning is in place:

- Variable: `ENABLE_IOS_BUILD = true`
- Secrets: `APPLE_DEVELOPMENT_TEAM`, plus the same
  `APPLE_CERTIFICATE` / `APPLE_CERTIFICATE_PASSWORD` / `APPLE_ID` /
  `APPLE_PASSWORD` / `APPLE_TEAM_ID` the desktop pipeline already documents, and
  a provisioning profile (`APPLE_MOBILE_PROVISION`).

## Roadmap fit

This is **Phase 2** of the cross-platform plan (web + macOS + Linux ship today).
Phase 3 adds the native capabilities above through the seam; the same shell then
also covers **Android** (`tauri android init`) with almost no extra work.
