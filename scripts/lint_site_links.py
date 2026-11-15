#!/usr/bin/env python3
"""Every link from this repository to securacv.com opens something that exists.

The Lab, the desktop apps, the iPhone and Apple TV apps, the Home Assistant
integration, the back office's mail and the firmware (a Canary draws help QR
codes on its own screen) all link pages on the website, which lives in another
repository. Nothing told this one when a page there moved. A census in 2026-11
found what that produces:

  - the Lab's footer opened securacv.com/site-map.html, which was never a page;
  - the back office's welcome mail linked /faq, a 404;
  - the desktop app's atlas opened /#about, an anchor the home page lacks;
  - the Lab's live bar and its static bar sent "Help" to two different pages.

The website states every address it answers in its upstream-contract.json —
its routes, the short links that redirect, the files it publishes, and each
page's anchors — and `gen_builder_manifest.py --site` carries that here as
canary-local/devices/site_contract.json. This lint reads the carried copy and
checks three kinds of link against it:

  1. a literal URL, https://securacv.com/<path>[#anchor], in any tracked text
     file;
  2. an origin constant joined to a path in code, SITE_ORIGIN + "/help" (the
     Lab, the desktop apps), which no URL regex sees;
  3. a site-relative href in the Lab's manifest (canary-local/build-line.json:
     the stages' site handoffs, siteNav, footerNav), which the Lab's renderers
     prefix with the origin.

A path must be a route or a published file; an anchor, when the contract knows
the page's anchors, must be one of them. A page's file name (/compare.html) is
refused even though it answers: one address per page, the one the site calls
canonical.

  python3 scripts/lint_site_links.py            # check, exit 1 on a dead link
  python3 scripts/lint_site_links.py --list     # print every link it checked

The carried contract goes stale when the site adds a page; a link to a page
newer than the copy fails here until the carry is rerun
(docs/GENERATED_FILES.md). That is the design: the two repositories agree in
a committed file, not in someone's memory.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CONTRACT = REPO / "canary-local" / "devices" / "site_contract.json"
MANIFEST = "canary-local/build-line.json"
ORIGIN = "https://securacv.com"

# A literal URL. The path stops at whitespace, a quote, or the punctuation
# that closes the sentence, tag, or Markdown link the URL sits in.
URL = re.compile(r"https://securacv\.com(?![\w.-])(/[^\s\"'<>)\]}\\`|,;]*)?")
# An origin constant joined to a path: SITE_ORIGIN + "/help", SITE + '/x#y'.
JOINED = re.compile(r"\b(?:SITE_ORIGIN|SITE_URL|SITE)\s*\+\s*([\"'])(/[^\"']*)\1")

# Not text, or not ours to read: binaries, lock files, compiled bundles (a
# dist is rebuilt by CI from sources this lint already reads), vendored code.
SKIP_SUFFIX = re.compile(
    r"\.(?:png|jpe?g|gif|webp|avif|ico|svg|stl|3mf|step|glb|gltf|hdr|wasm|bin|elf|uf2|"
    r"woff2?|ttf|otf|pdf|zip|gz|xz|tgz|mp3|wav|mp4|mov|lock|map|min\.js)$")
SKIP_DIR = re.compile(r"(?:^|/)(?:dist|vendor|node_modules|third_party|\.git)/")
# This lint and its own tests name dead links on purpose.
SELF = {"scripts/lint_site_links.py", "scripts/tests/test_lint_site_links.py"}

# A URL that is an example in a test, not a link anyone follows. Each entry
# is a file and the reason; a new one is a decision, not a convenience.
EXAMPLES = {
    "ios/Tests/SecuraCVTests/AppRouteTests.swift":
        "parses made-up universal-link paths to prove unknown ones are refused",
    "ios/Tests/SecuraCVTests/MagicPairPlanTests.swift":
        "a made-up receipt URL the pairing parser must reject",
}

# What a template leaves in a path: ${x}, {x}, %s, <x>, an ellipsis, a glob.
TEMPLATED = re.compile(r"[${}%<>*…]|\.\.\.")


def load_contract(path: Path = CONTRACT) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    missing = [k for k in ("routes", "files", "anchors") if k not in data]
    if missing:
        sys.exit(f"{path.relative_to(REPO) if path.is_relative_to(REPO) else path}: no "
                 f"{', '.join(missing)} — the carried contract predates the site's address list; "
                 "rerun docs/hardware/enclosure/gen_builder_manifest.py --site <website checkout>")
    return data


def split_link(raw: str) -> tuple[str, str]:
    """A link's path and anchor: the query dropped, sentence punctuation the
    regex could not tell from the URL trimmed, an empty path read as "/"."""
    raw = raw.rstrip(".:!?*")
    path, _, anchor = raw.partition("#")
    path = path.partition("?")[0]
    anchor = anchor.partition("?")[0].rstrip(".:!?*")
    return (path or "/"), anchor


def verdict(path: str, anchor: str, contract: dict) -> str | None:
    """None when the link is good, else what is wrong with it."""
    routes, files, anchors = set(contract["routes"]), set(contract["files"]), contract["anchors"]
    route = path if path in routes else path.rstrip("/") if path.rstrip("/") in routes else None
    if route is None:
        if path in files:
            return None if not anchor else f"{path} is a file; #{anchor} names nothing in it"
        if path.endswith(".html") and (path[:-5] in routes or (path == "/index.html")):
            return f"links the file name; the page's address is {path[:-5] if path != '/index.html' else '/'}"
        return "is not an address securacv.com answers"
    if anchor and route in anchors and anchor not in anchors[route]:
        return f"#{anchor} is not an anchor on {route}"
    if anchor and route not in anchors:
        return f"#{anchor}: {route} has no anchors at all"
    return None


def tracked() -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=REPO, capture_output=True, check=True).stdout
    return [f for f in out.decode("utf-8", "replace").split("\0") if f]


def scannable(rel: str) -> bool:
    return not (rel in SELF or SKIP_SUFFIX.search(rel) or SKIP_DIR.search(rel))


def links_in_text(rel: str, text: str) -> list[tuple[int, str, str]]:
    """(line, raw link as written, how it was written) for one file's text."""
    found = []
    for n, line in enumerate(text.splitlines(), 1):
        for m in URL.finditer(line):
            found.append((n, m.group(1) or "/", "url"))
        for m in JOINED.finditer(line):
            found.append((n, m.group(2), "joined"))
    return found


