#!/usr/bin/env python3
"""canary-local/tools/gen_homeassistant.py — The Hub's data, as data.

Emits canary-local/devices/homeassistant.json for the Home Assistant +
Raspberry Pi guide page (canary-local/homeassistant.html). The page never
hardcodes a fact this generator can derive — same anti-rot contract as
gen_enclosures.py: maintainers edit the sources they already edit, CI
regenerates and diffs, staleness cannot land silently.

Sources of truth:

  1. custom_components/securacv/manifest.json — integration version/domain
  2. hacs.json                                — minimum Home Assistant version
  3. canary-local/devices/registry.json       — the firmware train this
                                                snapshot teaches
  4. docs/homeassistant_setup.md              — the entity catalog (§Step 4)
     and the MQTT topic contract (§MQTT Topic Reference), parsed from the
     doc's own tables/bullets so the demo can only show entities the doc
     actually promises
  5. version.home-assistant.io/stable.json    — upstream HA OS + Core
     versions (ONLY with --refresh-upstream; the normal run is
     deterministic/offline and preserves the committed snapshot verbatim)

Authored-in-generator content (the WHY cards, the assembly choreography,
the bench-terminal scripts) lives HERE as constants — the repo's pattern
for curated copy that still travels through the drift gate (see
PRINT_SETTINGS in gen_enclosures.py). The terminal scripts are templated
on {{vars}} so a version bump never touches a script by hand.

Failure posture (self-healing): --refresh-upstream that cannot reach or
parse the feed keeps the previous committed snapshot untouched and exits
0 — the scheduled workflow then simply finds no diff. Values only ever
move forward on a successful, shape-checked fetch.

Run:  python3 canary-local/tools/gen_homeassistant.py [--refresh-upstream]
CI:   regenerates and diffs (drift gate); a weekly scheduled workflow
      runs --refresh-upstream and opens a PR when upstream moved.
"""
import json
import os
import re
import sys
import urllib.request
from datetime import date
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT_JSON = REPO / "canary-local/devices/homeassistant.json"
DOC = REPO / "docs/homeassistant_setup.md"
MANIFEST = REPO / "custom_components/securacv/manifest.json"
HACS = REPO / "hacs.json"
REGISTRY = REPO / "canary-local/devices/registry.json"

UPSTREAM_FEED = "https://version.home-assistant.io/stable.json"

# Seed used only when no committed snapshot exists yet (first generation).
# The scheduled freshness workflow replaces this with live values; the page
# renders fetched_at and says out loud how old the snapshot is.
SEED_UPSTREAM = {
    "haos_version": "14.1",
    "ha_version": "2025.12",
    "source": UPSTREAM_FEED,
    "fetched_at": "2026-01-01",
}


# ── §1 WHY cards (authored; voice-checked against docs/why_secure.md) ────
WHY = [
    {
        "title": "Every witness, one wall",
        "body": "Each Canary keeps its own signed chain — deliberately. The "
                "hub is where they converge: one dashboard, one timeline, "
                "one place the household actually looks.",
    },
    {
        "title": "Local, like everything else here",
        "body": "A Raspberry Pi on your shelf. No cloud account, no "
                "subscription — witness data never has to leave the house.",
    },
    {
        "title": "It does things",
        "body": "A smoke-alarm cadence heard → a push that bypasses every "
                "silent phone. A tamper → the lights come on. A chain "
                "failure → you know in seconds.",
    },
    {
        "title": "Verified ✓ means verified",
        "body": "The integration pins each device's Ed25519 key and "
                "verifies every publish. The timeline's checkmark is a "
                "signature check, not a decoration.",
    },
]

