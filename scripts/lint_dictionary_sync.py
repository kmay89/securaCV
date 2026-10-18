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
    # Several checks read the same item, so one broken item can be reported
    # by each of them in the same words; say it once.
    if msg not in ERRORS:
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

def _enum_variant_items(text: str, name: str, rel: str) -> list[tuple[str, list, list]] | None:
    """[(variant, [its attributes' inner tokens], [its fields / discriminant
    tokens]), ...] of the one `enum {name}`.

    Located from tokens like every Rust item here (rust_item): a
    commented-out or cfg'd copy can never be the enum this reads. The body is
    read from tokens too, variant by variant (attributes, the name, then any
    fields and discriminant up to the next top-level comma). The line-by-line
    reader this replaces took one variant per line, so a second variant on a
    line (`Night, Vacation,`) or one behind a block comment
    (`/* new */ Vacation,`) was never compared at all. A cfg / cfg_attr on a
    variant is a [parse] error: the linter cannot know which configuration
    builds, so it cannot say which variants exist. None after an error.
    """
    item = rust_item(text, rel, "enum", name)
    if item is None:
        return None
    toks, o, c = item
    where = f"{rel} enum {name}"
    out: list[tuple[str, list, list]] = []
    i = o + 1
    while i < c:
        attrs = []
        while _tok_is(toks, i, "punct", "#") and _tok_is(toks, i + 1, "punct", "["):
            close = _rust_close_of(toks, i + 1)
            if close < 0 or close >= c:
                err(f"[parse] {where}: an unbalanced attribute at line {_line_no(text, toks, i)}")
                return None
            if _rust_cfg_attrs(toks, i, close + 1):
                err(f"[parse] {where}: `{_tok_line(text, toks, i)}` (line "
                    f"{_line_no(text, toks, i)}) conditions a variant — the linter reads only "
                    f"unconditional code, since it cannot know which configuration builds")
                return None
            attrs.append(toks[i + 2 : close])
            i = close + 1
        if i >= c:
            break
        if not _tok_is(toks, i, "ident"):
            err(f"[parse] {where}: expected a variant name, found `{_tok_line(text, toks, i)}` "
                f"(line {_line_no(text, toks, i)}) — update the linter if the code is right")
            return None
        variant, i = toks[i][1], i + 1
        rest_at = i
        while i < c and not _tok_is(toks, i, "punct", ","):
            if toks[i][0] == "punct" and toks[i][1] in _RUST_CLOSER:
                close = _rust_close_of(toks, i)
                if close < 0 or close >= c:
                    err(f"[parse] {where}: unbalanced brackets at line {_line_no(text, toks, i)}")
                    return None
                i = close + 1
            else:
                i += 1
        out.append((variant, attrs, toks[rest_at:i]))
        i += 1
    return out


def rust_enum_variants(text: str, name: str, rel: str) -> list[str]:
    """Variant names of the one `enum {name}`, in declaration order ([] after an error)."""
    items = _enum_variant_items(text, name, rel)
    return [] if items is None else [v for v, _, _ in items]


def rust_enum_serde_renames(text: str, name: str, rel: str) -> dict[str, str]:
    """{variant: wire} for each variant carrying `#[serde(rename = "wire")]`."""
    items = _enum_variant_items(text, name, rel)
    renames: dict[str, str] = {}
    for variant, attrs, _ in items or []:
        for a in attrs:
            if (len(a) == 6 and _tok_is(a, 0, "ident", "serde") and _tok_is(a, 1, "punct", "(")
                    and _tok_is(a, 2, "ident", "rename") and _tok_is(a, 3, "punct", "=")
                    and _tok_is(a, 4, "str") and _tok_is(a, 5, "punct", ")")):
                renames[variant] = a[4][1]
    return renames


def _balanced_body(text: str, start: int) -> str:
    """The brace-balanced block beginning at the first `{` at/after `start`.

    For the C, C++ and Swift sources (Rust bodies come from tokens, see
    rust_item_body): a brace inside a `//` or `/* */` comment or a "..." or
    '...' literal is skipped, so it can neither cut a body short nor run it
    on into the next one.
    """
    open_at = text.find("{", start)
    if open_at < 0:
        return ""
    i, n = open_at + 1, len(text)
    depth = 1
    while i < n and depth:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return text[open_at + 1 : i - 1]


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
    # Each read from its own body by the strict match reader (rust_match_arms):
    # the file-wide regex this replaces took any `ClaimKind::X => "..."` in the
    # file — a later comment or test arm overrode the real one — and skipped
    # a guard arm or an early return entirely.
    ct_rel = "src/adapter/contract.rs"
    as_str = rust_match_arms(rust_impl_fn_body(contract, "ClaimKind", "as_str", ct_rel),
                             "ClaimKind", _RHS_STR, "ClaimKind::as_str()", rel=ct_rel)
    to_event = rust_match_arms(rust_item_body(contract, ct_rel, "fn", "claim_kind_to_event_type"),
                               "ClaimKind", _rhs_path("EventType"), "claim_kind_to_event_type()",
                               scrutinee="kind", rel=ct_rel)
    ev_variant_by_id = {e["id"]: e["rust_variant"] for e in ev}
    for c in claims:
        rv = c["rust_variant"]
        if as_str is not None and as_str.get(rv) != c["snake"]:
            err(f"[drift] ClaimKind::{rv}.as_str(): dictionary {c['snake']!r} vs code {as_str.get(rv)!r}")
        want_ev_variant = ev_variant_by_id.get(c["maps_to_event_type"])
        if to_event is not None and to_event.get(rv) != want_ev_variant:
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
    # Read by the strict match reader (rust_match_arms), so a guard arm, an
    # early return or a statement around the match is a [parse] error rather
    # than an arm a regex scan silently skipped.
    hk_rel = "src/bridge/homekit.rs"
    rust_ids = rust_match_arms(rust_item_body(rust_hk, hk_rel, "fn", "as_str"), "HomeSignal",
                               _RHS_STR, "HomeSignal::as_str()", rel=hk_rel)
    rust_haps = rust_match_arms(rust_item_body(rust_hk, hk_rel, "fn", "hap_characteristic"),
                                "HomeSignal", _RHS_STR, "HomeSignal::hap_characteristic()",
                                rel=hk_rel)
    # Matter device types (bridge site D, data-first): explicit Some(...) arms
    # only — a signal the dictionary maps to null must NOT appear in the match
    # body, so it falls to the last arm, `_ => None`.
    rust_matter = rust_match_arms(rust_item_body(rust_hk, hk_rel, "fn", "matter_device_type"),
                                  "HomeSignal", _rhs_some(_RHS_STR),
                                  "HomeSignal::matter_device_type()", rel=hk_rel,
                                  fallback=_RHS_NONE)
    # The tile name Apple's Home app shows is NOT the dictionary label — it is
    # the per-service Name characteristic in src/bridge/hap/accessory.rs. The
    # generated quickstart named the label ("Motion (person)") for a tile the
    # bridge publishes as "Person", so a user was told to look for a name that
    # is not on their screen. Pinned here so the doc cannot say it again.
    hap_accessory = read("src/bridge/hap/accessory.rs")
    acc_rel = "src/bridge/hap/accessory.rs"
    rust_tile_names = rust_match_arms(
        rust_item_body(hap_accessory, acc_rel, "fn", "service_name"), "HomeSignal", _RHS_STR,
        "service_name()", scrutinee="sig", rel=acc_rel)

    swift_hk = read("ios/Sources/SecuraCV/Native/HomeKitBridge.swift")
    swift_haps = swift_computed_property_map(
        swift_hk, "hapCharacteristic", "ios/Sources/SecuraCV/Native/HomeKitBridge.swift")

    for s in hk_signals:
        rv, sid, hap = s["rust_variant"], s["id"], s["hap_characteristic"]
        if rust_ids is not None and rust_ids.get(rv) != sid:
            err(f"[drift] HomeSignal::{rv}.as_str(): dictionary {sid!r} "
                f"vs code {rust_ids.get(rv)!r}")
        if rust_haps is not None and rust_haps.get(rv) != hap:
            err(f"[drift] HomeSignal::{rv}.hap_characteristic(): dictionary "
                f"{hap!r} vs code {rust_haps.get(rv)!r}")
        if rust_matter is not None and rust_matter.get(rv) != s.get("matter_device_type"):
            err(f"[drift] HomeSignal::{rv}.matter_device_type(): dictionary "
                f"{s.get('matter_device_type')!r} vs code {rust_matter.get(rv)!r}")
        swift_case = snake_to_lower_camel(sid)
        if swift_haps.get(swift_case) != hap:
            err(f"[drift] HomeKitBridge.swift .{swift_case}.hapCharacteristic: "
                f"dictionary {hap!r} vs code {swift_haps.get(swift_case)!r}")
        if f'"{s["label"]}"' not in swift_hk:
            err(f"[drift] HomeKitBridge.swift: label {s['label']!r} for "
                f"{sid!r} missing or reworded (labels mirror the dictionary)")
        if rust_tile_names is not None and rust_tile_names.get(rv) != s.get("home_app_name"):
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

    event_arms = rust_match_arms(rust_item_body(rust_hk, hk_rel, "fn", "signals_for_event"),
                                 "EventType", _rhs_slice("HomeSignal"), "signals_for_event()",
                                 scrutinee="event", rel=hk_rel)
    # Signals as the dictionary's snake_case ids, so the comparison is against
    # the vocabulary rather than against Rust spelling.
    rust_events = {ev_v: [_signal_id(sv) for sv in sigs]
                   for ev_v, sigs in (event_arms or {}).items()}
    py_const = read("custom_components/securacv/const.py")
    py_events = py_event_signal_map(py_const)

    if event_arms is not None:
        compare("signals_for_event (src/bridge/homekit.rs) events vs dictionary",
                sorted(hk_events), sorted(rust_events))
    compare("HOMEKIT_EVENT_SIGNALS (custom_components/securacv/const.py) "
            "events vs dictionary", sorted(hk_events), sorted(py_events))

    for event, signals in hk_events.items():
        expected = list(signals)
        if event_arms is not None and rust_events.get(event) != expected:
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
    _self_test_rust_parsers()
    _check_attention(d)

    # --- Every Rust module read above is the file that builds ---
    for rel in ("src/adapter/contract.rs", "src/bridge/homekit.rs",
                "src/bridge/hap/accessory.rs", _ATT_RS, _HK_POSTURE_RS):
        _check_rust_module_chain(rel)

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
# `match` the parsers below read from its own impl body: several impls here
# share method names, so a file-wide scan could not tell which mapping it read.

