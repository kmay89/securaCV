#!/usr/bin/env python3
"""Route-security conformance check for the canary (PlatformIO) firmware.

Every HTTP route firmware/canary registers must be credential-gated — the
device bearer token (auth_gate / auth_check), or the physical BOOT-tap
provisioning gate — unless it is on the explicit PUBLIC allowlist below
with a written reason. This pins the fail-closed property statically, so a
new endpoint cannot silently widen the unauthenticated surface. It is the
canary-tree sibling of
firmware/projects/canary-wap/tests_host/check_route_security.py (same
string-aware brace matcher, same allowlist-cannot-rot rule), and it closes
the standing ESP32S3_OPTIMIZATION_ROADMAP.md §3.8 P2 sweep "no new handler
is added without auth_gate".

The canary registers every route in ONE place,
lib/securacv_network/src/securacv_network.cpp's registerHttpHandlers(),
as `httpd_uri_t x = {...}; register_route(...)`. The captive-portal probes
are registered from a string array in a loop (`.uri = p`), so the parser
reads that array too — otherwise the six probe routes would be invisible
here and an unauthenticated route could hide in the same loop.

Beyond "every route reaches a gate", six wiring rules the marker scan alone
cannot see (each added after a review mutation, or a review finding, passed
every other gate), and the premise the last one stands on:

  * a gate marker counts only where its RESULT decides something — inside an
    `if (...)` / `while (...)` / `switch (...)` condition or a `return`
    expression, or initializing a `const` local that one of those later
    reads. A bare `auth_gate(req);`, `(void)bearer_present_and_valid(req);`
    or a local nobody tests ignores the answer and is not a gate, and a
    `return` inside a lambda body is the lambda deciding, not the handler.
    auth_bearer_grant() answers one of four (NONE / OK / WRONG / HELD), so
    it counts only where its call is compared `== AuthManager::BearerResult::OK`
    (`!= ...::WRONG` would serve a request with no header or a held one);
  * every `send_html_with_token(...)` call passes `page_token_inject(req)` as
    its third argument — never a literal `true`/`false` or anything else —
    and page_token_inject goes through provisioning_gate::page_token_decide
    and the take hook, never a peek (a peek let one BOOT tap unlock every
    home-LAN page load for 30 s);
  * inside registerHttpHandlers(server) every registration uses the
    `server` parameter: the table goes on whichever server is primary (TLS
    or plain), and on FEATURE_HTTPS builds m_http_server is still nullptr
    there, so `register_route(m_http_server, ...)` drops the route with only
    a serial line (the shape two sibling branches add routes in);
  * no function registers more routes than the max_uri_handlers budget it
    sets (registerHttpHandlers is held to kRouteTableSlots, with and without
    the mesh block; every #if branch is counted, i.e. the worst case);
  * the Host comes first on every path that can hand out the bearer token or
    spend the BOOT tap, and its answer is used (check_host_first).
    host_is_foreign(req) — the Host must name this device unless the request
    came over the SoftAP — is asked before any bearer is read, any tap is
    taken or any token is emitted, and it counts only in two shapes: an
    `if (host_is_foreign(req))` whose branch returns (the refusal), or the
    FIRST argument of a host-tested decision (receipt_decide,
    page_token_decide), directly or through a `const bool` local set from
    it. Any other first argument to those decisions fails. A gated route
    gets the Host from auth_gate (whose own body is held to that order) or
    asks it itself; the provisioning receipt, gated by a bearer or the tap,
    binds receipt_decide's verdict once, sends the receipt only under an
    explicit SERVE_BEARER / SERVE_TAP test and ends in the Host refusal, so
    REFUSE_HOST, a dropped branch or a verdict added later refuses;
    page_token_inject hands the Host to page_token_decide before any grant
    or tap; send_html_with_token streams the token only in
    `(<verdict> == PageToken::INJECT) ? auth_get_token() : ""`. This rule
    checks shapes, not meaning: the decisions' order is proved by
    firmware/tests_host/test_provisioning_gate.cpp, and this rule proves the
    glue hands them the Host and obeys their verdict (added with the
    provisioning receipt's own Host check; the four rules above were green
    without it);
  * every answer that carries a credential holds to a bearer-only grant, on
    the flagship's own gates (check_bearer_only_answers, sweep F338; the
    canary-wap checker's F328 rule, ported to what this tree has). The
    three credentials are the bearer token, the SoftAP password and the TLS
    pin — public in every TLS handshake, held because the apps trust it as
    the pin. Each is read where SECRET_READS spells it (its store, its
    accessor, its deriver) and carried up the call graph to the route
    handlers, the carry stopping at a device-side use and at a comparer.
    Every reader is named: an accessor in its home (SECRET_ACCESSORS), an
    emitter carrying exactly what TOKEN_EMITTERS says, a comparer reading
    only the token, a device-side use whose reading statements are exactly
    its SECRET_LOCAL_USES row (statement pins, keyed by file and qualified
    function; a mint's statement runs through its refusal, so the pin on
    main.cpp's `derive_ap_password(device.privkey, ...)` holds the call, its
    refusal and the halt), or a SECRET_ANSWERS handler; outside every
    function only OUTSIDE_FUNCTION_LINES may name one. Every registered
    handler that carries one is a SECRET_ANSWERS row with exactly what it
    carries and its grant: 'bearer' (`if (!auth_gate(req)) return ESP_OK;`
    at its own top level, before every answer site), 'receipt'
    (receipt_decide handed exactly RECEIPT_GRANTS, every answer under a
    SERVE_* test, rule 6 holding the dispatch) or 'page' (only through
    send_html_with_token(..., page_token_inject(req)) or another page
    handler; a public route may carry only this way). The glue every grant
    runs through is pinned whole (GLUE_DEFS: host_is_foreign, auth_gate,
    bearer_present_and_valid, from_ap_subnet, page_token_inject with its four
    inputs and F341's auth_handed_token on the tap that served,
    provisioning_gate_take, the hook setter, main.cpp's take hook, and the
    lockout glue F340 and F341 left: AuthManager::check, grant, compare and
    handedToken and the auth_check, auth_bearer_grant and auth_handed_token
    wrappers). The gate primitives are called only where the census says
    (GATE_CALL_SITES: every auth_gate call is the whole refusal statement,
    auth_handed_token runs on the receipt only under a served verdict, only
    the three wrappers reach the AuthManager, constantTimeCompare runs only
    in compare), and every AUTH_MARKERS and PRE_HOST_STEPS name is a census
    row, so a deleted gate's name cannot linger in either list;
  * the premise the last rule stands on, checked (no_session_problems): the
    flagship has no session. No cookie is read or set, the request headers
    read are `Authorization` (the auth lib) and `Host` (the network lib)
    only, a response header's name is a literal and never `Set-Cookie`, and
    a URL query is read only by the witness log's paging. A cookie, a
    session header, a query credential or a lockout keyed on a request
    header fails here, pointing at canary-wap's any-session class
    (ANY_SESSION_GATES, F328): CONSOLIDATION.md's Phase 6 cv_session port
    must bring that rule with it.

(The lambda clause of the first rule is why a receipt_decide(...) whose
verdict nobody reads fails it, although its bearer and tap lambdas return
the gates' answers.)

The parser names what it reads: one head parser (function_defs) names a
constructor for its class, its initializer list included, and a member as
`Class::name`; a lambda names no function, so what it calls is its
enclosing function's.

What a text check cannot see, stated so nobody leans on it: names are
matched by spelling, so a copy of a credential under another name, or a
value passed on as a parameter (begin()'s ap_password, the
announce_setup_network() it prints through), is not followed; a call by
bare name reaches every definition of that name (Preferences' begin()
reaches ScvNetworkManager::begin), which errs toward failing; the shared
firmware/common/network/auth_backoff.h is outside ROOTS and its arithmetic
is test_auth_backoff.cpp's; and a pinned body or statement means an edit to
that glue updates its row in the same review (the failure prints the row as
it now is, so the update is mechanical).

Run from the repo root (CI: firmware.yml job "Mesh + Scout Host Tests",
step "Check canary (PIO) route security", beside the WAP's audit):
    python3 firmware/canary/scripts/check_route_security.py
"""

import functools
import re
import sys
from pathlib import Path

# Every .cpp/.h under the canary tree's libs and src. Only the network lib
# registers routes today; scanning the whole tree means a second lib that
# starts registering routes is audited without editing this script.
ROOTS = (Path("firmware/canary/lib"), Path("firmware/canary/src"))

# Substrings whose presence in a handler body counts as a credential gate.
# Each is a real call that enforces access:
AUTH_MARKERS = (
    "auth_gate(",               # bearer, fail-closed 503 when unprovisioned
    "auth_check(",              # AuthManager bearer check (401/403/429), per source address
    "auth_bearer_grant(",       # the accounted grant (NONE/OK/WRONG/HELD), sends nothing;
                                # the caller tests for OK and answers the rest (F340):
                                # it counts only compared `== AuthManager::BearerResult::OK`
    "bearer_present_and_valid(",  # auth_bearer_grant plus refuses an unprovisioned credential
    "provisioning_gate_take(",  # physical BOOT tap, one tap = one consumer
    "receipt_decide(",          # provisioning_gate::receipt_decide: the Host, then
                                # the bearer, then the tap (host-tested). It runs the
                                # last two as lambdas, whose `return` is not the
                                # handler deciding, so the verdict itself must be
                                # bound and tested — and check_host_first holds the
                                # handler to serving only on a SERVE_* verdict
)

# (method, uri) pairs that are intentionally unauthenticated. Every entry
# needs a reason; an entry that stops matching a registered route fails the
# check so the list cannot rot.
PUBLIC_ALLOWLIST = {
    # The two HTML pages. The page itself carries no secret EXCEPT the
    # bearer token injected into its placeholder, and that injection is
    # decided per request by provisioning_gate::page_token_decide
    # (host-tested in firmware/tests_host/test_provisioning_gate.cpp):
    # first-boot setup, bearer-authenticated, SoftAP-subnet peer, or a BOOT
    # tap the load then spends — a home-LAN request with none of those gets
    # the page with an EMPTY token. Every API the page calls is auth_gate'd.
    # check_page_token_wiring() below is what makes this reason true: it
    # fails a page call that does not pass page_token_inject(req).
    # check_host_first() holds page_token_inject to asking the Host before
    # the grants and the tap.
    ("GET", "/"): "dashboard shell; token injection gated by page_token_decide (F20 gap #11), Host first",
    ("GET", "/setup"): "setup wizard shell; token injection gated by page_token_decide (F20 gap #11), Host first",
    # OS captive-portal probes must answer plainly or the OS disconnects.
    # While setup is active (or the STA is down) they serve the setup page
    # through the same page_token_policy; otherwise a fixed success body.
    ("GET", "/hotspot-detect.html*"): "Apple captive probe",
    ("GET", "/library/test/success.html*"): "Apple captive probe (legacy)",
    ("GET", "/generate_204*"): "Android captive probe",
    ("GET", "/gen_204*"): "Android captive probe (legacy)",
    ("GET", "/connecttest.txt*"): "Windows NCSI probe",
    ("GET", "/ncsi.txt*"): "Windows NCSI probe (legacy)",
    # The roll-call the Witness Wall polls (tvos/discovery/DISCOVERY.md): the
    # same coarse body the display and the WAP serve open, read cross-origin
    # by a television — name, product, online, chain, hub; never a key, a
    # MAC or an address. Rate-limited like every route. docs/TRUST_MODEL.md.
    ("GET", "/api/fleet"): "Witness Wall roll-call: coarse by construction, CORS-open (DISCOVERY.md)",
    ("OPTIONS", "/api/fleet"): "CORS preflight for the roll-call; serves nothing",
    # Two registrations share GET /*: the main table's captive wildcard
    # fallback (registered LAST: 302 → /setup while setup is active, else
    # 404), and, with FEATURE_HTTPS, the port-80 server's handle_https_redirect,
    # which POST /* is too. That one answers a plain_http_exempt path as the
    # captive probe does (the setup page through page_token_inject, the
    # page grant), and everything else a 307 to https:// (F15). Its
    # SECRET_ANSWERS row ('page') holds that.
    ("GET", "/*"): "captive wildcard fallback (302/404) / port-80 https redirect: "
                   "a probe path answers as the probe (the page grant), else 307",
    ("POST", "/*"): "FEATURE_HTTPS port-80 server: a probe path answers as the probe "
                    "(the page grant), else 307 to https:// (F15)",
}

# Handlers that gate internally in ways the marker scan cannot attribute to
# a single route. Name → reason. Keep this empty unless a handler genuinely
# needs it: a marker in the body is always the better proof.
SELF_GATING_HANDLERS = {}

# Floor on parsed registrations. Today's table is ~60 (fewer without the
# mesh feature, but the parser reads every #if branch). A parse that finds
# far fewer means the registration style changed and the parser went blind.
MIN_ROUTES = 50


def strip_comments(text: str) -> str:
    """Remove C/C++ comments, honoring string and char literals.

    A naive regex swallows code when a string literal contains */ or //,
    so we walk the source tracking literal state. Newlines are preserved
    (comments become spaces) so nothing merges across lines.
    """
    out = []
    i, n = 0, len(text)
    state = "code"  # code | line_comment | block_comment | string | char
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line_comment"
                i += 2
            elif c == "/" and nxt == "*":
                state = "block_comment"
                i += 2
            elif c == '"':
                out.append(c)
                state = "string"
                i += 1
            elif c == "'":
                out.append(c)
                state = "char"
                i += 1
            else:
                out.append(c)
                i += 1
        elif state == "line_comment":
            if c == "\n":
                out.append("\n")
                state = "code"
            i += 1
        elif state == "block_comment":
            if c == "*" and nxt == "/":
                state = "code"
                i += 2
            else:
                out.append("\n" if c == "\n" else " ")
                i += 1
        elif state in ("string", "char"):
            out.append(c)
            if c == "\\":
                if nxt:
                    out.append(nxt)
                i += 2
            else:
                if (state == "string" and c == '"') or (state == "char" and c == "'"):
                    state = "code"
                i += 1
    return "".join(out)


# Pure and called for the same few files hundreds of times (every handler
# lookup, every call/definition scan), so it is memoized: without the cache
# the check spends most of its time re-walking securacv_network.cpp.
@functools.lru_cache(maxsize=None)
def blank_string_contents(text: str) -> str:
    """Replace the *contents* of string/char literals with spaces, keeping
    the quotes and the original length so byte offsets still align with
    `text`. Used only for brace matching: a `{` or `}` inside a JSON literal
    (`"{\\"ok\\":true}"`) must not shift the depth counter.

    Raw string literals (R"delim(...)delim") are not understood: the canary's
    HTML blobs live in securacv_webui / securacv_setup_page, which define no
    handlers, and main() below fails on any file that defines an httpd
    handler AND holds a raw literal, so the assumption is enforced, not hoped.
    """
    out = []
    i, n = 0, len(text)
    state = "code"  # code | string | char
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            out.append(c)
            if c == '"':
                state = "string"
            elif c == "'":
                state = "char"
            i += 1
        else:
            if c == "\\":
                out.append("  " if nxt else " ")
                i += 2 if nxt else 1
                continue
            if (state == "string" and c == '"') or (state == "char" and c == "'"):
                out.append(c)
                state = "code"
            else:
                out.append("\n" if c == "\n" else " ")
            i += 1
    return "".join(out)


def function_body(source: str, name: str):
    """Return the brace-matched body of `esp_err_t <name>(...)` if present.

    Brace matching runs over a string-blanked view; the returned slice comes
    from the real `source` (blanking is length-preserving) so the marker
    scan sees the true handler text.
    """
    blanked = blank_string_contents(source)
    m = re.search(
        r"esp_err_t\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", blanked)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(blanked) and depth:
        if blanked[i] == "{":
            depth += 1
        elif blanked[i] == "}":
            depth -= 1
        i += 1
    return source[m.start():i]


def parse_routes(fname: str, text: str):
    """(uri, method, handler, fname) for every httpd_uri_t initializer
    (parse_routes_at, without the offsets)."""
    return [(u, m, h, f) for _, u, m, h, f in parse_routes_at(fname, text)]


def parse_routes_at(fname: str, text: str):
    """(offset, uri, method, handler, fname) for every httpd_uri_t initializer.

    `.uri = "<literal>"` is the common form. `.uri = <identifier>` inside a
    range-for over a `static const char* <array>[] = {...}` is the probe
    loop: every string in that array becomes a route with the loop's
    method and handler. Any other non-literal `.uri` is returned with the
    uri None so main() fails loudly instead of skipping it.
    """
    routes = []
    uri_decl = re.compile(r"httpd_uri_t\s+\w+\s*=\s*\{(.*?)\}\s*;", re.S)
    for m in uri_decl.finditer(text):
        fields = m.group(1)
        method = re.search(r"\.method\s*=\s*HTTP_(\w+)", fields)
        handler = re.search(r"\.handler\s*=\s*([A-Za-z_][\w:]*)", fields)
        if not (method and handler):
            continue
        lit = re.search(r'\.uri\s*=\s*"([^"]+)"', fields)
        if lit:
            routes.append((m.start(), lit.group(1), method.group(1), handler.group(1), fname))
            continue
        ident = re.search(r"\.uri\s*=\s*([A-Za-z_]\w*)\s*[,}]", fields)
        uris = None
        if ident:
            # Find the nearest preceding `for (... <ident> : <array>)`, then
            # that array's initializer.
            head = text[:m.start()]
            loops = list(re.finditer(
                r"for\s*\(\s*(?:const\s+)?char\s*(?:const\s*)?\*\s*"
                + re.escape(ident.group(1)) + r"\s*:\s*(\w+)\s*\)", head))
            if loops:
                arr = loops[-1].group(1)
                init = re.search(
                    r"\b" + re.escape(arr) + r"\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;",
                    head, re.S)
                if init:
                    uris = re.findall(r'"([^"]+)"', init.group(1))
        if uris:
            for u in uris:
                routes.append((m.start(), u, method.group(1), handler.group(1), fname))
        else:
            routes.append((m.start(), None, method.group(1), handler.group(1), fname))
    return routes


def _name_pattern(name: str) -> str:
    """`name` as a regex: a bare name, or a qualified one (`AuthManager::compare`)
    with any spacing around each `::`. Anchored at a word boundary, so
    `compare` is not the tail of `constantTimeCompare` and `AuthManager::compare`
    is not `XAuthManager::compare`."""
    return r"\b" + r"\s*::\s*".join(map(re.escape, name.split("::"))) + r"\s*\("


def definition_spans(text: str, name: str) -> list:
    """[(start, end)] of every brace-matched DEFINITION of function `name`
    (any return type, member or free; `Class::name` for one member only). A
    call is `name(...)` followed by `;` or `)`; only a definition is followed
    by `{`. The span starts at the name (its qualifier, for a qualified one)."""
    if name.split("::")[-1] not in text:
        return []
    blanked = blank_string_contents(text)
    out = []
    for m in re.finditer(_name_pattern(name), blanked):
        depth, i = 1, m.end()
        while i < len(blanked) and depth:
            if blanked[i] == "(":
                depth += 1
            elif blanked[i] == ")":
                depth -= 1
            i += 1
        j = i
        while j < len(blanked) and blanked[j] in " \t\n":
            j += 1
        if j < len(blanked) and blanked[j] == "{":
            depth, k = 1, j + 1
            while k < len(blanked) and depth:
                if blanked[k] == "{":
                    depth += 1
                elif blanked[k] == "}":
                    depth -= 1
                k += 1
            out.append((m.start(), k))
    return out


def definition_span(text: str, name: str):
    """(start, end) of the first definition of `name` (definition_spans), or
    None."""
    spans = definition_spans(text, name)
    return spans[0] if spans else None


def call_args(text: str, name: str):
    """[(offset, [arg, ...])] for every CALL of `name` in `text` — a
    definition (followed by `{`) is skipped. Arguments are split on
    top-level commas over a string-blanked view and returned stripped, from
    the real text."""
    out = []
    if name not in text:
        return out
    blanked = blank_string_contents(text)
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", blanked):
        depth, i, cuts = 1, m.end(), [m.end()]
        while i < len(blanked) and depth:
            c = blanked[i]
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == "," and depth == 1:
                cuts.append(i + 1)
            i += 1
        close = i - 1
        j = i
        while j < len(blanked) and blanked[j] in " \t\n":
            j += 1
        if j < len(blanked) and blanked[j] == "{":
            continue  # the definition, not a call
        bounds = cuts + [close + 1]
        args = [text[bounds[k]:bounds[k + 1] - 1].strip()
                for k in range(len(bounds) - 1)]
        out.append((m.start(), [a for a in args if a != ""] if args != [""] else []))
    return out


def _paren_open(blanked: str, close: int) -> int:
    """Offset of the `(` matching the `)` at `close`, or -1."""
    depth = 0
    for k in range(close, -1, -1):
        if blanked[k] == ")":
            depth += 1
        elif blanked[k] == "(":
            depth -= 1
            if depth == 0:
                return k
    return -1


def _paren_close(blanked: str, open_: int) -> int:
    """Offset of the `)` matching the `(` at `open_`, or -1."""
    depth = 0
    for k in range(open_, len(blanked)):
        if blanked[k] == "(":
            depth += 1
        elif blanked[k] == ")":
            depth -= 1
            if depth == 0:
                return k
    return -1


def _opens_lambda(blanked: str, brace: int) -> bool:
    """True when the `{` at `brace` opens a lambda body: `[...] {`,
    `[...](...) {`, with optional mutable / noexcept / `-> T`."""
    head = blanked[max(0, brace - 300):brace]
    m = re.search(r"(?:\s*\b(?:mutable|noexcept|constexpr)\b)*"
                  r"(?:\s*->\s*[\w:<>,*&\s]+?)?\s*$", head)
    j = (m.start() if m else len(head)) - 1
    while j >= 0 and head[j].isspace():
        j -= 1
    if j >= 0 and head[j] == ")":
        o = _paren_open(head, j)
        if o < 0:
            return False
        j = o - 1
        while j >= 0 and head[j].isspace():
            j -= 1
    return j >= 0 and head[j] == "]"


