"""dev_channel.py brings fw-dev-latest up to stable — held to that.

The `gh` it drives is a stand-in that answers the same argv a runner would
send, over in-memory releases shaped like the real ones (fw-v2.4.17 and the
fw-dev-latest it found on 2026-10-27: a 2.4.15 flash catalog, factory images,
no per-product manifest). The version order is held to the device's own host
cases in firmware/common/ota/test_ota_logic.cpp, which workflows-lint.yml
lists in its path filters for that reason.
"""
from __future__ import annotations

import io
import json
import re
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dev_channel as dc  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SLUG = "kmay89/securaCV"
DL = f"https://github.com/{SLUG}/releases/download"


def ota_manifest(product: str, tag: str, version: str, binary: str) -> bytes:
    return json.dumps({"manifest_version": 1, "product": product, "version": version,
                       "url": f"{DL}/{tag}/{binary}", "sha256": "ab" * 32, "size": 1,
                       "signature": "00" * 64, "manifest_signature": "00" * 64,
                       "release_url": f"https://github.com/{SLUG}/releases/tag/{tag}"}).encode()


def firmware_release(version: str) -> dict:
    tag = f"fw-v{version}"
    products = {"securacv-canary": ("manifest-canary.json", f"canary-{version}.bin", version),
                "securacv-canary-wap": ("manifest-canary-wap.json", f"canary-wap-{version}.bin",
                                        version.replace("-", "-wap.", 1) if "-" in version else f"{version}-wap"),
                "securacv-canary-display-dash": ("manifest-canary-display-dash.json",
                                                 f"canary-display-dash-{version}.bin", version)}
    assets = {}
    for pid, (name, binary, ver) in products.items():
        assets[name] = ota_manifest(pid, tag, ver, binary)
        assets[binary] = b"bin"
        assets[binary.replace(".bin", "-factory.bin")] = b"factory"
    assets["manifest-index.json"] = json.dumps(
        {"manifest_version": 1, "products": {pid: f"{DL}/{tag}/{n}" for pid, (n, _, _) in products.items()}}).encode()
    assets["manifest-flash.json"] = json.dumps(
        {"schema": "securacv-flash-1", "fw_train": version, "repo": SLUG,
         "products": {pid: {"version": ver, "factory": f"{DL}/{tag}/{b.replace('.bin', '-factory.bin')}"}
                      for pid, (_, b, ver) in products.items()}}).encode()
    assets["sha256sums.txt"] = b"ab  ./canary.bin\n"
    # On the release, never part of the dev channel's copy.
    assets["manifest-vision-model.json"] = b"{}"
    assets[f"provenance-firmware-{version}.sigstore.jsonl"] = b"{}"
    return {"prerelease": "-" in version, "assets": assets, "body": f"notes for {version}"}


def dev_today() -> dict:
    """fw-dev-latest as found on 2026-10-27."""
    return {"prerelease": True, "body": "Rolling dev channel build ... 2.4.15", "assets": {
        "manifest-flash.json": json.dumps({"schema": "securacv-flash-1", "fw_train": "2.4.15",
                                           "products": {}}).encode(),
        "flasher-sha256sums.txt": b"..",
        "canary-2.4.15-factory.bin": b"old factory",
        "canary-2.3.0-factory.bin": b"older factory",
    }}


