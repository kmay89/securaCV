#!/usr/bin/env python3
"""Route-security conformance check for the canary-wap firmware.

Every HTTP route the sketch registers must be credential-gated — Bearer
token, session cookie, pair token, or the physical provisioning gate —
unless it is on the explicit PUBLIC allowlist below with a written
justification. This pins the fail-closed property statically so a new
endpoint (or a resurrected one: three /api/ble routes shipped without
auth and were only unreachable because they overflowed the handler
table) cannot silently widen the unauthenticated surface.

Run from the repo root (CI: firmware.yml host tests):
    python3 firmware/projects/canary-wap/tests_host/check_route_security.py
"""

import re
import sys
from pathlib import Path

SKETCH = Path("firmware/projects/canary-wap/arduino/canary_wap")

# Substrings whose presence in a handler body (or its registration
# expression) counts as a credential gate. Each is a real call the auth
# audit verified enforces access.
AUTH_MARKERS = (
    "api_auth_check(",            # Bearer + session cookie (401/403/429)
    "api_auth_check_or_query(",   # + ?token= for <img>/stream tags
    "api_auth_check_optional(",   # silent variant (caller enforces)
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
    "handle_https_redirect": "302 only",
    "handle_tune_page": "session-cookie check, 302 to / when absent",
    "handle_wifi_pair_token": "gated on setup_wizard::is_active + direct Host",
}


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


def main() -> int:
    _assert_parser_robust()
    sources = {}
    for path in sorted(SKETCH.glob("*.h")) + sorted(SKETCH.glob("*.cpp")) + sorted(
            SKETCH.glob("*.ino")):
        if path.name == "web_assets_gz.h":
            continue  # generated byte arrays
        sources[path.name] = strip_comments(path.read_text(errors="replace"))

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
