#!/usr/bin/env python3
"""
SecuraCV Canary — OTA Auto-Deploy Script (Cross-platform)

Watches for new firmware builds and automatically deploys via OTA.
Uses watchdog for cross-platform file system events.

Usage:
    python ota_deploy.py [device_ip]
    python ota_deploy.py --watch 192.168.4.1
    CANARY_IP=192.168.4.1 python ota_deploy.py

Requirements:
    pip install requests watchdog

Copyright (c) 2026 ERRERlabs / Karl May
License: Apache-2.0
"""

import argparse
import os
import sys
import time
from pathlib import Path

try:
    import requests
except ImportError:
    print("Error: requests library required. Install with: pip install requests")
    sys.exit(1)

# Configuration
DEFAULT_IP = "192.168.4.1"
DEFAULT_PORT = 80
OTA_ENDPOINT = "/api/ota"
BUILD_DIR = ".pio/build"
FIRMWARE_NAME = "firmware.bin"

# ANSI colors
class Colors:
    RED = "\033[0;31m"
    GREEN = "\033[0;32m"
    YELLOW = "\033[1;33m"
    BLUE = "\033[0;34m"
    NC = "\033[0m"


def log_info(msg: str) -> None:
    print(f"{Colors.BLUE}[INFO]{Colors.NC} {msg}")


def log_success(msg: str) -> None:
    print(f"{Colors.GREEN}[OK]{Colors.NC} {msg}")


def log_warn(msg: str) -> None:
    print(f"{Colors.YELLOW}[WARN]{Colors.NC} {msg}")


def log_error(msg: str) -> None:
    print(f"{Colors.RED}[ERROR]{Colors.NC} {msg}")


def find_firmware(build_dir: str) -> Path | None:
    """Find the most recently modified firmware binary."""
    build_path = Path(build_dir)
    if not build_path.exists():
        return None

    latest = None
    latest_time = 0

    for env_dir in build_path.iterdir():
        if env_dir.is_dir():
            fw_path = env_dir / FIRMWARE_NAME
            if fw_path.exists():
                mtime = fw_path.stat().st_mtime
                if mtime > latest_time:
                    latest_time = mtime
                    latest = fw_path

    return latest


def deploy_firmware(firmware_path: Path, ip: str, port: int) -> bool:
    """Deploy firmware to device via OTA."""
    url = f"http://{ip}:{port}{OTA_ENDPOINT}"
    size = firmware_path.stat().st_size

    log_info(f"Deploying: {firmware_path} ({size} bytes)")
    log_info(f"Target: {url}")

    try:
        with open(firmware_path, "rb") as f:
            firmware_data = f.read()

        response = requests.post(
            url,
            data=firmware_data,
            headers={"Content-Type": "application/octet-stream"},
            timeout=120,
        )

        if response.status_code == 200:
            log_success("OTA update successful!")
            log_info(f"Response: {response.text}")
            log_info("Device will reboot...")
            return True
        else:
            log_error(f"OTA update failed (HTTP {response.status_code})")
            log_error(f"Response: {response.text}")
            return False

    except requests.exceptions.ConnectionError:
        log_error(f"Cannot connect to device at {ip}:{port}")
        log_error("Make sure you're connected to the Canary WiFi network")
        return False
    except requests.exceptions.Timeout:
        log_error("Connection timed out")
        return False
    except Exception as e:
        log_error(f"Unexpected error: {e}")
        return False


def check_device(ip: str, port: int) -> bool:
    """Check if device is reachable."""
    log_info(f"Checking device at {ip}:{port}...")

    try:
        response = requests.get(
            f"http://{ip}:{port}/api/status",
            timeout=5,
        )
        if response.status_code == 200:
            log_success("Device is reachable")
            return True
    except requests.exceptions.RequestException:
        pass

    log_warn("Device status endpoint not responding")
    log_warn("Continuing anyway (device may still accept OTA)")
    return True


def watch_and_deploy(build_dir: str, ip: str, port: int) -> None:
    """Watch for firmware changes and deploy automatically."""
    try:
        from watchdog.observers import Observer
        from watchdog.events import FileSystemEventHandler, FileModifiedEvent
    except ImportError:
        log_error("watchdog library required for watch mode")
        log_error("Install with: pip install watchdog")
        sys.exit(1)

    class FirmwareHandler(FileSystemEventHandler):
        def __init__(self):
            self.last_deployed = None
            self.debounce_time = 0

        def on_modified(self, event):
            if isinstance(event, FileModifiedEvent):
                if event.src_path.endswith(".bin"):
                    # Debounce to avoid multiple triggers
                    now = time.time()
                    if now - self.debounce_time < 2:
                        return
                    self.debounce_time = now

                    # Wait for build to complete
                    time.sleep(1)

                    firmware = find_firmware(build_dir)
                    if firmware and str(firmware) != self.last_deployed:
                        print()
                        log_info("New firmware detected!")
                        if deploy_firmware(firmware, ip, port):
                            self.last_deployed = str(firmware)
                        print()

    build_path = Path(build_dir)
    if not build_path.exists():
        log_error(f"Build directory not found: {build_dir}")
        log_info("Run 'pio run' to build first")
        sys.exit(1)

    log_info(f"Watching for firmware changes in: {build_dir}")
    log_info("Press Ctrl+C to stop")
    print()

    handler = FirmwareHandler()
    observer = Observer()
    observer.schedule(handler, str(build_path), recursive=True)
    observer.start()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        observer.stop()
        print()
        log_info("Stopped watching")

    observer.join()


def single_deploy(build_dir: str, ip: str, port: int) -> None:
    """Deploy the most recent firmware once."""
    firmware = find_firmware(build_dir)

    if firmware is None:
        log_error(f"No firmware found in {build_dir}")
        log_info("Run 'pio run' to build first")
        sys.exit(1)

    check_device(ip, port)
    success = deploy_firmware(firmware, ip, port)
    sys.exit(0 if success else 1)


def print_banner() -> None:
    print()
    print("╔══════════════════════════════════════════════════════════════╗")
    print("║       SecuraCV Canary — OTA Auto-Deploy                      ║")
    print("╚══════════════════════════════════════════════════════════════╝")
    print()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Deploy SecuraCV Canary firmware via OTA"
    )
    parser.add_argument(
        "ip",
        nargs="?",
        default=os.environ.get("CANARY_IP", DEFAULT_IP),
        help=f"Device IP address (default: {DEFAULT_IP})",
    )
    parser.add_argument(
        "--port",
        "-p",
        type=int,
        default=int(os.environ.get("CANARY_PORT", DEFAULT_PORT)),
        help=f"HTTP port (default: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--watch",
        "-w",
        action="store_true",
        help="Watch for changes and auto-deploy",
    )
    parser.add_argument(
        "--build-dir",
        "-b",
        default=os.environ.get("BUILD_DIR", BUILD_DIR),
        help=f"PlatformIO build directory (default: {BUILD_DIR})",
    )

    args = parser.parse_args()

    print_banner()

    if args.watch:
        watch_and_deploy(args.build_dir, args.ip, args.port)
    else:
        single_deploy(args.build_dir, args.ip, args.port)


if __name__ == "__main__":
    main()
