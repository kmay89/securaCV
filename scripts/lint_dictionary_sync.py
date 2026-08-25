#!/usr/bin/env python3
"""Witness Dictionary drift gate (Flight Rule FR-13; docs/strategy/13 G1).

`spec/witness_dictionary.json` is the single source of truth for the semantic
vocabularies that are otherwise duplicated as constants across Rust, Python,
JavaScript, firmware C++, and Swift (the iPhone/watch apps). This script
parses the *real* source in each language and fails if any copy has drifted
from the dictionary.

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
        # Match the variant *name* at line start regardless of what follows —
        # a trailing `// comment`, a discriminant (`= 1`), or fields
        # (`(Type)` / `{ ... }`). The strict `\s*,?$` form silently skipped
        # such variants and reported false drift.
        m = re.match(r"^([A-Z][A-Za-z0-9_]*)\b", s)
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
        vm = re.match(r"^([A-Z][A-Za-z0-9_]*)\b", s)
        if vm and pending is not None:
            renames[vm.group(1)] = pending
            pending = None
    return renames


def rust_match_pairs(text: str, pattern: str) -> dict[str, str]:
    return {m.group(1): m.group(2) for m in re.finditer(pattern, text)}


def _balanced_body(text: str, start: int) -> str:
    """The brace-balanced block beginning at the first `{` at/after `start`."""
    open_at = text.find("{", start)
    if open_at < 0:
        return ""
    i = open_at + 1
    depth = 1
    while i < len(text) and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return text[open_at + 1 : i - 1]


def rust_fn_body(text: str, name: str, rel: str) -> str:
    """Body of `fn {name}(...)`, or "" if absent.

    Needed because two functions here (`as_str`, `hap_characteristic`) have
    identical arm *shapes* — `HomeSignal::X => "y",` — so a file-wide regex
    cannot tell which mapping it just read, and later arms silently overwrite
    earlier ones. Scoping to one body is what makes a swapped pair detectable.
    """
    m = re.search(r"\bfn\s+" + re.escape(name) + r"\s*\(", text)
    if not m:
        err(f"[parse] could not find `fn {name}` in {rel} — update the linter if it moved")
        return ""
    return _balanced_body(text, m.end())


def rust_event_signal_map(body: str) -> dict[str, list[str]]:
    """Parse `signals_for_event`'s match arms into {EventType: [signal ids]}.

    Arms may list several variants (`A | B => &[...]`), so the variant list is
    parsed rather than assumed to be one name. Signals are returned as the
    dictionary's snake_case ids, not the Rust variant names, so the comparison
    is against the vocabulary rather than against Rust spelling.
    """
    out: dict[str, list[str]] = {}
    # Written to be unambiguous rather than merely correct. The obvious
    # form — `(?:\s*EventType::\w+\s*\|?)+` — nests a quantifier over a
    # group whose leading and trailing `\s*` can split the same whitespace
    # many ways, which is polynomial backtracking waiting to happen. Here
    # every repetition must consume a literal `EventType::` and a literal
    # `|`, and `\w`/`\s` are disjoint, so there is exactly one way to match.
    for arm in re.finditer(
        r"((?:EventType::\w+\s*\|\s*)*EventType::\w+)\s*=>\s*&\[([^\]]*)\]", body
    ):
        variants = re.findall(r"EventType::(\w+)", arm.group(1))
        signals = [_signal_id(v) for v in re.findall(r"HomeSignal::(\w+)", arm.group(2))]
        for v in variants:
            out[v] = signals
    return out


def _signal_id(variant: str) -> str:
    """`MotionPerson` -> `motion_person`."""
    return re.sub(r"(?<!^)(?=[A-Z])", "_", variant).lower()


def py_event_signal_map(text: str) -> dict[str, list[str]]:
    """Parse `HOMEKIT_EVENT_SIGNALS` out of the Home Assistant const module."""
    m = re.search(r"HOMEKIT_EVENT_SIGNALS\s*=\s*\{(.*?)\n\}", text, re.S)
    if not m:
        err("[missing] HOMEKIT_EVENT_SIGNALS not found in "
            "custom_components/securacv/const.py")
        return {}
    out: dict[str, list[str]] = {}
    for entry in re.finditer(r'"(\w+)"\s*:\s*\(([^)]*)\)', m.group(1)):
        signals = re.findall(r'"([a-z0-9_]+)"', entry.group(2))
        out[entry.group(1)] = signals
    return out


def swift_computed_property_map(text: str, name: str, rel: str) -> dict[str, str]:
    """{caseName: returnedString} for a Swift `var {name}: String { switch … }`.

    Handles multi-case arms (`case .a, .b: return "x"`), which is how the
    class-scoped signals share one HAP characteristic.
    """
    m = re.search(r"\bvar\s+" + re.escape(name) + r"\s*:", text)
    if not m:
        err(f"[parse] could not find `var {name}` in {rel} — update the linter if it moved")
        return {}
    body = _balanced_body(text, m.end())
    out: dict[str, str] = {}
    for arm in re.finditer(r"case\s+([^:]+?):\s*return\s+\"([^\"]+)\"", body):
        for case_name in re.findall(r"\.(\w+)", arm.group(1)):
            out[case_name] = arm.group(2)
    return out


def snake_to_lower_camel(s: str) -> str:
    head, *rest = s.split("_")
    return head + "".join(p[:1].upper() + p[1:] for p in rest)


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


def swift_enum_raw_values(text: str, name: str, rel: str) -> list[str]:
    """Raw string values of a Swift `enum {name}: String { case x = "y" }`.

    Swift enums carry conformances between the name and the brace
    (`enum Foo: String, CaseIterable {`), so this uses its own opener regex
    rather than _enum_body's bare `enum Foo {` form.
    """
    m = re.search(r"\benum\s+" + re.escape(name) + r"\b[^{]*\{", text)
    if not m:
        err(f"[parse] could not find `enum {name}` in {rel} — update the linter if it moved")
        return []
    i = m.end()
    depth = 1
    while i < len(text) and depth:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    body = text[m.end() : i - 1]
    return re.findall(r'\bcase\s+\w+\s*=\s*"([a-z0-9_]+)"', body)


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

    # Attestation, two checks:
    #  (a) the full variant set must match — otherwise a NEW variant added
    #      without a serde rename would serialize as its PascalCase name,
    #      reach HA as an unrecognized value, and `normalize_attestation`
    #      would silently render it device-attested (a provenance downgrade
    #      FR-13 must catch), while (b) alone stays green.
    #  (b) each variant that carries a rename must map to the wire string the
    #      dictionary expects (the `device` tier is the absent case, no variant).
    dict_att_variants = [a["rust_variant"] for a in att if a["rust_variant"]]
    compare("Attestation variants (src/lib.rs) vs dictionary",
            dict_att_variants, rust_enum_variants(lib, "Attestation", "src/lib.rs"))
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

    # --- Offline viewer's scrub-timeline mirror (viewer/timeline_core.js) ---
    # Keys must match the dictionary ids; the labels are pinned to the
    # dictionary by viewer/timeline_core.test.js, which runs in viewer.yml.
    viewer_tl_js = read("viewer/timeline_core.js")
    compare("timeline_core.js EVENT_TYPE_META keys vs dictionary",
            ev_ids, brace_object_keys(viewer_tl_js, r"EVENT_TYPE_META\s*=\s*\{", "timeline_core.js", "EVENT_TYPE_META"))

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

    # --- Swift mirror (iPhone + iPad + watch apps) ---
    # ios/Shared/EventVocabulary.swift compiles into every Apple target; its
    # WitnessEvent enum must carry exactly the dictionary's event ids, and
    # each dictionary label must appear verbatim (the apps deliberately show
    # the same sentence of meaning as the Home Assistant card).
    swift = read("ios/Shared/EventVocabulary.swift")
    compare("EventVocabulary.swift WitnessEvent ids vs dictionary",
            ev_ids, swift_enum_raw_values(swift, "WitnessEvent", "ios/Shared/EventVocabulary.swift"))
    for e in ev:
        if f'"{e["label"]}"' not in swift:
            err(f"[drift] EventVocabulary.swift: label {e['label']!r} for {e['id']!r} "
                "missing or reworded (labels mirror the dictionary verbatim)")

    # --- Apple Home projection (egress vocabulary: Rust core + Swift mirror) ---
    # The projection is the one vocabulary that leaves the household, so its
    # drift check is the strictest here: not just the names, but the HAP
    # characteristic each name projects as. A silent rename on either side
    # would publish a Canary into someone's home as the wrong kind of sensor.
    hk = d["homekit_projection"]
    hk_signals = hk["signals"]
    hk_ids = [s["id"] for s in hk_signals]

    rust_hk = read("src/bridge/homekit.rs")
    compare("HomeSignal variants (src/bridge/homekit.rs) vs dictionary",
            [s["rust_variant"] for s in hk_signals],
            rust_enum_variants(rust_hk, "HomeSignal", "src/bridge/homekit.rs"))

    # Both mappings are compared *per variant*, from their own function bodies.
    # A presence-only check would pass when two signals swap characteristics —
    # publishing a Canary into someone's home as the wrong kind of sensor while
    # CI stayed green.
    rust_ids = rust_match_pairs(rust_fn_body(rust_hk, "as_str", "src/bridge/homekit.rs"),
                                r'HomeSignal::(\w+) => "([a-z0-9_]+)"')
    rust_haps = rust_match_pairs(
        rust_fn_body(rust_hk, "hap_characteristic", "src/bridge/homekit.rs"),
        r'HomeSignal::(\w+) => "([a-z0-9-]+)"')
    # Matter device types (bridge site D, data-first): explicit Some(...) arms
    # only — a signal the dictionary maps to null must NOT appear in the match
    # body, so it falls to the `_ => None` arm.
    rust_matter = rust_match_pairs(
        rust_fn_body(rust_hk, "matter_device_type", "src/bridge/homekit.rs"),
        r'HomeSignal::(\w+) => Some\("([a-z0-9-]+)"\)')
    swift_hk = read("ios/Sources/SecuraCV/Native/HomeKitBridge.swift")
    swift_haps = swift_computed_property_map(
        swift_hk, "hapCharacteristic", "ios/Sources/SecuraCV/Native/HomeKitBridge.swift")

    for s in hk_signals:
        rv, sid, hap = s["rust_variant"], s["id"], s["hap_characteristic"]
        if rust_ids.get(rv) != sid:
            err(f"[drift] HomeSignal::{rv}.as_str(): dictionary {sid!r} "
                f"vs code {rust_ids.get(rv)!r}")
        if rust_haps.get(rv) != hap:
            err(f"[drift] HomeSignal::{rv}.hap_characteristic(): dictionary "
                f"{hap!r} vs code {rust_haps.get(rv)!r}")
        if rust_matter.get(rv) != s.get("matter_device_type"):
            err(f"[drift] HomeSignal::{rv}.matter_device_type(): dictionary "
                f"{s.get('matter_device_type')!r} vs code {rust_matter.get(rv)!r}")
        swift_case = snake_to_lower_camel(sid)
        if swift_haps.get(swift_case) != hap:
            err(f"[drift] HomeKitBridge.swift .{swift_case}.hapCharacteristic: "
                f"dictionary {hap!r} vs code {swift_haps.get(swift_case)!r}")
        if f'"{s["label"]}"' not in swift_hk:
            err(f"[drift] HomeKitBridge.swift: label {s['label']!r} for "
                f"{sid!r} missing or reworded (labels mirror the dictionary)")

    compare("HomeKitBridge.swift HomeSignal ids vs dictionary",
            hk_ids, swift_enum_raw_values(swift_hk, "HomeSignal",
                                          "ios/Sources/SecuraCV/Native/HomeKitBridge.swift"))

    # --- Which signals an event asserts (Rust core + Python HA mirror) ---
    # The egress half of the event vocabulary. Checked per event, in both
    # directions: a missing entry and a wrong entry are different bugs, and
    # both publish a Canary into someone's home as the wrong kind of sensor.
    hk_events = hk["event_signals"]["map"]

    rust_events = rust_event_signal_map(
        rust_fn_body(rust_hk, "signals_for_event", "src/bridge/homekit.rs"))
    py_const = read("custom_components/securacv/const.py")
    py_events = py_event_signal_map(py_const)

    compare("signals_for_event (src/bridge/homekit.rs) events vs dictionary",
            sorted(hk_events), sorted(rust_events))
    compare("HOMEKIT_EVENT_SIGNALS (custom_components/securacv/const.py) "
            "events vs dictionary", sorted(hk_events), sorted(py_events))

    for event, signals in hk_events.items():
        expected = list(signals)
        if rust_events.get(event) != expected:
            err(f"[drift] signals_for_event({event}): dictionary {expected} "
                f"vs Rust {rust_events.get(event)}")
        if py_events.get(event) != expected:
            err(f"[drift] HOMEKIT_EVENT_SIGNALS[{event!r}]: dictionary "
                f"{expected} vs Python {py_events.get(event)}")
        # Every signal named here must be a real signal, or the mapping
        # invents vocabulary the projection cannot publish.
        for named in expected:
            if named not in hk_ids:
                err(f"[drift] event_signals[{event!r}] names {named!r}, which "
                    f"is not a HomeSignal id")

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
    print("Witness Dictionary in sync across Rust / Python / JS / firmware / Swift.")
    return 0


def _check_sig(rel: str, text: str, sig: dict) -> None:
    pm = re.search(r'SIG_PREFIX\s*=\s*"([^"]+)"', text)
    sm = re.search(r"SCHEMA_V\s*=\s*(\d+)", text)
    if pm and pm.group(1) != sig["sig_prefix"]:
        err(f"[drift] {rel}: SIG_PREFIX {pm.group(1)!r} vs dictionary {sig['sig_prefix']!r}")
    if sm and int(sm.group(1)) != sig["schema_v"]:
        err(f"[drift] {rel}: SCHEMA_V {sm.group(1)} vs dictionary {sig['schema_v']}")
    # ALG_NAME wherever it is defined (signature.py AND every firmware copy that
    # declares it): firmware emits `alg` in signature metadata and the HA
    # verifier rejects any value that differs from the dictionary's alg_name.
    am = re.search(r'ALG_NAME\s*=\s*"([^"]+)"', text)
    if am and am.group(1) != sig["alg_name"]:
        err(f"[drift] {rel}: ALG_NAME {am.group(1)!r} vs dictionary {sig['alg_name']!r}")


if __name__ == "__main__":
    sys.exit(main())
