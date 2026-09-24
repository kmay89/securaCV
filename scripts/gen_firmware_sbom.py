#!/usr/bin/env python3
"""scripts/gen_firmware_sbom.py — the firmware SBOM, derived from the build inputs.

sbom.yml used to write sbom-firmware.cdx.json from a heredoc typed into the
workflow: Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7, with a hand-picked list of
IDF components, for "the Canary firmware". By the time anyone looked, the
shipped canary-display and canary-wap images were building on the pioarduino
core 3.3.8 / ESP-IDF 5.5.4 and nothing in that template knew. A bill of
materials that cannot go stale in step with the build is a false compliance
record with a timestamp on it.

This script derives the document from the files the build actually reads:

  firmware/flavors.json                    which products exist, their dir,
                                           the envs CI builds and ships
  <product>/platformio.ini + extra_configs the resolved config of every build
                                           env — platform, framework, board,
                                           lib_deps, lib_extra_dirs — through
                                           a small PlatformIO-ini resolver
                                           (extends, extra_configs,
                                           ${section.option} interpolation);
                                           `--verify-with-pio` proves the
                                           resolver against PlatformIO's own
                                           `pio project config --json-output`
  firmware/envs/platformio/platforms.ini   the ONE place a platform literal
                                           lives (firmware/PLATFORMS.md); an
                                           env's platform is mapped back to
                                           its section
  firmware/common/*/library.{json,properties}
  firmware/canary/lib/*/library.json       the first-party libraries the
                                           images are built from
  .github/workflows/*.yml                  the esp32:esp32 core versions the
                                           Arduino-CLI build path pins through
                                           .github/actions/setup-arduino-esp32
                                           (each row's library pins and the
                                           sketch it compiles come along)
  firmware/projects/*/arduino/*/sketch.yaml
                                           the other Arduino axis: the core
                                           and library pins of every sketch
                                           profile, parsed from YAML
  firmware/canary/include/canary_config.h  FIRMWARE_VERSION — the one train
                                           (scripts/lint_fw_version_sync.sh
                                           holds the other six copies to it)

The two Arduino axes must agree, and the agreement is asserted here instead
of in firmware.yml's "keep the two in lockstep" comment (check_core_pin_
agreement below; a finding fails generation naming the row, the profile and
the remedy):
  (a) every exact core-version a workflow row pins is pinned by some sketch
      profile;
  (b) every core a sketch profile pins is a workflow pin, or the Arduino core
      inside that product's PlatformIO platform (PLATFORM_FACTS) — the WAP
      sketch's 3.3.8 tracks the pioarduino platform while its Arduino-CLI
      rows build on the action's "latest", a recorded release decision
      (firmware/PLATFORMS.md), not a drift;
  (c) every library a workflow row pins on its core line (GFX / lvgl / NimBLE
      split their majors along the core boundary) is pinned to the same
      version by every profile, of a product that row builds, on that core.
A row on "latest" has nothing to agree with, and a sketch may pin more
libraries than its row does; neither is a finding.

What is STILL declared by hand — and where, so it is reviewable:
  PLATFORM_FACTS below maps each platform literal to the Arduino core and
  ESP-IDF release it packages. That fact is inside the platform package, not
  in any file of ours; it is typed here ONCE, keyed by the exact literal, so a
  pin bump in platforms.ini fails this script ("no facts for …") instead of
  quietly carrying the old numbers forward. The versions of the libraries
  ESP-IDF bundles (FreeRTOS, mbedTLS, lwIP, cJSON) are NOT listed: they are
  not pinned by anything in this tree and the old template's numbers were a
  guess dressed as a record. lib_deps are emitted as the requirement the ini
  states (exact when the spec is exact, a `securacv:version_spec` property
  when it is a range) — PlatformIO has no lockfile, so the resolved version is
  only known to the build that ran.

Determinism: sorted components, dependencies and properties; a serialNumber
derived (UUIDv5) from the content; NO timestamp unless `--timestamp` is given,
which sbom.yml does for the uploaded artifact and the committed copy never
carries. `--check` regenerates in memory and byte-compares against
sbom/sbom-firmware.cdx.json (lint.yml runs it on every PR).

`--validate` holds the rendered document to the CycloneDX 1.5 JSON schema in
strict mode (cyclonedx-python-lib's JsonStrictValidator, schema bundled, no
network) — read-only, like `--check`, and run before it compares; a `--out`
or `--timestamp` beside it without `--check` is refused, not dropped — or,
given a FILE, holds that file's bytes to it (sbom.yml's timestamped
artifact). The file's `specVersion` must be 1.5 as well: the schema types it
as a free string, so a document claiming 1.6 would otherwise pass as "a
strict 1.5 document". The schema does not check graph closure; the unit
tests and sbom.yml's jq step do.

Usage:
    python3 scripts/gen_firmware_sbom.py              # rewrite the committed file
    python3 scripts/gen_firmware_sbom.py --check      # byte gate
    python3 scripts/gen_firmware_sbom.py --check --validate   # what lint.yml runs
    python3 scripts/gen_firmware_sbom.py --validate FILE      # an artifact's bytes
    python3 scripts/gen_firmware_sbom.py --out X --timestamp now
    python3 scripts/gen_firmware_sbom.py --verify-with-pio   # needs `pio`

stdlib only, plus PyYAML for the two Arduino axes (workflow YAML, sketch.yaml)
and, for --validate only, cyclonedx-python-lib with its json-validation extra
(VALIDATOR_PIP below — pinned exactly: the extra brings the format checkers
that refuse an IRI with a space, and a library bump can tighten them).
"""

from __future__ import annotations

import argparse
import difflib
import hashlib
import json
import re
import subprocess
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import quote, urlparse

REPO = Path(__file__).resolve().parents[1]
FLAVORS = REPO / "firmware" / "flavors.json"
PLATFORMS_INI = REPO / "firmware" / "envs" / "platformio" / "platforms.ini"
COMMON_DIR = REPO / "firmware" / "common"
CANARY_LIB_DIR = REPO / "firmware" / "canary" / "lib"
WORKFLOWS = REPO / ".github" / "workflows"
ARDUINO_ACTION = "./.github/actions/setup-arduino-esp32"
PROJECTS = REPO / "firmware" / "projects"
SKETCH_GLOB = "*/arduino/*/sketch.yaml"
VERSION_HEADER = REPO / "firmware" / "canary" / "include" / "canary_config.h"
OUT = REPO / "sbom" / "sbom-firmware.cdx.json"

