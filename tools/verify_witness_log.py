#!/usr/bin/env python3
"""SecuraCV witness log verifier — prove the sealed log offline.

The canary-wap device appends every signed witness record to
/WITNESS/records.jsonl on its SD card (one self-describing JSON line per
record). This tool re-verifies that file with NOTHING but the device's
public key — no device, no vendor service:

  1. Recomputes every record's chain hash from its own fields
     (prev_hash, payload_hash, seq, time_bucket) and compares it to the
     stored chain hash — any edited field breaks this.
  2. Verifies the Ed25519 signature over each chain hash against the
     device public key — a forged or re-signed line breaks this.
  3. Checks continuity: each record's prev_hash must equal the previous
     record's chain hash, and sequence numbers must be contiguous.
     Gaps are REPORTED, not hidden — records created while the SD card
     was absent legitimately never reached the file, so the log is
     verified as contiguous segments.

Get the public key from the device: GET /api/chain (field "pubkey"), the
BLE witness export, or the boot serial output.

Usage:
  verify_witness_log.py verify RECORDS.jsonl --pubkey <64 hex>
                        [--device-id <id>]   # also verifies the genesis
  verify_witness_log.py inspect RECORDS.jsonl

Exit status: 0 when every complete line's integrity and signature verify
(gaps allowed, reported); 1 on any integrity or signature failure; 2 on
usage/parse errors.

Construction (mirrors canary_wap.ino exactly):
  sha256_domain(domain, data) = SHA-256(domain_ascii || 0x00 || data)
  chain_hash = sha256_domain("securacv:fw:chain:v1",
                             prev(32) || payload_hash(32) ||
                             seq(4, big-endian) || time_bucket(4, BE))
  genesis    = sha256_domain("securacv:genesis:v1", device_id)
  signature  = Ed25519(device_key, chain_hash)
"""

import argparse
import hashlib
import json
import sys

DOMAIN_CHAIN = b"securacv:fw:chain:v1"
DOMAIN_GENESIS = b"securacv:genesis:v1"

RECORD_TYPES = {
    0: "boot_attestation",
    1: "witness_event",
    2: "tamper_alert",
    3: "state_change",
    4: "power_shutdown",
}


def sha256_domain(domain: bytes, data: bytes) -> bytes:
    return hashlib.sha256(domain + b"\x00" + data).digest()


def chain_hash(prev: bytes, payload_hash: bytes, seq: int, tb: int) -> bytes:
    buf = prev + payload_hash + seq.to_bytes(4, "big") + tb.to_bytes(4, "big")
    return sha256_domain(DOMAIN_CHAIN, buf)


def parse_line(raw: str, lineno: int):
    """Returns a record dict or raises ValueError with a precise reason."""
    try:
        obj = json.loads(raw)
    except json.JSONDecodeError as e:
        raise ValueError(f"line {lineno}: not valid JSON ({e})") from e
    if not isinstance(obj, dict):
        raise ValueError(f"line {lineno}: not a JSON object")
    try:
        rec = {
            "seq": int(obj["seq"]),
            "tb": int(obj["tb"]),
            "type": int(obj["type"]),
            "ph": bytes.fromhex(obj["ph"]),
            "prev": bytes.fromhex(obj["prev"]),
            "ch": bytes.fromhex(obj["ch"]),
            "sig": bytes.fromhex(obj["sig"]),
        }
    except (KeyError, TypeError, ValueError) as e:
        raise ValueError(f"line {lineno}: missing/malformed field ({e})") from e
    for field, want in (("ph", 32), ("prev", 32), ("ch", 32), ("sig", 64)):
        if len(rec[field]) != want:
            raise ValueError(
                f"line {lineno}: {field} is {len(rec[field])} bytes, want {want}"
            )
    if not (0 <= rec["seq"] <= 0xFFFFFFFF and 0 <= rec["tb"] <= 0xFFFFFFFF):
        raise ValueError(f"line {lineno}: seq/tb out of u32 range")
    rec["lineno"] = lineno
    return rec


def load_records(path: str):
    """Parse the file. Returns (records, malformed, torn_tail): `malformed`
    is every complete line that does not parse, as (lineno, reason). A torn
    FINAL line is tolerated outright (power-cut model); a malformed line
    anywhere else is judged by `classify_malformed` against the records
    around it — a power-cut scar the firmware sealed onto its own line is
    tolerated, anything else is an integrity failure."""
    records, malformed, torn_tail = [], [], False
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            raw_lines = f.read().split("\n")
    except OSError as e:
        return [], [(0, f"error: cannot read '{path}': {e}")], False
    # A trailing "" after the final newline is normal. A final line with
    # NO newline is only "torn" if it does not parse as a complete
    # record: a power cut mid-append leaves half a line, but an attacker
    # deleting just the final newline must not exempt the newest record
    # from verification.
    tail_candidate = None
    if raw_lines and raw_lines[-1] == "":
        raw_lines.pop()
        complete = raw_lines
    else:
        complete = raw_lines[:-1]
        tail_candidate = raw_lines[-1] if raw_lines else None
    for i, raw in enumerate(complete, start=1):
        if not raw.strip():
            continue
        try:
            records.append(parse_line(raw, i))
        except ValueError as e:
            malformed.append((i, str(e)))
    if tail_candidate is not None and tail_candidate.strip():
        try:
            records.append(parse_line(tail_candidate, len(raw_lines)))
        except ValueError:
            torn_tail = True  # genuinely half a line — the crash model
    return records, malformed, torn_tail


