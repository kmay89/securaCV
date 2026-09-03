#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_vision.py — build canary-local/devices/vision.json from the canary-vision
firmware + the Grove Vision AI V2 docs.

The Vision teaching page (`canary-local/vision.html`) stages the whole life of
a Canary Vision — the module in 3D, the two-USB-C-port gotcha, the one-time
SenseCraft model load (with staged sensor boxes processed by the compiled
firmware core), the host flash, the boot console, the
MQTT/HA surfaces, the boxes-only Aim card, placement presets and live tuning.
None of it is hand-faked: every constant, topic, entity name, boot line, step
and threshold this page shows is either parsed straight out of the firmware /
docs, or authored here and then *validated to still exist in the source*
(this file `sys.exit(1)`s on drift). CI re-runs the generator and
`git diff --exit-code`s `vision.json` — the same anti-rot contract as
`gen_wap.py` / `gen_homeassistant.py` / `gen_boards.py`.

Sources of truth (all in-repo, deterministic, offline):
  firmware/projects/canary-vision/
      include/canary/config.h         identity, detection seeds, voxel/frame,
                                      aim cadence, OTA product
      include/canary/version.h        firmware version
      include/canary/detect_config.h  runtime-tunable bounds
      include/canary/topics.h         every MQTT topic template
      src/main.cpp                    boot scenes, aim payload, event payload
      include/canary/vision/detection_pipeline.h
                                      best-box rule + voxel mapping shared by
                                      hardware and browser firmware builds
      src/vision/vision_mgr.cpp       I2C link and production-core call
      src/state/presence_fsm.cpp      the event vocabulary
      src/ha/ha_discovery.cpp         the HA discovery entity set
      src/net/{wifi_mgr,mqtt_mgr}.cpp boot log lines
      README.md                       host-board envs, quickstart, tuning table
  firmware/common/boot/boot_banner.cpp   the shared boot banner scenes
  docs/hardware/grove_vision_ai_v2_guide.md      module specs, ports, protocol
  docs/hardware/canary_vision_getting_started.md the end-to-end walkthrough
  canary-local/devices/registry.json  fw_train + the canary-vision card facts
  canary-local/devices/boards.json    device -> board mapping

