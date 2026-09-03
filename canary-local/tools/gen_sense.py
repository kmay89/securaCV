#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_sense.py — build canary-local/devices/sense.json from the canary-sense firmware.

The Sense teaching page (`canary-local/sense.html`) stages the first boot,
USB provisioning, radar placement lab, serial console, and MQTT/Home-Assistant
surface of the `canary-sense` device — the 60 GHz mmWave witness on the Seeed
MR60BHA2 kit (XIAO ESP32-C6 host). None of it is hand-faked: every string the
page shows is either parsed straight out of the firmware/docs, or authored
here and then *validated to still exist in the source* (this file
`sys.exit(1)`s on drift). CI re-runs the generator and `git diff --exit-code`s
`sense.json`, so if the firmware changes a topic, a threshold, a boot line or
an HA entity, the page breaks the build until it is regenerated — the same
anti-rot contract as `gen_wap.py` / `gen_homeassistant.py`.

Sources of truth (all in-repo, deterministic, offline):
  firmware/projects/canary-sense/src/main.cpp        boot scenes, chokepoint vocab,
                                                     event names, LED grammar
  firmware/projects/canary-sense/src/net/*.cpp       WiFi/MQTT/mDNS/OTA log lines + payloads
  firmware/projects/canary-sense/src/ha/ha_discovery.cpp  the HA entity set
  firmware/projects/canary-sense/src/witness.cpp     Ed25519 witness log lines
  firmware/projects/canary-sense/include/canary/*    topics, runtime-config policy
  firmware/projects/canary-sense/secrets/secrets.example.h  provisioning fields
  firmware/configs/canary-sense/{default,wellbeing}/config.h  every FSM threshold
  firmware/boards/xiao-esp32c6-mr60/pins/pins.h      the kit's real wiring
  firmware/common/sensors/mmwave_mr60/mr60_uart.h    the radar wire protocol
  firmware/common/sensors/mmwave_mr60/mr60_{presence,vitals}.h  FSM contracts
  firmware/common/boot/boot_banner.cpp               shared boot scenes
  docs/canary_sense_mr60bha2_design.md               hardware envelope + scenarios
  firmware/projects/canary-sense/README.md           quickstart + entity table
  canary-local/devices/registry.json                 fw_train + the canary-sense card

Placement/tuning numbers that come from OUTSIDE the repo (the Seeed wiki,
the ESPHome component docs, community deployments) are carried with explicit
`src` provenance labels — the page renders where each claim comes from, and
in-repo claims are still drift-gated here.

Run:  python3 canary-local/tools/gen_sense.py
"""

import json
import re
from pathlib import Path

from _tooling import die, repo_root

REPO = repo_root()
PRJ = REPO / "firmware/projects/canary-sense"
MAIN_CPP = PRJ / "src/main.cpp"
WIFI_CPP = PRJ / "src/net/wifi_mgr.cpp"
MQTT_CPP = PRJ / "src/net/mqtt_mgr.cpp"
MDNS_CPP = PRJ / "src/net/mdns_mgr.cpp"
OTA_CPP = PRJ / "src/net/ota_mgr.cpp"
DISC_CPP = PRJ / "src/ha/ha_discovery.cpp"
WITNESS_CPP = PRJ / "src/witness.cpp"
TOPICS_H = PRJ / "include/canary/topics.h"
RUNTIME_H = PRJ / "include/canary/runtime_config.h"
PROJECT_CFG_H = PRJ / "include/canary/config.h"
VERSION_H = PRJ / "include/canary/version.h"
SECRETS_H = PRJ / "secrets/secrets.example.h"
FW_README = PRJ / "README.md"
PIO_INI = PRJ / "platformio.ini"
CFG_DEFAULT = REPO / "firmware/configs/canary-sense/default/config.h"
CFG_WELLBEING = REPO / "firmware/configs/canary-sense/wellbeing/config.h"
PINS_H = REPO / "firmware/boards/xiao-esp32c6-mr60/pins/pins.h"
UART_H = REPO / "firmware/common/sensors/mmwave_mr60/mr60_uart.h"
PRESENCE_H = REPO / "firmware/common/sensors/mmwave_mr60/mr60_presence.h"
VITALS_H = REPO / "firmware/common/sensors/mmwave_mr60/mr60_vitals.h"
BANNER_CPP = REPO / "firmware/common/boot/boot_banner.cpp"
DESIGN = REPO / "docs/canary_sense_mr60bha2_design.md"
REGISTRY = REPO / "canary-local/devices/registry.json"
OUT_JSON = REPO / "canary-local/devices/sense.json"

# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #

_CACHE: dict = {}


def read(path: Path) -> str:
    if path not in _CACHE:
        if not path.exists():
            die(f"source missing: {path.relative_to(REPO)}")
        _CACHE[path] = path.read_text(encoding="utf-8", errors="replace")
    return _CACHE[path]


def must(path: Path, needle: str, label: str) -> None:
    """Assert a literal still exists in a source file (the drift gate)."""
    if needle not in read(path):
        die(f"{label}: expected to find {needle!r} in {path.relative_to(REPO)} — firmware changed?")


def must_any(paths, needle: str, label: str) -> None:
    if not any(needle in read(p) for p in paths):
        where = ", ".join(p.name for p in paths)
        die(f"{label}: expected to find {needle!r} in one of [{where}] — firmware changed?")


def grab(path: Path, pattern: str, label: str, flags=0) -> str:
    m = re.search(pattern, read(path), flags)
    if not m:
        die(f"{label}: pattern /{pattern}/ not found in {path.relative_to(REPO)}")
    return m.group(1)


def grab_int(path: Path, macro: str) -> int:
    return int(grab(path, rf"#define\s+{macro}\s+(\d+)", macro))


def grab_str(path: Path, macro: str) -> str:
    return grab(path, rf'#define\s+{macro}\s+"([^"]+)"', macro)


# --------------------------------------------------------------------------- #
# 1. identity + version
# --------------------------------------------------------------------------- #

FW_VERSION = grab_str(VERSION_H, "CANARY_FW_VERSION")
DEVICE_TYPE = grab_str(CFG_DEFAULT, "CS_DEVICE_TYPE")
DEVICE_ID_SEED = grab_str(CFG_DEFAULT, "CS_DEVICE_ID")
MODEL = grab_str(CFG_DEFAULT, "CS_MODEL")
MODEL_WELLBEING = grab_str(CFG_WELLBEING, "CS_MODEL")
MANUFACTURER = grab_str(CFG_DEFAULT, "CS_MANUFACTURER")
OTA_PRODUCT = grab(PROJECT_CFG_H, r'#define\s+SECURACV_OTA_PRODUCT\s+"([^"]+)"', "OTA product")
BOARD_NAME = grab_str(PINS_H, "BOARD_NAME")
BOARD_ID = grab_str(PINS_H, "BOARD_ID")

registry = json.loads(read(REGISTRY))
FW_TRAIN = registry.get("fw_train")
if not FW_TRAIN:
    die("registry.json has no fw_train")
if not FW_VERSION.startswith(FW_TRAIN):
    die(f"firmware version {FW_VERSION!r} does not ride registry fw_train {FW_TRAIN!r}")

sense_reg = next((d for d in registry.get("devices", []) if d.get("id") == "canary-sense"), None)
if not sense_reg:
    die("registry.json has no canary-sense device entry")

# Illustrative values — a real unit derives its own from its NVS identity.
EX_ID = DEVICE_ID_SEED                      # the compiled first-boot seed
EX_FP = "b7e2c49a11f03d5c"                  # 16-hex Ed25519 fingerprint (witness.cpp fp_hex[16])
EX_HOST = "canary-sense-001-b7e2c4"         # mdns make_hostname(): id hyphenated + 6-hex pseudonym
EX_HWID = "9f41c2d8a06be375"                # salted MAC-free pseudonym (device_pseudonym)
EX_BROKER = "192.168.1.10"
must(WITNESS_CPP, "fp_hex[16] = '\\0'", "16-hex fingerprint length")
must(MDNS_CPP, '"%s-%.6s", base, devid_hex', "mdns hostname recipe")

# --------------------------------------------------------------------------- #
# 2. the radar — hardware envelope + the real UART wire protocol
# --------------------------------------------------------------------------- #

RADAR_TX = grab_int(PINS_H, "RADAR_UART_TX")
RADAR_RX = grab_int(PINS_H, "RADAR_UART_RX")
RADAR_BAUD = grab_int(PINS_H, "RADAR_UART_BAUD")
RADAR_UART_NUM = grab_int(PINS_H, "RADAR_UART_NUM")
I2C_SDA = grab_int(PINS_H, "I2C_PIN_SDA")
I2C_SCL = grab_int(PINS_H, "I2C_PIN_SCL")
LED_PIN = grab_int(PINS_H, "LED_WS2812_PIN")
BOOT_PIN = grab_int(PINS_H, "BOOT_BUTTON_PIN")
BH1750_ADDR = grab(PINS_H, r"#define\s+BH1750_I2C_ADDR\s+(0x[0-9A-Fa-f]+)", "BH1750 addr")

# The hardware envelope comes from the design doc (which cites the Seeed wiki
# + ESPHome component); every number here is asserted against that doc.
for needle in ("57–64 GHz", "ADT6101P", "80° × 80°", "1.5–6 m",
               "~90% acc", "~85% acc", "≤1.5 m", "BH1750", "1–65,535 lux"):
    must(DESIGN, needle, f"design-doc hardware envelope {needle!r}")

# Wire protocol facts, straight from the vendored decoder header.
SOF = grab(UART_H, r"MR60_SOF\s*=\s*(0x[0-9A-Fa-f]+)", "MR60_SOF")
FRAME_TYPES = []
for const, wire_name, decode in [
    ("MR60_TYPE_PEOPLE_EXIST", "PEOPLE_EXIST", "uint16 LE has_target (nonzero == present)"),
    ("MR60_TYPE_TARGET_COUNT", "PRINT_CLOUD", "uint32 LE target count"),
    ("MR60_TYPE_DISTANCE", "DISTANCE", "byte[0] valid flag, float32 LE @ [4..7] (meters)"),
    ("MR60_TYPE_BREATH_RATE", "BREATH_RATE", "float32 LE (breaths/min)"),
    ("MR60_TYPE_HEART_RATE", "HEART_RATE", "float32 LE (beats/min)"),
]:
    tid = grab(UART_H, rf"{const}\s*=\s*(0x[0-9A-Fa-f]+)", const)
    FRAME_TYPES.append({"id": tid, "name": wire_name, "decodes": decode})
must(UART_H, "c ^= b; c = ~c;", "XOR-fold checksum")
must(UART_H, "HEADER scalar fields (id/len/type) are BIG-endian", "mixed endianness note")
must(UART_H, "MR60_FRAME_QUEUE = 8", "frame queue depth")

RADAR = {
    "module": "MR60BHA2",
    "soc": "ADT6101P — 2T2R antenna, 1 MB flash, on-chip Cortex-M3 DSP",
    "band": "57–64 GHz FMCW",
    "fov_deg": 80,
    "fov": "≈ 80° × 80° detection sector",
    "presence_range": "static human presence 1.5–6 m",
    "presence_max_m": 6.0,
    "vitals_range_m": 1.5,
    "vitals_accuracy": "breathing ~90%, heart rate ~85% (≤1.5 m, single target)",
    "host": "XIAO ESP32-C6 (RISC-V, WiFi 6, BLE 5, 802.15.4)",
    "link": f"UART{RADAR_UART_NUM} · host TX GPIO{RADAR_TX} → radar RX, host RX GPIO{RADAR_RX} ← radar TX · {RADAR_BAUD} 8N1",
    "peripherals": [
        f"BH1750 ambient light (I2C SDA {I2C_SDA} / SCL {I2C_SCL}, addr {BH1750_ADDR}, 1–65,535 lux)",
        f"WS2812 RGB status LED (GPIO{LED_PIN})",
        f"BOOT button (GPIO{BOOT_PIN}, download-mode strap)",
    ],
    "protocol": {
        "sof": SOF,
        "header": "SOF(1) + frame id(2) + payload len(2) + frame type(2) + header checksum(1) — header scalars BIG-endian",
        "payload": "N payload bytes (every multi-byte payload field LITTLE-endian) + 1 data checksum byte",
        "checksum": "XOR-fold every covered byte into an 8-bit accumulator, then invert (c ^= b; c = ~c)",
        "frames": FRAME_TYPES,
        "aggregation": "each UART frame carries ONE scalar; the parser folds the latest of each into a combined snapshot for the FSMs",
        "resync": "on any checksum/length failure the parser drops a single byte and re-hunts — it never discards bytes that might start a real frame",
    },
    "privacy": "the raw radar IQ never leaves the radar module's own DSP — the host MCU only ever sees pre-digested scalars over UART: presence flag, target count, distance, breathing rate, heart rate. No camera, no microphone, no MAC surface.",
    "blackbox_note": "the radar module's own firmware (in-radar zone config etc.) is partnership-gated; SecuraCV treats it as a fixed black-box claim source and does all tuning host-side on the ESP32-C6.",
}
must(DESIGN, "witnessing without watching", "privacy thesis")
must(DESIGN, "only modifiable under a Seeed business partnership", "black-box note")

# --------------------------------------------------------------------------- #
# 3. the FSMs — every threshold the page teaches, from the flavor configs
# --------------------------------------------------------------------------- #

FSM = {
    "presence": {
        "debounce_ms": grab_int(CFG_DEFAULT, "CS_PRESENT_DEBOUNCE_MS"),
        "clear_ms": grab_int(CFG_DEFAULT, "CS_CLEAR_TIMEOUT_MS"),
        "stall_ms": grab_int(CFG_DEFAULT, "CS_RADAR_STALL_MS"),
        "near_cm": grab_int(CFG_DEFAULT, "CS_RANGE_NEAR_CM"),
        "mid_cm": grab_int(CFG_DEFAULT, "CS_RANGE_MID_CM"),
        "states": [
            {"name": "Unknown", "led": "amber", "rgb": [24, 8, 0],
             "meaning": "startup, or the radar UART silent past the stall deadline — radar-link health, not a witness event"},
            {"name": "Clear", "led": "blue", "rgb": [0, 0, 16],
             "meaning": "no target — the room reads empty"},
            {"name": "Present", "led": "green", "rgb": [0, 24, 0],
             "meaning": "a target sustained past the debounce — someone is here"},
        ],
        "stall_note": "deadline checks run BEFORE data guards: a silent radar drives the FSM to Unknown instead of freezing on the last good frame",
    },
    "vitals": {
        "flavor": "wellbeing builds only (-DCANARY_SENSE_VITALS)",
        "lock_ms": grab_int(CFG_WELLBEING, "CS_VITALS_LOCK_MS"),
        "lost_ms": grab_int(CFG_WELLBEING, "CS_VITALS_LOST_MS"),
        "breath_bpm": [grab_int(CFG_WELLBEING, "CS_BREATH_MIN_BPM"), grab_int(CFG_WELLBEING, "CS_BREATH_MAX_BPM")],
        "heart_bpm": [grab_int(CFG_WELLBEING, "CS_HEART_MIN_BPM"), grab_int(CFG_WELLBEING, "CS_HEART_MAX_BPM")],
        "single_target_rule": "vitals are hard-suppressed unless exactly one target is present — multi-person BPM attribution is the design doc's own risk table entry",
        "non_diagnostic": "wellbeing signals, not medical data: never sealed-logged, never precise-timestamped, P1 opt-in for BPM numerics",
    },
    "count_buckets": ["0", "1", "2+"],
    "range_bands": ["near", "mid", "far"],
    "identify": {"seconds": 10, "pattern": "2 Hz white flash — unmistakable against the steady presence colors"},
}
# chokepoint vocabulary + event names are load-bearing page copy
for needle in ('return "present"', 'return "clear"', 'return "unknown"',
               'return "near"', 'return "mid"', 'return "far"',
               'return "1"', 'return "2+"',
               '"presence_detected"', '"presence_cleared"', '"occupancy_changed"',
               "led_show(0, 24, 0)", "led_show(0, 0, 16)", "led_show(24, 8, 0)",
               "g_identify_until_ms = now + 10000UL"):
    must(MAIN_CPP, needle, f"chokepoint/main anchor {needle!r}")
must(VITALS_H, "vitals are suppressed", "single-target rule")
must(MAIN_CPP, "const bool single_target = (pev.count == CountBucket::One);", "single-target gate")

EVENTS = ["presence_detected", "presence_cleared", "occupancy_changed"]

# --------------------------------------------------------------------------- #
# 4. provisioning — Track A (USB flash seeds NVS) + Track B (stock ESPHome)
# --------------------------------------------------------------------------- #

# Track A quickstart commands are the README's own.
for cmd in ("cp secrets/secrets.example.h secrets/secrets.h",
            "pio run", "pio run -e canary-sense-wellbeing",
            "pio run -t upload", "pio device monitor -b 115200"):
    must(FW_README, cmd, f"README quickstart {cmd!r}")
must(PIO_INI, "default_envs = canary-sense-default", "default env")

SECRETS_FIELDS = []
for macro, hint in [("WIFI_SSID", "your home WiFi network name"),
                    ("WIFI_PASS", "your home WiFi password"),
                    ("MQTT_HOST", "broker address (your Home Assistant box)"),
                    ("MQTT_PORT", "broker port (1883)"),
                    ("MQTT_USER", "broker username (optional)"),
                    ("MQTT_PASS", "broker password (optional)")]:
    must(SECRETS_H, f"#define {macro}", f"secrets field {macro}")
    SECRETS_FIELDS.append({"macro": macro, "hint": hint})

# The NVS policy is the load-bearing "why your unit survives updates" fact.
must(RUNTIME_H, "device_id: NVS always wins", "NVS device-id policy")
must(RUNTIME_H, "real compiled values win and are persisted", "NVS credential policy")
must(FW_README, "seeds the unit's NVS", "NVS seeding note")

PROVISIONING = {
    "intro": "No captive portal on this witness — the Sense kit has no HTTP server at all "
             "(smaller attack surface, smaller firmware). You provision it once over USB; "
             "NVS remembers everything across every update after that.",
    "envs": [
        {"id": "canary-sense-default", "label": "presence-only", "note": "presence / occupants / range band / lux — vitals compiled OUT"},
        {"id": "canary-sense-wellbeing", "label": "wellbeing", "note": "adds the breathing lock + P1-gated BPM numerics (-DCANARY_SENSE_VITALS)"},
    ],
    "steps": [
        {"n": 1, "title": "Copy the secrets template",
         "cmd": "cp secrets/secrets.example.h secrets/secrets.h",
         "detail": "fill in WiFi + broker; secrets.h is git-ignored and never leaves your machine"},
        {"n": 2, "title": "Build the flavor you want",
         "cmd": "pio run   # or: pio run -e canary-sense-wellbeing",
         "detail": "the C6 builds on the pinned pioarduino platform (arduino-esp32 3.x); first run downloads it"},
        {"n": 3, "title": "Flash over USB-C",
         "cmd": "pio run -t upload",
         "detail": "this first real-secrets flash seeds the unit's NVS: identity, WiFi, broker"},
        {"n": 4, "title": "Watch it come up",
         "cmd": "pio device monitor -b 115200",
         "detail": "the USB-CDC console below is exactly what you'll see"},
    ],
    "secrets_fields": SECRETS_FIELDS,
    "nvs_policy": [
        "device_id: NVS always wins — identity is sticky across reflashes and OTA updates; the compiled seed only fills the very first boot",
        "WiFi / MQTT credentials: real compiled values win and are persisted; placeholder builds (CI stubs, generic OTA releases) defer to NVS",
        "net effect: provision once over USB, then every signed OTA release build inherits the unit's setup",
    ],
    "trackb": {
        "title": "Or: the 10-minute on-ramp (stock kit, zero flashing)",
        "body": "The kit ships pre-flashed with ESPHome (the upstream seeed_mr60bha2 component) speaking "
                "the ESPHome native API to Home Assistant — not MQTT. Two config-only bridges get its "
                "claims into the SecuraCV kernel:",
        "paths": [
            {"name": "HA MQTT Statestream (kit stays unmodified)",
             "how": "enable HA's built-in mqtt_statestream; HA republishes the kit's entities to MQTT and the mqtt_sensor adapter's mr60bha2 profile maps them into claims"},
            {"name": "ESPHome mqtt: overlay (one OTA, no custom firmware)",
             "how": "add an mqtt: block to the kit's ESPHome config and push it over the air; the device then publishes to the broker directly"},
        ],
        "honesty": "Track B claims are kernel-signed at ingest, not device-signed — Home Assistant renders them "
                   "with the yellow 'adapter-attested' badge, never the green 'device-verified ✓'. Track A (this page's path) earns the green badge.",
    },
}
must(DESIGN, "mqtt_statestream", "Track B statestream path")
must(DESIGN, "seeed_mr60bha2", "ESPHome component name")
must(DESIGN, "adapter-attested", "Track B badge honesty")
must(CFG_DEFAULT, "#define FEATURE_HTTP_SERVER         0", "no HTTP server")
must(CFG_DEFAULT, "#define FEATURE_WIFI_AP             0", "no SoftAP")

# --------------------------------------------------------------------------- #
# 5. the serial console — faithful to boot_banner.cpp + main.cpp setup() order
# --------------------------------------------------------------------------- #

for needle in ("Waking up...", "This is your privacy witness device.",
               "Checking the hardware...", "The canary is singing. Everything is working."):
    must(BANNER_CPP, needle, "boot banner scene")
for needle in ("Who is in the room?", "Connecting to MQTT...",
               '"It will witness presence over 60GHz radar"',
               '"and publish signed coarse claims via MQTT to Home Assistant."'):
    must(MAIN_CPP, needle, "sense boot scene")

D = FSM["presence"]
BANNER = [
    "              ,_,          Waking up...",
    "             (o.o)",
    "             /| |\\         SecuraCV Canary Sense",
    f"              d b          v{FW_VERSION}",
    "",
    "    This is your privacy witness device.",
    "    It creates tamper-proof records of what it",
    "    sees, so nobody can change the story later.",
    "",
    f"    Type        {DEVICE_TYPE}",
    f"    Model       {MODEL}",
    "    ------------------------------------------------",
    "",
    "              ,_,",
    "             (o.o) ?       Checking the hardware...",
    "             (  >)",
    "              \" \"          What am I running on?",
    "    ------------------------------------------------",
    f"    Board       {BOARD_NAME}",
    "    Chip        ESP32-C6 rev 0",
    "    CPU         160 MHz, 1 core(s)",
    "    Flash       4 MB",
    "    PSRAM       not found",
    "    Heap        305 KB free at boot",
    "",
    "              .   .   .",
    "           .  ((( o )))  .      Who is in the room?",
    "              '   '   '",
    "    ------------------------------------------------",
    "    Sensor      MR60BHA2 60GHz FMCW radar (UART)",
    f"    Radar       UART{RADAR_UART_NUM}  TX={RADAR_TX} RX={RADAR_RX}  @ {RADAR_BAUD} 8N1",
    f"    Lux         BH1750 I2C  SDA={I2C_SDA} SCL={I2C_SCL}  addr {BH1750_ADDR}",
    f"    LED         WS2812 on GPIO{LED_PIN}",
    "    Vitals      disabled (presence-only build)",
    f"    Present     {D['debounce_ms']} ms debounce, {D['clear_ms']} ms clear, {D['stall_ms']} ms stall",
    "",
]
# the boot-scene kv lines above are printf'd from main.cpp — anchor them
for needle in ('boot_kv("Sensor",  "MR60BHA2 60GHz FMCW radar (UART)")',
               '"Vitals",  "disabled (presence-only build)"',
               '"%lu ms debounce, %lu ms clear, %lu ms stall"'):
    must(MAIN_CPP, needle, f"radar boot scene {needle!r}")

BOOT = [
    {"tag": "[BH1750]", "text": "ambient light sensor online", "src": '"BH1750", "ambient light sensor online"', "srcf": MAIN_CPP},
    {"tag": "[WITNESS]", "text": "Generated new Ed25519 identity (first boot).", "src": "Generated new Ed25519 identity (first boot).", "srcf": WITNESS_CPP},
    {"tag": "[WITNESS]", "text": f"Ed25519 ready  fp={EX_FP}  chain_len=0", "src": "Ed25519 ready  fp=%s  chain_len=%lu", "srcf": WITNESS_CPP},
    {"tag": "[WIFI]", "text": 'Connecting SSID="Loft 2.4G" ...', "src": 'Connecting SSID=\\"%s\\" ...', "srcf": WIFI_CPP},
    {"tag": "", "text": ". . . . ."},
    {"tag": "[WIFI]", "text": "Connected IP=192.168.1.62 RSSI=-54dBm", "src": "Connected IP=%s RSSI=%ddBm", "srcf": WIFI_CPP},
    {"tag": "[MDNS]", "text": f"Fleet advert up as {EX_HOST}.local (_securacv._tcp)", "src": "Fleet advert up as %s.local", "srcf": MDNS_CPP},
    {"tag": "[MQTT]", "text": f"Connecting {EX_BROKER}:1883 as {EX_ID} ...", "src": "Connecting %s:%u as %s ...", "srcf": MQTT_CPP},
    {"tag": "[MQTT]", "text": "Connected.", "src": '"MQTT", "Connected."', "srcf": MQTT_CPP},
    {"tag": "[DISC]", "text": "Home Assistant discovery published (retained).", "src": "Home Assistant discovery published (retained).", "srcf": DISC_CPP},
    {"tag": "[OTA]", "text": "Pull-OTA engine ready.", "src": '"OTA", "Pull-OTA engine ready."', "srcf": OTA_CPP},
    {"tag": "[WD]", "text": "Watchdog    30 s timeout", "src": '"Watchdog", "%lu s timeout"', "srcf": MAIN_CPP},
]
for step in BOOT:
    src = step.pop("src", None)
    srcf = step.pop("srcf", None)
    if src:
        # the C sources hold the escaped literal for printf'd quotes
        raw = src.replace('\\"', '"')
        if src not in read(srcf) and raw not in read(srcf):
            die(f"boot log anchor {src!r} not found in {srcf.relative_to(REPO)}")

READY = [
    "",
    "    ================================================",
    "",
    "                   ,_,",
    "                  (o.o)  ~~",
    "                 /(> <)\\ ~~~~",
    "                  d | b  ~~~~~~",
    "",
    "    The canary is singing. Everything is working.",
    "",
    "    It will witness presence over 60GHz radar",
    "    and publish signed coarse claims via MQTT to Home Assistant.",
    "",
    "    ================================================",
]

RUNTIME = [
    {"tag": "[presence]", "text": "-> present", "cls": "ok"},
    {"tag": "[vitals]", "text": "breathing locked", "cls": "ok"},
    {"tag": "[health]", "text": "up 300s  heap 142KB  frame_errs 0", "cls": "faint"},
]
for needle in ('"[presence] -> %s%s"', '"[vitals] breathing %s%s"',
               '"[health] up %lus  heap %luKB  frame_errs %lu"'):
    must(MAIN_CPP, needle, f"runtime line {needle!r}")

SERIAL = {
    "baud": 115200,
    "port_note": "USB-CDC on the XIAO ESP32-C6's USB Serial/JTAG peripheral — UART0 stays free; the radar rides UART1",
    "banner": BANNER,
    "boot": BOOT,
    "ready": READY,
    "runtime": RUNTIME,
    "tags": {
        "[WITNESS]": "witness", "[WIFI]": "wifi", "[MDNS]": "wifi", "[MQTT]": "mqtt",
        "[DISC]": "mqtt", "[OTA]": "done", "[WD]": "done", "[BH1750]": "done",
        "[presence]": "presence", "[vitals]": "vitals", "[health]": "heap",
    },
    "wellbeing_delta": 'the wellbeing build prints "Vitals      ENABLED (P1-gated wellbeing channel)" in the radar scene instead',
}
must(MAIN_CPP, '"Vitals",  "ENABLED (P1-gated wellbeing channel)"', "wellbeing vitals line")

# --------------------------------------------------------------------------- #
# 6. MQTT — topics, payloads, LWT, HA discovery (parsed/validated)
# --------------------------------------------------------------------------- #

# Topic suffixes from topics.h (the single build_topics() source of truth).
topics_src = read(TOPICS_H)
TOPIC_FMTS = re.findall(r'"securacv/%s/([a-z/_]+)"', topics_src)
for want in ("events", "state", "status", "chain", "health",
             "update/state", "update/cmd", "update/auto", "update/auto/cmd",
             "identify/set", "identify"):
    if want not in TOPIC_FMTS:
        die(f"topic suffix {want!r} not built in topics.h")

STATE_PAYLOAD = ('{"device_id":"%s","device_type":"canary-sense","presence":true,'
                 '"presence_state":"present","occupants":"1","range":"mid","radar_ok":true,'
                 '"frame_errors":0,"lux":142.5,"last_event":"presence_detected","uptime_s":312,"ts_ms":312400}'
                 ) % EX_ID
STATE_PAYLOAD_WELLBEING = ('{…,"breathing_locked":true,"breath_bpm":14,"heart_bpm":68,…}  '
                           "(BPM fields null unless the lock holds — stale vitals never freeze in HA)")
EVENT_PAYLOAD = ('{"device_id":"%s","device_type":"canary-sense","event":"presence_detected",'
                 '"seq":313,"bucket_uptime_s":0,"presence":"present","occupants":"1","range":"mid",'
                 '"signed":true,"v":1,"alg":"ed25519","fp":"%s","sig":"…"}') % (EX_ID, EX_FP)
# field names must exist in the publisher sources
for f in ('\\"presence\\"', '\\"presence_state\\"', '\\"occupants\\"', '\\"range\\"',
          '\\"radar_ok\\"', '\\"frame_errors\\"', '\\"lux\\"', '\\"last_event\\"',
          '\\"uptime_s\\"', '\\"breathing_locked\\"', '\\"breath_bpm\\"', '\\"heart_bpm\\"'):
    must(MQTT_CPP, f, f"state field {f}")
for f in ('\\"event\\"', '\\"seq\\"', '\\"bucket_uptime_s\\"', '\\"signed\\"'):
    must(MAIN_CPP, f, f"event field {f}")
must(MAIN_CPP, "(now_ms / 1000UL / 600UL) * 600UL", "10-minute uptime bucketing")

TOPICS = [
    {"suffix": "events", "retained": False, "cadence": "per witnessed transition (presence/occupancy only)",
     "payload": EVENT_PAYLOAD,
     "note": "the FULL vocabulary that ever leaves the device about what the radar saw — no distance, no vitals, ever; time is a 10-minute uptime bucket"},
    {"suffix": "state", "retained": True, "cadence": "on change + heartbeat",
     "payload": STATE_PAYLOAD, "wellbeing": STATE_PAYLOAD_WELLBEING},
    {"suffix": "status", "retained": True, "cadence": "on connect + 5 s heartbeat",
     "payload": '{"device_id":"%s","device_type":"canary-sense","status":"online","presence":true,"radar_ok":true,"rssi":-54,"heap_free":145120,"heap_min":128044,"degraded":"normal","ts_ms":312400}' % EX_ID},
    {"suffix": "chain", "retained": True, "cadence": "on each witnessed event",
     "payload": '{"v":1,"length":313,"latest_hash":"9c2e…","algorithm":"ed25519","alg":"ed25519","fp":"%s","sig":"…"}' % EX_FP,
     "note": "HA rebuilds the canonical from (device_id, length, latest_hash) and verifies against the TOFU-pinned pubkey"},
    {"suffix": "health", "retained": True, "cadence": "~60 s + on reconnect",
     "payload": '{"battery":100,"battery_present":false,"memory_free":145120,"uptime":312,"firmware_version":"%s","public_key":"…"}' % FW_VERSION,
     "note": "carries the pubkey Home Assistant TOFU-pins on first sight — the green-badge trust anchor"},
    {"suffix": "update/state", "retained": True, "cadence": "on OTA state change",
     "payload": '{"installed_version":"%s","latest_version":"%s","in_progress":false,"update_percentage":null}' % (FW_VERSION, FW_VERSION)},
    {"suffix": "update/auto", "retained": True, "cadence": "on toggle", "payload": '"ON" | "OFF"'},
    {"suffix": "identify", "retained": False, "cadence": "on blink window open/close", "payload": '"on" | "off"',
     "note": "echoes the 10 s LED window so dashboards can pulse the device card in sync"},
]
SUBSCRIBED = [
    {"suffix": "update/cmd", "payload": '"install"'},
    {"suffix": "update/auto/cmd", "payload": '"ON" | "OFF"'},
    {"suffix": "identify/set", "payload": '"identify"'},
    # Runtime radar reflexes (sense_config): one command topic per knob, the
    # same cfg/* schema canary-vision's detection dials speak. Validated
    # against topics.h below so this list can't outlive the firmware.
    {"suffix": "cfg/debounce/set", "payload": "ms (clamped by sense_config)"},
    {"suffix": "cfg/clear/set", "payload": "ms"},
    {"suffix": "cfg/stall/set", "payload": "ms"},
    {"suffix": "cfg/near/set", "payload": "cm"},
    {"suffix": "cfg/mid/set", "payload": "cm"},
    {"suffix": "cfg/vitals_lock/set", "payload": "ms (wellbeing)"},
    {"suffix": "cfg/vitals_lost/set", "payload": "ms (wellbeing)"},
]
TOPICS_H = PRJ / "include/canary/topics.h"
for t in SUBSCRIBED:
    if t["suffix"].startswith("cfg/"):
        must(TOPICS_H, t["suffix"], f"cfg topic {t['suffix']}")
must(MQTT_CPP, '\\"status\\":\\"offline\\"', "LWT payload")

# HA discovery entities, validated against ha_discovery.cpp object ids + names.
def entity(comp, obj, name, state_topic, flavor="default", **kw):
    must(DISC_CPP, f'"{obj}"', f"HA entity object_id {obj}")
    must(DISC_CPP, f'\\"name\\":\\"{name}\\"', f"HA entity name {name}")
    e = {"component": comp, "object_id": obj, "name": name, "state_topic": state_topic, "flavor": flavor}
    e.update(kw)
    return e

ENTITIES = [
    entity("binary_sensor", "presence", "Presence", "state", device_class="occupancy"),
    entity("sensor", "occupants", "Occupants", "state", note="bucketed 0 / 1 / 2+ — never a track log"),
    entity("sensor", "range_band", "Range band", "state", category="diagnostic", note="coarse near/mid/far only"),
    entity("binary_sensor", "radar_link", "Radar link problem", "state", device_class="problem", category="diagnostic"),
    entity("sensor", "frame_errors", "Radar frame errors", "state", state_class="total_increasing", category="diagnostic"),
    entity("sensor", "illuminance", "Illuminance", "state", device_class="illuminance", unit="lx"),
    entity("sensor", "last_event", "Last event", "state"),
    entity("sensor", "uptime", "Uptime", "state", device_class="duration", unit="s"),
    entity("sensor", "rssi", "WiFi RSSI", "status", device_class="signal_strength", unit="dBm", category="diagnostic"),
    entity("sensor", "heap_free", "Free heap", "status", unit="B", category="diagnostic"),
    entity("binary_sensor", "breathing", "Breathing confirmed", "state", flavor="wellbeing",
           note="the P0 binary lock — the only always-on vitals signal"),
    entity("sensor", "breath_rate", "Breathing rate", "state", flavor="wellbeing-p1", unit="bpm",
           note="P1 opt-in numerics — non-diagnostic radar estimates"),
    entity("sensor", "heart_rate", "Heart rate", "state", flavor="wellbeing-p1", unit="bpm",
           note="P1 opt-in numerics — non-diagnostic radar estimates"),
    entity("update", "firmware", "Firmware", "update/state", cmd_topic="update/cmd", device_class="firmware"),
    entity("button", "identify", "Identify", "identify/set", device_class="identify", category="config"),
    entity("switch", "auto_update", "Auto Update", "update/auto", cmd_topic="update/auto/cmd", category="config"),
]
N_DEFAULT = sum(1 for e in ENTITIES if e["flavor"] == "default")
N_WELLBEING = len(ENTITIES)

MQTT = {
    "prefix": "securacv",
    "topic_pattern": "securacv/<device_id>/<suffix>",
    "broker_uri": f"mqtt://{EX_BROKER}:1883",
    "lwt": {"topic": "securacv/<id>/status", "payload": '{"device_id":"…","device_type":"canary-sense","status":"offline","ts_ms":0}',
            "note": "retained Last-Will; on connect the device replaces it with a full online status"},
    "topics": TOPICS,
    "subscribed": SUBSCRIBED,
    "offline_note": "no broker? the witness keeps sensing and chaining — every transition still advances the NVS-persisted hash chain; an outage shows up as a jump in seq/chain length, never lost tamper evidence",
    "discovery": {
        "prefix": grab_str(CFG_DEFAULT, "CS_HA_DISCOVERY_PREFIX"),
        "config_topic": "homeassistant/<component>/<device_id>/<object_id>/config",
        "device": {"ids": ["securacv_<id>"], "name": "SecuraCV Canary Sense <id>",
                   "mf": MANUFACTURER, "mdl": MODEL, "sw": FW_VERSION},
        "entities": ENTITIES,
        "counts": {"default": f"{N_DEFAULT} entities", "wellbeing": f"{N_WELLBEING} entities (13 + breathing lock + 2 P1 BPM)"},
        "gating_note": "BPM entities are provably absent from a presence-only build — their discovery payloads are compiled out, not hidden",
        "note": "the device announces its own entities the instant it connects — no YAML, no manual entities",
    },
}
must(DISC_CPP, "#ifdef CANARY_SENSE_VITALS", "vitals discovery gate")
must(DISC_CPP, "FEATURE_VITALS_BPM_P1", "P1 gate")

# --------------------------------------------------------------------------- #
# 7. use cases (design doc §2.1) + the capability table
# --------------------------------------------------------------------------- #

USE_CASES = [
    {"title": "Restricted-zone witness", "where": "workshop, server closet, storage cage",
     "how": "ceiling/wall mount; the presence FSM emits signed PresenceInRestrictedZone claims with coarse time buckets",
     "why": "works in total darkness where a camera can't, and indoors where WiFi-CSI is noisy — HVAC airflow doesn't affect radar"},
    {"title": "After-hours corroboration", "where": "paired with a camera / canary-vision zone",
     "how": "radar presence + camera person-detection in the same time bucket = a two-physics corroborated event",
     "why": "much stronger evidentiary weight — and a contradiction (camera says person, radar says empty) surfaces as an anomaly worth flagging"},
    {"title": "Wellbeing / welfare check", "where": "elder care, lone-worker — bedside, ≤1.5 m",
     "how": "'breathing confirmed within bucket' as a P0 binary; BPM numerics under P1 opt-in",
     "why": "witnessing that a person is alive and present without a camera in a bedroom — a deployment the vision canary can never ethically serve"},
    {"title": "Tamper enrichment", "where": "every canary-sense node, for free",
     "how": "BH1750 lux: a sudden lights-out while presence persists feeds the tamper-type sensors",
     "why": "the radar keeps seeing when someone kills the lights — the two signals disagree exactly when something is wrong"},
    {"title": "Sleep / quiet-hours baseline", "where": "bedrooms, quiet zones",
     "how": "presence + breathing-lock scalars feed the existing anomaly-baseline pattern",
     "why": "unusual-motion / unusual-breathing dashboards with zero new infrastructure"},
]
for needle in ("Restricted-zone presence witness", "After-hours occupancy corroboration",
               "Wellbeing / welfare-check canary", "Tamper enrichment",
               "Sleep/quiet-hours anomaly baseline"):
    must(DESIGN, needle, f"use case {needle!r}")

CAPABILITIES = [
    {"cap": "Works in darkness", "vision": "poor", "wap": "yes", "sense": "yes"},
    {"cap": "Through blankets / light cover", "vision": "no", "wap": "partially", "sense": "yes"},
    {"cap": "Static (sleeping) presence", "vision": "no", "wap": "weak", "sense": "yes, up to ~6 m"},
    {"cap": "Breathing rate", "vision": "no", "wap": "best-effort (CSI FFT)", "sense": "~90% accuracy, ≤1.5 m"},
    {"cap": "Heart rate", "vision": "no", "wap": "no", "sense": "~85% accuracy, ≤1.5 m"},
    {"cap": "Target count", "vision": "bbox count", "wap": "aggregate only", "sense": "yes (0 / 1 / 2+)"},
    {"cap": "Identity surface at sensor", "vision": "image exists on module", "wap": "none", "sense": "none"},
    {"cap": "Affected by light / temp / dust", "vision": "yes (camera)", "wap": "no", "sense": "no"},
]
must(DESIGN, "fail independently", "multi-witness corroboration")

# --------------------------------------------------------------------------- #
# 8. placement + tuning — the max-capability playbook.
#     In-repo claims are drift-gated above/here; numbers that come from the
#     Seeed wiki / ESPHome docs / community deployments carry a `src` label
#     the page renders. Source key:
#       repo      — this repository (firmware constants, design doc, enclosure)
#       seeed     — Seeed Studio MR60BHA2 wiki / datasheet
#       esphome   — ESPHome seeed_mr60bha2 component docs
#       community — Home Assistant forum / maker deployments
# --------------------------------------------------------------------------- #

must(DESIGN, "thin, flat, uniform membrane", "radome membrane rule")
must(DESIGN, "60 GHz radar transparency demands it", "radome transparency rationale")
must(DESIGN, "ceiling mount, 2.4–3.1 m, facing down", "FDA2 ceiling spec")
must(DESIGN, "do zone gating host-side from distance/range-band", "host-side zone gating")

PLACEMENT = {
    "mounts": [
        {"id": "wall", "name": "Wall / shelf — the presence workhorse",
         "height": "1.2–2 m high, radar face level and square to the room",
         "aim": "point the antenna face at the zone you care about; the sector covers most of a room from a corner",
         "range": "static presence: 6 m on the spec sheet, ~4 m effective per Seeed's own v1.6.12 troubleshooting doc — plan zones on 4",
         "best_for": "restricted zones, after-hours rooms, hallways",
         "src": "seeed"},
        {"id": "bedside", "name": "Over the bed — Seeed's official vitals install",
         "height": "1 m above the head of the bed, tilted 45° down toward the middle of the bed",
         "aim": "radar-to-chest ≤1.5 m, boresight at the torso; the vitals CORE zone is 0.5–1.5 m (outside it readings freeze or zero). Seeed recommends vitals during sleep only — not at a desk, not during exercise",
         "range": "tracking >96% at 0.5–1.5 m, >90% at 1.5–3 m, degraded 3–5 m",
         "best_for": "wellbeing builds — the welfare-check deployment; the repo ships a printable bedside stand",
         "src": "seeed"},
        {"id": "ceiling", "name": "Ceiling — the fall-detection sibling's home",
         "height": "2.2–3.0 m, facing straight down, ~2 m sensing radius",
         "aim": "that spec belongs to the MR60FDA2 fall kit (same protocol family, different radar firmware — cross-flashing bricks it); the BHA2 prefers wall/bedside",
         "range": "—",
         "best_for": "noted here so you don't ceiling-mount a BHA2 expecting FDA2 behavior",
         "src": "seeed"},
    ],
    "radome": {
        "rule": "anything in front of the antenna must be a thin, flat, uniform, non-metal membrane — no ribs, no labels, no paint blobs over the antenna zone",
        "why": "60 GHz has ~5 mm wavelength; ribs and thickness steps in front of the antenna refract and reflect the beam",
        "repo_note": "the repo's printable RADOME enclosure computes and asserts the membrane air gap in the OpenSCAD model — print it in plain PLA/PETG, never with metallic/carbon-fill filament",
        "src": "repo",
    },
    "geometry": {
        "fov_deg": 80,
        "presence_max_m": 6.0,
        "vitals_max_m": 1.5,
        "vitals_core_m": [0.5, 1.5],
        "near_cm": FSM["presence"]["near_cm"],
        "mid_cm": FSM["presence"]["mid_cm"],
        "note": "the near/mid/far bands are the firmware's own range gates (host-side zone gating) — raw centimeters never leave the device",
        "fov_flag": "Seeed's own numbers disagree: the mmWave comparison table says 120°×100° (presence) while the module datasheet says an 80°×80° sector (vitals). The lab below draws the conservative 80° — treat the extra width as bonus, never as budget",
    },
    "avoid": [
        # Seeed's official interference list (getting-started wiki, note block)
        {"what": "fans, A/C, swaying curtains and plants", "why": "micro-movement is motion to FMCW radar — officially acknowledged, and the #1 community false-positive; wind through a window makes curtains a permanent occupant", "src": "seeed"},
        {"what": "large metal surfaces and mirrors in-beam", "why": "radar mirrors — reflections fold ghost targets (or the next room) into your zone", "src": "seeed"},
        {"what": "glass or thin wooden panels between radar and room", "why": "Seeed explicitly says do not rely on through-glass/through-panel detection; at 60 GHz (an O₂ absorption band) every wall is a hard boundary — which is also why it never leaks into the neighbor's flat", "src": "seeed"},
        {"what": "another mmWave radar installed close by", "why": "two 60 GHz emitters raise each other's noise floor — officially listed interference", "src": "seeed"},
        {"what": "vibrating mounts and flowing water", "why": "the radar can't tell its own motion from the room's; water films and flow scatter the beam — both on Seeed's official list", "src": "seeed"},
        {"what": "low-quality USB power supplies", "why": "on Seeed's official interference list — a noisy 5 V rail degrades the radar front-end; budget 5 V/1 A clean", "src": "seeed"},
    ],
}

TUNING = {
    "goal": "maximum detection, minimum error — every knob below is a LIVE host-side number now (the radar module itself stays a black box): NVS-backed, tunable from Home Assistant's number entities (cfg/*/set), and bakeable as a room preset in the browser flasher. The compiled value seeds the first boot.",
    "knobs": [
        {"name": "present_debounce_ms", "value": D["debounce_ms"],
         "does": "how long a target must persist before Present fires",
         "raise_when": "a doorway clip or a passing pet flickers presence — 500–800 ms ignores sub-second transients",
         "lower_when": "you need instant hallway reaction and can tolerate blips",
         "src": "repo"},
        {"name": "clear_timeout_ms", "value": D["clear_ms"],
         "does": "how long with no target before Clear fires",
         "raise_when": "a sitting person occasionally drops out — 5–10 s hold keeps still-presence latched (the classic mmWave 'holds through stillness' advantage)",
         "lower_when": "you want fast vacancy for lighting automations",
         "src": "repo"},
        {"name": "stall_timeout_ms", "value": D["stall_ms"],
         "does": "no UART frame at all before the state goes Unknown and the radar_link problem sensor trips",
         "raise_when": "never much — 5 s already tolerates parser resync bursts",
         "lower_when": "you want faster hardware-fault alarms",
         "src": "repo"},
        {"name": "near_cm / mid_cm", "value": f"{D['near_cm']} / {D['mid_cm']}",
         "does": "the range-band gates — the host-side stand-in for in-radar zone config",
         "raise_when": "your zone of interest is deep (a long room): push mid_cm out so 'far' means 'outside my zone'",
         "lower_when": "you only care about close approach (a cabinet, a doorway)",
         "src": "repo"},
        {"name": "vitals lock/lost (wellbeing)", "value": f"{FSM['vitals']['lock_ms']} / {FSM['vitals']['lost_ms']} ms",
         "does": "sustained plausible vitals before the breathing lock confirms; loss window before it drops",
         "raise_when": "restless sleepers flap the lock — a longer lost window rides through position changes",
         "lower_when": "clinical-adjacent alerting where you want fast loss-of-signal notice (remember: non-diagnostic)",
         "src": "repo"},
        {"name": "plausibility bands", "value": f"breath {FSM['vitals']['breath_bpm'][0]}–{FSM['vitals']['breath_bpm'][1]}, heart {FSM['vitals']['heart_bpm'][0]}–{FSM['vitals']['heart_bpm'][1]} bpm",
         "does": "BPM outside these bands is rejected as noise before it can reach the lock FSM",
         "raise_when": "—", "lower_when": "—",
         "src": "repo"},
    ],
    "errors": [
        {"kind": "false-positive", "cause": "pets",
         "reality": "60 GHz radar sees any moving mass — a cat IS a target, and the module exposes no pet-height zone or sensitivity config at all (closed firmware). A cat sleeping inside the 1.5 m vitals bubble can even produce vitals readings — small-animal breathing is within the detectable band.",
         "fix": "placement + physics: mount high aiming level so a floor cat skims the cone's edge, gate on range band, and corroborate high-stakes automations with a second signal (lux, contact, a CSI Canary). The firmware's plausibility bands reject most feline heart rates from locking as a human.",
         "src": "community"},
        {"kind": "false-positive", "cause": "fan / HVAC / curtain",
         "reality": "officially acknowledged, with a documented signature: fans can make the radar report a NON-ZERO HEART RATE while breathing reads 0 and no target is detected — a phantom pulse with no lungs",
         "fix": "aim the cone away first; then trust vitals only when presence is on AND breathing > 0 — the firmware's plausibility gate plus the sit-still lock already encode that sanity filter",
         "src": "seeed"},
        {"kind": "false-positive", "cause": "reflections (mirror, glass, metal)",
         "reality": "presence from the hallway folded into the bedroom via a mirror is a classic; ghost targets at impossible ranges are the tell",
         "fix": "re-aim so no large specular surface is in-beam; range-band gating trims ghosts beyond your zone",
         "src": "community"},
        {"kind": "false-negative", "cause": "very still person at range",
         "reality": "official tracking numbers: >96% at 0.5–1.5 m, >90% at 1.5–3 m, occasional target loss at 3–5 m. Radar-module firmware matters: v1.6.12 fixed stationary-target loss inside 1.5 m — the #1 'presence drops while I sit still' fix is a module firmware update",
         "fix": "one device per ~4 m zone (the effective figure), keep the radar firmware current, and let clear_timeout ride through brief dropouts",
         "src": "seeed"},
        {"kind": "false-negative", "cause": "occlusion by dense/metal objects",
         "reality": "60 GHz penetrates blankets usefully, but a metal shelf is a wall — and Seeed says don't rely on detection through even glass or thin wood",
         "fix": "mount above furniture lines; never behind a TV, appliance, or glass door",
         "src": "seeed"},
        {"kind": "wrong-value", "cause": "vitals with 2+ people in the cone",
         "reality": "the radar reports ONE breath/heart estimate — with two people it's an attribution lottery. Seeed's own validity conditions demand exactly one, completely still person; occupant counting (up to 3) is officially 'experimental… a rough estimation'",
         "fix": "the firmware refuses to play: vitals are hard-suppressed unless the count bucket is exactly 1 (a code rule, not advice)",
         "src": "repo"},
        {"kind": "wrong-value", "cause": "stale vitals beyond the core zone",
         "reality": "the documented trap: beyond ~1.5 m the module FREEZES breath/heart at the last valid value rather than nulling — a dashboard can show a heartbeat for an empty chair. (Stock ESPHome also skips zero-valued frames, compounding it.)",
         "fix": "canary-sense publishes BPM as null the moment the lock drops — the freeze can't reach Home Assistant. On stock firmware, gate vitals on distance 50–150 cm and target count == 1 yourself",
         "src": "seeed"},
        {"kind": "wrong-value", "cause": "heart rate optimism",
         "reality": "the 85% claim deserves context: community verdict is 'trend indicator, not a monitor' — readings rarely leave the 60–100 bpm band, one forum bench measured breathing consistently +4 BPM high, and Seeed's own v1.6.12 notes admit the vitals algorithm 'had fundamental issues' pending an ML rework",
         "fix": "treat BPM as a wellbeing trend (the firmware already labels it non-diagnostic, P1-gated, never sealed-logged); the breathing LOCK binary is the signal to build on",
         "src": "community"},
    ],
    "playbook": [
        "Pick the job first: presence witness (wall, whole room) or wellbeing (over the bed, 0.5–1.5 m core zone). One device rarely does both well from one spot.",
        "Mount square and solid: level antenna face, rigid mount, clean 5 V/1 A power, nothing in front but the radome membrane.",
        "Update the radar module firmware to current (v1.6.12 fixed stationary-target loss) — it's a separate firmware from the host's, flashed once over a UART passthrough.",
        "Walk the cone: use the lab pattern below on the real device — walk the room edges watching range bands flip; what you can't flip, the radar can't see.",
        "Kill the movers: fans, curtains, dangling plants out of beam before touching any threshold.",
        "Then tune debounce/clear to your room's rhythm — raw radar asserts in ~1 s and clears in ~5 s; the firmware's debounce/clear knobs shape that, and thresholds are for the last 10%, not the first 90%.",
        "For automations that act (locks, alerts), corroborate: radar + lux, radar + contact, or two Canaries of different physics.",
    ],
    "reality": {
        "title": "The reality check — what the spec sheet won't tell you",
        "flags": [
            {"claim": "presence to 6 m", "reality": "Seeed's v1.6.12 troubleshooting doc says ~4 m effective; one reviewer measured useful distance readings capping at 2–3 m", "src": "seeed"},
            {"claim": "80°×80° field of view", "reality": "Seeed's comparison table says 120°×100°, the datasheet says 80°×80° — the numbers disagree; design for 80°", "src": "seeed"},
            {"claim": "heart rate 85% accurate", "reality": "band-limited ~60–100 bpm in practice; 'entertainment-grade' is the community verdict — a trend, not telemetry", "src": "community"},
            {"claim": "breathing 90% accurate", "reality": "usable as a trend; one metronome-controlled bench measured a consistent +4 BPM offset", "src": "community"},
            {"claim": "occupant count", "reality": "counts up to 3, officially experimental — which is exactly why the firmware buckets it to 0/1/2+ and never publishes a precise count", "src": "seeed"},
        ],
    },
    "sources_note": "repo = drift-gated firmware constants + design doc (CI-verified) · seeed = Seeed MR60BHA2 wiki, datasheet, and v1.6.12 troubleshooting doc · community = Home Assistant / ESPHome deployment reports and reviews. External claims are labeled so you know what to re-verify against your own room.",
    "links": [
        {"name": "Seeed: getting started with the MR60BHA2 kit", "url": "https://wiki.seeedstudio.com/getting_started_with_mr60bha2_mmwave_kit/"},
        {"name": "Seeed: Home Assistant + MR60BHA2 (incl. the troubleshooting tables)", "url": "https://wiki.seeedstudio.com/ha_with_mr60bha2/"},
        {"name": "ESPHome seeed_mr60bha2 component", "url": "https://esphome.io/components/seeed_mr60bha2/"},
        {"name": "Seeed Arduino mmWave library", "url": "https://github.com/Love4yzp/Seeed-mmWave-library"},
    ],
}

