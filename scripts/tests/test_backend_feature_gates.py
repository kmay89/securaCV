#!/usr/bin/env python3
"""Every non-pure-Rust detector backend stays behind a cargo feature that is off by default (sweep F34).

`firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md` asks for two
things about the kernel's detector backends (`src/detect/backends/`): an audit
row per backend, and a check that every backend which is not the default
pure-Rust path is feature-gated. This file is that check, and it holds the
audit table (`docs/security/backend_audit.md`) to the tree.

Why a static Python test and not a `cargo test` (a decision, #1802; the item
asked for the latter): the property lives in the source text (a `cfg`
attribute on a `mod` line, a feature list in `Cargo.toml`, the locked crate
tree in `Cargo.lock`), the kernel's crates are not in any offline cache here,
and `lint.yml` runs this directory on every pull request with no Rust
toolchain at all. A `cargo test` would see the same text through the
compiler, but only where the toolchain and every crate can be fetched;
`rust.yml` already builds the default and the `backend-tract` feature sets,
which is the compile-side half.

What "non-pure-Rust" means here (a second decision, #1802): a backend is
non-pure-Rust when a crate it names, optional or required, pulls into its
locked tree one of the build tools that compile or link native code (`cc`,
`cmake`, `bindgen`, `pkg-config`, `vcpkg`; `NATIVE_TOOLS`). Every backend that
needs an optional crate is gated too, whatever the crate is written in. The
lock lists every platform's dependencies, so a tool pulled in on another
platform counts (it fails closed); a crate that ships native code with none of
those tools in its tree is not seen.

What it reads, and what it refuses:

  * `Cargo.toml`: `[dependencies]` and every `[target.*.dependencies]` table
    (the build's crates; an entry with `optional = true` is an optional crate;
    the key is the name code uses, `-` read as `_`), and `[features]`,
    expanded transitively (`dep:x`, a bare optional-crate name, `x/feat`;
    `x?/feat` enables nothing on its own). An entry it cannot resolve is
    refused, never skipped.
  * `Cargo.lock`: from the kernel's own package, each named crate's locked
    tree (by name, and version where the lock gives one), searched for
    `NATIVE_TOOLS`. A crate the lock does not list is refused.
  * the backend modules, driven from `mod.rs`: each `mod name;` resolves to
    `name.rs` or `name/mod.rs`, and the module is that file plus every `.rs`
    under `name/` (read whether or not it is declared, so a submodule cannot
    hide). Its gate is the `cfg` on its `mod` item (on the same line or the
    lines above) and its root file's own inner `#![cfg(...)]` together, read
    as the features it REQUIRES: `feature = "x"`, or `all(...)` of those
    (other predicates inside `all` only narrow it, so they are ignored);
    `any(...)` or a bare `not(...)` gate is refused, because "required" is not
    one set there. Also refused: a `#[path]` or `cfg_attr` that points a
    module at another file, an `include!`, an inline `mod name { ... }` in
    `mod.rs`, a module with both `name.rs` and `name/mod.rs`.
  * the crates a file names, comments and string literals stripped first:
    every identifier before `::` (any path segment, so a module that shares a
    crate's name over-reads, and fails closed), every `extern crate`, and
    every identifier in a `use` tree (`use x;`, `use x as y;`).

It fails when:

  1. a backend names an optional crate, or a crate whose locked tree builds
     native code, and has no gate at all; or its gate's required features do
     not, between them, enable an optional crate it names;
  2. the default feature set (`[features] default`, empty when absent)
     enables a backend's gate feature or any optional crate a backend needs;
  3. a backend has no row in the audit table, a row names a module that is
     not there, a row's "Cargo feature" cell disagrees with the gate (the
     gate's features in backticks, or "none" for an ungated module), its
     "Crates it names" cell does not list (in backticks) exactly the build
     crates the module names, or its "Native code" cell says "none" for a
     module that names a native crate;
  4. a `.rs` file under the directory belongs to no module `mod.rs` declares
     (it would compile nowhere, so its gate could not be checked), or `mod.rs`
     declares a module with no file;
  5. `mod.rs` itself names an optional or native crate (code that needs one
     belongs in a gated backend file).

Run:  python3 -m unittest scripts/tests/test_backend_feature_gates.py -v
"""

from __future__ import annotations

import re
import shutil
import tempfile
import tomllib
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CARGO = "Cargo.toml"
LOCK = "Cargo.lock"
BACKENDS = "src/detect/backends"
AUDIT = "docs/security/backend_audit.md"

# The build tools a crate's tree pulls in to compile or link native code: `cc`
# and `cmake` build C, C++ or assembly, `bindgen` generates bindings to a C
# library, `pkg-config` and `vcpkg` find one to link. A crate whose locked tree
# holds one of them is native for this gate.
NATIVE_TOOLS = frozenset({"bindgen", "cc", "cmake", "pkg-config", "vcpkg"})


class Unreadable(Exception):
    """An input this gate cannot spell. It fails the check: a gate that read
    past it would answer "gated" for a file it never understood."""


# --------------------------------------------------------------------------
# Cargo.toml


def _dependency_tables(manifest: dict) -> list[dict]:
    tables = [manifest.get("dependencies") or {}]
    for spec in (manifest.get("target") or {}).values():
        tables.append((spec or {}).get("dependencies") or {})
    return tables


def dependency_crates(manifest: dict) -> dict[str, str]:
    """{name code uses: dependency key} for every crate in the build, optional
    or not (`[dependencies]` and each target table; dev-dependencies are not
    in the build)."""
    return {key.replace("-", "_"): key for table in _dependency_tables(manifest) for key in table}


