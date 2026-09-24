# Platform pins — which toolchain each env builds on, pinned once

Every PlatformIO env under `firmware/` builds on one of two platform *lines*
(the official `espressif32` platform, arduino-esp32 core 2.x; or the pioarduino
fork, core 3.x), and each pin is typed in exactly one place:
[`envs/platformio/platforms.ini`](envs/platformio/platforms.ini). No other
`.ini` may carry an `espressif32…` version spec or a pioarduino release URL on
a `platform =` line; they interpolate a section instead:

```ini
platform = ${platform_core3.platform}
```

[`scripts/lint_platform_pins.py`](scripts/lint_platform_pins.py) runs in
`lint.yml` on every PR and rejects a literal anywhere else, a reference to a
section `platforms.ini` does not define, and a section no env uses
(`scripts/tests/test_lint_platform_pins.py` feeds it doctored trees to prove
each of those actually goes red).

Before this file existed, the same pioarduino URL was typed in six `.ini`
files and the official pin in three more, each with its own "bump in
lockstep" comment. Lockstep held only as long as every editor remembered
every copy; now it is structural.

## The pin table

| Section | Literal | Who builds on it | Why this literal, and why it is distinct |
|---|---|---|---|
| `platform_s3c3` | `espressif32@6.9.0` | `common_esp32s3` / `common_esp32c3` in `common.ini`, so every env that extends them: canary-vision (all envs), canary-display's core-2 SPI panels (`watch`, `watch-modes`, `watch-debug`, `nightstand-s3`, `touch169`, `amoled241`) and `nightlight-c3`, `canary-sentinel-lite`; `firmware/canary` `[env]` default: `dev`, `release`, `dev_ha`, `release_ha`, `minimal`, `standalone`, `usb-onboard`, `esp32cam`, `esp32-wroom`, `freenove-s3`; `provisioning/platformio_secure.ini` (`secure`, `secure_ha`), which `firmware/canary/platformio.ini` includes through `extra_configs` | Official platform, **exact** pin: arduino-esp32 core 2.0.17 / ESP-IDF 4.4.7, the bench-validated release path that `projects/canary-display/arduino/PARITY.md` and the two `core_compat.h` name by number. The canary tree and the secure envs joined on 2026-09-22 (the decision below); until then they floated on `^7.0.0` (the same 2.0.17 core) and `^6.5.0` (the newest 6.x at build time: `6.9.0` or later, never recorded). |
| `platform_core3` | pioarduino release `55.03.38-1` (`platform-espressif32.zip`) | every ESP32-C6 env (canary-sense, canary-sentinel `door`/`window`/`hallway`/`demo-head`, canary-display `nightstand-c6`); canary-wap (all envs); canary-display's dash family incl. `dash7` / `nightstand7`; canary `[env:full]` | The only PlatformIO platform that packages arduino-esp32 3.x (3.3.8 / ESP-IDF 5.5.4). The official platform has no C6 support for `framework = arduino`; canary-wap calls IDF5-only APIs unconditionally; canary-wap and `[env:full]` link NimBLE-Arduino 2.x, which needs core 3; the dash family needs GFX 1.6.x's RGB bounce buffers, which need core 3. |
| `platform_ota_idf` | `espressif32@6.5.0` | `projects/canary-ota` (`dev`, `production`, `test`) | `framework = espidf`, not arduino: this pin selects an ESP-IDF release, and the project's `sdkconfig` is written against it. Exact pin, left where it was on 2026-09-22 (below). |

The env lists are what `pio project config --json-output` reports per project
on the tree this file was written against; `flavors.json` is the truth for
which of them CI builds and which ship.

## How to bump a pin

Edit the literal in `platforms.ini`; nothing else. Every consumer of that
section moves with it — that is the point, and also the caution: a bump of
`platform_core3` moves canary-sense, canary-wap, canary-sentinel, four display
envs and canary's `[env:full]` in one edit. Before and after, run
`~/.local/bin/pio project config --json-output` in each project directory
and diff: it shows exactly which envs' resolved `platform` changed and
nothing else. Then build. A pin bump is a toolchain change and the
`compile-tested` / `verified` distinction in `boards/boards.json` applies:
nothing built on the new pin is verified until it has been on a bench.

If a consumer genuinely needs different bytes, give it its own section with
the reason written beside it — one section per *distinct* literal, never a
second section for the same one (the lint refuses duplicates).

**Two lines cannot share a PlatformIO core directory.** Both platforms ship a
package named `framework-arduinoespressif32`, at 2.0.17 and 3.3.8; a core-3
build that finds the core-2 copy already installed fails with
`FRAMEWORK_DIR=None` before compiling anything. That is handled *outside*
this file — `flavors.json`'s `isolated_core_envs` / `core_dir_groups`,
honored by `firmware.yml`, the release workflows and `scripts/dev_flash.sh`
(`.github/RELEASE_LESSONS.md`, 2026-07-27). Moving an env between lines means
updating those lists too; `platforms.ini` does not know about core dirs.

## The version spread — decided 2026-09-22 (maintainer to confirm)

