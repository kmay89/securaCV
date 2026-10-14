"""The integration's own chain walk — the only thing that earns "Verified".

What is pinned:

  - the math is byte-equal to the Rust kernel's and viewer/verify_core.js's:
    the shared fixture tests/fixtures/envelope/sealed_log_document_vector.json
    (served by the kernel, signed by its key) walks clean — every stored
    entry_hash is recomputed and every Ed25519 signature verifies under the
    document's key — and, when node is available, verify_core.js's own
    hashEntry / domainSeparatedHash produce the same bytes;
  - a tampered payload, a tampered signature and a broken link each stop
    the walk at the right index with the Rust VerifyFailure's wire kind;
  - a served key other than the pin is a KEY MISMATCH, never "failed";
  - the incremental walk: the cursor, continuity by prev_hash, the kernel
    ignoring from_seq, a re-anchored tail, a rewritten log, a regressed
    head, and the empty increment;
  - the KernelVerifier: it fetches with from_seq, persists the cursor and
    the covered index in its Store, fires the verification events, routes a
    foreign key through the coordinator (Repairs tofu_mismatch), arms the
    idle walk on new entries and the 15-minute backstop, and only a
    person's Verify now re-anchors after a regressed head;
  - the D10 attribute rule for event rows, and the C5 attribute table.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import types
from datetime import timedelta
from pathlib import Path

import pytest

from . import conftest  # noqa: F401  (installs ha stubs at import time)
from .conftest import dispatched, issues, make_entry, make_runtime, run

from homeassistant.core import HomeAssistant  # noqa: E402

from .. import kernel_verify as kv  # noqa: E402
from ..api import SecuraCVApiConnectionError  # noqa: E402
from ..const import (  # noqa: E402
    CONF_KERNEL_VERIFYING_KEY,
    DOMAIN,
    ISSUE_TOFU_MISMATCH,
    TRUST_REASON_HA_VERIFIED_CHAIN,
    TRUST_REASON_HA_VERIFY_FAILED,
    TRUST_REASON_KERNEL_KEY_MISMATCH,
    TRUST_REASON_KERNEL_SEALED,
    VERIFICATION_EVENT_CHAIN_FAILED,
    VERIFICATION_EVENT_CHAIN_VERIFIED,
    VERIFICATION_EVENT_KEY_CHANGED,
    VERIFICATION_FAILED,
    VERIFICATION_SEALED,
    VERIFICATION_VERIFIED,
    VERIFY_BACKSTOP_INTERVAL_SECONDS,
    VERIFY_IDLE_DELAY_SECONDS,
    VERIFY_TRIGGER_BUTTON,
    VERIFY_TRIGGER_IDLE,
    VERIFY_TRIGGER_SCHEDULED,
    VERIFY_TRIGGER_SERVICE,
)
from ..coordinator import (  # noqa: E402
    SecuraCVKernelCoordinator,
    chain_problem_signal,
    verification_event_signal,
    witness_event_signal,
)
from ..data import VerificationState  # noqa: E402

REPO = Path(__file__).resolve().parents[3]
VECTOR = REPO / "tests" / "fixtures" / "envelope" / "sealed_log_document_vector.json"
VERIFY_CORE = REPO / "viewer" / "verify_core.js"

# The HACS mirror (kmay89/securacv-homeassistant) carries this file without the
# monorepo's tests/fixtures tree, where the kernel's own sealed-log vector
# lives; there this module skips itself (the monorepo's CI runs it) instead of
# failing collection and taking the whole mirror suite with it.
if not VECTOR.exists():
    pytest.skip(
        f"needs the monorepo's {VECTOR.relative_to(REPO)} (absent in the HACS mirror; "
        "runs in kmay89/securaCV CI)",
        allow_module_level=True,
    )

DOC = json.loads(VECTOR.read_text(encoding="utf-8"))
KEY = DOC["verifying_key"]
OTHER_KEY = "cd" * 32
GENESIS = "00" * 32


def _doc(entries=None, **extra):
    """A served document: the fixture's key, the given entries (default all)."""
    doc = {"verifying_key": KEY, "entries": [dict(e) for e in (DOC["entries"] if entries is None else entries)]}
    doc.update(extra)
    return doc


def _with_seq(doc):
    """The amended wire: `seq` beside `id`, plus `head`."""
    for entry in doc["entries"]:
        entry["seq"] = entry["id"]
    if doc["entries"]:
        last = doc["entries"][-1]
        doc["head"] = {"seq": last["id"], "entry_hash": last["entry_hash"]}
    return doc


# ─── the math, against the kernel's own fixture ────────────────────────


def test_the_fixture_walks_clean_under_the_kernel_math():
    """Every stored entry_hash recomputes and every signature verifies: the
    Python port hashes and domain-separates exactly as the Rust kernel that
    produced the fixture."""
    result = kv.verify_document(_doc(), KEY, None)
    assert result.ok and result.failure is None
    assert result.verified == 3
    assert result.anchor == kv.ANCHOR_GENESIS
    assert result.last_seq == 3
    assert result.last_hash.hex() == DOC["entries"][-1]["entry_hash"]
    # Two events and a heartbeat: the heartbeat indexes nothing.
    assert result.covered == [
        ("boundary_crossing_object_large", "zone:a", 1700000400),
        ("boundary_crossing_object_large", "zone:b", 1700001000),
    ]


