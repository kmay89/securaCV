#!/usr/bin/env python3
"""One advisory policy, two files: .cargo/audit.toml (cargo-audit, the CI gate)
and deny.toml (a local `cargo deny check`) must ignore the same advisories.
Run from audit.yml; exits 1 naming what differs."""
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def ignores(rel: str) -> set[str]:
    doc = tomllib.loads((ROOT / rel).read_text(encoding="utf-8"))
    return {e["id"] if isinstance(e, dict) else e for e in doc.get("advisories", {}).get("ignore", [])}


audit, deny = ignores(".cargo/audit.toml"), ignores("deny.toml")
if audit != deny:
    sys.exit(f"advisory ignore lists differ: only in .cargo/audit.toml {sorted(audit - deny)}, "
             f"only in deny.toml {sorted(deny - audit)}. Move them together.")
print(f"advisory ignore lists agree: {sorted(audit)}")
