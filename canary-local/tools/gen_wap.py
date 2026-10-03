#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_wap.py — build canary-local/devices/wap.json from the canary-wap firmware.

The WAP teaching page (`canary-local/wap.html`) emulates the first-boot,
captive-portal setup, serial console and MQTT of the `canary-wap` device — the
WiFi-CSI witness on the XIAO ESP32-S3 Sense. None of it is hand-faked: every
string this page shows is either parsed straight out of the firmware/docs, or
authored here and then *validated to still exist in the source* (this file
`sys.exit(1)`s on drift). CI re-runs the generator and `git diff --exit-code`s
`wap.json`, so if the firmware changes a topic, a boot line, or a wizard label,
the page breaks the build until it is regenerated — the same anti-rot contract
as `gen_homeassistant.py` / `gen_boards.py`.

Sources of truth (all in-repo, deterministic, offline):
  firmware/projects/canary-wap/arduino/canary_wap/
      canary_wap.ino        version/identity, AP ssid+password, boot log, routes
      wap_server.h          AP channel/clients, HTTP/HTTPS ports
      setup_wizard.h        NVS keys, setup timeout, DNS port, device-name cap
      setup_page_html.h     CAPTIVE_PORTAL_HTML  (rendered verbatim)
      boot_banner.cpp       boot scene text
      csi_mqtt.cpp          MQTT prefix, topics, each topic's payload keys,
                            HA discovery entity/trigger set
      csi_event_wire.h      the events and tamper bodies' keys (the one wire builder)
      core_presence.cpp, acoustic_events_module.cpp, tamper_events_module.cpp
                            the module / type / state an events example names
      csi_event_id_floor.h  where event ids start (kIdSpaceBase)
      mqtt_identity.h       the spelling of the MQTT fp and public_key (lowercase)
  docs/getting_started_canary.md   sensing pills, dashboard cards
  canary-local/devices/registry.json   fw_train + the canary-wap card facts
  canary-local/devices/boards.json     device -> board mapping

Run:  python3 canary-local/tools/gen_wap.py
"""

import json
import re
from pathlib import Path

from _tooling import die, repo_root

REPO = repo_root()
FW = REPO / "firmware/projects/canary-wap/arduino/canary_wap"
INO = FW / "canary_wap.ino"
WAP_SERVER_H = FW / "wap_server.h"
SETUP_WIZARD_H = FW / "setup_wizard.h"
SETUP_PAGE_H = FW / "setup_page_html.h"
BOOT_BANNER_CPP = FW / "boot_banner.cpp"
CAPTIVE_PROBE_H = FW / "captive_probe.h"
CSI_MQTT_CPP = FW / "csi_mqtt.cpp"
MQTT_IDENTITY_H = FW / "mqtt_identity.h"
EVENT_WIRE_H = FW / "csi_event_wire.h"
ID_FLOOR_H = FW / "csi_event_id_floor.h"
EGRESS_H = FW / "csi_event_egress.h"
CORE_PRESENCE_CPP = FW / "core_presence.cpp"
ACOUSTIC_CPP = FW / "acoustic_events_module.cpp"
TAMPER_CPP = FW / "tamper_events_module.cpp"
COMPANION_H = FW / "companion_pwa.h"
DOC = REPO / "docs/getting_started_canary.md"
REGISTRY = REPO / "canary-local/devices/registry.json"
BOARDS = REPO / "canary-local/devices/boards.json"
OUT_JSON = REPO / "canary-local/devices/wap.json"

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
    """Assert a literal exists in at least one of several source files."""
    if not any(needle in read(p) for p in paths):
        where = ", ".join(p.name for p in paths)
        die(f"{label}: expected to find {needle!r} in one of [{where}] — firmware changed?")


def grab(path: Path, pattern: str, label: str, flags=0) -> str:
    m = re.search(pattern, read(path), flags)
    if not m:
        die(f"{label}: pattern /{pattern}/ not found in {path.relative_to(REPO)}")
    return m.group(1)


def fn_body(path: Path, signature: str, label: str) -> str:
    """A function's text, from its signature to the closing brace in column 0."""
    text = read(path)
    i = text.find(signature)
    if i < 0:
        die(f"{label}: {signature!r} not found in {path.relative_to(REPO)}")
    j = text.find("\n}\n", i)
    return text[i:j if j >= 0 else len(text)]


def between(text: str, start: str, end: str, label: str) -> str:
    i = text.find(start)
    j = text.find(end, i + len(start)) if i >= 0 else -1
    if i < 0 or j < 0:
        die(f"{label}: {start!r} … {end!r} not found")
    return text[i:j]


def fmt_keys(fragment: str) -> list:
    """The JSON keys a C format string writes, in order."""
    return re.findall(r'\\"([a-z_0-9]+)\\":', fragment)


def keyed_as(payload: str, keys: list, label: str, optional=()) -> str:
    """Refuse an example whose keys are not the firmware's, in its order.
    `optional` keys are ones the firmware writes only sometimes; an example
    may carry any of them, in their place."""
    try:
        ex = list(json.loads(payload).keys())
    except ValueError:
        die(f"MQTT {label}: example is not JSON: {payload}")
    want = [k for k in keys if k not in optional or k in ex]
    if ex != want:
        die(f"MQTT {label}: example keys {ex} are not the firmware's {want}")
    return payload


# --------------------------------------------------------------------------- #
# 1. identity + version (parsed straight from the firmware)
# --------------------------------------------------------------------------- #

FW_VERSION = grab(INO, r'FIRMWARE_VERSION\s*=\s*"([^"]+)"', "FIRMWARE_VERSION")
DEVICE_TYPE = grab(INO, r'DEVICE_TYPE\s*=\s*"([^"]+)"', "DEVICE_TYPE")
OTA_PRODUCT = grab(INO, r'OTA_PRODUCT\s*=\s*"([^"]+)"', "OTA_PRODUCT")

registry = json.loads(read(REGISTRY))
FW_TRAIN = registry.get("fw_train")
if not FW_TRAIN:
    die("registry.json has no fw_train")
if not FW_VERSION.startswith(FW_TRAIN):
    die(f"firmware version {FW_VERSION!r} does not ride registry fw_train {FW_TRAIN!r}")

wap_reg = next((d for d in registry.get("devices", []) if d.get("id") == "canary-wap"), None)
if not wap_reg:
    die("registry.json has no canary-wap device entry")

boards = json.loads(read(BOARDS))
# device_board maps a device to a LIST of boards (primary first); the WAP's
# main board is the first. Tolerate the legacy single-string form too.
_wap_board = boards.get("device_board", {}).get("canary-wap")
BOARD_ID = _wap_board[0] if isinstance(_wap_board, list) and _wap_board else _wap_board
if not BOARD_ID:
    die("boards.json device_board has no canary-wap mapping")
BOARD_NAME_FULL = boards.get("boards", {}).get(BOARD_ID, {}).get("name", "")

# --------------------------------------------------------------------------- #
# 2. the SoftAP + captive portal (constants parsed; behavior validated)
# --------------------------------------------------------------------------- #

AP_SSID_PREFIX = grab(WAP_SERVER_H, r'AP_SSID_PREFIX\s*=\s*"([^"]+)"', "AP_SSID_PREFIX")
AP_CHANNEL = int(grab(WAP_SERVER_H, r"AP_CHANNEL\s*=\s*(\d+)", "AP_CHANNEL"))
AP_MAX_CLIENTS = int(grab(WAP_SERVER_H, r"AP_MAX_CLIENTS\s*=\s*(\d+)", "AP_MAX_CLIENTS"))
HTTPS_PORT = int(grab(WAP_SERVER_H, r"HTTPS_PORT\s*=\s*(\d+)", "HTTPS_PORT"))
HTTP_PORT = int(grab(WAP_SERVER_H, r"HTTP_PORT\s*=\s*(\d+)", "HTTP_PORT"))
DEVICE_NAME_MAX = int(grab(SETUP_WIZARD_H, r"DEVICE_NAME_MAX\s*=\s*(\d+)", "DEVICE_NAME_MAX"))
DNS_PORT = int(grab(SETUP_WIZARD_H, r"DNS_PORT\s*=\s*(\d+)", "DNS_PORT"))
NVS_NS = grab(SETUP_WIZARD_H, r'prefs\.begin\("([a-z]+)"', "NVS namespace")
NVS_KEY = grab(SETUP_WIZARD_H, r'getBool\("([a-z_]+)"', "NVS setup key")
AP_GRACE_MS = int(grab(INO, r"AP_DROP_GRACE_MS\s*=\s*(\d+)", "AP_DROP_GRACE_MS"))
SETUP_TIMEOUT_MS = 15 * 60 * 1000
must(SETUP_WIZARD_H, "SETUP_TIMEOUT_MS = 15UL * 60UL * 1000UL", "setup timeout")

# AP ssid/password format are the load-bearing "how do I connect" facts.
must(INO, '"SecuraCV-%s", suffix', "AP ssid format")
must(INO, '"cv-%s", encoded', "AP password format")
must(INO, "securacv:ap-password:v1", "AP password derivation domain")
must(INO, "WiFi.softAP(ap_ssid, ap_pass, AP_CHANNEL, false, AP_MAX_CLIENTS)", "softAP call")

# The captive landing page is rendered verbatim — this IS the firmware's page.
CAPTIVE_HTML = grab(SETUP_PAGE_H, r'R"HTML\((.*)\)HTML"', "CAPTIVE_PORTAL_HTML", re.DOTALL)
for needle in ("Set up your Canary", "canary.local", "192.168.4.1"):
    if needle not in CAPTIVE_HTML:
        die(f"captive HTML missing expected copy {needle!r}")