def in_lambda(blanked: str, offset: int) -> bool:
    """True when `offset` sits inside a lambda's body. A `return` there
    decides the lambda's value, not the enclosing handler's — a gate called
    in `[req]() { return bearer_present_and_valid(req); }` decides nothing
    until the lambda's caller uses the answer."""
    depth = 0
    for k in range(offset - 1, -1, -1):
        c = blanked[k]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth:
                depth -= 1
            elif _opens_lambda(blanked, k):
                return True
    return False


def _statement_prefix(blanked: str, i: int) -> str:
    """The statement's text before offset `i`, back to the previous
    `;`, `{` or `}`."""
    stmt = max(blanked.rfind(";", 0, i), blanked.rfind("{", 0, i),
               blanked.rfind("}", 0, i))
    return blanked[stmt + 1:i].strip()


_DECIDING_PREFIX = re.compile(r"^(?:else\s+)?(?:if|while|switch)\s*\(|^return\b")
_CONST_LOCAL = re.compile(r"^const\s+[\w:<>]+\s*[*&]?\s*([A-Za-z_]\w*)\s*=")
# The markers whose result is not a yes or no: the comparison their call must
# stand in to say yes. auth_bearer_grant()'s four values (NONE / OK / WRONG /
# HELD) say yes only as OK, so `!= ...::WRONG` or `== ...::NONE` would serve a
# request with no header, or a held one.
_MARKER_YES = {
    "auth_bearer_grant(": re.compile(r"\s*==\s*(?:AuthManager\s*::\s*)?BearerResult\s*::\s*OK\b"),
}


def _says_yes(blanked: str, marker: str, i: int) -> bool:
    """True when the call of `marker` at `i` says yes or no by itself, or
    stands in the one comparison _MARKER_YES names for it."""
    yes = _MARKER_YES.get(marker)
    if yes is None:
        return True
    close = _paren_close(blanked, i + len(marker) - 1)
    return close >= 0 and yes.match(blanked, close + 1) is not None


def first_deciding_marker(body: str, marker: str) -> int:
    """Offset of the first occurrence of `marker` whose result decides
    something, or -1. It decides when the statement's text before it (back
    to the previous ';', '{' or '}') opens with `if (`, `else if (`,
    `while (`, `switch (` or `return`, or when it initializes a `const`
    local that a later if / while / switch condition or return expression
    reads. A bare call, a `(void)` cast, a non-const or unread local, and a
    `return` inside a lambda body (the lambda deciding, not the function)
    discard the answer and do not count, and neither does a marker whose
    answer is not a yes or no outside the comparison _MARKER_YES names."""
    blanked = blank_string_contents(body)
    start = 0
    while True:
        i = blanked.find(marker, start)
        if i < 0:
            return -1
        start = i + 1
        if in_lambda(blanked, i) or not _says_yes(blanked, marker, i):
            continue
        prefix = _statement_prefix(blanked, i)
        if _DECIDING_PREFIX.match(prefix):
            return i
        local = _CONST_LOCAL.match(prefix)
        if local:
            end = blanked.find(";", i)
            for use in re.finditer(r"\b" + re.escape(local.group(1)) + r"\b",
                                   blanked[end + 1:] if end >= 0 else ""):
                at = end + 1 + use.start()
                if (not in_lambda(blanked, at)
                        and _DECIDING_PREFIX.match(_statement_prefix(blanked, at))):
                    return i


def marker_decides(body: str, marker: str) -> bool:
    return first_deciding_marker(body, marker) >= 0


def returns_before(body: str, limit: int) -> bool:
    """True when the function body has an UNCONDITIONAL return — a statement
    that starts with `return` directly in the function's own block — before
    offset `limit`. Anything after it is unreachable, so a gate there is no
    gate. `if (x) return y;` is conditional (its statement starts with
    `if`), as is any return nested in a deeper block."""
    blanked = blank_string_contents(body)
    open_brace = blanked.find("{")
    if open_brace < 0:
        return False
    depth = 0
    for m in re.finditer(r"[{}]|\breturn\b", blanked[:limit]):
        tok = m.group(0)
        if tok == "{":
            depth += 1
        elif tok == "}":
            depth -= 1
        elif depth == 1 and m.start() > open_brace:
            stmt = max(blanked.rfind(";", 0, m.start()), blanked.rfind("{", 0, m.start()),
                       blanked.rfind("}", 0, m.start()))
            if blanked[stmt + 1:m.start()].strip() == "":
                return True
    return False


def gate_guards(body: str) -> bool:
    """A handler body is gated when some marker decides something AND no
    unconditional return comes before the first such marker."""
    firsts = [i for i in (first_deciding_marker(body, mk) for mk in AUTH_MARKERS) if i >= 0]
    return bool(firsts) and not returns_before(body, min(firsts))


def check_page_token_wiring(sources: dict) -> list:
    """Every page that can carry the bearer must ask page_token_inject(req),
    and page_token_inject must spend the tap (take), never peek."""
    failures = []
    sites = 0
    for fname, text in sorted(sources.items()):
        for off, args in call_args(text, "send_html_with_token"):
            sites += 1
            line = text.count("\n", 0, off) + 1
            third = args[2] if len(args) >= 3 else None
            if third != "page_token_inject(req)":
                failures.append(
                    f"{fname}:{line}: send_html_with_token(...) passes "
                    f"{third!r} as the page-token verdict — it must be "
                    f"page_token_inject(req) (a literal PageToken::INJECT hands "
                    f"the bearer to every home-LAN caller; that reverts F20 "
                    f"gap #11)")
        span = definition_span(text, "page_token_inject")
        if span:
            body = text[span[0]:span[1]]
            if "page_token_decide(" not in body:
                failures.append(f"{fname}: page_token_inject no longer calls "
                                f"provisioning_gate::page_token_decide (host-tested)")
            if "provisioning_gate_take" not in body:
                failures.append(f"{fname}: page_token_inject does not spend the "
                                f"tap through provisioning_gate_take")
            for peek in ("is_open", "provisioning_gate_is_open", "s_gate_is_open"):
                if re.search(r"\b" + peek + r"\b", body):
                    failures.append(
                        f"{fname}: page_token_inject reads the gate with a peek "
                        f"({peek}) — one tap would unlock every page load for "
                        f"the whole TTL; take it (page_token_decide)")
                    break
            if re.search(r"\breturn\s+(?:true|1)\s*;", body):
                failures.append(f"{fname}: page_token_inject has an unconditional "
                                f"`return true`")
    if sites < 3:
        failures.append(f"found only {sites} send_html_with_token call(s) (floor 3: "
                        f"/, /setup, the Apple probe) — parser regression?")
    if not any(definition_span(t, "page_token_inject") for t in sources.values()):
        failures.append("page_token_inject definition not found")
    return failures


# ── The Host first ────────────────────────────────────────────────────────────

# The Host check: the Host a request targeted must name this device, unless
# the request arrived over the Canary's own SoftAP (securacv_network's
# host_is_foreign over firmware/common/network/host_guard.h), asked of THIS
# request.
_HOST_CALL = r"host_is_foreign\s*\(\s*req\s*\)"

# The host-tested decisions that take the Host verdict as their FIRST
# argument (firmware/common/network/provisioning_gate.h) and decide it before
# anything else they are given.
HOST_DECISIONS = ("receipt_decide", "page_token_decide")

# Functions that put a credential into a response: what each carries (the
# SECRET_READS kinds, which the bearer-only answer rule holds it to exactly)
# and what it emits. Every call of one must sit behind the Host check (or,
# for the page, behind page_token_inject, which asks it).
TOKEN_EMITTERS = {
    "send_provisioning_receipt": (
        "token ap pin",
        "the provisioning receipt (the API token, the SoftAP password and the TLS pin)"),
    "send_html_with_token": (
        "token", "a page with the API token in its __CV_TOKEN__ placeholder"),
}

# Functions that read auth_get_token() only to compare a presented credential
# against it, never into a response.
TOKEN_COMPARERS = {
    "auth_gate": "the API gate: the Host, then the bearer compare",
    "bearer_present_and_valid": "the accounted bearer grant the receipt and the pages use",
}

# ── The bearer-only answers: what the rule reads ─────────────────────────────

# The three credentials, by the spellings this tree reads them with (over
# comment-stripped, string-blanked text): each store, its accessor and its
# deriver. The TLS pin is public (every TLS handshake shows the certificate
# it hashes); it is held because the apps trust it as the pin.
SECRET_READS = {
    "token": re.compile(r"\bauth_get_token\s*\(|\bs_bearer\b|\bderive_api_token\s*\("),
    "ap": re.compile(r"\bgetApPassword\s*\(|\bm_ap_password\b|\bg_ap_password\b"
                     r"|\bderive_ap_password\s*\("),
    "pin": re.compile(r"\bgetTlsCertFp\s*\(|\bm_tls_cert_fp_hex\b"),
}
SECRET_NAMES = {"token": "the bearer token", "ap": "the SoftAP password",
                "pin": "the TLS pin"}
# The file suffixes the tables key rows by (a file is named by its path's
# tail, so a fixture's file and the tree's match alike).
_NET = "securacv_network.cpp"
_AUTH = "securacv_auth.cpp"
_MAIN = "src/main.cpp"

# The accessors and the derivers under ROOTS: reading the store (or making
# the value) is their job, so their own body is not a reader; a call of one
# is a read by its caller (SECRET_READS spells each call). Each is defined
# once, in its home file: a second definition by the same name elsewhere is
# a reader like any other. derive_ap_password lives outside ROOTS
# (firmware/common/network/ap_password.h, host-tested by test_ap_password.cpp).
SECRET_ACCESSORS = {
    "auth_get_token": (_AUTH, "token"),
    "derive_api_token": ("securacv_crypto.cpp", "token"),
    "getApPassword": ("securacv_network.h", "ap"),
    "getTlsCertFp": ("securacv_network.h", "pin"),
}
# The mints: a statement that calls one in an if condition is the whole if,
# its branch included, so a statement pin holds the call, its refusal and
# what the refusal does (the F350 review: a pin on the argument alone would
# pass a deleted call, a discarded result or a dropped halt).
SECRET_MINTS = ("derive_api_token", "derive_ap_password")

# The declarations outside every function that name a credential, as
# (file suffix, folded statement). Anything else outside a function fails:
# the rule reads credentials per function.
OUTSIDE_FUNCTION_LINES = (
    (_AUTH,
     "static char s_bearer[36];"),
    ("securacv_auth.h",
     "const char* auth_get_token();"),
    ("securacv_crypto.h",
     "bool derive_api_token(const uint8_t privkey[32], char* token_str, size_t token_str_len);"),
    ("securacv_network.h",
     "char m_ap_password[65];"),
    ("securacv_network.h",
     "char m_tls_cert_fp_hex[65];"),
    (_MAIN,
     "static char g_ap_password[16];"),
    (_MAIN,
     'static_assert(sizeof(g_ap_password) >= canary::net::AP_PASSWORD_LEN + 1, "g_ap_password '
     'must hold the cv- key and its NUL");'),
)

# Device-side uses: the functions that read a credential and use it on the
# device (a buffer, the radio, the serial line the owner and the flashers
# read), never in an answer. Keyed by (file suffix, qualified function): a
# function of the same name in another file or class is not excused. Each
# row is (reason, every statement of the function that reads a credential,
# whitespace folded, literals intact, in order). A statement runs from just
# after the nearest `;`, `{` or `}` at its block's level to the first `;`
# or `{` at that level (a mint's, SECRET_MINTS, through its branch). The
# carry up the call graph stops here. A new statement that reads a
# credential (a serial, BLE or MQTT sink), an edited one, or a dropped one
# fails until the row says it, and the failure prints the row as it now is.
SECRET_LOCAL_USES = {
    (_NET, "ScvNetworkManager::ScvNetworkManager"): (
        "the constructor clears both buffers",
        (
            "m_ap_password[0] = '\\0';",
            "m_tls_cert_fp_hex[0] = '\\0';",
        )),
    (_NET, "ScvNetworkManager::begin"): (
        "keeps the SoftAP password it is handed for raiseAp(), and prints the setup-network "
        "line both flashers parse",
        (
            "strncpy(m_ap_password, ap_password, sizeof(m_ap_password) - 1);",
            "m_ap_password[sizeof(m_ap_password) - 1] = '\\0';",
            "m_ap_password[0] = '\\0';",
            "announce_setup_network(m_ap_ssid, m_ap_password);",
        )),
    (_NET, "ScvNetworkManager::raiseAp"): (
        "re-raises the SoftAP with its own password after the home link drops, and prints the "
        "setup-network line again",
        (
            "bool ap_ok = WiFi.softAP(m_ap_ssid, m_ap_password, AP_CHANNEL, false, "
            "AP_MAX_CONNECTIONS);",
            "announce_setup_network(m_ap_ssid, m_ap_password);",
        )),
    (_NET, "ScvNetworkManager::initTls"): (
        "computes the pin from the certificate and logs its first 16 digits",
        (
            "canary::net::tls_policy::fingerprint_hex(digest, m_tls_cert_fp_hex);",
            'Serial.printf("[TLS] Cert fingerprint: %.16s...\\n", m_tls_cert_fp_hex);',
        )),
    (_MAIN, "setup"): (
        "derives the SoftAP password from the private key (F350: the whole statement, the halt "
        "included), starts the network with it, and prints it and the pin's first 16 digits on "
        "the serial banner the owner and the flashers read",
        (
            "if (!canary::net::derive_ap_password(device.privkey, g_ap_password, "
            'sizeof(g_ap_password))) { Serial.println("[!!] Setup network key derivation '
            'failed - HALTING"); while (true) { delay(1000); } }',
            "if (net.begin(ap_ssid, g_ap_password, device.device_id)) {",
            'Serial.printf("║ Password : %-45s ║\\n", g_ap_password);',
            'snprintf(fp_short, sizeof(fp_short), "%.16s...", network.getTlsCertFp());',
        )),
    (_AUTH, "auth_load_or_derive"): (
        "loads the bearer token from NVS or derives it from the private key (the whole mint "
        "statement, its refusal included) into the auth lib's own buffer, and persists it",
        (
            "if (tlen > 0 && tlen < sizeof(s_bearer)) {",
            "size_t got = nvs.getBytes(NVS_KEY_TOKEN, s_bearer, tlen);",
            "s_bearer[tlen] = '\\0';",
            'loaded = (strncmp(s_bearer, "cv_", 3) == 0 && strlen(s_bearer) >= 35);',
            "s_bearer[0] = '\\0';",
            "if (!derive_api_token(privkey, s_bearer, sizeof(s_bearer))) { s_bearer[0] = "
            "'\\0'; return false; }",
            "size_t wrote = nvs.putBytes(NVS_KEY_TOKEN, s_bearer, strlen(s_bearer));",
            "return s_bearer[0] != '\\0';",
        )),
}

# The route handlers whose answer carries a credential: exactly what it
# carries, and its grant.
#   bearer  — `if (!auth_gate(req)) return ESP_OK;` at the handler's own top
#             level, before every answer site.
#   receipt — receipt_decide handed exactly RECEIPT_GRANTS; rule 6 holds the
#             dispatch (a send only under a SERVE_* test, then the Host
#             refusal), and every answer site sits under a SERVE_* test.
#   page    — every answer site is send_html_with_token(...) (rule 3 holds
#             its verdict to page_token_inject(req)) or a call of another
#             page-grant handler. A PUBLIC_ALLOWLIST route may carry only
#             with this grant.
SECRET_ANSWERS = {
    "handle_provisioning_receipt": ("token ap pin", "receipt"),
    "handle_status": ("pin", "bearer"),
    "handle_ui": ("token", "page"),
    "handle_setup_page": ("token", "page"),
    "handle_captive_probe": ("token", "page"),
    # Port 80 beside HTTPS: a plain_http_exempt path goes to the probe.
    "handle_https_redirect": ("token", "page"),
}
# The refusal a 'bearer' grant stands after, folded (the census holds every
# auth_gate call to exactly this statement).
BEARER_REFUSAL = "if (!auth_gate(req)) return ESP_OK;"

# ── The bearer-only answers: the grant glue, pinned whole ────────────────────

# Every piece of glue a grant runs through, keyed by (file suffix, qualified
# function): what its body guarantees, and the body itself from the name to
# the closing brace, comment-stripped and whitespace folded. No host test
# reaches this glue (the decisions it calls are host-tested; the auth lib's
# arithmetic is auth_backoff.h's, test_auth_backoff.cpp's), so the body is
# pinned: an edit to it is an edit to its row in the same review, and the
# failure prints the body as it now is. host_is_foreign was the first row
# (HOST_IS_FOREIGN_DEF).
_PAGE_GRANTS = (
    "SECURITY_MODEL.md's four unlocks, and only them: first-boot setup, a real bearer "
    "(auth_bearer_grant through bearer_present_and_valid), a SoftAP peer (from_ap_subnet) "
    "and one BOOT tap taken, never peeked, with auth_handed_token only on the tap that "
    "served (F341). A home-LAN reload without a tap is CONSOLIDATION.md Phase 6's "
    "cv_session port, which must bring canary-wap's any-session class (ANY_SESSION_GATES, "
    "F328) with it: never widen an input here instead")
