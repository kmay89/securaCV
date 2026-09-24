#!/usr/bin/env python3
"""
SecuraCV Canary — OTA Auto-Deploy Script (Cross-platform)

Pushes a development build to a Canary's dev-only `POST /api/ota` endpoint
(compiled into dev builds only; release images have no such route), once or
every time PlatformIO writes a new firmware.bin. Watch mode uses watchdog for
cross-platform file system events.

The endpoint is behind the device's API bearer token, so the script needs it:
    CANARY_TOKEN   the `token` from the Canary's recovery kit
                   (canary-recovery-kit.json). Read from the environment,
                   or asked for at the terminal without echo. Never taken as
                   a command-line argument (argv is visible to every process
                   on the machine), and never printed.
A dev build serves its API over self-signed HTTPS on 443 once setup is done
(port 80 then only redirects). Such a device is spoken to over HTTPS only,
and only when its certificate matches a pin:
    CANARY_TLS_FP  the device's `tls_cert_fp` (SHA-256 of its certificate,
                   64 hex digits; colons allowed) — from the recovery kit,
                   or from /api/status. With it set, the script never falls
                   back to plain HTTP.
Without a pin, the script looks for TLS on 443 first. A device that answers
there is refused (its certificate's fingerprint is printed so it can be
compared with the recovery kit); a device with nothing on 443 is an
HTTP-only build and gets plain HTTP on --port, token included.

Usage:
    CANARY_TOKEN=... python ota_deploy.py [device_ip]
    CANARY_TOKEN=... CANARY_TLS_FP=... python ota_deploy.py --watch 192.168.4.1
    CANARY_IP=192.168.4.1 python ota_deploy.py      # asks for the token

Requirements:
    Python 3.10+; pip install watchdog (only for --watch)

Copyright (c) 2026 ERRERlabs / Karl May
License: Apache-2.0
"""

import argparse
import getpass
import hashlib
import hmac
import http.client
import json
import os
import socket
import ssl
import sys
import time
from dataclasses import dataclass
from pathlib import Path

# Configuration
DEFAULT_IP = "192.168.4.1"
DEFAULT_PORT = 80
DEFAULT_HTTPS_PORT = 443
OTA_ENDPOINT = "/api/ota"
STATUS_ENDPOINT = "/api/status"
BUILD_DIR = ".pio/build"
FIRMWARE_NAME = "firmware.bin"
TOKEN_ENV = "CANARY_TOKEN"
PIN_ENV = "CANARY_TLS_FP"
PROBE_TIMEOUT_S = 5
STATUS_TIMEOUT_S = 10
OTA_TIMEOUT_S = 120

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


# ── credentials ──────────────────────────────────────────────────────────


def read_token() -> str | None:
    """The bearer token: $CANARY_TOKEN, else a no-echo prompt on a terminal.

    Surrounding whitespace (a pasted newline) is dropped. A token holding
    anything but printable ASCII is refused: it would not survive an HTTP
    header, and the device's tokens never do.
    """
    token = os.environ.get(TOKEN_ENV, "").strip()
    if not token and sys.stdin.isatty():
        try:
            token = getpass.getpass(
                "Canary API token (the recovery kit's `token`; not echoed): "
            ).strip()
        except (EOFError, KeyboardInterrupt):
            print()
            token = ""
    if not token:
        log_error(f"No API token. Set {TOKEN_ENV} to the `token` in the Canary's recovery kit")
        log_error("(canary-recovery-kit.json), or run this from a terminal to be asked for it.")
        return None
    if any(not (0x21 <= ord(c) <= 0x7E) for c in token):
        log_error(f"{TOKEN_ENV} holds a space or a non-printable character; it is not a Canary token.")
        return None
    return token


def normalize_fp(value: str | None) -> str | None:
    """Lowercase 64-hex SHA-256, colons and whitespace dropped; None if not one."""
    if not value:
        return None
    fp = "".join(value.split()).replace(":", "").lower()
    if len(fp) != 64 or any(c not in "0123456789abcdef" for c in fp):
        return None
    return fp


# ── transport ────────────────────────────────────────────────────────────


@dataclass(frozen=True)
class Target:
    ip: str
    port: int
    pin: str | None  # set: HTTPS pinned to this certificate fingerprint

    @property
    def url(self) -> str:
        return f"{'https' if self.pin else 'http'}://{self.ip}:{self.port}"


class PinMismatch(Exception):
    def __init__(self, presented: str, pinned: str):
        super().__init__(f"certificate {presented} is not the pinned {pinned}")
        self.presented = presented
        self.pinned = pinned


def _tls_context() -> ssl.SSLContext:
    # The Canary's certificate is self-signed, so no CA can vouch for it: the
    # chain and hostname checks are replaced by the fingerprint pin, which is
    # checked on every connection before a single request byte is sent.
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    return ctx


