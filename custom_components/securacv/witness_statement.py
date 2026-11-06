"""WITNESS_STATEMENT.txt: the evidence pack read out in plain language.

``evidence.py`` composes one zip per ``securacv.export_evidence`` call. The
sealed bundle in it is the evidence and the offline viewer is the checker;
neither tells a person who has never seen SecuraCV what the record says.
This module writes that page: what the witness recorded in the window, by
kind, by zone and window by window; what the record proves; what it does
not prove; and how to check it. Every number is read from the bundle's
``artifact`` (the coarse, human-readable view the kernel exports, spec
``evidence_envelope.md`` §5) — nothing is computed from the ledgers, nothing
is sharper than a ten-minute window, and nothing is written that the sealed
file does not carry.

The statement is a reading of the sealed file, never evidence itself: it
says so in its last lines, names the file it was read from and that file's
SHA-256, and the checker ignores it. A recipient who changes it changes
nothing that verifies.
"""
from __future__ import annotations

import hashlib
import json
from collections import Counter
from collections.abc import Callable
from datetime import datetime, timezone
from typing import Any

from .const import TIME_BUCKET_SECONDS, event_type_metadata, normalize_event_type

# The failure vocabulary (spec/witness_dictionary.json `failure_types`): the
# kernel reporting on itself, never on the scene. A type this version does
# not know is printed as the kernel spelled it.
FAILURE_WORDS: dict[str, str] = {
    "StorageFull": "storage full",
    "StorageWriteFailed": "storage write failed",
    "CryptoFailure": "signing failure",
    "ClockSkew": "clock skew",
    "SensorDisagreement": "sensor disagreement",
    "PowerLoss": "power loss (unclean shutdown)",
    "FirmwareIntegrity": "firmware integrity",
    "GapMissingData": "gap, missing data",
}

# Window-by-window lines are capped so a month-long export stays readable;
# the sealed file carries every window regardless.
MAX_WINDOW_LINES = 200

# The time a window prints as. `local` is the caller's clock (Home
# Assistant's configured zone); the tests pass UTC.
LocalClock = Callable[[int], datetime]


def _utc(epoch_s: int) -> datetime:
    return datetime.fromtimestamp(epoch_s, tz=timezone.utc)


def zone_words(zone_id: Any) -> str:
    """`zone:front_door` → `front door`, as the digest blueprint spells it."""
    text = str(zone_id or "").strip()
    if text.startswith("zone:"):
        text = text[len("zone:") :]
    return text.replace("_", " ") or "unnamed zone"


def event_words(event_type: Any) -> str:
    """The dictionary label of an event type, with its id in brackets."""
    key = normalize_event_type(event_type)
    label = event_type_metadata(event_type)["label"]
    if key is None:
        return "Unknown"
    return f"{label} [{key}]"


def failure_words(failure_type: Any) -> str:
    """The plain words for a failure record's type."""
    text = str(failure_type or "").strip()
    return FAILURE_WORDS.get(text, text or "unknown")


def _bucket_start(obj: Any) -> int | None:
    bucket = obj.get("time_bucket") if isinstance(obj, dict) else None
    if not isinstance(bucket, dict):
        return None
    start = bucket.get("start_epoch_s")
    if isinstance(start, bool) or not isinstance(start, (int, float)):
        return None
    return int(start)


def _bucket_size(obj: Any) -> int:
    bucket = obj.get("time_bucket") if isinstance(obj, dict) else None
    size = bucket.get("size_s") if isinstance(bucket, dict) else None
    if isinstance(size, bool) or not isinstance(size, (int, float)) or int(size) <= 0:
        return TIME_BUCKET_SECONDS
    return int(size)


