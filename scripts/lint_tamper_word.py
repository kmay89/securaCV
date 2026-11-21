#!/usr/bin/env python3
"""Fail the build on the "-proof" form. The record is tamper-EVIDENT.

    python3 scripts/lint_tamper_word.py          # check the whole tree
    python3 scripts/lint_tamper_word.py PATH...  # check specific paths

AGENTS.md rule 4 (don't oversell) has always meant this word: a signed,
hash-chained log makes an edit visible; it does not prevent one, and a root
operator on the host sits outside the boundary. Every public page said so.
The devices did not: for a year a Canary's own boot banner printed "It
creates tamper-proof records of what it sees, so nobody can change the story
later", the console greeting said the same, three Lab generators carried the
sentence into the pages that show a boot log, and six compiled emulator
bundles carried it into the browser. The copy rule was enforced on the
website and on the HACS store page, by their own tests, and by nothing here.

So the stem is banned in this tree the way the bird-group word is
(lint_fleet_word.py is the model, down to the self-test): every tracked text
file, identifiers included, in any spelling — hyphen, space, underscore or
none.

WHAT IS ALLOWED
---------------
Only the files on ALLOW_FILES, for one of three reasons: the file STATES the
rule (which takes writing the word once, usually after "never"); it is an
audit or review that QUOTES the claim it found; or it is a test or another
lint that asserts the word's absence. Hard-wrapped prose puts "never" on one line and the word
on the next, so a per-line "is it negated?" check would be a guess — the list
is by file, with the reason beside each entry. If this lint just went red on
your new file, the fix is to write "tamper-evident", not to grow the list.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTS = {".rs", ".py", ".js", ".mjs", ".ts", ".c", ".h", ".cpp", ".hpp", ".md",
        ".html", ".css", ".swift", ".json", ".yml", ".yaml", ".sh", ".toml",
        ".scad", ".ino", ".txt", ".svg", ".plist", ".ini", ".properties",
        ".entitlements", ".csv", ".webmanifest"}
EXTRA_NAMES = {"Makefile", "makefile", "GNUmakefile", "Dockerfile"}
# dist/ is compiled from sources this lint reads; a stale bundle is the dist
# drift check's business (canary-local.yml), and it cannot be edited by hand.
SKIP_DIRS = {".git", "node_modules", "target", "build", "dist", ".venv",
             "__pycache__", ".pytest_cache"}
COMMITTED_THIRD_PARTY = ("firmware", "third_party")


def fetched_third_party(f):
    """True for a file under a fetched third_party/ (not firmware/third_party/)."""
    if "third_party" not in f.parts:
        return False
    parts = f.resolve().parts
    return any(p == "third_party" and parts[i - 1:i + 1] != COMMITTED_THIRD_PARTY
               for i, p in enumerate(parts) if i)


# These two files name the word, so they cannot police themselves.
SKIP_FILES = {"lint_tamper_word.py", "test_lint_tamper_word.py"}

# Any spelling of the stem: tamper-proof, tamperproof, tamper proof,
# tamper_proof, TamperProof, tamper-proofing.
BANNED = re.compile(r"tamper[\s_-]?proof", re.IGNORECASE)

ALLOW_FILES = {
    # State the rule ("tamper-evident, never ...").
    "docs/LEGAL.md",
    "docs/security/PENTEST_SCOPE.md",
    "docs/security/CEREMONY_RUNBOOK.md",
    "docs/security/CUSTODY_PRACTICE_STATEMENT_TEMPLATE.md",
    "docs/strategy/15-maker-marketing-kit.md",
    "docs/strategy/27-liability-minimization-posture.md",
    "docs/strategy/32-one-story-every-surface.md",
    "docs/research/enterprise_surveillance_landscape.md",
    # Audits and reviews that quote the claim they found.
    "docs/legal-audit-2026-07.md",
    "docs/review/01-flag-report.md",
    "docs/strategy/03-readme-and-marketing-audit.md",
    "scripts/tests/test_docs_claims.py",   # quotes the legal audit's sentence
    # A guard test asserting the word's absence in the witness statement.
    "custom_components/securacv/tests/test_witness_statement.py",
    # Another lint that bans the word: it is in that lint's claim-word
    # pattern and in its own must-fail self-test.
    "scripts/lint_compliance_map.py",
}

# The guard on the guard.
MUST_PASS = [
    "a signed, tamper-evident record",
    "Tamper detection, live",
    "the tamper GPIO is armed",
    "proof that tampering was caught",
    "tamper_detected",
    "a tamper-evident seal; proof of custody follows",
]
MUST_FAIL = [
    "It creates tamper-proof records of what it",
    "Tamperproof logging",
    "a tamper proof log",
    "TAMPER_PROOF_MODE",
    "tamper-proofing the record",
]


def walk(paths):
    for base in paths:
        p = Path(base).resolve()
        if not p.exists():
            print(f"lint_tamper_word.py: no such path: {base}", file=sys.stderr)
            raise SystemExit(2)
        if p.is_file():
            if p.name not in SKIP_FILES:
                yield p
            continue
        for f in p.rglob("*"):
            if (f.is_file()
                    and (f.suffix in EXTS or f.name in EXTRA_NAMES)
                    and f.name not in SKIP_FILES
                    and not any(d in f.parts for d in SKIP_DIRS)
                    and not fetched_third_party(f)):
                yield f


def offenders_in(paths):
    out = []
    for f in walk(paths):
        rel = f.relative_to(ROOT).as_posix() if f.is_relative_to(ROOT) else str(f)
        if rel in ALLOW_FILES:
            continue
        try:
            text = f.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for i, line in enumerate(text.splitlines(), 1):
            if BANNED.search(line):
                out.append(f"{rel}:{i}")
    return out


def main() -> int:
    broken_guard = ([s for s in MUST_PASS if BANNED.search(s)]
                    + [s for s in MUST_FAIL if not BANNED.search(s)])
    if broken_guard:
        print("lint_tamper_word.py: the pattern no longer behaves — these "
              "self-test strings get the wrong verdict:", file=sys.stderr)
        for s in broken_guard:
            print(f"  {s!r}", file=sys.stderr)
        return 2
    stale = sorted(a for a in ALLOW_FILES
                   if not (ROOT / a).is_file()
                   or not BANNED.search((ROOT / a).read_text(encoding="utf-8")))
    if stale and not sys.argv[1:]:
        print("lint_tamper_word.py: these ALLOW_FILES entries no longer contain "
              "the word (or the file is gone) — remove them:", file=sys.stderr)
        for a in stale:
            print(f"  {a}", file=sys.stderr)
        return 1
    offenders = offenders_in(sys.argv[1:] or [ROOT])
    if offenders:
        print(f"lint_tamper_word.py: {len(offenders)} use(s) of the \"-proof\" "
              "form — the record is tamper-EVIDENT: an edit shows, nothing "
              "prevents one (AGENTS.md rule 4):", file=sys.stderr)
        for o in offenders[:40]:
            print(f"  {o}", file=sys.stderr)
        if len(offenders) > 40:
            print(f"  … and {len(offenders) - 40} more", file=sys.stderr)
        return 1
    print("tamper word OK — the record is tamper-evident everywhere it is described")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
