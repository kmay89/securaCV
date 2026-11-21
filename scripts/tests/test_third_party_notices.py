#!/usr/bin/env python3
"""The desktop apps' third-party notices: one policy, one writer, attached.

The Flasher and Lab installers carried several hundred crates and bundled
programs with no license notices. Each release now writes
THIRD_PARTY_LICENSES.html (.github/scripts/third_party_notices.sh) and
attaches it beside the installers. These cases hold the parts together:

- each app's about.toml accepts exactly deny.toml's `[licenses] allow`, for
  exactly the targets that ship;
- both release workflows run the writer for their own app, on every run, and
  finalize waits for it and attaches the file before anything else;
- the file is attached, never listed in `bundle.resources`: tauri-build
  checks those on disk in every build, and this one exists only on a
  release runner;
- the writer itself, run with a stand-in cargo and curl: the app's name and
  programs land, license text is escaped, and a missing pin, a failed fetch
  or an empty license file writes nothing.

cargo-about's own run (the crates half) needs the tool and the network; the
release workflows' build-only runs exercise it.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import json
import os
import stat
import subprocess
import tempfile
import tomllib
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / ".github" / "scripts" / "third_party_notices.sh"
TEMPLATE = REPO / ".github" / "scripts" / "third_party_notices.hbs"
APPS = {
    "flasher": ("desktop", "desktop-flasher-release.yml", "SecuraCV Flasher"),
    "lab": ("desktop-lab", "desktop-release.yml", "SecuraCV Lab"),
}
SHIPPED_TARGETS = ["aarch64-apple-darwin", "x86_64-apple-darwin", "x86_64-unknown-linux-gnu"]
USBBOOT_SHA = "0123456789abcdef0123456789abcdef01234567"

# `cargo about generate ... --output-file F TEMPLATE`: the template with its
# handlebars removed stands in for the rendered crate list.
CARGO = """#!/bin/sh
out=""; tpl=""
while [ $# -gt 0 ]; do
  case "$1" in --output-file) out="$2"; shift ;; *.hbs) tpl="$1" ;; esac
  shift
done
sed -e 's/{{[^}]*}}//g' "$tpl" > "$out"
"""
# `curl ... URL -o FILE`: logs the URL and writes a license with characters
# HTML must escape; $CURL_MODE makes it fail or answer empty.
CURL = """#!/bin/sh
url=""; out=""
while [ $# -gt 0 ]; do
  case "$1" in -o) out="$2"; shift ;; http*) url="$1" ;; esac
  shift
done
echo "$url" >> "$CURL_LOG"
case "${CURL_MODE:-ok}" in
  fail) exit 22 ;;
  empty) : > "$out" ;;
  *) printf 'Copyright <c> A & B\\n%s\\n' "$url" > "$out" ;;
