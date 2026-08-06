#!/usr/bin/env python3
"""CloudKit container gate — no `CKContainer.default()`, one identifier, one schema list.

TWO RULES, ONE CRASH BEHIND BOTH
--------------------------------
`CKContainer.default()` reads the app's iCloud-container entitlement to decide
which container it means. With no such entitlement it raises an Objective-C
`CKException` ("containerIdentifier can not be nil") rather than returning nil
or throwing a Swift error — and Swift cannot catch an Objective-C exception, so
the process aborts. An unsigned build has no entitlements, CI builds exactly
that (`CODE_SIGNING_ALLOWED=NO`), and the app died on launch before a single
test could connect. Every iOS test failed and not one of them was about iCloud.

So:

  1. NO `CKContainer.default()` ANYWHERE. Call sites use
     `CloudContainer.shared`, which names the container explicitly and
     therefore never takes the nil path. See ios/Sources/SecuraCV/Cloud/
     CloudContainer.swift for the full account.

  2. THE IDENTIFIER IS TYPED IN THREE PLACES and they must agree — the Swift
     constant plus both entitlements files (release and dev). Rule 1 is only
     worth anything if the name it hardcodes is the name the app is actually
     signed for; a drifted string would swap one silent no-iCloud for another,
     with no crash to point at it.

Rule 2 is the reason this is a linter and not a code review note: "two numbers
that must agree, typed twice" is the failure this repo keeps meeting, and the
fix that works is a gate rather than a promise.

A THIRD RULE, THE SAME SHAPE, A DIFFERENT SILENCE
-------------------------------------------------
  3. EVERY CKRECORD TYPE **AND FIELD** THE APP WRITES IS LISTED IN
     ios/scripts/cloudkit_schema.sh. CloudKit invents record types and fields
     on first write in the DEVELOPMENT environment and refuses to in
     production, so anything nobody deployed makes every save against that type
     fail — and both CloudKit call sites in this app swallow their errors on
     purpose, because a failed sync must never stall the local alert already
     reaching the person in the room. The result is a feature that is simply
     dead in the shipped app with nothing anywhere to point at.

     `cloudkit_schema.sh` is the tool that promotes the schema and answers
     "did it ship?" — and it can only check what it has been told about. Its
     first version knew only `WitnessWake`, while `CloudSync` had been writing
     and querying `PairedDevice` since the day it was written. Nothing caught
     that, because nothing was looking. This rule looks.

     FIELDS ARE NOT A LESSER CASE OF THE SAME BUG — they are the nastier one.
     A missing type at least fails only its own feature. A single undeclared
     field makes production reject the WHOLE save, so adding one field to a
     shipped record type kills every write of that type, including the ones
     that worked yesterday. Checking type names alone would wave that through,
     because the type was already listed.

Run: python3 scripts/lint_cloudkit_container.py   (exit 0 = clean, 1 = drift)
"""

from __future__ import annotations

import plistlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# The declaration this gate treats as the source of truth.
SWIFT_SOURCE = "ios/Sources/SecuraCV/Cloud/CloudContainer.swift"
IDENT_RE = re.compile(r'static\s+let\s+identifier\s*=\s*"([^"]+)"')

# Every entitlements file that claims a CloudKit container. Both app variants
# are listed rather than globbed: a NEW entitlements file that grows a
# container should fail this gate until someone adds it here deliberately.
ENTITLEMENTS = [
    "ios/Support/SecuraCV.entitlements",
    "ios/Support/SecuraCV.dev.entitlements",
    # The Apple TV standing watch for the household: it PUBLISHES away wakes
    # into the same private database the phone reads (ResidentWatch), so it
    # claims the same container and is checked by the same gate.
    "tvos/WitnessWall/Support/WitnessWall.entitlements",
]
ENTITLEMENT_KEY = "com.apple.developer.icloud-container-identifiers"

