"""The dashboard strategy's chain-status header, rendered by the core's own
template engine against the chain_valid entity's real attribute vocabulary.

The strategy (custom_components/securacv/www/securacv-strategy.js,
`chainHeaderMarkdown`) emits a Jinja template for a markdown card; this
lifts that template from the source and renders it for each state the
sensor can be in — above all a key change, which must read "Witness key
changed — confirm it in Settings → Repairs" and never "Verification failed"
(D10)."""
from __future__ import annotations

import json
import re
from typing import Any

import pytest
from homeassistant.core import HomeAssistant
from homeassistant.helpers.template import Template

from conftest import REPO_ROOT

STRATEGY = REPO_ROOT / "custom_components" / "securacv" / "www" / "securacv-strategy.js"
CV = "binary_sensor.securacv_chain_valid"
CL = "sensor.securacv_chain_length"


def header_template() -> str:
    """The lines `chainHeaderMarkdown` joins for a connected kernel, with the
    two entity ids substituted the way `jinjaStr` would."""
    source = STRATEGY.read_text(encoding="utf-8")
    start = source.index("function chainHeaderMarkdown(")
    body = source[start: source.index("\n  }\n", start)]
    array = body[body.index("return [") + len("return ["): body.index("].join(")]
    lines: list[str] = []
    for raw in re.findall(r'^\s*(?:`([^`]*)`|"((?:[^"\\]|\\.)*)"),\s*$', array, re.M):
        template, plain = raw
        # A double-quoted JS literal with the escapes JSON shares (\" \\ \n).
        text = template if template else json.loads(f'"{plain}"')
        lines.append(text)
    assert lines, "the header template lines were not found in the strategy source"
    head = lines[0]
    assert "${jinjaStr(cv)}" in head and "${jinjaStr(cl || \"\")}" in head
    lines[0] = head.replace("${jinjaStr(cv)}", f"'{CV}'").replace("${jinjaStr(cl || \"\")}", f"'{CL}'")
    return "\n".join(lines)


@pytest.mark.parametrize(
    "state,attributes,expected",
    [
        (
            "unknown",
            {"key_mismatch": True, "trust_reason": "kernel_key_mismatch",
             "error": "the witness key changed; confirm it in Settings → Repairs",
             "verified_bucket_start": 1700001200, "verified_entries": 1204, "verifying": False},
            "### ⚠ Witness key changed — confirm it in Settings → Repairs",
        ),
        (
            # Belt and braces: the attribute alone, without the reason or the error.
            "unknown",
            {"key_mismatch": True, "verifying": False},
            "### ⚠ Witness key changed — confirm it in Settings → Repairs",
        ),
        (
            "off",
            {"key_mismatch": False, "trust_reason": "ha_verify_failed", "failed_seq": 1190,
             "error": "sealed_events hash chain broken at index 7", "verifying": False},
            "### ⚠ Verification failed at entry #1,190",
        ),
        (
            "on",
            {"key_mismatch": False, "trust_reason": "ha_verified_chain", "verified_bucket_start": 1700001200,
             "verified_entries": 1204, "verifying": False},
            "### ✅ Chain verified ✓ ",
        ),
        ("unknown", {"key_mismatch": False, "verifying": True}, "### Verifying…"),
        ("unknown", {"key_mismatch": False, "verifying": False}, "### Not verified yet"),
    ],
    ids=["key-changed", "key-changed-attr-only", "failed", "verified", "verifying", "not-yet"],
)
async def test_the_header_reads_a_key_change_as_repairs_never_as_failed(
    hass: HomeAssistant, state: str, attributes: dict[str, Any], expected: str
) -> None:
    hass.states.async_set(CV, state, attributes)
    hass.states.async_set(CL, "1204")
    await hass.async_block_till_done()
    rendered = Template(header_template(), hass).async_render()
    assert rendered.startswith(expected), rendered
    if attributes.get("key_mismatch"):
        assert "Verification failed" not in rendered
    if expected.startswith("### ✅"):
        assert "1,204 entries · key pinned" in rendered