_ATT_RS = "src/relay/attention.rs"
_HK_POSTURE_RS = "household/src/posture.rs"
_ATT_FIXTURE = "spec/fixtures/attention_v1.json"
_SENTINEL_H = "firmware/common/fusion/sentinel_fusion.h"
_SENTINEL_CPP = "firmware/common/fusion/sentinel_fusion.cpp"
_NOTIFY_H = "firmware/projects/canary-wap/arduino/canary_wap/notify.h"
_REPEAT_SWIFT = "ios/Sources/SecuraCV/Alerts/RepeatGovernor.swift"
_RELAY_RS = "src/relay/mod.rs"
# Number.MAX_SAFE_INTEGER: every integer up to it survives a double.
_JSON_EXACT_MAX = 2**53 - 1

# Words no phrase or reason text may say (spec household_keys_v0 §7.2, "No
# identity, ever"; Free Signals Invariant F and §5): no who, no
# known-vs-unknown, no authorized-vs-not, and the novelty hint never reaches
# wording.
_ATTENTION_FORBIDDEN_WORDS = frozenset({
    "name", "names", "known", "unknown", "stranger", "strangers", "intruder",
    "intruders", "recognized", "verified", "familiar", "unfamiliar",
    "authorized", "unauthorized", "household",
})


_RUST_PUNCT = ("..=", "=>", "::", "||", "&&", "==", "!=", "<=", ">=", "->", "..")
_RUST_SIMPLE_ESCAPES = {"n": "\n", "r": "\r", "t": "\t", "\\": "\\", "0": "\0", "'": "'", '"': '"'}
_RUST_WS_RE = re.compile(r"\s+")
_RUST_RAW_STR_RE = re.compile(r'(b|c)?r(#*)"')
_RUST_CHAR_RE = re.compile(
    r"'(?:\\(?:[nrt\\0'\"]|x[0-7][0-9A-Fa-f]|u\{[0-9A-Fa-f_]{1,8}\})|[^\\'\n])'")
_RUST_LIFETIME_RE = re.compile(r"'[A-Za-z_][A-Za-z0-9_]*")
_RUST_IDENT_RE = re.compile(r"(?:r#)?[A-Za-z_][A-Za-z0-9_]*")
_RUST_NUM_RE = re.compile(r"[0-9][0-9A-Za-z_]*")
_RUST_HEX2_RE = re.compile(r"[0-7][0-9A-Fa-f]")
_RUST_UNICODE_ESC_RE = re.compile(r"\{([0-9A-Fa-f_]{1,8})\}")


def _rust_tokens(text: str, where: str, unit: str = "body") -> list[tuple[str, str, int]] | None:
    """Rust source as (kind, value, offset) tokens, comments dropped.

    Kinds: `str` (value: the literal's DECODED contents, escapes and raw
    strings included, so `"a\\"b"` is the word a"b), `bytes` (a b"" / c""
    literal), `char`, `lifetime`, `ident`, `num`, `punct`. Small on purpose —
    just enough to locate an item and read a match's arms and a matches! call
    exactly instead of by regex, which is what let a hyphenated word, a
    trailing `|| ...` or a decoy in a comment through. An unterminated literal
    or comment, or an escape Rust has not got, is an error (None), never a
    guess. Every pattern is matched in place (`pattern.match(text, i)`), so a
    whole source file tokenizes in linear time.
    """
    out: list[tuple[str, str, int]] = []
    i, n = 0, len(text)

    def bad(msg: str) -> None:
        line = text.count("\n", 0, i) + 1
        err(f"[parse] {where}: {msg} at {unit} line {line} — update the linter if the code is right")

    while i < n:
        c = text[i]
        if c.isspace():
            i = _RUST_WS_RE.match(text, i).end()
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
        elif (m := _RUST_RAW_STR_RE.match(text, i)):
            # A raw string: no escapes, ends at a quote followed by as many #.
            close = '"' + m.group(2)
            j = text.find(close, m.end())
            if j < 0:
                bad("unterminated raw string")
                return None
            out.append(("bytes" if m.group(1) else "str", text[m.end() : j], i))
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
                elif esc == "x" and _RUST_HEX2_RE.match(text, j + 2):
                    buf.append(chr(int(text[j + 2 : j + 4], 16)))
                    j += 4
                elif esc == "u" and (um := _RUST_UNICODE_ESC_RE.match(text, j + 2)):
                    buf.append(chr(int(um.group(1).replace("_", ""), 16)))
                    j = um.end()
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
            if (m := _RUST_CHAR_RE.match(text, i)):
                out.append(("char", m.group(0), i))
                i = m.end()
            elif (m := _RUST_LIFETIME_RE.match(text, i)):
                out.append(("lifetime", m.group(0), i))
                i = m.end()
            else:
                bad("a quote that is neither a char literal nor a lifetime")
                return None
        elif (m := _RUST_IDENT_RE.match(text, i)):
            out.append(("ident", m.group(0), i))
            i = m.end()
        elif (m := _RUST_NUM_RE.match(text, i)):
            out.append(("num", m.group(0), i))
            i = m.end()
        else:
            p = next((p for p in _RUST_PUNCT if text.startswith(p, i)), c)
            out.append(("punct", p, i))
            i += len(p)
    return out


def _tok_is(toks: list, i: int, kind: str, value: str | None = None) -> bool:
    return 0 <= i < len(toks) and toks[i][0] == kind and (value is None or toks[i][1] == value)


def _tok_line(body: str, toks: list, i: int) -> str:
    """The source line a token sits on, for an error message."""
    at = toks[i][2] if 0 <= i < len(toks) else len(body)
    start = body.rfind("\n", 0, at) + 1
    end = body.find("\n", at)
    return body[start : end if end >= 0 else len(body)].strip()


def _line_no(text: str, toks: list, i: int) -> int:
    return text.count("\n", 0, toks[i][2]) + 1


# --------------------------------------------------------------------------
# Rust items, located from tokens
# --------------------------------------------------------------------------
#
# The first raw-text regex hit of `fn NAME(` or `impl T {` can sit inside a
# comment, or be a cfg'd-out twin of the real item, and a dictionary-perfect
# decoy there was read instead of the code that compiles. So an item is found
# among the WHOLE FILE's tokens (comments already gone), it must be the only
# definition of its name in its scope, and a #[cfg(..)] or #[cfg_attr(..)] on
# it or on anything enclosing it is a parse error: the linter cannot know
# which configuration builds, so it refuses to guess. Bodies are token
# ranges, so a brace inside a comment, string or char literal cannot cut one
# short or run it on.

_RUST_TOKEN_CACHE: dict[tuple[str, str], list | None] = {}
_RUST_CLOSER = {"(": ")", "[": "]", "{": "}"}


def _rust_file_tokens(text: str, rel: str) -> list[tuple[str, str, int]] | None:
    """A whole file's tokens, tokenized (and any error reported) once."""
    key = (rel, text)
    if key not in _RUST_TOKEN_CACHE:
        _RUST_TOKEN_CACHE[key] = _rust_tokens(text, rel, unit="file")
    return _RUST_TOKEN_CACHE[key]


def _rust_close_of(toks: list, i: int) -> int:
    """Index of the token closing the ( [ { group opened at toks[i], or -1."""
    stack: list[str] = []
    for j in range(i, len(toks)):
        kind, v, _ = toks[j]
        if kind != "punct":
            continue
        if v in _RUST_CLOSER:
            stack.append(_RUST_CLOSER[v])
        elif v in (")", "]", "}"):
            if not stack or stack.pop() != v:
                return -1
            if not stack:
                return j
    return -1