def test_the_primitives_match_the_fixture_byte_for_byte():
    first = DOC["entries"][0]
    computed = kv.hash_entry(bytes.fromhex(first["prev_hash"]), first["payload"])
    assert computed.hex() == first["entry_hash"]
    message = kv.domain_separated_hash(kv.DOMAIN_SEALED_LOG_ENTRY, computed)
    assert kv.verify_ed25519(bytes.fromhex(KEY), bytes.fromhex(first["signature"]), message)
    # One flipped bit anywhere and the signature no longer verifies.
    assert not kv.verify_ed25519(bytes.fromhex(KEY), bytes.fromhex(first["signature"]), message[:-1] + b"\x00")
    assert kv.le32(len(kv.DOMAIN_SEALED_LOG_ENTRY)) == len(kv.DOMAIN_SEALED_LOG_ENTRY).to_bytes(4, "little")


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_primitives_match_verify_core_js():
    """verify_core.js's hashEntry and domainSeparatedHash over the fixture,
    compared with the Python port: the two verifiers are one algorithm."""
    script = """
const core = require(process.argv[1]);
const doc = JSON.parse(require('fs').readFileSync(process.argv[2], 'utf8'));
(async () => {
  const out = [];
  for (const e of doc.entries) {
    const h = await core.hashEntry(core.hexToBytes(e.prev_hash), e.payload);
    const m = await core.domainSeparatedHash(core.V1_MANIFEST.signature_domains.sealed_log_entry, h);
    out.push([core.bytesToHex(h), core.bytesToHex(m)]);
  }
  process.stdout.write(JSON.stringify(out));
})();
"""
    proc = subprocess.run(
        ["node", "-e", script, str(VERIFY_CORE), str(VECTOR)],
        capture_output=True, text=True, check=True, timeout=60,
    )
    js = json.loads(proc.stdout)
    assert core_domain() == kv.DOMAIN_SEALED_LOG_ENTRY
    for entry, (js_hash, js_message) in zip(DOC["entries"], js):
        py_hash = kv.hash_entry(bytes.fromhex(entry["prev_hash"]), entry["payload"])
        assert py_hash.hex() == js_hash == entry["entry_hash"]
        assert kv.domain_separated_hash(kv.DOMAIN_SEALED_LOG_ENTRY, py_hash).hex() == js_message


def core_domain() -> str:
    """The sealed-log signing domain as verify_core.js spells it."""
    text = VERIFY_CORE.read_text(encoding="utf-8")
    start = text.index("sealed_log_entry: '") + len("sealed_log_entry: '")
    return text[start : text.index("'", start)]


# ─── failures stop the walk where the fault is ─────────────────────────


def test_a_tampered_payload_is_an_entry_hash_mismatch_at_its_index():
    doc = _doc()
    doc["entries"][1]["payload"] = doc["entries"][1]["payload"].replace("zone:b", "zone:x")
    result = kv.verify_document(doc, KEY, None)
    assert not result.ok
    assert result.failure.kind == kv.FAIL_ENTRY_HASH
    assert result.failure.index == 1 and result.failure.seq == 2
    assert result.verified == 1, "what came before the fault stays verified"
    assert result.covered == [("boundary_crossing_object_large", "zone:a", 1700000400)]


def test_a_tampered_signature_is_a_signature_mismatch():
    doc = _doc()
    sig = doc["entries"][2]["signature"]
    doc["entries"][2]["signature"] = ("00" if sig[:2] != "00" else "11") + sig[2:]
    result = kv.verify_document(doc, KEY, None)
    assert not result.ok and result.failure.kind == kv.FAIL_SIGNATURE
    assert result.failure.index == 2 and result.verified == 2


def test_a_broken_link_is_a_prev_hash_mismatch():
    doc = _doc()
    doc["entries"][2]["prev_hash"] = "ab" * 32
    result = kv.verify_document(doc, KEY, None)
    assert not result.ok and result.failure.kind == kv.FAIL_PREV_HASH
    assert result.failure.index == 2


def test_a_reordered_or_dropped_entry_breaks_the_chain():
    doc = _doc(entries=[DOC["entries"][0], DOC["entries"][2]])
    result = kv.verify_document(doc, KEY, None)
    assert not result.ok and result.failure.kind == kv.FAIL_PREV_HASH and result.failure.index == 1


def test_a_malformed_entry_is_named_not_crashed():
    doc = _doc()
    doc["entries"][0]["entry_hash"] = "zz"
    result = kv.verify_document(doc, KEY, None)
    assert not result.ok and result.failure.kind == kv.FAIL_MALFORMED and result.failure.index == 0


def test_a_served_key_other_than_the_pin_is_a_key_mismatch_not_a_failure():
    result = kv.verify_document(_doc(), OTHER_KEY, None)
    assert not result.ok and result.key_mismatch
    assert result.failure.kind == kv.FAIL_KEY_MISMATCH
    assert result.served_key == KEY
    assert result.verified == 0, "nothing is verified under a key that was never pinned"


def test_a_document_without_a_key_is_refused():
    result = kv.verify_document({"entries": []}, KEY, None)
    assert not result.ok and result.failure.kind == kv.FAIL_NO_KEY


def test_an_empty_log_from_genesis_verifies_nothing_and_is_fine():
    result = kv.verify_document(_doc(entries=[]), KEY, None)
    assert result.ok and result.verified == 0 and result.last_seq is None


