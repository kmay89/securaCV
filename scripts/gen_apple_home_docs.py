#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_apple_home_docs.py — make the Apple Home docs a *computed* artifact, so
they cannot drift from the vocabulary they describe.

The Apple Home quickstart tells a user exactly what their Canaries will
publish into their home: which signals, which are on by default, which carry
the coarse object class, and how fast the metronome ticks. Every one of those
facts already exists somewhere authoritative — `spec/witness_dictionary.json`
for the vocabulary and the pacing, `Cargo.toml` for the feature name,
`src/bridge/hap/accessory.rs` for whether a signal becomes a tile in the Home
app or a status badge on one.

One trap this file exists to avoid: the dictionary's `label` is *our* name for
a signal, mirrored into the Apple apps; the name on the tile is the bridge's
own `service_name()`, carried in the dictionary as `home_app_name` and pinned
to that function by `scripts/lint_dictionary_sync.py`. This table used to say
"Motion (person)" for four tiles the bridge publishes as "Person" / "Vehicle" /
"Animal" / "Package" — a section headed "What you'll see" naming things the
user cannot see. Render `home_app_name` for tiles, `label` for the signal.

Restating them in prose is how docs rot. Worse, *this* prose is a privacy
promise: a doc that still says "person/vehicle/animal/package are off by
default" after someone flipped that default is not merely stale, it is a lie
to the person deciding whether to publish their home into someone else's
ecosystem.

So the tables are generated between markers and CI byte-diffs the result.
Edit the dictionary, re-run this, commit both.

