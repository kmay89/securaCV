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

    # --- The HA event platform derives its event_types, never retypes them ---
    # custom_components/securacv/event.py builds `event_types` from
    # const.WITNESS_EVENT_TYPES (tuple(EVENT_TYPE_METADATA), checked above).
    # A literal id in that module would be a second copy of the vocabulary
    # with no linter on it, so the module may not spell any event type.
    event_rel = "custom_components/securacv/event.py"
    event_py = read(event_rel)
    if "WITNESS_EVENT_TYPES" not in event_py:
        err(f"[drift] {event_rel} does not read const.WITNESS_EVENT_TYPES for its event_types")
    for ev_id in ev_ids:
        if re.search(r"""(?<![A-Za-z0-9_])["']""" + re.escape(ev_id) + r"""["']""", event_py):
            err(f"[literal] {event_rel} spells event type {ev_id!r} itself; derive it from "
                "const.WITNESS_EVENT_TYPES instead")
    for ev_variant in ev_variants:
        if re.search(r"""["']""" + re.escape(ev_variant) + r"""["']""", event_py):
            err(f"[literal] {event_rel} spells the Rust variant {ev_variant!r}; the platform "
                "folds wire spellings through const.normalize_event_type")

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
    # The tile name Apple's Home app shows is NOT the dictionary label — it is
    # the per-service Name characteristic in src/bridge/hap/accessory.rs. The
    # generated quickstart named the label ("Motion (person)") for a tile the
    # bridge publishes as "Person", so a user was told to look for a name that
    # is not on their screen. Pinned here so the doc cannot say it again.
    hap_accessory = read("src/bridge/hap/accessory.rs")
    rust_tile_names = rust_match_pairs(
        rust_fn_body(hap_accessory, "service_name", "src/bridge/hap/accessory.rs"),
        r'HomeSignal::(\w+) => "([^"]+)"')

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
        if rust_tile_names.get(rv) != s.get("home_app_name"):
            err(f"[drift] service_name(HomeSignal::{rv}) (src/bridge/hap/"
                f"accessory.rs): dictionary home_app_name "
                f"{s.get('home_app_name')!r} vs code {rust_tile_names.get(rv)!r} "
                f"— this is the name on the tile in Apple's Home app, and "
                f"docs/integrations/apple-home-quickstart.md is generated from it")

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
    kinds_found: dict[str, set] = {"custom_components/securacv/signature.py":
                                   _canonical_kinds(sig_py)}
    for path in sorted(ROOT.glob("firmware/**/*")):
        if path.suffix not in (".h", ".cpp"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        # Only files that *define* SIG_PREFIX (constexpr/assignment), not test refs.
        if _SIG_PREFIX_DEF_RE.search(text):
            _check_sig(str(path.relative_to(ROOT)), text, sig)
    # The builders themselves live next to the headers that declare them, so
    # scan the whole firmware tree plus the desktop and kernel verifiers for
    # canonical format strings — not just the SIG_PREFIX definers above. A
    # verifier outside firmware/ that declares its own SIG_PREFIX / SCHEMA_V
    # (the kernel's fleet roll-call) is pinned to the dictionary here too.
    for rel in _SIG_KIND_SOURCES:
        p = ROOT / rel
        if not p.exists():
            err(f"[drift] signature_format: {rel} is gone — canonical_kinds "
                f"can no longer be proved against it; update _SIG_KIND_SOURCES")
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        kinds_found[rel] = _canonical_kinds(text)
        if not rel.startswith("firmware/") and _SIG_PREFIX_DEF_RE.search(text):
            _check_sig(rel, text, sig)
    _check_canonical_kinds(sig, kinds_found)

    # --- The normative prose must name the same nine kinds as the dictionary ---
    _check_event_contract(ev_ids)

    # --- system.integrity tamper kinds (firmware module + HA + narrations) ---
    _check_integrity_kinds(d["known_divergences"]["tamper_vocabularies"], const_py)

    # --- Household posture + the hub attention engine (Rust core + vectors) ---
    _check_attention(d)

    if ERRORS:
        print("Witness Dictionary drift detected "
              "(edit spec/witness_dictionary.json first, then every copy below):\n", file=sys.stderr)
        for e in ERRORS:
            print("  " + e, file=sys.stderr)
        print(f"\n{len(ERRORS)} problem(s). See docs/FLIGHT_RULES.md FR-13.", file=sys.stderr)
        return 1
    print("Witness Dictionary in sync across Rust / Python / JS / firmware / Swift.")
    return 0


def _check_event_contract(ev_ids: list) -> None:
    """Pin spec/event_contract.md's two claim lists to the dictionary.

    The dictionary's own `governance` field says event_contract.md is the
    normative prose and this file is its machine-readable projection — "where
    the two disagree, event_contract.md wins and this file is the bug". Nothing
    checked that, so §5's permitted-claims list quietly grew a tenth entry,
    `forced_entry_detected`, that existed in no vocabulary and no code; an
    adapter author following §5 (which spec/sensor_adapter_contract_v0.md sends
    them to) emitted a claim the kernel refuses. Both the §5 bullet list and
    the §11 table are now proved against the dictionary's event_types ids.
    """
    rel = "spec/event_contract.md"
    text = read(rel)
    if not text:
        return
    expected = sorted(ev_ids)

    # §5 — bullets between the two subheadings. Strict `- \`id\`` bullets only,
    # so prose that mentions an id in passing is not mistaken for the list.
    m = re.search(r"### Permitted Claims \(Good\)(.*?)### Forbidden Claims",
                  text, re.S)
    if not m:
        err(f"[parse] {rel}: could not find §5's 'Permitted Claims (Good)' list "
            f"— update the linter if the section moved")
    else:
        s5 = re.findall(r"^- `([a-z][a-z0-9_]*)`\s*$", m.group(1), re.M)
        if sorted(s5) != expected:
            err(f"[drift] {rel} §5 Permitted Claims {sorted(s5)} != dictionary "
                f"event_types {expected} — §5 is the vocabulary adapter authors "
                f"are sent to; a claim listed there that the kernel refuses is a "
                f"trap, and one the kernel accepts but §5 omits is invisible")

    # §11 — the `| \`event_type\` | description |` table.
    m = re.search(r"## 11\. Sensor Adapter Event Vocabulary(.*?)^## ",
                  text + "\n## ", re.S | re.M)
    if not m:
        err(f"[parse] {rel}: could not find §11's 'Sensor Adapter Event "
            f"Vocabulary' table — update the linter if the section moved")
    else:
        s11 = re.findall(r"^\| `([a-z][a-z0-9_]*)` \|", m.group(1), re.M)
        if sorted(s11) != expected:
            err(f"[drift] {rel} §11 table {sorted(s11)} != dictionary "
                f"event_types {expected}")


# The system.integrity csi_event module is the one firmware producer of the
# tamper-topic `type` words: canary-wap's csi_mqtt bridges every commit onto
# securacv/<id>/tamper as {"type": <kind>}, and the canary PIO tree registers
# the same module. Its kinds are HA's per-type sensor keys verbatim, so a kind
# minted here without the dictionary (or HA) is a tamper nobody's sensor
# lights for — and one the dictionary lists that the module cannot emit is an
# advertised protection that never fires (const.py's own doctrine).
_INTEGRITY_CPP = "firmware/common/csi/src/tamper_events_module.cpp"
_INTEGRITY_H = "firmware/common/csi/src/tamper_events_module.h"
# String literals in the module .cpp that are not kinds: the csi event
# type_name. (The module id "system.integrity" has a dot, so the kind
# pattern below never matches it; it is required as the anchor instead.)
_INTEGRITY_NON_KIND_LITERALS = frozenset({"tamper"})
_INTEGRITY_KIND_RE = re.compile(r"[a-z][a-z0-9_]*")
# The header's doctrine table: ` *   sd_remove         — the card left ...`
# (three spaces after the star; prose lines in the same comment have one).
_INTEGRITY_DOCTRINE_RE = re.compile(r"^ \*   ([a-z][a-z0-9_]*)\s+—\s", re.M)
# Where each host feeds the module a live state. A host whose dictionary
# list names a kind that needs a feed (an SD kind, the enclosure contact)
# must still be passing it — in code, not in a comment — or the list is a
# promise its firmware no longer keeps.
_INTEGRITY_FEEDS = {
    "sd": (frozenset({"sd_error", "sd_remove"}), {
        "canary-wap": ("firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino",
                       "(uint8_t)g_hw.sd_state"),
        "canary": ("firmware/canary/src/main.cpp", "storage_sd_state()"),
    }),
    "enclosure contact": (frozenset({"enclosure"}), {
        "canary-wap": ("firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino",
                       "tamper_events_watch_contact("),
        "canary": ("firmware/canary/src/main.cpp",
                   "securacv_csi_modules_tamper_watch_contact("),
    }),
}


def _c_code(text: str) -> str:
    """C/C++ source with every comment blanked, string literals kept intact.

    A token inside a comment is prose, not code — the module's comments quote
    kind words while explaining them, and main.cpp's comment names the very
    feed call it describes — so the walk skips comments while honoring string
    and char literals (a `//` inside a string is not a comment).
    """
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            out.append(" ")
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif text[i] in "\"'":
            quote, j = text[i], i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i : j + 1])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def _c_string_literals(text: str) -> list[str]:
    """Double-quoted string literals of a C/C++ source, comments skipped."""
    return re.findall(r'"((?:[^"\\\n]|\\.)*)"', _c_code(text))


