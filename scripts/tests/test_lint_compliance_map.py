#!/usr/bin/env python3
"""Tests for scripts/lint_compliance_map.py — the compliance maps' honesty gate.

Every rule the linter enforces has at least one test here, and every message
it prints is pinned as a literal string, so a reworded message is a red test
rather than a silent change to what CI tells an author. Each test builds a
small repository in a temporary directory and runs the linter against it with
--root; nothing touches the network. The first test runs the real tree.

The claim words themselves are never retyped here: the samples come from the
linter's own MUST_FAIL list, so the banned forms stay spelled out in one place
(scripts/lint_compliance_map.py, the AGENTS.md rule 3b practice).

Run:  python3 -m unittest discover -s scripts/tests -p 'test_lint_compliance_map.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""
from __future__ import annotations

import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "lint_compliance_map.py"
sys.path.insert(0, str(REPO / "scripts"))
import lint_compliance_map as L  # noqa: E402

P = "lint_compliance_map: "
MAP = "docs/compliance/maps/fixture.json"

STATUS_BLOCK = {
    "$comment": "fixture",
    "items": [{"id": s, "definition": "d", "requires": []} for s in L.STATUSES],
}

RUST = """\
pub fn append_thing(x: u8) {}
// pub fn commented_only(
pub struct Thing;

#[cfg(test)]
mod tests {
    #[test]
    fn real_test() {}

    #[tokio::test(flavor = "current_thread")]
    async fn async_test() {}