# Where a `.default()` call would be a crash waiting for an unsigned build.
SWIFT_ROOTS = ["ios", "tvos"]
BANNED_CALL = re.compile(r"\bCKContainer\s*\.\s*default\s*\(")

# The schema tool, and the table inside it that lists what production needs.
SCHEMA_SCRIPT = "ios/scripts/cloudkit_schema.sh"

# Rows of that table look like `WitnessWake|sev|-|createdTimestamp` and are the
# only lines in the file that begin with a bare identifier followed by a pipe.
# Matching the shape rather than parsing the heredoc keeps this gate working if
# the table grows a trailing column, which is the likelier edit; the two columns
# read here (type, fields) are the leading ones and stay put.
SCHEMA_ROW_RE = re.compile(r"^([A-Za-z][A-Za-z0-9_]*)\|([^|\n]*)", re.MULTILINE)

# How a record type's name reaches CloudKit from Swift. Two spellings, because
# the app uses both and a gate that knows only one is a gate with a hole:
#   * inline   — CKRecord(recordType: "PairedDevice", …), CKQuery(recordType: …)
#   * by name  — static let wakeRecordType = "WitnessWake", passed to
#                CKQuerySubscription and CKRecord as a constant
# The second pattern keys on the identifier containing "RecordType", which is
# the naming this repo already uses and the one a reviewer would expect. A
# constant named something else would slip past — so if you add one, name it
# for what it is.
RECORD_TYPE_INLINE = re.compile(r'\brecordType\s*:\s*"([A-Za-z_][A-Za-z0-9_]*)"')
RECORD_TYPE_CONST = re.compile(
    r'\blet\s+\w*[Rr]ecordType\w*\s*(?::\s*String\s*)?=\s*"([A-Za-z_][A-Za-z0-9_]*)"'
)

# FIELDS, which are the other half of the same silent failure. Production
# rejects a save carrying a field the schema doesn't define, so a NEW field on
# an EXISTING record type kills every write to that type — and checking only
# type names would wave it through, because the type was already listed.
#
# Attribution is the hard part: `record["name"] = …` says nothing about which
# record type `record` is. So rather than lex Swift (see `code_of` for why this
# file refuses to), the scan tracks the one shape the app actually writes:
#
#     let record = CKRecord(recordType: <literal or constant>)   -> binds a name
#     record[<literal or constant>] = …                          -> a field of it
#
# Bindings are file-scoped, which is why two files can both call their variable
# `record` without confusing the gate.
#
# Known limit, stated rather than papered over: a record obtained from a fetch
# (`try res.get()`) is not a binding this sees, so fields only ever READ are not
# checked. That is the harmless direction — reading an undefined field returns
# nil, while writing one fails the save.
CK_RECORD_BINDING = re.compile(
    r"\b(?:let|var)\s+(\w+)\s*=\s*CKRecord\s*\(\s*recordType\s*:\s*([^,)]+)"
)
SUBSCRIPT_ASSIGN = re.compile(r"\b(\w+)\s*\[\s*([^\]]+?)\s*\]\s*=(?!=)")

# Any `let NAME = "value"`, so a record type or field key written as a constant
# (`Self.wakeRecordType`, `WakePayload.classKey`) can be resolved to its string.
STRING_CONST = re.compile(r'\b(?:static\s+)?let\s+(\w+)\s*(?::\s*String\s*)?=\s*"([^"]*)"')
STRING_LITERAL = re.compile(r'^"([^"]*)"$')

errors: list[str] = []


def read(rel: str) -> str | None:
    p = ROOT / rel
    if not p.is_file():
        errors.append(f"[missing] {rel} — this gate expected it; did it move?")
        return None
    return p.read_text(encoding="utf-8")


def declared_identifier() -> str | None:
    text = read(SWIFT_SOURCE)
    if text is None:
        return None
    m = IDENT_RE.search(text)
    if not m:
        errors.append(
            f"[unparsable] {SWIFT_SOURCE} no longer declares "
            '`static let identifier = "..."`. A drift gate that cannot find '
            "what it checks is worse than no gate — fix the pattern here, do "
            "not delete the check."
        )
        return None
    return m.group(1)


