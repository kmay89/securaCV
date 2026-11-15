#!/usr/bin/env python3
"""What each setup-arduino-esp32 cache key stores (sweep CI19).

Run:  python3 -m unittest discover -s .github/scripts -p 'test_setup_arduino_esp32.py' -v

The release jobs (firmware-release.yml, flasher-release.yml) call
.github/actions/setup-arduino-esp32 three times in one job: core 3.3.8, then
2.0.17, then 3.3.10, each with exclusive-core. While the action cached with one
combined actions/cache step, every key was saved in the job's post phase, with
whatever ~/.arduino15 held when the job ended, so on a cold cache all three
keys stored the 3.3.10 install. These tests run the composite action's own
steps, in order, from its action.yml: each `run:` script under bash, with a
stand-in arduino-cli on PATH, and the actions/cache steps against a modeled
cache (restore extracts over the tree without clearing it, as the real one
does; an entry, once saved, is never replaced; a combined step saves in the
job's post phase, in reverse step order, unless its restore was an exact hit).
The stand-in models only what decides a key's contents: `core install` adds a
platform directory and its tools and a downloaded archive under staging/,
`core uninstall` removes the newest installed platform and its tools (as the
CLI does: `core uninstall` refuses a version, internal/cli/core/uninstall.go,
and the service picks the greatest installed release, package_manager.go
GetInstalledPlatformRelease — the action no longer runs it, and the model is
what showed the old warm third row throwing away the core it had restored),
`core list` names the newest. An exclusive call now clears the packager tree
before its restore (an `rm -rf` the harness runs for real under the modeled
HOME). It is not arduino-cli; the real three-core run shows only in a release
run. The last class reads firmware.yml: the display 3.x legs' profile build
installs its own core under ~/.arduino15/internal/, which the action's entry
leaves out, so the leg caches that under a sketch.yaml-keyed entry of its own.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest

import yaml

REPO = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
ACTION = os.path.join(REPO, ".github", "actions", "setup-arduino-esp32", "action.yml")

LATEST = "3.3.10"  # what the stand-in installs for an unversioned request

FAKE_CLI = textwrap.dedent(r"""
    #!/usr/bin/env bash
    # Stand-in arduino-cli: only the effects that decide what a cache key holds.
    set -euo pipefail
    D="$HOME/.arduino15"
    HW="$D/packages/esp32/hardware/esp32"
    TOOLS="$D/packages/esp32/tools"
    newest() { ls -1 "$HW" 2>/dev/null | sort -V | tail -n 1; }
    case "${1:-} ${2:-}" in
      "config init") mkdir -p "$D"; echo "directories: {data: $D}" > "$D/arduino-cli.yaml" ;;
      "config add"|"lib install") : ;;
      "config get") echo "$HOME/Arduino" ;;
      "core update-index") mkdir -p "$D"; echo '{}' > "$D/package_esp32_index.json" ;;
      "core uninstall")
        v="$(newest)"
        if [ -z "$v" ]; then echo "platform not installed" >&2; exit 1; fi
        rm -rf "${HW:?}/$v" "${TOOLS:?}/toolchain-$v" ;;
      "core install")
        spec="${3:?}"; v="${spec#esp32:esp32}"; v="${v#@}"; v="${v:-$FAKE_LATEST}"
        if [ ! -d "$HW/$v" ]; then
          mkdir -p "$D/staging/packages" "$HW/$v" "$TOOLS/toolchain-$v"
          echo "archive $v" > "$D/staging/packages/esp32-$v.tar.gz"
          echo "$v" > "$HW/$v/platform.txt"
        fi ;;
      "core list")
        echo "ID          Installed Latest Name"
        v="$(newest)"; if [ -n "$v" ]; then echo "esp32:esp32 $v $v esp32"; fi ;;
      *) echo "stand-in arduino-cli: unmodeled command: $*" >&2; exit 2 ;;
    esac