# OS captive-probe endpoints + exact success tokens (host-tested contract).
must(CAPTIVE_PROBE_H, "Microsoft NCSI", "Windows NCSI body")
must(CAPTIVE_PROBE_H, "Microsoft Connect Test", "Windows connecttest body")

# The fingerprint: the repo's Ed25519 test key's (seed 0x42 x 32, public key
# 2152f8d1...81db12), the key the WAP's tests_host/test_mqtt_identity.cpp and
# Home Assistant's tests/test_fingerprint_case.py sign with, so the page, the
# firmware test and HA show one fingerprint (tests/fingerprint_examples.test.js
# derives it from the seed). Two spellings of the same 8 bytes of pubkey_fp:
# the envelope `fp` of every signed publish is mqtt_identity.h's lowercase
# (sweep HA20; firmware 2.4.15 and older sent capitals), and the [PROV] boot
# line prints g_device.fingerprint_hex, which hex_to_str spells in capitals.
EX_FP = "7916ca487912fa1b"
EX_FP_SERIAL = EX_FP.upper()

# Every other identity example on the page is the same key's (sweep A29), as
# the firmware derives each one from pubkey_fp[0..1]:
#   device id  generate_device_id: DEVICE_ID_PREFIX + unambiguous_suffix16
#   SSID       generate_ap_ssid:   "SecuraCV-" + the same suffix, same case
#   mDNS host  generate_mdns_hostname, with no friendly name set:
#              "canary-%02x%02x", four lowercase hex digits, no "-s3-"
# so the page shows the device id Home Assistant's tests use (DEVICE_ID,
# canary-s3-4dC2), the SSID that device raises, and the host it advertises
# until it is named (canary-<name> after). A name the page used to show
# (canary-ab7k.local) is one only a friendly name could produce. The AP
# password stays illustrative: it is HMAC-derived from the PRIVATE key, a
# credential rather than an identity, and no host test pins that derivation.
UNAMBIGUOUS = grab(INO, r'UNAMBIGUOUS_ALPHABET\[\] =\s*"([^"]+)";', "UNAMBIGUOUS_ALPHABET")
DEVICE_ID_PREFIX = grab(INO, r'#else\s*static const char\* DEVICE_ID_PREFIX = "([^"]+)";',
                        "the S3 DEVICE_ID_PREFIX")
_FP_SUFFIX_CALL = ("unambiguous_suffix16((uint16_t)((g_device.pubkey_fp[0] << 8) | "
                   "g_device.pubkey_fp[1]),\n                       suffix);")
if read(INO).count(_FP_SUFFIX_CALL) != 2:
    die("generate_device_id and generate_ap_ssid no longer both encode pubkey_fp[0..1] — "
        "re-derive the identity examples")
must(INO, 'snprintf(out, cap, "%s%s", DEVICE_ID_PREFIX, suffix)', "device id format")
must(INO, "out[i] = UNAMBIGUOUS_ALPHABET[v % UNAMBIGUOUS_LEN];", "unambiguous_suffix16 digit")
must(INO, "v = (uint16_t)(v / UNAMBIGUOUS_LEN);", "unambiguous_suffix16 radix step")
must(INO, 'snprintf(out, cap, "canary-%02x%02x",\n           g_device.pubkey_fp[0], g_device.pubkey_fp[1]);',
     "unnamed mDNS host fallback")
if AP_SSID_PREFIX != "SecuraCV-":
    die(f"AP_SSID_PREFIX {AP_SSID_PREFIX!r} is not generate_ap_ssid's literal \"SecuraCV-\"")


def unambiguous_suffix16(v: int) -> str:
    """canary_wap.ino's unambiguous_suffix16: four base-54 digits, least first."""
    out = ""
    for _ in range(4):
        out += UNAMBIGUOUS[v % len(UNAMBIGUOUS)]
        v //= len(UNAMBIGUOUS)
    return out


_FP0, _FP1 = bytes.fromhex(EX_FP)[:2]
EX_SUFFIX = unambiguous_suffix16((_FP0 << 8) | _FP1)
EX_ID = f"{DEVICE_ID_PREFIX}{EX_SUFFIX}"
EX_SSID = f"{AP_SSID_PREFIX}{EX_SUFFIX}"
EX_PASS = "cv-7Q2M9XKP4RTN"
EX_MDNS = "canary-%02x%02x.local" % (_FP0, _FP1)
must(MQTT_IDENTITY_H, 'kLowerHex[] = "0123456789abcdef"', "envelope fp spelled in lowercase")
must(INO, "mqtt_identity::fingerprint_hex(mqtt_fp_hex, g_device.pubkey_fp);",
     "envelope fp spelled by mqtt_identity::fingerprint_hex")
must(INO, 'static const char hex[] = "0123456789ABCDEF";', "hex_to_str spells capitals")
must(INO, "hex_to_str(g_device.fingerprint_hex, g_device.pubkey_fp, 8);",
     "the boot line's fingerprint is hex_to_str of the 8-byte pubkey_fp")
must(INO, 'Serial.printf("[PROV] Public key fingerprint: %s\\n", g_device.fingerprint_hex);',
     "[PROV] fingerprint boot line")

AP = {
    "ssid_example": EX_SSID,
    "ssid_prefix": AP_SSID_PREFIX,
    "ssid_note": "the last 4 chars are the first two bytes of your device's pubkey fingerprint "
                 "in the unambiguous alphabet (no 0/O/o, 1/I/i/l/L), the same suffix, in the same "
                 "case, as its device id — never the MAC",
    "password_example": EX_PASS,
    "password_scheme": 'WPA2-PSK, device-unique: HMAC-SHA256(privkey, "securacv:ap-password:v1") -> "cv-" + 12 chars',
    "password_note": "printed on the serial console at first boot and on the box card; no shared default; release builds fail closed",
    "ip": "192.168.4.1",
    "channel": AP_CHANNEL,
    "max_clients": AP_MAX_CLIENTS,
    "hidden": False,
    "http_port": HTTP_PORT,
    "https_port": HTTPS_PORT,
    "mdns": ["canary.local", "canary-<name>.local", "canary-<4 hex>.local"],
    "mdns_example": EX_MDNS,
    "mdns_services": [
        {"service": "_http._tcp", "port": 80},
        {"service": "_securacv._tcp", "port": 80,
         "txt": ["device_id", "fw", "host", "name", "model", "dt=canary-wap", "role=witness"]},
    ],
    "nvs_namespace": NVS_NS,
    "nvs_key": NVS_KEY,
    "device_name_max": DEVICE_NAME_MAX,
    "setup_timeout_min": SETUP_TIMEOUT_MS // 60000,
    "ap_grace_sec": AP_GRACE_MS // 1000,
}

CAPTIVE = {
    "html": CAPTIVE_HTML,
    "dns": {"port": DNS_PORT, "a_ttl": 60,
            "note": "captive DNS answers every A query with 192.168.4.1 (TTL 60s); AAAA/HTTPS queries get NODATA so clients fall back to IPv4"},
    "probes": [
        {"os": "Apple", "paths": ["/hotspot-detect.html", "/library/test/success.html"],
         "resp": "200 + the captive page", "effect": "pops the 'Sign in' sheet, keeps WiFi up"},
        {"os": "Android", "paths": ["/generate_204", "/gen_204"],
         "resp": "204 No Content", "effect": "marks the AP online, no cellular fallback (no pop-up — normal)"},
        {"os": "Windows", "paths": ["/connecttest.txt"], "resp": "Microsoft Connect Test", "effect": "marks the network reachable"},
        {"os": "Windows", "paths": ["/ncsi.txt"], "resp": "Microsoft NCSI", "effect": "marks the network reachable"},
    ],
}

# --------------------------------------------------------------------------- #
# 3. HTTP routes the captive portal actually drives (authored, validated)
# --------------------------------------------------------------------------- #

ROUTES = [
    {"method": "GET", "path": "/", "kind": "html",
     "desc": "first boot + a direct host (canary.local/192.168.4.1) -> 302 to /companion?token=<64hex>; a foreign host gets the static captive page"},
    {"method": "GET", "path": "/companion", "kind": "html", "desc": "the setup wizard SPA (served gzipped)"},
    {"method": "GET", "path": "/api/wifi/scan", "kind": "json",
     "desc": "list of networks the Canary can see; ?force=1 rescans", "auth": "open (setup)"},
    {"method": "GET", "path": "/api/wifi/pair-token", "kind": "json",
     "desc": "re-issues the 64-hex pairing token (only while setup is active + a direct host)", "auth": "open (setup)"},
    {"method": "POST", "path": "/api/wifi/connect", "kind": "json",
     "desc": "body {ssid,password,token}: save creds, join home WiFi", "auth": "pairing token"},
    {"method": "POST", "path": "/api/wifi/ap-only", "kind": "json",
     "desc": "body {token}: run standalone, keep the SecuraCV network", "auth": "pairing token"},
    {"method": "GET", "path": "/api/wifi", "kind": "json",
     "desc": "the status the wizard polls (ap/sta state, rssi, fail_reason...)", "auth": "open (setup)"},
    {"method": "GET", "path": "/api/selftest", "kind": "json",
     "desc": "pre-flight checks (Wi-Fi/Camera/Bluetooth/SD/Microphone/GPIO)", "auth": "open (setup)"},
    {"method": "GET/POST", "path": "/api/mqtt/config", "kind": "json",
     "desc": "optional Home Assistant broker host/port/user/pass", "auth": "pairing token"},
    {"method": "POST", "path": "/api/mqtt/test", "kind": "json", "desc": "test the broker connection", "auth": "pairing token"},
]
# validate the setup-critical routes still exist as string literals in the firmware
for r in ("/api/wifi/scan", "/api/wifi/connect", "/api/wifi/ap-only", "/api/wifi/pair-token", "/companion"):
    must(INO, r, f"route {r}")