SPEC_VERSION = "1.5"
# 1.1.0: the sketch.yaml profiles became build paths of their core, and the
# `securacv:check` property names the schema gate.
GENERATOR_VERSION = "1.1.0"
ROOT_REF = "securacv-firmware"
# The exact spec lint.yml and sbom.yml install for --validate. The extra is
# load-bearing (jsonschema[format-nongpl]: the iri-reference checker), and
# the schema files ship inside the wheel, so a bump is a reviewed change to
# what "valid" means — never a float.
VALIDATOR_PIP = "cyclonedx-python-lib[json-validation]==11.12.0"
# UUIDv5 namespace for the serialNumber: content-addressed, so the same tree
# always yields the same document and a different tree a different one.
SERIAL_NAMESPACE = uuid.uuid5(uuid.NAMESPACE_URL,
                              "https://github.com/kmay89/securaCV/sbom/firmware")

# ---------------------------------------------------------------------------
# Hand-declared facts, keyed by the exact `platform =` literal in
# platforms.ini. A literal that is in use and not here fails generation.
# Numbers: firmware/PLATFORMS.md (the pin table), which cites the bench-
# validated parity docs for the core-2 line and the pioarduino release notes
# for the core-3 line. `exact` says whether the literal pins one platform
# release; a floating spec resolves at build time, so its core is what that
# platform line ships TODAY, not a promise.
PLATFORM_FACTS: dict[str, dict] = {
    "espressif32@6.9.0": {
        "name": "platformio/espressif32",
        "version": "6.9.0",
        "exact": True,
        "arduino_core": "2.0.17",
        "esp_idf": "4.4.7",
        "website": "https://registry.platformio.org/platforms/platformio/espressif32",
    },
    "https://github.com/pioarduino/platform-espressif32/releases/download/"
    "55.03.38-1/platform-espressif32.zip": {
        "name": "pioarduino/platform-espressif32",
        "version": "55.03.38-1",
        "exact": True,
        "arduino_core": "3.3.8",
        "esp_idf": "5.5.4",
        "website": "https://github.com/pioarduino/platform-espressif32",
    },
}

LICENSES = {
    # SPDX ids for the two frameworks; declared, not read from the packages.
    "arduino-esp32": "LGPL-2.1-or-later",
    "esp-idf": "Apache-2.0",
}

# ---------------------------------------------------------------------------
# A small PlatformIO project-config resolver.

_SECTION_RE = re.compile(r"^\s*\[([^\]]+)\]\s*$")
_OPTION_RE = re.compile(r"^([A-Za-z0-9_.\-]+)\s*=\s*(.*?)\s*$")
_INTERP_RE = re.compile(r"\$\{([^.}]+)\.([^}]+)\}")
_GLOB_CHARS = set("*?[")


def _strip_inline_comment(value: str) -> str:
    """PlatformIO (configparser) treats ' ;' / ' #' after a value as a comment."""
    for marker in (" ;", " #", "\t;", "\t#"):
        idx = value.find(marker)
        if idx >= 0:
            value = value[:idx]
    return value.strip()


def _lines(value: str) -> list[str]:
    return [ln.strip() for ln in value.splitlines() if ln.strip()]


def parse_ini(path: Path) -> dict[str, dict[str, str]]:
    """{section: {option: raw value}} — multi-line values joined with '\\n',
    full-line comments (indented or not) dropped, inline comments stripped."""
    sections: dict[str, dict[str, str]] = {}
    section: str | None = None
    key: str | None = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        if not raw.strip() or raw.lstrip().startswith((";", "#")):
            continue
        m = _SECTION_RE.match(raw)
        if m:
            section = m.group(1).strip()
            sections.setdefault(section, {})
            key = None
            continue
        if section is None:
            continue
        if raw[:1] in (" ", "\t"):
            if key is not None:
                cont = _strip_inline_comment(raw.strip())
                if cont:
                    sections[section][key] = f"{sections[section][key]}\n{cont}"
            continue
        m = _OPTION_RE.match(raw)
        if m:
            key = m.group(1)
            sections[section][key] = _strip_inline_comment(m.group(2))
    return sections


class PioConfig:
    """Enough of PlatformIO's ProjectConfig to answer what an env builds on.

    Reads platformio.ini, then every `[platformio] extra_configs` entry in
    order (later files override earlier ones for the same section/option, as
    PlatformIO's read order does). `resolve(section)` applies the implicit
    `[env]` base to every `[env:*]`, then `extends` parents (comma-separated,
    recursive), then the section's own options, and interpolates
    `${section.option}` — section may contain ':' — from the referenced
    section's RESOLVED value. `${sysenv.X}`, `${platformio.X}` and anything
    without a section are left as written; nothing here needs them.
    """

    def __init__(self, project_dir: Path):
        self.project_dir = project_dir
        self.sections: dict[str, dict[str, str]] = {}
        self._merge(parse_ini(project_dir / "platformio.ini"))
        extra = self.sections.get("platformio", {}).get("extra_configs", "")
        for pattern in _lines(extra):
            for path in self._expand(pattern):
                self._merge(parse_ini(path))
        self._cache: dict[str, dict[str, str]] = {}

    def _expand(self, pattern: str) -> list[Path]:
        if any(c in pattern for c in _GLOB_CHARS):
            base = self.project_dir / pattern
            return sorted(base.parent.glob(base.name))
        path = self.project_dir / pattern
        if not path.exists():
            raise SystemExit(f"gen_firmware_sbom.py: {self.project_dir}: extra_configs "
                             f"names {pattern}, which does not exist")
        return [path]

    def _merge(self, parsed: dict[str, dict[str, str]]) -> None:
        for section, options in parsed.items():
            self.sections.setdefault(section, {}).update(options)

    def envs(self) -> list[str]:
        return [s[len("env:"):] for s in self.sections if s.startswith("env:")]

    def resolve(self, section: str, _depth: int = 0) -> dict[str, str]:
        if section in self._cache:
            return self._cache[section]
        if _depth > 20:
            raise SystemExit(f"gen_firmware_sbom.py: `extends` cycle at [{section}]")
        raw = self.sections.get(section)
        if raw is None:
            raise SystemExit(f"gen_firmware_sbom.py: {self.project_dir}: no section "
                             f"[{section}]")
        merged: dict[str, str] = {}
        if section.startswith("env:") and "env" in self.sections:
            merged.update(self.resolve("env", _depth + 1))
        for parent in [p.strip() for p in raw.get("extends", "").split(",") if p.strip()]:
            merged.update(self.resolve(parent, _depth + 1))
        merged.update({k: v for k, v in raw.items() if k != "extends"})
        resolved = {k: self._interpolate(v, _depth) for k, v in merged.items()}
        self._cache[section] = resolved
        return resolved

    def _interpolate(self, value: str, depth: int) -> str:
        def sub(m: re.Match) -> str:
            sec, opt = m.group(1), m.group(2)
            if sec in ("sysenv", "platformio") or sec not in self.sections:
                return m.group(0)
            return self.resolve(sec, depth + 1).get(opt, m.group(0))
        prev = None
        while prev != value:
            prev, value = value, _INTERP_RE.sub(sub, value)
        return value

    def env(self, name: str) -> dict[str, str]:
        return self.resolve(f"env:{name}")


