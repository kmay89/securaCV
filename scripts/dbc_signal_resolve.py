#!/usr/bin/env python3
"""dbc_signal_resolve.py — resolve a named DBC signal into a CanRoute byte/mask.

Canary Vehicle's `CanRoute` (src/adapter/can_bus.rs) is a single-byte model:
`(can_id, byte_offset, mask, equals)`. Hand-deriving that from a DBC signal's
`start_bit@byte_order` is exactly the kind of arithmetic that's easy to get
subtly wrong (Motorola vs Intel bit numbering) and easy to fat-finger when
copying into `adapter_host.toml`. This script does that arithmetic once, with
golden-vector tests (`--selftest`), so nobody re-derives it by hand.

Two modes:
  resolve         Print the (can_id, byte_offset, mask) for one signal.
  check-profiles  Drift-gate: re-resolve every signal referenced by
                  vehicle_profiles.toml against its vendored DBC excerpt and
                  fail if the stored byte_offset/mask doesn't match what the
                  DBC actually says (a typo, a hand-edit, or an upstream
                  rename would all be caught here). Mirrors the philosophy of
                  scripts/lint_dictionary_sync.py — the derived artifact is
                  checked against its real source, not trusted on faith.
  selftest        Run bit-math unit tests against known-good vectors pulled
                  from the vendored excerpts (see docs/hardware/vehicle_dbc/).

See docs/hardware/canary_vehicle_profiles.md for the full picture (why DBC,
why opendbc, what's bench-confirmed vs. documented-only per vehicle).

Only single-byte-contained signals are supported — CanRoute has no multi-byte
model. A signal that spans a byte boundary is refused with a clear error,
never silently truncated.

Run:
  python3 scripts/dbc_signal_resolve.py selftest
  python3 scripts/dbc_signal_resolve.py resolve <dbc> <message> <signal>
  python3 scripts/dbc_signal_resolve.py check-profiles [vehicle_profiles.toml]
"""

from __future__ import annotations

import argparse
import re
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

BO_RE = re.compile(r"^BO_\s+(\d+)\s+(\w+)\s*:\s*(\d+)\s+(\w+)")
SG_RE = re.compile(
    r'^\s*SG_\s+(\w+)\s*:\s*(\d+)\|(\d+)@([01])([+-])\s*'
    r"\(([^,]+),([^)]+)\)\s*\[([^|]*)\|([^\]]*)\]\s*\"([^\"]*)\"\s*(.*)$"
)


@dataclass
class Signal:
    name: str
    start_bit: int
    length: int
    byte_order: str  # '0' = Motorola/big-endian, '1' = Intel/little-endian
    signed: str


@dataclass
class Message:
    can_id: int
    name: str
    dlc: int
    signals: dict[str, Signal]


def parse_dbc(path: Path) -> dict[str, Message]:
    """Parse BO_/SG_ blocks from a .dbc file. Ignores everything else (VERSION,
    NS_, BS_, BU_, comments, VAL_ tables) — this tool only needs message/signal
    geometry, not the full DBC semantic model."""
    messages: dict[str, Message] = {}
    current: Message | None = None
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        if raw_line.startswith("//"):
            continue
        bo = BO_RE.match(raw_line)
        if bo:
            can_id, name, dlc, _sender = bo.groups()
            current = Message(can_id=int(can_id), name=name, dlc=int(dlc), signals={})
            messages[name] = current
            continue
        sg = SG_RE.match(raw_line)
        if sg and current is not None:
            sig_name, start_bit, length, byte_order, signed = sg.groups()[:5]
            current.signals[sig_name] = Signal(
                name=sig_name,
                start_bit=int(start_bit),
                length=int(length),
                byte_order=byte_order,
                signed=signed,
            )
    return messages


class SignalSpanError(ValueError):
    """Raised when a signal doesn't fit CanRoute's single-byte model."""