# --------------------------------------------------------------------------- #
# 4. the five-step wizard (authored; each title validated in companion_pwa.h)
# --------------------------------------------------------------------------- #

WIZARD = {
    "redirect": "/companion?token=<64hex>",
    "pair_token": "64 hex chars (32 bytes), ~10 min TTL, wiped on reboot; the wizard silently refreshes + retries once on an expired token",
    "steps": [
        {"n": 1, "title": "Connect your Canary to WiFi",
         "sub": "Have your home WiFi password ready. Stay on this network until setup finishes.",
         "actions": ["Let's go", "I have a WiFi QR code", "Use without home WiFi"]},
        {"n": 2, "title": "Pick your home WiFi",
         "sub": "These are the networks your Canary can see. Pick the one it should join.",
         "api": "GET /api/wifi/scan", "actions": ["Scan again"]},
        {"n": 3, "title": "Type the password",
         "sub": "Joining <ssid>. Your password is sent only to the Canary.",
         "fields": [{"name": "password", "type": "password", "placeholder": "Network password"}],
         "actions": ["Back", "Connect"]},
        {"n": 4, "title": "Connecting…",
         "sub": "Talking to your Canary. Waiting for your home WiFi.",
         "api": "POST /api/wifi/connect -> poll GET /api/wifi",
         "outcomes": [
             {"kind": "success", "title": "Your Canary is online.",
              "body": "Joined <ssid>. The SecuraCV setup network turns itself off in about two minutes — reconnect this phone to your home WiFi and find your Canary at canary.local."},
             {"kind": "standalone", "title": "Running standalone.",
              "body": "Your Canary keeps its own SecuraCV network — no home WiFi needed, nothing leaves the device."},
             {"kind": "failure", "title": "Couldn't connect",
              "body": "Wrong password. Check for caps lock and try again."},
         ]},
        {"n": 5, "title": "Pre-flight checks",
         "sub": "Confirming the sensors are awake. About a second.",
         "api": "GET /api/selftest",
         "checks": ["Wi-Fi", "Camera", "Bluetooth", "SD", "Microphone", "GPIO"],
         "blocks": [
             {"title": "Save your recovery kit", "detail": "Short-press BOOT, then tap Save my recovery kit (downloads canary-recovery-kit.json)"},
             {"title": "Use Home Assistant? (optional)",
              "fields": [
                  {"name": "host", "placeholder": "Broker address (like 192.168.1.10)"},
                  {"name": "port", "placeholder": "Port (1883)"},
                  {"name": "user", "placeholder": "Username (optional)"},
                  {"name": "pass", "placeholder": "Password (optional)"},
              ], "actions": ["Skip", "Test & save"]},
             {"title": "Most homes use 3 or 4.", "detail": "Placement tips + Set up another / I'm done for now"},
         ],
         "actions": ["Finish"]},
    ],
}
for s in WIZARD["steps"]:
    must(COMPANION_H, s["title"], f"wizard step {s['n']} title")
must(COMPANION_H, "Your Canary is online.", "wizard success title")
must(COMPANION_H, "Running standalone.", "wizard standalone title")
must(COMPANION_H, "canary-recovery-kit.json", "recovery kit file")

# --------------------------------------------------------------------------- #
# 5. the serial console — boot banner + ordered setup() log (validated)
# --------------------------------------------------------------------------- #

# boot scene text lives in boot_banner.cpp; validate the load-bearing lines.
for needle in ("Waking up...", "This is your privacy witness device.",
               "Checking the hardware...", "The canary is singing. Everything is working."):
    must(BOOT_BANNER_CPP, needle, "boot banner")

BANNER = [
    "              ,_,          Waking up...",
    "             (o.o)",
    "             /| |\\         SecuraCV Canary WAP",
    f"              d b          v{FW_VERSION}",
    "",
    "    This is your privacy witness device.",
    "    It creates tamper-proof records of what it",
    "    sees, so nobody can change the story later.",
    "",
    f"    Type        {DEVICE_TYPE}",
    f"    Model       {BOARD_NAME_FULL or 'XIAO ESP32S3 Sense'}",
    "    ------------------------------------------------",
    "",
    "              ,_,",
    "             (o.o) ?       Checking the hardware...",
    "             (  >)",
    "              \" \"          What am I running on?",
    "    ------------------------------------------------",
    "    Board       XIAO ESP32S3",
    "    Chip        ESP32-S3 rev 0",
    "    CPU         240 MHz, 2 core(s)",
    "    Flash       8 MB",
    "    PSRAM       8192 KB total, 8188 KB free",
    "    Heap        360 KB free at boot",
    "",
]

# Ordered setup() serial log. `text` is the exact literal (static prefix
# validated against the .ino); {vars} render illustrative values.
BOOT = [
    {"tag": "[PROV]", "text": "Provisioning device identity..."},
    {"tag": "[PROV]", "text": "Loaded existing keypair from NVS"},
    {"tag": "[PROV]", "text": f"Public key fingerprint: {EX_FP_SERIAL}"},
    {"tag": "[PROV]", "text": f"Device ID: {EX_ID}"},
    {"tag": "[PROV]", "text": "Boot count: 5"},
    {"tag": "[..]", "text": "Initializing camera for peek/preview..."},
    {"tag": "[OK]", "text": "Camera ready for peek", "src": "[OK] Camera ready for peek"},
    {"tag": "[..]", "text": "Initializing SD card storage (with timeout)..."},
    {"tag": "[OK]", "text": "SD card ready for witness records", "src": "[OK] SD card ready for witness records"},
    {"tag": "[..]", "text": "Initializing PDM acoustic event detection..."},
    {"tag": "[OK]", "text": "Acoustic detector armed (T3 smoke / T4 CO)", "src": "[OK] Acoustic detector armed (T3 smoke / T4 CO)"},
    {"tag": "[..]", "text": "SETUP MODE: HTTP-only so the captive portal renders", "src": "SETUP MODE: HTTP-only so the captive portal renders"},
    {"tag": "[..]", "text": "Starting WiFi Access Point...", "src": "[..] Starting WiFi Access Point..."},
    {"tag": "[WIFI]", "text": f"AP started: {EX_SSID} (password: {EX_PASS})", "src": "[WIFI] AP started:"},
    {"tag": "[..]", "text": "Starting API server...", "src": "[..] Starting API server..."},
    {"tag": "[MQTT]", "text": "disabled in NVS — bridge not started", "src": "[MQTT] disabled in NVS"},
    {"tag": "[..]", "text": "Initializing mesh network (opera)...", "src": "Initializing mesh network"},
    {"tag": "[OK]", "text": "Mesh network initialized", "src": "[OK] Mesh network initialized"},
    {"tag": "[OK]", "text": "Community chirp channel ready (disabled until enabled)", "src": "[OK] Community chirp channel ready (disabled until enabled)"},
    {"tag": "[..]", "text": "Bluetooth/BLE bring-up deferred until the join window clears", "src": "[..] Bluetooth/BLE bring-up deferred until the join window clears"},
    {"tag": "[OK]", "text": "RF presence ready (disabled — enable from the RF tab)", "src": "[OK] RF presence ready (disabled"},
    {"tag": "[OK]", "text": f"Pull-OTA engine ready (product={OTA_PRODUCT}, version={FW_VERSION})", "src": "[OK] Pull-OTA engine ready"},
]
for step in BOOT:
    src = step.get("src")
    if src:
        must_any([INO, CSI_MQTT_CPP, BOOT_BANNER_CPP], src, f"boot log {src!r}")
        del step["src"]

# The "ready" scene (device block) + the join-time line.
READY = [
    "    ================================================",
    "",
    "                   ,_,",
    "                  (o.o)  ~~",
    "                 /(> <)\\ ~~~~",
    "                  d | b  ~~~~~~",
    "",
    "    The canary is singing. Everything is working.",
    "",
    "    It will now create a signed witness record",
    "    every second and store it to the SD card.",
    "",
    "    ================================================",
    "",
    "    ------------------------------------------------",
    f"    Device ID   {EX_ID}",
    f"    WiFi AP     {EX_SSID}",
    f"    Password    {EX_PASS}",
    "    Dashboard   http://192.168.4.1",
    "",
    "    Commands: h=help i=id s=stat t=time g=gps c=cam m=sys r=data b=bat",
    "    Hold  BOOT (>3s)   = factory reset",
    "    ------------------------------------------------",
]
must(INO, "[OK] Setup complete — WiFi connected; rebooting after the provisioning grace window...", "setup-complete line")

SERIAL = {
    "baud": 115200,
    "banner": BANNER,
    "boot": BOOT,
    "ready": READY,
    "join_line": "[OK] Setup complete — WiFi connected; rebooting after the provisioning grace window...",
    "tags": {
        "[OK]": "done", "[..]": "in progress", "[--]": "absent / disabled",
        "[WARN]": "warning", "[WIFI]": "wifi", "[MQTT]": "mqtt", "[PROV]": "provisioning", "[HEAP]": "heap",
    },
    "runtime_note": "in steady state the console is dominated by the 1 Hz witness-record table and a STATUS bar every 20 records; most subsystem events go to the signed health log + MQTT, not serial.",
    "runtime_header": "|  seq | type | state  | ok |     lat     |     lon     |     chain        |",
    "runtime_rows": [
        "| 1234 | WEVT |  quiet | OK |      -0.000 |      -0.000 | a1b2c3d4e5f6a7b8 |",
        "| 1235 | WEVT | motion | OK |      -0.000 |      -0.000 | b2c3d4e5f6a7b8c9 |",
    ],
    "status_bar": [
        "╔═══ STATUS ══╦═════════════╗",
        "║ Uptime: 0h05m12s ║  Records: 312  |  GPS: no-fix  |  SD: ready  |  WiFi: 1",
        "╚════════════╩═════════════╝",
    ],
}

