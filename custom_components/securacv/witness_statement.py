"""WITNESS_STATEMENT.txt: the evidence pack read out in plain language.

``evidence.py`` composes one zip per ``securacv.export_evidence`` call. The
sealed bundle in it is the evidence and the offline viewer is the checker;
neither tells a person who has never seen SecuraCV what the record says.
This module writes that page: what the witness recorded in the window, by
kind, by zone and window by window; what the record proves (the export
receipt the witness key signed over these bytes — the per-event seal-time
signatures live in the device's own log, which the bundle does not carry,
and the statement says so); what it does not prove; and how to check it. Every number is read from the bundle's
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
from dataclasses import dataclass
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


@dataclass(frozen=True)
class ArtifactReading:
    """What this version could read out of the bundle's artifact.

    ``readable`` is False when the bytes did not parse to a bundle holding an
    artifact; ``complete`` is False when a collection of a shape this version
    does not know (an ``events`` that is not a list, a batch that is not an
    object) was skipped. The statement names both cases instead of calling
    a file it could not read an empty window.
    """

    events: list[dict[str, Any]]
    failures: list[dict[str, Any]]
    readable: bool
    complete: bool


def _items(obj: Any, key: str) -> tuple[list[Any], bool]:
    """`obj[key]` as a list, and whether it was one (absent counts as empty)."""
    value = obj.get(key) if isinstance(obj, dict) else None
    if value is None:
        return [], True
    if isinstance(value, list):
        return value, True
    return [], False


def read_artifact(bundle_bytes: bytes) -> ArtifactReading:
    """The bundle's events and failure records, flattened, each with its window.

    Each item carries ``start`` (epoch seconds, or None when neither it nor
    its bucket has a readable window) and ``size``. Never raises: a shape
    this version cannot read is skipped and reported through ``complete``.
    """
    try:
        bundle = json.loads(bundle_bytes)
    except (json.JSONDecodeError, UnicodeDecodeError, ValueError):
        return ArtifactReading([], [], readable=False, complete=False)
    artifact = bundle.get("artifact") if isinstance(bundle, dict) else None
    if not isinstance(artifact, dict):
        return ArtifactReading([], [], readable=False, complete=False)
    events: list[dict[str, Any]] = []
    failures: list[dict[str, Any]] = []
    complete = True
    batches, ok = _items(artifact, "batches")
    complete &= ok
    for batch in batches:
        if not isinstance(batch, dict):
            complete = False
            continue
        buckets, ok = _items(batch, "buckets")
        complete &= ok
        for bucket in buckets:
            if not isinstance(bucket, dict):
                complete = False
                continue
            bucket_start = _bucket_start(bucket)
            bucket_size = _bucket_size(bucket)
            bucket_events, ok = _items(bucket, "events")
            complete &= ok
            for event in bucket_events:
                if not isinstance(event, dict):
                    complete = False
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
            bucket_failures, ok = _items(bucket, "failures")
            complete &= ok
            for failure in bucket_failures:
                if not isinstance(failure, dict):
                    complete = False
                    continue
                start = _bucket_start(failure)
                failures.append(
                    {
                        "failure_type": failure.get("failure_type"),
                        "start": bucket_start if start is None else start,
                        "size": _bucket_size(failure) if start is not None else bucket_size,
                    }
                )
    return ArtifactReading(events, failures, readable=True, complete=complete)


def _zone(moment: datetime) -> str:
    """The zone label of one converted time (EDT in July, EST in January)."""
    return moment.tzname() or "UTC"


def _stamp(moment: datetime, day: bool) -> str:
    return moment.strftime("%Y-%m-%d %H:%M" if day else "%H:%M")


def _window(start: int | None, size: int, local: LocalClock) -> str:
    """`2026-11-05 02:10 to 02:20 EST` in the caller's clock, the label taken
    from the window's own start time; `unknown window` without one."""
    if start is None:
        return "unknown window"
    begin = local(start)
    end = local(start + size)
    same_day = end.strftime("%Y-%m-%d") == begin.strftime("%Y-%m-%d")
    return f"{_stamp(begin, True)} to {_stamp(end, not same_day)} {_zone(begin)}"


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
    reading = read_artifact(bundle_bytes)
    events, failures = reading.events, reading.failures
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
    if not reading.readable:
        out(
            f"This version of the integration ({integration_version}) could not read "
            f"{bundle_filename}, so nothing here summarizes it. That says nothing about "
            "the file: the checker reads it directly, and a newer integration may "
            "read it. Ask the owner for a fresh export if the checker rejects it too."
        )
    elif not events and not failures and reading.complete:
        out(
            f"No events were sealed in this window ({bundle_filename} carries none). "
            "That is not proof nothing happened: it means the witness sealed nothing "
            "that met its rules, or was not running (a missing heartbeat is a coverage "
            "warning the checker shows)."
        )
    else:
        if not reading.complete:
            out(
                f"Parts of {bundle_filename} are in a shape this version of the "
                f"integration ({integration_version}) does not know and were skipped "
                "below; the checker reads the whole file. The counts here are a lower "
                "bound, not the record."
            )
            out("")
        starts = [e["start"] for e in events if e["start"] is not None]
        windows = {(e["start"], e["size"]) for e in events if e["start"] is not None}
        if starts:
            first = local(min(starts))
            last_start, last_size = max(windows, key=lambda w: w[0] + w[1])
            last = local(last_start + last_size)
            span = (
                f" in {_plural(len(windows), 'ten-minute window')} between "
                f"{_stamp(first, True)} {_zone(first)} and {_stamp(last, True)} {_zone(last)}"
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
            out(
                "Window by window (in the owner's time zone, each window labeled with "
                "its own; windows are deliberately ten minutes wide):"
            )
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
        "- This file is what the witness exported. At export time the witness key "
        "signed a receipt over the exact bytes of the record above, and that "
        "receipt is chained to the witness's earlier export receipts. The checker "
        "re-derives the hash and the signature: a record changed after export (an "
        "event added, removed, edited or reordered) fails the hash, and a receipt "
        "not made with the witness key fails the signature. Tamper-evident: "
        "interference becomes visible; it is not prevented."
    )
    out(
        "- The events were sealed into the witness's own signed, hash-chained log "
        "at the time they were made. That log is not inside this zip: the chain "
        "state line above is Home Assistant's walk of it against the pinned key, "
        "and the owner's court-export kit carries the log itself. This file "
        "proves the export; the log proves the sealing."
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
        "the full result. 'Self-attested' means the export checks out but the key "
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
