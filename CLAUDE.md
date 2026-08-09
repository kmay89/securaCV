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

- **US spellings, now and always.** Write `color`, `center`, `meter`,
  `behavior`, `analyze`, `gray`, `license`, `labeled`, `canceled`,
  `optimize`, `recognize`, `catalog` — never the British forms of those
  words. Binds user-facing copy, device UI strings, docs, comments **and
  code identifiers** (`bezel_color`, `frame_color`, `pal_color`).
  - The banned list is enumerated ONCE, in the regex in
    [`scripts/lint_spelling.py`](scripts/lint_spelling.py), which fails the
    build on any of them. **Do not repeat the British forms in prose** —
    a doc that spells them out is a doc the next sweep rewrites into
    nonsense. That happened to this bullet, and to AGENTS.md rule 3b, and
    to both of the website repo's copies. Four times, one cause.
  - Words that only *look* British to a substring match are correct and
    must survive: `analysis`, `emphasis`, `parameter`, `diameter`,
    `characteristic` (the BLE API), `realistic`, `optimistic`,
    `initialism`, `aria-labelledby`. The linter's `ALLOW` list asserts it.
  - See [`AGENTS.md`](AGENTS.md) rule 3b, the canonical statement.

## Generated files — there are TWENTY, not a handful

Committed generators whose output CI regenerates and byte-diffs. Editing a
source without re-running the right one leaves a gate to find it, and the
failure is often nowhere near the edit: a one-word spelling fix in
`canary-local/tools/hub_seed_apply.py` changed its bytes, which broke a
**sha256 pin** cross-checked by a Rust unit test in `desktop/hub-io`
(`test (hub-io)` went red, three layers away from the change).

Don't try to remember the list — derive it:

```sh
grep -rhoE "python3 [a-zA-Z0-9_/.-]*gen_[a-z_]+\.py|node [a-zA-Z0-9_/.-]*make-[a-z-]+\.mjs" \
  .github/workflows/*.yml | sort -u
```

The ones that bite most often: `gen_stamp.py` and `gen_builder_manifest.py`
(any enclosure `.scad`), `gen_enclosures.py` (catalog JSON),
`gen_agent_entrypoints.py` (any AGENTS.md edit — six vendor files),
`gen_hub_provision_bundle.py` (anything it embeds and pins by hash),
`gen_apple_home_docs.py` (any `homekit_projection` change in the dictionary —
the Apple Home quickstart's signal table is a privacy promise, so a stale one
is a false statement rather than merely old).

**The recipe above does not find the twenty-first, and it can't:** the WASM
emulator's `canary-local/emulator/dist/*.js` is generated and committed like
the rest, but its generator is a compiler and its *inputs are firmware
sources*. There is no `gen_*.py` or `make-*.mjs` to grep for. So an ordinary
C++ edit can leave `dist/` stale, and `canary-local.yml`'s "Dist drift check"
fails on a file you never opened.

Which edits move it is not obvious either — it depends on what
`canary-local/emulator/build.sh` actually compiles, which is `src/main.cpp`,
the LVGL faces and `care/`/`fleet/`/`trust`, **not** the `net/` layer:

- editing `common/color/look_engine.cpp` → `dist/` changes (the color engine
  is in the render path);
- editing `common/fleet_selfreport/fleet_selfreport.h` → `dist/` does **not**
  change, even though `net/glass_web.cpp` includes it, because that file is
  never compiled into the emulator.

Fixing it needs emsdk **6.0.3** exactly, which most working environments can't
install. Don't fight that — use **Actions → "Rebuild emulator dist (pinned
emsdk)"**, dispatched on your branch: it rebuilds where the toolchain lives and
pushes the bytes back. Two things to know before you rely on it:

1. **It pushes to the branch it was dispatched on.** Dispatch it on `main` and
   it commits to `main`; prefer a feature branch.
2. **Its push does not retrigger CI** (default `GITHUB_TOKEN`), so the PR
   keeps showing the old failure until you push again yourself. Re-running
   the failed job does *not* work — a re-run checks out the original commit,
   which still has the stale `dist/`.
3. **That push must touch a path the workflow watches, or nothing runs.**
   "One ordinary push" is not enough and this bullet used to say it was.
   `canary-local.yml` (the job that fails, "firmware → wasm → boots in a
   browser") filters on `firmware/projects/canary-display/**`,
   `firmware/common/**`, `firmware/envs/**` and friends; `firmware.yml`
   filters on `firmware/**`. A docs-only commit at the repo root matches
   neither, so the PR sits at **zero** check runs — which looks like CI is
   still queued, not like it never started. If you have nothing real to
   change under those paths, say so and hand the PR over red rather than
   inventing a no-op commit; a reviewer can re-run from a merge commit.
   (Costs an hour to learn: two "retrigger" pushes in #1536 ran nothing.)

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
