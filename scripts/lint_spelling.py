#!/usr/bin/env python3
"""Fail the build on British spellings. THE one enumerated list in this repo.

    python3 scripts/lint_spelling.py          # check the whole tree
    python3 scripts/lint_spelling.py PATH...  # check specific paths

WHY THE LIST LIVES HERE AND NOWHERE ELSE
----------------------------------------
AGENTS.md rule 3b points at this file instead of naming the banned forms,
and that is not tidiness — it is a bug fix. The rule used to spell them out
as "write X, not Y", so the first repo-wide spelling sweep rewrote its own
rule: the "never write these" column came back as a list of the CORRECT
spellings, leaving a rule that forbade exactly what it required. It happened
in the website repo too, to the same paragraph, on the same day.

A regex in a linter is the one place a sweep has no reason to touch, because
nobody sweeps the thing that defines the sweep. Keep it that way: if you find
yourself typing a British word into a .md to illustrate the rule, don't.

WHAT IS NOT A BRITISH SPELLING — the expensive half of this file
---------------------------------------------------------------
A naive substring sweep is actively dangerous. Every entry in ALLOW below is
a word this repo actually contains that a careless rule would corrupt:

  characteristic   257 hits, ALL of them the BLE GATT API name. A
                   `characteris -> characteriz` rule renames NimBLE's API and
                   breaks the firmware. (In the website repo the same rule
                   shipped "battery characteriztic" into user-facing copy
                   before a byte-compare test caught it.)
  realistic        correct English; `realis -> realiz` gives "realiztic"
  optimistic/ism   correct; `optimis -> optimiz` gives "optimiztic"
  initialism       correct; `initialis -> initializ` gives "initializm"
  programmer       correct; `programme -> program` is fine here but the
                   stem rule would also hit "programmers"
  emphasis         same in both dialects, like `analysis`, `parameter`,
                   `diameter` — not exceptions, simply not British
  aria-labelledby  an ARIA attribute. `labelled -> labeled` silently breaks
                   accessibility markup that no test asserts on.
  MakerBot         contains "kerb"
  checkerboard     contains "kerb"

So the patterns below are anchored to whole words or explicit suffix sets,
never bare substrings.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXTS = {".rs", ".py", ".js", ".mjs", ".ts", ".c", ".h", ".cpp", ".hpp", ".md",
        ".html", ".css", ".swift", ".json", ".yml", ".yaml", ".sh", ".toml",
        ".scad"}
SKIP_DIRS = {".git", "node_modules", "target", "build", "dist", ".venv",
             "__pycache__"}
# This file names the banned forms, so it cannot police itself.
SKIP_FILES = {"lint_spelling.py"}

BANNED = re.compile(
    r"\b("
    # NOT colou?r — the optional u matches "colored", which is the CORRECT
    # US spelling. That typo made this linter flag ~8 already-correct files on
    # its first run. Spell the British forms out; never make the u optional.
    r"colour|colours|coloured|colouring"
    r"|behaviour|behaviours|behavioural"
    r"|centre|centres|centred|centring|centrepiece"
    r"|metre|metres|millimetre|millimetres|centimetre|centimetres"
    r"|neighbour|neighbours|neighbouring"
    r"|grey|greys|greyed|greying"
    r"|labelled|labelling|mislabelled|relabelled"
    r"|cancelled|cancelling|uncancelled"
    r"|modelled|modelling"
    r"|levelled|levelling|travelling|signalling|dialling"
    r"|catalogue|catalogued|catalogues"
    r"|judgement|judgements|judgemental"
    r"|honour|honours|honoured|honouring"
    r"|defence|defences|odour|odours|labour|labours|favour|favours"
    r"|mould|moulded|moulding|fibre|fibres|theatre|litre|litres"
    r"|licence|licences|practise|practised|storey|storeys|tyre|tyres"
    r"|aluminium|sceptic|sceptical|cheque|cheques|kerb|kerbs"
    r"|recognise|recognised|recognises|recognising|recognisable|unrecognised"
    r"|organise|organised|organises|organising|organisation|organisations"
    r"|normalise|normalised|normalises|normalising|normalisation"
    r"|optimise|optimised|optimises|optimising|optimisation"
    r"|initialise|initialised|initialises|initialising|initialisation"
    r"|uninitialised"
    r"|specialise|specialised|specialising|specialisation"
    r"|generalise|generalised|generalising|generalisation"
    r"|characterise|characterised|characterising|characterisation"
    r"|analyse|analysed|analyser|analysers|analysing"
    r"|apologise|apologised|summarise|summarised|utilise|utilised"
    r"|prioritise|prioritised|standardise|standardised|customise|customised"
    r"|authorise|authorised|civilise|civilised|emphasise|emphasised"
    r"|programme|programmes"
    r")\b",
    re.IGNORECASE,
)
# Present in the tree, correct, and destroyed by a careless rule. Asserted so
# a future edit to BANNED that starts eating them fails loudly here.
ALLOW = ["characteristic", "realistic", "optimistic", "optimism", "initialism",
         "programmer", "emphasis", "analysis", "analyses", "aria-labelledby",
         "MakerBot", "checkerboard", "parameter", "diameter",
         # the US spellings themselves — a too-greedy alternative that starts
         # matching these is the failure mode that shipped on this file's
         # first run (colou?r matched "colored")
         "color", "colors", "colored", "coloring", "center", "centered",
         "meter", "gray", "labeled", "canceled", "modeled", "license",
         "analyze", "analyzed", "recognize", "organize", "optimize",
         "initialize", "catalog", "judgment", "honor", "defense", "story"]


def walk(paths):
    for base in paths:
        p = Path(base)
        if p.is_file():
            yield p
            continue
        for f in p.rglob("*"):
            if (f.is_file() and f.suffix in EXTS
                    and f.name not in SKIP_FILES
                    and not any(d in f.parts for d in SKIP_DIRS)):
                yield f


def main() -> int:
    paths = sys.argv[1:] or [ROOT]
    offenders = []
    for f in walk(paths):
        try:
            text = f.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for i, line in enumerate(text.splitlines(), 1):
            m = BANNED.search(line)
            if m:
                offenders.append(f"{f.relative_to(ROOT)}:{i}: {m.group(0)}")
    # The guard on the guard: if BANNED ever starts matching a word that is
    # correct English, say so here rather than in a corrupted commit.
    self_harm = [w for w in ALLOW if BANNED.search(w)]
    if self_harm:
        print("lint_spelling.py: the BANNED pattern now matches words that are "
              f"CORRECT English: {', '.join(self_harm)}.\n"
              "  Anchor the offending alternative to whole words — see this "
              "file's header for what each of these cost.", file=sys.stderr)
        return 2
    if offenders:
        print(f"lint_spelling.py: {len(offenders)} British spelling(s) — this "
              "repo is US English (AGENTS.md rule 3b):", file=sys.stderr)
        for o in offenders[:40]:
            print(f"  {o}", file=sys.stderr)
        if len(offenders) > 40:
            print(f"  … and {len(offenders) - 40} more", file=sys.stderr)
        return 1
    print("spelling OK — US English throughout")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