# Keywords that may stand before a `!` that is negation, not a macro call
# (`if !(..)`, `return !(..)`): `kw !(` is never a macro invocation.
_RUST_NOT_MACRO = frozenset({"if", "while", "match", "return", "in", "let", "else",
                             "break", "yield", "mut", "move", "as"})


def _rust_in_macro(toks: list, k: int) -> int | None:
    """Index of the `!` of the macro call (`name!(..)`, `name![..]`,
    `name!{..}`) or `macro_rules! name {..}` whose token tree holds toks[k],
    or None. Tokens there are not code the linter can see compile: a macro may
    drop them (a dictionary-perfect decoy) or splice them anywhere, so an item
    found inside one is never the item a check reads."""
    stack: list[int] = []
    for j in range(k):
        kind, v, _ = toks[j]
        if kind != "punct":
            continue
        if v in _RUST_CLOSER:
            stack.append(j)
        elif v in (")", "]", "}") and stack:
            stack.pop()
    for o in stack:
        if (_tok_is(toks, o - 1, "punct", "!") and _tok_is(toks, o - 2, "ident")
                and toks[o - 2][1] not in _RUST_NOT_MACRO):
            return o - 1
        if (_tok_is(toks, o - 1, "ident") and _tok_is(toks, o - 2, "punct", "!")
                and _tok_is(toks, o - 3, "ident", "macro_rules")):
            return o - 2
    return None


def _rust_refuse_macro(text: str, toks: list, hits: list, what: str, scope: str) -> bool:
    """False (after a [parse] error) if any hit sits inside a macro's tokens."""
    ok = True
    for k in hits:
        bang = _rust_in_macro(toks, k)
        if bang is not None:
            err(f"[parse] {scope}: {what} (line {_line_no(text, toks, k)}) sits inside the "
                f"tokens of `{toks[bang - 1][1]}!` (line {_line_no(text, toks, bang)}) — a macro "
                f"can drop or rewrite what it is given, so the linter reads only items written "
                f"out as plain code")
            ok = False
    return ok


def _rust_header_start(toks: list, i: int) -> int:
    """First token of the item (or statement) whose keyword or body opener is
    toks[i]: back over its qualifiers and attributes to the previous `;`, `{`
    or `}` outside brackets."""
    depth = 0
    j = i - 1
    while j >= 0:
        kind, v, _ = toks[j]
        if kind == "punct":
            if v in (")", "]"):
                depth += 1
            elif v in ("(", "["):
                if depth == 0:
                    break
                depth -= 1
            elif depth == 0 and v in (";", "{", "}"):
                break
        j -= 1
    return j + 1


def _rust_cfg_attrs(toks: list, start: int, end: int) -> list[int]:
    """`#` of every cfg / cfg_attr attribute, outer `#[..]` or inner `#![..]`,
    in toks[start:end] outside brackets (any path segment named cfg counts)."""
    hits: list[int] = []
    i = start
    while i < end:
        if _tok_is(toks, i, "punct", "#"):
            j = i + 1 if not _tok_is(toks, i + 1, "punct", "!") else i + 2
            if _tok_is(toks, j, "punct", "["):
                k = j + 1
                while _tok_is(toks, k, "ident") or _tok_is(toks, k, "punct", "::"):
                    if toks[k][0] == "ident" and toks[k][1].removeprefix("r#") in ("cfg", "cfg_attr"):
                        hits.append(i)
                        break
                    k += 1
                close = _rust_close_of(toks, j)
                i = close + 1 if close > 0 else end
                continue
        if toks[i][0] == "punct" and toks[i][1] in ("(", "["):
            close = _rust_close_of(toks, i)
            i = close + 1 if close > 0 else end
            continue
        i += 1
    return hits


def _rust_inner_attrs_end(toks: list, open_i: int) -> int:
    """Index just past the inner attributes (`#![..]`) that open a body."""
    i = open_i + 1
    while (_tok_is(toks, i, "punct", "#") and _tok_is(toks, i + 1, "punct", "!")
           and _tok_is(toks, i + 2, "punct", "[")):
        close = _rust_close_of(toks, i + 2)
        if close < 0:
            break
        i = close + 1
    return i


def _rust_refuse_cfg(text: str, toks: list, k: int, open_i: int, where: str) -> bool:
    """False (after a [parse] error) if a cfg / cfg_attr attribute sits on the
    item whose keyword is toks[k] and whose body opens at toks[open_i], opens
    that body, or sits on any item or block enclosing it."""
    spans = [(_rust_header_start(toks, k), open_i)]
    stack: list[int] = []
    for j in range(k):
        if toks[j][0] == "punct":
            if toks[j][1] == "{":
                stack.append(j)
            elif toks[j][1] == "}" and stack:
                stack.pop()
    spans += [(_rust_header_start(toks, b), b) for b in stack]
    ok = True
    for start, body_open in spans:
        hits = _rust_cfg_attrs(toks, start, body_open)
        hits += _rust_cfg_attrs(toks, body_open + 1, _rust_inner_attrs_end(toks, body_open))
        for h in hits:
            err(f"[parse] {where}: `{_tok_line(text, toks, h)}` (line {_line_no(text, toks, h)}) "
                f"conditions the item or something enclosing it — the linter reads only "
                f"unconditional code, since it cannot know which configuration builds")
            ok = False
    return ok


def _rust_is_inherent_impl_of(toks: list, k: int, type_name: str) -> bool:
    """Is the `impl` at toks[k] an inherent `impl [<..>] path::T [where ..] {`?"""
    j, depth = k + 1, 0
    if _tok_is(toks, j, "punct", "<"):
        while j < len(toks):
            if _tok_is(toks, j, "punct", "<"):
                depth += 1
            elif _tok_is(toks, j, "punct", ">"):
                depth -= 1
            j += 1
            if depth == 0:
                break
    last, in_where = None, False
    while j < len(toks):
        kind, v, _ = toks[j]
        if kind == "punct":
            if v in ("{", ";") and depth == 0:
                return v == "{" and last == type_name
            if v == "<":
                depth += 1
            elif v == ">":
                depth -= 1
        elif kind == "ident" and depth == 0 and not in_where:
            if v == "for":
                return False
            if v == "where":
                in_where = True
            else:
                last = v
        j += 1
    return False


def rust_item(text: str, rel: str, kind: str, name: str,
              within: tuple[int, int, str] | None = None,
              allow_cfg: bool = False) -> tuple[list, int, int] | None:
    """(tokens, body opener, body closer) of the one `kind name` item.

    `kind` is `fn`, `enum` or `impl` (an inherent `impl name { .. }`, never a
    trait impl). `within` is (first token, end token, label) to search inside
    an item's body instead of the whole file. Zero or several definitions, no
    body, unbalanced brackets, or a cfg anywhere on the way to it
    (_rust_refuse_cfg, unless `allow_cfg`, which only the test-module check
    uses) is a [parse] error and None.
    """
    toks = _rust_file_tokens(text, rel)
    if toks is None:
        return None
    lo, hi, label = within if within else (0, len(toks), "")
    scope = f"{rel}{' ' + label if label else ''}"
    hits = []
    for k in range(lo, hi):
        if toks[k][0] != "ident" or toks[k][1] != kind:
            continue
        if kind == "impl" and _rust_is_inherent_impl_of(toks, k, name):
            hits.append(k)
        elif kind != "impl" and _tok_is(toks, k + 1, "ident", name):
            hits.append(k)
    what = f"`{kind} {name}`"
    if not hits:
        err(f"[parse] could not find {what} in {scope} — update the linter if it moved")
        return None
    if not _rust_refuse_macro(text, toks, hits, what, scope):
        return None
    if len(hits) > 1:
        lines = ", ".join(str(_line_no(text, toks, k)) for k in hits)
        err(f"[parse] {scope}: {len(hits)} definitions of {what} (lines {lines}) — exactly one "
            f"is allowed, so a decoy or a cfg'd twin can never be the one this reads")
        return None
    k = hits[0]
    j = k + 1
    while j < hi:
        tk, v, _ = toks[j]
        if tk == "punct" and v in ("(", "["):
            close = _rust_close_of(toks, j)
            if close < 0:
                break
            j = close + 1
            continue
        if tk == "punct" and v in ("{", ";"):
            break
        j += 1
    if not _tok_is(toks, j, "punct", "{"):
        err(f"[parse] {scope}: {what} (line {_line_no(text, toks, k)}) has no body the linter "
            f"can find — update the linter if the code is right")
        return None
    close = _rust_close_of(toks, j)
    if close < 0 or close >= hi:
        err(f"[parse] {scope}: {what} (line {_line_no(text, toks, k)}) has unbalanced brackets")
        return None
    if not allow_cfg and not _rust_refuse_cfg(text, toks, k, j, f"{scope} {what}"):
        return None
    return toks, j, close


