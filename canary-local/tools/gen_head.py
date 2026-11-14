#!/usr/bin/env python3
"""canary-local/tools/gen_head.py — the derived half of every Lab page's <head>.

The Lab is a website: twenty-six pages on GitHub Pages that people link, share
and search for. Until 2026-11 none of them had a canonical link or a share
card, none said "SecuraCV" in its title (every one ended "canary.local", the
Lab's nickname and also a Canary's hostname), and twenty-one carried a
description too long for a search result to show — up to 653 characters, most
ending in a claim the claims audit had retired on the main site
("nothing phones anywhere").

The website solved the same problem by splitting each head in two
(securacv_website scripts/make-head.mjs), and this is that split for the Lab:

  * HAND-WRITTEN, in the page: <title> and <meta name="description">. They are
    copy. This tool only checks them: a title names the bench first and ends
    with the Lab's name; a description fits a result and makes no claim about
    what the network does not do.
  * DERIVED, stamped between the two markers below: the canonical link, the
    Open Graph and card tags, and where the page sits. All of it is a function
    of the page's file name and those two lines.

    python3 canary-local/tools/gen_head.py           # stamp every page
    python3 canary-local/tools/gen_head.py --check   # CI: name what is stale

Only meta and link tags are written: nothing here is a script or a style, so
the Content-Security-Policy line tools/gen_csp.py owns is unaffected, and the
share image is a URL a link preview fetches, never the page.
"""
from __future__ import annotations

import argparse
import html
import re
import sys
from pathlib import Path

LAB = Path(__file__).resolve().parents[1]
BASE = "https://kmay89.github.io/securaCV/canary-local/"
SITE = "https://securacv.com"
SITE_NAME = "SecuraCV Lab"

# The share card is the website's (its scripts/make-share-card.mjs renders it,
# and its tests hold these dimensions to the JPEG). One card for the family.
CARD = {
    "url": f"{SITE}/share-card.jpg",
    "width": 2400,
    "height": 1260,
    "alt": "Canary, SecuraCV's yellow bird mascot, holding a notepad beside the "
           "words: Records what happened. Never who. An open-source home camera "
           "with no cloud, no subscription, and no face recognition.",
}

BEGIN = ("<!-- head:derived — written by canary-local/tools/gen_head.py from this "
         "page's own title and description. Edit those, then re-run it. -->")
END = "<!-- /head:derived -->"

# A page whose canonical address is somewhere else. index.html is a redirect
# to the shell, so it points at the shell; nothing else here is a duplicate.
CANONICAL = {"index.html": "lab.html"}

TITLE_RE = re.compile(r"^([ \t]*)<title>([^<]*)</title>[ \t]*$", re.M)
DESC_RE = re.compile(r'^([ \t]*)<meta name="description" content="([^"]*)"\s*/?>[ \t]*$', re.M)
BLOCK_RE = re.compile(rf"^[ \t]*{re.escape(BEGIN)}.*?{re.escape(END)}[ \t]*\n", re.S | re.M)
# Tags the block owns, wherever a hand-written head left one.
OWNED = [
    re.compile(r'^[ \t]*<link rel="canonical" href="[^"]*"\s*/?>[ \t]*\n', re.M),
    re.compile(r'^[ \t]*<meta property="og:[a-z:_]+" content="[^"]*"\s*/?>[ \t]*\n', re.M),
    re.compile(r'^[ \t]*<meta name="twitter:[a-z:]+" content="[^"]*"\s*/?>[ \t]*\n', re.M),
]

# What a description may not say. The claims audit retired "nothing phones
# home / anywhere" on the website because devices and pages do make requests
# (updates, the device list); the Lab's honest line is what the page does with
# what you give it: it runs in your browser and nothing is uploaded.
RETIRED = re.compile(r"phones? (?:home|anywhere)|nothing leaves", re.I)
TITLE_MAX, DESC_MIN, DESC_MAX = 65, 70, 160


def pages() -> list[Path]:
    """Every page the Lab publishes at its top level. witness/witness.html is
    the website's own Witness Wall page (its canonical is on securacv.com) and
    the emulator harness is a developer fixture; neither is a Lab page."""
    return sorted(LAB.glob("*.html"))


def attr(s: str) -> str:
    """Text for a double-quoted attribute: &, <, > and " escaped, an
    apostrophe left as it is (it needs no entity there, and the pages' own
    descriptions write it plainly)."""
    return html.escape(html.unescape(s), quote=False).replace('"', "&quot;")