def optional_crates(manifest: dict) -> dict[str, str]:
    """{name code uses: dependency key} for every optional dependency."""
    out: dict[str, str] = {}
    for table in _dependency_tables(manifest):
        for key, spec in table.items():
            if isinstance(spec, dict) and spec.get("optional") is True:
                out[key.replace("-", "_")] = key
    return out


def _package_of(manifest: dict, key: str) -> str:
    """The package a dependency key names (`rumqttc = { package = ... }`)."""
    for table in _dependency_tables(manifest):
        spec = table.get(key)
        if isinstance(spec, dict) and isinstance(spec.get("package"), str):
            return spec["package"]
    return key


def feature_closure(manifest: dict, names: list[str]) -> tuple[set[str], set[str]]:
    """(features, optional dependency keys) the given features enable."""
    features = manifest.get("features") or {}
    optional = set(optional_crates(manifest).values())
    required = {key for table in _dependency_tables(manifest) for key in table} - optional
    seen: set[str] = set()
    deps: set[str] = set()
    stack = list(names)
    while stack:
        name = stack.pop()
        if name in seen:
            continue
        if name not in features:
            if name in optional:  # an optional crate is an implicit feature
                deps.add(name)
                seen.add(name)
                continue
            raise Unreadable(f"Cargo.toml: feature or optional dependency '{name}' is not declared")
        seen.add(name)
        for entry in features[name]:
            if entry.startswith("dep:"):
                dep = entry[4:]
                if dep not in optional:
                    raise Unreadable(f"Cargo.toml: [features] {name} names dep:{dep}, which is not optional")
                deps.add(dep)
            elif "?/" in entry:
                continue  # weak: enables the crate's feature only if something else enables the crate
            elif "/" in entry:
                head = entry.split("/", 1)[0]
                if head not in required:  # a required crate's feature enables no crate
                    stack.append(head)
            else:
                stack.append(entry)
    return seen & set(features), deps


# --------------------------------------------------------------------------
# Cargo.lock


class LockedTree:
    """Which `NATIVE_TOOLS` each build crate's locked tree pulls in, read from
    `Cargo.lock` from the kernel's own package down and answered on demand
    (so a crate no backend names is never looked up)."""

    def __init__(self, manifest: dict, lock: dict):
        self._manifest = manifest
        self._by_name: dict[str, list[dict]] = {}
        for pkg in lock.get("package") or []:
            self._by_name.setdefault(pkg.get("name"), []).append(pkg)
        name = (manifest.get("package") or {}).get("name")
        roots = [p for p in self._by_name.get(name, []) if "source" not in p]
        if len(roots) != 1:
            raise Unreadable(f"{LOCK}: no single local package named {name!r}")
        self._root = roots[0]
        self._cache: dict[str, set[str]] = {}

    def _resolve(self, entry: str, parent: str) -> dict:
        parts = entry.split()
        found = self._by_name.get(parts[0], [])
        if len(parts) > 1:
            found = [p for p in found if p.get("version") == parts[1]]
        if len(found) != 1:
            raise Unreadable(f"{LOCK}: '{entry}' (a dependency of {parent}) names no single package")
        return found[0]

    def native_tools(self, key: str) -> set[str]:
        if key not in self._cache:
            package = _package_of(self._manifest, key)
            root = self._root["name"]
            entries = [e for e in self._root.get("dependencies") or [] if e.split()[0] == package]
            if not entries:
                raise Unreadable(f"{LOCK}: {root} lists no {package}; the lock is stale against {CARGO}")
            seen: set[tuple[str, str]] = set()
            names: set[str] = set()
            stack = [(entry, root) for entry in entries]
            while stack:
                entry, parent = stack.pop()
                pkg = self._resolve(entry, parent)
                ident = (pkg["name"], pkg.get("version", ""))
                if ident in seen:
                    continue
                seen.add(ident)
                names.add(pkg["name"])
                stack.extend((dep, pkg["name"]) for dep in pkg.get("dependencies") or [])
            self._cache[key] = names & NATIVE_TOOLS
        return self._cache[key]


# --------------------------------------------------------------------------
# Rust source

_RAW_STRING = re.compile(r'b?r(#*)"')
_CHAR = re.compile(r"'(?:\\u\{[0-9A-Fa-f]+\}|\\.|[^'\\\n])'")


def _blank(text: str) -> str:
    return "".join(ch if ch == "\n" else " " for ch in text)


def strip_rust(src: str, keep_strings: bool = False) -> str:
    """The source with comments removed (newlines kept, so lines still line
    up) and, unless `keep_strings`, string and char literals blanked to `""`,
    so a crate named in prose or in a message is not a crate the file uses.
    Nested block comments, raw strings and lifetimes (`'a`, not a char) are
    told apart; an unterminated literal or comment runs to the end."""
    out: list[str] = []
    i, n = 0, len(src)
    while i < n:
        prev = src[i - 1] if i else ""
        if src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if src.startswith("/*", i):
            depth, j = 1, i + 2
            while j < n and depth:
                if src.startswith("/*", j):
                    depth, j = depth + 1, j + 2
                elif src.startswith("*/", j):
                    depth, j = depth - 1, j + 2
                else:
                    j += 1
            out.append(_blank(src[i:j]))
            i = j
            continue
        raw = _RAW_STRING.match(src, i) if not (prev.isalnum() or prev == "_") else None
        if raw:
            close = src.find('"' + raw.group(1), raw.end())
            j = n if close < 0 else close + 1 + len(raw.group(1))
            out.append(src[i:j] if keep_strings else '""' + "\n" * src[i:j].count("\n"))
            i = j
            continue
        if src[i] == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(src[i:j] if keep_strings else '""' + "\n" * src[i:j].count("\n"))
            i = j
            continue
        if src[i] == "'":
            m = _CHAR.match(src, i)
            if m:
                out.append(m.group(0) if keep_strings else "' '")
                i = m.end()
                continue
        out.append(src[i])
        i += 1
    return "".join(out)


