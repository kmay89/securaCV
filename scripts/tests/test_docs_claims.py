#!/usr/bin/env python3
"""The security docs may not carry a retired claim — a banned-claims gate.

docs/security/SECURITY_MODEL.md ships in every evidence export, and for a
while it promised things the firmware did not do: zero outbound connections
(the networked products open a broker socket, a daily signed-manifest check
and SNTP), TLS on every hop (the flagship's release images serve plain HTTP on
port 80, its dev and full builds serve HTTPS only from the first boot after
setup, and the WAP's HTTPS is an after-setup opt-in), Bluetooth compiled out
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
     "full builds serve HTTPS from the first boot after setup, CI-compiled); "
     "the WAP serves HTTPS after setup and falls back to HTTP when it fails "
     "to start"),
    ("displayed only via physical button press",
     "one BOOT tap hands the flagship's token to one page load, but it also "
     "goes with no press to the first-boot wizard, and after setup to a "
     "bearer caller and a request over the flagship's own access point "
     "(provisioning_gate.h page_token_policy); the WAP's landing page mints "
     "a one-tap session"),
    ("TLS is required for all API access (no HTTP fallback)",
     "plain HTTP is a stated posture: setup mode and start failure on the "
     "WAP, the flagship's release images (its dev and full builds redirect "
     "port 80 to HTTPS from the first boot after setup), the displays' LAN "
     "page"),
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
)

# Strings that must never trip the gate: the true prose that replaced the
# claims shares words with it, and the gate is worthless if it blocks the
# correction.
MUST_PASS = [
    "the broker socket is plain by default until you provision TLS",
    "BLE (NimBLE) is compiled into the shipped Canary WAP build",
    "the MINIMAL profile compiles it out; the code is not compiled in there",
    "a release image serves plain HTTP on port 80",
    "the WAP serves HTTPS on port 443 once setup has completed",
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
]


def normalized(text: str) -> str:
    """Collapse every whitespace run to one space so a wrapped sentence matches."""
    return re.sub(r"\s+", " ", text)


def offenders_in(text: str) -> list[str]:
    flat = normalized(text)
    return [needle for needle, _why in BANNED if needle in flat]


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


if __name__ == "__main__":
    unittest.main()
