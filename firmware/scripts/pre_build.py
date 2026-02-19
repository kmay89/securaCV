#!/usr/bin/env python3
"""
SecuraCV Canary — Pre-Build Validation

Runs automatically before PlatformIO builds.
Add to platformio.ini:
    extra_scripts = pre:../../scripts/pre_build.py
"""

Import("env")
import os
import re
import glob as globmod

firmware_dir = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
project_dir = env.get("PROJECT_DIR", ".")

errors = []
warnings = []


def check_file(filepath, pattern, message, is_error=True):
    """Search file for regex pattern. Report if found."""
    if not os.path.exists(filepath):
        return
    with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
        content = f.read()
    for m in re.finditer(pattern, content):
        line_num = content[: m.start()].count("\n") + 1
        rel_path = os.path.relpath(filepath, firmware_dir)
        if is_error:
            errors.append(f"  {rel_path}:{line_num} — {message}")
        else:
            warnings.append(f"  {rel_path}:{line_num} — {message}")


# ── Collect all source files across firmware tree ──
src_files = []
scan_dirs = [
    os.path.join(firmware_dir, "canary"),
    os.path.join(firmware_dir, "common"),
    os.path.join(firmware_dir, "projects"),
]

for scan_dir in scan_dirs:
    if not os.path.isdir(scan_dir):
        continue
    for ext in ("*.ino", "*.h", "*.cpp"):
        src_files.extend(
            globmod.glob(os.path.join(scan_dir, "**", ext), recursive=True)
        )

for f in src_files:
    # ERROR: Deprecated mbedTLS _ret functions
    check_file(
        f,
        r"mbedtls_\w+_ret\s*\(",
        "Deprecated mbedtls _ret() — won't compile on Core 3.x",
        True,
    )

    # ERROR: localStorage in web UI
    basename = os.path.basename(f).lower()
    if "web_ui" in basename or "webui" in basename:
        check_file(
            f,
            r"localStorage|sessionStorage|document\.cookie",
            "Browser storage API — tokens must stay in JS variables",
            True,
        )

    # WARNING: Hardcoded AP password
    check_file(
        f,
        r'"witness2026"',
        "Hardcoded AP password — should be device-unique",
        False,
    )

    # WARNING: High-precision GPS
    check_file(
        f,
        r"%\.([6-9]|\d{2,})f.*(?:lat|lon|gps)",
        "High-precision GPS format — verify coarsening",
        False,
    )

print("\n══════════════════════════════════════════════")
print("  SecuraCV Pre-Build Validation")
print("══════════════════════════════════════════════")

if errors:
    print(f"\n\033[91m✗ {len(errors)} ERROR(S) — BUILD BLOCKED:\033[0m")
    for e in errors:
        print(f"\033[91m{e}\033[0m")

if warnings:
    print(f"\n\033[93m⚠ {len(warnings)} WARNING(S):\033[0m")
    for w in warnings:
        print(f"\033[93m{w}\033[0m")

if not errors and not warnings:
    print("\n\033[92m✓ All pre-build checks passed\033[0m")

print("")

if errors:
    print("Fix errors before building. See firmware/LESSONS_LEARNED.md")
    env.Exit(1)
