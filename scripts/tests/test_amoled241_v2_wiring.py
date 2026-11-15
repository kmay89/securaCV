#!/usr/bin/env python3
"""The Glance AMOLED's glass, held to Waveshare's V2 board.

On the Waveshare ESP32-S3-Touch-AMOLED-2.41 V2 the RM690B0's reset is not a
GPIO: it is TCA9554 EXIO0, with a 10 kOhm pull-DOWN to ground (R10 on the
V2.0 schematic). The expander powers up with every line an input, so until
firmware makes EXIO0 an output and drives it high the panel sits in reset.
Firmware 2.4.18 followed the CircuitPython map of an earlier revision (reset
on GPIO21, touch reset on GPIO3) and left the expander all inputs: every
serial line said the panel was up, and the glass never lit. QSPI writes
carry no acknowledgement, so nothing on the device can notice.

There is no ESP32 toolchain on the Linux runners, so this reads the sources
and holds them to the vendor's V2 tree (github.com/waveshareteam/
ESP32-S3-Touch-AMOLED-2.41-V2: 04_schematic and the three display examples):

  * pins.h names no reset GPIO, puts the resets on EXIO0/EXIO1, and keeps
    GPIO21 (the panel's TE output) and GPIO3 (touch INT) as inputs;
  * the HAL makes EXIO0 an output and releases it BEFORE the panel's
    begin(), and drives neither GPIO21 nor GPIO3;
  * the init table is the vendor's V2 command sequence, in order, without
    the LilyGO T4-S3's SWIRE power writes (0x5A/0x5B).

It says what the source says, not that the panel lights; the hardware test
report that promotes the board says that.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PINS = ROOT / "firmware" / "boards" / "waveshare-esp32s3-amoled241" / "pins" / "pins.h"
HAL = ROOT / "firmware" / "projects" / "canary-display" / "src" / "hal" / "display_amoled241.cpp"

# Waveshare's V2 lcd_init_cmds[] (portrait), command bytes in order. The
# vendor ends with 0x51=0xFF; the HAL leaves brightness at 0 until the splash.
VENDOR_V2_INIT = [0xFE, 0x26, 0x24, 0xFE, 0x3A, 0xC2, 0x35, 0x51, 0x11, 0x2A, 0x2B, 0x29]


def _defines(src: str) -> dict[str, str]:
    return dict(re.findall(r"^#define\s+(\w+)\s+(\S+)", src, re.M))


def _init_commands(hal: str) -> list[int]:
    m = re.search(r"waveshare241_init_operations\[\]\s*=\s*\{(.*?)\n\};", hal, re.S)
    if not m:
        raise AssertionError("display_amoled241.cpp has no waveshare241_init_operations table")
    body = re.sub(r"//[^\n]*", "", m.group(1))
    return [int(c, 16) for c in re.findall(r"WRITE_(?:C8_D8|COMMAND_8|C8_BYTES),\s*(0x[0-9A-Fa-f]{2})", body)]


class Amoled241V2WiringTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.pins = _defines(PINS.read_text(encoding="utf-8"))
        cls.hal = HAL.read_text(encoding="utf-8")

    def test_the_resets_ride_the_expander(self) -> None:
        p = self.pins
        self.assertEqual(p["LCD_PIN_RST"], "-1", "V2 has no panel-reset GPIO")
        self.assertEqual(p["TOUCH_PIN_RST"], "-1", "V2 has no touch-reset GPIO")
        self.assertEqual(p["EXIO_LCD_RESET"], "0", "OLED_RESET is TCA9554 EXIO0")
        self.assertEqual(p["EXIO_TOUCH_RESET"], "1", "TP_RESET is TCA9554 EXIO1")
        self.assertEqual(p["EXIO_I2C_ADDR"], "0x20")

    def test_the_old_reset_pins_are_inputs_now(self) -> None:
        self.assertEqual(self.pins["LCD_PIN_TE"], "21", "GPIO21 is the panel's TE output")
        self.assertEqual(self.pins["TOUCH_PIN_INT"], "3", "GPIO3 is the touch INT")
        for name in ("LCD_PIN_TE", "TOUCH_PIN_INT", "TOUCH_PIN_RST"):
            self.assertNotRegex(self.hal, r"(pinMode|digitalWrite)\(\s*" + name,
                                f"the HAL never drives {name}")
        self.assertNotRegex(self.hal, r"(pinMode|digitalWrite)\(\s*(21|3)\s*,",
                            "the HAL never drives GPIO21 or GPIO3 by number")

    def test_the_panel_leaves_reset_before_its_first_command(self) -> None:
        fn = re.search(r"PanelReset panel_reset_via_expander\(\)\s*\{(.*?)\n\}", self.hal, re.S)
        self.assertIsNotNone(fn, "the HAL has the expander reset")
        body = fn.group(1)
        self.assertIn("tca_write(TCA_REG_CONFIG, (uint8_t)~kResetLines)", body,
                      "EXIO0/EXIO1 become outputs, every other line stays an input")
        self.assertIn("tca_write(TCA_REG_OUTPUT, kResetLines)", body, "and are driven high")
        self.assertIn("1u << EXIO_LCD_RESET", self.hal)
        init = self.hal.index("bool display_init()")
        call = self.hal.index("panel_reset_via_expander()", init)
        begin = self.hal.index("s_panel->begin(", init)
        self.assertLess(call, begin, "the reset is released before the panel's begin()")
        self.assertIn("LCD_PIN_RST", self.hal[init:begin], "the panel is built with the -1 reset pin")

    def test_the_init_is_the_vendors_v2_sequence(self) -> None:
        cmds = _init_commands(self.hal)
        self.assertEqual(cmds, VENDOR_V2_INIT)
        self.assertNotIn(0x5A, cmds, "no T4-S3 SWIRE write")
        self.assertNotIn(0x5B, cmds, "no T4-S3 SWIRE write")

    def test_the_parser_reads_a_table(self) -> None:
        sample = """static const uint8_t waveshare241_init_operations[] = {
    BEGIN_WRITE,
    WRITE_C8_D8, 0xFE, 0x20,  // page
    WRITE_COMMAND_8, 0x11,    // WRITE_C8_D8, 0x99 in a comment does not count
    WRITE_C8_BYTES, 0x2A, 4, 0, 16, 1, 209,
};"""
        self.assertEqual(_init_commands(sample), [0xFE, 0x11, 0x2A])


if __name__ == "__main__":
    unittest.main()
