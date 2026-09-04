#!/usr/bin/env python3
"""Fail the build on a broken relative link or anchor in ANY tracked .md file.

    python3 scripts/lint_md_links.py          # check the whole tree
    python3 scripts/lint_md_links.py PATH...  # check specific paths

WHY THIS EXISTS BESIDE lint_docs_index.py
-----------------------------------------
The docs index lint makes two promises about docs/ — every doc reachable from
the map, no dead links *in the map and directory READMEs*. That left two
holes this gate closes, both of which shipped real rot:

  1. The ~160 tracked .md files OUTSIDE docs/ (the root README, firmware
     project READMEs, .github/ guides) had no link check at all. Two
     canary-display READMEs pointed one `../` short of the repo root for
     months — six companion-doc links resolved under a `firmware/docs/`
     that has never existed.
  2. Links *inside* non-README docs pages, and every `#anchor` fragment
     everywhere, were never validated. The enclosure README's own table of
     contents carried four stale version anchors (one of them three
     releases behind).

WHAT IT CHECKS
--------------
Every inline markdown link or image `[text](target)` in every .md file:

  - a relative target must resolve to a file or directory that exists AND
    stays inside the repository (a `../` chain that escapes the root is a
    dead link on GitHub even when the path happens to exist on one machine);
  - a leading-/ target resolves against the repo root (how GitHub renders
    it), and must exist there;
  - a `#fragment` on a .md target (or a bare `#fragment`) must match a real
    heading in that file — case-sensitively, because browsers resolve
    fragments case-sensitively — using GitHub's slugification (lowercase,
    spaces to hyphens, punctuation stripped, `-n` suffixes for duplicates)
    or an explicit `id="..."`/`name="..."` HTML anchor.

Skipped, deliberately: external schemes (nothing offline can vouch for
them), links inside fenced code blocks, inline code spans, and HTML
comments — GitHub renders none of those as a link, so a quoted example is
not a promise.
"""
import re
import sys
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
SKIP_DIRS = {".git", "node_modules", "target", "build", "dist", ".venv",
             "__pycache__", ".pytest_cache", "third_party"}

# One level of balanced parentheses is allowed inside the target, so
# `[spec](file_(v2).md)` isn't truncated at the first `)`.
LINK_RE = re.compile(r'!?\[[^\]]*\]\(((?:[^()\s]|\([^()\s]*\))+)(?:\s+"[^"]*")?\)')
FENCE_RE = re.compile(r'^(`{3,}|~{3,})')
# GitHub honors up to three leading spaces on an ATX heading.
HEADING_RE = re.compile(r'^ {0,3}(#{1,6})\s+(.*?)\s*#*\s*$')
HTML_ANCHOR_RE = re.compile(r'(?:id|name)="([^"]+)"')
CODE_SPAN_RE = re.compile(r'`[^`]*`')
HTML_COMMENT_RE = re.compile(r'<!--.*?-->', re.DOTALL)
EXTERNAL = ("http://", "https://", "mailto:", "tel:", "data:", "ftp:")


class FenceTracker:
    """CommonMark fence pairing: a fence closes only on a same-character
    marker at least as long as its opener — a ~~~ line inside a ``` block
    is content, and a ``` line inside a ```` block is too."""

    def __init__(self):
        self.marker = None

    def feed(self, line: str) -> bool:
        """Feed one line; return True if the line is inside (or is) a fence."""
        m = FENCE_RE.match(line.strip())
        if self.marker is None:
            if m:
                self.marker = m.group(1)
                return True
            return False
        if m and m.group(1)[0] == self.marker[0] and \
                len(m.group(1)) >= len(self.marker):
            self.marker = None
        return True


def slugify(heading: str) -> str:
    """GitHub's heading-to-anchor rule, close enough to trust a red result."""
    h = heading.strip().lower()
    h = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', h)  # [text](url) -> text
    h = h.replace('`', '').replace('*', '')
    kept = [ch for ch in h if ch.isalnum() or ch in '-_ ']
    return ''.join(kept).replace(' ', '-')


def anchors_of(path: Path) -> set:
    """Every anchor a #fragment may legitimately point at in this file."""
    result, seen = set(), {}
    fence = FenceTracker()
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return result
    for line in lines:
        if fence.feed(line):
            continue
        m = HEADING_RE.match(line)
        if m:
            slug = slugify(m.group(2))
            n = seen.get(slug, 0)
            seen[slug] = n + 1
            result.add(slug if n == 0 else f"{slug}-{n}")
        for a in HTML_ANCHOR_RE.findall(line):
            result.add(a)
    return result


def md_files(paths):
    for base in paths:
        p = Path(base).resolve()
        if not p.exists():
            print(f"lint_md_links.py: no such path: {base}", file=sys.stderr)
            raise SystemExit(2)
        if p.is_file():
            yield p
            continue
        for f in sorted(p.rglob("*.md")):
            if f.is_file() and not any(d in f.parts for d in SKIP_DIRS):
                yield f


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(ROOT))
    except ValueError:
        return str(path)


def check_file(path: Path, anchor_cache: dict) -> list:
    problems = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return problems

    def anchors(p: Path) -> set:
        key = str(p)
        if key not in anchor_cache:
            anchor_cache[key] = anchors_of(p)
        return anchor_cache[key]

    # HTML comments are not rendered; blank them (preserving line numbers).
    text = HTML_COMMENT_RE.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)),
                               text)
    fence = FenceTracker()
    for i, line in enumerate(text.splitlines(), 1):
        if fence.feed(line):
            continue
        line = CODE_SPAN_RE.sub(lambda m: ' ' * len(m.group(0)), line)
        for m in LINK_RE.finditer(line):
            raw = m.group(1)
            target = raw.strip("<>")
            if target.lower().startswith(EXTERNAL):
                continue
            frag = None
            if "#" in target:
                target, frag = target.split("#", 1)
                frag = unquote(frag)
            if not target:  # bare #anchor into this same file
                if frag and frag not in anchors(path):
                    problems.append(f"{rel(path)}:{i}: "
                                    f"#{frag} — no such heading in this file")
                continue
            target = unquote(target)
            if target.startswith("/"):
                resolved = (ROOT / target.lstrip("/")).resolve()
            else:
                resolved = (path.parent / target).resolve()
            if not resolved.is_relative_to(ROOT):
                problems.append(f"{rel(path)}:{i}: {raw} — resolves outside "
                                f"the repository ({resolved})")
            elif not resolved.exists():
                problems.append(f"{rel(path)}:{i}: {raw} — "
                                f"no such file ({resolved})")
            elif frag and resolved.suffix == ".md":
                if frag not in anchors(resolved):
                    problems.append(f"{rel(path)}:{i}: {raw} — "
                                    f"file exists but #{frag} matches no "
                                    f"heading there")
    return problems


def main() -> int:
    paths = sys.argv[1:] or [ROOT]
    anchor_cache: dict = {}
    offenders = []
    n = 0
    for f in md_files(paths):
        n += 1
        offenders.extend(check_file(f, anchor_cache))
    if offenders:
        print(f"lint_md_links.py: {len(offenders)} broken link(s)/anchor(s) "
              f"across {n} .md files:", file=sys.stderr)
        for o in offenders[:40]:
            print(f"  {o}", file=sys.stderr)
        if len(offenders) > 40:
            print(f"  … and {len(offenders) - 40} more", file=sys.stderr)
        return 1
    print(f"markdown links OK — {n} files, every relative link and anchor "
          f"resolves")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
