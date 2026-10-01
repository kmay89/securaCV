#!/usr/bin/env python3
"""The salted device pseudonym, spelled the way the firmware spells it.

canary-sense and canary-vision print a "Hardware ID", build their MQTT
client id and their mDNS host from `device_pseudonym::device_id_hex`
(firmware/common/identity/device_pseudonym.h): SHA-256 of
"canary:device-id:v1:" || a per-device salt, rendered as 16 characters of
the 54-character unambiguous alphabet (no 0/O/o, no 1/I/i/l/L). The Lab
pages' examples used to be hex (9f41c2d8a06be375, b3f2a9c41d5e), a value
no unit can print (sweep A28). This module derives them instead, from a
salt, by the header's own construction, and refuses to when the header
moves.

  pseudonym(salt)                         derive()'s 16 characters
  make_hostname(device_id, pseudo, cpp)   mdns_mgr.cpp's make_hostname
  client_id(device_id, pseudo, cpp)       mqtt_mgr.cpp's MQTT client id

Each reads the source it imitates and dies (via _tooling.die) on the first
literal that is no longer there, so a changed recipe stops the generator
rather than shipping an example the firmware no longer prints. The examples
use the two salts the shared header's host test derives with
(firmware/projects/canary-wap/tests_host/test_device_pseudonym_common.cpp:
0x11 x 32 and 0x22 x 32); canary-local/tests/fingerprint_examples.test.js
derives the same values on its own.
"""
from __future__ import annotations

import hashlib
import re
from pathlib import Path

from _tooling import die, repo_root

REPO = repo_root()
HEADER = REPO / "firmware/common/identity/device_pseudonym.h"
HOST_TEST = REPO / "firmware/projects/canary-wap/tests_host/test_device_pseudonym_common.cpp"

# The salts the shared header's host test derives with (memset fills).
SALT_A = bytes([0x11]) * 32
SALT_B = bytes([0x22]) * 32

_CACHE: dict = {}


def _read(path: Path) -> str:
    if path not in _CACHE:
        if not path.exists():
            die(f"source missing: {path.relative_to(REPO)}")
        _CACHE[path] = path.read_text(encoding="utf-8")
    return _CACHE[path]


def _must(path: Path, needle: str, label: str) -> None:
    if needle not in _read(path):
        die(f"{label}: expected to find {needle!r} in {path.relative_to(REPO)} — "
            "the pseudonym recipe moved; re-derive the Lab's examples")


def _grab(path: Path, pattern: str, label: str) -> str:
    m = re.search(pattern, _read(path))
    if not m:
        die(f"{label}: pattern /{pattern}/ not found in {path.relative_to(REPO)}")
    return m.group(1)


def _construction() -> tuple[bytes, str, int, int]:
    domain = _grab(HEADER, r'constexpr char\s+DOMAIN\[\]\s*=\s*"([^"]+)";', "DOMAIN")
    alphabet = _grab(HEADER, r'constexpr char\s+ALPHABET\[\]\s*=\s*"([^"]+)";', "ALPHABET")
    limit = int(_grab(HEADER, r"ALPHABET_LIMIT\s*=\s*(\d+);", "ALPHABET_LIMIT"))
    token_bytes = int(_grab(HEADER, r"TOKEN_BYTES\s*=\s*(\d+);", "TOKEN_BYTES"))
    _must(HEADER, "HEX_LEN     = TOKEN_BYTES * 2;", "HEX_LEN is two characters per token byte")
    _must(HEADER, "if (hash[i] < detail::ALPHABET_LIMIT) {", "rejection sampling")
    _must(HEADER, "out_hex[produced++] = detail::ALPHABET[hash[i] % detail::ALPHABET_LEN];",
          "one alphabet character per accepted hash byte")
    _must(HEADER, "out_hex[produced] = detail::ALPHABET[produced];", "the deterministic top-up")
    _must(HEADER, "memcpy(input + off, detail::DOMAIN, detail::DOMAIN_LEN); off += detail::DOMAIN_LEN;",
          "the domain leads the hash input")
    _must(HEADER, "memcpy(input + off, secret, secret_len);", "the salt follows the domain")
    if limit % len(alphabet):
        die(f"ALPHABET_LIMIT {limit} is not a multiple of the {len(alphabet)}-character alphabet")
    for fill in ("memset(secret,  0x11, sizeof(secret));", "memset(secret2, 0x22, sizeof(secret2));"):
        _must(HOST_TEST, fill, "the host test's salts (the Lab's examples use the same two)")
    return domain.encode(), alphabet, limit, token_bytes * 2


def pseudonym(salt: bytes) -> str:
    """device_pseudonym::derive(salt): the 16 characters a unit prints."""
    domain, alphabet, limit, length = _construction()
    digest = hashlib.sha256(domain + salt).digest()
    out = []
    for b in digest:
        if len(out) >= length:
            break
        if b < limit:
            out.append(alphabet[b % len(alphabet)])
    while len(out) < length:
        out.append(alphabet[len(out)])
    return "".join(out)


def make_hostname(device_id: str, pseudo: str, mdns_cpp: Path) -> str:
    """mdns_mgr.cpp's make_hostname: the id (cut to 23 bytes, '_', ' ' and
    '.' turned to '-') plus '-' and the pseudonym's first six characters,
    case kept (the recipe never lowercases)."""
    label = f"make_hostname ({mdns_cpp.relative_to(REPO)})"
    _must(mdns_cpp, "char base[24];", label)
    _must(mdns_cpp, 'copy_str(base, sizeof(base), device_id && device_id[0] ? device_id : "canary");', label)
    _must(mdns_cpp, "if (*p == '_' || *p == ' ' || *p == '.') *p = '-';", label)
    _must(mdns_cpp, 'snprintf(out, cap, "%s-%.6s", base, devid_hex);', label)
    body = _read(mdns_cpp).split("void make_hostname(", 1)[-1].split("\n}\n", 1)[0]
    if "tolower" in body or "toupper" in body:
        die(f"{label} now changes case; the example host must follow it")
    base = (device_id or "canary")[:23]
    for ch in "_ .":
        base = base.replace(ch, "-")
    return f"{base}-{pseudo[:6]}"


def client_id(device_id: str, pseudo: str, mqtt_cpp: Path) -> str:
    """mqtt_mgr.cpp's MQTT client id, the name its "Connecting ... as" line prints."""
    label = f"MQTT client id ({mqtt_cpp.relative_to(REPO)})"
    _must(mqtt_cpp, 'String clientId = String("securacv-") + cfg.device_id + "-" + devid_hex;', label)
    _must(mqtt_cpp, '"Connecting %s:%u as %s ...\\n", cfg.mqtt_host, cfg.mqtt_port, clientId.c_str()', label)
    return f"securacv-{device_id}-{pseudo}"
