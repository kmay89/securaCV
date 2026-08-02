#!/usr/bin/env python3
"""Tests for the duplicate-compile half of lint_common_lib_manifests.py.

That lint's older half guards the UNDEFINED direction — a path-prefixed
include whose .cpp nobody compiles. This half guards the opposite, and the
two failures look nothing alike at the point where you read them:

    too few sources  -> `undefined reference to canary::color::wash_stops`
    too many sources -> `multiple definition of canary::color::gamma8`

Neither appears while the files are compiling, which is the part everyone
watches. `canary-display-nightstand7` shipped with the second one: it copied
a `build_src_filter` block from `nightstand-s3`, where the block is genuinely
required, into an env that inherits `lib_ldf_mode = chain` from the dash and
`lib_extra_dirs = ../../common` from the base — a combination that already
builds those directories as LDF libraries. Every symbol in
firmware/common/color linked twice and main went red.

Two defects were found in the guard itself by the fixtures below, before it
worked:

1. **The walk did not follow `extends = env:<name>`.** Section keys carry the
   prefix (`[env:canary-display-dash]`), so normalising the value to `dash`
   produced a key matching nothing and silently ended the chain. The guard
   passed the tree, passed a hand-written direct case, and was blind to the
   real bug — where BOTH halves arrive through `extends` and the offending
   section states neither.

2. **A commented-out `lib_extra_dirs` must not count.** These .ini files carry
   long explanatory comments that name the very settings being discussed, so a
   check that reads raw text rather than code sees settings that are not set.

The narrowness is deliberate. Only `chain` + `lib_extra_dirs` + explicit
sources is broken; the other three combinations all link and all exist in the
tree today. A false positive here would block correct work and teach people to
route around the check.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from lint_common_lib_manifests import (  # noqa: E402
    ENVS,
    duplicate_compile_problems,
)

SRCS = """
build_src_filter =
    +<*>
    +<../../../common/color/color_engine.cpp>
    +<../../../common/color/look_engine.cpp>
"""


def flags(ini_text):
    """Run the check over a throwaway env dir; return its problems."""
    with tempfile.TemporaryDirectory() as d:
        (pathlib.Path(d) / "fixture.ini").write_text(ini_text)
        return duplicate_compile_problems(d)


class CompiledTwice(unittest.TestCase):
    """The shapes that would link `multiple definition of ...`."""

    def test_both_halves_inherited(self):
        """The real regression: the env itself states neither half."""
        problems = flags(f"""
[base]
lib_extra_dirs =
    ../../common

[env:dash]
extends = base
lib_ldf_mode = chain

[env:nightstand7]
extends = env:dash
{SRCS}
""")
        self.assertTrue(problems, "inherited chain + extra + sources must flag")
        self.assertIn("nightstand7", problems[0])

    def test_stated_directly(self):
        self.assertTrue(flags(f"""
[env:direct]
lib_ldf_mode = chain
lib_extra_dirs =
    ../../common
{SRCS}
"""))

    def test_build_src_filter_is_inherited_too(self):
        """The child adds only `chain`; the sources come from the parent.

        Reading just the child's lines sees no sources and skips it — a false
        negative on exactly this guard's subject. build_src_filter inherits
        through `extends` like every other option, so it has to be resolved
        the same way.
        """
        problems = flags(f"""
[base]
lib_extra_dirs =
    ../../common
{SRCS}

[env:child]
extends = base
lib_ldf_mode = chain
""")
        self.assertTrue(problems, "inherited build_src_filter must still flag")
        self.assertIn("child", problems[0])

    def test_extends_cycle_terminates(self):
        """A malformed cycle must not hang the lint — and still decide."""
        self.assertTrue(flags(f"""
[a]
extends = b
[b]
extends = a
lib_ldf_mode = chain
lib_extra_dirs =
    ../../common
[env:cyclic]
extends = a
{SRCS}
"""))


class LinksCleanly(unittest.TestCase):
    """The three shapes that really are in the tree and really do link."""

    def test_deep_plus_with_extra_dirs(self):
        """nightstand-s3 / touch169 / vision — the sources ARE required."""
        self.assertEqual(flags(f"""
[base]
lib_extra_dirs =
    ../../common

[env:nightstand-s3]
extends = base
{SRCS}
"""), [])

    def test_chain_without_extra_dirs(self):
        """nightstand-c6 / sense / sentinel — no library storage to collide."""
        self.assertEqual(flags(f"""
[c6_core3]
lib_ldf_mode = chain

[env:nightstand-c6]
extends = c6_core3
{SRCS}
"""), [])

    def test_chain_with_extra_dirs_but_no_explicit_sources(self):
        """The fixed nightstand7: the LDF supplies them, so name nothing."""
        self.assertEqual(flags("""
[base]
lib_extra_dirs =
    ../../common

[env:dash]
extends = base
lib_ldf_mode = chain

[env:nightstand7]
extends = env:dash
build_flags = -DCONFIG_DASH
"""), [])

    def test_lib_extra_dirs_pointing_elsewhere_does_not_count(self):
        """`+<../../../common/x.cpp>` CONTAINS the substring `../../common`.

        So a section with an unrelated lib_extra_dirs plus explicit sources
        looked like the broken combination to a whole-section text search.
        That is the false positive that matters: it rejects a correct config
        and tells the author to delete sources the link needs.
        """
        self.assertEqual(flags(f"""
[env:elsewhere]
lib_ldf_mode = chain
lib_extra_dirs =
    ../../vendor/thirdparty
{SRCS}
"""), [])

    def test_child_overrides_parent_lib_extra_dirs(self):
        """A child that re-points lib_extra_dirs overrides — it does not merge."""
        self.assertEqual(flags(f"""
[base]
lib_extra_dirs =
    ../../common

[env:override]
extends = base
lib_ldf_mode = chain
lib_extra_dirs =
    ../../vendor/thirdparty
{SRCS}
"""), [])

    def test_commented_out_setting_does_not_count(self):
        """These files discuss their own settings in prose; read code only."""
        self.assertEqual(flags(f"""
[env:solo]
lib_ldf_mode = chain
; lib_extra_dirs = ../../common
{SRCS}
"""), [])


class RealTreeStaysClean(unittest.TestCase):
    def test_no_env_compiles_a_common_library_twice(self):
        self.assertEqual(duplicate_compile_problems(ENVS), [])


if __name__ == "__main__":
    unittest.main()
