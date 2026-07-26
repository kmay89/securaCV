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

## 2. Export the cert + read its identity (on your Mac)

**2a. Export the `.p12`:**
- Keychain Access → **login** keychain → **My Certificates**
- find **"Developer ID Application: Your Name (TEAMID)"** → right-click →
  **Export…** → save `flasher-cert.p12` → set an **export password** (remember
  it — it becomes `APPLE_CERTIFICATE_PASSWORD`).

**2b. Base64-encode it** for the secret (Terminal):

```sh
base64 -i flasher-cert.p12 | pbcopy      # now on your clipboard → APPLE_CERTIFICATE
```

**2c. Read the exact identity string** (must match byte-for-byte):

```sh
security find-identity -v -p codesigning
#  →  1) ABC...  "Developer ID Application: Your Name (AB12CD34EF)"
```

Copy the quoted string → `APPLE_SIGNING_IDENTITY`.

---

## 3. Add the GitHub secrets + variable

Repo → **Settings → Secrets and variables → Actions**.

**Secrets** (New repository secret), exactly these names:

| Secret | Value |
|---|---|
| `APPLE_CERTIFICATE` | the base64 blob from **2b** |
| `APPLE_CERTIFICATE_PASSWORD` | the `.p12` export password from **2a** |
| `APPLE_SIGNING_IDENTITY` | `Developer ID Application: Your Name (TEAMID)` from **2c** |
| `APPLE_ID` | your Apple ID email |
| `APPLE_PASSWORD` | the app-specific password from **1d** |
| `APPLE_TEAM_ID` | the 10-char Team ID from **1a** |

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

1. Bump the version in **all three** files that must agree — the release fails
   loudly at `desktop/scripts/check_app_versions.py` if any drift:
   `desktop/src-tauri/tauri.conf.json`, `desktop/package.json`, **and**
   `desktop/src-tauri/Cargo.toml` (this last one is easy to forget — it's
   `CARGO_PKG_VERSION`, the version the running app reports) — e.g. `0.3.0` →
   `0.3.1`. Run `python3 desktop/scripts/check_app_versions.py` first to confirm.
2. Either push a tag `flasher-v0.2.3`, or Actions → Run workflow with
   **`dry_run = false`**.
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
- **`APPLE_CERTIFICATE` import fails** → the base64 must be the whole `.p12`
  (`base64 -i file.p12`), and `APPLE_CERTIFICATE_PASSWORD` must be the **export**
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
