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

## 2026-08-09 — a wire field that only the firmware could read

**Symptom.** Every display in the fleet drew a generic symbol in the iPhone
app instead of its own picture, on hardware whose drawing had been in the
figure ledger for months.

**Cause.** Two lookups existed for "what does this device look like": by
device type, and by BOARD. The firmware had both (`common/core/fleet_figures.h`
— `figure_for` and `figure_for_hardware`). The apps had only the first. And the
first is deliberately incomplete: several products share one device type, so
those types are absent from the map on purpose, and every non-nightlight
display self-reported the family string `canary-display`, which is one of them.
So the app asked the only question it could ask, got the correct answer "I
cannot tell", and drew the honest fallback — forever.

**Lesson — a capability that exists on one side of the wire is not shipped.**
The board-precise lookup was real, tested, and unreachable by any client,
because nothing put the board id on the wire. When you add a second, stricter
way to identify something, check every consumer can actually obtain the input
it needs; a generator that emits a C++ table and no Swift one has shipped half
a feature, and the half that is missing is invisible from the half that works.

**Applies to every target.** The fix emits the board map to Swift alongside
the C++ header from the same generator run, so the two cannot drift; the iOS,
watch and widget bundles all compile `ios/Shared`, so they gained it together.
Anything that later grows its own copy of the figure lookup should read it
from the generator, never hand-maintain a third table.

**Related, same day.** The 7" display's brightness control had the mirror-image
problem: `/api/set` accepted `day_pct` on a board whose backlight is binary in
hardware, so the app's slider wrote a value that could not do anything, while
the knob that DOES dim that glass (`bright_pct`, a rendered scrim) was reachable
only from the on-glass menu. Same shape of bug — a working mechanism with no
path to the client — and the same fix: serve the capability, let the device
declare it, and let the app render what the device says it has.


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

### 2026-09-03 — The release notes said the false privacy claim was fixed; the installer's own background image still made it

- **Symptom:** Lab 0.2.4's notes celebrated exact network claims ("talks
  only to your own devices") while the committed DMG background —
  the first thing a macOS user sees when they open the installer — still
  read "nothing phones home", and the bundler's `longDescription` (the
  .deb/AppImage store text) called the fetched `latest.json` a "signed
  update manifest". Caught by review on the release-bump PR, not by any
  gate.
- **Cause:** a privacy/network claim lives on more surfaces than the file
  being edited: committed installer artwork (`src-tauri/dmg/background.png`
  + its generator), `tauri.conf.json`'s `longDescription`, README, release
  notes, and in-app copy — and nothing holds them together. And "signed
  update manifest" overstated the mechanism: Tauri verifies the
  installer's signature from `platforms[*].signature`; the manifest's own
  version/notes/URLs are outside any signature.
- **Fix:** reword every surface in one commit and regenerate the DMG
  background from its generator (no CI byte-gate exists on it — the
  committed PNG only changes when someone re-runs the generator, so the
  generator edit alone fixes nothing a user sees).
- **Applies to:** every app target with committed installer artwork or
  store descriptions. When a network/privacy claim changes, grep the
  claim's old wording across the app's whole directory — artwork
  generators included — and describe signing by what is actually signed
  (the artifact, unless the manifest itself carries a verified signature).

### 2026-08-23 — The dev channel quietly had no AMOLED: two workflows, one product list, no gate comparing them

- **Symptom:** the Canary Glance AMOLED read "no published release yet" on
  the dev channel while every sibling board was there. Nothing was red:
  the Flasher Factory Images runs were green.
- **Cause:** `firmware-release.yml` and `flasher-release.yml` each carry
  their OWN copy of the display PlatformIO env list, and when amoled241
  was added only the firmware release's list gained it. The factory-image
  path's display builds are non-blocking by design (a per-variant
  `::warning::`, then the product just isn't in the manifest), so the drop
  was silent — the same failure shape as 2026-07-28 (i), reached through a
  different door. `build_flash_manifest.py` knew the product all along;
  the env simply was never built on that path.