def test_the_walk_starts_at_the_checkpoint_head_when_the_kernel_serves_one():
    """The kernel anchors the served tail at its latest checkpoint (or the
    size-cap boundary): the first entry links to that hash, not genesis."""
    doc = _doc(entries=DOC["entries"][1:], checkpoint_head=DOC["entries"][0]["entry_hash"])
    result = kv.verify_document(doc, KEY, None)
    assert result.ok and result.verified == 2 and result.anchor == kv.ANCHOR_CHECKPOINT
    # The same tail claiming to start at genesis does not link.
    assert not kv.verify_document(_doc(entries=DOC["entries"][1:]), KEY, None).ok


# ─── the incremental walk ──────────────────────────────────────────────


def _cursor(index: int, verified: int | None = None) -> kv.Cursor:
    entry = DOC["entries"][index]
    return kv.Cursor(
        seq=entry["id"],
        entry_hash=bytes.fromhex(entry["entry_hash"]),
        verified_entries=verified if verified is not None else index + 1,
    )


def test_the_increment_continues_from_the_cursor_hash():
    doc = _with_seq(_doc(entries=DOC["entries"][1:]))
    doc["from_seq"] = 1
    result = kv.verify_document(doc, KEY, _cursor(0))
    assert result.ok and result.anchor == kv.ANCHOR_CONTINUITY
    assert result.verified == 2 and result.last_seq == 3 and result.head_seq == 3


def test_a_kernel_that_ignores_from_seq_serves_rows_already_verified():
    """An older kernel answers the full tail; rows at or below the cursor
    were verified on earlier walks and are skipped, continuity still holds."""
    result = kv.verify_document(_with_seq(_doc()), KEY, _cursor(0))
    assert result.ok and result.anchor == kv.ANCHOR_CONTINUITY and result.verified == 2


def test_nothing_new_is_an_unchanged_walk_that_keeps_the_cursor():
    doc = _with_seq(_doc())
    doc["entries"] = []
    result = kv.verify_document(doc, KEY, _cursor(2))
    assert result.ok and result.anchor == kv.ANCHOR_UNCHANGED and result.verified == 0
    assert result.last_seq == 3 and result.last_hash.hex() == DOC["entries"][2]["entry_hash"]


def test_a_head_behind_the_cursor_is_a_regressed_log():
    doc = {"verifying_key": KEY, "entries": [], "head": {"seq": 1, "entry_hash": DOC["entries"][0]["entry_hash"]}}
    result = kv.verify_document(doc, KEY, _cursor(2))
    assert not result.ok and result.failure.kind == kv.FAIL_HEAD_REGRESSED
    assert "replaced or restored" in result.failure.message


def test_a_head_at_the_cursor_with_another_hash_is_a_rewritten_log():
    doc = {"verifying_key": KEY, "entries": [], "head": {"seq": 3, "entry_hash": "ab" * 32}}
    result = kv.verify_document(doc, KEY, _cursor(2))
    assert not result.ok and result.failure.kind == kv.FAIL_HEAD_MISMATCH


def test_a_rewritten_entry_after_the_cursor_fails_continuity():
    doc = _with_seq(_doc(entries=DOC["entries"][1:]))
    doc["entries"][0]["prev_hash"] = "ab" * 32
    result = kv.verify_document(doc, KEY, _cursor(0))
    assert not result.ok and result.failure.kind == kv.FAIL_PREV_HASH
    assert "rewritten at the cursor" in result.failure.message


def test_a_tail_re_anchored_past_the_cursor_verifies_but_says_so():
    """A checkpoint (or the size cap) moved the kernel's anchor past the
    cursor: the served rows link to the anchor, verify, and the gap is the
    kernel's continuity claim (`anchor_moved`), not this walk's proof."""
    doc = _with_seq(_doc(entries=DOC["entries"][2:], checkpoint_head=DOC["entries"][1]["entry_hash"]))
    result = kv.verify_document(doc, KEY, _cursor(0))
    assert result.ok and result.anchor == kv.ANCHOR_REANCHORED and result.verified == 1


def test_a_declared_head_that_disagrees_with_the_verified_row_fails():
    doc = _with_seq(_doc())
    doc["head"]["entry_hash"] = "ab" * 32
    result = kv.verify_document(doc, KEY, None)
    assert not result.ok and result.failure.kind == kv.FAIL_HEAD_MISMATCH


# ─── the D10 attribute rule ────────────────────────────────────────────

EVENT = {
    "event_type": "BoundaryCrossingObjectLarge",
    "zone_id": "zone:a",
    "time_bucket": {"start_epoch_s": 1700000400, "size_s": 600},
    "confidence": 0.9,
    "attestation": "adapter",
    "kernel_version": "2.4.16",
    "ruleset_id": "ruleset:frigate_v1",
    "device_id": "pwk_1a2b3c4d",
    "seq": 1,
    "verified_ok": True,
    "verified_bucket_start": 1700000000,
}


KEY_A = ("boundary_crossing_object_large", "zone:a", 1700000400)


def test_verified_iff_the_walk_covered_the_event_s_own_entry():
    """A row with `seq` is Verified by the seq index alone: the walk must have
    verified THAT entry, with the row's own key."""
    state = VerificationState(ok=True, covered={KEY_A}, covered_seqs={1: KEY_A})
    assert kv.event_verification(state, EVENT) == (VERIFICATION_VERIFIED, TRUST_REASON_HA_VERIFIED_CHAIN)
    other = dict(EVENT, zone_id="zone:b")
    assert kv.event_verification(state, other) == (VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED)
    later = dict(EVENT, time_bucket={"start_epoch_s": 1700001000, "size_s": 600})
    assert kv.event_verification(state, later) == (VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED)
    # The same key under another seq is another entry: not this one's proof.
    unwalked = dict(EVENT, seq=2)
    assert kv.event_verification(state, unwalked) == (VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED)
    # A seq the walk verified under a different key is not this row either.
    assert kv.event_verification(VerificationState(ok=True, covered_seqs={1: ("tamper_detected", "zone:a", 1700000400)}), EVENT) == (
        VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED)


