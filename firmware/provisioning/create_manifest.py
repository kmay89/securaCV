#!/usr/bin/env python3
"""
SecuraCV Canary — Fleet Manifest Management Tool

Manages the fleet manifest for provisioned devices. Provides:
- Add new devices to the manifest
- Search by MAC address or fingerprint
- Generate fleet reports
- Export/import manifest data

Usage:
    python create_manifest.py add --mac AA:BB:CC:DD:EE:FF --fingerprint abc123
    python create_manifest.py list
    python create_manifest.py search --mac AA:BB:CC
    python create_manifest.py report --output report.json
    python create_manifest.py --help
"""

import argparse
import json
import sys
from datetime import datetime
from pathlib import Path
from typing import Optional


MANIFEST_FILE = Path(__file__).parent / "fleet_manifest.json"


def load_manifest() -> dict:
    """Load the fleet manifest from disk."""
    if not MANIFEST_FILE.exists():
        return {
            "version": 1,
            "created_at": datetime.now().isoformat(),
            "updated_at": datetime.now().isoformat(),
            "devices": [],
        }

    with open(MANIFEST_FILE) as f:
        return json.load(f)


def save_manifest(manifest: dict) -> None:
    """Save the fleet manifest to disk."""
    manifest["updated_at"] = datetime.now().isoformat()
    with open(MANIFEST_FILE, "w") as f:
        json.dump(manifest, f, indent=2)


def add_device(
    mac: str,
    fingerprint: Optional[str] = None,
    chip_id: Optional[str] = None,
    phase: int = 1,
    location: Optional[str] = None,
    notes: Optional[str] = None,
) -> dict:
    """Add a new device to the manifest."""
    manifest = load_manifest()

    # Check for duplicate MAC
    for device in manifest["devices"]:
        if device["mac"].lower() == mac.lower():
            print(f"Device with MAC {mac} already exists. Updating...")
            device["fingerprint"] = fingerprint if fingerprint is not None else device.get("fingerprint")
            device["chip_id"] = chip_id if chip_id is not None else device.get("chip_id")
            device["phase"] = phase
            device["location"] = location if location is not None else device.get("location")
            device["notes"] = notes if notes is not None else device.get("notes")
            device["updated_at"] = datetime.now().isoformat()
            save_manifest(manifest)
            return device

    # Add new device
    device = {
        "mac": mac.upper(),
        "fingerprint": fingerprint,
        "chip_id": chip_id,
        "phase": phase,
        "location": location,
        "notes": notes,
        "provisioned_at": datetime.now().isoformat(),
        "updated_at": datetime.now().isoformat(),
        "firmware_version": None,
        "last_seen": None,
    }

    manifest["devices"].append(device)
    save_manifest(manifest)

    return device


def list_devices(phase: Optional[int] = None) -> list:
    """List all devices in the manifest."""
    manifest = load_manifest()
    devices = manifest["devices"]

    if phase is not None:
        devices = [d for d in devices if d.get("phase") == phase]

    return devices


def search_devices(
    mac: Optional[str] = None,
    fingerprint: Optional[str] = None,
    location: Optional[str] = None,
) -> list:
    """Search for devices by various criteria."""
    manifest = load_manifest()
    results = []

    for device in manifest["devices"]:
        if mac and mac.lower() in device["mac"].lower():
            results.append(device)
            continue
        if fingerprint and device.get("fingerprint") and \
           fingerprint.lower() in device["fingerprint"].lower():
            results.append(device)
            continue
        if location and device.get("location") and \
           location.lower() in device["location"].lower():
            results.append(device)
            continue

    return results


def remove_device(mac: str) -> bool:
    """Remove a device from the manifest."""
    manifest = load_manifest()

    for i, device in enumerate(manifest["devices"]):
        if device["mac"].lower() == mac.lower():
            del manifest["devices"][i]
            save_manifest(manifest)
            return True

    return False


def generate_report(output: Optional[Path] = None) -> dict:
    """Generate a fleet summary report."""
    manifest = load_manifest()

    # Count by phase
    phase_counts = {}
    for device in manifest["devices"]:
        phase = device.get("phase", 0)
        phase_counts[phase] = phase_counts.get(phase, 0) + 1

    report = {
        "generated_at": datetime.now().isoformat(),
        "summary": {
            "total_devices": len(manifest["devices"]),
            "by_phase": phase_counts,
        },
        "devices": manifest["devices"],
    }

    if output:
        with open(output, "w") as f:
            json.dump(report, f, indent=2)

    return report


def export_manifest(output: Path) -> None:
    """Export the manifest to a file."""
    manifest = load_manifest()
    with open(output, "w") as f:
        json.dump(manifest, f, indent=2)


def import_manifest(input_file: Path, merge: bool = False) -> int:
    """Import manifest from a file."""
    with open(input_file) as f:
        imported = json.load(f)

    if not merge:
        # Full replace
        save_manifest(imported)
        return len(imported.get("devices", []))

    # Merge mode - add new devices
    manifest = load_manifest()
    existing_macs = {d["mac"].lower() for d in manifest["devices"]}

    added = 0
    for device in imported.get("devices", []):
        if device["mac"].lower() not in existing_macs:
            manifest["devices"].append(device)
            added += 1

    save_manifest(manifest)
    return added


