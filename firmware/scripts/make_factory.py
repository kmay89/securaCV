#!/usr/bin/env python3
"""Merge a PlatformIO/Arduino build into a single flashable *factory* image.

The over-the-air OTA channel ships app-only images (`firmware.bin`) that the
running bootloader installs into a spare slot. The browser flasher
(canary-local/flash.html) instead writes a *blank* board over USB, so it needs
the whole thing in one blob flashed at offset 0: the second-stage bootloader,
the partition table, and the app, each at its own flash address inside it.

This tool builds that blob with esptool's `merge_bin`, reading the app's flash
offset straight out of the compiled partition table (`partitions.bin`) so it's
correct for whatever layout a variant uses — no hard-coded offsets to drift.

    python firmware/scripts/make_factory.py \
        --chip esp32s3 \
        --build-dir firmware/projects/canary-wap/.pio/build/canary-wap-default \
        --out canary-wap-2.2.0-factory.bin

Inputs found in --build-dir: bootloader.bin, partitions.bin, firmware.bin.

Where the bootloader goes inside the image is the CHIP's answer, not ours: the
mask ROM loads the second stage from 0x1000 on a classic ESP32 and an ESP32-S2,
and from 0x0 on the S3/C3/C6 line. --chip picks it from ROM_BOOTLOADER below
(--chip auto reads the chip out of bootloader.bin's own image header), and the
bootloader's header must name the same chip or nothing is merged. An explicit
--bootloader-offset still overrides, with a warning when it disagrees with the
ROM. fw-v2.4.17 shipped its two classic-ESP32 images (ESP32-CAM, WROOM DevKit)
with the bootloader at 0x0, where their ROM never looks: the default was 0x0
for every chip and no caller passed the flag (firmware/LESSONS_LEARNED.md).

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
IMAGE_MAGIC = 0xE9                # first byte of every ESP app/bootloader image
IMAGE_CHIP_ID_AT = 12             # esp_image_header_t.chip_id (uint16 LE)

# esptool chip name → (where the ROM loads the second-stage bootloader from,
# the chip id that bootloader's image header carries). Both columns are
# esptool's own (BOOTLOADER_FLASH_OFFSET, IMAGE_CHIP_ID); test_make_factory.py
# holds this table to esptool's whenever esptool is installed. A chip that is
# not listed is refused unless --bootloader-offset names its offset: the P4 and
# the C5 load from 0x2000, so "0x0 unless told otherwise" is a guess, and a
# guess is how fw-v2.4.17's classic-ESP32 images got a bootloader at 0x0.
ROM_BOOTLOADER = {
    "esp32":   (0x1000, 0),
    "esp32s2": (0x1000, 2),
    "esp32s3": (0x0, 9),
    "esp32c3": (0x0, 5),
    "esp32c2": (0x0, 12),
    "esp32c6": (0x0, 13),
    "esp32h2": (0x0, 16),
}
CHIP_BY_IMAGE_ID = {cid: name for name, (_, cid) in ROM_BOOTLOADER.items()}


def image_chip_id(image: bytes, what: str) -> int:
    """The chip id an ESP image header names (bootloader or app), or exit.

    Every image esptool's elf2image writes opens with 0xE9 and carries the
    target chip at byte 12, so a bootloader built for one chip cannot be
    passed off as another's — the check the classic-ESP32 merge never had.
    """
    if len(image) < 24 or image[0] != IMAGE_MAGIC:
        raise SystemExit(f"make_factory: {what} is not an ESP image (no 0x{IMAGE_MAGIC:02X} "
                         f"magic at its first byte)")
    (chip_id,) = struct.unpack_from("<H", image, IMAGE_CHIP_ID_AT)
    return chip_id


def resolve_chip(chip: str, bootloader_image: bytes, what: str) -> str:
    """--chip as given (or, for `auto`, as the bootloader names it), checked
    against the chip id in the bootloader's own header."""
    chip = chip.lower()
    chip_id = image_chip_id(bootloader_image, what)
    if chip == "auto":
        if chip_id not in CHIP_BY_IMAGE_ID:
            raise SystemExit(f"make_factory: {what} names chip id {chip_id}, which ROM_BOOTLOADER "
                             f"does not list — pass --chip and --bootloader-offset explicitly.")
        return CHIP_BY_IMAGE_ID[chip_id]
    if chip in ROM_BOOTLOADER and ROM_BOOTLOADER[chip][1] != chip_id:
        named = CHIP_BY_IMAGE_ID.get(chip_id, f"chip id {chip_id}")
        raise SystemExit(f"make_factory: --chip {chip}, but {what} was built for {named} — "
                         f"refusing to merge an image whose bootloader the ROM would load "
                         f"from the wrong place, or not at all.")
    return chip


