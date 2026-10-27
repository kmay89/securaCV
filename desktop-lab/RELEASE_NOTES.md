# SecuraCV Lab — release notes

What changed **for the person using the app**, one section per released
version, newest first. Same contract as the Flasher's
`desktop/RELEASE_NOTES.md`:

- The release workflows put the newest section into the release body, so
  the human publishing the draft (and everyone downloading it) reads what
  the version changes.
- The self-updater's manifest carries the same section as its `notes`, so
  the Lab's "an update is ready" dialog says what's changing before you
  agree to install.
- `desktop/scripts/release_notes.py check` (lint + both app release
  workflows) fails if the newest section doesn't match the version in
  `src-tauri/tauri.conf.json`.

Heading grammar is `## <version> — <YYYY-MM-DD>`.

## 0.3.3 — 2026-10-27

**The 2.4.18 firmware catalog is baked in, a board on 2.4.18 says what it
runs, and an image the board could not start is refused before anything is
written.**

- **Firmware 2.4.18 on the Flash page.** On a Canary WAP, after setup,
  changing its Wi-Fi, rebooting it, registering a vault key or confirming a
  pairing takes its API token: save its recovery kit before you update one
  you cannot easily reach. Update both units of a WAP Opera before pairing
  them. Built by CI; not bench-tested.
- **A board on 2.4.18 or later says what it runs.** Its image now carries
  its product and version, so the Flash page reads it as, say, Canary WAP
  2.4.18-wap, and the install verdicts work on it: update, reinstall,
  downgrade, or switch from another product. A board on 2.4.17 or older
  still reads as an Arduino sketch until it updates. Host-tested; no
  stamped image has been flashed to a board yet.
- **ESP32-CAM and ESP32 DevKit images start.** fw-v2.4.17's two images for
  these boards put the bootloader where a classic ESP32 does not look, and
  0.3.2 wrote them anyway. 2.4.18's are laid out right: if one of these
  boards did not start after a 2.4.17 install, install 2.4.18 from here.
- **An image laid out for another chip is refused before anything is
  written,** from the catalog or a local file, on the Flash page and on the
  native bench, with the same message as the Flasher.
- **The fake-capacity check reads 8 KB, not 4,** so a correctly installed
  classic ESP32 no longer reads as blank.
- **Connecting no longer starts the board's own firmware** unless you say
  it is already plugged in: a board you held BOOT on stays in download
  mode until the install. Asking the board itself and the serial monitor
  still start it on a click.
- **The dev-channel banner says where the dev channel stands** against the
  release this app carries (newer, the same version, or older) instead of
  calling it ahead.
- **The display benches run 2.4.18's firmware,** so the splash bird sits
  clear of its bubble.
- Under the hood: Tauri 2.12.5, tokio 1.54.1.

## 0.3.2 — 2026-10-27

**The 2.4.17 firmware catalog is baked in, the monitor after a flash takes
commands, and every printable part says which revision it is.**

- **Firmware 2.4.17 on the Flash page.** Setup networks are named for the
  kind of device (`SecuraCV-sense-XXXX`), and a Sense or Vision can take
  its Home Assistant link from iPhone app 0.8.0. From 2.4.17 on, an update
  for another board is refused before it downloads, and one that has not
  run healthy for a minute is rolled back at the next restart. Host-tested
  and built by CI; not bench-tested.
- **A plain Canary's setup network changes once when it updates:** a new
  name (`SecuraCV-canary-` and the same four characters) and a new key.
  Save its recovery kit again afterward.
- **Seven displays take 2.4.17 over USB, not over the air** (the Dash
  modes build, the 7" and S3 nightstands, the C6 nightstand, the 1.69"
  touch, the 2.41" AMOLED and the C3 nightlight); a C3 Vision last flashed
  before 2.4.16 needs the same one USB flash. A Vision Doorbell built from
  the old wiring page moves its amp input from D3 to D10.
- **The monitor after a flash takes commands:** that firmware's own as
  buttons (single keys for the Canary, WAP and Vision, the Sense's console
  words, none for a display) and a box to type in; host-tested, not yet
  tried on a board.
- **Every printable part has a revision.** The enclosure and Assemble tabs
  tint what the newest one moved, and the enclosure tab names the revision
  of an STL you already have. Audited fixes moved parts of the released
  WAP, Vision, Vision Doorbell and Sense cases; no print of a moved part is
  on record, so print one before relying on it.
- **Each bench tells its own board's story:** its chip's ROM lines where a
  source quotes them, whether its firmware reads BOOT, the WAP's real BOOT
  gestures; the Nightlight's Try it bench turns its running glass.
- **Short Wi-Fi passwords are accepted:** no 8-character minimum, up to 64.
- **The Witness Wall shows the day a Canary's key was born** (or first
  dated) when the Canary answers for itself.
- Under the hood: rustls 0.23.46, rustls-webpki 0.103.16 and newer root
  certificates (RUSTSEC-2026-0346 to -0349, -0381 to -0383); Tauri 2.12.4.

## 0.3.1 — 2026-10-16

**A 1.47" C6 display is offered as a display, and every Canary on your
network reaches the Witness Wall.**

- **The Flash page shows the ESP32-C6 display next to the Sense.** The two
  boards share a chip and a flash size, so the page used to lead with the
  Sense and hide the Waveshare ESP32-C6-LCD-1.47 under "show all
  (developer)". It now shows a card for each board it can't rule out, with
  its picture. The same goes for the ESP32-C3 boards.
- **The Witness Wall shows every display, not just the first.** Without a
  hub, the Wall stopped at the first Canary that answered; it now asks each
  one it finds and shows them all, once each.
