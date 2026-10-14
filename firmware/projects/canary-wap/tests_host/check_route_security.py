#!/usr/bin/env python3
"""Route-security conformance check for the canary-wap firmware.

Every HTTP route the sketch registers must be credential-gated — Bearer
token, session cookie, pair token, or the physical provisioning gate —
unless it is on the explicit PUBLIC allowlist below with a written
justification. This pins the fail-closed property statically so a new
endpoint (or a resurrected one: three /api/ble routes shipped without
auth and were only unreachable because they overflowed the handler
table) cannot silently widen the unauthenticated surface. A route whose
handler is a lambda fails: the check reads gates by handler name. So does a
route registered in a shape the table does not read. Registrations are read
per function: every httpd_uri_t is an `httpd_uri_t name = { .uri = "...",
.method = HTTP_X, .handler = name };` inside a function, every
httpd_register_uri_handler() is a call `(server, &name)` after that
initializer in the same function, and the name appears nowhere else in that
function. Fields assigned one by one or after the initializer, a positional
initializer, an array, an initializer at file scope or in another function, a
copy or a pointer each fail. Every register_api_handler() is a call that
names its uri as a literal, and the helper itself has the modules' body.

It also holds the routes that answer a credential (sweep F289). The
landing page at / mints a session for any caller that reaches the device,
on the home network too, so a route that answers the bearer token or the
SoftAP password takes the bearer header or a session minted beside a served
receipt (CREDENTIAL_GATES), never a gate that takes any session or the
landing's pair token. Names are matched in the comment-stripped code with
string literals blanked; a call is `name(` with any space before the
parenthesis and any `::` qualifier. What it reads, and nothing more:

  - canary_wap.ino: every function that reads the bearer token, the SoftAP
    password, the device key or the TLS key (a `.api_token_str`,
    `.ap_password` or `.privkey` member, `g_tls_key_der`, the whole
    `g_device`, or a call to a CREDENTIAL_READERS "hands" function such as
    derive_api_token() or resolve_ap_password()), other than as the token a
    gate compares against, is named in CREDENTIAL_READERS with the secrets
    it may read, as an HTTP answer, a hand-out to its caller, or a local use
    (serial, NVS, the AP's own bring-up, a module's init). An unnamed
    reader fails, a reader that reads a secret its entry does not name
    fails, a local use with a writable char or byte buffer parameter (a
    hand-out in disguise) fails, a secret named outside every function
    fails unless it is one of the declarations in OUTSIDE_FUNCTION_LINES,
    and a macro that names a secret or g_device fails.
  - An answer sits in, or is called only from, a registered route handler
    whose credential gate is the whole condition of an `if` whose branch
    ends in a return: `if (!gate(...)) return ...;` refuses, and
    `if (gate(...)) { ... return ...; }` grants and leaves. A gate call
    anywhere else (a stored result, a second term, outside an if) fails,
    and so does a refusal or grant that does not return. Each answer site
    (a secret the handler reads, or its call of the answer) comes after a
    refusal at the top of the handler's body or inside a grant's branch.
    For the receipt, OTHER_GRANTS adds one refusal and two grants it cannot
    read as a credential gate: `if (!claim_served && !provisioning_gate_take())`
    at the top of the body, returning, and an `if (claim_served)` or
    `if (provisioning_gate_take())` whose branch returns; claim_served is
    read only as a condition and written only as `false` and by the claim's
    take. An answer above all of them fails, the top of the handler
    included. The handler and the
    answer hold no goto and no preprocessor line, name no gate in the
    any-session class (called or not), and call nothing but the credential
    gates, the answers and ANSWER_CALLS, an allowlist checked against the
    class: so a gate reached through a wrapper, a macro, a pointer, a
    lambda or a template helper fails as a name the answer may not call.
    The any-session class is ANY_SESSION_GATES and, to a fixed point,
    every function in the sketch whose body calls one and every macro
    whose body names one (so qr_scan_request_allowed() and
    wifi_change_authorize() are in it by what they call).
  - Every sketch file: a gate in the any-session class is named only inside
    a function the check reads, in a #define, in a declaration or in a raw
    string; a lambda or a pointer at file scope, an initializer or a
    function head the check cannot parse that names one fails. Each
    credential gate is defined once, in its CREDENTIAL_GATE_HOMES file, and
    named elsewhere only as a call (bare, `::` or `csi_integration::`), a
    `name &&` test of a weak symbol, or a declaration: a second definition,
    a macro or a local by that name fails.
  - every other sketch file: each line that names a module's copy of the
    token (csi_integration.cpp's g_api_token, a register_routes module's
    auth_token_storage(), csi_mqtt.cpp's s_api_token_provider) has one of
    the shapes in TOKEN_COPY_LINES, and in every function, at any
    indentation, a `tok` read from one goes only to an auth check or a null
    test.
  - csi_mqtt.cpp's POST /api/mqtt/config: the owner's gate,
    `if (moves_the_kept_password(c, password_sent, ca_changed) &&
    !authorized_for_credential(req, tok))`, stands before config_save() and
    its block returns, and authorized_for_credential() takes the bearer
    header or an owner's session and no gate in the any-session class.
    Whether moves_the_kept_password() is right, and the save's behavior,
    are test_mqtt_reinit's to prove, not this rule's.

It is a text check, and these pass it: a secret reached through a name it
does not know (a copy in a global made by a function allowed to read it, a
member under another name); what a function ANSWER_CALLS names does inside
(it is held only to call no gate in the class, so one that calls through a
pointer, its own or one another function stored, is trusted by name); whether the receipt's
BOOT tap and setup-door claim decide right; and a gate that decides wrong.
A gate result stored in a variable and tested later is refused though it
may be right. The host suites drive the behavior: test_receipt_session the
receipt's grants and the QR's, with the station link up and down, and
test_mqtt_reinit the broker save.

Run from the repo root (CI: firmware.yml host tests):
    python3 firmware/projects/canary-wap/tests_host/check_route_security.py
"""

import functools
import re
import sys
from pathlib import Path

SKETCH = Path("firmware/projects/canary-wap/arduino/canary_wap")


def _calls(names) -> re.Pattern:
    """A call of any of `names`: `name(`, or `name<` for a template, with
    any space before it and any qualifier in front (\\b after `::`)."""
    alt = "|".join(sorted((re.escape(n) for n in names), key=len, reverse=True))
    return re.compile(r"\b(" + alt + r")\s*[(<]")


# Names whose call in a handler body (or its registration expression)
# counts as a credential gate. Each is a real call the auth audit verified
# enforces access.
AUTH_MARKERS = (
    "api_auth_check",            # Bearer + session cookie (401/403/429)
    "api_auth_check_or_query",   # + ?token= for <img>/stream tags
    "api_auth_check_optional",   # silent variant (caller enforces)
    "api_auth_check_credential", # Bearer header or an owner's session (F289)
    "api_auth_check_credential_optional",  # its silent variant (the receipt's grant 1)
    "CSI_AUTH_OR_RETURN",        # csi_integration session/Bearer macro
    "pair_token_valid",          # 10-min wizard pair token
    "wifi_change_authorize",     # pair token OR lockout-throttled admin cred
    "qr_scan_request_allowed",   # pair token OR admin credential
    "provisioning_gate_is_open", # physical BOOT-press gate
    "session_validate_cookie",   # cv_session cookie (self-gating pages)
    "authorized",                # csi_mqtt session/Bearer helper
    "auth_gated",                # module trampolines (bt_/bcn_/plain), auth_gated<...>
)
_AUTH = _calls(AUTH_MARKERS)

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
    "handle_https_redirect": "301 to https at receipt_authority.h's address (503 when it has none); no data",
    "handle_tune_page": "session-cookie check, 302 to / when absent",
    "handle_wifi_pair_token": "gated on setup_wizard::is_active + direct Host",
}


# ── Routes that answer a credential (sweep F289) ──────────────────────────
#
# The gates that may stand in front of an answer that carries the bearer
# token or the SoftAP password: the Authorization header, or a session minted
# beside a served receipt (a BOOT tap or a setup-door claim). The first two
# are what a route handler calls; the rest are the primitives they are built
# from (api_auth.h, csi_integration.cpp).
CREDENTIAL_GATES = (
    "api_auth_check_credential",
    "api_auth_check_credential_optional",
    "api_auth_check_header",
    "api_auth_bearer_header_ok",
    "cv_session_validate_owner",
    "session_validate_owner_cookie",
)
# The seeds of the any-session class: gates that take any live session, the
# landing page's included, or the pair token the landing page hands any
# caller. The class is these and, to a fixed point, every function in the
# sketch whose body calls one (any_session_class), so a wrapper is in it by
# what it calls; the wrappers named here are named for the reader.
ANY_SESSION_GATES = (
    "api_auth_check",
    "api_auth_check_optional",
    "api_auth_check_or_query",
    "CSI_AUTH_OR_RETURN",
    "cv_session_validate",
    "session_validate_cookie",
    "authorized",
    "auth_gated",
    "pair_token_valid",
    "pair_token_consume",
    "pair_token_issue",
    "wifi_change_authorize",
    "qr_scan_request_allowed",
)
# Where each credential gate is defined, once: a gate of that name defined
# anywhere else (a namespace's own api_auth_check_credential over any
# session, a macro, a local of that name) would take its place unseen. A
# call names it bare, `::` or `csi_integration::`; a weak symbol is tested
# `name &&` before its call.
CREDENTIAL_GATE_HOMES = {
    "api_auth_check_credential": "api_auth.h",
    "api_auth_check_credential_optional": "api_auth.h",
    "api_auth_check_header": "api_auth.h",
    "api_auth_bearer_header_ok": "api_auth.h",
    "cv_session_validate_owner": "csi_integration.cpp",
    "session_validate_owner_cookie": "csi_integration.cpp",
}
# Everything a credential answer may call besides the credential gates and
# the answers themselves, by the called name (a method or a namespaced
# function by its last part). A new call fails until it is named here, so a
# gate reached through a name this check cannot classify (a wrapper, a
# macro, a pointer, a lambda) fails as an unknown call; a sketch function
# named here that calls a gate in the any-session class fails too.
ANSWER_CALLS = frozenset((
    # ESP-IDF's HTTP server: the answer and the query it reads.
    "httpd_resp_send_err", "httpd_resp_sendstr", "httpd_resp_set_hdr", "httpd_resp_set_status",
    "httpd_resp_set_type", "httpd_query_key_value", "httpd_req_get_url_query_len",
    "httpd_req_get_url_query_str",
    # libc and Arduino.
    "snprintf", "malloc", "free", "millis", "println", "c_str", "toString", "isConnected", "softAPIP",
    # The sketch's own: the QR, the error answer, the wipe, the health log, the
    # receipt's reach and pseudonym and writers, the BOOT tap, the claim's take
    # and the owner's session the receipt mints.
    "send_qr_svg", "http_send_error", "secure_zero", "log_health", "request_reach", "device_id_hex",
    "write", "provisioning_receipt", "provisioning_gate_take", "take", "session_issue",
))
# Handlers whose answers also stand behind grants this check cannot read as
# a credential gate. The handler still needs its credential gate in an `if`
# of the shape below, and every other rule holds; the entry names where else
# an answer may sit. "refusal": the whole condition (spaces aside) of an
# `if` at the top of the handler's body whose branch returns, the refusal on
# those grants: an answer after it is the host suite's. "grants": the whole
# conditions of an `if` whose returning branch is one of those grants: an
# answer inside one is the suite's too. "flag": the variable a grant sets,
# read only as a condition (`!flag`, `flag &&`, `flag ?`, `if (flag)`) and
# written only by the "writes" statements (one ending in `(` is that call,
# whole, then `;`). An answer before the refusal and outside every grant's
# branch, the top of the handler included, fails like any other handler's.
OTHER_GRANTS = {
    "handle_provisioning_receipt": {
        "refusal": "!claim_served && !provisioning_gate_take()",
        "grants": ("claim_served", "provisioning_gate_take()"),
        "flag": "claim_served",
        "writes": ("bool claim_served = false;", "claim_served = canary::net::claim_ticket::take("),
        "why": "the BOOT tap (provisioning_gate_take()) and the setup-door claim (claim_ticket::take()), "
               "which test_receipt_session drives with the station link up and down",
    },
}
# Names `_strict_calls` sees as calls that are not calls.
_NOT_CALLS = frozenset(("if", "for", "while", "switch", "return", "sizeof", "alignof", "decltype",
                        "static_cast", "reinterpret_cast", "const_cast", "dynamic_cast", "catch",
                        "noexcept", "defined"))