def bootloader_offset(chip: str, override: str | None) -> int:
    """Where the bootloader goes in the image: the ROM's address for `chip`,
    unless --bootloader-offset says otherwise (a warning when the two differ)."""
    rom = ROM_BOOTLOADER.get(chip, (None, None))[0]
    if override is None:
        if rom is None:
            raise SystemExit(f"make_factory: no ROM bootloader offset known for chip '{chip}' "
                             f"(ROM_BOOTLOADER lists {', '.join(ROM_BOOTLOADER)}) — pass "
                             f"--bootloader-offset with the address its ROM loads from.")
        return rom
    off = int(override, 0)
    if rom is not None and off != rom:
        print(f"make_factory: WARNING --bootloader-offset {hex(off)} overrides the {chip} ROM's "
              f"{hex(rom)}; a board flashed from 0x0 will not find this bootloader.",
              file=sys.stderr)
    return off


def check_factory_layout(image: bytes, bl_off: int, chip: str, app_off: int) -> None:
    """The merged file, read back: the bootloader where the ROM will look for
    it (built for this chip), the partition table at 0x8000, the app at its
    slot. A factory image that fails this is never published."""
    want = ROM_BOOTLOADER.get(chip, (None, None))[1]
    got = image_chip_id(image[bl_off:bl_off + 24], f"the merged image at {hex(bl_off)}")
    if want is not None and got != want:
        raise SystemExit(f"make_factory: the merged image's bootloader at {hex(bl_off)} names "
                         f"chip id {got}, not {chip}'s {want}")
    if image[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + 2] != struct.pack("<H", PARTITION_MAGIC):
        raise SystemExit(f"make_factory: no partition table at {hex(PARTITION_TABLE_OFFSET)} "
                         f"in the merged image")
    if len(image) <= app_off or image[app_off] != IMAGE_MAGIC:
        raise SystemExit(f"make_factory: no app image at {hex(app_off)} in the merged image")


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
    ap.add_argument("--chip", required=True,
                    help="esptool chip id, e.g. esp32 / esp32s3 / esp32c3 / esp32c6, or `auto` "
                         "to take it from bootloader.bin's image header")
    ap.add_argument("--build-dir", type=Path,
                    help="PlatformIO output dir holding bootloader.bin, partitions.bin, firmware.bin "
                         "(PlatformIO naming). Use the explicit part flags for arduino-cli output.")
    ap.add_argument("--bootloader", type=Path, help="explicit bootloader .bin (overrides --build-dir)")
    ap.add_argument("--partitions", type=Path, help="explicit partition-table .bin (overrides --build-dir)")
    ap.add_argument("--app", type=Path, help="explicit app .bin (overrides --build-dir)")
    ap.add_argument("--out", required=True, type=Path, help="output factory .bin")
    ap.add_argument("--bootloader-offset", default=None,
                    help="override the second-stage bootloader offset (default: the chip ROM's "
                         "own — 0x1000 for esp32/esp32s2, 0x0 for esp32s3/c3/c6/c2/h2)")
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

    chip = resolve_chip(args.chip, bootloader.read_bytes(), str(bootloader))
    bl_off = bootloader_offset(chip, args.bootloader_offset)

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
        sys.executable, "-m", "esptool", "--chip", chip, "merge_bin",
        "-o", str(args.out),
        "--flash_mode", "keep", "--flash_freq", "keep", "--flash_size", args.flash_size,
        hex(bl_off), str(bootloader),
        hex(PARTITION_TABLE_OFFSET), str(partitions),
        hex(app_off), str(app),
    ]
    print(f"make_factory: {chip} bootloader @ {hex(bl_off)}, app → {app_label} @ {hex(app_off)}; "
          f"running:\n  " + " ".join(cmd))
    if args.dry_run:
        return
    subprocess.run(cmd, check=True)

    size = args.out.stat().st_size
    if size <= 0:
        raise SystemExit("make_factory: produced an empty file")
    try:
        check_factory_layout(args.out.read_bytes(), bl_off, chip, app_off)
    except SystemExit:
        args.out.unlink()  # never leave an image that cannot boot where a caller would pick it up
        raise
    print(f"make_factory: wrote {args.out} ({size} bytes)")


if __name__ == "__main__":
    main()
