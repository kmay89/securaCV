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

WHY IT NOW READS EVERY PUBLISHING WORKFLOW, NOT ONE FILE
--------------------------------------------------------
The first version of this gate read `firmware-release.yml` and nothing else —
the one path that had ALREADY been fixed. `flasher-release.yml` builds the same
three S3 products for the browser flasher's rebuild and dev channels, and it
still carried `CDCOnBoot=default`. So the drift this file exists to prevent was
sitting in the release tree the whole time it was reporting OK, one filename
away. (`flasher-release.yml`'s own header says the two paths "can't drift"
because they share build_flash_manifest.py — true, and irrelevant: they drifted
UPSTREAM of it, in the compile flags.)

Coverage is therefore derived, not typed. A workflow is in scope when it
PUBLISHES something a user flashes — it uploads a release asset or emits
manifest-flash.json — so a new release path is covered the day it is written.
Bench-only workflows are out of scope because nothing they build is flashed,
but they cannot silently drop out either: a workflow carrying an S3 FQBN that
is neither publishing nor named in BENCH_ONLY below fails this gate. Exemptions
live where review can see them, the same rule .github/ci-policy.yml follows.

TWO WAYS THIS GATE COULD PASS WHILE CHECKING NOTHING, BOTH NOW CLOSED
--------------------------------------------------------------------
  * Zero FQBNs found. If the release workflows are ever restructured so the
    board strings move (a matrix file, a composite action), the scan matches
    nothing and every check vacuously passes. It printed "OK — 0 ESP32 release
    FQBN(s) agree" and exited 0. Zero is now a failure.
  * common.ini missing. The bench half of the contract was inside
    `if COMMON_INI.exists():`, so a rename turned the reference the release
    FQBNs are compared against into a silent skip. Missing is now a failure.

Both are the same lesson the dictionary gate states outright: a drift gate that
silently finds nothing is worse than no gate, because the PR that breaks the
thing reads as reviewed.
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WORKFLOWS = REPO / ".github/workflows"
COMMON_INI = REPO / "firmware/envs/platformio/common.ini"

# A workflow is in scope when it ships something a user flashes. Detected from
# what it DOES rather than from a list of names, so a new release path inherits
# the check instead of waiting to be remembered.
PUBLISH_RE = re.compile(
    r"action-gh-release|gh release (?:upload|create)|manifest-flash", re.I
)

# YAML comments, blanked before ANY rule runs. Both questions this gate asks —
# "does this workflow publish" and "what board strings does it build" — are
# questions about what a workflow DOES, and a comment does neither.
#
# Getting this wrong in one place and not the other is worse than getting it
# wrong in both: a bench workflow whose comment reads "# does not emit
# manifest-flash.json" would be classified as publishing, skip BENCH_ONLY, and
# then be failed for a bench FQBN that is correct. Prose that describes the
# rule would break the build. One strip, applied once, for every rule.
#
# `#` only opens a comment at line start or after whitespace, which is YAML's
# own rule — so `foo#bar` survives. To end of line only, never the newline, so
# line numbers still point where a human is looking.
YAML_COMMENT = re.compile(r"(?m)(?:(?<=\s)|^)#.*$")

# Workflows that carry an S3 FQBN and are deliberately NOT enforced, with the
# reason. Nothing these build is flashed by anyone — they compile to prove the
# tree compiles, and their binaries are measured or discarded, never shipped.
# A workflow with an S3 FQBN that is neither publishing nor listed here is an
# error: that is what keeps a new or renamed release path from landing outside
# this gate's field of view, which is exactly how the bug above survived.
BENCH_ONLY = {
    "firmware.yml": "compile matrix — proves the tree builds; artifacts discarded",
    "csi_module_disable_matrix.yml": "per-module disable builds — compile-only",
    "ram_audit.yml": "measures static RAM from the .elf — never flashed",
}

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


def code_of(text: str) -> str:
    """The workflow with its comments blanked, line numbering intact."""
    return YAML_COMMENT.sub("", text)


