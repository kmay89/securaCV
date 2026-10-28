#!/usr/bin/env python3
"""Pins scripts/carry_to_site.py — the one command that refreshes every fact the
website carries from this repo (the /checkup build matrix, the landing page's
kernel-status grid, the Witness Wall's vendored verifier and the fleet contract
vectors its parseFleet replays).

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
    upstream with matching PROVENANCE sha256 lines;
  • the fleet contract vectors ride the verifier carry: they land at the one
    path the website's tests/tv-wall.test.mjs reads, every PROVENANCE pin
    resolves the way that test resolves it (so the website's existing sha256
    check covers the new file with no edit there), a renamed upstream file
    still lands at that path, the upstream file keeps the shape the website's
    replay reads, and a missing upstream file stops the verifier carry before
    it writes any of its files (in a default run the builds and kernel-status
    carries ahead of it have already written theirs; the job's set -e keeps
    that run from being committed);
  • the playground carry writes the three files the website's /playground and
    /modes pages import, at the site paths those pages name: playground.json
    as `export const PLAYGROUND = ` + the JSON with its trailing newline
    replaced by `;\n` (so the module evaluates to exactly the upstream object),
    the two sims as the upstream bytes whole, each under a header that says
    CARRIED, names its upstream file and this tool within the twelve lines the
    website's tests/carry-claims.test.mjs reads, and never says HAND-COPIED;
    a missing or unparsable upstream file stops it before any of its writes;
  • the downloads carry moves each app-v<x.y.z> / flasher-v<x.y.z> token in
    download.html to its family's newest PUBLISHED release (not a draft, not a
    prerelease, not an -rc tag, not one without its installers uploaded;
    numeric order, not the API's listing order) and nothing else, so the
    website's tests/download-facts.test.mjs reads one version per family; a
    current page keeps its bytes; --check exits 1 on a stale page and writes
    nothing; a release list that cannot be read, one that shows no published
    release of a family, or a page that names no token of a family, leaves
    the page as it was and says why (and fails --check), and a default run
    still writes every other carry; the API reader follows every page, sends
    GH_TOKEN when set, retries a network error, a body cut off or not JSON, a
    5xx and a 429, and takes any other HTTP status, or an answer that is not
    a list, as final. No test here reaches the network.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import hashlib
import http.client
import importlib.util
import io
import json
import os
import re
import shutil
import tempfile
import unittest
import urllib.error
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "carry_to_site.py"

spec = importlib.util.spec_from_file_location("carry_to_site", SCRIPT)
cs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cs)  # type: ignore[union-attr]

MATRIX = json.loads(cs.BUILD_MATRIX.read_text(encoding="utf-8"))
BOARDS = json.loads(cs.BOARDS.read_text(encoding="utf-8"))

# The fleet contract vectors: where they live here (the Rust core's
# tests/fleet_contract.rs replays them) and the one site path the website's
# tests/tv-wall.test.mjs reads them from (its FLEET_VECTORS constant). Stated
# here, not read off the script, so a carry that lands them anywhere else fails.
UPSTREAM_VECTORS = REPO / "tvos" / "witness-core" / "tests" / "fixtures" / "fleet_contract_vectors.json"
SITE_VECTORS = "tests/fixtures/fleet_contract_vectors.json"

# The playground carry: site path (what the website's pages import, and what
# its update-everything.yml CARRY_PATHS and tests/carry-claims.test.mjs name)
# -> the upstream file it carries. Stated here, not read off the script.
PLAYGROUND_FILES = {
    "js/playground-data.js": REPO / "canary-local" / "devices" / "playground.json",
    "js/playground-sim.js": REPO / "canary-local" / "assets" / "playground-sim.js",
    "js/mode-sim.js": REPO / "canary-local" / "assets" / "mode-sim.js",
}
# How the website's tests/carry-claims.test.mjs reads a header: the first
# twelve lines of the file.
WEBSITE_HEADER_LINES = 12

# How the website's tests/tv-wall.test.mjs reads tv/vendor/PROVENANCE.txt: one
# pin per line, and a pinned name resolves to tv/vendor/verify_core.js or to
# tests/fixtures/<name>. Mirrored here so a carried file whose pin the website
# cannot resolve fails in this repo, not in a weekly carry PR.
WEBSITE_PIN = re.compile(r"^ {2}(\S+) {2}sha256:([0-9a-f]{64})$", re.M)


def website_pin_path(name: str) -> str:
    return "tv/vendor/verify_core.js" if name == "verify_core.js" else "tests/fixtures/" + name

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


# A miniature of the website's download.html: both token families in hrefs and
# in the note, mixed with version text that is NOT a token and must not move
# (the bare "since 0.12.0", "since Lab 0.3.0", a firmware tag, the rolling
# pointers, the `flasher-v` prefix with no version, and two strings the
# website test's word boundaries do not count).
SAMPLE_DOWNLOAD = (
    '<!doctype html>\n<title>Download</title>\n'
    '<a class="go" href="https://github.com/kmay89/securaCV/releases/tag/app-v0.3.1">macOS</a>\n'
    '<a class="go" href="https://github.com/kmay89/securaCV/releases/tag/app-v0.3.1">Linux</a>\n'
    '<p>Its own release train (tags starting <code>flasher-v</code>).</p>\n'
    '<a class="go" href="https://github.com/kmay89/securaCV/releases/tag/flasher-v0.11.9">Get</a>\n'
    '<div class="alt">It reads the fuses (since 0.12.0).</div>\n'
    '<p>All builds live on the releases page: <code>app-v0.3.1</code>, the Flasher\n'
    '<code>flasher-v0.11.9</code>. Since Lab 0.3.0, USB flashing; fw-v2.4.17;\n'
    'lab-latest and flasher-latest are the updater pointers.</p>\n'
    '<!-- not tokens: webapp-v9.9.9, app-v1.2.3x -->\n'
)
# The oracle, by plain replacement rather than the carry's own pattern.
STAMPED_DOWNLOAD = (SAMPLE_DOWNLOAD.replace("app-v0.3.1", "app-v0.3.2")
                    .replace("flasher-v0.11.9", "flasher-v0.12.2"))
# The page as website main still has it: the buttons open GitHub's release
# search, so no token names a version and there is nothing to stamp.
SEARCH_DOWNLOAD = (
    '<!doctype html>\n<title>Download</title>\n'
    '<a class="go" href="https://github.com/kmay89/securaCV/releases?q=app-v&amp;expanded=true">'
    'macOS</a>\n'
    '<a class="go" href="https://github.com/kmay89/securaCV/releases?q=flasher-v&amp;expanded=true">'
    'Get</a>\n'
    '<p>The newest <code>app-v</code> release; the Flasher the newest <code>flasher-v</code>.</p>\n'
)

# What a release carries once its builds are up (today's app-v0.3.2 and
# flasher-v0.12.2 have these, versioned): the three installers the page names,
# with signatures, the updater bundle and manifest that do not stand in for them.
ASSETS = [{"name": n, "state": "uploaded"} for n in (
    "SecuraCV.App_1.0.0_universal.dmg", "SecuraCV.App_1.0.0_universal.app.tar.gz",
    "SecuraCV.App_1.0.0_amd64.AppImage", "SecuraCV.App_1.0.0_amd64.AppImage.sig",
    "SecuraCV.App_1.0.0_amd64.deb", "SecuraCV.App_1.0.0_amd64.deb.sig", "latest.json")]
PUBLISHED = {"draft": False, "prerelease": False, "published_at": "2026-10-27T04:43:13Z",
             "assets": ASSETS}
# The release list as the API answers it: newest first by date, not by
# version, with drafts (one re-drafted after it was published, so it keeps a
# publish date), prereleases, -rc tags (one not flagged prerelease), a row
# with no flags, one with no publish date, published releases whose installers
# are not all up (none yet, Linux only, a .dmg still uploading), the rolling
# pointers and the firmware mixed in. The newest published are app-v0.3.2 and
# flasher-v0.12.2; a string compare would pick flasher-v0.4.1.
RELEASES = [
    {"tag_name": "flasher-v0.15.0", **PUBLISHED, "assets": []},
    {"tag_name": "flasher-v0.14.1", **PUBLISHED,
     "assets": [a for a in ASSETS if "universal" not in a["name"]]},
    {"tag_name": "app-v0.4.1", **PUBLISHED,
     "assets": [{**a, "state": "open"} if a["name"].endswith(".dmg") else a for a in ASSETS]},
    {"tag_name": "app-v0.4.0", "draft": True, "prerelease": False, "published_at": None},
    {"tag_name": "flasher-v0.14.0", **PUBLISHED, "draft": True},
    {"tag_name": "app-v0.5.0-rc.1", **PUBLISHED},
    {"tag_name": "flasher-v0.13.0-rc.1", **PUBLISHED, "prerelease": True},
    {"tag_name": "flasher-v0.13.0", **PUBLISHED, "prerelease": True},
    {"tag_name": "app-v0.9.0"},
    {"tag_name": "app-v0.8.0", **PUBLISHED, "published_at": None},
    {"tag_name": "flasher-v0.12.2", **PUBLISHED},
    {"tag_name": "app-v0.3.2", **PUBLISHED},
    {"tag_name": "flasher-v0.4.1", **PUBLISHED},
    {"tag_name": "flasher-v0.11.0", **PUBLISHED},
    {"tag_name": "lab-latest", **PUBLISHED, "prerelease": True},
    {"tag_name": "flasher-latest", **PUBLISHED, "prerelease": True},
    {"tag_name": "fw-v2.4.17", **PUBLISHED},
    {"tag_name": "app-v0.3.1", **PUBLISHED},
]


def website_versions(html: str, prefix: str) -> set[str]:
    """The website's tests/download-facts.test.mjs `versionsOf`, mirrored:
    every version a family's tokens name. It requires exactly one."""
    return set(re.findall(rf"\b{prefix}(\d+\.\d+\.\d+)\b", html, re.ASCII))


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
        (self.tmp / "download.html").write_text(SAMPLE_DOWNLOAD, encoding="utf-8")
        # A default run includes the downloads carry: answer it here, never
        # from the network.
        releases = mock.patch.object(cs, "read_releases", return_value=RELEASES)
        self.releases = releases.start()
        self.addCleanup(releases.stop)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def snapshot(self) -> dict:
        return {str(p.relative_to(self.tmp)): p.read_bytes()
                for p in sorted(self.tmp.rglob("*")) if p.is_file()}

    def run_carry(self, *extra):
        with redirect_stdout(io.StringIO()) as out:
            cs.main(["--site", str(self.tmp), *extra])
        return out.getvalue()

    def assert_playground_bytes(self, snap: dict):
        """What the weekly job commits is the written file, so hold the bytes
        on disk to an oracle built here from the upstream files, not to
        playground_outputs() (a writer that empties, swaps or re-ends the
        files would still agree with that)."""
        data_rel = "js/playground-data.js"
        upstream_json = PLAYGROUND_FILES[data_rel].read_bytes()
        self.assertTrue(upstream_json.endswith(b"}\n"))
        self.assertEqual(
            snap[data_rel],
            cs.PLAYGROUND_DATA_HEADER.encode("utf-8")
            + b"export const PLAYGROUND = " + upstream_json[:-1] + b";\n",
            f"{data_rel}: not the header, the export and the upstream JSON")
        for rel in ("js/playground-sim.js", "js/mode-sim.js"):
            upstream = PLAYGROUND_FILES[rel].read_bytes()
            written = snap[rel]
            self.assertTrue(written.endswith(upstream), f"{rel}: not the upstream bytes, whole")
            header = written[:-len(upstream)]
            self.assertTrue(header.startswith(f"// {rel} — ".encode("utf-8")), rel)
            self.assertIn(b"CARRIED", header, rel)
            self.assertTrue(header.endswith(b"\n\n"), f"{rel}: a blank line closes the header")
            self.assertNotIn(b"\r", header, f"{rel}: the header is written with LF line ends")

    EVERY_FILE = {
        "onboarding-spec.json", "js/onboarding-spec.js", "kernel-status.json",
        "download.html",
        "tv/vendor/verify_core.js", "tv/vendor/PROVENANCE.txt",
        *(f"tests/fixtures/envelope/{f}" for f in cs.ENVELOPE_FIXTURES),
        *(f"tests/fixtures/export_bundle/{f}" for f in cs.BUNDLE_FIXTURES),
        SITE_VECTORS,
        *PLAYGROUND_FILES,
    }

    def test_full_run_is_byte_reproducible_and_complete(self):
        first_out = self.run_carry()
        first = self.snapshot()
        self.assertEqual(self.run_carry(), first_out)
        self.assertEqual(self.snapshot(), first, "a second carry changed bytes")
        self.assertEqual(set(first), self.EVERY_FILE)
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
        self.assertEqual(first[SITE_VECTORS], UPSTREAM_VECTORS.read_bytes())
        self.assertIn(f"  fleet_contract_vectors.json  sha256:{sha256(UPSTREAM_VECTORS)}\n", prov)
        self.assertIsNone(re.search(r"\b20\d\d\b", prov), "no date in the provenance file")
        # the stamped spec parses, carries the projection, and kept its own keys
        spec_json = json.loads(first["onboarding-spec.json"])
        self.assertEqual(spec_json["builds"], cs.project_builds(MATRIX, BOARDS))
        self.assertEqual(spec_json["drive"]["name"], "CANARY-EVIDENCE")
        self.assertEqual(json.loads(first["kernel-status.json"]),
                         cs.kernel_status_module().site_document())
        self.assert_playground_bytes(first)
        self.assertEqual(first["download.html"], STAMPED_DOWNLOAD.encode("utf-8"))

    def test_a_page_with_no_release_tokens_is_left_and_every_other_carry_lands(self):
        # The weekly job's default run against website main, whose buttons
        # still open the release search: one carry with nothing to stamp
        # must not exit 1 and stop the rest from reaching the carry PR.
        (self.tmp / "download.html").write_text(SEARCH_DOWNLOAD, encoding="utf-8")
        with redirect_stderr(io.StringIO()) as err:
            out = self.run_carry()
        snap = self.snapshot()
        self.assertEqual(set(snap), self.EVERY_FILE)
        self.assertEqual(snap["download.html"], SEARCH_DOWNLOAD.encode("utf-8"))
        written = {line.removeprefix("wrote ") for line in out.splitlines()}
        self.assertEqual(written, self.EVERY_FILE - {"download.html"})
        self.assertIn("download.html left as it is — it names no app-v<x.y.z> to move",
                      err.getvalue())
        self.releases.assert_not_called()

    def test_only_selects_a_single_carry(self):
        self.run_carry("--only", "verifier")
        self.assertEqual(set(self.snapshot()) & {"js/onboarding-spec.js", "kernel-status.json"},
                         set())
        self.assertTrue((self.tmp / "tv" / "vendor" / "verify_core.js").is_file())

    def test_verifier_carry_brings_the_fleet_contract_vectors(self):
        # The website's Wall replays these through parseFleet; before this
        # carry they were a hand copy pinned by a sha256 a human had to move.
        self.run_carry("--only", "verifier")
        self.assertEqual((self.tmp / SITE_VECTORS).read_bytes(), UPSTREAM_VECTORS.read_bytes())

    def test_every_carried_file_has_a_pin_the_website_resolves(self):
        # The website's tv-wall.test.mjs hashes every PROVENANCE pin at the path
        # it resolves the name to. Every file the verifier carry writes must be
        # pinned exactly once, resolvable that way, at its own sha256 — so the
        # website's existing check covers each carried file with no edit there.
        self.run_carry("--only", "verifier")
        prov = (self.tmp / "tv" / "vendor" / "PROVENANCE.txt").read_text(encoding="utf-8")
        pins = WEBSITE_PIN.findall(prov)
        paths = [website_pin_path(name) for name, _sha in pins]
        self.assertEqual(len(paths), len(set(paths)), "a file is pinned twice")
        carried = {p for p in self.snapshot() if p != "tv/vendor/PROVENANCE.txt"
                   and p not in ("onboarding-spec.json", "download.html")}
        self.assertEqual(set(paths), carried)
        self.assertIn(SITE_VECTORS, paths)
        for (name, sha), path in zip(pins, paths):
            self.assertEqual(sha256(self.tmp / path), sha, name)

    def test_an_upstream_rename_still_lands_where_the_wall_test_reads(self):
        # The site path is the website's contract, not the upstream file name:
        # when FLEET_VECTORS follows a renamed upstream file, the copy must
        # still land at SITE_VECTORS (and be pinned there), never at a new name
        # the website's replay never reads.
        renamed = self.tmp / "renamed_upstream_vectors.json"
        renamed.write_bytes(UPSTREAM_VECTORS.read_bytes() + b"\n")
        with mock.patch.object(cs, "FLEET_VECTORS", renamed):
            self.run_carry("--only", "verifier")
        renamed.unlink()
        self.assertEqual(sorted(p.name for p in (self.tmp / "tests" / "fixtures").glob("*.json")),
                         ["fleet_contract_vectors.json"])
        self.assertEqual((self.tmp / SITE_VECTORS).read_bytes(), UPSTREAM_VECTORS.read_bytes() + b"\n")
        prov = (self.tmp / "tv" / "vendor" / "PROVENANCE.txt").read_text(encoding="utf-8")
        self.assertIn(f"  fleet_contract_vectors.json  sha256:{sha256(self.tmp / SITE_VECTORS)}\n", prov)

    def test_missing_upstream_vectors_stop_the_verifier_carry_before_any_of_its_writes(self):
        # The upstream file moved and FLEET_VECTORS did not follow: the
        # verifier carry names the missing file and writes none of its files,
        # rather than dying half way through and leaving a partial carry.
        # The claim is the verifier carry's, not the whole run's: in a default
        # run (what the weekly job runs) the builds and kernel-status carries
        # go first and have already written theirs, so both forms are pinned.
        missing = UPSTREAM_VECTORS.with_name("no_such_fleet_contract_vectors.json")
        self.assertFalse(missing.exists())

        def verifier_files(snap: dict) -> dict:
            return {p: b for p, b in snap.items() if p.startswith(("tv/", "tests/"))}

        for extra in (("--only", "verifier"), ()):
            with self.subTest(run=" ".join(extra) or "default (every carry)"):
                before = self.snapshot()
                with mock.patch.object(cs, "FLEET_VECTORS", missing):
                    with self.assertRaises(SystemExit) as stop:
                        self.run_carry(*extra)
                self.assertIn("no_such_fleet_contract_vectors.json", str(stop.exception.code))
                after = self.snapshot()
                self.assertEqual(verifier_files(after), verifier_files(before),
                                 "a failed verifier carry left a partial copy behind")
                if extra:
                    self.assertEqual(after, before, "--only verifier left a write behind")

    def test_only_playground_writes_exactly_its_three_files(self):
        self.run_carry("--only", "playground")
        snap = self.snapshot()
        self.assertEqual(set(snap), {"onboarding-spec.json", "download.html", *PLAYGROUND_FILES})
        self.assert_playground_bytes(snap)

    def test_a_missing_or_broken_upstream_file_stops_the_playground_carry_before_any_write(self):
        # Every source is checked before the first write: the website keeps
        # its three files as they were, never a mix of old and new.
        for rel in PLAYGROUND_FILES:
            (self.tmp / rel).write_text(f"// the website's current {rel}\n", encoding="utf-8")
        before = self.snapshot()
        missing = REPO / "canary-local" / "assets" / "no_such_mode_sim.js"
        broken = self.tmp / "broken_playground.json"
        broken.write_text('{"board": ', encoding="utf-8")
        not_object = self.tmp / "list_playground.json"
        not_object.write_text("[1, 2]\n", encoding="utf-8")
        cases = (("MODE_SIM", missing, "no_such_mode_sim.js"),
                 ("PLAYGROUND_JSON", broken, "not JSON"),
                 ("PLAYGROUND_JSON", not_object, "not a JSON object"))
        for attr, path, says in cases:
            with self.subTest(attr=attr, path=path.name):
                with mock.patch.object(cs, attr, path):
                    with self.assertRaises(SystemExit) as stop:
                        self.run_carry("--only", "playground")
                self.assertIn(says, str(stop.exception.code))
                after = {p: b for p, b in self.snapshot().items()
                         if not p.endswith("_playground.json")}
                before_files = {p: b for p, b in before.items()
                                if not p.endswith("_playground.json")}
                self.assertEqual(after, before_files, "a stopped playground carry wrote a file")

    def test_refuses_a_directory_that_is_not_the_website(self):
        other = Path(tempfile.mkdtemp(prefix="not_site_"))
        try:
            with self.assertRaises(SystemExit):
                with redirect_stdout(io.StringIO()):
                    cs.main(["--site", str(other)])
        finally:
            shutil.rmtree(other)


