#!/usr/bin/env python3
"""
SecuraCV Canary — Device eFuse Verification Tool

Reads security-relevant eFuses via espefuse.py, checks virgin state
pre-provisioning and correct lockdown post-provisioning, outputs JSON
for audit trail.

Usage:
    python verify_device.py --port /dev/ttyACM0 --expect-virgin
    python verify_device.py --port /dev/ttyACM0 --expect-locked
    python verify_device.py --port /dev/ttyACM0 --json
    python verify_device.py --help

Output:
    JSON report with all security-relevant eFuse values and validation status.
"""

import argparse
import json
import subprocess
import sys
import re
from datetime import datetime
from pathlib import Path
from typing import Optional


# Security-relevant eFuses for ESP32-S3
SECURITY_EFUSES = {
    # Secure Boot
    "SECURE_BOOT_EN": {
        "virgin": 0,
        "locked": 1,
        "description": "Enables Secure Boot v2",
    },
    "SECURE_BOOT_AGGRESSIVE_REVOKE": {
        "virgin": 0,
        "locked": 1,
        "description": "Aggressive key revocation mode",
    },
    # Flash Encryption
    "SPI_BOOT_CRYPT_CNT": {
        "virgin": 0,
        "locked": lambda v: v in [1, 3, 5, 7],  # Odd values = encrypted
        "description": "Flash encryption enable counter",
    },
    "DIS_DOWNLOAD_MODE": {
        "virgin": 0,
        "locked": 1,
        "description": "Disable ROM download mode",
    },
    # JTAG
    "JTAG_SEL_ENABLE": {
        "virgin": 0,
        "locked": 0,
        "description": "JTAG selection (should stay 0)",
    },
    "SOFT_DIS_JTAG": {
        "virgin": 0,
        "locked": 7,  # All 3 bits set
        "description": "Software JTAG disable",
    },
    "DIS_PAD_JTAG": {
        "virgin": 0,
        "locked": 1,
        "description": "Disable pad JTAG",
    },
    "DIS_USB_JTAG": {
        "virgin": 0,
        "locked": 1,
        "description": "Disable USB JTAG",
    },
    # Key protection
    "DIS_DOWNLOAD_MANUAL_ENCRYPT": {
        "virgin": 0,
        "locked": 1,
        "description": "Disable manual flash encryption in download mode",
    },
    # Anti-rollback (optional)
    "SECURE_VERSION": {
        "virgin": 0,
        "locked": lambda v: v >= 0,  # Any value is valid when locked
        "description": "Secure version for anti-rollback",
    },
}


def run_espefuse(port: str, command: list[str]) -> tuple[int, str, str]:
    """Run espefuse.py with given command and return (returncode, stdout, stderr)."""
    cmd = ["python3", "-m", "espefuse", "--port", port] + command
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=30,
        )
        return result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return 1, "", "Command timed out"
    except FileNotFoundError:
        return 1, "", "espefuse not found. Install with: pip install esptool"


def get_efuse_summary(port: str) -> Optional[dict]:
    """Get eFuse summary from device."""
    returncode, stdout, stderr = run_espefuse(port, ["summary", "--format", "json"])

    if returncode != 0:
        print(f"Error reading eFuses: {stderr}", file=sys.stderr)
        return None

    # espefuse outputs JSON to stdout
    try:
        # Find JSON in output (may have other text before/after)
        json_match = re.search(r'\{.*\}', stdout, re.DOTALL)
        if json_match:
            return json.loads(json_match.group())

        # Try parsing the whole output
        return json.loads(stdout)
    except json.JSONDecodeError:
        # Fallback: parse text output
        return parse_text_summary(stdout)


def parse_text_summary(text: str) -> dict:
    """Parse text-format espefuse summary into dict."""
    efuses = {}

    for line in text.split('\n'):
        # Match lines like: "SECURE_BOOT_EN (BLOCK0)   = False"
        match = re.match(r'\s*(\w+)\s+\([^)]+\)\s*=\s*(.+)', line)
        if match:
            name = match.group(1)
            value_str = match.group(2).strip()

            # Convert to appropriate type
            if value_str.lower() in ('true', 'false'):
                value = 1 if value_str.lower() == 'true' else 0
            elif value_str.isdigit():
                value = int(value_str)
            elif value_str.startswith('0x'):
                value = int(value_str, 16)
            else:
                value = value_str

            efuses[name] = {"value": value}

    return efuses


def get_chip_info(port: str) -> dict:
    """Get chip information."""
    returncode, stdout, stderr = run_espefuse(port, ["chip_id"])

    info = {
        "chip_id": None,
        "mac": None,
    }

    if returncode == 0:
        # Parse chip ID from output
        for line in stdout.split('\n'):
            if 'Chip ID' in line:
                match = re.search(r'0x([0-9a-fA-F]+)', line)
                if match:
                    info["chip_id"] = match.group(1)
            if 'MAC' in line:
                match = re.search(r'([0-9a-fA-F:]{17})', line)
                if match:
                    info["mac"] = match.group(1)

    return info


