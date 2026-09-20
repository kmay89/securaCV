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
  • a size guard on an env PR CI never builds is refused for EVERY product,
    sharded or not — firmware.yml fires a guard only right after `pio run
    -e` of the env its bin names, so such a guard measures nothing on a PR
    and first fires on the release artifact (canary's release_ha, #1567 to
    #1686). The check used to sit inside the `shards` branch and so missed
    the four unsharded products; the pre-wave-6 canary entry is the fixture.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
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
                         # only) lands in no leg
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

    def test_every_guarded_image_is_built_by_pr_ci(self):
        # validate() now refuses this structurally for every product (see
        # SizeGuardsMustBePrBuilt), so test_flavors_json_validates already
        # covers the committed manifest. This is the belt to that: it pins
        # the manifest directly, in words that name the guard, so a future
        # loosening of the validator cannot quietly let a release-only guard
        # back in. canary's release_ha (the published OTA image) sat outside
        # build_envs from #1567 until wave 6.
        for entry in fe.load_flavors():
            built = set(entry.get("build_envs") or [])
            for g in entry.get("size_guards") or []:
                env = fe.guard_env(g)
                self.assertIn(env, built,
                              f"{entry['name']}: size guard for {g['bin']} names an "
                              f"env PR CI does not build — it would fire only at "
                              f"release time")


class SizeGuardsMustBePrBuilt(unittest.TestCase):
    """validate() refuses a size guard on an env PR CI never builds, for
    EVERY product. firmware.yml fires a guard only in the build-loop
    iteration that just ran `pio run -e` of the env its bin names, so a
    guard on an env outside build_envs measures nothing on any PR and first
    fires in check_slot_budget.py on the release artifact. The check used to
    live inside validate()'s `shards` branch, so it caught the display and
    missed the four unsharded products — canary's release_ha sat exactly
    that way from #1567 until wave 6 added the env to build_envs."""

    def test_unsharded_guard_on_unbuilt_env_is_rejected(self):
        entry = {"name": "canary", "dir": "firmware/canary",
                 "build_envs": ["release"],
                 "size_guards": [guard("release"), guard("release_ha")]}
        problems = fe.validate([entry])
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("release_ha", problems[0])
        self.assertIn("not in build_envs", problems[0])
        # the guard on the built env alone is clean
        entry["size_guards"] = [guard("release")]
        self.assertEqual(fe.validate([entry]), [])

    def test_sharded_guard_on_unbuilt_env_is_rejected_once(self):
        # the loop MOVED out of the shards branch — a copy left behind would
        # make a sharded product report the same guard twice
        entry = display_entry({}, [], ["canary-display-a"], [],
                              shards={"x": ["canary-display-a"]},
                              size_guards=[guard("canary-display-zz")])
        problems = fe.validate([entry])
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("canary-display-zz", problems[0])
        self.assertIn("not in build_envs", problems[0])

    def test_guard_on_built_env_is_clean_sharded_and_unsharded(self):
        unsharded = {"name": "canary-sense", "dir": "x", "build_envs": ["a", "b"],
                     "size_guards": [guard("a"), guard("b")]}
        self.assertEqual(fe.validate([unsharded]), [])
        sharded = display_entry({}, [], ["canary-display-a", "canary-display-b"], [],
                                shards={"x": ["canary-display-a"],
                                        "y": ["canary-display-b"]},
                                size_guards=[guard("canary-display-b")])
        self.assertEqual(fe.validate([sharded]), [])

    def test_unparseable_guard_bin_is_rejected_for_an_unsharded_product(self):
        # neither firmware.yml nor check_slot_budget.py can key this to an
        # env, so it would never fire anywhere — not on a PR, not on a tag
        entry = {"name": "canary-wap", "dir": "x", "build_envs": ["a"],
                 "size_guards": [{"bin": "firmware.bin", "slot_bytes": 1}]}
        problems = fe.validate([entry])
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("does not sit under .pio/build/<env>/", problems[0])
        self.assertIn("'firmware.bin'", problems[0])

    def test_two_guards_for_one_env_are_rejected(self):
        # check_slot_budget.py refuses two that disagree, but only when a
        # release is being cut; the manifest should say one budget per env
        entry = {"name": "canary-wap", "dir": "x", "build_envs": ["a"],
                 "size_guards": [guard("a"), guard("a")]}
        problems = fe.validate([entry])
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("two size_guards entries", problems[0])
        self.assertIn("'a'", problems[0])

    def test_pre_wave6_canary_shape_is_rejected(self):
        # firmware/flavors.json's canary entry at b43e823 (the #1567 shape,
        # in force until #1686), verbatim: release_ha guarded, release_ha
        # not built. validate() returned [] on it for that whole span — the
        # slot_name even admits the guard fires "at release time".
        entry = {
            "name": "canary",
            "dir": "firmware/canary",
            "build_envs": ["dev", "release", "full", "esp32cam",
                           "esp32-wroom", "freenove-s3"],
            "check_env": "release",
            "pip_extras": "intelhex",
            "isolated_core_envs": ["full"],
            "size_guards": [
                {"bin": ".pio/build/release/firmware.bin",
                 "slot_bytes": 1966080,
                 "slot_name": "0x1E0000 OTA slot (partitions_ota.csv ota_0/ota_1)"},
                {"bin": ".pio/build/release_ha/firmware.bin",
                 "slot_bytes": 1966080,
                 "slot_name": "0x1E0000 OTA slot (partitions_ota.csv ota_0/ota_1 "
                              "— release_ha is the published OTA image; PR CI "
                              "builds `release`, so this entry fires via "
                              "check_slot_budget.py at release time)"},
            ],
        }
        problems = fe.validate([entry])
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("release_ha", problems[0])
        self.assertIn("not in build_envs", problems[0])
        # and the wave-6 fix — the env joins build_envs — is what clears it
        entry["build_envs"].append("release_ha")
        self.assertEqual(fe.validate([entry]), [])

    def test_cli_build_matrix_refuses_a_guard_on_an_unbuilt_env(self):
        # firmware.yml's `flavors` job path: exit 1, the problems on stderr
        # as ::error:: annotations, and NO matrix on stdout — so
        # build-platformio never starts, rather than starting without the
        # guard. This is the gate that runs on the PR that adds the guard.
        entry = {"name": "canary", "dir": "firmware/canary",
                 "build_envs": ["release"],
                 "size_guards": [guard("release"), guard("release_ha")]}
        with tempfile.TemporaryDirectory() as tmp:
            alt = Path(tmp) / "flavors.json"
            alt.write_text(json.dumps([entry]), encoding="utf-8")
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                rc = fe.main(["--flavors", str(alt), "--build-matrix"])
        self.assertEqual(rc, 1)
        self.assertEqual(out.getvalue(), "")
        self.assertIn("::error::", err.getvalue())
        self.assertIn("release_ha", err.getvalue())


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
