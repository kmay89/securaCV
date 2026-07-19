#!/usr/bin/env python3
"""Witness Dictionary drift gate (Flight Rule FR-13; docs/strategy/13 G1).

`spec/witness_dictionary.json` is the single source of truth for the semantic
vocabularies that are otherwise duplicated as constants across Rust, Python,
JavaScript, and firmware C++. This script parses the *real* source in each
language and fails if any copy has drifted from the dictionary.

Design: fail loud, fail safe. If a parser cannot locate the construct it is
looking for (an enum was renamed, a metadata block moved), that is itself a
hard error — a drift gate that silently finds nothing is worse than none. The
aviation lesson (FRED / ARINC 647A): the decode key is a maintained artifact
with the same discipline as the data.

Run: python3 scripts/lint_dictionary_sync.py   (exit 0 = in sync, 1 = drift)
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DICT_PATH = ROOT / "spec" / "witness_dictionary.json"

ERRORS: list[str] = []


def err(msg: str) -> None:
    ERRORS.append(msg)


def read(rel: str) -> str:
    p = ROOT / rel
    if not p.is_file():
        err(f"[missing file] {rel} — the linter expected it; did it move?")
        return ""
    return p.read_text(encoding="utf-8")


# --------------------------------------------------------------------------
# Minimal, defensive source parsers
# --------------------------------------------------------------------------

def _enum_body(text: str, name: str, rel: str) -> str | None:
    """Return the brace-balanced body of `enum {name} { ... }`, or None."""
    m = re.search(r"\benum\s+" + re.escape(name) + r"\s*\{", text)
    if not m:
        err(f"[parse] could not find `enum {name}` in {rel} — update the linter if it moved")
        return None
    i = m.end()
    depth = 1
    while i < len(text) and depth:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return text[m.end() : i - 1]


def rust_enum_variants(text: str, name: str, rel: str) -> list[str]:
    body = _enum_body(text, name, rel)
    if body is None:
        return []
    variants = []
    for line in body.splitlines():
        s = line.strip()
        if not s or s.startswith(("//", "#", "/*", "*")):
            continue
        m = re.match(r"^([A-Z][A-Za-z0-9_]*)\s*,?$", s)
        if m:
            variants.append(m.group(1))
    return variants


def rust_enum_serde_renames(text: str, name: str, rel: str) -> dict[str, str]:
    body = _enum_body(text, name, rel)
    if body is None:
        return {}
    renames: dict[str, str] = {}
    pending: str | None = None
    for line in body.splitlines():
        s = line.strip()
        rm = re.search(r'#\[serde\(rename\s*=\s*"([^"]+)"\)\]', s)
        if rm:
            pending = rm.group(1)
            continue
        vm = re.match(r"^([A-Z][A-Za-z0-9_]*)\s*,?$", s)
        if vm and pending is not None:
            renames[vm.group(1)] = pending
            pending = None
    return renames


def rust_match_pairs(text: str, pattern: str) -> dict[str, str]:
    return {m.group(1): m.group(2) for m in re.finditer(pattern, text)}


def brace_object_keys(text: str, opener_regex: str, rel: str, what: str) -> list[str]:
    """Keys of a `NAME = { ... }` object (Python dict or JS object literal)."""
    m = re.search(opener_regex, text)
    if not m:
        err(f"[parse] could not find {what} in {rel} — update the linter if it moved")
        return []
    i = m.end()
    depth = 1
    start = i
    while i < len(text) and depth:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    body = text[start : i - 1]
    keys = []
    for line in body.splitlines():
        km = re.match(r'^\s*"?([A-Za-z0-9_-]+)"?\s*:\s*\{', line)
        if km:
            keys.append(km.group(1))
    return keys


def compare(what: str, expected: list[str], actual: list[str]) -> None:
    exp, act = set(expected), set(actual)
    if exp == act:
        return
    missing = sorted(exp - act)
    extra = sorted(act - exp)
    detail = []
    if missing:
        detail.append(f"missing {missing}")
    if extra:
        detail.append(f"unexpected {extra}")
    err(f"[drift] {what}: " + "; ".join(detail))


# --------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------

def main() -> int:
    if not DICT_PATH.is_file():
        print(f"FATAL: {DICT_PATH} not found", file=sys.stderr)
        return 1
    d = json.loads(DICT_PATH.read_text(encoding="utf-8"))

    ev = d["event_types"]["items"]
    ev_ids = [e["id"] for e in ev]
    ev_variants = [e["rust_variant"] for e in ev]
    fail_variants = d["failure_types"]["rust_variants"]
    att = d["attestation_tiers"]["items"]
    claims = d["claim_kinds"]["items"]
    modalities = [m["id"] for m in d["modalities"]["items"]]
    sig = d["signature_format"]

    # --- Rust source of truth ---
    lib = read("src/lib.rs")
    contract = read("src/adapter/contract.rs")

    compare("EventType (src/lib.rs) vs dictionary",
            ev_variants, rust_enum_variants(lib, "EventType", "src/lib.rs"))
    compare("FailureType (src/lib.rs) vs dictionary",
            fail_variants, rust_enum_variants(lib, "FailureType", "src/lib.rs"))

    # Attestation: dictionary items that carry a Rust variant must match the
    # enum's serde renames exactly (the `device` tier is the absent case).
    rust_att = rust_enum_serde_renames(lib, "Attestation", "src/lib.rs")
    dict_att = {a["rust_variant"]: a["wire"] for a in att if a["rust_variant"]}
    if rust_att != dict_att:
        err(f"[drift] Attestation serde renames: dictionary {dict_att} vs src/lib.rs {rust_att}")

    # ClaimKind: variants, snake ids, and the (non-identity) claim->event map.
    ck_variants = rust_enum_variants(contract, "ClaimKind", "src/adapter/contract.rs")
    compare("ClaimKind variants vs dictionary",
            [c["rust_variant"] for c in claims], ck_variants)
    as_str = rust_match_pairs(contract, r'ClaimKind::(\w+)\s*=>\s*"([a-z_]+)"')
    to_event = rust_match_pairs(contract, r"ClaimKind::(\w+)\s*=>\s*EventType::(\w+)")
    ev_variant_by_id = {e["id"]: e["rust_variant"] for e in ev}
    for c in claims:
        rv = c["rust_variant"]
        if as_str.get(rv) != c["snake"]:
            err(f"[drift] ClaimKind::{rv}.as_str(): dictionary {c['snake']!r} vs code {as_str.get(rv)!r}")
        want_ev_variant = ev_variant_by_id.get(c["maps_to_event_type"])
        if to_event.get(rv) != want_ev_variant:
            err(f"[drift] claim_kind_to_event_type({rv}): dictionary -> {want_ev_variant} vs code -> {to_event.get(rv)}")

    # --- Home Assistant mirrors (const.py + JS card) must match the dictionary ---
    const_py = read("custom_components/securacv/const.py")
    card_js = read("custom_components/securacv/www/securacv-timeline-card.js")

    compare("const.py EVENT_TYPE_METADATA keys vs dictionary",
            ev_ids, brace_object_keys(const_py, r"EVENT_TYPE_METADATA\s*=\s*\{", "const.py", "EVENT_TYPE_METADATA"))
    compare("timeline-card.js EVENT_TYPE_METADATA keys vs dictionary",
            ev_ids, brace_object_keys(card_js, r"EVENT_TYPE_METADATA\s*=\s*\{", "timeline-card.js", "EVENT_TYPE_METADATA"))

    # Attestation wire strings present on the HA side (all three, incl. device).
    att_wires = [a["wire"] for a in att]
    py_att = re.findall(r'ATTESTATION_[A-Z_]+\s*=\s*"([^"]+)"', const_py)
    compare("const.py ATTESTATION_* vs dictionary", att_wires, py_att)
    compare("timeline-card.js ATTESTATION_METADATA keys vs dictionary",
            att_wires, brace_object_keys(card_js, r"ATTESTATION_METADATA\s*=\s*\{", "timeline-card.js", "ATTESTATION_METADATA"))

    # Modalities: JS keys are literal; const.py exposes them as MODALITY_* consts.
    compare("timeline-card.js MODALITY_METADATA keys vs dictionary",
            modalities, brace_object_keys(card_js, r"MODALITY_METADATA\s*=\s*\{", "timeline-card.js", "MODALITY_METADATA"))
    py_mod = set(re.findall(r'MODALITY_[A-Z_]+\s*=\s*"([^"]+)"', const_py))
    for mid in modalities:
        if mid not in py_mod:
            err(f"[drift] const.py is missing MODALITY constant for {mid!r} (dictionary modalities)")

    # --- Device-signature format constants (Python + every firmware copy) ---
    sig_py = read("custom_components/securacv/signature.py")
    _check_sig("custom_components/securacv/signature.py", sig_py, sig)
    for path in sorted(ROOT.glob("firmware/**/*")):
        if path.suffix not in (".h", ".cpp"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        # Only files that *define* SIG_PREFIX (constexpr/assignment), not test refs.
        if re.search(r'SIG_PREFIX\s*=\s*"', text):
            _check_sig(str(path.relative_to(ROOT)), text, sig)

    if ERRORS:
        print("Witness Dictionary drift detected "
              "(edit spec/witness_dictionary.json first, then every copy below):\n", file=sys.stderr)
        for e in ERRORS:
            print("  " + e, file=sys.stderr)
        print(f"\n{len(ERRORS)} problem(s). See docs/FLIGHT_RULES.md FR-13.", file=sys.stderr)
        return 1
    print("Witness Dictionary in sync across Rust / Python / JS / firmware.")
    return 0


def _check_sig(rel: str, text: str, sig: dict) -> None:
    pm = re.search(r'SIG_PREFIX\s*=\s*"([^"]+)"', text)
    sm = re.search(r"SCHEMA_V\s*=\s*(\d+)", text)
    if pm and pm.group(1) != sig["sig_prefix"]:
        err(f"[drift] {rel}: SIG_PREFIX {pm.group(1)!r} vs dictionary {sig['sig_prefix']!r}")
    if sm and int(sm.group(1)) != sig["schema_v"]:
        err(f"[drift] {rel}: SCHEMA_V {sm.group(1)} vs dictionary {sig['schema_v']}")
    if rel.endswith("signature.py"):
        am = re.search(r'ALG_NAME\s*=\s*"([^"]+)"', text)
        if am and am.group(1) != sig["alg_name"]:
            err(f"[drift] {rel}: ALG_NAME {am.group(1)!r} vs dictionary {sig['alg_name']!r}")


if __name__ == "__main__":
    sys.exit(main())
