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

Platform KEYS and the manifest's version/pub_date are preserved verbatim
(we inherit tauri-action's own scheme rather than reinventing it); only url and
signature are refreshed — plus, optionally, `notes`: with --notes-file the
manifest's notes are replaced by that file's text (the version's section of the
app's RELEASE_NOTES.md), which is what the in-app "update ready" UI shows the
user. A key we don't recognize is a hard error, not a silent skip — a new
bundle type must be mapped here on purpose.

Both desktop apps share this script (--product picks the installer filenames):
the Flasher's finalize job and the Lab's draft-finalize job both call it, so a
hardening fix lands on every self-updating app at once instead of drifting.

Usage:
  harden_updater_manifest.py [--product SecuraCV.Flasher|SecuraCV.Lab]
                             [--notes-file NOTES.md]
                             <manifest.json> <owner> <repo> <tag> <version> <sig_dir>
  harden_updater_manifest.py --self-test
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


DEFAULT_PRODUCT = "SecuraCV.Flasher"


def installer_filename(platform_key: str, version: str,
                       product: str = DEFAULT_PRODUCT) -> str:
    """Map a Tauri updater platform key to the installer asset filename the
    updater should fetch for it. GitHub stores asset names with dots where the
    local bundle had spaces, so these are the dotted forms (`product` is the
    dotted productName, e.g. SecuraCV.Flasher or SecuraCV.Lab)."""
    appimage = f"{product}_{version}_amd64.AppImage"
    deb = f"{product}_{version}_amd64.deb"
    app = f"{product}_{version}_universal.app.tar.gz"
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
           sig_of: dict, product: str = DEFAULT_PRODUCT,
           notes: str | None = None) -> dict:
    """Return the manifest with every platform's url + signature refreshed.
    `sig_of` maps an installer filename to its current .sig contents. When
    `notes` is given it replaces the manifest's notes — the text the in-app
    updater shows as "what's changing"."""
    platforms = manifest.get("platforms")
    if not platforms:
        raise SystemExit("harden_updater_manifest: manifest has no platforms")
    for key, info in platforms.items():
        fn = installer_filename(key, version, product)
        if fn not in sig_of:
            raise SystemExit(
                f"harden_updater_manifest: no signature available for {fn} "
                f"(platform {key}) — refusing to ship a manifest missing a signature"
            )
        info["url"] = stable_url(owner, repo, tag, fn)
        info["signature"] = sig_of[fn]
    if notes is not None:
        stripped = notes.strip()
        if not stripped:
            raise SystemExit(
                "harden_updater_manifest: --notes-file is empty — an update "
                "that can't say what it changes isn't ready to ship"
            )
        manifest["notes"] = stripped
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
    # --product maps the Lab's filenames the same way
    lab_aimg = f"SecuraCV.Lab_{ver}_amd64.AppImage"
    lab = harden(
        {"version": ver, "platforms": {"linux-x86_64": {"url": "x", "signature": "OLD"}}},
        "o", "r", f"app-v{ver}", ver, {lab_aimg: "SIG_LAB"}, product="SecuraCV.Lab")
    assert lab["platforms"]["linux-x86_64"]["url"].endswith(f"/download/app-v{ver}/{lab_aimg}")
    assert lab["platforms"]["linux-x86_64"]["signature"] == "SIG_LAB"
    # notes replace the manifest's notes; empty notes fail loud
    noted = harden(
        {"version": ver, "notes": "See the assets…",
         "platforms": {"linux-x86_64": {"url": "x", "signature": "OLD"}}},
        "o", "r", "t", ver, {aimg: "S"}, notes="- **New.** It helps.\n")
    assert noted["notes"] == "- **New.** It helps."
    try:
        harden({"platforms": {"linux-x86_64": {}}}, "o", "r", "t", ver,
               {aimg: "S"}, notes="   \n")
    except SystemExit:
        pass
    else:
        raise AssertionError("empty --notes-file should have failed")
    print("harden_updater_manifest self-test: OK")
    return 0


def main(argv: list[str]) -> int:
    if argv and argv[0] == "--self-test":
        return _self_test()
    product = DEFAULT_PRODUCT
    notes: str | None = None
    while argv and argv[0].startswith("--"):
        if argv[0] == "--product" and len(argv) >= 2:
            product = argv[1]
            argv = argv[2:]
        elif argv[0] == "--notes-file" and len(argv) >= 2:
            notes = Path(argv[1]).read_text(encoding="utf-8")
            argv = argv[2:]
        else:
            raise SystemExit(__doc__)
    if len(argv) != 6:
        raise SystemExit(__doc__)
    manifest_path, owner, repo, tag, version, sig_dir = argv
    manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8"))
    # Read each distinct installer's signature from the .sig files present.
    sig_of: dict[str, str] = {}
    for fn in {installer_filename(k, version, product) for k in manifest.get("platforms", {})}:
        sig_path = Path(sig_dir) / (fn + ".sig")
        sig_of[fn] = sig_path.read_text(encoding="utf-8").strip()
    hardened = harden(manifest, owner, repo, tag, version, sig_of, product, notes)
    Path(manifest_path).write_text(json.dumps(hardened, indent=2) + "\n", encoding="utf-8")
    print(
        f"hardened {len(hardened['platforms'])} platform entries with stable URLs"
        f" + fresh signatures{' + release notes' if notes is not None else ''}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
