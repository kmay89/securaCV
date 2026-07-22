# SecuraCV Flasher (desktop)

The [SecuraCV Lab](https://securacv.com/lab) as a small native app. It detects
your ESP32 Canary over USB-C, downloads the **signed** factory image from the
project's GitHub release, and writes it — with **no browser, no Web Serial, no
PlatformIO, no terminal**. Like the web flasher, you can't pick the wrong image
(it reads the chip first) and you can't brick the board (the ESP32's first-stage
bootloader is mask ROM).

## Why this exists

The in-browser flasher is great but needs a **Chromium** browser, because
flashing over USB uses the Web Serial API, which only Chrome/Edge/Brave ship —
never Safari or Firefox, and never anything on iOS/iPadOS. Going native removes
the browser from the loop entirely: serial comes straight from the OS and the
flash is driven by [`espflash`](https://github.com/esp-rs/espflash), esptool's
Rust sibling, bundled into the app. That matches the rest of this repo, which is
Rust-first.

> **Scope note.** A `.dmg` is macOS-only, and iOS/iPadOS **cannot** flash an
> ESP32 over USB at all — Apple blocks generic USB-serial access for third-party
> apps. So this app targets **macOS and Linux** (and builds for Windows for
> free). For iPad/iPhone, the browser Lab installs as a PWA and covers
> everything *except* USB flashing.

## How it's built

| Piece | Choice | Why |
|---|---|---|
| Shell | **Tauri v2** | ~5–10 MB, uses the OS WebView, Rust backend — fits this repo |
| Flash engine | **espflash** (sidecar) | native ESP flashing, no browser; stable CLI, not a fragile library binding |
| Serial | `serialport` crate | OS-native port enumeration |
| Front-end | plain HTML/CSS/JS in `src/` | no build step, mirrors the Lab's look |
| Self-update | `tauri-plugin-updater` | checks GitHub releases, one-click update |
| Catalog | `canary-local/devices/flash.json` | embedded fresh every build by `build.rs` (via `OUT_DIR`) — no committed copy to drift; the chip guard works offline |

The Rust commands live in `src-tauri/src/lib.rs`: `load_catalog`, `list_ports`,
`detect_chip`, `fetch_manifest`, `flash`, `check_update`, `install_update`.

## Layout

```
desktop/
├── src/                     # front-end (index.html, styles.css, app.js)
└── src-tauri/
    ├── src/                 # main.rs, lib.rs (the flashing backend)
    ├── build.rs             # embeds canary-local/devices/flash.json fresh each build
    ├── binaries/            # espflash sidecars (CI-populated, git-ignored)
    ├── icons/               # source.png + generator (set built by CI)
    ├── dmg/                 # branded installer-window background + generator
    ├── capabilities/        # Tauri permission scopes
    └── tauri.conf.json
```

## Releasing

CI does the whole thing — see `.github/workflows/desktop-flasher-release.yml`.
Bump `version` in `src-tauri/tauri.conf.json` and `package.json`, then push a
matching tag:

```sh
git tag flasher-v0.1.0 && git push origin flasher-v0.1.0
```

The workflow downloads the espflash sidecars, builds a **universal** macOS
`.dmg` plus Linux `.AppImage`/`.deb`, and publishes a GitHub release with the
`latest.json` self-update manifest. You can also run it from the Actions tab
(**Run workflow**) for a smoke build.

### One-button Mac app build/release

If you want both native Mac apps at once, use **Actions → Mac apps —
one-button build/release → Run workflow**. Leave **publish** unchecked for a
build-only smoke run, or check it to publish releases for both this Flasher app
and the sibling SecuraCV Lab app.

### One-time: real self-update signing (recommended)

Self-update artifacts are signed. Until you set a persistent key, CI mints an
**ephemeral** one per run so builds still succeed — but cross-release updates
won't verify. To make self-update work for good:

1. Generate a keypair (needs the Tauri CLI once): `npx @tauri-apps/cli signer generate -w ~/.tauri/securacv-flasher.key`
2. Put the **public** key into `plugins.updater.pubkey` in `src-tauri/tauri.conf.json`.
3. Add repo secrets **`TAURI_SIGNING_PRIVATE_KEY`** (the private key file's
   contents) and **`TAURI_SIGNING_PRIVATE_KEY_PASSWORD`**.

### Optional: notarized macOS (zero-step install)

To drop the one-time `xattr` step in `INSTALL.md`, add an Apple Developer
signing identity + notarization credentials to the `tauri-action` step
(`APPLE_CERTIFICATE`, `APPLE_ID`, `APPLE_PASSWORD`, `APPLE_TEAM_ID`). Costs
$99/yr; everything else stays the same.

## Local development

```sh
cd desktop
npm install
# put an espflash binary at src-tauri/binaries/espflash-<your-target-triple>
npx @tauri-apps/cli icon src-tauri/icons/source.png   # source.png is committed
npm run dev
```

Find your target triple with `rustc -vV | grep host`.
