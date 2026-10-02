#!/usr/bin/env python3
"""Every WAP device id, SoftAP name and mDNS host a doc shows is one a WAP can have.

canary_wap.ino derives three names, and none of them from the MAC:

  device id  generate_device_id: DEVICE_ID_PREFIX + unambiguous_suffix16
  SSID       generate_ap_ssid:   "SecuraCV-" + the same suffix, same case
  mDNS host  generate_mdns_hostname: canary-<name> once named, and until
             then "canary-%02x%02x" of pubkey_fp[0..1]

unambiguous_suffix16 writes the base-54 digits of a 16-bit value, least
significant first. 65535 < 54^3, so the fourth character is always the
alphabet's first ('2') and the third is at most 'R': AB7K, AABB, CCDD and
a3f7 are suffixes no Canary prints. The docs carried all four (sweep A29's
review), beside hosts no WAP advertises: canary-<mac-suffix>.local (the
fallback is the key fingerprint's, "never the MAC") and the device id as a
host (canary-s3-ab7k.local, canary-<your-id>.local).

So this reads the recipe from canary_wap.ino, builds the set of suffixes it
can produce, and holds every user-facing doc to it. Placeholders (XXXX) pass.
docs/audit/ is not read: a dated audit quotes the wrong examples it found.

Sources too (sweep A35): two firmware comments and a Lab test fixture
showed canary-s3-AB7K and SecuraCV-7fA3 after the docs were corrected,
because this read docs only. So the WAP's sketch, the shared firmware under
firmware/common/, the Lab (canary-local/), the Home Assistant integration and
tools/ are read as well, every line, comments and string literals alike. The
display (firmware/projects/canary-display) is not: its SoftAP is
"SecuraCV-%.4s" of its own token, another product's recipe. A source line
that must keep a WAP-shaped name no WAP has is a named exemption below, with
its reason, and the test fails on an exemption that no longer matches.

Why here and not beside canary-local/tests/fingerprint_examples.test.js
(which holds the Lab page's names to the same recipe): the input is prose
anywhere under docs/, tools/ and the firmware READMEs, and canary-local.yml's
path filter names a dozen specific docs. Repo Lints (lint.yml) is unfiltered.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_wap_name_examples.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests), unfiltered
"""

from __future__ import annotations

import re
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
INO = REPO / "firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino"

# The recipe, as canary_wap.ino spells it. A move fails here, not silently.
PINS = (
    'snprintf(out, cap, "%s%s", DEVICE_ID_PREFIX, suffix)',
    'snprintf(out, cap, "SecuraCV-%s", suffix)',
    "out[i] = UNAMBIGUOUS_ALPHABET[v % UNAMBIGUOUS_LEN];",
    "v = (uint16_t)(v / UNAMBIGUOUS_LEN);",
    'snprintf(out, cap, "canary-%02x%02x",\n           g_device.pubkey_fp[0], g_device.pubkey_fp[1]);',
    "// Fallback suffix from the pubkey fingerprint, never the MAC",
)
PLACEHOLDERS = {"XXXX"}

ID_OR_SSID = re.compile(r"(?<![\w-])(canary-[cs]3-|SecuraCV-)([A-Za-z0-9]{4})(?![\w-])")
# A host no WAP advertises: the device id as a host, or a template built
# from the MAC or the device id alone.
BAD_HOST = re.compile(
    r"(?<![\w-])canary-[cs]3-[A-Za-z0-9]{4}\.local\b"
    r"|<[^<>\s]*\b(?:mac|mac-suffix|your-id)\b[^<>\s]*>[\w<>-]*\.local\b"
    r"|<device-id>\.local\b"
)


def ino() -> str:
    return INO.read_text(encoding="utf-8")


def alphabet(src: str) -> str:
    m = re.search(r'UNAMBIGUOUS_ALPHABET\[\] =\s*"([^"]+)";', src)
    assert m, "canary_wap.ino's UNAMBIGUOUS_ALPHABET moved"
    return m.group(1)


def suffix16(v: int, abc: str) -> str:
    out = ""
    for _ in range(4):
        out += abc[v % len(abc)]
        v //= len(abc)
    return out


def valid_suffixes(abc: str) -> frozenset[str]:
    return frozenset(suffix16(v, abc) for v in range(1 << 16))


def problems_in(text: str, valid: frozenset[str]) -> list[str]:
    out = []
    for m in ID_OR_SSID.finditer(text):
        suf = m.group(2)
        if suf not in PLACEHOLDERS and suf not in valid:
            out.append(f"{m.group(0)}: no Canary prints the suffix {suf} (unambiguous_suffix16's 4th character is always 2)")
    for m in BAD_HOST.finditer(text):
        out.append(f"{m.group(0)}: no WAP advertises this host (canary-<name>.local, or canary-<4 hex>.local unnamed)")
    return out


def doc_files(root: Path) -> list[str]:
    tracked = subprocess.run(["git", "ls-files"], cwd=root, capture_output=True, text=True, check=True).stdout.split("\n")
    keep = []
    for f in tracked:
        if not f.endswith((".md", ".yaml", ".yml", ".txt")):
            continue
        if f.startswith("docs/audit/") or f.startswith(".github/"):
            continue
        if f in ("README.md",) or f.startswith(("docs/", "tools/", "firmware/", "canary-local/", "custom_components/")):
            keep.append(f)
    return keep


