#!/usr/bin/env python3
"""Pins firmware/scripts/flavor_envs.py — the one place the release workflows
get the Canary Display env list from.

What is pinned and why:
  • the release build ORDER: named core-dir groups first, then the default
    core dir, then isolated envs — a PLATFORMIO_CORE_DIR switch changes the
    project checksum and wipes .pio/build, so this order is load-bearing;
  • the current flavors.json validates (release_envs ⊆ build_envs, each with
    a flasher product) and the current workflows name only declared envs —
    the lint must be green on the tree it ships in;
  • the lint actually FAILS on a typo'd env and on a release env CI never
    builds — a guard that reads as covered while catching nothing is worse
    than no guard;
  • the release workflows DERIVE the list (`--release --json` emits it, in
    release order, and both workflows run that step), and the lint fails a
    workflow that types a `pio run -e canary-display-<env>` back in.
  • the build matrix firmware.yml gets from `--build-matrix`: a product's
    `shards` expand to one leg each, in build order, with only that leg's
    size guards; every display env lands in exactly one leg; and the
    validation refuses the mistakes that would silently drop an env from CI.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "firmware" / "scripts" / "flavor_envs.py"

spec = importlib.util.spec_from_file_location("flavor_envs", SCRIPT)
fe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fe)  # type: ignore[union-attr]


def display_entry(core_dir_groups, isolated, build, release, shards=None,
                  size_guards=None):
    entry = {
        "name": "canary-display",
        "build_envs": build,
        "release_envs": release,
        "core_dir_groups": core_dir_groups,
        "isolated_core_envs": isolated,
    }
    if shards is not None:
        entry["shards"] = shards
    if size_guards is not None:
        entry["size_guards"] = size_guards
    return entry


def guard(env):
    return {"bin": f".pio/build/{env}/firmware.bin", "slot_bytes": 1,
            "slot_name": "test"}


class ReleaseOrder(unittest.TestCase):
    def test_groups_then_default_then_isolated_in_build_order(self):
        entry = display_entry(
            {"canary-display-a": "pio3", "canary-display-b": "pio3"},
            ["canary-display-e"],
            ["canary-display-c", "canary-display-e", "canary-display-a",
             "canary-display-d", "canary-display-b"],
            # deliberately shuffled: the JSON order must not leak through
            ["canary-display-e", "canary-display-d", "canary-display-b",
             "canary-display-c", "canary-display-a"],
        )
        self.assertEqual(
            fe.ordered_release_envs(entry),
            ["canary-display-a", "canary-display-b",   # named group, build order
             "canary-display-c", "canary-display-d",   # default core, build order
             "canary-display-e"],                       # isolated last
        )
        self.assertEqual(fe.core_of(entry, "canary-display-a"), "pio3")
        self.assertEqual(fe.core_of(entry, "canary-display-c"), "default")
        self.assertEqual(fe.core_of(entry, "canary-display-e"), "isolated")

    def test_current_tree_release_order_matches_the_release_steps(self):
        # The order the release workflows always built in — the pioarduino
        # core-3 pair first, the espressif32 envs next, the C6 last.
        entry = fe.find_product(fe.load_flavors(), "canary-display")
        self.assertEqual(
            fe.ordered_release_envs(entry),
            ["canary-display-dash7", "canary-display-nightstand7",
             "canary-display-nightstand-s3", "canary-display-touch169",
             "canary-display-amoled241", "canary-display-nightlight-c3",
             "canary-display-nightstand-c6"],
        )

    def test_missing_release_envs_is_an_error_not_an_empty_list(self):
        entry = display_entry({}, [], ["canary-display-a"], None)
        del entry["release_envs"]
        with self.assertRaises(SystemExit):
            fe.ordered_release_envs(entry)


class JsonMatrix(unittest.TestCase):
    def test_json_is_the_release_list_in_release_order_with_core_words(self):
        entry = fe.find_product(fe.load_flavors(), "canary-display")
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(fe.main(["canary-display", "--release", "--json"]), 0)
        rows = json.loads(out.getvalue())
        self.assertEqual([r["env"] for r in rows], fe.ordered_release_envs(entry))
        for r in rows:
            self.assertEqual(sorted(r), ["core", "env", "short"])
            self.assertEqual(r["env"], f"canary-display-{r['short']}")
            self.assertEqual(r["core"], fe.core_of(entry, r["env"]))
        # The one thing the workflows' `case` needs: the C6 is `isolated`,
        # the pioarduino pair carries its group label, the rest `default`.
        cores = {r["short"]: r["core"] for r in rows}
        self.assertEqual(cores["nightstand-c6"], "isolated")
        self.assertEqual(cores["dash7"], cores["nightstand7"])
        self.assertNotIn(cores["dash7"], ("default", "isolated"))
        self.assertEqual(cores["amoled241"], "default")

    def test_flavors_path_override_reads_that_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            alt = Path(tmp) / "flavors.json"
            alt.write_text(json.dumps([display_entry(
                {}, [], ["canary-display-zz"], ["canary-display-zz"])]), encoding="utf-8")
            with redirect_stdout(io.StringIO()) as out:
                self.assertEqual(
                    fe.main(["--flavors", str(alt), "canary-display", "--release", "--json"]), 0)
        self.assertEqual(json.loads(out.getvalue()),
                         [{"env": "canary-display-zz", "short": "zz", "core": "default"}])
class BuildMatrixShards(unittest.TestCase):
    def test_unsharded_product_is_one_leg_with_its_own_name(self):
        entry = {"name": "canary-wap", "dir": "x", "build_envs": ["a", "b"],
                 "size_guards": [guard("a")]}
        legs = fe.build_legs(entry)
        self.assertEqual(len(legs), 1)
        self.assertEqual((legs[0]["leg"], legs[0]["shard"], legs[0]["cache_name"]),
                         ("canary-wap", "", "canary-wap"))
        self.assertEqual(legs[0]["build_envs"], ["a", "b"])
        self.assertEqual(legs[0]["size_guards"], [guard("a")])

    def test_shards_expand_in_build_order_with_only_their_guards(self):
        entry = display_entry(
            {"canary-display-a1": "pio3", "canary-display-a3": "pio3"}, [],
            ["canary-display-a1", "canary-display-a2", "canary-display-a3"],
            [],
            # the shard lists a3 before a1 on purpose: build order must win
            shards={"big": ["canary-display-a3", "canary-display-a1"],
                    "small": ["canary-display-a2"]},
            size_guards=[guard("canary-display-a2"), guard("canary-display-a1"),
                         # a guard for an env CI never builds (release-time
                         # only, like canary's release_ha) lands in no leg
                         guard("canary-display-zz")],
        )
        legs = fe.build_legs(entry)
        self.assertEqual([leg["leg"] for leg in legs],
                         ["canary-display/big", "canary-display/small"])
        self.assertEqual(legs[0]["build_envs"], ["canary-display-a1", "canary-display-a3"])
        self.assertEqual(legs[0]["size_guards"], [guard("canary-display-a1")])
        self.assertEqual(legs[1]["build_envs"], ["canary-display-a2"])
        self.assertEqual(legs[1]["size_guards"], [guard("canary-display-a2")])
        self.assertEqual(legs[0]["cache_name"], "canary-display-shard-big")
        self.assertNotIn("shards", legs[0])
        # everything else the build loop reads rides along unchanged
        self.assertEqual(legs[0]["core_dir_groups"], entry["core_dir_groups"])

    def test_validation_refuses_the_ways_a_shard_list_drops_or_doubles_an_env(self):
        base = dict(core_dir_groups={"canary-display-a": "pio3"}, isolated=[],
                    build=["canary-display-a", "canary-display-b"], release=[])
        cases = {
            "in no shard": {"only": ["canary-display-a"]},
            "exactly one leg": {"x": ["canary-display-a", "canary-display-b"],
                                "y": ["canary-display-b"]},
            "not in build_envs": {"x": ["canary-display-a", "canary-display-b",
                                        "canary-display-zz"]},
            "mixes PLATFORMIO_CORE_DIR": {"x": ["canary-display-a", "canary-display-b"]},
            "must match": {"Bad_Label": ["canary-display-a", "canary-display-b"]},
            "is empty": {"x": ["canary-display-a", "canary-display-b"], "e": []},
        }
        for needle, shards in cases.items():
            problems = fe.validate([display_entry(shards=shards, **base)])
            self.assertTrue(any(needle in p for p in problems), (needle, problems))
        # and a correct partition along the core line is clean
        ok = fe.validate([display_entry(shards={"x": ["canary-display-a"],
                                                "y": ["canary-display-b"]}, **base)])
        self.assertEqual(ok, [])

    def test_current_tree_display_shards_cover_every_env_once(self):
        entry = fe.find_product(fe.load_flavors(), "canary-display")
        legs = fe.build_legs(entry)
        built = [env for leg in legs for env in leg["build_envs"]]
        self.assertEqual(sorted(built), sorted(entry["build_envs"]))
        self.assertEqual(len(built), len(set(built)))
        for leg in legs:
            classes = {fe.core_of(entry, e) for e in leg["build_envs"]}
            self.assertEqual(len(classes), 1, (leg["leg"], classes))
        # every guard fires in the leg that builds its bin, and each guarded
        # env is guarded exactly once across the matrix
        guarded = [fe.guard_env(g) for leg in legs for g in leg["size_guards"]]
        expected = [fe.guard_env(g) for g in entry["size_guards"]]
        self.assertEqual(sorted(guarded), sorted(expected))
        for leg in legs:
            for g in leg["size_guards"]:
                self.assertIn(fe.guard_env(g), leg["build_envs"])

    def test_cli_build_matrix_is_one_line_of_json(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.assertEqual(fe.main(["--build-matrix"]), 0)
        out = buf.getvalue()
        self.assertEqual(out.count("\n"), 1)
        legs = json.loads(out)
        self.assertEqual(legs, fe.build_matrix(fe.load_flavors()))
        self.assertGreater(len(legs), len(fe.load_flavors()))


class CurrentTreeIsGreen(unittest.TestCase):
    def test_flavors_json_validates(self):
        self.assertEqual(fe.validate(fe.load_flavors()), [])

    def test_workflows_name_only_declared_envs(self):
        self.assertEqual(fe.check_workflows(fe.load_flavors()), [])

    def test_release_workflows_derive_the_list(self):
        self.assertEqual(fe.check_release_workflows(), [])
        for name in fe.RELEASE_WORKFLOWS:
            text = (fe.WORKFLOWS / name).read_text(encoding="utf-8")
            self.assertIn("flavor_envs.py", text)
            self.assertIn("--release --json", text)
            self.assertIn("fromJSON(steps.display_envs.outputs.matrix)", text)

    def test_cli_check_mode_exit_code(self):
        with redirect_stdout(io.StringIO()):
            self.assertEqual(fe.main(["--check-workflows"]), 0)


class LintCatchesRealMistakes(unittest.TestCase):
    def test_release_env_ci_never_builds_is_rejected(self):
        entry = display_entry({}, [], ["canary-display-a"],
                              ["canary-display-a", "canary-display-zz"])
        problems = fe.validate([entry])
        self.assertTrue(any("canary-display-zz" in p and "not in build_envs" in p
                            for p in problems), problems)

    def test_release_env_without_flasher_product_is_rejected(self):
        # An env CI builds but the flasher catalog has never heard of.
        entry = display_entry({}, [], ["canary-display-playground"],
                              ["canary-display-playground"])
        problems = fe.validate([entry])
        self.assertTrue(any("securacv-canary-display-playground" in p
                            for p in problems), problems)

    def test_release_workflow_that_types_the_list_is_flagged(self):
        with tempfile.TemporaryDirectory() as tmp:
            good = Path(tmp) / "firmware-release.yml"
            good.write_text(
                "run: |\n"
                "  MATRIX=$(python3 firmware/scripts/flavor_envs.py canary-display --release --json)\n"
                "  while read -r E CORE; do pio run -e \"$E\"; done\n",
                encoding="utf-8")
            bad = Path(tmp) / "flasher-release.yml"
            bad.write_text(
                "run: |\n"
                "  pio run -e canary-display-dash7\n"
                "  pio run -e \"canary-display-nightstand-c6\"\n",
                encoding="utf-8")
            problems = fe.check_release_workflows(Path(tmp))
        self.assertEqual(len(problems), 3, problems)
        self.assertTrue(all(p.startswith("flasher-release.yml") for p in problems), problems)
        self.assertTrue(any("does not derive" in p for p in problems), problems)
        self.assertTrue(any(":2:" in p and "canary-display-dash7" in p for p in problems), problems)
        self.assertTrue(any(":3:" in p and "nightstand-c6" in p for p in problems), problems)

    def test_missing_release_workflow_is_flagged(self):
        with tempfile.TemporaryDirectory() as tmp:
            problems = fe.check_release_workflows(Path(tmp))
        self.assertEqual(len(problems), len(fe.RELEASE_WORKFLOWS), problems)

    def test_typoed_env_in_a_workflow_is_flagged_and_templates_are_not(self):
        with tempfile.TemporaryDirectory() as tmp:
            wf = Path(tmp) / "x.yml"
            wf.write_text(
                "run: |\n"
                "  pio run -e canary-display-amoled214\n"       # typo
                "  cp .pio/build/canary-display-${E} out/\n"    # template: fine
                "  ls canary-display-dash7-${VERSION}.bin\n"    # trailing hyphen: fine
                "  echo securacv-canary-display-nightstand-c6\n",  # product id: fine
                encoding="utf-8",
            )
            saved = fe.WORKFLOWS
            fe.WORKFLOWS = Path(tmp)
            try:
                problems = fe.check_workflows(fe.load_flavors())
            finally:
                fe.WORKFLOWS = saved
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("canary-display-amoled214", problems[0])
        self.assertIn("x.yml:2", problems[0])


if __name__ == "__main__":
    sys.exit(unittest.main())
