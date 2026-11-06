"""WITNESS_STATEMENT.txt: the evidence pack read out in plain language.

What is pinned:
  - every number in the statement is read from the bundle's artifact (events
    by kind, zone and ten-minute window; the kernel's failure records), and
    the file names the bundle it was read from and that file's SHA-256;
  - the windows print in the owner's clock, never sharper than the bucket;
  - the labels are the dictionary's (const.EVENT_TYPE_METADATA), with the
    dictionary id beside each, and a type this version does not know prints
    as the kernel spelled it;
  - the chain state, the fingerprint and the export id are the caller's;
  - "what this does not prove" is always present, word for word on the four
    limits the Promise Card names;
  - a bundle with no events, or one this version cannot read, still yields a
    statement that says so rather than guessing;
  - an unreadable artifact never raises.
"""

from __future__ import annotations

import hashlib
import json
from datetime import datetime, timedelta, timezone

from . import conftest  # noqa: F401  (installs ha stubs at import time)

from .. import witness_statement as ws  # noqa: E402


def _bundle(buckets: list[dict]) -> bytes:
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
    assert "4 sealed events in 2 ten-minute windows between 2025-11-06 02:00 and 02:30 (UTC)." in text
    assert "By kind:\n      3  Person or vehicle [boundary_crossing_object_large]\n      1  Animal or small object [boundary_crossing_object_small]\n" in text
    assert "By zone:\n      2  front door\n      1  back yard\n      1  driveway\n" in text
    assert "  2025-11-06 02:00 to 02:10   front door   Person or vehicle [boundary_crossing_object_large] x2\n" in text
    assert "  2025-11-06 02:00 to 02:10   back yard   Animal or small object [boundary_crossing_object_small]\n" in text
    assert "  2025-11-06 02:20 to 02:30   driveway   Person or vehicle [boundary_crossing_object_large]\n" in text
    assert "System notes in this window: 1 failure record." in text
    assert "  2025-11-06 02:20 to 02:30   power loss (unclean shutdown)\n" in text


def test_the_windows_print_in_the_owner_s_clock_and_never_sharper_than_the_bucket():
    tz = timezone(timedelta(hours=-5), "EST")
    text = _statement(local=lambda s: datetime.fromtimestamp(s, tz=tz))
    assert "between 2025-11-05 21:00 and 21:30 (EST)." in text
    assert "  2025-11-05 21:00 to 21:10   front door" in text
    assert "Window by window (EST; windows are deliberately ten minutes wide):" in text
    assert ":00:" not in text and "02:03" not in text


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
    assert "  2025-11-06 02:00 to 02:10   NewFault\n" in text, "a failure with no window takes its bucket's"


def test_an_empty_or_unreadable_bundle_says_so_and_never_raises():
    empty = _statement(_bundle([]))
    assert "No events were sealed in this window (securacv-events-1762394400.json carries none)." in empty
    assert "That is not proof nothing happened" in empty
    broken = _statement(b"\x00not json")
    assert "No events were sealed in this window" in broken
    assert "SHA-256 " + hashlib.sha256(b"\x00not json").hexdigest() in broken
    not_a_dict = _statement(b"[1, 2, 3]")
    assert "No events were sealed" in not_a_dict


def test_the_limits_and_the_recipe_are_always_present():
    for text in (_statement(), _statement(_bundle([]))):
        assert "What this proves\n----------------" in text
        assert "tamper-evident: interference becomes visible; it is not prevented." in text
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


def test_read_artifact_tolerates_shapes_this_version_does_not_know():
    events, failures = ws.read_artifact(
        json.dumps(
            {
                "artifact": {
                    "batches": [
                        "not a batch",
                        {"buckets": ["not a bucket", {"events": "nope", "failures": None}]},
                        {"buckets": [{"time_bucket": {"start_epoch_s": "soon"}, "events": [{"event_type": 1}]}]},
                    ]
                }
            }
        ).encode()
    )
    assert failures == []
    assert events == [{"event_type": 1, "zone_id": None, "start": None, "size": 600}]
    assert "unknown window" in _statement(
        json.dumps({"artifact": {"batches": [{"buckets": [{"events": [{"event_type": "TamperDetected"}]}]}]}}).encode()
    )