def resolve_signal(sig: Signal) -> tuple[int, int]:
    """Return (byte_offset, mask) for a signal fully contained within one byte.

    DBC bit numbering: `start_bit` locates one bit in an absolute numbering
    where byte_index = start_bit // 8 always, for both byte orders — that part
    is order-independent. What byte ORDER changes is which direction the
    REST of a multi-bit field extends from that bit within the byte:
      - Motorola (@0, big-endian): the field's MSB is at `start_bit`; the
        remaining (length-1) bits extend toward LOWER bit numbers.
      - Intel (@1, little-endian): the field's LSB is at `start_bit`; the
        remaining (length-1) bits extend toward HIGHER bit numbers.
    """
    byte_index = sig.start_bit // 8
    bit_in_byte = sig.start_bit % 8
    if sig.byte_order == "0":  # Motorola
        lsb = bit_in_byte - (sig.length - 1)
        if lsb < 0:
            raise SignalSpanError(
                f"signal '{sig.name}' spans a byte boundary (Motorola, start_bit="
                f"{sig.start_bit}, length={sig.length}) — CanRoute is single-byte only"
            )
        mask = ((1 << sig.length) - 1) << lsb
    else:  # Intel
        if bit_in_byte + sig.length > 8:
            raise SignalSpanError(
                f"signal '{sig.name}' spans a byte boundary (Intel, start_bit="
                f"{sig.start_bit}, length={sig.length}) — CanRoute is single-byte only"
            )
        mask = ((1 << sig.length) - 1) << bit_in_byte
    return byte_index, mask


def resolve(dbc_path: Path, message_name: str, signal_name: str) -> tuple[int, int, int]:
    """Return (can_id, byte_offset, mask) for message.signal in dbc_path."""
    messages = parse_dbc(dbc_path)
    if message_name not in messages:
        raise KeyError(f"message '{message_name}' not found in {dbc_path}")
    msg = messages[message_name]
    if signal_name not in msg.signals:
        raise KeyError(f"signal '{signal_name}' not found in message '{message_name}'")
    byte_offset, mask = resolve_signal(msg.signals[signal_name])
    return msg.can_id, byte_offset, mask


# ---------------------------------------------------------------------------
# selftest — golden vectors, hand-verified against the vendored excerpts
# ---------------------------------------------------------------------------