def board_fqbns(text: str):
    """(line_no, fqbn) for every board string in `text`.

    Callers pass COMMENT-STRIPPED text, so an FQBN quoted in prose — including
    the counter-examples this rule's own comments give — is not a violation of
    the rule that explains it. That also covers trailing comments, which a
    line-starts-with-# test missed.
    """
    for line_no, line in enumerate(text.splitlines(), 1):
        for fqbn in FQBN_RE.findall(line):
            yield line_no, fqbn


def main() -> int:
    problems: list[str] = []

    if not WORKFLOWS.is_dir():
        print(f"{Path(__file__).name}: {WORKFLOWS} not found", file=sys.stderr)
        return 1

    seen = 0
    checked: list[str] = []
    for wf in sorted(WORKFLOWS.glob("*.yml")):
        text = code_of(wf.read_text(encoding="utf-8"))
        found = [(n, f) for n, f in board_fqbns(text) if family(f) != "other"]
        if not found:
            continue

        if not PUBLISH_RE.search(text):
            # Bench-only is fine, but only when someone said so on purpose.
            if wf.name not in BENCH_ONLY:
                problems.append(
                    f"{wf.relative_to(REPO)}: carries {len(found)} ESP32 FQBN(s) but "
                    "publishes nothing this gate recognizes, and is not listed in "
                    "BENCH_ONLY. Either it ships something a user flashes (then it "
                    "must upload a release asset / emit manifest-flash.json, and this "
                    "gate will enforce the console rule on it), or it is bench-only "
                    "and belongs in BENCH_ONLY with a reason. Silence here is how a "
                    "release path lands outside this check."
                )
            continue

        checked.append(wf.name)
        for line_no, fqbn in found:
            fam = family(fqbn)
            opt = cdc_option(fqbn)
            seen += 1
            if fam == "s3" and opt != "cdc":
                problems.append(
                    f"{wf.relative_to(REPO)}:{line_no}: ESP32-S3 FQBN has "
                    f"CDCOnBoot={opt or '(unset)'} — `Serial` will go to UART0 and the "
                    f"serial monitor will be silent on every board flashed from this "
                    f"artifact. Use CDCOnBoot=cdc, matching "
                    f"-DARDUINO_USB_CDC_ON_BOOT=1 in common.ini.\n    {fqbn}"
                )
            if fam == "jtag" and opt == "cdc":
                problems.append(
                    f"{wf.relative_to(REPO)}:{line_no}: ESP32-C3/C6 FQBN has "
                    f"CDCOnBoot=cdc — these parts provide `Serial` on USB-Serial/JTAG and "
                    f"the flag prevents it. common.ini undefines it for exactly this "
                    f"reason.\n    {fqbn}"
                )

    # A scan that matched nothing has checked nothing. Restructure the release
    # workflows so the board strings live somewhere else and every rule above
    # passes vacuously — which is how this gate reported OK on 0 FQBNs.
    if seen == 0:
        problems.append(
            "no ESP32 FQBNs found in any publishing workflow. Either the release "
            "path stopped building firmware, or the board strings moved somewhere "
            "this scan cannot see (a matrix file, a composite action). Point it at "
            "the new home — do not leave a gate that passes because it looked "
            "nowhere."
        )

    # The bench side of the contract, and it is REQUIRED to exist: the release
    # FQBNs above are checked for agreement with this file, so a missing one
    # turns the comparison into a coin flip nobody hears land.
    if not COMMON_INI.is_file():
        problems.append(
            f"{COMMON_INI.relative_to(REPO)} is missing — it is the bench reference "
            "the release FQBNs are compared against. If it moved, point this lint at "
            "its new home rather than letting the comparison quietly stop happening."
        )
    else:
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

    print(
        f"{Path(__file__).name}: OK — {seen} ESP32 FQBN(s) across "
        f"{len(checked)} publishing workflow(s) ({', '.join(checked)}) agree with "
        "the bench about `Serial`."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
