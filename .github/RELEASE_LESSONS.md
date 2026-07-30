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

### 2026-07-27 (b) — An "anti-rot" `release: published` trigger that can never fire for the releases CI publishes

- **Symptom:** every Vision module flash failed with *"no published release
  yet (manifest returned HTTP 404)"* — `manifest-vision-model.json` was
  absent from `fw-v2.3.0` even though `vision-model-release.yml` exists
  precisely to attach it "automatically on every published release."
- **Cause:** GitHub suppresses workflow events for anything created with a
  workflow's own `GITHUB_TOKEN` (the recursion guard). Every firmware
  release is published by `firmware-release.yml` with that token, so the
  `release: [published]` trigger never fires for them — it had only ever
  fired for the early human-published releases, which is exactly what made
  it look alive.
- **Fix:** `firmware-release.yml` now chain-dispatches
  `vision-model-release.yml` explicitly after publishing (`actions: write` +
  `gh workflow run`); the event trigger stays for human publishes, and the
  per-tag concurrency group de-dupes if both fire. **Generalize:** in this
  repo, never rely on a `release:`/`push:` event fired by a release another
  workflow publishes — if workflow B must follow workflow A's publish, A
  dispatches B by name.

### 2026-07-27 — A "successful" release quietly dropped the products that had just compiled, and the app that could tell you about updates was polling a URL that can never answer

Two independent failures, one release day, both invisible-by-design.

- **Symptom (1):** `fw-v2.3.0` published green, but carried none of the
  ESP32-S3 display images — the Dash 7 and Nightstand S3 had **compiled
  successfully** minutes earlier in the same job. The Flasher showed every
  display as "no published release yet" and nobody could say why, because the
  run concluded `success`.
- **Cause (1):** PlatformIO silently **cleans the whole `.pio/build`
  workspace when the project checksum changes** — and running the
  nightstand-c6 env with its (necessary) isolated `PLATFORMIO_CORE_DIR`
  changes exactly that. The c6 build erased its siblings' outputs; the
  display packaging loop is non-blocking on purpose, so "produced no binary"
  was a warning scrolled past in a 20-minute log.
- **Fix (1):** in `firmware-release.yml` and `flasher-release.yml`, stage
  each display env's `.pio/build/<env>` the moment it builds and restore the
  set after the c6 run. **Generalize:** any time two builds in one job vary
  `PLATFORMIO_CORE_DIR` / project options, treat earlier build outputs as
  already lost — copy them out first. And when a loop is deliberately
  non-blocking, its per-item failure messages are the *only* record; make
  them name the artifact and the consequence.
- **Symptom (2):** every installed Flasher's self-update check failed with
  *"Could not fetch a valid release JSON from the remote"*, forever, while
  the app reported itself up to date.
- **Cause (2):** the Tauri updater endpoint was
  `releases/latest/download/latest.json` — but this repo **deliberately pins
  `releases/latest` to the firmware releases** (it is the fleet's OTA URL;
  `keep-firmware-latest` moves it back within minutes of any app publish).
  A firmware release carries no `latest.json`, so the updater's one URL
  could never resolve. Two correct invariants, never introduced to each
  other.
- **Fix (2):** the updater now polls the rolling **`flasher-latest`
  prerelease** (a prerelease can never become `releases/latest`, so the two
  pointers cannot collide), which `desktop-flasher-release.yml`'s finalize
  job re-points — after the consistency guard — at every publish.
  **Generalize:** in this repo, no app updater may ever reference
  `releases/latest`; give each self-updating app its own rolling
  `<app>-latest` prerelease pointer. If the Lab (or a future target) gains
  an updater, copy this shape.

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

### 2026-07-25 — The dev channel had no button and no publisher: unreachable in the Lab, and unfillable without the OTA key

- **Symptom:** every product in the Lab's flasher read *"no published release
  yet"* — displays included, on hardware sitting wired up on the bench. Three
  facts stacked into one dead end. (1) The catalog pins `manifest_url` to an
  exact tag, `fw-v2.3.0`, which was never cut; the newest firmware release was
  `fw-v2.2.0`, published before the display sketch existed, so even reaching it
  would have offered zero display products. (2) `firmware-release.yml` — the
  only thing that publishes `manifest-flash.json` on its own — hard-stops in 20
  seconds without `OTA_SIGNING_KEY_PEM`. (3) `flasher-release.yml`, the
  documented no-key escape hatch, checks the tag *out*, so it could not help a
  tag that had never been created. The one remaining route, the dev channel,
  was reachable only as `?channel=dev` — and the Lab desktop app renders that
  page in a webview with **no address bar**. Every exit was closed, and each
  one closed for a different, individually reasonable reason.
- **Cause:** the dev channel was built as a *destination* with neither a road
  in nor a road out. Its constant was drift-gated across all three frontends
  (`desktop_parity.test.js`), its banner copy was written, its device-side NVS
  override was documented — but nothing could publish to `fw-dev-latest`
  without the signing key, and one of the two flashers had no control to select
  it. Both gaps were invisible to CI because both were about *absent*
  capability, and the drift gates all compared strings that were present and
  identical.