# --------------------------------------------------------------------------- #
# 9. sandbox scenarios — every effect traces to a real firmware signal path
# --------------------------------------------------------------------------- #

SANDBOX = [
    {"id": "walk", "label": "Walk into the room",
     "blurb": f"A target sustains past the {D['debounce_ms']} ms debounce → Present; the LED goes green and a signed event chains.",
     "state": "Present", "led": "green", "event": "presence_detected",
     "serial": "[presence] -> present",
     "mqtt": [{"suffix": "events", "payload": '{"event":"presence_detected","presence":"present","occupants":"1","range":"mid","signed":true}'},
              {"suffix": "state", "payload": '{"presence":true,"occupants":"1","range":"mid"}'},
              {"suffix": "chain", "payload": '{"length":+1}'}],
     "ha": "binary_sensor.<id>_presence -> ON"},
    {"id": "approach", "label": "Walk toward it",
     "blurb": "Range band steps far → mid → near as you close in — the host-side zone gate in action; raw centimeters never publish.",
     "state": "Present", "led": "green", "event": None,
     "serial": None,
     "mqtt": [{"suffix": "state", "payload": '{"presence":true,"range":"near"}'}],
     "ha": "sensor.<id>_range_band: far → mid → near"},
    {"id": "sit", "label": "Sit still and breathe (wellbeing)",
     "blurb": f"Plausible vitals sustain {FSM['vitals']['lock_ms']} ms with exactly one target → the breathing lock confirms; BPM numerics go live (P1).",
     "state": "Present", "led": "green", "event": None,
     "serial": "[vitals] breathing locked",
     "mqtt": [{"suffix": "state", "payload": '{"presence":true,"breathing_locked":true,"breath_bpm":14,"heart_bpm":68}'}],
     "ha": "binary_sensor.<id>_breathing -> ON · sensors read 14 / 68 bpm"},
    {"id": "second", "label": "A second person walks in",
     "blurb": "The count bucket moves 1 → 2+; occupancy_changed chains, and vitals hard-suppress (BPM → null) — attribution refused, by code.",
     "state": "Present", "led": "green", "event": "occupancy_changed",
     "serial": "[vitals] breathing lost",
     "mqtt": [{"suffix": "events", "payload": '{"event":"occupancy_changed","occupants":"2+","signed":true}'},
              {"suffix": "state", "payload": '{"occupants":"2+","breathing_locked":false,"breath_bpm":null,"heart_bpm":null}'},
              {"suffix": "chain", "payload": '{"length":+1}'}],
     "ha": "sensor.<id>_occupants -> 2+ · BPM entities -> unknown"},
    {"id": "leave", "label": "Everyone leaves",
     "blurb": f"No target for {D['clear_ms']} ms → Clear; the LED goes blue and presence_cleared chains.",
     "state": "Clear", "led": "blue", "event": "presence_cleared",
     "serial": "[presence] -> clear",
     "mqtt": [{"suffix": "events", "payload": '{"event":"presence_cleared","presence":"clear","occupants":"0","signed":true}'},
              {"suffix": "state", "payload": '{"presence":false,"occupants":"0"}'},
              {"suffix": "chain", "payload": '{"length":+1}'}],
     "ha": "binary_sensor.<id>_presence -> OFF"},
    {"id": "lights", "label": "Kill the lights — with someone inside",
     "blurb": "Lux collapses while radar presence persists: the tamper-corroboration pattern. Camera-blind means nothing to a radar.",
     "state": "Present", "led": "green", "event": None,
     "serial": None,
     "mqtt": [{"suffix": "state", "payload": '{"presence":true,"lux":1.0}'}],
     "ha": "lights-out + presence — the tamper automation's trigger pair"},
    {"id": "stall", "label": "Unplug the radar UART",
     "blurb": f"No frame for {D['stall_ms']} ms → Unknown (amber LED); the radar_link problem sensor trips. Health, not a witness event — silence is never evidence.",
     "state": "Unknown", "led": "amber", "event": None,
     "serial": "[presence] -> unknown (radar stall)",
     "mqtt": [{"suffix": "state", "payload": '{"radar_ok":false,"presence":false}'}],
     "ha": "binary_sensor.<id>_radar_link -> ON (problem)"},
    {"id": "identify", "label": "Press Identify in Home Assistant",
     "blurb": "The WS2812 flashes white at 2 Hz for 10 s and the identify echo mirrors the window — the 'which device is which' moment.",
     "state": None, "led": "white", "event": None,
     "serial": "[identify] flashing LED for 10 s",
     "mqtt": [{"suffix": "identify", "payload": '"on"'}],
     "ha": "button.<id>_identify pressed"},
]
must(MAIN_CPP, '"[identify] flashing LED for 10 s"', "identify serial line")
for sc in SANDBOX:
    for pub in sc.get("mqtt", []):
        if pub["suffix"] not in [t["suffix"] for t in TOPICS]:
            die(f"sandbox {sc['id']} publishes unknown topic {pub['suffix']!r}")