SOURCE_EXTS = (".h", ".hpp", ".c", ".cpp", ".ino", ".js", ".mjs", ".cjs", ".py",
               ".html", ".json", ".sh", ".css")
SOURCE_ROOTS = ("firmware/projects/canary-wap/", "firmware/common/", "canary-local/",
                "custom_components/", "tools/")

# (file, the line's text that may stay, why). Each must match exactly one line.
SOURCE_EXEMPT = (
    ("firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h",
     "// from the device's actual per-device hostname (canary-s3-XXXX.local) so",
     "describes what updateMdnsLinkFromDevice does: it builds the close-out link "
     "from /api/status's device_id, a host no WAP advertises (filed as a bug, not "
     "a comment to correct here: the comment is true to the code)"),
)


def source_files(root: Path) -> list[str]:
    tracked = subprocess.run(["git", "ls-files"], cwd=root, capture_output=True, text=True, check=True).stdout.split("\n")
    return [f for f in tracked if f.endswith(SOURCE_EXTS) and f.startswith(SOURCE_ROOTS)]


ABC = "23456789ABCDEFGHJKMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz"
MUST_PASS = [
    "device_id = `canary-s3-4dC2`, AP SSID `SecuraCV-4dC2`",
    "join `SecuraCV-XXXX`",
    "open http://canary-7916.local or http://canary-kitchen.local",
    "canary-<name>.local, or canary-<4 hex>.local if unnamed",
    "http://<device-id>-<pseudonym>.local",
    "the C3 build: canary-c3-Kx82",
]
MUST_FAIL = [
    "device_id = `canary-s3-AABB`",
    "> **SecuraCV-AB7K**",
    "(e.g., canary-s3-a3f7)",
    "`SecuraCV-CCDD` — a different suffix",
    "`canary-<mac-suffix>.local` if unnamed",
    "a unique `canary-<name>.local` (or `canary-<mac>.local`)",
    "http://canary-<your-id>.local",
    "e.g. `http://canary-s3-ab7k.local`",
    "live web mirror (`http://<device-id>.local`)",
]


class TheGuardItself(unittest.TestCase):
    def test_the_recipe_is_canary_wap_ino_s(self):
        src = ino()
        for pin in PINS:
            with self.subTest(pin=pin):
                self.assertIn(pin, src)
        self.assertEqual(alphabet(src), ABC)

    def test_the_suffix_set(self):
        valid = valid_suffixes(ABC)
        self.assertEqual(len(valid), 1 << 16, "one suffix per 16-bit value")
        self.assertEqual({s[3] for s in valid}, {"2"}, "65535 < 54^3: the fourth digit is always 0")
        self.assertEqual(max(ABC.index(s[2]) for s in valid), 22, "the third is at most 'R'")
        # the test key's (fp 7916ca48...): 0x7916 = 2 + 34*54 + 10*54^2
        self.assertEqual(suffix16(0x7916, ABC), "4dC2")

    def test_true_examples_pass(self):
        valid = valid_suffixes(ABC)
        for s in MUST_PASS:
            with self.subTest(s=s):
                self.assertEqual(problems_in(s, valid), [])

    def test_impossible_examples_fail(self):
        valid = valid_suffixes(ABC)
        for s in MUST_FAIL:
            with self.subTest(s=s):
                self.assertTrue(problems_in(s, valid), s)


class TheDocs(unittest.TestCase):
    maxDiff = None

    def test_every_wap_name_in_the_docs_is_one_a_wap_can_have(self):
        valid = valid_suffixes(alphabet(ino()))
        files = doc_files(REPO)
        self.assertGreater(len(files), 100, "the doc walk found almost nothing (git ls-files broke?)")
        problems = []
        for rel in files:
            try:
                text = (REPO / rel).read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            for i, line in enumerate(text.split("\n"), 1):
                problems += [f"{rel}:{i}: {p}" for p in problems_in(line, valid)]
        self.assertEqual(problems, [])


class TheSources(unittest.TestCase):
    maxDiff = None

    def test_every_wap_name_in_the_sources_is_one_a_wap_can_have(self):
        valid = valid_suffixes(alphabet(ino()))
        files = source_files(REPO)
        self.assertGreater(len(files), 500, "the source walk found almost nothing (git ls-files broke?)")
        for want in ("firmware/projects/canary-wap/arduino/canary_wap/csi_dashboard_html.h",
                     "firmware/common/encoding/cbor.h", "canary-local/tests/flash.test.js"):
            self.assertIn(want, files, "sweep A35's three files are read")
        problems, used = [], {x: 0 for x in SOURCE_EXEMPT}
        for rel in files:
            try:
                text = (REPO / rel).read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            for i, line in enumerate(text.split("\n"), 1):
                found = problems_in(line, valid)
                if not found:
                    continue
                hit = next((x for x in SOURCE_EXEMPT if x[0] == rel and line.strip() == x[1]), None)
                if hit:
                    used[hit] += 1
                    continue
                problems += [f"{rel}:{i}: {p}" for p in found]
        self.assertEqual(problems, [])
        for x, n in used.items():
            with self.subTest(exempt=x[0]):
                self.assertEqual(n, 1, f"a dead or doubled exemption: {x[1]!r} ({x[2]})")


if __name__ == "__main__":
    unittest.main()
