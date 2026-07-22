# SecuraCV Lab — desktop app

A native **Mac & Linux** application that wraps the local-first
[`canary-local`](../canary-local) Lab in a [Tauri](https://tauri.app) shell.
It runs the real firmware emulator, 3D device cards, and fix-it flows
entirely on your machine — **nothing phones home**, which is exactly the
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
  src-tauri/
    tauri.conf.json            frontendDist -> ../../canary-local (bundled as-is)
    Cargo.toml
    build.rs
    capabilities/default.json
    icons/                     generated from mascot.png
    src/{main,lib}.rs          native shell + capability seam
```

The frontend is **not built** — `canary-local/` is a static, committed bundle
(vanilla JS + committed WASM `dist/`), so Tauri packages it directly.

## Develop

```bash
cd desktop-lab
npm install
npm run dev      # opens the Lab in a native window
```

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
and publishes them to a **draft GitHub Release**. Bump `version` in both
`package.json` and `tauri.conf.json`, tag, and the installers appear on the
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

To build both native Mac apps together, use **Actions → Build Mac apps
(Flasher + Lab) → Run workflow**. Leave **publish** unchecked for build-only
smoke artifacts, or check it to publish the Flasher release and create the Lab
draft release in one click.

If you do not see that workflow in Actions yet, it has not landed on the
default branch. Until it does, run the two existing workflows separately:
**Desktop Flasher — build & release** and **Desktop app release**.

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

1. Add the Apple signing secrets that `tauri-action` reads (`APPLE_CERTIFICATE`,
   `APPLE_CERTIFICATE_PASSWORD`, `APPLE_SIGNING_IDENTITY`, `APPLE_ID`,
   `APPLE_PASSWORD`, `APPLE_TEAM_ID`).
2. Set the repository **variable** `ENABLE_MACOS_SIGNING` to `true`
   (Settings → Secrets and variables → Actions → **Variables**).

Until that variable is `true`, the macOS build is intentionally unsigned and
the secrets are ignored.