GLUE_DEFS = {
    (_NET, "host_is_foreign"): (
        "every token path trusts it: the SoftAP exemption (the interface, never the name), a "
        "missing or oversize Host is foreign, then host_guard.h's host_names_this_device",
        "host_is_foreign(httpd_req_t* req) { if (from_ap_subnet(req)) return false; char "
        'host[96]; if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) '
        "return true; return !canary::net::host_names_this_device(host); }"),
    (_NET, "auth_gate"): (
        "the gate of every auth_gate route: the Host first, fail closed on an empty token, "
        "then auth_check against request_peer_ipv4(req), the socket peer",
        "auth_gate(httpd_req_t* req) { if (host_is_foreign(req)) { send_host_refusal(req); "
        "return false; } const char* token = auth_get_token(); if (!token || token[0] == "
        '\'\\0\') { httpd_resp_set_status(req, "503 Service Unavailable"); '
        'httpd_resp_set_type(req, "application/json"); httpd_resp_sendstr(req, '
        '"{\\"error\\":\\"not_provisioned\\"}"); witness_get_health().http_errors++; return '
        "false; } if (!auth_check(req, token, request_peer_ipv4(req))) { "
        "witness_get_health().http_errors++; return false; } return true; }"),
    (_NET, "bearer_present_and_valid"): (
        "the receipt's and the pages' bearer grant: the empty-token guard, then "
        "auth_bearer_grant(...) == BearerResult::OK over the Authorization header and nothing "
        "else",
        "bearer_present_and_valid(httpd_req_t* req) { const char* token = auth_get_token(); if "
        "(!token || token[0] == '\\0') return false; return auth_bearer_grant(req, token, "
        "request_peer_ipv4(req)) == AuthManager::BearerResult::OK; }"),
    (_NET, "from_ap_subnet"): (
        "the SoftAP grant: provisioning_gate::request_on_softap over the socket's peer and "
        "local addresses, never a header or a name",
        "from_ap_subnet(httpd_req_t* req) { if (!(WiFi.getMode() & WIFI_AP)) return false; "
        "const int fd = httpd_req_to_sockfd(req); if (fd < 0) return false; struct "
        "sockaddr_storage peer_ss; struct sockaddr_storage local_ss; socklen_t plen = "
        "sizeof(peer_ss); socklen_t llen = sizeof(local_ss); memset(&peer_ss, 0, "
        "sizeof(peer_ss)); memset(&local_ss, 0, sizeof(local_ss)); if (getpeername(fd, (struct "
        "sockaddr*)&peer_ss, &plen) != 0) return false; if (getsockname(fd, (struct "
        "sockaddr*)&local_ss, &llen) != 0) return false; uint32_t ap_ip, ap_mask, sta_ip, "
        'sta_mask; netif_ip4("WIFI_AP_DEF", &ap_ip, &ap_mask); netif_ip4("WIFI_STA_DEF", '
        "&sta_ip, &sta_mask); return canary::net::provisioning_gate::request_on_softap( "
        "sockaddr_ip4_host_order(peer_ss), sockaddr_ip4_host_order(local_ss), ap_ip, ap_mask, "
        "sta_ip, sta_mask); }"),
    (_NET, "network_set_provisioning_gate_hooks"): (
        "the BOOT-tap hooks, stored as main.cpp hands them: the take hook, and the is_open "
        "hook /api/status reads as a wiring check",
        "network_set_provisioning_gate_hooks(network_gate_fn_t take, network_gate_fn_t "
        "is_open) { s_gate_take = take; s_gate_is_open = is_open; }"),
    (_NET, "provisioning_gate_take"): (
        "the tap: take, never peek; unregistered means closed",
        "provisioning_gate_take() { return s_gate_take ? s_gate_take() : false; }"),
    (_NET, "page_token_inject"): (
        _PAGE_GRANTS,
        "page_token_inject(httpd_req_t* req) { using "
        "canary::net::provisioning_gate::PageToken; using "
        "canary::net::provisioning_gate::page_token_decide; const bool foreign = "
        "host_is_foreign(req); const bool setup_active = setup_is_active() || "
        "setup_is_first_boot(); const bool bearer_ok = !foreign && "
        "bearer_present_and_valid(req); const bool on_ap = from_ap_subnet(req); bool tap_spent "
        "= false; const char* why = nullptr; const PageToken verdict = page_token_decide( "
        "foreign, setup_active, bearer_ok, on_ap, [&tap_spent]() { tap_spent = "
        "provisioning_gate_take(); return tap_spent; }, &why); if (verdict != "
        'PageToken::INJECT) { Serial.printf("[AUTH] page token withheld (%s)\\n", why ? why : '
        '""); } else if (tap_spent) { auth_handed_token(request_peer_ipv4(req)); '
        'Serial.println("[AUTH] page token handed to one page load on a BOOT tap. Gate '
        'closed."); log_health(LOG_LEVEL_INFO, LOG_CAT_NETWORK, "Page token unlocked", "BOOT '
        'gate"); } return verdict; }'),
    (_MAIN, "prov_gate_take_hook"): (
        "the take hook: provisioning_gate::take, never is_open",
        "prov_gate_take_hook() { return canary::net::provisioning_gate::take(g_prov_gate, "
        "millis(), PROVISIONING_GATE_TTL_MS); }"),
    (_AUTH, "AuthManager::check"): (
        "the bearer check of every auth_gate route: the lockout read before any compare, no "
        "Authorization header a 401 with no strike, every wrong header one strike, the right "
        "one clears",
        "AuthManager::check(httpd_req_t* req, const char* expected_token, uint32_t source) { "
        "if (!req || !expected_token) return false; const uint32_t now = millis(); if "
        "(canary::net::auth_backoff::locked(m_table, source, now)) { sendLockout(req, source); "
        "return false; } Presented why = Presented::WRONG; const bool presented = "
        'httpd_req_get_hdr_value_len(req, "Authorization") != 0; const BearerResult result = '
        "canary::net::auth_backoff::grant( m_table, source, now, presented, [&]() { why = "
        "compare(req, expected_token); return why == Presented::RIGHT; }); switch (result) { "
        "case BearerResult::OK: return true; case BearerResult::HELD: sendLockout(req, "
        'source); return false; case BearerResult::NONE: httpd_resp_set_status(req, "401 '
        'Unauthorized"); httpd_resp_set_type(req, "application/json"); httpd_resp_set_hdr(req, '
        '"WWW-Authenticate", "Bearer realm=\\"securacv\\""); httpd_resp_sendstr(req, '
        '"{\\"error\\":\\"unauthorized\\"," "\\"hint\\":\\"Include \'Authorization: Bearer '
        'cv_...\' header\\"}"); return false; case BearerResult::WRONG: break; } switch (why) '
        '{ case Presented::TOO_LONG: send_json_error(req, "401 Unauthorized", '
        '"{\\"error\\":\\"unauthorized\\"," "\\"hint\\":\\"Authorization header too '
        'long\\"}"); break; case Presented::UNREADABLE: send_json_error(req, "401 '
        'Unauthorized", "{\\"error\\":\\"unauthorized\\"}"); break; case '
        'Presented::NOT_BEARER: send_json_error(req, "401 Unauthorized", '
        '"{\\"error\\":\\"unauthorized\\"," "\\"hint\\":\\"Use \'Bearer <token>\' '
        'format\\"}"); break; case Presented::RIGHT: case Presented::WRONG: '
        'send_json_error(req, "403 Forbidden", "{\\"error\\":\\"forbidden\\"}"); break; } '
        "return false; }"),
    (_AUTH, "AuthManager::grant"): (
        "the accounted grant: auth_backoff::grant with presented = a non-empty Authorization "
        "header and a lambda over compare, so a held source is never compared",
        "AuthManager::grant(httpd_req_t* req, const char* expected_token, uint32_t source) { "
        "if (!req || !expected_token) return BearerResult::NONE; const bool presented = "
        'httpd_req_get_hdr_value_len(req, "Authorization") != 0; return '
        "canary::net::auth_backoff::grant( m_table, source, millis(), presented, [&]() { "
        "return compare(req, expected_token) == Presented::RIGHT; }); }"),
    (_AUTH, "AuthManager::compare"): (
        "the one compare: the Authorization header only, the Bearer prefix, the trim, the "
        "length, then constantTimeCompare",
        "AuthManager::compare(httpd_req_t* req, const char* expected_token) { size_t auth_len "
        '= httpd_req_get_hdr_value_len(req, "Authorization"); if (auth_len >= '
        "SECURACV_AUTH_HEADER_BUF) return Presented::TOO_LONG; char "
        "auth_buf[SECURACV_AUTH_HEADER_BUF]; if (httpd_req_get_hdr_value_str(req, "
        '"Authorization", auth_buf, sizeof(auth_buf)) != ESP_OK) { return '
        'Presented::UNREADABLE; } if (strncmp(auth_buf, "Bearer ", 7) != 0) return '
        "Presented::NOT_BEARER; const char* token = auth_buf + 7; while (*token == ' ') "
        "token++; size_t token_len = strlen(token); while (token_len > 0 && token[token_len - "
        "1] == ' ') token_len--; size_t expected_len = strlen(expected_token); if "
        "(expected_len == 0) return Presented::WRONG; if (token_len != expected_len) return "
        "Presented::WRONG; return constantTimeCompare(token, expected_token, expected_len) ? "
        "Presented::RIGHT : Presented::WRONG; }"),
    (_AUTH, "AuthManager::handedToken"): (
        "the tap's clear: record_success for that source only",
        "AuthManager::handedToken(uint32_t source) { "
        "canary::net::auth_backoff::record_success(m_table, source); }"),
    (_AUTH, "auth_check"): (
        "a one-line delegation to the instance's check",
        "auth_check(httpd_req_t* req, const char* expected_token, uint32_t source) { return "
        "auth_get_instance().check(req, expected_token, source); }"),
    (_AUTH, "auth_bearer_grant"): (
        "a one-line delegation to the instance's grant",
        "auth_bearer_grant(httpd_req_t* req, const char* expected_token, uint32_t source) { "
        "return auth_get_instance().grant(req, expected_token, source); }"),
    (_AUTH, "auth_handed_token"): (
        "a one-line delegation to the instance's handedToken",
        "auth_handed_token(uint32_t source) { auth_get_instance().handedToken(source); }"),
}

# ── The bearer-only answers: the gate primitives' call sites ─────────────────

# The census: where each gate primitive may be called, under ROOTS, outside
# its own definition and its declarations. Per name:
#   callers — the (file suffix, qualified function) set that may call it,
#             every one of which does (None: any function);
#   file    — only calls in this file count (a common name: `compare`);
#   pattern — the call's spelling, when `\bname\s*\(` is not it;
#   whole   — every call is exactly this folded statement;
#   once    — exactly one call;
#   served  — callers whose every call sits under a SERVE_* test of the
#             receipt_decide verdict (rule 6's _under_serve_test);
#   ident   — a variable: every mention counts, and outside a function only
#             the declarations in "decl" may name it.
# A row whose name has no call fails, and so does a listed caller that no
# longer calls. Every AUTH_MARKERS entry names a row, and every
# PRE_HOST_STEPS entry a row, a TOKEN_EMITTERS key or auth_get_token.
GATE_CALL_SITES = {
    "auth_gate": {"callers": None, "whole": BEARER_REFUSAL},
    "auth_check": {"callers": {(_NET, "auth_gate")}},
    "auth_bearer_grant": {"callers": {(_NET, "bearer_present_and_valid")}},
    "auth_handed_token": {"callers": {(_NET, "page_token_inject"),
                                      (_NET, "handle_provisioning_receipt")},
                          "served": {(_NET, "handle_provisioning_receipt")}},
    "auth_get_instance": {"callers": {(_AUTH, "auth_check"), (_AUTH, "auth_bearer_grant"),
                                      (_AUTH, "auth_handed_token")}},
    "compare": {"callers": {(_AUTH, "AuthManager::check"), (_AUTH, "AuthManager::grant")},
                "file": _AUTH, "pattern": r"(?<![\w.])compare\s*\("},
    "constantTimeCompare": {"callers": {(_AUTH, "AuthManager::compare")}},
    "bearer_present_and_valid": {"callers": {(_NET, "page_token_inject"),
                                             (_NET, "handle_provisioning_receipt")}},
    "provisioning_gate_take": {"callers": {(_NET, "page_token_inject"),
                                           (_NET, "handle_provisioning_receipt")}},
    "receipt_decide": {"callers": {(_NET, "handle_provisioning_receipt")}},
    "page_token_decide": {"callers": {(_NET, "page_token_inject")}},
    "network_set_provisioning_gate_hooks": {
        "callers": {(_MAIN, "setup")}, "once": True,
        "whole": "network_set_provisioning_gate_hooks(prov_gate_take_hook, "
                 "prov_gate_is_open_hook);"},
    "s_gate_take": {"callers": {(_NET, "network_set_provisioning_gate_hooks"),
                                (_NET, "provisioning_gate_take"), (_NET, "handle_status")},
                    "ident": True, "decl": ("static network_gate_fn_t s_gate_take = nullptr;",)},
}
# receipt_decide's grants after the Host, folded: the bearer, then the tap.
RECEIPT_GRANTS = ("[req]() { return bearer_present_and_valid(req); }",
                  "[]() { return provisioning_gate_take(); }")

# ── The bearer-only answers: the no-session premise ──────────────────────────

# The flagship has no session: no cookie, no session header, no credential
# in a URL query. The request headers each file may read, by literal name.
REQUEST_HEADERS = {_AUTH: ("Authorization",), _NET: ("Host",)}
# The functions that may read a URL query: the witness log's paging.
URL_QUERY_READERS = {(_NET, "handle_witness")}
_URL_QUERY = re.compile(r"\b(httpd_req_get_url_query_len|httpd_req_get_url_query_str"
                        r"|httpd_query_key_value)\s*\(")
_NO_SESSION = ("the flagship has no session today. A cookie, a session header or a query "
               "credential is canary-wap's any-session class (ANY_SESSION_GATES, F328): "
               "CONSOLIDATION.md Phase 6's cv_session port must bring that rule with it")

# What must not run before the Host is decided: reading the bearer, taking
# the tap, reading the token, emitting it.
PRE_HOST_STEPS = ("bearer_present_and_valid(", "auth_bearer_grant(", "auth_check(",
                  "provisioning_gate_take(", "auth_get_token(") + tuple(
                      name + "(" for name in TOKEN_EMITTERS)

_KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "sizeof", "defined"}


_FUNC_HEAD = re.compile(r"\b([A-Za-z_]\w*)\s*\([^;{}()]*\)\s*(?:const\s*)?\{")
# A constructor's parameter list closing, then its initializer list's `:`
# (not a `::`).
_INIT_COLON = re.compile(r"\)\s*:(?!:)")
# One initializer-list entry's member, up to its `(` or `{`.
_INIT_ENTRY = re.compile(r"\s*[A-Za-z_][\w:]*\s*(?=[({])")
# The qualifier directly before a function's name: `Class::` (or `Class::~`).
_QUALIFIER = re.compile(r"\b([A-Za-z_]\w*)\s*::\s*(~\s*)?$")


def _bracket_close(blanked: str, open_: int) -> int:
    """Offset of the `)` or `}` closing the `(` or `{` at `open_`, or -1."""
    pair = {"(": ")", "{": "}"}[blanked[open_]]
    depth = 0
    for k in range(open_, len(blanked)):
        if blanked[k] == blanked[open_]:
            depth += 1
        elif blanked[k] == pair:
            depth -= 1
            if depth == 0:
                return k
    return -1


def _qualified(blanked: str, at: int, name: str):
    """(qualified name, start) of the function name `name` found at `at`:
    `Class::name` when a qualifier sits directly before it (only that one:
    `ns::Type Class::name(` is `Class::name`), `Class::~name` for a
    destructor, else the bare name."""
    head = blanked[max(0, at - 200):at]
    m = _QUALIFIER.search(head)
    if m:
        return (f"{m.group(1)}::{'~' if m.group(2) else ''}{name}",
                at - len(head) + m.start())
    return name, at


def _initializer_body(blanked: str, i: int) -> int:
    """The offset of the `{` that opens a constructor's body, when an
    initializer list (`a_(x), b_{y}, ...`) starts at `i` (just past its
    `:`), else -1 (a ternary's `:`, a label, anything that is not one)."""
    while True:
        m = _INIT_ENTRY.match(blanked, i)
        if not m:
            return -1
        close = _bracket_close(blanked, m.end())
        if close < 0:
            return -1
        k = close + 1
        while k < len(blanked) and blanked[k].isspace():
            k += 1
        if k < len(blanked) and blanked[k] == ",":
            i = k + 1
            continue
        return k if k < len(blanked) and blanked[k] == "{" else -1


@functools.lru_cache(maxsize=None)
def function_defs(text: str) -> tuple:
    """(name, start, name_at, code_start, end) for every brace-bodied
    function definition in `text` whose parameter list holds no parentheses
    (every handler and helper here). `name` is qualified when the head has a
    qualifier (`AuthManager::grant`, `ScvNetworkManager::begin`); `start` is
    the head (its qualifier); `name_at` is where the name itself is;
    `code_start` is the body's `{`, or a constructor's initializer-list `:`;
    `end` is just past the closing `}`.

    A constructor is named for its class (`ScvNetworkManager::
    ScvNetworkManager`), its initializer list included, never for its last
    initializer entry (`m_peers_last_browse_ms(0) {` read as a head was the
    old parser's name for that body). A lambda has no name before its `(`,
    so a statement inside one is attributed to the function that holds the
    lambda."""
    blanked = blank_string_contents(text)
    heads = []
    inits = []
    for m in _INIT_COLON.finditer(blanked):
        open_ = _paren_open(blanked, m.start())
        if open_ < 0:
            continue
        j = open_
        while j > 0 and blanked[j - 1].isspace():
            j -= 1
        name = re.search(r"[A-Za-z_]\w*$", blanked[max(0, j - 80):j])
        if not name or name.group(0) in _KEYWORDS:
            continue
        body = _initializer_body(blanked, m.end())
        if body < 0:
            continue
        at = j - len(name.group(0))
        qual, start = _qualified(blanked, at, name.group(0))
        inits.append((m.end(), body))
        heads.append((qual, start, at, m.end() - 1, body))
    for m in _FUNC_HEAD.finditer(blanked):
        if m.group(1) in _KEYWORDS or any(a <= m.start() < b for a, b in inits):
            continue
        qual, start = _qualified(blanked, m.start(1), m.group(1))
        heads.append((qual, start, m.start(1), m.end() - 1, m.end() - 1))
    out = []
    for qual, start, at, code_start, body in heads:
        close = _bracket_close(blanked, body)
        out.append((qual, start, at, code_start, close + 1 if close >= 0 else len(blanked)))
    return tuple(sorted(out, key=lambda d: d[1]))


def function_spans(text: str):
    """[(name, start, end)] for every function_defs() definition: the name
    (qualified for a member), the head, the end."""
    return [(name, start, end) for name, start, _at, _code, end in function_defs(text)]


def enclosing_function(spans, offset: int):
    """The innermost (name, start, end) of `spans` holding `offset`, or None."""
    best = None
    for name, start, end in spans:
        if start <= offset < end and (best is None or start > best[1]):
            best = (name, start, end)
    return best


_HOST_LOCAL = re.compile(r"\bconst\s+bool\s+([A-Za-z_]\w*)\s*=\s*" + _HOST_CALL + r"\s*;")


def refusal_branch(blanked: str, at: int):
    """The statement an if condition ending at `at` controls, as (start,
    end), when it always returns — `return ...;`, or a `{...}` block with a
    `return` directly in it (a return nested deeper, or in a lambda, does
    not count) — else None."""
    j = at
    while j < len(blanked) and blanked[j].isspace():
        j += 1
    if re.match(r"return\b", blanked[j:j + 7]):
        end = blanked.find(";", j)
        return (j, end + 1) if end >= 0 else None
    if j >= len(blanked) or blanked[j] != "{":
        return None
    depth, k, returns = 0, j, False
    while k < len(blanked):
        c = blanked[k]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return (j, k + 1) if returns else None
        elif (depth == 1 and blanked.startswith("return", k)
              and not (blanked[k - 1].isalnum() or blanked[k - 1] == "_")
              and not (blanked[k + 6:k + 7].isalnum() or blanked[k + 6:k + 7] == "_")
              and _statement_prefix(blanked, k) == ""):
            returns = True
        k += 1
    return None


def _written_once(blanked: str, name: str) -> bool:
    """True when `name` is assigned exactly once in `blanked` (its
    declaration): no shadowing redeclaration, no reassignment."""
    return len(re.findall(r"\b" + re.escape(name) + r"\s*=(?!=)", blanked)) == 1


def host_refusals(body: str) -> list:
    """(offset, branch_text) for every `if (host_is_foreign(req))` — or
    `if (<local>)` for a `const bool <local> = host_is_foreign(req);`
    written once — whose condition is the Host verdict ALONE, whose branch
    always returns, and whose branch reads no credential, takes no tap and
    emits nothing (PRE_HOST_STEPS): the refusal. A branch that only logs,
    one that serves, and a condition that mixes the verdict with anything
    else are not a Host check."""
    blanked = blank_string_contents(body)
    out = []
    def consider(cond_end, at):
        if in_lambda(blanked, at):
            return
        span = refusal_branch(blanked, cond_end)
        if span and not steps_before(blanked[span[0]:span[1]], span[1] - span[0]):
            out.append((at, blanked[span[0]:span[1]]))
    for m in re.finditer(r"\b(?:else\s+)?if\s*\(\s*" + _HOST_CALL + r"\s*\)", blanked):
        consider(m.end(), blanked.find("host_is_foreign", m.start()))
    for loc in _HOST_LOCAL.finditer(blanked):
        if not _written_once(blanked, loc.group(1)):
            continue
        for m in re.finditer(r"\b(?:else\s+)?if\s*\(\s*" + re.escape(loc.group(1)) +
                             r"\s*\)", blanked[loc.end():]):
            consider(loc.end() + m.end(), loc.end() + m.start())
    return sorted(out)


def host_refusal_ifs(body: str) -> list:
    """The offsets of host_refusals(body)."""
    return [at for at, _ in host_refusals(body)]


def host_decision_args(body: str, fns=HOST_DECISIONS):
    """([offset], [(fn, first_arg)]) over every call of a HOST_DECISIONS
    function in `body`. The first list holds, for each call whose FIRST
    argument is the Host verdict — `host_is_foreign(req)` itself, or a
    `const bool` local initialized from exactly that and declared before
    the call — the offset where the Host is read. The second list holds
    every call given anything else (a literal, a negation, a local that
    is not the Host, a mutable, shadowed or reassigned local)."""
    blanked = blank_string_contents(body)
    good, bad = [], []
    for fn in fns:
        for off, args in call_args(body, fn):
            first = args[0] if args else ""
            if re.fullmatch(_HOST_CALL, first):
                good.append(blanked.find("host_is_foreign", off))
                continue
            locs = [m for m in _HOST_LOCAL.finditer(blanked[:off]) if m.group(1) == first]
            if locs and _written_once(blanked, first):
                good.append(locs[-1].start() + locs[-1].group(0).find("host_is_foreign"))
            else:
                bad.append((fn, first))
    return good, bad


def first_host_decision(body: str) -> int:
    """Offset in `body` where the Host is first decided, or -1: an
    `if (host_is_foreign(req))` refusal that returns (host_refusal_ifs), or
    the Host verdict handed to a host-tested decision as its first argument
    (host_decision_args). A bare call, a local nobody reads, and an if that
    only logs are not a Host check."""
    found = host_refusal_ifs(body) + host_decision_args(body)[0]
    return min(found) if found else -1


# The receipt dispatch: the verdict bound once, the receipt sent only under
# an explicit SERVE_* test, and the function ending in the Host refusal.
_RECEIPT_BIND = re.compile(r"\bconst\s+(?:[\w:]*::)?ReceiptVerdict\s+([A-Za-z_]\w*)\s*=\s*"
                           r"(?:[\w:]*::)?receipt_decide\s*\(")


def _serve_condition(cond: str, verdict: str) -> bool:
    """True when `cond` is `verdict == ReceiptVerdict::SERVE_BEARER` or
    `... SERVE_TAP`, or a disjunction of exactly those."""
    one = (r"\(?\s*" + re.escape(verdict) +
           r"\s*==\s*(?:[\w:]*::)?ReceiptVerdict::SERVE_(?:BEARER|TAP)\s*\)?")
    return re.fullmatch(r"\s*" + one + r"(?:\s*\|\|\s*" + one + r")*\s*", cond) is not None


def _under_serve_test(blanked: str, off: int, verdict: str) -> bool:
    """True when the call at `off` runs only inside an `if (<serve>)`: either
    it is that if's unbraced statement, or some block enclosing it is opened
    by that if (an `else` block is not)."""
    stmt = max(blanked.rfind(";", 0, off), blanked.rfind("{", 0, off),
               blanked.rfind("}", 0, off)) + 1
    m = re.compile(r"\s*(?:else\s+)?if\s*\(").match(blanked, stmt)
    if m:
        o = m.end() - 1
        c = _paren_close(blanked, o)
        if (c > 0 and _serve_condition(blanked[o + 1:c], verdict)
                and re.fullmatch(r"\s*(?:return)?\s*", blanked[c + 1:off])):
            return True
    depth = 0
    for k in range(off - 1, -1, -1):
        ch = blanked[k]
        if ch == "}":
            depth += 1
        elif ch == "{":
            if depth:
                depth -= 1
                continue
            j = k - 1
            while j >= 0 and blanked[j].isspace():
                j -= 1
            if j < 0 or blanked[j] != ")":
                continue
            o = _paren_open(blanked, j)
            if o < 0:
                continue
            if (re.search(r"\bif\s*$", blanked[max(0, o - 12):o])
                    and _serve_condition(blanked[o + 1:j], verdict)):
                return True
    return False