def test_two_entries_sharing_one_key_are_verified_one_by_one():
    """A dog and a cat in one zone in one bucket: two sealed entries, one
    (event_type, zone_id, bucket) key. Walking the dog's entry must not make
    the cat's row read Verified before the cat's signature is walked."""
    key = ("boundary_crossing_object_small", "zone:yard", 1700000400)
    dog = dict(EVENT, event_type="BoundaryCrossingObjectSmall", zone_id="zone:yard", seq=10)
    cat = dict(dog, seq=11)
    after_dog = VerificationState(
        ok=True, verified_seq=10, head_seq=10, verified_bucket_start=1700001200,
        covered={key}, covered_seqs={10: key},
    )
    assert kv.event_verification(after_dog, dog) == (VERIFICATION_VERIFIED, TRUST_REASON_HA_VERIFIED_CHAIN)
    assert kv.event_verification(after_dog, cat) == (VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED)
    after_cat = VerificationState(
        ok=True, verified_seq=11, head_seq=11, verified_bucket_start=1700001200,
        covered={key}, covered_seqs={10: key, 11: key},
    )
    assert kv.event_verification(after_cat, cat) == (VERIFICATION_VERIFIED, TRUST_REASON_HA_VERIFIED_CHAIN)
    assert kv.event_verification(after_cat, dog) == (VERIFICATION_VERIFIED, TRUST_REASON_HA_VERIFIED_CHAIN)


def test_a_row_without_seq_is_verified_only_once_its_bucket_was_walked_closed():
    """A key-only row (an older bridge, no `seq`) cannot name its entry, so it
    is Verified only when the walk began after its bucket closed and reached
    the head — then every entry of that bucket was walked, this one included."""
    key_only = {k: v for k, v in EVENT.items() if k != "seq"}
    walked_open = VerificationState(
        ok=True, verified_seq=3, head_seq=3, verified_bucket_start=1700000400,
        covered={KEY_A}, covered_seqs={1: KEY_A},
    )
    assert kv.event_verification(walked_open, key_only) == (VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED), (
        "the walk ran in the row's own bucket: a later entry with this key may still be unwalked")
    walked_closed = VerificationState(
        ok=True, verified_seq=3, head_seq=3, verified_bucket_start=1700001000,
        covered={KEY_A}, covered_seqs={1: KEY_A},
    )
    assert kv.event_verification(walked_closed, key_only) == (VERIFICATION_VERIFIED, TRUST_REASON_HA_VERIFIED_CHAIN)
    # The walk stopped short of the kernel's declared head: not every entry
    # of the bucket was walked.
    short = VerificationState(
        ok=True, verified_seq=3, head_seq=5, verified_bucket_start=1700001000,
        covered={KEY_A}, covered_seqs={1: KEY_A},
    )
    assert kv.event_verification(short, key_only) == (VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED)
    # A flat row carries its own bucket size; a size the wire does not state
    # is the kernel's 600 s.
    flat = {"event_type": "BoundaryCrossingObjectLarge", "zone_id": "zone:a",
            "time_bucket_start": 1700000400, "time_bucket_size": 1200}
    assert kv.event_verification(walked_closed, flat) == (VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED)
    assert kv.event_verification(
        VerificationState(ok=True, verified_seq=3, head_seq=3, verified_bucket_start=1700001600,
                          covered={KEY_A}, covered_seqs={1: KEY_A}), flat,
    ) == (VERIFICATION_VERIFIED, TRUST_REASON_HA_VERIFIED_CHAIN)


def test_a_kernel_self_report_never_makes_an_event_verified():
    """`verified_ok: true` on the wire is the kernel's own check; with no
    walk it stays Sealed (the badge the kernel cannot award itself)."""
    assert kv.event_verification(VerificationState(), EVENT) == (VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED)
    assert kv.event_verification(None, EVENT) == (VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED)


def test_a_failed_walk_renders_every_row_failed_and_a_key_change_is_never_failed():
    failed = VerificationState(ok=False, covered={("boundary_crossing_object_large", "zone:a", 1700000400)})
    assert kv.event_verification(failed, EVENT) == (VERIFICATION_FAILED, TRUST_REASON_HA_VERIFY_FAILED)
    # D10 / C5: a key change is Repairs tofu_mismatch, never "Failed". The
    # row stays Sealed (it IS sealed) and the reason names the key, which
    # is what the card's tooltip and the strategy header key on; it wins
    # over a covered index and over a failed walk alike.
    mismatch = VerificationState(ok=True, key_mismatch=True, covered=failed.covered)
    assert kv.event_verification(mismatch, EVENT) == (VERIFICATION_SEALED, TRUST_REASON_KERNEL_KEY_MISMATCH)
    assert kv.event_verification(VerificationState(ok=False, key_mismatch=True), EVENT) == (
        VERIFICATION_SEALED, TRUST_REASON_KERNEL_KEY_MISMATCH)
    attrs = kv.witness_event_attributes(
        EVENT, mismatch, kernel_device_id=None, kernel_reported_chain_ok=None, kernel_verified_bucket_start=None,
    )
    assert attrs["verification"] == "sealed" and attrs["verified"] is False
    assert attrs["trust_reason"] == "kernel_key_mismatch"


