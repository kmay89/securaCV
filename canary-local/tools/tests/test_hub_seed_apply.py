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
            [
                "add-repositories",
                "install-broker",
                # The devices' broker account is minted before anything that
                # publishes exists, so no Canary can ever meet a broker that
                # would refuse it. It is separate from connect-mqtt below
                # because Home Assistant needs no account of its own — it
                # reaches Mosquitto as the reserved `homeassistant` user.
                "mqtt-login",
                # Connecting Home Assistant to the broker sits between installing
                # it and installing anything that publishes to it: Frigate coming
                # up first would publish into a broker nobody is listening to.
                "connect-mqtt",
                "install-frigate",
                "write-frigate-config",
                "install-securacv",
            ],
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


class OptionalFeatures(unittest.TestCase):
    """Opt-in extras (Pi-hole): zero footprint unless asked for, loud on typos."""

    def test_default_plan_has_no_feature_steps(self):
        steps = hsa.plan_actions(REAL_PLAN, hsa.FRESH_HUB)
        ids = {s.id for s in steps}
        self.assertNotIn("install-pihole", ids)
        self.assertNotIn("install-display", ids)
        # And no feature-tagged repository leaks into the core register step:
        # an un-enabled feature must leave ZERO footprint on the hub.
        reg = next(s for s in steps if s.id == "add-repositories")
        for a in reg.actions:
            self.assertNotIn("Poeschl", a.url)
            self.assertNotIn("HAOS-kiosk", a.url)

    def test_with_pihole_appends_the_full_step(self):
        steps = hsa.plan_actions(REAL_PLAN, hsa.FRESH_HUB, frozenset({"pihole"}))
        pihole = next(s for s in steps if s.id == "install-pihole")
        kinds = [a.kind for a in pihole.actions]
        # Its OWN repo registration rides inside the step (order matters:
        # register before install), then the install, then the start.
        self.assertEqual(kinds, ["register_repo", "install_addon", "start_addon"])
        slug = next(a for a in pihole.actions if a.kind == "install_addon").slug
        self.assertTrue(slug.endswith("_pihole"), slug)
        self.assertNotEqual(slug, "pihole", "must be the hashed Supervisor slug")
        # The trust framing is the point of the step — the narration must give
        # the user a way to CHECK the quiet promise, not just block ads.
        narration = (pihole.why + pihole.for_what).lower()
        self.assertIn("phoning home", narration)
        self.assertIn("instead of taking our word", narration)
        self.assertTrue(pihole.user_must_finish, "router DNS change is the user's")

    def test_with_display_appends_install_but_never_a_start(self):
        steps = hsa.plan_actions(REAL_PLAN, hsa.FRESH_HUB, frozenset({"display"}))
        disp = next(s for s in steps if s.id == "install-display")
        kinds = [a.kind for a in disp.actions]
        # Its OWN repo registration rides inside the step, then the install —
        # and NOTHING else. The add-on refuses to run until the operator types
        # their Home Assistant login into its configuration, so a planned start
        # here would fail the whole run on a correctly-provisioned hub. The
        # last move is the user's, and the step must say so.
        self.assertEqual(kinds, ["register_repo", "install_addon"])
        slug = next(a for a in disp.actions if a.kind == "install_addon").slug
        self.assertTrue(slug.endswith("_haoskiosk"), slug)
        self.assertNotEqual(slug, "haoskiosk", "must be the hashed Supervisor slug")
        self.assertTrue(disp.user_must_finish, "the HA login is the user's to give")
        self.assertIn("username and password", disp.user_must_finish)
        # Least privilege is part of the promise: the narration must steer to
        # a dedicated, non-admin account — the add-on keeps the password in
        # its options, and a screen has no business holding admin rights.
        self.assertIn("Administrator off", disp.user_must_finish)
        # Headless stays the honest default — the narration must say a hub
        # never NEEDS a screen, not sell one.
        self.assertIn("headless is the default", disp.why)

    def test_display_plan_never_carries_a_credential(self):
        # The kiosk browser signs in as a Home Assistant user. That credential
        # is the operator's own: the plan may name the field, it may never
        # carry or mint a value for it. (Same invariant the broker login step
        # proves for its password — see MqttLogin.)
        step = next(s for s in REAL_PLAN["steps"] if s["id"] == "install-display")
        self.assertNotIn("password", json.dumps(step.get("options", {})).lower())
        self.assertNotIn("options", step, "no options are set — none are safe to guess")

    def test_both_features_together_compose(self):
        steps = hsa.plan_actions(
            REAL_PLAN, hsa.FRESH_HUB, frozenset({"pihole", "display"})
        )
        ids = [s.id for s in steps]
        self.assertIn("install-pihole", ids)
        self.assertIn("install-display", ids)

    def test_core_plan_is_identical_with_and_without_features(self):
        base = [s.id for s in hsa.plan_actions(REAL_PLAN, hsa.FRESH_HUB)]
        feature_steps = {"install-pihole", "install-display"}
        extra = [
            s.id
            for s in hsa.plan_actions(
                REAL_PLAN, hsa.FRESH_HUB, frozenset({"pihole", "display"})
            )
            if s.id not in feature_steps
        ]
        self.assertEqual(base, extra)

    def test_unknown_feature_fails_loudly_before_touching_anything(self):
        rc = hsa.main(["--dry-run", "--with", "typo-hole"])
        self.assertEqual(rc, 2)

    def test_dry_run_with_pihole_narrates_it(self):
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = hsa.main(["--dry-run", "--with", "pihole"])
        self.assertEqual(rc, 0)
        self.assertIn("pihole", buf.getvalue().lower())

    def test_dry_run_without_pihole_mentions_it_is_available(self):
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = hsa.main(["--dry-run"])
        self.assertEqual(rc, 0)
        self.assertIn("--with pihole", buf.getvalue())

    def test_dry_run_without_display_mentions_it_is_available(self):
        # Nobody can choose a feature they never hear about: a plain dry-run
        # must name the display option the same way it names Pi-hole.
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = hsa.main(["--dry-run"])
        self.assertEqual(rc, 0)
        self.assertIn("--with display", buf.getvalue())

    def test_dry_run_with_display_narrates_it(self):
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = hsa.main(["--dry-run", "--with", "display"])
        self.assertEqual(rc, 0)
        self.assertIn("haoskiosk", buf.getvalue().lower())


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
            "addon_options": {
                SECURACV_SUP: {"mode": "frigate"},
                # The devices' broker account already exists. Read back from the
                # add-on's own options, which is the only place it lives — see
                # test_a_rerun_never_reissues_the_password for why that matters.
                MOSQUITTO_SUP: {"logins": [{"username": "canary", "password": "already-set"}]},
            },
            # Home Assistant is already connected to the broker. Without this the
            # connect step would re-run forever and stack duplicate entries.
            "config_entries": {"mqtt"},
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
    def get_config_entry_domains(self):
        self.calls.append(("get_config_entry_domains",))
        return set()

    def create_config_entry(self, handler, data):
        self._maybe_fail("core_config_entry")
        self.calls.append(("create_config_entry", handler, dict(data)))

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

    def restart_addon(self, slug):
        self._maybe_fail("restart_addon")
        self.calls.append(("restart_addon", slug))


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


