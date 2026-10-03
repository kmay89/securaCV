#!/usr/bin/env python3
"""The canary's pairing routes hand the web UI the pairing's number and
outcome (sweep F133) — a pin on
firmware/canary/lib/securacv_network/src/securacv_network.cpp.

GET /api/mesh carries pairing_seq, pairing_result and pairing_fail_reason,
and POST pair/start and pair/join answer the pairing_seq they started, so
the pairing screen's poll (securacv_webui.cpp: startPairingPolling) can
tell a failed pairing from a finished one, and its own pairing from a later
one. Since F161 the body is built by
mesh_api::build_mesh_status_json_from_view from the view the main loop
publishes (mesh_session::read_status), which carries the numbers; the
builder fills the PairingReport from that view and hands it to
build_mesh_status_json. Both are host-tested (test_mesh_session.cpp:
test_build_mesh_status_json_reports_the_last_pairing,
test_get_mesh_tells_each_pairing_outcome, whose status_body() makes the
handler's two calls); the page's half is
firmware/tests_host/test_canary_mesh_pairing_poll.test.js. The handlers
themselves are compiled only by CI's [env:full] build and run by nothing on
a host, so these cases hold the wiring status_body() mirrors:

* handle_mesh_status fills a mesh_session::StatusView with
  mesh_session::read_status(&view) and then passes that view to the one
  mesh_api::build_mesh_status_json_from_view call, into a buffer of
  mesh_api::STATUS_JSON_CAP bytes (the widest body is past the 512 it had).
  securacv_network.cpp calls the field-by-field build_mesh_status_json
  nowhere: until F161 the handler did, filling the report itself, and a
  report dropped there (passing nullptr compiles) leaves the body without
  the three fields, so the page claims neither a success nor a failure:
  the old blind spot. firmware/scripts/check_canary_mesh_status.py holds
  the rest of F161 (no handler reads the session's live state; where the
  main loop publishes).
* handle_mesh_pair_start and handle_mesh_pair_join each set
  `doc["pairing_seq"] = res.pairing_seq;` once, before the answer is
  serialized. Without it the page has no number and cannot tell its own
  pairing's outcome from a later pairing's.
* GET /api/mesh is served by handle_mesh_status, and the two POST routes by
  those two handlers.

The statements are pinned by their exact token sequence (whitespace and
comments aside), and each rule is shown to fail on a mutation of the real
file, as test_canary_mesh_pair_bind_wiring.py does for main.cpp.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
NETWORK = REPO / "firmware/canary/lib/securacv_network/src/securacv_network.cpp"

STATUS_FN = "handle_mesh_status"
START_FN = "handle_mesh_pair_start"
JOIN_FN = "handle_mesh_pair_join"
BUILD = "mesh_api::build_mesh_status_json_from_view"
FIELD_BUILD = "mesh_api::build_mesh_status_json"


def _strip_comments(src: str) -> str:
    """Blank out comments, keeping string literals and offsets (the routes
    and `doc["pairing_seq"]` are strings)."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif src.startswith('R"', i):   # a raw string (the web pages): skip it whole
            m = re.match(r'R"([^(\s]*)\(', src[i:])
            if m is None:
                i += 1
                continue
            end = src.find(")" + m.group(1) + '"', i + m.end())
            i = n if end < 0 else end + len(m.group(1)) + 2
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            i = j + 1
        else:
            i += 1
    return "".join(out)


