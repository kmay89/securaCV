# securaCV — working notes

## Voice & naming

- **Never call a group of Canaries a "flock." We call it a *fleet*.**
  A company called Flock soured the word; it is off-limits in all
  user-facing copy, device UI strings, product/bundle names, code
  identifiers, and comments. Use "fleet" (which is already the established
  term across the firmware, e.g. `fleet_model.h`) — or plain "your
  Canaries" / "the devices."
  - ✅ "It's in the fleet", "your fleet", "Starter Fleet", `fleetSummary`, `NVS_FLEET_ID`
  - ❌ "flock" in any of those senses
  - The **only** allowed exception is the Unix `flock(2)` file-lock
    syscall (e.g. "no flock/PID lock" in the storage flight-rules) — that
    is a real API name, not the bird word. Do not rename it.

## Release & packaging

- **To ship anything, or to answer "why didn't this ship?", read
  [`docs/RELEASE_BUTTONS.md`](docs/RELEASE_BUTTONS.md) first** — the operator's
  index: every button, when to press it, when *not* to, and the three failures
  that have cost real time (no OTA signing key, a flasher pinned to an uncut
  release, an app version that was already published). The default button is
  **Actions → "Update everything (only what needs it)"**: it releases only what
  is genuinely ahead, explains what it skipped, and cannot re-ship a new tree
  under a published version.
- **Before touching any app build/release workflow** (Flasher, Lab, or the
  iPhone / iPad / tvOS / Mac targets), read
  [`.github/RELEASE_LESSONS.md`](.github/RELEASE_LESSONS.md) — the canonical
  home for build/release lessons. Top rule: copy bundled payloads with
  `cp -RL` (dereference symlinks), or a dangling link makes the bundler abort
  with a confusing "resource … doesn't exist". When you fix a
  release/packaging bug, append a dated entry there and apply it to every app
  target, not just the one that broke.
- **Two flashers, two frontends.** The in-browser flasher
  (`canary-local/assets/`) and the desktop Flasher app (`desktop/src/`) share no
  UI code. A user-facing diagnostic added to one must be added to the other, or
  half the users keep the vague version.
