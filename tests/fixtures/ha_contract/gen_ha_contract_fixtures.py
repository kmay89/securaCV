#!/usr/bin/env python3
"""Write the Home Assistant seam fixtures: the exact bytes that cross each
contract between the Rust kernel/bridges, the Python integration, the add-on
shell and the sidecar shell, pinned once so every side's test reads the SAME
file instead of retyping the example.

    python3 tests/fixtures/ha_contract/gen_ha_contract_fixtures.py          # write
    python3 tests/fixtures/ha_contract/gen_ha_contract_fixtures.py --check  # CI drift gate

Who reads what (every reader fails when a fixture disagrees with its code):

  announce.json     C1  src/bin/event_mqtt_bridge.rs (AnnouncePayload round-trip,
                        the signed message, the signature) ·
                        custom_components/securacv/tests/test_ha_contract_fixtures.py
                        (kernel_trust.parse_announce / verify_announce_signature)
  claim.json        C3  event_mqtt_bridge.rs (parse_claim_message, topics, freshness) ·
                        the Python claim module
  camera_slug.json  C8  tests/frigate_integration.rs (transport::frigate::camera_slug) ·
                        privacy_witness_kernel/tests/test_serve_wizard_setup.py
  discovery.json    C6  privacy_witness_kernel/tests/test_run_lib.sh
                        (compose_discovery_payload) · the config flow's hassio step
  pair.json         C2  tests/api_pairing.rs (the constant 401) ·
                        tests/witness_api_pairing_cli.rs (receipt keys) ·
                        privacy_witness_kernel/tests/test_config_render.sh (the
                        sidecar's json_line_field over the receipt) ·
                        the integration's async_pair_kernel
  event_state.json  C5  event_mqtt_bridge.rs (EventStatePayload) · the coordinator
                        and kernel_verify.witness_event_attributes
  frigate_link.json C5  src/bin/frigate_bridge.rs (FrigateLinkPayload) · the
                        coordinator and the frigate_link binary sensor
  identity.json     C4  tests/api_identity.rs (IdentityDocument) · the coordinator

Everything here is a TEST vector: the signing key is a fixed 32-byte seed,
the tokens are patterns, the times are one fixed 10-minute bucket. Nothing is
secret and nothing is a real kernel's identity. The announce signature is
Ed25519 (deterministic), so the vector is reproducible from the seed.
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# ---- the one test identity ------------------------------------------------
SEED = bytes([0x11] * 32)
DEVICE_ID = "pwk_1a2b3c4d"
KERNEL_VERSION = "2.4.16"
RULESET_ID = "ruleset:frigate_v1"
BUCKET_START = 1760140800  # 2025-10-11T00:00:00Z, a 10-minute bucket start
BUCKET_SIZE = 600
PREFIX = "witness"
DISCOVERY_PREFIX = "homeassistant"
ANNOUNCE_SIG_PREFIX = "securacv-announce|v1|"


def compact(value) -> str:
    """serde_json's compact writer, for the value types these payloads carry."""
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def verifying_key_hex() -> str:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    return Ed25519PrivateKey.from_private_bytes(SEED).public_key().public_bytes_raw().hex()


def sign(message: bytes) -> str:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    return Ed25519PrivateKey.from_private_bytes(SEED).sign(message).hex()


def fingerprint(key_hex: str) -> str:
    """First 8 hex of SHA-256 over the 32 raw key bytes (C1/C2)."""
    return hashlib.sha256(bytes.fromhex(key_hex)).hexdigest()[:8]


# ---- C1 announce -------------------------------------------------------------
def announce_fixture(key: str) -> dict:
    def payload(api: dict, pairing: str) -> dict:
        # Struct order of event_mqtt_bridge.rs AnnouncePayload, `sig` absent.
        return {
            "v": 1,
            "device_id": DEVICE_ID,
            "kernel_version": KERNEL_VERSION,
            "verifying_key": key,
            "api": api,
            "pairing": pairing,
            "mqtt_topic_prefix": PREFIX,
            "discovery_prefix": DISCOVERY_PREFIX,
            "availability_topic": f"{PREFIX}/status",
            "announced_bucket_start": BUCKET_START,
            "announced_bucket_size": BUCKET_SIZE,
        }

    vectors = []
    for name, api, pairing in (
        ("sidecar_required_no_host", {"scheme": "http", "bind": "all", "port": 8799}, "required"),
        (
            "addon_token_file_with_host",
            {"scheme": "http", "bind": "all", "port": 8799, "host": "d0491a67-privacy-witness-kernel"},
            "token_file",
        ),
    ):
        unsigned = payload(api, pairing)
        unsigned_json = compact(unsigned)
        message = ANNOUNCE_SIG_PREFIX + unsigned_json
        sig = sign(message.encode("utf-8"))
        published = compact({**unsigned, "sig": sig})
        vectors.append(
            {
                "name": name,
                "payload": unsigned_json,
                "canonical_message": message,
                "sig": sig,
                "published": published,
            }
        )
    return {
        "$comment": (
            "Contract C1: the retained announce. `payload` is the sig-less compact "
            "JSON in AnnouncePayload struct order (what the bridge POSTs to "
            "/api/sign-announce), `canonical_message` the bytes the kernel signs, "
            "`sig` plain Ed25519 over them under the test seed, `published` the "
            "retained bytes. Test identity only."
        ),
        "signing_key_seed": SEED.hex(),
        "verifying_key": key,
        "fingerprint": fingerprint(key),
        "topic": f"{PREFIX}/{DEVICE_ID}/announce",
        "vectors": vectors,
    }


