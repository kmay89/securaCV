"""Watches — bounded, self-expiring attention. The pure engine.

Design and rationale: docs/design/watches.md. In one line: a watch is a
period of attention on something the fleet already senses, it expires by
itself, and it learns what "normal" is instead of asking the owner for a
threshold they cannot know.

Everything here is pure — no Home Assistant imports, no clock of its own
(``now`` is always passed in) — so the whole decision core is host-tested
by ``tests/test_watches.py`` the same way voice.py is.

Three invariants this module is responsible for:

  1. **A watch always ends.** ``ends_at`` is required at construction;
     there is no "forever" to represent, so no code path can create one.
  2. **A watch never looks backward.** Observations before ``started_at``
     are refused — Invariant VI (no retroactive capability expansion) made
     literal, so you cannot decide today to have been watching last month.
  3. **A watch never alerts from an ignorant baseline.** Below
     MIN_BASELINE_OBSERVATIONS it reports that it is still settling in
     rather than guessing. A confident alert from too little data is worse
     than no alert.
"""
from __future__ import annotations

from typing import Any

DAY = 86400.0

# The five concerns — the whole vocabulary (docs/design/watches.md).
CONCERN_STOPPED = "stopped"
CONCERN_UNUSUAL = "unusual"
CONCERN_MORE = "more"
CONCERN_LESS = "less"
CONCERN_EVERY = "every"
CONCERNS = (CONCERN_STOPPED, CONCERN_UNUSUAL, CONCERN_MORE, CONCERN_LESS, CONCERN_EVERY)

# What each concern sounds like when a person says it — used to pick a
# concern from a spoken sentence, and to say one back.
CONCERN_PHRASE = {
    CONCERN_STOPPED: "if it stops",
    CONCERN_UNUSUAL: "if anything changes",
    CONCERN_MORE: "if it goes up",
    CONCERN_LESS: "if it drops",
    CONCERN_EVERY: "every time",
}

# Sensitivity is three words, never a number. These multiply the
# baseline's own spread, so they mean the same thing on any signal.
SENSITIVITY_K = {"gentle": 3.5, "normal": 2.5, "keen": 1.8}
DEFAULT_SENSITIVITY = "normal"

STATE_SETTLING = "settling"
STATE_WATCHING = "watching"
STATE_ENDED = "ended"

# Below this many baseline observations a watch says it is still settling
# in rather than forming an opinion.
MIN_BASELINE_OBSERVATIONS = 4
# Ring cap: a watch keeps enough to describe itself, never a diary.
MAX_OBSERVATIONS = 500
# Fallback spread when every baseline observation is identical (MAD == 0),
# as a fraction of the median — otherwise any variation at all would fire.
FLAT_BASELINE_SPREAD = 0.25

DEFAULT_DAYS = 14
# Settling-in is the first quarter of the watch, with a floor of a day and
# a ceiling of a week: long enough to learn, short enough to be useful.
SETTLE_FRACTION = 0.25
SETTLE_MIN = DAY
SETTLE_MAX = 7 * DAY


# ── Duration: what people say when they mean "for a while" ──────────────

_NUMBER_WORDS = {
    "a": 1, "an": 1, "one": 1, "two": 2, "three": 3, "four": 4, "five": 5,
    "six": 6, "seven": 7, "eight": 8, "nine": 9, "ten": 10, "twelve": 12,
    "fourteen": 14, "thirty": 30, "couple": 2, "few": 3,
}

_UNIT_DAYS = {
    "day": 1.0, "days": 1.0, "night": 1.0, "nights": 1.0,
    "week": 7.0, "weeks": 7.0, "fortnight": 14.0,
    "month": 30.0, "months": 30.0,
    "season": 90.0, "seasons": 90.0, "year": 365.0, "years": 365.0,
}


def parse_duration_days(text: str | None, default: float = DEFAULT_DAYS) -> float:
    """Days from a spoken duration: "two weeks", "10 days", "a month".

    Deliberately forgiving and deliberately bounded — an unparseable
    phrase becomes the default rather than an error the owner has to
    debug, and nothing may exceed a year, because a watch that outlives
    the concern that created it is the failure this feature exists to
    prevent.
    """
    if not text:
        return default
    words = str(text).lower().replace("-", " ").split()
    count: float | None = None
    for i, word in enumerate(words):
        unit = _UNIT_DAYS.get(word)
        if unit is None:
            continue
        # The number sits just before the unit, as a digit or a word.
        if i > 0:
            prev = words[i - 1]
            if prev.replace(".", "", 1).isdigit():
                count = float(prev)
            elif prev in _NUMBER_WORDS:
                count = float(_NUMBER_WORDS[prev])
        if count is None:
            count = 1.0
        return max(1.0, min(365.0, count * unit))
    return default