class MqttConnect(unittest.TestCase):
    """Installing a broker is not the same as USING one.

    A running Mosquitto with no MQTT integration is a post office nobody has an
    address for: Frigate publishes detections, every Canary publishes events,
    and Home Assistant subscribes to none of it — so no entities appear and the
    hub looks empty. On a hub with a keyboard you click through a dialog. There
    is no dialog on a headless hub, which is why this is a plan step.
    """

    def step(self):
        return next(s for s in REAL_PLAN["steps"] if s["id"] == "connect-mqtt")

    def test_the_plan_carries_a_config_entry_for_mqtt(self):
        entry = self.step()["core_config_entry"]
        self.assertEqual(entry["handler"], "mqtt")
        self.assertEqual(entry["data"]["port"], 1883)

    def test_the_broker_address_is_the_addon_not_localhost(self):
        # Core runs in a different container from the add-on, so 127.0.0.1 would
        # point Core at itself and time out with a very unhelpful message.
        broker = self.step()["core_config_entry"]["data"]["broker"]
        self.assertEqual(broker, "core-mosquitto")
        self.assertNotIn(broker, ("localhost", "127.0.0.1", "::1"))

    def test_it_runs_on_a_fresh_hub(self):
        steps = hsa.plan_actions(REAL_PLAN, hsa.FRESH_HUB)
        step = next(s for s in steps if s.id == "connect-mqtt")
        actions = [a for a in step.actions if a.kind == "core_config_entry"]
        self.assertEqual(len(actions), 1)
        self.assertFalse(actions[0].already)
        self.assertEqual(actions[0].handler, "mqtt")

    def test_it_is_skipped_once_home_assistant_is_connected(self):
        observed = dict(hsa.FRESH_HUB, config_entries={"mqtt"})
        steps = hsa.plan_actions(REAL_PLAN, observed)
        step = next(s for s in steps if s.id == "connect-mqtt")
        action = next(a for a in step.actions if a.kind == "core_config_entry")
        self.assertTrue(action.already)
        self.assertTrue(action.reason)

    def test_it_comes_before_anything_that_publishes(self):
        ids = [s.id for s in hsa.plan_actions(REAL_PLAN, hsa.FRESH_HUB)]
        self.assertLess(ids.index("install-broker"), ids.index("connect-mqtt"))
        for publisher in ("install-frigate", "install-securacv"):
            self.assertLess(
                ids.index("connect-mqtt"),
                ids.index(publisher),
                f"{publisher} would publish into a broker nobody is listening to",
            )

    def test_describe_says_something_a_person_can_read(self):
        steps = hsa.plan_actions(REAL_PLAN, hsa.FRESH_HUB)
        action = next(
            a for s in steps for a in s.actions if a.kind == "core_config_entry"
        )
        text = hsa.describe(action, "http://supervisor")
        self.assertIn("mqtt", text.lower())
        self.assertNotIn("None", text)


