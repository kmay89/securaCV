#!/usr/bin/env python3
"""lint_design_lang.py — the design-language conformance gate.

canary_core_lib.scad states the doctrine: the house look is a small set of
constants (`core_corner_r()`, `core_face_edge()`, the feature vocabulary),
and "a departure now has to type its own number AND say why." This lint is
that sentence as a build gate.

A case file's Customizer knob stays a LITERAL on purpose — a computed value
would vanish from the Customizer and from the website builder's manifest —
so the canon cannot be enforced by making cases read the functions. Instead:
every canonical parameter's default must either

  * equal the house value, or
  * carry `deviates:` in a comment on its own line, followed by the reason.

An unexplained outlier fails the build. That keeps the ledger of deviations
IN the files, next to the numbers, where the Customizer help and the next
editor will see it — not in a sidecar list that rots.

Second rule: a module the libraries own may not be redefined in a case file.
Four copies of the vent cluster had already forked once (hole size drifted on
two outdoor cases); the de-fork stays de-forked.

Third rule: a knob's help stays on the knob's own line. The web builder and
the Lab catalog read Customizer help through gen_builder_manifest.parse_scad,
which keeps a trailing `//` comment only on a line that holds ONE knob — so
`a = 1;  b = 2;  // help` reaches neither knob (and the Lab's own parser,
gen_enclosures.py, drops the whole line). 125 knobs had lost their help that
way (audit 2026-09, C1). A line holding two or more knobs AND a trailing
comment now fails: split it, one knob per line, each with its own help. So
does a knob beside a computed assignment and a trailing comment (C32): the
parser reads only literals, so it gives that help to the knob although it
may be the neighbor's (the 1.69's skirt_wall wore skirt_dep's). Every
assignment on a knob's line counts, a string's text and a comparison's `=`
excepted.
HELP_LINE_DEBT held the few lines the first sweep deliberately left (the 7"
case's four); the second (C10) split them, so the ledger is empty and can
only stay that way: a new shared-help line fails, and so would a listed one
that is gone (the entry must go with it).

Fourth rule: a knob name keeps ONE meaning across the catalog. People (and
agents) learn `usb_w` in one case and read it the same way in the next, so a
name that means two things transfers a wrong skill silently: `usb_w` was the
boot-clearance wall opening in the case files and the connector shell in the
display cases, `vm_*` a Grove Vision module in four files and a radar carrier
in two, `skirt_t` a rain skirt in four and a snap finger in one, `clip_w` a
6 mm board-clip tab in twelve and a 45 mm belt clip in one (audit 2026-09,
C2). The minority meanings were renamed (usb_shell_*, usb_slot_*, radar_*,
finger_t, leaf_w; and C10 gave the J-box's pigtail pass-through, which no
plug or boot ever crosses, pigtail_*), and KNOB_MEANINGS holds every one of
those names to its meaning through its help: a listed knob whose help does
not say its meaning word — or says the other meaning's — fails, naming the
name to use instead.
A listed knob with no help passes: there is no meaning on it to transfer yet
(writing that help is the parametric-UX backlog's next wave).

Fifth rule: the stud/keyhole hanging interface keeps ONE group name. The
README sells it as the part of the design language that transfers between
parts, and the audit found it under 14 group names across 17 files (C10). A
knob of the interface itself — the blind pocket's kh_* numbers, the T-stud's
stud_* numbers (INTERFACE_KNOBS) — sits in a group named INTERFACE_GROUP
(the builder's short name: text after " — " is a per-file note). The C3
pocket case's egg hanger is exempt (INTERFACE_EXEMPT, with its reason).

Sixth rule: a knob with a stated range keeps its whole help on its own line.
The web builder draws such a knob as a slider with that line's help beside
it, the Lab lists its range next to the same help, and neither reads the
comment lines below. C10 moved six ranges off continuation lines onto their
knobs, and the help on each of those lines had been wrapped mid-sentence
("...cavity_cut): the sharp", "...the validated spot). The", "...; bond
with"), so the knob came with half a sentence (review of C10). When a
comment-only continuation follows a ranged knob, its help must end at a
sentence boundary: parentheses closed, no dangling article, conjunction or
preposition (RUN_ON_WORDS), and the continuation starting a new sentence (a
capital letter or a digit). A knob without a range shows its help the same
way but is not judged yet: help wrapped that way predates C10 in many
files, and the rule starts where there is no debt.

Seventh rule: a group header heads at least one assignment before the next
header. The builder's parser and the Lab's both file a knob under the last
header above it and drop an empty group without a word, so a header left
above the wrong line moves its knobs into another group with every gate
green: the 1.69's [Side cluster] sat directly over [Port / button datums],
and its three side knobs were filed as port datums (C22). A header that is a
note on purpose (the 1.47 hallway stick's [Cooling], which says it has no vents)
is listed in EMPTY_GROUP_OK with its reason, and the list only shrinks.

Run from the repo root:  python3 scripts/lint_design_lang.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENC = ROOT / "docs" / "hardware" / "enclosure"
sys.path.insert(0, str(ENC))
# The parser whose output the web builder and the Lab catalog carry: the help
# rules below judge what IT keeps, so they cannot disagree with it.
import gen_builder_manifest  # noqa: E402

# The canon: parameter names (both naming dialects) -> the house value and
# the core/mount function that states it. Keep this in step with the
# functions themselves — the self-check below parses the libs and fails if
# this table and the libraries disagree.
CANON = {
    "corner_r":       (3.0,  "canary_core_lib.scad",  "core_corner_r"),
    "r_out":          (3.0,  "canary_core_lib.scad",  "core_corner_r"),
    "lid_edge":       (0.8,  "canary_core_lib.scad",  "core_face_edge"),
    "lid_edge2":      (0.8,  "canary_core_lib.scad",  "core_face_edge2"),
    "foot_cham":      (0.5,  "canary_core_lib.scad",  "core_foot_cham"),
    "wall_t":         (2.0,  "canary_core_lib.scad",  "core_wall"),
    "wall":           (2.0,  "canary_core_lib.scad",  "core_wall"),
    "lp_d":           (3.0,  "canary_core_lib.scad",  "core_lightpipe_d"),
    "vent_pad_d":     (12.0, "canary_core_lib.scad",  "core_vent_pad_d"),
    "vent_pad_depth": (0.8,  "canary_core_lib.scad",  "core_vent_pad_depth"),
    "vent_ring_d":    (6.0,  "canary_core_lib.scad",  "core_vent_ring_d"),
    "vent_hole_d":    (1.0,  "canary_core_lib.scad",  "core_vent_hole_d"),
    "vent_holes":     (10,   "canary_core_lib.scad",  "core_vent_holes"),
    "tol_slide":      (0.20, "canary_core_lib.scad",  "core_tol_slide"),
    "tol_press":      (0.10, "canary_core_lib.scad",  "core_tol_press"),
    "tol_hole":       (0.30, "canary_core_lib.scad",  "core_tol_hole"),
    "kh_head_d":      (7.0,  "canary_mount_lib.scad", "mount_kh_head_d"),
}

# The canon's word-valued knobs: name -> (the house choice, where it is stated).
# Empty since the parting line moved to the back face (canary_core_lib pl_*):
# there is no `screw_from` any more — every released case is a one-piece
# shell with a piston plate, and the screws have exactly one direction. The
# table stays so the next word-valued house choice has a home.
CANON_WORD = {}

# Modules the libraries own. A case redefining one is a fork by definition.
LIB_OWNED = [
    "vent_cluster", "core_vent_cluster", "core_lightpipe_bore",
    "soft_edge_plate", "foot_chamfer_ring", "seam_reveal_cut",
    "inner_cove_ring", "rrect", "rrect2d", "cs_cone90_cut",
    "cb_flat_cut", "cb_head_pad", "mount_keyhole_pocket", "mount_tstud",
    "port_bridge_profile2d", "port_usbc_stadium2d", "boss_tower", "wall_rib",
]

# Library files (define, never lint), and files that are not cases.
SKIP = {
    "canary_core_lib.scad", "canary_port_lib.scad", "canary_mount_lib.scad",
    "canary_snap_lib.scad", "canary_mark_lib.scad", "canary_board_lib.scad",
    "canary_color_lib.scad", "canary_vent_lib.scad", "canary_rib_lib.scad",
    "canary_panel_lib.scad", "canary_cradle_lib.scad",
    "canary_case_fitcheck.scad", "canary_s3_lcd7_fitcheck.scad",
    "canary_templates_2d.scad",
}

# A top-level assignment line, and then EVERY `name = value;` on it —
# Customizer defaults are frequently grouped (`wall_t = 2.2;  floor_t = 2.2;
# lid_t = 2.4;`), and an anchored single match would leave every value after
# the first outside the gate.
ASSIGN_LINE = re.compile(r"^[a-z_][a-z0-9_]*\s*=")
ASSIGN_EACH = re.compile(r"(?:^|(?<=;))\s*(?P<name>[a-z_][a-z0-9_]*)\s*=\s*(?P<val>-?\d+(?:\.\d+)?)\s*;")
# A deviation must SAY WHY: `deviates:` followed by at least two words. A
# bare marker is an outlier wearing a costume, not a declared decision.
DEVIATES = re.compile(r"deviates:\s*\S+\s+\S+")
FUNC = re.compile(r"function\s+(?P<fn>[a-z_][a-z0-9_]*)\(\)\s*=\s*(?P<val>-?\d+(?:\.\d+)?)\s*;")


def selfcheck():
    """The CANON table must agree with the libraries it quotes."""
    bad = []
    lib_vals = {}
    for lib in {lib for _, lib, _ in CANON.values()}:
        text = (ENC / lib).read_text()
        for m in FUNC.finditer(text):
            lib_vals[(lib, m.group("fn"))] = float(m.group("val"))
    for name, (want, lib, fn) in CANON.items():
        got = lib_vals.get((lib, fn))
        if got is None:
            bad.append(f"CANON['{name}'] quotes {lib}:{fn}() which does not exist")
        elif abs(got - float(want)) > 1e-9:
            bad.append(f"CANON['{name}'] says {want} but {lib}:{fn}() = {got} — "
                       "update the table (it mirrors the libs, it does not rule them)")
    return bad


def lint_file(path):
    problems = []
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        code = line.split("//", 1)[0]
        if ASSIGN_LINE.match(code):
            for m in ASSIGN_EACH.finditer(code):
                if m.group("name") not in CANON:
                    continue
                want, lib, fn = CANON[m.group("name")]
                got = float(m.group("val"))
                if abs(got - float(want)) > 1e-9 and not DEVIATES.search(line):
                    problems.append(
                        f"{path.name}:{lineno}: {m.group('name')} = {m.group('val')} "
                        f"but the house value is {want} ({lib}:{fn}()) — conform it, "
                        "or say why with a `deviates: <reason>` comment on the line "
                        "(a bare marker with no reason does not count)")
        mw = re.match(r'^(?P<name>[a-z_][a-z0-9_]*)\s*=\s*"(?P<val>[^"]*)"\s*;', code)
        if mw and mw.group("name") in CANON_WORD:
            want, where = CANON_WORD[mw.group("name")]
            if mw.group("val") != want and not DEVIATES.search(line):
                problems.append(
                    f"{path.name}:{lineno}: {mw.group('name')} = \"{mw.group('val')}\" "
                    f"but the house choice is \"{want}\" ({where}) — conform it, or say "
                    "why with a `deviates: <reason>` comment on the line")
        mm = re.match(r"\s*module\s+([a-z_][a-z0-9_]*)\s*\(", line)
        if mm and mm.group(1) in LIB_OWNED and not DEVIATES.search(line):
            problems.append(
                f"{path.name}:{lineno}: redefines library module "
                f"'{mm.group(1)}' — a copy is a fork; call the library's, "
                "or say why with a `deviates: <reason>` comment on the line")
    return problems


# Shared-help lines left alone ON PURPOSE, keyed by (file, first knob on the
# line). The 2026-09 sweep (C1) left the 7" case's four — its sources are
# hashed by gen_stamp.py — and C10 split them: gen_stamp's digest is
# comment-free and whitespace-normalized, so the split re-stamped nothing
# (gen_stamp.py --check stayed green). The ledger is paid off; it only shrinks.
HELP_LINE_DEBT: set[tuple[str, str]] = set()


# The fourth rule's table (DESIGN_RULES.md §10 states it for people). name ->
# (its meaning, a regex its help must match, a regex its help must NOT match or
# None, what to call the other meaning). Words, not values: a value is a design
# decision each case makes; a meaning is what the next reader assumes.
KNOB_MEANINGS = {
    "usb_w":         ("the wall opening a USB cable's plug and boot pass through",
                      r"\bboots?\b", None,
                      "usb_shell_w for a connector-shell size, usb_slot_w for a side slot, "
                      "pigtail_w for a pigtail cable's pass-through"),
    "usb_h":         ("the wall opening a USB cable's plug and boot pass through",
                      r"\bboots?\b", None,
                      "usb_shell_h for a connector-shell size, usb_slot_h for a side slot, "
                      "pigtail_h for a pigtail cable's pass-through"),
    "usb_shell_w":   ("the USB connector shell, or an opening sized to it", r"\bshell", None, None),
    "usb_shell_h":   ("the USB connector shell, or an opening sized to it", r"\bshell", None, None),
    "usb_slot_w":    ("a side slot a USB plug enters through", r"\bslot", None, None),
    "usb_slot_h":    ("a side slot a USB plug enters through", r"\bslot", None, None),
    # the J-box's (C10): the connector sits 7.1 inside the wall, so the plug
    # overmold of whatever is plugged in stands in the slot (2026-10-23: it
    # opens to the Type-C spec overmold) — a strain-relief boot never crosses it
    "pigtail_w":     ("the slot a pigtail cable and its plug's overmold pass through",
                      r"\bpigtail", r"\bboots?\b", "usb_w for an opening a plug and boot pass"),
    "pigtail_h":     ("the slot a pigtail cable and its plug's overmold pass through",
                      r"\bpigtail", r"\bboots?\b", "usb_h for an opening a plug and boot pass"),
    "vm_l":          ("the Grove Vision AI V2 module", r"grove|vision|\bmodule\b",
                      r"radar|mr60|carrier", "radar_l for the MR60 radar carrier"),
    "vm_w":          ("the Grove Vision AI V2 module", r"grove|vision|\bmodule\b",
                      r"radar|mr60|carrier", "radar_w for the MR60 radar carrier"),
    "vm_front_h":    ("the Grove Vision AI V2 module", r"grove|vision|\bmodule\b",
                      r"radar|mr60|carrier", "radar_front_h for the MR60 radar carrier"),
    "radar_l":       ("the MR60 radar carrier", r"radar|mr60|carrier", r"grove|vision", None),
    "radar_w":       ("the MR60 radar carrier", r"radar|mr60|carrier", r"grove|vision", None),
    "radar_front_h": ("the MR60 radar carrier", r"radar|mr60|carrier", r"grove|vision", None),
    "skirt_t":       ("the rain (drip-edge) skirt's wall", r"skirt", r"finger",
                      "finger_t for a snap finger"),
    "finger_t":      ("a snap finger's thickness", r"finger", None, None),
    "clip_w":        ("the snap board-clip's tab width", r"\btab\b", r"belt|leaf|extrusion",
                      "leaf_w for a belt clip's width"),
    "leaf_w":        ("a belt clip's leaf (extrusion) width", r"leaf|belt", None, None),
}


def lint_meanings(path):
    """Fourth rule: a listed knob's help says its meaning, and not another one."""
    problems = []
    for group in gen_builder_manifest.parse_scad(path, with_lines=True):
        for param in group["params"]:
            rule = KNOB_MEANINGS.get(param["name"])
            desc = param.get("desc", "")
            if not rule or not desc:
                continue
            meaning, must, must_not, instead = rule
            if re.search(must, desc, re.I) and not (must_not and re.search(must_not, desc, re.I)):
                continue
            problems.append(
                f"{path.name}:{param['line']}: `{param['name']}` means {meaning} across the "
                f"catalog, but its help reads \"{desc}\". If it means that, say so in the "
                "help; if it means something else, give it its own name"
                + (f" — {instead}" if instead else "")
                + " (KNOB_MEANINGS; DESIGN_RULES.md §10)")
    return problems


