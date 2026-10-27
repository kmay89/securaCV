#!/usr/bin/env python3
"""Stamp a SecuraCV product and version into a built app image's descriptor.

Every ESP32-family app image opens its first segment with an esp_app_desc_t
(image offset 0x20): the record esptool, the OTA engine and both flashers
read to say what an image is. SecuraCV's images are Arduino builds, and the
Arduino core's precompiled libraries define that record, so every release up
to fw-v2.4.17 called itself `arduino-lib-builder`, version `esp-idf: v4.4.7
38eeba213a` (or the core's other hashes). The flashers read exactly that off
a connected board, so they could not tell a SecuraCV board from any other
Arduino sketch: no product, no version, no downgrade warning
(firmware/LESSONS_LEARNED.md). A sketch cannot set the record at compile
time, so the release sets it after the build.

The identity, defined here once:

  project_name  the product's `asset_stem` in canary-local/devices/flash.json
                ("canary-wap", "canary-display-nightstand-s3"). Product ids
                outgrow the field's 31 characters; every stem fits, and both
                flashers already map a stem back to its product
                (matchProjectToProduct in flash-core.js and desktop/src/app.js).
  version       the firmware's own version string, the CANARY_FW_VERSION /
                FIRMWARE_VERSION literal ("2.4.18", "2.4.18-wap"), and only
                when the image already carries that literal outside the
                descriptor, so the release's `strings | grep -Fx` version
                guards still test the firmware and never this stamp.

Only those two char[32] fields change, then the image's checksum byte and,
when its header says one is appended, its SHA-256. app_elf_sha256 (the boot
guard's image identity, health/boot_guard.h), secure_version (anti-rollback),
date, time and idf_ver are never touched, and stamping a stamped image with
the same values changes nothing.

    python3 firmware/scripts/stamp_app_desc.py release --version 2.4.18
    python3 firmware/scripts/stamp_app_desc.py image --project canary-wap \\
        --version 2.4.18-wap /tmp/wap-build/canary_wap.ino.bin

`release` stamps, in place, every product build_flash_manifest.py packages
(its BUILD table says where each app lands), then reads each stamped image
back with esptool's image_info and fails unless esptool calls the checksum
and the hash valid and reads back the name and version written.
firmware-release.yml and flasher-release.yml run it after their last build
step and before anything hashes, signs or merges an app, so the OTA
manifest's sha256 and signature and the factory image all cover the stamped
bytes. An image whose version literal is missing is left unstamped with a
warning: the version guards after this step say which header to bump.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_flash_manifest  # noqa: E402 — local sibling tool
import make_factory  # noqa: E402 — local sibling tool

HEADER_LEN = 24                 # esp_image_header_t
SEGMENT_HEADER_LEN = 8          # esp_image_segment_header_t: load_addr, data_len
MAX_SEGMENTS = 16               # ESP_IMAGE_MAX_SEGMENTS
HASH_APPENDED_AT = 23           # esp_image_header_t.hash_appended
CHECKSUM_SEED = 0xEF            # ESP_ROM_CHECKSUM_INITIAL
HASH_LEN = 32
DESC_OFFSET = 0x20              # header + the first segment's header
DESC_MAGIC = 0xABCD5432
DESC_LEN = 256
FIELDS = {"version": (16, 32), "project_name": (48, 32)}   # offset inside esp_app_desc_t, size


class Refused(ValueError):
    """The image or a value is not one this tool can stamp safely."""


class NotCarried(Refused):
    """The image does not carry the version literal it would be stamped with."""


@dataclass(frozen=True)
class Layout:
    segments: tuple[tuple[int, int], ...]   # (data offset, data length) per segment
    checksum_at: int
    hash_at: int | None                     # None when the header appends no SHA-256


def layout(image: bytes) -> Layout:
    """Where an app image's segments, checksum and hash are, or Refused."""
    if len(image) < HEADER_LEN or image[0] != make_factory.IMAGE_MAGIC:
        raise Refused(f"not an ESP image (no 0x{make_factory.IMAGE_MAGIC:02X} magic)")
    chip_id = make_factory.image_chip_id(image, "the app")
    if chip_id not in make_factory.CHIP_BY_IMAGE_ID:
        raise Refused(f"chip id {chip_id} is not one make_factory.ROM_BOOTLOADER lists")
    count = image[1]
    if not 1 <= count <= MAX_SEGMENTS:
        raise Refused(f"{count} segments (an app image has 1 to {MAX_SEGMENTS})")
    if image[HASH_APPENDED_AT] not in (0, 1):
        raise Refused(f"hash_appended is 0x{image[HASH_APPENDED_AT]:02x}, not 0 or 1")
    pos, segments = HEADER_LEN, []
    for i in range(count):
        if pos + SEGMENT_HEADER_LEN > len(image):
            raise Refused(f"segment {i}'s header runs past the end of the file")
        (_, size) = struct.unpack_from("<II", image, pos)
        pos += SEGMENT_HEADER_LEN
        if pos + size > len(image):
            raise Refused(f"segment {i} ({size} bytes) runs past the end of the file")
        segments.append((pos, size))
        pos += size
    checksum_at = pos + 15 - pos % 16       # zero padding, then the byte ends 16-aligned
    hash_at = checksum_at + 1 if image[HASH_APPENDED_AT] else None
    end = checksum_at + 1 + (HASH_LEN if hash_at else 0)
    if len(image) != end:
        raise Refused(f"the file is {len(image)} bytes but its segments end the image at {end} "
                      f"(truncated, or something appended after it)")
    if segments[0][1] < DESC_LEN:
        raise Refused(f"the first segment is {segments[0][1]} bytes, too short for esp_app_desc_t")
    if struct.unpack_from("<I", image, DESC_OFFSET)[0] != DESC_MAGIC:
        raise Refused(f"no esp_app_desc_t at 0x{DESC_OFFSET:x} (magic 0x{DESC_MAGIC:08X})")
    return Layout(tuple(segments), checksum_at, hash_at)


