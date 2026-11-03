#!/usr/bin/env python3
"""The viewer-token mint step reads the same in the unit, the Wall and the docs.

examples/witnessd.service is the no-compose way to run the root `witnessd`
image (docs/container.md, "Run under systemd"). Its `docker run` set only
WITNESS_RTSP_URL, so a kernel started from it had no viewer-token file and
no pairing files: `docker exec witnessd witness_api mint-viewer-token`
refused ("no viewer token file is configured") and `POST /api/pair` stayed
off (sweep A104). docs/container.md's "Mint a Witness Wall viewer token"
step 1 names the `-e` that fixes it. So this reads that step and requires
the unit's `docker run` to carry every `-e` it names, with the same value,
on a path inside a volume the unit mounts (a token file outside the volume
is gone after `--rm`). It also holds the unit's own header to the doc's
mint command, under the container name the unit runs (`--name`, the
`docker rm -f` before it and the `docker stop` after it).

The unit is read the way systemd.syntax(7) and systemd.service(5) read it,
not the way a shell would: a line ending in a backslash joins the next one
with a space, `${NAME}` is one word holding the Environment= value, and
there is no shell. Quoting and escapes are where systemd and a plain
whitespace split part ways, so a command line holding a quote or a
backslash is refused here rather than split wrongly; so is a comment or a
blank line inside a continuation, and every `${NAME}` a command uses must
be set by an Environment= line of the same unit.

The tvOS Wall's Verification footer (WallSettingsView.swift) and
tvos/README.md's pairing paragraph named the host binary and the sidecar's
`entrypoint.sh` but not the root image's `docker exec <container>
witness_api mint-viewer-token` (sweep A105). Both are held to all three
forms here. The footer is read out of its Swift string literal by a small
lexer: one line, no unescaped quote, no `\\(` interpolation. A `Text` given a
literal is a LocalizedStringKey, which SwiftUI reads as Markdown, so the
footer may not hold a backtick, an asterisk, a square bracket or an angle
bracket either: to a CommonMark reader `<container>` is an HTML tag, and
what SwiftUI draws for one is not proven here (no Swift toolchain), so the
footer spells the placeholder CONTAINER, Docker's own usage word.

docs/frigate_integration.md sent the receipt to "Settings → Pair"; the
section is the one that holds the receipt field, and its header is read
from the Swift file. Every "Settings → Pair / Pairing / Verification" in
docs/ and tvos/ must name that header, and the four guides that walk a
pairing must name it. docs/audit/ is not read: a dated audit quotes the
wrong pointer it found.

This file adds no rows to test_docs_claims.py on purpose: that gate pins
retired sentences, and these are live pointers read off their sources.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_witnessd_unit.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests), unfiltered
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
UNIT = REPO / "examples/witnessd.service"
CONTAINER_DOC = REPO / "docs/container.md"
WALL_SETTINGS = REPO / "tvos/WitnessWall/Sources/WitnessWall/WallSettingsView.swift"
TVOS_README = REPO / "tvos/README.md"

# The guides that walk a Wall pairing and say where the receipt goes.
PAIRING_GUIDES = (
    "docs/container.md",
    "docs/frigate_integration.md",
    "tvos/README.md",
    "tvos/discovery/DISCOVERY.md",
)

# Names the Wall's pairing section has had or could be given. Any
# "Settings → <one of these>" in docs/ or tvos/ must be the live header.
PAIRING_SECTION_NAMES = ("Pairing", "Pair", "Verification")

# Text files scanned for a stale Settings pointer.
SCANNED_SUFFIXES = {".md", ".swift", ".txt", ".html", ".yml", ".yaml", ".json"}

# `docker run` options this unit uses that take a value, and its flags.
# An option outside both sets is refused, so a new one cannot be misread
# as the image.
DOCKER_RUN_VALUE_OPTS = {
    "--name", "-e", "--env", "-p", "--publish", "-v", "--volume",
}
DOCKER_RUN_FLAGS = {"--rm"}

# The three ways to mint, as the footer and the README must give them.
ROOT_FORM = re.compile(r"docker exec (\S+) witness_api mint-viewer-token")
SIDECAR_FORM = re.compile(r"entrypoint\.sh mint-viewer-token")
HOST_FORM = re.compile(r"witness_api mint-viewer-token")


def norm(text: str) -> str:
    """Collapse whitespace, so a Markdown reflow cannot hide a phrase."""
    return re.sub(r"\s+", " ", text)


# ---------------------------------------------------------------- the unit


def unit_header(text: str) -> str:
    """The comment block above the first section, without the `#` marks."""
    lines = []
    for raw in text.splitlines():
        if raw.startswith("["):
            break
        if raw.startswith("#"):
            lines.append(raw[1:])
    return "\n".join(lines)


def unit_entries(text: str) -> list[tuple[str, str, str]]:
    """(section, key, value) in file order, as systemd.syntax(7) reads them."""
    entries: list[tuple[str, str, str]] = []
    section = ""
    pending: str | None = None
    for number, raw in enumerate(text.splitlines(), start=1):
        if raw.rstrip() != raw and raw.rstrip().endswith("\\"):
            raise ValueError(f"line {number}: whitespace after a continuation backslash")
        line = raw.strip()
        if pending is not None and (not line or line[0] in "#;"):
            raise ValueError(
                f"line {number}: a blank or comment line inside a continuation "
                "(this reader does not model it, so it refuses it)"
            )
        if pending is None:
            if not line or line[0] in "#;":
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                continue
        if line.endswith("\\"):
            pending = (pending or "") + line[:-1] + " "
            continue
        full = (pending or "") + line
        pending = None
        key, sep, value = full.partition("=")
        if not sep:
            raise ValueError(f"line {number}: no '=' in {full!r}")
        entries.append((section, key.strip(), value.strip()))
    if pending is not None:
        raise ValueError("the file ends inside a continuation")
    return entries


def command_words(value: str) -> list[str]:
    """Split an Exec*= command line the way systemd does, or refuse it.

    systemd.service(5) splits on whitespace and honors quotes and C escapes;
    a value with none of those splits the same under both rules, and the
    leading `-` (ignore failure) prefix is dropped from the path.
    """
    for ch in "\"'\\":
        if ch in value:
            raise ValueError(f"command line holds {ch!r}, which a whitespace split misreads")
    words = value.split()
    if words and words[0][0] in "-@:+!":
        words[0] = words[0].lstrip("-@:+!")
    return words


def docker_run(words: list[str]) -> tuple[dict[str, list[str]], str, list[str]]:
    """The options, image and container command of a `docker run` argv."""
    if words[:2] != ["/usr/bin/docker", "run"]:
        raise ValueError(f"ExecStart is not a docker run: {words[:2]}")
    opts: dict[str, list[str]] = {}
    it = iter(words[2:])
    for word in it:
        if word in DOCKER_RUN_FLAGS:
            opts.setdefault(word, []).append("")
        elif word in DOCKER_RUN_VALUE_OPTS:
            try:
                opts.setdefault(word, []).append(next(it))
            except StopIteration:
                raise ValueError(f"{word} has no value") from None
        elif word.startswith("-"):
            raise ValueError(f"docker run option this reader does not know: {word}")
        else:
            return opts, word, list(it)
    raise ValueError("docker run names no image")


def unit_env_vars(opts: dict[str, list[str]]) -> dict[str, str]:
    env: dict[str, str] = {}
    for raw in opts.get("-e", []) + opts.get("--env", []):
        name, sep, value = raw.partition("=")
        if not sep:
            raise ValueError(f"-e {raw!r} passes the caller's environment, not a value")
        env[name] = value
    return env


def volume_targets(opts: dict[str, list[str]]) -> list[str]:
    targets = []
    for raw in opts.get("-v", []) + opts.get("--volume", []):
        parts = raw.split(":")
        if len(parts) < 2:
            raise ValueError(f"-v {raw!r} is an anonymous volume")
        targets.append(parts[1].rstrip("/"))
    return targets


# ----------------------------------------------------------- the container doc


def doc_section(text: str, heading: str) -> str:
    start = text.index(heading)
    nxt = re.search(r"^#{2,3} ", text[start + len(heading):], re.MULTILINE)
    end = start + len(heading) + nxt.start() if nxt else len(text)
    return text[start:end]


def doc_pairing_env(text: str) -> dict[str, str]:
    """Every `-e NAME=VALUE` code span in the mint section's step 1."""
    section = doc_section(text, "### Mint a Witness Wall viewer token")
    step1 = section[section.index("\n1. "):section.index("\n2. ")]
    return dict(re.findall(r"`-e ([A-Z][A-Z0-9_]*)=([^`\s]+)`", step1))