def _check_integrity_kinds(tv: dict, const_py: str) -> None:
    """Pin tamper_vocabularies.system_integrity_kinds to every copy.

    Four sides, all required to hold: (1) the module's kind literals and its
    header's doctrine table equal the dictionary list; (2) every kind is a
    const.py TAMPER_* value with its own per-type binary sensor and is not a
    fenced FUTURE_TAMPER_TYPES entry; (3) every narration surface knows every
    kind (superset — a table may narrate kinds this module never emits);
    (4) the per-host lists add nothing the dictionary lacks, cover it, and
    each host that claims a fed kind (SD, enclosure) still passes that feed.
    """
    declared = tv.get("system_integrity_kinds")
    if not declared:
        err("[drift] known_divergences.tamper_vocabularies.system_integrity_kinds "
            "is empty or missing")
        return
    want = set(declared)

    # (1) The producer and its doctrine.
    cpp = read(_INTEGRITY_CPP)
    if cpp:
        literals = _c_string_literals(cpp)
        if "system.integrity" not in literals or "tamper" not in literals:
            err(f"[parse] {_INTEGRITY_CPP}: no \"system.integrity\" / \"tamper\" "
                f"literal — the module moved or was renamed; update the linter")
        else:
            kinds = {s for s in literals if _INTEGRITY_KIND_RE.fullmatch(s)}
            kinds -= _INTEGRITY_NON_KIND_LITERALS
            compare(f"{_INTEGRITY_CPP} kind literals vs dictionary "
                    f"system_integrity_kinds", sorted(want), sorted(kinds))
    hdr = read(_INTEGRITY_H)
    if hdr:
        table = _INTEGRITY_DOCTRINE_RE.findall(hdr)
        if not table:
            err(f"[parse] {_INTEGRITY_H}: could not find the doctrine table "
                f"(` *   <kind> — <meaning>` lines) — update the linter if it moved")
        else:
            compare(f"{_INTEGRITY_H} doctrine table vs dictionary "
                    f"system_integrity_kinds", sorted(want), table)

    # (2) Home Assistant: a real TAMPER_* word, a real sensor, not fenced.
    consts = dict(re.findall(r'^(TAMPER_[A-Z_]+)\s*=\s*"([^"]+)"', const_py, re.M))
    if not consts:
        err("[parse] const.py: no TAMPER_* constants — update the linter if they moved")
        return
    fm = re.search(r"FUTURE_TAMPER_TYPES\s*=\s*\[(.*?)\]", const_py, re.S)
    if not fm:
        err("[parse] const.py: FUTURE_TAMPER_TYPES not found — update the linter if it moved")
        future = set()
    else:
        future = {consts.get(name, name) for name in re.findall(r"\b(TAMPER_[A-Z_]+)\b", fm.group(1))}
    bs_rel = "custom_components/securacv/binary_sensor.py"
    bs_src = read(bs_rel)
    sensor_names = re.findall(
        r"SecuraCVCanaryTamperTypeSensor\(\s*prefix,\s*device_id,\s*entry,\s*(TAMPER_[A-Z_]+)",
        bs_src)
    # The sensors may instead be built by one generator over a const.py list
    # (`SecuraCVCanaryTamperTypeSensor(..., t, ...) for t in ALL_TAMPER_TYPES`):
    # then that list's TAMPER_* names are the constructions.
    for var, loop_var, lst in re.findall(
            r"SecuraCVCanaryTamperTypeSensor\(\s*prefix,\s*device_id,\s*entry,\s*(\w+)"
            r"[^()]*(?:\([^()]*\)[^()]*)*\)\s*for\s+(\w+)\s+in\s+([A-Z_]+)\b", bs_src):
        if var != loop_var:
            continue
        lm = re.search(r"^" + lst + r"\s*=\s*\[(.*?)\]", const_py, re.S | re.M)
        if not lm:
            err(f"[parse] {bs_rel}: the tamper sensors iterate {lst}, which const.py "
                f"does not define as a list — update the linter if it moved")
            continue
        sensor_names += re.findall(r"\b(TAMPER_[A-Z_]+)\b", lm.group(1))
    if not sensor_names:
        err(f"[parse] {bs_rel}: no SecuraCVCanaryTamperTypeSensor(...) constructions "
            f"— update the linter if the per-type sensor list moved")
    sensors = {consts.get(name) for name in sensor_names}
    for kind in sorted(want):
        if kind not in consts.values():
            err(f"[drift] system_integrity_kinds {kind!r} is not a const.py TAMPER_* "
                f"value — HA's per-type sensors key on those exact strings")
        elif kind in future:
            err(f"[drift] system_integrity_kinds {kind!r} is fenced in const.py "
                f"FUTURE_TAMPER_TYPES — move it out when a firmware emits it")
        if sensor_names and kind not in sensors:
            err(f"[drift] system_integrity_kinds {kind!r} has no per-type "
                f"SecuraCVCanaryTamperTypeSensor in {bs_rel}")

    # (3) Every surface that narrates a kind in words.
    card_rel = "custom_components/securacv/www/securacv-timeline-card.js"
    narrations: dict[str, set] = {
        card_rel: set(brace_object_keys(read(card_rel), r"TAMPER_KIND_METADATA\s*=\s*\{",
                                        card_rel, "TAMPER_KIND_METADATA")),
    }
    voice_rel = "custom_components/securacv/voice.py"
    vm = re.search(r"_ALERT_EVENT_TYPES\s*=\s*frozenset\((.*?)\)", read(voice_rel), re.S)
    if not vm:
        err(f"[parse] {voice_rel}: _ALERT_EVENT_TYPES not found — update the linter if it moved")
    else:
        narrations[voice_rel] = set(re.findall(r'"([a-z0-9_]+)"', vm.group(1)))
    swift_rel = "ios/Shared/EventVocabulary.swift"
    narrations[swift_rel] = set(swift_enum_raw_values(read(swift_rel), "TamperKind", swift_rel))
    dash_rel = "firmware/projects/canary-wap/arduino/canary_wap/csi_dashboard_html.h"
    dash = read(dash_rel)
    narrations[dash_rel] = {k for k in want if re.search(r"\b" + k + r"\s*:\s*'", dash)}
    for rel, known in narrations.items():
        missing = want - known
        if missing:
            err(f"[drift] {rel} does not narrate system_integrity_kinds "
                f"{sorted(missing)} — every surface that names a tamper kind in "
                f"words must know every kind the module can emit")

    # (3b) The older `kind` words that also carry an HA `type`: the type
    # must be a real per-type sensor key, and the canary drain that
    # publishes the kind must still spell the type in code.
    kind_types = tv.get("firmware_kind_types", {})
    drain_rel = "firmware/canary/src/main.cpp"
    drain_literals = "\n".join(_c_string_literals(read(drain_rel)))
    for kind, typ in sorted(kind_types.items()):
        if kind not in tv.get("firmware_kinds", []):
            err(f"[drift] firmware_kind_types maps {kind!r}, which firmware_kinds does not list")
        if typ not in consts.values() or typ in future:
            err(f"[drift] firmware_kind_types {kind!r} -> {typ!r}: not a live const.py "
                f"TAMPER_* value")
        elif sensor_names and typ not in sensors:
            err(f"[drift] firmware_kind_types {kind!r} -> {typ!r} has no per-type "
                f"SecuraCVCanaryTamperTypeSensor in {bs_rel}")
        if kind not in drain_literals or f'\\"type\\":\\"{typ}\\"' not in drain_literals:
            err(f"[drift] {drain_rel}: the tamper drain no longer publishes {kind!r} "
                f"with \"type\":\"{typ}\" — HA's per-type sensor matches `type`, "
                f"never `kind`")

    # (4) Hosts.
    hosts = {h: ks for h, ks in tv.get("system_integrity_hosts", {}).items()
             if not h.startswith("$")}
    if not hosts:
        err("[drift] known_divergences.tamper_vocabularies.system_integrity_hosts "
            "is empty or missing")
        return
    union: set = set()
    for host, ks in sorted(hosts.items()):
        union |= set(ks)
        extra = set(ks) - want
        if extra:
            err(f"[drift] system_integrity_hosts[{host!r}] lists {sorted(extra)}, "
                f"which system_integrity_kinds does not")
        for what, (needs, feeds) in _INTEGRITY_FEEDS.items():
            if not set(ks) & needs:
                continue
            feed = feeds.get(host)
            if feed is None:
                err(f"[drift] system_integrity_hosts[{host!r}] lists {sorted(set(ks) & needs)} "
                    f"but the linter knows no {what} feed for that host — add it to "
                    f"_INTEGRITY_FEEDS")
            elif feed[1] not in _c_code(read(feed[0])):
                err(f"[drift] system_integrity_hosts[{host!r}] lists {sorted(set(ks) & needs)} "
                    f"but {feed[0]} no longer feeds the watcher a live {what} state "
                    f"({feed[1]!r} not found in code)")
    if union != want:
        err(f"[drift] system_integrity_hosts cover {sorted(union)}, not "
            f"system_integrity_kinds {sorted(want)} — a kind no host can emit "
            f"is a sensor that never fires")