- **Fix:** a road at each end. `flasher-release.yml` gained `channel: dev` —
  builds the dispatch ref's HEAD onto the rolling `fw-dev-latest` prerelease,
  no tag, no version bump, no signing key, `prerelease: true` so
  `releases/latest` can never drift off the firmware. The browser flasher gained
  the **Advanced → dev channel** toggle the desktop Flasher already had, with
  `?channel=dev` demoted to seeding it. The banner stopped claiming dev images
  are "signed with the same key" when no key exists — it now reports
  checksum-only verification, matching what `imageVerificationPolicy()`
  actually returns. And `channel: release` now names a missing tag in its own
  error instead of dying inside `actions/checkout`.
- **Applies to:** every escape hatch, forever. A fallback path is only real if
  something can *fill* it and someone can *reach* it — assert both, not the
  constant between them. `desktop_parity.test.js` now checks that each frontend
  has a user-reachable dev-channel control (not just the URL), and that the
  workflow publishes to the same tag the frontends read; a publisher and a
  consumer naming the same release in two files is two chances to be wrong.
  When a product is "unavailable" everywhere at once, suspect the pinned tag
  before the wiring — `canary-local.yml` already warns on every run while the
  pin is unresolvable, and that warning was right for weeks.

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

### 2026-07-26 — A bundled native USB tool needs its Linux access rule shipped

- **Symptom:** the Flasher's "Wait for my Pi" (flash a Pi over USB-C, no card
  reader) did nothing on Linux — the Pi never appeared as a disk, so the app
  looked like it "wasn't receiving the USB connection". Worked on macOS.
- **Cause:** the bundled `rpiboot` sidecar runs as the user and opens the Pi's
  boot-ROM USB device (`0a5c:2712` on a Pi 5) over libusb. Linux denies a
  non-root process access to a USB device without a **udev rule**, and we shipped
  none — so `libusb_open()` failed, rpiboot never served the mass-storage gadget,
  and nothing enumerated. macOS has no udev equivalent (it grants USB access
  freely), which is exactly why the same build worked there — a classic
  works-on-mac-fails-on-linux gap for any bundled USB tool.
- **Fix:** ship the rule. `desktop/src-tauri/packaging/rpiboot.rules` grants
  access to the Pi boot device (Broadcom `0a5c`, product ids 2711/2712/2763/2764;
  `TAG+="uaccess"` + `0666`), and `tauri.conf.json`'s `deb.files` installs it to
  `/usr/lib/udev/rules.d/`. INSTALL.md documents the manual add for AppImage
  users (no package to install it). Belt: `hub_core::hub_usbboot` (host-tested)
  recognises rpiboot's device-open failures so the app shows the fix in-line
  instead of a silent wait, gated to Linux where it applies.
- **Applies to:** **every bundled native tool that opens a USB device** — today
  `rpiboot`, tomorrow anything similar. Two rules generalize. **macOS "just
  works" for USB is a trap:** it needs no udev, so a Linux access rule is easy to
  forget, and its absence fails *silently* (the device just never opens). If a
  target ships a native USB tool, ship (deb) **and** document (AppImage/manual)
  its Linux access rule — the serial tools already do this via the `dialout`
  group; device-mode USB tools need a udev rule instead. And **a permission
  failure that reads like "nothing happened" needs an in-app hint**, or the user
  has no way to know a one-line fix exists.

### 2026-07-27 — Mixed-platform release job: a warm cache masked a core-dir conflict until GitHub evicted it

- **Symptom:** the first signed firmware release (fw-v2.3.0) failed in
  `canary-sense-default` with `TypeError: expected str, bytes or os.PathLike
  object, not NoneType` at pioarduino's `arduino.py` (`FRAMEWORK_DIR=None`)
  before compiling a single file — twice, deterministically — while
  `canary-sense-wellbeing` (same board, same platform pin) built fine seconds
  later in the same job.
- **Cause:** the release job builds every project sequentially in one shared
  `~/.platformio`. canary/canary-vision (official `espressif32` platform) run
  first and install `framework-arduinoespressif32` 2.0.x; canary-sense rides
  the pioarduino core-3.x fork, whose platform ships a package with the **same
  name** at 3.3.8. pioarduino sees the official copy "installed", skips its
  own, and dies with `FRAMEWORK_DIR=None` — the exact failure
  `firmware.yml`'s `isolated_core_envs` comment documents. It never bit the
  release workflow before because the PlatformIO cache (keyed on an unchanged
  file) stayed warm with both versions resolvable; GitHub evicted the cache
  after 7 days of no saves, and the first cold run exposed it. The
  nightstand-c6 env in the *same job* already had the isolation; sense didn't.
- **Fix:** run canary-sense under its own
  `PLATFORMIO_CORE_DIR="$GITHUB_WORKSPACE/.pio-core-canary-sense"` in both
  `firmware-release.yml` and `flasher-release.yml` (the flasher factory-image
  job has the same sequential shape and was one cache eviction away from the
  same failure).
- **Applies to:** **every job that builds more than one PlatformIO project in
  sequence.** If any project pins the pioarduino fork while another uses the
  official platform, the pioarduino build MUST get an isolated
  `PLATFORMIO_CORE_DIR` — sharing the core dir only appears to work while a
  cache is warm. And more generally: **a step that only ever ran against a
  restored cache has never actually been proven** — the cache is a
  performance layer, not part of the contract, and GitHub deletes it after 7
  idle days. If a build breaks only when the cache misses, the build is
  broken.