class PlaygroundCarry(unittest.TestCase):
    """The three files the website's /playground and /modes pages import."""

    @classmethod
    def setUpClass(cls):
        cls.out = cs.playground_outputs()

    def test_writes_the_site_paths_the_pages_import(self):
        self.assertEqual(set(self.out), set(PLAYGROUND_FILES))

    def header(self, rel: str) -> str:
        return "\n".join(self.out[rel].split("\n")[:WEBSITE_HEADER_LINES])

    def test_every_header_says_carried_names_its_upstream_and_this_tool(self):
        for rel, upstream in PLAYGROUND_FILES.items():
            h = self.header(rel)
            self.assertRegex(h, r"\bCARRIED\b", rel)
            self.assertIn(str(upstream.relative_to(REPO)), h, rel)
            self.assertIn("scripts/carry_to_site.py", h, rel)
            self.assertNotIn("HAND-COPIED", self.out[rel].split("export const")[0], rel)
            self.assertTrue(self.out[rel].startswith(f"// {rel} — "), rel)
            self.assertIsNone(re.search(r"\b20\d\d\b", h), f"{rel}: no date in the header")

    def test_data_module_is_the_upstream_json_with_its_newline_made_a_semicolon(self):
        rel = "js/playground-data.js"
        src = PLAYGROUND_FILES[rel].read_text(encoding="utf-8")
        self.assertTrue(src.endswith("}\n"), "gen_playground.py writes one object and a newline")
        head, sep, body = self.out[rel].partition("\nexport const PLAYGROUND = ")
        self.assertTrue(sep, "the module exports PLAYGROUND")
        self.assertEqual(body, src[:-1] + ";\n")
        self.assertEqual(json.loads(body[:-2]), json.loads(src))
        self.assertNotIn("export const PLAYGROUND = ", head.replace("`export const PLAYGROUND =`", ""))
        self.assertEqual(self.out[rel], cs.PLAYGROUND_DATA_HEADER + "export const PLAYGROUND = " + body)

    def test_data_module_evaluates_to_the_upstream_object(self):
        # The page imports it as an ES module; node, when present, proves the
        # carried text is JavaScript that evaluates to exactly the JSON.
        node = shutil.which("node")
        if not node:
            self.skipTest("node is not installed")
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            mod = Path(d) / "playground-data.mjs"
            mod.write_text(self.out["js/playground-data.js"], encoding="utf-8")
            got = subprocess.run(
                [node, "--input-type=module", "-e",
                 f"import {{ PLAYGROUND }} from {json.dumps(mod.as_uri())};"
                 "process.stdout.write(JSON.stringify(PLAYGROUND));"],
                check=True, capture_output=True, text=True).stdout
        self.assertEqual(json.loads(got),
                         json.loads(PLAYGROUND_FILES["js/playground-data.js"].read_text(encoding="utf-8")))

    def test_sims_are_the_upstream_bytes_whole_under_the_header(self):
        for rel in ("js/playground-sim.js", "js/mode-sim.js"):
            src = PLAYGROUND_FILES[rel].read_text(encoding="utf-8")
            self.assertTrue(self.out[rel].endswith(src), rel)
            header = self.out[rel][:-len(src)]
            self.assertTrue(header.endswith("\n\n"), f"{rel}: a blank line closes the header")
            self.assertEqual(header.count("\n") + 1, len(header.split("\n")))
            self.assertLessEqual(header.count("\n"), WEBSITE_HEADER_LINES, rel)
            # the website's carry-claims test finds the upstream file's own
            # first line right after the header: the copy is whole
            first = src.split("\n", 1)[0]
            self.assertTrue(first.startswith(f"// {PLAYGROUND_FILES[rel].relative_to(REPO)} — "), rel)
            self.assertIn("\n" + first + "\n", self.out[rel])


