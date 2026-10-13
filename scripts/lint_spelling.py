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
import functools
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

# THE WORD LIST. The website's tests/copy-honesty.test.mjs (UK_WORD_LIST)
# holds the same words, letter for letter, and the generated families below
# come from the same file — keep the two repos aligned, because a JSON file
# carried from here to there (scad/cad-dims.json, scad/colorways.json,
# onboarding-spec.json) is held to both. The carried .scad sources are held
# to this lint alone: the website's test reads .json, .md, .html and the like,
# never .scad, so this is the only gate their comments ever meet.
WORD_LIST = (
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
)

# THE FAMILIES. The list above is a list, not a rule, so a form nobody typed
# ships. One did, on the website: a page wrote "better-" plus the British past
# tense of "travel", and the list held only that verb's -ing form. So the
# website generates its families from US words — the right spelling, which is
# safe to write anywhere — and each family's rule makes every inflection at
# once. This is a port of those families (tests/copy-honesty.test.mjs,
# website origin/main 32fa634, 2026-10-10): the same stems, the same prefixes,
# the same suffix sets. Until it was here, a carried JSON file could pass this
# lint and fail that test on the carry PR (sweep item CI9).
#
# Adding a word means adding its US spelling to the family it belongs to — in
# both repos. self_test() below proves every form a family builds is caught,
# in any letter case, that every word-list entry still is, and that every US
# form a family is built from is not.

# The British form doubles the final l before a suffix; the US form does not.
# Only verbs whose last syllable is unstressed — "control", "annul", "compel"
# double in both dialects and are deliberately absent.
DOUBLE_L = ["travel", "signal", "dial", "model", "label", "level", "cancel",
            "bevel", "fuel", "channel", "tunnel", "total", "counsel", "equal",
            "panel", "swivel", "ravel", "rival", "pedal", "shovel", "jewel",
            "grovel", "initial", "libel", "marvel", "pencil", "quarrel",
            "spiral", "stencil", "enamel", "funnel", "kennel", "pummel",
            "revel", "tassel", "towel", "chisel", "gravel", "barrel",
            "snorkel"]
DOUBLE_L_PREFIXES = ["mis", "re", "un", "pre"]
DOUBLE_L_SUFFIXES = ["ed", "ing", "er", "ers", "or", "ors"]

# -or nouns; the British spelling inserts a u, and everything built on it
# (plurals, -ed, -ful, -less, -able, -ite, -hood, -way) keeps the u, so the
# rule takes any word ending after it. "glamor" is absent: US English keeps
# the u in that one.
OR_NOUNS = ["color", "behavior", "neighbor", "honor", "odor", "labor", "favor",
            "flavor", "humor", "rumor", "harbor", "armor", "vapor", "endeavor",
            "parlor", "rigor", "vigor", "valor", "savor", "savior", "candor",
            "fervor", "ardor", "tumor", "splendor", "rancor", "clamor",
            "demeanor"]
OR_PREFIXES = ["dis", "un", "mis"]
# What the self-test builds on each noun; the rule itself takes any ending.
OR_SUFFIXES = ["", "s", "ed", "ing", "ful", "less", "able", "ite", "hood",
               "way", "ation", "ize"]

