#!/usr/bin/env python3
"""canary-local/tools/mirror_release.py — the browser flasher's releases, same-origin.

A browser cannot fetch a GitHub release asset. github.com answers
/releases/download/<tag>/<file> with a 302 that carries no
Access-Control-Allow-Origin, and the asset host it redirects to answers 200
with none either, so the browser refuses every manifest and every image.
api.github.com does send the header, but its asset downloads redirect to the
same host, so it is no way out. The hosted flasher (flash.html on GitHub
Pages) therefore reported a connection problem and every product as
unavailable (.github/RELEASE_LESSONS.md, 2026-10-27).
The desktop apps fetch natively and never had the problem.

So pages.yml runs this after it assembles the site, and the releases the page
pins are served from the page's own origin:

  * flash.json `manifest_url`              (the stable channel, an exact fw-v* tag)
  * flash.json `we2_module.manifest_url`   (the Vision module's model)
  * flash-core.js `DEV_FLASH_MANIFEST_URL` (the dev channel's rolling fw-dev-latest)

For each one: the manifest, byte for byte (the page's Ed25519 check reads it
unchanged), and every file it lists, each checked against the manifest's
SHA-256 and size. They land at <site>/firmware/<tag>/<file>, where <tag> and
<file> come from each URL itself (a dev manifest may name images under the
real fw-v*-dev.* tag). flash-core.js `releaseFetchUrl` reads that same path,
and scripts/tests/test_pages_release_mirror.py holds the two layouts to each
other through node.

A channel is published whole or not at all. These skip it with a ::warning::
and leave the deploy green, because the deploy must survive them:

  * the release does not exist yet: the push deploy that merges a version bump
    runs BEFORE firmware-release.yml cuts the tag. The page then says this
    site's copy of the pinned release is not available (never that the
    release was not cut: it cannot see the release itself), and
    firmware-release.yml / flasher-release.yml / vision-model-release.yml
    dispatch pages.yml again once they publish;
  * an asset is missing, a SHA-256 or size disagrees with its manifest, or a
    manifest is unreadable: a release mid-upload, or a broken one. Nothing of
    that channel is published (the page says the same); the next dispatch
    tries again.

A transport failure that outlasts the retries, or a catalog this script cannot
read the pins from, exits 1. Failing the deploy keeps the last good site, and
its mirror, live; a deploy without the mirror would take every product down.

    python3 canary-local/tools/mirror_release.py --site _site/canary-local
    python3 canary-local/tools/mirror_release.py --plan    # the pins, no network

Reads GH_TOKEN (or GITHUB_TOKEN) for the REST API; `contents: read` is enough.
"""
from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CATALOG = REPO / "canary-local" / "devices" / "flash.json"
CORE = REPO / "canary-local" / "assets" / "flash-core.js"

# flash-core.js RELEASE_MIRROR_DIR: the directory, beside the pages, that the
# page reads a release from. The test holds the two spellings together.
MIRROR_DIR = "firmware"
# flash-core.js RELEASE_SEGMENT_RE: one path segment of a tag or asset name.
SEGMENT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
ASSET_PATH_RE = re.compile(r"^/([^/]+)/([^/]+)/releases/download/([^/]+)/([^/]+)$")
DEV_URL_RE = re.compile(r'export const DEV_FLASH_MANIFEST_URL\s*=\s*"(https://[^"]+)"')
SHA256_RE = re.compile(r"^[0-9a-fA-F]{64}$")
FLASH_SCHEMA = "securacv-flash-1"  # flash-core.js MANIFEST_SCHEMA

API = "https://api.github.com"
ATTEMPTS = 4


class PlanError(Exception):
    """The pins themselves cannot be read: a code defect, not a release state."""


class Skip(Exception):
    """This channel cannot be published whole right now; the deploy goes on."""


class TransportError(Exception):
    """GitHub did not answer, retries included."""