def crates_named(src: str, known: set[str]) -> set[str]:
    """The known crate names a file names (see the module docstring): any
    path segment before `::`, `extern crate`, and every identifier in a `use`
    tree, so `use tract_onnx as onnx;` and a bare `use tract_onnx;` count."""
    text = strip_rust(src)
    roots = set(re.findall(r"\b([A-Za-z_]\w*)\s*::", text))
    roots |= set(re.findall(r"\bextern\s+crate\s+([A-Za-z_]\w*)", text))
    for tree in re.findall(r"\buse\s+([^;]*);", text):
        roots |= set(re.findall(r"[A-Za-z_]\w*", tree))
    return roots & known


def _split_args(text: str) -> list[str]:
    parts, depth, cur = [], 0, ""
    for ch in text:
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
            continue
        depth += ch == "("
        depth -= ch == ")"
        cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts


def required_features(cfg: str, where: str) -> set[str]:
    """The features a cfg predicate requires (see the module docstring)."""
    cfg = cfg.strip()
    m = re.fullmatch(r'feature\s*=\s*"([^"]+)"', cfg)
    if m:
        return {m.group(1)}
    m = re.fullmatch(r"all\s*\((.*)\)", cfg, re.S)
    if m:
        out: set[str] = set()
        for arg in _split_args(m.group(1)):
            if re.match(r"(any|not)\s*\(", arg):
                if "feature" in arg and arg.startswith("any"):
                    raise Unreadable(f"{where}: cfg any(...) over features cannot be read as one requirement: {cfg}")
                continue
            if re.match(r"all\s*\(", arg) or arg.startswith("feature"):
                out |= required_features(arg, where)
            # target_os = "...", unix, test ...: they narrow, they do not gate
        return out
    if re.match(r"(any|not)\s*\(", cfg):
        raise Unreadable(f"{where}: cfg({cfg}) is not a feature requirement this gate can read")
    return set()  # target_os = "...", unix, test: not a feature gate


def file_gate(src: str, where: str) -> set[str]:
    """Features the file's inner `#![cfg(...)]` attributes require. Inner
    attributes may only open a file, so they are read from its head."""
    out: set[str] = set()
    head = re.match(r"(?:\s*#!\[(?:[^\[\]]|\[[^\]]*\])*\])*", strip_rust(src, keep_strings=True))
    for attr in re.finditer(r"#!\[\s*cfg\s*\((.*?)\)\s*\]", head.group(0), re.S):
        out |= required_features(attr.group(1), where)
    return out


_ATTR = r"#\[(?:[^\[\]]|\[[^\[\]]*\])*\]"
_MOD_ITEM = re.compile(r"((?:" + _ATTR + r"\s*)*)(?:pub(?:\s*\([^)]*\))?\s+)?\bmod\s+([A-Za-z_]\w*)\s*([;{])")
_PATH_ATTR = re.compile(r"#!?\[\s*(?:path\s*=|cfg_attr\s*\((?:[^\[\]]|\[[^\[\]]*\])*\bpath\s*=)")


def refuse_redirects(src: str, where: str) -> None:
    """A `#[path]` (or a `cfg_attr` carrying one) points a module at a file
    the gate would not read as that module, and `include!` pastes in source it
    does not read at all: both are refused, in every file it reads."""
    if _PATH_ATTR.search(strip_rust(src, keep_strings=True)):
        raise Unreadable(f"{where}: a #[path] attribute points a module at a file this gate does not read as it")
    if re.search(r"\binclude\s*!", strip_rust(src)):
        raise Unreadable(f"{where}: include!() pastes in source this gate does not read")


def mod_lines(mod_src: str, where: str) -> dict[str, set[str]]:
    """{module name: features the cfg attributes on its `mod name;` item
    require}, attributes on the same line or the lines above alike."""
    refuse_redirects(mod_src, where)
    out: dict[str, set[str]] = {}
    for m in _MOD_ITEM.finditer(strip_rust(mod_src, keep_strings=True)):
        attrs, name, end = m.groups()
        if end == "{":
            raise Unreadable(f"{where}: an inline `mod {name} {{ ... }}`; keep each backend in its own file, "
                             f"where this gate reads it")
        if name in out:
            raise Unreadable(f"{where}: `mod {name}` is declared twice")
        gate: set[str] = set()
        for attr in re.finditer(_ATTR, attrs):
            body = attr.group(0)[2:-1].strip()
            if re.match(r"cfg_attr\b", body):
                raise Unreadable(f"{where}: cfg_attr on `mod {name}` is not a gate this check can read")
            cfg = re.fullmatch(r"cfg\s*\((.*)\)", body, re.S)
            if cfg:
                gate |= required_features(cfg.group(1), where)
        out[name] = gate
    return out


