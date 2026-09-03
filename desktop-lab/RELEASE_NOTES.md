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

## 0.2.4 — 2026-09-03

**Honest about the one thing it fetches.**

- The network claim is now exact: the Lab talks only to your own devices,
  and the one thing it fetches on its own is its signed update manifest
  from GitHub (15 s after launch, then every 6 h). The old "nothing phones
  home" line predated the self-updater and overclaimed.
- The main window's webview permission set is now empty: the self-update
  flow (manifest check, native confirm dialog, relaunch) runs entirely from
  Rust, so an injected script has nothing left to reach.
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
