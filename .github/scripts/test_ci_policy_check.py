#!/usr/bin/env python3
"""Unit tests for the CI policy checker's newer rules (.github/CI.md).

Run:  python3 -m unittest discover -s .github/scripts -p 'test_*.py' -v

R9 rides on a tiny shell tokenizer (which words sit in COMMAND position), and
a tokenizer without tests is a rule that drifts the first time someone adds a
`sudo -E` or an `echo "python3 ..."`. R3's eviction half is a single string
test on the group name; the cases below pin what counts as a per-commit group
and what an exemption looks like, so the rule cannot silently widen or narrow.
R10 reuses the same tokenizer for "is this job installing a toolchain inline";
its cases pin that the composite actions' callers (which `source` the emsdk
env, or pass pins as inputs) are NOT flagged, and that the inline copies the
rule exists to retire ARE.
"""

from __future__ import annotations

import os
import sys
import tempfile
import textwrap
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ci_policy_check as cpc  # noqa: E402


class PythonCommandDetection(unittest.TestCase):
    def test_command_position_is_detected(self):
        for run in (
            "python3 scripts/lint_spelling.py",
            "python3 -m pip install --quiet pyyaml",
            "pip install -r requirements_test.txt",
            "ruff check custom_components",
            "  mypy",
            "pytest tests/ -q",
            "cargo test && python3 tools/gen.py",
            "if ! python3 x.py; then exit 1; fi",
            'ver=$(python3 -c "print(1)")',
            "FOO=1 BAR=2 python3 x.py",
            "time python3 x.py",
            "sudo python3 x.py",
            "/usr/bin/python3 x.py",
            "python3 - <<'PY'\nimport sys\nPY",
        ):
            self.assertIsNotNone(cpc.python_command_in(run), run)

    def test_mentions_are_not_invocations(self):
        for run in (
            "# python3 would be nice here",
            'echo "run python3 scripts/x.py to fix"',
            "cat <<EOF\nnothing\nEOF",
            "node --test canary-local/tests/x.test.js",
            "bash scripts/lint_thing.sh",
            "print(\"python3\")",
            "pip_extras=none",
            "./pipeline.sh",
            "",
        ):
            self.assertIsNone(cpc.python_command_in(run), run)


def _write_workflow(tmpdir: str, body: str) -> str:
    path = os.path.join(tmpdir, "wf.yml")
    with open(path, "w", encoding="utf-8") as f:
        f.write(textwrap.dedent(body))
    return path


BASE_POLICY = {"timeout_max": 120, "unfiltered_ok": ["wf.yml"]}


def _problems(body: str, policy: dict | None = None) -> list[str]:
    with tempfile.TemporaryDirectory() as d:
        return cpc.check_workflow(_write_workflow(d, body), policy or BASE_POLICY)


class R9SetupPython(unittest.TestCase):
    def test_python_without_setup_python_is_flagged(self):
        probs = _problems("""
            on: {workflow_dispatch: {}}
            permissions: {contents: read}
            jobs:
              lint:
                runs-on: ubuntu-latest
                timeout-minutes: 5
                steps:
                  - uses: actions/checkout@v7
                  - run: python3 scripts/lint_spelling.py
        """)
        self.assertTrue(any("R9" in p and "`lint`" in p for p in probs), probs)

    def test_setup_python_satisfies_the_rule(self):
        probs = _problems("""
            on: {workflow_dispatch: {}}
            permissions: {contents: read}
            jobs:
              lint:
                runs-on: ubuntu-latest
                timeout-minutes: 5
                steps:
                  - uses: actions/checkout@v7
                  - uses: actions/setup-python@v7
                    with: {python-version-file: pyproject.toml}
                  - run: python3 scripts/lint_spelling.py
        """)
        self.assertFalse(any("R9" in p for p in probs), probs)

    def test_shell_python_counts(self):
        probs = _problems("""
            on: {workflow_dispatch: {}}
            permissions: {contents: read}
            jobs:
              j:
                runs-on: ubuntu-latest
                timeout-minutes: 5
                steps:
                  - shell: python
                    run: print("hi")
        """)
        self.assertTrue(any("R9" in p and "shell: python" in p for p in probs), probs)

    def test_exemption_is_per_job(self):
        wf = """
            on: {workflow_dispatch: {}}
            permissions: {contents: read}
            jobs:
              j:
                runs-on: ubuntu-latest
                timeout-minutes: 5
                steps:
                  - run: pip install x
        """
        self.assertTrue(any("R9" in p for p in _problems(wf)))
        policy = dict(BASE_POLICY, system_python_ok=["wf.yml:j"])
        self.assertFalse(any("R9" in p for p in _problems(wf, policy)))


class R9LocalCompositeAction(unittest.TestCase):
    """A local composite action that carries actions/setup-python inside it
    satisfies R9 for the job using it — the shape .github/actions/setup-
    platformio has, and the reason a PlatformIO job needs no setup-python
    step of its own."""

    WF = """
        on: {workflow_dispatch: {}}
        permissions: {contents: read}
        jobs:
          build:
            runs-on: ubuntu-latest
            timeout-minutes: 5
            steps:
              - uses: actions/checkout@v7
              - uses: ./.github/actions/%s
              - run: python3 firmware/scripts/x.py
    """

    def _with_action(self, name: str, action_yaml: str, wf_body: str):
        with tempfile.TemporaryDirectory() as d:
            adir = os.path.join(d, ".github", "actions", name)
            os.makedirs(adir)
            with open(os.path.join(adir, "action.yml"), "w", encoding="utf-8") as f:
                f.write(textwrap.dedent(action_yaml))
            saved = cpc.REPO_ROOT
            cpc.REPO_ROOT = d
            try:
                return cpc.check_workflow(_write_workflow(d, wf_body), BASE_POLICY)
            finally:
                cpc.REPO_ROOT = saved

    def test_composite_with_setup_python_inside_satisfies_r9(self):
        probs = self._with_action("setup-toolchain", """
            name: t
            runs:
              using: composite
              steps:
                - uses: actions/setup-python@v7
                  with: {python-version: '3.11'}
                - shell: bash
                  run: pip install --upgrade platformio
        """, self.WF % "setup-toolchain")
        self.assertFalse(any("R9" in p for p in probs), probs)

    def test_composite_without_setup_python_does_not(self):
        probs = self._with_action("setup-other", """
            name: t
            runs:
              using: composite
              steps:
                - shell: bash
                  run: echo hi
        """, self.WF % "setup-other")
        self.assertTrue(any("R9" in p and "`build`" in p for p in probs), probs)