class FakeGh:
    """Answers dev_channel.GitHub's argv over in-memory releases."""

    def __init__(self, releases: dict, latest: str):
        self.releases = releases
        self.latest = latest
        self.calls: list[list[str]] = []
        self.on_upload = None  # hook(argv) run before an upload lands
        self.on_download = None  # hook(argv) run before a download
        self.on_edit = None  # hook(argv) run before a release edit lands

    @staticmethod
    def opt(argv, flag):
        return [argv[i + 1] for i, a in enumerate(argv) if a == flag]

    def __call__(self, argv):
        self.calls.append(list(argv))
        if argv[0] == "api":
            path = argv[1]
            if path == f"repos/{SLUG}/releases/latest":
                return self.latest + "\n"
            m = re.fullmatch(rf"repos/{SLUG}/releases/tags/(.+)", path)
            if m:
                rel = self.releases.get(m[1])
                if rel is None:
                    raise dc.GhError("gh: Not Found (HTTP 404)")
                return json.dumps({"tag_name": m[1], "prerelease": rel["prerelease"],
                                   "draft": rel.get("draft", False),
                                   "assets": [{"name": n} for n in rel["assets"]]})
            if re.fullmatch(rf"repos/{SLUG}/commits/.+", path):
                return "c0ffee\n"
        verb = argv[:2]
        if verb == ["release", "download"]:
            if self.on_download:
                self.on_download(argv)
            rel = self.releases[argv[2]]
            dest = Path(self.opt(argv, "--dir")[0])
            dest.mkdir(parents=True, exist_ok=True)
            for name in self.opt(argv, "--pattern"):
                if name not in rel["assets"]:
                    raise dc.GhError(f"no assets match the file pattern {name}")
                (dest / name).write_bytes(rel["assets"][name])
            return ""
        if verb == ["release", "upload"]:
            if self.on_upload:
                self.on_upload(argv)
            rel = self.releases[argv[2]]
            for p in argv[3:argv.index("--clobber")]:
                rel["assets"][Path(p).name] = Path(p).read_bytes()
            return ""
        if verb == ["release", "create"]:
            self.releases[argv[2]] = {"prerelease": "--prerelease" in argv, "assets": {},
                                      "body": Path(self.opt(argv, "--notes-file")[0]).read_text()}
            return ""
        if verb == ["release", "edit"]:
            if self.on_edit:
                self.on_edit(argv)
            rel = self.releases[argv[2]]
            rel["body"] = Path(self.opt(argv, "--notes-file")[0]).read_text()
            rel["prerelease"] = rel["prerelease"] or "--prerelease" in argv
            return ""
        raise AssertionError(f"dev_channel.py called gh {argv}, which it has no business calling")

    def writes(self):
        return [c for c in self.calls if c[0] == "release" and c[1] != "download"]


def world(dev: dict | None = None, latest: str = "fw-v2.4.17", extra: dict | None = None) -> FakeGh:
    releases = {"fw-v2.4.17": firmware_release("2.4.17"), **(extra or {})}
    if dev is not None:
        releases[dc.DEV_TAG] = dev
    return FakeGh(releases, latest)


def run(fake: FakeGh, source: str | None = None, dry_run: bool = False) -> bool:
    return dc.sync(dc.GitHub(SLUG, fake), source, dry_run, log=lambda _line: None)


class VersionOrder(unittest.TestCase):
    def test_prerelease_precedence(self):
        order = ["2.4.17-dev.3", "2.4.17-rc.1", "2.4.17", "2.4.18-dev.1", "2.4.18-dev.10", "2.4.18-rc.1", "2.10.0"]
        for lo, hi in zip(order, order[1:]):
            with self.subTest(lo=lo, hi=hi):
                self.assertEqual(dc.compare(hi, lo), 1)
                self.assertEqual(dc.compare(lo, hi), -1)
        self.assertEqual(dc.compare("2.4.17", "2.4.17"), 0)

    def test_variant_suffix_is_not_a_prerelease(self):
        self.assertEqual(dc.compare("2.4.17-wap", "2.4.17"), 0)
        self.assertEqual(dc.compare("2.4.17-wap", "2.4.17-wap.dev.2"), 1)

    def test_no_triple_has_no_order(self):
        for bad in ("2.4", "garbage", "", None, "v2.4.17"):
            with self.subTest(bad=bad):
                self.assertIsNone(dc.version_key(bad))
        with self.assertRaises(ValueError):
            dc.compare("2.4", "2.4.17")

    def test_same_order_as_the_device(self):
        # securacv_version_compare decides UPDATE vs ROLLBACK on every Canary;
        # a train this script calls newer must be newer there too.
        src = (REPO / "firmware/common/ota/test_ota_logic.cpp").read_text(encoding="utf-8")
        cases = re.findall(r'CHECK\(securacv_version_compare\("([^"]*)",\s*"([^"]*)"\)\s*==\s*(-?\d+)\);', src)
        held = [(a, b, int(n)) for a, b, n in cases if dc.version_key(a) and dc.version_key(b)]
        self.assertGreaterEqual(len(held), 12, "the device's version cases moved; re-point this test")
        for a, b, n in held:
            with self.subTest(a=a, b=b):
                self.assertEqual(dc.compare(a, b), n)


