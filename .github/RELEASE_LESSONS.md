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
6. **A new board reaches the flasher only when a release carries it.** The
   in-browser flasher lights a product up from `manifest-flash.json` in the
   release it reads (`releases/latest`, or `fw-dev-latest` via
   `?channel=dev`). Adding a board to `flash.json` + the release workflows is
   necessary but *not sufficient* — the product stays "unavailable" until the
   **next firmware release is actually cut**. After adding a board, cut a
   release (one-click above) or it will never appear, no matter how correct
   the wiring is.

## Entries

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