def doc_mint_command(text: str) -> str:
    """The mint command of step 2, without its container placeholder."""
    section = doc_section(text, "### Mint a Witness Wall viewer token")
    match = re.search(r"docker exec <container> (witness_api mint-viewer-token) \\", section)
    if not match:
        raise ValueError("docs/container.md's mint step no longer reads "
                         "`docker exec <container> witness_api mint-viewer-token \\`")
    return match.group(1)


# ----------------------------------------------------------------- the Wall


SWIFT_ESCAPES = set('"\\nrt0\'')


def swift_literal(source: str, start: int) -> tuple[str, int]:
    """Decode the single-line Swift string literal whose quote is at `start`.

    Returns the decoded text and the index after the closing quote. Refuses
    an interpolation, an unknown escape and a literal that reaches the end
    of its line, which is what an unescaped quote in the middle leaves.
    """
    if source[start] != '"' or source.startswith('"""', start):
        raise ValueError("not a single-line string literal")
    out = []
    i = start + 1
    while i < len(source):
        ch = source[i]
        if ch == "\n":
            raise ValueError("string literal runs past its line")
        if ch == '"':
            return "".join(out), i + 1
        if ch == "\\":
            esc = source[i + 1]
            if esc == "(":
                raise ValueError("string literal interpolates")
            if esc == "u":
                close = source.index("}", i)
                out.append(chr(int(source[i + 3:close], 16)))
                i = close + 1
                continue
            if esc not in SWIFT_ESCAPES:
                raise ValueError(f"unknown escape \\{esc}")
            out.append({"n": "\n", "r": "\r", "t": "\t", "0": "\0"}.get(esc, esc))
            i += 2
            continue
        out.append(ch)
        i += 1
    raise ValueError("unterminated string literal")


