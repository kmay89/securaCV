#!/usr/bin/env python3
"""
SecuraCV Canary — Device Verification (Stage 1)
ERRERlabs

Reads and verifies eFuse state on an ESP32-S3 to ensure:
  1. The device is genuine (chip ID matches ESP32-S3)
  2. eFuses are in virgin/factory-default state (not tampered)
  3. No security features have been pre-burned
  4. Key blocks are empty

Usage:
    python3 verify_device.py --port /dev/ttyUSB0
    python3 verify_device.py --port /dev/ttyUSB0 --post-provision
    python3 verify_device.py --port /dev/ttyUSB0 --json
"""

import argparse
import json
import subprocess
import sys
import re
from datetime import datetime, timezone
from dataclasses import dataclass, field, asdict
from typing import Optional
from pathlib import Path


# ── ANSI Colors ─────────────────────────────────────────────────────────────

class C:
    RED = "\033[0;31m"
    GREEN = "\033[0;32m"
    YELLOW = "\033[1;33m"
    CYAN = "\033[0;36m"
    BOLD = "\033[1m"
    NC = "\033[0m"

def ok(msg):    print(f"{C.GREEN}[PASS]{C.NC} {msg}")
def warn(msg):  print(f"{C.YELLOW}[WARN]{C.NC} {msg}")
def fail(msg):  print(f"{C.RED}[FAIL]{C.NC} {msg}")
def info(msg):  print(f"{C.CYAN}[INFO]{C.NC} {msg}")


# ── Data Structures ─────────────────────────────────────────────────────────

@dataclass
class DeviceIdentity:
    chip_model: str = ""
    chip_revision: str = ""
    chip_id: str = ""
    mac_address: str = ""
    flash_size: str = ""
    crystal_freq: str = ""

@dataclass
class EfuseState:
    """Tracks eFuse values relevant to security provisioning."""
    # Security features
    secure_boot_en: int = -1
    spi_boot_crypt_cnt: int = -1
    dis_download_manual_encrypt: int = -1

    # Debug interfaces
    dis_pad_jtag: int = -1
    dis_usb_jtag: int = -1
    dis_usb_serial_jtag: int = -1
    soft_dis_jtag: int = -1

    # Download mode
    enable_security_download: int = -1
    dis_download_mode: int = -1
    dis_direct_boot: int = -1

    # Key purposes
    key_purpose_0: str = ""
    key_purpose_1: str = ""
    key_purpose_2: str = ""
    key_purpose_3: str = ""
    key_purpose_4: str = ""
    key_purpose_5: str = ""

    # Other security
    dis_usb_device: int = -1
    jtag_sel_enable: int = -1
    secure_boot_key_revoke0: int = -1
    secure_boot_key_revoke1: int = -1
    secure_boot_key_revoke2: int = -1

@dataclass
class VerificationResult:
    timestamp: str = ""
    device: Optional[DeviceIdentity] = None
    efuse: Optional[EfuseState] = None
    checks: list = field(default_factory=list)
    overall_pass: bool = False
    mode: str = "virgin"  # "virgin" or "post_provision"


# ── eFuse Reading ───────────────────────────────────────────────────────────

def run_espefuse(port: str, args: list[str]) -> str:
    """Run espefuse.py and return stdout."""
    cmd = ["espefuse.py", "--port", port, "--chip", "esp32s3"] + args
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=30
        )
        if result.returncode != 0:
            raise RuntimeError(
                f"espefuse.py failed (rc={result.returncode}):\n"
                f"  stdout: {result.stdout}\n"
                f"  stderr: {result.stderr}"
            )
        return result.stdout
    except FileNotFoundError:
        raise RuntimeError(
            "espefuse.py not found. Ensure ESP-IDF is installed and sourced."
        )

def run_esptool(port: str, args: list[str]) -> str:
    """Run esptool.py and return stdout."""
    cmd = ["esptool.py", "--port", port, "--chip", "esp32s3"] + args
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=30
        )
        return result.stdout + result.stderr  # esptool mixes these
    except FileNotFoundError:
        raise RuntimeError(
            "esptool.py not found. Ensure ESP-IDF is installed and sourced."
        )