def check_receipt_dispatch(body: str, where: str) -> list:
    """A function that calls receipt_decide fails closed: the verdict is a
    `const ReceiptVerdict` local, every receipt it sends sits under an
    explicit SERVE_BEARER / SERVE_TAP test, and after the last send, at
    the function's own level and reachable, it answers the Host refusal —
    so REFUSE_HOST, a dropped branch or a verdict added later refuses."""
    failures = []
    blanked = blank_string_contents(body)
    calls = call_args(body, "receipt_decide")
    if not calls:
        return failures
    binds = list(_RECEIPT_BIND.finditer(blanked))
    if len(binds) != 1 or len(calls) != 1:
        return [f"{where}: receipt_decide's verdict must be bound once, as "
                f"`const ReceiptVerdict <v> = receipt_decide(...)`, and tested — "
                f"found {len(calls)} call(s) and {len(binds)} binding(s)"]
    verdict = binds[0].group(1)
    writes = re.findall(r"\b" + re.escape(verdict) + r"\s*=(?!=)", blanked)
    if len(writes) != 1:
        failures.append(f"{where}: `{verdict}` is written {len(writes)} times — the "
                        f"receipt_decide verdict is bound once and never shadowed "
                        f"or reassigned")
    sends = [off for off, _ in call_args(body, "send_provisioning_receipt")]
    for off in sends:
        if not _under_serve_test(blanked, off, verdict):
            failures.append(f"{where}: sends the receipt outside an explicit "
                            f"`if ({verdict} == ReceiptVerdict::SERVE_BEARER)` / "
                            f"`SERVE_TAP` test — every other verdict (REFUSE_HOST "
                            f"above all) must refuse, so a dropped or mistyped "
                            f"branch cannot serve")
    open_brace = blanked.find("{")
    tails = []
    for off, _ in call_args(body, "send_host_refusal"):
        depth = blanked[open_brace:off].count("{") - blanked[open_brace:off].count("}")
        if (depth == 1 and _statement_prefix(blanked, off) == ""
                and off > max(sends, default=-1) and not returns_before(body, off)):
            tails.append(off)
    if not tails:
        failures.append(f"{where}: does not end in the Host refusal — after the last "
                        f"receipt send, at the function's own level, "
                        f"send_host_refusal(req) must answer REFUSE_HOST and every "
                        f"verdict the tests above do not name")
    return failures


def steps_before(body: str, limit: int, steps=PRE_HOST_STEPS) -> list:
    """The PRE_HOST_STEPS that occur in `body` before offset `limit`."""
    blanked = blank_string_contents(body)
    out = []
    for step in steps:
        i = blanked.find(step)
        # `auth_check(` must not match inside `bearer_auth_check(` and friends.
        while i > 0 and (blanked[i - 1].isalnum() or blanked[i - 1] == "_"):
            i = blanked.find(step, i + 1)
        if 0 <= i < limit:
            out.append(step[:-1])
    return out


def check_host_first(sources: dict, require_defs: bool = True) -> list:
    """The Host is asked FIRST on every path that can hand out the token or
    spend the BOOT tap (see the module docstring)."""
    failures = []
    routes = []
    for fname, text in sources.items():
        routes.extend(parse_routes(fname, text))
    all_spans = {fname: function_spans(text) for fname, text in sources.items()}

    def body_of(name):
        for fname, text in sorted(sources.items()):
            span = definition_span(text, name)
            if span:
                return fname, text[span[0]:span[1]]
        return None, None

    # 0. host_is_foreign's own body is GLUE_DEFS's (check_bearer_only_answers).

    # 1. auth_gate refuses a foreign Host before it reads or compares the token.
    fname, body = body_of("auth_gate")
    if body is None:
        if require_defs:
            failures.append("auth_gate definition not found — parser regression?")
    else:
        refusals = [at for at, branch in host_refusals(body)
                    if re.search(r"\breturn\s+false\s*;", branch)
                    and not re.search(r"\breturn\s+(?!false\b)", branch)]
        if not refusals:
            failures.append(f"{fname}: auth_gate no longer refuses a foreign Host "
                            f"(if (host_is_foreign(req)) {{ ... return false; }}) — every "
                            f"auth_gate route would serve under a rebound name")
        else:
            late = steps_before(body, refusals[0],
                                ("auth_get_token(", "auth_check(", "auth_bearer_grant("))
            if late:
                failures.append(f"{fname}: auth_gate reads the token ({', '.join(late)}) "
                                f"before its Host check")

    # Every host-tested decision is handed the Host verdict itself.
    for fname, text in sorted(sources.items()):
        for name, start, end in all_spans[fname]:
            if name in HOST_DECISIONS:
                continue
            for fn, first in host_decision_args(text[start:end])[1]:
                failures.append(f"{fname}: {name} gives {fn} {first!r} as its Host "
                                f"verdict — the first argument must be "
                                f"host_is_foreign(req), or a `const bool` local set from "
                                f"exactly that")

    # 2. Every gated route reaches the Host check before any credential step:
    #    through auth_gate, or by asking it itself. A public route reaches no
    #    credential step at all except the page path (rule 3).
    seen = set()
    for uri, method, handler, fname in routes:
        base = handler.split("::")[-1]
        if (method, uri, base) in seen or uri is None:
            continue
        seen.add((method, uri, base))
        bodies = [b for b in (function_body(t, base) for t in sources.values()) if b]
        if not bodies:
            continue  # main() already names a missing handler
        body = bodies[0]
        if (method, uri) in PUBLIC_ALLOWLIST:
            steps = steps_before(body, len(body),
                                 tuple(st for st in PRE_HOST_STEPS if st != "send_html_with_token("))
            if steps:
                failures.append(f"{method} {uri}: public handler {handler} reaches "
                                f"{', '.join(steps)} — a public route may hand out the "
                                f"token only through send_html_with_token(..., "
                                f"page_token_inject(req))")
            continue
        via_gate = first_deciding_marker(body, "auth_gate(")
        via_host = first_host_decision(body)
        covered = [i for i in (via_gate, via_host) if i >= 0]
        if not covered:
            failures.append(f"{method} {uri}: handler {handler} ({fname}) never asks the "
                            f"Host — run auth_gate, or decide host_is_foreign(req) before "
                            f"the bearer, the tap and the token (the receipt route's shape)")
            continue
        late = steps_before(body, min(covered))
        if late:
            failures.append(f"{method} {uri}: handler {handler} ({fname}) runs "
                            f"{', '.join(late)} before the Host check — a foreign Host "
                            f"must be refused before the bearer is read or the tap is "
                            f"spent")

    # 3. The page path: page_token_inject asks the Host before any grant and
    #    before the tap, and hands that verdict to page_token_decide.
    fname, body = body_of("page_token_inject")
    if body is None:
        if require_defs:
            failures.append("page_token_inject definition not found — parser regression?")
    else:
        fed = host_decision_args(body, ("page_token_decide",))[0]
        h = min(fed) if fed else -1
        if h < 0:
            failures.append(f"{fname}: page_token_inject does not decide the Host first "
                            f"(host_is_foreign(req) as page_token_decide's first "
                            f"argument) — a page load under a foreign Host would spend "
                            f"the owner's BOOT tap")
        else:
            late = steps_before(body, h)
            if late:
                failures.append(f"{fname}: page_token_inject runs {', '.join(late)} "
                                f"before the Host check")

    # 4. send_html_with_token streams the token only on PageToken::INJECT:
    #    `const char* token = (<verdict> == PageToken::INJECT) ? auth_get_token()
    #    : "";`, <verdict> being its PageToken parameter, never reassigned.
    fname, body = body_of("send_html_with_token")
    if body is None:
        if require_defs:
            failures.append("send_html_with_token definition not found — parser regression?")
    else:
        blanked = blank_string_contents(body)
        brace = blanked.find("{")
        param = re.search(r"\bPageToken\s+([A-Za-z_]\w*)\s*\)\s*$", blanked[:brace])
        if not param:
            failures.append(f"{fname}: send_html_with_token no longer takes the "
                            f"page-token verdict (a PageToken, last parameter)")
        else:
            v = re.escape(param.group(1))
            if re.search(r"\b" + v + r"\s*=(?!=)", blanked[brace:]):
                failures.append(f"{fname}: send_html_with_token reassigns its verdict")
            shape = re.compile(r"const\s+char\s*\*\s*\w+\s*=\s*\(?\s*" + v +
                               r"\s*==\s*(?:[\w:]*::)?PageToken::INJECT\s*\)?\s*\?\s*"
                               r"auth_get_token\s*\(\s*\)\s*:\s*\"\"")
            for m in re.finditer(r"\bauth_get_token\s*\(", blanked):
                stmt_start = max(blanked.rfind(";", 0, m.start()),
                                 blanked.rfind("{", 0, m.start()),
                                 blanked.rfind("}", 0, m.start())) + 1
                stmt_end = blanked.find(";", m.start())
                if not shape.fullmatch(blanked[stmt_start:stmt_end].strip()):
                    failures.append(f"{fname}: send_html_with_token reads auth_get_token() "
                                    f"outside a PageToken::INJECT test (`const char* token "
                                    f"= ({param.group(1)} == PageToken::INJECT) ? "
                                    f"auth_get_token() : \"\"`) — only that verdict (Host "
                                    f"first, host-tested) may put the token in a page")

    # 5. Every call of an emitter (other than the page's, rule 3) sits behind
    #    the Host check in its own function. (Which functions may read a
    #    credential at all is check_bearer_only_answers's reader rule.)
    for fname, text in sorted(sources.items()):
        spans = all_spans[fname]
        for emitter in TOKEN_EMITTERS:
            if emitter == "send_html_with_token":
                continue
            for off, _ in call_args(text, emitter):
                enc = enclosing_function(spans, off)
                if enc is None:
                    continue  # a declaration
                name, start, end = enc
                body = text[start:end]
                covered = [i for i in (first_deciding_marker(body, "auth_gate("),
                                       first_host_decision(body)) if i >= 0]
                rel = off - start
                line = text.count("\n", 0, off) + 1
                if not covered or min(covered) > rel:
                    failures.append(f"{fname}:{line}: {name} calls {emitter}, which emits "
                                    f"{TOKEN_EMITTERS[emitter][1]}, without deciding "
                                    f"host_is_foreign(req) (or auth_gate) first")

    # 6. Every function that asks receipt_decide serves only on SERVE_* and
    #    ends in the Host refusal (check_receipt_dispatch).
    for fname, text in sorted(sources.items()):
        for name, start, end in all_spans[fname]:
            if name in HOST_DECISIONS or "receipt_decide" not in text[start:end]:
                continue
            failures.extend(check_receipt_dispatch(text[start:end], f"{fname}: {name}"))

    if require_defs:
        for table in (TOKEN_EMITTERS, TOKEN_COMPARERS):
            for name in table:
                if not any(definition_span(t, name) for t in sources.values()):
                    failures.append(f"{name} is listed as a token reader but is not "
                                    f"defined — remove or fix the entry")
    return failures


# ── The bearer-only answers ───────────────────────────────────────────────────
#
# check_bearer_only_answers() and its helpers are named for the gates they
# hold, not for what the gates protect: CodeQL's
# py/clear-text-logging-sensitive-data reads a call of a function (or a
# local) whose name says "secret", "password", "cert" or "lock" as a
# credential, and main() prints what these return. This script reads C++
# source text and never holds a credential, so that would be a false alert
# on every run (the canary-wap checker's #1832 lesson). Keep those words out
# of every function and local name below.


def _fold(text: str) -> str:
    return " ".join(text.split())


def _in_file(fname: str, suffix: str) -> bool:
    return fname == suffix or fname.endswith("/" + suffix)


def _kinds(kinds) -> str:
    return ", ".join(SECRET_NAMES[k] for k in SECRET_NAMES if k in kinds) or "nothing"


def _line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def _innermost(defs, pos: int):
    """The innermost function_defs() entry whose span holds `pos`, or None."""
    best = None
    for d in defs:
        if d[1] <= pos < d[4] and (best is None or d[1] > best[1]):
            best = d
    return best


def _statement_at(blanked: str, lo: int, hi: int, pos: int, whole_if: bool = False):
    """(start, end, level) of the statement holding `pos`, scanning the code
    from `lo` to `hi` (a function's code, or a whole file): it runs from just
    after the last `;`, `{` or `}` at the level of the innermost brace block
    holding `pos` (parentheses and brackets nest inside a statement; a
    lambda's braces open a block of their own) to the first `;` or `{` at
    that level, inclusive. With `whole_if`, a statement that is an `if` /
    `else if` header runs on through its braced branch. `level` is that
    block's nesting from `lo` (1 is a function body's own level)."""
    stack, starts = [], {0: lo}
    for k in range(lo, pos):
        c = blanked[k]
        if c in "([{":
            stack.append(c)
            if c == "{":
                starts[len(stack)] = k + 1
        elif c in ")]}":
            if stack:
                stack.pop()
            if c == "}":
                starts[len(stack)] = k + 1
        elif c == ";":
            starts[len(stack)] = k + 1
    level = max((i + 1 for i, c in enumerate(stack) if c == "{"), default=0)
    start = starts.get(level, lo)
    while start < pos and (blanked[start].isspace() or blanked[start] == "#"):
        if blanked[start] == "#":  # a preprocessor line before the statement is not of it
            nl = blanked.find("\n", start)
            start = pos if nl < 0 or nl > pos else nl + 1
        else:
            start += 1
    depth, k = len(stack), pos
    while k < hi:
        c = blanked[k]
        if depth == level and c in ";{}":
            break
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        k += 1
    if k >= hi or blanked[k] == "}":
        return start, k, level
    if (blanked[k] == "{" and whole_if
            and re.match(r"(?:else\s+)?if\s*\(", blanked[start:k])):
        close = _bracket_close(blanked, k)
        return start, (close + 1 if close >= 0 else hi), level
    return start, k + 1, level


def _outside_statement(text: str, blanked: str, pos: int):
    """(start, end, folded) of the statement holding `pos` outside every
    function (a declaration): from just after the nearest `;`, `{` or `}`
    before it to the `;` that ends it."""
    lo = max(blanked.rfind(c, 0, pos) for c in ";{}") + 1
    start, end, _level = _statement_at(blanked, lo, len(blanked), pos)
    return start, end, _fold(text[start:end])


def _function_index(sources: dict):
    """({(fname, qualified name): def}, {bare name: [keys]}) over every
    function_defs() definition (the first, when a file defines one twice)."""
    defs, by_name = {}, {}
    for fname, text in sorted(sources.items()):
        for d in function_defs(text):
            key = (fname, d[0])
            if key not in defs:
                defs[key] = d
                by_name.setdefault(d[0].split("::")[-1], []).append(key)
    return defs, by_name


_ANY_CALL = re.compile(r"(?<![\w~])([A-Za-z_]\w*)\s*\(")


def _reads_in(sources: dict, defs: dict, reads_tbl: dict):
    """({key: {kind: [pos]}}, {key: {called bare names}}, [(fname, pos, kind)]
    for each read outside every function). A call by bare name is any
    `name(`: free, member (`net.begin(`) or qualified (`ns::name(`)."""
    reads, calls, outside = {}, {}, []
    per_file = {}
    for key, d in defs.items():
        per_file.setdefault(key[0], []).append(d)
    for fname, text in sorted(sources.items()):
        blanked = blank_string_contents(text)
        fdefs = per_file.get(fname, [])
        for kind, rx in reads_tbl.items():
            for m in rx.finditer(blanked):
                d = _innermost(fdefs, m.start())
                if d is None:
                    outside.append((fname, m.start(), kind))
                elif m.start() != d[2]:  # not the definition's own name
                    reads.setdefault((fname, d[0]), {}).setdefault(kind, []).append(m.start())
        for d in fdefs:
            calls.setdefault((fname, d[0]), set()).update(
                n for n in _ANY_CALL.findall(blanked[d[3]:d[4]]) if n not in _KEYWORDS)
    return reads, calls, outside


def _carry_map(defs, by_name, reads, calls, stops) -> dict:
    """{key: kinds}: what each function carries — what it reads, and to a
    fixed point what every function it calls carries (a call by bare name
    reaches every definition of that name) — the carry stopping at `stops`."""
    carried = {k: set(reads.get(k, {})) for k in defs}
    changed = True
    while changed:
        changed = False
        for key in defs:
            if key in stops:
                continue
            for name in calls.get(key, ()):
                for g in by_name.get(name, ()):
                    if g != key and g not in stops and not carried[g] <= carried[key]:
                        carried[key] |= carried[g]
                        changed = True
    return carried


def _statements(sources, key, d, positions, T) -> list:
    """[(start, end, folded, kinds)] for the statements of function `d`
    holding the reads at `positions`, in order, one per statement (a mint's
    statement through its branch, SECRET_MINTS)."""
    text = sources[key[0]]
    blanked = blank_string_contents(text)
    mint = (re.compile(r"\b(?:" + "|".join(map(re.escape, T["mints"])) + r")\s*\(")
            if T["mints"] else None)
    out = []
    for pos in sorted(positions):
        if any(a <= pos < b for a, b, _f, _k in out):
            continue
        start, end, _level = _statement_at(blanked, d[3], d[4], pos)
        if mint and mint.search(blanked, start, end):
            start, end, _level = _statement_at(blanked, d[3], d[4], pos, whole_if=True)
        kinds = {k for k, rx in T["reads"].items() if rx.search(blanked, start, end)}
        out.append((start, end, _fold(text[start:end]), kinds))
    return out


def _answer_sites(code: str, reads_tbl: dict, carriers: set, self_name: str) -> list:
    """(offset, what) for each place `code` (a handler's string-blanked
    body) reaches a credential: a read, or a call of a function that
    carries one."""
    sites = {(m.start(), re.match(r"\w+", m.group(0)).group(0))
             for rx in reads_tbl.values() for m in rx.finditer(code)}
    sites |= {(m.start(), m.group(1)) for m in _ANY_CALL.finditer(code)
              if m.group(1) != self_name and m.group(1) in carriers}
    return sorted(sites)


def _reader_problems(sources, T, defs, reads, carried, outside, handlers) -> list:
    """(c) Every function that reads a credential is named: an accessor in
    its home, an emitter with exactly its carries, a comparer reading only
    the token, a local use (its statements: _local_use_problems) or a
    SECRET_ANSWERS handler (_answer_problems). Outside every function only
    OUTSIDE_FUNCTION_LINES may name one."""
    problems = []
    accessors = T["accessors"]
    for name, (home, _kind) in sorted(accessors.items()):
        homes = sorted({f for f, q in defs if q.split("::")[-1] == name})
        if not homes or any(not _in_file(f, home) for f in homes):
            where = ", ".join(homes) or "no file the check reads"
            problems.append(f"SECRET_ACCESSORS: {name}() is defined in {where}; an accessor is "
                            f"defined once, in {home} — remove or fix the row")
    for key in sorted(reads):
        fname, qual = key
        got = set(reads[key])
        bare = qual.split("::")[-1]
        line = _line_of(sources[fname], defs[key][1])
        if bare in accessors and _in_file(fname, accessors[bare][0]):
            if got - {accessors[bare][1]}:
                problems.append(f"{fname}:{line}: accessor {qual}() reads {_kinds(got)}; it is "
                                f"the accessor of {_kinds({accessors[bare][1]})} only")
        elif bare in T["emitters"]:
            want = set(T["emitters"][bare][0].split())
            if carried[key] != want:
                problems.append(f"{fname}:{line}: emitter {qual}() carries "
                                f"{_kinds(carried[key])}; its TOKEN_EMITTERS entry says "
                                f"{_kinds(want)}")
        elif bare in T["comparers"]:
            if got != {"token"}:
                problems.append(f"{fname}:{line}: comparer {qual}() reads {_kinds(got)}; a "
                                f"TOKEN_COMPARERS entry reads the bearer token only, to "
                                f"compare a presented one")
        elif not (any(_in_file(fname, f) and q == qual for f, q in T["local"])
                  or bare in T["answers"]):
            problems.append(f"{fname}:{line}: {qual}() reads {_kinds(got)} and is no "
                            f"TOKEN_EMITTERS, TOKEN_COMPARERS, SECRET_LOCAL_USES or "
                            f"SECRET_ANSWERS entry: name it, and if it answers, put it "
                            f"behind a bearer-only grant")
    for table, label in ((T["emitters"], "TOKEN_EMITTERS"), (T["comparers"], "TOKEN_COMPARERS")):
        for name in sorted(table):
            if not any(k[1].split("::")[-1] == name for k in reads):
                problems.append(f"{label} names {name}(), which reads no credential — "
                                f"remove it")
    seen = set()
    for fname, pos, _kind in sorted(outside):
        if (fname, pos) in seen:
            continue
        text = sources[fname]
        start, end, stmt = _outside_statement(text, blank_string_contents(text), pos)
        seen.update((fname, p) for f, p, _k in outside if f == fname and start <= p < end)
        if not any(_in_file(fname, f) and stmt == s for f, s in T["outside"]):
            problems.append(f"{fname}:{_line_of(text, pos)}: a credential named outside every "
                            f"function ({stmt[:90]!r}): the rule reads credentials per "
                            f"function, so only OUTSIDE_FUNCTION_LINES may")
    for suffix, stmt in T["outside"]:
        hit = False
        for fname, pos, _kind in outside:
            if _in_file(fname, suffix):
                text = sources[fname]
                hit = hit or _outside_statement(text, blank_string_contents(text), pos)[2] == stmt
        if not hit:
            problems.append(f"OUTSIDE_FUNCTION_LINES holds {stmt!r} ({suffix}), which no "
                            f"longer names a credential there — remove or fix the row")
    return problems


def _local_use_problems(sources, T, defs, reads, handlers) -> list:
    """Each SECRET_LOCAL_USES row: defined where it says, reading a
    credential, not a route handler, and its reading statements exactly the
    row's."""
    problems = []
    for (suffix, qual), (_why, want) in sorted(T["local"].items()):
        keys = [k for k in defs if _in_file(k[0], suffix) and k[1] == qual]
        if not keys:
            problems.append(f"SECRET_LOCAL_USES names {qual}() in {suffix}, which does not "
                            f"define it — remove or fix the row")
            continue
        for key in keys:
            fname = key[0]
            text = sources[fname]
            if qual.split("::")[-1] in handlers:
                problems.append(f"{fname}: {qual}() is a route handler, so it answers: not a "
                                f"SECRET_LOCAL_USES entry")
            if not reads.get(key):
                problems.append(f"SECRET_LOCAL_USES names {qual}() in {suffix}, which reads no "
                                f"credential — remove it")
                continue
            got = _statements(sources, key, defs[key],
                              [p for ps in reads[key].values() for p in ps], T)
            folds = tuple(f for _a, _b, f, _k in got)
            if folds == tuple(want):
                continue
            for start, _end, stmt, kinds in got:
                if stmt not in want:
                    problems.append(f"{fname}:{_line_of(text, start)}: {qual}() reads "
                                    f"{_kinds(kinds)} in a statement not in its "
                                    f"SECRET_LOCAL_USES row: {stmt!r}")
            for stmt in want:
                if stmt not in folds:
                    problems.append(f"{fname}: {qual}()'s SECRET_LOCAL_USES row holds {stmt!r}, "
                                    f"which it no longer has (a deleted call, a discarded "
                                    f"result or a dropped refusal fails here)")
            problems.append(f"{fname}: {qual}()'s statements that read a credential are not "
                            f"its SECRET_LOCAL_USES row. If the change is meant, update the "
                            f"row in the same review. Now: {folds!r}")
    return problems


