#!/usr/bin/env python3
"""Release FQBNs and PlatformIO envs must agree about where `Serial` goes.

The ESP32 Arduino core decides at BUILD time whether `Serial` is the USB
console or UART0 on the GPIO pins:

  * ESP32-S3 — USB-OTG. `Serial` reaches USB only with CDC-on-boot ENABLED
    (`CDCOnBoot=cdc`, or `-DARDUINO_USB_CDC_ON_BOOT=1` in PlatformIO).
  * ESP32-C3 / C6 — USB-Serial/JTAG. These provide `Serial` on their own, and
    the S3 flag PREVENTS it (`common.ini` undefines it on purpose).

Get it wrong on an S3 and everything still builds, flashes and runs — it just
prints to a header nobody has wired. That shipped: released S3 images were
built with `CDCOnBoot=default`, so the desktop app's serial monitor was silent
on every S3 board while the bench builds (which set the flag) printed fine, and
"the monitor doesn't work for some firmwares" had no visible cause.

That is the same release-vs-bench drift that kept the display firmware out of
five releases (RELEASE_LESSONS (i)) — the release path and the path we test
disagreeing, with nothing comparing them. This compares them.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RELEASE_WF = REPO / ".github/workflows/firmware-release.yml"
COMMON_INI = REPO / "firmware/envs/platformio/common.ini"

# Chip family inferred from the FQBN's board id.
S3_MARKERS = ("esp32s3", "esp32_s3", "xiao_esp32s3")
JTAG_MARKERS = ("esp32c3", "esp32c6", "esp32_c3", "esp32_c6", "xiao_esp32c3", "xiao_esp32c6")

FQBN_RE = re.compile(r"esp32:esp32:[A-Za-z0-9_\-]+(?::[A-Za-z0-9_=,\-]+)?")


def family(fqbn: str) -> str:
    low = fqbn.lower()
    if any(m in low for m in S3_MARKERS):
        return "s3"
    if any(m in low for m in JTAG_MARKERS):
        return "jtag"
    return "other"


def cdc_option(fqbn: str) -> str | None:
    m = re.search(r"CDCOnBoot=([A-Za-z0-9_]+)", fqbn)
    return m.group(1) if m else None


def main() -> int:
    problems: list[str] = []

    if not RELEASE_WF.exists():
        print(f"{Path(__file__).name}: {RELEASE_WF} not found", file=sys.stderr)
        return 1

    text = RELEASE_WF.read_text(encoding="utf-8")
    seen = 0
    for line_no, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith("#"):
            continue  # a comment explaining the rule is not a violation of it
        for fqbn in FQBN_RE.findall(line):
            fam = family(fqbn)
            opt = cdc_option(fqbn)
            if fam == "other":
                continue
            seen += 1
            if fam == "s3" and opt != "cdc":
                problems.append(
                    f"{RELEASE_WF.relative_to(REPO)}:{line_no}: ESP32-S3 FQBN has "
                    f"CDCOnBoot={opt or '(unset)'} — `Serial` will go to UART0 and the "
                    f"serial monitor will be silent on this board. Use CDCOnBoot=cdc, "
                    f"matching -DARDUINO_USB_CDC_ON_BOOT=1 in common.ini.\n    {fqbn}"
                )
            if fam == "jtag" and opt == "cdc":
                problems.append(
                    f"{RELEASE_WF.relative_to(REPO)}:{line_no}: ESP32-C3/C6 FQBN has "
                    f"CDCOnBoot=cdc — these parts provide `Serial` on USB-Serial/JTAG and "
                    f"the flag prevents it. common.ini undefines it for exactly this "
                    f"reason.\n    {fqbn}"
                )

    # The bench side of the contract: if common.ini ever stops enabling the flag
    # for S3, the rule above would be enforcing agreement with the wrong thing.
    if COMMON_INI.exists():
        ini = COMMON_INI.read_text(encoding="utf-8")
        if "-DARDUINO_USB_CDC_ON_BOOT=1" not in ini:
            problems.append(
                f"{COMMON_INI.relative_to(REPO)}: expected -DARDUINO_USB_CDC_ON_BOOT=1 "
                "(the S3 bench console). If that moved on purpose, update this lint — "
                "release FQBNs are checked against it."
            )

    if problems:
        print(f"{Path(__file__).name}: {len(problems)} problem(s):\n")
        for p in problems:
            print(f"  {p}\n")
        return 1

    print(f"{Path(__file__).name}: OK — {seen} ESP32 release FQBN(s) agree with the bench about `Serial`.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
