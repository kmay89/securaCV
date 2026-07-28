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

## 0.2.0 — 2026-07-28

- **The Lab now keeps itself fresh.** It checks for a newer version when
  it starts and every six hours while it stays open. Updates are signed
  and verified before they install, and nothing installs without your OK —
  when one is ready, the Lab shows that release's notes and asks.
- Every check and install is recorded in a local update journal (About →
  the app's data folder), so what the app did is visible and recoverable,
  never silent. Local-first still means local-first: the only thing the
  Lab fetches is its own update manifest, from the project's releases.