# ---------------------------------------------------------------------------
# Inputs.

def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def firmware_version() -> str:
    m = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"',
                  VERSION_HEADER.read_text(encoding="utf-8"))
    if not m:
        raise SystemExit(f"gen_firmware_sbom.py: no FIRMWARE_VERSION in "
                         f"{VERSION_HEADER.relative_to(REPO)}")
    return m.group(1)


def platform_sections() -> dict[str, str]:
    """{literal: section} from platforms.ini."""
    out: dict[str, str] = {}
    for section, options in parse_ini(PLATFORMS_INI).items():
        if section.startswith("platform_") and "platform" in options:
            out[options["platform"]] = section
    return out


_SPEC_SPLIT_RE = re.compile(r"\s*@\s*")
_RANGE_CHARS = set("^~<>=*!|, ")


def parse_lib_spec(spec: str) -> dict:
    """One lib_deps line → {group, name, spec, exact, url}.

    `owner/Name @ ^1.2` → group owner, name Name, spec ^1.2 (a range).
    `owner/Name@1.2.3`  → exact.
    a URL (zip/git)     → name from the last path segment before /archive|.git,
                          group from a github owner when there is one, version
                          from a /tags/vX.Y.Z or #tag when there is one.
    """
    spec = spec.strip()
    if "://" in spec:
        url = spec
        version = None
        m = re.search(r"/archive/refs/tags/v?([0-9][^/]*?)\.(?:zip|tar\.gz)$", url)
        if m:
            version = m.group(1)
        elif "#" in url:
            version = url.rsplit("#", 1)[1].lstrip("v") or None
        path = url.split("://", 1)[1]
        parts = [p for p in path.split("/") if p]
        gh = re.match(r"^(?:www\.)?github\.com$", parts[0]) if parts else None
        group = parts[1] if gh and len(parts) > 2 else None
        name = parts[2] if gh and len(parts) > 2 else parts[-1]
        name = re.sub(r"\.(git|zip|tar\.gz)$", "", name.split("#")[0])
        return {"group": group, "name": name, "spec": version, "exact": version is not None,
                "url": url.split("#")[0]}
    if "@" in spec:
        name_part, version = _SPEC_SPLIT_RE.split(spec, 1)
    else:
        name_part, version = spec, ""
    group, _, name = name_part.rpartition("/")
    version = version.strip()
    exact = bool(version) and not (set(version) & _RANGE_CHARS)
    return {"group": group or None, "name": name, "spec": version or None,
            "exact": exact, "url": None}


def env_facts(product: dict, cfg: PioConfig, env: str) -> dict:
    opts = cfg.env(env)
    return {
        "platform": opts.get("platform", ""),
        "framework": _lines(opts.get("framework", "")),
        "board": opts.get("board", ""),
        "lib_deps": _lines(opts.get("lib_deps", "")),
        "lib_extra_dirs": _lines(opts.get("lib_extra_dirs", "")),
        "reaches_common": any(
            "common" in opts.get(k, "") for k in ("lib_extra_dirs", "build_flags",
                                                  "build_src_filter")),
    }


def first_party_libs(base: Path, prefix: str) -> list[dict]:
    """One entry per library dir with a manifest — library.json first,
    library.properties when that is all there is."""
    libs: list[dict] = []
    for d in sorted(p for p in base.iterdir() if p.is_dir()):
        manifest = d / "library.json"
        props = d / "library.properties"
        if manifest.exists():
            data = load_json(manifest)
            name, version, license_id = data.get("name"), data.get("version"), data.get("license")
            src = manifest
        elif props.exists():
            kv = dict(ln.split("=", 1) for ln in _lines(props.read_text(encoding="utf-8"))
                      if "=" in ln)
            name, version, license_id = kv.get("name"), kv.get("version"), kv.get("license")
            src = props
        else:
            continue
        if not name or not version:
            raise SystemExit(f"gen_firmware_sbom.py: {src.relative_to(REPO)} lacks a "
                             f"name or version")
        libs.append({"ref": f"{prefix}:{d.name}", "name": str(name),
                     "version": str(version), "license": license_id,
                     "path": str(d.relative_to(REPO)),
                     "manifest": str(src.relative_to(REPO))})
    return libs


_MATRIX_EXPR = re.compile(r"^\$\{\{\s*matrix\.([A-Za-z0-9_-]+)\.([A-Za-z0-9_-]+)\s*\}\}$")
_MATRIX_ANY = re.compile(r"\$\{\{\s*matrix\.([A-Za-z0-9_-]+)\.([A-Za-z0-9_-]+)\s*\}\}")
# The sketch a workflow row goes on to compile: an explicit `.ino` under a
# product's arduino/ dir, or a `cd` into the sketch dir before a relative
# compile — the two shapes the Arduino jobs use. A `.py` or header path
# under the same dir (a regen check run from the display job) is neither.
_INO_TARGET_RE = re.compile(
    r"firmware/projects/([A-Za-z0-9_.-]+)/arduino/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\.ino\b")
_CD_SKETCH_RE = re.compile(r"\bcd\s+firmware/projects/([A-Za-z0-9_.-]+)/arduino/[A-Za-z0-9_.-]+\b")


