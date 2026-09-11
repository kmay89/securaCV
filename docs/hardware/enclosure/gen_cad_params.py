#!/usr/bin/env python3
"""gen_cad_params.py — the device manifests own the released cases' board knobs.

    python3 gen_cad_params.py            # write the manifest-owned literals into the .scad files
    python3 gen_cad_params.py --check    # CI gate: every owned literal equals its manifest

WHAT IT OWNS. devices/<slug>/device.json `cad.params` names the literal
Customizer knobs of that device's `cad.scad` that describe the BOARD or
MODULE the case is built around — `board_l`, `vm_w`, `stack_sock_h` — as
plain JSON numbers, strings or booleans, or as REFERENCES into the board
registry (next section). This generator WRITES those values into the
.scad's top-of-file literals (the line the knob already lives on, the
token only) and `--check` proves the file still says what the manifest
says. Nothing reads the manifest at render time: OpenSCAD, the Customizer,
render.sh, canary_case_fitcheck.scad, gen_builder_manifest.py,
gen_enclosures.py and scripts/lint_design_lang.py all keep seeing the same
literal knob they see today. The literal-knob contract lint_design_lang.py
states — "a case file's Customizer knob stays a LITERAL on purpose; a
computed value would vanish from the Customizer and from the website
builder's manifest" — is untouched; the manifest moved to the FRONT of the
chain, it did not replace a link of it.

A KNOB THAT IS A BOARD FACT REFERENCES THE REGISTRY. canary_board_lib.scad
already holds every board the catalog mounts, once, with its evidence rung:
BRD_REGISTRY rows of [id, along-USB length, width, PCB thickness, status,
note], plus the measured facts that refine a row (`function
brd_xiao_w_measured() = 17.8;`). So a cad.params value may be

    {"brd": "xiao", "dim": "l"}          ->  brd_l("xiao")      dim: l | w | t
    {"brd_fn": "brd_xiao_w_measured"}     ->  brd_xiao_w_measured()

and the generator resolves it to the registry's number BEFORE eligibility
and rewrite — the .scad still receives a literal, exactly as for a typed
number. The manifest carries the JOIN, the registry carries the number:
which row, which rung, and which decision (the Sense clips name
brd_w("xiao") = 17.5 spec; the Vision pins name brd_xiao_w_measured() =
17.8 — the registry states the truth, the manifest states the decision,
as the library's header asks the file to). A registry correction now
reaches every manifest-owned case through --check as "registry says X,
file says Y", and through a write. A reference is declared only where the
knob's help comment already cites the registry; a knob that merely equals
a row by coincidence (the WAP's board_h 1.2) stays a number, and so does a
case measurement with no registry home (board_clear, xiao_below, ant_h).
The registry is parsed as gen_builder_manifest.parse_colorways parses
CW_REGISTRY — a literal-shape regex with the row starts counted
independently, so a row that drifts from the shape fails the build rather
than shortening the registry — and the facts with the FUNC pattern
scripts/lint_design_lang.py already uses; both on the source with its `//`
and `/* */` comments stripped first, so a row commented out or a fact
inside a block comment is as absent here as it is to OpenSCAD.
canary_board_lib.scad is READ here, never written.

ELIGIBILITY IS THE BUILDER'S PARSER. The knobs a manifest may own are
exactly what gen_builder_manifest.parse_scad returns — a top-level
`name = <literal>;` before the first `module`, outside a [Hidden] group —
asked with `with_lines=True`, so the rewrite lands on the line that parser
accepted the literal from. There is no second scanner to disagree with it,
and a same-named local of a column-0 module can never be hit (parse_scad
stops at the first `^module`; that is the boundary it draws, and this file
draws no other).

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
  * a value with no Customizer-literal spelling: a number in exponent form
    (0.00001), NaN or Infinity (refused when the manifest loads), a string
    holding `//` or `/*` (a comment opener to the parser: it would write,
    then fail --check as "not a literal");
  * a line that holds two statements (`aa_dx = 0.0; aa_dy = 0.0;`, `$fn =
    64; shared = 7;`, `mixed = 3; dep = mixed;`) — counted on the line's
    code, comments and strings aside — split the line first, so a rewrite
    can never touch its neighbor;
  * a knob assigned twice at top level (OpenSCAD warns and takes the last);
  * two manifests naming one .scad that disagree on a shared key. Several
    manifests may name one case (the three Vision hosts); each asserts any
    SUBSET of its literal knobs, the union is written, shared keys must agree;
  * a reference that is not exactly {"brd", "dim"} or exactly {"brd_fn"},
    that names a row, a dim or a fact the registry does not define, or a
    registry whose rows do not all parse — the message names the manifest
    and the library.

HOW IT WRITES. The literal token only, on the recorded line, keeping the
old token's integer-vs-decimal spelling (`5` stays `5`, `21.0` stays a
decimal); the trailing comment, the line ending (CRLF stays CRLF) and
every other byte are left alone. A
token already equal to its manifest (numerically, for numbers) leaves its
line byte-identical, so a run over a tree whose manifests match its cases
changes nothing — which is what the first run proved, and what the
reference conversion proved again. When a value did change, the geometry
moved, and the rest of the chain follows in the order REGEN_ORDER prints
(previews first: a .scad change must be seeable).

scripts/lint_device_manifests.py imports check(), so `python3
scripts/lint_device_manifests.py` stays THE manifest gate; lint.yml and
enclosure.yml also run --check directly. --check also prints, as INFO
(never an error), the registry rows and facts no manifest references: the
display cases have manifests that own no knobs yet, and the doorbell, the
gang plate, the Hammond chassis, the J-box and the bench fixture have no
manifest — all of them cite the registry by comment.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from pathlib import Path
from typing import NamedTuple

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent          # as FIGURES_JSON does in gen_builder_manifest.py
DEVICES_DIR = REPO / "devices"
# The board registry a cad.params reference resolves from — read, never written.
BOARD_LIB_REL = "docs/hardware/enclosure/canary_board_lib.scad"

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

# What a manifest's cad.scad may be: devices/device.schema.json's pattern for
# it, verbatim (test_gen_cad_params.py pins the two equal). The standalone
# generator refuses what the schema would, so it never relies on the manifest
# linter having run first — and a `./` or `..` segment can never key the owned
# map under one spelling and be looked up under another (a KeyError that used
# to take the whole manifest gate down with it).
SCAD_PATH_RE = re.compile(r"^docs/hardware/enclosure/[a-z0-9_]+\.scad$")

# A reference's `dim` -> (the accessor it stands for, the row column it reads).
DIMS = {"l": ("brd_l", 1), "w": ("brd_w", 2), "t": ("brd_t", 3)}
# One BRD_REGISTRY row in its literal shape — ["id", l, w, t, "status", "note"] —
# as gen_builder_manifest._CW_ROW reads a CW_REGISTRY row: lowercase id and
# status, plain numbers, one quoted note. A row typed any other way does not
# match, and parse_board_registry() then fails on the row count.
_BRD_ROW = re.compile(
    r'\[\s*"([a-z][a-z0-9_]*)"\s*,\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*,'
    r'\s*(-?\d+(?:\.\d+)?)\s*,\s*"([a-z]+)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\]',
    re.S)
# `function <name>() = <number>;` — scripts/lint_design_lang.py's FUNC pattern,
# verbatim; only the brd_* matches are facts a manifest may name.
_FUNC = re.compile(r"function\s+(?P<fn>[a-z_][a-z0-9_]*)\(\)\s*=\s*(?P<val>-?\d+(?:\.\d+)?)\s*;")
_FACT_NAME = re.compile(r"^brd_[a-z0-9_]+$")
REF_SHAPE = ('{"brd": "<id>", "dim": "l"|"w"|"t"} (a BRD_REGISTRY row: along-USB length, '
             'width, PCB thickness) or exactly {"brd_fn": "brd_<name>"} (a numeric brd_*() fact)')

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


class Planned(NamedTuple):
    scad_rel: str          # the owned-map key: cad.scad, canonical (see load_params)
    path: Path
    rendered: Rendered


class Owned(NamedTuple):
    value: object              # the literal to write — a reference's is already resolved
    slugs: list[str]
    ref: dict | None = None    # the manifest's reference object, when there was one
    cite: str = ""             # what it stands for: brd_l("xiao") / brd_xiao_w_measured()
    where: str = ""            # where the number lives: canary_board_lib.scad:43, spec rung


class Row(NamedTuple):
    id: str
    dims: dict          # {"l": float, "w": float, "t": float}
    status: str         # a rung of the ladder: measured / drawing / spec / unmeasured
    note: str
    line: int           # 1-based, in the library


class Fact(NamedTuple):
    name: str
    value: float
    line: int


class Registry(NamedTuple):
    path: Path
    rows: dict[str, Row]        # in file order
    facts: dict[str, Fact]      # in file order


class RegistryError(ValueError):
    """The board library cannot be read as a registry. A build failure, never
    a shorter registry — the colorways precedent."""


class RefError(ValueError):
    """A cad.params reference the registry cannot resolve; the message is the
    clause after `cad.params.<knob> = <ref>`."""


class Unspellable(ValueError):
    """A manifest value with no Customizer-literal spelling: a number whose
    shortest repr is exponent form (1e-05; 1e+16 against a decimal token),
    a non-finite one, a string holding a comment opener. Raised, never
    returned, so it can never be mistaken for _token's None — "the file
    already says it" — which is what let 0.00005 pass a --check against 0.6."""


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


