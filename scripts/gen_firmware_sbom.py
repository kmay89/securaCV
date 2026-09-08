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
  firmware/canary/include/canary_config.h  FIRMWARE_VERSION — the one train
                                           (scripts/lint_fw_version_sync.sh
                                           holds the other five copies to it)

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

Usage:
    python3 scripts/gen_firmware_sbom.py              # rewrite the committed file
    python3 scripts/gen_firmware_sbom.py --check      # byte gate
    python3 scripts/gen_firmware_sbom.py --out X --timestamp now
    python3 scripts/gen_firmware_sbom.py --verify-with-pio   # needs `pio`

stdlib only, plus PyYAML for the Arduino-CLI core pins in the workflows.
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
from urllib.parse import quote

REPO = Path(__file__).resolve().parents[1]
FLAVORS = REPO / "firmware" / "flavors.json"
PLATFORMS_INI = REPO / "firmware" / "envs" / "platformio" / "platforms.ini"
COMMON_DIR = REPO / "firmware" / "common"
CANARY_LIB_DIR = REPO / "firmware" / "canary" / "lib"
WORKFLOWS = REPO / ".github" / "workflows"
ARDUINO_ACTION = "./.github/actions/setup-arduino-esp32"
VERSION_HEADER = REPO / "firmware" / "canary" / "include" / "canary_config.h"
OUT = REPO / "sbom" / "sbom-firmware.cdx.json"

SPEC_VERSION = "1.5"
GENERATOR_VERSION = "1.0.0"
ROOT_REF = "securacv-firmware"
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
    "espressif32 @ ^7.0.0": {
        "name": "platformio/espressif32",
        "version": None,
        "exact": False,
        # 7.x still ships the 2.0.17 core (the 7.x bump added ESP-IDF 6.0
        # support, not core 3.x) — PLATFORMS.md; what a release runner
        # resolved on the day is in that run's log, not here.
        "arduino_core": "2.0.17",
        "esp_idf": "4.4.7",
        "website": "https://registry.platformio.org/platforms/platformio/espressif32",
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


def arduino_core_pins() -> list[dict]:
    """[{version|None, where}] for every setup-arduino-esp32 call in the
    workflows. A literal `core-version` is a pin; `${{ matrix.a.b }}` resolves
    through the job's strategy.matrix; empty means the action's "latest"."""
    try:
        import yaml  # noqa: WPS433 — optional dependency, only for this pass
    except ImportError as exc:  # pragma: no cover — exercised by hand
        raise SystemExit("gen_firmware_sbom.py: PyYAML is needed to read the Arduino "
                         "core pins out of .github/workflows (pip install pyyaml)") from exc
    pins: list[dict] = []
    for path in sorted(WORKFLOWS.glob("*.yml")):
        wf = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        for job, spec in (wf.get("jobs") or {}).items():
            spec = spec or {}
            for step in spec.get("steps") or []:
                step = step or {}
                if step.get("uses") != ARDUINO_ACTION:
                    continue
                where = f"{path.name}:{job}"
                raw = str((step.get("with") or {}).get("core-version") or "").strip()
                m = _MATRIX_EXPR.match(raw)
                if m:
                    axis, key = m.groups()
                    entries = ((spec.get("strategy") or {}).get("matrix") or {}).get(axis) or []
                    for entry in entries:
                        if isinstance(entry, dict) and key in entry:
                            pins.append({"version": str(entry[key]), "where": where})
                    continue
                if "${{" in raw:
                    raise SystemExit(f"gen_firmware_sbom.py: {where} passes core-version "
                                     f"as an expression this script cannot resolve: {raw}")
                pins.append({"version": raw or None, "where": where})
    return pins


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
            if lib["group"] and "github.com/" in lib["url"] and lib["exact"]:
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

    for pin in arduino_core_pins():
        ver = pin["version"]
        ref = f"framework:arduino-esp32@{ver or 'latest'}"
        path = f"arduino-cli ({pin['where']})"
        if ref in components:
            components[ref]["_paths"] = sorted(set(components[ref]["_paths"]) | {path})
        else:
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
        dep(ROOT_REF, ref)

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
                           "the first-party library manifests and the workflows' Arduino "
                           "core pins",
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
            "securacv:check": "python3 scripts/gen_firmware_sbom.py --check",
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
    args = ap.parse_args(argv)

    if args.verify_with_pio:
        problems = verify_with_pio()
        if problems:
            for p in problems:
                print(f"::error::{p}")
            return 1
        print("gen_firmware_sbom.py --verify-with-pio: OK — the resolver agrees with "
              "PlatformIO on platform / framework / board / lib_deps / lib_extra_dirs "
              "for every build env.")
        if not args.check:
            return 0

    stamp = args.timestamp
    if stamp == "now":
        stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    text = render(build_document(stamp))

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
                  f"library manifest, an Arduino core pin, the firmware version). "
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