# --------------------------------------------------------------------------- #
# 6. MQTT — topics, payloads, LWT, Home Assistant discovery (validated)
# --------------------------------------------------------------------------- #

MQTT_PREFIX = grab(CSI_MQTT_CPP, r'DEFAULT_PREFIX\s*=\s*"([a-z]+)"', "MQTT DEFAULT_PREFIX")
must(CSI_MQTT_CPP, '"%s/%s/%s"', "MQTT build_topic format")
must(CSI_MQTT_CPP, '{\\"online\\":false}', "MQTT LWT payload")
must(CSI_MQTT_CPP, '\\"device_type\\":\\"canary-wap\\"', "MQTT status device_type")

# Every topic's example is keyed as the firmware publishes it (sweep A32):
# its keys, in their order, are the snprintf format's (or, for the events and
# tamper bodies, csi_event_wire.h's, the one builder both trees publish
# through), and this file refuses to write when they differ. Values are
# illustrative. Before this the events example carried seven of the wire
# body's seventeen keys plus the envelope, and a state ("motion") no module
# emits.
_wire_ev = fn_body(EVENT_WIRE_H, "inline size_t build_event_body(", "events body")
EVENT_ENVELOPE_KEYS = fmt_keys(between(_wire_ev, "if (signed_ok) {", "} else {", "the signed envelope"))
_ev_fmt = between(_wire_ev, "const int n = snprintf(body, cap,", "if (n <= 0", "the events body format")
EVENT_KEYS = fmt_keys(_ev_fmt)
if EVENT_ENVELOPE_KEYS != ["v", "alg", "fp", "sig"] or not _ev_fmt.replace(" ", "").count('"%s"\n"}"'):
    die("csi_event_wire.h's body no longer ends in the v/alg/fp/sig envelope")
must(EVENT_WIRE_H, '"\\"event_type\\":\\"%s\\","', "event_type rides the body")
must(EVENT_WIRE_H, "    (unsigned long)event_id,\n    state_s,\n", "event_type is the state name")
TAMPER_KEYS = fmt_keys(fn_body(EVENT_WIRE_H, "inline size_t build_tamper_bridge_body(", "tamper body"))


def _signed(sig: str) -> list:
    return fmt_keys(between(fn_body(CSI_MQTT_CPP, sig, sig), "if (signed_ok) {", "} else {", sig))


_health = fn_body(CSI_MQTT_CPP, "void publish_health(", "health")
HEALTH_KEYS = (fmt_keys(between(_health, "} else {", "if (n <= 0", "health without a battery"))
               + fmt_keys(between(_health, "if (n <= 0", "if (len + 1", "health's tamper levels")))
HEALTH_OPTIONAL = ("sd_mounted", "enclosure_open")
_ota = fn_body(INO, "static void ota_publish_update_state() {", "update/state")
UPDATE_KEYS = list(dict.fromkeys(re.findall(r'doc\["([a-z_]+)"\]', _ota)))
# The egress topic's body (sweep F149) names the health it follows (its
# firmware version and uptime), then carries csi_event_egress::stats_json() of
# stats() under `csi_event_egress`: the canary health's object of that name,
# the planner's counters nested under `planner`.
must(CSI_MQTT_CPP, 'build_topic(topic, sizeof(topic), "egress");', "the egress topic")
must(CSI_MQTT_CPP, "csi_event_egress::stats_json(csi_event_egress::stats(), object, sizeof(object))",
     "the egress object is stats_json()")
EGRESS_BODY_KEYS = fmt_keys(fn_body(CSI_MQTT_CPP, "void publish_egress(", "egress"))
if EGRESS_BODY_KEYS[-1:] != ["csi_event_egress"]:
    die(f"egress: publish_egress() no longer ends its body in csi_event_egress: {EGRESS_BODY_KEYS}")
_egress = fmt_keys(between(read(EGRESS_H), "inline size_t stats_json(", "(unsigned long)s.dropped", "egress"))
if "planner" not in _egress:
    die("egress: stats_json() no longer nests the planner's counters under `planner`")
EGRESS_KEYS = _egress[:_egress.index("planner") + 1]
EGRESS_PLANNER_KEYS = _egress[_egress.index("planner") + 1:]
KEYS = {
    "status": fmt_keys(fn_body(CSI_MQTT_CPP, "void publish_status(", "status")),
    "events": EVENT_KEYS + EVENT_ENVELOPE_KEYS,
    "chain": _signed("void publish_chain("),
    "health": HEALTH_KEYS,
    "counts": _signed("void publish_counts("),
    "tamper": TAMPER_KEYS,
    "sensing": fmt_keys(between(read(INO), "char sensing_json[320];", "if (sn > 0", "sensing")),
    "mesh": fmt_keys(fn_body(CSI_MQTT_CPP, "void publish_mesh(", "mesh")),
    "chirp": fmt_keys(fn_body(CSI_MQTT_CPP, "void publish_chirp_state(", "chirp")),
    "beacon": fmt_keys(fn_body(CSI_MQTT_CPP, "void publish_beacon_state(", "beacon")),
    "update/state": UPDATE_KEYS,
    "egress": EGRESS_BODY_KEYS,
}
OPTIONAL = {"health": HEALTH_OPTIONAL, "update/state": ("release_url", "release_summary")}
# the bare-string topics: what csi_mqtt.cpp writes, verbatim
must(CSI_MQTT_CPP, 'const char* pl = state ? "ON" : "OFF";', "update/auto is a bare ON/OFF (published from loop(), F106)")
must(CSI_MQTT_CPP, 'const char* pl = muted ? "muted" : "live";', "mic/state is a bare muted/live")
# The health example is the FULL profile's, the build the boot log above
# narrates: an SD card mounted this boot, so sd_mounted rides every health
# publish (F41); FEATURE_TAMPER_GPIO is off in every shipped profile, so
# enclosure_open does not.
must(INO, "tamper_lv.sd_mounted = (g_hw.sd_state != SD_ABSENT) ? 1 : 0;", "sd_mounted once a card mounted")
must(FW / "build_config.h", "#define FEATURE_TAMPER_GPIO   0\n  #endif\n  #define FEATURE_WATCHDOG      1\n"
     "  #define FEATURE_STATE_LOG     1\n  #define FEATURE_MESH_NETWORK  1", "FULL ships without the tamper contact")

# Event ids start at kIdSpaceBase on every device (sweep F46), so an example
# id below it is one no WAP sends.
ID_SPACE_BASE = int(grab(ID_FLOOR_H, r"constexpr uint32_t kIdSpaceBase = (0x[0-9A-Fa-f]+)u;", "kIdSpaceBase"), 16)
EX_EVENT_ID = ID_SPACE_BASE + 1234
EX_UPTIME_S = 312

# What an events example may name: a module the WAP registers, a type its
# manifest declares, a state that type emits, and the category and privacy
# class the wire then spells (csi_event_wire.h's category_word/privacy_word).
PRESENCE_STATES = re.findall(r'"([a-z]+)"', grab(CORE_PRESENCE_CPP,
    r"const char\* STATE_NAMES\[STATE__COUNT\] = \{\s*([^}]*)\}", "core.presence's STATE_NAMES"))
must(CORE_PRESENCE_CPP, '(void)csi_event_emit("core.presence", "presence_changed", &v);', "core.presence emit")
must(CORE_PRESENCE_CPP, "  v.category       = CSI_CATEGORY_EVENT;\n  v.present_fields = CSI_FIELD_STATE_NAME", "presence rows are events")
must(ACOUSTIC_CPP, 'type_name = "smoke_alarm_t3"; state = "smoke_alarm";', "smoke row")
must(ACOUSTIC_CPP, 'type_name = "co_alarm_t4";    state = "co_alarm";', "CO row")
must(TAMPER_CPP, 'emit_kind("sd_remove")', "the SD-removed tamper kind")
EMITTERS = {
    ("core.presence", "presence_changed"): {"states": PRESENCE_STATES, "category": "event", "privacy": "p0",
                                            "csi": True},
    ("acoustic.events", "smoke_alarm_t3"): {"states": ["smoke_alarm"], "category": "anomaly", "privacy": "p0",
                                            "csi": False},
    ("acoustic.events", "co_alarm_t4"): {"states": ["co_alarm"], "category": "anomaly", "privacy": "p0",
                                         "csi": False},
}
must(EVENT_WIRE_H, 'return (c == CSI_CATEGORY_AMBIENT) ? "ambient"\n       : (c == CSI_CATEGORY_ANOMALY) ? "anomaly" : "event";',
     "category words")
must(EVENT_WIRE_H, 'return (p == CSI_PRIVACY_P2) ? "p2" : (p == CSI_PRIVACY_P1) ? "p1" : "p0";', "privacy words")


def event_body(**over) -> str:
    """An events body as csi_event_wire.h builds it, signed by the test key."""
    body = {"event_id": EX_EVENT_ID, "event_type": "", "timestamp": EX_UPTIME_S, "zone": "",
            "confidence": "tentative", "signed": True, "module": "core.presence", "type": "presence_changed",
            "category": "event", "privacy": "p0", "state": "", "motion": 0, "breathing": 0, "bpm": 0,
            "duration_sec": 0, "bundled": 1, "replay": False,
            "v": 1, "alg": "ed25519", "fp": EX_FP, "sig": "…"}
    body.update(over)
    body["event_type"] = body["state"]
    return json.dumps(body, ensure_ascii=False, separators=(",", ":"))


