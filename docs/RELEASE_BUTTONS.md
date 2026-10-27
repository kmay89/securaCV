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
| the Lab's newer version already built as a **draft** | 📝 reports it waiting — publish it with **Actions → "Publish the Lab (draft → live)"**; nothing is rebuilt (a draft is not re-checked against the tree, so if the Lab changed after it was built, `force: lab` rebuilds it first) |
| site pages changed since the last deploy | redeploys the site |
| Apple target with `ENABLE_*_BUILD` off | leaves it alone (the workflow would no-op) |
| firmware, but no OTA signing key | reports it gated, with the ceremony |

**Use it:** whenever you want the world to be current and don't want to think
about which targets moved (everything but the kernel, which has no row: see
[The kernel: not released yet](#the-kernel-not-released-yet) below). Pressing
it twice costs nothing the second time — that's the design.

**Don't:** reach for a narrower button first. This one is the only thing that
knows the whole graph.

**Safe by construction.** It never re-ships a different tree under a published
version, and it doesn't go red for telling you a version needs bumping — that's
information, not failure. Tick **`plan_only`** to see the plan and dispatch
nothing. The decision engine is unit-tested (`.github/scripts/test_release_plan.py`)
because this decision *is* the product.

**Narrowing it.** Two inputs replace the launchers this button retired:

- **`only`** limits the run to one target. `only: firmware` is the old
  "release firmware if it moved" check — same rule, same watched paths.
- **`force`** re-cuts the named targets whether or not they moved: one name,
  comma-separated names (`flasher,lab`), or `all`. With `publish` unchecked
  the forced apps get a **build-only smoke run**; with it ticked, a real
  re-publish. `force` does **not** narrow the run: every other target is still
  planned by its own rule — and the ones with no build-only mode (firmware, the
  site) **sit a build-only run out**, so a dev press can never cut a signed
  firmware release or deploy the site by accident; tick `publish` for those. A
  name the catalog does not know fails the run on its first line rather than
  being silently ignored. To touch two apps and nothing
  else, press twice with `only:` set (`only: flasher` + `force: flasher`, then
  `only: lab` + `force: lab`), or tick `plan_only` first and read the plan.
  Forcing a version that is **already tagged overwrites that release's assets
  instead of cutting a new download** — the plan row says so when it applies
  — so bump first unless a re-upload after a packaging fix is the point. See
  *An app release that already exists* below.

---

## When you want exactly one thing

### Firmware Release

Builds, signs, and publishes every board's firmware for one version. Pick
`channel` (release/dev) and `version`; it creates the tag so the tag and the
built commit can never disagree.

**Use it:** cutting firmware and nothing else, or a dev-channel build
(`2.4.0-dev.1`) that only dev-channel devices can see.

**It also redeploys the site.** The browser flasher on GitHub Pages reads the
release from the site's own copy of it (a page in a browser cannot fetch a
GitHub release asset; see
[The flasher shows every product as unavailable](#the-flasher-shows-every-product-as-unavailable)),
so the last step dispatches **pages** on the default branch, and the deploy
copies into the site the releases that branch's `flash.json` pins (the new
one, once the version bump naming it is merged there), plus `fw-dev-latest`.
A failed dispatch is a warning and a line in the run summary, not a failed
release: press **Actions → pages → Run workflow** to publish the copy by hand.

**Don't:** use it if you haven't bumped `FIRMWARE_VERSION` / `CANARY_FW_VERSION`
to match — the version-string guard greps each binary and fails closed. And it
refuses a tag that already exists: bump, don't reuse.

**If it fails on "product silently drops out of the flasher":** a board that
shipped in the previous release is missing from this one (its build step
degraded to a warning further up — the matrix is non-blocking by design, so
this gate is where the drop becomes a decision). Fix that board's build, or
re-run with `allow_dropped_products` naming the product id(s) to consciously
ship without them. On the tag-push path (which has no inputs), set the
`ALLOW_DROPPED_PRODUCTS` repository variable instead, re-run the failed job,
then clear the variable. Dev-channel builds only report; they never block on
this.

### Promote backoffice to production

Sends one build of the backoffice to the production app on the server. With
the `tag` field empty it asks the server which build staging runs and promotes
that; name a tag to promote (or roll back to) a specific build, the 12-char
commit `kit ps` shows. The server pulls it, starts it, waits for healthy, and
rolls back to the previous tag by itself if it is not; the workflow then proves
from outside that production answers `ok production <tag>` on `/healthz`.

**Use it:** every time a backoffice change has merged, landed on staging (that
happens by itself, within minutes) and you have looked at it there. This is
the only way production changes; merging alone never touches it.

**Don't:** promote a tag staging has not run, or `latest` (refused — name the
build). If the outside check fails, `kit ps` on the server says what runs and
`kit deploy backoffice <previous>` is the way back; the backoffice took a copy
of its data before any migration the new build applied, so a bad build is
undone in two steps (`backoffice/src/migrations/README.md`).

### Flasher Factory Images

Rebuilds *only* the browser-flasher factory images and `manifest-flash.json`, and
attaches them to a release. Pick a `channel`. Either channel ends by dispatching
**pages**, which re-copies into the site (what the hosted browser flasher
actually reads) the releases the default branch's `flash.json` pins, plus
`fw-dev-latest`; the **Vision Model Asset** workflow does the same after it
attaches a model. So an upload to one of those replaces the site's copy, and
a rebuild of an older tag that branch no longer pins never reaches the site.

**`channel: release`** — an **existing** `fw-v*` tag.

**Use it** in two situations:
1. A packaging-tooling fix should reach a tag nobody installs from yet, or one
   whose factory images are all wrong, without a new version (on a tag people
   already install from, read "Know what it replaces" below first).
2. **Before the OTA key exists.** The browser flasher's integrity rests on
   SHA-256 + same-origin, not the Ed25519 release key, so these assets can ship
   pre-ceremony. If `Firmware Release` hard-stops on the missing key, this lights
   the flasher up anyway.

**Don't:** expect it to help a tag that doesn't exist — it checks out the tag, so
the tag has to be there first. It now fails with that sentence rather than a raw
git error, and points you at `channel: dev`. Read the version-burn warning below
before creating a tag by hand.

**Know what it replaces on a published tag.** It cannot pick products: it
compiles the tag's firmware for every product in today's `flash.json` and
re-attaches every `*-factory.bin` plus `manifest-flash.json` under the same
names. The builds are not byte-reproducible (the images embed their compile
date through `__DATE__`), so every image gets a new SHA-256, the good ones
too, and the tag's `sha256sums.txt` and build-provenance attestation (written
by `Firmware Release`) go on describing the old bytes; this path attests
nothing, on purpose. The firmware source is the tag's, and the PlatformIO
builds keep the tag's platform and library pins; the build steps, the
arduino-cli cores, libraries and FQBNs, the PlatformIO CLI and esptool pins,
and the display env list are the dispatch ref's. A product whose rebuild fails
drops out of `manifest-flash.json` with only a `::warning::`: this path has no
completeness gate. The OTA manifests and app images are left alone. So for a
defect in one or two products' factory images on a release people already
install from (the fw-v2.4.17 classic-ESP32 bootloader offset), ship the fixed
images in the next version rather than re-cutting every image under the old
tag. Weigh one thing against that: the desktop Flasher and the Lab carry
`flash.json` compiled in, and it pins the tag's `manifest-flash.json`, so the
next version reaches their users only once they update the app, while a
rebuild in place reaches them at once, at the cost above. Until the next
version, a `channel: dev` dispatch from `main` publishes the fixed images to
`fw-dev-latest` (both flashers' **Advanced → dev channel**) and leaves the tag
alone.

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

**The website's download buttons follow the publish, not the build.**
securacv.com/download's buttons open exact releases (`app-v<x.y.z>`,
`flasher-v<x.y.z>`), stamped by `scripts/carry_to_site.py`'s `downloads`
carry from the newest release of each app that the GitHub API shows as
published (not a draft, not a prerelease) with its `.dmg`, `.AppImage` and
`.deb` uploaded. A version that is only in `tauri.conf.json`, only a draft,
or a Flasher release whose builds have not uploaded (or whose run failed
after creating it) never reaches them. To move them after you publish (the
Lab here, or a Flasher the master button cut):
`python3 scripts/carry_to_site.py --site <website-checkout> --only downloads`
(`--check` asks without writing, and exits 1 on a stale page). That command
is, for now, the only thing that moves them: the website's weekly carry job
runs every carry but commits only the paths in its `CARRY_PATHS`, which does
not list `download.html` yet, and its carry step sets no `GH_TOKEN`. A page
with no exact release link to move is left as it is, with the reason, and
the other carries still land.