def read_device_identity(port: str) -> DeviceIdentity:
    """Read chip identity via esptool."""
    dev = DeviceIdentity()
    output = run_esptool(port, ["chip_id"])

    # Parse chip model
    m = re.search(r"Chip is (ESP32-S3[^\s]*)", output)
    if m:
        dev.chip_model = m.group(1)

    # Parse revision
    m = re.search(r"Chip Revision:\s*v?(\S+)", output, re.IGNORECASE)
    if not m:
        m = re.search(r"chip revision:\s*v?(\S+)", output, re.IGNORECASE)
    if m:
        dev.chip_revision = m.group(1)

    # Parse MAC
    m = re.search(r"MAC:\s*([0-9a-f:]{17})", output, re.IGNORECASE)
    if m:
        dev.mac_address = m.group(1)

    # Parse chip ID (from MAC)
    m = re.search(r"Chip ID:\s*0x([0-9a-f]+)", output, re.IGNORECASE)
    if m:
        dev.chip_id = f"0x{m.group(1)}"

    # Flash size
    output2 = run_esptool(port, ["flash_id"])
    m = re.search(r"Detected flash size:\s*(\S+)", output2)
    if m:
        dev.flash_size = m.group(1)

    # Crystal
    m = re.search(r"Crystal is\s*(\S+)", output)
    if m:
        dev.crystal_freq = m.group(1)

    return dev


def read_efuse_state(port: str) -> EfuseState:
    """Read security-relevant eFuses via espefuse summary."""
    state = EfuseState()
    output = run_espefuse(port, ["summary", "--format", "value_only"])

    # Also get the full summary for parsing
    full_output = run_espefuse(port, ["summary"])

    def find_efuse_value(name: str, text: str) -> str:
        """Extract eFuse value from summary output."""
        # Try "NAME = VALUE" pattern
        patterns = [
            rf"{name}\s*=\s*(\S+)",
            rf"{name}\s+\(.*?\)\s*=\s*(\S+)",
            rf"(?:^|\n)\s*{name}\s.*?=\s*(\S+)",
        ]
        for pat in patterns:
            m = re.search(pat, text, re.IGNORECASE | re.MULTILINE)
            if m:
                return m.group(1).strip()
        return ""

    def parse_int(val: str) -> int:
        """Parse an eFuse integer value."""
        if not val:
            return -1
        val = val.strip().lower()
        if val.startswith("0x"):
            return int(val, 16)
        if val.startswith("0b"):
            return int(val, 2)
        try:
            return int(val)
        except ValueError:
            return -1

    # Map eFuse names to state fields
    int_fields = {
        "SECURE_BOOT_EN": "secure_boot_en",
        "SPI_BOOT_CRYPT_CNT": "spi_boot_crypt_cnt",
        "DIS_DOWNLOAD_MANUAL_ENCRYPT": "dis_download_manual_encrypt",
        "DIS_PAD_JTAG": "dis_pad_jtag",
        "DIS_USB_JTAG": "dis_usb_jtag",
        "DIS_USB_SERIAL_JTAG": "dis_usb_serial_jtag",
        "SOFT_DIS_JTAG": "soft_dis_jtag",
        "ENABLE_SECURITY_DOWNLOAD": "enable_security_download",
        "DIS_DOWNLOAD_MODE": "dis_download_mode",
        "DIS_DIRECT_BOOT": "dis_direct_boot",
        "DIS_USB_DEVICE": "dis_usb_device",
        "JTAG_SEL_ENABLE": "jtag_sel_enable",
        "SECURE_BOOT_KEY_REVOKE0": "secure_boot_key_revoke0",
        "SECURE_BOOT_KEY_REVOKE1": "secure_boot_key_revoke1",
        "SECURE_BOOT_KEY_REVOKE2": "secure_boot_key_revoke2",
    }

    str_fields = {
        "KEY_PURPOSE_0": "key_purpose_0",
        "KEY_PURPOSE_1": "key_purpose_1",
        "KEY_PURPOSE_2": "key_purpose_2",
        "KEY_PURPOSE_3": "key_purpose_3",
        "KEY_PURPOSE_4": "key_purpose_4",
        "KEY_PURPOSE_5": "key_purpose_5",
    }

    for efuse_name, field_name in int_fields.items():
        val = find_efuse_value(efuse_name, full_output)
        setattr(state, field_name, parse_int(val))

    for efuse_name, field_name in str_fields.items():
        val = find_efuse_value(efuse_name, full_output)
        setattr(state, field_name, val if val else "USER/EMPTY")

    return state


# ── Verification Checks ────────────────────────────────────────────────────

def check(results: list, name: str, passed: bool, detail: str = ""):
    """Record a check result."""
    results.append({
        "name": name,
        "passed": passed,
        "detail": detail,
    })
    if passed:
        ok(f"{name}" + (f" — {detail}" if detail else ""))
    else:
        fail(f"{name}" + (f" — {detail}" if detail else ""))