def selftest() -> int:
    cases = [
        # (dbc, message, signal, expected_can_id, expected_byte_offset, expected_mask)
        ("honda_common_excerpt.dbc", "DOORS_STATUS", "DOOR_OPEN_FL", 1029, 4, 0x20),
        ("honda_common_excerpt.dbc", "DOORS_STATUS", "DOOR_OPEN_FR", 1029, 4, 0x40),
        ("honda_common_excerpt.dbc", "DOORS_STATUS", "DOOR_OPEN_RL", 1029, 4, 0x80),
        ("honda_common_excerpt.dbc", "DOORS_STATUS", "DOOR_OPEN_RR", 1029, 5, 0x01),
        ("honda_common_excerpt.dbc", "DOORS_STATUS", "TRUNK_OPEN", 1029, 5, 0x02),
        ("honda_common_excerpt.dbc", "GEARBOX_CVT", "SELECTED_P", 401, 0, 0x01),
        ("honda_common_excerpt.dbc", "GEARBOX_CVT", "GEAR_SHIFTER", 401, 5, 0x1F),
        ("toyota_2017_pt_excerpt.dbc", "ECT1S92", "B_P", 956, 1, 0x20),
        ("toyota_2017_pt_excerpt.dbc", "ECT1S92", "B_D", 956, 5, 0x80),
        ("vw_mqb_excerpt.dbc", "Klemmen_Status_01", "ZAS_Kl_15", 960, 2, 0x02),
        ("vw_mqb_excerpt.dbc", "Klemmen_Status_01", "ZAS_Kl_50", 960, 2, 0x08),
        ("vw_mqb_excerpt.dbc", "Getriebe_06", "GE_Ist_Fahrstufe", 296, 0, 0xF0),
    ]
    vendor_dir = REPO / "docs/hardware/vehicle_dbc"
    failures = 0
    for dbc_file, message, signal, want_id, want_byte, want_mask in cases:
        got_id, got_byte, got_mask = resolve(vendor_dir / dbc_file, message, signal)
        ok = (got_id, got_byte, got_mask) == (want_id, want_byte, want_mask)
        status = "ok" if ok else "FAIL"
        print(
            f"[{status}] {dbc_file}:{message}.{signal} -> "
            f"id={got_id} byte={got_byte} mask={got_mask:#04x} "
            f"(want id={want_id} byte={want_byte} mask={want_mask:#04x})"
        )
        if not ok:
            failures += 1

    # A signal that spans a byte boundary must be refused, not silently wrong.
    # ENG1F03.GEARINF: start_bit=19, length=4, Motorola -> lsb = (19%8) - 3 = 0,
    # byte = 19//8 = 2. That one fits. Construct a synthetic one that doesn't:
    crossing = Signal(name="CROSSES", start_bit=4, length=8, byte_order="0", signed="+")
    try:
        resolve_signal(crossing)
        print("[FAIL] expected SignalSpanError for a Motorola signal crossing a byte boundary")
        failures += 1
    except SignalSpanError:
        print("[ok] byte-crossing Motorola signal correctly refused")

    crossing_intel = Signal(name="CROSSES", start_bit=4, length=8, byte_order="1", signed="+")
    try:
        resolve_signal(crossing_intel)
        print("[FAIL] expected SignalSpanError for an Intel signal crossing a byte boundary")
        failures += 1
    except SignalSpanError:
        print("[ok] byte-crossing Intel signal correctly refused")

    if failures:
        print(f"\n{failures} selftest failure(s)")
        return 1
    print(f"\nOK: {len(cases)} golden vectors + 2 boundary-crossing refusals")
    return 0


# ---------------------------------------------------------------------------
# check-profiles — drift gate for vehicle_profiles.toml
# ---------------------------------------------------------------------------

