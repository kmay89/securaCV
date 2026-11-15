#!/usr/bin/env python3
"""Holds every board's runtime-thermal-gauge claim to the firmware that would
have to make it true (sweep C35).

`firmware/boards/boards.json` gives each board a `thermal_notes` line, and
`firmware/scripts/pin_budget.py` renders it, under its own intro, into the
generated `firmware/boards/PIN_BUDGET.md`. Six rows said the runtime gauge
was a "die-temp watchdog" (five false; freenove-esp32s3-cam's true but
unflagged), two of them and the intro naming `FEATURE_DIAGNOSTICS`, and the
intro adding "every build ships" it. The truth is per tree, not per board:
the canary reads die temperature in its passive thermal watchdog
(`FEATURE_THERMAL_WATCHDOG`), the canary-wap in `sys_monitor.h`
(`FEATURE_SYS_MONITOR`), and canary-display, canary-vision, canary-sense and
canary-sentinel read none. `FEATURE_DIAGNOSTICS` is the canary's heap / SD
health / self-test flag; its self-test (at boot and on demand) range-checks
die temperature, which is not a watchdog, and no other tree defines it.

What is held, for the boards.json rows, the generated doc's intro, and the
"## Thermals" section of each board's README:

  * a clause that claims a runtime gauge (it says "watchdog" or "gauge" and
    is not a negation such as "no runtime gauge") names a `FEATURE_*` flag;
  * each flag such a clause names is `#define`d in the tree of a product that
    uses the board, and an `#if` on that flag encloses a die-temperature read
    in that tree. The product is the one the clause names nearest before the
    flag; a clause that names none speaks for the board's only user, and a
    board used by several trees must say which tree each flag belongs to;
  * no PlatformIO env that builds the board for that product (the envs
    `firmware/build_matrix.json` names for the board, resolved through
    `extends`, `${...}` and `build_unflags`) sets the flag to 0, as the
    canary's classic-ESP32 envs do for `FEATURE_THERMAL_WATCHDOG`;
  * a board no tree uses has no gauge to claim;
  * a clause saying a named product "reads no die temperature" (or otherwise
    denies it a gauge) names a product whose tree reads none;
  * no clause calls `FEATURE_DIAGNOSTICS` a die-temperature gauge or watchdog.

"Die-temperature read" is a call to the chip's sensor API (Arduino's
`temperatureRead()`, IDF's `temperature_sensor_get_celsius()` /
`temp_sensor_read_celsius()`, the ROM's `temprature_sens_read()`) or to the
canary's shared provider `thermal_read_die_c()`, which the suite checks is
itself a sensor read. Comments are stripped first, so a comment that names
`temperatureRead()` does not count. A product's tree is its `dir` in
`firmware/flavors.json`, plus the `firmware/common` files its sources include
by name (one hop) and the sources beside each such header (a PlatformIO
library compiles its own `.cpp` once a header of it is included), plus every
`firmware/common` source its PlatformIO config names in a `build_src_filter`
(`+<.../common/...>`, in its `platformio.ini` and the `extra_configs` that
lists); `tests_host`/`test`/`tests` directories are not firmware. A build
profile chosen inside a sketch header (the canary-wap's `build_config.h`) is
not modeled: only `-D` flags in the env are.

The suite also mutates the committed text back to each wrong claim it
replaced and requires a failure, so the checker cannot pass by matching
nothing.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import configparser
import copy
import importlib.util
import json
import re
import unittest
from functools import lru_cache
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BOARDS_JSON = REPO / "firmware" / "boards" / "boards.json"
BOARDS_DIR = REPO / "firmware" / "boards"
FLAVORS_JSON = REPO / "firmware" / "flavors.json"
BUILD_MATRIX = REPO / "firmware" / "build_matrix.json"
COMMON_DIR = REPO / "firmware" / "common"
PIN_BUDGET = REPO / "firmware" / "scripts" / "pin_budget.py"

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".ino"}
NOT_FIRMWARE_DIRS = {"tests_host", "test", "tests", ".pio", "node_modules"}

RAW_READS = ("temperatureRead", "temperature_sensor_get_celsius",
             "temp_sensor_read_celsius", "temprature_sens_read")
PROVIDERS = ("thermal_read_die_c",)
RAW_READ_RE = re.compile(r"\b(?:" + "|".join(RAW_READS) + r")\s*\(")
READ_RE = re.compile(r"\b(?:" + "|".join(RAW_READS + PROVIDERS) + r")\s*\(")

FLAG_RE = re.compile(r"\bFEATURE_[A-Z0-9_]+\b")
CLAIM_RE = re.compile(r"\b(?:watchdog|gauge)\b", re.I)
DIE_RE = re.compile(r"\bdie\b|die[- ]temp|thermal watchdog", re.I)
NEGATION_RE = re.compile(
    r"\bno\b[^.;,]{0,24}?\b(?:die[- ]temp\w*|runtime gauge|gauge|watchdog)\b"
    r"|\breads? no die[- ]temp"
    r"|\breads? none\b"
    r"|\bnothing reads\b",
    re.I)
BANNED_FLAG = "FEATURE_DIAGNOSTICS"


# ── firmware side ──────────────────────────────────────────────────────────

def strip_comments(src: str) -> str:
    """C/C++ comments to spaces (newlines kept, so line structure survives);
    string and character literals are walked so a `//` inside one is not a
    comment."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in src[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(src[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def positive_flags(expr: str) -> set[str]:
    """FEATURE_* flags an `#if`/`#elif` expression requires to be on.
    Negated uses (`!FEATURE_X`, `!defined(FEATURE_X)`) do not count."""
    expr = re.sub(r"!\s*defined\s*\(\s*FEATURE_\w+\s*\)", " ", expr)
    expr = re.sub(r"!\s*defined\s+FEATURE_\w+", " ", expr)
    expr = re.sub(r"!\s*FEATURE_\w+", " ", expr)
    return set(FLAG_RE.findall(expr))