def check_entitlements(want: str) -> None:
    for rel in ENTITLEMENTS:
        p = ROOT / rel
        if not p.is_file():
            errors.append(f"[missing] {rel} — this gate expected it; did it move?")
            continue
        try:
            with p.open("rb") as fh:
                plist = plistlib.load(fh)
        except Exception as exc:  # malformed plist is a real failure, not a skip
            errors.append(f"[unreadable] {rel}: {exc}")
            continue
        got = plist.get(ENTITLEMENT_KEY)
        if not got:
            errors.append(
                f"[no container] {rel} declares no {ENTITLEMENT_KEY}. If the "
                "app genuinely dropped CloudKit, remove it from this gate's "
                "ENTITLEMENTS list in the same change."
            )
            continue
        if want not in got:
            errors.append(
                f"[drift] {rel} declares {got}, but {SWIFT_SOURCE} names "
                f'"{want}". The app would be signed for one container and ask '
                "for another — iCloud fails at runtime with nothing to point at."
            )


BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT = re.compile(r"//[^\n]*")


def _blank(m: re.Match[str]) -> str:
    """Replace a comment with spaces, keeping its newlines.

    Blanking rather than deleting is what lets the scan run over the whole file
    and still report a true line number: every byte outside a newline keeps its
    position.
    """
    return "".join("\n" if c == "\n" else " " for c in m.group(0))


def code_of(text: str) -> str:
    """A whole Swift source with its comments blanked out, offsets intact.

    WHY COMMENTS GO, AND WHY THE FILE STAYS
      The rule has to be explainable somewhere, and the place it is explained —
      CloudContainer.swift's header — necessarily writes the banned call out in
      prose. Blanket-skipping that file would blind the gate to a real call
      site in the one file most likely to grow one. Blanking comments keeps
      every file's actual code checked.

    WHY THE WHOLE FILE AND NOT LINE BY LINE
      `CKContainer\\n    .default()` is ordinary Swift — a chained call split
      across lines, which is what a formatter produces the moment the
      expression gets long. Matching per line never sees both halves, so the
      ban would have quietly stopped covering the exact formatting a real call
      site is most likely to arrive in.

    Deliberately not a Swift lexer: a `//` or `/*` inside a string literal is
    treated as a comment. The residual risk is a FALSE POSITIVE (a string that
    looks like a comment hides code from the scan, or contains the banned call
    verbatim), which fails loudly and is fixed by looking. That is the right
    direction for a ban to err; the alternative is maintaining a Swift lexer
    here, which is a much larger thing to keep correct than this gate is worth.
    """
    return LINE_COMMENT.sub(_blank, BLOCK_COMMENT.sub(_blank, text))


def check_no_default_calls() -> None:
    for root in SWIFT_ROOTS:
        base = ROOT / root
        if not base.is_dir():
            continue
        for f in sorted(base.rglob("*.swift")):
            blob = code_of(f.read_text(encoding="utf-8"))
            for m in BANNED_CALL.finditer(blob):
                line_no = blob.count("\n", 0, m.start()) + 1
                errors.append(
                    f"[banned] {f.relative_to(ROOT)}:{line_no}: CKContainer.default() "
                    "raises an uncatchable ObjC exception in an unsigned "
                    "build. Use CloudContainer.shared."
                )