# The fifth rule's table (DESIGN_RULES.md §10 states it for people).
INTERFACE_GROUP = "Stud/keyhole interface"
INTERFACE_KNOBS = {
    "kh_head_d", "kh_shank_d", "kh_slot_l", "kh_head_h", "kh_face", "kh_click",
    "stud_gap", "stud_d", "stud_head", "stud_head_t", "stud_stem_h",
}
INTERFACE_EXEMPT = {
    "canary_c3_lcd147.scad": "its egg hanger is a through-cut screw hanger sized to the largest wall-"
                             "screw head (kh_head_d 9.5), not the blind T-stud pocket",
}


def lint_interface_group(path):
    """Fifth rule: every interface knob sits in the one interface group."""
    if path.name in INTERFACE_EXEMPT:
        return []
    problems = []
    for group in gen_builder_manifest.parse_scad(path, with_lines=True):
        short = group["name"].split(" — ")[0].strip()
        for param in group["params"]:
            if param["name"] in INTERFACE_KNOBS and short != INTERFACE_GROUP:
                problems.append(
                    f"{path.name}:{param['line']}: `{param['name']}` is a knob of the catalog's "
                    f"stud/keyhole interface, but it sits in the group [{group['name']}]. The "
                    f"interface keeps one name everywhere it appears — put it under "
                    f"/* [{INTERFACE_GROUP}] */ (a per-file note may follow the bracket) "
                    "(DESIGN_RULES.md §10)")
    return problems