# ---- C3 claim ---------------------------------------------------------------
def claim_fixture() -> dict:
    nonce = "0123456789abcdef"
    return {
        "$comment": (
            "Contract C3 as amended: the retained claim the integration publishes "
            "and the bridge honors for 24 h (one bucket of future skew), and the "
            "retained claim_state word the bridge answers with."
        ),
        "topic": f"{PREFIX}/claim/{DEVICE_ID}",
        "nonce": nonce,
        "claimed_bucket_start": BUCKET_START,
        "payload": compact(
            {"v": 1, "claimed": True, "nonce": nonce, "claimed_bucket_start": BUCKET_START}
        ),
        "withdrawal": "",
        "fresh_for_secs": 24 * 60 * 60,
        "future_skew_secs": BUCKET_SIZE,
        "republish_interval_secs": 3600,
        "claim_state_topic": f"{PREFIX}/claim_state",
        "claim_state_values": ["claimed", "unclaimed"],
    }


# ---- C8 camera slug ---------------------------------------------------------
def camera_slug_fixture() -> dict:
    return {
        "$comment": (
            "Contract C8: ASCII-lowercase, every other code point becomes `_`, "
            "per code point, no collapsing, no trimming, cut to max_len. The "
            "wizard writes the allowlist with it; the bridge compares slugs."
        ),
        "max_len": 64,
        "vectors": [
            {"input": "Front Door", "slug": "front_door"},
            {"input": "front-door", "slug": "front_door"},
            {"input": "Front  Door", "slug": "front__door"},
            {"input": "Garage_2", "slug": "garage_2"},
            {"input": "Café", "slug": "caf_"},
            {"input": "porch", "slug": "porch"},
            {"input": "BACK yard!", "slug": "back_yard_"},
            {"input": "İstanbul", "slug": "_stanbul"},
            {"input": "ab " * 40, "slug": ("ab_" * 40)[:64]},
            {"input": "", "slug": ""},
        ],
    }


# ---- C6 Supervisor discovery -------------------------------------------------
def discovery_fixture() -> dict:
    host = "d0491a67-privacy-witness-kernel"
    config = {
        "host": host,
        "port": 8799,
        "token_file": "/config/api_token",
        "mqtt_topic_prefix": PREFIX,
        "device_id": DEVICE_ID,
    }
    return {
        "$comment": (
            "Contract C6: compose_discovery_payload HOST PORT TOKEN_FILE PREFIX "
            "DEVICE_ID emits `payload` byte for byte (jq -c); the integration's "
            "hassio step reads config.* and keys the entry kernel_<device_id>."
        ),
        "args": config,
        "payload": compact({"service": "securacv", "config": config}),
        "unique_id": f"kernel_{DEVICE_ID}",
        "url": f"http://{host}:8799",
    }


# ---- C2 pairing -------------------------------------------------------------
def pair_fixture(key: str) -> dict:
    token = "ab" * 32
    token_id = "0f1e2d3c"
    code = "483921"
    return {
        "$comment": (
            "Contract C2 as amended: the request the integration POSTs, the one "
            "constant refusal, the success body, the revoke answers, and the "
            "receipt `witness_api issue-pairing-code` prints (the sidecar's `pair` "
            "parses it with json_line_field). Patterns, not credentials."
        ),
        "code": code,
        "request": compact({"code": code, "label": "home-assistant"}),
        "refusal": {"status": 401, "body": compact({"error": "invalid_pairing_code"})},
        "malformed": {"status": 400, "body": compact({"error": "bad_request"})},
        "response": compact(
            {
                "token": token,
                "token_id": token_id,
                "device_id": DEVICE_ID,
                "verifying_key": key,
                "kernel_version": KERNEL_VERSION,
            }
        ),
        "revoke": {
            "path": f"/api/pair/{token_id}",
            "ok": {"status": 200, "body": compact({"revoked": token_id})},
            "gone": {"status": 404, "body": compact({"error": "not_found"})},
            "forbidden": {"status": 403, "body": compact({"error": "forbidden"})},
        },
        "issue_pairing_code_stdout": compact(
            {
                "kernel": "witness-kernel",
                "pairing_code": code,
                "expires": BUCKET_START + 600,
                "attempts": 5,
                "device_id": DEVICE_ID,
                "verifying_key": key,
                "fingerprint": fingerprint(key),
            }
        ),
        "pair_card": [
            f"SecuraCV pairing code: {code}   (valid 10 minutes, 5 attempts)",
            f"Kernel key fingerprint: {fingerprint(key)}   (Home Assistant shows the same fingerprint before it pairs)",
            "Kernel URL to confirm in Home Assistant: http://<this docker host>:8799",
            "In Home Assistant: Settings → Devices & Services → SecuraCV (discovered) → Configure → enter the code.",
        ],
    }