def read_artifact(bundle_bytes: bytes) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """The bundle's events and failure records, flattened, each with its window.

    Returns ``(events, failures)``; each item carries ``start`` (epoch
    seconds, or None when the bucket has no readable window) and ``size``.
    A bundle this version cannot read yields two empty lists — the statement
    then says so instead of guessing.
    """
    try:
        bundle = json.loads(bundle_bytes)
    except (json.JSONDecodeError, UnicodeDecodeError, ValueError):
        return [], []
    artifact = bundle.get("artifact") if isinstance(bundle, dict) else None
    if not isinstance(artifact, dict):
        return [], []
    events: list[dict[str, Any]] = []
    failures: list[dict[str, Any]] = []
    for batch in artifact.get("batches") or []:
        if not isinstance(batch, dict):
            continue
        for bucket in batch.get("buckets") or []:
            if not isinstance(bucket, dict):
                continue
            bucket_start = _bucket_start(bucket)
            bucket_size = _bucket_size(bucket)
            for event in bucket.get("events") or []:
                if not isinstance(event, dict):
                    continue
                start = _bucket_start(event)
                events.append(
                    {
                        "event_type": event.get("event_type"),
                        "zone_id": event.get("zone_id"),
                        "start": bucket_start if start is None else start,
                        "size": _bucket_size(event) if start is not None else bucket_size,
                    }
                )
            for failure in bucket.get("failures") or []:
                if not isinstance(failure, dict):
                    continue
                start = _bucket_start(failure)
                failures.append(
                    {
                        "failure_type": failure.get("failure_type"),
                        "start": bucket_start if start is None else start,
                        "size": _bucket_size(failure) if start is not None else bucket_size,
                    }
                )
    return events, failures


def _window(start: int | None, size: int, local: LocalClock) -> str:
    """`2026-11-05 02:10 to 02:20` in the caller's clock; `unknown window` without one."""
    if start is None:
        return "unknown window"
    begin = local(start)
    end = local(start + size)
    day = begin.strftime("%Y-%m-%d")
    if end.strftime("%Y-%m-%d") == day:
        return f"{day} {begin.strftime('%H:%M')} to {end.strftime('%H:%M')}"
    return f"{day} {begin.strftime('%H:%M')} to {end.strftime('%Y-%m-%d %H:%M')}"


def _plural(count: int, one: str, many: str | None = None) -> str:
    return f"{count} {one if count == 1 else (many or one + 's')}"


def _counts_block(counter: Counter[str]) -> list[str]:
    """Count first, right-aligned, then the name: the eye reads the number."""
    return [
        f"  {count:>5}  {name}"
        for name, count in sorted(counter.items(), key=lambda item: (-item[1], item[0]))
    ]


