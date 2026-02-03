#!/usr/bin/env python3
"""
SecuraCV Canary OTA Mock Server

A simple HTTPS server for testing OTA updates locally.
Serves a manifest.json and firmware binary files.

USAGE:
    # Generate self-signed certificate (first time only):
    openssl req -new -x509 -keyout key.pem -out cert.pem -days 365 -nodes \
        -subj "/CN=localhost"

    # Generate manifest for a firmware binary:
    python mock_ota_server.py generate firmware.bin 1.1.0

    # Start server (serves from current directory):
    python mock_ota_server.py serve

    # Or just run with defaults:
    python mock_ota_server.py

REQUIREMENTS:
    Python 3.7+
    No external dependencies (uses stdlib only)

The server listens on https://localhost:8443 by default.
Configure the ESP32 to use this URL as the manifest URL.

IMPORTANT: For development only! This uses self-signed certificates.
Enable SECURACV_OTA_SKIP_CERT_VERIFY in platformio.ini for testing.

Author: ERRERlabs
License: MIT
"""

import http.server
import ssl
import json
import hashlib
import sys
import os
import argparse
from pathlib import Path
from datetime import datetime

# Default settings
DEFAULT_PORT = 8443
DEFAULT_PRODUCT = "securacv-canary"
DEFAULT_VERSION = "1.1.0"

