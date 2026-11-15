#!/usr/bin/env python3
"""The desktop apps' AppImage tools are pinned, placed, and proven used (CI21).

Run:  python3 -m unittest discover -s .github/scripts -p 'test_pin_appimage_tools.py' -v

.github/actions/pin-appimage-tools puts sha256-pinned copies of the tools
tauri-bundler would download (AppRun, linuxdeploy, the linuxdeploy AppImage
plugin) in its tools directory, under the names prepare_tools() looks for, and
a pinned AppImage runtime where appimagetool is told to find it. These tests
hold four things:

  - pins.txt names exactly the files the bundler looks for, from dated (never
    `continuous`) URLs, each with a sha256;
  - the tauri CLI both lockfiles install is a version those names were read
    at (pins.txt `tauri-cli-read-at`), so a CLI bump fails here until someone
    re-reads linuxdeploy.rs at the new tag;
  - pin.sh, run with a stand-in curl, places each file under its name,
    executable, refuses a download whose hash differs, and after a bundle
    tells a bundle that used the files from one that did not;
  - both release workflows run the pin immediately before, and the verify
    immediately after, every tauri-action step a Linux leg can take.

What these cannot show: that a real `tauri build` reads the tools directory
the way linuxdeploy.rs says. That is proven by the verify step itself, on the
release workflows' build-only dispatches (desktop-flasher-release dry_run,
desktop-release publish=false) and on every release after.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest

import yaml

REPO = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
ACTION_DIR = os.path.join(REPO, ".github", "actions", "pin-appimage-tools")
PIN_SH = os.path.join(ACTION_DIR, "pin.sh")
PINS = os.path.join(ACTION_DIR, "pins.txt")
LOCKFILES = ("desktop/package-lock.json", "desktop-lab/package-lock.json")
RELEASES = {
    ".github/workflows/desktop-flasher-release.yml": "desktop",
    ".github/workflows/desktop-release.yml": "desktop-lab",
}

# What prepare_tools() in tauri-bundler's linuxdeploy.rs looks for and
# fetches, for arch x86_64 (tools_arch = the target triple's first field),
# read at the versions pins.txt names: (placed name, upstream asset path).
BUNDLER_FILES = {
    "AppRun-x86_64":
        "tauri-apps/binary-releases/releases/download/apprun-old/AppRun-x86_64",
    "linuxdeploy-07333c6-x86_64.AppImage":
        "tauri-apps/binary-releases/releases/download/linuxdeploy-07333c6/"
        "linuxdeploy-x86_64.AppImage",
    # The bundler fetches this one from `continuous`; pinned to a dated tag.
    "linuxdeploy-plugin-appimage.AppImage":
        "linuxdeploy/linuxdeploy-plugin-appimage/releases/download/*/"
        "linuxdeploy-plugin-appimage-x86_64.AppImage",
}
RUNTIME = "type2-runtime-x86_64"


def read_pins(path: str = PINS) -> tuple[list[str], list[tuple[str, str, str, str]]]:
    read_at: list[str] = []
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"^#\s*tauri-cli-read-at:\s*(.+)$", line)
            if m:
                read_at = m.group(1).split()
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rows.append(tuple(line.split()))
    return read_at, rows


def cli_versions(lockfile: str) -> dict[str, str | None]:
    """The tauri CLI a lockfile installs: the JS package and the Linux x64
    binary package that carries the bundler (tauri-action runs the project's
    own CLI, `npm run tauri`, when package.json depends on it)."""
    with open(lockfile, encoding="utf-8") as f:
        packages = json.load(f).get("packages", {})
    return {name: (packages.get(f"node_modules/{name}") or {}).get("version")
            for name in ("@tauri-apps/cli", "@tauri-apps/cli-linux-x64-gnu")}


def unread_cli(lockfile: str, read_at: list[str]) -> list[str]:
    """Problems with a lockfile's CLI against the versions the names were read
    at; empty when it is one of them."""
    problems = []
    for name, version in cli_versions(lockfile).items():
        if version is None:
            problems.append(f"{lockfile}: no {name} entry")
        elif version not in read_at:
            problems.append(
                f"{lockfile}: {name} {version} is not a version pins.txt's "
                f"names were read at ({', '.join(read_at)}): read "
                "crates/tauri-bundler/src/bundle/linux/appimage/linuxdeploy.rs "
                f"at @tauri-apps/cli-v{version}, move any name or URL that "
                "changed, and add the version to `tauri-cli-read-at`")
    return problems


class PinsTable(unittest.TestCase):
    def setUp(self):
        self.read_at, self.rows = read_pins()

    def test_names_are_the_bundlers_plus_the_runtime(self):
        names = [r[0] for r in self.rows]
        self.assertEqual(sorted(names), sorted([*BUNDLER_FILES, RUNTIME]))
        self.assertEqual(len(names), len(set(names)), names)

    def test_each_row_is_dated_hashed_and_from_the_bundlers_source(self):
        for name, sha, after, url in self.rows:
            self.assertRegex(sha, r"^[0-9a-f]{64}$", name)
            self.assertRegex(after, r"^([0-9a-f]{64}|=)$", name)
            self.assertNotIn("/continuous/", url, name)
            self.assertNotIn("/latest/", url, name)
            self.assertTrue(url.startswith("https://github.com/"), url)
            if name in BUNDLER_FILES:
                pattern = re.escape(BUNDLER_FILES[name]).replace(r"\*", r"[^/]+")
                self.assertRegex(url[len("https://github.com/"):], f"^{pattern}$", name)
        runtime = [r for r in self.rows if r[0] == RUNTIME][0]
        self.assertRegex(runtime[3], r"/AppImage/type2-runtime/releases/download/"
                         r"[^/]+/runtime-x86_64$")

    def test_only_linuxdeploy_changes_in_the_bundle(self):
        # prepare_tools() zeroes bytes 8-10 of linuxdeploy (its AppImage
        # magic) and writes nothing else of these files.
        changed = {r[0] for r in self.rows if r[2] != "="}
        self.assertEqual(changed, {"linuxdeploy-07333c6-x86_64.AppImage"})
        ld = [r for r in self.rows if r[2] != "="][0]
        self.assertNotEqual(ld[1], ld[2])


class TheCliIsOneTheNamesWereReadAt(unittest.TestCase):
    def setUp(self):
        self.read_at, _ = read_pins()

    def test_read_at_is_a_list_of_versions(self):
        # Today 2.12.3 and 2.12.4: linuxdeploy.rs is byte-identical at both
        # tags, so the lockfiles pass at either (2.12.3 now; #1907/#1908 move
        # them to 2.12.4).
        self.assertTrue(self.read_at)
        for v in self.read_at:
            self.assertRegex(v, r"^\d+\.\d+\.\d+$")

    def test_both_lockfiles(self):
        for lock in LOCKFILES:
            self.assertEqual(unread_cli(os.path.join(REPO, lock), self.read_at), [], lock)

    def _lock(self, d: str, version: str) -> str:
        path = os.path.join(d, "package-lock.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"packages": {
                "node_modules/@tauri-apps/cli": {"version": version},
                "node_modules/@tauri-apps/cli-linux-x64-gnu": {"version": version},
            }}, f)
        return path

    def test_an_unread_version_fails_and_a_read_one_passes(self):
        with tempfile.TemporaryDirectory() as d:
            for v in ("2.12.3", "2.12.4"):
                self.assertEqual(unread_cli(self._lock(d, v), self.read_at), [], v)
            for v in ("2.12.5", "2.13.0", "2.12.2"):
                probs = unread_cli(self._lock(d, v), self.read_at)
                self.assertEqual(len(probs), 2, (v, probs))
                self.assertIn(f"@tauri-apps/cli-v{v}", probs[0])


FAKE_CURL = textwrap.dedent(r"""
    #!/usr/bin/env bash
    # Stand-in curl: serves $FAKE_SERVE/<url basename>, logs each URL.
    out=""; url=""
    while [ $# -gt 0 ]; do
      case "$1" in
        -o) out="$2"; shift 2 ;;
        --retry) shift 2 ;;
        -*) shift ;;
        *) url="$1"; shift ;;
      esac
    done
    echo "$url" >> "$FAKE_CURL_LOG"
    src="$FAKE_SERVE/${url##*/}"
    [ -f "$src" ] || { echo "stand-in curl: 404 $url" >&2; exit 22; }
    cp "$src" "$out"