- **Your C6 nightstands get their own picture** on the fleet pages.

## 0.3.0 — 2026-10-10

**The Lab flashes over USB by itself, and keeps an eye on your fleet from
the menu bar.**

- **Native USB flashing.** The Lab carries the Flasher's flash engine and
  its pinned espflash, so the Flash page installs signed firmware without
  Chrome — the same chip guard, size, SHA-256 and Ed25519 release checks,
  Wi-Fi provisioning, receipts and diagnostics. Serial ports are listed
  natively, and the 2.4.16 firmware catalog is baked in.
- **A menu bar fleet companion:** a tray icon counts the Canaries that are
  online and posts a notification when one changes state; a board that just
  goes quiet reads "not heard lately" and never raises an alert.
- **LAN discovery by mDNS**, so Vision and Sense boards (no web server) are
  found too.
- **Broker encryption on the Flash page**, offered only when the pinned
  firmware release carries TLS (the form names the release that brings it).
- **Test it over Bluetooth** says where the check runs (the iPhone app, or
  the browser Lab in Chrome or Edge) instead of telling you to switch
  browsers.
- **Links that land:** *SecuraCV everywhere* opens the family map; old Lab
  addresses for pages that moved to securacv.com go to those pages; Help
  opens the Help Desk; the Witness Wall's back link returns to its bench.
- **Lighter:** the app no longer bundles the Lab's page tests or its own
  Python and shell tooling (the enclosure CAD scripts still ship with it),
  its native interface is trimmed to the commands its pages use, and the
  print estimate drops a slice button that could only say no.
- **Linux:** the .deb installs a udev rule covering Espressif native USB and
  the CP210x, CH340 and CH9102 bridges, so a Canary's serial port opens
  without root and ModemManager leaves it alone.
- Network: besides your own devices, the Lab fetches its update manifest
  (15 s after launch, then every 6 h), asks GitHub which signed firmware is
  published once a board is connected, and downloads the image you chose
  when you press Flash.
- Under the hood: rustls 0.23.45 (RUSTSEC-2026-0285), mdns-sd 0.21.4, Tauri
  CLI 2.12.0; publishing a draft attaches the project's SBOMs.

## 0.2.4 — 2026-09-03

**Honest about the one thing it fetches.**

- The network claim is now exact, on every surface: the Lab talks only to
  your own devices, and the one thing it fetches on its own is its update
  manifest from GitHub (15 s after launch, then every 6 h) — every update
  it offers is signature-verified before install. The old "nothing phones
  home" line predated the self-updater and overclaimed; the DMG
  installer's background image carried the same words and is regenerated
  without them.
- The main window's grant list no longer includes updater, dialog, or
  process: the self-update flow (manifest check, native confirm dialog,
  relaunch) runs entirely from Rust, so those plugins are out of an
  injected script's reach.
- Toolchain floor raised to Rust 1.88, matching the Flasher.

## 0.2.3 — 2026-08-21

- **A quit can no longer break an install.** The updater now marks the brief
  window while it is replacing the app bundle on disk, and the Lab's close and
  exit paths wait for it — being interrupted during the download costs only a
  re-download, and quitting mid-install (the one thing that could leave the
  Lab unable to open) is held off until the swap completes.
- **One update at a time.** The routine background check and the Settings
  page's "Install update" button now share a single gate, so they can never
  race two installs over the same app bundle.
- **Under the hood:** dependency updates across the desktop stack and the
  whole-repo audit cleanup, with no change to what the Lab stores or sends —
  everything stays on your machine.

## 0.2.2 — 2026-08-07

- **A Settings page that tells you what the app is doing.** New "Updates &
  about" in the sidebar: the exact version and build you're running, when it
  was built, and the firmware train it teaches.
- **Check for updates yourself.** The Lab has always checked on its own —
  shortly after launch and every six hours it stays open — but now there's a
  "Check now" button for when you don't want to wait, and a "Update &
  relaunch" button when one is ready, with that release's notes shown before
  you agree to anything.
- **The update log is visible at last.** Every check and install the app has
  ever done was already written to a journal on disk; now you can read it in
  the app, newest first, with the file's path shown so you can open it
  directly. If an update ever fails, the reason is in there.
- Offline is reported as what it is — "couldn't reach the release channel",
  not an error — since the Lab works entirely offline either way.

## 0.2.1 — 2026-08-07

- **Every bench now carries the Lab's navigation.** The bar at the top of
  every page shows where you are on the build line (stage › bench), walks
  you to the previous or next bench, and always offers "‹ The Lab" back to
  the shell — you can no longer open a bench and lose your way home. It is
  rendered from the same manifest as the sidebar, so the two can't disagree.
- **Links to the website now open in your browser.** Before, following
  SecuraCV, Glossary, Help, or a GitHub link replaced the app's window with
  the website — and with no Back button, the only way out was restarting
  the app. External links now open in your default browser and the Lab
  stays where you were.
- **Five benches stopped running edge-to-edge.** Get started, The Vault,
  The Vision, The Operator's Bench, and First boot laid their text against
  the window edge and let card grids clip offscreen; they now share the
  same centered measure as the rest of the Lab. The sitemap is also
  readable in dark mode.

## 0.2.0 — 2026-07-28

- **The Lab now keeps itself fresh.** It checks for a newer version when
  it starts and every six hours while it stays open. Updates are signed
  and verified before they install, and nothing installs without your OK —
  when one is ready, the Lab shows that release's notes and asks.
- Every check and install is recorded in a local update journal (About →
  the app's data folder), so what the app did is visible and recoverable,
  never silent. Local-first still means local-first: the only thing the
  Lab fetches is its own update manifest, from the project's releases.