""").lstrip()

EXPR_RE = re.compile(r"\$\{\{\s*(.*?)\s*\}\}")
COMPARE_RE = re.compile(r"^([A-Za-z0-9_.-]+)\s*(==|!=)\s*'([^']*)'$")


class Job:
    """One job on one runner: a HOME, a cache shared across jobs, and the
    combined-cache post saves still owed when the job ends."""

    def __init__(self, root: str, cache: dict[str, str], cache_root: str):
        self.home = os.path.join(root, "home")
        os.makedirs(self.home, exist_ok=True)
        self.bin = os.path.join(root, "bin")
        os.makedirs(self.bin, exist_ok=True)
        cli = os.path.join(self.bin, "arduino-cli")
        with open(cli, "w", encoding="utf-8") as f:
            f.write(FAKE_CLI)
        os.chmod(cli, 0o755)
        self.cache = cache          # key -> snapshot dir
        self.cache_root = cache_root
        self.posts: list[tuple[str, list[str], list[str]]] = []
        self.saves: list[str] = []  # keys this job wrote, in order
        self.log: list[str] = []

    # -- the modeled actions/cache ------------------------------------------
    def _paths(self, spec: str) -> tuple[list[str], list[str]]:
        inc, exc = [], []
        for line in spec.splitlines():
            line = line.strip()
            if not line:
                continue
            neg = line.startswith("!")
            p = line[1:] if neg else line
            if any(c in p for c in "*?["):
                raise AssertionError(f"cache path glob not modeled: {line}")
            if not p.startswith("~/"):
                raise AssertionError(f"cache path outside HOME not modeled: {line}")
            (exc if neg else inc).append(os.path.join(self.home, p[2:]))
        return inc, exc

    def _restore(self, key: str, spec: str) -> bool:
        inc, _ = self._paths(spec)
        snap = self.cache.get(key)
        if snap is None:
            return False
        for p in inc:
            src = os.path.join(snap, os.path.relpath(p, self.home))
            if os.path.isdir(src):
                shutil.copytree(src, p, dirs_exist_ok=True, symlinks=True)
        return True

    def _save(self, key: str, spec: str) -> None:
        if key in self.cache:
            self.log.append(f"save {key}: exists, not replaced")
            return
        inc, exc = self._paths(spec)
        snap = os.path.join(self.cache_root, f"entry{len(self.cache)}")
        for p in inc:
            if not os.path.isdir(p):
                continue

            def ignore(d: str, names: list[str], exc=exc) -> list[str]:
                return [n for n in names if os.path.join(d, n) in exc]

            shutil.copytree(p, os.path.join(snap, os.path.relpath(p, self.home)),
                            ignore=ignore, symlinks=True)
        os.makedirs(snap, exist_ok=True)
        self.cache[key] = snap
        self.saves.append(key)

    # -- expressions ----------------------------------------------------------
    def _value(self, ref: str, inputs: dict, outputs: dict) -> str:
        if ref == "runner.os":
            return "Linux"
        parts = ref.split(".")
        if parts[0] == "inputs" and len(parts) == 2:
            return str(inputs.get(parts[1], ""))
        if parts[0] == "steps" and len(parts) == 4 and parts[2] == "outputs":
            return outputs.get(parts[1], {}).get(parts[3], "")
        raise AssertionError(f"expression not modeled: {ref}")

    def _render(self, text: str, inputs: dict, outputs: dict) -> str:
        return EXPR_RE.sub(lambda m: self._value(m.group(1), inputs, outputs), text)

    def _cond(self, cond, inputs: dict, outputs: dict) -> bool:
        if cond is None:
            return True
        expr = EXPR_RE.sub(lambda m: m.group(1), str(cond)).strip()
        for term in expr.split("&&"):
            m = COMPARE_RE.match(term.strip())
            if not m:
                raise AssertionError(f"`if:` term not modeled: {term.strip()!r}")
            left = self._value(m.group(1), inputs, outputs)
            if (left == m.group(3)) != (m.group(2) == "=="):
                return False
        return True

    # -- the composite action -------------------------------------------------
    def call(self, action_path: str, **given: str) -> None:
        with open(action_path, encoding="utf-8") as f:
            action = yaml.safe_load(f)
        inputs = {k: str(v.get("default", "")) for k, v in action["inputs"].items()}
        for k, v in given.items():
            k = k.replace("_", "-")
            if k not in inputs:
                raise AssertionError(f"no such input: {k}")
            inputs[k] = v
        outputs: dict[str, dict[str, str]] = {}
        for step in action["runs"]["steps"]:
            if not self._cond(step.get("if"), inputs, outputs):
                continue
            sid = step.get("id", "")
            with_ = {k: self._render(str(v), inputs, outputs)
                     for k, v in (step.get("with") or {}).items()}
            uses = step.get("uses", "")
            if uses.startswith("arduino/setup-arduino-cli@"):
                continue  # the stand-in is already on PATH
            if uses.startswith("actions/cache/restore@"):
                hit = self._restore(with_["key"], with_["path"])
                outputs[sid] = {"cache-hit": "true" if hit else "false"}
            elif uses.startswith("actions/cache/save@"):
                self._save(with_["key"], with_["path"])
            elif uses.startswith("actions/cache@"):
                hit = self._restore(with_["key"], with_["path"])
                outputs[sid] = {"cache-hit": "true" if hit else "false"}
                if not hit:
                    self.posts.append((with_["key"], [with_["path"]], []))
            elif uses:
                raise AssertionError(f"step not modeled: uses {uses}")
            else:
                outputs[sid] = self._run(step, inputs, outputs)

    def _run(self, step: dict, inputs: dict, outputs: dict) -> dict[str, str]:
        out_file = os.path.join(self.home, ".github_output")
        open(out_file, "w").close()
        env = {
            "HOME": self.home,
            "PATH": self.bin + os.pathsep + os.environ.get("PATH", ""),
            "GITHUB_OUTPUT": out_file,
            "FAKE_LATEST": LATEST,
            "LC_ALL": "C",
        }
        for k, v in (step.get("env") or {}).items():
            env[k] = self._render(str(v), inputs, outputs)
        script = self._render(step["run"], inputs, outputs)
        r = subprocess.run(["bash", "-e", "-c", script], env=env,
                           capture_output=True, text=True, check=False)
        self.log.append(r.stdout + r.stderr)
        if r.returncode != 0:
            raise AssertionError(f"step {step.get('name')!r} failed "
                                 f"({r.returncode}):\n{r.stdout}{r.stderr}")
        got = {}
        with open(out_file, encoding="utf-8") as f:
            for line in f:
                if "=" in line:
                    k, v = line.rstrip("\n").split("=", 1)
                    got[k] = v
        return got

    def end(self) -> None:
        # actions/cache's post step: reverse step order, the tree as the job
        # left it.
        for key, specs, _ in reversed(self.posts):
            self._save(key, specs[0])
        self.posts.clear()


def contents(snap: str) -> dict[str, list[str]]:
    """What a cache entry holds: esp32 platforms, tool dirs, archives, and
    whether a profile build's internal/ came along."""
    base = os.path.join(snap, ".arduino15")

    def ls(*p: str) -> list[str]:
        d = os.path.join(base, *p)
        return sorted(os.listdir(d)) if os.path.isdir(d) else []

    return {
        "platforms": ls("packages", "esp32", "hardware", "esp32"),
        "tools": ls("packages", "esp32", "tools"),
        "staging": ls("staging"),
        "internal": ls("internal"),
    }