def _finite(value) -> bool:
    """True for a number a literal can carry: finite, and small enough for a
    float (Python's json reads NaN, Infinity and a 400-digit integer; OpenSCAD
    has no spelling for any of them)."""
    try:
        return math.isfinite(float(value))
    except OverflowError:
        return False


def _fmt_number(value, old: str) -> str:
    """`value` spelled the way `old` was: integer stays integer when it can.
    Raises Unspellable when the shortest spelling is not a Customizer literal."""
    if not _finite(value):
        raise Unspellable(f"{json.dumps(value)} is not a finite number")
    f = float(value)
    if "." not in old and f.is_integer():
        return str(int(f))
    s = repr(f)
    if not re.fullmatch(r"-?\d+\.?\d*", s):      # 1e-05, 1e+16 — exponent form
        raise Unspellable(f"Python spells it {s} and a Customizer literal is a plain decimal "
                          f"(digits, one optional point) — no exponent form")
    return s


def _token(value, old: str) -> str | None:
    """The new literal for `value`, or None when `old` already says it. A value
    with no literal spelling raises Unspellable — it is never None."""
    if isinstance(value, bool):
        new = "true" if value else "false"
        return None if new == old else new
    if isinstance(value, str):
        if "//" in value or "/*" in value:
            # parse_scad splits the line at `//` and `/*` before it reads the
            # literal, so a string holding either would write fine and then
            # fail --check as "not a literal knob" — refused before the write.
            raise Unspellable("a string holding `//` or `/*` reads as a comment opener to "
                              "parse_scad, so --check could not find the knob it had written")
        if json.loads(old) == value:               # keep the file's own spelling
            return None
        return json.dumps(value, ensure_ascii=False)
    if not _finite(value):
        raise Unspellable(f"{json.dumps(value)} is not a finite number")
    if float(old) == float(value):
        return None
    return _fmt_number(value, old)


