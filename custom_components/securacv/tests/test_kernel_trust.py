"""The kernel's identity: key spelling, fingerprint, the signed announce.

The announce is the one place a kernel offers itself over an untrusted
broker, so every field is shape-checked, and once a pin exists its
signature is checked against that pin over the exact bytes the kernel
signed (`securacv-announce|v1|` + the compact JSON without `sig`). The
fingerprint is what `pair` prints beside the code: eight hex characters of
SHA-256 over the raw key bytes.
"""

from __future__ import annotations

import hashlib
import json

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from . import conftest  # noqa: F401  (installs ha stubs at import time)

from ..kernel_trust import (  # noqa: E402
    ANNOUNCE_SIG_PREFIX,
    announce_canonical_bytes,
    kernel_key_fingerprint,
    normalize_kernel_key,
    parse_announce,
    valid_api_host,
    valid_kernel_device_id,
    verify_announce_signature,
)

PRIV = Ed25519PrivateKey.from_private_bytes(b"\x11" * 32)
KEY = PRIV.public_key().public_bytes_raw().hex()


def payload(**overrides) -> dict:
    data = {
        "v": 1,
        "device_id": "pwk_1a2b3c4d",
        "kernel_version": "2.4.16",
        "verifying_key": KEY,
        "api": {"scheme": "http", "bind": "all", "port": 8799},
        "pairing": "required",
        "mqtt_topic_prefix": "witness",
        "discovery_prefix": "homeassistant",
        "availability_topic": "witness/status",
        "announced_bucket_start": 1760140800,
        "announced_bucket_size": 600,
    }
    data.update(overrides)
    return data


def signed(**overrides) -> dict:
    data = payload(**overrides)
    data["sig"] = PRIV.sign(announce_canonical_bytes(data)).hex()
    return data


def test_fingerprint_is_sha256_of_the_raw_key_bytes():
    assert kernel_key_fingerprint(KEY) == hashlib.sha256(bytes.fromhex(KEY)).hexdigest()[:8]
    assert kernel_key_fingerprint(KEY.upper()) == kernel_key_fingerprint(KEY)
    with pytest.raises(ValueError):
        kernel_key_fingerprint("not a key")


def test_key_normalization():
    assert normalize_kernel_key(" " + KEY.upper() + "\n") == KEY
    for bad in (None, 42, KEY[:-2], KEY + "00", "zz" * 32, ""):
        assert normalize_kernel_key(bad) is None


def test_device_id_shape():
    assert valid_kernel_device_id("pwk_1a2b3c4d")
    for bad in ("pwk_1A2B3C4D", "pwk_1a2b3c4", "pwk_1a2b3c4d5", "canary01", "", None, 7):
        assert not valid_kernel_device_id(bad)


def test_parse_announce_reads_every_field():
    a = parse_announce(json.dumps(payload(api={"scheme": "https", "bind": "loopback",
                                              "port": 8799, "host": "192.168.1.20"})).encode())
    assert a is not None
    assert (a.device_id, a.kernel_version, a.verifying_key) == ("pwk_1a2b3c4d", "2.4.16", KEY)
    assert (a.api_scheme, a.api_bind, a.api_port, a.api_host) == ("https", "loopback", 8799, "192.168.1.20")
    assert a.pairing == "required"
    assert a.mqtt_topic_prefix == "witness"
    assert a.availability_topic == "witness/status"
    assert a.announced_bucket_start == 1760140800
    assert a.sig is None
    assert a.fingerprint == kernel_key_fingerprint(KEY)
    assert a.api_url(None) == "https://192.168.1.20:8799"
    assert a.api_url("ignored-when-announced") == "https://192.168.1.20:8799"


def test_api_url_falls_back_to_the_broker_host_and_brackets_ipv6():
    a = parse_announce(json.dumps(payload()))
    assert a.api_host is None
    assert a.api_url(None) is None
    assert a.api_url("10.0.0.5") == "http://10.0.0.5:8799"
    assert a.api_url("fd00::1") == "http://[fd00::1]:8799"
    assert a.api_url("[fd00::1]") == "http://[fd00::1]:8799"
    assert a.api_url("mosquitto") == "http://mosquitto:8799"


# What an announced `api.host` may NOT be: anything that would carry its own
# port, path, query, fragment or user info into the URL the 6-digit code is
# POSTed to (the announce is unsigned until a pin exists), plus whitespace,
# control characters, a bare IPv6 (brackets are the only spelling), and
# names RFC 1123 refuses.
HOSTILE_HOSTS = [
    "192.168.1.9:1234#",
    "192.168.1.9:1234",
    "192.168.1.9#frag",
    "192.168.1.9?x=1",
    "evil.example/x",
    "a@b",
    "user:pw@host",
    "host name",
    "host\tname",
    "host\nname",
    " host",
    "host ",
    "host\x00",
    "ho\x7fst",
    "fd00::1",
    "[fd00::1]:8799",
    "[fd00::1]#",
    "[not-an-address]",
    "[fd00::1%eth0]",
    "[192.168.1.9]",
    "[]",
    "-bad.example",
    "bad-.example",
    "ex..ample",
    ".example",
    "example.",
    "under_score.example",
    "h\u00f6st.example",
    "a" * 64 + ".example",
    ".".join(["a" * 63] * 4) + ".ab",
    "",
    "http://192.168.1.9",
]


