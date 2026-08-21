#!/usr/bin/env python3
"""No product silently drops out of the flasher between releases.

The firmware-release build matrix is deliberately non-blocking: one display
SKU failing to compile must not sink the signed release around it. The cost
of that choice was silence — the failed env printed a ::warning nobody reads,
and the product vanished from the new release's manifest-flash.json with no
gate going red. A board that shipped in release N and is absent from N+1 is
an OTA/flasher regression someone has to *decide*, not a log line.

This check turns the silence into a decision: compare the freshly built
manifest-flash.json against the previous release's, and fail when a product
that used to ship is missing — unless the operator listed it in
--allow-dropped (the workflow_dispatch input), which is the explicit "yes,
ship without it" ceremony.

Additions are reported but never fail (a new board joining the catalog is
the happy path). Dev-channel runs pass --advisory: prereleases report the
same table but never block.
"""

from __future__ import annotations

import argparse
import json
import os
import sys


def product_ids(manifest_path: str) -> set[str]:
    with open(manifest_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    products = data.get("products")
    if not isinstance(products, dict) or not products:
        raise SystemExit(f"{manifest_path}: no products map — not a flash manifest?")
    return set(products.keys())


def emit_summary(lines: list[str]) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not path:
        return
    with open(path, "a", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--current", required=True, help="freshly built manifest-flash.json")
    ap.add_argument("--previous", required=True, help="previous release's manifest-flash.json")
    ap.add_argument(
        "--allow-dropped",
        default="",
        help="comma-separated product ids allowed to be absent this release",
    )
    ap.add_argument(
        "--advisory",
        action="store_true",
        help="report only, never fail (dev-channel prereleases)",
    )
    args = ap.parse_args()

    current = product_ids(args.current)
    previous = product_ids(args.previous)
    allowed = {p.strip() for p in args.allow_dropped.split(",") if p.strip()}

    dropped = sorted(previous - current)
    added = sorted(current - previous)
    blocked = [p for p in dropped if p not in allowed]
    waved = [p for p in dropped if p in allowed]

    summary = ["### Flasher catalog completeness", ""]
    summary.append(f"- products this release: **{len(current)}** (previous: {len(previous)})")
    if added:
        summary.append(f"- new: {', '.join(added)}")
    if waved:
        summary.append(f"- dropped, explicitly allowed: {', '.join(waved)}")
    if blocked:
        summary.append(f"- **DROPPED WITHOUT SIGN-OFF: {', '.join(blocked)}**")
    if not dropped and not added:
        summary.append("- same product set as the previous release")
    emit_summary(summary)

    for line in summary:
        print(line)

    unknown = sorted(allowed - set(dropped))
    if unknown:
        print(f"note: --allow-dropped names products that did not drop: {', '.join(unknown)}")

    if blocked and not args.advisory:
        print(
            "\nA product that shipped in the previous release is missing from this "
            "one (its build step warned above). Either fix the build, or re-run "
            "with allow_dropped_products naming it to consciously ship without it.",
            file=sys.stderr,
        )
        return 1
    if blocked and args.advisory:
        print("(advisory mode: dev-channel prerelease — not failing)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