def record_types_in_swift() -> dict[str, str]:
    """Every CloudKit record type the app names, mapped to where it says it.

    Comments are blanked first (`code_of`), so the prose in AwayPush.swift and
    in this repo's docs-in-headers style can describe a record type without
    the gate mistaking the description for a call site.
    """
    found: dict[str, str] = {}
    for root in SWIFT_ROOTS:
        base = ROOT / root
        if not base.is_dir():
            continue
        for f in sorted(base.rglob("*.swift")):
            blob = code_of(f.read_text(encoding="utf-8"))
            for pattern in (RECORD_TYPE_INLINE, RECORD_TYPE_CONST):
                for m in pattern.finditer(blob):
                    line_no = blob.count("\n", 0, m.start()) + 1
                    found.setdefault(
                        m.group(1), f"{f.relative_to(ROOT)}:{line_no}"
                    )
    return found


def string_constants() -> dict[str, set[str]]:
    """Every `let NAME = "value"` in the Swift sources, NAME -> {values}.

    A set rather than a single value on purpose: two files may each define a
    constant with the same bare name, and resolving `Foo.classKey` by its last
    component would then be a coin flip. Collisions are reported at the point of
    use (below) instead of guessed at, because a wrong guess here invents a
    field name and fails the build for a reason that isn't true.
    """
    out: dict[str, set[str]] = {}
    for root in SWIFT_ROOTS:
        base = ROOT / root
        if not base.is_dir():
            continue
        for f in sorted(base.rglob("*.swift")):
            blob = code_of(f.read_text(encoding="utf-8"))
            for name, value in STRING_CONST.findall(blob):
                out.setdefault(name, set()).add(value)
    return out


def resolve(expr: str, consts: dict[str, set[str]], where: str, what: str) -> str | None:
    """A Swift expression in record-type or field-key position -> its string.

    Handles the two spellings the app uses: a literal, or a reference to a
    string constant (`Self.wakeRecordType`, `WakePayload.classKey`), matched on
    the last dotted component. Anything else returns None and is reported by the
    caller — a gate that silently ignores what it cannot read is a gate with a
    hole in exactly the shape of the next mistake.
    """
    expr = expr.strip()
    literal = STRING_LITERAL.match(expr)
    if literal:
        return literal.group(1)

    name = expr.split(".")[-1].strip()
    values = consts.get(name)
    if not values:
        errors.append(
            f"[unresolvable] {where}: cannot tell what {what} `{expr}` is. This "
            "gate resolves a string literal or a `let NAME = \"…\"` constant "
            "referenced by name. Use one of those, or teach this linter the new "
            "spelling — do not leave it unreadable, because unreadable reads as "
            "'nothing to check'."
        )
        return None
    if len(values) > 1:
        errors.append(
            f"[ambiguous] {where}: {what} `{expr}` resolves by its last "
            f"component to more than one value ({', '.join(sorted(values))}). "
            "Rename one of the constants so the gate does not have to guess."
        )
        return None
    return next(iter(values))


def record_fields_in_swift(consts: dict[str, set[str]]) -> dict[str, dict[str, str]]:
    """Fields written into each CKRecord, as {record type: {field: where}}."""
    out: dict[str, dict[str, str]] = {}
    for root in SWIFT_ROOTS:
        base = ROOT / root
        if not base.is_dir():
            continue
        for f in sorted(base.rglob("*.swift")):
            rel = f.relative_to(ROOT)
            blob = code_of(f.read_text(encoding="utf-8"))

            # Which local names hold a CKRecord, and of what type. File-scoped.
            bound: dict[str, str] = {}
            for m in CK_RECORD_BINDING.finditer(blob):
                ident, type_expr = m.group(1), m.group(2)
                line_no = blob.count("\n", 0, m.start()) + 1
                rtype = resolve(type_expr, consts, f"{rel}:{line_no}", "record type")
                if rtype is None:
                    continue
                if bound.get(ident, rtype) != rtype:
                    errors.append(
                        f"[rebound] {rel}:{line_no}: `{ident}` holds a "
                        f"{bound[ident]} record earlier in this file and a "
                        f"{rtype} one here, so field writes cannot be attributed "
                        "to either. Give them different names."
                    )
                    continue
                bound[ident] = rtype

            for m in SUBSCRIPT_ASSIGN.finditer(blob):
                ident, key_expr = m.group(1), m.group(2)
                if ident not in bound:
                    continue  # an ordinary dictionary, not one of our records
                line_no = blob.count("\n", 0, m.start()) + 1
                field = resolve(key_expr, consts, f"{rel}:{line_no}", "field key")
                if field is None:
                    continue
                out.setdefault(bound[ident], {}).setdefault(field, f"{rel}:{line_no}")
    return out