class MqttConnectExecution(unittest.TestCase):
    """The planned config entry actually reaches a client call."""

    def test_perform_routes_the_action_to_the_core_api(self):
        # Real assets root so the whole plan runs to completion rather than
        # dying on write_config and hiding whatever came after it.
        repo = Path(hsa.__file__).resolve().parents[2]
        client = FakeClient()
        steps = hsa.plan_actions(REAL_PLAN, hsa.FRESH_HUB)
        with tempfile.TemporaryDirectory() as tmp:
            for st in steps:
                for a in st.actions:
                    if a.kind == "write_config":
                        a.dest = str(Path(tmp) / Path(a.dest).name)
            hsa.execute(steps, client, repo)
        made = [c for c in client.calls if c[0] == "create_config_entry"]
        self.assertEqual(len(made), 1, f"calls were: {client.calls}")
        self.assertEqual(made[0][1], "mqtt")
        self.assertEqual(made[0][2]["broker"], "core-mosquitto")

    def test_a_failing_connect_is_reported_but_does_not_sink_the_run(self):
        """Loud, but not fatal — and the distinction matters a lot here.

        Home Assistant can answer a hand-started MQTT flow with a form asking
        for broker credentials: the Mosquitto add-on authenticates against HA
        users, and a manual flow does not inherit the add-on discovery that
        would supply them. If that aborted the run, provisioning would stop
        BEFORE Frigate and the witness kernel install — leaving a hub with far
        less on it than if we had never attempted the connect at all.

        So the connect is best-effort: it says what happened, and the steps
        after it still run.
        """
        import io
        import contextlib

        client = FakeClient(fail_on="core_config_entry")
        steps = hsa.plan_actions(REAL_PLAN, hsa.FRESH_HUB)
        repo = Path(hsa.__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as tmp:
            for st in steps:
                for a in st.actions:
                    if a.kind == "write_config":
                        a.dest = str(Path(tmp) / Path(a.dest).name)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                hsa.execute(steps, client, repo)  # must NOT raise SystemExit
        text = out.getvalue()
        self.assertIn("could not finish", text)
        self.assertIn("continuing", text)
        # And the steps after it really did run.
        kinds = [c[0] for c in client.calls]
        self.assertIn("install_addon", kinds)
        self.assertIn("start_addon", kinds)

    def test_a_failing_install_still_stops_the_run(self):
        # Only the advisory action is forgiven. A real install failure must
        # still fail closed, or a half-built hub reports success.
        client = FakeClient(fail_on="install_addon")
        steps = hsa.plan_actions(REAL_PLAN, hsa.FRESH_HUB)
        repo = Path(hsa.__file__).resolve().parents[2]
        with self.assertRaises(SystemExit) as cm:
            hsa.execute(steps, client, repo)
        self.assertNotEqual(cm.exception.code, 0)

    def test_observe_asks_home_assistant_what_is_already_connected(self):
        client = FakeClient()
        observed = hsa.observe(client, REAL_PLAN)
        self.assertIn("config_entries", observed)
        self.assertIn(("get_config_entry_domains",), client.calls)


class MqttLogin(unittest.TestCase):
    """The account a Canary signs in to the broker with.

    Mosquitto's add-on refuses anonymous connections. Home Assistant is exempt
    in practice — it reaches the broker over the internal container network as
    the reserved `homeassistant` user — so the connect-mqtt step needs no
    credential and correctly mints none. A Canary gets no such exemption: it is
    an ordinary external client on the LAN. Without this step a hub finishes
    provisioning, reports success, and then rejects the first device that ever
    tries to publish, with a failure that reads on a matchbox-sized screen as a
    wrong Wi-Fi password.
    """

    def step(self, observed=None):
        steps = hsa.plan_actions(REAL_PLAN, observed or hsa.FRESH_HUB)
        return next(s for s in steps if s.id == "mqtt-login")

    def test_a_fresh_hub_mints_the_account(self):
        action = next(a for a in self.step().actions if a.kind == "mqtt_login")
        self.assertFalse(action.already)
        self.assertEqual(action.username, "canary")
        self.assertEqual(action.slug, MOSQUITTO_SUP)

    def test_it_lands_before_anything_that_publishes(self):
        # Frigate and the witness kernel both publish to the broker. If the
        # account appeared after them, a device could meet a broker that would
        # refuse it — the exact window this ordering exists to close.
        ids = [s.id for s in hsa.plan_actions(REAL_PLAN, hsa.FRESH_HUB)]
        self.assertLess(ids.index("mqtt-login"), ids.index("install-frigate"))
        self.assertLess(ids.index("mqtt-login"), ids.index("install-securacv"))

    def test_an_existing_account_is_left_alone(self):
        obs = dict(hsa.FRESH_HUB)
        obs["addon_options"] = {MOSQUITTO_SUP: {"logins": [{"username": "canary", "password": "x"}]}}
        action = next(a for a in self.step(obs).actions if a.kind == "mqtt_login")
        self.assertTrue(action.already)
        self.assertIn("canary", action.reason)

    def test_someone_elses_account_does_not_count_as_ours(self):
        obs = dict(hsa.FRESH_HUB)
        obs["addon_options"] = {MOSQUITTO_SUP: {"logins": [{"username": "zigbee2mqtt", "password": "x"}]}}
        action = next(a for a in self.step(obs).actions if a.kind == "mqtt_login")
        self.assertFalse(action.already)

    def test_a_rerun_never_reissues_the_password(self):
        # THE property. The executor is meant to be safe to re-run, and this is
        # the one action where "do it again" would be destructive: a fresh
        # password silently locks out every Canary already flashed with the old
        # one, and the devices report it as an auth failure indistinguishable
        # from a typo. So a re-run must reuse what the add-on already holds.
        existing = {"logins": [{"username": "canary", "password": "the-one-devices-have"}]}
        client = FakeClient(current_options={MOSQUITTO_SUP: existing})
        action = hsa.Action(kind="mqtt_login", label="l", slug=MOSQUITTO_SUP, username="canary")
        hsa._perform(action, client, hsa.REPO)
        wrote = next(c for c in client.calls if c[0] == "set_options")
        self.assertEqual(
            wrote[2]["logins"], [{"username": "canary", "password": "the-one-devices-have"}]
        )

    def test_it_appends_rather_than_replacing_other_logins(self):
        # `logins` may already carry accounts an operator made by hand. Wiping
        # them would lock out whatever is using them.
        existing = {"logins": [{"username": "zigbee2mqtt", "password": "keep-me"}]}
        client = FakeClient(current_options={MOSQUITTO_SUP: existing})
        action = hsa.Action(kind="mqtt_login", label="l", slug=MOSQUITTO_SUP, username="canary")
        hsa._perform(action, client, hsa.REPO)
        wrote = next(c for c in client.calls if c[0] == "set_options")
        names = [e["username"] for e in wrote[2]["logins"]]
        self.assertIn("zigbee2mqtt", names)
        self.assertIn("canary", names)
        kept = next(e for e in wrote[2]["logins"] if e["username"] == "zigbee2mqtt")
        self.assertEqual(kept["password"], "keep-me")

    def test_the_broker_is_restarted_so_the_account_takes_effect(self):
        # Mosquitto parses `logins` once at startup. Without the cycle the step
        # reports success while the broker goes on refusing the account — the
        # failure mode this whole step exists to remove, reintroduced one layer
        # down.
        client = FakeClient()
        action = hsa.Action(kind="mqtt_login", label="l", slug=MOSQUITTO_SUP, username="canary")
        hsa._perform(action, client, hsa.REPO)
        kinds = [c[0] for c in client.calls]
        self.assertIn("restart_addon", kinds)
        self.assertLess(kinds.index("set_options"), kinds.index("restart_addon"))

    def test_a_minted_password_is_not_guessable_or_awkward(self):
        # It travels through flasher fields, QR codes and serial consoles, so it
        # must survive being pasted without an escaping question — and it must
        # not be short enough to be worth guessing at.
        seen = {hsa.mint_password() for _ in range(50)}
        self.assertEqual(len(seen), 50, "every mint must be distinct")
        for pw in seen:
            self.assertGreaterEqual(len(pw), 20)
            self.assertRegex(pw, r"^[A-Za-z0-9_-]+$")

    def test_the_password_is_never_printed(self):
        # This narration is streamed over SSH into the flasher's console, which
        # gets scrolled back, screenshotted and pasted into support threads —
        # anything that captures it keeps a working broker credential in clear
        # text forever. CodeQL flags this class directly. Nothing is lost by
        # withholding it: the add-on holds `logins` in its own configuration, so
        # the operator reads it on demand from the one place that must have it.
        import contextlib
        import io
        client = FakeClient()
        action = hsa.Action(kind="mqtt_login", label="l", slug=MOSQUITTO_SUP,
                            friendly="mosquitto", username="canary")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            hsa._perform(action, client, hsa.REPO)
        printed = buf.getvalue()
        wrote = next(c for c in client.calls if c[0] == "set_options")
        secret = wrote[2]["logins"][0]["password"]
        self.assertNotIn(secret, printed, "the minted password reached stdout")
        # It must still say WHERE to get it — "look it up" with no path is how
        # someone ends up resetting a credential that was fine.
        self.assertIn("canary", printed)
        self.assertIn("Configuration", printed)

    def test_a_failed_restart_rolls_the_login_back(self):
        # The repair contract: re-running fixes a partial failure. This step
        # decides it is done by asking whether the username is in the add-on's
        # options — so if the write landed and the restart did NOT, a later run
        # would see the account, call the step satisfied, and never perform the
        # restart that is the actual missing piece. The broker would go on
        # refusing every Canary while the executor reported success.
        #
        # Rolling the write back keeps the two halves atomic as far as observe()
        # can tell: either the account exists and the broker has read it, or it
        # does not exist and the next run does the whole thing again.
        existing = {"logins": [{"username": "zigbee2mqtt", "password": "keep-me"}]}
        client = FakeClient(fail_on="restart_addon", current_options={MOSQUITTO_SUP: existing})
        action = hsa.Action(kind="mqtt_login", label="l", slug=MOSQUITTO_SUP, username="canary")
        with self.assertRaises(hsa.SupervisorError):
            hsa._perform(action, client, hsa.REPO)
        writes = [c for c in client.calls if c[0] == "set_options"]
        self.assertEqual(len(writes), 2, "expected the write and then its rollback")
        # The hub is left exactly as it was found — our account gone, and the
        # operator's own account untouched.
        self.assertEqual(writes[-1][2], existing)
        self.assertFalse(hsa._login_present(writes[-1][2], "canary"))

    def test_no_password_is_committed_to_the_repo(self):
        # A password in the plan would be a published credential on every hub
        # anyone ever flashed. The plan may describe the account; it may never
        # carry its secret.
        step = next(s for s in REAL_PLAN["steps"] if s["id"] == "mqtt-login")
        self.assertNotIn("password", json.dumps(step["mqtt_login"]))
        # And the dry-run line must not print one either — there isn't one yet.
        action = next(a for a in self.step().actions if a.kind == "mqtt_login")
        line = hsa.describe(action, "http://supervisor")
        self.assertIn("generated", line)
        self.assertNotIn("password=", line)
