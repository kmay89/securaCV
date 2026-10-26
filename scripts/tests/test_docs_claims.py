#!/usr/bin/env python3
"""The security docs may not carry a retired claim — a banned-claims gate.

docs/security/SECURITY_MODEL.md ships in every evidence export, and for a
while it promised things the firmware did not do: zero outbound connections
(the networked products open a broker socket, a daily signed-manifest check
and SNTP), TLS on every hop (the flagship's release images serve plain HTTP on
port 80, and since F15's correction of 2026-10-19 no stock unit of either tree
serves HTTPS at all: the certificate generator's guard names a header no
pinned core ships, so the flagship's dev and full builds and the WAP, whose
HTTPS turns on by itself after setup once a certificate is stored, serve
plain HTTP), Bluetooth compiled out
(the shipped WAP profile compiles NimBLE), an API access code released only
by a button press (the flagship hands its token with no press to the
first-boot wizard, and after setup to a bearer caller and a request over its
own access point).
THREAT_MODEL.md repeated them in its principles and its Definition of Done,
firmware/FEATURES.md called the flagship's broker link plain after it had
joined the shared TLS transport, and three product READMEs said the flashers
had no field for a select both flashers already carried.

Each of those sentences was rewritten to the shipped posture, with the
CHANGELOG's status words; the review of that rewrite retired three more
(a client-certificate check no API performs, a GPS clock two products do
not have, a derived BSSID no code sets), and catching the rewrite up with the
flagship's page-token gate (#1704) and Host guard (#1691) retired two of its
own (no button press releasing the dashboard, the page handing its token to
whoever loads it); its review retired one more before it merged (all four
HTTPS builds called CI-compiled, when CI builds two). This test pins the
retirement: the EXACT retired sentences are the needles, so a later true
sentence that happens to share a fragment ("not compiled in", "plain by
default") cannot trip it, and a sentence that comes back verbatim — a merge
that resurrects an old hunk, a paste from a stale export — fails the build
with the file and the claim named.

Whitespace is normalized before matching, because the retired sentences wrap
across lines in the Markdown source and a needle that only matched the wrapped
form would be defeated by a reflow.

Same shape as scripts/lint_fleet_word.py: MUST_PASS / MUST_FAIL self-test
strings guard the guard, so a needle that stops matching its own retired
sentence is a red test, not a silent hole.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_docs_claims.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# The documents the retired claims lived in. Every one must exist — a moved
# file would otherwise make the gate pass by scanning nothing.
FILES = (
    "docs/security/SECURITY_MODEL.md",
    "docs/security/THREAT_MODEL.md",
    "firmware/FEATURES.md",
    "firmware/projects/canary-vision/README.md",
    "firmware/projects/canary-sense/README.md",
    "firmware/projects/canary-display/README.md",
)

# (needle, what shipped instead). The needle is the retired sentence as it
# was written — not a paraphrase, not a fragment. Add a row only for a
# sentence that was actually retired for being untrue; the replacement
# text is the reason, so the next reader can tell a resurrected claim from
# a legitimate new one.
BANNED = (
    ("zero outbound network connections",
     "the networked products open the broker socket, a daily signed-manifest "
     "check, SNTP on the display line and the opt-in forecast"),
    ("Bluetooth is disabled at compile time",
     "the shipped canary-wap FULL profile compiles NimBLE for pairing, the "
     "GATT status service, BLE OTA v2 and the Scout scanner"),
    ("All communication between your phone/computer and the device is "
     "encrypted with TLS",
     "the flagship's release images serve plain HTTP on port 80 (its dev and "
     "full builds serve HTTPS from the first boot after setup once a "
     "certificate is stored, CI-compiled); the WAP serves HTTPS after setup "
     "once a certificate is stored and falls back to HTTP when it fails to "
     "start; and since F15's correction (2026-10-19) no stock unit of either "
     "tree stores one, since the generator's guard names a header no pinned "
     "core ships"),
    ("displayed only via physical button press",
     "one BOOT tap hands the flagship's token to one page load, but it also "
     "goes with no press to the first-boot wizard, and after setup to a "
     "bearer caller and a request over the flagship's own access point "
     "(provisioning_gate.h page_token_policy); the WAP's landing page mints "
     "a one-tap session"),
    ("TLS is required for all API access (no HTTP fallback)",
     "plain HTTP is a stated posture: setup mode and start failure on the "
     "WAP, the flagship's release images (its dev and full builds redirect "
     "port 80 to HTTPS from the first boot after setup once a certificate is "
     "stored), the displays' LAN page, and since F15's correction "
     "(2026-10-19) every stock unit of either tree, none of which can "
     "generate its certificate"),
    ("`securacv_mqtt` still plain",
     "firmware/canary's securacv_mqtt rides the shared broker-TLS transport "
     "(plain / CA / pin / lab, fail-closed) since 2026-09-19"),
    ("there is no form field or on-device setting yet",
     "both flashers carry the Broker encryption select with the CA box or "
     "fingerprint field; desktop_parity.test.js pins the two forms equal"),
    # Retired by the review of the rewrite above — sentences the rewrite
    # itself introduced and that turned out untrue.
    ("Client presents an expired or unknown certificate to the API",
     "no API asks a client for a certificate: the kernel's rustls config is "
     "with_no_client_auth() and the WAP's httpd_ssl_config_t carries only the "
     "server certificate and key; the iPhone app pins the receipt's tls_cert_fp"),
    ("a witness's clock comes from GPS",
     "only the flagship and the WAP take time from GPS; sense and vision have "
     "no clock source of their own; the displays sync SNTP"),
    ("WiFi AP BSSID derived from device identity",
     "nothing in the firmware sets a derived or random MAC; the BSSID is the "
     "radio's factory address and carries Espressif's OUI"),
    # Retired when the rewrite met the flagship it describes: it was written
    # against a tree that had neither the page-token gate nor the Host guard.
    ("no button press releases the dashboard",
     "#1704: one BOOT tap unlocks one home-network page load with the token "
     "(provisioning_gate.h page_token_decide spends the tap), and a request "
     "over the flagship's own access point gets it with no tap"),
    ("the page itself carries the API token to whoever can load",
     "#1704: the token rides GET / and /setup for the first-boot wizard, and "
     "after setup only for a bearer caller, a request over the flagship's own "
     "access point or a page load that spends one BOOT tap; "
     "#1691: the page never carries it for a foreign Host, and the API's "
     "bearer gate (auth_gate) answers that Host 403 {\"error\":\"host\"} "
     "except over the access point"),
    # Retired by the review of that catch-up before it merged: it called all
    # four FEATURE_HTTPS builds CI-compiled.
    ("Those builds are compiled by CI and have never run on hardware",
     "CI compiles only dev and full of the four FEATURE_HTTPS builds "
     "(firmware/flavors.json canary build_envs); dev_ha and usb-onboard "
     "inherit the flag from dev and no workflow builds them"),
    # Retired by the second review of F289's third road: the WAP's rule,
    # stated in the flagship's broker paragraph with no device named.
    ("A dashboard session alone cannot re-point the bridge at another host, "
     "port, TLS mode or CA while keeping the stored password; that save needs "
     "the bearer or the password typed again (sweep F289).",
     "the flagship has no dashboard session and refuses a host-or-port move "
     "from any caller with 400 password_required_for_new_host "
     "(mqtt_tls_fields::credential_carry; a TLS-mode or CA change alone is "
     "not a move there); the WAP's 403 password_needed is for the session "
     "alone and counts the TLS mode and the CA as moves — each device's rule "
     "is stated under its own name"),
)

# Sentences retired from documents the gate above does not scan whole (the
# sweep ledger quotes retired claims on purpose, as what was retired), each
# scanned for its own sentence only: (file, needle, what is true instead).
RETIRED_IN = (
    # F289's second review: the ledger and LESSONS_LEARNED described the
    # hand repro (one loop pass after the host write) as what the race test
    # does under the busy-word mutant.
    ("docs/audit/repo_todo_sweep_2026-09.md",
     "With the busy word never spoken, the first fails at the host write "
     "with the stored password on a client for `10.0.0.66`.",
     "with *busy never set, each race test fails on its first check with no "
     "client opened (the request marked served from a half-written row; "
     "init() returning false); with the overlap check removed the first "
     "fails at the save's first write"),
    ("firmware/LESSONS_LEARNED.md",
     "with the busy word never spoken the first fails at the host write "
     "with the stored password on a client for the new host.",
     "the same: no client is opened under that mutant; the loop task marks "
     "the request served and the boot's init() returns false"),
    # The QR hub-provision road's grant called "not the session's": the scan
    # route takes the pair token, and GET /api/pair/token mints that token
    # for the cookie (CSI_AUTH_OR_RETURN takes the session before the bearer).
    ("docs/audit/repo_todo_sweep_2026-09.md",
     "but its grant is not the session's: a scan window opens from the boot "
     "auto-scan or from `POST /api/wifi/qr-scan`, which takes the wizard's "
     "one-shot pair token (`pair_token_valid()`), never the cookie",
     "the session buys the pair token over GET /api/pair/token; the camera "
     "is what it does not hold"),
    ("firmware/LESSONS_LEARNED.md",
     "Its grant is not the session's: the scan window opens from the boot "
     "auto-scan or from `POST /api/wifi/qr-scan` behind the wizard's one-shot "
     "pair token (`pair_token_valid()`)",
     "the same"),
    ("CHANGELOG.md",
     "behind the pair token and a code in front of the camera, not the "
     "session",
     "behind the pair token, which the session can mint, and a code in front "
     "of the camera, which it cannot"),
)

# Strings that must never trip the gate: the true prose that replaced the
# claims shares words with it, and the gate is worthless if it blocks the
# correction.
MUST_PASS = [
    "the broker socket is plain by default until you provision TLS",
    "BLE (NimBLE) is compiled into the shipped Canary WAP build",
    "the MINIMAL profile compiles it out; the code is not compiled in there",
    "a release image serves plain HTTP on port 80",
    "the WAP serves HTTPS on port 443 once setup has completed and a "
    "certificate is stored",
    "set from the Broker encryption select in either flasher's broker block",
    "nothing outbound that is not disclosed, named and tested",
    "the token stops drive-by web pages, not a LAN host",
    # A true sentence that shares its last two words with a retired one —
    # the review caught the fragment needle tripping on exactly this.
    "the socket is still plain until you provision TLS",
    "the AP's BSSID is the radio's factory MAC and carries Espressif's OUI",
    "the flagship and the WAP take their time from GPS",
    "no API asks the client for a certificate",
    # The page-token prose that replaced the two retired flagship sentences.
    "a home-network load after setup gets the page without its token",
    "the page carries the API token only for the first-boot wizard, a bearer "
    "caller, a request over the Canary's own access point or one BOOT tap",
    # The build-status prose that replaced the overclaimed HTTPS sentence.
    "CI compiles `dev` and `full`; `dev_ha` and `usb-onboard` inherit the "
    "flag from `dev` and no workflow builds them; none of the four has run "
    "on hardware",
    # The scoped prose that replaced the unscoped broker-move sentence.
    "On the WAP, the same save from the dashboard session alone is refused "
    "`403 password_needed` when it moves the host, port, TLS mode or CA; the "
    "bearer, or the password typed again, moves it (sweep F289).",
]
# The replacements of the RETIRED_IN sentences, which share most of their
# words with what they replaced.
MUST_PASS_IN = [
    "With the busy word never spoken (`open_client()` reporting no save it "
    "saw), each of the three fails on its first check, with no client opened",
    "with the busy word never spoken each of the three fails on its first "
    "check (the loop task marks the request served from a row a save was "
    "still writing",
    "and the session reaches it: a scan window opens from the boot auto-scan "
    "or from `POST /api/wifi/qr-scan`, which takes the wizard's one-shot pair "
    "token (`pair_token_valid()`) and not the cookie — but the session buys "
    "that token",
    "behind the pair token, which the session can mint (`GET /api/pair/token` "
    "takes the cookie), and a code in front of the camera, which it cannot",
]
# And the retired sentences as they stood in the source, wrapped and marked
# up — the ban losing its teeth is the other failure mode.
MUST_FAIL = [
    "The device makes **zero outbound network connections**. It does not",
    "- Bluetooth is disabled at compile time (the code to enable it does not\n"
    "  exist in standard firmware)",
    "All communication between your phone/computer and the device is\n"
    "encrypted with TLS (the same encryption used by banks and secure websites).",
    "3. **The API access code** (if enabled — displayed only via physical button press)",
    "7. TLS is required for all API access (no HTTP fallback)",
    "`firmware/canary`'s `securacv_mqtt` still plain.",
    "(the flashers' NVS builders seed them; there is no form field or on-device\n"
    "setting yet):",
    "| Client presents an expired or unknown certificate to the API | Rejected |",
    "- **SNTP** on the display line — a witness's clock comes from GPS, a\n"
    "  display has none",
    "- WiFi AP BSSID derived from device identity (no manufacturer OUI leak)",
    "it: no button press releases the dashboard, and the device cannot tell one\n"
    "host on its network from another.",
    "point and home network alike, and the page itself carries the API token to\n"
    "whoever can load `GET /` or `/setup`. That token is a defense against",
    "`/api/status` `tls_mode_reason`. Those builds are compiled by CI and have\n"
    "never run on hardware. The flagship's release images (`release`,",
    "clear like the rest of the device API. A dashboard session alone cannot\n"
    "re-point the bridge at another host, port, TLS mode or CA while keeping the\n"
    "stored password; that save needs the bearer or the password typed again\n"
    "(sweep F289). This is compile-tested, host-tested, not yet bench-tested.",
]
# And the RETIRED_IN sentences as they stood, wrapped.
MUST_FAIL_IN = [
    "  `config_clear` inside the boot's read). With the busy word never spoken,\n"
    "  the first fails at the host write with the stored password on a client for\n"
    "  `10.0.0.66`. The review also found `port_or_default()` unreachable with a 0",
    "  hooks; with the busy word never spoken the first fails at the host write\n"
    "  with the stored password on a client for the new host. Rule: when a",
    "  broker with the stored password and no password typed — but its grant is\n"
    "  not the session's: a scan window opens from the boot auto-scan or from\n"
    "  `POST /api/wifi/qr-scan`, which takes the wizard's one-shot pair token\n"
    "  (`pair_token_valid()`), never the cookie, and a code must be in front of",
    "  stored password and no password typed. Its grant is not the session's:\n"
    "  the scan window opens from the boot auto-scan or from\n"
    "  `POST /api/wifi/qr-scan` behind the wizard's one-shot pair token\n"
    "  (`pair_token_valid()`), and a code must be in front of the camera",
    "  still re-points the broker with the stored password — behind the pair\n"
    "  token and a code in front of the camera, not the session; named in the\n"
    "  ledger.",
]


def normalized(text: str) -> str:
    """Collapse every whitespace run to one space so a wrapped sentence matches."""
    return re.sub(r"\s+", " ", text)


def offenders_in(text: str) -> list[str]:
    flat = normalized(text)
    return [needle for needle, _why in BANNED if needle in flat]


def retired_in_offenders(text: str) -> list[str]:
    flat = normalized(text)
    return [needle for _rel, needle, _why in RETIRED_IN if needle in flat]


def scan_retired_in(root: Path = REPO) -> dict[str, list[str]]:
    """{relative path: [its own retired sentences found]} over RETIRED_IN."""
    found: dict[str, list[str]] = {}
    for rel, needle, _why in RETIRED_IN:
        if needle in normalized((root / rel).read_text(encoding="utf-8")):
            found.setdefault(rel, []).append(needle)
    return found


def scan(root: Path = REPO) -> dict[str, list[str]]:
    """{relative path: [needles found]} for every scanned file that carries one."""
    found: dict[str, list[str]] = {}
    for rel in FILES:
        hits = offenders_in((root / rel).read_text(encoding="utf-8"))
        if hits:
            found[rel] = hits
    return found


class TheGuardItself(unittest.TestCase):
    def test_every_needle_is_a_whole_retired_sentence_not_a_fragment(self):
        # A needle this short would start matching true prose; the critic's
        # rule is "exact retired sentences, never generic fragments".
        for needle, why in BANNED:
            with self.subTest(needle=needle):
                self.assertGreaterEqual(len(needle.split()), 2)
                self.assertEqual(needle, normalized(needle).strip())
                self.assertTrue(why, "every ban says what shipped instead")

    def test_true_prose_passes(self):
        for s in MUST_PASS:
            with self.subTest(s=s):
                self.assertEqual(offenders_in(s), [])

    def test_retired_sentences_fail_as_written_wrapped_and_marked_up(self):
        for s in MUST_FAIL:
            with self.subTest(s=s):
                self.assertNotEqual(offenders_in(s), [], "the ban lost its teeth")

    def test_every_scoped_needle_is_a_sentence_with_a_reason(self):
        for rel, needle, why in RETIRED_IN:
            with self.subTest(needle=needle):
                self.assertTrue((REPO / rel).is_file(), f"{rel} moved — update RETIRED_IN")
                self.assertGreaterEqual(len(needle.split()), 2)
                self.assertEqual(needle, normalized(needle).strip())
                self.assertTrue(why)

    def test_scoped_replacements_pass_and_retired_forms_fail(self):
        for s in MUST_PASS_IN:
            with self.subTest(s=s):
                self.assertEqual(retired_in_offenders(s), [])
        for s in MUST_FAIL_IN:
            with self.subTest(s=s):
                self.assertNotEqual(retired_in_offenders(s), [], "the ban lost its teeth")


class TheDocuments(unittest.TestCase):
    def test_every_scanned_file_exists(self):
        for rel in FILES:
            with self.subTest(file=rel):
                self.assertTrue((REPO / rel).is_file(), f"{rel} moved — update FILES")

    def test_no_retired_claim_survives(self):
        found = scan()
        lines = [f"  {rel}: {hits}" for rel, hits in sorted(found.items())]
        self.assertEqual(
            found, {},
            "a retired security claim is back; each row of BANNED says what "
            "shipped instead:\n" + "\n".join(lines),
        )

    def test_no_retired_sentence_survives_where_it_stood(self):
        found = scan_retired_in()
        lines = [f"  {rel}: {hits}" for rel, hits in sorted(found.items())]
        self.assertEqual(
            found, {},
            "a sentence retired for being untrue is back in the file it stood "
            "in; each row of RETIRED_IN says what is true instead:\n" + "\n".join(lines),
        )


# What the rewritten sentences claim of the code, held to the code: the
# ledger is what the next sweep reads, so a claim it makes about a gate must
# stay the gate's.
WAP = REPO / "firmware/projects/canary-wap/arduino/canary_wap"
FLAGSHIP_NET = REPO / "firmware/canary/lib/securacv_network/src/securacv_network.cpp"
FLAGSHIP_TLS = REPO / "firmware/canary/lib/securacv_mqtt/src/mqtt_tls_fields.h"


def body_after(text: str, start: str, end_pattern: str) -> str:
    """The text from `start` to the next match of `end_pattern` after it."""
    i = text.index(start)
    m = re.compile(end_pattern).search(text, i + len(start))
    return text[i:m.start()] if m else text[i:]


class TheClaimsHoldToTheCode(unittest.TestCase):
    def test_the_session_buys_the_pair_token_and_the_scan_route_takes_it(self):
        csi = (WAP / "csi_integration.cpp").read_text(encoding="utf-8")
        # GET /api/pair/token is handle_pair_token, whose first statement is
        # the session-or-bearer macro ...
        self.assertIn('.uri = "/api/pair/token", .method = HTTP_GET, .handler = handle_pair_token', csi)
        handler = body_after(csi, "esp_err_t handle_pair_token(httpd_req_t* req) {", r"\n}\n")
        first = [ln.strip() for ln in handler.splitlines()[1:] if ln.strip()][0]
        self.assertEqual(first, "CSI_AUTH_OR_RETURN(req);")
        # ... and that macro takes the cookie before it looks for a bearer.
        macro = body_after(csi, "#define CSI_AUTH_OR_RETURN(req)", r"\n\n")
        cookie = macro.index("cv_session_validate((req))")
        bearer = macro.index("api_auth_check_optional((req), g_api_token")
        self.assertLess(cookie, bearer)
        # POST /api/wifi/qr-scan takes that token and no cookie of its own.
        ino = (WAP / "canary_wap.ino").read_text(encoding="utf-8")
        self.assertIn('.uri = "/api/wifi/qr-scan", .method = HTTP_POST,   .handler = handle_qr_scan_start', ino)
        start = body_after(ino, "static esp_err_t handle_qr_scan_start(httpd_req_t* req) {", r"\nstatic esp_err_t ")
        self.assertIn("csi_integration::pair_token_valid(token)", start)
        for cookie_gate in ("cv_session_validate", "session_validate_cookie", "CSI_AUTH_OR_RETURN"):
            self.assertNotIn(cookie_gate, start)
        # The scan task's hub-code branch re-points the broker with the stored
        # password and re-checks the token only for a Wi-Fi-only code.
        self.assertIn("prov.wifi_only && !g_qr_auto_scan &&\n"
                      "                   !csi_integration::pair_token_valid(g_qr_scan_token)", ino)
        hub = body_after(ino, "if (!prov.wifi_only && prov.host[0]) {", r"\n          }\n")
        self.assertIn("strlcpy(mc.host, prov.host, sizeof(mc.host));", hub)
        self.assertIn("mc.port = prov.port;", hub)
        self.assertIn("csi_mqtt::config_save(mc)", hub)
        self.assertIn("csi_mqtt::request_reinit()", hub)
        self.assertNotIn("mc.pass", hub)

    def test_the_csi_macro_answers_a_held_bearer_429(self):
        # Sweep F339: the CSI routes' bearer is compared through the accounted
        # grant, which reports a held address through `held_` (no second
        # lockout read), and the macro answers that 429 before its 401.
        csi = (WAP / "csi_integration.cpp").read_text(encoding="utf-8")
        macro = body_after(csi, "#define CSI_AUTH_OR_RETURN(req)", r"\n\n")
        lines = [ln.rstrip("\\").strip() for ln in macro.splitlines()]
        self.assertEqual(lines[1], "do {")
        self.assertEqual(lines[2], "bool held_ = false;", "held_ is the do-block's first statement")
        grant = macro.index("if (api_auth_check_optional((req), g_api_token, &held_)) break;")
        answer = macro.index("if (held_) { api_auth_send_lockout((req)); return ESP_OK; }")
        unauthorized = macro.index('httpd_resp_set_status((req), "401 Unauthorized");')
        self.assertLess(macro.index("cv_session_validate((req))"), grant)
        self.assertLess(grant, answer)
        self.assertLess(answer, unauthorized)
        # The grant itself: the cookie, else api_auth_bearer_grant() with the
        # held out-parameter, in api_auth.h.
        auth = (WAP / "api_auth.h").read_text(encoding="utf-8")
        optional = body_after(auth, "static bool api_auth_check_optional(httpd_req_t* req, const char* expected_token, bool* held = nullptr) {", r"\n}\n")
        self.assertIn("return api_auth_bearer_grant(req, expected_token, held);", optional)
        self.assertNotIn("api_auth_bearer_valid(", optional)

    def test_the_flagship_rule_is_host_or_port_from_any_caller(self):
        net = FLAGSHIP_NET.read_text(encoding="utf-8")
        for word in ("cv_session", "session_validate_cookie", "Set-Cookie"):
            self.assertNotIn(word, net, "the flagship grew a dashboard session; re-read SECURITY_MODEL's broker paragraph")
        self.assertIn("tf::Verdict::PasswordRequiredForNewHost", net)
        tls = FLAGSHIP_TLS.read_text(encoding="utf-8")
        self.assertIn("inline CredentialCarry credential_carry(const char* stored_host, uint16_t stored_port, bool stored_pass,\n"
                      "                                        const char* new_host, uint16_t new_port, bool pass_given)", tls)
        self.assertIn("const bool same_endpoint = !fresh && same_broker_host(stored_host, new_host) && stored_port == new_port;", tls)
        self.assertIn("c.refuse = !fresh && stored_pass && !pass_given;", tls)
        # And the WAP's counts the TLS mode and the CA, for the session alone.
        wap = (WAP / "csi_mqtt.cpp").read_text(encoding="utf-8")
        gate = body_after(wap, "static bool move_needs_password(const Config& before, const Config& after,", r"\n}\n")
        self.assertIn("if (bearer) return false;", gate)
        self.assertIn("if (before.tls_mode != after.tls_mode) return true;", gate)
        self.assertIn("return ca_in_body;", gate)


if __name__ == "__main__":
    unittest.main()
