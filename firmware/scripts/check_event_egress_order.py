#!/usr/bin/env python3
"""Hold the canary's event egress to what its backfill test assumes.

The SD event log's reconnect backfill (backlog F37) keeps its rules in the
pure `firmware/common/csi/src/csi_event_backfill.h`, and
`firmware/tests_host/test_csi_event_backfill.cpp` replays whole outages
against it. That test runs the planner inside a model world, and two of the
properties it asserts are properties of the model, not of the planner
(rules 1-3 below; the model also supplies three glue values, rules 4-6):

- the model's live publish refuses while the MQTT offline queue still holds
  records, and
- the model's host publishes the tamper-topic bridge before it commits the
  row.

In the firmware those are `mqtt_publish_event_live()` in
`firmware/canary/lib/securacv_mqtt/src/securacv_mqtt.cpp` and
`csi_event_egress_pump()` in `firmware/canary/src/csi_event_egress.cpp`.
Reviewing F37, someone deleted the refusal and gated the bridge on the
backlog, and every host test and lint stayed green. This check is the guard
those two edits lacked. The re-review then got past its first rule set with
a bridge gated on `s_replay_run`, an `if (false)` decoy in front of a bridge
moved after the commit, a zero id floor in `current_link()` or `begin()`,
and a deleted epoch bump or `not_owed()` call; rules 3-6 refuse those.

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
   publishes its tamper bridge (`mqtt_publish_tamper(`, once in the pump)
   BEFORE the planner commits it (`s_backfill.commit(`). Between the
   dequeue and that publish there is no control flow (no `if`, `continue`,
   `return`, ...) and nothing reads the backfill's state (`s_backfill`,
   `s_replay_run`, `s_dest_epoch`, `s_port`, `csi_event_log::`) or the link
   state (`link.connected`, `mqtt_connected(`); `s_backfill.pending(` is
   not read anywhere before it. The `if` around the publish tests only
   `link.accepting`, that the body built, and the boot-story filter — so an
   `if (false ...` decoy, or a gate on the backlog, is refused. So a tamper
   alert never waits on the card or the backlog, and it queues through an
   outage. The bridge publishes with retained=false: a retained copy would
   re-fire Home Assistant's edge-latched tamper sensors on every restart. The backfill pass (`s_backfill.pass(`) runs after the row is
   committed.

The host test also takes three glue values from its model that the planner
cannot check for itself:

4. The id floor. `current_link()` sets `link.id_floor` once, from
   `s_id_floor_stored` (the allocator's floor as NVS holds it), and the pump
   takes its link from `current_link()`; `csi_event_egress_begin()` calls
   `restore_event_id_floor()` and then hands `s_id_floor_stored` to
   `s_backfill.begin(` as its floor. A zero there would let the ceiling pass
   the floor once the two strides fall out of step (a failed NVS write),
   and a new boot's first ids would read as delivered.
5. The broker-change epoch, in the pump. Before any row is dequeued, an
   `if` whose `||` condition holds `!link.accepting` and a comparison of
   `s_dest_epoch` with `mqtt_destination_epoch()` calls
   `s_backfill.not_owed(` and stores the new epoch. Without it a new broker
   is sent the old one's backlog.
6. The epoch itself, in securacv_mqtt.cpp. `apply_pending_reload()` bumps
   `s_destination_epoch` under an `if` on the same flag that guards the
   offline queue's flush (`s_offline_q.clear(`), and
   `mqtt_destination_epoch()` returns it.

Rule 3 holds the stretch from the dequeue to the bridge. The final review
got past it with edits that never reach that stretch: a dequeue loop that
stops, or a budget of zero, while a backfill runs; an early `return` from
the pump; a link muted before the loop; and a boot-story filter that
swallows every kind. Each one makes every tamper alert (and every row) wait
for the whole backlog, and a long backfill then fills the 8-deep egress
queue and drops them. So:

7. The pump's prefix. Its link is `const` and comes from `current_link()`.
   The dequeue loop's header is exactly
   `for (int budget = kPumpBudget; budget > 0; --budget)`. Before that
   loop, outside the card-poll `switch` and the epoch `if` (rule 5), no
   statement reads the backfill's or the link's state (the rule-3 list),
   and nothing leaves or loops (`return`, `continue`, `break`, `goto`,
   `for`, `while`, `do`) but the opening `if (!s_queue) return;`.
   `boot_story_bridged_elsewhere()` is one `return` of
   `strcmp(kind, "...") == 0` terms, naming exactly the three boot kinds
   the system.integrity story already narrates (`power_loss`, `watchdog`,
   `unexpected_reboot`).

## It proves it bites

Each run applies a set of mutations to the sources, in memory, and requires
the check to fail on every one. The reviewers' edits are in that set. So
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


def enclosing_if(text: str, pos: int) -> tuple[int, str, int, int] | None:
    """The innermost `if (...)` whose condition or controlled statement holds
    `pos`: (where the `if` starts, its condition, where its statement starts,
    where it ends)."""
    for m in reversed([m for m in re.finditer(r"\bif\s*\(", text) if m.start() < pos]):
        close = matching_paren(text, m.end() - 1)
        if close < 0:
            continue
        k = close + 1
        while k < len(text) and text[k].isspace():
            k += 1
        if k < len(text) and text[k] == "{":
            depth, end = 0, -1
            for j in range(k, len(text)):
                if text[j] == "{":
                    depth += 1
                elif text[j] == "}":
                    depth -= 1
                    if depth == 0:
                        end = j + 1
                        break
        else:
            end = text.find(";", k)
            end = end + 1 if end >= 0 else -1
        if end >= 0 and m.end() <= pos < end:
            return m.start(), text[m.end():close], close + 1, end
    return None


def call_args(text: str, call: str) -> list[str] | None:
    """The top-level arguments of the first `call` (e.g. `f(`), squashed."""
    at = text.find(call)
    if at < 0:
        return None
    close = matching_paren(text, at + len(call) - 1)
    if close < 0:
        return None
    return [squash(a) for a in top_level_terms(text[at + len(call):close], ",")]


# Signatures, matched against the blanked source.
SIG_LIVE = r"\bbool\s+mqtt_publish_event_live\s*\([^)]*\)"
SIG_PUMP = r"\bvoid\s+csi_event_egress_pump\s*\(\s*(?:void)?\s*\)"
SIG_BEGIN = r"\bvoid\s+csi_event_egress_begin\s*\(\s*(?:void)?\s*\)"
SIG_CURRENT_LINK = r"\bcsi_event_backfill::Link\s+current_link\s*\(\s*(?:void)?\s*\)"
SIG_SEND_LIVE = r"\bsend_live\s*\([^)]*\)\s*(?:override\s*)?"
SIG_SEND_BACKFILL = r"\bsend_backfill\s*\([^)]*\)\s*(?:override\s*)?"
SIG_RELOAD = r"\bvoid\s+apply_pending_reload\s*\(\s*(?:void)?\s*\)"
SIG_EPOCH = r"\buint32_t\s+mqtt_destination_epoch\s*\(\s*(?:void)?\s*\)"
SIG_BOOT_STORY = r"\bbool\s+boot_story_bridged_elsewhere\s*\(\s*const\s+char\s*\*\s*kind\s*\)"

# What the tamper bridge's `if` may test (each `&&` term, squashed).
BRIDGE_TERMS = (
    r"link\.accepting",
    r"csi_event_wire::build_tamper_bridge_body\(.*\)>0",
    r"!boot_story_bridged_elsewhere\(.*\)",
)
# The backfill's and the link's state: none of it may stand between a
# dequeued row and its tamper bridge.
BACKLOG_STATE = ("s_backfill", "s_replay_run", "s_dest_epoch", "s_port", "csi_event_log::",
                 "link.connected", "mqtt_connected(")
CONTROL_FLOW = r"\b(?:if|else|for|while|do|switch|return|continue|break|goto)\b"
# The allocator's floor as NVS holds it, read atomically or plainly.
FLOOR_READ = r"__atomic_load_n\(&s_id_floor_stored,[A-Z_]+\)|s_id_floor_stored"
# Rule 7: the dequeue loop, squashed; what may not happen before it; and the
# kinds the boot-story filter may name (system.integrity narrates these).
PUMP_LOOP = "for(intbudget=kPumpBudget;budget>0;--budget)"
PREFIX_FLOW = r"\b(?:return|continue|break|goto|for|while|do)\b"
BOOT_STORY_KINDS = {"power_loss", "watchdog", "unexpected_reboot"}


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
    bridge_args = call_args(body, "mqtt_publish_tamper(")
    if bridge_args is None or len(bridge_args) != 2 or bridge_args[1] != "false":
        errors.append(f"{where}: the tamper bridge publishes with retained=false — an event, "
                      "not a state; a retained copy re-fires HA's edge-latched tamper sensors on "
                      "every HA restart")
    if body.count("mqtt_publish_tamper(") != 1:
        errors.append(f"{where}: expected one tamper bridge publish (mqtt_publish_tamper), "
                      f"found {body.count('mqtt_publish_tamper(')} — a second one is either a "
                      "decoy or a duplicate alert")
    between = body[deq:tamper]
    for gate in BACKLOG_STATE:
        if gate in between:
            errors.append(f"{where}: the tamper bridge must not be gated on `{gate}` — it "
                          "publishes (or queues through an outage) whatever the backfill is doing")
    if "s_backfill.pending(" in body[:tamper]:
        errors.append(f"{where}: s_backfill.pending() is read before the tamper bridge — "
                      "the bridge must not depend on the backlog")
    # Nothing between the dequeue statement and the bridge's `if` may branch
    # (a `continue` would skip the bridge), and that `if` may test only what
    # BRIDGE_TERMS allows (an `if (false ...` decoy is refused here).
    dequeue_if = enclosing_if(body, deq)
    bridge_if = enclosing_if(body, tamper)
    if dequeue_if is None or bridge_if is None or bridge_if[0] < dequeue_if[3] or \
            not bridge_if[2] <= tamper < bridge_if[3]:
        errors.append(f"{where}: the row dequeue and the tamper bridge must each be an `if` "
                      "statement, the bridge's after the dequeue's, with the publish as its "
                      "statement")
    else:
        flow = re.search(CONTROL_FLOW, body[dequeue_if[3]:bridge_if[0]])
        if flow:
            errors.append(f"{where}: `{flow.group(0)}` between the row dequeue and the tamper "
                          "bridge — every dequeued row reaches the bridge")
        for term in top_level_terms(unwrap(bridge_if[1]), "&&"):
            if not any(re.fullmatch(ok, unwrap(term)) for ok in BRIDGE_TERMS):
                errors.append(f"{where}: the tamper bridge is gated on `{unwrap(term)}` — it may "
                              "test only link.accepting, that the body built and the boot-story "
                              "filter")
    passes = body.find("s_backfill.pass(", commit if commit >= 0 else 0)
    if passes < 0:
        errors.append(f"{where}: the backfill pass (s_backfill.pass) must run after the rows "
                      "are committed")


def check_floor_glue(egress_src: str, errors: list[str]) -> None:
    """Rule 4: the planner is given the allocator's floor, at boot and every pass."""
    code = blank_comments_and_strings(egress_src)
    span = the_body(code, SIG_CURRENT_LINK, f"{EGRESS_CPP}: current_link()", errors)
    if span is not None:
        body = code[span[0]:span[1]]
        sets = re.findall(r"\blink\.id_floor\s*=(?!=)([^;]*);", body)
        if len(sets) != 1 or not re.fullmatch(FLOOR_READ, squash(sets[0])):
            errors.append(
                f"{EGRESS_CPP}: current_link() must set link.id_floor once, from "
                "s_id_floor_stored (the allocator's floor as NVS holds it) — the planner caps its "
                "ceiling there, so a new boot's first ids are never read as delivered")
    span = the_body(code, SIG_PUMP, f"{EGRESS_CPP}: csi_event_egress_pump()", errors,
                    need="xQueueReceive(")
    if span is not None:
        body = code[span[0]:span[1]]
        if not re.search(r"\blink\s*=\s*current_link\s*\(\s*\)", body) or \
                re.search(r"\blink\.id_floor\s*=(?!=)", body):
            errors.append(f"{EGRESS_CPP}: csi_event_egress_pump() must take its link, id floor "
                          "included, from current_link()")
    span = the_body(code, SIG_BEGIN, f"{EGRESS_CPP}: csi_event_egress_begin()", errors)
    if span is not None:
        body = code[span[0]:span[1]]
        args = call_args(body, "s_backfill.begin(")
        restore = body.find("restore_event_id_floor(")
        if args is None or len(args) != 3 or not re.fullmatch(FLOOR_READ, args[1]):
            errors.append(f"{EGRESS_CPP}: csi_event_egress_begin() must hand s_id_floor_stored "
                          "to s_backfill.begin() as its floor (the first-boot record starts "
                          "there)")
        elif restore < 0 or restore > body.find("s_backfill.begin("):
            errors.append(f"{EGRESS_CPP}: csi_event_egress_begin() must restore the id floor "
                          "from NVS (restore_event_id_floor) before s_backfill.begin() reads it")


