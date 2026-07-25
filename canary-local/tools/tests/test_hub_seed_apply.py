#!/usr/bin/env python3
"""Host tests for canary-local/tools/hub_seed_apply.py — the hub provisioning executor.

These drive the PURE core (plan_actions / describe / counts) with hand-built
state snapshots, plus the execution loop against a fake Supervisor client, so
nothing here needs a Home Assistant or a network. Run:

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

# Import the executor from the sibling tools/ dir regardless of cwd.
TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import hub_seed_apply as hsa  # noqa: E402

REAL_PLAN = json.loads((hsa.REPO / "canary-local/devices/hub_seed.json").read_text())

# The two facts the whole executor hinges on: a third-party add-on is addressed
# by its hashed Supervisor slug, not its friendly name. If these ever drift, the
# real install 404s, so they are locked here.
SECURACV_SUP = "d0491a67_privacy_witness_kernel"
FRIGATE_SUP = "ccab4aaf_frigate"
MOSQUITTO_SUP = "core_mosquitto"


class SlugsInPlan(unittest.TestCase):
    """The committed plan carries the correct API-addressable slugs."""

    def test_supervisor_slugs_present_and_correct(self):
        by_id = {s["id"]: s for s in REAL_PLAN["steps"]}
        self.assertEqual(by_id["install-securacv"]["supervisor_slug"], SECURACV_SUP)
        self.assertEqual(by_id["install-frigate"]["supervisor_slug"], FRIGATE_SUP)
        self.assertEqual(by_id["install-broker"]["supervisor_slug"], MOSQUITTO_SUP)

    def test_friendly_and_supervisor_slug_differ_for_securacv(self):
        step = next(s for s in REAL_PLAN["steps"] if s["id"] == "install-securacv")
        # friendly stays human; supervisor_slug is what the API needs
        self.assertEqual(step["addon"], "privacy_witness_kernel")
        self.assertNotEqual(step["addon"], step["supervisor_slug"])


class PlanOnFreshHub(unittest.TestCase):
    """A fresh hub gets the whole plan, in order, nothing skipped."""

    def setUp(self):
        self.steps = hsa.plan_actions(REAL_PLAN, hsa.FRESH_HUB)

    def test_step_order(self):
        self.assertEqual(
            [s.id for s in self.steps],
            ["add-repositories", "install-broker", "install-frigate", "write-frigate-config", "install-securacv"],
        )

    def test_nothing_already_satisfied(self):
        self.assertTrue(all(not a.already for s in self.steps for a in s.actions))

    def test_every_step_narrates_why(self):
        # Option C's product requirement: you can always answer "why is this happening?"
        for s in self.steps:
            self.assertTrue(s.why.strip(), f"step {s.id} has no why")

    def test_install_uses_supervisor_slug_not_friendly(self):
        sec = next(s for s in self.steps if s.id == "install-securacv")
        install = next(a for a in sec.actions if a.kind == "install_addon")
        self.assertEqual(install.slug, SECURACV_SUP)
        self.assertEqual(install.friendly, "privacy_witness_kernel")

    def test_broker_installs_before_it_starts(self):
        broker = next(s for s in self.steps if s.id == "install-broker")
        kinds = [a.kind for a in broker.actions]
        self.assertEqual(kinds, ["install_addon", "start_addon"])

    def test_frigate_is_not_started_before_its_config(self):
        frig = next(s for s in self.steps if s.id == "install-frigate")
        self.assertNotIn("start_addon", [a.kind for a in frig.actions])
        cfg = next(s for s in self.steps if s.id == "write-frigate-config")
        self.assertEqual([a.kind for a in cfg.actions], ["write_config", "start_addon"])

    def test_config_write_then_starts_frigate_by_supervisor_slug(self):
        cfg = next(s for s in self.steps if s.id == "write-frigate-config")
        start = next(a for a in cfg.actions if a.kind == "start_addon")
        self.assertEqual(start.slug, FRIGATE_SUP)

    def test_counts(self):
        todo, done = hsa.counts(self.steps)
        self.assertEqual(done, 0)
        self.assertEqual(todo, sum(len(s.actions) for s in self.steps))


class Idempotency(unittest.TestCase):
    """Re-running against a provisioned hub skips everything."""

    def fully_provisioned(self):
        return {
            "repositories": {
                hsa.norm_url("https://github.com/blakeblackshear/frigate-hass-addons"),
                hsa.norm_url("https://github.com/kmay89/securaCV"),
            },
            "addons": {MOSQUITTO_SUP: "started", FRIGATE_SUP: "started", SECURACV_SUP: "started"},
            "existing_files": {"/addon_configs/ccab4aaf_frigate/config.yml"},
            "addon_options": {SECURACV_SUP: {"mode": "frigate"}},
        }

    def test_all_actions_skip(self):
        steps = hsa.plan_actions(REAL_PLAN, self.fully_provisioned())
        todo, done = hsa.counts(steps)
        self.assertEqual(todo, 0)
        self.assertTrue(all(a.already and a.reason for s in steps for a in s.actions))

    def test_never_overwrite_existing_config(self):
        # config present + never_overwrite => the write is skipped, not repeated
        cfg = next(
            s for s in hsa.plan_actions(REAL_PLAN, self.fully_provisioned())
            if s.id == "write-frigate-config"
        )
        write = next(a for a in cfg.actions if a.kind == "write_config")
        self.assertTrue(write.already)
        self.assertIn("never overwriting", write.reason)

    def test_absent_config_is_written(self):
        obs = self.fully_provisioned()
        obs["existing_files"] = set()  # config not there yet
        cfg = next(s for s in hsa.plan_actions(REAL_PLAN, obs) if s.id == "write-frigate-config")
        write = next(a for a in cfg.actions if a.kind == "write_config")
        self.assertFalse(write.already)


class Partial(unittest.TestCase):
    """A half-done hub: only the missing pieces are planned."""

    def test_broker_done_repos_done_rest_todo(self):
        obs = {
            "repositories": {
                hsa.norm_url("https://github.com/blakeblackshear/frigate-hass-addons"),
                hsa.norm_url("https://github.com/kmay89/securaCV"),
            },
            "addons": {MOSQUITTO_SUP: "started"},
            "existing_files": set(),
        }
        steps = hsa.plan_actions(REAL_PLAN, obs)
        by_id = {s.id: s for s in steps}
        # repos: both skip
        self.assertTrue(all(a.already for a in by_id["add-repositories"].actions))
        # broker: install + start both skip
        self.assertTrue(all(a.already for a in by_id["install-broker"].actions))
        # frigate: still needs installing
        self.assertFalse(next(a for a in by_id["install-frigate"].actions).already)
        # securacv: still to do
        self.assertFalse(all(a.already for a in by_id["install-securacv"].actions))


class Describe(unittest.TestCase):
    """Dry-run lines name the exact endpoints (so they teach + CI can assert)."""

    def test_endpoints(self):
        base = "http://supervisor"
        reg = hsa.Action(kind="register_repo", label="u", url="https://x/y")
        ins = hsa.Action(kind="install_addon", label="f", slug=SECURACV_SUP)
        sta = hsa.Action(kind="start_addon", label="f", slug=FRIGATE_SUP)
        wr = hsa.Action(kind="write_config", label="d", src="a/b.yaml", dest="/c/d.yml")
        self.assertIn("/store/repositories", hsa.describe(reg, base))
        self.assertIn(f"/store/addons/{SECURACV_SUP}/install", hsa.describe(ins, base))
        self.assertIn(f"/addons/{FRIGATE_SUP}/start", hsa.describe(sta, base))
        self.assertIn("a/b.yaml -> /c/d.yml", hsa.describe(wr, base))


class FakeClient:
    """Records the calls execute() makes, in order. Also serves observe(). Can
    be told to fail one action kind, and to report pre-existing add-on options."""

    def __init__(self, fail_on: str | None = None, current_options: dict | None = None):
        self.base = "http://supervisor"
        self.calls: list = []
        self.fail_on = fail_on
        self.current_options = current_options or {}

    def _maybe_fail(self, kind: str):
        if self.fail_on == kind:
            raise hsa.SupervisorError(f"boom on {kind}")

    # observe() surface
    def get_repositories(self):
        return set()

    def get_addons(self):
        return {}

    def get_addon_options(self, slug):
        return dict(self.current_options.get(slug, {}))

    # execute() surface (records)
    def register_repository(self, url):
        self._maybe_fail("register_repo")
        self.calls.append(("register_repo", url))

    def install_addon(self, slug):
        self._maybe_fail("install_addon")
        self.calls.append(("install_addon", slug))

    def set_addon_options(self, slug, options):
        self._maybe_fail("set_options")
        self.calls.append(("set_options", slug, options))

    def start_addon(self, slug):
        self._maybe_fail("start_addon")
        self.calls.append(("start_addon", slug))


class ExecuteLoop(unittest.TestCase):
    def test_performs_actions_in_order_and_copies_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            # a fake assets root holding the curated config the plan points at
            src_rel = "homeassistant/frigate/config.yaml"
            (tmp / "homeassistant/frigate").mkdir(parents=True)
            (tmp / src_rel).write_text("mqtt: {enabled: true}\nrecord: {enabled: false}\n")
            dest = tmp / "addon_configs/ccab4aaf_frigate/config.yml"

            # a compact plan exercising every action kind
            seed = {
                "steps": [
                    {"id": "add-repositories", "title": "repos", "why": "w", "for_what": "f",
                     "repositories": ["https://github.com/kmay89/securaCV"]},
                    {"id": "install-broker", "title": "broker", "why": "w", "for_what": "f",
                     "addon": "core_mosquitto", "supervisor_slug": MOSQUITTO_SUP, "start": True},
                    {"id": "write-frigate-config", "title": "cfg", "why": "w", "for_what": "f",
                     "source": src_rel, "dest": str(dest), "never_overwrite": True,
                     "then_start": "ccab4aaf_frigate", "then_start_supervisor_slug": FRIGATE_SUP},
                ]
            }
            steps = hsa.plan_actions(seed, hsa.FRESH_HUB)
            client = FakeClient()
            hsa.execute(steps, client, tmp)

            self.assertEqual(
                client.calls,
                [
                    ("register_repo", "https://github.com/kmay89/securaCV"),
                    ("install_addon", MOSQUITTO_SUP),
                    ("start_addon", MOSQUITTO_SUP),
                    ("start_addon", FRIGATE_SUP),
                ],
            )
            self.assertTrue(dest.exists())
            self.assertIn("record: {enabled: false}", dest.read_text())

    def test_fail_closed_stops_on_error(self):
        seed = {
            "steps": [
                {"id": "install-broker", "title": "broker", "why": "w", "for_what": "f",
                 "addon": "core_mosquitto", "supervisor_slug": MOSQUITTO_SUP, "start": True},
            ]
        }
        steps = hsa.plan_actions(seed, hsa.FRESH_HUB)
        client = FakeClient(fail_on="install_addon")
        with self.assertRaises(SystemExit) as cm:
            hsa.execute(steps, client, hsa.REPO)
        self.assertEqual(cm.exception.code, 1)
        # start must NOT have run after the install failed
        self.assertEqual(client.calls, [])

    def test_missing_source_is_fail_closed(self):
        seed = {
            "steps": [
                {"id": "write-frigate-config", "title": "cfg", "why": "w", "for_what": "f",
                 "source": "does/not/exist.yaml", "dest": "/tmp/never.yml", "never_overwrite": True},
            ]
        }
        steps = hsa.plan_actions(seed, hsa.FRESH_HUB)
        with self.assertRaises(SystemExit):
            hsa.execute(steps, FakeClient(), hsa.REPO)


class KernelFrigateMode(unittest.TestCase):
    """P1: the kernel must be put in `frigate` mode before start, or it boots the
    wizard and never produces a claim."""

    def test_securacv_step_is_install_then_setopts_then_start(self):
        steps = hsa.plan_actions(REAL_PLAN, hsa.FRESH_HUB)
        sec = next(s for s in steps if s.id == "install-securacv")
        self.assertEqual([a.kind for a in sec.actions], ["install_addon", "set_options", "start_addon"])
        setopts = next(a for a in sec.actions if a.kind == "set_options")
        self.assertEqual(setopts.slug, SECURACV_SUP)
        self.assertEqual(setopts.options, {"mode": "frigate"})
        self.assertFalse(setopts.already)  # fresh hub: options not set yet

    def test_setopts_skipped_when_mode_already_frigate(self):
        obs = dict(hsa.FRESH_HUB)
        obs["addon_options"] = {SECURACV_SUP: {"mode": "frigate", "cameras": ["front"]}}
        sec = next(s for s in hsa.plan_actions(REAL_PLAN, obs) if s.id == "install-securacv")
        setopts = next(a for a in sec.actions if a.kind == "set_options")
        self.assertTrue(setopts.already)

    def test_options_satisfied_helper(self):
        self.assertTrue(hsa.options_satisfied({"mode": "frigate"}, {"mode": "frigate", "x": 1}))
        self.assertFalse(hsa.options_satisfied({"mode": "frigate"}, {"mode": "standalone"}))
        self.assertFalse(hsa.options_satisfied({"mode": "frigate"}, {}))

    def test_execute_merges_options_without_wiping(self):
        seed = {
            "steps": [
                {"id": "install-securacv", "title": "k", "why": "w", "for_what": "f",
                 "addon": "privacy_witness_kernel", "supervisor_slug": SECURACV_SUP,
                 "options": {"mode": "frigate"}, "start": True},
            ]
        }
        # the hub already has an unrelated user option set; we must not lose it
        client = FakeClient(current_options={SECURACV_SUP: {"go2rtc_discovery": False}})
        observed = {"repositories": set(), "addons": {}, "existing_files": set(),
                    "addon_options": {}}  # not installed yet => set_options planned
        steps = hsa.plan_actions(seed, observed)
        hsa.execute(steps, client, hsa.REPO)
        setcall = next(c for c in client.calls if c[0] == "set_options")
        self.assertEqual(setcall[2], {"go2rtc_discovery": False, "mode": "frigate"})


class ConfigExtension(unittest.TestCase):
    """P2: an existing config.yaml must count as present so we never write a
    duplicate config.yml over the user's camera setup."""

    def test_config_siblings_covers_both_extensions(self):
        self.assertEqual(
            sorted(hsa.config_siblings("/addon_configs/ccab4aaf_frigate/config.yml")),
            ["/addon_configs/ccab4aaf_frigate/config.yaml", "/addon_configs/ccab4aaf_frigate/config.yml"],
        )
        # a non-yaml dest is returned as-is
        self.assertEqual(hsa.config_siblings("/etc/thing.conf"), ["/etc/thing.conf"])

    def test_observe_sees_existing_yaml_when_plan_wants_yml(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            # user's config is config.yaml; the plan targets config.yml
            (tmp / "config.yaml").write_text("cameras: {}\n")
            seed = {"steps": [{"id": "write-frigate-config", "dest": str(tmp / "config.yml"),
                               "source": "x", "never_overwrite": True}]}
            observed = hsa.observe(FakeClient(), seed)
            self.assertIn(str(tmp / "config.yml"), observed["existing_files"])
            # and the planner therefore skips the write
            cfg = next(s for s in hsa.plan_actions(seed, observed) if s.id == "write-frigate-config")
            write = next(a for a in cfg.actions if a.kind == "write_config")
            self.assertTrue(write.already)


if __name__ == "__main__":
    unittest.main()
