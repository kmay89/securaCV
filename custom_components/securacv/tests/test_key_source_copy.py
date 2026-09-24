"""The options flow must say where each product's key comes from.

The pin / rotate / unpin menu used to tell every operator to read the
fingerprint and pubkey hex off "its /enroll page", but only canary-wap
serves that route (canary_wap.ino's register_api_routes). The
firmware/canary build and canary-vision print the key on USB serial when
asked (`j`); canary-sense and canary-sentinel, on a firmware release
after 2.4.15, print it once at boot on their serial console, as the
`Ed25519 pubkey` line; a canary-display has no key at all.
docs/device_trust.md ("Where each product shows its key") holds the full
table, read from source.

These tests pin properties the copy has to keep, not its wording:

- every clause that offers /enroll names canary-wap and no other product,
  and no "any" / "each device" style word that would widen it again;
- the menu text names each product line in PRODUCT_LINES, and in the
  monorepo PRODUCT_LINES is held to firmware/flavors.json, so a flavor added
  there without saying here where its key is read fails (the HACS mirror has
  no firmware/ and skips that one cross-check);
- the pin step names a source for every product that signs and none for
  the one with no key, and in the monorepo the boot line it names for
  canary-sense and canary-sentinel is held to their witness.cpp, and the
  `Device ID` line it names beside it to their main.cpp;
- wherever the copy names that boot line it also names the release it
  first ships after, because no image up to that release prints it and
  the copy must not promise the key on every unit in the field;
- the pin form's error text claims no rule its validator does not enforce.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any, Iterator

import pytest

from . import conftest  # noqa: F401  (installs/augments ha stubs at import time)
from ..config_flow import _looks_like_pubkey_hex

PACKAGE_DIR = Path(__file__).resolve().parent.parent
STRINGS = PACKAGE_DIR / "strings.json"
# The monorepo's firmware tree. Absent in the HACS mirror checkout.
FIRMWARE = Path(__file__).resolve().parents[3] / "firmware"
FLAVORS = FIRMWARE / "flavors.json"

# Every product line the options menu has to account for: the ones that
# sign (and so can be pinned) and the display line, which signs nothing.
PRODUCT_LINES = (
    "canary-wap",
    "firmware/canary",
    "canary-vision",
    "canary-sense",
    "canary-sentinel",
    "canary-display",
)

# The product lines with no signing key, so nothing to pin.
KEYLESS_LINES = ("canary-display",)

# The products whose only full-key source is a line in the boot log, and
# the file whose init() prints it.
BOOT_LINE_WITNESS = {
    "canary-sense": "projects/canary-sense/src/witness.cpp",
    "canary-sentinel": "projects/canary-sentinel/src/witness.cpp",
}

# The same products' main.cpp, whose setup() prints the device_id on a
# `Device ID` boot line: the pin form's other field.
BOOT_LINE_MAIN = {
    "canary-sense": "projects/canary-sense/src/main.cpp",
    "canary-sentinel": "projects/canary-sentinel/src/main.cpp",
}


def _strings(node: Any, path: str = "") -> Iterator[tuple[str, str]]:
    if isinstance(node, dict):
        for key, value in node.items():
            yield from _strings(value, f"{path}.{key}" if path else key)
    elif isinstance(node, str):
        yield path, node


def _load() -> dict[str, Any]:
    return json.loads(STRINGS.read_text(encoding="utf-8"))


# A clause ends at any of these; "/enroll" contains none of them.
_CLAUSE_BREAK = re.compile(r"[.,;:()]")
# Words that would widen an /enroll clause back to every device.
_WIDENING = re.compile(r"\b(any|each|every|all|other)\b", re.IGNORECASE)


def _enroll_offenders(data: dict[str, Any]) -> list[tuple[str, str]]:
    out = []
    for path, text in _strings(data):
        for clause in _CLAUSE_BREAK.split(text):
            if "/enroll" not in clause:
                continue
            named = {line for line in PRODUCT_LINES if line in clause}
            if named != {"canary-wap"} or _WIDENING.search(clause):
                out.append((path, clause.strip()))
    return out


def test_enroll_is_offered_only_as_canary_wap_source() -> None:
    offenders = _enroll_offenders(_load())
    assert not offenders, (
        f"{offenders}: a clause that offers /enroll must name canary-wap and "
        "nothing wider; the other products have no such route"
    )


def test_enroll_check_catches_the_claim_it_replaced() -> None:
    # The two descriptions as they read before this test existed. Both have
    # "canary-wap" somewhere in the string, which is why a whole-string check
    # passed them; the clause check must not.
    old = {
        "init": (
            "Read each device's fingerprint and pubkey hex from its /enroll "
            "page on the local network (canary-wap, firmware/canary, "
            "canary-vision, canary-sense, canary-sentinel, canary-display)."
        ),
        "pin": (
            "Paste the device_id and full 64-character Ed25519 pubkey hex "
            "shown on the device's /enroll page (any canary-wap or other Canary)."
        ),
        "widened": "the /enroll page on a canary-wap or any other Canary",
    }
    flagged = {path for path, _ in _enroll_offenders(old)}
    assert flagged == {"init", "pin", "widened"}


def test_options_menu_names_every_product_line() -> None:
    text = _load()["options"]["step"]["init"]["description"]
    missing = [line for line in PRODUCT_LINES if line not in text]
    assert not missing, (
        f"the options menu does not say where {missing} show their key "
        "(or that they have none); see docs/device_trust.md"
    )


def test_product_lines_match_the_flavor_registry() -> None:
    if not FLAVORS.exists():
        pytest.skip("firmware/flavors.json not present (HACS mirror checkout)")
    flavors = json.loads(FLAVORS.read_text(encoding="utf-8"))
    # The flagship's flavor is named "canary"; the copy calls it by its
    # directory, firmware/canary, since "canary" alone names every product.
    names = {"firmware/canary" if f["name"] == "canary" else f["name"] for f in flavors}
    assert names == set(PRODUCT_LINES), (
        "firmware/flavors.json and PRODUCT_LINES disagree: say in the options "
        "menu (and docs/device_trust.md) where the new product's key is read, "
        "then list it here"
    )


def test_pubkey_error_claims_no_rule_the_validator_skips() -> None:
    key = "ab" * 32
    error = _load()["options"]["error"]["invalid_pubkey_hex"]
    # The form lowercases before it validates, so case never fails a paste,
    # and the error must not send someone chasing it. canary-wap's serial `i`
    # prints the key in capitals followed by "...": that fails, on the dots.
    assert _looks_like_pubkey_hex(key.upper())
    assert not _looks_like_pubkey_hex(key.upper() + "...")
    assert "lowercase" not in error.lower(), error


def test_pin_step_names_a_source_for_every_signing_product() -> None:
    # The pin step needs the full 64-hex key. Every product that signs shows
    # it somewhere off the network, so each must be named there with its
    # source; the product with no key must not be offered at all.
    text = _load()["options"]["step"]["pin"]["description"]
    missing = [
        line for line in PRODUCT_LINES
        if line not in KEYLESS_LINES and line not in text
    ]
    assert not missing, f"the pin step names no key source for {missing}"
    for line in KEYLESS_LINES:
        assert line not in text, f"{line} has no key to paste into the pin step"


_BOOT_KEY_PRINT = re.compile(
    r'printf\(\s*"([^"%]+) %s\\n",\s*device_signature::pubkey_hex\(\)\s*\)'
)


def _init_body(source: str) -> str:
    start = source.index("\nbool init() {")
    return source[start:source.index("\n}\n", start)]


def test_boot_line_the_copy_names_is_printed_by_the_firmware() -> None:
    if not FIRMWARE.is_dir():
        pytest.skip("firmware/ not present (HACS mirror checkout)")
    step = _load()["options"]["step"]
    for line, rel in BOOT_LINE_WITNESS.items():
        body = _init_body((FIRMWARE / rel).read_text(encoding="utf-8"))
        found = _BOOT_KEY_PRINT.findall(body)
        assert len(found) == 1, (
            f"{rel}: init() must print device_signature::pubkey_hex() on one "
            f"line of its own, or the copy's source for {line} is not there"
        )
        for name in ("init", "pin"):
            text = step[name]["description"]
            assert f"`{found[0]}`" in text and line in text, (
                f"options.step.{name} must name the `{found[0]}` boot line "
                f"that {rel} prints for {line}"
            )


_DEVICE_ID_PRINT = re.compile(r'boot_kv\(\s*"Device ID"\s*,')


def test_pin_step_names_the_device_id_line_the_firmware_prints() -> None:
    # The pin form takes a device_id as well as the key. For the products
    # whose key is a boot-log line, the copy names the boot-log line that
    # carries the device_id, and that line must exist.
    text = _load()["options"]["step"]["pin"]["description"]
    assert "`Device ID`" in text, (
        "the pin step names the boot key line for "
        f"{sorted(BOOT_LINE_MAIN)} but not where their device_id is read"
    )
    if not FIRMWARE.is_dir():
        pytest.skip("firmware/ not present (HACS mirror checkout)")
    for line, rel in BOOT_LINE_MAIN.items():
        source = (FIRMWARE / rel).read_text(encoding="utf-8")
        assert _DEVICE_ID_PRINT.search(source), (
            f"{rel} prints no `Device ID` boot line, which the pin step "
            f"names as the source of {line}'s device_id"
        )


# "a firmware release after 2.4.15": the last release without the line.
_RELEASE_FLOOR = re.compile(r"\bfirmware release after (\d+\.\d+\.\d+)\b")


def test_boot_line_copy_names_the_release_it_ships_after() -> None:
    # The boot key line is newer than the released images, so a sentence
    # offering it with no release qualifier tells every owner of a unit in
    # the field to look for a line their firmware never prints. Both steps
    # must name the same release.
    step = _load()["options"]["step"]
    floors = {}
    for name in ("init", "pin"):
        text = step[name]["description"]
        found = set(_RELEASE_FLOOR.findall(text))
        assert len(found) == 1, (
            f"options.step.{name} names the boot key line for "
            f"{sorted(BOOT_LINE_WITNESS)} without the release it first "
            "ships after (\"a firmware release after X.Y.Z\")"
        )
        floors[name] = found.pop()
    assert floors["init"] == floors["pin"], floors