def check_epoch_glue(egress_src: str, errors: list[str]) -> None:
    """Rule 5: a changed (or removed) broker drops the backlog, before any row."""
    code = blank_comments_and_strings(egress_src)
    span = the_body(code, SIG_PUMP, f"{EGRESS_CPP}: csi_event_egress_pump()", errors,
                    need="xQueueReceive(")
    if span is None:
        return
    body = code[span[0]:span[1]]
    where = f"{EGRESS_CPP}: csi_event_egress_pump()"
    call = body.find("s_backfill.not_owed(")
    guard = enclosing_if(body, call) if call >= 0 else None
    if guard is None or body.count("s_backfill.not_owed(") != 1:
        errors.append(f"{where}: must call s_backfill.not_owed() once, under an `if` on the "
                      "broker (no broker, or a changed one) — what waited for one broker is not "
                      "the next one's to see")
        return
    epochs = ["mqtt_destination_epoch()"] + [
        m.group(1) for m in re.finditer(r"\b(\w+)\s*=\s*mqtt_destination_epoch\s*\(\s*\)", body)]
    terms = [unwrap(t) for t in top_level_terms(unwrap(guard[1]), "||")]
    compares = any(t in (f"{e}!=s_dest_epoch", f"s_dest_epoch!={e}") for t in terms for e in epochs)
    if "!link.accepting" not in terms or not compares:
        errors.append(f"{where}: s_backfill.not_owed() must run when no broker is configured "
                      "(!link.accepting) OR the destination epoch moved (s_dest_epoch != "
                      "mqtt_destination_epoch()), as one `||` condition")
    if not re.search(r"\bs_dest_epoch\s*=(?!=)", body[guard[2]:guard[3]]):
        errors.append(f"{where}: the not_owed() branch must store the new epoch in s_dest_epoch, "
                      "or it drops the backlog on every pass after a broker change")
    deq = body.find("xQueueReceive(")
    if deq >= 0 and call > deq:
        errors.append(f"{where}: s_backfill.not_owed() must run before the rows are dequeued, "
                      "so a row committed for the new broker is not dropped with the old backlog")