def test_chain_trust_reason_speaks_the_rows_vocabulary():
    assert kv.chain_trust_reason(None) is None
    assert kv.chain_trust_reason(VerificationState()) is None
    assert kv.chain_trust_reason(VerificationState(ok=True)) == TRUST_REASON_HA_VERIFIED_CHAIN
    assert kv.chain_trust_reason(VerificationState(ok=False)) == TRUST_REASON_HA_VERIFY_FAILED
    for ok in (None, True, False):
        assert kv.chain_trust_reason(VerificationState(ok=ok, key_mismatch=True)) == TRUST_REASON_KERNEL_KEY_MISMATCH


def test_the_c5_attribute_table_exactly():
    state = VerificationState(
        ok=True, verified_bucket_start=1700001200, verified_seq=3, head_seq=3,
        covered={("boundary_crossing_object_large", "zone:a", 1700000400)},
        covered_seqs={1: ("boundary_crossing_object_large", "zone:a", 1700000400)},
    )
    attrs = kv.witness_event_attributes(
        EVENT, state, kernel_device_id="pwk_ffffffff",
        kernel_reported_chain_ok=False, kernel_verified_bucket_start=1699999000,
    )
    assert attrs == {
        "zone_id": "zone:a",
        "zone": "a",
        "time_bucket": {"start_epoch_s": 1700000400, "size_s": 600},
        "confidence": 0.9,
        "attestation": "adapter",
        "friendly_event": "Person or vehicle",
        "kernel_version": "2.4.16",
        "ruleset_id": "ruleset:frigate_v1",
        "kernel_device_id": "pwk_1a2b3c4d",
        "verification": "verified",
        "verified": True,
        "trust_reason": "ha_verified_chain",
        "verified_bucket_start": 1700001200,
        "signed": True,
        # The wire's own last-verify fields map to the kernel-reported pair.
        "kernel_reported_chain_ok": True,
        "kernel_verified_bucket_start": 1700000000,
        "seq": 1,
    }
    # Flattened buckets are nested; a wire without the kernel's report falls
    # back to the coordinator's; the sealed row never says verified.
    flat = {"event_type": "AcousticImpulseInZone", "zone_id": "zone:yard",
            "time_bucket_start": 1700000400, "time_bucket_size": 600, "verified_at": 1699999500}
    attrs = kv.witness_event_attributes(
        flat, state, kernel_device_id="pwk_ffffffff", kernel_reported_chain_ok=False,
        kernel_verified_bucket_start=1699999000,
    )
    assert attrs["time_bucket"] == {"start_epoch_s": 1700000400, "size_s": 600}
    assert attrs["kernel_device_id"] == "pwk_ffffffff"
    assert attrs["kernel_reported_chain_ok"] is False
    assert attrs["kernel_verified_bucket_start"] == 1699999500
    assert attrs["verification"] == "sealed" and attrs["verified"] is False
    assert attrs["verified_bucket_start"] is None and "seq" not in attrs
    assert attrs["attestation"] == "device"


def test_covered_keys_fold_the_wire_spelling_to_the_dictionary_id():
    assert kv.covered_key_of_event(EVENT) == ("boundary_crossing_object_large", "zone:a", 1700000400)
    assert kv.covered_key_of_payload(DOC["entries"][2]["payload"]) is None, "a heartbeat is not an event"
    assert kv.covered_key_of_payload("not json") is None


# Every bucket-start draw the kernel's export jitter can make (src/lib.rs
# jitter_time_bucket: -steps..=steps times 60 s, steps = 120 / 60).
JITTER_OFFSETS = (-120, -60, 0, 60, 120)


def _jittered(event: dict, offset: int, *, flat: bool = False) -> dict:
    """The C5 row of a sealed event, as the bridge publishes one export draw."""
    start = event["time_bucket"]["start_epoch_s"] + offset
    if flat:
        row = {k: v for k, v in event.items() if k != "time_bucket"}
        return dict(row, time_bucket_start=start, time_bucket_size=600)
    return dict(event, time_bucket={"start_epoch_s": start, "size_s": 600})


@pytest.mark.parametrize("offset", JITTER_OFFSETS)
@pytest.mark.parametrize("flat", [False, True])
def test_a_jittered_wire_row_matches_the_verified_entry_s_true_bucket(offset, flat):
    """The sealed log carries the true bucket; the wire row carries the
    kernel's export draw. The covered index and the lookup meet on the
    canonical start, so every draw of a verified entry reads Verified."""
    covered = {kv.covered_key_of_payload(DOC["entries"][0]["payload"])}
    assert covered == {("boundary_crossing_object_large", "zone:a", 1700000400)}
    state = VerificationState(
        ok=True, verified_bucket_start=1700001200, covered=covered,
        covered_seqs={1: ("boundary_crossing_object_large", "zone:a", 1700000400)},
    )
    row = _jittered(EVENT, offset, flat=flat)
    assert kv.covered_key_of_event(row) == ("boundary_crossing_object_large", "zone:a", 1700000400)
    assert kv.event_verification(state, row) == (VERIFICATION_VERIFIED, TRUST_REASON_HA_VERIFIED_CHAIN)
    attrs = kv.witness_event_attributes(
        row, state, kernel_device_id=None, kernel_reported_chain_ok=None, kernel_verified_bucket_start=None,
    )
    assert attrs["verification"] == "verified" and attrs["verified"] is True
    # The display window is the wire's own; only the identity is canonical.
    assert attrs["time_bucket"] == {"start_epoch_s": 1700000400 + offset, "size_s": 600}
    # The neighboring true bucket, drawn towards this one, is still another entry.
    neighbor = _jittered(dict(EVENT, time_bucket={"start_epoch_s": 1700001000, "size_s": 600}), -120, flat=flat)
    assert kv.event_verification(state, neighbor) == (VERIFICATION_SEALED, TRUST_REASON_KERNEL_SEALED)


