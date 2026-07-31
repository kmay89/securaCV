#!/usr/bin/env python3
"""No board may reboot a Wi-Fi link that has never worked.

This lint exists because of a specific, expensive bug. The 4-inch display's
boot-time Wi-Fi join timed out, the board rebooted, and the identical join ran
again against the identical network with the identical credentials — forever.
It never finished booting, so the setup wizard that could have fixed the
password never appeared. To an operator with no keyboard it looks like a dead
board, and it cost a real evening to diagnose.

The fix was not local. THREE boards carried copy-pasted retry logic and had
already drifted apart:

    canary-display   had the "don't reboot a dead link" guard, had LOST jitter
    canary-sense     had jitter, still rebooted forever
    canary-vision    had jitter, still rebooted forever

and a fourth copy sat in the wasm emulator, under a comment claiming it was
"the same as glass" when it no longer was. The rule now lives once, in
`firmware/common/network/wifi_join_policy.h`, and this lint is what stops a
fifth copy from quietly appearing.

## The rule

Inside a Wi-Fi supervision file, **every `ESP.restart()` must be reachable only
for a link that has been associated at least once since power-on.** In practice
that means the call sits inside the shared policy's `WifiAction::Reboot` branch,
or is directly guarded by the board's `ever_online` flag.

Why that specific rule: a reboot is plausible recovery for a link that WAS up
and wedged — it may clear a stuck radio or a stale DHCP lease. It is never
recovery for a link that has never associated, because the credentials or the
network are simply wrong and a reboot just re-runs the same failed join on a
timer. Boot-time join failures must therefore not reboot at all.

Run locally:  python3 scripts/lint_wifi_join_policy.py
CI:           lint.yml
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

# Files that supervise a Wi-Fi link. Anything that retries a join belongs here;
# if a new board adds one, add it to this list in the same change — a guard is
# only as wide as its list, which is the other lesson from that session.
WIFI_SUPERVISORS = [
    "firmware/projects/canary-display/src/net/wifi_mgr.cpp",
    "firmware/projects/canary-sense/src/net/wifi_mgr.cpp",
    "firmware/projects/canary-vision/src/net/wifi_mgr.cpp",
    "canary-local/emulator/src/emu_net.cpp",
]

# The generated Arduino sketch is a flattened copy of the display tree, so it
# carries the same code and must satisfy the same rule.
GENERATED_COPIES = [
    "firmware/projects/canary-display/arduino/canary_display/wifi_mgr.cpp",
]

# Evidence that a restart is genuinely gated on the link having once been
# online. Matched against the ENCLOSING construct, not "somewhere nearby" — see
# enclosing_guard(). A proximity window was the first version of this check and
# it had a false negative on the exact bug class it exists for: an ungated
# reboot placed just above the shared switch still had `ever_online` within the
# window, because the lines that populate the WifiRetry struct mention it. A
# lint that reads as covered while missing the real case is worse than none.
GATE_PATTERNS = [
    re.compile(r"WifiAction::Reboot"),
    # No LEADING \b: `_` is a word character, so `\bever_online\b` does not
    # match `s_ever_online` — the actual spelling of the flag on every board.
    # The real tree hid this because its reboot routes through the shared
    # switch, so only an adversarial probe surfaced it.
    re.compile(r"ever_online\b"),
    re.compile(r"ever_up\b"),
    # The shared dispatcher itself: reaching a restart through
    # `switch (wifi_next_action(...))` IS the gate, since the policy only ever
    # returns Reboot for a link that has been up.
    re.compile(r"\bwifi_next_action\b"),
]

RESTART = re.compile(r"\b(?:ESP\.restart|esp_restart)\s*\(")

# Every supervisor must defer to the shared core rather than re-deriving the
# rules. Checked as a PROPERTY (does this file speak the shared vocabulary?)
# rather than a mechanism (does it have this exact include line?): the emulator
# reaches the same header transitively through wifi_mgr.h, and demanding a
# redundant include there would mean editing a file the committed wasm `dist/`
# artifacts are built from, for no behavioural gain. If the header ever stopped
# being reachable, that is a compile error, which needs no lint.
POLICY_USE = re.compile(
    r'#include\s+"(?:network/)?wifi_join_policy\.h"'
    r'|\bwifi_next_action\b'
    r'|\bjoin_failure_(?:label|detail|hint)\b'
    r'|\bWifiAction::'
)


def _depths(lines: list[str]) -> list[int]:
    """Brace depth at the START of each line, comments stripped."""
    out, d = [], 0
    for ln in lines:
        out.append(d)
        code = re.sub(r"//.*$", "", ln)
        d += code.count("{") - code.count("}")
    return out


def _header_text(lines: list[str], opener: int) -> str:
    """The FULL header of the construct opening at `opener`, and nothing else.

    A blunt "opener plus the two lines above" was the third defect in this
    lint: those two lines are often `st.ever_online = s_ever_online;` from the
    WifiRetry population, which re-admits the very proximity false-negative the
    brace walk was written to remove.

    Instead, reconstruct the header exactly: a condition split across lines
    leaves the opener with more `)` than `(`, so walk back only while that is
    true. `if (a &&\n    b) {` picks up both lines; `if (x) {` picks up one.
    """
    text = re.sub(r"//.*$", "", lines[opener])
    j = opener
    while j > 0 and text.count(")") > text.count("("):
        j -= 1
        text = re.sub(r"//.*$", "", lines[j]) + "\n" + text
    return text


def enclosing_guard(lines: list[str], i: int) -> bool:
    """Is line `i` governed by ANY construct that gates on ever-having-been-online?

    Walks real brace depth outward through every enclosing construct, not just
    the innermost one. Both halves of that matter, and both were learned the
    hard way:

    * A fixed proximity window (v1) MISSED an ungated reboot placed beside the
      shared switch, because the lines populating WifiRetry mention
      `ever_online` — a false negative on the exact bug this exists to catch.
    * Checking only the INNERMOST block (v2) FLAGGED correct code of the shape
      `if (s_ever_online && ...) { if (radio_ok()) { restart } }`, because the
      inner `if` says nothing about being online. A false positive is the worse
      failure: it blocks correct work and teaches people to route around the
      check.

    So: any enclosing `if`/`while`/`switch` header, or any governing `case`
    label, that shows a gate makes the call site legitimate.
    """
    depth_before = _depths(lines)
    target = depth_before[i]

    def governing_case(level: int, upto: int) -> bool:
        """A `case X:` at `level`, above `upto`, before leaving that block."""
        for j in range(upto - 1, -1, -1):
            if depth_before[j] < level:
                return False
            if depth_before[j] != level:
                continue
            # Greedy to the LAST colon: a non-greedy match stops at the first
            # `:` of `canary::net::WifiAction::Reboot` and reads the label as
            # "canary", which turns every correct call site into a violation.
            m = re.match(r"\s*case\s+(.+):\s*$", lines[j])
            if m and re.search(r"WifiAction::Reboot", m.group(1)):
                return True
        return False

    if governing_case(target, i):
        return True

    # Walk outward: nearest opener at each successively shallower depth.
    j, level = i - 1, target - 1
    while level >= 0 and j >= 0:
        opener = None
        while j >= 0:
            if depth_before[j] == level and "{" in re.sub(r"//.*$", "", lines[j]):
                opener = j
                break
            j -= 1
        if opener is None:
            break
        head = _header_text(lines, opener)
        if any(p.search(head) for p in GATE_PATTERNS):
            return True
        if governing_case(level, opener):
            return True
        j, level = opener - 1, level - 1
    return False


def check_file(rel: str) -> list[str]:
    path = REPO / rel
    if not path.exists():
        return [
            f"{rel}: listed as a Wi-Fi supervisor but not found — if the file "
            f"moved, update WIFI_SUPERVISORS in this script."
        ]

    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    problems: list[str] = []

    if not POLICY_USE.search(text):
        problems.append(
            f"{rel}: supervises a Wi-Fi link but shows no sign of using the "
            f"shared policy (wifi_join_policy.h — wifi_next_action, "
            f"join_failure_*, WifiAction). The join/retry rules are shared on "
            f"purpose: three boards each kept their own copy and drifted apart, "
            f"which is how a reboot loop shipped on one of them."
        )

    for i, line in enumerate(lines):
        if line.lstrip().startswith("//") or not RESTART.search(line):
            continue
        if enclosing_guard(lines, i):
            continue
        problems.append(
            f"{rel}:{i + 1}: ESP.restart() with no evidence it is gated on the "
            f"link having been online at least once.\n"
            f"      {line.strip()}\n"
            f"      A link that never associated is a WRONG configuration, not a "
            f"wedged one: rebooting re-runs the same failed join forever and "
            f"hides the setup wizard that could fix it. Route this through "
            f"wifi_next_action() (WifiAction::Reboot) or guard it on the "
            f"board's ever_online flag."
        )
    return problems


def main() -> int:
    problems: list[str] = []
    print("Wi-Fi join policy (no reboot loops on a link that never worked):")

    for rel in WIFI_SUPERVISORS:
        found = check_file(rel)
        problems.extend(found)
        if not found:
            print(f"  {rel} ✓")

    for rel in GENERATED_COPIES:
        path = REPO / rel
        if not path.exists():
            # The sketch is regenerated by setup.sh; a missing copy is that
            # script's business, not this lint's.
            continue
        found = check_file(rel)
        problems.extend(found)
        if not found:
            print(f"  {rel} ✓ (generated copy)")

    if problems:
        print()
        for p in problems:
            print(f"::error::{p}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