def check_destination_epoch(mqtt_src: str, errors: list[str]) -> None:
    """Rule 6: the epoch moves exactly when the offline queue would be flushed."""
    code = blank_comments_and_strings(mqtt_src)
    span = the_body(code, SIG_EPOCH, f"{MQTT_CPP}: mqtt_destination_epoch()", errors)
    if span is not None and squash(code[span[0]:span[1]]) != "returns_destination_epoch;":
        errors.append(f"{MQTT_CPP}: mqtt_destination_epoch() must return s_destination_epoch")
    span = the_body(code, SIG_RELOAD, f"{MQTT_CPP}: apply_pending_reload()", errors)
    if span is None:
        return
    body = code[span[0]:span[1]]
    bumps = list(re.finditer(r"\+\+\s*s_destination_epoch\b|\bs_destination_epoch\s*\+\+", body))
    clear = body.find("s_offline_q.clear(")
    bump_if = enclosing_if(body, bumps[0].start()) if len(bumps) == 1 else None
    clear_if = enclosing_if(body, clear) if clear >= 0 else None
    flag = unwrap(bump_if[1]) if bump_if else ""
    if not re.fullmatch(r"\w+", flag) or clear_if is None or \
            flag not in [unwrap(t) for t in top_level_terms(unwrap(clear_if[1]), "&&")]:
        errors.append(
            f"{MQTT_CPP}: apply_pending_reload() must bump s_destination_epoch once, under an "
            "`if` on the same flag that flushes the offline queue (s_offline_q.clear) — the SD "
            "event log's backfill drops its backlog on exactly the broker changes the queue does")