- **Fix:** add `canary-display-amoled241` to `flasher-release.yml`'s
  display env loop (this entry's commit).
- **Applies to:** every place a product list is duplicated across
  workflows. When a board/env/product is added, `grep -rn` its env name
  across `.github/workflows/` and expect a hit in BOTH release paths
  (firmware release AND factory images) — one green run proves only the
  list it ran from. The release path's product-drop gate catches a product
  that vanishes between two firmware releases; nothing yet compares the
  two workflows' lists to each other, so the grep is the gate.

### 2026-08-22 — The upload succeeded, the tag was cut, and TestFlight showed nothing: the tvOS plist never declared export compliance

- **Symptom:** tvos-v0.2.2 uploaded to App Store Connect with "No errors
  uploading archive", the workflow went green and tagged the version — and no
  build appeared in TestFlight. Nothing was broken on our side to look at.
- **Cause:** `ITSAppUsesNonExemptEncryption` lives in the iPhone app's two
  Info.plists (added precisely "so TestFlight builds don't park on Missing
  Compliance") but was never copied to `tvos/WitnessWall/Support/Info.plist`.
  Without it, App Store Connect accepts the build and then parks it on the
  manual export-compliance questionnaire — invisible and uninstallable in
  TestFlight, with the only signals living in ASC's web UI and the account
  holder's email, neither of which CI can see. A green "upload" step proves
  delivery, not distribution.
- **Fix:** declare the key in the tvOS plist (this entry's commit) so every
  future build clears compliance automatically. The already-uploaded build is
  released by answering the questionnaire once in App Store Connect →
  TestFlight → the build's "Manage" link.
- **Applies to:** every current and future App Store Connect target. When a
  new Apple target is added, its Info.plist must carry the same declaration —
  and remember the general shape: a per-platform plist is a copy, not a
  shared file, so an "every build parks" fix on one platform fixes exactly
  one platform.

### 2026-08-22 — A CLI cleanup changed one flag to positional, and the workflow that broke was the one no PR gate compiles

- **Symptom:** the fw-v2.4.13 release published every board's firmware, then
  went red 20 minutes later on "vision model asset never landed" — the chained
  `vision-model-release.yml` run had died in 2 seconds on
  `ota_release.py: error: unrecognized arguments: --firmware`.
- **Cause:** `ota_release.py`'s `sign` subcommand takes the binary as a
  positional argument, and `firmware-release.yml` calls it that way — but
  `vision-model-release.yml` still passed `--firmware`. A script's CLI is a
  wire contract with every workflow that shells out to it, and nothing
  compiles a workflow's inline bash: the first execution IS the test, and for
  a release-only workflow that execution is a release.
- **Fix:** pass the path positionally (this entry's commit), and when changing
  any `firmware/scripts/*` CLI, `grep -rn` the script's name across
  `.github/workflows/` — every hit is a caller on the same contract.
- **Applies to:** every workflow that shells out to a repo script — the
  firmware release, the vision model asset, the factory-image rebuilds, and
  any future chained dispatch. The failure mode is invisible until the one
  moment it matters.

### 2026-08-09 — A committed build number of `1` meant every marketing version got exactly one upload, ever

- **Symptom:** an App Store Connect / TestFlight build could never be respun.
  Re-uploading the same marketing version is rejected as a duplicate, so a
  transient upload failure, a signing hiccup, or a one-line fix all cost a
  **marketing version bump** — and the version that failed is burned, because
  the workflow's own guard refuses a tag that already exists.
- **Cause:** `ios/project.yml` and `tvos/WitnessWall/project.yml` both commit
  `CURRENT_PROJECT_VERSION: "1"`, and neither release workflow overrode it. App
  Store Connect identifies a build by **(marketing version, build number)**, so
  a constant build number collapses that pair to just the marketing version.
  It looked harmless because the marketing version was bumped every time
  anyway — the constraint was invisible until someone needed a second build of
  one version.
- **Fix:** pass `CURRENT_PROJECT_VERSION="$BUILD_NUMBER"` on the `xcodebuild`
  command line at archive time, sourced from `${{ github.run_number }}`.
  Three details that are the whole point:
  - **The command line, not the project file.** It reaches every target in the
    scheme, and the watch app and widget extension MUST carry the same build
    number as the host app or App Store validation rejects the bundle.
  - **`<run_number>.<run_attempt>`, not a commit count and not either half
    alone.** The counter has to count **attempts, not content**: the respin
    case is "same commit, upload again", so a commit count emits the same
    value twice and is rejected identically — it looks like a fix and isn't
    one. And `run_number` alone fails the same way, because GitHub's **Re-run
    jobs** reuses the run: `run_number` does not move, only `run_attempt`
    does, and a re-run after an accepted upload with a failed later step is
    exactly the retry an operator reaches for. Composed they only ever
    increase (42.1 → 42.2 → 43.1), and a dotted `CFBundleVersion` is compared
    component by component. *(The first version of this fix used `run_number`
    alone and had to be corrected in review — the trap is convincing.)*
  - **A local fallback** (`: "${BUILD_NUMBER:=1}"` in the tvOS script) so a
    developer run still works, where uniqueness is nobody's problem.
- **Applies to:** both Apple targets, fixed together. The iPhone app is where
  it was noticed; tvOS carried the identical setting and would have hit the
  identical wall on its first respin. Any future signed target that inherits a
  committed `CURRENT_PROJECT_VERSION` needs the same override — the value in
  the project file should be read as a placeholder, never as the build number.
### 2026-08-09 — A six-minute emulator rebuild died on "fetch first", and it read like a build failure

- **Symptom:** the "Rebuild emulator dist (pinned emsdk)" run dispatched for
  #1536 went red on a job named `rebuild`. Every signal said the emulator had
  failed to compile.
- **Cause:** it compiled fine. The logs end with `OK: canary-wap-audio`, a
  clean `git commit` of the regenerated bytes, and then
  `! [rejected] HEAD -> <branch> (fetch first)`. The workflow checks out the
  branch, installs emsdk, compiles every flavor, commits and pushes — and one
  ordinary push to that branch anywhere inside the window makes the final
  `git push` a non-fast-forward. The rebuilt bytes existed only on the runner
  and went away with it. A one-line docs push cost the whole rebuild.
- **Fix:** rebase and retry, three attempts, in the push step. `dist/` is
  generated output no human edits, so replaying our own regenerated bytes on
  top of whatever landed is always the correct merge. A rebase that genuinely
  conflicts means the arriving commits changed firmware and these bytes are
  stale regardless — that now fails loudly and asks for a re-dispatch, rather
  than pushing a rebuild of an older tree over a newer one.
- **Applies to:** any workflow that builds for minutes and then pushes to the
  branch it was dispatched on. The push is not the cheap part of that job —
  it is the only part holding the result, and it is the part most exposed to
  everyone else's timing. Treat a bot push to a live branch as contended by
  default: rebase, retry, and make the failure say which half broke. Naming
  matters too: a job called `rebuild` that fails at its push step reads as a
  build failure to every human and every notification.
- **Note for operators:** this workflow's push still does not retrigger CI
  (default `GITHUB_TOKEN`), so after it lands you owe the branch another push
  of your own — and it **must touch a path the workflow filters on** or
  nothing runs at all. `canary-local.yml` watches
  `firmware/projects/canary-display/**`, `firmware/common/**`,
  `firmware/envs/**`; `firmware.yml` watches `firmware/**`. A docs-only
  commit at the repo root matches neither and leaves the PR at zero check
  runs, which reads as "queued" rather than "never started". Don't make that
  push while a rebuild is in flight either — that is the race above. The two
  failures compose: a docs push that retriggers nothing can still land
  mid-rebuild and destroy it, costing you the build AND the signal.


### 2026-08-08 — Five Lab releases built green and shipped to nobody, because "publish" was a human click no automation could make

- **Symptom:** the Lab's self-updater, shipped in 0.2.0, had never delivered a
  single update. Nothing was red. `desktop-release.yml` had succeeded every
  time, the installers were built and signed, and `docs/RELEASE_BUTTONS.md`
  said the release was done. But `app-v0.1.0` was the only Lab release ever
  published; drafts for **0.1.1, 0.1.2, 0.2.0, 0.2.1 and 0.2.2** were all
  sitting unpublished, and there was no `lab-latest` tag at all.
- **Cause:** the Lab is released as a draft on purpose, and a draft reaches
  nobody — no git tag, no public asset URLs, and `lab-latest` (the pointer the
  app polls) never moves. Finishing it was a human clicking Publish in the
  GitHub UI. That is not a step a release button, a script, or an agent can
  perform, so every release that nobody hand-finished simply stopped there,
  looking complete. The failure mode is silence: a *successful* build is
  indistinguishable from a shipped one.
  The reason it had to be a human is real, and it is the trap anyone
  automating this will hit: **GitHub suppresses `release:` events for releases
  published with the default `GITHUB_TOKEN`** (its recursion guard). A human
  publish fires `release: published`; a CI publish does not. Both
  `desktop-lab-updater-pointer.yml` (advances `lab-latest`) and
  `release-latest-guard.yml` (puts `releases/latest` back on the firmware)
  triggered on that event — so naively adding a publish step to CI would have
  made the release public while *silently* leaving the pointer stale. That is
  worse than the original bug: it looks shipped and still updates nobody.
- **Fix:** `lab-publish.yml` — a dispatchable button that publishes the newest
  draft and then **calls** both follow-ups instead of waiting for an event that
  will not come. Two details are load-bearing:
  - it publishes with `--latest=false`, so an app release can never take
    `releases/latest` from the firmware the fleet polls, and runs
    `keep-firmware-latest` anyway because the action is idempotent;
  - it invokes the pointer as a **reusable workflow** (`uses:`), not
    `gh workflow run`. A fire-and-forget dispatch would let a broken updater
    manifest pass for a successful publish — exactly the silent failure the
    pointer workflow exists to prevent. As a `uses:` call, its failure fails
    the publish.
  It also refuses to publish a draft older than what `lab-latest` already
  serves, since publishing is irreversible and would strand the installed base.
- **Applies to:** every target that leaves a draft or defers a publish step.
  The Flasher already had this shape — `desktop-flasher-release.yml` publishes
  from CI and calls both follow-ups directly — which is why it never
  accumulated drafts. The Lab was the outlier. **If a release path ends in a
  human action, that path has an unbounded queue in front of it; give it a
  button, and make the button call whatever the human's click used to
  trigger.**

### 2026-08-08 — A compile-time flag meant to protect the simulator hid an entire subsystem from the compiler, and the App Store release was its first type-check

- **Symptom:** `ios-release.yml` failed at **Archive**, three minutes into a
  real publish, with `cannot convert value of type 'CKRecord.Reference' to
  expected argument type 'CKRecord.ID'` in `HouseholdShare.swift`. Every other
  check was green, on that commit and on the two PRs that introduced the file.
  The remaining publish steps — export, watch-app embed proof, upload, tag —
  were skipped, so the app simply did not ship.
- **Cause:** `ios/scripts/heal.sh` builds the simulator app with
  `SWIFT_ACTIVE_COMPILATION_CONDITIONS=… SECURACV_NO_CLOUDKIT`, for a good
  reason that is documented at length beside it: an unsigned build carries no
  iCloud entitlement, and constructing a `CKContainer` without one does not
  fail, it **traps** — uncatchable from Swift, so the app aborts at launch and
  every test "fails" for reasons unrelated to iCloud.
  That reasoning is about **runtime**. But `#if` does not hide code from the
  runtime — it hides it from the **compiler**. So every CloudKit path in the
  app (`HouseholdShare`, `CloudSync`, `AwayPush`'s cloud branches) was never
  compiled by CI at all, and a signed release build was its first and only
  compiler. A type error sat in `main` through two green PRs.
- **Fix:** keep the flag on the build that *runs*, and add a second pass that
  only *compiles*. `heal.sh` now builds the same scheme a second time with the
  flag absent and `build` instead of `build test`. Compiling and linking an
  unsigned app is fine — it just cannot run, and nothing runs it, so the
  `CKContainer` trap never gets the launch it needs. Own derived data
  (`build-cloudkit/`) so it cannot disturb the products the watch-app embed
  proof reads out of `build/`.
- **Applies to:** every target that compiles code out for CI — and that meant
  **tvOS today, not someday.** The first draft of this entry said tvOS "will
  need the same pass if that app grows CloudKit code"; review pointed out it
  already has, in `ResidentWatch.swift`, which carries four
  `#if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT` branches that
  `tvos.yml` has never compiled. `tvos.yml` now runs the same non-running,
  flag-free `xcodebuild build` pass.
  **The general rule: a compile-time flag that exists to protect a build from
  RUNNING code must never be the only build that TYPE-CHECKS it.** When you
  add a `#if` around a subsystem for CI's benefit, add the pass that still
  compiles it, in the same change.
  And the meta-lesson, which is why this bullet reads the way it does: when
  you write "applies to X **if** Y", go and check whether Y is already true.
  Here it was, and the phrasing would have left a live hole behind a sentence
  that sounded like diligence.

### 2026-08-06 — A shared library's host tests were linkable all along; the first project to `#include` it linked three `main()`s into the firmware

- **Symptom:** `PlatformIO Build (canary-display)` failed at the very end, in
  `ld`, with `multiple definition of 'main'` — three times over, naming
  `test_fleet_beacon.cpp`, `test_fleet_beacon_udp.cpp` and
  `test_fleet_roster.cpp`. Every one of those files is a **host test**, and none
  of them has any business being in an ESP32 image. Nothing about the change
  that triggered it went near a test or a build file.
- **Cause:** `firmware/common/fleet_link/library.json` declares
  `srcFilter: ["+<*.cpp>"]` over a **flat** directory — headers, sources and
  `test_*.cpp` all side by side — so the manifest says "compile the tests."
  That had been true since the library landed and had never once mattered,
  because no project's `#include` graph reached `fleet_link` through the LDF:
  the firmwares that use the beacon include `<fleet_beacon.h>` via a plain `-I`,
  which resolves the *header* without ever making PlatformIO treat the directory
  as a library. The moment one display source did
  `#include "fleet_link/fleet_beacon_udp.h"`, `lib_ldf_mode = deep+` discovered
  the library, honored the manifest, and swept all three tests in.
- **Fix:** `srcFilter: ["+<*.cpp>", "-<test_*.cpp>"]`. `common/csi` gets the
  same protection for free by keeping real sources under `srcDir: "src"` while
  its tests sit at the top level — a flat library has to say it explicitly.
  **Two things to carry forward:** (1) a header resolving through `-I` is *not*
  evidence that the directory is being treated as a library; those are two
  different mechanisms, and a manifest can sit wrong and harmless for months
  until the first `#include` that crosses into it. This is the mirror image of
  the `common/color` lesson (there, `-I` made headers resolve while nothing
  compiled the `.cpp`; here, the `-I` path hid a manifest that compiles too
  much). (2) The failure lands at **link**, in the last job step, minutes in,
  and names files the change never touched — budget for that when a build dies
  somewhere that looks unrelated to the diff.
  **Still open, deliberately not fixed in that change:** `firmware/common/power`
  has **no manifest at all**, so PlatformIO's default sweep compiles
  `test_power_logic.cpp` into every display image — a host test's `main()` in
  shipped firmware. It links today only because it happens to be the *sole*
  `main()`. Giving that directory a manifest is the obvious fix and wants its
  own change and its own build: `common/color` failed to link twice after
  exactly that kind of manifest edit (see CLAUDE.md), so it is not a one-liner
  to ride along with unrelated work.

### 2026-08-02 — Cutting the firmware train invalidated every committed emulator artifact, and one of them failed as a UI timeout

- **Symptom:** bumping the release train from 2.4.2 to 2.4.3 — a version-only
  change, no behavior touched — turned two green CI jobs red. One said what it
  meant (`artifact fw (2.4.2) matches registry train (2.4.3)`). The other did
  not: `VISION_PROBE_FAIL: waiting for locator('#play')`, a Playwright timeout
  that reads like a flaky browser test or a broken page.
- **Cause:** `canary-local/emulator/dist/*` are generated **and committed**, and
  each carries a `.meta.json` stamped with the firmware version it was built
  from. Two separate gates pin that stamp to `registry.json`'s `fw_train`, so
  moving the train without rebuilding `dist/` is drift by construction. The
  vision page asserts the same freshness *at runtime* (`vision-core.js`
  `assertGeneratedData`), so it threw during page init and `#play` was never
  rendered — the assertion was doing its job, but the only thing the CI log
  showed was the selector that never appeared.
- **Fix:** rebuild `dist/` at the new train before expecting green. That needs
  the pinned emsdk 6.0.3, which network-restricted environments cannot install,
  so the button is **Actions → "Rebuild emulator dist (pinned emsdk)"**,
  dispatched on the branch; it rebuilds all six flavors and pushes the result.
  **Two things to carry forward:** (1) a version bump is not a "safe" change
  here — the committed-artifact set is part of the version, so plan the rebuild
  into the same piece of work rather than discovering it from CI; and (2) that
  workflow pushes with the default `GITHUB_TOKEN`, which by GitHub's recursion
  guard **does not retrigger checks** — the refreshed head lands with zero
  checks and needs one ordinary push (or a re-run) before it can go green, which
  looks like a stuck PR if you do not expect it.
  **Generalize:** when a generated-and-committed artifact embeds a version, the
  gate that catches drift should fail with the two versions in the message. The
  node test did; the browser probe inherited its assertion but surfaced only a
  timeout. A freshness check that can fire inside a page should be reported by
  that page's harness as the assertion it is, not as an absent element.


### 2026-07-31 — A self-updater that replaces its own bundle in place, with nothing on disk to say it was interrupted

- **Symptom:** the desktop Flasher was force quit, and from then on it never
  opened again — the Dock icon bounced, no window ever appeared, and the only
  way out was another force quit. Every launch behaved identically, so it
  looked permanent, and the app said nothing because the thing that failed was
  the launch itself.
- **Cause:** `tauri-plugin-updater`'s `install` **moves the running `.app`
  bundle**. On macOS 2.10.1 the ordinary path is two `rename`s (current app out
  to a temp backup, new app in), so a kill between them leaves *no app at that
  path*; the privileged path (`rm -rf && mv` via AppleScript) is not atomic and
  can leave a partial bundle that macOS refuses to finish launching. Either
  way the repair would have to run from the copy that moved, so it can't.
  Two smaller versions of the same shape were live alongside it: sidecars
  (`espflash`, `rpiboot`) survive SIGKILL to the parent and keep holding the
  board — `rpiboot` waits for a Pi *forever* — and a force quit mid-write can
  leave the webview's `localStorage` SQLite store in a state whose recovery
  wedges before first paint. Nothing anywhere recorded that a launch had been
  attempted, so no launch could learn from the last one.
- **Fix:** `desktop/src-tauri/src/launch_guard.rs` writes a breadcrumb as each
  launch advances and reads the previous one **before `tauri::Builder`** — the
  only moment when no webview is holding the store open. An install window is
  marked on disk before the first byte is written, so an interrupted update is
  *recognized* on the next launch and the user is told to reinstall (the only
  cure) instead of guessing; sidecar PIDs are recorded while they run and
  reaped on the next launch; a launch that never reported a usable window gets
  its webview store cleared once — once, not in a loop.
  **Generalize to every app target:** (1) if a target self-updates, something
  durable must mark the window in which the bundle *moves* — and only that
  window, not the download that precedes it, or a slow connection reads as a
  broken install. Pair the marker with the running version: a marker naming the
  version you are already running means the update landed, and warning there
  tells the user to reinstall the copy they are using; (2) any app that spawns
  sidecars must record their PIDs, because a force quit is not a clean exit and
  nothing else will reap them; (3) a launch that can fail before its window
  exists needs an on-disk record — an app that can only report failures through
  its own UI cannot report the failure that matters most.
  **Still owed:** `desktop-lab/` self-updates the same way (`self_update.rs`)
  and has no install-window marker yet.

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
  recognizes rpiboot's device-open failures so the app shows the fix in-line
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

### 2026-07-30 (q) — A released S3 prints to pins nobody wired: the serial monitor was silent by build flag

- **Symptom:** "the serial monitor in the app isn't working for all
  firmwares." It worked on some boards and was silent on others, with no
  error — the port opens, the monitor connects, and nothing ever
  arrives. This also blocked diagnosing two *other* open bugs, because
  the released firmware could not tell us anything about itself.
- **Cause:** the ESP32 Arduino core decides at build time whether
  `Serial` is the USB console or UART0 on the GPIO pins. ESP32-S3 needs
  CDC-on-boot ENABLED; ESP32-C3/C6 must NOT have it (they provide
  `Serial` on USB-Serial/JTAG, and the flag prevents it). The PlatformIO
  bench had this right — `-DARDUINO_USB_CDC_ON_BOOT=1` in `common.ini`,
  undefined for C3/C6 — but the RELEASE FQBNs said `CDCOnBoot=default`
  (i.e. off) on two S3 targets and omitted the option entirely on a
  third. So every released S3 image printed to a header nobody has
  wired, while every bench build printed fine.
- **Fix:** `CDCOnBoot=cdc` on all three S3 release FQBNs, plus
  `scripts/lint_usb_console.py` in the repo-lints job: it parses the
  release FQBNs, infers the chip family, and fails when an S3 lacks
  CDC-on-boot or a C3/C6 has it — cross-checked against `common.ini` so
  the rule can't quietly enforce agreement with the wrong thing. The
  lint found the third FQBN immediately; grepping for `CDCOnBoot` had
  missed it precisely because the option was absent.
- **Applies to:** the third instance of release-vs-bench drift, after
  (i) the display firmware missing from five releases and (j) the
  core-version pairing. The pattern is always the same: the path we
  TEST and the path we SHIP are configured in different files, and
  nothing compares them. When a build knob changes behavior and lives
  in two places, write the lint that compares them — a knob whose only
  symptom is silence will not report itself.

### 2026-07-30 (r) — PR CI compiled three files out of a crate, so a merged change had no compile coverage at all

- **Symptom:** a change to `desktop/src-tauri/src/hub.rs` was reviewed and
  merged with the crate never compiled — not locally (it needs GTK/WebKit
  dev libraries that a headless container lacks) and not in PR CI. The
  first compile would have been the next release build, on `main`. It
  happened to compile; the check was luck, not process.
- **Cause:** `desktop-hub-core.yml` path-triggered on `desktop/hub-core/**`,
  `desktop/hub-io/**`, and **three individually named files** —
  `rescue.rs`, `port_hint.rs`, `health.rs` — which a `tauri-pure-modules`
  job compiles in isolation as pure modules. Everything else in the crate
  (`hub.rs`, `provisioning.rs`, `lib.rs`) matched no path and was built by
  no job. The workflow looked like desktop coverage and was coverage of a
  hand-picked subset.
- **Fix:** a `tauri-crate-check` job that installs the Linux GUI dev
  libraries and runs `cargo check --all-targets` on `desktop/src-tauri`,
  plus a `desktop/src-tauri/**` path trigger so edits anywhere in the crate
  reach it. `check`, not `build` — the type error is the point, not a
  binary.
- **Applies to:** any workflow whose paths enumerate FILES rather than the
  unit that must compile. A per-file trigger silently becomes wrong the
  moment someone adds a file, and the failure mode is not a red build —
  it is no build, which reads identically to a green one. If a crate must
  compile to ship, something in PR CI must compile the crate; and if it
  cannot be compiled locally, that is a reason to add the job, not a
  reason to trust review.

### (s) 2026-07-31 — measure the artefact before designing against it

The design note for the hub Wi-Fi seed said "parse the image's **MBR** partition
table; find partition 1 (the **FAT32** boot volume)". Both halves were wrong.
`haos_rpi5-64-18.1.img` is **GPT** behind a protective MBR, and `hassos-boot` is
**FAT16** (32695 clusters — FAT32 starts at 65525). Either mistake alone would
have produced an injector that refused the real image, with a full green test
suite behind it, because the tests would have been written from the same wrong
premise.

Ten minutes with `curl -r`, `xz -dc` and a partition-table dump settled both
before any code existed. The rule: **when a change depends on the internal
layout of an artefact you download, dump the artefact first.** Recalling how
these images are "usually" laid out is not evidence.

Related, same shape: derive a format's variant from the data (FAT width comes
from the cluster count, per the spec) rather than from a self-declared field —
the partition type byte and the `FAT16`/`FAT32` ASCII in the boot sector are
both routinely wrong.

### (t) 2026-07-31 — your own reader agreeing with your own writer proves little

`hub_fat` writes into a FAT filesystem; its unit tests read the result back with
`hub_fat`. That cannot catch a shared misunderstanding — a writer and reader that
agree on a *wrong* layout pass every test and still hand the operator a card the
Pi can't read.

So the suite hands the artefact to code that has never seen ours: `mkfs.fat`
creates the volume, `fsck.fat -n` audits it (both FAT copies, every cluster
chain), `mtools` reads the file back. That is what caught the FSInfo free-cluster
count being left "unknown", which our own reader had no opinion about.

Two traps worth repeating:

- **A skipped test reports green.** These tests skip when the external tools are
  absent, so CI installs them *and* fails the job if a `SKIP(tooling)` marker
  appears. Without that second half the check would have been a tick over
  nothing — the same silent no-op the change existed to remove.
- **Distinguish kinds of skip.** The first version of that guard failed CI,
  because the optional real-image test legitimately skips when no multi-gigabyte
  HAOS image is on the runner. `SKIP(tooling)` fails; `SKIP(optional)` doesn't.

### (u) 2026-07-31 — a workflow that pushes to your branch leaves CI gated

`emulator-dist-refresh.yml` (and any workflow that commits back — the model
regenerators do the same) pushes with `GITHUB_TOKEN`. The run it creates on the
new head does not simply start: GitHub parks it at **`action_required`**,
awaiting manual approval, because the push actor is a bot.

The failure mode is quiet and specific. The PR head now has **zero completed
checks**, so `get_check_runs` returns nothing and the PR reads as "CI hasn't
started yet" rather than "CI is waiting for you". Wait long enough and it still
says nothing. Meanwhile the *previous* head's green ticks are what a reviewer
remembers, and those were for different bytes.

What to do:

- **Treat zero completed checks on a head as a red flag, never as "too early".**
  Twice in this session a PR with no checks turned out to be a PR whose CI could
  not run — once from squash-merge divergence (`mergeable_state: dirty`), once
  from this approval gate.
- **Push a commit from your own token to un-gate it.** A normal PR push runs the
  full suite without approval. Approving the held run works too if you have the
  permission and would rather not add a commit.
- **Re-sync before committing.** That workflow pushed directly to the branch, so
  a local tree from before it is behind; commit on top of a `git fetch` +
  reset, not on top of your stale HEAD.

### (v) 2026-07-31 — the guards this session's bugs earned

Four things went wrong that CI could not have caught, and three of them now have
a guard. Recording which, because "we fixed it" and "it cannot come back" are
different claims.

**Guarded now:**

- **`scripts/lint_wifi_join_policy.py`** — no board may `ESP.restart()` on a
  Wi-Fi link that has never associated. That reboot loop shipped on the 4-inch
  display: boot join times out, board reboots, identical join fails again,
  forever, so the setup wizard that could fix the password never appears. Three
  firmwares had copy-pasted the retry logic and drifted apart, and a fourth copy
  sat in the wasm emulator under a comment claiming it was "the same as glass"
  when it no longer was. The lint checks every supervisor file, the generated
  Arduino sketch, and the emulator.
- **`release_notes.py` now rejects malformed headings.** `## 0.3.7 —
  unreleased` did not fail the check; it was **invisible** to it, parsed as *no
  section at all*. The bill arrives later, on release day, as "no section for
  0.3.7". Anything starting `## <digit>` must now parse strictly or it is an
  error immediately.
- **`check_app_versions.py` covers `Cargo.lock`** (lesson (u)'s sibling): it
  printed "in all three files ✓" over a stale lockfile — the exact bug it was
  written to prevent, one file further along.

**Not guarded, and honestly can't be by a lint:** the design note that said
"MBR / FAT32" when the image is GPT / FAT16 (lesson (s)). No static check knows
what a downloaded artefact contains. The mitigation is procedural — dump the
artefact before designing against it — plus tests that pin the *measured*
geometry so a future HAOS change fails on a runner instead of on a card.

**The shape all of these share.** Every one was a step that did nothing while
reporting success: a reboot that looked like recovery, a heading that looked
like a section, a version check that looked complete, a skipped test that
looked green. When adding a guard, the question is not "does it pass?" but
**"if the thing it guards were broken right now, would it fail?"** Both new
lints were verified by reintroducing the original bug and watching them go red.
A guard nobody has seen fail is a guard nobody has tested.

### (w) 2026-07-31 — CI timeouts are a cliff, not a slope

`PlatformIO Build (canary-display)` ran 43–44 minutes against `timeout-minutes:
45`. It passed every time, so nothing ever drew attention to it — and it was one
slow runner away from a red X that reads like a broken build but is really a
stopwatch. Raised to 90.

The asymmetry was arithmetic, not toolchain: that job builds **17** environments
(dash plus nine feature variants, dash7, watch, watch-modes, two nightstands,
touch169, playground) while every other flavor builds 2–3 and finishes in under
five minutes.

Two things worth carrying:

- **A job at >90% of its timeout is a latent red, not a pass.** If you notice
  one, treat the margin as the finding.
- **The real fix is sharding, not a bigger ceiling.** Splitting those 17 across
  parallel jobs would cut wall-clock and say WHICH environment broke instead of
  "display failed". It changes job names, which can break required-status checks
  on main — so it belongs in its own reviewable change, not bundled into an
  unrelated PR.

### (x) 2026-07-31 — a lint is code, and it needs its own tests

`scripts/lint_wifi_join_policy.py` decides whether an `ESP.restart()` is
legitimately gated by walking C++ brace depth with regexes. It took **four**
attempts to get right, and the first three were all wrong in ways that would
have gone unnoticed, because the clean tree passed each time:

1. **A proximity window missed its own bug class.** v1 accepted any restart
   with `ever_online` within 20 lines above it — which includes the lines that
   populate the `WifiRetry` struct. An ungated reboot placed just above the
   shared switch sailed straight through. A false negative on the exact defect
   the lint exists to prevent.
2. **Checking only the innermost enclosing block rejected correct code.** v2
   flagged `if (s_ever_online && …) { if (radio_ok()) { restart } }`, because
   the inner `if` says nothing about being online. **False positives are the
   worse failure**: they block correct work and train people to route around
   the check.
3. **`\bever_online\b` does not match `s_ever_online`.** `_` is a word
   character, so there is no boundary before `ever`. The real tree hid this
   because its reboot routes through `wifi_next_action`; only an adversarial
   fixture surfaced it.
4. **"Opener plus two lines above" re-admitted defect 1.** Allowing two lines
   of context for a split condition pulled `st.ever_online = s_ever_online;`
   back into scope. The fix is to reconstruct the header exactly — walk back
   only while the parentheses are unbalanced.

Defects 3 and 4 were both caught by `scripts/tests/test_lint_wifi_join_policy.py`,
which is why it exists. The tests are the fixtures, not the tree: every real
call site is one shape, and a heuristic needs the shapes that are *not* in the
tree yet.

The rules that fall out of this:

- **Write the adversarial fixture, not just the happy one.** For every guard,
  ask what the *broken* code looks like — and then also what *correct but
  unusual* code looks like, because that is where false positives live.
- **A guard nobody has watched fail is a guard nobody has tested.** Verify by
  reintroducing the original bug. If it stays green, the guard is decoration.
- **Prefer checking the property over the mechanism.** This lint asks "does
  this file speak the shared vocabulary?" rather than "does it contain this
  include line" — the latter would have forced a cosmetic edit to a file the
  committed wasm `dist/` artifacts are built from, for no behavioral gain.

### (y) 2026-08-02 — imagestack layers are declared front-to-back, and the painting order is the exact opposite

**What happened:** the tvOS icon generator wrote the imagestack `Contents.json`
layers in painting order — `[Back, Middle, Front]`, back plate first, the way
you'd composite them. Apple's imagestack format lists layers **front-to-back**:
the FIRST entry is the front-most, and actool requires the LAST entry (the back
plate) to be fully opaque. With the order inverted, our transparent bird art
was being treated as the back plate — the simulator build passed, the
structural icon gate passed (it checked presence and sizes, not order), and the
failure surfaced as a build error in PR CI: *"The last image stack layer with
content, 'Front', must be a fully opaque bitmap."* Had it not failed there, the
shipped parallax would have rendered the canary BEHIND its background.

**The fix:** `make_app_icon.py` now keeps two orders — `LAYERS` (painting
order, for compositing the flat top-shelf image) and `STACK_ORDER`
(front-to-back, for the manifest) — and `check_app_icon.py` asserts the
declared order structurally, so the bug class fails by name in seconds.

**The generalized part:** when a format's list order is semantic, the natural
order you'd *produce* the items in is a plausible-but-wrong order to *declare*
them in — and a structural gate that checks presence without checking ORDER
passes both. When you add a "does every piece exist" gate, ask what the
pieces' arrangement means, and gate that too.

### (z) 2026-08-02 — a `lipo` with one slice still produces a file called "universal"

**What happened:** a user on a 2017 Intel MacBook Pro updated to the latest
Flasher and the Pi-over-USB-C panel failed instantly with

```
could not start rpiboot: Bad CPU type in executable (os error 86)
```

The macOS app is built `--target universal-apple-darwin`, so the sidecar Tauri
bundles is the file named `rpiboot-universal-apple-darwin`. The release job
built `rpiboot` once, natively, on the arm64 runner and then did
`lipo -create -output rpiboot-universal-apple-darwin rpiboot-aarch64-apple-darwin`
— a one-slice `lipo`, which succeeds and yields a perfectly valid Mach-O that
is arm64-only. It then `cp`'d that same arm64 binary to the
`rpiboot-x86_64-apple-darwin` name. Every artifact had the right filename and
the wrong contents. The reason was real (Homebrew's libusb only ever has the
runner's own arch, so there was nothing to link an x86_64 build against) and
was written down in a code comment claiming Intel Macs would "get a clear
error" — but os error 86 is not a clear error, and the sidecar next to it
(`espflash`) had been genuinely universal all along, so nothing looked odd.

**The fix:** build libusb from a pinned tarball once per arch, build `rpiboot`
once per arch against the matching one, and `lipo` both the dylib and the
binary. Pass the architecture through `CFLAGS`, never `CC` — upstream usbboot's
`CC_FOR_BUILD ?= $(CC)` would otherwise build the `bin2c` codegen helper for
x86_64 and then fail trying to *run* it on the arm64 runner. The step now
asserts `lipo -archs` lists both slices on the universal sidecar and on the
bundled dylib, asserts each per-arch file really is that arch, and asserts the
binary still points at `@executable_path/../Frameworks/libusb-1.0.0.dylib` and
at no build-machine path. Separately, `hub_core::hub_sidecar` turns this whole
error class into a sentence with a fix in it, for every sidecar, so the next
one is legible even before anyone reads a workflow.

**The generalized part:** two of these at once. First — **`lipo -create` with a
single input is not an error**, and neither is `cp a b`; a build step that
*names* an artifact for a property has to *verify* the property, because the
filename will keep the promise long after the contents stop. `file` printing
the answer into the log is not verification; nobody reads a green step. Second
— **a known limitation recorded only in a code comment is a limitation nobody
knows about.** If a platform is genuinely unsupported on a path, the app has to
say so in words the user can act on at the moment it fails; a comment in a
workflow reaches exactly the people who don't need it. Applies to every app
target that ships a bundled binary: Flasher, Lab, and the iPhone / iPad /
tvOS / Mac targets.

### (aa) 2026-08-04 — a lesson applied to one release path, and a lint scoped to the path that was already fixed

**What happened:** entry (q) above, again, in the release path it was never
applied to. `flasher-release.yml` builds the browser flasher's factory images
— the rebuild button and the dev channel, the two ways a board gets flashed
without cutting a version — and all three of its ESP32-S3 products were still
compiled with the console on UART0: `CDCOnBoot=default` on two, the option
absent entirely on the third. The same three sketches, built by
`firmware-release.yml` in the same repo, had carried `CDCOnBoot=cdc` since (q).

So the symptom (q) describes never actually went away for the people most
likely to hit it. A board flashed from the browser flasher was silent; the
identical product from a signed release printed fine; "the serial monitor
doesn't work for some firmwares" stayed true and stayed unexplainable. #1431
("say something when the board says nothing") was work spent on the symptom.

**Cause, in two layers.** The first is ordinary: a fix went in where the bug
was noticed and nowhere else, and `flasher-release.yml`'s own header says the
two paths "can't drift" because they share `build_flash_manifest.py` — true,
and irrelevant, because they drifted *upstream* of the shared part, in the
compile flags. Sharing the back half of a pipeline proves nothing about the
front half.

The second is the one worth the entry. Entry (q) ends with "write the lint that
compares them", and that lint was written — `scripts/lint_usb_console.py`,
pointed at `firmware-release.yml`. At the file that had just been fixed. A gate
aimed at the place you already looked is green on the day you write it and
green forever after, and its greenness is indistinguishable from coverage. It
reported OK, truthfully, about the wrong half of the problem, for months, while
the artifacts users flash carried the bug it was named after.

**The fix:** all three FQBNs matched to their twins in `firmware-release.yml`.
The lint now derives its own scope instead of being handed one: a workflow is
in scope when it PUBLISHES something a user flashes (uploads a release asset,
emits `manifest-flash.json`), so a new release path inherits the rule the day
it is written. Bench-only workflows are out of scope but cannot leave silently
— one carrying an S3 FQBN that is neither publishing nor named in `BENCH_ONLY`
fails the gate. Two vacuous-pass holes closed with it: zero FQBNs found is now
a failure (restructuring the workflows so the board strings moved made every
rule pass and printed "OK — 0 ESP32 release FQBN(s) agree"), and a missing
`common.ini` is now a failure rather than a skipped half-comparison.

**The generalized part:** when you fix a release bug, the question is not "did
I fix it" but **"which other path ships this exact thing?"** — and the answer
in this repo is nearly always "two", because there are two flashers, two
release workflows, and per-target app pipelines that each rebuild the same
payloads. CLAUDE.md already says this about user-facing diagnostics ("two
flashers, two frontends"); it is just as true of build flags, which are worse,
because a flag has no UI to look wrong in.

And when you write the lint that prevents the recurrence: **point it at the
population, not at the specimen.** Scope it by what a file DOES — publishes,
builds, ships — computed from the file, so new members join the population
automatically. A hand-written list of paths is a list of the places you had
already thought of, and it is exactly as complete as your memory on the day you
wrote it. Then prove the gate can fail: give it a tree with the bug in it and
watch it go red, and give it a tree with its own inputs removed and watch it go
red for that too. A drift gate that has never been observed failing is a
hypothesis, not a control. Applies to every release target: Flasher, Lab, the
firmware paths, and the iPhone / iPad / tvOS / Mac pipelines.

### (ab) 2026-08-04 — CloudKit does not fail without an entitlement, it dies; and the second fix died the same way

**What happened:** every iOS test failed, on every branch, for four days. Not
one of them was about iCloud. The whole account CI gave was

```
SecuraCV encountered an error (Early unexpected exit, operation never finished
bootstrapping. Underlying Error: Test crashed with signal abrt before
establishing connection.)
```

**Cause:** `CKContainer.default()` resolves its container by reading
`com.apple.developer.icloud-container-identifiers`. With no entitlement it does
not return nil and does not throw — it raises an Objective-C `CKException`,
"containerIdentifier can not be nil". Swift cannot catch that. The `try?` at
the call site is on `accountStatus()`; the process is already gone inside
`default()` before the await is reached. A build with `CODE_SIGNING_ALLOWED=NO`
carries no entitlements, CI builds exactly that, and #1430 had just wired
`CloudSync.refreshAvailability()` into `FleetStore.onAppear()`.

**The first fix was wrong, and it is the useful half of this entry.** Naming
the container — `CKContainer(identifier:)` instead of `.default()` — reasons
that the exception comes from resolving a nil identifier out of a missing
entitlement, so handing it a non-nil string skips that path and any real
entitlement problem then arrives as a `CKError` on the operation, which the
existing `try?` handles. That reasoning is sound and the conclusion is false.
CloudKit logs "Significant issue at CKContainer.m:748: your process must have a
com.apple.developer.icloud-services entitlement" and then traps *inside*
`__allocating_init(identifier:)` — `EXC_BREAKPOINT`, `brk 1`, equally
uncatchable. The signal changed from abrt to trap and nothing else did. It
shipped as "unverified, selfheal is the check that matters", and selfheal duly
said no.

**Fix:** decide at COMPILE time, because every runtime test is either wrong or
unavailable. `ubiquityIdentityToken` is cheap and non-throwing but answers
about iCloud *Documents*; this app declares no ubiquity container, so gating on
it risks switching iCloud off for every real user to protect a build nobody
ships. Reading the entitlement from the code signature needs `SecTask*`, which
is not in the public iOS SDK. What IS known for certain, before the app runs,
is that an unsigned build cannot carry entitlements — so `heal.sh` sets
`SECURACV_NO_CLOUDKIT` on the same line it passes `CODE_SIGNING_ALLOWED=NO`,
and the CloudKit paths are compiled out of exactly those builds. Signed builds
never see the flag, so it cannot mask a fault where CloudKit really works.

**Applies to:** any framework whose init can trap — CloudKit, and by the same
shape anything reading entitlements at construction. Three generalizations,
each paid for here:

**A framework that traps has no runtime guard, only a compile-time one.** Once
construction itself is the thing that dies, there is no object to hold and no
error to inspect; "call it and handle the failure" is not available at any
price. Ask whether the process could *ever* succeed, answer it before the
process starts, and compile the rest away.

**When a fix changes the signal but not the outcome, that is data, not
progress.** abrt to trap was the whole result of the first attempt, and it read
as movement. The test to apply is not "did the failure change" but "did the
failure stop"; anything else invites shipping the same bug with a new symptom.

**A crash the CI cannot describe will be diagnosed by guessing.** Two rounds
went to reading source because the job discarded the `.ips` report and the
`.xcresult` before anyone could look. It now keeps both on failure — and the
very first run that did named the faulting frame in one line
(`CloudContainer.swift:62`, `CKContainer.__allocating_init(identifier:)`),
after two rounds of reasoning had gotten it wrong. Collecting the evidence is
cheaper than being clever, and it belongs in every job that can crash a
process: Flasher, Lab, tvOS, and the iPhone / iPad / Mac targets.

### 2026-08-05 — The emsdk dist rebuild raced an ordinary push and silently threw its own work away

- **Symptom:** "Rebuild emulator dist (pinned emsdk)" ran green through every
  build step — `OK: canary-wap-audio (real Canary WAP acoustic core 2.4.6-wap
  @ 7365d1c)` — then failed at the very last step with
  `! [rejected] HEAD -> <branch> (fetch first)`. The rebuilt artifacts were
  committed on the runner (`10 files changed`) and then discarded with the
  workspace. The branch still carried the *older* dist, and nothing on the PR
  said so: the only red was a job named `rebuild`, long after the bytes it
  produced had ceased to exist.
- **Cause:** the workflow checks out the branch at dispatch time and pushes
  with no rebase and no retry. Dispatching it and then continuing to push to
  the same branch — a normal thing to do while a 5-minute emsdk build runs —
  guarantees a non-fast-forward. The build cost is paid in full and the result
  is dropped on the floor.
- **Fix (operator, until the workflow retries):** treat the dispatch as taking
  a lock on the branch. Push everything you have **first**, dispatch **second**,
  and don't push again until it lands. If it does get rejected, just
  re-dispatch after your push settles — the build is deterministic, so the
  second run reproduces the same bytes. Re-dispatching is also free when
  nothing changed: the workflow's `git status --porcelain` guard exits 0 with
  "nothing to push."
- **Then remember the second half, which is worse than it first looks:** when
  it *does* push, it pushes as `GITHUB_TOKEN`, and the bot commit does not
  merely land with zero checks — it can put the **whole PR's** workflow runs
  behind a manual **`action_required`** approval gate. Measured on #1479: the
  bot's push produced one `Firmware Build` run with
  `conclusion: action_required`, and the **next four ordinary pushes created no
  runs at all** — not firmware, not CodeQL, not the secret scan, not the lint
  jobs, every one of which has a broad trigger. `get_check_runs` answers
  `total_count: 0` and the combined status stays `pending` indefinitely.
  - **This does not clear itself, and pushing again does not clear it.** An
    ordinary push is the documented remedy for the plain recursion guard; it
    is *not* the remedy for the approval gate, and assuming it is costs a
    round of confused pushes. A human with write access must click **Approve
    and run** on the pending run in the Actions tab (API:
    `POST /repos/{owner}/{repo}/actions/runs/{run_id}/approve`, which is not
    in the GitHub MCP toolset — so an agent cannot self-serve this).
  - **Read `total_count: 0` as "gated", not "queued".** A pending state with
    zero runs looks exactly like CI being slow, so the natural response is to
    wait — and waiting is the one thing that never resolves it. If a
    `workflow_dispatch` you trigger yourself runs fine while PR-triggered runs
    stay empty, that asymmetry is the tell: Actions is healthy and the PR is
    gated.
- **Applies to:** any workflow that commits and pushes generated artifacts back
  to the branch under test — the dist rebuild today, and by the same shape any
  future "regenerate and commit" button. Two properties make it safe to run
  unattended: rebase-and-retry on rejection, so a race costs a retry rather
  than the whole build; and a loud failure when the push is dropped, because a
  silently-stale generated artifact is exactly the drift the byte-diff gates
  exist to catch.

### 2026-08-07 — fw-v2.4.6 shipped a C6 nightstand factory image 5 KB too big to boot, past three green gates

- **Symptom:** a Waveshare ESP32-C6-LCD-1.47 flashed with
  `canary-display-nightstand-c6-2.4.6-factory.bin` boot-looped from first
  power-on: `esp_image: Image length 1971152 doesn't fit in partition length
  1966080`, both OTA slots "not bootable", `No bootable app partitions in the
  partition table`. The desktop Flasher had just printed "Firmware write
  verified. Flashing is complete. ✓" — truthfully: every byte was written and
  read back exactly as published. The published bytes were the defect.
- **Cause:** the app outgrew its `min_spiffs.csv` 0x1E0000 (1,966,080-byte)
  A/B slot by 5,072 bytes, and each gate that should have said so had a hole
  the exact shape of this image. (1) PlatformIO's `checkprogsize` — the
  backstop the env comment leaned on since the 2026-07-25 lean-budget work —
  sums the **ELF's** flash sections; the flashed `.bin` adds the image header,
  segment padding, and the appended hash, so a build can pass `checkprogsize`
  while its `.bin` exceeds the slot. The ~11 KB margin the lean cuts left was
  exactly the kind this gap eats. (2) flavors.json's `size_guard` — the
  byte-accurate check that stats the real `.bin` — watched ONE bin per flavor,
  and canary-display's watched the S3 watch build against a 0x330000 slot.
  (3) `make_factory.py` read the app **offset** out of the partition table and
  never looked at the **size** sitting 4 bytes away, so it happily merged an
  app into a slot it had just parsed as too small.
- **Fix:** five layers, each byte-accurate. The 4 MB C6/C3 display boards get
  their own table (`partitions_display_4mb.csv`: the unused 128 KB spiffs
  folded into the slots — state is NVS-only — growing A/B to 0x1F0000, which
  fits 2.4.6 with ~60 KB spare). flavors.json's `size_guard` became
  `size_guards`, a LIST, so every env with its own slot budget gets its own
  stat-the-bin check (nightstand-c6 guarded at the new slot; nightlight-c3
  deliberately guarded at the OLD 0x1E0000 — see below). The RELEASE path
  measures the exact staged bytes it is about to sign
  (`check_slot_budget.py`, budgets single-sourced from those same
  size_guards entries) — a tag build is not the branch build, so the PR gate
  alone could not have protected a manual dispatch or an env PR CI never
  measured; fatal for the flagship canary/wap manifests, per-variant skip in
  the vision/sense/display loops. `make_factory.py` now refuses to merge an
  app bigger than the slot it parsed, so an unbootable factory image can
  never be published (build_flash_manifest.py degrades that to "unavailable
  in the flasher", per-variant). And PARTITIONS.md states the standing rule
  this incident bought.
- **The standing rule:** growing a partition table reaches only boards that
  get a USB factory flash. OTA ships app-only images into whatever table the
  board already carries — so the moment an image exceeds the OLD slot, every
  fielded old-table board is stranded (its OTA install fails, forever) until
  a human re-flashes it over USB. For the C6 that line was crossed AT 2.4.6
  itself: old-table nightstand-c6 boards cannot take 2.4.6+ over the air and
  need one USB re-flash to pick up the new table. The C3 nightlight has not
  crossed it, which is why its guard pins the old slot: crossing must be a
  red build a human overrides, not a side effect of link-time growth.
- **Applies to:** every gate that reasons about firmware size, forever. The
  number that matters is the size of the artifact a bootloader will actually
  measure — `stat` the `.bin`; never trust an ELF-derived proxy within ~20 KB
  of a boundary. And any tool that parses a partition table for an offset and
  ignores the size beside it is a tool that will eventually write a brick.

### 2026-08-13 — the first tvOS release ever: no registered Apple TV means no development profile, reported as an auth failure

- **Symptom:** the first real run of `tvos-release.yml` (gate finally open)
  failed at `xcodebuild … archive` in seconds with TWO errors that both point
  the wrong way: *"Authentication failed: Make sure a bearer token was
  provided…"* and *"No profiles for 'com.securacv.witnesswall' were found:
  Xcode couldn't find any tvOS App Development provisioning profiles."*
  Registering the bundle ID and creating the App Store Connect app record
  changed nothing — same failure, byte for byte.
- **What the errors are NOT:** the API key was fine. The decisive experiment
  was dispatching `ios-release.yml` build-only in the same hour — the iPhone
  archived and exported cleanly with the same `APPLE_*` secrets on the same
  runner image, so auth, key role, and Xcode were all healthy. Do that
  experiment FIRST next time a store pipeline fails at provisioning: the
  sibling pipeline is a free control group.
- **Cause:** under `CODE_SIGN_STYLE: Automatic`, `xcodebuild archive` signs
  with an "Apple Development" identity, and cloud-signing therefore has to
  mint a *tvOS App Development* provisioning profile — which requires at
  least one **registered Apple TV device** on the team. This team has
  iPhones registered (which is the only reason the iOS archive step ever
  worked) and no Apple TV, so Apple refuses the profile, and Xcode 26
  renders the refusal as the bearer-token error above.
- **The fix that does NOT work:** overriding `CODE_SIGN_IDENTITY="Apple
  Distribution"` on the archive command. Automatic signing rejects it by
  name — *"conflicting provisioning settings … automatically signed for
  development, but a conflicting code signing identity Apple Distribution
  has been manually specified"* — because automatic signing owns the
  identity choice and will not be argued with one setting at a time.
- **The fix:** store builds archive **unsigned**
  (`CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO`, passed by
  `tvos/scripts/release-tvos.sh` for every non-debugging export method), and
  `-exportArchive` signs from scratch with the App Store distribution
  profile — which needs no registered devices, and which the account
  demonstrably mints (the iPhone export does it on every release; export
  re-signs every archive anyway, so nothing store-bound ever shipped
  carrying archive-time signatures). The export pins
  `signingCertificate = Apple Distribution` like the iPhone's does, per (m)
  — doubly load-bearing here, since that export is now the only signing
  pass a store build gets. The debugging export keeps development signing
  end to end, because a development build is the one thing that genuinely
  needs it.
- **Why iOS was left alone:** its archive works today only because iPhones
  happen to be registered — the same latent fragility, one deregistration
  away. It was deliberately NOT switched in the same change: its archive
  carries the entitlements dance (`SECURACV_APP_ENTITLEMENTS`, Critical
  Alerts) that this fix was not tested against, and changing a shipping
  pipeline as a side effect of fixing a broken one is how (m) happened. If
  the iPhone ever fails at archive with this signature, this entry is the
  diagnosis and the tvOS script is the template.
- **Applies to:** any NEW Apple platform target this repo grows (visionOS,
  a Mac Catalyst split, …). The first release of a new platform runs on an
  account that has never registered a device of that platform — so archive
  distribution-signed from day one, and expect the first provisioning
  failure to blame authentication.

### 2026-08-13 (b) — the tvOS home-screen icon needs every layer at @2x, and the gate that should have said so had never actually run

- **Symptom:** the first tvOS publish attempt with working signing got a
  signed `.ipa` all the way to App Store Connect validation, which rejected
  it: *"Invalid Image Asset. The image asset 'App Icon' … is missing an
  image for the background layer with a scale value of '2'." (90709)* —
  minutes after a green build, in the last step before upload.
- **Cause, in two halves:**
  1. `make_app_icon.py` emitted every imagestack layer at @1x only. The
     top-shelf images already did @1x+@2x; the layered home-screen icon
     (400×240 base) needs the same per LAYER, and nothing in the repo knew.
  2. `check_app_icon.py` — whose whole job is naming this in CI before
     Apple names it at upload — had its per-layer size checks indented one
     level too deep, INSIDE the wrong-layer-order error branch. So the
     layer images were only checked when the catalog was already broken a
     different way. The gate existed, passed for months, and had never
     checked a single layer PNG.
- **Fix:** the generator takes scales per stack — home-screen `(1, 2)`,
  App Store 1280×768 `(1,)` per Apple's spec — and the checker requires
  every layer PNG at every scale AND the `Contents.json` scale declarations
  to match, with the loop de-indented so it actually runs. Verified by
  running the fixed checker against the old catalog (6 named failures) and
  the regenerated one (green).
- **Applies to:** every generated-asset gate. A checker that shares an
  error branch with another check is a checker that may never have run —
  when a gate is added, break the thing it guards ONCE and watch it go red
  (the same rule (x) states for lints). And upload-validation errors are
  the gate's requirements list: every 90xxx Apple has ever thrown at a
  target belongs in that target's preflight checker.

### 2026-08-13 (c) — the Flasher bakes its catalog in, and the release plan watched only where the code lives

- **Symptom:** firmware 2.4.9 (the first SIGNED release) shipped, the site's
  in-browser flasher picked it up on the next pages deploy — and the desktop
  Flasher kept installing 2.4.8, because its catalog is `include_str!`-baked
  at build time and the published 0.11.1 binary was built before 2.4.9
  existed. "Update everything" reported the Flasher `up_to_date` the whole
  time, truthfully by its own rules: the target watched `desktop/` and the
  catalog lives in `canary-local/devices/`.
- **Why it cost real time:** a user who flashed displays from the Mac app
  minutes after the firmware release got 2.4.8 — and 2.4.8 devices carry the
  all-zeros OTA key, which hard-disables OTA, so every one of them needs a
  USB re-flash that a current Flasher would have made unnecessary.
- **Fix:** the flasher target's `watch:` list now includes the exact files
  `build.rs` embeds (`flash.json`, `hatch.json`), so the next firmware
  release flips the Flasher to `needs_bump` in the plan summary instead of
  `up_to_date`. And 0.11.2 ships the 2.4.9 catalog.
- **Applies to:** every versioned target whose build EMBEDS files from
  outside its own tree. The watch list must cover what the build reads, not
  just where the code lives — `grep include_str!`/`cp` lines in the build
  script when adding a target, and add each embedded path to `watch:`.
  (The tvOS/iOS apps are safe today — their only cross-tree embeds are
  Swift sources listed in project.yml, inside watched paths.)

### 2026-08-20 — a non-blocking build matrix let products vanish from a release with only a ::warning

- **Symptom:** the firmware-release build matrix degrades most per-product
  steps (display SKUs, vision variants, the reach ports) to `::warning …
  absent from this release` on a failed compile — by design, so one broken
  SKU cannot sink the signed release around it. The unpriced half of that
  bargain: nothing ever compared release N+1's product set to release N's,
  so a board that shipped last month could silently drop out of
  `manifest-flash.json` and the first person to notice would be its owner,
  reflashing.
- **Fix:** `firmware/scripts/check_manifest_completeness.py` runs after the
  manifest build and BEFORE anything publishes: it diffs the fresh
  `manifest-flash.json` product ids against the previous stable release's
  (downloaded via `gh release download`), fails on any product that used to
  ship and is now missing, and prints the shipped/added/dropped table into
  the step summary. The `allow_dropped_products` workflow_dispatch input is
  the explicit "yes, ship without it" sign-off; dev-channel prereleases run
  the same diff in advisory mode. First release (no previous manifest)
  skips cleanly.
- **Applies to:** every release path built as a non-blocking matrix. A
  step that degrades a failure to a warning needs a paired gate that diffs
  the OUTPUT SET against the last shipped release — warnings scroll away,
  a missing product id in a set comparison does not. When adding such a
  gate, feed it a doctored manifest once and watch it go red (rule (x)).

### 2026-09-03 — the Flasher's build tool was resolved fresh on every release runner

- **Symptom:** `desktop/` had no committed `package-lock.json`. The Flasher
  release workflow ran `npm install`, so each run took whatever
  `@tauri-apps/cli@^2` resolved to that day: two builds of the same tag could
  bundle with different CLI versions, and a CLI regression would have arrived
  on the release path with no diff to point at. Because there was no
  lockfile, `npm audit` (audit.yml) and Dependabot both skipped `desktop/` —
  each said so in a comment. The Lab (`desktop-lab/`) already had its lockfile
  and `npm ci`; the Flasher was the odd one out.
- **Fix:** `desktop/package-lock.json` is committed, generated with
  `npm install --package-lock-only --ignore-scripts`. That form records every
  platform's optional `@tauri-apps/cli-*` binary (darwin arm64/x64, linux,
  win32), so a lockfile made on Linux installs on the macOS runners too.
  `desktop-flasher-release.yml` runs `npm ci` with the npm cache keyed on the
  lockfile, `audit.yml` audits it alongside canary-vision and desktop-lab, and
  dependabot.yml has an npm entry for `/desktop`. `desktop/` is already in the
  Flasher target's `watch:` list, so a Dependabot bump of the lockfile shows
  the Flasher as ahead in the release plan — the same way a
  `desktop/src-tauri/Cargo.lock` bump does today.
- **Applies to:** every Tauri target. "Pin-or-log every upstream ref" covers
  the tool that builds the bundle, not only what goes into it: a
  `package.json` with a caret range and no lockfile is an unpinned upstream
  ref on the release path. When adding an app target, check that its install
  step is `npm ci` against a committed lockfile, and that the lockfile is in
  audit.yml's matrix and paths and in dependabot.yml.

### 2026-09-04 — the vision model release signed whatever the CDN served

- **Symptom:** `vision-model-release.yml` fetched the source person-detection
  model from Seeed's CDN with a bare `curl`, compiled it, and signed the result
  with the project's Ed25519 OTA release key — so the bytes the CDN happened to
  serve on release day became a release-key-signed asset that the browser
  flasher's fail-closed signature check (`verifyPinnedModelAsset`) would then
  trust. The only checks on the input were structural (the TFL3 magic, an
  Ethos-U command-stream string, the input tensor shape), which any
  malicious-but-valid model passes. The compiler on the same path
  (`pip install ethos-u-vela`) was unpinned too.
- **Fix:** `MODEL_INT8_SHA256` sits next to `ZOO_BASE`/`MODEL_STEM` and is
  checked with `sha256sum` right after the download; the compiler is pinned
  (`VELA_VERSION`). The pin is REQUIRED: an empty value fails the run and
  prints the hash the CDN served, so pinning is a deliberate act with the
  bytes in hand, never an accident of what was up that day. Recompute both
  when bumping the model or the compiler, exactly as `ESPFLASH_VERSION` /
  `ESPFLASH_SHA256_*` prescribe for the flash engine. The first pin was
  taken exactly that way: a dispatched run on the fixing branch printed the
  served hash, and that hash went into the file with the run id beside it.
  Seeed publishes no digest of its own, so this is a first-use pin — the
  guarantee is that the bytes cannot change under us from here on, not that
  the first bytes were independently attested. Say which one you have.
- **Applies to:** every step between "download something" and "sign
  something" on any release path. A signature is a statement about bytes we
  chose; an unpinned download is bytes someone else chose. If an input comes
  from a third party, pin its hash (or verify a signature of theirs) before
  it reaches our key, and pin the tool that transforms it.

### 2026-09-04 — the signing jobs installed their Python toolchain unpinned

- **Symptom:** `firmware-release.yml` and `flasher-release.yml` ran
  `pip install --upgrade platformio cryptography` (plus `intelhex` and an
  `esptool>=4,<5` range) in the same job that writes the OTA release private
  key to disk and signs every fielded image. `--upgrade` guarantees a fresh,
  unreviewed PyPI resolution on every release, and installing a package runs
  its code on the runner holding the key. The 2026-09-03 lockfile lesson
  covered npm; the Python side of the same jobs was still floating.
- **Fix:** exact `==` pins on all four packages in both workflows, chosen to
  match what the unpinned install resolved to on the day, so the change is a
  freeze, not an upgrade. Bump them on purpose, together, when the toolchain
  needs to move.
- **Applies to:** every `pip install` in a job that touches a signing secret.
  A version range on a key-bearing runner is an unpinned upstream ref exactly
  like a caret in `package.json`. Dependabot does not see inline pins in
  workflow `run:` blocks, so a bump here is a deliberate maintainer action —
  which, on a signing path, is the point.
