#!/usr/bin/env python3
"""The display's RS-485 and CAN drivers keep a compiled caller (sweep F253).

firmware/projects/canary-display/src/io/rs485.cpp and can_bus.cpp compile
only under FEATURE_RS485 / FEATURE_CAN, and their one caller is the dev
playground's fieldbus stations (src/playground/playground.cpp), which compile
only in a 4.3" dash build that also turns on FEATURE_PLAYGROUND or
FEATURE_DEVMODE. F253 found the drivers built in two CI envs with no caller at
all. The fix lives in two places: the `#if` routing in playground.cpp, which
tests_host/test_fieldbus_glue.cpp drives, and the env flags in
firmware/envs/platformio/canary-display.ini that put the playground into the
envs that turn a driver on. Nothing read those flags: dropping
`-DFEATURE_DEVMODE=1` from canary-display-dash-rs485 and -dash-can put F253
back with every gate green, the ESP32 compile included (a driver with no
caller compiles fine), and the host suite took its flags from its own
Makefile, so it could not notice either.

What is held, for every [env:NAME] of the canary-display PlatformIO project
(resolved with scripts/_device_join.py, the reader the other lints share:
extra_configs, `extends`, `${section.key}`):

  1. an env that turns FEATURE_RS485 or FEATURE_CAN on also compiles the
     playground: its config dir defines CD_FLAVOR_DASH, it turns
     FEATURE_PLAYGROUND or FEATURE_DEVMODE on, and no build_src_filter drops
     the playground or the driver;
  2. CI builds every such env (flavor_envs.py's build matrix, the list
     firmware.yml's legs come from), and while a driver source exists some
     CI-built env turns its flag on;
  3. every build of the host suite that compiles playground.cpp (read off
     `make -n -B`, the commands CI's `make -C` runs) has the fieldbus flags,
     config dir and pins board of an env that compiles the playground, and
     every env that turns a driver on has a host build with exactly its
     flags, config and board. The inline path is the same code under
     FEATURE_PLAYGROUND and FEATURE_DEVMODE alone (the only DEVMODE-only block
     in playground.cpp is the bench's long-press exit), so one host build
     with neither driver flag covers dash-b and dash-modes too.

The gates the rules assume (the playground's `#if` and each driver's) are
read from the sources and compared to the text the rules were written for, so
a changed gate fails here and asks for the rules to follow it.

Each rule is also run on mutated copies of the ini, flavors.json and the
Makefile, and must fail on each: a guard that reads as covered while catching
nothing is worse than none.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import copy
import importlib.util
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPTS = REPO / "scripts"
if str(SCRIPTS) not in sys.path:
    sys.path.insert(0, str(SCRIPTS))
import _device_join as dj  # noqa: E402  (needs scripts/ on sys.path first)

_spec = importlib.util.spec_from_file_location(
    "flavor_envs", REPO / "firmware" / "scripts" / "flavor_envs.py")
fe = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fe)  # type: ignore[union-attr]

PRODUCT = "canary-display"
PROJECT = REPO / "firmware" / "projects" / PRODUCT
INI = REPO / "firmware" / "envs" / "platformio" / "canary-display.ini"
FLAVORS = REPO / "firmware" / "flavors.json"
TESTS_HOST = PROJECT / "tests_host"
CONFIGS = REPO / "firmware" / "configs" / PRODUCT
PLAYGROUND_SRC = PROJECT / "src" / "playground" / "playground.cpp"
DRIVERS = {  # flag -> the driver source it compiles
    "FEATURE_RS485": PROJECT / "src" / "io" / "rs485.cpp",
    "FEATURE_CAN": PROJECT / "src" / "io" / "can_bus.cpp",
}
DRIVER_FLAGS = tuple(DRIVERS)
CALLER_FLAGS = ("FEATURE_PLAYGROUND", "FEATURE_DEVMODE")
FIELDBUS_FLAGS = CALLER_FLAGS + DRIVER_FLAGS

# The gates the rules below are written for, as the sources state them
# (whitespace-normalized). A changed gate fails test_gates_are_the_ones_held.
PLAYGROUND_GATE = (
    "#if (((defined(FEATURE_PLAYGROUND) && FEATURE_PLAYGROUND) || "
    "(defined(FEATURE_DEVMODE) && FEATURE_DEVMODE))) && defined(CD_FLAVOR_DASH)")
DRIVER_GATES = {
    "FEATURE_RS485": "#if defined(FEATURE_RS485) && FEATURE_RS485",
    "FEATURE_CAN": "#if defined(FEATURE_CAN) && FEATURE_CAN",
}

CONFIG_DIR_RE = re.compile(r"configs/canary-display/([A-Za-z0-9_-]+)/?$")
PINS_DIR_RE = re.compile(r"boards/([A-Za-z0-9_-]+)/pins/?$")


# ── reading a build ─────────────────────────────────────────────────────────
class Build:
    """What one compile sees: its -D macros (last one wins, -U removes), its
    canary-display config dir(s) and its pins board(s)."""

    def __init__(self, name: str, tokens: list[str]):
        self.name = name
        self.macros: dict[str, str] = {}
        self.configs: set[str] = set()
        self.boards: set[str] = set()
        i = 0
        while i < len(tokens):
            tok = tokens[i]
            arg = None
            for opt in ("-D", "-U", "-I"):
                if tok == opt and i + 1 < len(tokens):
                    arg, i = (opt, tokens[i + 1]), i + 1
                    break
                if tok.startswith(opt) and len(tok) > 2:
                    arg = (opt, tok[2:])
                    break
            i += 1
            if arg is None:
                continue
            opt, val = arg
            if opt == "-D":
                name_, _, value = val.partition("=")
                self.macros[name_] = value if "=" in val else "1"
            elif opt == "-U":
                self.macros.pop(val, None)
            else:
                m = CONFIG_DIR_RE.search(val)
                if m:
                    self.configs.add(m.group(1))
                m = PINS_DIR_RE.search(val)
                if m:
                    self.boards.add(m.group(1))

    def on(self, flag: str) -> bool:
        """`defined(flag) && flag`, as the gates test it."""
        if flag not in self.macros:
            return False
        value = self.macros[flag].strip()
        try:
            return int(value, 0) != 0
        except ValueError:
            return True  # an expression the preprocessor would read; not a literal 0

    def fieldbus(self) -> frozenset[str]:
        return frozenset(f for f in FIELDBUS_FLAGS if self.on(f))

    def identity(self) -> tuple:
        return (self.fieldbus(), frozenset(self.configs), frozenset(self.boards))


def describe(identity: tuple) -> str:
    flags, configs, boards = identity
    return (f"flags {{{', '.join(sorted(flags)) or 'none'}}}, config "
            f"{'/'.join(sorted(configs)) or '?'}, pins {'/'.join(sorted(boards)) or '?'}")


def config_is_dash(config: str, configs_dir: Path = CONFIGS) -> bool:
    header = configs_dir / config / "config.h"
    if not header.is_file():
        return False
    return re.search(r"^\s*#\s*define\s+CD_FLAVOR_DASH\b", header.read_text(encoding="utf-8"),
                     re.M) is not None


def filter_keeps(src_filter: str | None, rel: str) -> bool:
    """Whether a PlatformIO build_src_filter keeps src/<rel>. No filter keeps
    everything; a filter keeps it when it adds `+<*>` and no `-<...>` entry
    names the file or a directory above it. Anything subtler is refused as
    "not kept", so a filter this reader cannot follow fails the rule."""
    if not src_filter:
        return True
    entries = re.findall(r"([+-])<([^>]*)>", src_filter)
    if ("+", "*") not in entries:
        return False
    for sign, path in entries:
        if sign != "-":
            continue
        p = path.rstrip("/")
        if rel == p or rel.startswith(p + "/") or p in ("*", ""):
            return False
    return True


def env_builds(sections) -> dict[str, tuple[Build, str | None]]:
    out = {}
    for section in sections:
        if not section.startswith("env:"):
            continue
        env = section[4:]
        flags = dj.expand(sections, dj.resolve_key(sections, section, "build_flags"))
        try:
            tokens = shlex.split(flags)
        except ValueError:
            tokens = flags.split()
        src_filter = dj.expand(sections, dj.resolve_key(sections, section, "build_src_filter"))
        out[env] = (Build(env, tokens), src_filter or None)
    return out


def compiles_playground(build: Build, src_filter: str | None,
                        configs_dir: Path = CONFIGS) -> list[str]:
    """Why `build` does NOT compile the playground's stations ([] when it does)."""
    why = []
    if len(build.configs) != 1 or not config_is_dash(next(iter(build.configs)), configs_dir):
        why.append(f"its config dir ({'/'.join(sorted(build.configs)) or 'none'}) "
                   "does not define CD_FLAVOR_DASH")
    if not any(build.on(f) for f in CALLER_FLAGS):
        why.append("it turns neither FEATURE_PLAYGROUND nor FEATURE_DEVMODE on")
    if not filter_keeps(src_filter, "playground/playground.cpp"):
        why.append("its build_src_filter drops src/playground/playground.cpp")
    return why