def verify_virgin(dev: DeviceIdentity, efuse: EfuseState) -> list:
    """Verify device is in factory-default (virgin) state."""
    checks = []

    info("── Chip Identity ──")
    check(checks, "Chip is ESP32-S3",
          "ESP32-S3" in dev.chip_model,
          f"Detected: {dev.chip_model}")

    check(checks, "MAC address present",
          len(dev.mac_address) == 17,
          dev.mac_address)

    info("")
    info("── Security eFuses (must be 0 / disabled) ──")

    check(checks, "Secure Boot NOT enabled",
          efuse.secure_boot_en == 0,
          f"SECURE_BOOT_EN = {efuse.secure_boot_en}")

    check(checks, "Flash Encryption NOT enabled",
          efuse.spi_boot_crypt_cnt == 0,
          f"SPI_BOOT_CRYPT_CNT = {efuse.spi_boot_crypt_cnt}")

    check(checks, "Manual encrypt download NOT disabled",
          efuse.dis_download_manual_encrypt == 0,
          f"DIS_DOWNLOAD_MANUAL_ENCRYPT = {efuse.dis_download_manual_encrypt}")

    info("")
    info("── Debug Interfaces (must be 0 / not yet disabled) ──")

    check(checks, "Pad JTAG NOT disabled",
          efuse.dis_pad_jtag == 0,
          f"DIS_PAD_JTAG = {efuse.dis_pad_jtag}")

    check(checks, "USB JTAG NOT disabled",
          efuse.dis_usb_jtag == 0,
          f"DIS_USB_JTAG = {efuse.dis_usb_jtag}")

    check(checks, "USB Serial JTAG NOT disabled",
          efuse.dis_usb_serial_jtag == 0,
          f"DIS_USB_SERIAL_JTAG = {efuse.dis_usb_serial_jtag}")

    info("")
    info("── Key Blocks (must be empty / unassigned) ──")

    for i in range(6):
        purpose = getattr(efuse, f"key_purpose_{i}", "")
        is_empty = purpose in ("", "USER", "USER/EMPTY", "0")
        check(checks, f"KEY_PURPOSE_{i} unassigned",
              is_empty,
              f"KEY_PURPOSE_{i} = {purpose}")

    info("")
    info("── Download Mode ──")

    check(checks, "Security download NOT enabled yet",
          efuse.enable_security_download == 0,
          f"ENABLE_SECURITY_DOWNLOAD = {efuse.enable_security_download}")

    check(checks, "Download mode NOT disabled",
          efuse.dis_download_mode == 0,
          f"DIS_DOWNLOAD_MODE = {efuse.dis_download_mode}")

    return checks


def verify_post_provision(dev: DeviceIdentity, efuse: EfuseState) -> list:
    """Verify device has been correctly provisioned."""
    checks = []

    info("── Chip Identity ──")
    check(checks, "Chip is ESP32-S3",
          "ESP32-S3" in dev.chip_model,
          f"Detected: {dev.chip_model}")

    info("")
    info("── Security Features (must be ENABLED) ──")

    check(checks, "Secure Boot ENABLED",
          efuse.secure_boot_en == 1,
          f"SECURE_BOOT_EN = {efuse.secure_boot_en}")

    check(checks, "Flash Encryption ENABLED (release)",
          efuse.spi_boot_crypt_cnt == 7 or efuse.spi_boot_crypt_cnt == 0x7,
          f"SPI_BOOT_CRYPT_CNT = {efuse.spi_boot_crypt_cnt} (want 7)")

    check(checks, "Manual encrypt download DISABLED",
          efuse.dis_download_manual_encrypt == 1,
          f"DIS_DOWNLOAD_MANUAL_ENCRYPT = {efuse.dis_download_manual_encrypt}")

    info("")
    info("── Debug Interfaces (must be DISABLED) ──")

    check(checks, "Pad JTAG DISABLED",
          efuse.dis_pad_jtag == 1,
          f"DIS_PAD_JTAG = {efuse.dis_pad_jtag}")

    check(checks, "USB JTAG DISABLED",
          efuse.dis_usb_jtag == 1,
          f"DIS_USB_JTAG = {efuse.dis_usb_jtag}")

    info("")
    info("── Key Purposes (must be assigned) ──")

    # Key 0 should be flash encryption
    fe_purposes = ("XTS_AES_128_KEY", "XTS_AES_256_KEY_1", "XTS_AES_256_KEY_2")
    check(checks, "KEY_PURPOSE_0 = Flash Encryption",
          any(p in efuse.key_purpose_0 for p in fe_purposes),
          f"KEY_PURPOSE_0 = {efuse.key_purpose_0}")

    # At least one key should be secure boot digest
    sb_found = False
    for i in range(6):
        purpose = getattr(efuse, f"key_purpose_{i}", "")
        if "SECURE_BOOT_DIGEST" in purpose:
            sb_found = True
            check(checks, f"KEY_PURPOSE_{i} = Secure Boot Digest",
                  True, f"KEY_PURPOSE_{i} = {purpose}")
    if not sb_found:
        check(checks, "Secure Boot Digest in key blocks",
              False, "No SECURE_BOOT_DIGEST found in any key purpose")

    info("")
    info("── Download Mode Security ──")

    check(checks, "Security download ENABLED",
          efuse.enable_security_download == 1,
          f"ENABLE_SECURITY_DOWNLOAD = {efuse.enable_security_download}")

    return checks