def guarded_reads(src: str) -> set[str]:
    """Flags whose `#if` branch encloses a die-temperature read in `src`."""
    flags: set[str] = set()
    stack: list[set[str]] = []
    for line in strip_comments(src).splitlines():
        m = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if m:
            kw, rest = m.group(1), m.group(2)
            if kw == "if":
                stack.append(positive_flags(rest))
            elif kw == "ifdef":
                stack.append(set(FLAG_RE.findall(rest)))
            elif kw == "ifndef":
                stack.append(set())
            elif kw == "elif" and stack:
                stack[-1] = positive_flags(rest)
            elif kw == "else" and stack:
                stack[-1] = set()
            elif kw == "endif" and stack:
                stack.pop()
            continue
        if READ_RE.search(line):
            for frame in stack:
                flags |= frame
    return flags


def _sources(root: Path):
    for p in sorted(root.rglob("*")):
        if p.suffix in SOURCE_SUFFIXES and p.is_file() and not (
                NOT_FIRMWARE_DIRS & set(p.relative_to(root).parts)):
            yield p


@lru_cache(maxsize=None)
def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


@lru_cache(maxsize=None)
def flavor_dirs() -> dict[str, Path]:
    return {f["name"]: REPO / f["dir"]
            for f in json.loads(FLAVORS_JSON.read_text(encoding="utf-8"))}


# ── PlatformIO side ────────────────────────────────────────────────────────

_INTERP_RE = re.compile(r"\$\{([\w:.-]+?)\.(\w+)\}")


@lru_cache(maxsize=None)
def pio_config(product: str) -> configparser.ConfigParser | None:
    """The product's platformio.ini with the extra_configs it lists merged
    in, uninterpolated (None for a tree with no PlatformIO project)."""
    root = flavor_dirs()[product]
    ini = root / "platformio.ini"
    if not ini.is_file():
        return None
    cp = configparser.ConfigParser(
        interpolation=None, strict=False, comment_prefixes=(";", "#"),
        inline_comment_prefixes=(";",))
    cp.optionxform = str  # type: ignore[assignment,method-assign]
    cp.read(ini, encoding="utf-8")
    for extra in cp.get("platformio", "extra_configs", fallback="").split():
        cp.read(root / extra, encoding="utf-8")
    return cp