@dataclass(frozen=True)
class Asset:
    repo: str
    tag: str
    file: str

    @property
    def path(self) -> str:
        return f"{MIRROR_DIR}/{self.tag}/{self.file}"


@dataclass(frozen=True)
class Channel:
    name: str   # "release" | "vision" | "dev"
    kind: str   # "flash" | "vision": which manifest shape it carries
    url: str
    asset: Asset


@dataclass(frozen=True)
class Ref:
    url: str
    sha256: str
    size: int | None


def asset_parts(url: str, repo: str | None = None) -> Asset | None:
    """The tag and file of a GitHub release-download URL, or None.

    The same rule as flash-core.js releaseAssetParts: https, github.com, no
    port, credentials, query or fragment, and a tag and file that are each one
    plain segment. With `repo`, only that repository's assets answer.
    """
    try:
        u = urllib.parse.urlsplit(str(url))
        port = u.port
    except ValueError:
        return None
    if (u.scheme != "https" or (u.hostname or "") != "github.com" or port is not None
            or u.username or u.password or u.query or u.fragment):
        return None
    m = ASSET_PATH_RE.match(u.path)
    if not m:
        return None
    tag = urllib.parse.unquote(m.group(3))
    file = urllib.parse.unquote(m.group(4))
    if not SEGMENT_RE.match(tag) or not SEGMENT_RE.match(file):
        return None
    owner = f"{m.group(1)}/{m.group(2)}"
    if repo and owner.lower() != repo.lower():
        return None
    return Asset(owner, tag, file)


def dev_manifest_url(core_src: str) -> str:
    m = DEV_URL_RE.search(core_src)
    if not m:
        raise PlanError("flash-core.js: no `export const DEV_FLASH_MANIFEST_URL = \"https://…\"`")
    return m.group(1)


def plan_channels(catalog: dict, core_src: str) -> list[Channel]:
    """The three releases the page pins, as flash.json and flash-core.js pin them."""
    repo = catalog.get("repo")
    if not isinstance(repo, str) or "/" not in repo:
        raise PlanError("flash.json: no `repo` (owner/name)")
    we2 = catalog.get("we2_module") or {}
    pins = [
        ("release", "flash", catalog.get("manifest_url")),
        ("vision", "vision", we2.get("manifest_url")),
        ("dev", "flash", dev_manifest_url(core_src)),
    ]
    out = []
    for name, kind, url in pins:
        asset = asset_parts(url or "", repo)
        if not asset:
            raise PlanError(
                f"{name}: {url!r} is not a {repo} release-download URL, so the page "
                f"cannot map it to {MIRROR_DIR}/<tag>/<file> either")
        out.append(Channel(name, kind, url, asset))
    return out


def _sha(text) -> str:
    if not isinstance(text, str) or not SHA256_RE.match(text):
        raise Skip(f"bad sha256 {text!r}")
    return text.lower()


def _size(value, required: bool) -> int | None:
    if value is None and not required:
        return None
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise Skip(f"bad size {value!r}")
    return value


def manifest_refs(kind: str, manifest) -> list[Ref]:
    """Every file a manifest lists, with the digest the page will check it by.

    The same shape the page validates (flash-core.js validateManifest for a
    flash manifest; we2-flash.js's model entry for the Vision one), so a
    manifest the page would refuse is not mirrored either.
    """
    if not isinstance(manifest, dict):
        raise Skip("manifest is not a JSON object")
    if kind == "flash":
        if manifest.get("schema") != FLASH_SCHEMA:
            raise Skip(f"schema {manifest.get('schema')!r}, want {FLASH_SCHEMA!r}")
        products = manifest.get("products")
        if not isinstance(products, dict):
            raise Skip("no products map")
        refs = []
        for pid, e in sorted(products.items()):
            if not isinstance(e, dict) or not isinstance(e.get("factory"), str):
                raise Skip(f"{pid}: no factory URL")
            try:
                refs.append(Ref(e["factory"], _sha(e.get("sha256")), _size(e.get("size"), True)))
            except Skip as why:
                raise Skip(f"{pid}: {why}") from None
        return refs
    if kind == "vision":
        model = manifest.get("model")
        if not isinstance(model, dict) or not isinstance(model.get("url"), str):
            raise Skip("no model entry with a url")
        return [Ref(model["url"], _sha(model.get("sha256")), _size(model.get("size"), False))]
    raise PlanError(f"unknown manifest kind {kind!r}")