# ── §2 hardware (assembly choreography for the 3D stage) ─────────────────
# Same contract as devices/assembly.json: parts and step text are honest
# (dimensions are the published Raspberry Pi 4B mechanical drawing, 85×56
# board, holes at 3.5/61.5 × 3.5/52.5), the choreography — seated poses,
# explode/insert vectors, camera — is authored and staged. All parts are
# procedural (builders in assets/hub-parts.js); positions representative.
HARDWARE = {
    "title": "One evening on the kitchen table",
    "intro": "A Raspberry Pi 4 or 5, a good microSD card, a vented case, "
             "wired ethernet if you can. Scrub it apart, then walk the build.",
    "needs": [
        {"item": "Raspberry Pi 4 (4 GB+) or Pi 5", "note": "Pi 5 works great — ~3 cameras at 10 fps if you later add Frigate", "from_doc": True},
        {"item": "microSD card, 32 GB+ (A2, endurance-rated)", "note": "this card IS the computer's disk", "from_doc": False},
        {"item": "Official USB-C power supply", "note": "15 W (Pi 4) / 27 W (Pi 5) — undervoltage is the classic mystery crash", "from_doc": False},
        {"item": "Case with airflow (+ heatsink)", "note": "any vented case; passive cooling is fine", "from_doc": False},
        {"item": "Ethernet cable (recommended)", "note": "a hub wants wire", "from_doc": False},
    ],
    "frame": {"rx": -0.5, "ry": 0.7, "pad": 2.3},
    "parts": [
        {"id": "case_base", "source": "proc", "part": "caseBase", "seated": {"pos": [0, 0, 0]},
         "explode": [0, 0, -34], "step": 0, "name": "Case base"},
        {"id": "pi", "source": "proc", "part": "piBoard", "seated": {"pos": [0, 0, 4.4]},
         "explode": [0, 0, 30], "insert": [0, 0, 44], "step": 1, "name": "Raspberry Pi 4", "ref": "U1"},
        {"id": "heatsink", "source": "proc", "part": "heatsink", "seated": {"pos": [-6.5, 4.5, 8.2]},
         "explode": [0, 0, 58], "insert": [0, 0, 30], "step": 2, "name": "Heatsink", "qty": 1},
        {"id": "sd", "source": "proc", "part": "microSd", "seated": {"pos": [-45, 0, 3.0]},
         "explode": [-32, 0, 6], "insert": [-26, 0, 0], "step": 3, "name": "microSD (flashed)", "ref": "SD1"},
        {"id": "lid", "source": "proc", "part": "caseLid", "seated": {"pos": [0, 0, 19.5]},
         "explode": [0, 0, 80], "insert": [0, 0, 44], "step": 4, "name": "Case lid"},
        {"id": "eth", "source": "proc", "part": "ethPlug", "seated": {"pos": [51, -18, 8.5]},
         "explode": [42, 0, 0], "insert": [34, 0, 0], "step": 5, "name": "Ethernet"},
        {"id": "psu", "source": "proc", "part": "psuPlug", "seated": {"pos": [-30, -34, 6.2]},
         "explode": [0, -34, 0], "insert": [0, -30, 0], "step": 5, "name": "USB-C power"},
    ],
    "steps": [
        {"title": "Start with the case",
         "note": "Any vented case will do. A hub runs 24/7 — airflow is the whole spec."},
        {"title": "Seat the board",
         "note": "Onto the posts, ports facing the openings. It should sit flat, no rock."},
        {"title": "Heatsink on the SoC",
         "note": "Peel, align to the big square chip, press. Passive is plenty."},
        {"title": "The card is the computer",
         "note": "Into the underside slot, label facing the board. Flash it first — "
                 "Chapter 1 in the bench terminal below."},
        {"title": "Close it up",
         "note": "Lid on until it clicks. Leave the SD slot reachable if you can."},
        {"title": "Wire it — ethernet first, power last",
         "note": "Ethernet to your router, then USB-C. Power IS the switch; first boot "
                 "takes up to 20 minutes."},
    ],
}

