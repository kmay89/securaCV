# Firmware Architecture (Normative)

**Status:** Canonical and normative for the `firmware/` subtree.  
**Precedence:** If a document conflicts with this file, this file wins within `firmware/`.

## Purpose

This document defines the **required** directory structure and layering rules for
firmware projects that target multiple boards and multiple configurations. The
pattern is intentionally similar to Marlin’s high-level organization (core +
board pins + configs + build environments), without assuming any Marlin-specific
implementation details.

## Design Goals

1. Support many boards without forking app logic.
2. Support many configurations without duplicating board or core code.
3. Make build targets explicit, reproducible, and reviewable.
4. Keep sensitive or board-specific data isolated from app behavior.

## Required Top-Level Layout

```
firmware/
  ARCHITECTURE.md           # This document (normative)
  README.md                 # Overview + project index
  common/                   # Board-agnostic core logic
  boards/                   # Board definitions & pin maps
  configs/                  # Product/app configurations
  envs/                     # Build environments (toolchains, targets)
  projects/                 # Thin project wrappers
```

### `common/` — Core Logic (Board-Agnostic)
**Must contain only** reusable, board-agnostic logic. Examples:
- Protocol logic
- Event formats
- Business rules
- Shared drivers that do not embed pin mappings

**Must not contain** board pin mappings, linker scripts, or board-specific
peripherals.

### `boards/` — Board Definitions
One directory per board:

```
boards/
  <board_id>/
    README.md               # Board metadata and constraints
    pins/                   # Pin maps and board wiring
    variants/               # Optional board revisions or sub-variants
```

**Must contain** pin maps and any strictly board-specific information.  
**Must not contain** app logic or configuration flags.

### `configs/` — App or Product Configurations
One directory per configuration target, grouped by app or product:

```
configs/
  <app_id>/
    <config_id>/
      README.md             # Intended behavior + constraints
      config.*              # Concrete config inputs (format-specific)
```

**Must contain** configuration data and feature flags.  
**Must not contain** board pin mappings or toolchain definitions.

### `envs/` — Build Environments
Defines toolchains, flags, and build targets. Example structure:

```
envs/
  platformio/
    <env_id>.*
```

Each build environment **must** bind exactly:
- One board (`boards/<board_id>`)
- One configuration (`configs/<app_id>/<config_id>`)
- One core entrypoint from `common/`

### `projects/` — Thin Wrappers
Projects should be thin “composition layers” that tie together:
- A board definition
- A configuration
- A build environment

Projects **must not** re-implement core logic. If you need new behavior, add it
to `common/` and reference it.

## Layering Rules (Non-Negotiable)

1. **No cross-layer leakage**
   - `common/` never imports `boards/` or `configs/`.
   - `boards/` never imports `configs/` or `common/` logic.
   - `configs/` never imports `boards/` or `common/` logic.
2. **Composition happens only in `projects/` and `envs/`**
3. **Single source of truth**
   - Pin maps live only under `boards/`.
   - Feature flags and behavior switches live only under `configs/`.
4. **No board-specific forks**
   - If code diverges for a board, make it a board abstraction in `common/` and
     bind the pins in `boards/`.
5. **No config-specific forks**
   - If behavior diverges by configuration, express it as a configuration input
     (data), not a code fork.

## Naming Conventions

- `<board_id>`: lowercase, dash-separated (e.g., `esp32-c3`, `stm32f4`)
- `<app_id>`: lowercase, dash-separated (e.g., `canary-vision`)
- `<config_id>`: lowercase, dash-separated (e.g., `default`, `retail`, `lab`)
- `<env_id>`: lowercase, dash-separated (e.g., `pio-esp32-c3-default`)

## Rules for Contributors and Automated Code Generation

- This document is **normative** and **must** be followed.
- Any generator or script that produces firmware structure **must** emit the
  layout defined here.
- If you need to add a new category, update this document **first**, then add
  the new structure in a separate commit.

## Migration Note

Existing firmware projects may not yet fully conform. New projects **must**
follow this structure, and existing projects should be incrementally aligned
when touched.