def _show(o: Owned) -> str:
    """A value as the manifest wrote it, with what it resolved to."""
    if o.ref is None:
        return json.dumps(o.value)
    return f"{json.dumps(o.ref)} (= {o.cite} = {json.dumps(o.value)})"


# ---------------------------------------------------------------------------
# comments — what OpenSCAD does not see, this file must not read
# ---------------------------------------------------------------------------

def _strip_comments(src: str) -> str:
    """`src` with every `// …` line comment and `/* … */` block blanked to
    spaces. Newlines are kept, so a line number counted on the result is the
    file's; string literals are kept whole, so a `//` inside a quoted note is
    not a comment. parse_scad's block-comment state, per character, with the
    string state OpenSCAD's lexer has and a line-splitter cannot: a
    BRD_REGISTRY row commented out with `//`, or a `function brd_*()` inside
    a block comment, is exactly as absent here as it is to OpenSCAD."""
    out: list[str] = []
    i, n, state = 0, len(src), "code"
    while i < n:
        c = src[i]
        if state == "code":
            if c == '"':
                state = "str"
                out.append(c)
            elif src.startswith("//", i):
                state = "line"
                out.append("  ")
                i += 1
            elif src.startswith("/*", i):
                state = "block"
                out.append("  ")
                i += 1
            else:
                out.append(c)
        elif state == "str":
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(src[i + 1])
                i += 1
            elif c == '"':
                state = "code"
        elif state == "line":
            if c == "\n":
                state = "code"
                out.append(c)
            else:
                out.append(" ")
        else:                                   # block
            if src.startswith("*/", i):
                state = "code"
                out.append("  ")
                i += 1
            else:
                out.append(c if c == "\n" else " ")
        i += 1
    return "".join(out)


