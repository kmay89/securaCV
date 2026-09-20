"""scripts/_device_join.py — the joins between devices/<slug>/device.json and the
firmware tree, defined ONCE for every lint that needs them.

Two linters read the device manifests: lint_device_manifests.py (every join,
the full gate) and lint_build_matrix.py (the build matrix's side of the same
join — a product with no manifest, a manifest naming an env its PlatformIO
project never defines). They must agree on what an env IS and which manifest a
matrix product resolves to, so those answers live here and nowhere else:

  parse_ini / load_project_ini / resolve_key / expand
      a grep-grade reader for a PlatformIO project: platformio.ini plus every
      file its `extra_configs` names, `extends` chains, the [env] base table
      and `${section.key}` interpolation. No PlatformIO install needed.
  env_pins_boards / env_pio_board
      what one [env:NAME] compiles: its `-I boards/<id>/pins` includes and its
      resolved `board =`.
  norm_chip
      esptool's chip vocabulary (ESP32, ESP32-S3, ESP32-C3, ESP32-C6) from
      any of the spellings the other files use.
  load_manifests
      every devices/<slug>/device.json as a dict, plus the errors for the ones
      that are not JSON objects with a slug. Schema validation is the manifest
      linter's job; this only guarantees the shape callers index into.
  matrix_product_envs / manifest_for_matrix_product
      which env(s) a firmware/build_matrix.json product builds, and the ONE
      manifest it resolves to: devices/<product id>/ when it exists, else the
      single manifest of its flavor that claims every env it builds.
  unknown_envs
      the board.envs a manifest lists that are not an [env:NAME] of its
      family's project.

Importable as `from _device_join import ...` because the linters run as
`python3 scripts/<lint>.py`, which puts this directory at sys.path[0]; the
unit tests in scripts/tests/ insert the same directory themselves.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

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


# ── the manifests ───────────────────────────────────────────────────────────
def load_manifests(devices_dir: Path) -> tuple[list[dict], list[str]]:
    """Every devices/<slug>/device.json, in directory order, plus the errors
    for the ones that cannot be used: unreadable, not JSON, not an object,
    or without a string `slug` / `family` / `board.envs`. Callers index into
    those three keys; everything else is the schema's business."""
    manifests: list[dict] = []
    errors: list[str] = []
    if not devices_dir.is_dir():
        return manifests, [f"{devices_dir}: no such directory"]
    for d in sorted(p for p in devices_dir.iterdir() if p.is_dir()):
        path = d / "device.json"
        if not path.exists():
            errors.append(f"{d.name}/: no device.json (every directory under devices/ is a device)")
            continue
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            errors.append(f"could not read {path}: {e}")
            continue
        shape_ok = (isinstance(data, dict)
                    and isinstance(data.get("slug"), str)
                    and isinstance(data.get("family"), str)
                    and isinstance(data.get("board"), dict)
                    and isinstance(data["board"].get("envs"), list))
        if not shape_ok:
            errors.append(f"{path}: not a device manifest (needs slug, family, board.envs)")
            continue
        manifests.append(data)
    return manifests, errors


def matrix_product_envs(prod: dict) -> list[str]:
    """The env(s) a firmware/build_matrix.json product builds: one per level
    for a leveled product, else its build.env. Never None entries."""
    if prod.get("hasLevels"):
        envs = [lv.get("env") for lv in (prod.get("levels") or {}).values()]
    else:
        envs = [(prod.get("build") or {}).get("env")]
    return [e for e in envs if e]


def manifest_for_matrix_product(prod: dict, manifests: list[dict]) -> tuple[dict | None, str | None]:
    """(manifest, None) for the one manifest a build_matrix.json product
    resolves to, or (None, why) when there is none or several.

    Resolution: devices/<product id>/ when a manifest carries that slug (most
    products ARE a device); otherwise the single manifest of the product's
    flavor (`flavor`, else its id) that claims every env the product builds —
    the board-specialized lanes (the classic-ESP32 reach ports, the display's
    watch lane) resolve this way."""
    pid = prod["id"]
    envs = matrix_product_envs(prod)
    by_slug = {m["slug"]: m for m in manifests}
    if pid in by_slug:
        return by_slug[pid], None
    flavor_of = prod.get("flavor", pid)
    cands = [m for m in manifests
             if m["family"] == flavor_of and all(e in m["board"]["envs"] for e in envs)]
    if len(cands) == 1:
        return cands[0], None
    if not cands:
        return None, (f"build_matrix.json product '{pid}' has no manifest: no devices/{pid}/ and no "
                      f"{flavor_of} manifest claims env(s) {', '.join(envs) or '(none)'}")
    return None, (f"build_matrix.json product '{pid}' matches several manifests by flavor+env: "
                  f"{', '.join(c['slug'] for c in cands)}")


def unknown_envs(manifest: dict, sections: dict[str, dict[str, str]]) -> list[str]:
    """The board.envs this manifest lists that are not an [env:NAME] in the
    project's resolved ini (`sections` from load_project_ini)."""
    return [env for env in manifest["board"]["envs"] if f"env:{env}" not in sections]
