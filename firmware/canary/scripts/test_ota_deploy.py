"""The canary OTA deploy scripts send the bearer, and only to a pinned TLS peer (F39).

firmware/canary/scripts/ota_deploy.py and ota_deploy.sh used to POST the
image to /api/ota with no Authorization header, so every device refused them
(handle_ota answers auth_gate() first). These tests run both scripts, as an
operator would, against a fake Canary on localhost:

  * an HTTP-only build (nothing on the HTTPS port): the image arrives with
    `Authorization: Bearer <token>` over plain HTTP;
  * a TLS build with CANARY_TLS_FP = its certificate's SHA-256: the image
    arrives over HTTPS, and nothing touches port 80;
  * a TLS build with a wrong pin, or with no pin: the script refuses and the
    fake device never sees the token (no request carries it);
  * a status that reports a different tls_cert_fp, or a refused token:
    no image is sent;
  * no token in the environment and no terminal: refused before any request;
    and `--token` is not an argument at all.

The token never appears in either script's output. The fake device's
certificate is a self-signed ECDSA P-256 one, like the Canary's.

Run: python3 -m pytest firmware/canary/scripts/test_ota_deploy.py -q
(needs `cryptography`; the shell cases also need curl and openssl).
"""

from __future__ import annotations

import datetime
import hashlib
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

cryptography = pytest.importorskip("cryptography")
from cryptography import x509  # noqa: E402
from cryptography.hazmat.primitives import hashes, serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import ec  # noqa: E402
from cryptography.x509.oid import NameOID  # noqa: E402

HERE = Path(__file__).resolve().parent
PY_SCRIPT = HERE / "ota_deploy.py"
SH_SCRIPT = HERE / "ota_deploy.sh"
TOKEN = "cv_0123456789abcdef0123456789abcdef"
IMAGE = b"\xe9" + bytes(range(256)) * 64  # a small stand-in firmware.bin

HAVE_SHELL_TOOLS = all(shutil.which(t) for t in ("bash", "curl", "openssl"))
shell_only = pytest.mark.skipif(not HAVE_SHELL_TOOLS, reason="needs bash, curl and openssl")


# ── a fake Canary ─────────────────────────────────────────────────────────


def _self_signed(tmp: Path) -> tuple[Path, Path, str]:
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "canary-test")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=30))
        .sign(key, hashes.SHA256())
    )
    cert_path, key_path = tmp / "cert.pem", tmp / "key.pem"
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    fp = hashlib.sha256(cert.public_bytes(serialization.Encoding.DER)).hexdigest()
    return cert_path, key_path, fp


class FakeCanary:
    """One plain and (optionally) one TLS listener sharing a request log."""

    def __init__(self, tmp: Path, tls: bool, reported_fp: str | None = None):
        self.requests: list[dict] = []
        self.cert_fp = ""
        self._servers: list[ThreadingHTTPServer] = []
        handler = self._handler(tls)
        self.http = self._serve(ThreadingHTTPServer(("127.0.0.1", 0), handler))
        if tls:
            cert, key, self.cert_fp = _self_signed(tmp)
            srv = ThreadingHTTPServer(("127.0.0.1", 0), handler)
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            # The same floor ota_deploy.py's client sets; the fake Canary
            # never offers what the device and the script refuse.
            ctx.minimum_version = ssl.TLSVersion.TLSv1_2
            ctx.load_cert_chain(cert, key)
            srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
            srv.handle_error = lambda *a: None  # handshake-only probes are expected
            self.https = self._serve(srv)
        else:
            self.https = _closed_port()
        self.reported_fp = self.cert_fp if reported_fp is None else reported_fp

    def _serve(self, srv: ThreadingHTTPServer) -> int:
        self._servers.append(srv)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        return srv.server_address[1]

    def close(self) -> None:
        for srv in self._servers:
            srv.shutdown()
            srv.server_close()

    def _handler(self, tls: bool):
        canary = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):  # quiet
                pass

            def _record(self, body: bytes = b"") -> bool:
                auth = self.headers.get("Authorization")
                canary.requests.append(
                    {
                        "scheme": "https" if isinstance(self.connection, ssl.SSLSocket) else "http",
                        "method": self.command,
                        "path": self.path,
                        "auth": auth,
                        "body": body,
                    }
                )
                return auth == f"Bearer {TOKEN}"

            def _send(self, code: int, doc: dict | None = None) -> None:
                # Compact, as ArduinoJson serializes the device's documents.
                data = json.dumps(doc or {}, separators=(",", ":")).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                if self.path == "/generate_204":
                    self._record()
                    self.send_response(204)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                ok = self._record()
                if self.path != "/api/status":
                    return self._send(404)
                if not ok:
                    return self._send(401, {"error": "unauthorized"})
                self._send(
                    200,
                    {
                        "ok": True,
                        "tls_enabled": tls,
                        "tls_cert_fp": canary.reported_fp if tls else "",
                    },
                )

            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
                ok = self._record(body)
                if self.path != "/api/ota":
                    return self._send(404)
                if not ok:
                    return self._send(401, {"error": "unauthorized"})
                self._send(200, {"ok": True, "message": "Rebooting..."})

        return Handler


def _closed_port() -> int:
    """A localhost port nothing listens on (connections are refused)."""
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture
def build_dir(tmp_path: Path) -> Path:
    env_dir = tmp_path / "build" / "dev"
    env_dir.mkdir(parents=True)
    (env_dir / "firmware.bin").write_bytes(IMAGE)
    return tmp_path / "build"


@pytest.fixture
def canary_factory(tmp_path: Path):
    made: list[FakeCanary] = []

    def make(tls: bool, reported_fp: str | None = None) -> FakeCanary:
        c = FakeCanary(tmp_path, tls, reported_fp)
        made.append(c)
        return c

    yield make
    for c in made:
        c.close()


