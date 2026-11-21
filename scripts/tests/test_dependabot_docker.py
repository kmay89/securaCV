#!/usr/bin/env python3
"""Every Dockerfile's directory has a Dependabot docker entry.

The base images are pinned by digest, and a digest pin only stays fresh if
something proposes the next one. Dependabot's docker ecosystem reads the
Dockerfiles in the one directory an entry names, so a Dockerfile in a
directory with no entry keeps its digest until a person notices: the Home
Assistant app's and the backoffice's did. This holds the list to the tree.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import subprocess
import unittest
from pathlib import Path, PurePosixPath

import yaml

REPO = Path(__file__).resolve().parents[2]


def dockerfile_dirs() -> set[str]:
    out = subprocess.run(["git", "ls-files"], cwd=REPO, check=True, capture_output=True, text=True).stdout
    dirs = set()
    for line in out.splitlines():
        path = PurePosixPath(line)
        name = path.name
        if name == "Dockerfile" or name.startswith("Dockerfile.") or name.endswith(".Dockerfile"):
            parent = path.parent.as_posix()
            dirs.add("/" if parent == "." else "/" + parent)
    return dirs


class DockerEntries(unittest.TestCase):
    def test_each_dockerfile_directory_is_watched(self):
        doc = yaml.safe_load((REPO / ".github" / "dependabot.yml").read_text(encoding="utf-8"))
        entries = [u for u in doc["updates"] if u["package-ecosystem"] == "docker"]
        watched = {u["directory"] for u in entries}
        have = dockerfile_dirs()
        self.assertGreaterEqual(len(have), 4, "the Dockerfile listing came back short")
        self.assertEqual(have - watched, set(), "Dockerfile directories with no Dependabot docker entry")
        self.assertEqual(watched - have, set(), "Dependabot docker entries for directories with no Dockerfile")
        self.assertEqual(len(watched), len(entries), "a directory is listed twice")
        for u in entries:
            with self.subTest(directory=u["directory"]):
                self.assertEqual(u["schedule"], {"interval": "weekly"})
                self.assertEqual(u["commit-message"], {"prefix": "deps(docker)"})


if __name__ == "__main__":
    unittest.main()