class Sim(unittest.TestCase):
    action = ACTION

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = self._tmp.name
        self.cache: dict[str, str] = {}
        self.cache_root = os.path.join(self.root, "cache")
        os.makedirs(self.cache_root)
        self.jobs = 0

    def tearDown(self):
        self._tmp.cleanup()

    def job(self) -> Job:
        self.jobs += 1
        return Job(os.path.join(self.root, f"job{self.jobs}"), self.cache,
                   self.cache_root)

    def release_job(self) -> Job:
        """firmware-release.yml / flasher-release.yml: three cores, one job."""
        j = self.job()
        for v in ("3.3.8", "2.0.17", "3.3.10"):
            j.call(self.action, core_version=v, exclusive_core="true")
        j.end()
        return j

    def entry(self, suffix: str) -> dict[str, list[str]]:
        keys = [k for k in self.cache if k.endswith(suffix)]
        self.assertEqual(len(keys), 1, (suffix, sorted(self.cache)))
        return contents(self.cache[keys[0]])

    def tree(self, j: Job) -> dict[str, list[str]]:
        """The runner's ~/.arduino15 as the job has it now, in the shape
        contents() reads an entry."""
        return contents(j.home)


class EachKeyStoresItsOwnCore(Sim):
    def test_a_cold_release_job_saves_each_core_under_its_own_key(self):
        j = self.release_job()
        self.assertEqual(len(j.saves), 3, j.saves)
        for v in ("3.3.8", "2.0.17", "3.3.10"):
            got = self.entry(f"-core-{v}")
            self.assertEqual(got["platforms"], [v], (v, got))
            self.assertEqual(got["tools"], [f"toolchain-{v}"], (v, got))
            self.assertEqual(got["staging"], [], (v, got))

    def test_a_warm_release_job_saves_nothing_and_each_exclusive_check_holds(self):
        self.release_job()
        before = {k: contents(v) for k, v in self.cache.items()}
        j = self.release_job()
        self.assertEqual(j.saves, [])
        self.assertEqual({k: contents(v) for k, v in self.cache.items()}, before)

    def test_a_warm_release_job_neither_warns_nor_downloads(self):
        # Each exclusive call's restore lands on the tree the previous call
        # filled. Cleared first, a hit is exactly this call's core: nothing
        # downloaded (no new archive under staging/), no second platform
        # beside it, and no "not saving" warning. The old action uninstalled
        # AFTER the restore, which removed only the newest version: on the
        # first row (3.3.8, alone on the tree) and the third (3.3.10 over
        # 2.0.17) the core it had just restored — so those rows downloaded it
        # again, and the third ended holding [2.0.17, 3.3.10] and warned,
        # every warm run.
        self.release_job()
        j = self.job()
        for v in ("3.3.8", "2.0.17", "3.3.10"):
            j.call(self.action, core_version=v, exclusive_core="true")
            got = self.tree(j)
            self.assertEqual(got["platforms"], [v], (v, got))
            self.assertEqual(got["tools"], [f"toolchain-{v}"], (v, got))
            self.assertEqual(got["staging"], [], (v, got))
        j.end()
        self.assertEqual(j.saves, [])
        self.assertFalse([s for s in j.log if "not saving the cache key" in s], j.log)

    def test_a_cold_exclusive_call_after_another_core_downloads_once_and_saves_its_own(self):
        # A cold second row: the first row's core is on the tree, this key
        # misses. The clear removes that core and its tools, the install
        # downloads this one, and the entry holds it alone.
        j = self.job()
        j.call(self.action, core_version="3.3.8", exclusive_core="true")
        j.call(self.action, core_version="2.0.17", exclusive_core="true")
        got = self.tree(j)
        self.assertEqual((got["platforms"], got["tools"]), (["2.0.17"], ["toolchain-2.0.17"]), got)
        self.assertEqual(self.entry("-core-2.0.17")["platforms"], ["2.0.17"])
        j.end()

    def test_a_single_core_job_after_the_release_job_restores_that_core(self):
        # ram_audit.yml / csi_module_disable_matrix.yml: core 3.3.8, not
        # exclusive, on a runner whose cache the release job filled.
        self.release_job()
        j = self.job()
        j.call(self.action, core_version="3.3.8")
        j.end()
        self.assertEqual(j.saves, [])
        hw = os.path.join(j.home, ".arduino15", "packages", "esp32", "hardware", "esp32")
        self.assertEqual(sorted(os.listdir(hw)), ["3.3.8"])

    def test_two_cores_in_one_job_without_exclusive_save_only_the_first(self):
        j = self.job()
        j.call(self.action, core_version="3.3.8")
        j.call(self.action, core_version="3.3.10")
        j.end()
        self.assertEqual(self.entry("-core-3.3.8")["platforms"], ["3.3.8"])
        self.assertFalse([k for k in self.cache if k.endswith("-core-3.3.10")],
                         sorted(self.cache))
        self.assertTrue(any("not saving the cache key" in s for s in j.log), j.log)

    def test_latest_saves_the_one_core_it_installed(self):
        j = self.job()
        j.call(self.action, libraries="ArduinoJson")
        j.end()
        self.assertEqual(len(j.saves), 1, j.saves)
        self.assertRegex(j.saves[0], r"-core-latest-\d{4}W\d{2}$")
        self.assertEqual(contents(self.cache[j.saves[0]])["platforms"], [LATEST])

    def test_a_later_step_cannot_change_what_a_key_stores(self):
        # firmware.yml's display leg runs `arduino-cli compile --profile`
        # after setup, which installs the profile's own core under internal/.
        j = self.job()
        j.call(self.action, core_version="3.3.10")
        os.makedirs(os.path.join(j.home, ".arduino15", "internal",
                                 "esp32_esp32_3.3.10_abc"))
        j.end()
        self.assertEqual(self.entry("-core-3.3.10")["internal"], [])


