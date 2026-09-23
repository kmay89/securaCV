#!/usr/bin/env python3
"""The hub plan's broker-TLS names are spelled in the plan, never in the docs.

canary-local/devices/hub_seed.json's `broker-tls` step points the Mosquitto
add-on at two files: its two option names (`certfile`, `keyfile`) and the two
file names they hold (`fullchain.pem`, `privkey.pem`) are spelled in the
step's `what` / `options` / `requires_files` and nowhere else in prose. A doc
that retyped them would go stale the day the add-on renamed one, and
`--dry-run --with broker_tls` narrates them for whoever needs them. So the
docs that ask a reader to act point at the step's flag instead, and this gate
holds them to it: no Markdown file under docs/ (nor README.md) may carry one
of the four names, and at least one must carry the flag.

Why here and not beside the plan's other tests: the input is Markdown
anywhere under docs/. This gate used to be the second half of a test in
canary-local/tools/tests/test_hub_seed_apply.py, which runs only in
canary-local.yml, and that workflow's path filter names a dozen specific docs,
not docs/**. So a docs-only PR that retyped `fullchain.pem` would merge green,
and the first red would land on the next unrelated canary.local PR. Repo Lints
(lint.yml) is unfiltered, so the check runs where its input changes — the
same argument as scripts/lint_bench_rows.py. The plan half (the names ARE in
the step's `what` and `options`) stays in test_hub_seed_apply.py.

The needles are the plan's own spellings, and the gate asserts each one is
still in the step's options, so an add-on rename fails here instead of
leaving a needle that no longer means anything. `keyfile` is matched as a
code token because the NetworkManager keyfile — a different thing — is
prose in the docs. The pointer is read from the plan, not typed here.

Same shape as test_docs_claims.py: MUST_PASS / MUST_FAIL strings guard the
guard, so a needle that stops matching is a red test, not a silent hole.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_hub_plan_prose.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests), unfiltered
"""

from __future__ import annotations

import json
import re
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PLAN = REPO / "canary-local" / "devices" / "hub_seed.json"
STEP_ID = "broker-tls"

# (needle as matched in prose, its bare form in the plan's options). Keep
# these exactly: they are the four names the step spells.
NEEDLES = (
    ("certfile", "certfile"),
    ("`keyfile`", "keyfile"),
    ("fullchain.pem", "fullchain.pem"),
    ("privkey.pem", "privkey.pem"),
)

# Prose that must never trip the gate: the NetworkManager keyfile is a real,
# different thing the hub docs describe in plain words, and the flag itself
# is what the docs are meant to say.
MUST_PASS = [
    "NetworkManager reads the Wi-Fi keyfile from the boot partition",
    "the flasher writes a keyfile named my-network into CONFIG/network",
    "run `sh provision.sh --dry-run --with broker_tls` to see what it checks",
    "put the broker's certificate chain and private key in the ssl folder",
]
# Each needle in the shapes a doc would retype it: backticked, in a command,
# in a table cell, in a path.
MUST_FAIL = [
    "set the add-on's `certfile` option",
    "certfile: fullchain.pem",
    "point `keyfile` at your key",
    "| `/ssl/fullchain.pem` | the certificate chain |",
    "copy it to /ssl/privkey.pem",
]


def load_plan(path: Path = PLAN) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def broker_tls_step(plan: dict) -> dict | None:
    return next((s for s in plan.get("steps", []) if s.get("id") == STEP_ID), None)


def flag_for(step: dict) -> str:
    """The pointer a doc carries instead of the names: `--with <feature>`."""
    return f"--with {step['feature']}"


def retyped_in(text: str) -> list[str]:
    return [needle for needle, _bare in NEEDLES if needle in text]


def prose_files(root: Path) -> list[Path]:
    return sorted((root / "docs").rglob("*.md")) + [root / "README.md"]


