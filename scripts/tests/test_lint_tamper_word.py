"""scripts/lint_tamper_word.py — the pattern, the allowlist, and the gate.

The record is tamper-EVIDENT. For a year a Canary's own boot banner, its
console greeting, three Lab generators and six compiled emulator bundles said
the "-proof" form, while the website and the HACS store page were held to the
right word by their own tests. This suite holds the gate that closed that:

- the pattern catches every spelling of the stem (hyphen, space, underscore,
  none, any letter case, a suffix) and nothing that merely has both words
  near each other;
- the lint's own MUST_PASS / MUST_FAIL lists get the verdicts they claim, and
  a pattern loosened or tightened past them stops the run with exit 2;
- the committed tree is clean;
- a file holding the form fails, by name and line, and a file holding the
  right word passes;
- the files a device's words come from are read: the boot banner, the console
  greeting, and the Lab generators that restate them — so "it is allowlisted"
  can never be why one of those is green;
- every ALLOW_FILES entry still exists and still holds the word, and an entry
  gone stale fails the full run (an exemption that outlives its reason is the
  hole the next claim walks through);
- lint.yml runs it.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "lint_tamper_word.py"
spec = importlib.util.spec_from_file_location("lint_tamper_word", SCRIPT)
L = importlib.util.module_from_spec(spec)
spec.loader.exec_module(L)  # type: ignore[union-attr]

WORD = "tamper" + "-" + "proof"

# Where a device's own words are written. None may ever be exempt.
DEVICE_COPY = [
    "firmware/common/boot/boot_banner.cpp",
    "firmware/common/ui/console_scenes.h",
    "firmware/projects/canary-wap/arduino/canary_wap/boot_banner.cpp",
    "firmware/projects/canary-display/arduino/canary_display/boot_banner.cpp",
    "canary-local/tools/gen_wap.py",
    "canary-local/tools/gen_sense.py",
    "canary-local/tools/gen_vision.py",
    "canary-local/devices/wap.json",
    "canary-local/devices/sense.json",
    "canary-local/devices/vision.json",
]


def run_lint(*paths: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(SCRIPT), *map(str, paths)],
                          capture_output=True, text=True)


def call_main(argv: list[str]) -> tuple[int, str]:
    out, err = io.StringIO(), io.StringIO()
    with mock.patch.object(sys, "argv", ["lint_tamper_word.py", *argv]), \
            redirect_stdout(out), redirect_stderr(err):
        code = L.main()
    return code, out.getvalue() + err.getvalue()


class Pattern(unittest.TestCase):
    def test_every_spelling_of_the_stem_is_caught(self):
        for s in (WORD, WORD.upper(), WORD.title(), "tamper" + "proof",
                  "tamper " + "proof", "tamper_" + "proof", "Tamper" + "Proof",
                  WORD + "ing", "a " + WORD + " log", "TAMPER_" + "PROOF_MODE"):
            self.assertRegex(s, L.BANNED, s)

    def test_the_right_word_and_its_neighbors_pass(self):
        for s in ("tamper-evident", "tamper evident", "tamper_evident",
                  "tamper detection", "proof that tampering was caught",
                  "tamper GPIO; proof of custody", "tampering, proof",
                  "tamper\nproofread"[:6], "waterproof", "proof-of-work"):
            self.assertNotRegex(s, L.BANNED, s)

    def test_the_lints_own_lists_get_the_verdicts_they_claim(self):
        self.assertGreaterEqual(len(L.MUST_PASS), 5)
        self.assertGreaterEqual(len(L.MUST_FAIL), 5)
        for s in L.MUST_PASS:
            self.assertNotRegex(s, L.BANNED, s)
        for s in L.MUST_FAIL:
            self.assertRegex(s, L.BANNED, s)

    def test_a_pattern_that_stops_matching_stops_the_run(self):
        import re
        with mock.patch.object(L, "BANNED", re.compile(r"never-matches-anything")):
            code, text = call_main([])
        self.assertEqual(code, 2, text)
        self.assertIn("self-test", text)

    def test_a_pattern_that_eats_the_right_word_stops_the_run(self):
        import re
        with mock.patch.object(L, "BANNED", re.compile(r"tamper", re.I)):
            code, text = call_main([])
        self.assertEqual(code, 2, text)


class Gate(unittest.TestCase):
    def test_the_committed_tree_is_clean(self):
        r = run_lint()
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("tamper word OK", r.stdout)

    def test_a_file_holding_the_form_fails_by_name_and_line(self):
        with tempfile.TemporaryDirectory() as d:
            bad = Path(d) / "banner.cpp"
            bad.write_text('out("hello\\n");\nout("It creates ' + WORD
                           + ' records\\n");\n', encoding="utf-8")
            r = run_lint(bad)
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn(f"{bad}:2", r.stderr)
            self.assertIn("tamper-EVIDENT", r.stderr)

    def test_a_directory_is_walked_and_every_text_kind_is_read(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            names = ["a.md", "b.html", "c.json", "d.h", "e.ino", "f.yaml",
                     "g.swift", "h.txt", "i.js", "j.rs", "k.py"]
            for n in names:
                (root / n).write_text("x\n" + WORD + "\n", encoding="utf-8")
            (root / "ok.md").write_text("a tamper-evident record\n", encoding="utf-8")
            r = run_lint(root)
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            for n in names:
                self.assertIn(f"{root / n}:2", r.stderr, n)
            self.assertNotIn("ok.md", r.stderr)

    def test_the_right_word_passes(self):
        with tempfile.TemporaryDirectory() as d:
            good = Path(d) / "banner.cpp"
            good.write_text('out("It keeps tamper-evident records\\n");\n',
                            encoding="utf-8")
            r = run_lint(good)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_a_path_that_does_not_exist_is_an_error_not_a_pass(self):
        r = run_lint(REPO / "no" / "such" / "file.md")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)


class DeviceCopyIsRead(unittest.TestCase):
    def test_no_device_string_source_is_exempt(self):
        walked = {f.relative_to(L.ROOT).as_posix() for f in L.walk([L.ROOT])}
        for rel in DEVICE_COPY:
            self.assertTrue((REPO / rel).is_file(), f"{rel} is gone — update DEVICE_COPY")
            self.assertIn(rel, walked, f"{rel} is not read by the lint")
            self.assertNotIn(rel, L.ALLOW_FILES, f"{rel} must never be allowlisted")

    def test_the_banner_and_the_greeting_say_the_right_word(self):
        for rel in DEVICE_COPY[:2]:
            text = (REPO / rel).read_text(encoding="utf-8")
            self.assertIn("tamper-evident", text, rel)
            self.assertNotRegex(text, L.BANNED, rel)


class Allowlist(unittest.TestCase):
    def test_every_entry_exists_and_still_holds_the_word(self):
        for rel in sorted(L.ALLOW_FILES):
            path = REPO / rel
            self.assertTrue(path.is_file(), f"{rel} is on ALLOW_FILES and is gone")
            self.assertRegex(path.read_text(encoding="utf-8"), L.BANNED,
                             f"{rel} no longer holds the word — drop it from ALLOW_FILES")

    def test_only_docs_tests_and_the_other_claim_lint_are_exempt(self):
        # scripts/lint_compliance_map.py bans the same word in the compliance
        # suite: it is in that lint's pattern and its must-fail self-test.
        other_lints = {"scripts/lint_compliance_map.py"}
        for rel in L.ALLOW_FILES:
            self.assertTrue(
                rel.startswith("docs/") or "/tests/" in rel or rel in other_lints,
                f"{rel}: only a doc that states the rule or quotes a finding, "
                "or a test or lint asserting the word's absence, may be exempt")
        for rel in other_lints:
            text = (REPO / rel).read_text(encoding="utf-8")
            self.assertIn("MUST_FAIL", text, f"{rel} no longer self-tests the word")

    def test_a_stale_entry_fails_the_full_run(self):
        with mock.patch.object(L, "ALLOW_FILES", set(L.ALLOW_FILES) | {"docs/README.md"}):
            code, text = call_main([])
        self.assertEqual(code, 1, text)
        self.assertIn("docs/README.md", text)

    def test_a_missing_entry_fails_the_full_run(self):
        with mock.patch.object(L, "ALLOW_FILES", set(L.ALLOW_FILES) | {"docs/no_such_doc.md"}):
            code, text = call_main([])
        self.assertEqual(code, 1, text)
        self.assertIn("docs/no_such_doc.md", text)


class Wiring(unittest.TestCase):
    def test_lint_yml_runs_it(self):
        workflow = (REPO / ".github" / "workflows" / "lint.yml").read_text(encoding="utf-8")
        self.assertIn("run: python3 scripts/lint_tamper_word.py", workflow)

    def test_the_two_files_that_name_the_word_skip_themselves(self):
        self.assertEqual(L.SKIP_FILES, {"lint_tamper_word.py", "test_lint_tamper_word.py"})


if __name__ == "__main__":
    unittest.main()
