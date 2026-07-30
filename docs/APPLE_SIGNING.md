# Apple signing across every SecuraCV target

One page for a question that has cost real time twice: **which certificate signs
which app, and which secret carries it.** Apple ships two unrelated certificate
types with confusingly similar names, and using the wrong one fails *after* a
full build with a message that blames the identity string.

Setup runbooks live next to their apps — [`desktop/SIGNING.md`](../desktop/SIGNING.md)
for the Mac apps. This page is the map.

## The matrix

| Target | Ships via | Certificate | Certificate secret | Notarized? |
|---|---|---|---|---|
| Flasher (macOS `.dmg`) | direct download | **Developer ID Application** | `APPLE_DESKTOP_CERTIFICATE` | yes |
| Lab (macOS `.dmg`) | direct download | **Developer ID Application** | `APPLE_DESKTOP_CERTIFICATE` | yes |
| iPhone / iPad | App Store | **Apple Distribution** | `APPLE_CERTIFICATE` | n/a |
| Apple TV | App Store | **Apple Distribution** | `APPLE_CERTIFICATE` | n/a |
| Watch | App Store (with its phone app) | **Apple Distribution** | `APPLE_CERTIFICATE` | n/a |
| Mac App Store build, if ever | App Store | **Apple Distribution** | `APPLE_CERTIFICATE` | n/a |

The split is **not** Mac-versus-phone — it is **how the user gets the app**:

- **Anything downloaded from a website** needs *Developer ID Application* plus
  notarization, or macOS refuses to open it.
- **Anything installed through an App Store** needs *Apple Distribution*. Apple
  notarizes App Store builds itself.

A Mac app could need either, depending on where it's published. That is exactly
why the two live in separate secrets.

> **The two secret names are deliberately different.** They shared the name
> `APPLE_CERTIFICATE` until 2026-07-29. Setting up the iPhone app overwrote the
> desktop identity, and three Flasher releases in a row died with *"certificate
> … does not match provided identity"* while 0.3.5 sat unshipped. One secret,
> one meaning. See `.github/RELEASE_LESSONS.md` (m).

## The supporting secrets

| Secret | Used by | What it is |
|---|---|---|
| `APPLE_DESKTOP_CERTIFICATE_PASSWORD` | Flasher, Lab | export password of the Developer ID `.p12` |
| `APPLE_SIGNING_IDENTITY` | Flasher, Lab | `Developer ID Application: NAME (TEAMID)`, byte-for-byte |
| `APPLE_CERTIFICATE_PASSWORD` | iPhone, iPad, tvOS | export password of the Apple Distribution `.p12` |
| `APPLE_ID` | desktop notarization | Apple ID email |
| `APPLE_PASSWORD` | desktop notarization | **app-specific** password, not the account password |
| `APPLE_API_KEY` / `APPLE_API_KEY_BASE64` / `APPLE_API_ISSUER` | App Store upload | App Store Connect API key (`.p8`) |
| `APPLE_TEAM_ID` *or* `APPLE_DEVELOPMENT_TEAM` | everything | the 10-character Team ID |

**Team ID:** two names exist for one value, for historical reasons — desktop
workflows read `APPLE_TEAM_ID`, the mobile and tvOS ones `APPLE_DEVELOPMENT_TEAM`.
Every workflow now falls back to the other, so **setting either one is enough**.
Don't add a second copy that can drift.

**Repo variable:** `ENABLE_MACOS_SIGNING` must be exactly `true` or the desktop
workflows build **unsigned** — and succeed while doing it. A green run is not
evidence of a signed app; check the release body, which now states which it is.

## Certificates are per account; profiles are per app

- Certificates and their private keys belong to the **developer account**, not
  to an app. One Developer ID Application certificate signs every Mac app you
  ship outside the App Store; one Apple Distribution certificate covers every
  App Store target. Adding an app does **not** mean adding a certificate.
- **Provisioning profiles** are per bundle ID, and the App Store workflows
  create them on demand with `-allowProvisioningUpdates`. Adding an app means a
  new bundle ID, not new signing setup.
- Apple limits Developer ID certificates per account, and revoking one breaks
  apps already shipped under it. **Back up the `.p12` and its password** in a
  password manager. `set-desktop-signing-secrets.sh` leaves a copy in
  `~/securacv-signing/` for exactly this reason.

## Adding a target

1. **Another Mac app, direct download** — nothing to set up. Reuse
   `APPLE_DESKTOP_CERTIFICATE` and `APPLE_SIGNING_IDENTITY`, and copy the
   preflight step from `desktop-flasher-release.yml` so it fails in seconds
   rather than mid-bundle.
2. **Another iPhone / iPad / tvOS / Watch app** — reuse `APPLE_CERTIFICATE`.
   Register the bundle ID; the profile is created automatically.
3. **A Mac App Store build of an app that also ships as a DMG** — that one app
   needs *both* certificates, one per pipeline. Never try to make one do both.

## When signing breaks

Read the preflight output first: **"Verify the macOS signing certificate matches
the identity"** in the desktop workflows lists every certificate inside the
`.p12`, states which identity was requested, re-encrypts the file into the form
macOS accepts, and proves it by importing it into a throwaway keychain. It fails
in about twenty seconds and names the cause.

Then, for the Mac apps, the fix for almost everything is one command on the Mac
holding the key:

```sh
bash desktop/scripts/set-desktop-signing-secrets.sh
```

Five distinct failures preceded Flasher 0.3.5, and only one was an Apple
subtlety — the rest were a human runbook over a credential whose correctness was
only observable in CI. `.github/RELEASE_LESSONS.md` entries (m) through (p) have
the details; the short version:

| Symptom | Cause |
|---|---|
| `certificate … does not match provided identity` | wrong certificate *type*, or extra identities in the `.p12` (Tauri validates the **last** one) |
| preflight says the secret is empty | `base64 -i` was pointed at a path that didn't exist; `gh secret set` accepts an empty value |
| `MAC verification failed during PKCS12 import (wrong password?)` | **not** the password — OpenSSL 3 wrote a SHA-256 MAC that macOS cannot read. The preflight now re-encrypts around this |
| build succeeds but the app is unsigned | `ENABLE_MACOS_SIGNING` is not exactly `true` |

## Known gap: notarization still uses an app-specific password

Desktop notarization authenticates with `APPLE_ID` + `APPLE_PASSWORD`. It works
(proven end-to-end in Flasher 0.3.5: signed, notarized, and self-updated on a
real machine). But an app-specific password is tied to one human's Apple ID and
is invalidated whenever that account's password changes, so it will fail one day
for a reason unrelated to this repo.

The better credential is already here — the App Store Connect API key the iOS
pipeline uses (`APPLE_API_KEY`, `APPLE_API_KEY_BASE64`, `APPLE_API_ISSUER`), and
`ios-release.yml` already decodes the `.p8` and exports `APPLE_API_KEY_PATH`.
Switching is deliberately **not** done yet: verify Tauri's own env contract for
API-key notarization first, and prove it on a `dry_run=true` build before it
touches a real release. Don't churn a signing path that has only just been
proven.
