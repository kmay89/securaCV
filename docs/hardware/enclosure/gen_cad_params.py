#!/usr/bin/env python3
"""gen_cad_params.py — the device manifests own the released cases' board knobs.

    python3 gen_cad_params.py            # write the manifest-owned literals into the .scad files
    python3 gen_cad_params.py --check    # CI gate: every owned literal equals its manifest

WHAT IT OWNS. devices/<slug>/device.json `cad.params` names the literal
Customizer knobs of that device's `cad.scad` that describe the BOARD or
MODULE the case is built around — `board_l`, `vm_w`, `stack_sock_h` — as
plain JSON numbers, strings or booleans. This generator WRITES those values
into the .scad's top-of-file literals (the line the knob already lives on,
the token only) and `--check` proves the file still says what the manifest
says. Nothing reads the manifest at render time: OpenSCAD, the Customizer,
render.sh, canary_case_fitcheck.scad, gen_builder_manifest.py,
gen_enclosures.py and scripts/lint_design_lang.py all keep seeing the same
literal knob they see today. The literal-knob contract lint_design_lang.py
states — "a case file's Customizer knob stays a LITERAL on purpose; a
computed value would vanish from the Customizer and from the website
builder's manifest" — is untouched; the manifest moved to the FRONT of the
chain, it did not replace a link of it.

ELIGIBILITY IS THE BUILDER'S PARSER. The knobs a manifest may own are
exactly what gen_builder_manifest.parse_scad returns — a top-level
`name = <literal>;` before the first `module`, outside a [Hidden] group —
asked with `with_lines=True`, so the rewrite lands on the line that parser
accepted the literal from. There is no second scanner to disagree with it,
and a same-named local inside a module can never be hit.

WHAT IT REFUSES (exit 1, naming the manifest, the knob and why):
  * a key that is not a literal knob of the file: a computed value
    (`board_stack_h = e_camera ? …`), a name only a module assigns, a
    [Hidden] knob — make it a top-level literal in a .scad PR (with
    previews) first;
  * a selector — a string knob whose help text carries an option list
    (`host`, `preset`, `radar`, `headers`, `port`, `model`, `panel_variant`,
    `part`): those are chosen per printable SET at render time (render.sh,
    canary_case_fitcheck.scad, gen_assembled_dims.py), not by a device;
  * a value whose type disagrees with the knob's (a number for a string);
  * a line that assigns two knobs (`aa_dx = 0.0; aa_dy = 0.0;`) — split the
    line first, so a rewrite can never touch its neighbor;
  * a knob assigned twice at top level (OpenSCAD warns and takes the last);
  * two manifests naming one .scad that disagree on a shared key. Several
    manifests may name one case (the three Vision hosts); each asserts any
    SUBSET of its literal knobs, the union is written, shared keys must agree.

HOW IT WRITES. The literal token only, on the recorded line, keeping the
old token's integer-vs-decimal spelling (`5` stays `5`, `21.0` stays a
decimal); the trailing comment and every other byte are left alone. A
token already equal to its manifest (numerically, for numbers) leaves its
line byte-identical, so a run over a tree whose manifests match its cases
changes nothing — which is what the first run proved. When a value did
change, the geometry moved, and the rest of the chain follows in the order
REGEN_ORDER prints (previews first: a .scad change must be seeable).

scripts/lint_device_manifests.py imports check(), so `python3
scripts/lint_device_manifests.py` stays THE manifest gate; lint.yml and
enclosure.yml also run --check directly.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import NamedTuple

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent          # as FIGURES_JSON does in gen_builder_manifest.py
DEVICES_DIR = REPO / "devices"

# The parser is shared, not copied: the same directory, on sys.path whether
# this file runs as a script (sys.path[0] is HERE), is imported by the
# linter (which puts HERE on sys.path itself) or is loaded by a test harness.
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
from gen_builder_manifest import parse_scad  # noqa: E402

# A literal token exactly as gen_builder_manifest.ASSIGN_RE accepts one.
LITERAL = r'"(?:[^"\\]|\\.)*"|true|false|-?\d+\.?\d*'
# One knob line: indent, name, ` = `, the token, then `;` and whatever
# follows (the help comment). The token is the only group that may change.
LINE_RE = r"^(\s*)(%s)(\s*=\s*)(" + LITERAL + r")(\s*;.*)$"

SCAD_TYPE = {"bool": "bool", "number": "number", "string": "string"}

REGEN_ORDER = """\
After an INTENDED change the geometry moved. Regenerate in this order:
  1. PNG previews of every affected part, shared with the requester, never
     committed (docs/hardware/enclosure/README.md, "Preview renders")
  2. (cd docs/hardware/enclosure && ./render.sh)                    # the STLs
  3. python3 docs/hardware/enclosure/gen_assembled_dims.py          # assembled envelopes
  4. node canary-local/tools/figures/gen_figures.mjs                # fleet figures + ledger
  5. node canary-local/tools/figures/gen_device_glbs.mjs            # the Lab's device glTFs
  6. (cd firmware/projects/canary-display && ./setup.sh regen)     # the sketch's figure mirror
  7. Actions -> "Rebuild emulator dist (pinned emsdk)" on the branch, then pull it
  8. python3 canary-local/tools/gen_flash.py
  9. python3 docs/hardware/enclosure/gen_builder_manifest.py [--site <website checkout>]
 10. python3 canary-local/tools/gen_enclosures.py"""


class Change(NamedTuple):
    line: int          # 1-based
    name: str
    old_token: str
    new_token: str
    old_line: str      # with its line ending
    new_line: str


class Rendered(NamedTuple):
    text: str
    changes: list[Change]
    errors: list[str]


class Owned(NamedTuple):
    value: object
    slugs: list[str]


# ---------------------------------------------------------------------------
# values
# ---------------------------------------------------------------------------

def _kind(value) -> str | None:
    """JSON value -> knob type name, or None for anything that is not a literal."""
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, str):
        return "string"
    return None


def _same(a, b) -> bool:
    """Manifest-value equality: numeric for numbers, exact otherwise."""
    if _kind(a) != _kind(b):
        return False
    if _kind(a) == "number":
        return float(a) == float(b)
    return a == b


def _fmt_number(value, old: str) -> str | None:
    """`value` spelled the way `old` was: integer stays integer when it can."""
    f = float(value)
    if "." not in old and f.is_integer():
        return str(int(f))
    s = repr(f)
    if not re.fullmatch(r"-?\d+\.?\d*", s):      # 1e-05, inf, nan — not a knob
        return None
    return s


def _token(value, old: str) -> str | None:
    """The new literal for `value`, or None when `old` already says it."""
    if isinstance(value, bool):
        new = "true" if value else "false"
        return None if new == old else new
    if isinstance(value, str):
        if json.loads(old) == value:               # keep the file's own spelling
            return None
        return json.dumps(value, ensure_ascii=False)
    if float(old) == float(value):
        return None
    return _fmt_number(value, old)


# ---------------------------------------------------------------------------
# the manifests' side
# ---------------------------------------------------------------------------

def _rel(path: Path, repo: Path) -> str:
    try:
        return str(path.relative_to(repo))
    except ValueError:
        return str(path)


def load_params(devices_dir: Path = DEVICES_DIR,
                repo: Path = REPO) -> tuple[dict[str, dict[str, Owned]], list[str]]:
    """{cad.scad (repo-relative): {knob: Owned(value, [slugs])}}, errors.

    Every devices/*/device.json with a `cad.params` contributes to its
    `cad.scad`; a knob two manifests assert with different values is an
    error naming both. The first assertion's value stays in the map so the
    caller can still report the rest of the file.
    """
    owned: dict[str, dict[str, Owned]] = {}
    errors: list[str] = []
    for path in sorted(devices_dir.glob("*/device.json")):
        try:
            m = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            errors.append(f"could not read {_rel(path, repo)}: {e}")
            continue
        slug = m.get("slug") or path.parent.name
        cad = m.get("cad") or {}
        params = cad.get("params")
        if params is None:
            continue
        scad = cad.get("scad")
        if not isinstance(scad, str) or not scad:
            errors.append(f"devices/{slug}: cad.params without a cad.scad — nothing to own")
            continue
        if not isinstance(params, dict):
            errors.append(f"devices/{slug}: cad.params must be an object of knob -> literal")
            continue
        per = owned.setdefault(scad, {})
        for key, value in params.items():
            if _kind(value) is None:
                errors.append(f"devices/{slug}: cad.params.{key} = {json.dumps(value)} is not a "
                              f"number, string or boolean — a knob is a literal")
                continue
            if key in per:
                prev = per[key]
                if not _same(prev.value, value):
                    others = ", ".join(f"devices/{s}" for s in prev.slugs)
                    errors.append(
                        f"{Path(scad).name}: cad.params.{key} is asserted by {others} as "
                        f"{json.dumps(prev.value)} and by devices/{slug} as {json.dumps(value)} — "
                        f"manifests sharing one case must agree on a shared key")
                    continue
                prev.slugs.append(slug)
            else:
                per[key] = Owned(value, [slug])
    return owned, errors


