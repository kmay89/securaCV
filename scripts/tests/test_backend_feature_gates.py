#!/usr/bin/env python3
"""Every detector backend that needs an optional crate stays behind a cargo feature (sweep F34).

`firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md` asks for two
things about the kernel's detector backends (`src/detect/backends/`): an audit
row per backend, and a check that every backend which is not the default
pure-Rust path is feature-gated. This file is that check, and it holds the
audit table (`docs/security/backend_audit.md`) to the tree.

Why a static Python test and not a `cargo test` (a decision, #1802; the item
asked for the latter): the property lives in the source text (a `cfg`
attribute on a `mod` line, a feature list in `Cargo.toml`), the kernel's
crates are not in any offline cache here, and `lint.yml` runs this directory
on every pull request with no Rust toolchain at all. A `cargo test` would see
the same text through the compiler, but only where the toolchain and every
crate can be fetched; `rust.yml` already builds the default and the
`backend-tract` feature sets, which is the compile-side half.

What it reads, and what it refuses:

  * `Cargo.toml`: `[dependencies]` and every `[target.*.dependencies]` table
    (an entry with `optional = true` is an optional crate; the key is the name
    code uses, `-` read as `_`), and `[features]`, expanded transitively
    (`dep:x`, a bare optional-crate name, `x/feat`; `x?/feat` enables nothing
    on its own). An entry it cannot resolve is refused, never skipped.
  * every `src/detect/backends/*.rs` but `mod.rs` is a backend file: the
    crates it names as a path root (`use tract_onnx::...`, `tract_onnx::x`,
    comments and string literals stripped first), and its gate, which is
    the `cfg` on its `mod` line in `mod.rs` and its own inner `#![cfg(...)]`
    together. A gate is read as the features it REQUIRES: `feature = "x"`,
    or `all(...)` of those (other predicates inside `all` only narrow it, so
    they are ignored); `any(...)` or a bare `not(...)` gate is refused,
    because "required" is not one set there.

It fails when:

  1. a backend file names an optional crate and its gate's required
     features do not, between them, enable that crate (no gate at all, or a
     gate on a feature that does not pull the crate in);
  2. the default feature set (`[features] default`, empty when absent)
     enables a backend's gate feature or any optional crate a backend needs;
  3. a backend file has no row in the audit table, a row names a file that
     is not there, or a row's "Cargo feature" cell disagrees with the gate
     (the gate's features in backticks, or "none" for an ungated file);
  4. a file in the directory is not declared in `mod.rs` (it would compile
     nowhere, so its gate could not be checked).

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
BACKENDS = "src/detect/backends"
AUDIT = "docs/security/backend_audit.md"


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


def optional_crates(manifest: dict) -> dict[str, str]:
    """{name code uses: dependency key} for every optional dependency."""
    out: dict[str, str] = {}
    for table in _dependency_tables(manifest):
        for key, spec in table.items():
            if isinstance(spec, dict) and spec.get("optional") is True:
                out[key.replace("-", "_")] = key
    return out


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
    """The known crate names a file uses as a path root."""
    roots = set(re.findall(r"\b([A-Za-z_]\w*)\s*::", strip_rust(src)))
    roots |= set(re.findall(r"\bextern\s+crate\s+([A-Za-z_]\w*)", strip_rust(src)))
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


def mod_lines(mod_src: str, where: str) -> dict[str, set[str]]:
    """{module name: features its mod line's cfg attributes require}."""
    out: dict[str, set[str]] = {}
    pending: set[str] = set()
    for raw in strip_rust(mod_src, keep_strings=True).splitlines():
        line = raw.strip()
        if not line:
            continue
        attr = re.fullmatch(r"#\[\s*cfg\s*\((.*)\)\s*\]", line)
        if attr:
            pending |= required_features(attr.group(1), where)
            continue
        if line.startswith("#["):
            continue  # another attribute on the same item
        m = re.fullmatch(r"(?:pub(?:\([^)]*\))?\s+)?mod\s+([A-Za-z_]\w*)\s*;", line)
        if m:
            out[m.group(1)] = pending
        pending = set()
    return out


# --------------------------------------------------------------------------
# The audit table

_ROW_FILE = re.compile(r"`src/detect/backends/([A-Za-z_]\w*)\.rs`")


def audit_rows(text: str) -> dict[str, str]:
    """{backend stem: its "Cargo feature" cell} from the audit table."""
    rows: dict[str, str] = {}
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
        if "cargo feature" not in header:
            continue
        m = _ROW_FILE.search(cells[0])
        if not m:
            continue
        if m.group(1) in rows:
            raise Unreadable(f"{AUDIT}: two rows for src/detect/backends/{m.group(1)}.rs")
        rows[m.group(1)] = cells[header.index("cargo feature")]
    return rows


# --------------------------------------------------------------------------
# The check


def findings(root: Path) -> list[str]:
    """Every rule broken in the tree at `root` (empty: the gate holds)."""
    manifest = tomllib.loads((root / CARGO).read_text(encoding="utf-8"))
    optional = optional_crates(manifest)
    backends = root / BACKENDS
    mods = mod_lines((backends / "mod.rs").read_text(encoding="utf-8"), f"{BACKENDS}/mod.rs")
    files = sorted(p for p in backends.glob("*.rs") if p.name != "mod.rs")
    if not files:
        raise Unreadable(f"{BACKENDS}: no backend files")
    out: list[str] = []
    gates: dict[str, set[str]] = {}
    needed_all: set[str] = set()
    for path in files:
        stem, rel = path.stem, f"{BACKENDS}/{path.name}"
        if stem not in mods:
            out.append(f"{rel}: not declared in mod.rs, so it compiles nowhere and its gate cannot be read")
            continue
        src = path.read_text(encoding="utf-8")
        gate = mods[stem] | file_gate(src, rel)
        gates[stem] = gate
        needed = {optional[c] for c in crates_named(src, set(optional))}
        needed_all |= needed
        if not needed:
            continue
        if not gate:
            out.append(f"{rel}: uses optional crate(s) {', '.join(sorted(needed))} with no "
                       f"cfg(feature) on its mod line or the file; gate it behind the feature "
                       f"that enables them (as tract.rs is, behind backend-tract)")
            continue
        _, enabled = feature_closure(manifest, sorted(gate))
        missing = needed - enabled
        if missing:
            out.append(f"{rel}: gated on {', '.join(sorted(gate))}, which does not enable "
                       f"{', '.join(sorted(missing))}; the gate must be a feature that pulls the crate in")
    # `default` is a feature like any other (its entries may be `dep:x` too);
    # absent, the default set is empty.
    has_default = "default" in (manifest.get("features") or {})
    feats, deps = feature_closure(manifest, ["default"] if has_default else [])
    for stem, gate in sorted(gates.items()):
        if gate & feats:
            out.append(f"Cargo.toml: the default feature set enables {', '.join(sorted(gate & feats))}, "
                       f"the gate of {BACKENDS}/{stem}.rs; a gated backend stays off by default")
    if deps & needed_all:
        out.append(f"Cargo.toml: the default feature set pulls in {', '.join(sorted(deps & needed_all))}, "
                   f"an optional crate a backend needs; keep it out of `default`")
    audit = root / AUDIT
    rows = audit_rows(audit.read_text(encoding="utf-8")) if audit.exists() else {}
    for stem in sorted(gates):
        if stem not in rows:
            out.append(f"{AUDIT}: no row for {BACKENDS}/{stem}.rs")
            continue
        cell = rows[stem]
        named = set(re.findall(r"`([^`]+)`", cell))
        if gates[stem]:
            if not gates[stem] <= named:
                out.append(f"{AUDIT}: the row for {stem}.rs says '{cell}', but its gate is "
                           f"{', '.join(sorted(gates[stem]))}")
        elif "none" not in cell.lower() or named & set(manifest.get("features") or {}):
            out.append(f"{AUDIT}: the row for {stem}.rs says '{cell}', but the file is ungated "
                       f"(the cell says 'none')")
    for stem in sorted(set(rows) - {p.stem for p in files}):
        out.append(f"{AUDIT}: a row names {BACKENDS}/{stem}.rs, which is not there")
    return out


# --------------------------------------------------------------------------
# Tests


def _copy_inputs(dst: Path, src: Path = REPO) -> None:
    for rel in (CARGO, AUDIT):
        (dst / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src / rel, dst / rel)
    shutil.copytree(src / BACKENDS, dst / BACKENDS)


class TheTreeHolds(unittest.TestCase):
    def test_every_backend_is_gated_audited_and_off_by_default(self):
        self.assertEqual(findings(REPO), [])

    def test_the_tree_is_what_the_audit_says(self):
        # The facts the audit table records, read the gate's way: tract is the
        # one backend behind a feature, and it is the one that needs a crate.
        manifest = tomllib.loads((REPO / CARGO).read_text(encoding="utf-8"))
        optional = optional_crates(manifest)
        mods = mod_lines((REPO / BACKENDS / "mod.rs").read_text(encoding="utf-8"), "mod.rs")
        self.assertEqual(mods["tract"], {"backend-tract"})
        for stem in ("cpu", "motion", "stub"):
            self.assertEqual(mods[stem], set(), stem)
        tract = (REPO / BACKENDS / "tract.rs").read_text(encoding="utf-8")
        self.assertEqual(file_gate(tract, "tract.rs"), {"backend-tract"})
        self.assertEqual({optional[c] for c in crates_named(tract, set(optional))}, {"tract-onnx"})
        for stem in ("cpu", "motion", "stub"):
            src = (REPO / BACKENDS / f"{stem}.rs").read_text(encoding="utf-8")
            self.assertEqual(crates_named(src, set(optional)), set(), stem)
        self.assertNotIn("default", manifest["features"])
        self.assertIn("backend-tract", feature_closure(manifest, ["detect-eval"])[0])


class Mutations(unittest.TestCase):
    """Each rule, broken on a copy of the real inputs."""

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

    def assertFinding(self, *needles: str):
        got = findings(self.root)
        self.assertTrue(any(all(n in f for n in needles) for f in got), got)

    def test_tract_with_no_gate_on_its_mod_line_or_file(self):
        self.edit(f"{BACKENDS}/mod.rs", '#[cfg(feature = "backend-tract")]\npub mod tract;', "pub mod tract;")
        self.edit(f"{BACKENDS}/tract.rs", '#![cfg(feature = "backend-tract")]\n', "")
        self.assertFinding("tract.rs", "tract-onnx", "no cfg(feature)")

    def test_either_gate_alone_is_enough(self):
        self.edit(f"{BACKENDS}/tract.rs", '#![cfg(feature = "backend-tract")]\n', "")
        self.assertEqual(findings(self.root), [])
        _copy_inputs(self.root / "b")
        b = self.root / "b"
        mod = (b / BACKENDS / "mod.rs").read_text(encoding="utf-8")
        (b / BACKENDS / "mod.rs").write_text(
            mod.replace('#[cfg(feature = "backend-tract")]\npub mod tract;', "pub mod tract;"), encoding="utf-8")
        self.assertEqual(findings(b), [])

    def test_a_gate_on_a_feature_that_does_not_pull_the_crate(self):
        self.edit(f"{BACKENDS}/mod.rs", '#[cfg(feature = "backend-tract")]\npub mod tract;',
                  '#[cfg(feature = "adapter-frigate")]\npub mod tract;')
        self.edit(f"{BACKENDS}/tract.rs", '#![cfg(feature = "backend-tract")]', '#![cfg(feature = "adapter-frigate")]')
        self.assertFinding("tract.rs", "adapter-frigate", "does not enable tract-onnx")

    def test_a_new_backend_on_an_optional_crate_must_be_gated(self):
        (self.root / BACKENDS / "ffmpeg.rs").write_text(
            "use ffmpeg_next::format;\n// tract_onnx:: in a comment is not a use\n"
            'pub fn name() -> &\'static str { "image::x" }\n', encoding="utf-8")
        self.edit(f"{BACKENDS}/mod.rs", "pub mod stub;", "pub mod stub;\npub mod ffmpeg;")
        got = [f for f in findings(self.root) if f.startswith(f"{BACKENDS}/ffmpeg.rs:")]
        # the crate in the `use`, and not the ones named in a comment or a string
        self.assertEqual(len(got), 1, got)
        self.assertIn("uses optional crate(s) ffmpeg-next with no cfg(feature)", got[0])

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
        text = (self.root / AUDIT).read_text(encoding="utf-8")
        row = next(ln for ln in text.splitlines()
                   if ln.startswith("|") and "`src/detect/backends/cpu.rs`" in ln)
        cells = row.strip().strip("|").split("|")
        header = next(ln for ln in text.splitlines() if ln.startswith("|") and "Cargo feature" in ln)
        col = [c.strip() for c in header.strip().strip("|").split("|")].index("Cargo feature")
        cells[col] = " `backend-tract` "
        (self.root / AUDIT).write_text(text.replace(row, "|" + "|".join(cells) + "|"), encoding="utf-8")
        self.assertFinding("row for cpu.rs", "ungated")

    def test_an_undeclared_file(self):
        (self.root / BACKENDS / "orphan.rs").write_text("pub fn x() {}\n", encoding="utf-8")
        self.assertFinding("orphan.rs: not declared in mod.rs")

    def test_an_unreadable_gate_is_refused_not_passed(self):
        self.edit(f"{BACKENDS}/mod.rs", '#[cfg(feature = "backend-tract")]\npub mod tract;',
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
               "let c = 'x';\nuse ::url::Url;\nfn f() { v4l::Device::new(0); }\n")
        self.assertEqual(crates_named(src, {"tract_onnx", "image", "ureq", "url", "v4l"}), {"url", "v4l"})

    def test_feature_closure(self):
        m = {"dependencies": {"a": {"version": "1", "optional": True},
                              "b-c": {"version": "1", "optional": True},
                              "d": "1"},
             "target": {"cfg(unix)": {"dependencies": {"e": {"version": "1", "optional": True}}}},
             "features": {"f1": ["a"], "f2": ["f1", "dep:b-c"], "f3": ["a?/x"], "f4": ["e/x"],
                          "f5": ["d/std"]}}
        self.assertEqual(optional_crates(m), {"a": "a", "b_c": "b-c", "e": "e"})
        self.assertEqual(feature_closure(m, ["f2"]), ({"f1", "f2"}, {"a", "b-c"}))
        self.assertEqual(feature_closure(m, ["f3"]), ({"f3"}, set()))
        self.assertEqual(feature_closure(m, ["f4"]), ({"f4"}, {"e"}))
        self.assertEqual(feature_closure(m, ["f5"]), ({"f5"}, set()))
        self.assertEqual(feature_closure(m, []), (set(), set()))
        with self.assertRaises(Unreadable):
            feature_closure(m, ["nope"])


if __name__ == "__main__":
    unittest.main()
