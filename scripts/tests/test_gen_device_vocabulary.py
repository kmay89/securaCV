"""scripts/gen_device_vocabulary.py — the device-details vocabulary's
generator, pinned: the committed module is what it writes; the scanner
finds a key a device sends and not one it reads, comments on or serves in a
page; every row it cannot prove is refused by name and writes nothing; and
the class decides the redaction.

Kept here rather than in custom_components/securacv/tests on purpose: the
HACS mirror carries that directory and runs it standalone, without spec/ or
firmware/, so a parity test there would be one more deselect (sweep HA56).
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import gen_device_vocabulary as gv  # noqa: E402

GENERATOR = REPO / "scripts" / "gen_device_vocabulary.py"


class CommittedModule(unittest.TestCase):
    def test_the_module_is_what_the_vocabulary_generates(self):
        proc = subprocess.run(
            [sys.executable, "-I", str(GENERATOR), "--check"],
            capture_output=True, text=True, timeout=120, check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_every_seed_row_is_a_key_of_ledger_f391(self):
        # The seed is F391's keys that a firmware source emits; the two it
        # also named that none emits stay integration-owned in diagnostics.py.
        data = json.loads((REPO / gv.VOCABULARY).read_text(encoding="utf-8"))
        f391 = {
            "hw_token", "mdns_host", "serial_number", "chip_id", "module_id",
            "local_address", "address", "peer_address",
            "uptime", "boot_count", "sd_writes", "http_requests", "last_frame_age_ms",
        }
        keys = {row["key"] for row in data["keys"]}
        self.assertEqual(keys, f391 - {"serial_number", "chip_id"})

    def test_a_key_no_firmware_source_emits_is_refused_on_this_tree(self):
        # Why serial_number and chip_id are not rows: on this tree the check
        # finds no device source that sends either, and says so.
        data = json.loads((REPO / gv.VOCABULARY).read_text(encoding="utf-8"))
        for key in ("serial_number", "chip_id"):
            trial = json.loads(json.dumps(data))
            trial["keys"].append({"key": key, "class": "identifier", "means": "x", "surfaces": []})
            _, _, errors = gv.validate(trial, REPO)
            self.assertTrue(any(f"row {key!r}: names no surface" in e for e in errors), errors)
            self.assertTrue(
                any(f"no firmware device source emits {key!r}" in e for e in errors), errors)


class Scanner(unittest.TestCase):
    def emits(self, src, key="k"):
        return key in gv.emitted_keys(src)

    def test_an_arduinojson_write_emits(self):
        self.assertTrue(self.emits('doc["k"] = v;'))
        self.assertTrue(self.emits('doc[ "k" ]   =   v;'))
        self.assertTrue(self.emits('doc["k"].set(v);'))
        self.assertTrue(self.emits('JsonObject o = doc["k"].to<JsonObject>();'))
        self.assertTrue(self.emits('doc["k"].add(1);'))

    def test_a_key_escaped_in_a_string_literal_emits(self):
        self.assertTrue(self.emits('snprintf(b, n, "{\\"k\\":%lu}", v);'))
        self.assertTrue(self.emits('wap_json::raw(w, ",\\"k\\":");'))
        self.assertTrue(self.emits('Serial.printf("  \\"k\\": \\"%s\\",\\n", v);'))

    def test_a_read_is_not_an_emission(self):
        self.assertFalse(self.emits('const char* a = in["k"].as<const char*>();'))
        self.assertFalse(self.emits('if (!in["k"].is<JsonVariant>()) return;'))
        self.assertFalse(self.emits('if (doc["k"] == 1) return;'))
        self.assertFalse(self.emits('const char* name = "k";'))
        self.assertFalse(self.emits('puts("\\"k\\" is a key");'))

    def test_a_comment_is_not_an_emission(self):
        self.assertFalse(self.emits('// doc["k"] = v;\nint x;'))
        self.assertFalse(self.emits('/* snprintf(b, n, "\\"k\\":%d", v); */ int x;'))
        self.assertFalse(self.emits('int x; /* doc["k"] = v;'))  # never closed

    def test_a_served_page_is_not_an_emission(self):
        page = 'const char PAGE[] = R"html(<script>d["k"] = 1; x = {"k": 2, \\"k\\": 3}</script>)html";'
        self.assertFalse(self.emits(page))

    def test_a_longer_key_does_not_emit_its_suffix(self):
        self.assertFalse(self.emits('doc["local_address"] = a;', "address"))
        self.assertFalse(self.emits('"{\\"peer_address\\":\\"%s\\"}"', "address"))
        self.assertTrue(self.emits('doc["local_address"] = a;', "local_address"))

    def test_quotes_slashes_and_separators_do_not_unseat_the_scan(self):
        self.assertTrue(self.emits("char q = '\"'; doc[\"k\"] = q;"))
        self.assertTrue(self.emits("char q = '\\''; doc[\"k\"] = q;"))
        self.assertTrue(self.emits("long n = 1'000'000; doc[\"k\"] = n;"))
        self.assertTrue(self.emits('const char* u = "http://x/*"; doc["k"] = u;'))
        self.assertTrue(self.emits('const char* p = R"(a "q" // not a comment)"; doc["k"] = 1;'))
        self.assertTrue(self.emits('const char* p = R"x(a )" b)x"; doc["k"] = 1;'))

    def test_a_device_source_is_firmware_c_or_cpp_and_not_a_test(self):
        ok = gv.device_source_problem
        self.assertIsNone(ok("firmware/projects/canary-wap/arduino/canary_wap/bluetooth_api.h"))
        self.assertIsNone(ok("firmware/canary/src/main.cpp"))
        self.assertIsNone(ok("firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino"))
        for bad in (
            "firmware/tests_host/test_x.cpp",
            "firmware/common/ota/test_ota_logic.cpp",
            "firmware/tests_host/stubs/Arduino.h",
            "firmware/provisioning/create_manifest.py",
            "scripts/x.cpp",
            "./firmware/canary/src/main.cpp",
            "firmware/../firmware/canary/src/main.cpp",
            "/firmware/canary/src/main.cpp",
            "firmware/canary/src/main.cpp/",
        ):
            self.assertIsNotNone(ok(bad), bad)


def _row(key, cls="identifier", surfaces=("firmware/a/answer.cpp",), means="what it is"):
    return {"key": key, "class": cls, "means": means, "surfaces": list(surfaces)}


class Tree(unittest.TestCase):
    """The generator run over a scratch tree: a vocabulary, firmware sources, an output."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="gen_device_vocabulary_"))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.source("firmware/a/answer.cpp", 'void f(JsonDocument& doc) { doc["hw_token"] = t; }\n'
                    'int g(char* b) { return snprintf(b, 9, "{\\"uptime\\":%lu}", 1UL); }\n')
        self.source("firmware/b/other.cpp", 'void h(JsonDocument& doc) { doc["radio_id"] = r; }\n')
        self.classes = {
            "identifier": {"redacted": True, "means": "names one unit"},
            "health_counter": {"redacted": True, "means": "a count it reports"},
        }
        self.rows = [_row("hw_token"), _row("uptime", "health_counter")]

    def source(self, rel, text):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def vocabulary(self, **override):
        data = {"$comment": "scratch", "vocabulary_version": 1,
                "classes": self.classes, "keys": self.rows}
        data.update(override)
        self.source(str(gv.VOCABULARY), json.dumps(data))

    def run_gen(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = gv.main(["--root", str(self.root), *args])
        return code, out.getvalue() + err.getvalue()

    @property
    def output(self):
        return self.root / gv.OUTPUT

    def load_output(self):
        spec = importlib.util.spec_from_file_location("scratch_device_vocabulary", self.output)
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def refused(self, *needles):
        code, text = self.run_gen()
        self.assertEqual(code, 1, text)
        self.assertIn("nothing written", text)
        for needle in needles:
            self.assertIn(needle, text)
        self.assertFalse(self.output.exists(), "a refused run wrote the module")
        return text

    def test_a_proved_vocabulary_writes_a_module_that_checks_and_redacts_by_class(self):
        self.vocabulary()
        self.assertEqual(self.run_gen()[0], 0)
        code, text = self.run_gen("--check")
        self.assertEqual(code, 0, text)
        module = self.load_output()
        self.assertEqual(dict(module.DEVICE_KEY_CLASSES),
                         {"hw_token": "identifier", "uptime": "health_counter"})
        self.assertEqual(module.REDACTED_DEVICE_KEYS, {"hw_token", "uptime"})

    def test_a_row_added_and_regenerated_joins_the_redacted_keys(self):
        self.vocabulary()
        self.run_gen()
        self.rows.append(_row("radio_id", surfaces=["firmware/b/other.cpp"]))
        self.vocabulary()
        code, text = self.run_gen("--check")
        self.assertEqual(code, 1, text)  # the committed module is now stale
        self.assertIn("is stale or was edited by hand", text)
        self.assertEqual(self.run_gen()[0], 0)
        self.assertIn("radio_id", self.load_output().REDACTED_DEVICE_KEYS)

    def test_a_hand_edit_of_the_module_fails_the_check_by_name(self):
        self.vocabulary()
        self.run_gen()
        edited = self.output.read_text(encoding="utf-8").replace('    "uptime",\n', "", 1)
        self.output.write_text(edited, encoding="utf-8")
        code, text = self.run_gen("--check")
        self.assertEqual(code, 1, text)
        self.assertIn(f"{gv.OUTPUT} is stale or was edited by hand", text)
        self.assertIn('+    "uptime",', text)  # the diff shows what the edit dropped

    def test_a_missing_module_fails_the_check(self):
        self.vocabulary()
        code, text = self.run_gen("--check")
        self.assertEqual(code, 1, text)
        self.assertIn("is stale or was edited by hand", text)

    def test_a_key_no_firmware_source_emits_is_refused_by_name(self):
        self.rows.append(_row("serial_number"))
        self.vocabulary()
        self.refused("row 'serial_number': surface firmware/a/answer.cpp does not emit",
                     "no firmware device source emits 'serial_number'")

    def test_a_row_with_no_surface_is_refused_even_when_some_source_emits_it(self):
        self.rows.append(_row("radio_id", surfaces=[]))
        self.vocabulary()
        self.refused("row 'radio_id': names no surface",
                     "'radio_id' is emitted by firmware/b/other.cpp")

    def test_a_surface_that_does_not_emit_the_key_names_the_sources_that_do(self):
        self.rows.append(_row("radio_id"))
        self.vocabulary()
        self.refused("row 'radio_id': surface firmware/a/answer.cpp does not emit 'radio_id'",
                     "'radio_id' is emitted by firmware/b/other.cpp")

    def test_a_key_only_in_a_comment_or_a_read_is_refused(self):
        self.source("firmware/c/quiet.cpp", '// doc["quiet_id"] = q;\n'
                    'const char* q = in["quiet_id"].as<const char*>();\n')
        self.rows.append(_row("quiet_id", surfaces=["firmware/c/quiet.cpp"]))
        self.vocabulary()
        self.refused("no firmware device source emits 'quiet_id'")

    def test_a_host_test_or_a_missing_file_is_not_a_surface(self):
        self.source("firmware/tests_host/test_answer.cpp", 'void t(JsonDocument& d) { d["hw_token"] = 1; }\n')
        self.rows[0] = _row("hw_token", surfaces=["firmware/tests_host/test_answer.cpp",
                                                  "firmware/a/gone.cpp"])
        self.vocabulary()
        self.refused("surface firmware/tests_host/test_answer.cpp is a host test",
                     "surface firmware/a/gone.cpp does not exist")

    def test_a_malformed_vocabulary_is_refused_by_row(self):
        cases = [
            (dict(keys=self.rows + [_row("hw_token")]), "row 'hw_token': the key is listed twice"),
            (dict(keys=self.rows + [_row("Hw-Token")]), "row 'Hw-Token': a key is [a-z][a-z0-9_]*"),
            (dict(keys=self.rows + [_row("radio_id", cls="location")]),
             "row 'radio_id': class 'location' is not one the vocabulary declares"),
            (dict(keys=self.rows + [{**_row("radio_id"), "extra": 1}]),
             "row 'radio_id': fields are class, key, means, surfaces"),
            (dict(keys=self.rows + [_row("radio_id", means=" ")]), "row 'radio_id': means is empty"),
            (dict(classes={**self.classes, "shown": {"redacted": "yes", "means": "x"}}),
             "class 'shown': redacted must be true or false"),
            (dict(classes={**self.classes, "shown": {"redacted": 0, "means": "x"}}),
             "class 'shown': redacted must be true or false"),
            (dict(vocabulary_version=2), "vocabulary_version is 2"),
            (dict(vocabulary_version=True), "vocabulary_version is True"),
            (dict(keys=[]), "keys: not a non-empty list"),
            (dict(surplus=1), "unknown top-level field(s): surplus"),
        ]
        for override, needle in cases:
            with self.subTest(needle=needle):
                self.vocabulary(**override)
                self.refused(needle)

    def test_a_class_that_is_not_redacted_keeps_its_keys_out_of_the_sweep(self):
        self.classes["shown"] = {"redacted": False, "means": "a fact a dump may show"}
        self.rows.append(_row("radio_id", cls="shown", surfaces=["firmware/b/other.cpp"]))
        self.vocabulary()
        self.assertEqual(self.run_gen()[0], 0)
        module = self.load_output()
        self.assertEqual(module.DEVICE_KEY_CLASSES["radio_id"], "shown")
        self.assertNotIn("radio_id", module.REDACTED_DEVICE_KEYS)
        self.assertEqual(module.REDACTED_CLASSES, {"identifier", "health_counter"})

    def test_the_order_of_the_rows_does_not_move_the_module(self):
        self.vocabulary()
        self.run_gen()
        first = self.output.read_bytes()
        self.rows.reverse()
        self.vocabulary(classes=dict(reversed(list(self.classes.items()))))
        self.run_gen()
        self.assertEqual(self.output.read_bytes(), first)


if __name__ == "__main__":
    unittest.main()
