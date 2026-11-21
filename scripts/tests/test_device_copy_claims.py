"""What a Canary, its portal pages and the Lab say about themselves, held to
the repo's own security model.

The website's claims audit retired three sentence families in 2026-07 and its
tests keep them out. Nothing kept them out of this tree, so for a year the
product contradicted its own docs/security/SECURITY_MODEL.md:

  * a display's boot scene promised it would "never phone home" — the model's
    disclosed outbound paths list a daily signed update check on every
    product with pull-OTA (path 2) and SNTP on every networked one (path 3);
  * the WAP's first-boot scene and its setup wizard said nobody could alter
    or quietly change a record — a signed, hash-chained log makes a change
    visible; it prevents none;
  * the captive-portal page, the flagship's setup page, the setup wizard and
    the Lab's Home Assistant walkthrough said nothing leaves your home, with
    no scope — and eighteen Lab pages ended "works offline · nothing here
    phones anywhere" on a site that has no offline copy and fetches every
    page from its host.

This suite reads the places a person is told what the product does — firmware
strings, portal pages, the Lab's pages, scripts, generators and generated
data, the desktop app's pages, and the iPhone, watch and Apple TV apps'
screens — and fails on any of the three, in code. A comment is not copy:
comment lines are skipped, and the two files that state the rule in prose are
listed with the reason.

The apps came last (2026-11): two iPhone screens said "nothing leaves your
home" about a Bluetooth search, and the Wall's searching card said "nothing
leaves this room" while its own settings screen explained, in a comment, why
that sentence cannot be unconditional there (a hub address somewhere else is a
supported input). Each was true of the search it described and none said so.

What to write instead is what stays put, said with its scope: the record
stays on the device; a change to any entry shows; this page runs in your
browser and nothing is uploaded.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Where the product speaks. Each entry: (directory, glob, recursive).
SOURCES = [
    ("firmware/common", "*", True),
    ("firmware/canary/lib", "*", True),
    ("firmware/canary/src", "*", True),
    ("firmware/projects", "*", True),
    ("canary-local", "*.html", False),
    ("canary-local/assets", "*.js", False),
    ("canary-local/tools", "*.py", False),
    ("canary-local/devices", "*.json", False),
    ("desktop/src", "*", True),
    ("desktop-lab/src", "*", True),
    ("ios/Sources", "*.swift", True),
    ("ios/Shared", "*.swift", True),
    ("tvos/WitnessWall/Sources", "*.swift", True),
]
EXTS = {".c", ".h", ".hpp", ".cpp", ".ino", ".html", ".js", ".mjs", ".py", ".json",
        ".swift"}
SKIP_PARTS = {"tests_host", "tests", "test", "third_party", "vendor", "build",
              "dist", "node_modules", ".pio", "target"}

RULES = [
    (re.compile(r"\bphones?\s+(?:home|anywhere)\b", re.I),
     "every product with pull-OTA asks the release host for a signed manifest "
     "each day and the networked ones sync time (SECURITY_MODEL.md, disclosed "
     "outbound paths 2 and 3); a Lab page is fetched from its host"),
    # An aside between the subject and the verb ("nobody — not even you — can
    # quietly edit") hid two Lab strings from the first form of this rule.
    (re.compile(r"\b(?:nobody|no[ -]one)\b(?:\s*[—–-]+[^—–.\n]{0,40}[—–-]+)?\s*"
                r"\bcan\s+(?:(?:quietly|silently)\s+)?"
                r"(?:alter|change|edit|forge|rewrite)\b", re.I),
     "a signed, hash-chained log shows a change; it prevents none"),
    # "This room" and "the room" are the Apple TV's form of the same sentence.
    (re.compile(r"\bnothing\s+leaves\s+(?:your\s+home|th(?:is|e)\s+room)\b", re.I),
     "unscoped: say what stays (the record, on the device; a search, on your "
     "network) and name the one call a device makes on its own"),
]

# Files that state the rule in prose rather than make the claim.
STATES_THE_RULE = {
    "canary-local/tools/gen_head.py": "its docstring and its RETIRED pattern name the retired phrase",
}

COMMENT_START = ("//", "/*", "*", "#", "<!--")

# Sentences the suite must judge the way it claims to.
MUST_FAIL = [
    'out("verify their chains on its own silicon, and never phone home.");',
    '"Nobody can alter these records after the fact."',
    "<li>Every entry is signed, so nobody can quietly change it.</li>",
    "<p>Local-only by default. Nothing leaves your home.</p>",
    "works offline · nothing here phones anywhere",
    '"custody": "No cloud, nothing phones home; the image is pinned."',
    "so no one can forge an entry",
    "give:'One dashboard and one timeline nobody — not even you — can quietly edit.'",
    '"The record it keeps now is one nobody — including you — can silently edit."',
    'Text("This phone listens for it over Bluetooth — nothing leaves your home. iOS asks for Bluetooth next.")',
    'Text("so does every Canary that publishes a fleet report, hub or not. Nothing leaves this room.")',
    "no subscription, nothing leaves the room",
]
MUST_PASS = [
    '"verify their chains on its own silicon, with no cloud in between.",',
    '"A change to any record after the fact will show."',
    "<li>It sends that record nowhere on its own.</li>",
    "runs in your browser · nothing is uploaded",
    "Your Canary keeps its own network: no home WiFi needed, nothing leaves the device.",
    "audio never recorded, never leaves this board",
    "the phone at home can change the setting",
    "Nothing leaves this page.",
    "give:'One dashboard, and one timeline where any edit shows — yours included.'",
    "Nobody answered: commit anyway, forgetting the session",
    'Text("This phone listens for it over Bluetooth — a short-range search that uses no internet. iOS asks for Bluetooth next.")',
    'Text("so does every Canary that publishes a fleet report, hub or not. The search stays on your network.")',
    "Nothing leaves the network, and nothing is stored.",
]


def files():
    for base, pattern, recursive in SOURCES:
        root = REPO / base
        if not root.is_dir():
            continue
        walker = root.rglob(pattern) if recursive else root.glob(pattern)
        for f in walker:
            if (f.is_file() and f.suffix in EXTS
                    and not SKIP_PARTS.intersection(f.relative_to(REPO).parts)):
                yield f


def is_comment(line: str) -> bool:
    return line.lstrip().startswith(COMMENT_START)


def verdict(line: str) -> str | None:
    """The reason a line of copy fails, or None."""
    if is_comment(line):
        return None
    for pattern, why in RULES:
        if pattern.search(line):
            return why
    return None


def wrapped_claims(html: str) -> list[tuple[str, str]]:
    """Claims a page makes across a line break: (the words, the reason).
    Comments are dropped first, then every run of whitespace is one space.
    A claim that sits on one line is the per-line scan's to report."""
    one_line = {m.group(0) for line in html.splitlines() if not is_comment(line)
                for pattern, _ in RULES for m in pattern.finditer(line)}
    # "--!>" closes a comment too (the HTML parser accepts it), so both do here.
    text = re.sub(r"\s+", " ", re.sub(r"<!--.*?--!?>", " ", html, flags=re.S))
    return [(m.group(0), why) for pattern, why in RULES
            for m in pattern.finditer(text) if m.group(0) not in one_line]