def _yaml():
    try:
        import yaml  # noqa: WPS433 — optional dependency, only for these passes
    except ImportError as exc:  # pragma: no cover — exercised by hand
        raise SystemExit("gen_firmware_sbom.py: PyYAML is needed to read the Arduino "
                         "core pins out of .github/workflows and the sketch.yaml "
                         "profiles (pip install pyyaml)") from exc
    return yaml


def _resolve_matrix(text: str, axis: str, entry: dict) -> str:
    """`${{ matrix.<axis>.<key> }}` → entry[key]; other expressions stay."""
    return _MATRIX_ANY.sub(
        lambda m: str(entry[m.group(2)]) if m.group(1) == axis and m.group(2) in entry
        else m.group(0), text)


def _library_pins(lines: list[str], where: str) -> dict[str, str | None]:
    """The composite action's `libraries` input (`Name` / `Name@1.2.3`, one
    per line) → {name: version|None}. An unresolved expression is refused,
    not read as a name."""
    pins: dict[str, str | None] = {}
    for line in lines:
        if "${{" in line:
            raise SystemExit(f"gen_firmware_sbom.py: {where} pins a library with an "
                             f"expression this script cannot resolve: {line}")
        name, _, version = line.partition("@")
        pins[name.strip()] = version.strip() or None
    return pins


def _products_built(steps: list[dict]) -> list[str]:
    """The products whose sketch these steps' run: blocks compile."""
    found: set[str] = set()
    for step in steps:
        for line in str(step.get("run") or "").splitlines():
            if line.strip().startswith("#"):
                continue
            for rx in (_INO_TARGET_RE, _CD_SKETCH_RE):
                found.update(rx.findall(line))
    return sorted(found)


def arduino_core_pins() -> list[dict]:
    """[{version|None, where, libraries, products}] for every
    setup-arduino-esp32 call in the workflows. A literal `core-version` is a
    pin; `${{ matrix.a.b }}` resolves through the job's strategy.matrix, and
    the row's library pins through the same matrix entry; empty means the
    action's "latest". `products` are the sketches the row goes on to
    compile — read off the run: blocks up to the job's next core install,
    because the release jobs install the WAP's core and both display cores
    in one job. A row that names no sketch is refused: the SBOM could not
    say which product's sketch.yaml it must agree with."""
    yaml = _yaml()
    pins: list[dict] = []
    for path in sorted(WORKFLOWS.glob("*.yml")):
        wf = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        for job, spec in (wf.get("jobs") or {}).items():
            spec = spec or {}
            steps = [s or {} for s in spec.get("steps") or []]
            for i, step in enumerate(steps):
                if step.get("uses") != ARDUINO_ACTION:
                    continue
                where = f"{path.name}:{job}"
                following: list[dict] = []
                for later in steps[i + 1:]:
                    if later.get("uses") == ARDUINO_ACTION:
                        break
                    following.append(later)
                products = _products_built(following)
                if not products:
                    raise SystemExit(
                        f"gen_firmware_sbom.py: {where} installs an esp32:esp32 core but no "
                        f"later step in the job names the sketch it compiles (a "
                        f"firmware/projects/<product>/arduino/<sketch>/*.ino path or a `cd` "
                        f"into the sketch dir) — the SBOM cannot tell which product's "
                        f"sketch.yaml this row must agree with")
                with_ = step.get("with") or {}
                raw = str(with_.get("core-version") or "").strip()
                lib_lines = _lines(str(with_.get("libraries") or ""))
                m = _MATRIX_EXPR.match(raw)
                if m:
                    axis, key = m.groups()
                    entries = ((spec.get("strategy") or {}).get("matrix") or {}).get(axis) or []
                    for entry in entries:
                        if isinstance(entry, dict) and key in entry:
                            resolved = [_resolve_matrix(ln, axis, entry) for ln in lib_lines]
                            pins.append({"version": str(entry[key]), "where": where,
                                         "libraries": _library_pins(resolved, where),
                                         "products": products})
                    continue
                if "${{" in raw:
                    raise SystemExit(f"gen_firmware_sbom.py: {where} passes core-version "
                                     f"as an expression this script cannot resolve: {raw}")
                pins.append({"version": raw or None, "where": where,
                             "libraries": _library_pins(lib_lines, where),
                             "products": products})
    return pins


_SKETCH_PLATFORM_RE = re.compile(r"^esp32:esp32 \((\d+\.\d+\.\d+)\)$")
_SKETCH_LIB_RE = re.compile(r"^(.*?)\s*\(([^()\s]+)\)$")


def sketch_core_pins(flavors: list[dict]) -> list[dict]:
    """[{version, product, sketch, profile, where, libraries}] for every
    profile of every firmware/projects/*/arduino/*/sketch.yaml.

    The pin is the profile's `platform: esp32:esp32 (X.Y.Z)` line, parsed
    from YAML and never grepped — the files' comments quote core numbers
    too. `libraries` is {name: version|None} from `Name (1.2.3)` / `Name`.
    The product is the flavors.json entry whose dir holds the sketch. A
    file or profile this reader cannot spell (no product, no profiles, not
    exactly one platform, a platform not written that way) is refused, not
    skipped: a --check that read past it would be green on a pin it never
    saw.
    """
    yaml = _yaml()
    by_dir = {REPO / p["dir"]: p["name"] for p in flavors}
    pins: list[dict] = []
    for path in sorted(PROJECTS.glob(SKETCH_GLOB)):
        rel = path.relative_to(REPO)
        product = next((name for d, name in by_dir.items() if path.is_relative_to(d)), None)
        if product is None:
            raise SystemExit(f"gen_firmware_sbom.py: {rel} is under no product's dir in "
                             f"firmware/flavors.json")
        doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        profiles = doc.get("profiles") or {}
        if not profiles:
            raise SystemExit(f"gen_firmware_sbom.py: {rel} declares no profiles — every "
                             f"sketch.yaml under a product dir pins its core in a profile, "
                             f"and a profile-less file would shrink the axis the workflow "
                             f"pins are checked against to nothing; add the profile, or "
                             f"extend sketch_core_pins() if such a file is now legal")
        for profile, body in profiles.items():
            body = body or {}
            where = f"{rel} ({profile})"
            platforms = [p or {} for p in body.get("platforms") or []]
            if len(platforms) != 1:
                raise SystemExit(f"gen_firmware_sbom.py: {where} lists {len(platforms)} "
                                 f"platforms; one `esp32:esp32 (X.Y.Z)` line is what this "
                                 f"reader spells — extend sketch_core_pins() before adding "
                                 f"a second")
            m = _SKETCH_PLATFORM_RE.match(str(platforms[0].get("platform") or "").strip())
            if not m:
                raise SystemExit(f"gen_firmware_sbom.py: {where}: platform is not written "
                                 f"`esp32:esp32 (X.Y.Z)`: {platforms[0].get('platform')!r}")
            libraries: dict[str, str | None] = {}
            for entry in body.get("libraries") or []:
                lm = _SKETCH_LIB_RE.match(str(entry).strip())
                libraries[lm.group(1) if lm else str(entry).strip()] = lm.group(2) if lm else None
            pins.append({"version": m.group(1), "product": product, "sketch": path.parent.name,
                         "profile": str(profile), "where": where, "libraries": libraries})
    return pins


