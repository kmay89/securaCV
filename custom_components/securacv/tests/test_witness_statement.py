"""WITNESS_STATEMENT.txt: the evidence pack read out in plain language.

What is pinned:
  - every number in the statement is read from the bundle's artifact (events
    by kind, zone and ten-minute window; the kernel's failure records), and
    the file names the bundle it was read from and that file's SHA-256;
  - the windows print in the owner's clock, each labeled with its own zone
    name at that moment (EDT in July, EST in January), never sharper than
    the bucket;
  - the labels are the dictionary's (const.EVENT_TYPE_METADATA), with the
    dictionary id beside each, and a type this version does not know prints
    as the kernel spelled it;
  - the chain state, the fingerprint and the export id are the caller's;
  - "what this proves" claims only what the bundle establishes: the export
    receipt the witness key signed over these bytes; the per-event
    seal-time signatures live in the device's log, which the statement says
    is not in the zip;
  - "what this does not prove" is always present, word for word on the five
    limits the Promise Card names;
  - an unreadable file is called unreadable, a partly readable one says what
    was skipped, and only a readable, complete, empty artifact says no
    events were sealed; none of them raises.
"""

from __future__ import annotations

import hashlib
import json
from datetime import datetime, timedelta, timezone

import pytest

from . import conftest  # noqa: F401  (installs ha stubs at import time)

from .. import witness_statement as ws  # noqa: E402


def _bundle(buckets: list) -> bytes:
    return json.dumps({"artifact": {"batches": [{"buckets": buckets}]}}).encode()


def _event(event_type: str, zone: str, start: int) -> dict:
    return {
        "event_type": event_type,
        "zone_id": f"zone:{zone}",
        "time_bucket": {"start_epoch_s": start, "size_s": 600},
        "confidence": 1.0,
    }


T0 = 1762394400  # 2025-11-06 02:00:00 UTC
BUNDLE = _bundle(
    [
        {
            "time_bucket": {"start_epoch_s": T0, "size_s": 600},
            "events": [
                _event("BoundaryCrossingObjectLarge", "front_door", T0),
                _event("BoundaryCrossingObjectLarge", "front_door", T0),
                _event("BoundaryCrossingObjectSmall", "back_yard", T0),
            ],
            "failures": [],
        },
        {
            "time_bucket": {"start_epoch_s": T0 + 1200, "size_s": 600},
            "events": [_event("BoundaryCrossingObjectLarge", "driveway", T0 + 1200)],
            "failures": [
                {
                    "failure_type": "PowerLoss",
                    "time_bucket": {"start_epoch_s": T0 + 1200, "size_s": 600},
                    "details": None,
                }
            ],
        },
    ]
)

EXPORTED_AT = datetime(2025, 11, 6, 9, 30, tzinfo=timezone.utc)


def _statement(bundle: bytes = BUNDLE, **overrides) -> str:
    kwargs = dict(
        bundle_bytes=bundle,
        bundle_filename="securacv-events-1762394400.json",
        window_hours=24,
        export_id="deadbeef",
        exported_at=EXPORTED_AT,
        kernel_device_id="pwk_1a2b3c4d",
        fingerprint="ab12cd34",
        chain_state="verified ✓",
        integration_version="0.9.0",
    )
    kwargs.update(overrides)
    return ws.witness_statement_text(**kwargs)


def test_the_header_names_the_export_the_witness_the_key_and_the_chain_state():
    text = _statement()
    assert text.startswith("SecuraCV witness statement\n==========================\n")
    assert "Export deadbeef, made 2025-11-06 09:30 (UTC) from Home Assistant, covering the last 24 hours before it." in text
    assert "Witness: kernel pwk_1a2b3c4d." in text
    assert "Witness key fingerprint: ab12cd34 (the owner can confirm this by phone" in text
    assert "Chain state when exported: verified ✓ (Home Assistant's own walk" in text