# -ize verbs; the British spelling swaps the z for an s in every form,
# -ization and -izable included. -ist and -ism are the same in both dialects
# (specialist, optimism) and are not suffixes here.
IZE_VERBS = ["organize", "recognize", "realize", "normalize", "optimize",
             "specialize", "generalize", "characterize", "initialize",
             "customize", "serialize", "synchronize", "standardize",
             "minimize", "maximize", "summarize", "prioritize", "utilize",
             "finalize", "visualize", "authorize", "categorize", "emphasize",
             "harmonize", "memorize", "stabilize", "sanitize", "randomize",
             "equalize", "apologize", "civilize", "anodize", "localize",
             "personalize", "polarize", "modernize", "digitize", "energize",
             "neutralize", "mobilize", "capitalize", "criticize", "monetize",
             "centralize", "symbolize", "sympathize", "socialize", "formalize",
             "legalize", "theorize", "tokenize", "quantize", "virtualize",
             "materialize", "penalize", "vaporize", "sterilize", "publicize",
             "scrutinize", "jeopardize", "itemize", "idealize", "privatize",
             "rationalize", "trivialize", "weaponize", "containerize",
             "parameterize", "hypothesize", "magnetize", "pressurize",
             "sensitize", "subsidize", "terrorize", "globalize", "humanize",
             "dramatize", "economize", "empathize", "familiarize", "fantasize",
             "homogenize", "immunize", "italicize", "legitimize", "liberalize",
             "regularize", "vocalize", "galvanize", "motorize", "unionize"]
IZE_PREFIXES = ["de", "re", "un", "non", "over", "under", "mis", "pre"]
IZE_SUFFIXES = ["e", "ed", "es", "ing", "ation", "ations", "er", "ers", "able"]

# -yze verbs, the same swap. Not "-yses": that is the plural of analysis.
YZE_VERBS = ["analyze", "paralyze", "catalyze", "dialyze"]
YZE_SUFFIXES = ["e", "ed", "ing", "er", "ers"]

# -er nouns the British spelling ends in -re, with the compounds a page would
# plausibly build on them. Suffixes are listed, not wildcarded: the stem of
# one of these nouns starts a Spanish hat.
ER_NOUNS = ["center", "meter", "fiber", "theater", "liter", "caliber", "saber",
            "somber", "specter", "luster", "meager", "miter", "scepter"]
ER_PREFIXES = ["epi", "milli", "centi", "kilo", "micro", "nano"]
ER_SUFFIXES = ["", "s", "d", "piece", "pieces", "line", "lines", "glass",
               "fold", "point", "points"]

# -se nouns the British spelling ends in -ce.
SE_NOUNS = ["defense", "offense", "pretense", "license"]
SE_SUFFIXES = ["e", "es", "ed"]


def _alt(words):
    return "|".join(words)


def _stems(words, us_ending):
    """Each US word with its US ending cut off: the part both dialects share."""
    assert all(w.endswith(us_ending) for w in words), us_ending
    return [w[: len(w) - len(us_ending)] for w in words]


FAMILIES = [
    rf"(?:{_alt(DOUBLE_L_PREFIXES)})?(?:{_alt(DOUBLE_L)})l"
    rf"(?:{_alt(DOUBLE_L_SUFFIXES)})",
    rf"(?:{_alt(OR_PREFIXES)})?(?:{_alt(_stems(OR_NOUNS, 'or'))})our\w*",
    rf"(?:{_alt(IZE_PREFIXES)})?(?:{_alt(_stems(IZE_VERBS, 'ize'))})is"
    rf"(?:{_alt(IZE_SUFFIXES)})",
    rf"(?:{_alt(_stems(YZE_VERBS, 'yze'))})ys(?:{_alt(YZE_SUFFIXES)})",
    rf"(?:{_alt(ER_PREFIXES)})?(?:{_alt(_stems(ER_NOUNS, 'er'))})re"
    rf"(?:{_alt(s for s in ER_SUFFIXES if s)})?",
    rf"(?:{_alt(_stems(SE_NOUNS, 'se'))})c(?:{_alt(SE_SUFFIXES)})",
    # single words whose inflections the list missed
    r"catalogu(?:e|es|ed|ing|er|ers)",
    r"grey(?:s|ed|er|est|ing|ish|ness|scale|scales)?",
    r"mould(?:s|ed|ing|ings|er|y)?",
    r"practis(?:e|ed|es|ing)",
    r"sceptic\w*",
]