def check_core_pin_agreement(workflow_pins: list[dict], sketch_pins: list[dict],
                             product_cores: dict[str, set[str]]) -> list[str]:
    """The agreement between the two Arduino axes, as findings (empty: holds).

    (a) every exact core a workflow row pins is pinned by a sketch profile of
        a product that row builds;
    (b) every core a sketch profile pins is a pin of a workflow row that builds
        that product, or an Arduino core inside that product's PlatformIO
        platform(s) — `product_cores`, from PLATFORM_FACTS — so a sketch may
        track either build path;
    (c) every library a workflow row pins on its core line is pinned to the
        same version by every profile, of a product that row builds, on that
        core (firmware.yml's "keep the two in lockstep", made structural).
    Not asserted, on purpose: a row on the action's "latest" has no version
    to agree with, and a sketch may pin libraries its row leaves floating.
    """
    findings: list[str] = []
    # Both directions are scoped to the product a row builds: a WAP row's pin
    # is answered only by a WAP profile, and a WAP profile only by a WAP row
    # (or the WAP's own PlatformIO core). A repository-wide set let one
    # product's profile answer for another's row — the review round on #1686
    # showed a WAP row moved to 3.3.10 staying green on the display's 3.3.10
    # profile — which is exactly the drift this gate exists to catch.
    for wp in workflow_pins:
        if not wp["version"]:
            continue
        of = {sp["version"] for sp in sketch_pins if sp["product"] in wp["products"]}
        if wp["version"] not in of:
            findings.append(f"{wp['where']} pins esp32:esp32 {wp['version']} for "
                            f"{', '.join(wp['products'])}, which no sketch.yaml profile of "
                            f"that product pins (its profiles pin: "
                            f"{', '.join(sorted(of)) or 'nothing'})")
    for sp in sketch_pins:
        rows = {wp["version"] for wp in workflow_pins
                if wp["version"] and sp["product"] in wp["products"]}
        cores = product_cores.get(sp["product"], set())
        if sp["version"] not in rows | cores:
            findings.append(f"{sp['where']} pins esp32:esp32 {sp['version']}, which is neither "
                            f"a core-version pin of a workflow row that builds {sp['product']} "
                            f"({', '.join(sorted(rows)) or 'none'}) nor the Arduino core of "
                            f"its PlatformIO platform ({', '.join(sorted(cores)) or 'none'})")
    for wp in workflow_pins:
        if not wp["version"]:
            continue
        pinned = {name: ver for name, ver in wp["libraries"].items() if ver}
        for sp in sketch_pins:
            if sp["version"] != wp["version"] or sp["product"] not in wp["products"]:
                continue
            for name, ver in sorted(pinned.items()):
                got = sp["libraries"].get(name)
                if got != ver:
                    findings.append(f"{sp['where']} rides esp32:esp32 {sp['version']} with "
                                    f"{name} {got or 'unpinned'}, but {wp['where']} pins "
                                    f"{name}@{ver} on that core line")
    return findings


# ---------------------------------------------------------------------------
# The document.

def prop(name: str, value) -> dict:
    return {"name": name, "value": str(value)}


def props(d: dict) -> list[dict]:
    return [prop(k, v) for k, v in sorted(d.items())]


