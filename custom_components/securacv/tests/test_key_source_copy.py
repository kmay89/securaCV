"""The options flow must say where each product's key comes from.

The pin / rotate / unpin menu used to tell every operator to read the
fingerprint and pubkey hex off "its /enroll page", but only canary-wap
serves that route (canary_wap.ino's register_api_routes). The
firmware/canary build and canary-vision print the key on USB serial; the
canary-sense and canary-sentinel show only a fingerprint; a canary-display
has no key at all. docs/device_trust.md ("Where each product shows its
key") holds the full table, read from source.

These tests pin properties the copy has to keep, not its wording:

- every clause that offers /enroll names canary-wap and no other product,
  and no "any" / "each device" style word that would widen it again;
- the menu text names each product line in PRODUCT_LINES, and in the
  monorepo PRODUCT_LINES is held to firmware/flavors.json, so a flavor added
  there without saying here where its key is read fails (the HACS mirror has
  no firmware/ and skips that one cross-check);
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
# The monorepo's flavor registry. Absent in the HACS mirror checkout.
FLAVORS = Path(__file__).resolve().parents[3] / "firmware" / "flavors.json"

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


def test_pin_step_offers_no_source_for_a_fingerprint_only_product() -> None:
    # canary-sense and canary-sentinel show only a fingerprint, so the pin
    # step (which needs the full 64-hex key) must not list them as a source.
    text = _load()["options"]["step"]["pin"]["description"]
    assert "canary-wap" in text
    for line in ("canary-sense", "canary-sentinel", "canary-display"):
        assert line not in text, f"{line} has no full key to paste into the pin step"
