#!/usr/bin/env python3
"""Hold the canary-wap's event egress glue to what its host test models.

Backlog F78: the canary-wap's live publish overtook its own backlog, and
its live path (on whichever task committed, the NimBLE host task for a
ble.scout close, under the chokepoint's commit lock) and its backfill (on
the loop task) both wrote the delivery watermark with no lock between them.
The fix moved every decision into
`firmware/projects/canary-wap/arduino/canary_wap/csi_event_egress.cpp`, on
`csi_event_backfill.h`'s Planner, and
`firmware/projects/canary-wap/tests_host/test_wap_event_egress.cpp` drives
it against the real SD event log and CSI library.

That test cannot compile three files: `csi_integration.cpp` (the commit
hook, the boot order), `csi_mqtt.cpp` (the loop's pump, the esp_mqtt event
handler, the broker-change epoch) and the device-only paths of the egress
itself. It models them. This check holds the source to the model, and to
the egress's own rules the test reaches only through behavior.

## The rules

1. The commit hook. `csi_integration.cpp`'s `csi_event_on_committed()`
   hands the row to `csi_event_egress::on_committed(` exactly once, after
   its privacy gate (`if (privacy > csi_event_get_privacy_ceiling())
   return;`), names no other `csi_event_egress::` function, and names
   nothing from `csi_mqtt::` or `csi_event_log::`: it may run on the NimBLE
   host task under the commit lock, where a publish, a pump or a card write
   is the bug.
2. The egress's `on_committed()` only enqueues: one `xQueueSend(` with a
   zero timeout (never blocks the committing task), and nothing from
   `csi_mqtt::`, `csi_event_log::`, `Preferences`, the egress state
   (`g_state`, `planner`) or its loop-task paths (`route(`, `flush_held(`,
   `pump(`, `current_link(`).
3. Boot order. In `csi_integration::init`, `csi_event_egress::begin();`
   runs exactly once, after `apply_event_id_floor_from_nvs()`: the planner
   restores its watermark with the floor NVS holds. Both come before the
   one `register_v1_modules();` (sweep F83): a module that emits while it
   registers (ble_scout_init() reports its init) must allocate from the
   restored floor, never write a floor from the id space's base over the
   persisted one, and must find the egress's queue there. The canary does
   the same in `csi_event_egress_begin()`, before its modules. Then
   (sweep F93) the modules' boot init: `csi_module_init_all(nullptr);`
   once, a statement of `init()`'s own body (no `if`, no block, no `#if`
   around it), after `register_v1_modules();` and before
   `csi_set_features_callback(`, the first CSI window's tick (the library
   ticks no module before its init, so a late init is a dead pipeline and
   a skipped one a device on defaults). It is the sketch's only call: one
   in `register_v1_modules()`, `reinit_module()` or a handler would run a
   module's boot init on another path (`reinit_module()` re-runs one
   module's `init()` directly).
4. The loop task. `csi_mqtt::loop()` calls `csi_event_egress::pump();`.
   The esp_mqtt event handler (`mqtt_event_handler`) names nothing from
   `csi_event_egress::` and publishes no row, `publish_event_row(`
   appears in `csi_mqtt.cpp` only where it is defined, and the one
   `build_topic(..., "events")` is inside it: every events publish is the
   egress's.
5. The link and the floor. `current_link()` sets `link.id_floor` from
   `csi_integration::event_id_floor_stored()` and `link.id_next` from
   `csi_event_get_next_event_id()`, once each; `pump()` takes its link,
   const, from `current_link()`; `begin()` hands `planner.begin(` the
   ceiling it read from `csi_mqtt::NVS_KEY_DELIVERED` and
   `csi_integration::event_id_floor_stored()`.
6. The port's sends. `WapPort::send_live()` refuses
   (`if (m_st->held_count > 0) return Sent::kNotNow;`) before it
   publishes: rows waiting in RAM are older. `WapPort::send_backfill()`
   refuses a dismissal line (`if (rec.values.dismissed != 0) return
   Sent::kNever;`) and sends the RAM rows below its row
   (`if (!send_held_below(rec.event_id)) return Sent::kNotNow;`) before it
   publishes.
7. The pump's order. The dequeue loop's header is exactly
   `for (int budget = kPumpBudget; budget > 0; --budget)`; in it, between
   the `xQueueReceive(` statement and `publish_tamper_bridge(` there is no
   control flow and no backlog or link state, so a tamper alert never
   waits; `route(` comes after the bridge. After the loop,
   `planner.pass(` and then `flush_held(`.
8. The RAM hold. In `route()`, before `st.planner.commit(`, an `if` holds
   the row (`st.push(` then `return;`) when one `&&` term is the `||` chain
   of `st.planner.pending()`, `st.card_wait`, `st.held_count > 0` and
   `!link.connected`: a row the card does not keep waits while anything
   older waits (on the card, on a card that is not open now, in RAM) or the
   link is down, and is never handed to the planner (which would write the
   NVS ceiling past the rows waiting on the card).
9. One pump, one begin, sketch-wide. Across the sketch's `.cpp`, `.h` and
   `.ino` files (comments and strings blanked), `csi_event_egress::pump(`
   appears exactly once, in `csi_mqtt::loop()`, and
   `csi_event_egress::begin(` exactly once, in `csi_integration::init`; no
   file says `using namespace csi_event_egress`. A second caller (an httpd
   handler, the esp_mqtt task, the commit hook) would run the egress's
   publishes, card writes and watermark on a second task.
10. The planner's not-on-card hand-over. `WapPort::hand_to_queue()`'s live
    publish (the one after its `if (flushing)` block) sits in an `if` whose
    `&&` terms include `!deferred`, `!m_st->planner.pending()` and
    `m_st->held_count == 0`: a row whose append failed goes live only when
    nothing older waits on the card or in RAM.
11. The broker-change epoch. In `csi_mqtt::init()`,
    `destination_digest(s_active_cfg)` is taken before
    `config_load(&s_active_cfg)` and compared after it, and a difference
    with `s_dest_known` set bumps `s_dest_epoch.fetch_add(`; `s_dest_known
    = true;` follows. `destination_epoch()` returns `s_dest_epoch`. The
    egress's side (a changed epoch drops the backlog) is host-tested.

## It proves it bites

Each run applies mutations to the three sources in memory and requires the
check to fail on every one. A mutation whose anchor moved fails the run.

Run locally:  python3 firmware/scripts/check_wap_event_egress.py   (repo root)
CI:           firmware.yml "CSI Sketch Copy Sync", via check_csi_sync.sh
"""