def backend_modules(root: Path) -> tuple[dict[str, dict], list[str]]:
    """({module name: its root file, files and mod-item gate}, rule-4
    findings), driven from `mod.rs`."""
    backends = root / BACKENDS
    mod_rs = backends / "mod.rs"
    mods = mod_lines(mod_rs.read_text(encoding="utf-8"), f"{BACKENDS}/mod.rs")
    modules: dict[str, dict] = {}
    out: list[str] = []
    covered = {mod_rs}
    for name, gate in mods.items():
        flat, nested = backends / f"{name}.rs", backends / name / "mod.rs"
        if flat.exists() and nested.exists():
            raise Unreadable(f"{BACKENDS}: both {name}.rs and {name}/mod.rs exist for `mod {name}`")
        if not (flat.exists() or nested.exists()):
            out.append(f"{BACKENDS}/mod.rs: declares `mod {name}`, but neither {name}.rs nor "
                       f"{name}/mod.rs is there")
            continue
        top = flat if flat.exists() else nested
        files = [top]
        if (backends / name).is_dir():
            files += sorted(p for p in (backends / name).rglob("*.rs") if p != top)
        covered |= set(files)
        modules[name] = {"root": top, "rel": top.relative_to(root).as_posix(), "files": files, "mod_gate": gate}
    for path in sorted(backends.rglob("*.rs")):
        if path not in covered:
            out.append(f"{path.relative_to(root).as_posix()}: not declared in mod.rs, so it compiles "
                       f"nowhere and its gate cannot be read")
    return modules, out


# --------------------------------------------------------------------------
# The audit table

_ROW_FILE = re.compile(r"`(src/detect/backends/[A-Za-z_]\w*(?:/mod)?\.rs)`")
COLUMNS = {"feature": "cargo feature", "crates": "crates it names", "native": "native code"}


def audit_rows(text: str) -> dict[str, dict[str, str]]:
    """{backend module's root file: {"feature", "crates", "native": cell}}
    from the audit table (the table whose header has a "Cargo feature"
    column; it must have the other two)."""
    rows: dict[str, dict[str, str]] = {}
    header: list[str] | None = None
    for line in text.splitlines():
        if not line.startswith("|"):
            header = None
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if header is None:
            header = [c.lower() for c in cells]
            continue
        if set("".join(cells)) <= set("-: "):
            continue
        if COLUMNS["feature"] not in header:
            continue
        missing = [c for c in COLUMNS.values() if c not in header]
        if missing or len(cells) != len(header):
            raise Unreadable(f"{AUDIT}: the backend table needs one cell per column and the columns "
                             f"{', '.join(repr(c) for c in COLUMNS.values())}: {line}")
        m = _ROW_FILE.search(cells[0])
        if not m:
            continue
        if m.group(1) in rows:
            raise Unreadable(f"{AUDIT}: two rows for {m.group(1)}")
        rows[m.group(1)] = {key: cells[header.index(col)] for key, col in COLUMNS.items()}
    return rows


# --------------------------------------------------------------------------
# The check


def findings(root: Path) -> list[str]:
    """Every rule broken in the tree at `root` (empty: the gate holds)."""
    manifest = tomllib.loads((root / CARGO).read_text(encoding="utf-8"))
    if not (root / LOCK).exists():
        raise Unreadable(f"{LOCK}: missing; the gate reads native code off the locked crate tree")
    lock = LockedTree(manifest, tomllib.loads((root / LOCK).read_text(encoding="utf-8")))
    deps = dependency_crates(manifest)
    optional = set(optional_crates(manifest).values())
    modules, out = backend_modules(root)
    if not modules:
        raise Unreadable(f"{BACKENDS}: no backend modules")
    gates: dict[str, set[str]] = {}
    named_by: dict[str, set[str]] = {}
    native_by: dict[str, dict[str, set[str]]] = {}
    needed_all: set[str] = set()
    for name, module in modules.items():
        rel = module["rel"]
        gate = set(module["mod_gate"])
        named: set[str] = set()
        for path in module["files"]:
            where = path.relative_to(root).as_posix()
            src = path.read_text(encoding="utf-8")
            refuse_redirects(src, where)
            if path == module["root"]:
                gate |= file_gate(src, where)
            named |= {deps[c] for c in crates_named(src, set(deps))}
        native = {key: lock.native_tools(key) for key in sorted(named) if lock.native_tools(key)}
        needed = named & optional
        gates[name], named_by[name], native_by[name] = gate, named, native
        needed_all |= needed
        if not gate and needed:
            out.append(f"{rel}: uses optional crate(s) {', '.join(sorted(needed))} with no "
                       f"cfg(feature) on its mod line or the file; gate it behind the feature "
                       f"that enables them (as tract.rs is, behind backend-tract)")
            continue
        if not gate and native:
            crates = "; ".join(f"{key} (its locked tree pulls in {', '.join(sorted(tools))})"
                               for key, tools in native.items())
            out.append(f"{rel}: uses {crates} with no cfg(feature) on its mod line or the file; "
                       f"a non-pure-Rust backend stays behind a feature that is off by default")
            continue
        if needed:
            _, enabled = feature_closure(manifest, sorted(gate))
            missing = needed - enabled
            if missing:
                out.append(f"{rel}: gated on {', '.join(sorted(gate))}, which does not enable "
                           f"{', '.join(sorted(missing))}; the gate must be a feature that pulls the crate in")
    own = {deps[c] for c in crates_named((root / BACKENDS / "mod.rs").read_text(encoding="utf-8"), set(deps))}
    bad = sorted(key for key in own if key in optional or lock.native_tools(key))
    if bad:
        out.append(f"{BACKENDS}/mod.rs: names {', '.join(bad)} itself; code that needs an optional "
                   f"or native crate belongs in a gated backend file")
    # `default` is a feature like any other (its entries may be `dep:x` too);
    # absent, the default set is empty.
    has_default = "default" in (manifest.get("features") or {})
    feats, default_deps = feature_closure(manifest, ["default"] if has_default else [])
    for name, gate in sorted(gates.items()):
        if gate & feats:
            out.append(f"Cargo.toml: the default feature set enables {', '.join(sorted(gate & feats))}, "
                       f"the gate of {modules[name]['rel']}; a gated backend stays off by default")
    if default_deps & needed_all:
        out.append(f"Cargo.toml: the default feature set pulls in {', '.join(sorted(default_deps & needed_all))}, "
                   f"an optional crate a backend needs; keep it out of `default`")
    audit = root / AUDIT
    rows = audit_rows(audit.read_text(encoding="utf-8")) if audit.exists() else {}
    for name in sorted(gates):
        rel = modules[name]["rel"]
        short = rel[len(BACKENDS) + 1:]
        if rel not in rows:
            out.append(f"{AUDIT}: no row for {rel}")
            continue
        row = rows[rel]
        cell = row["feature"]
        ticked = set(re.findall(r"`([^`]+)`", cell))
        if gates[name]:
            if not gates[name] <= ticked:
                out.append(f"{AUDIT}: the row for {short} says '{cell}', but its gate is "
                           f"{', '.join(sorted(gates[name]))}")
        elif "none" not in cell.lower() or ticked & set(manifest.get("features") or {}):
            out.append(f"{AUDIT}: the row for {short} says '{cell}', but the file is ungated "
                       f"(the cell says 'none')")
        listed = {deps[t.replace("-", "_")] for t in re.findall(r"`([^`]+)`", row["crates"])
                  if t.replace("-", "_") in deps}
        if named_by[name] - listed:
            out.append(f"{AUDIT}: the row for {short} does not list {', '.join(sorted(named_by[name] - listed))} "
                       f"under \"Crates it names\", and the module names it; the cell lists every build "
                       f"crate the backend names")
        if listed - named_by[name]:
            out.append(f"{AUDIT}: the row for {short} lists {', '.join(sorted(listed - named_by[name]))} "
                       f"under \"Crates it names\", which the module does not name")
        if native_by[name] and row["native"].lower().startswith("none"):
            out.append(f"{AUDIT}: the row for {short} says '{row['native']}' under \"Native code\", but "
                       f"{', '.join(sorted(native_by[name]))} builds native code per {LOCK}")
    known = {module["rel"] for module in modules.values()}
    for rel in sorted(set(rows) - known):
        state = "which is not there" if not (root / rel).exists() else "which mod.rs does not declare"
        out.append(f"{AUDIT}: a row names {rel}, {state}")
    return out