def _refusal_end(code: str, blanked: str, lo: int, hi: int) -> int:
    """End offset of the first BEARER_REFUSAL statement at a function body's
    own level within blanked[lo:hi], or -1."""
    for m in re.finditer(r"\bauth_gate\s*\(", blanked[lo:hi]):
        start, end, level = _statement_at(blanked, lo, hi, lo + m.start())
        if level == 1 and _fold(code[start:end]) == BEARER_REFUSAL:
            return end
    return -1


def _answer_problems(sources, T, defs, by_name, reads, carried, stops, routes) -> list:
    """(d) Every registered handler that carries a credential is a
    SECRET_ANSWERS row with exactly that set, and (e) its grant holds."""
    problems = []
    handlers = {h.split("::")[-1] for _u, _m, h, _f in routes}
    public = {h.split("::")[-1] for u, m, h, _f in routes if (m, u) in T["public"]}
    carriers = {k[1].split("::")[-1] for k, s in carried.items() if s and k not in stops}

    def carries(h):
        got = set()
        for key in by_name.get(h, ()):
            if key not in stops:
                got |= carried[key]
        return got

    for h in sorted(handlers):
        got = carries(h)
        if not got or h in T["answers"]:
            continue
        if h in public:
            problems.append(f"public route handler {h}() answers {_kinds(got)}: a credential's "
                            f"answer takes a bearer-only grant (SECRET_ANSWERS), never none")
        else:
            problems.append(f"route handler {h}() answers {_kinds(got)} and is not in "
                            f"SECRET_ANSWERS: name it there, behind a bearer-only grant")
    pages = {h for h, (_c, grant) in T["answers"].items() if grant == "page"}
    for h, (named, grant) in sorted(T["answers"].items()):
        if grant not in ("bearer", "receipt", "page"):
            problems.append(f"SECRET_ANSWERS gives {h}() the grant {grant!r}; a grant is "
                            f"'bearer', 'receipt' or 'page'")
            continue
        if h not in handlers:
            problems.append(f"SECRET_ANSWERS names {h}(), which is no registered route "
                            f"handler — remove or fix the row")
            continue
        got = carries(h)
        if set(named.split()) != got:
            problems.append(f"{h}() carries {_kinds(got)}; its SECRET_ANSWERS entry says "
                            f"{_kinds(named.split())}")
        if h in public and grant != "page":
            problems.append(f"{h}() is a PUBLIC_ALLOWLIST route handler with the {grant!r} "
                            f"grant: a public route may carry a credential only through the "
                            f"page path")
        for key in by_name.get(h, ()):
            fname, qual = key
            if key in stops or qual != h:
                continue
            text = sources[fname]
            blanked = blank_string_contents(text)
            d = defs[key]
            code = blanked[d[3]:d[4]]
            sites = _answer_sites(code, T["reads"], carriers, h)

            def where(off, fname=fname, text=text, d=d):
                return f"{fname}:{_line_of(text, d[3] + off)}: {h}()"
            if grant == "bearer":
                end = _refusal_end(text, blanked, d[3], d[4])
                if end < 0:
                    problems.append(f"{fname}: {h}() answers {_kinds(got)} with no "
                                    f"`{BEARER_REFUSAL}` at its own top level")
                for off, what in sites:
                    if end < 0 or d[3] + off < end:
                        problems.append(where(off) +
                                        f" reaches a credential ({what}) before its refusal "
                                        f"`{BEARER_REFUSAL}`")
            elif grant == "receipt":
                for off, args in call_args(text[d[3]:d[4]], "receipt_decide"):
                    given = tuple(_fold(a) for a in args[1:])
                    if given != tuple(T["receipt"]):
                        problems.append(where(off) +
                                        f" hands receipt_decide the grants {given!r} — they "
                                        f"are exactly {tuple(T['receipt'])!r}: the bearer "
                                        f"grant takes a real bearer, the tap is the tap")
                bind = _RECEIPT_BIND.search(code)
                for off, what in sites:
                    if not (bind and _under_serve_test(code, off, bind.group(1))):
                        problems.append(where(off) +
                                        f" reaches a credential ({what}) outside a SERVE_* "
                                        f"test of its receipt_decide verdict")
            else:
                for off, what in sites:
                    if what != "send_html_with_token" and what not in pages:
                        problems.append(where(off) +
                                        f" reaches a credential through {what}, not the page "
                                        f"path (send_html_with_token(..., "
                                        f"page_token_inject(req)) or another page handler)")
    return problems


def glue_def_problems(sources: dict, glue: dict) -> list:
    """Every GLUE_DEFS row: its function defined once in its file, its body
    exactly the row's."""
    problems = []
    for (suffix, qual), (why, want) in sorted(glue.items()):
        found = [(f, sp) for f, t in sorted(sources.items()) if _in_file(f, suffix)
                 for sp in definition_spans(t, qual)]
        if len(found) != 1:
            problems.append(f"GLUE_DEFS pins {qual}() in {suffix}, which defines it "
                            f"{len(found)} times — remove or fix the row")
            continue
        fname, (a, b) = found[0]
        got = _fold(sources[fname][a:b])
        if got != want:
            problems.append(f"{fname}: {qual} changed — {why}. If the change is meant, update "
                            f"its GLUE_DEFS row in this script in the same review. Now: {got}")
    return problems


def gate_census_problems(sources: dict, census: dict, markers, steps, emitters) -> list:
    """GATE_CALL_SITES: every call of a gate primitive is where its row says;
    every AUTH_MARKERS and PRE_HOST_STEPS name is a row."""
    problems = []
    for marker in markers:
        if marker.rstrip("(") not in census:
            problems.append(f"AUTH_MARKERS entry {marker!r} names no GATE_CALL_SITES row — a "
                            f"gate marker is a census primitive (a deleted gate's name "
                            f"cannot linger)")
    for step in steps:
        name = step.rstrip("(")
        if name not in census and name not in emitters and name != "auth_get_token":
            problems.append(f"PRE_HOST_STEPS entry {step!r} names no GATE_CALL_SITES row, "
                            f"TOKEN_EMITTERS key or auth_get_token")
    defs_of = {f: function_defs(t) for f, t in sources.items()}
    for name, row in sorted(census.items()):
        rx = re.compile(row.get("pattern") or
                        (r"\b" + re.escape(name) + (r"\b" if row.get("ident") else r"\s*\(")))
        callers = row.get("callers")
        seen, count = set(), 0
        for fname, text in sorted(sources.items()):
            if row.get("file") and not _in_file(fname, row["file"]):
                continue
            blanked = blank_string_contents(text)
            for m in rx.finditer(blanked):
                d = _innermost(defs_of[fname], m.start())
                if d is not None and m.start() == d[2] and d[0].split("::")[-1] == name:
                    continue  # the primitive's own definition
                line = _line_of(text, m.start())
                if d is None or m.start() < d[3]:
                    stmt = _outside_statement(text, blanked, m.start())[2]
                    if row.get("ident"):
                        if stmt not in row.get("decl", ()):
                            problems.append(f"{fname}:{line}: {name} named outside every "
                                            f"function other than in its declaration: {stmt!r}")
                    elif not re.search(r"[\w*&>]\s*$", blanked[max(0, m.start() - 40):m.start()]):
                        problems.append(f"{fname}:{line}: {name}() called outside every "
                                        f"function: {stmt!r}")
                    continue
                count += 1
                key = next(((f, q) for f, q in (callers or ())
                            if _in_file(fname, f) and q == d[0]), None)
                if callers is not None and key is None:
                    problems.append(f"{fname}:{line}: {name} called from {d[0]}, which its "
                                    f"GATE_CALL_SITES row does not name — a gate primitive is "
                                    f"called only where the census says")
                    continue
                seen.add(key)
                if row.get("whole"):
                    start, end, _lv = _statement_at(blanked, d[3], d[4], m.start())
                    if _fold(text[start:end]) != row["whole"]:
                        problems.append(f"{fname}:{line}: {d[0]} calls {name} other than as "
                                        f"the whole statement `{row['whole']}`: "
                                        f"{_fold(text[start:end])!r}")
                if key in row.get("served", ()):
                    code = blanked[d[3]:d[4]]
                    bind = _RECEIPT_BIND.search(code)
                    if not (bind and _under_serve_test(code, m.start() - d[3], bind.group(1))):
                        problems.append(f"{fname}:{line}: {d[0]} calls {name} outside a "
                                        f"SERVE_* test of its receipt_decide verdict — only "
                                        f"the tap that served hands the asker the token")
        if count == 0:
            problems.append(f"GATE_CALL_SITES row {name} has no call under ROOTS — remove or "
                            f"fix the row")
        if row.get("once") and count > 1:
            problems.append(f"{name} is called {count} times; its GATE_CALL_SITES row says "
                            f"exactly once")
        for f, q in sorted((callers or set()) - seen):
            problems.append(f"GATE_CALL_SITES row {name} names {q} ({f}), which no longer "
                            f"calls it — remove it")
    return problems


def no_session_problems(sources: dict, headers: dict, queries: set) -> list:
    """The premise: no cookie, request headers only REQUEST_HEADERS', no
    `Set-Cookie`, a URL query only in URL_QUERY_READERS."""
    problems = []
    for fname, text in sorted(sources.items()):
        blanked = blank_string_contents(text)
        defs = function_defs(text)
        allowed = next((names for suffix, names in headers.items() if _in_file(fname, suffix)),
                       ())
        for m in re.finditer(r"\bhttpd_req_get_cookie_val\s*\(", blanked):
            problems.append(f"{fname}:{_line_of(text, m.start())}: reads a cookie "
                            f"(httpd_req_get_cookie_val) — {_NO_SESSION}")
        for fn in ("httpd_req_get_hdr_value_len", "httpd_req_get_hdr_value_str"):
            for off, args in call_args(text, fn):
                hdr = args[1] if len(args) > 1 else ""
                lit = re.fullmatch(r'"([^"\\]*)"', hdr)
                if not lit or lit.group(1) not in allowed:
                    problems.append(f"{fname}:{_line_of(text, off)}: reads the request "
                                    f"header {hdr!r}, a header not in REQUEST_HEADERS for this "
                                    f"file ({', '.join(allowed) or 'none'}) — {_NO_SESSION}; a "
                                    f"lockout keyed on a request header (X-Forwarded-For) "
                                    f"is the client's to write")
        for off, args in call_args(text, "httpd_resp_set_hdr"):
            hdr = args[1] if len(args) > 1 else ""
            lit = re.fullmatch(r'"([^"\\]*)"', hdr)
            if not lit or lit.group(1).lower() == "set-cookie":
                problems.append(f"{fname}:{_line_of(text, off)}: sets the response header "
                                f"{hdr!r} (a header name is a literal, never Set-Cookie) — "
                                f"{_NO_SESSION}")
        for m in _URL_QUERY.finditer(blanked):
            d = _innermost(defs, m.start())
            if d is None or not any(_in_file(fname, f) and q == d[0] for f, q in queries):
                who = d[0] if d else "code outside every function"
                problems.append(f"{fname}:{_line_of(text, m.start())}: {who} reads the URL "
                                f"query ({m.group(1)}), outside URL_QUERY_READERS — "
                                f"{_NO_SESSION}")
    return problems


def check_bearer_only_answers(sources: dict, tables=None) -> list:
    """Every answer that carries a credential holds to its grant, on the
    flagship's own gates (see the module docstring). `tables` overrides the
    module tables by name (the self-test's fixtures pass their own)."""
    T = {"reads": SECRET_READS, "accessors": SECRET_ACCESSORS, "mints": SECRET_MINTS,
         "emitters": TOKEN_EMITTERS, "comparers": TOKEN_COMPARERS,
         "local": SECRET_LOCAL_USES, "answers": SECRET_ANSWERS,
         "outside": OUTSIDE_FUNCTION_LINES, "glue": GLUE_DEFS, "census": GATE_CALL_SITES,
         "receipt": RECEIPT_GRANTS, "headers": REQUEST_HEADERS, "queries": URL_QUERY_READERS,
         "public": PUBLIC_ALLOWLIST, "markers": AUTH_MARKERS, "steps": PRE_HOST_STEPS}
    T.update(tables or {})
    routes = []
    for fname, text in sorted(sources.items()):
        routes.extend(parse_routes(fname, text))
    handlers = {h.split("::")[-1] for _u, _m, h, _f in routes}
    defs, by_name = _function_index(sources)
    # (a) the reads and the calls, per (file, qualified function).
    reads, calls, outside = _reads_in(sources, defs, T["reads"])
    # (b) the carry, stopping at the local uses and the comparers.
    stops = {k for k in defs
             if any(_in_file(k[0], f) and k[1] == q for f, q in T["local"])
             or k[1].split("::")[-1] in T["comparers"]}
    carried = _carry_map(defs, by_name, reads, calls, stops)
    return (_reader_problems(sources, T, defs, reads, carried, outside, handlers)   # (c)
            + _local_use_problems(sources, T, defs, reads, handlers)
            + _answer_problems(sources, T, defs, by_name, reads, carried, stops,     # (d), (e)
                               routes)
            + glue_def_problems(sources, T["glue"])
            + gate_census_problems(sources, T["census"], T["markers"], T["steps"],
                                   T["emitters"])
            + no_session_problems(sources, T["headers"], T["queries"]))


def check_registration_sites(sources: dict) -> list:
    """Inside registerHttpHandlers(server) every registration goes to the
    `server` parameter — never a member handle."""
    failures = []
    found = False
    for fname, text in sorted(sources.items()):
        span = definition_span(text, "registerHttpHandlers")
        if not span:
            continue
        found = True
        body = text[span[0]:span[1]]
        for fn in ("register_route", "httpd_register_uri_handler"):
            for off, args in call_args(body, fn):
                if not args or args[0] != "server":
                    line = text.count("\n", 0, span[0] + off) + 1
                    failures.append(
                        f"{fname}:{line}: {fn}({args[0] if args else ''}, ...) inside "
                        f"registerHttpHandlers — use the `server` parameter: the "
                        f"table goes on whichever server is primary, and on "
                        f"FEATURE_HTTPS builds m_http_server is still nullptr here "
                        f"(the route would vanish with only a serial line)")
    if not found:
        failures.append("registerHttpHandlers definition not found — parser regression?")
    return failures


def check_route_budgets(sources: dict) -> list:
    """No function registers more routes than the max_uri_handlers it sets;
    registerHttpHandlers is held to kRouteTableSlots, with and without the
    mesh block. Every #if branch is counted (the worst case), so an #else /
    #elif inside a counted body is refused rather than double-counted."""
    failures = []
    slots_re = re.compile(
        r"#if\s+defined\(FEATURE_MESH_NETWORK\)\s*&&\s*FEATURE_MESH_NETWORK\s*\n"
        r"\s*static\s+const\s+uint16_t\s+kRouteTableSlots\s*=\s*(\d+)\s*;\s*\n"
        r"\s*#else\s*\n"
        r"\s*static\s+const\s+uint16_t\s+kRouteTableSlots\s*=\s*(\d+)\s*;")
    mesh_block_re = re.compile(
        r"#if\s+defined\(FEATURE_MESH_NETWORK\)\s*&&\s*FEATURE_MESH_NETWORK\b(.*?)#endif", re.S)
    checked_table = False
    for fname, text in sorted(sources.items()):
        routes = parse_routes_at(fname, text)
        # Functions that set a literal budget and register in their own body.
        for m in re.finditer(r"max_uri_handlers\s*=\s*(\d+)\s*;", text):
            owner = None
            for d in re.finditer(r"\b(\w+)\s*\([^;{}]*\)\s*\{", text[:m.start()]):
                if d.group(1) not in ("if", "for", "while", "switch", "catch"):
                    owner = d.group(1)
            span = definition_span(text, owner) if owner else None
            if not span or not (span[0] <= m.start() < span[1]):
                continue
            n = sum(1 for off, *_ in routes if span[0] <= off < span[1])
            if n > int(m.group(1)):
                failures.append(f"{fname}: {owner}() registers {n} routes but sets "
                                f"max_uri_handlers = {m.group(1)}")
        span = definition_span(text, "registerHttpHandlers")
        if not span:
            continue
        slots = slots_re.search(text)
        if not slots:
            failures.append(f"{fname}: kRouteTableSlots is not the expected "
                            f"#if FEATURE_MESH_NETWORK / #else pair — teach "
                            f"check_route_budgets the new shape")
            continue
        checked_table = True
        mesh_slots, base_slots = int(slots.group(1)), int(slots.group(2))
        body = text[span[0]:span[1]]
        if re.search(r"^\s*#\s*(?:else|elif)\b", body, re.M):
            failures.append(f"{fname}: registerHttpHandlers has an #else/#elif — the "
                            f"worst-case counter sums every branch; teach it the "
                            f"alternative before adding one")
        total = sum(1 for off, *_ in routes if span[0] <= off < span[1])
        mesh = 0
        for mb in mesh_block_re.finditer(body):
            lo, hi = span[0] + mb.start(), span[0] + mb.end()
            mesh += sum(1 for off, *_ in routes if lo <= off < hi)
        if total > mesh_slots:
            failures.append(
                f"{fname}: registerHttpHandlers registers {total} routes (every "
                f"feature on) but kRouteTableSlots is {mesh_slots} with the mesh "
                f"— raise it (and its #else twin) and the comment above it")
        if total - mesh > base_slots:
            failures.append(
                f"{fname}: registerHttpHandlers registers {total - mesh} routes "
                f"without the mesh block but kRouteTableSlots is {base_slots} "
                f"without the mesh — raise it")
    if not checked_table:
        failures.append("registerHttpHandlers / kRouteTableSlots not found — "
                        "parser regression?")
    return failures


