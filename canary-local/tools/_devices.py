"""_devices.py — the device manifests (devices/<slug>/device.json), read once
for the canary-local generators.

One manifest per Canary, holding the ids the seven device schemas use for one
piece of hardware (devices/README.md). scripts/lint_device_manifests.py proves
every one of those ids against the file that owns it; this module only READS
them, so a generator can take a fact from the manifest instead of typing it a
second time.

  load_manifests(repo=None) -> {slug: manifest}
      every manifest, keyed by slug. Malformed JSON dies here, naming the file.
  by_flasher_product(manifests) -> {flash.json product id: manifest}
      the reverse of `flasher.product` + `flasher.variants`. A product two
      manifests claim dies here — the linter refuses that tree too, but a
      generator must not pick one silently.

Importable as `from _devices import ...` for the same reason as _tooling:
the generators run as `python3 canary-local/tools/<tool>.py`, which puts this
directory at sys.path[0].
"""
from __future__ import annotations

import json
from pathlib import Path

from _tooling import die, repo_root


def load_manifests(repo: Path | None = None) -> dict[str, dict]:
    root = repo or repo_root()
    devices = root / "devices"
    out: dict[str, dict] = {}
    for d in sorted(p for p in devices.iterdir() if p.is_dir()):
        path = d / "device.json"
        if not path.exists():
            die(f"{path.relative_to(root)}: missing — every directory under devices/ is a device")
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            die(f"{path.relative_to(root)}: not JSON ({e})")
        if data.get("slug") != d.name:
            die(f"{path.relative_to(root)}: slug '{data.get('slug')}' != directory name '{d.name}'")
        out[d.name] = data
    if not out:
        die(f"{devices.relative_to(root)}/ holds no manifests")
    return out


def by_flasher_product(manifests: dict[str, dict]) -> dict[str, dict]:
    out: dict[str, dict] = {}
    for slug, m in manifests.items():
        fl = m.get("flasher")
        if not fl:
            continue
        if not fl.get("product"):
            die(f"devices/{slug}: flasher block has no `product` — name the flash.json "
                f"product this device installs onto, or drop the block")
        for pid in [fl["product"], *fl.get("variants", [])]:
            if pid in out:
                die(f"flasher product '{pid}' is claimed by devices/{out[pid]['slug']} and "
                    f"devices/{slug} — one product installs onto one device")
            out[pid] = m
    return out