def build_document(timestamp: str | None = None) -> dict:
    flavors = load_json(FLAVORS)
    version = firmware_version()
    sections = platform_sections()

    components: dict[str, dict] = {}
    depends: dict[str, set[str]] = {ROOT_REF: set()}

    def add(ref: str, component: dict) -> None:
        components[ref] = dict(component, **{"bom-ref": ref})
        depends.setdefault(ref, set())

    def dep(a: str, b: str) -> None:
        depends.setdefault(a, set()).add(b)

    platform_envs: dict[str, list[str]] = {}
    product_cores: dict[str, set[str]] = {}   # product → Arduino cores of its platforms
    lib_envs: dict[str, list[str]] = {}
    lib_meta: dict[str, dict] = {}
    common = first_party_libs(COMMON_DIR, "common")
    canary_libs = first_party_libs(CANARY_LIB_DIR, "canarylib")
    for lib in common + canary_libs:
        comp = {
            "type": "library",
            "group": "securacv",
            "name": lib["name"],
            "version": lib["version"],
            "properties": props({
                "securacv:path": lib["path"],
                "securacv:manifest": lib["manifest"],
                "securacv:first_party": "true",
            }),
        }
        if lib["license"]:
            comp["licenses"] = [{"license": {"id": lib["license"]}}]
        add(lib["ref"], comp)

    for product in flavors:
        name, pdir = product["name"], REPO / product["dir"]
        cfg = PioConfig(pdir)
        pref = f"product:{name}"
        build_envs = list(product.get("build_envs") or [])
        reaches_common = False
        boards: set[str] = set()
        for env in build_envs:
            facts = env_facts(product, cfg, env)
            literal = facts["platform"]
            section = sections.get(literal)
            if section is None:
                raise SystemExit(
                    f"gen_firmware_sbom.py: {name}:{env} builds on platform "
                    f"'{literal}', which is not a section of "
                    f"{PLATFORMS_INI.relative_to(REPO)} — lint_platform_pins.py "
                    f"should have refused this")
            if literal not in PLATFORM_FACTS:
                raise SystemExit(
                    f"gen_firmware_sbom.py: no PLATFORM_FACTS for platform literal "
                    f"'{literal}' ([{section}] in platforms.ini) — the pin moved; "
                    f"add the Arduino core / ESP-IDF release it packages "
                    f"(firmware/PLATFORMS.md) before regenerating the SBOM")
            platform_envs.setdefault(section, []).append(f"{name}:{env}")
            product_cores.setdefault(name, set()).add(PLATFORM_FACTS[literal]["arduino_core"])
            dep(pref, f"platform:{section}")
            boards.add(facts["board"])
            reaches_common = reaches_common or facts["reaches_common"]
            for spec in facts["lib_deps"]:
                lib = parse_lib_spec(spec)
                key = (f"lib:{lib['group']}/{lib['name']}" if lib["group"]
                       else f"lib:{lib['name']}")
                if lib["spec"]:
                    key += f"@{lib['spec']}"
                lib_envs.setdefault(key, []).append(f"{name}:{env}")
                lib_meta[key] = lib
                dep(pref, key)
        add(pref, {
            "type": "firmware",
            "group": "securacv",
            "name": f"securacv-{name}",
            "version": version,
            "description": f"PlatformIO product `{name}` ({product['dir']})",
            "properties": props({
                "securacv:project_dir": product["dir"],
                "securacv:build_envs": ",".join(build_envs),
                "securacv:release_envs": ",".join(product.get("release_envs") or []),
                "securacv:check_env": product.get("check_env", ""),
                "securacv:boards": ",".join(sorted(boards)),
            }),
        })
        dep(ROOT_REF, pref)
        if reaches_common:
            for lib in common:
                dep(pref, lib["ref"])
        if pdir == CANARY_LIB_DIR.parent:
            for lib in canary_libs:
                dep(pref, lib["ref"])

    for section, envs in platform_envs.items():
        literal = next(lit for lit, sec in sections.items() if sec == section)
        facts = PLATFORM_FACTS[literal]
        comp = {
            "type": "platform",
            "name": facts["name"],
            "description": f"PlatformIO platform package ([{section}] in "
                           f"firmware/envs/platformio/platforms.ini)",
            "externalReferences": [
                {"type": "distribution" if literal.startswith("http") else "website",
                 "url": literal if literal.startswith("http") else facts["website"]},
            ],
            "properties": props({
                "securacv:platforms_ini_section": section,
                "securacv:platform_literal": literal,
                "securacv:version_spec": facts["version"] or literal.split("@", 1)[1].strip(),
                "securacv:exact_pin": str(facts["exact"]).lower(),
                "securacv:envs": ",".join(sorted(envs)),
            }),
        }
        if facts["version"]:
            comp["version"] = facts["version"]
        add(f"platform:{section}", comp)
        core_ref = f"framework:arduino-esp32@{facts['arduino_core']}"
        idf_ref = f"framework:esp-idf@{facts['esp_idf']}"
        dep(f"platform:{section}", core_ref)
        dep(core_ref, idf_ref)
        core_paths = {f"platformio ({section})"}
        if core_ref in components:
            core_paths |= set(components[core_ref]["_paths"])
        add(core_ref, {
            "type": "framework",
            "group": "espressif",
            "name": "arduino-esp32",
            "version": facts["arduino_core"],
            "description": "Arduino core for the ESP32 (framework = arduino)",
            "purl": f"pkg:github/espressif/arduino-esp32@{facts['arduino_core']}",
            "licenses": [{"license": {"id": LICENSES["arduino-esp32"]}}],
            "_paths": sorted(core_paths),
        })
        add(idf_ref, {
            "type": "framework",
            "group": "espressif",
            "name": "esp-idf",
            "version": facts["esp_idf"],
            "description": "Espressif IoT Development Framework, bundled inside the Arduino core",
            "purl": f"pkg:github/espressif/esp-idf@v{facts['esp_idf']}",
            "licenses": [{"license": {"id": LICENSES["esp-idf"]}}],
            "properties": props({
                "securacv:declared_in": "scripts/gen_firmware_sbom.py PLATFORM_FACTS",
                "securacv:bundled_components": "not enumerated — FreeRTOS, mbedTLS, lwIP "
                                               "and cJSON ship inside this IDF release at "
                                               "versions nothing in this tree pins",
            }),
        })

    for ref, lib in lib_meta.items():
        comp = {
            "type": "library",
            "name": lib["name"],
            "properties": props({
                "securacv:version_spec": lib["spec"] or "(unpinned)",
                "securacv:exact_pin": str(lib["exact"]).lower(),
                "securacv:envs": ",".join(sorted(lib_envs[ref])),
                "securacv:resolved": "at build time — PlatformIO keeps no lockfile",
            }),
        }
        if lib["group"]:
            comp["group"] = lib["group"]
        if lib["exact"]:
            comp["version"] = lib["spec"]
        if lib["url"]:
            comp["externalReferences"] = [{"type": "distribution", "url": lib["url"]}]
            # Host check on the parsed URL, not a substring: a purl of type
            # `github` is a claim about where the bytes come from, and
            # "github.com/" can appear anywhere in a URL that points elsewhere.
            host = urlparse(lib["url"]).hostname or ""
            if lib["group"] and host.lower() in ("github.com", "www.github.com") and lib["exact"]:
                comp["purl"] = f"pkg:github/{lib['group']}/{lib['name']}@v{lib['spec']}"
        elif lib["group"]:
            # quote(): a registry name may carry spaces ("GFX Library for
            # Arduino"), and an externalReferences url must be a valid IRI.
            comp["externalReferences"] = [{
                "type": "website",
                "url": "https://registry.platformio.org/libraries/"
                       f"{quote(lib['group'])}/{quote(lib['name'])}",
            }]
        add(ref, comp)

    def core_path(ver: str | None, path: str) -> str:
        """Record `path` as a build path of the esp32:esp32 core `ver` (None:
        the action's "latest"), creating the Boards-Manager component when
        no PlatformIO platform already brought that core; returns its ref."""
        ref = f"framework:arduino-esp32@{ver or 'latest'}"
        if ref in components:
            components[ref]["_paths"] = sorted(set(components[ref]["_paths"]) | {path})
            return ref
        comp = {
            "type": "framework",
            "group": "espressif",
            "name": "arduino-esp32",
            "description": "Arduino core for the ESP32 (esp32:esp32 Boards Manager "
                           "package, Arduino-CLI build path)",
            "licenses": [{"license": {"id": LICENSES["arduino-esp32"]}}],
            "_paths": [path],
        }
        if ver:
            comp["version"] = ver
            comp["purl"] = f"pkg:github/espressif/arduino-esp32@{ver}"
        add(ref, comp)
        return ref

    workflow_pins = arduino_core_pins()
    for pin in workflow_pins:
        dep(ROOT_REF, core_path(pin["version"], f"arduino-cli ({pin['where']})"))

    # The other Arduino axis. Asserted before it is folded in: a document
    # that listed a profile on a core no build path agrees with would be a
    # record of the drift, not a refusal of it.
    sketch_pins = sketch_core_pins(flavors)
    findings = check_core_pin_agreement(workflow_pins, sketch_pins, product_cores)
    if findings:
        raise SystemExit(
            "gen_firmware_sbom.py: the Arduino core pins disagree between sketch.yaml and "
            "the workflows:\n  - " + "\n  - ".join(findings) + "\n"
            "Move the pin that is wrong — the sketch.yaml profile, or the workflow row's "
            "core-version / library pins (through .github/actions/setup-arduino-esp32) — "
            "and regenerate; firmware/PLATFORMS.md records which axis leads and why the "
            "WAP's Arduino-CLI rows float on the action's latest.")
    for pin in sketch_pins:
        core_path(pin["version"], f"sketch.yaml ({pin['sketch']}/{pin['profile']})")

    for ref, comp in components.items():
        if "_paths" in comp:
            paths = comp.pop("_paths")
            comp.setdefault("properties", [])
            comp["properties"] = props({
                "securacv:build_paths": "; ".join(paths),
                **({"securacv:declared_in": "scripts/gen_firmware_sbom.py PLATFORM_FACTS"}
                   if any(p.startswith("platformio") for p in paths) else {}),
            })

    ordered = [components[ref] for ref in sorted(components)]
    dependencies = [{"ref": ref, "dependsOn": sorted(deps)}
                    for ref, deps in sorted(depends.items())]
    body = json.dumps({"components": ordered, "dependencies": dependencies},
                      sort_keys=True, separators=(",", ":"))
    serial = uuid.uuid5(SERIAL_NAMESPACE, hashlib.sha256(body.encode()).hexdigest())

    metadata: dict = {}
    if timestamp:
        metadata["timestamp"] = timestamp
    metadata.update({
        "tools": {"components": [{
            "type": "application",
            "group": "securacv",
            "name": "gen_firmware_sbom.py",
            "version": GENERATOR_VERSION,
            "description": "scripts/gen_firmware_sbom.py — derives this document from "
                           "flavors.json, the resolved PlatformIO configs, platforms.ini, "
                           "the first-party library manifests, the workflows' Arduino "
                           "core pins and the sketch.yaml profiles (the two Arduino axes "
                           "are asserted to agree)",
        }]},
        "component": {
            "type": "firmware",
            "bom-ref": ROOT_REF,
            "group": "securacv",
            "name": "securacv-firmware",
            "version": version,
            "description": "SecuraCV Canary firmware — every PlatformIO product "
                           "firmware/flavors.json builds",
            "properties": props({
                "securacv:version_source": str(VERSION_HEADER.relative_to(REPO)),
                "securacv:products": ",".join(p["name"] for p in flavors),
            }),
        },
        "properties": props({
            "securacv:generator": "scripts/gen_firmware_sbom.py",
            "securacv:check": "python3 scripts/gen_firmware_sbom.py --check --validate",
            "securacv:declared_by_hand": "PLATFORM_FACTS (Arduino core / ESP-IDF release per "
                                         "platform literal); everything else is derived",
        }),
    })
    return {
        "bomFormat": "CycloneDX",
        "specVersion": SPEC_VERSION,
        "serialNumber": f"urn:uuid:{serial}",
        "version": 1,
        "metadata": metadata,
        "components": ordered,
        "dependencies": dependencies,
    }