# The sixth rule's table: a help that ends on one of these words (an article,
# a conjunction, a preposition, a relative pronoun) is a sentence cut off by
# a line wrap, whatever the next line says.
RUN_ON_WORDS = {
    "a", "an", "the", "and", "or", "but", "nor", "so", "with", "of", "to", "in", "on",
    "at", "for", "by", "from", "as", "into", "onto", "via", "per", "than", "that", "which",
}


def lint_ranged_help(path):
    """Sixth rule: a knob with a stated range keeps its whole help on its line."""
    problems = []
    lines = path.read_text(encoding="utf-8").splitlines()
    for group in gen_builder_manifest.parse_scad(path, with_lines=True):
        for param in group["params"]:
            desc = param.get("desc", "")
            if "min" not in param or not desc:
                continue
            nxt = lines[param["line"]] if param["line"] < len(lines) else ""
            cont = re.match(r"^\s+//\s*(\S.*)$", nxt)
            if not cont:
                continue
            last = re.search(r"([A-Za-z]+)\W*$", desc)
            cut = []
            if desc.count("(") > desc.count(")"):
                cut.append("an open parenthesis")
            if desc.rstrip()[-1] in ",:;(":
                cut.append(f"a trailing '{desc.rstrip()[-1]}'")
            if last and last.group(1).lower() in RUN_ON_WORDS:
                cut.append(f"a dangling '{last.group(1)}'")
            if not re.match(r"[A-Z0-9]", cont.group(1)):
                cut.append(f"a next line that goes on mid-sentence ('{cont.group(1)[:24]}…')")
            if cut:
                problems.append(
                    f"{path.name}:{param['line']}: `{param['name']}` states a range, so the "
                    "builder draws a slider and the Lab lists the range, each with this line's "
                    f"help beside it — and neither reads further. Its help ends \"…{desc[-32:]}\" "
                    f"with {' and '.join(cut)}, so the knob shows half a sentence. End the help "
                    "at a sentence boundary on the knob's line, and start the next line with a "
                    "new sentence (DESIGN_RULES.md §10)")
    return problems