class R10InlineToolchains(unittest.TestCase):
    def test_inline_installs_are_detected(self):
        for run in (
            "pip install --upgrade platformio intelhex",
            'pip install "platformio==6.1.19" "cryptography==50.0.1"',
            "python -m pip install platformio",
            "python3 -m pip install --upgrade pip && pip3 install platformio",
            "git clone --depth 1 https://github.com/emscripten-core/emsdk.git /tmp/emsdk",
            "curl -sSL https://example.invalid/emsdk.tar.gz | tar xz",
            "/tmp/emsdk/emsdk install 6.0.3",
            "sudo /opt/emsdk/emsdk install latest",
        ):
            self.assertIsNotNone(cpc.inline_toolchain_in(run), run)

    def test_callers_and_mentions_are_not_flagged(self):
        for run in (
            "source /tmp/emsdk/emsdk_env.sh\ncd canary-local/emulator\n./build.sh all",
            "/tmp/emsdk/emsdk activate 6.0.3",
            "# pip install platformio used to live here",
            'echo "pip install platformio"',
            "pip install pyyaml",
            "python -m pip install --upgrade pip",
            "pio run -e canary-display-watch",
            "",
        ):
            self.assertIsNone(cpc.inline_toolchain_in(run), run)

    def _wf(self, step: str) -> str:
        return """
            on: {workflow_dispatch: {}}
            permissions: {contents: read}
            jobs:
              j:
                runs-on: ubuntu-latest
                timeout-minutes: 5
                steps:
                  - uses: actions/checkout@v7
                  - uses: actions/setup-python@v7
                    with: {python-version-file: pyproject.toml}
                  - %s
        """ % step

    def test_inline_platformio_install_is_a_violation(self):
        probs = _problems(self._wf("run: pip install --upgrade platformio"))
        self.assertTrue(any("R10" in p and "`j`" in p for p in probs), probs)

    def test_inline_emsdk_clone_is_a_violation(self):
        probs = _problems(self._wf(
            "run: git clone --depth 1 https://github.com/emscripten-core/emsdk.git /tmp/emsdk"))
        self.assertTrue(any("R10" in p for p in probs), probs)

    def test_composite_action_callers_pass(self):
        probs = _problems(self._wf("uses: ./.github/actions/setup-platformio"))
        self.assertFalse(any("R10" in p for p in probs), probs)
        probs = _problems(self._wf("run: source /tmp/emsdk/emsdk_env.sh && emcc --version"))
        self.assertFalse(any("R10" in p for p in probs), probs)

    def test_exemption_is_per_job(self):
        wf = self._wf("run: pip install platformio")
        self.assertTrue(any("R10" in p for p in _problems(wf)))
        policy = dict(BASE_POLICY, inline_toolchain_ok=["wf.yml:j"])
        self.assertFalse(any("R10" in p for p in _problems(wf, policy)))


class R3Eviction(unittest.TestCase):
    TEST_WF = """
        on:
          push: {branches: [main], paths: ['x/**', '.github/workflows/wf.yml']}
          pull_request: {paths: ['x/**', '.github/workflows/wf.yml']}
        permissions: {contents: read}
        concurrency:
          group: %s
          cancel-in-progress: ${{ github.event_name == 'pull_request' }}
        jobs:
          t:
            runs-on: ubuntu-latest
            timeout-minutes: 5
            steps:
              - uses: actions/checkout@v7
    """

    def test_shared_per_ref_group_on_branch_pushes_is_flagged(self):
        probs = _problems(self.TEST_WF % "wf-${{ github.ref }}")
        self.assertTrue(any("R3" in p and "evicts" in p for p in probs), probs)

    def test_per_commit_group_passes(self):
        group = "wf-${{ github.ref }}-${{ github.event_name == 'pull_request' && 'pr' || github.sha }}"
        probs = _problems(self.TEST_WF % group)
        self.assertFalse(any("R3" in p for p in probs), probs)

    def test_publisher_exemption(self):
        policy = dict(BASE_POLICY, main_queue_ok=["wf.yml"])
        probs = _problems(self.TEST_WF % "wf-${{ github.ref }}", policy)
        self.assertFalse(any("R3" in p for p in probs), probs)

    def test_pull_request_only_workflow_is_not_a_branch_push(self):
        probs = _problems("""
            on:
              pull_request: {paths: ['x/**', '.github/workflows/wf.yml']}
            permissions: {contents: read}
            concurrency:
              group: wf-${{ github.ref }}
              cancel-in-progress: ${{ github.event_name == 'pull_request' }}
            jobs:
              t:
                runs-on: ubuntu-latest
                timeout-minutes: 5
                steps:
                  - uses: actions/checkout@v7
        """)
        self.assertFalse(any("R3" in p for p in probs), probs)


if __name__ == "__main__":
    unittest.main()
