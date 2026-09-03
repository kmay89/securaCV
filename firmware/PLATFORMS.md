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
| `platform_s3c3` | `espressif32@6.9.0` | `common_esp32s3` / `common_esp32c3` in `common.ini`, so every env that extends them: canary-vision (all envs), canary-display's core-2 SPI panels (`watch`, `watch-modes`, `watch-debug`, `nightstand-s3`, `touch169`, `amoled241`) and `nightlight-c3`, `canary-sentinel-lite` | Official platform, **exact** pin: arduino-esp32 core 2.0.17 / ESP-IDF 4.4.7, the bench-validated release path that `projects/canary-display/arduino/PARITY.md` and the two `core_compat.h` name by number. |
| `platform_core3` | pioarduino release `55.03.38-1` (`platform-espressif32.zip`) | every ESP32-C6 env (canary-sense, canary-sentinel `door`/`window`/`hallway`/`demo-head`, canary-display `nightstand-c6`); canary-wap (all envs); canary-display's dash family incl. `dash7` / `nightstand7`; canary `[env:full]` | The only PlatformIO platform that packages arduino-esp32 3.x (3.3.8 / ESP-IDF 5.5.4). The official platform has no C6 support for `framework = arduino`; canary-wap calls IDF5-only APIs unconditionally; canary-wap and `[env:full]` link NimBLE-Arduino 2.x, which needs core 3; the dash family needs GFX 1.6.x's RGB bounce buffers, which need core 3. |
| `platform_canary` | `espressif32 @ ^7.0.0` | `firmware/canary` `[env]` default: `dev`, `release`, `dev_ha`, `release_ha`, `minimal`, `standalone`, `usb-onboard`, `esp32cam`, `esp32-wroom`, `freenove-s3` | Official platform on a **floating** caret. 7.x still ships the same 2.0.17 core as 6.9.0 (the 7.x bump added ESP-IDF 6.0 support, not core 3.x), so it differs from `platform_s3c3` in what it may resolve to, not in the core it builds today. One visible consequence: the release workflows install `intelhex` because esptool 4.11, which 7.x brings, imports it without declaring it. |
| `platform_ota_idf` | `espressif32@6.5.0` | `projects/canary-ota` (`dev`, `production`, `test`) | `framework = espidf`, not arduino: this pin selects an ESP-IDF release, and the project's `sdkconfig` is written against it. Exact pin. |
| `platform_secure` | `espressif32 @ ^6.5.0` | `provisioning/platformio_secure.ini` (`secure`, `secure_ha`) when included from `firmware/canary/platformio.ini` per its header | The Phase-2 Secure Boot v2 + Flash Encryption env. Caret 6.5, so it resolves to the newest 6.x at build time. |

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

## The version spread is inherited, not designed — an open maintainer decision

The refactor that created `platforms.ini` changed **no value**. It did make
the spread visible in one place, and the spread is odd: the same XIAO
ESP32-S3 is built under three different specs of the official platform.

- `platform_s3c3` pins `6.9.0` exactly while `platform_canary` floats on
  `^7.0.0`. Both build the 2.0.17 core today, but `firmware/canary` — the
  canonical tree — can silently pick up a new 7.x on a release runner while
  the S3 line cannot. Should `firmware/canary` pin exactly (and if so, to
  6.9.0 — one section fewer — or to a chosen 7.x)?
- `platform_secure` floats on `^6.5.0`, which today resolves to the newest
  6.x; if that is 6.9.0 it is `platform_s3c3` under a different spelling.
  Pin it exactly, or merge it into `platform_s3c3`?
- `platform_ota_idf` is exactly `6.5.0` with `framework = espidf` — a
  legitimately different framework, but is 6.5.0 still the intended IDF
  release, or the one that was current when the project started?

Any of these is a build-behavior change, not a refactor: it needs a build on
each affected env and, for anything that ships, a bench pass. Until someone
makes that call, the sections stay as they are, and this section is the
place to record the decision.

## Not covered by `platforms.ini`

- **The Arduino CLI builds** pin their `esp32:esp32` core on their own axis
  (the `.github/actions/setup-arduino-esp32` composite action's `core-version`
  input and each sketch's `sketch.yaml`). The two axes are meant to agree
  (core 2.0.17 ↔ `platform_s3c3`, core 3.3.x ↔ `platform_core3`) but nothing
  enforces it yet.
- **`projects/canary-wap/setup.sh`** runs `pio pkg install --global --platform
  espressif32` (unpinned, latest official) for its interactive first-run path.
  That pre-installs a platform; the env's own `platform =` spec still governs
  what `pio run` resolves.
- **Core-dir isolation** (above) lives in `flavors.json`.
