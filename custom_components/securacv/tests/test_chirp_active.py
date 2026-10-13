"""The Chirp Active sensor reads what the canary-wap sends (sweep HA33).

The canary-wap is the one firmware that publishes `<prefix>/<id>/chirp`:
``csi_mqtt::publish_chirp_state()`` sends ``{"state":"<word>"}`` and nothing
else, retained, on a build with the mesh feature, every 30 s on mains or a
normal battery (stretched to 2 or 4 min in the battery saver and low power
modes), where the word is ``chirp_channel::state_name()`` of the channel's
shown state, passed by the one caller in ``canary_wap.ino``. The
sensor used to read ``enabled`` and ``ready`` (fields of the WAP's REST
status, never of this topic), so it read off whatever the channel did, with
seven attributes no firmware sends. It now reads ``state``.

What each word reads as is a decision (binary_sensor.CHIRP_STATE_IS_ON): on
while the channel is on with a session (``active``, ``cooldown``, ``muted``,
``listening``), off while it is ``disabled`` or ``initializing``, unknown for
any other word (the lookup is exact: ``ACTIVE`` is another word), and unknown
until the first publish is read. The tests below write that decision out
independently of the table, so a change to either is a visible edit.

The monorepo-only tests read the body and the words from the firmware
source, and the argument the sketch's one caller passes, so they fail when
the firmware changes its shape, adds a state the table does not name or
publishes some other word, not only when Home Assistant changes (python.yml
runs this suite on an edit to any of the three files); they also hold
``WAP_CHIRP_ACTIVE``, the literal every other chirp test feeds, to what the
firmware renders. The HACS mirror carries no firmware source, so they skip
there and the literal cases still run.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

from .test_mqtt_payload_hardening import _entity, _msg

from .. import binary_sensor as bs_platform

# The canary-wap's chirp body for an active channel, byte for byte:
# test_firmware_renders_the_literal_fixture holds it to csi_mqtt.cpp.
WAP_CHIRP_ACTIVE = '{"state":"active"}'

_WAP_DIR = (
    Path(__file__).resolve().parents[3]
    / "firmware" / "projects" / "canary-wap" / "arduino" / "canary_wap"
)
_WAP_MQTT = _WAP_DIR / "csi_mqtt.cpp"
_WAP_CHIRP = _WAP_DIR / "chirp_channel.cpp"
_WAP_SKETCH = _WAP_DIR / "canary_wap.ino"

monorepo_only = pytest.mark.skipif(
    not (_WAP_MQTT.is_file() and _WAP_CHIRP.is_file() and _WAP_SKETCH.is_file()),
    reason="monorepo-only: the HACS mirror carries no firmware source",
)

# The decision, written out here rather than read from the table it tests.
EXPECTED: dict[str, bool | None] = {
    "active": True,
    "cooldown": True,
    "muted": True,
    "listening": True,
    "disabled": False,
    "initializing": False,
    "unknown": None,
}


def _function_body(text: str, signature: str) -> str:
    m = re.search(r"\n" + re.escape(signature) + r"\s*\{(.*?)\n\}\n", text, re.S)
    assert m, f"{signature} moved; update this test"
    return m.group(1)


def _chirp_format() -> tuple[str, str]:
    """publish_chirp_state()'s body format, unescaped, and its null fallback."""
    body = _function_body(
        _WAP_MQTT.read_text(encoding="utf-8"),
        "void publish_chirp_state(const char* state_name)",
    )
    assert 'build_topic(topic, sizeof(topic), "chirp")' in body, (
        "publish_chirp_state() no longer publishes on the chirp topic"
    )
    m = re.search(r'snprintf\(body, sizeof\(body\),\s*"((?:[^"\\]|\\.)*)"\s*,\s*'
                  r'state_name \? state_name : "([a-z_]+)"\s*\)', body)
    assert m, "publish_chirp_state()'s snprintf moved; update this test"
    fmt = m.group(1).replace('\\"', '"')
    assert "\\" not in fmt, f"an escape this test does not read: {fmt!r}"
    assert fmt.count("%") == 1 and "%s" in fmt, f"a conversion this test does not read: {fmt!r}"
    return fmt, m.group(2)


def _render(word: str) -> str:
    """The body the canary-wap publishes for `word`, from its own format."""
    fmt, _ = _chirp_format()
    return fmt.replace("%s", word)


def _state_name_words() -> set[str]:
    """Every word chirp_channel::state_name() can return, its default included."""
    body = _function_body(
        _WAP_CHIRP.read_text(encoding="utf-8"),
        "const char* state_name(ChirpState state)",
    )
    words = set(re.findall(r'return "([^"]*)";', body))
    assert words, "state_name() returns no string literal; update this test"
    return words


def _sensor():
    return _entity(bs_platform.SecuraCVCanaryChirpActiveSensor)


def _assert_unsigned_only(attrs: dict, word: str) -> None:
    """The word, the unsigned trust slice, and nothing the firmware never sent."""
    assert attrs["channel_state"] == word
    assert attrs["verified"] is False
    assert attrs["trust_reason"] == "unsigned"
    assert set(attrs) == {
        "channel_state", "verified", "trust_reason",
        "pinned_fingerprint", "received_fingerprint",
    }


# ─── the firmware's side, read from its source ─────────────────────────


@monorepo_only
def test_firmware_sends_the_state_and_nothing_else() -> None:
    fmt, fallback = _chirp_format()
    for word in _state_name_words() | {fallback}:
        assert json.loads(_render(word)) == {"state": word}, fmt


@monorepo_only
def test_firmware_renders_the_literal_fixture() -> None:
    assert _render("active") == WAP_CHIRP_ACTIVE


