#!/usr/bin/env python3
"""Pins scripts/check_firmware_advisories.py — the firmware's third-party
versions against their upstreams' published repository advisories (A2-20).

What is pinned and why:
  • GitHub's `vulnerable_version_range` grammar (`>= a, < b`, `<= a`, `= a`,
    a `v` prefix, 2.8 == 2.8.0, a pioarduino `55.03.38-1`), and a range it
    cannot read is None, never "not affected";
  • coverage cannot pass vacuously: a third-party component with no exact
    version, or a name with no upstream repository, is a gap that fails;
    only an Arduino core on the action's "latest" that CI-only rows build
    is listed as not checked, and the same core on a release row is a gap;
  • the comparison: an affected version fails, a patched one passes, a
    withdrawn advisory is skipped, a REVIEWED one is skipped, an advisory
    with no range or an unreadable one is "unread" and fails;
  • main(): exit 0 clean, 1 on a match or a gap, 2 when an upstream cannot
    be read; --coverage makes no request; the job token is sent when set;
  • the API client follows the `Link: rel="next"` pages;
  • the committed SBOM: every third-party name maps to an upstream, and the
    only gaps are the floating lib_deps of FLOATING_PRODUCTS;
  • RELEASE_WORKFLOWS equals gen_firmware_sbom.py's, and audit.yml runs the
    check weekly with the job's own token and no secret.

No test makes a network request. Discovered by lint.yml's
`unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]


def _load(name: str, rel: str):
    spec = importlib.util.spec_from_file_location(name, REPO / rel)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


ca = _load("check_firmware_advisories", "scripts/check_firmware_advisories.py")

# Every product's lib_deps are exact pins (firmware/canary/platformio.ini
# included), so the committed SBOM may have no coverage gap at all. A product
# named here would be allowed to float; keep the set empty.
FLOATING_PRODUCTS: set[str] = set()


def _lib(name: str, version: str | None, group: str = "upstream", **props) -> dict:
    c = {"bom-ref": f"lib:{group}/{name}@{version or '^1'}", "type": "library",
         "group": group, "name": name,
         "properties": [{"name": k, "value": v} for k, v in sorted(props.items())]}
    if version:
        c["version"] = version
    return c


def _adv(ghsa: str, rng: str | None, patched: str = "9.9.9", **extra) -> dict:
    vulns = [] if rng == "NONE" else [{"package": {"ecosystem": "other", "name": "x"},
                                      "vulnerable_version_range": rng,
                                      "patched_versions": patched}]
    return dict({"ghsa_id": ghsa, "severity": "high", "summary": "s",
                 "withdrawn_at": None, "vulnerabilities": vulns}, **extra)


class Ranges(unittest.TestCase):
    def test_versions(self):
        self.assertEqual(ca.version_key("2.8"), ca.version_key("2.8.0"))
        self.assertEqual(ca.version_key("v5.5.4"), (5, 5, 4))
        self.assertEqual(ca.version_key("55.03.38-1"), (55, 3, 38, 1))
        for bad in ("", "5.1-rc1", "latest", "^7.0.0"):
            self.assertIsNone(ca.version_key(bad), bad)

    def test_the_github_range_grammar(self):
        cases = [
            (">= 1.0.0, < 1.4.3", "1.4.2", True), (">= 1.0.0, < 1.4.3", "1.4.3", False),
            (">= 1.0.0, < 1.4.3", "0.9.9", False), ("<= 2.5.1", "2.5.2", False),
            ("<= 2.5.1", "2.5.1", True), ("= 7.4.3", "7.4.3", True), ("7.4.3", "7.4.4", False),
            ("< 2.8", "2.8.0", False), ("< v5.5.4", "5.5.3", True), ("> 0.4", "0.4.0", False),
            ("== 55.03.38-1", "55.03.38-1", True), ("<55.03.38-2", "55.03.38-1", True),
        ]
        for rng, ver, hit in cases:
            parsed = ca.parse_range(rng)
            self.assertIsNotNone(parsed, rng)
            self.assertEqual(ca.in_range(ca.version_key(ver), parsed), hit, (rng, ver))

    def test_an_unreadable_range_is_none_not_unaffected(self):
        for rng in (None, "", "all versions", ">= 5.1-rc1", "~> 2.0", "< 2.0 || > 3.0"):
            self.assertIsNone(ca.parse_range(rng), rng)


class Coverage(unittest.TestCase):
    def test_first_party_is_not_compared_and_third_party_needs_an_exact_version(self):
        doc = {"components": [
            {"bom-ref": "common:ota", "group": "securacv", "name": "ota"},
            _lib("ArduinoJson", "7.4.3", "bblanchon"),
            _lib("PubSubClient", None, "knolleary", **{"securacv:version_spec": "^2.8",
                                                       "securacv:envs": "canary:release_ha"}),
            _lib("SomethingNew", "1.0.0", "someone"),
        ]}
        checked, gaps, skipped = ca.coverage(doc)
        self.assertEqual([c["name"] for c in checked], ["ArduinoJson"])
        self.assertEqual(skipped, [])
        self.assertEqual(len(gaps), 2, gaps)
        self.assertIn("has no exact version (spec ^2.8, used by canary:release_ha)", gaps[0])
        self.assertIn("no upstream repository for 'SomethingNew'", gaps[1])

    def test_only_a_ci_only_latest_core_is_left_unchecked(self):
        def core(paths: str) -> dict:
            return {"bom-ref": "framework:arduino-esp32@latest", "group": "espressif",
                    "name": "arduino-esp32", "type": "framework",
                    "properties": [{"name": "securacv:build_paths", "value": paths}]}
        _, gaps, skipped = ca.coverage({"components": [core(
            "arduino-cli (firmware.yml:build-arduino-cli)")]})
        self.assertEqual((len(gaps), len(skipped)), (0, 1))
        for release in ca.RELEASE_WORKFLOWS:
            _, gaps, skipped = ca.coverage({"components": [core(
                f"arduino-cli (firmware.yml:build-arduino-cli); arduino-cli ({release}:job)")]})
            self.assertEqual((len(gaps), len(skipped)), (1, 0), release)
        # a version-less core reached by anything but an arduino-cli row
        _, gaps, _ = ca.coverage({"components": [core("platformio (platform_core3)")]})
        self.assertEqual(len(gaps), 1)


class Compare(unittest.TestCase):
    comps = [_lib("NimBLE-Arduino", "2.5.2", "h2zero")]
    repo = "h2zero/NimBLE-Arduino"

    def _run(self, *advisories):
        return ca.compare(self.comps, {self.repo: list(advisories)})

    def test_an_affected_version_is_reported_and_a_patched_one_is_not(self):
        affected, unread = self._run(_adv("GHSA-aaaa", ">= 2.0.0, < 2.5.3", "2.5.3"))
        self.assertEqual(unread, [])
        self.assertEqual(len(affected), 1)
        self.assertIn("2.5.2 is in h2zero/NimBLE-Arduino GHSA-aaaa", affected[0])
        self.assertIn("patched: 2.5.3", affected[0])
        self.assertEqual(self._run(_adv("GHSA-bbbb", "< 2.5.2")), ([], []))

    def test_withdrawn_and_reviewed_advisories_are_skipped(self):
        self.assertEqual(self._run(_adv("GHSA-cccc", "<= 2.5.2",
                                        withdrawn_at="2026-01-01T00:00:00Z")), ([], []))
        with mock.patch.dict(ca.REVIEWED, {"GHSA-dddd": "BLE mesh only; not linked"}):
            self.assertEqual(self._run(_adv("GHSA-dddd", "<= 2.5.2")), ([], []))

    def test_an_advisory_without_a_readable_range_is_unread_and_fails(self):
        affected, unread = self._run(_adv("GHSA-eeee", "NONE"), _adv("GHSA-ffff", "all versions"))
        self.assertEqual(affected, [])
        self.assertEqual(len(unread), 2, unread)
        self.assertIn("names no affected range", unread[0])
        self.assertIn("'all versions' is not machine-readable", unread[1])


def _doc_file(tmp: str, components: list[dict]) -> Path:
    path = Path(tmp) / "sbom.json"
    path.write_text(json.dumps({"components": components}), encoding="utf-8")
    return path


class Main(unittest.TestCase):
    def _main(self, components, fetch, *extra, env=None):
        with tempfile.TemporaryDirectory() as tmp:
            path = _doc_file(tmp, components)
            out = io.StringIO()
            with mock.patch.dict("os.environ", env or {}, clear=True), redirect_stdout(out):
                rc = ca.main(["--sbom", str(path), *extra], fetch=fetch)
        return rc, out.getvalue()

    def test_exit_codes(self):
        comps = [_lib("ArduinoJson", "7.4.3", "bblanchon")]
        rc, out = self._main(comps, lambda repo, token: [])
        self.assertEqual(rc, 0, out)
        self.assertIn("1 third-party component(s) at an exact version, 1 upstream repo(s)", out)
        self.assertIn("Not covered:", out)
        rc, out = self._main(comps, lambda repo, token: [_adv("GHSA-1", "< 7.5.0")])
        self.assertEqual(rc, 1, out)
        self.assertIn("::error::affected:", out)

        def down(repo, token):
            raise OSError("proxy refused")
        rc, out = self._main(comps, down)
        self.assertEqual(rc, 2, out)
        self.assertIn("could not read the advisories of bblanchon/ArduinoJson", out)
        # a gap fails even when every advisory is clean
        rc, out = self._main(comps + [_lib("Crypto", None, "rweather")], lambda r, t: [])
        self.assertEqual(rc, 1, out)

    def test_coverage_mode_makes_no_request(self):
        def boom(repo, token):
            raise AssertionError("--coverage must not query")
        rc, _ = self._main([_lib("ArduinoJson", "7.4.3", "bblanchon")], boom, "--coverage")
        self.assertEqual(rc, 0)
        rc, _ = self._main([_lib("Crypto", None, "rweather")], boom, "--coverage")
        self.assertEqual(rc, 1)

    def test_the_job_token_is_sent_when_the_environment_has_one(self):
        seen = []
        self._main([_lib("ArduinoJson", "7.4.3", "bblanchon")],
                   lambda repo, token: seen.append(token) or [], env={"GH_TOKEN": "t0k"})
        self._main([_lib("ArduinoJson", "7.4.3", "bblanchon")],
                   lambda repo, token: seen.append(token) or [])
        self.assertEqual(seen, ["t0k", None])


class Client(unittest.TestCase):
    def test_pages_are_followed_and_the_token_is_a_bearer_header(self):
        pages = [([{"ghsa_id": "GHSA-1"}], '<https://api.github.com/next?after=x>; rel="next"'),
                 ([{"ghsa_id": "GHSA-2"}], "")]
        requests = []

        class Resp:
            def __init__(self, body, link):
                self.body, self.headers = body, {"Link": link}

            def read(self):
                return json.dumps(self.body).encode()

            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

        def fake_urlopen(req, timeout):
            requests.append(req)
            return Resp(*pages[len(requests) - 1])

        with mock.patch.object(ca.urllib.request, "urlopen", fake_urlopen):
            got = ca.fetch_advisories("h2zero/NimBLE-Arduino", "tok")
        self.assertEqual([a["ghsa_id"] for a in got], ["GHSA-1", "GHSA-2"])
        self.assertEqual(requests[0].full_url,
                         "https://api.github.com/repos/h2zero/NimBLE-Arduino/"
                         "security-advisories?state=published&per_page=100")
        self.assertEqual(requests[1].full_url, "https://api.github.com/next?after=x")
        self.assertEqual(requests[0].get_header("Authorization"), "Bearer tok")


class TheTree(unittest.TestCase):
    def test_every_third_party_name_in_the_committed_sbom_has_an_upstream(self):
        doc = json.loads(ca.SBOM.read_text(encoding="utf-8"))
        names = {c["name"] for c in ca.third_party(doc)}
        self.assertTrue({"NimBLE-Arduino", "ArduinoJson", "PubSubClient", "Crypto", "lvgl",
                         "esp-idf", "arduino-esp32"} <= names, names)
        self.assertEqual(names - set(ca.UPSTREAM), set())

    def test_the_committed_sbom_gaps_are_only_the_floating_products(self):
        doc = json.loads(ca.SBOM.read_text(encoding="utf-8"))
        checked, gaps, skipped = ca.coverage(doc)
        self.assertTrue(checked)
        self.assertEqual([s.split(":")[0] + ":" + s.split(":")[1] for s in skipped],
                         ["framework:arduino-esp32@latest"])
        by_ref = {c["bom-ref"]: c for c in doc["components"]}
        for gap in gaps:
            ref = gap.split(" has no exact version")[0]
            self.assertIn(ref, by_ref, gap)
            envs = next(p["value"] for p in by_ref[ref]["properties"]
                        if p["name"] == "securacv:envs")
            self.assertEqual({e.split(":")[0] for e in envs.split(",")} - FLOATING_PRODUCTS,
                             set(), gap)

    def test_release_workflows_match_the_generator(self):
        gs = _load("gen_firmware_sbom_for_advisories", "scripts/gen_firmware_sbom.py")
        self.assertEqual(ca.RELEASE_WORKFLOWS, gs.RELEASE_WORKFLOWS)

    def test_audit_yml_runs_it_weekly_with_no_secret(self):
        import yaml
        text = (REPO / ".github/workflows/audit.yml").read_text(encoding="utf-8")
        wf = yaml.safe_load(text)
        on = wf.get("on", wf.get(True))
        self.assertIn("schedule", on)
        self.assertIn("scripts/check_firmware_advisories.py", on["pull_request"]["paths"])
        job = wf["jobs"]["firmware-advisories"]
        runs = [s for s in job["steps"] if "check_firmware_advisories.py" in str(s.get("run"))]
        self.assertEqual(len(runs), 1)
        self.assertEqual(runs[0]["run"].strip(), "python3 scripts/check_firmware_advisories.py")
        self.assertEqual(runs[0]["env"], {"GH_TOKEN": "${{ github.token }}"})
        self.assertNotIn("secrets.", text)


if __name__ == "__main__":
    unittest.main()
