# SecuraCV Lab — desktop app

A native **Mac & Linux** application that wraps the local-first
[`canary-local`](../canary-local) Lab in a [Tauri](https://tauri.app) shell.
It runs the real firmware emulator, 3D device cards, and fix-it flows
entirely on your machine — it **talks only to your own devices**; the one
thing it fetches on its own is its update manifest from GitHub (15 s after
launch, then every 6 h) — every update it offers is signature-verified
before install — which is exactly the
point of a security product.

> **Sibling app.** [`../desktop`](../desktop) is the **SecuraCV Flasher** — a
> focused native tool that flashes a Canary over USB with bundled `espflash`
> (no browser, no Web Serial). This app is the broader one: the **whole Lab**
> in a native window. They're complementary and ship as separate installers.

## Why native (and why Tauri)

The Lab is already local-first web + WebAssembly, so wrapping it is natural.
Native earns its keep for the things a browser can't do well:

- **Reliable USB flashing** — WebSerial is Chromium-only and flaky; native
  serial (Rust `serialport`) is rock-solid. *(Phase 2 — stubbed today.)*
- **Device discovery** — mDNS + Bluetooth LE to the Canaries. *(Phase 2.)*
- **An always-on menubar companion** — live fleet status, native
  notifications on signed events, the tamper-evident timeline. *(Phase 2.)*

Tauri gives us all of that from **one frontend** shared with the website: a
~5 MB signed binary on the OS WebView, minimal attack surface (Rust) — a
better fit for a privacy brand than a 100 MB Chromium bundle.

The **capability layer** is the seam: in the browser it maps to WebSerial /
`fetch`; in the app it maps to Rust commands (see `src-tauri/src/lib.rs` →
`native_capabilities`). One spine, native per surface.

## Layout

```
desktop-lab/
  package.json                 @tauri-apps/cli
  frontend-stage.json          what the app's web root mirrors (shared with CI)
  scripts/stage-frontend.mjs   builds dist/ from it, before every dev/build
  dist/                        the staged web root (generated, gitignored)
  src-tauri/
    tauri.conf.json            frontendDist -> ../dist, window -> canary-local/lab.html
    Cargo.toml
    build.rs
    capabilities/              default.json + desktop.json (self-update perms)
    icons/                     generated from mascot.png
    src/{main,lib}.rs          native shell + capability seam
    src/self_update.rs         signed self-update (desktop only, see below)
```

## Self-update

The desktop Lab keeps itself fresh the same way the Flasher does (the shape
`RELEASE_LESSONS.md` 2026-07-27 prescribed): it checks its release channel
shortly after launch and every six hours while open, polling the rolling
**`lab-latest`** prerelease pointer — its own pointer; `releases/latest`
belongs to the firmware the fleet polls. When a newer signed build exists, a
native dialog shows that release's own notes (`RELEASE_NOTES.md`, the newest
section) and asks; nothing installs without a yes. Every check and install is
appended to `update-journal.log` in the app's data dir — visible, local,
never silent. iOS/iPadOS builds compile the updater out (`#[cfg(desktop)]`);
the App Store owns updates there. The pointer advances only when a human
publishes the draft release (`desktop-lab-updater-pointer.yml`), and only
after every URL in the hardened manifest resolves.

The frontend is **not built** — `canary-local/` is a static, committed bundle
(vanilla JS + committed WASM `dist/`), so Tauri packages it directly. It is
**staged**, though, and that distinction matters: Tauri serves `frontendDist` as
the whole origin, so anything the pages reach for *outside* `canary-local/` —
above all the enclosure library at `docs/hardware/enclosure/`, which every
preview render, STL mesh and `.scad` download comes from — has nowhere to
resolve if `canary-local/` is itself the root. `scripts/stage-frontend.mjs`
mirrors those siblings at their repo-relative paths into `dist/`, so the app's
web root has the same shape as the deployed site's, and one set of relative URLs
works on both. The mirror list is `frontend-stage.json`;
`canary-local/tests/lab_bundle.test.js` fails CI if the frontend starts reaching
for something the manifest doesn't carry.

## Develop