# --------------------------------------------------------------------------
# Tests


def _copy_inputs(dst: Path, src: Path = REPO) -> None:
    for rel in (CARGO, LOCK, AUDIT):
        (dst / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src / rel, dst / rel)
    shutil.copytree(src / BACKENDS, dst / BACKENDS)


def _read_lock(root: Path = REPO) -> LockedTree:
    manifest = tomllib.loads((root / CARGO).read_text(encoding="utf-8"))
    return LockedTree(manifest, tomllib.loads((root / LOCK).read_text(encoding="utf-8")))


class TheTreeHolds(unittest.TestCase):
    def test_every_backend_is_gated_audited_and_off_by_default(self):
        self.assertEqual(findings(REPO), [])

    def test_the_tree_is_what_the_audit_says(self):
        # The facts the audit table records, read the gate's way: tract is the
        # one backend behind a feature, and the one that needs an optional and
        # a native crate. Either gate site alone is a sufficient gate (rule 1
        # accepts a backend gated at one site), so it is this pin, not rule 1,
        # that notices tract losing one of its two.
        manifest = tomllib.loads((REPO / CARGO).read_text(encoding="utf-8"))
        deps = dependency_crates(manifest)
        mods = mod_lines((REPO / BACKENDS / "mod.rs").read_text(encoding="utf-8"), "mod.rs")
        self.assertEqual(mods, {"cpu": set(), "motion": set(), "stub": set(), "tract": {"backend-tract"}})
        tract = (REPO / BACKENDS / "tract.rs").read_text(encoding="utf-8")
        self.assertEqual(file_gate(tract, "tract.rs"), {"backend-tract"})
        named = {stem: {deps[c] for c in crates_named(
                    (REPO / BACKENDS / f"{stem}.rs").read_text(encoding="utf-8"), set(deps))}
                 for stem in mods}
        self.assertEqual(named, {"cpu": {"anyhow"}, "motion": {"anyhow", "sha2"},
                                 "stub": {"anyhow"}, "tract": {"anyhow", "tract-onnx"}})
        lock = _read_lock()
        self.assertIn("cc", lock.native_tools("tract-onnx"))
        for key in ("anyhow", "sha2"):
            self.assertEqual(lock.native_tools(key), set(), key)
        # A required crate the store builds C for: a backend naming it would
        # need a gate, though it is in every build.
        self.assertIn("cc", lock.native_tools("rusqlite"))
        self.assertNotIn("default", manifest["features"])
        self.assertIn("backend-tract", feature_closure(manifest, ["detect-eval"])[0])