def knob_lines(path):
    """{line number: [knob names]} as gen_builder_manifest.parse_scad sees them."""
    by_line = {}
    for group in gen_builder_manifest.parse_scad(path, with_lines=True):
        for param in group["params"]:
            by_line.setdefault(param["line"], []).append(param["name"])
    return by_line


# What lint_help_lines counts on a knob's line: every statement of its code
# that assigns. A string's text is not code (`label = "a = b"` is one
# assignment), and a comparison's `=` is not an assignment: an `=` counts
# unless `=`, `<`, `>` or `!` stands before it or `=` after it. A statement
# assigns when any `=` in it does — `y = let(m = 3) k * m` is one statement
# (the computed y), and so is `echo(w = knob)`, whose help may be just as
# much its — and the message names each one for what it is.
SCAD_STRING = re.compile(r'"(?:[^"\\]|\\.)*"')
ASSIGNS = re.compile(r"(?<![=<>!])=(?!=)")
STATEMENT_NAME = re.compile(r"\s*([$A-Za-z_][A-Za-z0-9_]*)\s*(?<![=<>!])=(?!=)")
FUNCTION_NAME = re.compile(r"\s*function\s+([$A-Za-z_][A-Za-z0-9_]*)\s*\(")


def _code(line):
    """A line's code: the text before the first `//` once strings are
    stripped, with block comments taken out the way
    gen_builder_manifest.parse_scad takes them out."""
    code = SCAD_STRING.sub('""', line)
    if "*/" in code and ("/*" not in code or code.index("*/") < code.index("/*")):
        code = code.split("*/", 1)[1]          # the end of a block opened above
    code = re.sub(r"/\*.*?\*/", " ", code)
    return code.split("/*", 1)[0].split("//", 1)[0]