# re.ASCII: \b and \w mean what they mean in the website's JavaScript regex
# (no u flag), so a word beside a non-ASCII letter is judged the same in both
# repos. On this tree the two flags find the same lines; the website's is the
# reading a carried file must pass.
BANNED = re.compile(rf"\b({WORD_LIST}|{'|'.join(FAMILIES)})\b",
                    re.IGNORECASE | re.ASCII)
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
         "initialize", "catalog", "judgment", "honor", "defense", "story",
         # the words the generated families come closest to — the website's
         # US_AND_SHARED list, carried with the families. Each is correct in
         # both dialects (or is the US form), and each sits one careless
         # suffix or stem away from a family rule.
         "programmed", "cancellation", "controlled", "annulled", "compelled",
         "totally", "initially", "equally", "rigorous", "humorous", "odorous",
         "contour", "detour", "glamour", "greyhound", "calibrate", "Polaris",
         "organism", "specialist", "terrorist", "economist", "advertise",
         "expertise", "exercise", "franchise", "comprise", "premise",
         "compromise", "paradise", "massacre", "mediocre", "genre", "acre",
         "sombrero", "centrifuge", "catalyst", "dialysis", "paralysis",
         "analyst", "analytics", "licensed", "jewelry", "dialog"]


def family_pairs():
    """Every (US form, British form) pair the families are built to tell apart.

    Each family, both ways, over every prefix and every suffix it names: the
    British form must be caught and the US form it was made from must not be.
    The British forms are BUILT here from the US stems, never typed — the same
    construction the rules use, which is the point: a stem added to a family
    is tested in every inflection the moment it is added.
    """
    pairs = []
    for p in [""] + DOUBLE_L_PREFIXES:
        for w in DOUBLE_L:
            for suf in DOUBLE_L_SUFFIXES:
                pairs.append((p + w + suf, p + w + "l" + suf))
    for p in [""] + OR_PREFIXES:
        for stem in _stems(OR_NOUNS, "or"):
            for suf in OR_SUFFIXES:
                pairs.append((p + stem + "or" + suf, p + stem + "our" + suf))
    for p in [""] + IZE_PREFIXES:
        for stem in _stems(IZE_VERBS, "ize"):
            for suf in IZE_SUFFIXES:
                pairs.append((p + stem + "iz" + suf, p + stem + "is" + suf))
    for stem in _stems(YZE_VERBS, "yze"):
        for suf in YZE_SUFFIXES:
            pairs.append((stem + "yz" + suf, stem + "ys" + suf))
    us_er = {"": "er", "s": "ers", "d": "ered"}
    for p in [""] + ER_PREFIXES:
        for stem in _stems(ER_NOUNS, "er"):
            for suf in ER_SUFFIXES:
                pairs.append((p + stem + us_er.get(suf, "er" + suf),
                              p + stem + "re" + suf))
    for stem in _stems(SE_NOUNS, "se"):
        for suf in SE_SUFFIXES:
            pairs.append((stem + "s" + suf, stem + "c" + suf))
    # the single-word rules, each with the US word it stands against
    for us, uk, sufs in (
            ("catalog", "catalogue", ["", "s", "ed", "ing", "er", "ers"]),
            ("gray", "grey", ["", "s", "ed", "er", "est", "ing", "ish", "ness",
                              "scale", "scales"]),
            ("mold", "mould", ["", "s", "ed", "ing", "ings", "er", "y"]),
            ("skeptic", "sceptic", ["", "s", "al", "ism", "ally"])):
        for suf in sufs:
            # the British form drops its final e before a vowel suffix
            drop_e = uk.endswith("e") and suf[:1] in ("e", "i")
            uk_form = (uk[:-1] if drop_e else uk) + suf
            pairs.append((us + suf, uk_form))
    for suf in ["e", "es", "ed", "ing"]:
        pairs.append(("practic" + suf, "practis" + suf))
    return pairs