The refactor that created `platforms.ini` changed **no value**. It made the
spread visible in one place, and the spread was odd: the same XIAO ESP32-S3
was built under three different specs of the official platform — `6.9.0`
exact for the S3/C3 line, a floating `^7.0.0` for the canary tree, a
floating `^6.5.0` for the secure envs. Three options were on the table: pin
the canary tree and the secure env to `6.9.0` by pointing both at
`platform_s3c3`; pin canary to a chosen exact 7.x (which needs the version a
green release runner actually resolved, readable only from that run's log)
and the secure env to `6.9.0`; or keep floating and record it as deliberate.
The first was taken. It is a build-behavior change under the rule above, not
a refactor, so it carries a maintainer's confirmation — recorded here when
given — and it is what the tree says now:

- **`firmware/canary` `[env]` interpolates `platform_s3c3`** (`6.9.0` exact)
  instead of the floating `^7.0.0`. 7.x ships the same 2.0.17 / 4.4.7 core
  (the 7.x bump added ESP-IDF 6.0 support, not core 3.x), so the canonical
  tree builds on the same 2.0.17 Arduino core as before (the tool packages
  around it change, below), stops picking up whichever 7.x is newest on a
  release runner, and joins the pin that `PARITY.md` and both
  `core_compat.h` name by number. `[env:full]` still overrides to
  `platform_core3`.
- **The secure envs interpolate `platform_s3c3`** instead of `^6.5.0`. A
  caret resolves to the newest 6.x at build time: `6.9.0` or later, and no
  build of these envs recorded which. If it was a later 6.x, this pin moves
  them back to `6.9.0`'s tool packages. Nothing here confirms either case,
  and no workflow builds these envs (below).
  `[platform_canary]` and `[platform_secure]` are gone: the lint refuses a
  dead section and a duplicate literal, so merging pins means deleting
  sections. Three remain, and the SBOM no longer carries a floating
  `platform` component (`scripts/gen_firmware_sbom.py` dropped the `^7.0.0`
  fact with it).
- **`platform_ota_idf` stays at `6.5.0` exact.** `framework = espidf` makes
  that pin an ESP-IDF release, and the project's `sdkconfig` was written
  against it; bumping it is its own bench pass, not this decision. Whether
  6.5.0 is still the intended IDF release is a separate call, recorded here
  when someone makes it.

What moves and what does not: the canary tree's Arduino core is unchanged
(2.0.17); the tool packages `6.9.0` bundles differ from the ones a 7.x
resolve brought — the 7.x line's esptool 4.11 is why the release workflows
install `intelhex`, and that pip extra (`flavors.json` `pip_extras`, `firmware-release.yml`,
`flasher-release.yml`) is now a leftover, kept because it is harmless and
the merged-bin steps pin their own esptool from pip.

The compile proof covers only part of what moved. `firmware.yml` builds the
canary envs that `flavors.json` lists under `build_envs`. On the new pin
those are `dev`, `release`, `release_ha`, `esp32cam`, `esp32-wroom` and
`freenove-s3` (`full` is in the list too, but it builds on `platform_core3`).
`firmware-release.yml` builds `release`, `release_ha` and the three board
envs again at tag time. **No workflow compiles `dev_ha`, `minimal`,
`standalone` or `usb-onboard` on the new pin.** They take the same `[env]`
platform as `dev` and `release` (they extend one of them) and differ in their
own options, not in the platform; all four move with this pin untested, so
build them by hand before relying on them, or add them to `build_envs`. The
secure envs resolve only because `firmware/canary/platformio.ini` lists
`provisioning/platformio_secure.ini` in `extra_configs`, and since F42
`firmware.yml`'s canary leg compiles `secure` and `secure_ha` on the pin in a
compile-only step (they are in no `flavors.json` list, so nothing ships from
them). The first tag after the change deserves a look at the release
log's bootloader / merged-bin step. Per `boards/boards.json`, the S3 canary
images are compile-tested on the pin until a bench pass. Rolling back is one
`.ini` edit.

## Not covered by `platforms.ini`

- **The Arduino CLI builds** pin their `esp32:esp32` core on their own axis
  (the `.github/actions/setup-arduino-esp32` composite action's `core-version`
  input and each sketch's `sketch.yaml`). `scripts/gen_firmware_sbom.py` reads
  both and refuses to generate — so `lint.yml`'s `--check` goes red on the PR
  — unless they agree: every core-version a workflow row pins is pinned by
  some sketch profile; every core a sketch profile pins is a workflow pin or
  the Arduino core inside that product's PlatformIO platform (the table
  above); and every library a workflow row pins on a core line (GFX / lvgl /
  NimBLE split their majors along the core boundary) is pinned to the same
  version by that product's profiles on that core
  ([`sbom/README.md`](../sbom/README.md)). What that leaves, on purpose: the
  WAP's Arduino-CLI rows (`firmware.yml`, `firmware-release.yml`,
  `flasher-release.yml`, `csi_module_disable_matrix.yml`, `ram_audit.yml`)
  pass no `core-version` and build on the action's weekly "latest", while the
  WAP sketch pins 3.3.8 — the core inside `platform_core3`. The sketch tracks
  the PlatformIO path; the CI rows float above it by design. Pinning those
  rows is a release decision (new cache keys, the weekly rotation ends),
  recorded here when someone makes it, not a lint's call. Note too that the
  core-3 lines sit on different patch releases (PlatformIO 3.3.8, Arduino-CLI
  3.3.10); the SBOM lists them as two components rather than one.
- **`projects/canary-wap/setup.sh`** runs `pio pkg install --global --platform
  espressif32` (unpinned, latest official) for its interactive first-run path.
  That pre-installs a platform; the env's own `platform =` spec still governs
  what `pio run` resolves.
- **Core-dir isolation** (above) lives in `flavors.json`.
