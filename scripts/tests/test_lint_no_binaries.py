"""scripts/lint_no_binaries.py — no compiled program, object or library in git.

Builds scratch git repositories and runs the linter's `tracked()` and
`problems()` on them: a committed ELF beside its test source is named, by
path and format; a Java class file, which shares the Mach-O universal magic,
is not; a symlink to a binary and an untracked or ignored binary are not the
linter's business; a PE is told from an "MZ" that is only text; and a tree
that is not a git checkout is refused (exit 2), never passed. The module's
own self-test runs here too, so a loosened magic check fails in this suite
as well as in lint.yml's step.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "lint_no_binaries", REPO / "scripts" / "lint_no_binaries.py")
lnb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lnb)  # type: ignore[union-attr]

ELF = b"\x7fELF\x02\x01\x01\x00" + b"\x00" * 120
JAVA = b"\xca\xfe\xba\xbe\x00\x00\x00\x3d" + b"\x00" * 120  # class file v61
FAT = b"\xca\xfe\xba\xbe\x00\x00\x00\x02" + b"\x00" * 120
AR = b"!<arch>\n" + b"x" * 120


def pe(sig_off: int) -> bytes:
    b = bytearray(b"MZ" + b"\x00" * (sig_off + 64))
    b[0x3C:0x40] = struct.pack("<I", sig_off)
    b[sig_off:sig_off + 4] = b"PE\x00\x00"
    return bytes(b)


@unittest.skipUnless(shutil.which("git"), "git is needed to list tracked files")
class TrackedTree(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, rel: str, data: bytes, add: bool = True):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        if add:
            subprocess.run(["git", "-C", str(self.root), "add", "-f", rel],
                           check=True)

    def found(self):
        found, unreadable = lnb.problems(self.root, lnb.tracked(self.root))
        self.assertEqual(unreadable, [])
        return found

    def test_clean_tree_passes(self):
        self.put("firmware/tests_host/test_x.cpp", b"int main() { return 0; }\n")
        self.put("canary-local/emulator/dist/core.wasm", b"\x00asm\x01\x00\x00\x00")
        self.put("docs/logo.png", b"\x89PNG\r\n\x1a\n" + b"\x00" * 40)
        self.put("tools/Demo.class", JAVA)
        self.put("README.md", b"MZ is how this line starts.\n" * 4)
        self.assertEqual(self.found(), [])

    def test_the_committed_test_binary_is_named(self):
        # The #1776 shape: the hand build beside the source it came from.
        self.put("firmware/tests_host/test_doorbell_audio.cpp", b"int main(){}\n")
        self.put("firmware/tests_host/test_doorbell_audio", ELF)
        self.assertEqual(self.found(),
                         ["firmware/tests_host/test_doorbell_audio: ELF"])

    def test_every_native_format_is_named(self):
        self.put("a/prog", ELF)
        self.put("b/uni", FAT)
        self.put("c/libx.a", AR)
        self.put("d/tool.exe", pe(0x80))
        self.put("e/far.dll", pe(0x400))  # signature past the first read
        self.assertEqual(sorted(self.found()), [
            "a/prog: ELF", "b/uni: Mach-O universal", "c/libx.a: ar archive",
            "d/tool.exe: PE", "e/far.dll: PE"])

    def test_untracked_ignored_and_linked_binaries_are_not_its_business(self):
        self.put(".gitignore", b"build/\n")
        self.put("build/test_y", ELF, add=False)
        self.put("loose_binary", ELF, add=False)
        self.put("real.cpp", b"// source\n")
        os.symlink("loose_binary", self.root / "link_to_binary")
        subprocess.run(["git", "-C", str(self.root), "add", "link_to_binary"],
                       check=True)
        self.assertEqual(self.found(), [])

    def test_allow_list_is_honored_and_empty(self):
        self.assertEqual(lnb.ALLOW, {})
        self.put("vendor/blob", ELF)
        lnb.ALLOW["vendor/blob"] = "test"
        try:
            self.assertEqual(self.found(), [])
        finally:
            lnb.ALLOW.clear()

    def test_unreadable_file_is_refused_not_passed(self):
        self.put("x/prog", b"plain")
        real_open = open

        def broken(path, *a, **k):
            if str(path).endswith("x/prog"):
                raise PermissionError("denied")
            return real_open(path, *a, **k)

        lnb.open = broken  # module-level name lookup; restored below
        try:
            found, unreadable = lnb.problems(self.root, ["x/prog"])
        finally:
            del lnb.open
        self.assertEqual(found, [])
        self.assertEqual(len(unreadable), 1)
        self.assertIn("x/prog", unreadable[0])


class NotACheckout(unittest.TestCase):
    def test_outside_git_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(Path(d).parent))
            old = os.environ.copy()
            os.environ.update(env)
            try:
                with self.assertRaises(SystemExit) as cm:
                    lnb.tracked(Path(d))
            finally:
                os.environ.clear()
                os.environ.update(old)
            self.assertEqual(cm.exception.code, 2)


class SelfTest(unittest.TestCase):
    def test_magic_checks_keep_their_verdicts(self):
        self.assertEqual(lnb.self_test(), [])

    def test_a_loosened_check_fails_the_self_test(self):
        real = lnb.kind
        lnb.kind = lambda head, read_at=None: None  # a check that sees nothing
        try:
            self.assertNotEqual(lnb.self_test(), [])
        finally:
            lnb.kind = real
        lnb.kind = lambda head, read_at=None: "ELF"  # one that sees everything
        try:
            self.assertNotEqual(lnb.self_test(), [])
        finally:
            lnb.kind = real


if __name__ == "__main__":
    unittest.main()
