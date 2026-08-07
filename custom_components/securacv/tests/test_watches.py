"""Tests for watches.py — bounded, self-expiring attention.

Three properties get the most attention here, because they are the ones a
regression would quietly break: a watch always ends, a watch never looks
backward, and a watch never alerts from a baseline it hasn't earned.
"""
from __future__ import annotations

from ..watches import (
    CONCERN_EVERY,
    CONCERN_LESS,
    CONCERN_MORE,
    CONCERN_STOPPED,
    CONCERN_UNUSUAL,
    DAY,
    STATE_ENDED,
    STATE_SETTLING,
    STATE_WATCHING,
    baseline,
    concern_from_text,
    days_left,
    evaluate,
    extend,
    make_watch,
    note_fired,
    observe,
    parse_duration_days,
    refresh_state,
    speak_ending,
    speak_fired,
    speak_roster,
    speak_started,
)

NOW = 1_000_000.0
SUBJECT = {"kind": "event", "ref": "litterbox"}


def _watch(**kw):
    kw.setdefault("days", 14)
    return make_watch("w1", kw.pop("label", "the litter box"), SUBJECT, NOW, **kw)


def _fill_baseline(watch, values, start=None, step=None):
    """Lay observations inside the settling window."""
    start = watch["started_at"] + 60 if start is None else start
    span = watch["settle_until"] - start
    step = (span / max(1, len(values))) if step is None else step
    for i, value in enumerate(values):
        observe(watch, value, start + i * step)
    return watch


# ── duration + concern parsing ──────────────────────────────────────────


def test_parse_duration_the_way_people_say_it():
    assert parse_duration_days("two weeks") == 14
    assert parse_duration_days("a week") == 7
    assert parse_duration_days("10 days") == 10
    assert parse_duration_days("a couple weeks") == 14
    assert parse_duration_days("a month") == 30
    assert parse_duration_days("a few days") == 3
    assert parse_duration_days("fortnight") == 14
    # Unparseable falls back rather than erroring at the person.
    assert parse_duration_days("for a bit") == 14
    assert parse_duration_days(None) == 14
    # Nothing may outlive the concern that created it.
    assert parse_duration_days("500 years") == 365
    assert parse_duration_days("0 days") == 1


def test_concern_from_how_it_was_phrased():
    assert concern_from_text("tell me if she stops using it") == CONCERN_STOPPED
    assert concern_from_text("tell me if the soil dries out") == CONCERN_LESS
    assert concern_from_text("let me know if it goes up") == CONCERN_MORE
    assert concern_from_text("tell me every time") == CONCERN_EVERY
    assert concern_from_text("keep an eye on it") == CONCERN_UNUSUAL
    # Absence wins over direction when both appear.
    assert concern_from_text("tell me if it stops going up") == CONCERN_STOPPED


# ── the three structural properties ─────────────────────────────────────


def test_a_watch_always_ends():
    watch = _watch()
    assert watch["ends_at"] > watch["started_at"]
    assert days_left(watch, NOW) == 14
    # There is no argument that makes it permanent; a year is the ceiling.
    forever = _watch(days=10_000)
    assert days_left(forever, NOW) == 365
    # And it reports ended once past its date.
    assert refresh_state(watch, NOW + 15 * DAY) == STATE_ENDED
    assert evaluate(watch, NOW + 15 * DAY)["reason"] == "ended"


def test_a_watch_never_looks_backward():
    # Invariant VI made literal: observations before the start are refused,
    # so a watch cannot be backfilled into a retroactive query.
    watch = _watch()
    observe(watch, 3, NOW - DAY)
    observe(watch, 3, NOW - 1)
    assert watch["observations"] == []
    observe(watch, 3, NOW + 60)
    assert len(watch["observations"]) == 1
    # Nor after it ends.
    observe(watch, 3, NOW + 99 * DAY)
    assert len(watch["observations"]) == 1


def test_a_watch_never_alerts_from_an_ignorant_baseline():
    watch = _watch()
    # Too few observations: past settling, still no opinion.
    _fill_baseline(watch, [3, 3])
    assert baseline(watch) is None
    observe(watch, 99, watch["settle_until"] + 60)
    assert evaluate(watch, watch["settle_until"] + 120)["reason"] == "not_enough"

    # During settling it says so rather than guessing.
    fresh = _watch()
    _fill_baseline(fresh, [3, 3, 3, 3, 3])
    assert evaluate(fresh, fresh["started_at"] + 120)["reason"] == "settling"