def checksum(image: bytes, lay: Layout) -> int:
    """The XOR of every segment's data, seeded 0xEF — what the ROM and esptool check."""
    c = CHECKSUM_SEED
    for off, size in lay.segments:
        for b in image[off:off + size]:
            c ^= b
    return c


def check_integrity(image: bytes, lay: Layout) -> None:
    """Refuse an image whose own checksum or hash is already wrong."""
    want = checksum(image, lay)
    if image[lay.checksum_at] != want:
        raise Refused(f"its checksum byte is 0x{image[lay.checksum_at]:02x}, "
                      f"but its segments sum to 0x{want:02x}")
    if lay.hash_at is not None and image[lay.hash_at:] != hashlib.sha256(image[:lay.hash_at]).digest():
        raise Refused("its appended SHA-256 does not match its bytes")


def field(image: bytes, name: str) -> str:
    off, size = FIELDS[name]
    raw = image[DESC_OFFSET + off:DESC_OFFSET + off + size]
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def identity(image: bytes) -> tuple[str, str]:
    """(project_name, version) as the image's descriptor states them."""
    layout(image)
    return field(image, "project_name"), field(image, "version")


def check_value(name: str, value: str) -> bytes:
    """The field's bytes for `value`, or Refused: printable ASCII, 1 to 31
    characters, so the char[32] keeps its NUL (31 + NUL is the whole field)."""
    size = FIELDS[name][1]
    if not value or not re.fullmatch(r"[\x20-\x7e]+", value):
        raise Refused(f"{name} {value!r} is not printable ASCII")
    if len(value) > size - 1:
        raise Refused(f"{name} {value!r} is {len(value)} characters; the field holds {size - 1}")
    return value.encode("ascii").ljust(size, b"\0")


def carries(image: bytes, text: str) -> bool:
    """Does the image hold `text` as a whole printable run OUTSIDE the two
    stamped fields? The same test as build_flash_manifest.image_has_version,
    with the fields blanked, so the stamp can never satisfy a version guard."""
    blanked = bytearray(image)
    for off, size in FIELDS.values():
        blanked[DESC_OFFSET + off:DESC_OFFSET + off + size] = bytes(size)
    target = text.encode()
    return any(run == target for run in re.split(rb"[^\x20-\x7e]+", bytes(blanked)))