def text_call_literal(source: str, after: int) -> tuple[str, int]:
    """The literal of the first `Text("…")` call at or after `after`."""
    at = source.index('Text("', after)
    text, end = swift_literal(source, at + len("Text("))
    if source[end] != ")":
        raise ValueError(f"Text literal not closed by ')': {source[end:end + 20]!r}")
    return text, end


def pairing_section(source: str) -> tuple[str, str]:
    """(header, footer) of the Section that holds the receipt field."""
    field = source.index('TextField("Pairing receipt')
    header_at = source.index("header: {", field)
    header, after_header = text_call_literal(source, header_at)
    footer_at = source.index("footer: {", after_header)
    # Only comments may sit between `footer: {` and the Text.
    between = source[footer_at + len("footer: {"):source.index('Text("', footer_at)]
    for line in between.splitlines():
        if line.strip() and not line.strip().startswith("//"):
            raise ValueError(f"something other than a comment before the footer Text: {line!r}")
    footer, _ = text_call_literal(source, footer_at)
    return header, footer


def mint_forms(text: str) -> dict[str, list[str]]:
    """Which of the three mint forms a text gives (root: the container names)."""
    text = norm(text)
    root = ROOT_FORM.findall(text)
    rest = ROOT_FORM.sub(" ", text)
    return {
        "root": root,
        "sidecar": SIDECAR_FORM.findall(rest),
        # Neither docker form: the host binary's own command.
        "host": HOST_FORM.findall(rest),
    }


def readme_pairing_window(text: str) -> str:
    """tvos/README.md from "mints a viewer token" to its Settings pointer."""
    text = norm(text)
    start = text.index("mints a viewer token on the hub")
    end = text.index("Settings →", start)
    return text[start:end + 80]


