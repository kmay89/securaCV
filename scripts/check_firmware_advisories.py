#!/usr/bin/env python3
"""scripts/check_firmware_advisories.py — published advisories against the
firmware's exact third-party versions.

Dependabot has no PlatformIO or Arduino ecosystem, and audit.yml's other jobs
read Cargo and npm lockfiles only, so nothing used to look at the libraries a
Canary image links (NimBLE-Arduino parses every BLE advertisement in radio
range, PubSubClient every MQTT packet from the broker). OSV and the GitHub
Advisory Database have no ecosystem for Arduino C/C++ libraries or ESP-IDF,
so a scanner fed the SBOM's purls would match nothing and pass. This check
reads the upstreams' own records instead:

  1. sbom/sbom-firmware.cdx.json (scripts/gen_firmware_sbom.py) is the list
     of what the firmware is built from. Every third-party component in it
     (group other than `securacv`) must carry an exact `version`, and its
     name must map to an upstream repository in UPSTREAM below. Either gap
     fails the check, so it cannot pass on a component it did not compare.
     The one exception is an Arduino core with no version that only CI rows
     outside RELEASE_WORKFLOWS build (firmware.yml's PR row on the action's
     "latest", the early warning for the next core): it ships in nothing and
     is listed as not checked.
  2. For each upstream repository, its published GitHub repository security
     advisories (`GET /repos/{owner}/{repo}/security-advisories`), compared
     with every pinned version mapped to it. An affected range that matches
     fails the check. A range this script cannot read (missing, or not the
     `>= a, < b` form) fails it too, until a reviewer records in REVIEWED why
     the advisory does not apply to the pinned versions.

What it does NOT cover, said where the output is read:
  - advisories that are not GitHub repository advisories of these repos:
    Espressif publishes ESP-IDF and chip advisories on its own site, and a
    CVE filed only with NVD has no repository advisory;
  - the libraries ESP-IDF bundles (mbedTLS, lwIP, FreeRTOS, cJSON): the SBOM
    does not enumerate them, so nothing here compares their versions;
  - transitive dependencies of the libraries, which the SBOM does not list.

No secret is needed: the job's own `GITHUB_TOKEN` (GH_TOKEN) is sent when the
environment has one, for the rate limit; GitHub's REST docs list this endpoint
as readable without authentication for a public repository.

Usage:
    python3 scripts/check_firmware_advisories.py              # query and compare
    python3 scripts/check_firmware_advisories.py --coverage   # step 1 only, offline

Exit 0: every third-party component was compared and none is affected.
Exit 1: an affected version, a coverage gap, or an advisory nobody has read.
Exit 2: an upstream could not be queried (so nothing was compared for it).
Tests: scripts/tests/test_check_firmware_advisories.py.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SBOM = REPO / "sbom" / "sbom-firmware.cdx.json"
API = "https://api.github.com"

# The workflows whose Arduino-CLI rows build an image that ships; the same
# tuple as scripts/gen_firmware_sbom.py's (the unit tests hold them equal —
# this script reads only the SBOM, so it does not import the generator).
RELEASE_WORKFLOWS = ("firmware-release.yml", "flasher-release.yml")

# Component name (as the SBOM writes it) → the GitHub repository whose
# security advisories cover it. A third-party component whose name is not
# here fails the check: add the row in the change that adds the library.
UPSTREAM: dict[str, str] = {
    "arduino-esp32": "espressif/arduino-esp32",
    "esp-idf": "espressif/esp-idf",
    "NimBLE-Arduino": "h2zero/NimBLE-Arduino",
    "ArduinoJson": "bblanchon/ArduinoJson",
    "PubSubClient": "knolleary/pubsubclient",
    # Crypto is one library of rweather/arduinolibs; every advisory there is
    # compared with it, whichever library of the repo it names.
    "Crypto": "rweather/arduinolibs",
    "lvgl": "lvgl/lvgl",
    "GFX Library for Arduino": "moononournation/Arduino_GFX",
    "Seeed_Arduino_SSCMA": "Seeed-Studio/Seeed_Arduino_SSCMA",
    "platformio/espressif32": "platformio/platform-espressif32",
    "pioarduino/platform-espressif32": "pioarduino/platform-espressif32",
}

# GHSA id → why it does not affect the pinned versions, for an advisory whose
# range this script cannot read or that a reviewer has ruled out. An entry is
# a reviewed claim about the versions pinned today; remove it when a pin moves.
# Reviewed 2026-11-16 at arduino-esp32 2.0.17 and 3.3.11, ESP-IDF 4.4.7 and
# 5.5.5. Each names the code that is not linked, or the fix and its test.
REVIEWED: dict[str, str] = {
    "GHSA-8cmm-3887-r32j": (
        "In range for 2.0.17 only (3.3.11 carries upstream's fix), and every core-2 build that "
        "compiles WebServer links firmware/third_party/WebServer-2.0.17-securacv/src/Parsing.cpp "
        "instead of the core's copy: a boundary over 70 bytes is refused and the buffer is "
        "fixed-size. scripts/tests/test_vendored_webserver_core2.py holds every such env and "
        "release row to that copy and firmware/tests_host/test_webserver_boundary.cpp drives "
        "the parser under AddressSanitizer; compile-tested and host-tested, not bench-tested. "
        "firmware/canary serves HTTP on esp_http_server "
        "(firmware/canary/lib/securacv_network/src/securacv_network.h) and links no WebServer."
    ),
    "GHSA-28hv-fwm3-rpcq": (
        "Digest-auth URI replay in WebServer::authenticate(), in range for 2.0.17 only. No "
        "firmware source calls authenticate() or requestAuthentication(): the WebServer users "
        "are firmware/common/network/node_http.cpp, setup_portal.cpp and the display's "
        "glass_web.cpp and provision.cpp, and the display gates writes with its own Host/Origin "
        "check and per-boot CSRF token (firmware/projects/canary-display/src/net/glass_web.cpp)."
    ),
    "GHSA-92j9-c75g-2c5f": (
        "NBNS name_len overflow in the core's NetBIOS library, in range for 2.0.17 only. No "
        "firmware source includes NetBIOS.h or calls NBNS.begin(), so the library is not "
        "compiled into any image and nothing listens on UDP/137; LAN naming is ESPmDNS "
        "(firmware/projects/canary-vision/src/net/mdns_mgr.cpp)."
    ),
    "GHSA-9vfw-wx65-c872": (
        "CSRF on HTTPUpdateServer's /update upload and two OTA example sketches, in range for "
        "2.0.17 only (< 3.2.1). No firmware includes HTTPUpdateServer or registers a WebServer "
        "upload handler; the display's /api/ota/* routes take no body and start the signed "
        "pull-OTA (firmware/projects/canary-display/src/net/glass_web.cpp), and firmware/canary's "
        "upload handler is compiled out of every SECURACV_BUILD_RELEASE image "
        "(firmware/canary/lib/securacv_network/src/securacv_network.cpp)."
    ),
    "GHSA-vqp8-ppw3-8mc7": (
        "Path traversal in WebServer::serveStatic(); the range '<=3.3.11, 4.0.0-alpha1' is "
        "unreadable here, and covers both pins, 2.0.17 and 3.3.11 (the fix is in 3.3.12). No "
        "firmware source calls serveStatic() or registers its own RequestHandler: every route in "
        "firmware/common/network/node_http.cpp, setup_portal.cpp and the display's "
        "glass_web.cpp and provision.cpp is a fixed on()/onNotFound() handler."
    ),
    "GHSA-w887-pg2w-mfj2": (
        "Plaintext WebServer::authenticate(user, pass) and AuthenticationMiddleware accept a "
        "bare username; the range '<=3.3.11, 4.0.0-alpha1' is unreadable here, covers 2.0.17 "
        "and 3.3.11 alike (the fix is in 3.3.12). No firmware source calls authenticate() "
        "or uses AuthenticationMiddleware; the display's write gate is its own Origin and CSRF "
        "check (firmware/projects/canary-display/src/net/glass_web.cpp)."
    ),
    "GHSA-5476-9jjq-563m": (
        "CRLF injection through WebServer::sendHeader(); the range '<= 3.3.0-alpha1, <= 3.2.0' "
        "is unreadable here, covers 2.0.17 and excludes 3.3.11, which rejects CR/LF. Every "
        "sendHeader() call in firmware/common/network/node_http.cpp, setup_portal.cpp and the "
        "display's glass_web.cpp and provision.cpp passes a literal name and value, so no "
        "request data reaches a header."
    ),
    "GHSA-rhcj-7qrh-rrwh": (
        "A pull_request_target weakness in upstream's own dangerjs.yml workflow, ranged by one "
        "master commit, not a version. It is not code in the 2.0.17 or 3.3.11 core packages we "
        "install (firmware/envs/platformio/platforms.ini and the arduino-cli rows), so no image "
        "contains it, and the advisory reports no tampered release."
    ),
    "GHSA-h52q-xhg2-6jw8": (
        "Poisoned-pipeline weaknesses in upstream's tests_results.yml and tests_wokwi.yml "
        "workflows, ranged by commits (fixed in a7cec020, 2024), not versions. It is not code "
        "in the 2.0.17 or 3.3.11 core packages we install (firmware/envs/platformio/"
        "platforms.ini and the arduino-cli rows), so no image contains it, and the advisory "
        "reports no tampered release."
    ),
    "GHSA-9w88-r2vm-qfc4": (
        "Range 'NA': the flaws are in ESP-IDF's BluFi example (examples/bluetooth/blufi/main/"
        "blufi_example_main.c, blufi_security.c), which no Arduino core links at 4.4.7 or 5.5.5. "
        "No firmware source calls esp_blufi_*; BLE onboarding is Improv over NimBLE-Arduino "
        "(firmware/common/network/improv_ble.cpp)."
    ),
    # The four below name ESP-IDF v5.5.5 (fixed in 5.5.6). Two independent
    # reviews read each against the sdkconfig and archives the 3.3.11 core
    # ships prebuilt (esp32-core-3.3.11-libs, the esp32-arduino-libs that
    # pioarduino 55.03.311 names; the arduino-cli esp32s3-libs zip carries
    # the same sdkconfig, liblwip.a and libbt.a; CONFIG_IDF_INIT_VERSION
    # "5.5.5"). Every core-3 image is an ESP32-S3 or ESP32-C6
    # (firmware/envs/platformio/platforms.ini's [platform_core3] users and the
    # 3.3.11 arduino-cli rows), and no env rebuilds the IDF (no
    # custom_sdkconfig, no board_espidf.*, no framework = arduino, espidf), so
    # those prebuilt libs are the ones linked. PrebuiltSdkconfigVerdicts in
    # scripts/tests/test_check_firmware_advisories.py fails if either changes.
    "GHSA-ch63-jxm5-vmvf": (
        "lwIP dns_lookup() overruns its caller's one-address buffer only when "
        "CONFIG_LWIP_DNS_MAX_HOST_IP is 2 or more, and the ESP-IDF 5.5.5 that arduino-esp32 "
        "3.3.11 ships prebuilt sets it to 1 on every chip (esp32s3, esp32c6, esp32c3, esp32), so "
        "a cache entry holds one address. No env rebuilds the IDF (no custom_sdkconfig, no "
        "board_espidf.custom_sdkconfig, no framework = arduino, espidf; "
        "scripts/tests/test_check_firmware_advisories.py holds that), so the stock libs pinned "
        "by firmware/envs/platformio/platforms.ini [platform_core3] are the ones linked. The "
        "advisory names ESP-IDF 5.1 and earlier unaffected, so core 2.0.17's 4.4.7 is out of range."
    ),
    "GHSA-49x8-xqjg-2pjg": (
        "The flaw is in ESP-TEE's RISC-V trap entry, which is compiled only into a build with "
        "CONFIG_SECURE_ENABLE_TEE and a tee partition, and is reached only by code already "
        "running on the device; the 3.3.11 core's prebuilt ESP-IDF 5.5.5 leaves that option "
        "unset on esp32c6 and links no esp_tee library, and the ESP32-S3 has no ESP-TEE at all. "
        "No C6 env rebuilds the IDF or carries a tee partition "
        "(firmware/projects/canary-display/partitions_display_4mb.csv, min_spiffs.csv in "
        "firmware/envs/platformio/canary-sense.ini and canary-sentinel.ini), so no Canary image "
        "contains the trap code."
    ),
    "GHSA-rjvf-fxv5-x87j": (
        "The overflow is in the Bluedroid BLE Mesh adapter's bt_mesh_bta_gatts_cb, which "
        "ESP-IDF 5.5.5 compiles only with CONFIG_BLE_MESH_NODE plus PB_GATT or the GATT proxy "
        "server, and the 3.3.11 core's prebuilt libs leave NODE and PB_GATT unset on every chip, "
        "so the handler is absent even from the classic-ESP32 Bluedroid libbt.a. The advisory "
        "also names ESP-IDF 5.1 and earlier: 4.4.7 compiles the handler under the same guard "
        "(mesh_bearer_adapt.c), and arduino-esp32 2.0.17's prebuilt libs leave NODE and PB_GATT "
        "unset too, so its esp32, esp32s3 and esp32c3 libbt.a carry no bt_mesh_bta_gatts_cb. "
        "Every core-3 image is an ESP32-S3 or ESP32-C6 built with the NimBLE host "
        "(firmware/envs/platformio/platforms.ini), no firmware source calls esp_ble_mesh_*, and "
        "our BLE is NimBLE-Arduino (firmware/common/network/improv_ble.cpp)."
    ),
    "GHSA-xcpr-5mqp-9qvv": (
        "The A2DP sink parser runs only after a paired Classic Bluetooth audio source connects "
        "to a Bluedroid build that called esp_a2d_sink_init; the ESP32-S3 and ESP32-C6 have no "
        "Classic Bluetooth radio, and the 3.3.11 core's prebuilt ESP-IDF 5.5.5 builds them "
        "without Bluedroid or A2DP. No core-3 env targets the classic ESP32 (every user of "
        "firmware/envs/platformio/platforms.ini's core-3 pin is an S3 or C6, and the classic "
        "esp32cam and esp32-wroom envs in firmware/canary/platformio.ini stay on core 2.0.17, "
        "whose ESP-IDF 4.4.7 the advisory does not list), and no firmware source calls "
        "esp_a2d_* or esp_bt_gap_*."
    ),
}

_CONSTRAINT_RE = re.compile(r"^(>=|<=|==|=|>|<)?\s*v?(\d+(?:[.-]\d+)*)$")


def version_key(text: str) -> tuple[int, ...] | None:
    """`5.5.4` / `v5.5.4` / `55.03.38-1` → a comparable tuple (trailing zeros
    dropped, so 2.8 == 2.8.0); anything else (a pre-release tag) → None."""
    m = re.fullmatch(r"v?(\d+(?:[.-]\d+)*)", text.strip())
    if not m:
        return None
    parts = [int(p) for p in re.split(r"[.-]", m.group(1))]
    while len(parts) > 1 and parts[-1] == 0:
        parts.pop()
    return tuple(parts)


def parse_range(text: str | None) -> list[tuple[str, tuple[int, ...]]] | None:
    """GitHub's `vulnerable_version_range` (`>= 1.0.0, < 1.4.3`, `<= 2.5.1`,
    `= 7.0.0`) → [(op, version)]; None when it is missing or not that form."""
    if not text or not text.strip():
        return None
    out: list[tuple[str, tuple[int, ...]]] = []
    for part in text.split(","):
        m = _CONSTRAINT_RE.match(part.strip())
        if not m:
            return None
        key = version_key(m.group(2))
        if key is None:
            return None
        out.append(({"==": "=", None: "="}.get(m.group(1), m.group(1)), key))
    return out


def in_range(version: tuple[int, ...], constraints: list[tuple[str, tuple[int, ...]]]) -> bool:
    def cmp(a: tuple[int, ...], b: tuple[int, ...]) -> int:
        n = max(len(a), len(b))
        a, b = a + (0,) * (n - len(a)), b + (0,) * (n - len(b))
        return (a > b) - (a < b)
    ops = {"=": lambda c: c == 0, ">": lambda c: c > 0, ">=": lambda c: c >= 0,
           "<": lambda c: c < 0, "<=": lambda c: c <= 0}
    return all(ops[op](cmp(version, bound)) for op, bound in constraints)


def _prop(component: dict, name: str) -> str:
    return next((p["value"] for p in component.get("properties") or []
                 if p.get("name") == name), "")


def third_party(doc: dict) -> list[dict]:
    """The components not built from this repository's own sources."""
    return [c for c in doc.get("components") or [] if c.get("group") != "securacv"]