### Fix a release's notes (rewrite the body from the repo)

Rewrites the **body** of one published `fw-v*`, `flasher-v*` or `app-v*`
release with the text its own release workflow would publish today:
`.github/scripts/release_body.py` runs that workflow's body step (the
firmware's CHANGELOG extraction and its 120,000-byte cap, the Flasher's and
the Lab's "says what this version changes"), so the words come from `main`'s
`CHANGELOG.md` and `RELEASE_NOTES.md`, not from this button. Its run summary
shows the diff. For an app release, `macos_signed` says whether *that*
release's Mac build was signed (today's signing switch says nothing about an
old build); `auto` reads it from the sentence the release workflow wrote from
that switch, and refuses a body from before Flasher 0.3.6, whose static
template claimed "signed & notarized" either way: say `yes` or `no` there.

**Use it:** after correcting a CHANGELOG or RELEASE_NOTES section that a
published release already carries. A published body does not follow its
sources. (Its first job, 2026-10-27: the five 2.4.16-train bodies the audit
corrected, sweep A115.)

**Don't:** expect it to touch anything else. It never changes an asset or a
tag, refuses a draft, and passes `--latest` explicitly on every edit (true only
for the tag that already is `releases/latest`), then fails the run unless
`releases/latest` is the same `fw-v*` tag as before and the asset list is
unchanged. The manifests a firmware release signed keep their 400-byte notes:
those are signed bytes, and rewriting them is a new release.