def scanned_files() -> list[Path]:
    files = []
    for root in ("docs", "tvos"):
        for path in sorted((REPO / root).rglob("*")):
            rel = path.relative_to(REPO).as_posix()
            if rel.startswith("docs/audit/") or not path.is_file():
                continue
            if path.suffix in SCANNED_SUFFIXES:
                files.append(path)
    return files


SETTINGS_POINTER = re.compile(
    r"Settings\s*(?:→|->)\s*\**(" + "|".join(PAIRING_SECTION_NAMES) + r")\b"
)


class UnitCarriesTheDocsPairingStep(unittest.TestCase):
    def setUp(self) -> None:
        self.unit_text = UNIT.read_text(encoding="utf-8")
        self.entries = unit_entries(self.unit_text)
        self.doc = CONTAINER_DOC.read_text(encoding="utf-8")
        starts = [v for s, k, v in self.entries if s == "Service" and k == "ExecStart"]
        self.assertEqual(len(starts), 1, "the unit has exactly one ExecStart")
        self.opts, self.image, self.cmd = docker_run(command_words(starts[0]))

    def test_doc_step_names_an_env(self) -> None:
        # A reworded step that names no `-e` would make the next test vacuous.
        env = doc_pairing_env(self.doc)
        self.assertIn("WITNESS_API_TOKEN_PATH", env, env)

    def test_docker_run_carries_every_env_the_pairing_step_names(self) -> None:
        unit_env = unit_env_vars(self.opts)
        for name, value in doc_pairing_env(self.doc).items():
            self.assertEqual(
                unit_env.get(name), value,
                f"examples/witnessd.service's docker run must carry -e {name}={value} "
                f"(docs/container.md, 'Mint a Witness Wall viewer token' step 1); "
                f"it carries {sorted(unit_env)}",
            )

    def test_the_token_path_lives_in_a_mounted_volume(self) -> None:
        # --rm discards the container's own filesystem on every stop, so the
        # token file and the viewer-token file beside it must be in a volume.
        unit_env = unit_env_vars(self.opts)
        targets = volume_targets(self.opts)
        for name in doc_pairing_env(self.doc):
            path = unit_env.get(name, "")
            self.assertTrue(
                any(path.startswith(t + "/") for t in targets),
                f"{name}={path!r} is not inside any -v target {targets}",
            )
        self.assertIn("${WITNESS_DATA_DIR}:/data", self.opts.get("-v", []))
        self.assertIn("", self.opts.get("--rm", []), "the unit runs with --rm")

    def test_the_image_is_last_and_runs_its_entrypoint(self) -> None:
        # An -e after the image would be witnessd's argument, not docker's.
        self.assertEqual(self.image, "${WITNESS_IMAGE}")
        self.assertEqual(self.cmd, [], "nothing follows the image")

    def test_every_variable_a_command_uses_is_set(self) -> None:
        env = {}
        for section, key, value in self.entries:
            if section == "Service" and key == "Environment":
                name, _, val = value.partition("=")
                env[name] = val
        for section, key, value in self.entries:
            if key.startswith("Exec"):
                for word in command_words(value):
                    self.assertNotRegex(word, r"\$(?!\{)", f"{key}: unbraced $ splits on whitespace")
                    for name in re.findall(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}", word):
                        self.assertTrue(env.get(name), f"{key} uses ${{{name}}}, which no Environment= sets")

    def test_one_container_name_throughout(self) -> None:
        names = self.opts.get("--name", [])
        self.assertEqual(len(names), 1, names)
        name = names[0]
        commands = {(k, tuple(command_words(v))) for s, k, v in self.entries if k.startswith("Exec")}
        self.assertIn(("ExecStartPre", ("/usr/bin/docker", "rm", "-f", name)), commands)
        self.assertIn(("ExecStop", ("/usr/bin/docker", "stop", name)), commands)
        header = norm(unit_header(self.unit_text))
        mint = doc_mint_command(self.doc)
        want = f"docker exec {name} {mint} --label "
        self.assertTrue(
            want in header,
            f"the unit's header must name the doc's mint step under its --name: {want!r}",
        )


