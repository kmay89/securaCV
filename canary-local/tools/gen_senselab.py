#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_senselab.py — build canary-local/devices/senselab.json from the canary-sense
firmware + the MR60BHA2 hardware notes.

The Sense Lab (`canary-local/senselab.html`) stages the radar witness: the real
UART frame grammar, the real presence/vitals FSM semantics (ported line-for-
line in assets/sense-sim.js), the privacy chokepoint, placement physics and
the power model. None of it is hand-faked: every constant the page shows is
either parsed straight out of the firmware/docs, or authored here and then
*validated to still exist in the source* (this file `sys.exit(1)`s on drift).
CI re-runs the generator and `git diff --exit-code`s `senselab.json`, the same
anti-rot contract as gen_wap.py / gen_homeassistant.py / gen_boards.py.

Sources of truth (all in-repo, deterministic, offline):
  firmware/common/sensors/mmwave_mr60/mr60_uart.h    wire protocol + [BENCH]
  firmware/configs/canary-sense/{default,wellbeing}/config.h   CS_* tunables
  firmware/boards/xiao-esp32c6-mr60/pins/pins.h      pin map
  firmware/projects/canary-sense/include/canary/{version,topics,types}.h
  firmware/projects/canary-sense/src/ha/ha_discovery.cpp   HA entity set
  firmware/projects/canary-sense/src/main.cpp        chokepoint vocabulary
  firmware/common/identity/device_signature.h        the `sense` canonical
  firmware/envs/platformio/canary-sense.ini          build flavors
  docs/hardware/mr60bha2_radar_notes.md              SIM: physics + power
  canary-local/devices/registry.json                 fw_train + device card

