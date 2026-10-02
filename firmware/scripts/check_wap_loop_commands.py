#!/usr/bin/env python3
"""Hold the canary-wap's loop-task ownership: mesh commands and MQTT re-inits.

Sweep F96: `canary_wap.ino`'s `handle_mesh_*` REST handlers called
`remove_peer`, `leave_opera`, `start_pairing_*`, `cancel_pairing`,
`confirm_pairing`, `set_enabled`, `set_opera_name` and `clear_alerts` on
esp_http_server's task, while `mesh_network::update()` read and wrote the
same peer table, pairing session, opera config (and its one `g_prefs` NVS
handle) and alert history on the loop task. Now those functions are
internal to `mesh_network.cpp`, a handler hands a `Command` to
`mesh_network::submit()`, and `update()` drains the command ring on the
loop task (`loop_command_ring.h`).

Sweep F106: `csi_mqtt::init()` tore the esp_mqtt client down and built a
new one. A config POST and `POST /api/mqtt/test` ran it on the httpd task,
and a QR hub provision on the scanner's, under a loop-task publish holding
the old handle. Now the client is the loop task's: other tasks call
`request_reinit()`, and `csi_mqtt::loop()` serves the re-init. And the loop
task never stops a client itself (the F106 review): `esp_mqtt_client_stop()`
can wait out a whole connect attempt (the esp_mqtt task holds the client's
lock across it, 10 s by default), past the loop task's 8 s panic watchdog.
The loop task detaches the client and a one-shot worker (`retire_task`)
stops and destroys it; a later pass opens the new one.

`test_mesh_commands_wap.cpp`, `test_mqtt_reinit.cpp` and
`test_loop_command_ring.cpp` run the real code on the host. No host test can
compile `canary_wap.ino`, and none can see which task a call will run on;
this check holds the sources to the shape those tests assume.

## The rules

Mesh (F96):

1. `mesh_network.h` declares none of the owner commands' functions
   (`MUTATORS` below) in `namespace mesh_network`; `mesh_network.cpp`
   defines each once, `static`. Nothing outside the file can call them.
2. In `mesh_network.cpp` they are called only from `run_command()` (the
   drain's runner), except `cancel_pairing()`, which `update()` (the pairing
   timeout) and the pairing frame handlers (dispatched from `update()`) call
   too. `run_command(` is never called directly: only `drain(` runs it.
3. `update()` drains the ring (`g_commands.drain(run_command);`) once, before
   its first `return` (a disabled mesh, which is what a fresh device is,
   still takes `enable` and `pair/start`); nothing else in the file drains.
4. `submit()` only posts and waits (`loop_command_ring::submit(`): it names
   no owner command, no `run_command(` and no `drain(`. The ESP-NOW
   callbacks (`espnow_recv_cb`, `espnow_send_cb`, on the Wi-Fi task) name
   none of them either.
5. Across the sketch (comments and strings blanked), no file but
   `mesh_network.cpp` names `mesh_network::<owner command>(`, and no file
   says `using namespace mesh_network`. Each of the nine changing
   `handle_mesh_*` handlers in `canary_wap.ino` calls
   `mesh_network::submit(` exactly once and answers a command that did not
   run with `mesh_network::not_run_status(`. `mesh_network::submit(` appears
   only in HTTP handlers (a function taking `httpd_req_t*`): from the loop
   task it would wait for itself. `mesh_network::update(` is called once,
   from the sketch's `loop()`.

MQTT (F106):

6. In `csi_mqtt.cpp`, who touches the client:
   - `esp_mqtt_client_stop(` is called only in `retire_task()` (the worker),
     and `esp_mqtt_client_destroy(` only there and once in `open_client()`,
     on its own never-started `client` after a failed
     `esp_mqtt_client_start(`. `esp_mqtt_client_init(` and
     `esp_mqtt_client_start(` are called only in `open_client()`, which
     publishes the client (`s_client.store(client`) before it starts it.
     `s_client` is written (`=`, `.store(`, `.exchange(`) only in
     `open_client()` and `detach_client()`.
   - `retire_task` is never called: `retire_finished()` hands it to
     `xTaskCreate(`, and nothing else creates it. `detach_client(` and
     `retire_finished(` are called only in `serve_reinit()`,
     `open_client(` only in `serve_reinit()` and `init()`, bare `init(`
     nowhere, and `serve_reinit(` once, in `loop()`, before
     `csi_event_egress::pump(`.
   - `serve_reinit()` returns while `retire_finished()` is false, both before
     it looks at the requests and after `detach_client()`; reads
     `s_reinit_wanted` into `wanted` after that, then runs `open_client()`,
     then `s_reinit_served.store(wanted`.
   - `request_reinit()` only bumps `s_reinit_wanted`.
   - The esp_mqtt event handler returns first for a client that is not
     `s_client` (`e->client != s_client.load(`), subscribes on `e->client`
     only, and names no lifecycle function.
7. `handle_config_post` and `handle_test` each call `request_reinit(`; no
   HTTP handler anywhere in the sketch names `init(` / `csi_mqtt::init(`,
   a lifecycle function of rule 6, an `esp_mqtt_client_*(` call, a write of
   `s_client`, `csi_mqtt::loop(`, `mesh_network::update(` or a `publish_`
   function (the owner commands of rule 1 are rule 5's).
   `set_update_auto_state()` only caches (no `publish_raw(`, no
   `build_topic(`), and `loop()` publishes the `update/auto` topic.
8. Across the sketch: `csi_mqtt::init(` is called once, from
   `register_api_routes()` (the boot init), which only `start_http_server()`
   calls (one of its two servers registers the routes per boot), which is
   called once, from `setup()`: the loop task, before `loop()` starts.
   `csi_mqtt::set_identity(` is called once, from `setup()`, and
   `csi_mqtt::loop(` once, from the sketch's `loop()`. The QR scanner
   (`qr_scan_task_fn`) calls `csi_mqtt::request_reinit(`. No file says
   `using namespace csi_mqtt`.

## It proves it bites

Each run applies mutations to the sources in memory and requires the check
to fail on every one. A mutation whose anchor moved fails the run.

Run locally:  python3 firmware/scripts/check_wap_loop_commands.py   (repo root)
CI:           firmware.yml "Regression guard", via regression_check.sh
"""