def classify_malformed(records, malformed, genesis):
    """Sort malformed lines into what the chain around them can vouch for.

    The firmware appends one line per record and a power cut can cut that
    line short. On the next mount the device seals the fragment with a
    newline before its first append (witness_store::tail_is_torn), so the
    fragment sits on a line of its own and the record after it chains from
    the last COMPLETE record before it — the fragment was never chained
    upon. So a malformed line is judged by its neighbors, never by itself:

    - the record after it continues the record before it (seq + 1 and
      prev == that record's chain hash): a **power-cut scar**, tolerated;
    - the record after it jumps ahead of the record before it: the line
      sits at a **gap boundary**. The fragment may be a torn append that
      the card then missed a stretch of records after, or a destroyed
      record; either way the records between are not in this file, which
      is exactly what a gap already means (a deleted line reads the same
      with no fragment at all). Tolerated and reported as a gap;
    - the record after it does not advance (seq at or below the record
      before): reordered or replayed — an integrity failure whether or not
      a fragment sits between them;
    - nothing after it: the last complete line of the file. That is what a
      second power cut leaves when the sealing newline landed but the
      record behind it did not — a sealed torn append at the end, with the
      chain ending at the record before it. Tolerated like a torn tail;
    - nothing before it: judged against the genesis when it is known (a
      torn first append is bridged by seq 1 chaining from genesis); without
      a genesis it cannot be judged, and is noted, not failed — the run
      that passes --device-id is the one that decides.

    Returns (scars, failures): message lists. Scars include the gap-boundary
    and unjudged cases, each saying which it is.
    """
    scars, failures = [], []
    for lineno, reason in malformed:
        if lineno == 0:  # a read error, not a line
            failures.append(reason)
            continue
        before = None
        after = None
        for rec in records:
            if rec["lineno"] < lineno:
                before = rec
            elif after is None:
                after = rec
                break
        if after is None:
            # The last complete line, with no record after it. That is the
            # writer's second-cut shape: the sealing newline landed and the
            # record behind it did not, so the old fragment is now a sealed
            # line at the end of the file. Nothing chains across it because
            # nothing follows it; nothing is missing either — the chain
            # ends at the record before it, exactly as a torn tail would.
            # Tolerated like a torn tail, and named so a reader can tell a
            # crash artifact from an integrity finding.
            scars.append(
                f"line {lineno}: sealed torn append at the end of the file "
                f"(a second power cut after the sealing newline landed) — "
                f"the chain ends at the record before it")
            continue
        if before is None:
            if genesis is None:
                scars.append(
                    f"line {lineno}: malformed line before the first record "
                    f"— cannot be judged without --device-id (a torn first "
                    f"append is bridged by seq 1 chaining from genesis)")
            elif after["seq"] == 1 and after["prev"] == genesis:
                scars.append(
                    f"line {lineno}: power-cut scar — a torn first append "
                    f"sealed onto its own line; seq 1 chains from genesis "
                    f"past it")
            else:
                failures.append(
                    f"{reason} — the first record (seq {after['seq']}) does "
                    f"not chain from genesis past it, so this is not a "
                    f"power-cut scar")
            continue
        if after["seq"] == before["seq"] + 1 and after["prev"] == before["ch"]:
            scars.append(
                f"line {lineno}: power-cut scar — a torn append sealed onto "
                f"its own line; seq {after['seq']} chains from seq "
                f"{before['seq']} across it")
        elif after["seq"] > before["seq"] + 1:
            scars.append(
                f"line {lineno}: malformed line at a gap boundary (seq "
                f"{before['seq']} before it, seq {after['seq']} after) — a "
                f"torn append or a destroyed record; the records between "
                f"are not in this file, which the gap below already reports")
        else:
            failures.append(
                f"{reason} — seq {after['seq']} after it does not advance "
                f"past seq {before['seq']} before it (reordered or "
                f"replayed), so this is not a power-cut scar")
    return scars, failures


