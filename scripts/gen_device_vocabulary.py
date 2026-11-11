#!/usr/bin/env python3
"""gen_device_vocabulary.py — the keys a Canary's answers use, and which of
them a Home Assistant diagnostics dump redacts, from one vocabulary.

`spec/device_details_vocabulary.json` holds one row per key a Canary's own
answers use (its HTTP and Bluetooth answers, its MQTT publishes, its serial
records): the key, its class (`identifier`, `health_counter`), and the
firmware sources (`surfaces`) that emit it as a JSON key. Each class says
whether a dump redacts its keys.

The integration cannot read `spec/` at runtime: HACS ships
`custom_components/securacv/` alone. So this writes
`custom_components/securacv/device_vocabulary.py`, and `diagnostics.py`
unions its `REDACTED_DEVICE_KEYS` into `TO_REDACT`: a key the vocabulary
gains in a redacted class is redacted with no edit to `diagnostics.py`
(sweep HA56, closing ledger F391's last clause).

The vocabulary is also the input the general identifier lint (ledger F430,
device-details PR 4) is to read, so that lint and the redaction set name
one list. That lint is not built; nothing here enforces F430's scope.

WHAT COUNTS AS EMITTING. A surface emits a key when, with its comments
dropped (a key in a comment is not on the wire), it holds one of:
  - an ArduinoJson write: `x["key"] = …`, `x["key"].set(…)`,
    `x["key"].to<…>()`, `x["key"].add(…)`;
  - the key escaped inside an ordinary C string literal and followed by a
    colon, `"\\"key\\":"` (the snprintf formats, `wap_json::raw`).
A read (`x["key"].as<…>()`, `x["key"].is<…>()`, `x["key"] == …`) is not an
emission, nor is a raw string literal (`R"(…)"`, the pages a Canary serves,
whose scripts read keys rather than send them). A spelling this scanner does
not know is a miss, and a miss refuses: name a surface it can read.

REFUSES (exit 1, naming the row; a write writes nothing):
  - a row that names no surface, or whose named surface does not emit its
    key (the message lists the firmware sources that do, or says none does:
    a row nothing emits is a guess, and a check that cannot find a key must
    fail rather than pass);
  - a surface that is not a firmware device source (under `firmware/`, a
    C or C++ source, not a host test or stub) or that does not exist;
  - a key or class name that is not `[a-z][a-z0-9_]*` (this generator
    spells nothing else), a duplicate key, a row of an undeclared class, a
    class whose `redacted` is not a boolean, an empty `means`, a field this
    script does not know, a missing one;
  - with --check, an output that differs from what this would write (a
    hand edit, or a vocabulary edited without the run).

Run:    python3 scripts/gen_device_vocabulary.py
Check:  python3 scripts/gen_device_vocabulary.py --check
        (--root DIR reads and writes the same paths under another tree)
"""
from __future__ import annotations

import argparse
import difflib
import json
import re
import sys
from pathlib import Path, PurePosixPath
from typing import Any

REPO = Path(__file__).resolve().parents[1]
VOCABULARY = PurePosixPath("spec/device_details_vocabulary.json")
OUTPUT = PurePosixPath("custom_components/securacv/device_vocabulary.py")
FIRMWARE = "firmware"

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hh", ".hpp", ".ino"}
# Path components that mark a host test, its stubs, or a build tree: never a
# device's answer.
NOT_DEVICE_DIRS = {"tests_host", "tests", "test", "stubs", "build", ".pio", "node_modules"}
NAME_RE = re.compile(r"[a-z][a-z0-9_]*\Z")
VERSION = 1

TOP_FIELDS = {"$comment", "vocabulary_version", "classes", "keys"}
TOP_REQUIRED = {"vocabulary_version", "classes", "keys"}
CLASS_FIELDS = {"redacted", "means"}
ROW_FIELDS = {"key", "class", "means", "surfaces"}

# An ArduinoJson write after the `"key"` literal: `] =` (never `==`),
# `].set(`, `].to<`, `].add(`.
WRITE_AFTER = re.compile(r"\s*\]\s*(?:=(?!=)|\.\s*(?:set|to|add)\s*[<(])")
# The key escaped inside an ordinary string literal, then a colon.
ESCAPED_KEY = re.compile(r'\\"([a-z][a-z0-9_]*)\\"\s*:')
RAW_PREFIXES = {"R", "LR", "uR", "UR", "u8R"}
CHAR_PREFIXES = {"", "L", "u", "U", "u8"}


