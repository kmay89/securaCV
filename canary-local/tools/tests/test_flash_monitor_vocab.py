#!/usr/bin/env python3
"""Each product's monitor commands are its own firmware's (sweep A86).

gen_flash.py's monitor_block() puts a `monitor` record on every flash.json
product — the command chips and the box placeholder all three benches show
after a flash — from MONITOR_VOCAB, and refuses a chip its firmware does not
answer: a key handle_serial_commands() does not take with no `#if` around it,
a tuning-console word or argument execute_() does not read, a label that is
not the firmware's own help-menu words for that command, a "reads nothing"
claim about a firmware that reads its console, a word console its project
never feeds. The table refusals run against the real firmware tree; the
parser readings (comments, literals and #if-gated lines are not answers, a
handler inside an #if is refused, an unwired tuning console is refused) run
on synthetic sources. The one-table-for-all chips the benches showed before
(h j i s t w m b, the WAP sketch's menu) fail against every firmware they
were shown for.

Sweep A93: a key the handler answers only under one `#if FEATURE_X` is a chip
the table names with its gate, (key, label, "FEATURE_X"), offered on a
product whose env resolves FEATURE_X to 1 (the env's -DFEATURE_X first, else
the config header's `#ifndef` default) and absent where it resolves to 0. A
key under any other conditional, an env or header spelling the reader does
not evaluate, and a value that is neither 0 nor 1 are refused by name. The
flagship's battery, power-policy, diagnostics and data-management chips are
on all four flagship products and its MQTT chip only on the HA build; the
gate readings run on synthetic projects as well. A directive is read on its
logical line, as the compiler reads it (a backslash splice, a block comment
closed on a later line), and an `#undef` of the macro in the config header,
or a `#define` / `#undef` of it in the handler's own file, is refused.

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import contextlib
import copy
import io
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import gen_flash as gf  # noqa: E402  (definitions only; main() runs under __main__)

CANARY = "firmware/canary"
WAP = "firmware/projects/canary-wap"
VISION = "firmware/projects/canary-vision"
SENSE = "firmware/projects/canary-sense"
DISPLAY = "firmware/projects/canary-display"

# The env each project's refusals are run against (any of its products' envs
# would do: only the flagship has gated chips, and its tests name theirs).
ENV = {CANARY: "release_ha", WAP: "arduino:XIAO_ESP32S3", VISION: "canary-vision-default",
       SENSE: "canary-sense-default", DISPLAY: "profile:watch"}

# The table every bench offered every product before A86 (flash.js MONITOR_CMDS).
OLD_TABLE = [("h", "help"), ("j", "self-manifest"), ("i", "identity"), ("s", "status"),
             ("t", "time"), ("w", "wifi"), ("m", "system"), ("b", "battery")]


class VocabCase(unittest.TestCase):
    def setUp(self):
        self.saved = copy.deepcopy(gf.MONITOR_VOCAB)
        gf._monitor_block.cache_clear()

    def tearDown(self):
        gf.MONITOR_VOCAB.clear()
        gf.MONITOR_VOCAB.update(self.saved)
        gf._monitor_block.cache_clear()

    def with_cmds(self, project, cmds, **extra):
        gf.MONITOR_VOCAB[project] = {**gf.MONITOR_VOCAB[project], "cmds": cmds, **extra}
        gf._monitor_block.cache_clear()

    def refuses(self, project, why, env=None):
        return self.refusal(project, env or ENV.get(project, "none"), why)

    def refusal(self, project, env, why=""):
        """The refusal text monitor_block() stops with: die()'s stderr line, or
        the message a SystemExit carries."""
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit, msg=why) as cm:
            gf.monitor_block(project, env)
        code = cm.exception.code
        return err.getvalue() + (code if isinstance(code, str) else "")

    def keys_of(self, project, env):
        return [c[0] for c in gf.monitor_block(project, env)["cmds"]]


class CommittedVocabulary(VocabCase):
    def test_every_firmware_project_derives(self):
        for project in gf.MONITOR_VOCAB:
            rec = gf.monitor_block(project, ENV[project])
            self.assertEqual(rec["placeholder"], gf.MONITOR_PLACEHOLDERS[rec["kind"]])
        self.assertEqual(gf.monitor_block(DISPLAY, ENV[DISPLAY])["cmds"], [])

    def test_gated_chips_follow_each_flagship_products_env(self):
        # A93: battery, power policy, diagnostics and data management default
        # to 1 in canary_config.h and no flagship env sets them, so all four
        # products offer them; only release_ha sets -DFEATURE_HA_MQTT=1 over
        # the header's 0, so only the Canary offers MQTT.
        envs = {p["id"]: p["env"] for p in gf.PRODUCTS if p["family"] == "canary"}
        self.assertEqual(sorted(envs), ["securacv-canary", "securacv-canary-esp32cam",
                                        "securacv-canary-freenove-s3", "securacv-canary-wroom"])
        committed = {p["id"]: p["monitor"] for p in
                     json.loads((gf.CANARY_LOCAL / "devices/flash.json").read_text(encoding="utf-8"))["products"]}
        for pid, env in envs.items():
            keys = self.keys_of(CANARY, env)
            self.assertTrue({"b", "p", "d", "r"} <= set(keys), f"{pid} ({env}) compiles b, p, d and r: {keys}")
            self.assertEqual("m" in keys, pid == "securacv-canary", f"{pid} ({env}): m only where -DFEATURE_HA_MQTT=1")
            self.assertEqual(committed[pid], gf.monitor_block(CANARY, env), f"flash.json's {pid} is stale")
        self.assertIn(["m", "MQTT"], committed["securacv-canary"]["cmds"])
        self.assertIn(["b", "battery"], committed["securacv-canary-wroom"]["cmds"])
        # A build that sets none of them (dev, which extends nothing that does)
        # gets the header's defaults: MQTT off.
        self.assertNotIn("m", self.keys_of(CANARY, "dev"))
        self.assertIn("m", self.keys_of(CANARY, "dev_ha"))

    def test_firmware_parsers_read_what_the_sources_say(self):
        keys, words, _, _ = gf.answered_keys(gf.REPO / gf.MONITOR_VOCAB[WAP]["handler"], "wap")
        self.assertNotIn("j", keys, "the WAP sketch has no j")
        self.assertTrue({"h", "i", "s", "t", "g", "w", "c", "m", "r", "b", "p", "u"} <= keys)
        self.assertIn("wifi", words["w"])
        keys, words, gated, refused = gf.answered_keys(gf.REPO / gf.MONITOR_VOCAB[CANARY]["handler"], "canary")
        self.assertTrue({"h", "j", "t", "w", "x"} <= keys)
        self.assertNotIn("m", keys, "MQTT status is behind #if FEATURE_HA_MQTT")
        self.assertNotIn("b", keys, "battery is behind #if FEATURE_POWER_MONITOR")
        self.assertEqual({k: gated[k] for k in "bpdrm"},
                         {"b": "FEATURE_POWER_MONITOR", "p": "FEATURE_POWER_POLICY", "d": "FEATURE_DIAGNOSTICS",
                          "r": "FEATURE_DATA_MGMT", "m": "FEATURE_HA_MQTT"})
        # n and a sit inside #if FEATURE_CONSOLE_THEME and a second #if.
        self.assertRegex(refused["n"], r"^nested under `#if FEATURE_CONSOLE_THEME` and `#if defined\(FEATURE_BLE_SCAN\)")
        self.assertRegex(refused["a"], r"^nested under `#if FEATURE_CONSOLE_THEME` and `#if FEATURE_DIAGNOSTICS`")
        self.assertIn("tests", words["t"])
        self.assertIn("tamper", words["w"])
        keys, _, _, _ = gf.answered_keys(gf.REPO / gf.MONITOR_VOCAB[VISION]["handler"], "vision")
        self.assertEqual(keys, {"j", "J", "h", "H", "?"})
        w, args, _ = gf.answered_words(gf.REPO / gf.MONITOR_VOCAB[SENSE]["handler"], "sense")
        self.assertEqual(w, {"help", "?", "cfg", "get", "set", "reset", "stream", "raw"})
        self.assertTrue({("stream", "on"), ("stream", "off"), ("raw", "on"), ("raw", "off")} <= args)


class Refusals(VocabCase):
    def test_the_old_shared_table_fails_every_firmware_but_none(self):
        for project in (CANARY, WAP, VISION, SENSE, DISPLAY):
            self.with_cmds(project, [list(c) for c in OLD_TABLE])
            self.refuses(project, f"the one-for-all table must not pass for {project}")

    def test_a_key_the_firmware_does_not_answer(self):
        self.with_cmds(WAP, [("h", "help"), ("j", "self-manifest")])
        self.refuses(WAP, "the WAP sketch answers no j")

    def test_a_key_behind_a_feature_flag(self):
        self.with_cmds(CANARY, [("h", "help"), ("m", "MQTT")])
        msg = self.refuses(CANARY, "m is answered only under #if FEATURE_HA_MQTT")
        self.assertIn("name the gate, ('m', 'MQTT', 'FEATURE_HA_MQTT')", msg)

    def test_a_gate_the_firmware_does_not_put_there(self):
        self.with_cmds(CANARY, [("h", "help"), ("m", "MQTT", "FEATURE_DIAGNOSTICS")])
        self.assertIn("answers it under `#if FEATURE_HA_MQTT`", self.refuses(CANARY, "the wrong gate"))
        self.with_cmds(CANARY, [("h", "help"), ("t", "all tests", "FEATURE_DIAGNOSTICS")])
        self.assertIn("with no #if around it — drop the gate", self.refuses(CANARY, "t has no gate"))
        self.with_cmds(CANARY, [("h", "help"), ("n", "nearby", "FEATURE_BLE_SCAN")])
        self.assertIn("only nested under", self.refuses(CANARY, "n is under two conditionals"))
        self.with_cmds(CANARY, [("h", "help"), ("m", "MQTT", "FEATURE_HA_MQTT")])
        gf.MONITOR_VOCAB[CANARY].pop("config")
        self.assertIn("names no config header", self.refuses(CANARY, "a gated chip needs its default's header"))
        self.with_cmds(CANARY, [("h", "help"), ("m", "MQTT", "FEATURE_HA_MQTT")],
                       config="firmware/projects/canary-vision/include/canary/config.h")
        self.assertIn("does not include", self.refuses(CANARY, "the header must be the handler's"))
        self.with_cmds(SENSE, [("help", "help", "FEATURE_X")])
        self.assertIn("a key console's gated key", self.refuses(SENSE, "only a key console has gates"))

    def test_a_label_that_is_not_the_firmwares_words(self):
        self.with_cmds(CANARY, [("h", "help"), ("t", "time")])
        self.refuses(CANARY, "the flagship's t runs all tests; it is not a time readout")
        self.with_cmds(CANARY, [("h", "help"), ("t", "all tests")])
        self.assertEqual(gf.monitor_block(CANARY, ENV[CANARY])["cmds"], [["h", "help"], ["t", "all tests"]])

    def test_keys_need_h_for_the_placeholder_promise(self):
        self.with_cmds(VISION, [("j", "install receipt")])
        self.refuses(VISION, "the placeholder says h shows the menu")

    def test_words_and_their_arguments(self):
        self.with_cmds(SENSE, [("stream sometimes", "radar line")])
        self.refuses(SENSE, "execute_() reads no 'sometimes' after stream")
        self.with_cmds(SENSE, [("j", "help")])
        self.refuses(SENSE, "the tuning console reads words, and j is not one")
        # "knobs" heads a help line ("[tune] knobs (name range current):") but
        # execute_() answers no such word: the help text alone is not enough.
        self.with_cmds(SENSE, [("knobs", "knobs")])
        self.refuses(SENSE, "a word the help prints but execute_() does not answer")
        self.with_cmds(SENSE, [("cfg", "radar line")])
        self.refuses(SENSE, "cfg's help line says nothing of a radar line")

    def test_none_is_held_to_a_firmware_that_reads_nothing(self):
        self.with_cmds(DISPLAY, [("h", "help")])
        self.refuses(DISPLAY, "kind none carries no commands")
        gf.MONITOR_VOCAB[WAP] = {"kind": "none", "cmds": []}
        self.refuses(WAP, "the WAP sketch reads its serial console")

    def test_a_project_with_no_entry(self):
        gf.MONITOR_VOCAB.pop(SENSE)
        self.refuses(SENSE, "a firmware with no vocabulary must be named, not defaulted")

    def test_a_word_console_the_firmware_never_feeds(self):
        # The tuning console's words are only answers if the project hands
        # its USB console to it. A project that reads Serial some other way,
        # or feeds it only inside a comment, must be refused.
        real = gf.project_source_text
        for unwired in ("void loop() { while (Serial.available()) handle(Serial.read()); }",
                        "void loop() {\n  // g_console.feed((char)Serial.read());\n}\n"):
            gf.project_source_text = lambda project, _src=unwired: _src if project == SENSE else real(project)
            try:
                gf._monitor_block.cache_clear()
                self.refuses(SENSE, f"an unwired tuning console must not pass: {unwired!r}")
            finally:
                gf.project_source_text = real
                gf._monitor_block.cache_clear()
        self.assertEqual(gf.monitor_block(SENSE, ENV[SENSE])["kind"], "words", "the committed sketch feeds it")


class Parsers(unittest.TestCase):
    """The reader sees code the way the compiler does."""

    def read(self, body: str, extra: str = ""):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "h.cpp"
            f.write_text(extra + "static void handle_serial_commands() {\n" + body + "\n}\n", encoding="utf-8")
            return gf.answered_keys(f, "synthetic")

    def handler(self, body: str, extra: str = "") -> set[str]:
        return self.read(body, extra)[0]

    def test_conditional_and_commented_keys_do_not_count(self):
        keys = self.handler(
            "  switch (c) {\n"
            "    case 'a': break;\n"
            "#if FEATURE_X\n    case 'b': break;\n#else\n    case 'c': break;\n#endif\n"
            "    // case 'd':\n"
            "    /* case 'e': */\n"
            "    case 'f': Serial.println(\"case 'g': } {\"); break;\n"
            "  }")
        self.assertEqual(keys, {"a", "f"})

    def test_each_gate_shape_is_read_or_refused_by_name(self):
        # A93: one `#if NAME` (a comment after it is not code) is a gate; every
        # other shape the compiler would answer is refused with what it is.
        _, _, gated, refused = self.read(
            "  switch (c) {\n"
            "#if FEATURE_A && FEATURE_B\n    case 'a': break;\n#endif\n"
            "#if defined(FEATURE_X)\n    case 'b': break;\n#endif\n"
            "#if FEATURE_X\n#if FEATURE_Y\n    case 'c': break;\n#endif\n#endif\n"
            "#if FEATURE_X\n    case 'd': break;\n#else\n    case 'e': break;\n#endif\n"
            "#if FEATURE_X\n    case 'f': break;\n#elif FEATURE_Y\n    case 'g': break;\n#endif\n"
            "#ifdef FEATURE_X\n    case 'h': break;\n#endif\n"
            "#if FEATURE_X // why it is gated\n    case 'i': break;\n#endif\n"
            "#if FEATURE_X /* a block comment */\n    case 'j': break;\n#endif\n"
            "#if FEATURE_X || FEATURE_Y\n    case 'k': break;\n#endif\n"
            "#if (FEATURE_X)\n    case 'l': break;\n#endif\n"
            "#if FEATURE_X\n    case 'm': break;\n#endif\n#if FEATURE_Y\n    case 'm': break;\n#endif\n"
            "#if FEATURE_X\n    case 'n': break;\n#endif\n    case 'n': break;\n"
            "  }")
        self.assertEqual(gated, {"d": "FEATURE_X", "f": "FEATURE_X", "i": "FEATURE_X", "j": "FEATURE_X"})
        self.assertEqual(refused, {
            "a": "under `#if FEATURE_A && FEATURE_B` (a compound or defined() condition)",
            "b": "under `#if defined(FEATURE_X)` (a compound or defined() condition)",
            "c": "nested under `#if FEATURE_X` and `#if FEATURE_Y`",
            "e": "in the `#else` branch of `#if FEATURE_X`",
            "g": "in the `#elif FEATURE_Y` branch of `#if FEATURE_X`",
            "h": "under `#ifdef FEATURE_X` (it asks whether a macro is defined, not its value)",
            "k": "under `#if FEATURE_X || FEATURE_Y` (a compound or defined() condition)",
            "l": "under `#if (FEATURE_X)` (not one macro name)",
            "m": "answered under more than one gate (`#if FEATURE_X`, `#if FEATURE_Y`)",
        })
        # n is answered unconditionally too, so it is a plain key.
        self.assertIn("n", self.handler("    case 'n': break;\n#if FEATURE_X\n    case 'n': break;\n#endif\n"))

    def test_a_directive_is_read_on_its_logical_line(self):
        # Review of A93: the compiler joins a backslash splice, and a block
        # comment that closes on a later line, into the directive's line
        # before it reads it (a comment is one space), so the gate is the
        # joined text, and the code a directive or a // comment runs on over
        # is not code. Each reading here is what g++ -E keeps.
        keys, _, gated, refused = self.read(
            "  switch (c) {\n"
            "#if FEATURE_X /* a note\n */ && FEATURE_Y\n    case 'a': break;\n#endif\n"
            "#if FEATURE_X /* a note\n   that ends here */\n    case 'b': break;\n#endif\n"
            "#if FEATURE_X \\\n  && FEATURE_Y\n    case 'c': break;\n#endif\n"
            "#if FEATURE_X // a note \\\n  && FEATURE_Y, still the note\n    case 'd': break;\n#endif\n"
            "#if FEATURE_X\n    case 'e': break;\n#endif /* a note\n */ case 'f': break;\n"
            "    case 'g': break; // a note \\\n    case 'h': break;\n"
            "    /* a note\n */ #if FEATURE_X\n    case 'i': break;\n#endif\n"
            "    case 'j': /* a note\n */ case 'k': break;\n"
            "  }")
        self.assertEqual(keys, {"g", "j", "k"}, "f is in #endif's line and h in the // comment: no code")
        self.assertEqual(gated, {"b": "FEATURE_X", "d": "FEATURE_X", "e": "FEATURE_X", "i": "FEATURE_X"})
        self.assertEqual(refused, {
            "a": "under `#if FEATURE_X && FEATURE_Y` (a compound or defined() condition)",
            "c": "under `#if FEATURE_X && FEATURE_Y` (a compound or defined() condition)",
        })

    def test_an_unbalanced_conditional_answers_nothing_after_it(self):
        keys, _, gated, refused = self.read("    case 'a': break;\n#endif\n    case 'b': break;\n")
        self.assertEqual((keys, gated), ({"a"}, {}))
        self.assertIn("unbalanced", refused["b"])

    def test_a_handler_inside_a_conditional_is_refused(self):
        with self.assertRaises(SystemExit):
            self.handler("  if (c == 'a') {}", extra="#if FEATURE_CONSOLE\n")

    def test_conditional_and_commented_words_do_not_count(self):
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "tuning_console.h"
            f.write_text(
                "void execute_() {\n"
                "  if (ieq_(cmd, \"help\")) {}\n"
                "  // else if (ieq_(cmd, \"zz\")) {}\n"
                "  /* else if (ieq_(cmd, \"yy\")) {} */\n"
                "  else if (ieq_(cmd, \"stream\")) { if (ieq_(tok[1], \"on\")) {} } // ieq_(tok[1], \"sometimes\")\n"
                "#if FEATURE_X\n"
                "  else if (ieq_(cmd, \"xx\")) {}\n"
                "#endif\n"
                "  Serial.println(\"ieq_(cmd, 'qq')\");\n"
                "}\n"
                "void print_help() { Serial.println(\"[tune] help - this list\"); }\n",
                encoding="utf-8")
            words, args, help_words = gf.answered_words(f, "synthetic")
        self.assertEqual(words, {"help", "stream"})
        self.assertEqual(args, {("stream", "on")})
        self.assertEqual(help_words, {"help": {"tune", "help", "this", "list"}})

    def test_raw_strings_do_not_hide_code(self):
        stripped = gf._c_strip('const char* p = R"html(<a href="//x">)html"; Serial.read();')
        self.assertIn("Serial.read()", stripped)
        self.assertNotIn("href", stripped)


SYNTH = "fw/synthetic"
SYNTH_INI = """[env]
build_flags =
    -DBOARD_HAS_PSRAM