def self_test():
    """(missed, eaten): British forms BANNED no longer catches — a family's
    built form, a word-list entry, or the shipped form in any letter case —
    and correct words (US forms and ALLOW) it now matches. Both must be
    empty."""
    pairs = family_pairs()
    missed = [uk for _, uk in pairs if not BANNED.search(uk)]
    # The word list is typed, not built, so nothing above covers it. BANNED is
    # a composition of it and the families; an edit to that composition that
    # drops the list (the families alone miss the forms no family builds)
    # would otherwise pass every pair here.
    missed += [w for w in WORD_LIST.split("|") if not BANNED.fullmatch(w)]
    # The one that shipped on the website: a compound adjective on a past
    # tense the word list had only in its -ing form. In every letter case,
    # because a sentence starts with a capital and a heading or a log line
    # can be all caps (two of the lines CI9 fixed were capitalized), and the
    # website's regex carries the i flag.
    shipped = DOUBLE_L[0] + "led"
    for form in (shipped, shipped.capitalize(), shipped.upper()):
        line = "a better-" + form + " route"
        if not BANNED.search(line):
            missed.append(line)
    eaten = [w for w in [us for us, _ in pairs] + ALLOW if BANNED.search(w)]
    return missed, eaten


# Every alternative in BANNED is word characters only, anchored by \b at both
# ends, so a match is always one whole run of word characters: a line has a
# banned form exactly when one of its \w+ runs fullmatches BANNED. Testing
# each distinct run once, cached, gives the same answer as searching every
# line, at a fraction of the cost — the families made BANNED twice as long,
# and a search tries every alternative at every word boundary of every line.
WORD = re.compile(r"\w+", re.ASCII)


def non_word_literals(pattern=None):
    """What is left of BANNED's pattern once its regex syntax is removed,
    when that is not letters alone. banned_words() is exact only while every
    alternative is word characters; an alternative with a hyphen, a space or
    an apostrophe in it would be split across two runs and never seen, so
    main() refuses to run on such a pattern instead of passing everything."""
    residue = re.sub(r"\\[bw]|[()|?:*]", "", BANNED.pattern if pattern is None
                     else pattern)
    return sorted(set(re.sub(r"[A-Za-z]", "", residue)))


@functools.lru_cache(maxsize=None)
def _is_banned(token):
    return BANNED.fullmatch(token) is not None


def banned_words(line):
    """The banned forms in one line, in order (the same set BANNED.search
    would find, one at a time)."""
    return [t for t in WORD.findall(line) if _is_banned(t)]


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
    # The guard on the guard, FIRST and before a single file is read: if
    # BANNED ever starts matching a word that is correct English, or stops
    # catching a form it is built to catch (a family's, the word list's, in
    # any letter case), say so here rather than in a corrupted commit or a
    # green run that checked nothing. scripts/tests/test_lint_spelling.py
    # holds main() to both exits.
    missed, eaten = self_test()
    if eaten:
        print("lint_spelling.py: the BANNED pattern now matches words that are "
              f"CORRECT English: {', '.join(eaten[:20])}.\n"
              "  Anchor the offending alternative to whole words — see this "
              "file's header for what each of these cost.", file=sys.stderr)
        return 2
    if missed:
        print("lint_spelling.py: BANNED no longer catches a British form it "
              "is built to catch (a family's form, a word-list entry, or a "
              f"form in capitals): {', '.join(missed[:20])}.",
              file=sys.stderr)
        return 2
    odd = non_word_literals()
    if odd:
        print("lint_spelling.py: BANNED has an alternative with characters "
              f"other than letters ({' '.join(map(repr, odd))}); the word-run "
              "check cannot see it. Spell the form as letters only.",
              file=sys.stderr)
        return 2
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
            for word in banned_words(probe):
                # A file outside this repo (the HACS mirror's README, as both
                # mirror briefs document) is named as given, not a traceback.
                where = f.relative_to(ROOT) if f.is_relative_to(ROOT) else f
                offenders.append(f"{where}:{i}: {word}")
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
