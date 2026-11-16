"""The base images the add-on is built on (privacy_witness_kernel/Dockerfile)
and the two CI pins that follow them (.github/workflows/pwk-wizard-tests.yml).

What is held here:
  * every base named from a registry is written on its FROM line with a tag
    and a sha256 digest, literally. No ARG carries the release or a digest,
    so both move on the lines a dependency updater rewrites (G6-22);
  * the two runtime bases and the two Alpine stages are one Alpine release,
    and not one older than 3.22: 3.18 left security support in May 2025
    (G3-07);
  * the workflow tests on the Python and the jq the Dockerfile says that
    release ships, so moving the base without moving the two pins (or the
    reverse) fails here instead of testing on versions the image never runs.

test_addon_store.py keeps the shape of the runtime stages (official base,
selected by TARGETARCH, no CMD); this file keeps which release they are.
"""

import os
import re
from pathlib import Path

import pytest

yaml = pytest.importorskip("yaml") if not os.environ.get("CI") else __import__("yaml")

ADDON = Path(__file__).resolve().parent.parent
REPO = ADDON.parent
DOCKERFILE = (ADDON / "Dockerfile").read_text()
WORKFLOW_PATH = REPO / ".github" / "workflows" / "pwk-wizard-tests.yml"

# The oldest Alpine release Home Assistant built base images for when the
# add-on left 3.18. A floor, not an equality: moving up never fails it.
OLDEST_ALLOWED_ALPINE = (3, 22)

_FROM = re.compile(r"^FROM[ \t]+(?P<ref>\S+)(?:[ \t]+AS[ \t]+(?P<stage>\S+))?[ \t]*$", re.M)
_PINNED = re.compile(r"^(?P<image>[a-z0-9./-]+):(?P<tag>[0-9]+\.[0-9]+)@sha256:[0-9a-f]{64}$")


def _from_lines():
    """(ref, stage) of every FROM line, in order."""
    return [(m.group("ref"), m.group("stage")) for m in _FROM.finditer(DOCKERFILE)]


def _registry_bases():
    """The FROM references that name an image in a registry, i.e. everything
    that is not an earlier stage of this file."""
    refs = _from_lines()
    stages = {stage for _, stage in refs if stage}
    return [
        ref for ref, _ in refs
        if ref not in stages and not ref.startswith("runtime-")
    ]


def test_the_dockerfile_has_the_four_registry_bases_this_file_expects():
    images = [ref.split(":", 1)[0] for ref in _registry_bases()]
    assert images == [
        "ghcr.io/home-assistant/amd64-base",
        "ghcr.io/home-assistant/aarch64-base",
        "docker.io/library/alpine",
        "docker.io/library/alpine",
    ]
    # The one FROM that is neither a registry image nor a named stage is the
    # runtime selector, which resolves to one of the two stages above.
    assert ("runtime-${TARGETARCH}", None) in _from_lines()


def test_every_registry_base_is_pinned_by_tag_and_digest_on_its_from_line():
    for ref in _registry_bases():
        assert "$" not in ref, f"{ref}: the tag and digest must be literal, not an ARG"
        assert _PINNED.match(ref), f"{ref}: expected <image>:<alpine release>@sha256:<64 hex>"
    # No build argument stands in for either: an updater that rewrites FROM
    # lines would leave it behind.
    assert re.search(r"^ARG[ \t]+ALPINE_", DOCKERFILE, re.M) is None
    assert "${ALPINE" not in DOCKERFILE


def test_the_two_alpine_stages_are_the_same_image():
    alpine = [ref for ref in _registry_bases() if ref.startswith("docker.io/library/alpine:")]
    assert len(alpine) == 2 and alpine[0] == alpine[1]


def _release(ref):
    match = _PINNED.match(ref)
    assert match, ref
    return tuple(int(part) for part in match.group("tag").split("."))


def test_all_bases_are_one_alpine_release_that_is_not_older_than_the_floor():
    releases = {_release(ref) for ref in _registry_bases()}
    assert len(releases) == 1, f"the bases are on different Alpine releases: {sorted(releases)}"
    (release,) = releases
    assert release >= OLDEST_ALLOWED_ALPINE, (
        f"Alpine {release[0]}.{release[1]} is older than "
        f"{OLDEST_ALLOWED_ALPINE[0]}.{OLDEST_ALLOWED_ALPINE[1]}"
    )


def _stated_versions():
    """What the Dockerfile says the pinned release ships, from the comment
    above the runtime stages: (alpine, python, jq)."""
    match = re.search(
        r"Alpine (?P<alpine>[0-9]+\.[0-9]+) == .*?Python (?P<python>[0-9]+\.[0-9]+)"
        r".*?jq (?P<jq>[0-9]+(?:\.[0-9]+)+)",
        DOCKERFILE,
        re.S,
    )
    assert match, "the Dockerfile no longer says which Python and jq its Alpine release ships"
    return match.group("alpine"), match.group("python"), match.group("jq")


def test_the_dockerfile_states_the_versions_of_the_release_it_pins():
    alpine, _, _ = _stated_versions()
    (release,) = {_release(ref) for ref in _registry_bases()}
    assert alpine == f"{release[0]}.{release[1]}"
    # The end of security support is written beside the pin, as a date.
    assert re.search(r"security support ends [0-9]{4}-[0-9]{2}-[0-9]{2}", DOCKERFILE)


def _workflow_steps():
    workflow = yaml.safe_load(WORKFLOW_PATH.read_text())
    return workflow["jobs"]["wizard-tests"]["steps"]


def test_ci_tests_on_the_python_the_base_ships():
    _, python, _ = _stated_versions()
    pins = [
        str(step["with"]["python-version"])
        for step in _workflow_steps()
        if str(step.get("uses", "")).startswith("actions/setup-python@")
    ]
    assert pins == [python]


def test_ci_tests_on_the_jq_the_base_ships():
    _, _, jq = _stated_versions()
    want = f"jq-{jq}"
    steps = _workflow_steps()
    required = [step["env"]["PWK_REQUIRE_JQ"] for step in steps if "PWK_REQUIRE_JQ" in step.get("env", {})]
    # The pytest step and the run_lib step each name the jq they must run on.
    assert required == [want, want]
    fetch = [step for step in steps if "APP_JQ_URL" in step.get("env", {})]
    assert len(fetch) == 1
    env = fetch[0]["env"]
    assert env["APP_JQ_VERSION"] == want
    assert env["APP_JQ_URL"] == f"https://github.com/jqlang/jq/releases/download/{want}/jq-linux-amd64"
    assert re.fullmatch(r"[0-9a-f]{64}", env["APP_JQ_SHA256"])
    # The binary is checked against the digest and asked its version before
    # it goes on PATH.
    run = fetch[0]["run"]
    assert 'echo "$APP_JQ_SHA256  $dir/jq" | sha256sum -c -' in run
    assert 'test "$("$dir/jq" --version)" = "$APP_JQ_VERSION"' in run
    assert run.index("sha256sum -c -") < run.index("chmod +x") < run.index("GITHUB_PATH")