def _assert_parser_robust() -> None:
    """Guard the parser on every run, so a silent false pass cannot creep in.

    1. A handler whose body carries an UNBALANCED brace inside a string
       literal before its auth call must still extract fully (a naive
       matcher truncates there and mis-reports the route's gating).
    2. The probe-loop form must expand to one route per array entry.
    """
    fixture = (
        'esp_err_t handle_demo(httpd_req_t* req) {\n'
        '  httpd_resp_sendstr(req, "{\\"code\\":\\"bad_}\\"}");\n'
        '  if (!auth_gate(req)) return ESP_OK;\n'
        '  return ESP_OK;\n'
        '}\n'
        'esp_err_t handle_other(httpd_req_t* req) { return ESP_OK; }\n'
    )
    body = function_body(fixture, "handle_demo")
    assert body is not None, "fixture handler not found"
    assert "auth_gate(" in body, (
        "brace matcher truncated the body at a string-literal brace — the "
        "auth call was lost (this is the bug this guard prevents)")
    assert "handle_other" not in body, "body over-extended into the next fn"
    fixture2 = (
        "esp_err_t handle_c(httpd_req_t* req) {\n"
        "  char close = '}';\n"
        "  if (!auth_gate(req)) return ESP_OK;\n"
        "  return ESP_OK;\n"
        "}\n"
    )
    b2 = function_body(fixture2, "handle_c")
    assert b2 is not None and "auth_gate(" in b2, "char-literal brace desync"

    loop = (
        'static const char* kPaths[] = { "/a*", "/b*" };\n'
        "for (const char* p : kPaths) {\n"
        "  httpd_uri_t probe = { .uri = p, .method = HTTP_GET, .handler = h };\n"
        "}\n"
        'httpd_uri_t x = { .uri = "/x", .method = HTTP_POST, .handler = hx };\n'
    )
    got = sorted((u, m, h) for u, m, h, _ in parse_routes("fixture", loop))
    assert got == [("/a*", "GET", "h"), ("/b*", "GET", "h"), ("/x", "POST", "hx")], (
        f"probe-loop expansion broke: {got}")

    # 3. A gate whose result is ignored is not a gate.
    for ok in ("if (!auth_gate(req)) return ESP_OK;",
               "} else if (auth_check(req, t)) {",
               "if (a && (b || bearer_present_and_valid(req))) {",
               "return auth_bearer_grant(req, token, request_peer_ipv4(req)) ==\n"
               "       AuthManager::BearerResult::OK;"):
        assert marker_decides("{ " + ok, next(m for m in AUTH_MARKERS if m in ok)), ok
    for bad in ("(void)bearer_present_and_valid(req); return send(req);",
                "(void)auth_bearer_grant(req, t, request_peer_ipv4(req)); return send(req);",
                # The grant's four values say yes only as OK (F340).
                "if (auth_bearer_grant(req, t, request_peer_ipv4(req)) != "
                "AuthManager::BearerResult::WRONG) return send(req);",
                "if (auth_bearer_grant(req, t, request_peer_ipv4(req)) == "
                "AuthManager::BearerResult::NONE) return send(req);",
                "const AuthManager::BearerResult r = auth_bearer_grant(req, t, request_peer_ipv4(req));\n"
                "  if (r != AuthManager::BearerResult::HELD) return send(req);",
                "if (auth_bearer_grant(req, t, request_peer_ipv4(req)) == "
                "AuthManager::BearerResult::OKAY) return send(req);",
                "auth_gate(req); return ESP_OK;",
                "bool ok = auth_gate(req); return send(req);",
                "const bool ok = auth_gate(req); return send(req);",
                "bool ok = auth_gate(req); if (!ok) return ESP_OK;"):
        assert not marker_decides("{ " + bad, next(m for m in AUTH_MARKERS if m in bad)), bad
    # ...a const local the function then tests is the gate deciding...
    assert marker_decides("{ const bool ok = auth_gate(req);\n  if (!ok) return ESP_OK; }",
                          "auth_gate(")
    # ...but a `return` inside a lambda is the lambda deciding, not the
    # handler: a receipt_decide whose verdict is dropped decides nothing.
    dropped = ("{\n  receipt_decide(host_is_foreign(req),\n"
               "      [req]() { return bearer_present_and_valid(req); },\n"
               "      []() mutable -> bool { return provisioning_gate_take(); });\n"
               "  return send_provisioning_receipt(req);\n}")
    assert not any(marker_decides(dropped, m) for m in AUTH_MARKERS), dropped
    # ...and a deciding gate below an unconditional return is unreachable,
    # while an early `if (...) return` (rate limit) or a nested one is fine.
    assert not gate_guards("{\n  (void)auth_gate(req);\n  return send(req);\n"
                           "  if (!auth_gate(req)) return ESP_OK;\n}")
    assert gate_guards("{\n  if (!rate_limit_check(req)) return ESP_OK;\n"
                       "  if (!auth_gate(req)) return ESP_OK;\n  return send(req);\n}")
    assert gate_guards("{\n  if (x) {\n    return a;\n  }\n"
                       "  if (bearer_present_and_valid(req)) {\n    return b;\n  }\n}")

    # 4. Argument splitting survives nested calls and a comma in a string.
    calls = call_args('x(); send_html_with_token(req, pick("a,b", 2), true);\n'
                      'static esp_err_t send_html_with_token(httpd_req_t* req, '
                      'const char* h, bool i) { return ESP_OK; }\n', "send_html_with_token")
    assert [a for _, a in calls] == [["req", 'pick("a,b", 2)', "true"]], calls

    # 5. Registration-site and budget rules see the two sibling-branch shapes.
    table = (
        "#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK\n"
        "static const uint16_t kRouteTableSlots = 3;\n#else\n"
        "static const uint16_t kRouteTableSlots = 2;\n#endif\n"
        "void Mgr::registerHttpHandlers(httpd_handle_t server) {\n"
        '  httpd_uri_t a = { .uri = "/a", .method = HTTP_GET, .handler = ha };\n'
        "  register_route(server, &a);\n"
        '  httpd_uri_t b = { .uri = "/b", .method = HTTP_GET, .handler = hb };\n'
        "  register_route(m_http_server, &b);\n"
        "#if defined(FEATURE_MESH_NETWORK) && FEATURE_MESH_NETWORK\n"
        '  httpd_uri_t c = { .uri = "/c", .method = HTTP_GET, .handler = hc };\n'
        "  register_route(server, &c);\n#endif\n}\n")
    site = check_registration_sites({"fx": table})
    assert len(site) == 1 and "m_http_server" in site[0], site
    assert check_route_budgets({"fx": table}) == [], check_route_budgets({"fx": table})
    over = table.replace("kRouteTableSlots = 3;", "kRouteTableSlots = 2;")
    assert any("with the mesh" in f for f in check_route_budgets({"fx": over})), over

    # 6. The Host first on the receipt: the shapes the rule refuses (each a
    #    review mutation that the Host-first rule alone passed), and the fixed
    #    ones.
    reg = ('httpd_uri_t r = { .uri = "/api/provisioning-receipt", .method = HTTP_GET, '
           '.handler = handle_provisioning_receipt };\n')
    emit = ("static esp_err_t send_provisioning_receipt(httpd_req_t* req) {\n"
            "  doc[\"token\"] = auth_get_token();\n  return ESP_OK;\n}\n")
    def receipt(body):
        return {"fx": reg + emit + "static esp_err_t handle_provisioning_receipt("
                "httpd_req_t* req) {\n" + body + "}\n"}
    def red(body, *needles):
        got = check_host_first(receipt(body), require_defs=False)
        assert any(all(n in f for n in needles) for f in got), (needles, body, got)
    def green(body):
        got = check_host_first(receipt(body), require_defs=False)
        assert got == [], (body, got)
        handler = function_body(receipt(body)["fx"], "handle_provisioning_receipt")
        assert gate_guards(handler), ("main()'s gate rule refuses", body)

    # The if-form: a refusal that returns, then the bearer, then the tap.
    no_host = ("  if (bearer_present_and_valid(req)) return send_provisioning_receipt(req);\n"
               "  if (!provisioning_gate_take()) return ESP_OK;\n"
               "  return send_provisioning_receipt(req);\n")
    red(no_host, "never asks the Host")
    red("  if (bearer_present_and_valid(req)) return send_provisioning_receipt(req);\n"
        "  if (host_is_foreign(req)) return ESP_OK;\n"
        "  if (!provisioning_gate_take()) return ESP_OK;\n"
        "  return send_provisioning_receipt(req);\n",
        "before the Host check", "bearer_present_and_valid")
    # The accounted grant itself, read before the Host (F340): it strikes
    # a wrong bearer against its address, so a rebound page could spend a
    # host's backoff (or test a token) under a name that is not this device.
    red("  if (auth_bearer_grant(req, t, request_peer_ipv4(req)) == "
        "AuthManager::BearerResult::OK)\n"
        "    return send_provisioning_receipt(req);\n"
        "  if (host_is_foreign(req)) return ESP_OK;\n"
        "  if (!provisioning_gate_take()) return ESP_OK;\n"
        "  return send_provisioning_receipt(req);\n",
        "before the Host check", "auth_bearer_grant")
    for not_a_check in (
            "  host_is_foreign(req);\n",                                # answer dropped
            "  const bool foreign = host_is_foreign(req);\n",           # local unread
            "  if (host_is_foreign(req)) { Serial.println(\"x\"); }\n",  # only logs
            "  const bool foreign = host_is_foreign(req);\n"
            "  if (foreign) Serial.println(\"x\");\n",                  # local only logs
            "  if (host_is_foreign(req) && false) return ESP_OK;\n",    # mixed condition
            "  if (!host_is_foreign(req)) return ESP_OK;\n",            # inverted
            "  if (host_is_foreign(req)) return send_provisioning_receipt(req);\n",  # serves
            "  if (host_is_foreign(req)) {\n    if (!provisioning_gate_take()) return ESP_OK;\n"
            "    return ESP_OK;\n  }\n",                                 # spends the tap
            "  const bool foreign = host_is_foreign(req);\n"
            "  {\n    const bool foreign = false;\n"
            "    if (foreign) return ESP_OK;\n  }\n"):                  # shadowed
        red(not_a_check + no_host, "never asks the Host")
    for gate in ("static bool auth_gate(httpd_req_t* req) {\n"
                 "  if (host_is_foreign(req)) {\n    send_host_refusal(req);\n"
                 "    return true;\n  }\n  const char* token = auth_get_token();\n"
                 "  return auth_check(req, token);\n}\n",
                 "static bool auth_gate(httpd_req_t* req) {\n"
                 "  if (host_is_foreign(req)) return auth_check(req, auth_get_token());\n"
                 "  const char* token = auth_get_token();\n"
                 "  return auth_check(req, token);\n}\n"):
        got = check_host_first({"fx": gate}, require_defs=False)
        assert any("auth_gate no longer refuses a foreign Host" in f for f in got), (gate, got)
    good_gate = ("static bool auth_gate(httpd_req_t* req) {\n"
                 "  if (host_is_foreign(req)) {\n    send_host_refusal(req);\n"
                 "    return false;\n  }\n  const char* token = auth_get_token();\n"
                 "  return auth_check(req, token, request_peer_ipv4(req));\n}\n")
    assert check_host_first({"fx": good_gate}, require_defs=False) == []
    early_grant = good_gate.replace(
        "  if (host_is_foreign(req)) {",
        "  if (auth_bearer_grant(req, t, request_peer_ipv4(req)) == "
        "AuthManager::BearerResult::HELD) return false;\n  if (host_is_foreign(req)) {")
    got = check_host_first({"fx": early_grant}, require_defs=False)
    assert any("auth_gate reads the token (auth_bearer_grant) before its Host check" in f
               for f in got), (early_grant, got)
    green("  if (host_is_foreign(req)) return ESP_OK;\n" + no_host)
    green("  if (host_is_foreign(req)) {\n    send_host_refusal(req);\n"
          "    return ESP_OK;\n  }\n" + no_host)
    green("  const bool foreign = host_is_foreign(req);\n"
          "  if (foreign) {\n    send_host_refusal(req);\n    return ESP_OK;\n  }\n" + no_host)

    # The decision form: receipt_decide gets the Host, and the dispatch fails
    # closed (the receipt only under SERVE_*, the Host refusal at the end).
    decide = ("  const ReceiptVerdict verdict = receipt_decide(host_is_foreign(req),\n"
              "      [req]() { return bearer_present_and_valid(req); },\n"
              "      []() { return provisioning_gate_take(); });\n")
    serve_bearer = ("  if (verdict == ReceiptVerdict::SERVE_BEARER) {\n"
                    "    return send_provisioning_receipt(req);\n  }\n")
    serve_tap = ("  if (verdict == ReceiptVerdict::SERVE_TAP) {\n"
                 "    const esp_err_t result = send_provisioning_receipt(req);\n"
                 "    log(\"served\");\n    return result;\n  }\n")
    no_tap = ("  if (verdict == ReceiptVerdict::REFUSE_NO_TAP) {\n"
              "    return httpd_resp_sendstr(req, \"{}\");\n  }\n")
    tail = "  send_host_refusal(req);\n  return ESP_OK;\n"
    green(decide + serve_bearer + serve_tap + no_tap + tail)
    green(decide + "  if (verdict == ReceiptVerdict::SERVE_BEARER) "
          "return send_provisioning_receipt(req);\n" + serve_tap + no_tap + tail)
    green(decide + "  if (verdict == ReceiptVerdict::SERVE_BEARER ||\n"
          "      verdict == ReceiptVerdict::SERVE_TAP) {\n"
          "    return send_provisioning_receipt(req);\n  }\n" + no_tap + tail)
    # The reviewed shape: branches for three verdicts, the receipt for the rest.
    refuse_host = ("  if (verdict == ReceiptVerdict::REFUSE_HOST) {\n"
                   "    send_host_refusal(req);\n    return ESP_OK;\n  }\n")
    open_tail = "  return send_provisioning_receipt(req);\n"
    outside = "sends the receipt outside an explicit"
    red(decide + refuse_host + serve_bearer + no_tap + open_tail, outside)
    red(decide + serve_bearer + no_tap + open_tail, outside)  # the refusal dropped
    red(decide + refuse_host.replace("REFUSE_HOST", "SERVE_TAP") + serve_bearer +
        no_tap + open_tail, outside)                         # the wrong enumerator
    red(decide + "  (void)verdict;\n" + open_tail, outside)  # the verdict ignored
    body = decide + "  (void)verdict;\n" + open_tail
    handler = function_body(receipt(body)["fx"], "handle_provisioning_receipt")
    assert not any(marker_decides(handler, m) for m in AUTH_MARKERS), \
        "main()'s gate rule accepts a receipt_decide verdict nobody reads"
    # A logged Host, then the decision told the Host is fine.
    red("  if (host_is_foreign(req)) { Serial.println(\"foreign\"); }\n" +
        decide.replace("receipt_decide(host_is_foreign(req),", "receipt_decide(false,") +
        serve_bearer + serve_tap + no_tap + tail,
        "gives receipt_decide 'false' as its Host verdict")
    for wrong in ("!host_is_foreign(req)", "host_is_foreign(nullptr)", "foreign"):
        red(decide.replace("host_is_foreign(req),", wrong + ","),
            f"gives receipt_decide {wrong!r}")
    red("  bool foreign = host_is_foreign(req);\n" +
        decide.replace("host_is_foreign(req),", "foreign,") + serve_bearer + tail,
        "gives receipt_decide 'foreign'")                    # a mutable local
    # The fail-closed dispatch, broken one hunk at a time.
    red(decide + serve_bearer.replace("== ReceiptVerdict::SERVE_BEARER",
                                      "!= ReceiptVerdict::REFUSE_NO_TAP") + tail, outside)
    red(decide + serve_bearer.replace("SERVE_BEARER", "REFUSE_HOST") + tail, outside)
    red(decide + serve_bearer.replace("SERVE_BEARER) {", "SERVE_BEARER || true) {") +
        tail, outside)
    red(decide + "  if (verdict == ReceiptVerdict::SERVE_TAP) {\n    log(\"x\");\n"
        "  } else {\n    return send_provisioning_receipt(req);\n  }\n" + tail, outside)
    red(decide + "  if (verdict == ReceiptVerdict::SERVE_TAP) {\n  }\n" + open_tail, outside)
    red(decide + serve_bearer + serve_tap + no_tap + "  return ESP_OK;\n",
        "does not end in the Host refusal")
    red(decide + serve_bearer + serve_tap + no_tap +
        "  if (verdict == ReceiptVerdict::REFUSE_HOST) send_host_refusal(req);\n"
        "  return ESP_OK;\n", "does not end in the Host refusal")
    red(decide + serve_bearer + "  return ESP_OK;\n" + tail,
        "does not end in the Host refusal")                  # the tail unreachable
    red(decide.replace("const ReceiptVerdict", "ReceiptVerdict") + serve_bearer + tail,
        "must be bound once")
    red(decide + "  {\n    const ReceiptVerdict verdict = ReceiptVerdict::SERVE_BEARER;\n"
        "    if (verdict == ReceiptVerdict::SERVE_BEARER) "
        "return send_provisioning_receipt(req);\n  }\n" + tail, "is written 2 times")
    # ...an emitter called from an unregistered helper is held to it too.
    helper = {"fx": emit + "static esp_err_t other(httpd_req_t* req) {\n"
              "  return send_provisioning_receipt(req);\n}\n"}
    got = check_host_first(helper, require_defs=False)
    assert any("other calls send_provisioning_receipt" in f for f in got), got

    # 7. The page path: the Host is page_token_decide's first argument, read
    #    before the grants and the tap.
    def inject(body):
        return {"fx": "static PageToken page_token_inject(httpd_req_t* req) {\n" + body + "}\n"}
    def inject_red(body, *needles):
        got = check_host_first(inject(body), require_defs=False)
        assert any(all(n in f for n in needles) for f in got), (needles, body, got)
    tap = "      []() { return provisioning_gate_take(); });\n"
    inject_red("  const bool bearer_ok = bearer_present_and_valid(req);\n"
               "  return page_token_decide(false, bearer_ok, false,\n" + tap,
               "does not decide the Host first")
    inject_red("  const bool bearer_ok = bearer_present_and_valid(req);\n"
               "  const bool foreign = host_is_foreign(req);\n"
               "  return page_token_decide(foreign, bearer_ok, false,\n" + tap,
               "before the Host check")
    inject_red("  const bool bearer_ok = auth_bearer_grant(req, t, request_peer_ipv4(req)) ==\n"
               "      AuthManager::BearerResult::OK;\n"
               "  const bool foreign = host_is_foreign(req);\n"
               "  return page_token_decide(foreign, bearer_ok, false,\n" + tap,
               "page_token_inject runs auth_bearer_grant before the Host check")
    good_inject = ("  const bool foreign = host_is_foreign(req);\n"
                   "  const bool bearer_ok = !foreign && bearer_present_and_valid(req);\n"
                   "  return page_token_decide(foreign, bearer_ok, false,\n" + tap)
    assert check_host_first(inject(good_inject), require_defs=False) == []
    assert check_host_first(inject("  return page_token_decide(host_is_foreign(req), "
                                   "setup_active(), false,\n" + tap),
                            require_defs=False) == []
    # A Host local that only logs, or an early refusal-shaped if, does not
    # stand in for the decision being told.
    for logged in ("  if (foreign) Serial.println(\"foreign\");\n",
                   "  if (host_is_foreign(req)) return PageToken::WITHHOLD;\n"):
        inject_red(good_inject.replace("  const bool bearer_ok", logged + "  const bool bearer_ok")
                   .replace("page_token_decide(foreign,", "page_token_decide(false,"),
                   "does not decide the Host first")
    for wrong in ("!foreign", "bearer_ok"):
        inject_red(good_inject.replace("page_token_decide(foreign,",
                                       f"page_token_decide({wrong},"),
                   f"gives page_token_decide {wrong!r}")
    inject_red(good_inject.replace("const bool foreign", "bool foreign"),
               "gives page_token_decide 'foreign'")
    inject_red(good_inject.replace("  return page_token_decide(",
                                   "  {\n  const bool foreign = false;\n  return page_token_decide(")
               + "  }\n", "gives page_token_decide 'foreign'")      # shadowed

    # 8. The page streams the token only on INJECT. (A stray reader of the
    #    token is the bearer-only answer rule's: _assert_answer_gate_rule.)
    def send(cond, sig="PageToken verdict", pre=""):
        return {"fx": "static esp_err_t send_html_with_token(httpd_req_t* req, "
                f"const char* h, {sig}) {{\n" + pre +
                f"  const char* token = {cond} ? auth_get_token() : \"\";\n"
                "  return ESP_OK;\n}\n"}
    assert check_host_first(send("(verdict == PageToken::INJECT)"), require_defs=False) == []
    assert check_host_first(send("verdict == canary::net::provisioning_gate::PageToken::INJECT"),
                            require_defs=False) == []
    for wrong in ("(verdict != PageToken::WITHHOLD)", "(verdict != PageToken::INJECT)",
                  "(verdict == PageToken::FOREIGN_HOST || verdict == PageToken::INJECT)",
                  "(true || verdict == PageToken::INJECT)", "(other == PageToken::INJECT)",
                  "(inject && !foreign)"):
        got = check_host_first(send(wrong), require_defs=False)
        assert any("outside a PageToken::INJECT test" in f for f in got), (wrong, got)
    got = check_host_first(send("(verdict == PageToken::INJECT)",
                                pre="  verdict = PageToken::INJECT;\n"), require_defs=False)
    assert any("reassigns its verdict" in f for f in got), got
    got = check_host_first(send("(inject)", sig="bool inject"), require_defs=False)
    assert any("no longer takes the page-token verdict" in f for f in got), got

    # 9. host_is_foreign is pinned (its GLUE_DEFS row, held to the tree's
    #    text here): reformatting passes, any other edit fails.
    hif = ("static bool host_is_foreign(httpd_req_t* req) {\n"
           "  if (from_ap_subnet(req)) return false;\n  char host[96];\n"
           "  if (httpd_req_get_hdr_value_str(req, \"Host\", host,\n"
           "                                  sizeof(host)) != ESP_OK) return true;\n"
           "  return !canary::net::host_names_this_device(host);\n}\n")
    row = {("fx/" + _NET, "host_is_foreign"): GLUE_DEFS[(_NET, "host_is_foreign")]}
    assert glue_def_problems({"fx/" + _NET: hif}, row) == []
    for mutant in (hif.replace("{\n  if (from_ap", "{\n  return false;\n  if (from_ap"),
                   hif.replace("ESP_OK) return true;", "ESP_OK) return false;"),
                   hif.replace("return !canary", "return canary"),
                   hif.replace("from_ap_subnet(req)) return", "from_ap_subnet(req) || 1) return")):
        got = glue_def_problems({"fx/" + _NET: mutant}, row)
        assert any("host_is_foreign changed" in f for f in got), (mutant, got)


# ── The bearer-only answers: fixtures ─────────────────────────────────────────
#
# A small tree on the post-F340 auth API (check, grant, compare, handedToken,
# the three wrappers), with the receipt and its dispatch and SERVE_TAP clear,
# /api/status behind its refusal answering the pin, a page, a public roll-call,
# the bearer glue, the accessors and device-side uses (the two mints among
# them). The clean tree passes; every plant fails for its named reason.