def check_event(payload: str, label: str) -> None:
    e = json.loads(payload)
    em = EMITTERS.get((e.get("module"), e.get("type")))
    if not em:
        die(f"{label}: no WAP module emits {e.get('module')}/{e.get('type')}")
    if e["state"] not in em["states"] or e["event_type"] != e["state"]:
        die(f"{label}: {e['module']}/{e['type']} does not emit state {e['state']!r} (it emits {em['states']})")
    if (e["category"], e["privacy"]) != (em["category"], em["privacy"]):
        die(f"{label}: {e['module']}/{e['type']} is {em['category']}/{em['privacy']}, not "
            f"{e['category']}/{e['privacy']}")
    if not em["csi"] and (e["motion"] or e["breathing"] or e["bpm"]):
        die(f"{label}: {e['module']} carries no CSI scores")
    if e["event_id"] < ID_SPACE_BASE:
        die(f"{label}: event_id {e['event_id']} is below kIdSpaceBase ({ID_SPACE_BASE})")


TOPICS = [
    {"suffix": "status", "retained": True, "cadence": "on connect + ~30 s",
     "payload": '{"online":true,"device_type":"canary-wap","csi_running":true,"wifi_connected":true,"rssi":-58}'},
    {"suffix": "events", "retained": False, "cadence": "per committed CSI event",
     "payload": event_body(state="active", motion=78, breathing=8)},
    {"suffix": "chain", "retained": True, "cadence": "on each new record",
     "payload": '{"v":1,"length":312,"latest_hash":"a1b2…","algorithm":"ed25519","alg":"ed25519","fp":"' + EX_FP + '","sig":"…"}'},
    {"suffix": "health", "retained": True, "cadence": "~60 s",
     "payload": '{"battery":100,"battery_present":false,"memory_free":204800,"uptime":' + str(EX_UPTIME_S)
                + ',"firmware_version":"' + FW_VERSION + '","public_key":"…","event_id_space_low":false,'
                + '"sd_mounted":true}',
     "note": "event_id_space_low turns true once the event-id allocator nears the end of its space "
             "(sweep F82); Home Assistant shows it as the Event ID Space Low binary sensor"},
    {"suffix": "egress", "retained": True, "cadence": "~60 s, after health",
     "payload": '{"firmware_version":"' + FW_VERSION + '","uptime":86400,"csi_event_egress":{"dropped":0,'
                '"held_dropped":0,"ambient_dropped":0,"unsent_dropped":0,"planner":{"live":12,"held":0,'
                '"queued":3,"replayed":0,"skipped":0,"untrusted":0,"unsendable":0,"truncated_unsent":0,'
                '"read_giveups":0}}}',
     "note": "what the committed-event egress dropped and sent since boot (sweep F149), named by the "
             "firmware_version and uptime of the health publish it follows; its own topic because the health "
             "body has no room; Home Assistant shows it as the Health sensor's csi_event_egress while it "
             "pairs with the current health"},
    {"suffix": "counts", "retained": True, "cadence": "on each new record",
     "payload": '{"v":1,"total":312,"alg":"ed25519","fp":"' + EX_FP + '","sig":"…"}'},
    {"suffix": "tamper", "retained": False, "cadence": "per committed system.integrity event (live only, never backfill)",
     "payload": '{"type":"sd_remove","severity":"tamper"}',
     "note": "type is a const.py tamper kind; the HA per-type tamper sensors key on it"},
    {"suffix": "sensing", "retained": True, "cadence": "on acoustic event + 60 s heartbeat",
     "payload": '{"acoustic_event":"none","mic_muted":false,"t3_detected":0,"t4_detected":0,"knock_detected":0,"doorbell_detected":0,"glass_break_detected":0,"i2s_read_errors":0}'},
    {"suffix": "mesh", "retained": True, "cadence": "~30 s",
     "payload": '{"airtime_pct_x100":40,"channel":6,"locked_to_sta":true,"locked_to_ap":false,"fallback":false,"routine_allowed":18,"routine_denied":0,"urgent_sends":0}'},
    {"suffix": "chirp", "retained": True, "cadence": "~30 s",
     "payload": '{"state":"listening"}', "note": "state is the lowercase chirp enum"},
    {"suffix": "beacon", "retained": True, "cadence": "~30 s",
     "payload": '{"state":"Normal","beacon_airtime_pct_x100":0,"active_template":"","beacon_sends":0,"beacon_set_size":2,"trouble_mask":0}',
     "note": "state is NFPA-capitalized"},
    {"suffix": "update/state", "retained": True, "cadence": "on OTA state change",
     "payload": '{"installed_version":"' + FW_VERSION + '","latest_version":"' + FW_VERSION + '","in_progress":false,"update_percentage":null}'},
    {"suffix": "update/auto", "retained": True, "cadence": "on toggle", "payload": '"ON" | "OFF"'},
    {"suffix": "mic/state", "retained": True, "cadence": "on mute toggle", "payload": '"muted" | "live"'},
]
for t in TOPICS:
    if t["suffix"] in KEYS:
        keyed_as(t["payload"], KEYS[t["suffix"]], t["suffix"], OPTIONAL.get(t["suffix"], ()))
    elif t["suffix"] not in ("update/auto", "mic/state"):
        die(f"MQTT topic {t['suffix']!r} has no firmware key list to hold its example to")
check_event(next(t for t in TOPICS if t["suffix"] == "events")["payload"], "the events example")
_egress_example = json.loads(next(t for t in TOPICS if t["suffix"] == "egress")["payload"])["csi_event_egress"]
if list(_egress_example.keys()) != EGRESS_KEYS:
    die(f"MQTT egress: example csi_event_egress keys {list(_egress_example)} are not the firmware's "
        f"{EGRESS_KEYS}")
if list(_egress_example["planner"].keys()) != EGRESS_PLANNER_KEYS:
    die(f"MQTT egress: example planner keys {list(_egress_example['planner'])} are not the firmware's "
        f"{EGRESS_PLANNER_KEYS}")
must(FW / "chirp_channel.cpp", 'case CHIRP_LISTENING:    return "listening";', "chirp state word")
must(FW / "beacon_channel.cpp", 'case BEACON_STATE_NORMAL:      return "Normal";', "beacon state word")
SUBSCRIBED = [
    {"suffix": "update/cmd", "payload": '"install"'},
    {"suffix": "update/auto/cmd", "payload": '"ON" | "OFF"'},
    {"suffix": "mic/cmd", "payload": '"mute" | "unmute"'},
]
# validate every topic suffix appears in the publisher/subscriber source
for t in TOPICS + SUBSCRIBED:
    leaf = t["suffix"].split("/")[-1]
    if f'"{t["suffix"]}"' not in read(CSI_MQTT_CPP) and f'"{leaf}"' not in read(CSI_MQTT_CPP):
        die(f"MQTT topic suffix {t['suffix']!r} not found in csi_mqtt.cpp")

DISCOVERY_PREFIX = grab(CSI_MQTT_CPP, r'DISCOVERY_PREFIX\s*=\s*"([a-z]+)"', "HA DISCOVERY_PREFIX")

# The 24-entity discovery set (object_id validated against csi_mqtt.cpp's ENTITIES[]).
ENTITIES = [
    ("binary_sensor", "presence", "Presence", "events", "motion", None, None),
    ("binary_sensor", "online", "Online", "status", "connectivity", None, None),
    ("sensor", "state", "State", "events", None, None, None),
    ("sensor", "confidence", "Confidence", "events", None, None, None),
    ("sensor", "motion", "Motion Score", "events", None, "measurement", None),
    ("sensor", "breathing", "Breathing Score", "events", None, "measurement", None),
    ("sensor", "bpm", "Breathing Rate", "events", None, "measurement", "bpm"),
    ("sensor", "witness_count", "Witness Records", "counts", None, "total_increasing", None),
    ("sensor", "chain_length", "Chain Length", "chain", None, "total_increasing", "blocks"),
    ("sensor", "uptime", "Uptime", "health", "duration", "measurement", "s"),
    ("sensor", "memory_free", "Free Memory", "health", "data_size", "measurement", "B"),
    ("sensor", "rssi", "Signal Strength", "status", "signal_strength", "measurement", "dBm"),
    ("sensor", "mesh_airtime_pct", "Mesh Airtime", "mesh", None, "measurement", "%"),
    ("sensor", "mesh_channel", "Mesh Channel", "mesh", None, "measurement", None),
    ("binary_sensor", "mesh_channel_locked_to_sta", "Mesh Follows Home WiFi", "mesh", None, None, None),
    ("sensor", "chirp_state", "Chirp Channel State", "chirp", None, None, None),
    ("sensor", "beacon_state", "Beacon Channel State", "beacon", None, None, None),
    ("sensor", "beacon_airtime_pct", "Beacon Airtime", "beacon", None, "measurement", "%"),
    ("sensor", "beacon_active_template", "Beacon Active Alarm", "beacon", None, None, None),
    ("binary_sensor", "smoke_alarm", "Smoke Alarm Heard", "sensing", "smoke", None, None),
    ("binary_sensor", "co_alarm", "CO Alarm Heard", "sensing", "carbon_monoxide", None, None),
    ("binary_sensor", "knock", "Knock Detected", "sensing", "sound", None, None),
    ("binary_sensor", "doorbell", "Doorbell Detected", "sensing", "sound", None, None),
    ("binary_sensor", "glass_break", "Glass Break Detected", "sensing", "sound", None, None),
]
entities = []
for comp, obj, name, topic, devcla, statcla, unit in ENTITIES:
    if f'"{obj}"' not in read(CSI_MQTT_CPP):
        die(f"HA entity object_id {obj!r} not found in csi_mqtt.cpp")
    e = {"component": comp, "object_id": obj, "name": name, "state_topic": topic}
    if devcla:
        e["device_class"] = devcla
    if statcla:
        e["state_class"] = statcla
    if unit:
        e["unit"] = unit
    entities.append(e)