def assignments(line):
    """The statements a line's code assigns in, in order, as (name, noun):
    `name` is what a `name = …` statement assigns (None for any other
    statement), and `noun` is how a message names the statement — "the
    computed half", "the special variable $fn", "the function half()", or
    the statement's own text in backticks (`echo(w = knob)`)."""
    code, parts, depth, start = _code(line), [], 0, 0
    for i, ch in enumerate(code):
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth = max(0, depth - 1)
        elif ch == ";" and depth == 0:
            parts.append(code[start:i])
            start = i + 1
    parts.append(code[start:])
    out = []
    for text in (t.strip() for t in parts):
        if not ASSIGNS.search(text):
            continue
        named, fn = STATEMENT_NAME.match(text), FUNCTION_NAME.match(text)
        if named:
            name = named.group(1)
            noun = (f"the special variable {name}" if name.startswith("$")
                    else f"the computed {name}")
            out.append((name, noun))
        elif fn:
            out.append((None, f"the function {fn.group(1)}()"))
        else:
            out.append((None, f"`{text if len(text) <= 48 else text[:47] + '…'}`"))
    return out


def assigned_names(line):
    """The names a line's `name = …` statements assign, in order."""
    return [name for name, _ in assignments(line) if name]


def lint_help_lines(path, debt=frozenset()):
    """Third rule: a line that holds a knob AND a trailing help holds nothing
    else that assigns — no second knob, and no computed neighbor.

    gen_builder_manifest.parse_scad keeps the help only on a line where it
    reads ONE literal assignment, so two knobs lose it (C1). It reads only
    literals, so beside a computed assignment (`a = 1;  b = a / 2;  // help`)
    it gives the knob a help that may be the neighbor's: the 1.69's
    skirt_wall carried skirt_dep's "must not exceed back_stack" that way
    (C10c-2), and the rule passed it because it counted knobs only (C32).
    Every statement on the line that assigns counts now (assignments).

    Returns (problems, debt entries seen) — main() fails an entry never seen.
    """
    problems, seen = [], set()
    lines = path.read_text().splitlines()
    for lineno, names in sorted(knob_lines(path).items()):
        line = lines[lineno - 1]
        others = [noun for name, noun in assignments(line) if name not in names]
        if len(names) + len(others) < 2:
            continue
        # "the computed a, b" once, then the statements that are not values
        values = [n.removeprefix("the computed ") for n in others if n.startswith("the computed ")]
        nouns = ([f"the computed {', '.join(values)}"] if values else []) + \
            [n for n in others if not n.startswith("the computed ")]
        neighbors = " and ".join(nouns)
        # the same split parse_scad makes: everything after the first `//`
        comment = (line.split("//", 1) + [""])[1]
        if not comment.strip():
            continue
        if (path.name, names[0]) in debt:
            seen.add((path.name, names[0]))
        elif others and len(names) == 1:
            problems.append(
                f"{path.name}:{lineno}: the knob {names[0]} shares the line and its trailing "
                f"help with {neighbors} — the builder's parser reads only literal assignments, "
                f"so it hands that help to {names[0]} although the help may belong to "
                f"{others[0]} (the 1.69's skirt_wall carried skirt_dep's help that way). Move "
                f"{others[0].removeprefix('the computed ')} to a line of its own, so the knob's "
                "line holds the knob and its own help only")
        elif others:
            problems.append(
                f"{path.name}:{lineno}: the knobs {', '.join(names)} share the line and its "
                f"trailing help with {neighbors} — the builder's parser keeps help only on a "
                "one-knob line, so none of the knobs gets it. Split the line: one knob per "
                f"line, each with its own help, and {others[0].removeprefix('the computed ')} "
                "on a line of its own")
        else:
            problems.append(
                f"{path.name}:{lineno}: {len(names)} knobs ({', '.join(names)}) share one "
                "trailing help, and the builder's parser keeps help only on a one-knob "
                "line — so none of them gets it. Split the line, one knob per line, and "
                "give each its own help (a group comment becomes one per knob, each "
                "citing its own fact; a `deviates:` reason stays on its knob's line)")
    return problems, seen