# ── §3 the bench terminal (scripts; {{vars}} resolved at render time) ────
# Every command is the real one. Output lines are recorded/representative
# transcripts — the terminal says so on its face. Hosts: "laptop" is your
# machine; "ha-ssh" is Home Assistant's Terminal/SSH add-on prompt.
TERMINAL = {
    "note": "Simulated bench: real commands, representative output, "
            "versions live from this page's drift-gated snapshot.",
    "chapters": [
        {
            "id": "flash",
            "title": "1 · Flash the card",
            "host": "laptop",
            "intro": "On your computer, card in a reader — or let Raspberry "
                     "Pi Imager (Other specific-purpose OS → Home assistants) "
                     "do all of this for you, verify included.",
            "steps": [
                {"cmd": "lsblk -d -o NAME,SIZE,MODEL",
                 "out": ["NAME  SIZE   MODEL",
                         "sda   931.5G Samsung SSD 870",
                         "sdb    59.5G SD Card Reader"],
                 "note": "Find the card — 59.5G in a reader. The only dangerous step on this page is getting this wrong."},
                {"cmd": "wget -q --show-progress https://github.com/home-assistant/operating-system/releases/download/{{haos}}/haos_rpi4-64-{{haos}}.img.xz",
                 "out": ["haos_rpi4-64-{{haos}}.img.xz   100%[==================>] 380.1M  21.4MB/s  in 18s"],
                 "note": "Pi 5: same release, image haos_rpi5-64-{{haos}}.img.xz. The version is live from this page's snapshot."},
                {"cmd": "sha256sum haos_rpi4-64-{{haos}}.img.xz",
                 "out": ["9f2c1a7e30b8…  haos_rpi4-64-{{haos}}.img.xz"],
                 "note": "Yours will differ — compare it against the SHA-256 on the release page. Never skip this."},
                {"cmd": "xz -d haos_rpi4-64-{{haos}}.img.xz",
                 "out": [],
                 "note": "Unpacks to a raw disk image."},
                {"cmd": "sudo dd if=haos_rpi4-64-{{haos}}.img of=/dev/sdb bs=4M conv=fsync status=progress",
                 "out": ["2101346304 bytes (2.1 GB, 2.0 GiB) copied, 128 s, 16.4 MB/s",
                         "512+1 records in",
                         "512+1 records out",
                         "2147483648 bytes (2.1 GB, 2.0 GiB) copied, 131.207 s, 16.4 MB/s"],
                 "note": "dd erases /dev/sdb completely — triple-check. Then eject, and back to the build: Step 4."},
            ],
        },
        {
            "id": "boot",
            "title": "2 · First boot",
            "host": "laptop",
            "intro": "Card in, ethernet in, power last. First boot installs — "
                     "up to 20 minutes. Watch for it:",
            "steps": [
                {"cmd": "ping -c 3 homeassistant.local",
                 "out": ["PING homeassistant.local (192.168.1.87): 56 data bytes",
                         "64 bytes from 192.168.1.87: icmp_seq=0 ttl=64 time=1.42 ms",
                         "64 bytes from 192.168.1.87: icmp_seq=1 ttl=64 time=1.11 ms",
                         "64 bytes from 192.168.1.87: icmp_seq=2 ttl=64 time=1.09 ms",
                         "--- homeassistant.local ping statistics ---",
                         "3 packets transmitted, 3 packets received, 0.0% packet loss"],
                 "note": "mDNS, same trick every Canary uses. Silent? Wait — first boot is genuinely slow, once."},
                {"cmd": "curl -sI http://homeassistant.local:8123 | head -n 1",
                 "out": ["HTTP/1.1 200 OK"],
                 "note": "It's up. Open http://homeassistant.local:8123, create the owner account (it lives only on the Pi) — that's onboarding."},
            ],
        },
        {
            "id": "broker",
            "title": "3 · The broker",
            "host": "ha-ssh",
            "intro": "Canaries speak MQTT. This prompt is the Terminal & SSH "
                     "app — or click the same installs in the App Store.",
            "steps": [
                {"cmd": "ha core info",
                 "out": ["arch: aarch64",
                         "audio_input: None",
                         "machine: rpi4-64",
                         "update_available: false",
                         "version: {{ha}}",
                         "version_latest: {{ha}}"],
                 "note": "That version is live from this page's snapshot."},
                {"cmd": "ha addons install core_mosquitto",
                 "out": ["Processing... Done.",
                         "",
                         "Add-on \"core_mosquitto\" successfully installed"],
                 "note": "Mosquitto — the setup guide's recommended broker."},
                {"cmd": "ha addons start core_mosquitto",
                 "out": ["Processing... Done."],
                 "note": "HA now offers the discovered MQTT integration under Settings → Devices & Services — accept the defaults."},
            ],
        },
        {
            "id": "integration",
            "title": "4 · The integration",
            "host": "ha-ssh",
            "intro": "SecuraCV ships through HACS; HACS installs with its "
                     "official one-liner. The rest is clicks:",
            "steps": [
                {"cmd": "wget -O - https://get.hacs.xyz | bash -",
                 "out": ["INFO: Downloading HACS",
                         "INFO: Unpacking HACS",
                         "INFO: Installation complete.",
                         "INFO: Remember to restart Home Assistant before you configure it"],
                 "note": "The official HACS installer, verbatim."},
                {"cmd": "ha core restart",
                 "out": ["Processing... Done."],
                 "note": "Then: HACS → ⋮ → Custom repositories → add https://github.com/kmay89/securaCV (Integration) → install SecuraCV v{{integration}} → restart → Add Integration → SecuraCV → \"Canary devices via MQTT (Recommended)\"."},
            ],
        },
        {
            "id": "fleet",
            "title": "5 · Meet the fleet",
            "host": "ha-ssh",
            "intro": "Point a Canary at the broker (its dashboard → Network "
                     "tab → homeassistant.local:1883) and it announces itself "
                     "within ~30 seconds. Watch the wire:",
            "steps": [
                {"cmd": "mosquitto_sub -h localhost -t 'securacv/#' -v -C 4",
                 "out": ["securacv/canary_wap_garage/availability online",
                         "securacv/canary_wap_garage/status {\"chain_seq\":1284,\"gps_fix\":true,\"uptime_s\":93412}",
                         "securacv/canary_wap_garage/health {\"free_heap\":168224,\"sd_mounted\":true,\"die_temp_c\":41}",
                         "securacv/canary_wap_garage/chain {\"length\":1284,\"latest_hash\":\"9f2c…\",\"sig\":\"ed25519:…\"}"],
                 "note": "The real topic contract. That signed chain line is what the integration verifies against the pinned key. Now scroll down."},
            ],
        },
    ],
}