class TheActionIsTwoSteps(unittest.TestCase):
    """The shape the simulation above relies on, read straight off the file."""

    def setUp(self):
        with open(ACTION, encoding="utf-8") as f:
            self.steps = yaml.safe_load(f)["runs"]["steps"]

    def _index(self, pred) -> list[int]:
        return [i for i, s in enumerate(self.steps) if pred(s)]

    def test_no_combined_cache_step(self):
        self.assertEqual(self._index(
            lambda s: str(s.get("uses", "")).startswith("actions/cache@")), [])

    def test_restore_before_install_and_save_right_after_it(self):
        restore = self._index(lambda s: str(s.get("uses", "")).startswith("actions/cache/restore@"))
        save = self._index(lambda s: str(s.get("uses", "")).startswith("actions/cache/save@"))
        install = self._index(lambda s: "core install" in str(s.get("run", "")))
        self.assertEqual((len(restore), len(save), len(install)), (1, 1, 1))
        self.assertLess(restore[0], install[0])
        self.assertEqual(save[0], install[0] + 1)
        r, s = self.steps[restore[0]], self.steps[save[0]]
        self.assertEqual(r.get("id"), "restore")
        self.assertEqual(r["with"]["key"], s["with"]["key"])
        self.assertEqual(r["with"]["path"], s["with"]["path"])
        self.assertIn("steps.restore.outputs.cache-hit != 'true'", s.get("if", ""))

    def test_an_exclusive_call_clears_before_the_restore_and_nothing_uninstalls_after_it(self):
        restore = self._index(lambda s: str(s.get("uses", "")).startswith("actions/cache/restore@"))
        clear = self._index(lambda s: "rm -rf" in str(s.get("run", ""))
                            and ".arduino15/packages/esp32" in str(s.get("run", "")))
        self.assertEqual((len(restore), len(clear)), (1, 1))
        self.assertLess(clear[0], restore[0])
        self.assertEqual(str(self.steps[clear[0]].get("if", "")).strip(),
                         "inputs.exclusive-core == 'true'")
        # After the restore, `core uninstall esp32:esp32` would remove the
        # newest version, on a warm row the one just restored.
        for s in self.steps[restore[0]:]:
            self.assertNotIn("core uninstall", str(s.get("run", "")), s.get("name"))

    def test_every_filtered_workflow_that_runs_it_watches_it(self):
        # CI.md R5/R6: an edit to the action alone must run every path-
        # filtered workflow that uses it. The release workflows that call it
        # (tag or dispatch only) have no path filter to name it in.
        wfdir = os.path.join(REPO, ".github", "workflows")
        filtered = []
        for name in sorted(os.listdir(wfdir)):
            if not name.endswith((".yml", ".yaml")):
                continue
            with open(os.path.join(wfdir, name), encoding="utf-8") as f:
                text = f.read()
            if "uses: ./.github/actions/setup-arduino-esp32" not in text:
                continue
            on = yaml.safe_load(text).get(True) or {}
            for event in ("push", "pull_request"):
                spec = on.get(event) if isinstance(on, dict) else None
                if isinstance(spec, dict) and "paths" in spec:
                    filtered.append(name)
                    self.assertIn(".github/actions/setup-arduino-esp32/**",
                                  spec["paths"], (name, event))
        self.assertTrue({"firmware.yml", "ram_audit.yml",
                         "csi_module_disable_matrix.yml"} <= set(filtered), filtered)