# ---------------------------------------------------------------------------
# the .scad side
# ---------------------------------------------------------------------------

def _params(scad_path: Path) -> list[dict]:
    """Every literal knob the builder's parser accepts, flat, with its line."""
    return [p for g in parse_scad(scad_path, with_lines=True) for p in g["params"]]


def eligible(scad_path: Path) -> dict[str, dict]:
    """{knob name: param} — what a manifest may own in this file. A knob
    assigned twice at top level carries `also_on` (the other lines) and is
    refused by render()."""
    out: dict[str, dict] = {}
    for p in _params(scad_path):
        if p["name"] in out:
            out[p["name"]].setdefault("also_on", []).append(p["line"])
        else:
            out[p["name"]] = dict(p)
    return out


def render(scad_path: Path, owned: dict[str, object],
           who: dict[str, list[str]] | None = None) -> Rendered:
    """The file's text with every owned knob's literal set to its manifest
    value, the list of lines that changed, and every refusal. Nothing is
    written. `who` (knob -> slugs) only decorates the messages."""
    text = scad_path.read_text(encoding="utf-8")
    # splitlines(keepends=True) splits exactly where parse_scad's splitlines()
    # does, so its line numbers index this list; joining reproduces the bytes.
    lines = text.splitlines(keepends=True)
    flat = _params(scad_path)
    per_line = Counter(p["line"] for p in flat)
    params = eligible(scad_path)
    changes: list[Change] = []
    errors: list[str] = []

    def tag(name: str) -> str:
        slugs = (who or {}).get(name)
        by = f" (devices/{', devices/'.join(slugs)})" if slugs else ""
        return f"{scad_path.name}: cad.params.{name}{by}"

    for name in owned:
        value = owned[name]
        p = params.get(name)
        if p is None:
            errors.append(
                f"{tag(name)} is not a literal Customizer knob of this file — a computed "
                f"value, a module-local or a [Hidden] knob is not ownable (scripts/"
                f"lint_design_lang.py: knobs stay literals; a computed value would vanish from "
                f"the Customizer and the builder). Make it a top-level literal in a .scad PR, "
                f"with previews, first")
            continue
        if "also_on" in p:
            where = ", ".join(str(n) for n in [p["line"], *p["also_on"]])
            errors.append(f"{tag(name)} is assigned more than once at top level (lines {where}) — "
                          f"OpenSCAD warns and takes the last; give the knob one line first")
            continue
        if "options" in p:
            errors.append(
                f"{tag(name)} is a selector ({json.dumps(p['options'])}) — selectors are chosen "
                f"per set at render time (render.sh, canary_case_fitcheck.scad), not owned by a "
                f"device")
            continue
        want = SCAD_TYPE[p["type"]]
        got = _kind(value)
        if got != want:
            errors.append(f"{tag(name)} is a {got} in the manifest but the knob is a {want} "
                          f"(line {p['line']}: {json.dumps(p['default'])})")
            continue
        if per_line[p["line"]] > 1:
            errors.append(f"{tag(name)}: line {p['line']} assigns more than one knob — split the "
                          f"line first, so a rewrite can never touch its neighbor")
            continue
        idx = p["line"] - 1
        raw = lines[idx]
        body = raw.rstrip("\r\n")
        ending = raw[len(body):]
        m = re.match(LINE_RE % re.escape(name), body)
        if not m:
            errors.append(f"{tag(name)}: could not locate the literal on line {p['line']} "
                          f"({body.strip()!r}) — the line is not `{name} = <literal>; …`")
            continue
        old = m.group(4)
        new = _token(value, old)
        if new is None:
            continue
        if not re.fullmatch(LITERAL, new):
            errors.append(f"{tag(name)}: {json.dumps(value)} cannot be spelled as a Customizer "
                          f"literal")
            continue
        new_body = m.group(1) + m.group(2) + m.group(3) + new + m.group(5)
        lines[idx] = new_body + ending
        changes.append(Change(p["line"], name, old, new, raw, new_body + ending))
    return Rendered("".join(lines), changes, errors)