class Decide(unittest.TestCase):
    def test_a_missing_group_is_copied(self):
        d = dc.decide("2.4.17", {"ota": None, "flash": "2.4.15"})
        self.assertEqual({k: v[0] for k, v in d.items()}, {"ota": True, "flash": True})
        self.assertIn("no manifest-canary.json", d["ota"][1])

    def test_an_equal_train_in_the_same_bytes_is_left(self):
        d = dc.decide("2.4.17", {"ota": "2.4.17", "flash": "2.4.17"})
        self.assertFalse(any(v[0] for v in d.values()))

    def test_an_equal_train_in_other_bytes_is_copied(self):
        # A bring-up catalog built at the release's version before its tag, or
        # this release's catalog from before a Flasher Factory Images rebuild:
        # the tagged release's bytes win.
        d = dc.decide("2.4.17", {"ota": "2.4.17", "flash": "2.4.17"}, differs={"flash"})
        self.assertEqual({k: v[0] for k, v in d.items()}, {"ota": False, "flash": True})

    def test_a_newer_dev_build_is_left_alone(self):
        d = dc.decide("2.4.17", {"ota": "2.4.18-dev.1", "flash": "2.4.17-rc.1"}, differs={"ota", "flash"})
        self.assertEqual({k: v[0] for k, v in d.items()}, {"ota": False, "flash": True})
        self.assertIn("left alone", d["ota"][1])

    def test_an_unreadable_train_is_refused(self):
        with self.assertRaises(dc.Refusal):
            dc.decide("2.4.17", {"ota": "None", "flash": None})

    def test_which_files(self):
        names = set(firmware_release("2.4.17")["assets"])
        ota, flash = dc.GROUPS
        self.assertEqual(dc.channel_files(ota, names),
                         ["manifest-canary-display-dash.json", "manifest-canary-wap.json",
                          "manifest-index.json", "sha256sums.txt", "manifest-canary.json"])
        self.assertEqual(dc.channel_files(flash, names), ["manifest-flash.json"])