SWITCHES = [
    {"component": "update", "object_id": "firmware", "name": "Firmware",
     "device_class": "firmware", "state_topic": "update/state", "cmd_topic": "update/cmd"},
    {"component": "switch", "object_id": "auto_update", "name": "Auto Update",
     "state_topic": "update/auto", "cmd_topic": "update/auto/cmd"},
    {"component": "switch", "object_id": "mic_mute", "name": "Microphone Mute",
     "state_topic": "mic/state", "cmd_topic": "mic/cmd", "on": "mute", "off": "unmute"},
]
TRIGGERS = [
    {"id": "presence_active", "subtype": "active"},
    {"id": "presence_subtle", "subtype": "subtle"},
    {"id": "presence_quiet", "subtype": "quiet"},
    {"id": "presence_together", "subtype": "together"},
    {"id": "presence_empty", "subtype": "empty"},
    {"id": "anomaly", "subtype": "any"},
]
for sw in SWITCHES:
    # these appear inside the config-topic format strings, e.g. ".../mic_mute/config"
    must(CSI_MQTT_CPP, f'{sw["object_id"]}/config', f"HA switch {sw['object_id']}")
    # and each asks Home Assistant for <component>.<id>_<object_id> (sweep HA16)
    must(CSI_MQTT_CPP, f'"\\"def_ent_id\\":\\"{sw["component"]}.%s_{sw["object_id"]}\\","',
         f"HA switch {sw['object_id']}'s entity id")
# The sandbox's "ha" lines (binary_sensor.<id>_smoke_alarm, switch.<id>_mic_mute)
# are the ids the configs ask for: every table entity's config carries
# def_ent_id <component>.<device id, slugged>_<object_id> (sweep HA16;
# firmware/projects/canary-wap/tests_host/test_ha_discovery_ids.cpp formats them).
must(CSI_MQTT_CPP, '"\\"def_ent_id\\":\\"%s.%s_%s\\","\n', "HA entity ids from the discovery table")
must(CSI_MQTT_CPP, "e.component, slug, e.object_id,", "HA entity ids from the discovery table")

MQTT = {
    "prefix": MQTT_PREFIX,
    "topic_pattern": f"{MQTT_PREFIX}/<device_id>/<suffix>",
    "keepalive": 60,
    "broker_uri": "mqtt://<host>:1883  (mqtts://<host>:8883 with TLS)",
    "lwt": {"topic": f"{MQTT_PREFIX}/<id>/status", "payload": '{"online":false}',
            "note": "retained Last-Will; on connect the device replaces it with {\"online\":true,\"device_type\":\"canary-wap\"}"},
    "topics": TOPICS,
    "subscribed": SUBSCRIBED,
    "discovery": {
        "prefix": DISCOVERY_PREFIX,
        "config_topic": DISCOVERY_PREFIX + "/<component>/canary_<id>/<object_id>/config",
        "device": {"ids": ["canary_<id>"], "name": "Canary <id>", "mf": "SecuraCV", "mdl": "Canary WAP", "sw": FW_VERSION},
        "entities": entities,
        "switches": SWITCHES,
        "triggers": TRIGGERS,
        "counts": {"entities": f"{len(entities)}/{len(entities)}", "triggers": f"{len(TRIGGERS)}/{len(TRIGGERS)}"},
        "note": "the device announces its own entities the instant it connects — no YAML, no manual entities",
    },
}

# --------------------------------------------------------------------------- #
# 7. sensing vocabulary + dashboard cards (parsed / validated against the doc)
# --------------------------------------------------------------------------- #

doc = read(DOC)
PILLS = [
    {"name": "Quiet", "meaning": "The room looks empty or perfectly still."},
    {"name": "Presence", "meaning": "Steady micro-motion — the kind of signal a person sitting and breathing makes."},
    {"name": "Motion", "meaning": "Clear room-scale movement. Someone is walking or moving objects."},
    {"name": "Active", "meaning": "Sustained, vigorous activity."},
    {"name": "Offline", "meaning": "The radio hasn't started yet, or this build doesn't include CSI sensing."},
]
for p in PILLS:
    if f"**{p['name']}**" not in doc:
        die(f"sensing pill {p['name']!r} not found in {DOC.relative_to(REPO)}")

CARDS = [
    {"title": "Acoustic alarms", "desc": "Listens only for the T3 (smoke, NFPA 72) and T4 (CO, UL 2034) cadences every code-compliant alarm already emits. No audio stored, ever — the mic is reduced to one loudness number per 20 ms, then the buffer is wiped. Not a UL-listed life-safety device."},
    {"title": "Touch", "desc": "A hidden pad on D3/GPIO4: long-press = silent panic (no LED, no beep), a sustained drop = enclosure tamper, optional brief approach."},
    {"title": "Appliance activity", "desc": "Passively hears IR remote pulses (NEC/RC5/Sony) on D2/GPIO3 — a household-active baseline without ever recording what was watched."},
    {"title": "Thermal drift", "desc": "Watches the chip's own die temperature; a sudden ±5 °C step (case opened, device moved) flips to a drift alert."},
    {"title": "Mesh", "desc": "Two Canaries on either end of a hallway unlock cross-device direction sensing over the Opera mesh."},
]
for needle in ("T3", "T4", "silent panic", "Thermal drift", "Appliance activity"):
    if needle not in doc:
        die(f"dashboard card anchor {needle!r} not found in {DOC.relative_to(REPO)}")

SENSING = {
    "pills": PILLS,
    "gauges": ["Motion", "Breathing-band", "Signal"],
    "spectrum_bands": 8,
    "directions": ["approach", "recede", "left", "right"],
    "cards": CARDS,
    "privacy": "no camera stream, no microphone recording, no MAC addresses stored — the WAP feels the WiFi field itself and emits only small signed claims.",
}

# --------------------------------------------------------------------------- #
# 8. sandbox scenarios (the fun bit) — every effect traces to a real signal
# --------------------------------------------------------------------------- #

# A scene's publishes are the firmware's whole payloads (sweep A30): each one
# is the topic's example above with the scene's fields laid over it (`set`)
# and its counters moved on (`advance`), key order kept, the way the Vision
# pane's vizEventPayload lays the sandbox over its example (A26). `payload` is
# what the first click publishes; wap-ui.js's scenePayload lays the same
# fields over the topic's payload as it then stands, so a second click takes
# the next event id and the next chain length. Before this a scene published
# only the fields it changed: an events row with no envelope, and chain
# {"length":+1}, which is not JSON, replacing the retained chain row.
#
# A committed row and every acoustic or mute detection create a witness
# record (csi_event_commit_witness and create_witness_record), and the loop
# publishes counts then chain whenever records_created moves, so every scene
# advances both. The chain's head moves with it, and the page cannot know
# the new hash, so it is elided whole once it moves.
must(INO, "      csi_mqtt::publish_counts(g_health.records_created);\n"
          "      csi_mqtt::publish_chain(g_device.seq, g_device.chain_head);", "counts then chain, together")
must(INO, "create_witness_record(payload, cb.size(), RECORD_WITNESS_EVENT, &g_last_record);\n        }\n"
          "        const bool life_safety =", "an acoustic detection is a witness record")
must(INO, 'cb.write_text("event_type"); cb.write_text("mic_mute");', "a mute is a witness record")
must(FW / "securacv_audio.cpp", 'case AUDIO_EVENT_T3_SMOKE_ALARM: return "smoke_alarm_t3";', "acoustic_event word (T3)")
must(FW / "securacv_audio.cpp", 'case AUDIO_EVENT_T4_CO_ALARM:    return "co_alarm_t4";', "acoustic_event word (T4)")
TOPIC_PAYLOAD = {t["suffix"]: t["payload"] for t in TOPICS}
RECORD = [{"suffix": "counts", "advance": ["total"]},
          {"suffix": "chain", "set": {"latest_hash": "…"}, "advance": ["length"]}]


def presence_row(state, motion, breathing):
    return {"suffix": "events", "advance": ["event_id"],
            "set": {"event_type": state, "confidence": "tentative", "module": "core.presence",
                    "type": "presence_changed", "category": "event", "privacy": "p0", "state": state,
                    "motion": motion, "breathing": breathing, "bpm": 0}}


def lay_over(payload: str, set_: dict, advance: list, label: str) -> str:
    obj = json.loads(payload)
    for k in list(set_) + list(advance):
        if k not in obj:
            die(f"sandbox {label}: {k!r} is not a key of the topic's payload {list(obj)}")
    for k in advance:
        if not isinstance(obj[k], int) or isinstance(obj[k], bool):
            die(f"sandbox {label}: {k!r} is not a counter")
        obj[k] += 1
    obj.update(set_)
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


# A presence scene's state is the one core.presence's derive_target_state
# lands its scores in under the balanced preset at the neutral slider (the
# default: preset 1, sensitivity 50, so the offset is 0) with pet mode off.
# Scores at the together gate are refused, since the streak decides those.
# Its console line and event word are that state, so the three
# surfaces a click moves say the same word. The pill is the getting-started
# guide's vocabulary (Quiet / Presence / Motion / Active), which is not the
# firmware's; which one the page should speak is an open decision, so the
# blurb names both.
_BAL = re.search(r"default: base_motion = (\d+); base_active = (\d+); base_breathing = (\d+); break; // balanced",
                 read(CORE_PRESENCE_CPP))
if not _BAL:
    die("core.presence's balanced preset: not found in core_presence.cpp — firmware changed?")
BALANCED = tuple(int(x) for x in _BAL.groups())  # (motion, active, breathing)
must(CORE_PRESENCE_CPP, "  const int32_t preset = csi_module_settings_int(s, \"core.presence.preset\",      1);\n"
     "  const int32_t sens   = csi_module_settings_int(s, \"core.presence.sensitivity\", 50);", "balanced, neutral by default")
must(CORE_PRESENCE_CPP, "  const int together_gate = (s_active_threshold + 20 > 127) ? 127 : (s_active_threshold + 20);\n"
     "  if (motion >= together_gate && streak >= 5) return STATE_TOGETHER;\n"
     "  if (motion >= s_active_threshold)                     return STATE_ACTIVE;", "together, then active")