# ── baseline + deviation ────────────────────────────────────────────────


def test_baseline_is_robust_to_one_weird_day():
    watch = _watch()
    _fill_baseline(watch, [3, 3, 4, 3, 40, 3, 4])  # one outlier
    base = baseline(watch)
    assert base is not None
    assert base["median"] == 3  # the outlier does not move normal
    assert base["count"] == 7


def test_unusual_fires_in_either_direction():
    watch = _watch(concern=CONCERN_UNUSUAL, sensitivity="keen")
    _fill_baseline(watch, [3, 3, 4, 3, 3, 4])
    after = watch["settle_until"] + 60

    observe(watch, 12, after)
    verdict = evaluate(watch, after + 10)
    assert verdict["fire"] is True
    assert "up from the usual 3" in verdict["detail"]

    # And downward, on a fresh watch of the same shape.
    down = _watch(concern=CONCERN_UNUSUAL, sensitivity="keen")
    _fill_baseline(down, [10, 11, 10, 10, 11, 10])
    observe(down, 0, down["settle_until"] + 60)
    assert evaluate(down, down["settle_until"] + 70)["fire"] is True


def test_directional_concerns_ignore_the_other_direction():
    # "tell me if it drops" stays quiet when it climbs.
    less = _watch(concern=CONCERN_LESS, sensitivity="keen")
    _fill_baseline(less, [40, 41, 40, 39, 40, 41])
    observe(less, 90, less["settle_until"] + 60)
    assert evaluate(less, less["settle_until"] + 70)["fire"] is False
    observe(less, 5, less["settle_until"] + 120)
    assert evaluate(less, less["settle_until"] + 130)["fire"] is True

    more = _watch(concern=CONCERN_MORE, sensitivity="keen")
    _fill_baseline(more, [3, 3, 4, 3, 3, 4])
    observe(more, 0, more["settle_until"] + 60)
    assert evaluate(more, more["settle_until"] + 70)["fire"] is False


def test_sensitivity_is_three_words_that_actually_differ():
    def fires(sensitivity, value):
        watch = _watch(concern=CONCERN_UNUSUAL, sensitivity=sensitivity)
        # median 11, spread ~1.5 -> keen fires past ~2.7, gentle past ~5.2
        _fill_baseline(watch, [10, 12, 10, 12, 10, 12])
        observe(watch, value, watch["settle_until"] + 60)
        return evaluate(watch, watch["settle_until"] + 70)["fire"]

    # A modest wobble reaches keen but not gentle.
    assert fires("keen", 15) is True
    assert fires("gentle", 15) is False
    # A big move reaches all three.
    assert fires("gentle", 30) is True


def test_stopped_fires_on_silence_relative_to_its_own_rhythm():
    # A 4-day watch settles for one day; fill that day hourly so the
    # rhythm it learns is "about once an hour".
    watch = _watch(concern=CONCERN_STOPPED, sensitivity="normal", days=4)
    start = watch["started_at"] + 60
    for i in range(24):
        observe(watch, 1, start + i * 3600)
    last = start + 23 * 3600

    # One skipped beat is not an alarm.
    assert evaluate(watch, last + 3600)["fire"] is False
    # Hours of silence, against an hourly rhythm, is.
    verdict = evaluate(watch, last + 5 * 3600)
    assert verdict["fire"] is True
    assert verdict["reason"] == "stopped"
    assert "nothing for" in verdict["detail"]

    # The same silence against a daily rhythm is NOT an alarm — "stopped"
    # is always relative to the signal's own pace.
    slow = _watch(concern=CONCERN_STOPPED, sensitivity="normal", days=60)
    slow_start = slow["started_at"] + 60
    for i in range(7):
        observe(slow, 1, slow_start + i * DAY)
    assert evaluate(slow, slow_start + 6 * DAY + 5 * 3600)["fire"] is False


def test_every_is_a_relay_needing_no_baseline():
    watch = _watch(concern=CONCERN_EVERY)
    observe(watch, 1, NOW + 60)
    verdict = evaluate(watch, NOW + 61)
    assert verdict["fire"] is True
    # It does not repeat the same news.
    note_fired(watch, NOW + 61)
    assert evaluate(watch, NOW + 62)["fire"] is False
    observe(watch, 1, NOW + 120)
    assert evaluate(watch, NOW + 121)["fire"] is True


