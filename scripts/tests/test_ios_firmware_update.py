#!/usr/bin/env python3
"""The iPhone's software-update screen, held to the firmware it talks to.

`ios/Sources/SecuraCV/Model/FirmwareUpdate.swift` reads `GET /api/ota/status`
and the answers to `POST /api/ota/check` / `install` from two firmware
families: every display (`canary-display/src/net/glass_web.cpp`) and the WAP
(`canary-wap/arduino/canary_wap/canary_wap.ino`). Both print the status from
the shared engine (`firmware/common/ota/src/securacv_ota.cpp`). There is no
Swift toolchain on the Linux runners, so this reads the sources and holds
the app's half of the contract to theirs:

  * every JSON key the Swift decoder reads is one a firmware prints, and the
    core keys are printed by BOTH families (a rename on either side would
    otherwise decode to defaults: "not checked", forever);
  * the state words the Swift phase mapping switches on are exactly the
    engine's `securacv_ota_state_str` words, and "No error" is its word for
    no error;
  * the two "busy" dialects the Swift kick reader accepts are the two the
    firmware sends: the display's 409 and the WAP's 400 "ota_busy";
  * the device screen mounts the section for both transports.

It says what the sources say, not that the Swift compiles;
`ios-selfheal.yml` compiles it and FirmwareUpdateTests runs the same bodies.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SWIFT = ROOT / "ios" / "Sources" / "SecuraCV" / "Model" / "FirmwareUpdate.swift"
DETAIL = ROOT / "ios" / "Sources" / "SecuraCV" / "Views" / "DeviceDetailView.swift"
GLASS = ROOT / "firmware" / "projects" / "canary-display" / "src" / "net" / "glass_web.cpp"
WAP = ROOT / "firmware" / "projects" / "canary-wap" / "arduino" / "canary_wap" / "canary_wap.ino"
ENGINE = ROOT / "firmware" / "common" / "ota" / "src" / "securacv_ota.cpp"

CORE_KEYS = {
    "installed_version", "latest_version", "state", "state_text", "progress",
    "error", "error_text", "update_available", "auto_update",
}


def _function(src: str, name: str) -> str:
    """The body of a C/C++ function named `name` (first match), by braces."""
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^)]*\)\s*\{", src)
    if not m:
        raise AssertionError(f"no function {name}")
    depth, i = 0, m.end() - 1
    while i < len(src):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[m.end():i]
        i += 1
    raise AssertionError(f"unbalanced {name}")


def _swift_keys(swift: str) -> set[str]:
    block = re.search(r"enum CodingKeys: String, CodingKey \{(.*?)\n    \}", swift, re.S)
    if not block:
        raise AssertionError("FirmwareStatus has no CodingKeys")
    body = block.group(1)
    keys = set(re.findall(r'=\s*"([a-z_]+)"', body))
    # Cases without a raw value use their own name as the key.
    for line in body.splitlines():
        line = line.strip()
        if line.startswith("case ") and "=" not in line:
            keys.add(line[len("case "):].strip())
    return keys


class FirmwareUpdateContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.swift = SWIFT.read_text(encoding="utf-8")
        cls.glass_status = _function(GLASS.read_text(encoding="utf-8"), "handle_ota_status")
        wap = WAP.read_text(encoding="utf-8")
        cls.wap = wap
        cls.wap_status = _function(wap, "handle_ota_status")
        cls.engine = ENGINE.read_text(encoding="utf-8")

    def test_every_decoded_key_is_printed_by_a_firmware(self) -> None:
        keys = _swift_keys(self.swift)
        self.assertTrue(CORE_KEYS <= keys, f"the decoder reads every core key: {CORE_KEYS - keys}")
        for key in keys:
            in_glass = f'\\"{key}\\"' in self.glass_status
            in_wap = f'doc["{key}"]' in self.wap_status
            self.assertTrue(in_glass or in_wap, f"{key} is printed by a firmware")

    def test_core_keys_are_printed_by_both_families(self) -> None:
        for key in sorted(CORE_KEYS):
            self.assertIn(f'\\"{key}\\"', self.glass_status, f"display prints {key}")
            self.assertIn(f'doc["{key}"]', self.wap_status, f"WAP prints {key}")

    def test_state_words_are_the_engines(self) -> None:
        state_fn = _function(self.engine, "securacv_ota_state_str")
        engine_words = set(re.findall(r'return "([A-Za-z]+)";', state_fn)) - {"Unknown"}
        steps = dict(re.findall(r'case (\w+) = "([A-Za-z]+)"', self.swift))
        swift_words = set(steps.values()) | set(re.findall(r'case "([A-Za-z]+)":', self.swift))
        swift_words |= {"Idle"}  # the default branch; also the decoder's default
        self.assertIn('state: String = "Idle"', self.swift)
        self.assertEqual(swift_words, engine_words,
                         "the Swift phase mapping covers exactly the engine's state words")
        # Both families print the engine's word, not a private spelling.
        self.assertIn("securacv_ota_state_str", self.wap_status)
        self.assertIn("s.state", self.glass_status)
        mgr = (ROOT / "firmware" / "projects" / "canary-display" / "src" / "net" / "ota_mgr.cpp").read_text(encoding="utf-8")
        self.assertIn("s.state = securacv_ota_state_str(st);", mgr)

    def test_no_error_word_is_the_engines(self) -> None:
        err_fn = _function(self.engine, "securacv_ota_error_str")
        m = re.search(r'SECURACV_OTA_ERR_NONE:\s*return "([^"]+)";', err_fn)
        self.assertIsNotNone(m)
        self.assertIn(f'static let noErrorWord = "{m.group(1)}"', self.swift)

    def test_both_busy_dialects_are_read_as_busy(self) -> None:
        glass_src = GLASS.read_text(encoding="utf-8")
        self.assertRegex(glass_src, r'send\(409, "application/json",\s*"\{\\"ok\\":false,\\"error\\":\\"ota_busy\\"\}"\)',
                         "the display answers busy with 409")
        self.assertIn('http_send_error(req, 400, "ota_busy")', self.wap, "the WAP answers busy with 400")
        self.assertIn("case 409:", self.swift)
        self.assertIn('case 400 where reason == "ota_busy":', self.swift)

    def test_the_device_screen_mounts_both_transports(self) -> None:
        detail = DETAIL.read_text(encoding="utf-8")
        self.assertIn("FirmwareUpdateSection(transport: GlassFirmwareTransport(base: base)", detail)
        self.assertIn("FirmwareUpdateSection(transport: WapFirmwareTransport(api: api)", detail)


if __name__ == "__main__":
    unittest.main()
