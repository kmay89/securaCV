# SecuraCV Flasher (desktop)

The [SecuraCV Lab](https://securacv.com/lab)'s flashing bench as a small
native app. (In-window it brands itself **Flasher** — the sibling
[`desktop-lab/`](../desktop-lab) app owns the name "SecuraCV Lab", and one
name per surface is the rule.) It detects
your ESP32 Canary over USB-C, downloads the official factory image from the
project's GitHub release, verifies its size and SHA-256 (and its Ed25519
signature when a non-placeholder release key is pinned), provisions the board,
and writes it — with **no browser, no Web Serial, no PlatformIO, no terminal**.
Like the web flasher, it reads the chip before exposing compatible images.

The Vision flow also recognizes the Grove Vision AI V2 module's CH343 USB
port. It preserves the ESP32 receipt while the cable is moved, writes the
pinned model with the WE2 ROM/XMODEM protocol, and requires an AT response plus
one inference before reporting that the Canary hatched.

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
| Serial | `serialport` crate | OS-native enumeration, persistent monitor, and public device receipt capture |
| Vision module | native WE2/XMODEM engine | verifies and burns the pinned model, then proves AT + inference |
| Front-end | plain HTML/CSS/JS in `src/` | no build step, mirrors the Lab's look |
| Self-update | `tauri-plugin-updater` | checks GitHub releases, one-click update |
| Catalog | `canary-local/devices/flash.json` | embedded fresh every build by `build.rs` (via `OUT_DIR`) — no committed copy to drift; the chip guard works offline |

The command registration lives in `src-tauri/src/lib.rs`. Release verification,
NVS provisioning, the serial monitor/receipt parser, and the WE2 engine are
split into `release.rs`, `provisioning.rs`, `serial_monitor.rs`, and `we2.rs`.

For `usb-secrets` images, Wi-Fi and MQTT values are patched into the ESP32 NVS
partition only after the untouched release image verifies. Passwords are not
logged or serialized, and the UI clears them after a successful write. The
patched image is handed to `espflash` through an atomically-created, randomly
named private temporary file (mode 0600 on Unix) that is removed on every
ordinary exit path. The host receipt records both the official release hash
and the installed, device-specific hash.

## Layout

```
desktop/
├── src/                     # front-end (index.html, styles.css, app.js)
└── src-tauri/
    ├── src/                 # flashing, verification, provisioning, serial, WE2
    ├── build.rs             # embeds canary-local/devices/flash.json fresh each build
    ├── binaries/            # espflash sidecars (CI-populated, git-ignored)
    ├── icons/               # source.png + generator (set built by CI)
    ├── dmg/                 # branded installer-window background + generator
    ├── capabilities/        # Tauri permission scopes
    └── tauri.conf.json
```

## Releasing

CI does the whole thing — see `.github/workflows/desktop-flasher-release.yml`.
Bump `version` in **all three** files that must agree — `src-tauri/tauri.conf.json`,
`package.json`, and `src-tauri/Cargo.toml` (the release gate
`scripts/check_app_versions.py` fails the build if any drift) — then push a
matching tag:

```sh
git tag flasher-v0.1.0 && git push origin flasher-v0.1.0
```

The workflow downloads the espflash sidecars, builds a **universal** macOS
`.dmg` plus Linux `.AppImage`/`.deb`, and publishes a GitHub release with the
`latest.json` self-update manifest. You can also run it from the Actions tab
(**Run workflow**) for a smoke build.

### One-button Mac app build/release

To build both native Mac apps at once:

1. Open [**Actions → Build Mac apps (Flasher + Lab)**](https://github.com/kmay89/securaCV/actions/workflows/mac-apps-release.yml).
2. Click **Run workflow**, choose the **main** branch, and leave **publish**
   unchecked for a build-only smoke run.
3. Check **publish** only when you want to publish the Flasher release and
   create the sibling SecuraCV Lab draft release.

GitHub only shows a newly added workflow in the Actions sidebar after its file
exists on the repository's default branch. The launcher is now on **main**. If
the entry is still missing, refresh the Actions page; as a fallback, run
**Desktop Flasher — build & release** and **Desktop app release** separately.

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
(`APPLE_DESKTOP_CERTIFICATE`, `APPLE_ID`, `APPLE_PASSWORD`, `APPLE_TEAM_ID` —
desktop-only cert name so the iPhone pipeline's `APPLE_CERTIFICATE` can't
clobber it; see `SIGNING.md`). Costs
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

## Verification

From `desktop/src-tauri/`, run `cargo test --lib`,
`cargo clippy --lib -- -D warnings`, and
`RUSTDOCFLAGS=-Dwarnings cargo doc --no-deps`. Build the paired host firmware
with `pio run -e canary-vision-xiao-c3` from the repository root.
