#!/usr/bin/env python3
"""Hold the PlatformIO canary's mesh status routes to the view the main loop
publishes (sweep F161).

`GET /api/mesh` and `/api/mesh/peers` (`handle_mesh_status`,
`handle_mesh_peers` in `securacv_network.cpp`) run on esp_http_server's
task. Until F161 they read `mesh_session`'s state in place (the pairing
state and code, F133's pairing number, outcome and reason, the opera name,
the peer links and the transport table) while the main loop's
`mesh_session::process()` wrote it, so one body could mix two passes. Now
the main loop builds a `StatusView` and publishes it
(`mesh_session::publish_status()`, through `loop_snapshot.h`), and the two
routes copy it (`mesh_session::read_status()`).

`test_mesh_session.cpp` runs the session, the view and the JSON on the
host: the read is the last published pass, a read right after a request's
answer shows the request, a stopped session still publishes, deinit()
publishes, the code is kept only while shown, the peer join, and a
two-thread run. No host test compiles `securacv_network.cpp` or
`main.cpp` (only CI's `[env:full]` build does), and none can see which task
a call runs on; this check holds the sources to the shape those tests
assume.

## The rules

1. No HTTP handler in `securacv_network.cpp` (a `static esp_err_t
   name(httpd_req_t* req)` function) names a live reader of the session's
   or the transport's state (`LIVE_READERS` below: `pairing_seq`,
   `pairing_state`, `get_opera_name`, `get_peer_links`,
   `mesh_transport::list_peers` and the rest). One allowance, named in
   `ALLOWED`: `handle_mesh_alerts` still reads `mesh_session::get_alerts`
   (F161 covered the two status routes; the alerts route is handed up).
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
ALLOWED = {("mesh_session", "get_alerts", "handle_mesh_alerts")}
STATUS_HANDLERS = ("handle_mesh_status", "handle_mesh_peers")

# Rule 4: names read_status() must not touch.
LIVE_STATE = (
    "s_ctx", "s_trusted_peers", "s_opera_name", "s_opera_id", "s_opera_id_set",
    "s_enabled", "s_running", "s_pairing_seq", "s_alerts_received", "build_status_view",
    "get_peer_links", "online_peer_count", "trusted_peer_count", "list_peers",
)

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
                if (ns, m.group(1), name) in ALLOWED:
                    continue
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
    peers = bodies.get("handle_mesh_peers")
    if peers is not None:
        joins = re.findall(r"\bmesh_api::peer_views_from_status\s*\(\s*view\s*,", peers)
        if len(joins) != 1:
            errors.append(f"{NETWORK}: handle_mesh_peers() must join its rows against the view "
                          f"once (`mesh_api::peer_views_from_status(view, ...)`, found "
                          f"{len(joins)}) (F161)")


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
          f"deinit, and setup's restore), no HTTP handler reads the session's live state "
          f"(the alerts route excepted) ({len(MUTATIONS)} mutations refused).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