# ── §4 the HA demo (curated subset of the doc-parsed entity catalog) ─────
# name must match a name parsed from docs/homeassistant_setup.md §Step 4
# (or the OTA section for the update entities) — generation FAILS otherwise,
# so the demo can never show an entity the doc stopped promising.
DEMO_ENTITIES = [
    {"name": "Witness Count", "kind": "sensor", "initial": "1,284", "unit": "records", "icon": "▦"},
    {"name": "Chain Valid", "kind": "binary_sensor", "initial": "on", "icon": "✓",
     "attributes": {"verified": "true", "trust_reason": "ok"}},
    {"name": "Online", "kind": "binary_sensor", "initial": "on", "icon": "●"},
    {"name": "Uptime", "kind": "sensor", "initial": "1d 1h 56m", "icon": "⏱"},
    {"name": "Die Temperature", "kind": "sensor", "initial": "41", "unit": "°C", "icon": "🌡"},
    {"name": "SD Card Healthy", "kind": "binary_sensor", "initial": "on", "icon": "▤"},
    {"name": "Tamper Detected", "kind": "binary_sensor", "initial": "off", "icon": "⚠"},
    {"name": "Smoke Alarm Heard", "kind": "binary_sensor", "initial": "off", "icon": "🔥"},
    {"name": "CO Alarm Heard", "kind": "binary_sensor", "initial": "off", "icon": "☁"},
    {"name": "Microphone Mute", "kind": "switch", "initial": "off", "icon": "🎙",
     "note": "every toggle is signed into the witness chain with its source"},
    {"name": "Firmware", "kind": "update", "initial": "up to date", "icon": "⬆",
     "from_section": "ota"},
]