class ReaderRefusesWhatItCannotModel(unittest.TestCase):
    def test_quotes_and_backslashes_are_refused(self) -> None:
        for value in ('/usr/bin/docker run -e "A=b c" img', "/usr/bin/docker run -e A=b\\ c img"):
            with self.assertRaises(ValueError):
                command_words(value)

    def test_continuation_joins_with_a_space(self) -> None:
        # The backslash becomes a space; runs of whitespace are one word break.
        entries = unit_entries("[Service]\nExecStart=/usr/bin/docker run \\\n    --rm \\\n    img\n")
        self.assertEqual([(s, k) for s, k, _ in entries], [("Service", "ExecStart")])
        self.assertEqual(command_words(entries[0][2]), ["/usr/bin/docker", "run", "--rm", "img"])

    def test_comment_inside_a_continuation_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            unit_entries("[Service]\nExecStart=/usr/bin/docker run \\\n# note\n    img\n")

    def test_unknown_docker_option_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            docker_run(["/usr/bin/docker", "run", "--network", "host", "img"])

    def test_env_after_the_image_is_not_docker_s(self) -> None:
        opts, image, cmd = docker_run(["/usr/bin/docker", "run", "img", "-e", "A=b"])
        self.assertEqual((image, cmd), ("img", ["-e", "A=b"]))
        self.assertEqual(unit_env_vars(opts), {})

    def test_swift_lexer(self) -> None:
        self.assertEqual(swift_literal('"a \\"b\\" c")', 0), ('a "b" c', 11))
        for bad in ('"a \\(x) b"', '"a " b"\n', '"a \\q"', '"open\n'):
            with self.assertRaises(ValueError, msg=bad):
                literal, end = swift_literal(bad, 0)
                if bad[end:end + 1] != ")":
                    raise ValueError("not closed by )")


class WallNamesEveryMintForm(unittest.TestCase):
    def setUp(self) -> None:
        self.header, self.footer = pairing_section(WALL_SETTINGS.read_text(encoding="utf-8"))

    def test_footer_gives_the_three_forms(self) -> None:
        forms = mint_forms(self.footer)
        for kind in ("host", "root", "sidecar"):
            self.assertTrue(forms[kind], f"the Verification footer has no {kind} form: {self.footer}")
        self.assertEqual(forms["root"], ["CONTAINER"], "the root form's placeholder is CONTAINER")

    def test_footer_is_plain_text_to_swiftui_markdown(self) -> None:
        for ch in "`*<>[]":
            self.assertNotIn(ch, self.footer, f"{ch!r} is Markdown to a Text literal")

    def test_readme_gives_the_three_forms(self) -> None:
        window = readme_pairing_window(TVOS_README.read_text(encoding="utf-8"))
        forms = mint_forms(window)
        for kind in ("host", "root", "sidecar"):
            self.assertTrue(forms[kind], f"tvos/README.md's pairing paragraph has no {kind} form: {window}")
        self.assertEqual(forms["root"], ["<container>"], "the README's placeholder is docs/container.md's")
        self.assertIn(f"Settings → {self.header}", window)


class SettingsPointersNameTheLiveSection(unittest.TestCase):
    def setUp(self) -> None:
        self.header, _ = pairing_section(WALL_SETTINGS.read_text(encoding="utf-8"))

    def test_header_is_read(self) -> None:
        self.assertEqual(self.header, "Verification")

    def test_no_stale_pointer_in_docs_or_tvos(self) -> None:
        files = scanned_files()
        self.assertGreater(len(files), 50, "the scan found the trees")
        stale = []
        for path in files:
            text = norm(path.read_text(encoding="utf-8", errors="replace"))
            for name in SETTINGS_POINTER.findall(text):
                if name != self.header:
                    stale.append(f"{path.relative_to(REPO)}: Settings → {name}")
        self.assertEqual(stale, [], f"the Wall's pairing section is Settings → {self.header}")

    def test_pairing_guides_name_the_section(self) -> None:
        for rel in PAIRING_GUIDES:
            text = norm((REPO / rel).read_text(encoding="utf-8"))
            self.assertTrue(f"Settings → {self.header}" in text,
                            f"{rel} does not send the receipt to Settings → {self.header}")


if __name__ == "__main__":
    unittest.main()