def pio_value(cp: configparser.ConfigParser, section: str, key: str,
              depth: int = 0) -> str:
    """`key` as PlatformIO sees it in `section`: the section's own value, else
    the first `extends` parent's, else `[env]`'s for an env section; each
    `${section.key}` reference expanded."""
    if depth > 20:
        raise ValueError(f"{section}.{key}: interpolation does not terminate")
    raw = None
    seen, todo = set(), [section]
    while todo and raw is None:
        sec = todo.pop(0)
        if sec in seen or not cp.has_section(sec):
            continue
        seen.add(sec)
        if cp.has_option(sec, key):
            raw = cp.get(sec, key)
        else:
            todo += [s for s in re.split(r"[,\s]+", cp.get(
                sec, "extends", fallback="")) if s]
    if raw is None and section.startswith("env:") and cp.has_option("env", key):
        raw = cp.get("env", key)
    return _INTERP_RE.sub(
        lambda m: pio_value(cp, m.group(1), m.group(2), depth + 1),
        raw or "")


def env_flags(product: str, env: str) -> dict[str, str] | None:
    """The FEATURE_* `-D` values env `env` compiles with (build_flags minus
    build_unflags, last one wins); None when the env is not in the config."""
    cp = pio_config(product)
    if cp is None or not cp.has_section(f"env:{env}"):
        return None
    flags = pio_value(cp, f"env:{env}", "build_flags").split()
    unflags = set(pio_value(cp, f"env:{env}", "build_unflags").split())
    out: dict[str, str] = {}
    for tok in flags:
        m = re.fullmatch(r"-D(FEATURE_[A-Z0-9_]+)(?:=(\S*))?", tok)
        if m and tok not in unflags:
            out[m.group(1)] = m.group(2) if m.group(2) is not None else "1"
    return out


@lru_cache(maxsize=None)
def board_envs() -> dict[str, dict[str, tuple[str, ...]]]:
    """board id → {product (flavor): the envs build_matrix.json builds it with}."""
    out: dict[str, dict[str, list[str]]] = {}
    for p in json.loads(BUILD_MATRIX.read_text(encoding="utf-8"))["products"]:
        flavor = p.get("flavor") or p["id"]
        if flavor not in flavor_dirs():
            continue
        envs = ([p["build"]["env"]] if p.get("build") else
                [lvl["env"] for lvl in (p.get("levels") or {}).values()])
        out.setdefault(p["board"], {}).setdefault(flavor, []).extend(envs)
    return {b: {f: tuple(e) for f, e in d.items()} for b, d in out.items()}


def src_filter_commons(product: str) -> list[Path]:
    """firmware/common sources the product's PlatformIO config compiles by
    name (`+<../../common/x.cpp>` in any build_src_filter)."""
    cp = pio_config(product)
    if cp is None:
        return []
    out = []
    for sec in cp.sections():
        for m in re.finditer(r"\+<(?:\.\./)+common/([^>]+)>",
                             cp.get(sec, "build_src_filter", fallback="")):
            path = COMMON_DIR / m.group(1)
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                out.append(path)
    return out


