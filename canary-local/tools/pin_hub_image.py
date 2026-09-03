#!/usr/bin/env python3
"""canary-local/tools/pin_hub_image.py — the hub image pin ceremony.

The hub-image catalog (gen_hub_image.py) is honest-before-pin: image URLs are
derivable from the HAOS version, but sha256 stays empty and `pinned` false until
a ceremony fills them — the same posture as flash.json's all-zero release key
until the signing ceremony. This tool IS that ceremony, and its weekly re-run is
the anti-rot loop: the writer should trust a repo-vouched hash, and that vouch
must renew itself instead of aging.

What a pin means here: for the HAOS version the Hub snapshot currently names,
download each supported board's release asset and hash the actual bytes, cross-
check that hash against GitHub's own asset digest when GitHub publishes one, and
commit the verified hash to canary-local/devices/hub_image_pins.json. (Home
Assistant ships no downloadable checksum of its own — no `<image>.sha256`
sibling, no combined SHA256SUMS — so the bytes you'd flash are the authoritative
source; the API digest is an independent cross-check, not the anchor.)
gen_hub_image.py folds committed pins back into the catalog (pinned=true) only
while they match the current version — a HAOS bump automatically un-pins until
the ceremony runs again, so the catalog can claim exactly as much trust as it
has.

Failure posture, two modes on purpose:

  · ceremony (default) is self-healing like the freshness refresh: if an asset
    cannot be downloaded, or the API digest is present and disagrees, existing
    pins are left untouched and the run exits 0 with a warning — the catalog
    degrades to the honest unpinned state, never to a wrong pin. Values only
    move forward on a verified read of the real bytes.
  · --verify is the loud alarm: re-fetch and compare against the COMMITTED pins
    and fail hard on a dead URL (link rot) or a hash that changed under a
    pinned version (upstream re-release or tampering — either way, a human
    looks before any flasher trusts it).

Run:  python3 canary-local/tools/pin_hub_image.py            # the ceremony
      python3 canary-local/tools/pin_hub_image.py --verify   # the alarm
CI:   the freshness workflow runs the ceremony weekly after the upstream
      snapshot refresh, and --verify guards the pins that already exist.
"""
from __future__ import annotations

import json
import os
import re
import sys
import urllib.request
from datetime import date
from pathlib import Path

from _tooling import repo_root, warn

REPO = repo_root()
sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_hub_image  # noqa: E402 — the catalog generator is the board/URL source of truth

PINS_JSON = REPO / "canary-local/devices/hub_image_pins.json"
HA_JSON = REPO / "canary-local/devices/homeassistant.json"

RELEASE_API = "https://api.github.com/repos/home-assistant/operating-system/releases/tags/{version}"

# Some CDNs 403 the default Python-urllib user agent — identify honestly
# instead (same convention as gen_homeassistant.py).
USER_AGENT = "securaCV-freshness/1.0 (+https://github.com/kmay89/securaCV)"

SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def _error(msg: str) -> None:
    print(f"pin_hub_image: {msg}", file=sys.stderr)
    if os.environ.get("GITHUB_ACTIONS"):
        print(f"::error::pin_hub_image: {msg}")


def _fetch(url: str, headers: dict | None = None) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **(headers or {})})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def normalize_sha256(raw: str) -> str | None:
    """Canonical lowercase hex from the shapes a checksum arrives in — bare hex,
    GitHub's `sha256:<hex>` digest, or a `sha256sum` line `<hex>  <file>`.
    Mirrors hub-core's normalize_sha256 (hub_image.rs) so the two ends of the
    trust chain accept exactly the same envelopes."""
    s = raw.strip()
    if s[:7].lower() == "sha256:":
        s = s[7:]
    token = s.split()[0] if s.split() else ""
    token = token.lower()
    return token if SHA256_RE.match(token) else None


def current_version() -> str:
    """The HAOS version the Hub snapshot currently names — pins are only ever
    minted for this version, so the catalog and the pins can't disagree about
    what they describe."""
    if not HA_JSON.exists():
        sys.exit("pin_hub_image: missing homeassistant.json — run gen_homeassistant.py first")
    version = json.loads(HA_JSON.read_text(encoding="utf-8")).get("upstream", {}).get("haos_version", "")
    if not version:
        sys.exit("pin_hub_image: no haos_version in homeassistant.json upstream snapshot")
    return version


