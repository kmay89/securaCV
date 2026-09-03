#!/usr/bin/env python3
"""scripts/carry_to_site.py — refresh every fact the website carries from this repo.

securacv.com (kmay89/securacv_website) is hand-written HTML with no build step,
and several of its pages state facts whose source of truth lives HERE:

  what the website carries                      upstream source of truth
  --------------------------------------------  ------------------------------------
  onboarding-spec.json -> its `builds` block,   firmware/build_matrix.json (+ board
    plus js/onboarding-spec.js, the browser       names from firmware/boards/
    mirror of the whole file                       boards.json): the /checkup
                                                   firmware-type selector
  kernel-status.json                            tools/gen_kernel_status.py: the
                                                   landing page's implementation-
                                                   status grid, DERIVED from the code
  tv/vendor/verify_core.js, the seven fixtures  viewer/verify_core.js and
    tests/tv-wall.test.mjs runs it against, and    tests/fixtures/{envelope,
    tv/vendor/PROVENANCE.txt                       export_bundle}/: the Witness
                                                   Wall's real verifier

(The CAD carries — /scad, js/builder-data.js, scad/cad-dims.json — have their
own tool, docs/hardware/enclosure/gen_builder_manifest.py --site.)

Before this script each of the three was refreshed by a human remembering to:
the `builds` block was re-typed from build_matrix.json by hand and duly fell
four products behind; kernel-status.json carried a "mirrored" date and a note
to "re-run the generator and copy its verdicts"; the verifier had
scripts/vendor_tv_verifier.sh but nothing ran it. Each could sit stale but
internally consistent, green in both repos' CI, indefinitely. This is the one
command that refreshes all three, and the website's weekly "Update everything"
carry job runs it from a fresh checkout of this repo and opens a PR when the
bytes moved.

Direction, honestly: onboarding-spec.json as a whole is WEBSITE-authored (the
probe fixes, the serial-command copy, the OS table). Only its `builds` block is
a projection of build_matrix.json, so this tool STAMPS that one block into the
website's file and leaves every other byte alone — the same way
tools/gen_kernel_status.py stamps a block into docs/witness-kernel.html. The
other two carries are whole files, copied verbatim.

Byte-reproducible: no dates, no commit shas. The same upstream tree always
produces the same bytes, so "did anything change?" is an exact answer.
Transforms live here, never on the website; the website only receives files.

Usage (DIR is the website checkout root):
  python3 scripts/carry_to_site.py --site DIR
  python3 scripts/carry_to_site.py --site DIR --only builds
  python3 scripts/carry_to_site.py --site DIR --only kernel-status
  python3 scripts/carry_to_site.py --site DIR --only verifier

Pinned by scripts/tests/test_carry_to_site.py (discovered by lint.yml).
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BUILD_MATRIX = REPO / "firmware" / "build_matrix.json"
BOARDS = REPO / "firmware" / "boards" / "boards.json"
KERNEL_STATUS_TOOL = REPO / "tools" / "gen_kernel_status.py"
VERIFIER = REPO / "viewer" / "verify_core.js"
FIXTURES = REPO / "tests" / "fixtures"

# The fixtures the website's tests/tv-wall.test.mjs runs the vendored verifier
# against: the valid envelopes for every auth mode + the rotated-key lineage,
# the tamper cases, and the self-attested export bundle. They travel WITH the
# verifier because a behavior test can only catch a stale copy if the inputs it
# exercises are the upstream ones.
ENVELOPE_FIXTURES = [
    "valid_envelope.json",
    "valid_envelope_legacy.json",
    "valid_envelope_self_export.json",
    "valid_envelope_rotated.json",
    "tampered_payload.json",
    "tampered_digest.json",
]
BUNDLE_FIXTURES = ["valid_bundle.json"]

# Website-only decoration for the /checkup product chips. Not a fact about the
# firmware, so it lives in the transform rather than in build_matrix.json; a
# product without one renders without one (checkup.js tolerates a missing
# emoji). Reach ports of the base Canary (flavor "canary") share its bird.
PRODUCT_EMOJI = {
    "canary": "🐤",
    "canary-wap": "📡",
    "canary-vision": "👁️",
    "canary-sense": "📶",
    "canary-display-watch": "⌚",
}
FLAVOR_EMOJI = {"canary": "🐤"}

# The one-line banner on the browser mirror of onboarding-spec.json. The
# website's tests/plugin-facts.test.mjs pins the mirror byte-for-byte to
# HEADER + "window.ONBOARDING_SPEC = " + JSON.stringify(spec, null, 2) + ";\n",
# which is what js_mirror() below writes.
SPEC_JS_HEADER = ("/* AUTO-GENERATED from onboarding-spec.json by securaCV "
                  "scripts/carry_to_site.py — do not edit by hand. */\n")

BUILDS_COMMENT = (
    "GENERATED by securaCV scripts/carry_to_site.py from firmware/build_matrix.json — "
    "do not edit this block by hand (the rest of this file is website-authored). "
    "build_matrix.json is pinned to canary/platformio.ini + canary_config.h by "
    "scripts/lint_build_matrix.py, so these products and per-level feature lists are "
    "what each image actually ships. Drives the /checkup firmware-type selector: pick "
    "a product (+ level, where the S3 canary has dev/release/full) and the self-test / "
    "feature tiles / BLE ladder show exactly what that image does. Only the `canary` "
    "product has levels; the others are single-level board builds. Refresh from the "
    "securaCV checkout: python3 scripts/carry_to_site.py --site <website checkout> "
    "(the website's weekly carry job does the same)."
)


def fail(msg: str) -> None:
    sys.exit(f"carry_to_site.py: {msg}")


def load_json(path: Path):
    if not path.is_file():
        fail(f"missing {path.relative_to(REPO)} — run from a full securaCV checkout")
    return json.loads(path.read_text(encoding="utf-8"))


# --------------------------------------------------------------------------- #
# 1. onboarding-spec.json `builds` <- firmware/build_matrix.json
# --------------------------------------------------------------------------- #

def _build(b: dict) -> dict:
    out = {"env": b["env"]}
    if "partition" in b:
        out["partition"] = b["partition"]
    if "profile" in b:
        out["profile"] = b["profile"]
    out["features"] = list(b["features"])
    return out


def project_builds(matrix: dict, boards: list) -> dict:
    """The website's `builds` block, projected from build_matrix.json.

    The shape is what js/checkup.js reads and tests/plugin-facts.test.mjs pins:
    products with id / name / tier / flow / hasLevels and per-level or single
    `build` feature lists; featureCatalog as key -> label; recommended with
    product / level / why; the flasher deep-link parts. Upstream detail the old
    hand copy dropped rides along (partition, otaProduct, manifestUrl, forChip,
    the board id and name) so the site never needs a second hand-mirror for it.
    """
    board_name = {b["id"]: b["name"] for b in boards}
    products = []
    for p in matrix["products"]:
        out: dict = {"id": p["id"], "name": p["name"]}
        emoji = PRODUCT_EMOJI.get(p["id"]) or FLAVOR_EMOJI.get(p.get("flavor", ""))
        if emoji:
            out["emoji"] = emoji
        if "flavor" in p:
            out["flavor"] = p["flavor"]
        if p["board"] not in board_name:
            fail(f"build_matrix.json product {p['id']!r} names board {p['board']!r}, "
                 "which firmware/boards/boards.json does not have")
        out["mcu"] = p["mcu"]
        out["board"] = p["board"]
        out["boardName"] = board_name[p["board"]]
        out["tier"] = p["tier"]
        out["flow"] = p["flow"]
        out["hasLevels"] = bool(p.get("hasLevels"))
        if "otaProduct" in p:
            out["otaProduct"] = p["otaProduct"]
        if "manifestUrl" in p:
            out["manifest"] = p["manifestUrl"].rsplit("/", 1)[-1]
            out["manifestUrl"] = p["manifestUrl"]
        if out["hasLevels"]:
            out["levels"] = {lvl: _build(b) for lvl, b in p["levels"].items()}
        else:
            out["build"] = _build(p["build"])
        if "note" in p:
            out["note"] = p["note"]
        products.append(out)
    rec = matrix["recommended"]
    return {
        "_comment": BUILDS_COMMENT,
        "downloadBase": matrix["downloadBase"],
        "flasherUrl": matrix["flasher"]["url"],
        "flasherProductPrefix": matrix["flasher"]["productPrefix"],
        "recommended": {
            "product": rec["default"]["product"],
            "level": rec["default"]["level"],
            "forChip": dict(rec["forChip"]),
            "why": rec["why"],
        },
        "featureCatalog": {k: v["label"] for k, v in matrix["featureCatalog"].items()},
        "levels": dict(matrix["levels"]),
        "products": products,
    }


def _skip_string(text: str, i: int) -> int:
    """Given text[i] == '"', return the index just past the closing quote."""
    j = i + 1
    while text[j] != '"':
        if text[j] == "\\":
            j += 1
        j += 1
    return j + 1


def _value_end(text: str, v: int) -> int:
    """Index just past the JSON value that starts at text[v]."""
    c = text[v]
    if c == '"':
        return _skip_string(text, v)
    if c in "{[":
        depth, i = 0, v
        while True:
            ch = text[i]
            if ch == '"':
                i = _skip_string(text, i)
                continue
            if ch in "{[":
                depth += 1
            elif ch in "}]":
                depth -= 1
                if depth == 0:
                    return i + 1
            i += 1
    i = v
    while text[i] not in ",}] \t\r\n":
        i += 1
    return i


def locate_top_level_value(text: str, key: str) -> tuple[int, int]:
    """Offsets [start, end) of the VALUE of top-level `key` in a JSON object.

    A string-aware scan, so the website's hand-formatted file keeps every
    other byte (json.load + json.dump would reflow the whole thing). Raises
    KeyError when the key is not a top-level member.
    """
    i = text.index("{") + 1
    depth = 1
    while i < len(text):
        c = text[i]
        if c == '"':
            j = _skip_string(text, i)
            if depth == 1:
                k = j
                while text[k] in " \t\r\n":
                    k += 1
                if text[k] == ":":
                    if json.loads(text[i:j]) == key:
                        v = k + 1
                        while text[v] in " \t\r\n":
                            v += 1
                        return v, _value_end(text, v)
                    i = k + 1
                    continue
            i = j
            continue
        if c in "{[":
            depth += 1
        elif c in "}]":
            depth -= 1
        i += 1
    raise KeyError(key)


def stamp_builds(spec_text: str, builds: dict) -> str:
    """Replace the `builds` value in the website's onboarding-spec.json text,
    byte-preserving everything else, at the key's own indentation."""
    start, end = locate_top_level_value(spec_text, "builds")
    line_start = spec_text.rfind("\n", 0, start) + 1
    head = spec_text[line_start:start]
    indent = len(head) - len(head.lstrip(" "))
    block = json.dumps(builds, indent=2, ensure_ascii=False)
    block = ("\n" + " " * indent).join(block.split("\n"))
    out = spec_text[:start] + block + spec_text[end:]
    if json.loads(out)["builds"] != builds:
        fail("internal error: the stamped builds block did not round-trip")
    return out