def _statements(body: str) -> int:
    """How many `;`-terminated statements the CODE of one line carries —
    comments stripped, strings skipped — so a `;` in the help text or inside
    a quoted value is not a statement, and `$fn = 64; shared = 7;` is two
    whatever parse_scad makes of `$fn`."""
    code = _strip_comments(body)
    count, in_str, i = 0, False, 0
    while i < len(code):
        c = code[i]
        if in_str:
            if c == "\\":
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == ";":
            count += 1
        i += 1
    return count


# ---------------------------------------------------------------------------
# the board registry
# ---------------------------------------------------------------------------

def parse_board_registry(lib: Path | None = None) -> Registry:
    """canary_board_lib.scad's BRD_REGISTRY rows and brd_*() facts, by id, with
    the line each lives on — read as OpenSCAD reads it, comments stripped
    first. Raises RegistryError — naming the file — when the registry is
    missing, a row does not parse, or an id is defined twice."""
    lib = lib or REPO / BOARD_LIB_REL
    try:
        src = lib.read_text(encoding="utf-8")
    except OSError as e:
        raise RegistryError(f"{lib.name}: the board registry cannot be read ({e})") from None
    # Comments first: a row commented out with `//` or a fact inside `/* */`
    # is not a row and not a fact. Newlines survive the stripping, so every
    # line number below is counted on the stripped text and is the file's.
    code = _strip_comments(src)
    start = code.find("BRD_REGISTRY = [")
    if start < 0:
        raise RegistryError(f"{lib.name}: no `BRD_REGISTRY = [` — the board registry has moved "
                            f"or been renamed; cad.params references resolve from it")
    end = code.find("];", start)
    if end < 0:
        raise RegistryError(f"{lib.name}: `BRD_REGISTRY = [` is never closed with `];`")
    region = code[start:end]
    matches = list(_BRD_ROW.finditer(region))
    # Every row must parse, not just some (gen_builder_manifest.parse_colorways):
    # findall() would silently drop a row that drifts from the literal shape and
    # a reference to it would then be "unknown" — or worse, a shorter registry
    # would still resolve every OTHER reference and look healthy. Row starts
    # are counted independently of the row regex — on the same stripped text,
    # so a commented-out row is neither a row nor a start — and drift is a
    # build failure.
    row_starts = len(re.findall(r'\[\s*"', region))
    if not matches or len(matches) != row_starts:
        raise RegistryError(
            f"{lib.name}: parsed {len(matches)} of {row_starts} BRD_REGISTRY rows — a row has "
            f"drifted from the literal [id, length, width, thickness, status, note] shape "
            f"(lowercase id and status, plain numbers, one quoted note; keep each row a plain "
            f"literal): a shorter registry would resolve cad.params references wrongly")
    rows: dict[str, Row] = {}
    for m in matches:
        line = code.count("\n", 0, start + m.start()) + 1
        rid = m.group(1)
        if rid in rows:
            raise RegistryError(f'{lib.name}:{line}: BRD_REGISTRY row "{rid}" is defined twice '
                                f"(also line {rows[rid].line}) — board_selfcheck() refuses that "
                                f"at render time; a lookup here would pick one silently")
        rows[rid] = Row(rid, {"l": float(m.group(2)), "w": float(m.group(3)),
                              "t": float(m.group(4))},
                        m.group(5), re.sub(r"\s+", " ", m.group(6)).strip(), line)
    facts: dict[str, Fact] = {}
    for lineno, line_code in enumerate(code.splitlines(), start=1):
        for m in _FUNC.finditer(line_code):
            name = m.group("fn")
            if not _FACT_NAME.match(name):
                continue
            if name in facts:
                raise RegistryError(f"{lib.name}:{lineno}: `function {name}()` is defined twice "
                                    f"(also line {facts[name].line})")
            facts[name] = Fact(name, float(m.group("val")), lineno)
    return Registry(lib, rows, facts)


