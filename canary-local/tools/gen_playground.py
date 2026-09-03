#!/usr/bin/env python3
"""Generate canary-local/devices/playground.json — the Waveshare 4.3B
peripheral Playground's fact sheet — from the firmware, so it cannot drift.

Honesty contract (CI drift-gates the output of this script against the
committed file, exactly like gen_senselab.py / gen_wap.py):

  * Board id/name/vendor/mcu, the CH422G command addresses, the isolated
    DI/DO expander bits, and the I2C pins are PARSED from
    firmware/boards/waveshare-esp32s3-lcd43b/pins/pins.h and injected into
    the terminal map + the per-station bring-up code. Change a pin in the
    firmware and this file changes → the drift gate fails until it's re-run.
  * The firmware version is parsed from the canary-display version.h.
  * The station wiring instructions are carried VERBATIM from the playground
    UI (firmware/projects/canary-display/.../playground_ui.cpp); the code
    snippets are line-for-line ports of that project's playground.cpp
    drivers. tests/playground.test.js re-checks the prose against the
    firmware source and the PG1 grammar against playground-sim.js.

The board body is a schematic procedural stand-in sized from Waveshare's
published outline (NOT vendor CAD) — the same honesty split wiring.json
uses for peripherals: only the terminal endpoints are claims, the routing
is staged for legibility.

Run:  python3 canary-local/tools/gen_playground.py
"""

import json
import re
import sys
from pathlib import Path

from _tooling import repo_root

ROOT = repo_root()
PINS_H = ROOT / "firmware/boards/waveshare-esp32s3-lcd43b/pins/pins.h"
VERSION_H = ROOT / "firmware/projects/canary-display/arduino/canary_display/version.h"
OUT = ROOT / "canary-local/devices/playground.json"


def parse_defines(path: Path) -> dict:
    """Pull `#define NAME VALUE` pairs (value = first token after the name)."""
    text = path.read_text()
    out = {}
    for m in re.finditer(r"^#define\s+(\w+)\s+(.+?)\s*(?://.*)?$", text, re.M):
        out[m.group(1)] = m.group(2).strip()
    return out


def need(defs: dict, key: str) -> str:
    if key not in defs:
        sys.exit(f"gen_playground: {key} missing from pins.h — firmware moved it?")
    return defs[key]


def dequote(v: str) -> str:
    return v.strip().strip('"')