# ---- C5 bridge event payload -------------------------------------------------
def event_state_fixture() -> dict:
    payload = {
        "event_type": "BoundaryCrossingObjectLarge",
        "zone_id": "zone:front_door",
        "time_bucket": {"start_epoch_s": BUCKET_START, "size_s": BUCKET_SIZE},
        "time_bucket_start": BUCKET_START,
        "time_bucket_size": BUCKET_SIZE,
        "confidence": 0.87,
        "attestation": "adapter",
        "kernel_version": KERNEL_VERSION,
        "ruleset_id": RULESET_ID,
        "device_id": DEVICE_ID,
        "published_bucket_start": BUCKET_START + BUCKET_SIZE,
        "published_bucket_size": BUCKET_SIZE,
        "verified_bucket_start": BUCKET_START - BUCKET_SIZE,
        "verified_bucket_size": BUCKET_SIZE,
        "verified_ok": True,
        "seq": 1287,
    }
    return {
        "$comment": (
            "Contract C5 as amended: ONE EventStatePayload on witness/last_event "
            "(retained), witness/events and witness/zone/<zone>/event; field order "
            "is the wire order. `ha` is what the integration derives from it."
        ),
        "topics": [f"{PREFIX}/last_event", f"{PREFIX}/events", f"{PREFIX}/zone/front_door/event"],
        "payload": compact(payload),
        "ha": {
            "event_type": "boundary_crossing_object_large",
            "zone_id": "zone:front_door",
            "zone": "front_door",
            "time_bucket_start": BUCKET_START,
            "dedupe_key": ["zone:front_door", BUCKET_START, "boundary_crossing_object_large"],
            "friendly_event": "Person or vehicle",
            "attestation": "adapter",
            "kernel_reported_chain_ok": True,
            "kernel_verified_bucket_start": BUCKET_START - BUCKET_SIZE,
            "seq": 1287,
            "verification_without_a_walk": "sealed",
            "trust_reason_without_a_walk": "kernel_sealed",
        },
    }


# ---- C5 / D14 frigate_link -------------------------------------------------
def frigate_link_fixture() -> dict:
    payload = {
        "v": 1,
        "device_id": DEVICE_ID,
        "connected": True,
        "topic_prefix": "frigate",
        "last_message_bucket_start": BUCKET_START,
        "last_message_bucket_size": BUCKET_SIZE,
        "messages_since_start": 12,
        "mode": "events",
    }
    return {
        "$comment": (
            "Contract C5 (D14): frigate_bridge's retained link-health payload, "
            "wire order. The integration's frigate_link sensor is on iff "
            "connected AND the last message bucket is within stale_after_secs."
        ),
        "topic": f"{PREFIX}/frigate_link",
        "payload": compact(payload),
        "stale_after_secs": 3600,
    }


# ---- C4 identity -----------------------------------------------------------
def identity_fixture(key: str) -> dict:
    payload = {
        "v": 1,
        "device_id": DEVICE_ID,
        "verifying_key": key,
        "key_algorithm": "ed25519",
        "kernel_version": KERNEL_VERSION,
        "ruleset_id": RULESET_ID,
        "chain_length": 1287,
        "latest_hash": "9c" * 32,
        "last_verify": {
            "at": BUCKET_START - BUCKET_SIZE,
            "bucket_size": BUCKET_SIZE,
            "ok": True,
            "verdict": "self-consistent; identity unverified",
            "identity_verified": False,
        },
        "fleet_rollcall": "self",
        "paired_tokens": 1,
    }
    return {
        "$comment": (
            "Contract C4 as amended: GET /api/identity, IdentityDocument field "
            "order. The coordinator pins verifying_key over this channel and "
            "reads chain_length, latest_hash and last_verify (a self-report)."
        ),
        "payload": compact(payload),
        "fingerprint": fingerprint(key),
    }


def fixtures() -> dict[str, dict]:
    key = verifying_key_hex()
    return {
        "announce.json": announce_fixture(key),
        "claim.json": claim_fixture(),
        "camera_slug.json": camera_slug_fixture(),
        "discovery.json": discovery_fixture(),
        "pair.json": pair_fixture(key),
        "event_state.json": event_state_fixture(),
        "frigate_link.json": frigate_link_fixture(),
        "identity.json": identity_fixture(key),
    }


def render(data: dict) -> str:
    return json.dumps(data, indent=2, ensure_ascii=False) + "\n"


def main(argv: list[str]) -> int:
    check = "--check" in argv
    stale: list[str] = []
    for name, data in fixtures().items():
        path = HERE / name
        text = render(data)
        if check:
            if not path.is_file() or path.read_text(encoding="utf-8") != text:
                stale.append(str(path.relative_to(HERE.parents[2])))
        else:
            path.write_text(text, encoding="utf-8")
            print(f"wrote {path.relative_to(HERE.parents[2])}")
    if stale:
        print(
            "stale HA contract fixtures (run tests/fixtures/ha_contract/"
            "gen_ha_contract_fixtures.py): " + ", ".join(stale),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