class Sync(unittest.TestCase):
    def assert_never_latest(self, fake):
        for argv in fake.writes():
            if argv[1] in ("create", "edit"):
                self.assertIn("--prerelease", argv)
                self.assertIn("--latest=false", argv)
            self.assertFalse([a for a in argv if a.startswith("--latest") and a != "--latest=false"], argv)
            self.assertNotIn("delete", argv[1])
            self.assertNotIn("delete-asset", argv)

    def test_todays_dev_channel_catches_up_to_stable(self):
        fake = world(dev_today())
        before = set(fake.releases[dc.DEV_TAG]["assets"])
        self.assertTrue(run(fake))
        dev = fake.releases[dc.DEV_TAG]
        stable = fake.releases["fw-v2.4.17"]["assets"]
        copied = {"manifest-canary.json", "manifest-canary-wap.json", "manifest-canary-display-dash.json",
                  "manifest-index.json", "sha256sums.txt", "manifest-flash.json"}
        for name in copied:
            self.assertEqual(dev["assets"][name], stable[name], name)  # byte for byte
        self.assertEqual(set(dev["assets"]), before | copied, "copied more, or lost an asset")
        self.assertNotIn("manifest-vision-model.json", dev["assets"])
        self.assertTrue(dev["prerelease"])
        self.assertEqual(fake.latest, "fw-v2.4.17")
        self.assert_never_latest(fake)
        self.assertIn("Currently carrying firmware **2.4.17**, the stable release", dev["body"])
        self.assertIn("had no newer build", dev["body"])
        self.assertIn("`flasher-sha256sums.txt`, when present, covers only", dev["body"])

    def test_each_marker_goes_up_last_in_its_own_call(self):
        fake = world(dev_today())
        run(fake)
        uploads = [[Path(p).name for p in c[3:c.index("--clobber")]] for c in fake.writes() if c[1] == "upload"]
        self.assertEqual(uploads[1], ["manifest-canary.json"])
        self.assertNotIn("manifest-canary.json", uploads[0])
        self.assertEqual(uploads[2], ["manifest-flash.json"])

    def test_a_run_cut_short_is_finished_by_the_next(self):
        fake = world(dev_today())

        def fail_on_marker(argv):
            if any(p.endswith("/manifest-canary.json") for p in argv):
                fake.on_upload = None
                raise dc.GhError("upload interrupted")
        fake.on_upload = fail_on_marker
        with self.assertRaises(dc.GhError):
            run(fake)
        self.assertNotIn("manifest-canary.json", fake.releases[dc.DEV_TAG]["assets"])
        self.assertTrue(run(fake))
        self.assertIn("manifest-canary.json", fake.releases[dc.DEV_TAG]["assets"])

    def test_pressing_twice_writes_nothing_the_second_time(self):
        fake = world(dev_today())
        run(fake)
        fake.calls.clear()
        self.assertFalse(run(fake))
        self.assertEqual(fake.writes(), [])

    def test_a_newer_dev_release_is_left_alone(self):
        dev = firmware_release("2.4.18-dev.1")
        fake = world({"prerelease": True, "assets": {k: v for k, v in dev["assets"].items()
                                                     if k.startswith("manifest-") or k == "sha256sums.txt"}})
        self.assertFalse(run(fake))
        self.assertEqual(fake.writes(), [])

    def test_a_dev_release_moves_the_channel_ahead(self):
        fake = world(dev_today(), extra={"fw-v2.4.18-dev.1": firmware_release("2.4.18-dev.1")})
        self.assertTrue(run(fake, "fw-v2.4.18-dev.1"))
        body = fake.releases[dc.DEV_TAG]["body"]
        self.assertIn("**2.4.18-dev.1**, the dev build", body)
        self.assertIn("ahead of stable 2.4.17", body)
        self.assert_never_latest(fake)

    def test_a_catalog_ahead_stays_and_the_body_says_so(self):
        dev = dev_today()
        dev["assets"]["manifest-flash.json"] = json.dumps({"fw_train": "2.4.18-dev.2"}).encode()
        fake = world(dev)
        self.assertTrue(run(fake))
        assets = fake.releases[dc.DEV_TAG]["assets"]
        self.assertEqual(json.loads(assets["manifest-flash.json"])["fw_train"], "2.4.18-dev.2")
        self.assertIn("manifest-canary.json", assets)
        body = fake.releases[dc.DEV_TAG]["body"]
        self.assertIn("Except the flashers' catalog (`manifest-flash.json`), left on **2.4.18-dev.2**, "
                      "which is newer.", body)
        self.assertNotIn("had no newer build", body, "a newer build is right there")

    def test_no_dev_release_is_created_as_a_prerelease_never_latest(self):
        fake = world(None)
        self.assertTrue(run(fake))
        create = [c for c in fake.writes() if c[1] == "create"]
        self.assertEqual(len(create), 1)
        self.assertEqual(FakeGh.opt(create[0], "--target"), ["c0ffee"])
        self.assertTrue(fake.releases[dc.DEV_TAG]["prerelease"])
        self.assertFalse([c for c in fake.writes() if c[1] == "edit"])
        self.assert_never_latest(fake)

    def test_latest_moving_off_the_firmware_during_the_write_fails_the_run(self):
        fake = world(dev_today())

        def move(_argv):
            fake.latest = "flasher-v9.9.9"
        fake.on_upload = move
        with self.assertRaisesRegex(dc.Refusal, "releases/latest is now flasher-v9.9.9, not a firmware release"):
            run(fake)

    def test_a_newer_release_during_the_write_says_run_it_again(self):
        fake = world(dev_today())

        def move(_argv):
            fake.latest = "fw-v2.4.18"
        fake.on_upload = move
        with self.assertRaisesRegex(dc.Refusal, "moved from fw-v2.4.17 to fw-v2.4.18 .* Refresh the dev channel again"):
            run(fake)

    def test_latest_is_checked_even_when_a_write_fails(self):
        # The check after the writes sits in `finally`: a run cut short by a
        # failed upload still says releases/latest moved, not just the upload.
        fake = world(dev_today())

        def move_and_fail(_argv):
            fake.latest = "app-v9.9.9"
            raise dc.GhError("upload interrupted")
        fake.on_upload = move_and_fail
        with self.assertRaisesRegex(dc.Refusal, "releases/latest is now app-v9.9.9"):
            run(fake)

    def test_latest_moving_before_the_write_writes_nothing(self):
        fake = world(dev_today())

        def move(argv):
            if argv[2] == "fw-v2.4.17" and "manifest-canary-wap.json" in argv:
                fake.latest = "fw-v2.4.18"
        fake.on_download = move
        with self.assertRaisesRegex(dc.Refusal, "moved from fw-v2.4.17 to fw-v2.4.18 while this run was deciding"):
            run(fake)
        self.assertEqual(fake.writes(), [])

    def test_another_writer_between_the_decision_and_the_write_stops_it(self):
        # A Flasher Factory Images dev run (or a release) lands a catalog after
        # this run read the old one: its decision is stale, so it writes nothing.
        fake = world(dev_today())

        def bring_up(argv):
            if argv[2] == "fw-v2.4.17" and "manifest-canary-wap.json" in argv:
                fake.releases[dc.DEV_TAG]["assets"]["manifest-flash.json"] = json.dumps(
                    {"fw_train": "2.4.18"}).encode()
        fake.on_download = bring_up
        with self.assertRaisesRegex(dc.Refusal, "changed while this run was deciding"):
            run(fake)
        self.assertEqual(fake.writes(), [])

    def test_a_dev_release_refused_by_a_race_is_finished_by_the_button_with_its_tag(self):
        # The release's own step cannot be re-run (its version step refuses
        # the tag it just created), and the button's default source,
        # releases/latest, is never the prerelease. So the refusal names the
        # button with this run's source, and a run with that source finishes it.
        fake = world(dev_today(), extra={"fw-v2.4.18-dev.1": firmware_release("2.4.18-dev.1")})
        landed = []

        def bring_up_once(argv):
            if not landed and argv[2] == "fw-v2.4.18-dev.1" and "manifest-canary-wap.json" in argv:
                landed.append(True)
                fake.releases[dc.DEV_TAG]["assets"]["manifest-flash.json"] = json.dumps(
                    {"fw_train": "2.4.17"}).encode()
        fake.on_download = bring_up_once
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(dc.main(["--repo", SLUG, "--source", "fw-v2.4.18-dev.1"], fake), 1)
        self.assertIn("Run it again: Actions → Refresh the dev channel, with source fw-v2.4.18-dev.1",
                      out.getvalue())
        self.assertIn("tag-exists check", out.getvalue())
        self.assertEqual(fake.writes(), [])
        with redirect_stdout(io.StringIO()):
            self.assertEqual(dc.main(["--repo", SLUG, "--source", "fw-v2.4.18-dev.1"], fake), 0)
        dev = fake.releases[dc.DEV_TAG]["assets"]
        self.assertEqual(json.loads(dev["manifest-canary.json"])["version"], "2.4.18-dev.1")
        self.assertEqual(json.loads(dev["manifest-flash.json"])["fw_train"], "2.4.18-dev.1")
        self.assertEqual(fake.latest, "fw-v2.4.17", "releases/latest never moves")

    def test_latest_off_the_firmware_refuses_before_anything(self):
        fake = world(dev_today(), latest="flasher-v0.12.2")
        with self.assertRaisesRegex(dc.Refusal, "not a firmware release"):
            run(fake)
        self.assertEqual(fake.writes(), [])

    def test_a_relative_url_is_refused_before_anything_is_written(self):
        fake = world(dev_today())
        assets = fake.releases["fw-v2.4.17"]["assets"]
        doc = json.loads(assets["manifest-canary-wap.json"])
        doc["url"] = "canary-wap-2.4.17.bin"
        assets["manifest-canary-wap.json"] = json.dumps(doc).encode()
        with self.assertRaisesRegex(dc.Refusal, "not an asset of fw-v2.4.17 by absolute URL"):
            run(fake)
        self.assertEqual(fake.writes(), [])

    def test_an_index_entry_off_the_tag_is_refused(self):
        for url in ("manifest-canary-wap.json", f"{DL}/fw-v2.4.16/manifest-canary-wap.json",
                    f"{DL}/fw-v2.4.17/manifest-canary-gone.json"):
            with self.subTest(url=url):
                fake = world(dev_today())
                assets = fake.releases["fw-v2.4.17"]["assets"]
                doc = json.loads(assets["manifest-index.json"])
                doc["products"]["securacv-canary-wap"] = url
                assets["manifest-index.json"] = json.dumps(doc).encode()
                with self.assertRaisesRegex(dc.Refusal, "manifest-index.json entry securacv-canary-wap"):
                    run(fake)
                self.assertEqual(fake.writes(), [])

    def test_a_flagship_that_disagrees_with_its_tag_is_refused(self):
        fake = world(dev_today())
        assets = fake.releases["fw-v2.4.17"]["assets"]
        doc = json.loads(assets["manifest-canary.json"])
        doc["version"] = "2.4.16"
        assets["manifest-canary.json"] = json.dumps(doc).encode()
        with self.assertRaisesRegex(dc.Refusal, "manifest-canary.json says version '2.4.16', not 2.4.17"):
            run(fake)
        self.assertEqual(fake.writes(), [])

    def test_a_draft_or_incomplete_or_non_firmware_source_is_refused(self):
        fake = world(dev_today())
        fake.releases["fw-v2.4.17"]["draft"] = True
        with self.assertRaisesRegex(dc.Refusal, "fw-v2.4.17 is not a published release"):
            run(fake)
        fake.releases["fw-v2.4.17"]["draft"] = False
        del fake.releases["fw-v2.4.17"]["assets"]["manifest-index.json"]
        with self.assertRaisesRegex(dc.Refusal, "carries no manifest-index.json"):
            run(fake)
        # "hub-2.4.18" parses as a version past its first four characters.
        for tag in ("flasher-v1.0.0", "fw-dev-latest", "fw-v2.4", "hub-2.4.18"):
            with self.subTest(tag=tag), self.assertRaisesRegex(dc.Refusal, "not a fw-v<version> firmware tag"):
                run(fake, tag)
        self.assertEqual(fake.writes(), [])

    def test_a_source_below_stable_writes_nothing_and_is_not_even_read(self):
        # A Flasher Factory Images rebuild of an old tag runs this too; that
        # tag may predate manifest-index.json, and that is no reason to go red.
        old = firmware_release("2.4.16")
        del old["assets"]["manifest-index.json"]
        fake = world(dev_today(), extra={"fw-v2.4.16": old, "fw-v2.4.17-rc.2": firmware_release("2.4.17-rc.2")})
        for tag in ("fw-v2.4.16", "fw-v2.4.17-rc.2"):
            with self.subTest(tag=tag):
                self.assertFalse(run(fake, tag))
        self.assertEqual([c for c in fake.calls if c[0] == "release"], [])

    def test_the_same_version_in_other_bytes_is_replaced(self):
        # A bring-up catalog cut at 2.4.17 (or fw-v2.4.17's own, before a
        # rebuild re-signed its images): the tagged release's bytes win, once.
        fake = world(dev_today())
        run(fake)
        dev = fake.releases[dc.DEV_TAG]["assets"]
        dev["manifest-flash.json"] = json.dumps({"fw_train": "2.4.17", "products": {}}).encode()
        fake.calls.clear()
        self.assertTrue(run(fake))
        uploaded = [Path(p).name for c in fake.writes() if c[1] == "upload" for p in c[3:c.index("--clobber")]]
        self.assertEqual(uploaded, ["manifest-flash.json"])
        self.assertEqual(dev["manifest-flash.json"], fake.releases["fw-v2.4.17"]["assets"]["manifest-flash.json"])
        fake.calls.clear()
        self.assertFalse(run(fake))
        self.assertEqual(fake.writes(), [])

    def test_the_body_goes_up_before_the_markers_and_a_failed_edit_is_redone(self):
        fake = world(dev_today())
        writes = []
        fake.on_upload = lambda argv: writes.append("upload")

        def fail_once(_argv):
            fake.on_edit = lambda _argv: writes.append("edit")
            raise dc.GhError("HTTP 502")
        fake.on_edit = fail_once
        with self.assertRaises(dc.GhError):
            run(fake)
        self.assertEqual(writes, [], "nothing uploaded after a failed body edit")
        self.assertTrue(run(fake))
        self.assertEqual(writes[0], "edit")
        self.assertIn("Currently carrying firmware **2.4.17**", fake.releases[dc.DEV_TAG]["body"])

    def test_a_url_to_a_missing_asset_is_refused(self):
        fake = world(dev_today())
        del fake.releases["fw-v2.4.17"]["assets"]["canary-display-dash-2.4.17-factory.bin"]
        with self.assertRaisesRegex(dc.Refusal, "manifest-flash.json entry securacv-canary-display-dash"):
            run(fake)
        self.assertEqual(fake.writes(), [])

    def test_a_catalog_that_disagrees_with_its_tag_is_refused(self):
        fake = world(dev_today())
        assets = fake.releases["fw-v2.4.17"]["assets"]
        doc = json.loads(assets["manifest-flash.json"])
        doc["fw_train"] = "2.4.16"
        assets["manifest-flash.json"] = json.dumps(doc).encode()
        with self.assertRaisesRegex(dc.Refusal, "fw_train '2.4.16', not 2.4.17"):
            run(fake)

    def test_a_dev_release_that_is_not_a_prerelease_is_refused(self):
        dev = dev_today()
        dev["prerelease"] = False
        fake = world(dev)
        with self.assertRaisesRegex(dc.Refusal, "not a prerelease"):
            run(fake)
        self.assertEqual(fake.writes(), [])

    def test_dry_run_writes_nothing(self):
        fake = world(dev_today())
        self.assertFalse(run(fake, dry_run=True))
        self.assertEqual(fake.writes(), [])

    def test_check_train_refuses_a_bring_up_below_stable(self):
        fake = world(dev_today())
        gh = dc.GitHub(SLUG, fake)
        for ok in ("2.4.17", "2.4.18", "2.4.18-dev.1"):
            with self.subTest(ok=ok):
                dc.check_train(gh, ok, log=lambda _line: None)
        for bad, why in (("2.4.16", "older than stable 2.4.17"), ("2.4.17-rc.3", "older than stable 2.4.17"),
                         ("2.4", "not a firmware version")):
            with self.subTest(bad=bad), self.assertRaisesRegex(dc.Refusal, why):
                dc.check_train(gh, bad, log=lambda _line: None)
        fake.latest = "app-v0.3.2"
        with self.assertRaisesRegex(dc.Refusal, "not a firmware release"):
            dc.check_train(gh, "2.4.18", log=lambda _line: None)
        self.assertEqual([c for c in fake.calls if c[0] == "release"], [], "a check writes nothing")

    def test_main_check_train_exits_by_the_verdict(self):
        fake = world(dev_today())
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(dc.main(["--repo", SLUG, "--check-train", "2.4.17"], fake), 0)
            self.assertEqual(dc.main(["--repo", SLUG, "--check-train", "2.4.16"], fake), 1)
        self.assertIn("::error::2.4.16 is older than stable 2.4.17", out.getvalue())
        self.assertEqual(fake.writes(), [])

    def test_main_reports_a_refusal_in_the_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            summary = Path(tmp) / "summary.md"
            fake = world(dev_today(), latest="app-v0.3.2")
            with mock.patch.dict("os.environ", {"GITHUB_STEP_SUMMARY": str(summary)}), \
                    redirect_stdout(io.StringIO()) as out:
                self.assertEqual(dc.main(["--repo", SLUG], fake), 1)
            self.assertIn("::error::releases/latest is 'app-v0.3.2'", out.getvalue())
            self.assertIn("refused: releases/latest is 'app-v0.3.2'", summary.read_text())