def js_mirror(spec_text: str) -> str:
    """js/onboarding-spec.js — the browser copy every page reads. Python's
    json.dumps(indent=2, ensure_ascii=False) is byte-equal to the
    JSON.stringify(spec, null, 2) the website's test compares against."""
    spec = json.loads(spec_text)
    return (SPEC_JS_HEADER + "window.ONBOARDING_SPEC = "
            + json.dumps(spec, indent=2, ensure_ascii=False) + ";\n")


def carry_builds(site: Path) -> list[Path]:
    spec_path = site / "onboarding-spec.json"
    if not spec_path.is_file():
        fail(f"{spec_path} is missing — this carry stamps a block INTO the website's "
             "file, it does not author the file")
    builds = project_builds(load_json(BUILD_MATRIX), load_json(BOARDS))
    text = spec_path.read_text(encoding="utf-8")
    try:
        stamped = stamp_builds(text, builds)
    except KeyError:
        fail("onboarding-spec.json has no top-level \"builds\" member to stamp")
    spec_path.write_text(stamped, encoding="utf-8")
    js_path = site / "js" / "onboarding-spec.js"
    js_path.write_text(js_mirror(stamped), encoding="utf-8")
    return [spec_path, js_path]


# --------------------------------------------------------------------------- #
# 2. kernel-status.json <- tools/gen_kernel_status.py
# --------------------------------------------------------------------------- #