def _ident_before(src: str, i: int) -> str:
    j = i
    while j > 0 and (src[j - 1].isalnum() or src[j - 1] == "_"):
        j -= 1
    return src[j:i]


def tokens(src: str) -> list[tuple[str, str]]:
    """The source as runs of ("code", text), ("str", body) and ("raw", body).

    A comment becomes one space of code. An ordinary string literal's body
    keeps its escapes as written; a raw string literal (`R"d(…)d"`) is its
    own kind. A char literal stays in the code run; a `'` after a digit is
    a digit separator. A literal or comment that never closes runs to the
    end of its line (a string) or of the file (a comment), never an error.
    """
    out: list[tuple[str, str]] = []
    code: list[str] = []

    def flush() -> None:
        if code:
            out.append(("code", "".join(code)))
            code.clear()

    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
            code.append(" ")
        elif c == "/" and src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            code.append(" ")
        elif c == '"' and _ident_before(src, i) in RAW_PREFIXES:
            k = src.find("(", i + 1)
            delim = src[i + 1:k] if k >= 0 else ""
            if k < 0 or len(delim) > 16 or any(ch in delim for ch in ' ()\\"\n\t'):
                # Not a raw literal this scanner can delimit: an ordinary one.
                code.append(c)
                i += 1
                continue
            close = ")" + delim + '"'
            e = src.find(close, k + 1)
            flush()
            out.append(("raw", src[k + 1:] if e < 0 else src[k + 1:e]))
            i = n if e < 0 else e + len(close)
        elif c == '"':
            j = i + 1
            while j < n and src[j] != '"' and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            flush()
            out.append(("str", src[i + 1:min(j, n)]))
            i = j + 1
        elif c == "'" and _ident_before(src, i) in CHAR_PREFIXES:
            j = i + 1
            while j < n and src[j] != "'" and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            code.append(src[i:j + 1])
            i = j + 1
        else:
            code.append(c)
            i += 1
    flush()
    return out


def emitted_keys(src: str) -> set[str]:
    """Every key `src` emits as a JSON key, by the two spellings above."""
    toks = tokens(src)
    keys: set[str] = set()
    for at, (kind, body) in enumerate(toks):
        if kind != "str":
            continue
        before = toks[at - 1][1] if at > 0 and toks[at - 1][0] == "code" else ""
        after = toks[at + 1][1] if at + 1 < len(toks) and toks[at + 1][0] == "code" else ""
        if NAME_RE.match(body) and before.rstrip().endswith("[") and WRITE_AFTER.match(after):
            keys.add(body)
        keys.update(m.group(1) for m in ESCAPED_KEY.finditer(body))
    return keys


def device_source_problem(rel: str) -> str | None:
    """Why `rel` is not a firmware device source, or None when it is one."""
    path = PurePosixPath(rel)
    if path.is_absolute() or ".." in path.parts or str(path) != rel:
        return "is not a plain repo-relative path"
    if not path.parts or path.parts[0] != FIRMWARE:
        return f"is not under {FIRMWARE}/"
    if path.suffix not in SOURCE_SUFFIXES:
        return "is not a C or C++ source (" + ", ".join(sorted(SOURCE_SUFFIXES)) + ")"
    if NOT_DEVICE_DIRS.intersection(path.parts[:-1]) or path.name.startswith("test_"):
        return "is a host test, a stub or a build output, not a device's answer"
    return None