def resolve_ref(ref: dict, registry: Registry) -> tuple[float, str, str]:
    """(number, citation, provenance) for a reference: 21.0, `brd_l("xiao")`,
    `canary_board_lib.scad:43, spec rung`. Raises RefError saying what is
    wrong — the shape, the row, the dim or the fact."""
    lib = registry.path.name
    keys = set(ref)
    if keys == {"brd", "dim"}:
        rid, dim = ref["brd"], ref["dim"]
        if not isinstance(dim, str) or dim not in DIMS:
            raise RefError(f'names dim {json.dumps(dim)} — a dim is "l" (along-USB length, '
                           f'brd_l), "w" (width, brd_w) or "t" (PCB thickness, brd_t)')
        fn = DIMS[dim][0]
        row = registry.rows.get(rid) if isinstance(rid, str) else None
        if row is None:
            raise RefError(f'references {fn}({json.dumps(rid)}) but {lib} BRD_REGISTRY has no '
                           f'row {json.dumps(rid)} (rows: {", ".join(registry.rows)})')
        return row.dims[dim], f'{fn}("{rid}")', f"{lib}:{row.line}, {row.status} rung"
    if keys == {"brd_fn"}:
        name = ref["brd_fn"]
        if not isinstance(name, str) or not _FACT_NAME.match(name):
            raise RefError(f"names {json.dumps(name)} — a fact is a brd_<name> zero-argument "
                           f"numeric function of {lib} (facts: {', '.join(registry.facts)})")
        fact = registry.facts.get(name)
        if fact is None:
            raise RefError(f"references {name}() but {lib} defines no numeric `function {name}() "
                           f"= <number>;` (facts: {', '.join(registry.facts)})")
        return fact.value, f"{name}()", f"{lib}:{fact.line}"
    raise RefError(f"is not a registry reference — a reference is exactly {REF_SHAPE}")