def read_copy(text: str, name: str) -> tuple[str, str, str]:
    head = text[: text.index("</head>")] if "</head>" in text else text
    t, d = TITLE_RE.search(head), DESC_RE.search(head)
    if not t:
        raise SystemExit(f"gen_head.py: {name}: no <title> on its own line in <head>")
    if not d:
        raise SystemExit(f'gen_head.py: {name}: no <meta name="description"> on its own line in <head>')
    return html.unescape(t.group(2)), html.unescape(d.group(2)), d.group(1)


def block(name: str, title: str, desc: str, indent: str) -> str:
    url = BASE + CANONICAL.get(name, name)
    share_title = title.removesuffix(f" — {SITE_NAME}").removesuffix(" — SecuraCV")
    lines = [
        BEGIN,
        f'<link rel="canonical" href="{url}" />',
        f'<meta property="og:site_name" content="{SITE_NAME}" />',
        '<meta property="og:locale" content="en_US" />',
        '<meta property="og:type" content="website" />',
        f'<meta property="og:url" content="{url}" />',
        f'<meta property="og:title" content="{attr(share_title)}" />',
        f'<meta property="og:description" content="{attr(desc)}" />',
        f'<meta property="og:image" content="{CARD["url"]}" />',
        f'<meta property="og:image:width" content="{CARD["width"]}" />',
        f'<meta property="og:image:height" content="{CARD["height"]}" />',
        f'<meta property="og:image:alt" content="{attr(CARD["alt"])}" />',
        '<meta name="twitter:card" content="summary_large_image" />',
        f'<meta name="twitter:image" content="{CARD["url"]}" />',
        END,
    ]
    return "\n".join(indent + line for line in lines) + "\n"


def stamp(text: str, name: str) -> str:
    """The page with its derived block current, directly under the description."""
    title, desc, indent = read_copy(text, name)
    cut = text.index("</head>")
    head, rest = text[:cut], text[cut:]
    head = BLOCK_RE.sub("", head)
    for pattern in OWNED:
        head = pattern.sub("", head)
    d = DESC_RE.search(head)
    at = d.end() + 1  # past the description line's newline
    return head[:at] + block(name, title, desc, indent) + head[at:] + rest


def copy_problems(name: str, title: str, desc: str) -> list[str]:
    out = []
    if "SecuraCV" not in title:
        out.append(f'{name}: <title> "{title}" does not say SecuraCV — a search result '
                   "and a browser tab show the title alone")
    if not (title.endswith(f" — {SITE_NAME}") or title.endswith(" — SecuraCV")):
        out.append(f'{name}: <title> must end " — {SITE_NAME}" (or " — SecuraCV" on the shell)')
    if len(title) > TITLE_MAX:
        out.append(f"{name}: <title> is {len(title)} characters; a result shows about {TITLE_MAX}")
    if not DESC_MIN <= len(desc) <= DESC_MAX:
        out.append(f"{name}: description is {len(desc)} characters; a result shows "
                   f"{DESC_MIN}–{DESC_MAX}, whole")
    if RETIRED.search(desc):
        out.append(f"{name}: description makes a retired claim ({RETIRED.search(desc).group(0)!r}); "
                   'say what the page does: "runs in your browser; nothing is uploaded"')
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="write nothing; name what is stale or malformed")
    args = ap.parse_args(argv)

    stale, problems, seen_titles, seen_descs = [], [], {}, {}
    for path in pages():
        text = path.read_text(encoding="utf-8")
        title, desc, _ = read_copy(text, path.name)
        problems += copy_problems(path.name, title, desc)
        for seen, value, what in ((seen_titles, title, "title"), (seen_descs, desc, "description")):
            if value in seen:
                problems.append(f"{path.name}: same {what} as {seen[value]} — a result cannot tell them apart")
            seen.setdefault(value, path.name)
        fresh = stamp(text, path.name)
        if fresh != text:
            stale.append(path.name)
            if not args.check:
                path.write_text(fresh, encoding="utf-8")
    if problems:
        print("gen_head.py: the hand-written half of these heads needs fixing:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
    if args.check and stale:
        print(f"gen_head.py: the derived <head> block is stale in: {', '.join(stale)}\n"
              "    python3 canary-local/tools/gen_head.py", file=sys.stderr)
    if problems or (args.check and stale):
        return 1
    print(f"{'stamped' if stale else 'Lab heads OK —'} {len(stale) or len(pages())} "
          f"page{'s' if (len(stale) or len(pages())) != 1 else ''}"
          + ("" if stale else ": canonical, share tags, titles and descriptions"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
