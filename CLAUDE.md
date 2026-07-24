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

- **Before touching any app build/release workflow** (Flasher, Lab, or the
  iPhone / iPad / tvOS / Mac targets), read
  [`.github/RELEASE_LESSONS.md`](.github/RELEASE_LESSONS.md) — the canonical
  home for build/release lessons. Top rule: copy bundled payloads with
  `cp -RL` (dereference symlinks), or a dangling link makes the bundler abort
  with a confusing "resource … doesn't exist". When you fix a
  release/packaging bug, append a dated entry there and apply it to every app
  target, not just the one that broke. The whole pipeline (apps + web) runs
  from one button: `.github/workflows/release-one-click.yml`.