# ANSI colors for output
class Colors:
    HEADER = '\033[95m'
    BLUE = '\033[94m'
    CYAN = '\033[96m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    END = '\033[0m'
    BOLD = '\033[1m'


def print_banner():
    """Print server banner."""
    print(f"""
{Colors.CYAN}╔═══════════════════════════════════════════════════════════════╗
║           SecuraCV Canary OTA Mock Server                     ║
║                                                               ║
║  A local HTTPS server for testing OTA firmware updates.       ║
╚═══════════════════════════════════════════════════════════════╝{Colors.END}
""")


def calculate_sha256(filepath: str) -> str:
    """Calculate SHA256 hash of a file."""
    sha256_hash = hashlib.sha256()
    with open(filepath, "rb") as f:
        for chunk in iter(lambda: f.read(4096), b""):
            sha256_hash.update(chunk)
    return sha256_hash.hexdigest()


def generate_manifest(bin_path: str, version: str = DEFAULT_VERSION,
                      product: str = DEFAULT_PRODUCT, port: int = DEFAULT_PORT,
                      output_path: str = "manifest.json") -> dict:
    """
    Generate a manifest.json for a firmware binary.

    Args:
        bin_path: Path to the firmware .bin file
        version: Version string (e.g., "1.1.0")
        product: Product identifier
        port: Server port for URL generation
        output_path: Where to write manifest.json

    Returns:
        Manifest dictionary
    """
    if not os.path.exists(bin_path):
        print(f"{Colors.RED}Error: Firmware file not found: {bin_path}{Colors.END}")
        sys.exit(1)

    file_size = os.path.getsize(bin_path)
    sha256 = calculate_sha256(bin_path)
    filename = os.path.basename(bin_path)

    manifest = {
        "product": product,
        "version": version,
        "min_version": "1.0.0",
        "url": f"https://localhost:{port}/{filename}",
        "sha256": sha256,
        "size": file_size,
        "release_notes": f"OTA test update to version {version}",
        "release_url": f"https://localhost:{port}/changelog.html",
        "timestamp": datetime.utcnow().isoformat() + "Z"
    }

    with open(output_path, 'w') as f:
        json.dump(manifest, f, indent=2)

    print(f"{Colors.GREEN}Generated manifest.json:{Colors.END}")
    print(f"  Product:  {manifest['product']}")
    print(f"  Version:  {manifest['version']}")
    print(f"  Size:     {manifest['size']:,} bytes")
    print(f"  SHA256:   {manifest['sha256'][:16]}...{manifest['sha256'][-16:]}")
    print(f"  URL:      {manifest['url']}")
    print(f"  Saved to: {output_path}")

    return manifest


def generate_self_signed_cert(cert_path: str = "cert.pem",
                               key_path: str = "key.pem"):
    """
    Generate self-signed certificate using openssl command.
    """
    import subprocess

    if os.path.exists(cert_path) and os.path.exists(key_path):
        print(f"{Colors.YELLOW}Certificate files already exist, skipping generation.{Colors.END}")
        return

    print(f"{Colors.CYAN}Generating self-signed certificate...{Colors.END}")

    cmd = [
        "openssl", "req", "-new", "-x509",
        "-keyout", key_path,
        "-out", cert_path,
        "-days", "365",
        "-nodes",
        "-subj", "/CN=localhost/O=SecuraCV/C=US"
    ]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode == 0:
            print(f"{Colors.GREEN}Certificate generated successfully!{Colors.END}")
            print(f"  Certificate: {cert_path}")
            print(f"  Private key: {key_path}")
        else:
            print(f"{Colors.RED}Failed to generate certificate:{Colors.END}")
            print(result.stderr)
            sys.exit(1)
    except FileNotFoundError:
        print(f"{Colors.RED}Error: openssl not found. Please install OpenSSL.{Colors.END}")
        print("\nManual command to generate certificate:")
        print(f"  {' '.join(cmd)}")
        sys.exit(1)


class OTARequestHandler(http.server.SimpleHTTPRequestHandler):
    """
    Custom HTTP request handler with CORS and logging.
    """

    def end_headers(self):
        # Add CORS headers
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', '*')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def do_OPTIONS(self):
        """Handle CORS preflight requests."""
        self.send_response(200)
        self.end_headers()

    def log_message(self, format, *args):
        """Custom logging with colors."""
        method = args[0].split()[0] if args else "?"
        path = args[0].split()[1] if args and len(args[0].split()) > 1 else "/"
        status = args[1] if len(args) > 1 else "?"

        # Color based on status
        if str(status).startswith('2'):
            status_color = Colors.GREEN
        elif str(status).startswith('4'):
            status_color = Colors.YELLOW
        elif str(status).startswith('5'):
            status_color = Colors.RED
        else:
            status_color = Colors.END

        print(f"{Colors.CYAN}[{self.log_date_time_string()}]{Colors.END} "
              f"{method} {path} {status_color}{status}{Colors.END}")


def run_server(port: int = DEFAULT_PORT,
               cert_path: str = "cert.pem",
               key_path: str = "key.pem",
               directory: str = "."):
    """
    Start the HTTPS server.

    Args:
        port: Port to listen on
        cert_path: Path to SSL certificate
        key_path: Path to SSL private key
        directory: Directory to serve files from
    """
    # Check for certificate files
    if not os.path.exists(cert_path) or not os.path.exists(key_path):
        print(f"{Colors.YELLOW}Certificate files not found. Generating...{Colors.END}")
        generate_self_signed_cert(cert_path, key_path)

    # Check for manifest.json
    manifest_path = os.path.join(directory, "manifest.json")
    if not os.path.exists(manifest_path):
        print(f"{Colors.YELLOW}Warning: manifest.json not found in {directory}{Colors.END}")
        print(f"Run: python {sys.argv[0]} generate <firmware.bin> [version]")
        print()

    # Change to serving directory
    os.chdir(directory)

    # Create SSL context
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert_path, key_path)

    # Create and configure server
    server_address = ('0.0.0.0', port)
    httpd = http.server.HTTPServer(server_address, OTARequestHandler)
    httpd.socket = context.wrap_socket(httpd.socket, server_side=True)

    print(f"{Colors.GREEN}Server running on:{Colors.END}")
    print(f"  https://localhost:{port}")
    print(f"  https://0.0.0.0:{port}")
    print()
    print(f"{Colors.YELLOW}Note: Using self-signed certificate.{Colors.END}")
    print(f"Enable SECURACV_OTA_SKIP_CERT_VERIFY=1 in platformio.ini for testing.")
    print()
    print(f"{Colors.CYAN}Press Ctrl+C to stop.{Colors.END}")
    print()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print(f"\n{Colors.CYAN}Server stopped.{Colors.END}")