def rust_item_body(text: str, rel: str, kind: str, name: str) -> str | None:
    """Source text of the one `kind name` item's body (between its braces)."""
    item = rust_item(text, rel, kind, name)
    if item is None:
        return None
    toks, o, c = item
    return text[toks[o][2] + 1 : toks[c][2]]


def rust_impl_body(text: str, type_name: str, rel: str) -> str:
    """Body of the one inherent `impl {type_name} { ... }`, or "" (after an error)."""
    return rust_item_body(text, rel, "impl", type_name) or ""


def rust_impl_fn_body(text: str, type_name: str, fn: str, rel: str) -> str | None:
    """Body of the one `fn {fn}` inside the one inherent `impl {type_name}`,
    or None after a [parse] error."""
    impl = rust_item(text, rel, "impl", type_name)
    if impl is None:
        return None
    toks, o, c = impl
    item = rust_item(text, rel, "fn", fn, within=(o + 1, c, f"impl {type_name}"))
    if item is None:
        return None
    _, fo, fc = item
    return text[toks[fo][2] + 1 : toks[fc][2]]


# A file is only the code that builds if every `mod` declaration from the
# crate root down to it names it plainly. `#[cfg(test)] mod attention;` beside
# `#[cfg(not(test))] #[path = "other.rs"] mod attention;` would let this
# linter, and the test replay with it, read a dictionary-perfect file that
# production never compiles. So each declaration on the way must be the one
# `mod name;` of its name in its parent, at the parent's top level, carry no
# #[path], and be conditioned by nothing but a cargo feature gate (the relay
# and the HomeKit bridge are feature-gated on purpose).

_RUST_MODULE_CHECKED: set[tuple[str, str]] = set()


def _rust_module_parent(rel: str) -> tuple[str, str] | None:
    """(the file declaring module file `rel`, the module's name), or None for
    a crate root."""
    p = Path(rel)
    if p.name in ("lib.rs", "main.rs") and p.parent.name == "src":
        return None
    name, d = (p.parent.name, p.parent.parent) if p.name == "mod.rs" else (p.stem, p.parent)
    if d.name == "src":
        root = d / "lib.rs"
        return (str(root if (ROOT / root).is_file() else d / "main.rs"), name)
    return (str(d / "mod.rs" if (ROOT / d / "mod.rs").is_file() else d.with_suffix(".rs")), name)


def _rust_feature_gate(inner: list) -> bool:
    """Are an attribute's inner tokens exactly `cfg(feature = "...")`?"""
    shape = (("ident", "cfg"), ("punct", "("), ("ident", "feature"), ("punct", "="),
             ("str", None), ("punct", ")"))
    return len(inner) == len(shape) and all(_tok_is(inner, d, kind, v)
                                            for d, (kind, v) in enumerate(shape))


def _rust_bad_module_attrs(text: str, toks: list, start: int, end: int, where: str,
                           inner: bool) -> bool:
    """False (after a [parse] error) if an attribute in toks[start:end] is a
    #[path] or a cfg / cfg_attr other than a plain feature gate."""
    ok = True
    i = start
    while i < end:
        bang = 1 if inner else 0
        if not (_tok_is(toks, i, "punct", "#") and (not inner or _tok_is(toks, i + 1, "punct", "!"))
                and _tok_is(toks, i + 1 + bang, "punct", "[")):
            i += 1
            continue
        close = _rust_close_of(toks, i + 1 + bang)
        if close < 0:
            err(f"[parse] {where}: an unbalanced attribute at line {_line_no(text, toks, i)}")
            return False
        attr = toks[i + 2 + bang : close]
        conditioned = bool(_rust_cfg_attrs(toks, i, close + 1)) and not _rust_feature_gate(attr)
        if conditioned or _tok_is(attr, 0, "ident", "path"):
            err(f"[parse] {where}: `{_tok_line(text, toks, i)}` (line {_line_no(text, toks, i)}) "
                f"— a module the linter reads must be declared as plain `mod name;`, with no "
                f"#[path] and no cfg but a cargo feature gate, or the file it reads may not be "
                f"the one that builds")
            ok = False
        i = close + 1
    return ok


def _check_rust_module_chain(rel: str) -> None:
    """Every `mod` declaration from the crate root down to `rel` is plain."""
    child = rel
    while (up := _rust_module_parent(child)) is not None:
        parent, name = up
        if (parent, name) in _RUST_MODULE_CHECKED:
            return
        _RUST_MODULE_CHECKED.add((parent, name))
        child_text = read(child)
        child_toks = _rust_file_tokens(child_text, child) if child_text else None
        if child_toks:
            lead = 0
            while (_tok_is(child_toks, lead, "punct", "#") and _tok_is(child_toks, lead + 1, "punct", "!")
                   and _tok_is(child_toks, lead + 2, "punct", "[")):
                close = _rust_close_of(child_toks, lead + 2)
                if close < 0:
                    break
                lead = close + 1
            _rust_bad_module_attrs(child_text, child_toks, 0, lead, f"{child} (inner attribute)",
                                   inner=True)
        text = read(parent)
        toks = _rust_file_tokens(text, parent) if text else None
        if not toks:
            return
        where = f"{parent} `mod {name}` (declares {child})"
        hits = [k for k in range(len(toks))
                if _tok_is(toks, k, "ident", "mod") and _tok_is(toks, k + 1, "ident", name)]
        if len(hits) != 1:
            err(f"[parse] {where}: found {len(hits)} declarations, exactly one expected — a "
                f"second (a cfg'd twin) could point the build at another file")
            return
        k = hits[0]
        if not _rust_refuse_macro(text, toks, hits, f"`mod {name}`", parent):
            return
        depth = sum(1 if v == "{" else -1 for kind, v, _ in toks[:k]
                    if kind == "punct" and v in ("{", "}"))
        if depth or not _tok_is(toks, k + 2, "punct", ";"):
            err(f"[parse] {where}: must be a top-level `mod {name};` — an inline or nested "
                f"module is not the file the linter reads (line {_line_no(text, toks, k)})")
            return
        if not _rust_bad_module_attrs(text, toks, _rust_header_start(toks, k), k, where,
                                      inner=False):
            return
        child = parent


# --------------------------------------------------------------------------
# Strict match bodies
# --------------------------------------------------------------------------
#
# Every per-variant fact the dictionary pins (`id`, `wire_byte`, `as_byte`,
# `modality`, `cloud_class`, `text`, `floor_tier`, `ceiling_tier`, `family`,
# `phrase`, `class_for_event_type`, the relay's `sev`) is read by
# rust_match_arms: the WHOLE body must be one `match` on the expected
# scrutinee whose every arm is `E::A | E::B => <rhs>` with the rhs exactly
# the expected shape. An early return, a statement before or after the
# match, a guard, a binding, a wildcard, a block or a call is a [parse]
# error, never an arm silently skipped (a regex scan of `E::X => rhs` arms,
# which this replaces, read the arms it liked and ignored the rest).


def _rhs_path(type_name: str) -> tuple:
    def parse(toks: list, i: int):
        if (_tok_is(toks, i, "ident", type_name) and _tok_is(toks, i + 1, "punct", "::")
                and _tok_is(toks, i + 2, "ident")):
            return toks[i + 2][1], i + 3
        return None
    return parse, f"{type_name}::X"


def _rhs_str_parse(toks: list, i: int):
    return (toks[i][1], i + 1) if _tok_is(toks, i, "str") else None


_RUST_INT_RE = re.compile(r"0x[0-9A-Fa-f][0-9A-Fa-f_]*|[0-9][0-9_]*")


def _rhs_int_parse(toks: list, i: int):
    if not (_tok_is(toks, i, "num") and _RUST_INT_RE.fullmatch(toks[i][1])):
        return None
    lit = toks[i][1].replace("_", "")
    return (int(lit[2:], 16) if lit.startswith("0x") else int(lit, 10)), i + 1


_RHS_STR = (_rhs_str_parse, 'a "string literal"')
_RHS_INT = (_rhs_int_parse, "an integer literal")


def _rhs_option(inner: tuple) -> tuple:
    """`Some(<inner>)` (read as the inner value) or `None` (read as None)."""
    parse_inner, desc = inner

    def parse(toks: list, i: int):
        if _tok_is(toks, i, "ident", "None"):
            return None, i + 1
        if _tok_is(toks, i, "ident", "Some") and _tok_is(toks, i + 1, "punct", "("):
            got = parse_inner(toks, i + 2)
            if got is not None and _tok_is(toks, got[1], "punct", ")"):
                return got[0], got[1] + 1
        return False  # neither: a parse error (None is a value here)
    return parse, f"Some({desc}) or None"


def _rhs_some(inner: tuple) -> tuple:
    """`Some(<inner>)` only, read as the inner value."""
    parse_inner, desc = inner

    def parse(toks: list, i: int):
        if _tok_is(toks, i, "ident", "Some") and _tok_is(toks, i + 1, "punct", "("):
            got = parse_inner(toks, i + 2)
            if got and _tok_is(toks, got[1], "punct", ")"):
                return got[0], got[1] + 1
        return False
    return parse, f"Some({desc})"


