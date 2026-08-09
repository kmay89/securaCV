# Which button do I press?

Every way to ship SecuraCV, what it does, and — the part that costs people time —
**when not to press it**. If you only read one line: press
**"Update everything (only what needs it)"**. It works out what genuinely needs
releasing, does exactly that, and explains anything it deliberately left alone.

For the *why* behind firmware channels and version grammar, see
[`RELEASE_PROCESS.md`](RELEASE_PROCESS.md). This page is the operator's index.
For failures we have already paid for once, see
[`.github/RELEASE_LESSONS.md`](../.github/RELEASE_LESSONS.md).

---

## The master button

### 🟢 Update everything (only what needs it)

**Actions → "Update everything (only what needs it)" → Run workflow.**

Reads [`.github/release-targets.yml`](../.github/release-targets.yml), compares
each target's **source version** against what has actually been tagged, and
dispatches only the ones that are ahead. Everything else it reports and skips.

| What it finds | What it does |
|---|---|
| source version newer than the newest tag | releases it (which is what cuts the tag) |
| never released | cuts the first release |
| same version, but watched files changed | **"bump the version first"** — ships everything else |
| same version, nothing changed | skips ✅ |
| site pages changed since the last deploy | redeploys the site |
| Apple target with `ENABLE_*_BUILD` off | leaves it alone (the workflow would no-op) |
| firmware, but no OTA signing key | reports it gated, with the ceremony |

**Use it:** whenever you want the world to be current and don't want to think
about which targets moved. Pressing it twice costs nothing the second time —
that's the design.

**Don't:** reach for a narrower button first. This one is the only thing that
knows the whole graph.

**Safe by construction.** It never re-ships a different tree under a published
version, and it doesn't go red for telling you a version needs bumping — that's
information, not failure. Tick **`plan_only`** to see the plan and dispatch
nothing. The decision engine is unit-tested (`.github/scripts/test_release_plan.py`,
47 cases) because this decision *is* the product.

---

## When you want exactly one thing

### Release — one click (firmware + apps + web)

Runs a chosen set **unconditionally**, no "does it need it?" reasoning.

**Use it:** you know precisely what you want, e.g. re-cutting the apps after a
packaging fix with no version change.

**Don't:** use it as your default. It will happily publish an app whose version
is already released, which **overwrites that release's assets instead of cutting
a new download** — see *An app release that already exists* below.

### Firmware Release

Builds, signs, and publishes every board's firmware for one version. Pick
`channel` (release/dev) and `version`; it creates the tag so the tag and the
built commit can never disagree.

**Use it:** cutting firmware and nothing else, or a dev-channel build
(`2.4.0-dev.1`) that only dev-channel devices can see.

**Don't:** use it if you haven't bumped `FIRMWARE_VERSION` / `CANARY_FW_VERSION`
to match — the version-string guard greps each binary and fails closed. And it
refuses a tag that already exists: bump, don't reuse.

### Firmware Release — if changed

The firmware-only version of the master button.

**Use it:** you only care about firmware and want the "release if it moved"
behavior. Otherwise the master button already covers it.

### Flasher Factory Images

Rebuilds *only* the browser-flasher factory images and `manifest-flash.json`, and
attaches them to a release. Pick a `channel`.

**`channel: release`** — an **existing** `fw-v*` tag.

**Use it** in two situations:
1. A packaging-tooling fix should reach an already-cut tag without a new version.
2. **Before the OTA key exists.** The browser flasher's integrity rests on
   SHA-256 + same-origin, not the Ed25519 release key, so these assets can ship
   pre-ceremony. If `Firmware Release` hard-stops on the missing key, this lights
   the flasher up anyway.

**Don't:** expect it to help a tag that doesn't exist — it checks out the tag, so
the tag has to be there first. It now fails with that sentence rather than a raw
git error, and points you at `channel: dev`. Read the version-burn warning below
before creating a tag by hand.

**`channel: dev`** — **no tag, no version, no signing key.** Builds the branch
you dispatch from and publishes to the rolling `fw-dev-latest` prerelease, which
is exactly what the flasher's **Advanced → dev channel** toggle reads.

**Use it:** hardware is on the bench and you need a real flashable image *today*
— a new board, a demo, a bring-up. It's the shortest path from "the code is on a
branch" to "the product is installable", and it burns no version.

**Don't:** point a stranger at it. Dev images are unsigned *while the key
ceremony is pending*: verified by SHA-256 against the manifest, and both
flashers say so on the banner and the receipt. Stable installs belong on a
signed release. It also cannot reach `releases/latest` — it publishes a
prerelease, on purpose, so the fleet's OTA URL never lands on it.

