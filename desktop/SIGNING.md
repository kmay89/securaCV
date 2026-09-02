# Signing & notarizing the SecuraCV Flasher (macOS)

Turn the Flasher from "unsigned, needs an `xattr` command on first launch" into
**signed + notarized — double-click and it opens.** Everything is already wired;
this is the ordered, no-guessing runbook to switch it on.

> **The signing happens in CI**, not on your machine. You create the credentials
> once, store them as GitHub secrets, flip one variable, and every release the
> workflow builds is signed, notarized, and stapled. You never run `codesign`
> by hand.

**What's wired for you** (`.github/workflows/desktop-flasher-release.yml`):
opt-in on the `ENABLE_MACOS_SIGNING` repo variable; when on, `tauri-action`
imports your Developer ID cert, signs the `.app` + the `espflash`/`rpiboot`
sidecars + the `libusb` framework under the hardened runtime, notarizes with
your Apple ID + app-specific password, and staples the ticket. The Lab
(`desktop-release.yml`) uses the same switch.

---

## 0. Prerequisites

- **Apple Developer Program** membership ($99/yr) — the personal team from a
  free Apple ID **cannot** create a Developer ID cert.
- A **Mac** (to make the certificate request and export the key).

---

## 1. On your Mac + developer.apple.com (create the credentials)

