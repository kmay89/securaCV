# Canary WAP (Snapshot)

This directory captures a **frozen snapshot** of the existing Arduino sketch so
we can iterate toward the firmware architecture without breaking the current
working demo. The files under `snapshot/` are intentionally **not wired into any
build system**; they serve as a reference baseline only.

## What belongs here

- The raw Arduino sketch files as-is (e.g., `*.ino`, `*.h`).
- No secrets, credentials, or environment-specific settings.
- No build config integration (that belongs under `envs/` and `projects/` once we
  conform to the firmware architecture).

## How to update this snapshot

1. Unzip or copy the working sketch into `snapshot/canary_wap/`.
2. Preserve file names and layout so diffs remain clear.
3. Keep this snapshot stable while we refactor into `common/`, `boards/`,
   `configs/`, and a proper project wrapper.

## Next step (planned)

We will translate this snapshot into a conforming firmware layout by extracting
board-agnostic logic into `firmware/common/`, pin maps into `firmware/boards/`,
and configuration into `firmware/configs/`, then wiring it through a proper
`envs/` target.