Run:    python3 scripts/gen_apple_home_docs.py
Check:  python3 scripts/gen_apple_home_docs.py --check
"""

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DICTIONARY = REPO / "spec/witness_dictionary.json"
ACCESSORY = REPO / "src/bridge/hap/accessory.rs"
CARGO = REPO / "Cargo.toml"

QUICKSTART = REPO / "docs/integrations/apple-home-quickstart.md"

# The cargo feature that gates the whole lane. Read from Cargo.toml rather
# than typed here, so renaming the feature updates every build command in the
# docs instead of leaving a copy-paste that no longer compiles.
FEATURE_RE = re.compile(r"^(bridge-homekit-server)\s*=", re.M)

ERRORS: list[str] = []


def err(msg: str) -> None:
    ERRORS.append(msg)


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def feature_name() -> str:
    m = FEATURE_RE.search(read(CARGO))
    if not m:
        err("Cargo.toml no longer defines `bridge-homekit-server`")
        return "bridge-homekit-server"
    return m.group(1)


def status_signals() -> set[str]:
    """Which signals ride as status characteristics rather than their own tile.

    Parsed from `expression()` in accessory.rs, because that function is what
    actually decides — a table here would be a third copy to keep in step.
    """
    text = read(ACCESSORY)
    m = re.search(
        r"pub fn expression\(sig: HomeSignal\) -> Expression \{(.*?)\n\}",
        text,
        re.S,
    )
    if not m:
        err("could not find `expression()` in src/bridge/hap/accessory.rs")
        return set()
    body = m.group(1)
    arm = re.search(r"(.*?)=> Expression::Status", body, re.S)
    if not arm:
        err("`expression()` no longer has an Expression::Status arm")
        return set()
    return {snake(v) for v in re.findall(r"HomeSignal::(\w+)", arm.group(1))}


def snake(variant: str) -> str:
    """`MotionPerson` -> `motion_person`."""
    return re.sub(r"(?<!^)(?=[A-Z])", "_", variant).lower()


def home_app_rendering(signal: dict, is_status: bool) -> str:
    """How this signal appears to someone looking at the Home app."""
    if is_status:
        return "Status on each sensor"
    return {
        "motion-detected": "Motion sensor",
        "occupancy-detected": "Occupancy sensor",
        "contact-sensor-state": "Contact sensor",
    }.get(signal["hap_characteristic"], "Sensor")


def signal_table(hk: dict) -> str:
    status = status_signals()
    rows = [
        "| Signal | Tile name in Home | Appears as | On by default |",
        "|---|---|---|---|",
    ]
    for s in hk["signals"]:
        is_status = s["id"] in status
        default = "yes" if s["default_enabled"] else "**no** — you must ask for it"
        # Tamper is the one signal that cannot be switched off at all, which
        # is a promise worth making visible in the table rather than a
        # footnote someone skips.
        if s["id"] == "tamper":
            default = "**always** — cannot be turned off"
        # The tile name is the bridge's per-service Name characteristic
        # (accessory.rs::service_name), not our label — telling a reader to
        # look for "Motion (person)" when the tile says "Person" sends them
        # hunting for something that is not on the screen. A status signal
        # hosts no service, so it has no tile of its own to name.
        tile = "— rides on each tile" if is_status else f"**{s['home_app_name']}**"
        rows.append(
            f"| {s['label']} | {tile} | {home_app_rendering(s, is_status)} "
            f"| {default} |")
    return "\n".join(rows)


def class_signal_list(hk: dict) -> str:
    ids = [s["id"] for s in hk["signals"] if not s["default_enabled"]]
    return ", ".join(f"`{i}`" for i in ids)


def pacing_block(hk: dict) -> str:
    p = hk["pacing"]
    default_s = p["default_tick_ms"] / 1000
    return (
        f"- **Default:** {p['default_tick_ms']} ms "
        f"({default_s:g} s) — instant to a human, and nothing downstream can "
        f"place an event more precisely than that.\n"
        f"- **Range:** {p['min_tick_ms']} ms to {p['max_tick_ms']} ms. "
        f"A value outside it is **refused, not clamped** — pacing is a privacy "
        f"parameter, so being quietly overruled would be worse than an error.\n"
        f"- **Motion hold:** {p['default_motion_hold_ticks']} ticks, so a "
        f"momentary event stays visible long enough for an automation to see it."
    )


def build_block(feature: str) -> str:
    return (
        "```sh\n"
        f"cargo build --release --features {feature} --bin hap_bridge\n"
        "```"
    )


def stamp(text: str, name: str, body: str, path: Path) -> str:
    """Replace the content between `<!-- BEGIN GENERATED: name -->` markers."""
    begin = f"<!-- BEGIN GENERATED: {name} -->"
    end = f"<!-- END GENERATED: {name} -->"
    pattern = re.compile(
        re.escape(begin) + r".*?" + re.escape(end),
        re.S,
    )
    if not pattern.search(text):
        err(f"{path.relative_to(REPO)} is missing the `{name}` generated block")
        return text
    replacement = f"{begin}\n<!-- Generated by scripts/gen_apple_home_docs.py — do not edit by hand. -->\n{body}\n{end}"
    return pattern.sub(lambda _: replacement, text)


def main() -> int:
    check = "--check" in sys.argv

    dictionary = json.loads(read(DICTIONARY))
    hk = dictionary["homekit_projection"]
    feature = feature_name()

    text = read(QUICKSTART)
    updated = text
    updated = stamp(updated, "signals", signal_table(hk), QUICKSTART)
    updated = stamp(updated, "classes", class_signal_list(hk), QUICKSTART)
    updated = stamp(updated, "pacing", pacing_block(hk), QUICKSTART)
    updated = stamp(updated, "build", build_block(feature), QUICKSTART)

    if ERRORS:
        for e in ERRORS:
            print(f"gen_apple_home_docs.py: ERROR: {e}", file=sys.stderr)
        return 1

    if updated == text:
        print("gen_apple_home_docs.py: OK — Apple Home docs match the dictionary.")
        return 0

    if check:
        print(
            "gen_apple_home_docs.py: DRIFT — "
            f"{QUICKSTART.relative_to(REPO)} does not match "
            "spec/witness_dictionary.json.\n"
            "  Fix: python3 scripts/gen_apple_home_docs.py",
            file=sys.stderr,
        )
        return 1

    QUICKSTART.write_text(updated, encoding="utf-8")
    print(f"gen_apple_home_docs.py: regenerated {QUICKSTART.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
