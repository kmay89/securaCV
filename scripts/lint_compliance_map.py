#!/usr/bin/env python3
"""Fail the build on a compliance map that says more than the tree can show.

    python3 scripts/lint_compliance_map.py               # the real tree
    python3 scripts/lint_compliance_map.py --root DIR    # a test tree

docs/compliance/ maps the clauses of outside regimes (21 CFR Part 11, EU GMP
Annex 11, ALCOA+, HIPAA 164.312, US evidence rules, eIDAS ledgers, the C2PA
program, BS 10008, the FIPS split-knowledge vocabulary) to the SecuraCV
mechanisms that exist. A map is only worth reading while every row is true,
so this gate holds each one to the disk:

  - the map schema `securacv-compliance-map:v1` is closed: an unknown or a
    missing key is an error at every level;
  - a row's status comes from the closed set in spec/witness_dictionary.json
    (`compliance_map_status`), and each status carries its evidence: a
    `structural` row names a test function that exists under a test
    attribute and a code symbol that exists on a non-comment line, so a
    renamed test or a removed mechanism turns the build red until the row is
    downgraded in the same change;
  - every evidence path is inside the repository and on disk, and every
    `#anchor` is a real heading;
  - no clause is marked verified when its source was not fetched, and no
    BS 10008 clause number appears (the standard was not read);
  - no result-looking number sits in a map (results live in the cited
    report, AGENTS.md rule 4);
  - no word that reads as a claim of standing under a regime appears, about
    SecuraCV, in any map string or in any prose file under docs/compliance/.

The claim words are enumerated ONCE, in CLAIM below, and nowhere else in the
repository's prose (the AGENTS.md rule 3b practice: a doc that spells a
banned list out is a doc the next sweep rewrites). Only the regime proper
nouns in MASK are blanked before CLAIM runs; quotes, backticks and fenced
blocks are NOT masked, so a quoted claim is still a claim. The MUST_PASS /
MUST_FAIL samples guard the guard, as in scripts/lint_fleet_word.py.

Exit 0 clean, 1 findings, 2 a file that cannot be parsed.
Stdlib only. The tests: scripts/tests/test_lint_compliance_map.py.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import unicodedata
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lint_md_links import anchors_of, slugify  # noqa: E402,F401  (slugify: shared rule)

REPO = Path(__file__).resolve().parents[1]
PREFIX = "lint_compliance_map: "

SCHEMA = "securacv-compliance-map:v1"
DICTIONARY = "spec/witness_dictionary.json"
MAPS_DIR = "docs/compliance/maps"
PROSE_DIR = "docs/compliance"
README = "docs/compliance/README.md"
ENTERPRISE_CUSTODY = "docs/security/ENTERPRISE_CUSTODY.md"
STATUSES = ("structural", "procedural", "designed", "not-applicable", "not-pursued")
PROSE_DETERMINATION = "the regulator, the court or the operator"

# ── The closed schema ───────────────────────────────────────────────────────
TOP_REQUIRED = ("schema", "regime", "last_reviewed", "rows")
TOP_OPTIONAL = ("preamble",)
REGIME_REQUIRED = ("id", "title", "kind", "determination_by", "source")
SOURCE_REQUIRED = ("citation", "access", "note")
ROW_REQUIRED = ("id", "clause", "clause_verified", "requirement", "mechanism", "status", "evidence")
ROW_OPTIONAL = ("operator_action", "limits", "reason", "depends_on", "alcoa")
EVIDENCE_REQUIRED = ("kind", "path")
EVIDENCE_OPTIONAL = ("test", "symbol", "anchor")

REGIME_KINDS = ("regulation", "standard", "rule-of-evidence", "program", "vocabulary", "tracker")
SOURCE_ACCESS = ("fetched", "blocked", "paywalled", "not-attempted")
EVIDENCE_KINDS = ("test", "code", "procedure", "design", "decision", "tracker", "doc")
ALCOA = ("Attributable", "Legible", "Contemporaneous", "Original", "Accurate",
         "Complete", "Consistent", "Enduring", "Available")
DEPENDS_ON = tuple(
    [f"item3:{x}" for x in "FGHIJKL"]
    + [f"item4:{x}" for x in ("M0", "M", "N", "O", "P", "Q", "R")]
    + [f"item5:{x}" for x in "STUVWXY"]
    + [f"item6:CC{n}" for n in range(1, 8)]
    + [f"item7:CP{n}" for n in range(2, 7)]
    + ["upstream:c2pa-cawg-cde"]
)

CAPS = {"title": 160, "requirement": 400, "mechanism": 800, "operator_action": 400,
        "limits": 400, "reason": 400}
PREAMBLE_CAP = 1200
MIN_REASON = 40

REGIME_ID_RE = re.compile(r"^[a-z0-9_]{2,40}$")
ROW_ID_PATTERN = r"^[A-Za-z0-9][A-Za-z0-9().:§ _-]{0,47}$"
ROW_ID_RE = re.compile(ROW_ID_PATTERN)
MONTH_RE = re.compile(r"^\d{4}-(0[1-9]|1[0-2])$")
CLAUSE_NUMBER_RE = re.compile(r"\b\d+(\.\d+)+\b")
RESULT_RE = re.compile(r"\d+(\.\d+)?\s*%|\b(precision|recall|accuracy|F1)\b\s*[:=]?\s*\d", re.IGNORECASE)
BIDI = {chr(c) for c in range(0x202A, 0x202F)} | {chr(c) for c in range(0x2066, 0x206A)}

# ── The claim words: the one place they are spelled out ─────────────────────
CLAIM = re.compile(
    r"\b(certif(?:ied|y|ies|iable)|(?:non-)?compliant|complies|conformant|conforms|accredited|"
    r"qualif(?:ied|ies)|satisf(?:y|ies|ied)|meets|guarantee[sd]?|admissible|tamper-proof|"
    r"validated\s+(?:system|module)|fips[-\s](?:validated|approved))\b", re.IGNORECASE)
MASK = [  # regime proper nouns and our own checker's verdict phrase: never a claim about SecuraCV's standing
    r"qualified\s+electronic\s+(?:ledgers?|time[- ]?stamps?|signatures?|seals?)",
    r"qualified\s+trust\s+service(?:\s+providers?)?s?", r"\bQTSPs?\b", r"qualified\s+certificates?",
    r"qualified\s+person",                 # FRE 902(11)/(13): the certification of a qualified person
    r"qualified\s+(?:TSAs?|time[- ]?stamp(?:ing)?\s+authorit(?:y|ies))",   # operator-declared, src/anchor_policy.rs:10-11
    r"(?:does\s+not\s+)?conforms?\s+to\s+securacv-c2pa-witness-profile:v\d+",  # item 6's checker verdict, brief-item6 §2.4
]
MASK_RE = [re.compile(p, re.IGNORECASE) for p in MASK]
MUST_PASS = ["a qualified electronic ledger under Art. 45k", "the draft certification for a qualified person to sign",
             "maps to 21 CFR 11.10(e)", "designed to support the operator's assessment", "the C2PA conformance program",
             "one TSA may be a qualified TSA the operator declares", "the compliance mapping suite",
             "verdict: conforms to securacv-c2pa-witness-profile:v1",
             "a qualified\nelectronic ledger"]                     # a Markdown wrap inside a masked noun
MUST_FAIL = ["SecuraCV is compliant", "this deployment meets 11.10", "a certified system", "satisfies Annex 11",
             "qualified by design", "a qualified ledger", "the ledger conforms to eIDAS", "admissible evidence",
             'SecuraCV is "compliant"', "SecuraCV is `compliant`", "uses FIPS-validated cryptography",
             "a qualified electronic ledger, and SecuraCV is qualified", "tamper-proof receipts",
             "SecuraCV is a validated\nsystem",                     # a Markdown wrap between two words
             "SecuraCV is compli\u00adant", "SecuraCV is compli\u200bant"]  # a soft hyphen, a zero-width space

# docs/compliance/README.md opens with this, verbatim after whitespace
# normalization (rule P1). Changing the posture is changing this string in
# the same commit as the README, in review.
POSTURE = """SecuraCV's record is its device-signed, hash-chained receipt ledgers, checked against a key the relying party pins. Everything in this directory is derived from that record or describes it, and adds no authority. That means:
- maps from a regime's clauses to the SecuraCV mechanisms that exist, each with a status from a closed vocabulary and evidence a reader can open;
- a methodology for checking those mechanisms without trusting SecuraCV;
- templates an operator fills;
- and, as they land, projections that reshape receipts into formats other systems read.