def unreferenced(owned: dict[str, dict[str, Owned]],
                 registry: Registry) -> tuple[list[str], list[str]]:
    """(rows, facts) of the registry no manifest references, in file order.
    INFO, never an error: cases with no manifest cite them by comment."""
    rows = {o.ref["brd"] for k in owned.values() for o in k.values()
            if o.ref is not None and "brd" in o.ref}
    fns = {o.ref["brd_fn"] for k in owned.values() for o in k.values()
           if o.ref is not None and "brd_fn" in o.ref}
    return ([r for r in registry.rows if r not in rows],
            [f for f in registry.facts if f not in fns])


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
    """{cad.scad (repo-relative): {knob: Owned(value, [slugs], …)}}, errors.

    Every devices/*/device.json with a `cad.params` contributes to its
    `cad.scad`; a reference is resolved from the board registry here, so
    everything downstream sees a literal. A knob two manifests assert with
    different values is an error naming both. The first assertion's value
    stays in the map so the caller can still report the rest of the file.
    """
    owned: dict[str, dict[str, Owned]] = {}
    errors: list[str] = []
    registry: Registry | None = None
    try:
        registry = parse_board_registry(repo / BOARD_LIB_REL)
    except RegistryError as e:
        errors.append(f"{e} — no cad.params reference resolves until it parses")
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
        if not SCAD_PATH_RE.match(scad):
            errors.append(f"devices/{slug}: cad.scad {json.dumps(scad)} is not a case file path — "
                          f"devices/device.schema.json spells it docs/hardware/enclosure/<name>"
                          f".scad (lowercase letters, digits, underscores; no `./` or `..` "
                          f"segment), and cad.params owns nothing until it is")
            continue
        if not isinstance(params, dict):
            errors.append(f"devices/{slug}: cad.params must be an object of knob -> literal")
            continue
        # The key every lookup uses. A path the pattern admits is already
        # canonical; normalizing anyway means two spellings of one file can
        # never render independently and clobber each other on write.
        scad = Path(os.path.normpath(scad)).as_posix()
        per = owned.setdefault(scad, {})
        for key, value in params.items():
            if isinstance(value, dict):
                if registry is None:            # reported once, above
                    continue
                try:
                    number, cite, where = resolve_ref(value, registry)
                except RefError as e:
                    errors.append(f"devices/{slug}: cad.params.{key} = {json.dumps(value)} {e} "
                                  f"— references are resolved from {BOARD_LIB_REL}")
                    continue
                entry = Owned(number, [slug], value, cite, where)
            elif _kind(value) is None:
                errors.append(f"devices/{slug}: cad.params.{key} = {json.dumps(value)} is not a "
                              f"number, string, boolean or registry reference — a knob is a "
                              f"literal, or exactly {REF_SHAPE} that the registry resolves to one")
                continue
            elif _kind(value) == "number" and not _finite(value):
                # Python's json reads NaN and Infinity; OpenSCAD cannot spell
                # them, and nan == nan is False, so an equality check would
                # never settle. Refused here, by manifest and key.
                errors.append(f"devices/{slug}: cad.params.{key} = {json.dumps(value)} is not a "
                              f"finite number — NaN and Infinity have no Customizer-literal "
                              f"spelling; write the dimension as a plain decimal")
                continue
            else:
                entry = Owned(value, [slug])
            if key in per:
                prev = per[key]
                if not _same(prev.value, entry.value):
                    others = ", ".join(f"devices/{s}" for s in prev.slugs)
                    errors.append(
                        f"{Path(scad).name}: cad.params.{key} is asserted by {others} as "
                        f"{_show(prev)} and by devices/{slug} as {_show(entry)} — manifests "
                        f"sharing one case must agree on a shared key")
                    continue
                prev.slugs.append(slug)
            else:
                per[key] = entry
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
    # Bytes in, bytes out: read_text() would translate every CRLF to LF
    # (universal newlines), and a real edit would then rewrite a CRLF file
    # LF on every line. splitlines(keepends=True) splits exactly where
    # parse_scad's splitlines() does, so its line numbers index this list;
    # joining reproduces the bytes, whatever each line ends with.
    text = scad_path.read_bytes().decode("utf-8")
    lines = text.splitlines(keepends=True)
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
        idx = p["line"] - 1
        raw = lines[idx]
        body = raw.rstrip("\r\n")
        ending = raw[len(body):]
        # Counted on the line's code, not on the knobs parse_scad accepted from
        # it: `shared2 = 8; $fn = 32;` has one parsed knob and two statements,
        # and `mixed = 3; dep = mixed;` likewise — a rewrite may touch neither
        # neighbor, so any second statement refuses the line.
        k = _statements(body)
        if k > 1:
            errors.append(f"{tag(name)}: line {p['line']} holds {k} statements "
                          f"({body.strip()!r}) — split the line first, so a rewrite can never "
                          f"touch its neighbor")
            continue
        m = re.match(LINE_RE % re.escape(name), body)
        if not m:
            errors.append(f"{tag(name)}: could not locate the literal on line {p['line']} "
                          f"({body.strip()!r}) — the line is not `{name} = <literal>; …`")
            continue
        old = m.group(4)
        try:
            new = _token(value, old)
        except Unspellable as e:
            errors.append(f"{tag(name)}: {json.dumps(value)} cannot be spelled as a Customizer "
                          f"literal — {e}")
            continue
        if new is None:                    # the file already says it
            continue
        if not re.fullmatch(LITERAL, new):     # belt and braces: never a silent skip
            errors.append(f"{tag(name)}: {json.dumps(value)} cannot be spelled as a Customizer "
                          f"literal (would write {new!r})")
            continue
        new_body = m.group(1) + m.group(2) + m.group(3) + new + m.group(5)
        lines[idx] = new_body + ending
        changes.append(Change(p["line"], name, old, new, raw, new_body + ending))
    return Rendered("".join(lines), changes, errors)