class GitHubReleases:
    """Release assets through the REST API (works for public and private repos)."""

    def __init__(self, repo: str, token: str | None = None, api: str = API,
                 attempts: int = ATTEMPTS, sleep=time.sleep, opener=None):
        self.repo = repo
        self.token = token
        self.api = api.rstrip("/")
        self.attempts = attempts
        self.sleep = sleep
        self.opener = opener or urllib.request.build_opener()
        self._assets: dict[str, dict[str, str] | None] = {}

    def _get(self, url: str, accept: str) -> bytes | None:
        """The body, or None on 404. Retries 5xx / 429 / transport errors."""
        last = None
        for attempt in range(self.attempts):
            req = urllib.request.Request(url, headers={
                "Accept": accept,
                "User-Agent": "securacv-pages-release-mirror",
                "X-GitHub-Api-Version": "2022-11-28",
            })
            if self.token:
                # Unredirected: the asset download 302s to a signed URL on the
                # asset host, which must not receive the token.
                req.add_unredirected_header("Authorization", f"Bearer {self.token}")
            try:
                with self.opener.open(req, timeout=60) as resp:
                    return resp.read()
            except urllib.error.HTTPError as err:
                if err.code == 404:
                    return None
                if err.code < 500 and err.code != 429:
                    raise TransportError(f"{url}: HTTP {err.code}") from None
                last = f"HTTP {err.code}"
            except (http.client.HTTPException, OSError) as err:
                # A body cut off mid-read is http.client.IncompleteRead, an
                # HTTPException and not an OSError; URLError, a timeout, a
                # reset and ssl.SSLError are all OSErrors.
                last = str(getattr(err, "reason", err)) or type(err).__name__
            if attempt + 1 < self.attempts:
                self.sleep(2 * 3 ** attempt)
        raise TransportError(f"{url}: {last} after {self.attempts} attempts")

    def assets(self, tag: str) -> dict[str, str] | None:
        """{asset name: API download URL} for a tag's release, or None when there is none."""
        if tag in self._assets:
            return self._assets[tag]
        quoted = urllib.parse.quote(tag, safe="")
        body = self._get(f"{self.api}/repos/{self.repo}/releases/tags/{quoted}",
                         "application/vnd.github+json")
        if body is None:
            self._assets[tag] = None
            return None
        release = json.loads(body)
        found: dict[str, str] = {}
        page = 1
        while True:
            listing = self._get(
                f"{self.api}/repos/{self.repo}/releases/{release['id']}/assets"
                f"?per_page=100&page={page}", "application/vnd.github+json")
            batch = json.loads(listing) if listing else []
            for a in batch:
                found[a["name"]] = a["url"]
            if len(batch) < 100:
                break
            page += 1
        self._assets[tag] = found
        return found

    def download(self, url: str) -> bytes | None:
        return self._get(url, "application/octet-stream")


def fetch_asset(source, asset: Asset) -> bytes:
    names = source.assets(asset.tag)
    if names is None:
        raise Skip(f"no release for tag {asset.tag}")
    if asset.file not in names:
        raise Skip(f"release {asset.tag} has no asset {asset.file}")
    data = source.download(names[asset.file])
    if data is None:
        raise Skip(f"release {asset.tag}: {asset.file} answered 404")
    return data