def load_pins() -> dict:
    if not PINS_JSON.exists():
        return {}
    try:
        return json.loads(PINS_JSON.read_text(encoding="utf-8"))
    except Exception as e:  # noqa: BLE001 — a corrupt pins file must be loud, not skipped
        sys.exit(f"pin_hub_image: {PINS_JSON.relative_to(REPO)} is unreadable ({e}) — fix or delete it")


def downloaded_sha256(image_url: str) -> str | None:
    """The authoritative channel, data plane: stream the actual asset bytes over
    TLS and hash them. Home Assistant publishes NO `<image>.sha256` sibling and
    no combined SHA256SUMS file for its OS release assets (both 404), so the
    only checksum that can't be faked out from under us is the one taken over
    the very bytes a flasher would write. None (with a warning) on any fetch
    failure — degrade to unpinned, never to a guess."""
    import hashlib

    try:
        req = urllib.request.Request(image_url, headers={"User-Agent": USER_AGENT})
        hasher = hashlib.sha256()
        with urllib.request.urlopen(req, timeout=120) as r:
            for chunk in iter(lambda: r.read(1024 * 1024), b""):
                hasher.update(chunk)
    except Exception as e:  # noqa: BLE001 — degrade to unpinned, never to a guess
        warn(f"could not download {image_url} to hash it ({e})")
        return None
    return hasher.hexdigest()


def github_digests(version: str) -> dict[str, str]:
    """GitHub's own per-asset digests for the release, as {asset_name: hex}.
    The independent second channel: the .sha256 file and the API digest come
    from different plumbing, so one quietly rewritten artifact can't satisfy
    both. Empty dict (with a warning) if the API can't be read."""
    tok = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    auth = {"Authorization": f"Bearer {tok}"} if tok else {}
    try:
        release = json.loads(_fetch(RELEASE_API.format(version=version), auth))
    except Exception as e:  # noqa: BLE001 — the ceremony degrades, --verify alarms
        warn(f"could not read the GitHub release for {version} ({e})")
        return {}
    out = {}
    for asset in release.get("assets", []):
        sha = normalize_sha256(str(asset.get("digest") or ""))
        if sha:
            out[asset.get("name", "")] = sha
    return out


def fetch_agreed_pins(version: str) -> dict[str, dict] | None:
    """Mint a pin per board from the authoritative data-plane hash, cross-checked
    against GitHub's asset digest whenever GitHub provides one. Returns
    {board_id: {asset, sha256}} only when EVERY board hashed cleanly; None
    otherwise — a partial ceremony pins nothing.

    Trust model, now that HA ships no downloadable checksum of its own:
      · the hash of the actual bytes is REQUIRED and is what we pin — it's the
        one value taken over exactly what a flasher writes;
      · GitHub's independent API digest is a cross-check: if present and it
        disagrees, that's a tamper/re-release alarm and we refuse to pin; if
        GitHub omits a digest, we pin the verified download anyway (with a
        warning) rather than block forever on a channel upstream may not fill.
    """
    digests = github_digests(version)
    pins: dict[str, dict] = {}
    for board in gen_hub_image.BOARDS:
        asset = f"{board['asset_stem']}-{version}.img.xz"
        url = f"{gen_hub_image.HAOS_RELEASE}/{version}/{asset}"
        hashed = downloaded_sha256(url)
        if not hashed:
            warn(f"{board['id']}: could not hash the download — not pinning")
            return None
        api = digests.get(asset)
        if api and api != hashed:
            _error(
                f"{board['id']}: channel disagreement for {asset} — the downloaded bytes hash to "
                f"{hashed}, but GitHub's asset digest says {api}. Refusing to pin either."
            )
            return None
        if not api:
            warn(
                f"{board['id']}: GitHub published no asset digest for {asset} — pinning the "
                "verified download on its own (single-sourced)"
            )
        pins[board["id"]] = {"asset": asset, "sha256": hashed}
    return pins