# The seventh rule's ledger: a group header that heads no assignment on
# purpose, as (file, group) with the reason. Shrink-only, like HELP_LINE_DEBT:
# a listed header that heads something now, or is gone, fails until its entry
# goes.
EMPTY_GROUP_OK = {
    ("canary_s3_lcd147.scad", "Cooling"):
        "a design note where vent knobs would be: the case has no vents on purpose, "
        "and the header says so",
}


def customizer_lines(path):
    """(line number, group name or None, code) for each line
    gen_builder_manifest.parse_scad reads before the first module, with
    block comments taken out as it takes them out. A header line carries its
    group's name and no code (the parser reads nothing else on it)."""
    in_block = False
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if re.match(r"^module\s", raw):
            return
        line = raw
        if in_block:
            if "*/" not in line:
                continue
            line = line.split("*/", 1)[1]
            in_block = False
        m = gen_builder_manifest.GROUP_RE.search(line)
        if m:
            yield lineno, m.group(1).strip(), ""
            in_block = "*/" not in line[m.end():]
            continue
        if "/*" in line:
            before, after = line.split("/*", 1)
            if "*/" in after:
                line = before + after.split("*/", 1)[1]
            else:
                in_block = True
                line = before
        yield lineno, None, _code(line)


def lint_empty_groups(path, allowed=frozenset()):
    """Seventh rule: a group header heads at least one assignment.

    Both parsers file each knob under the last header above it and drop a
    header with nothing under it without a word, so a header left above the
    wrong line moves its knobs into the group before it, and every gate stays
    green. The 1.69's [Side cluster] sat directly over [Port / button
    datums], so opt_side, side_open_h and side_open_dy were filed as port
    datums (C22). A header that is a note on purpose is listed in
    EMPTY_GROUP_OK.

    Returns (problems, allowed entries seen) — main() fails an entry never seen.
    """
    problems, seen = [], set()
    rows = list(customizer_lines(path))
    heads = [(i, row[0], row[1]) for i, row in enumerate(rows) if row[1]]
    for k, (i, lineno, name) in enumerate(heads):
        if name.lower() == "hidden":
            continue
        stop = heads[k + 1][0] if k + 1 < len(heads) else len(rows)
        if any(ASSIGNS.search(code) for _, _, code in rows[i + 1:stop]):
            continue
        if (path.name, name) in allowed:
            seen.add((path.name, name))
            continue
        until = (f"the next header [{heads[k + 1][2]}] (line {heads[k + 1][1]})"
                 if k + 1 < len(heads) else "the first module")
        problems.append(
            f"{path.name}:{lineno}: the group header [{name}] heads no assignment before "
            f"{until}. The builder and the Lab drop an empty group and file each knob under "
            "the last header above it, so the knobs this header was written for sit in "
            "another group (the 1.69's side-cluster knobs were filed as port datums that "
            "way, C22). Move the header down to the first knob it heads, or delete it; a "
            "header kept as a note goes in EMPTY_GROUP_OK with the reason")
    return problems, seen