class Mutations(unittest.TestCase):
    """Each rule, broken on a copy of the real inputs."""

    TRACT_MOD = '#[cfg(feature = "backend-tract")]\npub mod tract;'
    TRACT_FILE_GATE = '#![cfg(feature = "backend-tract")]\n'

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        _copy_inputs(self.root)

    def tearDown(self):
        self._tmp.cleanup()

    def edit(self, rel: str, old: str, new: str, count: int = 1):
        path = self.root / rel
        text = path.read_text(encoding="utf-8")
        self.assertEqual(text.count(old), count, (rel, old))
        path.write_text(text.replace(old, new), encoding="utf-8")

    def write(self, rel: str, text: str):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def declare(self, line: str):
        self.edit(f"{BACKENDS}/mod.rs", "pub mod stub;", "pub mod stub;\n" + line)

    def set_cell(self, rel: str, column: str, value: str):
        text = (self.root / AUDIT).read_text(encoding="utf-8")
        row = next(ln for ln in text.splitlines() if ln.startswith("|") and f"`{rel}`" in ln)
        header = next(ln for ln in text.splitlines() if ln.startswith("|") and "Cargo feature" in ln)
        col = [c.strip() for c in header.strip().strip("|").split("|")].index(column)
        cells = row.strip().strip("|").split("|")
        cells[col] = f" {value} "
        (self.root / AUDIT).write_text(text.replace(row, "|" + "|".join(cells) + "|"), encoding="utf-8")

    def add_row(self, rel: str, feature: str = "none", crates: str = "none", native: str = "none"):
        text = (self.root / AUDIT).read_text(encoding="utf-8")
        tract = next(ln for ln in text.splitlines()
                     if ln.startswith("|") and f"`{BACKENDS}/tract.rs`" in ln)
        row = f"| `{rel}` | probe | {feature} | {crates} | {native} | none | none |"
        (self.root / AUDIT).write_text(text.replace(tract, tract + "\n" + row), encoding="utf-8")

    def assertFinding(self, *needles: str):
        got = findings(self.root)
        self.assertTrue(any(all(n in f for n in needles) for f in got), got)

    def test_tract_with_no_gate_on_its_mod_line_or_file(self):
        self.edit(f"{BACKENDS}/mod.rs", self.TRACT_MOD, "pub mod tract;")
        self.edit(f"{BACKENDS}/tract.rs", self.TRACT_FILE_GATE, "")
        self.assertFinding("tract.rs", "tract-onnx", "no cfg(feature)")

    def test_either_gate_alone_is_enough(self):
        self.edit(f"{BACKENDS}/tract.rs", self.TRACT_FILE_GATE, "")
        self.assertEqual(findings(self.root), [])
        _copy_inputs(self.root / "b")
        b = self.root / "b"
        mod = (b / BACKENDS / "mod.rs").read_text(encoding="utf-8")
        (b / BACKENDS / "mod.rs").write_text(mod.replace(self.TRACT_MOD, "pub mod tract;"), encoding="utf-8")
        self.assertEqual(findings(b), [])

    def test_a_gate_on_the_same_line_as_the_mod_item(self):
        self.edit(f"{BACKENDS}/mod.rs", self.TRACT_MOD, '#[cfg(feature = "backend-tract")] pub mod tract;')
        self.edit(f"{BACKENDS}/tract.rs", self.TRACT_FILE_GATE, "")
        self.assertEqual(findings(self.root), [])

    def test_a_gate_on_a_feature_that_does_not_pull_the_crate(self):
        self.edit(f"{BACKENDS}/mod.rs", self.TRACT_MOD, '#[cfg(feature = "adapter-frigate")]\npub mod tract;')
        self.edit(f"{BACKENDS}/tract.rs", '#![cfg(feature = "backend-tract")]', '#![cfg(feature = "adapter-frigate")]')
        self.assertFinding("tract.rs", "adapter-frigate", "does not enable tract-onnx")

    def test_a_new_backend_on_an_optional_crate_must_be_gated(self):
        self.write(f"{BACKENDS}/ffmpeg.rs",
                   "use ffmpeg_next::format;\n// tract_onnx:: in a comment is not a use\n"
                   'pub fn name() -> &\'static str { "image::x" }\n')
        self.declare("pub mod ffmpeg;")
        got = [f for f in findings(self.root) if f.startswith(f"{BACKENDS}/ffmpeg.rs:")]
        # the crate in the `use`, and not the ones named in a comment or a string
        self.assertEqual(len(got), 1, got)
        self.assertIn("uses optional crate(s) ffmpeg-next with no cfg(feature)", got[0])

    def test_an_alias_or_a_bare_use_is_a_use(self):
        self.write(f"{BACKENDS}/alias.rs", "use tract_onnx as onnx;\npub fn f() { let _ = onnx::onnx(); }\n")
        self.write(f"{BACKENDS}/bare.rs", "use tract_onnx;\n")
        self.declare("pub mod alias;\npub mod bare;")
        self.assertFinding("alias.rs", "uses optional crate(s) tract-onnx", "no cfg(feature)")
        self.assertFinding("bare.rs", "uses optional crate(s) tract-onnx", "no cfg(feature)")

    def test_a_directory_module_is_read(self):
        self.write(f"{BACKENDS}/onnx/mod.rs", "use tract_onnx::prelude::*;\n")
        self.declare("pub mod onnx;")
        self.assertFinding(f"{BACKENDS}/onnx/mod.rs:", "uses optional crate(s) tract-onnx", "no cfg(feature)")
        self.assertFinding(f"no row for {BACKENDS}/onnx/mod.rs")

    def test_a_file_under_a_module_directory_is_read(self):
        # cpu/decode.rs is cpu's: read whether cpu.rs declares it or not.
        self.write(f"{BACKENDS}/cpu/decode.rs", "pub fn f() { tract_onnx::onnx(); }\n")
        self.assertFinding(f"{BACKENDS}/cpu.rs:", "uses optional crate(s) tract-onnx", "no cfg(feature)")

    def test_a_path_attribute_is_refused_not_passed(self):
        self.write("src/detect/other.rs", "use tract_onnx::prelude::*;\n")
        self.declare('#[path = "../other.rs"]\npub mod extra;')
        with self.assertRaises(Unreadable):
            findings(self.root)

    def test_a_path_attribute_inside_a_backend_file_is_refused(self):
        self.edit(f"{BACKENDS}/cpu.rs", "use anyhow::Result;", '#[path = "../../other.rs"]\nmod hidden;\nuse anyhow::Result;')
        with self.assertRaises(Unreadable):
            findings(self.root)

    def test_an_inline_module_in_mod_rs_is_refused(self):
        self.declare("mod inline { pub fn f() {} }")
        with self.assertRaises(Unreadable):
            findings(self.root)

    def test_a_declared_module_with_no_file(self):
        self.declare('#[cfg(feature = "backend-tract")]\npub mod ghost;')
        self.assertFinding("declares `mod ghost`", "neither ghost.rs nor ghost/mod.rs")

    def test_mod_rs_naming_an_optional_crate_itself(self):
        self.declare('#[cfg(feature = "backend-tract")]\npub use tract_onnx::prelude::Framework;')
        self.assertFinding(f"{BACKENDS}/mod.rs: names tract-onnx itself")

    def test_a_backend_on_a_required_native_crate_must_be_gated(self):
        # rusqlite is a required dependency (the store), and its locked tree
        # builds SQLCipher's C. A row that says it has no native code is wrong.
        self.write(f"{BACKENDS}/sqlnative.rs", "use rusqlite::Connection;\npub fn f(_: &Connection) {}\n")
        self.declare("pub mod sqlnative;")
        self.add_row(f"{BACKENDS}/sqlnative.rs")
        self.assertFinding("sqlnative.rs: uses rusqlite (its locked tree pulls in", "cc", "no cfg(feature)")
        self.assertFinding("row for sqlnative.rs does not list rusqlite")
        self.assertFinding("row for sqlnative.rs says 'none' under \"Native code\"", "rusqlite builds native code")

    def test_a_gated_backend_on_a_required_native_crate_passes(self):
        self.write(f"{BACKENDS}/sqlnative.rs", "use rusqlite::Connection;\npub fn f(_: &Connection) {}\n")
        self.declare('#[cfg(feature = "adapter-sandbox")]\npub mod sqlnative;')
        self.add_row(f"{BACKENDS}/sqlnative.rs", "`adapter-sandbox`", "`rusqlite` (required)",
                     "through `rusqlite`: bundled SQLCipher")
        self.assertEqual(findings(self.root), [])

    def test_a_row_must_list_every_crate_the_backend_names(self):
        self.set_cell(f"{BACKENDS}/motion.rs", "Crates it names", "`anyhow` (required)")
        self.assertFinding("row for motion.rs does not list sha2")

    def test_a_row_must_not_list_a_crate_the_backend_does_not_name(self):
        self.set_cell(f"{BACKENDS}/cpu.rs", "Crates it names", "`anyhow`, `image`")
        self.assertFinding("row for cpu.rs lists image", "does not name")

    def test_a_native_backend_whose_row_says_none(self):
        self.set_cell(f"{BACKENDS}/tract.rs", "Native code", "none")
        self.assertFinding("row for tract.rs says 'none' under \"Native code\"", "tract-onnx builds native code")

    def test_a_missing_lock_is_refused(self):
        (self.root / LOCK).unlink()
        with self.assertRaises(Unreadable):
            findings(self.root)

    def test_a_lock_without_a_named_crate_is_refused(self):
        text = (self.root / LOCK).read_text(encoding="utf-8")
        self.edit(LOCK, ' "tract-onnx",\n', "", count=text.count(' "tract-onnx",\n'))
        with self.assertRaises(Unreadable):
            findings(self.root)

    def test_default_enabling_the_backend_feature(self):
        self.edit(CARGO, "[features]\n", '[features]\ndefault = ["backend-tract"]\n')
        self.assertFinding("default feature set enables backend-tract", "tract.rs")

    def test_default_enabling_it_through_another_feature(self):
        self.edit(CARGO, "[features]\n", '[features]\ndefault = ["detect-eval"]\n')
        self.assertFinding("default feature set enables backend-tract")
        self.assertFinding("default feature set pulls in tract-onnx")

    def test_default_pulling_the_crate_directly(self):
        self.edit(CARGO, "[features]\n", '[features]\ndefault = ["dep:tract-onnx"]\n')
        self.assertFinding("default feature set pulls in tract-onnx")

    def test_a_backend_with_no_audit_row(self):
        text = (self.root / AUDIT).read_text(encoding="utf-8")
        kept = [ln for ln in text.splitlines() if "`src/detect/backends/tract.rs`" not in ln or not ln.startswith("|")]
        (self.root / AUDIT).write_text("\n".join(kept) + "\n", encoding="utf-8")
        self.assertFinding("no row for src/detect/backends/tract.rs")

    def test_a_row_for_a_file_that_is_not_there(self):
        (self.root / BACKENDS / "stub.rs").unlink()
        self.edit(f"{BACKENDS}/mod.rs", "pub mod stub;\n", "")
        self.assertFinding("names src/detect/backends/stub.rs, which is not there")

    def test_a_row_whose_feature_cell_disagrees(self):
        self.set_cell(f"{BACKENDS}/cpu.rs", "Cargo feature", "`backend-tract`")
        self.assertFinding("row for cpu.rs", "ungated")

    def test_an_undeclared_file(self):
        self.write(f"{BACKENDS}/orphan.rs", "pub fn x() {}\n")
        self.assertFinding("orphan.rs: not declared in mod.rs")

    def test_an_undeclared_directory(self):
        self.write(f"{BACKENDS}/loose/mod.rs", "pub fn x() {}\n")
        self.assertFinding("loose/mod.rs: not declared in mod.rs")

    def test_an_unreadable_gate_is_refused_not_passed(self):
        self.edit(f"{BACKENDS}/mod.rs", self.TRACT_MOD,
                  '#[cfg(any(feature = "backend-tract", feature = "detect-eval"))]\npub mod tract;')
        with self.assertRaises(Unreadable):
            findings(self.root)

    def test_an_unknown_feature_entry_is_refused(self):
        self.edit(CARGO, 'backend-tract = ["tract-onnx"]', 'backend-tract = ["tract-onnx", "no-such-thing"]')
        self.edit(CARGO, "[features]\n", '[features]\ndefault = ["backend-tract"]\n')
        with self.assertRaises(Unreadable):
            findings(self.root)


