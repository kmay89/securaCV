#!/usr/bin/env python3
"""
SecuraCV Canary Serial Monitor

Real-time serial debug monitor for SecuraCV Canary witness devices.
Connects via USB serial (UART), parses firmware log output, and displays
a live-updating TUI dashboard with witness chain stats, device health,
and colorized log output.

Usage:
    python canary_monitor.py                   # auto-detect port
    python canary_monitor.py --port /dev/ttyUSB0
    python canary_monitor.py --port COM3 --baud 115200
    python canary_monitor.py --raw             # raw passthrough, no TUI

Copyright (c) 2026 ERRERlabs / Karl May
License: Apache-2.0
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print(
        "ERROR: pyserial is required.\n"
        "  pip install pyserial\n"
        "  or: pip install -r requirements.txt"
    )
    sys.exit(1)


# ============================================================================
# Constants
# ============================================================================

DEFAULT_BAUD = 115200
KNOWN_USB_SERIAL_CHIPS = {
    # VID:PID pairs for common ESP32 USB-serial bridges
    (0x10C4, 0xEA60): "CP2102/CP2104 (Silicon Labs)",
    (0x1A86, 0x7523): "CH340 (WCH)",
    (0x1A86, 0x55D4): "CH9102 (WCH)",
    (0x0403, 0x6001): "FT232R (FTDI)",
    (0x0403, 0x6010): "FT2232 (FTDI)",
    (0x0403, 0x6015): "FT231X (FTDI)",
    (0x303A, 0x1001): "ESP32-S3 USB-JTAG/Serial",
    (0x303A, 0x0002): "ESP32-S2 USB CDC",
}

# Firmware version from canary_config.h
FIRMWARE_VERSION = "2.1.0"

# Paths
SCRIPT_DIR = Path(__file__).parent
ARTWORK_DIR = SCRIPT_DIR.parent / "artwork"
SPLASH_FILE = ARTWORK_DIR / "splash_serial.txt"

# Log line patterns from firmware log.h and main.cpp
# Firmware uses: [I] TAG: message, [W] TAG: message, [E] TAG: message
# Also direct Serial.printf with [OK], [..], [!!], [WARN], [ERR]
LOG_PATTERN = re.compile(
    r"^\[([IWEDV])\]\s+(\w+):\s+(.*)$"
)
STATUS_PATTERN = re.compile(
    r"^\[(OK|\.\.|\!\!|WARN|ERR)\]\s+(.*)$"
)
# Witness record creation: "Boot attestation: seq=N" or similar
WITNESS_SEQ_PATTERN = re.compile(
    r"seq[=:](\d+)"
)
# Records created: "Records: N (seq: M)"
RECORDS_PATTERN = re.compile(
    r"Records:\s+(\d+)\s+\(seq:\s+(\d+)\)"
)
# Heap info: "Free heap: N bytes"
HEAP_PATTERN = re.compile(
    r"Free heap:\s+(\d+)\s+bytes"
)
# Min heap: "Min heap: N bytes"
MIN_HEAP_PATTERN = re.compile(
    r"Min heap:\s+(\d+)\s+bytes"
)
# Uptime: "Uptime: Ns"
UPTIME_PATTERN = re.compile(
    r"Uptime:\s+(\d+)s"
)
# Device ID: "Device ID: canary-s3-XXXX"
DEVICE_ID_PATTERN = re.compile(
    r"Device ID:\s+(\S+)"
)
# Firmware version from banner
VERSION_PATTERN = re.compile(
    r"Version\s+([\d.]+)"
)
# State line: "State: stationary" or "State: moving"
STATE_PATTERN = re.compile(
    r"State:\s+(\w+)"
)
# GPS fix: "GPS: OK (lat, lon, N sats)"
GPS_PATTERN = re.compile(
    r"GPS:\s+(OK|No fix)"
)
# SD card: "SD: OK" or "SD: Not mounted"
SD_PATTERN = re.compile(
    r"SD:\s+(OK|Not mounted)"
)
# WiFi: "WiFi: OK" or "WiFi: Down"
WIFI_PATTERN = re.compile(
    r"WiFi:\s+(OK|Down)"
)
# Battery: "Battery: N mV (N%)"
BATTERY_PATTERN = re.compile(
    r"Battery:\s+(\d+)\s+mV\s+\((\d+)%\)"
)
# Boot attestation line
BOOT_ATTEST_PATTERN = re.compile(
    r"Boot attestation:\s+seq=(\d+)"
)
# Self-test score
SELFTEST_PATTERN = re.compile(
    r"Self-test:\s+(\d+)%\s+health"
)
# Witness chain events (record creation, signing)
WITNESS_EVENT_PATTERN = re.compile(
    r"(?:witness|chain|record|attestation|tamper)",
    re.IGNORECASE,
)


# ============================================================================
# ANSI Color Codes
# ============================================================================

class Color:
    """ANSI escape codes for terminal coloring."""

    RESET = "\033[0m"
    BOLD = "\033[1m"
    DIM = "\033[2m"
    UNDERLINE = "\033[4m"

    # Foreground
    BLACK = "\033[30m"
    RED = "\033[31m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    BLUE = "\033[34m"
    MAGENTA = "\033[35m"
    CYAN = "\033[36m"
    WHITE = "\033[37m"

    # Bright foreground
    BRIGHT_RED = "\033[91m"
    BRIGHT_GREEN = "\033[92m"
    BRIGHT_YELLOW = "\033[93m"
    BRIGHT_BLUE = "\033[94m"
    BRIGHT_MAGENTA = "\033[95m"
    BRIGHT_CYAN = "\033[96m"
    BRIGHT_WHITE = "\033[97m"

    # Background
    BG_RED = "\033[41m"
    BG_GREEN = "\033[42m"
    BG_YELLOW = "\033[43m"
    BG_BLUE = "\033[44m"
    BG_MAGENTA = "\033[45m"

    # Cursor control
    CLEAR_SCREEN = "\033[2J"
    CURSOR_HOME = "\033[H"
    CLEAR_LINE = "\033[2K"
    CURSOR_UP = "\033[A"
    HIDE_CURSOR = "\033[?25l"
    SHOW_CURSOR = "\033[?25h"

    @staticmethod
    def move_to(row: int, col: int = 1) -> str:
        return f"\033[{row};{col}H"


# Map log levels to colors
LEVEL_COLORS = {
    "I": Color.BRIGHT_GREEN,
    "W": Color.BRIGHT_YELLOW,
    "E": Color.BRIGHT_RED,
    "D": Color.BRIGHT_CYAN,
    "V": Color.DIM,
}

STATUS_COLORS = {
    "OK": Color.BRIGHT_GREEN,
    "..": Color.BRIGHT_CYAN,
    "!!": Color.BRIGHT_RED + Color.BOLD,
    "WARN": Color.BRIGHT_YELLOW,
    "ERR": Color.BRIGHT_RED,
}

LEVEL_NAMES = {
    "I": "INFO",
    "W": "WARN",
    "E": "ERROR",
    "D": "DEBUG",
    "V": "VERBOSE",
}


# ============================================================================
# Data Structures
# ============================================================================

@dataclass
class WitnessEvent:
    """A parsed witness chain event."""

    timestamp: float
    seq: int
    event_type: str
    detail: str


@dataclass
class DeviceHealth:
    """Aggregated device health state parsed from serial output."""

    device_id: str = "unknown"
    firmware_version: str = "unknown"
    uptime_sec: int = 0
    free_heap: int = 0
    min_heap: int = 0
    records_created: int = 0
    chain_seq: int = 0
    state: str = "unknown"
    gps_status: str = "unknown"
    sd_status: str = "unknown"
    wifi_status: str = "unknown"
    battery_mv: int = 0
    battery_pct: int = 0
    selftest_score: int = -1
    boot_attestation_seq: int = -1
    last_update: float = 0.0

    # Subsystem init tracking
    subsystems: dict = field(default_factory=dict)

    # Witness chain event log
    witness_events: deque = field(default_factory=lambda: deque(maxlen=50))

    # Counters
    lines_received: int = 0
    errors_seen: int = 0
    warnings_seen: int = 0


# ============================================================================
# Port Detection
# ============================================================================

def detect_serial_port() -> Optional[str]:
    """Auto-detect an ESP32 USB-serial port.

    Scans available serial ports for known USB-serial chip VID:PID pairs
    commonly used with ESP32 development boards.

    Returns:
        Port device path if found, None otherwise.
    """
    ports = serial.tools.list_ports.comports()
    candidates = []

    for port in ports:
        vid_pid = (port.vid, port.pid) if port.vid and port.pid else None
        if vid_pid and vid_pid in KNOWN_USB_SERIAL_CHIPS:
            chip_name = KNOWN_USB_SERIAL_CHIPS[vid_pid]
            candidates.append((port.device, chip_name, port.description))

    if not candidates:
        # Fall back: look for common device name patterns
        for port in ports:
            dev = port.device.lower()
            if any(
                pattern in dev
                for pattern in ("ttyusb", "ttyacm", "cu.usbserial", "cu.usbmodem", "com")
            ):
                candidates.append((port.device, "Unknown", port.description))

    if not candidates:
        return None

    if len(candidates) == 1:
        port_dev, chip, desc = candidates[0]
        print(f"  Auto-detected: {port_dev} [{chip}] ({desc})")
        return port_dev

    # Multiple candidates: let user pick
    print("  Multiple serial ports detected:")
    for i, (port_dev, chip, desc) in enumerate(candidates, 1):
        print(f"    {i}. {port_dev} [{chip}] ({desc})")

    while True:
        try:
            choice = input(f"  Select port [1-{len(candidates)}]: ").strip()
            idx = int(choice) - 1
            if 0 <= idx < len(candidates):
                return candidates[idx][0]
        except (ValueError, EOFError):
            pass
        print(f"  Please enter a number between 1 and {len(candidates)}")


def list_serial_ports():
    """Print all available serial ports with details."""
    ports = serial.tools.list_ports.comports()
    if not ports:
        print("  No serial ports found.")
        return

    print(f"\n  {'Port':<20} {'VID:PID':<12} {'Chip':<30} {'Description'}")
    print(f"  {'─' * 20} {'─' * 12} {'─' * 30} {'─' * 30}")

    for port in sorted(ports, key=lambda p: p.device):
        vid_pid = (port.vid, port.pid) if port.vid and port.pid else None
        vid_pid_str = f"{port.vid:04X}:{port.pid:04X}" if vid_pid else "----:----"
        chip = KNOWN_USB_SERIAL_CHIPS.get(vid_pid, "Unknown") if vid_pid else ""
        print(f"  {port.device:<20} {vid_pid_str:<12} {chip:<30} {port.description}")
    print()


# ============================================================================
# Splash Screen
# ============================================================================

def print_splash():
    """Display the branded splash screen."""
    if SPLASH_FILE.exists():
        try:
            splash_text = SPLASH_FILE.read_text(encoding="utf-8")
            print(f"{Color.BRIGHT_CYAN}{splash_text}{Color.RESET}")
            return
        except OSError:
            pass

    # Fallback inline splash if artwork file is missing
    print(f"{Color.BRIGHT_CYAN}")
    print("  ╔══════════════════════════════════════════════════════════╗")
    print("  ║         SecuraCV Canary  --  Serial Monitor             ║")
    print("  ║         Privacy Witness Device  |  Ed25519 Chain        ║")
    print("  ╚══════════════════════════════════════════════════════════╝")
    print(f"{Color.RESET}")


# ============================================================================
# Log Parser
# ============================================================================

class LogParser:
    """Parses firmware serial output and updates device health state."""

    def __init__(self, health: DeviceHealth):
        self.health = health

    def parse_line(self, line: str) -> tuple[str, str]:
        """Parse a single log line and update health state.

        Returns:
            Tuple of (colorized_line, raw_line) for display.
        """
        self.health.lines_received += 1
        self.health.last_update = time.time()
        stripped = line.strip()

        if not stripped:
            return "", stripped

        # Check for firmware log format: [I] TAG: message
        match = LOG_PATTERN.match(stripped)
        if match:
            level, tag, msg = match.groups()
            color = LEVEL_COLORS.get(level, Color.RESET)
            level_name = LEVEL_NAMES.get(level, level)

            if level == "E":
                self.health.errors_seen += 1
            elif level == "W":
                self.health.warnings_seen += 1

            self._extract_data(msg)

            colored = (
                f"{Color.DIM}[{Color.RESET}{color}{level_name:<5}{Color.RESET}"
                f"{Color.DIM}]{Color.RESET} "
                f"{Color.BOLD}{Color.BLUE}{tag}{Color.RESET}: "
                f"{color}{msg}{Color.RESET}"
            )
            return colored, stripped

        # Check for status format: [OK] message, [..] message, etc.
        match = STATUS_PATTERN.match(stripped)
        if match:
            status, msg = match.groups()
            color = STATUS_COLORS.get(status, Color.RESET)

            if status in ("!!", "ERR"):
                self.health.errors_seen += 1
            elif status == "WARN":
                self.health.warnings_seen += 1

            self._extract_data(msg)
            self._track_subsystem(status, msg)

            colored = (
                f"{Color.DIM}[{Color.RESET}{color}{status:>4}{Color.RESET}"
                f"{Color.DIM}]{Color.RESET} {msg}"
            )
            return colored, stripped

        # Check for box-drawing banner lines
        if stripped.startswith(("╔", "║", "╠", "╚", "+")):
            colored = f"{Color.BRIGHT_CYAN}{stripped}{Color.RESET}"
            self._extract_data(stripped)
            return colored, stripped

        # Check for section headers: "=== Title ==="
        if stripped.startswith("===") and stripped.endswith("==="):
            colored = (
                f"{Color.BOLD}{Color.BRIGHT_MAGENTA}{stripped}{Color.RESET}"
            )
            return colored, stripped

        # Default: dim passthrough
        self._extract_data(stripped)
        return f"{Color.DIM}{stripped}{Color.RESET}", stripped

    def _extract_data(self, text: str):
        """Extract device health data from log text."""
        h = self.health

        # Device ID
        m = DEVICE_ID_PATTERN.search(text)
        if m:
            h.device_id = m.group(1)

        # Firmware version
        m = VERSION_PATTERN.search(text)
        if m:
            h.firmware_version = m.group(1)

        # Records and sequence
        m = RECORDS_PATTERN.search(text)
        if m:
            h.records_created = int(m.group(1))
            h.chain_seq = int(m.group(2))

        # Free heap
        m = HEAP_PATTERN.search(text)
        if m:
            h.free_heap = int(m.group(1))

        # Min heap
        m = MIN_HEAP_PATTERN.search(text)
        if m:
            h.min_heap = int(m.group(1))

        # Uptime
        m = UPTIME_PATTERN.search(text)
        if m:
            h.uptime_sec = int(m.group(1))

        # State
        m = STATE_PATTERN.search(text)
        if m:
            h.state = m.group(1)

        # GPS
        m = GPS_PATTERN.search(text)
        if m:
            h.gps_status = m.group(1)

        # SD
        m = SD_PATTERN.search(text)
        if m:
            h.sd_status = m.group(1)

        # WiFi
        m = WIFI_PATTERN.search(text)
        if m:
            h.wifi_status = m.group(1)

        # Battery
        m = BATTERY_PATTERN.search(text)
        if m:
            h.battery_mv = int(m.group(1))
            h.battery_pct = int(m.group(2))

        # Boot attestation
        m = BOOT_ATTEST_PATTERN.search(text)
        if m:
            seq = int(m.group(1))
            h.boot_attestation_seq = seq
            h.chain_seq = max(h.chain_seq, seq)
            h.witness_events.append(
                WitnessEvent(
                    timestamp=time.time(),
                    seq=seq,
                    event_type="BOOT_ATTEST",
                    detail="Boot attestation record created",
                )
            )

        # Self-test
        m = SELFTEST_PATTERN.search(text)
        if m:
            h.selftest_score = int(m.group(1))

        # Generic witness sequence extraction
        if WITNESS_EVENT_PATTERN.search(text):
            m = WITNESS_SEQ_PATTERN.search(text)
            if m and not BOOT_ATTEST_PATTERN.search(text):
                seq = int(m.group(1))
                h.chain_seq = max(h.chain_seq, seq)
                h.witness_events.append(
                    WitnessEvent(
                        timestamp=time.time(),
                        seq=seq,
                        event_type="WITNESS",
                        detail=text[:60],
                    )
                )

    def _track_subsystem(self, status: str, msg: str):
        """Track subsystem initialization status."""
        h = self.health
        # Map common subsystem init messages
        subsystem_keywords = {
            "WiFi AP": "wifi",
            "SD card": "sd",
            "GPS": "gps",
            "GNSS": "gps",
            "Camera": "camera",
            "Watchdog": "watchdog",
            "Mesh": "mesh",
            "BLE": "ble",
            "MQTT": "mqtt",
            "CSI": "csi",
            "Acoustic": "audio",
            "Touch": "touch",
            "IR": "ir",
            "Temp": "temp",
            "Vision": "vision",
            "Power monitor": "power",
            "Power policy": "policy",
            "Setup": "setup",
            "Diagnostics": "diag",
            "Data mgmt": "datamgmt",
            "Self-test": "selftest",
            "Sensing witness": "sensing_witness",
        }

        for keyword, name in subsystem_keywords.items():
            if keyword.lower() in msg.lower():
                if status == "OK":
                    h.subsystems[name] = "OK"
                elif status in ("WARN", "ERR", "!!"):
                    h.subsystems[name] = "FAIL"
                elif status == "..":
                    h.subsystems[name] = "INIT"
                break


# ============================================================================
# Dashboard Renderer
# ============================================================================

class Dashboard:
    """Renders a live-updating TUI dashboard using ANSI escape codes."""

    HEADER_LINES = 12
    MIN_LOG_LINES = 5

    def __init__(self, health: DeviceHealth):
        self.health = health
        self.log_lines: deque[str] = deque(maxlen=200)
        self._last_render = 0.0
        self._term_rows = 24
        self._term_cols = 80
        self._refresh_interval = 1.0

    def _get_terminal_size(self):
        """Get current terminal dimensions."""
        try:
            size = os.get_terminal_size()
            self._term_rows = size.lines
            self._term_cols = size.columns
        except OSError:
            pass

    def add_log_line(self, colored_line: str):
        """Add a log line to the scrollback buffer."""
        if colored_line:
            self.log_lines.append(colored_line)

    def render(self, force: bool = False):
        """Render the dashboard if enough time has passed."""
        now = time.time()
        if not force and (now - self._last_render) < self._refresh_interval:
            return
        self._last_render = now
        self._get_terminal_size()

        h = self.health
        cols = self._term_cols
        rows = self._term_rows

        lines: list[str] = []

        # ── Header bar ──
        header = f" SecuraCV Canary Monitor "
        pad = max(0, cols - len(header) - 2)
        lines.append(
            f"{Color.BG_BLUE}{Color.BRIGHT_WHITE}{Color.BOLD}"
            f" {header}{'─' * pad}"
            f"{Color.RESET}"
        )

        # ── Device info row ──
        dev_str = f"Device: {h.device_id}"
        fw_str = f"FW: {h.firmware_version}"
        up_str = f"Uptime: {self._format_uptime(h.uptime_sec)}"
        state_color = (
            Color.BRIGHT_GREEN if h.state in ("stationary", "idle")
            else Color.BRIGHT_YELLOW if h.state == "moving"
            else Color.DIM
        )
        state_str = f"State: {state_color}{h.state}{Color.RESET}"
        lines.append(f"  {dev_str}  |  {fw_str}  |  {up_str}  |  {state_str}")

        # ── Health summary row ──
        heap_color = (
            Color.BRIGHT_GREEN if h.free_heap > 100_000
            else Color.BRIGHT_YELLOW if h.free_heap > 50_000
            else Color.BRIGHT_RED
        )
        heap_str = (
            f"Heap: {heap_color}{h.free_heap:,}{Color.RESET}"
            f" (min: {h.min_heap:,})"
        ) if h.free_heap > 0 else "Heap: --"

        lines.append(f"  {heap_str}  |  Records: {h.records_created}  |  Chain seq: {h.chain_seq}")

        # ── Subsystem status row ──
        sub_parts = []
        for name, status in sorted(h.subsystems.items()):
            if status == "OK":
                sub_parts.append(f"{Color.BRIGHT_GREEN}{name}{Color.RESET}")
            elif status == "FAIL":
                sub_parts.append(f"{Color.BRIGHT_RED}{name}!{Color.RESET}")
            elif status == "INIT":
                sub_parts.append(f"{Color.DIM}{name}..{Color.RESET}")
        if sub_parts:
            lines.append(f"  Subsystems: {' '.join(sub_parts)}")
        else:
            lines.append(f"  Subsystems: {Color.DIM}(waiting for data){Color.RESET}")

        # ── Quick-status indicators ──
        indicators = []
        gps_icon = self._status_icon(h.gps_status, "GPS")
        sd_icon = self._status_icon(h.sd_status, "SD")
        wifi_icon = self._status_icon(h.wifi_status, "WiFi")
        indicators.extend([gps_icon, sd_icon, wifi_icon])

        if h.battery_mv > 0:
            batt_color = (
                Color.BRIGHT_GREEN if h.battery_pct > 50
                else Color.BRIGHT_YELLOW if h.battery_pct > 20
                else Color.BRIGHT_RED
            )
            indicators.append(
                f"{batt_color}BAT:{h.battery_pct}%/{h.battery_mv}mV{Color.RESET}"
            )
        if h.selftest_score >= 0:
            st_color = (
                Color.BRIGHT_GREEN if h.selftest_score >= 90
                else Color.BRIGHT_YELLOW if h.selftest_score >= 70
                else Color.BRIGHT_RED
            )
            indicators.append(
                f"{st_color}Health:{h.selftest_score}%{Color.RESET}"
            )

        stats = (
            f"Lines: {h.lines_received}  "
            f"Errors: {Color.BRIGHT_RED if h.errors_seen else Color.DIM}"
            f"{h.errors_seen}{Color.RESET}  "
            f"Warnings: {Color.BRIGHT_YELLOW if h.warnings_seen else Color.DIM}"
            f"{h.warnings_seen}{Color.RESET}"
        )
        indicators.append(stats)

        lines.append(f"  {' | '.join(indicators)}")

        # ── Witness chain events (last 3) ──
        lines.append(
            f"  {Color.BOLD}Witness Chain:{Color.RESET}"
        )
        if h.witness_events:
            recent = list(h.witness_events)[-3:]
            for evt in recent:
                age = time.time() - evt.timestamp
                age_str = (
                    f"{age:.0f}s ago" if age < 60
                    else f"{age / 60:.0f}m ago"
                )
                lines.append(
                    f"    {Color.CYAN}seq={evt.seq:<6}{Color.RESET} "
                    f"{evt.event_type:<14} "
                    f"{Color.DIM}{age_str:<10}{Color.RESET} "
                    f"{evt.detail[:50]}"
                )
        else:
            lines.append(
                f"    {Color.DIM}(no witness events yet){Color.RESET}"
            )

        # ── Separator ──
        lines.append(
            f"{Color.DIM}{'─' * cols}{Color.RESET}"
        )

        # ── Log output area ──
        header_count = len(lines)
        available_log_lines = max(self.MIN_LOG_LINES, rows - header_count - 1)
        recent_logs = list(self.log_lines)[-available_log_lines:]

        # Build output
        output = [Color.CURSOR_HOME]
        for line in lines:
            output.append(f"{Color.CLEAR_LINE}{line}")
        for log_line in recent_logs:
            output.append(f"{Color.CLEAR_LINE}{log_line}")

        # Fill remaining screen with empty lines
        total_used = header_count + len(recent_logs)
        for _ in range(max(0, rows - total_used - 1)):
            output.append(Color.CLEAR_LINE)

        # Status bar
        status_bar = (
            f" Ctrl+C: exit | "
            f"Port: connected | "
            f"Baud: {DEFAULT_BAUD} | "
            f"{time.strftime('%H:%M:%S')}"
        )
        output.append(
            f"{Color.BG_BLUE}{Color.BRIGHT_WHITE}"
            f"{status_bar:<{cols}}"
            f"{Color.RESET}"
        )

        sys.stdout.write("\n".join(output))
        sys.stdout.flush()

    @staticmethod
    def _format_uptime(seconds: int) -> str:
        """Format uptime seconds to human-readable string."""
        if seconds <= 0:
            return "--"
        days = seconds // 86400
        hours = (seconds % 86400) // 3600
        minutes = (seconds % 3600) // 60
        secs = seconds % 60

        if days > 0:
            return f"{days}d {hours}h {minutes}m"
        if hours > 0:
            return f"{hours}h {minutes}m {secs}s"
        if minutes > 0:
            return f"{minutes}m {secs}s"
        return f"{secs}s"

    @staticmethod
    def _status_icon(status: str, label: str) -> str:
        """Create a colored status icon for a subsystem."""
        if status == "OK":
            return f"{Color.BRIGHT_GREEN}{label}:OK{Color.RESET}"
        if status in ("No fix", "Not mounted", "Down"):
            return f"{Color.BRIGHT_RED}{label}:--{Color.RESET}"
        return f"{Color.DIM}{label}:??{Color.RESET}"


# ============================================================================
# Main Monitor
# ============================================================================

class CanaryMonitor:
    """Main serial monitor controller."""

    def __init__(
        self,
        port: str,
        baud: int = DEFAULT_BAUD,
        raw: bool = False,
    ):
        self.port = port
        self.baud = baud
        self.raw = raw
        self.health = DeviceHealth()
        self.parser = LogParser(self.health)
        self.dashboard = Dashboard(self.health)
        self._serial: Optional[serial.Serial] = None

    def connect(self) -> bool:
        """Open the serial connection."""
        try:
            self._serial = serial.Serial(
                port=self.port,
                baudrate=self.baud,
                timeout=0.1,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
            )
            return True
        except serial.SerialException as e:
            print(f"{Color.BRIGHT_RED}ERROR: Cannot open {self.port}: {e}{Color.RESET}")
            return False

    def disconnect(self):
        """Close the serial connection."""
        if self._serial and self._serial.is_open:
            self._serial.close()

    def run(self):
        """Main monitor loop."""
        if not self.connect():
            return 1

        if not self.raw:
            sys.stdout.write(Color.CLEAR_SCREEN)
            sys.stdout.write(Color.HIDE_CURSOR)
            sys.stdout.flush()

        try:
            self._monitor_loop()
        except KeyboardInterrupt:
            pass
        finally:
            if not self.raw:
                sys.stdout.write(Color.SHOW_CURSOR)
                sys.stdout.write(f"\n{Color.RESET}")
                sys.stdout.flush()
            self.disconnect()
            self._print_summary()

        return 0

    def _monitor_loop(self):
        """Read and process serial data."""
        line_buffer = b""

        while True:
            if not self._serial or not self._serial.is_open:
                print(f"{Color.BRIGHT_RED}Serial port disconnected{Color.RESET}")
                break

            try:
                data = self._serial.read(self._serial.in_waiting or 1)
            except serial.SerialException as e:
                print(f"\n{Color.BRIGHT_RED}Serial error: {e}{Color.RESET}")
                break

            if not data:
                # No data, but refresh dashboard periodically
                if not self.raw:
                    self.dashboard.render()
                continue

            line_buffer += data

            # Process complete lines
            while b"\n" in line_buffer:
                line_bytes, line_buffer = line_buffer.split(b"\n", 1)
                line = line_bytes.decode("utf-8", errors="replace").rstrip("\r")

                if self.raw:
                    print(line, flush=True)
                else:
                    colored, _raw = self.parser.parse_line(line)
                    self.dashboard.add_log_line(colored)
                    self.dashboard.render()

    def _print_summary(self):
        """Print session summary on exit."""
        h = self.health
        print(f"\n{Color.BOLD}Session Summary{Color.RESET}")
        print(f"{'─' * 40}")
        print(f"  Device:          {h.device_id}")
        print(f"  Firmware:        {h.firmware_version}")
        print(f"  Lines received:  {h.lines_received}")
        print(f"  Errors:          {h.errors_seen}")
        print(f"  Warnings:        {h.warnings_seen}")
        print(f"  Records created: {h.records_created}")
        print(f"  Chain height:    {h.chain_seq}")
        if h.witness_events:
            print(f"  Witness events:  {len(h.witness_events)}")
        if h.subsystems:
            ok = sum(1 for s in h.subsystems.values() if s == "OK")
            fail = sum(1 for s in h.subsystems.values() if s == "FAIL")
            print(f"  Subsystems:      {ok} OK, {fail} failed")
        print()


# ============================================================================
# CLI Entry Point
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        prog="canary_monitor",
        description="SecuraCV Canary Serial Monitor - Real-time device debug console",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Examples:\n"
            "  %(prog)s                          Auto-detect port\n"
            "  %(prog)s --port /dev/ttyUSB0      Specify port\n"
            "  %(prog)s --port COM3 --baud 9600  Custom baud rate\n"
            "  %(prog)s --raw                    Raw output mode\n"
            "  %(prog)s --list                   List serial ports\n"
        ),
    )
    parser.add_argument(
        "--port", "-p",
        help="Serial port device (e.g., /dev/ttyUSB0, COM3). Auto-detects if omitted.",
    )
    parser.add_argument(
        "--baud", "-b",
        type=int,
        default=DEFAULT_BAUD,
        help=f"Baud rate (default: {DEFAULT_BAUD})",
    )
    parser.add_argument(
        "--raw", "-r",
        action="store_true",
        help="Raw passthrough mode (no TUI dashboard, just colorized log output)",
    )
    parser.add_argument(
        "--list", "-l",
        action="store_true",
        dest="list_ports",
        help="List available serial ports and exit",
    )
    parser.add_argument(
        "--version", "-V",
        action="version",
        version=f"%(prog)s 1.0.0 (for SecuraCV Canary FW {FIRMWARE_VERSION})",
    )

    args = parser.parse_args()

    # Print splash
    print_splash()

    if args.list_ports:
        list_serial_ports()
        return 0

    # Resolve port
    port = args.port
    if not port:
        print(f"  {Color.BRIGHT_CYAN}Scanning for Canary device...{Color.RESET}")
        port = detect_serial_port()
        if not port:
            print(
                f"\n  {Color.BRIGHT_RED}No ESP32 serial port found.{Color.RESET}\n"
                f"  Use --port to specify manually, or --list to see available ports.\n"
            )
            return 1

    print(
        f"  {Color.BRIGHT_GREEN}Connecting to {port} "
        f"@ {args.baud} baud...{Color.RESET}\n"
    )

    monitor = CanaryMonitor(port=port, baud=args.baud, raw=args.raw)
    return monitor.run()


if __name__ == "__main__":
    sys.exit(main())