def check_pump_prefix(egress_src: str, errors: list[str]) -> None:
    """Rule 7: nothing before the dequeue loop can hold the rows back."""
    code = blank_comments_and_strings(egress_src)
    span = the_body(code, SIG_PUMP, f"{EGRESS_CPP}: csi_event_egress_pump()", errors,
                    need="xQueueReceive(")
    if span is None:
        return
    body = code[span[0]:span[1]]
    where = f"{EGRESS_CPP}: csi_event_egress_pump()"
    if not re.search(r"\bconst\s+csi_event_backfill::Link\s+link\s*=\s*current_link\s*\(\s*\)\s*;",
                     body) or re.search(r"\blink\s*\.\s*\w+\s*(?:[-+*/|&^]?=(?!=)|\+\+|--)", body):
        errors.append(f"{where}: the pump's link must be `const csi_event_backfill::Link link = "
                      "current_link();` and never written — a link muted while a backfill runs "
                      "holds every tamper alert behind the backlog")
    deq = body.find("xQueueReceive(")
    loops = [m for m in re.finditer(r"\bfor\s*\(", body) if m.start() < deq]
    if not loops:
        errors.append(f"{where}: the row dequeue must sit in the pump's budget loop")
        return
    loop = loops[-1]
    close = matching_paren(body, loop.end() - 1)
    if close < 0 or squash(body[loop.start():close + 1]) != PUMP_LOOP:
        errors.append(f"{where}: the dequeue loop's header must be exactly `{PUMP_LOOP}` "
                      "(squashed) — a budget or condition that looks at the backlog stops the "
                      "rows, tamper alerts included, while a backfill runs")
    # Before the loop: blank out the statements rules 3-5 already hold (the
    # card-poll switch, the epoch `if`) and the opening `if (!s_queue) return;`,
    # then refuse backlog/link state and any way out.
    prefix = list(body[:loop.start()])

    def blank(a: int, b: int) -> None:
        for k in range(a, b):
            if prefix[k] != "\n":
                prefix[k] = " "

    opening = re.match(r"(?:\s*#[^\n]*\n)*\s*if\s*\(\s*!\s*s_queue\s*\)\s*return\s*;", body)
    if opening:
        blank(0, opening.end())
    sw = re.search(r"\bswitch\s*\(\s*csi_event_log::poll\(", body[:loop.start()])
    if sw:
        close_sw = matching_paren(body, body.find("(", sw.start()))
        open_b = body.find("{", close_sw)
        depth, end_b = 0, -1
        for j in range(open_b, loop.start()):
            if body[j] == "{":
                depth += 1
            elif body[j] == "}":
                depth -= 1
                if depth == 0:
                    end_b = j + 1
                    break
        if close_sw > 0 and end_b > 0:
            blank(sw.start(), end_b)
    call = body.find("s_backfill.not_owed(")
    epoch_if = enclosing_if(body, call) if 0 <= call < loop.start() else None
    if epoch_if:
        blank(epoch_if[0], epoch_if[3])
    rest = "".join(prefix)
    for gate in BACKLOG_STATE:
        if gate in rest:
            errors.append(f"{where}: `{gate}` is read before the dequeue loop, outside the card "
                          "poll and the broker-change check — the rows must not wait on it")
    flow = re.search(PREFIX_FLOW, rest)
    if flow:
        errors.append(f"{where}: `{flow.group(0)}` before the dequeue loop — only "
                      "`if (!s_queue) return;` may leave the pump before its rows")


