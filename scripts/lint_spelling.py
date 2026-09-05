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
        ".scad",
        # Arduino sketches are firmware source and were invisible to the first
        # version of this gate, which then reported "US English throughout"
        # while three tracked .ino files still carried banned forms. A gate
        # that advertises repo-wide has to mean it.
        ".ino",
        # Same lesson, wider: figures and icons (.svg), Apple property lists
        # and entitlements, config (.ini/.properties), data tables (.csv),
        # web manifests and any future .xml are text with words in them.
        # All scanned clean the day they were added.
        ".svg", ".webmanifest", ".plist", ".ini", ".properties",
        ".entitlements", ".csv", ".xml"}
# Extensionless files that are still source. Same reason as .ino.
EXTRA_NAMES = {"Makefile", "makefile", "GNUmakefile", "Dockerfile"}
SKIP_DIRS = {".git", "node_modules", "target", "build", "dist", ".venv",
             "__pycache__",
             # Tool caches: mypy's cache serializes typeshed stubs (which spell
             # `CANCELLED` the way the stdlib does), so a local `mypy` run made
             # this gate red on files nobody wrote. CI never has them; the
             # person running both tools locally does.
             ".mypy_cache", ".pytest_cache", ".ruff_cache",
             # Vendored upstream sources, fetched into the tree by a build and
             # not ours to respell. canary-local/emulator/build.sh clones LVGL,
             # ArduinoJson and arduinolibs into third_party/ before the wasm
             # build; ArduinoJson alone ships a bundled catch.hpp that trips
             # this gate 240 times. CI never saw it — the lint job gets a
             # fresh checkout and the wasm job is a different job — so the
             # only person who ever met those 242 failures was whoever built
             # the emulator locally and then ran the linter, which is exactly
             # the person a gate should not be lying to.
             #
             # Note this is a DIRECTORY name, not a path: nothing tracked in
             # git lives under a third_party/ anywhere, so skipping the name
             # cannot narrow what the gate covers in a clean checkout. (The
             # tracked vendor/ directories are deliberately NOT skipped —
             # those files are committed here and do get linted.)
             "third_party"}
# This file names the banned forms, so it cannot police itself.
SKIP_FILES = {"lint_spelling.py"}

# The website's tests/copy-honesty.test.mjs holds the same families for that repo — keep them aligned.
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
    r"|levelled|levelling|travelling|signalling|dialling|dialled"
    r"|bevelled|bevelling"
    r"|catalogue|catalogued|catalogues"
    r"|judgement|judgements|judgemental"
    r"|honour|honours|honoured|honouring"
    r"|defence|defences|odour|odours|labour|labours|favour|favours"
    r"|mould|moulded|moulding|fibre|fibres|theatre|litre|litres"
    r"|licence|licences|practise|practised|storey|storeys|tyre|tyres"
    r"|aluminium|sceptic|sceptical|cheque|cheques|kerb|kerbs"
    r"|anodise|anodised|anodising"
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
    r"|realise|realised|realises|realising"
    r"|apologise|apologised|summarise|summarised|summarising"
    r"|utilise|utilised|utilising"
    r"|prioritise|prioritised|prioritising"
    r"|standardise|standardised|standardising"
    r"|customise|customised|customising"
    r"|authorise|authorised|authorising|civilise|civilised"
    r"|emphasise|emphasised|emphasising"
    # The list is a word list, not a suffix rule, so a family that nobody
    # happened to type is a family that ships. This one did: the enclosure
    # sources carried it four times, and one of those is a trailing comment
    # on a released case's Customizer knob — which the builder manifest
    # parses as that control's help text and carries to the website. The
    # lint reported "US English throughout" the whole time.
    r"|equalise|equalised|equalises|equalising|equalisation"
    r"|randomise|randomised|randomising"
    r"|serialise|serialised|serialising"
    r"|synchronise|synchronised|synchronising"
    r"|minimise|minimised|minimising"
    r"|maximise|maximised|maximising"
    r"|finalise|finalised|finalising"
    r"|visualise|visualised|visualising"
    r"|categorise|categorised|categorising"
    r"|harmonise|harmonised|harmonising"
    r"|memorise|memorised|memorising"
    r"|stabilise|stabilised|stabilising"
    r"|sanitise|sanitised|sanitising"
    r"|programme|programmes"
    r")\b",
    re.IGNORECASE,
)
# Present in the tree, correct, and destroyed by a careless rule. Asserted so
# a future edit to BANNED that starts eating them fails loudly here.
# FIXED THIRD-PARTY API NAMES, masked out before the ban runs.
#
# These are not our words to respell, and the rule has always said so — but
# "not ours to respell" has to be MECHANICAL here, because the first sweep
# duly rewrote Swift's `Task.isCancelled` to `isCanceled` and Network's
# `.cancelled` browser state to `.canceled`. Neither member exists; both
# Apple targets stop compiling. Reverting them by hand is not enough either,
# because the correct spelling would then fail this very lint.
#
# Keyed by suffix so an exemption cannot leak into a language that does not
# have the API. Add sparingly, and only for identifiers a vendor defines.
API_EXEMPT = {
    ".swift": [r"\bisCancelled\b", r"\.cancelled\b", r"\bCancellationError\b"],
}

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
            if (f.is_file()
                    and (f.suffix in EXTS or f.name in EXTRA_NAMES)
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
        exempt = API_EXEMPT.get(f.suffix, [])
        for i, line in enumerate(text.splitlines(), 1):
            # Blank the vendor API names FIRST, so the ban never sees them and
            # never has to be weakened to accommodate them.
            probe = line
            for pat in exempt:
                probe = re.sub(pat, lambda m: "\x00" * len(m.group(0)), probe)
            m = BANNED.search(probe)
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
