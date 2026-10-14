#!/usr/bin/env python3
"""Hold the CSI chokepoint's commit path to one id space in commit order.

Backlog F46 gave every committed csi_event its id at COMMIT time, from one
allocator (`firmware/common/csi/src/csi_event.cpp`), so bundled and direct
rows share one id space and ids rise in commit order. Home Assistant's
replay gate refuses an `event_id` below the last one it verified, so an id
that reaches the commit hooks (witness chain, SD event log, MQTT) out of
order is a real event thrown away.

Emits come from two tasks in both firmware trees: the loop task, and the
NimBLE host task (ble.scout's arrivals), whose admit can close another
bundle and commit it there. The host tests run single-threaded with the
FreeRTOS locks compiled out (`firmware/tests_host/test_csi_event_id_space.cpp`
covers the ordering on one task), so the cross-task half is a property of
the source shape. This check holds that shape.

## The rules

In `csi_event.cpp`:

1. The commit lock is a RECURSIVE FreeRTOS mutex: on the device branch,
   `struct CommitLock` takes with `xSemaphoreTakeRecursive(` and gives with
   `xSemaphoreGiveRecursive(`, over a mutex made by
   `xSemaphoreCreateRecursiveMutex(`. (A plain mutex deadlocks the first
   time a task that holds it commits again, e.g. set_event_id_floor's
   caller.)
2. `allocate_event_id(` is called exactly once, in `commit_row()`, after a
   `CommitLock <name>;` declared at the top of that body, so the lock is
   held from the id to the end of the commit.
3. The commit hooks, `csi_event_commit_witness(` and
   `csi_event_on_committed(`, are called only in `commit_row()`, after the
   allocation (inside the lock's scope). `csi_event_emit()` commits through
   `commit_row(` and calls neither hook nor the allocator itself;
   `csi_event_commit_bundle_()` commits through `commit_row(`.
4. `g_next_event_id` is written only by `allocate_event_id()`,
   `csi_event_test_reset()` and `csi_event_set_event_id_floor()`, and the
   last takes the commit lock before the ring lock (the one lock order:
   commit, then ring).

In `csi_bundler.cpp`:

5. The bundler never allocates an event id and never calls a commit hook
   (`allocate_event_id(`, `csi_event_on_id_advance(`,
   `csi_event_commit_witness(`, `csi_event_on_committed(`): it commits a
   closed bundle through `csi_event_commit_bundle_(`, from one helper.
6. That helper is never called while the slot lock is held: no block that
   encloses a call declares a `SlotLock` before it. (The commit takes the
   commit lock and runs SD / MQTT hooks; under the slot lock it would also
   stall the httpd task's snapshot.)

## It proves it bites

Each run applies mutations to the two sources in memory and requires the
check to fail on every one; a mutation whose anchor moved fails the run
rather than passing quietly.

Run locally:  python3 firmware/scripts/check_csi_commit_order.py   (repo root)
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
)

REPO = Path(__file__).resolve().parents[2]
EVENT_CPP = "firmware/common/csi/src/csi_event.cpp"
BUNDLER_CPP = "firmware/common/csi/src/csi_bundler.cpp"

SIG_COMMIT_ROW = r"\buint32_t\s+commit_row\s*\([^)]*\)"
SIG_EMIT = r"\buint32_t\s+csi_event_emit\s*\([^)]*\)"
SIG_COMMIT_BUNDLE = r"\buint32_t\s+csi_event_commit_bundle_\s*\([^)]*\)"
SIG_ALLOCATE = r"\buint32_t\s+allocate_event_id\s*\(\s*\)"
SIG_SET_FLOOR = r"\bvoid\s+csi_event_set_event_id_floor\s*\([^)]*\)"
SIG_TEST_RESET = r"\bvoid\s+csi_event_test_reset\s*\(\s*(?:void)?\s*\)"
SIG_COMMIT_LOCK = r"\bstruct\s+CommitLock"
SIG_COMMIT_MUTEX = r"\bSemaphoreHandle_t\s+commit_mutex\s*\(\s*\)"

HOOKS = ("csi_event_commit_witness", "csi_event_on_committed")
LOCK_DECL = r"\bCommitLock\s+\w+\s*;"


def calls(code: str, name: str) -> list[int]:
    """Offsets of every CALL of `name` (a definition or declaration, where the
    name follows a return type, is not a call)."""
    out = []
    for m in re.finditer(r"\b" + name + r"\s*\(", code):
        before = code[max(0, m.start() - 40):m.start()]
        if re.search(r"\b(?:bool|void|uint32_t)\s*$", before):
            continue
        out.append(m.start())
    return out


def one_body(code: str, signature: str, what: str, errors: list[str]) -> tuple[int, int] | None:
    spans = [s for s in bodies(code, signature) if code[s[0]:s[1]].strip()]
    if len(spans) != 1:
        errors.append(f"{what}: expected one definition with a body, found {len(spans)}")
        return None
    return spans[0]


def inside(span: tuple[int, int] | None, pos: int) -> bool:
    return span is not None and span[0] <= pos < span[1]


def check_event(src: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(src)

    # Rule 1: a recursive mutex.
    lock = [s for s in bodies(code, SIG_COMMIT_LOCK) if "xSemaphore" in code[s[0]:s[1]]]
    if len(lock) != 1:
        errors.append(f"{EVENT_CPP}: expected one device-side `struct CommitLock` that takes a "
                      "FreeRTOS mutex")
    else:
        body = code[lock[0][0]:lock[0][1]]
        if "xSemaphoreTakeRecursive(" not in body or "xSemaphoreGiveRecursive(" not in body or \
                re.search(r"\bxSemaphore(?:Take|Give)\s*\(", body):
            errors.append(f"{EVENT_CPP}: CommitLock must take and give a RECURSIVE mutex "
                          "(xSemaphoreTakeRecursive / xSemaphoreGiveRecursive) — a plain one "
                          "deadlocks a task that commits while it holds the lock")
        if "commit_mutex()" not in body:
            errors.append(f"{EVENT_CPP}: CommitLock must lock commit_mutex()")
    maker = one_body(code, SIG_COMMIT_MUTEX, f"{EVENT_CPP}: commit_mutex()", errors)
    if maker is not None:
        body = code[maker[0]:maker[1]]
        if "xSemaphoreCreateRecursiveMutex(" not in body or \
                re.search(r"\bxSemaphoreCreateMutex\s*\(", body):
            errors.append(f"{EVENT_CPP}: commit_mutex() must make its mutex with "
                          "xSemaphoreCreateRecursiveMutex()")

    commit = one_body(code, SIG_COMMIT_ROW, f"{EVENT_CPP}: commit_row()", errors)
    alloc_def = one_body(code, SIG_ALLOCATE, f"{EVENT_CPP}: allocate_event_id()", errors)

    # Rule 2: one allocation, in commit_row, under the lock.
    allocs = calls(code, "allocate_event_id")
    if len(allocs) != 1 or not inside(commit, allocs[0]):
        errors.append(f"{EVENT_CPP}: allocate_event_id() must be called exactly once, in "
                      f"commit_row() (found {len(allocs)} call(s)) — an id taken anywhere else "
                      "can reach the hooks out of order")
    if commit is not None:
        body = code[commit[0]:commit[1]]
        if not re.match(r"\s*(CommitLock\s+\w+\s*;)", body):
            errors.append(f"{EVENT_CPP}: commit_row() must open with `CommitLock <name>;` — "
                          "the lock is held from the id to the last hook")

    # Rule 3: the hooks, only in commit_row, after the allocation.
    for hook in HOOKS:
        sites = calls(code, hook)
        if not sites or any(not inside(commit, p) for p in sites):
            errors.append(f"{EVENT_CPP}: {hook}() must be called only in commit_row(), under "
                          "the commit lock")
        elif allocs and any(p < allocs[0] for p in sites):
            errors.append(f"{EVENT_CPP}: {hook}() runs before the id is taken in commit_row()")
    emit = one_body(code, SIG_EMIT, f"{EVENT_CPP}: csi_event_emit()", errors)
    if emit is not None:
        body = code[emit[0]:emit[1]]
        if "commit_row(" not in body:
            errors.append(f"{EVENT_CPP}: csi_event_emit() must commit through commit_row()")
    bundle = one_body(code, SIG_COMMIT_BUNDLE, f"{EVENT_CPP}: csi_event_commit_bundle_()", errors)
    if bundle is not None and "commit_row(" not in code[bundle[0]:bundle[1]]:
        errors.append(f"{EVENT_CPP}: csi_event_commit_bundle_() must commit through commit_row()")

    # Rule 4: the allocator's writers.
    set_floor = one_body(code, SIG_SET_FLOOR, f"{EVENT_CPP}: csi_event_set_event_id_floor()",
                         errors)
    reset = one_body(code, SIG_TEST_RESET, f"{EVENT_CPP}: csi_event_test_reset()", errors)
    for m in re.finditer(r"\bg_next_event_id\s*(?:\+\+|--|[-+*/|&^]?=(?!=))|(?:\+\+|--)\s*g_next_event_id\b",
                         code):
        pos = m.start()
        if re.match(r"\s*=", code[pos + len("g_next_event_id"):]) and \
                re.search(r"\buint32_t\s+$", code[max(0, pos - 40):pos]):
            continue   # the definition
        if not (inside(alloc_def, pos) or inside(set_floor, pos) or inside(reset, pos)):
            errors.append(f"{EVENT_CPP}: g_next_event_id is written outside allocate_event_id(), "
                          "csi_event_set_event_id_floor() and csi_event_test_reset()")
            break
    if set_floor is not None:
        body = code[set_floor[0]:set_floor[1]]
        c = re.search(LOCK_DECL, body)
        r = re.search(r"\bRingLock\s+\w+\s*;", body)
        if not c or (r and r.start() < c.start()):
            errors.append(f"{EVENT_CPP}: csi_event_set_event_id_floor() moves the allocator: it "
                          "must take the commit lock, before the ring lock (one lock order)")


def check_bundler(src: str, errors: list[str]) -> None:
    code = blank_comments_and_strings(src)
    # Rule 5.
    for name in ("allocate_event_id", "csi_event_on_id_advance") + HOOKS:
        if calls(code, name):
            errors.append(f"{BUNDLER_CPP}: calls {name}() — the bundler never takes an event id "
                          "or runs a commit hook; it commits through csi_event_commit_bundle_()")
    commits = calls(code, "csi_event_commit_bundle_")
    if len(commits) != 1:
        errors.append(f"{BUNDLER_CPP}: expected one call of csi_event_commit_bundle_() (in the "
                      f"commit helper), found {len(commits)}")
        return
    # The helper that holds that call.
    helper = None
    for m in re.finditer(r"\bvoid\s+(\w+)\s*\([^)]*\)\s*\{", code):
        spans = bodies(code, r"\bvoid\s+" + m.group(1) + r"\s*\([^)]*\)")
        if any(a <= commits[0] < b for a, b in spans):
            helper = m.group(1)
            break
    if helper is None:
        errors.append(f"{BUNDLER_CPP}: csi_event_commit_bundle_() must be called from a helper")
        return
    # Rule 6: never under the slot lock.
    for pos in calls(code, helper):
        if under_slot_lock(code, pos):
            errors.append(f"{BUNDLER_CPP}: {helper}() is called while the slot lock is held — "
                          "a commit takes the commit lock and runs the SD / MQTT hooks, never "
                          "under the slot mutex")
            break


def under_slot_lock(code: str, pos: int) -> bool:
    """True when a block enclosing `pos` declares a SlotLock before it."""
    depth = 0
    i = pos - 1
    while i >= 0:
        ch = code[i]
        if ch == "}":
            depth += 1
        elif ch == "{":
            if depth == 0:
                # `i` opens a block that encloses pos: scan its direct text.
                if slot_lock_at_level(code, i + 1, pos):
                    return True
            else:
                depth -= 1
        i -= 1
    return False


def slot_lock_at_level(code: str, start: int, end: int) -> bool:
    depth = 0
    for m in re.finditer(r"[{}]|\bSlotLock\s+\w+\s*;", code[start:end]):
        tok = m.group(0)
        if tok == "{":
            depth += 1
        elif tok == "}":
            depth -= 1
        elif depth == 0:
            return True
    return False


def check(event_src: str, bundler_src: str) -> list[str]:
    errors: list[str] = []
    check_event(event_src, errors)
    check_bundler(bundler_src, errors)
    return errors


# ── Self-test: mutations the check must refuse ───────────────────────────


def mutate_in(src: str, signature: str, pattern: str, repl: str, count: int = 1) -> str:
    """Apply a regex substitution inside the (raw) body of one definition."""
    code = blank_comments_and_strings(src)
    spans = [s for s in bodies(code, signature) if code[s[0]:s[1]].strip()]
    if len(spans) != 1:
        raise AnchorMissing(f"definition {signature!r}")
    start, end = spans[0]
    new_body, n = re.subn(pattern, repl, src[start:end], count=count, flags=re.S)
    if n == 0:
        raise AnchorMissing(pattern)
    return src[:start] + new_body + src[end:]


def mutate_file(src: str, pattern: str, repl: str, count: int = 1) -> str:
    new, n = re.subn(pattern, repl, src, count=count, flags=re.S)
    if n == 0:
        raise AnchorMissing(pattern)
    return new


Mutation = Callable[[str, str], "tuple[str, str]"]

MUTATIONS: list[tuple[str, Mutation]] = [
    ("the commit lock is a plain mutex",
     lambda e, b: (mutate_file(mutate_file(e, r"xSemaphoreTakeRecursive\(", "xSemaphoreTake("),
                               r"xSemaphoreGiveRecursive\(", "xSemaphoreGive("), b)),
    ("the commit lock's mutex is not recursive",
     lambda e, b: (mutate_file(e, r"xSemaphoreCreateRecursiveMutex\(\)", "xSemaphoreCreateMutex()"),
                   b)),
    ("commit_row() takes no lock",
     lambda e, b: (mutate_in(e, SIG_COMMIT_ROW, r"CommitLock\s+_commit\s*;", ""), b)),
    ("commit_row() takes the id before the lock",
     lambda e, b: (mutate_in(e, SIG_COMMIT_ROW,
                             r"CommitLock\s+_commit\s*;(\s*const\s+uint32_t\s+event_id\s*=\s*allocate_event_id\(\)\s*;)",
                             r"\1 CommitLock _commit;"), b)),
    ("emit() takes its own id and runs the hook itself",
     lambda e, b: (mutate_in(e, SIG_EMIT,
                             r"return\s+commit_row\([^;]*;",
                             "const uint32_t id = allocate_event_id(); "
                             "csi_event_on_committed(id, module_id, type_name, v.category, "
                             "decl->privacy, &v); return id;"), b)),
    ("a hook runs before the id is taken",
     lambda e, b: (mutate_in(e, SIG_COMMIT_ROW,
                             r"(CommitLock\s+_commit\s*;)",
                             r"\1 csi_event_on_committed(0, module_id, type_name, v->category, "
                             r"privacy, v);"), b)),
    ("the bundle commit bypasses commit_row()",
     lambda e, b: (mutate_in(e, SIG_COMMIT_BUNDLE, r"return\s+commit_row\([^;]*;",
                             "csi_event_on_committed(1, module_id, type_name, values->category, "
                             "privacy, values); return 1;"), b)),
    ("set_event_id_floor() moves the allocator without the commit lock",
     lambda e, b: (mutate_in(e, SIG_SET_FLOOR, r"CommitLock\s+_commit\s*;", ""), b)),
    ("set_event_id_floor() takes the ring lock first",
     lambda e, b: (mutate_in(e, SIG_SET_FLOOR,
                             r"(CommitLock\s+_commit\s*;)(.*?)(RingLock\s+_lock\s*;)",
                             r"\3\2\1"), b)),
    ("a second writer of the allocator",
     lambda e, b: (mutate_file(e, r"(return\s+g_next_event_id\s*;)",
                               r"g_next_event_id++; \1"), b)),
    ("the bundler runs the commit hook itself",
     lambda e, b: (e, mutate_in(b, r"\bvoid\s+commit_closed\s*\([^)]*\)",
                                r"\(void\)csi_event_commit_bundle_\([^;]*;",
                                "csi_event_on_committed(c->handle, c->module_id, c->type_name, "
                                "c->values.category, c->privacy, &c->values);"))),
    ("the bundler commits under its slot lock",
     lambda e, b: (e, mutate_in(b, r"\bvoid\s+csi_bundler_flush_key\s*\([^)]*\)",
                                r"if \(s\) close_slot_locked\(s, pending, &npending\);",
                                "if (s) { close_slot_locked(s, pending, &npending); "
                                "commit_closed(&pending[0]); }"))),
    ("the bundler commits under its slot lock, in admit",
     lambda e, b: (e, mutate_in(b, r"\bcsi_bundler_outcome_t\s+csi_bundler_admit\s*\([^)]*\)",
                                r"(expire_overdue\(now_ms, pending, &npending\);)",
                                r"\1 for (size_t k = 0; k < npending; ++k) "
                                r"commit_closed(&pending[k]);"))),
]


def self_test(event_src: str, bundler_src: str) -> list[str]:
    problems = []
    for name, mutate in MUTATIONS:
        try:
            e, b = mutate(event_src, bundler_src)
        except AnchorMissing as missing:
            problems.append(f"self-test: mutation '{name}' no longer applies (anchor {missing}) — "
                            "the source changed shape; update this guard's mutations with it")
            continue
        if (e, b) == (event_src, bundler_src):
            problems.append(f"self-test: mutation '{name}' changed nothing")
        elif not check(e, b):
            problems.append(f"self-test: the check did not bite on mutation '{name}'")
    return problems


def main() -> int:
    event_src = (REPO / EVENT_CPP).read_text(encoding="utf-8")
    bundler_src = (REPO / BUNDLER_CPP).read_text(encoding="utf-8")
    errors = check(event_src, bundler_src)
    for err in errors:
        print(f"::error::{err}")
    problems = self_test(event_src, bundler_src)
    for problem in problems:
        print(f"::error::{problem}")
    if errors or problems:
        return 1
    print(f"CSI commit order holds: one allocator, called once under a recursive commit lock "
          f"that spans the hooks; the bundler commits through it, outside its slot lock "
          f"({len(MUTATIONS)} mutations refused).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