### 2026-07-27 (c) — The Linux .deb shipped serial access half-solved: dialout was documented, but ModemManager was left free to knock the board over mid-boot

- **Symptom:** the identical flash flow that turns green on macOS stalled on
  Linux: either every port open failed and the app retried in silence
  ("Waiting for the board's serial port…" forever, or the misleading "put it
  in download mode" advice), or — with permissions fixed — the flash verified
  and the live boot receipt still never arrived, so the result never turned
  green.
- **Cause:** two Linux-only facts about USB serial that macOS never taught us.
  (1) Opening `/dev/ttyACM*` needs `dialout` membership or a udev rule; the
  .deb shipped a udev rule for **rpiboot** (the 2026-07-26 lesson) but nothing
  for the serial devices the app's core flow lives on. (2) ModemManager
  probes any newly enumerated CDC device as a modem — including the board
  re-enumerating right after espflash's post-flash hard reset — holding the
  port for ~30 s and toggling control lines the ESP32-S3's USB-Serial/JTAG
  can take as a reset. The boot the app was watching for got knocked over by
  the OS itself, deterministically, on stock Ubuntu.
- **Fix:** the .deb now installs `61-securacv-canary.rules` (Espressif
  `303a` + CH343 `1a86:55d3`): `TAG+="uaccess"`/`MODE="0666"` for access and
  `ID_MM_DEVICE_IGNORE`/`ID_MM_PORT_IGNORE` so ModemManager never touches a
  Canary; INSTALL.md carries the copy-paste rule for AppImage users. In-app,
  `port_hint.rs` classifies permission/busy open failures so the serial
  monitor, chip detection, and Vision-module flows say the one-line fix
  instead of retrying silently or coaching the BOOT/RESET ritual.
- **Applies to:** **every target that opens a serial device on Linux, and
  every future USB product ID.** A new board VID/PID must be added to
  `61-securacv-canary.rules` when it's added to the catalog. More generally:
  "the port exists" is not "the port is ours" on Linux — permission *and*
  ModemManager both need settling in packaging, and an open failure the app
  can classify must be explained in-app (a rule that lives only in docs is
  invisible at the moment it's needed). The udev-rule lesson from rpiboot
  (2026-07-26) applies to serial CDC devices too, with ModemManager as the
  extra tenant nobody invited.

### 2026-07-28 (a) — The DMG opened on a giant, cropped background: Finder maps background PIXELS to window POINTS

- **Symptom:** the Flasher's installer window showed the branded background
  blown up to double size — title cut off mid-word ("SecuraC…"), the drag
  instruction unreadable, the whole bottom "first launch" panel pushed out of
  the window. The PNG itself looked perfect in every image viewer.
- **Cause:** both DMG background generators rendered on a 2x canvas "for
  retina" and shipped that raw 2x PNG (1320×920 for a 660×460 window). Finder
  maps a background image's **pixels** straight onto window **points** and
  ignores PNG DPI metadata, so the window showed the top-left quarter of the
  image at double size. Retina Macs don't rescue it — the window is 660
  points regardless of the panel behind it.
- **Fix:** keep the 2x canvas as text supersampling only, and **downscale to
  the exact `windowSize` before saving** (`img.resize((WIN_W, WIN_H),
  LANCZOS)`) in both `desktop/src-tauri/dmg/generate_background.py` and
  `desktop-lab/src-tauri/dmg/generate_background.py`; both committed
  `background.png` files regenerated. If true retina sharpness is ever wanted,
  the mechanism is a multi-resolution TIFF built on a real Mac (`tiffutil
  -cathidpicheck`), not a big PNG.
- **Applies to:** **any image Finder itself draws** — DMG backgrounds in every
  app target, and any future volume icon layout. The shipped file's pixel
  size must equal the configured point size; a "2x for quality" canvas is a
  render-time trick that must never reach the artifact.

### 2026-07-28 (b) — Every installed Flasher ≤ 0.3.0 lost self-update forever when the endpoint moved: a compiled-in URL must be SERVED, not just corrected

- **Symptom:** installed Flashers logged "Update check failed: … Could not
  fetch a valid release JSON from the remote" on every check — including
  after #1286 fixed the updater endpoint. New installs were fine; the field
  was not, and 0.3.1+ could never reach them.
- **Cause:** builds ≤ 0.3.0 shipped polling
  `releases/latest/download/latest.json`, and `releases/latest` is
  deliberately owned by the **firmware** releases (Principle 6), which carry
  no `latest.json` — a permanent 404 for every copy already installed. Fixing
  the endpoint in `tauri.conf.json` only helps builds that don't exist on
  users' disks yet: an updater endpoint is **compiled in**, so the only way to
  reach old installs is to serve a valid manifest at the OLD address.
- **Fix:** `.github/actions/keep-firmware-latest` now has a second duty:
  after asserting `releases/latest` is firmware, it mirrors `flasher-latest`'s
  hardened `latest.json` onto that firmware release (digest-compared,
  idempotent). All three release paths converge on it: the guard workflow
  (human releases), `desktop-flasher-release.yml`'s `keep-latest` job (now
  `needs: finalize` so it mirrors the freshly hardened manifest), and
  `firmware-release.yml` right after publishing (a new fw-v* release takes
  `latest` the instant it publishes and would otherwise be born without the
  manifest). Old installs then see the update, verify it against the same
  updater pubkey every build has embedded, and come out polling the right
  endpoint. Devices never notice: they fetch `manifest-<product>.json` only.
- **Applies to:** **every compiled-in update URL in every target** — the
  Flasher's and Lab's updater endpoints, the firmware's OTA manifest URL, the
  Vision model manifest. Before moving any such URL, ask "who is already
  polling the old one, and what will they fetch there tomorrow?" — the answer
  must be "a working manifest", indefinitely, or the field is stranded. And
  when a rolling pointer release (`flasher-latest`, `fw-dev-latest`) gains a
  consumer, every workflow that creates or advances the release it shadows
  must keep the mirror fresh.

### 2026-07-28 (c) — `xcode-select` to an unversioned Xcode path never matches

- **Symptom:** iOS jobs "selected Xcode 16" and then built with whatever the
  runner image's default was — or, once a step actually depended on it,
  failed in ways that looked unrelated (missing simulators, wrong SDK).
- **Cause:** runner images install Xcode at *versioned* paths
  (`/Applications/Xcode_16.2.app`, never `Xcode_16.app`), and the trailing
  `|| true` swallowed the `invalid developer directory` error, so the no-op
  passed silently for months.
- **Fix:** resolve the newest installed version and always print the result:
  ```sh
  latest="$(find /Applications -maxdepth 1 -name 'Xcode_16*.app' | sort -V | tail -1)"
  if [ -n "$latest" ]; then sudo xcode-select -s "$latest"; fi
  xcodebuild -version
  ```
  A selection step that can't tell you what it selected isn't a step, it's a
  wish. Same class of rot as hard-coding a simulator model name
  (`heal.sh` now picks the newest installed iPhone simulator for the same
  reason).
- **Applies to:** `ios-selfheal.yml`, `ios-release.yml` (both fixed);
  any future macOS job that pins a toolchain path.

### 2026-07-28 (d) — CI archives die on a fresh team: automatic signing archives as DEVELOPMENT, and development profiles need a registered device

- **Symptom:** the first authenticated `ios-release.yml` run failed archiving
  all three targets with "Your team has no devices from which to generate a
  provisioning profile" — even though the release was bound for TestFlight,
  which never involves registered devices.
- **Cause:** `xcodebuild archive` under automatic signing signs with a
  *development* profile (distribution happens at export), and Apple will not
  mint a development profile for a team with zero registered devices. CI
  runners never have a device to offer.
- **Fix:** register **any one device** on the team (Certificates,
  Identifiers & Profiles → Devices → +, with the phone's UDID from Finder) —
  one-time, and it unblocks every future archive. The tempting CI-side
  patch — `CODE_SIGN_IDENTITY="Apple Distribution"` on the archive — does
  NOT work: automatic signing rejects it with "conflicting provisioning
  settings". Corollary for dry runs: export with `app-store-connect`
  (+ `publish=false`); `release-testing` is an ad-hoc export that *also*
  requires registered devices.
- **Applies to:** `ios-release.yml`, `tvos-release.yml` (an Apple TV
  registers the same way), and any Apple-platform CI archive with automatic
  signing on a team that may have no registered devices.

### 2026-07-28 (e) — Per-run cloud certificates orphan themselves: the second CI signing run always fails

- **Symptom:** the first signing run after device registration got past every
  earlier gate and failed with "Revoke certificate: Your account already has
  an Apple Development signing certificate for this machine, but its private
  key is not installed in your keychain."
- **Cause:** `-allowProvisioningUpdates` cloud signing *created* a
  certificate during an earlier run — and its private key lived only in that
  ephemeral runner's keychain, which was destroyed with the runner. Every
  later run sees the account's certificate, has no key for it, and
  non-interactive xcodebuild will not auto-revoke. Per-run certificate
  creation is structurally unsound on throwaway machines; distribution
  certificates make it worse (Apple caps them at 2–3 per team).
- **Fix:** import a persistent identity: export Apple Development + Apple
  Distribution from a real Mac's Keychain Access as one `.p12`, store as the
  `APPLE_CERTIFICATE` / `APPLE_CERTIFICATE_PASSWORD` secrets (the exact
  names desktop-lab/MOBILE.md already documents), and have the workflow
  import it into a temp keychain (`security create-keychain` → `import` →
  `set-key-partition-list` → `list-keychains`). Orphaned Apple Development
  certificates on the account are safe to revoke — dev certs are recreated
  on demand. Cloud signing then only manages *profiles*, which are
  stateless.
  One caveat: `APPLE_CERTIFICATE` is repo-wide — the macOS desktop pipeline
  keeps its *Developer ID Application* identity in the same secret
  (`desktop/SIGNING.md`), so the `.p12` must be a **combined** bundle
  containing every identity the pipelines need, never a platform-only
  replacement.
- **Applies to:** `ios-release.yml` and `tvos-release.yml` (both fixed), and
  any Apple-platform job that signs on ephemeral runners with
  `-allowProvisioningUpdates`.

### 2026-07-28 (f) — Distribution export needs an Admin API key: "Cloud signing permission error"

- **Symptom:** with archive green, `xcodebuild -exportArchive` (automatic
  signing, `app-store-connect` method) failed in seconds with "Cloud signing
  permission error" + "No profiles for '<bundle id>' were found" for every
  target — even though the keychain held a valid imported Apple Distribution
  identity.
- **Cause:** creating **distribution** provisioning profiles and touching
  cloud-managed distribution certificates requires an **Admin**-role App
  Store Connect API key. A lesser key can mint development profiles (so the
  archive works, which makes the export failure look mysterious), and a
  key's role cannot be upgraded after creation.
- **Fix:** two halves. (1) Generate a new API key with **Admin** access and
  update `APPLE_API_KEY` / `APPLE_API_KEY_BASE64`. (2) Pin
  `signingCertificate` in ExportOptions ("Apple Distribution", or "Apple
  Development" for a development-method export) so the export prefers the
  persistent imported identity over requesting a cloud-managed one.
- **Applies to:** `ios-release.yml` (fixed); any future Apple-platform
  export with automatic signing.

### 2026-07-28 (g) — altool never takes a key path: stage the .p8 where it actually looks

- **Symptom:** the first run to reach "Upload to App Store Connect" failed
  validation with Cocoa error -43: "The file 'AuthKey_….p8' could not be
  found in any of these locations: './private_keys', '~/private_keys',
  '~/.private_keys', '~/.appstoreconnect/private_keys'."
- **Cause:** `xcrun altool` accepts only `--apiKey <id>` and searches those
  four fixed directories — unlike `xcodebuild`, it has no
  `-authenticationKeyPath` equivalent, so the `$RUNNER_TEMP/keys/…p8` the
  workflow materializes is invisible to it. Latent until every earlier
  signing gate was cleared, because no run had ever reached the upload.
- **Fix:** `asc_publish.sh` now copies `$APPLE_API_KEY_PATH` into
  `~/.appstoreconnect/private_keys/AuthKey_$APPLE_API_KEY.p8` before calling
  altool. Fixed in the shared script precisely so both callers inherit it.
- **Applies to:** `ios-release.yml` and `tvos-release.yml` (both publish
  through `.github/scripts/asc_publish.sh`).

### 2026-07-28 (h) — ASC's SDK floor bites AFTER a green upload: ITMS-90725 arrives by email

- **Symptom:** the first fully green `ios-release.yml` run (validated,
  uploaded, tagged `ios-v0.1.0`) never reached TestFlight. Minutes later
  Apple emailed "ITMS-90725: SDK version issue — this app was built with the
  iOS 18.2 SDK. All iOS and iPadOS apps must be built with the iOS 26 SDK or
  later." `altool --validate-app` does NOT catch this; asynchronous
  post-upload processing does, so CI is green while the build is dead.
- **Cause:** the workflow pinned `macos-14` + `Xcode_16*` (both chosen for
  earlier, unrelated reasons). Apple enforces a rolling minimum SDK for App
  Store Connect uploads; the runner image and Xcode glob silently aged out.
- **Fix:** `runs-on: macos-26` (GA since 2026-02, default Xcode 26.x) and
  select `Xcode_26*`, in the release, the self-heal/CI builds (the first
  compiler must match the shipping SDK), and both tvOS jobs — the same floor
  applies per-platform. Fallout: the tag guard correctly refuses to re-run
  a tagged version, and ASC has the dead binary as v0.1.0 build 1 — a
  post-upload rejection therefore BURNS the version; bump
  `MARKETING_VERSION` and ship the next one.
- **Applies to:** `ios-release.yml`, `ios-selfheal.yml`,
  `tvos-release.yml`, `tvos.yml` (all fixed), `desktop-mobile-release.yml`
  (already on `macos-latest`, which tracks new images). Watch for the same
  aging on the next annual Xcode requirement.

### 2026-07-28 (i) — Non-blocking build loops rot silently: watch/dash/dash-modes missing from every firmware release ever cut

- **Symptom:** the Flasher's product picker showed "no published release
  yet" for Canary Watch Station, Dash, and Dash · Modes while their
  siblings said "release 2.4.0". The fw-v2.4.0 release carried no
  `canary-display-watch/dash/dash-modes` binaries, factory images, or
  manifests — and neither did any earlier release from this workflow.
  Every run was green.
- **Cause:** two independent failures inside a deliberately non-blocking
  loop. (1) The release step compiled the three flavors with `arduino-cli
  compile --profile`, but profile mode installs libraries into an isolated
  internal dir where the sketch's `lv_conf.h` can never be staged — LVGL
  8.4 resolves its config as `libraries/lvgl/src/../../lv_conf.h`, so
  watch/dash died on `fatal error: ../../lv_conf.h: No such file or
  directory`. (2) The `modes` profile pins a vendor board
  (`waveshare_esp32_s3_touch_lcd_43b`) that fails `Invalid FQBN` in
  profile mode. Meanwhile push CI (`firmware.yml`) proved a DIFFERENT
  path — explicit `--fqbn` + user-space libraries + a `cp lv_conf.h`
  staging step — and only exercised profile mode on the core-3 profiles
  the release never uses. Each release failure surfaced as `::warning::`
  (non-blocking by design, so a display hiccup can't sink the signed OTA
  release), which meant nobody ever saw it.
- **Fix:** `firmware-release.yml` now builds the three flavors exactly the
  way `firmware.yml` proves on every push: a pinned core-2.0.17 install
  (GFX 1.4.9 / LVGL 8.4.0 / NimBLE 1.4.3) via `setup-arduino-esp32`,
  `lv_conf.h` copied to the libraries-dir root, and the sketch.yaml
  profiles' own FQBNs passed explicitly (`modes` = the dash board + the
  gear flags staged by `setup.sh flavor modes`; the assemble step's
  product-string check still guards the dash-modes identity).
- **Applies to:** any workflow with a non-blocking loop around builds —
  if a step is allowed to fail quietly, something else must count the
  outputs. The assemble step already warned per-missing-binary; a warning
  nobody reads is not a control. And: a release workflow must build the
  same way CI builds, or CI green proves nothing about the release.

### 2026-07-28 (j) — RGB-panel boards must ship the core-3 pairing: core-2 has no bounce buffers and the glass visibly flickers under WiFi

- **Symptom:** flicker / tearing / sideways shifting on the 4.3B dash
  glass in normal operation (WiFi is always up on a fielded Canary).
- **Cause:** the ESP32-S3 RGB peripheral scans its framebuffer out of
  PSRAM continuously; when the radio contends for PSRAM bandwidth the
  scanout underruns and the panel visibly glitches. The fix is bounce
  buffers (small internal-SRAM staging the LCD DMA streams from), which
  exist only in GFX 1.6.x — the core-3 pairing. GFX 1.4.9 (the core-2
  pairing) predates the parameter, so any core-2 dash build carries the
  defect by construction. And EVERY RGB build was core-2: the (i) fix
  below initially put the 4.3B dash/modes release builds on core 2.0.17
  (correct for the SPI watch, wrong for RGB glass), and the dash-family
  PlatformIO envs — dash7 included, the one RGB product that actually
  shipped in fw-v2.4.0 — all extended the core-2 base, so fielded Dash 7
  units flicker today. (An earlier version of this entry claimed dash7
  had already moved to core-3; that was wrong — only the C6 nightstand
  base had.)
- **Fix:** two halves. `firmware-release.yml` splits the display Arduino
  builds: watch on the core-2 row (SPI panel, bench-validated pairing),
  dash + modes on core 3.3.10 + GFX 1.6.6 / LVGL 9.5.0 / NimBLE 2.5.0
  with bounce buffers compiled in — the same rows firmware.yml proves on
  every push. And `canary-display.ini` moves `[env:canary-display-dash]`
  (which every dash-family env, dash7 included, extends) onto the
  pioarduino core-3 platform with the core-3 library row, so PlatformIO
  release and bench builds get the bounce buffers too. `flavors.json`
  groups the CI build order by platform so the shared PLATFORMIO_CORE_DIR
  sees one platform switch, not many (the fw-v2.3.0 output-wipe
  mechanism). `display_dash.cpp` marks the core-2 RGB path bench-only.
- **Applies to:** every RGB-parallel-panel board, on any target. SPI
  panels (watch, nightstand, touch169) have no scanout-contention
  mechanism and stay on their bench-validated pairing. If a new RGB board
  joins the fleet, its release build belongs on the core-3 row from day
  one.

### 2026-07-29 (k) — Two core versions in one job: the cache layers them and an unversioned FQBN takes the newest

- **Symptom:** `fw-v2.4.1` — the release that finally fixed the display
  builds — shipped six of seven display products. The Canary Watch
  Station was missing again: `canary-display watch produced no binary`,
  a `::warning::`, run green. Dash and Dash · Modes, built two steps
  later, were fine.
- **Cause:** the release job calls `setup-arduino-esp32` three times
  (latest core for the WAP, 2.0.17 for the SPI watch, 3.3.10 for the RGB
  dash). Each call restores a `~/.arduino15` cache **into the same
  directory** — `actions/cache` extracts, it does not clear — and
  `arduino-cli core install X@V` is a no-op when V is already present.
  So both 3.3.x and 2.0.17 sat in the tree, and an FQBN carries no
  version: the watch compiled against the NEWEST installed core (3.x)
  with the core-2 library row pinned beside it, which is the documented
  mixing failure (`esp32-hal-periman.h: No such file or directory`) and
  dies in seconds. The dash row "worked" only because its wanted core
  happened to be the newest — which is exactly why the defect was
  invisible from that side.
- **Fix:** `setup-arduino-esp32` gains `exclusive-core`. When true it
  uninstalls every other `esp32:esp32` version before installing this
  row's, then **asserts** the requested version is the installed one and
  fails the step if not. The assertion lives in setup, where failing
  loudly is correct — the display compiles downstream are deliberately
  non-blocking, so a wrong core there can only ever surface as a warning
  and a missing product. Both display rows in `firmware-release.yml`
  set it; `flasher-release.yml` was also ported off profile mode onto
  the same two-row build (it had carried lesson (i)'s defect untouched,
  so the "Flasher Factory Images" button had never produced a watch,
  dash, or modes image either).
- **Applies to:** any job that installs more than one version of the same
  Arduino platform — and, generally, any toolchain selected by an
  unversioned identifier. If a build's correctness depends on *which*
  version is installed, assert it after install; don't infer it from the
  install command succeeding. Same shape as the PlatformIO core-dir
  isolation (lesson (j)/#1313): shared toolchain directories are state,
  and a cache restore is not an install.

### 2026-07-29 (l) — A local `uses: ./` action comes from the WORKSPACE, so a tag checkout silently downgrades it

- **Symptom (caught in review, before it shipped):** `flasher-release.yml`
  on `channel=release` would have rebuilt an old tag using that tag's copy
  of `.github/actions/setup-arduino-esp32` — the one without
  `exclusive-core`. GitHub only *warns* on an unexpected input, so the
  run would look fine while quietly reproducing the mixed-core failure
  from (k); a tag predating the action entirely would fail the step.
- **Cause:** local composite actions are read from the checked-out
  workspace at step time. This workflow deliberately checks out the
  **tag** it is rebuilding, so every `./`-referenced action is that tag's
  version — even though the workflow YAML itself is the dispatch ref's.
  The two halves of a run can therefore come from different commits.
- **Fix:** the existing tooling overlay (which already refreshes
  `make_factory.py`, `build_flash_manifest.py`, and `flash.json` from the
  dispatch ref) now also overlays `.github/actions/setup-arduino-esp32`,
  with `cp -RL`.
- **Applies to:** any workflow that checks out a ref other than the one it
  was dispatched from — rebuild buttons, backport jobs, tag-repair jobs.
  If the workflow body is "today" but the workspace is "then", every
  local action, script, and config it touches is "then" until you
  overlay it. Decide per file which era it should come from, and say so.

### 2026-07-29 (m) — One secret name, two Apple certificates: setting up the iPhone app silently un-shipped the Mac apps

- **Symptom:** `flasher-v0.3.4` published and signed fine at 06:23. Every
  desktop release after 17:00 that day failed — three in a row, so
  **Flasher 0.3.5 never shipped** — with tauri's
  `failed to bundle project: certificate from APPLE_CERTIFICATE "Apple
  Development: …" does not match provided identity "***"`. Nothing in the
  desktop tree had changed. The firmware releases beside them were green,
  so the master button reported a healthy push while one product silently
  stopped shipping.
- **Cause:** `APPLE_CERTIFICATE` / `APPLE_CERTIFICATE_PASSWORD` were read by
  **six** workflows that need **different certificates**. The Apple-native
  pipelines (`ios-release`, `tvos-release`, `desktop-mobile-release`) sign
  for the **App Store** and want an **Apple Distribution** identity; the
  Tauri desktop pipelines (`desktop-flasher-release`, `desktop-release`)
  produce a **notarized DMG downloaded outside the App Store**, which
  requires **Developer ID Application** — `desktop/SIGNING.md` even says in
  bold *not* Apple Distribution. Standing up iOS signing wrote an iOS `.p12`
  into the shared name, which is correct for iOS and fatal for macOS.
  Whoever writes the secret last breaks the other platform, with no warning
  and no diff to point at.
- **Fix:** give each signing domain its own namespace. The desktop apps now
  read `APPLE_DESKTOP_CERTIFICATE` / `APPLE_DESKTOP_CERTIFICATE_PASSWORD`
  (Developer ID Application); iOS/tvOS/mobile keep `APPLE_CERTIFICATE`
  (Apple Distribution) untouched, so the working iPhone setup is not
  disturbed. Both desktop workflows also gained a **preflight**: decode the
  `.p12`, list every certificate common-name in it, and fail in seconds if
  `APPLE_SIGNING_IDENTITY` isn't among them — printing what *was* found —
  instead of dying ten minutes into a bundle. It warns, too, if the identity
  isn't a `Developer ID Application:` one.
- **Applies to:** every credential shared across targets in one repo —
  signing certs, API keys, provisioning profiles. If two platforms need
  *different values* for the same concept, they need *different names*; a
  shared name is a silent coupling that only shows up as someone else's
  build failing later. And validate credentials at the START of a long job,
  where the error is cheap and legible.

### 2026-07-29 (n) — `security export -t identities` exports EVERY identity, and tauri validates the last one

- **Symptom:** with the Developer ID certificate correctly created and the
  new `APPLE_DESKTOP_CERTIFICATE` secret in place, the signed macOS build
  still died ~8 minutes in with the same message as the collision it was
  meant to fix: `certificate from APPLE_CERTIFICATE "Apple Development:
  …" does not match provided identity`. The (m) preflight PASSED, because
  the requested identity really was inside the `.p12`.
- **Cause:** two facts meeting. `security export -k login.keychain-db -t
  identities` exports **every** identity in the keychain — not just the
  ones `security find-identity -v -p codesigning` lists (that command
  filters to identities valid for the codesigning policy, which is why it
  showed only one while three were present). And tauri checks the **last**
  certificate it finds in the bundle against `APPLE_SIGNING_IDENTITY`, so
  a `.p12` carrying the iOS Apple Development / Apple Distribution
  identities alongside the Developer ID one aborts even though the right
  certificate is there.
- **Fix:** the `.p12` must contain exactly ONE identity — in Keychain
  Access, expand the certificate and export the certificate row alone. The
  preflight now also fails when the bundle holds anything besides the
  requested identity, naming the extras, so this costs seconds instead of
  a full build. "Contains the right cert" was too weak a check; "contains
  only the right cert" is the real invariant.
- **Applies to:** every `.p12` fed to a CI signer. Verify the *shape* of a
  credential bundle, not just that the needle is somewhere in it — and
  never assume an export command exports the subset you were looking at.

### 2026-07-29 (o) — A credential runbook made of copy-paste steps fails at the paste, not the crypto

- **Symptom:** four consecutive dry runs of `desktop-flasher-release.yml`
  died in the signing preflight, and not one of them was a signing
  problem. In order: a three-identity `.p12` (twice, see (n)); then
  `APPLE_DESKTOP_CERTIFICATE` **empty**, because Keychain Access had saved
  the re-export somewhere other than the path the instructions assumed, so
  `base64 -i <that path>` printed nothing and `gh secret set` stored the
  empty string; and before that, a pasted block whose assignment line
  carried a trailing `# comment`, which zsh runs as a command — leaving the
  variable unset so every later check read zero and *still* wrote both
  secrets.
- **Cause:** the runbook was a sequence of human steps over a credential
  whose correctness is invisible locally. Each step had a silent failure
  mode (`gh secret set` accepts an empty value; `base64` of a missing file
  exits 0 on some paths; a GUI remembers its own last directory; `#` is a
  comment in a file but a command in an interactive zsh line), and the
  first place any of them surfaced was a CI job eight steps in.
- **Fix:** `desktop/scripts/set-desktop-signing-secrets.sh` — one command,
  no paths to substitute, no GUI. It reads the identity from the keychain,
  repacks *only* that certificate and the private key that matches it **by
  public-key digest** (the export's order is not guaranteed; in testing the
  key sat two positions away from its certificate, so pairing by position
  would have produced a `.p12` that imports cleanly and then cannot sign),
  and refuses to upload anything unless the result has exactly one
  certificate, one matching key, an unexpired cert, and a CN identical to
  the identity string. It sets `APPLE_SIGNING_IDENTITY` from that same
  certificate, so the workflow's exact-string comparison cannot fail on a
  stray space. Values go to `gh` on **stdin**, never argv.
- **Applies to:** any credential a human has to move by hand into CI. If
  correctness is only observable in CI, the local step must verify it
  locally and refuse to proceed — a documented click-path cannot. Ask what
  the preflight demands and make the setup tool produce exactly that; here
  the guard's real invariant was "exactly one certificate", so the script
  omits even the Apple intermediate.
- **Second-order trap caught in review:** a *partial* credential setup is
  worse than none. The desktop workflows pick the unsigned branch unless
  `ENABLE_MACOS_SIGNING` is exactly `true`, and notarization needs
  `APPLE_ID` / `APPLE_PASSWORD` / `APPLE_TEAM_ID` — none of which the
  certificate script owns. Setting only the three cert secrets and calling
  it done would ship an **unsigned** app under a green checkmark. A setup
  tool that covers part of a credential set must enumerate what it did
  *not* set, because the failure mode of the remainder is silent success.

### 2026-07-29 (p) — openssl and macOS disagree about PKCS#12: "wrong password?" when the password is right

- **Symptom:** with a correct single-identity Developer ID `.p12` in
  `APPLE_DESKTOP_CERTIFICATE`, the (m)/(n)/(o) preflight passing cleanly,
  and `ENABLE_MACOS_SIGNING=true`, the signed macOS build still died —
  7.5 minutes in, at the bundling step:
  ```
  security: SecKeychainItemImport: MAC verification failed during PKCS12 import (wrong password?)
  failed codesign application: failed to run command security import
  ```
  The password was **not** wrong. The same `.p12` and the same password
  had just been opened successfully by `openssl` three times: once by the
  setup script, twice by the preflight.
- **Cause:** the two tools support different PKCS#12 encryption. OpenSSL 3
  defaults to AES-256-CBC with a **SHA-256** MAC; macOS's Security
  framework only understands the legacy **SHA-1** MAC, and reports every
  other MAC as a password failure. A Mac with Homebrew openssl first on
  `PATH` therefore produces a file `openssl` reads perfectly and
  `security import` — which is what tauri runs to sign — cannot read at
  all. The misleading error sends you to re-check the password, which is
  the one thing that was never wrong.
- **Fix:** two halves. `set-desktop-signing-secrets.sh` now exports with
  `-keypbe PBE-SHA1-3DES -certpbe PBE-SHA1-3DES -macalg sha1`, and the
  release preflight **re-encrypts** whatever it is handed into that form
  before tauri sees it (`APPLE_CERT_P12_B64`), so the pipeline no longer
  depends on which openssl the operator happens to have. Both then prove
  the result by running `security import` into a throwaway keychain and
  deleting it — the same command that failed.
- **Applies to:** any credential validated by one implementation and
  consumed by another. Six checks passed before this because all six were
  openssl checking openssl's own output; the format was never the thing
  under test. **Validate with the tool that will actually consume the
  artifact**, especially when a library and an OS framework both claim to
  implement the same standard. And distrust the error text: "wrong
  password?" was a guess by code that could not tell the difference.
- **Outcome:** Flasher 0.3.5 shipped signed and notarized on the next run —
  the first Apple-signed desktop build this repo has produced. Verified
  end to end on real machines, not just in CI: the `.dmg` opened on macOS
  with no Gatekeeper prompt, and on Linux an installed 0.3.4 detected the
  update, showed these notes, installed on one click, and logged it. All
  ten release assets were counted, and the updater-URL consistency guard
  passed. The map of which certificate signs which target now lives in
  [`docs/APPLE_SIGNING.md`](../docs/APPLE_SIGNING.md).
