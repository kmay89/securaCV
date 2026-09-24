"""The options flow must say where each product's key comes from.

The pin / rotate / unpin menu used to tell every operator to read the
fingerprint and pubkey hex off "its /enroll page", but only canary-wap
serves that route (canary_wap.ino's register_api_routes). The
firmware/canary build and canary-vision print the key on USB serial; the
canary-sense and canary-sentinel show only a fingerprint; a canary-display
has no key at all. docs/device_trust.md ("Where each product shows its
key") holds the full table, read from source.

These tests pin the two properties the copy has to keep, not its wording:
/enroll is never offered as anyone's source but canary-wap's, and the menu
text accounts for every product line, so adding a product without saying
where its key is read fails here.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Iterator

PACKAGE_DIR = Path(__file__).resolve().parent.parent
STRINGS = PACKAGE_DIR / "strings.json"

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


def test_enroll_is_offered_only_as_canary_wap_source() -> None:
    offenders = [
        path
        for path, text in _strings(_load())
        if "/enroll" in text and "canary-wap" not in text
    ]
    assert not offenders, (
        f"{offenders} send operators to /enroll without saying only canary-wap "
        "serves it — the other products have no such route"
    )


def test_options_menu_names_every_product_line() -> None:
    text = _load()["options"]["step"]["init"]["description"]
    missing = [line for line in PRODUCT_LINES if line not in text]
    assert not missing, (
        f"the options menu does not say where {missing} show their key "
        "(or that they have none); see docs/device_trust.md"
    )


def test_pin_step_offers_no_source_for_a_fingerprint_only_product() -> None:
    # canary-sense and canary-sentinel show only a fingerprint, so the pin
    # step (which needs the full 64-hex key) must not list them as a source.
    text = _load()["options"]["step"]["pin"]["description"]
    assert "canary-wap" in text
    for line in ("canary-sense", "canary-sentinel", "canary-display"):
        assert line not in text, f"{line} has no full key to paste into the pin step"