# ---------------------------------------------------------------------------
# the gate and the writer
# ---------------------------------------------------------------------------

def _plan(devices_dir: Path, repo: Path) -> tuple[list[Planned], list[str],
                                                  dict[str, dict[str, Owned]]]:
    """Every owned case rendered in memory, every error so far, and the owned
    map — the whole run, short of writing. A test asserts the fixed point on
    this, never by calling write() against the live tree."""
    owned, errors = load_params(devices_dir, repo)
    plan: list[Planned] = []
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
        plan.append(Planned(scad_rel, path, r))
    return plan, errors, owned


def check(devices_dir: Path | None = None, repo: Path | None = None) -> list[str]:
    """Every problem, as one message each; [] means every owned literal equals
    its manifest. Renders in memory only. This is what the manifest linter
    appends to its own errors."""
    devices_dir = devices_dir or DEVICES_DIR
    repo = repo or REPO
    plan, errors, owned = _plan(devices_dir, repo)
    for scad_rel, path, r in plan:
        keys = owned[scad_rel]           # the key _plan iterated, not a re-derived path
        for c in r.changes:
            o = keys[c.name]
            slugs = ", ".join(f"devices/{s}" for s in o.slugs)
            if o.ref is not None:
                # The whole point of a reference: a registry correction reaches
                # the case through this message, then through a write.
                errors.append(
                    f"{path.name}:{c.line}: {c.name} = {c.old_token} in the .scad, but {slugs} "
                    f"cad.params references {o.cite} ({o.where}): registry says {c.new_token}, "
                    f"file says {c.old_token} — the manifest carries the join and the registry "
                    f"the number: run python3 docs/hardware/enclosure/gen_cad_params.py (then "
                    f"the regen order it prints), or name a different registry entry in the "
                    f"manifest")
            else:
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
    plan, errors, _ = _plan(devices_dir, repo)
    if errors:
        return [], errors
    written: list[tuple[Path, Change]] = []
    for _, path, r in plan:
        if not r.changes:
            continue
        path.write_bytes(r.text.encode("utf-8"))    # no newline translation, on any OS
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
        refs = sum(1 for k in owned.values() for o in k.values() if o.ref is not None)
        print(f"gen_cad_params.py: OK — {n} manifest-owned knobs across {len(owned)} case "
              f"file(s) equal their .scad literals ({refs} of them resolved from "
              f"{Path(BOARD_LIB_REL).name})")
        rows, facts = unreferenced(owned, parse_board_registry())   # parsed: check() was clean
        if rows or facts:
            print(f"INFO: registry entries no manifest references — rows: "
                  f"{', '.join(rows) or 'none'}; facts: {', '.join(facts) or 'none'}. Cases whose "
                  f"manifests own no knobs yet (the C6 display, the 7\" frame) and cases with no "
                  f"manifest (the doorbell, the gang plate, the Hammond chassis, the J-box, the "
                  f"bench fixture) cite them by comment; not an error")
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
    owned, _ = load_params()
    files = sorted({p for p, _ in written})
    for path, c in written:
        o = owned.get(_rel(path, REPO), {}).get(c.name)
        via = f"  (registry: {o.cite}, {o.where})" if o is not None and o.ref is not None else ""
        print(f"wrote {_rel(path, REPO)}:{c.line}  {c.name}: {c.old_token} -> {c.new_token}{via}")
    print(f"\n{len(written)} knob(s) in {len(files)} file(s) changed — render PNG previews of "
          f"every affected part of: {', '.join(p.name for p in files)}")
    print()
    print(REGEN_ORDER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
