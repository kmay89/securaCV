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
  3. EVERY CKRECORD TYPE THE APP USES IS LISTED IN ios/scripts/
     cloudkit_schema.sh. CloudKit invents record types on first write in the
     DEVELOPMENT environment and refuses to in production, so a type nobody
     deployed makes every save and every query against it fail — and both
     CloudKit call sites in this app swallow their errors on purpose, because a
     failed sync must never stall the local alert already reaching the person
     in the room. The result is a feature that is simply dead in the shipped
     app with nothing anywhere to point at.

     `cloudkit_schema.sh` is the tool that promotes the schema and answers
     "did it ship?" — and it can only check the types it has been told about.
     Its first version knew only `WitnessWake`, while `CloudSync` had been
     writing and querying `PairedDevice` since the day it was written. Nothing
     caught that, because nothing was looking. This rule looks.

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
# the table grows a column, which is the likelier edit.
SCHEMA_ROW_RE = re.compile(r"^([A-Za-z][A-Za-z0-9_]*)\|", re.MULTILINE)

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


def check_schema_coverage() -> None:
    text = read(SCHEMA_SCRIPT)
    if text is None:
        return
    listed = set(SCHEMA_ROW_RE.findall(text))
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
    types = ", ".join(sorted(record_types_in_swift())) or "none"
    print(
        f'CloudKit container OK — "{want}" everywhere, no .default() call '
        f"sites, schema table covers: {types}."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