class Wiring(unittest.TestCase):
    """The two callers, and the button's guard."""

    def steps(self, workflow: str, job: str) -> list[dict]:
        wf = yaml.safe_load((REPO / ".github/workflows" / workflow).read_text(encoding="utf-8"))
        return wf["jobs"][job]["steps"]

    def test_every_firmware_release_runs_it_with_its_own_tag(self):
        steps = self.steps("firmware-release.yml", "build-sign-release")
        calls = [s for s in steps if "dev_channel.py" in s.get("run", "")]
        self.assertEqual(len(calls), 1)
        step = calls[0]
        self.assertIn('--source "$TAG"', step["run"])
        self.assertEqual(step["env"]["TAG"], "${{ steps.ver.outputs.tag }}")
        self.assertNotIn("prerelease", str(step.get("if", "")), "a stable release must refresh it too")
        for s in steps:
            self.assertNotEqual((s.get("with") or {}).get("tag_name"), dc.DEV_TAG,
                                "a second writer of fw-dev-latest in firmware-release.yml")
        names = [s.get("name") for s in steps]
        redeploy = next(i for i, s in enumerate(steps) if "pages.yml" in s.get("run", ""))
        self.assertLess(names.index(step["name"]), redeploy, "the site must redeploy after the copy")

    def test_a_failed_dev_step_on_a_stable_release_still_guards_latest(self):
        # continue-on-error would swallow the script's alarm that
        # releases/latest left the firmware; the action after it does not.
        steps = self.steps("firmware-release.yml", "build-sign-release")
        i = next(i for i, s in enumerate(steps) if "dev_channel.py" in s.get("run", ""))
        self.assertEqual(steps[i]["id"], "dev_channel")
        guard = steps[i + 1]
        self.assertEqual(guard["uses"], "./.github/actions/keep-firmware-latest")
        self.assertIn("steps.dev_channel.outcome == 'failure'", guard["if"])
        self.assertIn("failure()", guard["if"], "a dev release's failed step fails the job; the guard must still run")
        self.assertNotIn("continue-on-error", guard)

    def test_flasher_factory_images_never_publishes_a_dev_build_below_stable(self):
        steps = self.steps("flasher-release.yml", "factory-images")
        names = [s.get("name", "") for s in steps]
        checks = [i for i, s in enumerate(steps) if "--check-train" in s.get("run", "")]
        self.assertEqual(len(checks), 2, "once before the hour of building, once right before publishing")
        first_build = next(i for i, n in enumerate(names) if n.startswith("Build "))
        publish = names.index("Publish the rolling dev-channel release")
        self.assertLess(checks[0], first_build)
        self.assertEqual(checks[1], publish - 1, "stable can move during the build")
        for i in checks:
            self.assertEqual(steps[i]["if"], "inputs.channel == 'dev'")
            self.assertIn('--check-train "$VERSION"', steps[i]["run"])
            self.assertEqual(steps[i]["env"]["VERSION"], "${{ steps.ver.outputs.version }}")
            self.assertNotIn("continue-on-error", steps[i])

    def test_a_tag_rebuild_brings_the_dev_channel_copy_along(self):
        steps = self.steps("flasher-release.yml", "factory-images")
        names = [s.get("name", "") for s in steps]
        i = next(i for i, s in enumerate(steps) if '--source "$TAG"' in s.get("run", ""))
        self.assertIn("dev_channel.py", steps[i]["run"])
        self.assertEqual(steps[i]["env"]["TAG"], "${{ steps.tag.outputs.tag }}")
        self.assertEqual(steps[i]["if"], "steps.attach.outcome == 'success'")
        self.assertLess(names.index("Attach to the release"), i)
        self.assertLess(i, next(j for j, s in enumerate(steps) if "pages.yml" in s.get("run", "")),
                        "the site must redeploy after the copy")
        # The release channel builds the TAG's tree, which may predate the
        # script: the overlay brings today's.
        overlay = next(s for s in steps if s.get("name") == "Apply the tooling overlay")
        self.assertIn("cp _tooling/.github/scripts/dev_channel.py .github/scripts/dev_channel.py", overlay["run"])

    def test_the_button_refreshes_from_latest_or_the_named_tag_then_guards(self):
        wf = yaml.safe_load((REPO / ".github/workflows/dev-channel-refresh.yml").read_text(encoding="utf-8"))
        # PyYAML reads the bare key `on` as True.
        source = (wf.get("on") or wf[True])["workflow_dispatch"]["inputs"]["source"]
        self.assertEqual(source["type"], "string")
        self.assertFalse(source["required"])
        self.assertEqual(source["default"], "", "blank must mean releases/latest, the catch-up")
        steps = self.steps("dev-channel-refresh.yml", "refresh")
        call = next(s for s in steps if "dev_channel.py" in s.get("run", ""))
        # The input reaches the script through the environment, never spliced
        # into the command, and only when it is set.
        self.assertEqual(call["env"]["SOURCE"], "${{ inputs.source }}")
        for s in steps:
            self.assertNotIn("${{", s.get("run", ""), f"{s.get('name')}: an expression inside run:")
        self.assertRegex(call["run"], r'if \[ -n "\$SOURCE" \]; then\s+args\+=\(--source "\$SOURCE"\)')
        self.assertIn('python3 .github/scripts/dev_channel.py "${args[@]}"', call["run"])
        self.assertTrue(any("gh workflow run pages.yml" in s.get("run", "") for s in steps))
        guard = steps[-1]
        self.assertEqual(guard.get("if"), "success() || failure()")
        for needle in ("releases/latest", "fw-v*", "prerelease", "exit 1"):
            self.assertIn(needle, guard["run"])

    def test_the_button_is_a_listed_publisher(self):
        policy = yaml.safe_load((REPO / ".github/ci-policy.yml").read_text(encoding="utf-8"))
        self.assertIn("dev-channel-refresh.yml", policy["main_queue_ok"])


if __name__ == "__main__":
    unittest.main()
