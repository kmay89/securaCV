#!/usr/bin/env python3
"""Harden a Tauri updater manifest (latest.json) against asset churn.

tauri-action writes each platform's `url` as an **asset-id** link
(`api.github.com/.../releases/assets/<id>`). Those ids change every time an
asset is re-uploaded — a re-run, a flaky-upload retry, a second matrix job —
so the manifest silently starts pointing at deleted ids while the installer
files themselves are fine. A partial job failure can also leave a platform's
embedded `signature` stale relative to the installer that actually shipped.

This rewrites every platform entry to be durable:
  • `url`   → the stable **name-based** download URL
             (`github.com/<owner>/<repo>/releases/download/<tag>/<file>`),
             which survives re-uploads because it is keyed by filename.
  • `signature` → the CURRENT contents of that installer's `.sig`, read from
             the files this release actually carries — so the manifest can
             never disagree with the bytes it points at.

Platform KEYS and the manifest's version/notes/pub_date are preserved verbatim
(we inherit tauri-action's own scheme rather than reinventing it); only url and
signature are refreshed. A key we don't recognize is a hard error, not a
silent skip — a new bundle type must be mapped here on purpose.

Usage:
  harden_updater_manifest.py <manifest.json> <owner> <repo> <tag> <version> <sig_dir>
  harden_updater_manifest.py --self-test
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


def installer_filename(platform_key: str, version: str) -> str:
    """Map a Tauri updater platform key to the installer asset filename the
    updater should fetch for it. GitHub stores asset names with dots where the
    local bundle had spaces, so these are the dotted forms."""
    appimage = f"SecuraCV.Flasher_{version}_amd64.AppImage"
    deb = f"SecuraCV.Flasher_{version}_amd64.deb"
    app = f"SecuraCV.Flasher_{version}_universal.app.tar.gz"
    if "deb" in platform_key:
        return deb
    if "linux" in platform_key:
        return appimage
    if "darwin" in platform_key:
        return app
    raise SystemExit(
        f"harden_updater_manifest: unrecognized platform key {platform_key!r} — "
        "add its installer mapping before shipping a manifest for it"
    )


def stable_url(owner: str, repo: str, tag: str, filename: str) -> str:
    # Name-based release download URL: stable across re-uploads (unlike the
    # asset-id API URL tauri-action emits). The dotted filenames need no
    # percent-encoding.
    return f"https://github.com/{owner}/{repo}/releases/download/{tag}/{filename}"


def harden(manifest: dict, owner: str, repo: str, tag: str, version: str,
           sig_of: dict) -> dict:
    """Return the manifest with every platform's url + signature refreshed.
    `sig_of` maps an installer filename to its current .sig contents."""
    platforms = manifest.get("platforms")
    if not platforms:
        raise SystemExit("harden_updater_manifest: manifest has no platforms")
    for key, info in platforms.items():
        fn = installer_filename(key, version)
        if fn not in sig_of:
            raise SystemExit(
                f"harden_updater_manifest: no signature available for {fn} "
                f"(platform {key}) — refusing to ship a manifest missing a signature"
            )
        info["url"] = stable_url(owner, repo, tag, fn)
        info["signature"] = sig_of[fn]
    return manifest


def _self_test() -> int:
    ver = "9.9.9"
    aimg = f"SecuraCV.Flasher_{ver}_amd64.AppImage"
    deb = f"SecuraCV.Flasher_{ver}_amd64.deb"
    app = f"SecuraCV.Flasher_{ver}_universal.app.tar.gz"
    # every key tauri-action emits today
    keys = [
        "linux-x86_64", "linux-x86_64-appimage", "linux-x86_64-deb",
        "darwin-aarch64", "darwin-x86_64", "darwin-universal",
        "darwin-aarch64-app", "darwin-x86_64-app", "darwin-universal-app",
    ]
    manifest = {"version": ver, "platforms": {k: {"url": "https://api.github.com/x/assets/1", "signature": "OLD"} for k in keys}}
    sig_of = {aimg: "SIG_AIMG", deb: "SIG_DEB", app: "SIG_APP"}
    out = harden(manifest, "o", "r", f"flasher-v{ver}", ver, sig_of)
    expect = {
        "linux-x86_64": (aimg, "SIG_AIMG"), "linux-x86_64-appimage": (aimg, "SIG_AIMG"),
        "linux-x86_64-deb": (deb, "SIG_DEB"),
        "darwin-aarch64": (app, "SIG_APP"), "darwin-x86_64": (app, "SIG_APP"),
        "darwin-universal": (app, "SIG_APP"), "darwin-aarch64-app": (app, "SIG_APP"),
        "darwin-x86_64-app": (app, "SIG_APP"), "darwin-universal-app": (app, "SIG_APP"),
    }
    for k, (fn, sig) in expect.items():
        got = out["platforms"][k]
        assert got["url"].endswith(f"/download/flasher-v{ver}/{fn}"), (k, got["url"])
        assert got["signature"] == sig, (k, got["signature"])
        assert "api.github.com" not in got["url"], (k, got["url"])
    # an unknown key must fail loud
    try:
        harden({"platforms": {"windows-x86_64": {}}}, "o", "r", "t", ver, sig_of)
    except SystemExit:
        pass
    else:
        raise AssertionError("unknown platform key should have failed")
    print("harden_updater_manifest self-test: OK")
    return 0


def main(argv: list[str]) -> int:
    if argv and argv[0] == "--self-test":
        return _self_test()
    if len(argv) != 6:
        raise SystemExit(__doc__)
    manifest_path, owner, repo, tag, version, sig_dir = argv
    manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
    # Read each distinct installer's signature from the .sig files present.
    sig_of: dict[str, str] = {}
    for fn in {installer_filename(k, version) for k in manifest.get("platforms", {})}:
        sig_path = Path(sig_dir) / (fn + ".sig")
        sig_of[fn] = sig_path.read_text(encoding="utf-8").strip()
    hardened = harden(manifest, owner, repo, tag, version, sig_of)
    Path(manifest_path).write_text(json.dumps(hardened, indent=2) + "\n", encoding="utf-8")
    print(f"hardened {len(hardened['platforms'])} platform entries with stable URLs + fresh signatures")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