def verify(path: str, pubkey_hex: str, device_id: str | None) -> int:
    try:
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PublicKey,
        )
    except ImportError:
        print("error: pip install cryptography", file=sys.stderr)
        return 2

    try:
        pub = Ed25519PublicKey.from_public_bytes(bytes.fromhex(pubkey_hex))
    except (ValueError, TypeError):
        print("error: --pubkey must be 64 hex chars of a valid Ed25519 key",
              file=sys.stderr)
        return 2

    records, malformed, torn_tail = load_records(path)
    sig_ok = integrity_ok = 0
    segments = []  # (start_seq, end_seq, chained_from_genesis)

    genesis = (sha256_domain(DOMAIN_GENESIS, device_id.encode())
               if device_id else None)
    scars, failures = classify_malformed(records, malformed, genesis)

    prev_rec = None
    for rec in records:
        recomputed = chain_hash(rec["prev"], rec["ph"], rec["seq"], rec["tb"])
        if recomputed != rec["ch"]:
            failures.append(
                f"line {rec['lineno']} (seq {rec['seq']}): stored chain hash "
                f"does not match its own fields — record edited")
        else:
            integrity_ok += 1
        try:
            pub.verify(rec["sig"], rec["ch"])
            sig_ok += 1
        except InvalidSignature:
            failures.append(
                f"line {rec['lineno']} (seq {rec['seq']}): Ed25519 signature "
                f"invalid for this public key")

        if prev_rec is None:
            anchored = genesis is not None and rec["prev"] == genesis
            segments.append([rec["seq"], rec["seq"], anchored])
        elif rec["seq"] == prev_rec["seq"] + 1:
            if rec["prev"] != prev_rec["ch"]:
                failures.append(
                    f"line {rec['lineno']} (seq {rec['seq']}): contiguous "
                    f"sequence but prev_hash does not chain from seq "
                    f"{prev_rec['seq']} — reordered or spliced")
            segments[-1][1] = rec["seq"]
        elif rec["seq"] > prev_rec["seq"] + 1:
            # Legitimate gap: records created while the card was absent.
            segments.append([rec["seq"], rec["seq"], False])
        else:
            failures.append(
                f"line {rec['lineno']} (seq {rec['seq']}): sequence moved "
                f"backwards from {prev_rec['seq']} — reordered or replayed")
            segments.append([rec["seq"], rec["seq"], False])
        prev_rec = rec

    print(f"records parsed      : {len(records)}")
    print(f"chain-hash verified : {integrity_ok}/{len(records)}")
    print(f"signatures verified : {sig_ok}/{len(records)}")
    if torn_tail:
        print("note: torn final line ignored (power cut mid-append is the "
              "expected crash mode)")
    if scars:
        print(f"note: {len(scars)} malformed line(s) tolerated — each is "
              f"judged by the records around it, never by itself:")
        for scar in scars:
            print(f"  {scar}")
    if segments:
        print("segments:")
        for start, end, anchored in segments:
            tag = " (chains from genesis)" if anchored else ""
            print(f"  seq {start}..{end}{tag}")
        if len(segments) > 1:
            print(f"note: {len(segments) - 1} gap(s) — records created while "
                  f"the SD card was absent never reached the file; each "
                  f"segment above verified independently")
    # --device-id asks for the genesis to be verified (see the usage text), so
    # a log that does not start there is a failure, not a note: a head-truncated
    # file would otherwise pass as "every record verified".
    if device_id and segments and not segments[0][2]:
        failures.append(
            f"first record (seq {segments[0][0]}) does not chain from the "
            f"genesis of device id '{device_id}' — records before it are "
            f"missing, or the id is wrong")

    if failures:
        print(f"\nFAILED — {len(failures)} problem(s):", file=sys.stderr)
        for f_ in failures:
            print(f"  {f_}", file=sys.stderr)
        return 1
    print("\nOK — every record's integrity and signature verified")
    return 0


def inspect(path: str) -> int:
    records, malformed, torn_tail = load_records(path)
    # Without --device-id inspect knows no genesis: a scar before the first
    # record reads as a problem here and a scar under verify --device-id.
    scars, problems = classify_malformed(records, malformed, None)
    print(f"records: {len(records)}")
    if records:
        print(f"seq range: {records[0]['seq']}..{records[-1]['seq']}")
        by_type: dict[str, int] = {}
        for rec in records:
            name = RECORD_TYPES.get(rec["type"], f"type_{rec['type']}")
            by_type[name] = by_type.get(name, 0) + 1
        for name in sorted(by_type):
            print(f"  {name}: {by_type[name]}")
        print(f"newest chain hash: {records[-1]['ch'].hex()}")
    if torn_tail:
        print("note: torn final line present")
    for scar in scars:
        print(f"note: {scar}")
    for p in problems:
        print(f"problem: {p}", file=sys.stderr)
    return 0 if not problems else 1


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    v = sub.add_parser("verify", help="verify chain integrity + signatures")
    v.add_argument("file")
    v.add_argument("--pubkey", required=True,
                   help="device Ed25519 public key (64 hex, from /api/chain)")
    v.add_argument("--device-id", default=None,
                   help="verify the first record chains from this device's genesis")

    i = sub.add_parser("inspect", help="summarize the log without verifying")
    i.add_argument("file")

    args = ap.parse_args()
    if args.cmd == "verify":
        return verify(args.file, args.pubkey, args.device_id)
    return inspect(args.file)


if __name__ == "__main__":
    sys.exit(main())