def check_profiles(profiles_path: Path) -> int:
    data = tomllib.loads(profiles_path.read_text(encoding="utf-8"))
    errors: list[str] = []
    checked = 0
    for vehicle in data.get("vehicle", []):
        vid = vehicle.get("id", "<unnamed>")
        dbc_rel = vehicle.get("dbc")
        if not dbc_rel:
            errors.append(f"{vid}: missing 'dbc' path")
            continue
        dbc_path = REPO / dbc_rel
        if not dbc_path.is_file():
            errors.append(f"{vid}: dbc file not found: {dbc_rel}")
            continue
        for sig in vehicle.get("signal", []):
            sid = f"{vid}/{sig.get('name', '<unnamed>')}"
            try:
                can_id, byte_offset, mask = resolve(
                    dbc_path, sig["message"], sig["signal"]
                )
            except (KeyError, SignalSpanError) as e:
                errors.append(f"{sid}: {e}")
                continue
            checked += 1
            want = (sig.get("can_id"), sig.get("byte_offset"), sig.get("mask"))
            got_hex_mask = mask
            got = (can_id, byte_offset, got_hex_mask)
            # Profile stores can_id/mask as decimal or 0x-hex strings, same
            # grammar adapter_host.toml accepts — normalize before comparing.
            def norm(v):
                if isinstance(v, str):
                    v = v.strip()
                    return int(v, 16) if v.lower().startswith("0x") else int(v)
                return v

            want_norm = tuple(norm(v) for v in want)
            if want_norm != got:
                errors.append(
                    f"{sid}: profile says (can_id={want[0]}, byte_offset={want[1]}, "
                    f"mask={want[2]}) but {dbc_rel} resolves to "
                    f"(can_id={can_id}, byte_offset={byte_offset}, mask={mask:#04x}) — "
                    "regenerate the profile entry from the DBC"
                )

    if errors:
        print(f"[dbc-lint] {len(errors)} drift error(s):", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1
    print(f"[dbc-lint] OK: {checked} signal(s) across "
          f"{len(data.get('vehicle', []))} vehicle profile(s) match their DBC excerpts")
    return 0


def emit_routes(profiles_path: Path, vehicle_id: str) -> int:
    """Print ready-to-paste [[adapter.route]] TOML blocks for one vehicle profile — the
    DBC-to-adapter_host.toml loop, closed. Every 1-bit signal gets TWO routes (bit=1, bit=0),
    the same "one route per transition, sharing a can_id" convention documented in
    adapter_host.example.toml — a 1-bit signal has exactly two states, so both are always
    knowable without guessing. A signal wider than 1 bit is skipped with a note: this tool knows
    the bit geometry, not which of its several values means what on your vehicle."""
    data = tomllib.loads(profiles_path.read_text(encoding="utf-8"))
    vehicles = {v["id"]: v for v in data.get("vehicle", [])}
    if vehicle_id not in vehicles:
        print(f"unknown vehicle id '{vehicle_id}'. Known: {', '.join(sorted(vehicles))}", file=sys.stderr)
        return 1
    vehicle = vehicles[vehicle_id]
    dbc_path = REPO / vehicle["dbc"]
    print(f"# {vehicle['make']} {vehicle['model']} ({vehicle['years']}) — status: {vehicle['status']}")
    print(f"# source: {vehicle['dbc']}")
    if vehicle["status"] != "bench-confirmed":
        print("# ⚠️ NOT bench-confirmed — verify against your actual vehicle before trusting this.")
    for sig in vehicle.get("signal", []):
        can_id, byte_offset, mask = resolve(dbc_path, sig["message"], sig["signal"])
        if mask == 0 or (mask & (mask - 1)) != 0:
            print(f"# skipping '{sig['name']}': multi-bit signal (mask {mask:#04x}) — "
                  "supply the target value(s) yourself, this tool only knows the bit geometry")
            continue
        print(f"\n# {sig['name']} ({vehicle['dbc']}: {sig['message']}.{sig['signal']})")
        for state, equals in (("on", mask), ("off", 0x00)):
            print("[[adapter.route]]")
            print(f'can_id = "0x{can_id:X}"')
            print(f"byte_offset = {byte_offset}")
            print(f'mask = "0x{mask:02X}"')
            print(f'equals = "0x{equals:02X}"')
            print(f'kind = "{sig["kind"]}"')
            print(f'zone = "{sig["zone"]}"')
            print(f'# ^ {sig["name"]} = {state}')
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("selftest", help="run bit-math golden-vector tests")

    p_resolve = sub.add_parser("resolve", help="resolve one signal to (can_id, byte_offset, mask)")
    p_resolve.add_argument("dbc", type=Path)
    p_resolve.add_argument("message")
    p_resolve.add_argument("signal")

    p_check = sub.add_parser("check-profiles", help="drift-gate vehicle_profiles.toml against vendored DBCs")
    p_check.add_argument(
        "profiles", type=Path, nargs="?",
        default=REPO / "docs/hardware/vehicle_dbc/vehicle_profiles.toml",
    )

    p_emit = sub.add_parser("emit-routes", help="print [[adapter.route]] TOML for one vehicle profile")
    p_emit.add_argument("vehicle_id")
    p_emit.add_argument(
        "--profiles", type=Path,
        default=REPO / "docs/hardware/vehicle_dbc/vehicle_profiles.toml",
    )

    args = ap.parse_args()
    if args.cmd == "selftest":
        return selftest()
    if args.cmd == "resolve":
        can_id, byte_offset, mask = resolve(args.dbc, args.message, args.signal)
        print(f"can_id = \"0x{can_id:X}\"")
        print(f"byte_offset = {byte_offset}")
        print(f"mask = \"0x{mask:02X}\"")
        return 0
    if args.cmd == "check-profiles":
        return check_profiles(args.profiles)
    if args.cmd == "emit-routes":
        return emit_routes(args.profiles, args.vehicle_id)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