class Scanner:
    """Reads and scans each firmware source once."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self._keys: dict[str, set[str]] = {}
        self._all: list[str] | None = None

    def keys_of(self, rel: str) -> set[str]:
        if rel not in self._keys:
            text = (self.root / rel).read_text(encoding="utf-8", errors="replace")
            self._keys[rel] = emitted_keys(text)
        return self._keys[rel]

    def device_sources(self) -> list[str]:
        if self._all is None:
            base = self.root / FIRMWARE
            found = []
            if base.is_dir():
                for p in sorted(base.rglob("*")):
                    rel = p.relative_to(self.root).as_posix()
                    if p.is_file() and device_source_problem(rel) is None:
                        found.append(rel)
            self._all = found
        return self._all

    def emitters(self, key: str) -> list[str]:
        """Every firmware device source that emits `key` (the hint a refusal gives)."""
        needles = (f'"{key}"', f'\\"{key}\\"')
        hits = []
        for rel in self.device_sources():
            text = (self.root / rel).read_text(encoding="utf-8", errors="replace")
            if any(needle in text for needle in needles) and key in self.keys_of(rel):
                hits.append(rel)
        return hits


def _nonempty_text(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def validate(data: Any, root: Path) -> tuple[dict[str, bool], dict[str, str], list[str]]:
    """(class -> redacted, key -> class, errors). Errors name the row."""
    errors: list[str] = []
    classes: dict[str, bool] = {}
    rows: dict[str, str] = {}
    if not isinstance(data, dict):
        return classes, rows, ["the vocabulary is not a JSON object"]
    unknown = set(data) - TOP_FIELDS
    missing = TOP_REQUIRED - set(data)
    if unknown:
        errors.append(f"unknown top-level field(s): {', '.join(sorted(unknown))}")
    if missing:
        errors.append(f"missing top-level field(s): {', '.join(sorted(missing))}")
    version = data.get("vocabulary_version")
    if "vocabulary_version" in data and (type(version) is not int or version != VERSION):
        errors.append(f"vocabulary_version is {version!r}; this generator reads {VERSION}")

    raw_classes = data.get("classes")
    if not isinstance(raw_classes, dict) or not raw_classes:
        errors.append("classes: not a non-empty object")
        raw_classes = {}
    for name, spec in raw_classes.items():
        where = f"class {name!r}"
        if not NAME_RE.match(name):
            errors.append(f"{where}: a class name is [a-z][a-z0-9_]*")
            continue
        if not isinstance(spec, dict):
            errors.append(f"{where}: not an object")
            continue
        if set(spec) != CLASS_FIELDS:
            errors.append(f"{where}: fields are {', '.join(sorted(CLASS_FIELDS))}, "
                          f"got {', '.join(sorted(spec)) or 'none'}")
        if type(spec.get("redacted")) is not bool:
            errors.append(f"{where}: redacted must be true or false, got {spec.get('redacted')!r}")
            continue
        if not _nonempty_text(spec.get("means")):
            errors.append(f"{where}: means is empty")
        classes[name] = spec["redacted"]

    raw_rows = data.get("keys")
    if not isinstance(raw_rows, list) or not raw_rows:
        errors.append("keys: not a non-empty list")
        raw_rows = []
    scanner = Scanner(root)
    for index, row in enumerate(raw_rows):
        if not isinstance(row, dict):
            errors.append(f"keys[{index}]: not an object")
            continue
        key = row.get("key")
        where = f"row {key!r}" if isinstance(key, str) else f"keys[{index}]"
        if set(row) != ROW_FIELDS:
            errors.append(f"{where}: fields are {', '.join(sorted(ROW_FIELDS))}, "
                          f"got {', '.join(sorted(row)) or 'none'}")
        if not isinstance(key, str) or not NAME_RE.match(key):
            errors.append(f"{where}: a key is [a-z][a-z0-9_]* (this generator spells nothing else)")
            continue
        if key in rows:
            errors.append(f"{where}: the key is listed twice")
            continue
        cls = row.get("class")
        if not isinstance(cls, str) or cls not in raw_classes:
            errors.append(f"{where}: class {cls!r} is not one the vocabulary declares")
            continue
        if not _nonempty_text(row.get("means")):
            errors.append(f"{where}: means is empty")
        surfaces = row.get("surfaces")
        if not isinstance(surfaces, list) or not all(isinstance(s, str) for s in surfaces):
            errors.append(f"{where}: surfaces is not a list of paths")
            continue
        if len(set(surfaces)) != len(surfaces):
            errors.append(f"{where}: a surface is listed twice")
        emitting = 0
        for rel in surfaces:
            problem = device_source_problem(rel)
            if problem is None and not (root / rel).is_file():
                problem = "does not exist"
            if problem is not None:
                errors.append(f"{where}: surface {rel} {problem}")
                continue
            if key in scanner.keys_of(rel):
                emitting += 1
            else:
                errors.append(f"{where}: surface {rel} does not emit {key!r} as a JSON key "
                              f'(x["{key}"] = …, or \\"{key}\\": in a string literal, '
                              "outside comments)")
        if not surfaces or emitting < len(surfaces):
            if not surfaces:
                errors.append(f"{where}: names no surface")
            emitters = scanner.emitters(key)
            if emitters:
                shown = ", ".join(emitters[:6]) + (" …" if len(emitters) > 6 else "")
                errors.append(f"{where}: {key!r} is emitted by {shown}")
            else:
                errors.append(f"{where}: no firmware device source emits {key!r} as a JSON key "
                              "(host tests and stubs aside); a row nothing emits is a guess, "
                              "so it is refused rather than passed")
        rows[key] = cls
    return classes, rows, errors


HEADER = '''\
"""The keys a Canary's answers use, by class, and the ones a dump redacts.