def test_every_number_is_read_from_the_artifact():
    text = _statement()
    assert "4 sealed events in 2 ten-minute windows between 2025-11-06 02:00 UTC and 2025-11-06 02:30 UTC." in text
    assert "By kind:\n      3  Person or vehicle [boundary_crossing_object_large]\n      1  Animal or small object [boundary_crossing_object_small]\n" in text
    assert "By zone:\n      2  front door\n      1  back yard\n      1  driveway\n" in text
    assert "  2025-11-06 02:00 to 02:10 UTC   front door   Person or vehicle [boundary_crossing_object_large] x2\n" in text
    assert "  2025-11-06 02:00 to 02:10 UTC   back yard   Animal or small object [boundary_crossing_object_small]\n" in text
    assert "  2025-11-06 02:20 to 02:30 UTC   driveway   Person or vehicle [boundary_crossing_object_large]\n" in text
    assert "System notes in this window: 1 failure record." in text
    assert "  2025-11-06 02:20 to 02:30 UTC   power loss (unclean shutdown)\n" in text
    assert "Parts of" not in text, "a complete reading never says anything was skipped"


def test_the_windows_print_in_the_owner_s_clock_and_never_sharper_than_the_bucket():
    tz = timezone(timedelta(hours=-5), "EST")
    text = _statement(local=lambda s: datetime.fromtimestamp(s, tz=tz))
    assert "between 2025-11-05 21:00 EST and 2025-11-05 21:30 EST." in text
    assert "  2025-11-05 21:00 to 21:10 EST   front door" in text
    assert "  2025-11-05 21:20 to 21:30 EST   power loss" in text
    assert "UTC" not in text.split("What the witness recorded")[1].split("What this proves")[0]
    assert ":00:" not in text and "02:03" not in text


def test_each_window_carries_the_zone_label_of_its_own_moment():
    zoneinfo = pytest.importorskip("zoneinfo")
    try:
        tz = zoneinfo.ZoneInfo("America/New_York")
    except zoneinfo.ZoneInfoNotFoundError:  # pragma: no cover - no tzdata on this host
        pytest.skip("no tz database on this host")
    july = int(datetime(2026, 7, 4, 12, 0, tzinfo=timezone.utc).timestamp())
    january = int(datetime(2026, 1, 4, 12, 0, tzinfo=timezone.utc).timestamp())
    bundle = _bundle(
        [
            {
                "time_bucket": {"start_epoch_s": january, "size_s": 600},
                "events": [_event("BoundaryCrossingObjectLarge", "porch", january)],
                "failures": [],
            },
            {
                "time_bucket": {"start_epoch_s": july, "size_s": 600},
                "events": [_event("BoundaryCrossingObjectLarge", "porch", july)],
                "failures": [],
            },
        ]
    )
    text = _statement(bundle, local=lambda s: datetime.fromtimestamp(s, tz=tz))
    assert "  2026-01-04 07:00 to 07:10 EST   porch" in text
    assert "  2026-07-04 08:00 to 08:10 EDT   porch" in text
    assert "between 2026-01-04 07:00 EST and 2026-07-04 08:10 EDT." in text


def test_an_unknown_event_type_prints_as_the_kernel_spelled_it_and_a_missing_zone_is_named():
    bundle = _bundle(
        [
            {
                "time_bucket": {"start_epoch_s": T0, "size_s": 600},
                "events": [{"event_type": "SomethingNewer", "zone_id": None}],
                "failures": [{"failure_type": "NewFault"}],
            }
        ]
    )
    text = _statement(bundle)
    assert "      1  SomethingNewer [something_newer]\n" in text
    assert "      1  unnamed zone\n" in text
    assert "  2025-11-06 02:00 to 02:10 UTC   NewFault\n" in text, "a failure with no window takes its bucket's"


def test_an_empty_artifact_says_no_events_and_what_that_means():
    text = _statement(_bundle([]))
    assert "No events were sealed in this window (securacv-events-1762394400.json carries none)." in text
    assert "That is not proof nothing happened" in text
    assert "could not read" not in text


def test_an_unreadable_file_is_called_unreadable_not_empty_and_never_raises():
    for raw in (b"\x00not json", b"[1, 2, 3]", b'{"artifact": "nope"}', b""):
        text = _statement(raw)
        assert "could not read securacv-events-1762394400.json, so nothing here summarizes it." in text
        assert "No events were sealed" not in text
        assert "SHA-256 " + hashlib.sha256(raw).hexdigest() in text