def _floats_on_ci_only(component: dict) -> bool:
    """An Arduino core with no version that only non-release CI rows build."""
    if component.get("name") != "arduino-esp32" or component.get("version"):
        return False
    paths = [p.strip() for p in _prop(component, "securacv:build_paths").split(";") if p.strip()]
    rows = [re.fullmatch(r"arduino-cli \(([^:()]+):[^()]+\)", p) for p in paths]
    return bool(rows) and all(r and r.group(1) not in RELEASE_WORKFLOWS for r in rows)


def coverage(doc: dict) -> tuple[list[dict], list[str], list[str]]:
    """(the components to compare, the gaps that fail, the not-checked notes)."""
    checked: list[dict] = []
    gaps: list[str] = []
    skipped: list[str] = []
    for c in third_party(doc):
        ref = c.get("bom-ref", c.get("name"))
        if _floats_on_ci_only(c):
            skipped.append(f"{ref}: the action's latest core on CI-only rows "
                           f"({_prop(c, 'securacv:build_paths')}); ships in no image")
            continue
        version = str(c.get("version") or "")
        if not version or version_key(version) is None:
            spec = _prop(c, "securacv:version_spec") or "(none)"
            gaps.append(f"{ref} has no exact version (spec {spec}, used by "
                        f"{_prop(c, 'securacv:envs') or _prop(c, 'securacv:build_paths') or '?'}"
                        f"): pin it, or its advisories cannot be compared")
            continue
        if c.get("name") not in UPSTREAM:
            gaps.append(f"{ref}: no upstream repository for {c.get('name')!r} in "
                        f"scripts/check_firmware_advisories.py UPSTREAM")
            continue
        checked.append(c)
    return checked, gaps, skipped


