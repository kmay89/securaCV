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
  emitted manifest). What DOES work with no monitor: the bundle now carries
  `host_provision.sh`, a wrapper runnable from the ONE shell a fresh headless hub
  exposes — the HAOS developer console on port 22222, unlocked by the
  `authorized_keys` maintenance key the flasher seeds next to this bundle. The
  flasher's first-boot companion drives it over SSH from the operator's computer;
  the same command works typed by hand. `sh provision.sh` still works from the
  Advanced SSH & Web Terminal add-on (python3 + SUPERVISOR_TOKEN present). HAOS
  ignores boot-partition files it doesn't recognize, so a bundle that isn't run
  is harmless — worst case is a normal onboarding, never a broken boot.

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
from pathlib import Path

from _tooling import die, repo_root

REPO = repo_root()
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
    # The host-side runner: provision.sh needs python3 + SUPERVISOR_TOKEN, which
    # the HAOS developer console (port 22222) doesn't have — this wrapper borrows
    # both from the running stack (the Core container) so the bundle can be run
    # from the ONE shell a headless hub exposes with no add-ons installed yet.
    # It is what the flasher's first-boot companion invokes over SSH.
    {"role": "host-runner", "source": "canary-local/tools/hub_host_provision.sh",
     "bundle_path": "host_provision.sh"},
]

RUNNER_NAME = "provision.sh"


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def runner_script() -> str:
    """The one-command runner. POSIX sh so it runs anywhere; it invokes the
    bundled executor against the bundled plan + assets, passing args through so
    `sh provision.sh --dry-run` previews and `sh provision.sh` provisions."""
    return (
        "#!/bin/sh\n"
        "# securaCV hub provisioning — one command: registers the add-on repos,\n"
        "# installs the broker + Frigate + the securaCV kernel, and starts them,\n"
        "# narrating why at every step.\n"
        "#   preview (no hub needed):  sh provision.sh --dry-run\n"
        "#   provision (needs SUPERVISOR_TOKEN + http://supervisor):  sh provision.sh\n"
        "set -eu\n"
        'here="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"\n'
        'exec python3 "$here/hub_seed_apply.py" --plan "$here/hub_seed.json" '
        '--assets-root "$here" "$@"\n'
    )


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
    # The runner is generated (deterministic) but SHIPS in the bundle, so pin its
    # hash too — the manifest promises every carried file is pinned, and a change
    # to how we invoke the executor must show up in the drift gate. (MANIFEST.json
    # is the manifest itself and so cannot self-pin; it is verified by equality
    # with this committed file, not by an entry inside it. The bundle carries no
    # separate README — provision.sh is self-documenting.)
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
        "host_runner": {
            "file": "host_provision.sh",
            "context": (
                "The HAOS developer console (SSH, port 22222) — the one shell a fresh headless "
                "hub exposes. Borrows python3 + SUPERVISOR_TOKEN from the running Core container, "
                "so it needs no add-on installed first."
            ),
            "runs": "sh /mnt/boot/CONFIG/securacv/host_provision.sh",
            "dry_run": "sh /mnt/boot/CONFIG/securacv/host_provision.sh --dry-run",
        },
        "files": files,
        "first_boot": {
            "status": "planned",
            "what_works_today": (
                "Headless, from another screen: the flasher's first-boot companion connects to the "
                "HAOS developer console (port 22222, unlocked by the maintenance key the flasher "
                "seeds next to this bundle) and runs `sh /mnt/boot/CONFIG/securacv/host_provision.sh` "
                "once the hub answers — the hub itself never needs a monitor. The same command works "
                "by hand from that console, and `sh provision.sh` still works from the Advanced SSH "
                "& Web Terminal add-on (python3 + SUPERVISOR_TOKEN present). `--dry-run` previews "
                "either path."
            ),
            "candidate_hooks": [
                "A curated HA package/automation seeded into CONFIG/ that shells out to the runner "
                "on first HA start (HA Core can reach http://supervisor with its own token).",
                "A first-boot provisioner add-on the flasher pre-registers, which runs the bundle.",
            ],
            "note": (
                "HAOS ignores boot-partition files it doesn't recognize, so a bundle that isn't "
                "auto-run is harmless — worst case is a normal onboarding, never a broken boot. The "
                "write side is hub_io::seed (the CONFIG/ tree, already used for Wi-Fi). True "
                "zero-touch (the hub running the bundle with no companion at all) stays `planned` "
                "until a supported HAOS boot hook is pinned on hardware; the companion-over-SSH path "
                "is the honest interim, and it too needs its first real-hardware run."
            ),
        },
        "meta": {
            "design": "docs/design/raspberry_pi_hub_flashing.md",
            "guide": "docs/full_stack_setup.md",
            "executor": "canary-local/tools/hub_seed_apply.py",
            "sources": [s["source"] for s in SOURCES],
        },
    }


