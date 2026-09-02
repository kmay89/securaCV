#!/usr/bin/env python3
"""Create or update the labels declared in .github/labels.yml.

Run locally (needs a token with issues:write on the repo):

    GH_TOKEN=... GH_REPO=kmay89/securaCV python3 .github/scripts/sync_labels.py

Why this exists at all: GitHub silently ignores a label named in an issue
template when that label doesn't exist in the repo. The issue still opens — it
just opens unlabeled — so a missing label takes the whole Community Ideas board
offline (it lists issues by `labels=idea`) with no error anywhere to explain it.
The labels are therefore declared in a file, applied by CI, and reviewable in a
diff. See the header of .github/labels.yml.

ADDITIVE ON PURPOSE. This creates the labels in the manifest and corrects their
color and description; it never deletes or renames a label it doesn't know
about. The repo's other labels are hand-managed, and a "reconcile to exactly
this list" sync would strip them off live issues.

Idempotent: a label that already matches its entry is reported as "ok" and left
untouched, so re-running is free and the log says what actually changed.
"""

from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

import yaml

MANIFEST = ".github/labels.yml"
API = "https://api.github.com"


def request(method: str, url: str, token: str, payload: dict | None = None):
    """One GitHub API call. Returns (status, decoded-body-or-None)."""
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", f"Bearer {token}")
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        # 30 s like every other network helper in the tooling; without a
        # timeout a stalled API connection hangs the job until its
        # timeout-minutes circuit breaker fires.
        with urllib.request.urlopen(req, timeout=30) as res:
            body = res.read()
            return res.status, (json.loads(body) if body else None)
    except urllib.error.HTTPError as err:
        body = err.read()
        try:
            return err.code, json.loads(body) if body else None
        except json.JSONDecodeError:
            return err.code, {"message": body.decode("utf-8", "replace")}
    except (urllib.error.URLError, TimeoutError, OSError) as err:
        # Network-level failure (DNS, reset, timeout): no HTTP status exists,
        # so report it as a synthetic 599 with the reason, which the caller's
        # non-2xx path already surfaces.
        return 599, {"message": f"request failed: {err}"}


def load_manifest() -> list[dict]:
    with open(MANIFEST, encoding="utf-8") as fh:
        doc = yaml.safe_load(fh) or {}
    labels = doc.get("labels") or []
    if not isinstance(labels, list) or not labels:
        sys.exit(f"{MANIFEST}: expected a non-empty `labels:` list")
    for entry in labels:
        missing = [k for k in ("name", "color", "description") if not entry.get(k)]
        if missing:
            sys.exit(f"{MANIFEST}: label {entry!r} is missing {', '.join(missing)}")
    return labels


def main() -> int:
    token = os.environ.get("GH_TOKEN")
    repo = os.environ.get("GH_REPO")
    if not token or not repo:
        sys.exit("GH_TOKEN and GH_REPO must both be set")

    created, updated, unchanged, failed = [], [], [], []

    for entry in load_manifest():
        name = entry["name"]
        # Colors are written without the leading '#' in the manifest; the API
        # rejects it either way, so normalize rather than trusting the author.
        want = {
            "name": name,
            "color": str(entry["color"]).lstrip("#"),
            "description": entry["description"],
        }
        url = f"{API}/repos/{repo}/labels/{urllib.parse.quote(name)}"

        status, existing = request("GET", url, token)
        if status == 404:
            status, body = request("POST", f"{API}/repos/{repo}/labels", token, want)
            (created if status == 201 else failed).append(
                name if status == 201 else f"{name}: {(body or {}).get('message')}"
            )
            continue
        if status != 200:
            failed.append(f"{name}: lookup returned {status}")
            continue

        drift = {
            k: v
            for k, v in want.items()
            if k != "name" and (existing or {}).get(k) != v
        }
        if not drift:
            unchanged.append(name)
            continue

        status, body = request("PATCH", url, token, want)
        (updated if status == 200 else failed).append(
            f"{name} ({', '.join(sorted(drift))})"
            if status == 200
            else f"{name}: {(body or {}).get('message')}"
        )

    for head, items in (
        ("created", created),
        ("updated", updated),
        ("already correct", unchanged),
        ("FAILED", failed),
    ):
        if items:
            print(f"{head}: {', '.join(items)}")

    if failed:
        print("\nSome labels did not sync. The idea board lists issues by the")
        print("`idea` label — if that one failed, the board will read empty.")
        return 1

    print("\nLabels are in sync.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
