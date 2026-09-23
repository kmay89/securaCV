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

Beyond "every route reaches a gate", five wiring rules the marker scan alone
cannot see (each added after a review mutation, or a review finding, passed
every other gate):

  * a gate marker counts only where its RESULT decides something — inside an
    `if (...)` / `while (...)` condition or a `return` expression. A bare
    `auth_gate(req);` or `(void)bearer_present_and_valid(req);` ignores the
    answer and is not a gate;
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
    spend the BOOT tap (check_host_first): host_is_foreign(req) — the Host
    must name this device unless the request came over the SoftAP — decides
    before any bearer is read, any tap is taken or any token is emitted. A
    gated route gets it from auth_gate (whose own body is held to that
    order) or asks it itself (the provisioning receipt, gated by a bearer or
    the tap); page_token_inject asks it before page_token_decide may take
    the tap; send_html_with_token streams the token only on
    PageToken::INJECT; and every function that reads auth_get_token() is
    named here as an emitter or a comparer, so a new reader cannot appear
    unclassified (added with the provisioning receipt's own Host check; the
    four rules above were green without it).

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
    "auth_check(",              # AuthManager bearer check (401/403/429)
    "auth_check_optional(",     # silent bearer check; caller enforces the else
    "bearer_present_and_valid(",  # auth_check_optional + refuses an unprovisioned credential
    "provisioning_gate_take(",  # physical BOOT tap, one tap = one consumer
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
    # Two registrations share this key: the main table's captive wildcard
    # fallback (registered LAST: 302 → /setup while setup is active, else 404)
    # and, with FEATURE_HTTPS, the port-80 server's redirect to https://.
    # Neither serves content.
    ("GET", "/*"): "captive wildcard fallback / port-80 https redirect; serves nothing",
    ("POST", "/*"): "FEATURE_HTTPS port-80 server: 307 to https://, serves nothing (F15)",
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


def definition_span(text: str, name: str):
    """(start, end) of the brace-matched DEFINITION of any function `name`
    (any return type, member or free), or None. A call is `name(...)`
    followed by `;` or `)`; only a definition is followed by `{`."""
    if name not in text:
        return None
    blanked = blank_string_contents(text)
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*\(", blanked):
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
            return (m.start(), k)
    return None


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


def first_deciding_marker(body: str, marker: str) -> int:
    """Offset of the first occurrence of `marker` whose result decides
    something — inside an if/while condition or a return expression — or -1.
    The statement's text before the marker (back to the previous ';', '{' or
    '}') must open with `if (`, `else if (`, `while (` or `return`. A bare
    call or a `(void)` cast discards the answer and does not count."""
    blanked = blank_string_contents(body)
    start = 0
    while True:
        i = blanked.find(marker, start)
        if i < 0:
            return -1
        start = i + 1
        stmt = max(blanked.rfind(";", 0, i), blanked.rfind("{", 0, i),
                   blanked.rfind("}", 0, i))
        prefix = blanked[stmt + 1:i].strip()
        if re.match(r"^(?:else\s+)?(?:if|while)\s*\(|^return\b", prefix):
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
                    f"{third!r} as `inject` — it must be page_token_inject(req) "
                    f"(a literal true hands the bearer to every home-LAN "
                    f"caller; that reverts F20 gap #11)")
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
# host_is_foreign over firmware/common/network/host_guard.h).
HOST_CHECK = "host_is_foreign("

# The host-tested decisions that take the Host verdict as their FIRST
# argument (firmware/common/network/provisioning_gate.h) and decide it before
# anything else they are given.
HOST_DECISIONS = ("receipt_decide", "page_token_decide")

# Functions that put a secret into a response. Name → what they emit. Every
# call of one must sit behind the Host check (or, for the page, behind
# page_token_inject, which asks it).
TOKEN_EMITTERS = {
    "send_provisioning_receipt": "the provisioning receipt (the API token and the AP password)",
    "send_html_with_token": "a page with the API token in its __CV_TOKEN__ placeholder",
}

