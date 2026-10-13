#!/usr/bin/env python3
"""Route-security conformance check for the canary-wap firmware.

Every HTTP route the sketch registers must be credential-gated — Bearer
token, session cookie, pair token, or the physical provisioning gate —
unless it is on the explicit PUBLIC allowlist below with a written
justification. This pins the fail-closed property statically so a new
endpoint (or a resurrected one: three /api/ble routes shipped without
auth and were only unreachable because they overflowed the handler
table) cannot silently widen the unauthenticated surface.

It also holds the routes that answer a credential (sweep F289). The
landing page at / mints a session for any caller that reaches the device,
on the home network too, so a route that answers the bearer token or the
SoftAP password takes the bearer header or a session minted beside a served
receipt (api_auth_check_credential / _optional), never a gate that takes any
session or the landing's pair token. What it reads, and nothing more:

  - canary_wap.ino: every function that reads g_device.api_token_str,
    g_device.ap_password or g_device.privkey, the TLS key (g_tls_key_der),
    or calls derive_api_token() / derive_ap_password(), other than as the
    token a gate compares against, is named in CREDENTIAL_READERS as an HTTP
    answer or a local use (serial, NVS, the AP's own bring-up, a module's
    init); an unnamed reader fails, and an answer must sit in, or be called
    only from, a route handler gated that way (the gate names are matched
    in code, not in string literals).
  - every other sketch file: each line that names a module's copy of the
    token (csi_integration.cpp's g_api_token, a register_routes module's
    auth_token_storage(), csi_mqtt.cpp's s_api_token_provider) has one of
    the shapes in TOKEN_COPY_LINES, and a `tok` read from one goes only to
    an auth check or a null test.
  - csi_mqtt.cpp's POST /api/mqtt/config: a save that keeps the stored
    broker password and moves it asks authorized_for_credential() (the
    bearer header or an owner's session, no session gate) before
    config_save().

It is a text check: a gate inside a branch that never runs, or a secret
reached through a name it does not know, passes it. The host suites
(test_receipt_session, test_mqtt_reinit) drive the behavior.

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
    "api_auth_check_credential(", # Bearer header or an owner's session (F289)
    "api_auth_check_credential_optional(",  # its silent variant (the receipt's grant 1)
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
    "handle_https_redirect": "301 to https at receipt_authority.h's address (503 when it has none); no data",
    "handle_tune_page": "session-cookie check, 302 to / when absent",
    "handle_wifi_pair_token": "gated on setup_wizard::is_active + direct Host",
}


# ── Routes that answer a credential (sweep F289) ──────────────────────────
#
# The gates that may stand in front of an answer that carries the bearer
# token or the SoftAP password: the Authorization header, or a session minted
# beside a served receipt (a BOOT tap or a setup-door claim).
CREDENTIAL_GATES = (
    "api_auth_check_credential(",
    "api_auth_check_credential_optional(",
)
# Gates that take any live session, the landing page's included, or the
# pair token the landing page hands any caller: never in front of such an
# answer.
ANY_SESSION_GATES = (
    "api_auth_check(",
    "api_auth_check_optional(",
    "api_auth_check_or_query(",
    "CSI_AUTH_OR_RETURN",
    "cv_session_validate(",
    "session_validate_cookie(",
    "authorized(req",
    "pair_token_valid(",
    "wifi_change_authorize(",
)
# Every function in canary_wap.ino that reads the bearer token, the SoftAP
# password, the device key or the TLS key (or derives one) other than as the
# token a gate or a module compares against. "answer": it puts a secret in
# an HTTP answer. "local": it never does.
CREDENTIAL_READERS = {
    "send_provisioning_receipt": ("answer", "the receipt's token and ap_password (the recovery kit)"),
    "handle_pairing_qr": ("answer", "the pairing QR's payload carries the token"),
    "resolve_ap_password": ("local", "hands the SoftAP its own password at bring-up"),
    "provision_device": ("local", "derives and stores the key, the token and the AP password at first boot"),
    "derive_api_token": ("local", "computes the token from the device key"),
    "derive_ap_password": ("local", "computes the SoftAP password from the device key"),
    "print_quick_connect_details": ("local", "USB serial, at the desk"),
    "handle_serial_commands": ("local", "USB serial commands"),
    "setup": ("local", "boot banner on USB serial; hands the key to the mesh"),
    "loop": ("local", "hands the key to the data-management pass"),
    "register_api_routes": ("local", "hands the key to device_signature"),
    "create_witness_record": ("local", "signs a witness record"),
    "tls_load_from_nvs": ("local", "reads the TLS key from NVS"),
    "tls_store_to_nvs": ("local", "writes the TLS key to NVS"),
    "tls_generate_self_signed_cert": ("local", "makes the TLS key"),
    "start_http_server": ("local", "hands the TLS key to the HTTPS server"),
}
_COMPARER = re.compile(
    r"\b(?:api_auth_check\w*|\w+::register_routes|csi_integration::init)\s*\(\s*\w+"
    r"\s*,\s*g_device\.api_token_str\s*\)")
_SECRET = re.compile(
    r"\bg_device\.(?:api_token_str|ap_password|privkey)\b|\bg_tls_key_der\b"
    r"|\bderive_(?:api_token|ap_password)\s*\(")

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
_FUNC_HEAD = re.compile(
    r"^(?!(?:if|for|while|switch|return|else|do)\b)[A-Za-z_][\w:<>\*&\s,]*?"
    r"\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*(?:const\s*)?\{", re.M)


def top_level_functions(text: str):
    """(name, body) for every function defined at column 0 of `text` (a
    comment-stripped source), braces matched over the string-blanked view."""
    blanked = blank_string_contents(text)
    out = []
    for m in _FUNC_HEAD.finditer(blanked):
        depth, i = 1, m.end()
        while i < len(blanked) and depth:
            if blanked[i] == "{":
                depth += 1
            elif blanked[i] == "}":
                depth -= 1
            i += 1
        out.append((m.group(1), text[m.start():i]))
    return out


def credential_problems(ino: str, handlers: set) -> list:
    """The F289 rule over canary_wap.ino (comment-stripped) and the names of
    the handlers it registers. Returns the problems, empty when it holds."""
    problems = []
    funcs = top_level_functions(ino)
    bodies = {}
    for name, body in funcs:
        bodies.setdefault(name, []).append(body)
    readers = set()
    for name, body in funcs:
        if _SECRET.search(_COMPARER.sub("", body)):
            readers.add(name)
    for name in sorted(readers - set(CREDENTIAL_READERS)):
        problems.append(f"{name}() reads the bearer token, the SoftAP password, the device key "
                        f"or the TLS key and is not in CREDENTIAL_READERS (name it as an answer "
                        f"or a local use)")
    for name in sorted(set(CREDENTIAL_READERS) - readers):
        problems.append(f"CREDENTIAL_READERS names {name}(), which reads none of those secrets — "
                        f"remove or fix it")
    # A local use never answers a request: a registered route handler named
    # "local" would hand its read to the caller unchecked.
    for name in sorted(readers & set(CREDENTIAL_READERS) & handlers):
        if CREDENTIAL_READERS[name][0] == "local":
            problems.append(f"{name}() is a registered route handler and reads a secret, so it "
                            f"is an answer, not a local use (CREDENTIAL_READERS)")

    def gated_for_credentials(handler: str) -> str:
        found = bodies.get(handler, [])
        if len(found) != 1:
            return f"{handler}() defined {len(found)} times"
        # Gate names count in code only: a log line that names a gate is not one.
        body = blank_string_contents(found[0])
        if not any(g in body for g in CREDENTIAL_GATES):
            return f"{handler}() answers a credential behind no {' / '.join(CREDENTIAL_GATES)}"
        took = [g for g in ANY_SESSION_GATES if g in body]
        if took:
            return (f"{handler}() answers a credential and takes {', '.join(took)}, which a "
                    f"landing-page session passes")
        return ""

    for name, (kind, _why) in sorted(CREDENTIAL_READERS.items()):
        if kind != "answer" or name not in readers:
            continue
        if name in handlers:
            why = gated_for_credentials(name)
            if why:
                problems.append(why)
            continue
        call = re.compile(r"\b" + re.escape(name) + r"\s*\(")
        callers = sorted({fn for fn, body in funcs
                          if fn != name and call.search(body)})
        if not callers:
            problems.append(f"{name}() answers a credential and nothing calls it")
        for fn in callers:
            if fn not in handlers:
                problems.append(f"{name}() answers a credential and is called from {fn}(), "
                                f"not a registered route handler")
                continue
            why = gated_for_credentials(fn)
            if why:
                problems.append(why)
    return problems


def token_copy_problems(sources: dict) -> list:
    """Every line outside canary_wap.ino that names a module's copy of the
    token has a TOKEN_COPY_LINES shape, and each `tok` read from one meets
    only _TOK_USES. `sources` maps file names to comment-stripped text."""
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
        for name, body in top_level_functions(text):
            if not re.search(r"const char\* tok = (?:auth_token_storage|s_api_token_provider)", body):
                continue
            rest = _TOK_USES.sub("", blank_string_contents(body))
            if re.search(r"\btok\b", rest):
                problems.append(f"{fname}: {name}() reads the token into `tok` and uses it other "
                                f"than in an auth check or a null test")
    return problems


_MQTT_GATE = re.compile(
    r"if\s*\(\s*moves_the_kept_password\s*\(\s*c\s*,\s*password_sent\s*,\s*ca_changed\s*\)"
    r"\s*&&\s*!\s*authorized_for_credential\s*\(\s*req\s*,\s*tok\s*\)\s*\)")


def mqtt_password_problems(mqtt: str) -> list:
    """POST /api/mqtt/config (csi_mqtt.cpp, comment-stripped): the save that
    keeps the stored broker password and moves it asks the owner's gate
    before config_save(), and that gate takes no session of any origin."""
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
    owner = re.search(r"static bool authorized_for_credential\s*\([^)]*\)\s*\{(.*?)\n\}", mqtt, re.S)
    if owner is None:
        problems.append("csi_mqtt.cpp: authorized_for_credential() not found")
    else:
        body = blank_string_contents(owner.group(1))
        took = [g for g in ANY_SESSION_GATES if g in body]
        if took or "cv_session_validate_owner" not in body or "api_auth_bearer_header_ok(" not in body:
            problems.append("csi_mqtt.cpp: authorized_for_credential() must take the bearer header "
                            "or an owner's session only" + (f" (it takes {', '.join(took)})" if took else ""))
    return problems


def _assert_credential_rule() -> None:
    """The F289 rule on fixtures, every run: the shapes it must pass and
    the ones it must refuse."""
    good = (
        "static esp_err_t send_provisioning_receipt(httpd_req_t* req, Grant g) {\n"
        "  in.token = g_device.api_token_str;\n  in.ap_password = g_device.ap_password;\n}\n"
        "static esp_err_t handle_provisioning_receipt(httpd_req_t* req) {\n"
        "  if (api_auth_check_credential_optional(req, g_device.api_token_str)) {\n"
        "    return send_provisioning_receipt(req, Grant::BEARER);\n  }\n"
        "  return send_provisioning_receipt(req, Grant::BOOT_TAP);\n}\n"
        "static esp_err_t handle_pairing_qr(httpd_req_t* req) {\n"
        "  if (!api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n"
        "  snprintf(p, n, \"%s\", g_device.api_token_str);\n}\n"
        "static esp_err_t handle_status_auth(httpd_req_t* req) {\n"
        "  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;\n}\n"
        "static bool resolve_ap_password(char* o, size_t n) { snprintf(o, n, \"%s\", g_device.ap_password); }\n"
        "static void provision_device() { x(g_device.api_token_str, g_device.ap_password); }\n"
        "static void print_quick_connect_details() { p(g_device.api_token_str); }\n"
        "static void handle_serial_commands() { p(g_device.ap_password); }\n"
        "void setup() {\n  p(g_device.ap_password);\n"
        "  bluetooth_api::register_routes(active_server, g_device.api_token_str);\n}\n"
        "static bool derive_api_token(const uint8_t k[32], char* o, size_t n) { return true; }\n"
        "static void derive_ap_password(const uint8_t k[32], char* o, size_t n) {}\n"
        "void loop() { datamgmt::process(g_device.privkey); }\n"
        "static void register_api_routes(httpd_handle_t s) { device_signature::init(g_device.privkey); }\n"
        "static void create_witness_record() { sign_message(g_device.privkey); }\n"
        "static bool tls_load_from_nvs() { return load(&g_tls_key_der); }\n"
        "static bool tls_store_to_nvs() { return store(g_tls_key_der); }\n"
        "static bool tls_generate_self_signed_cert() { return make(&g_tls_key_der); }\n"
        "static void start_http_server() { c.prvtkey_pem = g_tls_key_der; }\n"
    )
    handlers = {"handle_provisioning_receipt", "handle_pairing_qr", "handle_status_auth"}
    assert credential_problems(good, handlers) == [], credential_problems(good, handlers)
    qr_gate = "  if (!api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n"
    bad = {
        "the receipt's old first grant": good.replace(
            "api_auth_check_credential_optional(req", "api_auth_check_optional(req"),
        "the pairing QR's old gate": good.replace(
            "api_auth_check_credential(req", "api_auth_check(req"),
        "a credential gate beside a session gate": good.replace(
            "  return send_provisioning_receipt(req, Grant::BOOT_TAP);",
            "  if (cv_session_validate(req)) return send_provisioning_receipt(req, Grant::BEARER);"),
        "a new answer nobody classified": good + (
            "static esp_err_t handle_leak(httpd_req_t* req) {\n"
            "  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;\n"
            "  return httpd_resp_sendstr(req, g_device.api_token_str);\n}\n"),
        "the receipt written from a second route": good + (
            "static esp_err_t handle_other(httpd_req_t* req) {\n"
            "  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;\n"
            "  return send_provisioning_receipt(req, Grant::BEARER);\n}\n"),
        # The review's plants (F289 review): a gate that takes the landing's
        # pair token or any session beside the credential gate, and a
        # credential gate named only in a string literal.
        "the landing's pair token beside the credential gate": good.replace(qr_gate, (
            "  if (!csi_integration::pair_token_valid(\"x\") &&\n"
            "      !api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n")),
        "wifi_change_authorize beside the credential gate": good.replace(qr_gate, (
            "  if (!wifi_change_authorize(req) &&\n"
            "      !api_auth_check_credential(req, g_device.api_token_str)) return ESP_OK;\n")),
        "a credential gate named only in a string": good.replace(qr_gate, (
            "  Serial.println(\"api_auth_check_credential(\");\n"
            "  if (!csi_integration::pair_token_valid(t)) return ESP_OK;\n")),
        "the SoftAP password derived in a new answer": good + (
            "static esp_err_t handle_status_ap(httpd_req_t* req) {\n"
            "  if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;\n"
            "  derive_ap_password(g_device.privkey, buf, sizeof(buf));\n"
            "  return httpd_resp_sendstr(req, buf);\n}\n"),
    }
    handlers_bad = handlers | {"handle_leak", "handle_other", "handle_status_ap"}
    for why, text in bad.items():
        assert credential_problems(text, handlers_bad), f"credential rule passed {why}"
    # A route handler listed as a local use is still a route handler.
    assert credential_problems(good, handlers | {"handle_serial_commands"}), \
        "credential rule passed a route handler named as a local use"

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
    }
    for why, (fname, text) in copy_bad.items():
        assert token_copy_problems({**copies, fname: text}), f"token-copy rule passed {why}"

    # POST /api/mqtt/config.
    mqtt_good = (
        "static bool authorized_for_credential(httpd_req_t* req, const char* api_token) {\n"
        "  if (api_token && api_token[0] != '\\0' && api_auth_bearer_header_ok(req, api_token)) return true;\n"
        "  return cv_session_validate_owner && cv_session_validate_owner(req);\n}\n"
        "esp_err_t handle_config_post(httpd_req_t* req) {\n"
        "  if (!authorized(req, tok)) {\n    return ESP_OK;\n  }\n"
        "  if (moves_the_kept_password(c, password_sent, ca_changed) &&\n"
        "      !authorized_for_credential(req, tok)) {\n    return ESP_OK;\n  }\n"
        "  if (!config_save(c)) {\n    return ESP_OK;\n  }\n  return ESP_OK;\n}\n")
    assert mqtt_password_problems(mqtt_good) == [], mqtt_password_problems(mqtt_good)
    mqtt_gate = ("  if (moves_the_kept_password(c, password_sent, ca_changed) &&\n"
                 "      !authorized_for_credential(req, tok)) {\n    return ESP_OK;\n  }\n")
    mqtt_bad = {
        "the old handler, no owner's gate": mqtt_good.replace(mqtt_gate, ""),
        "the owner's gate after the save": mqtt_good.replace(mqtt_gate, "").replace(
            "  return ESP_OK;\n}\n", mqtt_gate + "  return ESP_OK;\n}\n"),
        "an owner's gate that takes any session": mqtt_good.replace(
            "  return cv_session_validate_owner && cv_session_validate_owner(req);",
            "  return csi_integration::session_validate_cookie(req);"),
    }
    for why, text in mqtt_bad.items():
        assert mqtt_password_problems(text), f"MQTT rule passed {why}"


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
    _assert_credential_rule()
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

    ino_handlers = {h.split("::")[-1].split("<")[0]
                    for _u, _m, h, f in routes if f == "canary_wap.ino"}
    failures.extend(credential_problems(ino, ino_handlers))
    failures.extend(token_copy_problems(sources))
    failures.extend(mqtt_password_problems(sources.get("csi_mqtt.cpp", "")))

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