Generated by scripts/gen_device_vocabulary.py from
spec/device_details_vocabulary.json, which names each key's class and the
firmware sources that emit it. Do not edit this file: edit the vocabulary,
run the generator and commit both (lint.yml runs its --check). HACS ships
this directory alone, so the integration reads the vocabulary here rather
than from spec/. diagnostics.py unions REDACTED_DEVICE_KEYS into TO_REDACT,
so a key the vocabulary gains in a redacted class is redacted with no edit
there.
"""
from __future__ import annotations

from collections.abc import Mapping
from types import MappingProxyType
'''


def render(classes: dict[str, bool], rows: dict[str, str]) -> str:
    redacted_classes = sorted(name for name, redacted in classes.items() if redacted)
    redacted_keys = sorted(key for key, cls in rows.items() if classes[cls])
    lines = [HEADER, "", "# Every key the vocabulary holds, and its class."]
    lines.append("DEVICE_KEY_CLASSES: Mapping[str, str] = MappingProxyType({")
    lines += [f'    "{key}": "{rows[key]}",' for key in sorted(rows)]
    lines += ["})", "", "# The classes whose keys a diagnostics dump redacts."]
    lines.append("REDACTED_CLASSES: frozenset[str] = frozenset({")
    lines += [f'    "{name}",' for name in redacted_classes]
    lines += ["})", "", "# Every key of a redacted class."]
    lines.append("REDACTED_DEVICE_KEYS: frozenset[str] = frozenset({")
    lines += [f'    "{key}",' for key in redacted_keys]
    lines += ["})", ""]
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Write custom_components/securacv/device_vocabulary.py from the vocabulary.")
    parser.add_argument("--check", action="store_true",
                        help="refuse a stale or hand-edited output; write nothing")
    parser.add_argument("--root", type=Path, default=REPO,
                        help="the tree to read and write (default: this repo)")
    args = parser.parse_args(argv)
    root: Path = args.root.resolve()
    vocab_path = root / VOCABULARY
    out_path = root / OUTPUT
    me = "gen_device_vocabulary"

    try:
        data = json.loads(vocab_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"{me}: cannot read {VOCABULARY}: {exc}", file=sys.stderr)
        return 1
    classes, rows, errors = validate(data, root)
    if errors:
        print(f"{me}: {VOCABULARY} refused ({len(errors)} problem(s)); nothing written:",
              file=sys.stderr)
        for line in errors:
            print(f"  - {line}", file=sys.stderr)
        return 1

    text = render(classes, rows)
    redacted = sum(1 for cls in rows.values() if classes[cls])
    summary = (f"{len(rows)} keys in {len(classes)} classes, {redacted} redacted; "
               "every row's surfaces emit its key")
    if args.check:
        try:
            current = out_path.read_text(encoding="utf-8")
        except OSError:
            current = None
        if current != text:
            print(f"{me}: {OUTPUT} is stale or was edited by hand: it is not what "
                  f"{VOCABULARY} generates. Run python3 scripts/gen_device_vocabulary.py "
                  "and commit both.", file=sys.stderr)
            diff = difflib.unified_diff(
                (current or "").splitlines(), text.splitlines(),
                fromfile=f"{OUTPUT} (committed)", tofile=f"{OUTPUT} (generated)", lineterm="")
            for line in list(diff)[:60]:
                print(line, file=sys.stderr)
            return 1
        print(f"{me}: {OUTPUT} is current ({summary})")
        return 0
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(text, encoding="utf-8")
    print(f"{me}: wrote {OUTPUT} ({summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
