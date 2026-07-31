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
    re.compile(r"\bever_online\b"),
    re.compile(r"\bg_wifi_ever_up\b"),
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


def enclosing_guard(lines: list[str], i: int) -> bool:
    """Is line `i` inside a construct that gates on ever-having-been-online?

    Walks real brace depth rather than a fixed window: find the `{` that opens
    the block containing this line, then read that opener (plus the two lines
    above it, for conditions split across lines). Also accepts a `case
    WifiAction::Reboot:` label governing the line inside a switch.
    """
    # Depth *before* each line.
    depth_before: list[int] = []
    d = 0
    for ln in lines:
        depth_before.append(d)
        code = re.sub(r"//.*$", "", ln)
        d += code.count("{") - code.count("}")

    target = depth_before[i]

    # A `case` label governing this line, at the same depth, above it.
    for j in range(i - 1, -1, -1):
        if depth_before[j] < target:
            break
        # Greedy to the LAST colon on the line: a non-greedy match stops at the
        # first `:` of `canary::net::WifiAction::Reboot` and reads the label as
        # "canary", which silently turns every correct call site into a
        # violation. Caught because the clean tree went red.
        m = re.match(r"\s*case\s+(.+):\s*$", lines[j])
        if m and depth_before[j] == target:
            return bool(re.search(r"WifiAction::Reboot", m.group(1)))

    # Otherwise, the opener of the enclosing block.
    for j in range(i - 1, -1, -1):
        if depth_before[j] == target - 1 and "{" in re.sub(r"//.*$", "", lines[j]):
            head = "\n".join(lines[max(0, j - 2) : j + 1])
            return any(p.search(head) for p in GATE_PATTERNS)
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
