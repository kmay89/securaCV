# securaCV — working notes

> **Start with [`AGENTS.md`](AGENTS.md).** It's the canonical brief every AI
> assistant here works from: what the project is, the privacy invariants that
> are non-negotiable, the repo map, the "where do I look for X" index, and the
> CI gates you'll trip. This file adds the Claude-specific working notes on top
> of it — it is not a replacement.
>
> Two more worth having open:
> - [`docs/GLOSSARY.md`](docs/GLOSSARY.md) — every proper noun in the project
>   (SecuraCV vs Canary vs the kernel, Opera/Chirp/Beacon, the device line,
>   break-glass, the invariants), defined once. **Read this before answering a
>   user's question about what something is** — most of the terms here are
>   ours, not general knowledge.
> - [`docs/FAQ.md`](docs/FAQ.md) — the questions users actually ask, with the
>   honest-status answers.

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

- **US spellings, now and always.** `color`, `center`, `meter`, `behavior`,
  `analyze`, `gray`, `license`, `labeled`, `canceled`, `optimize`,
  `recognize`, `catalog` — never the British forms. This binds user-facing
  copy, device UI strings, docs, comments **and code identifiers**.
  - ✅ `color`, `center`, `bezel_color`, `frame_color`, `pal_color`
  - ❌ `colour`, `centre`, `bezel_colour`, `frame_colour`
  - Exceptions, because they are not ours to respell: SPDX tags and
    `LICENSE` filenames, third-party API/CSS identifiers, and quoted text
    from an external source. `analysis`, `parameter` and `diameter` are the
    same in both — they are not British and need no change.
  - See [`AGENTS.md`](AGENTS.md) rule 3b, the canonical statement.

## Enclosure CAD

- **Always send rendering previews.** Any change to an enclosure `.scad`
  ships with PNG previews of every affected part, shared with the requester
  in the conversation. This is a repo-wide rule — it lives in the AGENTS.md
  brief ("Before you commit"), and the render recipe is in
  [`docs/hardware/enclosure/README.md`](docs/hardware/enclosure/README.md)
  under "Preview renders".

- **Re-export an STL → regenerate the figures.** The fleet figures
  ([`docs/design/FLEET_FIGURES.md`](docs/design/FLEET_FIGURES.md)) read their
  dimensions live off the committed STL bounding boxes, so a re-export moves
  the drawings every surface shows. Run
  `node canary-local/tools/figures/gen_figures.mjs` and commit its output in
  the same change; `--check` is the CI gate and will name every stale file.
  The generator refuses to emit a figure that has drifted from its part, or
  one that puts two materials on the same plane — both are real defects, not
  style notes.

- **Which of these is real?** Don't hand-write a status next to a product.
  The confidence ladder in `canary-local/devices/figures.json` (shipping /
  confirmed / prototype / idea) is *derived* from evidence on disk, and the
  evidence sits beside the verdict. An idea renders as a dashed ghost by
  construction — if you find yourself wanting to draw a concept as a solid
  product, that's the invariant working, not a bug.

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
