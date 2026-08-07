#!/usr/bin/env python3
"""Merge a PlatformIO/Arduino build into a single flashable *factory* image.

The over-the-air OTA channel ships app-only images (`firmware.bin`) that the
running bootloader installs into a spare slot. The browser flasher
(canary-local/flash.html) instead writes a *blank* board over USB, so it needs
the whole thing in one blob flashed at offset 0: the second-stage bootloader,
the partition table, and the app.

This tool builds that blob with esptool's `merge_bin`, reading the app's flash
offset straight out of the compiled partition table (`partitions.bin`) so it's
correct for whatever layout a variant uses — no hard-coded offsets to drift.

    python firmware/scripts/make_factory.py \
        --chip esp32s3 \
        --build-dir firmware/projects/canary-wap/.pio/build/canary-wap-default \
        --out canary-wap-2.2.0-factory.bin

Inputs found in --build-dir: bootloader.bin, partitions.bin, firmware.bin.
On these chips (ESP32-S3/C3/C6) the bootloader sits at offset 0; classic
ESP32 (0x1000) can be selected with --bootloader-offset.

Note: a factory image spans 0..app-end, so flashing it re-initializes NVS
(saved WiFi/settings) — which is exactly SecuraCV's first-flash model
(docs/firmware_ota.md: a USB flash seeds NVS; every later OTA inherits it).
"""
from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

PARTITION_MAGIC = 0x50AA          # bytes 0xAA,0x50
APP_TYPE = 0x00
APP_SUBTYPE_NAME = {0x00: "factory", 0x10: "ota_0", 0x11: "ota_1"}
PARTITION_TABLE_OFFSET = 0x8000   # standard on all ESP32 variants


def parse_app_offset(partitions_bin: bytes) -> tuple[int, int, str]:
    """Return (offset, size, label) of the app partition to flash the app into.

    Prefers factory, then ota_0/app0, then the first app partition — the same
    order the browser reader uses to decide which slot to read a version from.
    """
    apps = []
    for o in range(0, len(partitions_bin) - 32 + 1, 32):
        (magic,) = struct.unpack_from("<H", partitions_bin, o)
        if magic != PARTITION_MAGIC:
            break  # 0xFFFF padding or the MD5 marker → table ended
        ptype = partitions_bin[o + 2]
        subtype = partitions_bin[o + 3]
        (offset, size) = struct.unpack_from("<II", partitions_bin, o + 4)
        label = partitions_bin[o + 12:o + 28].split(b"\x00")[0].decode("ascii", "replace")
        if ptype == APP_TYPE:
            apps.append((subtype, offset, size, label))
    if not apps:
        raise SystemExit("make_factory: no app partition found in partitions.bin")
    for want in (0x00, 0x10):  # factory, then ota_0
        for subtype, offset, size, label in apps:
            if subtype == want:
                return offset, size, label or APP_SUBTYPE_NAME.get(want, "app")
    subtype, offset, size, label = apps[0]
    return offset, size, label or "app"


def main() -> None:
    ap = argparse.ArgumentParser(description="Merge a build into a single factory image.")
    ap.add_argument("--chip", required=True, help="esptool chip id, e.g. esp32s3 / esp32c3 / esp32c6")
    ap.add_argument("--build-dir", type=Path,
                    help="PlatformIO output dir holding bootloader.bin, partitions.bin, firmware.bin "
                         "(PlatformIO naming). Use the explicit part flags for arduino-cli output.")
    ap.add_argument("--bootloader", type=Path, help="explicit bootloader .bin (overrides --build-dir)")
    ap.add_argument("--partitions", type=Path, help="explicit partition-table .bin (overrides --build-dir)")
    ap.add_argument("--app", type=Path, help="explicit app .bin (overrides --build-dir)")
    ap.add_argument("--out", required=True, type=Path, help="output factory .bin")
    ap.add_argument("--bootloader-offset", default="0x0",
                    help="second-stage bootloader offset (0x0 for S3/C3/C6, 0x1000 for classic ESP32)")
    ap.add_argument("--flash-size", default="keep", help="esptool --flash_size (default: keep)")
    ap.add_argument("--app-offset", default=None,
                    help="override the app offset (default: read from partitions.bin)")
    ap.add_argument("--dry-run", action="store_true", help="print the esptool command and exit")
    args = ap.parse_args()

    if args.bootloader and args.partitions and args.app:
        bootloader, partitions, app = args.bootloader, args.partitions, args.app
    elif args.build_dir:
        bd = args.build_dir
        bootloader = bd / "bootloader.bin"
        partitions = bd / "partitions.bin"
        app = bd / "firmware.bin"
    else:
        raise SystemExit("make_factory: give --build-dir, or all of --bootloader/--partitions/--app")
    for f in (bootloader, partitions, app):
        if not f.exists():
            raise SystemExit(f"make_factory: missing {f} — is the build complete?")

    if args.app_offset is not None:
        app_off = int(args.app_offset, 0)
        app_label = "override"
    else:
        app_off, app_size, app_label = parse_app_offset(partitions.read_bytes())
        # The slot the bootloader will read from must actually hold the app.
        # fw-v2.4.6 shipped a nightstand-c6 factory image whose app was 5 KB
        # bigger than its 0x1E0000 slot — the flasher wrote it faithfully and
        # every board it touched boot-looped on "Image length … doesn't fit in
        # partition length …". A factory image that cannot boot must never be
        # merged, let alone published; failing here drops just this variant
        # (build_flash_manifest.py treats it as a per-variant hiccup).
        actual = app.stat().st_size
        if actual > app_size:
            raise SystemExit(
                f"make_factory: {app} is {actual} bytes but the {app_label} slot at "
                f"{hex(app_off)} holds only {app_size} — this image could never boot. "
                f"Trim features or grow the partition table (firmware/PARTITIONS.md).")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable, "-m", "esptool", "--chip", args.chip, "merge_bin",
        "-o", str(args.out),
        "--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", args.flash_size,
        args.bootloader_offset, str(bootloader),
        hex(PARTITION_TABLE_OFFSET), str(partitions),
        hex(app_off), str(app),
    ]
    print(f"make_factory: app → {app_label} @ {hex(app_off)}; running:\n  " + " ".join(cmd))
    if args.dry_run:
        return
    subprocess.run(cmd, check=True)

    size = args.out.stat().st_size
    if size <= 0:
        raise SystemExit("make_factory: produced an empty file")
    print(f"make_factory: wrote {args.out} ({size} bytes)")


if __name__ == "__main__":
    main()
