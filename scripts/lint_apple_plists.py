#!/usr/bin/env python3
"""lint_apple_plists.py — the two Apple-platform facts no compiler checks.

1. App Transport Security. Every Canary is reached over plain HTTP at a `.local`
   name or a private IP. Since iOS 10, ATS refuses cleartext to `.local` and
   unqualified hosts unless the Info.plist declares
   `NSAppTransportSecurity.NSAllowsLocalNetworking = true`. The tvOS Wall's
   plist carried it from day one; the iPhone's did not, so every
   `http://<canary>.local` call was rejected on device (NSURLErrorDomain -1022)
   while the simulator build in CI was green. Rule: any Info.plist that
   declares `NSBonjourServices` (i.e. talks to LAN devices) must also allow
   local networking — and ONLY local networking (`NSAllowsArbitraryLoads` is
   forbidden; the "nothing off-network" promise is enforced by the OS).

2. Privacy manifests. Since May 2024 App Store Connect rejects any upload whose
   bundles use a "required reason" API without a `PrivacyInfo.xcprivacy`
   declaring it. Every SecuraCV target reads `UserDefaults`. The rejection
   arrives by EMAIL after CI has passed, so the gate has to live here: the
   manifest must exist, declare the UserDefaults category with an accepted
   reason code, declare no tracking, and be listed under the `sources:` of
   every app / extension target in project.yml whose sources use UserDefaults.

Stdlib only (plistlib + a tiny YAML walk), so it runs on the Linux lint job on
every PR — not just on the gated macOS runner.
"""
from __future__ import annotations

import plistlib
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
IOS = REPO / "ios"
TVOS = REPO / "tvos" / "WitnessWall"

LAN_PLISTS = [IOS / "Support" / "Info.plist", TVOS / "Support" / "Info.plist"]
# Every Xcode project in the repo, with its privacy manifest. The first
# version of this gate walked ios/project.yml only, so the two tvOS bundles
# (the Wall reads UserDefaults in WallModel and ResidentWatch, the Top Shelf
# extension in ShelfCache) could ship without a manifest while the lint
# stayed green — the exact rejection the gate exists to catch.
PROJECTS = [
    (IOS / "project.yml", IOS / "Support" / "PrivacyInfo.xcprivacy"),
    (TVOS / "project.yml", TVOS / "Support" / "PrivacyInfo.xcprivacy"),
]
# Reason codes Apple accepts for NSPrivacyAccessedAPICategoryUserDefaults.
USERDEFAULTS_REASONS = {"CA92.1", "1C8F.1", "C56D.1", "AC6B.1"}

errors: list[str] = []


def check_ats(path: Path) -> None:
    with path.open("rb") as fh:
        plist = plistlib.load(fh)
    if "NSBonjourServices" not in plist:
        return  # does not talk to LAN devices; nothing to require
    ats = plist.get("NSAppTransportSecurity")
    if not isinstance(ats, dict) or ats.get("NSAllowsLocalNetworking") is not True:
        errors.append(
            f"{path.relative_to(REPO)}: declares NSBonjourServices but not "
            "NSAppTransportSecurity.NSAllowsLocalNetworking=true — ATS will refuse "
            "every http://<canary>.local request on device"
        )
    if ats and ats.get("NSAllowsArbitraryLoads"):
        errors.append(
            f"{path.relative_to(REPO)}: NSAllowsArbitraryLoads is set — that is a "
            "blanket cleartext exemption; the apps promise LOCAL networking only"
        )