### The kernel: not released yet

The Rust kernel (the `witness-kernel` crate; `Cargo.toml` said `0.8.0` on
2026-10-14) **has never been released**: no `v*` tag and no `v*` release
exist, and `release.yml` has no recorded run (tags, releases and Actions API,
2026-10-14). There is no button for it, and the master button
does not ship it — `.github/release-targets.yml` has no kernel row, on purpose
(its header says why), so the plan never names the kernel and `only:` has no
kernel choice.

**How one would be cut.** Only by pushing a `v*` tag: `release.yml` listens
for that and nothing else (`contents: write`, `packages: write`). It runs the
crate's fmt, clippy and tests, builds the x86_64 Linux binaries, publishes them
as one tarball with `sha256sums.txt` and a provenance attestation, puts
`releases/latest` back on the firmware, and attaches the SBOMs. The tag *is*
the version: nothing checks it against `Cargo.toml`, and the release notes are
`CHANGELOG.md`'s `## [<version>]` section, or just "Release <version>" when
there is none, so add that section before you tag. The same tag also runs
`addon-image.yml` and `docker-sidecar.yml`, which listen for `v*` tags too.
Each rebuilds from the tagged commit and re-pushes its GHCR image as
`:<privacy_witness_kernel/config.yaml version>` and `:latest`, so tag a
commit that is current `main`, or both `:latest` tags move backwards and the
app's `:<version>` (the tag the Supervisor installs) is overwritten with
older bits.

**Don't:** read a moving `Cargo.toml` version as a kernel release. The version
is shared with the Home Assistant integration and app
(`scripts/bump_version.sh` writes all four places), and those ship by their
own routes: the HACS mirror, the app image (`addon-image.yml`) and the
Frigate sidecar image (`docker-sidecar.yml`), which publish from `main` and
again on any `v*` tag. The standalone `witnessd` image ships from `main` too
(`container-images.yml`, `:latest` and the commit sha, never a version tag).
Nor read `CHANGELOG.md`'s dated `## [0.1.0]` through `## [0.7.0]` headings as
kernel releases: they record what landed under each version number, and none
was ever tagged `v*` or published by `release.yml`.

