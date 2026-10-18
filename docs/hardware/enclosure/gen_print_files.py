#!/usr/bin/env python3
"""gen_print_files.py — print-ready STLs for the website, with their provenance.

The in-development cases have no committed STL (dev_*.stl is gitignored on
purpose, and the figures' evidence ladder reads a committed STL as a shipping
part). People still want to print one NOW, from the site, and know what they
are printing. So this renders the parts of one case at full quality straight
into the WEBSITE checkout — never into this tree — beside a manifest that
says exactly what the bytes are:

    DIR/print/<name>.stl              the parts (binary STL, millimeters)
    DIR/print/print-files.json        version, source commit + date, the
                                      .scad's sha256, each file's bytes,
                                      sha256, bounding box and triangle count

The case's version string is read off its own header (the "(v0.3-dev)" on
line 2), never typed here. The status line says "in development — not
print-validated" until the README's released table says otherwise.

OpenSCAD's STL bytes are not deterministic, so --check cannot re-render and
compare. It checks what CAN rot instead: the manifest's scad sha256 against
the .scad as it is now (a case edit without a re-render fails loudly), and
every listed file's bytes + sha256 against what sits in DIR/print (a file
replaced by hand fails too). tests/print-files.test.mjs on the website side
holds the same two facts against the carried scad copy.

Run from the repo root:
    python3 docs/hardware/enclosure/gen_print_files.py --site DIR          # render + write
    python3 docs/hardware/enclosure/gen_print_files.py --site DIR --check  # the gate
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE))
import scad_probe  # noqa: E402

# (scad, part -D value, file stem, title, how it prints) — one case today
JOBS = {
    "canary_watch_station.scad": [
        ("drum", "canary_watch_station_drum", "Display drum",
         "open face up, as modeled — no supports"),
        ("bezel", "canary_watch_station_bezel", "Snap bezel",
         "face down, as modeled — no supports"),
        ("stand", "canary_watch_station_stand", "Dock stand",
         "base down, as modeled — no supports"),
    ],
}
MANIFEST = "print-files.json"
STATUS_IN_DEV = "in development — not print-validated"


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stl_stats(path: Path) -> tuple[int, list[float]]:
    """(triangle count, [x, y, z] extents in mm) of a binary STL."""
    data = path.read_bytes()
    n = struct.unpack_from("<I", data, 80)[0]
    lo = [float("inf")] * 3
    hi = [float("-inf")] * 3
    off = 84
    for _ in range(n):
        v = struct.unpack_from("<12f", data, off)
        for k in range(3):
            for j in (3 + k, 6 + k, 9 + k):
                lo[k] = min(lo[k], v[j])
                hi[k] = max(hi[k], v[j])
        off += 50
    return n, [round(top - bot, 3) for bot, top in zip(lo, hi)]


def version_of(scad: Path) -> str:
    """The case's own version tag, from its header's "(vX.Y[-dev])"."""
    head = scad.read_text(encoding="utf-8").splitlines()[:3]
    for line in head:
        m = re.search(r"\((v\d+\.\d+(?:-\w+)?)\)", line)
        if m:
            return m.group(1)
    sys.exit(f"gen_print_files: {scad.name} carries no (vX.Y) tag in its header")


def git(*args: str) -> str:
    return subprocess.check_output(["git", "-C", str(REPO), *args], text=True).strip()


def openscad_version() -> str:
    # The binary scad_probe.render() just used ($OPENSCAD, else `openscad`),
    # so the version recorded is the version that rendered the files.
    out = subprocess.run([scad_probe.OPENSCAD, "--version"], capture_output=True, text=True)
    return (out.stdout + out.stderr).strip().replace("OpenSCAD version ", "")


def build(site: Path) -> dict:
    out_dir = site / "print"
    out_dir.mkdir(parents=True, exist_ok=True)
    entries = []
    sources = []
    for scad_name, parts in JOBS.items():
        scad = HERE / scad_name
        src = {
            "scad": f"docs/hardware/enclosure/{scad_name}",
            "scad_sha256": sha256_of(scad),
            "version": version_of(scad),
            "status": STATUS_IN_DEV,
        }
        sources.append(src)
        for part, stem, title, how in parts:
            out = out_dir / f"{stem}.stl"
            print(f"render: {out.name}")
            try:
                scad_probe.render(scad, {"part": f'"{part}"'}, keep=out, label=out.name)
            except scad_probe.ProbeError as e:
                sys.exit(f"gen_print_files: {e}")
            tris, size = stl_stats(out)
            entries.append({
                "file": out.name,
                "scad": src["scad"],
                "part": part,
                "title": title,
                "print": how,
                "bytes": out.stat().st_size,
                "sha256": sha256_of(out),
                "triangles": tris,
                "size_mm": size,
            })
    return {
        "comment": ("GENERATED by docs/hardware/enclosure/gen_print_files.py --site in kmay89/securaCV — "
                    "do not edit. The STLs beside this file are the case's parts rendered at full quality "
                    "from the commit named here; sha256s are of the bytes as written."),
        "generated_at": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "openscad": openscad_version(),
        "repo": "kmay89/securaCV",
        "commit": git("rev-parse", "HEAD"),
        "commit_date": git("log", "-1", "--format=%cI"),
        "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "sources": sources,
        "files": entries,
    }


def check(site: Path) -> list[str]:
    out_dir = site / "print"
    mpath = out_dir / MANIFEST
    if not mpath.exists():
        return [f"{mpath}: missing — run gen_print_files.py --site {site}"]
    m = json.loads(mpath.read_text(encoding="utf-8"))
    bad = []
    by_scad = {s["scad"]: s for s in m.get("sources", [])}
    for scad_name, parts in JOBS.items():
        rel = f"docs/hardware/enclosure/{scad_name}"
        src = by_scad.get(rel)
        if not src:
            bad.append(f"{MANIFEST}: no source entry for {rel}")
            continue
        now = sha256_of(HERE / scad_name)
        if src.get("scad_sha256") != now:
            bad.append(f"{MANIFEST}: {rel} moved since the print files were rendered "
                       f"(manifest {src.get('scad_sha256', '')[:12]}, tree {now[:12]}) — re-run --site")
        ver = version_of(HERE / scad_name)
        if src.get("version") != ver:
            bad.append(f"{MANIFEST}: {rel} is {ver}, the print files say {src.get('version')}")
        listed = {e["file"] for e in m.get("files", []) if e.get("scad") == rel}
        for part, stem, _, _ in parts:
            if f"{stem}.stl" not in listed:
                bad.append(f"{MANIFEST}: {stem}.stl is not listed")
    for e in m.get("files", []):
        f = out_dir / e["file"]
        if not f.exists():
            bad.append(f"{f}: listed but missing")
            continue
        if f.stat().st_size != e["bytes"] or sha256_of(f) != e["sha256"]:
            bad.append(f"{f}: bytes differ from the manifest — replaced by hand? re-run --site")
    return bad


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--site", metavar="DIR", type=Path, required=True,
                    help="the website checkout; writes DIR/print/")
    ap.add_argument("--check", action="store_true", help="verify DIR/print against this tree; write nothing")
    args = ap.parse_args(argv)
    site = args.site.resolve()
    if args.check:
        bad = check(site)
        for b in bad:
            print(f"::error::{b}")
        if bad:
            return 1
        print(f"print files OK: {site / 'print'} matches this tree")
        return 0
    m = build(site)
    (site / "print" / MANIFEST).write_text(json.dumps(m, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {site / 'print' / MANIFEST} — {len(m['files'])} files, "
          f"{', '.join(s['version'] for s in m['sources'])} @ {m['commit'][:9]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