""").lstrip()


def sha(path: str) -> str:
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


@unittest.skipUnless(shutil.which("bash") and shutil.which("readelf")
                     and shutil.which("cc"), "needs bash, readelf and cc")
class PinScript(unittest.TestCase):
    """pin.sh itself, offline: synthetic assets with their own pins table
    (the real assets are 35 MB and not in the repo), served by a stand-in
    curl; the committed table only for the refusal."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        t = self.t = self._tmp.name
        self.home = os.path.join(t, "home")
        self.serve = os.path.join(t, "serve")
        self.bin = os.path.join(t, "bin")
        for d in (self.home, self.serve, self.bin):
            os.makedirs(d)
        with open(os.path.join(self.bin, "curl"), "w", encoding="utf-8") as f:
            f.write(FAKE_CURL)
        os.chmod(os.path.join(self.bin, "curl"), 0o755)
        self.log = os.path.join(t, "curl.log")
        self.genv = os.path.join(t, "github_env")
        open(self.genv, "w").close()
        self.tools = os.path.join(self.home, ".cache", "tauri")
        # Synthetic assets. The runtime is a real ELF with a Build ID, since
        # verify reads it with readelf.
        self.runtime_id = "0123456789abcdef0123456789abcdef01234567"
        self.runtime = self._elf("runtime", self.runtime_id)
        self.assets = {
            "AppRun-x86_64": self._blob("AppRun-x86_64", b"\x7fELF apprun"),
            "linuxdeploy-x86_64.AppImage": self._blob(
                "linuxdeploy-x86_64.AppImage", b"\x7fELF\x02\x01\x01\x00AI\x02 linuxdeploy"),
            "linuxdeploy-plugin-appimage-x86_64.AppImage": self._blob(
                "linuxdeploy-plugin-appimage-x86_64.AppImage", b"\x7fELF plugin"),
            "runtime-x86_64": self.runtime,
        }
        with open(self.assets["linuxdeploy-x86_64.AppImage"], "rb") as f:
            ld = bytearray(f.read())
        ld[8:11] = b"\0\0\0"
        self.ld_after = hashlib.sha256(bytes(ld)).hexdigest()
        self.table = os.path.join(t, "pins.txt")
        with open(self.table, "w", encoding="utf-8") as f:
            f.write("# synthetic\n")
            for name, asset, after, url in (
                ("AppRun-x86_64", "AppRun-x86_64", "=",
                 "https://example.invalid/apprun-old/AppRun-x86_64"),
                ("linuxdeploy-07333c6-x86_64.AppImage", "linuxdeploy-x86_64.AppImage",
                 self.ld_after,
                 "https://example.invalid/linuxdeploy-07333c6/linuxdeploy-x86_64.AppImage"),
                ("linuxdeploy-plugin-appimage.AppImage",
                 "linuxdeploy-plugin-appimage-x86_64.AppImage", "=",
                 "https://example.invalid/1-alpha/linuxdeploy-plugin-appimage-x86_64.AppImage"),
                (RUNTIME, "runtime-x86_64", "=",
                 "https://example.invalid/20251108/runtime-x86_64"),
            ):
                f.write(f"{name} {sha(self.assets[asset])} {after} {url}\n")
        self.project = os.path.join(t, "proj")

    def tearDown(self):
        self._tmp.cleanup()

    def _blob(self, name: str, data: bytes) -> str:
        p = os.path.join(self.serve, name)
        with open(p, "wb") as f:
            f.write(data)
        return p

    def _elf(self, name: str, build_id: str) -> str:
        out = os.path.join(self.serve, name + "-x86_64")
        subprocess.run(["cc", "-x", "c", "-", "-o", out, f"-Wl,--build-id=0x{build_id}"],
                       input="int main(void){return 0;}\n", text=True, check=True,
                       capture_output=True)
        return out

    def pin(self, *args: str) -> subprocess.CompletedProcess:
        env = {"HOME": self.home, "PATH": self.bin + os.pathsep + os.environ["PATH"],
               "GITHUB_ENV": self.genv, "FAKE_SERVE": self.serve,
               "FAKE_CURL_LOG": self.log, "LC_ALL": "C"}
        return subprocess.run(["bash", PIN_SH, *args], env=env, capture_output=True,
                              text=True, check=False)

    def bundle(self, *, mark: bool = True, runtime: str | None = None) -> str:
        """What a tauri build leaves: the dd mark on linuxdeploy, and an
        AppImage that is a runtime with a filesystem appended."""
        if mark:
            p = os.path.join(self.tools, "linuxdeploy-07333c6-x86_64.AppImage")
            with open(p, "r+b") as f:
                f.seek(8)
                f.write(b"\0\0\0")
        out = os.path.join(self.project, "src-tauri", "target", "release", "bundle", "appimage")
        os.makedirs(out, exist_ok=True)
        img = os.path.join(out, "App_1.0.0_amd64.AppImage")
        with open(runtime or self.runtime, "rb") as src, open(img, "wb") as f:
            f.write(src.read() + b"hsqs" + b"\0" * 64)
        return img

    def test_place_puts_each_file_under_its_name_executable(self):
        r = self.pin("place", "", self.table)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        _, rows = read_pins(self.table)
        for name, digest, _, _ in rows:
            p = os.path.join(self.tools, name)
            self.assertEqual(sha(p), digest, name)
            self.assertEqual(os.stat(p).st_mode & 0o777, 0o755, name)
        self.assertEqual(sorted(os.listdir(self.tools)), sorted(r[0] for r in rows))
        with open(self.genv, encoding="utf-8") as f:
            self.assertEqual(f.read(), f"LDAI_RUNTIME_FILE={self.tools}/{RUNTIME}\n")

    def test_place_overwrites_what_was_there(self):
        os.makedirs(self.tools)
        stale = os.path.join(self.tools, "AppRun-x86_64")
        with open(stale, "wb") as f:
            f.write(b"left by an earlier run")
        self.assertEqual(self.pin("place", "", self.table).returncode, 0)
        self.assertEqual(sha(stale), sha(self.assets["AppRun-x86_64"]))

    def test_xdg_cache_home_moves_the_directory_as_the_bundler_does(self):
        xdg = os.path.join(self.t, "xdg")
        env_cmd = ["env", f"XDG_CACHE_HOME={xdg}", "bash", PIN_SH, "place", "", self.table]
        env = {"HOME": self.home, "PATH": self.bin + os.pathsep + os.environ["PATH"],
               "FAKE_SERVE": self.serve, "FAKE_CURL_LOG": self.log}
        r = subprocess.run(env_cmd, env=env, capture_output=True, text=True, check=False)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(os.path.isfile(os.path.join(xdg, "tauri", RUNTIME)))
        self.assertFalse(os.path.exists(self.tools))

    def test_a_planted_wrong_hash_fails_and_places_nothing_for_it(self):
        # The committed table, with every asset served as the wrong bytes: a
        # replaced release asset, or a tag moved to another build.
        for name in ("AppRun-x86_64", "linuxdeploy-x86_64.AppImage",
                     "linuxdeploy-plugin-appimage-x86_64.AppImage", "runtime-x86_64"):
            self._blob(name, b"not the pinned bytes")
        r = self.pin("place")
        self.assertNotEqual(r.returncode, 0, r.stdout)
        self.assertIn("::error::AppRun-x86_64:", r.stdout)
        self.assertIn("The release asset was replaced or its tag moved", r.stdout)
        self.assertFalse(os.path.exists(os.path.join(self.tools, "AppRun-x86_64")))
        self.assertEqual(os.listdir(self.tools), [])  # no temp file left either

    def test_the_committed_table_asks_for_dated_urls_only(self):
        self.pin("place")  # fails on the first hash; the log has what it asked
        with open(self.log, encoding="utf-8") as f:
            asked = f.read().split()
        self.assertTrue(asked)
        for url in asked:
            self.assertNotIn("/continuous/", url)

    def test_verify_passes_after_a_bundle_that_used_the_tools(self):
        self.assertEqual(self.pin("place", "", self.table).returncode, 0)
        self.bundle()
        r = self.pin("verify", self.project, self.table)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(f"AppImage runtime build {self.runtime_id}", r.stdout)

    def test_verify_fails_when_the_bundler_never_read_the_directory(self):
        self.assertEqual(self.pin("place", "", self.table).returncode, 0)
        self.bundle(mark=False)
        r = self.pin("verify", self.project, self.table)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("never ran prepare_tools()", r.stdout)

    def test_verify_fails_when_a_tool_was_replaced(self):
        self.assertEqual(self.pin("place", "", self.table).returncode, 0)
        self.bundle()
        with open(os.path.join(self.tools, "linuxdeploy-plugin-appimage.AppImage"), "wb") as f:
            f.write(b"continuous build")
        r = self.pin("verify", self.project, self.table)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("linuxdeploy-plugin-appimage.AppImage in", r.stdout)

    def test_verify_fails_on_an_appimage_with_another_runtime(self):
        self.assertEqual(self.pin("place", "", self.table).returncode, 0)
        other = self._elf("other", "fedcba9876543210fedcba9876543210fedcba98")
        self.bundle(runtime=other)
        r = self.pin("verify", self.project, self.table)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not the pinned type2-runtime-x86_64", r.stdout)

    def test_verify_fails_with_no_appimage_to_check(self):
        self.assertEqual(self.pin("place", "", self.table).returncode, 0)
        r = self.pin("verify", self.project, self.table)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("no .AppImage under", r.stdout)


