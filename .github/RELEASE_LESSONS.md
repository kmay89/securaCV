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
6. **`releases/latest` belongs to the firmware — the apps must never take it.**
   Every Canary polls `releases/latest/download/manifest-<product>.json`
   (each project's `config.h` `SECURACV_OTA_MANIFEST_URL`), and GitHub's
   "latest" is the newest published release of **any** kind. The desktop apps
   ship from the same release namespace, so publishing `flasher-v*` / `app-v*`
   silently re-points the whole fleet's update URL at a release with no
   manifests in it. `.github/actions/keep-firmware-latest` enforces it from
   both sides — the app workflows call it (CI-created releases don't fire
   `release:` events), and `release-latest-guard.yml` catches every
   human-created one.
7. **A compiled-in URL is a promise the release has to keep.** If firmware
   polls a manifest, some workflow must *sign* that manifest — otherwise the
   product is flashable and structurally unable to update, and says nothing
   about it. `firmware/scripts/check_ota_channels.py` proves the two sides
   match, statically, on every PR. A flavor with no channel yet is declared in
   that script with a reason; silence is not an option.
8. **An app release version that already exists is not a release.** The app
   workflows derive the tag from `tauri.conf.json`, so publishing without
   bumping rewrites the assets of a shipped release instead of cutting a new
   download. Bump first, then publish.
9. **A wired pipeline is not a working pipeline — build the DEV half too.**
   A release workflow that is gated behind an account nobody has yet (an
   Apple Developer program, a signing secret) verifies *nothing* until the day
   someone enables it — which is the worst possible day to discover it never
   worked. Every app target therefore gets a second, ungated workflow that
   exercises everything not requiring permission: compile the app, run its
   unit tests, on a simulator, with `CODE_SIGNING_ALLOWED=NO`. Today: `tvos.yml`
   (dev) alongside `tvos-release.yml` (prod). The gate belongs on the *upload*,
   never on the *build*.
10. **One button that only does what's needed.** `release-everything.yml`
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
11. **A new board reaches the flasher when a release carries it.** The
   in-browser flasher lights a product up from `manifest-flash.json` in the
   release it reads (`releases/latest`, or `fw-dev-latest` via
   `?channel=dev`). Adding a board to `flash.json` + the release workflows is
   necessary but *not sufficient* — the product stays "unavailable" until the
   **next firmware release is actually cut**. After adding a board, cut a
   release (one-click above) or it will never appear, no matter how correct
   the wiring is.
12. **A button must know its own preconditions.** "Release the firmware" with no
   signing key isn't a release, it's a 20-second failure in a different run with
   the real consequence three inferences away. Preconditions the repo can check
   belong in the plan, stated before anything is dispatched —
   `release-targets.yml` carries `gate_var` + `gate_reason` for exactly this, and
   `ota_key_state.py` answers the key question without needing the key.
13. **The same fact in N files is N-1 chances to be wrong.** Each app states its
   version three times (`tauri.conf.json` names the tag, `Cargo.toml` is what the
   app tells the *user*) — and a guard covering two of the three is worse than
   none, because it reads as covered. If a value must repeat, one script holds
   them together: `desktop/scripts/check_app_versions.py`.
14. **The two flashers do not share a frontend.** `canary-local/assets/` (browser)
   and `desktop/src/` (desktop app) are separate UIs over the same catalog. A
   user-facing diagnostic added to one leaves the other vague — fix both, or
   half your users get the unhelpful version.

## Entries

### 2026-07-25 — The whole pipeline was blocked on one missing key, and every button's answer to that was silence

- **Symptom:** a maintainer opened the Flasher and every product read **"no
  published release yet"**. Nothing was broken: the catalog pins
  `fw-v<train>`, no `fw-v2.3.0` release existed, so the fetch 404'd. Underneath
  that, `firmware-release.yml` had failed once, 20 seconds in, with
  `OTA_SIGNING_KEY_PEM secret is not set` — and nothing anywhere connected those
  three facts. The one-click button would have cheerfully dispatched the same
  doomed run again; "Update everything" would have reported *releasing firmware*
  while nothing shipped. The app's own footer said `v0.1.0` for a build released
  as `0.2.2`, so even reporting the problem named a version that never existed.
- **Cause:** every individual piece was correct and none of them talked. The key
  guard fails in the *right* place but only after you press a button in a
  different workflow; the flasher's "no release yet" copy can't tell "never
  shipped" from "pinned to a tag nobody cut"; the version drift was invisible
  because the guard checked two of three files and skipped the one users read;
  and the ceremony script's own instructions omitted `flash.json`, so following
  them exactly turned `canary-local` red on the next push.
- **Fix:** make the preconditions part of the plan, and make every dead end name
  itself.
  - `firmware/scripts/ota_key_state.py` answers "is the signing key ready?" from
    the committed public header alone — no secret needed, so any button, and any
    human, can ask.
  - `release-targets.yml` gained `gate_var: OTA_SIGNING_KEY_READY` +
    `gate_reason`, so the master button reports firmware as **gated with the
    ceremony inline** and still ships everything else. `gate_reason` exists
    because the default gate text describes a repo *variable*, which would have
    sent someone hunting a flag that was never the problem. Four new unit tests.
  - `release-one-click.yml` **refuses** a firmware dispatch without the key
    (it means "do exactly this", so skipping would be wrong), and warns when a
    publish would overwrite an already-released app version.
  - `desktop/scripts/check_app_versions.py` holds all three version files
    together, for both apps, on every PR.
  - Both flashers now name the pinned tag instead of shrugging.
  - `setup_release_key.sh` prints the *complete* commit list (including the
    regenerated `flash.json`), tells you to clear the clipboard after pasting a
    master key, and explains the PEP 668 venv escape when `pip install
    cryptography` is refused by a Homebrew Python — which is what actually
    happened.
  - `docs/RELEASE_BUTTONS.md`: one page saying which button, when, and when not.
- **Applies to:** every button that fans out to another workflow. A launcher that
  can't state why a target won't work is a launcher that turns a five-minute
  ceremony into an afternoon of archaeology. Check the preconditions you *can*
  check, before dispatching, and put the remediation in the message — not in a
  doc the reader doesn't know exists yet.

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
### 2026-07-24 — Publishing an app without bumping its version rewrote a shipped release instead of cutting a new one → three merged features never reached an installer

- **Symptom:** `flasher-v0.2.1` was published, complete and healthy, yet the
  embedded Witness Wall (#1211), post-flash LAN discovery (#1216), and the
  release hardening (#1219) were all on `main` and in none of its bytes.
  Re-running "Desktop Flasher — build & release" with `dry_run: false` would
  not have helped: it derives the tag from `tauri.conf.json`, so it re-uploads
  over `flasher-v0.2.1` — same release, new assets, no new download for anyone
  who already has it, and an existing installed base that sees no update.
- **Cause:** the version bump and the publish are separate acts, and only the
  bump decides whether a publish is a *release*. `desktop/package.json` had
  also drifted to `0.1.3` against `tauri.conf.json`'s `0.2.1`, so the source
  tree stated the app's version two different ways and neither was obviously
  authoritative.
- **Fix:** bumped to `0.2.2` and published that (its release carries all three).
  Added a drift guard to the workflow's plan step: `tauri.conf.json` and
  `package.json` must agree or the build fails with both values named, before
  anything is built. Principle 8 above states the rule.
- **Applies to:** the Lab and every future app target with a version-derived
  tag. Before pressing publish, ask what tag the workflow will compute and
  whether that release already exists — if it does, you are editing history,
  not shipping.

### 2026-07-24 — Desktop app releases were quietly taking `releases/latest`, the URL the whole fleet polls for OTA

- **Symptom:** none visible, which is the point. After publishing
  `flasher-v0.2.2`, `GET /repos/kmay89/securaCV/releases/latest` returned the
  **Flasher app release** — so every device's compiled-in manifest URL
  (`releases/latest/download/manifest-<product>.json`) resolved to a release
  containing installers and no manifests. Devices 404 on their next check and
  stop seeing updates. No workflow fails, no log says anything, and the only
  way to notice is to ask that endpoint by hand.
- **Cause:** firmware and apps share one release namespace, and GitHub's
  "latest" is the newest published non-prerelease of any kind — so the most
  recent *app* release always wins it. `gen_flash.py` already knew this and
  dodged it for the browser flasher by pinning its catalog to an exact
  `fw-v<train>` tag (see `release_download_base()`), but **devices can't
  dodge it**: their URL is compiled in, and changing it means shipping
  firmware, which needs the very channel that is broken. Latent so far only
  because the zero OTA key hard-disables installs — the day the key ceremony
  lands, the next app release would break the fleet.
- **Fix:** `.github/actions/keep-firmware-latest` (one implementation, two
  callers) asserts `releases/latest` is a `fw-v*` release and re-points it at
  the newest published firmware release when an app has taken it. The app
  release workflows call it directly, because releases created with
  `GITHUB_TOKEN` do **not** fire `release:` events (GitHub's recursion guard),
  so a workflow-only trigger would miss exactly the case that breaks the fleet.
  `release-latest-guard.yml` covers everything a human does — publishing a
  draft, creating a release in the UI. It re-points the firmware release
  explicitly rather than setting the app release to `make_latest:false`, since
  that only makes GitHub recompute by date and can land on another app release.
  With no firmware release published yet it warns instead of failing.
- **Applies to:** every repo that ships device firmware and host apps from one
  releases page — today the Flasher and the Lab, tomorrow the iPhone / iPad /
  tvOS / Mac targets if they ever publish here. Treat `releases/latest` as
  owned by whatever the *fielded hardware* reads, and enforce it from outside
  the workflow that publishes, because the publisher can't see the collision.

### 2026-07-24 — Firmware polled display OTA manifests that no release has ever signed → flashable boards that can never update, silently

- **Symptom:** the three Canary Display flavors were fully wired for USB
  flashing — `flash.json`, `build_flash_manifest.py`, factory images in both
  release workflows — and each flavor's `config.h` pointed
  `SECURACV_OTA_MANIFEST_URL` at `manifest-canary-display-<flavor>.json`. No
  workflow anywhere wrote such a file. `manifest-index.json` listed 7 products;
  the displays were not among them. A display board therefore fetches a 404 on
  every OTA check, forever, and reports nothing wrong.
- **Cause:** the display build steps were added to `firmware-release.yml` for
  the *browser flasher* (they feed `build_flash_manifest.py`'s factory images),
  and that felt like "displays are in the release". The signing half was never
  written. Nothing connected the URL the firmware *polls* to the manifests the
  release *publishes*, so the two could disagree indefinitely — and the only
  observable difference is a device that never updates.
- **Fix:** `firmware-release.yml` now signs, verifies, and indexes
  `manifest-canary-display-{watch,dash,dash-modes}.json` from the flavors CI
  builds. Best-effort like the display builds themselves: a flavor that didn't
  compile, or whose binary doesn't carry this release's version, is left out
  with a `::warning::` rather than sinking the signed release or publishing a
  manifest that would make devices install, boot reporting the old version,
  witness a rollback, and be re-offered the same update forever. The durable
  half is `firmware/scripts/check_ota_channels.py` (in "Regression Guards"):
  it parses every `SECURACV_OTA_MANIFEST_URL` in the tree and every manifest
  the release's variant index publishes, and fails when a polled manifest is
  neither published nor declared unpublished-with-a-reason. Verified it fails
  by removing flavors from the release loop and watching it go red.
- **Applies to:** every product that gains an OTA URL — the display flavors
  with no profile yet (`watch-modes`, `nightstand`, bare `canary-display`) are
  now *declared* rather than forgotten. Generally: a compiled-in URL is a
  contract with the release, and a contract nobody checks is a wish. Check it
  statically, on the PR that adds the flavor, not on a release nobody inspects.

### 2026-07-24 — The flasher catalog was pinned to a firmware release that had never been cut → every product read "unavailable", with no way to tell why

- **Symptom:** `flash.html` showed every product as unavailable. The previous
  entry below blamed the release predating the boards; the live cause on
  2026-07-24 was simpler and separate — `flash.json`'s `manifest_url` pointed
  at `fw-v2.3.0`, a tag that did not exist, so the page fetched a 404 and hid
  everything. The banner said "No signed firmware release is published yet",
  which is indistinguishable from a repo that has never cut one.
- **Cause:** `fw_train` was bumped to 2.3.0 (correctly — the headers moved) and
  `gen_flash.py` regenerated the pin to match, but the release itself was
  blocked on the missing OTA signing key. The bump-then-release window is a
  legitimate state; what was missing was any *signal* while it lasted, and any
  way for a user or maintainer to tell "pinned to a release that isn't cut"
  apart from "nothing has ever shipped".
- **Fix:** two halves, neither of which blocks the legitimate window.
  `canary-local.yml` warns on every run (title + run summary) while the pinned
  `manifest_url` doesn't resolve, naming the tag and the HTTP code — advisory
  because the site is much more than the flasher, but impossible to miss. And
  the page itself now names the pin: `releaseTagFromManifestUrl()` (pure,
  host-tested) turns the dead end into "this page is pinned to firmware release
  fw-v2.3.0 — no release manifest (HTTP 404)".
- **Applies to:** any client pinned to an exact release asset. Pinning is right
  (see the `releases/latest` entry above — `/latest/` is unsafe in a shared
  namespace), but a pin is a claim about something outside the repo, so
  something must notice when the claim stops being true, and the failure must
  name the claim rather than shrug.

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

### 2026-07-25 — The Flasher shipped unsigned while the Lab had the signing scaffold; and a loadable dylib in Resources/ is a notarization trap

- **Symptom:** the macOS Flasher had no Apple code-signing or notarization at
  all — `desktop-flasher-release.yml` passed only the Tauri *updater* keys
  (`TAURI_SIGNING_*`) to tauri-action, no `APPLE_*` — so every build shipped
  unsigned and users had to `xattr -dr com.apple.quarantine` on first launch.
  The Lab (`desktop-release.yml`) already had the opt-in `ENABLE_MACOS_SIGNING`
  scaffold; the Flasher — the app people actually run to flash hardware — never
  got it.
- **Cause:** the signing scaffold was added to one target and not generalized to
  the other (the exact anti-pattern this file exists to stop). Separately, the
  Flasher bundles a loadable `libusb-1.0.0.dylib` (the rpiboot sidecar needs it),
  declared under `bundle.resources` → copied into `Contents/Resources/`. Tauri
  signs the app, the `externalBin` sidecars, and frameworks — but **not** an
  arbitrary dylib in Resources. Notarization rejects any unsigned nested Mach-O,
  so the moment signing was switched on the Flasher would have failed
  notarization on libusb, with a confusing per-binary error.
- **Fix:** gave the Flasher the same opt-in signed/unsigned split as the Lab
  (`ENABLE_MACOS_SIGNING` var + the six `APPLE_*` secrets, honoring the
  "define no `APPLE_*` keys when off" gotcha), and moved libusb from
  `bundle.resources` to **`bundle.macOS.frameworks`** so Tauri copies it to
  `Contents/Frameworks/` **and code-signs it** with the app's Developer ID under
  the hardened runtime; the rpiboot `install_name_tool` re-point now targets
  `@executable_path/../Frameworks/libusb-1.0.0.dylib`. Runbook: `desktop/SIGNING.md`.
  The switch stays OFF until a `dry_run:true` build proves notarization passes
  (Principle 3 / 9 — the credential gates the *upload*, not the *build*, and a
  dry-run still signs + notarizes locally in CI).
- **Applies to:** every app target that bundles a third-party dylib or helper
  (the Lab if it ever gains one; iPhone / iPad / tvOS / Mac). A loadable Mach-O
  must live somewhere the bundler signs it — `Frameworks/` (or an `externalBin`
  sidecar), never `Resources/` — or notarization fails on it. And when you add a
  signing/release capability to one target, generalize it to the siblings the
  same day: a scaffold on one app and not the app users actually run is a gap
  waiting for the worst day to surface.

### 2026-07-25 — The desktop Flasher's help copy promised "Install a local file under Advanced" — a feature only the browser flasher had

- **Symptom:** three separate desktop error paths (no published release, dev
  interest, air-gapped setup) told the user to "install a local file under
  Advanced". The desktop app had no Advanced local-file UI and no Tauri
  command behind it — the copy had been ported from the browser flasher
  (which really has the feature) without the feature itself. A user following
  the app's own advice hit a dead end the app could not see.
- **Cause:** Principle 14's failure mode in mirror image. The two flashers
  share a catalog but no frontend; a diagnostic *message* was kept in parity
  while the *capability* it references was not. Copy parity without feature
  parity is worse than divergence — it turns honest advice on one frontend
  into a false promise on the other.
- **Fix:** the desktop app grew the real thing, mirroring the browser
  flasher's semantics exactly: local-file install (fingerprint-only —
  SHA-256 on the receipt, `release_verification: "local-file (fingerprint
  only)"`, the same "we can't vouch for a personal file's origin" honesty)
  and the dev channel (one fixed `fw-dev-latest` constant — never a general
  manifest override — pinned identical across flash-core.js, lib.rs, and
  app.js by a new `desktop_parity` drift test, with the same chip guard,
  SHA-256, and fail-closed signature policy as stable).
- **Applies to:** both flashers, forever. When one frontend's copy names a
  capability, the drift gate question is "does the other frontend have the
  capability?", not just "does it have the string?". Cross-frontend constants
  (manifest URLs, channel names) belong in `desktop_parity.test.js` so the
  next divergence fails a test instead of a user.

### 2026-07-25 — The C6 nightstand's first real link came out 21% bigger than the OTA slot it ships into

- **Symptom:** `canary-display-nightstand-c6`'s first full build against the
  real toolchain linked clean and then died in `checkprogsize`: 2,382,638
  bytes into a 0x1E0000 (1,966,080-byte) A/B slot. Every earlier signal was
  green — the env was registered, the OTA channel published, the flasher
  catalog carried the product — because nothing before the link ever does the
  slot arithmetic.
- **Cause:** the env was assembled from siblings that never share its budget.
  The graphics stack (LVGL + eight Montserrat faces + Arduino_GFX) was sized
  on 16 MB boards with 0x330000+ slots; the BLE features were costed on
  canary-sense, which carries no display. On the one board with BOTH a
  display and a 4 MB flash, the two budgets met for the first time at link
  time — the last possible moment.
- **Fix:** measure, then cut what the board never uses — never squeeze what
  it does. `esp-idf-size --archives` on the .map named the spend:
  `libble_app` + the NimBLE host (~300 KB) and the 36/48 room-scale fonts
  (~170 KB). The C6 env compiles both out, each cut documented in the env
  with its size tag (`CD_LEAN_BUILD` in lv_conf.h + a lean type ladder in
  character.cpp; `FEATURE_CHIRP_SCAN=0` / `FEATURE_FLEET_LINK=0` made
  `#ifndef`-overridable in the nightstand config). Landed at 1,955,024 of
  1,966,080 bytes — an ~11 KB margin, kept deliberately with the next two
  cuts already scoped in the env comment. Bonus find: `fleet_link.cpp`'s
  disabled-path stubs never compiled before (missing `<stdint.h>`) — a
  feature flag nobody has ever turned off is a build nobody has ever tested.
- **Applies to:** every env that pairs a rich UI stack with a 4 MB part
  (`min_spiffs`-class slots), and every future flavors.json entry: do the
  slot arithmetic at env-authoring time, not at first link. A flavor's
  size_guard watches ONE binary — a new env on a smaller part must either
  fit inside the guarded budget or document its own (checkprogsize is the
  final backstop, but it speaks at the last moment, in bytes, without
  naming the cuts). And when a feature flag exists, CI must compile at
  least one env with it OFF, or the disabled path is fiction.

### 2026-07-25 — The Lab app shipped a workshop with no renders: the web root had no parent to reach into

- **Symptom:** in the native **SecuraCV Lab** on macOS, the Workshop's "Start
  from a package" card showed the WebKit broken-image glyph instead of the
  package render, and the 3D viewport was empty for every part except the
  handful of watch-station / dash / combo meshes. The same page in a browser —
  same commit, same files — was perfect, so it read like a rendering or CSP
  problem in the webview. It was neither: the pieces that worked were exactly
  the ones whose files live *inside* `canary-local/`, and the pieces that
  failed were exactly the ones under `docs/hardware/enclosure/`.
- **Cause:** `frontendDist` was `../../canary-local`, so Tauri embedded that
  directory and served it as the **entire origin**. The shared frontend
  addresses the enclosure library as a sibling — `../docs/hardware/enclosure/…`
  in `workshop.js`, `enclosure-lab.js`, `assembly-lab.js`, `real-shapes.js`,
  `chooser.js` — which is correct from a repo checkout and from the deployed
  site (`pages.yml` assembles `_site/canary-local` *and*
  `_site/docs/hardware/enclosure`), and impossible in a bundle: the webview
  collapses `../` at the root, requests `/docs/hardware/enclosure/…`, and 404s.
  Nothing logged, nothing crashed — a static asset that isn't there just isn't
  there, and only half the page knows.
- **Fix:** stage the app's web root instead of pointing at a source directory.
  `desktop-lab/scripts/stage-frontend.mjs` mirrors the trees named in
  `desktop-lab/frontend-stage.json` (`canary-local` + `docs/hardware/enclosure`
  — deliberately the same list `pages.yml` deploys) into `desktop-lab/dist/`
  with `cp -RL` semantics, verifies its sentinels, and runs from
  `beforeDevCommand`/`beforeBuildCommand`; `frontendDist` is `../dist` and the
  window opens `canary-local/lab.html`. The app's root and the site's root now
  have the same shape, so one set of relative URLs is true on both. The two
  frontend links that pointed at repo docs rather than assets
  (`hub-setup-wizard.js`, `site-map.html`) moved to the `GH +` source-link idiom
  their siblings already use. `canary-local/tests/lab_bundle.test.js` is the
  gate: it resolves every escaping URL in the shipped frontend and fails unless
  the manifest carries it.
- **Applies to:** every app target that wraps a web root — the Lab, its iOS/iPad
  builds (same `tauri.conf.json`, so they inherit the fix), and any future
  webview shell. Two rules generalize. **A bundled web root has no parent:**
  if the frontend is shared with a site, the packaging step must reproduce the
  site's *layout*, not just its files. And **a missing static asset is a silent
  failure** — it produces a broken glyph, not an error, so the guard has to be
  a test over the source, not a hope that someone clicks the right tab before
  publishing.