class TheProfileLegCachesItsOwnCore(unittest.TestCase):
    """firmware.yml's display 3.x legs compile through a sketch.yaml profile.
    arduino-cli installs the profile's platform, its tools and its libraries
    under ~/.arduino15/internal/<id>/ (profiles.go: ProfilesCacheDir), not in
    the Boards-Manager tree, and the setup action keeps internal/ out of its
    entry on purpose. Without an entry of its own, every 3.x leg downloaded
    that core again on every run once the action stopped saving at job end."""

    SKETCH = "firmware/projects/canary-display/arduino/canary_display/sketch.yaml"

    def setUp(self):
        with open(os.path.join(REPO, ".github", "workflows", "firmware.yml"), encoding="utf-8") as f:
            self.steps = yaml.safe_load(f)["jobs"]["build-arduino-cli-display"]["steps"]

    def test_the_profile_build_restores_internal_under_a_sketch_yaml_key(self):
        build = [i for i, s in enumerate(self.steps)
                 if "compile --profile" in str(s.get("run", ""))]
        self.assertEqual(len(build), 1, [s.get("name") for s in self.steps])
        cache = [i for i, s in enumerate(self.steps)
                 if str(s.get("uses", "")).startswith("actions/cache@")
                 and "~/.arduino15/internal" in str(s.get("with", {}).get("path", ""))]
        self.assertEqual(len(cache), 1, [s.get("name") for s in self.steps])
        self.assertLess(cache[0], build[0])
        c, b = self.steps[cache[0]], self.steps[build[0]]
        self.assertEqual(c.get("if"), b.get("if"))
        self.assertEqual(c["with"]["path"].strip(), "~/.arduino15/internal")
        key = c["with"]["key"]
        self.assertIn(f"hashFiles('{self.SKETCH}')", key)
        self.assertIn("matrix.flavor.name", key)
        self.assertIn("runner.os", key)
        self.assertTrue(os.path.isfile(os.path.join(REPO, self.SKETCH)))
        self.assertIn(self.SKETCH.rsplit("/", 1)[0], b["run"])


if __name__ == "__main__":
    sys.exit(unittest.main())
