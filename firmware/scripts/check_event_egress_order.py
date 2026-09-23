#!/usr/bin/env python3
"""Hold the canary's event egress to the two things its backfill test assumes.

The SD event log's reconnect backfill (backlog F37) keeps its rules in the
pure `firmware/common/csi/src/csi_event_backfill.h`, and
`firmware/tests_host/test_csi_event_backfill.cpp` replays whole outages
against it. That test runs the planner inside a model world, and two of the
properties it asserts are properties of the model, not of the planner:

- the model's live publish refuses while the MQTT offline queue still holds
  records, and
- the model's host publishes the tamper-topic bridge before it commits the
  row.

In the firmware those are `mqtt_publish_event_live()` in
`firmware/canary/lib/securacv_mqtt/src/securacv_mqtt.cpp` and
`csi_event_egress_pump()` in `firmware/canary/src/csi_event_egress.cpp`.
Reviewing F37, someone deleted the refusal and gated the bridge on the
backlog, and every host test and lint stayed green. This check is the guard
those two edits lacked.

## The rules

1. `mqtt_publish_event_live()` refuses while the offline queue holds
   records. Before its publish there is an `if (...) return false;` whose
   condition is a plain `||` chain with `!s_offline_q.empty()` as one term.
   Without that refusal, a backfilled row (its id above the watermark) goes
   out ahead of queued `events` rows with lower ids, and Home Assistant's
   replay gate then refuses those rows. A queued tamper alert would also
   wait behind the backlog. The function never buffers either: no
   `publish_or_queue(` and no `s_offline_q.push(` in its body.
2. The planner's live and backfill sends (`send_live` / `send_backfill` in
   csi_event_egress.cpp) publish through `mqtt_publish_event_live()`, never
   through the buffering `mqtt_publish_event()`.
3. In `csi_event_egress_pump()`, each dequeued row (`xQueueReceive(`)
   publishes its tamper bridge (`mqtt_publish_tamper(`) BEFORE the planner
   commits it (`s_backfill.commit(`). Between the dequeue and that publish,
   nothing consults the planner (`s_backfill`) or the link state
   (`link.connected`, `mqtt_connected(`), and `s_backfill.pending(` is not
   read anywhere before it. So a tamper alert never waits on the card or
   the backlog, and it queues through an outage. The backfill pass
   (`s_backfill.pass(`) runs after the row is committed.

## It proves it bites

Each run applies a set of mutations to the sources, in memory, and requires
the check to fail on every one. The reviewer's two edits are in that set. So
the check is proven against the code as it stands. If a refactor moves an
anchor a mutation needs, the run fails and says so; it does not pass quietly.

Run locally:  python3 firmware/scripts/check_event_egress_order.py   (repo root)
CI:           firmware.yml "CSI Sketch Copy Sync", via check_csi_sync.sh
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Callable

REPO = Path(__file__).resolve().parents[2]
MQTT_CPP = "firmware/canary/lib/securacv_mqtt/src/securacv_mqtt.cpp"
EGRESS_CPP = "firmware/canary/src/csi_event_egress.cpp"


class AnchorMissing(Exception):
    """A self-test mutation no longer finds the code it mutates."""


def blank_comments_and_strings(src: str) -> str:
    """Comments and the insides of string/char literals become spaces.

    Offsets and newlines are kept, so a position in the result is the same
    position in the source (the mutations below rely on that).
    """
    out: list[str] = []
    i, n = 0, len(src)
    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", src[i:j]))
            i = j
        elif src[i] in "\"'":
            quote = src[i]
            j = i + 1
            while j < n and src[j] != quote and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(quote + " " * max(0, j - i - 2) + (quote if j - i >= 2 else ""))
            i = j
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def bodies(code: str, signature: str) -> list[tuple[int, int]]:
    """(start, end) of the body of every definition matching `signature`
    (a regex that ends just before the opening brace), braces excluded."""
    spans = []
    for m in re.finditer(signature + r"\s*\{", code):
        open_at = m.end() - 1
        depth = 0
        for j in range(open_at, len(code)):
            if code[j] == "{":
                depth += 1
            elif code[j] == "}":
                depth -= 1
                if depth == 0:
                    spans.append((open_at + 1, j))
                    break
    return spans


def top_level_terms(cond: str, op: str) -> list[str]:
    """Split `cond` on `op` where it is not nested in parentheses."""
    terms, depth, start, i = [], 0, 0, 0
    while i < len(cond):
        c = cond[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        elif depth == 0 and cond.startswith(op, i):
            terms.append(cond[start:i])
            start = i + len(op)
            i += len(op)
            continue
        i += 1
    terms.append(cond[start:])
    return terms


def squash(text: str) -> str:
    return re.sub(r"\s+", "", text)


def matching_paren(text: str, open_at: int) -> int:
    """Index of the ')' closing the '(' at `open_at`, or -1."""
    depth = 0
    for j in range(open_at, len(text)):
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
            if depth == 0:
                return j
    return -1


def unwrap(term: str) -> str:
    """`((x))` -> `x`, whitespace removed; `(a)||(b)` is left alone."""
    term = squash(term)
    while term.startswith("(") and matching_paren(term, 0) == len(term) - 1:
        term = term[1:-1]
    return term


# Signatures, matched against the blanked source.
SIG_LIVE = r"\bbool\s+mqtt_publish_event_live\s*\([^)]*\)"
SIG_PUMP = r"\bvoid\s+csi_event_egress_pump\s*\(\s*(?:void)?\s*\)"
SIG_SEND_LIVE = r"\bsend_live\s*\([^)]*\)\s*(?:override\s*)?"
SIG_SEND_BACKFILL = r"\bsend_backfill\s*\([^)]*\)\s*(?:override\s*)?"


def the_body(code: str, signature: str, what: str, errors: list[str],
             need: str | None = None) -> tuple[int, int] | None:
    """The one non-empty definition of `what` (containing `need`, if given)."""
    spans = [s for s in bodies(code, signature) if code[s[0]:s[1]].strip()]
    if need is not None:
        spans = [s for s in spans if need in code[s[0]:s[1]]]
    if len(spans) != 1:
        errors.append(f"{what}: expected one definition with a body, found {len(spans)}")
        return None
    return spans[0]


def check_live_publish(mqtt_src: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(mqtt_src)
    span = the_body(code, SIG_LIVE, f"{MQTT_CPP}: mqtt_publish_event_live()", errors)
    if span is None:
        return
    body = code[span[0]:span[1]]
    pub = body.find("s_mqtt.publish(")
    if pub < 0:
        errors.append(f"{MQTT_CPP}: mqtt_publish_event_live() has no s_mqtt.publish( call")
        return
    # An early refusal: `if (<cond>) return false;` (or `{ return false; }`)
    # before the publish, with the queue test as one term of an `||` chain.
    refused = False
    head = body[:pub]
    for m in re.finditer(r"\bif\s*\(", head):
        close = matching_paren(head, m.end() - 1)
        if close < 0 or not re.match(r"\s*\{?\s*return\s+false\s*;", head[close + 1:]):
            continue
        terms = [unwrap(t) for t in top_level_terms(unwrap(head[m.end():close]), "||")]
        if "!s_offline_q.empty()" in terms:
            refused = True
    if not refused:
        errors.append(
            f"{MQTT_CPP}: mqtt_publish_event_live() must `return false` while the offline queue "
            "holds records — an `if (... || !s_offline_q.empty()) return false;` before its "
            "publish. Without it a backfilled id overtakes queued rows with lower ids (Home "
            "Assistant's replay gate then refuses them) and a queued tamper alert waits behind "
            "the backlog.")
    for buffering in ("publish_or_queue(", "s_offline_q.push("):
        if buffering in body:
            errors.append(f"{MQTT_CPP}: mqtt_publish_event_live() must never buffer, "
                          f"but calls {buffering}")


def check_port_sends(egress_src: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(egress_src)
    for sig, what in ((SIG_SEND_LIVE, "send_live"), (SIG_SEND_BACKFILL, "send_backfill")):
        span = the_body(code, sig, f"{EGRESS_CPP}: EgressPort::{what}()", errors)
        if span is None:
            continue
        body = code[span[0]:span[1]]
        if "mqtt_publish_event_live(" not in body or re.search(r"\bmqtt_publish_event\s*\(", body):
            errors.append(
                f"{EGRESS_CPP}: EgressPort::{what}() must publish through "
                "mqtt_publish_event_live() "
                "and never the buffering mqtt_publish_event(): the planner keeps an unsent row on "
                "the card and sends it in id order, which a buffered copy would break.")


def check_pump_order(egress_src: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(egress_src)
    span = the_body(code, SIG_PUMP, f"{EGRESS_CPP}: csi_event_egress_pump()", errors,
                    need="xQueueReceive(")
    if span is None:
        return
    body = code[span[0]:span[1]]
    where = f"{EGRESS_CPP}: csi_event_egress_pump()"
    deq = body.find("xQueueReceive(")
    tamper = body.find("mqtt_publish_tamper(", deq)
    commit = body.find("s_backfill.commit(", deq)
    if body.count("xQueueReceive(") != 1:
        errors.append(f"{where}: expected one row dequeue (xQueueReceive), "
                      f"found {body.count('xQueueReceive(')}")
        return
    if tamper < 0 or commit < 0:
        errors.append(f"{where}: each dequeued row must publish its tamper bridge "
                      "(mqtt_publish_tamper) and be committed (s_backfill.commit)")
        return
    if tamper > commit:
        errors.append(f"{where}: the tamper bridge must publish BEFORE the row is committed "
                      "to the planner — a tamper alert never waits on the card or the backlog")
    between = body[deq:tamper]
    for gate in ("s_backfill", "link.connected", "mqtt_connected("):
        if gate in between:
            errors.append(f"{where}: the tamper bridge must not be gated on `{gate}` — it "
                          "publishes (or queues through an outage) whatever the backfill is doing")
    if "s_backfill.pending(" in body[:tamper]:
        errors.append(f"{where}: s_backfill.pending() is read before the tamper bridge — "
                      "the bridge must not depend on the backlog")
    passes = body.find("s_backfill.pass(", commit if commit >= 0 else 0)
    if passes < 0:
        errors.append(f"{where}: the backfill pass (s_backfill.pass) must run after the rows "
                      "are committed")


def check(mqtt_src: str, egress_src: str) -> list[str]:
    errors: list[str] = []
    check_live_publish(mqtt_src, errors)
    check_port_sends(egress_src, errors)
    check_pump_order(egress_src, errors)
    return errors


# ── Self-test: mutations the check must refuse ───────────────────────────


def mutate_in(src: str, signature: str, pattern: str, repl: str,
              need: str | None = None, count: int = 1) -> str:
    """Apply a regex substitution inside the (raw) body of one definition."""
    code = blank_comments_and_strings(src)
    spans = [s for s in bodies(code, signature) if code[s[0]:s[1]].strip()]
    if need is not None:
        spans = [s for s in spans if need in code[s[0]:s[1]]]
    if len(spans) != 1:
        raise AnchorMissing(f"definition {signature!r}")
    start, end = spans[0]
    new_body, n = re.subn(pattern, repl, src[start:end], count=count, flags=re.S)
    if n == 0:
        raise AnchorMissing(pattern)
    return src[:start] + new_body + src[end:]


Mutation = Callable[[str, str], "tuple[str, str]"]

MUTATIONS: list[tuple[str, Mutation]] = [
    # The review's edit: the live publish no longer waits for the queue.
    ("live publish ignores the offline queue",
     lambda m, e: (mutate_in(m, SIG_LIVE, r"\s*\|\|\s*!s_offline_q\.empty\(\)", ""), e)),
    ("live publish refuses only when down AND queued",
     lambda m, e: (mutate_in(m, SIG_LIVE, r"\|\|(\s*!s_offline_q\.empty\(\))", r"&&\1"), e)),
    ("live publish refuses on an EMPTY queue",
     lambda m, e: (mutate_in(m, SIG_LIVE, r"!(s_offline_q\.empty\(\))", r"\1"), e)),
    ("live publish falls back to buffering",
     lambda m, e: (mutate_in(m, SIG_LIVE, r"(return\s+s_mqtt\.publish\()",
                             "if (!s_mqtt.connected()) return publish_or_queue("
                             "mqtt_offline_queue::KIND_EVENT, s_topic_events, json_payload, false);"
                             r" \1"), e)),
    ("backfill send uses the buffering publish",
     lambda m, e: (m, mutate_in(e, SIG_SEND_BACKFILL, r"\bmqtt_publish_event_live\(",
                                "mqtt_publish_event("))),
    ("live send uses the buffering publish",
     lambda m, e: (m, mutate_in(e, SIG_SEND_LIVE, r"\bmqtt_publish_event_live\(",
                                "mqtt_publish_event("))),
    # The review's other edit: the bridge waits for the backlog.
    ("tamper bridge gated on the backlog",
     lambda m, e: (m, mutate_in(e, SIG_PUMP, r"if\s*\(\s*link\.accepting\s*&&",
                                "if (link.accepting && !s_backfill.pending() &&",
                                need="xQueueReceive("))),
    ("tamper bridge gated on a live link",
     lambda m, e: (m, mutate_in(e, SIG_PUMP, r"if\s*\(\s*link\.accepting\s*&&",
                                "if (link.connected &&", need="xQueueReceive("))),
    ("row committed before its tamper bridge",
     lambda m, e: (m, mutate_in(
         mutate_in(e, SIG_PUMP, r"\n[ \t]*\(void\)\s*s_backfill\.commit\([^;]*\);", "",
                   need="xQueueReceive("),
         SIG_PUMP, r"(xQueueReceive\([^;]*;)",
         r"\1 (void)s_backfill.commit(to_record(ev), link, s_port);", need="xQueueReceive("))),
    ("backfill pass before the rows",
     lambda m, e: (m, mutate_in(
         mutate_in(e, SIG_PUMP, r"s_backfill\.pass\(", "s_backfill.stats(",
                   need="xQueueReceive("),
         SIG_PUMP, r"(CommittedEvent\s+ev\s*;)", r"(void)s_backfill.pass(link, s_port); \1",
         need="xQueueReceive("))),
]


def self_test(mqtt_src: str, egress_src: str) -> list[str]:
    problems = []
    for name, mutate in MUTATIONS:
        try:
            m, e = mutate(mqtt_src, egress_src)
        except AnchorMissing as missing:
            problems.append(f"self-test: mutation '{name}' no longer applies "
                            f"(anchor {missing}) — "
                            "the source changed shape; update this guard's mutations with it")
            continue
        if (m, e) == (mqtt_src, egress_src):
            problems.append(f"self-test: mutation '{name}' changed nothing")
        elif not check(m, e):
            problems.append(f"self-test: the check did not bite on mutation '{name}'")
    return problems


def main() -> int:
    mqtt_src = (REPO / MQTT_CPP).read_text(encoding="utf-8")
    egress_src = (REPO / EGRESS_CPP).read_text(encoding="utf-8")
    errors = check(mqtt_src, egress_src)
    for err in errors:
        print(f"::error::{err}")
    problems = self_test(mqtt_src, egress_src)
    for problem in problems:
        print(f"::error::{problem}")
    if errors or problems:
        return 1
    print(f"Event egress order holds: the live publish waits for the offline queue, the "
          f"planner's sends never buffer, the tamper bridge goes first "
          f"({len(MUTATIONS)} mutations refused).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
