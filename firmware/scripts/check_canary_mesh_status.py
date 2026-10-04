#!/usr/bin/env python3
"""Hold the PlatformIO canary's mesh status routes to the view the main loop
publishes (sweep F161), and its alerts route to the log the main loop changes
under its lock (sweep F197).

`GET /api/mesh` and `/api/mesh/peers` (`handle_mesh_status`,
`handle_mesh_peers` in `securacv_network.cpp`) run on esp_http_server's
task. Until F161 they read `mesh_session`'s state in place (the pairing
state and code, F133's pairing number, outcome and reason, the opera name,
the peer links and the transport table) while the main loop's
`mesh_session::process()` wrote it, so one body could mix two passes. Now
the main loop builds a `StatusView` and publishes it
(`mesh_session::publish_status()`, through `loop_snapshot.h`), and the two
routes copy it (`mesh_session::read_status()`). `GET /api/mesh/alerts`
(`handle_mesh_alerts`) copied the alert ring in place while the main loop's
receive path stored into it and a DELETE cleared it; now the history is a
`loop_snapshot::Log` the main loop appends to and clears under its lock,
and the route copies it with `mesh_session::read_alerts()`, newest first
(F197).

`test_mesh_session.cpp` runs the session, the view and the JSON on the
host: the read is the last published pass, a read right after a request's
answer shows the request, a stopped session still publishes, deinit()
publishes, the code is kept only while shown, the peer join, and a
two-thread run; and the alert history newest first after the log wraps, a
cap, a clear, a leave, deinit, and a two-thread run of stores and clears.
No host test compiles `securacv_network.cpp` or `main.cpp` (only CI's
`[env:full]` build does), and none can see which task a call runs on; this
check holds the sources to the shape those tests assume. Nor does a test
see the device's lock, and CI builds the two-thread runs without a
sanitizer, where a host lock that does nothing usually still passes (11
of 12 such runs did; a local ThreadSanitizer build catches it every time);
rule 7 holds both locks' shape.

## The rules

1. No HTTP handler in `securacv_network.cpp` (a `static esp_err_t
   name(httpd_req_t* req)` function) names a live reader of the session's
   or the transport's state (`LIVE_READERS` below: `pairing_seq`,
   `pairing_state`, `get_opera_name`, `get_peer_links`,
   `mesh_transport::list_peers` and the rest, `get_alerts` among them).
   No allowance: F161 allowed `handle_mesh_alerts`' `get_alerts` until the
   alerts route moved, which F197 did.
2. `handle_mesh_status` and `handle_mesh_peers` each read the view once
   (`mesh_session::read_status(&view);`), and `handle_mesh_peers` joins its
   rows with `mesh_api::peer_views_from_status(view, ...)` once.
3. In `mesh_session.cpp`, `process()` calls `publish_status();` right before
   every `return` and as its last statement (a stopped session's transport
   table still ages); `drain_request()` calls it once, after
   `execute_request(` and before `slot_cas(SLOT_RUNNING, SLOT_DONE)` (the
   handler collects the result at DONE and the page reads the status right
   after); `deinit()` ends with it. Nothing else in the file calls it.
4. `s_status_view` is published only in `publish_status()` and read only in
   `read_status()`, and `read_status()` names no live state (it runs on the
   httpd task).
5. `main.cpp` calls `mesh_session::publish_status();` once, in `setup()`,
   after `mesh_session::set_opera_create_handler(` (the end of the mesh
   restore: the HTTP server is up before the first loop pass), and no other
   file under `firmware/canary` calls it: a handler that published would
   run the main loop's build on the httpd task.
6. `handle_mesh_alerts` reads the alert log once
   (`mesh_session::read_alerts(recs, mesh_session::MAX_ALERT_HISTORY);`).
   In `mesh_session.cpp` (F197), `s_alert_log` is appended to once, in
   `dispatch_verified()` (the TAMPER_ALERT receive path), cleared only in
   `clear_alerts()` and `reset_alerts()` (once each), read once, in
   `read_alerts()`, and attached (and its `storage()` asked) only in
   `init()`; `s_alert_store`, the records' memory, is named only by its
   declaration and that attach; `read_alerts()` names no live state; and no
   in-place history is left (`s_alert_ring`, `s_alert_head`,
   `s_alert_count`, `get_alerts`).
7. The locks those snapshots copy under are real, and the alert log has one
   writer (F197's review). In `mesh_session.cpp`, `AlertLogLock` and
   `StatusViewLock` are each defined twice, under `#ifdef
   CSI_TEST_HOST_BUILD` (a `std::mutex m` whose `lock()` / `unlock()` are
   `m.lock()` / `m.unlock()`, the lock a two-thread host test runs) and in
   its `#else` (`portENTER_CRITICAL(&mux)` / `portEXIT_CRITICAL(&mux)` on
   its own `static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;`,
   declared once in that branch and named nowhere else: `s_alert_log_mux`,
   `s_status_view_mux`); `s_alert_log` is a `loop_snapshot::Log<AlertEntry,
   MAX_ALERT_HISTORY, AlertLogLock>` and `s_status_view` a
   `loop_snapshot::Value<StatusView, StatusViewLock>`. `Log::clear()` zeroes
   the records after it drops the lock, so it relies on no append running on
   another task: `clear_alerts()` is called only in `execute_request()`
   (the `RequestType::CLEAR_ALERTS` case, run by the main loop's request
   drain) and `reset_alerts()` only in `deinit()` and `leave_opera()`; no
   other file under `firmware/canary` calls either; in
   `securacv_network.cpp` no HTTP handler names them,
   `RequestType::CLEAR_ALERTS` is posted only by `handle_mesh_alerts_clear`,
   once, before its one `mesh_call(req, r, &res, &rc)`, and a `#pragma GCC
   poison` ahead of the first mesh handler still names `clear_alerts` and
   the other main-loop mutators (the compile error `[env:full]` gives a
   direct call).

Each rule is proved to bite: `self_test()` mutates the real sources in
memory and requires the check to fail on every mutation.

Run:  python3 firmware/scripts/check_canary_mesh_status.py
(firmware/scripts/regression_check.sh runs it.)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Callable

REPO = Path(__file__).resolve().parents[2]
NETWORK = "firmware/canary/lib/securacv_network/src/securacv_network.cpp"
SESSION = "firmware/canary/lib/securacv_mesh/src/mesh_session.cpp"
MAIN = "firmware/canary/src/main.cpp"
CANARY = "firmware/canary"

# Rule 1: what an HTTP handler must not read. The session's are its status
# accessors; the transport's are its peer-table readers.
LIVE_READERS = {
    "mesh_session": (
        "is_enabled", "is_running", "has_opera", "has_opera_secret", "get_opera_id",
        "get_opera_name", "pairing_state", "pairing_confirmation_code", "pairing_seq",
        "pairing_outcome", "pairing_fail_reason", "trusted_peer_count",
        "online_peer_count", "alerts_received", "get_alerts", "get_peer_links",
        "get_paired_peer_pubkey", "get_paired_peer_mac", "get_replay_counters",
        "outbound_counter", "rekey_in_progress", "revoked_count", "is_revoked",
        "can_hold_partner", "encode_revocations",
    ),
    "mesh_transport": ("list_peers", "get_peer", "has_peer"),
}
STATUS_HANDLERS = ("handle_mesh_status", "handle_mesh_peers")

# Rule 4: names read_status() must not touch.
LIVE_STATE = (
    "s_ctx", "s_trusted_peers", "s_opera_name", "s_opera_id", "s_opera_id_set",
    "s_enabled", "s_running", "s_pairing_seq", "s_alerts_received", "build_status_view",
    "get_peer_links", "online_peer_count", "trusted_peer_count", "list_peers",
)

# Rule 6 (F197): where the alert log is touched, and what read_alerts() must
# not name (it runs on the httpd task).
ALERT_LOG_USES = {
    "append": (("dispatch_verified",), 1),
    "clear": (("clear_alerts", "reset_alerts"), 2),
    "read": (("read_alerts",), 1),
    "attach": (("init",), 1),
    "storage": (("init",), 1),
}
ALERT_READER_LIVE = LIVE_STATE + ("s_alert_store", "s_alert_seq")
OLD_ALERT_RING = ("s_alert_ring", "s_alert_head", "s_alert_count", "get_alerts")

# Rule 7: each lock, its device portMUX, and the snapshot that locks with it.
LOCKS = (
    ("AlertLogLock", "s_alert_log_mux",
     r"\bstatic\s+loop_snapshot::Log\s*<\s*AlertEntry\s*,\s*MAX_ALERT_HISTORY\s*,\s*"
     r"AlertLogLock\s*>\s*s_alert_log\s*;"),
    ("StatusViewLock", "s_status_view_mux",
     r"\bstatic\s+loop_snapshot::Value\s*<\s*StatusView\s*,\s*StatusViewLock\s*>\s*"
     r"s_status_view\s*;"),
)
HOST_LOCK_BODY = "{std::mutexm;voidlock(){m.lock();}voidunlock(){m.unlock();}}"
ALERT_CLEARERS = {"clear_alerts": ("execute_request",), "reset_alerts": ("deinit", "leave_opera")}
MAIN_LOOP_MUTATORS = ("leave_opera", "set_opera_name", "set_enabled", "clear_alerts", "remove_peer",
                      "start_pairing_initiator", "start_pairing_joiner", "confirm_pairing_code",
                      "cancel_pairing")

HANDLER_SIG = re.compile(r"\bstatic\s+esp_err_t\s+(\w+)\s*\(\s*httpd_req_t\s*\*\s*\w+\s*\)\s*\{")


class AnchorMissing(Exception):
    pass


def blank(src: str) -> str:
    """Comments and string/char literals blanked to spaces, offsets kept
    (raw strings too: the web pages are raw strings)."""
    out = list(src)
    i, n = 0, len(src)

    def wipe(a: int, b: int) -> None:
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            wipe(i, j)
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            wipe(i, j)
            i = j
        elif src.startswith('R"', i) and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == "_")):
            m = re.match(r'R"([^(\s]*)\(', src[i:])
            if m is None:
                i += 1
                continue
            end = src.find(")" + m.group(1) + '"', i + m.end())
            j = n if end < 0 else end + len(m.group(1)) + 2
            wipe(i, j)
            i = j
        elif src[i] in "\"'":
            q = src[i]
            j = i + 1
            while j < n and src[j] != q and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            wipe(i, min(j + 1, n))
            i = j + 1
        else:
            i += 1
    return "".join(out)


def block_at(code: str, brace: int) -> tuple[int, int]:
    depth = 0
    for i in range(brace, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return brace, i + 1
    return brace, len(code)


def function_spans(code: str, sig: str) -> list[tuple[int, int]]:
    out = []
    for m in re.finditer(sig, code):
        out.append(block_at(code, m.end() - 1))
    return out


def one_body(code: str, sig: str, what: str, errors: list[str]) -> str | None:
    spans = function_spans(code, sig)
    if len(spans) != 1:
        errors.append(f"{what}: found {len(spans)} definitions; expected one")
        return None
    a, b = spans[0]
    return code[a:b]


def squash(s: str) -> str:
    return re.sub(r"\s+", "", s)


def handler_bodies(code: str) -> list[tuple[str, str]]:
    out = []
    for m in HANDLER_SIG.finditer(code):
        a, b = block_at(code, m.end() - 1)
        out.append((m.group(1), code[a:b]))
    return out


def enclosing(code: str, pos: int) -> str | None:
    """The name of the function definition whose body holds `pos`."""
    best = None
    for m in re.finditer(r"\b(\w+)\s*\([^;{}()]*(?:\([^()]*\)[^;{}()]*)*\)\s*(?:const\s*)?\{", code):
        name = m.group(1)
        if name in ("if", "for", "while", "switch", "catch", "return", "sizeof"):
            continue
        a, b = block_at(code, m.end() - 1)
        if a < pos < b and (best is None or a > best[0]):
            best = (a, name)
    return None if best is None else best[1]


def check_network(net_src: str, errors: list[str]) -> None:
    code = blank(net_src)
    for name, body in handler_bodies(code):
        for ns, readers in LIVE_READERS.items():
            for m in re.finditer(r"\b" + ns + r"::(" + "|".join(readers) + r")\s*\(", body):
                errors.append(f"{NETWORK}: HTTP handler {name}() reads {ns}::{m.group(1)}( — the "
                              "live state is the main loop's; read the published view "
                              "(mesh_session::read_status) (F161)")
        if re.search(r"\bpublish_status\s*\(", body):
            errors.append(f"{NETWORK}: HTTP handler {name}() calls publish_status( — the view is "
                          "built and published on the main loop only (F161)")
    bodies = dict(handler_bodies(code))
    for h in STATUS_HANDLERS:
        body = bodies.get(h)
        if body is None:
            errors.append(f"{NETWORK}: {h}() is missing")
            continue
        reads = re.findall(r"\bmesh_session::read_status\s*\(\s*&\s*view\s*\)\s*;", body)
        if len(reads) != 1:
            errors.append(f"{NETWORK}: {h}() must read the published view once "
                          f"(`mesh_session::read_status(&view);`, found {len(reads)}) (F161)")
    alerts = bodies.get("handle_mesh_alerts")
    if alerts is None:
        errors.append(f"{NETWORK}: handle_mesh_alerts() is missing")
    else:
        reads = re.findall(r"\bmesh_session::read_alerts\s*\(\s*recs\s*,\s*"
                           r"mesh_session::MAX_ALERT_HISTORY\s*\)\s*;", alerts)
        calls = re.findall(r"\bread_alerts\s*\(", alerts)
        if len(reads) != 1 or len(calls) != 1:
            errors.append(f"{NETWORK}: handle_mesh_alerts() must read the alert log once "
                          f"(`mesh_session::read_alerts(recs, mesh_session::MAX_ALERT_HISTORY);`, "
                          f"found {len(reads)}) (F197)")
    check_alert_clear_route(code, errors)
    peers = bodies.get("handle_mesh_peers")
    if peers is not None:
        joins = re.findall(r"\bmesh_api::peer_views_from_status\s*\(\s*view\s*,", peers)
        if len(joins) != 1:
            errors.append(f"{NETWORK}: handle_mesh_peers() must join its rows against the view "
                          f"once (`mesh_api::peer_views_from_status(view, ...)`, found "
                          f"{len(joins)}) (F161)")


def check_alert_clear_route(code: str, errors: list[str]) -> None:
    """Rule 7 (F197's review): the alert log's one writer is the main loop."""
    handlers = handler_bodies(code)
    for name, body in handlers:
        hit = re.search(r"\b(clear_alerts|reset_alerts)\s*\(", body)
        if hit:
            errors.append(f"{NETWORK}: HTTP handler {name}() calls {hit.group(1)}( — the alert log "
                          "is cleared on the main loop only; post RequestType::CLEAR_ALERTS through "
                          "mesh_call (F197)")
        if name != "handle_mesh_alerts_clear" and re.search(r"\bRequestType::CLEAR_ALERTS\b", body):
            errors.append(f"{NETWORK}: HTTP handler {name}() posts RequestType::CLEAR_ALERTS — only "
                          "DELETE /api/mesh/alerts clears the history (F197)")
    body = dict(handlers).get("handle_mesh_alerts_clear")
    if body is None:
        errors.append(f"{NETWORK}: handle_mesh_alerts_clear() is missing")
    else:
        posts = [m.start() for m in re.finditer(
            r"\br\.type\s*=\s*mesh_session::RequestType::CLEAR_ALERTS\s*;", body)]
        calls = [m.start() for m in re.finditer(r"\bmesh_call\s*\(\s*req\s*,\s*r\s*,\s*&\s*res\s*,"
                                                  r"\s*&\s*rc\s*\)", body)]
        if len(posts) != 1 or len(calls) != 1 or calls[0] < posts[0] or \
                len(re.findall(r"\bmesh_call\s*\(", body)) != 1:
            errors.append(f"{NETWORK}: handle_mesh_alerts_clear() must post "
                          "`r.type = mesh_session::RequestType::CLEAR_ALERTS;` once and then hand it "
                          "to the main loop once (`mesh_call(req, r, &res, &rc)`) (F197)")
    poisoned: set[str] = set()
    first = None
    for m in re.finditer(r"^[ \t]*#[ \t]*pragma[ \t]+GCC[ \t]+poison[ \t]+([^\n]*)$", code, re.M):
        poisoned.update(m.group(1).split())
        first = m.start() if first is None else first
    mesh_handlers = [m.start() for m in HANDLER_SIG.finditer(code) if m.group(1).startswith("handle_mesh_")]
    missing = [n for n in MAIN_LOOP_MUTATORS if n not in poisoned]
    if missing or first is None or not mesh_handlers or first > min(mesh_handlers):
        errors.append(f"{NETWORK}: a `#pragma GCC poison` ahead of the first mesh handler must name "
                      f"every main-loop mutator (missing: {', '.join(missing) or 'none'}) — a direct "
                      "call from a handler is then a compile error (F33 part 5, F197)")


SIG_PROCESS = r"\bvoid\s+process\s*\(\s*uint32_t\s+\w+\s*\)\s*\{"
SIG_DRAIN = r"\bstatic\s+void\s+drain_request\s*\(\s*uint32_t\s+\w+\s*\)\s*\{"
SIG_DEINIT = r"\bvoid\s+deinit\s*\(\s*\)\s*\{"
SIG_PUBLISH = r"\bvoid\s+publish_status\s*\(\s*\)\s*\{"
SIG_READ = r"\bvoid\s+read_status\s*\(\s*StatusView\s*\*\s*\w+\s*\)\s*\{"
PUBLISH_CALL = re.compile(r"(?<![\w:.>])publish_status\s*\(\s*\)\s*;")


def check_session(sess_src: str, errors: list[str]) -> None:
    code = blank(sess_src)
    proc = one_body(code, SIG_PROCESS, f"{SESSION}: process()", errors)
    if proc is not None:
        s = squash(proc[1:-1])
        rets = [m.start() for m in re.finditer(r"\breturn\b", s)]
        if not s.endswith("publish_status();") or any(
                not s[:r].endswith("publish_status();") for r in rets):
            errors.append(f"{SESSION}: process() must call publish_status() right before every "
                          "return and as its last statement — the routes show the pass it ends, "
                          "a stopped session's included (F161)")
    drain = one_body(code, SIG_DRAIN, f"{SESSION}: drain_request()", errors)
    if drain is not None:
        s = squash(drain)
        pubs = [m.start() for m in re.finditer(r"publish_status\(\);", s)]
        exe = s.find("execute_request(")
        done = s.find("slot_cas(SLOT_RUNNING,SLOT_DONE)")
        if len(pubs) != 1 or exe < 0 or done < 0 or not (exe < pubs[0] < done):
            errors.append(f"{SESSION}: drain_request() must call publish_status() once, after "
                          "execute_request( and before slot_cas(SLOT_RUNNING, SLOT_DONE) — the "
                          "handler collects the result at DONE and its page reads the status "
                          "right after (F161)")
    deinit = one_body(code, SIG_DEINIT, f"{SESSION}: deinit()", errors)
    if deinit is not None and not squash(deinit[1:-1]).endswith("publish_status();"):
        errors.append(f"{SESSION}: deinit() must end with publish_status() — no view of the "
                      "session it wiped (F161)")
    for m in PUBLISH_CALL.finditer(code):
        where = enclosing(code, m.start())
        if where not in ("process", "drain_request", "deinit"):
            errors.append(f"{SESSION}: {where}() calls publish_status() — only process(), "
                          "drain_request() and deinit() publish in this file (F161)")
    for m in re.finditer(r"\bs_status_view\s*\.\s*(\w+)", code):
        where = enclosing(code, m.start())
        want = {"publish": "publish_status", "read": "read_status"}.get(m.group(1))
        if want is None or where != want:
            errors.append(f"{SESSION}: {where}() uses s_status_view.{m.group(1)} — the view is "
                          "published only by publish_status() and read only by read_status() "
                          "(F161)")
    rd = one_body(code, SIG_READ, f"{SESSION}: read_status()", errors)
    if rd is not None:
        if "s_status_view.read(" not in squash(rd):
            errors.append(f"{SESSION}: read_status() must copy the published view "
                          "(s_status_view.read()) (F161)")
        hit = re.search(r"\b(" + "|".join(LIVE_STATE) + r")\b", rd)
        if hit:
            errors.append(f"{SESSION}: read_status() names {hit.group(1)} — it runs on the httpd "
                          "task and reads only what the main loop published (F161)")
    if one_body(code, SIG_PUBLISH, f"{SESSION}: publish_status()", errors) is None:
        pass
    check_alert_log(code, errors)
    check_locks(code, errors)
    for name, allowed in ALERT_CLEARERS.items():
        for m in re.finditer(r"(?<![\w:.>])" + name + r"\s*\(\s*\)\s*;", code):
            if code[max(0, m.start() - 40):m.start()].rstrip().endswith("void"):
                continue   # its definition's forward declaration
            where = enclosing(code, m.start())
            if where not in allowed:
                errors.append(f"{SESSION}: {where}() calls {name}() — only "
                              f"{' / '.join(a + '()' for a in allowed)} may: Log::clear() relies on "
                              "no append on another task (F197)")


def check_locks(code: str, errors: list[str]) -> None:
    """Rule 7 (F197's review): each snapshot's lock is a real lock — a
    std::mutex on the host, its own portMUX critical section on the device."""
    directives = [(m.start(), squash(m.group(1)))
                  for m in re.finditer(r"^[ \t]*#[ \t]*(\w[^\n]*)$", code, re.M)]
    for name, mux, decl in LOCKS:
        defs = [(m.start(), block_at(code, m.end() - 1))
                for m in re.finditer(r"\bstruct\s+" + name + r"\s*\{", code)]
        if len(defs) != 2:
            errors.append(f"{SESSION}: struct {name} is defined {len(defs)} time(s); expected two, "
                          "the host's and the device's (F197)")
            continue
        (h0, (ha, hb)), (d0, (da, db)) = defs
        if squash(code[ha:hb]) != HOST_LOCK_BODY:
            errors.append(f"{SESSION}: the host {name} must hold a std::mutex m and lock and unlock "
                          "it (m.lock() / m.unlock()) — the two-thread test runs this lock (F197)")
        want = ("{voidlock(){portENTER_CRITICAL(&%s);}voidunlock(){portEXIT_CRITICAL(&%s);}}"
                % (mux, mux))
        if squash(code[da:db]) != want:
            errors.append(f"{SESSION}: the device {name} must take portENTER_CRITICAL(&{mux}) in "
                          f"lock() and portEXIT_CRITICAL(&{mux}) in unlock() (F197)")
        before = [d for d in directives if d[0] < h0]
        between = [d for d in directives if hb <= d[0] < d0]
        after = [d for d in directives if d[0] >= db]
        framed = (before and before[-1][1] == "ifdefCSI_TEST_HOST_BUILD"
                  and [d[1] for d in between] == ["else"]
                  and after and after[0][1].startswith("endif"))
        if not framed:
            errors.append(f"{SESSION}: the host {name} must sit under #ifdef CSI_TEST_HOST_BUILD "
                          "and the device one in its #else (F197)")
        decls = list(re.finditer(r"\bstatic\s+portMUX_TYPE\s+" + mux +
                                 r"\s*=\s*portMUX_INITIALIZER_UNLOCKED\s*;", code))
        if len(decls) != 1 or not between or not (between[0][0] < decls[0].start() < d0):
            errors.append(f"{SESSION}: `static portMUX_TYPE {mux} = portMUX_INITIALIZER_UNLOCKED;` "
                          f"must be declared once, in the #else branch ahead of the device {name} "
                          "(F197)")
        names = len(re.findall(r"\b" + mux + r"\b", code))
        if names != 3:
            errors.append(f"{SESSION}: {mux} is named {names} time(s); expected three (its "
                          f"declaration and {name}'s lock() and unlock()) (F197)")
        if len(re.findall(decl, code)) != 1:
            errors.append(f"{SESSION}: the snapshot {name} guards must be declared once with it as "
                          "its lock (F197)")


SIG_READ_ALERTS = (r"\bsize_t\s+read_alerts\s*\(\s*mesh_alert::Record\s*\*\s*\w+\s*,"
                   r"\s*size_t\s+\w+\s*\)\s*\{")
ALERT_STORE_DECL = re.compile(r"\bstatic\s+AlertEntry\s+s_alert_store\s*\[")


def check_alert_log(code: str, errors: list[str]) -> None:
    """Rule 6 (F197): the alert log is changed only on the main loop's paths
    and read only by read_alerts(), which names no live state."""
    seen: dict[str, list[str]] = {}
    for m in re.finditer(r"\bs_alert_log\s*\.\s*(\w+)\s*\(", code):
        where = enclosing(code, m.start())
        seen.setdefault(m.group(1), []).append(where)
        use = ALERT_LOG_USES.get(m.group(1))
        if use is None or where not in use[0]:
            errors.append(f"{SESSION}: {where}() uses s_alert_log.{m.group(1)}( — the alert log is "
                          "appended to on the receive path, cleared by clear_alerts() / "
                          "reset_alerts(), attached in init() and read only by read_alerts() (F197)")
    for name, (_, count) in ALERT_LOG_USES.items():
        if len(seen.get(name, [])) != count:
            errors.append(f"{SESSION}: s_alert_log.{name}( is called {len(seen.get(name, []))} "
                          f"time(s); expected {count} (F197)")
    if seen.get("clear", []).count("clear_alerts") != 1 or seen.get("clear", []).count("reset_alerts") != 1:
        errors.append(f"{SESSION}: clear_alerts() and reset_alerts() must each clear the alert log "
                      "once (F197)")
    decl = list(ALERT_STORE_DECL.finditer(code))
    if len(decl) != 1:
        errors.append(f"{SESSION}: `static AlertEntry s_alert_store[...]` declared {len(decl)} "
                      "time(s); expected once (F197)")
    for m in re.finditer(r"\bs_alert_store\b", code):
        if any(d.start() <= m.start() < d.end() + len("s_alert_store") for d in decl):
            continue
        where = enclosing(code, m.start())
        if where != "init" or not re.match(r"s_alert_store\s*\)\s*;", code[m.start():]) or \
                not re.search(r"\bs_alert_log\s*\.\s*attach\s*\(\s*$", code[max(0, m.start() - 40):m.start()]):
            errors.append(f"{SESSION}: {where}() names s_alert_store — the records are reached only "
                          "through the log (s_alert_log.attach(s_alert_store) in init()) (F197)")
    rd = one_body(code, SIG_READ_ALERTS, f"{SESSION}: read_alerts()", errors)
    if rd is not None:
        hit = re.search(r"\b(" + "|".join(ALERT_READER_LIVE) + r")\b", rd)
        if hit:
            errors.append(f"{SESSION}: read_alerts() names {hit.group(1)} — it runs on the httpd "
                          "task and reads only the alert log (F197)")
    for name in OLD_ALERT_RING:
        if re.search(r"\b" + name + r"\b", code):
            errors.append(f"{SESSION}: names {name} — the in-place alert ring the httpd task read "
                          "while the main loop wrote it is gone; the history is s_alert_log (F197)")


SIG_SETUP = r"\bvoid\s+setup\s*\(\s*\)\s*\{"


def check_main(main_src: str, others: dict[str, str], errors: list[str]) -> None:
    code = blank(main_src)
    calls = list(re.finditer(r"\bmesh_session::publish_status\s*\(\s*\)\s*;", code))
    setup = function_spans(code, SIG_SETUP)
    if len(calls) != 1 or len(setup) != 1:
        errors.append(f"{MAIN}: mesh_session::publish_status() must be called once, in setup() "
                      f"(found {len(calls)}) — the HTTP server is up before the first loop "
                      "pass (F161)")
    else:
        a, b = setup[0]
        at = calls[0].start()
        last_handler = [m.end() for m in re.finditer(
            r"\bmesh_session::set_opera_create_handler\s*\(", code) if a < m.start() < b]
        if not (a < at < b) or not last_handler or at < last_handler[-1]:
            errors.append(f"{MAIN}: setup()'s mesh_session::publish_status() must follow "
                          "mesh_session::set_opera_create_handler( — the end of the mesh "
                          "restore, so the view it publishes is the restored opera (F161)")
    for path, src in list(others.items()) + [(MAIN, main_src)]:
        code = re.sub(r"\bvoid\s+clear_alerts\s*\(\s*\)\s*;", "", blank(src))   # its declaration
        if re.search(r"\b(clear_alerts|reset_alerts)\s*\(", code):
            errors.append(f"{path}: calls clear_alerts()/reset_alerts() — the alert log is cleared "
                          "only by mesh_session.cpp's main-loop paths (F197)")
    for path, src in others.items():
        code = re.sub(r"\bvoid\s+publish_status\s*\(\s*\)\s*;", "", blank(src))   # its declaration
        if re.search(r"(?<![\w.>])(?:mesh_session::)?publish_status\s*\(\s*\)", code):
            errors.append(f"{path}: calls publish_status() — only mesh_session.cpp's main-loop "
                          "paths and main.cpp's setup() publish the status view (F161)")


def canary_others() -> dict[str, str]:
    out = {}
    skip = {NETWORK, SESSION, MAIN}
    for pattern in ("**/*.cpp", "**/*.h"):
        for path in sorted((REPO / CANARY).glob(pattern)):
            rel = path.relative_to(REPO).as_posix()
            if rel in skip or "/test_" in rel or rel.endswith("/loop_snapshot.h"):
                continue
            out[rel] = path.read_text(encoding="utf-8", errors="replace")
    return out


def check(srcs: dict[str, str], others: dict[str, str]) -> list[str]:
    errors: list[str] = []
    check_network(srcs["network"], errors)
    check_session(srcs["session"], errors)
    check_main(srcs["main"], others, errors)
    return errors


# ── self-test ────────────────────────────────────────────────────────────

Mutation = Callable[[dict], dict]


def raw(key: str, old: str, new: str) -> Mutation:
    def apply(srcs: dict) -> dict:
        s = srcs[key]
        if s.count(old) != 1:
            raise AnchorMissing(f"{key}: {old[:60]!r} x{s.count(old)}")
        out = dict(srcs)
        out[key] = s.replace(old, new)
        return out
    return apply


def chain(*steps: Mutation) -> Mutation:
    def apply(srcs: dict) -> dict:
        for step in steps:
            srcs = step(srcs)
        return srcs
    return apply


def other(path: str, old: str, new: str) -> Mutation:
    def apply(srcs: dict) -> dict:
        others = dict(srcs["others"])
        s = others.get(path, "")
        if s.count(old) != 1:
            raise AnchorMissing(f"{path}: {old[:60]!r} x{s.count(old)}")
        others[path] = s.replace(old, new)
        out = dict(srcs)
        out["others"] = others
        return out
    return apply


STATUS_READ = "  mesh_session::StatusView view;\n  mesh_session::read_status(&view);\n\n  char body[mesh_api::STATUS_JSON_CAP];"
PEERS_READ = ("  mesh_session::StatusView view;\n  mesh_session::read_status(&view);\n\n"
              "  mesh_api::PeerView views[mesh_state::MAX_TRUSTED_PEERS];")
PEERS_JOIN = "  mesh_api::peer_views_from_status(view, pubkeys, count, (uint32_t)millis(), views);"
PROC_END = ("   * just before this call (main.cpp), so the view is this whole pass. */\n"
            "  publish_status();\n}")
PROC_EARLY = "    publish_status();   /* F161: a stopped or disabled session's view too */\n"
DRAIN_PUB = "   * before it. */\n  publish_status();\n  s_slot_result = res;"
DEINIT_PUB = "  publish_status();   /* F161: no view of the session it just wiped */\n"
MAIN_PUB = "    mesh_session::publish_status();\n  } else {"
ALERTS_READ = "  const size_t n = mesh_session::read_alerts(recs, mesh_session::MAX_ALERT_HISTORY);"
ALERT_APPEND = "      (void)s_alert_log.append(e);"
ALERT_CLEAR = ("  /* History only — the lifetime counters keep counting (WAP parity). */\n"
               "  s_alert_log.clear();")
ALERT_RESET = "  s_alert_log.clear();   /* F197: under the log's lock; zeroes the records */"
ALERT_READ = "  const size_t n = s_alert_log.read(held, MAX_ALERT_HISTORY);"
ALERT_ATTACH = "  if (s_alert_log.storage() == nullptr) s_alert_log.attach(s_alert_store);"
HOST_ALERT_LOCK = ("struct AlertLogLock {\n  std::mutex m;\n  void lock()   { m.lock(); }\n"
                   "  void unlock() { m.unlock(); }\n};")
HOST_VIEW_LOCK = ("struct StatusViewLock {\n  std::mutex m;\n  void lock()   { m.lock(); }\n"
                  "  void unlock() { m.unlock(); }\n};")
ALERTS_CLEAR_POST = ("  r.type = mesh_session::RequestType::CLEAR_ALERTS;\n"
                     "  mesh_session::RequestResult res;\n  esp_err_t rc = ESP_OK;\n"
                     "  if (!mesh_call(req, r, &res, &rc)) return rc;")

MUTATIONS: list[tuple[str, Mutation]] = [
    ("status handler reads pairing_seq live",
     raw("network", STATUS_READ, STATUS_READ + "\n  (void)mesh_session::pairing_seq();")),
    ("status handler reads the opera name live",
     raw("network", STATUS_READ, STATUS_READ + "\n  char n[33]; mesh_session::get_opera_name(n, sizeof(n));")),
    ("status handler reads get_alerts (allowed only for the alerts route)",
     raw("network", STATUS_READ, STATUS_READ + "\n  (void)mesh_session::get_alerts(nullptr, 0);")),
    ("peers handler reads the peer links live",
     raw("network", PEERS_JOIN, PEERS_JOIN + "\n  mesh_session::PeerLink l[8]; (void)mesh_session::get_peer_links(l, 8);")),
    ("peers handler reads the transport table",
     raw("network", PEERS_JOIN, PEERS_JOIN + "\n  mesh_transport::Peer p[8]; (void)mesh_transport::list_peers(p, 8);")),
    ("another handler reads is_enabled live",
     raw("network", "  r.type = mesh_session::RequestType::LEAVE;",
         "  r.type = mesh_session::RequestType::LEAVE;\n  (void)mesh_session::is_enabled();")),
    ("a handler publishes the view",
     raw("network", STATUS_READ, STATUS_READ + "\n  mesh_session::publish_status();")),
    ("status handler reads the view twice",
     raw("network", STATUS_READ, STATUS_READ + "\n  mesh_session::read_status(&view);")),
    ("status handler never reads the view",
     raw("network", STATUS_READ, STATUS_READ.replace("  mesh_session::read_status(&view);\n", "  memset(&view, 0, sizeof(view));\n"))),
    ("peers handler never reads the view",
     raw("network", PEERS_READ, PEERS_READ.replace("  mesh_session::read_status(&view);\n", "  memset(&view, 0, sizeof(view));\n"))),
    ("peers handler joins nothing",
     raw("network", PEERS_JOIN, "  memset(views, 0, sizeof(views));")),
    ("process() has no publish at its end",
     raw("session", PROC_END, "   * just before this call (main.cpp), so the view is this whole pass. */\n}")),
    ("process() returns early without a publish",
     raw("session", PROC_EARLY, "")),
    ("drain_request() publishes nothing",
     raw("session", DRAIN_PUB, "   * before it. */\n  s_slot_result = res;")),
    ("drain_request() publishes after the result is posted",
     raw("session", "  if (!slot_cas(SLOT_RUNNING, SLOT_DONE)) {\n    /* ABANDONED",
         "  bool posted = slot_cas(SLOT_RUNNING, SLOT_DONE);\n  publish_status();\n  if (!posted) {\n    /* ABANDONED")),
    ("drain_request() publishes before the request runs",
     raw("session", "  execute_request(req, now_ms, &res);",
         "  publish_status();\n  execute_request(req, now_ms, &res);")),
    ("deinit() publishes nothing",
     raw("session", DEINIT_PUB, "")),
    ("a setter publishes mid-pass",
     raw("session", "  strncpy(s_opera_name, name, sizeof(s_opera_name) - 1);",
         "  strncpy(s_opera_name, name, sizeof(s_opera_name) - 1);\n  publish_status();")),
    ("read_status() builds from the live state",
     raw("session", "  if (s_status_view.read(out)) return;", "  build_status_view(out); return;")),
    ("read_status() reads the live name",
     raw("session", "  out->enabled             = true;", "  out->enabled             = s_opera_name[0] != 0;")),
    ("the view is published outside publish_status()",
     raw("session", "  v->member_count = (uint32_t)n_links;\n}",
         "  v->member_count = (uint32_t)n_links;\n  s_status_view.publish(*v);\n}")),
    ("main.cpp's setup publish removed",
     raw("main", MAIN_PUB, "  } else {")),
    ("main.cpp publishes before the restore is done",
     chain(raw("main", MAIN_PUB, "  } else {"),
           raw("main", "    mesh_session::set_paired_callback(&on_pairing_succeeded);",
               "    mesh_session::publish_status();\n"
               "    mesh_session::set_paired_callback(&on_pairing_succeeded);"))),
    ("main.cpp publishes from loop() as well",
     raw("main", "  mesh_session::process((uint32_t)millis());\n",
         "  mesh_session::process((uint32_t)millis());\n  mesh_session::publish_status();\n")),
    ("alerts handler reads the ring in place (get_alerts)",
     raw("network", ALERTS_READ,
         "  const size_t n = mesh_session::get_alerts(recs, mesh_session::MAX_ALERT_HISTORY);")),
    ("alerts handler reads the log twice",
     raw("network", ALERTS_READ, ALERTS_READ + "\n  (void)mesh_session::read_alerts(recs, 1);")),
    ("alerts handler never reads the log",
     raw("network", ALERTS_READ, "  const size_t n = 0;")),
    ("alerts handler reads the alert counter live",
     raw("network", ALERTS_READ, ALERTS_READ + "\n  (void)mesh_session::alerts_received();")),
    ("the receive path writes the records in place",
     raw("session", ALERT_APPEND, "      s_alert_store[0] = e;")),
    ("the log appended outside the receive path",
     raw("session", ALERT_CLEAR, ALERT_CLEAR + "\n  AlertEntry z; memset(&z, 0, sizeof(z)); (void)s_alert_log.append(z);")),
    ("clear_alerts() wipes the records in place",
     raw("session", ALERT_CLEAR, ALERT_CLEAR.replace("  s_alert_log.clear();",
                                                     "  memset(s_alert_store, 0, sizeof(s_alert_store));"))),
    ("reset_alerts() keeps the history",
     raw("session", ALERT_RESET, "")),
    ("read_alerts() copies the records in place",
     raw("session", ALERT_READ, "  memcpy(held, s_alert_store, sizeof(held));\n"
                                "  const size_t n = s_alert_log.count();")),
    ("read_alerts() names the main loop's counter",
     raw("session", ALERT_READ, ALERT_READ + "\n  (void)s_alert_seq;")),
    ("read_alerts() reads the log twice",
     raw("session", ALERT_READ, ALERT_READ + "\n  (void)s_alert_log.read(held, 1);")),
    ("the log attached outside init()",
     raw("session", ALERT_CLEAR, ALERT_CLEAR + "\n  s_alert_log.attach(s_alert_store);")),
    ("init() attaches nothing",
     raw("session", ALERT_ATTACH, "")),
    ("the in-place ring comes back",
     raw("session", "static uint32_t   s_alert_seq = 0;",
         "static size_t s_alert_head = 0;\nstatic uint32_t   s_alert_seq = 0;")),
    ("a get_alerts() reader comes back",
     raw("session", "void clear_alerts() {",
         "size_t get_alerts(mesh_alert::Record* out, size_t cap) { return read_alerts(out, cap); }\n\n"
         "void clear_alerts() {")),
    ("the device alert lock is a no-op",
     raw("session", "  void lock()   { portENTER_CRITICAL(&s_alert_log_mux); }", "  void lock()   { }")),
    ("the device alert lock never unlocks",
     raw("session", "  void unlock() { portEXIT_CRITICAL(&s_alert_log_mux); }", "  void unlock() { }")),
    ("the device alert lock takes the status view's portMUX",
     raw("session", "  void lock()   { portENTER_CRITICAL(&s_alert_log_mux); }\n"
                    "  void unlock() { portEXIT_CRITICAL(&s_alert_log_mux); }",
         "  void lock()   { portENTER_CRITICAL(&s_status_view_mux); }\n"
         "  void unlock() { portEXIT_CRITICAL(&s_status_view_mux); }")),
    ("the host alert lock is a no-op",
     raw("session", HOST_ALERT_LOCK, HOST_ALERT_LOCK.replace("{ m.lock(); }", "{ }")
                                                    .replace("{ m.unlock(); }", "{ }"))),
    ("the host alert lock holds no mutex",
     raw("session", HOST_ALERT_LOCK, HOST_ALERT_LOCK.replace("  std::mutex m;\n", "  int m = 0;\n")
                                                    .replace("{ m.lock(); }", "{ ++m; }")
                                                    .replace("{ m.unlock(); }", "{ --m; }"))),
    ("the device build gets the host's alert lock",
     raw("session", "#ifdef CSI_TEST_HOST_BUILD\n" + HOST_ALERT_LOCK,
         "#ifndef CSI_TEST_HOST_BUILD\n" + HOST_ALERT_LOCK)),
    ("the alert log locks with a no-op struct",
     raw("session", "static loop_snapshot::Log<AlertEntry, MAX_ALERT_HISTORY, AlertLogLock> s_alert_log;",
         "struct NoLock { void lock() {} void unlock() {} };\n"
         "static loop_snapshot::Log<AlertEntry, MAX_ALERT_HISTORY, NoLock> s_alert_log;")),
    ("the alert portMUX taken somewhere else too",
     raw("session", ALERT_READ, "  portENTER_CRITICAL(&s_alert_log_mux);\n" + ALERT_READ)),
    ("the device status view lock is a no-op",
     raw("session", "  void lock()   { portENTER_CRITICAL(&s_status_view_mux); }", "  void lock()   { }")),
    ("the host status view lock is a no-op",
     raw("session", HOST_VIEW_LOCK, HOST_VIEW_LOCK.replace("{ m.lock(); }", "{ }"))),
    ("the DELETE handler clears on the httpd task",
     raw("network", ALERTS_CLEAR_POST, "  mesh_session::clear_alerts();\n"
                                       "  mesh_session::RequestResult res;\n  esp_err_t rc = ESP_OK;\n"
                                       "  (void)res; (void)rc;")),
    ("the DELETE handler posts nothing",
     raw("network", ALERTS_CLEAR_POST, ALERTS_CLEAR_POST.replace(
         "  if (!mesh_call(req, r, &res, &rc)) return rc;", "  (void)res; (void)rc;"))),
    ("another handler posts CLEAR_ALERTS",
     raw("network", "  r.type = mesh_session::RequestType::LEAVE;",
         "  r.type = mesh_session::RequestType::CLEAR_ALERTS;")),
    ("the poison pragma no longer names clear_alerts",
     raw("network", "#pragma GCC poison leave_opera set_opera_name set_enabled clear_alerts remove_peer",
         "#pragma GCC poison leave_opera set_opera_name set_enabled remove_peer")),
    ("clear_alerts() called from read_alerts() (the httpd task)",
     raw("session", ALERT_READ, "  clear_alerts();\n" + ALERT_READ)),
    ("reset_alerts() called from process()",
     raw("session", PROC_END, PROC_END.replace("  publish_status();\n}", "  reset_alerts();\n  publish_status();\n}"))),
    ("main.cpp clears the alert log",
     raw("main", "  mesh_session::process((uint32_t)millis());\n",
         "  mesh_session::process((uint32_t)millis());\n  mesh_session::clear_alerts();\n")),
    ("another canary file publishes",
     other("firmware/canary/src/csi_modules_integration.cpp",
           "  mesh_session::set_beacon_event_handler(&on_peer_beacon_event_inbound);",
           "  mesh_session::set_beacon_event_handler(&on_peer_beacon_event_inbound);\n  mesh_session::publish_status();")),
]


def self_test(srcs: dict[str, str], others: dict[str, str]) -> list[str]:
    problems = []
    base = dict(srcs)
    base["others"] = others
    for name, mutate in MUTATIONS:
        try:
            m = mutate(base)
        except AnchorMissing as missing:
            problems.append(f"self-test: mutation '{name}' no longer applies (anchor {missing}) — "
                            "the source changed shape; update this guard's mutations with it")
            continue
        if m == base:
            problems.append(f"self-test: mutation '{name}' changed nothing")
        elif not check({k: m[k] for k in ("network", "session", "main")}, m["others"]):
            problems.append(f"self-test: the check did not bite on mutation '{name}'")
    return problems


def main() -> int:
    srcs = {
        "network": (REPO / NETWORK).read_text(encoding="utf-8"),
        "session": (REPO / SESSION).read_text(encoding="utf-8"),
        "main": (REPO / MAIN).read_text(encoding="utf-8"),
    }
    others = canary_others()
    errors = check(srcs, others)
    for err in errors:
        print(f"::error::{err}")
    problems = self_test(srcs, others)
    for problem in problems:
        print(f"::error::{problem}")
    if errors or problems:
        return 1
    print(f"canary mesh status routes hold: GET /api/mesh and /peers read only the view the main "
          f"loop publishes (each pass, its early return, each request before its result, "
          f"deinit, and setup's restore), GET /api/mesh/alerts reads only the log the main loop "
          f"changes under its lock, both locks are real (a std::mutex on the host, each its "
          f"own portMUX on the device), only the main loop clears the alert log, no HTTP "
          f"handler reads the session's live state "
          f"({len(MUTATIONS)} mutations refused).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