@pytest.mark.parametrize("offset", JITTER_OFFSETS)
def test_the_walk_s_covered_index_verifies_every_draw_of_its_rows(monkeypatch, offset):
    """End to end: the walk indexes the sealed log's true buckets, and a
    wire row under any export draw of those buckets is covered."""
    import time as _time

    monkeypatch.setattr(_time, "time", lambda: 1700002000.0)
    _hass, _entry, _api, _coordinator, verifier, _events = _verifier([_with_seq(_doc())])
    state = run(verifier.async_verify(VERIFY_TRIGGER_SCHEDULED))
    assert state.ok is True
    for entry_payload in (DOC["entries"][0]["payload"], DOC["entries"][1]["payload"]):
        sealed = json.loads(entry_payload)
        row = _jittered(sealed, offset)
        assert kv.event_verification(state, row)[0] == VERIFICATION_VERIFIED, (offset, sealed["zone_id"])


# ─── the verifier ──────────────────────────────────────────────────────

IDENTITY = {"device_id": "pwk_1a2b3c4d", "verifying_key": KEY, "kernel_version": "2.4.16"}


class FakeApi:
    base_url = "http://kernel:8799"
    token_file_unreadable = False

    def __init__(self, docs) -> None:
        self.docs = list(docs)
        self.from_seqs: list[int | None] = []

    async def async_get_sealed_log(self, from_seq=None):
        self.from_seqs.append(from_seq)
        doc = self.docs.pop(0) if len(self.docs) > 1 else self.docs[0]
        if isinstance(doc, Exception):
            raise doc
        return json.loads(json.dumps(doc))

    async def async_get_health(self):
        return {"ok": True}

    async def async_get_identity(self):
        return dict(IDENTITY)

    async def async_get_status(self):
        return None

    async def async_get_latest_event(self):
        return None

    async def async_get_digest(self):
        return None


def _verifier(docs, *, pinned=KEY, store_payload=None):
    hass = HomeAssistant()
    hass.data = {}
    entry = make_entry(hass, data={CONF_KERNEL_VERIFYING_KEY: pinned} if pinned else {})
    api = FakeApi(docs)
    coordinator = SecuraCVKernelCoordinator(
        hass, entry, api, auth_mode="paired", topic_prefix="witness", pinned_key=pinned
    )
    entry.runtime_data = make_runtime(hass, coordinator=coordinator, api=api)
    verifier = kv.KernelVerifier(hass, entry, coordinator, api)
    if store_payload is not None:
        verifier._store._payload = json.loads(json.dumps(store_payload))
    entry.runtime_data.verifier = verifier
    events: list = []
    from homeassistant.helpers.dispatcher import async_dispatcher_connect

    async_dispatcher_connect(
        hass, verification_event_signal(entry.entry_id),
        lambda event_type, attrs: events.append((event_type, attrs)),
    )
    return hass, entry, api, coordinator, verifier, events


def test_the_first_walk_verifies_from_the_anchor_and_persists_the_cursor(monkeypatch):
    import time as _time

    # The fixture's buckets are from 2023; freeze the clock beside them so
    # the retention bound (tested on its own below) keeps them indexed.
    monkeypatch.setattr(_time, "time", lambda: 1700002000.0)
    hass, entry, api, _coordinator, verifier, events = _verifier([_with_seq(_doc())])
    state = run(verifier.async_verify(VERIFY_TRIGGER_SCHEDULED))
    assert api.from_seqs == [None]
    assert state.ok is True and state.error is None
    assert state.verified_seq == 3 and state.verified_entries == 3 and state.walk_entries == 3
    assert state.head_seq == 3 and state.head_hash == DOC["entries"][2]["entry_hash"]
    assert state.verified_bucket_start is not None and state.verified_bucket_start % 600 == 0
    assert state.last_trigger == VERIFY_TRIGGER_SCHEDULED and state.verifying is False
    assert state.covered == {
        ("boundary_crossing_object_large", "zone:a", 1700000400),
        ("boundary_crossing_object_large", "zone:b", 1700001000),
    }
    assert state.covered_seqs == {
        1: ("boundary_crossing_object_large", "zone:a", 1700000400),
        2: ("boundary_crossing_object_large", "zone:b", 1700001000),
    }
    assert state.verified_bucket_start == 1700001600
    stored = verifier._store._payload
    assert stored["v"] == 1 and stored["verified_seq"] == 3
    assert stored["verified_hash"] == DOC["entries"][2]["entry_hash"]
    assert stored["verified_entries"] == 3
    assert stored["covered"] == [
        ["boundary_crossing_object_large", "zone:a", 1700000400, 1],
        ["boundary_crossing_object_large", "zone:b", 1700001000, 2],
    ], "the covered index, seq included, survives a restart"
    assert events[-1][0] == VERIFICATION_EVENT_CHAIN_VERIFIED
    attrs = events[-1][1]
    assert attrs["chain_valid"] is True and attrs["events_verified"] == 3
    assert attrs["trigger"] == VERIFY_TRIGGER_SCHEDULED and attrs["verified_seq"] == 3
    assert attrs["verified_bucket_size"] == 600