_FIX_AUTH = "fx/lib/securacv_auth.cpp"
_FIX_CRYPTO = "fx/lib/securacv_crypto.cpp"
_FIX_NET_H = "fx/lib/securacv_network.h"
_FIX_NET = "fx/lib/securacv_network.cpp"
_FIX_MAIN = "fx/src/main.cpp"
_FIX_OTHER = "fx/lib/securacv_other.cpp"
_FIX_FILES = {
    _FIX_AUTH: (
        "static AuthManager s_auth;\n"
        "AuthManager& auth_get_instance() {\n  return s_auth;\n}\n"
        "AuthManager::AuthManager() : m_table{} {}\n"
        "bool AuthManager::constantTimeCompare(const void* a, const void* b, size_t len) {\n"
        "  const volatile uint8_t* pa = (const volatile uint8_t*)a;\n"
        "  const volatile uint8_t* pb = (const volatile uint8_t*)b;\n"
        "  volatile uint8_t diff = 0;\n"
        "  for (size_t i = 0; i < len; i++) diff |= pa[i] ^ pb[i];\n"
        "  return diff == 0;\n}\n"
        "void AuthManager::handedToken(uint32_t source) {\n"
        "  canary::net::auth_backoff::record_success(m_table, source);\n}\n"
        "AuthManager::Presented AuthManager::compare(httpd_req_t* req,\n"
        "                                            const char* expected_token) {\n"
        "  char auth_buf[128];\n"
        "  if (httpd_req_get_hdr_value_str(req, \"Authorization\", auth_buf, sizeof(auth_buf)) "
        "!= ESP_OK) {\n    return Presented::UNREADABLE;\n  }\n"
        "  if (strncmp(auth_buf, \"Bearer \", 7) != 0) return Presented::NOT_BEARER;\n"
        "  const char* token = auth_buf + 7;\n"
        "  const size_t expected_len = strlen(expected_token);\n"
        "  if (expected_len == 0 || strlen(token) != expected_len) return Presented::WRONG;\n"
        "  return constantTimeCompare(token, expected_token, expected_len) ? Presented::RIGHT\n"
        "                                                                   : Presented::WRONG;\n"
        "}\n"
        "AuthManager::BearerResult AuthManager::grant(httpd_req_t* req,\n"
        "                                             const char* expected_token,\n"
        "                                             uint32_t source) {\n"
        "  const bool presented = httpd_req_get_hdr_value_len(req, \"Authorization\") != 0;\n"
        "  return canary::net::auth_backoff::grant(\n"
        "      m_table, source, millis(), presented,\n"
        "      [&]() { return compare(req, expected_token) == Presented::RIGHT; });\n}\n"
        "bool AuthManager::check(httpd_req_t* req, const char* expected_token, uint32_t source) {\n"
        "  const uint32_t now = millis();\n"
        "  if (canary::net::auth_backoff::locked(m_table, source, now)) {\n"
        "    httpd_resp_set_status(req, \"429 Too Many Requests\");\n"
        "    return false;\n  }\n"
        "  Presented why = Presented::WRONG;\n"
        "  const bool presented = httpd_req_get_hdr_value_len(req, \"Authorization\") != 0;\n"
        "  const BearerResult result = canary::net::auth_backoff::grant(\n"
        "      m_table, source, now, presented, [&]() {\n"
        "        why = compare(req, expected_token);\n"
        "        return why == Presented::RIGHT;\n      });\n"
        "  if (result == BearerResult::OK) return true;\n"
        "  httpd_resp_set_status(req, why == Presented::WRONG ? \"403 Forbidden\" "
        ": \"401 Unauthorized\");\n"
        "  return false;\n}\n"
        "bool auth_check(httpd_req_t* req, const char* expected_token, uint32_t source) {\n"
        "  return auth_get_instance().check(req, expected_token, source);\n}\n"
        "AuthManager::BearerResult auth_bearer_grant(httpd_req_t* req,\n"
        "                                            const char* expected_token,\n"
        "                                            uint32_t source) {\n"
        "  return auth_get_instance().grant(req, expected_token, source);\n}\n"
        "void auth_handed_token(uint32_t source) {\n"
        "  auth_get_instance().handedToken(source);\n}\n"
        "static char s_bearer[36];\n"
        "bool auth_load_or_derive(const uint8_t privkey[32]) {\n"
        "  if (!derive_api_token(privkey, s_bearer, sizeof(s_bearer))) {\n"
        "    s_bearer[0] = '\\0';\n    return false;\n  }\n"
        "  return true;\n}\n"
        "const char* auth_get_token() {\n  return s_bearer;\n}\n"),
    _FIX_CRYPTO: (
        "bool derive_api_token(const uint8_t privkey[32], char* token_str,\n"
        "                      size_t token_str_len) {\n"
        "  if (!token_str || token_str_len < 36) return false;\n"
        "  return hmac_into(privkey, token_str, token_str_len);\n}\n"),
    _FIX_NET_H: (
        "struct ScvNetworkManager {\n"
        "  const char* getTlsCertFp() const { return m_tls_cert_fp_hex; }\n"
        "  const char* getApPassword() const { return m_ap_password; }\n"
        "  char m_ap_password[65];\n"
        "  char m_tls_cert_fp_hex[65];\n"
        "};\n"),
    _FIX_NET: (
        "static bool from_ap_subnet(httpd_req_t* req) {\n"
        "  if (!(WiFi.getMode() & WIFI_AP)) return false;\n"
        "  return canary::net::provisioning_gate::request_on_softap(\n"
        "      peer_of(req), local_of(req), ap_ip(), ap_mask(), sta_ip(), sta_mask());\n}\n"
        "static bool host_is_foreign(httpd_req_t* req) {\n"
        "  if (from_ap_subnet(req)) return false;\n"
        "  char host[96];\n"
        "  if (httpd_req_get_hdr_value_str(req, \"Host\", host, sizeof(host)) != ESP_OK) "
        "return true;\n"
        "  return !canary::net::host_names_this_device(host);\n}\n"
        "static uint32_t request_peer_ipv4(httpd_req_t* req) {\n  return peer_of(req);\n}\n"
        "static void send_host_refusal(httpd_req_t* req) {\n"
        "  httpd_resp_set_status(req, \"403 Forbidden\");\n"
        "  httpd_resp_sendstr(req, \"{\\\"error\\\":\\\"host\\\"}\");\n}\n"
        "static bool auth_gate(httpd_req_t* req) {\n"
        "  if (host_is_foreign(req)) {\n    send_host_refusal(req);\n    return false;\n  }\n"
        "  const char* token = auth_get_token();\n"
        "  if (!token || token[0] == '\\0') {\n    return false;\n  }\n"
        "  if (!auth_check(req, token, request_peer_ipv4(req))) {\n    return false;\n  }\n"
        "  return true;\n}\n"
        "static network_gate_fn_t s_gate_take = nullptr;\n"
        "static network_gate_fn_t s_gate_is_open = nullptr;\n"
        "void network_set_provisioning_gate_hooks(network_gate_fn_t take,\n"
        "                                         network_gate_fn_t is_open) {\n"
        "  s_gate_take = take;\n  s_gate_is_open = is_open;\n}\n"
        "static bool provisioning_gate_take() { return s_gate_take ? s_gate_take() : false; }\n"
        "static bool bearer_present_and_valid(httpd_req_t* req) {\n"
        "  const char* token = auth_get_token();\n"
        "  if (!token || token[0] == '\\0') return false;\n"
        "  return auth_bearer_grant(req, token, request_peer_ipv4(req)) ==\n"
        "         AuthManager::BearerResult::OK;\n}\n"
        "bool ScvNetworkManager::begin(const char* ap_password) {\n"
        "  strncpy(m_ap_password, ap_password, sizeof(m_ap_password) - 1);\n"
        "  return true;\n}\n"
        "void ScvNetworkManager::raiseAp() {\n"
        "  WiFi.softAP(m_ap_ssid, m_ap_password);\n"
        "  announce_setup_network(m_ap_ssid, m_ap_password);\n}\n"
        "void ScvNetworkManager::idle() {\n  delay(1);\n}\n"
        "static esp_err_t send_html_with_token(httpd_req_t* req, const char* html,\n"
        "                                      PageToken verdict) {\n"
        "  const char* token = (verdict == PageToken::INJECT) ? auth_get_token() : \"\";\n"
        "  return send_page(req, html, token);\n}\n"
        "static PageToken page_token_inject(httpd_req_t* req) {\n"
        "  const bool foreign      = host_is_foreign(req);\n"
        "  const bool setup_active = setup_is_active() || setup_is_first_boot();\n"
        "  const bool bearer_ok    = !foreign && bearer_present_and_valid(req);\n"
        "  const bool on_ap        = from_ap_subnet(req);\n"
        "  bool tap_spent = false;\n"
        "  const PageToken verdict = page_token_decide(\n"
        "      foreign, setup_active, bearer_ok, on_ap,\n"
        "      [&tap_spent]() {\n        tap_spent = provisioning_gate_take();\n"
        "        return tap_spent;\n      });\n"
        "  if (verdict != PageToken::INJECT) {\n    log_withheld();\n"
        "  } else if (tap_spent) {\n    auth_handed_token(request_peer_ipv4(req));\n  }\n"
        "  return verdict;\n}\n"
        "static esp_err_t handle_ui(httpd_req_t* req) {\n"
        "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n}\n"
        "static esp_err_t send_provisioning_receipt(httpd_req_t* req) {\n"
        "  ScvNetworkManager& net = network_get_instance();\n"
        "  JsonDocument doc;\n"
        "  doc[\"token\"] = auth_get_token();\n"
        "  doc[\"ap_password\"] = net.getApPassword();\n"
        "  doc[\"tls_cert_fp\"] = net.getTlsCertFp();\n"
        "  return send_doc(req, doc);\n}\n"
        "static esp_err_t handle_provisioning_receipt(httpd_req_t* req) {\n"
        "  const ReceiptVerdict verdict = receipt_decide(\n"
        "      host_is_foreign(req),\n"
        "      [req]() { return bearer_present_and_valid(req); },\n"
        "      []() { return provisioning_gate_take(); });\n"
        "  if (verdict == ReceiptVerdict::SERVE_BEARER) {\n"
        "    return send_provisioning_receipt(req);\n  }\n"
        "  if (verdict == ReceiptVerdict::SERVE_TAP) {\n"
        "    auth_handed_token(request_peer_ipv4(req));\n"
        "    return send_provisioning_receipt(req);\n  }\n"
        "  if (verdict == ReceiptVerdict::REFUSE_NO_TAP) {\n"
        "    return httpd_resp_sendstr(req, \"{}\");\n  }\n"
        "  send_host_refusal(req);\n  return ESP_OK;\n}\n"
        "static esp_err_t handle_status(httpd_req_t* req) {\n"
        "  if (!rate_limit_check(req)) return ESP_OK;\n"
        "  if (!auth_gate(req)) return ESP_OK;\n"
        "  ScvNetworkManager& net = network_get_instance();\n"
        "  JsonDocument doc;\n"
        "  doc[\"provisioning_gate\"] = s_gate_take ? \"physical_button\" : \"unwired\";\n"
        "  doc[\"tls_cert_fp\"] = net.getTlsCertFp();\n"
        "  return send_doc(req, doc);\n}\n"
        "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
        "  if (!auth_gate(req)) return ESP_OK;\n"
        "  return httpd_resp_sendstr(req, \"{}\");\n}\n"
        "static esp_err_t handle_witness(httpd_req_t* req) {\n"
        "  if (!auth_gate(req)) return ESP_OK;\n"
        "  char query[64];\n"
        "  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) "
        "page_from(query);\n"
        "  return httpd_resp_sendstr(req, \"[]\");\n}\n"
        "static esp_err_t handle_fleet(httpd_req_t* req) {\n"
        "  prefs.begin(\"fleet\", true);\n"
        "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n"
        "  return httpd_resp_sendstr(req, fleet_body());\n}\n"
        "void ScvNetworkManager::registerHttpHandlers(httpd_handle_t server) {\n"
        "  httpd_uri_t a = { .uri = \"/\", .method = HTTP_GET, .handler = handle_ui };\n"
        "  httpd_uri_t b = { .uri = \"/api/provisioning-receipt\", .method = HTTP_GET,\n"
        "                    .handler = handle_provisioning_receipt };\n"
        "  httpd_uri_t c = { .uri = \"/api/status\", .method = HTTP_GET, "
        ".handler = handle_status };\n"
        "  httpd_uri_t d = { .uri = \"/api/wifi/connect\", .method = HTTP_POST,\n"
        "                    .handler = handle_wifi_connect };\n"
        "  httpd_uri_t e = { .uri = \"/api/witness\", .method = HTTP_GET, "
        ".handler = handle_witness };\n"
        "  httpd_uri_t f = { .uri = \"/api/fleet\", .method = HTTP_GET, "
        ".handler = handle_fleet };\n"
        "}\n"),
    _FIX_MAIN: (
        "static char g_ap_password[16];\n"
        "static canary::net::provisioning_gate::State g_prov_gate = {0};\n"
        "static bool prov_gate_take_hook() {\n"
        "  return canary::net::provisioning_gate::take(g_prov_gate, millis(), TTL);\n}\n"
        "static bool prov_gate_is_open_hook() {\n"
        "  return canary::net::provisioning_gate::is_open(g_prov_gate, millis(), TTL);\n}\n"
        "void setup() {\n"
        "  DeviceIdentity& device = witness_get_device();\n"
        "  if (!canary::net::derive_ap_password(device.privkey, g_ap_password, "
        "sizeof(g_ap_password))) {\n"
        "    Serial.println(\"[!!] Setup network key derivation failed - HALTING\");\n"
        "    while (true) { delay(1000); }\n  }\n"
        "  network_set_provisioning_gate_hooks(prov_gate_take_hook, prov_gate_is_open_hook);\n"
        "#if FEATURE_WIFI_AP\n"
        "  Serial.printf(\"Password: %s\\n\", g_ap_password);\n"
        "#endif\n"
        "}\n"),
}
_FIX_MINT = ("if (!canary::net::derive_ap_password(device.privkey, g_ap_password, "
             "sizeof(g_ap_password))) { Serial.println(\"[!!] Setup network key derivation "
             "failed - HALTING\"); while (true) { delay(1000); } }")
_FIX_GLUE = ((_NET, "host_is_foreign"), (_NET, "auth_gate"), (_NET, "bearer_present_and_valid"),
             (_NET, "from_ap_subnet"), (_NET, "network_set_provisioning_gate_hooks"),
             (_NET, "provisioning_gate_take"), (_NET, "page_token_inject"),
             (_MAIN, "prov_gate_take_hook"), (_AUTH, "AuthManager::check"),
             (_AUTH, "AuthManager::grant"), (_AUTH, "AuthManager::compare"),
             (_AUTH, "AuthManager::handedToken"), (_AUTH, "auth_check"),
             (_AUTH, "auth_bearer_grant"), (_AUTH, "auth_handed_token"))
_FIX_TABLES = {
    "local": {
        (_NET, "ScvNetworkManager::begin"): (
            "keeps the SoftAP password",
            ("strncpy(m_ap_password, ap_password, sizeof(m_ap_password) - 1);",)),
        (_NET, "ScvNetworkManager::raiseAp"): (
            "raises the SoftAP",
            ("WiFi.softAP(m_ap_ssid, m_ap_password);",
             "announce_setup_network(m_ap_ssid, m_ap_password);")),
        (_MAIN, "setup"): (
            "mints the SoftAP password, prints it",
            (_FIX_MINT, "Serial.printf(\"Password: %s\\n\", g_ap_password);")),
        (_AUTH, "auth_load_or_derive"): (
            "mints the token",
            ("if (!derive_api_token(privkey, s_bearer, sizeof(s_bearer))) { "
             "s_bearer[0] = '\\0'; return false; }",)),
    },
    "answers": {"handle_provisioning_receipt": ("token ap pin", "receipt"),
                "handle_status": ("pin", "bearer"),
                "handle_ui": ("token", "page")},
    "outside": (("securacv_network.h", "char m_ap_password[65];"),
                ("securacv_network.h", "char m_tls_cert_fp_hex[65];"),
                (_AUTH, "static char s_bearer[36];"),
                (_MAIN, "static char g_ap_password[16];")),
    "public": {("GET", "/"): "the page", ("GET", "/api/fleet"): "the roll-call"},
}


def _fixture_sources() -> dict:
    return {f: strip_comments(t) for f, t in _FIX_FILES.items()}


def _fixture_tables(sources: dict) -> dict:
    """_FIX_TABLES, with each _FIX_GLUE row pinned to the clean fixture's
    body under the tree's row's message (so a plant reads what the tree's
    failure says)."""
    glue = {}
    for suffix, qual in _FIX_GLUE:
        fname = next(f for f in sources if _in_file(f, suffix))
        a, b = definition_span(sources[fname], qual)
        glue[(suffix, qual)] = (GLUE_DEFS[(suffix, qual)][0], _fold(sources[fname][a:b]))
    return dict(_FIX_TABLES, glue=glue)