@lru_cache(maxsize=None)
def tree_files(product: str) -> tuple[Path, ...]:
    """The product's own sources, the firmware/common headers they include by
    name (one hop) with the sources beside each, and the firmware/common
    sources its PlatformIO config compiles by name."""
    own = list(_sources(flavor_dirs()[product]))
    included = set()
    for p in own:
        included.update(Path(m).name for m in re.findall(
            r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', _read(p), re.M))
    commons = list(_sources(COMMON_DIR))
    headers = [p for p in commons if p.name in included]
    dirs = {p.parent for p in headers}
    beside = [p for p in commons if p.parent in dirs
              and p.suffix in {".c", ".cc", ".cpp"}]
    picked = dict.fromkeys(headers + beside + src_filter_commons(product))
    return tuple(own + [p for p in picked if p not in own])


def facts_of(files, read=None) -> tuple[frozenset, frozenset, bool]:
    """(flags #define'd, flags guarding a die-temp read, reads die temp at all)
    over `files`, each read through `read` (the file on disk by default)."""
    read = read or _read
    defined, gauges, reads = set(), set(), False
    for p in files:
        text = read(p)
        defined.update(re.findall(r"^\s*#\s*define\s+(FEATURE_[A-Z0-9_]+)\b",
                                  text, re.M))
        gauges |= guarded_reads(text)
        reads = reads or bool(READ_RE.search(strip_comments(text)))
    return frozenset(defined), frozenset(gauges), reads


@lru_cache(maxsize=None)
def tree_facts(product: str) -> tuple[frozenset, frozenset, bool]:
    return facts_of(tree_files(product))


# ── prose side ─────────────────────────────────────────────────────────────

def clauses(text: str) -> list[str]:
    """Split prose at sentence ends and semicolons outside parentheses."""
    out, depth, cur = [], 0, []
    i = 0
    while i < len(text):
        c = text[i]
        depth += c in "(["
        depth -= c in ")]" and depth > 0
        cur.append(c)
        at_end = i + 1 == len(text) or text[i + 1].isspace()
        if depth == 0 and at_end and (c == ";" or (
                c == "." and not re.search(r"\b(?:e\.g|i\.e|vs|\w\.h)\.$",
                                            "".join(cur[-6:])))):
            out.append("".join(cur).strip())
            cur = []
        i += 1
    if "".join(cur).strip():
        out.append("".join(cur).strip())
    return out


@lru_cache(maxsize=None)
def _product_re() -> re.Pattern:
    names = sorted(flavor_dirs(), key=len, reverse=True)
    return re.compile(r"(?<![\w-])(" + "|".join(map(re.escape, names))
                      + r")(?![\w-])")


def problems(where: str, text: str, users: list[str],
             board: str | None = None) -> list[str]:
    """Every way `text` (one board's thermal prose, used by `users`) claims a
    die-temperature gauge its firmware does not have. With `board`, a flag
    is also held to every env build_matrix.json builds that board with.

    Each clause is a denial (NEGATION_RE), or else every `FEATURE_*` flag it
    names is held as a gauge flag: thermal prose names a flag only to say
    where die temperature is read. A clause that claims a die-temp gauge
    (CLAIM_RE and DIE_RE both match) must name one."""
    errs = []
    for clause in clauses(text):
        named = [(m.start(), m.group(1)) for m in _product_re().finditer(clause)]
        if NEGATION_RE.search(clause):
            for _, product in named:
                if tree_facts(product)[2]:
                    errs.append(f"{where}: says {product} has no die-temp gauge, "
                                f"but its tree reads die temperature: {clause!r}")
            continue
        flags = [(m.start(), m.group(0)) for m in FLAG_RE.finditer(clause)]
        claims = bool(CLAIM_RE.search(clause) and DIE_RE.search(clause))
        if not users and (claims or flags):
            errs.append(f"{where}: claims a runtime gauge, but no firmware tree "
                        f"uses this board: {clause!r}")
            continue
        if claims and not flags:
            errs.append(f"{where}: claims a runtime gauge without naming the "
                        f"flag that provides it: {clause!r}")
        for pos, flag in flags:
            if flag == BANNED_FLAG:
                errs.append(f"{where}: calls {BANNED_FLAG} a die-temp gauge; it "
                            f"is the canary's heap/SD/self-test flag: {clause!r}")
                continue
            before = [p for at, p in named if at < pos]
            if before:
                product = before[-1]
                if product not in users:
                    errs.append(f"{where}: names {flag} on {product}, which does "
                                f"not use this board (used_by {users})")
                    continue
            elif len(users) == 1:
                product = users[0]
            else:
                errs.append(f"{where}: names {flag} without saying which of "
                            f"{users} it belongs to: {clause!r}")
                continue
            defined, gauges, _ = tree_facts(product)
            if flag not in defined:
                errs.append(f"{where}: {flag} is not #define'd anywhere in "
                            f"{product}'s tree")
            elif flag not in gauges:
                errs.append(f"{where}: no #if {flag} in {product}'s tree "
                            f"encloses a die-temperature read")
            for env in board_envs().get(board or "", {}).get(product, ()):
                if (env_flags(product, env) or {}).get(flag) == "0":
                    errs.append(f"{where}: env:{env} builds this board for "
                                f"{product} with {flag}=0: {clause!r}")
    return errs


def board_rows(rows) -> list[str]:
    errs = []
    for b in rows:
        if b.get("thermal_notes"):
            errs += problems(f"boards.json {b['id']}", b["thermal_notes"],
                             b.get("used_by", []), b["id"])
    return errs


def readme_thermals(path: Path) -> str:
    m = re.search(r"^## Thermals\s*\n(.*?)(?=^## |\Z)",
                  path.read_text(encoding="utf-8"), re.M | re.S)
    return " ".join(m.group(1).split()) if m else ""


def load_pin_budget():
    spec = importlib.util.spec_from_file_location("pin_budget", PIN_BUDGET)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


def intro_of(doc: str) -> str:
    """The generated doc's intro paragraph(s) about thermals (the title's
    "fullness gauge" is the pin gauge, not a thermal one)."""
    head = doc.split("## Fleet summary", 1)[0]
    paras = [" ".join(p.split()) for p in re.split(r"\n\s*\n", head)]
    return " ".join(p for p in paras if re.search(r"thermal", p, re.I))


# ── tests ──────────────────────────────────────────────────────────────────

class TreeFacts(unittest.TestCase):
    def test_provider_is_a_sensor_read(self):
        for name in PROVIDERS:
            hosts = [p for prod in flavor_dirs() for p in tree_files(prod)
                     if re.search(r"\bbool\s+" + name + r"\s*\(",
                                  strip_comments(_read(p)))
                     and RAW_READ_RE.search(strip_comments(_read(p)))]
            self.assertTrue(hosts, f"{name}() is counted as a die-temp read, "
                            f"but no tree defines it beside a sensor read")

    def test_trees_are_what_the_notes_describe(self):
        # The per-tree truth the notes and the intro state. If a tree gains or
        # loses a die-temp read, this names the drift before the prose lies.
        self.assertIn("FEATURE_THERMAL_WATCHDOG", tree_facts("canary")[1])
        self.assertIn("FEATURE_SYS_MONITOR", tree_facts("canary-wap")[1])
        self.assertNotIn(BANNED_FLAG, tree_facts("canary-wap")[0])
        self.assertNotIn(BANNED_FLAG, tree_facts("canary-display")[0])
        for product in ("canary-display", "canary-vision", "canary-sense",
                        "canary-sentinel"):
            self.assertFalse(tree_facts(product)[2],
                             f"{product} now reads die temperature; its "
                             f"boards' thermal_notes say it does not")

    def test_shared_sources_are_in_the_tree(self):
        # firmware/common/boot/boot_banner.cpp is compiled by canary-display
        # (a PlatformIO library beside the boot_banner.h it includes) and by
        # canary-sense and canary-sentinel (named in build_src_filter). A
        # die-temp read added there must reach those trees' facts, or the
        # "reads no die temperature" notes would stay green over it.
        banner = COMMON_DIR / "boot" / "boot_banner.cpp"

        def read(p: Path) -> str:
            extra = ("\nstatic float boot_die_c() { return temperatureRead(); }\n"
                     if p == banner else "")
            return _read(p) + extra

        for product in ("canary-display", "canary-sense", "canary-sentinel"):
            files = tree_files(product)
            with self.subTest(product=product):
                self.assertIn(banner, files)
                self.assertFalse(facts_of(files)[2])
                self.assertTrue(facts_of(files, read)[2],
                                f"a read in {banner.name} is invisible to "
                                f"{product}'s tree")
        self.assertIn(banner, src_filter_commons("canary-sense"))
        self.assertIn(banner, src_filter_commons("canary-sentinel"))

    def test_env_model_resolves(self):
        # Every env build_matrix.json builds a noted board with must resolve,
        # or the env check below would pass by finding nothing.
        rows = json.loads(BOARDS_JSON.read_text(encoding="utf-8"))
        for b in rows:
            if not b.get("thermal_notes"):
                continue
            for product, envs in board_envs().get(b["id"], {}).items():
                for env in envs:
                    with self.subTest(board=b["id"], env=env):
                        self.assertIsNotNone(env_flags(product, env))
        # The classic-ESP32 envs turn the canary's watchdog off; the S3 envs
        # that build the same tree leave it on (the header default).
        for env in ("esp32cam", "esp32-wroom"):
            self.assertEqual(env_flags("canary", env)
                             .get("FEATURE_THERMAL_WATCHDOG"), "0", env)
        for env in ("dev", "release_ha", "full", "freenove-s3"):
            self.assertNotEqual(env_flags("canary", env)
                                .get("FEATURE_THERMAL_WATCHDOG"), "0", env)
        self.assertEqual(env_flags("canary", "release_ha")
                         .get("FEATURE_HA_MQTT"), "1")
        # build_unflags drops the release value the classic env re-sets.
        self.assertEqual(env_flags("canary", "esp32cam")
                         .get("FEATURE_TEMP_TAMPER"), "0")

    def test_comment_is_not_a_read(self):
        self.assertEqual(guarded_reads(
            "#if FEATURE_X\n// temperatureRead()\n/* temperatureRead() */\n#endif\n"),
            set())
        self.assertEqual(guarded_reads(
            "#if FEATURE_X\nfloat t = temperatureRead();\n#endif\n"), {"FEATURE_X"})
        self.assertEqual(guarded_reads(
            "#if !FEATURE_X\nfloat t = temperatureRead();\n#else\n#endif\n"), set())
        self.assertEqual(guarded_reads(
            "#if FEATURE_X\n#else\nfloat t = temperatureRead();\n#endif\n"), set())
        self.assertEqual(guarded_reads(
            "#if defined(FEATURE_X) && FEATURE_X\nthermal_read_die_c(&c);\n#endif\n"),
            {"FEATURE_X"})


class ThermalNotes(unittest.TestCase):
    def setUp(self):
        self.rows = json.loads(BOARDS_JSON.read_text(encoding="utf-8"))
        self.by_id = {b["id"]: b for b in self.rows}

    def test_committed_rows_hold(self):
        self.assertEqual(board_rows(self.rows), [])

    def test_committed_readmes_hold(self):
        errs = []
        for b in self.rows:
            readme = BOARDS_DIR / b["id"] / "README.md"
            if readme.exists():
                errs += problems(f"{b['id']}/README.md Thermals",
                                 readme_thermals(readme), b.get("used_by", []),
                                 b["id"])
        self.assertEqual(errs, [])

    def test_generated_intro_holds(self):
        mod = load_pin_budget()
        intro = intro_of(mod.render_doc())
        self.assertIn("thermal", intro.lower())
        self.assertEqual(problems("PIN_BUDGET.md intro", intro,
                                  sorted(flavor_dirs())), [])
        self.assertNotRegex(intro, r"every build ships")
        self.assertEqual(intro_of(mod.DOC_PATH.read_text(encoding="utf-8")),
                         intro, "PIN_BUDGET.md is stale: run "
                         "python3 firmware/scripts/pin_budget.py --write")

    # Each wrong claim this suite replaced must fail it — a checker that
    # passes by matching nothing reads as covered while catching nothing.
    # The exact notes this suite replaced (36119c7a). freenove-esp32s3-cam's
    # was true and fails only for want of its flag; the rest were false.
    OLD = {
        "xiao-esp32s3-sense": "Camera PEEK + Wi-Fi bursts are the heat "
            "sources; sustained capture warms the sensor (image noise). "
            "Runtime gauge is the die-temp watchdog (FEATURE_DIAGNOSTICS). In "
            "enclosures, give the camera ribbon and u.FL antenna airspace and "
            "derate PEEK cadence before the watchdog throttles.",
        "xiao-esp32s3": "Light duty — inference runs on the Vision AI module, "
            "not this host. Die-temp watchdog covers it; no special measures.",
        "freenove-esp32s3-cam": "Same S3 heat sources as the flagship (camera "
            "PEEK + Wi-Fi bursts) on a larger PCB with more copper — runs "
            "cooler than the XIAO. Die-temp watchdog covers it.",
        "waveshare-esp32c6-lcd169": "Battery charging warms the PCB next to "
            "the PCF85063 crystal (clock drift) and the QMI8658 (bias) — "
            "charge with the backlight dimmed and don't calibrate the IMU "
            "mid-charge. Die-temp watchdog is the runtime gauge.",
        "waveshare-esp32s3-lcd147": "USB-A stick body: S3 + octal PSRAM "
            "heat-soak into a small stick that plugs into an already-warm "
            "port. The double-buffered animation budget is the throttle knob; "
            "die-temp watchdog is the gauge.",
        "waveshare-esp32s3-lcd7": "The hottest board in the fleet: a 7\" "
            "backlight plus continuous 800x480 RGB refresh out of octal PSRAM. "
            "Backlight duty (via the CH422G) is the main relief valve and the "
            "die-temp watchdog (FEATURE_DIAGNOSTICS) the gauge; keep the GT911 "
            "glass out of direct sun — capacitive touch drifts when hot.",
    }
    OLD_README = {
        "waveshare-esp32s3-touch-lcd169": "sun. Runtime gauge: the die-temp "
            "watchdog (`FEATURE_DIAGNOSTICS`-class passive watchdog) plus the "
            "night backlight floor, which is also the thermal relief valve.",
        "waveshare-esp32c6-lcd169": "the IMU during charge. Runtime gauge: the "
            "die-temp watchdog (`FEATURE_DIAGNOSTICS`) that every build ships.",
    }

    def test_each_replaced_claim_fails(self):
        for board, text in self.OLD.items():
            rows = copy.deepcopy(self.rows)
            next(b for b in rows if b["id"] == board)["thermal_notes"] = text
            with self.subTest(board=board):
                self.assertTrue(board_rows(rows), f"old {board} note passed")

    def test_each_replaced_readme_claim_fails(self):
        for board, text in self.OLD_README.items():
            with self.subTest(board=board):
                self.assertTrue(problems(board, text,
                                         self.by_id[board].get("used_by", []),
                                         board))

    def test_old_intro_fails(self):
        old = ("The *runtime* thermal gauge is the die-temperature watchdog "
               "every build ships (`FEATURE_DIAGNOSTICS` — see "
               "`firmware/build_matrix.json`).")
        self.assertTrue(problems("intro", old, sorted(flavor_dirs())))

    def test_classic_esp32_cannot_claim_the_watchdog(self):
        # The flag is #define'd in the canary's tree and guards a read there,
        # but the envs that build these two boards set it to 0.
        notes = {
            "esp32cam-ai-thinker": "Camera + Wi-Fi bursts on a thumb-sized "
                "PCB. The canary's passive thermal watchdog "
                "(FEATURE_THERMAL_WATCHDOG) covers it.",
            "esp32-wroom-devkit": "Open-air devkit. Die-temp watchdog "
                "(FEATURE_THERMAL_WATCHDOG) is the runtime gauge.",
        }
        for board, text in notes.items():
            rows = copy.deepcopy(self.rows)
            next(b for b in rows if b["id"] == board)["thermal_notes"] = text
            with self.subTest(board=board):
                errs = board_rows(rows)
                self.assertTrue(any("FEATURE_THERMAL_WATCHDOG=0" in e
                                    for e in errs), errs)
        # The same claim on the S3 board the canary ships on passes.
        self.assertEqual(problems("x", notes["esp32cam-ai-thinker"],
                                  ["canary"], "freenove-esp32s3-cam"), [])

    def test_wrong_tree_and_wrong_flag_fail(self):
        def row(note, users):
            return problems("x", note, users)
        # A flag on the wrong tree of a shared board.
        self.assertTrue(row("Runtime gauge: on the canary-wap, the watchdog "
                            "(FEATURE_THERMAL_WATCHDOG).", ["canary", "canary-wap"]))
        # A flag that exists but guards no die-temp read.
        self.assertTrue(row("Die-temp watchdog (FEATURE_CAMERA_PEEK) covers it.",
                            ["canary-wap"]))
        # A shared board must say which tree each flag is in.
        self.assertTrue(row("Die-temp watchdog (FEATURE_THERMAL_WATCHDOG) "
                            "covers it.", ["canary", "canary-wap"]))
        # A negation must be true of the tree it names.
        self.assertTrue(row("The canary reads no die temperature, so there is "
                            "no runtime gauge.", ["canary"]))
        # And the true forms pass.
        self.assertEqual(row("Die-temp watchdog covers it "
                             "(FEATURE_THERMAL_WATCHDOG).", ["canary"]), [])
        self.assertEqual(row("canary-display reads no die temperature, so "
                             "there is no runtime gauge.", ["canary-display"]), [])


if __name__ == "__main__":
    unittest.main()