Run:  python3 canary-local/tools/gen_senselab.py
"""

import json
import re
from pathlib import Path

from _tooling import die, repo_root

REPO = repo_root()
FW = REPO / "firmware"
MR60_H = FW / "common/sensors/mmwave_mr60/mr60_uart.h"
PRESENCE_H = FW / "common/sensors/mmwave_mr60/mr60_presence.h"
VITALS_H = FW / "common/sensors/mmwave_mr60/mr60_vitals.h"
CFG_DEFAULT = FW / "configs/canary-sense/default/config.h"
CFG_WELLBEING = FW / "configs/canary-sense/wellbeing/config.h"
PINS_H = FW / "boards/xiao-esp32c6-mr60/pins/pins.h"
PROJ = FW / "projects/canary-sense"
VERSION_H = PROJ / "include/canary/version.h"
TOPICS_H = PROJ / "include/canary/topics.h"
TYPES_H = PROJ / "include/canary/types.h"
MAIN_CPP = PROJ / "src/main.cpp"
HA_CPP = PROJ / "src/ha/ha_discovery.cpp"
SIG_H = FW / "common/identity/device_signature.h"
ENV_INI = FW / "envs/platformio/canary-sense.ini"
NOTES_MD = REPO / "docs/hardware/mr60bha2_radar_notes.md"
CARDS_MD = REPO / "docs/standard/CANARY_CARDS.md"
REGISTRY = REPO / "canary-local/devices/registry.json"
OUT_JSON = REPO / "canary-local/devices/senselab.json"

# --------------------------------------------------------------------------- #
# helpers (the gen_wap.py toolkit)
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
        die(f"{label}: expected to find {needle!r} in {path.relative_to(REPO)} — source changed?")


def grab(path: Path, pattern: str, label: str, flags=0) -> str:
    m = re.search(pattern, read(path), flags)
    if not m:
        die(f"{label}: pattern /{pattern}/ not found in {path.relative_to(REPO)}")
    return m.group(1)


def grab_int(path: Path, pattern: str, label: str) -> int:
    return int(grab(path, pattern, label))


# --------------------------------------------------------------------------- #
# 1. identity + version
# --------------------------------------------------------------------------- #

FW_VERSION = grab(VERSION_H, r'CANARY_FW_VERSION\s+"([^"]+)"', "CANARY_FW_VERSION")
DEVICE_TYPE = grab(CFG_DEFAULT, r'CS_DEVICE_TYPE\s+"([^"]+)"', "CS_DEVICE_TYPE")
MODEL_DEFAULT = grab(CFG_DEFAULT, r'CS_MODEL\s+"([^"]+)"', "CS_MODEL (default)")
MODEL_WELLBEING = grab(CFG_WELLBEING, r'CS_MODEL\s+"([^"]+)"', "CS_MODEL (wellbeing)")
BOARD_NAME = grab(PINS_H, r'BOARD_NAME\s+"([^"]+)"', "BOARD_NAME")

registry = json.loads(read(REGISTRY))
FW_TRAIN = registry.get("fw_train")
if not FW_TRAIN:
    die("registry.json has no fw_train")
if not FW_VERSION.startswith(FW_TRAIN):
    die(f"firmware version {FW_VERSION!r} does not ride registry fw_train {FW_TRAIN!r}")

sense_reg = next((d for d in registry.get("devices", []) if d.get("id") == "canary-sense"), None)
if not sense_reg:
    die("registry.json has no canary-sense device entry")

# --------------------------------------------------------------------------- #
# 2. pins (parsed from the board def)
# --------------------------------------------------------------------------- #

PINS = {
    "radar_tx": grab_int(PINS_H, r"RADAR_UART_TX\s+(\d+)", "RADAR_UART_TX"),
    "radar_rx": grab_int(PINS_H, r"RADAR_UART_RX\s+(\d+)", "RADAR_UART_RX"),
    "baud": grab_int(PINS_H, r"RADAR_UART_BAUD\s+(\d+)", "RADAR_UART_BAUD"),
    "uart_num": grab_int(PINS_H, r"RADAR_UART_NUM\s+(\d+)", "RADAR_UART_NUM"),
    "sda": grab_int(PINS_H, r"I2C_PIN_SDA\s+(\d+)", "I2C_PIN_SDA"),
    "scl": grab_int(PINS_H, r"I2C_PIN_SCL\s+(\d+)", "I2C_PIN_SCL"),
    "bh1750_addr": grab(PINS_H, r"BH1750_I2C_ADDR\s+(0x[0-9A-Fa-f]+)", "BH1750_I2C_ADDR"),
    "led_pin": grab_int(PINS_H, r"LED_WS2812_PIN\s+(\d+)", "LED_WS2812_PIN"),
    "boot_pin": grab_int(PINS_H, r"BOOT_BUTTON_PIN\s+(\d+)", "BOOT_BUTTON_PIN"),
}

# --------------------------------------------------------------------------- #
# 3. wire protocol (parsed from mr60_uart.h)
# --------------------------------------------------------------------------- #

SOF = grab(MR60_H, r"MR60_SOF\s*=\s*(0x[0-9A-Fa-f]+)", "MR60_SOF")
HEADER_LEN = grab_int(MR60_H, r"MR60_HEADER_LEN\s*=\s*(\d+)", "MR60_HEADER_LEN")
MAX_PAYLOAD = grab_int(MR60_H, r"MR60_MAX_PAYLOAD\s*=\s*(\d+)", "MR60_MAX_PAYLOAD")
FRAME_QUEUE = grab_int(MR60_H, r"MR60_FRAME_QUEUE\s*=\s*(\d+)", "MR60_FRAME_QUEUE")

FRAME_TYPES = []
for const, name, payload in [
    ("MR60_TYPE_PEOPLE_EXIST", "PEOPLE_EXIST", "uint16 LE has_target (nonzero == present)"),
    ("MR60_TYPE_TARGET_COUNT", "TARGET_COUNT", "uint32 LE target count"),
    ("MR60_TYPE_DISTANCE", "DISTANCE", "byte[0] valid flag, float32 LE @ [4..7]"),
    ("MR60_TYPE_BREATH_RATE", "BREATH_RATE", "float32 LE breaths/min"),
    ("MR60_TYPE_HEART_RATE", "HEART_RATE", "float32 LE beats/min"),
]:
    hexid = grab(MR60_H, const + r"\s*=\s*(0x[0-9A-Fa-f]+)", const)
    FRAME_TYPES.append({"type_hex": hexid, "name": name, "payload": payload})

# The [BENCH] assumptions, quoted from the header (drift-gated verbatim).
bench_lines = re.findall(r"\[BENCH\]\s+(.+?)(?=\n \*(?:\s+\[BENCH\]|/|\s*$)|\n \*\s*\n)",
                         read(MR60_H), re.S)
BENCH = []
for b in bench_lines:
    BENCH.append(re.sub(r"\s*\n \*\s*", " ", b).strip())
if len(BENCH) < 3:
    die(f"expected >=3 [BENCH] notes in mr60_uart.h, found {len(BENCH)}")

# Frames the module speaks that we deliberately drop (documented in the notes
# doc §2; validated to still be documented there).
DROPPED_FRAMES = [
    {"type_hex": "0x0A13", "name": "PHASE_WAVEFORM",
     "payload": "3x float32 total/breath/heart phase",
     "why": "the chest-displacement waveform itself — scalars only past the chokepoint"},
    {"type_hex": "0x0A08", "name": "POINT_CLOUD",
     "payload": "uint32 count + per-target x, y, doppler, cluster",
     "why": "per-target trajectories are forbidden by design (§2.2)"},
    {"type_hex": "0xFFFF", "name": "RADAR_FW_VERSION",
     "payload": "uint32 {project, major, sub, modified}",
     "why": "not decoded yet — bench flag 6 proposes wiring it to the health log"},
]
for f in DROPPED_FRAMES:
    must(NOTES_MD, f["type_hex"], f"dropped frame {f['name']} documented in notes doc")

must(MR60_H, "unknown_count", "unknown-frame counter exists in the parser")

# --------------------------------------------------------------------------- #
# 4. FSM tunables (CS_* from both flavors; wellbeing carries the vitals set)
# --------------------------------------------------------------------------- #


def cs_int(path: Path, name: str) -> int:
    return grab_int(path, name + r"\s+(\d+)", name)


PRESENCE_CFG = {
    "present_debounce_ms": cs_int(CFG_DEFAULT, "CS_PRESENT_DEBOUNCE_MS"),
    "clear_timeout_ms": cs_int(CFG_DEFAULT, "CS_CLEAR_TIMEOUT_MS"),
    "stall_timeout_ms": cs_int(CFG_DEFAULT, "CS_RADAR_STALL_MS"),
    "near_cm": cs_int(CFG_DEFAULT, "CS_RANGE_NEAR_CM"),
    "mid_cm": cs_int(CFG_DEFAULT, "CS_RANGE_MID_CM"),
}
# the wellbeing flavor must carry the same presence tuning (one witness story)
for k, macro in [
    ("present_debounce_ms", "CS_PRESENT_DEBOUNCE_MS"),
    ("clear_timeout_ms", "CS_CLEAR_TIMEOUT_MS"),
    ("stall_timeout_ms", "CS_RADAR_STALL_MS"),
    ("near_cm", "CS_RANGE_NEAR_CM"),
    ("mid_cm", "CS_RANGE_MID_CM"),
]:
    if cs_int(CFG_WELLBEING, macro) != PRESENCE_CFG[k]:
        die(f"{macro} differs between default and wellbeing configs")

VITALS_CFG = {
    "lock_confirm_ms": cs_int(CFG_WELLBEING, "CS_VITALS_LOCK_MS"),
    "lock_lost_ms": cs_int(CFG_WELLBEING, "CS_VITALS_LOST_MS"),
    "breath_min_bpm": cs_int(CFG_WELLBEING, "CS_BREATH_MIN_BPM"),
    "breath_max_bpm": cs_int(CFG_WELLBEING, "CS_BREATH_MAX_BPM"),
    "heart_min_bpm": cs_int(CFG_WELLBEING, "CS_HEART_MIN_BPM"),
    "heart_max_bpm": cs_int(CFG_WELLBEING, "CS_HEART_MAX_BPM"),
}

# the struct defaults in the library headers must match the shipped configs
# (mr60_presence.h documents the same values as its defaults)
for k, v in PRESENCE_CFG.items():
    must(PRESENCE_H, str(v), f"PresenceConfig default for {k}")
for k, v in VITALS_CFG.items():
    must(VITALS_H, str(v), f"VitalsConfig default for {k}")

# FSM semantics the page narrates — validated to still exist in the source.
SEMANTICS = [
    ("deadline-first stall safety", MR60_H.parent / "mr60_presence.cpp", "DEADLINE FIRST"),
    ("no vitals unless exactly one target", MR60_H.parent / "mr60_vitals.cpp",
     "no vitals unless exactly one target"),
    ("absence zeroes the aggregate", MR60_H.parent / "mr60_uart.cpp",
     "no target zeroes the other scalars"),
    ("resync drops one byte, never the buffer", MR60_H.parent / "mr60_uart.cpp",
     "dropping just the latched SOF byte"),
]
for label, path, needle in SEMANTICS:
    must(path, needle, label)

HEARTBEAT_MS = cs_int(CFG_DEFAULT, "CS_HEARTBEAT_MS")

# --------------------------------------------------------------------------- #
# 5. privacy chokepoint (vocabulary + canonical + bucket, from main.cpp)
# --------------------------------------------------------------------------- #

for word in ['"present"', '"clear"', '"unknown"', '"near"', '"mid"', '"far"', '"2+"']:
    must(MAIN_CPP, word, f"chokepoint vocabulary {word}")

EVENTS = ["presence_detected", "presence_cleared", "occupancy_changed"]
for e in EVENTS:
    must(MAIN_CPP, f'"{e}"', f"witness event {e}")

must(MAIN_CPP, "600UL) * 600UL", "10-minute uptime bucket")
CANONICAL_PREFIX = grab(SIG_H, r'SIG_PREFIX\s*=\s*"([^"]+)"', "SIG_PREFIX")
must(SIG_H, "|sense|", "v1 sense canonical domain")
CANONICAL = (CANONICAL_PREFIX +
             "|v1|sense|<device_id>|<seq>|<event>|<presence>|<occupants>|<range>|<bucket_uptime_s>")

PRIVACY_CLASSES = [
    {"signal": "presence (binary, debounced)", "cls": "P0", "note": "witness claim, signed + chained"},
    {"signal": "occupants bucket 0/1/2+", "cls": "P0", "note": "never a per-target track log"},
    {"signal": "distance", "cls": "P2", "note": "read + dropped; only near/mid/far leaves"},
    {"signal": "breathing lock (binary)", "cls": "P0", "note": "wellbeing builds; a lock, not a number"},
    {"signal": "breath / heart BPM", "cls": "P1", "note": "opt-in entities; never sealed-logged"},
    {"signal": "phase waveforms / point cloud", "cls": "dropped",
     "note": "arrive as unknown frames; never decoded"},
    {"signal": "lux (BH1750)", "cls": "P0", "note": "tamper corroboration"},
]

# --------------------------------------------------------------------------- #
# 6. topics + HA entities (parsed from topics.h / ha_discovery.cpp)
# --------------------------------------------------------------------------- #

topics_src = read(TOPICS_H)
TOPICS = []
for suffix, what, retained in [
    ("events", "presence transitions only — the signed witness stream", False),
    ("state", "full coarse snapshot", True),
    ("status", "availability + health heartbeat", True),
    ("chain", "signed hash-chain head + length", True),
    ("health", "pubkey for HA TOFU-pin + heap/uptime/fw", True),
    ("update/state", "signed pull-OTA state", True),
]:
    frag = suffix.split("/")[0]
    if f'"securacv/%s/{frag}"' not in topics_src and f"/{frag}" not in topics_src:
        die(f"topic suffix {suffix} not found in topics.h")
    TOPICS.append({"suffix": suffix, "what": what, "retained": retained})

ha_src = read(HA_CPP)
ENTITIES = []
for uid, platform, ecls, name, vitals_only, p1_only, diag in [
    ("presence", "binary_sensor", "occupancy", "Presence", False, False, False),
    ("occupants", "sensor", None, "Occupants", False, False, False),
    ("range_band", "sensor", None, "Range band", False, False, True),
    ("radar_link", "binary_sensor", "problem", "Radar link", False, False, True),
    ("frame_errors", "sensor", None, "Frame errors", False, False, True),
    ("illuminance", "sensor", "illuminance", "Illuminance", False, False, False),
    ("last_event", "sensor", None, "Last event", False, False, False),
    ("uptime", "sensor", "duration", "Uptime", False, False, False),
    ("rssi", "sensor", "signal_strength", "RSSI", False, False, True),
    ("heap_free", "sensor", None, "Heap free", False, False, True),
    ("breathing", "binary_sensor", None, "Breathing confirmed", True, False, False),
    ("breath_rate", "sensor", None, "Breath rate", True, True, False),
    ("heart_rate", "sensor", None, "Heart rate", True, True, False),
    ("firmware", "update", "firmware", "Firmware", False, False, False),
    ("identify", "button", "identify", "Identify", False, False, False),
    ("auto_update", "switch", None, "Auto update", False, False, False),
]:
    if f'_{uid}\\"' not in ha_src and f'_{uid}"' not in ha_src:
        die(f"HA entity unique_id suffix _{uid} not found in ha_discovery.cpp")
    ENTITIES.append({
        "id": uid, "platform": platform, "class": ecls, "name": name,
        "vitals_only": vitals_only, "p1_only": p1_only, "diagnostic": diag,
    })

# the P1 gate must really be compile-gated in the discovery source
must(HA_CPP, "CANARY_SENSE_VITALS", "vitals discovery compile gate")
must(HA_CPP, "FEATURE_VITALS_BPM_P1", "P1 BPM discovery gate")

# --------------------------------------------------------------------------- #
# 7. build flavors (from the env file)
# --------------------------------------------------------------------------- #

env_src = read(ENV_INI)
FLAVORS = []
for env, vitals in [("canary-sense-default", False),
                    ("canary-sense-wellbeing", True),
                    ("canary-sense-debug", False)]:
    if f"[env:{env}]" not in env_src:
        die(f"env {env} missing from canary-sense.ini")
    FLAVORS.append({"env": env, "vitals": vitals})
must(ENV_INI, "-DCANARY_SENSE_VITALS=1", "vitals build flag in wellbeing env")
OTA_PRODUCTS = sorted(set(re.findall(r'securacv-canary-sense[a-z-]*', env_src)))

# --------------------------------------------------------------------------- #
# 8. hardware physics + power rails (parsed from the notes doc SIM: tables)
# --------------------------------------------------------------------------- #


def sim_table(heading: str) -> dict:
    """Parse a `### SIM:<name>` markdown table from the notes doc into a dict."""
    src = read(NOTES_MD)
    m = re.search(rf"### SIM:{heading}.*?\n\|[^\n]*\|\n\|[-| ]+\|\n((?:\|[^\n]*\|\n)+)",
                  src, re.S)
    if not m:
        die(f"SIM:{heading} table not found in {NOTES_MD.relative_to(REPO)}")
    out = {}
    for row in m.group(1).strip().splitlines():
        cells = [c.strip() for c in row.strip("|").split("|")]
        if len(cells) < 2:
            continue
        key, val = cells[0], cells[1]
        try:
            out[key] = int(val) if re.fullmatch(r"-?\d+", val) else float(val)
        except ValueError:
            out[key] = val
    return out


hw_tab = sim_table("hardware")
power_tab = sim_table("power")

HARDWARE = {
    "soc": str(hw_tab["soc"]),
    "band": str(hw_tab["band"]),
    "fov_deg": float(hw_tab["fov_deg"]),
    "presence_min_m": float(hw_tab["presence_min_m"]),
    "presence_max_m": float(hw_tab["presence_max_m"]),
    "vitals_max_m": float(hw_tab["vitals_max_m"]),
    "vitals_ref_m": float(hw_tab["vitals_ref_m"]),
    "vitals_grace": float(hw_tab["vitals_grace"]),
    "breath_disp_mm": str(hw_tab["breath_disp_mm"]),
    "heart_disp_mm": str(hw_tab["heart_disp_mm"]),
    "vitals_orientation": {
        "facing": float(hw_tab["orientation_facing"]),
        "side": float(hw_tab["orientation_side"]),
        "back": float(hw_tab["orientation_back"]),
    },
    "fan_penalty": float(hw_tab["fan_penalty"]),
    "fan_false_presence": float(hw_tab["fan_false_presence"]),
    "vitals_period_ms": int(hw_tab["vitals_period_ms"]),
    "vitals_dropout": float(hw_tab["vitals_dropout"]),
    "breath_jitter_bpm": float(hw_tab["breath_jitter_bpm"]),
    "heart_jitter_bpm": float(hw_tab["heart_jitter_bpm"]),
}
if not (0 < HARDWARE["vitals_ref_m"] <= HARDWARE["vitals_max_m"] <= HARDWARE["presence_max_m"]):
    die("hardware ranges are not ordered: vitals_ref <= vitals_max <= presence_max")

RAILS = {k: float(v) for k, v in power_tab.items()}
for k in ["radar_mw", "c6_active_mw", "wifi_listen_mw", "wifi_modem_sleep_mw",
          "wifi_tx_mw", "led_mw", "bh1750_mw"]:
    if k not in RAILS:
        die(f"power rail {k} missing from SIM:power table")

# calibration anchor: default-active total must stay near Seeed's 0.8 W figure
default_total = (RAILS["radar_mw"] + RAILS["c6_active_mw"] + RAILS["wifi_listen_mw"]
                 + RAILS["led_mw"] + RAILS["bh1750_mw"])
if not (700 <= default_total <= 950):
    die(f"power rails drifted off the 0.8 W kit anchor (default total {default_total} mW)")

must(NOTES_MD, "0.8 W", "kit active power anchor documented")
POWER_NOTES = [
    "Rails calibrated to Seeed's published kit envelope (0.5 W standby / 0.8 W active, 5 V) — "
    "see docs/hardware/mr60bha2_radar_notes.md §4 for the derivation and citations.",
    "The radar has no public sleep/duty-cycle command; it IS the useful computation. "
    "The only meaningful discretionary consumer is the C6's WiFi radio.",
    "WIFI_POWER_SAVE (modem sleep) is off by default on this mains-powered witness "
    "(include/canary/config.h documents the ~20 mA trade).",
]
must(PROJ / "include/canary/config.h", "WIFI_POWER_SAVE", "modem-sleep lever exists")

# --------------------------------------------------------------------------- #
# 9. placement presets (authored; geometry facts from the notes doc)
# --------------------------------------------------------------------------- #

must(NOTES_MD, "1 m above the head of the bed", "Seeed bedside mount guidance")
must(NOTES_MD, "45°", "bedside 45° tilt guidance")

MOUNTS = [
    {"id": "wall", "label": "wall (presence)", "height_m": 1.6, "tilt_deg": 5,
     "person": {"x": 2.6, "y": 0.0, "posture": "standing", "orientation": "facing", "moving": False},
     "blurb": "restricted-zone witness: workshop, server closet, storage cage"},
    {"id": "stand", "label": "bedside stand (wellbeing)", "height_m": 1.0, "tilt_deg": 45,
     "person": {"x": 0.9, "y": 0.0, "posture": "lying", "orientation": "facing", "moving": False},
     "blurb": "Seeed reference sleep geometry: ~1 m above the headboard, 45° down, chest <= 1.5 m"},
    {"id": "ceiling", "label": "ceiling (fda2-style)", "height_m": 2.6, "tilt_deg": 0,
     "person": {"x": 0.6, "y": 0.0, "posture": "lying", "orientation": "facing", "moving": False},
     "blurb": "the fall-detection sibling's mount — presence works, vitals geometry is marginal"},
]

# --------------------------------------------------------------------------- #
# 10. cards + display bridge cross-checks
# --------------------------------------------------------------------------- #

must(CARDS_MD, "one entity to one card", "Canary Cards invariant")
must(REPO / "canary-local/assets/canary-cards.js", "CARD_SCHEMA_V = 1", "cards schema version")
# the display emulator ingests this device type through its real dispatcher
must(REPO / "canary-local/emulator/web/emu-shell.js", "canary-sense", "emulator knows canary-sense")

DISPLAY = {
    "device_type_wire": DEVICE_TYPE,
    "emulator_module": "emulator/dist/canary-display-watch.js",
    "note": ("The display firmware ingests any securacv/<id>/# sibling generically today "
             "(fleet_model Witness); Canary Cards (docs/standard/CANARY_CARDS.md) is the "
             "documented path to type-aware cards."),
}

# --------------------------------------------------------------------------- #
# 11. sanity floors + write
# --------------------------------------------------------------------------- #

if len(ENTITIES) < 14:
    die(f"entity parse too thin: {len(ENTITIES)}")
if len(FRAME_TYPES) != 5:
    die("expected exactly 5 decoded frame types")
if len(BENCH) < 3:
    die("bench list too thin")

out = {
    "$note": "GENERATED by canary-local/tools/gen_senselab.py — do not edit by hand; "
             "CI drift-gates this file against the firmware + hardware notes.",
    "generated_by": "canary-local/tools/gen_senselab.py",
    "device": {
        "id": "canary-sense",
        "name": sense_reg["name"],
        "tagline": sense_reg["tagline"],
        "device_type": DEVICE_TYPE,
        "model_default": MODEL_DEFAULT,
        "model_wellbeing": MODEL_WELLBEING,
        "board": BOARD_NAME,
        "board_short": "XIAO ESP32-C6 + MR60BHA2",
        "fw_version": FW_VERSION,
        "fw_train": FW_TRAIN,
        "modality": sense_reg["modality"],
        "status": sense_reg["status"],
        "heartbeat_ms": HEARTBEAT_MS,
    },
    "pins": PINS,
    "protocol": {
        "sof": SOF,
        "header_len": HEADER_LEN,
        "max_payload": MAX_PAYLOAD,
        "frame_queue": FRAME_QUEUE,
        "checksum": "XOR-fold every covered byte, then bitwise-invert (header bytes [0..6]; payload)",
        "frames": FRAME_TYPES,
        "dropped_frames": DROPPED_FRAMES,
        "bench": BENCH,
        "source": "firmware/common/sensors/mmwave_mr60/mr60_uart.h (protocol notes + [BENCH] list)",
    },
    "fsm": {
        "presence": PRESENCE_CFG,
        "vitals": VITALS_CFG,
        "semantics": [s[0] for s in SEMANTICS],
    },
    "privacy": {
        "vocabulary": {
            "presence": ["unknown", "clear", "present"],
            "occupants": ["0", "1", "2+"],
            "range": ["unknown", "near", "mid", "far"],
        },
        "events": EVENTS,
        "canonical": CANONICAL,
        "bucket_s": 600,
        "classes": PRIVACY_CLASSES,
    },
    "topics": TOPICS,
    "entities": ENTITIES,
    "flavors": FLAVORS,
    "ota_products": OTA_PRODUCTS,
    "hardware": HARDWARE,
    "power": {"rails": RAILS, "notes": POWER_NOTES},
    "placement": {"mounts": MOUNTS},
    "display": DISPLAY,
    "docs": {
        "notes": "docs/hardware/mr60bha2_radar_notes.md",
        "design": "docs/canary_sense_mr60bha2_design.md",
        "cards": "docs/standard/CANARY_CARDS.md",
        "firmware": "firmware/projects/canary-sense/README.md",
    },
}

OUT_JSON.write_text(json.dumps(out, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
print(f"gen_senselab.py: wrote {OUT_JSON.relative_to(REPO)}  "
      f"({len(ENTITIES)} entities, {len(FRAME_TYPES)} frame types, "
      f"{len(BENCH)} bench notes, rails total {default_total:.0f} mW)")