_PUBLIC_AP = "public route handler handle_fleet() answers the SoftAP password"
_STATUS_REFUSAL = "handle_status calls auth_gate other than as the whole statement"
# (what, file, old text, new text, words every one of which a problem names).
# An empty old text appends the new text to the file (a new file when absent).
_FIX_PLANTS = (
    ("M1: the public roll-call answers the SoftAP password", _FIX_NET,
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     "  httpd_resp_set_hdr(req, \"X-AP\", network_get_instance().getApPassword());\n"
     "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n",
     (_PUBLIC_AP, "handle_fleet() reads the SoftAP password and is no")),
    ("M4: the roll-call answers it through a helper", _FIX_NET,
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n",
     "static void fill_ap(char* out, size_t cap) {\n"
     "  snprintf(out, cap, \"%s\", network_get_instance().getApPassword());\n}\n"
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n  char buf[16];\n"
     "  fill_ap(buf, sizeof(buf));\n",
     (_PUBLIC_AP, "fill_ap() reads the SoftAP password and is no")),
    ("M7: /api/status answers the SoftAP password behind its refusal", _FIX_NET,
     "  doc[\"tls_cert_fp\"] = net.getTlsCertFp();\n  return send_doc(req, doc);\n}\n"
     "static esp_err_t handle_wifi_connect",
     "  doc[\"tls_cert_fp\"] = net.getTlsCertFp();\n  doc[\"ap\"] = net.getApPassword();\n"
     "  return send_doc(req, doc);\n}\nstatic esp_err_t handle_wifi_connect",
     ("handle_status() carries the SoftAP password, the TLS pin; its SECRET_ANSWERS entry "
      "says the TLS pin",)),
    ("M2: /api/status open during setup", _FIX_NET,
     "  if (!auth_gate(req)) return ESP_OK;\n  ScvNetworkManager& net",
     "  if (!setup_is_active() && !auth_gate(req)) return ESP_OK;\n  ScvNetworkManager& net",
     (_STATUS_REFUSAL, "handle_status() answers the TLS pin with no `if (!auth_gate(req))")),
    ("M3: /api/status serves under setup or a bearer, before its refusal", _FIX_NET,
     "  if (!auth_gate(req)) return ESP_OK;\n  ScvNetworkManager& net",
     "  if (setup_is_active() || bearer_present_and_valid(req)) "
     "return send_provisioning_receipt(req);\n"
     "  if (!auth_gate(req)) return ESP_OK;\n  ScvNetworkManager& net",
     ("bearer_present_and_valid called from handle_status",
      "reaches a credential (send_provisioning_receipt) before its refusal")),
    ("W1: Wi-Fi connect open during setup", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!setup_is_active() && !auth_gate(req)) return ESP_OK;\n",
     ("handle_wifi_connect calls auth_gate other than as the whole statement",)),
    ("W2: Wi-Fi connect open to a SoftAP peer", _FIX_NET,
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     "static esp_err_t handle_wifi_connect(httpd_req_t* req) {\n"
     "  if (!from_ap_subnet(req) && !auth_gate(req)) return ESP_OK;\n",
     ("handle_wifi_connect calls auth_gate other than as the whole statement",)),
    ("M8: the receipt's bearer grant passes during setup", _FIX_NET,
     "[req]() { return bearer_present_and_valid(req); },",
     "[req]() { return bearer_present_and_valid(req) || setup_is_active(); },",
     ("hands receipt_decide the grants",)),
    ("M9: the receipt's tap grant passes a SoftAP peer", _FIX_NET,
     "      []() { return provisioning_gate_take(); });\n  if (verdict == ReceiptVerdict::",
     "      [req]() { return from_ap_subnet(req) || provisioning_gate_take(); });\n"
     "  if (verdict == ReceiptVerdict::",
     ("hands receipt_decide the grants",)),
    ("M10: the bearer grant takes a cv_session cookie", _FIX_NET,
     "  if (!token || token[0] == '\\0') return false;\n  return auth_bearer_grant(",
     "  if (!token || token[0] == '\\0') return false;\n  char c[64];\n"
     "  if (httpd_req_get_hdr_value_str(req, \"Cookie\", c, sizeof(c)) == ESP_OK &&\n"
     "      strstr(c, \"cv_session=\")) return true;\n  return auth_bearer_grant(",
     ("bearer_present_and_valid changed", "reads the request header '\"Cookie\"'",
      "ANY_SESSION_GATES")),
    ("M12: auth_gate passes during setup", _FIX_NET,
     "  if (!auth_check(req, token, request_peer_ipv4(req))) {",
     "  if (!auth_check(req, token, request_peer_ipv4(req)) && !setup_is_active()) {",
     ("auth_gate changed",)),
    ("G1: the bearer grant passes an unprovisioned token", _FIX_NET,
     "  if (!token || token[0] == '\\0') return false;\n  return auth_bearer_grant(",
     "  if (!token || token[0] == '\\0') return true;\n  return auth_bearer_grant(",
     ("bearer_present_and_valid changed",)),
    ("G2: auth_gate's compare made dead", _FIX_NET,
     "  if (!auth_check(req, token, request_peer_ipv4(req))) {",
     "  if (!auth_check(req, token, request_peer_ipv4(req)) && false) {",
     ("auth_gate changed",)),
    ("M11: the page's bearer grant widened", _FIX_NET,
     "!foreign && bearer_present_and_valid(req);",
     "!foreign && (bearer_present_and_valid(req) || other());",
     ("page_token_inject changed", "four unlocks", "Phase 6")),
    ("M14: the page's SoftAP grant takes the home Wi-Fi", _FIX_NET,
     "const bool on_ap        = from_ap_subnet(req);",
     "const bool on_ap        = from_ap_subnet(req) || sta_connected;",
     ("page_token_inject changed",)),
    ("M15: the page's SoftAP grant takes a broker link", _FIX_NET,
     "const bool on_ap        = from_ap_subnet(req);",
     "const bool on_ap        = from_ap_subnet(req) || mqtt_connected();",
     ("page_token_inject changed",)),
    ("M16: the page's SoftAP grant always", _FIX_NET,
     "const bool on_ap        = from_ap_subnet(req);", "const bool on_ap        = true;",
     ("page_token_inject changed",)),
    ("G3: the page's setup grant while the home link is down", _FIX_NET,
     "setup_is_active() || setup_is_first_boot();",
     "setup_is_active() || setup_is_first_boot() || !sta_connected;",
     ("page_token_inject changed",)),
    ("A2: the page's tap grant passes a SoftAP peer", _FIX_NET,
     "        return tap_spent;\n", "        return tap_spent || from_ap_subnet(req);\n",
     ("page_token_inject changed",)),
    ("A1: the SoftAP grant whenever the AP is up", _FIX_NET,
     "  if (!(WiFi.getMode() & WIFI_AP)) return false;\n",
     "  if (WiFi.getMode() & WIFI_AP) return true;\n",
     ("from_ap_subnet changed",)),
    ("A4: an unwired tap reads open", _FIX_NET,
     "s_gate_take ? s_gate_take() : false; }", "s_gate_take ? s_gate_take() : true; }",
     ("provisioning_gate_take changed",)),
    ("A5: the take peeks", _FIX_NET,
     "s_gate_take ? s_gate_take() : false; }", "s_gate_is_open ? s_gate_is_open() : false; }",
     ("provisioning_gate_take changed", "names provisioning_gate_take")),
    ("A6: the peek wired as the take hook", _FIX_MAIN,
     "network_set_provisioning_gate_hooks(prov_gate_take_hook, prov_gate_is_open_hook);",
     "network_set_provisioning_gate_hooks(prov_gate_is_open_hook, prov_gate_is_open_hook);",
     ("calls network_set_provisioning_gate_hooks other than as the whole statement",)),
    ("A7: the take hook peeks", _FIX_MAIN,
     "  return canary::net::provisioning_gate::take(g_prov_gate",
     "  return canary::net::provisioning_gate::is_open(g_prov_gate",
     ("prov_gate_take_hook changed",)),
    ("K1: compare takes a session under Authorization", _FIX_AUTH,
     "  if (strncmp(auth_buf, \"Bearer \", 7) != 0)",
     "  if (session_table_has(auth_buf, strlen(auth_buf))) return Presented::RIGHT;\n"
     "  if (strncmp(auth_buf, \"Bearer \", 7) != 0)",
     ("AuthManager::compare changed",)),
    ("L1: grant compares a held source", _FIX_AUTH,
     "  const bool presented = httpd_req_get_hdr_value_len(req, \"Authorization\") != 0;\n"
     "  return canary::net::auth_backoff::grant(",
     "  if (compare(req, expected_token) == Presented::RIGHT) return BearerResult::OK;\n"
     "  const bool presented = httpd_req_get_hdr_value_len(req, \"Authorization\") != 0;\n"
     "  return canary::net::auth_backoff::grant(",
     ("AuthManager::grant changed",)),
    ("L2: check drops its lockout read", _FIX_AUTH,
     "  if (canary::net::auth_backoff::locked(m_table, source, now)) {\n"
     "    httpd_resp_set_status(req, \"429 Too Many Requests\");\n    return false;\n  }\n",
     "", ("AuthManager::check changed",)),
    ("L3: the page clears the backoff without a tap", _FIX_NET,
     "  } else if (tap_spent) {\n    auth_handed_token(request_peer_ipv4(req));\n  }\n",
     "  }\n  auth_handed_token(request_peer_ipv4(req));\n",
     ("page_token_inject changed",)),
    ("L4: the receipt clears the backoff on a refusal", _FIX_NET,
     "  if (verdict == ReceiptVerdict::REFUSE_NO_TAP) {\n",
     "  if (verdict == ReceiptVerdict::REFUSE_NO_TAP) {\n"
     "    auth_handed_token(request_peer_ipv4(req));\n",
     ("calls auth_handed_token outside a SERVE_* test",)),
    ("L5: a handler reaches the instance", _FIX_NET,
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n",
     "static esp_err_t handle_wifi_connect2(httpd_req_t* req) {\n"
     "  if (!auth_gate(req)) return ESP_OK;\n"
     "  auth_get_instance().handedToken(request_peer_ipv4(req));\n  return ESP_OK;\n}\n"
     "static esp_err_t handle_fleet(httpd_req_t* req) {\n",
     ("auth_get_instance called from handle_wifi_connect2",)),
    ("L5: a raw compare outside the lib's two", _FIX_AUTH, "",
     "bool auth_peek(const char* t, size_t n) {\n"
     "  return AuthManager::constantTimeCompare(t, auth_get_token(), n);\n}\n",
     ("constantTimeCompare called from auth_peek", "auth_peek() reads the bearer token and is no")),
    ("L6: the lockout keyed on X-Forwarded-For", _FIX_NET,
     "static uint32_t request_peer_ipv4(httpd_req_t* req) {\n  return peer_of(req);\n}\n",
     "static uint32_t request_peer_ipv4(httpd_req_t* req) {\n  char xff[48];\n"
     "  if (httpd_req_get_hdr_value_str(req, \"X-Forwarded-For\", xff, sizeof(xff)) == ESP_OK)\n"
     "    return parse_ip(xff);\n  return peer_of(req);\n}\n",
     ("a header not in REQUEST_HEADERS",)),
    ("S1: the page sets a session cookie", _FIX_NET,
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     "  httpd_resp_set_hdr(req, \"Set-Cookie\", \"cv_session=1\");\n"
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     ("sets the response header '\"Set-Cookie\"'", "ANY_SESSION_GATES", "F328")),
    ("S1: the page sets one, lower case", _FIX_NET,
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     "  httpd_resp_set_hdr(req, \"set-cookie\", \"cv_session=1\");\n"
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     ("sets the response header '\"set-cookie\"'",)),
    ("S1: a header name that is no literal", _FIX_NET,
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     "  httpd_resp_set_hdr(req, kSession, \"cv_session=1\");\n"
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     ("sets the response header 'kSession'",)),
    ("S1: a handler reads the Cookie header", _FIX_NET,
     "  ScvNetworkManager& net = network_get_instance();\n  JsonDocument doc;\n"
     "  doc[\"provisioning_gate\"]",
     "  char c[64];\n  httpd_req_get_hdr_value_str(req, \"Cookie\", c, sizeof(c));\n"
     "  ScvNetworkManager& net = network_get_instance();\n  JsonDocument doc;\n"
     "  doc[\"provisioning_gate\"]",
     ("reads the request header '\"Cookie\"'",)),
    ("S1: a handler reads a cookie", _FIX_NET,
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     "  char v[40];\n  size_t n = sizeof(v);\n"
     "  httpd_req_get_cookie_val(req, \"cv_session\", v, &n);\n"
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     ("reads a cookie (httpd_req_get_cookie_val)",)),
    ("P12: compare reads a query credential", _FIX_AUTH,
     "  char auth_buf[128];\n",
     "  char auth_buf[128];\n  char q[64];\n"
     "  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) return Presented::RIGHT;\n",
     ("AuthManager::compare reads the URL query",)),
    ("P12: /api/status reads the URL query", _FIX_NET,
     "  if (!auth_gate(req)) return ESP_OK;\n  ScvNetworkManager& net",
     "  if (!auth_gate(req)) return ESP_OK;\n  (void)httpd_req_get_url_query_len(req);\n"
     "  ScvNetworkManager& net",
     ("handle_status reads the URL query",)),
    ("M13: a helper prints the SoftAP password", _FIX_NET, "",
     "static void dump_ap() {\n  Serial.println(network_get_instance().getApPassword());\n}\n",
     ("dump_ap() reads the SoftAP password and is no",)),
    ("a helper prints the token", _FIX_NET, "",
     "static void dump(void) {\n  Serial.println(auth_get_token());\n}\n",
     ("dump() reads the bearer token and is no",)),
    ("A10: setup hands the SoftAP password to the BLE label", _FIX_MAIN,
     "  network_set_provisioning_gate_hooks(",
     "  ble_status_set_label(g_ap_password);\n  network_set_provisioning_gate_hooks(",
     ("setup() reads the SoftAP password in a statement not in its SECRET_LOCAL_USES row",
      "Now: (")),
    ("A11: raiseAp publishes it over MQTT", _FIX_NET,
     "  announce_setup_network(m_ap_ssid, m_ap_password);\n",
     "  announce_setup_network(m_ap_ssid, m_ap_password);\n"
     "  mqtt_publish_state(\"ap\", m_ap_password);\n",
     ("ScvNetworkManager::raiseAp() reads the SoftAP password in a statement not in its "
      "SECRET_LOCAL_USES row",)),
    ("mint drift: the SoftAP key from the public key", _FIX_MAIN,
     "derive_ap_password(device.privkey,", "derive_ap_password(device.pubkey,",
     ("setup()'s SECRET_LOCAL_USES row holds", "in a statement not in its")),
    ("mint: the result discarded", _FIX_MAIN,
     "  if (!canary::net::derive_ap_password(device.privkey, g_ap_password, "
     "sizeof(g_ap_password))) {\n"
     "    Serial.println(\"[!!] Setup network key derivation failed - HALTING\");\n"
     "    while (true) { delay(1000); }\n  }\n",
     "  canary::net::derive_ap_password(device.privkey, g_ap_password, sizeof(g_ap_password));\n",
     ("setup()'s SECRET_LOCAL_USES row holds",)),
    ("mint: the halt dropped", _FIX_MAIN, "    while (true) { delay(1000); }\n", "",
     ("setup()'s SECRET_LOCAL_USES row holds",)),
    ("mint: the call deleted", _FIX_MAIN,
     "  if (!canary::net::derive_ap_password(device.privkey, g_ap_password, "
     "sizeof(g_ap_password))) {\n"
     "    Serial.println(\"[!!] Setup network key derivation failed - HALTING\");\n"
     "    while (true) { delay(1000); }\n  }\n", "",
     ("setup()'s SECRET_LOCAL_USES row holds",)),
    ("mint: the token's refusal dropped", _FIX_AUTH,
     "    s_bearer[0] = '\\0';\n    return false;\n", "    s_bearer[0] = '\\0';\n",
     ("auth_load_or_derive()'s SECRET_LOCAL_USES row holds",)),
    # The roll-call calls a `begin(` (the tree's does, through Preferences);
    # ScvNetworkManager::begin's local-use row is keyed by its file and
    # class, so a `begin` elsewhere that reads a credential is not excused.
    ("keyed rows: another file's begin answers the SoftAP password", _FIX_OTHER, "",
     "const char* Other::begin(const char* ns, bool ro) {\n"
     "  return network_get_instance().getApPassword();\n}\n",
     ("Other::begin() reads the SoftAP password and is no", _PUBLIC_AP)),
    ("keyed rows: the same member in another file", _FIX_OTHER, "",
     "bool ScvNetworkManager::begin(const char* x) {\n  return m_ap_password[0] != x[0];\n}\n",
     ("ScvNetworkManager::begin() reads the SoftAP password and is no", _PUBLIC_AP)),
    ("a credential outside every function", _FIX_NET, "",
     "static const char* kLeak = auth_get_token();\n",
     ("a credential named outside every function ('static const char* kLeak",)),
    ("the page answers the token by itself", _FIX_NET,
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     "  httpd_resp_set_hdr(req, \"X-T\", auth_get_token());\n"
     "  return send_html_with_token(req, UI_HTML, page_token_inject(req));\n",
     ("handle_ui() reaches a credential through auth_get_token, not the page path",)),
    ("the status answers before its refusal", _FIX_NET,
     "  if (!rate_limit_check(req)) return ESP_OK;\n  if (!auth_gate(req)) return ESP_OK;\n",
     "  if (!rate_limit_check(req)) return ESP_OK;\n"
     "  if (network_get_instance().getTlsCertFp()[0]) return ESP_OK;\n"
     "  if (!auth_gate(req)) return ESP_OK;\n",
     ("reaches a credential (getTlsCertFp) before its refusal",)),
    ("the receipt answers outside a served verdict", _FIX_NET,
     "  if (verdict == ReceiptVerdict::SERVE_BEARER) {\n",
     "  Serial.println(network_get_instance().getApPassword());\n"
     "  if (verdict == ReceiptVerdict::SERVE_BEARER) {\n",
     ("reaches a credential (getApPassword) outside a SERVE_* test",)),
    ("the receipt stops carrying the pin", _FIX_NET,
     "  doc[\"tls_cert_fp\"] = net.getTlsCertFp();\n  return send_doc(req, doc);\n}\n"
     "static esp_err_t handle_provisioning_receipt",
     "  return send_doc(req, doc);\n}\nstatic esp_err_t handle_provisioning_receipt",
     ("emitter send_provisioning_receipt() carries the bearer token, the SoftAP password; its "
      "TOKEN_EMITTERS entry says", "handle_provisioning_receipt() carries the bearer token, "
      "the SoftAP password; its SECRET_ANSWERS entry says")),
    ("a comparer reads the SoftAP password", _FIX_NET,
     "  return auth_bearer_grant(req, token, request_peer_ipv4(req)) ==\n",
     "  (void)network_get_instance().getApPassword();\n"
     "  return auth_bearer_grant(req, token, request_peer_ipv4(req)) ==\n",
     ("comparer bearer_present_and_valid() reads the bearer token, the SoftAP password",)),
    ("an accessor defined twice", _FIX_OTHER, "",
     "const char* getApPassword() {\n  return \"\";\n}\n",
     ("SECRET_ACCESSORS: getApPassword() is defined in",)),
    ("a gate primitive called outside every function", _FIX_NET, "",
     "static bool g_ok = auth_check(nullptr, \"\", 0);\n",
     ("auth_check() called outside every function",)),
    ("the hooks wired twice", _FIX_MAIN,
     "  network_set_provisioning_gate_hooks(prov_gate_take_hook, prov_gate_is_open_hook);\n",
     "  network_set_provisioning_gate_hooks(prov_gate_take_hook, prov_gate_is_open_hook);\n"
     "  network_set_provisioning_gate_hooks(prov_gate_take_hook, prov_gate_is_open_hook);\n",
     ("network_set_provisioning_gate_hooks is called 2 times",)),
    ("the take hook's variable set outside its setter", _FIX_NET,
     "static network_gate_fn_t s_gate_take = nullptr;\n",
     "static network_gate_fn_t s_gate_take = prov_gate_is_open_hook;\n",
     ("s_gate_take named outside every function other than in its declaration",)),
)


def _with(key: str, change):
    """A table plant: `change` applied to a copy of the fixture table `key`."""
    def plant(tables):
        tables = dict(tables)
        tables[key] = change(tables.get(key))
        return tables
    return plant


_FIX_TABLE_PLANTS = (
    ("a stale marker left in AUTH_MARKERS (F340 deleted auth_check_optional)",
     _with("markers", lambda m: tuple(m or AUTH_MARKERS) + ("auth_check_optional(",)),
     ("AUTH_MARKERS entry 'auth_check_optional(' names no GATE_CALL_SITES row",)),
    ("a PRE_HOST_STEPS entry naming nothing", _with("steps", lambda s: tuple(s or PRE_HOST_STEPS)
                                                    + ("auth_check_optional(",)),
     ("PRE_HOST_STEPS entry 'auth_check_optional(' names no GATE_CALL_SITES row",)),
    ("a SECRET_ANSWERS row naming no handler",
     _with("answers", lambda a: dict(a, handle_gone=("token", "page"))),
     ("SECRET_ANSWERS names handle_gone(), which is no registered route handler",)),
    ("a grant that is no grant",
     _with("answers", lambda a: dict(a, handle_ui=("token", "session"))),
     ("gives handle_ui() the grant 'session'",)),
    ("a public route with the bearer grant",
     _with("public", lambda p: dict(p) | {("GET", "/api/status"): "x"}),
     ("handle_status() is a PUBLIC_ALLOWLIST route handler with the 'bearer' grant",)),
    ("a local-use row for a function that is gone",
     _with("local", lambda u: dict(u) | {(_NET, "ScvNetworkManager::gone"): ("x", ())}),
     ("SECRET_LOCAL_USES names ScvNetworkManager::gone() in securacv_network.cpp, which does "
      "not define it",)),
    ("a local-use row reading nothing",
     _with("local", lambda u: dict(u) | {(_NET, "ScvNetworkManager::idle"): ("x", ())}),
     ("SECRET_LOCAL_USES names ScvNetworkManager::idle() in securacv_network.cpp, which reads "
      "no credential",)),
    ("a local-use row naming a route handler",
     _with("local", lambda u: dict(u) | {(_NET, "handle_status"): (
         "x", ("doc[\"tls_cert_fp\"] = net.getTlsCertFp();",))}),
     ("handle_status() is a route handler, so it answers",)),
    ("a GLUE_DEFS row whose function is gone",
     _with("glue", lambda g: dict(g) | {(_NET, "gone_glue"): ("x", "y")}),
     ("GLUE_DEFS pins gone_glue() in securacv_network.cpp, which defines it 0 times",)),
    ("a census row with no call",
     _with("census", lambda c: dict(c or GATE_CALL_SITES) | {"gone_gate": {"callers": None}}),
     ("GATE_CALL_SITES row gone_gate has no call",)),
    ("a census caller that no longer calls",
     _with("census", lambda c: dict(c or GATE_CALL_SITES) | {"auth_check": {
         "callers": {(_NET, "auth_gate"), (_NET, "handle_status")}}}),
     ("GATE_CALL_SITES row auth_check names handle_status (securacv_network.cpp), which no "
      "longer calls it",)),
    ("an OUTSIDE_FUNCTION_LINES row naming nothing",
     _with("outside", lambda o: tuple(o) + ((_NET, "char m_gone[1];"),)),
     ("OUTSIDE_FUNCTION_LINES holds 'char m_gone[1];'",)),
    ("an emitter row reading nothing",
     _with("emitters", lambda e: dict(e or TOKEN_EMITTERS) | {"send_gone": ("token", "x")}),
     ("TOKEN_EMITTERS names send_gone(), which reads no credential",)),
)


def _assert_answer_gate_rule() -> None:
    """The bearer-only answer rule: the head parser names what it reads, the
    clean fixture passes, and every plant fails for its named reason. Runs
    every time, before the tree."""
    # The head parser: a constructor is named for its class, its initializer
    # list included (the old parser named this body `b_`); a lambda names no
    # function, so grant's compare( is grant's; a brace-initialized member
    # and a destructor are named too.
    heads = ("Foo::Foo() : a_(0), b_(0) { x = auth_get_token(); }\n"
             "R A::grant(int s) { return g([&]() { return compare(s); }); }\n"
             "B::B() : m_t{}, n_(f(1)) { y(); }\nC::~C() { z(); }\n"
             "ns::Type D::get() const { return 1; }\nint free_fn(int a) { return a ? f(a) : 0; }\n")
    spans = function_spans(heads)
    assert [n for n, _s, _e in spans] == ["Foo::Foo", "A::grant", "B::B", "C::~C", "D::get",
                                          "free_fn"], spans
    for needle, owner in (("auth_get_token", "Foo::Foo"), ("compare(", "A::grant"),
                          ("y()", "B::B"), ("f(1)", "B::B")):
        assert enclosing_function(spans, heads.find(needle))[0] == owner, (needle, spans)
    assert definition_span("AuthManager::Presented AuthManager::compare(int a) { return a; }\n"
                           "bool constantTimeCompare(int b) { return b; }\n",
                           "AuthManager::compare")[0] == 23
    assert definition_span("bool constantTimeCompare(int b) { return b; }\n",
                           "AuthManager::compare") is None
    clean = _fixture_sources()
    tables = _fixture_tables(clean)
    got = check_bearer_only_answers(clean, tables)
    assert got == [], "the bearer-only answer rule's clean fixture fails: " + "; ".join(got)
    for what, fname, old, new, words in _FIX_PLANTS:
        planted = _fixture_sources()
        text = planted.get(fname, "")
        assert old in text, f"fixture plant {what!r}: its text is not in {fname}"
        planted[fname] = text.replace(old, new, 1) if old else text + new
        got = check_bearer_only_answers(planted, tables)
        missing = [w for w in words if not any(w in p for p in got)]
        assert not missing, (f"the bearer-only answer rule passed a plant ({what}): expected "
                             f"problems naming {missing!r}, got {got!r}")
    for what, plant, words in _FIX_TABLE_PLANTS:
        got = check_bearer_only_answers(clean, plant(tables))
        missing = [w for w in words if not any(w in p for p in got)]
        assert not missing, (f"the bearer-only answer rule passed a table plant ({what}): "
                             f"expected problems naming {missing!r}, got {got!r}")


def main() -> int:
    _assert_parser_robust()
    _assert_answer_gate_rule()
    sources = {}
    for root in ROOTS:
        for path in sorted(root.rglob("*.cpp")) + sorted(root.rglob("*.h")):
            rel = path.as_posix()
            sources[rel] = strip_comments(path.read_text(errors="replace"))

    routes = []
    for fname, text in sources.items():
        routes.extend(parse_routes(fname, text))

    failures = []
    handler_def = re.compile(r"esp_err_t\s+\w+\s*\(\s*httpd_req_t\s*\*")
    for fname, text in sorted(sources.items()):
        if handler_def.search(text) and re.search(r'\bR"[^(\s]*\(', text):
            failures.append(
                f"{fname} defines an httpd handler AND holds a raw string "
                f"literal — the brace matcher does not understand R\"...\"; "
                f"move the literal to its own file or teach blank_string_contents")

    if len(routes) < MIN_ROUTES:
        print(f"FAIL: parsed only {len(routes)} routes (floor {MIN_ROUTES}) — "
              f"parser regression?")
        return 1

    matched_allowlist = set()
    for uri, method, handler, fname in routes:
        if uri is None:
            failures.append(f"{method} <non-literal uri> → {handler} ({fname}): "
                            f"the parser cannot resolve this .uri; use a string "
                            f"literal or the probe-array loop form")
            continue
        key = (method, uri)
        if key in PUBLIC_ALLOWLIST:
            matched_allowlist.add(key)
            continue
        base = handler.split("::")[-1]
        if base in SELF_GATING_HANDLERS:
            continue
        bodies = [b for b in (function_body(t, base) for t in sources.values()) if b]
        if not bodies:
            failures.append(f"{method} {uri}: handler {handler} not found "
                            f"(registered in {fname})")
            continue
        present = any(marker in body for body in bodies for marker in AUTH_MARKERS)
        decides = any(marker_decides(body, marker)
                      for body in bodies for marker in AUTH_MARKERS)
        guarded = any(gate_guards(body) for body in bodies)
        if not present:
            failures.append(f"{method} {uri}: handler {handler} ({fname}) has NO "
                            f"credential gate and is not on the public allowlist")
        elif not decides:
            failures.append(f"{method} {uri}: handler {handler} ({fname}) calls a "
                            f"credential gate but ignores its result — use it in an "
                            f"if (...) condition or a return expression")
        elif not guarded:
            failures.append(f"{method} {uri}: handler {handler} ({fname}) returns "
                            f"unconditionally before its first credential gate — the "
                            f"gate is unreachable")

    failures.extend(check_page_token_wiring(sources))
    failures.extend(check_host_first(sources))
    failures.extend(check_bearer_only_answers(sources))
    failures.extend(check_registration_sites(sources))
    failures.extend(check_route_budgets(sources))

    for key in sorted(set(PUBLIC_ALLOWLIST) - matched_allowlist):
        failures.append(f"allowlist entry {key[0]} {key[1]} matches no registered "
                        f"route — remove or fix it")

    if failures:
        print(f"ROUTE SECURITY CHECK FAILED ({len(failures)} problem(s)):")
        for f in failures:
            print("  -", f)
        return 1

    print(f"canary route security OK: {len(routes)} registrations checked, "
          f"{len(matched_allowlist)} documented-public, rest gated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