    fn not_a_test() {}
}
"""

FILES = {
    "src/lib.rs": RUST,
    "docs/security/ENTERPRISE_CUSTODY.md": "# EC\n\n## 6. Regulated-market assurance tier\n",
    "docs/runbook.md": "# Runbook\n\n## Step one\n",
    "spec/design.md": "# Design\n\n## Layer 3\n",
    "spec/invariants.md": "# Invariants\n\n## Invariant III\n",
    "docs/design/note.md": "# Note\n",
    "scripts/tests/test_thing.py": "def test_python_form():\n    pass\n",
    "viewer/thing.test.js": "describe('a js describe', () => {});\ntest(\"a js test\", () => {});\n",
    "firmware/x/tests_host/test_y.cpp": "static void test_cpp_form() {}\n",
    "firmware/x/src/not_host.cpp": "static void test_cpp_form() {}\n",
    "scripts/not_a_test_file.py": "def test_python_form():\n    pass\n",
}


def base_map() -> dict:
    return {
        "schema": "securacv-compliance-map:v1",
        "regime": {
            "id": "fixture",
            "title": "Fixture regime",
            "kind": "regulation",
            "determination_by": "the fixture regulator",
            "source": {"citation": "Fixture 1", "access": "blocked", "note": "not fetched"},
        },
        "last_reviewed": "2026-11",
        "preamble": ["A paragraph."],
        "rows": [
            {"id": "s1", "clause": "1.1", "clause_verified": False, "requirement": "r", "mechanism": "m",
             "status": "structural",
             "evidence": [{"kind": "test", "path": "src/lib.rs", "test": "real_test"},
                          {"kind": "code", "path": "src/lib.rs", "symbol": "pub fn append_thing("}]},
            {"id": "p1", "clause": "1.2", "clause_verified": False, "requirement": "r", "mechanism": "m",
             "status": "procedural", "operator_action": "do it",
             "evidence": [{"kind": "procedure", "path": "docs/runbook.md", "anchor": "step-one"}]},
            {"id": "d1", "clause": "1.3", "clause_verified": False, "requirement": "r", "mechanism": "m",
             "status": "designed", "depends_on": ["item3:F"],
             "evidence": [{"kind": "design", "path": "spec/design.md", "anchor": "layer-3"}]},
            {"id": "n1", "clause": "1.4", "clause_verified": False, "requirement": "r", "mechanism": "m",
             "status": "not-applicable",
             "reason": "This requirement addresses a kind of system this is not.",
             "evidence": []},
            {"id": "x1", "clause": "1.5", "clause_verified": False, "requirement": "r", "mechanism": "m",
             "status": "not-pursued",
             "reason": "Deliberately not provided; the decision is recorded in the invariants.",
             "evidence": [{"kind": "decision", "path": "spec/invariants.md", "anchor": "invariant-iii"}]},
            {"id": "k1", "clause": "1.6", "clause_verified": False, "requirement": "r", "mechanism": "m",
             "status": "procedural", "operator_action": "stand in",
             "evidence": [{"kind": "procedure", "path": "docs/runbook.md"},
                          {"kind": "tracker", "path": "docs/security/ENTERPRISE_CUSTODY.md",
                           "anchor": "6-regulated-market-assurance-tier"}]},
        ],
    }


class Tree:
    """A throwaway repository the linter runs against with --root."""

    def __init__(self, testcase: unittest.TestCase):
        self._tmp = tempfile.TemporaryDirectory()
        testcase.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.write("spec/witness_dictionary.json",
                   json.dumps({"known_divergences": {}, "compliance_map_status": STATUS_BLOCK}))
        self.write("docs/compliance/README.md", "# Compliance\n\n" + L.POSTURE + "\n\n## Map schema\n")
        for rel, text in FILES.items():
            self.write(rel, text)
        self.map = base_map()

    def write(self, rel: str, text: str) -> Path:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
        return p

    def write_map(self, data=None, name: str = MAP):
        self.write(name, json.dumps(self.map if data is None else data, indent=2))

    def run(self, write: bool = True):
        if write:
            self.write_map()
        proc = subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root)],
                              capture_output=True, text=True, env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})
        return proc.returncode, proc.stdout, proc.stderr.splitlines()

    def row(self, rid: str) -> dict:
        return next(r for r in self.map["rows"] if r["id"] == rid)


def word_of(sample: str) -> str:
    """The claim word the linter finds in one of its own MUST_FAIL samples."""
    return L.claim_words(sample)[0]


class Base(unittest.TestCase):
    def tree(self) -> Tree:
        return Tree(self)

    def assertFinds(self, tree: Tree, *messages: str, rc: int = 1):
        code, out, err = tree.run(write=False) if getattr(tree, "_written", False) else tree.run()
        for m in messages:
            self.assertIn(P + m, err, f"missing:\n{P + m}\nin:\n" + "\n".join(err))
        self.assertEqual(code, rc, "\n".join(err))
        self.assertEqual(out, "")
        return err

    def assertClean(self, tree: Tree):
        code, out, err = tree.run()
        self.assertEqual((code, err), (0, []), "\n".join(err))
        return out


# ── Repository and self-test ────────────────────────────────────────────────
class RepositoryAndSelfTest(Base):
    def test_repo_maps_and_prose_pass(self):
        proc = subprocess.run([sys.executable, str(SCRIPT)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue(proc.stdout.startswith(P), proc.stdout)
        self.assertTrue(proc.stdout.rstrip().endswith("; no claim words"), proc.stdout)

    def test_must_fail_and_must_pass_samples(self):
        self.assertEqual(L.self_test(), [])
        for s in L.MUST_PASS:
            with self.subTest(s=s):
                self.assertEqual(L.claim_words(s), [])
        for s in L.MUST_FAIL:
            with self.subTest(s=s):
                self.assertNotEqual(L.claim_words(s), [])

    def test_quoted_and_backticked_claim_words_still_fail(self):
        quoted = next(s for s in L.MUST_FAIL if '"' in s)
        ticked = next(s for s in L.MUST_FAIL if "`" in s)
        t = self.tree()
        t.write("docs/compliance/a.md", f"# A\n\n{quoted}\n\n```text\n{ticked}\n```\n")
        w = word_of(quoted)
        self.assertFinds(
            t,
            f'docs/compliance/a.md:3: "{w}" reads as a claim about SecuraCV\'s standing — write "maps to", '
            '"designed to support" or "shaped as"; the determination belongs to the regulator, the court '
            "or the operator",
            f'docs/compliance/a.md:6: "{word_of(ticked)}" reads as a claim about SecuraCV\'s standing — write '
            '"maps to", "designed to support" or "shaped as"; the determination belongs to the regulator, '
            "the court or the operator")

    def test_mask_does_not_launder_a_second_use(self):
        sample = next(s for s in L.MUST_FAIL if "electronic ledger" in s)
        self.assertEqual(len(L.claim_words(sample)), 1)
        t = self.tree()
        t.write("docs/compliance/a.md", f"# A\n\n{sample}\n")
        self.assertFinds(
            t,
            f'docs/compliance/a.md:3: "{word_of(sample)}" reads as a claim about SecuraCV\'s standing — '
            'write "maps to", "designed to support" or "shaped as"; the determination belongs to the '
            "regulator, the court or the operator")

    def test_regime_noun_phrases_pass(self):
        t = self.tree()
        t.write("docs/compliance/a.md", "# A\n\n" + "\n\n".join(L.MUST_PASS) + "\n")
        t.map["rows"][0]["mechanism"] = L.MUST_PASS[0] + "; " + L.MUST_PASS[1]
        self.assertClean(t)

    def test_own_profile_verdict_phrase_passes_and_other_conforms_fails(self):
        verdict = next(s for s in L.MUST_PASS if "witness-profile" in s)
        self.assertEqual(L.claim_words(verdict), [])
        phrase = verdict.split(": ", 1)[1]           # the verb, then "to securacv-c2pa-witness-profile:v1"
        verb = phrase.split()[0]
        negative = "verdict: does not " + verb[:-1] + phrase[len(verb):]
        self.assertEqual(L.claim_words(negative), [])
        other = next(s for s in L.MUST_FAIL if "eIDAS" in s)
        t = self.tree()
        t.map["rows"][0]["mechanism"] = other
        self.assertFinds(
            t,
            f'{MAP}: rows[0].mechanism: "{word_of(other)}" reads as a claim about SecuraCV\'s standing — '
            'write "maps to", "designed to support" or "shaped as"; the determination belongs to the '
            "fixture regulator")

    def test_broken_self_test_is_reported(self):
        # W2: a sample that changes verdict is named, so the patterns never drift silently.
        saved = L.MUST_PASS
        try:
            L.MUST_PASS = saved + [L.MUST_FAIL[0]]
            res = L.run(self.tree().root)
        finally:
            L.MUST_PASS = saved
        msgs = [f.message for f in res.findings if f.rule == "W2"]
        self.assertEqual(msgs, ["the claim-word patterns no longer behave — these MUST_PASS/MUST_FAIL samples "
                                f"changed verdict: {L.MUST_FAIL[0]!r}"])


# ── Schema ──────────────────────────────────────────────────────────────────
class Schema(Base):
    def test_dictionary_block_missing_or_reordered_fails(self):
        msg = ("spec/witness_dictionary.json has no compliance_map_status block, or its ids are not "
               "structural, procedural, designed, not-applicable, not-pursued — the status set is read "
               "from there")
        t = self.tree()
        t.write("spec/witness_dictionary.json", json.dumps({"known_divergences": {}}))
        self.assertFinds(t, msg)
        t2 = self.tree()
        block = copy.deepcopy(STATUS_BLOCK)
        block["items"].reverse()
        t2.write("spec/witness_dictionary.json", json.dumps({"compliance_map_status": block}))
        self.assertFinds(t2, msg)

    def test_bad_json_exits_2(self):
        t = self.tree()
        t.write(MAP, "{ not json")
        t._written = True
        code, out, err = t.run(write=False)
        self.assertEqual(code, 2)
        self.assertEqual(len(err), 1)
        self.assertTrue(err[0].startswith(P + f"{MAP}: not valid JSON ("), err)
        self.assertTrue(err[0].endswith(")"), err)

    def test_schema_tag_wrong(self):
        t = self.tree()
        t.map["schema"] = "securacv-compliance-map:v2"
        self.assertFinds(t, f'{MAP}: "schema" must be "securacv-compliance-map:v1"')

    def test_unknown_key_fails_at_each_level(self):
        t = self.tree()
        t.map["extra"] = 1
        t.map["regime"]["extra"] = 1
        t.map["regime"]["source"]["extra"] = 1
        t.map["rows"][0]["extra"] = 1
        t.map["rows"][0]["evidence"][0]["extra"] = 1
        tail = ' — the map schema is closed (docs/compliance/README.md#map-schema)'
        self.assertFinds(
            t,
            f'{MAP}: top level: unknown key "extra"' + tail,
            f'{MAP}: regime: unknown key "extra"' + tail,
            f'{MAP}: regime.source: unknown key "extra"' + tail,
            f'{MAP}: rows[0]: unknown key "extra"' + tail,
            f'{MAP}: rows[0].evidence[0]: unknown key "extra"' + tail)

    def test_missing_key_fails_at_each_level(self):
        t = self.tree()
        del t.map["last_reviewed"]
        del t.map["regime"]["title"]
        del t.map["regime"]["source"]["note"]
        del t.map["rows"][1]["clause"]
        del t.map["rows"][1]["evidence"][0]["path"]
        self.assertFinds(
            t,
            f'{MAP}: top level: missing key "last_reviewed"',
            f'{MAP}: regime: missing key "title"',
            f'{MAP}: regime.source: missing key "note"',
            f'{MAP}: rows[1]: missing key "clause"',
            f'{MAP}: rows[1].evidence[0]: missing key "path"')

    def test_test_and_symbol_placement(self):
        t = self.tree()
        t.map["rows"][0]["evidence"][1]["test"] = "real_test"
        self.assertFinds(t, f'{MAP}: rows[0].evidence[1]: at most one of "test" and "symbol"',
                         f'{MAP}: rows[0].evidence[1]: "test" is allowed only on evidence of kind "test"')

    def test_length_cap_and_control_character(self):
        t = self.tree()
        t.map["rows"][0]["mechanism"] = "m" * 801
        t.map["rows"][1]["requirement"] = "a‮reversed"
        t.map["rows"][2]["limits"] = "tab\there"
        t.map["regime"]["title"] = "t" * 161
        t.map["preamble"] = ["p" * 1201]
        t.map["rows"][3]["clause_verified"] = "no"
        self.assertFinds(
            t,
            f'{MAP}: rows[0]: "mechanism" exceeds 800 characters',
            f'{MAP}: rows[1]: "requirement" contains a control or text-direction character',
            f'{MAP}: rows[2]: "limits" contains a control or text-direction character',
            f'{MAP}: regime: "title" exceeds 160 characters',
            f'{MAP}: preamble[0]: "preamble" exceeds 1200 characters',
            f'{MAP}: rows[3]: "clause_verified" must be a boolean')

    def test_regime_id_must_equal_stem(self):
        t = self.tree()
        t.map["regime"]["id"] = "other"
        self.assertFinds(t, f'{MAP}: regime id "other" must equal the file stem "fixture"')
        t2 = self.tree()
        t2.map["regime"]["id"] = "Bad-Stem"
        t2.write_map(name="docs/compliance/maps/Bad-Stem.json")
        t2.map = base_map()
        self.assertFinds(t2, 'docs/compliance/maps/Bad-Stem.json: regime id "Bad-Stem" does not match '
                             "^[a-z0-9_]{2,40}$")

    def test_duplicate_row_id(self):
        t = self.tree()
        t.map["rows"][1]["id"] = "s1"
        t.map["rows"][2]["id"] = "-bad id"
        self.assertFinds(t, f"{MAP}:s1: duplicate row id",
                         f"{MAP}:-bad id: row id does not match ^[A-Za-z0-9][A-Za-z0-9().:§ _-]{{0,47}}$")

    def test_last_reviewed_with_a_day_fails(self):
        t = self.tree()
        t.map["last_reviewed"] = "2026-11-08"
        self.assertFinds(t, f"{MAP}: last_reviewed must be a month, YYYY-MM — never a day or a time")

    def test_closed_value_sets(self):
        t = self.tree()
        t.map["regime"]["kind"] = "law"
        t.map["regime"]["source"]["access"] = "read"
        t.map["rows"][0]["alcoa"] = ["Attributable", "Fast"]
        t.map["rows"][1]["evidence"][0]["kind"] = "runbook"
        self.assertFinds(
            t,
            f'{MAP}: regime.kind: "law" is not one of program, regulation, rule-of-evidence, standard, '
            "tracker, vocabulary",
            f'{MAP}: regime.source.access: "read" is not one of blocked, fetched, not-attempted, paywalled',
            f'{MAP}: rows[0].alcoa: "Fast" is not one of Accurate, Attributable, Available, Complete, '
            "Consistent, Contemporaneous, Enduring, Legible, Original",
            f'{MAP}: rows[1].evidence[0].kind: "runbook" is not one of code, decision, design, doc, '
            "procedure, test, tracker")


# ── Statuses ────────────────────────────────────────────────────────────────
V2_TEST = ('{m}:s1: "structural" needs a test evidence entry naming a test function — without one the row '
           'is "designed" or "procedural"')


class Statuses(Base):
    def test_unknown_status(self):
        t = self.tree()
        t.row("s1")["status"] = "gap"
        self.assertFinds(t, f'{MAP}:s1: status "gap" is not one of structural, procedural, designed, '
                            "not-applicable, not-pursued (spec/witness_dictionary.json compliance_map_status)")

    def test_structural_without_test_fails(self):
        t = self.tree()
        del t.row("s1")["evidence"][0]
        self.assertFinds(t, V2_TEST.format(m=MAP))

    def test_structural_without_code_fails(self):
        t = self.tree()
        del t.row("s1")["evidence"][1]
        self.assertFinds(t, f'{MAP}:s1: "structural" needs a code evidence entry naming a symbol')

    def test_structural_with_missing_test_fn_fails(self):
        t = self.tree()
        t.row("s1")["evidence"][0]["test"] = "invented_test"
        self.assertFinds(t, f'{MAP}:s1: "invented_test" is not a test function in src/lib.rs — a structural '
                            "row may only name a test that exists; downgrade the row, never invent a test")

    def test_structural_with_fn_lacking_test_attribute_fails(self):
        t = self.tree()
        t.row("s1")["evidence"][0]["test"] = "not_a_test"
        self.assertFinds(t, f'{MAP}:s1: "not_a_test" is not a test function in src/lib.rs — a structural '
                            "row may only name a test that exists; downgrade the row, never invent a test")
        t2 = self.tree()
        t2.row("s1")["evidence"][0]["test"] = "async_test"
        self.assertClean(t2)

    def test_bare_test_file_without_name_fails(self):
        t = self.tree()
        del t.row("s1")["evidence"][0]["test"]
        self.assertFinds(t, V2_TEST.format(m=MAP),
                         f'{MAP}:s1: a test evidence entry must name its test function ("test") — a bare '
                         "test file proves nothing")

    def test_python_js_and_cpp_host_test_forms_pass(self):
        t = self.tree()
        t.row("s1")["evidence"][0:1] = [
            {"kind": "test", "path": "scripts/tests/test_thing.py", "test": "test_python_form"},
            {"kind": "test", "path": "viewer/thing.test.js", "test": "a js describe"},
            {"kind": "test", "path": "viewer/thing.test.js", "test": "a js test"},
            {"kind": "test", "path": "firmware/x/tests_host/test_y.cpp", "test": "test_cpp_form"},
        ]
        self.assertClean(t)
        t2 = self.tree()
        t2.row("s1")["evidence"][0:1] = [
            {"kind": "test", "path": "scripts/not_a_test_file.py", "test": "test_python_form"},
            {"kind": "test", "path": "firmware/x/src/not_host.cpp", "test": "test_cpp_form"},
        ]
        tail = " — a structural row may only name a test that exists; downgrade the row, never invent a test"
        self.assertFinds(t2,
                         f'{MAP}:s1: "test_python_form" is not a test function in scripts/not_a_test_file.py'
                         + tail,
                         f'{MAP}:s1: "test_cpp_form" is not a test function in firmware/x/src/not_host.cpp'
                         + tail)

    def test_procedural_without_operator_action_fails(self):
        t = self.tree()
        del t.row("p1")["operator_action"]
        msg = (f'{MAP}:p1: "procedural" needs a procedure evidence entry (a runbook, CPS, operator-pack or '
               "guide section) and an operator_action")
        self.assertFinds(t, msg)
        t2 = self.tree()
        t2.row("p1")["evidence"] = []
        self.assertFinds(t2, msg)

    def test_designed_without_depends_on_fails(self):
        t = self.tree()
        del t.row("d1")["depends_on"]
        self.assertFinds(t, f'{MAP}:d1: "designed" needs a design evidence entry (spec/, docs/design/, '
                            "docs/security/ or a docs/compliance design note) and depends_on")

    def test_not_applicable_short_reason_fails(self):
        t = self.tree()
        t.row("n1")["reason"] = "Too short to be a reason."
        self.assertFinds(t, f'{MAP}:n1: "not-applicable" needs a reason of at least 40 characters')

    def test_not_pursued_without_decision_fails(self):
        t = self.tree()
        t.row("x1")["evidence"] = []
        self.assertFinds(t, f'{MAP}:x1: "not-pursued" needs a reason of at least 40 characters and a decision '
                            "evidence entry naming where the decision is recorded")

    def test_tracker_must_point_at_enterprise_custody(self):
        t = self.tree()
        t.row("k1")["evidence"][1]["path"] = "docs/runbook.md"
        t.row("k1")["evidence"][1]["anchor"] = "step-one"
        msg = f"{MAP}:k1: a tracker entry must point at docs/security/ENTERPRISE_CUSTODY.md#<anchor>"
        self.assertFinds(t, msg)
        t2 = self.tree()
        del t2.row("k1")["evidence"][1]["anchor"]
        self.assertFinds(t2, msg)


# ── Evidence ────────────────────────────────────────────────────────────────
class Evidence(Base):
    def test_missing_path_fails(self):
        t = self.tree()
        t.row("p1")["evidence"][0]["path"] = "docs/gone.md"
        self.assertFinds(t, f"{MAP}:p1: evidence docs/gone.md does not exist — a map may only point at what "
                            "is on disk")

    def test_path_escape_fails(self):
        t = self.tree()
        outside = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(outside, ignore_errors=True))
        (outside / "x.md").write_text("# X\n")
        (t.root / "docs" / "link").symlink_to(outside)
        t.row("p1")["evidence"] = [{"kind": "procedure", "path": "../etc/passwd"},
                                   {"kind": "procedure", "path": "/etc/passwd"},
                                   {"kind": "procedure", "path": "docs/link/x.md"}]
        self.assertFinds(t, f"{MAP}:p1: evidence path ../etc/passwd escapes the repository",
                         f"{MAP}:p1: evidence path /etc/passwd escapes the repository",
                         f"{MAP}:p1: evidence path docs/link/x.md escapes the repository")

    def test_symbol_gone_fails(self):
        t = self.tree()
        t.row("s1")["evidence"][1]["symbol"] = "pub fn append_thin("
        msg = (f'{MAP}:s1: src/lib.rs no longer contains "pub fn append_thin(" — the mechanism the row names '
               "moved or regressed; fix the row, or restore the mechanism")
        self.assertFinds(t, msg)
        t2 = self.tree()  # word boundaries: a prefix of a longer name is not the name
        t2.row("s1")["evidence"][1]["symbol"] = "pub struct Thin"
        self.assertFinds(t2, msg.replace("pub fn append_thin(", "pub struct Thin"))

    def test_symbol_only_in_a_comment_fails(self):
        t = self.tree()
        t.row("s1")["evidence"][1]["symbol"] = "pub fn commented_only("
        self.assertFinds(t, f'{MAP}:s1: src/lib.rs no longer contains "pub fn commented_only(" — the mechanism '
                            "the row names moved or regressed; fix the row, or restore the mechanism")

    def test_missing_anchor_fails(self):
        t = self.tree()
        t.row("p1")["evidence"][0]["anchor"] = "step-two"
        self.assertFinds(t, f'{MAP}:p1: anchor "#step-two" is not a heading in docs/runbook.md')

    def test_design_kind_outside_allowed_trees_fails(self):
        t = self.tree()
        t.row("d1")["evidence"][0] = {"kind": "design", "path": "src/lib.rs", "symbol": "pub struct Thing"}
        t.row("p1")["evidence"][0] = {"kind": "procedure", "path": "spec/design.md"}
        self.assertFinds(t, f'{MAP}:d1: evidence of kind "design" cannot live at src/lib.rs',
                         f'{MAP}:p1: evidence of kind "procedure" cannot live at spec/design.md')
        t2 = self.tree()  # the allowed homes, including a docs/compliance design note
        t2.write("docs/compliance/NOTE.md", "# Note\n")
        t2.row("d1")["evidence"] = [{"kind": "design", "path": "docs/compliance/NOTE.md"},
                                    {"kind": "design", "path": "docs/design/note.md"},
                                    {"kind": "design", "path": "docs/security/ENTERPRISE_CUSTODY.md"}]
        self.assertClean(t2)

    def test_unknown_depends_on_fails(self):
        t = self.tree()
        t.row("d1")["depends_on"] = ["item3:F", "item9:Z"]
        self.assertFinds(t, f'{MAP}:d1: depends_on "item9:Z" is not a roadmap PR id this linter knows')


# ── Rules ───────────────────────────────────────────────────────────────────
class Rules(Base):
    def test_clause_verified_from_unfetched_source_fails(self):
        t = self.tree()
        t.row("s1")["clause_verified"] = True
        self.assertFinds(t, f'{MAP}:s1: the regime source was not fetched, so its clause cannot be '
                            '"clause_verified": true')
        t2 = self.tree()
        t2.map["regime"]["source"]["access"] = "fetched"
        t2.row("s1")["clause_verified"] = True
        self.assertClean(t2)

    def test_bs10008_clause_number_fails(self):
        t = self.tree()
        bs = base_map()
        bs["regime"]["id"] = "bs10008"
        bs["rows"][0]["clause"] = "BS 10008 6.2.1"
        bs["rows"][1]["clause"] = "BS 10008 (topic; clause number not cited — standard not read)"
        t.write_map(bs, name="docs/compliance/maps/bs10008.json")
        self.assertFinds(t, "docs/compliance/maps/bs10008.json:s1: a BS 10008 clause number appears, but the "
                            "standard was not read — cite the topic only")

    def test_result_number_in_json_fails(self):
        t = self.tree()
        t.row("s1")["limits"] = "detects 97 % of crossings"
        t.row("p1")["mechanism"] = "precision: 0.9 on the set"
        msg = "a validation result belongs in the cited report, not in the tree (AGENTS.md rule 4)"
        self.assertFinds(t, f"{MAP}: rows[0].limits: {msg}", f"{MAP}: rows[1].mechanism: {msg}")

    def test_readme_posture_paragraph_pinned(self):
        msg = ("docs/compliance/README.md must open with the posture paragraph verbatim (pinned in "
               "scripts/lint_compliance_map.py)")
        t = self.tree()
        t.write("docs/compliance/README.md", "# Compliance\n\nSomething else first.\n\n" + L.POSTURE + "\n")
        self.assertFinds(t, msg)
        t2 = self.tree()
        (t2.root / "docs/compliance/README.md").unlink()
        self.assertFinds(t2, msg)
        t3 = self.tree()  # reflowed whitespace is the same paragraph
        t3.write("docs/compliance/README.md", "# Compliance\n\n" + L.POSTURE.replace(" ", "\n", 7) + "\n")
        self.assertClean(t3)

    def test_ok_line_counts(self):
        t = self.tree()
        out = self.assertClean(t)
        self.assertEqual(out, P + "1 map(s), 6 row(s), 1 prose file(s): 1 structural, 2 procedural, 1 designed, "
                              "1 not-applicable, 1 not-pursued; no claim words\n")
        t.write("docs/compliance/templates/x.template.md", "# T\n")
        t.write("docs/compliance/templates/x.template.json", "{}\n")
        out = self.assertClean(t)
        self.assertIn(" 3 prose file(s): ", out)


if __name__ == "__main__":
    unittest.main()