def fetch_advisories(repo: str, token: str | None = None) -> list[dict]:
    """Every published repository security advisory of `repo` (all pages)."""
    url: str | None = f"{API}/repos/{repo}/security-advisories?state=published&per_page=100"
    out: list[dict] = []
    while url:
        req = urllib.request.Request(url, headers={
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "User-Agent": "securacv-firmware-advisories",
            **({"Authorization": f"Bearer {token}"} if token else {}),
        })
        with urllib.request.urlopen(req, timeout=30) as resp:  # noqa: S310 — fixed https host
            page = json.loads(resp.read().decode("utf-8"))
            link = resp.headers.get("Link") or ""
        if not isinstance(page, list):
            raise ValueError(f"{repo}: expected a list of advisories, got {type(page).__name__}")
        out.extend(page)
        nxt = re.search(r'<([^>]+)>;\s*rel="next"', link)
        url = nxt.group(1) if nxt else None
    return out


def compare(components: list[dict], advisories: dict[str, list[dict]]) -> tuple[list[str], list[str]]:
    """(affected, unreadable) findings for every component against its
    upstream's advisories. A withdrawn advisory is skipped; a REVIEWED one is
    skipped with its reason printed by the caller."""
    affected: list[str] = []
    unreadable: list[str] = []
    for c in components:
        repo = UPSTREAM[c["name"]]
        ver = version_key(str(c["version"]))
        for adv in advisories.get(repo, []):
            ghsa = adv.get("ghsa_id") or adv.get("html_url") or "?"
            if adv.get("withdrawn_at") or ghsa in REVIEWED:
                continue
            title = f"{ghsa} ({adv.get('severity') or 'severity unset'}: {adv.get('summary') or ''})"
            vulns = adv.get("vulnerabilities") or []
            if not vulns:
                unreadable.append(f"{c['bom-ref']} {c['version']}: {repo} {title} names no "
                                  f"affected range — read it and record the verdict in REVIEWED")
                continue
            for v in vulns:
                pkg = ((v.get("package") or {}).get("name")) or "?"
                rng = parse_range(v.get("vulnerable_version_range"))
                if rng is None:
                    unreadable.append(f"{c['bom-ref']} {c['version']}: {repo} {title} package "
                                      f"{pkg} range {v.get('vulnerable_version_range')!r} is not "
                                      f"machine-readable — read it and record the verdict in "
                                      f"REVIEWED")
                elif in_range(ver, rng):
                    affected.append(f"{c['bom-ref']} {c['version']} is in {repo} {title} "
                                    f"package {pkg} range "
                                    f"{v.get('vulnerable_version_range')!r}; patched: "
                                    f"{v.get('patched_versions') or 'none listed'}")
    return affected, unreadable