# --------------------------------------------------------------------------
# Household posture and the hub attention engine
# --------------------------------------------------------------------------
#
# The attention engine (src/relay/attention.rs) decides how loudly the hub
# notifies, so every vocabulary it speaks is pinned here: a silent rename of a
# tier, a class quietly moved off critical, a phrase that grows a second blank
# or an identity word, a channel regrouped so two radios on one phone count as
# independent corroboration. The engine mirrors the dictionary through one
# method per fact (`id`, `floor_tier`, `family`, `text`, ...), each a plain
# `match` the parsers below read from its own impl body — the same scoping
# reason as rust_fn_body: several impls here share method names.

_ATT_RS = "src/relay/attention.rs"
_HK_POSTURE_RS = "household/src/posture.rs"
_ATT_FIXTURE = "spec/fixtures/attention_v1.json"
_SENTINEL_H = "firmware/common/fusion/sentinel_fusion.h"
_SENTINEL_CPP = "firmware/common/fusion/sentinel_fusion.cpp"
_NOTIFY_H = "firmware/projects/canary-wap/arduino/canary_wap/notify.h"
_REPEAT_SWIFT = "ios/Sources/SecuraCV/Alerts/RepeatGovernor.swift"
_RELAY_RS = "src/relay/mod.rs"

# Words no phrase or reason text may say (spec household_keys_v0 §7.2, "No
# identity, ever"; Free Signals Invariant F and §5): no who, no
# known-vs-unknown, no authorized-vs-not, and the novelty hint never reaches
# wording.
_ATTENTION_FORBIDDEN_WORDS = frozenset({
    "name", "names", "known", "unknown", "stranger", "strangers", "intruder",
    "intruders", "recognized", "verified", "familiar", "unfamiliar",
    "authorized", "unauthorized", "household",
})


def rust_impl_body(text: str, type_name: str, rel: str) -> str:
    """Body of `impl {type_name} { ... }` (not a trait impl), or ""."""
    m = re.search(r"\bimpl\s+" + re.escape(type_name) + r"\s*\{", text)
    if not m:
        err(f"[parse] could not find `impl {type_name}` in {rel} — update the linter if it moved")
        return ""
    return _balanced_body(text, m.end() - 1)


def rust_impl_fn_body(text: str, type_name: str, fn: str, rel: str) -> str:
    """Body of `fn {fn}` inside `impl {type_name}`, or ""."""
    impl = rust_impl_body(text, type_name, rel)
    if not impl:
        return ""
    return rust_fn_body(impl, fn, f"{rel} impl {type_name}")


def rust_variant_arms(body: str, enum: str, rhs: str) -> dict[str, str]:
    """{variant: rhs-group} for `E::A | E::B => rhs` arms, braces allowed.

    `rhs` is a regex with exactly one group. Unambiguous for the same reason
    as rust_event_signal_map: every repetition consumes a literal `E::` and a
    literal `|`.
    """
    out: dict[str, str] = {}
    e = re.escape(enum)
    pat = (r"((?:" + e + r"::\w+\s*\|\s*)*" + e + r"::\w+)\s*=>\s*\{?\s*" + rhs)
    for m in re.finditer(pat, body):
        for var in re.findall(e + r"::(\w+)", m.group(1)):
            out[var] = m.group(2)
    return out


_RUST_PUNCT = ("..=", "=>", "::", "||", "&&", "==", "!=", "<=", ">=", "->", "..")
_RUST_SIMPLE_ESCAPES = {"n": "\n", "r": "\r", "t": "\t", "\\": "\\", "0": "\0", "'": "'", '"': '"'}


