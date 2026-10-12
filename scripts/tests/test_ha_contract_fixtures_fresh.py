"""The Home Assistant seam vectors are generated, never hand-edited.

``tests/fixtures/ha_contract/*.json`` are read by the Rust bridge/API tests,
the integration's stub-lane tests, the add-on's shell tests and the wizard
tests — one file per contract, both ends. Their generator carries a
``--check`` form; this is the gate that runs it, so an edited vector (or an
edited generator with stale output) fails here before any reader does.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "tests" / "fixtures" / "ha_contract" / "gen_ha_contract_fixtures.py"
EXPECTED = {
    "announce.json",
    "claim.json",
    "camera_slug.json",
    "discovery.json",
    "pair.json",
    "event_state.json",
    "frigate_link.json",
    "identity.json",
}


def test_every_vector_is_present():
    present = {p.name for p in GENERATOR.parent.glob("*.json")}
    assert present == EXPECTED, present ^ EXPECTED


def test_the_vectors_are_what_the_generator_writes():
    proc = subprocess.run(
        [sys.executable, "-I", str(GENERATOR), "--check"],
        capture_output=True, text=True, timeout=120, check=False,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