def _run(kind: str, canary: FakeCanary, build_dir: Path, *, token: str | None = TOKEN,
         pin: str | None = None, args: tuple[str, ...] = ()) -> subprocess.CompletedProcess:
    env = {k: v for k, v in os.environ.items()
           if k not in ("CANARY_TOKEN", "CANARY_TLS_FP", "CANARY_IP")}
    env.update(
        CANARY_IP="127.0.0.1",
        CANARY_PORT=str(canary.http),
        CANARY_HTTPS_PORT=str(canary.https),
        BUILD_DIR=str(build_dir),
    )
    if token is not None:
        env["CANARY_TOKEN"] = token
    if pin is not None:
        env["CANARY_TLS_FP"] = pin
    cmd = [sys.executable, str(PY_SCRIPT), *args] if kind == "py" else ["bash", str(SH_SCRIPT), *args]
    proc = subprocess.run(cmd, env=env, stdin=subprocess.DEVNULL, capture_output=True,
                          text=True, timeout=60)
    assert TOKEN not in proc.stdout + proc.stderr, "the token was printed"
    return proc


def _posts(canary: FakeCanary) -> list[dict]:
    return [r for r in canary.requests if r["method"] == "POST" and r["path"] == "/api/ota"]


def _with_token(canary: FakeCanary) -> list[dict]:
    return [r for r in canary.requests if r["auth"]]


KINDS = ["py", pytest.param("sh", marks=shell_only)]


# ── the cases ─────────────────────────────────────────────────────────────


@pytest.mark.parametrize("kind", KINDS)
def test_http_only_build_gets_the_bearer(kind, canary_factory, build_dir) -> None:
    canary = canary_factory(tls=False)
    proc = _run(kind, canary, build_dir)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    posts = _posts(canary)
    assert len(posts) == 1
    assert posts[0]["scheme"] == "http"
    assert posts[0]["auth"] == f"Bearer {TOKEN}"
    assert posts[0]["body"] == IMAGE
    assert "unencrypted" in proc.stdout


@pytest.mark.parametrize("kind", KINDS)
def test_tls_build_with_the_right_pin_gets_https(kind, canary_factory, build_dir) -> None:
    canary = canary_factory(tls=True)
    # Colons and upper case, as a fingerprint is often written, are accepted.
    pin = ":".join(canary.cert_fp[i:i + 2] for i in range(0, 64, 2)).upper()
    proc = _run(kind, canary, build_dir, pin=pin)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    posts = _posts(canary)
    assert len(posts) == 1
    assert posts[0]["scheme"] == "https"
    assert posts[0]["auth"] == f"Bearer {TOKEN}"
    assert posts[0]["body"] == IMAGE
    assert all(r["scheme"] == "https" for r in canary.requests), "plain HTTP was used"


@pytest.mark.parametrize("kind", KINDS)
def test_wrong_pin_never_sends_the_token(kind, canary_factory, build_dir) -> None:
    canary = canary_factory(tls=True)
    proc = _run(kind, canary, build_dir, pin="00" * 32)
    assert proc.returncode != 0
    assert "mismatch" in proc.stdout
    assert canary.cert_fp in proc.stdout  # the presented fingerprint is named
    assert not _with_token(canary)
    assert not _posts(canary)


@pytest.mark.parametrize("kind", KINDS)
def test_tls_build_without_a_pin_is_refused(kind, canary_factory, build_dir) -> None:
    canary = canary_factory(tls=True)
    proc = _run(kind, canary, build_dir)
    assert proc.returncode != 0
    assert "CANARY_TLS_FP is not set" in proc.stdout
    assert canary.cert_fp in proc.stdout
    assert not _with_token(canary)
    assert not _posts(canary)


@pytest.mark.parametrize("kind", KINDS)
def test_status_reporting_another_certificate_stops_the_deploy(kind, canary_factory, build_dir) -> None:
    canary = canary_factory(tls=True, reported_fp="ab" * 32)
    proc = _run(kind, canary, build_dir, pin=canary.cert_fp)
    assert proc.returncode != 0
    assert "not the pinned" in proc.stdout
    assert not _posts(canary)


@pytest.mark.parametrize("kind", KINDS)
def test_refused_token_stops_before_the_image(kind, canary_factory, build_dir) -> None:
    canary = canary_factory(tls=False)
    proc = _run(kind, canary, build_dir, token="cv_wrong")
    assert proc.returncode != 0
    assert "refused the API token" in proc.stdout
    assert not _posts(canary)


@pytest.mark.parametrize("kind", KINDS)
def test_no_token_and_no_terminal_is_refused(kind, canary_factory, build_dir) -> None:
    canary = canary_factory(tls=False)
    proc = _run(kind, canary, build_dir, token=None)
    assert proc.returncode != 0
    assert "No API token" in proc.stdout
    assert not canary.requests


@pytest.mark.parametrize("kind", KINDS)
def test_malformed_pin_is_refused(kind, canary_factory, build_dir) -> None:
    canary = canary_factory(tls=True)
    proc = _run(kind, canary, build_dir, pin="not-a-fingerprint")
    assert proc.returncode != 0
    assert "not a SHA-256 fingerprint" in proc.stdout
    assert not _with_token(canary)


def test_the_token_is_never_an_argument(canary_factory, build_dir) -> None:
    canary = canary_factory(tls=False)
    proc = _run("py", canary, build_dir, token=None, args=("--token", TOKEN.replace("cv_", "x")))
    assert proc.returncode == 2 and "unrecognized arguments" in proc.stderr
    assert not canary.requests
