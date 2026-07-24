# Release & packaging lessons — every app target

Read this before touching **any** app build/release workflow: the SecuraCV
Flasher, the Lab, and the mobile/TV targets (**iPhone, iPad, tvOS, Mac**).
It applies to every `*-release.yml` and the one-click launcher. Each entry is
a real failure we paid for once — the point is to never pay for it twice, on
any platform.

> **For AI agents:** this file is the canonical home for build/release lessons.
> It is pointed to from `AGENTS.md` and `CLAUDE.md` so it surfaces each session.
> When a release/packaging bug is fixed, **append a dated entry here** in the
> same shape (symptom → cause → fix → applies-to), and generalize it to the
> other app targets rather than fixing only the one that broke.

## Principles (hold these on every app target)

1. **Dereference symlinks when copying a payload into a bundle.** Use
   `cp -RL` (or `cp -a --dereference`, `rsync -L`, `ditto` follows by
   default). App bundlers (Tauri, Xcode resource copy, electron-builder)
   reject a dangling link with a confusing "resource … doesn't exist" even
   though the copy step "succeeded". Upstreams love shipping symlinks that
   point *outside* the directory you copy.
2. **Pin or log every upstream ref.** An empty ref (e.g. `USBBOOT_REF=""`)
   tracks a moving default branch and will break you with no change on your
   side. Always print the resolved commit SHA in the build log; replace the
   empty ref with the exact tag/SHA once it's validated on real hardware.
3. **Prove the bundle before you publish.** Every app workflow has a
   build-only path — run it first, publish only after it's green:
   - `desktop-flasher-release.yml` → `dry_run: true` (build only)
   - `desktop-release.yml` (Lab), `ios-release.yml`, `tvos-release.yml`,
     `desktop-mobile-release.yml` → `publish: false` (build only)
4. **Verify a bundled resource exists in the copy step**, so the failure is
   a clear line in *that* step, not an opaque bundler abort 3 minutes later:
   `test -s "$res/bootfiles.bin" || { echo "::error::payload missing"; exit 1; }`
5. **One button for the whole pipeline.** `release-one-click.yml`
   ("Release — one click (firmware + apps + web)") fans out to the **firmware
   release** (the OTA `.bin` images *and* the browser-flasher factory images
   + `manifest-flash.json`), the desktop app builds, and the GitHub Pages web
   deploy. `firmware` = none / dev / release (opt-in — a firmware release is
   always a publish); the apps' `publish` off = dev smoke run, on = real
   releases; `deploy_web` redeploys the site. Prefer it over triggering the
   per-target workflows by hand. `firmware-release.yml` also takes the same
   `channel` + `version` inputs directly (Actions → "Firmware Release"),
   which is what the launcher dispatches.
6. **A wired pipeline is not a working pipeline — build the DEV half too.**
   A release workflow that is gated behind an account nobody has yet (an
   Apple Developer program, a signing secret) verifies *nothing* until the day
   someone enables it — which is the worst possible day to discover it never
   worked. Every app target therefore gets a second, ungated workflow that
   exercises everything not requiring permission: compile the app, run its
   unit tests, on a simulator, with `CODE_SIGNING_ALLOWED=NO`. Today: `tvos.yml`
   (dev) alongside `tvos-release.yml` (prod). The gate belongs on the *upload*,
   never on the *build*.
7. **One button that only does what's needed.** `release-everything.yml`
   ("Update everything (only what needs it)") compares every target's source
   version against what's actually been tagged and dispatches only what is
   genuinely ahead — apps, firmware, and the site. Targets are declared in
   `.github/release-targets.yml`; the decision engine
   (`.github/scripts/release_plan.py`) is unit-tested, and the suite also
   validates the catalog against the repo, so a renamed workflow or moved
   version file fails CI instead of failing a release. Prefer it over pressing
   per-target buttons and guessing what moved. `release-one-click.yml` remains
   the unconditional "run these now" launcher for when you know exactly what
   you want.
