#!/usr/bin/env python3
"""Route-security conformance check for the canary-wap firmware.

Every HTTP route the sketch registers must be credential-gated — Bearer
token, session cookie, pair token, or the physical provisioning gate —
unless it is on the explicit PUBLIC allowlist below with a written
justification. This pins the fail-closed property statically so a new
endpoint (or a resurrected one: three /api/ble routes shipped without
auth and were only unreachable because they overflowed the handler
table) cannot silently widen the unauthenticated surface.

It also holds the routes whose answer carries a secret (sweep F289): the
bearer token, the SoftAP password, the TLS pin, the broker password. The
landing page at / mints a pair token for any caller that reaches the WAP, on
the home network too and at any time, not only during setup, and its Open
dashboard link trades that token for a 24 h cv_session: the owner's chosen
LAN posture for the dashboard's own routes. So a session is no proof of
holding the token, and such a route takes a bearer-only gate
(api_auth_bearer_grant / api_auth_check_bearer), or the receipt's BOOT tap
or setup-door claim, never a gate a session passes (ANY_SESSION_GATES and,
to a fixed point, every function that calls one). What it reads:

  - canary_wap.ino: which functions read a secret (SECRET_READS, the token a
    gate compares against aside) or are handed one (SECRET_HANDS), carried up
    the call graph to the route handlers that reach them, stopping at a named
    device-side use (SECRET_LOCAL_USES). A handler that carries a secret is
    named in SECRET_ANSWERS with exactly what it carries. Its bearer gate is
    the whole condition of an `if` whose branch returns, `if (!gate(req,
    g_device.api_token_str)) return ...;` (a refusal at the top of the body)
    or `if (gate(...)) { ... return ...; }` (a grant), and each answer site
    sits after a refusal or inside a grant (OTHER_GRANTS adds the receipt's
    tap-and-claim refusal, whose flag only the claim's take may set). The
    handler and the functions it reaches the secret through name no gate in
    the any-session class, called or not, and hold no goto or preprocessor
    line.
  - csi_mqtt.cpp: only BROKER_PASSWORD_USES name the stored broker password,
    and POST /api/mqtt/config's first write of the row or the CA is preceded
    by a returning `if` whose whole condition is the move gate
    (move_needs_password(), #1835), handed a `const bool` declared at the top
    of the handler whose `&&` terms include a bearer gate's call; neither
    that condition, nor the bearer, nor the gate's own body names an
    any-session gate. So a save that moves the kept password takes the
    bearer header or the password typed again. The route's own session gate
    for the rest of the save stays: the dashboard's.
  - Each bearer gate is defined once, in its BEARER_GATES home, and names no
    any-session gate.
  - Every bearer compare is accounted (sweep F339): the raw compares
    (RAW_COMPARES: api_auth_bearer_valid(), constant_time_compare()) are
    called only in RAW_COMPARE_HOME, api_auth.h, under its accounted grants.
    Any other sketch file that names one, outside a comment or a string,
    fails, by file and line. There is no allowlist: a route that compares a
    bearer goes through api_auth_bearer_grant(), api_auth_check_bearer() or a
    gate built on them, so a wrong one is a strike against its address.
  - The modules' own copies of the token (csi_integration's g_api_token,
    csi_mqtt's provider, each register_routes module's auth_token_storage()):
    named only by the functions and macros MODULE_TOKEN_COPIES lets keep it,
    and in csi_mqtt's handlers only read into a local that is handed to a
    gate, as `gate(req, tok)` or `gate(req, tok, &held)` (the grant's held
    out-parameter, sweep F339), or tested against nullptr, and nothing else.
    Every module canary_wap.ino hands the token to has an entry.

It is a text check, and names are matched by spelling: a helper or a local
named like a gate in the class fails it, and a secret reached through a name
it does not know (a copy in a global, a member under another name, a whole
struct handed out, a copy a module's keeper passes on, a gate called through
an object or a pointer) passes it. Whether a gate decides right, whether the
receipt's tap and claim are right, and what the move gate counts as a move
are the host suites' (test_receipt_grant, test_receipt_authority,
test_mqtt_reinit). Its fixtures (_assert_secret_rules) run first, every
run: a clean sketch that passes and twenty-nine plants that each fail it.

Run from the repo root (CI: firmware.yml host tests):
    python3 firmware/projects/canary-wap/tests_host/check_route_security.py
"""

import functools
import re
import sys
from pathlib import Path

SKETCH = Path("firmware/projects/canary-wap/arduino/canary_wap")

# Substrings whose presence in a handler body (or its registration
# expression) counts as a credential gate. Each is a real call the auth
# audit verified enforces access.
AUTH_MARKERS = (
    "api_auth_check(",            # Bearer + session cookie (401/403/429)
    "api_auth_check_bearer(",     # Bearer ONLY, never the cookie (F289: the
                                  # pairing QR, whose answer carries the token)
    "api_auth_check_or_query(",   # + ?token= for <img>/stream tags
    "api_auth_check_optional(",   # the cookie or the accounted grant, nothing
                                  # sent (caller enforces)
    "api_auth_bearer_valid(",     # the raw compare: only api_auth.h calls it
                                  # (RAW_COMPARES, sweep F339)
    "api_auth_bearer_grant(",     # the same under the lockout, nothing sent
                                  # (F289: the receipt's first grant; the tap
                                  # and the claim are the handler's own)
    "CSI_AUTH_OR_RETURN",         # csi_integration session/Bearer macro
    "pair_token_valid(",          # 10-min wizard pair token
    "wifi_change_authorize(",     # pair token OR lockout-throttled admin cred
    "qr_scan_request_allowed(",   # pair token OR admin credential
    "provisioning_gate_is_open(", # physical BOOT-press gate
    "session_validate_cookie(",   # cv_session cookie (self-gating pages)
    "authorized(req",             # csi_mqtt session/Bearer helper
    "auth_gated<",                # module trampolines (bt_/bcn_/plain)
)

# (method, uri) pairs that are intentionally unauthenticated. Every entry
# needs a reason; an entry that stops matching a registered route fails
# the check so the list can't rot.
PUBLIC_ALLOWLIST = {
    # Pages/static assets: the HTML is public, every API it calls is gated.
    ("GET", "/admin"): "static admin/settings page; all APIs it calls are gated",
    ("GET", "/settings"): "same page as /admin",
    ("GET", "/companion"): "wizard/companion PWA shell (BLE chars enforce pairing)",
    ("GET", "/companion-sw.js"): "service worker for /companion",
    ("GET", "/companion-manifest.webmanifest"): "PWA manifest",
    ("GET", "/manifest.webmanifest"): "PWA manifest (sense dashboard)",
    ("GET", "/sw.js"): "service worker (sense dashboard)",
    ("GET", "/sense"): "permanent 301 redirect to /",
    # Coarse fleet presence/health is deliberately public — the DISCOVERY.md
    # contract the Witness Wall emulator + Flasher read. Presence + a coarse
    # chain-OK flag, plus the optional coarse wellbeing words (presence/
    # occupants/breathing/seeing) a hub-shaped row MAY carry — the same facts
    # the fleet already tells anyone in radio range over the BLE v2 beacon,
    # words only, absent unless honestly known. Nothing finer rides here: no
    # range, no lux, no vital-sign numbers, no secrets, no media, no evidence
    # (that stays behind the Bearer-gated /api/fleet-scan and the break-glass
    # evidence paths). The WAP itself never fills the wellbeing keys.
    ("GET", "/api/fleet"): "public coarse fleet presence/health/wellbeing words (DISCOVERY.md); no secrets/media",
    ("OPTIONS", "/api/fleet"): "CORS preflight for the public /api/fleet contract",
    # Device identity is deliberately public (pubkey + fingerprint).
    ("GET", "/api/device/enroll"): "public device identity (pubkey/fingerprint)",
    ("GET", "/enroll"): "human-readable enroll page for the same data",
    ("GET", "/api/device-info"): "non-sensitive metadata; advertises auth_required",
    # Wizard pre-credential surface: the SoftAP password is the boundary.
    ("GET", "/api/wifi"): "wizard status poll before any token exists (documented)",
    ("GET", "/api/wifi/scan"): "wizard network list before any token exists",
    ("GET", "/api/selftest"): "wizard step-5 health check on the AP (no secrets)",
    ("GET", "/api/help-qr"): "Help Desk deep-link QR for the current verdict; "
        "public website URL with a coarse anchor only — strictly less than "
        "/api/selftest already answers on this surface (no device id, no "
        "network name, no token; composition pinned in help_qr_logic.h)",
    # OS captive-portal probes must answer plainly or the OS disconnects.
    ("GET", "/hotspot-detect.html"): "Apple captive probe",
    ("GET", "/library/test/success.html"): "Apple captive probe (legacy)",
    ("GET", "/generate_204"): "Android captive probe",
    ("GET", "/gen_204"): "Android captive probe (legacy)",
    ("GET", "/connecttest.txt"): "Windows NCSI probe",
    ("GET", "/ncsi.txt"): "Windows NCSI probe (legacy)",
    ("GET", "/*"): "port-80 redirect-to-HTTPS catch-all",
    ("POST", "/*"): "port-80 redirect-to-HTTPS catch-all",
}

