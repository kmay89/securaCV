#!/usr/bin/env python3
"""Pin budget gauge — how full is each board, and what room is left.

Every board dir ships a data-only pins/pins.h (enforced by
check_board_registry.py: "pins are data, not code"). That makes the pin
map machine-readable — so instead of answering "do we have a spare GPIO
for X?" by re-reading vendor wikis, this script derives a standardized
utilization report per board:

  * which GPIOs the pin map COMMITS to onboard hardware (display, SD,
    camera, LEDs, buttons, sensors — hard wiring),
  * which are ASSIGNED by convention but reclaimable (the I2C/SPI/UART
    header suggestions, when nothing onboard rides them),
  * which are CONDITIONAL (USB-Serial/JTAG data lines, the UART0 console
    pair, strapping pins — usable if you accept the trade),
  * and which are genuinely FREE, with ADC / deep-sleep-wake notes,

plus an approximate peripheral-channel budget (SPI/I2C/UART buses, RMT,
LEDC) against the MCU's totals, and the HAS_* capability flags as the
feature gauge. The one-line summary is the fuel gauge:

  10/24 committed · 2 assigned · 6 conditional · 6 free (4 ADC)

Buckets are a priority ladder — each GPIO is counted once, in the first
bucket that claims it: committed > assigned > conditional > free.

What this is NOT: it reads the declared pin map, not the running code.
A peripheral that exists but is unused at runtime (e.g. a TF slot with
no SD driver enabled) still counts as committed — the copper is spent.
Off-map wiring that bypasses pins.h (e.g. a radar riding the UART0
header pins) shows up as conditional, not committed; the per-board
README stays the narrative source for that.

Modes:
    python3 firmware/scripts/pin_budget.py                # print report
    python3 firmware/scripts/pin_budget.py --board <id>   # one board
    python3 firmware/scripts/pin_budget.py --write        # regen the doc
    python3 firmware/scripts/pin_budget.py --check        # CI: fail on drift
    python3 firmware/scripts/pin_budget.py --json         # machine-readable

The generated doc is firmware/boards/PIN_BUDGET.md — generated AND
committed (same philosophy as the website's .glb models): edit a pins.h,
re-run --write in the same change, and --check keeps CI honest.
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BOARDS_DIR = REPO_ROOT / "firmware" / "boards"
REGISTRY = BOARDS_DIR / "boards.json"
DOC_PATH = BOARDS_DIR / "PIN_BUDGET.md"

# ----------------------------------------------------------------------------
# Per-MCU resource tables.
#
# "usable" is the module-level GPIO set: package pins minus the in-module
# flash (and, for octal-PSRAM S3 modules, the PSRAM lines — applied per
# board below). Sources: Espressif datasheets/TRMs for ESP32-C3, ESP32-C6,
# ESP32-S3. Conditional pins are usable only at a cost, which the report
# spells out.
# ----------------------------------------------------------------------------

MCUS = {
    "ESP32": {
        # Classic dual-core ESP32 (WROOM-32 family). Package GPIOs 20, 24 and
        # 28-31 do not exist, and 37/38 are not bonded out on WROOM/WROVER
        # modules; 6-11 carry the SPI flash. 34-39 are input-only (no output
        # driver, no pulls) — the maps themselves document that.
        "usable": set(range(0, 20)) | {21, 22, 23, 25, 26, 27, 32, 33, 34, 35, 36, 39},
        "flash_reserved": set(range(6, 12)),
        # External quad PSRAM on classic modules (WROVER, ESP32-CAM) hangs
        # off GPIO16/17 (CS + clock) — subtracted in bucketize() when the
        # board carries PSRAM, same pattern as the S3 octal lines.
        "psram_reserved": {16, 17},
        "strapping": {0, 2, 5, 12, 15},
        "adc": {0, 2, 4, 12, 13, 14, 15, 25, 26, 27} | set(range(32, 40)),
        "usb": set(),                     # no native USB — UART flashing only
        "uart0": {1, 3},                  # default console
        "sleep_wake": {0, 2, 4, 12, 13, 14, 15, 25, 26, 27} | set(range(32, 40)),
        "periph": {"SPI": 2, "I2C": 2, "UART": 3, "RMT TX": 8, "LEDC": 16},
    },
    "ESP32-C3": {
        "usable": set(range(0, 11)) | {18, 19, 20, 21},
        "flash_reserved": set(range(11, 18)),
        "strapping": {2, 8, 9},
        "adc": set(range(0, 6)),          # ADC1 0-4, ADC2 5 (Wi-Fi caveat)
        "usb": {18, 19},                  # USB-Serial/JTAG D-/D+
        "uart0": {20, 21},                # default console
        "sleep_wake": set(range(0, 6)),   # RTC/deep-sleep-wake capable
        "periph": {"SPI": 1, "I2C": 1, "UART": 2, "RMT TX": 2, "LEDC": 6},
    },
    "ESP32-C6": {
        "usable": set(range(0, 24)),
        "flash_reserved": set(range(24, 31)),
        "strapping": {4, 5, 8, 9, 15},
        "adc": set(range(0, 7)),          # ADC1 ch0-6
        "usb": {12, 13},                  # USB-Serial/JTAG D-/D+
        "uart0": {16, 17},                # default console
        "sleep_wake": set(range(0, 8)),   # LP GPIOs
        "periph": {"SPI": 1, "I2C": 1, "UART": 2, "RMT TX": 2, "LEDC": 6},
    },
    "ESP32-S3": {
        # Package GPIOs; the in-module flash (26-32) and, on octal-PSRAM
        # modules, the PSRAM lines (33-37 — matches the PIN_RESERVED_PSRAM_*
        # headers) are subtracted in bucketize() before anything is counted.
        "usable": set(range(0, 22)) | set(range(26, 49)),
        "flash_reserved": set(range(26, 33)),
        "octal_psram_reserved": set(range(33, 38)),
        "strapping": {0, 3, 45, 46},
        "adc": set(range(1, 21)),         # ADC1 1-10, ADC2 11-20 (Wi-Fi caveat)
        "usb": {19, 20},                  # native USB / USB-Serial/JTAG
        "uart0": {43, 44},                # default console
        "sleep_wake": set(range(0, 22)),  # RTC GPIOs
        "periph": {"SPI": 2, "I2C": 2, "UART": 3, "RMT TX": 4, "LEDC": 8},
    },
}

# Exposure aliases enumerate what the board breaks out physically; they
# claim nothing. PIN_D6/PIN_A0-style names (XIAO) and PIN_GPIO* (devkits).
ALIAS_RE = re.compile(r"^PIN_(D\d+|A\d+|GPIO\d+)$")

# Annotation defines — reservations/strap notes/ADC channel enumerations,
# never usage.
ANNOTATION_RE = re.compile(r"(RESERVED|STRAP)")
ADC_ENUM_RE = re.compile(r"^ADC\d*_CH\d+_PIN$")

# Real copper despite not carrying a PIN token.
SPECIAL_FUNCTIONAL = {"LED_BUILTIN"}

# A functional define must carry PIN as a whole underscore-token.
DEFINE_RE = re.compile(
    r"^\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(-?\d+)\s*$", re.M)

# Groups that are header-convention ASSIGNMENTS (reclaimable) rather than
# onboard copper — unless promoted to hard below (e.g. I2C driving touch).
SOFT_GROUPS = {"I2C", "SPI", "UART0", "UART1", "UART2", "GROVE"}

# USB data-line defines duplicate the MCU-level conditional bucket.
USB_GROUPS = {"USB_DM", "USB_DP", "USB"}


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def group_of(name: str) -> str:
    """TFT_PIN_SCK -> TFT · RGBLED_PIN -> RGBLED · UART1_PIN_TX -> UART1."""
    if "_PIN_" in name:
        return name.split("_PIN_")[0]
    if name.endswith("_PIN"):
        return name[: -len("_PIN")]
    return name


def parse_pins(pins_h: Path):
    """Return (functional {gpio: [names]}, aliases {gpio}, flags {name: val})."""
    text = strip_comments(pins_h.read_text(encoding="utf-8"))
    functional, aliases, flags, unresolved = {}, set(), {}, []
    for name, raw in DEFINE_RE.findall(text):
        val = int(raw)
        if name.startswith("HAS_"):
            flags[name] = val
            continue
        if val < 0:
            if "PIN" in name.split("_") and not ALIAS_RE.match(name):
                unresolved.append(name)
            continue
        if name in SPECIAL_FUNCTIONAL:
            functional.setdefault(val, []).append(name)
            continue
        if "PIN" not in name.split("_"):
            continue
        if ALIAS_RE.match(name):
            aliases.add(val)
            continue
        if ANNOTATION_RE.search(name) or ADC_ENUM_RE.match(name):
            continue
        functional.setdefault(val, []).append(name)
    return functional, aliases, flags, sorted(unresolved)


def is_soft(name: str) -> bool:
    """Header-convention suggestion rather than committed copper."""
    return name.endswith("_DEFAULT") or group_of(name) in SOFT_GROUPS


def bucketize(board, functional, flags):
    mcu = MCUS[board["mcu"]]
    # Never offer module-reserved copper: flash lines always, PSRAM lines
    # on octal-PSRAM modules. (A gauge that lists GPIO26-32 as free would
    # invite wiring that hangs or corrupts the module.) Quad PSRAM shares
    # the flash bus and takes no extra GPIOs — the interface comes from
    # boards.json psram_type (matching the env's memory_type: qio_opi =
    # octal, qio_qspi = quad); with PSRAM present but no psram_type we
    # assume octal, because under-offering is the safe failure.
    usable = set(mcu["usable"]) - mcu.get("flash_reserved", set())
    if (board["mcu"] == "ESP32-S3" and board.get("psram_mb", 0) > 0
            and board.get("psram_type", "octal") == "octal"):
        usable -= mcu["octal_psram_reserved"]
    # Classic ESP32 PSRAM is an external die on GPIO16/17 (WROVER, ESP32-CAM)
    # — never offer those as free copper on a PSRAM-bearing classic board.
    # (17 isn't even bonded out on the ESP32-CAM; under-offering is the safe
    # failure.)
    if board["mcu"] == "ESP32" and board.get("psram_mb", 0) > 0:
        usable -= mcu["psram_reserved"]

    # I2C is hard copper when something onboard rides the bus (touch, an
    # IMU/RTC pair — flagged via HAS_TOUCH/HAS_RTC).
    i2c_is_hard = flags.get("HAS_TOUCH", 0) == 1 or flags.get("HAS_RTC", 0) == 1

    committed = {}
    for gpio, names in sorted(functional.items()):
        if gpio not in usable:
            # Pin map references a pin this module reserves — surface loudly.
            committed[gpio] = names + ["⚠ conflicts with module-reserved pin"]
            continue
        groups = {group_of(n) for n in names}
        hard = any(not is_soft(n) and group_of(n) not in USB_GROUPS
                   for n in names)
        if hard or (i2c_is_hard and "I2C" in groups):
            committed[gpio] = names

    conditional = {}
    for gpio in sorted(usable):
        if gpio in committed:
            continue
        why = []
        if gpio in mcu["usb"]:
            why.append("USB-Serial/JTAG — free only if you give up USB")
        if gpio in mcu["uart0"]:
            why.append("UART0 console — free only if you give up the serial log")
        if gpio in mcu["strapping"]:
            why.append("strapping pin — must not be driven at reset; "
                       "check the boot-mode level before repurposing")
        if not why and gpio in functional and all(
                group_of(n) in USB_GROUPS for n in functional[gpio]):
            why.append("USB data line")
        if why:
            riding = sorted(set(functional.get(gpio, [])))
            if riding:
                why.append(f"(declared as {', '.join(riding)})")
            conditional[gpio] = why

    assigned = {}
    for gpio, names in sorted(functional.items()):
        if gpio in usable and gpio not in committed and gpio not in conditional:
            assigned[gpio] = names

    free = sorted(usable - set(committed) - set(assigned) - set(conditional))
    return usable, committed, assigned, conditional, free


def pin_notes(gpio, mcu):
    notes = []
    if gpio in mcu["adc"]:
        notes.append("ADC")
    if gpio in mcu["sleep_wake"]:
        notes.append("sleep-wake")
    if gpio in mcu["strapping"]:
        notes.append("strap⚠")
    return notes


def periph_usage(functional):
    """Approximate bus/channel demand from the declared groups."""
    groups = {}
    for names in functional.values():
        for n in names:
            groups.setdefault(group_of(n), set()).add(n)
    # Count SPI buses by distinct clock pins, but only from groups that are
    # actually SPI — a mic/I2S/camera clock is not another SPI controller.
    spi_bus_groups = {"SPI", "TFT", "SD", "EPD", "LORA"}
    spi_clk_pins = set()
    for gpio, names in functional.items():
        if any(n.endswith(("_SCK", "_CLK")) and group_of(n) in spi_bus_groups
               for n in names):
            spi_clk_pins.add(gpio)
    used = {
        "SPI": len(spi_clk_pins),
        "I2C": 1 if any(g == "I2C" for g in groups) else 0,
        "UART": sum(1 for g in groups if re.match(r"UART[12]$", g)),
        "RMT TX": sum(1 for g in groups if g in ("RGBLED", "LED_WS2812")),
        "LEDC": sum(1 for g in groups if g in ("BUZZER",))
                + sum(1 for names in functional.values()
                      for n in names if n.endswith("_PIN_BL") or n == "TFT_PIN_BL"),
    }
    return used


def gauge_line(usable, committed, assigned, conditional, free, mcu):
    free_adc = sum(1 for g in free if g in mcu["adc"])
    return (f"**{len(committed)}/{len(usable)} committed** · "
            f"{len(assigned)} assigned · {len(conditional)} conditional · "
            f"**{len(free)} free** ({free_adc} ADC-capable)")


def render_board(board, pins_h):
    mcu = MCUS[board["mcu"]]
    functional, aliases, flags, unresolved = parse_pins(pins_h)
    usable, committed, assigned, conditional, free = bucketize(
        board, functional, flags)

    out = [f"### `{board['id']}` — {board['name']}", ""]
    out.append(f"{board['mcu']} · flash {board['flash_mb']} MB · "
               f"PSRAM {board['psram_mb']} MB · pin map "
               f"[`pins/pins.h`]({board['id']}/pins/pins.h)")
    out.append("")
    out.append(gauge_line(usable, committed, assigned, conditional, free, mcu))
    out.append("")

    out.append("| GPIO | bucket | held by / trade | notes |")
    out.append("|---|---|---|---|")
    for gpio in sorted(usable):
        notes = ", ".join(pin_notes(gpio, mcu))
        if gpio in committed:
            out.append(f"| {gpio} | committed | {', '.join(sorted(set(committed[gpio])))} | {notes} |")
        elif gpio in assigned:
            out.append(f"| {gpio} | assigned | {', '.join(sorted(set(assigned[gpio])))} | {notes} |")
        elif gpio in conditional:
            out.append(f"| {gpio} | conditional | {'; '.join(conditional[gpio])} | {notes} |")
        else:
            out.append(f"| {gpio} | **free** | — | {notes} |")
    out.append("")

    if aliases:
        exposed_free = sorted(set(free) & aliases)
        out.append(f"Physically broken out (from `PIN_D*/PIN_A*/PIN_GPIO*` "
                   f"aliases): {sorted(aliases)} — of the free pins, "
                   f"**{exposed_free or 'none'}** reach a header.")
        out.append("")

    if unresolved:
        out.append(f"⚠ {len(unresolved)} pin define(s) are `-1` — not wired "
                   f"OR not yet verified (see the comments in pins.h): "
                   f"{', '.join(f'`{n}`' for n in unresolved)}. Free counts "
                   f"above may shrink as these resolve.")
        out.append("")

    used = periph_usage(functional)
    parts = []
    for k, total in mcu["periph"].items():
        parts.append(f"{k} {used.get(k, 0)}/{total}")
    out.append(f"Peripheral demand (declared pin map vs MCU): {' · '.join(parts)}.")
    out.append("")

    on = sorted(k for k, v in flags.items() if v)
    off = sorted(k for k, v in flags.items() if not v)
    out.append(f"Capabilities on: {', '.join(f'`{f}`' for f in on) or '—'}.")
    out.append(f"Capabilities off (room to grow): "
               f"{', '.join(f'`{f}`' for f in off) or '—'}.")
    out.append("")
    if board.get("thermal_notes"):
        out.append(f"**Thermals:** {board['thermal_notes']}")
        out.append("")
    return (out, len(usable), len(committed), len(assigned),
            len(conditional), len(free))


def render_doc():
    entries = json.loads(REGISTRY.read_text(encoding="utf-8"))
    if isinstance(entries, dict):
        entries = entries.get("boards", entries)
    entries = sorted(entries, key=lambda e: e["id"])

    body, summary = [], []
    for board in entries:
        pins_h = BOARDS_DIR / board["id"] / "pins" / "pins.h"
        section, n_usable, n_com, n_asn, n_cond, n_free = render_board(
            board, pins_h)
        pct = round(100 * n_com / n_usable)
        summary.append(f"| `{board['id']}` | {board['mcu']} | {n_usable} | "
                       f"{n_com} ({pct}%) | {n_asn} | {n_cond} | {n_free} |")
        body.extend(section)

    head = [
        "# Pin budget — the board fullness gauge",
        "",
        "<!-- GENERATED FILE — do not edit by hand. -->",
        "<!-- Regenerate: python3 firmware/scripts/pin_budget.py --write -->",
        "<!-- CI guard:   python3 firmware/scripts/pin_budget.py --check -->",
        "",
        "One standardized question, answered per board: **how full is this",
        "firmware's pin map, and what room is left — for what?** Derived",
        "mechanically from each board's data-only `pins/pins.h` (the",
        "registry guard enforces \"pins are data\") scored against the MCU's",
        "GPIO/peripheral tables. Buckets, in claim order:",
        "",
        "- **committed** — copper spent on onboard hardware (or an explicit",
        "  pin-map commitment). Counts toward the gauge percentage.",
        "- **assigned** — header-convention assignments (I2C/SPI/UART",
        "  suggestions) that are reclaimable when nothing onboard rides them.",
        "- **conditional** — usable only at a cost: USB-Serial/JTAG data",
        "  lines, the UART0 console pair, strapping pins (⚠ = check boot",
        "  level before repurposing).",
        "- **free** — genuinely available, annotated with ADC and",
        "  deep-sleep-wake capability.",
        "",
        "Each board also carries a **Thermals** line (from `thermal_notes`",
        "in `boards.json`): where the heat comes from and what to derate",
        "before adding load to the free pins. The *runtime* thermal gauge",
        "is the die-temperature watchdog every build ships",
        "(`FEATURE_DIAGNOSTICS` — see `firmware/build_matrix.json`).",
        "",
        "The gauge reads the *declared pin map*, not runtime code: a TF slot",
        "with no SD driver still counts committed (the copper is gone), and",
        "off-map wiring documented only in a board README (e.g. a radar on",
        "the UART0 header pins) shows up as conditional. The per-board",
        "README stays the narrative source for those stories.",
        "",
        "## Fleet summary",
        "",
        "| board | MCU | usable GPIOs | committed | assigned | conditional | free |",
        "|---|---|---|---|---|---|---|",
    ]
    tail = ["", "## Per-board budgets", ""]
    return "\n".join(head + summary + tail + body).rstrip() + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--board", help="render one board id to stdout")
    ap.add_argument("--write", action="store_true",
                    help=f"write {DOC_PATH.relative_to(REPO_ROOT)}")
    ap.add_argument("--check", action="store_true",
                    help="fail if the committed doc drifts from the pin maps")
    ap.add_argument("--json", action="store_true",
                    help="emit the full budget as JSON (for tooling)")
    args = ap.parse_args()

    if args.json:
        entries = json.loads(REGISTRY.read_text(encoding="utf-8"))
        if isinstance(entries, dict):
            entries = entries.get("boards", entries)
        data = []
        for board in sorted(entries, key=lambda e: e["id"]):
            pins_h = BOARDS_DIR / board["id"] / "pins" / "pins.h"
            functional, aliases, flags, unresolved = parse_pins(pins_h)
            usable, committed, assigned, conditional, free = bucketize(
                board, functional, flags)
            mcu = MCUS[board["mcu"]]
            data.append({
                "id": board["id"],
                "mcu": board["mcu"],
                "usable": sorted(usable),
                "committed": {g: sorted(set(n)) for g, n in committed.items()},
                "assigned": {g: sorted(set(n)) for g, n in assigned.items()},
                "conditional": dict(conditional),
                "free": free,
                "free_adc": [g for g in free if g in mcu["adc"]],
                "broken_out": sorted(aliases),
                "unresolved_defines": unresolved,
                "capabilities": flags,
                "thermal_notes": board.get("thermal_notes", ""),
            })
        print(json.dumps(data, indent=2))
        return

    if args.board:
        entries = json.loads(REGISTRY.read_text(encoding="utf-8"))
        if isinstance(entries, dict):
            entries = entries.get("boards", entries)
        match = [e for e in entries if e["id"] == args.board]
        if not match:
            sys.exit(f"unknown board id: {args.board}")
        section, *_ = render_board(
            match[0], BOARDS_DIR / args.board / "pins" / "pins.h")
        print("\n".join(section))
        return

    doc = render_doc()
    if args.check:
        current = DOC_PATH.read_text(encoding="utf-8") if DOC_PATH.exists() else ""
        if current != doc:
            sys.exit("firmware/boards/PIN_BUDGET.md is stale — a pins.h or "
                     "boards.json changed without regenerating the budget.\n"
                     "Fix: python3 firmware/scripts/pin_budget.py --write "
                     "and commit the result.")
        print("pin budget: OK (doc matches the pin maps)")
        return
    if args.write:
        DOC_PATH.write_text(doc, encoding="utf-8")
        print(f"wrote {DOC_PATH.relative_to(REPO_ROOT)}")
        return
    print(doc)


if __name__ == "__main__":
    main()