def main() -> None:
    pins = parse_defines(PINS_H)
    ver = parse_defines(VERSION_H)

    board_id = dequote(need(pins, "BOARD_ID"))
    board_name = dequote(need(pins, "BOARD_NAME"))
    board_vendor = dequote(need(pins, "BOARD_VENDOR"))
    board_mcu = dequote(need(pins, "BOARD_MCU"))
    fw_version = dequote(ver.get("CANARY_FW_VERSION", '"?"'))

    sda = need(pins, "I2C_PIN_SDA")
    scl = need(pins, "I2C_PIN_SCL")
    addr_oc = need(pins, "CH422G_ADDR_OC")   # WR_OC — DO0/DO1 open-drain latch
    addr_in = need(pins, "CH422G_ADDR_IN")   # RD_IO — DI0/DI1 input read
    bit_di0 = need(pins, "ISO_IN_BIT_DI0")
    bit_di1 = need(pins, "ISO_IN_BIT_DI1")
    bit_do0 = need(pins, "ISO_OUT_BIT_DO0")
    bit_do1 = need(pins, "ISO_OUT_BIT_DO1")

    # ── Wire net colors (RGB float triples), reused by the 3D renderer ──
    colors = {
        "5v": [0.85, 0.2, 0.16],     # supply + / VIN / DI COM
        "gnd": [0.12, 0.12, 0.14],   # ground
        "di": [0.98, 0.83, 0.3],     # isolated input signal
        "do": [0.62, 0.45, 0.85],    # isolated open-drain output
        "sda": [0.35, 0.68, 0.4],    # I2C data
        "scl": [0.32, 0.5, 0.85],    # I2C clock
        "vout": [0.95, 0.55, 0.2],   # I2C header VOUT (3V3/5V)
        "rs485": [0.55, 0.55, 0.62],  # RS485 A/B
        "can": [0.45, 0.72, 0.78],   # CAN H/L
    }

    # ── Terminal map — the REAL 4.3B-BOX rear connector.
    #
    # Transcribed from the enclosure's back silkscreen (see the product photo /
    # Waveshare wiki): ONE 16-way pluggable green terminal block along the top
    # edge of the BACK face, in this fixed left→right order, with the group
    # legend printed under it:
    #
    #   [ Isolated I/O            ][RS485][ CAN ][   I2C    ][   6-36V   ]
    #     DI1 DI0 GND DI-COM DO1 DO0  A  B   H  L   SCL SDA GND  VOUT GND VIN
    #
    # Frame: the enclosure lies with its BIG faces in the X–Z plane, +Y up.
    # The back (wiring) face is +Y; the connector sits near the +Z edge and the
    # terminals point up (+Y). Positions are mm — only that a wire lands on this
    # named terminal is a claim; the row geometry is staged for legibility.
    row_y = 13.0     # terminal tops, just above the +Y (back) face
    row_z = 30.0     # near the +Z edge of the enclosure
    x0, dx = -56.25, 7.5   # 16 terminals, centered, 7.5 mm pitch
    # (id, label, net, group, extra)
    ORDER = [
        ("DI1", "DI1", "di", "iso", {"exio": "EXIO5", "bit": bit_di1,
            "blurb": "isolated digital input 1 (optocoupled, read via CH422G)"}),
        ("DI0", "DI0", "di", "iso", {"exio": "EXIO0", "bit": bit_di0,
            "blurb": "isolated digital input 0 (optocoupled, read via CH422G)"}),
        ("GND_ISO", "GND", "gnd", "iso", {"blurb": "isolated-side ground (DO load return)"}),
        ("DI_COM", "DI COM", "5v", "iso", {"blurb": "isolated-input common — external 5–36 V supply +"}),
        ("DO1", "DO1", "do", "iso", {"od": "OD1", "bit": bit_do1,
            "blurb": "isolated open-drain output 1 (≤450 mA sink)"}),
        ("DO0", "DO0", "do", "iso", {"od": "OD0", "bit": bit_do0,
            "blurb": "isolated open-drain output 0 (≤450 mA sink)"}),
        ("RS485_A", "A", "rs485", "rs485", {"blurb": "RS485 A (GPIO44/43 — shares the USB-UART console)"}),
        ("RS485_B", "B", "rs485", "rs485", {"blurb": "RS485 B"}),
        ("CAN_H", "H", "can", "can", {"blurb": "CAN High (dedicated transceiver, GPIO15/16)"}),
        ("CAN_L", "L", "can", "can", {"blurb": "CAN Low"}),
        ("SCL", "SCL", "scl", "i2c", {"gpio": int(scl), "blurb": f"I2C clock (GPIO{scl}) — shared with GT911 + CH422G"}),
        ("SDA", "SDA", "sda", "i2c", {"gpio": int(sda), "blurb": f"I2C data (GPIO{sda}) — shared with GT911 + CH422G"}),
        ("GND_I2C", "GND", "gnd", "i2c", {"blurb": "I2C header ground"}),
        ("VOUT", "VOUT", "vout", "pwr", {"blurb": "sensor-header supply out (3V3/5V, per your sensor)"}),
        ("GND_PWR", "GND", "gnd", "pwr", {"blurb": "power ground"}),
        ("VIN", "VIN", "5v", "pwr", {"blurb": "wide-input supply + (6-36V)"}),
    ]
    terminals = {}
    for i, (tid, label, net, group, extra) in enumerate(ORDER):
        terminals[tid] = {"label": label, "net": net, "group": group,
                          "pos": [round(x0 + i * dx, 2), row_y, row_z], **extra}

    # Silkscreen legend groups printed under the connector (left→right).
    groups = [
        {"id": "iso", "label": "Isolated I/O", "members": ["DI1", "DI0", "GND_ISO", "DI_COM", "DO1", "DO0"]},
        {"id": "rs485", "label": "RS485", "members": ["RS485_A", "RS485_B"]},
        {"id": "can", "label": "CAN", "members": ["CAN_H", "CAN_L"]},
        {"id": "i2c", "label": "I2C", "members": ["SCL", "SDA", "GND_I2C"]},
        {"id": "pwr", "label": "6-36V", "members": ["VOUT", "GND_PWR", "VIN"]},
    ]

    # ── Bring-up code snippets — ports of playground.cpp, with the pin facts
    # injected from pins.h (so the code can never claim a stale address/bit).
    def di_code(chan):
        t = terminals[chan]
        return (
            f"// Waveshare 4.3B — {chan} isolated input ({t['exio']}) bring-up.\n"
            f"// Field side energized -> optocoupler pulls the expander bit LOW.\n"
            f"#include <Wire.h>\n"
            f"#define CH422G_RD_IO {addr_in}      // input read (EXIO0..7)\n"
            f"#define ISO_IN_{chan}   {t['bit']}  // {t['exio']}\n\n"
            f"bool {chan.lower()}_active() {{\n"
            f"  Wire.requestFrom(CH422G_RD_IO, 1);\n"
            f"  uint8_t bits = Wire.read();\n"
            f"  return (bits & ISO_IN_{chan}) == 0;  // active-LOW when energized\n"
            f"}}"
        )

    def do_code(chan):
        t = terminals[chan]
        return (
            f"// Waveshare 4.3B — {chan} isolated open-drain output ({t['od']}).\n"
            f"// Bit LOW conducts (sinks the load); HIGH releases it. VERIFY polarity.\n"
            f"#include <Wire.h>\n"
            f"#define CH422G_WR_OC {addr_oc}       // open-drain latch (OD0..3)\n"
            f"#define ISO_OUT_{chan}  {t['bit']}   // {t['od']}\n\n"
            f"void {chan.lower()}_pulse_ms(uint16_t ms) {{\n"
            f"  Wire.beginTransmission(CH422G_WR_OC);\n"
            f"  Wire.write((uint8_t)~ISO_OUT_{chan} & 0x0F);  // sink {chan}\n"
            f"  Wire.endTransmission();\n"
            f"  delay(ms);                                    // bounded pulse\n"
            f"  Wire.beginTransmission(CH422G_WR_OC);\n"
            f"  Wire.write(0x0F);                             // release all\n"
            f"  Wire.endTransmission();\n"
            f"}}"
        )

    light_code = (
        "// VEML7700 ambient light (I2C 0x10) on the sensor header.\n"
        "#include <Wire.h>\n"
        "#define VEML7700 0x10\n\n"
        "void light_init() {                 // gain x1, 100 ms, enabled\n"
        "  Wire.beginTransmission(VEML7700);\n"
        "  Wire.write(0x00); Wire.write(0x00); Wire.write(0x00);\n"
        "  Wire.endTransmission();\n"
        "}\n"
        "float light_lux() {\n"
        "  Wire.beginTransmission(VEML7700); Wire.write(0x04);\n"
        "  Wire.endTransmission(false);\n"
        "  Wire.requestFrom(VEML7700, 2);\n"
        "  uint16_t raw = Wire.read() | (Wire.read() << 8);\n"
        "  return raw * 0.0576f;              // gain x1 @ 100 ms\n"
        "}"
    )

    tof_code = (
        "// VL53L0X time-of-flight (I2C 0x29). Continuous ranging, mm.\n"
        "// Full init in playground.cpp tof_init(); the read is:\n"
        "#include <Wire.h>\n"
        "#define VL53L0X 0x29\n\n"
        "bool tof_read_mm(uint16_t* mm) {\n"
        "  uint8_t status = 0;\n"
        "  Wire.beginTransmission(VL53L0X); Wire.write(0x13);\n"
        "  Wire.endTransmission(false); Wire.requestFrom(VL53L0X, 1);\n"
        "  status = Wire.read();\n"
        "  if (!(status & 0x07)) return false;         // no new sample\n"
        "  uint8_t r[2];\n"
        "  Wire.beginTransmission(VL53L0X); Wire.write(0x1E);\n"
        "  Wire.endTransmission(false); Wire.requestFrom(VL53L0X, 2);\n"
        "  r[0] = Wire.read(); r[1] = Wire.read();\n"
        "  *mm = (r[0] << 8) | r[1];\n"
        "  Wire.beginTransmission(VL53L0X); Wire.write(0x0B); Wire.write(0x01);\n"
        "  Wire.endTransmission();                      // clear interrupt\n"
        "  return true;\n"
        "}"
    )

    pad_code = (
        "// MPR121 12-electrode cap-touch (I2C 0x5A). Read the touch bitmap.\n"
        "#include <Wire.h>\n"
        "#define MPR121 0x5A\n\n"
        "uint16_t pad_touched() {\n"
        "  uint8_t b[2];\n"
        "  Wire.beginTransmission(MPR121); Wire.write(0x00);\n"
        "  Wire.endTransmission(false); Wire.requestFrom(MPR121, 2);\n"
        "  b[0] = Wire.read(); b[1] = Wire.read();\n"
        "  return (b[0] | (b[1] << 8)) & 0x0FFF;   // electrodes 0..11\n"
        "}"
    )

    rs485_code = (
        "// RS485 · Modbus RTU probe — read holding register 0 of slave 1.\n"
        "// Uses the pure core canary/io/modbus_rtu.h (CRC + framing).\n"
        "#include <Arduino.h>\n"
        "#include \"canary/io/modbus_rtu.h\"\n"
        "namespace mb = canary::io::modbus;\n\n"
        "void rs485_probe() {\n"
        "  Serial1.begin(9600, SERIAL_8N1, RS485_PIN_RX, RS485_PIN_TX);\n"
        "  uint8_t req[8];\n"
        "  size_t n = mb::build_read_holding(1, 0, 1, req, sizeof(req));\n"
        "  Serial1.write(req, n); Serial1.flush();          // A/B, auto-dir\n"
        "  uint8_t resp[16]; size_t got = 0;\n"
        "  uint32_t deadline = millis() + 300;\n"
        "  while (got < 7 && (int32_t)(deadline - millis()) > 0)\n"
        "    while (Serial1.available()) resp[got++] = Serial1.read();\n"
        "  uint16_t reg[1];\n"
        "  if (mb::parse_registers(resp, got, 1, mb::FN_READ_HOLDING, reg, 1) == 1)\n"
        "    Serial.printf(\"slave 1 reg0 = %u\\n\", reg[0]);\n"
        "}"
    )

    can_code = (
        "// CAN · TWAI — send one test frame; received frames are drained.\n"
        "// Uses the pure core canary/io/can_frame.h + ESP-IDF TWAI.\n"
        "#include <driver/twai.h>\n\n"
        "void can_begin() {\n"
        "  auto g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_PIN_TX,\n"
        "                                       (gpio_num_t)CAN_PIN_RX, TWAI_MODE_NORMAL);\n"
        "  auto t = TWAI_TIMING_CONFIG_500KBITS();\n"
        "  auto f = TWAI_FILTER_CONFIG_ACCEPT_ALL();\n"
        "  twai_driver_install(&g, &t, &f); twai_start();   // H/L, 500 kbit/s\n"
        "}\n"
        "void can_send() {\n"
        "  twai_message_t m = {};\n"
        "  m.identifier = 0x100; m.data_length_code = 8;\n"
        "  for (int i = 0; i < 8; i++) m.data[i] = 0xC0 + i;\n"
        "  twai_transmit(&m, pdMS_TO_TICKS(50));\n"
        "}"
    )

    census_code = (
        "// I2C census — scan the shared 8/9 bus (playground.cpp census()).\n"
        "#include <Wire.h>\n\n"
        "void i2c_census() {\n"
        "  for (uint8_t a = 0x08; a <= 0x77; a++) {\n"
        "    Wire.beginTransmission(a);\n"
        "    if (Wire.endTransmission() == 0) Serial.printf(\"  0x%02X\\n\", a);\n"
        "  }\n"
        "}"
    )

    # ── Stations — order matches the firmware META[] table. Instructions are
    # verbatim from playground_ui.cpp (the test greps that file for them).
    stations = [
        {
            "id": "doorbell", "title": "Doorbell", "where": "DI0",
            "signal": "di0", "dir": "in", "kind": "di",
            "peripheral": {"name": "Doorbell button", "part": "button",
                           "blurb": "Dry contact (wet/NPN/PNP also OK, 5–36 V) — optocoupled, never touches the S3."},
            "port": {"label": "Input channel", "choices": ["DI0", "DI1"], "default": "DI0"},
            "wires": [["DI_COM", "5v"], ["DI0", "di"]],
            "instructions": (
                "Wire (supply OFF first):\n"
                "1. External 5-24 V DC supply \"+\" -> DI COM.\n"
                "2. Doorbell button between supply \"-\" and DI0\n"
                "   (dry contact; wet/NPN/PNP also OK, 5-36 V).\n"
                "3. Power the supply, press the button.\n"
                "Presses count here and pulse DO0 (the \"ding\" link).\n"
                "Safe: DI is optocoupled - it never touches the S3."),
            "code": {"DI0": di_code("DI0"), "DI1": di_code("DI1")},
            "stimulus": [{"id": "press", "label": "Press", "action": "di:on"},
                         {"id": "release", "label": "Release", "action": "di:off"}],
            "expect": ["state=active count=", "state=clear held_ms="],
        },
        {
            "id": "intrusion", "title": "Intrusion", "where": "DI1",
            "signal": "di1", "dir": "in", "kind": "di",
            "peripheral": {"name": "PIR / reed / beam-break", "part": "reed",
                           "blurb": "PIR, reed contact, or laser break-beam receiver — relay or open-collector output."},
            "port": {"label": "Input channel", "choices": ["DI1", "DI0"], "default": "DI1"},
            "wires": [["DI_COM", "5v"], ["DI1", "di"]],
            "instructions": (
                "PIR / reed contact / laser break-beam receiver:\n"
                "1. Supply \"+\" -> DI COM (shared with DI0).\n"
                "2. Sensor output (relay or open-collector) between\n"
                "   supply \"-\" and DI1.\n"
                "3. For a beam-gap sensor: aim emitter at receiver;\n"
                "   breaking the beam trips DI1 - the held-ms readout\n"
                "   is your gap timing."),
            "code": {"DI1": di_code("DI1"), "DI0": di_code("DI0")},
            "stimulus": [{"id": "trip", "label": "Trip (magnet / motion)", "action": "di:on"},
                         {"id": "clear", "label": "Clear", "action": "di:off"}],
            "expect": ["state=active count=", "state=clear held_ms="],
        },
        {
            "id": "chime", "title": "Chime out", "where": "DO0",
            "signal": "do0", "dir": "out", "kind": "do",
            "peripheral": {"name": "Chime / buzzer", "part": "piezo",
                           "blurb": "Chime, LED, or relay coil (add a flyback diode across coils). Driven, never sensed."},
            "port": {"label": "Output channel", "choices": ["DO0", "DO1"], "default": "DO0"},
            "wires": [["DO0", "do"], ["GND_ISO", "gnd"]],
            "instructions": (
                "Isolated open-drain output (max 450 mA sink):\n"
                "1. External supply \"+\" -> load \"+\" (chime, LED,\n"
                "   relay coil - add a flyback diode across coils).\n"
                "2. Load \"-\" -> DO0. Supply \"-\" -> the isolated GND.\n"
                "3. PULSE drives 1.5 s; LATCH holds 30 s max.\n"
                "Every drive is bounded - outputs release themselves."),
            "code": {"DO0": do_code("DO0"), "DO1": do_code("DO1")},
            "stimulus": [{"id": "pulse", "label": "PULSE 1.5s", "action": "do:pulse"},
                         {"id": "latch", "label": "LATCH 30s", "action": "do:latch"}],
            "expect": ["out=on", "out=off"],
        },
        {
            "id": "strobe", "title": "Strobe out", "where": "DO1",
            "signal": "do1", "dir": "out", "kind": "do",
            "peripheral": {"name": "Strobe / siren", "part": "ws2812",
                           "blurb": "Second isolated output — a strobe/siren candidate while DO0 holds the chime."},
            "port": {"label": "Output channel", "choices": ["DO1", "DO0"], "default": "DO1"},
            "wires": [["DO1", "do"], ["GND_ISO", "gnd"]],
            "instructions": (
                "Second isolated output - same wiring as DO0.\n"
                "Use it for a strobe/siren candidate while DO0\n"
                "holds the chime, to test both alert voices at\n"
                "once. PULSE = 1.5 s, LATCH auto-releases at 30 s."),
            "code": {"DO1": do_code("DO1"), "DO0": do_code("DO0")},
            "stimulus": [{"id": "pulse", "label": "PULSE 1.5s", "action": "do:pulse"},
                         {"id": "latch", "label": "LATCH 30s", "action": "do:latch"}],
            "expect": ["out=on", "out=off"],
        },
        {
            "id": "light", "title": "Light", "where": "I2C",
            "signal": "light", "dir": "i2c", "kind": "i2c",
            "peripheral": {"name": "Ambient light sensor", "part": "i2c_sensor",
                           "blurb": "VEML7700 (0x10, preferred) or BH1750 strapped to 0x5C — its default 0x23 is the CH422G's."},
            "port": {"label": "Sensor / address", "choices": ["VEML7700 · 0x10", "BH1750 · 0x5C"],
                     "default": "VEML7700 · 0x10"},
            "wires": [["VOUT", "vout"], ["GND_I2C", "gnd"], ["SDA", "sda"], ["SCL", "scl"]],
            "instructions": (
                "Ambient light sensor on the I2C terminal:\n"
                "1. VEML7700 (addr 0x10, preferred) or BH1750 with\n"
                "   ADDR strapped HIGH (0x5C - its default 0x23 is\n"
                "   the CH422G's, do not use it!).\n"
                "2. VOUT->VCC GND->GND SDA->SDA SCL->SCL.\n"
                "   Check VOUT is 5 V or 3.3 V per your sensor.\n"
                "Hot-plug OK - the census attaches it in ~3 s."),
            "code": {"VEML7700 · 0x10": light_code, "BH1750 · 0x5C": light_code},
            "stimulus": [{"id": "bright", "label": "Uncover (bright)", "action": "light:bright"},
                         {"id": "dark", "label": "Cover (dark)", "action": "light:dark"}],
            "expect": ["attached=0x10", "attached=0x5C"],
        },
        {
            "id": "tof", "title": "ToF range", "where": "I2C",
            "signal": "tof", "dir": "i2c", "kind": "i2c",
            "peripheral": {"name": "VL53L0X ToF", "part": "i2c_sensor",
                           "blurb": "Time-of-flight ranging (0x29). Under the TRIP threshold counts one trip — a laser-gap prototype."},
            "port": {"label": "Trip threshold", "choices": ["50 mm", "100 mm", "200 mm", "400 mm"],
                     "default": "100 mm"},
            "wires": [["VOUT", "vout"], ["GND_I2C", "gnd"], ["SDA", "sda"], ["SCL", "scl"]],
            "instructions": (
                "VL53L0X time-of-flight on the I2C terminal\n"
                "(addr 0x29). Wire like the light sensor.\n"
                "Live mm readout; dropping under the TRIP\n"
                "threshold counts one trip - point it across a\n"
                "doorway and it is your laser-gap prototype.\n"
                "Bench driver: uncalibrated, +/- a few percent."),
            "code": {c: tof_code for c in ["50 mm", "100 mm", "200 mm", "400 mm"]},
            "stimulus": [{"id": "near", "label": "Object near (trip)", "action": "tof:near"},
                         {"id": "far", "label": "Object far (clear)", "action": "tof:far"},
                         {"id": "cycle", "label": "Cycle trip", "action": "tof:cycle"}],
            "expect": ["trip=1 mm=", "trip_mm="],
        },
        {
            "id": "captouch", "title": "Cap touch", "where": "I2C",
            "signal": "captouch", "dir": "i2c", "kind": "i2c",
            "peripheral": {"name": "MPR121 pad", "part": "i2c_sensor",
                           "blurb": "12-electrode controller (0x5A). Cycle sensitivity for the printed shell-thickness coupon test."},
            "port": {"label": "Sensitivity", "choices": ["contact", "2mm shell", "4mm shell", "max gain"],
                     "default": "contact"},
            "wires": [["VOUT", "vout"], ["GND_I2C", "gnd"], ["SDA", "sda"], ["SCL", "scl"]],
            "instructions": (
                "MPR121 12-pad controller (addr 0x5A).\n"
                "Shell-thickness test (printed plastic coupons):\n"
                "1. Tape a coupon over an electrode.\n"
                "2. Cycle SENSITIVITY until a finger through the\n"
                "   coupon registers reliably, no ghost touches.\n"
                "3. Note preset vs thickness in the bench log -\n"
                "   that pair is the enclosure design input."),
            "code": {c: pad_code for c in ["contact", "2mm shell", "4mm shell", "max gain"]},
            "stimulus": [{"id": "touch", "label": "Touch pad", "action": "pad:touch"},
                         {"id": "release", "label": "Release", "action": "pad:release"},
                         {"id": "preset", "label": "Cycle sensitivity", "action": "pad:preset"}],
            "expect": ["pads=0x", "preset="],
        },
        {
            "id": "rs485", "title": "RS485", "where": "A/B",
            "signal": "rs485", "dir": "bus", "kind": "bus",
            "peripheral": {"name": "Modbus RTU device", "part": "rs485",
                           "blurb": "The industrial serial bus. One A/B pair daisy-chains up to 32 devices - energy meters, PLCs, VFDs, HVAC controllers, alarm panels - which you poll by address and read/write numbered registers."},
            "port": None,
            "wires": [["RS485_A", "rs485"], ["RS485_B", "rs485"]],
            "instructions": (
                "RS485 - the industrial serial bus. One A/B pair\n"
                "daisy-chains up to 32 devices on two wires: energy\n"
                "meters, PLCs, VFDs, HVAC controllers, alarm panels.\n"
                "1. A -> A, B -> B (share a ground reference).\n"
                "2. Match the line: 9600 8N1 here (Modbus RTU).\n"
                "3. 120 ohm terminators on long runs, both ends.\n"
                "PROBE reads holding register 0 of slave 1 and shows\n"
                "the value. Shares GPIO44/43 with the USB console."),
            "code": {"rs485": rs485_code},
            "stimulus": [{"id": "probe", "label": "Probe (read reg 0)", "action": "rs485:probe"},
                         {"id": "quiet", "label": "No device", "action": "rs485:quiet"}],
            "expect": ["slave=1 reg=0 val=", "reply=none"],
        },
        {
            "id": "can", "title": "CAN bus", "where": "H/L",
            "signal": "can", "dir": "bus", "kind": "bus",
            "peripheral": {"name": "CAN 2.0 node", "part": "can",
                           "blurb": "The vehicle & automation bus. Two wires (H/L), multi-master, every node hears every frame - cars (OBD-II/J1939), CANopen building gear, gate/barrier controllers, fleet telematics."},
            "port": None,
            "wires": [["CAN_H", "can"], ["CAN_L", "can"]],
            "instructions": (
                "CAN 2.0 / TWAI - the vehicle & automation bus. Two\n"
                "wires (H/L), multi-master, every node hears every\n"
                "frame: cars (OBD-II / J1939), CANopen building gear,\n"
                "gate / barrier controllers, fleet telematics.\n"
                "1. H -> H, L -> L. 120 ohm at BOTH bus ends.\n"
                "2. Match the bit rate: 500 kbit/s here.\n"
                "SEND FRAME transmits one test frame; received frames\n"
                "are counted and logged. Dedicated transceiver."),
            "code": {"can": can_code},
            "stimulus": [{"id": "send", "label": "Send test frame", "action": "can:send"},
                         {"id": "rx", "label": "Node replies", "action": "can:rx"}],
            "expect": ["tx id=", "rx id="],
        },
        {
            "id": "census", "title": "I2C census", "where": "bus",
            "signal": "bus", "dir": "i2c", "kind": "info",
            "peripheral": None,
            "port": None,
            "wires": [["SDA", "sda"], ["SCL", "scl"]],
            "instructions": (
                "Live scan of the shared GPIO8/9 bus (every 3 s).\n"
                "Reserved here: 0x23/0x24/0x26/0x38 = CH422G\n"
                "commands, 0x5D/0x14 = GT911. A sensor set to a\n"
                "reserved address will misbehave AND can glitch\n"
                "backlight/touch - re-strap it before wiring."),
            "code": {"bus": census_code},
            "stimulus": [{"id": "scan", "label": "Scan bus", "action": "bus:scan"}],
            "expect": ["i2c="],
        },
    ]

    # ── Pin tracker — the 4.3B's used-vs-open budget (dev_playground_43b.md).
    pin_tracker = [
        {"name": "DI0 doorbell", "status": "open", "note": "isolated input (EXIO0)"},
        {"name": "DI1 intrusion", "status": "open", "note": "isolated input (EXIO5)"},
        {"name": "DI COM", "status": "open", "note": "isolated-input common"},
        {"name": "DO0 chime", "status": "open", "note": "open-drain out (OD0)"},
        {"name": "DO1 strobe", "status": "open", "note": "open-drain out (OD1)"},
        {"name": f"I2C {sda}/{scl}", "status": "shared", "note": "sensor header — shared with GT911 + CH422G"},
        {"name": "VOUT", "status": "open", "note": "sensor-header supply"},
        {"name": "RS485 44/43", "status": "shared", "note": "Modbus RTU station - shares the USB-UART console"},
        {"name": "CAN 15/16", "status": "open", "note": "CAN/TWAI station - dedicated transceiver, 500 kbit/s"},
        {"name": "VIN 6-36V", "status": "reserved", "note": "wide-input supply"},
        {"name": "LCD x21", "status": "reserved", "note": "RGB565 panel — consumed"},
        {"name": "Touch INT 4", "status": "reserved", "note": "GT911"},
        {"name": "USB 19/20", "status": "reserved", "note": "native USB CDC"},
        {"name": "SD 11-13", "status": "reserved", "note": "reserved-unused v0.1"},
        {"name": "Free GPIO", "status": "none", "note": "none — expansion happens on the buses"},
    ]

    data = {
        "$note": (
            "GENERATED by canary-local/tools/gen_playground.py — do not edit by hand. "
            "CI drift-gates this file against firmware/boards/waveshare-esp32s3-lcd43b/"
            "pins/pins.h and the canary-display playground firmware. The board body is a "
            "schematic procedural stand-in sized from Waveshare's outline (not vendor CAD); "
            "only the terminal endpoints are claims, routing is staged for legibility. "
            "Behavior is ported in canary-local/assets/playground-sim.js (Node-tested)."),
        "schema": 1,
        "generated_by": "canary-local/tools/gen_playground.py",
        "board": {
            "id": board_id,
            "name": board_name,
            "vendor": board_vendor,
            "mcu": board_mcu,
            "fw_version": fw_version,
            "enclosure": "LCD-4.3B-BOX",
            "dims_mm": [136, 88, 26],
            "connector": {
                "face": "back", "edge": "+z", "ways": 16,
                "pitch_mm": 7.5, "row_y": row_y, "row_z": row_z,
                "note": ("ONE pluggable 16-way green terminal block along the top edge "
                         "of the BACK face — see the enclosure rear silkscreen."),
            },
            "display": {"size_in": 4.3, "res": [int(need(pins, "LCD_WIDTH")),
                                                int(need(pins, "LCD_HEIGHT"))],
                        "iface": "RGB565 parallel (DE mode)", "face": "front"},
            "tagline": "Wire it, watch it — the peripheral playground on real Waveshare hardware.",
            "blurb": ("The industrial-IO dash SKU. No raw ESP32 GPIO is broken out — every "
                      "external wire lands on an isolated, buffered, or bused terminal, which "
                      "is exactly what makes it the safe host for the peripheral playground."),
            "i2c": {"sda": int(sda), "scl": int(scl)},
            "expander": {"read": addr_in, "oc": addr_oc},
            "provenance": ("Pin/terminal map + the CH422G addresses and isolated DI/DO bits are "
                           "parsed from firmware/boards/waveshare-esp32s3-lcd43b/pins/pins.h; "
                           "station behavior ports firmware/projects/canary-display/.../playground.cpp; "
                           "wiring text is verbatim from playground_ui.cpp; comms + pin tracker from "
                           "docs/hardware/dev_playground_43b.md. Compile-verified board (the pin map "
                           "carries VERIFY tags) — verify polarity/timings against your revision."),
        },
        "comms": {
            "hello": "PG1 HELLO board=<id> fw=<ver>",
            "evt": "PG1 <ms> EVT <station> <k>=<v>...",
            "snap": "PG1 <ms> SNAP di0=.. di1=.. do0=.. do1=.. lux=.. tof_mm=.. tof_ok=.. pads=.. preset=.. i2c=..",
            "snap_every_ms": 1000,
        },
        "colors": colors,
        "terminals": terminals,
        "groups": groups,
        "stations": stations,
        "pinTracker": pin_tracker,
    }

    OUT.write_text(json.dumps(data, indent=1, ensure_ascii=True) + "\n")
    print(f"wrote {OUT.relative_to(ROOT)} ({len(stations)} stations, "
          f"{len(terminals)} terminals) fw={fw_version}")


if __name__ == "__main__":
    main()