must(CORE_PRESENCE_CPP, "  if (breathing >= s_breathing_threshold) {\n"
     "    if (s_pet_mode && streak < s_pet_mode_required_win) return STATE_SUBTLE;\n    return STATE_QUIET;\n  }\n"
     "  if (motion >= s_motion_threshold) return STATE_SUBTLE;\n  return STATE_EMPTY;", "quiet, subtle, empty")


def derive_state(motion: int, breathing: int) -> str:
    m_thr, a_thr, b_thr = BALANCED
    if motion >= min(a_thr + 20, 127):
        return None  # together or active, by the streak: a scene must not sit on that line
    if motion >= a_thr:
        return "active"
    if breathing >= b_thr:
        return "quiet"
    if motion >= m_thr:
        return "subtle"
    return "empty"


def presence_scene(id_, label, blurb, pill, state, motion, breathing):
    if derive_state(motion, breathing) != state:
        die(f"sandbox {id_}: motion {motion}, breathing {breathing} is {derive_state(motion, breathing)!r} "
            f"under core.presence's balanced thresholds {BALANCED}, not {state!r}")
    return {"id": id_, "label": label, "blurb": blurb, "pill": pill, "event": state,
            "serial": "witness record: state=" + state,
            "mqtt": [presence_row(state, motion, breathing)] + RECORD}


_EX = json.loads(TOPIC_PAYLOAD["events"])
if derive_state(_EX["motion"], _EX["breathing"]) != _EX["state"]:
    die(f"the events example's motion {_EX['motion']}, breathing {_EX['breathing']} do not land in "
        f"{_EX['state']!r} under core.presence's balanced thresholds {BALANCED}")

SANDBOX = [
    presence_scene("wave", "Wave your arm",
                   "Room-scale movement lights the CSI motion gauge; the firmware calls it active (the guide's Motion pill).",
                   "Motion", "active", 78, 8),
    presence_scene("sit", "Sit still and breathe",
                   "A still, breathing person lights the breathing band; the firmware calls it quiet (the guide's Presence pill).",
                   "Presence", "quiet", 12, 41),
    presence_scene("leave", "Leave the room",
                   "The field goes still; the firmware reports empty (the guide's Quiet pill).",
                   "Quiet", "empty", 2, 3),
    {"id": "smoke", "label": "Fire a T3 smoke cadence",
     "blurb": "The mic matches the NFPA-72 smoke pattern; the Acoustic card turns red and a signed sensing event fires an HA notification.",
     "pill": "Motion", "event": "smoke_alarm_t3", "serial": "acoustic: matched smoke_alarm_t3",
     "mqtt": [{"suffix": "sensing", "set": {"acoustic_event": "smoke_alarm_t3"}, "advance": ["t3_detected"]}] + RECORD,
     "ha": "binary_sensor.<id>_smoke_alarm -> ON"},
    {"id": "co", "label": "Fire a T4 CO cadence",
     "blurb": "The mic matches the UL-2034 CO pattern.",
     "pill": "Motion", "event": "co_alarm_t4", "serial": "acoustic: matched co_alarm_t4",
     "mqtt": [{"suffix": "sensing", "set": {"acoustic_event": "co_alarm_t4"}, "advance": ["t4_detected"]}] + RECORD,
     "ha": "binary_sensor.<id>_co_alarm -> ON"},
    {"id": "mute", "label": "Mute the microphone",
     "blurb": "POST /api/audio/mute uninstalls the I2S driver and tri-states the mic GPIOs; the switch signs the change into the chain.",
     "event": "mic_mute", "serial": "audio: I2S driver uninstalled (muted)",
     # publish_mic_state writes the bare word, no quotes
     "mqtt": [{"suffix": "mic/state", "payload": "muted"}] + RECORD,
     "ha": "switch.<id>_mic_mute -> ON"},
]
# The canary-wap has no touch pad: the silent-panic scene this page used to
# stage ({"event_type":"silent_panic"}) is firmware/canary's (securacv_touch),
# and no WAP module emits it, so there is no payload of the WAP's to lay it
# over. It is gone rather than dressed in a WAP envelope.
for sc in SANDBOX:
    for pub in sc["mqtt"]:
        label = f"{sc['id']} {pub['suffix']}"
        if "set" in pub or "advance" in pub:
            pub.setdefault("set", {})
            pub.setdefault("advance", [])
            pub["payload"] = lay_over(TOPIC_PAYLOAD[pub["suffix"]], pub["set"], pub["advance"], label)
            keyed_as(pub["payload"], KEYS[pub["suffix"]], label, OPTIONAL.get(pub["suffix"], ()))
            if pub["suffix"] == "events":
                check_event(pub["payload"], f"sandbox {label}")
        elif pub["suffix"] != "mic/state" or pub["payload"] not in ("muted", "live"):
            die(f"sandbox {label}: a publish with no fields to lay over must be a bare word the firmware writes")
for word in ("smoke_alarm_t3", "co_alarm_t4"):
    must(CSI_MQTT_CPP, f"value_json.acoustic_event == '{word}'", f"HA reads {word}")

# What the MQTT pane says about itself (A30 review): its keys are the
# firmware's, but some values are elided and the sandbox's timing is
# compressed, so the note says which (the pane adds the non-retained topics
# from each topic's own flag). A non-ambient row is a bundle the device
# commits, with its witness record, when the bundle closes; the acoustic
# module bundles a row beside each alarm and mute, which no scene publishes.
BUNDLER_H = FW / "csi_bundler.h"
must(BUNDLER_H, "#define CSI_BUNDLER_WINDOW_MS  (10u * 60u * 1000u)", "the 10-minute bundle window")
must(BUNDLER_H, "#define CSI_BUNDLER_MAX_GAP_MS (2u * 60u * 1000u)", "the 2-minute quiet gap")
must(FW / "csi_bundler.cpp", "  /* Ambient bypasses the bundler entirely. */\n  if (values->category == CSI_CATEGORY_AMBIENT) {",
     "only ambient rows skip the bundler")
must(ACOUSTIC_CPP, '(void)csi_event_emit("acoustic.events",\n                       muted ? "mic_muted" : "mic_unmuted", &v);',
     "a mute is an acoustic.events row too")
if any("sig" not in json.loads(t["payload"]) for t in TOPICS if t["suffix"] in ("events", "chain", "counts")):
    die("the pane note says each signed row's sig is elided")
MQTT["pane_note"] = (
    "Every topic and every key, in its order, is the firmware's. What this page cannot know reads …: each sig, "
    "the health topic's public_key, and a chain head's hash once a scene moves it. The timing is compressed: "
    "the device commits a presence row, and the witness record that moves counts and chain, when the row's "
    "bundle closes (a 2-minute quiet gap or a 10-minute window), where the sandbox publishes them at the click "
    "and keeps the example's timestamp; and the smoke, CO and mute scenes leave out the acoustic.events row the "
    "device bundles beside them. The broker is staged; the contract is real.")

# --------------------------------------------------------------------------- #
# 8.5 flashing — the bench skills (parsed from the firmware README + build
#     files + docs, so the teaching can never go stale against the toolchain)
# --------------------------------------------------------------------------- #

FW_README = REPO / "firmware/projects/canary-wap/README.md"
PIO_INI = REPO / "firmware/projects/canary-wap/platformio.ini"
MAKEFILE = REPO / "firmware/projects/canary-wap/Makefile"
SETUP_SH = REPO / "firmware/projects/canary-wap/setup.sh"
BENCH_JS = REPO / "canary-local/emulator/web/bench.js"

readme = read(FW_README)


def between(text: str, start: str, end: str, label: str) -> str:
    i = text.find(start)
    if i < 0:
        die(f"{label}: anchor {start!r} not found in {FW_README.relative_to(REPO)}")
    j = text.find(end, i)
    return text[i:j] if j >= 0 else text[i:]


def links(text: str):
    return [{"name": n, "url": u} for n, u in re.findall(r"\[([^\]]+)\]\(([^)]+)\)", text)]


# ---- Option A: PlatformIO -------------------------------------------------
opt_a = between(readme, "Option A: PlatformIO", "</details>", "PlatformIO option")
pio_prereqs = links(between(opt_a, "**Prerequisites:**", "\n\n", "PlatformIO prereqs"))
fence = re.search(r"```bash\n(.*?)```", opt_a, re.S)
if not fence:
    die("PlatformIO option has no bash fence")
PIO_COMMANDS = []
note = None
for ln in fence.group(1).splitlines():
    ln = ln.rstrip()
    if not ln:
        continue
    if ln.lstrip().startswith("#"):
        note = ln.lstrip("# ").strip()
    else:
        PIO_COMMANDS.append({"cmd": ln.strip(), "note": note})
        note = None
# the commands must still exist as real build machinery
SRC_DIR = grab(PIO_INI, r"src_dir\s*=\s*(\S+)", "platformio src_dir")
if SRC_DIR != "arduino/canary_wap":
    die(f"platformio src_dir moved: {SRC_DIR!r}")
for target in ("setup:", "upload:", "monitor:"):
    must(MAKEFILE, target, f"Makefile target {target}")
if not SETUP_SH.exists():
    die("setup.sh missing next to the Makefile")
for c in PIO_COMMANDS:
    if c["cmd"].startswith("make "):
        must(MAKEFILE, c["cmd"].split()[1] + ":", f"README command {c['cmd']!r}")

# ---- Option B: Arduino IDE ------------------------------------------------
opt_b = between(readme, "Option B: Arduino IDE", "</details>", "Arduino option")
ard_prereqs = links(between(opt_b, "**Prerequisites:**", "\n\n", "Arduino prereqs"))
boards_url_m = re.search(r"(https://raw\.githubusercontent\.com/\S+)", opt_b)
if not boards_url_m:
    die("Arduino boards-manager URL not found in README Option B")