def witness_statement_text(
    *,
    bundle_bytes: bytes,
    bundle_filename: str,
    window_hours: int,
    export_id: str,
    exported_at: datetime,
    kernel_device_id: str | None,
    fingerprint: str | None,
    chain_state: str,
    integration_version: str,
    local: LocalClock = _utc,
) -> str:
    """The statement, as text with Unix newlines (the zip member's bytes)."""
    events, failures = read_artifact(bundle_bytes)
    zone_name = local(0).tzname() or "UTC"
    lines: list[str] = []
    out = lines.append

    out("SecuraCV witness statement")
    out("==========================")
    out("")
    out(
        f"Export {export_id}, made {exported_at.strftime('%Y-%m-%d %H:%M')} "
        f"({exported_at.tzname() or 'UTC'}) from Home Assistant, covering the last "
        f"{_plural(window_hours, 'hour')} before it."
    )
    out(f"Witness: kernel {kernel_device_id or 'unnamed'}.")
    if fingerprint:
        out(
            f"Witness key fingerprint: {fingerprint} (the owner can confirm this by "
            "phone or in person; WITNESS_KEY.txt in this zip is their copy)."
        )
    else:
        out("Witness key fingerprint: not pinned when this pack was made.")
    out(
        f"Chain state when exported: {chain_state} (Home Assistant's own walk of "
        "the sealed log against the key it pinned, never the kernel's word for it)."
    )
    out("")

    out("What the witness recorded")
    out("-------------------------")
    out("")
    if not events and not failures:
        out(
            f"No events were sealed in this window ({bundle_filename} carries none). "
            "That is not proof nothing happened: it means the witness sealed nothing "
            "that met its rules, or was not running (a missing heartbeat is a coverage "
            "warning the checker shows)."
        )
    else:
        starts = [e["start"] for e in events if e["start"] is not None]
        windows = {(e["start"], e["size"]) for e in events if e["start"] is not None}
        if starts:
            first = min(starts)
            last_start, last_size = max(windows, key=lambda w: w[0] + w[1])
            span = (
                f" in {_plural(len(windows), 'ten-minute window')} between "
                f"{_window(first, 0, local).split(' to ')[0]} and "
                f"{_window(last_start, last_size, local).split(' to ')[-1]} ({zone_name})"
            )
        else:
            span = ""
        out(f"{_plural(len(events), 'sealed event')}{span}.")
        out("")
        if events:
            out("By kind:")
            lines.extend(_counts_block(Counter(event_words(e["event_type"]) for e in events)))
            out("")
            out("By zone:")
            lines.extend(_counts_block(Counter(zone_words(e["zone_id"]) for e in events)))
            out("")
            out(f"Window by window ({zone_name}; windows are deliberately ten minutes wide):")
            rows: Counter[tuple[int | None, int, str, str]] = Counter(
                (e["start"], e["size"], zone_words(e["zone_id"]), event_words(e["event_type"]))
                for e in events
            )
            ordered = sorted(
                rows.items(), key=lambda item: (item[0][0] is None, item[0][0] or 0, item[0][2], item[0][3])
            )
            for (start, size, zone, kind), count in ordered[:MAX_WINDOW_LINES]:
                times = "x" if count == 1 else f"x{count}"
                out(f"  {_window(start, size, local)}   {zone}   {kind} {times}".rstrip(" x"))
            if len(ordered) > MAX_WINDOW_LINES:
                out(
                    f"  ... and {len(ordered) - MAX_WINDOW_LINES} more lines; every one is in "
                    f"{bundle_filename}, which the checker reads in full."
                )
            out("")
        if failures:
            out(
                f"System notes in this window: {_plural(len(failures), 'failure record')}. "
                "These are the witness reporting on itself (its power, clock, storage or "
                "signing), never on the scene; a note can explain a gap in the events."
            )
            for f in sorted(failures, key=lambda f: (f["start"] is None, f["start"] or 0)):
                out(f"  {_window(f['start'], f['size'], local)}   {failure_words(f['failure_type'])}")
            out("")
        else:
            out("System notes in this window: none. The witness reported no fault on itself.")
            out("")

    out("What this proves")
    out("----------------")
    out("")
    out(
        "- Each line above is a claim the kernel sealed at the time it was made, "
        "signed with the witness key and chained to the claim before it."
    )
    out(
        "- Any change to the sealed file after it was exported, an event added, "
        "removed, edited or reordered, makes verification fail. The record is "
        "tamper-evident: interference becomes visible; it is not prevented."
    )
    out(
        "- The times are deliberate ten-minute windows. The kernel never records a "
        "precise second, so none can be recovered from it."
    )
    out("")

    out("What this does not prove")
    out("------------------------")
    out("")
    out(
        "- What was in the video. SecuraCV never saw the pixels; the camera system "
        "(Frigate, or the camera itself) keeps those, if it kept them at all."
    )
    out(
        "- Who or what it was. 'Person or vehicle' is a size class, not an "
        "identification: there is no face, plate, name or description in the "
        "record, and the software to produce one was never written."
    )
    out(
        "- That nothing else happened. A window with no line means the witness "
        "sealed nothing that met its rules in that window. A system note, or a "
        "missing heartbeat the checker flags, is the honest account of a gap."
    )
    out(
        "- Whose hand held the key. A signature proves the witness key signed the "
        "claim. A device that was seized, copied or opened could sign with the "
        "same key; the fingerprint confirms the key, not the person."
    )
    out(
        "- That this is the only history. One device cannot, by itself, prove it "
        "has shown every reader the same chain. An RFC 3161 timestamp from the "
        "owner's court-export kit, or a second witness, narrows that."
    )
    out("")

    out("How to check it")
    out("---------------")
    out("")
    out(
        "1. Open evidence_viewer.html from this zip in a current browser, with the "
        "network off if you like, and drop the whole zip onto it. It re-derives "
        "every signature and hash on your own computer; nothing is uploaded."
    )
    out(
        "2. Read the verdict at the top. 'Verified' with 'authorship confirmed' is "
        "the full result. 'Self-attested' means the chain is intact but the key "
        "traveled inside the record: ask the owner for the fingerprint above by "
        "another channel, compare, then paste the key from WITNESS_KEY.txt into "
        "the expected-key field and check again."
    )
    out(
        "3. 'Not verified' means a check failed. Do not rely on this copy; ask for "
        "a fresh export and keep both."
    )
    out(
        "4. For the formal kit (an RFC 3161 timestamp, the sha256sum and openssl "
        "walk-through, a custody record and affidavit draft), the owner runs "
        "SecuraCV's court-export tool on the same sealed file."
    )
    out("")

    digest = hashlib.sha256(bundle_bytes).hexdigest()
    out(
        f"This statement was written by the SecuraCV Home Assistant integration "
        f"{integration_version} as a reading of {bundle_filename} (SHA-256 {digest}). "
        "It is not evidence and the checker ignores it; the sealed file is the "
        "evidence. Project: https://github.com/kmay89/securaCV"
    )
    return "\n".join(lines) + "\n"
