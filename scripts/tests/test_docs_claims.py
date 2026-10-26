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
    # Retired by the owner-authority PR (sweep F333, F334, F346, F361): what
    # the landing page's session, or a code held to the camera, could still
    # do on the WAP after setup.
    ("The QR hub-provision path is the road that rule leaves open",
     "F333 closes that road where it opens: the boot scan asks the setup "
     "door's rule (keyless_join_open()), POST /api/wifi/qr-scan answers 404 "
     "after setup (F334), and Forget clears the stored broker login "
     "(csi_mqtt::login_clear())"),
    ("accepted as proximity-bound in the ledger's F289, with F333 keeping the "
     "smallest fix",
     "F333 is decided and done: the hub re-point stays outside the move gate "
     "on purpose, and the keyless joins reach a set-up unit only after the "
     "owner's bearer Forget (which clears the login) or the 10 s hold"),
    ("F334, still open, asks whether Wi-Fi changes after setup take a bearer",
     "F334 is done: after setup POST /api/wifi/connect, /ap-only, /disconnect "
     "and /forget take the bearer alone, on every interface"),
    ("a standalone unit with a camera keeps scanning for a Wi-Fi QR code "
     "after setup",
     "since F333 the boot scan runs only while no network is stored and the "
     "unit is not standalone"),
    ("needs the six digits the WAP shows in the PIN box and writes to the "
     "health log",
     "since F346 a Passkey Entry's digits reach only a page signed in with "
     "the API token and are no longer written to the health log"),
    ("a landing session can still reboot the unit",
     "since F361 POST /api/reboot and POST /api/safe-mode/retry take the "
     "bearer alone"),
    ("on the WAP any landing session can reboot the unit",
     "since F361 the WAP's reboot routes take the bearer alone, as the "
     "flagship's does"),
    ("owner's session: a reboot, or any other write a session may make",
     "any write a session may make, of which since F361 only the firmware "
     "install restarts the unit, and only into a newer signed image"),
    # Retired by that PR's review: a session's firmware install
    # (POST /api/ota/install, api_auth_check) still restarts the unit once a
    # newer signed release is published, so "no longer a reboot" was false.
    ("any write a session may make, which since F361 is no longer a reboot",
     "of a session's writes only the firmware install restarts the unit, "
     "and only into a newer signed image"),
    ("The session also cannot make a write that grants a credential, move "
     "the unit off its owner's network, or reboot it.",
     "the session cannot grant a credential or move the unit off its "
     "owner's network, and of its writes only the firmware install restarts "
     "the unit, which installs only a newer signed image"),
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
    # The owner-authority PR's review: the bearer is not the only authority
    # over the unit's restarts while the firmware install takes the session.
    ("docs/audit/repo_todo_sweep_2026-09.md",
     "it is the only post-setup authority over the network the WAP joins, "
     "its bonds, its vault key and its reboots, and",
     "its reboots, but for a session's firmware install, which restarts the "
     "unit only into a newer signed image"),
    ("CHANGELOG.md",
     "after setup the API token alone moves its network, reboots it, re-keys "
     "its vault and confirms a pairing",
     "the firmware install aside: a session may start it, and it restarts "
     "the unit only into a newer signed image"),
    ("CHANGELOG.md",
     "canary-wap: the API token alone reboots the unit (sweep F361).",
     "the same, the firmware install aside"),
    ("firmware/LESSONS_LEARNED.md",
     "every write that grants a credential (the vault key, the two confirms) "
     "or reboots the unit;",
     "the two reboot routes; the firmware install stays on the session and "
     "restarts the unit only into a newer signed image"),
    ("docs/audit/hardware_verification_checklist.md",
     "after setup the API token alone moves the network, reboots the unit, "
     "re-keys the vault and confirms a pairing",
     "the firmware install aside, which a session may start"),
    # The PR's second review: F344's copies of the blind-write sentence the
    # docs retired above (the sketch's comment, its README, the CHANGELOG's
    # F344 entry), and a CHANGELOG claim about every card that four cards
    # hold and three do not.
    ("firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino",
     "(a reboot, or any other write a session may make)",
     "any write a session may make, of which since F361 only the firmware "
     "install restarts the unit, and only into a newer signed image"),
    ("firmware/projects/canary-wap/arduino/canary_wap/README.md",
     "(a reboot, or any other write a session may make)",
     "the same"),
    ("CHANGELOG.md",
     "owner's session: a reboot, or any other write a session may make",
     "the same"),
    ("CHANGELOG.md",
     "every card whose action takes the token says so beforehand",
     "the Wi-Fi, vault key, reboot and pairing-QR cards say beforehand that "
     "their action takes the token; Retry full boot, Save my recovery kit, "
     "Start Pairing and the opera's Create/Join raise the sign-in when "
     "pressed, with no note on the card"),
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
    # The owner-authority PR's replacements.
    "The camera's QR hub re-point stays outside that rule on purpose (sweep "
    "F333): every code also chooses the network the unit joins",
    "Since F334 the pair token serves only the first-boot wizard: after setup "
    "the first two take the bearer alone and the scan's start answers `404`.",
    "pairs by Passkey Entry, which needs the six digits the WAP shows in the "
    "PIN box of a page signed in with the API token (since F346 no longer in "
    "the health log)",
    "Until F361 a landing session could reboot the unit at will",
    "Until F361 any landing session could reboot the WAP",
    "owner's session: any write a session may make, of which since F361 only "
    "the firmware install restarts the unit, and only into a newer signed "
    "image",
    "The session also cannot make a write that grants a credential or moves "
    "the unit off its owner's network, and of its writes only the firmware "
    "install restarts the unit",
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
    # The owner-authority PR review's replacements.
    "it is the only post-setup authority over the network the WAP joins, its "
    "bonds, its vault key and its reboots (a session's firmware install "
    "still restarts the unit, only into a newer signed image), and",
    "after setup the API token alone moves its network (a network joined "
    "before the update can stay saved), re-keys its vault, confirms a pairing "
    "and reboots it (the firmware install aside)",
    "canary-wap: the API token alone reboots the unit, the firmware install "
    "aside (sweep F361).",
    "every write that grants a credential (the vault key, the two confirms), "
    "and the two reboot routes;",
    "after setup the API token alone moves the network, re-keys the vault, "
    "confirms a pairing and, the firmware install aside, reboots the unit",
    # The second review's replacements.
    "it can still write blind with the owner's session (any write a session "
    "may make, of which since F361 only the firmware install restarts the "
    "unit, and only into a newer signed image)",
    "owner's session: any write a session may make, of which since F361 only "
    "the firmware install restarts the unit, and only into a newer signed "
    "image); a page",
    "the Wi-Fi, vault key, reboot and pairing-QR cards say beforehand that "
    "their action takes the token",
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
    # The owner-authority PR's retirements, as they stood (SECURITY_MODEL,
    # THREAT_MODEL).
    "(host-tested). The QR hub-provision path is the road that rule leaves open:\n"
    "a scanned hub code re-points the broker with the stored password, in a scan",
    "and the code has to be held in the camera's view: accepted as "
    "proximity-bound in the ledger's F289, with F333 keeping the smallest fix |",
    "its ten minutes. F334, still open, asks whether Wi-Fi changes after setup\n"
    "take a bearer, so that the pair token serves only the first-boot wizard; its",
    "ledger entry names connect and ap-only, not the scan. And a standalone unit\n"
    "with a camera keeps scanning for a Wi-Fi QR code after setup\n"
    "(`qr_auto_scan_tick()` runs whenever no Wi-Fi is saved)",
    "a keyboard-only initiator pairs by Passkey Entry, which needs the six "
    "digits the WAP shows in the PIN box and writes to the health log, in or "
    "out of pairing mode (filed in the 2026-09 sweep).",
    "deliberate denial from the LAN: a landing session can still reboot the unit\n"
    "(`POST /api/reboot`, `POST /api/safe-mode/retry`), and ARP spoofing, httpd's",
    "table lives in RAM, and on the WAP any landing session can reboot the unit\n"
    "(`POST /api/reboot`, `POST /api/safe-mode/retry`), which empties it: a",
    "requests still carry the session cookie and it can write blind with the\n"
    "owner's session: a reboot, or any other write a session may make); a page",
    # And the owner-authority PR's own overclaims, as its review found them.
    "requests still carry the session cookie and it can write blind with the\n"
    "owner's session: any write a session may make, which since F361 is no\n"
    "longer a reboot); a page",
    "The session also cannot make a write that grants a credential, move the\n"
    "unit off its owner's network, or reboot it. These routes take the bearer",
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
    "  set up with is its token for life. After F334, F345, F346 and F361 it is the\n"
    "  only post-setup authority over the network the WAP joins, its bonds, its\n"
    "  vault key and its reboots, and the setup door's winner after the owner's own",
    "### canary-wap: after setup the API token alone moves its network, reboots it, "
    "re-keys its vault and confirms a pairing, and the sign-in checks",
    "- **canary-wap: the API token alone reboots the unit (sweep F361).** `POST",
    "  every change to the network the unit joins, on every interface, and every\n"
    "  write that grants a credential (the vault key, the two confirms) or\n"
    "  reboots the unit; `POST /api/wifi/qr-scan` answers `404` outside setup.",
    "## canary-wap: after setup the API token alone moves the network, reboots "
    "the unit, re-keys the vault and confirms a pairing (F334, F345",
    # The second review's, as they stood.
    "// requests; in one that does not, it can still write blind with the owner's\n"
    "// session (a reboot, or any other write a session may make). F15 narrows",
    "so in one that does not it can still write blind with the owner's session "
    "(a reboot, or any other write a session may make). A page served over https",
    "  requests still carry the session cookie and it can write blind with the\n"
    "  owner's session: a reboot, or any other write a session may make); a page",
    "  nothing on top of it; every card whose action takes the token says so\n"
    "  beforehand (\"… takes the API token: paste it (it is in your recovery kit),",
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
    def test_the_scan_route_answers_only_during_setup_and_forget_drops_the_login(self):
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
        # So the session buys a pair token, and since sweep F334 POST
        # /api/wifi/qr-scan answers 404 outside first-boot setup before it
        # reads that token: its first statement after the request count is
        # the wizard check, and it names no cookie gate of its own.
        ino = (WAP / "canary_wap.ino").read_text(encoding="utf-8")
        self.assertIn('.uri = "/api/wifi/qr-scan", .method = HTTP_POST,   .handler = handle_qr_scan_start', ino)
        start = body_after(ino, "static esp_err_t handle_qr_scan_start(httpd_req_t* req) {", r"\nstatic esp_err_t ")
        code = [ln.strip() for ln in start.splitlines()[1:] if ln.strip() and not ln.strip().startswith("//")]
        self.assertEqual(code[0], "g_health.http_requests++;")
        self.assertEqual(code[1], "if (!setup_wizard::is_active()) {")
        self.assertEqual(code[2], 'httpd_resp_set_status(req, "404 Not Found");')
        self.assertLess(start.index('"404 Not Found"'), start.index("csi_integration::pair_token_valid(token)"))
        self.assertLess(start.index('"404 Not Found"'), start.index("g_qr_auto_next_ms = millis() + 15000;"))
        for cookie_gate in ("cv_session_validate", "session_validate_cookie", "CSI_AUTH_OR_RETURN"):
            self.assertNotIn(cookie_gate, start)
        # The scan task's hub-code branch re-points the broker and keeps the
        # row's login (outside move_needs_password() on purpose, F333/F348),
        # and re-checks the token only for a Wi-Fi-only code.
        self.assertIn("prov.wifi_only && !g_qr_auto_scan &&\n"
                      "                   !csi_integration::pair_token_valid(g_qr_scan_token)", ino)
        hub = body_after(ino, "if (!prov.wifi_only && prov.host[0]) {", r"\n          }\n")
        self.assertIn("strlcpy(mc.host, prov.host, sizeof(mc.host));", hub)
        self.assertIn("mc.port = prov.port;", hub)
        self.assertIn("csi_mqtt::config_save(mc)", hub)
        self.assertIn("csi_mqtt::request_reinit()", hub)
        self.assertNotIn("mc.pass", hub)
        # The boot scan asks the setup door's rule first: no scan on a
        # standalone unit (F333).
        tick = body_after(ino, "static void qr_auto_scan_tick(uint32_t now) {", r"\n}\n")
        first = [ln.strip() for ln in tick.splitlines()[1:] if ln.strip()][0]
        self.assertEqual(first, "if (!provisioning_logic::keyless_join_open(g_wifi_creds.configured, g_wifi_ap_only)) return;")
        door = body_after(ino, "bool ble_improv_door_should_open() {", r"\n}\n")
        self.assertIn("provisioning_logic::keyless_join_open(", door)
        # Forget takes the bearer alone (F345), and after the networks it
        # clears the broker login and asks for the re-init when that held
        # (F333); Disconnect takes the bearer too (F334).
        forget = body_after(ino, "static esp_err_t handle_wifi_forget(httpd_req_t* req) {", r"\n}\n")
        wipe = forget.index("wifi_clear_credentials();")
        clear = forget.index("const bool login_cleared = csi_mqtt::login_clear();")
        self.assertLess(wipe, clear)
        self.assertLess(clear, forget.index("(void)csi_mqtt::request_reinit();"))
        self.assertIn('doc["broker_login_cleared"] = login_cleared;', forget)
        for wrapper in ("handle_wifi_forget_auth", "handle_wifi_disconnect_auth"):
            body = body_after(ino, f"static esp_err_t {wrapper}(httpd_req_t* req) {{", r"\n}\n")
            self.assertIn("api_auth_check_bearer(req, g_device.api_token_str)", body)
            self.assertNotIn("api_auth_check(", body)

    def test_the_firmware_install_is_the_one_restart_a_session_makes(self):
        # Since F361 POST /api/reboot and /api/safe-mode/retry take the bearer
        # alone, but the dashboard's firmware install and its update settings
        # still open with api_auth_check(), which takes the session cookie;
        # the install task restarts the unit after a verified install, and
        # the decision installs only a version above the running one and the
        # NVS floor. So the security docs name that restart wherever they say
        # what a session cannot do (the PR's review: "no longer a reboot").
        ino = (WAP / "canary_wap.ino").read_text(encoding="utf-8")
        for handler in ("handle_ota_install", "handle_ota_config"):
            body = body_after(ino, f"static esp_err_t {handler}(httpd_req_t* req) {{", r"\n}\n")
            code = [ln.strip() for ln in body.splitlines()[1:] if ln.strip() and not ln.strip().startswith("//")]
            self.assertEqual(code[0], "if (!api_auth_check(req, g_device.api_token_str)) return ESP_OK;", handler)
        self.assertIn("securacv_ota_check_and_install();",
                      body_after(ino, "static esp_err_t handle_ota_install(httpd_req_t* req) {", r"\n}\n"))
        ota = (WAP / "securacv_ota.cpp").read_text(encoding="utf-8")
        self.assertIn("if (cmp > 0)  return SECURACV_OTA_DECISION_UPDATE;", ota)
        self.assertIn("esp_restart();", body_after(ota, "if (s_ctx.config.auto_reboot) {", r"\n    } else {"))
        sec = normalized((REPO / "docs/security/SECURITY_MODEL.md").read_text(encoding="utf-8"))
        threat = normalized((REPO / "docs/security/THREAT_MODEL.md").read_text(encoding="utf-8"))
        blind = ("any write a session may make, of which since F361 only the firmware "
                 "install restarts the unit, and only into a newer signed image")
        self.assertIn(blind, sec)
        self.assertIn(blind, threat)
        self.assertIn("and of its writes only the firmware install restarts the unit "
                      "(`POST /api/ota/install`, or automatic updates turned on with "
                      "`POST /api/ota/config`), which installs only a newer signed image.", sec)
        self.assertIn("a session's firmware install restarts the unit only into a newer "
                      "signed image", sec)
        self.assertIn("the dashboard's firmware install, which a session may start, "
                      "restarts the unit only into a newer signed image", threat)

    def test_a_network_saved_before_f334_stays_saved_and_the_docs_say_so(self):
        # F334's decision names a known limit: a network a session moved the
        # unit to before the update stays in the saved-networks ring, and the
        # unit rotates to it by itself; the remedy is a bearer Forget, then
        # setting the unit up again. The PR's second review found it written
        # nowhere while the docs said "no longer move". The code first: every
        # join is written into the ring ...
        ino = (WAP / "canary_wap.ino").read_text(encoding="utf-8")
        self.assertIn("wifi_store_note_joined(g_wifi_creds.ssid, g_wifi_creds.password);", ino)
        note = body_after(ino, "static void wifi_store_note_joined(const char* ssid, const char* pass) {", r"\n}\n")
        self.assertIn("canary::net::wifi_store::add(g_wifi_store, ssid, pass)", note)
        # ... the FAILED retry rotates to the next saved network before it
        # rejoins, asking nobody: two NotFound reads and a ring of two ...
        retry = body_after(ino, "    case WIFI_PROV_FAILED:\n", r"\n      break;\n")
        self.assertLess(retry.index("wifi_store_rotate_if_due();"), retry.index("wifi_connect_to_home();"))
        rotate = body_after(ino, "static void wifi_store_rotate_if_due() {", r"\n}\n")
        first = [ln.strip() for ln in rotate.splitlines()[1:] if ln.strip()][0]
        self.assertEqual(first, "if (g_improv_join_pending || g_wifi_notfound_streak < 2 || g_wifi_store.count < 2) return;")
        self.assertIn("wifi_store_adopt(next);", rotate)
        self.assertNotIn("api_auth", rotate)
        # ... no route removes one saved network (the API serves only the
        # count), and Forget wipes the whole ring.
        self.assertNotIn("wifi_store::remove(", ino)
        self.assertIn("ns.saved_networks = g_wifi_store.count;", ino)
        clear = body_after(ino, "static bool wifi_clear_credentials() {", r"\n}\n")
        self.assertIn("nvs->remove(canary::net::wifi_store::NVS_KEY);", clear)
        self.assertIn("canary::net::wifi_store::init(g_wifi_store);", clear)
        self.assertIn("wifi_clear_credentials();",
                      body_after(ino, "static esp_err_t handle_wifi_forget(httpd_req_t* req) {", r"\n}\n"))
        # The docs say so where they say the session no longer moves it.
        sec = normalized((REPO / "docs/security/SECURITY_MODEL.md").read_text(encoding="utf-8"))
        threat = normalized((REPO / "docs/security/THREAT_MODEL.md").read_text(encoding="utf-8"))
        log = normalized((REPO / "CHANGELOG.md").read_text(encoding="utf-8"))
        rotates = ("when the current network is not found twice running the unit tries "
                   "the next saved one by itself (`wifi_store_rotate_if_due()`), with no "
                   "request and no bearer")
        for doc in (sec, threat):
            self.assertIn("A network a session moved it to before F334 can stay among its "
                          "saved networks", doc)
            self.assertIn(rotates, doc)
            self.assertIn("the remedy is a bearer Forget, which wipes the ring", doc)
        self.assertIn("After updating: a network a session moved the unit to before this "
                      "release can stay among its saved networks", log)

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