def concern_from_text(text: str | None, default: str = CONCERN_UNUSUAL) -> str:
    """Pick a concern from how the sentence was phrased.

    Order matters: "stops" is checked before the directional words so
    "tell me if it stops going up" reads as absence, which is what a
    person means by it.
    """
    if not text:
        return default
    low = str(text).lower()
    if any(w in low for w in ("stops", "stop ", "quiet", "nothing", "goes silent")):
        return CONCERN_STOPPED
    if any(w in low for w in ("every time", "everything", "each time", "any activity")):
        return CONCERN_EVERY
    if any(w in low for w in ("drops", "falls", "dries", "below", "goes down", "lower")):
        return CONCERN_LESS
    if any(w in low for w in ("goes up", "rises", "above", "more often", "increases", "higher")):
        return CONCERN_MORE
    return default


# ── The watch itself ────────────────────────────────────────────────────


def make_watch(
    watch_id: str,
    label: str,
    subject: dict[str, Any],
    now: float,
    days: float = DEFAULT_DAYS,
    concern: str = CONCERN_UNUSUAL,
    sensitivity: str = DEFAULT_SENSITIVITY,
) -> dict[str, Any]:
    """Build a watch. ``ends_at`` is computed, never optional.

    There is no argument that makes a watch permanent, and no state a
    watch can be put in later that removes its end — the only way to keep
    attention going is to deliberately extend it.
    """
    days = max(1.0, min(365.0, float(days)))
    span = days * DAY
    settle = max(SETTLE_MIN, min(SETTLE_MAX, span * SETTLE_FRACTION))
    return {
        "id": watch_id,
        "label": label.strip() or watch_id,
        "subject": dict(subject),
        "concern": concern if concern in CONCERNS else CONCERN_UNUSUAL,
        "sensitivity": sensitivity if sensitivity in SENSITIVITY_K else DEFAULT_SENSITIVITY,
        "started_at": float(now),
        "ends_at": float(now) + span,
        "settle_until": float(now) + settle,
        "observations": [],
        "state": STATE_SETTLING,
        "fired": 0,
        "last_fired_at": None,
    }


def observe(watch: dict[str, Any], value: float, now: float) -> None:
    """Record one observation. Refuses anything before the watch began.

    That refusal is Invariant VI in three lines: a watch cannot be handed
    history it did not witness, so backfilling it into a retroactive
    query is not a thing the API can express.
    """
    if now < watch["started_at"]:
        return
    if now > watch["ends_at"]:
        return
    obs = watch.setdefault("observations", [])
    obs.append([float(now), float(value)])
    if len(obs) > MAX_OBSERVATIONS:
        del obs[: len(obs) - MAX_OBSERVATIONS]


def _median(values: list[float]) -> float:
    ordered = sorted(values)
    n = len(ordered)
    mid = n // 2
    if n % 2:
        return ordered[mid]
    return (ordered[mid - 1] + ordered[mid]) / 2.0


def baseline(watch: dict[str, Any]) -> dict[str, Any] | None:
    """Median and spread of the settling-in observations, or None.

    Median + MAD rather than mean + standard deviation: a two-week watch
    has few observations and one strange day should not move what counts
    as normal. Returns None while there is too little to be honest about.
    """
    settle_until = watch.get("settle_until", 0.0)
    values = [v for (t, v) in watch.get("observations", []) if t <= settle_until]
    if len(values) < MIN_BASELINE_OBSERVATIONS:
        return None
    med = _median(values)
    mad = _median([abs(v - med) for v in values])
    # 1.4826 scales MAD to a standard-deviation-equivalent for normal data.
    spread = mad * 1.4826
    if spread <= 0:
        # Every baseline reading identical: use a fraction of the level so
        # a genuinely flat signal doesn't fire on any wobble at all.
        spread = abs(med) * FLAT_BASELINE_SPREAD
    return {"median": med, "spread": spread, "count": len(values)}