def test_the_covered_index_is_bounded_by_retention(monkeypatch):
    """The fixture's buckets are from 2023; with retention they are gone,
    with a long retention they stay. Either way the index never grows past
    the retention window."""
    import time as _time

    hass, entry, _api, coordinator, verifier, _events = _verifier([_with_seq(_doc())])
    monkeypatch.setattr(_time, "time", lambda: 1700002000.0)
    run(verifier.async_verify(VERIFY_TRIGGER_SCHEDULED))
    assert len(verifier.state.covered) == 2
    coordinator.data["digest"] = {"retention_days": 1}
    monkeypatch.setattr(_time, "time", lambda: 1700002000.0 + 2 * 86400)
    run(verifier.async_verify(VERIFY_TRIGGER_SCHEDULED))
    assert verifier.state.covered == set()


def test_the_second_walk_is_incremental_and_the_cursor_survives_a_reload():
    hass, entry, api, _c, verifier, events = _verifier([_with_seq(_doc())])
    run(verifier.async_verify(VERIFY_TRIGGER_SCHEDULED))
    stored = verifier._store._payload

    # A new verifier (a reload) loads the cursor and asks for the increment.
    hass2, entry2, api2, _c2, verifier2, events2 = _verifier(
        [{"verifying_key": KEY, "entries": [], "head": {"seq": 3, "entry_hash": DOC["entries"][2]["entry_hash"]}, "from_seq": 3}],
        store_payload=stored,
    )
    run(verifier2._async_load())
    assert verifier2.cursor == kv.Cursor(3, bytes.fromhex(DOC["entries"][2]["entry_hash"]), 3)
    assert verifier2.state.verified_seq == 3
    state = run(verifier2.async_verify(VERIFY_TRIGGER_IDLE))
    assert api2.from_seqs == [3]
    assert state.ok is True and state.walk_entries == 0 and state.verified_entries == 3
    assert events2[-1][1]["verdict"] == "chain intact; no new entries since the last walk"


def test_a_failed_walk_keeps_the_cursor_and_fires_the_failure():
    bad = _with_seq(_doc())
    bad["entries"][1]["payload"] = bad["entries"][1]["payload"].replace("zone:b", "zone:x")
    hass, entry, api, _c, verifier, events = _verifier([bad])
    state = run(verifier.async_verify(VERIFY_TRIGGER_BUTTON))
    assert state.ok is False
    assert "entry_hash mismatch" in state.error and state.failed_seq == 2
    assert state.walk_entries == 1
    assert verifier.cursor is None, "a failed walk moves no cursor"
    assert verifier._store._payload is None, "and persists nothing"
    assert events[-1][0] == VERIFICATION_EVENT_CHAIN_FAILED
    assert events[-1][1]["error"] == kv.FAIL_ENTRY_HASH and events[-1][1]["failed_seq"] == 2
    assert events[-1][1]["trigger"] == VERIFY_TRIGGER_BUTTON


def test_a_foreign_key_goes_through_the_coordinator_to_repairs_never_failed():
    hass, entry, api, coordinator, verifier, events = _verifier([_with_seq(_doc())], pinned=OTHER_KEY)
    state = run(verifier.async_verify(VERIFY_TRIGGER_SERVICE))
    assert state.key_mismatch is True
    assert state.ok is None, "a key change is not a chain failure"
    assert state.error == "the witness key changed; confirm it in Settings → Repairs"
    assert kv.chain_trust_reason(state) == TRUST_REASON_KERNEL_KEY_MISMATCH
    assert coordinator.key_mismatch is True
    assert (DOMAIN, f"{ISSUE_TOFU_MISMATCH}_{entry.entry_id}") in issues(hass)
    assert [e[0] for e in events] == [VERIFICATION_EVENT_KEY_CHANGED]
    assert events[0][1]["pinned_fingerprint"] == coordinator.pinned_fingerprint
    # While the mismatch stands, walks do not run against the wrong key.
    run(verifier.async_verify(VERIFY_TRIGGER_SCHEDULED))
    assert api.from_seqs == [None], "no second fetch while the pin is disputed"


def test_without_a_pin_the_walk_waits_and_says_so():
    hass, entry, api, _c, verifier, events = _verifier([_with_seq(_doc())], pinned=None)
    state = run(verifier.async_verify(VERIFY_TRIGGER_SCHEDULED))
    assert state.ok is None and "no witness key pinned" in state.error
    assert api.from_seqs == [] and events == []


def test_only_a_person_re_anchors_after_a_regressed_head():
    regressed = {"verifying_key": KEY, "entries": [], "head": {"seq": 1, "entry_hash": DOC["entries"][0]["entry_hash"]}}
    hass, entry, api, _c, verifier, events = _verifier(
        [regressed, regressed, _with_seq(_doc())],
        store_payload={"v": 1, "verified_seq": 3, "verified_hash": DOC["entries"][2]["entry_hash"], "verified_entries": 3, "covered": []},
    )
    run(verifier._async_load())
    state = run(verifier.async_verify(VERIFY_TRIGGER_SCHEDULED))
    assert state.ok is False and "replaced or restored" in state.error
    # The backstop keeps asking from the cursor and keeps failing.
    run(verifier.async_verify(VERIFY_TRIGGER_IDLE))
    assert api.from_seqs == [3, 3]
    # A person's Verify now walks from the document's own anchor again.
    state = run(verifier.async_verify(VERIFY_TRIGGER_BUTTON))
    assert api.from_seqs == [3, 3, None]
    assert state.ok is True and state.verified_entries == 3


