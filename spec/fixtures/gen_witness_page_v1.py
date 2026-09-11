#!/usr/bin/env python3
"""Generate (or --check) spec/fixtures/witness_page_v1.json — the shared
`GET /api/v1/witness` page fixture both ends of the contract decode.

The fixture is the byte-exact page a canary-wap renders for four records
(spec/witness_api_v1.md, `chain_format: wap_v1`). Everything in it is real:

  * chain hashes are the firmware's own construction
    (canary_wap.ino compute_chain_hash, mirrored by tools/verify_witness_log.py):
      sha256_domain(d, m) = SHA-256(d || 0x00 || m)
      chain_hash = sha256_domain("securacv:fw:chain:v1",
                                 prev || payload_hash || seq(BE32) || tb(BE32))
      genesis    = sha256_domain("securacv:genesis:v1", device_id)
      payload_hash = sha256_domain("securacv:fw:payload:v1", payload)
  * signatures are Ed25519 over the raw 32-byte chain hash, made with a
    TEST-ONLY key whose 32-byte seed is the ASCII below (never on a device);
  * the timestamps are the bucket starts floored to Invariant III's
    ten-minute grain, anchored exactly as the firmware anchors them
    (witness_page.h record_epoch).

Consumers:
  firmware/projects/canary-wap/tests_host/test_witness_page.cpp renders the
    same records through witness_page.h and byte-compares; it also recomputes
    every hash with OpenSSL and verifies every signature against PUBKEY_HEX.
  ios/Tests/SecuraCVTests/WitnessPageFixtureTests.swift decodes the file with
    the app's decoder and expects ChainVerifier to reach `.verified` against
    the same public key.

Usage:
  python3 spec/fixtures/gen_witness_page_v1.py            # write the fixture
  python3 spec/fixtures/gen_witness_page_v1.py --check    # exit 1 on drift
"""

from __future__ import annotations

import datetime as dt
import hashlib
import sys
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
OUT = HERE / "witness_page_v1.json"

# Test-only signing key. 32 ASCII bytes, readable on purpose so nobody
# mistakes it for a device key. The C++ test and the Swift test carry the
# derived PUBKEY_HEX (printed on every run) as a constant.
SEED = b"fixture-key-for-witness-page-v1!"
assert len(SEED) == 32

DEVICE_ID = "canary-fixture-0001"
BUCKET_MS = 5000
NOW_MS = 700_000                 # 700 s of uptime at render
NOW_EPOCH_S = 1_788_864_000      # 2026-09-08T10:40:00Z — a believable clock
COARSE_S = 600

# (seq, time_bucket, record_type, payload). The buckets are chosen so the
# coarse timestamps land in TWO different ten-minute buckets (10:20 and
# 10:30), proving the floor rather than a constant.
RECORDS = [
    (1, 1,   0, b"boot:canary-fixture-0001"),
    (2, 130, 1, b"event:presence_changed"),
    (3, 131, 2, b"tamper:enclosure_tamper"),
    (4, 132, 3, b"state:NOFIX->ACQRD"),
]

# witness_page.h event_type_name(): the dictionary id for tamper, the
# offline verifier's names for the rest (tools/verify_witness_log.py).
EVENT_TYPE = {0: "boot_attestation", 1: "witness_event", 2: "tamper_detected",
              3: "state_change", 4: "power_shutdown"}


def sha256_domain(domain: bytes, data: bytes) -> bytes:
    return hashlib.sha256(domain + b"\x00" + data).digest()


def chain_hash(prev: bytes, payload_hash: bytes, seq: int, tb: int) -> bytes:
    return sha256_domain(b"securacv:fw:chain:v1",
                         prev + payload_hash + seq.to_bytes(4, "big") + tb.to_bytes(4, "big"))


def iso8601(epoch: int) -> str:
    return dt.datetime.fromtimestamp(epoch, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def record_epoch(tb: int, bucket_ms: int) -> int:
    age_s = ((NOW_MS - tb * bucket_ms) & 0xFFFFFFFF) // 1000
    epoch = NOW_EPOCH_S - age_s
    return epoch - (epoch % COARSE_S)


def build() -> tuple[str, str]:
    key = Ed25519PrivateKey.from_private_bytes(SEED)
    pub_hex = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw).hex()

    prev = sha256_domain(b"securacv:genesis:v1", DEVICE_ID.encode())
    rows = []
    for seq, tb, rtype, payload in RECORDS:
        ph = sha256_domain(b"securacv:fw:payload:v1", payload)
        ch = chain_hash(prev, ph, seq, tb)
        sig = key.sign(ch)
        rows.append(
            '{"seq":%u,"hash":"%s","prev_hash":"%s","payload_hash":"%s",'
            '"time_bucket":%u,"time_bucket_ms":%u,"record_type":%u,'
            '"event_type":"%s","zone":"",'
            '"time_source":"device_clock","timestamp":"%s",'
            '"signature":"%s"}'
            % (seq, ch.hex(), prev.hex(), ph.hex(), tb, BUCKET_MS, rtype,
               EVENT_TYPE[rtype], iso8601(record_epoch(tb, BUCKET_MS)), sig.hex()))
        prev = ch

    page = ('{"schema":"securacv/witness_page/v1","chain_format":"wap_v1",'
            '"device_id":"%s","total":%u,"uptime_s":%u,"records":[%s]}'
            % (DEVICE_ID, RECORDS[-1][0], NOW_MS // 1000, ",".join(rows)))
    return page, pub_hex


def main(argv: list[str]) -> int:
    page, pub_hex = build()
    if "--check" in argv:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else None
        if current != page:
            print(f"{OUT.relative_to(REPO)} is stale — rerun "
                  f"python3 spec/fixtures/gen_witness_page_v1.py", file=sys.stderr)
            return 1
        print(f"fixture up to date (pubkey {pub_hex})")
        return 0
    OUT.write_text(page, encoding="utf-8")
    print(f"wrote {OUT.relative_to(REPO)} ({len(page)} bytes); PUBKEY_HEX={pub_hex}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