_RHS_NONE = (lambda toks, i: (None, i + 1) if _tok_is(toks, i, "ident", "None") else False,
             "None")


def _rhs_slice(type_name: str) -> tuple:
    """`&[T::A, T::B]` or `&[]`, read as a tuple of the variant names."""
    path = _rhs_path(type_name)[0]

    def parse(toks: list, i: int):
        if not (_tok_is(toks, i, "punct", "&") and _tok_is(toks, i + 1, "punct", "[")):
            return False
        close = _rust_close_of(toks, i + 1)
        j, out = i + 2, []
        while 0 <= j < close:
            got = path(toks, j)
            if got is None:
                return False
            out.append(got[0])
            j = got[1]
            if _tok_is(toks, j, "punct", ","):
                j += 1
            elif j != close:
                return False
        return (tuple(out), close + 1) if close > 0 else False
    return parse, f"&[{type_name}::A, ...]"


def rust_match_arms(body: str | None, enum: str, rhs: tuple, what: str, *,
                    scrutinee: str = "self", prelude: str = "",
                    rel: str = _ATT_RS, fallback: tuple | None = None) -> dict | None:
    """{variant: rhs value} of a body that is EXACTLY `[prelude] match
    <scrutinee> { E::A | E::B => <rhs>, ... }`, each variant once; None after
    a [parse] error (or for a body that could not be located, already
    reported). `rhs` is one of the _RHS_* / _rhs_* shapes; a `{ <rhs> }` arm
    body is the same arm as rustfmt wraps it. With `fallback` (an rhs shape),
    one `_ => <fallback>` arm is allowed, as the LAST arm, read under key "_"."""
    if body is None:
        return None
    where = f"{rel} {what}"
    toks = _rust_tokens(body, where)
    if toks is None:
        return None
    parse_rhs, rhs_desc = rhs
    lead = f"{prelude} " if prelude else ""
    tail = f", then `_ => {fallback[1]}`" if fallback else ""
    shape = (f"the body must be exactly `{lead}match {scrutinee} {{ {enum}::A | {enum}::B => "
             f"{rhs_desc}, ... }}`{tail}, each variant once — no early return, statement, "
             f"guard, binding, {'other ' if fallback else ''}wildcard, block or call")
    want = _rust_tokens(f"{lead}match {scrutinee} {{", "lint_dictionary_sync shape")
    for idx, (kind, value, _) in enumerate(want or []):
        if not _tok_is(toks, idx, kind, value):
            err(f"[parse] {where}: {shape}; found `{_tok_line(body, toks, idx)}`")
            return None
    i = len(want or [])
    close = _rust_close_of(toks, i - 1)
    if close != len(toks) - 1:
        at = close + 1 if close >= 0 else i - 1
        err(f"[parse] {where}: {shape} — and nothing after the match; found "
            f"`{_tok_line(body, toks, at)}`")
        return None
    out: dict = {}
    while i < close:
        arm_at = i
        if _tok_is(toks, i, "punct", "|"):
            i += 1
        variants: list[str] = []
        wild = fallback is not None and _tok_is(toks, i, "ident", "_")
        if wild:
            variants.append("_")
            i += 1
        while not wild:
            if not (_tok_is(toks, i, "ident", enum) and _tok_is(toks, i + 1, "punct", "::")
                    and _tok_is(toks, i + 2, "ident")):
                err(f"[parse] {where}: {shape}; found `{_tok_line(body, toks, i)}`")
                return None
            variants.append(toks[i + 2][1])
            i += 3
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
        got = (fallback[0] if wild else parse_rhs)(toks, i)
        if not got:
            err(f"[parse] {where}: {shape}; found `{_tok_line(body, toks, i)}`")
            return None
        value, i = got
        if braced:
            if not _tok_is(toks, i, "punct", "}"):
                err(f"[parse] {where}: {shape}; found `{_tok_line(body, toks, i)}`")
                return None
            i += 1
        if _tok_is(toks, i, "punct", ","):
            i += 1
        elif i != close and not braced:
            err(f"[parse] {where}: {shape}; found `{_tok_line(body, toks, arm_at)}`")
            return None
        for v in variants:
            if v in out:
                err(f"[parse] {where}: {enum}::{v} has two arms — {shape}")
                return None
            out[v] = value
        if wild and i < close:
            err(f"[parse] {where}: the `_ =>` arm must be the last — {shape}")
            return None
    return out


def rust_matches_set(body: str | None, enum: str, what: str,
                     rel: str = _ATT_RS) -> set[str] | None:
    """Variants of a method whose WHOLE body is `matches!(self, E::A | E::B)`.

    Anything else in the body — `matches!(self, E::A) || self == E::B`, a
    guard, a second statement — is a parse error and returns None: the
    variants the dictionary pins must be every variant the method answers
    true for, and only a lone matches! makes the set it names that answer.
    (This used to read the first matches!'s arguments and nothing after.)
    A body that could not be located (None) was already reported.
    """
    if body is None:
        return None
    where = f"{rel} {what}"
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


def rust_device_word_arms(body: str | None, what: str,
                          rel: str = _ATT_RS) -> tuple[list[tuple[str, str]], str | None] | None:
    """Arms of a body that is exactly `match word { ... }`, as written.

    Returns ([(word, EventClass variant), ...] in source order, the `_ =>`
    fallback's variant or None). Every arm must be one or more string
    literals (`"a" | "b"`, escapes decoded) mapped straight to
    `EventClass::X`, with only `_ => EventClass::X` after them: matching is
    exact and case-sensitive, so the literals ARE the words the hub knows,
    and anything else (a guard, a binding, a normalized `word`, a block) is a
    parse error rather than an arm this reader silently skipped. The old
    reader matched only `"[a-z0-9_]+" => ...`, so `"smoke-alarm" => ...` or
    the second word of `"a" | "b" =>` was never compared at all. A body that
    could not be located (None) was already reported.
    """
    if body is None:
        return None
    where = f"{rel} {what}"
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
    """Variants listed in the one `const ALL: [T; n] = [T::A, ...];` of the
    one inherent `impl T`, read from tokens (so a commented copy is never it)."""
    impl = rust_item(text, rel, "impl", type_name)
    if impl is None:
        return []
    toks, o, c = impl
    head = (("ident", "const"), ("ident", "ALL"), ("punct", ":"), ("punct", "["),
            ("ident", type_name), ("punct", ";"))
    hits = [k for k in range(o + 1, c)
            if all(_tok_is(toks, k + d, kind, v) for d, (kind, v) in enumerate(head))]
    shape = f"`const ALL: [{type_name}; n] = [{type_name}::A, ...];` exactly once in impl {type_name}"
    if len(hits) != 1:
        err(f"[parse] {rel}: {shape} — found {len(hits)} — update the linter if it moved")
        return []
    if not _rust_refuse_macro(text, toks, hits, f"{type_name}::ALL", rel):
        return []
    k = hits[0] + len(head)
    if not (_tok_is(toks, k, "num") and _tok_is(toks, k + 1, "punct", "]")
            and _tok_is(toks, k + 2, "punct", "=") and _tok_is(toks, k + 3, "punct", "[")):
        err(f"[parse] {rel}: {shape}; found `{_tok_line(text, toks, k)}`")
        return []
    if not _rust_refuse_cfg(text, toks, hits[0], k + 2, f"{rel} {type_name}::ALL"):
        return []
    close = _rust_close_of(toks, k + 3)
    out: list[str] = []
    i = k + 4
    while 0 <= i < close:
        got = _rhs_path(type_name)[0](toks, i)
        if got is None:
            err(f"[parse] {rel}: {shape}; found `{_tok_line(text, toks, i)}`")
            return []
        out.append(got[0])
        i = got[1]
        if _tok_is(toks, i, "punct", ","):
            i += 1
        elif i != close:
            err(f"[parse] {rel}: {shape}; found `{_tok_line(text, toks, i)}`")
            return []
    if close < 0 or not _tok_is(toks, close + 1, "punct", ";"):
        err(f"[parse] {rel}: {shape}; found `{_tok_line(text, toks, max(close, k))}`")
        return []
    return out


def rust_const_int(text: str, name: str, rel: str) -> int | None:
    """The integer literal of the one `const {name}: uN = <int>;` in `text`
    (a file, or an item body), read from tokens and refused under a cfg."""
    toks = _rust_file_tokens(text, rel)
    if toks is None:
        return None
    hits = [k for k in range(len(toks))
            if _tok_is(toks, k, "ident", "const") and _tok_is(toks, k + 1, "ident", name)]
    if len(hits) != 1:
        err(f"[parse] {rel}: `const {name}` found {len(hits)} times, exactly one expected — "
            f"update the linter if it moved")
        return None
    if not _rust_refuse_macro(text, toks, hits, f"`const {name}`", rel):
        return None
    k = hits[0]
    value = _rhs_int_parse(toks, k + 5)
    if not (_tok_is(toks, k + 2, "punct", ":") and _tok_is(toks, k + 3, "ident")
            and re.fullmatch(r"u(?:8|16|32|64|128|size)", toks[k + 3][1])
            and _tok_is(toks, k + 4, "punct", "=") and value is not None
            and _tok_is(toks, k + 6, "punct", ";")):
        err(f"[parse] {rel}: `const {name}: uN = <integer literal>;` expected, found "
            f"`{_tok_line(text, toks, k)}`")
        return None
    if not _rust_refuse_cfg(text, toks, k, k + 4, f"{rel} const {name}"):
        return None
    return value[0]


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