def cmd_add(args):
    """Handle add command."""
    device = add_device(
        mac=args.mac,
        fingerprint=args.fingerprint,
        chip_id=args.chip_id,
        phase=args.phase,
        location=args.location,
        notes=args.notes,
    )
    print(f"Added device: {device['mac']}")
    if args.json:
        print(json.dumps(device, indent=2))


def cmd_list(args):
    """Handle list command."""
    devices = list_devices(phase=args.phase)

    if args.json:
        print(json.dumps(devices, indent=2))
        return

    if not devices:
        print("No devices found.")
        return

    print()
    print(f"{'MAC':<20} {'Phase':<7} {'Fingerprint':<18} {'Location':<20}")
    print("-" * 70)
    for device in devices:
        mac = device["mac"]
        phase = device.get("phase", "?")
        fp = (device.get("fingerprint") or "")[:16]
        loc = (device.get("location") or "")[:18]
        print(f"{mac:<20} {phase:<7} {fp:<18} {loc:<20}")
    print()
    print(f"Total: {len(devices)} device(s)")


def cmd_search(args):
    """Handle search command."""
    results = search_devices(
        mac=args.mac,
        fingerprint=args.fingerprint,
        location=args.location,
    )

    if args.json:
        print(json.dumps(results, indent=2))
        return

    if not results:
        print("No devices found matching criteria.")
        return

    print(f"Found {len(results)} device(s):")
    for device in results:
        print(json.dumps(device, indent=2))


def cmd_remove(args):
    """Handle remove command."""
    if remove_device(args.mac):
        print(f"Removed device: {args.mac}")
    else:
        print(f"Device not found: {args.mac}")
        sys.exit(1)


def cmd_report(args):
    """Handle report command."""
    report = generate_report(output=args.output)

    if args.output:
        print(f"Report saved to: {args.output}")
    else:
        print(json.dumps(report, indent=2))


def cmd_export(args):
    """Handle export command."""
    export_manifest(args.output)
    print(f"Manifest exported to: {args.output}")


def cmd_import(args):
    """Handle import command."""
    count = import_manifest(args.input, merge=args.merge)
    if args.merge:
        print(f"Imported {count} new device(s)")
    else:
        print(f"Replaced manifest with {count} device(s)")


def main():
    parser = argparse.ArgumentParser(
        description="SecuraCV Canary — Fleet Manifest Management",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    subparsers = parser.add_subparsers(dest="command", help="Commands")

    # Add command
    add_parser = subparsers.add_parser("add", help="Add a device to the manifest")
    add_parser.add_argument("--mac", "-m", required=True, help="Device MAC address")
    add_parser.add_argument("--fingerprint", "-f", help="Device public key fingerprint")
    add_parser.add_argument("--chip-id", help="ESP32 chip ID")
    add_parser.add_argument("--phase", "-p", type=int, default=1, help="Provisioning phase (1 or 2)")
    add_parser.add_argument("--location", "-l", help="Physical location/description")
    add_parser.add_argument("--notes", "-n", help="Additional notes")
    add_parser.add_argument("--json", action="store_true", help="Output as JSON")
    add_parser.set_defaults(func=cmd_add)

    # List command
    list_parser = subparsers.add_parser("list", help="List all devices")
    list_parser.add_argument("--phase", "-p", type=int, help="Filter by phase")
    list_parser.add_argument("--json", action="store_true", help="Output as JSON")
    list_parser.set_defaults(func=cmd_list)

    # Search command
    search_parser = subparsers.add_parser("search", help="Search for devices")
    search_parser.add_argument("--mac", "-m", help="Search by MAC (partial match)")
    search_parser.add_argument("--fingerprint", "-f", help="Search by fingerprint (partial)")
    search_parser.add_argument("--location", "-l", help="Search by location (partial)")
    search_parser.add_argument("--json", action="store_true", help="Output as JSON")
    search_parser.set_defaults(func=cmd_search)

    # Remove command
    remove_parser = subparsers.add_parser("remove", help="Remove a device")
    remove_parser.add_argument("--mac", "-m", required=True, help="Device MAC address")
    remove_parser.set_defaults(func=cmd_remove)

    # Report command
    report_parser = subparsers.add_parser("report", help="Generate fleet report")
    report_parser.add_argument("--output", "-o", type=Path, help="Save report to file")
    report_parser.set_defaults(func=cmd_report)

    # Export command
    export_parser = subparsers.add_parser("export", help="Export manifest")
    export_parser.add_argument("--output", "-o", type=Path, required=True, help="Output file")
    export_parser.set_defaults(func=cmd_export)

    # Import command
    import_parser = subparsers.add_parser("import", help="Import manifest")
    import_parser.add_argument("--input", "-i", type=Path, required=True, help="Input file")
    import_parser.add_argument("--merge", action="store_true", help="Merge with existing (don't replace)")
    import_parser.set_defaults(func=cmd_import)

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(1)

    args.func(args)


if __name__ == "__main__":
    main()