# ── Main ────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="SecuraCV Canary — Device Verification"
    )
    parser.add_argument(
        "--port", "-p", required=True,
        help="Serial port (e.g., /dev/ttyUSB0, /dev/ttyACM0, COM3)"
    )
    parser.add_argument(
        "--post-provision", action="store_true",
        help="Verify post-provisioning state instead of virgin state"
    )
    parser.add_argument(
        "--json", action="store_true",
        help="Output results as JSON"
    )
    parser.add_argument(
        "--output", "-o", type=str, default=None,
        help="Write JSON results to file"
    )
    args = parser.parse_args()

    result = VerificationResult()
    result.timestamp = datetime.now(timezone.utc).isoformat()
    result.mode = "post_provision" if args.post_provision else "virgin"

    print("")
    print(f"{C.BOLD}╔══════════════════════════════════════════════════╗{C.NC}")
    print(f"{C.BOLD}║  SecuraCV Canary — Device Verification           ║{C.NC}")
    print(f"{C.BOLD}║  Mode: {'Post-Provision' if args.post_provision else 'Virgin State':<40s}║{C.NC}")
    print(f"{C.BOLD}╚══════════════════════════════════════════════════╝{C.NC}")
    print("")

    # ── Read Device ─────────────────────────────────────────────────────
    info("Reading device identity...")
    try:
        result.device = read_device_identity(args.port)
    except Exception as e:
        fail(f"Cannot read device: {e}")
        sys.exit(2)

    info(f"  Chip:     {result.device.chip_model}")
    info(f"  Revision: {result.device.chip_revision}")
    info(f"  MAC:      {result.device.mac_address}")
    info(f"  Flash:    {result.device.flash_size}")
    print("")

    # ── Read eFuses ─────────────────────────────────────────────────────
    info("Reading eFuse state...")
    try:
        result.efuse = read_efuse_state(args.port)
    except Exception as e:
        fail(f"Cannot read eFuses: {e}")
        sys.exit(2)
    print("")

    # ── Run Checks ──────────────────────────────────────────────────────
    if args.post_provision:
        result.checks = verify_post_provision(result.device, result.efuse)
    else:
        result.checks = verify_virgin(result.device, result.efuse)

    # ── Summary ─────────────────────────────────────────────────────────
    passed = sum(1 for c in result.checks if c["passed"])
    total = len(result.checks)
    result.overall_pass = (passed == total)

    print("")
    if result.overall_pass:
        print(f"{C.GREEN}{C.BOLD}══════════════════════════════════════════════════{C.NC}")
        print(f"{C.GREEN}{C.BOLD}  ALL CHECKS PASSED ({passed}/{total}){C.NC}")
        if not args.post_provision:
            print(f"{C.GREEN}  Device is in virgin state — safe to provision.{C.NC}")
        else:
            print(f"{C.GREEN}  Device is correctly provisioned and hardened.{C.NC}")
        print(f"{C.GREEN}{C.BOLD}══════════════════════════════════════════════════{C.NC}")
    else:
        failed = [c for c in result.checks if not c["passed"]]
        print(f"{C.RED}{C.BOLD}══════════════════════════════════════════════════{C.NC}")
        print(f"{C.RED}{C.BOLD}  VERIFICATION FAILED ({passed}/{total} passed){C.NC}")
        print(f"{C.RED}  {total - passed} check(s) failed:{C.NC}")
        for c in failed:
            print(f"{C.RED}    ✗ {c['name']}: {c['detail']}{C.NC}")
        if not args.post_provision:
            print(f"{C.RED}")
            print(f"  ⚠  DO NOT PROVISION THIS DEVICE  ⚠")
            print(f"  eFuses have been pre-burned, indicating the device")
            print(f"  may have been tampered with or is not factory-new.")
            print(f"  Quarantine this device and the entire batch.{C.NC}")
        print(f"{C.RED}{C.BOLD}══════════════════════════════════════════════════{C.NC}")

    # ── Output ──────────────────────────────────────────────────────────
    if args.json or args.output:
        json_data = {
            "timestamp": result.timestamp,
            "mode": result.mode,
            "overall_pass": result.overall_pass,
            "device": asdict(result.device) if result.device else None,
            "efuse": asdict(result.efuse) if result.efuse else None,
            "checks": result.checks,
        }
        if args.output:
            Path(args.output).parent.mkdir(parents=True, exist_ok=True)
            with open(args.output, "w") as f:
                json.dump(json_data, f, indent=2)
            info(f"Results written to {args.output}")
        if args.json:
            print(json.dumps(json_data, indent=2))

    sys.exit(0 if result.overall_pass else 1)


if __name__ == "__main__":
    main()