class BothReleasesPinAndVerify(unittest.TestCase):
    """Every tauri-action step a Linux leg can take is bracketed: the pin
    right before it, the verify (with its projectPath) right after it, both
    under the build step's own `if:`."""

    PIN = "./.github/actions/pin-appimage-tools"

    def _linux_jobs(self, wf: dict):
        for name, job in (wf.get("jobs") or {}).items():
            include = ((job.get("strategy") or {}).get("matrix") or {}).get("include") or []
            labels = [str(e.get("platform", "")) for e in include] or [str(job.get("runs-on", ""))]
            if any("ubuntu" in lbl for lbl in labels):
                yield name, job

    def test_each_tauri_build_is_bracketed(self):
        seen = 0
        for path, project in RELEASES.items():
            with open(os.path.join(REPO, path), encoding="utf-8") as f:
                wf = yaml.safe_load(f)
            for job_name, job in self._linux_jobs(wf):
                steps = job.get("steps") or []
                for i, step in enumerate(steps):
                    if not str(step.get("uses", "")).startswith("tauri-apps/tauri-action@"):
                        continue
                    seen += 1
                    where = f"{path} {job_name} step {step.get('name')!r}"
                    self.assertEqual(step["with"]["projectPath"], project, where)
                    before, after = steps[i - 1], steps[i + 1]
                    self.assertEqual(before.get("uses"), self.PIN, where)
                    self.assertEqual((before.get("with") or {}).get("mode", "place"),
                                     "place", where)
                    self.assertEqual(before.get("if"), step.get("if"), where)
                    self.assertEqual(after.get("uses"), self.PIN, where)
                    self.assertEqual(after.get("with"),
                                     {"mode": "verify", "project-path": project}, where)
                    self.assertEqual(after.get("if"), step.get("if"), where)
        self.assertEqual(seen, 4)

    def test_no_other_workflow_runs_tauri_action(self):
        wfdir = os.path.join(REPO, ".github", "workflows")
        users = []
        for n in sorted(os.listdir(wfdir)):
            with open(os.path.join(wfdir, n), encoding="utf-8") as f:
                if "tauri-apps/tauri-action@" in f.read():
                    users.append(n)
        self.assertEqual(users, sorted(os.path.basename(p) for p in RELEASES))


if __name__ == "__main__":
    unittest.main()
