#!/usr/bin/env python3
"""Every enclosure preview PNG is referenced, present, and has a render recipe.

    python3 scripts/lint_previews.py          # check the tree (CI, lint.yml)
    python3 scripts/lint_previews.py --list   # print the three sets and exit 0

WHY. docs/hardware/enclosure/README.md embeds one `preview_*.png` per
printable variant (the variant-picker gallery), and gen_enclosures.py reads
those <img> cells into canary-local/devices/enclosures.json / catalog.json /
workshop.json — so the Lab's catalog shows the same pictures. The PNGs are
rendered by the hand-kept `png "preview_*.png" …` list at the end of
docs/hardware/enclosure/render.sh, and enclosure.yml runs `render.sh
--no-png`: nothing in CI ever opened a preview. So three kinds of rot were
free: a README that embeds a PNG nobody rendered (a broken image on the
page and in the Lab), a PNG on disk that nothing embeds any more (an orphan
that keeps shipping), and a README image whose recipe was dropped from
render.sh (it renders once, then never again, and drifts from the CAD
silently). This script closes all three:

  missing   a preview_*.png referenced by the README, another docs/hardware
            page, or a generated catalog, that is not on disk
  orphan    a preview_*.png on disk that no README, docs/hardware page or
            generated catalog references
  unrendered  a README-referenced preview that render.sh has no `png` line
            for (the recipe is the only way it is ever refreshed)
  stale-recipe  a render.sh `png` line whose output nothing references
            (it would render an orphan on the next full render)

NOT in scope: scripts/regen_cad.py's PREVIEW_SETS. Those are the per-part
review renders a CAD change owes the requester (preview_<case>_<set>_<part>_
<view>.png, written to a --previews directory, never committed) — a
different namespace from the README gallery, and one of them landing in
docs/hardware/enclosure/ is exactly the orphan this script flags.

The name pattern is the one the README uses (`preview_<tag>.png`); a
reference is any occurrence of such a name in the file's text, so a link, an
<img src>, a JSON string and a shell `png "…"` line all count.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENC_REL = Path("docs/hardware/enclosure")
README_REL = ENC_REL / "README.md"
RENDER_SH_REL = ENC_REL / "render.sh"
# generated catalogs that carry the README's previews into the Lab
CATALOG_RELS = (
    Path("canary-local/devices/enclosures.json"),
    Path("canary-local/devices/catalog.json"),
    Path("canary-local/devices/workshop.json"),
)

NAME = re.compile(r"\bpreview_[A-Za-z0-9_]+\.png\b")
# render.sh: `png "preview_x.png" -D …` — the only line form the recipe list uses
# (a `(SRC=…; png "…")` one-liner counts too — `[^;\n]` keeps the SRC
# prefix on its own line, or a multi-line subshell's first line would swallow
# every `png` line after it up to the next `;`)
RECIPE = re.compile(r'^\s*(?:\(\s*SRC=[^;\n]*;\s*)?png\s+"(preview_[A-Za-z0-9_]+\.png)"', re.M)


def names_in(text: str) -> set[str]:
    return set(NAME.findall(text))


def referenced(repo: Path) -> dict[str, set[str]]:
    """{preview name: {the files that reference it, repo-relative}} across
    the README, every other docs/hardware/**/*.md, and the catalogs."""
    out: dict[str, set[str]] = {}
    sources = [repo / README_REL]
    sources += sorted(p for p in (repo / "docs/hardware").rglob("*.md") if p != repo / README_REL)
    sources += [repo / rel for rel in CATALOG_RELS]
    for path in sources:
        if not path.exists():
            continue
        for name in names_in(path.read_text(encoding="utf-8", errors="replace")):
            out.setdefault(name, set()).add(str(path.relative_to(repo)))
    return out


def recipes(repo: Path) -> set[str]:
    path = repo / RENDER_SH_REL
    return set(RECIPE.findall(path.read_text(encoding="utf-8"))) if path.exists() else set()


def on_disk(repo: Path) -> set[str]:
    return {p.name for p in (repo / ENC_REL).glob("preview_*.png")}


def problems(repo: Path = REPO) -> list[str]:
    refs = referenced(repo)
    readme = names_in((repo / README_REL).read_text(encoding="utf-8", errors="replace"))
    disk = on_disk(repo)
    recipe = recipes(repo)
    bad: list[str] = []
    for name in sorted(set(refs) - disk):
        bad.append(f"missing: {ENC_REL / name} is referenced by {', '.join(sorted(refs[name]))} "
                   "but is not on disk — render it (render.sh, the `png` list) and commit")
    for name in sorted(disk - set(refs)):
        bad.append(f"orphan: {ENC_REL / name} is on disk but nothing references it — "
                   "embed it in the README or delete it (and its render.sh line)")
    for name in sorted(readme - recipe):
        bad.append(f"unrendered: {README_REL} embeds {name} but {RENDER_SH_REL} has no "
                   f'`png "{name}"` recipe — add one, or the picture can never be refreshed')
    for name in sorted(recipe - set(refs)):
        bad.append(f"stale-recipe: {RENDER_SH_REL} renders {name} but nothing references it — "
                   "embed it or drop the line")
    return bad


def main(argv: list[str]) -> int:
    if "--list" in argv:
        refs, disk, recipe = referenced(REPO), on_disk(REPO), recipes(REPO)
        print(f"referenced ({len(refs)}): {' '.join(sorted(refs))}")
        print(f"on disk    ({len(disk)}): {' '.join(sorted(disk))}")
        print(f"recipes    ({len(recipe)}): {' '.join(sorted(recipe))}")
        return 0
    bad = problems(REPO)
    for b in bad:
        print(f"lint_previews: {b}", file=sys.stderr)
    if bad:
        print(f"lint_previews: {len(bad)} problem(s)", file=sys.stderr)
        return 1
    n = len(on_disk(REPO))
    print(f"lint_previews: OK — {n} previews on disk, all referenced, all with a render.sh recipe")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
