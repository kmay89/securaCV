#!/usr/bin/env python3
"""No Apple product name in any App Intent string App Store Connect reads.

App Store Connect validates the App Intents metadata in every upload, and it
rejects one whose intent strings use Apple's product names. Build 39.1
(0.8.0) came back by EMAIL, after a green upload, with:

    ITMS-90626: Invalid Siri Support - App Intent description 'The fleet's one
    honest answer, from what this watch last heard from your iPhone.' cannot
    contain 'iphone'

The rejection lands only after the binary is built, signed and uploaded, so
the cheapest place to catch it is here, on Linux, before anything is built.
This reads every Swift source of the app, the watch app and the widgets, and
holds the strings that become intent metadata to the rule:

  * `title` (a `LocalizedStringResource` on an intent),
  * `IntentDescription(...)` and a `description` assigned a literal,
  * every Siri phrase in an `AppShortcut(... phrases: [...])`,
  * `shortTitle:`,
  * `@Parameter(title: ...)` and `DisplayRepresentation(title: ...)`.

The words are Apple's product and service names, matched as whole words in
any case. Plain "watch" or "phone" is not a trademark and stays allowed.
Runtime dialog text is not intent metadata and is not checked.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE_DIRS = [ROOT / "ios" / "Sources", ROOT / "ios" / "Shared"]

TRADEMARKS = [
    "apple watch", "iphone", "ipad", "ipod", "apple", "siri", "ios", "ipados",
    "watchos", "macos", "mac", "airpods", "homekit", "carplay", "icloud",
    "facetime", "testflight",
]
_TM = re.compile(r"(?<![A-Za-z])(" + "|".join(re.escape(w) for w in TRADEMARKS) + r")(?![A-Za-z])",
                 re.IGNORECASE)

# Where a string becomes intent metadata. Each pattern matches up to the
# opening of the region whose literals count.
_ANCHORS = [
    re.compile(r"static\s+(?:let|var)\s+title\s*:\s*LocalizedStringResource\s*=\s*"),
    re.compile(r"static\s+(?:let|var)\s+description\s*(?::\s*IntentDescription\??)?\s*=\s*"),
    re.compile(r"IntentDescription\s*\("),
    re.compile(r"phrases\s*:\s*\["),
    re.compile(r"shortTitle\s*:\s*"),
    re.compile(r"@Parameter\s*\(\s*title\s*:\s*"),
    re.compile(r"DisplayRepresentation\s*\(\s*title\s*:\s*"),
]
_LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
_INTENT_FILE = re.compile(r"\b(AppIntent|AppShortcutsProvider|SetFocusFilterIntent|"
                          r"WidgetConfigurationIntent|AppEntity|AppEnum)\b")


def _region(src: str, start: int) -> str:
    """From `start`, the text up to the end of the bracketed group that
    begins there, or to the end of the statement (the line, plus any
    continuation lines that begin with a string literal or a `+`)."""
    i = start
    while i < len(src) and src[i] in " \t":
        i += 1
    if i < len(src) and src[i] in "([":
        open_c = src[i]
        close_c = ")" if open_c == "(" else "]"
        depth, j, in_str = 0, i, False
        while j < len(src):
            c = src[j]
            if in_str:
                if c == "\\":
                    j += 2
                    continue
                if c == '"':
                    in_str = False
            elif c == '"':
                in_str = True
            elif c == open_c:
                depth += 1
            elif c == close_c:
                depth -= 1
                if depth == 0:
                    return src[i:j + 1]
            j += 1
        return src[i:]
    lines = src[i:].split("\n")
    out = [lines[0]]
    for nxt in lines[1:]:
        if nxt.strip().startswith(('"', "+")):
            out.append(nxt)
        else:
            break
    return "\n".join(out)


def intent_strings(src: str) -> list[str]:
    """Every string literal in `src` that lands in intent metadata, once each
    (two anchors can cover one literal: `static let description =
    IntentDescription(...)`), in source order. An interpolation such as
    `\\(.applicationName)` is the app's own name filled in by the system."""
    seen: dict[int, str] = {}
    for anchor in _ANCHORS:
        for m in anchor.finditer(src):
            start = m.end() - (1 if src[m.end() - 1] in "([" else 0)
            region = _region(src, start)
            base = src.index(region, start) if region else start
            for lit in _LITERAL.finditer(region):
                seen.setdefault(base + lit.start(), lit.group(1))
    return [seen[k] for k in sorted(seen)]


def offenses(src: str) -> list[tuple[str, str]]:
    return [(lit, hit.group(1)) for lit in intent_strings(src) for hit in [_TM.search(lit)] if hit]


class IntentStringsTest(unittest.TestCase):
    def test_no_intent_string_names_an_apple_product(self) -> None:
        files = [p for d in SOURCE_DIRS for p in d.rglob("*.swift")]
        intent_files = [p for p in files if _INTENT_FILE.search(p.read_text(encoding="utf-8"))]
        self.assertTrue(intent_files, "the app declares App Intents")
        checked = 0
        problems = []
        for p in intent_files:
            src = p.read_text(encoding="utf-8")
            checked += len(intent_strings(src))
            for lit, word in offenses(src):
                problems.append(f"{p.relative_to(ROOT)}: '{lit}' contains '{word}'")
        self.assertGreater(checked, 10, "the scanner found the intent strings it is meant to read")
        self.assertEqual(problems, [], "App Store Connect rejects these (ITMS-90626):\n" + "\n".join(problems))

    def test_the_scanner_catches_the_string_apple_rejected(self) -> None:
        rejected = '''
        struct CheckFleetIntent: AppIntent {
            static let title: LocalizedStringResource = "Check the Fleet"
            static let description = IntentDescription(
                "The fleet's one honest answer, from what this watch last heard from your iPhone.")
        }'''
        self.assertEqual([w.lower() for _, w in offenses(rejected)], ["iphone"])

    def test_the_scanner_reads_every_metadata_slot(self) -> None:
        src = '''
        static var title: LocalizedStringResource = "Ask Siri"
        static var description: IntentDescription? = "Uses HomeKit"
        AppShortcut(intent: X(), phrases: ["Check \\(.applicationName) on my iPad", "Plain"],
                    shortTitle: "For Apple Watch", systemImageName: "bird")
        @Parameter(title: "Sync with iCloud", default: false)
        DisplayRepresentation(title: "CarPlay view", subtitle: "x")
        '''
        words = sorted(w.lower() for _, w in offenses(src))
        self.assertEqual(words, sorted(["siri", "homekit", "ipad", "apple watch", "icloud", "carplay"]))

    def test_plain_words_stay_allowed(self) -> None:
        src = 'static let description = IntentDescription("The latest snapshot on your wrist, from your phone, on your watch.")'
        self.assertEqual(offenses(src), [])
        self.assertEqual(len(intent_strings(src)), 1)


if __name__ == "__main__":
    unittest.main()
