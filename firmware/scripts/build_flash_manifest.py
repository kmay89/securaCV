#!/usr/bin/env python3
"""Build the browser-flasher factory images + manifest-flash.json for a release.

Shared by both release paths so they can't drift:
  - .github/workflows/firmware-release.yml — on a fw-v* tag, alongside the
    signed OTA artifacts (one build, no duplication).
  - .github/workflows/flasher-release.yml  — a manual (workflow_dispatch) run
    that rebuilds the flasher assets for an existing tag, independent of the
    OTA signing key (the browser channel's integrity is SHA-256 + same-origin,
    not the Ed25519 release key — see docs/browser_flasher.md § Trust model).

Product ids, chips, and asset stems come from canary-local/devices/flash.json —
the same drift-gated catalog the flasher page trusts for its chip guard — so
this script never re-states them. All it adds is where each variant's compiled
outputs land and its version suffix. Factory images are merged by
make_factory.py (which reads the app offset from each variant's own partition
table); the manifest records every image's SHA-256 for the page to verify
before writing.

    python firmware/scripts/build_flash_manifest.py \
        --version 2.3.0 --repo kmay89/securaCV --tag fw-v2.3.0 \
        --out-dir /tmp/release [--notes "release notes"]

Per-variant build failures are warnings, not errors: that product is simply
left out of the manifest and shows as "unavailable" in the flasher.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
FLASH_JSON = REPO_ROOT / "canary-local/devices/flash.json"
MAKE_FACTORY = REPO_ROOT / "firmware/scripts/make_factory.py"

# product id → where its compiled outputs land + its version suffix. Chips and
# asset stems are NOT here — they come from flash.json. `dir` is repo-relative
# for PlatformIO builds and absolute for the arduino-cli output.
BUILD = {
    "securacv-canary":                 {"toolchain": "pio", "dir": "firmware/canary/.pio/build/release_ha", "vsuffix": ""},
    "securacv-canary-wap":             {"toolchain": "arduino", "dir": "/tmp/wap-build", "vsuffix": "-wap"},
    "securacv-canary-vision":          {"toolchain": "pio", "dir": "firmware/projects/canary-vision/.pio/build/canary-vision-default", "vsuffix": ""},
    "securacv-canary-vision-xiao-c3":  {"toolchain": "pio", "dir": "firmware/projects/canary-vision/.pio/build/canary-vision-xiao-c3", "vsuffix": ""},
    "securacv-canary-vision-xiao-s3":  {"toolchain": "pio", "dir": "firmware/projects/canary-vision/.pio/build/canary-vision-xiao-s3", "vsuffix": ""},
    "securacv-canary-sense":           {"toolchain": "pio", "dir": "firmware/projects/canary-sense/.pio/build/canary-sense-default", "vsuffix": ""},
    "securacv-canary-sense-wellbeing": {"toolchain": "pio", "dir": "firmware/projects/canary-sense/.pio/build/canary-sense-wellbeing", "vsuffix": ""},
}


def esptool_chip(chip: str) -> str:
    """flash.json's "ESP32-S3" → esptool's "esp32s3"."""
    return chip.lower().replace("-", "")


def resolve_dir(d: str) -> pathlib.Path:
    p = pathlib.Path(d)
    return p if p.is_absolute() else (REPO_ROOT / p)


def make_factory_cmd(product: dict, build: dict, out: pathlib.Path) -> list[str]:
    cmd = ["python", str(MAKE_FACTORY), "--chip", esptool_chip(product["chip"]), "--out", str(out)]
    d = resolve_dir(build["dir"])
    if build["toolchain"] == "pio":
        cmd += ["--build-dir", str(d)]
    else:  # arduino-cli names its parts <sketch>.ino.*
        cmd += ["--bootloader", str(d / "canary_wap.ino.bootloader.bin"),
                "--partitions", str(d / "canary_wap.ino.partitions.bin"),
                "--app", str(d / "canary_wap.ino.bin")]
    return cmd


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--version", required=True, help="release version, e.g. 2.3.0 (no fw-v prefix)")
    ap.add_argument("--repo", required=True, help="owner/name, e.g. kmay89/securaCV")
    ap.add_argument("--tag", required=True, help="the release tag, e.g. fw-v2.3.0")
    ap.add_argument("--out-dir", required=True, type=pathlib.Path, help="where to write the .bin + manifest")
    ap.add_argument("--notes", default="", help="release notes (optional; the flasher doesn't display them)")
    args = ap.parse_args()

    catalog = json.loads(FLASH_JSON.read_text(encoding="utf-8"))
    out_dir = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    dl = f"https://github.com/{args.repo}/releases/download/{args.tag}"
    rel_url = f"https://github.com/{args.repo}/releases/tag/{args.tag}"
    notes = args.notes.strip() or f"SecuraCV Canary firmware {args.version}."

    products: dict[str, dict] = {}
    for p in catalog["products"]:
        pid = p["id"]
        build = BUILD.get(pid)
        if not build:
            print(f"::warning::no build mapping for {pid} in build_flash_manifest.py; skipping")
            continue
        out = out_dir / f"{p['asset_stem']}-{args.version}-factory.bin"
        try:
            subprocess.run(make_factory_cmd(p, build, out), check=True)
            data = out.read_bytes()
            products[pid] = {
                "version": args.version + build["vsuffix"],
                "chipFamily": p["chip"],
                "factory": f"{dl}/{out.name}",
                "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data),
                "release_notes": notes,
            }
        except Exception as e:  # noqa: BLE001 — a bad variant must not sink the rest
            print(f"::warning::browser-flasher factory image for {pid} not built ({e}); "
                  f"it will show as unavailable in the in-browser flasher.")

    manifest = {
        "schema": "securacv-flash-1",
        "fw_train": args.version,
        "repo": args.repo,
        "release_url": rel_url,
        "products": products,
    }
    (out_dir / "manifest-flash.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"browser flasher: {len(products)}/{len(catalog['products'])} factory images, "
          f"manifest-flash.json written to {out_dir}")
    if not products:
        print("::warning::no factory images were produced — is this running after the builds?")


if __name__ == "__main__":
    main()