from __future__ import annotations

import re
import sys
from functools import lru_cache
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_event_egress_order import (  # noqa: E402  (shared C++ scanning helpers)
    AnchorMissing,
    blank_comments_and_strings,
    bodies,
    call_args,
    matching_paren,
    mutate_in,
    squash,
    the_body,
    top_level_statement,
    top_level_terms,
    unwrap,
)

# The sketch-wide rules blank every sketch file on every check() call, and
# the self-test calls check() once per mutation; the other files never change
# between calls, so blank each distinct source once.
blank_cached = lru_cache(maxsize=None)(blank_comments_and_strings)

REPO = Path(__file__).resolve().parents[2]
SKETCH = "firmware/projects/canary-wap/arduino/canary_wap"
INTEG_CPP = f"{SKETCH}/csi_integration.cpp"
EGRESS_CPP = f"{SKETCH}/csi_event_egress.cpp"
MQTT_CPP = f"{SKETCH}/csi_mqtt.cpp"
SKETCH_GLOBS = ("*.cpp", "*.h", "*.ino")

SIG_HOOK = r"\bvoid\s+csi_event_on_committed\s*\([^)]*\)"
SIG_INIT = r"\bbool\s+init\s*\(\s*httpd_handle_t\s+server[^)]*\)"
SIG_ON_COMMITTED = r"\bvoid\s+on_committed\s*\([^)]*\)"
SIG_BEGIN = r"\bvoid\s+begin\s*\(\s*\)"
SIG_CURRENT_LINK = r"\bLink\s+current_link\s*\(\s*\)"
SIG_SEND_LIVE = r"\bSent\s+WapPort::send_live\s*\([^)]*\)"
SIG_SEND_BACKFILL = r"\bSent\s+WapPort::send_backfill\s*\([^)]*\)"
SIG_PUMP = r"\bvoid\s+pump\s*\(\s*\)"
SIG_ROUTE = r"\bvoid\s+route\s*\([^)]*\)"
SIG_LOOP = r"\bvoid\s+loop\s*\(\s*\)"
SIG_HANDLER = r"\bvoid\s+mqtt_event_handler\s*\([^)]*\)"
SIG_PUBLISH_ROW = r"\bEventSend\s+publish_event_row\s*\([^)]*\)"
SIG_HAND_TO_QUEUE = r"\bbool\s+WapPort::hand_to_queue\s*\([^)]*\)"
SIG_MQTT_INIT = r"\bbool\s+init\s*\(\s*const\s+char\s*\*\s*device_id[^)]*\)"
SIG_DEST_EPOCH = r"\buint32_t\s+destination_epoch\s*\(\s*\)"
SIG_DISMISS = r"\besp_err_t\s+handle_events_dismiss\s*\([^)]*\)"
SIG_INTEG_LOOP = r"\bvoid\s+loop\s*\(\s*bool\s+run_csi\s*\)"
SIG_REINIT = r"\bvoid\s+reinit_module\s*\(\s*const\s+char\s*\*\s*module_id\s*\)"
SIG_REGISTER = r"\bvoid\s+register_v1_modules\s*\(\s*\)"

CONTROL_FLOW = r"\b(?:if|else|for|while|do|switch|return|continue|break|goto)\b"
PUMP_LOOP = "for(intbudget=kPumpBudget;budget>0;--budget)"
ENQUEUE_ONLY_FORBIDDEN = ("csi_mqtt::", "csi_event_log::", "Preferences", "g_state", "planner",
                          "route(", "flush_held(", "pump(", "current_link(")
BRIDGE_GAP_FORBIDDEN = ("planner", "held", "replay_run", "link.connected", "csi_mqtt::connected(",
                        "csi_event_log::")
HOLD_TERMS = {"st.planner.pending()", "st.card_wait", "st.held_count>0", "!link.connected"}
HAND_OVER_TERMS = {"!deferred", "!m_st->planner.pending()", "m_st->held_count==0"}
PRIVACY_GATE = "privacy>csi_event_get_privacy_ceiling()"


def body_of(code: str, sig: str, what: str, errors: list[str]) -> str | None:
    span = the_body(code, sig, what, errors)
    return None if span is None else code[span[0]:span[1]]


def ifs_returning(text: str, ret: str) -> list[str]:
    """The squashed conditions of every `if (<cond>) [{] <ret>` in `text`."""
    conds = []
    for m in re.finditer(r"\bif\s*\(", text):
        close = matching_paren(text, m.end() - 1)
        if close < 0:
            continue
        if re.match(r"\s*\{?\s*" + ret, text[close + 1:]):
            conds.append(unwrap(text[m.end():close]))
    return conds