def test_an_unreachable_kernel_leaves_the_verdict_alone_and_raises_for_a_caller():
    hass, entry, api, _c, verifier, events = _verifier([_with_seq(_doc()), SecuraCVApiConnectionError("x")])
    run(verifier.async_verify(VERIFY_TRIGGER_SCHEDULED))
    with pytest.raises(SecuraCVApiConnectionError):
        run(verifier.async_verify(VERIFY_TRIGGER_SERVICE))
    assert verifier.state.ok is True, "no answer is not a failed chain"
    assert verifier.state.verifying is False
    # The background wrapper never raises.
    api.docs = [SecuraCVApiConnectionError("x")]
    run(verifier.async_run(VERIFY_TRIGGER_SCHEDULED))


def test_start_arms_the_backstop_the_idle_walk_and_the_bridge_listener(monkeypatch):
    from homeassistant.helpers import event as event_helpers

    intervals: list = []
    laters: list = []
    cancels: list[str] = []

    def _interval(hass, action, interval, **kw):
        intervals.append((action, interval))
        return lambda: cancels.append("interval")

    def _later(hass, delay, action):
        laters.append((delay, action))
        return lambda: cancels.append("later")

    monkeypatch.setattr(event_helpers, "async_track_time_interval", _interval)
    monkeypatch.setattr(event_helpers, "async_call_later", _later)
    monkeypatch.setattr(kv, "async_track_time_interval", _interval)
    monkeypatch.setattr(kv, "async_call_later", _later)

    hass, entry, api, coordinator, verifier, events = _verifier([_with_seq(_doc())])
    run(verifier.async_start())
    assert intervals and intervals[0][1] == timedelta(seconds=VERIFY_BACKSTOP_INTERVAL_SECONDS)
    assert dispatched(hass, witness_event_signal(entry.entry_id))
    assert dispatched(hass, chain_problem_signal(entry.entry_id))

    # New entries (the live feed) arm the idle walk; another entry re-arms it.
    for listener in dispatched(hass, witness_event_signal(entry.entry_id)):
        listener({"event_type": "x"})
    assert [delay for delay, _a in laters] == [VERIFY_IDLE_DELAY_SECONDS]
    for listener in dispatched(hass, witness_event_signal(entry.entry_id)):
        listener({"event_type": "y"})
    assert len(laters) == 2 and cancels == ["later"]

    # The bridge clearing its own chain problem arms a walk (never a verdict);
    # the bridge raising one is surfaced at once as a kernel-reported failure.
    for listener in dispatched(hass, chain_problem_signal(entry.entry_id)):
        listener(False)
    assert len(laters) == 3
    for listener in dispatched(hass, chain_problem_signal(entry.entry_id)):
        listener(True)
    assert events[-1][0] == VERIFICATION_EVENT_CHAIN_FAILED
    assert events[-1][1]["kernel_reported"] is True and events[-1][1]["trigger"] == "bridge"

    verifier.async_stop()
    assert "interval" in cancels and cancels.count("later") == 3
    assert dispatched(hass, witness_event_signal(entry.entry_id)) == []
    verifier.async_stop()  # idempotent


def test_verification_response_shapes_every_outcome():
    ok = VerificationState(ok=True, walk_entries=2, verified_entries=5, verified_seq=5,
                           verified_bucket_start=1700000400)
    response = kv.verification_response(ok, "pwk_1a2b3c4d")
    assert response["chain_valid"] is True and response["error"] is None
    assert response["events_verified"] == 2 and response["verified_entries"] == 5
    assert response["kernel_device_id"] == "pwk_1a2b3c4d"
    assert kv.verification_response(VerificationState(ok=True), None)["verdict"].startswith("chain intact")
    failed = kv.verification_response(VerificationState(ok=False, error="boom"), None)
    assert failed["chain_valid"] is False and failed["verdict"] == "boom" and failed["error"] == "boom"
    changed = kv.verification_response(VerificationState(key_mismatch=True), None)
    assert changed["key_mismatch"] is True and "Repairs" in changed["verdict"]
    assert kv.verification_response(VerificationState(), None)["verdict"] == "not verified yet"


def test_the_store_key_is_per_entry_and_junk_in_it_is_ignored():
    assert kv.storage_key("e1") == "securacv.verify_e1"
    hass, entry, api, _c, verifier, _events = _verifier(
        [_with_seq(_doc())], store_payload={"v": 7, "verified_seq": "three"}
    )
    run(verifier._async_load())
    assert verifier.cursor is None
    hass, entry, api, _c, verifier, _events = _verifier(
        [_with_seq(_doc())],
        store_payload={"v": 1, "verified_seq": 2, "verified_hash": "zz", "covered": [["a"], "junk", ["t", "z", 1]]},
    )
    run(verifier._async_load())
    assert verifier.cursor is None, "an unreadable hash means no cursor"


def test_async_verify_entry_needs_a_verifier():
    hass = HomeAssistant()
    hass.data = {}
    entry = make_entry(hass)
    with pytest.raises(ValueError):
        run(kv.async_verify_entry(hass, entry, trigger=VERIFY_TRIGGER_SERVICE))
    entry.runtime_data.verifier = types.SimpleNamespace(
        async_verify=lambda trigger: _answer(VerificationState(ok=True))
    )
    assert run(kv.async_verify_entry(hass, entry, trigger=VERIFY_TRIGGER_SERVICE)).ok is True


async def _answer(value):
    return value