@monorepo_only
def test_the_table_names_every_word_but_unknown() -> None:
    words = _state_name_words()
    _, fallback = _chirp_format()
    assert "unknown" in words and fallback == "unknown", (
        "state_name()'s default or publish_chirp_state()'s null fallback moved"
    )
    assert set(bs_platform.CHIRP_STATE_IS_ON) == words - {"unknown"}, (
        "a word chirp_channel::state_name() can send is missing from "
        "CHIRP_STATE_IS_ON (or the table names one it cannot): decide what it reads as"
    )
    assert set(EXPECTED) == words


@monorepo_only
def test_the_sketch_publishes_state_name_of_the_shown_state() -> None:
    """The table reads state_name()'s words only if that is what is sent:
    the sketch's one caller passes state_name() of get_status().state (the
    shown state), not a word of its own (an NFPA-72 mapping, say)."""
    # Line comments out (the sketch's own comment names the call's topic).
    text = re.sub(r"//[^\n]*", "", _WAP_SKETCH.read_text(encoding="utf-8"))
    calls = []
    for m in re.finditer(r"\bpublish_chirp_state\s*\(", text):
        depth, i = 1, m.end()
        while depth:
            assert i < len(text), "unbalanced publish_chirp_state( call"
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        calls.append(text[m.end():i - 1])
    assert len(calls) == 1, f"canary_wap.ino calls publish_chirp_state() {len(calls)} times"
    arg = re.sub(r"\s+", "", calls[0])
    assert arg == "chirp_channel::state_name(chirp_channel::get_status().state)", (
        f"the chirp topic now carries {arg!r}: decide what the sensor reads it as"
    )


@monorepo_only
def test_python_yml_runs_this_suite_on_each_firmware_file_it_reads() -> None:
    """A firmware-only edit (a new Chirp state, say) still runs these tests."""
    workflow = _WAP_DIR.parents[4] / ".github" / "workflows" / "python.yml"
    if not workflow.is_file():
        pytest.skip("no python.yml beside this tree")
    lines = workflow.read_text(encoding="utf-8").splitlines()
    for source in (_WAP_MQTT, _WAP_CHIRP, _WAP_SKETCH):
        rel = source.relative_to(_WAP_DIR.parents[4]).as_posix()
        listed = [ln for ln in lines if ln.strip() == f"- '{rel}'"]
        assert len(listed) == 2, f"python.yml's push and pull_request paths must list {rel}"


@monorepo_only
def test_each_firmware_word_reads_as_decided() -> None:
    for word in sorted(_state_name_words()):
        sensor = _sensor()
        sensor._handle_message(_msg(_render(word)))
        assert sensor.writes == [True], word
        assert sensor._attr_is_on is EXPECTED[word], word
        _assert_unsigned_only(sensor._attr_extra_state_attributes, word)


# ─── the literal cases (these run in the HACS mirror too) ──────────────


def test_the_sensor_is_unknown_until_a_state_is_read() -> None:
    """Off is a claim (the channel is disabled); before any publish, and
    after bodies that carry no string `state`, the sensor says nothing."""
    sensor = _sensor()
    assert sensor._attr_is_on is None
    for payload in (
        '{"enabled": true, "ready": true}',
        '{"state": 3}',
        '{"state": true}',
    ):
        sensor._handle_message(_msg(payload))
        assert sensor._attr_is_on is None, payload
    assert sensor.writes == []


def test_an_active_channel_reads_on() -> None:
    sensor = _sensor()
    sensor._handle_message(_msg(WAP_CHIRP_ACTIVE))
    assert sensor._attr_is_on is True
    _assert_unsigned_only(sensor._attr_extra_state_attributes, "active")


def test_cooldown_and_muted_are_a_channel_still_on() -> None:
    for word in ("cooldown", "muted"):
        sensor = _sensor()
        sensor._handle_message(_msg(json.dumps({"state": word})))
        assert sensor._attr_is_on is True, word
        assert sensor._attr_extra_state_attributes["channel_state"] == word


def test_disabled_reads_off_and_follows_the_channel_back_on() -> None:
    sensor = _sensor()
    sensor._handle_message(_msg('{"state":"disabled"}'))
    assert sensor._attr_is_on is False
    sensor._handle_message(_msg(WAP_CHIRP_ACTIVE))
    assert sensor._attr_is_on is True
    sensor._handle_message(_msg('{"state":"disabled"}'))
    assert sensor._attr_is_on is False
    assert sensor.writes == [True, True, True]


def test_a_word_the_table_does_not_know_reads_unknown_not_off() -> None:
    sensor = _sensor()
    sensor._handle_message(_msg(WAP_CHIRP_ACTIVE))
    for word in ("unknown", "Normal", "alarm", "", "ACTIVE", " active", "Disabled"):
        sensor._handle_message(_msg(json.dumps({"state": word})))
        assert sensor._attr_is_on is None, word
        assert sensor._attr_extra_state_attributes["channel_state"] == word


def test_a_body_without_a_string_state_leaves_the_sensor_alone() -> None:
    """The REST status shape the sensor used to read is not this topic's."""
    sensor = _sensor()
    sensor._handle_message(_msg(WAP_CHIRP_ACTIVE))
    for payload in (
        '{"enabled": true, "ready": true, "session_id": "abc"}',
        '{"state": 3}',
        '{"state": true}',
        '{"state": false}',
        '{"state": null}',
        '{"state": ["active"]}',
    ):
        sensor._handle_message(_msg(payload))
        assert sensor._attr_is_on is True, payload
    assert sensor.writes == [True]
    _assert_unsigned_only(sensor._attr_extra_state_attributes, "active")