**1a. Team ID** — [developer.apple.com/account](https://developer.apple.com/account)
→ **Membership details** → copy the 10-character **Team ID** (e.g. `AB12CD34EF`).
→ this is `APPLE_TEAM_ID`.

**1b. Make a certificate request (CSR)** — this generates your private key
locally and keeps it on your Mac:
- **Keychain Access** → menu **Certificate Assistant → Request a Certificate
  From a Certificate Authority…**
- **User Email**: your Apple ID email. **CA Email: leave blank.**
- Select **"Saved to disk"** → save `CertificateSigningRequest.certSigningRequest`.

**1c. Create the *Developer ID Application* certificate** — the one and only
correct type for a directly-downloaded (non-App-Store) app:
- developer.apple.com → **Certificates** → **＋**
- pick **"Developer ID Application"** ⚠️ *not* "Apple Distribution", *not*
  "Developer ID Installer" (that's for `.pkg`).
- Upload the CSR from 1b → **Download** the `.cer`.
- **Double-click the `.cer`** → it installs into your **login** keychain and
  pairs with the private key from 1b.

**1d. App-specific password (for notarization)** — the fastest method, no API
key needed:
- [appleid.apple.com](https://appleid.apple.com) → **Sign-In & Security** →
  **App-Specific Passwords** → **＋** → name it `securaCV notarize`.
- Copy the `xxxx-xxxx-xxxx-xxxx` it shows (you only see it once).
- → your Apple ID email is `APPLE_ID`; this password is `APPLE_PASSWORD`.

---

## 2. Export the cert and set the secrets — one command

On the Mac that holds the signing key:

```sh
bash desktop/scripts/set-desktop-signing-secrets.sh
```

It finds your **Developer ID Application** identity, repacks it into a `.p12`
holding **only** that one certificate and its matching private key, and sets
`APPLE_DESKTOP_CERTIFICATE`, `APPLE_DESKTOP_CERTIFICATE_PASSWORD` and
`APPLE_SIGNING_IDENTITY` — all three from the same certificate, so they cannot
disagree. Nothing is written to GitHub unless the file it built passes the same
checks the release workflow runs. macOS will ask permission to export the
private key; that prompt is the keychain doing its job.

Those three are the *certificate*. Signing also needs notarization credentials
and the `ENABLE_MACOS_SIGNING` variable, which the script does not invent — so
it finishes by listing whatever is still missing, with the command for each.
**If it prints "Still to do", work through that list (§3 explains each one)
before §4.** Take that seriously: the workflows build **unsigned** unless
`ENABLE_MACOS_SIGNING` is exactly `true`, so a half-finished setup ships an
unsigned app under a green checkmark instead of failing.

If it instead prints `notarization secrets present, ENABLE_MACOS_SIGNING=true`,
you're done here — go to **§4**.

<details>
<summary>Doing it by hand (and the four ways that went wrong)</summary>

Keychain Access → **login** → **My Certificates** → expand *"Developer ID
Application: Your Name (TEAMID)"* → select **the certificate row alone** →
right-click → **Export…** → `.p12` with an export password. Then
`base64 -i cert.p12 | pbcopy` → `APPLE_DESKTOP_CERTIFICATE`, the password →
`APPLE_DESKTOP_CERTIFICATE_PASSWORD`, and the quoted string from
`security find-identity -v -p codesigning` → `APPLE_SIGNING_IDENTITY`.

Every one of these failed a real dry run before the script existed:

- **`security export -t identities` exports *every* identity in the keychain.**
  Tauri validates the **last** certificate in the `.p12`, so an iOS cert riding
  along aborts the build with *"certificate … does not match provided
  identity"* even though the right cert is present. Two runs died here.
- **Keychain Access saves where it last saved.** `base64 -i <the path you
  assumed>` found no file, emitted nothing, and `gh secret set` stored an
  **empty** secret — which looks identical to "set" in the UI.
- **`VAR=path   # comment` in a pasted block.** zsh runs `#` as a command,
  `VAR` stays unset, and every check downstream reads zero.
- **A placeholder password pasted literally**, so the secret said
  `the-export-password`.

The script exists because none of those are visible until CI, and each costs a
release cycle to discover.

</details>

---

## 3. Add the GitHub secrets + variable

Repo → **Settings → Secrets and variables → Actions**.

**Secrets** (New repository secret), exactly these names:

> ⚠️ **These names are deliberately not `APPLE_CERTIFICATE`.** The iPhone /
> iPad / tvOS pipelines use `APPLE_CERTIFICATE` for an **Apple Distribution**
> `.p12` (App Store signing). The Mac apps need a **Developer ID Application**
> `.p12` (notarized, downloaded outside the App Store) — a different
> certificate for a different job. They shared one name until 2026-07-29, and
> setting up the iPhone app silently overwrote the desktop identity: three
> Flasher releases in a row failed with *"certificate ... does not match
> provided identity"* and 0.3.5 never shipped. One secret, one meaning.

| Secret | Value | Also used by |
|---|---|---|
| `APPLE_DESKTOP_CERTIFICATE` | base64 of a **Developer ID Application** `.p12` holding that one identity | Flasher + Lab only |
| `APPLE_DESKTOP_CERTIFICATE_PASSWORD` | that `.p12`'s export password | Flasher + Lab only |
| `APPLE_SIGNING_IDENTITY` | `Developer ID Application: Your Name (TEAMID)`, byte-for-byte | Flasher + Lab only |
| `APPLE_ID` | your Apple ID email | shared with notarization |
| `APPLE_PASSWORD` | the app-specific password from **1d** | shared with notarization |
| `APPLE_TEAM_ID` | the 10-char Team ID from **1a** | shared |

The release workflows check the `.p12` **before** building: if it doesn't
contain the identity `APPLE_SIGNING_IDENTITY` names, the run stops in seconds
and prints every certificate it did find, instead of dying ten minutes into a
bundle.

### Who signs with what, across this repo

| Target | Certificate | Secret holding it |
|---|---|---|
| Flasher / Lab (macOS `.dmg`) | **Developer ID Application** | `APPLE_DESKTOP_CERTIFICATE` |
| iPhone / iPad / tvOS (App Store) | **Apple Distribution** | `APPLE_CERTIFICATE` |

Both can — and should — exist at once. They are different certificates for
different distribution channels; neither is a substitute for the other.

**Variable** (the **Variables** tab, *not* Secrets):

| Variable | Value |
|---|---|
| `ENABLE_MACOS_SIGNING` | `true` |

> The switch is a **variable**, not a secret, on purpose: the workflow must be
> able to read it in an `if:` to choose the signed vs unsigned build path.

---

## 4. Verify BEFORE you publish (the repo's "prove the bundle first" rule)

Do **not** cut a real release first. Run a build-only pass — it still signs +
notarizes in CI, so it surfaces any problem without touching the Releases page.

1. Actions → **"Desktop Flasher — build & release"** → **Run workflow** → leave
   **`dry_run` = true** (the default) → Run.
2. Watch the **macOS** leg. In the `Build & release (signed + notarized macOS)`
   step you should see codesign then a notarization submission that finishes
   **Accepted**. Notarization takes a few minutes — that's normal.
3. Download the run's **`flasher-macos-latest`** artifact and check locally:

```sh
# unzip the artifact, then:
codesign -dv --verbose=4 "SecuraCV Flasher.app"     # Authority = Developer ID Application: …
spctl -a -vvv -t install  "SecuraCV Flasher.app"    # → accepted  source=Notarized Developer ID
xcrun stapler validate    *.dmg                      # → The validate action worked!
```

If all three pass, you're done — signing is real.

---

## 5. Cut the signed release

Signing changes nothing about versioning — the repo's rule still holds: **a
release version that already exists is not a release** (bump first).

1. Bump the version in **all four** files that must agree — the release fails
   loudly at `desktop/scripts/check_app_versions.py` if any drift:
   `desktop/src-tauri/tauri.conf.json`, `desktop/package.json`,
   `desktop/src-tauri/Cargo.toml` (easy to forget — it's `CARGO_PKG_VERSION`,
   the version the running app reports), **and**
   `desktop/src-tauri/Cargo.lock` (refresh it by running
   `cargo update -w` in `desktop/src-tauri`, or by building once) — e.g.
   `0.3.0` → `0.3.1`. Run `python3 desktop/scripts/check_app_versions.py`
   first to confirm.
2. Either push a tag `flasher-v0.3.1` (matching the version from step 1), or
   Actions → Run workflow with **`dry_run = false`**.
3. The release's `.dmg`/`.app` are now signed + notarized; users double-click to
   open. (The `xattr` step in `desktop/INSTALL.md` no longer applies to signed
   builds.)

---

## Troubleshooting

- **"The binary is not signed with a valid Developer ID certificate"** on a
  *nested* file during notarization → a bundled Mach-O wasn't signed. The app
  bundles three: `espflash` + `rpiboot` (Tauri signs these as `externalBin`) and
  `libusb-1.0.0.dylib` (signed because it's declared under
  `bundle.macOS.frameworks`, **not** `resources` — a dylib in `Resources/`
  is *not* signed and is the classic cause). If a new nested binary is added,
  bundle it as a framework or sidecar, never a bare resource.
- **"You must first sign the relevant contracts"** → accept the latest Apple
  Developer + Program License agreements at developer.apple.com, then re-run.
- **`APPLE_DESKTOP_CERTIFICATE` import fails** → the base64 must be the whole `.p12`
  (`base64 -i file.p12`), and `APPLE_DESKTOP_CERTIFICATE_PASSWORD` must be the **export**
  password you set in 2a, not your Apple ID password.
- **Notarization hangs / times out** → it's an Apple-side queue; re-run the
  workflow. The submission is idempotent.
- **Alternative to the app-specific password:** an App Store Connect API key
  (`APPLE_API_KEY` + `APPLE_API_ISSUER` + the `.p8`) also works and doesn't
  expire — the repo already uses that pattern for iOS/tvOS. The app-specific
  password is simpler to set up, which is why it's the default here.

## Not covered here: the branded permission prompt

Signing does **not** by itself change the macOS disk-write prompt from
"`authopen` wants to make changes" to "SecuraCV Flasher wants to make changes" —
that's Apple's built-in helper. Rebranding it needs a **signed privileged helper**
(`SMAppService`), which *builds on* this signing being in place. It's a separate,
larger piece; do this first.

---

## The updater (minisign) key — shared by both apps

Separate from the Apple signing above: every self-update artifact (the
`.tar.gz` / `.AppImage` the in-app updater downloads) is signed with a
**minisign** key that Tauri's updater plugin verifies against the public key
embedded in the app.

- **One key for both apps, by design.** `desktop/src-tauri/tauri.conf.json`
  and `desktop-lab/src-tauri/tauri.conf.json` embed the same
  `plugins.updater.pubkey`; `canary-local/tests/desktop_parity.test.js` pins
  them equal so a rotation cannot land in one and not the other. The channel
  separation (`flasher-latest` vs `lab-latest`) is by manifest URL, not by key.
- **Secrets:** `TAURI_SIGNING_PRIVATE_KEY` (the private key file's contents)
  and `TAURI_SIGNING_PRIVATE_KEY_PASSWORD`. Generate once with
  `npx @tauri-apps/cli signer generate -w ~/.tauri/securacv.key`.
- **Missing secret = no publish.** `desktop-flasher-release.yml` and
  `desktop-release.yml` mint an ephemeral key for build-only runs and exit with
  an error on a publish, for the same reason `firmware-release.yml` refuses to
  publish without its OTA key: an update signed with a throwaway key bricks
  self-update for every installed copy until the next real release.
- **Rotation:** generate a new pair, update `pubkey` in BOTH config files in
  the same commit, replace the two secrets, then publish. Installs that were
  built with the old pubkey will not accept the first new release over
  self-update — they need a manual download once. Say so in RELEASE_NOTES.md.