class DownloadsStamp(unittest.TestCase):
    """Which release is the newest published one, and what the stamp moves."""

    def test_newest_published_is_the_highest_stable_release_the_api_says_is_published(self):
        self.assertEqual(cs.newest_published(RELEASES, "app-v"), "0.3.2")
        self.assertEqual(cs.newest_published(RELEASES, "flasher-v"), "0.12.2")

    def test_a_family_with_nothing_published_has_no_newest(self):
        self.assertIsNone(cs.newest_published([], "app-v"))
        unpublished = [r for r in RELEASES if r["tag_name"] in
                       ("app-v0.4.1", "app-v0.4.0", "app-v0.5.0-rc.1", "app-v0.8.0",
                        "app-v0.9.0", "flasher-v0.15.0", "flasher-v0.14.1",
                        "flasher-v0.14.0", "flasher-v0.13.0", "flasher-v0.13.0-rc.1")]
        self.assertIsNone(cs.newest_published(unpublished, "app-v"))
        self.assertIsNone(cs.newest_published(unpublished, "flasher-v"))

    def test_a_published_release_counts_only_with_every_installer_uploaded(self):
        # desktop-flasher-release.yml creates the release published before
        # either build leg uploads, and a failed run leaves it that way.
        last_good = {"tag_name": "flasher-v0.12.2", **PUBLISHED}
        for why, assets in (
                ("nothing uploaded", []),
                ("no .dmg", [a for a in ASSETS if not a["name"].endswith(".dmg")]),
                ("only the .AppImage's signature", [a for a in ASSETS
                                                    if not a["name"].endswith(".AppImage")]),
                ("no .deb", [a for a in ASSETS if not a["name"].endswith(".deb")]),
                ("every asset still uploading", [{**a, "state": "open"} for a in ASSETS])):
            with self.subTest(why):
                newer = {"tag_name": "flasher-v0.13.0", **PUBLISHED, "assets": assets}
                self.assertEqual(cs.newest_published([newer, last_good], "flasher-v"), "0.12.2")
        newer = {"tag_name": "flasher-v0.13.0", **PUBLISHED}
        self.assertEqual(cs.newest_published([newer, last_good], "flasher-v"), "0.13.0")

    def test_stamp_moves_only_the_two_token_families(self):
        out = cs.stamp_downloads(SAMPLE_DOWNLOAD, {"app-v": "0.3.2", "flasher-v": "0.12.2"})
        self.assertEqual(out, STAMPED_DOWNLOAD)
        self.assertEqual(website_versions(out, "app-v"), {"0.3.2"})
        self.assertEqual(website_versions(out, "flasher-v"), {"0.12.2"})

    def test_stamp_is_idempotent_and_makes_a_mixed_page_name_one_version(self):
        mixed = SAMPLE_DOWNLOAD.replace("app-v0.3.1", "app-v0.2.4", 1)
        self.assertEqual(website_versions(mixed, "app-v"), {"0.2.4", "0.3.1"})
        newest = {"app-v": "0.3.2", "flasher-v": "0.12.2"}
        once = cs.stamp_downloads(mixed, newest)
        self.assertEqual(once, STAMPED_DOWNLOAD)
        self.assertEqual(cs.stamp_downloads(once, newest), once)

    def test_a_token_after_a_non_ascii_letter_moves_as_the_website_test_counts_it(self):
        # JavaScript's \b is ASCII, so the website test counts "éapp-v0.3.1";
        # Python's Unicode \b would not, and the two would disagree.
        page = "<p>éapp-v0.3.1, ñflasher-v0.11.9</p>\n"
        self.assertEqual(website_versions(page, "app-v"), {"0.3.1"})
        self.assertEqual(cs.stamp_downloads(page, {"app-v": "0.3.2", "flasher-v": "0.12.2"}),
                         "<p>éapp-v0.3.2, ñflasher-v0.12.2</p>\n")


