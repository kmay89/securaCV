#!/usr/bin/env python3
"""Pin firmware/build_matrix.json to the real feature flags so it can't rot.

build_matrix.json is the single source of truth the securacv_website /checkup
firmware-type selector mirrors: "which build includes which feature". This lint
proves the load-bearing cells against the ONLY files that actually decide them —
firmware/canary/platformio.ini (per-env -DFEATURE_* lines) and
firmware/canary/include/canary_config.h (the #ifndef defaults) — plus internal
consistency against flavors.json + boards.json. It is a cheap grep-style check
(no PlatformIO), run in .github/workflows/lint.yml.

Exit non-zero on any drift, printing every problem.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware"

errors = []


def err(msg):
    errors.append(msg)


def load_json(path):
    try:
        return json.loads(path.read_text())
    except Exception as e:  # noqa: BLE001
        err(f"could not read/parse {path.relative_to(ROOT)}: {e}")
        return None


def env_sections(ini_text):
    """Split a platformio.ini into {section_name: body_text}. The base table is
    keyed 'env'; environments are keyed by their full '[env:NAME]' minus brackets."""
    sections = {}
    current = None
    buf = []
    for line in ini_text.splitlines():
        m = re.match(r"^\[([^\]]+)\]\s*$", line)
        if m:
            if current is not None:
                sections[current] = "\n".join(buf)
            current = m.group(1)
            buf = []
        elif current is not None:
            buf.append(line)
    if current is not None:
        sections[current] = "\n".join(buf)
    return sections


def flag_set(body, flag):
    """Return the int value FLAG is set to in this section body, or None if the
    section doesn't set it. Matches `-DFEATURE_X=1` (ignoring trailing comments)."""
    m = re.search(rf"-D{re.escape(flag)}=(\d+)", body)
    return int(m.group(1)) if m else None