def _rust_tokens(text: str, where: str) -> list[tuple[str, str, int]] | None:
    """Rust source as (kind, value, offset) tokens, comments dropped.

    Kinds: `str` (value: the literal's DECODED contents, escapes and raw
    strings included, so `"a\\"b"` is the word a"b), `bytes` (a b"" / c""
    literal), `char`, `lifetime`, `ident`, `num`, `punct`. Small on purpose —
    just enough to read a match's arms and a matches! call exactly instead of
    by regex, which is what let a hyphenated word or a trailing `|| ...`
    through. An unterminated literal or comment, or an escape Rust has not
    got, is an error (None), never a guess.
    """
    out: list[tuple[str, str, int]] = []
    i, n = 0, len(text)

    def bad(msg: str) -> None:
        line = text.count("\n", 0, i) + 1
        err(f"[parse] {where}: {msg} at body line {line} — update the linter if the code is right")

    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            depth, j = 1, i + 2
            while j < n and depth:
                if text.startswith("/*", j):
                    depth, j = depth + 1, j + 2
                elif text.startswith("*/", j):
                    depth, j = depth - 1, j + 2
                else:
                    j += 1
            if depth:
                bad("unterminated block comment")
                return None
            i = j
        elif (m := re.match(r'(b|c)?r(#*)"', text[i:])):
            # A raw string: no escapes, ends at a quote followed by as many #.
            close = '"' + m.group(2)
            j = text.find(close, i + m.end())
            if j < 0:
                bad("unterminated raw string")
                return None
            out.append(("bytes" if m.group(1) else "str", text[i + m.end() : j], i))
            i = j + len(close)
        elif c == '"' or (c in "bc" and text.startswith('"', i + 1)):
            kind = "str" if c == '"' else "bytes"
            j = i + (1 if c == '"' else 2)
            buf: list[str] = []
            while True:
                if j >= n:
                    bad("unterminated string literal")
                    return None
                ch = text[j]
                if ch == '"':
                    j += 1
                    break
                if ch != "\\":
                    buf.append(ch)
                    j += 1
                    continue
                esc = text[j + 1 : j + 2]
                if esc in _RUST_SIMPLE_ESCAPES:
                    buf.append(_RUST_SIMPLE_ESCAPES[esc])
                    j += 2
                elif esc == "x" and re.fullmatch(r"[0-7][0-9A-Fa-f]", text[j + 2 : j + 4]):
                    buf.append(chr(int(text[j + 2 : j + 4], 16)))
                    j += 4
                elif esc == "u" and (um := re.match(r"\{([0-9A-Fa-f_]{1,8})\}", text[j + 2 :])):
                    buf.append(chr(int(um.group(1).replace("_", ""), 16)))
                    j += 2 + um.end()
                elif esc == "\n":
                    # A line continuation: the newline and the next line's
                    # leading whitespace are not part of the string.
                    j += 2
                    while j < n and text[j] in " \t\r\n":
                        j += 1
                else:
                    bad(f"escape \\{esc} is not a Rust string escape")
                    return None
            out.append((kind, "".join(buf), i))
            i = j
        elif c == "'":
            m = re.match(r"'(?:\\(?:[nrt\\0'\"]|x[0-7][0-9A-Fa-f]|u\{[0-9A-Fa-f_]{1,8}\})|[^\\'\n])'",
                         text[i:])
            if m:
                out.append(("char", m.group(0), i))
                i += m.end()
            else:
                m = re.match(r"'[A-Za-z_][A-Za-z0-9_]*", text[i:])
                if not m:
                    bad("a quote that is neither a char literal nor a lifetime")
                    return None
                out.append(("lifetime", m.group(0), i))
                i += m.end()
        elif (m := re.match(r"(?:r#)?[A-Za-z_][A-Za-z0-9_]*", text[i:])):
            out.append(("ident", m.group(0), i))
            i += m.end()
        elif (m := re.match(r"[0-9][0-9A-Za-z_]*", text[i:])):
            out.append(("num", m.group(0), i))
            i += m.end()
        else:
            p = next((p for p in _RUST_PUNCT if text.startswith(p, i)), c)
            out.append(("punct", p, i))
            i += len(p)
    return out


def _tok_is(toks: list, i: int, kind: str, value: str | None = None) -> bool:
    return i < len(toks) and toks[i][0] == kind and (value is None or toks[i][1] == value)


def _tok_line(body: str, toks: list, i: int) -> str:
    """The source line a token sits on, for an error message."""
    at = toks[i][2] if i < len(toks) else len(body)
    start = body.rfind("\n", 0, at) + 1
    end = body.find("\n", at)
    return body[start : end if end >= 0 else len(body)].strip()


def rust_matches_set(body: str, enum: str, what: str) -> set[str] | None:
    """Variants of a method whose WHOLE body is `matches!(self, E::A | E::B)`.

    Anything else in the body — `matches!(self, E::A) || self == E::B`, a
    guard, a second statement — is a parse error and returns None: the
    variants the dictionary pins must be every variant the method answers
    true for, and only a lone matches! makes the set it names that answer.
    (This used to read the first matches!'s arguments and nothing after.)
    """
    where = f"{_ATT_RS} {what}"
    toks = _rust_tokens(body, where)
    if toks is None:
        return None
    shape = f"the body must be exactly one `matches!(self, {enum}::A | {enum}::B)`"
    i = 0
    for kind, value in (("ident", "matches"), ("punct", "!"), ("punct", "("),
                        ("ident", "self"), ("punct", ",")):
        if not _tok_is(toks, i, kind, value):
            err(f"[parse] {where}: {shape}, found `{_tok_line(body, toks, i)}`")
            return None
        i += 1
    if _tok_is(toks, i, "punct", "|"):
        i += 1
    variants: set[str] = set()
    while True:
        if not (_tok_is(toks, i, "ident", enum) and _tok_is(toks, i + 1, "punct", "::")
                and _tok_is(toks, i + 2, "ident")):
            err(f"[parse] {where}: {shape}, found `{_tok_line(body, toks, i)}`")
            return None
        variants.add(toks[i + 2][1])
        i += 3
        if not _tok_is(toks, i, "punct", "|"):
            break
        i += 1
    if _tok_is(toks, i, "punct", ","):
        i += 1
    if not _tok_is(toks, i, "punct", ")") or i + 1 != len(toks):
        err(f"[parse] {where}: {shape} and nothing else, found `{_tok_line(body, toks, i)}`")
        return None
    return variants


def rust_device_word_arms(body: str, what: str) -> tuple[list[tuple[str, str]], str | None] | None:
    """Arms of a body that is exactly `match word { ... }`, as written.

    Returns ([(word, EventClass variant), ...] in source order, the `_ =>`
    fallback's variant or None). Every arm must be one or more string
    literals (`"a" | "b"`, escapes decoded) mapped straight to
    `EventClass::X`, with only `_ => EventClass::X` after them: matching is
    exact and case-sensitive, so the literals ARE the words the hub knows,
    and anything else (a guard, a binding, a normalized `word`, a block) is a
    parse error rather than an arm this reader silently skipped. The old
    reader matched only `"[a-z0-9_]+" => ...`, so `"smoke-alarm" => ...` or
    the second word of `"a" | "b" =>` was never compared at all.
    """
    where = f"{_ATT_RS} {what}"
    toks = _rust_tokens(body, where)
    if toks is None:
        return None
    if not (_tok_is(toks, 0, "ident", "match") and _tok_is(toks, 1, "ident", "word")
            and _tok_is(toks, 2, "punct", "{") and _tok_is(toks, len(toks) - 1, "punct", "}")):
        err(f"[parse] {where}: the body must be exactly `match word {{ ... }}` (exact, "
            f"case-sensitive matching on the word as received), found "
            f"`{_tok_line(body, toks, 0)}`")
        return None
    arms: list[tuple[str, str]] = []
    fallback: str | None = None
    i, end = 3, len(toks) - 1
    while i < end:
        arm_at = i
        shape = ("every arm must be `\"word\" => EventClass::X,` (or `\"a\" | \"b\" => ...`), "
                 "then `_ => EventClass::X`")
        if fallback is not None:
            err(f"[parse] {where}: an arm after the `_ =>` fallback: `{_tok_line(body, toks, i)}`")
            return None
        words: list[str] = []
        wild = False
        if _tok_is(toks, i, "punct", "|"):
            i += 1
        if _tok_is(toks, i, "ident", "_"):
            wild = True
            i += 1
        else:
            while True:
                if not _tok_is(toks, i, "str"):
                    err(f"[parse] {where}: {shape}; found `{_tok_line(body, toks, i)}`")
                    return None
                words.append(toks[i][1])
                i += 1
                if not _tok_is(toks, i, "punct", "|"):
                    break
                i += 1
        if not _tok_is(toks, i, "punct", "=>"):
            err(f"[parse] {where}: {shape}; found `{_tok_line(body, toks, i)}`")
            return None
        i += 1
        braced = _tok_is(toks, i, "punct", "{")
        if braced:
            i += 1
        if not (_tok_is(toks, i, "ident", "EventClass") and _tok_is(toks, i + 1, "punct", "::")
                and _tok_is(toks, i + 2, "ident")):
            err(f"[parse] {where}: {shape}; found `{_tok_line(body, toks, i)}`")
            return None
        variant = toks[i + 2][1]
        i += 3
        if braced:
            if not _tok_is(toks, i, "punct", "}"):
                err(f"[parse] {where}: {shape}; found `{_tok_line(body, toks, i)}`")
                return None
            i += 1
        if _tok_is(toks, i, "punct", ","):
            i += 1
        elif i < end and not braced:
            err(f"[parse] {where}: {shape}; found `{_tok_line(body, toks, arm_at)}`")
            return None
        if wild:
            fallback = variant
        else:
            arms.extend((w, variant) for w in words)
    return arms, fallback


