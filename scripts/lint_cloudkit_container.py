#!/usr/bin/env python3
"""CloudKit container gate — no `CKContainer.default()`, and one identifier.

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


def code_of(line: str) -> str:
    """The line with its `//` comment removed.

    The rule has to be explainable somewhere, and the place it is explained —
    CloudContainer.swift's header — necessarily writes the banned call out in
    prose. Blanket-skipping that file would blind the gate to a real call site
    in the one file most likely to grow one, so strip comments instead and keep
    checking every file's actual code.

    Deliberately naive: a `//` inside a string literal ends the scan early. The
    cost is a missed call on a line that both contains a string with `//` in it
    AND calls the banned API after it; the alternative is lexing Swift here,
    which is a much larger thing to maintain correctly than this gate is worth.
    """
    return line.split("//", 1)[0]


def check_no_default_calls() -> None:
    for root in SWIFT_ROOTS:
        base = ROOT / root
        if not base.is_dir():
            continue
        for f in sorted(base.rglob("*.swift")):
            for i, line in enumerate(f.read_text(encoding="utf-8").splitlines(), 1):
                if BANNED_CALL.search(code_of(line)):
                    errors.append(
                        f"[banned] {f.relative_to(ROOT)}:{i}: CKContainer.default() "
                        "raises an uncatchable ObjC exception in an unsigned "
                        "build. Use CloudContainer.shared."
                    )


def main() -> int:
    want = declared_identifier()
    if want is not None:
        check_entitlements(want)
    check_no_default_calls()

    if errors:
        print(
            f"lint_cloudkit_container.py: {len(errors)} problem(s):",
            file=sys.stderr,
        )
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1
    print(f'CloudKit container OK — "{want}" everywhere, no .default() call sites.')
    return 0


if __name__ == "__main__":
    sys.exit(main())