esac
"""


def workflow(name: str) -> dict:
    return yaml.safe_load((REPO / ".github" / "workflows" / name).read_text(encoding="utf-8"))


def espflash_version() -> str:
    for line in (REPO / ".github" / "espflash-pins.env").read_text(encoding="utf-8").splitlines():
        if line.startswith("ESPFLASH_VERSION="):
            return line.split("=", 1)[1]
    raise AssertionError("no ESPFLASH_VERSION pin")


class OnePolicy(unittest.TestCase):
    def test_each_app_accepts_exactly_what_deny_toml_allows(self):
        allow = tomllib.loads((REPO / "deny.toml").read_text(encoding="utf-8"))["licenses"]["allow"]
        self.assertTrue(allow)
        for app, (tree, _, _) in APPS.items():
            with self.subTest(app=app):
                about = tomllib.loads((REPO / tree / "about.toml").read_text(encoding="utf-8"))
                self.assertEqual(about["accepted"], allow,
                                 f"{tree}/about.toml and deny.toml [licenses] allow must move together")
                self.assertEqual(about["targets"], SHIPPED_TARGETS)
                self.assertIs(about["ignore-dev-dependencies"], True)
                # Nothing else: an unknown key is a per-crate override to cargo-about.
                self.assertEqual(set(about), {"accepted", "targets", "ignore-dev-dependencies"})

    def test_the_template_has_one_marker_and_names_the_app_by_placeholder(self):
        text = TEMPLATE.read_text(encoding="utf-8")
        self.assertEqual(text.count("<!-- bundled-programs -->"), 1)
        self.assertIn("@APP@", text)
        self.assertIn("{{#each licenses}}", text)


class TheReleaseWorkflows(unittest.TestCase):
    def test_each_release_writes_and_attaches_its_own_apps_notices(self):
        for app, (_, name, _) in APPS.items():
            with self.subTest(workflow=name):
                jobs = workflow(name)["jobs"]
                notices = jobs["notices"]
                self.assertNotIn("if", notices, "the notices job runs on build-only runs too")
                self.assertEqual(notices["permissions"], {"contents": "read"})
                runs = [s.get("run", "") for s in notices["steps"]]
                self.assertIn(
                    f'bash .github/scripts/third_party_notices.sh {app} "$RUNNER_TEMP/notices/THIRD_PARTY_LICENSES.html"',
                    runs)
                # The binary needs the `cli` feature: without it `cargo install`
                # succeeds and installs nothing (the 0.12.6 / 0.4.2 release runs).
                self.assertIn('cargo install cargo-about --version "$CARGO_ABOUT_VERSION" --locked --features cli', runs)
                upload = notices["steps"][-1]
                self.assertTrue(upload["uses"].startswith("actions/upload-artifact@"))
                self.assertEqual(upload["with"]["name"], "third-party-licenses")
                self.assertEqual(upload["with"]["if-no-files-found"], "error")

                finalize = jobs["finalize"]
                self.assertEqual(finalize["needs"], ["build", "notices"])
                names = [s.get("name") for s in finalize["steps"]]
                attach = names.index("Attach THIRD_PARTY_LICENSES.html to the release")
                download = finalize["steps"][attach - 1]
                self.assertTrue(download["uses"].startswith("actions/download-artifact@"))
                self.assertEqual(download["with"]["name"], "third-party-licenses")
                self.assertIn('gh release upload "$TAG" "$file" --clobber', finalize["steps"][attach]["run"])
                # Before the updater manifest is touched.
                rewrite = next(i for i, n in enumerate(names) if n and n.startswith("Rewrite latest.json"))
                self.assertLess(attach, rewrite)

    def test_the_flasher_workflow_pins_what_the_writer_reads(self):
        env = workflow("desktop-flasher-release.yml")["env"]
        self.assertRegex(env["USBBOOT_SHA"], r"^[0-9a-f]{40}$")
        self.assertRegex(env["LIBUSB_VERSION"], r"^\d+\.\d+\.\d+$")

    def test_the_notices_are_not_a_bundle_resource(self):
        for tree in ("desktop", "desktop-lab"):
            for conf in sorted((REPO / tree / "src-tauri").glob("tauri*.conf.json")):
                with self.subTest(conf=str(conf.relative_to(REPO))):
                    bundle = json.loads(conf.read_text(encoding="utf-8")).get("bundle", {})
                    self.assertNotIn("THIRD_PARTY", json.dumps(bundle.get("resources", {})))


class TheWriter(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        for name, body in (("cargo", CARGO), ("curl", CURL)):
            exe = self.bin / name
            exe.write_text(body, encoding="utf-8")
            exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
        self.log = self.tmp / "curl.log"
        self.log.write_text("", encoding="utf-8")
        self.out = self.tmp / "out" / "THIRD_PARTY_LICENSES.html"

    def write(self, app: str, **env: str) -> subprocess.CompletedProcess:
        base = {k: v for k, v in os.environ.items() if k not in ("USBBOOT_SHA", "LIBUSB_VERSION")}
        base.update(PATH=f"{self.bin}{os.pathsep}{os.environ['PATH']}", CURL_LOG=str(self.log), **env)
        return subprocess.run(["bash", str(SCRIPT), app, str(self.out)], cwd=REPO, env=base,
                              capture_output=True, text=True, timeout=60)

    def urls(self) -> list[str]:
        return self.log.read_text(encoding="utf-8").split()

    def test_the_lab_carries_espflash_and_nothing_of_the_flashers(self):
        r = self.write("lab")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        html = self.out.read_text(encoding="utf-8")
        version = espflash_version()
        self.assertIn("<title>SecuraCV Lab: third-party licenses</title>", html)
        self.assertIn(f"<h3>espflash {version}</h3>", html)
        self.assertIn("Copyright &lt;c&gt; A &amp; B", html)
        self.assertNotIn("Copyright <c>", html)
        self.assertNotIn("rpiboot", html)
        self.assertNotIn("libusb", html)
        self.assertNotIn("@APP@", html)
        self.assertNotIn("<!-- bundled-programs -->", html)
        self.assertEqual(self.urls(), [
            f"https://raw.githubusercontent.com/esp-rs/espflash/v{version}/LICENSE-MIT",
            f"https://raw.githubusercontent.com/esp-rs/espflash/v{version}/LICENSE-APACHE",
        ])
        # The programs sit inside the page, after the crates.
        self.assertLess(html.index("<h2>Rust crates, by license</h2>"), html.index("<h2>Bundled programs</h2>"))
        self.assertLess(html.index("<h3>espflash"), html.index("</main>"))

    def test_the_flasher_adds_rpiboot_and_libusb_at_its_pins(self):
        r = self.write("flasher", USBBOOT_SHA=USBBOOT_SHA, LIBUSB_VERSION="1.2.3")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        html = self.out.read_text(encoding="utf-8")
        self.assertIn("<title>SecuraCV Flasher: third-party licenses</title>", html)
        self.assertIn("<h3>rpiboot and the mass-storage-gadget64 boot image (Raspberry Pi usbboot)</h3>", html)
        self.assertIn("https://github.com/raspberrypi/buildroot/tree/mass-storage-gadget64", html)
        # usbboot's Apache-2.0 LICENSE is rpiboot's. The boot image is a Linux
        # system image: the file says so, and that its components' license
        # texts are not in it, and never files the image under Apache-2.0.
        self.assertIn("rpiboot is licensed under Apache-2.0", html)
        self.assertIn("The boot image is not one program under that one license", html)
        self.assertIn("Their license texts are not reproduced in this file", html)
        self.assertNotIn("</a>. Apache-2.0.", html)
        self.assertIn("<h3>libusb 1.2.3 (macOS installers)</h3>", html)
        self.assertEqual(self.urls()[2:], [
            f"https://raw.githubusercontent.com/raspberrypi/usbboot/{USBBOOT_SHA}/LICENSE",
            "https://raw.githubusercontent.com/libusb/libusb/v1.2.3/COPYING",
        ])

    def test_the_notice_file_does_not_file_the_boot_image_under_apache(self):
        notice = " ".join((REPO / "NOTICE").read_text(encoding="utf-8").split())
        self.assertIn("rpiboot, the program that runs on your computer, is licensed under the Apache License",
                      notice)
        self.assertIn("boot image that rpiboot sends to the board is not one program under that one license",
                      notice)
        self.assertIn("The license texts of the boot image's components are not reproduced there", notice)
        self.assertNotIn("boot image from Raspberry Pi's usbboot, licensed under the Apache License", notice)

    def test_a_missing_pin_a_failed_fetch_or_an_empty_license_writes_nothing(self):
        cases = {
            "no usbboot pin": ("flasher", {"LIBUSB_VERSION": "1.2.3"}, "USBBOOT_SHA"),
            "no libusb pin": ("flasher", {"USBBOOT_SHA": USBBOOT_SHA}, "LIBUSB_VERSION"),
            "a branch where a commit id belongs": ("flasher", {"USBBOOT_SHA": "main", "LIBUSB_VERSION": "1.2.3"},
                                                   "USBBOOT_SHA"),
            "fetch fails": ("lab", {"CURL_MODE": "fail"}, "could not fetch"),
            "license is empty": ("lab", {"CURL_MODE": "empty"}, "is empty"),
        }
        for label, (app, env, said) in cases.items():
            with self.subTest(case=label):
                r = self.write(app, **env)
                self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
                self.assertIn(said, r.stderr)
                self.assertFalse(self.out.exists(), "a partial notices file was written")

    def test_an_unknown_app_is_a_usage_error(self):
        r = self.write("kernel")
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertFalse(self.out.exists())


if __name__ == "__main__":
    unittest.main()