```bash
cd desktop-lab
npm install
npm run dev      # stages dist/, then opens the Lab in a native window
```

`npm run dev` re-stages on start, so an edit to `canary-local/` needs the dev
window restarted (or `npm run stage` and a reload) to show up — the app reads
`dist/`, not the source tree.

Linux dev deps (Ubuntu/Debian):

```bash
sudo apt-get install -y libwebkit2gtk-4.1-dev libgtk-3-dev \
  libayatana-appindicator3-dev librsvg2-dev patchelf
```

## Build installers

```bash
cd desktop-lab
npm run build
# → src-tauri/target/release/bundle/{dmg,macos,appimage,deb}/…
```

## Release pipeline

Pushing a tag like `app-v0.1.0` runs
[`.github/workflows/desktop-release.yml`](../.github/workflows/desktop-release.yml),
which builds macOS (universal `.dmg`/`.app`) and Linux (`.AppImage`/`.deb`)
and publishes them to a **draft GitHub Release**. Bump `version` in **all three**
files that must agree — `package.json`, `src-tauri/tauri.conf.json`, and
`src-tauri/Cargo.toml` (the release gate `../desktop/scripts/check_app_versions.py`
fails the build if any drift) — tag, and the installers appear on the
[releases page](https://github.com/kmay89/securaCV/releases) — which is where
the website's Download page sends people.

```bash
# cut a release
git tag app-v0.1.0 && git push origin app-v0.1.0
```

You can also run the workflow manually (**Actions → Desktop app release →
Run workflow**): leave **publish** unchecked for a throwaway build-only smoke
test, or check it to cut a draft release. Either way both installers are
attached to the workflow run as artifacts (14-day retention), so you can grab
and test a build without touching the Releases page. CI installs with
`npm ci` against the committed `package-lock.json` for reproducible builds.

### One-button Mac app build/release

To build both native Mac apps at once:

1. Open [**Actions → Build Mac apps (Flasher + Lab)**](https://github.com/kmay89/securaCV/actions/workflows/mac-apps-release.yml).
2. Click **Run workflow**, choose the **main** branch, and leave **publish**
   unchecked for build-only smoke artifacts.
3. Check **publish** only when you want to publish the Flasher release and
   create the SecuraCV Lab draft release.

GitHub only shows a newly added workflow in the Actions sidebar after its file
exists on the repository's default branch. The launcher is now on **main**. If
the entry is still missing, refresh the Actions page; as a fallback, run
**Desktop Flasher — build & release** and **Desktop app release** separately.

## Roadmap

1. **This** — Tauri shell of the Lab, Mac/Linux installers, release pipeline.
2. **Native USB flashing** (`serialport`) — replace WebSerial; the biggest
   reliability win. Bundle `esptool`.
3. **Menubar fleet companion** — mDNS/BLE status, native notifications on
   signed events, the signed timeline.
4. Extend the same shared core to **iOS/iPad** (Tauri v2 mobile — **scaffolded**,
   see [`MOBILE.md`](MOBILE.md) or the rendered walkthrough
   [`ipad-guide.html`](ipad-guide.html); needs your Apple Developer account to
   build) and a **Home Assistant add-on**.

## Signing & notarization

The pipeline ships unsigned artifacts by default — signing is **opt-in** so a
stray or invalid cert can never break the build. To codesign + notarize macOS
(so it opens without the Gatekeeper warning):

1. Add the Apple signing secrets the desktop apps read
   (`APPLE_DESKTOP_CERTIFICATE`, `APPLE_DESKTOP_CERTIFICATE_PASSWORD` — a
   **Developer ID Application** .p12, deliberately a different name from the
   iPhone pipeline's `APPLE_CERTIFICATE`; plus `APPLE_SIGNING_IDENTITY`, `APPLE_ID`,
   `APPLE_PASSWORD`, `APPLE_TEAM_ID`).
2. Set the repository **variable** `ENABLE_MACOS_SIGNING` to `true`
   (Settings → Secrets and variables → Actions → **Variables**).

Until that variable is `true`, the macOS build is intentionally unsigned and
the secrets are ignored.