class DownloadsCarry(unittest.TestCase):
    """The carry and its --check against a website-shaped tree."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="carry_downloads_"))
        self.addCleanup(shutil.rmtree, self.tmp)
        (self.tmp / "js").mkdir()
        (self.tmp / "tests").mkdir()
        self.page = self.tmp / "download.html"
        self.page.write_text(SAMPLE_DOWNLOAD, encoding="utf-8")
        self.releases = mock.patch.object(cs, "read_releases", return_value=RELEASES).start()
        mock.patch.dict(os.environ).start()
        self.addCleanup(mock.patch.stopall)
        os.environ.pop("GH_TOKEN", None)

    def run_main(self, *extra) -> tuple[str, str]:
        with redirect_stdout(io.StringIO()) as out, redirect_stderr(io.StringIO()) as err:
            cs.main(["--site", str(self.tmp), "--only", "downloads", *extra])
        return out.getvalue(), err.getvalue()

    def run_check(self) -> tuple[object, str]:
        with redirect_stdout(io.StringIO()) as out, redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as stop:
                cs.main(["--site", str(self.tmp), "--only", "downloads", "--check"])
        return stop.exception.code, out.getvalue()

    def test_stamps_an_older_page_to_the_newest_published_releases(self):
        out, _err = self.run_main()
        self.assertEqual(out, "wrote download.html\n")
        self.assertEqual(self.page.read_text(encoding="utf-8"), STAMPED_DOWNLOAD)

    def test_a_current_page_keeps_its_bytes(self):
        self.page.write_text(STAMPED_DOWNLOAD, encoding="utf-8")
        self.run_main()
        self.assertEqual(self.page.read_bytes(), STAMPED_DOWNLOAD.encode("utf-8"))
        self.assertEqual(self.run_check()[0], 0)

    def test_check_reports_a_stale_page_and_writes_nothing(self):
        code, out = self.run_check()
        self.assertEqual(code, 1)
        self.assertIn("names app-v0.3.1; the newest published release of the Lab is app-v0.3.2",
                      out)
        self.assertIn("names flasher-v0.11.9; the newest published release of the Flasher is "
                      "flasher-v0.12.2", out)
        self.assertEqual(self.page.read_text(encoding="utf-8"), SAMPLE_DOWNLOAD)

    def test_reads_the_releases_with_gh_token_when_it_is_set(self):
        self.run_main()
        self.releases.assert_called_with(None)
        os.environ["GH_TOKEN"] = "ghs_example"
        self.run_main()
        self.releases.assert_called_with("ghs_example")

    def test_an_unreadable_release_list_leaves_the_page_and_says_why(self):
        self.releases.side_effect = cs.ReleasesUnavailable("HTTP 503 from the API (3 attempts)")
        out, err = self.run_main()
        self.assertEqual(out, "")
        self.assertIn("download.html left as it is", err)
        self.assertIn("HTTP 503", err)
        self.assertIn("GH_TOKEN is not set", err)
        self.assertEqual(self.page.read_text(encoding="utf-8"), SAMPLE_DOWNLOAD)
        # and a check that cannot read the truth does not pass
        with redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as stop:
            cs.main(["--site", str(self.tmp), "--only", "downloads", "--check"])
        self.assertIn("cannot check download.html", str(stop.exception.code))

    def test_a_family_with_no_published_release_is_not_stamped(self):
        self.releases.return_value = [r for r in RELEASES
                                      if not r["tag_name"].startswith("flasher-v")]
        _out, err = self.run_main()
        self.assertIn("no published flasher-v<x.y.z> release of the Flasher", err)
        self.assertEqual(self.page.read_text(encoding="utf-8"), SAMPLE_DOWNLOAD)

    def test_a_page_without_a_family_token_is_left_and_fails_check(self):
        page = SAMPLE_DOWNLOAD.replace("flasher-v0.11.9", "the Flasher")
        self.page.write_text(page, encoding="utf-8")
        out, err = self.run_main()
        self.assertEqual(out, "")
        self.assertIn("download.html left as it is — it names no flasher-v<x.y.z> to move", err)
        self.assertEqual(self.page.read_text(encoding="utf-8"), page)
        self.releases.assert_not_called()
        # a check with nothing to move must not read as current
        with redirect_stdout(io.StringIO()), self.assertRaises(SystemExit) as stop:
            cs.main(["--site", str(self.tmp), "--only", "downloads", "--check"])
        self.assertIn("cannot check download.html — it names no flasher-v<x.y.z>",
                      str(stop.exception.code))

    def test_check_is_the_downloads_carry_alone(self):
        for args in (["--check"], ["--only", "builds", "--check"],
                     ["--only", "downloads", "--only", "builds", "--check"]):
            with self.subTest(args=args):
                with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as stop:
                    cs.main(["--site", str(self.tmp), *args])
                self.assertEqual(stop.exception.code, 2)
        self.assertEqual(self.page.read_text(encoding="utf-8"), SAMPLE_DOWNLOAD)


class FakeResponse:
    def __init__(self, body, link: str | None = None):
        # bytes are the body as sent (a malformed answer), an exception is
        # what read() raises (a body cut off), anything else is sent as JSON
        self.body = (body if isinstance(body, (bytes, Exception))
                     else json.dumps(body).encode("utf-8"))
        self.headers = {"Link": link} if link else {}

    def read(self) -> bytes:
        if isinstance(self.body, Exception):
            raise self.body
        return self.body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class ReleasesApi(unittest.TestCase):
    """The GitHub REST reader, with urlopen stood in: pages, token, retries."""

    def setUp(self):
        self.urlopen = mock.patch.object(cs.urllib.request, "urlopen").start()
        self.sleep = mock.patch.object(cs.time, "sleep").start()
        self.addCleanup(mock.patch.stopall)

    def http_error(self, code: int) -> urllib.error.HTTPError:
        return urllib.error.HTTPError(cs.RELEASES_API, code, "status", {}, None)

    def test_follows_every_page_and_sends_the_token(self):
        page2 = cs.RELEASES_API + "?per_page=100&page=2"
        self.urlopen.side_effect = [
            FakeResponse(RELEASES[:5], f'<{page2}>; rel="next", <{page2}>; rel="last"'),
            FakeResponse(RELEASES[5:], f'<{cs.RELEASES_API}?per_page=100&page=1>; rel="prev"'),
        ]
        self.assertEqual(cs.read_releases("ghs_example"), RELEASES)
        reqs = [c.args[0] for c in self.urlopen.call_args_list]
        self.assertEqual([r.full_url for r in reqs], [cs.RELEASES_API + "?per_page=100", page2])
        for r in reqs:
            self.assertEqual(r.get_header("Authorization"), "Bearer ghs_example")

    def test_no_token_sends_no_authorization(self):
        self.urlopen.side_effect = [FakeResponse(RELEASES)]
        cs.read_releases(None)
        self.assertIsNone(self.urlopen.call_args.args[0].get_header("Authorization"))

    def test_retries_a_network_error_and_a_5xx_then_answers(self):
        self.urlopen.side_effect = [urllib.error.URLError("reset"), self.http_error(502),
                                    FakeResponse(RELEASES)]
        self.assertEqual(cs.read_releases(None), RELEASES)
        self.assertEqual(self.urlopen.call_count, 3)
        self.assertEqual(self.sleep.call_count, 2)

    def test_retries_a_body_cut_off_mid_transfer(self):
        # IncompleteRead is an http.client.HTTPException, not an OSError
        self.urlopen.side_effect = [FakeResponse(http.client.IncompleteRead(b'[{"tag', 4000)),
                                    FakeResponse(RELEASES)]
        self.assertEqual(cs.read_releases(None), RELEASES)
        self.assertEqual(self.urlopen.call_count, 2)

    def test_retries_a_body_that_is_not_json(self):
        self.urlopen.side_effect = [FakeResponse(b"<html>unicorn</html>"),
                                    FakeResponse(RELEASES)]
        self.assertEqual(cs.read_releases(None), RELEASES)
        self.assertEqual(self.urlopen.call_count, 2)

    def test_gives_up_after_the_last_attempt(self):
        for why, answer in (("429", lambda: self.http_error(429)),
                            ("cut off", lambda: FakeResponse(http.client.IncompleteRead(b"["))),
                            ("not JSON", lambda: FakeResponse(b"<html>"))):
            with self.subTest(why):
                self.urlopen.reset_mock()
                self.urlopen.side_effect = [answer() for _ in range(cs.API_ATTEMPTS)]
                with self.assertRaises(cs.ReleasesUnavailable) as stop:
                    cs.read_releases(None)
                self.assertIn(f"{cs.API_ATTEMPTS} attempts", str(stop.exception))
                self.assertEqual(self.urlopen.call_count, cs.API_ATTEMPTS)

    def test_another_http_error_is_the_answer_not_retried(self):
        self.urlopen.side_effect = [self.http_error(401)]
        with self.assertRaises(cs.ReleasesUnavailable) as stop:
            cs.read_releases("bad")
        self.assertIn("HTTP 401", str(stop.exception))
        self.assertEqual(self.urlopen.call_count, 1)

    def test_an_answer_that_is_not_a_list_is_refused(self):
        self.urlopen.side_effect = [FakeResponse({"message": "Bad credentials"})]
        with self.assertRaises(cs.ReleasesUnavailable):
            cs.read_releases(None)


class FleetVectorsShape(unittest.TestCase):
    """The upstream vectors keep the shape the website's replay reads.

    tests/tv-wall.test.mjs parses each vector's string `input`, compares it to
    `normalized` (kernel / verified_through / devices), wants at least five
    vectors and at least four rows that say nothing about `online` (they must
    read offline). A vectors edit here that breaks that reader would reach the
    website as a red carry PR; this makes it red here, where it was made.
    """

    @classmethod
    def setUpClass(cls):
        cls.doc = json.loads(UPSTREAM_VECTORS.read_text(encoding="utf-8"))

    def test_the_website_replay_can_read_every_vector(self):
        vectors = self.doc["vectors"]
        self.assertGreaterEqual(len(vectors), 5)
        silent_rows = 0
        for v in vectors:
            self.assertIsInstance(v["name"], str)
            self.assertIsInstance(v["input"], str, v["name"])
            raw = json.loads(v["input"])
            rows = raw if isinstance(raw, list) else raw["devices"]
            want = v["normalized"]["devices"]
            self.assertEqual(len(rows), len(want), v["name"])
            for row, w in zip(rows, want):
                self.assertIsInstance(w["name"], str, v["name"])
                self.assertIsInstance(w["online"], bool, v["name"])
                if "online" not in row:
                    silent_rows += 1
                    self.assertIs(w["online"], False,
                                  f"{v['name']}: a silent row is never a presence claim")
        self.assertGreaterEqual(silent_rows, 4)


if __name__ == "__main__":
    unittest.main()