def _fingerprint(der: bytes | None) -> str:
    return hashlib.sha256(der).hexdigest() if der else ""


class PinnedHTTPSConnection(http.client.HTTPSConnection):
    """HTTPS that refuses to talk unless the server's certificate DER hashes
    to the pin — the same SHA-256 the device reports as tls_cert_fp."""

    def __init__(self, host: str, port: int, pin: str, timeout: float):
        super().__init__(host, port, timeout=timeout, context=_tls_context())
        self._pin = pin

    def connect(self) -> None:
        super().connect()
        presented = _fingerprint(self.sock.getpeercert(binary_form=True))
        if not hmac.compare_digest(presented, self._pin):
            self.close()
            raise PinMismatch(presented, self._pin)


def presented_fingerprint(ip: str, port: int, timeout: float = PROBE_TIMEOUT_S) -> str:
    """Handshake only, send nothing: the SHA-256 of the certificate on ip:port.
    Raises ConnectionRefusedError when nothing listens there."""
    with socket.create_connection((ip, port), timeout=timeout) as raw:
        with _tls_context().wrap_socket(raw) as tls:
            return _fingerprint(tls.getpeercert(binary_form=True))


def resolve_target(ip: str, http_port: int, https_port: int, pin_value: str | None) -> Target | None:
    """Decide how to reach the device. Pinned HTTPS when a pin is given;
    otherwise plain HTTP only if nothing listens on the HTTPS port."""
    if pin_value:
        pin = normalize_fp(pin_value)
        if pin is None:
            log_error(f"{PIN_ENV} is not a SHA-256 fingerprint (64 hex digits, colons allowed).")
            return None
        return Target(ip, https_port, pin)
    try:
        presented = presented_fingerprint(ip, https_port)
    except ConnectionRefusedError:
        log_info(f"Nothing on port {https_port}: an HTTP-only build; using plain HTTP on port {http_port}")
        log_warn("Plain HTTP: the token and the image cross this link unencrypted.")
        return Target(ip, http_port, None)
    except (OSError, ssl.SSLError) as err:
        log_error(f"Cannot tell whether {ip} serves HTTPS on port {https_port}: {err}")
        log_error("Check the address, or set CANARY_TLS_FP for a device that does.")
        return None
    log_error(f"{ip} serves HTTPS on port {https_port}, and {PIN_ENV} is not set.")
    log_error("The token is only ever sent to a certificate you have pinned.")
    log_error(f"The device presented SHA-256 {presented}.")
    log_error(f"If that equals `tls_cert_fp` in its recovery kit, set {PIN_ENV} to it and run again.")
    return None


def _request(target: Target, method: str, path: str, token: str, timeout: float,
             body: bytes | None = None) -> tuple[int, str, str | None]:
    """One request with the bearer. Returns (status, body text, Location)."""
    if target.pin:
        conn: http.client.HTTPConnection = PinnedHTTPSConnection(
            target.ip, target.port, target.pin, timeout)
    else:
        conn = http.client.HTTPConnection(target.ip, target.port, timeout=timeout)
    headers = {"Authorization": f"Bearer {token}"}
    if body is not None:
        headers["Content-Type"] = "application/octet-stream"
    try:
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        text = resp.read().decode("utf-8", errors="replace")
        return resp.status, text, resp.getheader("Location")
    finally:
        conn.close()


def _explain_refusal(status: int, location: str | None) -> None:
    if status in (301, 302, 307, 308):
        log_error(f"The device redirects to HTTPS ({location or 'no Location'}).")
        log_error(f"Set {PIN_ENV} to its `tls_cert_fp` (recovery kit) and run again.")
    elif status in (401, 403):
        log_error(f"The device refused the API token (HTTP {status}). Use the recovery kit's `token`.")
    elif status == 429:
        log_error("The device is rate-limiting after failed attempts (HTTP 429); wait and retry.")
    elif status == 503:
        log_error("The device has no API token yet (HTTP 503): finish its setup first.")


def check_device(target: Target, token: str) -> bool:
    """GET /api/status with the token. False stops the deploy: the token is
    refused, the device redirects, or (over HTTPS) the device reports a
    tls_cert_fp other than the pin."""
    log_info(f"Checking device at {target.url}...")
    try:
        status, text, location = _request(target, "GET", STATUS_ENDPOINT, token, STATUS_TIMEOUT_S)
    except PinMismatch as err:
        log_error(f"TLS certificate mismatch: presented {err.presented}, pinned {err.pinned}.")
        log_error("Refusing to send the token. A factory reset makes a new certificate;")
        log_error("re-read `tls_cert_fp` from the recovery kit if that is what happened.")
        return False
    except (OSError, ssl.SSLError, http.client.HTTPException) as err:
        log_warn(f"Status endpoint not responding ({err})")
        log_warn("Continuing anyway (device may still accept OTA)")
        return True
    if status != 200:
        if status in (301, 302, 307, 308, 401, 403, 429, 503):
            _explain_refusal(status, location)
            return False
        log_warn(f"Status endpoint answered HTTP {status}; continuing anyway")
        return True
    try:
        doc = json.loads(text)
    except ValueError:
        doc = {}
    if not isinstance(doc, dict):
        doc = {}
    if target.pin:
        reported = normalize_fp(str(doc.get("tls_cert_fp", "")))
        if reported != target.pin:
            log_error(f"The device reports tls_cert_fp {reported or 'none'}, not the pinned {target.pin}.")
            log_error("Refusing to deploy.")
            return False
    elif doc.get("tls_enabled") is True:
        log_error(f"The device reports TLS on; set {PIN_ENV} to its `tls_cert_fp` and run again.")
        return False
    log_success("Device is reachable and accepts the token")
    return True