def test_a_shape_this_version_does_not_know_is_skipped_and_said_never_raised():
    bundle = json.dumps(
        {
            "artifact": {
                "batches": [
                    "not a batch",
                    {"buckets": ["not a bucket", {"events": 3, "failures": None}]},
                    {
                        "buckets": [
                            {
                                "time_bucket": {"start_epoch_s": T0, "size_s": 600},
                                "events": [_event("BoundaryCrossingObjectLarge", "gate", T0), "not an event"],
                            }
                        ]
                    },
                ]
            }
        }
    ).encode()
    reading = ws.read_artifact(bundle)
    assert reading.readable and not reading.complete
    assert [e["zone_id"] for e in reading.events] == ["zone:gate"] and reading.failures == []
    text = _statement(bundle)
    assert "Parts of securacv-events-1762394400.json are in a shape this version" in text
    assert "The counts here are a lower bound, not the record." in text
    assert "1 sealed event in 1 ten-minute window" in text
    only_bad = json.dumps({"artifact": {"batches": [{"buckets": [{"events": 3}]}]}}).encode()
    assert "Parts of" in _statement(only_bad) and "No events were sealed" not in _statement(only_bad)


def test_an_event_with_no_window_anywhere_prints_unknown_window():
    reading = ws.read_artifact(
        json.dumps({"artifact": {"batches": [{"buckets": [{"events": [{"event_type": "TamperDetected"}]}]}]}}).encode()
    )
    assert reading.readable and reading.complete
    assert reading.events == [{"event_type": "TamperDetected", "zone_id": None, "start": None, "size": 600}]
    assert "  unknown window   unnamed zone   Tamper detected [tamper_detected]\n" in _statement(
        json.dumps({"artifact": {"batches": [{"buckets": [{"events": [{"event_type": "TamperDetected"}]}]}]}}).encode()
    )


def test_what_this_proves_claims_only_the_export_receipt_and_names_where_the_log_is():
    text = _statement()
    proves = text.split("What this proves\n----------------")[1].split("What this does not prove")[0]
    assert "the witness key signed a receipt over the exact bytes of the record above" in proves
    assert "chained to the witness's earlier export receipts" in proves
    assert "That log is not inside this zip" in proves
    assert "This file proves the export; the log proves the sealing." in proves
    assert "Tamper-evident: interference becomes visible; it is not prevented." in proves
    assert "chained to the claim before it" not in proves, "the per-event chain is not in the bundle"


def test_the_limits_and_the_recipe_are_always_present():
    for text in (_statement(), _statement(_bundle([])), _statement(b"garbage")):
        assert "What this proves\n----------------" in text
        assert "What this does not prove\n------------------------" in text
        assert "- What was in the video." in text
        assert "- Who or what it was." in text
        assert "- That nothing else happened." in text
        assert "- Whose hand held the key." in text
        assert "- That this is the only history." in text
        assert "How to check it\n---------------" in text
        assert "1. Open evidence_viewer.html from this zip" in text
        assert "4. For the formal kit (an RFC 3161 timestamp" in text
        assert "tamper-proof" not in text and "unforgeable" not in text


def test_the_footer_names_the_bundle_its_hash_and_the_version_and_disclaims_itself():
    text = _statement()
    digest = hashlib.sha256(BUNDLE).hexdigest()
    assert text.rstrip("\n").endswith(
        "This statement was written by the SecuraCV Home Assistant integration 0.9.0 "
        f"as a reading of securacv-events-1762394400.json (SHA-256 {digest}). It is not "
        "evidence and the checker ignores it; the sealed file is the evidence. "
        "Project: https://github.com/kmay89/securaCV"
    )
    assert text.endswith("\n") and "\r" not in text


def test_no_fingerprint_is_said_plainly():
    text = _statement(fingerprint=None)
    assert "Witness key fingerprint: not pinned when this pack was made." in text


def test_long_exports_are_capped_and_say_where_the_rest_is():
    buckets = [
        {
            "time_bucket": {"start_epoch_s": T0 + 600 * i, "size_s": 600},
            "events": [_event("BoundaryCrossingObjectLarge", "front_door", T0 + 600 * i)],
            "failures": [],
        }
        for i in range(ws.MAX_WINDOW_LINES + 7)
    ]
    text = _statement(_bundle(buckets))
    assert f"{ws.MAX_WINDOW_LINES + 7} sealed events in {ws.MAX_WINDOW_LINES + 7} ten-minute windows" in text
    assert "  ... and 7 more lines; every one is in securacv-events-1762394400.json" in text
    assert text.count("   front door   Person or vehicle") == ws.MAX_WINDOW_LINES