# Functions that read auth_get_token() only to compare a presented credential
# against it, never into a response.
TOKEN_COMPARERS = {
    "auth_gate": "the API gate: the Host, then the bearer compare",
    "bearer_present_and_valid": "the silent bearer compare the receipt and the pages use",
}

# What must not run before the Host is decided: reading the bearer, taking
# the tap, reading the token, emitting it.
PRE_HOST_STEPS = ("bearer_present_and_valid(", "auth_check_optional(", "auth_check(",
                  "provisioning_gate_take(", "auth_get_token(") + tuple(
                      name + "(" for name in TOKEN_EMITTERS)

_KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "sizeof", "defined"}


def function_spans(text: str):
    """[(name, start, end)] for every brace-bodied function definition in
    `text` whose parameter list holds no parentheses (every handler and
    helper here). A lambda has no name before its `(`, so a statement inside
    one is attributed to the function that holds the lambda."""
    blanked = blank_string_contents(text)
    spans = []
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\([^;{}()]*\)\s*(?:const\s*)?\{", blanked):
        if m.group(1) in _KEYWORDS:
            continue
        depth, k = 1, m.end()
        while k < len(blanked) and depth:
            if blanked[k] == "{":
                depth += 1
            elif blanked[k] == "}":
                depth -= 1
            k += 1
        spans.append((m.group(1), m.start(), k))
    return spans


def enclosing_function(spans, offset: int):
    """The innermost (name, start, end) of `spans` holding `offset`, or None."""
    best = None
    for name, start, end in spans:
        if start <= offset < end and (best is None or start > best[1]):
            best = (name, start, end)
    return best


def first_host_decision(body: str) -> int:
    """Offset in `body` of the first host_is_foreign(...) whose answer
    decides, or -1: in an if/while condition or a return expression
    (first_deciding_marker); passed straight in as the first argument of a
    HOST_DECISIONS call; or assigned to a bool local that is then that first
    argument, or tested in an if (...). A bare call, or a local nobody
    reads, is not a Host check."""
    blanked = blank_string_contents(body)
    found = []
    i = first_deciding_marker(body, HOST_CHECK)
    if i >= 0:
        found.append(i)
    local = re.compile(r"\b(?:const\s+)?bool\s+([A-Za-z_]\w*)\s*=\s*host_is_foreign\s*\(\s*req\s*\)\s*;")
    for fn in HOST_DECISIONS:
        for off, args in call_args(body, fn):
            if not args:
                continue
            if re.fullmatch(r"host_is_foreign\s*\(\s*req\s*\)", args[0]):
                found.append(blanked.find(HOST_CHECK, off))
                continue
            for m in local.finditer(blanked[:off]):
                if m.group(1) == args[0]:
                    found.append(m.start() + m.group(0).find(HOST_CHECK))
    for m in local.finditer(blanked):
        tail = blanked[m.end():]
        if re.search(r"\b(?:if|while)\s*\(\s*!?\s*" + re.escape(m.group(1)) + r"\b", tail):
            found.append(m.start() + m.group(0).find(HOST_CHECK))
    return min(found) if found else -1


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

    # 1. auth_gate asks the Host before it reads or compares the token.
    fname, body = body_of("auth_gate")
    if body is None:
        if require_defs:
            failures.append("auth_gate definition not found — parser regression?")
    else:
        h = first_deciding_marker(body, HOST_CHECK)
        if h < 0:
            failures.append(f"{fname}: auth_gate no longer refuses a foreign Host "
                            f"(if (host_is_foreign(req)) ...) — every auth_gate route "
                            f"would serve under a rebound name")
        else:
            late = steps_before(body, h, ("auth_get_token(", "auth_check(", "auth_check_optional("))
            if late:
                failures.append(f"{fname}: auth_gate reads the token ({', '.join(late)}) "
                                f"before its Host check")

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
        h = first_host_decision(body)
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

    # 4. send_html_with_token streams the token only on PageToken::INJECT.
    fname, body = body_of("send_html_with_token")
    if body is None:
        if require_defs:
            failures.append("send_html_with_token definition not found — parser regression?")
    else:
        blanked = blank_string_contents(body)
        for m in re.finditer(r"\bauth_get_token\s*\(", blanked):
            stmt_start = max(blanked.rfind(";", 0, m.start()), blanked.rfind("{", 0, m.start()),
                             blanked.rfind("}", 0, m.start())) + 1
            stmt_end = blanked.find(";", m.start())
            if not re.search(r"==\s*PageToken::INJECT\s*\)?\s*\?\s*auth_get_token\s*\(",
                             blanked[stmt_start:stmt_end]):
                failures.append(f"{fname}: send_html_with_token reads auth_get_token() "
                                f"outside a PageToken::INJECT test (`verdict == "
                                f"PageToken::INJECT ? auth_get_token() : \"\"`) — only "
                                f"that verdict (Host first, host-tested) may put the "
                                f"token in a page")

    # 5. Every call of an emitter (other than the page's, rule 3) sits behind
    #    the Host check in its own function; every token reader is named.
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
                                    f"{TOKEN_EMITTERS[emitter]}, without deciding "
                                    f"host_is_foreign(req) (or auth_gate) first")
        for off, _ in call_args(text, "auth_get_token"):
            enc = enclosing_function(spans, off)
            if enc is None:
                continue  # the declaration
            if enc[0] not in TOKEN_EMITTERS and enc[0] not in TOKEN_COMPARERS:
                line = text.count("\n", 0, off) + 1
                failures.append(f"{fname}:{line}: {enc[0]} reads auth_get_token() but is "
                                f"neither a TOKEN_EMITTERS nor a TOKEN_COMPARERS entry — "
                                f"name it, and if it puts the token in a response, route "
                                f"every caller through the Host check")
    if require_defs:
        for table in (TOKEN_EMITTERS, TOKEN_COMPARERS):
            for name in table:
                if not any(definition_span(t, name) for t in sources.values()):
                    failures.append(f"{name} is listed as a token reader but is not "
                                    f"defined — remove or fix the entry")
    return failures


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
               "return auth_check_optional(req, token);"):
        assert marker_decides("{ " + ok, next(m for m in AUTH_MARKERS if m in ok)), ok
    for bad in ("(void)bearer_present_and_valid(req); return send(req);",
                "auth_gate(req); return ESP_OK;",
                "bool ok = auth_gate(req); return send(req);"):
        assert not marker_decides("{ " + bad, next(m for m in AUTH_MARKERS if m in bad)), bad
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

    # 6. The Host first: receipt shapes the rule refuses, and the fixed ones.
    reg = ('httpd_uri_t r = { .uri = "/api/provisioning-receipt", .method = HTTP_GET, '
           '.handler = handle_provisioning_receipt };\n')
    emit = ("static esp_err_t send_provisioning_receipt(httpd_req_t* req) {\n"
            "  doc[\"token\"] = auth_get_token();\n  return ESP_OK;\n}\n")
    def receipt(body):
        return {"fx": reg + emit + "static esp_err_t handle_provisioning_receipt("
                "httpd_req_t* req) {\n" + body + "}\n"}
    no_host = ("  if (bearer_present_and_valid(req)) return send_provisioning_receipt(req);\n"
               "  if (!provisioning_gate_take()) return ESP_OK;\n"
               "  return send_provisioning_receipt(req);\n")
    got = check_host_first(receipt(no_host), require_defs=False)
    assert any("never asks the Host" in f for f in got), got
    late = ("  if (bearer_present_and_valid(req)) return send_provisioning_receipt(req);\n"
            "  if (host_is_foreign(req)) return ESP_OK;\n"
            "  if (!provisioning_gate_take()) return ESP_OK;\n"
            "  return send_provisioning_receipt(req);\n")
    got = check_host_first(receipt(late), require_defs=False)
    assert any("before the Host check" in f and "bearer_present_and_valid" in f
               for f in got), got
    ignored = "  host_is_foreign(req);\n" + no_host
    got = check_host_first(receipt(ignored), require_defs=False)
    assert any("never asks the Host" in f for f in got), got
    unread = "  const bool foreign = host_is_foreign(req);\n" + no_host
    got = check_host_first(receipt(unread), require_defs=False)
    assert any("never asks the Host" in f for f in got), got
    fixed_if = "  if (host_is_foreign(req)) return ESP_OK;\n" + no_host
    assert check_host_first(receipt(fixed_if), require_defs=False) == []
    fixed_decide = (
        "  const ReceiptVerdict v = receipt_decide(host_is_foreign(req),\n"
        "      [req]() { return bearer_present_and_valid(req); },\n"
        "      []() { return provisioning_gate_take(); });\n"
        "  if (v == ReceiptVerdict::REFUSE_HOST) return ESP_OK;\n"
        "  return send_provisioning_receipt(req);\n")
    assert check_host_first(receipt(fixed_decide), require_defs=False) == []
    # ...an emitter called from an unregistered helper is held to it too.
    helper = {"fx": emit + "static esp_err_t other(httpd_req_t* req) {\n"
              "  return send_provisioning_receipt(req);\n}\n"}
    got = check_host_first(helper, require_defs=False)
    assert any("other calls send_provisioning_receipt" in f for f in got), got

    # 7. The page path: the Host before the grants and the tap.
    def inject(body):
        return {"fx": "static PageToken page_token_inject(httpd_req_t* req) {\n" + body + "}\n"}
    old_inject = ("  const bool bearer_ok = bearer_present_and_valid(req);\n"
                  "  return page_token_decide(false, bearer_ok, false,\n"
                  "      []() { return provisioning_gate_take(); });\n")
    got = check_host_first(inject(old_inject), require_defs=False)
    assert any("does not decide the Host first" in f for f in got), got
    late_inject = ("  const bool bearer_ok = bearer_present_and_valid(req);\n"
                   "  const bool foreign = host_is_foreign(req);\n"
                   "  return page_token_decide(foreign, bearer_ok, false,\n"
                   "      []() { return provisioning_gate_take(); });\n")
    got = check_host_first(inject(late_inject), require_defs=False)
    assert any("before the Host check" in f for f in got), got
    good_inject = ("  const bool foreign = host_is_foreign(req);\n"
                   "  const bool bearer_ok = !foreign && bearer_present_and_valid(req);\n"
                   "  return page_token_decide(foreign, bearer_ok, false,\n"
                   "      []() { return provisioning_gate_take(); });\n")
    assert check_host_first(inject(good_inject), require_defs=False) == []

    # 8. The page streams the token only on INJECT; every reader is named.
    old_send = {"fx": "static esp_err_t send_html_with_token(httpd_req_t* req, "
                "const char* h, bool inject) {\n"
                "  const char* token = (inject && !foreign) ? auth_get_token() : \"\";\n"
                "  return ESP_OK;\n}\n"}
    got = check_host_first(old_send, require_defs=False)
    assert any("outside a PageToken::INJECT test" in f for f in got), got
    new_send = {"fx": old_send["fx"].replace("(inject && !foreign)",
                                             "(verdict == PageToken::INJECT)")}
    assert check_host_first(new_send, require_defs=False) == []
    for wrong in ("(verdict != PageToken::WITHHOLD)", "(verdict != PageToken::INJECT)"):
        bad_send = {"fx": old_send["fx"].replace("(inject && !foreign)", wrong)}
        got = check_host_first(bad_send, require_defs=False)
        assert any("outside a PageToken::INJECT test" in f for f in got), (wrong, got)
    stray = {"fx": "static void dump(void) {\n  Serial.println(auth_get_token());\n}\n"}
    got = check_host_first(stray, require_defs=False)
    assert any("dump reads auth_get_token()" in f for f in got), got


def main() -> int:
    _assert_parser_robust()
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