# Handlers that gate internally in ways the marker scan can't attribute to
# a single route (multi-branch pages). Name → reason.
SELF_GATING_HANDLERS = {
    "handle_ui": "cookie → dashboard; else pair-landing; wizard branch Host-gated",
    "handle_legacy_ui": "serves static HTML only",
    "handle_captive_probe": "returns fixed probe responses",
    "handle_https_redirect": "301 to https at the request's Host when it is an IP literal or the device's own name, else receipt_authority.h's address (503 when it has none); no data",
    "handle_tune_page": "session-cookie check, 302 to / when absent",
    "handle_wifi_pair_token": "gated on setup_wizard::is_active + direct Host",
}


# ── Routes whose answer carries a secret (sweep F289) ─────────────────────
#
# The gates that take only a real `Authorization: Bearer` header, each with
# the one file that defines it: a second definition by the same name (a
# module's own api_auth_bearer_valid over the cookie) would stand in unseen.
BEARER_GATES = {
    "api_auth_bearer_valid": "api_auth.h",  # the raw compare; only api_auth.h calls it (RAW_COMPARES)
    "api_auth_bearer_grant": "api_auth.h",  # the lockout, nothing sent: the receipt's first grant
    "api_auth_check_bearer": "api_auth.h",  # lockout + 401/403/429: the pairing QR
}
# The raw compares (sweep F339): each compares a presented token against the
# device's with no lockout, so outside api_auth.h a call of one is a token
# oracle that costs nothing. api_auth.h's accounted grants are their only
# callers; raw_compare_problems() holds that, with no allowlist.
RAW_COMPARES = ("api_auth_bearer_valid", "constant_time_compare")
RAW_COMPARE_HOME = "api_auth.h"
# The gates a landing-page caller passes: the cv_session cookie, every gate
# that takes it or a Bearer header, and the pair token the landing hands to
# any caller that loads it (send_pair_landing, at any time, not only during
# setup). The class is these and, to a fixed point, every function and
# macro in the sketch that names one (any_session_gates()), so a wrapper is
# in it by what it calls; qr_scan_request_allowed() and
# wifi_change_authorize() are, for example.
ANY_SESSION_GATES = (
    "cv_session_validate",      # the cookie (api_auth.h's weak hook)
    "session_validate_cookie",  # csi_integration's cookie check
    "api_auth_check",           # cookie OR Bearer
    "api_auth_check_optional",  # its variant that sends nothing
    "api_auth_check_or_query",  # + ?token= for <img> tags
    "CSI_AUTH_OR_RETURN",       # csi_integration's macro
    "authorized",               # csi_mqtt's cookie OR Bearer
    "auth_gated",               # the module trampolines (bt_/bcn_/plain)
    "pair_token_valid",         # the pair token the landing mints for any caller
    "pair_token_consume",
)

# The secrets, as canary_wap.ino reads them: a member of that name on any
# object, or the pin's global. An emptiness test of the pin is not a read.
SECRET_READS = {
    "token": re.compile(r"(?:\.|->)\s*api_token_str\b"),
    "ap": re.compile(r"(?:\.|->)\s*ap_password\b"),
    "pin": re.compile(r"\bg_tls_cert_fp_hex\b(?!\s*\[\s*0\s*\])"),
}
SECRET_NAMES = {"token": "the bearer token", "ap": "the SoftAP password", "pin": "the TLS pin"}
# The token a gate, or a module's init, is handed to compare against (what
# a module then does with its copy is not this rule's).
TOKEN_COMPARED = re.compile(
    r"\b(?:api_auth_check|api_auth_check_optional|api_auth_check_or_query|api_auth_check_bearer"
    r"|api_auth_bearer_valid|api_auth_bearer_grant|\w+::register_routes|csi_integration::init)"
    r"\s*\(\s*\w+\s*,"
    r"\s*g_device\s*\.\s*api_token_str\s*\)")
# Functions whose call hands their caller a secret, in an out-buffer.
SECRET_HANDS = {
    "derive_api_token": "token",
    "derive_ap_password": "ap",
    "resolve_ap_password": "ap",
}
# Functions that read a secret and use it on the device, neither answering
# it nor handing it on (so no writable char or byte buffer parameter and no
# char pointer returned): the carry up to the routes that call them stops
# here. Only the ones a route reaches need naming.
SECRET_LOCAL_USES = {
    "wifi_raise_ap": "re-raises the SoftAP with its own password, wiped after",
    "wifi_init_provisioning": "raises the first-boot SoftAP with its own password",
}
# The route handlers whose answer carries a secret: what it carries, and why.
SECRET_ANSWERS = {
    "handle_pairing_qr": (
        "token pin", "the pairing QR: the token, and the pin on a TLS device"),
    "handle_provisioning_receipt": (
        "token ap pin", "the receipt (the recovery kit), through send_provisioning_receipt()"),
}
# A refusal an answer may also stand after, on grants that are not
# credential gates: the receipt's BOOT tap (provisioning_gate_take()) and its
# setup-door claim, whose flag only the claim's take may set ("writes"; one
# ending in `(` is that call, then `;`). test_receipt_grant drives both.
OTHER_GRANTS = {
    "handle_provisioning_receipt": {
        "refusal": "!claim_served && !provisioning_gate_take()",
        "flag": "claim_served",
        "writes": ("bool claim_served = false;",
                   "claim_served = canary::net::claim_ticket::take("),
    },
}

# csi_mqtt.cpp's stored broker password: the functions that name the member
# (`.pass` / `->pass`, its emptiness test `pass[0]` aside). GET
# /api/mqtt/config, behind a session, answers only whether one is set.
BROKER_PASSWORD = re.compile(r"(?:\.|->)\s*pass\b(?!\s*\[\s*0\s*\])")
BROKER_PASSWORD_USES = {
    "config_load": "reads it from NVS into the caller's Config",
    "write_row": "writes it to NVS, inside its caller's SaveScope",
    "open_client": "hands it to the MQTT client, for the broker it was saved with",
    "handle_config_post": "takes a typed one, or keeps the stored one behind the move gate",
}
# The route that may send the kept broker password somewhere new (sweep
# F289, the third road, #1835): its handler, the writes a moved password must
# not reach before the move gate has refused, and the move gate, whose
# bearer argument is a `const bool` the handler computes once from a bearer
# gate (the bearer half of authorized()).
BROKER_REPOINT = {"handler": "handle_config_post",
                  "writes": ("write_row", "write_ca", "config_save", "ca_save"),
                  "gate": "move_needs_password"}

# The modules' own copies of the bearer token: what csi_integration::init()
# and each ns::register_routes(server, token) keep of the token canary_wap.ino
# hands them (the hand-over TOKEN_COMPARED excuses). Per file: the spelling
# that names the copy; the functions and macros that may name it ("keep":
# the getter, the setter, the gate that compares it); the declarations
# outside every function that may, whitespace folded ("decl"); and the route
# handlers that read it into a local and hand that local only to a gate, as
# `gate(req, tok)` ("read"). A module route that answered its copy names it
# somewhere else, and fails; a module the sketch hands the token to with no
# entry here fails too.
def _gated_module(gate: str) -> dict:
    return {"copy": r"\bauth_token_storage\s*\(", "keep": ("auth_token_storage", gate, "register_routes"),
            "decl": (), "read": ()}


MODULE_TOKEN_COPIES = {
    "csi_integration.cpp": {"copy": r"\bg_api_token\b", "keep": ("CSI_AUTH_OR_RETURN", "init"),
                            "decl": ("const char* g_api_token = nullptr;",), "read": ()},
    "csi_mqtt.cpp": {"copy": r"\bs_api_token_provider\b", "keep": ("set_api_token_provider",),
                     "decl": ("const char* (*s_api_token_provider)() = nullptr;",),
                     "read": ("handle_config_get", "handle_config_post", "handle_test")},
    "audible_chirp_api.h": _gated_module("auth_gated"),
    "beacon_api.h": _gated_module("bcn_auth_gated"),
    "bluetooth_api.h": _gated_module("bt_auth_gated"),
    "chirp_api.h": _gated_module("chirp_auth_gated"),
    "household_api.h": _gated_module("auth_gated"),
    "rf_presence_api.h": _gated_module("auth_gated"),
    "wifi_presence_api.h": _gated_module("auth_gated"),
}
_TOKEN_HANDED = re.compile(r"\b(\w+)::(?:register_routes|init)\s*\(\s*\w+\s*,"
                           r"\s*g_device\s*\.\s*api_token_str\s*\)")


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
        elif state == "string":
            out.append(c)
            if c == "\\":
                if nxt:
                    out.append(nxt)
                i += 2
            else:
                if c == '"':
                    state = "code"
                i += 1
        elif state == "char":
            out.append(c)
            if c == "\\":
                if nxt:
                    out.append(nxt)
                i += 2
            else:
                if c == "'":
                    state = "code"
                i += 1
    return "".join(out)