def write_pins(version: str, pins: dict[str, dict], prev: dict) -> None:
    # Byte-stable when nothing moved: keep the previous ceremony date so a
    # weekly re-run that confirms the same hashes produces no diff (and no PR).
    unchanged = prev.get("haos_version") == version and {
        b: p.get("sha256") for b, p in prev.get("boards", {}).items()
    } == {b: p["sha256"] for b, p in pins.items()}
    pinned_at = prev.get("pinned_at") if unchanged else date.today().isoformat()
    out = {
        "$generated_by": "canary-local/tools/pin_hub_image.py — the pin ceremony; do not edit by hand",
        "$doc": (
            "Repo-vouched SHA-256 pins for the hub base-OS images (docs/design/"
            "raspberry_pi_hub_flashing.md). Each hash was taken over the actual downloaded asset "
            "bytes and cross-checked against GitHub's asset digest where one was published; "
            "gen_hub_image.py folds these into hub_image.json (pinned=true) only while they match "
            "the current haos_version, so a version bump honestly un-pins until the ceremony "
            "re-runs. Re-verified weekly by pin_hub_image.py --verify in the freshness workflow."
        ),
        "schema_version": 1,
        "haos_version": version,
        "pinned_at": pinned_at,
        "channels": ["downloaded asset bytes (sha256)", "GitHub release asset digest (cross-check)"],
        "boards": pins,
    }
    PINS_JSON.write_text(json.dumps(out, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    state = "unchanged" if unchanged else "updated"
    print(f"wrote {PINS_JSON.relative_to(REPO)} — HAOS {version}, {len(pins)} boards, {state}")


def ceremony() -> int:
    version = current_version()
    prev = load_pins()
    pins = fetch_agreed_pins(version)
    if pins is None:
        if prev.get("haos_version") == version:
            warn(f"ceremony could not double-source HAOS {version}; keeping the existing pins verbatim")
        else:
            warn(
                f"ceremony could not verify HAOS {version} and the committed pins cover "
                f"{prev.get('haos_version') or 'nothing'} — the catalog stays unpinned. Home "
                "Assistant publishes no checksum to fall back on, so the writer cannot verify an "
                "unpinned image and will refuse to flash it until this ceremony commits a pin."
            )
        gen_hub_image.main()  # keep the catalog consistent with whatever pins state we're in
        return 0
    # A pin may never MOVE under the same version. New version → new pins is
    # the ceremony; same version → different hash is an upstream re-release or
    # tampering, and re-pinning it silently would erase exactly the signal the
    # pins exist to hold. Refuse loudly; a human decides.
    if prev.get("haos_version") == version:
        for board_id, prev_pin in prev.get("boards", {}).items():
            got = pins.get(board_id, {}).get("sha256")
            if got and got != prev_pin.get("sha256"):
                _error(
                    f"{board_id}: upstream now serves {got} but the committed pin for HAOS "
                    f"{version} is {prev_pin.get('sha256')} — a pin never moves under the same "
                    "version. Investigate; if the re-release is legitimate, delete "
                    f"{PINS_JSON.relative_to(REPO)} and re-run the ceremony deliberately."
                )
                return 1
    write_pins(version, pins, prev)
    gen_hub_image.main()
    return 0


def verify() -> int:
    """The alarm. Re-fetch both channels and compare with the committed pins.
    Any dead URL, missing digest, or moved hash for the pinned version is a hard
    failure — pinned trust is only worth keeping while it re-verifies."""
    prev = load_pins()
    if not prev.get("boards"):
        print("pin_hub_image --verify: no pins committed yet — nothing to guard")
        return 0
    version = prev.get("haos_version", "")
    live = fetch_agreed_pins(version)
    if live is None:
        _error(f"could not re-verify the committed pins for HAOS {version} (see warnings above)")
        return 1
    failed = False
    for board_id, pin in prev.get("boards", {}).items():
        got = live.get(board_id, {}).get("sha256")
        if got != pin.get("sha256"):
            _error(
                f"{board_id}: committed pin {pin.get('sha256')} no longer matches upstream "
                f"({got or 'missing'}) for HAOS {version} — the artifact changed under a pinned "
                "version; investigate before trusting either value"
            )
            failed = True
    if not failed:
        print(f"pin_hub_image --verify: HAOS {version} pins re-verified on both channels")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(verify() if "--verify" in sys.argv[1:] else ceremony())