def check_boot_init_callers(integ: str, others: dict[str, str], errors: list[str]) -> None:
    """Rule 3, F93: the modules' boot init has one caller in the sketch."""
    files = dict(others)
    files[INTEG_CPP] = integ
    sites = []
    for name, src in files.items():
        code = blank_cached(src)
        for m in re.finditer(r"\bcsi_module_init_all\s*\(", code):
            # The staged library's own declaration and definition are not calls.
            if re.search(r"\bsize_t\s+$", code[:m.start()]):
                continue
            sites.append(name)
    if sites != [INTEG_CPP]:
        errors.append(f"{SKETCH}: `csi_module_init_all(` must be called exactly once in the sketch, "
                      f"in csi_integration::init() (found {len(sites)}: "
                      f"{', '.join(sorted(set(sites))) or 'none'}) — a second caller runs a "
                      "module's boot init on another path; a settings change re-runs one "
                      "module's init() through reinit_module() (F93)")


def check_hook(integ: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(integ)
    body = body_of(code, SIG_HOOK, f"{INTEG_CPP}: csi_event_on_committed()", errors)
    if body is None:
        return
    call = "csi_event_egress::on_committed("
    if body.count(call) != 1 or body.count("csi_event_egress::") != 1:
        errors.append(f"{INTEG_CPP}: csi_event_on_committed() must hand the row to "
                      f"{call} exactly once and name no other csi_event_egress:: function — "
                      "the hook may run on the NimBLE host task under the commit lock")
    else:
        at = body.find(call)
        if PRIVACY_GATE not in ifs_returning(body[:at], r"return\s*;"):
            errors.append(f"{INTEG_CPP}: csi_event_on_committed() must refuse a row above the "
                          "privacy ceiling (`if (privacy > csi_event_get_privacy_ceiling()) "
                          f"return;`) before {call}")
    for tok in ("csi_mqtt::", "csi_event_log::"):
        if tok in body:
            errors.append(f"{INTEG_CPP}: csi_event_on_committed() names `{tok}` — the hook may run "
                          "on the NimBLE host task under the commit lock; the egress publishes "
                          "and writes the card on the loop task (F78)")


def check_enqueue_only(egress: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(egress)
    body = body_of(code, SIG_ON_COMMITTED, f"{EGRESS_CPP}: on_committed()", errors)
    if body is None:
        return
    if body.count("xQueueSend(") != 1:
        errors.append(f"{EGRESS_CPP}: on_committed() must enqueue the row with one xQueueSend(")
    else:
        args = call_args(body, "xQueueSend(")
        if not args or args[-1] != "0":
            errors.append(f"{EGRESS_CPP}: on_committed()'s xQueueSend( must not wait (timeout 0) — "
                          "the committing task may be the NimBLE host task")
    for tok in ENQUEUE_ONLY_FORBIDDEN:
        if tok in body:
            errors.append(f"{EGRESS_CPP}: on_committed() names `{tok}` — it only copies the row "
                          "into the queue; the pump on the loop task does the rest (F78)")


def check_boot_order(integ: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(integ)
    body = body_of(code, SIG_INIT, f"{INTEG_CPP}: csi_integration::init()", errors)
    if body is None:
        return
    begin = "csi_event_egress::begin();"
    if body.count(begin) != 1:
        errors.append(f"{INTEG_CPP}: csi_integration::init() must call `{begin}` exactly once")
        return
    floor = body.find("apply_event_id_floor_from_nvs()")
    if floor < 0 or body.find(begin) < floor:
        errors.append(f"{INTEG_CPP}: csi_integration::init() must call `{begin}` after "
                      "apply_event_id_floor_from_nvs() — the planner restores its watermark with "
                      "the floor NVS holds")
    modules = "register_v1_modules();"
    if body.count(modules) != 1:
        errors.append(f"{INTEG_CPP}: csi_integration::init() must call `{modules}` exactly once")
    elif floor < 0 or body.find(modules) < floor or body.find(modules) < body.find(begin):
        errors.append(f"{INTEG_CPP}: csi_integration::init() must restore the event-id floor "
                      f"(apply_event_id_floor_from_nvs()) and call `{begin}` before `{modules}` — "
                      "a module that commits while it registers would allocate from the id "
                      "space's base and write that floor over the persisted one (F83)")
    boot_init = "csi_module_init_all(nullptr);"
    callback = body.find("csi_set_features_callback(")
    if body.count("csi_module_init_all(") != 1 or body.count(boot_init) != 1:
        errors.append(f"{INTEG_CPP}: csi_integration::init() must run the modules' boot init, "
                      f"`{boot_init}`, exactly once — registration initializes nothing, so a "
                      "saved preset, threshold or cooldown applies only after a settings change "
                      "(F93)")
    else:
        at = body.find(boot_init)
        if body.find(modules) < 0 or at < body.find(modules):
            errors.append(f"{INTEG_CPP}: csi_integration::init() must call `{boot_init}` after "
                          f"`{modules}` — it initializes only the modules registered so far (F93)")
        if callback < 0 or at > callback:
            errors.append(f"{INTEG_CPP}: csi_integration::init() must call `{boot_init}` before "
                          "csi_set_features_callback( installs the first tick — the library "
                          "ticks no module before its init (F93)")
        if not top_level_statement(body, at):
            errors.append(f"{INTEG_CPP}: `{boot_init}` must be a statement of "
                          "csi_integration::init()'s own body — no `if`, block or `#if` "
                          "around it: every boot initializes the modules (F93)")


def check_loop_task(mqtt: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(mqtt)
    loop = body_of(code, SIG_LOOP, f"{MQTT_CPP}: csi_mqtt::loop()", errors)
    if loop is not None and "csi_event_egress::pump();" not in loop:
        errors.append(f"{MQTT_CPP}: csi_mqtt::loop() must call csi_event_egress::pump(); — the "
                      "egress runs on the loop task")
    handler = body_of(code, SIG_HANDLER, f"{MQTT_CPP}: mqtt_event_handler()", errors)
    if handler is not None:
        for tok in ("csi_event_egress::", "publish_event_row("):
            if tok in handler:
                errors.append(f"{MQTT_CPP}: mqtt_event_handler() names `{tok}` — it runs on the "
                              "esp_mqtt task; a row published there races the backfill (F78)")
    if code.count("publish_event_row(") != 1:
        errors.append(f"{MQTT_CPP}: publish_event_row( appears outside its definition — every "
                      "events publish is the egress's")
    # The events topic is built in one place, publish_event_row's body. The
    # literal is a string, so it is found in the source and placed by the
    # blanked code (same offsets), which also drops matches inside comments.
    span = the_body(code, SIG_PUBLISH_ROW, f"{MQTT_CPP}: publish_event_row()", errors)
    sites = [m.start() for m in re.finditer(r'\bbuild_topic\s*\([^;]*"events"\s*\)', mqtt)
             if code.startswith("build_topic", m.start())]
    if span is not None and (len(sites) != 1 or not span[0] <= sites[0] < span[1]):
        errors.append(f"{MQTT_CPP}: the events topic (`build_topic(..., \"events\")`) must be built "
                      "once, in publish_event_row() — a row published anywhere else bypasses the "
                      "egress's order (F78)")


def check_link_glue(egress: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(egress)
    link = body_of(code, SIG_CURRENT_LINK, f"{EGRESS_CPP}: current_link()", errors)
    if link is not None:
        s = squash(link)
        for field, want in (("id_floor", "csi_integration::event_id_floor_stored()"),
                            ("id_next", "csi_event_get_next_event_id()")):
            if s.count(f"link.{field}=") != 1 or f"link.{field}={want};" not in s:
                errors.append(f"{EGRESS_CPP}: current_link() must set link.{field} once, from "
                              f"{want}")
    pump = body_of(code, SIG_PUMP, f"{EGRESS_CPP}: pump()", errors)
    if pump is not None:
        s = squash(pump)
        if "constLinklink=current_link();" not in s or re.search(r"\blink\.\w+\s*=[^=]", pump):
            errors.append(f"{EGRESS_CPP}: pump() takes its link, const and untouched, from "
                          "current_link()")
        args = call_args(pump, "planner.card_open(")
        if args is None or args[-1] != "link":
            errors.append(f"{EGRESS_CPP}: pump() must open the card with the link current_link() "
                          "built (its id_next bounds what the planner trusts)")
    begin = body_of(code, SIG_BEGIN, f"{EGRESS_CPP}: begin()", errors)
    if begin is not None:
        args = call_args(begin, "planner.begin(")
        if (args is None or len(args) != 3 or args[0] != "ceiling"
                or args[1] != "csi_integration::event_id_floor_stored()"
                or "csi_mqtt::NVS_KEY_DELIVERED" not in begin):
            errors.append(f"{EGRESS_CPP}: begin() must hand planner.begin( the ceiling read from "
                          "csi_mqtt::NVS_KEY_DELIVERED and csi_integration::event_id_floor_stored()")


def check_port_sends(egress: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(egress)
    live = body_of(code, SIG_SEND_LIVE, f"{EGRESS_CPP}: WapPort::send_live()", errors)
    if live is not None:
        pub = live.find("publish_event_row(")
        if pub < 0 or "m_st->held_count>0" not in ifs_returning(live[:pub], r"return\s+Sent::kNotNow\s*;"):
            errors.append(f"{EGRESS_CPP}: WapPort::send_live() must refuse (`if (m_st->held_count > 0) "
                          "return Sent::kNotNow;`) before it publishes — rows in RAM are older")
    back = body_of(code, SIG_SEND_BACKFILL, f"{EGRESS_CPP}: WapPort::send_backfill()", errors)
    if back is not None:
        pub = back.find("publish_event_row(")
        head = back[:pub] if pub >= 0 else ""
        if "rec.values.dismissed!=0" not in ifs_returning(head, r"return\s+Sent::kNever\s*;"):
            errors.append(f"{EGRESS_CPP}: WapPort::send_backfill() must refuse a dismissal line "
                          "(`if (rec.values.dismissed != 0) return Sent::kNever;`) before it publishes")
        if "!send_held_below(rec.event_id)" not in ifs_returning(head, r"return\s+Sent::kNotNow\s*;"):
            errors.append(f"{EGRESS_CPP}: WapPort::send_backfill() must send the RAM rows below its "
                          "row first (`if (!send_held_below(rec.event_id)) return Sent::kNotNow;`)")


def check_pump_order(egress: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(egress)
    pump = body_of(code, SIG_PUMP, f"{EGRESS_CPP}: pump()", errors)
    if pump is None:
        return
    loops = [m for m in re.finditer(r"\bfor\s*\(", pump)]
    header = None
    for m in loops:
        close = matching_paren(pump, m.end() - 1)
        if close > 0 and squash(pump[m.start():close + 1]) == PUMP_LOOP:
            header = (m.start(), close)
    if header is None:
        errors.append(f"{EGRESS_CPP}: pump()'s dequeue loop header must be exactly `{PUMP_LOOP}`")
        return
    open_b = pump.find("{", header[1])
    depth, end_b = 0, -1
    for j in range(open_b, len(pump)):
        if pump[j] == "{":
            depth += 1
        elif pump[j] == "}":
            depth -= 1
            if depth == 0:
                end_b = j
                break
    loop_body = pump[open_b + 1:end_b]
    recv = re.search(r"if\s*\(\s*xQueueReceive\([^;]*\)\s*!=\s*pdTRUE\s*\)\s*break\s*;", loop_body)
    bridge = loop_body.find("publish_tamper_bridge(")
    route = loop_body.find("route(")
    if not recv or bridge < 0 or route < 0:
        errors.append(f"{EGRESS_CPP}: pump()'s dequeue loop must receive a row "
                      "(`if (xQueueReceive(...) != pdTRUE) break;`), publish its tamper bridge "
                      "and route it")
        return
    gap = loop_body[recv.end():bridge]
    if re.search(CONTROL_FLOW, gap) or any(t in gap for t in BRIDGE_GAP_FORBIDDEN):
        errors.append(f"{EGRESS_CPP}: pump(): nothing may stand between a dequeued row and its "
                      "tamper bridge — a tamper alert never waits on the card or a backlog")
    if route < bridge:
        errors.append(f"{EGRESS_CPP}: pump() must publish the tamper bridge before it routes the row")
    after = pump[end_b:]
    p, f = after.find("planner.pass("), after.find("flush_held(")
    if p < 0 or f < 0 or f < p or "planner.pass(" in pump[:header[0]]:
        errors.append(f"{EGRESS_CPP}: pump() must run planner.pass( after the dequeue loop, and "
                      "flush_held( after it — RAM rows go once nothing older waits on the card")


def check_route_hold(egress: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(egress)
    route = body_of(code, SIG_ROUTE, f"{EGRESS_CPP}: route()", errors)
    if route is None:
        return
    commit = route.find("st.planner.commit(")
    if commit < 0:
        errors.append(f"{EGRESS_CPP}: route() must hand the row to st.planner.commit(")
        return
    head = route[:commit]
    held = False
    for m in re.finditer(r"\bif\s*\(", head):
        close = matching_paren(head, m.end() - 1)
        if close < 0:
            continue
        block = re.match(r"\s*\{([^{}]*)\}", head[close + 1:])
        if not block or "st.push(" not in block.group(1) or not re.search(r"\breturn\s*;", block.group(1)):
            continue
        for term in top_level_terms(unwrap(head[m.end():close]), "&&"):
            ors = {unwrap(t) for t in top_level_terms(unwrap(term), "||")}
            if ors == HOLD_TERMS:
                held = True
    if not held:
        errors.append(f"{EGRESS_CPP}: route() must hold a row the card does not keep in RAM "
                      "(`st.push(...); return;`) before st.planner.commit( when "
                      "`st.planner.pending() || st.card_wait || st.held_count > 0 || "
                      "!link.connected`")


def check_single_callers(integ: str, mqtt: str, others: dict[str, str], errors: list[str]) -> None:
    files = dict(others)
    files[INTEG_CPP] = integ
    files[MQTT_CPP] = mqtt
    code = {name: blank_cached(src) for name, src in files.items()}
    for name, c in code.items():
        if re.search(r"\busing\s+namespace\s+csi_event_egress\b", c):
            errors.append(f"{name}: `using namespace csi_event_egress` hides the egress's callers "
                          "from this check — call it qualified")
    for call, home, sig, where in (("csi_event_egress::pump(", MQTT_CPP, SIG_LOOP, "csi_mqtt::loop()"),
                                   ("csi_event_egress::begin(", INTEG_CPP, SIG_INIT,
                                    "csi_integration::init")):
        sites = [name for name, c in code.items() for _ in range(c.count(call))]
        span = the_body(code[home], sig, f"{home}: {where}", [])
        inside = span is not None and call in code[home][span[0]:span[1]]
        if len(sites) != 1 or not inside:
            errors.append(f"{SKETCH}: `{call}` must appear exactly once in the sketch, in {where} "
                          f"(found {len(sites)}: {', '.join(sorted(set(sites))) or 'none'}) — a "
                          "second caller runs the egress on a second task")


def check_hand_over(egress: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(egress)
    body = body_of(code, SIG_HAND_TO_QUEUE, f"{EGRESS_CPP}: WapPort::hand_to_queue()", errors)
    if body is None:
        return
    flushing = re.search(r"\bif\s*\(\s*flushing\s*\)\s*\{", body)
    pubs = [m.start() for m in re.finditer(r"publish_event_row\(", body)]
    after = [p for p in pubs if flushing and p > flushing.end()]
    guarded = False
    if flushing:
        close = body.find("}", flushing.end())
        after = [p for p in pubs if p > close]
    for pub in after[:1]:
        for m in re.finditer(r"\bif\s*\(", body[:pub]):
            cl = matching_paren(body, m.end() - 1)
            if cl < 0:
                continue
            block = body[cl + 1:]
            if not block.lstrip().startswith("{"):
                continue
            open_at = cl + 1 + (len(block) - len(block.lstrip()))
            depth, end = 0, -1
            for j in range(open_at, len(body)):
                if body[j] == "{":
                    depth += 1
                elif body[j] == "}":
                    depth -= 1
                    if depth == 0:
                        end = j
                        break
            if not (open_at < pub < end):
                continue
            terms = {unwrap(t) for t in top_level_terms(unwrap(body[m.end():cl]), "&&")}
            if HAND_OVER_TERMS <= terms:
                guarded = True
    if not guarded:
        errors.append(f"{EGRESS_CPP}: WapPort::hand_to_queue() must publish a row that is not on the "
                      "card only inside `if (!deferred && !m_st->planner.pending() && "
                      "m_st->held_count == 0) {...}` — a failed append never overtakes older rows")


def check_destination_epoch(mqtt: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(mqtt)
    init = body_of(code, SIG_MQTT_INIT, f"{MQTT_CPP}: csi_mqtt::init()", errors)
    if init is not None:
        s = squash(init)
        before = s.find("constuint32_tprev_dest=destination_digest(s_active_cfg);")
        load = s.find("config_load(&s_active_cfg)")
        bump = s.find("if(s_dest_known&&destination_digest(s_active_cfg)!=prev_dest){"
                      "s_dest_epoch.fetch_add(")
        known = s.find("s_dest_known=true;")
        if not (0 <= before < load < bump < known):
            errors.append(f"{MQTT_CPP}: csi_mqtt::init() must take destination_digest(s_active_cfg) "
                          "before config_load(&s_active_cfg), bump s_dest_epoch when it changed "
                          "(s_dest_known set), then set s_dest_known = true — the egress drops what "
                          "waited for the old broker on that epoch")
    epoch = body_of(code, SIG_DEST_EPOCH, f"{MQTT_CPP}: csi_mqtt::destination_epoch()", errors)
    if epoch is not None and "returns_dest_epoch.load(" not in squash(epoch):
        errors.append(f"{MQTT_CPP}: csi_mqtt::destination_epoch() must return s_dest_epoch")


def check(integ: str, egress: str, mqtt: str, others: dict[str, str] | None = None) -> list[str]:
    errors: list[str] = []
    check_hook(integ, errors)
    check_enqueue_only(egress, errors)
    check_boot_order(integ, errors)
    check_loop_task(mqtt, errors)
    check_link_glue(egress, errors)
    check_port_sends(egress, errors)
    check_pump_order(egress, errors)
    check_route_hold(egress, errors)
    check_single_callers(integ, mqtt, others or {}, errors)
    check_boot_init_callers(integ, others or {}, errors)
    check_hand_over(egress, errors)
    check_destination_epoch(mqtt, errors)
    return errors


# ── Self-test: mutations the check must refuse ───────────────────────────

Srcs = "tuple[str, str, str]"
Mutation = Callable[[str, str, str], "tuple[str, str, str]"]


def on_i(sig: str, pat: str, repl: str, need: str | None = None) -> Mutation:
    return lambda i, e, m: (mutate_in(i, sig, pat, repl, need=need), e, m)


def on_e(sig: str, pat: str, repl: str, need: str | None = None) -> Mutation:
    return lambda i, e, m: (i, mutate_in(e, sig, pat, repl, need=need), m)


def on_m(sig: str, pat: str, repl: str, need: str | None = None) -> Mutation:
    return lambda i, e, m: (i, e, mutate_in(m, sig, pat, repl, need=need))


def moved_begin(i: str, e: str, m: str) -> "tuple[str, str, str]":
    i = mutate_in(i, SIG_INIT, r"\n[ \t]*csi_event_egress::begin\(\);", "")
    i = mutate_in(i, SIG_INIT, r"(const\s+bool\s+floor_restored)", r"csi_event_egress::begin(); \1")
    return i, e, m


def boot_init_before(anchor: str) -> Mutation:
    """csi_module_init_all(nullptr); moved to just before `anchor` in init()."""
    def mutate(i: str, e: str, m: str) -> "tuple[str, str, str]":
        i = mutate_in(i, SIG_INIT, r"\n[ \t]*csi_module_init_all\(nullptr\);", "")
        i = mutate_in(i, SIG_INIT, "(" + anchor + ")", r"csi_module_init_all(nullptr); \1")
        return i, e, m
    return mutate


def modules_before(anchor: str) -> Mutation:
    """register_v1_modules(); moved to just before `anchor` in init()."""
    def mutate(i: str, e: str, m: str) -> "tuple[str, str, str]":
        i = mutate_in(i, SIG_INIT, r"\n[ \t]*register_v1_modules\(\);", "")
        i = mutate_in(i, SIG_INIT, "(" + anchor + ")", r"register_v1_modules(); \1")
        return i, e, m
    return mutate


MUTATIONS: list[tuple[str, Mutation]] = [
    ("the hook forwards nothing",
     on_i(SIG_HOOK, r"\n[ \t]*csi_event_egress::on_committed\([^;]*;", "")),
    ("the hook also pumps MQTT on the committing task",
     on_i(SIG_HOOK, r"(csi_event_egress::on_committed\([^;]*;)", r"\1 csi_mqtt::loop();")),
    ("the hook writes the card",
     on_i(SIG_HOOK, r"(csi_event_egress::on_committed\([^;]*;)",
          r"\1 (void)csi_event_log::flush_dismissals();")),
    ("on_committed publishes the row",
     on_e(SIG_ON_COMMITTED, r"(\n[ \t]*/\* Never block)",
          r" (void)csi_mqtt::publish_event_row(c.rec, 1, false);\1")),
    ("on_committed blocks on a full queue",
     on_e(SIG_ON_COMMITTED, r"xQueueSend\(q,\s*&c,\s*0\)", "xQueueSend(q, &c, portMAX_DELAY)")),
    ("on_committed routes the row on the committing task",
     on_e(SIG_ON_COMMITTED, r"(\n[ \t]*/\* Never block)", r" route(c, current_link());\1")),
    ("on_committed reads the planner",
     on_e(SIG_ON_COMMITTED, r"(\n[ \t]*/\* Never block)", r" (void)g_state->planner.pending();\1")),
    ("begin() runs before the floor is restored", moved_begin),
    ("begin() is never called",
     on_i(SIG_INIT, r"\n[ \t]*csi_event_egress::begin\(\);", "")),
    # Rule 3, F83: the floor and the egress come before the modules register.
    ("the modules register before the floor is restored",
     modules_before(r"const\s+bool\s+floor_restored")),
    ("the modules register between the floor and the egress",
     modules_before(r"csi_event_egress::begin\(\);")),
    ("the modules also register before the floor",
     on_i(SIG_INIT, r"(g_api_token\s*=\s*api_token\s*;)", r"\1 register_v1_modules();")),
    # Rule 3, F93: the modules' boot init, once, after they register.
    ("the modules are never initialized at boot",
     on_i(SIG_INIT, r"\n[ \t]*csi_module_init_all\(nullptr\);", "")),
    ("the boot init runs before the modules register",
     boot_init_before(r"register_v1_modules\(\);")),
    ("the boot init runs before the floor is restored",
     boot_init_before(r"const\s+bool\s+floor_restored")),
    ("the boot init runs after the first tick is installed",
     lambda i, e, m: (mutate_in(mutate_in(i, SIG_INIT, r"\n[ \t]*csi_module_init_all\(nullptr\);", ""),
                                SIG_INIT, r"(csi_set_features_callback\([^;]*;)",
                                r"\1 csi_module_init_all(nullptr);"), e, m)),
    ("the boot init runs only when the floor was read",
     on_i(SIG_INIT, r"(csi_module_init_all\(nullptr\);)", r"if (floor_restored) \1")),
    ("the boot init sits in a block",
     on_i(SIG_INIT, r"(csi_module_init_all\(nullptr\);)", r"{ if (g_api_token) { \1 } }")),
    ("the boot init is compiled out",
     on_i(SIG_INIT, r"(csi_module_init_all\(nullptr\);)", r"\n#if CSI_BOOT_INIT\n  \1\n#endif\n")),
    ("reinit_module() runs the boot init too",
     on_i(SIG_REINIT, r"(if\s*\(\s*m->init\s*\))", r"csi_module_init_all(nullptr); \1")),
    ("register_v1_modules() initializes the modules instead",
     lambda i, e, m: (mutate_in(mutate_in(i, SIG_INIT, r"\n[ \t]*csi_module_init_all\(nullptr\);", ""),
                                SIG_REGISTER, r"(apply_quiet_hours_from_nvs\(\);)",
                                r"csi_module_init_all(nullptr); \1"), e, m)),
    ("the esp_mqtt handler pumps the egress",
     on_m(SIG_HANDLER, r"(s_connected\.store\(true,[^;]*;)", r"\1 csi_event_egress::pump();")),
    ("the esp_mqtt handler publishes a row",
     on_m(SIG_HANDLER, r"(s_connected\.store\(true,[^;]*;)",
          r"\1 { csi_event_record_t r = {}; (void)publish_event_row(r, 1, false); }")),
    ("the esp_mqtt handler publishes on the events topic itself",
     on_m(SIG_HANDLER, r"(s_connected\.store\(true,[^;]*;)",
          r'\1 { char t[192]; build_topic(t, sizeof(t), "events"); publish_raw(t, "{}", 2, false); }')),
    ("the loop does not pump",
     on_m(SIG_LOOP, r"\n[ \t]*csi_event_egress::pump\(\);", "")),
    ("current_link() drops the id floor",
     on_e(SIG_CURRENT_LINK, r"(link\.id_floor\s*=)[^;]*;", r"\1 0;")),
    ("current_link() drops the allocator's next id",
     on_e(SIG_CURRENT_LINK, r"(link\.id_next\s*=)[^;]*;", r"\1 0;")),
    ("the pump zeroes its link's floor",
     on_e(SIG_PUMP, r"const\s+(Link\s+link\s*=\s*current_link\(\)\s*;)", r"\1 link.id_floor = 0;")),
    ("the card opens with an empty link",
     on_e(SIG_PUMP, r"(planner\.card_open\(\s*log_size\s*,\s*tail_id\s*,)\s*link\s*\)", r"\1 Link{})")),
    ("begin() hands the planner no floor",
     on_e(SIG_BEGIN, r"(planner\.begin\(\s*ceiling\s*,)[^,]*,", r"\1 0,")),
    ("send_live ignores the RAM hold",
     on_e(SIG_SEND_LIVE, r"\n[ \t]*if\s*\(\s*m_st->held_count\s*>\s*0\s*\)\s*return\s+Sent::kNotNow\s*;", "")),
    ("send_backfill skips the RAM rows below it",
     on_e(SIG_SEND_BACKFILL, r"\n[ \t]*if\s*\(\s*!send_held_below\([^)]*\)\s*\)\s*return\s+Sent::kNotNow\s*;", "")),
    ("send_backfill replays a dismissal line",
     on_e(SIG_SEND_BACKFILL, r"\n[ \t]*if\s*\(\s*rec\.values\.dismissed\s*!=\s*0\s*\)\s*return\s+Sent::kNever\s*;", "")),
    ("the row is routed before its tamper bridge",
     on_e(SIG_PUMP, r"(\(void\)csi_mqtt::publish_tamper_bridge\([^;]*;)(.*?)(route\(ev,\s*link\);)",
          r"\3\2\1", need="xQueueReceive(")),
    ("the tamper bridge waits for the backlog",
     on_e(SIG_PUMP, r"(\(void\)csi_mqtt::publish_tamper_bridge\()", r"if (!st.planner.pending()) \1",
          need="xQueueReceive(")),
    ("dequeued rows are skipped while a backfill runs",
     on_e(SIG_PUMP, r"(if\s*\(\s*xQueueReceive\([^;]*break\s*;)", r"\1 if (st.replay_run) continue;",
          need="xQueueReceive(")),
    ("the dequeue budget depends on the backlog",
     on_e(SIG_PUMP, r"budget\s*>\s*0\s*;", "budget > 0 && !st.planner.pending();", need="xQueueReceive(")),
    ("the RAM rows are flushed before the backfill pass",
     on_e(SIG_PUMP, r"(const\s+size_t\s+replayed\s*=\s*st\.planner\.pass\([^;]*;)(\s*)(flush_held\(link\);)",
          r"\3\2\1", need="xQueueReceive(")),
    ("route() never holds a row",
     on_e(SIG_ROUTE, r"if\s*\(\s*!card_row\s*&&\s*link\.accepting\s*&&\s*\([^{]*\)\s*\)\s*\{",
          "if (false) {")),
    ("route() holds only while the link is down",
     on_e(SIG_ROUTE, r"st\.planner\.pending\(\)\s*\|\|\s*st\.card_wait\s*\|\|\s*"
                     r"st\.held_count\s*>\s*0\s*\|\|\s*", "")),
    ("route() lets a row overtake the RAM hold",
     on_e(SIG_ROUTE, r"\|\|\s*st\.held_count\s*>\s*0\s*", "")),
    ("route() lets a row overtake a card that is not open",
     on_e(SIG_ROUTE, r"\|\|\s*st\.card_wait\s*", "")),
    ("route() hands a held row to the planner too",
     on_e(SIG_ROUTE, r"(st\.push\([^;]*;)\s*return\s*;", r"\1")),
    # Rule 1: the hook names only on_committed, after the privacy gate.
    ("the hook also pumps the egress",
     on_i(SIG_HOOK, r"(csi_event_egress::on_committed\([^;]*;)", r"\1 csi_event_egress::pump();")),
    ("the hook reads the egress's watermark",
     on_i(SIG_HOOK, r"(csi_event_egress::on_committed\([^;]*;)",
          r"\1 (void)csi_event_egress::watermark();")),
    ("the hook forwards before the privacy gate",
     on_i(SIG_HOOK, r"(if\s*\(\s*privacy\s*>\s*csi_event_get_privacy_ceiling\(\)\s*\)\s*return\s*;)",
          r"csi_event_egress::on_committed(event_id, module_id, type_name, category, privacy, values); \1")),
    # Rule 9: one pump, one begin, sketch-wide.
    ("the dismiss handler pumps the egress (httpd task)",
     on_i(SIG_DISMISS, r"(httpd_resp_set_type\()", r"csi_event_egress::pump(); \1")),
    ("csi_integration::loop pumps the egress too",
     on_i(SIG_INTEG_LOOP, r"(csi_bundler_tick\(\);)", r"\1 csi_event_egress::pump();")),
    ("the pump moves out of csi_mqtt::loop into csi_integration::loop",
     lambda i, e, m: (mutate_in(i, SIG_INTEG_LOOP, r"(csi_bundler_tick\(\);)", r"\1 csi_event_egress::pump();"),
                      e, mutate_in(m, SIG_LOOP, r"\n[ \t]*csi_event_egress::pump\(\);", ""))),
    ("csi_mqtt::init calls begin() a second time",
     on_m(SIG_MQTT_INIT, r"(teardown_client\(\);)", r"\1 csi_event_egress::begin();")),
    ("csi_mqtt.cpp hides the pump's caller behind a using-directive",
     lambda i, e, m: (i, e, m.replace('#include "csi_event_egress.h"',
                                      '#include "csi_event_egress.h"\nusing namespace csi_event_egress;', 1))),
    # Rule 10: hand_to_queue's live publish waits for older rows.
    ("hand_to_queue publishes past rows waiting in RAM",
     on_e(SIG_HAND_TO_QUEUE, r"\s*&&\s*m_st->held_count\s*==\s*0", "")),
    ("hand_to_queue publishes past rows waiting on the card",
     on_e(SIG_HAND_TO_QUEUE, r"\s*&&\s*!m_st->planner\.pending\(\)", "")),
    ("hand_to_queue publishes a deferred row live",
     on_e(SIG_HAND_TO_QUEUE, r"!deferred\s*&&\s*", "")),
    # Rule 11: a changed broker bumps the epoch.
    ("init() never bumps the destination epoch",
     on_m(SIG_MQTT_INIT, r"\n[ \t]*s_dest_epoch\.fetch_add\([^;]*;", "")),
    ("init() reads the old destination after the load",
     on_m(SIG_MQTT_INIT, r"(const\s+uint32_t\s+prev_dest\s*=\s*destination_digest\(s_active_cfg\);)\s*"
                         r"(if\s*\(\s*!config_load\(&s_active_cfg\)\)\s*return\s+false;)", r"\2 \1")),
    ("init() never records that a destination is known",
     on_m(SIG_MQTT_INIT, r"\n[ \t]*s_dest_known\s*=\s*true\s*;", "")),
    ("destination_epoch() returns a constant",
     on_m(SIG_DEST_EPOCH, r"return\s+s_dest_epoch\.load\([^;]*;", "return 0;")),
]


def self_test(integ: str, egress: str, mqtt: str, others: dict[str, str]) -> list[str]:
    problems = []
    for name, mutate in MUTATIONS:
        try:
            i, e, m = mutate(integ, egress, mqtt)
        except AnchorMissing as missing:
            problems.append(f"self-test: mutation '{name}' no longer applies (anchor {missing}) — "
                            "the source changed shape; update this guard's mutations with it")
            continue
        if (i, e, m) == (integ, egress, mqtt):
            problems.append(f"self-test: mutation '{name}' changed nothing")
        elif not check(i, e, m, others):
            problems.append(f"self-test: the check did not bite on mutation '{name}'")
    return problems


def sketch_others() -> dict[str, str]:
    """Every other source file of the sketch, by repo-relative path."""
    skip = {INTEG_CPP, MQTT_CPP}
    out = {}
    for pattern in SKETCH_GLOBS:
        for path in sorted((REPO / SKETCH).glob(pattern)):
            rel = path.relative_to(REPO).as_posix()
            if rel not in skip:
                out[rel] = path.read_text(encoding="utf-8", errors="replace")
    return out


def main() -> int:
    integ = (REPO / INTEG_CPP).read_text(encoding="utf-8")
    egress = (REPO / EGRESS_CPP).read_text(encoding="utf-8")
    mqtt = (REPO / MQTT_CPP).read_text(encoding="utf-8")
    others = sketch_others()
    errors = check(integ, egress, mqtt, others)
    for err in errors:
        print(f"::error::{err}")
    problems = self_test(integ, egress, mqtt, others)
    for problem in problems:
        print(f"::error::{problem}")
    if errors or problems:
        return 1
    print(f"canary-wap event egress holds: the commit hook only enqueues, after the privacy gate; "
          f"the egress runs once, on the loop task, after the id floor and before the modules "
          f"register; the modules' boot init runs once, after they register and before the "
          f"first tick; live rows wait behind the "
          f"card and RAM backlog; the tamper bridge goes first; a changed broker bumps the epoch "
          f"({len(MUTATIONS)} mutations refused).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
