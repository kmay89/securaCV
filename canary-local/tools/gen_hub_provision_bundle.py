#!/usr/bin/env python3
"""canary-local/tools/gen_hub_provision_bundle.py — the hub's provisioning bundle.

Assembles the SELF-CONTAINED payload that turns a freshly-flashed hub into a
working securaCV stack: the provisioning plan, the curated Frigate config, and
the executor that runs them — plus a one-line runner — packed so they can be
dropped onto a device and run with a single command, no repo checkout required.

This is the "seed assembler" from docs/design/raspberry_pi_hub_flashing.md
(Step 5). It is deliberately the CONTENT half of the assembler:

  * This tool (host-testable, here) produces the bundle from the repo's curated
    sources and pins each file's SHA-256, so the bundle can never quietly carry
    stale code.
  * The flasher's seed layer (`hub_io::seed`, Rust, hardware-gated) is what
    writes that bundle into the card's `CONFIG/` tree — the same mechanism that
    already drops the Wi-Fi keyfile.

Why a bundle of the transparent executor, and NOT an opaque HA "backup":
  The early design imagined seeding a curated Home Assistant *backup*. The
  executor (canary-local/tools/hub_seed_apply.py) makes that unnecessary and
  worse — a narrated, idempotent, auditable provisioner beats a binary blob for
  a product whose whole point is "understand *why* it's configuring each thing".
  So the bundle carries the executor, not a backup.

The one honest gap — first-boot AUTO-run:
  HAOS has no supported hook to auto-run an arbitrary script from the boot
  partition, so true zero-touch first boot is NOT something this tool can prove
  offline; that hook is what on-hardware validation pins (see `first_boot` in the
  emitted manifest). What DOES work today, and is what CI exercises: drop the
  bundle somewhere with Python + the Supervisor token (the Advanced SSH & Web
  Terminal add-on) and run `sh provision.sh`. HAOS ignores boot-partition files
  it doesn't recognise, so a bundle that isn't auto-run is harmless — worst case
  is a normal onboarding, never a broken boot.

Emits (drift-gated, like its sibling generators):
  canary-local/devices/hub_provision_bundle.json — the manifest: every file the
  bundle carries, its on-card path, and its SHA-256; the runner; and the honest
  first-boot status.

Run:
  python3 canary-local/tools/gen_hub_provision_bundle.py            # write manifest
  python3 canary-local/tools/gen_hub_provision_bundle.py --build DIR # assemble a real bundle
CI: the manifest command + `git diff --exit-code`, plus a build-and-dry-run smoke.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT_JSON = REPO / "canary-local/devices/hub_provision_bundle.json"

# Where, on the card's boot partition, the bundle lands. Namespaced under our own
# folder so it can never collide with anything HAOS puts in CONFIG/.
BUNDLE_ROOT_ON_CARD = "CONFIG/securacv"

# The curated sources the bundle carries. `bundle_path` is where each lands
# INSIDE the bundle; the executor resolves the Frigate config via its
# repo-relative path, so that path is preserved to keep the bundle self-contained
# (--assets-root <bundle> then resolves homeassistant/frigate/config.yaml).
SOURCES = [
    {"role": "plan", "source": "canary-local/devices/hub_seed.json", "bundle_path": "hub_seed.json"},
    {"role": "frigate-config", "source": "homeassistant/frigate/config.yaml",
     "bundle_path": "homeassistant/frigate/config.yaml"},
    {"role": "executor", "source": "canary-local/tools/hub_seed_apply.py", "bundle_path": "hub_seed_apply.py"},
]

RUNNER_NAME = "provision.sh"


def die(msg: str) -> None:
    sys.exit(f"gen_hub_provision_bundle.py: {msg}")


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def runner_script() -> str:
    """The one-command runner. POSIX sh so it runs anywhere; it invokes the
    bundled executor against the bundled plan + assets, passing args through so
    `sh provision.sh --dry-run` previews and `sh provision.sh` provisions."""
    return (
        "#!/bin/sh\n"
        "# securaCV hub provisioning — one command. See README.md.\n"
        "#   preview (no hub needed):  sh provision.sh --dry-run\n"
        "#   provision (needs SUPERVISOR_TOKEN + http://supervisor):  sh provision.sh\n"
        "set -eu\n"
        'here="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"\n'
        'exec python3 "$here/hub_seed_apply.py" --plan "$here/hub_seed.json" '
        '--assets-root "$here" "$@"\n'
    )


def readme(files: list[dict]) -> str:
    lines = [
        "# securaCV hub provisioning bundle",
        "",
        "Everything needed to turn a freshly-flashed Home Assistant OS hub into a",
        "working securaCV stack — the plan, the curated Frigate config, and the",
        "executor that runs them — in one self-contained folder. No repo checkout.",
        "",
        "## Run it today",
        "",
        "From the **Advanced SSH & Web Terminal** add-on (it ships `python3` and the",
        "`SUPERVISOR_TOKEN` this needs):",
        "",
        "```sh",
        "sh provision.sh --dry-run   # read the narrated plan; changes nothing",
        "sh provision.sh             # do it for real",
        "```",
        "",
        "The dry run prints every step, *why* it happens, and the exact Supervisor",
        "API call it will make. The real run is idempotent and fails closed — safe to",
        "re-run if anything goes wrong.",
        "",
        "## What's inside",
        "",
    ]
    for f in files:
        lines.append(f"- `{f['bundle_path']}` — {f['role']}")
    lines += [
        f"- `{RUNNER_NAME}` — the runner above",
        "",
        "## First boot",
        "",
        "HAOS has no supported hook to auto-run a script from the boot partition, so",
        "for now this is a one-command step, not zero-touch. Dropped onto the card's",
        "`CONFIG/` tree it does no harm — HAOS ignores files it doesn't recognise, so",
        "the worst case is a normal onboarding. Wiring a first-boot hook to run",
        "`provision.sh` automatically is the on-hardware-validated frontier; see",
        "`docs/design/raspberry_pi_hub_flashing.md`.",
        "",
    ]
    return "\n".join(lines)


def build_manifest() -> dict:
    files = []
    for s in SOURCES:
        src = REPO / s["source"]
        if not src.exists():
            die(f"missing bundle source {s['source']} — the bundle can't be assembled without it")
        files.append(
            {
                "role": s["role"],
                "source": s["source"],
                "bundle_path": s["bundle_path"],
                "card_path": f"{BUNDLE_ROOT_ON_CARD}/{s['bundle_path']}",
                "sha256": sha256_of(src),
            }
        )
    # The runner is generated (deterministic), so pin its hash too — a change to
    # how we invoke the executor should show up in the drift gate.
    files.append(
        {
            "role": "runner",
            "generated": True,
            "bundle_path": RUNNER_NAME,
            "card_path": f"{BUNDLE_ROOT_ON_CARD}/{RUNNER_NAME}",
            "sha256": hashlib.sha256(runner_script().encode("utf-8")).hexdigest(),
        }
    )
    return {
        "$generated_by": "canary-local/tools/gen_hub_provision_bundle.py — do not edit by hand",
        "$doc": (
            "The securaCV hub provisioning bundle: the self-contained payload (plan + curated "
            "Frigate config + executor + runner) that provisions a freshly-flashed hub with one "
            "command. Generated so the bundle can never carry stale code — each file's SHA-256 is "
            "pinned, and CI regenerates this manifest and fails on drift."
        ),
        "schema_version": 1,
        "bundle_root_on_card": BUNDLE_ROOT_ON_CARD,
        "runner": {
            "file": RUNNER_NAME,
            "runs": "python3 hub_seed_apply.py --plan hub_seed.json --assets-root .",
            "dry_run": f"sh {RUNNER_NAME} --dry-run",
        },
        "files": files,
        "first_boot": {
            "status": "planned",
            "what_works_today": (
                "From the Advanced SSH & Web Terminal add-on (python3 + SUPERVISOR_TOKEN present): "
                f"copy this bundle and run `sh {RUNNER_NAME}` — `--dry-run` first to preview."
            ),
            "candidate_hooks": [
                "A curated HA package/automation seeded into CONFIG/ that shells out to the runner "
                "on first HA start (HA Core can reach http://supervisor with its own token).",
                "A first-boot provisioner add-on the flasher pre-registers, which runs the bundle.",
            ],
            "note": (
                "HAOS ignores boot-partition files it doesn't recognise, so a bundle that isn't "
                "auto-run is harmless — worst case is a normal onboarding, never a broken boot. The "
                "write side is hub_io::seed (the CONFIG/ tree, already used for Wi-Fi); which hook "
                "actually auto-runs the bundle is what on-hardware validation pins."
            ),
        },
        "meta": {
            "design": "docs/design/raspberry_pi_hub_flashing.md",
            "guide": "docs/full_stack_setup.md",
            "executor": "canary-local/tools/hub_seed_apply.py",
            "sources": [s["source"] for s in SOURCES],
        },
    }


def build_bundle(manifest: dict, out_dir: Path) -> None:
    """Assemble a real, runnable bundle into out_dir from the manifest."""
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    for f in manifest["files"]:
        dest = out_dir / f["bundle_path"]
        dest.parent.mkdir(parents=True, exist_ok=True)
        if f.get("generated"):
            continue  # written below
        src = REPO / f["source"]
        # cp -L semantics: dereference so a symlinked source lands as real bytes.
        shutil.copyfile(src, dest, follow_symlinks=True)
    (out_dir / RUNNER_NAME).write_text(runner_script(), encoding="utf-8")
    (out_dir / RUNNER_NAME).chmod(0o755)
    source_files = [f for f in manifest["files"] if not f.get("generated")]
    (out_dir / "README.md").write_text(readme(source_files), encoding="utf-8")
    (out_dir / "MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, metavar="DIR", help="assemble a runnable bundle into DIR")
    ap.add_argument("--manifest", type=Path, default=OUT_JSON, help="where to write the manifest JSON")
    args = ap.parse_args(argv)

    manifest = build_manifest()

    if args.build:
        build_bundle(manifest, args.build)
        n = len(manifest["files"]) + 2  # + README + MANIFEST
        print(f"built bundle at {args.build} — {n} files, root-on-card {BUNDLE_ROOT_ON_CARD}")
        return 0

    args.manifest.write_text(json.dumps(manifest, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(f"wrote {args.manifest.relative_to(REPO)} — {len(manifest['files'])} files in the bundle")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