8. **A new board reaches the flasher only when a release carries it.** The
   in-browser flasher lights a product up from `manifest-flash.json` in the
   release it reads (`releases/latest`, or `fw-dev-latest` via
   `?channel=dev`). Adding a board to `flash.json` + the release workflows is
   necessary but *not sufficient* — the product stays "unavailable" until the
   **next firmware release is actually cut**. After adding a board, cut a
   release (one-click above) or it will never appear, no matter how correct
   the wiring is.

## Entries

### 2026-07-24 — A required store asset was missing, and the obvious gate for it would have failed for the wrong reason

- **Symptom:** `tvos/WitnessWall/project.yml` set
  `ASSETCATALOG_COMPILER_APPICON_NAME` to an asset catalog that did not exist.
  The simulator build passed (icons aren't required there), so CI was green —
  but `altool --validate-app` rejects a tvOS archive with no app icon or
  top-shelf image, so the release path could never have shipped. Caught in
  review, not by any check.
- **Cause:** tvOS icons are not one PNG. The App Icon is a **layered image
  stack** (Back/Middle/Front, parallaxed by the focus engine) in two sizes,
  plus two top-shelf images at @1x and @2x — ~10 images and a tree of
  `Contents.json`. Nothing verified they existed, and the only build that would
  have noticed was the one gated behind an Apple account nobody had yet.
- **Fix:** generate them (`tvos/scripts/make_app_icon.py`) and commit them —
  the same generated-and-committed contract the website uses for its glTF
  models. `release-tvos.sh` fails early and by name if the catalog is missing,
  and `tvos/scripts/check_app_icon.py` runs in PR CI.
- **The part worth generalizing:** that CI check is **structural, not a
  byte-diff against a regenerated copy**. The icon composites a PNG through
  Pillow, whose resampling output can differ between versions — so a byte-diff
  would go red because a runner image bumped a dependency, with nothing wrong
  in the change under test. A gate that fails for reasons unrelated to the diff
  trains people to ignore it, which is worse than not having it. Assert the
  property that actually matters (every required image exists, at the exact
  size the store demands) and leave incidental bytes alone. Byte-exactness is
  the right check only where the generator is genuinely deterministic and
  dependency-free — which is why the website's `.glb` models *do* get one.
- **Applies to:** every store asset on every platform (Apple icons, Samsung
  `.wgt` icons, Play Store feature graphics). If a store requires an asset,
  something in CI must assert it exists at the required size — the build
  succeeding is not evidence, because the build that checks it is usually the
  one you can't run yet.

### 2026-07-24 — The tvOS pipeline was fully wired to an app that did not exist; every gate it had was green

- **Symptom:** `tvos-release.yml` existed, passed `ci_policy_check.py`, and was
  documented end to end in `docs/tvos/AUTOPIPELINE.md` — yet it could not have
  produced a build. `tvos/WitnessWall/` (the Xcode project) and
  `tvos/witness-core/` (the Rust core) were never written; both scripts were
  honest stubs whose entire body was "this doesn't exist yet, exit 1". Nothing
  in CI was red, because the workflow's `ENABLE_TVOS_BUILD` gate made every run
  a green no-op. The failure was scheduled for the first day someone set the
  variable — i.e. the day of the first real release.
- **Cause:** the gate was on the **whole workflow** instead of on the **upload**.
  Everything that needs no Apple account — compiling the Rust core, compiling
  the SwiftUI app, running unit tests on the simulator — was gated behind a
  paid developer account along with the parts that genuinely need one. A gate
  that skips the build is indistinguishable from a build that passes, and
  "wired and policy-clean" got mistaken for "works".
- **Fix:** built the app and core for real (`tvos/witness-core` — the chain math,
  pinned to the kernel's OWN `domain_separation_vectors.json` so the TV can
  never verify a different chain than the source of truth — and
  `tvos/WitnessWall`, a SwiftUI app generated by XcodeGen like `ios/`), then
  split the pipeline in two: **`tvos.yml` (dev)** builds and unit-tests on the
  tvOS simulator with `CODE_SIGNING_ALLOWED=NO` on every PR, ungated;
  **`tvos-release.yml` (prod)** keeps the `ENABLE_TVOS_BUILD` gate but now only
  gates signing and upload, gained a `publish: false` build-only path
  (Principle 3), a version check so a `tvos-v*` tag and `MARKETING_VERSION`
  cannot disagree, `altool --validate-app` before uploading, and a retrying
  upload (the 2026-07-24 5xx lesson below).
- **Applies to:** every gated target, present and future — the iOS build has
  the identical shape (`ENABLE_IOS_BUILD`). If a workflow is skipped by default,
  ask what is verifying the code it would have built. Gate the credential, not
  the compiler. And when a doc describes a pipeline in the present tense, check
  that the files it names exist.

### 2026-07-24 — New boards wired into the flasher but never cut into a release → invisible in the flasher; firmware releases were tag-only and not one-click

- **Symptom:** the three Canary Display flavors (watch / dash / dash-modes)
  were fully wired — in `flash.json`, in `firmware-release.yml`,
  `flasher-release.yml`, and `build_flash_manifest.py`, with green gates — yet
  never appeared in the in-browser flasher. Every product showed as
  "unavailable".
- **Cause:** two compounding gaps. (1) The only published release
  (`fw-v2.2.0`) predated the display boards, and `manifest-flash.json` is
  built from the catalog *as of the tagged commit* — so the live release
  simply didn't contain them. (2) Cutting a new firmware release was a
  local-only `git tag && git push` ceremony: the one-click launcher
  (`release-one-click.yml`) shipped the apps + web but **not** the firmware,
  so there was no low-friction way to publish the release that would surface
  the boards. Correct wiring + no release = invisible.
- **Fix:** made `firmware-release.yml` dispatchable (Actions → "Firmware
  Release", `channel` release/dev + `version`, deriving and creating the tag
  so tag and source can't disagree; existing tag-push path unchanged), folded
  the firmware release into `release-one-click.yml` as a first-class opt-in
  target, and made `flasher-release.yml` default a blank tag to
  `releases/latest`. Documented the one-click ceremony in
  `docs/RELEASE_PROCESS.md` and added Principle 6 above.
- **Applies to:** every future board/flavor. Wiring a product into the
  catalog and workflows is necessary but not sufficient — it is invisible in
  the flasher until the next release is cut. Cut one (one-click) as the final
  step of adding a board.

### 2026-07-24 — Bundled resource copied from an external checkout was a symlink → dangling link → build aborted

- **Symptom:** every "Desktop Flasher — build & release" run failed (macOS
  **and** Linux) at the Tauri build step — even though the "Bundle rpiboot
  sidecar + gadget" step reported success:

  ```
  resource path `resources/mass-storage-gadget64/bootfiles.bin` doesn't exist
  Command "npm run tauri build" failed with exit code 1
  ```
- **Cause:** upstream `raspberrypi/usbboot` ships
  `mass-storage-gadget64/bootfiles.bin` as a **symlink** to
  `../firmware/bootfiles.bin`. The job copied the gadget payload with
  `cp -R`, which preserves the symlink but not its target (it lives *outside*
  the copied subtree) → a dangling link that Tauri's resource bundler
  rejects. `USBBOOT_REF` being empty (tracks upstream's default branch) meant
  an upstream change broke us with no change on our side.
- **Fix:** copy with `cp -RL` in both the macOS and Linux gadget steps so the
  symlink is dereferenced into the real ~1.5 MB `bootfiles.bin` inside the
  bundled resource directory. (PR #1188.)
- **Applies to:** any target that bundles a vendored payload copied from a
  cloned upstream. Today only the Flasher does; if the Lab / iOS / iPad /
  tvOS / Mac builds ever vendor an external payload into their app bundle,
  dereference on copy (Principle 1) and verify it exists (Principle 4).

### 2026-07-24 — Hub catalog shipped unpinned → flasher downloads the OS image, then dead-ends at 100% with nothing to verify against

- **Symptom:** on real hardware, the Raspberry Pi hub flow downloaded the HAOS
  image to 100%, then errored ("didn't download properly") and never reached
  the write step. Reproduced on every attempt, Pi 5.
- **Cause:** the hub-image catalog (`hub_image.json`) was **unpinned**
  (`pinned:false`, empty `sha256`), and the whole verification design assumed
  Home Assistant publishes a `<image>.sha256` sibling to fall back on. It does
  **not** — that URL 404s, and there's no combined `SHA256SUMS` either. So
  after a clean download the writer had neither a repo pin nor a published
  checksum, hit `VerifyError::NoChecksumAvailable`, and refused to write (it
  will not flash an unverifiable OS image). The pin ceremony *also* depended on
  that dead `.sha256` channel as one of its two required sources, so it
  silently pinned nothing — the catalog stayed unpinned and no CI check
  noticed, because CI never downloads the real image.
- **Fix:** re-point the pin ceremony's authoritative channel at the **actual
  downloaded bytes** (stream + hash), cross-checked against GitHub's asset
  digest when present (best-effort, alarms on disagreement) — HA's own bytes
  are the only checksum that exists. Committed real pins for HAOS 18.1
  (`hub_image_pins.json`) and regenerated the catalog (`pinned:true`). Added a
  release guard in `desktop-flasher-release.yml` that fails the build if the
  shipped catalog isn't pinned, so a broken flasher can never reach a user
  (Principle 4 — prove it before you publish).
- **Applies to:** any app that bundles a catalog whose entries are verified at
  runtime. The runtime refuses to act on an unverifiable entry (correct), so
  the *build* must prove the catalog carries what the runtime needs. Never
  assume an upstream publishes a checksum you can fetch — if trust hinges on a
  hash, mint it from the bytes yourself and pin it in-repo.

### 2026-07-24 — A transient GitHub upload 5xx left the release with a stale updater manifest (self-update broken; installs fine)

- **Symptom:** a `flasher-v0.2.1` publish went red on the Linux job at the
  *last* asset upload (`...AppImage.sig`) with `##[error]No server is currently
  available to service your request.` The `.dmg`/`.AppImage`/`.deb` were all
  uploaded and installable, but `latest.json` pointed at **deleted asset ids**
  and carried a **stale signature** — so the in-app updater would fail while
  fresh installs worked.
- **Cause:** two compounding fragilities. (1) tauri-action has no upload retry,
  so one transient 5xx fails the whole publish. (2) It writes each platform's
  `url` as an **asset-id** link (`.../releases/assets/<id>`); those ids change
  on every re-upload, and in a matrix build each job writes `latest.json` and
  relies on merge-with-existing — so a re-run or a half-failed job leaves the
  manifest pointing at ids that no longer exist, with a signature from a
  different build. Nothing failed loudly; the release just quietly couldn't
  self-update.
- **Fix:** three layers, all on the publish path only. (a) A per-job
  **"Reconcile release assets"** step re-uploads any missing installer/`.sig`
  with backoff, so a transient 5xx self-heals within the run. (b) A single
  **`finalize` job** (runs once, after both builds) rewrites `latest.json` via
  `desktop/scripts/harden_updater_manifest.py` to use **stable name-based
  download URLs** (immune to asset-id churn) with **signatures read from the
  `.sig` assets the release actually carries** (can't drift from the bytes).
  (c) A **consistency guard** in that job fails the release loudly unless every
  updater URL resolves — an inconsistent manifest can never ship silently.
- **Applies to:** any app target that publishes a Tauri/self-update manifest
  from a **matrix** build (Flasher today; the Lab / iOS / iPad / tvOS / Mac if
  they gain in-app updates). Treat the updater manifest as a single-writer,
  post-build artifact with stable URLs and asset-sourced signatures — never as
  something each matrix job races to write with churning asset-id links. And
  give every release upload a retry (Principle 3 — prove the bundle, and here,
  prove the *manifest*, before it counts as published).