def _rust_enum_derives(text: str, rel: str, name: str) -> set[str] | None:
    """Every name in the `#[derive(..)]` attributes on the one `enum {name}`."""
    item = rust_item(text, rel, "enum", name)
    if item is None:
        return None
    toks, o, _ = item
    derives: set[str] = set()
    i = _rust_header_start(toks, o)
    while i < o:
        if (_tok_is(toks, i, "punct", "#") and _tok_is(toks, i + 1, "punct", "[")
                and _tok_is(toks, i + 2, "ident", "derive") and _tok_is(toks, i + 3, "punct", "(")):
            close = _rust_close_of(toks, i + 3)
            derives |= {t[1] for t in toks[i + 4 : close] if t[0] == "ident"}
            i = max(close, i) + 1
            continue
        i += 1
    return derives


def _check_id_enum(att_rs: str, rust_type: str, items: list, what: str,
                   ordered: bool = False) -> dict[str, str]:
    """Variants (order included), ALL, and `fn id` of one id-carrying enum.

    Every variant must be bare — no fields, no `= discriminant` — and an
    `ordered` enum (the tier ladder) must derive PartialOrd and Ord: a derived
    order follows the discriminants, so `Important = 3, Critical = 2`, or a
    hand-written Ord, would reorder the ladder behind a declaration order
    this check reads as right.

    Returns {rust_variant: id} as the dictionary states it, for the callers'
    cross-references.
    """
    want_variants = [i["rust_variant"] for i in items]
    variant_items = _enum_variant_items(att_rs, rust_type, _ATT_RS)
    got_variants = [v for v, _, _ in variant_items or []]
    for v, _, rest in variant_items or []:
        if rest:
            err(f"[parse] {_ATT_RS} enum {rust_type}: {v} carries "
                f"`{' '.join(t[1] for t in rest)}` — an id-carrying variant is bare, no fields "
                f"and no discriminant (a discriminant reorders a derived Ord)")
    if ordered:
        derives = _rust_enum_derives(att_rs, _ATT_RS, rust_type)
        if derives is not None and not {"PartialOrd", "Ord"} <= derives:
            err(f"[drift] {_ATT_RS} enum {rust_type}: must #[derive(PartialOrd, Ord)] — "
                f"declaration order is the ladder only when the order is derived (found "
                f"{sorted(derives)})")
    if variant_items is not None and got_variants != want_variants:
        err(f"[drift] {_ATT_RS} enum {rust_type}: variants {got_variants} vs dictionary "
            f"{what} {want_variants} (order is part of the contract)")
    all_arr = rust_all_array(att_rs, rust_type, _ATT_RS)
    if all_arr and all_arr != want_variants:
        err(f"[drift] {_ATT_RS} {rust_type}::ALL {all_arr} vs dictionary {what} {want_variants}")
    ids = rust_match_arms(rust_impl_fn_body(att_rs, rust_type, "id", _ATT_RS),
                          rust_type, _RHS_STR, f"{rust_type}::id()")
    want = {i["rust_variant"]: i["id"] for i in items}
    if ids is not None and ids != want:
        err(f"[drift] {_ATT_RS} {rust_type}::id(): code {ids} vs dictionary {what} {want}")
    return want


def _identity_word_forms(word: str) -> set[str]:
    """The forms of one word the identity-word check looks up: the word, the
    word without a negating `un`, and either without a plural `s`. The list
    names known/unknown and stranger/strangers; this holds the same pattern
    for every word on it (unrecognized, unverified, households, ...). The
    Rust backstop test applies the same three steps."""
    w = word.lower()
    forms = {w, w.removeprefix("un")}
    return forms | {f[:-1] for f in forms if f.endswith("s")}


def _check_attention_text(what: str, text: str) -> None:
    """A phrase or reason text: plain printable ASCII, one blank at most
    ({zone}), no identity word in any of its forms."""
    if any(not " " <= ch <= "~" for ch in text):
        err(f"[drift] attention {what} {text!r}: plain printable ASCII only — an invisible "
            f"or look-alike character (a zero-width space, a soft hyphen, a Cyrillic S) "
            f"would split or disguise a word the identity-word check looks for")
    bare = text.replace("{zone}", "")
    if "{" in bare or "}" in bare:
        err(f"[drift] attention {what} {text!r}: the only blank allowed is {{zone}}")
    for word in re.findall(r"[A-Za-z]+", bare):
        if _identity_word_forms(word) & _ATTENTION_FORBIDDEN_WORDS:
            err(f"[drift] attention {what} {text!r} says {word!r} — no who, no "
                f"known-vs-unknown, no novelty in any wording (spec §7.2, \"No identity, ever\")")


_FORBIDDEN_TEST_FN = "phrases_have_one_blank_and_no_identity_words"


