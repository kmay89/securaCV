"""Tests for scripts/hooks/read_guard.py — the guard refuses exactly one shape of call.

Pinned: a whole read of a file over the limit is refused (exit 2, the outline
on stderr with line numbers); a ranged read of the same file, a small file, a
raster image, a non-Read tool, a call with no path, a missing file and
unparseable stdin are all allowed (exit 0, silent). READ_GUARD_OFF=1 allows
everything. Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

GUARD = Path(__file__).resolve().parents[1] / "hooks" / "read_guard.py"


def run(call, **env):
    e = {**os.environ, "READ_GUARD_LIMIT": "1000", **env}
    e.pop("READ_GUARD_OFF", None) if "READ_GUARD_OFF" not in env else None
    return subprocess.run(
        [sys.executable, "-I", str(GUARD)],
        input=call if isinstance(call, str) else json.dumps(call),
        capture_output=True, text=True, env=e,
    )


class ReadGuard(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.big = self.dir / "big.py"
        self.big.write_text("x = 1\n" * 100 + "def alpha():\n    pass\n" + "y = 2\n" * 100 + "class Beta:\n    pass\n")
        self.small = self.dir / "small.py"
        self.small.write_text("def tiny():\n    pass\n")
        self.image = self.dir / "big.png"
        self.image.write_bytes(b"\x89PNG" + b"\0" * 2000)

    def read(self, path, **extra):
        return {"tool_name": "Read", "tool_input": {"file_path": str(path), **extra}}

    def test_a_whole_read_over_the_limit_is_refused_with_an_outline(self):
        r = run(self.read(self.big))
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("a whole read is refused", r.stderr)
        self.assertIn("def alpha", r.stderr)
        self.assertIn("class Beta", r.stderr)
        self.assertRegex(r.stderr, r"\n\s+101\s+def alpha")  # line numbers, not guesses
        self.assertEqual(r.stdout, "")

    def test_a_ranged_read_of_the_same_file_is_allowed(self):
        self.assertEqual(run(self.read(self.big, offset=100, limit=5)).returncode, 0)
        self.assertEqual(run(self.read(self.big, limit=40)).returncode, 0)

    def test_everything_else_is_allowed_silently(self):
        for call in (
            self.read(self.small),
            self.read(self.image),
            self.read(self.dir / "missing.py"),
            {"tool_name": "Grep", "tool_input": {"pattern": "x", "path": str(self.big)}},
            {"tool_name": "Read", "tool_input": {}},
            "not json at all",
            "[1, 2, 3]",
        ):
            r = run(call)
            self.assertEqual(r.returncode, 0, (call, r.stderr))
            self.assertEqual(r.stderr, "", call)

    def test_the_off_switch(self):
        self.assertEqual(run(self.read(self.big), READ_GUARD_OFF="1").returncode, 0)


if __name__ == "__main__":
    unittest.main()