# The secrets, and how a function reads one: a member of that name on any
# object (g_device.api_token_str, (&g_device)->ap_password), the TLS key's
# global, or the whole g_device (every secret at once).
SECRETS = {
    "token": "the bearer token",
    "ap": "the SoftAP password",
    "key": "the device key",
    "tls": "the TLS key",
}
_SECRET_READS = {
    "token": re.compile(r"(?:\.|->)\s*api_token_str\b"),
    "ap": re.compile(r"(?:\.|->)\s*ap_password\b"),
    "key": re.compile(r"(?:\.|->)\s*privkey\b"),
    "tls": re.compile(r"\bg_tls_key_der\b"),
}
_CALL = re.compile(r"\b([A-Za-z_]\w*)\s*[(<]")
_WHOLE_DEVICE = re.compile(r"\bg_device\b(?!\s*(?:\.|->)\s*\w)")
# The token a gate or a module compares against is not a read.
_COMPARER = re.compile(
    r"\b(?:api_auth_check\w*|\w+::register_routes|csi_integration::init)\s*\(\s*\w+"
    r"\s*,\s*g_device\s*\.\s*api_token_str\s*\)")

# Every function in canary_wap.ino that reads a secret other than as the
# token a gate or a module compares against: (kind, the secrets it may read,
# why). "answer": it puts a secret in an HTTP answer. "hands": it gives the
# secret to its caller (an out-param or a return value), so every caller
# reads that secret too: the secrets in its set after the "->" are the ones
# it gives. "local": it neither answers nor hands a secret out.
def _r(kind: str, reads: str, why: str, gives: str = "") -> tuple:
    return (kind, frozenset(reads.split()), why, frozenset(gives.split()))


CREDENTIAL_READERS = {
    "send_provisioning_receipt": _r("answer", "token ap", "the receipt's token and ap_password (the recovery kit)"),
    "handle_pairing_qr": _r("answer", "token", "the pairing QR's payload carries the token"),
    "derive_api_token": _r("hands", "", "computes the token, from the key it is given, into its caller's buffer",
                           gives="token"),
    "derive_ap_password": _r("hands", "", "computes the SoftAP password, from the key it is given, into its "
                             "caller's buffer", gives="ap"),
    "resolve_ap_password": _r("hands", "ap key", "copies the SoftAP password (derived from the device key "
                              "when it is missing) into its caller's buffer", gives="ap"),
    "wifi_raise_ap": _r("local", "ap", "raises the SoftAP with its own password"),
    "wifi_init_provisioning": _r("local", "ap", "raises the setup SoftAP with its own password"),
    "provision_device": _r("local", "token ap key", "derives and stores the key, the token and the AP password "
                           "at first boot"),
    "print_quick_connect_details": _r("local", "token ap", "USB serial, at the desk"),
    "handle_serial_commands": _r("local", "token ap", "USB serial commands"),
    "setup": _r("local", "token ap key", "boot banner on USB serial; hands the key to the mesh"),
    "loop": _r("local", "key", "hands the key to the data-management pass"),
    "register_api_routes": _r("local", "key", "hands the key to device_signature"),
    "create_witness_record": _r("local", "key", "signs a witness record"),
    "tls_load_from_nvs": _r("local", "tls", "reads the TLS key from NVS"),
    "tls_store_to_nvs": _r("local", "tls", "writes the TLS key to NVS"),
    "tls_generate_self_signed_cert": _r("local", "tls", "makes the TLS key"),
    "start_http_server": _r("local", "tls", "hands the TLS key to the HTTPS server"),
}
# The declarations outside every function that name a secret (whitespace
# folded). Anything else there (a macro, a struct member function, a global
# initialized from one) fails.
OUTSIDE_FUNCTION_LINES = (
    "static DeviceIdentity g_device;",
    "static uint8_t* g_tls_key_der = nullptr;",
    "static bool resolve_ap_password(char* out_password, size_t out_len);",
)

# Each line, anywhere but canary_wap.ino, that names a module's copy of the
# token, in the shapes it has today (whitespace folded): stored, checked for
# null, handed to an auth check, read into a `tok` that only an auth check
# reads. Anything else (a copy answered, logged, copied on) fails.
TOKEN_COPY = re.compile(r"\bg_api_token\b|\bauth_token_storage\s*\(\s*\)|\bs_api_token_provider\b")
TOKEN_COPY_LINES = (
    r"const char\* g_api_token = nullptr;",
    r"if \(!g_api_token\) \{",
    r"if \(api_auth_check_optional\(\(req\), g_api_token\)\) break;",
    r"g_api_token = api_token;",
    r"csi_mqtt::set_api_token_provider\(\[\]\(\) -> const char\* \{ return g_api_token; \}\);",
    r"inline const char\*& auth_token_storage\(\) \{",
    r"const char\* tok = auth_token_storage\(\);",
    r"auth_token_storage\(\) = api_token;",
    r"const char\* \(\*s_api_token_provider\)\(\) = nullptr;",
    r"const char\* tok = s_api_token_provider \? s_api_token_provider\(\) : nullptr;",
    r"s_api_token_provider = fn;",
)
# What a `tok` read from a copy may meet: its own declaration, an auth check
# that compares against it, a null test.
_TOK_USES = re.compile(
    r"const char\* tok = [^;]*;"
    r"|\b(?:api_auth_check\w*|authorized|authorized_for_credential)\s*\(\s*\(?req\)?\s*,\s*tok\s*\)"
    r"|\btok\s*&&|!\s*tok\b")
# A function head: a return type, the name, parameters with balanced
# parentheses (one level, for a function-pointer parameter), then `{`. The
# balance keeps the last line of a multi-line call or condition
# (`    s, sizeof(s)) {`) from reading as a head.
_HEAD = (r"(?!(?:if|for|while|switch|return|else|do|case|catch)\b)[A-Za-z_][\w:<>\*&\s,]*?"
         r"\b([A-Za-z_]\w*)\s*\(([^;{}()]*(?:\([^;{}()]*\)[^;{}()]*)*)\)\s*(?:const\s*)?\{")
_FUNC_HEAD = re.compile(r"^" + _HEAD, re.M)
_ANY_FUNC_HEAD = re.compile(r"^[ \t]*" + _HEAD, re.M)
# A preprocessor definition, its continuation lines included.
_DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)((?:[^\n]*\\\n)*[^\n]*)", re.M)
# A raw string literal (R"delim( ... )delim"), which blank_string_contents
# does not read: the any-session rule skips a gate name inside one.
_RAW_STRING = re.compile(r'R"([^()\\\s"]{0,16})\((.*?)\)\1"', re.S)
# A parameter a secret could be handed out through: a writable char or byte
# buffer (char* out, uint8_t buf[], char& c), or a returned char pointer.
_BUFFER_PARAM = re.compile(r"\b(?:char|uint8_t|unsigned\s+char|int8_t)\b[^,]*[\*\[&]")


@functools.lru_cache(maxsize=None)
def functions(text: str, indented: bool = False) -> tuple:
    """(name, head, body) for every function defined in `text` (a
    comment-stripped source), at column 0 or, with `indented`, at any
    indentation (struct members, namespaced code); braces matched over the
    string-blanked view. `body` is the text after the head's `{`."""
    blanked = blank_string_contents(text)
    out = []
    for m in (_ANY_FUNC_HEAD if indented else _FUNC_HEAD).finditer(blanked):
        depth, i = 1, m.end()
        while i < len(blanked) and depth:
            if blanked[i] == "{":
                depth += 1
            elif blanked[i] == "}":
                depth -= 1
            i += 1
        out.append((m.group(1), text[m.start():m.end()], text[m.end():i], (m.start(), i)))
    return tuple(out)


def top_level_functions(text: str):
    """(name, whole text) for every function defined at column 0."""
    return [(n, h + b) for n, h, b, _span in functions(text)]


def any_session_class(sources: dict) -> set:
    """ANY_SESSION_GATES and, to a fixed point, every function in `sources`
    (name -> comment-stripped text) whose body calls one of them and every
    macro whose body names one. The credential gates are never in it: the
    host suite proves what they take. A gate named anywhere else (a lambda
    or pointer at file scope, a function head this check cannot parse) is
    gate_outside_problems()'s."""
    calls = {}
    for text in sources.values():
        for name, _head, body, _span in functions(text, indented=True):
            calls.setdefault(name, set()).update(_called(blank_string_contents(body)))
        for m in _DEFINE.finditer(blank_string_contents(text)):
            calls.setdefault(m.group(1), set()).update(_identifiers(m.group(2)))
    members = set(ANY_SESSION_GATES)
    while True:
        grown = {name for name, called in calls.items()
                 if name not in members and name not in CREDENTIAL_GATES and called & members}
        if not grown:
            return members
        members |= grown


def class_reason(name: str, sources: dict) -> str:
    """Why `name` is in the any-session class, for a message: it is a seed,
    or the chain of calls (or a macro's names) from it to one, with the
    files that define it."""
    if name in ANY_SESSION_GATES:
        return f"{name} is one of ANY_SESSION_GATES"
    calls, homes = {}, {}
    for fname, text in sorted(sources.items()):
        for fn, _head, body, _span in functions(text, indented=True):
            calls.setdefault(fn, set()).update(_called(blank_string_contents(body)))
            homes.setdefault(fn, set()).add(fname)
        for m in _DEFINE.finditer(blank_string_contents(text)):
            calls.setdefault(m.group(1), set()).update(_identifiers(m.group(2)))
            homes.setdefault(m.group(1), set()).add(fname)
    prev, queue, seed = {name: None}, [name], None
    while queue and seed is None:
        here = queue.pop(0)
        for called in sorted(calls.get(here, ())):
            if called in prev or called in CREDENTIAL_GATES:
                continue
            prev[called] = here
            if called in ANY_SESSION_GATES:
                seed = called
                break
            queue.append(called)
    where = ", ".join(sorted(homes.get(name, ()))) or "no file the check reads"
    if seed is None:
        return f"{name} in {where}"
    chain = [seed]
    while prev[chain[-1]] is not None:
        chain.append(prev[chain[-1]])
    return f"{name} in {where} reaches {seed}() through " + " -> ".join(c + "()" for c in reversed(chain))


def _called(code: str) -> set:
    """Every name `code` (string-blanked) calls: `name(` or `name<`."""
    return {m.group(1) for m in _CALL.finditer(code)}


def _identifiers(code: str) -> set:
    """Every name `code` (string-blanked) names, called or not."""
    return set(re.findall(r"\b[A-Za-z_]\w*\b", code))


