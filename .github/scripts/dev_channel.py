#!/usr/bin/env python3
"""Keep the rolling dev channel (`fw-dev-latest`) at or ahead of stable.

A Canary set to the dev update channel polls
`releases/download/fw-dev-latest/manifest-<product>.json`, and both flashers'
dev toggle reads `fw-dev-latest/manifest-flash.json`. Until 2026-10-27 only a
`-dev.N` / `-rc.N` release wrote those manifests, and none had ever been cut,
so the dev channel held no per-product manifest at all (a Canary on it got a
404 on every check) and a flash catalog two stable releases old
(.github/RELEASE_LESSONS.md, 2026-10-27).

So every firmware release, stable or dev, runs this with its own tag, and
**Actions → Refresh the dev channel** runs it with `releases/latest`. It
copies the source release's channel manifests to `fw-dev-latest`, byte for
byte, when the dev channel carries something older:

  over the air     every `manifest-canary*.json`, `manifest-index.json` and
                   `sha256sums.txt`; what the dev channel carries is read
                   from its `manifest-canary.json` `version`
  flasher catalog  `manifest-flash.json`; read from its `fw_train`

Each group is decided on its own (Flasher Factory Images writes only the
catalog), and each is copied when fw-dev-latest lacks it or carries an older
version than the source. A newer dev build is left alone, and so is any
source older than stable: the dev channel never goes below `releases/latest`.
Versions are ordered the way the device orders them
(`securacv_version_compare`: the triple, then stable > rc.N > dev.N, numeric
within a band), so 2.4.18-dev.1 > 2.4.17 > 2.4.17-rc.1.

The copies stay valid where they land because every file a source manifest
names is an absolute URL on the source's own tag, and the Ed25519 signatures
cover those URLs, not where the manifest is served from. The URLs are checked
before anything is written: a manifest naming a file that is not an asset of
the source release, by absolute URL on its tag, is refused.

What it never does: delete an asset (the old dev factory images stay), make
fw-dev-latest anything but a prerelease, or move `releases/latest`. Every
create and edit passes `--prerelease --latest=false`, and it fails unless
`releases/latest` names the same fw-v* release afterwards as before.

    python3 .github/scripts/dev_channel.py --repo owner/name [--source fw-vX.Y.Z] [--dry-run]

Needs `gh` with GH_TOKEN (contents: write). Exit 0: copied, or nothing to
do; 1: refused or a `gh` call failed, with the reason.

Run the tests:  python3 -m unittest discover -s .github/scripts -p 'test_*.py'
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

DEV_TAG = "fw-dev-latest"
TITLE = "Canary Firmware — dev channel (rolling)"
FLASH = "manifest-flash.json"
INDEX = "manifest-index.json"
SUMS = "sha256sums.txt"
FLAGSHIP = "manifest-canary.json"
PRODUCT_GLOB = "manifest-canary*.json"
REQUIRED = (FLASH, INDEX, SUMS, FLAGSHIP)


class Refusal(Exception):
    """Nothing is written (or nothing more): the reason says why."""


class GhError(Exception):
    """A `gh` call failed; the message carries its stderr."""


@dataclass(frozen=True)
class Group:
    name: str
    label: str
    marker: str  # the file that says what the group carries; uploaded last
    field: str   # the marker's version field


GROUPS = (
    Group("ota", "the over-the-air manifests (`manifest-canary*.json`)", FLAGSHIP, "version"),
    Group("flash", "the flashers' catalog (`manifest-flash.json`)", FLASH, "fw_train"),
)


# ─────────────────────────────── versions ──────────────────────────────────

_TRIPLE = re.compile(r"(\d+)\.(\d+)\.(\d+)")
_BANDS = ((2, re.compile(r"[-.]rc\.(\d*)")), (1, re.compile(r"[-.]dev\.(\d*)")))


def version_key(version) -> tuple | None:
    """Sort key in securacv_version_compare's order, or None with no triple.

    The device's rule (firmware/common/ota/src/securacv_ota.cpp): the numeric
    triple, then a band, stable (3) > rc.N (2) > dev.N (1), then N. Only an
    explicit rc.N / dev.N segment is a prerelease marker; a variant suffix
    (`2.4.17-wap`) is not. Unlike the device, which reads "1.0" as 1.0.0 and
    garbage as equal to everything, a version without a full triple has no key
    here, so it can never be taken for older.
    """
    if not isinstance(version, str):
        return None
    m = _TRIPLE.match(version)
    if not m:
        return None
    rest = version[m.end():]
    band, num = 3, 0
    for b, pattern in _BANDS:
        pm = pattern.search(rest)
        if pm:
            band, num = b, int(pm.group(1) or 0)
            break
    return (int(m[1]), int(m[2]), int(m[3]), band, num)


def compare(a: str, b: str) -> int:
    ka, kb = version_key(a), version_key(b)
    if ka is None or kb is None:
        raise ValueError(f"not a firmware version: {a if ka is None else b!r}")
    return (ka > kb) - (ka < kb)


def tag_version(tag: str) -> str:
    if not tag.startswith("fw-v") or version_key(tag[4:]) is None:
        raise Refusal(f"{tag!r} is not a fw-v<version> firmware tag")
    return tag[4:]


# ─────────────────────────────── decisions ─────────────────────────────────


def decide(source: str, stable: str, trains: dict[str, str | None]) -> dict[str, tuple[bool, str]]:
    """Per group: (copy it?, why). `trains` is what fw-dev-latest carries now."""
    if compare(source, stable) < 0:
        why = f"{source} is older than stable {stable}, and the dev channel never goes below stable"
        return {g.name: (False, why) for g in GROUPS}
    out = {}
    for g in GROUPS:
        train = trains.get(g.name)
        if train is None:
            out[g.name] = (True, f"{DEV_TAG} has no {g.marker}")
            continue
        if version_key(train) is None:
            raise Refusal(
                f"{DEV_TAG}'s {g.marker} says {g.field} {train!r}, which is not a firmware "
                f"version, so there is no telling whether it is ahead of {source}. Look at it by hand.")
        cmp = compare(source, train)
        if cmp > 0:
            out[g.name] = (True, f"{DEV_TAG} carries {train}, older than {source}")
        elif cmp == 0:
            out[g.name] = (False, f"{DEV_TAG} already carries {train}")
        else:
            out[g.name] = (False, f"{DEV_TAG} carries {train}, newer than {source}: left alone")
    return out


def channel_files(group: Group, names) -> list[str]:
    """The source files a group copies, its marker last."""
    if group.name == "flash":
        return [FLASH]
    products = sorted(n for n in names if fnmatch.fnmatchcase(n, PRODUCT_GLOB) and n != FLAGSHIP)
    return products + [INDEX, SUMS, FLAGSHIP]


def render_body(repo: str, source_tag: str, stable: str, decisions, trains) -> str:
    source = source_tag[4:]
    link = f"[`{source_tag}`](https://github.com/{repo}/releases/tag/{source_tag})"
    if version_key(source)[3] == 3:
        head = (f"Currently carrying firmware **{source}**, the stable release {link}: the dev "
                f"channel had no newer build, so it carries the stable one until the next "
                f"`-dev.N` / `-rc.N` release.")
    else:
        head = (f"Currently carrying firmware **{source}**, the dev build {link}, ahead of "
                f"stable {stable}.")
    lines = [
        "Rolling pointer for the **dev update channel**. A Canary set to the dev channel "
        "polls `manifest-<product>.json` here; the flashers' **Advanced → dev channel** "
        "reads `manifest-flash.json`. Devices and flashers on the release channel never "
        "see this: their URL rides `releases/latest`, which GitHub never points at a "
        "prerelease.",
        "",
        head,
    ]
    for g in GROUPS:
        if decisions[g.name][0]:
            continue
        train = trains[g.name]
        how = "the same version" if compare(source, train) == 0 else "newer"
        lines += ["", f"Except {g.label}, left on **{train}** ({how})."]
    lines += [
        "",
        "`.github/scripts/dev_channel.py` keeps this release at or ahead of stable: every "
        "firmware release runs it, and so does **Actions → Refresh the dev channel**. "
        "See `docs/RELEASE_PROCESS.md`.",
    ]
    return "\n".join(lines) + "\n"


# ─────────────────────────────── checks ────────────────────────────────────


def _load(path: Path, what: str) -> dict:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise Refusal(f"{what} is not readable JSON ({exc})") from exc
    if not isinstance(doc, dict):
        raise Refusal(f"{what} is not a JSON object")
    return doc


def check_source(src: Path, names: set[str], files: list[str], repo: str, tag: str) -> None:
    """Every manifest about to be copied names its files absolutely, on `tag`.

    Copied to another release, a relative or foreign URL would stop resolving
    (or resolve to something its signature never covered); the train each
    marker declares must be the tag's own version.
    """
    base = f"https://github.com/{repo}/releases/download/{tag}/"
    version = tag[4:]

    def held(url, where):
        if not (isinstance(url, str) and url.startswith(base) and url[len(base):] in names):
            raise Refusal(f"{tag}'s {where} names {url!r}, not an asset of {tag} by absolute URL; "
                          f"copied to {DEV_TAG} it would not stay valid")

    for name in files:
        if not name.endswith(".json"):
            continue
        doc = _load(src / name, f"{tag}'s {name}")
        if name == FLASH:
            if doc.get("fw_train") != version:
                raise Refusal(f"{tag}'s {FLASH} says fw_train {doc.get('fw_train')!r}, not {version}")
            products = doc.get("products")
            if not isinstance(products, dict) or not products:
                raise Refusal(f"{tag}'s {FLASH} lists no products")
            for pid, entry in products.items():
                held((entry or {}).get("factory"), f"{FLASH} entry {pid}")
        elif name == INDEX:
            products = doc.get("products")
            if not isinstance(products, dict) or not products:
                raise Refusal(f"{tag}'s {INDEX} lists no products")
            for pid, url in products.items():
                held(url, f"{INDEX} entry {pid}")
        else:
            held(doc.get("url"), name)
            if name == FLAGSHIP and doc.get("version") != version:
                raise Refusal(f"{tag}'s {FLAGSHIP} says version {doc.get('version')!r}, not {version}")


# ─────────────────────────────── GitHub ────────────────────────────────────


def _gh(argv: list[str]) -> str:
    proc = subprocess.run(["gh", *argv], capture_output=True, text=True, check=False)
    if proc.returncode:
        raise GhError(f"gh {' '.join(argv)}: {proc.stderr.strip() or proc.returncode}")
    return proc.stdout


class GitHub:
    """The `gh` calls this makes. `run` is injectable; the tests answer the
    same argv a runner would send."""

    def __init__(self, repo: str, run: Callable[[list[str]], str] = _gh):
        self.repo = repo
        self.run = run

    def latest_tag(self) -> str:
        return self.run(["api", f"repos/{self.repo}/releases/latest", "--jq", ".tag_name"]).strip()

    def release(self, tag: str) -> dict | None:
        try:
            return json.loads(self.run(["api", f"repos/{self.repo}/releases/tags/{tag}"]))
        except GhError as exc:
            if "HTTP 404" in str(exc):
                return None
            raise

    def commit(self, tag: str) -> str:
        return self.run(["api", f"repos/{self.repo}/commits/{tag}", "--jq", ".sha"]).strip()

    def download(self, tag: str, names: list[str], dest: Path) -> None:
        argv = ["release", "download", tag, "--repo", self.repo, "--dir", str(dest)]
        for name in names:
            argv += ["--pattern", name]
        self.run(argv)

    def upload(self, tag: str, paths: list[Path]) -> None:
        self.run(["release", "upload", tag, *map(str, paths), "--clobber", "--repo", self.repo])

    def create(self, tag: str, target: str, body: Path) -> None:
        self.run(["release", "create", tag, "--repo", self.repo, "--target", target,
                  "--prerelease", "--latest=false", "--title", TITLE, "--notes-file", str(body)])

    def edit(self, tag: str, body: Path) -> None:
        self.run(["release", "edit", tag, "--repo", self.repo,
                  "--prerelease", "--latest=false", "--title", TITLE, "--notes-file", str(body)])


# ─────────────────────────────── the run ───────────────────────────────────


def _names(release: dict | None) -> set[str]:
    return {a["name"] for a in (release or {}).get("assets") or []}


def _train(path: Path, group: Group) -> str:
    value = _load(path, f"{DEV_TAG}'s {group.marker}").get(group.field)
    return value if isinstance(value, str) else repr(value)


def sync(gh: GitHub, source: str | None = None, dry_run: bool = False,
         log: Callable[[str], None] = print) -> bool:
    """Bring fw-dev-latest up to `source` (default releases/latest) where it is
    older. Returns whether anything was written."""
    latest = gh.latest_tag()
    if not latest.startswith("fw-v"):
        raise Refusal(f"releases/latest is {latest!r}, not a firmware release; fix that first "
                      f"(keep-firmware-latest)")
    stable = tag_version(latest)
    source = source or latest
    version = tag_version(source)
    rel = gh.release(source)
    if rel is None or rel.get("draft"):
        raise Refusal(f"{source} is not a published release")
    names = _names(rel)
    missing = [n for n in REQUIRED if n not in names]
    if missing:
        raise Refusal(f"{source} carries no {', '.join(missing)}: not a complete firmware release")
    dev = gh.release(DEV_TAG)
    if dev is not None and not dev.get("prerelease"):
        raise Refusal(f"{DEV_TAG} is not a prerelease; it must never be a release "
                      f"releases/latest could point at. Fix that by hand first")
    dev_names = _names(dev)

    with tempfile.TemporaryDirectory() as tmp:
        here = Path(tmp)
        markers = [g.marker for g in GROUPS if g.marker in dev_names]
        if markers:
            gh.download(DEV_TAG, markers, here / "dev")
        trains = {g.name: _train(here / "dev" / g.marker, g) if g.marker in dev_names else None
                  for g in GROUPS}
        decisions = decide(version, stable, trains)
        for g in GROUPS:
            log(f"{g.name}: {'copy' if decisions[g.name][0] else 'leave'} — {decisions[g.name][1]}")
        todo = [g for g in GROUPS if decisions[g.name][0]]
        if not todo:
            log(f"{DEV_TAG} is at or ahead of {source}: nothing to do")
            return False
        files = [f for g in todo for f in channel_files(g, names)]
        gh.download(source, files, here / "src")
        check_source(here / "src", names, files, gh.repo, source)
        body = here / "body.md"
        body.write_text(render_body(gh.repo, source, stable, decisions, trains), encoding="utf-8")
        if dry_run:
            log(f"dry run: would copy {len(files)} file(s) from {source} to {DEV_TAG}")
            return False
        # Each group's marker goes up last, in its own call (`gh release
        # upload` sends a call's files concurrently): a run cut short leaves
        # the marker on the old version, so the next run copies the group
        # again instead of reading it as done.
        try:
            if dev is None:
                gh.create(DEV_TAG, gh.commit(source), body)
            for g in todo:
                group = channel_files(g, names)
                if group[:-1]:
                    gh.upload(DEV_TAG, [here / "src" / f for f in group[:-1]])
                gh.upload(DEV_TAG, [here / "src" / group[-1]])
            if dev is not None:
                gh.edit(DEV_TAG, body)
        finally:
            now = gh.latest_tag()
            if now != latest:
                raise Refusal(f"releases/latest moved from {latest} to {now} while {DEV_TAG} was "
                              f"written: every Canary's OTA manifest URL now points at {now}. "
                              f"Re-point it (keep-firmware-latest) and find what moved it")
    log(f"{DEV_TAG} now carries {source}'s {' and '.join(g.name for g in todo)} manifests "
        f"({len(files)} file(s)); releases/latest is still {latest}")
    return True


def main(argv: list[str] | None = None, run: Callable[[list[str]], str] = _gh) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY"),
                    help="owner/name (default: $GITHUB_REPOSITORY)")
    ap.add_argument("--source", help="the fw-v* release to copy from (default: releases/latest)")
    ap.add_argument("--dry-run", action="store_true", help="decide and check, write nothing")
    args = ap.parse_args(argv)
    if not args.repo:
        ap.error("--repo is required outside Actions")
    lines: list[str] = []

    def log(line: str) -> None:
        print(line)
        lines.append(line)

    try:
        sync(GitHub(args.repo, run), args.source, args.dry_run, log)
        status = 0
    except (Refusal, GhError) as exc:
        print(f"::error::{exc}")
        lines.append(f"refused: {exc}")
        status = 1
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as fh:
            fh.write(f"### Dev channel ({DEV_TAG})\n\n" + "".join(f"- {line}\n" for line in lines))
    return status


if __name__ == "__main__":
    sys.exit(main())