def blank_string_contents(text: str) -> str:
    """Replace the *contents* of string/char literals with spaces, keeping
    the quotes and the original length so byte offsets still align with
    `text`. Used only for brace matching: a `{` or `}` inside a JSON literal
    or log message (e.g. `"{\\"ok\\":true}"`, `"}"`) must not shift the
    depth counter. We can't just drop string bodies globally in
    strip_comments — the route parser needs `.uri = "/api/…"` intact — so
    this length-preserving blank is a matching-only view.

    Assumption: no C++ raw string literal (R"delim(...)delim") precedes an
    `esp_err_t` handler in the SAME file — this walker treats a raw block's
    embedded quotes as ordinary delimiters. The current tree satisfies this
    (the raw-string HTML blobs live in header files with no handlers, and
    csi_mqtt.cpp's block has even quote parity so state resyncs before its
    handlers); a verify pass confirmed every handler body extracts correctly.
    If a future edit adds a handler after a raw block in the same file, teach
    this walker the R"..." form.
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
        else:  # inside a string or char literal
            if c == "\\":
                out.append("  " if nxt else " ")  # blank the escape pair
                i += 2 if nxt else 1
                continue
            if (state == "string" and c == '"') or (state == "char" and c == "'"):
                out.append(c)  # keep the closing quote
                state = "code"
            else:
                out.append("\n" if c == "\n" else " ")  # blank the body
            i += 1
    return "".join(out)


def function_body(source: str, name: str):
    """Return the brace-matched body of `esp_err_t <name>(...)` if present.

    Brace matching runs over a string-blanked view so literal braces inside
    strings can't truncate or over-extend the body, but the returned slice
    comes from the real `source` (offsets align — blanking is length-
    preserving) so the auth-marker scan sees the true handler text.
    """
    blanked = _blank(source)  # cached: one blank per file, not per lookup
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


def _assert_parser_robust() -> None:
    """Guard the string-aware brace matcher on every run. A handler whose
    body carries an UNBALANCED brace inside a string literal before its auth
    call must still extract fully — the naive matcher truncated it there and
    would mis-report the route's gating (a silent false pass/negative)."""
    # An extra '}' inside a JSON string ("bad_}") would, under naive
    # counting, close the function early — before the api_auth_check line.
    fixture = (
        'esp_err_t handle_demo(httpd_req_t* req) {\n'
        '  httpd_resp_sendstr(req, "{\\"code\\":\\"bad_}\\"}");\n'
        '  if (!api_auth_check(req, tok)) return ESP_OK;\n'
        '  return http_send_json(req, "ok");\n'
        '}\n'
        'esp_err_t handle_other(httpd_req_t* req) { return ESP_OK; }\n'
    )
    body = function_body(fixture, "handle_demo")
    assert body is not None, "fixture handler not found"
    assert "api_auth_check(" in body, (
        "brace matcher truncated the body at a string-literal brace — the "
        "auth call was lost (this is the bug this guard prevents)")
    assert "handle_other" not in body, "body over-extended into the next fn"
    # A char literal brace must not desync either. Use '}' so this genuinely
    # discriminates: under the naive matcher the literal '}' decrements depth
    # to 0 and closes the body BEFORE the api_auth_check line (fails); the
    # blanked matcher ignores it (passes).
    fixture2 = (
        "esp_err_t handle_c(httpd_req_t* req) {\n"
        "  char close = '}';\n"
        "  if (!api_auth_check(req, tok)) return ESP_OK;\n"
        "  return ESP_OK;\n"
        "}\n"
    )
    b2 = function_body(fixture2, "handle_c")
    assert b2 is not None and "api_auth_check(" in b2, "char-literal brace desync"


# ── The secret rule (sweep F289) ──────────────────────────────────────────

# A function head: a return type, the name, parameters with balanced
# parentheses (one level, for a function-pointer parameter), then `{`. The
# balance keeps the last line of a multi-line call (`  s, sizeof(s)) {`)
# from reading as a head.
_HEAD = (r"(?!(?:if|for|while|switch|return|else|do|case|catch)\b)[A-Za-z_][\w:<>\*&\s,]*?"
         r"\b([A-Za-z_]\w*)\s*\(([^;{}()]*(?:\([^;{}()]*\)[^;{}()]*)*)\)\s*(?:const\s*)?\{")
_FUNC_HEAD = re.compile(r"^" + _HEAD, re.M)
_ANY_FUNC_HEAD = re.compile(r"^[ \t]*" + _HEAD, re.M)
_DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)((?:[^\n]*\\\n)*[^\n]*)", re.M)
_CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
# A parameter a secret could be handed out through: a writable char or byte
# buffer (char* out, uint8_t buf[], char& c).
_BUFFER_PARAM = re.compile(r"\b(?:char|uint8_t|unsigned\s+char|int8_t)\b[^,]*[\*\[&]")
# The declarations outside every function of canary_wap.ino that name a
# secret, whitespace folded. Anything else there fails.
OUTSIDE_FUNCTION_LINES = (
    "static char g_tls_cert_fp_hex[65] = {0};",
    "static bool resolve_ap_password(char* out_password, size_t out_len);",
)


@functools.lru_cache(maxsize=None)
def _blank(text: str) -> str:
    return blank_string_contents(text)


@functools.lru_cache(maxsize=None)
def functions(text: str, indented: bool = False) -> tuple:
    """(name, params, head start, body start, body end) for every function
    defined in `text` (comment-stripped): heads at column 0, or at any
    indentation with `indented` (namespaced and member code). Braces are
    matched over the string-blanked view; the body runs from just past the
    head's `{` to its closing `}`."""
    blanked = _blank(text)
    out = []
    for m in (_ANY_FUNC_HEAD if indented else _FUNC_HEAD).finditer(blanked):
        depth, i = 1, m.end()
        while i < len(blanked) and depth:
            if blanked[i] == "{":
                depth += 1
            elif blanked[i] == "}":
                depth -= 1
            i += 1
        out.append((m.group(1), m.group(2), m.start(), m.end(), i - 1))
    return tuple(out)


def _names(code: str) -> set:
    """Every identifier `code` (string-blanked) names, called or not, other
    than a member (`x.name`, `x->name`): an object's field or method is not
    the free function of that name."""
    return set(re.findall(r"(?<![\w.])[A-Za-z_]\w*", re.sub(r"(?:\.|->)\s*", ".", code)))


def blank_raw_strings(text: str) -> str:
    """The contents of every raw string literal (R"delim( ... )delim") as
    spaces, newlines kept, so its HTML and JavaScript read as no code. Run
    before strip_comments(): it does not know the raw form, and a `//` in an
    https:// URL would read as a comment opener."""
    def blank(m):
        inner = re.sub(r"[^\n]", " ", m.group(2))
        return m.group(0)[:m.start(2) - m.start()] + inner + m.group(0)[m.end(2) - m.start():]
    return re.sub(r'R"([^()\\\s"]{0,16})\((.*?)\)\1"', blank, text, flags=re.S)