def ci_envs(flavors: list[dict]) -> set[str]:
    return {env for leg in fe.build_matrix(flavors) if leg.get("name") == PRODUCT
            for env in (leg.get("build_envs") or [])}


# ── reading the host suite's builds ─────────────────────────────────────────
def _make_env() -> dict[str, str]:
    env = {k: v for k, v in os.environ.items()
           if k not in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEFILES", "GNUMAKEFLAGS")}
    env["LC_ALL"] = "C"
    return env


def host_builds(makedir: Path = TESTS_HOST, makefile: Path | None = None) -> list[Build]:
    """Every compile in the default goal's plan (`make -n -B`, what CI's
    `make -C` runs) that names src/playground/playground.cpp."""
    cmd = ["make", "--no-print-directory", "-n", "-B", "-C", str(makedir)]
    if makefile is not None:
        cmd += ["-f", str(makefile)]
    proc = subprocess.run(cmd, capture_output=True, text=True, env=_make_env(),
                          timeout=120, check=False)
    if proc.returncode != 0:
        raise AssertionError(f"`{' '.join(cmd)}` failed ({proc.returncode}):\n{proc.stderr}")
    builds = []
    for line in proc.stdout.splitlines():
        try:
            tokens = shlex.split(line)
        except ValueError:
            continue
        if not any(t.endswith("src/playground/playground.cpp") for t in tokens):
            continue
        out = tokens[tokens.index("-o") + 1] if "-o" in tokens[:-1] else "?"
        builds.append(Build(Path(out).name, tokens))
    return builds


# ── the rules ───────────────────────────────────────────────────────────────
def collect(sections, flavors: list[dict], hosts: list[Build],
            configs_dir: Path = CONFIGS, drivers: dict[str, Path] = DRIVERS) -> list[str]:
    errs: list[str] = []
    envs = env_builds(sections)
    built = ci_envs(flavors)
    playground_envs = {e: b for e, (b, f) in envs.items()
                       if not compiles_playground(b, f, configs_dir)}

    # 1 + 2: a driver flag brings its caller, and CI builds it.
    for env, (build, src_filter) in sorted(envs.items()):
        on = [f for f in DRIVER_FLAGS if build.on(f)]
        if not on:
            continue
        for why in compiles_playground(build, src_filter, configs_dir):
            errs.append(f"[env:{env}] turns {' and '.join(on)} on but does not compile the "
                        f"playground, the drivers' only caller: {why} (F253)")
        for flag in on:
            rel = drivers[flag].relative_to(drivers[flag].parents[1]).as_posix()
            if not filter_keeps(src_filter, rel):
                errs.append(f"[env:{env}] turns {flag} on but its build_src_filter drops src/{rel}")
        if env not in built:
            errs.append(f"[env:{env}] turns {' and '.join(on)} on but no leg of CI's build "
                        f"matrix builds it (flavors.json {PRODUCT} build_envs/shards)")
    for flag, src in drivers.items():
        if src.is_file() and not any(envs[e][0].on(flag) for e in built if e in envs):
            errs.append(f"{src.relative_to(REPO)} exists but no CI-built env turns {flag} on: "
                        "the driver is compiled nowhere CI looks")

    # 3: the host suite builds what the envs build.
    if not hosts:
        errs.append(f"no build in `make -n -B -C {TESTS_HOST.relative_to(REPO)}` compiles "
                    "src/playground/playground.cpp")
    env_ids = {e: b.identity() for e, b in playground_envs.items()}
    for host in hosts:
        if host.identity() not in env_ids.values():
            errs.append(f"host build {host.name} ({describe(host.identity())}) matches no env "
                        "that compiles the playground; it tests a build nothing ships")
    host_ids = {h.identity() for h in hosts}
    for env, ident in sorted(env_ids.items()):
        if ident[0] & set(DRIVER_FLAGS):
            if ident not in host_ids:
                errs.append(f"[env:{env}] ({describe(ident)}) has no host build of "
                            "test_fieldbus_glue with the same flags, config and pins")
        elif not any(not (h[0] & set(DRIVER_FLAGS)) and h[1:] == ident[1:] for h in host_ids):
            errs.append(f"[env:{env}] ({describe(ident)}) compiles the stations' inline "
                        "transport and no host build tests it")
    return errs


def load_tree(ini_text: str | None = None, flavors: list[dict] | None = None):
    """The project's sections (with canary-display.ini optionally replaced by
    `ini_text`) and flavors.json."""
    sections: dict[str, dict[str, str]] = {}
    dj.parse_ini((PROJECT / "platformio.ini").read_text(encoding="utf-8"), sections)
    for rel in sections.get("platformio", {}).get("extra_configs", "").split():
        path = (PROJECT / rel).resolve()
        text = path.read_text(encoding="utf-8")
        if ini_text is not None and path == INI.resolve():
            text = ini_text
        dj.parse_ini(text, sections)
    if flavors is None:
        flavors = json.loads(FLAVORS.read_text(encoding="utf-8"))
    return sections, flavors


def _normalized(line: str) -> str:
    return " ".join(line.split())


# ── tests ───────────────────────────────────────────────────────────────────
@unittest.skipUnless(shutil.which("make"), "make is required: the host plan is make's own")
class CommittedTree(unittest.TestCase):
    def test_rules_hold(self):
        sections, flavors = load_tree()
        self.assertEqual(collect(sections, flavors, host_builds()), [])

    def test_the_rules_see_what_they_are_about(self):
        sections, flavors = load_tree()
        envs = env_builds(sections)
        on = {f: sorted(e for e, (b, _) in envs.items() if b.on(f)) for f in DRIVER_FLAGS}
        self.assertEqual(on, {"FEATURE_RS485": ["canary-display-dash-rs485"],
                              "FEATURE_CAN": ["canary-display-dash-can"]})
        hosts = {h.name: sorted(h.fieldbus()) for h in host_builds()}
        self.assertEqual(hosts, {
            "test_fieldbus_glue_inline": ["FEATURE_PLAYGROUND"],
            "test_fieldbus_glue_rs485": ["FEATURE_DEVMODE", "FEATURE_RS485"],
            "test_fieldbus_glue_can": ["FEATURE_CAN", "FEATURE_DEVMODE"],
        })

    def test_gates_are_the_ones_held(self):
        first_if = next(ln for ln in PLAYGROUND_SRC.read_text(encoding="utf-8").splitlines()
                        if ln.startswith("#if"))
        self.assertEqual(_normalized(first_if), PLAYGROUND_GATE)
        for flag, src in DRIVERS.items():
            first_if = next(ln for ln in src.read_text(encoding="utf-8").splitlines()
                            if ln.startswith("#if"))
            self.assertEqual(_normalized(first_if), DRIVER_GATES[flag], src)


def _drop_line(text: str, section: str, line: str) -> str:
    """`text` with the first `line` inside [section] removed."""
    start = text.index(f"[{section}]")
    end = text.find("\n[", start + 1)
    end = len(text) if end < 0 else end
    body = text[start:end]
    assert line in body, (section, line)
    return text[:start] + body.replace(line, "", 1) + text[end:]


def _replace_in(text: str, section: str, old: str, new: str) -> str:
    start = text.index(f"[{section}]")
    end = text.find("\n[", start + 1)
    end = len(text) if end < 0 else end
    body = text[start:end]
    assert old in body, (section, old)
    return text[:start] + body.replace(old, new, 1) + text[end:]


@unittest.skipUnless(shutil.which("make"), "make is required: the host plan is make's own")
class MutantsFail(unittest.TestCase):
    """Each mistake the guard exists for, made on a copy, must fail it."""

    @classmethod
    def setUpClass(cls):
        cls.ini = INI.read_text(encoding="utf-8")
        cls.makefile = (TESTS_HOST / "Makefile").read_text(encoding="utf-8")
        cls.hosts = host_builds()

    def errs_for_ini(self, ini_text: str) -> list[str]:
        sections, flavors = load_tree(ini_text)
        return collect(sections, flavors, self.hosts)

    def errs_for_makefile(self, makefile_text: str) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            mk = Path(tmp) / "Makefile"
            mk.write_text(makefile_text, encoding="utf-8")
            hosts = host_builds(TESTS_HOST, mk)
        sections, flavors = load_tree()
        return collect(sections, flavors, hosts)

    def assertFlags(self, errs: list[str], *needles: str):
        self.assertTrue(errs, "the mutant passed the guard")
        for needle in needles:
            self.assertTrue(any(needle in e for e in errs), (needle, errs))

    # M16 (the review's): both DEVMODE lines gone, the drivers lose their caller.
    def test_devmode_dropped_from_both_driver_envs(self):
        ini = _drop_line(self.ini, "env:canary-display-dash-rs485", "    -DFEATURE_DEVMODE=1\n")
        ini = _drop_line(ini, "env:canary-display-dash-can", "    -DFEATURE_DEVMODE=1\n")
        errs = self.errs_for_ini(ini)
        self.assertFlags(errs,
                         "[env:canary-display-dash-rs485] turns FEATURE_RS485 on but does not "
                         "compile the playground",
                         "[env:canary-display-dash-can] turns FEATURE_CAN on but does not "
                         "compile the playground",
                         "host build test_fieldbus_glue_rs485",
                         "host build test_fieldbus_glue_can")

    def test_devmode_dropped_from_one_env(self):
        ini = _drop_line(self.ini, "env:canary-display-dash-can", "    -DFEATURE_DEVMODE=1\n")
        errs = self.errs_for_ini(ini)
        self.assertFlags(errs, "[env:canary-display-dash-can] turns FEATURE_CAN on")
        self.assertFalse(any("dash-rs485" in e for e in errs), errs)

    def test_devmode_set_to_zero(self):
        ini = _replace_in(self.ini, "env:canary-display-dash-rs485",
                          "-DFEATURE_DEVMODE=1", "-DFEATURE_DEVMODE=0")
        self.assertFlags(self.errs_for_ini(ini), "[env:canary-display-dash-rs485]")

    def test_driver_env_on_a_non_dash_config(self):
        ini = _replace_in(self.ini, "env:canary-display-dash-rs485",
                          "configs/canary-display/dash\n", "configs/canary-display/watch\n")
        self.assertFlags(self.errs_for_ini(ini), "does not define CD_FLAVOR_DASH")

    def test_driver_flag_on_an_env_without_the_playground(self):
        ini = _replace_in(self.ini, "env:canary-display-dash-vault",
                          "    -DFEATURE_TIME_MACHINE_PERSIST=1\n",
                          "    -DFEATURE_TIME_MACHINE_PERSIST=1\n    -DFEATURE_CAN=1\n")
        self.assertFlags(self.errs_for_ini(ini),
                         "[env:canary-display-dash-vault] turns FEATURE_CAN on but does not "
                         "compile the playground")

    def test_src_filter_drops_the_playground(self):
        ini = _replace_in(self.ini, "env:canary-display-dash-can", "build_flags =",
                          "build_src_filter = +<*> -<playground/>\nbuild_flags =")
        self.assertFlags(self.errs_for_ini(ini), "drops src/playground/playground.cpp")

    def test_driver_flag_removed_from_its_env(self):
        ini = _drop_line(self.ini, "env:canary-display-dash-rs485", "    -DFEATURE_RS485=1\n")
        self.assertFlags(self.errs_for_ini(ini),
                         "no CI-built env turns FEATURE_RS485 on",
                         "host build test_fieldbus_glue_rs485")

    def test_driver_env_moved_to_another_board(self):
        ini = _replace_in(self.ini, "env:canary-display-dash-can",
                          "boards/waveshare-esp32s3-lcd43b/pins", "boards/waveshare-esp32s3-lcd43/pins")
        self.assertFlags(self.errs_for_ini(ini),
                         "[env:canary-display-dash-can] (flags {FEATURE_CAN, FEATURE_DEVMODE}, "
                         "config dash, pins waveshare-esp32s3-lcd43) has no host build")

    def test_driver_env_not_built_by_ci(self):
        sections, flavors = load_tree()
        flavors = copy.deepcopy(flavors)
        for entry in flavors:
            if entry.get("name") == PRODUCT:
                entry["build_envs"].remove("canary-display-dash-can")
        self.assertFlags(collect(sections, flavors, self.hosts),
                         "[env:canary-display-dash-can] turns FEATURE_CAN on but no leg",
                         "no CI-built env turns FEATURE_CAN on")

    def test_makefile_rs485_build_without_devmode(self):
        mk = self.makefile.replace(
            "-DFEATURE_DEVMODE=1 -DFEATURE_RS485=1 -o $@", "-DFEATURE_RS485=1 -o $@", 1)
        self.assertNotEqual(mk, self.makefile)
        self.assertFlags(self.errs_for_makefile(mk),
                         "host build test_fieldbus_glue_rs485 (flags {FEATURE_RS485}",
                         "[env:canary-display-dash-rs485]")

    def test_makefile_can_build_flags_swapped(self):
        mk = self.makefile.replace(
            "-DFEATURE_DEVMODE=1 -DFEATURE_CAN=1 -o $@", "-DFEATURE_DEVMODE=1 -DFEATURE_RS485=1 -o $@", 1)
        self.assertNotEqual(mk, self.makefile)
        self.assertFlags(self.errs_for_makefile(mk), "[env:canary-display-dash-can]")

    def test_makefile_can_build_dropped_from_run(self):
        mk = self.makefile.replace(
            "run-fieldbus-glue: test_fieldbus_glue_inline test_fieldbus_glue_rs485 test_fieldbus_glue_can",
            "run-fieldbus-glue: test_fieldbus_glue_inline test_fieldbus_glue_rs485", 1)
        mk = mk.replace("\t$(call run_marked,test_fieldbus_glue_can)\n", "", 1)
        self.assertNotEqual(mk, self.makefile)
        self.assertFlags(self.errs_for_makefile(mk), "[env:canary-display-dash-can]")

    def test_makefile_inline_build_dropped(self):
        mk = self.makefile.replace(
            "run-fieldbus-glue: test_fieldbus_glue_inline ", "run-fieldbus-glue: ", 1)
        mk = mk.replace("\t$(call run_marked,test_fieldbus_glue_inline)\n", "", 1)
        self.assertNotEqual(mk, self.makefile)
        self.assertFlags(self.errs_for_makefile(mk),
                         "[env:canary-display-playground]",
                         "compiles the stations' inline transport and no host build tests it")

    def test_makefile_build_on_another_config(self):
        mk = self.makefile.replace(
            "-I ../../../configs/canary-display/dash \\\n  -I ../../../boards",
            "-I ../../../configs/canary-display/dash7 \\\n  -I ../../../boards", 1)
        self.assertNotEqual(mk, self.makefile)
        self.assertFlags(self.errs_for_makefile(mk), "matches no env that compiles the playground")


class Readers(unittest.TestCase):
    def test_build_reads_both_spellings_and_last_wins(self):
        b = Build("x", ["-DFEATURE_CAN=1", "-D", "FEATURE_DEVMODE", "-DFEATURE_CAN=0",
                        "-I", "x/configs/canary-display/dash", "-Iy/boards/b1/pins"])
        self.assertFalse(b.on("FEATURE_CAN"))
        self.assertTrue(b.on("FEATURE_DEVMODE"))
        self.assertEqual(b.configs, {"dash"})
        self.assertEqual(b.boards, {"b1"})
        self.assertFalse(Build("u", ["-DFEATURE_RS485=1", "-UFEATURE_RS485"]).on("FEATURE_RS485"))

    def test_filter_keeps(self):
        self.assertTrue(filter_keeps(None, "playground/playground.cpp"))
        self.assertTrue(filter_keeps("+<*>\n+<../../../common/x.cpp>", "playground/playground.cpp"))
        self.assertFalse(filter_keeps("+<*> -<playground/>", "playground/playground.cpp"))
        self.assertFalse(filter_keeps("+<*> -<playground/playground.cpp>", "playground/playground.cpp"))
        self.assertFalse(filter_keeps("+<main.cpp>", "playground/playground.cpp"))
        self.assertTrue(filter_keeps("+<*> -<io/>", "playground/playground.cpp"))


if __name__ == "__main__":
    unittest.main()
