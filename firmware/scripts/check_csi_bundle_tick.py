#!/usr/bin/env python3
"""Hold the canary's main loop to ticking the CSI bundler where F81 put it.

Sweep F81 stopped the canary closing every open CSI bundle after each CSI
window (`csi_event_flush_bundles()` in the module bridge's feed). Bundles
now close on time, past their 10-minute window or their 2-minute quiet gap,
in `csi_bundler_tick()`, which the bridge's `securacv_csi_modules_tick()`
runs and `main.cpp`'s `loop()` calls once per pass, as the canary-wap's
loop does.

`firmware/tests_host/test_csi_modules_integration.cpp` drives the real
bridge over the real library and plays the loop, but its loop is a stand-in
for `main.cpp`'s (no host suite compiles `main.cpp`). So the device wiring
is a property of the source shape. Reviewing F81, deleting the call from
`loop()`, or moving it under the CSI power and degrade gates, left every
host test and lint green. Without the call a bundle closes only inside a
later admit, and never while the power policy or a heap degrade sheds CSI.
This check is that wiring's guard.

## The rules

In `firmware/canary/src/main.cpp`'s `loop()`:

1. `securacv_csi_modules_tick()` is called exactly once.
2. No preprocessor conditional opened inside `loop()` wraps the call: no
   `FEATURE_POWER_POLICY` or `FEATURE_DIAGNOSTICS` block, and not
   `#if FEATURE_CSI` either. That is #1763's placement, which the merge kept
   (#1762 had the call under `#if FEATURE_CSI`). It costs nothing in a
   CSI-off build: no module registers there (`securacv_csi_modules_init()`
   runs only under `FEATURE_CSI`, after `csi::init`), so nothing opens and
   the tick scans eight empty slots. A system.integrity tamper never waits
   for the tick in any build: the module seals its own key with
   `csi_bundler_flush_key()` at emit. The rule forbids every wrapper so that
   no gate added later can strand an open bundle.
3. The call is a statement of its own at the top level of the loop body:
   inside no nested block (`{ ... }`), and not the statement an unbraced
   `if`, `else`, `for` or `while` controls. The text before it, with
   preprocessor lines and comments blanked, ends in `;` or `}`; the call is
   exactly `securacv_csi_modules_tick();`. So it is outside the
   `if (pf->csi)` / `if (dl < DEGRADE_CRITICAL)` gate on `csi::process()`.
4. Nothing leaves `loop()` before it: no `return` or `goto` between the
   start of the body and the call.
5. It runs before `csi_event_egress_pump()`, which `loop()` also calls, so
   a bundle that closes is published in the same pass.

In `firmware/canary/src/csi_modules_integration.cpp`:

6. `securacv_csi_modules_tick()` calls `csi_bundler_tick()`, and
   `securacv_csi_modules_feed()` closes no bundle: it calls neither
   `csi_event_flush_bundles()` nor `csi_bundler_flush_all()`.

## It proves it bites

Each run applies mutations to the two sources in memory and requires the
check to fail on every one, the review's edits among them. A mutation whose
anchor moved fails the run rather than passing quietly.

Run locally:  python3 firmware/scripts/check_csi_bundle_tick.py   (repo root)
CI:           firmware.yml "CSI Sketch Copy Sync", via check_csi_sync.sh
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_event_egress_order import (  # noqa: E402  (shared C++ scanning helpers)
    AnchorMissing,
    blank_comments_and_strings,
    bodies,
    squash,
)

REPO = Path(__file__).resolve().parents[2]
MAIN_CPP = "firmware/canary/src/main.cpp"
BRIDGE_CPP = "firmware/canary/src/csi_modules_integration.cpp"

SIG_LOOP = r"\bvoid\s+loop\s*\(\s*(?:void)?\s*\)"
SIG_TICK = r"\bvoid\s+securacv_csi_modules_tick\s*\(\s*(?:void)?\s*\)"
SIG_FEED = r"\bvoid\s+securacv_csi_modules_feed\s*\([^)]*\)"
TICK_CALL = "securacv_csi_modules_tick("
PUMP_CALL = "csi_event_egress_pump("
DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*(\w+)([^\n]*)", re.M)


def the_body(code: str, signature: str, what: str, errors: list[str]) -> tuple[int, int] | None:
    """The one definition of `what` (an empty body counts: an emptied tick
    is refused for what it no longer calls)."""
    spans = bodies(code, signature)
    if len(spans) != 1:
        errors.append(f"{what}: expected one definition with a body, found {len(spans)}")
        return None
    return spans[0]


def blank_directives(text: str) -> str:
    """Preprocessor lines become spaces (offsets and newlines kept)."""
    return DIRECTIVE.sub(lambda m: " " * len(m.group(0)), text)


def conditionals_at(text: str, pos: int) -> list[tuple[str, bool]]:
    """The conditionals opened in `text` and still open at `pos`, outermost
    first: (the opening directive, squashed; whether `pos` is past an
    `#else` / `#elif` of it)."""
    stack: list[list] = []
    for m in DIRECTIVE.finditer(text[:pos]):
        kind = m.group(1)
        if kind in ("if", "ifdef", "ifndef"):
            stack.append([squash("#" + kind + m.group(2)), False])
        elif kind in ("else", "elif") and stack:
            stack[-1][1] = True
        elif kind == "endif" and stack:
            stack.pop()
    return [(d, e) for d, e in stack]


def check_loop(main_src: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(main_src)
    span = the_body(code, SIG_LOOP, f"{MAIN_CPP}: loop()", errors)
    if span is None:
        return
    body = code[span[0]:span[1]]
    where = f"{MAIN_CPP}: loop()"
    calls = [m.start() for m in re.finditer(r"\bsecuracv_csi_modules_tick\s*\(", body)]
    if len(calls) != 1:
        errors.append(f"{where}: must call securacv_csi_modules_tick() exactly once, found "
                      f"{len(calls)} — without it a CSI bundle closes only inside a later admit, "
                      "and never while CSI is shed (sweep F81)")
        return
    at = calls[0]
    conds = conditionals_at(body, at)
    if conds:
        errors.append(f"{where}: securacv_csi_modules_tick() must sit in no preprocessor "
                      "conditional opened in loop(), `#if FEATURE_CSI` included (#1763's "
                      "placement: no gate may strand an open bundle); found "
                      f"{[d + (' (else)' if e else '') for d, e in conds]}")
    flat = blank_directives(body)
    depth = flat[:at].count("{") - flat[:at].count("}")
    before = flat[:at].rstrip()
    if depth != 0 or (before and before[-1] not in ";}"):
        errors.append(f"{where}: securacv_csi_modules_tick() must be a statement of its own at "
                      "the top of the loop body — in no block and controlled by no `if`, `else`, "
                      "`for` or `while` — so the CSI power and degrade gates never skip it")
    if not re.match(r"securacv_csi_modules_tick\s*\(\s*\)\s*;", flat[at:]):
        errors.append(f"{where}: the call must be exactly `securacv_csi_modules_tick();`")
    exit_ = re.search(r"\b(?:return|goto)\b", flat[:at])
    if exit_:
        errors.append(f"{where}: `{exit_.group(0)}` before securacv_csi_modules_tick() — "
                      "nothing may leave the loop before the bundles are ticked")
    pump = flat.find(PUMP_CALL)
    if pump < 0 or pump < at:
        errors.append(f"{where}: securacv_csi_modules_tick() must run before "
                      "csi_event_egress_pump(), so a closed bundle is published in the same pass")


def check_bridge(bridge_src: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(bridge_src)
    span = the_body(code, SIG_TICK, f"{BRIDGE_CPP}: securacv_csi_modules_tick()", errors)
    if span is not None and not re.search(r"\bcsi_bundler_tick\s*\(\s*\)\s*;",
                                          code[span[0]:span[1]]):
        errors.append(f"{BRIDGE_CPP}: securacv_csi_modules_tick() must call csi_bundler_tick()")
    span = the_body(code, SIG_FEED, f"{BRIDGE_CPP}: securacv_csi_modules_feed()", errors)
    if span is not None:
        closes = re.search(r"\b(?:csi_event_flush_bundles|csi_bundler_flush_all)\s*\(",
                           code[span[0]:span[1]])
        if closes:
            errors.append(f"{BRIDGE_CPP}: securacv_csi_modules_feed() calls "
                          f"{closes.group(0)}) — the feed closes no bundle: closing every bundle "
                          "each window makes every refresh a row that spends the hourly ceiling "
                          "(sweep F81)")


def check(main_src: str, bridge_src: str) -> list[str]:
    errors: list[str] = []
    check_loop(main_src, errors)
    check_bridge(bridge_src, errors)
    return errors


# ── Self-test: mutations the check must refuse ───────────────────────────


def mutate_in(src: str, signature: str, pattern: str, repl: str) -> str:
    """Apply a regex substitution inside the (raw) body of one definition."""
    code = blank_comments_and_strings(src)
    spans = [s for s in bodies(code, signature) if code[s[0]:s[1]].strip()]
    if len(spans) != 1:
        raise AnchorMissing(f"definition {signature!r}")
    start, end = spans[0]
    new_body, n = re.subn(pattern, repl, src[start:end], count=1, flags=re.S)
    if n == 0:
        raise AnchorMissing(pattern)
    return src[:start] + new_body + src[end:]


TICK_STMT = r"\n([ \t]*)securacv_csi_modules_tick\(\)\s*;"
# The call with the comment block above it.
TICK_BLOCK = r"\n(?:[ \t]*//[^\n]*\n)*[ \t]*securacv_csi_modules_tick\(\)\s*;"


def in_loop(pattern: str, repl: str) -> Callable[[str, str], tuple[str, str]]:
    return lambda m, b: (mutate_in(m, SIG_LOOP, pattern, repl), b)


MUTATIONS: list[tuple[str, Callable[[str, str], tuple[str, str]]]] = [
    # The review's edits.
    ("the loop no longer ticks the bundles", in_loop(TICK_STMT, "")),
    ("the tick moved under the CSI power and degrade gates",
     lambda m, b: (mutate_in(mutate_in(m, SIG_LOOP, TICK_STMT, ""), SIG_LOOP,
                             r"csi::process\(\)\s*;",
                             "{ csi::process(); securacv_csi_modules_tick(); }"), b)),
    # The same gates, spelled other ways.
    ("the tick gated on the power policy",
     in_loop(TICK_STMT, "\n#if FEATURE_POWER_POLICY\n\\1if (pf->csi)\n#endif\n"
                        "\\1securacv_csi_modules_tick();")),
    ("the tick gated on the degrade level",
     in_loop(TICK_STMT, "\n\\1if (dl < DEGRADE_CRITICAL)\n\\1securacv_csi_modules_tick();")),
    ("the tick in a braced block under the power policy",
     in_loop(TICK_STMT, "\n\\1if (pf->csi) { securacv_csi_modules_tick(); }")),
    ("the tick as the else of a gate",
     in_loop(TICK_STMT, "\n\\1if (pf->csi) {} else securacv_csi_modules_tick();")),
    ("the tick behind a short-circuit",
     in_loop(TICK_STMT, "\n\\1(void)(pf->csi && (securacv_csi_modules_tick(), true));")),
    ("the tick under a second preprocessor gate",
     in_loop(TICK_STMT, "\n#if FEATURE_POWER_POLICY\n\\1securacv_csi_modules_tick();\n#endif")),
    ("the tick in the #else branch",
     in_loop(TICK_STMT, "\n#if FEATURE_CSI\n#else\n\\1securacv_csi_modules_tick();\n#endif")),
    ("the tick back under #if FEATURE_CSI",
     in_loop(TICK_STMT, "\n#if FEATURE_CSI\n\\1securacv_csi_modules_tick();\n#endif")),
    ("the tick called twice", in_loop(TICK_STMT, r"\g<0>\n\1securacv_csi_modules_tick();")),
    ("the loop returns before the tick",
     in_loop(TICK_STMT, "\n\\1if (millis() == 0) return;\\g<0>")),
    ("the tick after the egress pump",
     lambda m, b: (mutate_in(mutate_in(m, SIG_LOOP, TICK_BLOCK, ""), SIG_LOOP,
                             r"\n[ \t]*// Create witness records at interval",
                             "\n  securacv_csi_modules_tick();"
                             r"\g<0>"), b)),
    # The bridge.
    ("the bridge's tick closes nothing",
     lambda m, b: (m, mutate_in(b, SIG_TICK, r"csi_bundler_tick\(\)\s*;", ""))),
    ("the feed flushes every window again",
     lambda m, b: (m, mutate_in(b, SIG_FEED, r"(csi_module_tick_all\(f\)\s*;)",
                                r"\1 csi_event_flush_bundles();"))),
    ("the feed closes every bundle through the bundler",
     lambda m, b: (m, mutate_in(b, SIG_FEED, r"(csi_module_tick_all\(f\)\s*;)",
                                r"\1 csi_bundler_flush_all();"))),
]


def self_test(main_src: str, bridge_src: str) -> list[str]:
    problems = []
    for name, mutate in MUTATIONS:
        try:
            m, b = mutate(main_src, bridge_src)
        except AnchorMissing as missing:
            problems.append(f"self-test: mutation '{name}' no longer applies (anchor {missing}) "
                            "— the source changed shape; update this guard's mutations with it")
            continue
        if (m, b) == (main_src, bridge_src):
            problems.append(f"self-test: mutation '{name}' changed nothing")
        elif not check(m, b):
            problems.append(f"self-test: the check did not bite on mutation '{name}'")
    return problems


def main() -> int:
    main_src = (REPO / MAIN_CPP).read_text(encoding="utf-8")
    bridge_src = (REPO / BRIDGE_CPP).read_text(encoding="utf-8")
    errors = check(main_src, bridge_src)
    for err in errors:
        print(f"::error::{err}")
    problems = self_test(main_src, bridge_src)
    for problem in problems:
        print(f"::error::{problem}")
    if errors or problems:
        return 1
    print("CSI bundle tick holds: the canary's loop ticks the bundler once, under "
          "no #if, outside the CSI power and degrade gates, before the event "
          f"egress pump; the bridge's feed closes nothing ({len(MUTATIONS)} mutations refused).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