def rust_all_array(text: str, type_name: str, rel: str) -> list[str]:
    """Variants listed in `impl T { pub const ALL: [T; n] = [...] }`."""
    impl = rust_impl_body(text, type_name, rel)
    m = re.search(r"const\s+ALL\s*:\s*\[\s*" + re.escape(type_name) + r"\s*;\s*\d+\s*\]\s*=\s*\[(.*?)\];",
                  impl, re.S)
    if not m:
        err(f"[parse] {rel}: `{type_name}::ALL` not found — update the linter if it moved")
        return []
    return re.findall(re.escape(type_name) + r"::(\w+)", m.group(1))


def rust_const_int(text: str, name: str, rel: str) -> int | None:
    m = re.search(r"\bconst\s+" + re.escape(name) + r"\s*:\s*u\d+\s*=\s*([0-9_]+)\s*;", text)
    if not m:
        err(f"[parse] {rel}: const {name} not found — update the linter if it moved")
        return None
    return int(m.group(1).replace("_", ""))


def cpp_fn_body(text: str, name: str, rel: str) -> str:
    """Body of a C/C++ function DEFINITION `... {name}(...) {`, or ""."""
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^()]*\)\s*\{", text)
    if not m:
        err(f"[parse] could not find the definition of {name}() in {rel} — update the linter")
        return ""
    return _balanced_body(text, m.end() - 1)


def cpp_enum_class_variants(text: str, name: str, rel: str) -> list[str]:
    """Variants of a C++ `enum class {name} : T { ... }`, minus `kCount`."""
    m = re.search(r"\benum\s+class\s+" + re.escape(name) + r"\b[^{;]*\{", text)
    if not m:
        err(f"[parse] could not find `enum class {name}` in {rel} — update the linter if it moved")
        return []
    body = _balanced_body(text, m.end() - 1)
    out = []
    for line in _c_code(body).splitlines():
        vm = re.match(r"^\s*([A-Z][A-Za-z0-9_]*)\b", line)
        if vm and vm.group(1) != "kCount":
            out.append(vm.group(1))
    return out


def _check_id_enum(att_rs: str, rust_type: str, items: list, what: str) -> dict[str, str]:
    """Variants (order included), ALL, and `fn id` of one id-carrying enum.

    Returns {rust_variant: id} as the dictionary states it, for the callers'
    cross-references.
    """
    want_variants = [i["rust_variant"] for i in items]
    got_variants = rust_enum_variants(att_rs, rust_type, _ATT_RS)
    if got_variants != want_variants:
        err(f"[drift] {_ATT_RS} enum {rust_type}: variants {got_variants} vs dictionary "
            f"{what} {want_variants} (order is part of the contract)")
    all_arr = rust_all_array(att_rs, rust_type, _ATT_RS)
    if all_arr and all_arr != want_variants:
        err(f"[drift] {_ATT_RS} {rust_type}::ALL {all_arr} vs dictionary {what} {want_variants}")
    ids = rust_variant_arms(rust_impl_fn_body(att_rs, rust_type, "id", _ATT_RS),
                            rust_type, r'"([a-z0-9_]+)"')
    want = {i["rust_variant"]: i["id"] for i in items}
    if ids != want:
        err(f"[drift] {_ATT_RS} {rust_type}::id(): code {ids} vs dictionary {what} {want}")
    return want


def _check_attention_text(what: str, text: str) -> None:
    """A phrase or reason text: one blank at most ({zone}), no identity word."""
    bare = text.replace("{zone}", "")
    if "{" in bare or "}" in bare:
        err(f"[drift] attention {what} {text!r}: the only blank allowed is {{zone}}")
    for word in re.findall(r"[A-Za-z]+", bare):
        if word.lower() in _ATTENTION_FORBIDDEN_WORDS:
            err(f"[drift] attention {what} {text!r} says {word!r} — no who, no "
                f"known-vs-unknown, no novelty in any wording (spec §7.2, \"No identity, ever\")")


def _check_attention(d: dict) -> None:
    """Pin `household_posture` and `attention` to src/relay/attention.rs.

    Also holds the channel grouping to the sentinel's own modality_of, the
    sustained-dwell threshold to canary-wap notify.h, the governor's numbers
    to the Apple apps' RepeatGovernor, cloud classes to the relay's PokeClass
    wire words, and the conformance vectors' ids to the dictionary.
    """
    hp = d.get("household_posture")
    att = d.get("attention")
    if not hp or not att:
        err("[drift] spec/witness_dictionary.json has no household_posture / attention block")
        return
    src = read(_ATT_RS)
    if not src:
        return

    # Household posture: variants, ids, and the signed statement's wire byte.
    postures = hp["values"]
    _check_id_enum(src, "HouseholdPosture", postures, "household_posture")
    wire = rust_variant_arms(rust_impl_fn_body(src, "HouseholdPosture", "wire_byte", _ATT_RS),
                             "HouseholdPosture", r"(Some\(\s*(?:0x[0-9A-Fa-f]+|\d+)\s*\)|None)")
    for p in postures:
        arm = wire.get(p["rust_variant"])
        got = None if arm in (None, "None") else int(re.search(r"\((.*)\)", arm).group(1).strip(), 0)
        if arm is None:
            err(f"[drift] HouseholdPosture::wire_byte() has no arm for {p['rust_variant']}")
        elif got != p["wire"]:
            err(f"[drift] HouseholdPosture::{p['rust_variant']}.wire_byte(): code {got!r} vs "
                f"dictionary {p['wire']!r} — the posture byte of a signed statement")
    if [p["id"] for p in postures if p["wire"] is None] != ["unset"]:
        err("[drift] household_posture: exactly `unset` must have no wire byte (it is never signed)")

    # The household crate signs the statements, so its enum is the signed
    # subset (everything but `unset`) and its as_byte() is the wire byte.
    hk = read(_HK_POSTURE_RS)
    if hk:
        signed = [p for p in postures if p["wire"] is not None]
        compare(f"HouseholdPosture ({_HK_POSTURE_RS}) vs dictionary household_posture (signed values)",
                [p["rust_variant"] for p in signed],
                rust_enum_variants(hk, "HouseholdPosture", _HK_POSTURE_RS))
        hk_wire = rust_variant_arms(rust_impl_fn_body(hk, "HouseholdPosture", "as_byte", _HK_POSTURE_RS),
                                    "HouseholdPosture", r"(0x[0-9A-Fa-f]+|\d+)")
        for p in signed:
            arm = hk_wire.get(p["rust_variant"])
            if arm is None or int(arm, 0) != p["wire"]:
                err(f"[drift] {_HK_POSTURE_RS} HouseholdPosture::{p['rust_variant']}.as_byte(): "
                    f"code {arm!r} vs dictionary {p['wire']!r} — the signed statement's posture byte")

    # Tiers: declaration order IS the ladder (derive(Ord)), so it must equal rank order.
    tiers = sorted(att["tiers"], key=lambda t: t["rank"])
    _check_id_enum(src, "Tier", tiers, "attention.tiers")
    tier_by_id = {t["id"]: t["rust_variant"] for t in tiers}

    # Zone kinds and which of them household posture acts on.
    zks = att["zone_kinds"]
    _check_id_enum(src, "ZoneKind", zks, "attention.zone_kinds")
    applies = rust_matches_set(rust_impl_fn_body(src, "ZoneKind", "posture_applies", _ATT_RS),
                               "ZoneKind", "ZoneKind::posture_applies()")
    want_applies = {z["rust_variant"] for z in zks if z["posture_applies"]}
    if applies is not None and applies != want_applies:
        err(f"[drift] ZoneKind::posture_applies(): code {sorted(applies)} vs dictionary "
            f"{sorted(want_applies)} — household posture must act on indoor zones only")

    # Modality classes, the corroboration cap, and the sentinel's own classes.
    mods = att["modalities"]["items"]
    _check_id_enum(src, "Modality", mods, "attention.modalities")
    mod_by_id = {m["id"]: m for m in mods}
    cap = rust_const_int(rust_impl_body(src, "CorroborationClasses", _ATT_RS), "MAX", _ATT_RS)
    if att["modalities"]["max_classes"] != len(mods) or (cap is not None and cap != len(mods)):
        err(f"[drift] corroboration cap: dictionary max_classes "
            f"{att['modalities']['max_classes']}, CorroborationClasses::MAX {cap}, "
            f"{len(mods)} modality classes — all three must agree")
    sentinel_h = read(_SENTINEL_H)
    sentinel_mods = cpp_enum_class_variants(sentinel_h, "Modality", _SENTINEL_H) if sentinel_h else []
    named = {m["sentinel_variant"] for m in mods if m["sentinel_variant"]}
    if sentinel_mods and named != set(sentinel_mods):
        err(f"[drift] attention.modalities sentinel_variant {sorted(named)} vs {_SENTINEL_H} "
            f"Modality {sorted(sentinel_mods)} — every sentinel class must be counted, once")

    # Channels: id, class, weak, sees_person — and the sentinel's modality_of.
    chans = att["channels"]["items"]
    _check_id_enum(src, "Channel", chans, "attention.channels")
    modality_arms = rust_variant_arms(rust_impl_fn_body(src, "Channel", "modality", _ATT_RS),
                                      "Channel", r"(Some\(\s*Modality::\w+\s*\)|None)")
    weak = rust_matches_set(rust_impl_fn_body(src, "Channel", "is_weak", _ATT_RS), "Channel",
                            "Channel::is_weak()")
    sees = rust_matches_set(rust_impl_fn_body(src, "Channel", "sees_person", _ATT_RS), "Channel",
                            "Channel::sees_person()")
    for c in chans:
        rv = c["rust_variant"]
        arm = modality_arms.get(rv)
        got = None
        if arm and arm != "None":
            got = re.search(r"Modality::(\w+)", arm).group(1)
        want = mod_by_id[c["modality"]]["rust_variant"] if c["modality"] else None
        if arm is None:
            err(f"[drift] Channel::modality() has no arm for {rv}")
        elif got != want:
            err(f"[drift] Channel::{rv}.modality(): code {got!r} vs dictionary {want!r}")
        if c["modality"] is not None and c["modality"] not in mod_by_id:
            err(f"[drift] attention.channels {c['id']!r} names modality {c['modality']!r}, "
                f"which attention.modalities does not list")
    want_weak = {c["rust_variant"] for c in chans if c["weak"]}
    if weak is not None and weak != want_weak:
        err(f"[drift] Channel::is_weak(): code {sorted(weak)} vs dictionary {sorted(want_weak)}")
    want_sees = {c["rust_variant"] for c in chans if c["sees_person"]}
    if sees is not None and sees != want_sees:
        err(f"[drift] Channel::sees_person(): code {sorted(sees)} vs dictionary {sorted(want_sees)}")
    sentinel_cpp = read(_SENTINEL_CPP)
    if sentinel_cpp and sentinel_h:
        of = dict(re.findall(r"case\s+Channel::(\w+)\s*:\s*return\s+Modality::(\w+)\s*;",
                             cpp_fn_body(_c_code(sentinel_cpp), "modality_of", _SENTINEL_CPP)))
        sentinel_chans = cpp_enum_class_variants(sentinel_h, "Channel", _SENTINEL_H)
        if not of:
            err(f"[parse] {_SENTINEL_CPP}: no `case Channel::X: return Modality::Y;` arms in "
                f"modality_of — update the linter if it moved")
        chan_by_rv = {c["rust_variant"]: c for c in chans}
        for sc in sentinel_chans:
            c = chan_by_rv.get(sc)
            if c is None:
                err(f"[drift] sentinel channel {sc} ({_SENTINEL_H}) is not in attention.channels")
                continue
            ours = mod_by_id.get(c["modality"] or "", {}).get("sentinel_variant")
            if of.get(sc) != ours:
                err(f"[drift] channel {c['id']!r}: dictionary groups it as sentinel class "
                    f"{ours!r}, {_SENTINEL_CPP} modality_of says {of.get(sc)!r} — two channels an "
                    f"evader defeats together must count as one class")

    # Dwell bands, and the 90 s the WAP's own filter uses.
    dwell = att["dwell_bands"]
    _check_id_enum(src, "DwellBand", dwell["items"], "attention.dwell_bands")
    secs = rust_const_int(src, "DWELL_SUSTAINED_SECS", _ATT_RS)
    if secs is not None and secs != dwell["sustained_secs"]:
        err(f"[drift] DWELL_SUSTAINED_SECS {secs} vs dictionary sustained_secs "
            f"{dwell['sustained_secs']}")
    notify = read(_NOTIFY_H)
    if notify:
        nm = re.search(r"MIN_SUSTAINED_MS\s*=\s*(\d+)\s*UL?\s*\*\s*(\d+)\s*UL?\s*;", notify)
        if not nm:
            err(f"[parse] {_NOTIFY_H}: MIN_SUSTAINED_MS (`N * 1000`) not found — update the linter")
        elif int(nm.group(1)) * int(nm.group(2)) != dwell["sustained_secs"] * 1000:
            err(f"[drift] {_NOTIFY_H} MIN_SUSTAINED_MS {nm.group(1)}*{nm.group(2)} ms vs dictionary "
                f"sustained_secs {dwell['sustained_secs']} — one notion of a sustained dwell")

    # Novelty.
    _check_id_enum(src, "Novelty", att["novelty"]["items"], "attention.novelty")

    # Episode families, and the only four that may map to a cloud poke class.
    fams = att["episode_families"]["items"]
    fam_rv = _check_id_enum(src, "EpisodeFamily", fams, "attention.episode_families")
    relay = read(_RELAY_RS)
    sev = rust_variant_arms(rust_fn_body(relay, "sev", _RELAY_RS), "PokeClass",
                            r'"([a-z]+)"') if relay else {}
    cc_body = rust_fn_body(src, "cloud_class", _ATT_RS)
    some = rust_variant_arms(cc_body, "EpisodeFamily", r"Some\(\s*PokeClass::(\w+)\s*\)")
    none = rust_variant_arms(cc_body, "EpisodeFamily", r"(None)\b")
    for f in fams:
        rv, want = f["rust_variant"], f["cloud_class"]
        got = sev.get(some[rv], f"PokeClass::{some[rv]}") if rv in some else None
        if got != want:
            err(f"[drift] cloud_class() for EpisodeFamily::{rv}: code {got!r} vs dictionary "
                f"{want!r}")
        if want is None and rv not in none:
            err(f"[drift] cloud_class(): EpisodeFamily::{rv} must be listed explicitly in the "
                f"`=> None` arm (no wildcard: a new family decides on purpose)")
        if want is not None and want not in sev.values():
            err(f"[drift] attention.episode_families {f['id']!r} cloud_class {want!r} is not a "
                f"PokeClass wire word in {_RELAY_RS}")
        if want == "drill":
            err("[drift] attention.episode_families: `drill` is lane-local and never a cloud class")

    # Episode governor numbers: the Rust consts and the Apple apps' RepeatGovernor.
    gov = att["episode_governor"]
    swift_gov = read(_REPEAT_SWIFT)
    for key, const, swift_name in (
            ("base_cooldown_secs", "EPISODE_BASE_COOLDOWN_SECS", "baseCooldown"),
            ("cooldown_cap_secs", "EPISODE_COOLDOWN_CAP_SECS", "cooldownCap"),
            ("calm_gap_secs", "EPISODE_CALM_GAP_SECS", "calmGap")):
        got = rust_const_int(src, const, _ATT_RS)
        if got is not None and got != gov[key]:
            err(f"[drift] {const} {got} vs dictionary episode_governor.{key} {gov[key]}")
        sm = re.search(r"static\s+let\s+" + swift_name + r"\s*:\s*TimeInterval\s*=\s*(\d+)",
                       swift_gov)
        if not sm:
            err(f"[parse] {_REPEAT_SWIFT}: `static let {swift_name}` not found — update the linter")
        elif int(sm.group(1)) != gov[key]:
            err(f"[drift] {_REPEAT_SWIFT} {swift_name} {sm.group(1)} vs dictionary "
                f"episode_governor.{key} {gov[key]} — hub and phone must rest a burst alike")

    # Phrases and reasons: ids, texts, and the wording rules.
    phrases = att["phrase_ids"]["items"]
    phrase_rv = _check_id_enum(src, "PhraseId", phrases, "attention.phrase_ids")
    texts = rust_variant_arms(rust_impl_fn_body(src, "PhraseId", "text", _ATT_RS),
                              "PhraseId", r'"([^"\\]*)"')
    for p in phrases:
        _check_attention_text(f"phrase {p['id']!r}", p["text"])
        if texts.get(p["rust_variant"]) != p["text"]:
            err(f"[drift] PhraseId::{p['rust_variant']}.text(): code "
                f"{texts.get(p['rust_variant'])!r} vs dictionary {p['text']!r}")
    reasons = att["reason_codes"]["items"]
    _check_id_enum(src, "ReasonCode", reasons, "attention.reason_codes")
    rtexts = rust_variant_arms(rust_impl_fn_body(src, "ReasonCode", "text", _ATT_RS),
                               "ReasonCode", r'"([^"\\]*)"')
    for r in reasons:
        _check_attention_text(f"reason {r['id']!r}", r["id"].replace("_", " "))
        _check_attention_text(f"reason {r['id']!r}", r["text"])
        if rtexts.get(r["rust_variant"]) != r["text"]:
            err(f"[drift] ReasonCode::{r['rust_variant']}.text(): code "
                f"{rtexts.get(r['rust_variant'])!r} vs dictionary {r['text']!r}")

    # Classes: floor, ceiling, family, phrase, person flag — and their sanity.
    classes = att["classes"]["items"]
    _check_id_enum(src, "EventClass", classes, "attention.classes")
    class_by_id = {c["id"]: c for c in classes}
    rank = {t["id"]: t["rank"] for t in tiers}
    floors = rust_variant_arms(rust_impl_fn_body(src, "EventClass", "floor_tier", _ATT_RS),
                               "EventClass", r"Tier::(\w+)")
    ceilings = rust_variant_arms(rust_impl_fn_body(src, "EventClass", "ceiling_tier", _ATT_RS),
                                 "EventClass", r"Tier::(\w+)")
    families = rust_variant_arms(rust_impl_fn_body(src, "EventClass", "family", _ATT_RS),
                                 "EventClass", r"EpisodeFamily::(\w+)")
    phrase_of = rust_variant_arms(rust_impl_fn_body(src, "EventClass", "phrase", _ATT_RS),
                                  "EventClass", r"PhraseId::(\w+)")
    person = rust_matches_set(rust_impl_fn_body(src, "EventClass", "person_class", _ATT_RS),
                              "EventClass", "EventClass::person_class()")
    fam_variant = {f["id"]: f["rust_variant"] for f in fams}
    phrase_variant = {p["id"]: p["rust_variant"] for p in phrases}
    for c in classes:
        rv = c["rust_variant"]
        for field, got_map, want in (
                ("floor_tier", floors, tier_by_id.get(c["floor_tier"])),
                ("ceiling_tier", ceilings, tier_by_id.get(c["ceiling_tier"])),
                ("family", families, fam_variant.get(c["family"])),
                ("phrase", phrase_of, phrase_variant.get(c["phrase"]))):
            if want is None:
                err(f"[drift] attention.classes {c['id']!r} {field} {c[field]!r} is not in the "
                    f"dictionary's own list")
            elif got_map.get(rv) != want:
                err(f"[drift] EventClass::{rv}.{field}(): code {got_map.get(rv)!r} vs "
                    f"dictionary {want!r}")
        if person is not None and (rv in person) != bool(c["person_class"]):
            err(f"[drift] EventClass::{rv}.person_class(): code {rv in person} vs dictionary "
                f"{c['person_class']}")
        f, ce = rank.get(c["floor_tier"], -1), rank.get(c["ceiling_tier"], -1)
        if c["floor_tier"] == "critical" and c["ceiling_tier"] != "critical":
            err(f"[drift] attention.classes {c['id']!r}: a critical floor needs a critical ceiling")
        if c["floor_tier"] != "critical" and c["ceiling_tier"] not in ("digest", "important"):
            err(f"[drift] attention.classes {c['id']!r}: a non-critical class can earn at most "
                f"important (critical comes only from the class)")
        if c["floor_tier"] not in ("digest", "critical") or f > ce:
            err(f"[drift] attention.classes {c['id']!r}: floor {c['floor_tier']!r} / ceiling "
                f"{c['ceiling_tier']!r} — floors are digest or critical, and never above the ceiling")
    used_phrases = {c["phrase"] for c in classes}
    if used_phrases != set(phrase_rv.values()):
        err(f"[drift] attention.phrase_ids not used by any class: "
            f"{sorted(set(phrase_rv.values()) - used_phrases)}")
    used_fams = {c["family"] for c in classes}
    if used_fams != set(fam_rv.values()):
        err(f"[drift] attention.episode_families not used by any class: "
            f"{sorted(set(fam_rv.values()) - used_fams)}")

    # The class map, both dialects.
    cm = att["class_map"]
    ev_items = d["event_types"]["items"]
    if sorted(cm["kernel_event_types"]) != sorted(e["id"] for e in ev_items):
        err(f"[drift] attention.class_map.kernel_event_types keys must be exactly the "
            f"event_types ids; got {sorted(cm['kernel_event_types'])}")
    kernel_arms = rust_variant_arms(rust_fn_body(src, "class_for_event_type", _ATT_RS),
                                    "EventType", r"EventClass::(\w+)")
    for e in ev_items:
        want_class = cm["kernel_event_types"].get(e["id"])
        want = class_by_id.get(want_class, {}).get("rust_variant")
        if want is None:
            err(f"[drift] class_map.kernel_event_types[{e['id']!r}] = {want_class!r} is not a class")
        elif kernel_arms.get(e["rust_variant"]) != want:
            err(f"[drift] class_for_event_type(EventType::{e['rust_variant']}): code "
                f"{kernel_arms.get(e['rust_variant'])!r} vs dictionary {want!r}")
    word_body = rust_fn_body(src, "class_for_device_word", _ATT_RS)
    parsed = rust_device_word_arms(word_body, "class_for_device_word()")
    want_words = {w: class_by_id[c]["rust_variant"] for w, c in cm["device_words"].items()
                  if c in class_by_id}
    for w, c in cm["device_words"].items():
        if c not in class_by_id:
            err(f"[drift] class_map.device_words[{w!r}] = {c!r} is not a class")
        if w in cm["kernel_event_types"]:
            err(f"[drift] class_map.device_words lists {w!r}, a kernel event id — one dialect each")
    fallback = None
    if parsed is not None:
        arms, fallback = parsed
        word_arms: dict[str, str] = {}
        for w, v in arms:
            if w in word_arms:
                err(f"[drift] class_for_device_word(): {w!r} has two arms — one word, one class")
            word_arms[w] = v
            if w not in cm["device_words"]:
                err(f"[drift] class_for_device_word(): arm {w!r} => EventClass::{v} is not a "
                    f"class_map.device_words key — matching is exact and case-sensitive, so a "
                    f"spelling the dictionary does not list is a word it does not govern (add it "
                    f"to the dictionary first, or drop the arm)")
        for w, want in want_words.items():
            if w not in word_arms:
                err(f"[drift] class_for_device_word(): no arm for dictionary device word {w!r} "
                    f"(EventClass::{want})")
            elif word_arms[w] != want:
                err(f"[drift] class_for_device_word({w!r}): code EventClass::{word_arms[w]} vs "
                    f"dictionary EventClass::{want}")
    unknown_cls = class_by_id.get(cm["unknown_word_class"])
    if unknown_cls is None:
        err(f"[drift] class_map.unknown_word_class {cm['unknown_word_class']!r} is not a class")
    else:
        if parsed is not None and fallback != unknown_cls["rust_variant"]:
            err(f"[drift] class_for_device_word(): the `_ =>` fallback must be "
                f"EventClass::{unknown_cls['rust_variant']} (dictionary unknown_word_class)")
        if unknown_cls["ceiling_tier"] != "digest" or unknown_cls["floor_tier"] != "digest":
            err("[drift] attention: the unknown-word class must be digest, floor and ceiling — "
                "an unknown word falls back to digest, never critical")

    _check_attention_fixture(d, class_by_id)


def _check_attention_fixture(d: dict, class_by_id: dict) -> None:
    """Every id the conformance vectors use is a dictionary id, and every word
    vector agrees with the dictionary's class map. The Rust replay proves the
    engine against the vectors; this proves the vectors against the vocabulary,
    in the unfiltered lint job, for every surface that replays them."""
    raw = read(_ATT_FIXTURE)
    if not raw:
        return
    try:
        fx = json.loads(raw)
    except json.JSONDecodeError as e:
        err(f"[drift] {_ATT_FIXTURE} is not valid JSON: {e}")
        return
    att, hp = d["attention"], d["household_posture"]
    ids = {
        "class": set(class_by_id),
        "zone_kind": {z["id"] for z in att["zone_kinds"]},
        "posture": {p["id"] for p in hp["values"]},
        "channel": {c["id"] for c in att["channels"]["items"]},
        "dwell": {b["id"] for b in att["dwell_bands"]["items"]},
        "novelty": {n["id"] for n in att["novelty"]["items"]},
        "tier": {t["id"] for t in att["tiers"]},
        "phrase": {p["id"] for p in att["phrase_ids"]["items"]},
        "reason": {r["id"] for r in att["reason_codes"]["items"]},
        "family": {f["id"] for f in att["episode_families"]["items"]},
    }

    def need(where: str, kind: str, value) -> None:
        if value not in ids[kind]:
            err(f"[drift] {_ATT_FIXTURE} {where}: {kind} {value!r} is not a dictionary id")

    vectors = fx.get("vectors") or []
    if not vectors or not fx.get("words") or not fx.get("episodes"):
        err(f"[drift] {_ATT_FIXTURE}: vectors, words and episodes must all be non-empty")
    for v in vectors:
        name, i, e = v.get("name"), v.get("input", {}), v.get("expect", {})
        for kind in ("class", "zone_kind", "posture", "channel", "dwell", "novelty"):
            need(name, kind, i.get(kind))
        for ch in i.get("corroboration", []):
            need(name, "channel", ch)
        for kind in ("tier", "phrase", "reason"):
            need(name, kind, e.get(kind))
        need(name, "family", e.get("episode", {}).get("family"))
    cm = att["class_map"]
    ev_by_variant = {ev["rust_variant"]: ev["id"] for ev in d["event_types"]["items"]}
    for w in fx.get("words", []):
        if "device_word" in w:
            want = cm["device_words"].get(w["device_word"], cm["unknown_word_class"])
            where = f"device_word {w['device_word']!r}"
        else:
            want = cm["kernel_event_types"].get(ev_by_variant.get(w.get("kernel_event_type")))
            where = f"kernel_event_type {w.get('kernel_event_type')!r}"
        if w.get("class") != want:
            err(f"[drift] {_ATT_FIXTURE} {where}: class {w.get('class')!r} vs dictionary "
                f"class_map {want!r}")
    for s in fx.get("episodes", []):
        for step in s.get("steps", []):
            need(s.get("name"), "tier", step.get("tier"))
            need(s.get("name"), "family", step.get("episode", {}).get("family"))


# Files that BUILD or REBUILD a `securacv-canary-sig|v<n>|<kind>|...` canonical
# string. The union of the kinds they emit must equal the dictionary's
# canonical_kinds — that is what stops a whole signed kind from being invented
# (or, as happened with `whoami`, from existing for months unrecorded).
# `verified` in this project means an Ed25519 signature checked against a pinned
# key, so the list of things that get signed is not an optional detail.
_SIG_KIND_SOURCES = (
    "firmware/common/identity/device_signature.cpp",
    "firmware/projects/canary-wap/arduino/canary_wap/device_signature.cpp",
    "firmware/projects/canary-display/src/trust.cpp",
    "desktop/src-tauri/src/whoami.rs",
    # The kernel's fleet roll-call rebuilds the `chain` canonical to verify
    # peers' publishes against their TOFU pins (and declares SIG_PREFIX /
    # SCHEMA_V, which _check_sig pins to the dictionary below).
    "src/fleet_peers.rs",
)

# `SIG_PREFIX|v1|kind|`, in C (`"%s|v%d|chain|..."`), Python
# (f"{SIG_PREFIX}|v{SCHEMA_V}|chain|..."), and Rust (a literal `|v1|whoami|`,
# or a `format!("{SIG_PREFIX}|v{SCHEMA_V}|chain|…")` — the same brace form
# as Python's f-string, which is why the constant must be spelled SCHEMA_V).
_CANONICAL_KIND_RE = re.compile(r"\|v(?:%d|\{SCHEMA_V\}|\d+)\|([a-z][a-z0-9_]*)\|")

# A file that DEFINES the prefix: C (`constexpr const char* SIG_PREFIX = "…"`),
# Python (`SIG_PREFIX = "…"`) or Rust (`const SIG_PREFIX: &str = "…"`).
_SIG_PREFIX_DEF_RE = re.compile(r'SIG_PREFIX(?:\s*:\s*&\s*(?:\'static\s+)?str)?\s*=\s*"([^"]+)"')
_SCHEMA_V_DEF_RE = re.compile(r"SCHEMA_V(?:\s*:\s*u\d+)?\s*=\s*(\d+)")


def _canonical_kinds(text: str) -> set:
    """Kinds this file builds a canonical string for, from the format literals."""
    return set(_CANONICAL_KIND_RE.findall(text))


def _check_canonical_kinds(sig: dict, found: dict) -> None:
    """Pin signature_format.canonical_kinds to the code that emits the kinds.

    Union-equals, not per-file-equals: a tree may legitimately carry a subset
    (canary-display's trust.cpp verifies chain heads and nothing else). What is
    never legitimate is a kind on the wire that the dictionary does not list,
    or a kind the dictionary lists that nothing implements.
    """
    declared = set(sig.get("canonical_kinds", []))
    if not declared:
        err("[drift] signature_format.canonical_kinds is empty or missing")
        return
    union = set()
    for rel, kinds in sorted(found.items()):
        union |= kinds
        extra = kinds - declared
        if extra:
            err(f"[drift] {rel}: signs kind(s) {sorted(extra)} that "
                f"spec/witness_dictionary.json signature_format.canonical_kinds "
                f"does not list — add them to the dictionary first (FR-13)")
    missing = declared - union
    if missing:
        err(f"[drift] signature_format.canonical_kinds lists {sorted(missing)}, "
            f"which no builder emits (searched {', '.join(_SIG_KIND_SOURCES)} "
            f"and custom_components/securacv/signature.py) — remove it, or point "
            f"_SIG_KIND_SOURCES at the file that implements it")


def _check_sig(rel: str, text: str, sig: dict) -> None:
    pm = _SIG_PREFIX_DEF_RE.search(text)
    sm = _SCHEMA_V_DEF_RE.search(text)
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