def _block_at(code: str, brace: int) -> str:
    """The brace-matched block whose `{` is at `brace`."""
    depth = 0
    for i in range(brace, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[brace : i + 1]
    return code[brace:]


def _handler(code: str, name: str) -> str | None:
    """The body of `static esp_err_t name(httpd_req_t* req) {`, or None."""
    m = re.search(r"static\s+esp_err_t\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", code)
    return None if m is None else _block_at(code, m.end() - 1)


_TOKEN = re.compile(r'"(?:[^"\\]|\\.)*"|\w+|::|->|\+\+|&&|\|\||[!=<>]=|\S')


def _seq(snippet: str) -> str:
    """A regex for `snippet`'s exact token sequence, whitespace-insensitive."""
    toks = _TOKEN.findall(snippet)
    out = []
    for i, t in enumerate(toks):
        if i:
            word = re.match(r"\w", toks[i - 1][-1]) and re.match(r"\w", t[0])
            out.append(r"\s+" if word else r"\s*")
        out.append(re.escape(t))
    return "".join(out)


def _args(code: str, open_paren: int) -> list[str]:
    """The top-level arguments of the call whose `(` is at `open_paren`."""
    depth, args, cur = 0, [], []
    for i in range(open_paren, len(code)):
        c = code[i]
        if c in "([{":
            depth += 1
            if depth == 1:
                continue
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append("".join(cur))
                break
        elif c == "," and depth == 1:
            args.append("".join(cur))
            cur = []
            continue
        cur.append(c)
    return [" ".join(a.split()) for a in args]


VIEW = _seq("mesh_session::StatusView view; mesh_session::read_status(&view);")
BUFFER = _seq("char body[mesh_api::STATUS_JSON_CAP];")
SEQ_FIELD = _seq('doc["pairing_seq"] = res.pairing_seq;')
SERIALIZE = _seq("serializeJson(doc, response);")
ROUTES = {
    STATUS_FN: _seq('.uri = "/api/mesh", .method = HTTP_GET, .handler = handle_mesh_status }'),
    START_FN: _seq('.uri = "/api/mesh/pair/start", .method = HTTP_POST, '
                   '.handler = handle_mesh_pair_start }'),
    JOIN_FN: _seq('.uri = "/api/mesh/pair/join", .method = HTTP_POST, '
                  '.handler = handle_mesh_pair_join }'),
}


def _status_problems(code: str) -> list[str]:
    body = _handler(code, STATUS_FN)
    if body is None:
        return [f"{STATUS_FN} is missing"]
    problems: list[str] = []
    view = re.search(VIEW, body)
    if view is None:
        problems.append(f"{STATUS_FN} does not fill its view from the main loop's published "
                        f"copy (`mesh_session::StatusView view; "
                        f"mesh_session::read_status(&view);`)")
    if not re.search(BUFFER, body):
        problems.append(f"{STATUS_FN}'s buffer is not `char body[mesh_api::STATUS_JSON_CAP];`")
    fields = re.findall(re.escape(FIELD_BUILD) + r"\s*\(", code)
    if fields:
        problems.append(f"{FIELD_BUILD} is called {len(fields)} times in securacv_network.cpp; "
                        f"the status body is built from the published view "
                        f"({BUILD}), which carries the last pairing's report")
    calls = [m for m in re.finditer(re.escape(BUILD) + r"\s*\(", code)]
    if len(calls) != 1:
        problems.append(f"{BUILD} is called {len(calls)} times in securacv_network.cpp; "
                        f"expected once, from {STATUS_FN}")
        return problems
    in_status = re.search(re.escape(BUILD) + r"\s*\(", body)
    if in_status is None:
        problems.append(f"{BUILD} is not called from {STATUS_FN}")
        return problems
    args = _args(body, in_status.end() - 1)
    if args != ["body", "sizeof(body)", "view"]:
        problems.append(f"{STATUS_FN} does not pass the view it read to {BUILD} "
                        f"(`body, sizeof(body), view`; got {args}): the body loses the "
                        f"view's pairing_seq, pairing_result and pairing_fail_reason")
    elif view is not None and in_status.start() < view.end():
        problems.append(f"{STATUS_FN} builds the body before it reads the view")
    return problems


def _seq_answer_problems(code: str, name: str) -> list[str]:
    body = _handler(code, name)
    if body is None:
        return [f"{name} is missing"]
    sets = list(re.finditer(SEQ_FIELD, body))
    if len(sets) != 1:
        return [f"{name} sets `doc[\"pairing_seq\"] = res.pairing_seq;` {len(sets)} times; "
                f"expected once: the page cannot tell its own pairing from a later one"]
    ser = re.search(SERIALIZE, body)
    if ser is None or ser.start() < sets[0].end():
        return [f"{name} serializes its answer before it sets pairing_seq"]
    return []


def check(src: str) -> list[str]:
    """Every rule the wiring breaks, as a message; empty when it holds."""
    code = _strip_comments(src)
    problems = _status_problems(code)
    problems += _seq_answer_problems(code, START_FN)
    problems += _seq_answer_problems(code, JOIN_FN)
    for fn, route in ROUTES.items():
        if len(re.findall(route, code)) != 1:
            problems.append(f"{fn} is not registered once for its route")
    return problems


class MeshStatusWiring(unittest.TestCase):
    def setUp(self) -> None:
        self.src = NETWORK.read_text(encoding="utf-8")

    def mutate(self, old: str, new: str) -> str:
        self.assertEqual(self.src.count(old), 1, f"mutation anchor not unique: {old!r}")
        return self.src.replace(old, new)

    def test_the_real_handlers_hold(self) -> None:
        self.assertEqual(check(self.src), [])

    def test_the_field_by_field_builder_back_fails(self) -> None:
        old = ("  if (!mesh_api::build_mesh_status_json_from_view(body, sizeof(body), view)) {")
        new = ("  if (!mesh_api::build_mesh_status_json(body, sizeof(body), view.enabled, "
               "view.has_opera, view.opera_id, view.opera_name, view.pairing_state, "
               "view.peers_total, view.peers_online, view.alerts_received, "
               "view.pairing_code, nullptr)) {")
        problems = check(self.mutate(old, new))
        self.assertTrue(any("is called 1 times in securacv_network.cpp; the status body" in p
                            for p in problems), problems)

    def test_another_view_passed_fails(self) -> None:
        src = self.mutate("build_mesh_status_json_from_view(body, sizeof(body), view)) {",
                          "build_mesh_status_json_from_view(body, sizeof(body), "
                          "mesh_session::StatusView{})) {")
        self.assertTrue(any("does not pass the view it read" in p for p in check(src)))

    def test_the_view_not_read_fails(self) -> None:
        src = self.mutate("  mesh_session::read_status(&view);\n\n  char body[mesh_api::STATUS_JSON_CAP];",
                          "  memset(&view, 0, sizeof(view));\n\n  char body[mesh_api::STATUS_JSON_CAP];")
        self.assertTrue(any("does not fill its view" in p for p in check(src)))

    def test_the_body_built_before_the_read_fails(self) -> None:
        old = ("  mesh_session::StatusView view;\n"
               "  mesh_session::read_status(&view);\n\n"
               "  char body[mesh_api::STATUS_JSON_CAP];\n"
               "  if (!mesh_api::build_mesh_status_json_from_view(body, sizeof(body), view)) {\n"
               "    return http_send_error(req, 500, \"encode_failed\");\n"
               "  }\n")
        new = ("  mesh_session::StatusView view;\n"
               "  char body[mesh_api::STATUS_JSON_CAP];\n"
               "  if (!mesh_api::build_mesh_status_json_from_view(body, sizeof(body), view)) {\n"
               "    return http_send_error(req, 500, \"encode_failed\");\n"
               "  }\n"
               "  mesh_session::read_status(&view);\n")
        problems = check(self.mutate(old, new))
        self.assertTrue(any("builds the body before it reads the view" in p
                            or "does not fill its view" in p for p in problems), problems)

    def test_the_old_buffer_fails(self) -> None:
        body = re.search(r"static esp_err_t handle_mesh_status\(", self.src)
        self.assertIsNotNone(body)
        assert body is not None
        old = "  char body[mesh_api::STATUS_JSON_CAP];\n"
        at = self.src.index(old, body.start())
        src = self.src[:at] + "  char body[512];\n" + self.src[at + len(old):]
        self.assertTrue(any("buffer is not" in p for p in check(src)))

    def test_the_seq_left_out_of_either_answer_fails(self) -> None:
        for anchor, fn in [
            ('  doc["pairing_seq"] = res.pairing_seq;   // F133: GET /api/mesh reports', START_FN),
            ('  doc["pairing_seq"] = res.pairing_seq;   // F133\n', JOIN_FN),
        ]:
            with self.subTest(fn=fn):
                src = self.mutate(anchor, "  // " + anchor.lstrip())
                self.assertTrue(any(p.startswith(f"{fn} sets") for p in check(src)), check(src))

    def test_a_constant_seq_fails(self) -> None:
        src = self.mutate('  doc["pairing_seq"] = res.pairing_seq;   // F133\n',
                          '  doc["pairing_seq"] = 0;   // F133\n')
        self.assertTrue(any(p.startswith(f"{JOIN_FN} sets") for p in check(src)))

    def test_the_seq_set_after_the_answer_is_serialized_fails(self) -> None:
        old = ('  doc["pairing_seq"] = res.pairing_seq;   // F133\n'
               '  String response;\n'
               '  serializeJson(doc, response);\n')
        new = ('  String response;\n'
               '  serializeJson(doc, response);\n'
               '  doc["pairing_seq"] = res.pairing_seq;   // F133\n')
        self.assertTrue(any("serializes its answer before" in p
                            for p in check(self.mutate(old, new))))

    def test_the_status_route_moved_fails(self) -> None:
        src = self.mutate('.uri = "/api/mesh", .method = HTTP_GET, .handler = handle_mesh_status }',
                          '.uri = "/api/mesh/x", .method = HTTP_GET, .handler = handle_mesh_status }')
        self.assertTrue(any(f"{STATUS_FN} is not registered" in p for p in check(src)))


if __name__ == "__main__":
    unittest.main()