HA_DEMO = {
    "device_name": "SecuraCV Canary canary_wap_garage",
    "device_id": "canary_wap_garage",
    "note": "A faithful sketch of Home Assistant, not its real frontend — "
            "but the entity names, topics, and behaviors are the drift-gated "
            "real ones from the setup guide.",
    "drill": {
        "label": "Play the smoke-alarm drill",
        "trigger_entity": "Smoke Alarm Heard",
        "automation": "SecuraCV Alerts (blueprint)",
        "notification": {
            "title": "🔥 Smoke alarm heard — Garage",
            "body": "Canary canary_wap_garage matched an NFPA 72 T3 smoke "
                    "cadence. Critical alert: bypasses silent mode.",
        },
        "clear_after_s": 8,
        "time_note": "time compressed — on a real device the sensor clears "
                     "about 30 seconds after the alarm stops",
    },
}


# ── parsing helpers ──────────────────────────────────────────────────────
def section(text, start_pat, end_pat):
    m = re.search(start_pat, text)
    if not m:
        sys.exit(f"gen_homeassistant: doc section not found: {start_pat}")
    rest = text[m.end():]
    e = re.search(end_pat, rest)
    return rest[: e.start()] if e else rest


def parse_entities(doc_text):
    """§Step 4 bullet catalog → [{name, desc}] (bullets may wrap lines and
    may carry two bold names split by a slash)."""
    body = section(doc_text, r"### Step 4: Verify Discovery", r"\n### ")
    bullets, cur = [], None
    for line in body.splitlines():
        if re.match(r"^\s*-\s+\*\*", line):
            if cur:
                bullets.append(cur)
            cur = line.strip()
        elif cur and line.strip() and not line.strip().startswith("#"):
            cur += " " + line.strip()
        elif cur and not line.strip():
            bullets.append(cur)
            cur = None
    if cur:
        bullets.append(cur)

    out = []
    for b in bullets:
        head, _, desc = b.partition("—")
        names = re.findall(r"\*\*(.+?)\*\*", head)
        for n in names:
            out.append({"name": n.strip(), "desc": desc.strip().split(". ")[0].strip()})
    if len(out) < 10:
        sys.exit(f"gen_homeassistant: entity parse looks broken ({len(out)} found)")
    return out


def parse_topics(doc_text):
    """§MQTT Topic Reference table → [{topic, direction, content}]."""
    body = section(doc_text, r"### MQTT Topic Reference", r"\n### |\n## ")
    rows = []
    for line in body.splitlines():
        m = re.match(r"^\|\s*`(.+?)`\s*\|\s*(.+?)\s*\|\s*(.+?)\s*\|$", line)
        if m:
            rows.append({"topic": m.group(1), "direction": m.group(2), "content": m.group(3)})
    if len(rows) < 6:
        sys.exit(f"gen_homeassistant: topic table parse looks broken ({len(rows)} rows)")
    return rows


# Some CDNs (version.home-assistant.io included) 403 the default
# Python-urllib user agent — identify honestly instead.
USER_AGENT = "securaCV-freshness/1.0 (+https://github.com/kmay89/securaCV)"


def _get_json(url, headers=None):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **(headers or {})})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)


def _warn(msg):
    print(f"gen_homeassistant: {msg}", file=sys.stderr)
    if os.environ.get("GITHUB_ACTIONS"):
        print(f"::warning::gen_homeassistant: {msg}")


def _snapshot(haos, ha, source):
    if not re.match(r"^\d", str(ha)) or not re.match(r"^\d", str(haos)):
        raise ValueError(f"unexpected shapes: ha={ha!r} haos={haos!r}")
    return {
        "haos_version": str(haos),
        "ha_version": str(ha),
        "source": source,
        "fetched_at": date.today().isoformat(),
    }


