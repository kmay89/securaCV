# SecuraCV Flasher — release notes

What changed **for the person using the app**, one section per released
version, newest first. This file is load-bearing, not decoration:

- The release workflows publish the newest section as the GitHub release
  body, so the download page says what the download changes.
- The self-updater's manifest (`latest.json`) carries the same section as
  its `notes`, so the in-app "an update is ready" banner can say **what's
  changing** instead of just a version number.
- `desktop/scripts/release_notes.py check` (run by lint and by both app
  release workflows) fails the build if the newest section here doesn't
  match the version in `src-tauri/tauri.conf.json` — bumping the version
  and writing the notes are one act, like the three version files.

Write for the user, not the diff: what they can do now, what got fixed,
and what to expect after updating. Heading grammar is
`## <version> — <YYYY-MM-DD>`.

## 0.3.5 — 2026-07-28

- **The update banner now says what's changing.** When a newer version is
  ready, the app shows that release's own notes — these very sections —
  not just a version number, so you decide with the facts in front of you.
- **Update checks are now routine, not just at launch.** The app re-checks
  every six hours while it stays open, and again when you come back to a
  window that sat idle — so a bench machine that never relaunches still
  hears about updates. Nothing installs without your click, and every
  update stays signed and verified before it runs.
- Checks and installs are recorded in the About page's activity log, kept
  on this computer only.

## 0.3.4 — 2026-07-28

- The macOS installer window (the DMG) draws its background at the right
  size again — no more giant, cropped artwork with the instructions pushed
  out of view.
- Flashers 0.3.0 and older lost self-update when the update address moved;
  the release pipeline now serves a valid manifest at the old address too,
  so those installs see updates again and heal forward on their own.