def targets_using_user_defaults(project_yml: Path) -> dict[str, list[str]]:
    """target name -> source paths, for app/extension targets (not tests)."""
    text = project_yml.read_text(encoding="utf-8")
    targets: dict[str, list[str]] = {}
    in_targets = False
    current: str | None = None
    current_type = ""
    in_sources = False
    for line in text.splitlines():
        if re.match(r"^targets:\s*$", line):
            in_targets = True
            continue
        if in_targets and re.match(r"^\S", line):  # left the targets: block
            in_targets = False
        if not in_targets:
            continue
        m = re.match(r"^  ([A-Za-z0-9_]+):\s*$", line)
        if m:
            current = m.group(1)
            current_type = ""
            in_sources = False
            targets[current] = []
            continue
        if current is None:
            continue
        m = re.match(r"^    type:\s*(\S+)", line)
        if m:
            current_type = m.group(1)
            if current_type not in ("application", "app-extension", "application.watchapp2", "application.watchapp"):
                targets.pop(current, None)
                current = None
            continue
        if re.match(r"^    sources:\s*$", line):
            in_sources = True
            continue
        if in_sources:
            m = re.match(r"^      - path:\s*(\S+)", line)
            if m:
                targets[current].append(m.group(1))
            elif re.match(r"^    \S", line):
                in_sources = False
    return targets


def sources_use_user_defaults(project_root: Path, paths: list[str]) -> bool:
    for rel in paths:
        p = project_root / rel
        files = [p] if p.is_file() else list(p.rglob("*.swift")) if p.is_dir() else []
        for f in files:
            if f.suffix == ".swift" and "UserDefaults" in f.read_text(encoding="utf-8", errors="replace"):
                return True
    return False


def check_privacy_manifest(project_yml: Path, manifest_path: Path) -> None:
    rel_manifest = manifest_path.relative_to(REPO)
    rel_project = project_yml.relative_to(REPO)
    if not manifest_path.is_file():
        errors.append(f"{rel_manifest} is missing — App Store Connect rejects the upload")
        return
    with manifest_path.open("rb") as fh:
        manifest = plistlib.load(fh)
    if manifest.get("NSPrivacyTracking") is not False:
        errors.append(f"{rel_manifest}: NSPrivacyTracking must be false — the app tracks nobody")
    if manifest.get("NSPrivacyCollectedDataTypes"):
        errors.append(f"{rel_manifest}: NSPrivacyCollectedDataTypes must be empty — nothing is collected")
    kinds = {
        entry.get("NSPrivacyAccessedAPIType"): set(entry.get("NSPrivacyAccessedAPITypeReasons") or [])
        for entry in manifest.get("NSPrivacyAccessedAPITypes") or []
    }
    reasons = kinds.get("NSPrivacyAccessedAPICategoryUserDefaults")
    if not reasons or not reasons & USERDEFAULTS_REASONS:
        errors.append(
            f"{rel_manifest}: must declare NSPrivacyAccessedAPICategoryUserDefaults with an "
            f"accepted reason ({', '.join(sorted(USERDEFAULTS_REASONS))})"
        )

    targets = targets_using_user_defaults(project_yml)
    if not targets:
        errors.append(f"{rel_project}: no app/extension targets found — the walker is broken, not the project")
    for name, paths in targets.items():
        if not sources_use_user_defaults(project_yml.parent, paths):
            continue
        if not any(Path(p).name == "PrivacyInfo.xcprivacy" for p in paths):
            errors.append(
                f"{rel_project} target {name}: its sources use UserDefaults but "
                f"{rel_manifest.name} is not listed under its sources:, so the "
                "bundle ships without a privacy manifest"
            )


def main() -> int:
    for plist in LAN_PLISTS:
        if plist.is_file():
            check_ats(plist)
        else:
            errors.append(f"{plist.relative_to(REPO)} not found")
    for project_yml, manifest_path in PROJECTS:
        check_privacy_manifest(project_yml, manifest_path)
    if errors:
        for e in errors:
            print(f"::error::{e}")
        print("lint_apple_plists: FAIL")
        return 1
    print("lint_apple_plists: OK — ATS allows local networking only on both LAN apps; "
          "PrivacyInfo.xcprivacy declares UserDefaults and rides in every iOS and tvOS bundle that reads it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