def kernel_status_module():
    spec = importlib.util.spec_from_file_location("gen_kernel_status", KERNEL_STATUS_TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod


def carry_kernel_status(site: Path) -> list[Path]:
    return [kernel_status_module().write_site(site)]


# --------------------------------------------------------------------------- #
# 3. tv/vendor/verify_core.js + fixtures + PROVENANCE.txt <- viewer/, tests/fixtures/
# --------------------------------------------------------------------------- #

def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def provenance_text() -> str:
    lines = [
        "Vendored from the securaCV repo — the canonical evidence-envelope verifier.",
        "Generated by scripts/carry_to_site.py — DO NOT EDIT verify_core.js or the",
        "carried fixtures by hand; change viewer/verify_core.js in securaCV, then",
        "re-run the carry. The website's tests/tv-wall.test.mjs verifies BEHAVIOR",
        "against these fixtures (a stronger guarantee than a byte-compare).",
        "",
        "upstream: securaCV viewer/verify_core.js",
        f"  verify_core.js  sha256:{_sha256(VERIFIER)}",
        "",
        "upstream fixtures: securaCV tests/fixtures/",
    ]
    lines += [f"  envelope/{f}  sha256:{_sha256(FIXTURES / 'envelope' / f)}"
              for f in ENVELOPE_FIXTURES]
    lines += [f"  export_bundle/{f}  sha256:{_sha256(FIXTURES / 'export_bundle' / f)}"
              for f in BUNDLE_FIXTURES]
    return "\n".join(lines) + "\n"


def carry_verifier(site: Path) -> list[Path]:
    vendor = site / "tv" / "vendor"
    if not vendor.is_dir():
        fail(f"{site} does not look like the website checkout (no tv/vendor)")
    sources = [VERIFIER] + [FIXTURES / "envelope" / f for f in ENVELOPE_FIXTURES] \
        + [FIXTURES / "export_bundle" / f for f in BUNDLE_FIXTURES]
    for src in sources:
        if not src.is_file():
            fail(f"missing {src.relative_to(REPO)}")
    written = []
    dest = vendor / "verify_core.js"
    shutil.copyfile(VERIFIER, dest)
    written.append(dest)
    for sub, names in (("envelope", ENVELOPE_FIXTURES), ("export_bundle", BUNDLE_FIXTURES)):
        (site / "tests" / "fixtures" / sub).mkdir(parents=True, exist_ok=True)
        for f in names:
            dest = site / "tests" / "fixtures" / sub / f
            shutil.copyfile(FIXTURES / sub / f, dest)
            written.append(dest)
    dest = vendor / "PROVENANCE.txt"
    dest.write_text(provenance_text(), encoding="utf-8")
    written.append(dest)
    return written


# --------------------------------------------------------------------------- #

CARRIES = {
    "builds": carry_builds,
    "kernel-status": carry_kernel_status,
    "verifier": carry_verifier,
}


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--site", metavar="DIR", type=Path, required=True,
                    help="the website checkout root (kmay89/securacv_website)")
    ap.add_argument("--only", metavar="CARRY", action="append", choices=sorted(CARRIES),
                    help="refresh only this carry (repeatable); default: all of them")
    args = ap.parse_args(argv)

    site = args.site.resolve()
    if not (site / "js").is_dir() or not (site / "tests").is_dir():
        fail(f"{site} does not look like the website checkout (no js/ and tests/)")
    for name in args.only or list(CARRIES):
        for path in CARRIES[name](site):
            print(f"wrote {path.relative_to(site)}")


if __name__ == "__main__":
    main()