# ---------------------------------------------------------------------------
# the gate and the writer
# ---------------------------------------------------------------------------

def _plan(devices_dir: Path, repo: Path) -> tuple[list[tuple[Path, Rendered]], list[str]]:
    owned, errors = load_params(devices_dir, repo)
    plan: list[tuple[Path, Rendered]] = []
    for scad_rel in sorted(owned):
        keys = owned[scad_rel]
        path = repo / scad_rel
        slugs = sorted({s for o in keys.values() for s in o.slugs})
        if not path.is_file():
            errors.append(f"{scad_rel} (cad.scad of devices/{', devices/'.join(slugs)}) does not "
                          f"exist")
            continue
        r = render(path, {k: o.value for k, o in keys.items()},
                   {k: o.slugs for k, o in keys.items()})
        errors.extend(r.errors)
        plan.append((path, r))
    return plan, errors


def check(devices_dir: Path | None = None, repo: Path | None = None) -> list[str]:
    """Every problem, as one message each; [] means every owned literal equals
    its manifest. Renders in memory only. This is what the manifest linter
    appends to its own errors."""
    devices_dir = devices_dir or DEVICES_DIR
    repo = repo or REPO
    plan, errors = _plan(devices_dir, repo)
    owned, _ = load_params(devices_dir, repo)
    for path, r in plan:
        keys = owned[_rel(path, repo)]
        for c in r.changes:
            slugs = ", ".join(f"devices/{s}" for s in keys[c.name].slugs)
            errors.append(
                f"{path.name}:{c.line}: {c.name} = {c.old_token} in the .scad, but {slugs} "
                f"cad.params says {c.new_token} — the manifest owns this knob: run "
                f"python3 docs/hardware/enclosure/gen_cad_params.py (then the regen order it "
                f"prints), or change the manifest back")
    return errors