def test_fired_news_is_not_repeated():
    watch = _watch(concern=CONCERN_UNUSUAL, sensitivity="keen")
    _fill_baseline(watch, [3, 3, 4, 3, 3, 4])
    at = watch["settle_until"] + 60
    observe(watch, 20, at)
    assert evaluate(watch, at + 5)["fire"] is True
    note_fired(watch, at + 5)
    assert evaluate(watch, at + 10)["fire"] is False
    assert watch["fired"] == 1


# ── lifecycle ───────────────────────────────────────────────────────────


def test_state_advances_and_extend_is_the_only_way_to_continue():
    watch = _watch(days=8)
    assert refresh_state(watch, NOW + 60) == STATE_SETTLING
    assert refresh_state(watch, NOW + 3 * DAY) == STATE_WATCHING
    assert refresh_state(watch, NOW + 9 * DAY) == STATE_ENDED

    # Extending measures from the conversation, not the old end date.
    extend(watch, 14, NOW + 9 * DAY)
    assert days_left(watch, NOW + 9 * DAY) == 14
    assert watch["state"] == STATE_WATCHING
    # The learned baseline survives, so it isn't blind all over again.
    assert watch["settle_until"] < watch["ends_at"]


# ── speech ──────────────────────────────────────────────────────────────


def test_speak_started_repeats_it_back():
    speech = speak_started(_watch(concern=CONCERN_STOPPED))
    assert "Watching the litter box for 14 days" in speech
    assert "I'll tell you if it stops" in speech
    assert "ends by itself" in speech


def test_speak_roster_shows_time_left_and_settling():
    a = make_watch("a", "the litter box", SUBJECT, NOW, days=14)
    b = make_watch("b", "the soil", SUBJECT, NOW, days=3)
    assert speak_roster([], NOW).startswith("Nothing right now")

    speech = speak_roster([a], NOW + 5 * DAY)
    assert speech == "One watch: the litter box, 9 more days."

    speech = speak_roster([a, b], NOW + 60)
    assert speech.startswith("2 watches:")
    assert "still getting a feel for normal" in speech

    # An expired watch drops off the roster.
    assert "Nothing right now" in speak_roster([b], NOW + 30 * DAY)


def test_speak_ending_is_honest_about_what_it_learned():
    # Nothing ever seen — says so, rather than reporting calm.
    empty = _watch()
    assert "never saw anything from it" in speak_ending(empty)

    # Some data but not enough to learn from.
    thin = _watch()
    _fill_baseline(thin, [3, 3])
    assert "not enough for me to have learned what normal looks like" in speak_ending(thin)

    # A real, quiet run.
    calm = _watch()
    _fill_baseline(calm, [3, 3, 4, 3, 3, 4])
    speech = speak_ending(calm)
    assert "holding steady around 3, nothing unusual" in speech
    assert "keep going, or let it go?" in speech

    # A run that flagged things counts them.
    noisy = _watch()
    _fill_baseline(noisy, [3, 3, 4, 3, 3, 4])
    noisy["fired"] = 2
    assert "I flagged 2 things" in speak_ending(noisy)


def test_speak_fired_names_the_watch():
    watch = _watch()
    assert "the litter box watch" in speak_fired(
        watch, {"reason": "deviation", "detail": "9, up from the usual 3"}
    )
    assert "Something to flag" in speak_fired(
        watch, {"reason": "stopped", "detail": "nothing for 2 days"}
    )


def test_watch_roster_summarizes_past_a_handful():
    # Law 3 (docs/design/voice_moments.md): speech is serial, so a long
    # list of watches is summarized and handed to the screen.
    many = [
        make_watch(f"w{i}", f"watch {i}", SUBJECT, NOW, days=10 + i)
        for i in range(5)
    ]
    speech = speak_roster(many, NOW + DAY)
    assert speech.startswith("5 watches running.")
    assert "The next to finish is watch 0, in 9 days." in speech
    assert "The dashboard has the rest." in speech
    # Three still read out in full.
    assert speak_roster(many[:3], NOW + DAY).startswith("3 watches:")
