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

    python3 -m unittest discover -s canary-local/tools/tests -p 'test_*.py'
"""
from __future__ import annotations

import copy
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

    def refuses(self, project, why):
        with self.assertRaises(SystemExit, msg=why):
            gf.monitor_block(project)


class CommittedVocabulary(VocabCase):
    def test_every_firmware_project_derives(self):
        for project in gf.MONITOR_VOCAB:
            rec = gf.monitor_block(project)
            self.assertEqual(rec["placeholder"], gf.MONITOR_PLACEHOLDERS[rec["kind"]])
        self.assertEqual(gf.monitor_block(DISPLAY)["cmds"], [])

    def test_firmware_parsers_read_what_the_sources_say(self):
        keys, words = gf.answered_keys(gf.REPO / gf.MONITOR_VOCAB[WAP]["handler"], "wap")
        self.assertNotIn("j", keys, "the WAP sketch has no j")
        self.assertTrue({"h", "i", "s", "t", "g", "w", "c", "m", "r", "b", "p", "u"} <= keys)
        self.assertIn("wifi", words["w"])
        keys, words = gf.answered_keys(gf.REPO / gf.MONITOR_VOCAB[CANARY]["handler"], "canary")
        self.assertTrue({"h", "j", "t", "w", "x"} <= keys)
        self.assertNotIn("m", keys, "MQTT status is behind #if FEATURE_HA_MQTT")
        self.assertNotIn("b", keys, "battery is behind #if FEATURE_POWER_MONITOR")
        self.assertIn("tests", words["t"])
        self.assertIn("tamper", words["w"])
        keys, _ = gf.answered_keys(gf.REPO / gf.MONITOR_VOCAB[VISION]["handler"], "vision")
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
        self.refuses(CANARY, "m is answered only under #if FEATURE_HA_MQTT")

    def test_a_label_that_is_not_the_firmwares_words(self):
        self.with_cmds(CANARY, [("h", "help"), ("t", "time")])
        self.refuses(CANARY, "the flagship's t runs all tests; it is not a time readout")
        self.with_cmds(CANARY, [("h", "help"), ("t", "all tests")])
        self.assertEqual(gf.monitor_block(CANARY)["cmds"], [["h", "help"], ["t", "all tests"]])

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
        self.assertEqual(gf.monitor_block(SENSE)["kind"], "words", "the committed sketch feeds it")


class Parsers(unittest.TestCase):
    """The reader sees code the way the compiler does."""

    def handler(self, body: str, extra: str = "") -> set[str]:
        with tempfile.TemporaryDirectory() as d:
            f = Path(d) / "h.cpp"
            f.write_text(extra + "static void handle_serial_commands() {\n" + body + "\n}\n", encoding="utf-8")
            return gf.answered_keys(f, "synthetic")[0]

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


if __name__ == "__main__":
    unittest.main()