def render(doc: dict) -> str:
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


# ---------------------------------------------------------------------------
# --verify-with-pio: the resolver against PlatformIO's own reading.

_ORACLE_KEYS = ("platform", "framework", "board", "lib_deps", "lib_extra_dirs")


def verify_with_pio(pio: str = "pio") -> list[str]:
    problems: list[str] = []
    for product in load_json(FLAVORS):
        pdir = REPO / product["dir"]
        try:
            out = subprocess.run([pio, "project", "config", "--json-output"],
                                 cwd=pdir, capture_output=True, text=True, check=True).stdout
        except (OSError, subprocess.CalledProcessError) as exc:
            return [f"{product['name']}: `{pio} project config` failed: {exc}"]
        oracle = {sec: dict(opts) for sec, opts in json.loads(out) if sec.startswith("env:")}
        cfg = PioConfig(pdir)
        for env in product.get("build_envs") or []:
            theirs = oracle.get(f"env:{env}")
            if theirs is None:
                problems.append(f"{product['name']}:{env}: PlatformIO knows no such env")
                continue
            ours = env_facts(product, cfg, env)
            for key in _ORACLE_KEYS:
                # PlatformIO reports an unset option as null, a scalar as a
                # string and a list option as a list; normalize both sides.
                want = theirs.get(key)
                if isinstance(want, list):
                    want = [str(w).strip() for w in want]
                elif key == "framework" and isinstance(want, str):
                    want = [want]
                got = ours[key] or None
                if got != want:
                    problems.append(f"{product['name']}:{env}: {key}: resolver says "
                                    f"{got!r}, PlatformIO says {want!r}")
    return problems


# ---------------------------------------------------------------------------
# --validate: the document against the CycloneDX schema, strictly.