Once the key exists this button signs like every other path, and **refuses to
run** if the key is pinned but `OTA_SIGNING_KEY_PEM` isn't set — an unsigned
manifest published after the ceremony isn't a weaker install, it's an
uninstallable one (the flashers' policy becomes `require-signature` and they
reject the whole manifest).

### Build Mac apps (Flasher + Lab)

The two desktop app pipelines, nothing else. Superseded by the master button for
most purposes; keep for a macOS-only smoke run.

### Publish the Lab (draft → live)

Takes the Lab's newest `app-v*` **draft** and finishes it: publishes it (never
as `releases/latest`), puts `releases/latest` back on the firmware, then
verifies the updater manifest and advances **`lab-latest`** — the pointer every
installed Lab polls. Blank `tag` publishes the newest draft; pass one to be
explicit.

**Use it:** after the master button has cut a Lab release, to actually ship it.
This is the step that makes installed Labs see the update; without it the
release exists only as a draft nobody can download.

**Don't:** expect it to build anything — it only finishes a draft the build
already produced. It refuses a draft older than what `lab-latest` serves, since
publishing that would strand the installed base on a stale manifest.

**Why it's a separate button.** GitHub suppresses `release:` events for
releases published with `GITHUB_TOKEN`, so the pointer and the latest-guard
can't be *triggered* by a CI publish — they have to be **called**. This
workflow calls both, and calls the pointer as a reusable workflow so a broken
updater manifest fails the publish run instead of passing quietly. (A human
clicking Publish in the UI *does* fire the event, so that path still works.)

---

## The three things that bite

### Firmware will not release

**Symptom.** The master button reports firmware as **gated**; or `Firmware
Release` dies after ~20 seconds with `OTA_SIGNING_KEY_PEM secret is not set`; or
every product in the flasher reads *"no published release yet"*.

**Cause.** Signing needs both halves of the Ed25519 release key, and they live in
different places:

| half | lives in | check it |
|---|---|---|
| **public** | `firmware/common/ota/src/ota_release_key.h` | `python3 firmware/scripts/ota_key_state.py` |
| **private** | the `OTA_SIGNING_KEY_PEM` Actions secret | Settings → Secrets and variables → Actions |

All zeros is the shipped default and means OTA is **hard-disabled in firmware** —
a safe default, but nothing can ship until it's replaced.

**The one-time ceremony.** On your own machine — never in CI or a cloud shell,
because this generates your master signing key:

```sh
firmware/scripts/setup_release_key.sh --key ~/securacv-releaser.pem
```

If it stops on a missing `cryptography` module and `pip` refuses with
`externally-managed-environment` (PEP 668 — normal on Homebrew Python), use a
throwaway venv; the script uses whatever `python3` is first on `PATH`:

```sh
python3 -m venv ~/.venvs/securacv
source ~/.venvs/securacv/bin/activate
pip install cryptography
```

Then, in this order:

1. **Add the secret.** `pbcopy < ~/securacv-releaser.pem`, paste it as
   `OTA_SIGNING_KEY_PEM`, then **`pbcopy < /dev/null`** — a master key left on
   the clipboard goes wherever you paste next.
2. **Regenerate the catalog and commit four files**, not three:
   ```sh
   python3 canary-local/tools/gen_flash.py
   git add firmware/common/ota/src/ota_release_key.h \
           firmware/projects/canary-wap/arduino/canary_wap/ota_release_key.h \
           firmware/projects/canary-display/arduino/canary_display/ota_release_key.h \
           canary-local/devices/flash.json
   ```
   `flash.json` carries the public key as `release_pubkey`, and CI regenerates
   the catalog and fails if what's committed differs. Headers without the catalog
   = red `canary-local`, and a flasher still on checksum-only verification.
3. **Press the master button.**

Secret *before* release: in between, a release fails with the less helpful
"public key does not match" rather than "secret is not set".

Record the **key id** the script prints — that's how you tell later which key
signed a given build.

### The flasher shows every product as unavailable

The catalog pins an **exact** release tag (`gen_flash.py`
`release_download_base()` — `/latest/` is unsafe because app releases share the
namespace and would shadow it). If that release hasn't been cut, the fetch 404s
and every product goes dark.

Both flashers now name the tag rather than shrugging — *"pinned to firmware
release fw-v2.3.0, which has no published images"*. `canary-local` CI also warns
on every run while the pin is unresolvable.

**Fix:** cut that firmware release (master button), or run **Flasher Factory
Images** for the tag if the key ceremony hasn't happened yet. If the tag itself
was never created — the usual case, and what makes this look like "nothing
works" rather than "one release is missing" — run **Flasher Factory Images**
with `channel: dev` instead: it needs no tag, and the images land on the channel
the flasher's **Advanced → dev channel** toggle reads.

