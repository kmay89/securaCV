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
sentence is a red test, not a silent hole. The file-scoped rows (RETIRED_IN)
are guarded per file: MUST_FAIL_IN and MUST_PASS_IN name the file each form
and each replacement stood in, every form must match exactly one row of its
own file, every row must have a form there, and every replacement must stand
in its file, so a row deleted beside a sibling that shares its needle, or
pointed at a file that never held the sentence, fails instead of passing on
text it never scanned.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_docs_claims.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import json
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
    # Retired by #1843's fourth review: a tier qualifier that told a reader
    # the secure tier hides the key. Flash encryption does not cover NVS, and
    # NVS encryption is not available under framework = arduino, which every
    # released tree builds with.
    ("flash encryption is an opt-in tier, so on a default Canary the key is "
     "recoverable by reading the flash directly",
     "the key is plaintext in NVS on every tier today, on a fused board too "
     "(THREAT_MODEL.md Scenario 4)"),
    ("Flash encryption (data at rest protected)",
     "flash encryption protects the app image at rest, not NVS, where the "
     "keys and the saved passwords stay plaintext on every tier today"),
    ("on a Canary in its default configuration, someone who takes the device "
     "away and opens it up can read the private key out of the flash chip",
     "the private key is readable off the flash chip in the default "
     "configuration and, today, on a board with flash encryption fused too"),
    ("On a device in its default configuration this includes recovering the "
     "device's private key",
     "on every released firmware tree today, on every tier: the key is "
     "plaintext in NVS on a fused board too"),
    # Retired by #1843's fourth review: the owner's browser moves the WAP to
    # a network the striking host is not on, but the WAP keeps that host's
    # network saved and goes back to it when the new one is not found twice
    # running (wifi_store_rotate_if_due); only Forget Wi-Fi or the hold drop it.
    ("The hold helps only with a setup onto a network that host is not on, "
     "which the owner's browser does without it",
     "the browser's move keeps the host's network saved; the hold, or Forget "
     "Wi-Fi, followed by that setup drops it, and reopens the setup door"),
    ("Once A65 is settled the lane would move the WAP with no browser, but "
     "only while that host leaves it alone, and taking it away needs no radio.",
     "the lane would hold only while that host leaves it alone and the new "
     "network stays up (it saves through the same paths as the browser)"),
    # Retired by #1843's fifth review: the copies the fourth review's "every
    # copy" left, which still tied the key's exposure to the default tier or
    # offered the opt-in tiers as the answer to seizure. Burning their fuses
    # protects the firmware image today, not the key: NVS stays plaintext.
    ("on a default-configured Canary they can recover the key",
     "today they can recover it from any Canary, a board with flash "
     "encryption fused included: NVS encryption is in no released build"),
    ("this is the case the opt-in tiers exist for",
     "the opt-in tiers buy the firmware image's protection today, not the "
     "key's; the defenses against a seizer are the anchored chain, a "
     "clone's divergent chain and a device kept out of reach"),
    ("understand that you are trading recoverability for extraction "
     "resistance",
     "burning the fuses trades recoverability for no key secrecy today: the "
     "key, the saved passwords and the bonds stay plaintext in NVS on a "
     "fused board"),
    # #1843's sweep for every remaining copy: the reference's own unqualified
    # sentences about a served tap, the opt-in tier's Secure Boot credited as
    # working, the secure env called a flash-encryption image, and the
    # flagship's 5 s reset.
    ("ends that address's backoff as a right bearer would (sweep F341)",
     "the tap ends that address's own backoff, never the shared overflow's "
     "(auth_handed_token frees only its own slot)"),
    ("that serves ends that address's backoff, as a right bearer would",
     "the claim or the tap ends that address's own backoff, never the shared "
     "overflow's: an address held only through the overflow stays held"),
    ("Secure boot prevents firmware replacement",
     "Secure Boot is prepared in the provisioning kit, not enabled on any tier"),
    ("Secure boot (firmware integrity verified)",
     "the same"),
    ("Refuse to boot **on the opt-in secure-provisioning tier only**",
     "no tier refuses a well-formed unsigned image today; every release "
     "image relies on the OTA signature check"),
    ("Secure Boot v2 + Flash Encryption env; compile-only in CI",
     "the secure env enables neither and refuses the key on every board"),
    ("Factory reset via BOOT button | ✅ (short/long press) | ❌ | ✅ (5s hold)",
     "the WAP sketch resets at 10 s; the flagship resets at 10 s (key kept) and "
     "wipes at 30 s"),
    # #1843's sweep, third pass (its review round 2): the kit's Secure Boot called off or on,
    # the OTA check credited to every tier, a full erase called the only way
    # to a new identity, and un-fused meaning flash encryption off.
    ('they are **off on every Canary today**',
     'neither works: Phase 2 leaves flash encryption off and Secure Boot enabled with nothing to verify against'),
    ('leaves the irreversible lockdown as an explicit, key-backup-enforced opt-in',
     'the design calls for the key-backup gate (hardware_root_of_trust.md §5.2); the kit checks only that the key files exist'),
    ('so neither flash encryption nor Secure Boot is on after it',
     'flash encryption is off and Secure Boot is enabled with nothing to verify against'),
    ("so every tier relies on the OTA path's SHA-256 and Ed25519 release-signature check",
     'every release image does; a flagship image built without SECURACV_BUILD_RELEASE also takes an unsigned image over its bearer-gated POST /api/ota'),
    ('a full erase in either flasher is what removes',
     'an install in either flasher rewrites the settings region too'),
    ('(a clean install, a full erase in either flasher, is its way to a new identity)',
     'an install in either flasher, with or without the erase, starts NVS over'),
    ('so a clean install (full erase) in either flasher is its way to a new identity on purpose',
     'the same'),
    ('a seizer of a fused board who reads its flash after the hold has the mesh secret',
     'the FE gate writes the secret only on a board with flash encryption on, which a Phase 2 board is not'),
    ('keeps the secret off un-fused boards',
     'off every board without flash encryption, a Phase 2 board included'),
    ('the FE gate keeps it off un-fused boards',
     'the same'),
    ('an un-fused board keeps the ring in RAM only',
     'the same'),
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
    # #1843's fourth review: the same two retirements as BANNED's, where the
    # spec and the design doc carried them, and the lane's hold-time in the
    # three copies of the F342 posture that left out the saved networks.
    ("spec/beacon_channel_v0.md",
     "readable from flash on a device without flash encryption",
     "readable from flash on every tier today, since it is plaintext in NVS "
     "(THREAT_MODEL.md Scenario 4)"),
    ("docs/design/magic_pairing.md",
     "The hold helps only with a setup onto a network that host is not on, "
     "which the owner's browser does without it",
     "the browser's move keeps the host's network saved; the hold, or Forget "
     "Wi-Fi, followed by that setup drops it, and reopens the setup door"),
    ("docs/design/magic_pairing.md",
     "Once A65 is settled the lane would move the WAP with no browser, but "
     "only while that host leaves it alone, and taking it away needs no radio.",
     "the same, while the new network stays up"),
    ("CHANGELOG.md",
     "Once that is settled the lane holds only while that host leaves it "
     "alone, and taking it away needs no radio",
     "only while that host leaves it alone and the new network stays up"),
    ("docs/security/SECURITY-AUDIT.md",
     "Once that is settled the lane holds only while that host leaves it "
     "alone, and taking it away needs no radio",
     "the same"),
    ("docs/audit/esp32s3_firmware_full_audit_2026-07.md",
     "Once that is settled the lane holds only while that host leaves it "
     "alone, and taking it away needs no radio",
     "the same"),
    ("firmware/LESSONS_LEARNED.md",
     "under the lockout), while that host leaves it alone (the next three "
     "bullets).",
     "while that host leaves it alone and the new network stays up (the next "
     "four bullets)"),
    # #1843's fifth review: the same retirement in the design docs, the
    # bench runbook and the in-browser flasher's trust card, which told an
    # owner that flash encryption protects the key, the saved Wi-Fi or the
    # mesh secret at rest.
    ("docs/design/hardware_root_of_trust.md",
     "Flash encryption defends key-at-rest and casual readout, not "
     "decapping/fault-injection by a well-funded lab.",
     "flash encryption with NVS encryption (Tier 3) would; flash encryption "
     "alone defends the app image only"),
    ("docs/design/hardware_root_of_trust.md",
     "Dev mode is the sweet spot: it protects WiFi/keys at rest from a thief "
     "while the device stays field-recoverable.",
     "with NVS encryption beside it, it would; until then it protects the app "
     "image only"),
    ("docs/design/vault_operator_ux_v1_1.md",
     "Secure it (protect the key at rest) — already tooled, not yet burned.",
     "the fuses are tooled; the key's protection needs NVS encryption, which "
     "no released build can turn on under framework = arduino"),
    ("docs/design/vault_operator_ux_v1_1.md",
     "Keeps the entire existing signing scheme; the key can't be dumped off a "
     "locked device.",
     "once NVS encryption is in the build it can't; until then it can"),
    ("docs/V1_BENCH_TEST_RUNBOOK.md",
     "the `opera_secret` in NVS is only confidential under FE",
     "the firmware writes it only with FE on (audit O2), and FE does not make "
     "it confidential: NVS is plaintext on a fused board too"),
    ("canary-local/assets/flash.js",
     "(then the flash reads back as ciphertext)",
     "the firmware image does; the identity key and the saved WiFi, in NVS, "
     "read back in the clear from a fused board too"),
    ("canary-local/assets/flash.js",
     "For a board deployed somewhere physically exposed, use Phase-2 secure "
     "provisioning (flash encryption + eFuse).",
     "put it out of reach: Phase-2 provisioning does not yet protect its key "
     "or saved WiFi"),
    # #1843's fifth review: a factory reset offered as the whole fix for a
    # WAP whose setup door a stranger took. The stranger chose the network,
    # so it can spend the claim and hold the token and the SoftAP password,
    # which the hold and Forget Wi-Fi keep; and either reopens the door.
    ("docs/design/magic_pairing.md",
     "an owner who finds the unit on the wrong network holds it, "
     "factory-resets it, and starts again.",
     "on a WAP the reset keeps the token the stranger's claim bought and "
     "reopens the first-come door (ledger F345)"),
    ("docs/design/magic_pairing.md",
     "a unit joined to the wrong network is a unit its owner holds, not one "
     "that has been taken; the fix is a factory reset.",
     "the same"),
    # #1843's fifth review: Forget Wi-Fi drops every saved network, and the
    # door it reopens is first-come, behind a saved Bluetooth switch.
    ("docs/FAQ.md",
     "which clears the stored network and opens the door again for half an "
     "hour from that moment.",
     "which clears every saved network and opens the door to the first phone "
     "or computer nearby that connects, once Bluetooth Enabled is on"),
    # #1843's sweep for every remaining copy: a tap or a claim that serves an
    # address frees that address's own slot, never the shared overflow
    # (auth_backoff.h record_success; auth_handed_token), and the F343 ledger
    # notes that named the iPhone's lane as the way out carry their
    # correction pointers (a needle that runs a retired sentence into what
    # followed it fires only if the pointer is taken out).
    ("firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino",
     "and the address they serve has its backoff cleared",
     "their serve frees that address's own slot, never the shared overflow "
     "(auth_backoff.h record_success)"),
    ("CHANGELOG.md",
     "a tap clears an address only when it is served to that address's ask; "
     "under that host the hold is no way out",
     "the F341 entry covers both trees: under that host the hold is no way out "
     "unless the unit is then set up on a network that host is not on, and the "
     "flagship's tap goes to the first asker (F354)"),
    ("firmware/canary/CONSOLIDATION.md",
     "ends that address's backoff through `auth_handed_token()`",
     "auth_handed_token() frees the address's own slot, never the shared "
     "overflow"),
    ("firmware/canary/lib/securacv_network/src/securacv_network.cpp",
     "token ends its address's backoff (auth_handed_token, sweep F341), so a",
     "its own backoff, never the shared overflow's"),
    ("docs/audit/repo_todo_sweep_2026-09.md",
     "set the unit up on a network that host is not on. "
     "`test_auth_backoff.cpp`'s overflow row",
     "the F343 third-round note keeps its correction pointer: the iPhone app "
     "never reaches a WAP's lane (A65), so the WAP's way out is the owner's "
     "browser"),
    ("docs/audit/repo_todo_sweep_2026-09.md",
     "the bonded lane gets the owner out only while that host holds no bond of "
     "its own. Any landing session",
     "the first-review note keeps its correction pointer: radio range is no "
     "bound and the iPhone app never reaches the lane (A65)"),
    ("docs/audit/repo_todo_sweep_2026-09.md",
     "the way out F343's posture names from under a host striking",
     "F343's posture then named the lane; the way out is the owner's browser, "
     "and the lane is no way out until A65 is settled"),
    # The same sweep: the 10 s BOOT hold keeps the identity key, the chain
    # and the HTTPS certificate on every Canary, and the token on a WAP
    # (factory_reset_by_hold(); reset_scope_keys.h kIdentityKept; the
    # flagship's hold removes its stored token and the next boot derives one
    # from the key, so a flasher-seeded token does not come back). The
    # flagship's 30 s wipe, and an install or a full erase in either flasher
    # (whose factory image starts NVS over), change them, and a
    # WAP's setup network comes up with no button while it is off its
    # Wi-Fi.
    ("docs/getting_started_canary.md",
     "You *can* lose your witness chain by factory-resetting it",
     "the 10 s hold keeps the key and the chain; an install or a full erase "
     "in a flasher (or the flagship's 30 s wipe) loses them"),
    ("docs/getting_started_canary.md",
     "export the chain before you factory-reset",
     "export it before a reinstall or erase in a flasher, or the flagship's "
     "30 s wipe"),
    ("docs/getting_started_canary.md",
     "factory reset makes a new certificate, so pair the iPhone app again",
     "the 10 s reset keeps the certificate; the 30 s wipe or an install or a "
     "full erase in a flasher makes a new one"),
    ("firmware/canary/include/canary_config.h",
     "(factory reset erases both,",
     "reset_scope_keys.h kIdentityKept keeps tls_cert and tls_key on the 10 s "
     "reset"),
    ("firmware/canary/scripts/ota_deploy.py",
     "A factory reset makes a new certificate;",
     "the 30 s wipe, or an install or a full erase in a flasher, makes a new "
     "certificate"),
    ("firmware/canary/scripts/ota_deploy.sh",
     "A factory reset makes a new certificate;",
     "the same"),
    ("firmware/canary/src/main.cpp",
     "5s=factory reset",
     "the flagship warns at 3 s, resets at 10 s (key kept) and wipes at 30 s"),
    ("ios/Sources/SecuraCV/Transport/DeviceAPI.swift",
     "If you reset or replaced it, re-pair it",
     "a 10-second reset keeps the certificate, so after one a mismatch means "
     "something else is answering"),
    ("ios/Sources/SecuraCV/Security/DeviceStore.swift",
     "a factory-reset device with a new key",
     "a factory reset keeps the key; an install or a full erase in a flasher "
     "gives a new one"),
    ("ios/Sources/SecuraCV/Alerts/HeadWatch.swift",
     "means the chain restarted (a wipe, a reset)",
     "the 10 s reset keeps the chain; an install or a full erase in a "
     "flasher, the flagship's 30 s wipe or a replaced board restarts it"),
    ("ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift",
     "the BOOT-button portal can still rescue",
     "holding a WAP's BOOT is its factory reset; its own setup network comes up "
     "while it is off its Wi-Fi"),
    ("ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift",
     "(hold BOOT) will take the correction",
     "the same"),
    ("ios/Sources/SecuraCV/Transport/BLEConsole.swift",
     "use its setup portal (hold BOOT) instead",
     "the same"),
    ("custom_components/securacv/__init__.py",
     "identity for this device_id (fresh flash, factory reset, a",
     "the 10 s reset keeps the key; an install or a full erase in a flasher, "
     "the flagship's 30 s wipe or a replacement board gives a new one"),
    ("custom_components/securacv/__init__.py",
     "starts its counters over (fresh flash, factory",
     "the same"),
    ("custom_components/securacv/sensor.py",
     "which is what a factory reset looks like from here",
     "a re-pin is what an install or a full erase looks like; the 10 s reset "
     "keeps the key and its counters"),
    ("custom_components/securacv/tests/test_replay_reset_paths.py",
     "a factory-reset or replaced Canary starts them over",
     "an erased or replaced Canary does; the 10 s reset keeps them"),
    ("custom_components/securacv/tests/test_replay_and_device_id.py",
     "# Factory reset: the device pins fresh",
     "a full erase pins fresh; a factory reset keeps the key"),
    ("firmware/common/network/auth_backoff.h",
     "polling a reset unit with its old token",
     "the 10 s hold leaves the token as it was; an install or a full erase in "
     "a flasher changes it"),
    ("firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h",
     "polling a reset unit with its old token",
     "the same (the staged copy)"),
    ("firmware/LESSONS_LEARNED.md",
     "polling a reset unit with its old token",
     "the same"),
    ("CHANGELOG.md",
     "still polling a reset unit with its old token (filed as F352)",
     "the same"),
    ("docs/design/magic_pairing.md",
     "a pairing for a unit that was reset keeps polling with the old token",
     "the hold and Forget Wi-Fi keep the token; only an install or a full "
     "erase in a flasher leaves a stale pairing striking"),
    ("docs/design/magic_pairing.md",
     "| No credentials stored (first boot, after a factory reset), within **30 "
     "minutes** of power-on | **Open** (`Door::NoCredentials`) | A device "
     "nobody owns yet. The window",
     "after a reset the unit still has an owner, and on a WAP the door goes to "
     "whichever client connects first (F345)"),
    ("docs/audit/repo_todo_sweep_2026-09.md",
     "polling a reset unit with its old token. What it is now.",
     "the F343 Done note keeps its amendment: a re-flashed unit"),
    ("docs/audit/repo_todo_sweep_2026-09.md",
     "(iOS, a reset unit's stale pairing).",
     "the same"),
    ("docs/audit/repo_todo_sweep_2026-09.md",
     "A factory reset that regenerates the WAP's key changes both its token and "
     "its `device_id`.",
     "no factory reset regenerates the WAP's key; an install or a full erase in "
     "a flasher does"),
    ("docs/audit/repo_todo_sweep_2026-09.md",
     "a pairing for a reset Canary keeps polling",
     "the same"),
    ("docs/audit/repo_todo_sweep_2026-09.md",
     "the canary-wap half is unchanged. No behavior changed; not run on a "
     "device. - [x] **F83",
     "F82's amendment keeps its canary-wap note: the WAP has the 10 s hold, "
     "which keeps the floor"),
    ("firmware/common/csi/src/csi_event_id_floor.h",
     "stranger. A canary-wap has no reset path",
     "the canary-wap has the 10 s hold, which keeps ev.next and csi.evsent"),
    ("firmware/projects/canary-wap/arduino/canary_wap/csi_event_id_floor.h",
     "stranger. A canary-wap has no reset path",
     "the same (the staged copy)"),
    ("docs/csi_developer_api.md",
     "a canary-wap has no reset path, so its owner flashes it",
     "the same"),
    ("CHANGELOG.md",
     "a factory reset on a canary, a full flash erase on a canary-wap",
     "the canary's 10 s reset keeps the floor; only its 30 s wipe clears it"),
    ("canary-local/tools/gen_flash.py",
     "A factory reset and an install in one pass.",
     "a chip erase removes the identity key; the factory reset keeps it"),
    ("canary-local/devices/flash.json",
     "A factory reset and an install in one pass.",
     "the same (generated)"),
    ("firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md",
     "(BOOT held ≥ 3 s is the only path)",
     "the reset fires at 10 s and keeps the identity, the chain and the token"),
    ("firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md",
     "never be able to wipe a witness",
     "the hold wipes no witness; it reopens the setup door"),
    ("docs/design/onboarding_shared_module.md",
     "the 5 s factory-reset hold",
     "the factory-reset hold is 10 s on every board"),
    ("firmware/canary/src/csi_event_log.h",
     "a factory reset) is left untouched and unused",
     "the 10 s reset keeps the key the owner file names"),
    ("firmware/canary/lib/securacv_mesh/src/mesh_state.h",
     "Used on factory reset and",
     "no reset calls it; POST /api/mesh/leave does"),
    ("firmware/projects/canary-wap/arduino/canary_wap/web_ui.h",
     "Remove saved WiFi credentials from this Canary?",
     "Forget Wi-Fi drops every saved network and reopens the first-come "
     "Bluetooth setup door"),
    ("firmware/common/network/improv_core.h",
     "NO CREDENTIALS STORED (first boot, or after a factory reset): the",
     "after a reset or Forget Wi-Fi the unit still has an owner (F345)"),
    ("firmware/projects/canary-wap/arduino/canary_wap/improv_core.h",
     "NO CREDENTIALS STORED (first boot, or after a factory reset): the",
     "the same (the staged copy)"),
    ("firmware/projects/canary-wap/arduino/canary_wap/ble_improv.h",
     "for a WAP nobody owns yet: while no credentials are stored",
     "the hold and Forget Wi-Fi keep the identity and the token, so a reset WAP "
     "still has an owner"),
    # The same sweep: NVS is plaintext on every tier (flash encryption does
    # not cover it, and NVS encryption is not available under
    # framework = arduino), the flash-encryption gate keeps a secret off
    # un-fused boards without making it confidential, and the secure kit
    # enables neither Secure Boot nor flash encryption as tooled.
    ("firmware/canary/lib/securacv_webui/src/securacv_webui.cpp",
     "Paired devices are stored securely and",
     "the bonds are plaintext NVS on every build"),
    ("canary-local/assets/intake.js",
     "Reads come back as ciphertext and a plaintext image won't boot.",
     "the firmware image and flagged partitions read back as ciphertext; NVS "
     "does not"),
    ("desktop/src-tauri/src/efuse.rs",
     "Reads come back as ciphertext and a plaintext image won't boot.",
     "the same (two flashers)"),
    ("firmware/projects/canary-wap/arduino/canary_wap/household.h",
     "(flash-encrypted at rest on ESP32 with secure",
     "the IRKs are plaintext NVS on every build"),
    ("docs/design/household_presence_and_posture.md",
     "Its comment says the IRKs are",
     "household.h's comment no longer says so"),
    ("docs/research/face_id_without_faces.md",
     "`household.h`'s claim that IRKs are \"flash-encrypted at rest on ESP32 "
     "with secure boot\" is wrong",
     "the same"),
    ("firmware/projects/canary-wap/arduino/canary_wap/mesh_network.cpp",
     "On an unencrypted flash, a physical-access attacker",
     "a fused board's NVS is plaintext too; the gate keeps the secret off "
     "un-fused boards only"),
    ("firmware/canary/lib/securacv_ble_scan/src/ble_scan.h",
     "(under flash-encryption gate, mirroring opera_secret's protection)",
     "ble_scout_key.cpp has no gate, and the opera_secret gate is no protection "
     "at rest"),
    ("firmware/projects/canary-wap/arduino/canary_wap/ble_scan.h",
     "(under flash-encryption gate, mirroring opera_secret's protection)",
     "the same (the staged copy)"),
    ("firmware/canary/lib/securacv_ble_scan/src/ble_scout_key.h",
     "(mirroring opera_secret's hygiene — flash-encryption gate is the",
     "no layer gates the scout key, and flash encryption does not cover NVS"),
    ("firmware/projects/canary-wap/arduino/canary_wap/ble_scout_key.h",
     "(mirroring opera_secret's hygiene — flash-encryption gate is the",
     "the same (the staged copy)"),
    ("firmware/scripts/regression_check.sh",
     "must never be persisted to unencrypted NVS (#610)",
     "the gate keeps the secret off un-fused boards; it does not encrypt NVS"),
    ("firmware/common/network/wifi_store.h",
     "and the secure kit in firmware/provisioning/ is the path up",
     "the path is NVS encryption; the kit's flash encryption does not cover NVS"),
    ("firmware/projects/canary-wap/arduino/canary_wap/wifi_store.h",
     "and the secure kit in firmware/provisioning/ is the path up",
     "the same (the staged copy)"),
    ("firmware/projects/canary-display/arduino/canary_display/wifi_store.h",
     "and the secure kit in firmware/provisioning/ is the path up",
     "the same (the staged copy)"),
    ("firmware/projects/canary-sense/src/main.cpp",
     "the secure kit is the path up.",
     "the same"),
    ("firmware/projects/canary-sentinel/src/main.cpp",
     "the secure kit is the path up.",
     "the same"),
    ("firmware/projects/canary-vision/src/main.cpp",
     "the secure kit is the path up.",
     "the same"),
    ("docs/PARITY.md",
     "NVS encryption or flash encryption on the credential blobs",
     "flash encryption alone does not cover NVS"),
    ("docs/PARITY.md",
     "Secure Boot v2) stays the documented path",
     "the path is NVS encryption; the secure kit does not encrypt NVS"),
    ("docs/FIRMWARE_MAP.md",
     "that no CI leg builds. NVS encryption is not configurable",
     "the snapshot keeps its amendment: firmware.yml compiles both envs"),
    ("docs/FIRMWARE_MAP.md",
     "the secure kit documented as the path; no fake encryption",
     "the gap row keeps its amendment: the path is NVS encryption"),
    ("docs/FIRMWARE_MAP.md",
     "and the secure kit is the documented path (PARITY footnote 1)",
     "the path is NVS encryption"),
    ("docs/design/hardware_root_of_trust.md",
     "Without flash encryption or a hardware-bound key, physical possession = "
     "key compromise, which",
     "without NVS encryption a fused board's key is compromised by possession "
     "too"),
    ("docs/design/hardware_root_of_trust.md",
     "and flash reads come back ciphertext",
     "the app image reads back as ciphertext; NVS does not"),
    ("docs/design/hardware_root_of_trust.md",
     "Tier 3's non-extractable key (§5.4) additionally stops",
     "no tier stops it today; the DS key is a separate, optional attestation key"),
    ("docs/design/hardware_root_of_trust.md",
     "Protects at-rest, **not** a live chip.",
     "it protects the app image and flagged partitions, not NVS"),
    ("docs/design/vault_operator_ux_v1_1.md",
     "eFuse / flash-encryption / DS peripheral",
     "flash encryption alone does not cover NVS"),
    ("docs/design/vault_operator_ux_v1_1.md",
     "path **(a)** hardens the ESP32-S3 Ed25519 identity *at rest* by burning",
     "burning Phase 2 alone leaves the key plaintext in NVS"),
    ("docs/design/apple_home_integration.md",
     "Pairings persist (encrypted NVS / hub keystore)",
     "no Canary build has encrypted NVS"),
    ("docs/design/apple_home_integration.md",
     "refuse to persist on flash-encryption-off devices (the tiered story from",
     "the gate keeps the keys off un-fused boards; it does not make them "
     "confidential"),
    ("docs/hardware/tv_display_design.md",
     "relies on the flash-encrypted secure build",
     "no display env uses the secure sdkconfig, and the broker credentials are "
     "plaintext"),
    ("docs/research/bitchat_protocol_review.md",
     "Device seizure is already handled by flash encryption",
     "no tier stops a seizer reading NVS"),
    ("docs/strategy/12-engineering-foundations-flight-rules.md",
     "either enable flash encryption + secure boot for production builds",
     "the key needs NVS encryption too"),
    ("docs/strategy/13-black-box-evidence-model.md",
     "plaintext-NVS key on current canaries until flash encryption ships",
     "flash encryption alone would not end it"),
    ("spec/beacon_channel_v0.md",
     "(NVS, eFuse-derived flash encryption, key generation at first boot)",
     "the key is plaintext in NVS on every tier"),
    ("docs/secure_provisioning.md",
     "JTAG permanently disabled via eFuse - NVS encryption enabled",
     "the kit's NVS encryption is inert under framework = arduino"),
    ("docs/secure_provisioning.md",
     "Secure Boot v2 **prepared but not enabled**, Flash Encryption release "
     "mode, NVS encryption, anti-rollback, JTAG off",
     "flash encryption and anti-rollback are commented out too, NVS encryption "
     "is inert and JTAG is the provisioning script's"),
    ("firmware/provisioning/sdkconfig.defaults.secure",
     "using a key stored in eFuse. # CONFIG_NVS_ENCRYPTION=y",
     "the line is inert under framework = arduino, and the file says so"),
    ("docs/audit/mesh_and_chirp_audit_v1.md",
     "become a permanent opera member from outside the household. **Fix in this "
     "branch:**",
     "the dated note keeps its amendment: flash encryption does not cover NVS"),
    ("docs/audit/mesh_and_chirp_audit_v1.md",
     "| NVS confidentiality | M | No FE gate (O2). |",
     "the same"),
    ("docs/audit/mesh_and_chirp_audit_v1.md",
     "flash-encrypted NVS ring demoted to a recent-view cache",
     "the same"),
    ("docs/audit/esp32s3_firmware_full_audit_2026-07.md",
     "sit in plaintext flash today; a PSRAM-backed",
     "the dated note keeps its amendment: they would with Secure Boot and flash "
     "encryption too"),
    ("docs/V1_BENCH_TEST_RUNBOOK.md",
     "**no mesh secret lands in unencrypted NVS**",
     "the track proves no secret lands on a board without flash encryption"),
    ("docs/V1_BENCH_TEST_RUNBOOK.md",
     "**FE secret-at-rest gate**",
     "a persistence gate, not at-rest secrecy"),
    ("docs/V1_LAUNCH_REVIEW.md",
     "can never be silently persisted to unencrypted NVS",
     "the gate keeps it off boards without flash encryption only"),
    ("docs/BLE_MESH_OPERA_TANDEM.md",
     "encrypted by the device's secure-element-backed key (where available)",
     "no Canary has a secure element and NVS is plaintext on every tier"),
    ("docs/security/PENTEST_SCOPE.md",
     "We know a physical attacker can dump flash and recover the identity key. "
     "What",
     "from a board with flash encryption fused too"),
    ("firmware/README.md",
     "the provisioning kit's Tier 3/4 image, flash encryption required",
     "the secure env refuses the key on every board, since no board has NVS "
     "encryption"),
    ("docs/unflashed_board_intake.md",
     "If you want to be the last person who can ever do this to that board",
     "Phase 2 as tooled enables neither secure boot nor flash encryption"),
    # The same sweep: the unlock banner and the first-asker tap are the
    # flagship's; a WAP's receipt tap goes to the one address asking
    # (tap_arbitrate()), so its clients ask first, and the iPhone app fetches
    # no kit with a tap.
    ("docs/getting_started_canary.md",
     "**Why the dashboard asks to be unlocked on your home network.**",
     "the banner is the all-rounder Canary's; a WAP's sign-in asks first"),
    ("docs/getting_started_canary.md",
     "one page load, or one app or browser fetching the recovery kit",
     "on the flagship one page load or one browser fetch; a WAP's tap goes to "
     "the one asker"),
    ("docs/getting_started_canary.md",
     "the iPhone app fetching the kit on its own needs a fresh tap",
     "the iPhone app never fetches the kit with a tap"),
    ("firmware/common/network/provisioning_gate.h",
     "(page_token_decide). The tap is // one consumer across both paths",
     "that is the flagship's; the WAP's receipt tap goes to one asker "
     "(tap_arbitrate)"),
    ("firmware/projects/canary-wap/arduino/canary_wap/provisioning_gate.h",
     "(page_token_decide). The tap is // one consumer across both paths",
     "the same (the staged copy)"),
    ("canary-vision/spa/app.js",
     "so a press anywhere in our 60 s window lands. // Returns a handle",
     "while this page is the only one asking; a WAP answers 409 tap_contended"),
    ("canary-vision/spa/app.js",
     "el('div', { className: 'pair-step-title', textContent: 'Short-tap the "
     "BOOT button' }),",
     "the step says not to press until the device has answered an ask with 403"),
    ("canary-vision/device-api/routes/provision.js",
     "Mirrors the firmware's BOOT-button provisioning gate",
     "it models the first-asker rule, not the WAP's tap_arbitrate()"),
    ("canary-vision/device-api/lib/device-state.js",
     "(mirrors firmware: a short-tap opens a",
     "the same"),
    # #1843's sweep, second pass (its review round 1): the kit as tooled turns
    # on neither Secure Boot nor flash encryption; a cleared unit still has an
    # owner and its door is first-come; no reset clears the mesh or scout keys;
    # a served tap frees its own slot only; the one-asker tap is the WAP's; the
    # iPhone app never reaches a WAP's Bluetooth lane (A65); and a re-flash is
    # not a full erase.
    ('docs/security/THREAT_MODEL.md',
     '│ │ Secure Boot │ │ Flash │ │ GPS Time │ │',
     "no tier has Secure Boot or flash encryption on; the boundary's boxes are the OTA signature check and the bootloader's image check"),
    ('docs/security/THREAT_MODEL.md',
     'secure boot, flash encryption and eFuse locks on the opt-in secure-provisioning tier',
     'the kit prepares both and turns on neither; its Phase 2 burns the JTAG fuses'),
    ('docs/security/THREAT_MODEL.md',
     '**Secure tier: PARTIAL PASS.**',
     'the secure tier is FAIL today, as the default: the kit turns neither protection on'),
    ('docs/security/THREAT_MODEL.md',
     'Secure boot and flash encryption are an opt-in secure-provisioning build',
     'they are staged for that build, which turns on neither today'),
    ('docs/security/THREAT_MODEL.md',
     '**PARTIAL PASS:** Documented limitation. Future mitigations: application-layer SD card encryption',
     'with both tiers FAIL the scenario is FAIL, documented: the limitation is disclosed, not mitigated'),
    ('docs/security/THREAT_MODEL.md',
     'flash encryption keeps the app image from being read off the chip',
     'the kit never turns flash encryption on; once it does, flash encryption would keep the app image from being read'),
    ('docs/security/THREAT_MODEL.md',
     '| Firmware corrupt | No tier refuses to boot it today',
     "every tier's bootloader checks an image's checksum and SHA-256 and tries the other slot; what no tier refuses is a well-formed unsigned image"),
    ('docs/security/THREAT_MODEL.md',
     'Scenario 4) — opt-in secure-provisioning tier only',
     'flash encryption is enabled on no tier; the kit prepares it'),
    ('docs/security/THREAT_MODEL.md',
     'with physical access can potentially bypass ESP32 secure boot',
     'no tier enables Secure Boot today; the limitation applies once one does'),
    ('docs/security/THREAT_MODEL.md',
     'No outbound connections (compromised chip has no exfiltration channel)',
     'the networked products open the disclosed paths: broker, signed-manifest check, SNTP, forecast'),
    ('docs/security/THREAT_MODEL.md',
     'which flash encryption (an opt-in tier) does not cover',
     'the opt-in kit does not turn flash encryption on'),
    ('docs/security/SECURITY_MODEL.md',
     'but they are **off unless you turn them on**',
     'they are off on every Canary today: the kit prepares both and turns on neither'),
    ('docs/security/SECURITY_MODEL.md',
     'flash encryption keeps the app image from being read off the chip',
     'the same: the opt-in tiers buy nothing yet'),
    ('canary-local/assets/flash.js',
     'eFuse locks) protects the firmware image',
     'Phase 2 turns on neither flash encryption nor secure boot, so it protects nothing on the board yet'),
    ('canary-local/assets/flash.js',
     'Phase-2 provisioning (flash encryption + eFuse) does not yet protect its key or saved WiFi.',
     'it protects nothing yet, the firmware included'),
    ('docs/V1_BENCH_TEST_RUNBOOK.md',
     'Secure Boot v2 + Flash Encryption burned; `verify_device.py` confirms',
     'the kit makes no FE board, and its lockdown check fails on fuses it does not burn'),
    ('docs/secure_provisioning.md',
     '# 4. Enable Secure Boot v2',
     'the script burns SECURE_BOOT_EN with no key digest and flashes no signed bootloader'),
    ('docs/secure_provisioning.md',
     '# 6. Enable anti-rollback',
     'no step burns SECURE_VERSION'),
    ('docs/secure_provisioning.md',
     '# 7. Verify all eFuses are correctly set',
     'verify_device.py --expect-locked fails on fuses the script never burns'),
    ('docs/secure_provisioning.md',
     '| `FLASH_CRYPT_CNT` | 0 | odd (1,3,5,7) | Enables flash encryption |',
     'the Phase 2 column is the target; the script burns no crypt counter today'),
    ('docs/secure_provisioning.md',
     '### Step 2: Build Signed Firmware',
     "the secure env's image is not Secure-Boot-signed"),
    ('docs/secure_provisioning.md',
     'virgin verify → burn keys → flash signed firmware',
     'the script flashes a plaintext, unsigned image, then burns the key'),
    ('firmware/canary/include/secure_defaults.h',
     'Secure Boot v2 and flash encryption are opt-in, through the provisioning',
     'the kit stages both and turns on neither'),
    ('docs/strategy/16-kit-commerce-pricing-and-fulfillment.md',
     'SKU** run the Phase-2 secure provisioning path',
     "the path as tooled turns on neither, and flash encryption would not cover the key's NVS"),
    ('firmware/provisioning/provision_canary.sh',
     '- Flash signed firmware - Burn flash encryption key - Enable Secure Boot v2',
     'the script flashes an unsigned plaintext image and sets up no Secure Boot'),
    ('firmware/provisioning/provision_canary.sh',
     'echo " - Enable Secure Boot v2"',
     'the burn sets SECURE_BOOT_EN with no key digest'),
    ('firmware/provisioning/provision_canary.sh',
     'echo " - Enable anti-rollback"',
     'no anti-rollback fuse is burned'),
    ('firmware/provisioning/provision_canary.sh',
     'Device is locked down and ready for production.',
     'neither Secure Boot nor flash encryption is on after the burn'),
    ('firmware/provisioning/provision_canary.sh',
     '2. Flash signed firmware → 3. Burn keys',
     'the image is plaintext and unsigned'),
    ('docs/design/hardware_root_of_trust.md',
     '(virgin-verify → burn → flash → post-verify, with `--dry-run`)',
     'the script flashes before it burns, and a Phase 2 run turns neither protection on'),
    ('docs/design/vault_operator_ux_v1_1.md',
     "Secure it (protect the key at rest) — the fuses are tooled, the key's",
     'the kit burns no crypt counter and no Secure Boot key digest'),
    ('docs/design/vault_operator_ux_v1_1.md',
     'The fuse and signing steps are largely already tooled',
     'the signing step is not tooled at all'),
    ('docs/design/vault_operator_ux_v1_1.md',
     'the fuse steps are mostly tooled',
     'the same'),
    ('docs/security/PENTEST_SCOPE.md',
     '**Flash encryption and Secure Boot are off by default.** Deliberate, and the',
     'off on every Canary today: the kit turns neither on'),
    ('firmware/common/network/improv_core.h',
     "a device NOBODY OWNS YET, and for an owner's own tap",
     'the door is open for a device with no network saved, a cleared one included, which still has an owner'),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     "a device NOBODY OWNS YET, and for an owner's own tap",
     'the same (the staged copy)'),
    ('docs/design/magic_pairing.md',
     "the door is open for a device NOBODY OWNS YET, and for an owner's own tap",
     'the same'),
    ('docs/design/magic_pairing.md',
     'of power-on (after a reset, of the credentials vanishing)',
     "after a WAP's hold the window runs from the restart on release"),
    ('docs/design/magic_pairing.md',
     'a factory reset re-arms it from the moment the credentials vanish',
     'the same'),
    ('docs/design/onboarding_shared_module.md',
     'a factory reset re-arms it from the moment the credentials vanish',
     'the hold restarts on release; credentials vanishing at runtime re-arm the window'),
    ('docs/ble_protocol.md',
     'the door is open for a device NOBODY OWNS YET',
     "the header's rule now names a cleared unit, and the bullet the first-come door"),
    ('docs/GLOSSARY.md',
     "device nobody owns yet, and for an owner's own tap",
     'the same'),
    ('docs/FAQ.md',
     "device nobody owns yet, and for an owner's own tap",
     'the same'),
    ('firmware/projects/canary-sense/README.md',
     "device nobody owns yet, and for an owner's own tap",
     "the same; the Sense's hold keeps the witness identity"),
    ('firmware/projects/canary-vision/README.md',
     "device nobody owns yet, and for an owner's own tap",
     "the same; the Vision's hold keeps the witness identity"),
    ('docs/FIRMWARE_MAP.md',
     'claim an unowned device during its 30-minute first-boot window.',
     'the snapshot keeps its amendment: or an owned one its owner reset'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     'no secret persisted (first boot, factory-reset state)',
     'the 10 s reset keeps it; a leave or the 30 s wipe drops it'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     'Erase the persisted opera_secret. Use on factory reset or',
     "POST /api/mesh/leave and persist_rotation()'s failure path call it; no reset does"),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     'absent (idempotent — factory-reset semantics)',
     'the same'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     'On "no peers persisted" (first boot / factory',
     'the 10 s reset keeps the list'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     'FE disabled, nothing persisted (first boot /',
     'the 10 s reset keeps the elected hub'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.cpp',
     'call idempotent across factory-reset semantics',
     'no reset calls clear_opera_secret()'),
    ('firmware/canary/src/main.cpp',
     'false on first-boot / factory-reset / FE-disabled devices',
     'the 10 s reset keeps the secret'),
    ('firmware/canary/src/main.cpp',
     'Empty-list (first * boot / factory reset) is fine',
     'the 10 s reset keeps the peer list'),
    ('firmware/canary/lib/securacv_ble_scan/src/ble_scout_key.h',
     'Use for "unpair all Scouts" / factory reset.',
     'nothing calls it, and no BOOT hold touches the scout key'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_scout_key.h',
     'Use for "unpair all Scouts" / factory reset.',
     'the same (the staged copy)'),
    ('firmware/canary/lib/securacv_network/src/securacv_network.cpp',
     "also ends the asking address's backoff (auth_handed_token, sweep",
     "its own backoff, never the shared overflow's"),
    ('firmware/projects/canary-wap/tests_host/test_receipt_grant.cpp',
     "and the tap ends that address's backoff (sweep F341)",
     'the same'),
    ('docs/getting_started_canary.md',
     'it opens the provisioning gate for 30 seconds, which hands the recovery kit to the one page already asking for it',
     "that is a WAP's rule; the all-rounder Canary's tap goes to whichever asks first"),
    ('docs/getting_started_canary.md',
     "(This and the next paragraph are the all-rounder Canary's,",
     'the next paragraph covers the WAP too'),
    ('docs/getting_started_canary.md',
     'or from a kit you open in it. That is how you get the kit back later without a USB cable.',
     "the kit comes back through the tap-unlocked page's save"),
    ('docs/onboarding_multiple_canaries.md',
     'The app listens for ~60 seconds. **Short-tap the BOOT button** on that',
     "the app asks first and says when to press (after the device's first 403)"),
    ('docs/onboarding_multiple_canaries.md',
     'every device after the first is one tap in the app plus one tap on the device.',
     "the device's tap comes once the app says to"),
    ('canary-vision/spa/app.js',
     'pick (or discover) a device, // short-tap its BOOT button',
     'the app asks first and says when to press'),
    ('ios/README.md',
     'the receipt its own pages save after a BOOT tap',
     "a WAP's pages ask first, and BOOT is pressed after"),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     "this Canary's Bluetooth rescue or its own setup network",
     "the iPhone app never reaches a WAP's lane (A65); the setup network takes the correction"),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     '(which the BLE provisioning // service and its own setup network can still rescue)',
     'the same'),
    ('ios/Sources/SecuraCV/Views/FleetWiFiSheet.swift',
     'which keeps its Bluetooth rescue — never the fleet',
     'the same'),
    ('ios/README.md',
     'one device (which keeps its rescues), never the fleet',
     'the same'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     '(ble_provision.h) — /// the rescue path for a Canary the router password change stranded.',
     'the rescue uses only a console the class dialed, and a WAP advertises none (A65)'),
    ('docs/legal-audit-2026-07.md',
     'which never leaves the secure element." **Changed now:**',
     'no Canary has a secure element; the key is plaintext NVS'),
    ('firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md',
     '(a remote credential alone must never be able to reopen the setup door)',
     'Forget Wi-Fi already does, from any session (F345)'),
    ('firmware/FEATURES.md',
     '| `provision_canary.sh` | ✅ | Full workflow with --dry-run |',
     'its Phase 2 turns on neither protection, and its lockdown check fails'),
    ('firmware/FEATURES.md',
     '| API token NVS storage | ✅ | ❌ | ❌ | ❌ |',
     'both trees store the token in NVS'),
    ('firmware/FEATURES.md',
     '| TLS cert NVS storage | ✅ | ❌ | ❌ | ❌ |',
     'both trees store the certificate in NVS'),
    ('firmware/FEATURES.md',
     '| WiFi credentials NVS storage | ✅ | ❌ | ❌ | ❌ |',
     'both trees store the saved networks in NVS'),
    ('firmware/FEATURES.md',
     'identity kept; 30 s: wipe, identity too) | ❌ |',
     'the canary-wap PlatformIO lane compiles the same sketch'),
    ('canary-local/tools/hatchery/derive.mjs',
     'a factory reset // that mints a NEW key is honestly a new bird',
     'no factory reset mints a key; an install or a full erase in a flasher, '
     'or the 30 s wipe, does'),
    ('ios/Sources/SecuraCV/Security/DeviceStore.swift',
     'a fully erased (re-flashed) device with a new',
     'the comment names the install and the full erase, whose factory image '
     'starts NVS over; an OTA update and the 10 s hold keep the key'),
    ('custom_components/securacv/tests/test_replay_reset_paths.py',
     'an erased (re-flashed) or replaced Canary starts them over',
     'the same'),
    ('firmware/common/network/auth_backoff.h',
     'polling a re-flashed unit with its old token',
     'the same'),
    ('firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h',
     'polling a re-flashed unit with its old token',
     'the same (the staged copy)'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     'F352 [code] iOS: a pairing for a re-flashed Canary keeps polling',
     'the same'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '#1843: a re-flashed unit; the 10 s hold keeps the token',
     'the same'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "#1843: a re-flashed unit's, since the 10 s hold keeps the token",
     'the same'),
    ('firmware/LESSONS_LEARNED.md',
     'polling a re-flashed unit (a full erase gives it a new key and token',
     'the same'),
    ('CHANGELOG.md',
     'still polling a re-flashed unit (a full erase gives it a new key and',
     'the same'),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     'Remove every saved Wi-Fi network from this Canary? For half an hour after, its Bluetooth setup goes',
     'a build without the door, or with Bluetooth off, opens none'),
    # #1843's sweep, third pass (its review round 2): the kit's own recipe for full
    # Secure Boot, an ordinary re-flash said to keep the key (both flashers
    # write a factory image whose settings region is blank), the door rule's
    # last copies, the iPhone design doc's BOOT-tap tokens, the Fleet Wi-Fi
    # sheet's Bluetooth offers and un-fused meaning flash encryption off.
    ('firmware/provisioning/platformio_secure.ini',
     'To enable full Secure Boot:',
     'no step of the kit turns Secure Boot on: the sdkconfig is inert under framework = arduino, and Phase 2 burns SECURE_BOOT_EN with no Secure Boot key digest'),
    ('firmware/provisioning/platformio_secure.ini',
     'Prepares for Secure Boot (not enabled until Phase 2 eFuse burn)',
     'the Phase 2 burn leaves Secure Boot enabled with nothing to verify against'),
    ('firmware/provisioning/sdkconfig.defaults.secure',
     'To enable full security (Phase 2):',
     'uncommenting changes nothing under framework = arduino, and Phase 2 turns on neither protection'),
    ('firmware/provisioning/sdkconfig.defaults.secure',
     'Uncomment these lines for production lockdown:',
     'the same: no Canary build reads these lines'),
    ('firmware/provisioning/README.md',
     'Production lockdown with Secure Boot v2, Flash Encryption, JTAG disabled',
     'as tooled, Phase 2 reaches only the JTAG fuses'),
    ('firmware/provisioning/README.md',
     '# 3. Execute production provisioning',
     'Phase 2 is not usable yet: the dry run is the only step to run'),
    ('firmware/provisioning/generate_keys.sh',
     'Loss of signing key = bricked devices on next OTA',
     'the kit never uses the signing key: it burns no Secure Boot key digest and signs nothing'),
    ('docs/secure_provisioning.md',
     'Loss of signing key = bricked devices on next OTA.',
     'the same'),
    ('docs/secure_provisioning.md',
     'keys/secure_boot_signing_key.pub (embedded in firmware)',
     'no step embeds it or burns its digest'),
    ('docs/secure_provisioning.md',
     'The secure configuration disables Bluetooth at compile time (`CONFIG_BT_ENABLED=n`).',
     "that sdkconfig is inert under framework = arduino; the env's -DFEATURE_BLE=0 flags keep Bluetooth code out"),
    ('docs/secure_provisioning.md',
     '# Or with ESP-IDF directly',
     'there is no ESP-IDF project for this tree yet'),
    ('canary-local/assets/flash.js',
     'so today it protects " + "nothing on the board.',
     'Phase 2 does burn the JTAG-disable fuses; it protects neither the firmware, the key nor the saved WiFi'),
    ('ios/Sources/SecuraCV/Security/DeviceStore.swift',
     '(an ordinary re-flash keeps the key)',
     'both flashers write a factory image whose settings region is blank, so a re-install gives a new key; an OTA update and the 10 s hold keep it'),
    ('custom_components/securacv/tests/test_replay_reset_paths.py',
     '(an ordinary re-flash keeps the key and its counters)',
     'the same'),
    ('firmware/common/network/auth_backoff.h',
     'device_id; an ordinary re-flash and',
     'the same: an install or a full erase in either flasher gives a new key and token'),
    ('firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h',
     'device_id; an ordinary re-flash and',
     'the same (the staged copy)'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     'the 10 s hold and an ordinary re-flash keep the key and the token',
     'an install in either flasher changes them too: its factory image starts NVS over'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     'an ordinary re-flash keep the token, and only a full erase in a flasher changes it',
     'the same'),
    ('firmware/LESSONS_LEARNED.md',
     'the 10 s hold and an ordinary re-flash keep the token',
     'the same'),
    ('CHANGELOG.md',
     'the 10 s hold and an ordinary re-flash keep the token',
     'the same'),
    ('CHANGELOG.md',
     '"re-flashed" no longer stands for "fully erased" (an ordinary re-flash keeps NVS)',
     'an ordinary re-flash in either flasher does not keep NVS: its factory image blanks the settings region'),
    ('CHANGELOG.md',
     "only the flagship's 30 s wipe or a full erase in a flasher changes them",
     'an install in either flasher changes them too'),
    ('CHANGELOG.md',
     'full erase in either flasher is what removes them',
     'an install rewrites the settings region too'),
    ('firmware/canary/scripts/ota_deploy.py',
     'Only the 30 s BOOT wipe or a full flash erase',
     'an install in a flasher makes a new certificate too'),
    ('firmware/canary/scripts/ota_deploy.sh',
     'Only the 30 s BOOT wipe or a full flash erase',
     'the same'),
    ('docs/security/PENTEST_SCOPE.md',
     "Only the all-rounder Canary's 30 s wipe or a full erase in a flasher sheds them.",
     'an install in either flasher sheds them too'),
    ('docs/design/magic_pairing.md',
     'only a full erase in a flasher replaces them, with the identity.',
     'an install in either flasher replaces them too'),
    ('desktop/src/index.html',
     'Reflashing a Canary you set up here and want to keep the identity of? Untick it',
     'every install starts the settings region over, the identity key with it'),
    ('firmware/projects/canary-sense/include/canary/witness.h',
     'keypair survives // reflashes and OTA installs',
     'an install or a full erase in either flasher starts NVS over, so the next boot makes a new key'),
    ('firmware/projects/canary-vision/include/canary/witness.h',
     'keypair survives // reflashes and OTA installs',
     'the same'),
    ('firmware/projects/canary-sentinel/include/canary/witness.h',
     'keypair survives reflashes and OTA installs',
     'the same'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'it stays stable across reflashes because the keypair persists in NVS.',
     'an install in either flasher starts NVS over, so the suffix changes with the key'),
    ('canary-local/tools/gen_flash.py',
     'The chain rides through reflashes untouched.',
     'an install in a flasher starts the settings region over, and the board a new chain under a new key'),
    ('canary-local/devices/flash.json',
     'The chain rides through reflashes untouched.',
     'the same (generated by gen_flash.py)'),
    ('canary-local/tools/gen_flash.py',
     'then writes fresh, so the board comes up with a new "',
     "a normal install starts the settings region over too, so a new identity is not the full erase's alone"),
    ('canary-local/devices/flash.json',
     'then writes fresh, so the board comes up with a new identity. More than a factory reset',
     'the same (generated by gen_flash.py)'),
    ('canary-local/tools/gen_flash.py',
     'a normal install only touches the regions the new firmware needs.',
     'a normal install writes the regions its image covers, a blank settings region among them'),
    ('canary-local/devices/flash.json',
     'a normal install only touches the regions the new firmware needs.',
     'the same (generated by gen_flash.py)'),
    ('docs/README.md',
     'The door rule (open for a device nobody owns yet,',
     'the door is for a device with no network saved, which can be one its owner cleared, and goes to whichever client connects first'),
    ('firmware/LESSONS_LEARNED.md',
     'that has an owner and a bad day.* - **Regression check:**',
     "the quote carries its amendment: the header's sentence now names a device with no network saved"),
    ('firmware/LESSONS_LEARNED.md',
     'A door that is open to whoever has no owner yet must still be a door nobody can wedge.',
     'a door open to whichever client connects while no network is saved'),
    ('docs/design/iphone_companion_app.md',
     'the per-device tokens it earned by the BOOT-button tap',
     "the app pairs through the setup door's claim or a WAP's recovery kit; it has no BOOT-tap pairing"),
    ('docs/design/iphone_companion_app.md',
     'Trust-on-Pair BOOT-button receipt → Keychain token;',
     'the same'),
    ('ios/Sources/SecuraCV/Views/FleetWiFiSheet.swift',
     "Come back when it's in Bluetooth range",
     "the app never reaches a WAP's Bluetooth lane (A65); the WAP's own setup network takes the password"),
    ('ios/Sources/SecuraCV/Views/FleetWiFiSheet.swift',
     'goes only to your Canaries — over bonded Bluetooth',
     'the same'),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     'Move within Bluetooth range for its encrypted lane',
     'the same'),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     "Their encrypted lanes aren't available: Bluetooth needs",
     'the same'),
    ('docs/GLOSSARY.md',
     'keeps the household secret off un-fused boards',
     'off every board without flash encryption, a Phase 2 board included'),
    ('docs/design/apple_home_integration.md',
     'keeps them off un-fused boards',
     'the same'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     'keeps the secret off un-fused boards',
     'the same'),
    ('firmware/projects/canary-wap/arduino/canary_wap/mesh_network.cpp',
     'keeps the secret off un-fused boards only',
     'the same'),
    ('spec/canary_mesh_network_v0.md',
     'keeps the secret off un-fused boards',
     'the same'),
    ('spec/beacon_channel_v0.md',
     'an un-fused board keeps the set in RAM only',
     'the same'),
    ('firmware/scripts/regression_check.sh',
     'off un-fused boards',
     'the same'),
    ('docs/audit/mesh_and_chirp_audit_v1.md',
     'off un-fused boards',
     'the same (the 2026-10-21 amendments)'),
    # #1843's sweep, fourth pass (its review round 3): both flashers' install
    # verdicts and first-contact copy, the browser trust card's key that
    # "never leaves", the device_id and handle said to survive reflashes, the
    # flagship's certificate comment and the other key-change lists that left
    # out an install, the token the flagship's hold does not keep, the
    # dashboard's press-at-once retry lines, the kit's Phase 2 run and table
    # notes, and Secure Boot described as not enabled.
    ('canary-local/assets/flash-core.js',
     'Your settings region is left alone unless the map below says otherwise.',
     'every install writes a factory image whose settings region is blank; the verdict ends on USB_INSTALL_STARTS_OVER'),
    ('canary-local/assets/flash-core.js',
     'The board already runs this exact version. Installing again rewrites it byte-for-byte —',
     'a reinstall starts the settings region over too, and so gives the board a new witness key'),
    ('canary-local/assets/flash-core.js',
     'harmless, and occasionally exactly what a misbehaving board needs.',
     'the same: not harmless to an owner who needs the old key'),
    ('canary-local/assets/flash-core.js',
     'but newer settings may not be understood by older firmware.',
     'the install starts the settings region over, so older firmware is never handed newer settings'),
    ('desktop/src/app.js',
     'A newer build of the same firmware. Your settings region is left alone.',
     "the same as the browser's: the engine's write-bin 0x0 writes a blank settings region"),
    ('desktop/src/app.js',
     'The board already runs this exact version. Installing again rewrites it byte-for-byte —',
     'the same'),
    ('desktop/src/app.js',
     'harmless, and occasionally exactly what a misbehaving board needs.',
     'the same'),
    ('desktop/src/app.js',
     'but newer settings may not be understood by older firmware.',
     'the same'),
    ('canary-local/assets/flash.js',
     'chip. The private key is born there and never leaves — there is no',
     "the safety copy this page saves is a full read of the flash, the key's plaintext NVS included"),
    ('canary-local/assets/flash.js',
     'export function, and this page never reads it (the health check shows',
     'the same: the safety copy reads it'),
    ('canary-local/assets/flash.js',
     'forging it needs the key that never left the board.',
     'the same: a copy of the flash carries the key'),
    ('canary-local/assets/flash.js',
     'Key theft through the browser — the identity key is never transmitted,',
     'the safety copy carries the key over USB into the page and to the downloads folder; nothing goes to the network'),
    ('canary-local/assets/flash.js',
     'If this is genuinely your own Canary and you want to keep its',
     'the claim waives the full erase only; every install starts the settings region over, the identity key with it'),
    ('canary-local/assets/flash.js',
     "it's mine — keep its data",
     'the same: the button skips the full erase'),
    ('canary-local/assets/flash.js',
     'this board is already mine, keep its data',
     'the same (the state comment)'),
    ('canary-local/assets/flash.js',
     'Phase-2 provisioning burns eFuses but does not',
     'Phase 2 burns SECURE_BOOT_EN with no key digest, so Secure Boot is enabled with nothing to verify against'),
    ('canary-local/assets/flash.js',
     'dev_id is sticky // forever once written',
     'an install in either flasher starts NVS over, dev_id with it'),
    ('canary-local/assets/intake.js',
     'the human said "this is my board, keep its data"',
     'the claim skips the full erase; the install still starts the settings region over'),
    ('docs/unflashed_board_intake.md',
     'deliberate "it\'s mine — keep its data" button.',
     'the button skips the full erase; the factory image every install writes carries a blank settings region'),
    ('docs/unflashed_board_intake.md',
     'That preserves reflashing your own Canary without losing its identity key',
     'the same: the next boot makes a new key either way'),
    ('canary-local/tools/gen_flash.py',
     'factory install like this one is deeper: on some boards the diary’s',
     'every Canary keeps its key and chain state in NVS, which the factory image starts over'),
    ('canary-local/devices/flash.json',
     'on some boards the diary’s chain state rides the settings region and starts fresh here.',
     'the same (generated by gen_flash.py)'),
    ('firmware/projects/canary-sense/include/canary/runtime_config.h',
     'NVS always wins (identity is sticky across reflashes and',
     'an install in either flasher starts NVS over, so the dev_id goes with it; an OTA update and a PlatformIO or Arduino IDE upload keep it'),
    ('firmware/projects/canary-vision/include/canary/runtime_config.h',
     'NVS always wins (identity is sticky across reflashes and',
     'the same'),
    ('firmware/projects/canary-sentinel/include/canary/runtime_config.h',
     'NVS always wins (identity is sticky across reflashes and',
     'the same'),
    ('firmware/projects/canary-display/include/canary/runtime_config.h',
     'NVS always wins (identity is sticky across reflashes and',
     'the same'),
    ('firmware/projects/canary-display/arduino/canary_display/runtime_config.h',
     'NVS always wins (identity is sticky across reflashes and',
     'the same (the sketch mirror setup.sh regen writes)'),
    ('firmware/canary/lib/securacv_crypto/src/securacv_crypto.cpp',
     "the handle is stable for the device's lifetime",
     'the suffix lasts as long as the key: an install in either flasher or the 30 s wipe gives a new one'),
    ('firmware/canary/lib/securacv_crypto/src/securacv_crypto.h',
     "(no 0/O/o, 1/I/i/l/L); stable for the device's lifetime.",
     'the same'),
    ('firmware/common/identity/device_pseudonym.h',
     "fixed for the device's lifetime, so the result is computed once and cached in RAM.",
     'the salt is NVS: an install in either flasher starts it over, and the next boot mints a new salt and pseudonym'),
    ('firmware/projects/canary-wap/arduino/canary_wap/device_pseudonym.h',
     "fixed for the device's lifetime, so the result is computed once and cached in RAM.",
     'the same (the staged copy)'),
    ('firmware/projects/canary-display/arduino/canary_display/device_pseudonym.h',
     "fixed for the device's lifetime, so the result is computed once and cached in RAM.",
     'the same (the sketch mirror)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'One stable SSID for the lifetime of the device',
     'the SSID derives from the key fingerprint, which an install in either flasher changes'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'the API token (derived from the // key), the OTA settings',
     'the hold keeps api_tkn whether the WAP derived it or a flasher seeded it'),
    ('firmware/LESSONS_LEARNED.md',
     'per-board uniqueness and reflash-stability are preserved',
     'the suffix changes with the key, which an install in either flasher changes'),
    ('docs/firmware_ota.md',
     'HA entities survive OTA updates and reflashes.',
     'an install in either flasher starts NVS over, so the device_id the entities hang on changes; an OTA update and a PlatformIO upload keep it'),
    ('canary-local/tools/gen_sense.py',
     'identity is sticky across reflashes and OTA updates',
     'the same: the Lab Sense page shows it'),
    ('canary-local/devices/sense.json',
     'identity is sticky across reflashes and OTA updates',
     'the same (generated by gen_sense.py)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_improv.cpp',
     'a phone that never reads it needs the BOOT-tap route later.',
     'the iPhone app has no BOOT-tap pairing; a phone that missed the claim pairs from a recovery kit the WAP pages save'),
    ('firmware/canary/include/canary_config.h',
     'only the 30 s wipe // (ResetScope::Everything) or a full flash erase removes them',
     'an install in either flasher removes them too: its factory image starts NVS over'),
    ('custom_components/securacv/sensor.py',
     "which is what a full flash erase (or an all-rounder Canary's 30 s BOOT wipe) looks like from here",
     'an install in either flasher re-pins too'),
    ('custom_components/securacv/tests/test_replay_and_device_id.py',
     '# A full erase (a new key): the device pins fresh',
     'the same'),
    ('firmware/canary/src/csi_event_log.h',
     'a card from before * the 30 s BOOT wipe or a full erase; the 10 s reset keeps the key)',
     'an install in either flasher gives a new key too'),
    ('canary-local/tools/hatchery/derive.mjs',
     "a full erase // (or the all-rounder Canary's 30 s BOOT wipe) that mints a NEW key is",
     'an install in either flasher mints one too'),
    ('firmware/common/health/boot_policy.h',
     'Re-flashing the SAME build keeps the count; the operator clear',
     'only an upload that leaves NVS alone keeps the count; an install in either flasher starts it over'),
    ('firmware/common/health/boot_policy.h',
     'Re-flashing the SAME build keeps the count (it is still the image that could',
     'the same'),
    ('firmware/tests_host/test_boot_guard.cpp',
     'A/B rollback or USB re-flash) — and re-flashing the SAME build does not;',
     'the same, for an upload that keeps NVS'),
    ('firmware/common/csi/src/csi_event_id_floor.h',
     "so its owner flashes it with the flasher's full // erase, which clears all of NVS",
     'an install in either flasher, with or without the full erase, clears it'),
    ('firmware/projects/canary-wap/arduino/canary_wap/csi_event_id_floor.h',
     "so its owner flashes it with the flasher's full // erase, which clears all of NVS",
     'the same (the staged copy)'),
    ('docs/csi_developer_api.md',
     "keeps both, so its owner flashes it with the flasher's full erase, which clears",
     'the same'),
    ('docs/TRUST_MODEL.md',
     'shedding the key is the opt-in 30-second stage the PIO canary alone enables',
     'an install in either flasher sheds it too'),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     "Didn't see a tap. Click Use BOOT Tap Once again, then short-tap BOOT.",
     'the line says to press only once the device has answered with 403, as the companion waits for its note'),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     'and only then short-tap BOOT.',
     'the same, for both 409 lines'),
    ('docs/FAQ.md',
     '**Use BOOT Tap Once** again and only then short-tap BOOT',
     'the same: press when the page says to'),
    ('docs/audit/hardware_verification_checklist.md',
     "Didn't see a tap. Click Use BOOT Tap Once again, then short-tap BOOT.",
     "the dashboard's line now waits for its own instruction"),
    ('CHANGELOG.md',
     'the identity key, the chain, the token and the HTTPS certificate on every Canary;',
     "the flagship's hold removes its stored token, so a token a flasher seeded does not come back"),
    ('CHANGELOG.md',
     'now says the 10 s reset keeps the identity, the token and the certificate by design',
     'the same: the token on a WAP'),
    ('CHANGELOG.md',
     'a PlatformIO or Arduino IDE upload and the 10 s BOOT reset keep them.',
     'the 10 s reset keeps the key and the certificate; a flagship hold drops a flasher-seeded token'),
    ('CHANGELOG.md',
     'changes it; every copy now says what the flashers do:',
     'the fourth pass found copies the third missed'),
    ('CHANGELOG.md',
     'and the third pass below corrects every copy.)',
     'the same'),
    ('CHANGELOG.md',
     'click Use BOOT Tap Once again, and only then short-tap BOOT"',
     "the dashboard's 409 line now says to press when it says to"),
    ('CHANGELOG.md',
     "Didn't see a tap. Click Use BOOT Tap Once again, then short-tap BOOT.",
     'the same, the timeout line'),
    ('docs/security/PENTEST_SCOPE.md',
     'and the API token, which is derived from the key, comes back the same',
     "a WAP's hold keeps its token as stored; the flagship's removes it and derives one, so a seeded token does not come back"),
    ('docs/design/magic_pairing.md',
     'keeps the identity key and the API token derived from it',
     'the hold keeps api_tkn whether the WAP derived it or a flasher seeded it'),
    ('docs/design/magic_pairing.md',
     'keep both, since both derive from the identity key the reset keeps',
     'the SoftAP password derives from the key; the token is kept as stored, derived or seeded'),
    ('docs/design/magic_pairing.md',
     'new token and device_id; the 10 s hold and an OTA update keep the token) keeps',
     'the paragraph covers the flagship, whose hold replaces a flasher-seeded token'),
    ('firmware/projects/canary-wap/README.md',
     'keeps the identity key and the API token derived from it',
     'the same'),
    ('firmware/projects/canary-wap/arduino/canary_wap/README.md',
     'API token derived via HKDF (never stored in plaintext)',
     'the token is stored in NVS, plaintext on every tier; a flasher may seed it instead of the HKDF derivation'),
    ('docs/security/THREAT_MODEL.md',
     'Every install verifies the Ed25519 release signature and the version floor first.',
     'a flagship image built without SECURACV_BUILD_RELEASE also takes an unsigned POST /api/ota'),
    ('docs/security/THREAT_MODEL.md',
     "that is Secure Boot's job, which the provisioning kit prepares but does not enable",
     'Phase 2 sets SECURE_BOOT_EN with no key digest: enabled with nothing to verify against'),
    ('docs/security/THREAT_MODEL.md',
     'Secure boot (would verify firmware integrity; prepared in the provisioning kit, not yet enabled on any tier)',
     'the same'),
    ('docs/design/household_presence_and_posture.md',
     'and secure boot is prepared but not enabled on any tier',
     'the same'),
    ('docs/device_trust.md',
     'a mismatch means something else is publishing under that `device_id`: do not rotate to it.',
     'a Sense, Vision or Sentinel that could not read its key (F362) also makes one; read the key off the device'),
    ('docs/secure_provisioning.md',
     '# Review output carefully, then execute for real:',
     'Phase 2 is not usable yet; the dry run is the only step to run, as the kit README says'),
    ('docs/secure_provisioning.md',
     '### Step 3: Provision Device (Dry Run First!)',
     'the same'),
    ('firmware/provisioning/provision_canary.sh',
     'SecuraCV Canary — Production Provisioning',
     'Phase 2 reaches no production lockdown yet'),
    ('firmware/provisioning/provision_canary.sh',
     'Provision a SecuraCV Canary device for production deployment.',
     'the same'),
    ('firmware/provisioning/provision_canary.sh',
     '# Execute Phase 2 provisioning (IRREVERSIBLE)',
     'the same: the help no longer offers the run'),
    ('firmware/provisioning/generate_keys.sh',
     'for SecuraCV Canary production provisioning (Phase 2).',
     'Phase 2 is not usable yet and never uses the signing key'),
    ('firmware/provisioning/README.md',
     'Always use `--dry-run` before production provisioning',
     'Phase 2 runs only as a dry run today'),
    ('firmware/provisioning/partitions_secure.csv',
     'Encrypted flags are enforced after eFuse burn',
     'Phase 2 burns no SPI_BOOT_CRYPT_CNT, so flash encryption stays off and the flags are never enforced'),
    ('firmware/provisioning/partitions_secure.csv',
     'nvs_keys stores NVS encryption key (derived from eFuse)',
     'no Canary build reads nvs_keys: NVS encryption is not available under framework = arduino'),
    ('ios/Shared/SetupGuide.swift',
     'A WAP keeps its own setup page on its own network, SecuraCV-XXXX.',
     "the WAP's setup network is SecuraCV-wap-XXXX (canary_wap.ino generate_ap_ssid)"),
    ('docs/research/face_id_without_faces.md',
     'and secure boot is prepared but not enabled on any tier',
     'Phase 2 sets SECURE_BOOT_EN with no key digest, so it verifies nothing'),
    ('docs/security/THREAT_MODEL.md',
     'Once Secure Boot is enabled (no tier enables it today)',
     'a Phase 2 board has SECURE_BOOT_EN burned with nothing to verify against; what no tier has is a working Secure Boot'),

    # #1843's sweep, fifth pass (its review round 4): the copies the fourth
    # pass left. A flasher's pre-flash safety copy is a full read of the
    # flash over USB, so the key's plaintext NVS rides it to the computer:
    # "never leaves" is the firmware's promise, not the chip's. An install in
    # either flasher starts NVS over, the boot count and the handle with it.
    # Phase 2 leaves flash encryption off and Secure Boot enabled with nothing
    # to verify against. A WAP's token may be a flasher's seeded one, and the
    # flagship's hold drops a seeded token. The iPhone app reaches no WAP's
    # Bluetooth lane (A65), and a cleared unit is not one nobody owns.
    ("firmware/canary/src/main.cpp",
     "a re-flash will not reset the crash-loop count",
     "an install in either flasher starts NVS over, the count with it; only an upload that keeps NVS keeps a count a zero image identity cannot reset"),
    ("firmware/canary/src/main.cpp",
     "Bench row E6: a re-flash of another build resets the count only if the",
     "a PlatformIO or Arduino IDE upload of another build does; an install in either flasher starts NVS over whatever the identity"),
    ("firmware/common/health/boot_guard.h",
     "and a re-flash of another build silently stops resetting the count",
     "an upload that keeps NVS does; a flasher install starts the count over regardless"),
    ("docs/design/magic_pairing.md",
     "the first is a device nobody owns, the second is a device that has an owner and a bad day.",
     "the first is a device with no network saved, a new one or one its owner cleared (§2.2)"),
    ("docs/security/THREAT_MODEL.md",
     "and on the default tier, whose flash is readable on a bench, an install or a full erase",
     "NVS is plaintext on a fused board too, so the default tier is not the only one a bench reads"),
    ("docs/security/THREAT_MODEL.md",
     "Requires Ed25519 private key (never leaves device) AND",
     "no firmware path exports it, but a full read of the flash, a flasher's safety copy included, carries it"),
    ("docs/security/THREAT_MODEL.md",
     "Forgery requires physical device possession AND key extraction.",
     "the device or a full read of its flash, a flasher's safety copy included"),
    ("docs/security/THREAT_MODEL.md",
     "and stored in NVS. It is **never**:",
     "the list holds for anything the firmware runs; the flashers' safety copy reads the whole flash over USB"),
    ("docs/security/THREAT_MODEL.md",
     "Every item above is a statement about *software* paths. It is not a claim that the key survives an adversary who takes the device to a bench:",
     "firmware paths; a flasher's safety copy is a software path that reads the key over USB"),
    ("docs/security/THREAT_MODEL.md",
     "Reviewers should hold this principle to the software boundary",
     "the firmware boundary"),
    ("docs/security/THREAT_MODEL.md",
     "provisioning kit but enabled on no tier today (Scenario 4); its Phase 2",
     "Phase 2 leaves flash encryption off and Secure Boot enabled with nothing to verify against"),
    ("docs/unflashed_board_intake.md",
     "where secure boot and flash encryption are staged, prepared but not yet enabled:",
     "Phase 2 sets SECURE_BOOT_EN with no digest: Secure Boot enabled with nothing to verify against"),
    ("docs/secure_provisioning.md",
     "and Flash Encryption (release mode) **prepared but not enabled** (commented out)",
     "the whole file is inert under framework = arduino, so uncommenting them turns nothing on"),
    ("firmware/provisioning/sdkconfig.defaults.secure",
     "NVS ENCRYPTION (Protects stored keys and settings)",
     "it would; no Canary build reads the file"),
    ("firmware/provisioning/sdkconfig.defaults.secure",
     "This can be enabled independently of flash encryption for Phase 1.",
     "no Canary build can enable it, Phase 1 or Phase 2"),
    ("firmware/provisioning/sdkconfig.defaults.secure",
     "# It encrypts NVS partition contents using a key stored in eFuse. # KNOWN INERT",
     "an ESP-IDF build would encrypt NVS; no Canary build is one"),
    ("firmware/canary/lib/securacv_mesh/src/mesh_state.cpp",
     "pairing on every boot until the eFuse is committed.",
     "until flash encryption is on, which the kit's Phase 2 does not do, and then the secret is still plaintext NVS"),
    ("docs/TRUST_MODEL.md",
     "BOOT-tap minute are the only times a node takes Wi-Fi from a phone.",
     "a WAP also takes it over its bonded Bluetooth lane from any bonded peer (F346)"),
    ("docs/IMPROVEMENT_ROADMAP.md",
     "**Swift compiled and tested by the PR's iOS CI; the WAP endpoint and fixture are host-tested.** | S |",
     "row 13 carries its amendment: no device fills the BLE lane (A65)"),
    ("firmware/LESSONS_LEARNED.md",
     "rollout prefers the bonded BLE lane and discloses plain http. - **Guidance:**",
     "the lesson carries its amendment: no WAP fills that lane (A65)"),
    ("firmware/LESSONS_LEARNED.md",
     "has no export, backup, or read interface - **Why:**",
     "in the firmware; a flasher's safety copy reads the whole flash, key included"),
    ("firmware/LESSONS_LEARNED.md",
     "way to start fresh on purpose is the flagship's 30 s BOOT wipe, or a clean install on the canary-wap,",
     "an install in either flasher starts fresh on either tree, with or without its erase"),
    ("firmware/projects/canary-wap/arduino/canary_wap/README.md",
     "portal and the AP password and API token are derived per device.",
     "the token is a flasher's seeded one, or with none one derived per device"),
    ("firmware/projects/canary-wap/secrets/secrets.example.h",
     "The API token: derived per device from the private key",
     "the one a flasher seeded at install or, with none, one derived from the key"),
    ("firmware/projects/canary-wap/setup.sh",
     "The API token: derived per device from the private key",
     "the same, in the heredoc that writes secrets.example.h"),
    ("docs/onboarding_workflow_evaluation.md",
     "keypair, then derives a stable `device_id`",
     "stable while the keypair is: an install in either flasher makes a new one"),
    ("docs/onboarding_workflow_evaluation.md",
     "event_contract §10), and derives the API token. (`canary_wap.ino`)",
     "it loads the stored token (a flasher's seeded one included) and derives one only when none is stored"),
    ("docs/security/PENTEST_SCOPE.md",
     "Extracting a device identity key by any means. Reading footage",
     "every means but a read of the flash of a board in hand, which is documented"),
    ("docs/security/PENTEST_SCOPE.md",
     "**Retrieving key bytes is always a finding, including from a device whose key you already hold**",
     "by any path but the documented flash read"),
    ("docs/security/PENTEST_SCOPE.md",
     "*retrieval* of key bytes is always a finding**",
     "the same"),
    ("docs/security/PENTEST_SCOPE.md",
     "and it is the key-extraction target. Handing over every key",
     "a read of its flash recovers it on every tier and is documented; the target is every other path"),
    ("docs/security/PENTEST_SCOPE.md",
     "are off by default.** Off on every Canary today, in fact:",
     "neither works on any Canary today: Phase 2 leaves Secure Boot enabled with nothing to verify against"),
    ("docs/security/SECURITY_MODEL.md",
     "Stored only on the device's internal flash memory",
     "a flasher's safety copy holds it too"),
    ("docs/security/SECURITY_MODEL.md",
     "Never transmitted over any interface (WiFi, USB, or otherwise)",
     "never sent by the firmware; a flasher's safety copy reads it over USB"),
    ("docs/security/SECURITY_MODEL.md",
     "Not backed up, escrowed, or recoverable by anyone — including ERRERlabs",
     "a flasher's safety copy is a backup that holds it, and restoring it restores the key"),
    ("docs/security/SECURITY_MODEL.md",
     "is no software path that will hand it over",
     "no firmware path"),
    ("docs/security/SECURITY_MODEL.md",
     "It is a statement about software. Someone who physically takes the device and reads the flash chip directly is a different adversary,",
     "a statement about the firmware; a read over the USB port, the flashers' safety copy among them, carries the key"),
    ("docs/security/SECURITY_MODEL.md",
     "Ed25519 private key (which never leaves the device)",
     "no firmware path exports it; a full read of the flash carries it"),
    ("docs/security/SECURITY_MODEL.md",
     "someone who takes a Canary away and opens it up can read the private key out of the flash chip,",
     "it reads over the USB port too, and a flasher's safety copy holds it"),
    ("CHANGELOG.md",
     "is its only way to a new identity on purpose, halted or not. It halts,",
     "the F360 entry carries its amendment: any install in either flasher is"),
    ("CHANGELOG.md",
     "a WAP's bonded Bluetooth provisioning service for a brand-new unit; and the Fleet tab's discovery",
     "the walkthrough entry carries its amendment: that step never listed a WAP (A65)"),
    ("docs/audit/repo_todo_sweep_2026-09.md",
     "its only way to a new identity on purpose, halted or not; the halt removes no path",
     "the F360 note carries its amendment: any install in either flasher is"),
    ("firmware/README.md",
     "Production device provisioning tools are in `provisioning/`:",
     "the opt-in kit; its Phase 2 runs only as a dry run today"),
    ("firmware/README.md",
     "production key + eFuse tooling",
     "the opt-in key + eFuse kit, Phase 2 dry-run only"),
    ("firmware/README.md",
     "# Full provisioning workflow",
     "the default run is Phase 1, which burns no eFuse"),
    ("firmware/canary/include/secure_defaults.h",
     "The Ed25519 private key is stored in NVS and has no read interface.",
     "the firmware has none; the chip's ROM download mode reads the whole flash"),
    ("firmware/canary/include/secure_defaults.h",
     "That is the SOFTWARE boundary.",
     "the FIRMWARE boundary: a flasher's safety copy goes around it"),
    ("firmware/projects/canary-wap/arduino/canary_wap/wap_server.h",
     "Physical BOOT button required for provisioning receipt",
     "the bearer, the setup door's claim, or a tap to the one address asking"),
    ("ios/Sources/SecuraCV/Transport/DeviceAPI.swift",
     "The one-shot receipt handed over after a physical BOOT-tap (Trust-on-Pair).",
     "served to a bearer, a WAP's claim or a tap; the app takes it from a claim or a kit"),
    ("desktop/src/app.js",
     "it was probably reflashed elsewhere. One reflash here mints a fresh one.",
     "or an all-rounder Canary's 10 s hold dropped a seeded token"),
    ("desktop/src/app.js",
     "Copy it if you want it somewhere else too — a reflash mints a new one.</span>",
     "and the all-rounder Canary's 10 s reset drops it"),
    ("desktop/src/app.js",
     "A clean install usually clears this — it wipes stale settings a previous",
     "every install starts the settings region over; the erase adds the partitions an install never writes"),
    ("canary-local/assets/flash-core.js",
     "A clean install usually clears this — it wipes stale settings a previous",
     "the same"),
    ("canary-local/assets/flash.js",
     "the device's settings page shows it again later, and a reflash mints a new one.",
     "the all-rounder Canary's 10 s reset drops it for a derived one"),
    ("docs/flasher_profiles_fleet_book.md",
     "(reflashed elsewhere → reflash here mints a fresh one)",
     "or an all-rounder Canary reset with its 10 s hold"),
    ("docs/onboarding_multiple_canaries.md",
     "Because the handle is derived from immutable hardware, it is **the same every time you flash**",
     "it lasts as long as the key; an install in either flasher makes a new key and handle"),
    ("docs/onboarding_multiple_canaries.md",
     "Each board has a globally-unique hardware identity. The firmware derives a stable per-device handle from it",
     "the handle derives from the key's public fingerprint, never the hardware"),
    ("docs/manual_test_plan_captive_portal.md",
     "a 4-char suffix derived from the device's hardware identity",
     "from the identity key fingerprint, which an install in either flasher replaces"),
    ("docs/manual_test_plan_captive_portal.md",
     "The softAP is always on (AP+STA)",
     "it drops once the home link has held for AP_DROP_GRACE_MS and comes back when it drops"),
    ("docs/FAQ.md",
     "so only a BOOT tap or the token itself fetches it,",
     "the setup door's one-time claim fetches it too"),
    ("canary-local/tools/gen_wap.py",
     "keys and witness chain ride through every reset)",
     "every tap of RESET; the 10 s hold clears the WiFi, and an install starts NVS over"),
    ("canary-local/devices/wap.json",
     "keys and witness chain ride through every reset)",
     "the same, regenerated"),
    ("docs/review/02-roadmap.md",
     "anticipates `nvs_keys` from eFuse). *Acceptance:* device key not recoverable from a config seed. *Status 2026-09:*",
     "the roadmap carries its amendment: no Canary build reads nvs_keys"),
    ("docs/review/02-roadmap.md",
     "`nvs_keys` for this (but fix its 4 MB assumption — F-06). 6. **Always keep big buffers in PSRAM**",
     "the same"),
    # #1843's sweep, sixth pass: the remaining copies of the corrected
    # positions, each where it stood (the forms are the text as the pass
    # found it; the replacements are what stands there now).
    ('docs/unflashed_board_intake.md',
     'Phase 2 lockdown, if you want to close the door behind you',
     'as tooled Phase 2 turns on neither Secure Boot nor flash encryption, and protects neither the key nor the Wi-Fi in NVS'),
    ('docs/security/SECURITY_MODEL.md',
     'domestic abuser who does not take the device to a lab',
     'no lab is needed: a laptop and a USB cable read the key through the ROM download mode'),
    ('firmware/PARTITIONS.md',
     '**Secure / flash‑encrypted**',
     "the kit's reference table; Phase 2 turns on neither Secure Boot nor flash encryption"),
    ('firmware/PARTITIONS.md',
     'The `encrypted` flag is enforced only after the Phase‑2 eFuse burn',
     'the burn sets no SPI_BOOT_CRYPT_CNT, so the flag takes effect on no board any tool here makes'),
    ('firmware/PARTITIONS.md',
     '(Phase‑2 secure/encrypted)',
     "the Phase 2 kit's compile-only env, which turns on neither protection"),
    ('docs/sd_card_health.md',
     'or reflash with a full chip erase',
     'any install in either flasher starts NVS over, the identity key and chain state with it'),
    ('docs/canary_settings_access_validation.md',
     '| API token fallback | Paste the `cv_...` API token into the auth modal',
     "in a browser with no session: on a session page the box takes the cookie's word for any token"),
    ('docs/design/usb_onboard.md',
     'Re-seat the card, reboot, done.',
     'only a head left behind by a power cut is rebuilt; a new key never adopts the old tail'),
    ('docs/browser_flasher.md',
     '— installing keeps it, updated in place.',
     'an install starts the settings over with a new key; an over-the-air update keeps them'),
    ('docs/browser_flasher.md',
     "the iOS app's `BLEConsole.swift` speaks the same UUIDs",
     "it knows them but never dials a WAP's console (A65)"),
    ('docs/design/magic_pairing.md',
     "the window is re-armed from now — the owner's own act",
     "the re-armed door is first-come, and a WAP's Forget Wi-Fi is reachable by any landing session (F345)"),
    ('docs/design/magic_pairing.md',
     "The WAP's bearer token lasts for the life of the device.",
     'until an install or a full erase in a flasher; nothing rotates it'),
    ('docs/design/magic_pairing.md',
     'or, after a reset, one whose owner chose to reopen it',
     'a cleared unit that still has an owner, and on a WAP any landing session can send Forget Wi-Fi'),
    ('docs/design/iphone_companion_app.md',
     'a native app erases that footnote',
     "the app reaches the setup door and the beacons, not a WAP's console or bonded lane (A65)"),
    ('docs/design/iphone_companion_app.md',
     'bonded) + chirp/beacon scan — off-grid status.',
     'the step carries its as-shipped amendment: the console reaches no WAP (A65)'),
    ('docs/firmware_ota.md',
     '— a USB reflash always works too',
     'an install works but starts the settings over under a new key'),
    ('docs/FAQ.md',
     "So a Canary on your network offers nothing to a stranger's phone",
     "the setup door does not; a WAP's Forget Wi-Fi and bonded lane are the caveats (F345, F346)"),
    ('docs/security/THREAT_MODEL.md',
     'Private keys are not "access-controlled" — there is no read interface',
     'the firmware has none; a whole-flash read still recovers the key'),
    ('docs/security/THREAT_MODEL.md',
     'Ed25519 private key has no read/export interface of any kind',
     'the firmware offers none; a full flash read recovers it on every tier'),
    ('docs/design/flasher_experience.md',
     'no secret ever leaves the device — same rule as the trust card',
     "the firmware exports no key, and the trust card says a flasher's safety copy carries it"),
    ('docs/strategy/08-product-strategy.md',
     'and your proof is unforgeable.',
     "tamper-evident: forging takes the device's key, which a seized device gives up"),
    ('canary-vision/docs/firmware-integration.md',
     'The private key never leaves NVS',
     'no firmware path exports it; a full flash read carries it'),
    ('canary-vision/docs/discovery.md',
     '# stable device identifier',
     'kept across OTA updates and the 10 s hold, not promised across an install'),
    ('spec/beacon_channel_v0.md',
     'All endpoints Bearer-token-gated identically to',
     'they take api_auth_check(), the bearer or a landing session (F346)'),
    ('spec/canary_mesh_network_v0.md',
     'at the cost of refusing to work on dev boards without FE',
     'every released board is FE-off, so nothing persists on any of them'),
    ('docs/V1_LAUNCH_REVIEW.md',
     'still runs for the current boot on an FE-off dev board;',
     'the note carries its amendment: every released board is FE-off'),
    ('firmware/LESSONS_LEARNED.md',
     'credentials vanishing at runtime (factory reset) re-arm the window',
     "the lesson carries its amendment: a WAP's Forget Wi-Fi does too, from any landing session"),
    ('firmware/LESSONS_LEARNED.md',
     'The bearer token lasts for the life of the device. Nothing that lasts',
     'the lesson carries its amendment: until an install or a full erase in a flasher'),
    ('firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md',
     'one BOOT tap releases the provisioning receipt',
     'the tap goes to one asker, and an unasked tap to the first asker after it (F342)'),
    ('firmware/projects/canary-wap/README.md',
     "which are the owner's ways to re-arm the window",
     'any landing session can send Forget Wi-Fi (F345)'),
    ('firmware/projects/canary-wap/README.md',
     'a short press opens the 30 s receipt gate, a 2 s hold',
     'the tap goes to one asker, so the page asks first and the press comes second'),
    ('firmware/projects/canary-wap/README.md',
     '**Session Rotation:** Tokens rotate every 4 hours',
     'the RF presence session tokens rotate; the API token never does'),
    ('firmware/README.md',
     '- Local-first data storage - Session token rotation',
     'RF-presence session tokens rotate; the API token does not'),
    ('ios/README.md',
     "the device's own PWA has to send people to Bluefy; native erases that",
     "the app reaches no WAP's console or bonded lane (A65)"),
    ('ios/README.md',
     'Tamper heard over a BLE console NOTIFY re-evaluates immediately',
     "no WAP's console connects (A65); the chirp and the sentinel carry tamper"),
    ('firmware/canary/lib/securacv_witness/src/securacv_witness.cpp',
     '(or an NVS wipe/reflash while the card kept its',
     'a wipe or an install gives a new key, so the old tail never verifies'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '(or an NVS wipe/reflash while the card kept its',
     'an erase or an install gives a new key, so the old tail never verifies'),
    ('firmware/canary/lib/securacv_usb_onboard/src/securacv_usb_onboard.cpp',
     'To recover a device that lost its NVS head, just re-seat the',
     'only a head a power cut left behind is rebuilt; a new key keeps the old card unadopted'),
    ('firmware/common/network/auth_backoff.h',
     'an OTA update and the 10 s BOOT reset leave the token',
     "a WAP's hold keeps it; the flagship's 10 s reset re-derives it, dropping a seeded one"),
    ('firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h',
     'an OTA update and the 10 s BOOT reset leave the token',
     'the same, in the staged copy'),
    ('firmware/common/network/improv_core.h',
     '(and again after a factory reset wipes them at runtime)',
     "a WAP's Forget Wi-Fi, or the hold before its restart"),
    ('firmware/common/network/improv_core.h',
     '(a factory reset) re-arms the window from now — that is the',
     "any landing session can send a WAP's Forget Wi-Fi (F345): not proof of the owner"),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     '(and again after a factory reset wipes them at runtime)',
     'the same, in the staged copy'),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     '(a factory reset) re-arms the window from now — that is the',
     'the same, in the staged copy'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     "(wifi_clear_credentials: the owner's own act re-arms the",
     'the hold, or Forget Wi-Fi from any landing session (F345)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     "The owner's own act re-arms the Bluetooth setup door's first-boot",
     "a credential wipe does, and Forget Wi-Fi is any landing session's (F345)"),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'LAN-local curl could hit /api/wifi/forget and force a re-provisioning',
     'the routes take the cookie any LAN host can mint, so any host still can (F345)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/bluetooth_api.h',
     'secureFetch already attaches `Authorization: Bearer <api_token>` on',
     'the gate takes the cookie any landing session holds (F346)'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     'IS sensitive metadata (an attacker who reads the',
     'the gate keeps it off FE-off boards only; FE does not cover NVS'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     'reboot of an FE-off dev board',
     'every released board is FE-off'),
    ('firmware/canary/src/csi_modules_integration.cpp',
     'flash encryption disabled on a dev board',
     'every released board: no tier turns flash encryption on'),
    ('firmware/canary/lib/securacv_network/src/securacv_network.cpp',
     'pubkeys needed flash encryption, so a dev board answered 500',
     'every released board answered it'),
    ('firmware/canary/src/main.cpp',
     'typical on FE-disabled dev hardware where save refuses',
     'on every Canary today'),
    ('firmware/scripts/check_canary_mesh_status.py',
     'so a dev board answered 500 `load_failed`',
     'every board answered it: no Canary has flash encryption on'),
    ('scripts/tests/test_canary_mesh_status_wiring.py',
     'the dev-board 500 F199 removed',
     'the 500 every board answered'),
    ('firmware/scripts/regression_check.sh',
     'NVS is not compatible with flash encryption: ESP-IDF protects it with NVS',
     'no Canary build has NVS encryption; NVS is plaintext on every tier'),
    ('firmware/scripts/regression_check.sh',
     'NVS is protected by NVS encryption (nvs_keys), not by flash encryption',
     'only NVS encryption could, and it is unavailable under framework = arduino'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_provision.h',
     'Replaces (well — augments) the captive-AP setup flow with a phone-pairs-',
     'the iPhone app never reaches this service on a WAP (A65)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_provision.h',
     'capped at 5 attempts per hour to',
     'ten an hour, one budget for every bonded peer (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_provision.h',
     'so 10/hour is a comfortable security ceiling',
     "a hostile bonded peer keeps the owner's write refused (F346)"),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_console.h',
     'so a peer holding the bond has the same trust level as one holding',
     'any landing session can bond a peer of its own (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/household_api.h',
     'All endpoints require bearer-token auth via api_auth_check',
     'api_auth_check takes the landing session too (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.h',
     'every route behind the Bearer-token trampoline',
     'the trampoline is api_auth_check, the bearer or a landing session (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/beacon_api.h',
     '<api_token> header is required for any successful response',
     'a landing session gets one too (F346)'),
    ('firmware/projects/canary-wap/tests_host/test_bluetooth_commands_wap.cpp',
     "passkey reaches the channel and waits for the owner, only the owner's",
     'any landing session can send the confirm (F346)'),
    ('firmware/tests_host/test_provisioning_gate.cpp',
     'click Use BOOT Tap Once again, and only then',
     'the press comes after the device answers the new ask with 403'),
    ('firmware/tests_host/test_improv_core.cpp',
     'The owner wiped the credentials (a factory reset that did not reboot).',
     "a WAP's Forget Wi-Fi, from any landing session (F345)"),
    ('firmware/tests_host/test_improv_core.cpp',
     "Only the owner's tap reopens it.",
     'the credentials vanishing reopens it too, first-come'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_improv.cpp',
     '(a factory reset re-armed the window) is a new',
     'a runtime wipe: Forget Wi-Fi, or the hold before its restart'),
    ('firmware/projects/canary-wap/tests_host/test_provisioning_logic.cpp',
     '(the dashboard that issued the reset)',
     'the dashboard sends Forget Wi-Fi; the reset is the physical hold'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'Provisioning gate stays open for this long after a BOOT press, so a',
     'the tap goes to one asker (F342): ask first'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'Forget Wi-Fi on the dashboard resets the network only',
     'it clears the saved Wi-Fi and reopens the first-come setup door'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '(receipt available for %lu seconds)',
     'the receipt goes to one asker; an unasked tap, to the first asker after it'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'receipt gate open 30s; hold 2-3s',
     'the receipt goes to one asker: ask first'),
    ('canary-local/tools/gen_wap.py',
     'receipt gate open 30s; hold 2-3s',
     "the same, the banner's literal"),
    ('canary-local/devices/wap.json',
     'receipt gate open 30s; hold 2-3s',
     'the same, regenerated'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wap_server.h',
     '// + last 4 chars of MAC',
     "unused; the SSID's suffix is the key fingerprint's, never the MAC"),
    ('firmware/projects/canary-wap/arduino/canary_wap/familiar.cpp',
     'flash dump alone cannot replay which fingerprints were recorded',
     'the salt sits in plaintext NVS beside the filters'),
    ('firmware/projects/canary-wap/arduino/canary_wap/familiar.cpp',
     'Why salt? Prevents flash-dump recovery of fingerprints',
     'per-device hashing only; a dump holds the salt'),
    ('firmware/projects/canary-wap/arduino/canary_wap/familiar.h',
     'Hashes are salted with a per-device secret kept in NVS; flash',
     'plaintext NVS: a dump holds the salt with the filters'),
    ('firmware/common/color/plumage.h',
     'Derived from the device id; stable for',
     'the id is kept across OTA updates, not always across an install'),
    ('src/bridge/hap/pairing.rs',
     'a long-term pairing key is exactly the kind of',
     'the gate keeps it off FE-off boards but does not make it confidential'),
    ('src/bridge/hap/store.rs',
     'get the `opera_secret` treatment and are refused',
     'the same, with the plaintext-NVS caveat'),
    ('canary-local/assets/flash.js',
     'the SecuraCV iPhone app (its App Store release is still pending)',
     "the iPhone app does not read a WAP's console (A65)"),
    ('canary-local/assets/flash.js',
     'the SecuraCV app talks to a Canary over Bluetooth',
     "it does not read a WAP's Bluetooth console (A65)"),
    ('canary-local/assets/flash.js',
     '(that’s deliberate — same trust as the WiFi token)',
     'any landing session can bond a peer (F346)'),
    ('canary-local/assets/flash.js',
     'keys, passwords and tokens are never read out of the board, here or anywhere.',
     'the check reads the settings region and the safety copy saves the whole flash'),
    ('canary-local/assets/flash-core.js',
     '// identity key exists (never read)',
     'the NVS read carries it; it is never shown'),
    ('canary-local/assets/flash-core.js',
     'BLEConsole.swift speaks the same UUIDs',
     'the app never dials a WAP for them (A65)'),
    ('canary-local/assets/flash-core.js',
     'runs ${cur.name} — installing keeps it,',
     'an install starts the settings over under a new key'),
    ('canary-local/assets/flash-core.js',
     'the saved WiFi is cleared, so the board comes up with its setup network again',
     'a new witness key too, where the board keeps one'),
    ('desktop/src/app.js',
     'installing keeps it, updated in place.',
     'an install starts the settings over under a new key'),
    ('desktop/src/app.js',
     'so the check is a pointer — to the iPhone app, or to',
     'the browser Lab only (A65)'),
    ('desktop/src/app.js',
     'the SecuraCV iPhone app (on TestFlight by invite; App Store release pending), or in the',
     "the iPhone app does not read a WAP's console (A65)"),
    ('desktop/src/app.js',
     'stays the stable slug written during provisioning',
     'an install in either flasher can change it'),
    ('desktop/src/index.html',
     'so the check runs in the SecuraCV iPhone app',
     'the browser Lab only (A65)'),
    ('canary-local/tests/desktop_parity.test.js',
     'SecuraCV iPhone app[\\s\\S]',
     'the pin now requires the browser Lab'),
    ('canary-local/tests/desktop_parity.test.js',
     'flat.includes("the SecuraCV iPhone app") && flat.includes',
     'the pin forbids naming the iPhone app as where the check runs'),
    ('desktop/flash-engine/src/changemap.rs',
     '(its own tuning, witness-chain counters) starts fresh.',
     'the witness key starts fresh too, where the board keeps one'),
    ('desktop/flash-engine/src/changemap.rs',
     'the saved Wi-Fi is cleared, so the board',
     'a new witness key too, where the board keeps one'),
    ('canary-local/tools/gen_flash.py',
     'secrets ever touch the browser.',
     'the pre-install safety copy reads the whole flash, key and saved passwords included'),
    ('canary-local/devices/flash.json',
     'No secrets ever touch the browser.',
     'the same, regenerated'),
    ('canary-local/assets/hatchery.js',
     'functional id stays its stable provisioned slug',
     'kept across OTA updates and the hold; an install can change it'),
    ('canary-local/devices/hatch.json',
     'functional id stays a stable slug',
     'the same'),
    ('canary-local/assets/wap-ui.js',
     'app running — BOOT does nothing until the next reset',
     'the running WAP reads BOOT: a tap opens the gate, 10 s is a factory reset'),
    ('canary-local/assets/wap-ui.js',
     "(BOOT held — nothing happens: it's only sampled at reset)",
     'held 10 s it resets and reopens the first-come setup door'),
    ('ios/Sources/SecuraCV/App/FleetStore.swift',
     'Tamper heard over a BLE NOTIFY is the fastest signal the fleet',
     "no WAP's console connects (A65)"),
    ('ios/Sources/SecuraCV/App/FleetStore.swift',
     'Tier 1a: the WAP-class GATT console snapshot',
     'none for a WAP today (A65)'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     'GATT CONSOLE (WAP-class enrichment). canary-wap additionally serves a',
     'a WAP does not advertise the console, so the layer never fills (A65)'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     'A compact snapshot pushed by a WAP-class device over BLE NOTIFY.',
     'the app never dials a WAP for it (A65)'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     'BLE NOTIFY is sub-second, so the alert loop gets',
     'no console connects today, so it never fires (A65)'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     'Tamper over BLE NOTIFY is the fastest signal the fleet has',
     'the chirp and the sentinel carry tamper (A65)'),
    ('ios/Sources/SecuraCV/Transport/FleetWiFiRunner.swift',
     'Not reachable right now — over Wi-Fi or Bluetooth.',
     'every candidate is a WAP, whose Bluetooth lane the app never reaches (A65)'),
    ('ios/Tests/SecuraCVTests/FleetWiFiRolloutTests.swift',
     'func testADarkWAPInRangeRidesTheBLERescue()',
     'policy only: no WAP fills bleReachable (A65)'),
    ('ios/Tests/SecuraCVTests/FleetWiFiRolloutTests.swift',
     'func testADarkOutOfRangeWAPIsUnreachable()',
     'every dark WAP is unreachable, in range or not (A65)'),
    ('ios/Sources/SecuraCV/Model/CanaryVoiceStage.swift',
     'from then on, nothing else speaks for it.',
     'a different key is flagged; a flash read can copy the key'),
    ('ios/Sources/SecuraCV/Model/Witness.swift',
     '// stable device_id, e.g.',
     'kept across OTA updates and the hold; an install can change it'),
    ('ios/Support/Info.plist',
     'so you still get status if the network drops',
     "the app hears beacons and the setup door; it reaches no WAP's console (A65)"),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     'Paste the API token, or use BOOT Tap Once.',
     "on a session page the token box takes the cookie's word"),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     'Paste the token from the device receipt or Serial output.',
     "a signed-in page can't check a typed token"),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     'so a press anywhere in the 60 s window lands',
     'a press lands while the page alone asks (F342)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/recovery_kit.test.js',
     'Paste the API token, or use BOOT Tap Once.',
     'the pin holds the line that names no token box'),
    ('firmware/projects/canary-wap/arduino/canary_wap/recovery_kit.test.js',
     'the old order still works: pressed first, the first ask is served',
     'a press before Save is served only when no other address asks first'),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     'iOS: requires the Bluefy browser (App Store)',
     'unverified for a WAP (A65); the page points iPhone owners at the dashboard'),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     'iOS: install <a href="https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055"',
     'the same, in the footer'),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     'On iOS, install Bluefy from the App Store to use the Bluetooth console.',
     'the same, in the capability notice'),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     'Web Bluetooth not supported by this browser. On iOS, install Bluefy.',
     'the same, in the connect error'),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     'the old order (press first, then Save) is served on the',
     'only if this page is the first asker after the press'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wizard_logic.test.js',
     'missing Web Bluetooth points at Bluefy',
     'the pin forbids Bluefy (A65)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     'tell iOS users to install Bluefy',
     'the notice names the browsers that have Web Bluetooth (A65)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     'hide the Bluefy footer along with the banner',
     'the footer names no Bluefy now'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'Every endpoint Bearer-token-gated via the chirp_api template trampoline.',
     'the trampoline is api_auth_check: the bearer or a landing session (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'Bearer-gated from day one (see spec/beacon_channel_v0.md §10).',
     'auth-gated by api_auth_check, which takes a landing session (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/beacon_api.h',
     'paged audit log (Bearer-gated).',
     'behind bcn_auth_gated, which takes a landing session'),
    ('firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.cpp',
     '(beacon_api.h, Bearer-gated, from canary_wap.ino)',
     'behind api_auth_check, which a landing session passes'),
    ('firmware/projects/canary-wap/arduino/canary_wap/chirp_api.h',
     'Register all chirp API routes with the HTTP server, Bearer-gated.',
     'chirp_auth_gated is api_auth_check, the bearer or a landing session (F346)'),
    ('docs/security/THREAT_MODEL.md',
     'REST API (`/api/chirp/*`) Bearer-token-gated when wired into',
     'behind api_auth_check(), the bearer or a landing session (F346)'),
    ('docs/security/THREAT_MODEL.md',
     '(behind `FEATURE_BEACON_CHANNEL`, Bearer-gated)',
     'behind api_auth_check(), which a landing session passes'),
    ('spec/chirp_channel_v0.md',
     'REST API Bearer-gated (C12)',
     'the revision note carries its amendment: api_auth_check() takes a landing session (F346)'),
    ('CHANGELOG.md',
     'installing keeps it, updated in place"); else the chip',
     'the entry carries its amendment: an install starts the settings over under a new key'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     'on a dev board without flash encryption every peers read fails',
     'the F199 note carries its amendment: every released board is without it'),
    # #1843's sweep, sixth pass, round-1 review: the copies the pass
    # missed and the replacements it wrote that were themselves untrue
    # (the tap, a cleared unit's owner, the WAP's gate, the old card, the
    # released framework, the console's board, a restore's token).
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'The BOOT tap stays open this long for the one page already asking:',
     'the tap goes to one asker; an unasked tap goes to the first asker after it'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'the receipt goes only to the one address already asking (ask first, then tap)',
     'with no page asking, the first address to ask gets it (tap_arbitrate)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'receipt to the one page already asking (30s)',
     'an unasked tap goes to the first asker after it'),
    ('canary-local/tools/gen_wap.py',
     'receipt to the one page already asking (30s)',
     "the same, the banner's literal"),
    ('canary-local/tools/gen_wap.py',
     'hands the recovery kit to the one page already asking for it',
     'a tap no page was asking for goes to the first address that asks after it'),
    ('canary-local/tools/gen_wap.py',
     'hands the recovery kit only to the one page already asking for it',
     "the guide's pin follows the guide"),
    ('canary-local/devices/wap.json',
     'receipt to the one page already asking (30s)',
     'the same, regenerated'),
    ('canary-local/devices/wap.json',
     'hands the recovery kit to the one page already asking for it',
     'the same, regenerated'),
    ('firmware/projects/canary-wap/README.md',
     'receipt gate for the one page already asking for it',
     'a tap no page was asking for goes to the first address that asks after it'),
    ('docs/getting_started_canary.md',
     'hands the recovery kit only to the one page already asking for it',
     'an unasked tap goes to the first address that asks after it'),
    ('docs/design/magic_pairing.md',
     'A short press opens the 30 s receipt gate, a 2 s hold',
     'the press hands the receipt to one asker, and the page asks first'),
    ('canary-local/assets/wap-ui.js',
     'a tap opens the receipt gate, 10 s held is a factory reset',
     'a tap hands the receipt to one asking page, so the page asks first'),
    ('canary-local/assets/wap-ui.js',
     'broker and name and reopens its first-come Bluetooth setup door for half an hour)',
     'unless a landing session turned Bluetooth off, which the hold keeps (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/recovery_kit.test.js',
     "The tap goes to an address that is already asking, and the page's",
     'a tap no page was asking for goes to the first asker after it'),
    ('CHANGELOG.md',
     'press hands it to the one page already asking, so click',
     'an unasked tap goes to the first address that asks after it'),
    ('firmware/common/network/improv_core.h',
     "owner cleared), and for an owner's own tap — never for a device that",
     "the header's rule reads 'one whose network was cleared': on a WAP any landing session can send Forget Wi-Fi (F345)"),
    ('firmware/common/network/improv_core.h',
     'NO CREDENTIALS STORED (first boot, or after the owner cleared them:',
     "after they were cleared; Forget Wi-Fi is any landing session's (F345)"),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     "owner cleared), and for an owner's own tap — never for a device that",
     'the same, in the staged copy'),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     'NO CREDENTIALS STORED (first boot, or after the owner cleared them:',
     'the same, in the staged copy'),
    ('docs/design/magic_pairing.md',
     "yet, or one its owner cleared), and for an owner's own tap",
     "the header's sentence, as it now reads"),
    ('docs/design/magic_pairing.md',
     'A unit its owner cleared (the 10 s hold, or Forget Wi-Fi on a WAP)',
     'a cleared unit; on a WAP any landing session can send Forget Wi-Fi (F345)'),
    ('docs/design/magic_pairing.md',
     'The owner re-arms it with **Forget Wi-Fi** on the dashboard',
     'Forget Wi-Fi re-arms it from any landing session (F345)'),
    ('docs/design/magic_pairing.md',
     'a new unit, one its owner cleared with the hold or Forget Wi-Fi',
     'one cleared with the hold or with Forget Wi-Fi, which any landing session can send'),
    ('docs/design/magic_pairing.md',
     'a new one, or one its owner cleared, §2.2',
     'one whose network was cleared'),
    ('docs/ble_protocol.md',
     "or one its owner cleared), and for an owner's own tap",
     "the header's sentence, as it now reads"),
    ('docs/ble_protocol.md',
     '(first boot, or after the owner cleared them with the 10 s factory-reset hold',
     "after they were cleared; Forget Wi-Fi is any landing session's (F345)"),
    ('docs/GLOSSARY.md',
     "or one its owner cleared), and for an owner's own tap",
     "the header's sentence, as it now reads"),
    ('docs/GLOSSARY.md',
     'does the owner clearing the credentials with the 10 s factory-reset hold',
     "clearing them re-arms it; Forget Wi-Fi is any landing session's (F345)"),
    ('docs/FAQ.md',
     "or one its owner cleared), and for an owner's own tap",
     "the header's sentence, as it now reads"),
    ('docs/README.md',
     'or one its owner cleared, which still has an owner',
     'one whose network was cleared, which still has an owner'),
    ('docs/TRUST_MODEL.md',
     '(a first boot, or after the owner cleared it)',
     "after it was cleared; Forget Wi-Fi is any landing session's (F345)"),
    ('firmware/LESSONS_LEARNED.md',
     "or one its owner cleared), and for an owner's own tap",
     'the amendment quotes the header as it now reads'),
    ('firmware/projects/canary-sense/README.md',
     "or one its owner cleared — and for an owner's own tap",
     "the header's rule, as it now reads"),
    ('firmware/projects/canary-vision/README.md',
     "or one its owner cleared), and for an owner's own tap",
     "the header's rule, as it now reads"),
    ('docs/firmware_ota.md',
     'All endpoints are bearer-token gated on canary and canary-wap.',
     "the WAP's take api_auth_check(): any LAN host can install the signed release or move the update server (F346)"),
    ('firmware/common/network/node_status.h',
     'two trees served a rich bearer-gated status',
     "the WAP's gate takes a landing session (F346)"),
    ('firmware/common/network/node_status.h',
     'the WAP, whose /api/status is bearer-gated and richer',
     "the WAP's takes api_auth_check(), the bearer or a landing session (F346)"),
    ('firmware/projects/canary-wap/arduino/canary_wap/node_status.h',
     'two trees served a rich bearer-gated status',
     'the same, in the staged copy'),
    ('firmware/projects/canary-wap/arduino/canary_wap/node_status.h',
     'the WAP, whose /api/status is bearer-gated and richer',
     'the same, in the staged copy'),
    ('firmware/projects/canary-display/arduino/canary_display/node_status.h',
     'two trees served a rich bearer-gated status',
     "the same, in the display's staged copy"),
    ('firmware/projects/canary-display/arduino/canary_display/node_status.h',
     'the WAP, whose /api/status is bearer-gated and richer',
     "the same, in the display's staged copy"),
    ('docs/PARITY.md',
     '| **canary-wap** | ✅ | ✅ bearer-gated ¹⁰ |',
     "the WAP's /api/status takes api_auth_check(), which a landing session passes (F346)"),
    ('docs/PARITY.md',
     'The WAP and the PIO canary answer `/api/status` behind their bearer token',
     "the PIO canary's takes the bearer; the WAP's also a landing session (F346)"),
    ('docs/PARITY.md',
     'rides inside the bearer-gated status on the two that already had one',
     "the WAP's status is not the bearer's alone (F346)"),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'Rename this device → canary-<name>.local (Bearer-gated).',
     'handle_device_name takes api_auth_check(), which a landing session passes (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'Authenticated API endpoints (Bearer token required)',
     'api_auth_check(): a landing session or the bearer (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'These wrappers add Bearer token authentication to existing handlers.',
     'the wrappers run api_auth_check(): the cookie first, then the bearer (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     "pair_token_valid. The dashboard's secureFetch already supplies the",
     'secureFetch sends a bearer only when the page holds a token (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     'Bearer-gated mDNS browser for the operator UI; this one is presence-only and',
     '/api/fleet-scan takes api_auth_check(), which a landing session passes (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/airtime_governor.cpp',
     '`cosign_pending_request` REST path (Bearer-gated, same task).',
     'bcn_auth_gated runs api_auth_check(), which a landing session passes (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/chirp_api.h',
     'register_routes() now Bearer-gates every endpoint via the',
     'chirp_auth_gated runs api_auth_check(), the bearer or a landing session (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/household_api.h',
     "The dashboard's secureFetch already attaches `Authorization: Bearer",
     'secureFetch sends a bearer only when the page holds a token (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/api_auth.h',
     'Provides Bearer token authentication for the REST API with:',
     "api_auth_check() takes the landing session's cookie first (F346)"),
    ('firmware/projects/canary-wap/arduino/canary_wap/wap_server.h',
     '- Bearer token authentication on protected endpoints',
     "api_auth_check(): the landing session's cookie, or the bearer (F346)"),
    ('firmware/projects/canary-wap/arduino/canary_wap/csi_integration.cpp',
     'api_auth_check() — Bearer token gate',
     'it takes the session cookie or the bearer (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wap_diagnostics.h',
     '(canary_wap.ino handle_diagnostics, token-gated,',
     'handle_diagnostics_auth runs api_auth_check(), which a landing session passes (F346)'),
    ('firmware/projects/canary-wap/tests_host/check_route_security.py',
     '(that stays behind the Bearer-gated /api/fleet-scan and the break-glass',
     '/api/fleet-scan takes api_auth_check(), which a landing session passes (F346)'),
    ('docs/FIRMWARE_MAP.md',
     'bearer-gated `/api/ota/{status,check,install,config}` on canary and WAP',
     "the WAP's take api_auth_check(), which a landing session passes (F346)"),
    ('docs/homeassistant_setup.md',
     'return `csi_event_egress` from their token-gated `GET /api/diagnostics`',
     "the WAP's also passes a landing session (F346)"),
    ('docs/manual_test_plan_captive_portal.md',
     'their bearer-gated `/api/status` under `node_status`',
     "the WAP's takes api_auth_check(), which a landing session passes (F346)"),
    ('docs/canary_settings_access_validation.md',
     'WiFi access alone does not authorize settings API calls.',
     'the landing page mints a session for any host on the WiFi (F346)'),
    ('spec/witness_api_v1.md',
     'all behind the same Bearer token.',
     'on canary-wap the gate also takes a landing session (F346)'),
    ('spec/witness_api_v1.md',
     'holder of the Bearer token can confirm a guess at a low-entropy payload',
     'any caller the gate admits can, a landing session on a WAP included (F346)'),
    ('tvos/discovery/DISCOVERY.md',
     'sealed evidence stays behind the Bearer-gated `/api/fleet-scan`',
     'on a WAP it takes api_auth_check(), which a landing session passes (F346)'),
    ('ios/Sources/SecuraCV/Transport/DeviceAPI.swift',
     "`GET /api/status` — the WAP's Bearer-gated status body",
     "the WAP's gate also takes a landing session's cookie (F346)"),
    ('ios/Sources/SecuraCV/Transport/DeviceAPI.swift',
     'committed ring rows newest-first, ≤72 rows, Bearer-gated;',
     'gated like /api/status: a landing session passes too (F346)'),
    ('ios/Sources/SecuraCV/Transport/DeviceAPI.swift',
     'SSE; Bearer-gated; Wire/WapEvents.swift documents its three variant',
     'gated like /api/status: a landing session passes too (F346)'),
    ('ios/Sources/SecuraCV/Views/Components/SensingNowSection.swift',
     '(the stream is Bearer-gated; it would only 401 forever).',
     'the app reaches it only with the bearer; the gate also takes a landing session (F346)'),
    ('ios/Sources/SecuraCV/Views/DeviceDetailView.swift',
     '(the stream is Bearer-gated; an unpaired row or a Keychain-',
     'the app reaches it only with the bearer; the gate also takes a landing session (F346)'),
    ('ios/Sources/SecuraCV/Wire/WapEvents.swift',
     '64 rows, Bearer-token-gated (csi_integration.cpp:handle_events_today).',
     "CSI_AUTH_OR_RETURN takes a landing session's cookie first (F346)"),
    ('ios/Sources/SecuraCV/Wire/WapEvents.swift',
     'a plain 1 Hz GET snapshot (not SSE), Bearer-gated,',
     'gated like /api/events/today: a landing session passes too (F346)'),
    ('ios/Tests/SecuraCVTests/WapEventsTests.swift',
     'open the sensing feed to anyone on the LAN — a privacy regression',
     'any LAN host can already mint a landing session that passes the gate (F346)'),
    ('ios/Tests/SecuraCVTests/WapEventsTests.swift',
     'the feed must stay token-gated',
     'the gate takes a landing session too (F346)'),
    ('CHANGELOG.md',
     '`handle_witness_v1`, Bearer-gated, chunked)',
     'the entry carries its amendment: api_auth_check() takes a landing session (F346)'),
    ('CHANGELOG.md',
     '(`beacon_api.h`): Bearer-token-gated surface —',
     'the entry carries its amendment: api_auth_check() takes a landing session (F346)'),
    ('CHANGELOG.md',
     'template-trampoline pattern; wired into `canary_wap.ino`.',
     'the entry carries its amendment: api_auth_check() takes a landing session (F346)'),
    ('docs/audit/mesh_and_chirp_audit_v1.md',
     'and `household_api.h` (PR #438). `POST /api/chirp/send` additionally',
     'the fix carries its amendment: api_auth_check() takes a landing session (F346)'),
    ('docs/audit/v0.3_closeout.md',
     '| Bearer-token template trampoline; wired in INO |',
     'the row carries its amendment: api_auth_check() takes a landing session (F346)'),
    ('docs/audit/v0.3_closeout.md',
     '**Bearer-token-gated REST surface** on `/api/beacon/*`',
     'the bullet carries its amendment: api_auth_check() takes a landing session (F346)'),
    ('docs/design/iphone_companion_app.md',
     "the device's own `/companion` PWA has to tell people to install",
     "the page sends an iPhone owner to the WAP's dashboard; it names no Bluefy (A65)"),
    ('docs/design/usb_onboard.md',
     "gives a new key, so the old card's chain stays verifiable off-device",
     "left in, the card takes the new key's records in the same file, which the verifier fails"),
    ('firmware/canary/lib/securacv_usb_onboard/src/securacv_usb_onboard.cpp',
     'device has a new key: the old card is kept, not adopted.',
     'copy the old log off or use a fresh card: new-key records join the old file'),
    ('firmware/canary/lib/securacv_witness/src/securacv_witness.cpp',
     'history is ahead of ours is refused for the same reason, instead of',
     'refused only while its tail is ahead; then a later mount appends to that file'),
    ('firmware/canary/lib/securacv_witness/src/securacv_witness.cpp',
     'SD history ahead of this chain - reboot to reconcile',
     'a reboot never reconciles a card another key signed'),
    ('firmware/PARTITIONS.md',
     '(every one is `framework = arduino`)',
     'every released build; the canary-ota sample is ESP-IDF with NVS encryption on'),
    ('firmware/scripts/regression_check.sh',
     'no Canary build has (every one is framework = arduino',
     'every released build; the canary-ota sample sets CONFIG_NVS_ENCRYPTION'),
    ('firmware/scripts/regression_check.sh',
     'it is unavailable under framework = arduino, so NVS stays plaintext either way',
     'on the framework every released build uses'),
    ('firmware/provisioning/partitions_secure.csv',
     'Canary build reads it (every one is framework = arduino',
     'every released build; canary-ota has no nvs_keys partition'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     'flash learns the household graph either way.',
     'without FE the gate keeps it out of NVS; with FE on, a flash read still learns it'),
    ('desktop/src/app.js',
     'The read-only Bluetooth console runs on the AP-provisioned Canaries and the',
     'the WAP alone serves it (ble_console.h)'),
    ('canary-local/assets/flash.js',
     'AP-based Canaries (canary / WAP) run the read-only Bluetooth console, so',
     'the WAP alone serves it (ble_console.h)'),
    ('docs/browser_flasher.md',
     'every board, C3/C6 included, advertises `SecuraCV-Canary` by name',
     'only a WAP serves the console (ble_console.h)'),
    ('docs/browser_flasher.md',
     'after flashing an AP/WAP board) answers three plain questions',
     'offered after flashing a WAP, the one board that serves the console'),
    ('firmware/projects/canary-wap/README.md',
     'and only an install or a full erase in a flasher replaces it',
     "a flasher's restore of a safety copy puts back the token that copy holds"),
    ('docs/design/magic_pairing.md',
     'is installed again or fully erased in a flasher: OTA updates',
     "or a flasher restores a safety copy over it, putting back that copy's token"),
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
    # The tier and way-out prose that replaced #1843's fourth-review bans.
    "so on every tier today the key is recoverable by reading the flash "
    "directly, on a fused board too (Scenario 4 below)",
    "Flash encryption (would cover the app image at rest, not NVS, where the "
    "keys and the saved passwords stay plaintext on every tier today; prepared "
    "in the provisioning kit, not yet enabled on any tier, Scenario 4)",
    "someone who takes a Canary away and opens it up can read the private key "
    "out of the flash chip, in its default configuration and, today, on a "
    "board with flash encryption fused too",
    "On every released firmware tree today this includes recovering the "
    "device's private key, on every tier",
    "The hold helps only with a setup onto a network that host is not on. The "
    "owner's browser makes that move without it",
    "Once A65 is settled the lane would move the WAP with no browser, but only "
    "while that host leaves it alone and the new network stays up",
    # The seizure prose that replaced #1843's fifth-review bans.
    "different adversary, and today they can recover the key from any Canary, "
    "a board with flash encryption fused included",
    "know what the opt-in tiers buy today before you burn anything: nothing "
    "yet. The provisioning kit's Phase 2 (`provision_canary.sh --phase 2`) "
    "burns a flash-encryption key, `SECURE_BOOT_EN` and the JTAG fuses",
    "Read that design note before you burn a fuse — that decision cannot be "
    "walked back, and today it does not buy key secrecy.",
    # The prose that replaced #1843's sweep's bans.
    "address the token, on the receipt or on the page load it unlocks, ends that\n"
    "address's own backoff as a right bearer would (sweep F341), never the shared\n"
    "overflow's (`auth_handed_token()` frees only the address's own slot). The\n"
    "dashboard",
    "that serves ends that address's own backoff, as a right bearer would, never\n"
    "the shared overflow's: an address held only through the overflow stays held\n"
    "(sweep F341). The key is the request's socket peer address, never a",
    "- **Secure tier: FAIL today, as the default.** The provisioning kit's\n"
    "  Phase 2 (`provision_canary.sh --phase 2`) burns a flash-encryption key,",
    "- Secure boot (would verify firmware integrity; prepared in the provisioning kit, and on no tier does it verify anything yet: Phase 2 sets `SECURE_BOOT_EN` with no key digest)",
    "| Firmware corrupt | Every tier's bootloader checks an app image's checksum and appended SHA-256 before it boots it",
    "| `platformio_secure.ini` | ✅ | the secure env: its sdkconfig stages Secure Boot v2 and flash encryption (commented out) and names NVS encryption, all inert under `framework =",
    "| Factory reset via BOOT button | ✅ (short/long press) | ✅ (10 s hold: Wi-Fi, broker, name; identity and token kept) | ✅ (10 s: credentials + settings, identity kept; 30 s: wipe,",
    # #1843's sweep, third pass (its review round 2):
    'mitigations, but **neither works on any Canary today**: the provisioning',
    'lockdown as an explicit opt-in. The design calls for that opt-in to enforce a key backup',
    'so after it flash encryption is off and Secure Boot is enabled with nothing to verify against',
    "so every release image relies on the OTA path's SHA-256 and Ed25519 release-signature check",
    'an install or a full erase in either flasher, each of which rewrites the settings region, is what removes them',
    '(an install or a full erase in either flasher, whose factory image starts NVS over, is its way to a new identity)',
    'so an install in either flasher, with or without the full erase (its factory image starts NVS over), is its way',
    'a seizer of a board with flash encryption on who reads its flash after the hold has the mesh secret',
    'That keeps the secret off every board without flash encryption',
    'the FE gate keeps it off boards without flash encryption',
    'a board without flash encryption keeps the ring in RAM only',
]
# The replacements of the RETIRED_IN sentences, which share most of their
# words with what they replaced: (file, the replacement as it stands there).
# Each must not trip the gate, and each must stand in its file, so every file
# RETIRED_IN names carries at least one: a row pointed at the wrong file, or a
# replacement rewritten away, fails here instead of passing by scanning text
# that never held the retired sentence.
MUST_PASS_IN = [
    ('firmware/FEATURES.md',
     '| `provision_canary.sh` | ⚠️ | the workflow, with `--dry-run`; its Phase 2 turns on neither Secure Boot nor flash encryption'),
    ('docs/security/THREAT_MODEL.md',
     '- **Default tier: FAIL.** Secure boot and flash encryption are staged for an\n'
     '  opt-in secure-provisioning build that turns on neither today'),
    ('docs/security/THREAT_MODEL.md',
     '**FAIL on both tiers today, documented:** the limitation is disclosed\n'
     '(SECURITY_MODEL.md), not mitigated. Future mitigations: NVS encryption (the'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     'With the busy word never spoken (`open_client()` reporting no save it saw), each of the three fails on its first check, with no client opened'),
    ('firmware/LESSONS_LEARNED.md',
     'with the busy word never spoken each of the three fails on its first check (the loop task marks the request served from a row a save was still writing'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "and the session reaches it: a scan window opens from the boot auto-scan or from `POST /api/wifi/qr-scan`, which takes the wizard's one-shot pair token (`pair_token_valid()`) and not the cookie — but the session buys that token"),
    ('CHANGELOG.md',
     'behind the pair token, which the session can mint (`GET /api/pair/token` takes the cookie), and a code in front of the camera, which it cannot'),
    ('spec/beacon_channel_v0.md',
     "A holder of one member's Ed25519 private key — readable from flash on every tier today: it is plaintext in NVS, which flash encryption does not cover"),
    ('docs/design/magic_pairing.md',
     "but that move keeps the host's network saved; the hold, or Forget Wi-Fi, followed by that setup drops it, and reopens that door"),
    ('docs/security/SECURITY_MODEL.md',
     "but that move keeps the host's network saved; the hold, or Forget Wi-Fi, followed by that setup drops it, and reopens that door"),
    ('CHANGELOG.md',
     'Once that is settled the lane holds only while that host leaves it alone and the new network stays up, and taking it away needs no radio'),
    ('docs/audit/esp32s3_firmware_full_audit_2026-07.md',
     'Once that is settled the lane holds only while that host leaves it alone and the new network stays up, and taking it away needs no radio'),
    ('docs/security/SECURITY-AUDIT.md',
     'Once that is settled the lane holds only while that host leaves it alone and the new network stays up, and taking it away needs no radio'),
    ('firmware/LESSONS_LEARNED.md',
     'under the lockout), while that host leaves it alone and the new network stays up (the next four bullets).'),
    ('docs/design/hardware_root_of_trust.md',
     'Flash encryption with NVS encryption (Tier 3) would defend key-at-rest and casual readout, not decapping/fault-injection by a well-funded lab.'),
    ('docs/design/hardware_root_of_trust.md',
     'Dev mode is the sweet spot: with NVS encryption beside it, it would protect WiFi/keys at rest from a thief while the device stays field-recoverable.'),
    ('docs/design/vault_operator_ux_v1_1.md',
     "Keeps the entire existing signing scheme; once NVS encryption is in the build, the key can't be dumped off a locked device"),
    ('docs/V1_BENCH_TEST_RUNBOOK.md',
     'the firmware writes the `opera_secret` to NVS only with FE on (audit O2). FE does not make it confidential: NVS is plaintext on a fused board too'),
    ('docs/design/magic_pairing.md',
     'An owner who finds the unit on the wrong network still holds it, but on a WAP a reset does not undo a stranger who took the door.'),
    ('docs/design/magic_pairing.md',
     'a unit joined to the wrong network is one its owner still holds. On a WAP a factory reset is not the whole fix'),
    ('docs/FAQ.md',
     'which clears every saved network and opens the door again for half an hour from that moment, to the first phone or computer nearby that connects'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  //    and the address they serve has its own slot cleared (sweep F341,\n'
     '  //    after step 4), never the shared overflow: an address held only\n'
     '  //    through the overflow, which a host striking from nine or more\n'
     '  //    addresses keeps running, stays held after it is served.'),
    ('CHANGELOG.md',
     '  section above: a tap clears an address only when it is served to that\n'
     "  address's own ask, and the flagship's tap still goes to the first asker,\n"
     '  F354; under that host the hold is no way out unless the unit is then set\n'
     '  up on a network that host is not on, the iPhone app does not reach a\n'
     "  WAP's Bluetooth lane today, ledger A65, and that section gives both\n"
     "  trees' ways out with their caveats.) **Host-tested**"),
    ('firmware/canary/CONSOLIDATION.md',
     '| 1 | API authentication (bearer token + HKDF derivation + exponential backoff + constant-time compare) | ✅ (Phase 2.5: SPA injects bearer; all read + mutating endpoints gated;'),
    ('firmware/canary/lib/securacv_network/src/securacv_network.cpp',
     "// token ends its address's own backoff (auth_handed_token, sweep F341;\n"
     "// never the shared overflow's), so a browser that mistyped the token is not\n"
     '// held on its first API call.'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '  it stops, or hold BOOT 10 s and set the unit up on a network that host\n'
     "  is not on. (Corrected by #1843's later reviews, in the amendments below:\n"
     "  the iPhone app never reaches a WAP's lane, A65, so the sentence the docs\n"
     "  use now names the owner's browser on `POST /api/wifi/connect` as the\n"
     "  WAP's way out, with its caveats, and the iPhone's lane as no way out\n"
     "  until A65 is settled.) `test_auth_backoff.cpp`'s overflow row starts from the empty"),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "  *Amended once more 2026-10-21 (#1843's first review):* the bonded lane\n"
     '  gets the owner out only while that host holds no bond of its own.\n'
     '  (Corrected by the fourth and fifth amendments below: radio range is no\n'
     '  bound, since any landing session can take the lane away, and the lane is\n'
     "  no way out today, since the iPhone app never reaches a WAP's CREDS, A65.)\n"
     '  Any landing session can pair a Bluetooth peer: it can turn pairing on'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "  which F343's posture then named as the way out from under a host striking\n"
     "  from nine or more addresses (corrected by the third review's note below:\n"
     "  the way out is the owner's browser, and the iPhone's lane is no way out\n"
     "  until A65 is settled), and spend that lane's ten writes an hour, so the decision"),
    ('docs/getting_started_canary.md',
     "You can't brick the device through the dashboard. You *can* lose your witness chain by erasing the board: installing again in a flasher, with or without its full erase"),
    ('docs/getting_started_canary.md',
     'on the all-rounder Canary, holding BOOT to 30 seconds removes the Ed25519 device key'),
    ('docs/getting_started_canary.md',
     '10-second BOOT reset and an over-the-air update will keep the certificate, so that fingerprint will stay right; holding BOOT to 30 seconds, or installing again in a flasher'),
    ('firmware/canary/include/canary_config.h',
     '// generated once on the first TLS-capable boot. The 10 s BOOT reset and an\n'
     '// OTA update keep both (reset_scope_keys.h kIdentityKept); the 30 s wipe\n'
     '// (ResetScope::Everything), or an install or a full erase in either flasher\n'
     '// (whose factory image starts NVS over), removes them, after which the'),
    ('firmware/canary/scripts/ota_deploy.py',
     'log_error("erase in a flasher (its factory image starts NVS over), makes a new")'),
    ('firmware/canary/scripts/ota_deploy.sh',
     'log_error "erase in a flasher (its factory image starts NVS over), makes a new"'),
    ('firmware/canary/src/main.cpp',
     '  Serial.println("║  BOOT: tap=gate, 2s=info, 10s=reset (key kept), 30s=wipe     ║");'),
    ('ios/Sources/SecuraCV/Transport/DeviceAPI.swift',
     '+ "place — check the device itself. If you installed it again or fully erased "'),
    ('ios/Sources/SecuraCV/Alerts/HeadWatch.swift',
     "// means the chain restarted (an install or a full erase in a // flasher, an all-rounder Canary's 30 s BOOT wipe, a replaced board;"),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     '            resolveProvision(.failed("This Canary\'s firmware doesn\'t offer the Bluetooth rescue — use its own setup network (SecuraCV-wap-XXXX, which it raises while it is off'),
    ('custom_components/securacv/__init__.py',
     '# identity for this device_id (an install or a full erase in either # flasher, whose factory image starts NVS over;'),
    ('custom_components/securacv/__init__.py',
     "# A newly pinned device starts its counters over (an install or a full # erase in either flasher, an all-rounder Canary's 30 s BOOT wipe);"),
    ('custom_components/securacv/sensor.py',
     '    device is (re)pinned via TOFU (async_record_verify), which is what an\n'
     '    install or a full erase in either flasher (whose factory image starts\n'
     "    NVS over), or an all-rounder Canary's 30 s BOOT wipe, looks like from"),
    ('custom_components/securacv/tests/test_replay_and_device_id.py',
     '    # An install or a full erase in a flasher (a new key): the device pins\n'
     '    # fresh and its chain starts over.'),
    ('docs/design/magic_pairing.md',
     'A phone that still holds a pairing for a unit that was installed again or fully erased in a flasher (a new key, so a new token and device_id;'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '  untouched) and `docs/csi_developer_api.md` say so now; the canary-wap half\n'
     '  is unchanged. No behavior changed; not run on a device.\n'
     '  *Amended 2026-10-21 (#1843):* the canary-wap half said it has no reset\n'
     '  path; it has the 10 s hold (`factory_reset_by_hold()`, #1816), which keeps\n'
     '  `csi`/`ev.next` and `csi.evsent`, so the full erase is still its\n'
     '  recovery. The note at `space_low()` (both trees) and\n'
     '  `docs/csi_developer_api.md` say so now.\n'
     '- [x] **F83 [code] The canary-wap can commit an event before its id floor'),
    ('firmware/common/csi/src/csi_event_id_floor.h',
     "// stranger. A canary-wap's 10 s BOOT hold (factory_reset_by_hold()) clears\n"
     '// neither (it removes the saved Wi-Fi, the broker row and the name, and\n'
     '// keeps the identity), so its owner installs it again in a flasher (with\n'
     '// or without the full erase: the factory image starts NVS over either way),\n'
     '// which clears all of NVS, the identity key, the floor `ev.next` and\n'
     '// `csi.evsent` included. Either way the allocator starts again at kIdSpaceBase,'),
    ('firmware/projects/canary-wap/arduino/canary_wap/csi_event_id_floor.h',
     "// stranger. A canary-wap's 10 s BOOT hold (factory_reset_by_hold()) clears\n"
     '// neither (it removes the saved Wi-Fi, the broker row and the name, and\n'
     '// keeps the identity), so its owner installs it again in a flasher (with\n'
     '// or without the full erase: the factory image starts NVS over either way),\n'
     '// which clears all of NVS, the identity key, the floor `ev.next` and\n'
     '// `csi.evsent` included. Either way the allocator starts again at kIdSpaceBase,'),
    ('docs/csi_developer_api.md',
     "  sees a stranger; a canary-wap's 10 s BOOT hold (`factory_reset_by_hold()`)\n"
     '  keeps both, so its owner installs it again in a flasher (with or without'),
    ('CHANGELOG.md',
     '  `space_low()` on both trees: the 30 s BOOT wipe on a canary (its 10 s reset\n'
     '  keeps the floor), a full flash erase on a canary-wap (whose 10 s hold keeps\n'
     "  it too) without restoring the flasher's pre-erase backup, then a"),
    ('canary-local/tools/gen_flash.py',
     '"reset: the 10-second BOOT reset keeps the key.",'),
    ('canary-local/devices/flash.json',
     'so the board comes up with a new identity either way. More than a factory reset: the 10-second BOOT reset keeps the key.'),
    ('firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md',
     '        WAP has no factory-reset route at all (BOOT held 10 s,\n'
     '        `factory_reset_by_hold()`, is the only path; it clears the saved\n'
     '        Wi-Fi, the broker row and the name, keeps the identity, the chain and\n'
     '        the token, and reopens the first-come Bluetooth setup door). Whether\n'
     '        `POST /api/factory-reset` may exist is the decision;'),
    ('docs/design/onboarding_shared_module.md',
     "  before 700 ms — the WAP's 2 s hold and the 10 s factory-reset hold are"),
    ('firmware/canary/src/csi_event_log.h',
     ' * the 30 s BOOT wipe or an install or a full erase in either flasher, each\n'
     ' * of which gives a new key; the 10 s reset and an OTA update keep it) is left\n'
     ' * untouched and unused. The backfill signs what it'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     '/* Erase the persisted trusted-peer list. Used by POST /api/mesh/leave\n'
     " * (securacv_network.cpp's handle_mesh_leave). No factory reset calls it: the\n"
     ' * 10 s BOOT reset keeps the list, and the 30 s wipe drops it with the whole\n'
     ' * namespace (nvs.clear()). Idempotent — succeeds if the list was empty. Also'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_improv.h',
     ' * for a WAP nobody owns yet (or one whose owner held BOOT 10 s or forgot its\n'
     ' * Wi-Fi: both keep the identity and the token, and the door goes to\n'
     ' * whichever client connects first, docs/design/magic_pairing.md §2.2):\n'
     ' * while no credentials are stored, for the'),
    ('firmware/canary/lib/securacv_webui/src/securacv_webui.cpp',
     "          PIN confirmation to prevent unauthorized access. Paired devices are kept in the device's\n"
     '          settings storage (NVS), which is plaintext on the flash chip on every build today, so\n'
     '          someone holding the board can read them; manage them from this panel.'),
    ('canary-local/assets/intake.js',
     '    meaning: "Flash encryption is on, with a key burned into this chip: the firmware image (and any partition flagged encrypted) reads back as ciphertext, and a plaintext image'),
    ('desktop/src-tauri/src/efuse.rs',
     '    meaning: "Flash encryption is on, with a key burned into this chip: the firmware image (and any partition flagged encrypted) reads back as ciphertext, and a plaintext image'),
    ('canary-local/assets/intake.js',
     '            meaning: "Flash encryption is on, with a key burned into this chip: the firmware image (and any partition flagged encrypted) reads back as ciphertext, and a plaintext'),
    ('desktop/src-tauri/src/efuse.rs',
     '            meaning: "Flash encryption is on, with a key burned into this chip: the firmware image (and any partition flagged encrypted) reads back as ciphertext, and a plaintext'),
    ('firmware/projects/canary-wap/arduino/canary_wap/household.h',
     ' *      store IRKs only in NVS (plaintext on the flash chip on every build\n'
     ' *      today: flash encryption does not cover NVS and NVS encryption is in\n'
     ' *      no released build; common/identity/key_at_rest.h) and never export\n'
     ' *      them over any wire, MQTT topic, or web UI.'),
    ('docs/design/household_presence_and_posture.md',
     '  `hh_count`. Its comment said the IRKs are "flash-encrypted at rest on ESP32\n'
     '  with secure boot". That was false, and the comment now says so (2026-10-21):\n'
     '  flash encryption does not cover NVS, NVS encryption cannot be built under\n'
     "  `framework = arduino` (neither pinned core's precompiled `sdkconfig` has\n"
     '  it), and secure boot works on no tier: the provisioning kit prepares it,\n'
     '  and its Phase 2 sets `SECURE_BOOT_EN` with no key digest, so nothing is\n'
     '  verified ([`key_at_rest.h`](../../firmware/common/identity/key_at_rest.h)).'),
    ('docs/research/face_id_without_faces.md',
     "Until then, the IRKs sit in plaintext: `household.h`'s old claim that they\n"
     'were "flash-encrypted at rest on ESP32 with secure boot" was wrong, and its\n'
     'comment now says so (2026-10-21), since flash encryption does not cover NVS,\n'
     'NVS encryption cannot be built under `framework = arduino` (neither pinned\n'
     "core's precompiled `sdkconfig` has it), and secure boot works on no tier: the\n"
     'provisioning kit prepares it, and its Phase 2 sets `SECURE_BOOT_EN` with no\n'
     'key digest, so nothing is verified ([`key_at_rest.h`](../../firmware/common/identity/key_at_rest.h)).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/mesh_network.cpp',
     '// extract it and become a permanent opera member from outside the household, // on a board with flash encryption on too: flash encryption does not cover'),
    ('firmware/canary/lib/securacv_ble_scan/src/ble_scan.h',
     ' * (with no flash-encryption gate: Tier 0 like the identity key, plaintext\n'
     ' * in NVS on every tier today; mesh_state.h, common/identity/key_at_rest.h).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_scan.h',
     ' * (with no flash-encryption gate: Tier 0 like the identity key, plaintext\n'
     ' * in NVS on every tier today; mesh_state.h, common/identity/key_at_rest.h).'),
    ('firmware/canary/lib/securacv_ble_scan/src/ble_scout_key.h',
     ' * (not gated on flash encryption, unlike the opera_secret, and plaintext\n'
     ' * in NVS on every tier today: flash encryption does not cover NVS;\n'
     ' * common/identity/key_at_rest.h).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_scout_key.h',
     ' * (not gated on flash encryption, unlike the opera_secret, and plaintext\n'
     ' * in NVS on every tier today: flash encryption does not cover NVS;\n'
     ' * common/identity/key_at_rest.h).'),
    ('firmware/scripts/regression_check.sh',
     '      check_fail "Mesh impl \'$rel\' has no esp_flash_encryption_enabled() gate — the opera_secret must never be persisted on a board without flash encryption (#610; the gate keeps'),
    ('firmware/common/network/wifi_store.h',
     ' * and the path up is NVS encryption, which needs the arduino-as-IDF-component\n'
     ' * migration (firmware/ESP32S3_OPTIMIZATION_ROADMAP.md item 9); the secure kit\n'
     ' * in firmware/provisioning/ is not it, since flash encryption does not cover\n'
     ' * NVS — docs/PARITY.md says so per target rather than pretending.'),
    ('firmware/projects/canary-display/arduino/canary_display/wifi_store.h',
     ' * and the path up is NVS encryption, which needs the arduino-as-IDF-component\n'
     ' * migration (firmware/ESP32S3_OPTIMIZATION_ROADMAP.md item 9); the secure kit\n'
     ' * in firmware/provisioning/ is not it, since flash encryption does not cover\n'
     ' * NVS — docs/PARITY.md says so per target rather than pretending.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wifi_store.h',
     ' * and the path up is NVS encryption, which needs the arduino-as-IDF-component\n'
     ' * migration (firmware/ESP32S3_OPTIMIZATION_ROADMAP.md item 9); the secure kit\n'
     ' * in firmware/provisioning/ is not it, since flash encryption does not cover\n'
     ' * NVS — docs/PARITY.md says so per target rather than pretending.'),
    ('firmware/projects/canary-sense/src/main.cpp',
     '  // (common/identity/key_at_rest.h Tier 0); the path up is NVS encryption\n'
     '  // (the arduino-as-IDF-component migration), not the secure kit, whose flash\n'
     '  // encryption does not cover NVS.'),
    ('firmware/projects/canary-sentinel/src/main.cpp',
     '  // (common/identity/key_at_rest.h Tier 0); the path up is NVS encryption\n'
     '  // (the arduino-as-IDF-component migration), not the secure kit, whose flash\n'
     '  // encryption does not cover NVS.'),
    ('firmware/projects/canary-vision/src/main.cpp',
     '  // (common/identity/key_at_rest.h Tier 0); the path up is NVS encryption\n'
     '  // (the arduino-as-IDF-component migration), not the secure kit, whose flash\n'
     '  // encryption does not cover NVS.'),
    ('docs/PARITY.md',
     '| C9 | Credentials encrypted at rest | NVS encryption on the NVS that holds the credential blobs (flash encryption alone does not cover NVS) |'),
    ('docs/PARITY.md',
     '   and the path is NVS encryption, which needs the arduino-as-IDF-component\n'
     '   migration (`firmware/ESP32S3_OPTIMIZATION_ROADMAP.md` item 9): the opt-in\n'
     '   secure kit (`firmware/provisioning/`) does not encrypt NVS, since flash\n'
     "   encryption does not cover it and the kit's NVS-encryption setting is\n"
     '   inert under `framework = arduino`. This is the one brief item the'),
    ('docs/FIRMWARE_MAP.md',
     'opt-in secure kit (flash encryption + Secure Boot v2) that no CI leg builds.\n'
     "*(Amended 2026-10-21: `firmware.yml` compiles the kit's two envs,\n"
     'compile-only, since #1744; the kit stages flash encryption and Secure Boot,\n'
     'commented out, rather than enabling them, and neither would encrypt NVS.)*\n'
     'NVS encryption is not configurable on either pinned Arduino core as built'),
    ('docs/FIRMWARE_MAP.md',
     '| Encrypted credentials at rest | plaintext NVS everywhere; NVS encryption not configurable on the pinned cores | honest `⚠️`: posture reported on every node, the secure kit'),
    ('docs/FIRMWARE_MAP.md',
     '  posture is reported as `creds_at_rest` on every node, and NVS encryption,\n'
     '  after the arduino-as-IDF-component migration, is the documented path; the\n'
     '  secure kit is not, since flash encryption does not cover NVS (PARITY\n'
     '  footnote 1).'),
    ('docs/design/hardware_root_of_trust.md',
     '3. **Key extraction.** The identity key is plaintext-at-rest. Without NVS\n'
     '   encryption (which needs flash encryption beside it and is in no released\n'
     '   build) or a hardware-bound key, physical possession = key compromise, on a\n'
     '   board with flash encryption fused too, today, which undermines both\n'
     '   attestation and the pinned-identity model.'),
    ('docs/design/hardware_root_of_trust.md',
     'still exists but the ROM **refuses to boot an unsigned image**, and reads of the\n'
     'app image come back ciphertext (NVS does not: without NVS encryption, the Tier 3\n'
     'row, the key and saved passwords still read back in the clear) — so the browser\n'
     'flasher\'s "write any factory image and boot'),
    ('docs/design/hardware_root_of_trust.md',
     'firmware"* actually hold. Tier 3, once NVS encryption is buildable, would\n'
     'additionally stop the identity key being lifted out of a stolen board, closing\n'
     'the clone-with-stolen-key gap (an optional DS-bound attestation key, §5.4, is\n'
     'non-extractable but a separate key); today no tier does.'),
    ('docs/design/hardware_root_of_trust.md',
     '  disables plaintext serial programming. Protects the app image and the\n'
     '  partitions flagged `encrypted` at rest, **not** NVS (that takes NVS\n'
     '  encryption) and **not** a live chip.'),
    ('docs/design/vault_operator_ux_v1_1.md',
     "  the **ESP32-S3's own eFuse / NVS encryption / DS peripheral** (device) holds\n"
     '  (no released build uses any of them yet; flash encryption alone does not\n'
     '  cover NVS) —'),
    ('docs/design/vault_operator_ux_v1_1.md',
     '  path **(a)** would harden the ESP32-S3 Ed25519 identity *at rest* with'),
    ('docs/design/apple_home_integration.md',
     '| Bridge restarts / Canary reboots | HAP state is **re-derived, never restored**: re-advertise mDNS, republish current signals. Pairings persist (NVS on a Canary — plaintext on'),
    ('docs/design/apple_home_integration.md',
     'NVS; they get the `opera_secret` treatment — refuse to persist on flash-encryption-off devices, which keeps them off every board without flash encryption but does not make them confidential'),
    ('docs/hardware/tv_display_design.md',
     '  the NVS-stored broker credentials are plaintext at rest on every build: no\n'
     '  display env uses the secure sdkconfig\n'
     '  (`provisioning/sdkconfig.defaults.secure`), which is inert under\n'
     '  `framework = arduino` anyway, and flash encryption does not cover NVS (NVS\n'
     '  encryption needs the arduino-as-IDF-component migration); and (2) an opt-in `WiFiClientSecure` path with a'),
    ('docs/research/bitchat_protocol_review.md',
     '| Triple-tap panic wipe | Right instinct, wrong layer for us. Chirp already retains nothing (ephemeral keys, no history). For Beacon, wiping the beacon set would silently destroy'),
    ('docs/strategy/12-engineering-foundations-flight-rules.md',
     'deferral, but it ships today: either enable flash encryption + NVS encryption\n'
     '(which needs the arduino-as-IDF-component migration; flash encryption alone\n'
     'does not cover NVS) + secure boot for production builds, or state the exposure\n'
     'explicitly in the threat model and on'),
    ('docs/strategy/13-black-box-evidence-model.md',
     '| P10 | **A known instrument** — you can know what code witnessed | signed firmware/OTA, versioned dictionary hash in checkpoints, (doc 12) release provenance | verify firmware'),
    ('spec/beacon_channel_v0.md',
     '- Reuses the existing key-management infrastructure (the key in NVS, plaintext at rest on every tier today since flash encryption does not cover NVS, see the threat table below;'),
    ('docs/secure_provisioning.md',
     '- JTAG permanently disabled via eFuse\n'
     '- NVS encryption — named in `sdkconfig.defaults.secure` but inert under\n'
     '  `framework = arduino` (`platformio_secure.ini`, "KNOWN INERT"), so a Phase 2\n'
     "  board's NVS (the identity key, the saved Wi-Fi and broker passwords, the\n"
     '  bonds) stays plaintext until the arduino-as-IDF-component migration'),
    ('docs/audit/mesh_and_chirp_audit_v1.md',
     '| NVS confidentiality | M | No FE gate (O2). *(Amended 2026-10-21: the O2 gate keeps the secret off boards without flash encryption; it does not give NVS confidentiality, since flash encryption'),
    ('docs/audit/mesh_and_chirp_audit_v1.md',
     '  SD — pure append, never truncated — with the 64-entry flash-encrypted NVS\n'
     '  ring *(amended 2026-10-21: an NVS ring written only on a board with flash\n'
     '  encryption on, and plaintext NVS there too)* demoted to a recent-view cache (`beacon_channel.cpp::sd_append_audit_entry`).'),
    ('docs/audit/esp32s3_firmware_full_audit_2026-07.md',
     "but aren't wired into the FULL build — identity keys and the chain sit in plaintext flash today\n"
     '*(amended 2026-10-21: and would on a FULL build with Secure Boot and flash encryption too, since\n'
     'flash encryption does not cover NVS and NVS encryption is not available under\n'
     '`framework = arduino`; `key_at_rest.h`)*; a\n'
     'PSRAM-backed deferred-write ring would stop record loss while the SD card is absent (the mechanism'),
    ('docs/V1_BENCH_TEST_RUNBOOK.md',
     'proves on hardware that **no mesh secret lands in NVS on a board without flash encryption** (on an\n'
     'FE board it does, in NVS that is plaintext too: flash encryption does not cover NVS).'),
    ('docs/V1_BENCH_TEST_RUNBOOK.md',
     '| C2 | **FE persistence gate** (keeps the secret off non-FE boards; not at-rest secrecy) | pair on a **non-FE** board, then dump NVS | serial shows `active for this boot but NVS'),
    ('docs/V1_LAUNCH_REVIEW.md',
     '   `opera_secret` is never persisted on a board without flash encryption (on an FE board it is,\n'
     '   in NVS that flash encryption leaves plaintext) — the static half of issue'),
    ('docs/BLE_MESH_OPERA_TANDEM.md',
     '- The household key never leaves the device over any wire; it would live in NVS, which is plaintext on the flash chip on every tier today (no Canary has a secure element and NVS'),
    ('docs/security/PENTEST_SCOPE.md',
     '  We know a physical attacker can dump flash and recover the identity key (and\n'
     '  the saved Wi-Fi and broker passwords and the Bluetooth bonds), from a board\n'
     '  with flash encryption fused too: NVS is plaintext on every tier, since NVS\n'
     '  encryption is in no released build. What'),
    ('firmware/README.md',
     "| `secure` / `secure_ha` | the provisioning kit's Tier 3/4 image: it refuses to store or load the identity key unless NVS is encrypted, which no board is under `framework ="),
    ('firmware/common/network/provisioning_gate.h',
     '// BOOT tap — which the page load then SPENDS (page_token_decide). On the\n'
     '// flagship, the one tree that serves a page by the tap, the tap is one\n'
     '// consumer across both paths: one page load or one receipt fetch, whichever\n'
     "// asks first (the ledger's F354). The WAP serves no page by the tap, and its\n"
     "// receipt's tap goes to one asker (tap_arbitrate() below). A page that got\n"
     '// the token already holds the bearer, so its "Save recovery kit" fetch\n'
     '// needs no second tap.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/provisioning_gate.h',
     '// BOOT tap — which the page load then SPENDS (page_token_decide). On the\n'
     '// flagship, the one tree that serves a page by the tap, the tap is one\n'
     '// consumer across both paths: one page load or one receipt fetch, whichever\n'
     "// asks first (the ledger's F354). The WAP serves no page by the tap, and its\n"
     "// receipt's tap goes to one asker (tap_arbitrate() below). A page that got\n"
     '// the token already holds the bearer, so its "Save recovery kit" fetch\n'
     '// needs no second tap.'),
    ('canary-vision/spa/app.js',
     "// body's gate_ttl_seconds), so a press anywhere in our 60 s window lands\n"
     '// while this page is the only one asking (a WAP answers 409 tap_contended\n'
     '// when another address asked during the tap, sweep F342). The poll starts\n'
     "// before the press, and handlers.onTick's third argument says whether the\n"
     '// device has answered an ask yet (its first 403): a WAP hands a tap only to\n'
     '// the one address asking from 10 s before it onward, so until that answer\n'
     '// the step says not to press — a press the first ask has not reached yet is\n'
     "// one no page of the owner's is asking for, and goes to the first asker."),
    ('canary-vision/spa/app.js',
     '      stepTitle,\n'
     '      stepSubtitle,'),
    ('canary-vision/device-api/routes/provision.js',
     "// Models the WAP's BOOT-button provisioning gate (canary_wap.ino's\n"
     '// handle_provisioning_receipt) only in part: GET /api/provisioning-receipt\n'
     '// is unauthenticated — it is how a client obtains the API token in the\n'
     '// first place — returns 403 until the physical BOOT button is short-tapped,\n'
     '// and spends one tap on one receipt. Unlike the WAP since sweep F342, it\n'
     '// hands the tap to the first client that asks: the WAP serves it only to\n'
     '// the sole address that asked from 10 s before the tap onward, and a second\n'
     "// address closes the tap with 409 tap_contended (provisioning_gate.h's"),
    ('canary-vision/device-api/lib/device-state.js',
     '  // BOOT-button provisioning gate (a short tap opens a ~30 s window during\n'
     '  // which /api/provisioning-receipt returns the token to the first request;\n'
     '  // the WAP firmware hands the tap only to the sole address asking from 10 s\n'
     '  // before it and answers 409 tap_contended to a second, which this\n'
     '  // reference server does not model)'),
    ('docs/security/THREAT_MODEL.md',
     "Secure Boot and flash encryption are in no tier's boundary today: the"),
    ('docs/security/THREAT_MODEL.md',
     "- **Secure tier: FAIL today, as the default.** The provisioning kit's"),
    ('docs/security/THREAT_MODEL.md',
     '  Secure Boot would refuse unsigned firmware and flash encryption would keep\n'
     '  the app image from being read off the chip, but flash encryption does'),
    ('docs/security/THREAT_MODEL.md',
     "| Firmware corrupt | Every tier's bootloader checks an app image's checksum and appended SHA-256 before it boots it (neither pinned core's precompiled bootloader sets `CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON`; it does skip the check on a wake from deep sleep), and on a failed check it tries the other app slot instead (ESP-IDF's `bootloader_utility_load_boot_image`; read from the cores' `sdkconfig` and IDF source, not run on a device). What no tier refuses today is a well-formed image that is unsigned or forged: that is Secure Boot's job, which works on no Canary today: the provisioning kit's Phase 2 sets `SECURE_BOOT_EN` with no Secure Boot key digest, so Secure Boot is enabled with nothing to verify against, and it is NOT on default builds (see [Scope of this principle](#scope-of-this-principle)), so every release image relies on the OTA path's SHA-256 and Ed25519 release-signature check"),
    ('docs/security/THREAT_MODEL.md',
     '  keys and the saved passwords stay plaintext on every tier today; prepared\n'
     '  in the provisioning kit, not yet enabled on any tier, Scenario 4)'),
    ('docs/security/THREAT_MODEL.md',
     '**LIMITATION:** Once Secure Boot works (on no tier today: Phase 2 sets\n'
     '`SECURE_BOOT_EN` with no key digest, so nothing is verified), a'),
    ('docs/security/THREAT_MODEL.md',
     '- No undisclosed outbound connections (a compromised chip would have to originate traffic of its own on a network whose expected traffic is short and known; SECURITY_MODEL.md, Hardware Trust Boundary)'),
    ('docs/security/SECURITY_MODEL.md',
     'mitigations, but **neither works on any Canary today**: the provisioning kit prepares both and turns on neither'),
    ('docs/security/SECURITY_MODEL.md',
     'tiers buy today before you burn anything: nothing yet. The provisioning'),
    ('canary-local/assets/flash.js',
     '    "leaves flash encryption off and enables secure boot with nothing to " +\n'
     '    "verify against (a board may not boot afterward), so today it protects " +'),
    ('canary-local/assets/flash.js',
     '    "For a board deployed somewhere physically exposed, put it out of reach: Phase-2 provisioning does not yet protect its firmware, its key or its saved WiFi.",'),
    ('docs/V1_BENCH_TEST_RUNBOOK.md',
     "| C0 | Production-provision FE boards (**blocked today**) | `firmware/provisioning/` → `./generate_keys.sh` then `./provision_canary.sh --port … --phase 2 --dry-run` (uses `platformio_secure.ini` / `sdkconfig.defaults.secure` / `partitions_secure.csv`); do not run it without `--dry-run` on a C1 board yet | As tooled the kit makes no FE board: it writes a plaintext image and burns no `SPI_BOOT_CRYPT_CNT` (and `SECURE_BOOT_EN` with no key digest, untested on a bench), and its `verify_device.py --expect-locked` fails on the fuses it does not burn (`docs/secure_provisioning.md`, Step 3). C1's FE boards need flash encryption turned on some other way until the kit does it | `O2/` |"),
    ('docs/secure_provisioning.md',
     '# 4. Burn SECURE_BOOT_EN, SOFT_DIS_JTAG (7), DIS_USB_JTAG, DIS_PAD_JTAG'),
    ('docs/secure_provisioning.md',
     'run on a bench: treat the board as possibly unbootable). No step enables\n'
     'anti-rollback (`SECURE_VERSION`) or disables ROM download mode. The RSA-3072'),
    ('docs/secure_provisioning.md',
     '# 5. Run verify_device.py --expect-locked, which fails and stops the run'),
    ('docs/secure_provisioning.md',
     '| `FLASH_CRYPT_CNT` | 0 | odd (1,3,5,7) (not burned today) | Enables flash encryption |'),
    ('docs/secure_provisioning.md',
     "### Step 2: Build the Secure Env's Firmware"),
    ('docs/secure_provisioning.md',
     '| `provision_canary.sh` | Orchestration: virgin verify → flash firmware (plaintext, unsigned) → burn the flash-encryption key → burn security eFuses → post-verify → generate manifest. Has `--dry-run` mode. As tooled it turns on neither Secure Boot nor flash encryption (Step 3 below) |'),
    ('firmware/canary/include/secure_defaults.h',
     '// Secure Boot v2 and flash encryption are staged in the provisioning kit'),
    ('docs/strategy/16-kit-commerce-pricing-and-fulfillment.md',
     '- **R3 "Assembled Witness" and any future evidence-grade SKU** would run the Phase-2'),
    ('firmware/provisioning/provision_canary.sh',
     "    - Flash the secure env's app image, in plaintext and unsigned"),
    ('firmware/provisioning/provision_canary.sh',
     '    echo "  - Burn SECURE_BOOT_EN with no key digest and no signed bootloader"'),
    ('firmware/provisioning/provision_canary.sh',
     '    echo "No anti-rollback fuse is burned, and the lockdown check after the burn"'),
    ('firmware/provisioning/provision_canary.sh',
     '            echo "  Secure Boot nor flash encryption is on (docs/secure_provisioning.md)."'),
    ('firmware/provisioning/provision_canary.sh',
     '#   1. Virgin verify → 2. Flash firmware (plaintext, unsigned) → 3. Burn keys →'),
    ('docs/design/hardware_root_of_trust.md',
     'documents the eFuse target state. As tooled, a Phase 2 run turns neither\n'
     'protection on: it writes a plaintext, unsigned image, burns the'),
    ('docs/design/vault_operator_ux_v1_1.md',
     '- **Secure it (protect the key at rest) — the fuses are partly tooled, the\n'
     "  key's protection is not yet buildable.** `docs/secure_provisioning.md` Phase 2 wraps"),
    ('docs/design/vault_operator_ux_v1_1.md',
     '  or a Secure Boot key digest), and the signing step is not tooled at all. This is opt-in *per device* — burning eFuses'),
    ('docs/design/vault_operator_ux_v1_1.md',
     '   signing scheme; the fuse steps are partly tooled (not the crypt counter or a\n'
     '   Secure Boot key digest, so the kit turns neither on yet), and NVS encryption, without'),
    ('docs/design/magic_pairing.md',
     "| No credentials stored (first boot, or after a factory reset or Forget Wi-Fi), within **30 minutes** of power-on (after Forget Wi-Fi, of the credentials vanishing; after a WAP's hold, of the restart on release) | **Open** (`Door::NoCredentials`) | A device nobody owns yet — or, after a reset or a Forget Wi-Fi, a cleared unit that still has an owner (on a WAP any landing session can send Forget Wi-Fi, ledger F345, so the owner may not have chosen it): the hold keeps the witness identity on a Sense, a Vision and a WAP, a WAP's hold and Forget Wi-Fi keep its token too, and the door goes to whichever client connects first (§2.2). The window is `IMPROV_FIRST_BOOT_WINDOW_MS` (`Timing::first_boot_window_ms`, half an hour by default); a power cycle re-arms it, and so does the credentials vanishing (Forget Wi-Fi re-arms it at that moment; a WAP's hold zeroes the carried record and restarts on release, so its fresh window runs from the restart); compile-time `0` makes the device tap-only. A **software restart does not** re-arm it: the spent time rides in RTC memory across `ESP.restart()` and watchdog resets and is cleared only by a power-on, a brownout, a power glitch or a reset a host asserted from the USB / JTAG port — the Flasher's, so a re-flashed unit never inherits the old image's spent window (the WAP's first-boot wizard restarts the unit every 15 idle minutes, which would otherwise have re-opened the door forever). |"),
    ('docs/design/onboarding_shared_module.md',
     "- **No credentials stored** (first boot, or a factory reset: the owner's\n"
     '  act, which keeps the witness identity, so the unit still has an owner):'),
    ('docs/GLOSSARY.md',
     'Sense or Vision (the WAP has no tap door yet). A cleared unit still has an\n'
     "owner (the hold keeps the witness identity, and a WAP's hold and Forget"),
    ('docs/FAQ.md',
     'power-on (unplug and replug to get another 30). That includes one you\n'
     'cleared yourself with the 10-second BOOT hold or, on a WAP, Forget Wi-Fi: it'),
    ('firmware/projects/canary-sense/README.md',
     '  is not claimable from the street. A unit reset with the hold still has an\n'
     '  owner (the hold keeps the witness identity and the chain), yet for that'),
    ('firmware/projects/canary-vision/README.md',
     '  hold re-arms it; 0 = tap-only). A unit reset with the hold still has an\n'
     '  owner (the hold keeps the witness identity and the chain), yet for that'),
    ('docs/FIRMWARE_MAP.md',
     '  2026-10-22: or an owned one its owner reset or made forget its Wi-Fi,'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     ' *              * no secret persisted (never paired, after POST'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     '/* Erase the persisted opera_secret. Called by POST /api/mesh/leave'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     ' * failure path. No factory reset calls it: the 10 s BOOT reset keeps the\n'
     ' * secret, and the 30 s wipe drops it with the whole namespace'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     ' * buffer too small. On "no peers persisted" (never paired, after POST'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     ' *   false — null pointer, FE disabled, nothing persisted (never elected,'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.cpp',
     '   * call idempotent (a leave on a board that never paired) WITHOUT swallowing'),
    ('firmware/canary/src/main.cpp',
     '     * false on a device that never paired, left its opera (POST'),
    ('firmware/canary/src/main.cpp',
     '     * paired, a leave, the 30 s wipe; the 10 s reset keeps the list)'),
    ('firmware/canary/lib/securacv_ble_scan/src/ble_scout_key.h',
     ' * offers yet: nothing calls this today, and no BOOT hold touches the key'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_scout_key.h',
     ' * offers yet: nothing calls this today, and no BOOT hold touches the key'),
    ('firmware/canary/lib/securacv_network/src/securacv_network.cpp',
     "  //      also ends the asking address's own backoff (auth_handed_token,"),
    ('firmware/projects/canary-wap/tests_host/test_receipt_grant.cpp',
     "  // and the tap ends that address's own backoff (sweep F341; never the"),
    ('docs/getting_started_canary.md',
     "network.** (This paragraph and its list are the all-rounder Canary's,"),
    ('docs/getting_started_canary.md',
     'recovery kit without a second tap: that is how you get the kit back later'),
    ('docs/onboarding_multiple_canaries.md',
     "2. The app asks the Canary first. **Don't press BOOT yet:** the step reads"),
    ('docs/onboarding_multiple_canaries.md',
     '   one tap in the app plus, once the app says to, one tap on the device.'),
    ('canary-vision/spa/app.js',
     '// let the app ask it first, short-tap its BOOT button once the step says'),
    ('ios/README.md',
     '    kit (the receipt its own pages save once a save started there is asking'),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     '        + "was wrong, this Canary\'s own setup network (SecuraCV-wap-XXXX, which "'),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     "// never reaches a WAP's today, ledger A65), never the fleet."),
    ('ios/Sources/SecuraCV/Views/FleetWiFiSheet.swift',
     '// never reaches a WAP today, ledger A65) — never the fleet. Progress is honest'),
    ('ios/README.md',
     '  that lane stays empty until ledger A65 is settled), plain http is offered only when nothing encrypted reaches the'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     '    /// stranded. It never fills for a WAP today: the rescue uses only a'),
    ('docs/legal-audit-2026-07.md',
     '  which never leaves the secure element." *(Amended 2026-10-22: do not use\n'
     '  this phrasing. No Canary has a secure element: the key is plaintext in NVS'),
    ('firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md',
     "        alone should not be able to reopen the setup door, is not today's\n"
     '        posture: `POST /api/wifi/forget` takes only `api_auth_check`, which'),
    ('firmware/FEATURES.md',
     '| API token NVS storage | ✅ | ✅ (`api_tkn`) | ✅ (`api_token`) | ✅ (same sketch as Arduino IDE via `src_dir`) |'),
    ('firmware/FEATURES.md',
     '| TLS cert NVS storage | ✅ | ✅ (`tls_cert` / `tls_key`) | ✅ (`tls_cert` / `tls_key`, in the HTTPS builds) | ✅ (same sketch as Arduino IDE via `src_dir`) |'),
    ('firmware/FEATURES.md',
     '| WiFi credentials NVS storage | ✅ | ✅ (`wifi_ssid` / `wifi_pass` and the `wifi_nets` ring) | ✅ (the same keys and ring) | ✅ (same sketch as Arduino IDE via `src_dir`) |'),
    ('firmware/FEATURES.md',
     '| Factory reset via BOOT button | ✅ (short/long press) | ✅ (10 s hold: Wi-Fi, broker, name; identity and token kept) | ✅ (10 s: credentials + settings, identity kept; 30 s: wipe, identity too) | ✅ (same sketch as Arduino IDE via `src_dir`) |'),
    ('canary-local/tools/hatchery/derive.mjs',
     '//   honestly a new bird, and says so. The 10 s factory reset keeps the key,'),
    ('ios/Sources/SecuraCV/Security/DeviceStore.swift',
     '// sight then (TOFU) — a device installed again or fully erased in a'),
    ('custom_components/securacv/tests/test_replay_reset_paths.py',
     'to the KEY: a Canary installed again or fully erased in a flasher (whose'),
    ('firmware/common/network/auth_backoff.h',
     '// polling a re-installed unit with its old token (an install or a full'),
    ('firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h',
     '// polling a re-installed unit with its old token (an install or a full'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '- [ ] **F352 [code] iOS: a pairing for a re-installed Canary keeps polling with its old token.**'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     'token. *(Amended 2026-10-21, #1843: a re-installed unit; the 10 s hold and an OTA update keep the token,'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "  (iOS, a reset unit's stale pairing; amended 2026-10-21, #1843: a fully\n"
     "  erased unit's, since the 10 s hold keeps the token)."),
    ('firmware/LESSONS_LEARNED.md',
     "and the likeliest such host was the owner's own phone, polling a re-installed unit (an install or a full erase in either flasher starts NVS over and gives it a new key and token; the 10 s hold and an OTA update keep the token) with its old token."),
    ('CHANGELOG.md',
     'still polling a re-installed unit (an install or a full erase in either flasher starts NVS over and gives it a new key and token; the 10 s hold and an OTA update keep the token) with its old token (filed as F352).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     "      if (!confirm('Remove every saved Wi-Fi network from this Canary? If its build has the Bluetooth setup door and Bluetooth is on, then for half an hour after, that door goes to the first phone or computer nearby that connects, so have yours ready.')) return;"),
    ('docs/unflashed_board_intake.md',
     'on the board, and it does not protect the key or the saved Wi-Fi in NVS. Its\n'
     'burn is still irreversible, and it sets `SECURE_BOOT_EN` with no Secure Boot\n'
     'key digest, which has not been run on a bench (a board may not boot after'),
    ('docs/audit/mesh_and_chirp_audit_v1.md',
     '*(Amended 2026-10-21: flash encryption does not stop it either. It does not cover NVS, so on a board with flash encryption on the secret is still plaintext, and no released build turns flash encryption on. The O2 gate keeps the secret off boards without flash encryption only; `spec/canary_mesh_network_v0.md` §5.5.)*'),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     '        + "it raises while it is off your Wi-Fi) will take the correction."'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     '            resolveProvision(.failed("This Canary\'s firmware doesn\'t offer the Bluetooth rescue — use its own setup network (SecuraCV-wap-XXXX, which it raises while it is off your Wi-Fi) instead."))'),
    # #1843's sweep, third pass (its review round 2):
    ('firmware/provisioning/platformio_secure.ini',
     '; - Turns on neither Secure Boot nor flash encryption, and no step of the ; kit does.'),
    ('firmware/provisioning/platformio_secure.ini',
     'so a Phase 2 board has flash encryption off and Secure Boot ; enabled with nothing to verify against.'),
    ('firmware/provisioning/sdkconfig.defaults.secure',
     '# IMPORTANT: Secure Boot and Flash Encryption are commented out below, # and uncommenting them changes nothing under framework = arduino.'),
    ('firmware/provisioning/sdkconfig.defaults.secure',
     '# What an ESP-IDF build would set; no Canary build reads them today:'),
    ('firmware/provisioning/README.md',
     '**Phase 2 (target, not reached):** Secure Boot v2, flash encryption and JTAG disabled.'),
    ('firmware/provisioning/README.md',
     '## Phase 2 (not usable yet: do not run it on a board you want to keep)'),
    ('firmware/provisioning/generate_keys.sh',
     '"The kit does not use the signing key yet: no Secure Boot key digest is burned and nothing is signed with it",'),
    ('docs/secure_provisioning.md',
     'The kit does not use the signing key yet (no step burns its digest or signs with it, Step 3)'),
    ('docs/secure_provisioning.md',
     '# keys/secure_boot_signing_key.pub (not yet used: no step burns its digest)'),
    ('docs/secure_provisioning.md',
     'The secure env compiles no Bluetooth code: its `-DFEATURE_BLE=0`,'),
    ('docs/secure_provisioning.md',
     '# There is no ESP-IDF project for this tree yet'),
    ('canary-local/assets/flash.js',
     '"neither the firmware, the identity key nor the saved WiFi (it does " +'),
    ('ios/Sources/SecuraCV/Security/DeviceStore.swift',
     '// flasher, whose factory image starts NVS over and so gives it a new'),
    ('custom_components/securacv/tests/test_replay_reset_paths.py',
     'factory image starts NVS over), or replaced, starts them over (an OTA update'),
    ('firmware/common/network/auth_backoff.h',
     '// erase in either flasher starts NVS over and gives it a new key, so a new'),
    ('firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h',
     '// erase in either flasher starts NVS over and gives it a new key, so a new'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "factory image starts NVS over and so regenerates the WAP's key (an OTA"),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     'an OTA update keep the token, and an install or a full erase in either'),
    ('firmware/LESSONS_LEARNED.md',
     're-installed unit (an install or a full erase in either flasher starts NVS'),
    ('CHANGELOG.md',
     'still polling a re-installed unit (an install or a full erase in either'),
    ('CHANGELOG.md',
     'pass also wrote that an ordinary re-flash keeps NVS; it does not, since'),
    ('CHANGELOG.md',
     "  come back: the fourth pass below); the flagship's 30 s wipe, and an install\n"
     '  or a full erase in either flasher (whose factory image starts NVS over),'),
    ('CHANGELOG.md',
     'install or a full erase in either flasher, each of which rewrites the'),
    ('firmware/canary/scripts/ota_deploy.py',
     'log_error("Refusing to send the token. Only the 30 s BOOT wipe, or an install or a full")'),
    ('firmware/canary/scripts/ota_deploy.sh',
     'log_error "Refusing to send the token. Only the 30 s BOOT wipe, or an install or a full"'),
    ('docs/security/PENTEST_SCOPE.md',
     'or a full erase in either flasher (whose factory image starts NVS over), sheds them.'),
    ('docs/design/magic_pairing.md',
     'full erase in either flasher, whose factory image starts NVS over, replaces'),
    ('desktop/src/index.html',
     'not its identity: every install starts the settings region over,'),
    ('firmware/projects/canary-sense/include/canary/witness.h',
     '// OTA installs and a PlatformIO upload (which leaves NVS alone); an install'),
    ('firmware/projects/canary-vision/include/canary/witness.h',
     '// OTA installs and a PlatformIO upload (which leaves NVS alone); an install'),
    ('firmware/projects/canary-sentinel/include/canary/witness.h',
     '// keypair survives OTA installs and a PlatformIO upload (which leaves NVS'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '// it stays stable across OTA updates and the 10 s hold because the keypair'),
    ('canary-local/tools/gen_flash.py',
     '"its sealed diary. The chain rides through over-the-air updates "'),
    ('canary-local/devices/flash.json',
     'The chain rides through over-the-air updates untouched; an install here starts the settings region over'),
    ('canary-local/tools/gen_flash.py',
     '"install already starts the settings region over, the identity key with it, "'),
    ('canary-local/devices/flash.json',
     'A normal install already starts the settings region over, the identity key with it, so the board comes up with a new identity either way.'),
    ('canary-local/tools/gen_flash.py',
     '"Off — a normal install writes only the regions its image covers (the "'),
    ('canary-local/devices/flash.json',
     'Off — a normal install writes only the regions its image covers (the bootloader, the partition table, a fresh settings region and the firmware) and leaves the rest.'),
    ('firmware/LESSONS_LEARNED.md',
     "*(Amended 2026-10-22, #1843: the header's sentence now reads"),
    ('firmware/LESSONS_LEARNED.md',
     'A door that is open to whichever client connects while no network is saved must still be a door nobody can wedge.'),
    ('docs/design/iphone_companion_app.md',
     "nothing but the per-device tokens it was handed when it paired (the setup door's"),
    ('docs/design/iphone_companion_app.md',
     'BOOT-button receipt this step first planned was never built into the app);'),
    ('ios/Sources/SecuraCV/Views/FleetWiFiSheet.swift',
     'candidateRow(c, note: "Not answering on your network. Off your Wi-Fi, its own "'),
    ('ios/Sources/SecuraCV/Views/FleetWiFiSheet.swift',
     '+ "goes only to your Canaries — over a secure connection when "'),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     '+ "approved. Re-pair it with a secure (https) receipt, then try "'),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     '+ "re-pair it with one. This app has no Bluetooth lane to these "'),
    ('docs/GLOSSARY.md',
     'That keeps the household secret off every board without flash encryption;'),
    ('docs/design/apple_home_integration.md',
     'which keeps them off every board without flash encryption'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     'So today the gate keeps the secret off every board without flash'),
    ('firmware/projects/canary-wap/arduino/canary_wap/mesh_network.cpp',
     '// which keeps the secret off boards without it only.'),
    ('spec/canary_mesh_network_v0.md',
     'The FE gate keeps the secret off every board without flash encryption,'),
    ('spec/beacon_channel_v0.md',
     'a board without flash encryption keeps the set in RAM only'),
    ('firmware/scripts/regression_check.sh',
     'the gate keeps it off boards without flash encryption, it does not encrypt NVS'),
    ('docs/audit/mesh_and_chirp_audit_v1.md',
     'The O2 gate keeps the secret off boards without flash encryption only;'),
    # #1843's sweep, fourth pass (its review round 3): both flashers' install
    # verdicts and first-contact copy, the browser trust card's key that
    # "never leaves", the device_id and handle said to survive reflashes, the
    # flagship's certificate comment and the other key-change lists that left
    # out an install, the token the flagship's hold does not keep, the
    # dashboard's press-at-once retry lines, the kit's Phase 2 run and table
    # notes, and Secure Boot described as not enabled.
    ('canary-local/assets/flash-core.js',
     '  "A USB install starts the settings region over, so the board comes up with no saved " +'),
    ('canary-local/assets/flash-core.js',
     '    detail: "The board already runs this exact version. Installing again is a clean start, " +'),
    ('canary-local/assets/flash-core.js',
     '            "occasionally exactly what a misbehaving board needs. " + USB_INSTALL_STARTS_OVER,'),
    ('canary-local/assets/flash-core.js',
     '              "and the automatic safety copy is your way back. " + USB_INSTALL_STARTS_OVER,'),
    ('desktop/src/app.js',
     '      detail: "A newer build of the same firmware. " + USB_INSTALL_STARTS_OVER };'),
    ('desktop/src/app.js',
     '    detail: "The board already runs this exact version. Installing again is a clean start, " +'),
    ('desktop/src/app.js',
     '      "occasionally exactly what a misbehaving board needs. " + USB_INSTALL_STARTS_OVER };'),
    ('desktop/src/app.js',
     '        "misbehaves — and the automatic safety copy is your way back. " + USB_INSTALL_STARTS_OVER };'),
    ('canary-local/assets/flash.js',
     '    "chip. The firmware has no export function for the private key, and " +'),
    ('canary-local/assets/flash.js',
     '    "exactly that read. Every witness record is hash-chained to the previous " +'),
    ('canary-local/assets/flash.js',
     '    "permission; forging it needs that key: the board\'s flash, or a copy of it."));'),
    ('canary-local/assets/flash.js',
     '    "Key theft over the network — this page sends nothing it reads off the " +'),
    ('canary-local/assets/flash.js',
     '        "either way: they live in the settings region, which every install starts over. "));'),
    ('canary-local/assets/flash.js',
     '      const mine = el("button", "ghost small", "it\'s mine — skip the full erase");'),
    ('canary-local/assets/flash.js',
     '  // The user\'s explicit "this board is already mine, skip the full erase". The'),
    ('canary-local/assets/flash.js',
     '    "verify against (a board may not boot afterward), so today it protects " +'),
    ('canary-local/assets/flash.js',
     '    // once written (until the next install here starts NVS over) — so with'),
    ('canary-local/assets/intake.js',
     '//   - `ownerClaimed` — the human said "this is my board, skip the full erase"'),
    ('docs/unflashed_board_intake.md',
     '  deliberate "it\'s mine — skip the full erase" button. That lets you reinstall'),
    ('docs/unflashed_board_intake.md',
     "  the one party a hostile image can't impersonate. It does not keep the\n"
     '  identity key: the factory image every install writes carries a blank'),
    ('canary-local/tools/gen_flash.py',
     '             "region over, where the identity key and the diary’s chain state "'),
    ('canary-local/devices/flash.json',
     'A USB factory install like this one is deeper: it starts the settings region over, where the identity key and the diary’s chain state live, so the board starts a new chain under a new key.'),
    ('firmware/projects/canary-sense/include/canary/runtime_config.h',
     '//   - device_id: NVS always wins, so it is kept across OTA updates and a'),
    ('firmware/projects/canary-vision/include/canary/runtime_config.h',
     '//   - device_id: NVS always wins, so it is kept across OTA updates and a'),
    ('firmware/projects/canary-sentinel/include/canary/runtime_config.h',
     '//   - device_id: NVS always wins, so it is kept across OTA updates and a'),
    ('firmware/projects/canary-display/include/canary/runtime_config.h',
     '//   - device_id: NVS always wins, so it is kept across OTA updates and a'),
    ('firmware/projects/canary-display/arduino/canary_display/runtime_config.h',
     '//   - device_id: NVS always wins, so it is kept across OTA updates and a'),
    ('firmware/canary/lib/securacv_crypto/src/securacv_crypto.cpp',
     '// the handle lasts as long as the key — a *device* handle, not a per-boot'),
    ('firmware/canary/lib/securacv_crypto/src/securacv_crypto.h',
     '// alphabet (no 0/O/o, 1/I/i/l/L); it lasts as long as the key does (an'),
    ('firmware/common/identity/device_pseudonym.h',
     '// fixed for as long as NVS keeps it (an OTA update keeps it; an install in either'),
    ('firmware/projects/canary-wap/arduino/canary_wap/device_pseudonym.h',
     '// fixed for as long as NVS keeps it (an OTA update keeps it; an install in either'),
    ('firmware/projects/canary-display/arduino/canary_display/device_pseudonym.h',
     '// fixed for as long as NVS keeps it (an OTA update keeps it; an install in either'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  // One SSID for as long as the key lasts — the same name during first-boot'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '// witness records and health log on SD, the API token (the one derived from'),
    ('firmware/LESSONS_LEARNED.md',
     '  bits (injective, so per-board uniqueness is preserved, and the suffix still'),
    ('docs/firmware_ota.md',
     '  survive them. An install in either flasher does not: its factory image'),
    ('canary-local/tools/gen_sense.py',
     '        "device_id: NVS always wins — it is kept across OTA updates and a re-run of the upload above (which leaves NVS alone);'),
    ('canary-local/devices/sense.json',
     '"device_id: NVS always wins \\u2014 it is kept across OTA updates and a re-run of the upload above (which leaves NVS alone);'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_improv.cpp',
     '// own TTL. A phone that never reads it pairs later from a recovery kit: the'),
    ('firmware/canary/include/canary_config.h',
     '// OTA update keep both (reset_scope_keys.h kIdentityKept); the 30 s wipe\n'
     '// (ResetScope::Everything), or an install or a full erase in either flasher'),
    ('custom_components/securacv/sensor.py',
     '    device is (re)pinned via TOFU (async_record_verify), which is what an\n'
     '    install or a full erase in either flasher (whose factory image starts'),
    ('custom_components/securacv/tests/test_replay_and_device_id.py',
     '    # An install or a full erase in a flasher (a new key): the device pins'),
    ('firmware/canary/src/csi_event_log.h',
     ' * the 30 s BOOT wipe or an install or a full erase in either flasher, each'),
    ('canary-local/tools/hatchery/derive.mjs',
     '//   a full erase in either flasher (whose factory image starts NVS over), or'),
    ('firmware/common/health/boot_policy.h',
     ' *   Re-uploading the SAME build with PlatformIO or the Arduino IDE (which'),
    ('firmware/common/health/boot_policy.h',
     '// Re-uploading the SAME build over PlatformIO or the Arduino IDE keeps the'),
    ('firmware/tests_host/test_boot_guard.cpp',
     ' *     (a PlatformIO or Arduino IDE upload, which leaves NVS alone: an install'),
    ('firmware/common/csi/src/csi_event_id_floor.h',
     '// keeps the identity), so its owner installs it again in a flasher (with'),
    ('firmware/projects/canary-wap/arduino/canary_wap/csi_event_id_floor.h',
     '// keeps the identity), so its owner installs it again in a flasher (with'),
    ('docs/csi_developer_api.md',
     '  keeps both, so its owner installs it again in a flasher (with or without'),
    ('docs/TRUST_MODEL.md',
     'flasher sheds it too, on every Canary that keeps one: its factory image starts'),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     '        ? "Didn\'t see a tap. Click Use BOOT Tap Once again, and short-tap BOOT when this line says to."'),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     "            s.textContent = tapContendedText('click Use BOOT Tap Once again, and short-tap BOOT when this line says to.');"),
    ('docs/FAQ.md',
     'seconds, then click **Use BOOT Tap Once** again and short-tap BOOT when'),
    ('docs/audit/hardware_verification_checklist.md',
     'After 60 s the page says "Didn\'t see a tap. Click Use BOOT Tap Once again, and short-tap BOOT when this line says to."'),
    ('CHANGELOG.md',
     '  the identity key, the chain and the HTTPS certificate on every Canary, and'),
    ('CHANGELOG.md',
     '  now says the 10 s reset keeps the identity and the certificate by design,'),
    ('CHANGELOG.md',
     '  upload and the 10 s BOOT reset keep the key and the certificate (the'),
    ('CHANGELOG.md',
     '  changes it; these copies now say what the flashers do (the fourth pass'),
    ('CHANGELOG.md',
     '    blank, and the third and fourth passes below correct the copies.)'),
    ('CHANGELOG.md',
     '  seconds, then click Use BOOT Tap Once again, and short-tap BOOT when this\n'
     '  line says to" (the companion: tap Save again, and press when its note'),
    ('CHANGELOG.md',
     '  Tap Once again, and short-tap BOOT when this line says to."; it said'),
    ('docs/security/PENTEST_SCOPE.md',
     "  A WAP's hold keeps its API token too, whether the WAP derived it from the"),
    ('docs/design/magic_pairing.md',
     'API token (the one the WAP derived from the key, or the one a flasher'),
    ('docs/design/magic_pairing.md',
     '  hold and Forget Wi-Fi keep both: the SoftAP password derives from the'),
    ('docs/design/magic_pairing.md',
     "new token and device_id; an OTA update and a WAP's 10 s hold keep the token,"),
    ('firmware/projects/canary-wap/README.md',
     'keeps the identity key and the API token (derived from the key, or the'),
    ('firmware/projects/canary-wap/arduino/canary_wap/README.md',
     '- API token derived via HKDF from the private key (or seeded random by a flasher at install), stored in NVS, which is plaintext on every tier today (NVS encryption is in no released build)'),
    ('docs/security/THREAT_MODEL.md',
     'Every install over that pull path verifies the Ed25519 release signature and the version floor first'),
    ('docs/security/THREAT_MODEL.md',
     "that is Secure Boot's job, which works on no Canary today: the provisioning kit's Phase 2 sets `SECURE_BOOT_EN` with no Secure Boot key digest, so Secure Boot is enabled with nothing to verify against"),
    ('docs/security/THREAT_MODEL.md',
     '- Secure boot (would verify firmware integrity; prepared in the provisioning kit, and on no tier does it verify anything yet: Phase 2 sets `SECURE_BOOT_EN` with no key digest)'),
    ('docs/design/household_presence_and_posture.md',
     '  it), and secure boot works on no tier: the provisioning kit prepares it,'),
    ('docs/device_trust.md',
     'that `device_id`: read the key off the device itself (the table above) and'),
    ('docs/secure_provisioning.md',
     '# Do not run it without --dry-run on a board you want to keep: as tooled it'),
    ('docs/secure_provisioning.md',
     '### Step 3: Preview the Provisioning (Dry Run Only)'),
    ('firmware/provisioning/provision_canary.sh',
     '# SecuraCV Canary — Provisioning Script (Phase 2 not usable yet)'),
    ('firmware/provisioning/provision_canary.sh',
     'target, is not usable yet: run it only with --dry-run (see Phase 2 below).'),
    ('firmware/provisioning/provision_canary.sh',
     '  # Phase 2 without --dry-run is IRREVERSIBLE and not usable yet: it turns'),
    ('firmware/provisioning/generate_keys.sh',
     '    echo "for SecuraCV Canary\'s Phase 2 provisioning, which is not usable yet:"'),
    ('firmware/provisioning/README.md',
     '- Run Phase 2 only with `--dry-run` today (Phase 2 above); Phase 1 burns nothing'),
    ('firmware/provisioning/partitions_secure.csv',
     '#   Encrypted flags are still not enforced: it burns no SPI_BOOT_CRYPT_CNT,'),
    ('firmware/provisioning/partitions_secure.csv',
     "# - nvs_keys is where ESP-IDF's NVS encryption would keep its keys; no"),
    ('ios/Shared/SetupGuide.swift',
     'body: "A WAP keeps its own setup page on its own network, SecuraCV-wap-XXXX. Join it'),
    ('docs/research/face_id_without_faces.md',
     "core's precompiled `sdkconfig` has it), and secure boot works on no tier: the"),
    ('docs/security/THREAT_MODEL.md',
     '**LIMITATION:** Once Secure Boot works (on no tier today: Phase 2 sets'),
    ('CHANGELOG.md',
     '  *(Amended 2026-10-22, #1843\'s sweep: `improv_core.h` now reads "a device'),
    ('CHANGELOG.md',
     '  changes it; these copies now say what the flashers do (the fourth pass'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '      either reset reopens the same first-come door. *(Amended 2026-10-22,\n'
     "      #1843's sweep, fourth pass: the token is derived from the key only"),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '    Click Use BOOT Tap Once again, then short-tap BOOT." *(Amended\n'
     '    2026-10-22, #1843\'s sweep, fourth pass: it now says "…, and short-tap'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "      only then tap *(amended 2026-10-22, #1843's sweep, fourth pass: both\n"
     "      now say to short-tap BOOT when the sign-in's line says to)*; the companion"),

    ('firmware/canary/src/main.cpp',
     ': "ZERO or unreadable - an upload that keeps NVS will not reset the crash-loop count");'),
    ('firmware/canary/src/main.cpp',
     '  // Bench row E6: a PlatformIO or Arduino IDE upload of another build, which'),
    ('firmware/common/health/boot_guard.h',
     '// and a PlatformIO or Arduino IDE upload of another build (which leaves NVS'),
    ('docs/security/THREAT_MODEL.md',
     '  too, the secure-tier bullet above), and on the default tier an install or\n'
     '  a full erase in either flasher, each of which rewrites the settings\n'
     '  region, is what removes them.'),
    ('docs/security/THREAT_MODEL.md',
     '**Result:** Requires Ed25519 private key (no firmware path exports it, but'),
    ('docs/security/THREAT_MODEL.md',
     '**PASS if:** Forgery requires the device, or a full read of its flash such as\n'
     "a flasher's safety copy, AND key extraction from it."),
    ('docs/security/THREAT_MODEL.md',
     'and stored in NVS. By anything the firmware runs, it is **never**:'),
    ('docs/security/THREAT_MODEL.md',
     '*firmware* paths. It is not a claim that the key survives an adversary who\n'
     'holds the device, over its USB port or on a bench: the key lives in NVS, which flash encryption\n'
     '(an opt-in tier the provisioning kit does not yet turn on) does not cover,'),
    ('docs/security/THREAT_MODEL.md',
     'flash directly, on a fused board too (Scenario 4 below). Our own flashers\n'
     'do exactly that read: before an install, unless told to skip it, each reads\n'
     'the whole flash over USB, through the chip\'s ROM download mode, which the\n'
     'Canary firmware does not control, and saves it as a safety copy on the\n'
     'computer'),
    ('docs/security/THREAT_MODEL.md',
     'Reviewers should hold this principle to the firmware boundary and treat'),
    ('docs/security/THREAT_MODEL.md',
     '  provisioning kit but work on no tier today (Scenario 4): its Phase 2\n'
     '  leaves flash encryption off and Secure Boot enabled with nothing to\n'
     '  verify against, burns the JTAG-disable fuses,'),
    ('docs/unflashed_board_intake.md',
     'where secure boot and flash encryption are staged, and as tooled it turns on\n'
     'neither: it leaves flash encryption off and Secure Boot enabled with nothing\n'
     'to verify against.'),
    ('docs/secure_provisioning.md',
     '| `sdkconfig.defaults.secure` | ESP-IDF Kconfig, staged: BT disabled, Secure Boot v2 (with anti-rollback) and Flash Encryption (release mode) commented out, NVS encryption named. The whole file is inert under `framework = arduino`, so uncommenting those lines turns nothing on;'),
    ('firmware/provisioning/sdkconfig.defaults.secure',
     '# NVS ENCRYPTION (would protect stored keys and settings; inert)'),
    ('firmware/provisioning/sdkconfig.defaults.secure',
     "# An ESP-IDF build would encrypt the NVS partition's contents with it. No\n"
     '# Canary build is one, so it protects nothing on any board, Phase 1 or\n'
     '# Phase 2.\n'
     '# KNOWN INERT under framework = arduino (platformio_secure.ini): the line\n'
     "# below never reaches the Arduino core's precompiled build, so no released\n"
     '# image has NVS encryption, and NVS stays plaintext on every board.'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.cpp',
     ' * user must complete pairing on every boot until flash encryption is\n'
     ' * on, and then the stored secret is still plaintext NVS, which flash\n'
     ' * encryption does not cover (mesh_state.h).'),
    ('docs/TRUST_MODEL.md',
     "its bonded Bluetooth lane (`ble_provision.cpp`'s CREDS, an authenticated"),
    ('docs/IMPROVEMENT_ROADMAP.md',
     '*(Amended 2026-10-22, #1843: no device fills the BLE lane today.'),
    ('firmware/LESSONS_LEARNED.md',
     '  2026-10-22, #1843: no WAP fills that lane, since the app dials only a'),
    ('firmware/LESSONS_LEARNED.md',
     '  in the firmware. *(Amended 2026-10-22, #1843: a read of the whole flash goes'),
    ('firmware/LESSONS_LEARNED.md',
     '  ways to start fresh on purpose are an install in either flasher, with or\n'
     '  without its full erase (its factory image starts NVS over), on either\n'
     "  tree, and the flagship's 30 s BOOT wipe;"),
    ('firmware/projects/canary-wap/arduino/canary_wap/README.md',
     'a flasher seeded at install or, with none, one derived per device. See'),
    ('firmware/projects/canary-wap/secrets/secrets.example.h',
     ' *   - The API token: the one a flasher seeded at install or, with none,\n'
     ' *     one derived per device from the private key (derive_api_token);'),
    ('firmware/projects/canary-wap/setup.sh',
     ' *   - The API token: the one a flasher seeded at install or, with none,\n'
     ' *     one derived per device from the private key (derive_api_token);'),
    ('docs/onboarding_workflow_evaluation.md',
     '   keypair, then derives a `device_id` (`canary-s3-4dC2`) and AP SSID'),
    ('docs/onboarding_workflow_evaluation.md',
     '   when none is stored. (`canary_wap.ino`)'),
    ('docs/security/PENTEST_SCOPE.md',
     'Extracting a device identity key by any means but a read of the flash of a board in your hands'),
    ('docs/security/PENTEST_SCOPE.md',
     '**Retrieving key bytes by any path but the documented flash read is always a finding'),
    ('docs/security/PENTEST_SCOPE.md',
     '   *retrieval* of key bytes by any path but that documented flash read is\n'
     '   always a finding**'),
    ('docs/security/PENTEST_SCOPE.md',
     '   target. A read of its flash, over USB or on a bench, recovers it on every'),
    ('docs/security/PENTEST_SCOPE.md',
     '- **Flash encryption and Secure Boot are off by default.** Neither works on\n'
     '  any Canary today, in fact: the opt-in provisioning kit stages both and turns\n'
     '  on neither'),
    ('docs/security/SECURITY_MODEL.md',
     "- Stored on the device's internal flash memory, in plaintext NVS"),
    ('docs/security/SECURITY_MODEL.md',
     '- Never sent by the firmware over any interface (WiFi, USB, or otherwise)'),
    ('docs/security/SECURITY_MODEL.md',
     '- Not backed up or escrowed by the firmware, and never held by ERRERlabs'),
    ('docs/security/SECURITY_MODEL.md',
     'is no firmware path that will hand it over: no API, no export, no'),
    ('docs/security/SECURITY_MODEL.md',
     '**The limit of that promise.** It is a statement about the firmware.'),
    ('docs/security/SECURITY_MODEL.md',
     "1. The device's Ed25519 private key (which no firmware path exports; a"),
    ('docs/security/SECURITY_MODEL.md',
     '**The honest statement: someone who takes a Canary away can read the\n'
     'private key out of its flash, over its USB port or off the chip, in its'),
    ('CHANGELOG.md',
     "  halted or not *(amended 2026-10-22, #1843's sweep: any install in either"),
    ('CHANGELOG.md',
     '  service for a brand-new unit *(amended 2026-10-22, #1843: that step could'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '  its only way to a new identity on purpose, halted or not *(amended\n'
     "  2026-10-22, #1843's sweep: any install in either flasher, with or without"),
    ('firmware/README.md',
     'The opt-in provisioning kit is in `provisioning/`. Its Phase 2, the'),
    ('firmware/README.md',
     '# the opt-in key + eFuse kit, Phase 2 dry-run only'),
    ('firmware/README.md',
     '# Phase 1 workflow (the default; burns no eFuse)'),
    ('firmware/canary/include/secure_defaults.h',
     '// The Ed25519 private key is stored in NVS and the firmware has no read'),
    ('firmware/canary/include/secure_defaults.h',
     '// That is the FIRMWARE boundary. A read of the whole flash goes around it:'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wap_server.h',
     " * - Provisioning receipt only for the bearer token, the setup door's claim, or"),
    ('ios/Sources/SecuraCV/Transport/DeviceAPI.swift',
     "/// to its bearer, to a WAP's setup-door claim, or after a short BOOT tap. This"),
    ('desktop/src/app.js',
     'it was probably reflashed elsewhere, or, on an all-rounder Canary, reset with its 10-second BOOT hold'),
    ('desktop/src/app.js',
     "a reflash mints a new one, and an all-rounder Canary's 10-second BOOT reset drops it"),
    ('desktop/src/app.js',
     '"region over; the full erase also wipes the partitions an install never writes " +\n'
     '      "(the second firmware slot, the file system), where a previous firmware may have " +\n'
     '      "left stale data. Reconnect and tick first contact.",'),
    ('canary-local/assets/flash-core.js',
     '"region over; the full erase also wipes the partitions an install never writes " +\n'
     '      "(the second firmware slot, the file system), where a previous firmware may have " +\n'
     '      "left stale data. Reconnect and choose “clean install”.",'),
    ('canary-local/assets/flash.js',
     "\"(an all-rounder Canary's 10-second BOOT reset drops it for one the board derives from its own key). \" +"),
    ('docs/flasher_profiles_fleet_book.md',
     '  elsewhere, or an all-rounder Canary reset with its 10 s BOOT hold, which'),
    ('docs/onboarding_multiple_canaries.md',
     'as long as the key: an over-the-air update and the 10-second BOOT reset keep'),
    ('docs/onboarding_multiple_canaries.md',
     'Each board makes its own Ed25519 identity key on its first boot. The firmware'),
    ('docs/manual_test_plan_captive_portal.md',
     "  device's identity key fingerprint, never the MAC, encoded in the"),
    ('docs/manual_test_plan_captive_portal.md',
     '  new key and so a new suffix. The softAP is up while the device is'),
    ('docs/FAQ.md',
     "network's password, so only a BOOT tap, the token itself or the setup\n"
     "door's one-time claim fetches it, never the dashboard's own session."),
    ('canary-local/tools/gen_wap.py',
     '"tap of RESET; an install in either flasher is what starts NVS over)",'),
    ('canary-local/devices/wap.json',
     'witness chain ride through every tap of RESET; an install in either flasher is what starts NVS over)'),
    ('docs/review/02-roadmap.md',
     "  *(Amended 2026-10-22, #1843: `nvs_keys` is where an ESP-IDF build's NVS encryption would keep its"),
    ('docs/review/02-roadmap.md',
     '   `nvs_keys` for this (but fix its 4 MB assumption — F-06). *(Amended 2026-10-22, #1843: no'),
    # #1843's sweep, sixth pass: the remaining copies of the corrected
    # positions, each where it stood (the forms are the text as the pass
    # found it; the replacements are what stands there now).
    ('docs/unflashed_board_intake.md',
     "- [secure_provisioning.md](secure_provisioning.md) — the Phase 2 lockdown's\n"
     '  staging, which as tooled turns on neither Secure Boot nor flash encryption\n'
     '  (read "After the flash" above before you burn anything)'),
    ('docs/security/SECURITY_MODEL.md',
     '**If your threat model is an intruder, a landlord, or a domestic abuser**,\n'
     'the default is still the right one for you (no tier protects the key today),\n'
     'and the tamper-evidence properties are unaffected. But anyone who can take\n'
     'the device, even for a few minutes with a laptop and a USB cable, can read\n'
     'its key, so keep it out of their reach.'),
    ('firmware/PARTITIONS.md',
     '| **Secure-kit reference table** (Phase‑2 provisioning, which as tooled turns on neither Secure Boot nor flash encryption) | 8 MB |'),
    ('firmware/PARTITIONS.md',
     "| `provisioning/platformio_secure.ini` (the Phase‑2 kit's compile-only env; Phase 2 turns on neither flash encryption nor Secure Boot) |"),
    ('docs/sd_card_health.md',
     'card, erase the `diag` NVS namespace. (An install in either flasher re-zeroes\n'
     'them too, with or without its full erase, but it starts all of NVS over, the\n'
     'identity key and the chain state with it, so the device comes back as a new\n'
     'witness.)'),
    ('docs/canary_settings_access_validation.md',
     "| API token fallback | In a browser with no session for the device (as in the WiFi-only row), open `/settings`, paste the `cv_...` API token into the auth modal and click Connect. | Modal closes and Settings data loads. Invalid token shows a clear error. (On a page a session already signed in, the box takes the session's word for any token: do not test or advise pasting there.) |"),
    ('docs/browser_flasher.md',
     '   names the product: "This board already runs X, so it is preselected. An\n'
     '   install starts its settings over (a new witness key, where it keeps one);\n'
     '   an over-the-air update keeps them."'),
    ('docs/browser_flasher.md',
     "the iOS app's `BLEConsole.swift` knows the same UUIDs but never reaches a WAP's\n"
     'console: it dials only a peripheral advertising the console service, which a\n'
     "WAP does not, ledger A65). The flasher's"),
    ('docs/design/magic_pairing.md',
     "| credentials vanish at runtime (the 10 s hold before its restart, or a WAP's Forget Wi-Fi, which any landing session can send, F345) | the window is re-armed from now, and the door goes to whichever client connects first (§2.2) |"),
    ('docs/design/magic_pairing.md',
     'A device nobody owns yet — or, after a reset or a Forget Wi-Fi, a cleared unit that still has an owner (on a WAP any landing session can send Forget Wi-Fi, ledger F345, so the owner may not have chosen it):'),
    ('docs/design/iphone_companion_app.md',
     '   Snapshot char `8fc1cee1-…`, bonded) + chirp/beacon scan — off-grid status\n'
     '   (as shipped, the console reaches no WAP: `BLEConsole` dials only a\n'
     '   peripheral advertising the console service, which a WAP does not, ledger\n'
     '   A65; the connectionless beacon and chirp scan are what work off-grid).'),
    ('docs/firmware_ota.md',
     "Reconnect via dashboard (canary/WAP), or join the setup network from a phone and enter the new password (vision/sense). A USB install also works, but either flasher's install starts the board's settings over: a new witness key and, on most boards, a new device id and new Home Assistant entities (the canary-vision section above), so keep it for when neither path can be reached |"),
    ('docs/FAQ.md',
     '`SecuraCV-XXXX` recovery network instead, which needs the key printed for\n'
     "it. So the setup door offers nothing to a stranger's phone while a Canary\n"
     'stays on your network, on a good day or a bad one. Two caveats on a WAP: any\n'
     'device on your network can open its dashboard and send Forget Wi-Fi, which\n'
     'reopens the door for half an hour; and a device on your network that is also\n'
     "in Bluetooth range can bond a phone of its own to the WAP's other Bluetooth\n"
     'lane, which takes a new Wi-Fi network (the\n'
     '[security model](security/SECURITY_MODEL.md) has the details).'),
    ('docs/security/THREAT_MODEL.md',
     '- Private keys are not "access-controlled" — the firmware has no read\n'
     '  interface for them (a whole-flash read, over USB through the ROM download\n'
     '  mode or on a bench, still recovers the key: NVS is plaintext on every\n'
     "  tier, Principle 1's scope)"),
    ('docs/security/THREAT_MODEL.md',
     '3. The firmware offers no read or export path for the Ed25519 private key (a\n'
     '   full flash read over USB or on a bench still recovers it on every tier:\n'
     '   SECURITY_MODEL.md, "Physical extraction and the flash-encryption default")'),
    ('docs/design/flasher_experience.md',
     '  self-manifest carries no secret (the firmware has no path that exports the key;\n'
     "  a full flash read, which is what either flasher's safety copy is, does carry it,\n"
     '  as the trust card says), so reading it is safe.'),
    ('docs/strategy/08-product-strategy.md',
     'evidently true. SecuraCV is built for the world after both realizations: **your\n'
     "footage is nobody's business, and your proof shows any edit.** (Forging a record\n"
     "takes the device's own key, which a seized device gives up: tamper-evident, not\n"
     'unforgeable.)'),
    ('canary-vision/docs/firmware-integration.md',
     "The witness chain signing key pair is generated once during provisioning and stored in NVS. No firmware path exports the private key: it is read into a stack buffer, used to sign a hash, and immediately zeroed. It still sits in plaintext NVS, so a full flash read (a flasher's pre-install safety copy among them) carries it."),
    ('canary-vision/docs/discovery.md',
     '  device_id=canary-a3f7      # device identifier (kept across OTA updates and the 10 s hold; an install in either flasher can change it)'),
    ('spec/beacon_channel_v0.md',
     'All endpoints take `api_auth_check()`, like `/api/mesh/*` and `/api/bluetooth/*`: a bearer, or the dashboard session cookie, which the landing page mints for any host on the network (F346).'),
    ('spec/canary_mesh_network_v0.md',
     'encryption, at the cost of persisting nothing on any released board: no tier\n'
     "turns flash encryption on today, a Phase 2 board included, so a household's\n"
     'pairing lasts only until the next reboot.'),
    ('docs/V1_LAUNCH_REVIEW.md',
     '   **#610**. (Note: by design the live in-RAM session still runs for the current boot on an FE-off\n'
     "   dev board *(amended 2026-10-22, #1843's sweep: every released board is FE-off, since no tier and\n"
     '   no kit phase turns flash encryption on)*;'),
    ('firmware/LESSONS_LEARNED.md',
     '  credentials vanishing at runtime (factory reset) *(amended 2026-10-22,\n'
     "  #1843's sweep: on a WAP also Forget Wi-Fi, which any landing session can\n"
     "  send, F345, so the re-arm is not the owner's act alone)* re-arm the window,"),
    ('firmware/LESSONS_LEARNED.md',
     '  listener, not an active one who paired first. The bearer token lasts for\n'
     "  the life of the device *(amended 2026-10-22, #1843's sweep: until an install\n"
     '  or a full erase in a flasher; OTA updates, the 10 s hold and Forget Wi-Fi\n'
     '  keep it, and nothing rotates it)*. Nothing that lasts belongs on a link like\n'
     '  that.'),
    ('firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md',
     '  - [x] Credential reset path without the serial monitor: one BOOT tap,\n'
     "        pressed while the owner's page or the wizard's Save is already\n"
     '        asking, releases the provisioning receipt to that one asker\n'
     '        (`GET /api/provisioning-receipt`; a second address asking in the\n'
     '        window closes the tap, 409 `tap_contended`, sweep F342), which\n'
     '        re-reveals the API token and the AP password; the wizard\'s "Save\n'
     '        your recovery kit" block walks it.'),
    ('firmware/projects/canary-wap/README.md',
     'and by **Forget Wi-Fi** or the 10 s BOOT hold on a unit that had\n'
     'credentials, each of which re-arms the window; any landing session can send\n'
     'Forget Wi-Fi, ledger F345, so a re-armed window is no proof of the owner), a phone at the door is a sign of life'),
    ('firmware/README.md',
     '- Local-first data storage\n'
     '- RF-presence session tokens rotate (the API token does not: an OTA update\n'
     "  and a WAP's 10 s hold keep it)"),
    ('ios/README.md',
     '**Dynamic Island / Live Activities**, **CoreBluetooth** (iOS Safari has no Web\n'
     'Bluetooth; the app uses it for the fleet beacons and the setup door, and does\n'
     "not yet reach a WAP's console or bonded lane, ledger A65), **HomeKit**,"),
    ('ios/README.md',
     '  waiting out the 20-second cycle. A console NOTIFY would re-evaluate tamper\n'
     "  at once, sub-second, but the app reaches no WAP's console today (ledger\n"
     '  A65), so tamper arrives by the chirp and the 5-second sentinel. And the\n'
     '  Test Alert'),
    ('firmware/canary/lib/securacv_witness/src/securacv_witness.cpp',
     '  // The NVS cache persists only every SD_PERSIST_INTERVAL records, so\n'
     '  // after a power cut the cached head can be BEHIND the last record\n'
     '  // actually signed — resuming from it would fork the supposedly\n'
     '  // append-only chain. (An NVS wipe or a flasher install gives a new key,\n'
     "  // so the old card's tail does not verify below and the chain starts\n"
     '  // over at genesis.)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '// The NVS cache persists only every SD_PERSIST_INTERVAL records, so after\n'
     '// a power cut the cached head can be BEHIND the last record actually signed\n'
     '// — resuming from it would fork the supposedly append-only chain. (An NVS\n'
     "// erase or a flasher install gives a new key, so the old card's tail does\n"
     '// not verify below and the chain starts over.)'),
    ('firmware/common/network/auth_backoff.h',
     "// token and device_id; an OTA update and a WAP's 10 s hold leave the token\n"
     "// as it was, while the all-rounder Canary's 10 s reset re-derives it from the\n"
     '// kept key, so a flasher-seeded token is gone). A Table keeps'),
    ('firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h',
     "// token and device_id; an OTA update and a WAP's 10 s hold leave the token\n"
     "// as it was, while the all-rounder Canary's 10 s reset re-derives it from the\n"
     '// kept key, so a flasher-seeded token is gone). A Table keeps'),
    ('firmware/common/network/improv_core.h',
     '  /// A device with no stored credentials opens the door for this long after\n'
     "  /// power-on (and again when they vanish at runtime: a WAP's Forget Wi-Fi,\n"
     '  /// or the 10 s hold before the restart its release brings, after which the\n'
     '  /// window runs from that boot).'),
    ('firmware/common/network/improv_core.h',
     '/// Provisioned or Provisioning for the phone to read. Credentials vanishing\n'
     "/// at runtime (the 10 s hold before its restart, or a WAP's Forget Wi-Fi,\n"
     '/// which any landing session can send, ledger F345) re-arms the window from\n'
     '/// now, and the door goes to whichever client connects first: not proof of\n'
     '/// the owner. A recovery portal'),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     '  /// A device with no stored credentials opens the door for this long after\n'
     "  /// power-on (and again when they vanish at runtime: a WAP's Forget Wi-Fi,\n"
     '  /// or the 10 s hold before the restart its release brings, after which the\n'
     '  /// window runs from that boot).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     '/// Provisioned or Provisioning for the phone to read. Credentials vanishing\n'
     "/// at runtime (the 10 s hold before its restart, or a WAP's Forget Wi-Fi,\n"
     '/// which any landing session can send, ledger F345) re-arms the window from\n'
     '/// now, and the door goes to whichever client connects first: not proof of\n'
     '/// the owner. A recovery portal'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     "// once at the door's init (ble_improv_window_used_ms), zeroed by a\n"
     '// credential wipe (wifi_clear_credentials: the 10 s hold, or Forget Wi-Fi,\n'
     '// which any landing session can send, ledger F345, re-arms the window, as\n'
     "// improv_core.h's session does in RAM). Loop task only after setup()."),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  // A credential wipe (the 10 s hold, or Forget Wi-Fi, which any landing\n'
     "  // session can send, ledger F345) re-arms the Bluetooth setup door's\n"
     "  // first-boot window: improv_core.h's session"),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '// meaningful AFTER first-setup. The handlers were originally registered\n'
     '// without an _auth wrapper, so a LAN-local curl could hit /api/wifi/forget\n'
     '// with no credential at all. They now take api_auth_check(): the cookie or\n'
     '// the bearer, and the landing page mints that cookie for any host on the\n'
     '// network, so any LAN host can still forget the Wi-Fi, which reopens the\n'
     "// first-come setup door (ledger F345). That session is also the owner's\n"
     '// way out under the lockout (F343). (/api/wifi'),
    ('firmware/projects/canary-wap/arduino/canary_wap/bluetooth_api.h',
     "// initializes after WiFi is up, so there's no captive-portal flow that\n"
     '// needs to hit these routes before setup. Wrapping handlers in\n'
     '// api_auth_check is a plumbing-only fix (mirrors #435/#436 for the\n'
     '// wifi-mgmt + mesh families), and it takes the cookie or the bearer: the\n'
     '// landing page mints that cookie for any host on the network, so any\n'
     '// landing session can turn Bluetooth off, delete every bond and read the\n'
     "// passkey (ledger F346, SECURITY_MODEL.md's lockout paragraph)."),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     ' * household-identifying, and gating it would make "off" silently revert\n'
     ' * to "on" at every reboot of a board without flash encryption, which is\n'
     ' * every released board today, a Phase 2 board included.'),
    ('firmware/canary/src/csi_modules_integration.cpp',
     "     * we just won't have the hint cached across the next reboot (on\n"
     '     * every released board: the gate needs flash encryption, which no\n'
     '     * tier turns on today, a Phase 2 board included). Leave the cache'),
    ('firmware/canary/lib/securacv_network/src/securacv_network.cpp',
     "  // /api/mesh's peers_total, and no NVS is read here (F199: the persisted\n"
     '  // pubkeys needed flash encryption, so every board without it answered 500\n'
     '  // load_failed, which is every released board, since no tier turns flash\n'
     '  // encryption on; and an NVS write'),
    ('firmware/canary/src/main.cpp',
     '   * fails — on every Canary today, since no released image or\n'
     '   * provisioning step turns flash encryption on and save refuses\n'
     '   * without it by design.'),
    ('firmware/scripts/check_canary_mesh_status.py',
     '   (`mesh_state::load_trusted_peers`, refused without flash encryption,\n'
     '   which no Canary has on today, so every board answered 500\n'
     '   `load_failed`) and joined them'),
    ('scripts/tests/test_canary_mesh_status_wiring.py',
     "        # F199's review: the 500 every board without flash encryption (all\n"
     '        # of them today) answered, which F199 removed, back without a\n'
     '        # mesh_state call.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_provision.h',
     " * A bonded peer can write the WAP's Wi-Fi over BLE (CREDS). The iPhone\n"
     ' * app never reaches this service on a WAP (ledger A65): a phone sets a WAP\n'
     ' * up through the Improv setup door (ble_improv.h), and the SoftAP page\n'
     ' * stays the fallback.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_provision.h',
     ' * - Write rate-limited to 1 / 5 s and capped at 10 per hour, one budget\n'
     ' *   shared by every bonded peer. A refused write costs the writer nothing,\n'
     " *   so a hostile bonded peer can keep the owner's write refused\n"
     ' *   (SECURITY_MODEL.md, the WAP lockout paragraph; ledger F346).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_provision.h',
     '// 10 an hour leaves a fumbling user room to retype a password. The\n'
     '// cooldown and the cap are one budget for every bonded peer together, so a\n'
     "// bonded peer that keeps writing keeps the owner's write refused for the\n"
     '// hour (SECURITY_MODEL.md, ledger F346): they slow a guesser and do not\n'
     '// keep a host out, since any landing session can read the passkey and\n'
     '// bond a peer of its own.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_console.h',
     ' *   The bond is established via Numeric Comparison (MITM-safe after PR A),\n'
     " *   but a bond is not the bearer's trust: any landing session can bond a\n"
     ' *   peer of its own (it reads the passkey from GET /api/bluetooth and\n'
     ' *   confirms it, ledger F346), so the console exposes nothing a landing\n'
     ' *   session cannot already read over WiFi.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/household_api.h',
     ' * Every endpoint passes api_auth_check (same as the rest of the API),\n'
     ' * which takes the bearer or a dashboard session cookie, and the landing\n'
     ' * page mints that cookie for any host on the network (ledger F346).\n'
     ' * Mutations (POST role, override) are scoped to slots that are ALREADY in\n'
     " * the household store — the API can't create new paired devices, only\n"
     ' * adjust roles on devices that completed the BLE pairing ceremony, which a\n'
     ' * host on the network that is also in radio range can complete for a peer\n'
     " * of its own (SECURITY_MODEL.md's lockout paragraph). Default role for any\n"
     ' * new pairing is ROLE_GUEST.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.h',
     ' * from canary_wap.ino, every route behind the api_auth_check trampoline\n'
     ' * (bcn_auth_gated) the /api/mesh and /api/bluetooth routes use, which\n'
     ' * takes the bearer or a landing session any host on the network can mint\n'
     ' * (ledger F346).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/beacon_api.h',
     ' * Auth-gated from day one (see spec/beacon_channel_v0.md §10 and\n'
     ' * docs/audit/mesh_and_chirp_audit_v1.md C12 closure for Chirp). Every route\n'
     ' * is wrapped in the bcn_auth_gated trampoline, which runs api_auth_check:\n'
     ' * the bearer, or a landing-page session any host on the network can mint\n'
     ' * (ledger F346), gets a successful response. Mirrors the pattern\n'
     ' * established in bluetooth_api.h (#437) and household_api.h (#438).'),
    ('firmware/projects/canary-wap/tests_host/test_bluetooth_commands_wap.cpp',
     '// never saw the link or the bond. Now both go through the dispatcher: the\n'
     '// passkey reaches the channel and waits for a confirm over REST (any session\n'
     "// the landing page minted can send it, ledger F346; the dashboard's own is\n"
     "// the owner's), which answers it once, yes, on the loop task; the bond\n"
     '// reaches the paired list, and Opera still counts the link up and down.'),
    ('firmware/tests_host/test_provisioning_gate.cpp',
     '// pages\' "wait 10 seconds, then click Use BOOT Tap Once again" prescribes,\n'
     '// with the press after the device has answered the new ask with 403 (the\n'
     '// line says when).'),
    ('firmware/tests_host/test_improv_core.cpp',
     "  // The credentials were wiped at runtime (a WAP's Forget Wi-Fi, which any\n"
     '  // landing session can send, ledger F345, or the 10 s hold before its\n'
     '  // restart): the door reopens, first-come, for the window.'),
    ('firmware/tests_host/test_improv_core.cpp',
     '  // A tap reopens it here (on a board whose firmware reads one; the WAP has\n'
     '  // none). The credentials vanishing would reopen it too (the 10 s hold, or a\n'
     "  // WAP's Forget Wi-Fi), as the first-come NoCredentials door."),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_improv.cpp',
     '      // A door that reopens (the credentials were wiped at runtime, by\n'
     '      // Forget Wi-Fi or by the 10 s hold before its restart, re-arming the\n'
     '      // window) is a new provisioning: a claim from the last one is void.'),
    ('firmware/projects/canary-wap/tests_host/test_provisioning_logic.cpp',
     '  // Credentials wiped at runtime (Forget Wi-Fi, or the 10 s hold before its\n'
     '  // restart): the door opens, but a bonded phone is still connected. Held:\n'
     '  // the bonded session keeps the strict profile.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '      Serial.println("[BOOT] Released before 10 s: nothing reset (hold 10 s for a factory reset; Forget Wi-Fi on the dashboard clears only the saved Wi-Fi; on a unit that had Wi-Fi, either reopens the Bluetooth setup door for half an hour, to the first client that connects)");'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wap_server.h',
     'static const char* AP_SSID_PREFIX    = "SecuraCV-";      // unused: the SSID is SecuraCV-wap-<4 chars of the key fingerprint> (generate_ap_ssid), never the MAC, so a flasher install changes it'),
    ('firmware/projects/canary-wap/arduino/canary_wap/familiar.cpp',
     ' * it in; we hash it into Bloom filters with a device-specific salt, so one\n'
     " * device's filter bits mean nothing on another's. The salt is no defense\n"
     ' * against a flash dump: it sits in plaintext NVS (fm_salt) beside the\n'
     ' * persisted filters (fm_yest, fm_ignore), so a dump can test all 2,048\n'
     ' * fingerprints against them (household_presence_and_posture.md §4.1).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/familiar.cpp',
     '// Why salt? Per-device hashing: a filter means nothing without its own\n'
     "// device's salt. It does not stop a flash dump, which holds the salt too\n"
     '// (plaintext NVS on every tier today).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/familiar.h',
     ' *   • Hashes are salted with a per-device value kept in NVS. NVS is\n'
     ' *     plaintext on every tier today (key_at_rest.h), so a flash dump holds\n'
     ' *     the salt with the filters and can test every fingerprint; the salt\n'
     " *     only keeps one device's filter from meaning anything on another."),
    ('firmware/common/color/plumage.h',
     "// A device's personality, all 0..255. Derived from the device id, which is\n"
     '// kept across reboots and OTA updates, not always across a flasher install.'),
    ('src/bridge/hap/pairing.rs',
     '//! Canary the answer is "NVS, and only if flash encryption is on" while on a\n'
     '//! hub it is a file. That gate keeps a long-term pairing key off every board\n'
     '//! without flash encryption; it does not make the key confidential on one\n'
     '//! with it, since flash encryption does not cover NVS and no released build\n'
     '//! has NVS encryption (`docs/design/apple_home_integration.md`).'),
    ('src/bridge/hap/store.rs',
     '//! On a Canary the equivalent store is NVS, and the same rule applies one\n'
     '//! layer down: pairing keys get the `opera_secret` treatment, refused on a\n'
     '//! device without flash encryption, which keeps them off such a device but\n'
     '//! does not make them confidential on one with it (NVS is plaintext there\n'
     '//! too until NVS encryption is in the build).'),
    ('canary-local/assets/flash.js',
     '      "nothing. This app’s window has no Bluetooth of its own, so the check runs in " +\n'
     '      "the browser Lab in Chrome or Edge on this computer (the iPhone app does not " +\n'
     '      "read a WAP’s console today)."));'),
    ('canary-local/assets/flash.js',
     '      "On iPhone or iPad there is no way to run this check today: Safari has no " +\n'
     '      "Web Bluetooth, and the SecuraCV app does not read a WAP’s Bluetooth " +\n'
     '      "console. On a computer, use a Chromium browser (Chrome, Edge, Brave).");'),
    ('canary-local/assets/flash.js',
     '          "only. Pair this " +'),
    ('canary-local/assets/flash.js',
     '      "Counters and fingerprints only: this report shows no key, password or token. (The check reads the " +\n'
     '      "settings region off the board to find them, and the safety copy taken before an install saves the " +\n'
     '      "whole flash, key included, to your downloads.)"));'),
    ('canary-local/assets/flash-core.js',
     '    provisioned: !!get("privkey"),           // identity key exists (presence only; its bytes are never shown)'),
    ('canary-local/assets/flash-core.js',
     "// firmware/projects/canary-wap/arduino/canary_wap/ble_console.h (the iOS app's\n"
     '// BLEConsole.swift knows the same UUIDs, but never dials a WAP for them:\n'
     '// ledger A65).'),
    ('canary-local/assets/flash-core.js',
     '        why: `This board already runs ${cur.name}, so it is preselected. An install starts its ` +\n'
     '             `settings over (a new witness key, where it keeps one); an over-the-air update keeps them.` };'),
    ('canary-local/assets/flash-core.js',
     '    ? "Your settings are reset — the saved WiFi is cleared and, where the board keeps a witness key, it makes a new one, so it comes up with its setup network again, like the first day."'),
    ('desktop/src/app.js',
     '        why: `This board already runs ${cur.name}, so it is preselected. An install starts its settings over (a new witness key, where it keeps one); an over-the-air update keeps them.` };'),
    ('desktop/src/app.js',
     '  "nothing. This app’s window has no Bluetooth of its own, so the check runs in " +\n'
     '  "the browser Lab in Chrome or Edge on this computer (the iPhone app does not " +\n'
     '  "read a WAP’s console today).";'),
    ('desktop/src/app.js',
     '// stays the slug written during provisioning (kept across OTA updates and the\n'
     '// 10 s hold; an install in either flasher can change it); here it just becomes the'),
    ('desktop/src/index.html',
     '                reachable, and writes nothing. This app’s window has no Bluetooth of its\n'
     '                own, so the check runs in the browser Lab in Chrome or Edge on this computer\n'
     '                (the iPhone app does not read a WAP’s console today).</p>'),
    ('canary-local/tests/desktop_parity.test.js',
     '  assert.match(reassure, /if \\(nativeShell\\) \\{[\\s\\S]*?in Chrome or Edge on this computer[\\s\\S]*?\\} else \\{[\\s\\S]*?phaseBluetoothCheck/,\n'
     '    "the Lab app\'s Bluetooth entry must point to where the check runs, and only the browser opens it in-page");'),
    ('canary-local/tests/desktop_parity.test.js',
     '    assert.ok(flat.includes("the check runs in the browser Lab in Chrome or Edge on this computer"),\n'
     '      `${label} must name where the Bluetooth check runs`);\n'
     "    // The check reads a WAP's console, and the iPhone app dials only a\n"
     '    // peripheral that advertises the console service, which a WAP does not\n'
     '    // (ledger A65): naming the app as a place the check runs sends an iPhone\n'
     '    // owner into Bluetooth range for nothing.\n'
     '    assert.ok(!/runs in the SecuraCV iPhone app|runs in the iPhone app/.test(flat),\n'
     '      `${label} must not name the iPhone app as where the Bluetooth check runs`);'),
    ('desktop/flash-engine/src/changemap.rs',
     '            "Your settings are replaced with the ones you entered here — the board \\\n'
     '             comes up already on that network. Anything it had stored itself \\\n'
     '             starts fresh: its witness key, where it keeps one, its own tuning and \\\n'
     '             the witness chain, so it comes up under a new key."'),
    ('desktop/flash-engine/src/changemap.rs',
     '            "Your settings are reset — the saved Wi-Fi is cleared and, where the \\\n'
     '             board keeps a witness key, it makes a new one, so it comes up with its \\\n'
     '             setup network again, like the first day."'),
    ('canary-local/tools/gen_flash.py',
     '          "opens automatically.",'),
    ('canary-local/tools/gen_flash.py',
     '               "console at boot — join it and a setup page opens automatically.",'),
    ('canary-local/devices/flash.json',
     '      "provisioning_note": "Power it on near the SecuraCV iPhone app and tap the card that appears — it hands over your WiFi in one tap. No iPhone? It also raises a WiFi network of its own (SecuraCV-XXXX) with a key it prints on its serial console at boot — join it and a setup page opens automatically.",'),
    ('canary-local/devices/flash.json',
     '      "provisioning_note": "When it boots it raises a WiFi network of its own (SecuraCV-XXXX) with a key it prints on its serial console at boot — join it and a setup page opens automatically.",'),
    ('canary-local/assets/hatchery.js',
     "// hatches (first successful flash). The name is WHIMSY ONLY: the device's\n"
     '// functional id stays its provisioned slug (kept across OTA updates and the\n'
     '// 10 s hold; an install in either flasher can change it); this is a\n'
     '// display/certificate layer, the same one the native app mints.'),
    ('canary-local/devices/hatch.json',
     '  "$doc": "The Hatchery — the single source of truth for how a Canary is named and certified when it hatches. Shared verbatim by the website Lab (fetched from GitHub Pages) and the desktop flasher (embedded at build time), so the birthing moment is identical on both and stays in sync when this file changes. Pure data: the tiny assembly logic (pick a title with title_chance, first + house, Nth-of-its-name ordinal) is mirrored in both clients. Names are whimsy only — the device\'s functional id stays its provisioned slug (kept across OTA updates and the 10 s hold; an install in either flasher can change it); this is a display/certificate layer.",'),
    ('ios/Sources/SecuraCV/App/FleetStore.swift',
     "        // A console's tamper NOTIFY would be the fastest signal (sub-second,\n"
     "        // no polling in the path), but no WAP's console connects today: the\n"
     '        // app dials only a peripheral advertising the console service, which\n'
     '        // a WAP does not (ledger A65). Kept wired, so the path re-folds and\n'
     '        // re-evaluates at once when a console does connect.'),
    ('ios/Sources/SecuraCV/App/FleetStore.swift',
     '        // ── Tier 1a: a GATT console snapshot (none for a WAP today: the app\n'
     '        // dials only a peripheral advertising the console service, which a\n'
     '        // WAP does not, ledger A65) ──'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     '//   2. GATT CONSOLE. canary-wap serves a connectable console with a richer\n'
     '//      JSON snapshot, but we connect only when a peripheral advertises that\n'
     '//      service, and a WAP does not (the service is in its GATT table only),\n'
     '//      so this layer never fills for a WAP today (ledger A65).'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     '/// A compact console snapshot over BLE NOTIFY (a WAP serves this console\n'
     '/// but does not advertise it, so the app never dials a WAP for it, A65).'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     "    /// Would fire when a connected console reports tamper that wasn't there\n"
     "    /// a beat ago (NOTIFY is sub-second). No Canary's console connects today\n"
     '    /// (a WAP does not advertise it, ledger A65), so in practice this never\n'
     '    /// fires. Wired by FleetStore.'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     "            // A connected console's tamper NOTIFY goes to the alert loop at\n"
     "            // once. No Canary's console connects today (ledger A65), so the\n"
     '            // chirp and the 5-second sentinel carry tamper.'),
    ('ios/Sources/SecuraCV/Transport/FleetWiFiRunner.swift',
     '        for c in plan.unreachable {\n'
     '            steps[c.id] = .failed(FleetWiFiRollout.unreachableReason)\n'
     '        }'),
    ('ios/Sources/SecuraCV/Transport/FleetWiFiRunner.swift',
     '            steps[candidate.id] = .failed(FleetWiFiRollout.unreachableReason)'),
    ('ios/Tests/SecuraCVTests/FleetWiFiRolloutTests.swift',
     '    // Policy only: no WAP fills bleReachable today, since the app dials only\n'
     '    // a peripheral advertising the console service (ledger A65).\n'
     '    func testADarkCanaryOnTheBondedLaneRidesBLE() {'),
    ('ios/Tests/SecuraCVTests/FleetWiFiRolloutTests.swift',
     '    func testADarkCanaryOffTheBondedLaneIsUnreachable() {'),
    ('ios/Sources/SecuraCV/Model/CanaryVoiceStage.swift',
     '            line = "A Canary’s signing key is pinned the first time you meet it — from then on, a different key is flagged. Anyone who reads its flash can copy the key, so keep it out of reach."'),
    ('ios/Sources/SecuraCV/Model/Witness.swift',
     '    var id: String                       // device_id, e.g. "canary-a3f7" (kept across OTA updates and the 10 s hold; a flasher install can change it)'),
    ('ios/Support/Info.plist',
     "  <string>SecuraCV listens for your Canaries' Bluetooth beacons, and hands a brand-new Canary your Wi-Fi over its Bluetooth setup door when you set it up.</string>"),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     "            : 'Your session does not cover this, or has ended. Use BOOT Tap Once.');"),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     "From your recovery kit or the USB console. A signed-in page can't check it: use /settings in a browser with no session.</p>"),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     '      // than this budget; a press lands while the page alone asks.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/recovery_kit.test.js',
     '// It names no token box: on a page a session signed in, that box takes the\n'
     "// cookie's word for any token, so a mistyped one looks accepted.\n"
     "const NO_TOKEN_REFUSAL = 'Your session does not cover this, or has ended. Use BOOT Tap Once.';"),
    ('firmware/projects/canary-wap/arduino/canary_wap/recovery_kit.test.js',
     "  it('a press made before Save is served to the first ask only when no other address asks first (the residual, not a supported order)', async () => {"),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     ' * - iOS: no supported path. Safari and iOS Chrome omit Web Bluetooth, and\n'
     ' *   whether a WKWebView wrapper such as Bluefy lists or bonds a WAP is\n'
     ' *   unverified (ledger A65), so the page does not send iPhone owners into\n'
     ' *   Bluetooth range: it points them at the dashboard instead.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     '  Bluetooth here needs Chrome or Edge on Android or a computer.<br>\n'
     "  On an iPhone, use this Canary's dashboard instead."),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     "      return { show: true, text: 'This browser has no Web Bluetooth: the Bluetooth console needs Chrome or Edge on Android or a computer. On an iPhone, use the dashboard instead.' };"),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     "    showErr('Web Bluetooth is not supported by this browser. Use Chrome or Edge on Android or a computer; on an iPhone, use the dashboard instead.');"),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     '  // out nothing. The old order (press first, then Save) is served only if\n'
     '  // this page is the first asker after the press: a tap no page of the\n'
     "  // owner's was asking for goes to whichever address asks first, a poller\n"
     '  // included. The banner says'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wizard_logic.test.js',
     "  it('missing Web Bluetooth names a browser that has it, and sends no iPhone owner to Bluefy (ledger A65)', () => {\n"
     '    const n = L.capabilityNotice(false, true, false);\n'
     '    assert.equal(n.show, true);\n'
     '    assert.match(n.text, /Chrome or Edge on Android or a computer/);\n'
     '    assert.doesNotMatch(n.text, /Bluefy/);\n'
     '  });'),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     '// (1) insecure context → tell them to use HTTPS; (2) no Web Bluetooth\n'
     '// API → name the browsers that have it and send an iPhone owner to the\n'
     "// dashboard (no iPhone path reaches a WAP's console, ledger A65)."),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     '    // BLE-console chrome — hide the browser footer along with the banner.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  // Every endpoint behind the chirp_api template trampoline, api_auth_check:\n'
     '  // the bearer or a landing session any host on the network can mint (F346).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  // Auth-gated from day one (see spec/beacon_channel_v0.md §10): api_auth_check,\n'
     '  // the bearer or a landing session any host on the network can mint (F346).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/beacon_api.h',
     '// GET /api/beacon/audit — paged audit log (behind bcn_auth_gated).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.cpp',
     ' * (beacon_api.h, behind api_auth_check, which a landing session passes,\n'
     ' * from canary_wap.ino); the runtime loop is'),
    ('firmware/projects/canary-wap/arduino/canary_wap/chirp_api.h',
     '// Register all chirp API routes with the HTTP server, each behind\n'
     '// chirp_auth_gated (api_auth_check: the bearer, or a landing session any host\n'
     '// on the network can mint, ledger F346).'),
    ('docs/security/THREAT_MODEL.md',
     '  - REST API (`/api/chirp/*`) behind `api_auth_check()` when wired into\n'
     '    `canary_wap.ino` (audit C12): the bearer, or a landing session any host\n'
     '    on the network can mint (F346).'),
    ('docs/security/THREAT_MODEL.md',
     '  routes are registered (behind `FEATURE_BEACON_CHANNEL` and\n'
     '  `api_auth_check()`, which a landing session passes), but'),
    ('spec/chirp_channel_v0.md',
     "  (C11); REST API Bearer-gated *(amended 2026-10-22, #1843's sweep: behind\n"
     '  `api_auth_check()`, which also takes the landing session any host on the\n'
     '  network can mint, F346)* (C12); presence requirement on ACK origination'),
    ('CHANGELOG.md',
     '  app descriptor already on the board ("This board already runs X —\n'
     '  installing keeps it, updated in place" *(amended 2026-10-22, #1843\'s sweep:\n'
     '  an install starts the settings over under a new key, and the card now says\n'
     '  so)*); else the chip **plus the'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "  table; on a dev board *(amended 2026-10-22, #1843's sweep: on every\n"
     '  released board, since no tier turns flash encryption on)* without flash\n'
     '  encryption every peers read fails, and'),
    # #1843's sweep, sixth pass, round-1 review: the copies the pass
    # missed and the replacements it wrote that were themselves untrue
    # (the tap, a cleared unit's owner, the WAP's gate, the old card, the
    # released framework, the console's board, a restore's token).
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '// The BOOT tap stays open this long for one asker:'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '      Serial.printf("[AUTH] Provisioning gate OPENED for %lu seconds: the receipt goes to one asker; with no page asking, the first address to ask gets it (ask first, then tap)\\n",'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  boot_line("    Press BOOT (<2s)   = receipt to one asker: ask first, or the first to ask gets it (30s); hold 2-3s = print identity");'),
    ('canary-local/tools/gen_wap.py',
     '    "    Press BOOT (<2s)   = receipt to one asker: ask first, or the first to ask gets it (30s); hold 2-3s = print identity",'),
    ('canary-local/tools/gen_wap.py',
     '             f"{GATE_TTL_MS // 1000} s and hands the recovery kit to one asking page "'),
    ('canary-local/tools/gen_wap.py',
     'must_words(DOC, "On a WAP it hands the recovery kit to one asking address: click **Use BOOT Tap Once** on "'),
    ('canary-local/devices/wap.json',
     '      "    Press BOOT (<2s)   = receipt to one asker: ask first, or the first to ask gets it (30s); hold 2-3s = print identity",'),
    ('canary-local/devices/wap.json',
     '          "short press (under 2 s): opens the provisioning gate for 30 s and hands the recovery kit to one asking page (click Use BOOT Tap Once on the dashboard first, then press; a tap no page was asking for goes to the first address that asks after it; it prints no key)",'),
    ('firmware/projects/canary-wap/README.md',
     'it, and a tap no page was asking for goes to the first address that asks'),
    ('docs/getting_started_canary.md',
     'dashboard within the 30 seconds. On a WAP it hands the recovery kit to one'),
    ('docs/design/magic_pairing.md',
     "sketch's own gestures. A short press hands the receipt to one asker for 30 s"),
    ('canary-local/assets/wap-ui.js',
     '        : "app running — the ROM ignores BOOT until the next reset, but the firmware reads it (a tap hands the receipt to one asking page, so the page asks first; 10 s held is a factory reset)";'),
    ('canary-local/assets/wap-ui.js',
     '    if (held && plugged && mode === "run") say("(BOOT held — the ROM only samples it at reset, but the running WAP counts it: kept 10 s, it clears Wi-Fi, broker and name, keeps its key and token, and reopens its first-come Bluetooth setup door for half an hour, unless a landing session turned its Bluetooth off, which the hold keeps)");'),
    ('firmware/projects/canary-wap/arduino/canary_wap/recovery_kit.test.js',
     '    // no page was asking for goes to the first asker after it), and the'),
    ('CHANGELOG.md',
     '  press hands it to one asking page, so click **Use BOOT Tap Once** first'),
    ('firmware/common/network/improv_core.h',
     " *   network was cleared), and for an owner's own tap — never for a device"),
    ('firmware/common/network/improv_core.h',
     ' *   - NO CREDENTIALS STORED (first boot, or after they were cleared: the'),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     " *   network was cleared), and for an owner's own tap — never for a device"),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     ' *   - NO CREDENTIALS STORED (first boot, or after they were cleared: the'),
    ('docs/design/magic_pairing.md',
     "> yet, or one whose network was cleared), and for an owner's own tap — never"),
    ('docs/design/magic_pairing.md',
     'A cleared unit (the 10 s hold, or Forget Wi-Fi on a WAP, which any landing'),
    ('docs/design/magic_pairing.md',
     'dashboard (`POST /api/wifi/forget`, which any landing session can send,\n'
     "ledger F345, so it is not the owner's act alone) and the 10 s hold re-arm"),
    ('docs/design/magic_pairing.md',
     'While the door is **open** (a new unit, one cleared with the hold or with'),
    ('docs/design/magic_pairing.md',
     'one whose network was cleared, §2.2), the second is a device that has an owner and a'),
    ('docs/ble_protocol.md',
     "From `improv_core.h`: the door is open for a device with NO NETWORK SAVED (a new one nobody owns yet, or one whose network was cleared), and for an owner's own tap — never for a device that has an owner and a bad day."),
    ('docs/ble_protocol.md',
     "- **No credentials stored** (first boot, or after they were cleared with the 10 s factory-reset hold or, on a WAP, Forget Wi-Fi, which any landing session can send, ledger F345): open for a first-boot window of **30 minutes** after power-on (`IMPROV_FIRST_BOOT_WINDOW_MS`; a power cycle re-arms it, and so does clearing the credentials; compile-time 0 = tap-only). A cleared unit still has an owner: the hold keeps the witness identity on a Sense, a Vision and a WAP, and a WAP's hold and Forget Wi-Fi keep its token too. The door still goes to whichever client connects first, so for that half hour a stranger in radio range can give it their network ([`design/magic_pairing.md`](design/magic_pairing.md) §2.2; on a WAP, ledger F345)."),
    ('docs/GLOSSARY.md',
     'device with no network saved (a new one nobody owns yet, or one whose'),
    ('docs/GLOSSARY.md',
     're-arms it, and so does clearing the credentials with the 10 s'),
    ('docs/FAQ.md',
     'device with no network saved (a new one nobody owns yet, or one whose'),
    ('docs/README.md',
     "- [**Magic pairing — the Bluetooth setup door (design)**](design/magic_pairing.md) — built, host-tested, **not bench-tested**: a new Sense, Vision or WAP takes its Wi-Fi from the iPhone app with one tap over the open Improv Wi-Fi standard. The door rule (open for a device with no network saved — a new one nobody owns yet, or one whose network was cleared, which still has an owner — for 30 minutes after power-on, to whichever client connects first, or for 60 s after the owner's own BOOT tap; never because a saved network is failing), the bounds a stranger in radio range runs into, the WAP's claim ticket that gets the phone paired without the bearer token ever riding Bluetooth, the UUIDs and byte layouts, the per-family lifecycle, the iOS flow, the residual risks and the bench checklist"),
    ('docs/TRUST_MODEL.md',
     'session can send, ledger F345) and in the BOOT-tap minute. A WAP also takes it over'),
    ('firmware/LESSONS_LEARNED.md',
     '  new one nobody owns yet, or one whose network was cleared), and for an'),
    ('firmware/projects/canary-sense/README.md',
     'saved — a new one nobody owns yet, or one whose network was cleared — and'),
    ('firmware/projects/canary-vision/README.md',
     'network saved (a new one nobody owns yet, or one whose network was'),
    ('docs/firmware_ota.md',
     'All endpoints are bearer-token gated on canary. On canary-wap they take'),
    ('firmware/common/network/node_status.h',
     ' * two trees served a rich, gated status and four served none.'),
    ('firmware/common/network/node_status.h',
     " * canary and the WAP, whose /api/status is gated and richer (the canary's"),
    ('firmware/projects/canary-wap/arduino/canary_wap/node_status.h',
     ' * two trees served a rich, gated status and four served none.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/node_status.h',
     " * canary and the WAP, whose /api/status is gated and richer (the canary's"),
    ('firmware/projects/canary-display/arduino/canary_display/node_status.h',
     ' * two trees served a rich, gated status and four served none.'),
    ('firmware/projects/canary-display/arduino/canary_display/node_status.h',
     " * canary and the WAP, whose /api/status is gated and richer (the canary's"),
    ('docs/PARITY.md',
     '| **canary-wap** | ✅ | ✅ gated ¹⁰ | ⚠️ ¹¹ | ⚠️ ¹² | ❌ ¹³ | ❌ ¹³ | ✅ | ✅ | ✅ | ✅ |'),
    ('docs/PARITY.md',
     '    behind its bearer token, and the WAP behind `api_auth_check()`, which'),
    ('docs/PARITY.md',
     '  rides inside the gated status on the two that already had one. The'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  // Rename this device → canary-<name>.local (api_auth_check(): the bearer,'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  // Authenticated API endpoints (api_auth_check(): a landing session or the'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '// These wrappers put api_auth_check() in front of existing handlers: the'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '// (secureFetch sends a bearer only when the page holds a token), so'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     ' * mDNS browser for the operator UI behind api_auth_check() (the bearer, or a'),
    ('firmware/projects/canary-wap/arduino/canary_wap/airtime_governor.cpp',
     '//     `cosign_pending_request` REST path (behind api_auth_check(): the'),
    ('firmware/projects/canary-wap/arduino/canary_wap/chirp_api.h',
     ' *     handler is wrapped in chirp_auth_gated<>, which runs api_auth_check():'),
    ('firmware/projects/canary-wap/arduino/canary_wap/household_api.h',
     '// The dashboard reaches these with its session cookie (secureFetch sends'),
    ('firmware/projects/canary-wap/arduino/canary_wap/api_auth.h',
     " * Provides the REST API's authentication with (api_auth_check() takes the"),
    ('firmware/projects/canary-wap/arduino/canary_wap/wap_server.h',
     ' * - api_auth_check() on protected endpoints: the session cookie the landing'),
    ('firmware/projects/canary-wap/arduino/canary_wap/csi_integration.cpp',
     '#include "api_auth.h"             // api_auth_check() — session cookie or bearer token (F346)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wap_diagnostics.h',
     ' * The route (canary_wap.ino handle_diagnostics, behind api_auth_check():'),
    ('firmware/projects/canary-wap/tests_host/check_route_security.py',
     '    # (that stays behind the api_auth_check()-gated /api/fleet-scan, which a'),
    ('docs/FIRMWARE_MAP.md',
     '  on canary (the bearer) and WAP (`api_auth_check()`: the bearer, or a landing'),
    ('docs/homeassistant_setup.md',
     'return `csi_event_egress` from their gated `GET /api/diagnostics` (the'),
    ('docs/manual_test_plan_captive_portal.md',
     "their gated `/api/status` under `node_status`: the canary's takes the bearer,"),
    ('docs/canary_settings_access_validation.md',
     '  until it signs in. That is not a boundary against a host on the WiFi: the'),
    ('spec/witness_api_v1.md',
     '  behind the same gate (on canary-wap, `api_auth_check()`: the Bearer token'),
    ('spec/witness_api_v1.md',
     'caller the gate admits (on canary-wap, the Bearer token or a landing session'),
    ('tvos/discovery/DISCOVERY.md',
     '  sealed evidence stays behind the gated `/api/fleet-scan` (on a WAP,'),
    ('ios/Sources/SecuraCV/Transport/DeviceAPI.swift',
     "    /// `GET /api/status` — the WAP's gated status body (the app sends the"),
    ('ios/Sources/SecuraCV/Transport/DeviceAPI.swift',
     '    /// first, then committed ring rows newest-first, ≤72 rows, gated like'),
    ('ios/Sources/SecuraCV/Transport/DeviceAPI.swift',
     '    /// SSE; gated like `/api/status`; Wire/WapEvents.swift documents its'),
    ('ios/Sources/SecuraCV/Views/Components/SensingNowSection.swift',
     '    /// (the app reaches the stream only with the bearer — it never holds'),
    ('ios/Sources/SecuraCV/Views/DeviceDetailView.swift',
     '            // (the app reaches the stream only with the bearer, never a'),
    ('ios/Sources/SecuraCV/Wire/WapEvents.swift',
     '// 64 rows, behind CSI_AUTH_OR_RETURN (csi_integration.cpp:handle_events_today:'),
    ('ios/Sources/SecuraCV/Wire/WapEvents.swift',
     '/// `GET /api/csi/stream` — a plain 1 Hz GET snapshot (not SSE), gated like'),
    ('ios/Tests/SecuraCVTests/WapEventsTests.swift',
     '        // open the sensing feed with no sign-in at all — a regression the'),
    ('ios/Tests/SecuraCVTests/WapEventsTests.swift',
     '                      "events/today lost its auth gate — the feed must stay gated")'),
    ('CHANGELOG.md',
     "  (`witness_page.h`, `handle_witness_v1`, Bearer-gated *(amended 2026-10-26, #1843's sweep: behind `api_auth_check()`, which also takes the landing session any host on the network can mint, F346)*, chunked) as"),
    ('CHANGELOG.md',
     "- **Beacon REST API** (`beacon_api.h`): Bearer-token-gated surface *(amended 2026-10-26, #1843's sweep: behind `api_auth_check()`, which also takes the landing session any host on the network can mint, F346)* —"),
    ('CHANGELOG.md',
     "  template-trampoline pattern *(amended 2026-10-26, #1843's sweep: behind `api_auth_check()`, which also takes the landing session any host on the network can mint, F346)*; wired into `canary_wap.ino`."),
    ('docs/audit/mesh_and_chirp_audit_v1.md',
     "**Fix in this branch:** register `chirp_api::register_routes()` in `canary_wap.ino` using the same Bearer-token template trampoline as `bluetooth_api.h` (PR #437) and `household_api.h` (PR #438) *(amended 2026-10-26, #1843's sweep: behind `api_auth_check()`, which also takes the landing session any host on the network can mint, F346)*. `POST /api/chirp/send` additionally requires a 2-second hold timestamp from the UI per `spec/chirp_channel_v0.md` §4.1."),
    ('docs/audit/v0.3_closeout.md',
     "| C12 | High (latent) | Bearer-token template trampoline; wired in INO *(amended 2026-10-26, #1843's sweep: behind `api_auth_check()`, which also takes the landing session any host on the network can mint, F346)* | `chirp_api.h::chirp_auth_gated<>` | manual: cURL with/without auth |"),
    ('docs/audit/v0.3_closeout.md',
     "- **Bearer-token-gated REST surface** *(amended 2026-10-26, #1843's sweep: behind `api_auth_check()`, which also takes the landing session any host on the network can mint, F346)* on `/api/beacon/*`, wired into"),
    ('docs/design/iphone_companion_app.md',
     "> Bluetooth, so the device's own `/companion` PWA sends an iPhone owner to the"),
    ('docs/design/usb_onboard.md',
     '  or put in a fresh card. Left in, that card takes no evidence while its old'),
    ('firmware/canary/lib/securacv_usb_onboard/src/securacv_usb_onboard.cpp',
     '  Serial.println("     new key, which never adopts the old card: copy records.jsonl");'),
    ('firmware/canary/lib/securacv_witness/src/securacv_witness.cpp',
     '// history is ahead of ours is refused for the same reason, but only while'),
    ('firmware/canary/lib/securacv_witness/src/securacv_witness.cpp',
     '    return sd_append_fail("SD log ahead of chain: reboot or use new card");'),
    ('firmware/PARTITIONS.md',
     '  build has it (every released build is `framework = arduino`; only the unreleased canary-ota'),
    ('firmware/scripts/regression_check.sh',
     '# no released Canary build has (every released build is framework = arduino, so'),
    ('firmware/scripts/regression_check.sh',
     '  blue "  Fix: drop the flag; only NVS encryption (nvs_keys) could protect NVS, and it is unavailable under framework = arduino, which every released build uses, so NVS stays plaintext on those either way"'),
    ('firmware/provisioning/partitions_secure.csv',
     '#   Canary build reads it (every released build is framework = arduino,'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     ' * read learns nothing of it; on a board with FE on it is still'),
    ('desktop/src/app.js',
     '// The read-only Bluetooth console runs on the WAP alone (ble_console.h is the'),
    ('canary-local/assets/flash.js',
     '  // The WAP runs the read-only Bluetooth console (ble_console.h is the only'),
    ('docs/browser_flasher.md',
     'is a cleaner over-the-air "it\'s a *branded* Canary" signal, but on a WAP only:'),
    ('docs/browser_flasher.md',
     'after flashing a WAP, the one board that serves the console) answers three plain questions with **Web'),
    ('firmware/projects/canary-wap/README.md',
     "- **Session Rotation:** the RF presence detector's ephemeral session tokens rotate every 4 hours. The API token does not rotate: the 10 s hold and Forget Wi-Fi keep it, an install or a full erase in a flasher replaces it, and a flasher's restore of a safety copy puts back the token that copy holds"),
    ('docs/design/magic_pairing.md',
     'safety copy over it, which puts back the token (and the key) that copy holds:'),
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
    "takes the device to a bench: flash encryption is an opt-in tier, so on a\n"
    "default Canary the key is recoverable by reading the flash directly. What",
    "- Flash encryption (data at rest protected) — opt-in secure-provisioning tier only",
    "**The honest statement: on a Canary in its default configuration, someone\n"
    "who takes the device away and opens it up can read the private key out of\n"
    "the flash chip.**",
    "SD card and flash storage. On a device in its default configuration this\n"
    "includes recovering the device's private key; the section below states",
    "The hold helps only with a setup onto a network that host is not on, which\n"
    "the owner's browser does without it, and without reopening the first-boot",
    "not, so it never writes a WAP's CREDS (ledger A65, open). Once A65 is\n"
    "settled the lane would move the WAP with no browser, but only while that\n"
    "host leaves it alone, and taking it away needs no radio. Any landing",
    "different adversary, and on a default-configured Canary they can recover\n"
    "the key. That is a deliberate trade — the alternative permanently bricks",
    "**If your threat model includes device seizure**, this is the case the\n"
    "opt-in tiers exist for. Turn them on deliberately, back up the key first,",
    "opt-in tiers exist for. Turn them on deliberately, back up the key first,\n"
    "and understand that you are trading recoverability for extraction\n"
    "resistance. Read [`../design/hardware_root_of_trust.md`]",
    "address the token, on the receipt or on the page load it unlocks, ends that\n"
    "address's backoff as a right bearer would (sweep F341). The dashboard",
    "that serves ends that address's backoff, as a right bearer would (sweep",
    "- **Secure tier: PARTIAL PASS.** Secure boot prevents firmware replacement;",
    "- Secure boot (firmware integrity verified) — opt-in secure-provisioning tier only",
    "| Firmware corrupt | Refuse to boot **on the opt-in secure-provisioning tier only** (secure boot is NOT",
    "| `platformio_secure.ini` | ✅ | Secure Boot v2 + Flash Encryption env; compile-only in CI (F42), no bench pass |",
    "| Factory reset via BOOT button | ✅ (short/long press) | ❌ | ✅ (5s hold) | ❌ |",
    # #1843's sweep, third pass (its review round 2):
    'mitigations, but they are **off on every Canary today**: the provisioning',
    'reversible protections in the default path and leaves the irreversible\n'
    'lockdown as an explicit, key-backup-enforced opt-in. The settings are',
    'so neither flash encryption nor Secure Boot is on after it (what the chip',
    "| Firmware corrupt | Every tier's bootloader checks an app image's checksum and appended SHA-256 before it boots it (neither pinned core's precompiled bootloader sets `CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON`; it does skip the check on a wake from deep sleep), and on a failed check it tries the other app slot instead (ESP-IDF's `bootloader_utility_load_boot_image`; read from the cores' `sdkconfig` and IDF source, not run on a device). What no tier refuses today is a well-formed image that is unsigned or forged: that is Secure Boot's job, which the provisioning kit prepares but does not enable (it is NOT on default builds — see [Scope of this principle](#scope-of-this-principle)), so every tier relies on the OTA path's SHA-256 and Ed25519 release-signature check, which runs on the image written to the spare slot before the device switches its boot slot to it (`firmware/common/ota`) |",
    '  is readable on a bench, a full erase in either flasher is what removes',
    '  bytes stay in flash the same way. No hold sheds its key (a clean install,\n'
    '  a full erase in either flasher, is its way to a new identity). The',
    '| NVS corruption | A key NVS does not hold (never stored, or wiped) is a fresh start: a new identity. On the flagship (`firmware/canary`) and the canary-wap, a key IDF reports as stored that cannot be read, is not 32 bytes or is 32 zero bytes, or an NVS that cannot say whether one is stored, halts the boot instead: no key is generated, and nothing is written to the identity key or the chain state (sweep F360). A fault in the read that leaves the entry in place (a session that will not open, a flash read error while IDF copies the data) leaves the key for a later boot that can read it. Corruption is IDF\'s to meet first, and the firmware sees only its answer (read from IDF v4.4.7, v5.5.1 and v5.5.4\'s source). An entry whose data checksum fails IDF erases in the read it refuses, so that boot halts: on IDF 4.4 (every flagship env but `env:full`) the blob\'s index survives and each later boot halts until the 30 s BOOT hold at power-on wipes the identity; on IDF 5.5 (`env:full` and the canary-wap) the next boot\'s NVS init erases the orphaned index, so that boot finds no key and makes a new identity. Some faults make a new identity with no halt, because IDF answers "not found": a data chunk whose header fails IDF\'s check, on IDF 5.5 (on 4.4 it halts every boot, as above); an index entry whose header fails it, which IDF erases before the key is read (when it loads the page, or in the probe\'s own lookup); and a flash read error in IDF\'s own lookup of the key, which sets that page aside for the boot and leaves the old entry on it. All of it is today\'s behavior, not a decision; what holds throughout is that the firmware never writes over a key IDF still reports as stored. The canary-wap\'s BOOT hold keeps the identity (it has no wipe stage), so a clean install (full erase) in either flasher is its way to a new identity on purpose, halted or not. Before F360 a read that failed after the length check, before IDF copied anything into the buffer, ran the boot on the zero key, whose setup-network key is the published `cv-3jByJ6rHdhM2` (on a release canary-wap, the SoftAP came up under it and records were signed with it), and a failed open or length lookup replaced the stored identity. Host-tested; not run on a device. canary-sense, canary-vision and canary-sentinel never sign with the zero key, but still replace a stored key they fail to read with a new one (Home Assistant shows a changed fingerprint; sweep F362) |',
    '  NVS the same way, so a seizer of a fused board who reads its flash after\n'
    '  the hold has the mesh secret.',
    '  and log loudly (v0.2 audit O2). That keeps the secret off un-fused\n'
    '  boards; it does **not** make it confidential at rest on fused ones.',
    '| Physical extraction of opera_secret | partial (the FE gate keeps it off un-fused boards; NVS is not encrypted, so a fused board still holds it in plaintext) | n/a | n/a |',
    '  `beacon_channel.cpp` uses the same gate as O2; an un-fused board keeps\n'
    '  the ring in RAM only), and it is plaintext NVS there too: flash',
]
# And the RETIRED_IN sentences as they stood, wrapped: (file, the sentence as
# it stood there). Each form must trip a row of ITS OWN file, and each row
# must have a form in its own file, so a row deleted while a sibling row in
# another file shares its needle, or a row pointed at another file, leaves a
# form with no row behind it and fails, instead of passing on the sibling.
MUST_FAIL_IN = [
    ('firmware/FEATURES.md',
     '| `provision_canary.sh` | ✅ | Full workflow with --dry-run |'),
    ('docs/security/THREAT_MODEL.md',
     '- **Default tier: FAIL.** Secure boot and flash encryption are an opt-in\n'
     '  secure-provisioning build (`firmware/provisioning/sdkconfig.defaults.secure`'),
    ('docs/security/THREAT_MODEL.md',
     '**PARTIAL PASS:** Documented limitation. Future mitigations: application-layer\n'
     'SD card encryption, RISC-V open silicon.'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "  `config_clear` inside the boot's read). With the busy word never spoken,\n"
     '  the first fails at the host write with the stored password on a client for\n'
     '  `10.0.0.66`. The review also found `port_or_default()` unreachable with a 0'),
    ('firmware/LESSONS_LEARNED.md',
     '  hooks; with the busy word never spoken the first fails at the host write\n'
     '  with the stored password on a client for the new host. Rule: when a'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '  broker with the stored password and no password typed — but its grant is\n'
     "  not the session's: a scan window opens from the boot auto-scan or from\n"
     "  `POST /api/wifi/qr-scan`, which takes the wizard's one-shot pair token\n"
     '  (`pair_token_valid()`), never the cookie, and a code must be in front of'),
    ('firmware/LESSONS_LEARNED.md',
     "  stored password and no password typed. Its grant is not the session's:\n"
     '  the scan window opens from the boot auto-scan or from\n'
     "  `POST /api/wifi/qr-scan` behind the wizard's one-shot pair token\n"
     '  (`pair_token_valid()`), and a code must be in front of the camera'),
    ('CHANGELOG.md',
     '  still re-points the broker with the stored password — behind the pair\n'
     '  token and a code in front of the camera, not the session; named in the\n'
     '  ledger.'),
    ('spec/beacon_channel_v0.md',
     "| One member's stolen device key forging solo frames | The solo path's BOOT-button and no-fresh-cosigner gates run on the originating device and nothing about them is on the wire (§6.2 security note). A holder of one member's Ed25519 private key — readable from flash on a device without flash encryption — can sign a solo ALERT"),
    ('CHANGELOG.md',
     "  app never dials a WAP's lane (ledger A65, open). Once that is settled the\n"
     '  lane holds only while that host leaves it alone, and taking it away needs\n'
     "  no radio: any landing session can turn the WAP's Bluetooth off, a saved"),
    ('docs/audit/esp32s3_firmware_full_audit_2026-07.md',
     "  app never dials a WAP's lane (ledger A65, open). Once that is settled the\n"
     '  lane holds only while that host leaves it alone, and taking it away needs\n'
     "  no radio: any landing session can turn the WAP's Bluetooth off, a saved"),
    ('docs/security/SECURITY-AUDIT.md',
     "  app never dials a WAP's lane (ledger A65, open). Once that is settled the\n"
     '  lane holds only while that host leaves it alone, and taking it away needs\n'
     "  no radio: any landing session can turn the WAP's Bluetooth off, a saved"),
    ('firmware/LESSONS_LEARNED.md',
     '  browser gives with no hold (its landing session reaches\n'
     '  `POST /api/wifi/connect` under the lockout), while that host leaves it\n'
     '  alone (the next three bullets). Run the attack again from the state'),
    ('docs/design/hardware_root_of_trust.md',
     '- **Nation-state physical security.** Flash encryption defends key-at-rest and\n'
     '  casual readout, not decapping/fault-injection by a well-funded lab. We say so.'),
    ('docs/design/hardware_root_of_trust.md',
     '   full Tier 4.** Dev mode is the sweet spot: it protects WiFi/keys at rest from a\n'
     '   thief while the device stays field-recoverable. Release mode (which disables'),
    ('docs/design/vault_operator_ux_v1_1.md',
     '- **Secure it (protect the key at rest) — already tooled, not yet burned.**\n'
     "  `docs/secure_provisioning.md` Phase 2 wraps Espressif's guide: **eFuse +"),
    ('docs/design/vault_operator_ux_v1_1.md',
     '  Secure Boot v2 + JTAG-off. Keeps the entire existing signing scheme; the key\n'
     "  can't be dumped off a locked device. Largely already tooled in"),
    ('docs/V1_BENCH_TEST_RUNBOOK.md',
     '| Boards with **flash encryption** enabled | ≥ 2 | Required for mesh (the `opera_secret` in NVS is only confidential under FE) |'),
    ('canary-local/assets/flash.js',
     '    "encryption and eFuse locks (then the flash reads back as ciphertext), " +'),
    ('canary-local/assets/flash.js',
     '    "For a board deployed somewhere physically exposed, use Phase-2 secure provisioning (flash encryption + eFuse).",'),
    ('docs/design/magic_pairing.md',
     '  forever; a join that succeeds stores credentials and shuts the door; an\n'
     '  owner who finds the unit on the wrong network holds it, factory-resets it,\n'
     '  and starts again.'),
    ('docs/design/magic_pairing.md',
     '  owner, and a unit joined to the wrong network is a unit its owner holds,\n'
     '  not one that has been taken; the fix is a factory reset. A deployment'),
    ('docs/FAQ.md',
     '`/api/wifi/forget` route), which clears the stored network and opens the\n'
     "door again for half an hour from that moment. Holding a WAP's BOOT button"),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  //    and the address they serve has its backoff cleared (sweep F341,'),
    ('CHANGELOG.md',
     '  section above: a tap clears an address only when it is served to that\n'
     "  address's ask; under that host the hold is no way out, the iPhone app"),
    ('firmware/canary/CONSOLIDATION.md',
     "token, on the receipt or on the page load, ends that address's backoff through `auth_handed_token()`, as the WAP's tap and"),
    ('firmware/canary/lib/securacv_network/src/securacv_network.cpp',
     "// token ends its address's backoff (auth_handed_token, sweep F341), so a"),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '  it stops, or hold BOOT 10 s and set the unit up on a network that host\n'
     "  is not on. `test_auth_backoff.cpp`'s overflow row starts from the empty"),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "  *Amended once more 2026-10-21 (#1843's first review):* the bonded lane\n"
     '  gets the owner out only while that host holds no bond of its own. Any\n'
     '  landing session can pair a Bluetooth peer: it can turn pairing on'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "  the way out F343's posture names from under a host striking from nine or"),
    ('docs/getting_started_canary.md',
     "You can't brick the device through the dashboard. You *can* lose your\n"
     'witness chain by factory-resetting it (the Ed25519 device key is'),
    ('docs/getting_started_canary.md',
     'for evidence, **export the chain before you factory-reset** —'),
    ('docs/getting_started_canary.md',
     'factory reset makes a new certificate, so pair the iPhone app again'),
    ('firmware/canary/include/canary_config.h',
     '// generated once on the first TLS-capable boot (factory reset erases both,'),
    ('firmware/canary/scripts/ota_deploy.py',
     '        log_error("Refusing to send the token. A factory reset makes a new certificate;")'),
    ('firmware/canary/scripts/ota_deploy.sh',
     '        log_error("Refusing to send the token. A factory reset makes a new certificate;")'),
    ('firmware/canary/scripts/ota_deploy.py',
     '    log_error "Refusing to send the token. A factory reset makes a new certificate;"'),
    ('firmware/canary/scripts/ota_deploy.sh',
     '    log_error "Refusing to send the token. A factory reset makes a new certificate;"'),
    ('firmware/canary/src/main.cpp',
     '  Serial.println("║  BOOT: tap=provisioning gate, 2s=info, 5s=factory reset      ║");'),
    ('ios/Sources/SecuraCV/Transport/DeviceAPI.swift',
     '                + "place — check the device itself. If you reset or replaced it, re-pair it "'),
    ('ios/Sources/SecuraCV/Security/DeviceStore.swift',
     '        // sight then (TOFU) — a factory-reset device with a new key must not'),
    ('ios/Sources/SecuraCV/Alerts/HeadWatch.swift',
     '//     means the chain restarted (a wipe, a reset) — exactly the kind of'),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     '// service and the BOOT-button portal can still rescue), never the fleet.'),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     '        + "(hold BOOT) will take the correction."'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     '            resolveProvision(.failed("This Canary\'s firmware doesn\'t offer the Bluetooth rescue — use its setup portal (hold BOOT) instead."))'),
    ('custom_components/securacv/__init__.py',
     '        # identity for this device_id (fresh flash, factory reset, a'),
    ('custom_components/securacv/__init__.py',
     '        # A newly pinned device starts its counters over (fresh flash, factory'),
    ('custom_components/securacv/sensor.py',
     '    device is (re)pinned via TOFU (async_record_verify), which is what a\n'
     '    factory reset looks like from here.'),
    ('custom_components/securacv/tests/test_replay_reset_paths.py',
     'to the KEY: a factory-reset or replaced Canary starts them over. So every'),
    ('custom_components/securacv/tests/test_replay_and_device_id.py',
     '    # Factory reset: the device pins fresh and its chain starts over.'),
    ('firmware/LESSONS_LEARNED.md',
     '// polling a reset unit with its old token. A Table keeps one Slot per source'),
    ('firmware/common/network/auth_backoff.h',
     '// polling a reset unit with its old token. A Table keeps one Slot per source'),
    ('firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h',
     '// polling a reset unit with its old token. A Table keeps one Slot per source'),
    ('firmware/LESSONS_LEARNED.md',
     "  and the likeliest such host was the owner's own phone, polling a reset unit\n"
     '  with its old token. Because a `static` in a header is a separate object in'),
    ('firmware/common/network/auth_backoff.h',
     "  and the likeliest such host was the owner's own phone, polling a reset unit\n"
     '  with its old token. Because a `static` in a header is a separate object in'),
    ('firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h',
     "  and the likeliest such host was the owner's own phone, polling a reset unit\n"
     '  with its old token. Because a `static` in a header is a separate object in'),
    ('CHANGELOG.md',
     '  still polling a reset unit with its old token (filed as F352). The backoff'),
    ('firmware/LESSONS_LEARNED.md',
     '  still polling a reset unit with its old token (filed as F352). The backoff'),
    ('firmware/common/network/auth_backoff.h',
     '  still polling a reset unit with its old token (filed as F352). The backoff'),
    ('firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h',
     '  still polling a reset unit with its old token (filed as F352). The backoff'),
    ('docs/design/magic_pairing.md',
     "visiting browser's own address. A phone that still holds a pairing for a\n"
     'unit that was reset keeps polling with the old token, and so holds its own'),
    ('docs/design/magic_pairing.md',
     '| No credentials stored (first boot, after a factory reset), within **30 minutes** of power-on | **Open** (`Door::NoCredentials`) | A device nobody owns yet. The window is `IMPROV_FIRST_BOOT_WINDOW_MS`'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "  striker was the owner's own phone, still polling a reset unit with its old\n"
     '  token.\n'
     "  What it is now. The backoff is keyed by the request's socket peer address,"),
    ('firmware/LESSONS_LEARNED.md',
     "  striker was the owner's own phone, still polling a reset unit with its old\n"
     '  token.\n'
     "  What it is now. The backoff is keyed by the request's socket peer address,"),
    ('firmware/common/network/auth_backoff.h',
     "  striker was the owner's own phone, still polling a reset unit with its old\n"
     '  token.\n'
     "  What it is now. The backoff is keyed by the request's socket peer address,"),
    ('firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h',
     "  striker was the owner's own phone, still polling a reset unit with its old\n"
     '  token.\n'
     "  What it is now. The backoff is keyed by the request's socket peer address,"),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "  (iOS, a reset unit's stale pairing)."),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "  old token.** A factory reset that regenerates the WAP's key changes both\n"
     '  its token and its `device_id`. `DeviceStore.add`'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '- [ ] **F352 [code] iOS: a pairing for a reset Canary keeps polling with its'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '  untouched) and `docs/csi_developer_api.md` say so now; the canary-wap half\n'
     '  is unchanged. No behavior changed; not run on a device.\n'
     '- [x] **F83 [code] The canary-wap can commit an event before its id floor'),
    ('firmware/common/csi/src/csi_event_id_floor.h',
     '// stranger. A canary-wap has no reset path, so its owner flashes it with the'),
    ('firmware/projects/canary-wap/arduino/canary_wap/csi_event_id_floor.h',
     '// stranger. A canary-wap has no reset path, so its owner flashes it with the'),
    ('docs/csi_developer_api.md',
     '  sees a stranger; a canary-wap has no reset path, so its owner flashes it with the'),
    ('CHANGELOG.md',
     '  `space_low()` on both trees: a factory reset on a canary, a full flash erase\n'
     "  on a canary-wap without restoring the flasher's pre-erase backup, then a"),
    ('canary-local/devices/flash.json',
     '                    "then writes fresh. A factory reset and an install in one pass.",'),
    ('canary-local/tools/gen_flash.py',
     '                    "then writes fresh. A factory reset and an install in one pass.",'),
    ('canary-local/devices/flash.json',
     '      "what": "Wipes every byte — firmware, settings, stored WiFi, witness history — then writes fresh. A factory reset and an install in one pass.",'),
    ('canary-local/tools/gen_flash.py',
     '      "what": "Wipes every byte — firmware, settings, stored WiFi, witness history — then writes fresh. A factory reset and an install in one pass.",'),
    ('firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md',
     '        WAP has no factory-reset route at all (BOOT held ≥ 3 s is the only\n'
     '        path). Whether `POST /api/factory-reset` may exist is the decision;'),
    ('firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md',
     '        never be able to wipe a witness), with a two-step confirmation in the'),
    ('docs/design/onboarding_shared_module.md',
     "  before 700 ms — the WAP's 2 s hold and the 5 s factory-reset hold are"),
    ('firmware/canary/src/csi_event_log.h',
     ' * a factory reset) is left untouched and unused. The backfill signs what it'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     '/* Erase the persisted trusted-peer list. Used on factory reset and'),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     "      if (!confirm('Remove saved WiFi credentials from this Canary?')) return;"),
    ('firmware/common/network/improv_core.h',
     ' *   - NO CREDENTIALS STORED (first boot, or after a factory reset): the'),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     ' *   - NO CREDENTIALS STORED (first boot, or after a factory reset): the'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_improv.h',
     ' * for a WAP nobody owns yet: while no credentials are stored, for the'),
    ('firmware/canary/lib/securacv_webui/src/securacv_webui.cpp',
     '          PIN confirmation to prevent unauthorized access. Paired devices are stored securely and'),
    ('canary-local/assets/intake.js',
     '    meaning: "Flash is encrypted with a key burned into this chip. Reads come back as ciphertext and a plaintext image won\'t boot. Not reversible.",'),
    ('desktop/src-tauri/src/efuse.rs',
     '    meaning: "Flash is encrypted with a key burned into this chip. Reads come back as ciphertext and a plaintext image won\'t boot. Not reversible.",'),
    ('canary-local/assets/intake.js',
     '            meaning: "Flash is encrypted with a key burned into this chip. Reads come back as ciphertext and a plaintext image won\'t boot. Not reversible.",'),
    ('desktop/src-tauri/src/efuse.rs',
     '            meaning: "Flash is encrypted with a key burned into this chip. Reads come back as ciphertext and a plaintext image won\'t boot. Not reversible.",'),
    ('firmware/projects/canary-wap/arduino/canary_wap/household.h',
     ' *      store IRKs only in NVS (flash-encrypted at rest on ESP32 with secure'),
    ('docs/design/household_presence_and_posture.md',
     '  `hh_count`. Its comment says the IRKs are "flash-encrypted at rest on ESP32'),
    ('docs/research/face_id_without_faces.md',
     'Until then, `household.h`\'s claim that IRKs are "flash-encrypted at rest on\n'
     'ESP32 with secure boot" is wrong: flash encryption does not cover NVS, NVS'),
    ('firmware/projects/canary-wap/arduino/canary_wap/mesh_network.cpp',
     '// and opera_id derivation. On an unencrypted flash, a physical-access attacker'),
    ('firmware/canary/lib/securacv_ble_scan/src/ble_scan.h',
     " * (under flash-encryption gate, mirroring opera_secret's protection)."),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_scan.h',
     " * (under flash-encryption gate, mirroring opera_secret's protection)."),
    ('firmware/canary/lib/securacv_ble_scan/src/ble_scout_key.h',
     " * (mirroring opera_secret's hygiene — flash-encryption gate is the"),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_scout_key.h',
     " * (mirroring opera_secret's hygiene — flash-encryption gate is the"),
    ('firmware/scripts/regression_check.sh',
     '      check_fail "Mesh impl \'$rel\' has no esp_flash_encryption_enabled() gate — the opera_secret must never be persisted to unencrypted NVS (#610)"'),
    ('firmware/common/network/wifi_store.h',
     ' * and the secure kit in firmware/provisioning/ is the path up —'),
    ('firmware/projects/canary-display/arduino/canary_display/wifi_store.h',
     ' * and the secure kit in firmware/provisioning/ is the path up —'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wifi_store.h',
     ' * and the secure kit in firmware/provisioning/ is the path up —'),
    ('firmware/projects/canary-sense/src/main.cpp',
     '  // (common/identity/key_at_rest.h Tier 0); the secure kit is the path up.'),
    ('firmware/projects/canary-sentinel/src/main.cpp',
     '  // (common/identity/key_at_rest.h Tier 0); the secure kit is the path up.'),
    ('firmware/projects/canary-vision/src/main.cpp',
     '  // (common/identity/key_at_rest.h Tier 0); the secure kit is the path up.'),
    ('docs/PARITY.md',
     '| C9 | Credentials encrypted at rest | NVS encryption or flash encryption on the credential blobs |'),
    ('docs/PARITY.md',
     '   Secure Boot v2) stays the documented path. This is the one brief item the'),
    ('docs/FIRMWARE_MAP.md',
     'opt-in secure kit (flash encryption + Secure Boot v2) that no CI leg builds.\n'
     'NVS encryption is not configurable on either pinned Arduino core as built'),
    ('docs/FIRMWARE_MAP.md',
     'honest `⚠️`: posture reported on every node, the secure kit documented as the path; no fake encryption |'),
    ('docs/FIRMWARE_MAP.md',
     '  posture is reported as `creds_at_rest` on every node and the secure kit is\n'
     '  the documented path (PARITY footnote 1).'),
    ('docs/design/hardware_root_of_trust.md',
     '3. **Key extraction.** The identity key is plaintext-at-rest. Without flash\n'
     '   encryption or a hardware-bound key, physical possession = key compromise,\n'
     '   which undermines both attestation and the pinned-identity model.'),
    ('docs/design/hardware_root_of_trust.md',
     'still exists but the ROM **refuses to boot an unsigned image**, and flash reads\n'
     'come back ciphertext — so the browser flasher\'s "write any factory image and boot'),
    ('docs/design/hardware_root_of_trust.md',
     'firmware"* actually hold. Tier 3\'s non-extractable key (§5.4) additionally stops'),
    ('docs/design/hardware_root_of_trust.md',
     '  disables plaintext serial programming. Protects at-rest, **not** a live chip.'),
    ('docs/design/vault_operator_ux_v1_1.md',
     "  the **ESP32-S3's own eFuse / flash-encryption / DS peripheral** (device) holds —"),
    ('docs/design/vault_operator_ux_v1_1.md',
     '  path **(a)** hardens the ESP32-S3 Ed25519 identity *at rest* by burning'),
    ('docs/design/apple_home_integration.md',
     're-advertise mDNS, republish current signals. Pairings persist (encrypted NVS / hub keystore); nothing else is worth'),
    ('docs/design/apple_home_integration.md',
     '  NVS; they get the `opera_secret` treatment — refuse to persist on\n'
     '  flash-encryption-off devices (the tiered story from'),
    ('docs/hardware/tv_display_design.md',
     '  at-rest protection of the NVS-stored broker credentials relies on the\n'
     '  flash-encrypted secure build (`provisioning/sdkconfig.defaults.secure`), not'),
    ('docs/research/bitchat_protocol_review.md',
     "degrades neighbors' life-safety is a footgun. Device seizure is already handled by flash encryption + the witness kernel's"),
    ('docs/strategy/12-engineering-foundations-flight-rules.md',
     'deferral, but it ships today: either enable flash encryption + secure boot for\n'
     'production builds, or state the exposure explicitly in the threat model and on'),
    ('docs/strategy/13-black-box-evidence-model.md',
     'firmware signatures + dictionary hash | plaintext-NVS key on current canaries until flash encryption ships (doc 12 F2) — the honest'),
    ('spec/beacon_channel_v0.md',
     '- Reuses the existing key-management infrastructure (NVS, eFuse-derived flash encryption, key generation at first boot).'),
    ('docs/secure_provisioning.md',
     '- JTAG permanently disabled via eFuse\n'
     '- NVS encryption enabled'),
    ('docs/secure_provisioning.md',
     '`sdkconfig.defaults.secure` | ESP-IDF Kconfig with BT disabled, Secure Boot v2 **prepared but not enabled**, Flash Encryption release mode, NVS encryption, anti-rollback, JTAG off |'),
    ('firmware/provisioning/sdkconfig.defaults.secure',
     '# It encrypts NVS partition contents using a key stored in eFuse.\n'
     '#\n'
     'CONFIG_NVS_ENCRYPTION=y'),
    ('docs/audit/mesh_and_chirp_audit_v1.md',
     "`OperaConfig.opera_secret[32]` is the 32-byte symmetric secret from which `opera_id` is derived and which gates pairing. If the device's flash is not encrypted (ESP32 flash encryption is opt-in, not on by default outside production builds), a physical-access attacker can pull the secret with `esptool.py read_flash` and become a permanent opera member from outside the household.\n"
     '\n'
     '**Fix in this branch:** in `mesh_network::init`, call `esp_efuse_read_field_bit(ESP_EFUSE_FLASH_CRYPT_CNT)` (or the equivalent SDK helper) and refuse to load/save the opera_secret if flash encryption is not active. Log loudly to the health log at `LOG_LEVEL_ALERT, LOG_CAT_SECURITY`. Document the requirement in `spec/canary_mesh_network_v0.md` and `docs/security/SECURITY_MODEL.md`.'),
    ('docs/audit/mesh_and_chirp_audit_v1.md',
     '| NVS confidentiality | M | No FE gate (O2). |'),
    ('docs/audit/mesh_and_chirp_audit_v1.md',
     '  SD — pure append, never truncated — with the 64-entry flash-encrypted NVS\n'
     '  ring demoted to a recent-view cache (`beacon_channel.cpp::sd_append_audit_entry`).'),
    ('docs/audit/esp32s3_firmware_full_audit_2026-07.md',
     "but aren't wired into the FULL build — identity keys and the chain sit in plaintext flash today; a\n"
     'PSRAM-backed deferred-write ring would stop record loss while the SD card is absent (the mechanism'),
    ('docs/V1_BENCH_TEST_RUNBOOK.md',
     'proves on hardware that **no mesh secret lands in unencrypted NVS**.'),
    ('docs/V1_BENCH_TEST_RUNBOOK.md',
     '| C2 | **FE secret-at-rest gate** | pair on a **non-FE**'),
    ('docs/V1_LAUNCH_REVIEW.md',
     '   `opera_secret` can never be silently persisted to unencrypted NVS — the static half of issue'),
    ('docs/BLE_MESH_OPERA_TANDEM.md',
     "- The household key never leaves the device cleartext; it lives in NVS, encrypted by the device's secure-element-backed key (where available)."),
    ('docs/security/PENTEST_SCOPE.md',
     '  We know a physical attacker can dump flash and recover the identity key. What'),
    ('firmware/README.md',
     "| `secure` / `secure_ha` | the provisioning kit's Tier 3/4 image, flash encryption required (`provisioning/platformio_secure.ini`)"),
    ('docs/unflashed_board_intake.md',
     'If you want to be the last person who can ever do this to that board, the'),
    ('docs/getting_started_canary.md',
     '**Why the dashboard asks to be unlocked on your home network.** The page'),
    ('docs/getting_started_canary.md',
     'Each BOOT tap unlocks exactly one thing within those 30 seconds: one\n'
     'page load, or one app or browser fetching the recovery kit — whichever'),
    ('docs/getting_started_canary.md',
     'without a second tap; the iPhone app fetching the kit on its own needs a\n'
     'fresh tap. That is how you get the kit back later without a USB cable. *(New in 2026-09; checked by automated tests, with its first automated build pending; not'),
    ('firmware/common/network/provisioning_gate.h',
     '// BOOT tap — which the page load then SPENDS (page_token_decide). The tap is\n'
     '// one consumer across both paths: one page load or one receipt fetch,'),
    ('firmware/projects/canary-wap/arduino/canary_wap/provisioning_gate.h',
     '// BOOT tap — which the page load then SPENDS (page_token_decide). The tap is\n'
     '// one consumer across both paths: one page load or one receipt fetch,'),
    ('canary-vision/spa/app.js',
     "// body's gate_ttl_seconds), so a press anywhere in our 60 s window lands.\n"
     '// Returns a handle with cancel(); callers must cancel on navigation.'),
    ('canary-vision/spa/app.js',
     "      el('div', { className: 'pair-step-title', textContent: 'Short-tap the BOOT button' }),"),
    ('canary-vision/device-api/routes/provision.js',
     "// Mirrors the firmware's BOOT-button provisioning gate (wap_server.h):"),
    ('canary-vision/device-api/lib/device-state.js',
     '  // BOOT-button provisioning gate (mirrors firmware: a short-tap opens a'),
    ('docs/design/magic_pairing.md',
     'The hold helps only with a setup onto a network that host is not on, which\n'
     "the owner's browser does without it, and without reopening the first-boot"),
    ('docs/design/magic_pairing.md',
     "not, so it never writes a WAP's CREDS (ledger A65, open). Once A65 is\n"
     'settled the lane would move the WAP with no browser, but only while that\n'
     'host leaves it alone, and taking it away needs no radio. Any landing'),
    ('docs/security/THREAT_MODEL.md',
     '│  │  Secure Boot │  │  Flash       │  │  GPS Time    │  │'),
    ('docs/security/THREAT_MODEL.md',
     '  secure boot, flash encryption and eFuse locks on the opt-in\n'
     '  secure-provisioning tier (NOT on default builds)'),
    ('docs/security/THREAT_MODEL.md',
     '- **Secure tier: PARTIAL PASS.** Secure Boot, which the provisioning kit'),
    ('docs/security/THREAT_MODEL.md',
     '  flash encryption keeps the app image from being read off the chip, but it'),
    ('docs/security/THREAT_MODEL.md',
     '| Firmware corrupt | No tier refuses to boot it today: Secure Boot is prepared in the provisioning kit but not enabled (and is NOT on default builds — see [Scope of this principle](#scope-of-this-principle)); every tier relies on the OTA signature check before the image is written |'),
    ('docs/security/THREAT_MODEL.md',
     '  saved passwords stay plaintext on every tier today, Scenario 4) — opt-in\n'
     '  secure-provisioning tier only'),
    ('docs/security/THREAT_MODEL.md',
     '**LIMITATION:** A sufficiently resourced adversary with physical access\n'
     'can potentially bypass ESP32 secure boot (active area of research).'),
    ('docs/security/THREAT_MODEL.md',
     '- No outbound connections (compromised chip has no exfiltration channel)'),
    ('docs/security/THREAT_MODEL.md',
     'takes the device to a bench: the key lives in NVS, which flash encryption\n'
     '(an opt-in tier) does not cover, and the NVS encryption that would is not'),
    ('docs/security/SECURITY_MODEL.md',
     'mitigations, but they are **off unless you turn them on** — see'),
    ('docs/security/SECURITY_MODEL.md',
     'protection (flash encryption keeps the app image from being read off the\n'
     'chip; Secure Boot, which the provisioning kit prepares but does not yet'),
    ('canary-local/assets/flash.js',
     '    "eFuse locks) protects the firmware image, not the identity key or the " +'),
    ('canary-local/assets/flash.js',
     '    "For a board deployed somewhere physically exposed, put it out of reach: Phase-2 provisioning (flash encryption + eFuse) does not yet protect its key or saved WiFi.",'),
    ('docs/V1_BENCH_TEST_RUNBOOK.md',
     '| C0 | Production-provision FE boards | `firmware/provisioning/` → `./generate_keys.sh` then `./provision_canary.sh --port … --phase 2 --dry-run` then without `--dry-run` (uses `platformio_secure.ini` / `sdkconfig.defaults.secure` / `partitions_secure.csv`) | Secure Boot v2 + Flash Encryption burned; `verify_device.py` confirms | `O2/` |'),
    ('docs/secure_provisioning.md',
     '# 4. Enable Secure Boot v2'),
    ('docs/secure_provisioning.md',
     '# 6. Enable anti-rollback'),
    ('docs/secure_provisioning.md',
     '# 7. Verify all eFuses are correctly set'),
    ('docs/secure_provisioning.md',
     '| `FLASH_CRYPT_CNT` | 0 | odd (1,3,5,7) | Enables flash encryption |'),
    ('docs/secure_provisioning.md',
     '### Step 2: Build Signed Firmware'),
    ('docs/secure_provisioning.md',
     '| `provision_canary.sh` | Full orchestration: virgin verify → burn keys → flash signed firmware → burn security eFuses → post-verify → generate manifest. Has `--dry-run` mode |'),
    ('firmware/canary/include/secure_defaults.h',
     '// Secure Boot v2 and flash encryption are opt-in, through the provisioning'),
    ('docs/strategy/16-kit-commerce-pricing-and-fulfillment.md',
     '- **R3 "Assembled Witness" and any future evidence-grade SKU** run the Phase-2 secure\n'
     '  provisioning path from `firmware/provisioning/` (Secure Boot v2, flash encryption,'),
    ('firmware/provisioning/provision_canary.sh',
     '    - Flash signed firmware\n'
     '    - Burn flash encryption key\n'
     '    - Enable Secure Boot v2'),
    ('firmware/provisioning/provision_canary.sh',
     '    echo "  - Enable Secure Boot v2"'),
    ('firmware/provisioning/provision_canary.sh',
     '    echo "  - Enable anti-rollback"'),
    ('firmware/provisioning/provision_canary.sh',
     '            echo "  Device is locked down and ready for production."'),
    ('firmware/provisioning/provision_canary.sh',
     '#   1. Virgin verify → 2. Flash signed firmware → 3. Burn keys →'),
    ('docs/design/hardware_root_of_trust.md',
     '(virgin-verify → burn → flash → post-verify, with `--dry-run`), and'),
    ('docs/design/vault_operator_ux_v1_1.md',
     "- **Secure it (protect the key at rest) — the fuses are tooled, the key's"),
    ('docs/design/vault_operator_ux_v1_1.md',
     '  The fuse and signing steps are largely already tooled in'),
    ('docs/design/vault_operator_ux_v1_1.md',
     '   signing scheme; the fuse steps are mostly tooled, and NVS encryption, without'),
    ('docs/security/PENTEST_SCOPE.md',
     '- **Flash encryption and Secure Boot are off by default.** Deliberate, and the'),
    ('firmware/common/network/improv_core.h',
     " *   a device NOBODY OWNS YET, and for an owner's own tap — never for a"),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     " *   a device NOBODY OWNS YET, and for an owner's own tap — never for a"),
    ('docs/design/magic_pairing.md',
     "> the door is open for a device NOBODY OWNS YET, and for an owner's own tap —"),
    ('docs/design/magic_pairing.md',
     "| No credentials stored (first boot, or after a factory reset or Forget Wi-Fi), within **30 minutes** of power-on (after a reset, of the credentials vanishing) | **Open** (`Door::NoCredentials`) |"),
    ('docs/design/magic_pairing.md',
     "(`Timing::first_boot_window_ms`, half an hour by default); a power cycle re-arms it, a factory reset re-arms it from the moment the credentials vanish; compile-time `0` makes the device tap-only."),
    ('docs/design/onboarding_shared_module.md',
     '  cycle re-arms it, a factory reset re-arms it from the moment the\n'
     '  credentials vanish; compile-time 0 makes the device tap-only).'),
    ('docs/ble_protocol.md',
     "From `improv_core.h`: the door is open for a device NOBODY OWNS YET, and for an owner's own tap — never for a device that has an owner and a bad day."),
    ('docs/GLOSSARY.md',
     "device nobody owns yet, and for an owner's own tap — never for a device"),
    ('docs/FAQ.md',
     "device nobody owns yet, and for an owner's own tap — never for a device that"),
    ('firmware/projects/canary-sense/README.md',
     "**When the door is open** (the header's rule: for a device nobody owns yet,\n"
     "and for an owner's own tap — never for a device that has an owner and a bad"),
    ('firmware/projects/canary-vision/README.md',
     "**When the door is open** — the header's rule: for a device nobody owns\n"
     "yet, and for an owner's own tap — never for a device that has an owner and"),
    ('docs/FIRMWARE_MAP.md',
     '  claim an unowned device during its 30-minute first-boot window.'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     ' *              * no secret persisted (first boot, factory-reset state)'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     '/* Erase the persisted opera_secret. Use on factory reset or'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     ' *   • the key was already absent (idempotent — factory-reset semantics)'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     ' * buffer too small. On "no peers persisted" (first boot / factory'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     ' *   false — null pointer, FE disabled, nothing persisted (first boot /'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.cpp',
     '   * call idempotent across factory-reset semantics WITHOUT swallowing'),
    ('firmware/canary/src/main.cpp',
     '     * false on first-boot / factory-reset / FE-disabled devices —'),
    ('firmware/canary/src/main.cpp',
     '     * from peers paired in previous sessions. Empty-list (first\n'
     '     * boot / factory reset) is fine — mesh_session::on_opera_frame'),
    ('firmware/canary/lib/securacv_ble_scan/src/ble_scout_key.h',
     ' * a new one. Use for "unpair all Scouts" / factory reset. Returns'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_scout_key.h',
     ' * a new one. Use for "unpair all Scouts" / factory reset. Returns'),
    ('firmware/canary/lib/securacv_network/src/securacv_network.cpp',
     "  //      also ends the asking address's backoff (auth_handed_token, sweep"),
    ('firmware/projects/canary-wap/tests_host/test_receipt_grant.cpp',
     "  // and the tap ends that address's backoff (sweep F341): the page just got"),
    ('docs/getting_started_canary.md',
     'BOOT press prints neither: it opens the provisioning gate for 30 seconds,\n'
     'which hands the recovery kit to the one page already asking for it, so'),
    ('docs/getting_started_canary.md',
     "network.** (This and the next paragraph are the all-rounder Canary's,"),
    ('docs/getting_started_canary.md',
     'never fetches the kit with a tap: it pairs during setup, or from a kit you\n'
     'open in it. That is how you get the kit back later without a USB cable. *(New in 2026-09; checked by automated tests, with its first automated build pending; not'),
    ('docs/onboarding_multiple_canaries.md',
     '2. The app listens for ~60 seconds. **Short-tap the BOOT button** on that'),
    ('docs/onboarding_multiple_canaries.md',
     '4. Tap **+ Add another Canary** and repeat — every device after the first is\n'
     '   one tap in the app plus one tap on the device.'),
    ('canary-vision/spa/app.js',
     '// Pairing leads with the zero-typing path: pick (or discover) a device,\n'
     '// short-tap its BOOT button, and the app captures the provisioning'),
    ('ios/README.md',
     '    kit (the receipt its own pages save after a BOOT tap; opened from'),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     '        + "was wrong, this Canary\'s Bluetooth rescue or its own setup network "'),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     '// network. A wrong password strands one device (which the BLE provisioning\n'
     '// service and its own setup network can still rescue), never the fleet.'),
    ('ios/Sources/SecuraCV/Views/FleetWiFiSheet.swift',
     '// which keeps its Bluetooth rescue — never the fleet. Progress is honest'),
    ('ios/README.md',
     '  one device (which keeps its rescues), never the fleet. Each device rides'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     "    /// canary-wap's bonded Wi-Fi provisioning service (ble_provision.h) —\n"
     '    /// the rescue path for a Canary the router password change stranded.'),
    ('docs/legal-audit-2026-07.md',
     '  which never leaves the secure element."\n'
     '\n'
     '**Changed now:** I aligned the highest-stakes instances — the "tamper-proof"'),
    ('firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md',
     '        a BOOT-tap `provisioning_gate_take()` (a remote credential alone must\n'
     '        never be able to reopen the setup door), with a two-step confirmation in the'),
    ('firmware/FEATURES.md',
     '| API token NVS storage | ✅ | ❌ | ❌ | ❌ |'),
    ('firmware/FEATURES.md',
     '| TLS cert NVS storage | ✅ | ❌ | ❌ | ❌ |'),
    ('firmware/FEATURES.md',
     '| WiFi credentials NVS storage | ✅ | ❌ | ❌ | ❌ |'),
    ('firmware/FEATURES.md',
     '| Factory reset via BOOT button | ✅ (short/long press) | ✅ (10 s hold: Wi-Fi, broker, name; identity and token kept) | ✅ (10 s: credentials + settings, identity kept; 30 s: wipe, identity too) | ❌ |'),
    ('canary-local/tools/hatchery/derive.mjs',
     '//   gets its certificate the moment a client can see its key; a factory reset\n'
     '//   that mints a NEW key is honestly a new bird, and says so.'),
    ('ios/Sources/SecuraCV/Security/DeviceStore.swift',
     '        // sight then (TOFU) — a fully erased (re-flashed) device with a new'),
    ('custom_components/securacv/tests/test_replay_reset_paths.py',
     'to the KEY: an erased (re-flashed) or replaced Canary starts them over. So every'),
    ('firmware/common/network/auth_backoff.h',
     '// polling a re-flashed unit with its old token (a full erase gives it a new'),
    ('firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h',
     '// polling a re-flashed unit with its old token (a full erase gives it a new'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '- [ ] **F352 [code] iOS: a pairing for a re-flashed Canary keeps polling with'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '  token. *(Amended 2026-10-21, #1843: a re-flashed unit; the 10 s hold keeps\n'
     '  the token, and only a full erase in a flasher changes it, F352.)*'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     "  (iOS, a reset unit's stale pairing; amended 2026-10-21, #1843: a\n"
     "  re-flashed unit's, since the 10 s hold keeps the token)."),
    ('firmware/LESSONS_LEARNED.md',
     "  and the likeliest such host was the owner's own phone, polling a\n"
     '  re-flashed unit (a full erase gives it a new key and token; the 10 s hold'),
    ('CHANGELOG.md',
     '  still polling a re-flashed unit (a full erase gives it a new key and'),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     "      if (!confirm('Remove every saved Wi-Fi network from this Canary? For half an hour after, its Bluetooth setup goes to the first phone or computer nearby that connects, so have yours ready.')) return;"),
    # #1843's sweep, third pass (its review round 2):
    ('firmware/provisioning/platformio_secure.ini',
     '; To enable full Secure Boot:'),
    ('firmware/provisioning/platformio_secure.ini',
     '; - Prepares for Secure Boot (not enabled until Phase 2 eFuse burn)'),
    ('firmware/provisioning/sdkconfig.defaults.secure',
     '# To enable full security (Phase 2):'),
    ('firmware/provisioning/sdkconfig.defaults.secure',
     '# Uncomment these lines for production lockdown:'),
    ('firmware/provisioning/README.md',
     '- **Phase 2 (Future):** Production lockdown with Secure Boot v2, Flash Encryption, JTAG disabled'),
    ('firmware/provisioning/README.md',
     '# 3. Execute production provisioning'),
    ('firmware/provisioning/generate_keys.sh',
     '    "Loss of signing key = bricked devices on next OTA",'),
    ('docs/secure_provisioning.md',
     'Loss of signing key = bricked devices on next OTA.'),
    ('docs/secure_provisioning.md',
     '#   keys/secure_boot_signing_key.pub  (embedded in firmware)'),
    ('docs/secure_provisioning.md',
     'The secure configuration disables Bluetooth at compile time (`CONFIG_BT_ENABLED=n`).'),
    ('docs/secure_provisioning.md',
     '# Or with ESP-IDF directly'),
    ('canary-local/assets/flash.js',
     '    "yet turn on flash encryption or secure boot, so today it protects " +\n'
     '    "nothing on the board. Even turned on, flash encryption would cover the " +'),
    ('ios/Sources/SecuraCV/Security/DeviceStore.swift',
     '        // previous life (an ordinary re-flash keeps the key).'),
    ('custom_components/securacv/tests/test_replay_reset_paths.py',
     'to the KEY: a fully erased or replaced Canary starts them over (an ordinary\n'
     're-flash keeps the key and its counters). So every'),
    ('firmware/common/network/auth_backoff.h',
     '// gives it a new key, so a new token and device_id; an ordinary re-flash and'),
    ('firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h',
     '// gives it a new key, so a new token and device_id; an ordinary re-flash and'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '  key (the 10 s hold and an ordinary re-flash keep the key and the token),'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '  an ordinary re-flash keep the token, and only a full erase in a flasher\n'
     '  changes it, F352.)*'),
    ('firmware/LESSONS_LEARNED.md',
     '  fully erased unit (a full erase gives it a new key and token; the 10 s hold\n'
     '  and an ordinary re-flash keep the token) with its old token. Because a `static` in a header is a separate object in'),
    ('CHANGELOG.md',
     '  token; the 10 s hold and an ordinary re-flash keep the token) with its old'),
    ('CHANGELOG.md',
     '    "re-flashed" no longer stands for "fully erased" (an ordinary re-flash\n'
     '    keeps NVS).'),
    ('CHANGELOG.md',
     "  Canary; only the flagship's 30 s wipe or a full erase in a flasher changes\n"
     '  them. The getting started guide said a factory reset loses the witness'),
    ('CHANGELOG.md',
     '  full erase in either flasher is what removes them).'),
    ('firmware/canary/scripts/ota_deploy.py',
     '        log_error("Refusing to send the token. Only the 30 s BOOT wipe or a full flash erase")'),
    ('firmware/canary/scripts/ota_deploy.sh',
     '    log_error "Refusing to send the token. Only the 30 s BOOT wipe or a full flash erase"'),
    ('docs/security/PENTEST_SCOPE.md',
     "  by design, so every hub and display that pinned the key still knows it. Only the all-rounder Canary's 30 s wipe or a full erase\n"
     "  in a flasher sheds them. So whatever already holds a WAP's token, such as a"),
    ('docs/design/magic_pairing.md',
     '  the reset keeps, and nothing rotates the token today; only a full erase in\n'
     '  a flasher replaces them, with the identity. And either reset reopens the'),
    ('desktop/src/index.html',
     '                left in the other partitions would survive. Reflashing a Canary\n'
     '                you set up here and want to keep the identity of? Untick it —'),
    ('firmware/projects/canary-sense/include/canary/witness.h',
     '// same storage contract as the canary-wap tree, so the keypair survives\n'
     '// reflashes and OTA installs. The fingerprint is the wap formula:'),
    ('firmware/projects/canary-vision/include/canary/witness.h',
     '// same storage contract as the canary-wap tree, so the keypair survives\n'
     '// reflashes and OTA installs. The fingerprint is the wap formula:'),
    ('firmware/projects/canary-sentinel/include/canary/witness.h',
     '// keypair survives reflashes and OTA installs. The fingerprint is the wap'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  // it stays stable across reflashes because the keypair persists in NVS.'),
    ('canary-local/tools/gen_flash.py',
     '                    "its sealed diary. The chain rides through reflashes untouched.",'),
    ('canary-local/devices/flash.json',
     '      "what": "How many entries this Canary\'s tamper-evident hash chain holds — its sealed diary. The chain rides through reflashes untouched."'),
    ('canary-local/tools/gen_flash.py',
     '                    "key and the chain — then writes fresh, so the board comes up with a new "'),
    ('canary-local/devices/flash.json',
     '      "what": "Wipes every byte of flash — firmware, settings, stored WiFi, the identity key and the chain — then writes fresh, so the board comes up with a new identity. More than a factory reset: the 10-second BOOT reset keeps the key.",'),
    ('canary-local/tools/gen_flash.py',
     '            "default": "Off — a normal install only touches the regions the new firmware needs.",'),
    ('canary-local/devices/flash.json',
     '      "default": "Off — a normal install only touches the regions the new firmware needs."'),
    ('docs/README.md',
     "- [**Magic pairing — the Bluetooth setup door (design)**](design/magic_pairing.md) — built, host-tested, **not bench-tested**: a new Sense, Vision or WAP takes its Wi-Fi from the iPhone app with one tap over the open Improv Wi-Fi standard. The door rule (open for a device nobody owns yet, for 30 minutes after power-on, or for 60 s after the owner's own BOOT tap — never because a saved network is failing), the bounds a stranger in radio range runs into, the WAP's claim ticket that gets the phone paired without the bearer token ever riding Bluetooth, the UUIDs and byte layouts, the per-family lifecycle, the iOS flow, the residual risks and the bench checklist"),
    ('firmware/LESSONS_LEARNED.md',
     '  that has an owner and a bad day.*\n'
     '- **Regression check:** `tests_host/test_improv_core.cpp` — a session begun'),
    ('firmware/LESSONS_LEARNED.md',
     '  A door that is open to whoever has no owner yet must still be a door\n'
     '  nobody can wedge.'),
    ('docs/design/iphone_companion_app.md',
     'nothing but the per-device tokens it earned by the BOOT-button tap, held in'),
    ('docs/design/iphone_companion_app.md',
     '2. **Pairing + local read.** Trust-on-Pair BOOT-button receipt → Keychain token;'),
    ('ios/Sources/SecuraCV/Views/FleetWiFiSheet.swift',
     '                    candidateRow(c, note: "Come back when it\'s in Bluetooth range")'),
    ('ios/Sources/SecuraCV/Views/FleetWiFiSheet.swift',
     '               + "goes only to your Canaries — over bonded Bluetooth, over a "'),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     '        + "approved. Move within Bluetooth range for its encrypted lane, or "'),
    ('ios/Sources/SecuraCV/Model/FleetWiFiRollout.swift',
     '        + "flight. Their encrypted lanes aren\'t available: Bluetooth needs "'),
    ('docs/GLOSSARY.md',
     'flash encryption (audit O2). That keeps the household secret off un-fused\n'
     'boards; it does **not** make it confidential at rest on fused ones, because'),
    ('docs/design/apple_home_integration.md',
     '  flash-encryption-off devices, which keeps them off un-fused boards but does'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     ' * route). So today the gate keeps the secret off un-fused boards; it does'),
    ('firmware/projects/canary-wap/arduino/canary_wap/mesh_network.cpp',
     '// which keeps the secret off un-fused boards only. Devices that do not'),
    ('spec/canary_mesh_network_v0.md',
     'opera member. The FE gate keeps the secret off un-fused boards, at the cost'),
    ('spec/beacon_channel_v0.md',
     '| Beacon set (pubkeys, names, last_seen) | NVS | No — persisted only with flash encryption on (same gate as the Opera secret; an un-fused board keeps the set in RAM only), and plaintext NVS there too: flash encryption does not cover NVS, and NVS encryption is not available under `framework = arduino` (`docs/security/THREAT_MODEL.md`, the Opera mesh section) |'),
    ('firmware/scripts/regression_check.sh',
     '# item 9) — the gate keeps the secret off un-fused boards, it does not encrypt'),
    ('docs/audit/mesh_and_chirp_audit_v1.md',
     "`OperaConfig.opera_secret[32]` is the 32-byte symmetric secret from which `opera_id` is derived and which gates pairing. If the device's flash is not encrypted (ESP32 flash encryption is opt-in, not on by default outside production builds), a physical-access attacker can pull the secret with `esptool.py read_flash` and become a permanent opera member from outside the household. *(Amended 2026-10-21: flash encryption does not stop it either. It does not cover NVS, so on a fused board the secret is still plaintext, and no released build turns flash encryption on. The O2 gate keeps the secret off un-fused boards only; `spec/canary_mesh_network_v0.md` §5.5.)*"),
    # #1843's sweep, fourth pass (its review round 3): both flashers' install
    # verdicts and first-contact copy, the browser trust card's key that
    # "never leaves", the device_id and handle said to survive reflashes, the
    # flagship's certificate comment and the other key-change lists that left
    # out an install, the token the flagship's hold does not keep, the
    # dashboard's press-at-once retry lines, the kit's Phase 2 run and table
    # notes, and Secure Boot described as not enabled.
    ('canary-local/assets/flash-core.js',
     '      detail: "A newer build of the same firmware. Your settings region is left alone unless the map below says otherwise.",'),
    ('canary-local/assets/flash-core.js',
     '    detail: "The board already runs this exact version. Installing again rewrites it byte-for-byte — " +'),
    ('canary-local/assets/flash-core.js',
     '            "harmless, and occasionally exactly what a misbehaving board needs.",'),
    ('canary-local/assets/flash-core.js',
     '              "but newer settings may not be understood by older firmware. The automatic safety copy is your way back.",'),
    ('desktop/src/app.js',
     '      detail: "A newer build of the same firmware. Your settings region is left alone." };'),
    ('desktop/src/app.js',
     '    detail: "The board already runs this exact version. Installing again rewrites it byte-for-byte — " +'),
    ('desktop/src/app.js',
     '      "harmless, and occasionally exactly what a misbehaving board needs." };'),
    ('desktop/src/app.js',
     '        "misbehaves — but newer settings may not be understood by older firmware. The automatic " +'),
    ('canary-local/assets/flash.js',
     '    "chip. The private key is born there and never leaves — there is no " +'),
    ('canary-local/assets/flash.js',
     '    "export function, and this page never reads it (the health check shows " +'),
    ('canary-local/assets/flash.js',
     '    "permission; forging it needs the key that never left the board."));'),
    ('canary-local/assets/flash.js',
     '    "Key theft through the browser — the identity key is never transmitted, " +'),
    ('canary-local/assets/flash.js',
     '        "that decides. If this is genuinely your own Canary and you want to keep its " +'),
    ('canary-local/assets/flash.js',
     '      const mine = el("button", "ghost small", "it\'s mine — keep its data");'),
    ('canary-local/assets/flash.js',
     '  // The user\'s explicit "this board is already mine, keep its data". The ONLY'),
    ('canary-local/assets/flash.js',
     '    "theater. The project\'s Phase-2 provisioning burns eFuses but does not " +'),
    ('canary-local/assets/flash.js',
     "    // A display's glass setup only ever asks for Wi-Fi, and dev_id is sticky\n"
     '    // forever once written — so with nothing seeded, first boot persists the'),
    ('canary-local/assets/intake.js',
     '//   - `ownerClaimed` — the human said "this is my board, keep its data"'),
    ('docs/unflashed_board_intake.md',
     '  deliberate "it\'s mine — keep its data" button. That preserves reflashing your'),
    ('docs/unflashed_board_intake.md',
     'button. That preserves reflashing your\n'
     '  own Canary without losing its identity key, and puts the decision with the'),
    ('canary-local/tools/gen_flash.py',
     '             "factory install like this one is deeper: on some boards the diary’s "'),
    ('canary-local/devices/flash.json',
     'A USB factory install like this one is deeper: on some boards the diary’s chain state rides the settings region and starts fresh here.'),
    ('firmware/projects/canary-sense/include/canary/runtime_config.h',
     '//   - device_id: NVS always wins (identity is sticky across reflashes and'),
    ('firmware/projects/canary-vision/include/canary/runtime_config.h',
     '//   - device_id: NVS always wins (identity is sticky across reflashes and'),
    ('firmware/projects/canary-sentinel/include/canary/runtime_config.h',
     '//   - device_id: NVS always wins (identity is sticky across reflashes and'),
    ('firmware/projects/canary-display/include/canary/runtime_config.h',
     '//   - device_id: NVS always wins (identity is sticky across reflashes and'),
    ('firmware/projects/canary-display/arduino/canary_display/runtime_config.h',
     '//   - device_id: NVS always wins (identity is sticky across reflashes and'),
    ('firmware/canary/lib/securacv_crypto/src/securacv_crypto.cpp',
     '// API payload, log, or advertised identifier). The keypair persists in NVS, so\n'
     "// the handle is stable for the device's lifetime — a *device* handle, not a"),
    ('firmware/canary/lib/securacv_crypto/src/securacv_crypto.h',
     "// alphabet (no 0/O/o, 1/I/i/l/L); stable for the device's lifetime."),
    ('firmware/common/identity/device_pseudonym.h',
     '// pseudonym token. Reads no hardware MAC; never exposes a trackable ID. The salt is\n'
     "// fixed for the device's lifetime, so the result is computed once and cached in RAM."),
    ('firmware/projects/canary-wap/arduino/canary_wap/device_pseudonym.h',
     '// pseudonym token. Reads no hardware MAC; never exposes a trackable ID. The salt is\n'
     "// fixed for the device's lifetime, so the result is computed once and cached in RAM."),
    ('firmware/projects/canary-display/arduino/canary_display/device_pseudonym.h',
     '// pseudonym token. Reads no hardware MAC; never exposes a trackable ID. The salt is\n'
     "// fixed for the device's lifetime, so the result is computed once and cached in RAM."),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  // One stable SSID for the lifetime of the device — the same name during'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '// witness records and health log on SD, the API token (derived from the\n'
     "// key), the OTA settings, the CSI module's tuning. Runs on the loop task"),
    ('firmware/LESSONS_LEARNED.md',
     '  bits (injective, so per-board uniqueness and reflash-stability are preserved).'),
    ('docs/firmware_ota.md',
     '  seeds the very first boot. HA entities survive OTA updates and reflashes.'),
    ('canary-local/tools/gen_sense.py',
     '        "device_id: NVS always wins — identity is sticky across reflashes and OTA updates; the compiled seed only fills the very first boot",'),
    ('canary-local/devices/sense.json',
     '      "device_id: NVS always wins \\u2014 identity is sticky across reflashes and OTA updates; the compiled seed only fills the very first boot",'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_improv.cpp',
     "// The claim stays readable over the link that provisioned for the ticket's\n"
     '// own TTL; a phone that never reads it needs the BOOT-tap route later.'),
    ('firmware/canary/include/canary_config.h',
     '// generated once on the first TLS-capable boot. The 10 s BOOT reset keeps\n'
     '// both (reset_scope_keys.h kIdentityKept); only the 30 s wipe\n'
     '// (ResetScope::Everything) or a full flash erase removes them, after which\n'
     '// the device re-keys and a pinned iPhone pairing must be redone.'),
    ('custom_components/securacv/sensor.py',
     '    device is (re)pinned via TOFU (async_record_verify), which is what a\n'
     "    full flash erase (or an all-rounder Canary's 30 s BOOT wipe) looks like\n"
     '    from here; the 10 s BOOT reset keeps the key and its counters.'),
    ('custom_components/securacv/tests/test_replay_and_device_id.py',
     '    # A full erase (a new key): the device pins fresh and its chain starts over.'),
    ('firmware/canary/src/csi_event_log.h',
     " * this device (another device's card, a canary-wap card, a card from before\n"
     ' * the 30 s BOOT wipe or a full erase; the 10 s reset keeps the key) is left'),
    ('canary-local/tools/hatchery/derive.mjs',
     '//   gets its certificate the moment a client can see its key; a full erase\n'
     "//   (or the all-rounder Canary's 30 s BOOT wipe) that mints a NEW key is"),
    ('firmware/common/health/boot_policy.h',
     ' *   Re-flashing the SAME build keeps the count; the operator clear (or an NVS'),
    ('firmware/common/health/boot_policy.h',
     '// Re-flashing the SAME build keeps the count (it is still the image that could'),
    ('firmware/tests_host/test_boot_guard.cpp',
     ' *     A/B rollback or USB re-flash) — and re-flashing the SAME build does not;'),
    ('firmware/common/csi/src/csi_event_id_floor.h',
     "// keeps the identity), so its owner flashes it with the flasher's full\n"
     '// erase, which clears all of NVS, the floor `ev.next` and `csi.evsent`'),
    ('firmware/projects/canary-wap/arduino/canary_wap/csi_event_id_floor.h',
     "// keeps the identity), so its owner flashes it with the flasher's full\n"
     '// erase, which clears all of NVS, the floor `ev.next` and `csi.evsent`'),
    ('docs/csi_developer_api.md',
     '  keeps both, so its owner flashes it with the\n'
     "  flasher's full erase, which clears the floor (`ev.next`) and `csi.evsent`"),
    ('docs/TRUST_MODEL.md',
     'every pin knows; shedding the key is the opt-in 30-second stage the PIO canary\n'
     'alone enables, with a serial warning first.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     '        ? "Didn\'t see a tap. Click Use BOOT Tap Once again, then short-tap BOOT."'),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     "            s.textContent = tapContendedText('click Use BOOT Tap Once again, and only then short-tap BOOT.');"),
    ('docs/FAQ.md',
     'seconds, then click **Use BOOT Tap Once** again and only then short-tap\n'
     'BOOT (a press with no page of yours asking goes to whichever device asks'),
    ('docs/audit/hardware_verification_checklist.md',
     'After 60 s the page says "Didn\'t see a tap. Click Use BOOT Tap Once again, then short-tap BOOT."'),
    ('CHANGELOG.md',
     '  left the flagship out of its way-out amendment. The 10 s BOOT hold keeps\n'
     '  the identity key, the chain, the token and the HTTPS certificate on every\n'
     "  Canary; the flagship's 30 s wipe, and an install or a full erase in either"),
    ('CHANGELOG.md',
     "  says what the kit does (and `PENTEST_SCOPE.md`'s known-and-accepted list\n"
     '  now says the 10 s reset keeps the identity, the token and the certificate\n'
     '  by design).'),
    ('CHANGELOG.md',
     '  and certificate; an over-the-air update, a PlatformIO or Arduino IDE\n'
     '  upload and the 10 s BOOT reset keep them. The second pass had written the'),
    ('CHANGELOG.md',
     "  changes it; every copy now says what the flashers do: the iPhone app's"),
    ('CHANGELOG.md',
     '    blank, and the third pass below corrects every copy.)'),
    ('CHANGELOG.md',
     '  seconds, then click Use BOOT Tap Once again, and only then short-tap\n'
     '  BOOT" (the companion: tap Save again, and press when its note says to)'),
    ('CHANGELOG.md',
     '  press), the dashboard\'s timeout line ("Didn\'t see a tap. Click Use BOOT\n'
     '  Tap Once again, then short-tap BOOT."; it said "Press BOOT and click'),
    ('docs/security/PENTEST_SCOPE.md',
     '  certificate, and the API token, which is derived from the key, comes back\n'
     '  the same (a Sense, a Vision and a Sentinel keep their key and chain too),'),
    ('docs/design/magic_pairing.md',
     'row, the device name and the first-boot flag, keeps the identity key and the\n'
     'API token derived from it, and restarts into first-boot setup when BOOT is'),
    ('docs/design/magic_pairing.md',
     '  hold and Forget Wi-Fi keep both, since both derive from the identity key\n'
     '  the reset keeps, and nothing rotates the token today; only an install or a'),
    ('docs/design/magic_pairing.md',
     'new token and device_id; the 10 s hold and an OTA update keep the token) keeps\n'
     'polling with the old token,'),
    ('firmware/projects/canary-wap/README.md',
     'keeps the identity key and the API token derived from it, and restarts'),
    ('firmware/projects/canary-wap/arduino/canary_wap/README.md',
     '- API token derived via HKDF (never stored in plaintext)'),
    ('docs/security/THREAT_MODEL.md',
     'Install is a button, and the per-device Auto Update switch is an explicit owner opt-in (off by default). Every install verifies the Ed25519 release signature and the version floor first.'),
    ('docs/security/THREAT_MODEL.md',
     "What no tier refuses today is a well-formed image that is unsigned or forged: that is Secure Boot's job, which the provisioning kit prepares but does not enable (it is NOT on default builds"),
    ('docs/security/THREAT_MODEL.md',
     '- Secure boot (would verify firmware integrity; prepared in the provisioning kit, not yet enabled on any tier)'),
    ('docs/design/household_presence_and_posture.md',
     '  it), and secure boot is prepared but not enabled on any tier'),
    ('docs/device_trust.md',
     'alone) and the 10 s BOOT reset keep the key, so after one of those a mismatch means something else is\n'
     'publishing under that `device_id`: do not rotate to it.'),
    ('docs/secure_provisioning.md',
     '# Review output carefully, then execute for real:\n'
     './provision_canary.sh --port /dev/ttyACM0 --phase 2'),
    ('docs/secure_provisioning.md',
     '### Step 3: Provision Device (Dry Run First!)'),
    ('firmware/provisioning/provision_canary.sh',
     '# SecuraCV Canary — Production Provisioning Script'),
    ('firmware/provisioning/provision_canary.sh',
     'Provision a SecuraCV Canary device for production deployment.'),
    ('firmware/provisioning/provision_canary.sh',
     '  # Execute Phase 2 provisioning (IRREVERSIBLE)'),
    ('firmware/provisioning/generate_keys.sh',
     '    echo "for SecuraCV Canary production provisioning (Phase 2)."'),
    ('firmware/provisioning/README.md',
     '- Always use `--dry-run` before production provisioning'),
    ('firmware/provisioning/partitions_secure.csv',
     '# Phase 2 (production):\n'
     '#   Encrypted flags are enforced after eFuse burn'),
    ('firmware/provisioning/partitions_secure.csv',
     '# - nvs_keys stores NVS encryption key (derived from eFuse); ESP-IDF says to'),
    ('ios/Shared/SetupGuide.swift',
     'body: "A WAP keeps its own setup page on its own network, SecuraCV-XXXX. Join it'),
    ('docs/research/face_id_without_faces.md',
     "core's precompiled `sdkconfig` has it), and secure boot is prepared but not\n"
     'enabled on any tier ([`key_at_rest.h`](../../firmware/common/identity/key_at_rest.h)).'),
    ('docs/security/THREAT_MODEL.md',
     '**LIMITATION:** Once Secure Boot is enabled (no tier enables it today), a'),

    ('firmware/canary/src/main.cpp',
     '                    : "ZERO or unreadable - a re-flash will not reset the crash-loop count");'),
    ('firmware/canary/src/main.cpp',
     '  // Bench row E6: a re-flash of another build resets the count only if the\n'
     '  // build filled app_elf_sha256 in the app descriptor. Say so on every boot.'),
    ('firmware/common/health/boot_guard.h',
     '// descriptor; if it reads as all zeros, every build looks like the same image\n'
     '// and a re-flash of another build silently stops resetting the count (the'),
    ('docs/design/magic_pairing.md',
     "from the owner's: the first is a device nobody owns, the second is a device\n"
     'that has an owner and a bad day.'),
    ('docs/security/THREAT_MODEL.md',
     '  too, the secure-tier bullet above), and on the default tier, whose flash\n'
     '  is readable on a bench, an install or a full erase in either flasher, each\n'
     '  of which rewrites the settings region, is what removes them.'),
    ('docs/security/THREAT_MODEL.md',
     '**Result:** Requires Ed25519 private key (never leaves device) AND\n'
     're-hashing/re-signing every subsequent record in the chain.'),
    ('docs/security/THREAT_MODEL.md',
     '**PASS if:** Forgery requires physical device possession AND key extraction.'),
    ('docs/security/THREAT_MODEL.md',
     "The Ed25519 private key is generated on the ESP32-family chip's hardware RNG\n"
     'and stored in NVS. It is **never**:\n'
     '- Transmitted over WiFi, USB, or any interface'),
    ('docs/security/THREAT_MODEL.md',
     'Every item above is a statement about\n'
     '*software* paths. It is not a claim that the key survives an adversary who\n'
     'takes the device to a bench: the key lives in NVS, which flash encryption'),
    ('docs/security/THREAT_MODEL.md',
     'Reviewers should hold this principle to the software boundary and treat\n'
     'physical extraction as the separately-documented trade it is.'),
    ('docs/security/THREAT_MODEL.md',
     '  services used. Secure boot and flash encryption are prepared in the opt-in\n'
     '  provisioning kit but enabled on no tier today (Scenario 4); its Phase 2\n'
     '  burns the JTAG-disable fuses, and is NOT run on default builds'),
    ('docs/unflashed_board_intake.md',
     'The Phase 2 lockdown in [secure_provisioning.md](secure_provisioning.md) is\n'
     'where secure boot and flash encryption are staged, prepared but not yet\n'
     'enabled: today it does not make you the last person who can change what runs'),
    ('docs/secure_provisioning.md',
     '| `sdkconfig.defaults.secure` | ESP-IDF Kconfig with BT disabled; Secure Boot v2 (with anti-rollback) and Flash Encryption (release mode) **prepared but not enabled** (commented out); NVS encryption named but inert under `framework = arduino`;'),
    ('firmware/provisioning/sdkconfig.defaults.secure',
     '# NVS ENCRYPTION (Protects stored keys and settings)'),
    ('firmware/provisioning/sdkconfig.defaults.secure',
     '# This can be enabled independently of flash encryption for Phase 1.'),
    ('firmware/provisioning/sdkconfig.defaults.secure',
     '# It encrypts NVS partition contents using a key stored in eFuse.\n'
     '# KNOWN INERT under framework = arduino (platformio_secure.ini): the line'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.cpp',
     ' * on its opera_config write/load paths. Devices without FE blown\n'
     ' * simply cannot store an opera_secret — the user must complete\n'
     ' * pairing on every boot until the eFuse is committed. The device\'s'),
    ('docs/TRUST_MODEL.md',
     'above (Invariants II/III); the first-boot 30-minute Improv window and the\n'
     'BOOT-tap minute are the only times a node takes Wi-Fi from a phone.'),
    ('docs/IMPROVEMENT_ROADMAP.md',
     '`FleetWiFiRolloutTests` extended (lanes, pilot order, disclosure, gate). **Swift compiled and tested by the PR\'s iOS CI; the WAP endpoint and fixture are host-tested.** | S |'),
    ('firmware/LESSONS_LEARNED.md',
     '  rollout prefers the bonded BLE lane and discloses plain http.\n'
     '- **Guidance:** a contract that exists only as "what the other side sends'),
    ('firmware/LESSONS_LEARNED.md',
     '- **Principle:** The Ed25519 private key has no export, backup, or read interface\n'
     '- **Why:** A key that can be exported can be compelled (court orders, coercion)'),
    ('firmware/LESSONS_LEARNED.md',
     '  way to start fresh on purpose is the flagship\'s 30 s BOOT wipe, or a clean\n'
     '  install on the canary-wap, whose BOOT hold keeps the identity.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/README.md',
     'takes no compile-time credentials; Wi-Fi is provisioned over the captive\n'
     'portal and the AP password and API token are derived per device. See'),
    ('firmware/projects/canary-wap/secrets/secrets.example.h',
     ' *   - The API token: derived per device from the private key\n'
     ' *     (derive_api_token), stored in NVS, shown on the serial banner.'),
    ('firmware/projects/canary-wap/setup.sh',
     ' *   - The API token: derived per device from the private key\n'
     ' *     (derive_api_token), stored in NVS, shown on the serial banner.'),
    ('docs/onboarding_workflow_evaluation.md',
     '1. **Boot / identity** — `provision_device()` loads/creates the Ed25519\n'
     '   keypair, then derives a stable `device_id` (`canary-s3-4dC2`) and AP SSID'),
    ('docs/onboarding_workflow_evaluation.md',
     '   event_contract §10), and derives the API token. (`canary_wap.ino`)'),
    ('docs/security/PENTEST_SCOPE.md',
     '| **Critical** | Forging a record that passes verification. Extracting a device identity key by any means. Reading footage or event content off a device you have not been granted access to.'),
    ('docs/security/PENTEST_SCOPE.md',
     '**Retrieving key bytes is always a finding, including from a device whose key you already hold** (see below).'),
    ('docs/security/PENTEST_SCOPE.md',
     '   To be explicit, because the distinction is the whole point: **demonstrating\n'
     '   *retrieval* of key bytes is always a finding**, including from device #1 or'),
    ('docs/security/PENTEST_SCOPE.md',
     "3. **The third device's key is withheld**, and it is the key-extraction\n"
     '   target. Handing over every key would have made the entire Critical'),
    ('docs/security/PENTEST_SCOPE.md',
     '- **Flash encryption and Secure Boot are off by default.** Off on every\n'
     '  Canary today, in fact: the opt-in provisioning kit stages both and turns on'),
    ('docs/security/SECURITY_MODEL.md',
     "- Stored only on the device's internal flash memory"),
    ('docs/security/SECURITY_MODEL.md',
     '- Never transmitted over any interface (WiFi, USB, or otherwise)'),
    ('docs/security/SECURITY_MODEL.md',
     '- Not backed up, escrowed, or recoverable by anyone — including ERRERlabs'),
    ('docs/security/SECURITY_MODEL.md',
     'the device, an adversary with sufficient power WILL extract it. So there\n'
     'is no software path that will hand it over: no API, no export, no'),
    ('docs/security/SECURITY_MODEL.md',
     '**The limit of that promise.** It is a statement about software. Someone\n'
     'who physically takes the device and reads the flash chip directly is a\n'
     'different adversary, and today they can recover the key from any Canary,'),
    ('docs/security/SECURITY_MODEL.md',
     "1. The device's Ed25519 private key (which never leaves the device)"),
    ('docs/security/SECURITY_MODEL.md',
     '**The honest statement: someone who takes a Canary away and opens it up\n'
     'can read the private key out of the flash chip, in its default'),
    ('CHANGELOG.md',
     '  install in either flasher is its only way to a new identity on purpose,\n'
     '  halted or not. It halts, where other provisioning failures go on (sweep'),
    ('CHANGELOG.md',
     "  the device's `/status` verdict shown; a WAP's bonded Bluetooth provisioning\n"
     "  service for a brand-new unit; and the Fleet tab's discovery for the"),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '  build if the sketch arms one), so a clean install in either flasher is\n'
     '  its only way to a new identity on purpose, halted or not; the halt\n'
     '  removes no path that was there.'),
    ('firmware/README.md',
     'Production device provisioning tools are in `provisioning/`:'),
    ('firmware/README.md',
     '├── provisioning/   # production key + eFuse tooling (and the compile-only secure envs)'),
    ('firmware/README.md',
     '# Full provisioning workflow\n'
     './provisioning/provision_canary.sh --port /dev/ttyACM0'),
    ('firmware/canary/include/secure_defaults.h',
     '// The Ed25519 private key is stored in NVS and has no read interface.'),
    ('firmware/canary/include/secure_defaults.h',
     '// That is the SOFTWARE boundary. At-rest confidentiality against a bench'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wap_server.h',
     ' * - Physical BOOT button required for provisioning receipt'),
    ('ios/Sources/SecuraCV/Transport/DeviceAPI.swift',
     '/// The one-shot receipt handed over after a physical BOOT-tap (Trust-on-Pair).'),
    ('desktop/src/app.js',
     '    return "The board refused the stored key — it was probably reflashed elsewhere. One reflash here mints a fresh one.";'),
    ('desktop/src/app.js',
     "    `for the fleet book. Copy it if you want it somewhere else too — a reflash mints a new one.</span>` +"),
    ('desktop/src/app.js',
     '    fix: "A clean install usually clears this — it wipes stale settings a previous " +\n'
     '      "firmware may have left behind. Reconnect and tick first contact.",'),
    ('canary-local/assets/flash-core.js',
     '    fix: "A clean install usually clears this — it wipes stale settings a previous " +\n'
     '      "firmware may have left behind. Reconnect and choose “clean install”.",'),
    ('canary-local/assets/flash.js',
     '      "the device\'s settings page shows it again later, and a reflash mints a new one. " +'),
    ('docs/flasher_profiles_fleet_book.md',
     '- a 401 is surfaced as "the board refused the stored key" (reflashed\n'
     '  elsewhere → reflash here mints a fresh one), never retried blindly.'),
    ('docs/onboarding_multiple_canaries.md',
     'you might misread. Because the handle is derived from immutable hardware, it is\n'
     '**the same every time you flash** — that stability is the point: it\'s how'),
    ('docs/onboarding_multiple_canaries.md',
     'Each board has a globally-unique hardware identity. The firmware derives a\n'
     'stable per-device handle from it (`device_id` = `canary-s3-4dC2`, AP SSID'),
    ('docs/manual_test_plan_captive_portal.md',
     '- **AP SSID** is `SecuraCV-XXXX` (a 4-char suffix derived from the device\'s\n'
     '  hardware identity, encoded in the unambiguous alphabet — no `0/O/o`,'),
    ('docs/manual_test_plan_captive_portal.md',
     '  `1/I/i/l/L`) in both setup and steady state. The softAP is always on\n'
     '  (AP+STA) and does **not** NAT.'),
    ('docs/FAQ.md',
     "network's password, so only a BOOT tap or the token itself fetches it, never\n"
     "the dashboard's own session."),
    ('canary-local/tools/gen_wap.py',
     '         "what": "chip reset — RAM clears, NVS flash survives (your WiFi credentials, keys and witness chain ride through every reset)",'),
    ('canary-local/devices/wap.json',
     '        "what": "chip reset \\u2014 RAM clears, NVS flash survives (your WiFi credentials, keys and witness chain ride through every reset)",'),
    ('docs/review/02-roadmap.md',
     '  Element / ESP32-S3 **eFuse + flash encryption + secure boot** (the secure partition table already\n'
     '  anticipates `nvs_keys` from eFuse). *Acceptance:* device key not recoverable from a config seed.\n'
     '  *Status 2026-09:* the prerequisite is done'),
    ('docs/review/02-roadmap.md',
     '   encryption + secure boot v2**; the `provisioning/partitions_secure.csv` already reserves\n'
     '   `nvs_keys` for this (but fix its 4 MB assumption — F-06).\n'
     '6. **Always keep big buffers in PSRAM** (CSI rings, web scratch).'),
    # #1843's sweep, sixth pass: the remaining copies of the corrected
    # positions, each where it stood (the forms are the text as the pass
    # found it; the replacements are what stands there now).
    ('docs/unflashed_board_intake.md',
     '- [secure_provisioning.md](secure_provisioning.md) — Phase 2 lockdown, if you\n'
     '  want to close the door behind you'),
    ('docs/security/SECURITY_MODEL.md',
     '**If your threat model is an intruder, a landlord, or a domestic abuser\n'
     'who does not take the device to a lab**, the default is the right one for\n'
     'you, and the tamper-evidence properties are unaffected.'),
    ('firmware/PARTITIONS.md',
     '| **Secure / flash‑encrypted** (Phase‑2 provisioning) | 8 MB |'),
    ('firmware/PARTITIONS.md',
     '- The `encrypted` flag is enforced only after the Phase‑2 eFuse burn; in development it is ignored.'),
    ('firmware/PARTITIONS.md',
     '| `provisioning/platformio_secure.ini` (Phase‑2 secure/encrypted) |'),
    ('docs/sd_card_health.md',
     'card, erase the `diag` NVS namespace or reflash with a full chip erase.'),
    ('docs/canary_settings_access_validation.md',
     '| API token fallback | Paste the `cv_...` API token into the auth modal and click Connect. | Modal closes and Settings data loads. Invalid token shows a clear error. |'),
    ('docs/design/usb_onboard.md',
     "- **Recovery** is the witness chain's existing *SD-wins* reconciliation\n"
     '  (`witness_recover_from_sd()`): a factory reset erases NVS settings but never\n'
     '  the SD evidence; on boot the device adopts the SD tail only when it is\n'
     '  strictly ahead and its signature verifies. Re-seat the card, reboot, done.'),
    ('docs/browser_flasher.md',
     '   names the product: "This board already runs X — installing keeps it,\n'
     '   updated in place."'),
    ('docs/browser_flasher.md',
     "the iOS app's `BLEConsole.swift` speaks the same UUIDs). The flasher's"),
    ('docs/design/magic_pairing.md',
     "| credentials vanish at runtime (factory reset) | the window is re-armed from now — the owner's own act |"),
    ('docs/design/magic_pairing.md',
     "*lasts* belongs on such a link. The WAP's bearer token lasts for the life of\n"
     'the device.'),
    ('docs/design/magic_pairing.md',
     'A device nobody owns yet — or, after a reset, one whose owner chose to reopen it:'),
    ('docs/design/iphone_companion_app.md',
     '> Bluefy; a native app erases that footnote), **Secure Enclave key custody**,'),
    ('docs/design/iphone_companion_app.md',
     '   Snapshot char `8fc1cee1-…`, bonded) + chirp/beacon scan — off-grid status.'),
    ('docs/firmware_ota.md',
     'Reconnect via dashboard (canary/WAP), or join the setup network from a phone and enter the new password (vision/sense) — a USB reflash always works too |'),
    ('docs/FAQ.md',
     '`SecuraCV-XXXX` recovery network instead, which needs the key printed for\n'
     "it. So a Canary on your network offers nothing to a stranger's phone, on a\n"
     'good day or a bad one.'),
    ('docs/security/THREAT_MODEL.md',
     '- Private keys are not "access-controlled" — there is no read interface'),
    ('docs/security/THREAT_MODEL.md',
     '3. Ed25519 private key has no read/export interface of any kind'),
    ('docs/design/flasher_experience.md',
     '  self-manifest leaks nothing (no secret ever leaves the device — same rule as the\n'
     '  trust card), so reading it is safe.'),
    ('docs/strategy/08-product-strategy.md',
     'evidently true. SecuraCV is built for the world after both realizations: **your\n'
     "footage is nobody's business, and your proof is unforgeable.**"),
    ('canary-vision/docs/firmware-integration.md',
     'The witness chain signing key pair is generated once during provisioning and stored in NVS. The private key never leaves NVS -- it is read into a stack buffer, used to sign a hash, and immediately zeroed.'),
    ('canary-vision/docs/discovery.md',
     '  device_id=canary-a3f7      # stable device identifier'),
    ('spec/beacon_channel_v0.md',
     'All endpoints Bearer-token-gated identically to `/api/mesh/*` and `/api/bluetooth/*`.'),
    ('spec/canary_mesh_network_v0.md',
     'encryption, at the cost of refusing to work on dev boards without FE — which\n'
     'is the correct trade-off for a production safety system.'),
    ('docs/V1_LAUNCH_REVIEW.md',
     '   **#610**. (Note: by design the live in-RAM session still runs for the current boot on an FE-off\n'
     '   dev board;'),
    ('firmware/LESSONS_LEARNED.md',
     '  credentials vanishing at runtime (factory reset) re-arm the window,'),
    ('firmware/LESSONS_LEARNED.md',
     '  listener, not an active one who paired first. The bearer token lasts for\n'
     '  the life of the device. Nothing that lasts belongs on a link like that.'),
    ('firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md',
     '  - [x] Credential reset path without the serial monitor: one BOOT tap\n'
     '        releases the provisioning receipt (`GET /api/provisioning-receipt`,\n'
     '        one tap = one fetch), which re-reveals the API token and the AP\n'
     '        password; the wizard\'s "Save your recovery kit" block walks it.'),
    ('firmware/projects/canary-wap/README.md',
     'and by **Forget Wi-Fi** or the 10 s BOOT hold on a unit that had\n'
     "credentials, which are the owner's ways to re-arm the window), a phone at the door is a sign of life"),
    ('firmware/projects/canary-wap/README.md',
     "BOOT button keeps the sketch's own gestures: a short press opens the 30 s\n"
     'receipt gate, a 2 s hold prints the identity on serial,'),
    ('firmware/projects/canary-wap/README.md',
     '- **Session Rotation:** Tokens rotate every 4 hours'),
    ('firmware/README.md',
     '- Local-first data storage\n'
     '- Session token rotation'),
    ('ios/README.md',
     '**Dynamic Island / Live Activities**, **CoreBluetooth** (iOS Safari has no Web\n'
     "Bluetooth — the device's own PWA has to send people to Bluefy; native erases\n"
     'that), **HomeKit**,'),
    ('ios/README.md',
     '  waiting out the 20-second cycle. Tamper heard over a BLE console NOTIFY\n'
     '  re-evaluates immediately — sub-second, the fastest transport the fleet\n'
     '  has. And the Test Alert'),
    ('firmware/canary/lib/securacv_witness/src/securacv_witness.cpp',
     '  // The NVS cache persists only every SD_PERSIST_INTERVAL records, so\n'
     '  // after a power cut (or an NVS wipe/reflash while the card kept its\n'
     '  // history) the cached head can be BEHIND the last record actually\n'
     '  // signed — resuming from it would fork the supposedly append-only\n'
     '  // chain.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '// The NVS cache persists only every SD_PERSIST_INTERVAL records, so after\n'
     '// a power cut (or an NVS wipe/reflash while the card kept its history) the\n'
     '// cached head can be BEHIND the last record actually signed — resuming\n'
     '// from it would fork the supposedly append-only chain.'),
    ('firmware/canary/lib/securacv_usb_onboard/src/securacv_usb_onboard.cpp',
     '  Serial.println("  3. To recover a device that lost its NVS head, just re-seat the");\n'
     '  Serial.println("     SD and reboot; the chain head is rebuilt from the SD tail.");'),
    ('firmware/common/network/auth_backoff.h',
     '// token and device_id; an OTA update and the 10 s BOOT reset leave the token\n'
     '// as it was). A Table keeps'),
    ('firmware/projects/canary-wap/arduino/canary_wap/auth_backoff.h',
     '// token and device_id; an OTA update and the 10 s BOOT reset leave the token\n'
     '// as it was). A Table keeps'),
    ('firmware/common/network/improv_core.h',
     '  /// A device with no stored credentials opens the door for this long after\n'
     '  /// power-on (and again after a factory reset wipes them at runtime).'),
    ('firmware/common/network/improv_core.h',
     '/// Provisioned or Provisioning for the phone to read. Credentials vanishing\n'
     '/// at runtime (a factory reset) re-arms the window from now — that is the\n'
     "/// owner's own act. A recovery portal"),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     '  /// A device with no stored credentials opens the door for this long after\n'
     '  /// power-on (and again after a factory reset wipes them at runtime).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     '/// Provisioned or Provisioning for the phone to read. Credentials vanishing\n'
     '/// at runtime (a factory reset) re-arms the window from now — that is the\n'
     "/// owner's own act. A recovery portal"),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     "// once at the door's init (ble_improv_window_used_ms), zeroed by a\n"
     "// credential wipe (wifi_clear_credentials: the owner's own act re-arms the\n"
     "// window, as improv_core.h's session does in RAM). Loop task only after\n"
     '// setup().'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     "  // The owner's own act re-arms the Bluetooth setup door's first-boot\n"
     "  // window: improv_core.h's session"),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     "// meaningful AFTER first-setup. The dashboard's secureFetch already sends\n"
     '// `Authorization: Bearer <api_token>` on all three, but the handlers\n'
     '// themselves were originally registered without an _auth wrapper, so a\n'
     '// LAN-local curl could hit /api/wifi/forget and force a re-provisioning\n'
     '// without authenticating. (/api/wifi'),
    ('firmware/projects/canary-wap/arduino/canary_wap/bluetooth_api.h',
     "// initializes after WiFi is up, so there's no captive-portal flow that\n"
     "// needs to hit these routes without an API token. The dashboard's\n"
     '// secureFetch already attaches `Authorization: Bearer <api_token>` on\n'
     '// every call, so wrapping handlers in api_auth_check is a plumbing-only\n'
     '// fix (mirrors #435/#436 for the wifi-mgmt + mesh families).'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     ' * trusted peers IS sensitive metadata (an attacker who reads the\n'
     " * unencrypted partition learns the household graph). canary-wap's\n"
     ' * opera_config persistence applies the same posture.'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     ' * household-identifying, and gating it would make "off" silently revert\n'
     ' * to "on" at every reboot of an FE-off dev board.'),
    ('firmware/canary/src/csi_modules_integration.cpp',
     "     * we just won't have the hint cached across the next reboot (e.g.\n"
     '     * flash encryption disabled on a dev board). Leave the cache'),
    ('firmware/canary/lib/securacv_network/src/securacv_network.cpp',
     "  // /api/mesh's peers_total, and no NVS is read here (F199: the persisted\n"
     '  // pubkeys needed flash encryption, so a dev board answered 500\n'
     '  // load_failed, and an NVS write'),
    ('firmware/canary/src/main.cpp',
     '   * fails — typical on FE-disabled dev hardware where save refuses\n'
     '   * by design.'),
    ('firmware/scripts/check_canary_mesh_status.py',
     '   (`mesh_state::load_trusted_peers`, refused without flash encryption, so\n'
     '   a dev board answered 500 `load_failed`) and joined them'),
    ('scripts/tests/test_canary_mesh_status_wiring.py',
     "        # F199's review: the dev-board 500 F199 removed, back without a\n"
     '        # mesh_state call.'),
    ('firmware/scripts/regression_check.sh',
     '# NVS is not compatible with flash encryption: ESP-IDF protects it with NVS\n'
     '# encryption (CONFIG_NVS_ENCRYPTION, keys in an nvs_keys partition), and with\n'
     '# flash encryption on,'),
    ('firmware/scripts/regression_check.sh',
     '  blue "  Fix: drop the flag; NVS is protected by NVS encryption (nvs_keys), not by flash encryption"'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_provision.h',
     ' * Replaces (well — augments) the captive-AP setup flow with a phone-pairs-\n'
     ' * Canary, write-credentials-over-BLE ceremony. AP mode is preserved as a\n'
     " * fallback for users who can't / won't use BLE."),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_provision.h',
     ' * - Write rate-limited to 1 / 5 s and capped at 5 attempts per hour to\n'
     ' *   mitigate brute-force from a stolen-bonded-phone scenario.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_provision.h',
     '// 10 attempts/hour gives a fumbling user room to retype a password a\n'
     '// few times without locking the BLE channel out for an hour. The 5 s\n'
     '// per-write cooldown still rate-limits brute-force, and BLE requires\n'
     '// physical proximity, so 10/hour is a comfortable security ceiling.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_console.h',
     ' *   The bond is established via Numeric Comparison (MITM-safe after PR A),\n'
     ' *   so a peer holding the bond has the same trust level as one holding\n'
     ' *   the bearer token over WiFi — exposing the same fields is consistent.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/household_api.h',
     ' * All endpoints require bearer-token auth via api_auth_check (same as the\n'
     ' * rest of the API). Mutations (POST role, override) are scoped to slots\n'
     " * that are ALREADY in the household store — the API can't create new\n"
     ' * paired devices, only adjust roles on devices that completed the BLE\n'
     ' * pairing ceremony. Default role for any new pairing is ROLE_GUEST.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.h',
     ' * from canary_wap.ino, every route behind the Bearer-token trampoline\n'
     ' * (bcn_auth_gated) the /api/mesh and /api/bluetooth routes use.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/beacon_api.h',
     ' * Bearer-gated from day one (see spec/beacon_channel_v0.md §10 and\n'
     ' * docs/audit/mesh_and_chirp_audit_v1.md C12 closure for Chirp). Every route\n'
     ' * is wrapped in the bcn_auth_gated trampoline; an Authorization: Bearer\n'
     ' * <api_token> header is required for any successful response. Mirrors the\n'
     ' * pattern established in bluetooth_api.h (#437) and household_api.h (#438).'),
    ('firmware/projects/canary-wap/tests_host/test_bluetooth_commands_wap.cpp',
     '// never saw the link or the bond. Now both go through the dispatcher: the\n'
     "// passkey reaches the channel and waits for the owner, only the owner's\n"
     '// confirm answers it (once, yes, on the loop task), the bond reaches the\n'
     '// paired list, and Opera still counts the link up and down.'),
    ('firmware/tests_host/test_provisioning_gate.cpp',
     '// pages\' "wait 10 seconds, then click Use BOOT Tap Once again, and only then\n'
     '// short-tap BOOT" prescribes.'),
    ('firmware/tests_host/test_improv_core.cpp',
     '  // The owner wiped the credentials (a factory reset that did not reboot).'),
    ('firmware/tests_host/test_improv_core.cpp',
     "  // Only the owner's tap reopens it."),
    ('firmware/projects/canary-wap/arduino/canary_wap/ble_improv.cpp',
     '      // A door that reopens (a factory reset re-armed the window) is a new\n'
     '      // provisioning: a claim from the last one is void.'),
    ('firmware/projects/canary-wap/tests_host/test_provisioning_logic.cpp',
     "  // Factory reset at runtime: the door opens, but the owner's bonded phone\n"
     '  // is still connected (the dashboard that issued the reset). Held: the\n'
     '  // bonded session keeps the strict profile.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '// Provisioning gate stays open for this long after a BOOT press, so a\n'
     '// browser tab that polls every 2 s reliably catches it and a captive\n'
     "// portal interstitial can't time the user out. The gate is still\n"
     '// single-use: the first successful receipt fetch closes it immediately.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '      Serial.println("[BOOT] Released before 10 s: nothing reset (hold 10 s for a factory reset; Forget Wi-Fi on the dashboard resets the network only)");'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '      Serial.printf("[AUTH] Provisioning gate OPENED (receipt available for %lu seconds)\\n",'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  boot_line("    Press BOOT (<2s)   = receipt gate open 30s; hold 2-3s = print identity");'),
    ('canary-local/tools/gen_wap.py',
     '    "    Press BOOT (<2s)   = receipt gate open 30s; hold 2-3s = print identity",'),
    ('canary-local/devices/wap.json',
     '      "    Press BOOT (<2s)   = receipt gate open 30s; hold 2-3s = print identity",'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wap_server.h',
     'static const char* AP_SSID_PREFIX    = "SecuraCV-";      // + last 4 chars of MAC'),
    ('firmware/projects/canary-wap/arduino/canary_wap/familiar.cpp',
     ' * it in; we hash it into Bloom filters with a device-specific salt so a\n'
     ' * flash dump alone cannot replay which fingerprints were recorded.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/familiar.cpp',
     '// Why salt? Prevents flash-dump recovery of fingerprints across the 2048-\n'
     '// entry search space.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/familiar.h',
     ' *   • Hashes are salted with a per-device secret kept in NVS; flash\n'
     ' *     dumps alone do not reveal which fingerprints were recorded.'),
    ('firmware/common/color/plumage.h',
     "// A device's personality, all 0..255. Derived from the device id; stable for\n"
     '// the life of the device.'),
    ('src/bridge/hap/pairing.rs',
     '//! Canary the answer is "NVS, and only if flash encryption is on" while on a\n'
     '//! hub it is a file — and a long-term pairing key is exactly the kind of\n'
     '//! secret the hardware-root-of-trust tiering exists to govern.'),
    ('src/bridge/hap/store.rs',
     '//! On a Canary the equivalent store is NVS, and the same rule applies one\n'
     '//! layer down: pairing keys get the `opera_secret` treatment and are refused\n'
     '//! on a device without flash encryption.'),
    ('canary-local/assets/flash.js',
     '      "nothing. This app’s window has no Bluetooth of its own, so the check runs in " +\n'
     '      "the SecuraCV iPhone app (its App Store release is still pending), or in the " +\n'
     '      "browser Lab in Chrome or Edge on this computer."));'),
    ('canary-local/assets/flash.js',
     '      "On iPhone or iPad, the SecuraCV app talks to a Canary over Bluetooth " +\n'
     '      "natively — Safari has no Web Bluetooth. On a computer, use a Chromium " +\n'
     '      "browser (Chrome, Edge, Brave).");'),
    ('canary-local/assets/flash.js',
     '          "only (that’s deliberate — same trust as the WiFi token). Pair this " +'),
    ('canary-local/assets/flash.js',
     '      "Counters and fingerprints only — keys, passwords and tokens are never read out of the board, here or anywhere."));'),
    ('canary-local/assets/flash-core.js',
     '    provisioned: !!get("privkey"),           // identity key exists (never read)'),
    ('canary-local/assets/flash-core.js',
     "// firmware/projects/canary-wap/arduino/canary_wap/ble_console.h (the iOS app's\n"
     '// BLEConsole.swift speaks the same UUIDs).'),
    ('canary-local/assets/flash-core.js',
     '        why: `This board already runs ${cur.name} — installing keeps it, ` +\n'
     '             `updated in place.` };'),
    ('canary-local/assets/flash-core.js',
     '    ? "Your settings are reset — the saved WiFi is cleared, so the board comes up with its setup network again, like the first day."'),
    ('desktop/src/app.js',
     '        why: `This board already runs ${cur.name} — installing keeps it, updated in place.` };'),
    ('desktop/src/app.js',
     '// WAP (the browser offers its check for exactly these). This window has no\n'
     '// Bluetooth of its own, so the check is a pointer — to the iPhone app, or to\n'
     '// the browser Lab in a Chromium browser — never a dead end.'),
    ('desktop/src/app.js',
     '  "nothing. This app’s window has no Bluetooth of its own, so the check runs in " +\n'
     '  "the SecuraCV iPhone app (on TestFlight by invite; App Store release pending), or in the " +\n'
     '  "browser Lab in Chrome or Edge on this computer.";'),
    ('desktop/src/app.js',
     '// stays the stable slug written during provisioning; here it just becomes the'),
    ('desktop/src/index.html',
     '                reachable, and writes nothing. This app’s window has no Bluetooth of its\n'
     '                own, so the check runs in the SecuraCV iPhone app (its App Store release is\n'
     '                still pending), or in the browser Lab in Chrome or Edge on this computer.</p>'),
    ('canary-local/tests/desktop_parity.test.js',
     '  assert.match(reassure, /if \\(nativeShell\\) \\{[\\s\\S]*?SecuraCV iPhone app[\\s\\S]*?\\} else \\{[\\s\\S]*?phaseBluetoothCheck/,\n'
     '    "the Lab app\'s Bluetooth entry must point to where the check runs, and only the browser opens it in-page");'),
    ('canary-local/tests/desktop_parity.test.js',
     '    assert.ok(flat.includes("the SecuraCV iPhone app") && flat.includes("in Chrome or Edge on this computer"),\n'
     '      `${label} must name both places the Bluetooth check runs`);'),
    ('desktop/flash-engine/src/changemap.rs',
     '            "Your settings are replaced with the ones you entered here — the board \\\n'
     '             comes up already on that network. Anything it had stored itself \\\n'
     '             (its own tuning, witness-chain counters) starts fresh."'),
    ('desktop/flash-engine/src/changemap.rs',
     '            "Your settings are reset — the saved Wi-Fi is cleared, so the board \\\n'
     '             comes up with its setup network again, like the first day."'),
    ('canary-local/tools/gen_flash.py',
     '          "opens automatically. No secrets ever touch the browser.",'),
    ('canary-local/tools/gen_flash.py',
     '               "console at boot — join it and a setup page opens automatically. No "\n'
     '               "secrets ever touch the browser.",'),
    ('canary-local/devices/flash.json',
     '      "provisioning_note": "Power it on near the SecuraCV iPhone app and tap the card that appears — it hands over your WiFi in one tap. No iPhone? It also raises a WiFi network of its own (SecuraCV-XXXX) with a key it prints on its serial console at boot — join it and a setup page opens automatically. No secrets ever touch the browser.",'),
    ('canary-local/devices/flash.json',
     '      "provisioning_note": "When it boots it raises a WiFi network of its own (SecuraCV-XXXX) with a key it prints on its serial console at boot — join it and a setup page opens automatically. No secrets ever touch the browser.",'),
    ('canary-local/assets/hatchery.js',
     "// hatches (first successful flash). The name is WHIMSY ONLY: the device's\n"
     '// functional id stays its stable provisioned slug; this is a display/certificate\n'
     '// layer, the same one the native app mints.'),
    ('canary-local/devices/hatch.json',
     '  "$doc": "The Hatchery — the single source of truth for how a Canary is named and certified when it hatches. Shared verbatim by the website Lab (fetched from GitHub Pages) and the desktop flasher (embedded at build time), so the birthing moment is identical on both and stays in sync when this file changes. Pure data: the tiny assembly logic (pick a title with title_chance, first + house, Nth-of-its-name ordinal) is mirrored in both clients. Names are whimsy only — the device\'s functional id stays a stable slug; this is a display/certificate layer.",'),
    ('canary-local/assets/wap-ui.js',
     '        : "app running — BOOT does nothing until the next reset";'),
    ('canary-local/assets/wap-ui.js',
     '    if (held && plugged && mode === "run") say("(BOOT held — nothing happens: it\'s only sampled at reset)");'),
    ('ios/Sources/SecuraCV/App/FleetStore.swift',
     '        // Tamper heard over a BLE NOTIFY is the fastest signal the fleet\n'
     '        // has — sub-second, no polling anywhere in the path. Re-fold and\n'
     '        // re-evaluate NOW; waiting out the 20-second cycle would squander\n'
     '        // the one transport that pushes.'),
    ('ios/Sources/SecuraCV/App/FleetStore.swift',
     '        // ── Tier 1a: the WAP-class GATT console snapshot ──'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     '//   2. GATT CONSOLE (WAP-class enrichment). canary-wap additionally serves a\n'
     '//      connectable console with a richer JSON snapshot. We connect only when a\n'
     '//      peripheral actually advertises that service.'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     '/// A compact snapshot pushed by a WAP-class device over BLE NOTIFY.'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     "    /// Fires the moment a connected console reports tamper that wasn't\n"
     '    /// there a beat ago — BLE NOTIFY is sub-second, so the alert loop gets\n'
     '    /// to run NOW instead of at the next poll. Wired by FleetStore.'),
    ('ios/Sources/SecuraCV/Transport/BLEConsole.swift',
     '            // Tamper over BLE NOTIFY is the fastest signal the fleet has —\n'
     '            // hand it to the alert loop now, not at the next poll.'),
    ('ios/Sources/SecuraCV/Transport/FleetWiFiRunner.swift',
     '        for c in plan.unreachable {\n'
     '            steps[c.id] = .failed("Not reachable right now — over Wi-Fi or Bluetooth.")\n'
     '        }'),
    ('ios/Sources/SecuraCV/Transport/FleetWiFiRunner.swift',
     '            steps[candidate.id] = .failed("Not reachable right now — over Wi-Fi or Bluetooth.")'),
    ('ios/Tests/SecuraCVTests/FleetWiFiRolloutTests.swift',
     '    func testADarkWAPInRangeRidesTheBLERescue() {'),
    ('ios/Tests/SecuraCVTests/FleetWiFiRolloutTests.swift',
     '    func testADarkOutOfRangeWAPIsUnreachable() {'),
    ('ios/Sources/SecuraCV/Model/CanaryVoiceStage.swift',
     '            line = "A Canary’s signing key is pinned the first time you meet it — from then on, nothing else speaks for it."'),
    ('ios/Sources/SecuraCV/Model/Witness.swift',
     '    var id: String                       // stable device_id, e.g. "canary-a3f7"'),
    ('ios/Support/Info.plist',
     '  <string>SecuraCV connects to your Canaries over Bluetooth so you still get status if the network drops, and hands a brand-new Canary your Wi-Fi when you set it up.</string>'),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     "            : 'Your session does not cover this, or has ended. Paste the API token, or use BOOT Tap Once.');"),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     'Paste the token from the device receipt or Serial output. It starts with <code>cv_</code>. Type it exactly as shown.</p>'),
    ('firmware/projects/canary-wap/arduino/canary_wap/web_ui.h',
     '      // than this budget, so a press anywhere in the 60 s window lands.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/recovery_kit.test.js',
     "const NO_TOKEN_REFUSAL = 'Your session does not cover this, or has ended. Paste the API token, or use BOOT Tap Once.';"),
    ('firmware/projects/canary-wap/arduino/canary_wap/recovery_kit.test.js',
     "  it('the old order still works: pressed first, the first ask is served', async () => {"),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     ' * - iOS: requires the Bluefy browser (App Store) — Safari and iOS Chrome\n'
     ' *   deliberately omit Web Bluetooth from WebKit. Bluefy wraps WKWebView\n'
     ' *   and exposes the same JS API; the user pairs the Canary in iOS\n'
     ' *   Settings > Bluetooth first, then opens this page in Bluefy.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     '  iOS: install <a href="https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055" target="_blank" rel="noopener">Bluefy</a> for Web Bluetooth support.<br>\n'
     '  Android &amp; desktop Chrome work natively.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     "      return { show: true, text: 'This browser has no Web Bluetooth. On iOS, install Bluefy from the App Store to use the Bluetooth console.' };"),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     "    showErr('Web Bluetooth not supported by this browser. On iOS, install Bluefy.');"),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     '  // out nothing; the old order (press first, then Save) is served on the\n'
     '  // first ask. The banner says'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wizard_logic.test.js',
     "  it('missing Web Bluetooth points at Bluefy', () => {\n"
     '    const n = L.capabilityNotice(false, true, false);\n'
     '    assert.equal(n.show, true);\n'
     '    assert.match(n.text, /Bluefy/);\n'
     '  });'),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     '// (1) insecure context → tell them to use HTTPS; (2) no Web Bluetooth\n'
     '// API → tell iOS users to install Bluefy.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/companion_pwa.h',
     '    // BLE-console chrome — hide the Bluefy footer along with the banner.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  // Every endpoint Bearer-token-gated via the chirp_api template trampoline.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  // Bearer-gated from day one (see spec/beacon_channel_v0.md §10).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/beacon_api.h',
     '// GET /api/beacon/audit — paged audit log (Bearer-gated).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.cpp',
     ' * (beacon_api.h, Bearer-gated, from canary_wap.ino); the runtime loop is'),
    ('firmware/projects/canary-wap/arduino/canary_wap/chirp_api.h',
     '// Register all chirp API routes with the HTTP server, Bearer-gated.'),
    ('docs/security/THREAT_MODEL.md',
     '  - REST API (`/api/chirp/*`) Bearer-token-gated when wired into\n'
     '    `canary_wap.ino` (audit C12).'),
    ('docs/security/THREAT_MODEL.md',
     '  routes are registered (behind `FEATURE_BEACON_CHANNEL`, Bearer-gated), but'),
    ('spec/chirp_channel_v0.md',
     '  (C11); REST API Bearer-gated (C12); presence requirement on ACK origination'),
    ('CHANGELOG.md',
     '  app descriptor already on the board ("This board already runs X —\n'
     '  installing keeps it, updated in place"); else the chip **plus the'),
    ('docs/audit/repo_todo_sweep_2026-09.md',
     '  table; on a dev board without flash encryption every peers read fails, and'),
    # #1843's sweep, sixth pass, round-1 review: the copies the pass
    # missed and the replacements it wrote that were themselves untrue
    # (the tap, a cleared unit's owner, the WAP's gate, the old card, the
    # released framework, the console's board, a restore's token).
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '// The BOOT tap stays open this long for the one page already asking:'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '      Serial.printf("[AUTH] Provisioning gate OPENED for %lu seconds: the receipt goes only to the one address already asking (ask first, then tap)\\n",'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  boot_line("    Press BOOT (<2s)   = receipt to the one page already asking (30s); hold 2-3s = print identity");'),
    ('canary-local/tools/gen_wap.py',
     '    "    Press BOOT (<2s)   = receipt to the one page already asking (30s); hold 2-3s = print identity",'),
    ('canary-local/tools/gen_wap.py',
     '             f"{GATE_TTL_MS // 1000} s and hands the recovery kit to the one page already asking for it "'),
    ('canary-local/tools/gen_wap.py',
     'must_words(DOC, "On a WAP it hands the recovery kit only to the one page already asking for it, "'),
    ('canary-local/devices/wap.json',
     '      "    Press BOOT (<2s)   = receipt to the one page already asking (30s); hold 2-3s = print identity",'),
    ('canary-local/devices/wap.json',
     '          "short press (under 2 s): opens the provisioning gate for 30 s and hands the recovery kit to the one page already asking for it (click Use BOOT Tap Once on the dashboard first; it prints no key)",'),
    ('firmware/projects/canary-wap/README.md',
     'receipt gate for the one page already asking for it (click Use BOOT Tap Once'),
    ('docs/getting_started_canary.md',
     'dashboard within the 30 seconds. On a WAP it hands the recovery kit only\n'
     'to the one page already asking for it, so click **Use BOOT Tap Once** on the'),
    ('docs/design/magic_pairing.md',
     "sketch's own gestures. A short press opens the 30 s receipt gate, a 2 s hold"),
    ('canary-local/assets/wap-ui.js',
     '        : "app running — the ROM ignores BOOT until the next reset, but the firmware reads it (a tap opens the receipt gate, 10 s held is a factory reset)";'),
    ('canary-local/assets/wap-ui.js',
     '    if (held && plugged && mode === "run") say("(BOOT held — the ROM only samples it at reset, but the running WAP counts it: kept 10 s, it clears Wi-Fi, broker and name and reopens its first-come Bluetooth setup door for half an hour)");'),
    ('firmware/projects/canary-wap/arduino/canary_wap/recovery_kit.test.js',
     "    // The tap goes to an address that is already asking, and the page's"),
    ('CHANGELOG.md',
     '  press hands it to the one page already asking, so click **Use BOOT Tap'),
    ('firmware/common/network/improv_core.h',
     " *   owner cleared), and for an owner's own tap — never for a device that"),
    ('firmware/common/network/improv_core.h',
     ' *   - NO CREDENTIALS STORED (first boot, or after the owner cleared them:'),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     " *   owner cleared), and for an owner's own tap — never for a device that"),
    ('firmware/projects/canary-wap/arduino/canary_wap/improv_core.h',
     ' *   - NO CREDENTIALS STORED (first boot, or after the owner cleared them:'),
    ('docs/design/magic_pairing.md',
     "> yet, or one its owner cleared), and for an owner's own tap — never for a"),
    ('docs/design/magic_pairing.md',
     'A unit its owner cleared (the 10 s hold, or Forget Wi-Fi on a WAP) still has'),
    ('docs/design/magic_pairing.md',
     "seeded), and restarts into first-boot setup when BOOT is released. A WAP's door is the first-boot window alone. The owner re-arms it\n"
     'with **Forget Wi-Fi** on the dashboard (`POST /api/wifi/forget`) or with the'),
    ('docs/design/magic_pairing.md',
     'While the door is **open** (a new unit, one its owner cleared with the hold\n'
     'or Forget Wi-Fi, or the owner tapped it):'),
    ('docs/design/magic_pairing.md',
     "from the owner's: the first is a device with no network saved (a new one, or\n"
     'one its owner cleared, §2.2), the second is a device that has an owner and a'),
    ('docs/ble_protocol.md',
     "From `improv_core.h`: the door is open for a device with NO NETWORK SAVED (a new one nobody owns yet, or one its owner cleared), and for an owner's own tap — never for a device that has an owner and a bad day."),
    ('docs/ble_protocol.md',
     "- **No credentials stored** (first boot, or after the owner cleared them with the 10 s factory-reset hold or, on a WAP, Forget Wi-Fi): open for a first-boot window of **30 minutes** after power-on (`IMPROV_FIRST_BOOT_WINDOW_MS`; a power cycle re-arms it, and so does clearing the credentials; compile-time 0 = tap-only). A cleared unit still has an owner: the hold keeps the witness identity on a Sense, a Vision and a WAP, and a WAP's hold and Forget Wi-Fi keep its token too. The door still goes to whichever client connects first, so for that half hour a stranger in radio range can give it their network ([`design/magic_pairing.md`](design/magic_pairing.md) §2.2; on a WAP, ledger F345)."),
    ('docs/GLOSSARY.md',
     'device with no network saved (a new one nobody owns yet, or one its owner\n'
     "cleared), and for an owner's own tap — never for a device that has an owner"),
    ('docs/GLOSSARY.md',
     'does the owner clearing the credentials with the 10 s factory-reset hold'),
    ('docs/FAQ.md',
     'device with no network saved (a new one nobody owns yet, or one its owner\n'
     "cleared), and for an owner's own tap — never for a device that has an owner"),
    ('docs/README.md',
     "- [**Magic pairing — the Bluetooth setup door (design)**](design/magic_pairing.md) — built, host-tested, **not bench-tested**: a new Sense, Vision or WAP takes its Wi-Fi from the iPhone app with one tap over the open Improv Wi-Fi standard. The door rule (open for a device with no network saved — a new one nobody owns yet, or one its owner cleared, which still has an owner — for 30 minutes after power-on, to whichever client connects first, or for 60 s after the owner's own BOOT tap; never because a saved network is failing), the bounds a stranger in radio range runs into, the WAP's claim ticket that gets the phone paired without the bearer token ever riding Bluetooth, the UUIDs and byte layouts, the per-family lifecycle, the iOS flow, the residual risks and the bench checklist"),
    ('docs/TRUST_MODEL.md',
     'in its 30-minute window while no network is saved (a first boot, or after\n'
     'the owner cleared it) and in the BOOT-tap minute. A WAP also takes it over'),
    ('firmware/LESSONS_LEARNED.md',
     "  new one nobody owns yet, or one its owner cleared), and for an owner's own\n"
     '  tap". The 10 s hold, and Forget Wi-Fi on a WAP, clear the network but keep'),
    ('firmware/projects/canary-sense/README.md',
     'saved — a new one nobody owns yet, or one its owner cleared — and for an\n'
     "owner's own tap; never for a device that has an owner and a bad day):"),
    ('firmware/projects/canary-vision/README.md',
     'network saved (a new one nobody owns yet, or one its owner cleared), and\n'
     "for an owner's own tap — never for a device that has an owner and a bad"),
    ('docs/firmware_ota.md',
     'All endpoints are bearer-token gated on canary and canary-wap. Since 2026-08'),
    ('firmware/common/network/node_status.h',
     ' * two trees served a rich bearer-gated status and four served none.'),
    ('firmware/common/network/node_status.h',
     ' * canary and the WAP, whose /api/status is bearer-gated and richer, these'),
    ('firmware/projects/canary-wap/arduino/canary_wap/node_status.h',
     ' * two trees served a rich bearer-gated status and four served none.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/node_status.h',
     ' * canary and the WAP, whose /api/status is bearer-gated and richer, these'),
    ('firmware/projects/canary-display/arduino/canary_display/node_status.h',
     ' * two trees served a rich bearer-gated status and four served none.'),
    ('firmware/projects/canary-display/arduino/canary_display/node_status.h',
     ' * canary and the WAP, whose /api/status is bearer-gated and richer, these'),
    ('docs/PARITY.md',
     '| **canary-wap** | ✅ | ✅ bearer-gated ¹⁰ | ⚠️ ¹¹ | ⚠️ ¹² | ❌ ¹³ | ❌ ¹³ | ✅ | ✅ | ✅ | ✅ |'),
    ('docs/PARITY.md',
     '10. **Gated, with the same body.** The WAP and the PIO canary answer\n'
     '    `/api/status` behind their bearer token (they also serve a rich, gated'),
    ('docs/PARITY.md',
     '  rides inside the bearer-gated status on the two that already had one. The'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  // Rename this device → canary-<name>.local (Bearer-gated).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '  // Authenticated API endpoints (Bearer token required)'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     '// These wrappers add Bearer token authentication to existing handlers.'),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     "// pair_token_valid. The dashboard's secureFetch already supplies the"),
    ('firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino',
     ' * Bearer-gated mDNS browser for the operator UI; this one is presence-only and'),
    ('firmware/projects/canary-wap/arduino/canary_wap/airtime_governor.cpp',
     '//     `cosign_pending_request` REST path (Bearer-gated, same task).'),
    ('firmware/projects/canary-wap/arduino/canary_wap/chirp_api.h',
     ' *   - register_routes() now Bearer-gates every endpoint via the'),
    ('firmware/projects/canary-wap/arduino/canary_wap/household_api.h',
     "// The dashboard's secureFetch already attaches `Authorization: Bearer"),
    ('firmware/projects/canary-wap/arduino/canary_wap/api_auth.h',
     ' * Provides Bearer token authentication for the REST API with:'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wap_server.h',
     ' * - Bearer token authentication on protected endpoints'),
    ('firmware/projects/canary-wap/arduino/canary_wap/csi_integration.cpp',
     '#include "api_auth.h"             // api_auth_check() — Bearer token gate'),
    ('firmware/projects/canary-wap/arduino/canary_wap/wap_diagnostics.h',
     ' * The route (canary_wap.ino handle_diagnostics, token-gated,'),
    ('firmware/projects/canary-wap/tests_host/check_route_security.py',
     '    # (that stays behind the Bearer-gated /api/fleet-scan and the break-glass'),
    ('docs/FIRMWARE_MAP.md',
     '  device-published discovery; bearer-gated `/api/ota/{status,check,install,config}`\n'
     '  on canary and WAP; LAN-open `/api/ota/{status,check,install}` on the'),
    ('docs/homeassistant_setup.md',
     'return `csi_event_egress` from their token-gated `GET /api/diagnostics`'),
    ('docs/manual_test_plan_captive_portal.md',
     'their bearer-gated `/api/status` under `node_status`).'),
    ('docs/canary_settings_access_validation.md',
     '- WiFi access alone does not authorize settings API calls.'),
    ('spec/witness_api_v1.md',
     "  MQTT `chain` publish, and `uptime_s` is `/api/status`'s `uptime_sec`, all\n"
     '  behind the same Bearer token. The bucket itself adds no precision finer'),
    ('spec/witness_api_v1.md',
     'holder of the Bearer token can confirm a guess at a low-entropy payload — a'),
    ('tvos/discovery/DISCOVERY.md',
     '  sealed evidence stays behind the Bearer-gated `/api/fleet-scan` and'),
    ('ios/Sources/SecuraCV/Transport/DeviceAPI.swift',
     "    /// `GET /api/status` — the WAP's Bearer-gated status body, decoded down"),
    ('ios/Sources/SecuraCV/Transport/DeviceAPI.swift',
     '    /// first, then committed ring rows newest-first, ≤72 rows, Bearer-gated;'),
    ('ios/Sources/SecuraCV/Transport/DeviceAPI.swift',
     '    /// SSE; Bearer-gated; Wire/WapEvents.swift documents its three variant'),
    ('ios/Sources/SecuraCV/Views/Components/SensingNowSection.swift',
     '    /// (the stream is Bearer-gated; it would only 401 forever).'),
    ('ios/Sources/SecuraCV/Views/DeviceDetailView.swift',
     '            // (the stream is Bearer-gated; an unpaired row or a Keychain-'),
    ('ios/Sources/SecuraCV/Wire/WapEvents.swift',
     '// 64 rows, Bearer-token-gated (csi_integration.cpp:handle_events_today).'),
    ('ios/Sources/SecuraCV/Wire/WapEvents.swift',
     '/// `GET /api/csi/stream` — a plain 1 Hz GET snapshot (not SSE), Bearer-gated,'),
    ('ios/Tests/SecuraCVTests/WapEventsTests.swift',
     '        // open the sensing feed to anyone on the LAN — a privacy regression'),
    ('ios/Tests/SecuraCVTests/WapEventsTests.swift',
     '                      "events/today lost its auth gate — the feed must stay token-gated")'),
    ('CHANGELOG.md',
     '  (`witness_page.h`, `handle_witness_v1`, Bearer-gated, chunked) as'),
    ('CHANGELOG.md',
     '- **Beacon REST API** (`beacon_api.h`): Bearer-token-gated surface —'),
    ('CHANGELOG.md',
     '  template-trampoline pattern; wired into `canary_wap.ino`.'),
    ('docs/audit/mesh_and_chirp_audit_v1.md',
     '**Fix in this branch:** register `chirp_api::register_routes()` in `canary_wap.ino` using the same Bearer-token template trampoline as `bluetooth_api.h` (PR #437) and `household_api.h` (PR #438). `POST /api/chirp/send` additionally requires a 2-second hold timestamp from the UI per `spec/chirp_channel_v0.md` §4.1.'),
    ('docs/audit/v0.3_closeout.md',
     '| C12 | High (latent) | Bearer-token template trampoline; wired in INO | `chirp_api.h::chirp_auth_gated<>` | manual: cURL with/without auth |'),
    ('docs/audit/v0.3_closeout.md',
     '- **Bearer-token-gated REST surface** on `/api/beacon/*`, wired into'),
    ('docs/design/iphone_companion_app.md',
     "> Bluetooth — the device's own `/companion` PWA has to tell people to install"),
    ('docs/design/usb_onboard.md',
     "  flasher, gives a new key, so the old card's chain stays verifiable\n"
     '  off-device and the device starts a new one.'),
    ('firmware/canary/lib/securacv_usb_onboard/src/securacv_usb_onboard.cpp',
     '  Serial.println("     device has a new key: the old card is kept, not adopted.");'),
    ('firmware/canary/lib/securacv_witness/src/securacv_witness.cpp',
     '// history is ahead of ours is refused for the same reason, instead of'),
    ('firmware/canary/lib/securacv_witness/src/securacv_witness.cpp',
     '    return sd_append_fail("SD history ahead of this chain - reboot to reconcile");'),
    ('firmware/PARTITIONS.md',
     '  (every one is `framework = arduino`), so NVS is plaintext on every tier.'),
    ('firmware/scripts/regression_check.sh',
     '# no Canary build has (every one is framework = arduino, so NVS is plaintext on'),
    ('firmware/scripts/regression_check.sh',
     '  blue "  Fix: drop the flag; only NVS encryption (nvs_keys) could protect NVS, and it is unavailable under framework = arduino, so NVS stays plaintext either way"'),
    ('firmware/provisioning/partitions_secure.csv',
     '#   Canary build reads it (every one is framework = arduino, where NVS'),
    ('firmware/canary/lib/securacv_mesh/src/mesh_state.h',
     " * flash learns the household graph either way. canary-wap's"),
    ('desktop/src/app.js',
     '// The read-only Bluetooth console runs on the AP-provisioned Canaries and the'),
    ('canary-local/assets/flash.js',
     '  // AP-based Canaries (canary / WAP) run the read-only Bluetooth console, so'),
    ('docs/browser_flasher.md',
     'is the cleaner cross-board "it\'s a *branded* Canary" signal — every board, C3/C6\n'
     'included, advertises `SecuraCV-Canary` by name.'),
    ('docs/browser_flasher.md',
     'after flashing an AP/WAP board) answers three plain questions with **Web'),
    ('firmware/projects/canary-wap/README.md',
     "- **Session Rotation:** the RF presence detector's ephemeral session tokens rotate every 4 hours. The API token does not rotate: the 10 s hold and Forget Wi-Fi keep it, and only an install or a full erase in a flasher replaces it"),
    ('docs/design/magic_pairing.md',
     'is installed again or fully erased in a flasher: OTA updates, the 10 s hold'),
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
    texts: dict[str, str] = {}
    for rel, needle, _why in RETIRED_IN:
        if rel not in texts:
            texts[rel] = normalized((root / rel).read_text(encoding="utf-8"))
        if needle in texts[rel]:
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
        for rel, s in MUST_PASS_IN:
            with self.subTest(rel=rel, s=s):
                self.assertEqual(retired_in_offenders(s), [])
        for rel, s in MUST_FAIL_IN:
            with self.subTest(rel=rel, s=s):
                self.assertNotEqual(retired_in_offenders(s), [], "the ban lost its teeth")

    def test_every_scoped_row_is_listed_once(self):
        rows = [(rel, needle) for rel, needle, _why in RETIRED_IN]
        self.assertEqual(len(rows), len(set(rows)), "a (file, needle) row is listed twice")

    def test_every_retired_form_trips_a_row_of_its_own_file(self):
        # A row deleted or pointed at another file leaves its form here with
        # no row of that file to trip: a sibling row elsewhere with the same
        # needle does not count.
        for rel, s in MUST_FAIL_IN:
            flat = normalized(s)
            with self.subTest(rel=rel, s=s):
                self.assertTrue(any(r == rel and needle in flat for r, needle, _why in RETIRED_IN),
                                f"no RETIRED_IN row for {rel} matches this retired form")

    def test_every_retired_form_belongs_to_one_row(self):
        # A form that two rows of one file both match would leave either row
        # free to go: the other still trips it. One form, one row.
        for rel, s in MUST_FAIL_IN:
            flat = normalized(s)
            with self.subTest(rel=rel, s=s):
                self.assertEqual(len([n for r, n, _w in RETIRED_IN if r == rel and n in flat]), 1,
                                 f"{rel}: this form matches more than one row of its file; split it")

    def test_every_scoped_row_has_its_own_retired_form(self):
        for rel, needle, _why in RETIRED_IN:
            with self.subTest(rel=rel, needle=needle):
                self.assertTrue(any(r == rel and needle in normalized(s) for r, s in MUST_FAIL_IN),
                                f"{rel}: this row has no MUST_FAIL_IN form of its own file")


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

    def test_every_scoped_file_carries_its_replacement(self):
        # The positive anchor: the replacement stands in the file the row
        # names, so a row pointed at a file that never held the sentence
        # (README.md, say) has nothing there to stand on.
        anchored = {rel for rel, _s in MUST_PASS_IN}
        for rel, needle, _why in RETIRED_IN:
            with self.subTest(rel=rel, needle=needle):
                self.assertIn(rel, anchored, f"{rel}: no MUST_PASS_IN replacement names this file")
        texts: dict[str, str] = {}
        for rel, s in MUST_PASS_IN:
            if rel not in texts:
                texts[rel] = normalized((REPO / rel).read_text(encoding="utf-8"))
            with self.subTest(rel=rel, s=s):
                self.assertTrue(normalized(s).strip() in texts[rel],
                                f"{rel}: the replacement no longer stands there")

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

    def test_a_wap_move_keeps_the_old_networks_and_only_a_forget_drops_them(self):
        # #1843's fourth review: the docs say the owner's browser moves the
        # WAP off a striking host's network only while the new network stays
        # up, because the WAP keeps the networks it joined and moves on to
        # them in turn, and that no route forgets one network, so only
        # Forget Wi-Fi or the hold drop them, each re-arming the setup door.
        # Each half held to the code: a route that forgets one network, or a
        # move that drops the ring, fails here until the docs follow.
        ino = (WAP / "canary_wap.ino").read_text(encoding="utf-8")
        store = (WAP / "wifi_store.h").read_text(encoding="utf-8")
        # The ring keeps up to four, and a new network takes a free slot.
        self.assertIn("static constexpr uint8_t MAX_NETWORKS = 4;", store)
        add = body_after(store, "inline bool add(Store& s, const char* ssid, const char* pass) {", r"\n}\n")
        self.assertIn("if (s.count < MAX_NETWORKS) {\n      idx = s.count++;", add)
        # A proven join adds to the ring; a save (the connect route's, the
        # lane's) writes the current pair only.
        joined = body_after(ino, "static void wifi_store_note_joined(const char* ssid, const char* pass) {", r"\n}\n")
        self.assertIn("canary::net::wifi_store::add(g_wifi_store, ssid, pass)", joined)
        for sig in ("static bool wifi_save_credentials() {",
                    "static esp_err_t handle_wifi_connect(httpd_req_t* req) {",
                    "bool ble_request_wifi_provisioning(const char* ssid, const char* password) {"):
            with self.subTest(fn=sig):
                self.assertNotIn("wifi_store", body_after(ino, sig, r"\n}\n"))
        # Two NotFounds running move the retry on to the next saved network.
        rotate = body_after(ino, "static void wifi_store_rotate_if_due() {", r"\n}\n")
        self.assertIn("g_wifi_notfound_streak < 2", rotate)
        self.assertIn("canary::net::wifi_store::pick(g_wifi_store, nullptr, nullptr, 0, cur)", rotate)
        self.assertIn("wifi_store_adopt(next);", rotate)
        failed = body_after(ino, "    case WIFI_PROV_FAILED:\n      // Attempt reconnection periodically", r"\n      break;\n")
        self.assertLess(failed.index("wifi_store_rotate_if_due();"), failed.index("wifi_connect_to_home();"))
        # No route forgets one network: nothing in the sketch calls remove().
        for f in sorted(WAP.iterdir()):
            if f.suffix in (".ino", ".cpp", ".h"):
                with self.subTest(file=f.name):
                    self.assertNotIn("wifi_store::remove(", f.read_text(encoding="utf-8", errors="replace"))
        # Forget Wi-Fi and the hold drop the whole ring, and re-arm the door.
        clear = body_after(ino, "static bool wifi_clear_credentials() {", r"\n}\n")
        self.assertIn("nvs->remove(canary::net::wifi_store::NVS_KEY);", clear)
        self.assertIn("canary::net::wifi_store::init(g_wifi_store);", clear)
        self.assertIn("g_improv_window_magic = 0;", clear)
        for sig in ("static esp_err_t handle_wifi_forget(httpd_req_t* req) {",
                    "static void factory_reset_by_hold() {"):
            with self.subTest(fn=sig):
                self.assertIn("\n  wifi_clear_credentials();", body_after(ino, sig, r"\n}\n"))

    def test_nvs_encryption_is_in_no_released_build(self):
        # #1843's fourth and fifth reviews: the docs say the key, the saved
        # passwords and the bonds are plaintext in NVS on every tier, a fused
        # board included, because flash encryption does not cover NVS and no
        # released build can turn NVS encryption on. Held to the build files:
        # a tree that moves to ESP-IDF, or a secure env whose sdkconfig starts
        # to reach a build, fails here until the docs follow.
        frameworks = {}
        for ini in sorted((REPO / "firmware").rglob("*.ini")):
            for line in ini.read_text(encoding="utf-8").splitlines():
                m = re.match(r"\s*framework\s*=\s*(\S+)", line)
                if m:
                    frameworks[str(ini.relative_to(REPO))] = m.group(1)
        # canary-ota is the teaching sample (its README says so), not a
        # released tree.
        self.assertEqual(frameworks.pop("firmware/projects/canary-ota/platformio.ini"), "espidf")
        self.assertIn("firmware/envs/platformio/common.ini", frameworks)
        self.assertIn("firmware/canary/platformio.ini", frameworks)
        self.assertEqual(set(frameworks.values()), {"arduino"}, frameworks)
        ota = (REPO / "firmware/projects/canary-ota/README.md").read_text(encoding="utf-8")
        self.assertIn("**This is a teaching sample, not the production OTA path.**", ota)
        # The secure env's sdkconfig, the only place NVS encryption is asked
        # for, is marked inert under that framework ...
        secure = (REPO / "firmware/provisioning/platformio_secure.ini").read_text(encoding="utf-8")
        self.assertIn("; KNOWN INERT under `framework = arduino`", secure)
        self.assertIn("So this image has flash encryption at most, never NVS\n; encryption", secure)
        # ... and its partition table leaves nvs unflagged, so flash
        # encryption leaves it in plaintext.
        table = (REPO / "firmware/provisioning/partitions_secure.csv").read_text(encoding="utf-8")
        nvs = [ln for ln in table.splitlines() if ln.startswith("nvs,")]
        self.assertEqual(len(nvs), 1)
        self.assertNotIn("encrypted", nvs[0])


    def test_the_secure_kit_turns_on_neither_secure_boot_nor_flash_encryption(self):
        # #1843's sweep, second pass: the docs say the provisioning kit's
        # Phase 2 burns fuses but turns on neither protection (a plaintext
        # image, no SPI_BOOT_CRYPT_CNT, no Secure Boot key digest), and that
        # its own lockdown check then fails. Held to the kit: a Phase 2 that
        # starts to burn the counter or a digest, or to encrypt the image,
        # fails here until THREAT_MODEL, SECURITY_MODEL, secure_provisioning
        # and the flasher's trust card follow.
        kit = REPO / "firmware/provisioning"
        sh = (kit / "provision_canary.sh").read_text(encoding="utf-8")
        burn = body_after(sh, "    EFUSES_TO_BURN=(", r"\n    \)\n")
        self.assertEqual(re.findall(r'"(\w+)"', burn),
                         ["SECURE_BOOT_EN", "SOFT_DIS_JTAG", "DIS_USB_JTAG", "DIS_PAD_JTAG",
                          "DIS_DOWNLOAD_MANUAL_ENCRYPT"])
        for fuse in ("SPI_BOOT_CRYPT_CNT", "FLASH_CRYPT_CNT", "SECURE_VERSION",
                     "DIS_DOWNLOAD_MODE", "SECURE_BOOT_AGGRESSIVE_REVOKE"):
            with self.subTest(fuse=fuse):
                self.assertNotIn(f"burn_efuse {fuse}", sh)
                self.assertNotIn(f'"{fuse}"', burn)
        for step in ("burn_key_digest", "--encrypt", "secure_boot_signing_key.pem BLOCK",
                     "espsecure"):
            with self.subTest(step=step):
                self.assertNotIn(step, sh)
        self.assertIn('python3 -m esptool --port "${PORT}" write_flash 0x10000 "${FW_PATH}"', sh)
        self.assertEqual(sh.count("burn_key "), 2, "the dry-run echo and the one key burn")
        self.assertIn('burn_key BLOCK_KEY0 \\\n        "${KEYS_DIR}/flash_encryption_key.bin" XTS_AES_256_KEY', sh)
        # The sdkconfig that would turn them on has them commented out.
        cfg = (kit / "sdkconfig.defaults.secure").read_text(encoding="utf-8")
        for key in ("CONFIG_SECURE_BOOT=y", "CONFIG_SECURE_BOOT_V2_ENABLED=y",
                    "CONFIG_SECURE_FLASH_ENC_ENABLED=y", "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=y"):
            with self.subTest(key=key):
                self.assertIn("# " + key, cfg)
                self.assertNotIn("\n" + key, cfg)
        # And the post-check expects three fuses the burn never sets, so it fails.
        verify = (kit / "verify_device.py").read_text(encoding="utf-8")
        self.assertIn('"SPI_BOOT_CRYPT_CNT": {\n        "virgin": 0,\n        "locked": lambda v: v in [1, 3, 5, 7],', verify)
        self.assertIn('"DIS_DOWNLOAD_MODE": {\n        "virgin": 0,\n        "locked": 1,', verify)
        self.assertIn('"SECURE_BOOT_AGGRESSIVE_REVOKE": {\n        "virgin": 0,\n        "locked": 1,', verify)
        self.assertIn('verify_device.py" --port "${PORT}" --expect-locked', sh)

    def test_no_boot_hold_clears_the_mesh_or_scout_keys(self):
        # The same pass: mesh_state.h, main.cpp and ble_scout_key.h say no
        # factory reset clears the opera secret, the peers, the elected hub
        # or the scout key: the flagship's 10 s reset keeps them (they are not
        # in its key list), its 30 s wipe drops the namespace, the WAP's hold
        # touches none, and only POST /api/mesh/leave and the rotation's
        # failure path clear the secret.
        scope = (REPO / "firmware/canary/include/reset_scope_keys.h").read_text(encoding="utf-8")
        tenth = body_after(scope, "inline constexpr const char* const kCredentialsAndSettings[] = {", r"\n};\n")
        mesh = (REPO / "firmware/canary/lib/securacv_mesh/src/mesh_state.cpp").read_text(encoding="utf-8")
        scout = (REPO / "firmware/canary/lib/securacv_ble_scan/src/ble_scout_key.cpp").read_text(encoding="utf-8")
        self.assertIn('constexpr const char* NVS_NAMESPACE = "securacv";', mesh)
        self.assertIn('constexpr const char* NVS_NAMESPACE = "securacv";', scout)
        keys = ["opera_secret", "trusted_peers", "peer_macs", "elected_hub", "opera_name", "scout_key"]
        for key in keys:
            with self.subTest(key=key):
                self.assertTrue(f'"{key}"' in mesh or f'"{key}"' in scout, key)
                self.assertNotIn(f'"{key}"', tenth)
        main = (REPO / "firmware/canary/src/main.cpp").read_text(encoding="utf-8")
        reset = body_after(main, "static void factory_reset(canary::io::ResetScope scope) {", r"\n}\n")
        self.assertIn("nvs.clear();", reset)      # the 30 s wipe: the whole namespace
        self.assertIn("canary::reset_scope::kCredentialsAndSettings[i]", reset)
        self.assertIn('#define NVS_MAIN_NS       "securacv"',
                      (REPO / "firmware/canary/include/canary_config.h").read_text(encoding="utf-8"))
        ino = (WAP / "canary_wap.ino").read_text(encoding="utf-8")
        hold = body_after(ino, "static void factory_reset_by_hold() {", r"\n}\n")
        for word in ("opera", "scout", "mesh", "nvs->clear", "elected"):
            with self.subTest(hold=word):
                self.assertNotIn(word, hold)
        # The hold's own body is not the whole hold: it calls
        # wifi_clear_credentials(), csi_mqtt::config_clear() and
        # setup_wizard::reset_to_first_boot(), and a key-by-key remove of
        # "opera_sec" in one of them passed this test (#1843's sweep, second
        # review). recovery_kit.test.js walks every free function the hold
        # reaches for the mesh's keys, its namespace and the scout key; that
        # row must stay.
        kit = (WAP / "recovery_kit.test.js").read_text(encoding="utf-8")
        row = body_after(kit, "it(\"the 10 s hold keeps the WAP's opera membership and its scout key: nothing it reaches names them",
                         r"\n  it\(")
        self.assertIn("sketchCallWalk('factory_reset_by_hold')", row)
        for key in ("'opera_sec'", "'opera_id'", "'peer_cnt'", "scout_key", "NVS_MESH_NS"):
            with self.subTest(walked=key):
                self.assertIn(key, row)
        callers = []
        for f in sorted((REPO / "firmware").rglob("*")):
            if f.suffix in (".cpp", ".h", ".ino") and "tests_host" not in f.parts and "test_" not in f.name:
                text = f.read_text(encoding="utf-8", errors="replace")
                for call in ("clear_opera_secret()", "ble_scout_key_factory_reset()"):
                    for line in text.splitlines():
                        s = line.strip()
                        if call not in s or s.startswith(("//", "*", "/*")):
                            continue
                        if re.match(r"bool (\w+::)?(clear_opera_secret|ble_scout_key_factory_reset)\(\)", s):
                            continue    # the declaration or the definition
                        callers.append((f.name, call, s))
        self.assertEqual(sorted(callers), sorted([
            ("mesh_state.cpp", "clear_opera_secret()", "clear_opera_secret();"),
            ("securacv_network.cpp", "clear_opera_secret()",
             "bool cleared = mesh_state::clear_opera_secret();"),
        ]), callers)

    def test_a_flasher_install_starts_nvs_over(self):
        # #1843's sweep, third pass: the docs say an install in either
        # flasher, with or without the full erase, starts NVS over, so the
        # next boot makes a new key, token and certificate, and that an OTA
        # update, a PlatformIO upload and the 10 s hold keep the key and the
        # certificate (the second pass had said an ordinary re-flash keeps
        # the key; the token is test_the_flagship_hold_keeps_no_seeded_token's).
        # Held to the
        # code: the factory image is merge_bin of the bootloader, the
        # partition table and the app alone, so the gap between the table
        # and the app, where every partition table puts nvs, is blank (0xFF)
        # in it, and both flashers write it at offset 0. A factory image that
        # starts to carry NVS, or a flasher that writes past it, fails here
        # until the docs follow. (The cores' default tables, which the WAP,
        # the Sense and the Vision build with, put nvs at 0x9000 and app0 at
        # 0x10000 too: read from both pinned cores, not from this tree.)
        mf = (REPO / "firmware/scripts/make_factory.py").read_text(encoding="utf-8")
        self.assertIn("so flashing it re-initializes NVS", normalized(mf))
        merge = body_after(mf, '"merge_bin",', r"\n    \]\n")
        self.assertEqual(re.findall(r"str\(([\w.]+)\)", merge), ["args.out", "bootloader", "partitions", "app"])
        self.assertIn("hex(PARTITION_TABLE_OFFSET), str(partitions),\n        hex(app_off), str(app),", merge)
        for flag in ("--fill-flash-size", "--pad-to-size", "nvs"):
            with self.subTest(merge=flag):
                self.assertNotIn(flag, merge)
        js = (REPO / "canary-local/assets/flash.js").read_text(encoding="utf-8")
        self.assertIn("fileArray: wifiFile ? [{ data, address: 0 }, wifiFile] : [{ data, address: 0 }],", js)
        rescue = (REPO / "desktop/flash-engine/src/rescue.rs").read_text(encoding="utf-8")
        self.assertIn('pub fn write_bin_args(port: &str, path: &str, baud: u32) -> Vec<String> {\n'
                      '    vec![\n        "write-bin".into(),\n        "0x0".into(),', rescue)
        tables = sorted((REPO / "firmware").rglob("partitions*.csv"))
        self.assertGreaterEqual(len(tables), 4)
        for table in tables:
            rows = [[c.strip() for c in ln.split(",")] for ln in table.read_text(encoding="utf-8").splitlines()
                    if ln.strip() and not ln.lstrip().startswith("#")]
            nvs = [int(r[3], 16) for r in rows if r[0] == "nvs"]
            apps = [int(r[3], 16) for r in rows if r[1] == "app"]
            with self.subTest(table=str(table.relative_to(REPO))):
                self.assertEqual(len(nvs), 1)
                self.assertTrue(0x8000 + 0xC00 <= nvs[0] < min(apps), (hex(nvs[0]), [hex(a) for a in apps]))

    def test_the_flashers_safety_copy_reads_the_whole_flash(self):
        # #1843's sweep, fifth pass: SECURITY_MODEL.md, THREAT_MODEL.md
        # (Principle 1's scope, Scenario 6), secure_defaults.h and
        # LESSONS_LEARNED.md say the key's "never leaves" is the firmware's
        # promise: both flashers read the whole flash over USB for their
        # pre-flash safety copy, a file on the computer that so holds the
        # key's plaintext NVS, and restoring it puts the key back. Held to the
        # code: the browser reads from offset 0 for the board's whole flash
        # and saves it as a download, the desktop engine runs read-flash 0x0
        # <size> into its backups folder, and both restores write the file
        # back at offset 0. A safety copy that starts to skip NVS fails here
        # until the docs follow.
        js = (REPO / "canary-local/assets/flash.js").read_text(encoding="utf-8")
        take = body_after(js, "async function takeBackup(box) {", r"\n}\n")
        self.assertIn("const total = state.flashBytes;", take)
        self.assertIn("await readFlashChunked(esploader, 0, total,", take)
        self.assertIn("downloadBytes(bytes, name);", take)
        self.assertIn("a backup holds the board's \" +\n    \"identity key and saved WiFi", js)
        rescue = (REPO / "desktop/flash-engine/src/rescue.rs").read_text(encoding="utf-8")
        self.assertIn('pub fn read_flash_args(port: &str, size: u64, out: &str, baud: u32) -> Vec<String> {\n'
                      '    vec![\n        "read-flash".into(),\n        "0x0".into(),\n        size.to_string(),',
                      rescue)
        self.assertIn('pub fn write_bin_args(port: &str, path: &str, baud: u32) -> Vec<String> {\n'
                      '    vec![\n        "write-bin".into(),\n        "0x0".into(),', rescue)
        lib = (REPO / "desktop/src-tauri/src/lib.rs").read_text(encoding="utf-8")
        self.assertIn("rescue::read_flash_args(&port, flash_size, &out_path, baud),", lib)
        self.assertIn('.join("backups");', lib)
        self.assertIn('let staged = stage_firmware(&bytes, "rescue-restore")?;', lib)
        self.assertIn("rescue::write_bin_args(&port, &staged_path, baud),", lib)

    def test_the_flagship_hold_keeps_no_seeded_token(self):
        # #1843's sweep, fourth pass: the docs say the 10 s hold keeps the
        # identity key, the chain and the certificate on every Canary and the
        # token on a WAP, and that the flagship's hold removes its stored
        # token, so the next boot derives one from the key and a token a
        # flasher seeded (both seed one when Wi-Fi is typed in for a blob-
        # scheme board) does not come back. The CHANGELOG and
        # PENTEST_SCOPE.md had said the hold keeps the token on every Canary,
        # "derived from the key". Held to the code: the flagship's 10 s list
        # removes NVS_KEY_TOKEN and keeps the key and the certificate, the
        # load prefers a stored token and derives only without one, and both
        # flashers seed the flagship's key and the WAP's.
        keys = (REPO / "firmware/canary/include/reset_scope_keys.h").read_text(encoding="utf-8")
        tenth = body_after(keys, "kCredentialsAndSettings[] = {", r"\n};\n")
        kept = body_after(keys, "kIdentityKept[] = {", r"\n};\n")
        self.assertIn("NVS_KEY_TOKEN,", tenth)
        for key in ("NVS_KEY_PRIV,", "NVS_KEY_CHAINST,", "NVS_KEY_TLS_CERT,", "NVS_KEY_TLS_KEY,"):
            with self.subTest(kept=key):
                self.assertIn(key, kept)
                self.assertNotIn(key, tenth)
        self.assertIn('#define NVS_KEY_TOKEN     "api_token"',
                      (REPO / "firmware/canary/include/canary_config.h").read_text(encoding="utf-8"))
        auth = (REPO / "firmware/canary/lib/securacv_auth/src/securacv_auth.cpp").read_text(encoding="utf-8")
        load = body_after(auth, "bool auth_load_or_derive(const uint8_t privkey[32]) {", r"\n}\n")
        self.assertLess(load.index("nvs.getBytes(NVS_KEY_TOKEN"), load.index("if (!loaded) {"))
        self.assertLess(load.index("if (!loaded) {"), load.index("derive_api_token(privkey"))
        core = (REPO / "canary-local/assets/flash-core.js").read_text(encoding="utf-8")
        self.assertIn("return { blobs: { api_token: bytes, api_tkn: bytes } };", core)
        js = (REPO / "canary-local/assets/flash.js").read_text(encoding="utf-8")
        self.assertIn('const apiToken = opts.product && opts.product.wifi_nvs === "blob" && opts.wifi\n'
                      '          ? core.mintApiToken(', js)
        rs = (REPO / "desktop/flash-engine/src/provisioning.rs").read_text(encoding="utf-8")
        self.assertIn('writer.blob("api_token", config.api_token.as_bytes())?;', rs)
        self.assertIn('writer.blob("api_tkn", config.api_token.as_bytes())?;', rs)
        app = (REPO / "desktop/src/app.js").read_text(encoding="utf-8")
        self.assertIn('provisioning && provisioning.wifiSsid && product.wifi_nvs === "blob" ? mintApiToken() : ""', app)
        catalog = json.loads((REPO / "canary-local/devices/flash.json").read_text(encoding="utf-8"))
        blob = {p["id"] for p in catalog["products"] if p.get("wifi_nvs") == "blob"}
        self.assertTrue({"securacv-canary", "securacv-canary-wap"} <= blob, sorted(blob))
        # The WAP's hold keeps api_tkn, the key, the chain, the certificate
        # and the Beacon keys: its own body names none of them, and
        # recovery_kit.test.js walks every function it reaches for each, by
        # literal and by constant (a key-by-key remove in a callee passed
        # every suite before this row); that row must stay.
        ino = (WAP / "canary_wap.ino").read_text(encoding="utf-8")
        hold = body_after(ino, "static void factory_reset_by_hold() {", r"\n}\n")
        for word in ("NVS_KEY_API_TKN", "NVS_KEY_PRIV", "NVS_KEY_TLS", "api_tkn", "privkey", "beacon"):
            with self.subTest(hold=word):
                self.assertNotIn(word, hold)
        kit = (WAP / "recovery_kit.test.js").read_text(encoding="utf-8")
        row = body_after(kit, "it(\"the 10 s hold keeps the WAP's identity key, chain, token, HTTPS certificate and Beacon keys",
                         r"\n  it\(")
        self.assertIn("sketchCallWalk('factory_reset_by_hold')", row)
        for key in ("'NVS_KEY_PRIV', 'privkey'", "'NVS_KEY_API_TKN', 'api_tkn'", "'NVS_KEY_TLS_CERT', 'tls_cert'",
                    "'NVS_KEY_TLS_KEY', 'tls_key'", "'NVS_KEY_CHAINST', 'chain_st'", "'NVS_NS', 'beacon'",
                    "'NVS_X25519_PRIV', 'x25519_priv'", "beacon_channel"):
            with self.subTest(walked=key):
                self.assertIn(key, row)

    def test_the_iphone_app_reaches_no_wap_console(self):
        # The same pass: the Fleet Wi-Fi copy no longer offers the Bluetooth
        # rescue for a WAP, because the iPhone app dials a console only for a
        # peripheral advertising the console service and no WAP advertises it
        # (ledger A65). A WAP that starts to advertise the console, or an app
        # that stops filtering on it, fails here until the copy follows.
        ble = (REPO / "ios/Sources/SecuraCV/Transport/BLEConsole.swift").read_text(encoding="utf-8")
        console = "8fc1cee0-b162-4401-9607-c8ac21383e90"
        self.assertIn(f'static let consoleServiceUUID = CBUUID(string: "{console}")', ble)
        self.assertIn("guard advertised.contains(Self.consoleServiceUUID) else { return }", ble)
        self.assertIn(f'static const char* SERVICE_UUID  = "{console}";',
                      (WAP / "ble_console.h").read_text(encoding="utf-8"))
        advertised = []
        for f in sorted(WAP.iterdir()):
            if f.suffix in (".ino", ".cpp", ".h"):
                text = f.read_text(encoding="utf-8", errors="replace")
                advertised += [(f.name, a) for a in re.findall(r"addServiceUUID\(([^;]*)\);", text)]
        self.assertEqual(sorted(advertised), sorted([
            ("bluetooth_channel.cpp", "SERVICE_UUID"),          # bluetooth_channel.h: 8fc1ceca-…
            ("ble_chirp.h", "SCV_SERVICE_UUID"),                # ble_config.h: a1b2c3d4-…
            ("ble_improv.cpp", "NimBLEUUID(improv::SERVICE_UUID)"),
            ("ble_opera.h", "SCV_SERVICE_UUID"),
            ("ble_opera.h", "SCV_SERVICE_UUID"),
        ]), advertised)
        self.assertIn('static const char* SERVICE_UUID = "8fc1ceca-b162-4401-9607-c8ac21383e90";',
                      (WAP / "bluetooth_channel.h").read_text(encoding="utf-8"))
        self.assertIn('#define SCV_SERVICE_UUID          "a1b2c3d4-e5f6-7890-abcd-ef0123456001"',
                      (WAP / "ble_config.h").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