def main():
    matrix = load_json(FW / "build_matrix.json")
    flavors = load_json(FW / "flavors.json")
    boards = load_json(FW / "boards" / "boards.json")
    flash_catalog = load_json(ROOT / "canary-local" / "devices" / "flash.json")
    ini_path = FW / "canary" / "platformio.ini"
    cfg_path = FW / "canary" / "include" / "canary_config.h"
    if matrix is None or flavors is None or boards is None:
        _finish()
    ini = ini_path.read_text()
    cfg = cfg_path.read_text()
    sections = env_sections(ini)

    catalog = matrix.get("featureCatalog", {})
    board_ids = {b["id"] for b in boards}
    flavor_names = {f["name"] for f in flavors}

    # ── 1. platformio.ini ground truth: BLE is [env:full]-only ────────────────
    base = sections.get("env", "")
    full = sections.get("env:full", "")
    dev = sections.get("env:dev", "")
    release = sections.get("env:release", "")

    if flag_set(base, "FEATURE_BLE_STATUS") != 0:
        err("platformio.ini base [env] must set -DFEATURE_BLE_STATUS=0 "
            "(the whole BLE-only-in-full invariant rests on this)")
    if flag_set(full, "FEATURE_BLE_STATUS") != 1:
        err("[env:full] must set -DFEATURE_BLE_STATUS=1")
    if flag_set(full, "FEATURE_MESH_NETWORK") != 1:
        err("[env:full] must set -DFEATURE_MESH_NETWORK=1")
    for name, body in (("env:dev", dev), ("env:release", release)):
        if flag_set(body, "FEATURE_BLE_STATUS") == 1:
            err(f"[{name}] must NOT enable BLE (it doesn't fit the OTA slots)")
        if flag_set(body, "FEATURE_MESH_NETWORK") not in (0, None):
            err(f"[{name}] must not enable mesh (full-only)")

    # FEATURE_TEST_CONSOLE must never ship: not enabled in ANY env, default off.
    if re.search(r"-DFEATURE_TEST_CONSOLE=1", ini):
        err("FEATURE_TEST_CONSOLE=1 found in platformio.ini — the demo/mutate "
            "test console must never be compiled into a shippable image")
    m = re.search(r"#\s*define\s+FEATURE_TEST_CONSOLE\s+(\d+)", cfg)
    if not m or m.group(1) != "0":
        err("canary_config.h FEATURE_TEST_CONSOLE default must be 0")
    m = re.search(r"#\s*define\s+FEATURE_DIAGNOSTICS\s+(\d+)", cfg)
    if not m or m.group(1) != "1":
        err("canary_config.h FEATURE_DIAGNOSTICS default must be 1 "
            "(self-test is expected in every build)")

    # ── 2. build_matrix.json internal + cross-file consistency ────────────────
    def check_features(where, feats):
        for f in feats:
            if f not in catalog:
                err(f"{where}: feature '{f}' not in featureCatalog")
        if "selftest" not in feats:
            err(f"{where}: every build must include 'selftest' "
                "(FEATURE_DIAGNOSTICS is always on)")

    canary = None
    for p in matrix.get("products", []):
        if p["board"] not in board_ids:
            err(f"product {p['id']}: board '{p['board']}' not in boards.json")
        # Every product must resolve to a real flavor. Most ARE one (id ==
        # flavor). A board-specialized product — one env of a flavor, built
        # for a board with a different feature posture, e.g. the classic-ESP32
        # reach ports — says so with `flavor` instead of inventing a flavor
        # that firmware/flavors.json would have to grow for no build reason.
        flavor_of = p.get("flavor", p["id"])
        if flavor_of not in flavor_names:
            err(f"product {p['id']}: flavor '{flavor_of}' is not in flavors.json")
        if p.get("flavor") and not p.get("build", {}).get("env"):
            err(f"product {p['id']}: declares flavor '{p['flavor']}' but no "
                "build.env — a board-specialized product must name the env it is")
        if p["id"] == "canary":
            canary = p
        if p.get("hasLevels"):
            for lvl, b in p.get("levels", {}).items():
                check_features(f"{p['id']}.{lvl}", b["features"])
        else:
            b = p.get("build", {})
            check_features(f"{p['id']}.build", b.get("features", []))
            # serial-only / non-S3 products can't carry BLE or mesh
            if p["flow"] in ("serial", "vision"):
                for banned in ("bluetooth", "mesh"):
                    if banned in b.get("features", []):
                        err(f"product {p['id']} (flow={p['flow']}) must not list "
                            f"'{banned}' — it's a serial/vision board with no BLE radio in use")

    # ── 2a-bis. a product must not advertise a feature its OWN env turns off ──
    # The matrix exists to answer "which build includes which feature", so a
    # tile claiming a feature the env compiles out is the exact lie it is for.
    # Checks only flags the env sets EXPLICITLY to 0 (no `extends` resolution),
    # which is sound in one direction: no false positives, and it catches the
    # real mistake — copying a feature list from the flagship onto a board
    # whose posture is deliberately smaller.
    for p in matrix.get("products", []):
        env_name = p.get("build", {}).get("env")
        if not env_name or p.get("hasLevels"):
            continue
        body = sections.get(f"env:{env_name}")
        if body is None:
            continue  # env lives in another project's ini; not ours to read
        # build_unflags lists `-DFEATURE_X=1` to REMOVE an inherited flag; only
        # the build_flags half states what this env compiles.
        flags_body = body.split("build_flags", 1)[1] if "build_flags" in body else ""
        for feat in p["build"].get("features", []):
            flag = catalog.get(feat, {}).get("flag")
            if flag and flag_set(flags_body, flag) == 0:
                err(f"product {p['id']}: lists '{feat}' but [env:{env_name}] "
                    f"sets -D{flag}=0 — the build does not have it")

    # ── 2b. flasher deep-link can't rot: every product must resolve to a real
    #        flasher catalog product (both files live in this repo). ───────────
    flasher = matrix.get("flasher", {})
    if not flasher.get("url"):
        err("build_matrix.json: flasher.url is required for the /checkup Flash button")
    prefix = flasher.get("productPrefix", "securacv-")
    if flash_catalog is not None:
        flash_ids = {p.get("id") for p in flash_catalog.get("products", [])}
        for p in matrix.get("products", []):
            want = prefix + p["id"]
            if want not in flash_ids:
                err(f"product {p['id']}: flasher catalog has no '{want}' "
                    f"(canary-local/devices/flash.json) — the Flash-in-browser "
                    f"deep-link would 404; add it or fix productPrefix")

    # ── 2c. the default recommendation must resolve to a real product/level ───
    by_id = {p["id"]: p for p in matrix.get("products", [])}
    rec = matrix.get("recommended", {})
    dft = rec.get("default", {})
    if not dft.get("product"):
        err("recommended.default.product is required (the pick when nothing is known)")
    elif dft["product"] not in by_id:
        err(f"recommended.default.product '{dft['product']}' is not a product")
    else:
        dp = by_id[dft["product"]]
        if dp.get("hasLevels"):
            if dft.get("level") not in (dp.get("levels") or {}):
                err(f"recommended.default.level '{dft.get('level')}' not a level of "
                    f"{dft['product']}")
    # forChip: each detected-chip pick must be a product whose mcu is that family.
    for chip, pid in (rec.get("forChip") or {}).items():
        if pid not in by_id:
            err(f"recommended.forChip[{chip}] '{pid}' is not a product")
        elif chip not in (by_id[pid].get("mcu") or ""):
            err(f"recommended.forChip[{chip}] points at {pid}, whose mcu "
                f"'{by_id[pid].get('mcu')}' isn't a {chip}")

    # ── 3. the matrix must agree with the ini on the BLE/mesh full-only cell ───
    if canary:
        lv = canary.get("levels", {})
        for lvl in ("dev", "release"):
            for banned in ("bluetooth", "mesh"):
                if banned in lv.get(lvl, {}).get("features", []):
                    err(f"canary.{lvl} lists '{banned}', but platformio.ini "
                        f"enables it only in [env:full]")
        for needed in ("bluetooth", "mesh"):
            if needed not in lv.get("full", {}).get("features", []):
                err(f"canary.full must list '{needed}' (it's on in [env:full])")

    _finish()


def _finish():
    if errors:
        print("build_matrix.json lint FAILED:", file=sys.stderr)
        for e in errors:
            print(f"  ✗ {e}", file=sys.stderr)
        sys.exit(1)
    print("build_matrix.json lint OK — matrix matches platformio.ini + config.h")
    sys.exit(0)


if __name__ == "__main__":
    main()
