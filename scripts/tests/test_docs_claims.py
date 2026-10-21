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
     "no tier refuses a corrupt image today; every tier relies on the OTA "
     "signature check"),
    ("Secure Boot v2 + Flash Encryption env; compile-only in CI",
     "the secure env enables neither and refuses the key on every board"),
    ("Factory reset via BOOT button | ✅ (short/long press) | ❌ | ✅ (5s hold)",
     "the WAP sketch resets at 10 s; the flagship resets at 10 s (key kept) and "
     "wipes at 30 s"),
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
    # The same sweep: the 10 s BOOT hold keeps the identity key, the chain,
    # the token and the HTTPS certificate on every Canary
    # (factory_reset_by_hold(); reset_scope_keys.h kIdentityKept). Only the
    # flagship's 30 s wipe or a full erase in a flasher changes them, and a
    # WAP's setup network comes up with no button while it is off its
    # Wi-Fi.
    ("docs/getting_started_canary.md",
     "You *can* lose your witness chain by factory-resetting it",
     "the 10 s hold keeps the key and the chain; a full erase (or the "
     "flagship's 30 s wipe) loses them"),
    ("docs/getting_started_canary.md",
     "export the chain before you factory-reset",
     "export it before a full erase or the flagship's 30 s wipe"),
    ("docs/getting_started_canary.md",
     "factory reset makes a new certificate, so pair the iPhone app again",
     "the 10 s reset keeps the certificate; the 30 s wipe or a full erase makes "
     "a new one"),
    ("firmware/canary/include/canary_config.h",
     "(factory reset erases both,",
     "reset_scope_keys.h kIdentityKept keeps tls_cert and tls_key on the 10 s "
     "reset"),
    ("firmware/canary/scripts/ota_deploy.py",
     "A factory reset makes a new certificate;",
     "only the 30 s wipe or a full flash erase makes a new certificate"),
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
     "a factory reset keeps the key; a full erase gives a new one"),
    ("ios/Sources/SecuraCV/Alerts/HeadWatch.swift",
     "means the chain restarted (a wipe, a reset)",
     "the 10 s reset keeps the chain; a full erase, the flagship's 30 s wipe or "
     "a replaced board restarts it"),
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
     "the 10 s reset keeps the key; a full erase, the flagship's 30 s wipe or a "
     "replacement board gives a new one"),
    ("custom_components/securacv/__init__.py",
     "starts its counters over (fresh flash, factory",
     "the same"),
    ("custom_components/securacv/sensor.py",
     "which is what a factory reset looks like from here",
     "a re-pin is what a full erase looks like; the 10 s reset keeps the key "
     "and its counters"),
    ("custom_components/securacv/tests/test_replay_reset_paths.py",
     "a factory-reset or replaced Canary starts them over",
     "an erased or replaced Canary does; the 10 s reset keeps them"),
    ("custom_components/securacv/tests/test_replay_and_device_id.py",
     "# Factory reset: the device pins fresh",
     "a full erase pins fresh; a factory reset keeps the key"),
    ("firmware/common/network/auth_backoff.h",
     "polling a reset unit with its old token",
     "the 10 s hold leaves the token as it was; only a full erase changes it"),
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
     "the hold and Forget Wi-Fi keep the token; only a full erase leaves a "
     "stale pairing striking"),
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
     "no factory reset regenerates the WAP's key; a full erase in a flasher does"),
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
    # The scoped prose that replaced the unscoped broker-move sentence.
    "On the WAP, the same save from the dashboard session alone is refused "
    "`403 password_needed` when it moves the host, port, TLS mode or CA; the "
    "bearer, or the password typed again, moves it (sweep F289).",
    # The tier and way-out prose that replaced #1843's fourth-review bans.
    "so on every tier today the key is recoverable by reading the flash "
    "directly, on a fused board too (Scenario 4 below)",
    "Flash encryption (the app image at rest; not NVS, where the keys and the "
    "saved passwords stay plaintext on every tier today, Scenario 4)",
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
    "know what the opt-in tiers buy today before you burn anything: at most "
    "the firmware image's protection (flash encryption keeps the app image "
    "from being read off the chip; Secure Boot, which the provisioning kit "
    "prepares but does not yet enable, would refuse unsigned firmware), not "
    "the key's.",
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
    "- **Secure tier: PARTIAL PASS.** Secure Boot, which the provisioning kit\n"
    "  prepares but does not yet enable, would refuse unsigned firmware;",
    "- Secure boot (would verify firmware integrity; prepared in the provisioning kit, not yet enabled on any tier)",
    "| Firmware corrupt | No tier refuses to boot it today: Secure Boot is prepared in the provisioning kit but not enabled (and is NOT on default builds — see [Scope of this",
    "| `platformio_secure.ini` | ✅ | the secure env: its sdkconfig stages Secure Boot v2 and flash encryption (commented out) and names NVS encryption, all inert under `framework =",
    "| Factory reset via BOOT button | ✅ (short/long press) | ✅ (10 s hold: Wi-Fi, broker, name; identity and token kept) | ✅ (10 s: credentials + settings, identity kept; 30 s: wipe,",
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
    "A holder of one member's Ed25519 private key — readable from flash on "
    "every tier today: it is plaintext in NVS, which flash encryption does not "
    "cover",
    "but that move keeps the host's network saved; the hold, or Forget Wi-Fi, "
    "followed by that setup drops it, and reopens that door",
    "Once that is settled the lane holds only while that host leaves it alone "
    "and the new network stays up, and taking it away needs no radio",
    "under the lockout), while that host leaves it alone and the new network "
    "stays up (the next four bullets).",
    "Flash encryption with NVS encryption (Tier 3) would defend key-at-rest and "
    "casual readout, not decapping/fault-injection by a well-funded lab.",
    "Dev mode is the sweet spot: with NVS encryption beside it, it would "
    "protect WiFi/keys at rest from a thief while the device stays "
    "field-recoverable.",
    "Secure it (protect the key at rest) — the fuses are tooled, the key's "
    "protection is not yet buildable.",
    "Keeps the entire existing signing scheme; once NVS encryption is in the "
    "build, the key can't be dumped off a locked device",
    "the firmware writes the `opera_secret` to NVS only with FE on (audit O2). "
    "FE does not make it confidential: NVS is plaintext on a fused board too",
    "protects the firmware image, not the identity key or the saved WiFi",
    "For a board deployed somewhere physically exposed, put it out of reach: "
    "Phase-2 provisioning (flash encryption + eFuse) does not yet protect its "
    "key or saved WiFi.",
    "An owner who finds the unit on the wrong network still holds it, but on a "
    "WAP a reset does not undo a stranger who took the door.",
    "a unit joined to the wrong network is one its owner still holds. On a WAP "
    "a factory reset is not the whole fix",
    "which clears every saved network and opens the door again for half an "
    "hour from that moment, to the first phone or computer nearby that "
    "connects",
    # The replacements from #1843's sweep for every remaining copy.
    "  //    and the address they serve has its own slot cleared (sweep F341,\n"
    "  //    after step 3), never the shared overflow: an address held only\n"
    "  //    through the overflow, which a host striking from nine or more\n"
    "  //    addresses keeps running, stays held after it is served.",
    "  section above: a tap clears an address only when it is served to that\n"
    "  address's own ask, and the flagship's tap still goes to the first asker,\n"
    "  F354; under that host the hold is no way out unless the unit is then set\n"
    "  up on a network that host is not on, the iPhone app does not reach a\n"
    "  WAP's Bluetooth lane today, ledger A65, and that section gives both\n"
    "  trees' ways out with their caveats.) **Host-tested**",
    "| 1 | API authentication (bearer token + HKDF derivation + exponential backoff + constant-time compare) | ✅ (Phase 2.5: SPA injects bearer; all read + mutating endpoints gated;",
    "// token ends its address's own backoff (auth_handed_token, sweep F341;\n"
    "// never the shared overflow's), so a browser that mistyped the token is not\n"
    "// held on its first API call.",
    "  it stops, or hold BOOT 10 s and set the unit up on a network that host\n"
    "  is not on. (Corrected by #1843's later reviews, in the amendments below:\n"
    "  the iPhone app never reaches a WAP's lane, A65, so the sentence the docs\n"
    "  use now names the owner's browser on `POST /api/wifi/connect` as the\n"
    "  WAP's way out, with its caveats, and the iPhone's lane as no way out\n"
    "  until A65 is settled.) `test_auth_backoff.cpp`'s overflow row starts from the empty",
    "  *Amended once more 2026-10-21 (#1843's first review):* the bonded lane\n"
    "  gets the owner out only while that host holds no bond of its own.\n"
    "  (Corrected by the fourth and fifth amendments below: radio range is no\n"
    "  bound, since any landing session can take the lane away, and the lane is\n"
    "  no way out today, since the iPhone app never reaches a WAP's CREDS, A65.)\n"
    "  Any landing session can pair a Bluetooth peer: it can turn pairing on",
    "  which F343's posture then named as the way out from under a host striking\n"
    "  from nine or more addresses (corrected by the third review's note below:\n"
    "  the way out is the owner's browser, and the iPhone's lane is no way out\n"
    "  until A65 is settled), and spend that lane's ten writes an hour, so the decision",
    "You can't brick the device through the dashboard. You *can* lose your\n"
    "witness chain by erasing the board: a full erase in a flasher (or, on the\n"
    "all-rounder Canary, holding BOOT to 30 seconds) removes the Ed25519 device\n"
    "key, the next boot makes a new one from a fresh hardware-RNG seed, and the\n"
    "previous chain is no longer signable. The 10-second BOOT reset keeps the\n"
    "key and the chain. If you've been using the Canary as a witness device\n"
    "for evidence, **export the chain before you erase it** —",
    "witness chain by erasing the board: a full erase in a flasher (or, on the\n"
    "all-rounder Canary, holding BOOT to 30 seconds) removes the Ed25519 device\n"
    "key, the next boot makes a new one from a fresh hardware-RNG seed, and the\n"
    "previous chain is no longer signable. The 10-second BOOT reset keeps the\n"
    "key and the chain. If you've been using the Canary as a witness device\n"
    "for evidence, **export the chain before you erase it** —",
    "app checks it against the fingerprint in your recovery kit instead. The\n"
    "10-second BOOT reset keeps the certificate, so that fingerprint stays\n"
    "right; holding BOOT to 30 seconds, or a full erase in a flasher, makes a\n"
    "new one, so pair the iPhone app again after either. The standard release\n"
    "image does not turn this on yet.",
    "// generated once on the first TLS-capable boot. The 10 s BOOT reset keeps\n"
    "// both (reset_scope_keys.h kIdentityKept); only the 30 s wipe\n"
    "// (ResetScope::Everything) or a full flash erase removes them, after which\n"
    "// the device re-keys and a pinned iPhone pairing must be redone.",
    "        log_error(\"Refusing to send the token. Only the 30 s BOOT wipe or a full flash erase\")\n"
    "        log_error(\"makes a new certificate (the 10 s reset keeps it); re-read `tls_cert_fp`\")\n"
    "        log_error(\"from a new recovery kit only if one of those happened.\")",
    "    log_error \"Refusing to send the token. Only the 30 s BOOT wipe or a full flash erase\"\n"
    "    log_error \"makes a new certificate (the 10 s reset keeps it); re-read \\`tls_cert_fp\\`\"\n"
    "    log_error \"from a new recovery kit only if one of those happened.\"",
    "  Serial.println(\"║  BOOT: tap=gate, 2s=info, 10s=reset (key kept), 30s=wipe     ║\");",
    "                + \"place — check the device itself. If you fully erased it in a flasher (or \"\n"
    "                + \"held an all-rounder Canary's BOOT to 30 seconds) or replaced it, re-pair it \"\n"
    "                + \"to get its new receipt. A 10-second BOOT reset keeps its certificate, so \"\n"
    "                + \"after one this mismatch means something else is answering.\"",
    "        // sight then (TOFU) — a fully erased (re-flashed) device with a new\n"
    "        // key must not inherit a permanent \"signature failed\" from its\n"
    "        // previous life.",
    "//     means the chain restarted (a full erase, an all-rounder Canary's\n"
    "//     30 s BOOT wipe, a replaced board; the 10 s BOOT reset keeps the\n"
    "//     chain) — exactly the kind of",
    "// service and its own setup network can still rescue), never the fleet.",
    "        + \"was wrong, this Canary's Bluetooth rescue or its own setup network \"\n"
    "        + \"(SecuraCV-wap-XXXX, which it raises while it is off your Wi-Fi) will \"\n"
    "        + \"take the correction.\"",
    "            resolveProvision(.failed(\"This Canary's firmware doesn't offer the Bluetooth rescue — use its own setup network (SecuraCV-wap-XXXX, which it raises while it is off",
    "        # identity for this device_id (a full flash erase, an all-rounder\n"
    "        # Canary's 30 s BOOT wipe, a replacement board; the 10 s BOOT reset\n"
    "        # keeps the key), whose counters start over — so its replay",
    "        # A newly pinned device starts its counters over (a full flash erase,\n"
    "        # an all-rounder Canary's 30 s BOOT wipe); its replay high-water marks\n"
    "        # must start over with it. The verifiers do not return this reason\n"
    "        # today — the health hook and the options flow clear the marks\n"
    "        # themselves — but a future verifier that pins inline gets the same\n"
    "        # behavior for free.",
    "    device is (re)pinned via TOFU (async_record_verify), which is what a\n"
    "    full flash erase (or an all-rounder Canary's 30 s BOOT wipe) looks like\n"
    "    from here; the 10 s BOOT reset keeps the key and its counters.",
    "to the KEY: an erased (re-flashed) or replaced Canary starts them over. So every",
    "    # A full erase (a new key): the device pins fresh and its chain starts over.",
    "// polling a re-flashed unit with its old token (a full erase gives it a new\n"
    "// key, so a new token and device_id; the 10 s BOOT reset leaves the token as\n"
    "// it was). A Table keeps one Slot per source address instead, so a host's\n"
    "// wrong bearers hold only that host's address.",
    "// polling a re-flashed unit with its old token (a full erase gives it a new\n"
    "// key, so a new token and device_id; the 10 s BOOT reset leaves the token as\n"
    "// it was). A Table keeps one Slot per source address instead, so a host's\n"
    "// wrong bearers hold only that host's address.",
    "  and the likeliest such host was the owner's own phone, polling a\n"
    "  re-flashed unit (a full erase gives it a new key and token; the 10 s hold\n"
    "  keeps the token) with its old token. Because a `static` in a header is a separate object in",
    "  still polling a re-flashed unit (a full erase gives it a new key and\n"
    "  token; the 10 s hold keeps the token) with its old token (filed as F352). The backoff",
    "visiting browser's own address. A phone that still holds a pairing for a\n"
    "unit that was fully erased and re-flashed (a new key, so a new token and\n"
    "device_id; the 10 s hold keeps the token) keeps polling with the old token,\n"
    "and so holds its own address, its new pairing included (F352). Host-tested",
    "| No credentials stored (first boot, or after a factory reset or Forget Wi-Fi), within **30 minutes** of power-on (after a reset, of the credentials vanishing) | **Open**",
    "  (iOS, a reset unit's stale pairing; amended 2026-10-21, #1843: a\n"
    "  re-flashed unit's, since the 10 s hold keeps the token).",
    "- [ ] **F352 [code] iOS: a pairing for a re-flashed Canary keeps polling with\n"
    "  its old token.** A full erase in a flasher, which regenerates the WAP's key\n"
    "  (the 10 s hold keeps the key and the token), changes both its token and its\n"
    "  `device_id`. *(Amended 2026-10-21, #1843: this item had said a factory\n"
    "  reset, which keeps both.)* `DeviceStore.add`",
    "  untouched) and `docs/csi_developer_api.md` say so now; the canary-wap half\n"
    "  is unchanged. No behavior changed; not run on a device.\n"
    "  *Amended 2026-10-21 (#1843):* the canary-wap half said it has no reset\n"
    "  path; it has the 10 s hold (`factory_reset_by_hold()`, #1816), which keeps\n"
    "  `csi`/`ev.next` and `csi.evsent`, so the full erase is still its\n"
    "  recovery. The note at `space_low()` (both trees) and\n"
    "  `docs/csi_developer_api.md` say so now.\n"
    "- [x] **F83 [code] The canary-wap can commit an event before its id floor",
    "// stranger. A canary-wap's 10 s BOOT hold (factory_reset_by_hold()) clears\n"
    "// neither (it removes the saved Wi-Fi, the broker row and the name, and\n"
    "// keeps the identity), so its owner flashes it with the flasher's full\n"
    "// erase, which clears all of NVS, the floor `ev.next` and `csi.evsent`\n"
    "// included. Either way the allocator starts again at kIdSpaceBase,",
    "// stranger. A canary-wap's 10 s BOOT hold (factory_reset_by_hold()) clears\n"
    "// neither (it removes the saved Wi-Fi, the broker row and the name, and\n"
    "// keeps the identity), so its owner flashes it with the flasher's full\n"
    "// erase, which clears all of NVS, the floor `ev.next` and `csi.evsent`\n"
    "// included. Either way the allocator starts again at kIdSpaceBase,",
    "  sees a stranger; a canary-wap's 10 s BOOT hold (`factory_reset_by_hold()`)\n"
    "  keeps both, so its owner flashes it with the",
    "  `space_low()` on both trees: the 30 s BOOT wipe on a canary (its 10 s reset\n"
    "  keeps the floor), a full flash erase on a canary-wap (whose 10 s hold keeps\n"
    "  it too) without restoring the flasher's pre-erase backup, then a",
    "            \"what\": \"Wipes every byte of flash — firmware, settings, stored WiFi, the identity \"\n"
    "                    \"key and the chain — then writes fresh, so the board comes up with a new \"\n"
    "                    \"identity. More than a factory reset: the 10-second BOOT reset keeps the key.\",",
    "      \"what\": \"Wipes every byte of flash — firmware, settings, stored WiFi, the identity key and the chain — then writes fresh, so the board comes up with a new identity. More",
    "        WAP has no factory-reset route at all (BOOT held 10 s,\n"
    "        `factory_reset_by_hold()`, is the only path; it clears the saved\n"
    "        Wi-Fi, the broker row and the name, keeps the identity, the chain and\n"
    "        the token, and reopens the first-come Bluetooth setup door). Whether\n"
    "        `POST /api/factory-reset` may exist is the decision;",
    "        never be able to reopen the setup door), with a two-step confirmation in the",
    "  before 700 ms — the WAP's 2 s hold and the 10 s factory-reset hold are",
    " * the 30 s BOOT wipe or a full erase; the 10 s reset keeps the key) is left\n"
    " * untouched and unused. The backfill signs what it",
    "/* Erase the persisted trusted-peer list. Used by POST /api/mesh/leave\n"
    " * (securacv_network.cpp's handle_mesh_leave). No factory reset calls it: the\n"
    " * 10 s BOOT reset keeps the list, and the 30 s wipe drops it with the whole\n"
    " * namespace (nvs.clear()). Idempotent — succeeds if the list was empty. Also",
    "      if (!confirm('Remove every saved Wi-Fi network from this Canary? For half an hour after, its Bluetooth setup goes to the first phone or computer nearby that connects, so",
    " *   - NO CREDENTIALS STORED (first boot, or after a factory reset or Forget\n"
    " *     Wi-Fi, which the owner chose: on a WAP both keep the identity and the\n"
    " *     token, so that unit still has an owner, and the door goes to whichever\n"
    " *     client connects first, docs/design/magic_pairing.md §2.2): the",
    " *   - NO CREDENTIALS STORED (first boot, or after a factory reset or Forget\n"
    " *     Wi-Fi, which the owner chose: on a WAP both keep the identity and the\n"
    " *     token, so that unit still has an owner, and the door goes to whichever\n"
    " *     client connects first, docs/design/magic_pairing.md §2.2): the",
    " * for a WAP nobody owns yet (or one whose owner held BOOT 10 s or forgot its\n"
    " * Wi-Fi: both keep the identity and the token, and the door goes to\n"
    " * whichever client connects first, docs/design/magic_pairing.md §2.2):\n"
    " * while no credentials are stored, for the",
    "          PIN confirmation to prevent unauthorized access. Paired devices are kept in the device's\n"
    "          settings storage (NVS), which is plaintext on the flash chip on every build today, so\n"
    "          someone holding the board can read them; manage them from this panel.",
    "    meaning: \"Flash encryption is on, with a key burned into this chip: the firmware image (and any partition flagged encrypted) reads back as ciphertext, and a plaintext image",
    "            meaning: \"Flash encryption is on, with a key burned into this chip: the firmware image (and any partition flagged encrypted) reads back as ciphertext, and a plaintext",
    " *      store IRKs only in NVS (plaintext on the flash chip on every build\n"
    " *      today: flash encryption does not cover NVS and NVS encryption is in\n"
    " *      no released build; common/identity/key_at_rest.h) and never export\n"
    " *      them over any wire, MQTT topic, or web UI.",
    "  `hh_count`. Its comment said the IRKs are \"flash-encrypted at rest on ESP32\n"
    "  with secure boot\". That was false, and the comment now says so (2026-10-21):\n"
    "  flash encryption does not cover NVS, NVS encryption cannot be built under\n"
    "  `framework = arduino` (neither pinned core's precompiled `sdkconfig` has\n"
    "  it), and secure boot is prepared but not enabled on any tier\n"
    "  ([`key_at_rest.h`](../../firmware/common/identity/key_at_rest.h)).",
    "Until then, the IRKs sit in plaintext: `household.h`'s old claim that they\n"
    "were \"flash-encrypted at rest on ESP32 with secure boot\" was wrong, and its\n"
    "comment now says so (2026-10-21), since flash encryption does not cover NVS,\n"
    "NVS encryption cannot be built under `framework = arduino` (neither pinned\n"
    "core's precompiled `sdkconfig` has it), and secure boot is prepared but not\n"
    "enabled on any tier ([`key_at_rest.h`](../../firmware/common/identity/key_at_rest.h)).",
    "// and opera_id derivation. A physical-access attacker who reads NVS can\n"
    "// extract it and become a permanent opera member from outside the household,\n"
    "// on a fused board too: flash encryption does not cover NVS, and NVS\n"
    "// encryption is not available under framework = arduino\n"
    "// (spec/canary_mesh_network_v0.md §5.5). Both the save and load paths refuse\n"
    "// to operate unless flash encryption is enabled (eFuse FLASH_CRYPT_CNT > 0),\n"
    "// which keeps the secret off un-fused boards only. Devices that do not",
    " * (with no flash-encryption gate: Tier 0 like the identity key, plaintext\n"
    " * in NVS on every tier today; mesh_state.h, common/identity/key_at_rest.h).",
    " * (with no flash-encryption gate: Tier 0 like the identity key, plaintext\n"
    " * in NVS on every tier today; mesh_state.h, common/identity/key_at_rest.h).",
    " * (not gated on flash encryption, unlike the opera_secret, and plaintext\n"
    " * in NVS on every tier today: flash encryption does not cover NVS;\n"
    " * common/identity/key_at_rest.h).",
    " * (not gated on flash encryption, unlike the opera_secret, and plaintext\n"
    " * in NVS on every tier today: flash encryption does not cover NVS;\n"
    " * common/identity/key_at_rest.h).",
    "      check_fail \"Mesh impl '$rel' has no esp_flash_encryption_enabled() gate — the opera_secret must never be persisted on a board without flash encryption (#610; the gate keeps",
    " * and the path up is NVS encryption, which needs the arduino-as-IDF-component\n"
    " * migration (firmware/ESP32S3_OPTIMIZATION_ROADMAP.md item 9); the secure kit\n"
    " * in firmware/provisioning/ is not it, since flash encryption does not cover\n"
    " * NVS — docs/PARITY.md says so per target rather than pretending.",
    " * and the path up is NVS encryption, which needs the arduino-as-IDF-component\n"
    " * migration (firmware/ESP32S3_OPTIMIZATION_ROADMAP.md item 9); the secure kit\n"
    " * in firmware/provisioning/ is not it, since flash encryption does not cover\n"
    " * NVS — docs/PARITY.md says so per target rather than pretending.",
    " * and the path up is NVS encryption, which needs the arduino-as-IDF-component\n"
    " * migration (firmware/ESP32S3_OPTIMIZATION_ROADMAP.md item 9); the secure kit\n"
    " * in firmware/provisioning/ is not it, since flash encryption does not cover\n"
    " * NVS — docs/PARITY.md says so per target rather than pretending.",
    "  // (common/identity/key_at_rest.h Tier 0); the path up is NVS encryption\n"
    "  // (the arduino-as-IDF-component migration), not the secure kit, whose flash\n"
    "  // encryption does not cover NVS.",
    "  // (common/identity/key_at_rest.h Tier 0); the path up is NVS encryption\n"
    "  // (the arduino-as-IDF-component migration), not the secure kit, whose flash\n"
    "  // encryption does not cover NVS.",
    "  // (common/identity/key_at_rest.h Tier 0); the path up is NVS encryption\n"
    "  // (the arduino-as-IDF-component migration), not the secure kit, whose flash\n"
    "  // encryption does not cover NVS.",
    "| C9 | Credentials encrypted at rest | NVS encryption on the NVS that holds the credential blobs (flash encryption alone does not cover NVS) |",
    "   and the path is NVS encryption, which needs the arduino-as-IDF-component\n"
    "   migration (`firmware/ESP32S3_OPTIMIZATION_ROADMAP.md` item 9): the opt-in\n"
    "   secure kit (`firmware/provisioning/`) does not encrypt NVS, since flash\n"
    "   encryption does not cover it and the kit's NVS-encryption setting is\n"
    "   inert under `framework = arduino`. This is the one brief item the",
    "opt-in secure kit (flash encryption + Secure Boot v2) that no CI leg builds.\n"
    "*(Amended 2026-10-21: `firmware.yml` compiles the kit's two envs,\n"
    "compile-only, since #1744; the kit stages flash encryption and Secure Boot,\n"
    "commented out, rather than enabling them, and neither would encrypt NVS.)*\n"
    "NVS encryption is not configurable on either pinned Arduino core as built",
    "| Encrypted credentials at rest | plaintext NVS everywhere; NVS encryption not configurable on the pinned cores | honest `⚠️`: posture reported on every node, the secure kit",
    "  posture is reported as `creds_at_rest` on every node, and NVS encryption,\n"
    "  after the arduino-as-IDF-component migration, is the documented path; the\n"
    "  secure kit is not, since flash encryption does not cover NVS (PARITY\n"
    "  footnote 1).",
    "3. **Key extraction.** The identity key is plaintext-at-rest. Without NVS\n"
    "   encryption (which needs flash encryption beside it and is in no released\n"
    "   build) or a hardware-bound key, physical possession = key compromise, on a\n"
    "   board with flash encryption fused too, today, which undermines both\n"
    "   attestation and the pinned-identity model.",
    "still exists but the ROM **refuses to boot an unsigned image**, and reads of the\n"
    "app image come back ciphertext (NVS does not: without NVS encryption, the Tier 3\n"
    "row, the key and saved passwords still read back in the clear) — so the browser\n"
    "flasher's \"write any factory image and boot",
    "firmware\"* actually hold. Tier 3, once NVS encryption is buildable, would\n"
    "additionally stop the identity key being lifted out of a stolen board, closing\n"
    "the clone-with-stolen-key gap (an optional DS-bound attestation key, §5.4, is\n"
    "non-extractable but a separate key); today no tier does.",
    "  disables plaintext serial programming. Protects the app image and the\n"
    "  partitions flagged `encrypted` at rest, **not** NVS (that takes NVS\n"
    "  encryption) and **not** a live chip.",
    "  the **ESP32-S3's own eFuse / NVS encryption / DS peripheral** (device) holds\n"
    "  (no released build uses any of them yet; flash encryption alone does not\n"
    "  cover NVS) —",
    "  path **(a)** would harden the ESP32-S3 Ed25519 identity *at rest* with",
    "| Bridge restarts / Canary reboots | HAP state is **re-derived, never restored**: re-advertise mDNS, republish current signals. Pairings persist (NVS on a Canary — plaintext on",
    "  NVS; they get the `opera_secret` treatment — refuse to persist on\n"
    "  flash-encryption-off devices, which keeps them off un-fused boards but does\n"
    "  not make them confidential (NVS is plaintext on a fused board too until NVS\n"
    "  encryption is in the build; the tiered story from",
    "  the NVS-stored broker credentials are plaintext at rest on every build: no\n"
    "  display env uses the secure sdkconfig\n"
    "  (`provisioning/sdkconfig.defaults.secure`), which is inert under\n"
    "  `framework = arduino` anyway, and flash encryption does not cover NVS (NVS\n"
    "  encryption needs the arduino-as-IDF-component migration); and (2) an opt-in `WiFiClientSecure` path with a",
    "| Triple-tap panic wipe | Right instinct, wrong layer for us. Chirp already retains nothing (ephemeral keys, no history). For Beacon, wiping the beacon set would silently destroy",
    "deferral, but it ships today: either enable flash encryption + NVS encryption\n"
    "(which needs the arduino-as-IDF-component migration; flash encryption alone\n"
    "does not cover NVS) + secure boot for production builds, or state the exposure\n"
    "explicitly in the threat model and on",
    "| P10 | **A known instrument** — you can know what code witnessed | signed firmware/OTA, versioned dictionary hash in checkpoints, (doc 12) release provenance | verify firmware",
    "- Reuses the existing key-management infrastructure (the key in NVS, plaintext at rest on every tier today since flash encryption does not cover NVS, see the threat table below;",
    "- JTAG permanently disabled via eFuse\n"
    "- NVS encryption — named in `sdkconfig.defaults.secure` but inert under\n"
    "  `framework = arduino` (`platformio_secure.ini`, \"KNOWN INERT\"), so a Phase 2\n"
    "  board's NVS (the identity key, the saved Wi-Fi and broker passwords, the\n"
    "  bonds) stays plaintext until the arduino-as-IDF-component migration",
    "| `sdkconfig.defaults.secure` | ESP-IDF Kconfig with BT disabled; Secure Boot v2 (with anti-rollback) and Flash Encryption (release mode) **prepared but not enabled** (commented",
    "# It encrypts NVS partition contents using a key stored in eFuse.\n"
    "# KNOWN INERT under framework = arduino (platformio_secure.ini): the line\n"
    "# below never reaches the Arduino core's precompiled build, so no released\n"
    "# image has NVS encryption, and NVS stays plaintext on every board.\n"
    "#\n"
    "CONFIG_NVS_ENCRYPTION=y",
    "`OperaConfig.opera_secret[32]` is the 32-byte symmetric secret from which `opera_id` is derived and which gates pairing. If the device's flash is not encrypted (ESP32 flash\n"
    "\n"
    "**Fix in this branch:** in `mesh_network::init`, call `esp_efuse_read_field_bit(ESP_EFUSE_FLASH_CRYPT_CNT)` (or the equivalent SDK helper) and refuse to load/save the opera_secret",
    "| NVS confidentiality | M | No FE gate (O2). *(Amended 2026-10-21: the O2 gate keeps the secret off un-fused boards; it does not give NVS confidentiality, since flash encryption",
    "  SD — pure append, never truncated — with the 64-entry flash-encrypted NVS\n"
    "  ring *(amended 2026-10-21: an NVS ring written only on a board with flash\n"
    "  encryption on, and plaintext NVS there too)* demoted to a recent-view cache (`beacon_channel.cpp::sd_append_audit_entry`).",
    "but aren't wired into the FULL build — identity keys and the chain sit in plaintext flash today\n"
    "*(amended 2026-10-21: and would on a FULL build with Secure Boot and flash encryption too, since\n"
    "flash encryption does not cover NVS and NVS encryption is not available under\n"
    "`framework = arduino`; `key_at_rest.h`)*; a\n"
    "PSRAM-backed deferred-write ring would stop record loss while the SD card is absent (the mechanism",
    "proves on hardware that **no mesh secret lands in NVS on a board without flash encryption** (on an\n"
    "FE board it does, in NVS that is plaintext too: flash encryption does not cover NVS).",
    "| C2 | **FE persistence gate** (keeps the secret off non-FE boards; not at-rest secrecy) | pair on a **non-FE** board, then dump NVS | serial shows `active for this boot but NVS",
    "   `opera_secret` is never persisted on a board without flash encryption (on an FE board it is,\n"
    "   in NVS that flash encryption leaves plaintext) — the static half of issue",
    "- The household key never leaves the device over any wire; it would live in NVS, which is plaintext on the flash chip on every tier today (no Canary has a secure element and NVS",
    "  We know a physical attacker can dump flash and recover the identity key (and\n"
    "  the saved Wi-Fi and broker passwords and the Bluetooth bonds), from a board\n"
    "  with flash encryption fused too: NVS is plaintext on every tier, since NVS\n"
    "  encryption is in no released build. What",
    "| `secure` / `secure_ha` | the provisioning kit's Tier 3/4 image: it refuses to store or load the identity key unless NVS is encrypted, which no board is under `framework =",
    "The Phase 2 lockdown in [secure_provisioning.md](secure_provisioning.md) is\n"
    "where secure boot and flash encryption are staged, prepared but not yet\n"
    "enabled: today it does not make you the last person who can change what runs\n"
    "on the board, and it does not protect the key or the saved Wi-Fi in NVS. It is\n"
    "irreversible, so it is an explicit opt-in and never a default.",
    "**Why the all-rounder Canary's dashboard asks to be unlocked on your home\n"
    "network.** (This and the next paragraph are the all-rounder Canary's,\n"
    "`firmware/canary`. A WAP's dashboard shows no such banner: its sign-in's\n"
    "**Use BOOT Tap Once** asks first, and you press BOOT only when the page\n"
    "says to.) The page",
    "On the all-rounder Canary each BOOT tap unlocks exactly one thing within\n"
    "those 30 seconds: one page load, or one browser fetching the recovery kit —\n"
    "whichever asks first. A page you unlocked with the tap can save the\n"
    "recovery kit without a second tap. On a WAP the tap unlocks no page load:\n"
    "it hands the recovery kit only to the one device asking for it, so click\n"
    "**Use BOOT Tap Once** (or **Save my recovery kit**) first and press BOOT\n"
    "when the page says to; if another device on your network asks during the\n"
    "same tap, the WAP hands out nothing and the page says so. The iPhone app",
    "// BOOT tap — which the page load then SPENDS (page_token_decide). On the\n"
    "// flagship, the one tree that serves a page by the tap, the tap is one\n"
    "// consumer across both paths: one page load or one receipt fetch, whichever\n"
    "// asks first (the ledger's F354). The WAP serves no page by the tap, and its\n"
    "// receipt's tap goes to one asker (tap_arbitrate() below). A page that got\n"
    "// the token already holds the bearer, so its \"Save recovery kit\" fetch\n"
    "// needs no second tap.",
    "// BOOT tap — which the page load then SPENDS (page_token_decide). On the\n"
    "// flagship, the one tree that serves a page by the tap, the tap is one\n"
    "// consumer across both paths: one page load or one receipt fetch, whichever\n"
    "// asks first (the ledger's F354). The WAP serves no page by the tap, and its\n"
    "// receipt's tap goes to one asker (tap_arbitrate() below). A page that got\n"
    "// the token already holds the bearer, so its \"Save recovery kit\" fetch\n"
    "// needs no second tap.",
    "// body's gate_ttl_seconds), so a press anywhere in our 60 s window lands\n"
    "// while this page is the only one asking (a WAP answers 409 tap_contended\n"
    "// when another address asked during the tap, sweep F342). The poll starts\n"
    "// before the press, and handlers.onTick's third argument says whether the\n"
    "// device has answered an ask yet (its first 403): a WAP hands a tap only to\n"
    "// the one address asking from 10 s before it onward, so until that answer\n"
    "// the step says not to press — a press the first ask has not reached yet is\n"
    "// one no page of the owner's is asking for, and goes to the first asker.",
    "      stepTitle,\n"
    "      stepSubtitle,",
    "// Models the WAP's BOOT-button provisioning gate (canary_wap.ino's\n"
    "// handle_provisioning_receipt) only in part: GET /api/provisioning-receipt\n"
    "// is unauthenticated — it is how a client obtains the API token in the\n"
    "// first place — returns 403 until the physical BOOT button is short-tapped,\n"
    "// and spends one tap on one receipt. Unlike the WAP since sweep F342, it\n"
    "// hands the tap to the first client that asks: the WAP serves it only to\n"
    "// the sole address that asked from 10 s before the tap onward, and a second\n"
    "// address closes the tap with 409 tap_contended (provisioning_gate.h's",
    "  // BOOT-button provisioning gate (a short tap opens a ~30 s window during\n"
    "  // which /api/provisioning-receipt returns the token to the first request;\n"
    "  // the WAP firmware hands the tap only to the sole address asking from 10 s\n"
    "  // before it and answers 409 tap_contended to a second, which this\n"
    "  // reference server does not model)",
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
    "| One member's stolen device key forging solo frames | The solo path's "
    "BOOT-button and no-fresh-cosigner gates run on the originating device and "
    "nothing about them is on the wire (§6.2 security note). A holder of one "
    "member's Ed25519 private key — readable from flash on a device without "
    "flash encryption — can sign a solo ALERT",
    "  app never dials a WAP's lane (ledger A65, open). Once that is settled the\n"
    "  lane holds only while that host leaves it alone, and taking it away needs\n"
    "  no radio: any landing session can turn the WAP's Bluetooth off, a saved",
    "  browser gives with no hold (its landing session reaches\n"
    "  `POST /api/wifi/connect` under the lockout), while that host leaves it\n"
    "  alone (the next three bullets). Run the attack again from the state",
    "- **Nation-state physical security.** Flash encryption defends key-at-rest and\n"
    "  casual readout, not decapping/fault-injection by a well-funded lab. We say so.",
    "   full Tier 4.** Dev mode is the sweet spot: it protects WiFi/keys at rest from a\n"
    "   thief while the device stays field-recoverable. Release mode (which disables",
    "- **Secure it (protect the key at rest) — already tooled, not yet burned.**\n"
    "  `docs/secure_provisioning.md` Phase 2 wraps Espressif's guide: **eFuse +",
    "  Secure Boot v2 + JTAG-off. Keeps the entire existing signing scheme; the key\n"
    "  can't be dumped off a locked device. Largely already tooled in",
    "| Boards with **flash encryption** enabled | ≥ 2 | Required for mesh (the "
    "`opera_secret` in NVS is only confidential under FE) |",
    '    "encryption and eFuse locks (then the flash reads back as ciphertext), " +',
    '    "For a board deployed somewhere physically exposed, use Phase-2 secure '
    'provisioning (flash encryption + eFuse).",',
    "  forever; a join that succeeds stores credentials and shuts the door; an\n"
    "  owner who finds the unit on the wrong network holds it, factory-resets it,\n"
    "  and starts again.",
    "  owner, and a unit joined to the wrong network is a unit its owner holds,\n"
    "  not one that has been taken; the fix is a factory reset. A deployment",
    "`/api/wifi/forget` route), which clears the stored network and opens the\n"
    "door again for half an hour from that moment. Holding a WAP's BOOT button",
    # #1843's sweep for every remaining copy, as each stood.
    "  //    and the address they serve has its backoff cleared (sweep F341,",
    "  section above: a tap clears an address only when it is served to that\n"
    "  address's ask; under that host the hold is no way out, the iPhone app",
    "token, on the receipt or on the page load, ends that address's backoff through `auth_handed_token()`, as the WAP's tap and",
    "// token ends its address's backoff (auth_handed_token, sweep F341), so a",
    "  it stops, or hold BOOT 10 s and set the unit up on a network that host\n"
    "  is not on. `test_auth_backoff.cpp`'s overflow row starts from the empty",
    "  *Amended once more 2026-10-21 (#1843's first review):* the bonded lane\n"
    "  gets the owner out only while that host holds no bond of its own. Any\n"
    "  landing session can pair a Bluetooth peer: it can turn pairing on",
    "  the way out F343's posture names from under a host striking from nine or",
    "You can't brick the device through the dashboard. You *can* lose your\n"
    "witness chain by factory-resetting it (the Ed25519 device key is",
    "for evidence, **export the chain before you factory-reset** —",
    "factory reset makes a new certificate, so pair the iPhone app again",
    "// generated once on the first TLS-capable boot (factory reset erases both,",
    "        log_error(\"Refusing to send the token. A factory reset makes a new certificate;\")",
    "    log_error \"Refusing to send the token. A factory reset makes a new certificate;\"",
    "  Serial.println(\"║  BOOT: tap=provisioning gate, 2s=info, 5s=factory reset      ║\");",
    "                + \"place — check the device itself. If you reset or replaced it, re-pair it \"",
    "        // sight then (TOFU) — a factory-reset device with a new key must not",
    "//     means the chain restarted (a wipe, a reset) — exactly the kind of",
    "// service and the BOOT-button portal can still rescue), never the fleet.",
    "        + \"(hold BOOT) will take the correction.\"",
    "            resolveProvision(.failed(\"This Canary's firmware doesn't offer the Bluetooth rescue — use its setup portal (hold BOOT) instead.\"))",
    "        # identity for this device_id (fresh flash, factory reset, a",
    "        # A newly pinned device starts its counters over (fresh flash, factory",
    "    device is (re)pinned via TOFU (async_record_verify), which is what a\n"
    "    factory reset looks like from here.",
    "to the KEY: a factory-reset or replaced Canary starts them over. So every",
    "    # Factory reset: the device pins fresh and its chain starts over.",
    "// polling a reset unit with its old token. A Table keeps one Slot per source",
    "// polling a reset unit with its old token. A Table keeps one Slot per source",
    "  and the likeliest such host was the owner's own phone, polling a reset unit\n"
    "  with its old token. Because a `static` in a header is a separate object in",
    "  still polling a reset unit with its old token (filed as F352). The backoff",
    "visiting browser's own address. A phone that still holds a pairing for a\n"
    "unit that was reset keeps polling with the old token, and so holds its own",
    "| No credentials stored (first boot, after a factory reset), within **30 minutes** of power-on | **Open** (`Door::NoCredentials`) | A device nobody owns yet. The window is `IMPROV_FIRST_BOOT_WINDOW_MS`",
    "  striker was the owner's own phone, still polling a reset unit with its old\n"
    "  token.\n"
    "  What it is now. The backoff is keyed by the request's socket peer address,",
    "  (iOS, a reset unit's stale pairing).",
    "  old token.** A factory reset that regenerates the WAP's key changes both\n"
    "  its token and its `device_id`. `DeviceStore.add`",
    "- [ ] **F352 [code] iOS: a pairing for a reset Canary keeps polling with its",
    "  untouched) and `docs/csi_developer_api.md` say so now; the canary-wap half\n"
    "  is unchanged. No behavior changed; not run on a device.\n"
    "- [x] **F83 [code] The canary-wap can commit an event before its id floor",
    "// stranger. A canary-wap has no reset path, so its owner flashes it with the",
    "// stranger. A canary-wap has no reset path, so its owner flashes it with the",
    "  sees a stranger; a canary-wap has no reset path, so its owner flashes it with the",
    "  `space_low()` on both trees: a factory reset on a canary, a full flash erase\n"
    "  on a canary-wap without restoring the flasher's pre-erase backup, then a",
    "                    \"then writes fresh. A factory reset and an install in one pass.\",",
    "      \"what\": \"Wipes every byte — firmware, settings, stored WiFi, witness history — then writes fresh. A factory reset and an install in one pass.\",",
    "        WAP has no factory-reset route at all (BOOT held ≥ 3 s is the only\n"
    "        path). Whether `POST /api/factory-reset` may exist is the decision;",
    "        never be able to wipe a witness), with a two-step confirmation in the",
    "  before 700 ms — the WAP's 2 s hold and the 5 s factory-reset hold are",
    " * a factory reset) is left untouched and unused. The backfill signs what it",
    "/* Erase the persisted trusted-peer list. Used on factory reset and",
    "      if (!confirm('Remove saved WiFi credentials from this Canary?')) return;",
    " *   - NO CREDENTIALS STORED (first boot, or after a factory reset): the",
    " *   - NO CREDENTIALS STORED (first boot, or after a factory reset): the",
    " * for a WAP nobody owns yet: while no credentials are stored, for the",
    "          PIN confirmation to prevent unauthorized access. Paired devices are stored securely and",
    "    meaning: \"Flash is encrypted with a key burned into this chip. Reads come back as ciphertext and a plaintext image won't boot. Not reversible.\",",
    "            meaning: \"Flash is encrypted with a key burned into this chip. Reads come back as ciphertext and a plaintext image won't boot. Not reversible.\",",
    " *      store IRKs only in NVS (flash-encrypted at rest on ESP32 with secure",
    "  `hh_count`. Its comment says the IRKs are \"flash-encrypted at rest on ESP32",
    "Until then, `household.h`'s claim that IRKs are \"flash-encrypted at rest on\n"
    "ESP32 with secure boot\" is wrong: flash encryption does not cover NVS, NVS",
    "// and opera_id derivation. On an unencrypted flash, a physical-access attacker",
    " * (under flash-encryption gate, mirroring opera_secret's protection).",
    " * (under flash-encryption gate, mirroring opera_secret's protection).",
    " * (mirroring opera_secret's hygiene — flash-encryption gate is the",
    " * (mirroring opera_secret's hygiene — flash-encryption gate is the",
    "      check_fail \"Mesh impl '$rel' has no esp_flash_encryption_enabled() gate — the opera_secret must never be persisted to unencrypted NVS (#610)\"",
    " * and the secure kit in firmware/provisioning/ is the path up —",
    " * and the secure kit in firmware/provisioning/ is the path up —",
    " * and the secure kit in firmware/provisioning/ is the path up —",
    "  // (common/identity/key_at_rest.h Tier 0); the secure kit is the path up.",
    "  // (common/identity/key_at_rest.h Tier 0); the secure kit is the path up.",
    "  // (common/identity/key_at_rest.h Tier 0); the secure kit is the path up.",
    "| C9 | Credentials encrypted at rest | NVS encryption or flash encryption on the credential blobs |",
    "   Secure Boot v2) stays the documented path. This is the one brief item the",
    "opt-in secure kit (flash encryption + Secure Boot v2) that no CI leg builds.\n"
    "NVS encryption is not configurable on either pinned Arduino core as built",
    "honest `⚠️`: posture reported on every node, the secure kit documented as the path; no fake encryption |",
    "  posture is reported as `creds_at_rest` on every node and the secure kit is\n"
    "  the documented path (PARITY footnote 1).",
    "3. **Key extraction.** The identity key is plaintext-at-rest. Without flash\n"
    "   encryption or a hardware-bound key, physical possession = key compromise,\n"
    "   which undermines both attestation and the pinned-identity model.",
    "still exists but the ROM **refuses to boot an unsigned image**, and flash reads\n"
    "come back ciphertext — so the browser flasher's \"write any factory image and boot",
    "firmware\"* actually hold. Tier 3's non-extractable key (§5.4) additionally stops",
    "  disables plaintext serial programming. Protects at-rest, **not** a live chip.",
    "  the **ESP32-S3's own eFuse / flash-encryption / DS peripheral** (device) holds —",
    "  path **(a)** hardens the ESP32-S3 Ed25519 identity *at rest* by burning",
    "re-advertise mDNS, republish current signals. Pairings persist (encrypted NVS / hub keystore); nothing else is worth",
    "  NVS; they get the `opera_secret` treatment — refuse to persist on\n"
    "  flash-encryption-off devices (the tiered story from",
    "  at-rest protection of the NVS-stored broker credentials relies on the\n"
    "  flash-encrypted secure build (`provisioning/sdkconfig.defaults.secure`), not",
    "degrades neighbors' life-safety is a footgun. Device seizure is already handled by flash encryption + the witness kernel's",
    "deferral, but it ships today: either enable flash encryption + secure boot for\n"
    "production builds, or state the exposure explicitly in the threat model and on",
    "firmware signatures + dictionary hash | plaintext-NVS key on current canaries until flash encryption ships (doc 12 F2) — the honest",
    "- Reuses the existing key-management infrastructure (NVS, eFuse-derived flash encryption, key generation at first boot).",
    "- JTAG permanently disabled via eFuse\n"
    "- NVS encryption enabled",
    "`sdkconfig.defaults.secure` | ESP-IDF Kconfig with BT disabled, Secure Boot v2 **prepared but not enabled**, Flash Encryption release mode, NVS encryption, anti-rollback, JTAG off |",
    "# It encrypts NVS partition contents using a key stored in eFuse.\n"
    "#\n"
    "CONFIG_NVS_ENCRYPTION=y",
    "`OperaConfig.opera_secret[32]` is the 32-byte symmetric secret from which `opera_id` is derived and which gates pairing. If the device's flash is not encrypted (ESP32 flash encryption is opt-in, not on by default outside production builds), a physical-access attacker can pull the secret with `esptool.py read_flash` and become a permanent opera member from outside the household.\n"
    "\n"
    "**Fix in this branch:** in `mesh_network::init`, call `esp_efuse_read_field_bit(ESP_EFUSE_FLASH_CRYPT_CNT)` (or the equivalent SDK helper) and refuse to load/save the opera_secret if flash encryption is not active. Log loudly to the health log at `LOG_LEVEL_ALERT, LOG_CAT_SECURITY`. Document the requirement in `spec/canary_mesh_network_v0.md` and `docs/security/SECURITY_MODEL.md`.",
    "| NVS confidentiality | M | No FE gate (O2). |",
    "  SD — pure append, never truncated — with the 64-entry flash-encrypted NVS\n"
    "  ring demoted to a recent-view cache (`beacon_channel.cpp::sd_append_audit_entry`).",
    "but aren't wired into the FULL build — identity keys and the chain sit in plaintext flash today; a\n"
    "PSRAM-backed deferred-write ring would stop record loss while the SD card is absent (the mechanism",
    "proves on hardware that **no mesh secret lands in unencrypted NVS**.",
    "| C2 | **FE secret-at-rest gate** | pair on a **non-FE**",
    "   `opera_secret` can never be silently persisted to unencrypted NVS — the static half of issue",
    "- The household key never leaves the device cleartext; it lives in NVS, encrypted by the device's secure-element-backed key (where available).",
    "  We know a physical attacker can dump flash and recover the identity key. What",
    "| `secure` / `secure_ha` | the provisioning kit's Tier 3/4 image, flash encryption required (`provisioning/platformio_secure.ini`)",
    "If you want to be the last person who can ever do this to that board, the",
    "**Why the dashboard asks to be unlocked on your home network.** The page",
    "Each BOOT tap unlocks exactly one thing within those 30 seconds: one\n"
    "page load, or one app or browser fetching the recovery kit — whichever",
    "without a second tap; the iPhone app fetching the kit on its own needs a\n"
    "fresh tap. That is how you get the kit back later without a USB cable. *(New in 2026-09; checked by automated tests, with its first automated build pending; not",
    "// BOOT tap — which the page load then SPENDS (page_token_decide). The tap is\n"
    "// one consumer across both paths: one page load or one receipt fetch,",
    "// BOOT tap — which the page load then SPENDS (page_token_decide). The tap is\n"
    "// one consumer across both paths: one page load or one receipt fetch,",
    "// body's gate_ttl_seconds), so a press anywhere in our 60 s window lands.\n"
    "// Returns a handle with cancel(); callers must cancel on navigation.",
    "      el('div', { className: 'pair-step-title', textContent: 'Short-tap the BOOT button' }),",
    "// Mirrors the firmware's BOOT-button provisioning gate (wap_server.h):",
    "  // BOOT-button provisioning gate (mirrors firmware: a short-tap opens a",
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


if __name__ == "__main__":
    unittest.main()