Run:  python3 canary-local/tools/gen_vision.py
"""

import json
import re
from pathlib import Path

from _tooling import die, repo_root

REPO = repo_root()
FW = REPO / "firmware/projects/canary-vision"
CONFIG_H = FW / "include/canary/config.h"
VERSION_H = FW / "include/canary/version.h"
DETECT_CFG_H = FW / "include/canary/detect_config.h"
DETECT_PROFILES_H = FW / "include/canary/detect_profiles.h"
TOPICS_H = FW / "include/canary/topics.h"
MAIN_CPP = FW / "src/main.cpp"
VISION_MGR_CPP = FW / "src/vision/vision_mgr.cpp"
DETECTION_PIPELINE_H = FW / "include/canary/vision/detection_pipeline.h"
PRESENCE_FSM_CPP = FW / "src/state/presence_fsm.cpp"
HA_DISCOVERY_CPP = FW / "src/ha/ha_discovery.cpp"
WIFI_MGR_CPP = FW / "src/net/wifi_mgr.cpp"
MQTT_MGR_CPP = FW / "src/net/mqtt_mgr.cpp"
FW_README = FW / "README.md"
BOOT_BANNER_CPP = REPO / "firmware/common/boot/boot_banner.cpp"
GUIDE = REPO / "docs/hardware/grove_vision_ai_v2_guide.md"
GETTING_STARTED = REPO / "docs/hardware/canary_vision_getting_started.md"
REGISTRY = REPO / "canary-local/devices/registry.json"
BOARDS = REPO / "canary-local/devices/boards.json"
FLASH_JSON = REPO / "canary-local/devices/flash.json"
OUT_JSON = REPO / "canary-local/devices/vision.json"

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
        die(f"{label}: expected to find {needle!r} in {path.relative_to(REPO)} — source changed?")


def grab(path: Path, pattern: str, label: str, flags=0) -> str:
    m = re.search(pattern, read(path), flags)
    if not m:
        die(f"{label}: pattern {pattern!r} not found in {path.relative_to(REPO)}")
    return m.group(1)


def igrab(path: Path, pattern: str, label: str) -> int:
    return int(grab(path, pattern, label))


# --------------------------------------------------------------------------- #
# 1. identity + firmware constants (config.h / version.h / detect_config.h)
# --------------------------------------------------------------------------- #

FW_VERSION = grab(VERSION_H, r'#define CANARY_FW_VERSION\s+"([^"]+)"', "fw version")
DEVICE_TYPE = grab(CONFIG_H, r'DEVICE_TYPE\s*=\s*"([^"]+)"', "device type")
DEVICE_ID = grab(CONFIG_H, r'DEVICE_ID\s*=\s*"([^"]+)"', "device id")
MODEL = grab(CONFIG_H, r'MODEL\s*=\s*"([^"]+)"', "model string")

PERSON_TARGET = igrab(CONFIG_H, r"PERSON_TARGET\s*=\s*(\d+)", "PERSON_TARGET")
SCORE_MIN = igrab(CONFIG_H, r"SCORE_MIN\s*=\s*(\d+)", "SCORE_MIN")
LOST_TIMEOUT_MS = igrab(CONFIG_H, r"LOST_TIMEOUT_MS\s*=\s*(\d+)", "LOST_TIMEOUT_MS")
DWELL_START_MS = igrab(CONFIG_H, r"DWELL_START_MS\s*=\s*(\d+)", "DWELL_START_MS")
VOXEL_COLS = igrab(CONFIG_H, r"VOXEL_COLS\s*=\s*(\d+)", "VOXEL_COLS")
VOXEL_ROWS = igrab(CONFIG_H, r"VOXEL_ROWS\s*=\s*(\d+)", "VOXEL_ROWS")
FRAME_W = igrab(CONFIG_H, r"FRAME_W\s*=\s*(\d+)", "FRAME_W")
FRAME_H = igrab(CONFIG_H, r"FRAME_H\s*=\s*(\d+)", "FRAME_H")
INVOKE_PERIOD_MS = igrab(CONFIG_H, r"INVOKE_PERIOD_MS\s*=\s*(\d+)", "INVOKE_PERIOD_MS")
INTERACTION_WINDOW_MS = igrab(CONFIG_H, r"INTERACTION_AFTER_LEAVE_WINDOW_MS\s*=\s*(\d+)",
                              "INTERACTION_AFTER_LEAVE_WINDOW_MS")
ZONE_INTERACTION_MS = igrab(CONFIG_H, r"ZONE_INTERACTION_MS\s*=\s*(\d+)", "ZONE_INTERACTION_MS")
HEARTBEAT_MS = igrab(CONFIG_H, r"HEARTBEAT_MS\s*=\s*(\d+)", "HEARTBEAT_MS")
AIM_PUBLISH_MS = igrab(CONFIG_H, r"AIM_PUBLISH_MS\s*=\s*(\d+)", "AIM_PUBLISH_MS")
AIM_IDLE_PUBLISH_MS = igrab(CONFIG_H, r"AIM_IDLE_PUBLISH_MS\s*=\s*(\d+)", "AIM_IDLE_PUBLISH_MS")
AIM_AUTO_OFF_MS = igrab(CONFIG_H, r"AIM_AUTO_OFF_MS\s*=\s*(\d+)", "AIM_AUTO_OFF_MS")
HA_PREFIX = grab(CONFIG_H, r'HA_DISCOVERY_PREFIX\s*=\s*"([^"]+)"', "HA discovery prefix")
OTA_PRODUCT = grab(CONFIG_H, r'#define SECURACV_OTA_PRODUCT\s+"([^"]+)"', "OTA product")

BOUNDS = {
    "score": [igrab(DETECT_CFG_H, r"DETECT_SCORE_MIN_LO\s*=\s*(\d+)", "score lo"),
              igrab(DETECT_CFG_H, r"DETECT_SCORE_MIN_HI\s*=\s*(\d+)", "score hi")],
    "target": [0, 255],
    "lost_ms": [igrab(DETECT_CFG_H, r"DETECT_LOST_MS_LO\s*=\s*(\d+)", "lost lo"),
                igrab(DETECT_CFG_H, r"DETECT_LOST_MS_HI\s*=\s*(\d+)", "lost hi")],
    "dwell_ms": [igrab(DETECT_CFG_H, r"DETECT_DWELL_MS_LO\s*=\s*(\d+)", "dwell lo"),
                 igrab(DETECT_CFG_H, r"DETECT_DWELL_MS_HI\s*=\s*(\d+)", "dwell hi")],
}
# the HA number entity for target really is 0..255 in ha_discovery.cpp
must(HA_DISCOVERY_CPP, '"box", 0, 255, 1', "target number bounds")

# --------------------------------------------------------------------------- #
# 2. registry + boards (card facts, board mapping, fw train)
# --------------------------------------------------------------------------- #

registry = json.loads(read(REGISTRY))
FW_TRAIN = registry.get("fw_train") or die("registry fw_train missing")
vis_reg = next((d for d in registry["devices"] if d["id"] == "canary-vision"), None)
if not vis_reg:
    die("registry has no canary-vision entry")

boards = json.loads(read(BOARDS))
BOARD_ID = None
BOARD_NAME_FULL = None
# the board whose own devices list includes canary-vision is the definitive map
for bid, b in (boards.get("boards") or {}).items():
    if "canary-vision" in (b.get("devices") or []):
        BOARD_ID, BOARD_NAME_FULL = bid, b.get("name")
        break
if not BOARD_ID:
    # fall back to device_board (a LIST of boards, primary first; tolerate a string)
    _v = boards.get("device_board", {}).get("canary-vision")
    BOARD_ID = _v[0] if isinstance(_v, list) and _v else _v
    BOARD_NAME_FULL = (boards.get("boards", {}).get(BOARD_ID) or {}).get("name")
if not BOARD_ID:
    die("boards.json maps no board to canary-vision")

# --------------------------------------------------------------------------- #
# 3. the module (specs + ports + protocol) — from the device guide
# --------------------------------------------------------------------------- #

guide = read(GUIDE)

SKU = grab(GUIDE, r"SKU (\d+)", "module SKU")


def md_table(text: str, anchor: str, label: str, min_rows: int = 2):
    """Parse the first markdown table after `anchor`; returns list of row lists."""
    i = text.find(anchor)
    if i < 0:
        die(f"{label}: anchor {anchor!r} not found")
    rows = []
    started = False
    for ln in text[i:].splitlines():
        s = ln.strip()
        if s.startswith("|"):
            cells = [c.strip() for c in s.strip("|").split("|")]
            if all(set(c) <= set("-: ") for c in cells):
                continue
            rows.append(cells)
            started = True
        elif started:
            break
    if len(rows) < min_rows + 1:
        die(f"{label}: table under {anchor!r} parsed thin ({len(rows)} rows)")
    return rows


def strip_md(s: str) -> str:
    s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)
    s = s.replace("**", "").replace("`", "")
    return re.sub(r"\s+", " ", s).strip()


spec_rows = md_table(guide, "| Component | Spec |", "module spec table", 5)
MODULE_SPECS = [{"k": strip_md(r[0]), "v": strip_md(r[1])} for r in spec_rows[1:]]

port_rows = md_table(guide, "| Task | Plug your computer into |", "two-port table", 4)
TWO_PORTS = [{"task": strip_md(r[0]), "port": strip_md(r[1]), "chip": strip_md(r[2])}
             for r in port_rows[1:]]
must(GUIDE, "Model work → module port. Firmware work → XIAO port.", "two-port rule of thumb")

grove_rows = md_table(guide, "| Grove wire | Signal |", "grove pin table", 3)
GROVE_PINS = [{"wire": strip_md(r[0]), "signal": strip_md(r[1]),
               "devkit": strip_md(r[2]), "xiao_c3": strip_md(r[3]), "xiao_s3": strip_md(r[4])}
              for r in grove_rows[1:]]

must(GUIDE, "`0x62`", "i2c address in guide")
must(VISION_MGR_CPP, "AI.begin()", "SSCMA begin call")
must(GUIDE, "921600", "uart baud in guide")

MODULE = {
    "name": "Grove Vision AI V2",
    "sku": SKU,
    "specs": MODULE_SPECS,
    "i2c_addr": "0x62",
    "i2c_hz": "400 kHz (SSCMA default; 100 kHz also works)",
    "uart_baud": 921600,
    "frame": {"w": FRAME_W, "h": FRAME_H},
    "invoke_period_ms": INVOKE_PERIOD_MS,
    "camera": "OV5647 family — OV5647-62 recommended; -67 / -160 supported. "
              "Other CSI cameras may enumerate but render green-only (no ISP driver).",
    "privacy": "Image capture, processing and model inference all happen inside the HX6538. "
               "In event mode the ESP32 host only ever receives semantic results — boxes, "
               "class IDs, confidence scores — over a 4-wire I2C bus. No pixels cross the wire.",
    "unused": "The module's SD slot and PDM microphone are deliberately unused: saving JPEGs "
              "or capturing audio would violate the no-raw-export posture. Leave the SD slot empty.",
}
must(GUIDE, "OV5647-62 recommended", "camera recommendation")
must(GUIDE, "No pixels cross the wire", "privacy posture line")
must(GUIDE, "Leave the SD slot empty in deployments.", "SD slot posture")

# protocol facts (§8) — what the wire actually says
must(GUIDE, "AT+INVOKE=1,0,1", "raw invoke example")
must(GUIDE, "{x, y, w, h, score, target}", "boxes tuple")
PROTOCOL = {
    "firmware": "SSCMA-Micro (Seeed) — an AT-style protocol over I2C/UART/USB",
    "library": "Seeed_Arduino_SSCMA (pinned v1.0.3)",
    "invoke": 'AI.invoke(times, filter, show) — filter=true only replies when results change; '
              'show=false suppresses image data',
    "boxes": "AI.boxes() → {x, y, w, h, score, target} (box center coords, score 0–100, "
             "target = class index)",
    "wire_example": 'AT+INVOKE=1,0,1\\r → \\r{"type":1,"name":"INVOKE","code":0,'
                    '"data":{"boxes":[[x,y,w,h,score,target]],"perf":[...]}}\\n',
    "never_used": [
        'AT+SAMPLE — returns base64 JPEG frames',
        'AT+ACTION="save_jpeg()" — writes frames to SD',
        "AT+WIFI / AT+MQTTSERVER — the module's own SenseCraft cloud/MQTT uplink",
    ],
}
must(GUIDE, "AT+SAMPLE", "AT+SAMPLE mention")
must(GUIDE, 'AT+ACTION="save_jpeg()"', "save_jpeg mention")
must(FW_README, "SSCMA", "SSCMA in README")

# --------------------------------------------------------------------------- #
# 4. hosts + assembly (README env table + getting-started §1)
# --------------------------------------------------------------------------- #

readme = read(FW_README)
host_rows = md_table(readme, "| Build env | Host board |", "host boards table", 3)
HOSTS = [{"env": strip_md(r[0]), "board": strip_md(r[1]), "hookup": strip_md(r[2]),
          "i2c": strip_md(r[3])} for r in host_rows[1:]]
for h in HOSTS:
    must(FW_README, h["env"], f"env {h['env']}")

gs = read(GETTING_STARTED)
need_rows = md_table(gs, "| Item | Notes |", "what-you-need table", 5)
NEEDS = [{"item": strip_md(r[0]), "note": strip_md(r[1])} for r in need_rows[1:]]

ASSEMBLY = {
    "steps": [
        "Camera first: lift the black latch on the module's CSI connector, slide the ribbon in "
        "with the contacts facing the PCB (check both ends — backwards insertion is the most "
        "common \"no preview\" cause), press the latch closed. Same procedure at the camera end.",
        "Solder the headers onto the XIAO (pins point down).",
        "Stack the XIAO into the module's socket. Orientation is critical: both USB-C ports "
        "must face the same direction. Backwards seating feeds power into GPIO and can kill "
        "either board.",
        "The assembly now has two USB-C ports that go to two different chips — this trips "
        "everyone once.",
    ],
    "warning": "both USB-C ports must face the same direction",
}
must(GETTING_STARTED, "contacts facing the PCB", "CSI ribbon orientation")
must(GETTING_STARTED, "both USB-C ports must face the same direction", "stacking orientation warning")
must(GUIDE, "plugging it in backwards feeds power into GPIO", "backwards-seating consequence")

# --------------------------------------------------------------------------- #
# 5. the one-time model load (SenseCraft) — steps parsed from the walkthrough
# --------------------------------------------------------------------------- #

SENSECRAFT_URL = grab(GETTING_STARTED, r"<(https://sensecraft\.seeed\.cc/ai/device/local/\d+)>",
                      "sensecraft url")
must(GETTING_STARTED, "Select Model → Person Detection", "model pick step")
must(GETTING_STARTED, "Chrome or Edge", "browser requirement")
must(GUIDE, "Confidence", "confidence slider")
must(GUIDE, "IoU", "iou slider")
must(GUIDE, "16 MB flash and **persists across power cycles", "model persistence")
must(GUIDE, "I2C results to the host are not delivered concurrently", "preview pauses i2c")

MODEL_LOAD = {
    "securacv_flasher": "Prefer to stay in the Lab? flash.html now carries a module flow: "
                        "the same pinned model, burned from our own page over WebSerial, "
                        "SHA-256-verified, with a live bench check after. The SenseCraft "
                        "path below remains the documented vendor fallback — staged here "
                        "click for click either way.",
    "url": SENSECRAFT_URL,
    "browser": "Chrome or Edge — the flasher needs WebSerial; Firefox/Safari won't work",
    "port": "the MODULE's USB-C port (the big carrier-PCB one, next to the Grove connector) — "
            "not the XIAO's",
    "model": "Person Detection",
    "duration": "1–2 minutes; keep the tab foregrounded — backgrounding it can abort the transfer",
    "persistence": "The model lives in the module's own 16 MB flash and persists across power "
                   "cycles and every future host reflash — you do this once.",
    "class_check": f"Confirm the model's class list shows person as class {PERSON_TARGET} "
                   "(the firmware default; a different model → adjust later from HA).",
    "sliders": [
        {"name": "Confidence", "what": "minimum score to report a detection. The firmware "
         f"applies its own threshold too (SCORE_MIN in config.h, default {SCORE_MIN})."},
        {"name": "IoU", "what": "box-overlap threshold for de-duplication (non-max suppression) — "
         "how much two boxes must overlap before the lower-scoring one is dropped."},
    ],
    "preview_note": "SenseCraft's preview streams camera frames to the browser over the USB "
                    "cable you just plugged in — a physical, one-time, attended operation. The "
                    "deployed system never does this: day-to-day aiming uses the boxes-only Aim "
                    "card over local MQTT.",
    "pauses_i2c": "While a computer is connected to the module's USB port doing live preview, "
                  "I2C results to the host are not delivered — the module does one job at a "
                  "time. Unplug before bench-testing the ESP32 event path.",
    "driver": {
        "note": "If no serial port appears for the module, install the CH343 USB-serial driver. "
                "The XIAO port needs no driver (native USB CDC).",
        "linux_udev": 'SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="55d3", MODE:="0666"',
    },
    # Vendor-side mechanics — authored from Seeed's public sources (URLs kept
    # with each claim so a reader can verify); not drift-gated (out of repo).
    "wire": {
        "how": "Under the hood the preview is the SSCMA AT protocol over WebSerial at 921600 "
               "baud: AT+INVOKE streams events whose data carries a base64 JPEG frame plus a "
               "boxes array [x, y, w, h, score, target] — the browser decodes the JPEG and "
               "draws the boxes on top. No video stream, just stills and coordinates.",
        "sliders": "The Confidence / IoU sliders write AT+TSCORE / AT+TIOU (0–100). "
                   "SSCMA-Micro's YOLO defaults are 50 / 45.",
        "tscore_default": 50,
        "tiou_default": 45,
        "sources": [
            "https://github.com/Seeed-Studio/SSCMA-Micro/blob/main/docs/protocol/at-protocol-en_US.md",
            "https://github.com/Seeed-Studio/SenseCraft-Web-Toolkit",
        ],
    },
    "model_facts": {
        "arch": "Swift-YOLO (tiny), 192×192×3 RGB input",
        "accuracy": "91.6 mAP at INT8 on its evaluation set (model card)",
        "speed": "~48–76 ms per inference on the WE2 NPU (community benchmarks) — the same "
                 "INT8 model needs ~608 ms on an ESP32-S3. The NPU is the whole point.",
        "license": "MIT (SSCMA model zoo) — the person model is redistributable with attribution",
        "source": "https://github.com/Seeed-Studio/sscma-model-zoo/blob/main/docs/en/person_Detection_Swift-YOLO_192.md",
    },
}
must(GUIDE, 'ATTRS{idVendor}=="1a86"', "CH343 udev rule")
must(GETTING_STARTED, "streams camera frames to the", "preview privacy note")

# --------------------------------------------------------------------------- #
# 6. detection semantics — the exact rules the firmware runs
# --------------------------------------------------------------------------- #

# best-box rule (detection_pipeline.h): person class only, score >= threshold,
# highest score wins — one box, however many people are in frame.
must(VISION_MGR_CPP, "detection::sample_from_boxes(boxes, det)", "production detection-core call")
must(DETECTION_PIPELINE_H, "if (box.target != det.person_target) continue;", "class filter")
must(DETECTION_PIPELINE_H, "if (box.score < det.score_min) continue;", "score filter")
must(DETECTION_PIPELINE_H, "if (box.score > best_score) {", "best-box rule")
# voxel mapping — center of the box, integer grid math (refactored into
# point_to_cell() upstream in #1071; same math, verified where it now lives)
must(DETECTION_PIPELINE_H, "point_to_cell(box.x + (box.w / 2), box.y + (box.h / 2), rows, cols, row, col);", "voxel center")
must(DETECTION_PIPELINE_H, "c = (px * safe_cols) / FRAME_W;", "voxel col math")

EVENTS = re.findall(r'emit\(out_event,\s*"([a-z_]+)"', read(PRESENCE_FSM_CPP))
if len(set(EVENTS)) < 5:
    die(f"presence FSM event vocabulary parsed thin: {EVENTS}")

# Watch profiles — the per-use-case preset table behind HA's "Watch profile"
# select (room presence vs litter box). Parsed straight from the firmware
# table so the page can never promise a preset the firmware doesn't ship.
PROFILE_ROWS = re.findall(
    r'\{"([a-z_]+)",\s*"([^"]+)",\s*"([^"]+)",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*'
    r'/\*beacon_class=\*/0x0([0-9a-fA-F])\}',
    read(DETECT_PROFILES_H), re.S)
if len(PROFILE_ROWS) < 2:
    die(f"watch profile table parsed thin: {PROFILE_ROWS}")
BEACON_CLASS_NAMES = {"1": "person", "2": "vehicle", "3": "animal", "4": "package"}
PROFILES = []
for key, label, subject, target, score, lost, dwell, bclass in PROFILE_ROWS:
    if bclass not in BEACON_CLASS_NAMES:
        die(f"profile {key} advertises unknown beacon class 0x0{bclass}")
    PROFILES.append({
        "key": key, "label": label, "subject": subject,
        "target": int(target), "score": int(score),
        "lost_ms": int(lost), "dwell_ms": int(dwell),
        "beacon_class": BEACON_CLASS_NAMES[bclass],
    })
if PROFILES[0]["key"] != "room_presence":
    die("profile 0 must stay room_presence — the NVS default and HA fallback")
LITTER = next((p for p in PROFILES if p["key"] == "litter_box"), None)
if not LITTER:
    die("litter_box watch profile missing from detect_profiles.h")

DETECT = {
    "person_target": PERSON_TARGET,
    "score_min": SCORE_MIN,
    "lost_timeout_ms": LOST_TIMEOUT_MS,
    "dwell_start_ms": DWELL_START_MS,
    "bounds": BOUNDS,
    "interaction_window_ms": INTERACTION_WINDOW_MS,
    "zone_interaction_ms": ZONE_INTERACTION_MS,
    "voxel": {"cols": VOXEL_COLS, "rows": VOXEL_ROWS},
    "frame": {"w": FRAME_W, "h": FRAME_H},
    "invoke_period_ms": INVOKE_PERIOD_MS,
    "best_box": "Person class only, score ≥ threshold, highest score wins — one best box per "
                "frame, however many people are visible. A second person never adds a second "
                "claim; the witness reports someone, not everyone.",
    "voxel_rule": "The box's center point maps to one cell of a "
                  f"{VOXEL_COLS}×{VOXEL_ROWS} grid over the {FRAME_W}×{FRAME_H} frame — "
                  "that cell is the coarsest useful \"where\", and the only \"where\" that "
                  "ever leaves the device.",
    "events": sorted(set(EVENTS)),
    "profiles": PROFILES,
    "profiles_note": "Watch profiles are one-step per-use-case presets (HA's Watch profile "
                     "select): picking one applies its recommended tuning to the four "
                     "settings and retargets the fleet beacon's detect class. The event "
                     "vocabulary and the signed witness record are identical across "
                     "profiles — no new claim types (Invariant VI).",
    "seeds_note": "The compiled constants seed the first boot only; live values are NVS-backed, "
                  "adjustable from Home Assistant, and persist across reboots and OTA installs.",
}

# --------------------------------------------------------------------------- #
# 7. serial console — the staged boot, every line anchored to its source
# --------------------------------------------------------------------------- #

must(BOOT_BANNER_CPP, ',_,          Waking up...', "banner: waking up")
must(BOOT_BANNER_CPP, "This is your privacy witness device.", "banner: witness line")
must(BOOT_BANNER_CPP, "(o.o) ?       Checking the hardware...", "banner: hardware scene")
must(MAIN_CPP, '(^.^)         What can I see?', "vision scene")
must(MAIN_CPP, '(o.o)  ))     Connecting to MQTT...', "mqtt scene")
must(MAIN_CPP, 'boot_kv("Sensor",  "Grove Vision AI V2 (SSCMA)")', "sensor kv")
must(MAIN_CPP, 'boot_kvf("Profile", "%s  (watching for a %s)"', "profile kv")
must(VISION_MGR_CPP, 'Grove Vision AI ID=%d', "i2c id line")
must(VISION_MGR_CPP, "ERROR: Grove Vision AI V2 not responding", "i2c error line")
must(WIFI_MGR_CPP, "Connecting to the provisioned WiFi network ...", "wifi connecting line")
must(WIFI_MGR_CPP, "Connected IP=%s RSSI=%ddBm", "wifi connected line")
must(MQTT_MGR_CPP, "Connecting %s:%u as %s ...", "mqtt connecting line")
must(MQTT_MGR_CPP, 'log_line("MQTT", "Connected.")', "mqtt connected line")
must(HA_DISCOVERY_CPP, "Home Assistant discovery published (retained).", "discovery done line")
must(MAIN_CPP, "Ed25519 identity ready (events signed)", "witness ready line")

EX_IP = "192.168.1.117"
EX_HEX = "b3f2a9c41d5e"

SERIAL = {
    "port_hint": "the XIAO's USB-C · USB-CDC serial · 115200 8N1  (pio device monitor)",
    "banner": [
        "              ,_,          Waking up...",
        "             (o.o)",
        "             /| |\\         SecuraCV Canary Vision",
        f"              d b          v{FW_VERSION}",
        "",
        "    This is your privacy witness device.",
        "    It creates tamper-proof records of what it",
        "    sees, so nobody can change the story later.",
    ],
    "boot": [
        {"tag": "[--]", "text": f"Type   {DEVICE_TYPE}"},
        {"tag": "[--]", "text": f"Model  {MODEL}"},
        {"tag": "", "text": "              ,_,"},
        {"tag": "", "text": "             (o.o) ?       Checking the hardware..."},
        {"tag": "[OK]", "text": "Board  Seeed XIAO ESP32-C3 · 160 MHz · 4 MB flash"},
        {"tag": "", "text": "              ,_,"},
        {"tag": "", "text": "             (^.^)         What can I see?"},
        {"tag": "[OK]", "text": "Sensor Grove Vision AI V2 (SSCMA)"},
        {"tag": "[--]", "text": "Profile room_presence  (watching for a person)"},
        {"tag": "[--]", "text": f"Target class {PERSON_TARGET}  (the model's person class)"},
        {"tag": "[--]", "text": f"Score  >= {SCORE_MIN}%  (confidence threshold)"},
        {"tag": "[--]", "text": f"Lost   {LOST_TIMEOUT_MS} ms  (timeout before 'person left')"},
        {"tag": "[--]", "text": f"Dwell  {DWELL_START_MS} ms  (lingering detection)"},
        {"tag": "[--]", "text": f"Voxel  {VOXEL_COLS}x{VOXEL_ROWS} grid ({FRAME_W}x{FRAME_H} frame)"},
        {"tag": "[--]", "text": f"Rate   every {INVOKE_PERIOD_MS} ms"},
        {"tag": "[WIFI]", "text": "Connecting to the provisioned WiFi network ..."},
        {"tag": "[WIFI]", "text": f"Connected IP={EX_IP} RSSI=-52dBm"},
        {"tag": "[I2C]", "text": "Grove Vision AI ID=2"},
        {"tag": "", "text": "              ,_,  ))"},
        {"tag": "", "text": "             (o.o)  ))     Connecting to MQTT..."},
        {"tag": "[--]", "text": f"Device ID  {DEVICE_ID}"},
        {"tag": "[--]", "text": f"Hardware ID  {EX_HEX}  (salted pseudonym — never the MAC)"},
        {"tag": "[OK]", "text": "Witness  Ed25519 identity ready (events signed)"},
        {"tag": "[MQTT]", "text": f"Connecting 192.168.1.10:1883 as {DEVICE_ID} ..."},
        {"tag": "[MQTT]", "text": "Connected."},
        {"tag": "[DISC]", "text": "Home Assistant discovery published (retained)."},
    ],
    "ready": [
        "",
        f"  watching · invoke every {INVOKE_PERIOD_MS} ms · heartbeat every {HEARTBEAT_MS // 1000} s",
        "  a non-zero Grove Vision AI ID is the handshake — ID=0 means check the I2C link",
    ],
    "id_zero_hint": "Grove Vision AI ID=0 → I2C wiring/pins wrong, Grove cable on the wrong "
                    "socket, or module in bootloader mode (tap Reset).",
}
must(GUIDE, "Grove Vision AI ID=0", "ID=0 symptom")

# --------------------------------------------------------------------------- #
# 8. MQTT topics + HA discovery entities — parsed from the firmware
# --------------------------------------------------------------------------- #

topics_h = read(TOPICS_H)
suffixes = re.findall(r'"securacv/%s/([a-z_/]+)"', topics_h)
if len(suffixes) < 12:
    die(f"only {len(suffixes)} topic templates parsed from topics.h")

TOPIC_DESCS = {
    "events": {"retain": False, "desc": "presence_started / dwell_started / … — the signed witness events, with confidence, voxel and bbox"},
    "state": {"retain": True, "desc": "retained snapshot: presence, occupants, range, confidence, voxel"},
    "status": {"retain": True, "desc": "online/offline (LWT) + heap-health diagnostics"},
    "health": {"retain": True, "desc": "device health — carries public_key for HA's TOFU pinning"},
    "chain": {"retain": True, "desc": "the retained signed head of the canonical hash chain"},
    "update/state": {"retain": True, "desc": "signed pull-OTA state (HA update entity)"},
    "update/cmd": {"retain": False, "desc": "HA writes \"install\""},
    "update/auto": {"retain": True, "desc": "auto-update switch state"},
    "update/auto/cmd": {"retain": False, "desc": "auto-update switch command"},
    "cfg/state": {"retain": True, "desc": "retained runtime detection settings: {\"target\",\"score\",\"lost_ms\",\"dwell_ms\",\"profile\",\"profile_label\"}"},
    "cfg/target/set": {"retain": False, "desc": "set the subject class index (0–255)"},
    "cfg/score/set": {"retain": False, "desc": "set the score threshold (0–100)"},
    "cfg/lost/set": {"retain": False, "desc": "set the lost timeout (ms)"},
    "cfg/dwell/set": {"retain": False, "desc": "set the dwell start (ms)"},
    "cfg/profile/set": {"retain": False, "desc": "select the watch profile (room_presence / litter_box) — applies that profile's tuning preset"},
    "aim": {"retain": False, "desc": "the boxes-only aim stream — coordinates and scores at ~5 Hz, never pixels"},
    "aim/state": {"retain": True, "desc": "aim switch state (off by default; auto-off)"},
    "aim/set": {"retain": False, "desc": "HA's Aim assist switch writes ON/OFF"},
    "identify/set": {"retain": False, "desc": "HA's Identify button — 10 s LED blink"},
    "identify": {"retain": False, "desc": "identify echo — dashboards pulse the card in sync"},
}
TOPICS = []
for s in suffixes:
    d = TOPIC_DESCS.get(s)
    if not d:
        die(f"topics.h grew a topic this page doesn't describe: securacv/<id>/{s}")
    TOPICS.append({"suffix": s, "retain": d["retain"], "desc": d["desc"]})

disc = read(HA_DISCOVERY_CPP)
# entity names appear as \"name\":\"X\" inside C string literals; a %s name
# (the device card itself) is excluded — the page describes entities, not the card
raw_names = re.findall(r'\\"name\\":\\"([^"\\]+)\\"', disc)
number_names = re.findall(r'\{"cfg_[a-z]+",\s*"([^"]+)"', disc)
ENTITIES = [n for n in raw_names if "%" not in n] + number_names
if "Presence" not in ENTITIES or "Aim assist" not in ENTITIES:
    die(f"HA entity parse broke: {ENTITIES}")
ENTITY_META = {
    "Presence": ("binary_sensor", "someone is here (motion class)"),
    "Dwelling": ("binary_sensor", "someone has stayed — occupancy class"),
    "Confidence": ("sensor", "best-box score, 0–100 %"),
    "Voxel": ("sensor", f"the occupied cell of the {VOXEL_COLS}×{VOXEL_ROWS} grid, as \"r,c\""),
    "Occupancy": ("sensor", "coarse count bucket — none / one / two / several, never an exact tally"),
    "Posture": ("sensor", "coarse posture ordinal from box shape — upright / horizontal / ambiguous"),
    "Proximity": ("sensor", "coarse distance ordinal from box area — near / mid / far"),
    "Last event": ("sensor", "the most recent witness event name"),
    "Uptime": ("sensor", "seconds since boot"),
    "WiFi RSSI": ("sensor", "diagnostic — link strength"),
    "Free heap": ("sensor", "diagnostic — working memory"),
    "Firmware": ("update", "signed pull-OTA with an Install button"),
    "Watch profile": ("select", "one-step per-use-case preset — room presence or litter box"),
    "Person class index": ("number", "which class the loaded model calls the subject"),
    "Score threshold": ("number", "confidence floor, live-tunable"),
    "Lost timeout": ("number", "silence before \"person left\""),
    "Dwell start": ("number", "sustained presence before \"dwelling\""),
    "Aim assist": ("switch", "the boxes-only live aim stream (auto-off 10 min)"),
    "Identify": ("button", "blink the LED to find this device"),
    "Auto Update": ("switch", "install signed releases automatically"),
}
DISCOVERY = []
seen = set()
for n in ENTITIES:
    if n in seen:
        continue
    seen.add(n)
    meta = ENTITY_META.get(n)
    if not meta:
        die(f"ha_discovery.cpp grew an entity this page doesn't describe: {n!r}")
    DISCOVERY.append({"name": n, "component": meta[0], "desc": meta[1]})
for n in ENTITY_META:
    if n not in seen:
        die(f"expected entity {n!r} no longer published by ha_discovery.cpp")

MQTT = {
    "prefix": "securacv",
    "topic_base": f"securacv/{DEVICE_ID}",
    "ha_prefix": HA_PREFIX,
    "topics": TOPICS,
    "discovery": {"entities": DISCOVERY,
                  "note": "No YAML, no pairing codes: the firmware publishes retained MQTT "
                          "Discovery configs the moment the broker link is up, and the device "
                          "simply appears in Home Assistant."},
    "cfg_state_example": {"target": PERSON_TARGET, "score": SCORE_MIN,
                          "lost_ms": LOST_TIMEOUT_MS, "dwell_ms": DWELL_START_MS,
                          "profile": PROFILES[0]["key"]},
    "event_example": {
        "device_id": DEVICE_ID, "device_type": DEVICE_TYPE,
        "event": "presence_started", "seq": 42, "presence": "present",
        "occupants": "one", "range": "near", "signed": True,
        "confidence": 91, "voxel": {"rows": VOXEL_ROWS, "cols": VOXEL_COLS, "r": 1, "c": 1},
        "bbox": {"x": 96, "y": 88, "w": 64, "h": 128},
    },
}
must(MAIN_CPP, '\\"voxel\\":{\\"rows\\":%u,\\"cols\\":%u,\\"r\\":%d,\\"c\\":%d}', "event voxel keys")
must(MAIN_CPP, '\\"bbox\\":{\\"x\\":%d,\\"y\\":%d,\\"w\\":%d,\\"h\\":%d}', "event bbox keys")

# --------------------------------------------------------------------------- #
# 9. aim assist — the boxes-only preview (payload keys from main.cpp)
# --------------------------------------------------------------------------- #

aim_fmt = grab(MAIN_CPP, r'static void aim_publish.*?snprintf\(msg, sizeof\(msg\),\s*(.*?)\);',
               "aim payload format", re.S)
for key in ["present", '\\"x\\"', '\\"y\\"', '\\"w\\"', '\\"h\\"', "score", '\\"vr\\"',
            '\\"vc\\"', "rows", "cols", '\\"fw\\"', '\\"fh\\"']:
    if key not in aim_fmt:
        die(f"aim payload lost key {key}")
must(MAIN_CPP, 'Aim assist ON (auto-off in 10 min).', "aim on log line")
must(GETTING_STARTED, "boxes-only", "aim: boxes-only phrasing")
must(GETTING_STARTED, "Start aiming", "aim card button")

AIM = {
    "publish_ms": AIM_PUBLISH_MS,
    "idle_publish_ms": AIM_IDLE_PUBLISH_MS,
    "auto_off_ms": AIM_AUTO_OFF_MS,
    "topic_suffix": "aim",
    "payload_keys": ["present", "x", "y", "w", "h", "score", "vr", "vc", "rows", "cols", "fw", "fh"],
    "payload_example": {"present": True, "x": 96, "y": 88, "w": 64, "h": 128, "score": 91,
                        "vr": 1, "vc": 1, "rows": VOXEL_ROWS, "cols": VOXEL_COLS,
                        "fw": FRAME_W, "fh": FRAME_H},
    "steps": [
        "Mount the device where it will live — then aim from the couch, not the module port.",
        "On the dashboard's Live view, press Start aiming (it flips the device's Aim assist "
        "switch; the live stream needs an HA admin user).",
        f"The firmware streams box coordinates + scores at ~{1000 // AIM_PUBLISH_MS} Hz on "
        f"securacv/<device_id>/aim — local MQTT, non-retained, no pixels.",
        "Walk the space: adjust camera tilt until people appear where you expect across the "
        "zone you care about, with scores comfortably above your threshold "
        f"(default {SCORE_MIN}).",
        f"Press Stop aiming — or walk away; it turns itself off after "
        f"{AIM_AUTO_OFF_MS // 60000} minutes and it's off by default on every boot.",
    ],
    "why_not_sensecraft": "Re-opening SenseCraft to aim needs a laptop physically on the "
                          "module's USB port, pauses I2C events to the host while connected, "
                          "and streams raw frames. The aim card works over the device's normal "
                          "local MQTT path, after mounting, from the couch.",
}

# --------------------------------------------------------------------------- #
# 10. flashing the host + the in-browser flasher hookup
# --------------------------------------------------------------------------- #

flash_products = json.loads(read(FLASH_JSON))
VISION_FLASH_PRODUCTS = [p for p in flash_products.get("products", [])
                         if p["id"].startswith("securacv-canary-vision")]
if not VISION_FLASH_PRODUCTS:
    die("flash.json lists no canary-vision products — gen_flash.py moved?")

qs = re.search(r"## Quickstart \(PlatformIO\)(.*?)##", readme, re.S)
if not qs:
    die("README quickstart section missing")
must(FW_README, "secrets/secrets.example.h", "secrets example path")
QUICKSTART = ["cp secrets/secrets.example.h secrets/secrets.h"]
for cmd in re.findall(r"`(pio [^`]+)`", qs.group(1)):
    QUICKSTART.append(cmd)
if len(QUICKSTART) < 3:
    die(f"quickstart parsed thin: {QUICKSTART}")

FLASH = {
    "port_rule": "Always through the XIAO's / DevKit's USB-C, never the module's. The module "
                 "port cannot see the ESP32, and the XIAO port cannot reach the Himax flash.",
    "quickstart": QUICKSTART,
    "monitor": "pio device monitor — 115200 baud; a non-zero `Grove Vision AI ID=...` line "
               "confirms the I2C link",
    "secrets": "cp secrets/secrets.example.h secrets/secrets.h — WiFi + MQTT fields. Your "
               "first USB flash with real secrets seeds the device's NVS; every signed OTA "
               "update it installs later inherits that setup automatically.",
    "browser": {
        "page": "flash.html",
        "note": "The Lab's in-browser flasher already knows every canary-vision host build — "
                "it detects the chip over WebSerial and will only offer images built for that "
                "exact silicon.",
        "products": [{"id": p["id"], "name": p["name"], "chip": p["chip"], "board": p["board"]}
                     for p in VISION_FLASH_PRODUCTS],
    },
    "ota_product": OTA_PRODUCT,
}
must(FW_README, "pio device monitor", "monitor command")
must(GUIDE, "never the module's", "port rule")

# --------------------------------------------------------------------------- #
# 11. placement + use-case presets (authored; every number inside the
#     firmware's own tunable bounds, every concept anchored in the docs)
# --------------------------------------------------------------------------- #


def preset(score, lost_ms, dwell_ms):
    lo, hi = BOUNDS["score"]
    if not (lo <= score <= hi):
        die(f"preset score {score} outside firmware bounds")
    lo, hi = BOUNDS["lost_ms"]
    if not (lo <= lost_ms <= hi):
        die(f"preset lost_ms {lost_ms} outside firmware bounds")
    lo, hi = BOUNDS["dwell_ms"]
    if not (lo <= dwell_ms <= hi):
        die(f"preset dwell_ms {dwell_ms} outside firmware bounds")
    return {"score": score, "lost_ms": lost_ms, "dwell_ms": dwell_ms}


PLACEMENT = {
    "fov_note": "The recommended OV5647-62 sees a 62° field — about the width of a doorway "
                "from 2 m back, a whole small room from a corner. The -160 fisheye trades "
                "reach for coverage and distorts edges the model wasn't trained on; keep it "
                "for tight spaces.",
    "rules": [
        {"k": "Height", "v": "1.8–2.2 m, tilted slightly down — faces and full bodies in "
         "frame, not the tops of heads. Straight-on at standing height is what person "
         "models are trained on."},
        {"k": "Light", "v": "Aim with the light, not into it. A window or lamp behind the "
         "subject silhouettes them and scores drop; the same person front-lit scores 20 "
         "points higher."},
        {"k": "Backdrop", "v": "Avoid TVs and monitors in frame — a person on screen is a "
         "person to the model. If unavoidable, let the score threshold and dwell filter "
         "do the work: screen-people flicker, real people persist."},
        {"k": "Depth", "v": f"Best detection from 1–4 m. Beyond ~6 m a standing person is "
         f"under 40 px tall in the {FRAME_W}×{FRAME_H} frame and scores fall off fast."},
        {"k": "Pets", "v": "Under the default room_presence profile a cat or dog is not "
         "the subject class — the class filter drops it before the score threshold even "
         "looks. The rare crawling-dog false positive sits at low score; the default "
         "threshold already eats it. (The litter_box watch profile inverts this on "
         "purpose: load an animal model and the cat IS the subject.)"},
        {"k": "Mounting", "v": "The witness never pans: pick the one view whose voxel grid "
         "answers your question — the door cell, the safe cell, the hallway lane — and "
         "let the Aim card prove it before you drill."},
    ],
    "use_cases": [
        {"id": "entry", "icon": "🚪", "title": "Entryway / front door",
         "blurb": "Presence at the door is the event; nobody dwells in a doorway. Fast lost "
                  "timeout, short dwell — the voxel column tells you in/out of the frame.",
         "preset": preset(75, 1000, 5000)},
        {"id": "living", "icon": "🛋️", "title": "Living room / shared space",
         "blurb": "Occupancy is the question, not motion. Longer lost timeout rides through "
                  "stillness (reading, TV); dwelling becomes the useful signal.",
         "preset": preset(70, 4000, 30000)},
        {"id": "hall", "icon": "🚶", "title": "Hallway / corridor",
         "blurb": "People transit fast — a low dwell would never latch anyway. Default "
                  "score, snappy lost timeout, dwell long so a lingerer stands out.",
         "preset": preset(70, 1000, 60000)},
        {"id": "workshop", "icon": "🧰", "title": "Garage / workshop",
         "blurb": "Harsh light and clutter: raise the score floor to shrug off shadows and "
                  "shapes; dwell marks a real work session.",
         "preset": preset(80, 3000, 60000)},
        {"id": "office", "icon": "💻", "title": "Home office",
         "blurb": "A monitor is in frame by definition — raise the threshold, trust dwell. "
                  "Screen-people flicker below it; you at the desk persist above it.",
         "preset": preset(85, 5000, 15000)},
        {"id": "litter", "icon": "🐈", "title": "Litter box (watch profile)",
         "blurb": "Load a cat-detection model in SenseCraft, pick the litter_box watch "
                  "profile, done: lower score floor (a mid-dig cat is a strange shape), "
                  "long lost timeout so digging doesn't fragment one visit into five, "
                  "short dwell so a real visit latches. interaction_likely = visit "
                  "completed. The space is already lit — aim across the box, not into "
                  "the lamp.",
         "preset": preset(LITTER["score"], LITTER["lost_ms"], LITTER["dwell_ms"])},
    ],
    "no_outdoor": "Indoor witness. The module and camera are unsealed boards; weather, IR "
                  "floodlights and headlights are out of scope — the fence line belongs to "
                  "Canary Fence Guard.",
}

# --------------------------------------------------------------------------- #
# 12. tuning — the four live numbers (README table is the source of truth)
# --------------------------------------------------------------------------- #

tune_rows = md_table(readme, "| Setting | JSON key | Range |", "tuning table", 4)
TUNING = [{"setting": strip_md(r[0]), "key": strip_md(r[1]), "range": strip_md(r[2]),
           "why": strip_md(r[3])} for r in tune_rows[1:]]
if len(TUNING) != 5:
    die("expected exactly 5 runtime tuning settings (watch profile + 4 numbers)")
if TUNING[0]["key"] != "profile":
    die("tuning table must lead with the watch profile row")
must(GETTING_STARTED, "False positives → raise; missed detections → lower", "score tuning advice")

# --------------------------------------------------------------------------- #
# 13. sandbox — staged scenes; every claim traces to a firmware rule
# --------------------------------------------------------------------------- #

SANDBOX = [
    {"id": "walk", "label": "Walk into frame", "icon": "🚶",
     "blurb": "One person crosses the room. The box locks on, presence_started fires, the "
              "voxel cell tracks the walk.",
     "event": "presence_started"},
    {"id": "linger", "label": "Stay a while", "icon": "🧍",
     "blurb": f"Stand still past the dwell timer ({DWELL_START_MS // 1000} s default) — "
              "dwell_started fires; stillness is not absence. Leave afterwards and the "
              "qualified visit signs interaction_likely (dwell_then_left).",
     "event": "dwell_started"},
    {"id": "leave", "label": "Leave the frame", "icon": "👋",
     "blurb": f"The box drops; after the lost timeout ({LOST_TIMEOUT_MS} ms default) "
              "presence_ended closes the visit.",
     "event": "presence_ended"},
    {"id": "cat", "label": "Send the cat through", "icon": "🐈",
     "blurb": "The model may box it — but it isn't the subject class, so the class filter "
              "drops it before the score check even runs. Nothing publishes. (Flip the "
              "litter_box watch profile with an animal model loaded and the cat becomes "
              "the subject instead.)",
     "event": None},
    {"id": "two", "label": "Two people at once", "icon": "🧑‍🤝‍🧑",
     "blurb": "Both get boxes on-module; the firmware keeps the best-scoring person box. "
              "One claim — someone is here — not a headcount.",
     "event": "presence_started"},
    {"id": "tv", "label": "Turn the TV on", "icon": "📺",
     "blurb": "A face on screen scores low and flickers. Below the threshold nothing "
              "happens; near it, you'll see why the office preset raises the floor.",
     "event": None},
]
for sc in SANDBOX:
    if sc["event"] and sc["event"] not in DETECT["events"]:
        die(f"sandbox scenario {sc['id']} claims unknown event {sc['event']}")

# --------------------------------------------------------------------------- #
# 14. the flasher roadmap (honest: what ships today vs what's in the nest)
# --------------------------------------------------------------------------- #

ROADMAP = {
    "today": "The Lab's own module flasher lives on the flash page: the pinned person-"
             "detection model, fetched from the project's release manifest, SHA-256-"
             "verified, burned to the module over WebSerial, then proven with an AT "
             "handshake and a live bench preview — no vendor site, no account, no "
             "choices to get wrong.",
    "next": "SenseCraft remains the documented fallback (this page stages every click of "
            "it), and the firmware's runtime class-index setting means even a different "
            "model never needs a rebuild.",
    "feasibility": "The engine is a clean-room mirror of the public protocol: XMODEM/"
                   "CRC-16 at 921600 baud with the module's ROM-bootloader burn menu — "
                   "the same wire Seeed's open-source flasher speaks — and the model-zoo "
                   "person model is MIT-licensed, redistributed with attribution and "
                   "pinned per release (manifest-vision-model.json).",
    "status": "beta — live on flash.html",
}

# --------------------------------------------------------------------------- #
# 15. troubleshooting — the symptom tables, verbatim from the docs
# --------------------------------------------------------------------------- #

sym_rows = md_table(guide, "| Symptom | Likely cause / fix |", "guide symptom table", 4)
TROUBLE = [{"symptom": strip_md(r[0]), "fix": strip_md(r[1])} for r in sym_rows[1:]]
gs_rows = md_table(gs, "| Symptom | Fix |", "getting-started symptom table", 4)
for r in gs_rows[1:]:
    sym = strip_md(r[0])
    if not any(t["symptom"][:24] == sym[:24] for t in TROUBLE):
        TROUBLE.append({"symptom": sym, "fix": strip_md(r[1])})
if len(TROUBLE) < 6:
    die(f"only {len(TROUBLE)} troubleshooting rows parsed")

RECOVERY = {
    "boot_reset": [
        "BootLoader mode: hold Boot, plug in USB, release. (Or, while connected: hold "
        "Boot, tap Reset.) Needed for stubborn flashing sessions.",
        "Reset: taps the HX6538 if preview freezes or results stop.",
    ],
    "i2c_rescue": "A bricked module bootloader can be restored through the host MCU: flash "
                  "the we2_iic_bootloader_recover example from the Seeed_Arduino_SSCMA "
                  "library onto the ESP32, connect over I2C, and press Enter when detected. "
                  "Hold the module's BOOT button while connecting power; 3–10 attempts is "
                  "normal per Seeed.",
    "factory": "Factory module firmware: Seeed's factory flasher bundle, or re-flash via "
               "the SenseCraft process page.",
}
must(GUIDE, "we2_iic_bootloader_recover", "i2c recovery example name")
must(GUIDE, "3–10 is normal per Seeed", "recovery attempt count")

# --------------------------------------------------------------------------- #
# 16. assemble + write
# --------------------------------------------------------------------------- #

out = {
    "$note": "GENERATED by canary-local/tools/gen_vision.py from the canary-vision firmware "
             "+ the Grove Vision AI V2 docs. Do not edit by hand; run the generator. "
             "Drift-gated in .github/workflows/canary-local.yml.",
    "generated_by": "canary-local/tools/gen_vision.py",
    "device": {
        "id_example": DEVICE_ID,
        "name": vis_reg.get("name", "Canary Vision"),
        "product_name": "SecuraCV Canary Vision",
        "device_type": DEVICE_TYPE,
        "tagline": vis_reg.get("tagline", ""),
        "model": MODEL,
        "board_short": "Grove Vision AI V2",
        "board_id": BOARD_ID,
        "fw_version": FW_VERSION,
        "fw_train": FW_TRAIN,
        "ota_product": OTA_PRODUCT,
        "senses": vis_reg.get("senses", []),
        "modality": vis_reg.get("modality", "camera"),
        "network": vis_reg.get("network", {}),
        "status": vis_reg.get("status", ""),
        "docs": vis_reg.get("docs", []),
        "hosts": HOSTS,
        "needs": NEEDS,
    },
    "module": MODULE,
    "protocol": PROTOCOL,
    "ports": {
        "table": TWO_PORTS,
        "rule": "Model work → module port. Firmware work → XIAO port. Neither port can do "
                "the other's job: the module port cannot see the ESP32, and the XIAO port "
                "cannot reach the Himax flash.",
        "power": "You never need both cables at once for normal operation. Powering either "
                 "port powers the stacked pair.",
    },
    "assembly": ASSEMBLY,
    "grove_pins": GROVE_PINS,
    "model_load": MODEL_LOAD,
    "detect": DETECT,
    "serial": SERIAL,
    "mqtt": MQTT,
    "aim": AIM,
    "flash": FLASH,
    "placement": PLACEMENT,
    "tuning": TUNING,
    "sandbox": SANDBOX,
    "roadmap": ROADMAP,
    "troubleshooting": TROUBLE,
    "recovery": RECOVERY,
    "docs": {
        "getting_started": "docs/hardware/canary_vision_getting_started.md",
        "device_guide": "docs/hardware/grove_vision_ai_v2_guide.md",
        "firmware_readme": "firmware/projects/canary-vision/README.md",
        "program": "docs/strategy/10-grove-vision-ai-v2-program.md",
        "registry": "canary-local/devices/registry.json",
    },
}

# sanity floors — a broken parse must fail the build, not ship thin data
if len(DISCOVERY) != len(ENTITY_META):
    die(f"{len(DISCOVERY)} HA entities parsed, expected {len(ENTITY_META)}")
if len(TOPICS) < 15:
    die(f"only {len(TOPICS)} MQTT topics")
if len(SERIAL["boot"]) < 20:
    die("boot log too short")
if len(HOSTS) < 3:
    die("host boards table parsed thin")
if len(PLACEMENT["use_cases"]) < 4:
    die("too few placement presets")
if len(MODULE_SPECS) < 5:
    die("module spec table parsed thin")

OUT_JSON.write_text(json.dumps(out, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
print(f"wrote {OUT_JSON.relative_to(REPO)}  "
      f"({len(DISCOVERY)} entities, {len(TOPICS)} topics, {len(SERIAL['boot'])} boot lines, "
      f"{len(SANDBOX)} sandbox scenes, {len(PLACEMENT['use_cases'])} presets, fw {FW_VERSION})")