def typical_gap(watch: dict[str, Any]) -> float | None:
    """Median time between observations during settling in.

    This is what ``stopped`` compares against: silence only means
    something relative to how often this signal normally speaks.
    """
    settle_until = watch.get("settle_until", 0.0)
    times = [t for (t, _v) in watch.get("observations", []) if t <= settle_until]
    if len(times) < MIN_BASELINE_OBSERVATIONS:
        return None
    gaps = [b - a for a, b in zip(times, times[1:]) if b > a]
    if not gaps:
        return None
    return _median(gaps)


def evaluate(watch: dict[str, Any], now: float) -> dict[str, Any]:
    """Should this watch speak right now, and what would it say?

    Returns ``{"fire": bool, "reason": str, "detail": str}``. Never
    mutates the watch — the caller decides whether to act, so the same
    call is safe to make from a test, a dry run, or the live tick.
    """
    quiet = {"fire": False, "reason": "quiet", "detail": ""}

    if watch.get("state") == STATE_ENDED or now >= watch.get("ends_at", 0.0):
        return {"fire": False, "reason": "ended", "detail": ""}

    concern = watch.get("concern", CONCERN_UNUSUAL)
    obs = watch.get("observations", [])

    # "Every time" needs no baseline — it is a relay, and says so.
    if concern == CONCERN_EVERY:
        if not obs:
            return quiet
        last_t, last_v = obs[-1]
        if watch.get("last_fired_at") is not None and last_t <= watch["last_fired_at"]:
            return quiet
        return {"fire": True, "reason": "every", "detail": f"{_num(last_v)}"}

    if now < watch.get("settle_until", 0.0):
        return {"fire": False, "reason": "settling", "detail": ""}

    base = baseline(watch)
    if base is None:
        # Past the settling window but too little data to have an opinion.
        return {"fire": False, "reason": "not_enough", "detail": ""}

    k = SENSITIVITY_K.get(watch.get("sensitivity", DEFAULT_SENSITIVITY), 2.5)

    if concern == CONCERN_STOPPED:
        gap = typical_gap(watch)
        if gap is None:
            return {"fire": False, "reason": "not_enough", "detail": ""}
        last_t = obs[-1][0] if obs else watch["started_at"]
        silent_for = now - last_t
        # k scales the tolerated silence; a floor keeps a chatty signal
        # from firing on one skipped beat.
        if silent_for > max(gap * k, gap + 300):
            return {
                "fire": True,
                "reason": "stopped",
                "detail": f"nothing for {_duration_phrase(silent_for)}",
            }
        return quiet

    if not obs:
        return quiet
    last_t, last_v = obs[-1]
    if watch.get("last_fired_at") is not None and last_t <= watch["last_fired_at"]:
        return quiet
    if last_t <= watch.get("settle_until", 0.0):
        return quiet

    delta = last_v - base["median"]
    if abs(delta) <= base["spread"] * k:
        return quiet
    if concern == CONCERN_MORE and delta <= 0:
        return quiet
    if concern == CONCERN_LESS and delta >= 0:
        return quiet

    direction = "up from" if delta > 0 else "down from"
    return {
        "fire": True,
        "reason": "deviation",
        "detail": f"{_num(last_v)}, {direction} the usual {_num(base['median'])}",
    }


def note_fired(watch: dict[str, Any], now: float) -> None:
    """Mark that the watch spoke, so it doesn't repeat the same news."""
    watch["fired"] = int(watch.get("fired", 0)) + 1
    watch["last_fired_at"] = float(now)


def refresh_state(watch: dict[str, Any], now: float) -> str:
    """Advance settling -> watching -> ended. Returns the new state."""
    if now >= watch.get("ends_at", 0.0):
        watch["state"] = STATE_ENDED
    elif now >= watch.get("settle_until", 0.0):
        watch["state"] = STATE_WATCHING
    else:
        watch["state"] = STATE_SETTLING
    return watch["state"]


def extend(watch: dict[str, Any], days: float, now: float) -> None:
    """Keep a watch going — the only way attention continues past its end.

    Extending from ``now`` rather than from the old end date, so "another
    two weeks" means two weeks from the conversation, which is what the
    person meant. The baseline is kept: it was learned on this same
    signal and re-learning it would blind the watch all over again.
    """
    days = max(1.0, min(365.0, float(days)))
    watch["ends_at"] = float(now) + days * DAY
    if watch.get("state") == STATE_ENDED:
        watch["state"] = STATE_WATCHING


# ── Saying it out loud ──────────────────────────────────────────────────