class Readers(unittest.TestCase):
    def test_required_features(self):
        self.assertEqual(required_features('feature = "a"', "x"), {"a"})
        self.assertEqual(required_features('all(feature = "a", target_os = "linux", feature = "b")', "x"), {"a", "b"})
        self.assertEqual(required_features('all(feature = "a", not(feature = "b"))', "x"), {"a"})
        self.assertEqual(required_features("test", "x"), set())
        for bad in ('any(feature = "a", feature = "b")', 'not(feature = "a")',
                    'all(any(feature = "a", feature = "b"))'):
            with self.assertRaises(Unreadable, msg=bad):
                required_features(bad, "x")

    def test_crate_roots_ignore_comments_and_strings(self):
        src = ('// use tract_onnx::prelude::*;\n/* image::open */\nlet s = "ureq::get";\n'
               "let c = 'x';\nuse ::url::Url;\nfn f() { v4l::Device::new(0); }\n"
               "use rand as r;\nuse hex;\n// use log as l;\n")
        self.assertEqual(crates_named(src, {"tract_onnx", "image", "ureq", "url", "v4l", "rand", "hex", "log"}),
                         {"url", "v4l", "rand", "hex"})

    def test_mod_items(self):
        src = ('pub mod a;\n#[cfg(feature = "x")]\n#[allow(dead_code)]\npub(crate) mod b;\n'
               '#[cfg(feature = "y")] mod c;\n#[cfg(feature = "z")]\npub use c::C;\nmod d;\n'
               '// mod e;\n')
        self.assertEqual(mod_lines(src, "m"), {"a": set(), "b": {"x"}, "c": {"y"}, "d": set()})
        for bad in ("mod a;\nmod a;\n", "mod a { }\n", '#[cfg_attr(unix, path = "u.rs")]\nmod a;\n',
                    '#[cfg_attr(feature = "x", allow(dead_code))]\nmod a;\n', 'include!("x.rs");\n'):
            with self.assertRaises(Unreadable, msg=bad):
                mod_lines(bad, "m")

    def test_feature_closure(self):
        m = {"dependencies": {"a": {"version": "1", "optional": True},
                              "b-c": {"version": "1", "optional": True},
                              "d": "1"},
             "target": {"cfg(unix)": {"dependencies": {"e": {"version": "1", "optional": True}}}},
             "features": {"f1": ["a"], "f2": ["f1", "dep:b-c"], "f3": ["a?/x"], "f4": ["e/x"],
                          "f5": ["d/std"]}}
        self.assertEqual(optional_crates(m), {"a": "a", "b_c": "b-c", "e": "e"})
        self.assertEqual(dependency_crates(m), {"a": "a", "b_c": "b-c", "d": "d", "e": "e"})
        self.assertEqual(feature_closure(m, ["f2"]), ({"f1", "f2"}, {"a", "b-c"}))
        self.assertEqual(feature_closure(m, ["f3"]), ({"f3"}, set()))
        self.assertEqual(feature_closure(m, ["f4"]), ({"f4"}, {"e"}))
        self.assertEqual(feature_closure(m, ["f5"]), ({"f5"}, set()))
        self.assertEqual(feature_closure(m, []), (set(), set()))
        with self.assertRaises(Unreadable):
            feature_closure(m, ["nope"])

    def test_locked_tree(self):
        manifest = {"package": {"name": "k"},
                    "dependencies": {"pure": "1", "wrap": "1", "dup": "2",
                                     "renamed": {"version": "1", "package": "real"}}}
        lock = {"package": [
            {"name": "k", "version": "0.1.0", "dependencies": ["pure", "wrap", "real", "dup 2.0.0"]},
            {"name": "pure", "version": "1.0.0", "source": "registry", "dependencies": ["dup 1.0.0"]},
            {"name": "dup", "version": "2.0.0", "source": "registry", "dependencies": ["cmake"]},
            {"name": "dup", "version": "1.0.0", "source": "registry"},
            {"name": "wrap", "version": "1.0.0", "source": "registry", "dependencies": ["wrap-sys"]},
            {"name": "wrap-sys", "version": "1.0.0", "source": "registry", "dependencies": ["cc", "pkg-config"]},
            {"name": "real", "version": "1.0.0", "source": "registry", "dependencies": ["bindgen"]},
            {"name": "cc", "version": "1.0.0", "source": "registry"},
            {"name": "cmake", "version": "1.0.0", "source": "registry", "dependencies": ["cc"]},
            {"name": "pkg-config", "version": "1.0.0", "source": "registry"},
            {"name": "bindgen", "version": "1.0.0", "source": "registry"},
        ]}
        tree = LockedTree(manifest, lock)
        # dup 1.0.0 is pure; the native dup 2.0.0, listed first, is a
        # different package, told apart by the version the entry names
        self.assertEqual(tree.native_tools("pure"), set())
        self.assertEqual(tree.native_tools("dup"), {"cc", "cmake"})
        self.assertEqual(tree.native_tools("wrap"), {"cc", "pkg-config"})
        self.assertEqual(tree.native_tools("renamed"), {"bindgen"})
        bad = dict(lock, package=[p for p in lock["package"] if p["name"] != "real"])
        with self.assertRaises(Unreadable):
            LockedTree(manifest, bad).native_tools("renamed")
        ambiguous = dict(lock, package=lock["package"] + [{"name": "k", "version": "0.2.0"}])
        with self.assertRaises(Unreadable):
            LockedTree(manifest, ambiguous)


if __name__ == "__main__":
    unittest.main()