from __future__ import annotations

import functools
import re
import sys
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_event_egress_order import (  # noqa: E402  (shared C++ scanning helpers)
    AnchorMissing,
    blank_comments_and_strings as _blank,
    matching_paren,
    mutate_in,
    squash,
    the_body,
)

# The sketch is about 5 MB of source and every mutation re-checks all of it:
# blank each distinct text once.
blank_comments_and_strings = functools.lru_cache(maxsize=512)(_blank)

REPO = Path(__file__).resolve().parents[2]
SKETCH = "firmware/projects/canary-wap/arduino/canary_wap"
INO = f"{SKETCH}/canary_wap.ino"
MESH_H = f"{SKETCH}/mesh_network.h"
MESH_CPP = f"{SKETCH}/mesh_network.cpp"
MQTT_CPP = f"{SKETCH}/csi_mqtt.cpp"
SKETCH_GLOBS = ("*.cpp", "*.h", "*.ino")

MUTATORS = ("set_enabled", "remove_peer", "set_opera_name", "leave_opera",
            "start_pairing_initiator", "start_pairing_joiner", "cancel_pairing",
            "confirm_pairing", "clear_alerts")
# Where cancel_pairing() may be called besides run_command(): update() (the
# pairing timeout) and the pairing frame handlers, which update() dispatches.
CANCEL_CALLERS = ("run_command", "update", "handle_pair_discover", "handle_pair_offer",
                  "handle_pair_accept", "handle_pair_confirm", "handle_pair_complete")
CHANGING_HANDLERS = ("handle_mesh_alerts_clear", "handle_mesh_enable", "handle_mesh_pair_start",
                     "handle_mesh_pair_join", "handle_mesh_pair_confirm", "handle_mesh_pair_cancel",
                     "handle_mesh_leave", "handle_mesh_remove", "handle_mesh_name")

SIG_HANDLER = r"\besp_err_t\s+(\w+)\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)"
SIG_UPDATE = r"\bvoid\s+update\s*\(\s*\)"
SIG_SUBMIT = r"\bloop_command_ring::Wait\s+submit\s*\([^)]*\)"
SIG_RECV_CB = r"\bstatic\s+void\s+espnow_recv_cb\s*\([^)]*\)"
SIG_SEND_CB = r"\bstatic\s+void\s+espnow_send_cb\s*\([^)]*\)"
SIG_MQTT_LOOP = r"\bvoid\s+loop\s*\(\s*\)"
SIG_REQUEST = r"\buint32_t\s+request_reinit\s*\(\s*\)"
SIG_SET_AUTO = r"\bvoid\s+set_update_auto_state\s*\([^)]*\)"
SIG_EVENT_HANDLER = r"\bvoid\s+mqtt_event_handler\s*\([^)]*\)"
SIG_CONFIG_POST = r"\besp_err_t\s+handle_config_post\s*\([^)]*\)"
SIG_TEST = r"\besp_err_t\s+handle_test\s*\([^)]*\)"
SIG_SERVE = r"\bvoid\s+serve_reinit\s*\(\s*\)"
SIG_OPEN = r"\bbool\s+open_client\s*\(\s*\)"
SIG_RETIRE_FINISHED = r"\bbool\s+retire_finished\s*\(\s*\)"
SIG_INO_LOOP = r"\bvoid\s+loop\s*\(\s*\)"
SIG_SETUP = r"\bvoid\s+setup\s*\(\s*\)"
SIG_START_HTTP = r"\bstatic\s+void\s+start_http_server\s*\(\s*\)"
SIG_REGISTER = r"\bstatic\s+void\s+register_api_routes\s*\(\s*httpd_handle_t\s+\w+\s*\)"
SIG_QR_TASK = r"\bstatic\s+void\s+qr_scan_task_fn\s*\([^)]*\)"

# A call of a bare `init(`: not a member, not qualified, not part of a name.
BARE_INIT = r"(?<![\w:.>])init\s*\("


def body_of(code: str, sig: str, what: str, errors: list[str]) -> str | None:
    span = the_body(code, sig, what, errors)
    return None if span is None else code[span[0]:span[1]]


KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "sizeof", "defined", "alignof",
            "decltype", "static_assert", "else", "do"}