[env:on]
build_flags =
    ${env.build_flags}
    -DFEATURE_X=1

[env:off]
build_flags =
    ${env.build_flags}
    -DFEATURE_X=0

[env:bare]
build_flags =
    ${env.build_flags}
    -DFEATURE_X

[env:plain]
build_flags =
    ${env.build_flags}

[env:inherits]
extends = env:off
"""
SYNTH_CFG = """#ifndef SYNTH_CFG_H
#define SYNTH_CFG_H
#ifndef FEATURE_X
  #define FEATURE_X 1   // on unless the env says otherwise
#endif
#endif
"""


class GatedChips(VocabCase):
    """A93 on a synthetic project: a handler with h and one gated key q, a
    config header with FEATURE_X's default, envs that set it each way."""

    def make(self, ini=SYNTH_INI, cfg=SYNTH_CFG, gate="#if FEATURE_X", include='#include "cfg.h"'):
        close = "#endif\n" * gate.count("#if")
        root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, root)
        proj = root / SYNTH
        (proj / "src").mkdir(parents=True)
        (proj / "include").mkdir()
        (proj / "platformio.ini").write_text(ini, encoding="utf-8")
        (proj / "include/cfg.h").write_text(cfg, encoding="utf-8")
        (proj / "src/main.cpp").write_text(
            include + "\n\nstatic void handle_serial_commands() {\n  switch (c) {\n"
            "    case 'h': Serial.println(\"  h - help\"); Serial.println(\"  q - quiet status\"); break;\n"
            f"{gate}\n    case 'q': break;\n{close}  }}\n}}\n", encoding="utf-8")
        saved = gf.REPO
        gf.REPO = root
        self.addCleanup(setattr, gf, "REPO", saved)
        gf.MONITOR_VOCAB[SYNTH] = {"kind": "keys", "handler": f"{SYNTH}/src/main.cpp",
                                   "config": f"{SYNTH}/include/cfg.h",
                                   "cmds": [("h", "help"), ("q", "quiet", "FEATURE_X")]}
        gf._monitor_block.cache_clear()

    def test_offered_where_the_env_sets_one_absent_at_zero(self):
        self.make()
        self.assertEqual(self.keys_of(SYNTH, "on"), ["h", "q"], "-DFEATURE_X=1 compiles q in")
        self.assertEqual(self.keys_of(SYNTH, "off"), ["h"], "-DFEATURE_X=0 compiles q out")
        self.assertEqual(self.keys_of(SYNTH, "bare"), ["h", "q"], "a bare -DFEATURE_X is 1 to the compiler")
        self.assertEqual(self.keys_of(SYNTH, "plain"), ["h", "q"], "no -D: the header's default, 1")
        self.assertEqual(self.keys_of(SYNTH, "inherits"), ["h"], "extends env:off: its flags")
        self.assertEqual(self.keys_of(SYNTH, "arduino:XIAO_ESP32S3"), ["h", "q"], "an Arduino env sets no flags")
        self.make(cfg=SYNTH_CFG.replace("FEATURE_X 1", "FEATURE_X 0"))
        self.assertEqual(self.keys_of(SYNTH, "plain"), ["h"], "the header's default, 0")
        self.assertEqual(self.keys_of(SYNTH, "on"), ["h", "q"], "the env's -D over the header's #ifndef default")
        self.make(gate="#if FEATURE_X  // the gate, with a comment the compiler skips")
        self.assertEqual(self.keys_of(SYNTH, "on"), ["h", "q"])
        self.assertEqual(self.keys_of(SYNTH, "off"), ["h"])

    def test_a_compound_or_other_gate_is_refused_by_name(self):
        for gate, says in (("#if FEATURE_X && FEATURE_Y", "`#if FEATURE_X && FEATURE_Y` (a compound or defined() condition)"),
                           ("#if defined(FEATURE_X)", "`#if defined(FEATURE_X)` (a compound or defined() condition)"),
                           ("#ifdef FEATURE_X", "`#ifdef FEATURE_X` (it asks whether a macro is defined, not its value)"),
                           ("#if FEATURE_Y\n#if FEATURE_X", "nested under `#if FEATURE_Y` and `#if FEATURE_X`"),
                           # The comment is one space: the directive is `#if FEATURE_X && 0`.
                           ("#if FEATURE_X /* a note\n */ && 0", "`#if FEATURE_X && 0` (a compound or defined() condition)")):
            self.make(gate=gate)
            msg = self.refusal(SYNTH, "on", gate)
            self.assertIn(says, msg, gate)
            self.assertIn("chip 'q' (quiet)", msg)

    def test_env_spellings_the_reader_does_not_evaluate_are_refused(self):
        for flag in ("-DFEATURE_X=0x1", "-DFEATURE_X=true", "'-DFEATURE_X=1'", "-D FEATURE_X"):
            self.make(ini=SYNTH_INI.replace("-DFEATURE_X=1", flag))
            msg = self.refusal(SYNTH, "on", flag)
            self.assertIn("names FEATURE_X in a spelling or under a key this derivation does not parse", msg, flag)
        self.make(ini=SYNTH_INI.replace("-DFEATURE_X=1", "-DFEATURE_X=2"))
        self.assertIn("is 2, neither 0 nor 1", self.refusal(SYNTH, "on"))
        self.make(ini=SYNTH_INI.replace("-DFEATURE_X=1", "-DFEATURE_X=01"))
        self.assertIn("leading-zero (octal)", self.refusal(SYNTH, "on"))
        self.make(ini=SYNTH_INI.replace("-DFEATURE_X=1", "-DFEATURE_X=1\n    -DFEATURE_X=0"))
        self.assertIn("defines FEATURE_X as [0, 1]", self.refusal(SYNTH, "on"))
        self.make(ini=SYNTH_INI.replace("[env:on]\nbuild_flags =", "[env:on]\nbuild_unflags = -DFEATURE_X=1\nbuild_flags ="))
        self.assertIn("build_unflags names FEATURE_X", self.refusal(SYNTH, "on"))

    def test_header_defaults_the_reader_cannot_place_are_refused(self):
        self.make(cfg=SYNTH_CFG.replace("FEATURE_X 1", "FEATURE_X (1)"))
        self.assertIn("is `(1)`, not a plain integer literal", self.refusal(SYNTH, "plain"))
        self.make(cfg=SYNTH_CFG.replace("FEATURE_X 1", "FEATURE_X 2"))
        self.assertIn("is 2, neither 0 nor 1", self.refusal(SYNTH, "plain"))
        # Not inside #ifndef FEATURE_X: the header's value wins over any -D,
        # so the env's reading would be wrong; refused even where the env sets it.
        self.make(cfg="#pragma once\n#define FEATURE_X 0\n")
        self.assertIn("not as the `#ifndef FEATURE_X` / `#define FEATURE_X <digits>` default", self.refusal(SYNTH, "on"))
        self.make(cfg=SYNTH_CFG.replace("#ifndef FEATURE_X\n", "#if BOARD_IS_BIG\n#ifndef FEATURE_X\n")
                  .replace("#endif\n#endif\n", "#endif\n#endif\n#endif\n"))
        self.assertIn("`#if BOARD_IS_BIG`", self.refusal(SYNTH, "plain"))
        self.make(cfg="#pragma once\n#ifndef FEATURE_X\n#else\n#define FEATURE_X 1\n#endif\n")
        self.assertIn("(its `#else` branch)", self.refusal(SYNTH, "plain"))
        self.make(cfg="#pragma once\n")
        self.assertIn("neither the env's build_flags nor", self.refusal(SYNTH, "plain"))
        self.make(cfg="#pragma once\n#if !defined(FEATURE_X)\n#define FEATURE_X 1\n#endif\n")
        self.assertEqual(self.keys_of(SYNTH, "plain"), ["h", "q"], "#if !defined(X) guards it as #ifndef does")
        self.make(include='#include "other.h"')
        self.assertIn("does not include", self.refusal(SYNTH, "on"))

    def test_what_the_preprocessor_does_after_the_default_is_refused(self):
        # Review of A93: each of these leaves the handler's `#if FEATURE_X`
        # reading 0 (or 2) where the env and the #ifndef default say 1, so the
        # chip would be offered for a build that compiles q out (g++ -E agrees).
        self.make(cfg=SYNTH_CFG.replace("#endif\n#endif\n", "#endif\n#undef FEATURE_X\n#endif\n"))
        self.assertIn("has `#undef FEATURE_X`; after it FEATURE_X is undefined", self.refusal(SYNTH, "on"))
        self.assertIn("has `#undef FEATURE_X`", self.refusal(SYNTH, "plain"))
        self.make(cfg=SYNTH_CFG.replace("FEATURE_X 1   // on unless the env says otherwise",
                                        "FEATURE_X 1   /* on unless\n */ + 1"))
        self.assertIn("defines FEATURE_X as `#define FEATURE_X 1 + 1`, a logical line that runs past",
                      self.refusal(SYNTH, "plain"))
        self.make(cfg=SYNTH_CFG.replace("#ifndef FEATURE_X\n", "#if !defined(FEATURE_X) /* a note\n */ || 1\n"))
        self.assertIn("inside `#ifndef SYNTH_CFG_H` and `#if !defined(FEATURE_X) || 1`", self.refusal(SYNTH, "on"))
        for touch in ("#undef FEATURE_X", "#define FEATURE_X 0", "#  define FEATURE_X /* a note\n */ 0"):
            self.make(include='#include "cfg.h"\n' + touch)
            msg = self.refusal(SYNTH, "on", touch)
            self.assertIn("chip 'q' names gate FEATURE_X, and fw/synthetic/src/main.cpp itself has", msg, touch)
            self.assertIn("(line 2)", msg, touch)
        # A comment that closes on a later line with nothing after it is no
        # code, so the default and the guard still read as written.
        self.make(cfg=SYNTH_CFG.replace("// on unless the env says otherwise", "/* on unless\n   the env says */")
                  .replace("#ifndef FEATURE_X\n", "#ifndef FEATURE_X /* a note\n */\n"))
        self.assertEqual(self.keys_of(SYNTH, "plain"), ["h", "q"])
        self.assertEqual(self.keys_of(SYNTH, "off"), ["h"])
        # A macro the handler file names only in a comment or a string is untouched.
        self.make(include='#include "cfg.h"\n// #undef FEATURE_X\nstatic const char* k = "#define FEATURE_X 0";')
        self.assertEqual(self.keys_of(SYNTH, "on"), ["h", "q"])


if __name__ == "__main__":
    unittest.main()