def scan(root: Path, flag: str) -> tuple[dict[str, list[str]], list[str]]:
    """({relative path: [needles]} for files that retype a name, [files carrying the flag])."""
    retyped: dict[str, list[str]] = {}
    pointers: list[str] = []
    for md in prose_files(root):
        rel = md.relative_to(root).as_posix()
        text = md.read_text(encoding="utf-8")
        hits = retyped_in(text)
        if hits:
            retyped[rel] = hits
        if flag in text:
            pointers.append(rel)
    return retyped, pointers


class TheGuardItself(unittest.TestCase):
    def test_true_prose_passes(self):
        for s in MUST_PASS:
            with self.subTest(s=s):
                self.assertEqual(retyped_in(s), [])

    def test_every_retyped_name_is_flagged(self):
        for s in MUST_FAIL:
            with self.subTest(s=s):
                self.assertNotEqual(retyped_in(s), [], "the gate lost its teeth")

    def test_every_needle_has_a_must_fail_line(self):
        caught = {n for s in MUST_FAIL for n in retyped_in(s)}
        self.assertEqual(caught, {n for n, _bare in NEEDLES})

    def test_the_scan_names_the_file_and_counts_pointers(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "docs" / "deep").mkdir(parents=True)
            (root / "docs" / "deep" / "retyped.md").write_text(
                "copy it to /ssl/privkey.pem\n", encoding="utf-8")
            (root / "docs" / "clean.md").write_text(
                "NetworkManager's keyfile lands on the boot partition\n", encoding="utf-8")
            (root / "README.md").write_text("nothing here\n", encoding="utf-8")
            retyped, pointers = scan(root, "--with broker_tls")
            self.assertEqual(retyped, {"docs/deep/retyped.md": ["privkey.pem"]})
            self.assertEqual(pointers, [])
            (root / "README.md").write_text(
                "run `sh provision.sh --with broker_tls`\n", encoding="utf-8")
            self.assertEqual(scan(root, "--with broker_tls")[1], ["README.md"])


class ThePlan(unittest.TestCase):
    def setUp(self):
        self.plan = load_plan()
        self.step = broker_tls_step(self.plan)

    def test_the_step_exists(self):
        # A moved or renamed step must fail here, not leave the gate
        # scanning for names nothing defines any more.
        self.assertIsNotNone(self.step, f"{PLAN.relative_to(REPO)} has no {STEP_ID!r} step")

    def test_every_needle_is_spelled_in_the_steps_options(self):
        self.assertIsNotNone(self.step)
        spelled = set(self.step["options"]) | {str(v) for v in self.step["options"].values()}
        for _needle, bare in NEEDLES:
            with self.subTest(name=bare):
                self.assertIn(bare, spelled,
                              "the plan renamed an option or a file; update NEEDLES to its new spelling")

    def test_the_flag_is_the_plans_own(self):
        self.assertIsNotNone(self.step)
        feature = self.step["feature"]
        enable = self.plan["optional_features"][feature]["enable"]
        self.assertIn(flag_for(self.step), enable)
        self.assertRegex(flag_for(self.step), re.compile(r"^--with [a-z_]+$"))


class TheDocuments(unittest.TestCase):
    def setUp(self):
        step = broker_tls_step(load_plan())
        self.assertIsNotNone(step, f"no {STEP_ID!r} step to read the flag from")
        self.flag = flag_for(step)
        self.retyped, self.pointers = scan(REPO, self.flag)

    def test_no_doc_retypes_the_names(self):
        lines = [f"  {rel}: {hits}" for rel, hits in sorted(self.retyped.items())]
        self.assertEqual(
            self.retyped, {},
            "the plan is the one place these names are spelled; point at "
            f"`{self.flag}` instead:\n" + "\n".join(lines),
        )

    def test_a_doc_points_at_the_flag(self):
        self.assertGreater(
            len(self.pointers), 0,
            f"no file under docs/ (nor README.md) carries `{self.flag}`, so "
            "nothing tells a reader how to see the step's names",
        )


if __name__ == "__main__":
    unittest.main()