def _num(value: float) -> str:
    """A number said the way a person would say it."""
    if abs(value - round(value)) < 0.05:
        return str(int(round(value)))
    return f"{value:.1f}"


def _duration_phrase(seconds: float) -> str:
    if seconds < 5400:
        return f"{max(1, int(round(seconds / 60)))} minutes"
    if seconds < 2 * DAY:
        hours = int(round(seconds / 3600))
        return f"{hours} hour{'' if hours == 1 else 's'}"
    days = int(round(seconds / DAY))
    return f"{days} days"


def days_left(watch: dict[str, Any], now: float) -> int:
    """Whole days remaining, floored at zero."""
    return max(0, int(round((watch.get("ends_at", 0.0) - now) / DAY)))


def speak_started(watch: dict[str, Any]) -> str:
    """Confirmation when a watch begins — repeats it back so a
    misheard sentence is caught immediately, not in two weeks."""
    days = max(1, int(round((watch["ends_at"] - watch["started_at"]) / DAY)))
    phrase = CONCERN_PHRASE.get(watch["concern"], "if anything changes")
    return (
        f"Watching {watch['label']} for {days} day{'' if days == 1 else 's'} — "
        f"I'll tell you {phrase}. I'll get a feel for normal first, then it's "
        "on watch. It ends by itself, and I'll say so."
    )


def speak_fired(watch: dict[str, Any], verdict: dict[str, Any]) -> str:
    """What a watch says when it has something to report."""
    label = watch["label"]
    if verdict["reason"] == "stopped":
        return f"Something to flag on the {label} watch: {verdict['detail']}."
    if verdict["reason"] == "every":
        return f"The {label} watch: {verdict['detail']}."
    return f"The {label} watch has something unusual: {verdict['detail']}."


def speak_roster(watches: list[dict[str, Any]], now: float) -> str:
    """"What am I watching?" — the list, with time remaining."""
    live = [w for w in watches if w.get("state") != STATE_ENDED and now < w.get("ends_at", 0.0)]
    if not live:
        return "Nothing right now. Ask me to keep an eye on something and I'll start a watch."
    if len(live) > 3:
        # Speech is serial: past a handful, summarize (voice_moments.md law 3).
        soonest = min(live, key=lambda w: w.get("ends_at", 0.0))
        left = days_left(soonest, now)
        when = "ends today" if left == 0 else f"in {left} day{'' if left == 1 else 's'}"
        return (
            f"{len(live)} watches running. The next to finish is "
            f"{soonest['label']}, {when}. The dashboard has the rest."
        )
    bits = []
    for watch in live:
        left = days_left(watch, now)
        when = "ends today" if left == 0 else f"{left} more day{'' if left == 1 else 's'}"
        settling = " — still getting a feel for normal" if now < watch.get("settle_until", 0) else ""
        bits.append(f"{watch['label']}, {when}{settling}")
    if len(bits) == 1:
        return f"One watch: {bits[0]}."
    return f"{len(bits)} watches: " + "; ".join(bits) + "."


def speak_ending(watch: dict[str, Any]) -> str:
    """The end-of-watch summary — the sentence that makes it worth it.

    Reports what was actually seen, and stays honest when the watch never
    learned enough to have an opinion: "still getting a feel for normal"
    rather than a reassuring number it did not earn.
    """
    days = max(1, int(round((watch["ends_at"] - watch["started_at"]) / DAY)))
    label = watch["label"]
    fired = int(watch.get("fired", 0))
    base = baseline(watch)
    obs_count = len(watch.get("observations", []))

    if obs_count == 0:
        return (
            f"The {label} watch ended after {days} days. I never saw anything "
            "from it — worth checking it was reporting at all. Want me to keep "
            "going, or let it go?"
        )
    if base is None:
        return (
            f"The {label} watch ended after {days} days. Only {obs_count} "
            "reading" + ("" if obs_count == 1 else "s") + " came in — not enough "
            "for me to have learned what normal looks like. Want me to keep "
            "going, or let it go?"
        )
    if fired == 0:
        return (
            f"The {label} watch ended. {days} days, holding steady around "
            f"{_num(base['median'])}, nothing unusual. Want me to keep going, "
            "or let it go?"
        )
    return (
        f"The {label} watch ended. {days} days, usually around "
        f"{_num(base['median'])}, and I flagged {fired} thing"
        f"{'' if fired == 1 else 's'}. Want me to keep going, or let it go?"
    )