**The follow-up, when a release is wanted** (sweep CI10, option (a)): give
`release.yml` a `workflow_dispatch` with a `version` input whose first step
creates and pushes `v${version}`, and make every other read of the ref follow
the input: checkout and the changelog step (today `GITHUB_REF_NAME`), the
`softprops/action-gh-release` step (it needs `tag_name: v${{ inputs.version }}`;
without one it uses the run's ref, a branch on a dispatch) and the `sbom`
job's `tag:` (today `${{ github.ref_name }}`). A tag the workflow pushes with
its own `GITHUB_TOKEN` starts no other workflow, so on that path
`addon-image.yml` and `docker-sidecar.yml` do not run. Then add a `kernel`
row to `release-targets.yml` (versioned; version file `Cargo.toml`; tag
prefix `v`; watching `src`, `Cargo.toml` and `Cargo.lock`), a
`test_release_plan.py` case, and `kernel` in `release-everything.yml`'s
`only` choices. A row added before that dispatch exists would plan the first
release on the next press with `publish` ticked and fail to start it:
`release.yml` cannot be dispatched today.

---

## Retired buttons, and what to press instead

Four dispatch-only launchers were deleted on 2026-09-08 after sixty days
unpressed. Three were one input combination of the master button, kept "for
when you know exactly what you want", and a second place every new target had
to be wired; the fourth had never produced a build. If muscle memory reaches
for one:

| You used to press | Press instead |
|---|---|
| **Release — one click (firmware + apps + web)** — a ticked set, unconditionally | the master button with **`force`** naming the targets (`flasher,lab`, `web`, or `all`). A dev-channel firmware build was never a smoke run: dispatch **Firmware Release** directly with `channel: dev` and the `-dev.N` version. |
| **Firmware Release — if changed** | the master button with **`only: firmware`** — it is the same rule, generalized (`.github/scripts/release_plan.py`) |
| **Build Mac apps (Flasher + Lab)** | the master button with **`force: flasher,lab`**; leave `publish` unchecked for the build-only smoke run, tick it to publish the Flasher and cut the Lab draft. Unlike the old button this does not stop at the two apps — the rest of the plan runs too (firmware and the site publish for real if they are ahead). For the two apps and nothing else, press twice with `only:` set, or tick `plan_only` first |
| **Mobile (iOS) build** — the Lab as a Tauri iOS shell | nothing: it never produced a build (one gated no-op run), and the iPhone + iPad app that ships is the native companion, through **iOS release** — the `ios` target row. The local recipe and the revive path (a target row, not a launcher) are in [`desktop-lab/MOBILE.md`](../desktop-lab/MOBILE.md). |

The one thing only the one-click launcher did — warn that publishing an
already-tagged app version overwrites the shipped release — now lives in the
plan itself: the forced target's summary row says it.

**The rule going forward:** a new thing to ship is a **row in
[`.github/release-targets.yml`](../.github/release-targets.yml)**, never a new
launcher. The per-target workflow keeps its build logic, signing and
concurrency guard; the button, the summary and the catalog tests pick the row
up with no YAML branch. (`.github/RELEASE_LESSONS.md`, 2026-09-08.)

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

**First: is it the hosted page and a "connection problem"?** Until 2026-10-27
the browser flasher on GitHub Pages fetched the release straight from
github.com, and no browser can: the `/releases/download/` redirect and the
asset host both answer without a CORS header, so every product read
unavailable on every visit. The release is now mirrored into the site by
`pages.yml` (`canary-local/firmware/<tag>/`), and the page reads only that
copy. If the hosted page says *this site's copy* of the pinned release isn't
available, open the newest **pages** run: its summary says, per channel,
whether the mirror landed or why it was skipped (the release not cut yet, an
asset missing, a checksum that disagrees with the manifest). Right after a
release it is expected until that redeploy lands: the release workflow
dispatches it as its last step. **Fix:** once the release exists, **Actions →
pages → Run workflow**. (The desktop apps fetch natively and never had this
problem.)

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

**An empty Flasher release counts as shipped too.** The Flasher's build job
creates its release, and with it the tag, *before* it builds (so the release is
never born as `releases/latest`). A run that fails after that leaves a published
release with no installers, and the master button reads its tag as released.
Re-run the failed jobs: the build finds the release and uploads into it. If the
fix needs a new commit, delete the release and its tag first
(`gh release delete flasher-v<version> --cleanup-tag`), then press again.

---

## Invariants that hold themselves up

You don't have to remember these; CI does. Listed so a red run makes sense.

| Invariant | Enforced by |
|---|---|
| `releases/latest` is always a `fw-v*` release — it's the URL every device polls | No app release is born `latest`: the Flasher creates its release with `--latest=false` before it builds and fails the run if `latest` is not `fw-v*` afterwards; the Lab builds a draft and publishes it with `--latest=false` (`.github/scripts/test_app_releases_never_latest.py` holds both). The repair, if anything moves it anyway: `.github/actions/keep-firmware-latest`, called by the app workflows + `release-latest-guard.yml` |
| Every OTA manifest the firmware polls is one the release publishes | `firmware/scripts/check_ota_channels.py` (Regression Guards) |
| Each app states one version across three files | `desktop/scripts/check_app_versions.py` (lint + both app workflows) |
| A display binary carries the identity it's published under | the product-string check in `firmware-release.yml` |
| The signing key in CI matches the committed public key | `firmware-release.yml`, fails before building |
| The flasher's pinned release actually resolves | advisory warning in `canary-local.yml` |
| The hosted flasher reads its release from its own origin, at the path the site deploy writes it | `canary-local/tools/mirror_release.py` (in `pages.yml`), held to `flash-core.js` by `scripts/tests/test_pages_release_mirror.py`; `canary-local/tests/flash_mirror_probe.mjs` in Chromium |
| Every updater URL in a published `latest.json` resolves | the consistency guards in `desktop-flasher-release.yml` + `desktop-lab-updater-pointer.yml` |
| An app release says what it changes, in the body and in the updater notes | `desktop/scripts/release_notes.py` (lint + both app workflows) |
| Each self-updating app polls its own rolling pointer (`flasher-latest`, `lab-latest`), never `releases/latest`, and the workflow that advances it names the same tag | `desktop_parity.test.js` |
| The committed firmware SBOM matches the build inputs, validates against CycloneDX 1.5, and its two Arduino core axes (workflow rows, `sketch.yaml` profiles) agree | `scripts/gen_firmware_sbom.py --check --validate` (lint.yml) + `--verify-with-pio` in `sbom.yml` |

## Things no button can do for you

- **The OTA key ceremony.** It makes your master signing key; it has to happen on
  your machine.
- **Promoting the CloudKit schema.** A new field on a synced record
  (`PairedDevice.tlsCertFP`, the TLS pin the iPhone app dials through)
  exists in DEVELOPMENT from its first write and is **rejected by
  PRODUCTION until promoted** — and `CloudSync.push` swallows that error by
  design, so an unpromoted release silently stops fleet sync for every user,
  with nothing in any log. CI cannot see this: `lint.yml`'s CloudKit step
  only proves the script's table matches the Swift sources.
  `ios/scripts/cloudkit_schema.sh check` exports both the development and
  the production schema and names which one lacks what, so a green `check`
  covers production. Run it before submitting an iPhone/iPad build for App
  Review (TestFlight builds read production too, so sooner is better); when
  production is short, the fix is `ios/scripts/cloudkit_schema.sh promote`,
  from a machine with the CloudKit CLI token (or, if cktool refuses the
  production import, CloudKit Console's Deploy Schema Changes, which is
  Apple's documented route), then `check` again. A red `check` is a release
  blocker, not a warning (`.github/RELEASE_LESSONS.md`, 2026-09-08).
- **Rebuilding the emulator dist after a firmware VERSION bump.** A bump
  moves `canary-local/emulator/dist/` (the stamp is compiled in and written
  to each `meta.json`), and the toolchain that rebuilds it is pinned to an
  emsdk most machines cannot install. Before the master button: dispatch
  **Actions → "Rebuild emulator dist (pinned emsdk)"** on the branch, pull
  its commit, then regenerate `flash.json` (`gen_flash.py` embeds the
  artifact stamps — run it *after* the rebuild, or the catalog goes stale
  in the same push). The order and the two symptoms of getting it wrong are
  in `CLAUDE.md`, "Generated files". The same bump moves the committed
  firmware SBOM, which carries `FIRMWARE_VERSION`: run
  `python3 scripts/gen_firmware_sbom.py` and commit
  `sbom/sbom-firmware.cdx.json` with it, or `lint.yml`'s `--check` fails.
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
- **Minting `MIRROR_PAT`.** HACS users install the integration from the mirror
  repository `kmay89/securacv-homeassistant`, and
  `.github/workflows/homeassistant-mirror.yml` pushes every main change under
  `custom_components/securacv/` there as a PR — but only with a fine-grained
  personal access token for that one repository (Contents, Pull requests and
  Issues: read and write) in the `MIRROR_PAT` secret; the default token cannot
  reach another repository. Without it the run stays green and raises one
  issue per gap (*"HACS mirror not refreshed"*), and the mirror's weekly
  freshness check keeps reporting the drift. The token is a human's to make;
  the workflow header says exactly what to scope it to.
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