@pytest.mark.parametrize("bad", HOSTILE_HOSTS)
def test_an_announced_host_that_is_not_a_bare_host_is_refused(bad):
    assert not valid_api_host(bad), bad
    assert parse_announce(json.dumps(payload(api={"scheme": "http", "bind": "all", "port": 8799, "host": bad}))) is None


@pytest.mark.parametrize("non_string", [None, 1, True, ["h"], {"h": 1}])
def test_a_non_string_host_is_refused(non_string):
    assert not valid_api_host(non_string)
    assert parse_announce(json.dumps(payload(api={"scheme": "http", "bind": "all", "port": 8799, "host": non_string}))) is None


@pytest.mark.parametrize("good,url_host", [
    ("192.168.1.9", "192.168.1.9"),
    ("sidecar", "sidecar"),
    ("frigate.lan", "frigate.lan"),
    ("my-host.example.com", "my-host.example.com"),
    ("Host.Example", "Host.Example"),
    ("9front", "9front"),
    ("[fd00::1]", "[fd00::1]"),
    ("[::1]", "[::1]"),
    ("[2001:db8::ffff:192.168.1.9]", "[2001:db8::ffff:192.168.1.9]"),
    ("a" * 63 + ".example", "a" * 63 + ".example"),
])
def test_a_bare_host_is_accepted_and_the_url_is_scheme_host_port_only(good, url_host):
    assert valid_api_host(good)
    a = parse_announce(json.dumps(payload(api={"scheme": "https", "bind": "all", "port": 8443, "host": good})))
    assert a is not None and a.api_host == good
    assert a.api_url("ignored") == f"https://{url_host}:8443"


@pytest.mark.parametrize("bad", HOSTILE_HOSTS)
def test_api_url_never_builds_a_url_from_a_hostile_fallback_host(bad):
    """The fallback (HA's own broker setting) is trusted input, but the URL
    is still only ever scheme://host:port — anything else prefills nothing."""
    a = parse_announce(json.dumps(payload()))
    if bad == "fd00::1":
        # The one courtesy: a bare IPv6 from HA's own MQTT entry gets brackets.
        assert a.api_url(bad) == "http://[fd00::1]:8799"
    else:
        assert a.api_url(bad) is None, bad


@pytest.mark.parametrize(
    "bad",
    [
        "",
        b"",
        "not json",
        "[]",
        "null",
        b"\xff\xfe{",
        json.dumps(payload(v=2)),
        json.dumps(payload(device_id="pwk_ZZZZZZZZ")),
        json.dumps(payload(device_id="canary01")),
        json.dumps(payload(verifying_key="ab" * 31)),
        json.dumps(payload(kernel_version="")),
        json.dumps(payload(pairing="pending-code-483921")),
        json.dumps(payload(api="http://x:8799")),
        json.dumps(payload(api={"scheme": "ftp", "bind": "all", "port": 8799})),
        json.dumps(payload(api={"scheme": "http", "bind": "everywhere", "port": 8799})),
        json.dumps(payload(api={"scheme": "http", "bind": "all", "port": 70000})),
        json.dumps(payload(api={"scheme": "http", "bind": "all", "port": True})),
        json.dumps(payload(api={"scheme": "http", "bind": "all", "port": 8799, "host": "evil.example/x"})),
        json.dumps(payload(api={"scheme": "http", "bind": "all", "port": 8799, "host": "a@b"})),
        json.dumps(payload(mqtt_topic_prefix="witness/#")),
        json.dumps(payload(sig="nothex")),
        json.dumps(payload(sig="ab" * 63)),
    ],
)
def test_parse_announce_refuses_malformed_payloads(bad):
    assert parse_announce(bad) is None


def test_canonical_bytes_are_prefix_plus_compact_json_without_sig():
    data = signed()
    unsigned = {k: v for k, v in data.items() if k != "sig"}
    expected = ANNOUNCE_SIG_PREFIX + json.dumps(
        unsigned, separators=(",", ":"), ensure_ascii=False
    ).encode()
    assert announce_canonical_bytes(data) == expected
    assert b" " not in announce_canonical_bytes(data)
    assert announce_canonical_bytes(data).startswith(b"securacv-announce|v1|{")


def test_a_valid_signature_verifies_only_against_the_signing_key():
    a = parse_announce(json.dumps(signed()))
    assert a.sig is not None
    assert verify_announce_signature(a, KEY) is True
    assert verify_announce_signature(a, KEY.upper()) is True
    other = Ed25519PrivateKey.from_private_bytes(b"\x22" * 32).public_key().public_bytes_raw().hex()
    assert verify_announce_signature(a, other) is False
    assert verify_announce_signature(a, "not a key") is False


def test_tampering_with_any_signed_field_breaks_the_signature():
    data = signed()
    for key, value in (
        ("verifying_key", "ab" * 32),
        ("api", {"scheme": "http", "bind": "all", "port": 8799, "host": "attacker.lan"}),
        ("pairing", "token_file"),
        ("mqtt_topic_prefix", "pwned"),
    ):
        tampered = dict(data)
        tampered[key] = value
        a = parse_announce(json.dumps(tampered))
        assert a is not None, key
        assert verify_announce_signature(a, KEY) is False, key


def test_an_unsigned_announce_never_verifies():
    a = parse_announce(json.dumps(payload()))
    assert verify_announce_signature(a, KEY) is False
