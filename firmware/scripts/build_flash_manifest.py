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
import re
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
    "securacv-canary-wap":             {"toolchain": "arduino", "dir": "/tmp/wap-build", "sketch": "canary_wap", "vsuffix": "-wap"},
    # The Phase 0 reach ports (docs/strategy/30). USB-flashable from here on;
    # their signed OTA channels stay closed until a Hardware Test Report
    # promotes the board off compile-tested (check_ota_channels.py UNPUBLISHED).
    # Those are separate artifacts — manifest-flash.json is the factory image
    # a person installs deliberately, manifest-canary-*.json is the update a
    # device takes on its own — so reach and self-update move independently.
    "securacv-canary-esp32cam":        {"toolchain": "pio", "dir": "firmware/canary/.pio/build/esp32cam", "vsuffix": ""},
    "securacv-canary-wroom":           {"toolchain": "pio", "dir": "firmware/canary/.pio/build/esp32-wroom", "vsuffix": ""},
    "securacv-canary-freenove-s3":     {"toolchain": "pio", "dir": "firmware/canary/.pio/build/freenove-s3", "vsuffix": ""},
    "securacv-canary-vision-c3-super-mini": {"toolchain": "pio", "dir": "firmware/projects/canary-vision/.pio/build/canary-vision-c3-super-mini", "vsuffix": ""},
    "securacv-canary-vision":          {"toolchain": "pio", "dir": "firmware/projects/canary-vision/.pio/build/canary-vision-default", "vsuffix": ""},
    "securacv-canary-vision-xiao-c3":  {"toolchain": "pio", "dir": "firmware/projects/canary-vision/.pio/build/canary-vision-xiao-c3", "vsuffix": ""},
    "securacv-canary-vision-xiao-s3":  {"toolchain": "pio", "dir": "firmware/projects/canary-vision/.pio/build/canary-vision-xiao-s3", "vsuffix": ""},
    "securacv-canary-sense":           {"toolchain": "pio", "dir": "firmware/projects/canary-sense/.pio/build/canary-sense-default", "vsuffix": ""},
    "securacv-canary-sense-wellbeing": {"toolchain": "pio", "dir": "firmware/projects/canary-sense/.pio/build/canary-sense-wellbeing", "vsuffix": ""},
    # The display flavors: arduino-cli --profile builds (sketch.yaml pins the
    # platform + libs), exported by firmware-release.yml to these dirs. A
    # missing dir just drops the product from the flasher — same per-variant
    # resilience as everything else here.
    "securacv-canary-display-watch":      {"toolchain": "arduino", "dir": "/tmp/display-watch-build", "sketch": "canary_display", "vsuffix": ""},
    "securacv-canary-display-dash":       {"toolchain": "arduino", "dir": "/tmp/display-dash-build", "sketch": "canary_display", "vsuffix": ""},
    "securacv-canary-display-dash-modes": {"toolchain": "arduino", "dir": "/tmp/display-modes-build", "sketch": "canary_display", "vsuffix": ""},
    # The Nightstand Line boards release from their PlatformIO envs (the same
    # envs firmware.yml compiles on every push) — board pins and flash
    # geometry live in the env, so there is no sketch profile to stage.
    "securacv-canary-display-dash7":         {"toolchain": "pio", "dir": "firmware/projects/canary-display/.pio/build/canary-display-dash7", "vsuffix": ""},
    "securacv-canary-display-nightstand7":   {"toolchain": "pio", "dir": "firmware/projects/canary-display/.pio/build/canary-display-nightstand7", "vsuffix": ""},
    "securacv-canary-display-nightstand-s3": {"toolchain": "pio", "dir": "firmware/projects/canary-display/.pio/build/canary-display-nightstand-s3", "vsuffix": ""},
    "securacv-canary-display-nightstand-c6": {"toolchain": "pio", "dir": "firmware/projects/canary-display/.pio/build/canary-display-nightstand-c6", "vsuffix": ""},
    "securacv-canary-display-touch169":      {"toolchain": "pio", "dir": "firmware/projects/canary-display/.pio/build/canary-display-touch169", "vsuffix": ""},
    "securacv-canary-display-amoled241":     {"toolchain": "pio", "dir": "firmware/projects/canary-display/.pio/build/canary-display-amoled241", "vsuffix": ""},
    "securacv-canary-display-nightlight-c3": {"toolchain": "pio", "dir": "firmware/projects/canary-display/.pio/build/canary-display-nightlight-c3", "vsuffix": ""},
}


def esptool_chip(chip: str) -> str:
    """flash.json's "ESP32-S3" → esptool's "esp32s3"."""
    return chip.lower().replace("-", "")


def resolve_dir(d: str) -> pathlib.Path:
    p = pathlib.Path(d)
    return p if p.is_absolute() else (REPO_ROOT / p)


