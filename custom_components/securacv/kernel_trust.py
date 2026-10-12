"""The kernel's identity: key spelling, fingerprint, and the signed announce.

The kernel (the Privacy Witness Kernel behind the app or the Docker sidecar)
has one Ed25519 device key. Its verifying key is 64 lowercase hex everywhere
this repository exposes it (the sealed-log document, /api/identity, the
/api/pair response, the MQTT announce). The integration pins that key once,
trust-on-first-use, over the AUTHENTICATED HTTP channel, and afterwards:

  - checks every retained announce's signature against the pin (contract C1),
    so a rogue broker client cannot plant a "SecuraCV discovered" card that
    points at its own host;
  - shows the key's fingerprint next to the pairing-code field before a pin
    exists, so the person compares eight characters with what `pair` printed
    and types nothing new.

The fingerprint is the first 8 hex digits of SHA-256 over the 32 raw key
bytes — what `witness_api issue-pairing-code` and `docker exec securacv
pair` print beside the code. (Canary fingerprints use the firmware's
domain-separated hash in device_trust.py; the kernel is not a Canary and its
printout is the contract.)
"""
from __future__ import annotations

import hashlib
import ipaddress
import json
import re
from dataclasses import dataclass
from typing import Any

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric import ed25519

# 64 lowercase hex: the one spelling of a kernel verifying key.
KERNEL_KEY_RE = re.compile(r"^[0-9a-f]{64}$")
# `pwk_` + the first 8 hex of sha256("securacv:ha-device-id:v1:" + seed).
KERNEL_DEVICE_ID_RE = re.compile(r"^pwk_[0-9a-f]{8}$")
# An Ed25519 signature, 64 bytes as 128 hex.
SIGNATURE_RE = re.compile(r"^[0-9a-f]{128}$")
# A host as RFC 1123 spells one: labels of letters, digits and hyphens (no
# leading or trailing hyphen, at most 63 characters), joined by dots, at most
# 253 characters in all — which also admits a dotted-quad IPv4 — or an IPv6
# literal in brackets. Nothing else: a colon, a slash, `?`, `#`, `@`,
# whitespace or a control character would let an announced host (unsigned
# until a pin exists) smuggle a port, a path or a fragment into the URL the
# pairing code is POSTed to. The port is `api.port`, never part of the host.
_HOST_LABEL = r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
HOST_NAME_RE = re.compile(rf"^{_HOST_LABEL}(?:\.{_HOST_LABEL})*$")
BRACKETED_IPV6_RE = re.compile(r"^\[[0-9A-Fa-f:.]{2,45}\]$")
# The canonical message the kernel signs: the prefix, then the compact JSON
# of the announce payload without its `sig` field, keys in the order the
# bridge serialized them.
ANNOUNCE_SIG_PREFIX = b"securacv-announce|v1|"
ANNOUNCE_SCHEMA_VERSION = 1
PAIRING_REQUIRED = "required"
PAIRING_TOKEN_FILE = "token_file"
PAIRING_MODES = (PAIRING_REQUIRED, PAIRING_TOKEN_FILE)
# How many characters of the fingerprint a person is asked to compare.
FINGERPRINT_LENGTH = 8


def normalize_kernel_key(value: Any) -> str | None:
    """The key as 64 lowercase hex, or None for anything else.

    A pasted or printed key may arrive in capitals or with surrounding
    whitespace; the pin keeps one spelling so comparisons are byte-equal.
    """
    if not isinstance(value, str):
        return None
    key = value.strip().lower()
    return key if KERNEL_KEY_RE.fullmatch(key) else None


def kernel_key_fingerprint(key_hex: str) -> str:
    """First 8 hex of SHA-256 over the raw key bytes, as `pair` prints it."""
    key = normalize_kernel_key(key_hex)
    if key is None:
        raise ValueError("kernel verifying key must be 64 hex characters")
    return hashlib.sha256(bytes.fromhex(key)).hexdigest()[:FINGERPRINT_LENGTH]


def valid_kernel_device_id(value: Any) -> bool:
    """True iff `value` is a kernel device id the identity derivation yields."""
    return isinstance(value, str) and KERNEL_DEVICE_ID_RE.fullmatch(value) is not None


def valid_api_host(value: Any) -> bool:
    """True iff `value` is a bare host: an RFC 1123 hostname, a dotted-quad
    IPv4 address, or a bracketed IPv6 literal that parses as one. Anything
    that could carry URL syntax (a port, a path, a query, a fragment, user
    info, whitespace, control characters) is not a host."""
    if not isinstance(value, str) or not value:
        return False
    if HOST_NAME_RE.fullmatch(value) is not None:
        return len(value) <= 253
    if BRACKETED_IPV6_RE.fullmatch(value) is not None:
        try:
            ipaddress.IPv6Address(value[1:-1])
        except ValueError:
            return False
        return True
    return False