NOT_COVERED = (
    "Not covered: advisories that are not GitHub repository advisories of the "
    "repos above (Espressif's own advisory page; a CVE with no repository "
    "advisory); the libraries ESP-IDF bundles (mbedTLS, lwIP, FreeRTOS, cJSON — "
    "not enumerated in the SBOM); transitive dependencies of the libraries."
)


def main(argv: list[str] | None = None, fetch=fetch_advisories) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sbom", type=Path, default=SBOM, help="the firmware SBOM to read")
    ap.add_argument("--coverage", action="store_true",
                    help="only the coverage step (exact versions, known upstreams); no network")
    args = ap.parse_args(argv)
    try:
        doc = json.loads(args.sbom.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"::error::check_firmware_advisories.py: cannot read {args.sbom}: {exc}")
        return 1
    checked, gaps, skipped = coverage(doc)
    for note in skipped:
        print(f"not checked: {note}")
    for gap in gaps:
        print(f"::error::coverage: {gap}")
    if args.coverage:
        print(f"check_firmware_advisories.py --coverage: {len(checked)} third-party "
              f"component(s) comparable, {len(gaps)} gap(s)")
        return 1 if gaps else 0

    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or None
    repos = sorted({UPSTREAM[c["name"]] for c in checked})
    advisories: dict[str, list[dict]] = {}
    failed: list[str] = []
    for repo in repos:
        try:
            advisories[repo] = fetch(repo, token)
        except (urllib.error.URLError, OSError, ValueError) as exc:
            failed.append(f"{repo}: {exc}")
    for f in failed:
        print(f"::error::could not read the advisories of {f} — nothing was compared for it")
    affected, unreadable = compare(checked, advisories)
    for line in affected:
        print(f"::error::affected: {line}")
    for line in unreadable:
        print(f"::error::unread: {line}")
    for ghsa, why in sorted(REVIEWED.items()):
        print(f"reviewed, not counted: {ghsa} — {why}")
    print(f"check_firmware_advisories.py: {len(checked)} third-party component(s) at an "
          f"exact version, {len(repos)} upstream repo(s), "
          f"{sum(len(v) for v in advisories.values())} published advisory(ies) read; "
          f"{len(affected)} affected, {len(unreadable)} unread, {len(gaps)} coverage gap(s), "
          f"{len(failed)} repo(s) unreachable.")
    print(NOT_COVERED)
    if failed:
        return 2
    return 1 if (affected or unreadable or gaps) else 0


if __name__ == "__main__":
    sys.exit(main())