def image_has_version(path: pathlib.Path, expected: str) -> bool:
    """Does the built image actually carry the version we're about to advertise?

    Mirrors firmware-release.yml's guard: it greps the binary for the compiled
    version literal (`strings | grep -Fx`). We do the same by extracting
    NUL/whitespace-delimited printable runs and requiring a whole-run match, so
    the manual pre-key path can't publish a manifest whose version disagrees
    with the firmware a board would actually boot as.
    """
    data = path.read_bytes()
    target = expected.encode()
    return any(run == target for run in re.split(rb"[^\x20-\x7e]+", data))


def make_factory_cmd(product: dict, build: dict, out: pathlib.Path) -> list[str]:
    cmd = ["python", str(MAKE_FACTORY), "--chip", esptool_chip(product["chip"]), "--out", str(out)]
    d = resolve_dir(build["dir"])
    if build["toolchain"] == "pio":
        cmd += ["--build-dir", str(d)]
    else:  # arduino-cli names its parts <sketch>.ino.*
        sketch = build.get("sketch", "canary_wap")
        cmd += ["--bootloader", str(d / f"{sketch}.ino.bootloader.bin"),
                "--partitions", str(d / f"{sketch}.ino.partitions.bin"),
                "--app", str(d / f"{sketch}.ino.bin")]
    return cmd


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--version", required=True, help="release version, e.g. 2.3.0 (no fw-v prefix)")
    ap.add_argument("--repo", required=True, help="owner/name, e.g. kmay89/securaCV")
    ap.add_argument("--tag", required=True, help="the release tag, e.g. fw-v2.3.0")
    ap.add_argument("--out-dir", required=True, type=pathlib.Path, help="where to write the .bin + manifest")
    ap.add_argument("--notes", default="", help="release notes (optional; the flasher doesn't display them)")
    ap.add_argument("--signing-key", type=pathlib.Path, default=None,
                    help="Ed25519 release private-key PEM. When given, each factory image is "
                         "signed (same scheme as ota_release.py) so the flasher verifies the "
                         "signature in-browser. Omit for the pre-key path (checksum-only).")
    args = ap.parse_args()

    catalog = json.loads(FLASH_JSON.read_text(encoding="utf-8"))
    out_dir = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    dl = f"https://github.com/{args.repo}/releases/download/{args.tag}"
    rel_url = f"https://github.com/{args.repo}/releases/tag/{args.tag}"
    notes = args.notes.strip() or f"SecuraCV Canary firmware {args.version}."

    # Load the release signer if a key was supplied — reuse ota_release.py so the
    # signature scheme (uint32_le(size) || sha256, Ed25519) can never drift from
    # the OTA path or the device verifier.
    signer = None
    if args.signing_key and args.signing_key.exists():
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
        import ota_release  # noqa: E402 — local sibling tool
        priv = ota_release.load_private_key(args.signing_key)
        signer = (ota_release, priv)
        print(f"build_flash_manifest: signing factory images (key id "
              f"{ota_release.signing_key_id(priv.public_key())}).")
    else:
        print("build_flash_manifest: no signing key — factory images are checksum-only "
              "(the flasher verifies SHA-256 and shows the build as unsigned).")

    products: dict[str, dict] = {}
    version_errors: list[str] = []
    for p in catalog["products"]:
        pid = p["id"]
        build = BUILD.get(pid)
        if not build:
            print(f"::warning::no build mapping for {pid} in build_flash_manifest.py; skipping")
            continue
        out = out_dir / f"{p['asset_stem']}-{args.version}-factory.bin"
        expected_version = args.version + build["vsuffix"]
        # A missing/incomplete build is a per-variant hiccup (skip it); a
        # version mismatch is a release-hygiene error affecting the whole tag
        # (abort — see the guard below).
        try:
            subprocess.run(make_factory_cmd(p, build, out), check=True)
        except Exception as e:  # noqa: BLE001 — a bad variant must not sink the rest
            print(f"::warning::browser-flasher factory image for {pid} not built ({e}); "
                  f"it will show as unavailable in the in-browser flasher.")
            continue
        if not image_has_version(out, expected_version):
            version_errors.append(
                f"{pid}: factory image does not contain the compiled version string "
                f"'{expected_version}'. Bump the variant's FIRMWARE_VERSION / "
                f"CANARY_FW_VERSION to match before tagging {args.tag}.")
            continue
        data = out.read_bytes()
        rec = {
            "version": expected_version,
            "chipFamily": p["chip"],
            "factory": f"{dl}/{out.name}",
            "sha256": hashlib.sha256(data).hexdigest(),
            "size": len(data),
            "release_notes": notes,
        }
        if signer:
            ota, priv = signer
            rec["signature"] = ota.sign_firmware(priv, data).hex()
            rec["signing_key_id"] = ota.signing_key_id(priv.public_key())
        products[pid] = rec

    # Refuse to publish a manifest that would advertise a version the firmware
    # doesn't report — the check firmware-release.yml runs before its OTA
    # artifacts, now shared so the manual pre-key path can't skip it.
    if version_errors:
        for e in version_errors:
            print("::error::" + e)
        sys.exit("build_flash_manifest: version mismatch — refusing to publish "
                 "flasher assets that misstate the firmware version.")

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