def _check_forbidden_words_mirror(src: str) -> None:
    """The Rust backstop test's FORBIDDEN list is exactly _ATTENTION_FORBIDDEN_WORDS.

    The test refuses an identity word in the engine's texts even where this
    linter does not run; two lists that drifted apart would let a word one of
    them misses (a plural, say) through the other. Read from tokens; the test
    lives in the cfg(test) module by design, so it is the one item read with
    cfg allowed — and still the only definition of its name.
    """
    item = rust_item(src, _ATT_RS, "fn", _FORBIDDEN_TEST_FN, allow_cfg=True)
    if item is None:
        return
    toks, o, c = item
    head = (("ident", "const"), ("ident", "FORBIDDEN"), ("punct", ":"), ("punct", "&"),
            ("punct", "["), ("punct", "&"), ("ident", "str"), ("punct", "]"), ("punct", "="),
            ("punct", "&"), ("punct", "["))
    shape = f'`const FORBIDDEN: &[&str] = &["word", ...];` once in fn {_FORBIDDEN_TEST_FN}'
    hits = [k for k in range(o + 1, c)
            if all(_tok_is(toks, k + d, kind, v) for d, (kind, v) in enumerate(head))]
    if len(hits) != 1:
        err(f"[parse] {_ATT_RS}: {shape} — found {len(hits)}")
        return
    if not _rust_refuse_macro(src, toks, hits, "`const FORBIDDEN`", _ATT_RS):
        return
    close = _rust_close_of(toks, hits[0] + len(head) - 1)
    words: list[str] = []
    i = hits[0] + len(head)
    while 0 <= i < close:
        if not _tok_is(toks, i, "str"):
            err(f"[parse] {_ATT_RS}: {shape}; found `{_tok_line(src, toks, i)}`")
            return
        words.append(toks[i][1])
        i += 1
        if _tok_is(toks, i, "punct", ","):
            i += 1
        elif i != close:
            err(f"[parse] {_ATT_RS}: {shape}; found `{_tok_line(src, toks, i)}`")
            return
    if close < 0 or not _tok_is(toks, close + 1, "punct", ";"):
        err(f"[parse] {_ATT_RS}: {shape}")
        return
    dupes = sorted({w for w in words if words.count(w) > 1})
    missing = sorted(_ATTENTION_FORBIDDEN_WORDS - set(words))
    extra = sorted(set(words) - _ATTENTION_FORBIDDEN_WORDS)
    if dupes or missing or extra:
        err(f"[drift] {_ATT_RS} {_FORBIDDEN_TEST_FN} FORBIDDEN vs this linter's "
            f"_ATTENTION_FORBIDDEN_WORDS: missing {missing}, extra {extra}, repeated {dupes} — "
            f"one list of identity words, kept identical in both")


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
    wire = rust_match_arms(
        rust_impl_fn_body(src, "HouseholdPosture", "wire_byte", _ATT_RS),
        "HouseholdPosture", _rhs_option(_RHS_INT), "HouseholdPosture::wire_byte()")
    for p in postures if wire is not None else []:
        got = wire.get(p["rust_variant"])
        if p["rust_variant"] not in wire:
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
        hk_wire = rust_match_arms(
            rust_impl_fn_body(hk, "HouseholdPosture", "as_byte", _HK_POSTURE_RS),
            "HouseholdPosture", _RHS_INT, "HouseholdPosture::as_byte()", rel=_HK_POSTURE_RS)
        for p in signed if hk_wire is not None else []:
            arm = hk_wire.get(p["rust_variant"])
            if arm is None or arm != p["wire"]:
                err(f"[drift] {_HK_POSTURE_RS} HouseholdPosture::{p['rust_variant']}.as_byte(): "
                    f"code {arm!r} vs dictionary {p['wire']!r} — the signed statement's posture byte")

    # Tiers: declaration order IS the ladder (derive(Ord)), so it must equal rank order.
    tiers = sorted(att["tiers"], key=lambda t: t["rank"])
    _check_id_enum(src, "Tier", tiers, "attention.tiers", ordered=True)
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
    modality_arms = rust_match_arms(rust_impl_fn_body(src, "Channel", "modality", _ATT_RS),
                                    "Channel", _rhs_option(_rhs_path("Modality")),
                                    "Channel::modality()")
    weak = rust_matches_set(rust_impl_fn_body(src, "Channel", "is_weak", _ATT_RS), "Channel",
                            "Channel::is_weak()")
    sees = rust_matches_set(rust_impl_fn_body(src, "Channel", "sees_person", _ATT_RS), "Channel",
                            "Channel::sees_person()")
    for c in chans:
        rv = c["rust_variant"]
        want = mod_by_id[c["modality"]]["rust_variant"] if c["modality"] in mod_by_id else None
        if modality_arms is not None and rv not in modality_arms:
            err(f"[drift] Channel::modality() has no arm for {rv}")
        elif modality_arms is not None and modality_arms[rv] != want:
            err(f"[drift] Channel::{rv}.modality(): code {modality_arms[rv]!r} vs dictionary "
                f"{want!r}")
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
    sev = rust_match_arms(rust_impl_fn_body(relay, "PokeClass", "sev", _RELAY_RS),
                          "PokeClass", _RHS_STR, "PokeClass::sev()", rel=_RELAY_RS) if relay else None
    # cloud_class's one statement before its match is the critical gate; it
    # is part of the shape, so anything else there is a parse error too.
    classes_of = rust_match_arms(
        rust_item_body(src, _ATT_RS, "fn", "cloud_class"), "EpisodeFamily",
        _rhs_option(_rhs_path("PokeClass")), "cloud_class()",
        scrutinee="decision.episode.family",
        prelude="if decision.tier != Tier::Critical { return None; }")
    for f in fams:
        rv, want = f["rust_variant"], f["cloud_class"]
        if want == "drill":
            err("[drift] attention.episode_families: `drill` is lane-local and never a cloud class")
        if sev is None:
            continue
        if want is not None and want not in sev.values():
            err(f"[drift] attention.episode_families {f['id']!r} cloud_class {want!r} is not a "
                f"PokeClass wire word in {_RELAY_RS}")
        if classes_of is None:
            continue
        if rv not in classes_of:
            err(f"[drift] cloud_class(): EpisodeFamily::{rv} has no arm — list it explicitly "
                f"(no wildcard: a new family decides on purpose)")
            continue
        variant = classes_of[rv]
        got = None if variant is None else sev.get(variant, f"PokeClass::{variant}")
        if got != want:
            err(f"[drift] cloud_class() for EpisodeFamily::{rv}: code {got!r} vs dictionary "
                f"{want!r}")

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
    texts = rust_match_arms(rust_impl_fn_body(src, "PhraseId", "text", _ATT_RS),
                            "PhraseId", _RHS_STR, "PhraseId::text()")
    for p in phrases:
        _check_attention_text(f"phrase {p['id']!r}", p["text"])
        if texts is not None and texts.get(p["rust_variant"]) != p["text"]:
            err(f"[drift] PhraseId::{p['rust_variant']}.text(): code "
                f"{texts.get(p['rust_variant'])!r} vs dictionary {p['text']!r}")
    _check_forbidden_words_mirror(src)
    reasons = att["reason_codes"]["items"]
    _check_id_enum(src, "ReasonCode", reasons, "attention.reason_codes")
    rtexts = rust_match_arms(rust_impl_fn_body(src, "ReasonCode", "text", _ATT_RS),
                             "ReasonCode", _RHS_STR, "ReasonCode::text()")
    for r in reasons:
        _check_attention_text(f"reason {r['id']!r}", r["id"].replace("_", " "))
        _check_attention_text(f"reason {r['id']!r}", r["text"])
        if rtexts is not None and rtexts.get(r["rust_variant"]) != r["text"]:
            err(f"[drift] ReasonCode::{r['rust_variant']}.text(): code "
                f"{rtexts.get(r['rust_variant'])!r} vs dictionary {r['text']!r}")

    # Classes: floor, ceiling, family, phrase, person flag — and their sanity.
    classes = att["classes"]["items"]
    _check_id_enum(src, "EventClass", classes, "attention.classes")
    class_by_id = {c["id"]: c for c in classes}
    rank = {t["id"]: t["rank"] for t in tiers}
    def class_arms(fn: str, rhs_type: str) -> dict | None:
        return rust_match_arms(rust_impl_fn_body(src, "EventClass", fn, _ATT_RS),
                               "EventClass", _rhs_path(rhs_type), f"EventClass::{fn}()")
    floors = class_arms("floor_tier", "Tier")
    ceilings = class_arms("ceiling_tier", "Tier")
    families = class_arms("family", "EpisodeFamily")
    phrase_of = class_arms("phrase", "PhraseId")
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
            elif got_map is not None and got_map.get(rv) != want:
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
    kernel_arms = rust_match_arms(rust_item_body(src, _ATT_RS, "fn", "class_for_event_type"),
                                  "EventType", _rhs_path("EventClass"), "class_for_event_type()",
                                  scrutinee="event")
    for e in ev_items:
        want_class = cm["kernel_event_types"].get(e["id"])
        want = class_by_id.get(want_class, {}).get("rust_variant")
        if want is None:
            err(f"[drift] class_map.kernel_event_types[{e['id']!r}] = {want_class!r} is not a class")
        elif kernel_arms is not None and kernel_arms.get(e["rust_variant"]) != want:
            err(f"[drift] class_for_event_type(EventType::{e['rust_variant']}): code "
                f"{kernel_arms.get(e['rust_variant'])!r} vs dictionary {want!r}")
    word_body = rust_item_body(src, _ATT_RS, "fn", "class_for_device_word")
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
            at = step.get("at")
            if type(at) is not int or not 0 <= at <= _JSON_EXACT_MAX:
                err(f"[drift] {_ATT_FIXTURE} {s.get('name')!r}: `at` {at!r} must be an integer "
                    f"second from 0 to {_JSON_EXACT_MAX} (2^53 - 1), the largest a JSON number "
                    f"carries exactly on every surface that replays the vectors (a JavaScript "
                    f"or double-based parser rounds anything above it)")


# --------------------------------------------------------------------------
# Self-test: the Rust readers refuse every shape that once fooled them
# --------------------------------------------------------------------------
#
# Run on every invocation (lint.yml runs this script with no flags), in a few
# milliseconds: each case is a small Rust source fed to the same readers the
# attention checks use, and a reader that stops refusing one is itself drift.

_SELF_TEST = "<lint self-test>"


def _run_isolated(reader, source: str) -> tuple:
    """(result, errors) of reader(source), its errors kept out of ERRORS."""
    saved = ERRORS[:]
    del ERRORS[:]
    try:
        return reader(source), ERRORS[:]
    finally:
        ERRORS[:] = saved


def _st_text(src: str):
    return rust_match_arms(rust_impl_fn_body(src, "PhraseId", "text", _SELF_TEST), "PhraseId",
                           _RHS_STR, "PhraseId::text()", rel=_SELF_TEST)


def _st_floor(src: str):
    return rust_match_arms(rust_impl_fn_body(src, "EventClass", "floor_tier", _SELF_TEST),
                           "EventClass", _rhs_path("Tier"), "EventClass::floor_tier()",
                           rel=_SELF_TEST)


def _st_kernel(src: str):
    return rust_match_arms(rust_item_body(src, _SELF_TEST, "fn", "class_for_event_type"),
                           "EventType", _rhs_path("EventClass"), "class_for_event_type()",
                           scrutinee="event", rel=_SELF_TEST)


def _st_words(src: str):
    return rust_device_word_arms(rust_item_body(src, _SELF_TEST, "fn", "class_for_device_word"),
                                 "class_for_device_word()", rel=_SELF_TEST)


def _st_weak(src: str):
    return rust_matches_set(rust_impl_fn_body(src, "Channel", "is_weak", _SELF_TEST), "Channel",
                            "Channel::is_weak()", rel=_SELF_TEST)


def _st_variants(src: str):
    items = _enum_variant_items(src, "Tier", _SELF_TEST)
    if items is None:
        return None
    names = rust_enum_variants(src, "Tier", _SELF_TEST)
    if names != [v for v, _, _ in items]:
        return ("rust_enum_variants disagrees", names)
    return [(v, " ".join(t[1] for t in rest)) for v, _, rest in items]


def _st_matter(src: str):
    return rust_match_arms(rust_item_body(src, _SELF_TEST, "fn", "matter_device_type"),
                           "HomeSignal", _rhs_some(_RHS_STR), "matter_device_type()",
                           rel=_SELF_TEST, fallback=_RHS_NONE)


def _st_const(src: str):
    return rust_const_int(src, "EPISODE_CALM_GAP_SECS", _SELF_TEST)