def _line(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def _inside(pos: int, spans) -> bool:
    return any(a <= pos < b for a, b in spans)


def _close(code: str, i: int) -> int:
    """The index of the bracket closing the one at code[i], or -1."""
    pair = {"(": ")", "{": "}"}[code[i]]
    depth = 0
    for j in range(i, len(code)):
        if code[j] == code[i]:
            depth += 1
        elif code[j] == pair:
            depth -= 1
            if depth == 0:
                return j
    return -1


def _statement_end(code: str, k: int) -> int:
    """The `;` ending the statement that starts at `k`, outside brackets."""
    depth = 0
    for j in range(k, len(code)):
        if code[j] in "([{":
            depth += 1
        elif code[j] in ")]}":
            depth -= 1
            if depth < 0:
                return -1
        elif code[j] == ";" and depth == 0:
            return j
    return -1


def _branch(code: str, k: int):
    """The branch of an `if` whose condition closed just before `k`: (start,
    end, whether it returns). A branch returns when it is a `return`
    statement, or a block whose last statement is one."""
    while k < len(code) and code[k].isspace():
        k += 1
    if k < len(code) and code[k] == "{":
        end = _close(code, k)
        if end < 0:
            return k, len(code), False
        inner = code[k + 1:end].rstrip()
        depth, j = 0, len(inner) - 2
        while j >= 0:  # back to the start of the last statement
            if inner[j] in ")]":
                depth += 1
            elif inner[j] in "([":
                depth -= 1
            elif inner[j] in ";{}" and depth == 0:
                break
            j -= 1
        last = inner[j + 1:]
        return k, end + 1, inner.endswith(";") and re.match(r"\s*return\b", last) is not None
    end = _statement_end(code, k)
    if end < 0:
        return k, len(code), False
    return k, end + 1, re.match(r"return\b", code[k:end]) is not None


def _ifs(code: str) -> list:
    """(position, condition, condition span, branch span, returns) for every
    `if` in `code` (string-blanked)."""
    out = []
    for m in re.finditer(r"\bif\s*\(", code):
        close = _close(code, m.end() - 1)
        if close < 0:
            continue
        start, end, returns = _branch(code, close + 1)
        out.append((m.start(), code[m.end():close], (m.end(), close), (start, end), returns))
    return out


def _depth(code: str, pos: int) -> int:
    return code.count("{", 0, pos) - code.count("}", 0, pos)


def _squash(text: str) -> str:
    return re.sub(r"\s+", "", text)


def _terms(cond: str, op: str) -> list:
    """`cond` split on `op` (`&&`, `||`) outside parentheses."""
    out, depth, start, i = [], 0, 0, 0
    while i < len(cond):
        if cond[i] == "(":
            depth += 1
        elif cond[i] == ")":
            depth -= 1
        elif depth == 0 and cond.startswith(op, i):
            out.append(cond[start:i])
            start = i + len(op)
            i += len(op)
            continue
        i += 1
    return out + [cond[start:]]


def _secret_list(secrets) -> str:
    return ", ".join(SECRET_NAMES[s] for s in sorted(secrets))


def _called(code: str) -> set:
    """Every free function `code` (string-blanked) calls: `name(` or
    `name<...>(`, a member's call aside."""
    code = re.sub(r"(?:\.|->)\s*", ".", code)
    found = {m.group(1) for m in re.finditer(r"(?<![\w.])([A-Za-z_]\w*)\s*\(", code)}
    found |= {m.group(1) for m in re.finditer(r"(?<![\w.])([A-Za-z_]\w*)\s*<[^;(){}<>]*>\s*\(",
                                              code)}
    return found


def any_session_gates(sources: dict) -> set:
    """ANY_SESSION_GATES and, to a fixed point, every function (at any
    indentation) in `sources` whose body calls one and every macro whose body
    names one. A bearer gate is never added: bearer_gate_problems() holds
    that it names none. A function that only names a handler (a route
    table's initializer) is not a wrapper of it."""
    reaches = {}
    for text in sources.values():
        blanked = _blank(text)
        for name, _p, _h, b, e in functions(text, indented=True):
            reaches.setdefault(name, set()).update(_called(blanked[b:e]))
        for m in _DEFINE.finditer(blanked):
            reaches.setdefault(m.group(1), set()).update(_names(m.group(2)))
    members = set(ANY_SESSION_GATES)
    while True:
        grown = {n for n, ids in reaches.items()
                 if n not in members and n not in BEARER_GATES and ids & members}
        if not grown:
            return members
        members |= grown


def bearer_gate_problems(sources: dict, any_session: set) -> list:
    """Each bearer gate is defined once, in its BEARER_GATES home, and names
    no gate in the any-session class."""
    problems = []
    for gate, home in sorted(BEARER_GATES.items()):
        defs = [(fname, b, e) for fname, text in sorted(sources.items())
                for name, _p, _h, b, e in functions(text, indented=True) if name == gate]
        files = [fname for fname, _b, _e in defs]
        if files != [home]:
            where = ", ".join(files) or "no file the check reads"
            problems.append(f"{gate}() is defined in {where}; a bearer gate is defined once, "
                            f"in {home}")
        for fname, b, e in defs:
            body = _blank(sources[fname])[b:e]
            took = sorted(_names(body) & any_session)
            if took:
                problems.append(f"{fname}: {gate}() names {', '.join(took)}, a gate a landing-page "
                                f"caller passes: a bearer gate takes the Authorization header only")
    return problems


def secret_carriers(ino: str, hands: dict, local: dict):
    """(reads, carried, calls) for canary_wap.ino's column-0 functions: the
    secrets each body reads (SECRET_READS, the compared token aside; a call
    of a `hands` function), the secrets it carries (those, and what each
    function it calls carries; a `local` function's carry stops at it), and
    the names it calls."""
    blanked = _blank(ino)
    reads, calls = {}, {}
    for name, _p, _h, b, e in functions(ino):
        body = blanked[b:e]
        called = _called(body)
        got = {s for s, rx in SECRET_READS.items() if rx.search(TOKEN_COMPARED.sub("", body))}
        got |= {hands[h] for h in called & set(hands) if h != name}
        reads.setdefault(name, set()).update(got)
        calls.setdefault(name, set()).update(called)
    carried = {n: set(s) for n, s in reads.items()}
    changed = True
    while changed:
        changed = False
        for name, called in calls.items():
            for g in called - {name}:
                if g in carried and g not in local and not carried[g] <= carried[name]:
                    carried[name] |= carried[g]
                    changed = True
    return reads, carried, calls


def _answer_sites(code: str, carriers: set, hands: dict, self_name: str) -> list:
    """(position, what) for each place `code` (a body, string-blanked)
    reaches a secret: a read, or a call of a function that carries one."""
    compared = [(m.start(), m.end()) for m in TOKEN_COMPARED.finditer(code)]
    sites = [(m.start(), SECRET_NAMES[s]) for s, rx in SECRET_READS.items()
             for m in rx.finditer(code) if not _inside(m.start(), compared)]
    free_call = re.compile(r"(?<![\w.])(?<!->)([A-Za-z_]\w*)\s*\(")  # a member's call aside
    sites += [(m.start(), m.group(1) + "()") for m in free_call.finditer(code)
              if m.group(1) != self_name and (m.group(1) in carriers or m.group(1) in hands)]
    return sorted(sites)


def _flag_problems(handler: str, code: str, entry: dict, at) -> list:
    """The grant flag is read only as a condition and written only by the
    entry's "writes"."""
    flag, problems = entry["flag"], []
    for m in re.finditer(r"\b" + re.escape(flag) + r"\b", code):
        before, after = code[:m.start()].rstrip(), code[m.end():].lstrip()
        if re.match(r"(?:&&|\|\||\?|\)|==|!=)", after) and not re.search(r"(?<!&)&$", before):
            continue  # !flag &&, if (flag), flag ? ...: a read
        start = max(code.rfind(c, 0, m.start()) for c in ";{}") + 1
        end = _statement_end(code, start)
        stmt = " ".join(code[start:end + 1].split()) if end >= 0 else ""
        ok = False
        for allowed in entry["writes"]:
            if not allowed.endswith("("):
                ok = ok or stmt == allowed
            elif stmt.startswith(allowed):
                call_close = _close(stmt, len(allowed) - 1)
                ok = ok or (call_close >= 0 and stmt[call_close + 1:] == ";")
        if not ok:
            writes = " / ".join(repr(w) for w in entry["writes"])
            problems.append(f"canary_wap.ino:{at(m.start())}: {handler}() names {flag} other than "
                            f"as a condition or in {writes}: only the grant may set the flag its "
                            f"refusal trusts")
    return problems


def gated_answer_problems(sources: dict, handlers: set, any_session: set,
                           answers: dict, local: dict, hands: dict, grants: dict) -> list:
    """The routes whose answer carries a secret, over canary_wap.ino."""
    ino = sources.get("canary_wap.ino", "")
    blanked = _blank(ino)
    funcs = functions(ino)
    bodies = {name: (b, e) for name, _p, _h, b, e in funcs}
    reads, carried, calls = secret_carriers(ino, hands, local)
    carriers = {n for n, s in carried.items() if s and n not in local}
    problems = []

    # Nothing outside a function names a secret but its declaration.
    spans = [(h, e) for _n, _p, h, _b, e in funcs]
    outside = [m.start() for rx in SECRET_READS.values() for m in rx.finditer(blanked)]
    outside += [m.start() for m in _CALL.finditer(blanked) if m.group(1) in hands]
    for pos in sorted(p for p in outside if not _inside(p, spans)):
        line = " ".join(ino.splitlines()[_line(blanked, pos) - 1].split())
        if line not in OUTSIDE_FUNCTION_LINES:
            problems.append(f"canary_wap.ino:{_line(blanked, pos)}: a secret named outside every "
                            f"function ({line[:80]!r}): the check reads secrets per function")

    for name in sorted(hands):
        if name not in bodies:
            problems.append(f"SECRET_HANDS names {name}(), which canary_wap.ino does not define")
    for name, why in sorted(local.items()):
        if name not in bodies or not reads.get(name):
            problems.append(f"SECRET_LOCAL_USES names {name}(), which reads no secret: remove it")
            continue
        head_start, params = next((h, p) for n, p, h, _b, _e in funcs if n == name)
        writable = [p.strip() for p in params.split(",")
                    if _BUFFER_PARAM.search(p) and not p.strip().startswith("const")]
        if writable or re.match(r"[^(]*\bchar\s*\*", blanked[head_start:bodies[name][0]]):
            problems.append(f"{name}() reads {_secret_list(reads[name])} and could hand it to its "
                            f"caller ({', '.join(writable) or 'it returns a char pointer'}): it is "
                            f"not a SECRET_LOCAL_USES entry ({why})")
        if name in handlers:
            problems.append(f"{name}() is a route handler, so it answers: not a local use")

    # What each route carries, wherever its handler is defined: a handler
    # outside canary_wap.ino reaches the secrets by calling a carrier there.
    elsewhere = {}
    for fname, text in sources.items():
        if fname != "canary_wap.ino":
            for name, _p, _h, b, e in functions(text, indented=True):
                elsewhere.setdefault(name, set()).update(_called(_blank(text)[b:e]))
    carrying = {}
    for h in sorted(handlers):
        got = set(carried.get(h, ())) if h not in local else set()
        called = elsewhere.get(h, set())
        got |= {s for g in called & carriers for s in carried[g]}
        got |= {hands[g] for g in called & set(hands)}
        if got:
            carrying[h] = got
    for h, got in sorted(carrying.items()):
        if h not in answers:
            problems.append(f"route handler {h}() answers {_secret_list(got)} and is not in "
                            f"SECRET_ANSWERS: name it there, behind a bearer-only gate")
    for h, (named, _why) in sorted(answers.items()):
        if h not in handlers:
            problems.append(f"SECRET_ANSWERS names {h}(), which is no registered route handler")
            continue
        if set(named.split()) != carrying.get(h, set()):
            has = _secret_list(carrying.get(h, ())) or "no secret"
            problems.append(f"{h}() carries {has}; its SECRET_ANSWERS entry says "
                            f"{_secret_list(named.split())}")
        if h not in bodies:
            problems.append(f"{h}() is not defined at column 0 in canary_wap.ino, where the check "
                            f"reads an answer's gate")
            continue
        problems += _answer_gate_problems(h, blanked, bodies, calls, carriers, any_session,
                                          hands, grants)
    return problems


def _answer_gate_problems(h, blanked, bodies, calls, carriers, any_session, hands, grants):
    """One answer's gate: a bearer refusal or grant as the whole condition of
    a returning `if`, each answer site after a refusal or inside a grant, and
    no gate a session passes in the handler or on its way to the secret."""
    b, e = bodies[h]
    code = blanked[b:e]

    def at(pos):
        return _line(blanked, b + pos)

    problems = []
    path, queue = {h}, [h]  # the handler and the functions it reaches the secret through
    while queue:
        for g in sorted(calls.get(queue.pop(), set()) & carriers - path):
            path.add(g)
            queue.append(g)
    for fn in sorted(path):
        fb, fe = bodies[fn]
        who = f"{h}() answers a secret and" if fn == h else f"{fn}(), on the way to {h}()'s secret,"
        took = sorted(_names(blanked[fb:fe]) & any_session - path)
        if took:
            problems.append(f"{who} names {', '.join(took)}, a gate a landing-page caller "
                            f"passes: a secret's answer takes the bearer header (BEARER_GATES), "
                            f"never a session")
        if re.search(r"\bgoto\b", blanked[fb:fe]):
            problems.append(f"{who} holds a goto, which can jump past its gate")
        if re.search(r"^[ \t]*#", blanked[fb:fe], re.M):
            problems.append(f"{who} holds a preprocessor line: the check reads one text, not "
                            f"each build's")

    gate = re.compile(r"\s*(!\s*)?(" + "|".join(map(re.escape, BEARER_GATES)) +
                      r")\s*\(\s*req\s*,\s*g_device\s*\.\s*api_token_str\s*\)\s*")
    other = grants.get(h)
    refusals, granted, gate_conds = [], [], []
    for pos, cond, cspan, bspan, returns in _ifs(code):
        g = gate.fullmatch(cond)
        if g:
            gate_conds.append(cspan)
        elif not (other and _squash(cond) == _squash(other["refusal"])):
            continue
        if not returns:
            continue
        if g and not g.group(1):
            granted.append(bspan)
        elif _depth(code, pos) == 0:
            refusals.append(bspan[1])
    for m in re.finditer(r"\b(" + "|".join(map(re.escape, BEARER_GATES)) + r")\s*\(", code):
        if not _inside(m.start(), gate_conds):
            problems.append(f"canary_wap.ino:{at(m.start())}: {h}() calls {m.group(1)}() other "
                            f"than as the whole condition of an if, `if (!{m.group(1)}(req, "
                            f"g_device.api_token_str)) return ...;` or `if ({m.group(1)}(...)) "
                            f"{{ ... return ...; }}`")
    if not gate_conds:
        problems.append(f"{h}() answers a secret and has no bearer gate (BEARER_GATES) as the "
                        f"whole condition of an if")
    if other:
        problems += _flag_problems(h, code, other, at)
    first = min(refusals, default=None)
    for pos, what in _answer_sites(code, carriers, hands, h):
        if (first is None or pos < first) and not _inside(pos, granted):
            problems.append(f"canary_wap.ino:{at(pos)}: {h}() reaches {what} before any refusal "
                            f"and outside every grant: a secret's answer stands after `if "
                            f"(!<bearer gate>(...)) return` or inside `if (<bearer gate>(...)) "
                            f"{{ ... return; }}`")
    return problems


def broker_gate_problems(sources: dict, handlers: set, any_session: set,
                             uses: dict) -> list:
    """csi_mqtt.cpp's stored broker password: named only by `uses`, and the
    save that may move it behind the move gate, handed the bearer."""
    text = sources.get("csi_mqtt.cpp")
    if text is None:
        return ["csi_mqtt.cpp not found: the broker-password rule reads it"]
    blanked = _blank(text)
    funcs = functions(text)
    problems, named = [], set()
    for name, _p, _h, b, e in funcs:
        if BROKER_PASSWORD.search(blanked[b:e]):
            named.add(name)
    spans = [(h, e) for _n, _p, h, _b, e in funcs]
    for m in BROKER_PASSWORD.finditer(blanked):
        if not _inside(m.start(), spans):
            problems.append(f"csi_mqtt.cpp:{_line(blanked, m.start())}: the broker password named "
                            f"outside every function the check reads")
    for name in sorted(named - set(uses)):
        problems.append(f"csi_mqtt.cpp: {name}() names the stored broker password and is not in "
                        f"BROKER_PASSWORD_USES (a GET answers only whether one is set)")
    for name in sorted(set(uses) - named):
        problems.append(f"BROKER_PASSWORD_USES names {name}(), which names no broker password")
    for name in sorted(named & handlers - {BROKER_REPOINT["handler"]}):
        problems.append(f"csi_mqtt.cpp: route handler {name}() names the stored broker password")

    h, gate_fn = BROKER_REPOINT["handler"], BROKER_REPOINT["gate"]
    found = [(b, e) for name, _p, _h, b, e in funcs if name == h]
    if h not in handlers or not found:
        return problems + [f"csi_mqtt.cpp: {h}() is not a registered route handler defined at "
                           f"column 0; BROKER_REPOINT reads its move gate"]
    b, e = found[0]
    code = blanked[b:e]

    def at(pos):
        return _line(blanked, b + pos)

    writes = "|".join(map(re.escape, BROKER_REPOINT["writes"]))
    first = re.search(r"(?<![\w.])(" + writes + r")\s*\(", code)
    if first is None:
        return problems + [f"csi_mqtt.cpp: {h}() calls none of "
                           f"{', '.join(w + '()' for w in BROKER_REPOINT['writes'])}; "
                           f"BROKER_REPOINT names the writes a moved password must not reach"]
    # The move gate: defined once at column 0, deciding on what it is handed.
    defs = [(gb, ge) for name, _p, _h, gb, ge in funcs if name == gate_fn]
    if len(defs) != 1:
        problems.append(f"csi_mqtt.cpp: {gate_fn}() is defined {len(defs)} times at column 0; "
                        f"BROKER_REPOINT reads one move gate")
    for gb, ge in defs:
        took = sorted(_names(blanked[gb:ge]) & any_session)
        if took:
            problems.append(f"csi_mqtt.cpp: {gate_fn}() names {', '.join(took)}, a gate a "
                            f"landing-page caller passes: the move gate decides on the bearer "
                            f"it is handed")
    whole = re.compile(r"\s*" + re.escape(gate_fn) + r"\s*\(([^()]*)\)\s*")
    gate_bearer = re.compile(r"\s*(?:" + "|".join(map(re.escape, BEARER_GATES)) +
                             r")\s*\([^()]*\)\s*")
    gated = False
    for pos, cond, _c, _b, returns in _ifs(code):
        if not re.search(r"(?<![\w.])" + re.escape(gate_fn) + r"\s*\(", cond):
            continue
        took = sorted(_names(cond) & any_session)
        call = whole.fullmatch(cond)
        if took:
            problems.append(f"csi_mqtt.cpp:{at(pos)}: {h}()'s move gate names "
                            f"{', '.join(took)}, a gate a landing-page caller passes")
            continue
        if call is None:
            problems.append(f"csi_mqtt.cpp:{at(pos)}: {h}()'s move gate is not the whole "
                            f"condition of its if: `if ({gate_fn}(..., bearer)) {{ ... return "
                            f"...; }}`")
            continue
        if pos > first.start() or _depth(code, pos) != 0 or not returns:
            continue  # no gate before the write: said below
        # The bearer it is handed: a `const bool` declared once, at the top
        # of the handler before the gate, one `||` term whose `&&` terms
        # include a bearer gate's call, naming no gate a session passes.
        bearers = []
        for arg in (a.strip() for a in _terms(call.group(1), ",")):
            if not re.fullmatch(r"[A-Za-z_]\w*", arg):
                continue
            decls = list(re.finditer(r"\bconst\s+bool\s+" + arg + r"\s*=([^;]*);", code))
            if len(decls) != 1 or decls[0].start() > pos or _depth(code, decls[0].start()) != 0:
                continue
            init = decls[0].group(1)
            took = sorted(_names(init) & any_session)
            if took:
                problems.append(f"csi_mqtt.cpp:{at(decls[0].start())}: {h}() hands {gate_fn}() a "
                                f"bearer that names {', '.join(took)}, a gate a landing-page "
                                f"caller passes")
                bearers.append(None)
                continue
            terms = _terms(init, "&&") if len(_terms(init, "||")) == 1 else []
            if any(gate_bearer.fullmatch(t) for t in terms):
                bearers.append(arg)
        if not bearers:
            problems.append(f"csi_mqtt.cpp:{at(pos)}: {h}() hands {gate_fn}() no bearer: one "
                            f"argument is a `const bool` declared at the top of the handler "
                            f"before the gate, `... && <bearer gate>(req, tok)` (BEARER_GATES)")
        elif None not in bearers:
            gated = True
    if not gated:
        problems.append(f"csi_mqtt.cpp: {h}() reaches {first.group(1)}() with no move gate before "
                        f"it: a top-level `if ({gate_fn}(..., bearer)) {{ ... return ...; }}`, "
                        f"the bearer from a bearer gate (BEARER_GATES), so a save that moves the "
                        f"kept password takes the bearer header or the password typed")
    return problems


def module_token_problems(sources: dict, any_session: set, copies: dict) -> list:
    """Each module's copy of the token is named only where `copies` says:
    kept by its getter, setter and gate, declared once, or read into a local
    that only a gate is handed (`gate(req, tok)`, or `gate(req, tok, &held)`
    with the grant's held out-parameter). Every module the sketch hands the
    token to has an entry."""
    problems = []
    for ns in sorted(set(_TOKEN_HANDED.findall(_blank(sources.get("canary_wap.ino", ""))))):
        homes = [f for f, t in sources.items() if re.search(r"\bnamespace\s+" + ns + r"\s*\{", t)]
        if not any(f in copies for f in homes):
            problems.append(f"canary_wap.ino hands the token to {ns}::, and MODULE_TOKEN_COPIES "
                            f"names no copy of it in {', '.join(homes) or 'any file the check reads'}")
    gates = "|".join(map(re.escape, sorted(set(BEARER_GATES) | any_session)))
    for fname, entry in sorted(copies.items()):
        text = sources.get(fname)
        if text is None:
            problems.append(f"MODULE_TOKEN_COPIES names {fname}, which the check does not read")
            continue
        blanked = _blank(text)
        funcs = functions(text, indented=True)
        macros = [(m.group(1), m.start(), m.end()) for m in _DEFINE.finditer(blanked)]
        decls = {_squash(d) for d in entry["decl"]}
        named, readers = set(), {}
        for m in re.finditer(entry["copy"], blanked):
            inner = sorted((e - h, n, b, e) for n, _p, h, b, e in funcs if h <= m.start() < e)
            macro = [n for n, a, z in macros if a <= m.start() < z]
            stmt_a = max(blanked.rfind(c, 0, m.start()) for c in ";{}\n") + 1
            stmt = blanked[stmt_a:blanked.find(";", m.start()) + 1]
            where = f"{fname}:{_line(blanked, m.start())}"
            if macro:
                named.add(macro[0])
            elif inner:
                name, b, e = inner[0][1], inner[0][2], inner[0][3]
                named.add(name)
                if name in entry["read"]:
                    local = re.fullmatch(r"\s*const\s+char\s*\*\s*(\w+)\s*=[^;]*;", stmt)
                    if local is None:
                        problems.append(f"{where}: {name}() names its module's copy of the token "
                                        f"outside a `const char* tok = ...;` it hands only to a gate")
                    else:
                        readers.setdefault((name, local.group(1)), (b, e, m.start()))
            elif _squash(stmt) not in decls:
                problems.append(f"{where}: the module's copy of the token named outside every "
                                f"function and macro the check reads, and not a declaration "
                                f"MODULE_TOKEN_COPIES lists")
        for (name, tok), (b, e, at) in sorted(readers.items()):
            body = blanked[b:e]
            decl_end = blanked.find(";", at) - b
            for u in re.finditer(r"\b" + re.escape(tok) + r"\b", body):
                if u.start() < decl_end and blanked.rfind("\n", 0, b + u.start()) == blanked.rfind(
                        "\n", 0, at):
                    continue  # the declaration itself
                if re.match(r"\s*[!=]=\s*nullptr\b", body[u.end():]):
                    continue  # a null test (#1835's bearer: tok != nullptr && ...) hands nothing on
                head = body[:u.start()]
                call = re.search(r"\b(?:" + gates + r")\s*\(\s*\w+\s*,\s*$", head)
                # The token argument, then `)` or the grant's held
                # out-parameter, `, &<identifier>)` (sweep F339).
                if call is None or not re.match(r"\s*(?:,\s*&\s*[A-Za-z_]\w*\s*)?\)", body[u.end():]):
                    problems.append(f"{fname}:{_line(blanked, b + u.start())}: {name}() uses `{tok}`, "
                                    f"its copy of the token, other than as a gate's token argument "
                                    f"or in a null test")
        for name in sorted(named - set(entry["keep"]) - set(entry["read"])):
            problems.append(f"{fname}: {name}() names its module's copy of the token and is not in "
                            f"MODULE_TOKEN_COPIES (a route that names it can answer it)")
        for name in sorted((set(entry["keep"]) | set(entry["read"])) - named):
            problems.append(f"MODULE_TOKEN_COPIES lets {fname}'s {name}() name the token's copy, "
                            f"and it names none")
    return problems


def raw_compare_problems(sources: dict) -> list:
    """Every bearer compare is accounted (sweep F339): outside
    RAW_COMPARE_HOME, no sketch file names a RAW_COMPARES function followed by
    `(`, in its code (comments and string contents blanked). No allowlist:
    each hit is a problem, named by file and line."""
    problems = []
    call = re.compile(r"\b(" + "|".join(map(re.escape, RAW_COMPARES)) + r")\s*\(")
    for fname, text in sorted(sources.items()):
        if fname == RAW_COMPARE_HOME:
            continue
        blanked = _blank(text)
        for m in call.finditer(blanked):
            problems.append(f"{fname}:{_line(blanked, m.start())}: calls {m.group(1)}(), the raw "
                            f"compare, which records no strike: outside {RAW_COMPARE_HOME} a bearer "
                            f"is compared through api_auth_bearer_grant(), api_auth_check_bearer() "
                            f"or a gate built on them, under its address's lockout")
    return problems


# The three rule functions are named for the gates they hold, not for what
# the gates protect: CodeQL's py/clear-text-logging-sensitive-data reads a
# call to a function whose name says "secret" or "password" as a secret
# value, and main() prints the problems these return. This script reads
# C++ source text and never holds a credential, so that would be a false
# alert on every run (#1832's CodeQL check). Keep the words out of these
# three names.
def answer_gate_rule_problems(sources: dict, handlers: set, answers=None, local=None, hands=None,
                         grants=None, broker_uses=None, token_copies=None) -> list:
    """The whole secret rule over `sources` (file name -> comment-stripped
    text) and the registered handler names; the tables default to the ones
    above (the fixtures pass their own)."""
    any_session = any_session_gates(sources)
    return (bearer_gate_problems(sources, any_session)
            + gated_answer_problems(
                sources, handlers, any_session,
                SECRET_ANSWERS if answers is None else answers,
                SECRET_LOCAL_USES if local is None else local,
                SECRET_HANDS if hands is None else hands,
                OTHER_GRANTS if grants is None else grants)
            + broker_gate_problems(
                sources, handlers, any_session,
                BROKER_PASSWORD_USES if broker_uses is None else broker_uses)
            + module_token_problems(
                sources, any_session,
                MODULE_TOKEN_COPIES if token_copies is None else token_copies))


def handler_names(routes) -> set:
    """The function each registration names: `ns::fn` by fn, a trampoline
    `auth_gated<fn>` by fn."""
    out = set()
    for _uri, _method, handler, _fname in routes:
        inner = re.search(r"<\s*([\w:]+)\s*>", handler)
        out.add((inner.group(1) if inner else handler).split("::")[-1])
    return out


# The secret rule's fixtures: a small sketch that passes, and plants that
# each fail it for the reason named.
_FIX_API_AUTH = (
    "extern bool cv_session_validate(httpd_req_t* req) __attribute__((weak));\n"
    "static bool api_auth_check_bearer(httpd_req_t* req, const char* t) {\n"
    "  return t && header_matches(req, t);\n"
    "}\n"
    "static bool api_auth_bearer_valid(httpd_req_t* req, const char* t) {\n"
    "  return t && header_matches(req, t);\n"
    "}\n"
    "static bool api_auth_bearer_grant(httpd_req_t* req, const char* t, bool* held = nullptr) {\n"
    "  if (auth_is_locked_out()) return false;\n"
    "  return api_auth_bearer_valid(req, t);\n"
    "}\n"
    "static bool api_auth_check(httpd_req_t* req, const char* t) {\n"
    "  if (cv_session_validate && cv_session_validate(req)) return true;\n"
    "  return api_auth_check_bearer(req, t);\n"
    "}\n"
)
_FIX_INO = (
    "static DeviceIdentity g_device;\n"
    "static void send_kit(httpd_req_t* req) {\n"
    "  httpd_resp_sendstr(req, g_device.ap_password);\n"
    "}\n"
    "static esp_err_t handle_qr(httpd_req_t* req) {\n"
    "  if (!api_auth_check_bearer(req, g_device.api_token_str)) return ESP_OK;\n"
    "  return httpd_resp_sendstr(req, g_device.api_token_str);\n"
    "}\n"
    "static esp_err_t handle_kit(httpd_req_t* req) {\n"
    "  if (api_auth_bearer_grant(req, g_device.api_token_str)) {\n"
    "    send_kit(req);\n"
    "    return ESP_OK;\n"
    "  }\n"
    "  bool claimed = false;\n"
    "  claimed = claim_take(presented);\n"
    "  if (!claimed && !gate_take()) {\n"
    "    return httpd_resp_sendstr(req, \"{\\\"error\\\":\\\"tap\\\"}\");\n"
    "  }\n"
    "  send_kit(req);\n"
    "  return ESP_OK;\n"
    "}\n"
    "static esp_err_t handle_status(httpd_req_t* req) {\n"
    "  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;\n"
    "  return httpd_resp_sendstr(req, \"{}\");\n"
    "}\n"
    "static void start_server(httpd_handle_t server) {\n"
    "  csi_integration::init(server, g_device.api_token_str);\n"
    "}\n"
)
_FIX_CSI = (
    "namespace csi_integration {\n"
    "const char*   g_api_token  = nullptr;\n"
    "#define CSI_AUTH_OR_RETURN(req) \\\n"
    "  do { if (!api_auth_check_optional((req), g_api_token)) return ESP_OK; } while (0)\n"
    "bool init(httpd_handle_t server, const char* api_token) {\n"
    "  g_api_token = api_token;\n"
    "  return true;\n"
    "}\n"
    "esp_err_t handle_csi_status(httpd_req_t* req) {\n"
    "  CSI_AUTH_OR_RETURN(req);\n"
    "  return httpd_resp_sendstr(req, \"{}\");\n"
    "}\n"
    "}\n"
)
_FIX_MQTT = (
    "const char* (*s_api_token_provider)() = nullptr;\n"
    "void set_api_token_provider(const char* (*fn)()) { s_api_token_provider = fn; }\n"
    "static bool authorized(httpd_req_t* req, const char* t, bool* held = nullptr) {\n"
    "  return csi_integration::session_validate_cookie(req) || api_auth_check_optional(req, t, held);\n"
    "}\n"
    "bool write_row(const Config& cfg) { return prefs.putString(\"p\", cfg.pass) > 0; }\n"
    "static bool move_needs_password(const Config& before, const Config& after, bool typed,\n"
    "                                bool bearer) {\n"
    "  if (bearer) return false;\n"
    "  if (typed) return false;\n"
    "  return before.pass[0] != '\\0' && strcmp(before.host, after.host) != 0;\n"
    "}\n"
    "esp_err_t handle_config_get(httpd_req_t* req) {\n"
    "  const char* tok = s_api_token_provider ? s_api_token_provider() : nullptr;\n"
    "  bool held = false;\n"
    "  if (!authorized(req, tok, &held)) return ESP_OK;\n"
    "  Config c;\n"
    "  return httpd_resp_sendstr(req, c.pass[0] ? \"set\" : \"none\");\n"
    "}\n"
    "esp_err_t handle_config_post(httpd_req_t* req) {\n"
    "  const char* tok = s_api_token_provider ? s_api_token_provider() : nullptr;\n"
    "  bool held = false;\n"
    "  const bool bearer = tok != nullptr && api_auth_bearer_grant(req, tok, &held);\n"
    "  if (!bearer && !csi_integration::session_validate_cookie(req)) return ESP_OK;\n"
    "  Config c;\n"
    "  const Config before = c;\n"
    "  if (typed) strncpy(c.pass, buf, 128);\n"
    "  if (move_needs_password(before, c, typed, bearer)) {\n"
    "    httpd_resp_set_status(req, \"403 Forbidden\");\n"
    "    return ESP_OK;\n"
    "  }\n"
    "  if (!write_row(c)) return ESP_OK;\n"
    "  return ESP_OK;\n"
    "}\n"
)
# A module route that compares the bearer for a field of its answer, as F346's
# GET /api/bluetooth passkey does: through the accounted grant.
_FIX_BT = (
    "namespace bluetooth_api {\n"
    "esp_err_t handle_bluetooth_status(httpd_req_t* req) {\n"
    "  const char* tok = auth_token_storage();\n"
    "  const bool digits = tok != nullptr && api_auth_bearer_grant(req, tok);\n"
    "  return httpd_resp_sendstr(req, digits ? \"{\\\"pin\\\":1}\" : \"{}\");\n"
    "}\n"
    "}\n"
)
_FIX_TABLES = {
    "answers": {"handle_qr": ("token", "fixture"), "handle_kit": ("ap", "fixture")},
    "local": {},
    "hands": {},
    "grants": {"handle_kit": {"refusal": "!claimed && !gate_take()", "flag": "claimed",
                              "writes": ("bool claimed = false;", "claimed = claim_take(")}},
    "broker_uses": {"write_row": "fixture", "handle_config_post": "fixture"},
    "token_copies": {
        "csi_integration.cpp": {"copy": r"\bg_api_token\b", "keep": ("CSI_AUTH_OR_RETURN", "init"),
                                "decl": ("const char* g_api_token = nullptr;",), "read": ()},
        "csi_mqtt.cpp": {"copy": r"\bs_api_token_provider\b", "keep": ("set_api_token_provider",),
                         "decl": ("const char* (*s_api_token_provider)() = nullptr;",),
                         "read": ("handle_config_get", "handle_config_post")},
    },
}
_FIX_HANDLERS = {"handle_qr", "handle_kit", "handle_status", "handle_config_get",
                 "handle_config_post"}
# (what the plant is, file, text replaced, replacement, a word the failure names)
_FIX_PLANTS = (
    ("the QR behind a gate a session passes", "canary_wap.ino",
     "if (!api_auth_check_bearer(req, g_device.api_token_str)) return ESP_OK;\n"
     "  return httpd_resp_sendstr(req, g_device.api_token_str);",
     "if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;\n"
     "  return httpd_resp_sendstr(req, g_device.api_token_str);", "names api_auth_check,"),
    ("the kit's grant also taking the cookie", "canary_wap.ino",
     "if (api_auth_bearer_grant(req, g_device.api_token_str)) {",
     "if (api_auth_bearer_grant(req, g_device.api_token_str) || cv_session_validate(req)) {",
     "names cv_session_validate,"),
    ("the QR answering above its refusal", "canary_wap.ino",
     "  if (!api_auth_check_bearer(req, g_device.api_token_str)) return ESP_OK;\n"
     "  return httpd_resp_sendstr(req, g_device.api_token_str);",
     "  httpd_resp_sendstr(req, g_device.api_token_str);\n"
     "  if (!api_auth_check_bearer(req, g_device.api_token_str)) return ESP_OK;\n"
     "  return ESP_OK;", "before any refusal"),
    ("a refusal that does not return", "canary_wap.ino",
     "if (!api_auth_check_bearer(req, g_device.api_token_str)) return ESP_OK;",
     "if (!api_auth_check_bearer(req, g_device.api_token_str)) log_refusal(req);",
     "before any refusal"),
    ("a gate result stored and tested later", "canary_wap.ino",
     "if (api_auth_bearer_grant(req, g_device.api_token_str)) {",
     "bool ok = api_auth_bearer_grant(req, g_device.api_token_str);\n  if (ok) {",
     "whole condition"),
    ("a new route that reaches the kit behind a session", "canary_wap.ino",
     "  return httpd_resp_sendstr(req, \"{}\");", "  send_kit(req);\n  return ESP_OK;",
     "not in SECRET_ANSWERS"),
    ("a session wrapper on the way to the secret", "canary_wap.ino",
     "static void send_kit(httpd_req_t* req) {\n",
     "static bool signed_in(httpd_req_t* r) { return cv_session_validate(r); }\n"
     "static void send_kit(httpd_req_t* req) {\n  if (!signed_in(req)) return;\n",
     "names signed_in,"),
    ("the claim flag set by hand", "canary_wap.ino",
     "  claimed = claim_take(presented);\n", "  claimed = true;\n", "claimed"),
    ("a second api_auth_bearer_valid over the cookie", "csi_integration.cpp", "",
     "static bool api_auth_bearer_valid(httpd_req_t* req, const char* t) {\n"
     "  return t && cv_session_validate(req);\n}\n", "defined in"),
    ("the bearer gate taking the cookie", "api_auth.h",
     "static bool api_auth_bearer_valid(httpd_req_t* req, const char* t) {\n"
     "  return t && header_matches(req, t);",
     "static bool api_auth_bearer_valid(httpd_req_t* req, const char* t) {\n"
     "  return cv_session_validate(req) || header_matches(req, t);",
     "api_auth_bearer_valid() names cv_session_validate"),
    ("the receipt's lockout grant taking the cookie", "api_auth.h",
     "  if (auth_is_locked_out()) return false;\n",
     "  if (cv_session_validate(req)) return true;\n  if (auth_is_locked_out()) return false;\n",
     "api_auth_bearer_grant() names cv_session_validate"),
    ("the move gate's bearer taking a session", "csi_mqtt.cpp",
     "&& api_auth_bearer_grant(req, tok, &held);", "&& authorized(req, tok);",
     "a bearer that names authorized"),
    ("the move gate's bearer also taking the cookie", "csi_mqtt.cpp",
     "&& api_auth_bearer_grant(req, tok, &held);",
     "&& (api_auth_bearer_grant(req, tok, &held) || authorized(req, tok));",
     "a bearer that names authorized"),
    ("the move gate also letting a session through", "csi_mqtt.cpp",
     "if (move_needs_password(before, c, typed, bearer)) {",
     "if (move_needs_password(before, c, typed, bearer) && !authorized(req, tok)) {",
     "move gate names authorized"),
    ("a dead move gate", "csi_mqtt.cpp",
     "if (move_needs_password(", "if (false && move_needs_password(", "not the whole condition"),
    ("the move gate handed no bearer", "csi_mqtt.cpp",
     "typed, bearer)) {", "typed, true)) {", "no bearer"),
    ("the move gate asking a session itself", "csi_mqtt.cpp",
     "  if (bearer) return false;\n",
     "  if (bearer || csi_integration::session_validate_cookie(nullptr)) return false;\n",
     "move_needs_password() names session_validate_cookie"),
    ("the broker save above its move gate", "csi_mqtt.cpp",
     "  if (move_needs_password(",
     "  if (!write_row(c)) return ESP_OK;\n  if (move_needs_password(", "no move gate before it"),
    ("a move gate that does not return", "csi_mqtt.cpp",
     "    httpd_resp_set_status(req, \"403 Forbidden\");\n    return ESP_OK;\n",
     "    httpd_resp_set_status(req, \"403 Forbidden\");\n", "no move gate before it"),
    ("the GET answering the broker password", "csi_mqtt.cpp",
     "c.pass[0] ? \"set\" : \"none\"", "c.pass", "handle_config_get"),
    ("a module route answering its copy of the token", "csi_integration.cpp",
     "  return httpd_resp_sendstr(req, \"{}\");", "  return httpd_resp_sendstr(req, g_api_token);",
     "handle_csi_status() names its module's copy"),
    ("a module handler answering the token it read for its gate", "csi_mqtt.cpp",
     "  if (!authorized(req, tok, &held)) return ESP_OK;\n  Config c;\n  return",
     "  if (!authorized(req, tok, &held)) return ESP_OK;\n  httpd_resp_sendstr(req, tok);\n"
     "  Config c;\n  return", "other than as a gate's token argument"),
    ("a module handler reading the token it tests for null", "csi_mqtt.cpp",
     "tok != nullptr && api_auth_bearer_grant(req, tok, &held);",
     "tok[0] != '\\0' && api_auth_bearer_grant(req, tok, &held);", "or in a null test"),
    ("a module handler handing its token on past a gate's held out-parameter", "csi_mqtt.cpp",
     "authorized(req, tok, &held)", "authorized(req, tok, held_of(tok))",
     "other than as a gate's token argument"),
    # Every bearer compare accounted (sweep F339): the raw compares outside
    # api_auth.h, with no allowlist.
    ("the move gate's bearer through the raw compare", "csi_mqtt.cpp",
     "&& api_auth_bearer_grant(req, tok, &held);", "&& api_auth_bearer_valid(req, tok);",
     "calls api_auth_bearer_valid(), the raw compare"),
    ("a module's answer field through the raw compare (F346's passkey)", "bluetooth_api.h",
     "api_auth_bearer_grant(req, tok);", "api_auth_bearer_valid(req, tok);",
     "bluetooth_api.h:4: calls api_auth_bearer_valid(), the raw compare"),
    ("a module's own constant_time_compare() of a token", "csi_integration.cpp", "",
     "static bool same(const char* a, const char* b) { return constant_time_compare(a, b, 32); }\n",
     "calls constant_time_compare(), the raw compare"),
    ("the module's copy named outside every function", "csi_integration.cpp",
     "bool init(", "const char* g_leak = g_api_token;\nbool init(", "outside every function"),
    ("the token handed to a module whose copy is not listed", "canary_wap.ino",
     "  csi_integration::init(server, g_device.api_token_str);\n",
     "  csi_integration::init(server, g_device.api_token_str);\n"
     "  vault_api::register_routes(server, g_device.api_token_str);\n",
     "hands the token to vault_api::"),
)


def _fixture_sources() -> dict:
    return {"api_auth.h": _FIX_API_AUTH, "canary_wap.ino": _FIX_INO,
            "csi_integration.cpp": _FIX_CSI, "csi_mqtt.cpp": _FIX_MQTT,
            "bluetooth_api.h": _FIX_BT}


def _fixture_problems(sources: dict) -> list:
    """The rules the fixtures exercise: the secret rule and the raw compare
    rule, as main() runs them over the tree."""
    return (answer_gate_rule_problems(sources, _FIX_HANDLERS, **_FIX_TABLES)
            + raw_compare_problems(sources))


def _assert_secret_rules() -> None:
    """Each plant fails the secret rule or the raw compare rule for its
    reason; the clean fixture passes both. Runs every time, before the tree."""
    clean = _fixture_problems(_fixture_sources())
    assert clean == [], "the secret rule's clean fixture fails: " + "; ".join(clean)
    for what, fname, old, new, word in _FIX_PLANTS:
        sources = _fixture_sources()
        assert old in sources[fname], f"fixture plant {what!r}: its text is not in {fname}"
        sources[fname] = sources[fname].replace(old, new, 1) if old else sources[fname] + new
        got = _fixture_problems(sources)
        assert any(word in p for p in got), (
            f"the secret rule passed a plant ({what}): expected a problem naming {word!r}, "
            f"got {got!r}")
    # The raw compare rule reads code: a comment (stripped, as main() strips
    # every source) or a string that names a raw compare is no call of it.
    quiet = {"x.cpp": strip_comments('// api_auth_bearer_valid(req, t)\n'
                                     'const char* s = "constant_time_compare(a, b, 3)";\n'
                                     '/* constant_time_compare( */\n')}
    assert raw_compare_problems(quiet) == [], (
        "the raw compare rule read a comment or a string as a call: "
        + "; ".join(raw_compare_problems(quiet)))
    assert len(_FIX_PLANTS) == 29, "the module docstring counts the plants: update it"


def main() -> int:
    _assert_parser_robust()
    _assert_secret_rules()
    sources = {}
    secret_sources = {}  # the same, raw strings blanked first (the secret rule)
    for path in sorted(SKETCH.glob("*.h")) + sorted(SKETCH.glob("*.cpp")) + sorted(
            SKETCH.glob("*.ino")):
        if path.name == "web_assets_gz.h":
            continue  # generated byte arrays
        raw = path.read_text(errors="replace")
        sources[path.name] = strip_comments(raw)
        secret_sources[path.name] = strip_comments(blank_raw_strings(raw))

    routes = []  # (uri, method, handler_expr, file)

    # Style 1: httpd_uri_t initializers (single- or multi-line).
    uri_decl = re.compile(
        r"httpd_uri_t\s+\w+\s*=\s*\{(.*?)\}\s*;", re.S)
    for fname, text in sources.items():
        for m in uri_decl.finditer(text):
            fields = m.group(1)
            uri = re.search(r'\.uri\s*=\s*"([^"]+)"', fields)
            method = re.search(r"\.method\s*=\s*HTTP_(\w+)", fields)
            handler = re.search(r"\.handler\s*=\s*([A-Za-z_][\w:<>]*)", fields)
            if uri and method and handler:
                routes.append((uri.group(1), method.group(1),
                               handler.group(1), fname))

    # Reachability: a module's register_api_handler routes are only live if
    # that module's register function is actually called from the sketch.
    # rf_presence_api::register_routes, for example, has no call site — its
    # /api/rf/* handlers are dead code, not an unauthenticated exposure — so
    # counting them would be dishonest. Track which namespaces the .ino
    # actually wires up.
    ino = sources.get("canary_wap.ino", "")
    unreachable = []

    # Style 2: module register_api_handler("uri", HTTP_X, handler) calls.
    reg_call = re.compile(
        r'register_api_handler\s*\(\s*server\s*,\s*"([^"]+)"\s*,\s*'
        r"HTTP_(\w+)\s*,\s*([A-Za-z_][\w:<>]*)")
    for fname, text in sources.items():
        ns = re.search(r"namespace\s+(\w+)\s*\{", text)
        namespace = ns.group(1) if ns else None
        module_wired = (
            namespace is not None
            and re.search(re.escape(namespace) + r"::register\w*\s*\(", ino)
            is not None
        )
        for m in reg_call.finditer(text):
            if namespace and not module_wired:
                unreachable.append((m.group(1), m.group(2), namespace, fname))
                continue
            routes.append((m.group(1), m.group(2), m.group(3), fname))

    if unreachable:
        by_mod = sorted({f"{ns} ({fn})" for _, _, ns, fn in unreachable})
        print(f"note: {len(unreachable)} route(s) skipped as unreachable "
              f"(register function never called from the sketch): "
              + ", ".join(by_mod))

    if len(routes) < 100:
        print(f"FAIL: parsed only {len(routes)} routes — parser regression?")
        return 1

    failures = []
    matched_allowlist = set()
    for uri, method, handler, fname in routes:
        key = (method, uri)
        if key in PUBLIC_ALLOWLIST:
            matched_allowlist.add(key)
            continue
        if "auth_gated<" in handler:
            continue
        base = handler.split("::")[-1].split("<")[0]
        if base in SELF_GATING_HANDLERS:
            continue
        bodies = [b for b in (function_body(t, base) for t in sources.values())
                  if b]
        if not bodies:
            failures.append(f"{method} {uri}: handler {handler} not found "
                            f"(registered in {fname})")
            continue
        if not any(marker in body for body in bodies
                   for marker in AUTH_MARKERS):
            failures.append(f"{method} {uri}: handler {handler} "
                            f"({fname}) has NO credential gate and is not "
                            f"on the public allowlist")

    failures += answer_gate_rule_problems(secret_sources, handler_names(routes))
    failures += raw_compare_problems(secret_sources)

    stale = set(PUBLIC_ALLOWLIST) - matched_allowlist
    for key in sorted(stale):
        failures.append(f"allowlist entry {key[0]} {key[1]} matches no "
                        f"registered route — remove or fix it")

    if failures:
        print(f"ROUTE SECURITY CHECK FAILED ({len(failures)} problem(s)):")
        for f in failures:
            print("  -", f)
        return 1

    print(f"route security OK: {len(routes)} registrations checked, "
          f"{len(matched_allowlist)} documented-public, rest gated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