def _strict_calls(code: str) -> set:
    """Every name `code` (string-blanked) calls, `name(` or `name<...>(`,
    the keywords that look like calls left out."""
    found = {m.group(1) for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", code)}
    found |= {m.group(1) for m in re.finditer(r"\b([A-Za-z_]\w*)\s*<[^;(){}<>]*>\s*\(", code)}
    return found - _NOT_CALLS


def _close(code: str, i: int) -> int:
    """The index of the bracket that closes the one at `code[i]` (string-
    blanked code), or -1."""
    pair = {"(": ")", "{": "}", "[": "]"}[code[i]]
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
    """The index of the `;` that ends the statement starting at `k`, outside
    any bracket, or -1."""
    depth = 0
    for j in range(k, len(code)):
        c = code[j]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth < 0:
                return -1
        elif c == ";" and depth == 0:
            return j
    return -1


def _returns(code: str, k: int):
    """The branch starting at `k` (after an `if`'s condition): (start, end,
    whether it returns). A branch returns when it is a `return` statement
    or a block whose last statement is one."""
    while k < len(code) and code[k].isspace():
        k += 1
    if k >= len(code):
        return k, k, False
    if code[k] == "{":
        end = _close(code, k)
        if end < 0:
            return k, len(code), False
        inner = code[k + 1:end].rstrip()
        if not inner.endswith(";"):
            return k, end + 1, False
        depth, j = 0, len(inner) - 2
        while j >= 0:
            c = inner[j]
            if c in ")]":
                depth += 1
            elif c in "([":
                depth -= 1
            elif c in ";{}" and depth == 0:
                break
            j -= 1
        return k, end + 1, re.match(r"\s*return\b", inner[j + 1:]) is not None
    end = _statement_end(code, k)
    if end < 0:
        return k, len(code), False
    return k, end + 1, re.match(r"return\b", code[k:end]) is not None


def _gate_ifs(code: str) -> list:
    """Every `if` in `code` (string-blanked) whose whole condition is one
    credential gate's call, `!gate(...)` (a refusal) or `gate(...)` (a
    grant): (if position, refusal?, gate, condition span, branch span,
    branch returns?)."""
    out = []
    gate = re.compile(r"(!\s*)?(?:(?:[A-Za-z_]\w*)?::)*(" + "|".join(
        sorted(map(re.escape, CREDENTIAL_GATES), key=len, reverse=True)) + r")\s*\(")
    for m in re.finditer(r"\bif\s*\(", code):
        open_ = m.end() - 1
        close = _close(code, open_)
        if close < 0:
            continue
        cond = code[open_ + 1:close]
        lead = len(cond) - len(cond.lstrip())
        g = gate.match(cond, lead)
        if not g:
            continue
        args_close = _close(cond, g.end() - 1)
        if args_close < 0 or cond[args_close + 1:].strip():
            continue
        start, end, returns = _returns(code, close + 1)
        out.append((m.start(), bool(g.group(1)), g.group(2), (open_ + 1, close),
                    (start, end), returns))
    return out


def _brace_depth(code: str, pos: int) -> int:
    return code.count("{", 0, pos) - code.count("}", 0, pos)


def _squash(text: str) -> str:
    return re.sub(r"\s+", "", text)


def _other_grant_ifs(handler: str, code: str, entry: dict):
    """The refusal and the grants OTHER_GRANTS names for `handler` in its
    body (string-blanked): (ends of each returning refusal at the top of the
    body, spans of each returning grant's branch, problems). An `if` on one
    of those conditions whose branch does not return is neither (a log line
    on the flag is not a grant); no returning refusal at the top, and the
    flag read or written other than as the entry allows, are problems."""
    refused, granted, problems = [], [], []
    refusal, grants = _squash(entry["refusal"]), {_squash(g) for g in entry["grants"]}
    for m in re.finditer(r"\bif\s*\(", code):
        close = _close(code, m.end() - 1)
        if close < 0:
            continue
        cond = _squash(code[m.end():close])
        if cond != refusal and cond not in grants:
            continue
        start, end, returns = _returns(code, close + 1)
        if not returns:
            continue
        if cond != refusal:
            granted.append((start, end))
        elif _brace_depth(code, m.start()) == 0:
            refused.append(end)
    if not refused:
        problems.append(f"{handler}() has no `if ({entry['refusal']}) {{ ... return ...; }}` at the top of "
                        f"its body: OTHER_GRANTS lets its answers stand after that refusal, not anywhere")
    flag = entry["flag"]
    for m in re.finditer(r"\b" + re.escape(flag) + r"\b", code):
        before, after = code[:m.start()].rstrip(), code[m.end():].lstrip()
        if (re.search(r"(?:!|&&|\|\||==|!=)$", before) or re.match(r"(?:&&|\|\||\?|==|!=)", after)) \
                and not re.search(r"(?<!&)&$", before):
            continue  # an operand of !, &&, ||, ?: or a comparison: a read
        if re.search(r"\bif\s*\($", before) and after.startswith(")"):
            continue  # if (flag)
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
            lineno = code.count("\n", 0, m.start()) + 1
            problems.append(f"{handler}() names {flag} (body line {lineno}) other than as a condition or in "
                            f"{' / '.join(repr(w) for w in entry['writes'])}: OTHER_GRANTS trusts that flag "
                            f"to say a grant was given, so only the grant may set it")
    return refused, granted, problems


def _is_declaration(code: str, start: int, end: int) -> bool:
    """Whether the name at code[start:end] (string-blanked, outside every
    function) is declared there and nothing more: a type before it, its
    parameters after it, then attributes or qualifiers and a `;`."""
    before = code[max(code.rfind(c, 0, start) for c in ";{}") + 1:start]
    before = re.split(r"(?m)^[ \t]*#[^\n]*\n", before)[-1]  # an #include or #if line above it
    before = re.sub(r'\bextern\s*"\s*"', "extern", before)  # extern "C" (its string blanked)
    if not re.fullmatch(r"[\s\w:\*&<>,]*", before) or re.search(r"\breturn\b", before):
        return False
    rest = code[end:]
    p = re.match(r"\s*\(", rest)
    if not p:
        return False
    close = _close(rest, p.end() - 1)
    return close >= 0 and re.match(r"[^;{}=]*;", rest[close + 1:]) is not None


def _readable_spans(text: str) -> tuple:
    """(the string-blanked view, the spans of every function the check reads
    at any indentation, the spans of every #define, the spans of every raw
    string literal) of `text` (comment-stripped)."""
    blanked = blank_string_contents(text)
    funcs = [span for _n, _h, _b, span in functions(text, indented=True)]
    defines = [(m.start(), m.end()) for m in _DEFINE.finditer(blanked)]
    raws = [(m.start(), m.end()) for m in _RAW_STRING.finditer(text)]
    return blanked, funcs, defines, raws


def _inside(pos: int, spans) -> bool:
    return any(a <= pos < b for a, b in spans)


def gate_outside_problems(sources: dict, any_session: set) -> list:
    """A gate in the any-session class named outside every function the
    check reads and every #define (a lambda or a pointer at file scope, an
    initializer, a function head it cannot parse), other than in a
    declaration. The class is derived from functions and macros, so a gate
    named anywhere else would reach an answer unseen."""
    problems = []
    rx = re.compile(r"\b(" + "|".join(sorted(map(re.escape, any_session), key=len, reverse=True)) + r")\b")
    for fname, text in sorted(sources.items()):
        blanked, funcs, defines, raws = _readable_spans(text)
        for m in rx.finditer(blanked):
            if _inside(m.start(), funcs) or _inside(m.start(), defines) or _inside(m.start(), raws):
                continue
            if _is_declaration(blanked, m.start(), m.end()):
                continue
            lineno = blanked.count("\n", 0, m.start()) + 1
            line = " ".join(text.splitlines()[lineno - 1].split())
            problems.append(f"{fname}:{lineno}: {m.group(1)}, counted with the gates a landing-page "
                            f"session passes ({class_reason(m.group(1), sources)}), named outside every "
                            f"function this check reads ({line[:90]!r}): a lambda or a pointer at file "
                            f"scope, an initializer or a function head it cannot parse hides what calls "
                            f"it; write a plain function, or move an initializer into the function that "
                            f"uses it")
    return problems


def gate_home_problems(sources: dict) -> list:
    """Each credential gate is defined once, in its home file
    (CREDENTIAL_GATE_HOMES), and named elsewhere only as a call (bare, `::`
    or `csi_integration::`), a `name &&` test of a weak symbol inside a
    function, or a declaration outside every function."""
    problems = []
    defined = {g: [] for g in CREDENTIAL_GATE_HOMES}
    rx = re.compile(r"\b(" + "|".join(sorted(map(re.escape, CREDENTIAL_GATE_HOMES), key=len, reverse=True))
                    + r")\b")
    for fname, text in sorted(sources.items()):
        blanked, _funcs, _defines, raws = _readable_spans(text)
        funcs = functions(text, indented=True)
        heads = {}
        for name, head, _body, span in funcs:
            if name in CREDENTIAL_GATE_HOMES:
                defined[name].append(fname)
                heads[span[0] + head.rindex(name)] = name
        for m in rx.finditer(blanked):
            if m.start() in heads or _inside(m.start(), raws):
                continue
            lineno = blanked.count("\n", 0, m.start()) + 1
            in_func = _inside(m.start(), [s for *_x, s in funcs])
            qual = re.search(r"((?:[A-Za-z_]\w*)?::\s*)*$", blanked[:m.start()]).group(0)
            call = re.match(r"\s*\(", blanked[m.end():]) is not None
            weak = re.match(r"\s*&&", blanked[m.end():]) is not None and not qual
            ok_qual = re.sub(r"\s", "", qual) in ("", "::", "csi_integration::")
            if in_func and ((call and ok_qual) or weak):
                continue
            if not in_func and not re.search(r"#[ \t]*define[^\n]*$", blanked[:m.start()]) \
                    and _is_declaration(blanked, m.start(), m.end()):
                continue
            problems.append(f"{fname}:{lineno}: {m.group(1)} named other than as a call, a `{m.group(1)} &&` "
                            f"test or a declaration (a qualifier other than csi_integration::, a macro, "
                            f"a local or a definition by that name): a credential gate is the one "
                            f"{CREDENTIAL_GATE_HOMES[m.group(1)]} defines")
    for gate, files in sorted(defined.items()):
        if files != [CREDENTIAL_GATE_HOMES[gate]]:
            problems.append(f"{gate}() is defined in {', '.join(files) or 'no file the check reads'}; a "
                            f"credential gate is defined once, in {CREDENTIAL_GATE_HOMES[gate]}")
    return problems


def _secrets_in(code: str, self_name: str = "") -> set:
    """The secrets `code` (string-blanked) reads, the comparers removed."""
    code = _COMPARER.sub("", code)
    found = {s for s, rx in _SECRET_READS.items() if rx.search(code)}
    if _WHOLE_DEVICE.search(code):
        found |= set(SECRETS)
    for name, (kind, _reads, _why, gives) in CREDENTIAL_READERS.items():
        if kind == "hands" and name != self_name and re.search(r"\b" + re.escape(name) + r"\s*\(", code):
            found |= gives
    return found


def _names(secrets) -> str:
    return ", ".join(SECRETS[s] for s in sorted(secrets))


def credential_problems(ino: str, handlers: set, sources: dict = None) -> list:
    """The F289 rule over canary_wap.ino (comment-stripped), the names of the
    handlers it registers and every sketch source (name -> comment-stripped
    text; the any-session class is derived from them, from `ino` alone when
    None). Returns the problems, empty when it holds. Where the credential
    gates are defined and where an any-session gate may be named are
    gate_home_problems()' and gate_outside_problems()'."""
    sources = sources if sources is not None else {"canary_wap.ino": ino}
    any_session = any_session_class(sources)
    problems = []
    funcs = functions(ino)
    bodies = {}
    for name, head, body, _span in funcs:
        bodies.setdefault(name, []).append(body)
    readers = {}
    for name, head, body, _span in funcs:
        got = _secrets_in(blank_string_contents(body), name)
        if got:
            readers[name] = readers.get(name, set()) | got
    defined = {name for name, _h, _b, _s in funcs}
    for name in sorted(set(readers) - set(CREDENTIAL_READERS)):
        problems.append(f"{name}() reads {_names(readers[name])} and is not in CREDENTIAL_READERS "
                        f"(name it as an answer, a hand-out or a local use, with what it reads)")
    for name, (kind, reads, _why, gives) in sorted(CREDENTIAL_READERS.items()):
        if kind == "hands":
            if name not in defined:
                problems.append(f"CREDENTIAL_READERS names {name}(), which canary_wap.ino does not "
                                f"define — remove or fix it")
            elif not gives:
                problems.append(f"CREDENTIAL_READERS names {name}() as a hand-out of nothing")
        elif name not in readers:
            problems.append(f"CREDENTIAL_READERS names {name}(), which reads none of those secrets — "
                            f"remove or fix it")
        extra = readers.get(name, set()) - reads
        if extra:
            problems.append(f"{name}() reads {_names(extra)}, which its CREDENTIAL_READERS entry "
                            f"does not let it read ({_names(reads) or 'nothing'})")
        if name in handlers and kind != "answer":
            problems.append(f"{name}() is a registered route handler and reads a secret, so it "
                            f"is an answer, not a {'hand-out' if kind == 'hands' else 'local use'} "
                            f"(CREDENTIAL_READERS)")
    # A local use with a buffer it could hand a secret out through is a
    # hand-out until shown otherwise: its callers would read the secret too.
    for name, head, _body, _span in funcs:
        entry = CREDENTIAL_READERS.get(name)
        if not entry or entry[0] != "local" or not (readers.get(name, set()) & {"token", "ap", "key", "tls"}):
            continue
        sig = re.match(r"(.*?)\b" + re.escape(name) + r"\s*\((.*)\)", head, re.S)
        ret, params = sig.group(1), sig.group(2)
        writable = [p.strip() for p in params.split(",")
                    if _BUFFER_PARAM.search(p) and not p.strip().startswith("const")]
        if writable or re.search(r"\bchar\s*\*", ret):
            problems.append(f"{name}() reads a secret and could hand it to its caller "
                            f"({', '.join(writable) or 'it returns a char pointer'}): name it "
                            f"\"hands\" in CREDENTIAL_READERS so its callers are read too")
    # Nothing outside a function names a secret but the declarations.
    blanked = blank_string_contents(ino)
    spans = [span for _n, _h, _b, span in funcs]
    for rx in list(_SECRET_READS.values()) + [_WHOLE_DEVICE] + [
            re.compile(r"\b" + re.escape(n) + r"\s*\(") for n, e in CREDENTIAL_READERS.items()
            if e[0] == "hands"]:
        for m in rx.finditer(blanked):
            if any(a <= m.start() < b for a, b in spans):
                continue
            lineno = blanked.count("\n", 0, m.start()) + 1
            line = " ".join(ino.splitlines()[lineno - 1].split())
            if line not in OUTSIDE_FUNCTION_LINES:
                problems.append(f"canary_wap.ino:{lineno}: a secret named outside every function "
                                f"({line!r}): the check reads secrets per function "
                                f"(OUTSIDE_FUNCTION_LINES)")

    answers = {n for n, e in CREDENTIAL_READERS.items() if e[0] == "answer"}
    for name in sorted(ANSWER_CALLS & any_session):
        problems.append(f"ANSWER_CALLS names {name}(), and {class_reason(name, sources)}: ANSWER_CALLS "
                        f"holds names by spelling, so every helper of that name in the sketch is taken "
                        f"for it; rename the helper, or take {name} off ANSWER_CALLS")

    # A macro that names a secret (`#define TOKF api_token_str`, then
    # `g_device.TOKF`) hides the read from every rule above, in whichever
    # sketch file it is defined.
    for fname, text in sorted(sources.items()):
        source = blank_string_contents(text)
        for m in _DEFINE.finditer(source):
            if re.search(r"\b(?:api_token_str|ap_password|privkey|g_tls_key_der|g_device)\b", m.group(2)):
                lineno = source.count("\n", 0, m.start()) + 1
                problems.append(f"{fname}:{lineno}: the macro {m.group(1)} names a secret: the check "
                                f"reads secrets by their names, so a macro would carry one past it")

    def answer_code(fn: str, code: str) -> list:
        """What `code` (fn's body, string-blanked) names that a landing
        session could pass, calls that ANSWER_CALLS does not name, or holds
        that this text check cannot read as one path."""
        out = []
        took = sorted(_identifiers(code) & any_session)
        for t in took:
            out.append(f"{fn}() answers a credential and names {t}, which this check counts with the "
                       f"gates a landing-page session or the landing's pair token passes "
                       f"({class_reason(t, sources)}): names are matched by spelling, a local or a "
                       f"member included, so rename one that is not that gate")
        unknown = sorted(_strict_calls(code) - ANSWER_CALLS - set(CREDENTIAL_GATES) - answers
                         - any_session)
        if unknown:
            out.append(f"{fn}() answers a credential and calls {', '.join(u + '()' for u in unknown)}, "
                       f"which ANSWER_CALLS does not name (name it there once it is shown to decide "
                       f"no access)")
        if re.search(r"^[ \t]*#", code, re.M):
            out.append(f"{fn}() answers a credential and holds a preprocessor line: the check "
                       f"reads one text of its body, not each build's; move the #if out of the "
                       f"answer, into a helper named on ANSWER_CALLS")
        if re.search(r"\bgoto\b", code):
            out.append(f"{fn}() answers a credential and holds a goto, which can jump past its gate")
        return out

    def answer_sites(handler: str, answer: str, code: str) -> list:
        """Where `handler`'s body (string-blanked) answers: each secret it
        reads when it is the answer, each call of `answer` otherwise."""
        if handler != answer:
            return [m.start() for m in re.finditer(r"\b" + re.escape(answer) + r"\s*\(", code)]
        compared = [(m.start(), m.end()) for m in _COMPARER.finditer(code)]
        rxs = list(_SECRET_READS.values()) + [_WHOLE_DEVICE] + [
            re.compile(r"\b" + re.escape(n) + r"\s*\(") for n, e in CREDENTIAL_READERS.items()
            if e[0] == "hands"]
        return sorted(m.start() for rx in rxs for m in rx.finditer(code) if not _inside(m.start(), compared))

    def gated_for_credentials(handler: str, answer: str) -> list:
        found = bodies.get(handler, [])
        if len(found) != 1:
            return [f"{handler}() defined {len(found)} times"]
        # Gate names count in code only: a log line that names a gate is not one.
        code = blank_string_contents(found[0])
        ifs = _gate_ifs(code)
        out = []
        if not ifs:
            out.append(f"{handler}() answers a credential behind no `if (!{CREDENTIAL_GATES[0]}(...)) "
                       f"return ...;` or `if ({CREDENTIAL_GATES[1]}(...)) {{ ... return ...; }}`")
        conds = [cond for _p, _r, _g, cond, _b, _ret in ifs]
        for g in _calls(CREDENTIAL_GATES).finditer(code):
            if not _inside(g.start(), [(a, b) for a, b in conds]):
                out.append(f"{handler}() calls {g.group(1)}() other than as the whole condition of an "
                           f"`if` (a stored result, a second term, a call outside an if): its answer "
                           f"is not behind it")
        for _pos, _refusal, gate, _cond, _branch, returns in ifs:
            if not returns:
                out.append(f"{handler}()'s `if` on {gate}() does not return in its branch (a refusal "
                           f"that falls through, or a grant that goes on): end the branch with a return")
        refused = [branch[1] for pos, refusal, _g, _c, branch, returns in ifs
                   if refusal and returns and _brace_depth(code, pos) == 0]
        granted = [branch for _p, refusal, _g, _c, branch, returns in ifs if not refusal and returns]
        if handler in OTHER_GRANTS:
            other_refused, other_granted, other_problems = _other_grant_ifs(handler, code, OTHER_GRANTS[handler])
            refused, granted = refused + other_refused, granted + other_granted
            out.extend(other_problems)
        for site in answer_sites(handler, answer, code):
            if not any(site >= end for end in refused) and not _inside(site, granted):
                lineno = found[0].count("\n", 0, site) + 1
                out.append(f"{handler}() answers a credential (body line {lineno}) before any "
                           f"refusal at the top of its body and outside every grant's branch")
        return out + answer_code(handler, code)

    for name, (kind, _reads, _why, _gives) in sorted(CREDENTIAL_READERS.items()):
        if kind != "answer" or name not in readers:
            continue
        if name in handlers:
            problems.extend(gated_for_credentials(name, name))
            continue
        for body in bodies.get(name, []):
            problems.extend(answer_code(name, blank_string_contents(body)))
        call = re.compile(r"\b" + re.escape(name) + r"\s*\(")
        callers = sorted({fn for fn, _head, body, _span in funcs
                          if fn != name and call.search(blank_string_contents(body))})
        if not callers:
            problems.append(f"{name}() answers a credential and nothing calls it")
        for fn in callers:
            if fn not in handlers:
                problems.append(f"{name}() answers a credential and is called from {fn}(), "
                                f"not a registered route handler")
                continue
            problems.extend(gated_for_credentials(fn, name))
    return problems


def token_copy_problems(sources: dict) -> list:
    """Every line outside canary_wap.ino that names a module's copy of the
    token has a TOKEN_COPY_LINES shape, and each `tok` read from one meets
    only _TOK_USES, in a function at any indentation. `sources` maps file
    names to comment-stripped text."""
    problems = []
    allowed = [re.compile(r) for r in TOKEN_COPY_LINES]
    for fname, text in sorted(sources.items()):
        if fname == "canary_wap.ino":
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            if not TOKEN_COPY.search(line):
                continue
            shape = " ".join(line.strip().rstrip("\\").split())
            if not any(a.fullmatch(shape) for a in allowed):
                problems.append(f"{fname}:{lineno}: a new use of the module's copy of the token "
                                f"({shape!r}): it may only be stored, null-tested or compared "
                                f"(TOKEN_COPY_LINES)")
        for name, _head, body, _span in functions(text, indented=True):
            if not re.search(r"const char\* tok = (?:auth_token_storage|s_api_token_provider)", body):
                continue
            rest = _TOK_USES.sub("", blank_string_contents(body))
            if re.search(r"\btok\b", rest):
                problems.append(f"{fname}: {name}() reads the token into `tok` and uses it other "
                                f"than in an auth check or a null test")
    return problems


_MQTT_GATE = re.compile(
    r"if\s*\(\s*moves_the_kept_password\s*\(\s*c\s*,\s*password_sent\s*,\s*ca_changed\s*\)"
    r"\s*&&\s*!\s*authorized_for_credential\s*\(\s*req\s*,\s*tok\s*\)\s*\)\s*\{")


def mqtt_password_problems(mqtt: str, sources: dict = None) -> list:
    """POST /api/mqtt/config (csi_mqtt.cpp, comment-stripped): the owner's
    gate stands before config_save() and its block returns, and
    authorized_for_credential() takes the bearer header or an owner's
    session and calls no gate in the any-session class (derived from
    `sources`, from `mqtt` alone when None)."""
    any_session = any_session_class(sources if sources is not None else {"csi_mqtt.cpp": mqtt})
    problems = []
    post = function_body(mqtt, "handle_config_post")
    if post is None:
        return ["csi_mqtt.cpp: handle_config_post() not found"]
    code = blank_string_contents(post)
    gate = _MQTT_GATE.search(code)
    save = code.find("config_save(")
    if gate is None:
        problems.append("csi_mqtt.cpp: handle_config_post() saves a moved broker with the stored "
                        "password and no authorized_for_credential() gate")
    elif save < 0 or gate.start() > save:
        problems.append("csi_mqtt.cpp: handle_config_post() asks authorized_for_credential() "
                        "after config_save(), or never saves")
    else:
        depth, i = 1, gate.end()
        while i < len(code) and depth:
            depth += {"{": 1, "}": -1}.get(code[i], 0)
            i += 1
        if not re.search(r"\breturn\b", code[gate.end():i]):
            problems.append("csi_mqtt.cpp: handle_config_post()'s owner's gate does not return: "
                            "the refusal falls through to the save")
    owner = re.search(r"static bool authorized_for_credential\s*\([^)]*\)\s*\{(.*?)\n\}", mqtt, re.S)
    if owner is None:
        problems.append("csi_mqtt.cpp: authorized_for_credential() not found")
    else:
        body = blank_string_contents(owner.group(1))
        took = sorted({m.group(1) for m in _calls(any_session).finditer(body)})
        if (took or not re.search(r"\bcv_session_validate_owner\s*\(", body)
                or not re.search(r"\bapi_auth_bearer_header_ok\s*\(", body)):
            problems.append("csi_mqtt.cpp: authorized_for_credential() must take the bearer header "
                            "or an owner's session only" + (f" (it calls {', '.join(took)})" if took else ""))
    return problems


def _assert_credential_rule() -> None:
    """The F289 rule on fixtures, every run: the shapes it must pass and
    the ones it must refuse."""
    good = (
        "static DeviceIdentity g_device;\n"
        "static bool resolve_ap_password(char* out_password, size_t out_len);\n"
        "static esp_err_t send_provisioning_receipt(httpd_req_t* req, Grant g) {\n"
        "  in.token = g_device.api_token_str;\n  in.ap_password = g_device.ap_password;\n}\n"
        "static esp_err_t handle_provisioning_receipt(httpd_req_t* req) {\n"
        "  g_health.http_requests++;\n"
        "  if (api_auth_check_credential_optional(req, g_device.api_token_str)) {\n"
        "    return send_provisioning_receipt(req, Grant::BEARER);\n  }\n"
        "  bool claim_served = false;\n"
        "  {\n"
        "    if (httpd_query_key_value(qs, \"claim\", presented, sizeof(presented)) == ESP_OK) {\n"
        "      claim_served = canary::net::claim_ticket::take(g_improv_claim, presented, millis());\n"
        "      if (!claim_served) log_health(2, 7, \"claim spent\", nullptr);\n"
        "    }\n"
        "  }\n"
        "  if (!claim_served && !provisioning_gate_take()) {\n"
        "    httpd_resp_set_status(req, \"403 Forbidden\");\n"
        "    return httpd_resp_sendstr(req, body);\n  }\n"
        "  csi_integration::session_issue(hex, sizeof(hex), cv_session::Origin::OWNER);\n"
        "  return send_provisioning_receipt(req, claim_served ? Grant::SETUP_DOOR_CLAIM : Grant::BOOT_TAP);\n"
        "}\n"
        "static esp_err_t handle_pairing_qr(httpd_req_t* req) {\n"
        "  if (!api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n"
        "  snprintf(p, n, \"%s\", g_device.api_token_str);\n}\n"
        "static bool qr_scan_request_allowed(httpd_req_t* req) {\n"
        "  if (csi_integration::pair_token_valid(tok)) return true;\n"
        "  return api_auth_check_optional(req, g_device.api_token_str);\n}\n"
        "static WifiChangeAuth my_wifi_gate(httpd_req_t* req) {\n"
        "  return api_auth_check(req, g_device.api_token_str) ? A : B;\n}\n"
        "static esp_err_t handle_status_auth(httpd_req_t* req) {\n"
        "  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;\n}\n"
        "static bool resolve_ap_password(char* o, size_t n) {\n"
        "  derive_ap_password(g_device.privkey, g_device.ap_password, 64);\n"
        "  snprintf(o, n, \"%s\", g_device.ap_password);\n}\n"
        "static void wifi_raise_ap() { char p[72]; if (!resolve_ap_password(p, sizeof(p))) return; }\n"
        "static void wifi_init_provisioning() { char p[72]; resolve_ap_password(p, sizeof(p)); }\n"
        "static void provision_device() {\n"
        "  derive_api_token(g_device.privkey, g_device.api_token_str, 40);\n"
        "  x(g_device.api_token_str, g_device.ap_password);\n}\n"
        "static void print_quick_connect_details(const char* title) { p(g_device.api_token_str); }\n"
        "static void handle_serial_commands() { p(g_device.ap_password); }\n"
        "void setup() {\n  p(g_device.ap_password); mesh(g_device.privkey);\n"
        "  bluetooth_api::register_routes(active_server, g_device.api_token_str);\n}\n"
        "static bool derive_api_token(const uint8_t k[32], char* o, size_t n) { return true; }\n"
        "static void derive_ap_password(const uint8_t k[32], char* o, size_t n) {}\n"
        "void loop() { datamgmt::process(g_device.privkey); }\n"
        "static void register_api_routes(httpd_handle_t s) { device_signature::init(g_device.privkey); }\n"
        "static bool create_witness_record(const uint8_t* p, size_t n, RecordType t, WitnessRecord* out) {\n"
        "  sign_message(g_device.privkey);\n}\n"
        "static bool tls_load_from_nvs() { return load(&g_tls_key_der); }\n"
        "static bool tls_store_to_nvs() { return store(g_tls_key_der); }\n"
        "static bool tls_generate_self_signed_cert() { return make(&g_tls_key_der); }\n"
        "static void start_http_server() { c.prvtkey_pem = g_tls_key_der; }\n"
    )
    handlers = {"handle_provisioning_receipt", "handle_pairing_qr", "handle_status_auth"}
    assert credential_problems(good, handlers) == [], credential_problems(good, handlers)
    # A space before a gate's parenthesis is still the gate.
    spaced = good.replace("api_auth_check_credential(req", "api_auth_check_credential (req")
    assert credential_problems(spaced, handlers) == [], credential_problems(spaced, handlers)
    qr_gate = "  if (!api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n"

    def beside(x: str) -> str:
        return good.replace(qr_gate, (
            f"  if (!{x} &&\n      !api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n"))

    status = "static esp_err_t handle_status_auth(httpd_req_t* req) {\n" \
             "  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;\n}\n"

    def status_answers(line: str) -> str:
        return good.replace(status, status.replace("}\n", line + "}\n"))

    bad = {
        "the receipt's old first grant": good.replace(
            "api_auth_check_credential_optional(req", "api_auth_check_optional(req"),
        "the pairing QR's old gate": good.replace(
            "api_auth_check_credential(req", "api_auth_check(req"),
        "a credential gate beside a session gate": good.replace(
            "  return send_provisioning_receipt(req, claim_served ? Grant::SETUP_DOOR_CLAIM : Grant::BOOT_TAP);",
            "  if (cv_session_validate(req)) return send_provisioning_receipt(req, Grant::BEARER);"),
        "a new answer nobody classified": good + (
            "static esp_err_t handle_leak(httpd_req_t* req) {\n"
            "  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;\n"
            "  return httpd_resp_sendstr(req, g_device.api_token_str);\n}\n"),
        "the receipt written from a second route": good + (
            "static esp_err_t handle_other(httpd_req_t* req) {\n"
            "  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;\n"
            "  return send_provisioning_receipt(req, Grant::BEARER);\n}\n"),
        # The first review's plants: a gate that takes the landing's pair
        # token or any session beside the credential gate, and a credential
        # gate named only in a string literal.
        "the landing's pair token beside the credential gate": beside("csi_integration::pair_token_valid(\"x\")"),
        "wifi_change_authorize beside the credential gate": beside("wifi_change_authorize(req)"),
        "a credential gate named only in a string": good.replace(qr_gate, (
            "  Serial.println(\"api_auth_check_credential(\");\n"
            "  if (!csi_integration::pair_token_valid(t)) return ESP_OK;\n")),
        "the SoftAP password derived in a new answer": good + (
            "static esp_err_t handle_status_ap(httpd_req_t* req) {\n"
            "  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;\n"
            "  derive_ap_password(g_device.privkey, buf, sizeof(buf));\n"
            "  return httpd_resp_sendstr(req, buf);\n}\n"),
        # The second review's plants (PL5-PL7, PL16): a space before the
        # parenthesis, the sketch's own pair-token-or-session gate, the
        # landing exchange, and a wrapper the seeds do not name.
        "session_validate_cookie (req), a space before the parenthesis":
            beside("csi_integration::session_validate_cookie (req)"),
        "cv_session_validate (req), a space before the parenthesis": beside("cv_session_validate (req)"),
        "qr_scan_request_allowed beside the credential gate": beside("qr_scan_request_allowed(req)"),
        "pair_token_consume beside the credential gate": beside("csi_integration::pair_token_consume(\"x\")"),
        "pair_token_issue beside the credential gate": beside("csi_integration::pair_token_issue(h, 65)"),
        "a wrapper of api_auth_check beside the credential gate": beside("my_wifi_gate(req) == A"),
        "an unclassified request check beside the credential gate": good.replace(qr_gate, (
            "  if (!request_from_lan(req) &&\n"
            "      !api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n")) + (
            "static bool request_from_lan(httpd_req_t* req) { return peer_is_lan(req); }\n"),
        "an unclassified request check inside the answer itself": good.replace(
            "  in.token = g_device.api_token_str;\n",
            "  if (request_from_lan(req)) in.token = g_device.api_token_str;\n") + (
            "static bool request_from_lan(httpd_req_t* req) { return peer_is_lan(req); }\n"),
        "an any-session gate inside the answer itself": good.replace(
            "  in.token = g_device.api_token_str;\n",
            "  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;\n"
            "  in.token = g_device.api_token_str;\n"),
        # PL4: the gate called, its result ignored.
        "the credential gate's result ignored": good.replace(
            qr_gate, "  (void)api_auth_check_credential(req, g_device.api_token_str);\n"),
        # PL1: a lambda route in register_api_routes, a local use of the key only.
        "a token read in register_api_routes (a lambda route)": good.replace(
            "device_signature::init(g_device.privkey); }",
            "device_signature::init(g_device.privkey);\n"
            "  h = [](httpd_req_t* r) -> esp_err_t { return httpd_resp_sendstr(r, g_device.ap_password); }; }"),
        # PL8: a handler answering resolve_ap_password()'s out-param.
        "a status handler answering resolve_ap_password()'s buffer": status_answers(
            "  char p[72];\n  if (resolve_ap_password(p, sizeof(p))) return httpd_resp_sendstr(req, p);\n"),
        # PL2, PL3: the secret through another spelling of the member.
        "g_device .api_token_str (a space before the dot)": status_answers(
            "  return httpd_resp_sendstr(req, g_device .api_token_str);\n"),
        "(&g_device)->ap_password": status_answers(
            "  return httpd_resp_sendstr(req, (&g_device)->ap_password);\n"),
        "the whole g_device handed on": status_answers("  dump_identity(&g_device);\n"),
        "a local use that reads a secret its entry does not name": good.replace(
            "void loop() { datamgmt::process(g_device.privkey); }",
            "void loop() { datamgmt::process(g_device.privkey); log(g_device.api_token_str); }"),
        "a local use with an out-buffer": good.replace(
            "static void handle_serial_commands() {", "static void handle_serial_commands(char* out) {"),
        "a secret named outside every function": good + "#define LEAK_TOKEN g_device.api_token_str\n",
        # The third review's N9: a member macro.
        "a macro that names the token's member": status_answers(
            "  return httpd_resp_sendstr(req, g_device.TOKF);\n") + "#define TOKF api_token_str\n",
        # The fourth review's V36 and V6: a macro naming the SoftAP password's
        # member, and a second route that calls the receipt's writer before
        # its refusal (nothing else in it fails).
        "a macro that names the SoftAP password's member": status_answers(
            "  return httpd_resp_sendstr(req, g_device.V4AP);\n") + "#define V4AP ap_password\n",
        "a macro that names the whole g_device": status_answers("  dump_identity(&DEV);\n") + "#define DEV g_device\n",
        "a second route calling the receipt's writer before its refusal": good + (
            "static esp_err_t handle_kit2(httpd_req_t* req) {\n"
            "  if (WiFi.isConnected()) return send_provisioning_receipt(req, Grant::BEARER);\n"
            "  if (!api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n"
            "  return send_provisioning_receipt(req, Grant::BEARER);\n}\n"),
    }
    handlers_bad = handlers | {"handle_leak", "handle_other", "handle_status_ap", "handle_kit2"}
    for why, text in bad.items():
        assert credential_problems(text, handlers_bad), f"credential rule passed {why}"
    # A route handler listed as a local use or a hand-out is still a route handler.
    assert credential_problems(good, handlers | {"handle_serial_commands"}), \
        "credential rule passed a route handler named as a local use"
    assert credential_problems(good, handlers | {"resolve_ap_password"}), \
        "credential rule passed a route handler named as a hand-out"
    # The any-session class reaches a wrapper in another file, and a call
    # ANSWER_CALLS names passes.
    elsewhere = {"canary_wap.ino": beside("other::gate(req)"),
                 "other.h": "namespace other {\ninline int gate(httpd_req_t* r) {\n"
                            "  return session_validate_cookie(r) ? 1 : 0;\n}\n}\n"}
    assert credential_problems(elsewhere["canary_wap.ino"], handlers, elsewhere), \
        "credential rule passed a wrapper of an any-session gate defined in another file"
    named = good.replace(qr_gate, qr_gate + "  send_qr_svg(req, p);\n")
    assert credential_problems(named, handlers) == [], credential_problems(named, handlers)
    # The fourth review's V17, V19, V20 and its C4: the class read from an
    # indented namespaced wrapper, to a fixed point down a chain, from a
    # macro that names a gate without calling it; a member macro in a header.
    other_files = {
        "V17 an indented namespaced wrapper named like an ANSWER_CALLS entry": (good, {
            "other.h": "namespace v4p {\n  inline bool take(httpd_req_t* r) { return session_validate_cookie(r); }\n}\n"}),
        "V19 a three-call chain to an any-session gate under an ANSWER_CALLS name": (good, {
            "other.h": "inline bool lvl1(httpd_req_t* r) { return session_validate_cookie(r); }\n"
                       "inline bool lvl2(httpd_req_t* r) { return lvl1(r); }\n"
                       "inline bool take(httpd_req_t* r) { return lvl2(r); }\n"}),
        "V20 a macro that names an any-session gate, called from an ANSWER_CALLS function": (good + (
            "#define V4_ANY_GATE api_auth_check_optional\n"
            "static bool provisioning_gate_take() {\n  return V4_ANY_GATE(g_req, g_tok);\n}\n"), {}),
        "C4 a header macro naming the token's member, read in an answer": (status_answers(
            "  return httpd_resp_sendstr(req, g_device.TOKF);\n"), {"api_auth.h": "#define TOKF api_token_str\n"}),
        "a header macro naming the whole g_device, handed on by a handler": (status_answers(
            "  dump_identity(&DEV);\n"), {"api_auth.h": "#define DEV g_device\n"}),
    }
    for why, (text, extra) in other_files.items():
        assert credential_problems(text, handlers, {"canary_wap.ino": text, **extra}), \
            f"credential rule passed {why}"

    # The third review's plants (N1-N8, N12, N13, N21, N22 and the heads it
    # could not parse), and one fixture each rule alone refuses.
    def qr(gate_line: str, extra: str = "") -> str:
        return good.replace(qr_gate, gate_line) + extra

    either = "  if (!{x} &&\n      !api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n"
    wrapper = "  return api_auth_check_optional(r, t);\n}\n"
    shapes = {
        "N1 an any-session gate taken by address into a local pointer": qr(
            "  bool (*any_gate)(httpd_req_t*, const char*) = api_auth_check_optional;\n"
            + either.format(x="any_gate(req, g_device.api_token_str)")),
        "N1b auto g = &api_auth_check_optional": qr(
            "  auto g = &api_auth_check_optional;\n" + either.format(x="g(req, t)")),
        "N2 a function-like macro over an any-session gate":
            "#define ANY_OK(r, t) api_auth_check_optional((r), (t))\n" + qr(either.format(x="ANY_OK(req, t)")),
        "N3 a file-scope lambda over an any-session gate": qr(either.format(x="s_any_ok(req, t)"), (
            "static auto s_any_ok = [](httpd_req_t* r, const char* t) { return api_auth_check_optional(r, t); };\n")),
        "N4 a wrapper whose head carries __attribute__((noinline))": qr(either.format(x="any_ok(req, t)"), (
            "static bool __attribute__((noinline)) any_ok(httpd_req_t* r, const char* t) {\n" + wrapper)),
        "N4b a wrapper with a trailing return type": qr(either.format(x="any_ok(req, t)"), (
            "static auto any_ok(httpd_req_t* r, const char* t) -> bool {\n" + wrapper)),
        "N4c a noexcept int wrapper": qr(either.format(x="any_ok(req, t)"), (
            "static int any_ok(httpd_req_t* r, const char* t) noexcept {\n" + wrapper)),
        "N4d a [[nodiscard]] wrapper": qr(either.format(x="any_ok(req, t)"), (
            "[[nodiscard]] static bool any_ok(httpd_req_t* r, const char* t) {\n" + wrapper)),
        "N6 a refusal whose block does not return": qr(
            "  if (!api_auth_check_credential(req, g_device.api_token_str)) { }\n"),
        "N7 a grant whose branch only logs": qr(
            "  if (api_auth_check_credential(req, g_device.api_token_str)) Serial.println(\"ok\");\n"),
        "N8 the refusal's condition with a second term": qr(
            "  if (!api_auth_check_credential(req, g_device.api_token_str) && false) return ESP_OK;\n"),
        "N12 an any-session gate handed to a template helper": qr(
            either.format(x="call_gate(api_auth_check_optional, req, t)"),
            "template <typename F> static bool call_gate(F f, httpd_req_t* r, const char* t) { return f(r, t); }\n"),
        "N13 the refusal inside a branch of its own": qr(
            "  if (WiFi.isConnected()) {\n"
            "    if (!api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n  }\n"),
        "N21 a refusal by goto": qr(
            "  if (!api_auth_check_credential(req, g_device.api_token_str)) goto out;\n  out:\n"),
        "N22 an any-session gate in a struct's pointer member": qr(either.format(x="g_gates.any(req, t)"), (
            "struct Gates { bool (*any)(httpd_req_t*, const char*); } g_gates = { api_auth_check_optional };\n")),
        # The fourth review's V2, V5 and V12, each with nothing else in it
        # that fails: a plain leading term, a refusal block whose only return
        # is conditional, a template call nothing classified.
        "V2 a plain term before the refusal's gate": qr(
            "  if (!g_tls_enabled && !api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n"),
        "V5 a refusal block whose only return is conditional": qr(
            "  if (!api_auth_check_credential(req, g_device.api_token_str)) {\n"
            "    if (g_tls_enabled) return ESP_OK;\n    g_health.http_requests++;\n  }\n"),
        "V12 a template call in the answer": qr(
            qr_gate + "  if (!lan_peer<0>(req)) return ESP_OK;\n",
            "template <int N> static bool lan_peer(httpd_req_t* r) { return r != nullptr; }\n"),
        # Each rule alone. The receipt's answers after its grant stand behind
        # OTHER_GRANTS, so only the returning branch holds these two.
        "the receipt's grant whose block goes on": good.replace(
            "    return send_provisioning_receipt(req, Grant::BEARER);\n  }\n",
            "    send_provisioning_receipt(req, Grant::BEARER);\n  }\n"),
        "the receipt's grant whose branch only logs": good.replace(
            "  if (api_auth_check_credential_optional(req, g_device.api_token_str)) {\n"
            "    return send_provisioning_receipt(req, Grant::BEARER);\n  }\n",
            "  if (api_auth_check_credential_optional(req, g_device.api_token_str)) Serial.println(\"ok\");\n"),
        "an answer before its refusal": qr(
            "  snprintf(p, n, \"%s\", g_device.api_token_str);\n" + qr_gate),
        "a goto past the refusal": qr("  goto skip;\n" + qr_gate + "  skip:\n"),
        "a goto past the refusal from inside a branch": qr(
            "  if (WiFi.isConnected()) {\n    goto skip;\n  }\n" + qr_gate + "  skip:\n"),
        "a preprocessor branch around the refusal": qr("#if 0\n" + qr_gate + "#endif\n"),
        "an any-session gate named, not called, in the answer": qr(
            qr_gate + "  (void)api_auth_check_optional;\n"),
        "an ANSWER_CALLS name that calls an any-session gate": good + (
            "static bool provisioning_gate_take() {\n  return api_auth_check_optional(g_req, g_tok);\n}\n"),
        "an ANSWER_CALLS name defined as a macro over an any-session gate":
            "#define send_qr_svg(r, p) api_auth_check_optional((r), (p))\n" + good,
    }
    for why, text in shapes.items():
        assert credential_problems(text, handlers), f"credential rule passed {why}"

    # The receipt (OTHER_GRANTS): its BOOT-tap and claim grants are the host
    # suite's, but only after the refusal on them or inside a grant's own
    # branch. The fourth review's plant (an answer on a down station link at
    # the top of the handler) and its kin fail; the grants' branches pass.
    receipt_top = "  g_health.http_requests++;\n"
    grant1_end = "    return send_provisioning_receipt(req, Grant::BEARER);\n  }\n"
    tap_refusal = "  if (!claim_served && !provisioning_gate_take()) {\n"
    refusal_end = "    return httpd_resp_sendstr(req, body);\n  }\n"
    take_line = "      claim_served = canary::net::claim_ticket::take(g_improv_claim, presented, millis());\n"
    leak = "  if (!WiFi.isConnected()) return send_provisioning_receipt(req, Grant::BEARER);\n"
    receipt_bad = {
        "an answer at the top of the receipt on a down station link": good.replace(
            receipt_top, receipt_top + leak),
        "an answer between the receipt's credential grant and its claim": good.replace(
            grant1_end, grant1_end + leak),
        "an answer between the claim and the tap refusal": good.replace(tap_refusal, leak + tap_refusal),
        "an answer inside the claim's block": good.replace(take_line, take_line + "    " + leak),
        "the tap refusal widened": good.replace(
            tap_refusal, "  if (!claim_served && !provisioning_gate_take() && WiFi.isConnected()) {\n"),
        "the tap refusal inside a branch of its own": good.replace(
            tap_refusal, "  if (WiFi.isConnected()) {\n" + tap_refusal).replace(refusal_end, refusal_end + "  }\n"),
        "the tap refusal falls through": good.replace(refusal_end, refusal_end.replace("return ", "")),
        "the claim flag set by the link": good.replace(
            "  bool claim_served = false;\n", "  bool claim_served = !WiFi.isConnected();\n"),
        "the claim flag OR'd with the link": good.replace(
            take_line, take_line.replace("millis());", "millis()) || !WiFi.isConnected();")),
        "the claim flag written through a pointer": good.replace(
            tap_refusal, "  bool* served = &claim_served;\n  *served = true;\n" + tap_refusal),
        "the claim flag set again": good.replace(tap_refusal, "  claim_served = true;\n" + tap_refusal),
        "an answer under a condition that holds the refusal's": good.replace(tap_refusal, (
            "  if (!claim_served && !provisioning_gate_take() || !WiFi.isConnected()) {\n"
            "    return send_provisioning_receipt(req, Grant::BOOT_TAP);\n  }\n" + tap_refusal)),
    }
    for why, text in receipt_bad.items():
        assert text != good, f"receipt fixture did not apply: {why}"
        assert credential_problems(text, handlers), f"credential rule passed {why}"
    receipt_good = {
        "a BOOT-tap grant's own branch": good.replace(tap_refusal, (
            "  if (provisioning_gate_take()) {\n    return send_provisioning_receipt(req, Grant::BOOT_TAP);\n  }\n"
            + tap_refusal)),
        "a claim grant's own branch": good.replace(tap_refusal, (
            "  if (claim_served) {\n    return send_provisioning_receipt(req, Grant::SETUP_DOOR_CLAIM);\n  }\n"
            + tap_refusal)),
    }
    for why, text in receipt_good.items():
        assert text != good and credential_problems(text, handlers) == [], (why, credential_problems(text, handlers))

    # An any-session gate is named only in the functions and macros the
    # class is derived from, or in a declaration (or a raw string).
    api_home = (
        "extern bool cv_session_validate(httpd_req_t* req) __attribute__((weak));\n"
        "extern bool cv_session_validate_owner(httpd_req_t* req) __attribute__((weak));\n"
        "static bool api_auth_check_header(httpd_req_t* req, const char* t) {\n  return true;\n}\n"
        "static bool api_auth_check_credential(httpd_req_t* req, const char* t) {\n"
        "  if (cv_session_validate_owner && cv_session_validate_owner(req)) return true;\n"
        "  return api_auth_check_header(req, t);\n}\n"
        "static bool api_auth_bearer_header_ok(httpd_req_t* req, const char* t) {\n  return true;\n}\n"
        "static bool api_auth_check_credential_optional(httpd_req_t* req, const char* t) {\n"
        "  if (api_auth_bearer_header_ok(req, t)) return true;\n"
        "  return cv_session_validate_owner && cv_session_validate_owner(req);\n}\n"
        "static bool api_auth_check_optional(httpd_req_t* req, const char* t) {\n"
        "  if (cv_session_validate && cv_session_validate(req)) return true;\n"
        "  return api_auth_check_header(req, t);\n}\n")
    csi_home = (
        "#include <esp_http_server.h>\n"
        "bool cv_session_validate(httpd_req_t* req);\n"
        "namespace csi_integration {\nbool session_validate_cookie(httpd_req_t* req) {\n  return true;\n}\n"
        "bool session_validate_owner_cookie(httpd_req_t* req) {\n  return true;\n}\n}\n"
        "bool cv_session_validate_owner(httpd_req_t* req) {\n"
        "  return csi_integration::session_validate_owner_cookie(req);\n}\n"
        "#define CSI_AUTH_OR_RETURN(req) do { \\\n"
        "    if (api_auth_check_optional((req), g_api_token)) break; \\\n} while (0)\n"
        # Last: a raw string's odd quote leaves the gate's name in code to
        # blank_string_contents, and the rest of the file in a string.
        "static const char kPage[] = R\"PAGE(<a title=\"x>api_auth_check_optional(</a>)PAGE\";\n")
    homes = {"api_auth.h": api_home, "csi_integration.cpp": csi_home,
             "csi_integration.h": "namespace csi_integration {\n"
                                  "bool session_validate_owner_cookie(httpd_req_t* req);\n}\n"}

    def outside(text: str) -> list:
        sources = {**homes, "canary_wap.ino": text}
        return gate_outside_problems(sources, any_session_class(sources))

    assert outside(good) == [], outside(good)
    outside_bad = {
        "N3 a file-scope lambda": shapes["N3 a file-scope lambda over an any-session gate"],
        "N4 an attribute in the head": shapes["N4 a wrapper whose head carries __attribute__((noinline))"],
        "N4b a trailing return type": shapes["N4b a wrapper with a trailing return type"],
        "N22 a struct initializer": shapes["N22 an any-session gate in a struct's pointer member"],
        "a file-scope pointer": good + "static bool (*g_gate)(httpd_req_t*, const char*) = api_auth_check_optional;\n",
        "an ANSWER_CALLS name defined as a file-scope lambda": good + (
            "static auto send_qr_svg = [](httpd_req_t* r) { return api_auth_check_optional(r, nullptr); };\n"),
    }
    for why, text in outside_bad.items():
        assert outside(text), f"any-session rule passed {why}"

    # The credential gates live in their homes.
    def home(text: str, extra: dict = None) -> list:
        return gate_home_problems({**homes, "canary_wap.ino": text, **(extra or {})})

    assert home(good) == [], home(good)
    home_bad = {
        "N5 a credential gate's name redefined in a namespace": qr(
            "  if (!leak::api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n",
            "namespace leak {\nstatic bool api_auth_check_credential(httpd_req_t* r, const char* t) {\n"
            + wrapper + "}\n"),
        "a credential gate called through another namespace": qr(
            "  if (!leak::api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n"),
        "a macro by a credential gate's name":
            "#define api_auth_check_credential api_auth_check_optional\n" + good,
        "a local by a credential gate's name": qr(
            "  auto api_auth_check_credential = [&](httpd_req_t* r, const char* t) {\n"
            "    return api_auth_check_optional(r, t);\n  };\n" + qr_gate),
    }
    for why, text in home_bad.items():
        assert home(text), f"credential-gate home rule passed {why}"
    assert home(good, {"other.h": "inline bool api_auth_check_header(httpd_req_t* r, const char* t) {\n"
                                  "  return true;\n}\n"}), \
        "credential-gate home rule passed a gate defined in a second file"
    # The fourth review's V33 and V34, in the gate's own home: a second
    # definition (the overload C++ picks for a non-const token) and a macro.
    home_own = {
        "V33 a second definition of a credential gate in its home": api_home + (
            "static bool api_auth_check_credential(httpd_req_t* req, char* t) {\n"
            "  return api_auth_check_optional(req, t);\n}\n"),
        "V34 a macro by a credential gate's name in its home": api_home.replace(
            "static bool api_auth_check_optional(",
            "#define api_auth_check_credential api_auth_check_optional\nstatic bool api_auth_check_optional("),
    }
    for why, text in home_own.items():
        assert text != api_home and home(good, {"api_auth.h": text}), f"credential-gate home rule passed {why}"
    assert gate_home_problems({**homes, "api_auth.h": api_home.replace(
        "static bool api_auth_bearer_header_ok(", "static bool api_auth_bearer_header_ok_v2("),
        "canary_wap.ino": good}), "credential-gate home rule passed a gate missing from its home"

    # The modules' copies of the token.
    copies = {
        "csi_integration.cpp": (
            "const char*                             g_api_token          = nullptr;\n"
            "#define CSI_AUTH_OR_RETURN(req) do { \\\n"
            "    if (!g_api_token) {                                               \\\n"
            "    if (api_auth_check_optional((req), g_api_token)) break;           \\\n"
            "} while (0)\n"
            "bool init(const char* api_token) {\n  g_api_token = api_token;\n"
            "  csi_mqtt::set_api_token_provider([]() -> const char* { return g_api_token; });\n}\n"),
        "chirp_api.h": (
            "inline const char*& auth_token_storage() {\n  static const char* token = nullptr;\n"
            "  return token;\n}\n"
            "static esp_err_t chirp_auth_gated(httpd_req_t* req) {\n"
            "  const char* tok = auth_token_storage();\n"
            "  if (tok && !api_auth_check(req, tok)) return ESP_OK;\n  return Real(req);\n}\n"
            "void register_routes(httpd_handle_t s, const char* api_token) {\n"
            "  auth_token_storage() = api_token;\n}\n"),
        "csi_mqtt.cpp": (
            "const char* (*s_api_token_provider)() = nullptr;\n"
            "esp_err_t handle_test(httpd_req_t* req) {\n"
            "  const char* tok = s_api_token_provider ? s_api_token_provider() : nullptr;\n"
            "  if (!authorized(req, tok)) {\n    return ESP_OK;\n  }\n  return ESP_OK;\n}\n"
            "void set_api_token_provider(const char* (*fn)()) {\n  s_api_token_provider = fn;\n}\n"),
    }
    assert token_copy_problems(copies) == [], token_copy_problems(copies)
    copy_bad = {
        "a CSI handler answering g_api_token": ("csi_integration.cpp", copies["csi_integration.cpp"] + (
            "esp_err_t handle_x(httpd_req_t* req) {\n  CSI_AUTH_OR_RETURN(req);\n"
            "  return httpd_resp_sendstr(req, g_api_token);\n}\n")),
        "a module logging its copy": ("chirp_api.h", copies["chirp_api.h"].replace(
            "  auth_token_storage() = api_token;\n",
            "  auth_token_storage() = api_token;\n  Serial.println(auth_token_storage());\n")),
        "a tok answered": ("chirp_api.h", copies["chirp_api.h"].replace(
            "  if (tok && !api_auth_check(req, tok)) return ESP_OK;\n",
            "  if (tok && !api_auth_check(req, tok)) return ESP_OK;\n  httpd_resp_sendstr(req, tok);\n")),
        "the MQTT provider answered": ("csi_mqtt.cpp", copies["csi_mqtt.cpp"].replace(
            "  return ESP_OK;\n}\nvoid set_api",
            "  return httpd_resp_sendstr(req, s_api_token_provider());\n}\nvoid set_api")),
        # PL9: an indented struct member answering its tok.
        "an indented member answering tok": ("chirp_api.h", copies["chirp_api.h"] + (
            "struct LeakTok {\n  static esp_err_t handle(httpd_req_t* req) {\n"
            "    const char* tok = auth_token_storage();\n    return httpd_resp_sendstr(req, tok);\n"
            "  }\n};\n")),
    }
    for why, (fname, text) in copy_bad.items():
        assert token_copy_problems({**copies, fname: text}), f"token-copy rule passed {why}"

    # POST /api/mqtt/config.
    mqtt_good = (
        "static bool authorized(httpd_req_t* req, const char* api_token) {\n"
        "  return api_auth_check_optional(req, api_token);\n}\n"
        "static bool authorized_for_credential(httpd_req_t* req, const char* api_token) {\n"
        "  if (api_token && api_token[0] != '\\0' && api_auth_bearer_header_ok(req, api_token)) return true;\n"
        "  return cv_session_validate_owner && cv_session_validate_owner(req);\n}\n"
        "esp_err_t handle_config_post(httpd_req_t* req) {\n"
        "  if (!authorized(req, tok)) {\n    return ESP_OK;\n  }\n"
        "  if (moves_the_kept_password(c, password_sent, ca_changed) &&\n"
        "      !authorized_for_credential(req, tok)) {\n"
        "    httpd_resp_sendstr(req, \"{}\");\n    return ESP_OK;\n  }\n"
        "  if (!config_save(c)) {\n    return ESP_OK;\n  }\n  return ESP_OK;\n}\n")
    assert mqtt_password_problems(mqtt_good) == [], mqtt_password_problems(mqtt_good)
    mqtt_gate = ("  if (moves_the_kept_password(c, password_sent, ca_changed) &&\n"
                 "      !authorized_for_credential(req, tok)) {\n"
                 "    httpd_resp_sendstr(req, \"{}\");\n    return ESP_OK;\n  }\n")
    owner_line = "  return cv_session_validate_owner && cv_session_validate_owner(req);"
    mqtt_bad = {
        "the old handler, no owner's gate": mqtt_good.replace(mqtt_gate, ""),
        "the owner's gate after the save": mqtt_good.replace(mqtt_gate, "").replace(
            "  return ESP_OK;\n}\n", mqtt_gate + "  return ESP_OK;\n}\n"),
        "an owner's gate that takes any session": mqtt_good.replace(
            owner_line, "  return csi_integration::session_validate_cookie(req);"),
        # PL10, PL11.
        "an owner's gate that takes session_validate_cookie (req)": mqtt_good.replace(
            owner_line, "  if (csi_integration::session_validate_cookie (req)) return true;\n" + owner_line),
        "an owner's gate that takes the module's own any-session helper": mqtt_good.replace(
            owner_line, "  return cv_session_validate_owner && authorized(req, api_token);"),
        "the refusal falls through to the save": mqtt_good.replace(
            "    httpd_resp_sendstr(req, \"{}\");\n    return ESP_OK;\n  }\n  if (!config_save",
            "    httpd_resp_sendstr(req, \"{}\");\n  }\n  if (!config_save"),
    }
    for why, text in mqtt_bad.items():
        assert mqtt_password_problems(text), f"MQTT rule passed {why}"

    # main()'s rule list (rule_problems): a clean planted set passes, and
    # each plant is one only its own rule refuses, so a rule dropped from the
    # list, or the class read from canary_wap.ino alone, fails here.
    clean = {**homes, "canary_wap.ino": good, "chirp_api.h": copies["chirp_api.h"], "csi_mqtt.cpp": mqtt_good}
    assert rule_problems(clean, good, handlers) == [], rule_problems(clean, good, handlers)
    one_rule = {
        "credential: the pairing QR's old gate": {"canary_wap.ino": bad["the pairing QR's old gate"]},
        "credential, the class from every file: a header wrapper named like an ANSWER_CALLS entry": {
            "chirp_api.h": copies["chirp_api.h"] + (
                "namespace v4p {\ninline bool take(httpd_req_t* r) { return csi_integration::session_validate_cookie(r); }\n}\n")},
        "outside: N3, a file-scope lambda": {"canary_wap.ino": good + (
            "static auto s_any_ok = [](httpd_req_t* r, const char* t) { return api_auth_check_optional(r, t); };\n")},
        "home: N5, a credential gate's name redefined in a namespace": {"canary_wap.ino": good + (
            "namespace leak {\nstatic bool api_auth_check_credential(httpd_req_t* r, const char* t) {\n" + wrapper + "}\n")},
        "registration: N24, a positional initializer": {"canary_wap.ino": good + (
            "static void register_more(httpd_handle_t server) {\n"
            "  httpd_uri_t leak3 = { \"/api/leak3\", HTTP_GET, handle_leak3, nullptr };\n"
            "  httpd_register_uri_handler(server, &leak3);\n}\n")},
        "token copy: a module logging its copy": {"chirp_api.h": copy_bad["a module logging its copy"][1]},
        "MQTT: the broker save with no owner's gate": {"csi_mqtt.cpp": mqtt_bad["the old handler, no owner's gate"]},
    }
    for why, plant in one_rule.items():
        planted = {**clean, **plant}
        assert rule_problems(planted, planted["canary_wap.ino"], handlers), f"main()'s rule list passed {why}"

    # And main() runs the route table and that list (tree_problems): with
    # the three handlers registered, the clean set fails only on the
    # allowlist's entries a small set does not register, each plant's
    # problems are among its failures, and a route with no gate fails.
    routes_fn = (
        "static void register_fixture_routes(httpd_handle_t server) {\n"
        "  httpd_uri_t prov = { .uri = \"/api/provisioning-receipt\", .method = HTTP_GET, "
        ".handler = handle_provisioning_receipt };\n  httpd_register_uri_handler(server, &prov);\n"
        "  httpd_uri_t qr = { .uri = \"/api/pairing-qr\", .method = HTTP_GET, .handler = handle_pairing_qr };\n"
        "  httpd_register_uri_handler(server, &qr);\n"
        "  httpd_uri_t st = { .uri = \"/api/status\", .method = HTTP_GET, .handler = handle_status_auth };\n"
        "  httpd_register_uri_handler(server, &st);\n}\n")
    tree_clean = {**clean, "canary_wap.ino": good + routes_fn}
    stale_only = [f for f in tree_problems(tree_clean)[0] if not f.startswith("allowlist entry ")]
    assert stale_only == [], stale_only
    for why, plant in one_rule.items():
        planted = {**tree_clean, **{f: t + routes_fn if f == "canary_wap.ino" else t for f, t in plant.items()}}
        want = rule_problems(planted, planted["canary_wap.ino"], handlers)
        got = tree_problems(planted)[0]
        assert want and all(w in got for w in want), f"main()'s checks passed {why}"
    open_route = good + routes_fn.replace("}\n", (
        "  httpd_uri_t open = { .uri = \"/api/open\", .method = HTTP_GET, .handler = handle_open };\n"
        "  httpd_register_uri_handler(server, &open);\n}\n")) + (
        "static esp_err_t handle_open(httpd_req_t* req) { return httpd_resp_sendstr(req, \"{}\"); }\n")
    assert any("handle_open" in f and "NO credential gate" in f
               for f in tree_problems({**tree_clean, "canary_wap.ino": open_route})[0]), \
        "main()'s route table passed a route with no gate"


def rule_problems(sources: dict, ino: str, handlers: set) -> list:
    """Every rule past the route table, over the sketch's sources (file name
    -> comment-stripped text), canary_wap.ino's text and the handlers it
    registers: main()'s list in one place, so a fixture holds it
    (_assert_credential_rule runs it on a planted set, one plant per rule)."""
    problems = []
    problems.extend(credential_problems(ino, handlers, sources))
    problems.extend(gate_outside_problems(sources, any_session_class(sources)))
    problems.extend(gate_home_problems(sources))
    problems.extend(registration_problems(sources))
    problems.extend(token_copy_problems(sources))
    problems.extend(mqtt_password_problems(sources.get("csi_mqtt.cpp", ""), sources))
    return problems


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


@functools.lru_cache(maxsize=None)
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


_REG_CALL = re.compile(
    r'register_api_handler\s*\(\s*server\s*,\s*"([^"]+)"\s*,\s*'
    r"HTTP_(\w+)\s*,\s*([A-Za-z_][\w:<>]*)")
_UNNAMED_REG = re.compile(
    r'register_api_handler\s*\(\s*server\s*,\s*"([^"]+)"\s*,\s*'
    r"HTTP_(\w+)\s*,\s*(?=[^\s\w])")


# The module helper register_api_handler(), exactly as the modules write it
# (whitespace folded): its calls are read as routes instead of its body. A
# function of that name with any other body is held like any other.
_REG_HELPER_BODY = ("httpd_uri_t route = { .uri = uri, .method = method, .handler = handler, "
                    ".user_ctx = nullptr }; httpd_register_uri_handler(server, &route);")
# What the route table reads of an initializer: the uri a literal, the
# method an HTTP_ name, the handler a name, each once.
_ROUTE_FIELDS = {"uri": r'"[^"\\]+"', "method": r"HTTP_\w+", "handler": r"[A-Za-z_][\w:<>]*"}
_ROUTE_INIT = re.compile(r"httpd_uri_t\s+(\w+)\s*=\s*\{")


def _route_fields(text: str, blanked: str, open_: int, close: int):
    """The designated fields of the initializer whose braces sit at open_
    and close ({field: [values]}, values from `text`), or None when a piece
    is not `.field = value` (a positional initializer)."""
    fields, start, depth = {}, open_ + 1, 0
    for j in range(open_ + 1, close + 1):
        c = blanked[j] if j < close else ","
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            piece = text[start:j].strip()
            start = j + 1
            if not piece and j == close:
                continue  # a trailing comma
            m = re.fullmatch(r"\.\s*(\w+)\s*=\s*(.+)", piece, re.S)
            if not m:
                return None
            fields.setdefault(m.group(1), []).append(m.group(2).strip())
    return fields


def registration_problems(sources: dict) -> list:
    """Every route registration is one the route table reads, read per
    function: every httpd_uri_t is an `httpd_uri_t NAME = { .uri = "...",
    .method = HTTP_X, .handler = NAME };` inside a function; every
    httpd_register_uri_handler() is a call `(server, &NAME)` after NAME's
    initializer in the same function; and NAME is named nowhere else in that
    function (a field assigned after the initializer, a copy, a pointer, a
    second registration). The module helper register_api_handler(), in the
    shape _REG_HELPER_BODY pins, is read by its calls instead, each of which
    names its uri as a literal. A route registered any other way (fields
    assigned one by one, a positional initializer, an array, an initializer
    at file scope or in another function, a reused name) would be served
    and never checked."""
    problems = []
    for fname, text in sorted(sources.items()):
        blanked = blank_string_contents(text)
        funcs = functions(text, indented=True)

        def where(pos: int) -> str:
            lineno = blanked.count("\n", 0, pos) + 1
            return f"{fname}:{lineno} ({' '.join(text.splitlines()[lineno - 1].split())[:90]!r})"

        def home(pos: int):
            inner = [span for *_x, span in funcs if span[0] <= pos < span[1]]
            return min(inner, key=lambda s: s[1] - s[0]) if inner else None

        helper = [span for name, _h, body, span in funcs if name == "register_api_handler"
                  and " ".join(body.rstrip()[:-1].split()) == _REG_HELPER_BODY]
        heads = {span[0] + head.rindex(name) for name, head, _b, span in funcs
                 if name == "register_api_handler"}
        inits = {}
        for m in re.finditer(r"\bhttpd_uri_t\b", blanked):
            if _inside(m.start(), helper):
                continue
            d = _ROUTE_INIT.match(blanked, m.start())
            span = home(m.start())
            close = _close(blanked, d.end() - 1) if d else -1
            fields = _route_fields(text, blanked, d.end() - 1, close) if close > 0 else None
            if span is None or fields is None or not re.match(r"\s*;", blanked[close + 1:]) or any(
                    len(fields.get(f, [])) != 1 or not re.fullmatch(rx, fields[f][0])
                    for f, rx in _ROUTE_FIELDS.items()):
                problems.append(f"{where(m.start())}: an httpd_uri_t the route table does not read: declare "
                                f"each route as `httpd_uri_t name = {{ .uri = \"/...\", .method = HTTP_X, "
                                f".handler = name }};` inside the function that registers it (a declaration "
                                f"with no such initializer, a pointer, a positional or computed field, or a "
                                f"route outside every function would be served unchecked)")
                continue
            inits.setdefault((span, d.group(1)), []).append(d.start(1))
        registered = {}
        for m in re.finditer(r"\bhttpd_register_uri_handler\b(?!\s*\()", blanked):
            problems.append(f"{where(m.start())}: httpd_register_uri_handler named other than in a call: "
                            f"register each route with a plain call")
        for m in re.finditer(r"\bhttpd_register_uri_handler\s*\(", blanked):
            if _inside(m.start(), helper):
                continue
            close = _close(blanked, m.end() - 1)
            arg = re.fullmatch(r"\s*\w+\s*,\s*&\s*(\w+)\s*", blanked[m.end():close]) if close > 0 else None
            span = home(m.start())
            key = (span, arg.group(1)) if arg and span else None
            if key not in inits or min(inits[key]) > m.start():
                problems.append(f"{where(m.start())}: a route registered in a shape the route table does not "
                                f"read: it reads `httpd_register_uri_handler(server, &name)` after "
                                f"`httpd_uri_t name = {{ ... }};` in the same function")
                continue
            registered.setdefault(key, []).append(m.end() + arg.start(1))
        for (span, name), decls in sorted(inits.items()):
            regs = registered.get((span, name), [])
            if len(decls) > 1 or len(regs) > 1:
                problems.append(f"{where(decls[-1])}: the route {name} is declared or registered more than "
                                f"once in one function: give each route its own name")
            allowed = set(decls) | set(regs)
            for m in re.finditer(r"(?<![\w.])(?<!->)" + re.escape(name) + r"\b", blanked[span[0]:span[1]]):
                if span[0] + m.start() not in allowed:
                    problems.append(f"{where(span[0] + m.start())}: the route {name} is named other than in its "
                                    f"initializer and its one registration (a field assigned after the "
                                    f"initializer, a copy or a pointer registers what the table never read): "
                                    f"declare another route instead")
        for m in re.finditer(r"\bregister_api_handler\b", blanked):
            if m.start() in heads:
                continue
            if _REG_CALL.match(text, m.start()) or _UNNAMED_REG.match(text, m.start()):
                continue
            problems.append(f"{where(m.start())}: a register_api_handler() the route table does not read "
                            f"(it reads the call `register_api_handler(server, \"/uri\", HTTP_X, handler)`)")
    return problems


def _assert_registration_rule() -> None:
    """The registration rule on fixtures, every run."""
    good = {
        "canary_wap.ino": (
            "static void register_api_routes(httpd_handle_t server) {\n"
            "  httpd_uri_t status = { .uri = \"/api/status\", .method = HTTP_GET, .handler = handle_status_auth };\n"
            "  httpd_register_uri_handler(server, &status);\n}\n"),
        "beacon_api.h": (
            "static inline void register_api_handler(httpd_handle_t server, const char* uri,\n"
            "                                        httpd_method_t method,\n"
            "                                        esp_err_t (*handler)(httpd_req_t*)) {\n"
            "  httpd_uri_t route = { .uri = uri, .method = method, .handler = handler, .user_ctx = nullptr };\n"
            "  httpd_register_uri_handler(server, &route);\n}\n"
            "inline void register_routes(httpd_handle_t server, const char* api_token = nullptr) {\n"
            "  register_api_handler(server, \"/api/beacon\", HTTP_GET, bcn_auth_gated<handle_status>);\n}\n"),
    }
    assert registration_problems(good) == [], registration_problems(good)
    reg = "  httpd_register_uri_handler(server, &status);\n}\n"
    ino = good["canary_wap.ino"]
    init = "  httpd_uri_t status = { .uri = \"/api/status\", .method = HTTP_GET, .handler = handle_status_auth };\n"

    def then(extra: str) -> str:
        return ino.replace(reg, reg.replace("}\n", "") + extra + "}\n")

    bad = {
        "N23 a route whose fields are assigned one by one": then(
            "  httpd_uri_t leak2;\n  leak2.uri = \"/api/leak2\";\n  leak2.method = HTTP_GET;\n"
            "  leak2.handler = [](httpd_req_t* r) -> esp_err_t { return httpd_resp_sendstr(r, \"x\"); };\n"
            "  httpd_register_uri_handler(server, &leak2);\n"),
        "N24 a positional initializer": then(
            "  httpd_uri_t leak3 = { \"/api/leak3\", HTTP_GET, handle_leak3, nullptr };\n"
            "  httpd_register_uri_handler(server, &leak3);\n"),
        "a route from an array": then("  for (auto& r : kRoutes) httpd_register_uri_handler(server, &r);\n"),
        # The fourth review's C5 and C5b: a name the table parsed, reused.
        "C5 a parsed route's fields assigned again and registered again": then(
            "  status.uri = \"/api/leak9\";\n  status.handler = handle_leak9;\n"
            "  httpd_register_uri_handler(server, &status);\n"),
        "C5b a route assigned field by field under a name another function parses": ino + (
            "static void register_leak10(httpd_handle_t server) {\n  httpd_uri_t status;\n"
            "  status.uri = \"/api/leak10\";\n  status.method = HTTP_GET;\n  status.handler = handle_leak10;\n"
            "  httpd_register_uri_handler(server, &status);\n}\n"),
        "a registration argument that is not &name": ino.replace("(server, &status);", "(server, g_alt ? g_alt : &status);"),
        "a route at file scope registered from a function": (
            "static httpd_uri_t kLeak = { .uri = \"/api/leak11\", .method = HTTP_GET, .handler = handle_leak11 };\n"
            + then("  httpd_register_uri_handler(server, &kLeak);\n")),
        "a route registered before its initializer": ino.replace(init + reg.replace("}\n", ""),
                                                                  reg.replace("}\n", "") + init),
        "a route copied": then("  auto copy = status;\n"),
        "a route's fields written through a pointer": then("  auto* p = &status;\n  p->uri = \"/api/leak12\";\n"),
        "a route declared twice in one function": then("  {\n" + init + "  }\n"),
        "a route with a computed handler": ino.replace(".handler = handle_status_auth",
                                                        ".handler = g_x ? handle_status_auth : handle_leak"),
        "httpd_register_uri_handler taken by address": then(
            "  auto reg = httpd_register_uri_handler;\n  reg(server, &status);\n"),
        "a registration in a macro": "#define REG(n) httpd_register_uri_handler(server, &n)\n" + ino,
        "httpd_register_uri_handler named other than in a call": then("  auto reg = httpd_register_uri_handler;\n"),
    }
    for why, text in bad.items():
        assert text != ino, f"registration fixture did not apply: {why}"
        assert registration_problems({**good, "canary_wap.ino": text}), f"registration rule passed {why}"
    # Two routes in two functions may share a name: each is read where it is registered.
    two = ino + ("static void register_more(httpd_handle_t server) {\n"
                 + init.replace("/api/status", "/api/more") + "  httpd_register_uri_handler(server, &status);\n}\n")
    assert registration_problems({**good, "canary_wap.ino": two}) == [], registration_problems({**good, "canary_wap.ino": two})
    helper_other = good["beacon_api.h"].replace(
        "  httpd_register_uri_handler(server, &route);\n}\n",
        "  httpd_register_uri_handler(server, &route);\n  httpd_register_uri_handler(server, &g_leak_route);\n}\n", 1)
    assert helper_other != good["beacon_api.h"] and registration_problems({**good, "beacon_api.h": helper_other}), \
        "registration rule passed a register_api_handler() whose body is not the module helper's"
    by_address = good["beacon_api.h"] + "static auto g_reg = register_api_handler;\n"
    assert registration_problems({**good, "beacon_api.h": by_address}), \
        "registration rule passed register_api_handler taken by address"
    helper_bad = good["beacon_api.h"].replace(
        "  register_api_handler(server, \"/api/beacon\",",
        "  register_api_handler(server, kUri, HTTP_GET, handle_x);\n  register_api_handler(server, \"/api/beacon\",")
    assert registration_problems({**good, "beacon_api.h": helper_bad}), \
        "registration rule passed a register_api_handler() call with a uri that is not a literal"


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


def tree_problems(sources: dict):
    """Everything main() holds the sketch to, over its sources (file name ->
    comment-stripped text): (failures, the routes read, the allowlist
    entries matched, the routes skipped as unreachable). The route table
    (every route gated or on PUBLIC_ALLOWLIST, no stale entry) and then
    rule_problems(); a fixture runs it on a planted set."""
    routes = []  # (uri, method, handler_expr, file)
    failures = []

    # Style 1: httpd_uri_t initializers (single- or multi-line).
    uri_decl = re.compile(
        r"httpd_uri_t\s+\w+\s*=\s*\{(.*?)\}\s*;", re.S)
    for fname, text in sources.items():
        for m in uri_decl.finditer(text):
            fields = m.group(1)
            uri = re.search(r'\.uri\s*=\s*"([^"]+)"', fields)
            method = re.search(r"\.method\s*=\s*HTTP_(\w+)", fields)
            handler = re.search(r"\.handler\s*=\s*([A-Za-z_][\w:<>]*)", fields)
            if re.search(r"\.handler\s*=", fields) and not handler:
                failures.append(f"{fname}: a route whose handler is not a name "
                                f"({' '.join(fields.split())[:90]!r}): the check reads a gate by "
                                f"its handler's name, so name the handler")
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
    unnamed_reg = re.compile(
        r'register_api_handler\s*\(\s*server\s*,\s*"([^"]+)"\s*,\s*'
        r"HTTP_(\w+)\s*,\s*(?=[^\s\w])")
    for fname, text in sources.items():
        for m in unnamed_reg.finditer(text):
            failures.append(f"{fname}: {m.group(2)} {m.group(1)} is registered with a handler "
                            f"that is not a name: the check reads a gate by its handler's "
                            f"name, so name the handler")
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
        # A gate counts in code only: one named in a log line is not one.
        if not any(_AUTH.search(blank_string_contents(body)) for body in bodies):
            failures.append(f"{method} {uri}: handler {handler} "
                            f"({fname}) has NO credential gate and is not "
                            f"on the public allowlist")

    ino_handlers = {h.split("::")[-1].split("<")[0]
                    for _u, _m, h, f in routes if f == "canary_wap.ino"}
    failures.extend(rule_problems(sources, ino, ino_handlers))

    stale = set(PUBLIC_ALLOWLIST) - matched_allowlist
    for key in sorted(stale):
        failures.append(f"allowlist entry {key[0]} {key[1]} matches no "
                        f"registered route — remove or fix it")
    return failures, routes, matched_allowlist, unreachable



def main() -> int:
    _assert_parser_robust()
    _assert_credential_rule()
    _assert_registration_rule()
    sources = {}
    for path in sorted(SKETCH.glob("*.h")) + sorted(SKETCH.glob("*.cpp")) + sorted(
            SKETCH.glob("*.ino")):
        if path.name == "web_assets_gz.h":
            continue  # generated byte arrays
        sources[path.name] = strip_comments(path.read_text(errors="replace"))

    failures, routes, matched_allowlist, unreachable = tree_problems(sources)
    if unreachable:
        by_mod = sorted({f"{ns} ({fn})" for _, _, ns, fn in unreachable})
        print(f"note: {len(unreachable)} route(s) skipped as unreachable "
              f"(register function never called from the sketch): "
              + ", ".join(by_mod))

    if len(routes) < 100:
        print(f"FAIL: parsed only {len(routes)} routes — parser regression?")
        return 1

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