def links_in_manifest(text: str) -> list[tuple[int, str, str]]:
    """The Lab manifest's site-relative hrefs. Its renderers prefix the origin
    to an href that starts with "/", so each is a link to the site."""
    data = json.loads(text)
    hrefs = []
    if isinstance(data.get("onramp"), dict) and isinstance(data["onramp"].get("site"), str):
        hrefs.append(data["onramp"]["site"])
    for stage in data.get("stages", []):
        hrefs += [x.get("href") for x in stage.get("site", [])]
    hrefs += [x.get("href") for x in data.get("siteNav", []) + data.get("footerNav", [])]
    lines = text.splitlines()

    def line_of(href: str) -> int:
        needle = json.dumps(href)
        return next((i for i, ln in enumerate(lines, 1) if needle in ln), 1)

    return [(line_of(h), h, "manifest") for h in hrefs if isinstance(h, str) and h.startswith("/")]


def scan(files: list[str], read=None) -> tuple[list[tuple[str, int, str, str]], list[str]]:
    """Every checked link as (file, line, path#anchor, how), and the files
    whose links are examples (returned so a stale exemption can be named)."""
    read = read or (lambda rel: (REPO / rel).read_text(encoding="utf-8"))
    out, exempt_used = [], []
    for rel in files:
        if not scannable(rel):
            continue
        try:
            text = read(rel)
        except (UnicodeDecodeError, FileNotFoundError, IsADirectoryError):
            continue
        found = links_in_text(rel, text)
        if rel == MANIFEST:
            found += links_in_manifest(text)
        if not found:
            continue
        if rel in EXAMPLES:
            exempt_used.append(rel)
            continue
        for line, raw, how in found:
            if TEMPLATED.search(raw):
                continue
            out.append((rel, line, raw, how))
    return out, exempt_used


def check(links, contract) -> list[str]:
    problems = []
    for rel, line, raw, how in links:
        path, anchor = split_link(raw)
        why = verdict(path, anchor, contract)
        if why:
            shown = f"{ORIGIN}{raw}" if how == "url" else f'"{raw}" ({how}, read as {ORIGIN}{raw})'
            problems.append(f"{rel}:{line}: {shown} — {why}")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", action="store_true", help="print every link checked")
    args = ap.parse_args(argv)

    contract = load_contract()
    links, exempt_used = scan(tracked())
    if args.list:
        for rel, line, raw, how in links:
            print(f"{rel}:{line}: {raw} [{how}]")
    problems = check(links, contract)
    stale = sorted(set(EXAMPLES) - set(exempt_used))
    for rel in stale:
        problems.append(f"{rel}: listed in EXAMPLES and has no securacv.com link — remove the entry")
    if problems:
        print("site links: these do not open anything on securacv.com\n", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        print(f"\n{len(problems)} problem(s). The site's addresses are in "
              f"{CONTRACT.relative_to(REPO)} (carried from the website; "
              "docs/GENERATED_FILES.md says how to refresh it).", file=sys.stderr)
        return 1
    pages = len({split_link(raw)[0] for _, _, raw, _ in links})
    print(f"site links OK — {len(links)} links to {pages} securacv.com addresses, every one answered")
    return 0


if __name__ == "__main__":
    sys.exit(main())