def create_sample_changelog():
    """Create a sample changelog.html file."""
    html = """<!DOCTYPE html>
<html>
<head>
    <title>SecuraCV Canary Firmware Changelog</title>
    <style>
        body { font-family: -apple-system, system-ui, sans-serif; max-width: 800px; margin: 0 auto; padding: 20px; }
        h1 { color: #333; }
        .version { background: #f5f5f5; padding: 15px; border-radius: 8px; margin: 20px 0; }
        .version h2 { margin-top: 0; color: #0066cc; }
        .date { color: #666; font-size: 0.9em; }
        ul { padding-left: 20px; }
        li { margin: 8px 0; }
    </style>
</head>
<body>
    <h1>SecuraCV Canary Firmware Changelog</h1>

    <div class="version">
        <h2>Version 1.1.0</h2>
        <p class="date">Released: (Test Build)</p>
        <ul>
            <li>Added OTA update support</li>
            <li>Improved WiFi connection stability</li>
            <li>Enhanced boot self-test validation</li>
        </ul>
    </div>

    <div class="version">
        <h2>Version 1.0.0</h2>
        <p class="date">Initial Release</p>
        <ul>
            <li>Privacy Witness Kernel with Ed25519 signatures</li>
            <li>SHA256 hash chain for tamper-evident logging</li>
            <li>NVS storage for persistent identity</li>
        </ul>
    </div>
</body>
</html>
"""
    with open("changelog.html", "w") as f:
        f.write(html)
    print(f"{Colors.GREEN}Created changelog.html{Colors.END}")


def main():
    """Main entry point."""
    print_banner()

    parser = argparse.ArgumentParser(
        description="SecuraCV Canary OTA Mock Server",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s serve                     Start HTTPS server on port 8443
  %(prog)s generate fw.bin 1.2.0     Generate manifest for firmware
  %(prog)s cert                      Generate self-signed certificate
  %(prog)s                           Start server (default action)
        """
    )

    subparsers = parser.add_subparsers(dest='command', help='Available commands')

    # serve command
    serve_parser = subparsers.add_parser('serve', help='Start HTTPS server')
    serve_parser.add_argument('-p', '--port', type=int, default=DEFAULT_PORT,
                              help=f'Port to listen on (default: {DEFAULT_PORT})')
    serve_parser.add_argument('-d', '--directory', default='.',
                              help='Directory to serve files from (default: current)')
    serve_parser.add_argument('--cert', default='cert.pem',
                              help='SSL certificate file (default: cert.pem)')
    serve_parser.add_argument('--key', default='key.pem',
                              help='SSL private key file (default: key.pem)')

    # generate command
    gen_parser = subparsers.add_parser('generate', help='Generate manifest.json')
    gen_parser.add_argument('firmware', help='Path to firmware .bin file')
    gen_parser.add_argument('version', nargs='?', default=DEFAULT_VERSION,
                           help=f'Version string (default: {DEFAULT_VERSION})')
    gen_parser.add_argument('-p', '--port', type=int, default=DEFAULT_PORT,
                           help=f'Server port for URL (default: {DEFAULT_PORT})')
    gen_parser.add_argument('--product', default=DEFAULT_PRODUCT,
                           help=f'Product name (default: {DEFAULT_PRODUCT})')
    gen_parser.add_argument('-o', '--output', default='manifest.json',
                           help='Output manifest file (default: manifest.json)')

    # cert command
    cert_parser = subparsers.add_parser('cert', help='Generate self-signed certificate')
    cert_parser.add_argument('--cert', default='cert.pem',
                            help='Certificate output file (default: cert.pem)')
    cert_parser.add_argument('--key', default='key.pem',
                            help='Private key output file (default: key.pem)')

    # init command
    init_parser = subparsers.add_parser('init', help='Initialize server files')

    args = parser.parse_args()

    # Default to serve if no command specified
    if args.command is None:
        args.command = 'serve'
        args.port = DEFAULT_PORT
        args.directory = '.'
        args.cert = 'cert.pem'
        args.key = 'key.pem'

    if args.command == 'serve':
        run_server(args.port, args.cert, args.key, args.directory)
    elif args.command == 'generate':
        generate_manifest(args.firmware, args.version, args.product,
                         args.port, args.output)
    elif args.command == 'cert':
        generate_self_signed_cert(args.cert, args.key)
    elif args.command == 'init':
        generate_self_signed_cert()
        create_sample_changelog()
        print(f"\n{Colors.GREEN}Initialization complete!{Colors.END}")
        print("Next steps:")
        print(f"  1. Copy your firmware.bin to this directory")
        print(f"  2. Run: python {sys.argv[0]} generate firmware.bin 1.1.0")
        print(f"  3. Run: python {sys.argv[0]} serve")


if __name__ == '__main__':
    main()
