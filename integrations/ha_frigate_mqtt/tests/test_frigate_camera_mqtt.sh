#!/usr/bin/env bash
#
# Pins the stack's Frigate config: every camera carries `mqtt: enabled: false`.
# Frigate's camera-level `mqtt.enabled` is on by default and, whatever
# `snapshots.enabled` says, publishes a cropped JPEG of every tracked object to
# frigate/<camera>/<label>/snapshot as a retained message, which the broker
# keeps. The top-level `mqtt:` is only the broker connection; frigate/events,
# which the bridge reads, is published either way. There is no global switch
# for the camera key, so a camera added without it fails here.
#
#   bash integrations/ha_frigate_mqtt/tests/test_frigate_camera_mqtt.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG="$HERE/../frigate.yml"

python3 -I - "$CONFIG" <<'PY'
import sys

path = sys.argv[1]
text = open(path, encoding="utf-8").read()


def fail(msg):
    sys.exit(f"FAIL: {path}: {msg}")


def cameras_structural(text):
    """{camera: mqtt images off?} read by indentation (no PyYAML needed)."""
    lines = []
    for raw in text.splitlines():
        if raw.lstrip().startswith("#") or not raw.strip():
            continue
        body = raw.split(" #", 1)[0].rstrip()
        lines.append((len(raw) - len(raw.lstrip(" ")), body.strip()))
    cams, inside, cam_indent, cur = {}, False, None, None
    key_indent, in_mqtt = None, False
    for indent, line in lines:
        if indent == 0:
            inside, cur = line == "cameras:", None
            continue
        if not inside:
            continue
        if cam_indent is None:
            cam_indent = indent
        if indent == cam_indent:
            cur, key_indent, in_mqtt = line.rstrip(":").strip("'\""), None, False
            cams[cur] = False
            continue
        if cur is None:
            continue
        if key_indent is None:
            key_indent = indent
        if indent == key_indent:
            compact = line.replace(" ", "")
            in_mqtt = compact == "mqtt:"
            if compact == "mqtt:{enabled:false}":
                cams[cur] = True
        elif in_mqtt and indent > key_indent and line.replace(" ", "") == "enabled:false":
            cams[cur] = True
    return cams


cams = cameras_structural(text)
if not cams:
    fail("no cameras found under `cameras:`")
left_on = sorted(name for name, off in cams.items() if not off)
if left_on:
    fail("camera(s) without `mqtt: enabled: false`: " + ", ".join(left_on)
         + " (Frigate would publish a retained JPEG of every tracked object to the broker)")

try:
    import yaml
except ImportError:
    yaml = None
if yaml is not None:
    parsed = yaml.safe_load(text)
    if parsed.get("mqtt", {}).get("enabled", True) is not True:
        fail("the top-level mqtt block (the broker connection) must stay on")
    for name, conf in parsed["cameras"].items():
        if (conf or {}).get("mqtt") != {"enabled": False}:
            fail(f"camera {name!r}: parsed mqtt is {(conf or {}).get('mqtt')!r}, not {{'enabled': False}}")
print(f"ok: {len(cams)} camera(s), per-camera MQTT images off")
PY