### An app release that already exists

**Symptom.** You publish the Flasher and users see no update; a merged feature
never reaches an installer.

**Cause.** App workflows derive the tag from `tauri.conf.json`. If that version
is already released, publishing **rewrites the existing release's assets** rather
than cutting a new download — so nobody's updater sees anything new. Three
`desktop/` features sat unreleased behind `flasher-v0.2.1` this way.

**Fix.** Bump first. All three files, or CI fails the build:

| file | authoritative for |
|---|---|
| `src-tauri/tauri.conf.json` | the bundle version **and the release tag** |
| `package.json` | the npm package version |
| `src-tauri/Cargo.toml` | `CARGO_PKG_VERSION` — **the version the app shows the user** |

```sh
python3 desktop/scripts/check_app_versions.py    # all three, both apps
```

Bumping is half the act: the version must also **say what it changes**. Add a
`## <version> — <date>` section at the top of the app's `RELEASE_NOTES.md`
(`desktop/` or `desktop-lab/`) — it becomes the release body and the text the
in-app updater shows as "what's changing". `desktop/scripts/release_notes.py
check` (lint + both app workflows) fails the build if the newest section
doesn't match the version being shipped.

Cargo.toml is the one that bit us: the Flasher shipped as `0.2.2` with a footer
reading `v0.1.0`, so a bug report named a version that had never been released.
Remember `Cargo.lock` too — it pins the crate's own version and a `--locked`
build fails without it.

The master button won't make this mistake: same version + changed files means it
says **"bump the version first"** and ships everything else.

---

## Invariants that hold themselves up

You don't have to remember these; CI does. Listed so a red run makes sense.

| Invariant | Enforced by |
|---|---|
| `releases/latest` is always a `fw-v*` release — it's the URL every device polls | `.github/actions/keep-firmware-latest`, called by the app workflows + `release-latest-guard.yml` |
| Every OTA manifest the firmware polls is one the release publishes | `firmware/scripts/check_ota_channels.py` (Regression Guards) |
| Each app states one version across three files | `desktop/scripts/check_app_versions.py` (lint + both app workflows) |
| A display binary carries the identity it's published under | the product-string check in `firmware-release.yml` |
| The signing key in CI matches the committed public key | `firmware-release.yml`, fails before building |
| The flasher's pinned release actually resolves | advisory warning in `canary-local.yml` |
| Every updater URL in a published `latest.json` resolves | the consistency guards in `desktop-flasher-release.yml` + `desktop-lab-updater-pointer.yml` |
| An app release says what it changes, in the body and in the updater notes | `desktop/scripts/release_notes.py` (lint + both app workflows) |
| Each self-updating app polls its own rolling pointer (`flasher-latest`, `lab-latest`), never `releases/latest`, and the workflow that advances it names the same tag | `desktop_parity.test.js` |

## Things no button can do for you

- **The OTA key ceremony.** It makes your master signing key; it has to happen on
  your machine.
- **Apple signing.** `ENABLE_IOS_BUILD` / `ENABLE_TVOS_BUILD` plus the `APPLE_*`
  secrets need a developer account. Until then those targets are honest no-ops.
  **Which certificate signs which app, and which secret carries it, is in
  [`docs/APPLE_SIGNING.md`](APPLE_SIGNING.md)** — read it before touching an
  `APPLE_*` secret. Apple's two certificate types have near-identical names and
  the wrong one fails only *after* a full build; sharing one secret name between
  the iPhone and Mac pipelines cost five releases. For the Mac apps the whole
  setup is one command:
  `bash desktop/scripts/set-desktop-signing-secrets.sh`.
  Note `ENABLE_MACOS_SIGNING` must be exactly `true` or the desktop apps build
  **unsigned and still go green** — the release body states which you got.
- **Publishing the Lab's release.** It's created as a **draft** on purpose, so
  the build can be looked at before it goes public — but a draft reaches
  nobody. It has no git tag, its assets have no public URLs, and `lab-latest`
  (the pointer every installed Lab polls) does not move. **Finish it with
  Actions → "Publish the Lab (draft → live)"**, or click Publish in the UI;
  either way is fine, and the button is the one to reach for when nobody is
  around to click.
  Left unfinished it is silent, not loud: drafts for 0.1.1, 0.1.2, 0.2.0,
  0.2.1 and 0.2.2 all built green and all sat unpublished, so the
  self-updater shipped in 0.2.0 had never once delivered an update.