def refresh_upstream(prev):
    """Fetch the live HA OS + Core versions: the version feed first, the
    GitHub releases API as fallback (authenticated with GH_TOKEN/GITHUB_TOKEN
    when present, as in the freshness workflow). Any failure of both —
    network, HTTP, shape — returns the previous snapshot untouched
    (self-healing: values only move forward on a verified read)."""
    try:
        feed = _get_json(UPSTREAM_FEED)
        ha = feed["homeassistant"]["default"]
        # hassos key maps board → version; rpi boards share one OS version
        hassos = feed.get("hassos") or {}
        haos = hassos.get("rpi4-64") or hassos.get("ota") or next(iter(hassos.values()))
        return _snapshot(haos, ha, UPSTREAM_FEED)
    except Exception as e:  # noqa: BLE001 — fall through to the releases API
        _warn(f"version feed failed ({e}); trying the GitHub releases API")
    try:
        tok = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
        auth = {"Authorization": f"Bearer {tok}"} if tok else {}
        gh = "https://api.github.com/repos/home-assistant/{}/releases/latest"
        haos = _get_json(gh.format("operating-system"), auth)["tag_name"].lstrip("v")
        ha = _get_json(gh.format("core"), auth)["tag_name"].lstrip("v")
        return _snapshot(haos, ha, "https://api.github.com/repos/home-assistant (releases)")
    except Exception as e:  # noqa: BLE001 — the whole point is to survive
        _warn(f"upstream refresh failed on both sources ({e}); keeping "
              f"previous snapshot from {prev.get('fetched_at')}")
        return prev


def main():
    refresh = "--refresh-upstream" in sys.argv[1:]
    doc_text = DOC.read_text(encoding="utf-8")
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    hacs = json.loads(HACS.read_text(encoding="utf-8"))
    registry = json.loads(REGISTRY.read_text(encoding="utf-8"))

    prev_upstream = SEED_UPSTREAM
    if OUT_JSON.exists():
        try:
            prev_upstream = json.loads(OUT_JSON.read_text(encoding="utf-8"))["upstream"]
        except Exception:
            pass
    upstream = refresh_upstream(prev_upstream) if refresh else prev_upstream

    entities = parse_entities(doc_text)
    entity_names = {e["name"] for e in entities}
    desc_of = {e["name"]: e["desc"] for e in entities}

    # validate the curated demo against the doc's promises
    for d in DEMO_ENTITIES:
        if d.get("from_section") == "ota":
            if f"**{d['name']}**" not in doc_text:
                sys.exit(f"gen_homeassistant: demo entity '{d['name']}' no longer in the doc")
        elif d["name"] not in entity_names:
            sys.exit(f"gen_homeassistant: demo entity '{d['name']}' not in §Step 4 "
                     f"of docs/homeassistant_setup.md — demo and doc drifted")
        d.setdefault("desc", desc_of.get(d["name"], ""))

    out = {
        "$note": "GENERATED by canary-local/tools/gen_homeassistant.py — do not edit by hand. "
                 "Sources: custom_components/securacv/manifest.json, hacs.json, "
                 "devices/registry.json, docs/homeassistant_setup.md; upstream versions "
                 "refreshed by the scheduled homeassistant-freshness workflow.",
        "integration": {
            "domain": manifest["domain"],
            "name": manifest["name"],
            "version": manifest["version"],
            "iot_class": manifest["iot_class"],
            "min_ha": hacs["homeassistant"],
            "source": "custom_components/securacv/manifest.json + hacs.json",
        },
        "fw_train": registry["fw_train"],
        "upstream": upstream,
        "why": WHY,
        "hardware": HARDWARE,
        "terminal": TERMINAL,
        "ha_demo": {**HA_DEMO, "entities": DEMO_ENTITIES},
        "entity_catalog": entities,
        "topics": parse_topics(doc_text),
        "docs": {
            "setup": "docs/homeassistant_setup.md",
            "blueprints": "docs/blueprints/securacv_alerts.yaml",
            "timeline_card": "docs/lovelace_timeline.md",
            "device_trust": "docs/device_trust.md",
            "firmware_ota": "docs/firmware_ota.md",
            "frigate": "docs/frigate_integration.md",
        },
    }

    OUT_JSON.write_text(json.dumps(out, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(f"wrote {OUT_JSON.relative_to(REPO)} "
          f"({len(entities)} doc entities, {len(out['topics'])} topics, "
          f"upstream {upstream['haos_version']}/{upstream['ha_version']} "
          f"as of {upstream['fetched_at']})")


if __name__ == "__main__":
    main()