@dataclass(frozen=True)
class Announce:
    """A parsed, shape-checked `<prefix>/<device_id>/announce` payload."""

    device_id: str
    kernel_version: str
    verifying_key: str
    api_scheme: str
    api_bind: str
    api_port: int
    api_host: str | None
    pairing: str
    mqtt_topic_prefix: str
    discovery_prefix: str
    availability_topic: str
    announced_bucket_start: int | None
    sig: str | None
    # The payload exactly as received (minus nothing), for the signature
    # check: the canonical bytes are rebuilt from it, so key order survives.
    raw: dict[str, Any]

    @property
    def fingerprint(self) -> str:
        """The announced key's fingerprint, for the pairing step's copy."""
        return kernel_key_fingerprint(self.verifying_key)

    def api_url(self, fallback_host: str | None) -> str | None:
        """The kernel URL to prefill: the announced host when one was set
        explicitly, else `fallback_host` (the broker host HA already talks
        to), else nothing — a person corrects it either way.

        Only ever `scheme://host:port`: the host is checked as a bare host
        (`valid_api_host`) after bracketing a bare IPv6 literal, so no
        announced or configured value can carry its own port, path or
        fragment into the URL; anything else yields no prefill at all.
        """
        host = self.api_host or fallback_host
        if not host:
            return None
        if ":" in host and not host.startswith("["):
            # A bare IPv6 literal (HA's own broker setting) needs brackets in a URL.
            host = f"[{host}]"
        if not valid_api_host(host):
            return None
        return f"{self.api_scheme}://{host}:{self.api_port}"


def _nonempty_str(value: Any, *, max_len: int = 256) -> str | None:
    if isinstance(value, str):
        text = value.strip()
        if text and len(text) <= max_len:
            return text
    return None


def _bucket_start(value: Any) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        return None
    return value


def parse_announce(payload: Any) -> Announce | None:
    """Decode an announce payload; None for anything not a well-formed one.

    Everything here crosses an untrusted broker, so every field is shape-
    checked before it reaches a form, a unique_id or a URL. Unknown fields
    are kept in `raw` (the signature covers them) and otherwise ignored.
    """
    if isinstance(payload, bytes):
        try:
            payload = payload.decode()
        except UnicodeDecodeError:
            return None
    if not isinstance(payload, str) or not payload.strip():
        return None
    try:
        data = json.loads(payload)
    except (json.JSONDecodeError, TypeError, ValueError):
        return None
    if not isinstance(data, dict) or data.get("v") != ANNOUNCE_SCHEMA_VERSION:
        return None
    device_id = data.get("device_id")
    if not valid_kernel_device_id(device_id):
        return None
    verifying_key = normalize_kernel_key(data.get("verifying_key"))
    if verifying_key is None:
        return None
    kernel_version = _nonempty_str(data.get("kernel_version"), max_len=64)
    if kernel_version is None:
        return None
    pairing = data.get("pairing")
    if pairing not in PAIRING_MODES:
        return None
    api = data.get("api")
    if not isinstance(api, dict):
        return None
    scheme = api.get("scheme")
    if scheme not in ("http", "https"):
        return None
    bind = api.get("bind")
    if bind not in ("all", "loopback"):
        return None
    port = api.get("port")
    if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
        return None
    host: str | None = None
    if "host" in api:
        # Unsigned until a pin exists, and the pairing code is POSTed to the
        # URL built from it: a host is a hostname, an IPv4 address or a
        # bracketed IPv6 literal and nothing more (no port, path, query,
        # fragment, user info, whitespace or control characters).
        raw_host = api.get("host")
        if not isinstance(raw_host, str) or not valid_api_host(raw_host):
            return None
        host = raw_host
    mqtt_topic_prefix = _nonempty_str(data.get("mqtt_topic_prefix"), max_len=64)
    if mqtt_topic_prefix is None or "#" in mqtt_topic_prefix or "+" in mqtt_topic_prefix:
        return None
    discovery_prefix = _nonempty_str(data.get("discovery_prefix"), max_len=64) or "homeassistant"
    availability_topic = (
        _nonempty_str(data.get("availability_topic"), max_len=128)
        or f"{mqtt_topic_prefix}/status"
    )
    sig: str | None = None
    if "sig" in data:
        raw_sig = data.get("sig")
        if not isinstance(raw_sig, str) or SIGNATURE_RE.fullmatch(raw_sig.lower()) is None:
            return None
        sig = raw_sig.lower()
    return Announce(
        device_id=str(device_id),
        kernel_version=kernel_version,
        verifying_key=verifying_key,
        api_scheme=str(scheme),
        api_bind=str(bind),
        api_port=port,
        api_host=host,
        pairing=str(pairing),
        mqtt_topic_prefix=mqtt_topic_prefix,
        discovery_prefix=discovery_prefix,
        availability_topic=availability_topic,
        announced_bucket_start=_bucket_start(data.get("announced_bucket_start")),
        sig=sig,
        raw=data,
    )


def announce_canonical_bytes(payload: dict[str, Any]) -> bytes:
    """The bytes the kernel signed: prefix + compact JSON without `sig`.

    `json.dumps` with these settings matches serde_json's compact writer
    for the value types an announce carries (strings, integers, booleans,
    one nested object): no whitespace, the same short escapes for control
    characters, non-ASCII emitted raw. Key order is the order received,
    which is the bridge's struct order.
    """
    unsigned = {key: value for key, value in payload.items() if key != "sig"}
    body = json.dumps(unsigned, separators=(",", ":"), ensure_ascii=False)
    return ANNOUNCE_SIG_PREFIX + body.encode("utf-8")


def verify_announce_signature(announce: Announce, pinned_key_hex: str) -> bool:
    """True iff the announce carries a valid signature by the pinned key."""
    if announce.sig is None:
        return False
    key = normalize_kernel_key(pinned_key_hex)
    if key is None:
        return False
    try:
        public_key = ed25519.Ed25519PublicKey.from_public_bytes(bytes.fromhex(key))
        public_key.verify(bytes.fromhex(announce.sig), announce_canonical_bytes(announce.raw))
    except (InvalidSignature, ValueError):
        return False
    return True