def main():
    problems = selfcheck()
    debt_seen, empty_seen = set(), set()
    for path in sorted(ENC.glob("canary_*.scad")):
        if path.name in SKIP:
            continue
        problems += lint_file(path)
        found, seen = lint_help_lines(path, HELP_LINE_DEBT)
        problems += found
        problems += lint_meanings(path)
        problems += lint_interface_group(path)
        problems += lint_ranged_help(path)
        debt_seen |= seen
        found, seen = lint_empty_groups(path, EMPTY_GROUP_OK.keys())
        problems += found
        empty_seen |= seen
    for name, knob in sorted(HELP_LINE_DEBT - debt_seen):
        problems.append(f"{name}: HELP_LINE_DEBT lists the shared-help line starting "
                        f"'{knob} =', which is gone — delete the entry (the ledger only shrinks)")
    for name, group in sorted(EMPTY_GROUP_OK.keys() - empty_seen):
        problems.append(f"{name}: EMPTY_GROUP_OK lists the header [{group}], which heads an "
                        "assignment now or is gone — delete the entry (the ledger only shrinks)")
    if debt_seen:
        print(f"INFO: {len(debt_seen)} shared-help knob line(s) left by decision "
              "(HELP_LINE_DEBT)")
    if problems:
        for p in problems:
            print(f"::error::design language: {p}")
        print(f"\ndesign language: {len(problems)} problem(s)")
        return 1
    print("design language OK — every canonical default conforms or explains itself, "
          "every knob's help is on its own line, every ranged knob's help ends there, "
          "every shared name keeps its meaning, the stud/keyhole interface keeps its "
          "one group name, and every group header heads something")
    return 0


if __name__ == "__main__":
    sys.exit(main())