SecuraCV holds no certification, approval, registration or qualification under any regime named here, and nothing here states that a deployment is acceptable under one. That determination belongs to the regulated organization and its regulator, to an assessor, or to a court. The consumer kernel stays self-signed and pinned. Enterprise-tier projections are opt-in files an operator command writes and the operator ships, and each says that the receipts, not the projection, are the record."""

# ── Messages (after PREFIX); every one is pinned by a test ──────────────────
M_S0 = (f"{DICTIONARY} has no compliance_map_status block, or its ids are not "
        "structural, procedural, designed, not-applicable, not-pursued — the status set is read from there")
M_S1 = "{file}: not valid JSON ({error})"
M_S2 = '{file}: "schema" must be "securacv-compliance-map:v1"'
M_S3_UNKNOWN = '{file}: {trail}: unknown key "{key}" — the map schema is closed (docs/compliance/README.md#map-schema)'
M_S3_MISSING = '{file}: {trail}: missing key "{key}"'
M_S3_TEST_KIND = '{file}: {trail}: "test" is allowed only on evidence of kind "test"'
M_S3_TEST_AND_SYMBOL = '{file}: {trail}: at most one of "test" and "symbol"'
M_S4_TYPE = '{file}: {trail}: "{key}" must be a {type}'
M_S4_CAP = '{file}: {trail}: "{key}" exceeds {n} characters'
M_S4_CHAR = '{file}: {trail}: "{key}" contains a control or text-direction character'
M_S5_STEM = '{file}: regime id "{id}" must equal the file stem "{stem}"'
M_S5_REGIME_SHAPE = '{file}: regime id "{id}" does not match ^[a-z0-9_]{{2,40}}$'
M_S5_ROW = "{file}:{row}: row id does not match " + ROW_ID_PATTERN.replace("{", "{{").replace("}", "}}")
M_S5_DUP = "{file}:{row}: duplicate row id"
M_S6 = "{file}: last_reviewed must be a month, YYYY-MM — never a day or a time"
M_S7 = '{file}: {trail}: "{value}" is not one of {allowed}'
M_V1 = ('{file}:{row}: status "{s}" is not one of structural, procedural, designed, not-applicable, '
        "not-pursued (spec/witness_dictionary.json compliance_map_status)")
M_V2_TEST = ('{file}:{row}: "structural" needs a test evidence entry naming a test function — '
             'without one the row is "designed" or "procedural"')
M_V2_CODE = '{file}:{row}: "structural" needs a code evidence entry naming a symbol'
M_V3 = ('{file}:{row}: "procedural" needs a procedure evidence entry (a runbook, CPS, operator-pack or '
        "guide section) and an operator_action")
M_V4 = ('{file}:{row}: "designed" needs a design evidence entry (spec/, docs/design/, docs/security/ or a '
        "docs/compliance design note) and depends_on")
M_V5 = '{file}:{row}: "not-applicable" needs a reason of at least 40 characters'
M_V6 = ('{file}:{row}: "not-pursued" needs a reason of at least 40 characters and a decision evidence '
        "entry naming where the decision is recorded")
M_V7 = "{file}:{row}: a tracker entry must point at docs/security/ENTERPRISE_CUSTODY.md#<anchor>"
M_E0_BARE_TEST = ('{file}:{row}: a test evidence entry must name its test function ("test") — '
                  "a bare test file proves nothing")
M_E1 = "{file}:{row}: evidence path {p} escapes the repository"
M_E2 = "{file}:{row}: evidence {p} does not exist — a map may only point at what is on disk"
M_E3 = ('{file}:{row}: {p} no longer contains "{symbol}" — the mechanism the row names moved or '
        "regressed; fix the row, or restore the mechanism")
M_E4 = ('{file}:{row}: "{t}" is not a test function in {p} — a structural row may only name a test '
        "that exists; downgrade the row, never invent a test")
M_E5 = '{file}:{row}: anchor "#{a}" is not a heading in {p}'
M_E6 = '{file}:{row}: evidence of kind "{k}" cannot live at {p}'
M_D1 = '{file}:{row}: depends_on "{d}" is not a roadmap PR id this linter knows'
M_C1 = ('{file}:{row}: the regime source was not fetched, so its clause cannot be '
        '"clause_verified": true')
M_C2 = ("{file}:{row}: a BS 10008 clause number appears, but the standard was not read — "
        "cite the topic only")
M_W1 = ('{where}: "{word}" reads as a claim about SecuraCV\'s standing — write "maps to", '
        '"designed to support" or "shaped as"; the determination belongs to {determination}')
M_W2 = ("the claim-word patterns no longer behave — these MUST_PASS/MUST_FAIL samples changed "
        "verdict: {list}")
M_N1 = "{file}: {trail}: a validation result belongs in the cited report, not in the tree (AGENTS.md rule 4)"
M_P1 = ("docs/compliance/README.md must open with the posture paragraph verbatim "
        "(pinned in scripts/lint_compliance_map.py)")
M_OK = ("{m} map(s), {r} row(s), {f} prose file(s): {a} structural, {b} procedural, {c} designed, "
        "{d} not-applicable, {e} not-pursued; no claim words")


class Finding:
    """One message with the rule that raised it (the generator refuses on S/V/E/D/C)."""

    __slots__ = ("rule", "message")

    def __init__(self, rule: str, message: str):
        self.rule, self.message = rule, message

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"Finding({self.rule!r}, {self.message!r})"


class ParseError(Exception):
    """A file that cannot be parsed (exit 2)."""


# ── Claim words ─────────────────────────────────────────────────────────────
def visible(text: str) -> str:
    """Drop format characters (Unicode Cf: soft hyphen, zero-width space and
    joiners, word joiner, BOM, the text-direction marks). They render as
    nothing but split a word, so a claim word with one inside is still the
    word. Newlines are never Cf, so line numbers survive."""
    return "".join(ch for ch in text if unicodedata.category(ch) != "Cf")


def forbidden_char(ch: str) -> bool:
    """A control or text-direction character (S4): Cc, Cf (which holds the
    bidi embeddings and isolates, the marks and the zero-width characters)."""
    return unicodedata.category(ch) in ("Cc", "Cf") or ch in BIDI


def masked(text: str) -> str:
    """Format characters dropped, then each MASK span blanked to spaces of the
    same length, so an offset into the result is an offset into visible(text)."""
    text = visible(text)
    for rx in MASK_RE:
        text = rx.sub(lambda m: " " * len(m.group(0)), text)
    return text


def claim_hits(text: str) -> list[tuple[int, str]]:
    """(offset into visible(text), word) for each claim; the whitespace inside
    a multi-word hit (a wrapped line) is normalized to one space."""
    return [(m.start(), " ".join(m.group(0).split())) for m in CLAIM.finditer(masked(text))]


def claim_words(text: str) -> list[str]:
    return [w for _, w in claim_hits(text)]


def self_test() -> list[str]:
    """The samples whose verdict changed; empty when the patterns behave."""
    return ([s for s in MUST_PASS if claim_words(s)]
            + [s for s in MUST_FAIL if not claim_words(s)])


def normalized(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


# ── Status set ──────────────────────────────────────────────────────────────
def read_statuses(root: Path) -> tuple[list[str] | None, list[Finding]]:
    try:
        data = json.loads((root / DICTIONARY).read_text(encoding="utf-8"))
        ids = [item["id"] for item in data["compliance_map_status"]["items"]]
    except (OSError, ValueError, KeyError, TypeError):
        return None, [Finding("S0", M_S0)]
    if ids != list(STATUSES):
        return None, [Finding("S0", M_S0)]
    return ids, []


# ── Evidence helpers ────────────────────────────────────────────────────────
COMMENT_PREFIXES = {
    ".rs": ("//", "/*", "*"), ".js": ("//", "/*", "*"), ".mjs": ("//", "/*", "*"),
    ".ts": ("//", "/*", "*"), ".c": ("//", "/*", "*"), ".h": ("//", "/*", "*"),
    ".cpp": ("//", "/*", "*"), ".hpp": ("//", "/*", "*"), ".ino": ("//", "/*", "*"),
    ".swift": ("//", "/*", "*"), ".py": ("#",), ".sh": ("#",), ".toml": ("#",),
    ".yml": ("#",), ".yaml": ("#",), ".md": ("<!--",),
}


def symbol_pattern(symbol: str) -> re.Pattern:
    left = r"(?<![A-Za-z0-9_])" if re.match(r"\w", symbol[:1]) else ""
    right = r"(?![A-Za-z0-9_])" if re.match(r"\w", symbol[-1:]) else ""
    return re.compile(left + re.escape(symbol) + right)


def has_symbol(path: Path, symbol: str) -> bool:
    if not named(symbol):
        return False
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return False
    rx = symbol_pattern(symbol)
    prefixes = COMMENT_PREFIXES.get(path.suffix, ())
    for line in lines:
        if prefixes and line.lstrip().startswith(prefixes):
            continue
        if rx.search(line):
            return True
    return False


RUST_TEST_ATTR = re.compile(r"^#\[(?:test|tokio::test)\b")


def has_test(path: Path, rel: str, name: str) -> bool:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return False
    if path.suffix == ".rs":
        fn = re.compile(r"\bfn\s+" + re.escape(name) + r"\s*[(<]")
        for i, line in enumerate(lines):
            if line.lstrip().startswith("//") or not fn.search(line):
                continue
            # The three non-blank lines above, but only the run of attributes
            # and comments that belongs to this fn: an earlier item's
            # #[test] must not lend itself to a helper declared after it.
            above = [l.strip() for l in lines[:i] if l.strip()][-3:]
            own = []
            for l in reversed(above):
                if not l.startswith(("#[", "//")):
                    break
                own.append(l)
            if any(RUST_TEST_ATTR.match(l) for l in own):
                return True
        return False
    if path.suffix == ".py" and path.name.startswith("test_"):
        rx = re.compile(r"^\s*(?:async\s+)?def\s+" + re.escape(name) + r"\s*\(")
        return any(rx.search(l) for l in lines)
    if path.name.endswith(".test.js"):
        rx = re.compile(r"\b(test|it|describe)\(\s*['\"]" + re.escape(name) + r"['\"]")
        return any(rx.search(l) for l in lines)
    parts = rel.split("/")
    if parts[0] == "firmware" and "tests_host" in parts[:-1] and path.suffix in (".cpp", ".cc", ".c"):
        rx = re.compile(r"\bvoid\s+" + re.escape(name) + r"\s*\(")
        return any(rx.search(l) for l in lines)
    return False


def design_path_ok(p: str) -> bool:
    if p.startswith(("spec/", "docs/design/", "docs/security/")):
        return True
    parts = p.split("/")
    return len(parts) == 3 and parts[:2] == ["docs", "compliance"] and p.endswith(".md")


# ── One map ─────────────────────────────────────────────────────────────────
TYPE_NAMES = {str: "string", bool: "boolean", list: "list", dict: "JSON object"}


def walk_strings(value, trail: str):
    """Yield (trail, key, string) for every string value under value."""
    if isinstance(value, str):
        key = trail.rsplit(".", 1)[-1] if "." in trail else trail
        yield trail, re.sub(r"\[\d+\]$", "", key), value
    elif isinstance(value, dict):
        for k, v in value.items():
            yield from walk_strings(v, f"{trail}.{k}" if trail else k)
    elif isinstance(value, list):
        for i, v in enumerate(value):
            yield from walk_strings(v, f"{trail}[{i}]")


def lint_map(path: Path, root: Path, statuses: list[str] | None) -> tuple[dict | None, list[Finding]]:
    """Lint one map. Returns (the parsed map or None, findings). Raises ParseError."""
    file = path.relative_to(root).as_posix()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise ParseError(PREFIX + M_S1.format(file=file, error=str(e).splitlines()[0])) from None
    out: list[Finding] = []

    def add(rule, msg, **kw):
        out.append(Finding(rule, msg.format(file=file, **kw)))

    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        add("S2", M_S2)
        if not isinstance(data, dict):
            return None, out

    def closed(obj, trail, required, optional=()):
        for k in obj:
            if k not in required and k not in optional:
                add("S3", M_S3_UNKNOWN, trail=trail, key=k)
        for k in required:
            if k not in obj:
                add("S3", M_S3_MISSING, trail=trail, key=k)

    def typed(obj, trail, key, typ):
        if key in obj and not isinstance(obj[key], typ):
            add("S4", M_S4_TYPE, trail=trail, key=key, type=TYPE_NAMES[typ])
            return False
        return key in obj

    def list_of_str(obj, trail, key):
        if key not in obj:
            return False
        v = obj[key]
        if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
            add("S4", M_S4_TYPE, trail=trail, key=key, type="list of strings")
            return False
        return True

    closed(data, "top level", TOP_REQUIRED, TOP_OPTIONAL)
    typed(data, "top level", "schema", str)
    if list_of_str(data, "top level", "preamble"):
        for i, para in enumerate(data["preamble"]):
            if len(para) > PREAMBLE_CAP:
                add("S4", M_S4_CAP, trail=f"preamble[{i}]", key="preamble", n=PREAMBLE_CAP)
    if typed(data, "top level", "last_reviewed", str) and not MONTH_RE.match(data["last_reviewed"]):
        add("S6", M_S6)

    regime = data.get("regime")
    access = None
    regime_id = None
    if "regime" in data and not isinstance(regime, dict):
        add("S4", M_S4_TYPE, trail="top level", key="regime", type="JSON object")
        regime = None
    if isinstance(regime, dict):
        closed(regime, "regime", REGIME_REQUIRED)
        for k in ("id", "title", "kind", "determination_by"):
            typed(regime, "regime", k, str)
        if isinstance(regime.get("title"), str) and len(regime["title"]) > CAPS["title"]:
            add("S4", M_S4_CAP, trail="regime", key="title", n=CAPS["title"])
        if isinstance(regime.get("id"), str):
            regime_id = regime["id"]
            if regime_id != path.stem:
                add("S5", M_S5_STEM, id=regime_id, stem=path.stem)
            elif not REGIME_ID_RE.match(regime_id):
                add("S5", M_S5_REGIME_SHAPE, id=regime_id)
        if isinstance(regime.get("kind"), str) and regime["kind"] not in REGIME_KINDS:
            add("S7", M_S7, trail="regime.kind", value=regime["kind"], allowed=", ".join(sorted(REGIME_KINDS)))
        source = regime.get("source")
        if "source" in regime and not isinstance(source, dict):
            add("S4", M_S4_TYPE, trail="regime", key="source", type="JSON object")
        elif isinstance(source, dict):
            closed(source, "regime.source", SOURCE_REQUIRED)
            for k in SOURCE_REQUIRED:
                typed(source, "regime.source", k, str)
            if isinstance(source.get("access"), str):
                access = source["access"]
                if access not in SOURCE_ACCESS:
                    add("S7", M_S7, trail="regime.source.access", value=access,
                        allowed=", ".join(sorted(SOURCE_ACCESS)))

    # Strings everywhere: control and text-direction characters; N1 results.
    for trail, key, s in walk_strings(data, ""):
        if any(forbidden_char(ch) for ch in s):
            obj_trail = trail.rsplit(".", 1)[0] if "." in trail else "top level"
            add("S4", M_S4_CHAR, trail=obj_trail, key=key)
        if RESULT_RE.search(s):
            add("N1", M_N1, trail=trail)

    rows = data.get("rows")
    if "rows" in data and not isinstance(rows, list):
        add("S4", M_S4_TYPE, trail="top level", key="rows", type="list")
        rows = None
    seen_ids: set[str] = set()
    for i, row in enumerate(rows or []):
        trail = f"rows[{i}]"
        if not isinstance(row, dict):
            add("S4", M_S4_TYPE, trail="rows", key=trail, type="JSON object")
            continue
        rid = row["id"] if isinstance(row.get("id"), str) else trail

        def radd(rule, msg, **kw):
            add(rule, msg, row=rid, **kw)

        closed(row, trail, ROW_REQUIRED, ROW_OPTIONAL)
        for k in ("id", "clause", "requirement", "mechanism", "status",
                  "operator_action", "limits", "reason"):
            typed(row, trail, k, str)
        typed(row, trail, "clause_verified", bool)
        for k, cap in CAPS.items():
            if k != "title" and isinstance(row.get(k), str) and len(row[k]) > cap:
                add("S4", M_S4_CAP, trail=trail, key=k, n=cap)
        if isinstance(row.get("id"), str):
            if not ROW_ID_RE.match(row["id"]):
                radd("S5", M_S5_ROW)
            if row["id"] in seen_ids:
                radd("S5", M_S5_DUP)
            seen_ids.add(row["id"])
        if list_of_str(row, trail, "alcoa"):
            for v in row["alcoa"]:
                if v not in ALCOA:
                    add("S7", M_S7, trail=f"{trail}.alcoa", value=v, allowed=", ".join(sorted(ALCOA)))
        depends = row["depends_on"] if list_of_str(row, trail, "depends_on") else []
        for d in depends:
            if d not in DEPENDS_ON:
                radd("D1", M_D1, d=d)

        if row.get("clause_verified") is True and access != "fetched":
            radd("C1", M_C1)
        if regime_id == "bs10008" and isinstance(row.get("clause"), str) \
                and CLAUSE_NUMBER_RE.search(row["clause"]):
            radd("C2", M_C2)

        evidence = row.get("evidence")
        if "evidence" in row and not isinstance(evidence, list):
            add("S4", M_S4_TYPE, trail=trail, key="evidence", type="list")
            evidence = []
        kinds: dict[str, list[dict]] = {}
        for j, ev in enumerate(evidence or []):
            etrail = f"{trail}.evidence[{j}]"
            if not isinstance(ev, dict):
                add("S4", M_S4_TYPE, trail=f"{trail}.evidence", key=f"evidence[{j}]", type="JSON object")
                continue
            closed(ev, etrail, EVIDENCE_REQUIRED, EVIDENCE_OPTIONAL)
            for k in EVIDENCE_REQUIRED + EVIDENCE_OPTIONAL:
                typed(ev, etrail, k, str)
            for k in EVIDENCE_OPTIONAL:  # a blank name names nothing, and "" matches every line
                if isinstance(ev.get(k), str) and not ev[k].strip():
                    add("S4", M_S4_TYPE, trail=etrail, key=k, type="non-empty string")
            kind, p = ev.get("kind"), ev.get("path")
            if not isinstance(kind, str) or not isinstance(p, str):
                continue
            if kind not in EVIDENCE_KINDS:
                add("S7", M_S7, trail=f"{etrail}.kind", value=kind, allowed=", ".join(sorted(EVIDENCE_KINDS)))
                continue
            if "test" in ev and "symbol" in ev:
                add("S3", M_S3_TEST_AND_SYMBOL, trail=etrail)
            if "test" in ev and kind != "test":
                add("S3", M_S3_TEST_KIND, trail=etrail)
            if kind == "test" and "test" not in ev:
                radd("E0", M_E0_BARE_TEST)
            kinds.setdefault(kind, []).append(ev)
            check_evidence(ev, kind, p, root, radd)

        status = row.get("status")
        if not isinstance(status, str):
            continue
        valid = statuses if statuses is not None else list(STATUSES)
        if status not in valid:
            radd("V1", M_V1, s=status)
            continue
        if status == "structural":
            if not any(named(e.get("test")) for e in kinds.get("test", [])):
                radd("V2", M_V2_TEST)
            if not any(named(e.get("symbol")) for e in kinds.get("code", [])):
                radd("V2", M_V2_CODE)
        elif status == "procedural":
            if not kinds.get("procedure") or not str(row.get("operator_action", "")).strip():
                radd("V3", M_V3)
        elif status == "designed":
            if not kinds.get("design") or not depends:
                radd("V4", M_V4)
        elif status == "not-applicable":
            if len(str(row.get("reason", "")).strip()) < MIN_REASON:
                radd("V5", M_V5)
        elif status == "not-pursued":
            if len(str(row.get("reason", "")).strip()) < MIN_REASON or not kinds.get("decision"):
                radd("V6", M_V6)

    return data, out


def named(value) -> bool:
    """A test or symbol name that names something: a string with a word character."""
    return isinstance(value, str) and re.search(r"\w", value) is not None


def check_evidence(ev: dict, kind: str, p: str, root: Path, radd) -> None:
    if kind == "tracker" and (p != ENTERPRISE_CUSTODY or not ev.get("anchor")):
        radd("V7", M_V7)
    parts = p.replace("\\", "/").split("/")
    if p.startswith(("/", "\\")) or re.match(r"^[A-Za-z]:", p) or ".." in parts or p == "":
        radd("E1", M_E1, p=p)
        return
    target = root / p
    try:
        resolved = target.resolve()
    except OSError:
        resolved = target
    if not resolved.is_relative_to(root.resolve()):
        radd("E1", M_E1, p=p)
        return
    if not target.exists():
        radd("E2", M_E2, p=p)
        return
    if kind == "design" and not design_path_ok(p):
        radd("E6", M_E6, k=kind, p=p)
    if kind == "procedure" and not p.startswith("docs/"):
        radd("E6", M_E6, k=kind, p=p)
    # A blank name is already an S4 finding; a non-blank one with no word
    # character (a lone "(") matches nearly every line, so E3 refuses it.
    if isinstance(ev.get("symbol"), str) and ev["symbol"].strip() and not has_symbol(target, ev["symbol"]):
        radd("E3", M_E3, p=p, symbol=ev["symbol"])
    if kind == "test" and isinstance(ev.get("test"), str) and ev["test"].strip() \
            and not has_test(target, p, ev["test"]):
        radd("E4", M_E4, t=ev["test"], p=p)
    if isinstance(ev.get("anchor"), str) and ev["anchor"].strip() and ev["anchor"] not in anchors_of(target):
        radd("E5", M_E5, a=ev["anchor"], p=p)


# ── Prose ───────────────────────────────────────────────────────────────────
def prose_files(root: Path) -> list[Path]:
    base = root / PROSE_DIR
    if not base.is_dir():
        return []
    files = {f for f in base.rglob("*.md") if f.is_file()}
    templates = base / "templates"
    if templates.is_dir():
        files |= {f for f in templates.iterdir() if f.is_file()}
    return sorted(files)


def lint_prose(path: Path, root: Path) -> list[Finding]:
    file = path.relative_to(root).as_posix()
    out = []
    # The whole file at once, so a claim (or a masked regime noun) that a
    # Markdown wrap splits across two lines is still seen; each hit is
    # reported at the line it starts on.
    text = visible(path.read_text(encoding="utf-8", errors="replace"))
    for offset, word in claim_hits(text):
        line = text.count("\n", 0, offset) + 1
        out.append(Finding("W1", M_W1.format(where=f"{file}:{line}", word=word,
                                             determination=PROSE_DETERMINATION)))
    return out


def lint_map_words(file: str, data: dict) -> list[Finding]:
    determination = PROSE_DETERMINATION
    regime = data.get("regime")
    if isinstance(regime, dict) and isinstance(regime.get("determination_by"), str):
        determination = regime["determination_by"]
    out = []
    for trail, _key, s in walk_strings(data, ""):
        for word in claim_words(s):
            out.append(Finding("W1", M_W1.format(where=f"{file}: {trail}", word=word,
                                                 determination=determination)))
    return out


def check_posture(root: Path) -> list[Finding]:
    readme = root / README
    try:
        text = readme.read_text(encoding="utf-8")
    except OSError:
        return [Finding("P1", M_P1)]
    lines = text.splitlines()
    while lines and not lines[0].strip():
        lines.pop(0)
    if lines and lines[0].startswith("# "):
        lines.pop(0)
    if not normalized("\n".join(lines)).startswith(normalized(POSTURE)):
        return [Finding("P1", M_P1)]
    return []


# ── The run ─────────────────────────────────────────────────────────────────
class Result:
    def __init__(self):
        self.findings: list[Finding] = []
        self.parse_errors: list[str] = []
        self.maps: dict[str, dict] = {}
        self.map_findings: dict[str, list[Finding]] = {}
        self.prose: list[str] = []


def run(root: Path) -> Result:
    res = Result()
    broken = self_test()
    if broken:
        res.findings.append(Finding("W2", M_W2.format(list="; ".join(repr(s) for s in broken))))
    statuses, s0 = read_statuses(root)
    res.findings.extend(s0)
    maps_dir = root / MAPS_DIR
    for path in sorted(maps_dir.glob("*.json")) if maps_dir.is_dir() else []:
        file = path.relative_to(root).as_posix()
        try:
            data, found = lint_map(path, root, statuses)
        except ParseError as e:
            res.parse_errors.append(str(e))
            continue
        if data is not None:
            found = found + lint_map_words(file, data)
            res.maps[path.stem] = data
        res.map_findings[path.stem] = found
        res.findings.extend(found)
    for path in prose_files(root):
        res.prose.append(path.relative_to(root).as_posix())
        res.findings.extend(lint_prose(path, root))
    res.findings.extend(check_posture(root))
    return res


def ok_line(res: Result) -> str:
    counts = {s: 0 for s in STATUSES}
    nrows = 0
    for data in res.maps.values():
        for row in data.get("rows") or []:
            if isinstance(row, dict):
                nrows += 1
                if row.get("status") in counts:
                    counts[row["status"]] += 1
    return PREFIX + M_OK.format(m=len(res.maps), r=nrows, f=len(res.prose),
                                a=counts["structural"], b=counts["procedural"], c=counts["designed"],
                                d=counts["not-applicable"], e=counts["not-pursued"])


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=REPO, help="the repository root (tests use a temp tree)")
    args = ap.parse_args(argv)
    root = args.root.resolve()
    res = run(root)
    for line in res.parse_errors:
        print(line, file=sys.stderr)
    for f in res.findings:
        print(PREFIX + f.message, file=sys.stderr)
    if res.parse_errors:
        return 2
    if res.findings:
        return 1
    print(ok_line(res))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
