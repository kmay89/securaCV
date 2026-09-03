#!/usr/bin/env python3
"""Pins scripts/carry_to_site.py — the one command that refreshes every fact the
website carries from this repo (the /checkup build matrix, the landing page's
kernel-status grid, the Witness Wall's vendored verifier).

What is pinned and why:
  • the `builds` projection of firmware/build_matrix.json keeps the invariants
    the website's tests/plugin-facts.test.mjs enforces (every feature is in the
    catalog, every build has the self-test, serial/vision boards list no BLE or
    mesh, the recommendation resolves) — so a carry can never open a red PR on
    the website for a reason that was decidable here;
  • stamping touches ONLY the `builds` value of the website's hand-formatted
    onboarding-spec.json, byte-preserving everything around it, and is
    idempotent — the file is website-authored; this tool owns one block of it;
  • the browser mirror is HEADER + JSON.stringify-equivalent + ";\\n", which the
    website pins byte-for-byte;
  • kernel-status.json orders tiles done → wip → planned, carries an evidence
    sentence for every tile and every status a detector can return, and has no
    date or sha (a carry that changes bytes on every run cannot say "nothing
    changed");
  • a full --site run into a website-shaped tree is byte-reproducible (a second
    run changes nothing) and the vendored verifier + fixtures are byte-equal to
    upstream with matching PROVENANCE sha256 lines.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import re
import shutil
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "carry_to_site.py"

spec = importlib.util.spec_from_file_location("carry_to_site", SCRIPT)
cs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cs)  # type: ignore[union-attr]

MATRIX = json.loads(cs.BUILD_MATRIX.read_text(encoding="utf-8"))
BOARDS = json.loads(cs.BOARDS.read_text(encoding="utf-8"))

# A miniature of the website's hand-formatted onboarding-spec.json: compact
# inline objects, a string with braces and an escaped quote before the block,
# a nested "builds" key AFTER it that must not be mistaken for the top-level one.
SAMPLE_SPEC = (
    '{\n'
    '  "_comment": "hand {formatted} \\"copy\\"",\n'
    '  "drive": { "name": "CANARY-EVIDENCE", "folders": ["WITNESS", "HEALTH"] },\n'
    '  "builds": {\n'
    '    "_comment": "old hand copy",\n'
    '    "products": [ { "id": "canary", "levels": { "dev": { "features": ["selftest"] } } } ]\n'
    '  },\n'
    '  "tools": { "list": [ { "id": "plugin", "builds": { "nested": true } } ] },\n'
    '  "provenance": { "builds": "firmware/build_matrix.json" }\n'
    '}\n'
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class BuildsProjection(unittest.TestCase):
    def setUp(self):
        self.builds = cs.project_builds(MATRIX, BOARDS)

    def test_every_upstream_product_is_carried_in_order(self):
        self.assertEqual([p["id"] for p in self.builds["products"]],
                         [p["id"] for p in MATRIX["products"]])

    def test_website_invariants_hold(self):
        b = self.builds
        cat = b["featureCatalog"]
        for k in ("selftest", "peek", "mic", "csi", "gps", "thermal", "battery", "mesh",
                  "bluetooth"):
            self.assertIn(k, cat)
            self.assertIsInstance(cat[k], str)   # checkup.js renders it as a pill label
        by_id = {p["id"]: p for p in b["products"]}
        for p in b["products"]:
            builds = list(p["levels"].values()) if p["hasLevels"] else [p["build"]]
            self.assertEqual(p["hasLevels"], "levels" in p)
            self.assertNotEqual(p["hasLevels"], "build" in p)
            for bl in builds:
                self.assertIn("selftest", bl["features"], p["id"])
                for f in bl["features"]:
                    self.assertIn(f, cat, f"{p['id']}: feature {f} not in the catalog")
            if p["flow"] in ("serial", "vision"):
                self.assertFalse({"bluetooth", "mesh"} & set(p["build"]["features"]), p["id"])
            self.assertIn(p["tier"], ("verified", "compile-tested"))
            self.assertTrue(p["boardName"], p["id"])
        canary = by_id["canary"]
        self.assertTrue(canary["hasLevels"])
        for lvl in ("dev", "release"):
            self.assertFalse({"bluetooth", "mesh"} & set(canary["levels"][lvl]["features"]))
        self.assertTrue({"bluetooth", "mesh"} <= set(canary["levels"]["full"]["features"]))
        rec = b["recommended"]
        self.assertIn(rec["product"], by_id)
        self.assertIn(rec["level"], by_id[rec["product"]]["levels"])
        self.assertTrue(rec["why"])
        self.assertTrue(b["flasherUrl"].startswith("https://") and b["flasherUrl"].endswith("flash.html"))
        self.assertEqual(b["flasherProductPrefix"], "securacv-")
        self.assertTrue(b["_comment"].startswith("GENERATED by securaCV scripts/carry_to_site.py"))

    def test_manifest_basename_matches_upstream_url(self):
        for p in self.builds["products"]:
            if "manifestUrl" in p:
                self.assertTrue(p["manifestUrl"].endswith("/" + p["manifest"]))
            else:
                self.assertNotIn("manifest", p)

    def test_unknown_board_fails_loudly(self):
        broken = json.loads(json.dumps(MATRIX))
        broken["products"][0]["board"] = "no-such-board"
        with self.assertRaises(SystemExit):
            cs.project_builds(broken, BOARDS)


class Stamping(unittest.TestCase):
    NEW = {"_comment": "new", "products": [{"id": "canary", "hasLevels": False}]}

    def test_replaces_only_the_top_level_builds_value(self):
        out = cs.stamp_builds(SAMPLE_SPEC, self.NEW)
        parsed = json.loads(out)
        self.assertEqual(parsed["builds"], self.NEW)
        # everything before the block, and everything after it, byte-identical
        head, tail = SAMPLE_SPEC.split('"builds": {\n    "_comment": "old hand copy",')
        tail = tail.split('\n  },\n', 1)[1]
        self.assertTrue(out.startswith(head + '"builds": {\n'), out)
        self.assertTrue(out.endswith('\n  },\n' + tail), out)
        # the nested "builds" keys deeper in the file were not touched
        self.assertEqual(parsed["tools"]["list"][0]["builds"], {"nested": True})
        self.assertEqual(parsed["provenance"]["builds"], "firmware/build_matrix.json")

    def test_block_is_indented_like_the_key(self):
        out = cs.stamp_builds(SAMPLE_SPEC, self.NEW)
        self.assertIn('\n  "builds": {\n    "_comment": "new",\n    "products": [\n      {\n', out)

    def test_idempotent(self):
        once = cs.stamp_builds(SAMPLE_SPEC, self.NEW)
        self.assertEqual(cs.stamp_builds(once, self.NEW), once)

    def test_missing_key_is_a_keyerror(self):
        with self.assertRaises(KeyError):
            cs.locate_top_level_value('{"a": {"builds": 1}}', "builds")

    def test_locates_scalar_and_string_values_too(self):
        text = '{ "a": "x{y", "n": 12, "b": [1, {"c": "]"}], "last": null }'
        for key, want in (("a", '"x{y"'), ("n", "12"), ("b", '[1, {"c": "]"}]'),
                          ("last", "null")):
            s, e = cs.locate_top_level_value(text, key)
            self.assertEqual(text[s:e], want)


class JsMirror(unittest.TestCase):
    def test_header_and_body_shape(self):
        js = cs.js_mirror(SAMPLE_SPEC)
        self.assertTrue(js.startswith(cs.SPEC_JS_HEADER + "window.ONBOARDING_SPEC = {\n"))
        self.assertTrue(js.endswith("\n};\n"))
        body = js[len(cs.SPEC_JS_HEADER) + len("window.ONBOARDING_SPEC = "):-2]
        self.assertEqual(body, json.dumps(json.loads(SAMPLE_SPEC), indent=2, ensure_ascii=False))
        self.assertEqual(json.loads(body), json.loads(SAMPLE_SPEC))

    def test_header_names_the_generator(self):
        self.assertIn("carry_to_site.py", cs.SPEC_JS_HEADER)
        self.assertIn("do not edit by hand", cs.SPEC_JS_HEADER)


class KernelStatusDocument(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ks = cs.kernel_status_module()
        cls.doc = cls.ks.site_document()

    def test_ordered_done_wip_planned_with_evidence(self):
        rank = self.ks.RANK
        ranks = [rank[t["status"]] for t in self.doc["tiles"]]
        self.assertEqual(ranks, sorted(ranks))
        self.assertEqual(len(self.doc["tiles"]), len(self.ks.TILES))
        for t in self.doc["tiles"]:
            self.assertGreater(len(t["evidence"]), 20, t["label"])
        self.assertEqual(self.doc["glyphs"], self.ks.GLYPH)
        self.assertEqual(set(self.doc["rules"]), {"done", "wip", "planned"})

    def test_no_date_no_sha(self):
        self.assertEqual(set(self.doc), {"_comment", "source", "rules", "glyphs", "tiles"})

    def test_every_tile_has_an_evidence_sentence_for_every_status_it_can_return(self):
        # Which statuses each detector can return is written in the detector;
        # the table below is that contract, so a detector that grows a new
        # verdict fails here and gets its sentence written.
        can_return = {
            "Frame isolation types": {"done", "wip", "planned"},
            "Hash-chained event log": {"done", "planned"},
            "Break-glass quorum": {"done", "planned"},
            "Event contract enforcement": {"done", "planned"},
            "Cryptographic signatures": {"done", "planned"},
            "Encrypted vault envelopes": {"done", "planned"},
            "RTSP video ingestion": {"done", "wip", "planned"},
            "WASM module sandboxing": {"done", "wip", "planned"},
        }
        self.assertEqual({label for label, _fn in self.ks.TILES}, set(can_return))
        for label, statuses in can_return.items():
            self.assertEqual(set(self.ks.EVIDENCE[label]), statuses, label)
            for s in statuses:
                self.assertGreater(len(self.ks.evidence_for(label, s)), 20)

    def test_unknown_status_dies(self):
        with self.assertRaises(SystemExit):
            self.ks.evidence_for("Hash-chained event log", "wip")


class EndToEnd(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="carry_to_site_"))
        (self.tmp / "js").mkdir()
        (self.tmp / "tests").mkdir()
        (self.tmp / "tv" / "vendor").mkdir(parents=True)
        (self.tmp / "onboarding-spec.json").write_text(SAMPLE_SPEC, encoding="utf-8")

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def snapshot(self) -> dict:
        return {str(p.relative_to(self.tmp)): p.read_bytes()
                for p in sorted(self.tmp.rglob("*")) if p.is_file()}

    def run_carry(self, *extra):
        with redirect_stdout(io.StringIO()) as out:
            cs.main(["--site", str(self.tmp), *extra])
        return out.getvalue()

    def test_full_run_is_byte_reproducible_and_complete(self):
        first_out = self.run_carry()
        first = self.snapshot()
        self.assertEqual(self.run_carry(), first_out)
        self.assertEqual(self.snapshot(), first, "a second carry changed bytes")
        self.assertEqual(set(first), {
            "onboarding-spec.json", "js/onboarding-spec.js", "kernel-status.json",
            "tv/vendor/verify_core.js", "tv/vendor/PROVENANCE.txt",
            *(f"tests/fixtures/envelope/{f}" for f in cs.ENVELOPE_FIXTURES),
            *(f"tests/fixtures/export_bundle/{f}" for f in cs.BUNDLE_FIXTURES),
        })
        # the verifier and its fixtures are the upstream bytes, and PROVENANCE says so
        self.assertEqual(first["tv/vendor/verify_core.js"], cs.VERIFIER.read_bytes())
        prov = first["tv/vendor/PROVENANCE.txt"].decode()
        self.assertIn(f"  verify_core.js  sha256:{sha256(cs.VERIFIER)}\n", prov)
        for f in cs.ENVELOPE_FIXTURES:
            src = cs.FIXTURES / "envelope" / f
            self.assertEqual(first[f"tests/fixtures/envelope/{f}"], src.read_bytes())
            self.assertIn(f"  envelope/{f}  sha256:{sha256(src)}\n", prov)
        for f in cs.BUNDLE_FIXTURES:
            src = cs.FIXTURES / "export_bundle" / f
            self.assertEqual(first[f"tests/fixtures/export_bundle/{f}"], src.read_bytes())
            self.assertIn(f"  export_bundle/{f}  sha256:{sha256(src)}\n", prov)
        self.assertIsNone(re.search(r"\b20\d\d\b", prov), "no date in the provenance file")
        # the stamped spec parses, carries the projection, and kept its own keys
        spec_json = json.loads(first["onboarding-spec.json"])
        self.assertEqual(spec_json["builds"], cs.project_builds(MATRIX, BOARDS))
        self.assertEqual(spec_json["drive"]["name"], "CANARY-EVIDENCE")
        self.assertEqual(json.loads(first["kernel-status.json"]),
                         cs.kernel_status_module().site_document())

    def test_only_selects_a_single_carry(self):
        self.run_carry("--only", "verifier")
        self.assertEqual(set(self.snapshot()) & {"js/onboarding-spec.js", "kernel-status.json"},
                         set())
        self.assertTrue((self.tmp / "tv" / "vendor" / "verify_core.js").is_file())

    def test_refuses_a_directory_that_is_not_the_website(self):
        other = Path(tempfile.mkdtemp(prefix="not_site_"))
        try:
            with self.assertRaises(SystemExit):
                with redirect_stdout(io.StringIO()):
                    cs.main(["--site", str(other)])
        finally:
            shutil.rmtree(other)


if __name__ == "__main__":
    unittest.main()