def check_boot_story_filter(egress_src: str, errors: list[str]) -> None:
    """Rule 7: the bridge's one filter drops exactly the boot kinds."""
    code = blank_comments_and_strings(egress_src)
    span = the_body(code, SIG_BOOT_STORY, f"{EGRESS_CPP}: boot_story_bridged_elsewhere()", errors)
    if span is None:
        return
    shape = squash(code[span[0]:span[1]])
    kinds = re.findall(r'strcmp\(\s*kind\s*,\s*"([^"\\]*)"\s*\)', egress_src[span[0]:span[1]])
    if not re.fullmatch(r'returnstrcmp\(kind,""\)==0(?:\|\|strcmp\(kind,""\)==0)*;', shape) or \
            set(kinds) != BOOT_STORY_KINDS or len(kinds) != len(BOOT_STORY_KINDS):
        errors.append(
            f"{EGRESS_CPP}: boot_story_bridged_elsewhere() must be one `return` of "
            "`strcmp(kind, \"...\") == 0` terms naming exactly "
            f"{sorted(BOOT_STORY_KINDS)} — any other test, or kind, silences tamper bridges")


def check(mqtt_src: str, egress_src: str) -> list[str]:
    errors: list[str] = []
    check_live_publish(mqtt_src, errors)
    check_port_sends(egress_src, errors)
    check_pump_order(egress_src, errors)
    check_floor_glue(egress_src, errors)
    check_epoch_glue(egress_src, errors)
    check_destination_epoch(mqtt_src, errors)
    check_pump_prefix(egress_src, errors)
    check_boot_story_filter(egress_src, errors)
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
    # The re-review's edits: gates and decoys the first rule set let through.
    ("tamper bridge gated on the backfill's replay run",
     lambda m, e: (m, mutate_in(e, SIG_PUMP, r"if\s*\(\s*link\.accepting\s*&&",
                                "if (link.accepting && s_replay_run == 0 &&",
                                need="xQueueReceive("))),
    ("tamper bridge moved after the commit behind an `if (false)` decoy",
     lambda m, e: (m, mutate_in(
         e, SIG_PUMP,
         r"(if\s*\(\s*link\.accepting\s*&&.*?mqtt_publish_tamper\([^;]*;\s*\})"
         r"(.*?\(void\)\s*s_backfill\.commit\([^;]*;)",
         r"if (false) mqtt_publish_tamper(tb, false);\2 \1", need="xQueueReceive("))),
    ("a dequeued row can skip its tamper bridge",
     lambda m, e: (m, mutate_in(e, SIG_PUMP, r"(char\s+tb\[128\]\s*;)",
                                r"\1 if (millis() == 0) continue;", need="xQueueReceive("))),
    ("current_link() drops the id floor",
     lambda m, e: (m, mutate_in(e, SIG_CURRENT_LINK, r"(link\.id_floor\s*=)[^;]*;", r"\1 0;"))),
    ("the pump overrides the link's id floor",
     lambda m, e: (m, mutate_in(e, SIG_PUMP,
                                r"const\s+(csi_event_backfill::Link\s+link\s*=\s*current_link\(\)\s*;)",
                                r"\1 link.id_floor = 0;", need="xQueueReceive("))),
    ("the pump builds its own link with no floor",
     lambda m, e: (m, mutate_in(e, SIG_PUMP, r"=\s*current_link\(\)\s*;",
                                "= csi_event_backfill::Link{mqtt_accepting(), mqtt_connected(), "
                                "0, millis()};", need="xQueueReceive("))),
    ("begin() hands the planner no floor",
     lambda m, e: (m, mutate_in(e, SIG_BEGIN, r"(s_backfill\.begin\(\s*ceiling\s*,).*?(,\s*s_port\s*\))",
                                r"\1 0\2"))),
    ("the id floor is restored after the planner starts",
     lambda m, e: (m, mutate_in(
         mutate_in(e, SIG_BEGIN, r"restore_event_id_floor\(\)\s*;", ""),
         SIG_BEGIN, r"(s_backfill\.begin\([^;]*;)", r"\1 restore_event_id_floor();"))),
    ("a broker change keeps the old backlog",
     lambda m, e: (m, mutate_in(e, SIG_PUMP, r"\n[ \t]*s_backfill\.not_owed\([^;]*;", "",
                                need="xQueueReceive("))),
    ("the destination epoch is never compared",
     lambda m, e: (m, mutate_in(e, SIG_PUMP, r"\s*\|\|\s*epoch\s*!=\s*s_dest_epoch", "",
                                need="xQueueReceive("))),
    ("the new destination epoch is never stored",
     lambda m, e: (m, mutate_in(e, SIG_PUMP, r"\n[ \t]*s_dest_epoch\s*=\s*epoch\s*;", "",
                                need="xQueueReceive("))),
    ("the old backlog is dropped after the rows",
     lambda m, e: (m, mutate_in(
         e, SIG_PUMP, r"(if\s*\(\s*!link\.accepting[^{]*\{[^}]*\})(.*?)(const\s+size_t\s+replayed)",
         r"\2\1 \3", need="xQueueReceive("))),
    ("the destination epoch is never bumped",
     lambda m, e: (mutate_in(m, SIG_RELOAD,
                             r"if\s*\(\s*destination_changed\s*\)\s*s_destination_epoch\+\+\s*;",
                             ""), e)),
    ("the epoch moves only when the queue held records",
     lambda m, e: (mutate_in(m, SIG_RELOAD, r"if\s*\(\s*destination_changed\s*\)",
                             "if (!s_offline_q.empty() && destination_changed)"), e)),
    ("mqtt_destination_epoch() returns a constant",
     lambda m, e: (mutate_in(m, SIG_EPOCH, r"return\s+s_destination_epoch\s*;", "return 0;"), e)),
    ("the tamper bridge is retained",
     lambda m, e: (m, mutate_in(e, SIG_PUMP, r"(mqtt_publish_tamper\(\s*tb\s*,[^;]*?)false\s*\)",
                                r"\1true)", need="xQueueReceive("))),
    # The final review's edits: the rows held back before they reach rule 3.
    ("the dequeue loop stops while a backfill runs",
     lambda m, e: (m, mutate_in(e, SIG_PUMP, r"budget\s*>\s*0\s*;", "budget > 0 && s_replay_run == 0;",
                                need="xQueueReceive("))),
    ("the dequeue budget is zero while a backfill runs",
     lambda m, e: (m, mutate_in(e, SIG_PUMP, r"int\s+budget\s*=\s*kPumpBudget",
                                "int budget = s_replay_run ? 0 : kPumpBudget",
                                need="xQueueReceive("))),
    ("the pump returns before its rows while a backfill runs",
     lambda m, e: (m, mutate_in(e, SIG_PUMP, r"(CommittedEvent\s+ev\s*;)",
                                r"if (s_replay_run > 0) { s_replay_run += s_backfill.pass(link, s_port); "
                                r"return; } \1", need="xQueueReceive("))),
    ("the pump returns before its rows on a helper",
     lambda m, e: (m, mutate_in(e, SIG_PUMP, r"(CommittedEvent\s+ev\s*;)",
                                r"if (backlog_busy()) return; \1", need="xQueueReceive("))),
    ("the pump mutes its link while a backfill runs",
     lambda m, e: (m, mutate_in(e, SIG_PUMP,
                                r"const\s+(csi_event_backfill::Link\s+link\s*=\s*current_link\(\)\s*;)",
                                r"\1 if (s_replay_run) link.accepting = false;",
                                need="xQueueReceive("))),
    ("the boot-story filter swallows every kind while a backfill runs",
     lambda m, e: (m, mutate_in(e, SIG_BOOT_STORY, r"return\s+strcmp\(",
                                "if (backlog_busy()) return true;\n  return strcmp("))),
    ("the boot-story filter swallows an SD kind",
     lambda m, e: (m, mutate_in(e, SIG_BOOT_STORY, r"(==\s*0)\s*;",
                                r'\1 || strcmp(kind, "sd_removed") == 0;'))),
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
          f"planner's sends never buffer, the tamper bridge goes first, the planner gets the "
          f"id floor and the broker-change epoch ({len(MUTATIONS)} mutations refused).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