def write(devices_dir: Path | None = None,
          repo: Path | None = None) -> tuple[list[tuple[Path, Change]], list[str]]:
    """Write every changed file. Nothing is written when anything is refused,
    and an unchanged file is not rewritten. Returns the (file, change) pairs
    written — the list of parts that now need previews — and the errors."""
    devices_dir = devices_dir or DEVICES_DIR
    repo = repo or REPO
    plan, errors = _plan(devices_dir, repo)
    if errors:
        return [], errors
    written: list[tuple[Path, Change]] = []
    for path, r in plan:
        if not r.changes:
            continue
        path.write_text(r.text, encoding="utf-8")
        written.extend((path, c) for c in r.changes)
    return written, errors


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="verify every manifest-owned literal equals its .scad (CI); write nothing")
    args = ap.parse_args(argv)

    if args.check:
        errors = check()
        if errors:
            print(f"gen_cad_params.py: {len(errors)} problem(s):\n")
            for e in errors:
                print(f"  ✗ {e}")
            print()
            print(REGEN_ORDER)
            return 1
        owned, _ = load_params()
        n = sum(len(k) for k in owned.values())
        print(f"gen_cad_params.py: OK — {n} manifest-owned knobs across {len(owned)} case "
              f"file(s) equal their .scad literals")
        return 0

    written, errors = write()
    if errors:
        print(f"gen_cad_params.py: refused, nothing written — {len(errors)} problem(s):\n")
        for e in errors:
            print(f"  ✗ {e}")
        return 1
    if not written:
        print("gen_cad_params.py: nothing to write — every manifest-owned literal already equals "
              "its .scad")
        return 0
    files = sorted({p for p, _ in written})
    for path, c in written:
        print(f"wrote {_rel(path, REPO)}:{c.line}  {c.name}: {c.old_token} -> {c.new_token}")
    print(f"\n{len(written)} knob(s) in {len(files)} file(s) changed — render PNG previews of "
          f"every affected part of: {', '.join(p.name for p in files)}")
    print()
    print(REGEN_ORDER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