def close_brace(code: str, open_at: int) -> int:
    depth = 0
    for j in range(open_at, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return j
    return -1


def named_bodies(code: str) -> list[tuple[str, int, int]]:
    """(name, start, end) of every function definition's body in `code`: a
    name, its parameter list, then `{`, after a return type (an identifier,
    `*`, `&`, `>` or a `::` qualifier ends what precedes the name)."""
    out = []
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", code):
        name = m.group(1)
        if name in KEYWORDS:
            continue
        close = matching_paren(code, m.end() - 1)
        if close < 0:
            continue
        tail = re.match(r"\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?\{", code[close + 1:close + 64])
        if not tail:
            continue
        before = code[max(0, m.start() - 64):m.start()].rstrip()
        if not before or not re.search(r"[\w*&>:]$", before) or re.search(r"\b(?:return|else)$", before):
            continue
        open_at = close + 1 + tail.end() - 1
        end = close_brace(code, open_at)
        if end > 0:
            out.append((name, open_at + 1, end))
    return out


def enclosing_function(spans: list[tuple[str, int, int]], pos: int) -> str | None:
    """The innermost named body holding `pos`."""
    best = None
    for name, s, e in spans:
        if s <= pos < e and (best is None or s > best[1]):
            best = (name, s, e)
    return None if best is None else best[0]


@functools.lru_cache(maxsize=1024)
def handler_spans(code: str) -> tuple[tuple[str, int, int], ...]:
    """(name, start, end) of every HTTP handler body (`esp_err_t f(httpd_req_t* r)`)."""
    out = []
    for m in re.finditer(SIG_HANDLER + r"\s*\{", code):
        end = close_brace(code, m.end() - 1)
        if end > 0:
            out.append((m.group(1), m.end(), end))
    return tuple(out)


def namespace_block(code: str, ns: str) -> str:
    """The text of every `namespace ns { ... }` block in `code`."""
    parts = []
    for m in re.finditer(r"\bnamespace\s+" + ns + r"\s*\{", code):
        end = close_brace(code, m.end() - 1)
        if end > 0:
            parts.append(code[m.end():end])
    return "\n".join(parts)


# ── Mesh (F96) ───────────────────────────────────────────────────────────

def check_mesh_internal(mesh_h: str, mesh_cpp: str, errors: list[str]) -> None:
    hcode = namespace_block(blank_comments_and_strings(mesh_h), "mesh_network")
    for fn in MUTATORS:
        if re.search(r"\b" + fn + r"\s*\(", hcode):
            errors.append(f"{MESH_H}: declares {fn}() in namespace mesh_network — the owner commands "
                          "stay internal to mesh_network.cpp, so no other task can call them (F96)")
    code = blank_comments_and_strings(mesh_cpp)
    for fn in MUTATORS:
        defs = [m for m in re.finditer(r"\b(?:bool|void)\s+" + fn + r"\s*\([^;{}]*\)\s*\{", code)]
        if len(defs) != 1:
            errors.append(f"{MESH_CPP}: expected one definition of {fn}(), found {len(defs)}")
            continue
        line_start = code.rfind("\n", 0, defs[0].start()) + 1
        if not re.match(r"\s*static\b", code[line_start:defs[0].start() + 1]):
            errors.append(f"{MESH_CPP}: {fn}() must be defined static — the owner commands are "
                          "internal to mesh_network.cpp (F96)")


def check_mesh_callers(mesh_cpp: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(mesh_cpp)
    spans = named_bodies(code)
    for fn in MUTATORS:
        allowed = CANCEL_CALLERS if fn == "cancel_pairing" else ("run_command",)
        for m in re.finditer(r"(?<![\w:.>])" + fn + r"\s*\(", code):
            where = enclosing_function(spans, m.start())
            if where is None:
                continue                      # a declaration or the definition's own header
            if where not in allowed:
                errors.append(f"{MESH_CPP}: {where}() calls {fn}() — an owner command runs from "
                              f"run_command() (update()'s drain){' or the loop task paths ' + ', '.join(CANCEL_CALLERS[1:]) if fn == 'cancel_pairing' else ''} "
                              "only (F96)")
    for m in re.finditer(r"(?<![\w:.>])run_command\s*\(", code):
        if enclosing_function(spans, m.start()) is not None:
            errors.append(f"{MESH_CPP}: run_command( is called directly in "
                          f"{enclosing_function(spans, m.start())}() — only update()'s drain runs it (F96)")
    update = body_of(code, SIG_UPDATE, f"{MESH_CPP}: update()", errors)
    if update is not None:
        drain = "g_commands.drain(run_command);"
        s = squash(update)
        ret = re.search(r"\breturn\b", update)
        at = update.find("g_commands.drain(")
        if s.count(drain) != 1 or at < 0 or (ret is not None and ret.start() < at):
            errors.append(f"{MESH_CPP}: update() must run `{drain}` once, before its first return — "
                          "a disabled mesh still takes enable and pair/start (F96)")
    drains = [enclosing_function(spans, m.start()) for m in re.finditer(r"\.drain\s*\(", code)]
    if drains != ["update"]:
        errors.append(f"{MESH_CPP}: the command ring is drained in {drains or 'nothing'} — only "
                      "update(), on the loop task, drains it (F96)")
    submit = body_of(code, SIG_SUBMIT, f"{MESH_CPP}: submit()", errors)
    if submit is not None:
        if "loop_command_ring::submit(" not in submit:
            errors.append(f"{MESH_CPP}: submit() must post and wait through loop_command_ring::submit(")
        for tok in MUTATORS + ("run_command", "drain"):
            if re.search(r"\b" + tok + r"\s*\(", submit):
                errors.append(f"{MESH_CPP}: submit() names {tok}( — it runs on the HTTP server's "
                              "task and only posts the command and waits (F96)")
    for sig, what in ((SIG_RECV_CB, "espnow_recv_cb()"), (SIG_SEND_CB, "espnow_send_cb()")):
        cb = body_of(code, sig, f"{MESH_CPP}: {what}", errors)
        if cb is None:
            continue
        for tok in MUTATORS + ("run_command", "drain", "submit"):
            if re.search(r"\b" + tok + r"\s*\(", cb):
                errors.append(f"{MESH_CPP}: {what} names {tok}( — it runs on the Wi-Fi task (F96)")


@functools.lru_cache(maxsize=1024)
def mesh_file_findings(name: str, c: str) -> tuple[str, ...]:
    """Rule 5's per-file part, for one blanked file (cached: most files are
    the same text in every mutation)."""
    out = []
    if re.search(r"\busing\s+namespace\s+mesh_network\b", c):
        out.append(f"{name}: `using namespace mesh_network` hides the mesh's callers from "
                   "this check — call it qualified")
    if name == MESH_CPP:
        return tuple(out)
    if "mesh_network::" not in c:
        return tuple(out)
    for fn in MUTATORS:
        if re.search(r"\bmesh_network::" + fn + r"\s*\(", c):
            out.append(f"{name}: calls mesh_network::{fn}() — hand a Command to "
                       "mesh_network::submit() instead; update() runs it on the loop task (F96)")
    handlers = handler_spans(c)
    for m in re.finditer(r"\bmesh_network::submit\s*\(", c):
        if enclosing_function(handlers, m.start()) is None:
            out.append(f"{name}: mesh_network::submit( outside an HTTP handler — from the "
                       "loop task it would wait for itself (F96)")
    return tuple(out)


def check_mesh_sketch(ino: str, others: dict[str, str], errors: list[str]) -> None:
    files = dict(others)
    files[INO] = ino
    code = {name: blank_comments_and_strings(src) for name, src in files.items()}
    for name, c in code.items():
        errors.extend(mesh_file_findings(name, c))
    ino_code = code[INO]
    for h in CHANGING_HANDLERS:
        body = body_of(ino_code, r"\bstatic\s+esp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)",
                       f"{INO}: {h}()", errors)
        if body is None:
            continue
        if body.count("mesh_network::submit(") != 1 or "mesh_network::not_run_status(" not in body:
            errors.append(f"{INO}: {h}() must hand its command to mesh_network::submit( once and "
                          "answer one that did not run with mesh_network::not_run_status( (F96)")
    loop = body_of(ino_code, SIG_INO_LOOP, f"{INO}: loop()", errors)
    total = sum(c.count("mesh_network::update(") for c in code.values())
    if loop is None or loop.count("mesh_network::update(") != 1 or total != 1:
        errors.append(f"{SKETCH}: mesh_network::update( must be called once, from the sketch's "
                      f"loop() (found {total}) — its drain is the loop task's (F96)")


# ── MQTT (F106) ──────────────────────────────────────────────────────────

# Who may call what in csi_mqtt.cpp (rule 6): the call, the functions it may
# appear in, and why.
MQTT_CALLERS = (
    ("esp_mqtt_client_stop", ("retire_task",),
     "a stop can wait out a connect attempt, past the loop task's watchdog: only the worker stops"),
    ("esp_mqtt_client_destroy", ("retire_task", "open_client"),
     "only the worker destroys a client that ran; open_client() only its own unstarted one"),
    ("esp_mqtt_client_init", ("open_client",), "only open_client() makes a client"),
    ("esp_mqtt_client_start", ("open_client",), "only open_client() starts a client"),
    ("detach_client", ("serve_reinit",), "only the loop task's re-init detaches the client"),
    ("retire_finished", ("serve_reinit",), "only the loop task's re-init retires the client"),
    ("open_client", ("serve_reinit", "init"), "the client opens on the loop task: the boot, then a re-init"),
    ("serve_reinit", ("loop",), "the re-init is served by loop(), on the loop task"),
)
# A write of the client handle.
S_CLIENT_WRITE = r"\bs_client\s*(?:=(?!=)|\.store\s*\(|\.exchange\s*\(|\.compare_exchange_\w+\s*\()"


def check_mqtt_reinit(mqtt: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(mqtt)
    spans = named_bodies(code)

    def where_of(m: re.Match) -> str | None:
        return enclosing_function(spans, m.start())

    for call, allowed, why in MQTT_CALLERS:
        for m in re.finditer(r"(?<![\w:.>])" + call + r"\s*\(", code):
            where = where_of(m)
            if where is None or where == call:
                continue                      # the definition's own header
            if where not in allowed:
                errors.append(f"{MQTT_CPP}: {where}() calls {call}() — {why} (F106)")
    for m in re.finditer(S_CLIENT_WRITE, code):
        where = where_of(m)
        if where not in ("open_client", "detach_client"):
            errors.append(f"{MQTT_CPP}: {where + '()' if where else 'file scope'} writes s_client — only open_client() "
                          "and detach_client(), on the loop task, do (F106)")
    for m in re.finditer(BARE_INIT, code):
        where = where_of(m)
        if where is not None:
            errors.append(f"{MQTT_CPP}: {where}() calls init() — init() is the boot's (the sketch's "
                          "start_http_server); a re-init is serve_reinit()'s, and other tasks call "
                          "request_reinit() (F106)")
    for m in re.finditer(r"(?<![\w:.>])retire_task\b", code):
        where = where_of(m)
        if where is None or where == "retire_task":
            continue
        tail = code[m.end():m.end() + 8]
        before = code[max(0, m.start() - 24):m.start()]
        if where != "retire_finished" or not re.search(r"\bxTaskCreate\s*\(\s*$", before) or \
                tail.lstrip().startswith("("):
            errors.append(f"{MQTT_CPP}: {where}() names retire_task other than as "
                          "retire_finished()'s xTaskCreate( argument — the stop runs on that worker, "
                          "never on the caller's task (F106)")
    opened = body_of(code, SIG_OPEN, f"{MQTT_CPP}: open_client()", errors)
    if opened is not None:
        s = squash(opened)
        at_store = s.find("s_client.store(client")
        at_start = s.find("esp_mqtt_client_start(client")
        destroys = re.findall(r"esp_mqtt_client_destroy\(([^)]*)\)", s)
        if at_store < 0 or at_start < 0 or at_store > at_start:
            errors.append(f"{MQTT_CPP}: open_client() must store the new client in s_client before "
                          "esp_mqtt_client_start( — its event handler ignores any other client (F106)")
        if destroys and (destroys != ["client"] or s.find("esp_mqtt_client_destroy(") < at_start):
            errors.append(f"{MQTT_CPP}: open_client() destroys {destroys} — only its own client, "
                          "after a failed esp_mqtt_client_start( (never started: nothing to wait for) (F106)")
    serve = body_of(code, SIG_SERVE, f"{MQTT_CPP}: serve_reinit()", errors)
    if serve is not None:
        s = squash(serve)
        first_guard = s.find("if(!retire_finished())return;")
        at_wanted_cmp = s.find("s_reinit_wanted.load(")
        at_detach = s.find("detach_client();")
        second_guard = s.find("if(!retire_finished())return;", at_detach) if at_detach >= 0 else -1
        at_capture = s.rfind("constuint32_twanted=s_reinit_wanted.load(")
        at_open = s.find("open_client();")
        at_served = s.find("s_reinit_served.store(wanted")
        ok = (0 <= first_guard < at_wanted_cmp and first_guard < at_detach < second_guard < at_capture
              < at_open < at_served and s.count("open_client(") == 1)
        if not ok:
            errors.append(f"{MQTT_CPP}: serve_reinit() must wait for a retiring client without "
                          "blocking (`if (!retire_finished()) return;` first, and again after "
                          "detach_client()), then read `const uint32_t wanted = s_reinit_wanted.load(` "
                          "(a request made before the open's NVS read is the open's), run "
                          "open_client() once, then `s_reinit_served.store(wanted` (F106)")
    loop = body_of(code, SIG_MQTT_LOOP, f"{MQTT_CPP}: csi_mqtt::loop()", errors)
    if loop is not None:
        s = squash(loop)
        at_serve = s.find("serve_reinit();")
        at_pump = s.find("csi_event_egress::pump();")
        if s.count("serve_reinit();") != 1 or at_pump < 0 or at_serve > at_pump:
            errors.append(f"{MQTT_CPP}: csi_mqtt::loop() must call serve_reinit() once, before "
                          "csi_event_egress::pump() — the pass that opens a new destination's client "
                          "is the pass the pump sees its epoch (F106)")
        # The topic is a string: found in the source, placed by the blanked
        # code's offsets (the same in both).
        span = the_body(code, SIG_MQTT_LOOP, "", [])
        raw_loop = mqtt[span[0]:span[1]] if span is not None else ""
        if not re.search(r'build_topic\s*\([^;]*"update/auto"\s*\)', raw_loop) or \
                "publish_raw(" not in loop:
            errors.append(f"{MQTT_CPP}: csi_mqtt::loop() must publish the update/auto state "
                          "set_update_auto_state() left (F106)")
    finished = body_of(code, SIG_RETIRE_FINISHED, f"{MQTT_CPP}: retire_finished()", errors)
    if finished is not None and "xTaskCreate(retire_task," not in squash(finished):
        errors.append(f"{MQTT_CPP}: retire_finished() must hand the detached client to "
                      "xTaskCreate(retire_task, ...) — the stop is the worker's (F106)")
    req = body_of(code, SIG_REQUEST, f"{MQTT_CPP}: request_reinit()", errors)
    if req is not None:
        if "s_reinit_wanted.fetch_add(" not in squash(req) or re.search(
                BARE_INIT + r"|\b(?:open_client|serve_reinit|detach_client|retire_finished)\s*\(|publish",
                req):
            errors.append(f"{MQTT_CPP}: request_reinit() only bumps s_reinit_wanted — any task "
                          "calls it (F106)")
    auto = body_of(code, SIG_SET_AUTO, f"{MQTT_CPP}: set_update_auto_state()", errors)
    if auto is not None and re.search(r"\b(?:publish_raw|build_topic)\s*\(", auto):
        errors.append(f"{MQTT_CPP}: set_update_auto_state() publishes — it runs on the httpd task "
                      "too, and only caches; loop() publishes (F106)")
    handler = body_of(code, SIG_EVENT_HANDLER, f"{MQTT_CPP}: mqtt_event_handler()", errors)
    if handler is not None:
        s = squash(handler)
        guard = re.search(r"if\(!e\|\|e->client!=s_client\.load\([^)]*\)\)return;", s)
        if guard is None or guard.start() > s.find("switch("):
            errors.append(f"{MQTT_CPP}: mqtt_event_handler() must return first for an event whose "
                          "client is not s_client (`if (!e || e->client != s_client.load(...)) return;`) "
                          "— a detached client runs on until its worker's stop returns (F106)")
        if re.search(r"esp_mqtt_client_subscribe\((?!e->client,)", s):
            errors.append(f"{MQTT_CPP}: mqtt_event_handler() subscribes on another client than "
                          "e->client — the loop task may have detached s_client meanwhile (F106)")
        if re.search(BARE_INIT + r"|\b(?:open_client|serve_reinit|detach_client|retire_finished|"
                     r"esp_mqtt_client_stop|esp_mqtt_client_destroy)\s*\(", handler):
            errors.append(f"{MQTT_CPP}: mqtt_event_handler() runs a client lifecycle step — it runs on "
                          "the esp_mqtt task (F106)")
    for sig, what in ((SIG_CONFIG_POST, "handle_config_post()"), (SIG_TEST, "handle_test()")):
        body = body_of(code, sig, f"{MQTT_CPP}: {what}", errors)
        if body is not None and body.count("request_reinit(") != 1:
            errors.append(f"{MQTT_CPP}: {what} must ask the loop task for its re-init with "
                          "request_reinit( (F106)")


# What no HTTP handler may name. The owner commands of rule 1 are refused
# qualified anywhere outside mesh_network.cpp (rule 5), and unqualified they
# do not compile outside it (internal linkage).
HTTPD_FORBIDDEN = (
    (BARE_INIT, "init("),
    (r"\bcsi_mqtt::init\s*\(", "csi_mqtt::init("),
    (r"\b(?:open_client|serve_reinit|detach_client|retire_finished)\s*\(|\bretire_task\b",
     "a client lifecycle step of csi_mqtt.cpp"),
    (r"\besp_mqtt_client_\w+\s*\(", "an esp_mqtt_client_ call"),
    (S_CLIENT_WRITE, "a write of s_client"),
    (r"\bcsi_mqtt::loop\s*\(", "csi_mqtt::loop("),
    (r"\bmesh_network::update\s*\(", "mesh_network::update("),
    (r"(?<![\w:.>])publish_\w+\s*\(", "a publish_ function"),
    (r"\bcsi_mqtt::publish_\w+\s*\(", "csi_mqtt::publish_"),
)


@functools.lru_cache(maxsize=1024)
def httpd_findings(name: str, code: str) -> tuple[str, ...]:
    out = []
    for hname, s, e in handler_spans(code):
        body = code[s:e]
        for pattern, label in HTTPD_FORBIDDEN:
            if re.search(pattern, body):
                out.append(f"{name}: HTTP handler {hname}() names {label} — the httpd task "
                           "hands that work to the loop task (F96, F106)")
    return tuple(out)


def check_httpd_paths(files: dict[str, str], errors: list[str]) -> None:
    for name, src in files.items():
        errors.extend(httpd_findings(name, blank_comments_and_strings(src)))


@functools.lru_cache(maxsize=4096)
def call_sites(c: str, name: str) -> tuple[int, ...]:
    """Where `name(` is called in blanked `c`: declarations and definitions
    (a type before the name) are not calls."""
    if name.split("::")[-1] not in c:
        return ()
    out = []
    for m in re.finditer(r"(?<![\w.>])" + re.escape(name) + r"\s*\(", c):
        if re.search(r"\b(?:void|bool|esp_err_t|uint32_t)\s*$", c[max(0, m.start() - 40):m.start()]):
            continue
        out.append(m.start())
    return tuple(out)


def check_mqtt_sketch(ino: str, others: dict[str, str], errors: list[str]) -> None:
    files = dict(others)
    files[INO] = ino
    code = {name: blank_comments_and_strings(src) for name, src in files.items()}
    for name, c in code.items():
        if re.search(r"\busing\s+namespace\s+csi_mqtt\b", c):
            errors.append(f"{name}: `using namespace csi_mqtt` hides the bridge's callers from this "
                          "check — call it qualified")
    ino_code = code[INO]

    def sites(call: str) -> list[tuple[str, int]]:
        return [(fname, p) for fname, c in code.items() for p in call_sites(c, call[:-1])]

    def only_in(call: str, sig: str, where: str, times: int = 1) -> None:
        found = sites(call)
        span = the_body(ino_code, sig, f"{INO}: {where}", errors)
        inside = [p for f, p in found if f == INO and span is not None and span[0] <= p < span[1]]
        if len(inside) != len(found) or not (1 <= len(found) <= times):
            errors.append(f"{SKETCH}: `{call}` must be called only from {where} "
                          f"({'once' if times == 1 else f'at most {times} times'}; found {len(found)}, "
                          f"{len(found) - len(inside)} elsewhere) — the bridge's client is the loop "
                          "task's (F106)")

    only_in("csi_mqtt::init(", SIG_REGISTER, "register_api_routes()")
    # One of start_http_server()'s two servers registers the routes per boot.
    only_in("register_api_routes(", SIG_START_HTTP, "start_http_server()", times=2)
    only_in("start_http_server(", SIG_SETUP, "setup()")
    only_in("csi_mqtt::set_identity(", SIG_SETUP, "setup()")
    only_in("csi_mqtt::loop(", SIG_INO_LOOP, "loop()")
    qr = body_of(ino_code, SIG_QR_TASK, f"{INO}: qr_scan_task_fn()", errors)
    if qr is not None and "csi_mqtt::request_reinit(" not in qr:
        errors.append(f"{INO}: qr_scan_task_fn() must ask for the bridge's re-init with "
                      "csi_mqtt::request_reinit( — it runs on the scanner's task (F106)")


def check(ino: str, mesh_h: str, mesh_cpp: str, mqtt: str, others: dict[str, str]) -> list[str]:
    errors: list[str] = []
    files = dict(others)
    files[INO] = ino
    files[MESH_H] = mesh_h
    files[MESH_CPP] = mesh_cpp
    files[MQTT_CPP] = mqtt
    rest = {k: v for k, v in files.items() if k != INO}
    check_mesh_internal(mesh_h, mesh_cpp, errors)
    check_mesh_callers(mesh_cpp, errors)
    check_mesh_sketch(ino, rest, errors)
    check_mqtt_reinit(mqtt, errors)
    check_httpd_paths(files, errors)
    check_mqtt_sketch(ino, rest, errors)
    return errors


# ── Self-test: mutations the check must refuse ───────────────────────────

Srcs = dict  # {"ino", "mesh_h", "mesh_cpp", "mqtt"}
Mutation = Callable[[dict], dict]


def on(key: str, sig: str, pattern: str, repl: str, need: str | None = None) -> Mutation:
    def mutate(s: dict) -> dict:
        out = dict(s)
        out[key] = mutate_in(s[key], sig, pattern, repl, need=need)
        return out
    return mutate


def raw(key: str, old: str, new: str) -> Mutation:
    def mutate(s: dict) -> dict:
        if s[key].count(old) != 1:
            raise AnchorMissing(old)
        out = dict(s)
        out[key] = s[key].replace(old, new, 1)
        return out
    return mutate


def ino_handler(h: str) -> str:
    return r"\bstatic\s+esp_err_t\s+" + h + r"\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)"


SUBMIT_IF = (r"const\s+loop_command_ring::Wait\s+w\s*=\s*mesh_network::submit\(cmd,\s*&ok\);\s*"
             r"if\s*\(w\s*!=\s*loop_command_ring::Wait::kDone\)\s*\{[^}]*\}")

MUTATIONS: list[tuple[str, Mutation]] = [
    # Rule 5: a handler changes the mesh itself.
    ("the remove handler calls remove_peer itself",
     on("ino", ino_handler("handle_mesh_remove"), SUBMIT_IF,
        "ok = mesh_network::remove_peer(cmd.fingerprint);")),
    ("the alerts DELETE clears the history on the httpd task too",
     on("ino", ino_handler("handle_mesh_alerts_clear"), r"(bool\s+ok\s*=\s*false;)",
        r"\1 mesh_network::clear_alerts();")),
    ("the leave handler answers 200 to a command that did not run",
     on("ino", ino_handler("handle_mesh_leave"),
        r"if\s*\(w\s*!=\s*loop_command_ring::Wait::kDone\)\s*\{[^}]*\}", "(void)w;")),
    ("the loop task submits a command (it would wait for itself)",
     on("ino", SIG_INO_LOOP, r"(mesh_network::update\(\);)",
        r"\1 (void)mesh_network::submit(mesh_network::make_command(mesh_network::MESH_CMD_CLEAR_ALERTS), nullptr);")),
    ("the sketch hides the mesh's callers behind a using-directive",
     raw("ino", '#include "mesh_network.h"', '#include "mesh_network.h"\nusing namespace mesh_network;')),
    ("the sketch calls mesh_network::update twice",
     on("ino", SIG_INO_LOOP, r"(mesh_network::update\(\);)", r"\1 mesh_network::update();")),
    # Rule 1: the owner commands are internal.
    ("mesh_network.h declares remove_peer again",
     raw("mesh_h", "MESH_CMD_SET_ENABLED.)\nbool is_enabled();",
         "MESH_CMD_SET_ENABLED.)\nbool is_enabled();\nbool remove_peer(const uint8_t* fingerprint);")),
    ("remove_peer is defined without static",
     raw("mesh_cpp", "static bool remove_peer(const uint8_t* fingerprint) {\n",
         "bool remove_peer(const uint8_t* fingerprint) {\n")),
    # Rule 2: only run_command (and update's loop-task paths, for cancel) call them.
    ("update() clears the alert history itself",
     on("mesh_cpp", SIG_UPDATE, r"(g_commands\.drain\(run_command\);)", r"\1 clear_alerts();")),
    ("send_heartbeat cancels the pairing",
     on("mesh_cpp", r"\bvoid\s+send_heartbeat\s*\(\s*\)", r"(if\s*\(!g_opera_config\.configured\)\s*return;)",
        r"\1 cancel_pairing();")),
    ("update() runs a command directly",
     on("mesh_cpp", SIG_UPDATE, r"(g_commands\.drain\(run_command\);)",
        r"\1 (void)run_command(make_command(MESH_CMD_LEAVE));")),
    # Rule 3: update drains first.
    ("update() drains after its disabled return",
     lambda s: on("mesh_cpp", SIG_UPDATE, r"\n[ \t]*g_commands\.drain\(run_command\);", "")(
         on("mesh_cpp", SIG_UPDATE, r"(mesh_channel_policy::poll_radio\(\);)",
            r"g_commands.drain(run_command); \1")(s))),
    ("update() never drains",
     on("mesh_cpp", SIG_UPDATE, r"\n[ \t]*g_commands\.drain\(run_command\);", "")),
    ("send_heartbeat drains the ring too",
     on("mesh_cpp", r"\bvoid\s+send_heartbeat\s*\(\s*\)", r"(if\s*\(!g_opera_config\.configured\)\s*return;)",
        r"\1 g_commands.drain(run_command);")),
    # Rule 4: submit only posts; the Wi-Fi task's callbacks touch none of it.
    ("submit() runs the command in place",
     on("mesh_cpp", SIG_SUBMIT, r"(bool\s+result\s*=\s*false;)",
        r"\1 if (ok != nullptr) *ok = run_command(cmd); return loop_command_ring::Wait::kDone;")),
    ("submit() drains the ring itself",
     on("mesh_cpp", SIG_SUBMIT, r"(bool\s+result\s*=\s*false;)", r"\1 g_commands.drain(run_command);")),
    ("the ESP-NOW receive callback drains the ring",
     on("mesh_cpp", SIG_RECV_CB, r"(g_rx_pending\s*=\s*true;)", r"\1 g_commands.drain(run_command);")),
    # Rule 6: the loop task serves a re-init without ever stopping a client;
    # only the worker stops one.
    ("the config POST runs init() itself",
     on("mqtt", SIG_CONFIG_POST, r"\(void\)wait_reinit\(request_reinit\(\),\s*kReinitWaitMs\);",
        "init(s_device_id, s_firmware_version, s_public_key_hex);")),
    ("the test handler runs init() itself",
     on("mqtt", SIG_TEST, r"wait_reinit\(request_reinit\(\),\s*kTestBudgetMs\)",
        "init(s_device_id, s_firmware_version, s_public_key_hex)")),
    ("the test handler stops and destroys the client by hand (the review's S1)",
     on("mqtt", SIG_TEST, r"(const\s+uint32_t\s+start\s*=\s*millis\(\);)",
        r"{ esp_mqtt_client_handle_t c = s_client.load(); if (c) { esp_mqtt_client_stop(c); "
        r"esp_mqtt_client_destroy(c); s_client = nullptr; } } \1")),
    ("the config POST publishes on the client directly (the review's S2)",
     on("mqtt", SIG_CONFIG_POST, r"(\(void\)wait_reinit\(request_reinit\(\),\s*kReinitWaitMs\);)",
        r'\1 esp_mqtt_client_publish(s_client.load(), "t", "p", 1, 0, 0);')),
    ("loop() no longer serves re-inits",
     on("mqtt", SIG_MQTT_LOOP, r"\n[ \t]*serve_reinit\(\);", "")),
    ("loop() serves the re-init after the pump (the review's MQ3)",
     lambda s: on("mqtt", SIG_MQTT_LOOP, r"(csi_event_egress::pump\(\);)", r"\1 serve_reinit();")(
         on("mqtt", SIG_MQTT_LOOP, r"\n[ \t]*serve_reinit\(\);", "")(s))),
    ("serve_reinit() stops the old client on the loop task (the reviewed shape)",
     on("mqtt", SIG_SERVE, r"detach_client\(\);\s*if\s*\(!retire_finished\(\)\)\s*return;[^\n]*",
        "esp_mqtt_client_handle_t old = s_client.exchange(nullptr); "
        "esp_mqtt_client_stop(old); esp_mqtt_client_destroy(old);")),
    ("serve_reinit() opens the new client before the worker is done",
     on("mqtt", SIG_SERVE, r"(detach_client\(\);\s*)if\s*\(!retire_finished\(\)\)\s*return;",
        r"\1(void)retire_finished();")),
    ("serve_reinit() opens while a client is still retiring",
     on("mqtt", SIG_SERVE, r"if\s*\(!retire_finished\(\)\)\s*return;", "")),
    ("serve_reinit() marks every request so far served (the review's MQ2)",
     on("mqtt", SIG_SERVE, r"s_reinit_served\.store\(wanted,", "s_reinit_served.store(s_reinit_wanted.load(),")),
    ("serve_reinit() reads the requests before it detaches",
     lambda s: on("mqtt", SIG_SERVE, r"(\n[ \t]*if\s*\(s_client\.load\()",
                  r"\n  const uint32_t wanted = s_reinit_wanted.load(std::memory_order_acquire);\1")(
         on("mqtt", SIG_SERVE, r"\n[ \t]*const\s+uint32_t\s+wanted\s*=\s*s_reinit_wanted\.load\([^;]*;", "")(s))),
    ("init() stops an open client in place",
     on("mqtt", r"\bbool\s+init\s*\([^)]*\)", r"\(void\)request_reinit\(\);",
        "{ esp_mqtt_client_handle_t old = s_client.exchange(nullptr); esp_mqtt_client_stop(old); "
        "esp_mqtt_client_destroy(old); }")),
    ("retire_finished() stops the client itself when the worker cannot start",
     on("mqtt", SIG_RETIRE_FINISHED, r"(if\s*\(!s_retire_create_failed_logged\)\s*\{)",
        r"esp_mqtt_client_stop(s_retiring); esp_mqtt_client_destroy(s_retiring); \1")),
    ("retire_finished() runs the worker's body in place",
     on("mqtt", SIG_RETIRE_FINISHED,
        r"if\s*\(xTaskCreate\(retire_task,[^;]*?\)\s*!=\s*pdPASS\)\s*\{",
        "retire_task(s_retiring); if (false) {")),
    ("loop() stops the client itself",
     on("mqtt", SIG_MQTT_LOOP, r"(csi_event_egress::pump\(\);)", r"esp_mqtt_client_stop(s_client.load()); \1")),
    ("open_client() destroys the open client",
     on("mqtt", SIG_OPEN, r"(ca_load\(\);)", r"esp_mqtt_client_destroy(s_client.load()); \1")),
    ("open_client() starts the client before it is s_client",
     lambda s: on("mqtt", SIG_OPEN, r'(\n[ \t]*Serial\.printf\("\[MQTT\] bridge started)',
                  r"\n  s_client.store(client, std::memory_order_release);\1")(
         on("mqtt", SIG_OPEN, r"\n[ \t]*s_client\.store\(client,[^;]*;", "")(s))),
    ("publish_raw() clears the client",
     on("mqtt", r"\bbool\s+publish_raw\s*\([^)]*\)", r"(if\s*\(msg_id\s*<\s*0\))",
        r"if (msg_id < -1) s_client = nullptr; \1")),
    ("request_reinit() re-inits in place",
     on("mqtt", SIG_REQUEST, r"(return\s+s_reinit_wanted)", r"serve_reinit(); \1")),
    ("the esp_mqtt handler re-inits",
     on("mqtt", SIG_EVENT_HANDLER, r"(s_connected\.store\(false,[^;]*;)", r"\1 (void)init(nullptr, nullptr, nullptr);")),
    ("the esp_mqtt handler stops its own client",
     on("mqtt", SIG_EVENT_HANDLER, r"(s_connected\.store\(false,[^;]*;)", r"\1 esp_mqtt_client_stop(e->client);")),
    ("the esp_mqtt handler handles a detached client's events",
     on("mqtt", SIG_EVENT_HANDLER, r"if\s*\(!e\s*\|\|\s*e->client\s*!=\s*s_client\.load\([^)]*\)\)\s*return;", "")),
    ("the esp_mqtt handler subscribes on s_client",
     on("mqtt", SIG_EVENT_HANDLER, r"esp_mqtt_client_subscribe\(e->client,", "esp_mqtt_client_subscribe(s_client.load(),")),
    ("set_update_auto_state publishes in place",
     on("mqtt", SIG_SET_AUTO, r"(s_update_auto_dirty\.store\([^;]*;)",
        r'\1 { char t[192]; build_topic(t, sizeof(t), "update/auto"); publish_raw(t, "ON", 2, true); }')),
    ("loop() never publishes the auto-update state",
     on("mqtt", SIG_MQTT_LOOP, r'build_topic\(topic,\s*sizeof\(topic\),\s*"update/auto"\);',
        'build_topic(topic, sizeof(topic), "update/x");')),
    # Rules 7, 8: the sketch's other tasks.
    ("the OTA settings handler publishes the switch on the httpd task",
     on("ino", ino_handler("handle_ota_config"), r"csi_mqtt::set_update_auto_state\(enabled\);",
        "csi_mqtt::publish_update_state(\"{}\");")),
    ("the QR scanner runs init() itself",
     on("ino", SIG_QR_TASK, r"\(void\)csi_mqtt::request_reinit\(\);",
        'csi_mqtt::init(g_device.device_id, FIRMWARE_VERSION, "");')),
    ("start_http_server() inits the bridge a second time",
     on("ino", SIG_START_HTTP, r"(register_api_routes\(g_http_server\);)",
        r'\1 csi_mqtt::init(g_device.device_id, FIRMWARE_VERSION, "");')),
    ("loop() starts the HTTP server (and with it the boot init) again",
     on("ino", SIG_INO_LOOP, r"(mesh_network::update\(\);)", r"\1 start_http_server();")),
    ("setup() inits the bridge a second time",
     on("ino", SIG_SETUP, r"(csi_mqtt::set_identity\([^;]*;)",
        r'\1 csi_mqtt::init(g_device.device_id, FIRMWARE_VERSION, "");')),
    ("setup() never gives the bridge its identity",
     on("ino", SIG_SETUP, r"csi_mqtt::set_identity\([^;]*;", "")),
    ("an HTTP handler pumps the bridge",
     on("ino", ino_handler("handle_ota_config"), r"(csi_mqtt::set_update_auto_state\(enabled\);)",
        r"\1 csi_mqtt::loop();")),
    ("the sketch hides the bridge's callers behind a using-directive",
     raw("ino", '#include "csi_mqtt.h"', '#include "csi_mqtt.h"\nusing namespace csi_mqtt;')),
]


def self_test(srcs: dict, others: dict[str, str]) -> list[str]:
    problems = []
    for name, mutate in MUTATIONS:
        try:
            m = mutate(srcs)
        except AnchorMissing as missing:
            problems.append(f"self-test: mutation '{name}' no longer applies (anchor {missing}) — "
                            "the source changed shape; update this guard's mutations with it")
            continue
        if m == srcs:
            problems.append(f"self-test: mutation '{name}' changed nothing")
        elif not check(m["ino"], m["mesh_h"], m["mesh_cpp"], m["mqtt"], others):
            problems.append(f"self-test: the check did not bite on mutation '{name}'")
    return problems


def sketch_others() -> dict[str, str]:
    """Every other source file of the sketch, by repo-relative path."""
    skip = {INO, MESH_H, MESH_CPP, MQTT_CPP}
    out = {}
    for pattern in SKETCH_GLOBS:
        for path in sorted((REPO / SKETCH).glob(pattern)):
            rel = path.relative_to(REPO).as_posix()
            if rel not in skip:
                out[rel] = path.read_text(encoding="utf-8", errors="replace")
    return out


def main() -> int:
    srcs = {
        "ino": (REPO / INO).read_text(encoding="utf-8"),
        "mesh_h": (REPO / MESH_H).read_text(encoding="utf-8"),
        "mesh_cpp": (REPO / MESH_CPP).read_text(encoding="utf-8"),
        "mqtt": (REPO / MQTT_CPP).read_text(encoding="utf-8"),
    }
    others = sketch_others()
    errors = check(srcs["ino"], srcs["mesh_h"], srcs["mesh_cpp"], srcs["mqtt"], others)
    for err in errors:
        print(f"::error::{err}")
    problems = self_test(srcs, others)
    for problem in problems:
        print(f"::error::{problem}")
    if errors or problems:
        return 1
    print(f"canary-wap loop-task ownership holds: the mesh's owner commands are internal to "
          f"mesh_network.cpp and run from update()'s drain, the REST handlers only submit, and the "
          f"MQTT client is replaced only by loop()'s re-init, which never stops a client (the "
          f"retire_task worker does) "
          f"({len(MUTATIONS)} mutations refused).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