class TheRules(unittest.TestCase):
    def test_the_sentences_that_shipped_are_caught(self):
        for s in MUST_FAIL:
            self.assertIsNotNone(verdict(s), s)

    def test_scoped_and_corrected_sentences_pass(self):
        for s in MUST_PASS:
            self.assertIsNone(verdict(s), s)

    def test_a_comment_is_not_copy(self):
        for s in ("// a mode that can't phone home can't be steered",
                  " * headers with a key nobody can change",
                  "# preserving the promise that nothing phones anywhere",
                  "<!-- nothing leaves your home -->",
                  '                    // No unconditional "nothing leaves this room" here: a',
                  '    /// contacted. This is the app\'s "nothing phones home" gate: every base URL'):
            self.assertIsNone(verdict(s), s)


class TheTree(unittest.TestCase):
    def test_the_places_the_product_speaks_are_read(self):
        seen = {f.relative_to(REPO).as_posix() for f in files()}
        self.assertGreater(len(seen), 400, "the scan lost most of its inputs")
        for rel in (
            "firmware/common/boot/boot_banner.cpp",
            "firmware/common/ui/console_scenes.h",
            "firmware/projects/canary-display/src/main.cpp",
            "firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino",
            "firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h",
            "firmware/projects/canary-wap/arduino/canary_wap/setup_page_html.h",
            "firmware/canary/lib/securacv_webui/src/securacv_setup_page.cpp",
            "canary-local/lab.html",
            "canary-local/assets/hub-setup-wizard.js",
            "canary-local/tools/gen_hub_image.py",
            "canary-local/devices/wap.json",
            "canary-local/devices/hub_image.json",
            "desktop/src/index.html",
            "ios/Sources/SecuraCV/Views/TodayView.swift",
            "ios/Sources/SecuraCV/Views/CanarySetupView.swift",
            "ios/Shared/MagicPairPlan.swift",
            "tvos/WitnessWall/Sources/WitnessWall/WallView.swift",
            "tvos/WitnessWall/Sources/WitnessWall/WallSettingsView.swift",
        ):
            self.assertIn(rel, seen, f"{rel} is no longer read — a device's words left the scan")

    def test_no_retired_claim_in_anything_the_product_says(self):
        found = []
        for f in files():
            rel = f.relative_to(REPO).as_posix()
            if rel in STATES_THE_RULE:
                continue
            try:
                text = f.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for n, line in enumerate(text.splitlines(), 1):
                why = verdict(line)
                if why:
                    found.append(f"{rel}:{n}: {why}")
            # A page wraps its sentences, and a claim broken across two lines
            # is still the claim: start.html said "a witness log nobody —
            # including you —" on one line and "can silently edit" on the next.
            if f.suffix == ".html":
                for sentence, why in wrapped_claims(text):
                    found.append(f"{rel}: {why} (across lines: \"{sentence}\")")
        self.assertEqual(found, [], "\n" + "\n".join(found[:40]))

    def test_a_claim_wrapped_across_lines_is_still_read(self):
        page = ("<p>Four ways in, one destination: a witness log nobody — including you —\n"
                "      can silently edit. Pick where you're starting from.</p>\n")
        self.assertEqual(len(wrapped_claims(page)), 1)
        self.assertEqual(wrapped_claims("<!-- a key nobody can\n change -->\n<p>fine</p>"), [])
        self.assertEqual(wrapped_claims("<!-- a key nobody can\n change --!>\n<p>fine</p>"), [])
        self.assertEqual(wrapped_claims("<p>A change to any record\n will show.</p>"), [])

    def test_every_exemption_still_needs_one(self):
        for rel, reason in STATES_THE_RULE.items():
            path = REPO / rel
            self.assertTrue(path.is_file(), f"{rel} is exempt and is gone")
            text = path.read_text(encoding="utf-8")
            self.assertTrue(any(verdict(line) for line in text.splitlines()),
                            f"{rel} no longer names a retired phrase ({reason}) — drop the exemption")

    def test_the_lab_footer_says_the_one_line(self):
        pages = sorted((REPO / "canary-local").glob("*.html"))
        said = [p.name for p in pages if "nothing is uploaded" in p.read_text(encoding="utf-8")]
        self.assertGreaterEqual(len(said), 17, said)
        for p in pages:
            self.assertNotIn("works offline", p.read_text(encoding="utf-8"),
                             f"{p.name}: the Lab on the web has no offline copy; "
                             'say "runs in your browser"')


if __name__ == "__main__":
    unittest.main()