def validate_document(text: str) -> str | None:
    """None when `text` is a strict CycloneDX SPEC_VERSION document; else the
    first violation as `<json path>: <reason>`.

    cyclonedx-python-lib's JsonStrictValidator with the schema it bundles
    ($refs resolve from a local registry — no network) and format checking
    on. The `json-validation` extra is what brings the iri-reference
    checker; without it a URL with a space passes, so the import is guarded
    with the exact pip spec, the way the PyYAML guard names its remedy.

    Two things the schema run cannot say come first: that `text` is JSON at
    all (the validator would raise a traceback, not report), and that the
    document claims SPEC_VERSION — the 1.5 schema types `specVersion` as a
    free string with an example, no enum, so a file declaring 1.6 satisfies
    it, and the OK line names 1.5.
    """
    try:
        doc = json.loads(text)
    except json.JSONDecodeError as exc:
        return f"$: not JSON — {exc.msg} at line {exc.lineno} column {exc.colno}"
    if isinstance(doc, dict) and doc.get("specVersion") != SPEC_VERSION:
        return (f"$.specVersion: expected {SPEC_VERSION!r}, got {doc.get('specVersion')!r} "
                f"(the schema types it as a free string; the generator promises "
                f"{SPEC_VERSION})")
    remedy = (f"gen_firmware_sbom.py: cyclonedx-python-lib with its json-validation extra "
              f"is needed for --validate (pip install '{VALIDATOR_PIP}')")
    try:
        from cyclonedx.exception import MissingOptionalDependencyException
        from cyclonedx.schema import SchemaVersion
        from cyclonedx.validation.json import JsonStrictValidator
    except ImportError as exc:  # pragma: no cover — exercised by hand
        raise SystemExit(remedy) from exc
    try:
        error = JsonStrictValidator(SchemaVersion.from_version(SPEC_VERSION)).validate_str(text)
    except MissingOptionalDependencyException as exc:  # pragma: no cover — the bare lib
        raise SystemExit(remedy) from exc
    if error is None:
        return None
    detail = getattr(error, "data", None)   # the jsonschema error underneath
    if detail is not None and hasattr(detail, "json_path"):
        return f"{detail.json_path}: {detail.message}"
    return str(error).splitlines()[0]


def _report_validation(label: str, text: str) -> int:
    problem = validate_document(text)
    if problem:
        print(f"::error::{label} is not a strict CycloneDX {SPEC_VERSION} document — {problem}")
        return 1
    print(f"gen_firmware_sbom.py --validate: OK — {label} is a strict CycloneDX "
          f"{SPEC_VERSION} document (schema bundled with cyclonedx-python-lib; no network).")
    return 0


# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=OUT,
                    help=f"where to write (default {OUT.relative_to(REPO)})")
    ap.add_argument("--check", action="store_true",
                    help="regenerate in memory and byte-compare against --out; exit 1 on drift")
    ap.add_argument("--timestamp", default=None,
                    help="stamp metadata.timestamp: an RFC 3339 instant, or `now`. "
                         "For a build artifact; the committed copy carries none")
    ap.add_argument("--verify-with-pio", action="store_true",
                    help="cross-check the ini resolver against `pio project config` "
                         "for every build env (needs PlatformIO on PATH)")
    ap.add_argument("--validate", nargs="?", const="", default=None, metavar="FILE",
                    help=f"hold the document to the CycloneDX {SPEC_VERSION} JSON schema, "
                         f"strictly (cyclonedx-python-lib's JsonStrictValidator): the "
                         f"rendered document (read-only, like --check, and before it "
                         f"compares; --out/--timestamp beside it need --check); or, with "
                         f"FILE, that file's bytes (sbom.yml's timestamped artifact), whose "
                         f"specVersion must be {SPEC_VERSION}. "
                         f"Needs: pip install '{VALIDATOR_PIP}'")
    args = ap.parse_args(argv)
    custom_out = args.out.resolve() != OUT.resolve()

    if args.validate:   # a FILE: validate those bytes and nothing else
        if args.check or args.verify_with_pio or args.timestamp or custom_out:
            ap.error("--validate FILE validates that file only and takes no --check, "
                     "--verify-with-pio, --timestamp or --out; bare --validate holds the "
                     "rendered document, alone or with --check")
        target = Path(args.validate)
        try:
            text = target.read_text(encoding="utf-8")
        except OSError as exc:
            print(f"::error::{target}: cannot read it — {exc.strerror or exc}")
            return 1
        return _report_validation(str(target), text)

    if args.validate is not None and not args.check and (args.timestamp or custom_out):
        # Read-only means read-only: a write flag beside bare --validate
        # used to be dropped for a green exit and no file.
        ap.error("bare --validate is read-only (the rendered document, like --check) and "
                 "writes nothing, so --out/--timestamp need --check beside it; to validate "
                 "an artifact, write it first and run --validate FILE on the file")

    if args.verify_with_pio:
        problems = verify_with_pio()
        if problems:
            for p in problems:
                print(f"::error::{p}")
            return 1
        print("gen_firmware_sbom.py --verify-with-pio: OK — the resolver agrees with "
              "PlatformIO on platform / framework / board / lib_deps / lib_extra_dirs "
              "for every build env.")
        if not args.check and args.validate is None:
            return 0

    stamp = args.timestamp
    if stamp == "now":
        stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    text = render(build_document(stamp))

    if args.validate is not None:
        # Read-only, like --check, and before the byte compare: a document
        # the schema refuses is the emitter's defect, not the file's.
        rc = _report_validation("the derived document (fix the generator, not the file)",
                                text)
        if rc or not args.check:
            return rc

    if args.check:
        current = args.out.read_text(encoding="utf-8") if args.out.exists() else ""
        if current != text:
            diff = difflib.unified_diff(current.splitlines(), text.splitlines(),
                                        "committed", "derived", lineterm="", n=1)
            for i, line in enumerate(diff):
                if i > 60:
                    print("…")
                    break
                print(line)
            print(f"::error::{args.out.relative_to(REPO) if args.out.is_relative_to(REPO) else args.out} "
                  f"is stale — a build input moved (a platform pin, a lib_deps line, a "
                  f"library manifest, an Arduino core pin in a workflow or a sketch.yaml "
                  f"profile, the firmware version). "
                  f"Regenerate: python3 scripts/gen_firmware_sbom.py")
            return 1
        doc = json.loads(text)
        print(f"gen_firmware_sbom.py --check: OK — {args.out.relative_to(REPO) if args.out.is_relative_to(REPO) else args.out} "
              f"matches the build inputs ({len(doc['components'])} components).")
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")
    doc = json.loads(text)
    print(f"wrote {args.out} — firmware {doc['metadata']['component']['version']}, "
          f"{len(doc['components'])} components")
    return 0


if __name__ == "__main__":
    sys.exit(main())