def _st_mod(src: str):
    """True if the one `mod attention` declaration in src is plain, else None."""
    toks = _rust_tokens(src, _SELF_TEST, unit="file")
    k = next(k for k in range(len(toks)) if _tok_is(toks, k, "ident", "mod"))
    ok = _rust_bad_module_attrs(src, toks, _rust_header_start(toks, k), k, _SELF_TEST, inner=False)
    return True if ok else None


def _st_phrase(body: str) -> str:
    return ("impl PhraseId {\n    pub fn text(self) -> &'static str {\n" + body
            + "\n    }\n}\n")


_ST_KERNEL_FN = ("pub fn class_for_event_type(event: &EventType) -> EventClass {\n"
                 "    match event { EventType::TamperDetected => EventClass::Tamper }\n}\n")
_ST_WORDS_FN = ("pub fn class_for_device_word(word: &str) -> EventClass {\n"
                "    match word { \"smoke-alarm\" => EventClass::SmokeAlarmHeard, "
                "_ => EventClass::Other }\n}\n")
_PARSE = "a [parse] error"

_SELF_TEST_CASES = (
    ("a plain match is read", _st_text,
     _st_phrase('match self { PhraseId::A => "a at {zone}", PhraseId::B => { "b" } }'),
     {"A": "a at {zone}", "B": "b"}),
    ("a brace inside a string literal neither cuts the body short nor runs it on", _st_text,
     _st_phrase('match self { PhraseId::A => "a {", PhraseId::B => "} b" }'),
     {"A": "a {", "B": "} b"}),
    ("an early return before the match", _st_text,
     _st_phrase('if matches!(self, PhraseId::A) { return "Strangers at {zone}"; }\n'
                'match self { PhraseId::A => "a", PhraseId::B => "b" }'), _PARSE),
    ("a guard arm in PhraseId::text", _st_text,
     _st_phrase('match self { PhraseId::A if true => "Strangers at {zone}", '
                'PhraseId::A | PhraseId::B => "b" }'), _PARSE),
    ("a binding arm", _st_text,
     _st_phrase('match self { x @ PhraseId::A => "a", PhraseId::B => "b" }'), _PARSE),
    ("a wildcard arm", _st_text,
     _st_phrase('match self { PhraseId::A => "a", _ => "b" }'), _PARSE),
    ("a call after the match, hidden from brace counting by a `// }` comment", _st_text,
     _st_phrase('match self { PhraseId::A => "a", PhraseId::B => "b" } // }\n'
                '.trim_start_matches("a")'), _PARSE),
    ("a guard arm in EventClass::floor_tier", _st_floor,
     "impl EventClass {\n    pub fn floor_tier(self) -> Tier {\n        match self {\n"
     "            EventClass::Tamper if true => Tier::Digest,\n"
     "            EventClass::Tamper | EventClass::Other => Tier::Critical,\n"
     "        }\n    }\n}\n", _PARSE),
    ("a guard arm in class_for_event_type", _st_kernel,
     "pub fn class_for_event_type(event: &EventType) -> EventClass {\n    match event {\n"
     "        EventType::TamperDetected if true => EventClass::Other,\n"
     "        EventType::TamperDetected => EventClass::Tamper,\n    }\n}\n", _PARSE),
    ("a decoy in a string literal is not an item", _st_kernel,
     'const DECOY: &str = "pub fn class_for_event_type(event: &EventType) -> EventClass '
     '{ match event { EventType::TamperDetected => EventClass::Other } }";\n' + _ST_KERNEL_FN,
     {"TamperDetected": "Tamper"}),
    ("a /* commented */ decoy above the real class_for_device_word", _st_words,
     "/*\npub fn class_for_device_word(word: &str) -> EventClass {\n"
     "    match word { \"smoke_alarm_t3\" => EventClass::SmokeAlarmHeard, "
     "_ => EventClass::Other }\n}\n*/\n" + _ST_WORDS_FN,
     ([("smoke-alarm", "SmokeAlarmHeard")], "Other")),
    ("a #[cfg(test)] decoy beside a #[cfg(not(test))] real function", _st_kernel,
     "#[cfg(test)]\n" + _ST_KERNEL_FN.replace("EventClass::Tamper", "EventClass::Other")
     + "#[cfg(not(test))]\n" + _ST_KERNEL_FN, _PARSE),
    ("a lone #[cfg(not(test))] function", _st_kernel,
     "#[cfg(not(test))]\n" + _ST_KERNEL_FN, _PARSE),
    ("a #[cfg_attr(..)] on the function", _st_kernel,
     "#[cfg_attr(test, allow(dead_code))]\n" + _ST_KERNEL_FN, _PARSE),
    ("an inner #![cfg(..)] opening the function's body", _st_kernel,
     _ST_KERNEL_FN.replace("{\n    match", "{\n    #![cfg(test)]\n    match"), _PARSE),
    ("the function inside a #[cfg(test)] module", _st_kernel,
     "#[cfg(test)]\nmod m {\n" + _ST_KERNEL_FN + "}\n", _PARSE),
    ("the method inside a #[cfg(test)] impl", _st_text,
     "#[cfg(test)]\n" + _st_phrase('match self { PhraseId::A => "a" }'), _PARSE),
    ("a doc-comment decoy above the real is_weak", _st_weak,
     "impl Channel {\n    /// pub fn is_weak(self) -> bool { matches!(self, Channel::Pir) }\n"
     "    pub fn is_weak(self) -> bool {\n"
     "        matches!(self, Channel::Pir) || self == Channel::Contact\n    }\n}\n", _PARSE),
    ("a decoy inside a macro's tokens, the real method macro-generated", _st_text,
     "macro_rules! drop_all { ($($t:tt)*) => {}; }\nimpl PhraseId {\n    drop_all! {\n"
     "        pub fn text(self) -> &'static str { match self { PhraseId::A => \"a\" } }\n"
     "    }\n    make_text!(text);\n}\n", _PARSE),
    ("Some arms then a last `_ => None` are read", _st_matter,
     "fn matter_device_type(self) -> Option<&'static str> {\n    match self {\n"
     "        HomeSignal::A => Some(\"a\"),\n        _ => None,\n    }\n}\n",
     {"A": "a", "_": None}),
    ("a `_ =>` arm before the last one", _st_matter,
     "fn matter_device_type(self) -> Option<&'static str> {\n    match self {\n"
     "        _ => None,\n        HomeSignal::A => Some(\"a\"),\n    }\n}\n", _PARSE),
    ("a decoy const inside a macro's tokens", _st_const,
     "ignore! { pub const EPISODE_CALM_GAP_SECS: u64 = 1800; }\nmk!(EPISODE_CALM_GAP_SECS, 60);\n",
     _PARSE),
    ("every variant is read: two on a line, one behind a block comment, fields and a "
     "discriminant kept for the caller", _st_variants,
     "pub enum Tier {\n    A, B,\n    /* new */ C,\n    D(u8),\n    E = 4,\n}\n",
     [("A", ""), ("B", ""), ("C", ""), ("D", "( u8 )"), ("E", "= 4")]),
    ("a cfg on a variant", _st_variants,
     "pub enum Tier {\n    A,\n    #[cfg(test)]\n    B,\n}\n", _PARSE),
    ("a plain feature-gated module declaration", _st_mod,
     '#[cfg(feature = "alert-relay")]\npub mod attention;\n', True),
    ("a #[path] on the module declaration", _st_mod,
     '#[path = "attention_prod.rs"]\npub mod attention;\n', _PARSE),
    ("a cfg(test) on the module declaration", _st_mod,
     "#[cfg(test)]\npub mod attention;\n", _PARSE),
)

# Phrase and reason texts the identity-word check must refuse (True) or let
# through (False): a word behind a negating `un` or a plural `s`, or split or
# disguised by an invisible or look-alike character.
_SELF_TEST_TEXTS = (
    ("Someone at {zone}", False),
    ("Unrecognized person at {zone}", True),
    ("Households at {zone}", True),
    ("Stran\u200bgers at {zone}", True),
    ("\u0405trangers at {zone}", True),
)


def _self_test_rust_parsers() -> None:
    """Every _SELF_TEST_CASES case reads as it must, or a [self-test] error."""
    for label, reader, source, want in _SELF_TEST_CASES:
        got, errs = _run_isolated(reader, source)
        if want == _PARSE:
            if got is not None or not any(e.startswith("[parse]") for e in errs):
                err(f"[self-test] {label}: the reader must refuse it with {_PARSE}; it read "
                    f"{got!r} (errors: {errs})")
        elif errs or got != want:
            err(f"[self-test] {label}: expected {want!r} and no error; got {got!r} "
                f"(errors: {errs})")
    for text, refused in _SELF_TEST_TEXTS:
        _, errs = _run_isolated(lambda t: _check_attention_text("self-test", t), text)
        if refused != any(e.startswith("[drift]") for e in errs):
            err(f"[self-test] the identity-word check must {'refuse' if refused else 'accept'} "
                f"{text!r}; errors: {errs}")


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