def deploy_firmware(firmware_path: Path, target: Target, token: str) -> bool:
    """Deploy firmware to device via OTA."""
    size = firmware_path.stat().st_size

    log_info(f"Deploying: {firmware_path} ({size} bytes)")
    log_info(f"Target: {target.url}{OTA_ENDPOINT}")

    try:
        firmware_data = firmware_path.read_bytes()
        status, text, location = _request(target, "POST", OTA_ENDPOINT, token, OTA_TIMEOUT_S,
                                          body=firmware_data)
    except PinMismatch as err:
        log_error(f"TLS certificate mismatch: presented {err.presented}, pinned {err.pinned}.")
        log_error("Refusing to send the token or the image.")
        return False
    except ConnectionRefusedError:
        log_error(f"Cannot connect to device at {target.ip}:{target.port}")
        log_error("Make sure you're connected to the Canary WiFi network")
        return False
    except (socket.timeout, TimeoutError):
        log_error("Connection timed out")
        return False
    except (OSError, ssl.SSLError, http.client.HTTPException) as err:
        log_error(f"OTA request failed: {err}")
        return False

    if status == 200:
        log_success("OTA update successful!")
        log_info(f"Response: {text}")
        log_info("Device will reboot...")
        return True
    log_error(f"OTA update failed (HTTP {status})")
    log_error(f"Response: {text}")
    _explain_refusal(status, location)
    return False


def find_firmware(build_dir: str) -> Path | None:
    """Find the most recently modified firmware binary."""
    build_path = Path(build_dir)
    if not build_path.exists():
        return None

    latest = None
    latest_time = 0.0

    for env_dir in build_path.iterdir():
        if env_dir.is_dir():
            fw_path = env_dir / FIRMWARE_NAME
            if fw_path.exists():
                mtime = fw_path.stat().st_mtime
                if mtime > latest_time:
                    latest_time = mtime
                    latest = fw_path

    return latest


def watch_and_deploy(build_dir: str, target: Target, token: str) -> None:
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
                        if deploy_firmware(firmware, target, token):
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


def single_deploy(build_dir: str, target: Target, token: str) -> None:
    """Deploy the most recent firmware once."""
    firmware = find_firmware(build_dir)

    if firmware is None:
        log_error(f"No firmware found in {build_dir}")
        log_info("Run 'pio run' to build first")
        sys.exit(1)

    if not check_device(target, token):
        sys.exit(1)
    success = deploy_firmware(firmware, target, token)
    sys.exit(0 if success else 1)


def print_banner() -> None:
    print()
    print("╔══════════════════════════════════════════════════════════════╗")
    print("║       SecuraCV Canary — OTA Auto-Deploy                      ║")
    print("╚══════════════════════════════════════════════════════════════╝")
    print()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Deploy SecuraCV Canary firmware via OTA",
        epilog=(f"The API token comes from ${TOKEN_ENV} or a no-echo prompt, never from an "
                f"argument. A TLS device needs ${PIN_ENV} (or --tls-fp): its tls_cert_fp."),
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
        help=f"HTTP port for an HTTP-only build (default: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--https-port",
        type=int,
        default=int(os.environ.get("CANARY_HTTPS_PORT", DEFAULT_HTTPS_PORT)),
        help=f"HTTPS port (default: {DEFAULT_HTTPS_PORT})",
    )
    parser.add_argument(
        "--tls-fp",
        default=os.environ.get(PIN_ENV, ""),
        help=f"Pin: the device's tls_cert_fp (default: ${PIN_ENV}). Not a secret.",
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
    return parser


def main() -> None:
    args = build_parser().parse_args()

    print_banner()

    token = read_token()
    if token is None:
        sys.exit(1)
    target = resolve_target(args.ip, args.port, args.https_port, args.tls_fp)
    if target is None:
        sys.exit(1)

    if args.watch:
        if not check_device(target, token):
            sys.exit(1)
        watch_and_deploy(args.build_dir, target, token)
    else:
        single_deploy(args.build_dir, target, token)


if __name__ == "__main__":
    main()