def verify_efuses(efuse_data: dict, expect_mode: str) -> dict:
    """Verify eFuse values against expected state."""
    results = {
        "mode": expect_mode,
        "passed": True,
        "checks": [],
    }

    for efuse_name, spec in SECURITY_EFUSES.items():
        check = {
            "efuse": efuse_name,
            "description": spec["description"],
            "expected": None,
            "actual": None,
            "passed": False,
        }

        # Get actual value
        if efuse_name in efuse_data:
            actual = efuse_data[efuse_name]
            if isinstance(actual, dict):
                actual = actual.get("value", actual)
            check["actual"] = actual
        else:
            check["actual"] = "NOT_FOUND"
            check["passed"] = False
            results["passed"] = False
            results["checks"].append(check)
            continue

        # Get expected value based on mode
        if expect_mode == "virgin":
            expected = spec["virgin"]
        elif expect_mode == "locked":
            expected = spec["locked"]
        else:
            expected = None

        check["expected"] = expected if not callable(expected) else f"<validator>"

        # Validate
        if callable(expected):
            check["passed"] = expected(actual)
        else:
            check["passed"] = (actual == expected)

        if not check["passed"]:
            results["passed"] = False

        results["checks"].append(check)

    return results


def generate_report(
    port: str,
    expect_mode: Optional[str] = None,
) -> dict:
    """Generate full device verification report."""
    report = {
        "timestamp": datetime.now().isoformat(),
        "tool_version": "1.0.0",
        "port": port,
        "chip_info": {},
        "efuse_summary": {},
        "verification": None,
        "status": "unknown",
    }

    # Get chip info
    chip_info = get_chip_info(port)
    report["chip_info"] = chip_info

    # Get eFuse summary
    efuse_data = get_efuse_summary(port)
    if efuse_data is None:
        report["status"] = "error"
        report["error"] = "Failed to read eFuses"
        return report

    report["efuse_summary"] = efuse_data

    # Verify if mode specified
    if expect_mode:
        verification = verify_efuses(efuse_data, expect_mode)
        report["verification"] = verification
        report["status"] = "pass" if verification["passed"] else "fail"
    else:
        report["status"] = "info"

    return report


def main():
    parser = argparse.ArgumentParser(
        description="SecuraCV Canary — Device eFuse Verification Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --port /dev/ttyACM0 --expect-virgin
      Verify device is in virgin state (ready for provisioning)

  %(prog)s --port /dev/ttyACM0 --expect-locked
      Verify device has security features enabled

  %(prog)s --port /dev/ttyACM0 --json
      Output raw eFuse data as JSON (no verification)

  %(prog)s --port /dev/ttyACM0 --output report.json
      Save report to file
""",
    )

    parser.add_argument(
        "--port", "-p",
        required=True,
        help="Serial port (e.g., /dev/ttyACM0, COM3)",
    )
    parser.add_argument(
        "--expect-virgin",
        action="store_true",
        help="Verify device is in virgin state (no security eFuses burned)",
    )
    parser.add_argument(
        "--expect-locked",
        action="store_true",
        help="Verify device has security features enabled",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output as JSON",
    )
    parser.add_argument(
        "--output", "-o",
        type=Path,
        help="Save report to file",
    )
    parser.add_argument(
        "--quiet", "-q",
        action="store_true",
        help="Only output errors",
    )

    args = parser.parse_args()

    # Determine verification mode
    expect_mode = None
    if args.expect_virgin:
        expect_mode = "virgin"
    elif args.expect_locked:
        expect_mode = "locked"

    # Generate report
    report = generate_report(args.port, expect_mode)

    # Output
    if args.json or args.output:
        json_output = json.dumps(report, indent=2)

        if args.output:
            args.output.write_text(json_output)
            if not args.quiet:
                print(f"Report saved to {args.output}")
        else:
            print(json_output)
    else:
        # Human-readable output
        print()
        print("═" * 60)
        print(" SecuraCV Canary — Device Verification Report")
        print("═" * 60)
        print()
        print(f"Timestamp: {report['timestamp']}")
        print(f"Port:      {report['port']}")
        print(f"Chip ID:   {report['chip_info'].get('chip_id', 'Unknown')}")
        print(f"MAC:       {report['chip_info'].get('mac', 'Unknown')}")
        print()

        if report["verification"]:
            mode = report["verification"]["mode"]
            passed = report["verification"]["passed"]
            status_str = "PASS" if passed else "FAIL"
            status_color = "\033[92m" if passed else "\033[91m"
            reset_color = "\033[0m"

            print(f"Verification Mode: {mode}")
            print(f"Status: {status_color}{status_str}{reset_color}")
            print()
            print("eFuse Checks:")
            print("-" * 60)

            for check in report["verification"]["checks"]:
                check_status = "OK" if check["passed"] else "FAIL"
                print(f"  {check['efuse']:30} {check_status:6} "
                      f"(expected={check['expected']}, actual={check['actual']})")
        else:
            print("Security eFuse Summary:")
            print("-" * 60)
            for name, spec in SECURITY_EFUSES.items():
                if name in report["efuse_summary"]:
                    value = report["efuse_summary"][name]
                    if isinstance(value, dict):
                        value = value.get("value", value)
                    print(f"  {name:30} = {value}")

        print()

    # Exit code
    if report["status"] == "fail":
        sys.exit(1)
    elif report["status"] == "error":
        sys.exit(2)
    else:
        sys.exit(0)


if __name__ == "__main__":
    main()
