#!/usr/bin/env python3
"""The browser flasher's same-origin release mirror: the writer, the reader, the wiring.

A browser cannot fetch a github.com release asset (the /releases/download/
redirect and the asset host both answer without Access-Control-Allow-Origin),
so the hosted flasher called every product unavailable
(.github/RELEASE_LESSONS.md, 2026-10-27). pages.yml now runs
canary-local/tools/mirror_release.py, which copies the releases the page pins
into the site at firmware/<tag>/<file>, and flash-core.js releaseFetchPlan
reads that path first. What this holds:

  * the writer reads the tag the way flash.json pins it (manifest_url is
    release_download + /manifest-flash.json, an exact fw-v* tag), the Vision
    manifest under the same tag, and the dev channel from flash-core.js's
    DEV_FLASH_MANIFEST_URL;
  * the writer's path for every pin is the URL the page computes for it, and
    the two URL readers agree on a table of edge URLs (through node: the page's
    own module, not a copy of its rules);
  * a channel is mirrored whole or not at all: a missing release, a missing
    asset, a SHA-256 or size that disagrees, an image outside the repo, or two
    channels disagreeing about one path each leave that channel out and the
    deploy green; a transport failure that outlasts the retries exits 1;
  * the REST client retries 5xx and transport errors, answers 404 as "absent",
    and never forwards the token across the asset download's redirect;
  * pages.yml runs it after the site is assembled and before the upload, with
    a token; the three publishing workflows dispatch pages.yml after they
    publish, on the default branch, with `actions: write`, and never fail the
    release over it.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_pages_release_mirror.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests; node is set
      up in that job)
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import urllib.error
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "canary-local" / "tools"))

import mirror_release as mr  # noqa: E402  (definitions only; main() runs under __main__)

CATALOG = json.loads((REPO / "canary-local/devices/flash.json").read_text(encoding="utf-8"))
CORE_SRC = (REPO / "canary-local/assets/flash-core.js").read_text(encoding="utf-8")
WORKFLOWS = REPO / ".github" / "workflows"
REL = "https://github.com/{repo}/releases/download/{tag}/{file}"


def rel(tag: str, file: str, repo: str = "kmay89/securaCV") -> str:
    return REL.format(repo=repo, tag=tag, file=file)


def load_workflow(name: str) -> dict:
    return yaml.safe_load((WORKFLOWS / name).read_text(encoding="utf-8"))


def triggers(wf: dict) -> dict:
    return wf.get("on", wf.get(True, {})) or {}


# ── the pins ─────────────────────────────────────────────────────────────────

class ThePins(unittest.TestCase):
    def test_the_stable_tag_is_the_one_flash_json_pins(self):
        channels = {c.name: c for c in mr.plan_channels(CATALOG, CORE_SRC)}
        self.assertEqual(set(channels), {"release", "vision", "dev"})
        # flash.json pins an exact tag: release_download ends in it, and the
        # manifest sits right under it (gen_flash.py's release_download_base()).
        pinned = CATALOG["release_download"].rsplit("/", 1)[-1]
        self.assertRegex(pinned, r"^fw-v\d+\.\d+\.\d+")
        self.assertEqual(CATALOG["manifest_url"], CATALOG["release_download"] + "/manifest-flash.json")
        self.assertEqual(channels["release"].asset.tag, pinned)
        self.assertEqual(channels["release"].asset.file, "manifest-flash.json")
        self.assertEqual(channels["release"].asset.tag, f"fw-v{CATALOG['fw_train']}")
        self.assertEqual(channels["vision"].asset.tag, pinned)
        self.assertEqual(channels["vision"].asset.file, "manifest-vision-model.json")
        self.assertEqual(channels["dev"].asset.tag, "fw-dev-latest")
        self.assertEqual(channels["dev"].asset.file, "manifest-flash.json")
        for c in channels.values():
            self.assertEqual(c.asset.repo.lower(), CATALOG["repo"].lower())
            self.assertEqual(c.asset.path, f"firmware/{c.asset.tag}/{c.asset.file}")

    def test_an_unmappable_pin_is_a_plan_error_not_a_skip(self):
        # A pin the page could not map either is a code defect: it fails the
        # deploy (exit 1) rather than quietly shipping a flasher with no
        # release. Same for a missing dev constant or repo.
        for mutate in (
            lambda c: c.update(manifest_url="https://github.com/kmay89/securaCV/releases/latest/download/manifest-flash.json"),
            lambda c: c.update(manifest_url="https://example.com/manifest-flash.json"),
            lambda c: c["we2_module"].update(manifest_url=rel("fw-v1", "m.json", repo="someone/fork")),
            lambda c: c.pop("repo"),
        ):
            cat = json.loads(json.dumps(CATALOG))
            mutate(cat)
            with self.assertRaises(mr.PlanError):
                mr.plan_channels(cat, CORE_SRC)
        with self.assertRaises(mr.PlanError):
            mr.plan_channels(CATALOG, CORE_SRC.replace("DEV_FLASH_MANIFEST_URL", "DEV_URL_RENAMED"))

    def test_the_layout_constants_are_the_pages_own(self):
        self.assertIn(f'export const RELEASE_MIRROR_DIR = "{mr.MIRROR_DIR}";', CORE_SRC)
        m = re.search(r"const RELEASE_SEGMENT_RE = /(.+)/;", CORE_SRC)
        self.assertIsNotNone(m, "flash-core.js lost RELEASE_SEGMENT_RE")
        self.assertEqual(m.group(1), mr.SEGMENT_RE.pattern)
        self.assertIn(f'export const MANIFEST_SCHEMA = "{mr.FLASH_SCHEMA}";', CORE_SRC)


# ── the writer's path is the reader's URL (through node) ─────────────────────

NODE_EVAL = r"""
const core = await import(process.argv[1]);
const { page, base, repo, urls } = JSON.parse(process.argv[2]);
const out = {};
for (const u of urls) {
  out[u] = { parts: core.releaseAssetParts(u, repo),
             plan: core.releaseFetchPlan(u, { pageHref: page, mirrorBase: base, repo }) };
}
out.__dev = core.DEV_FLASH_MANIFEST_URL;
process.stdout.write(JSON.stringify(out));
"""

EDGE_URLS = [
    rel("fw-v2.4.17", "canary-2.4.17-factory.bin"),
    rel("fw-v2.5.0-dev.1", "canary-2.5.0-dev.1-factory.bin"),
    rel("fw-dev-latest", "manifest-flash.json"),
    rel("fw-v2.4.17", "person-detection-we2-v2.4.17.tflite"),
    rel("fw-v1", "x.bin", repo="KMAY89/SecuraCV"),
    rel("fw-v1", "x.bin", repo="someone/fork"),
    rel("fw-v1", "x.bin") + "?raw=1",
    rel("fw-v1", "x.bin") + "#f",
    rel("fw-v1", "%2E%2E"),
    rel("fw-v1", "a%2Fb.bin"),
    rel("fw-v1", ".hidden"),
    rel("..", "x.bin"),
    "http://github.com/kmay89/securaCV/releases/download/fw-v1/x.bin",
    "https://github.com:8443/kmay89/securaCV/releases/download/fw-v1/x.bin",
    "https://u:p@github.com/kmay89/securaCV/releases/download/fw-v1/x.bin",
    "https://GitHub.com/kmay89/securaCV/releases/download/fw-v1/x.bin",
    "https://github.com/kmay89/securaCV/releases/latest/download/x.bin",
    "https://evil.example/kmay89/securaCV/releases/download/fw-v1/x.bin",
]


class WriterAndReaderAgree(unittest.TestCase):
    PAGE = "https://kmay89.github.io/securaCV/canary-local/flash.html"
    BASE = "https://kmay89.github.io/securaCV/canary-local/firmware/"

    @classmethod
    def setUpClass(cls):
        node = shutil.which("node")
        if not node:
            raise AssertionError("node is required: this test runs flash-core.js itself (lint.yml sets it up)")
        pins = [CATALOG["manifest_url"], CATALOG["we2_module"]["manifest_url"]]
        payload = json.dumps({"page": cls.PAGE, "base": cls.BASE, "repo": CATALOG["repo"],
                              "urls": pins + EDGE_URLS + [mr.dev_manifest_url(CORE_SRC)]})
        core = (REPO / "canary-local/assets/flash-core.js").as_uri()
        proc = subprocess.run([node, "--input-type=module", "-e", NODE_EVAL, core, payload],
                              capture_output=True, text=True, timeout=60, check=False)
        if proc.returncode:
            raise AssertionError(f"node failed: {proc.stderr}")
        cls.js = json.loads(proc.stdout)

    def test_every_pin_is_written_where_the_hosted_page_reads_it(self):
        self.assertEqual(self.js["__dev"], mr.dev_manifest_url(CORE_SRC))
        for ch in mr.plan_channels(CATALOG, CORE_SRC):
            plan = self.js[ch.url]["plan"]
            # A public host: the mirror, and only the mirror.
            self.assertEqual(plan, [self.BASE.rsplit("firmware/", 1)[0] + ch.asset.path], ch.name)

    def test_the_two_url_readers_agree_on_every_edge(self):
        for url in EDGE_URLS:
            js = self.js[url]["parts"]
            py = mr.asset_parts(url, CATALOG["repo"])
            if py is None:
                self.assertIsNone(js, f"the page maps {url} and the writer does not")
            else:
                self.assertEqual(js, {"repo": py.repo, "tag": py.tag, "file": py.file}, url)


# ── the writer, against a fake release host ──────────────────────────────────

def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


class FakeReleases:
    """{tag: {name: bytes}}; assets() answers names → opaque ids."""

    def __init__(self, releases: dict[str, dict[str, bytes]]):
        self.releases = releases
        self.downloads: list[str] = []

    def assets(self, tag):
        names = self.releases.get(tag)
        return None if names is None else {n: f"{tag}/{n}" for n in names}

    def download(self, ident):
        self.downloads.append(ident)
        tag, name = ident.split("/", 1)
        return self.releases.get(tag, {}).get(name)


REPO_NAME = CATALOG["repo"]
TAG = f"fw-v{CATALOG['fw_train']}"
DEV_TAG = "fw-v9.9.9-dev.1"


def flash_manifest(tag: str, images: dict[str, bytes], **override) -> bytes:
    products = {}
    for i, (name, data) in enumerate(sorted(images.items())):
        products[f"product-{i}"] = {"version": "x", "chipFamily": "ESP32-S3",
                                    "factory": rel(tag, name, REPO_NAME),
                                    "sha256": sha(data), "size": len(data)}
    m = {"schema": "securacv-flash-1", "fw_train": "x", "products": products}
    m.update(override)
    return json.dumps(m).encode()


def vision_manifest(tag: str, model: bytes) -> bytes:
    return json.dumps({"tag": tag, "model": {"name": "m.tflite", "url": rel(tag, "m.tflite", REPO_NAME),
                                             "sha256": sha(model), "size": len(model)}}).encode()


def world() -> dict:
    img_a, img_b, model, dev_img = b"A" * 300, b"B" * 200, b"M" * 100, b"D" * 50
    releases = {
        TAG: {
            "manifest-flash.json": flash_manifest(TAG, {"a-factory.bin": img_a, "b-factory.bin": img_b}),
            "a-factory.bin": img_a, "b-factory.bin": img_b,
            "manifest-vision-model.json": vision_manifest(TAG, model), "m.tflite": model,
        },
        "fw-dev-latest": {"manifest-flash.json": flash_manifest(DEV_TAG, {"d-factory.bin": dev_img})},
        DEV_TAG: {"d-factory.bin": dev_img},
    }
    return releases


class TheWriter(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="mirror-"))
        self.channels = mr.plan_channels(CATALOG, CORE_SRC)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_mirror(self, releases):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            lines = mr.mirror(self.channels, FakeReleases(releases), self.tmp, REPO_NAME)
        return lines, out.getvalue()

    def files(self):
        return sorted(str(p.relative_to(self.tmp)) for p in self.tmp.rglob("*") if p.is_file())

    def test_every_channel_lands_whole_and_byte_for_byte(self):
        releases = world()
        lines, log = self.run_mirror(releases)
        self.assertNotIn("::warning::", log)
        self.assertEqual(self.files(), sorted([
            f"firmware/{TAG}/manifest-flash.json", f"firmware/{TAG}/a-factory.bin",
            f"firmware/{TAG}/b-factory.bin", f"firmware/{TAG}/manifest-vision-model.json",
            f"firmware/{TAG}/m.tflite", "firmware/fw-dev-latest/manifest-flash.json",
            # the dev manifest names its image under the real dev tag
            f"firmware/{DEV_TAG}/d-factory.bin",
        ]))
        for tag, names in releases.items():
            for name, data in names.items():
                self.assertEqual((self.tmp / "firmware" / tag / name).read_bytes(), data, f"{tag}/{name}")
        self.assertEqual(sum("✅" in line for line in lines), 3)

    def test_a_release_not_cut_yet_is_skipped_and_the_rest_still_lands(self):
        # The push deploy that merges a version bump runs before the tag exists.
        releases = world()
        del releases[TAG]
        lines, log = self.run_mirror(releases)
        self.assertEqual(log.count("::warning::"), 2)  # stable + vision
        self.assertIn(f"no release for tag {TAG}", log)
        self.assertEqual(self.files(), ["firmware/fw-dev-latest/manifest-flash.json",
                                        f"firmware/{DEV_TAG}/d-factory.bin"])
        self.assertEqual(sum("⚠️" in line for line in lines), 2)

    def test_a_channel_is_published_whole_or_not_at_all(self):
        cases = {
            "sha256 disagrees": lambda r: r[TAG].update({"b-factory.bin": b"B" * 199 + b"X"}),
            "size disagrees": lambda r: r[TAG].update({"manifest-flash.json": json.dumps(
                {**json.loads(r[TAG]["manifest-flash.json"]),
                 "products": {k: {**v, "size": v["size"] + 1}
                              for k, v in json.loads(r[TAG]["manifest-flash.json"])["products"].items()}}
            ).encode()}),
            "asset missing": lambda r: r[TAG].pop("b-factory.bin"),
            "not JSON": lambda r: r[TAG].update({"manifest-flash.json": b"<html>"}),
            "wrong schema": lambda r: r[TAG].update({"manifest-flash.json": flash_manifest(
                TAG, {"a-factory.bin": r[TAG]["a-factory.bin"]}, schema="other")}),
            "image outside the repo": lambda r: r[TAG].update({"manifest-flash.json": json.dumps(
                {"schema": "securacv-flash-1", "products": {"p": {
                    "factory": rel(TAG, "a-factory.bin", "someone/fork"),
                    "sha256": sha(r[TAG]["a-factory.bin"]), "size": 300}}}).encode()}),
            "bad sha field": lambda r: r[TAG].update({"manifest-flash.json": json.dumps(
                {"schema": "securacv-flash-1", "products": {"p": {
                    "factory": rel(TAG, "a-factory.bin", REPO_NAME), "sha256": "nope", "size": 300}}}).encode()}),
        }
        for why, mutate in cases.items():
            with self.subTest(why):
                shutil.rmtree(self.tmp)
                self.tmp.mkdir()
                releases = world()
                mutate(releases)
                _, log = self.run_mirror(releases)
                self.assertIn("release channel", log)
                self.assertEqual(log.count("::warning::"), 1, log)
                written = self.files()
                # nothing of the stable channel's manifest-flash.json or images
                self.assertNotIn(f"firmware/{TAG}/manifest-flash.json", written)
                self.assertNotIn(f"firmware/{TAG}/a-factory.bin", written)
                self.assertNotIn(f"firmware/{TAG}/b-factory.bin", written)
                # the other two channels are untouched by it
                self.assertIn(f"firmware/{TAG}/manifest-vision-model.json", written)
                self.assertIn("firmware/fw-dev-latest/manifest-flash.json", written)

    def test_two_channels_disagreeing_about_one_path_keep_the_first(self):
        releases = world()
        # The dev manifest points at an image under the STABLE tag whose
        # bytes differ from what the stable channel already wrote there.
        other = b"Z" * 300
        releases["fw-dev-latest"]["manifest-flash.json"] = flash_manifest(TAG, {"a-factory.bin": other})
        src = FakeReleases(releases)
        real_download = src.download

        def download(ident):
            # the dev channel reads a different a-factory.bin for the same path
            if ident == f"{TAG}/a-factory.bin" and src.downloads.count(ident) >= 1:
                src.downloads.append(ident)
                return other
            return real_download(ident)

        src.download = download
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            mr.mirror(self.channels, src, self.tmp, REPO_NAME)
        log = out.getvalue()
        self.assertIn("dev channel", log)
        self.assertIn("already written by another channel", log)
        self.assertEqual((self.tmp / "firmware" / TAG / "a-factory.bin").read_bytes(), releases[TAG]["a-factory.bin"])
        self.assertFalse((self.tmp / "firmware" / "fw-dev-latest").exists())

    def test_a_transport_failure_fails_the_run(self):
        class Down:
            def assets(self, tag):
                raise mr.TransportError("api.github.com: HTTP 502 after 4 attempts")

        class FakeCls:
            def __init__(self, *a, **k):
                pass

            assets = Down.assets

        real = mr.GitHubReleases
        mr.GitHubReleases = FakeCls
        try:
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = mr.main(["--site", str(self.tmp)])
        finally:
            mr.GitHubReleases = real
        self.assertEqual(code, 1)
        self.assertIn("::error::", out.getvalue())
        self.assertEqual(self.files(), [])

    def test_plan_mode_reads_the_pins_without_the_network(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(mr.main(["--plan"]), 0)
        self.assertIn(f"{TAG}/manifest-flash.json → firmware/{TAG}/manifest-flash.json", out.getvalue())
        self.assertIn("fw-dev-latest/manifest-flash.json", out.getvalue())


# ── the REST client ──────────────────────────────────────────────────────────

class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class FakeOpener:
    def __init__(self, answers):
        self.answers = list(answers)
        self.requests = []

    def open(self, req, timeout=None):
        self.requests.append(req)
        a = self.answers.pop(0)
        if isinstance(a, Exception):
            raise a
        return FakeResponse(a)


def http_error(code):
    return urllib.error.HTTPError("https://api.github.com/x", code, "err", {}, None)


class TheRestClient(unittest.TestCase):
    def client(self, answers, token="t0ken"):
        opener = FakeOpener(answers)
        return mr.GitHubReleases("kmay89/securaCV", token, sleep=lambda s: None, opener=opener), opener

    def test_retries_5xx_and_transport_errors_then_answers(self):
        c, opener = self.client([http_error(502), urllib.error.URLError("reset"), http_error(429), b"bytes"])
        self.assertEqual(c.download("https://api.github.com/repos/kmay89/securaCV/releases/assets/1"), b"bytes")
        self.assertEqual(len(opener.requests), 4)

    def test_gives_up_after_the_attempts(self):
        c, _ = self.client([http_error(503)] * mr.ATTEMPTS)
        with self.assertRaises(mr.TransportError):
            c.download("https://api.github.com/x")

    def test_404_is_absent_and_other_4xx_is_a_failure(self):
        c, _ = self.client([http_error(404)])
        self.assertIsNone(c.assets("fw-v0.0.0"))
        c, _ = self.client([http_error(401)])
        with self.assertRaises(mr.TransportError):
            c.assets("fw-v1")

    def test_assets_page_through_every_listing(self):
        page1 = json.dumps([{"name": f"f{i}", "url": f"u{i}"} for i in range(100)]).encode()
        page2 = json.dumps([{"name": "last", "url": "u-last"}]).encode()
        c, opener = self.client([json.dumps({"id": 42}).encode(), page1, page2])
        names = c.assets("fw-dev-latest")
        self.assertEqual(len(names), 101)
        self.assertEqual(names["last"], "u-last")
        self.assertTrue(opener.requests[0].full_url.endswith("/repos/kmay89/securaCV/releases/tags/fw-dev-latest"))
        self.assertIn("/releases/42/assets?per_page=100&page=2", opener.requests[2].full_url)
        # cached: a second channel on the same tag asks nothing more
        self.assertIs(c.assets("fw-dev-latest"), names)
        self.assertEqual(len(opener.requests), 3)

    def test_the_token_never_follows_the_asset_redirect(self):
        c, opener = self.client([b"x"])
        c.download("https://api.github.com/repos/kmay89/securaCV/releases/assets/1")
        req = opener.requests[0]
        self.assertEqual(req.get_header("Accept"), "application/octet-stream")
        # unredirected: urllib does not copy it onto the 302's signed asset URL
        self.assertNotIn("Authorization", req.headers)
        self.assertEqual(req.unredirected_hdrs.get("Authorization"), "Bearer t0ken")
        c, opener = self.client([b"x"], token=None)
        c.download("https://api.github.com/x")
        self.assertIsNone(opener.requests[0].get_header("Authorization"))


# ── the wiring ───────────────────────────────────────────────────────────────

MIRROR_CMD = "python3 canary-local/tools/mirror_release.py --site _site/canary-local"


class TheWiring(unittest.TestCase):
    def test_pages_mirrors_after_assembling_and_before_uploading(self):
        wf = load_workflow("pages.yml")
        self.assertIn("workflow_dispatch", triggers(wf), "the release workflows dispatch pages.yml")
        self.assertEqual(wf["permissions"].get("contents"), "read")
        steps = wf["jobs"]["build"]["steps"]
        names = [s.get("name", s.get("uses", "")) for s in steps]
        assemble = names.index("Assemble site")
        mirror = next(i for i, s in enumerate(steps) if MIRROR_CMD in str(s.get("run", "")))
        upload = next(i for i, s in enumerate(steps) if str(s.get("uses", "")).startswith("actions/upload-pages-artifact@"))
        self.assertLess(assemble, mirror)
        self.assertLess(mirror, upload)
        step = steps[mirror]
        self.assertEqual(step.get("env", {}).get("GH_TOKEN"), "${{ github.token }}")
        self.assertNotIn("continue-on-error", step, "a transport failure must fail the deploy (the last good site stays)")
        self.assertNotIn("if", step)
        # canary-local/** is already in the push filter, so editing the script redeploys
        self.assertIn("canary-local/**", triggers(wf)["push"]["paths"])

    def test_the_site_never_publishes_the_script(self):
        # Assemble site deletes every .py under the site copy; the step runs
        # the checkout's copy.
        run = next(s["run"] for s in load_workflow("pages.yml")["jobs"]["build"]["steps"]
                   if s.get("name") == "Assemble site")
        self.assertRegex(run, r"find _site/canary-local -type f .*-name '\*\.py'.* -delete")

    def assert_dispatches_pages(self, name, publish_ids):
        wf = load_workflow(name)
        self.assertEqual(wf["permissions"].get("actions"), "write", f"{name}: dispatching needs actions: write")
        jobs = [j for j in wf["jobs"].values()
                if any("gh workflow run pages.yml" in str(s.get("run", "")) for s in (j.get("steps") or []))]
        self.assertEqual(len(jobs), 1, f"{name}: one job dispatches pages.yml")
        steps = jobs[0]["steps"]
        idx = [i for i, s in enumerate(steps) if "gh workflow run pages.yml" in str(s.get("run", ""))]
        self.assertEqual(len(idx), 1, f"{name}: exactly one step dispatches pages.yml")
        # Last: after every publish, and (firmware-release.yml) after the dev
        # channel's rolling manifest is in place too.
        self.assertEqual(idx[0], len(steps) - 1, f"{name}: the dispatch is its job's last step")
        step = steps[idx[0]]
        ids = [i for i, s in enumerate(steps) if s.get("id") in publish_ids]
        self.assertEqual(len(ids), len(publish_ids), f"{name}: publish step ids {publish_ids}")
        self.assertGreater(idx[0], max(ids), f"{name}: the dispatch runs after publishing")
        cond = str(step.get("if", ""))
        # success() || failure(): every outcome but a canceled run, so it runs
        # even after a later step (the Vision chain, say) went red.
        self.assertIn("(success() || failure())", cond, f"{name}: runs even after a later step went red")
        for pid in publish_ids:
            self.assertIn(f"steps.{pid}.outcome == 'success'", cond)
        self.assertEqual(step["env"]["REF"], "${{ github.event.repository.default_branch || 'main' }}",
                         f"{name}: the github-pages environment deploys only the default branch")
        run = step["run"]
        self.assertIn('--ref "$REF"', run)
        self.assertIn("::warning::", run)
        self.assertIn("GITHUB_STEP_SUMMARY", run)
        self.assertNotIn("exit 1", run, f"{name}: a failed dispatch must not fail the release")
        self.assertRegex(run, r"if gh workflow run pages\.yml .*; then")
        self.assertNotIn("continue-on-error", step)

    def test_firmware_release_redeploys_the_site_last(self):
        self.assert_dispatches_pages("firmware-release.yml", ["publish"])

    def test_flasher_release_redeploys_after_either_channel(self):
        self.assert_dispatches_pages("flasher-release.yml", ["attach", "publish_dev"])

    def test_vision_model_release_redeploys_after_attaching(self):
        self.assert_dispatches_pages("vision-model-release.yml", ["attach"])


if __name__ == "__main__":
    unittest.main()
