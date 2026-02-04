#!/usr/bin/env python3
"""
SecuraCV Canary — Device Manifest Manager
ERRERlabs

Utilities for managing the provisioning audit trail:
  - List all provisioned devices
  - Search by MAC address
  - Verify manifest integrity
  - Export fleet summary

Usage:
    python3 create_manifest.py list
    python3 create_manifest.py search --mac aa:bb:cc:dd:ee:ff
    python3 create_manifest.py summary --output fleet_report.json
"""

import argparse
import json
import sys
from pathlib import Path
from datetime import datetime


MANIFEST_DIR = Path(__file__).parent.parent / "logs" / "manifests"


def list_devices():
    """List all provisioned devices."""
    manifests = sorted(MANIFEST_DIR.glob("canary_*.json"))

    if not manifests:
        print("No provisioned devices found.")
        return

    print(f"\n{'MAC Address':<20} {'Chip':<15} {'Date':<22} {'Operator':<12} {'Dry Run'}")
    print("─" * 85)

    for mf in manifests:
        try:
            with open(mf) as f:
                data = json.load(f)
            mac = data.get("device", {}).get("mac_address", "?")
            chip = data.get("device", {}).get("chip_model", "?")
            ts = data.get("provisioning", {}).get("timestamp_utc", "?")
            op = data.get("provisioning", {}).get("operator", "?")
            dry = "YES" if data.get("provisioning", {}).get("dry_run", False) else "no"
            print(f"{mac:<20} {chip:<15} {ts:<22} {op:<12} {dry}")
        except (json.JSONDecodeError, KeyError) as e:
            print(f"  [ERROR reading {mf.name}: {e}]")

    print(f"\nTotal: {len(manifests)} device(s)")


def search_device(mac: str):
    """Search for a device by MAC address."""
    mac_lower = mac.lower().strip()
    found = []

    for mf in MANIFEST_DIR.glob("canary_*.json"):
        try:
            with open(mf) as f:
                data = json.load(f)
            dev_mac = data.get("device", {}).get("mac_address", "").lower()
            if mac_lower in dev_mac or mac_lower.replace(":", "-") in mf.name.lower():
                found.append((mf, data))
        except (json.JSONDecodeError, KeyError):
            continue

    if not found:
        print(f"No device found with MAC matching '{mac}'")
        return

    for mf, data in found:
        print(f"\n{'═' * 60}")
        print(f"  Manifest: {mf.name}")
        print(f"{'═' * 60}")
        print(json.dumps(data, indent=2))


def generate_summary(output_path: str = None):
    """Generate a fleet summary report."""
    manifests = sorted(MANIFEST_DIR.glob("canary_*.json"))

    devices = []
    for mf in manifests:
        try:
            with open(mf) as f:
                data = json.load(f)
            if not data.get("provisioning", {}).get("dry_run", True):
                devices.append({
                    "mac": data.get("device", {}).get("mac_address"),
                    "chip": data.get("device", {}).get("chip_model"),
                    "provisioned_utc": data.get("provisioning", {}).get("timestamp_utc"),
                    "operator": data.get("provisioning", {}).get("operator"),
                    "secure_boot": data.get("security", {}).get("secure_boot"),
                    "flash_encryption": data.get("security", {}).get("flash_encryption"),
                    "bluetooth": "disabled" if not data.get("security", {}).get("bluetooth_compiled") else "enabled",
                    "app_hash": data.get("firmware", {}).get("application_sha256"),
                })
        except (json.JSONDecodeError, KeyError):
            continue

    summary = {
        "report_generated_utc": datetime.utcnow().isoformat() + "Z",
        "product": "SecuraCV Canary",
        "manufacturer": "ERRERlabs",
        "total_devices": len(devices),
        "security_posture": {
            "secure_boot_enabled": sum(1 for d in devices if d.get("secure_boot")),
            "flash_encrypted": sum(1 for d in devices if d.get("flash_encryption")),
            "bluetooth_disabled": sum(1 for d in devices if d.get("bluetooth") == "disabled"),
        },
        "firmware_versions": list(set(
            d.get("app_hash", "unknown")[:16] + "..." for d in devices if d.get("app_hash")
        )),
        "devices": devices,
    }

    if output_path:
        with open(output_path, "w") as f:
            json.dump(summary, f, indent=2)
        print(f"Fleet summary written to {output_path}")
    else:
        print(json.dumps(summary, indent=2))


def main():
    parser = argparse.ArgumentParser(description="SecuraCV Device Manifest Manager")
    sub = parser.add_subparsers(dest="command")

    sub.add_parser("list", help="List all provisioned devices")

    search = sub.add_parser("search", help="Search by MAC address")
    search.add_argument("--mac", "-m", required=True, help="MAC address to search")

    summary = sub.add_parser("summary", help="Generate fleet summary report")
    summary.add_argument("--output", "-o", help="Output file path")

    args = parser.parse_args()

    if args.command == "list":
        list_devices()
    elif args.command == "search":
        search_device(args.mac)
    elif args.command == "summary":
        generate_summary(args.output)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