# --------------------------------------------------------------------------- #
# 10. assemble + write
# --------------------------------------------------------------------------- #

out = {
    "$note": "GENERATED by canary-local/tools/gen_sense.py from the canary-sense firmware + docs/registry. Do not edit by hand; run the generator. Drift-gated in .github/workflows/canary-local.yml.",
    "generated_by": "canary-local/tools/gen_sense.py",
    "device": {
        "id_example": EX_ID,
        "fp_example": EX_FP,
        "host_example": EX_HOST,
        "hwid_example": EX_HWID,
        "name": sense_reg.get("name", "Canary Sense"),
        "product_name": "SecuraCV Canary Sense",
        "device_type": DEVICE_TYPE,
        "tagline": sense_reg.get("tagline", ""),
        "model": MODEL,
        "model_wellbeing": MODEL_WELLBEING,
        "board": BOARD_NAME,
        "board_short": "XIAO ESP32C6",
        "board_id": BOARD_ID,
        "fw_version": FW_VERSION,
        "fw_train": FW_TRAIN,
        "ota_product": OTA_PRODUCT,
        "senses": sense_reg.get("senses", []),
        "modality": sense_reg.get("modality", "radar"),
        "network": sense_reg.get("network", {}),
        "status": sense_reg.get("status", ""),
        "docs": sense_reg.get("docs", []),
    },
    "radar": RADAR,
    "fsm": FSM,
    "events": EVENTS,
    "provisioning": PROVISIONING,
    "serial": SERIAL,
    "mqtt": MQTT,
    "use_cases": USE_CASES,
    "capabilities": CAPABILITIES,
    "placement": PLACEMENT,
    "tuning": TUNING,
    "sandbox": SANDBOX,
    "docs": {
        "design": "docs/canary_sense_mr60bha2_design.md",
        "firmware_readme": "firmware/projects/canary-sense/README.md",
        "registry": "canary-local/devices/registry.json",
        "enclosure": "docs/hardware/enclosure/canary_sense_enclosure.scad",
    },
}

# sanity floors — a broken parse must fail the build, not ship thin data
if len(ENTITIES) < 14:
    die(f"only {len(ENTITIES)} HA entities — parse likely broke")
if len(RADAR["protocol"]["frames"]) != 5:
    die("expected exactly 5 radar frame types")
if len(SANDBOX) < 6:
    die("sandbox too thin")
if len(TUNING["knobs"]) < 5 or len(TUNING["errors"]) < 6:
    die("tuning section parsed thin")
if len(BANNER) < 20 or len(BOOT) < 10:
    die("serial log too short — parse likely broke")

OUT_JSON.write_text(json.dumps(out, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
print(f"wrote {OUT_JSON.relative_to(REPO)}  "
      f"({len(ENTITIES)} entities, {len(TOPICS)} topics, {len(BANNER) + len(BOOT)} boot lines, "
      f"{len(SANDBOX)} sandbox scenarios, fw {FW_VERSION})")
