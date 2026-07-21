#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_kernel_status.py — make docs/witness-kernel.html's Implementation status
grid a *computed* artifact of the code, so it can't silently go stale.

The page promises "every claim can be verified by inspecting the code." This
generator does exactly that: for each status tile it inspects the actual source
(structs, functions, Cargo features) and derives done / in-progress / planned,
then re-stamps the `<div class="status-grid">` block (tiles sorted
done → wip → planned). CI re-runs it and `git diff --exit-code`s the HTML — so
if a PR ships a feature (or removes one) the grid flips with it, and if the HTML
overstates what the code does, the build fails until it's regenerated.

Status rules (grounded in the tree):
  done    — the implementing code is present AND on the default build path
  wip     — the code exists but is behind a NON-default cargo feature
  planned — no implementing code (or a different mechanism entirely)

Run:  python3 tools/gen_kernel_status.py
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PAGE = REPO / "docs/witness-kernel.html"

_CACHE: dict = {}


def die(msg: str) -> None:
    print(f"gen_kernel_status.py: ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def read(rel: str) -> str:
    p = REPO / rel
    if p not in _CACHE:
        _CACHE[p] = p.read_text(encoding="utf-8", errors="replace") if p.exists() else ""
    return _CACHE[p]


def exists(rel: str) -> bool:
    return (REPO / rel).exists()


def has(rel: str, *needles: str) -> bool:
    t = read(rel)
    return bool(t) and all(n in t for n in needles)


def cargo_default_features() -> set:
    m = re.search(r"^\s*default\s*=\s*\[([^\]]*)\]", read("Cargo.toml"), re.M)
    return set(re.findall(r'"([^"]+)"', m.group(1))) if m else set()


def cargo_has_dep(*names: str) -> bool:
    t = read("Cargo.toml")
    return any(re.search(r"^\s*" + re.escape(n) + r"\s*=", t, re.M) for n in names)


# --------------------------------------------------------------------------- #
# per-tile detection — each returns "done" | "wip" | "planned"
# --------------------------------------------------------------------------- #

def _derives_on(src: str, struct_name: str) -> str:
    """The `#[...]` attribute lines immediately above a struct (its derives).

    Only real attribute lines — NOT doc comments, which for RawFrame literally
    read 'no Clone, no AsRef<[u8]>' and would otherwise trip a naive scan.
    """
    lines = src.splitlines()
    for i, ln in enumerate(lines):
        if re.match(r"\s*(pub\s+)?struct\s+" + re.escape(struct_name) + r"\b", ln):
            block, j = [], i - 1
            while j >= 0 and lines[j].strip() != "":
                s = lines[j].strip()
                if s.startswith("#["):
                    block.append(s)
                j -= 1
            return "\n".join(block)
    return ""


def _inherent_impl(src: str, name: str) -> str:
    """The bodies of `impl <name> { ... }` blocks (inherent methods, NOT trait impls),
    brace-matched — so a byte-getter check is scoped to RawFrame's own methods and
    doesn't trip on getters that legitimately live on other types in the file."""
    out = []
    for m in re.finditer(r"\bimpl\s+" + re.escape(name) + r"\s*\{", src):
        depth, j = 0, m.end() - 1
        while j < len(src):
            if src[j] == "{":
                depth += 1
            elif src[j] == "}":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append(src[m.end() - 1:j + 1])
    return "\n".join(out)


def _raw_frame_export_hole(src: str) -> bool:
    """True if RawFrame gained a raw-export hole (breaks Invariant I: No Raw Export).

    The Frame-isolation tile's whole meaning is the *type-level* guarantee that raw
    bytes can't escape — so a change that adds Clone, AsRef<[u8]>, a byte-exposing
    trait, or a public byte getter must NOT keep the tile green just because the
    isolation machinery (export_for_vault, BreakGlassToken) is still present.
    """
    if re.search(r"impl\s+Clone\s+for\s+RawFrame\b", src):
        return True
    if re.search(r"\bClone\b", _derives_on(src, "RawFrame")):
        return True
    forbidden_traits = [
        r"impl\s+AsRef<\s*\[u8\]\s*>\s+for\s+RawFrame\b",
        r"impl\s+(?:std::|core::)?ops::Deref\s+for\s+RawFrame\b",
        r"impl\s+Deref\s+for\s+RawFrame\b",
        r"impl\s+(?:std::|core::)?borrow::Borrow<\s*\[u8\]\s*>\s+for\s+RawFrame\b",
        r"impl\s+Borrow<\s*\[u8\]\s*>\s+for\s+RawFrame\b",
    ]
    if any(re.search(p, src) for p in forbidden_traits):
        return True
    # a public byte accessor on RawFrame itself (export_for_vault, the ONE sanctioned
    # path, is deliberately not in this name set) — scoped to RawFrame's inherent impl
    if re.search(r"pub\s+fn\s+(?:as_bytes|as_slice|bytes|raw_bytes|data)\s*\([^)]*\)\s*->\s*&?\s*(?:\[u8\]|Vec<u8>)",
                 _inherent_impl(src, "RawFrame")):
        return True
    return False


def st_frame_isolation():
    src = read("src/frame.rs")
    if not all(x in src for x in ("struct RawFrame", "fn export_for_vault", "BreakGlassToken")):
        return "planned"
    # present, but a raw-export hole means the type-level guarantee is broken —
    # don't let the public grid read ✓ for exactly the privacy regression this
    # tile represents.
    return "wip" if _raw_frame_export_hole(src) else "done"


def st_hash_log():
    return "done" if has("src/log/mod.rs", "fn hash_entry") and exists("src/bin/log_verify.rs") else "planned"


def st_quorum():
    ok = has("src/break_glass/core.rs", "struct QuorumPolicy",
             "count_valid_distinct_approvals", "trustees_used.len() >= policy.n")
    return "done" if ok else "planned"


def st_contract():
    return "done" if has("src/lib.rs", "ContractEnforcer") and exists("spec/event_contract.md") else "planned"


def st_signatures():
    ok = cargo_has_dep("ed25519-dalek") and has("src/crypto/signatures.rs", "sign_with_domain", "verify_with_domain")
    return "done" if ok else "planned"


def st_vault_envelopes():
    ok = (has("src/vault/crypto.rs", "AEAD_ALG_CHACHA20POLY1305", "fn seal_v2")
          and has("src/vault/format.rs", 'b"VLT2"')
          and has("src/bin/witnessd.rs", "seal_frame"))
    return "done" if ok else "planned"


def st_rtsp():
    # real GStreamer/FFmpeg decoders exist behind cargo features; the status
    # turns on whether they're in the DEFAULT build (else a plain build gets
    # only the synthetic stub — honestly "in progress").
    real = has("src/ingest/rtsp.rs", 'feature = "rtsp-ffmpeg"') or has("src/ingest/rtsp.rs", 'feature = "rtsp-gstreamer"')
    if not real:
        return "planned"
    return "done" if ({"rtsp-ffmpeg", "rtsp-gstreamer"} & cargo_default_features()) else "wip"


def st_wasm():
    # the shipping module sandbox is seccomp process-isolation, not WASM; a real
    # WASM sandbox would pull in a wasm runtime dependency.
    if not cargo_has_dep("wasmtime", "wasmer", "wasmi"):
        return "planned"
    wired = has("src/module_runtime/sandbox.rs", "wasm") or has("src/module_runtime/mod.rs", "wasm")
    return "done" if wired else "wip"


# authored (logical) order; the emit step re-sorts done → wip → planned
TILES = [
    ("Frame isolation types", st_frame_isolation),
    ("Hash-chained event log", st_hash_log),
    ("Break-glass quorum", st_quorum),
    ("Event contract enforcement", st_contract),
    ("Cryptographic signatures", st_signatures),
    ("Encrypted vault envelopes", st_vault_envelopes),
    ("RTSP video ingestion", st_rtsp),
    ("WASM module sandboxing", st_wasm),
]

GLYPH = {"done": "✓", "wip": "◐", "planned": "○"}
RANK = {"done": 0, "wip": 1, "planned": 2}
# reveal-delay by final position (pairs), matching the page's animation cadence
REVEAL = [
    "reveal", "reveal reveal-delay-1", "reveal reveal-delay-1",
    "reveal reveal-delay-2", "reveal reveal-delay-2",
    "reveal reveal-delay-3", "reveal reveal-delay-3", "reveal reveal-delay-4",
]

# --------------------------------------------------------------------------- #

def main():
    if not PAGE.exists():
        die(f"page missing: {PAGE.relative_to(REPO)}")
    if not exists("Cargo.toml") or not exists("src/lib.rs"):
        die("kernel sources not found (Cargo.toml / src/lib.rs) — run from the repo root")

    computed = [(label, fn(), i) for i, (label, fn) in enumerate(TILES)]
    ordered = sorted(computed, key=lambda t: (RANK[t[1]], t[2]))

    items = []
    for pos, (label, status, _idx) in enumerate(ordered):
        items.append(
            f'          <div class="status-item {REVEAL[pos]}">\n'
            f'            <div class="status-icon {status}">{GLYPH[status]}</div>\n'
            f'            <h4>{label}</h4>\n'
            f'          </div>'
        )
    block = '        <div class="status-grid">\n' + "\n".join(items) + '\n        </div>'

    html = PAGE.read_text(encoding="utf-8")
    pat = re.compile(r'        <div class="status-grid">\n.*?\n        </div>', re.DOTALL)
    if not pat.search(html):
        die("could not find the <div class=\"status-grid\"> block in witness-kernel.html")
    new_html = pat.sub(lambda _m: block, html, count=1)
    PAGE.write_text(new_html, encoding="utf-8")

    tally = {}
    for _l, s, _i in computed:
        tally[s] = tally.get(s, 0) + 1
    print("stamped docs/witness-kernel.html status grid: "
          + ", ".join(f"{tally.get(k, 0)} {k}" for k in ("done", "wip", "planned")))
    for label, status, _i in ordered:
        print(f"  {GLYPH[status]} {label}  ({status})")


if __name__ == "__main__":
    main()