def stamp(image: bytes, project: str, version: str) -> bytes:
    """The image with project_name and version stamped and its checksum and
    hash recomputed. Raises Refused (NotCarried for a missing version literal)."""
    values = {"project_name": check_value("project_name", project),
              "version": check_value("version", version)}
    lay = layout(image)
    check_integrity(image, lay)
    if not carries(image, version):
        raise NotCarried(f"the image does not carry the version string {version!r} "
                         f"outside its descriptor")
    out = bytearray(image)
    for name, raw in values.items():
        off, size = FIELDS[name]
        out[DESC_OFFSET + off:DESC_OFFSET + off + size] = raw
    out[lay.checksum_at] = checksum(out, lay)
    if lay.hash_at is not None:
        out[lay.hash_at:] = hashlib.sha256(out[:lay.hash_at]).digest()
    return bytes(out)


def esptool_readback(path: Path, project: str, version: str, hashed: bool) -> list[str]:
    """What esptool's image_info says is wrong with a stamped image (empty when
    nothing is). image_info exits 0 on an invalid checksum or hash, so its
    words are read, not its status."""
    r = subprocess.run([sys.executable, "-m", "esptool", "image_info", "--version", "2", str(path)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return [f"esptool image_info failed: {(r.stderr or r.stdout).strip()}"]
    said = dict(line.split(": ", 1) for line in r.stdout.splitlines() if ": " in line)
    problems = []
    if not said.get("Checksum", "").endswith("(valid)"):
        problems.append(f"esptool reads its checksum as {said.get('Checksum', 'missing')!r}")
    if hashed and not said.get("Validation hash", "").endswith("(valid)"):
        problems.append(f"esptool reads its hash as {said.get('Validation hash', 'missing')!r}")
    for key, want in (("Project name", project), ("App version", version)):
        if said.get(key) != want:
            problems.append(f"esptool reads {key} {said.get(key)!r}, not {want!r}")
    return problems


def stamp_file(path: Path, project: str, version: str) -> list[str]:
    """Stamp one image file in place and read it back with esptool."""
    data = path.read_bytes()
    stamped = stamp(data, project, version)
    if stamped != data:
        path.write_bytes(stamped)
    return esptool_readback(path, project, version, layout(stamped).hash_at is not None)


def release(version: str, catalog: dict, builds: dict) -> int:
    """Stamp every built product app; 0 when every stamped image reads back right."""
    errors = []
    for p in catalog["products"]:
        pid, build = p["id"], builds.get(p["id"])
        if not build:
            continue  # build_flash_manifest.py warns about a product with no mapping
        path = build_flash_manifest.app_path(build)
        if not path.exists():
            print(f"stamp_app_desc: {pid}: nothing built at {path}, skipped (its build step said why)")
            continue
        want = version + build["vsuffix"]
        try:
            problems = stamp_file(path, p["asset_stem"], want)
        except NotCarried as e:
            print(f"::warning::{pid}: left unstamped — {e}. The version guards after this step "
                  f"name the header to bump.")
            continue
        except Refused as e:
            errors.append(f"{pid} ({path}): {e}")
            continue
        if problems:
            errors.append(f"{pid} ({path}): " + "; ".join(problems))
            continue
        print(f"stamp_app_desc: {pid}: {p['asset_stem']} {want} (esptool: checksum and hash valid)")
    for e in errors:
        print(f"::error::{e}")
    return 1 if errors else 0


def main() -> None:
    ap = argparse.ArgumentParser(description="Stamp SecuraCV's product and version into app images.")
    sub = ap.add_subparsers(dest="cmd", required=True)
    rel = sub.add_parser("release", help="stamp every product build_flash_manifest.py packages")
    rel.add_argument("--version", required=True, help="the release version, e.g. 2.4.18 (no fw-v)")
    one = sub.add_parser("image", help="stamp one app image in place")
    one.add_argument("--project", required=True, help="project_name: the product's asset_stem")
    one.add_argument("--version", required=True, help="the firmware's own version string")
    one.add_argument("path", type=Path)
    args = ap.parse_args()

    if args.cmd == "release":
        catalog = json.loads(build_flash_manifest.FLASH_JSON.read_text(encoding="utf-8"))
        sys.exit(release(args.version, catalog, build_flash_manifest.BUILD))
    try:
        problems = stamp_file(args.path, args.project, args.version)
    except Refused as e:
        sys.exit(f"stamp_app_desc: {args.path}: {e}")
    if problems:
        sys.exit(f"stamp_app_desc: {args.path}: " + "; ".join(problems))
    print(f"stamp_app_desc: {args.path}: {args.project} {args.version}")


if __name__ == "__main__":
    main()