def check_schema_coverage() -> None:
    text = read(SCHEMA_SCRIPT)
    if text is None:
        return
    rows = {t: set(f.split()) - {"-"} for t, f in SCHEMA_ROW_RE.findall(text)}
    listed = set(rows)
    if not listed:
        errors.append(
            f"[unparsable] {SCHEMA_SCRIPT} no longer contains a `Type|fields|…` "
            "table. A coverage gate that cannot find what it checks is worse "
            "than no gate — fix the pattern in this linter, do not delete the "
            "check."
        )
        return

    used = record_types_in_swift()

    for rtype, where in sorted(used.items()):
        if rtype not in listed:
            errors.append(
                f"[unlisted] {where}: record type \"{rtype}\" is used by the app "
                f"but is not in {SCHEMA_SCRIPT}'s requirements table. "
                "Production does not auto-create record types, so nobody would "
                "deploy it and every read and write against it would fail "
                "silently. Add a row naming its fields and the indexes its "
                "queries need."
            )

    for rtype in sorted(listed - set(used)):
        errors.append(
            f"[stale] {SCHEMA_SCRIPT} requires record type \"{rtype}\", but no "
            "Swift source names it. Either the app dropped it (remove the row) "
            "or it is reached by a spelling this gate cannot see (name the "
            "constant `…RecordType`). A requirement nobody needs teaches the "
            "next person to distrust the table."
        )

    # Fields, for the types both sides agree exist. A type missing from one side
    # is already reported above; comparing its fields would just repeat that.
    written = record_fields_in_swift(string_constants())
    for rtype in sorted(listed & set(used)):
        want = rows[rtype]
        got = written.get(rtype, {})

        for field, where in sorted(got.items()):
            if field not in want:
                errors.append(
                    f"[unlisted field] {where}: \"{rtype}\" is written with a "
                    f"\"{field}\" field that {SCHEMA_SCRIPT} does not require. "
                    "Production rejects a save carrying a field its schema "
                    "doesn't define — so once this ships, EVERY write of this "
                    f"record type fails, not just the new field. Add \"{field}\" "
                    f"to the {rtype} row, then promote the schema."
                )

        # The reverse only when the gate could see any writes at all. A type
        # written somewhere this scan cannot attribute would otherwise report
        # all of its fields as stale, which is a lie in the loud direction.
        if got:
            for field in sorted(want - set(got)):
                errors.append(
                    f"[stale field] {SCHEMA_SCRIPT} requires \"{field}\" on "
                    f"\"{rtype}\", but no Swift source writes it. Drop it from "
                    "the row if the app stopped setting it — a schema "
                    "requirement nobody writes is one more thing standing "
                    "between a real gap and the person reading this output."
                )


def main() -> int:
    want = declared_identifier()
    if want is not None:
        check_entitlements(want)
    check_no_default_calls()
    check_schema_coverage()

    if errors:
        print(
            f"lint_cloudkit_container.py: {len(errors)} problem(s):",
            file=sys.stderr,
        )
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1
    # Print what was actually matched, not just "OK". A silent gate that has
    # quietly stopped finding anything looks exactly like a clean tree.
    written = record_fields_in_swift(string_constants())
    types = record_types_in_swift()
    summary = ", ".join(
        f"{t} ({', '.join(sorted(written.get(t, {}))) or 'no fields written'})"
        for t in sorted(types)
    ) or "none"
    print(
        f'CloudKit container OK — "{want}" everywhere, no .default() call '
        f"sites, schema table covers: {summary}."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
