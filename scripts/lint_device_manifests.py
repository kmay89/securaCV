#!/usr/bin/env python3
"""scripts/lint_device_manifests.py — one manifest per Canary; every join checked.

A Canary device is described in seven schemas that nothing used to join:
firmware/flavors.json + build_matrix.json + the PlatformIO envs (which builds),
firmware/boards/boards.json + boards/<id>/pins/pins.h (which silicon and
peripherals), canary-local/emulator/build.sh + dist/*.meta.json (which browser
twin), canary-local/devices/figures.json (which picture, and how real),
docs/hardware/enclosure/*.scad + canary-local/devices/enclosures.json (which
case), canary-local/devices/flash.json (which flasher product) and the website
(which page and model). devices/<slug>/device.json is the join — it names the
id each of those files uses for one device and nothing else. This lint proves
every one of those names against the file that owns it.

Nothing consumes the manifests yet (docs/IMPROVEMENT_ROADMAP.md §4, wave 1:
"Describe"), so this gate is the only thing that can catch a stale join.

What is PROVED (each a hard error, exit 1):
  schema       every manifest validates against devices/device.schema.json —
               a stdlib validator (type, required, enum, pattern, minLength,
               minimum, properties, additionalProperties, items, minItems,
               uniqueItems); CI has no jsonschema module. additionalProperties
               is false, so a hand-typed `status` / `confidence` cannot appear.
  identity     directory name == slug; slugs unique; `family` is a
               firmware/flavors.json product.
  envs         every board.envs[] is an [env:NAME] in the family's
               platformio.ini with its extra_configs resolved into
               firmware/envs/platformio/*.ini; no env claimed by two devices;
               every flavors.json build_env is claimed by exactly one manifest
               OR listed in devices/unclaimed.json with a reason (an unclaimed
               entry that is also claimed, or that CI no longer builds, fails).
  board        board_id is a boards.json row; mcu / psram_mb / flash_mb match
               it; each env's resolved `board =` is that row's pio_board; each
               env's `-I boards/<id>/pins` include is board_id or a listed
               variant.
  peripherals  each is `#define HAS_<NAME> 1` in boards/<board_id>/pins/pins.h.
  figure       exists in figures.json with role `device` and the same family;
               agrees with figures.json's hardware map for board_id and with
               every flash.json product that draws this device. The ladder
               verdict is READ from the figure and printed — never typed.
  flasher      product (+ variants) exist in flash.json; chip == board.mcu
               (normalized); tier.board_id is board_id or a variant; flash_mb
               agrees; every flash.json product is claimed by exactly one
               manifest.
  emulator     flavor is in build.sh's allowlist; dist/canary-display-<flavor>
               .meta.json exists; build.sh compiles this device's pins dir for
               that flavor; every dist display flavor is claimed exactly once.
  build_matrix every product has a manifest (by id, else by flavor + env);
               its board, mcu and env(s) agree with that manifest.
  cad          scad exists; enclosure sets exist in enclosures.json; scad is
               the source of at least one listed set.
  site         shape only — those paths live in the website repository.

Output: one table row per device (slug · mcu · envs · emulator · confidence ·
flasher product), then every error.

Run:  python3 scripts/lint_device_manifests.py
CI:   .github/workflows/lint.yml (Repo Lints), beside lint_build_matrix.py
Test: scripts/tests/test_device_manifests.py (unittest discover)
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEVICES_DIR = REPO / "devices"

# ── chip vocabulary ─────────────────────────────────────────────────────────
# esptool's names (ESP32, ESP32-S3, ESP32-C3, ESP32-C6) are the canon; the
# build matrix says things like "ESP32 (classic dual-core)" and "ESP32-C6 ·
# MR60BHA2 radar", so compare the leading chip token, case- and hyphen-blind.
CHIP_RE = re.compile(r"ESP32(?:[-_ ]?([SC]\d))?", re.I)


def norm_chip(text: str | None) -> str | None:
    m = CHIP_RE.search(text or "")
    if not m:
        return None
    suffix = m.group(1)
    return "ESP32" + (f"-{suffix.upper()}" if suffix else "")


# ── a small JSON Schema (2020-12) validator ─────────────────────────────────
# Only the keywords devices/device.schema.json uses. Unknown keywords are
# ignored, which is what a full validator does too.
def _is_type(value, name: str) -> bool:
    if name == "object":
        return isinstance(value, dict)
    if name == "array":
        return isinstance(value, list)
    if name == "string":
        return isinstance(value, str)
    if name == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if name == "boolean":
        return isinstance(value, bool)
    if name == "null":
        return value is None
    return False


# Keys people reach for when they want to say how real a device is. The
# ladder verdict is derived from figures.json evidence; the schema forbids
# these by construction and this hint says why.
LADDER_WORDS = {"status", "confidence", "tier", "ladder", "readiness"}


def validate(instance, schema: dict, path: str = "$") -> list[str]:
    errs: list[str] = []
    want = schema.get("type")
    if want is not None:
        names = want if isinstance(want, list) else [want]
        if not any(_is_type(instance, n) for n in names):
            errs.append(f"{path}: expected {'/'.join(names)}, got {type(instance).__name__}")
            return errs
    if "enum" in schema and instance not in schema["enum"]:
        errs.append(f"{path}: {instance!r} is not one of {schema['enum']}")
    if isinstance(instance, str):
        if "pattern" in schema and not re.search(schema["pattern"], instance):
            errs.append(f"{path}: {instance!r} does not match /{schema['pattern']}/")
        if "minLength" in schema and len(instance) < schema["minLength"]:
            errs.append(f"{path}: shorter than minLength {schema['minLength']}")
    if _is_type(instance, "number") and "minimum" in schema and instance < schema["minimum"]:
        errs.append(f"{path}: {instance} is below minimum {schema['minimum']}")
    if isinstance(instance, dict):
        for key in schema.get("required", []):
            if key not in instance:
                errs.append(f"{path}: missing required key '{key}'")
        props = schema.get("properties", {})
        extra = schema.get("additionalProperties", True)
        for key, value in instance.items():
            if key in props:
                errs.extend(validate(value, props[key], f"{path}.{key}"))
            elif extra is False:
                hint = (" — the confidence ladder is derived from "
                        "canary-local/devices/figures.json evidence and printed by this "
                        "lint; it is never typed in a manifest"
                        if key.lower() in LADDER_WORDS else "")
                errs.append(f"{path}: key '{key}' is not allowed "
                            f"(additionalProperties: false){hint}")
            elif isinstance(extra, dict):
                errs.extend(validate(value, extra, f"{path}.{key}"))
    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            errs.append(f"{path}: fewer than minItems {schema['minItems']}")
        if schema.get("uniqueItems"):
            seen: list = []
            for i, item in enumerate(instance):
                if item in seen:
                    errs.append(f"{path}[{i}]: duplicate item {item!r}")
                seen.append(item)
        if "items" in schema:
            for i, item in enumerate(instance):
                errs.extend(validate(item, schema["items"], f"{path}[{i}]"))
    return errs


# ── PlatformIO ini reading (no PlatformIO needed) ───────────────────────────
INLINE_COMMENT_RE = re.compile(r"\s+;.*$")
SECTION_RE = re.compile(r"^\[([^\]]+)\]\s*$")
KEY_RE = re.compile(r"^([A-Za-z_][\w.]*)\s*=\s*(.*)$")
INTERP_RE = re.compile(r"\$\{([^}.]+)\.([^}]+)\}")
PINS_INCLUDE_RE = re.compile(r"boards/([A-Za-z0-9_-]+)/pins")


def parse_ini(text: str, sections: dict[str, dict[str, str]]) -> None:
    """Fill `sections` ({section: {key: value}}) from one ini file. Multi-line
    values (PlatformIO's indented continuation lines) are joined with '\\n';
    full-line and trailing `;` comments are dropped."""
    current: str | None = None
    last_key: str | None = None
    for raw in text.splitlines():
        line = raw.rstrip()
        if not line.strip() or line.lstrip().startswith((";", "#")):
            continue
        line = INLINE_COMMENT_RE.sub("", line)
        m = SECTION_RE.match(line)
        if m:
            current = m.group(1)
            sections.setdefault(current, {})
            last_key = None
            continue
        if current is None:
            continue
        if line[0] in " \t":
            if last_key is not None:
                sections[current][last_key] += "\n" + line.strip()
            continue
        m = KEY_RE.match(line)
        if m:
            last_key = m.group(1)
            sections[current][last_key] = m.group(2).strip()


def load_project_ini(project_dir: Path) -> dict[str, dict[str, str]]:
    """The project's platformio.ini plus every file its extra_configs names."""
    sections: dict[str, dict[str, str]] = {}
    root = project_dir / "platformio.ini"
    if not root.exists():
        return sections
    parse_ini(root.read_text(encoding="utf-8"), sections)
    for rel in sections.get("platformio", {}).get("extra_configs", "").split():
        extra = (project_dir / rel).resolve()
        if extra.exists():
            parse_ini(extra.read_text(encoding="utf-8"), sections)
    return sections


def resolve_key(sections, section: str, key: str, depth: int = 0) -> str | None:
    """A section's value for key, following `extends` and the [env] base."""
    if depth > 16 or section not in sections:
        return None
    body = sections[section]
    if key in body:
        return body[key]
    for parent in (body.get("extends") or "").split():
        found = resolve_key(sections, parent, key, depth + 1)
        if found is not None:
            return found
    if section.startswith("env:") and key in sections.get("env", {}):
        return sections["env"][key]
    return None


def expand(sections, text: str | None, depth: int = 0) -> str:
    """Expand `${section.key}` references (PlatformIO interpolation)."""
    if not text or depth > 8:
        return text or ""
    return INTERP_RE.sub(
        lambda m: expand(sections, resolve_key(sections, m.group(1), m.group(2)), depth + 1),
        text)


def env_pins_boards(sections, env: str) -> set[str]:
    flags = expand(sections, resolve_key(sections, f"env:{env}", "build_flags"))
    return set(PINS_INCLUDE_RE.findall(flags))


def env_pio_board(sections, env: str) -> str | None:
    value = resolve_key(sections, f"env:{env}", "board")
    return value.split()[0] if value else None


# ── emulator build.sh facts ─────────────────────────────────────────────────
FLAVOR_CMP_RE = re.compile(r'"\$FLAVOR"\s*==\s*"([a-z0-9]+)"')
FLAVOR_IF_RE = re.compile(r'^\s*(?:el)?if\s+\[\[\s*"\$FLAVOR"\s*==\s*"([a-z0-9]+)"\s*\]\]')
PINS_DIR_RE = re.compile(r'PINS_DIR="\$FW/boards/([A-Za-z0-9_-]+)/pins"')


def emulator_facts(build_sh: Path, dist: Path) -> tuple[set[str], dict[str, str], set[str]]:
    """(allowlisted flavors, flavor -> boards/<id> it compiles, dist display flavors)."""
    text = build_sh.read_text(encoding="utf-8") if build_sh.exists() else ""
    allow = set(FLAVOR_CMP_RE.findall(text)) - {"all"}
    pins: dict[str, str] = {}
    branch: str | None = None
    for line in text.splitlines():
        m = FLAVOR_IF_RE.match(line)
        if m:
            branch = m.group(1)
            continue
        if re.match(r"^\s*else\s*$", line):
            branch = "__else__"
            continue
        if re.match(r"^\s*fi\s*$", line):
            branch = None
            continue
        m = PINS_DIR_RE.search(line)
        if m and branch:
            pins[branch] = m.group(1)
    display = {p.name[len("canary-display-"):-len(".meta.json")]
               for p in dist.glob("canary-display-*.meta.json")}
    # The wiring block's `else` is the one display flavor no branch names.
    if "__else__" in pins:
        rest = display - set(pins)
        if len(rest) == 1:
            pins[rest.pop()] = pins.pop("__else__")
        else:
            pins.pop("__else__")
    return allow, pins, display


# ── the lint ────────────────────────────────────────────────────────────────
def _load(path: Path, errors: list[str]):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        errors.append(f"could not read {path}: {e}")
        return None


def lint(devices_dir: Path = DEVICES_DIR, repo: Path = REPO) -> tuple[list[dict], list[str]]:
    """Return (table rows, errors). rows carry the derived confidence."""
    errors: list[str] = []
    rows: list[dict] = []

    def err(msg: str) -> None:
        errors.append(msg)

    def rel(p: Path) -> str:
        try:
            return str(p.relative_to(repo))
        except ValueError:
            return str(p)

    schema = _load(devices_dir / "device.schema.json", errors)
    unclaimed = _load(devices_dir / "unclaimed.json", errors)
    flavors = _load(repo / "firmware" / "flavors.json", errors)
    matrix = _load(repo / "firmware" / "build_matrix.json", errors)
    boards = _load(repo / "firmware" / "boards" / "boards.json", errors)
    figures = _load(repo / "canary-local" / "devices" / "figures.json", errors)
    flash = _load(repo / "canary-local" / "devices" / "flash.json", errors)
    enclosures = _load(repo / "canary-local" / "devices" / "enclosures.json", errors)
    if None in (schema, unclaimed, flavors, matrix, boards, figures, flash, enclosures):
        return rows, errors

    flavor_by_name = {f["name"]: f for f in flavors}
    board_by_id = {b["id"]: b for b in boards}
    figure_by_id = {f["id"]: f for f in figures.get("figures", [])}
    hardware_figure = {h["hardware"]: h["figure"]
                       for h in (figures.get("hardware") or {}).get("mapped", [])}
    flash_by_id = {p["id"]: p for p in flash.get("products", [])}
    set_by_id = {s["id"]: s for s in enclosures.get("sets", [])}
    emu_allow, emu_pins, dist_display = emulator_facts(
        repo / "canary-local" / "emulator" / "build.sh",
        repo / "canary-local" / "emulator" / "dist")

    # ── 1. read + validate every manifest ───────────────────────────────────
    manifests: list[dict] = []
    for d in sorted(p for p in devices_dir.iterdir() if p.is_dir()):
        path = d / "device.json"
        if not path.exists():
            err(f"{rel(d)}/: no device.json (every directory under devices/ is a device)")
            continue
        data = _load(path, errors)
        if data is None:
            continue
        schema_errs = validate(data, schema, rel(path))
        errors.extend(schema_errs)
        if schema_errs:
            continue
        if data["slug"] != d.name:
            err(f"{rel(path)}: slug '{data['slug']}' != directory name '{d.name}'")
        manifests.append(data)

    slugs: dict[str, int] = {}
    for m in manifests:
        slugs[m["slug"]] = slugs.get(m["slug"], 0) + 1
    for slug, n in slugs.items():
        if n > 1:
            err(f"slug '{slug}' appears in {n} manifests")

    # ── 2. per-manifest joins ───────────────────────────────────────────────
    ini_cache: dict[str, dict] = {}
    claimed_env: dict[str, str] = {}
    claimed_flash: dict[str, str] = {}
    claimed_emu: dict[str, str] = {}

    for m in manifests:
        slug, fam, board = m["slug"], m["family"], m["board"]
        board_id = board["board_id"]
        allowed_boards = {board_id, *board.get("variants", [])}
        flavor = flavor_by_name.get(fam)
        sections = None
        if flavor is None:
            err(f"{slug}: family '{fam}' is not a product in firmware/flavors.json "
                f"(known: {', '.join(sorted(flavor_by_name))})")
        else:
            if flavor["dir"] not in ini_cache:
                ini_cache[flavor["dir"]] = load_project_ini(repo / flavor["dir"])
            sections = ini_cache[flavor["dir"]]

        # envs exist, and are claimed once
        for env in board["envs"]:
            if sections is not None and f"env:{env}" not in sections:
                err(f"{slug}: env '{env}' is not an [env:{env}] in "
                    f"{flavor['dir']}/platformio.ini (extra_configs included)")
            if env in claimed_env:
                err(f"{slug}: env '{env}' is also claimed by devices/{claimed_env[env]} — "
                    f"one env belongs to one device")
            else:
                claimed_env[env] = slug

        # board registry row
        row = board_by_id.get(board_id)
        if row is None:
            err(f"{slug}: board_id '{board_id}' is not in firmware/boards/boards.json")
        else:
            if norm_chip(row.get("mcu")) != norm_chip(board["mcu"]):
                err(f"{slug}: board.mcu {board['mcu']} but boards.json {board_id} "
                    f"mcu is {row.get('mcu')}")
            for key in ("psram_mb", "flash_mb"):
                if key in board and board[key] != row.get(key):
                    err(f"{slug}: board.{key} {board[key]} but boards.json {board_id} "
                        f"{key} is {row.get(key)}")
            if sections is not None:
                for env in board["envs"]:
                    if f"env:{env}" not in sections:
                        continue
                    pio = env_pio_board(sections, env)
                    if pio and row.get("pio_board") and pio != row["pio_board"]:
                        err(f"{slug}: [env:{env}] resolves board = {pio}, but boards.json "
                            f"{board_id} pio_board is {row['pio_board']}")
                    stray = env_pins_boards(sections, env) - allowed_boards
                    if stray:
                        err(f"{slug}: [env:{env}] compiles boards/{'/'.join(sorted(stray))}/pins, "
                            f"which is neither board_id {board_id} nor a listed variant")
        for v in board.get("variants", []):
            vrow = board_by_id.get(v)
            if vrow is None:
                err(f"{slug}: board.variants '{v}' is not in boards.json")
            elif norm_chip(vrow.get("mcu")) != norm_chip(board["mcu"]):
                err(f"{slug}: board.variants '{v}' is a {vrow.get('mcu')}, not a {board['mcu']}")

        # peripherals: proven by the pins header
        pins_h = repo / "firmware" / "boards" / board_id / "pins" / "pins.h"
        if pins_h.exists():
            has = {x.lower() for x in re.findall(
                r"^#define\s+HAS_([A-Z0-9_]+)\s+1\b", pins_h.read_text(encoding="utf-8"), re.M)}
            for p in m.get("peripherals", []):
                if p not in has:
                    err(f"{slug}: peripheral '{p}' is not `#define HAS_{p.upper()} 1` in "
                        f"{rel(pins_h)}")
        elif m.get("peripherals"):
            err(f"{slug}: peripherals listed but {rel(pins_h)} does not exist")

        # figure: exists, right role/family, agrees with the hardware map
        confidence = "—"
        fig_id = m.get("figure")
        hw_fig = hardware_figure.get(board_id)
        if fig_id:
            fig = figure_by_id.get(fig_id)
            if fig is None:
                err(f"{slug}: figure '{fig_id}' is not in canary-local/devices/figures.json")
            elif fig.get("role") != "device":
                err(f"{slug}: figure '{fig_id}' has role '{fig.get('role')}', not 'device'")
            else:
                confidence = fig.get("confidence", "?")
                if fig.get("family") and fig["family"] != fam:
                    err(f"{slug}: figure '{fig_id}' belongs to family {fig['family']}, "
                        f"manifest says {fam}")
            if hw_fig and hw_fig != fig_id:
                err(f"{slug}: figure '{fig_id}', but figures.json's hardware map draws "
                    f"boards/{board_id} as {hw_fig}")
        elif hw_fig:
            err(f"{slug}: no figure, but figures.json's hardware map draws boards/{board_id} "
                f"as {hw_fig} — claim it")

        # flasher: products exist, on this chip, for this board, drawn as this figure
        flasher_col = "—"
        fl = m.get("flasher")
        if fl:
            products = [fl["product"], *fl.get("variants", [])]
            flasher_col = fl["product"] + (f" +{len(products) - 1}" if len(products) > 1 else "")
            for pid in products:
                prod = flash_by_id.get(pid)
                if prod is None:
                    err(f"{slug}: flasher product '{pid}' is not in "
                        f"canary-local/devices/flash.json")
                    continue
                if pid in claimed_flash:
                    err(f"{slug}: flasher product '{pid}' is also claimed by "
                        f"devices/{claimed_flash[pid]}")
                else:
                    claimed_flash[pid] = slug
                if norm_chip(prod.get("chip")) != norm_chip(board["mcu"]):
                    err(f"{slug}: flash.json {pid} chip {prod.get('chip')} does not match "
                        f"board.mcu {board['mcu']} — the flasher's chip guard would offer "
                        f"this image to the wrong silicon")
                if "flash_mb" in board and prod.get("flash_mb") not in (None, board["flash_mb"]):
                    err(f"{slug}: flash.json {pid} flash_mb {prod.get('flash_mb')} != "
                        f"board.flash_mb {board['flash_mb']}")
                tier_board = (prod.get("tier") or {}).get("board_id")
                if tier_board and tier_board not in allowed_boards:
                    err(f"{slug}: flash.json {pid} tier.board_id {tier_board} is neither "
                        f"board_id {board_id} nor a listed variant")
                prod_fig = (prod.get("figure") or {}).get("id")
                if prod_fig and prod_fig != fig_id:
                    err(f"{slug}: flash.json {pid} draws figure {prod_fig}, manifest says "
                        f"{fig_id or 'none'}")

        # emulator: allowlisted, built, compiled from this board's pins
        emu_col = "—"
        emu = m.get("emulator")
        if emu:
            fl_name = emu["flavor"]
            emu_col = fl_name
            if fam != "canary-display":
                err(f"{slug}: emulator flavors are canary-display artifacts "
                    f"(dist/canary-display-<flavor>.js); family is {fam}")
            if fl_name not in emu_allow:
                err(f"{slug}: emulator flavor '{fl_name}' is not in canary-local/emulator/"
                    f"build.sh's allowlist ({', '.join(sorted(emu_allow))})")
            if fl_name not in dist_display:
                err(f"{slug}: canary-local/emulator/dist/canary-display-{fl_name}.meta.json "
                    f"does not exist")
            if fl_name in claimed_emu:
                err(f"{slug}: emulator flavor '{fl_name}' is also claimed by "
                    f"devices/{claimed_emu[fl_name]}")
            else:
                claimed_emu[fl_name] = slug
            emu_board = emu_pins.get(fl_name)
            if emu_board and emu_board != board_id:
                err(f"{slug}: build.sh compiles boards/{emu_board}/pins for flavor '{fl_name}', "
                    f"manifest board_id is {board_id}")

        # cad: real files, real sets, and the scad is one of theirs
        cad = m.get("cad")
        if cad:
            scad = repo / cad["scad"]
            if not scad.exists():
                err(f"{slug}: cad.scad {cad['scad']} does not exist")
            sets = cad.get("enclosure_sets", [])
            for s in sets:
                if s not in set_by_id:
                    err(f"{slug}: enclosure set '{s}' is not in "
                        f"canary-local/devices/enclosures.json")
            known = [s for s in sets if s in set_by_id]
            if known and not any(set_by_id[s].get("scad") == scad.name for s in known):
                err(f"{slug}: cad.scad {scad.name} is not the source of any listed enclosure set "
                    f"({', '.join(set_by_id[s].get('scad', '?') for s in known)})")

        rows.append({"slug": slug, "mcu": board["mcu"], "envs": len(board["envs"]),
                     "emulator": emu_col, "confidence": confidence, "flasher": flasher_col})

    # ── 3. completeness: every fact on the other side is claimed ────────────
    unclaimed_envs: dict[str, dict] = {}
    for entry in unclaimed.get("envs", []):
        env = entry.get("env", "")
        if not str(entry.get("reason", "")).strip():
            err(f"devices/unclaimed.json: '{env}' has no reason")
        if env in unclaimed_envs:
            err(f"devices/unclaimed.json: '{env}' listed twice")
        unclaimed_envs[env] = entry
        if env in claimed_env:
            err(f"devices/unclaimed.json lists '{env}', but devices/{claimed_env[env]} claims it — "
                f"remove one")
    all_build_envs = {(f["name"], e) for f in flavors for e in f.get("build_envs", [])}
    build_env_names = {e for _, e in all_build_envs}
    for env in unclaimed_envs:
        if env not in build_env_names:
            err(f"devices/unclaimed.json: '{env}' is not a build_env of any flavors.json product — "
                f"stale entry")
    for fam, env in sorted(all_build_envs):
        if env not in claimed_env and env not in unclaimed_envs:
            err(f"flavors.json {fam} build env '{env}' is claimed by no manifest — add it to a "
                f"devices/<slug>/device.json board.envs, or to devices/unclaimed.json with a "
                f"reason")
    for pid in flash_by_id:
        if pid not in claimed_flash:
            err(f"flash.json product '{pid}' is claimed by no manifest (flasher.product or "
                f"flasher.variants)")
    for fl_name in sorted(dist_display):
        if fl_name not in claimed_emu:
            err(f"emulator dist flavor '{fl_name}' (canary-local/emulator/dist/canary-display-"
                f"{fl_name}.js) is claimed by no manifest")

    # ── 4. build_matrix.json: every product lane has a manifest that agrees ─
    by_slug = {m["slug"]: m for m in manifests}
    for prod in matrix.get("products", []):
        pid = prod["id"]
        if prod.get("hasLevels"):
            envs = [lv.get("env") for lv in (prod.get("levels") or {}).values()]
        else:
            envs = [(prod.get("build") or {}).get("env")]
        envs = [e for e in envs if e]
        target = by_slug.get(pid)
        if target is None:
            flavor_of = prod.get("flavor", pid)
            cands = [m for m in manifests
                     if m["family"] == flavor_of and all(e in m["board"]["envs"] for e in envs)]
            if len(cands) == 1:
                target = cands[0]
            elif not cands:
                err(f"build_matrix.json product '{pid}' has no manifest: no devices/{pid}/ and no "
                    f"{flavor_of} manifest claims env(s) {', '.join(envs) or '(none)'}")
            else:
                err(f"build_matrix.json product '{pid}' matches several manifests by flavor+env: "
                    f"{', '.join(c['slug'] for c in cands)}")
        if target is None:
            continue
        if prod.get("board") and prod["board"] != target["board"]["board_id"]:
            err(f"build_matrix.json product '{pid}' board {prod['board']} != "
                f"devices/{target['slug']} board_id {target['board']['board_id']}")
        if norm_chip(prod.get("mcu")) != norm_chip(target["board"]["mcu"]):
            err(f"build_matrix.json product '{pid}' mcu '{prod.get('mcu')}' != "
                f"devices/{target['slug']} board.mcu {target['board']['mcu']}")
        for e in envs:
            if e not in target["board"]["envs"]:
                err(f"build_matrix.json product '{pid}' builds env '{e}', which "
                    f"devices/{target['slug']} does not list")

    return rows, errors


def format_table(rows: list[dict]) -> str:
    cols = [("slug", "device"), ("mcu", "mcu"), ("envs", "envs"), ("emulator", "emulator"),
            ("confidence", "confidence"), ("flasher", "flasher product")]
    cells = [[str(r[k]) for k, _ in cols] for r in rows]
    widths = [max(len(h), *(len(c[i]) for c in cells)) if cells else len(h)
              for i, (_, h) in enumerate(cols)]
    line = "  ".join(h.ljust(w) for (_, h), w in zip(cols, widths))
    out = [line, "  ".join("-" * w for w in widths)]
    out += ["  ".join(c.ljust(w) for c, w in zip(row, widths)) for row in cells]
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    rows, errors = lint()
    print(format_table(rows))
    print()
    print("confidence is read from canary-local/devices/figures.json (derived from evidence on "
          "disk); '—' = no figure draws this hardware yet")
    if errors:
        print(f"\nlint_device_manifests.py: {len(errors)} problem(s):\n")
        for e in errors:
            print(f"  ✗ {e}")
        return 1
    print(f"\nlint_device_manifests.py: OK — {len(rows)} device manifests; every env, board, "
          f"figure, flasher, emulator and CAD join holds.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
