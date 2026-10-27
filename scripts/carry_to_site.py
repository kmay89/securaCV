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
  tests/fixtures/fleet_contract_vectors.json    tvos/witness-core/tests/fixtures/
    (rides the verifier carry; pinned in           fleet_contract_vectors.json:
    the same PROVENANCE.txt)                       the GET /api/fleet contract the
                                                   Wall's parseFleet replays
  js/playground-data.js                         canary-local/devices/playground.json
                                                   (gen_playground.py): the /playground
                                                   Test bench's pins, stations and
                                                   fw_version
  js/playground-sim.js, js/mode-sim.js          canary-local/assets/{playground,mode}-
                                                   sim.js: the bench's PG1 driver port
                                                   and the /modes page's mode registry
                                                   port, each drift-gated here
  download.html's app-v<x.y.z> and              this repo's newest PUBLISHED app-v*
    flasher-v<x.y.z> tokens: the /download         and flasher-v* releases (not draft,
    buttons' exact release pages and the           not prerelease), read off the GitHub
    note that names them                           REST API: never tauri.conf.json

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
bytes moved. The playground carry joined them in 2026-10: its three files were
website hand copies headed HAND-COPIED, and every firmware version bump left
playground-data.js's fw_version behind. The downloads carry joined them the
same month: the /download buttons had opened GitHub's release search, which
matches text, not tags, and a hand-typed tag goes stale with the next release.

Direction, honestly: onboarding-spec.json as a whole is WEBSITE-authored (the
probe fixes, the serial-command copy, the OS table). Only its `builds` block is
a projection of build_matrix.json, so this tool STAMPS that one block into the
website's file and leaves every other byte alone — the same way
tools/gen_kernel_status.py stamps a block into docs/witness-kernel.html. The
other carries are whole files: copied verbatim, or (the playground carry) the
upstream bytes under a CARRIED header the website's tests/carry-claims.test.mjs
reads, with playground.json wrapped as an ES module (`export const PLAYGROUND =`
and the JSON, its trailing newline replaced by `;\n`).
The downloads carry is a stamp too: in download.html it moves the version in
each app-v<x.y.z> and flasher-v<x.y.z> token and nothing else.

Byte-reproducible: no dates, no commit shas. The same upstream tree always
produces the same bytes, so "did anything change?" is an exact answer. The
downloads carry's one input outside the tree is the release list: the same
published releases always stamp the same bytes, and a run that cannot read
the list (offline, rate-limited, an API error after retries) writes nothing
and says why. It never stamps a guess.
Transforms live here, never on the website; the website only receives files.

Usage (DIR is the website checkout root):
  python3 scripts/carry_to_site.py --site DIR
  python3 scripts/carry_to_site.py --site DIR --only builds
  python3 scripts/carry_to_site.py --site DIR --only kernel-status
  python3 scripts/carry_to_site.py --site DIR --only verifier
  python3 scripts/carry_to_site.py --site DIR --only playground
  python3 scripts/carry_to_site.py --site DIR --only downloads
  python3 scripts/carry_to_site.py --site DIR --only downloads --check

The downloads carry reads the releases with GH_TOKEN when it is set (the
API's unauthenticated limit is 60 requests an hour). --check writes nothing:
it exits 1 when download.html does not name the newest published releases,
or when the release list cannot be read (a check that cannot read the truth
does not pass). It is the downloads carry's alone; the weekly job's diff is
the other carries' check.

Pinned by scripts/tests/test_carry_to_site.py (discovered by lint.yml).
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import shutil
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BUILD_MATRIX = REPO / "firmware" / "build_matrix.json"
BOARDS = REPO / "firmware" / "boards" / "boards.json"
KERNEL_STATUS_TOOL = REPO / "tools" / "gen_kernel_status.py"
VERIFIER = REPO / "viewer" / "verify_core.js"
FIXTURES = REPO / "tests" / "fixtures"
# The GET /api/fleet contract vectors (tvos/discovery/DISCOVERY.md). The Rust
# core replays them in tvos/witness-core/tests/fleet_contract.rs; the website's
# tests/tv-wall.test.mjs replays the carried copy through the Wall's parseFleet,
# so the two decoders cannot disagree on a default. They land at the site's
# tests/fixtures/<SITE_FLEET_VECTORS>, the one path that test reads (its own
# FLEET_VECTORS constant) — fixed here rather than taken from the upstream
# name, so an upstream rename cannot land a copy the website never reads.
FLEET_VECTORS = REPO / "tvos" / "witness-core" / "tests" / "fixtures" / "fleet_contract_vectors.json"
SITE_FLEET_VECTORS = "fleet_contract_vectors.json"

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

# The /playground Test bench and the /modes page run on these three upstream
# files. Each was a hand copy on the website, headed HAND-COPIED, and lagged:
# every firmware version bump left playground-data.js's fw_version behind, and
# an upstream fix to a sim waited for someone to re-copy it. The site paths are
# the website's contract (its pages import them by these names), fixed here
# rather than derived from the upstream names.
PLAYGROUND_JSON = REPO / "canary-local" / "devices" / "playground.json"
PLAYGROUND_SIM = REPO / "canary-local" / "assets" / "playground-sim.js"
MODE_SIM = REPO / "canary-local" / "assets" / "mode-sim.js"
SITE_PLAYGROUND_DATA = "js/playground-data.js"
SITE_PLAYGROUND_SIM = "js/playground-sim.js"
SITE_MODE_SIM = "js/mode-sim.js"

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
# 3. tv/vendor/verify_core.js + fixtures + PROVENANCE.txt <- viewer/, tests/fixtures/,
#    and the fleet contract vectors <- tvos/witness-core/tests/fixtures/
# --------------------------------------------------------------------------- #

def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def provenance_text() -> str:
    # Each pin is "  <name>  sha256:<hex>", and the website's tv-wall.test.mjs
    # resolves <name> to tv/vendor/verify_core.js or tests/fixtures/<name>, so a
    # carried fixture's name here is its path under the site's tests/fixtures/.
    lines = [
        "Vendored from the securaCV repo — the canonical evidence-envelope verifier",
        "and the fleet contract vectors the Wall's parseFleet replays.",
        "Generated by scripts/carry_to_site.py — DO NOT EDIT verify_core.js or the",
        "carried fixtures by hand; change them in securaCV (viewer/verify_core.js,",
        "tests/fixtures/, tvos/witness-core/tests/fixtures/), then re-run the",
        "carry. The website's tests/tv-wall.test.mjs verifies BEHAVIOR against",
        "these fixtures (a stronger guarantee than a byte-compare).",
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
    lines += [
        "",
        "upstream fleet contract vectors: securaCV tvos/witness-core/tests/fixtures/",
        f"  {SITE_FLEET_VECTORS}  sha256:{_sha256(FLEET_VECTORS)}",
    ]
    return "\n".join(lines) + "\n"


def carry_verifier(site: Path) -> list[Path]:
    vendor = site / "tv" / "vendor"
    if not vendor.is_dir():
        fail(f"{site} does not look like the website checkout (no tv/vendor)")
    sources = [VERIFIER] + [FIXTURES / "envelope" / f for f in ENVELOPE_FIXTURES] \
        + [FIXTURES / "export_bundle" / f for f in BUNDLE_FIXTURES] + [FLEET_VECTORS]
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
    dest = site / "tests" / "fixtures" / SITE_FLEET_VECTORS
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(FLEET_VECTORS, dest)
    written.append(dest)
    dest = vendor / "PROVENANCE.txt"
    dest.write_text(provenance_text(), encoding="utf-8")
    written.append(dest)
    return written


# --------------------------------------------------------------------------- #
# 4. js/playground-data.js, js/playground-sim.js, js/mode-sim.js
#    <- canary-local/devices/playground.json, canary-local/assets/
# --------------------------------------------------------------------------- #

# Each header is a comment block the website's tests/carry-claims.test.mjs
# reads in a carried file's first lines: it must say CARRIED, name the upstream
# file and name this tool. No date, no sha: byte-reproducible like the rest.
PLAYGROUND_DATA_HEADER = (
    "// js/playground-data.js — CARRIED from the securaCV repo by its\n"
    "//   scripts/carry_to_site.py (the `playground` carry): the object below is\n"
    "//   canary-local/devices/playground.json verbatim, generated there by\n"
    "//   canary-local/tools/gen_playground.py from the Waveshare 4.3B firmware\n"
    "//   pins.h + the playground driver, and drift-gated in CI there.\n"
    "// Do not edit it here: the website's weekly carry job rewrites it whenever\n"
    "// upstream moves (fw_version moves with every firmware release). Only the\n"
    "// terminal endpoints are claims; the board body + wire routing are\n"
    "// schematic stand-ins staged for legibility.\n"
    "\n"
)


def sim_header(site_path: str, upstream: str, gate: str, subject: str) -> str:
    return (
        f"// {site_path} — CARRIED VERBATIM from the securaCV repo by its\n"
        "//   scripts/carry_to_site.py (the `playground` carry): everything below\n"
        f"//   this header is {upstream}, byte for byte.\n"
        f"// The authoritative copy is drift-gated there by {gate}\n"
        f"// against {subject}. Do not edit it here: the\n"
        "// website's weekly carry job rewrites it whenever upstream moves.\n"
        "\n"
    )


def _shown(path: Path) -> str:
    """A path as an error message names it: repo-relative when it is in the repo."""
    try:
        return str(path.relative_to(REPO))
    except ValueError:
        return str(path)


def playground_data_js(json_text: str) -> str:
    """playground.json as the ES module the /playground page imports: the
    header, then `export const PLAYGROUND = ` and the JSON with its trailing
    newline replaced by `;\\n`. Refuses a file that is not one JSON object,
    so a broken upstream file stops the carry instead of shipping a module
    that throws on import."""
    try:
        doc = json.loads(json_text)
    except json.JSONDecodeError as e:
        fail(f"{_shown(PLAYGROUND_JSON)} is not JSON ({e}) — not carrying it")
    if not isinstance(doc, dict):
        fail(f"{_shown(PLAYGROUND_JSON)} is not a JSON object — not carrying it")
    body = json_text[:-1] if json_text.endswith("\n") else json_text
    return PLAYGROUND_DATA_HEADER + "export const PLAYGROUND = " + body + ";\n"


def playground_outputs() -> dict[str, str]:
    """Site path -> the exact text the playground carry writes there."""
    for src in (PLAYGROUND_JSON, PLAYGROUND_SIM, MODE_SIM):
        if not src.is_file():
            fail(f"missing {_shown(src)}")
    return {
        SITE_PLAYGROUND_DATA: playground_data_js(PLAYGROUND_JSON.read_text(encoding="utf-8")),
        SITE_PLAYGROUND_SIM: sim_header(
            SITE_PLAYGROUND_SIM, "canary-local/assets/playground-sim.js",
            "canary-local/tests/playground.test.js",
            "the Waveshare 4.3B playground firmware")
        + PLAYGROUND_SIM.read_text(encoding="utf-8"),
        SITE_MODE_SIM: sim_header(
            SITE_MODE_SIM, "canary-local/assets/mode-sim.js",
            "canary-local/tests/mode.test.js",
            "the canary-display mode firmware")
        + MODE_SIM.read_text(encoding="utf-8"),
    }


def carry_playground(site: Path) -> list[Path]:
    # Every source is read and checked before the first write, so a missing or
    # broken upstream file leaves the website's three files as they were.
    outputs = playground_outputs()
    written = []
    for rel, text in outputs.items():
        dest = site / rel
        dest.write_text(text, encoding="utf-8")
        written.append(dest)
    return written


# --------------------------------------------------------------------------- #
# 5. download.html's app-v / flasher-v tokens <- the newest published releases
# --------------------------------------------------------------------------- #

# The /download buttons open exact release pages (.../releases/tag/app-v<x.y.z>
# for the Lab, .../releases/tag/flasher-v<x.y.z> for the Flasher) and the note
# under the cards names the same tags; the website's
# tests/download-facts.test.mjs holds every token of a family to one version.
# The truth is the newest PUBLISHED release of each family, never
# tauri.conf.json: a version bump merges before its release exists (a Lab
# release is a draft until someone publishes it), and a stamp from the config
# would point the buttons at a 404 until then.
RELEASES_API = "https://api.github.com/repos/kmay89/securaCV/releases"
SITE_DOWNLOAD = "download.html"
DOWNLOAD_FAMILIES = {"app-v": "the Lab", "flasher-v": "the Flasher"}
SEMVER = r"[0-9]+\.[0-9]+\.[0-9]+"
API_ATTEMPTS = 3
API_PAGE_LIMIT = 20


class ReleasesUnavailable(Exception):
    """The API cannot say which release of a family is the newest published one."""


def _token_re(prefix: str) -> re.Pattern:
    # The website test's own pattern (`\b${prefix}(${SEMVER})\b`, ASCII like
    # JavaScript's), so the stamp moves exactly the tokens that test counts.
    return re.compile(rf"\b{re.escape(prefix)}({SEMVER})\b", re.ASCII)


def _api_page(url: str, token: str | None) -> tuple[list, str | None]:
    """One page of the release list and the next page's URL. Retries a network
    error, a 5xx and a 429; any other HTTP error is the answer."""
    req = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        **({"Authorization": f"Bearer {token}"} if token else {}),
    })
    why = ""
    for attempt in range(API_ATTEMPTS):
        if attempt:
            time.sleep(2 ** attempt)
        try:
            with urllib.request.urlopen(req, timeout=30) as res:
                page = json.loads(res.read())
                link = res.headers.get("Link") or ""
        except urllib.error.HTTPError as e:
            why = f"HTTP {e.code} from {url}"
            if e.code < 500 and e.code != 429:
                raise ReleasesUnavailable(why) from None
            continue
        except (urllib.error.URLError, TimeoutError, OSError, ValueError) as e:
            why = f"{url}: {e}"
            continue
        if not isinstance(page, list):
            raise ReleasesUnavailable(f"{url} did not answer with a list of releases")
        nxt = re.search(r'<([^>]+)>;\s*rel="next"', link)
        return page, nxt.group(1) if nxt else None
    raise ReleasesUnavailable(f"{why} ({API_ATTEMPTS} attempts)")


def read_releases(token: str | None) -> list[dict]:
    """Every release of this repo, drafts included (the filter is
    newest_published's), following the API's pages to the end."""
    url: str | None = RELEASES_API + "?per_page=100"
    releases: list[dict] = []
    for _ in range(API_PAGE_LIMIT):
        page, url = _api_page(url, token)
        releases += page
        if url is None:
            return releases
    raise ReleasesUnavailable(f"the release list ran past {API_PAGE_LIMIT} pages")


def newest_published(releases: list[dict], prefix: str) -> str | None:
    """The highest x.y.z among the family's published releases, or None. A
    release counts only when the API says so outright: draft and prerelease
    both false and a publish date. A tag with anything after x.y.z (an -rc)
    is not a stable release and would stamp a token the page cannot hold."""
    tag = re.compile(re.escape(prefix) + f"({SEMVER})", re.ASCII)
    best = None
    for r in releases:
        m = tag.fullmatch(str(r.get("tag_name") or ""))
        if not m or r.get("draft") is not False or r.get("prerelease") is not False \
                or not r.get("published_at"):
            continue
        version = tuple(int(n) for n in m.group(1).split("."))
        if best is None or version > best:
            best = version
    return ".".join(map(str, best)) if best else None


def stamp_downloads(html: str, newest: dict[str, str]) -> str:
    """Every app-v / flasher-v token at its family's newest published version;
    every other byte as it was."""
    for prefix, version in newest.items():
        html = _token_re(prefix).sub(prefix + version, html)
    return html


def stamped_download_page(site: Path) -> tuple[Path, str, str, dict[str, str]]:
    """download.html, its text, the stamped text and the versions stamped.
    Raises ReleasesUnavailable when the API cannot name a family's newest
    published release; a page without a family's token stops the carry."""
    path = site / SITE_DOWNLOAD
    if not path.is_file():
        fail(f"{path} is missing — this carry stamps release tags INTO the website's "
             "page, it does not author it")
    html = path.read_text(encoding="utf-8")
    for prefix in DOWNLOAD_FAMILIES:
        if not _token_re(prefix).search(html):
            fail(f"{SITE_DOWNLOAD} names no {prefix}<x.y.z> — the page changed shape, "
                 "and a stamp with nothing to move would say it is current")
    token = os.environ.get("GH_TOKEN") or None
    try:
        releases = read_releases(token)
    except ReleasesUnavailable as e:
        hint = "" if token else "; GH_TOKEN is not set"
        raise ReleasesUnavailable(f"the release list could not be read ({e}{hint})") from None
    newest = {}
    for prefix, app in DOWNLOAD_FAMILIES.items():
        newest[prefix] = newest_published(releases, prefix)
        if newest[prefix] is None:
            raise ReleasesUnavailable(f"the API shows no published {prefix}<x.y.z> "
                                      f"release of {app}")
    return path, html, stamp_downloads(html, newest), newest


def carry_downloads(site: Path) -> list[Path]:
    try:
        path, _html, stamped, _newest = stamped_download_page(site)
    except ReleasesUnavailable as e:
        print(f"carry_to_site.py: downloads: {SITE_DOWNLOAD} left as it is — {e}",
              file=sys.stderr)
        return []
    path.write_text(stamped, encoding="utf-8")
    return [path]


def check_downloads(site: Path) -> int:
    """0 when download.html names the newest published releases, 1 when not."""
    try:
        _path, html, stamped, newest = stamped_download_page(site)
    except ReleasesUnavailable as e:
        fail(f"downloads: cannot check {SITE_DOWNLOAD} — {e}")
    if stamped == html:
        print(f"{SITE_DOWNLOAD} names the newest published releases: "
              + ", ".join(p + v for p, v in newest.items()))
        return 0
    for prefix, version in newest.items():
        named = sorted(set(_token_re(prefix).findall(html)) - {version})
        if named:
            print(f"{SITE_DOWNLOAD} is stale: it names {', '.join(prefix + n for n in named)}; "
                  f"the newest published release of {DOWNLOAD_FAMILIES[prefix]} is "
                  f"{prefix}{version}")
    print("refresh it: python3 scripts/carry_to_site.py --site <website checkout> "
          "--only downloads")
    return 1


# --------------------------------------------------------------------------- #

CARRIES = {
    "builds": carry_builds,
    "kernel-status": carry_kernel_status,
    "verifier": carry_verifier,
    "playground": carry_playground,
    "downloads": carry_downloads,
}


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--site", metavar="DIR", type=Path, required=True,
                    help="the website checkout root (kmay89/securacv_website)")
    ap.add_argument("--only", metavar="CARRY", action="append", choices=sorted(CARRIES),
                    help="refresh only this carry (repeatable); default: all of them")
    ap.add_argument("--check", action="store_true",
                    help="with --only downloads: write nothing, exit 1 unless download.html "
                         "names the newest published releases")
    args = ap.parse_args(argv)
    if args.check and set(args.only or ()) != {"downloads"}:
        ap.error("--check is the downloads carry's: pass --only downloads (the weekly "
                 "job's diff is the other carries' check)")

    site = args.site.resolve()
    if not (site / "js").is_dir() or not (site / "tests").is_dir():
        fail(f"{site} does not look like the website checkout (no js/ and tests/)")
    if args.check:
        sys.exit(check_downloads(site))
    for name in args.only or list(CARRIES):
        for path in CARRIES[name](site):
            print(f"wrote {path.relative_to(site)}")


if __name__ == "__main__":
    main()