# Files that unmistakably mark a directory as one of our own bundles — the only
# case in which --build is allowed to recursively clear a non-empty target.
BUNDLE_SENTINELS = frozenset({"MANIFEST.json", RUNNER_NAME, "hub_seed_apply.py"})


def _prepare_build_dir(out_dir: Path) -> None:
    """Leave out_dir as an empty directory to build into — WITHOUT ever recursively
    deleting something that isn't one of our own bundles. `--build .` or a path full
    of unrelated files must fail loudly, never wipe the user's working tree or data.
    """
    if not out_dir.exists():
        out_dir.mkdir(parents=True)
        return
    if not out_dir.is_dir():
        die(f"--build target {out_dir} exists and is not a directory")
    entries = {p.name for p in out_dir.iterdir()}
    if not entries:
        return  # empty directory: safe to build into
    if BUNDLE_SENTINELS.issubset(entries):
        # Unmistakably a previous bundle of ours — safe to replace.
        shutil.rmtree(out_dir)
        out_dir.mkdir(parents=True)
        return
    die(
        f"refusing to overwrite non-empty {out_dir}: it is not a securaCV bundle "
        f"(missing {', '.join(sorted(BUNDLE_SENTINELS))}). Pass a new or empty directory."
    )


def build_bundle(manifest: dict, out_dir: Path) -> None:
    """Assemble a real, runnable bundle into out_dir from the manifest."""
    _prepare_build_dir(out_dir)
    source_files = [f for f in manifest["files"] if not f.get("generated")]
    for f in source_files:
        dest = out_dir / f["bundle_path"]
        dest.parent.mkdir(parents=True, exist_ok=True)
        src = REPO / f["source"]
        # cp -L semantics: dereference so a symlinked source lands as real bytes.
        shutil.copyfile(src, dest, follow_symlinks=True)
    # The runner is generated, written from the SAME function its manifest hash
    # pins, so the shipped bytes always match the pin. (No README — provision.sh
    # documents itself.)
    (out_dir / RUNNER_NAME).write_text(runner_script(), encoding="utf-8")
    (out_dir / RUNNER_NAME).chmod(0o755)
    (out_dir / "MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def _display(p: Path) -> str:
    """Path for status messages: repo-relative when it is under the repo, else the
    path as given — so an out-of-repo --manifest never crashes on relative_to."""
    resolved = p.resolve()
    try:
        return str(resolved.relative_to(REPO))
    except ValueError:
        return str(p)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, metavar="DIR", help="assemble a runnable bundle into DIR")
    ap.add_argument("--manifest", type=Path, default=OUT_JSON, help="where to write the manifest JSON")
    args = ap.parse_args(argv)

    manifest = build_manifest()

    if args.build:
        build_bundle(manifest, args.build)
        n = len(manifest["files"]) + 1  # bundle files + MANIFEST.json
        print(f"built bundle at {_display(args.build)} — {n} files, root-on-card {BUNDLE_ROOT_ON_CARD}")
        return 0

    args.manifest.write_text(json.dumps(manifest, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(f"wrote {_display(args.manifest)} — {len(manifest['files'])} files in the bundle")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