def stage_channel(ch: Channel, source, repo: str) -> dict[Asset, bytes]:
    """Every file of one channel, each checked against its manifest, or Skip. Nothing is written here."""
    staged: dict[Asset, bytes] = {}
    manifest_bytes = fetch_asset(source, ch.asset)
    try:
        manifest = json.loads(manifest_bytes)
    except ValueError:
        raise Skip(f"{ch.asset.file} is not JSON") from None
    staged[ch.asset] = manifest_bytes
    for ref in manifest_refs(ch.kind, manifest):
        asset = asset_parts(ref.url, repo)
        if not asset:
            raise Skip(f"{ref.url} is not a {repo} release asset; the page could not read it "
                       f"from {MIRROR_DIR}/ either")
        if asset in staged:
            data = staged[asset]
        else:
            data = fetch_asset(source, asset)
        got = hashlib.sha256(data).hexdigest()
        if got != ref.sha256:
            raise Skip(f"{asset.tag}/{asset.file}: sha256 {got} != manifest {ref.sha256}")
        if ref.size is not None and len(data) != ref.size:
            raise Skip(f"{asset.tag}/{asset.file}: {len(data)} bytes != manifest {ref.size}")
        staged[asset] = data
    return staged


def _annotate(level: str, text: str) -> None:
    print(f"::{level}::{text}", flush=True)


def _summary(lines: list[str]) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    with open(path, "a", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")


def mirror(channels: list[Channel], source, site: Path, repo: str) -> list[str]:
    """Write every channel that stages whole; return the summary lines."""
    written: dict[Asset, str] = {}
    lines = ["### Browser flasher: same-origin release mirror", ""]
    for ch in channels:
        try:
            staged = stage_channel(ch, source, repo)
            for asset, data in staged.items():
                prior = written.get(asset)
                if prior is not None and prior != hashlib.sha256(data).hexdigest():
                    raise Skip(f"{asset.path} was already written by another channel "
                               f"with different bytes")
        except Skip as why:
            msg = (f"{ch.name} channel ({ch.asset.tag}/{ch.asset.file}) not mirrored: {why}. "
                   f"The flasher reports it missing until the next deploy.")
            _annotate("warning", msg)
            lines.append(f"- ⚠️ {msg}")
            continue
        total = 0
        for asset, data in staged.items():
            dest = site / MIRROR_DIR / asset.tag / asset.file
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
            written[asset] = hashlib.sha256(data).hexdigest()
            total += len(data)
        msg = (f"{ch.name} channel: {ch.asset.tag}/{ch.asset.file} + {len(staged) - 1} "
               f"file(s), {total / 1e6:.1f} MB → {MIRROR_DIR}/")
        print(msg, flush=True)
        lines.append(f"- ✅ {msg}")
    return lines


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--site", type=Path, help="the deployed canary-local directory (e.g. _site/canary-local)")
    ap.add_argument("--catalog", type=Path, default=CATALOG)
    ap.add_argument("--core", type=Path, default=CORE)
    ap.add_argument("--plan", action="store_true", help="print the pinned releases and exit")
    args = ap.parse_args(argv)
    try:
        catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
        channels = plan_channels(catalog, args.core.read_text(encoding="utf-8"))
    except (OSError, ValueError, PlanError) as err:
        _annotate("error", f"mirror_release: cannot read the pinned releases: {err}")
        return 1
    if args.plan:
        for ch in channels:
            print(f"{ch.name:8} {ch.asset.repo} {ch.asset.tag}/{ch.asset.file} → {ch.asset.path}")
        return 0
    if not args.site:
        ap.error("--site is required unless --plan")
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    source = GitHubReleases(catalog["repo"], token)
    try:
        lines = mirror(channels, source, args.site, catalog["repo"])
    except (TransportError, ValueError, KeyError) as err:
        _annotate("error", f"mirror_release: {err}. Failing the deploy keeps the last good "
                           f"site and its mirror live; re-run once GitHub answers.")
        return 1
    _summary(lines)
    return 0


if __name__ == "__main__":
    sys.exit(main())