BOARDS_URL = boards_url_m.group(1)
must(FW_README, "esp32 by Espressif Systems", "Arduino board package name")

lib_block = between(opt_b, "**Install Libraries**", "3. **Configure Board**", "Arduino libraries")
ARD_LIBS = []
for ln in lib_block.splitlines():
    if re.match(r"\s+- ", ln):
        ARD_LIBS.append(re.sub(r"\s+", " ", ln.strip()[2:]))
    elif ARD_LIBS and re.match(r"\s{4,}\S", ln) and not re.match(r"\s+-", ln):
        ARD_LIBS[-1] += " " + ln.strip()
ARD_LIBS = [lib.replace("**", "") for lib in ARD_LIBS]
if len(ARD_LIBS) < 3:
    die(f"Arduino library list too short: {ARD_LIBS}")

BOARD_CONFIG = [[k.strip(), v.strip()] for k, v in
                re.findall(r"- ([A-Za-z ]+): \*\*([^*]+)\*\*", opt_b)]
for want in ("Board", "USB CDC On Boot", "Flash Size", "PSRAM"):
    if not any(k == want for k, _ in BOARD_CONFIG):
        die(f"Arduino board-config row {want!r} not found in README")
SKETCH = "arduino/canary_wap/canary_wap.ino"
must(FW_README, SKETCH, "sketch path")
if not (REPO / "firmware/projects/canary-wap" / SKETCH).exists():
    die(f"sketch path {SKETCH!r} does not exist on disk")

note_m = re.search(r"> \*\*One firmware, two toolchains\.\*\*(.*?)\n\n", readme, re.S)
if not note_m:
    die("'One firmware, two toolchains' note not found in README")
ONE_FIRMWARE = re.sub(r"\s+", " ", ("One firmware, two toolchains." + note_m.group(1))
                      .replace(">", " ").replace("**", "").replace("*", "")).strip()

# ---- Troubleshooting (the frustrating part, verbatim from the README) -----
ts = readme[readme.find("## Troubleshooting"):]
if "## Troubleshooting" not in readme:
    die("README has no Troubleshooting section")
TROUBLE = []
for symptom, body in re.findall(r"<summary><strong>(.+?)</strong></summary>(.*?)</details>", ts, re.S):
    fixes = [re.sub(r"\s+", " ", b.strip("- ").strip())
             for b in re.findall(r"^- .+", body, re.M)]
    for code in re.findall(r"```bash\n(.*?)```", body, re.S):
        fixes.extend(ln.strip() for ln in code.splitlines() if ln.strip() and not ln.strip().startswith("#"))
    TROUBLE.append({"symptom": symptom.strip(), "fixes": fixes})
if len(TROUBLE) < 3:
    die(f"only {len(TROUBLE)} troubleshooting entries parsed")
must(FW_README, "Hold BOOT button while connecting", "download-mode troubleshooting line")

# ---- the two little buttons (constants from the .ino; rituals validated) --
BOOT_GPIO = int(grab(INO, r"BOOT_BUTTON_GPIO\s*=\s*(\d+)", "BOOT_BUTTON_GPIO"))
BOOT_LONG_MS = int(grab(INO, r"BOOT_LONG_PRESS_MS\s*=\s*(\d+)", "BOOT_LONG_PRESS_MS"))
must(INO, "Press the BOOT button on the device to reveal the provisioning receipt.", "BOOT receipt hint")
must(DOC, "Press the **BOOT** button on the device for ~1 second", "docs password recovery")
must(DOC, "press and hold for **5 seconds** during power-up", "docs factory recovery")
# the ROM's own download-mode strings, exactly as the bench emulator prints them
must(BENCH_JS, "DOWNLOAD(USB/UART0)", "ROM download strap line")
must(BENCH_JS, "waiting for download", "ROM waiting line")

FLASH = {
    "kit_note": "Kits arrive already flashed — you flash when you build from parts, or to update over USB. "
                "Updates never touch the ROM's USB recovery, so a Canary can always be re-flashed.",
    "cable": "a data-capable USB-C cable (not charge-only) — the #1 'device not detected' cause",
    "port_hint": "the board shows up as a USB-CDC serial port · 115200 8N1",
    "buttons": [
        {"id": "boot", "label": "BOOT", "gpio": BOOT_GPIO,
         "what": f"GPIO{BOOT_GPIO}, a strapping pin — the ROM samples it only at reset. While the app runs, the firmware reads it for its own gestures:",
         "gestures": [
             "short press (~1 s): prints the provisioning receipt — AP SSID + password — on serial",
             f"hold >{BOOT_LONG_MS // 1000} s: factory reset (docs: hold 5 s during power-up if the device is unresponsive — new keypair, fresh credentials)",
             "held LOW through a reset: the mask ROM parks in download mode and waits for a flasher",
         ]},
        {"id": "reset", "label": "RESET",
         "what": "chip reset — RAM clears, NVS flash survives (your WiFi credentials, keys and witness chain ride through every reset)",
         "gestures": ["tap: reboot the app", "tap while BOOT is held: enter download mode"]},
    ],
    "download_mode": {
        "title": "Download mode by hand (when the upload can't find the port)",
        "steps": [
            "Hold BOOT down and keep it held",
            "Tap RESET once (or plug the USB cable in) while BOOT is still held",
            "Release BOOT — the serial port re-enumerates and the ROM prints 'waiting for download'",
            "Click Upload in your IDE now; when it finishes, tap RESET to boot the app",
        ],
        "rom_line": "rst:0x1 (POWERON),boot:0x0 (DOWNLOAD(USB/UART0))",
        "note": "if the upload still fails: try again (esptool retries often just work), close every serial monitor "
                "holding the port, or restart the IDE — then redo the ritual.",
    },
    "toolchains": [
        {"id": "platformio", "name": "PlatformIO (recommended)",
         "prereqs": pio_prereqs, "commands": PIO_COMMANDS, "src_dir": SRC_DIR},
        {"id": "arduino", "name": "Arduino IDE",
         "prereqs": ard_prereqs, "boards_url": BOARDS_URL,
         "board_pkg": "esp32 by Espressif Systems",
         "libraries": ARD_LIBS, "board_config": BOARD_CONFIG, "sketch": SKETCH},
    ],
    "one_firmware_note": ONE_FIRMWARE,
    "troubleshooting": TROUBLE,
    "recovery": {
        "password": [
            "Plug the Canary into a computer with a USB-C cable",
            "Open a serial terminal at 115 200 baud on the new USB-CDC port",
            "Press BOOT for ~1 second — the terminal prints the AP SSID and password",
        ],
        "factory": f"Hold BOOT >{BOOT_LONG_MS // 1000} s while running — or 5 s during power-up — for a factory reset: "
                   "fresh keypair, new credentials printed on serial",
    },
    "sources": {
        "build_paths": "firmware/projects/canary-wap/README.md §Choose Your Build Path + §Troubleshooting",
        "buttons": "canary_wap.ino BOOT_BUTTON_* constants",
        "recovery": "docs/getting_started_canary.md §Recovering the password",
    },
}

# --------------------------------------------------------------------------- #
# 9. assemble + write
# --------------------------------------------------------------------------- #

out = {
    "$note": "GENERATED by canary-local/tools/gen_wap.py from the canary-wap firmware + docs/registry. Do not edit by hand; run the generator. Drift-gated in .github/workflows/canary-local.yml.",
    "generated_by": "canary-local/tools/gen_wap.py",
    "device": {
        "id_example": EX_ID,
        "name": wap_reg.get("name", "Canary WAP"),
        "product_name": "SecuraCV Canary WAP",
        "device_type": DEVICE_TYPE,
        "tagline": wap_reg.get("tagline", ""),
        "model": BOARD_NAME_FULL or "Seeed XIAO ESP32-S3 Sense",
        "board_short": "XIAO ESP32S3",
        "board_id": BOARD_ID,
        "fw_version": FW_VERSION,
        "fw_train": FW_TRAIN,
        "ota_product": OTA_PRODUCT,
        "senses": wap_reg.get("senses", []),
        "modality": wap_reg.get("modality", "wifi-csi"),
        "network": wap_reg.get("network", {}),
        "status": wap_reg.get("status", ""),
        "docs": wap_reg.get("docs", []),
    },
    "ap": AP,
    "captive": CAPTIVE,
    "routes": ROUTES,
    "wizard": WIZARD,
    "serial": SERIAL,
    "mqtt": MQTT,
    "sensing": SENSING,
    "sandbox": SANDBOX,
    "flash": FLASH,
    "docs": {
        "getting_started": "docs/getting_started_canary.md",
        "firmware_readme": "firmware/projects/canary-wap/README.md",
        "registry": "canary-local/devices/registry.json",
    },
}

# sanity floors — a broken parse must fail the build, not ship thin data
if len(entities) < 20:
    die(f"only {len(entities)} HA entities — parse likely broke")
if len(WIZARD["steps"]) != 5:
    die("expected exactly 5 wizard steps")
if len(BOOT) < 15:
    die("boot log too short — parse likely broke")
if len(FLASH["toolchains"]) != 2:
    die("expected exactly 2 flashing toolchains")
if len(FLASH["troubleshooting"]) < 3 or len(PIO_COMMANDS) < 3:
    die("flash section parsed thin — README moved?")

OUT_JSON.write_text(json.dumps(out, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
print(f"wrote {OUT_JSON.relative_to(REPO)}  "
      f"({len(entities)} entities, {len(TOPICS)} topics, {len(BOOT)} boot lines, "
      f"{len(SANDBOX)} sandbox scenarios, fw {FW_VERSION})")
