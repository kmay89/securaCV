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
`docker rm -f` before it and the `docker stop` after it), with the `sudo`
its other install steps use. The Wall runs on an Apple TV, another
machine, so while the unit publishes the API on loopback only, the
header's pairing step must say to widen the `-p` binding first and name
the host port the Wall dials.

The unit is read the way systemd.syntax(7) and systemd.service(5) read it,
not the way a shell would: a line ending in a backslash joins the next one
with a space, `${NAME}` is one word holding the Environment= value, and
there is no shell. Quoting, escapes and `%` specifiers (systemd.unit(5)
"Specifiers": `%i`, `%n` and the rest are rewritten before the command
runs) are where systemd and a plain whitespace split part ways, so a
command line holding a quote, a backslash or a `%` is refused here rather
than read wrongly; so is a comment or a blank line inside a continuation,
and every `${NAME}` a command uses must be set by an Environment= line of
the same unit.

The tvOS Wall's Verification footer (WallSettingsView.swift) and
tvos/README.md's pairing paragraph named the host binary and the sidecar's
`entrypoint.sh` but not the root image's `docker exec <container>
witness_api mint-viewer-token` (sweep A105). Both are held to all three
forms here, each with the `--label` witness_api refuses a mint without.
The footer is read out of its Swift string literal by a small lexer: one
line, no unescaped quote, no `\\(` interpolation. A `Text` given a literal
is a LocalizedStringKey, which SwiftUI reads as Markdown, so the footer
may not hold these inline openers either: a backtick (code span), an
asterisk or an underscore at a word boundary (emphasis), a tilde (GFM
strikethrough), a square bracket (link), an angle bracket (autolink, raw
HTML: to a CommonMark reader `<container>` is an HTML tag), an entity
reference such as `&amp;`, or a backslash (an escape). Nor a `%`, which a
LocalizedStringKey may read as a format specifier, nor a control
character. What SwiftUI draws for any of those is not proven here (no
Swift toolchain), so the footer avoids them all and spells the root
form's placeholder CONTAINER, Docker's own usage word. An underscore
inside a word (`witness_api`) cannot open emphasis and is allowed. A bare
URL, which a GFM autolink extension could link, is not checked (the
footer has none, and a link would not change the text it shows).

docs/frigate_integration.md sent the receipt to "Settings → Pair"; the
section is the one that holds the receipt field, and its header is read
from the Swift file. Every "Settings → Pair / Pairing / Verification" in
docs/ and tvos/ must name that header, and the guides that walk a
pairing must name it. docs/audit/ is not read: a dated audit quotes the
wrong pointer it found.

The glossary's Witness Wall entry and the fast path's Step 0
(tvos/RUN_ON_APPLE_TV.md) named only `witness_api mint-viewer-token`, the
host binary's command, which a reader on the root image cannot run as
written, and the fast path sent the receipt to "the Wall's settings"
(sweep A118). Each must now give the root form (`docker exec <container>
witness_api mint-viewer-token --label`, docs/container.md's placeholder) or
link docs/container.md's "Mint a Witness Wall viewer token", where step 1
names the token path the mint needs, and each must name the live
Settings header. That step 1 says the stock unit already passes the token
path, so a unit user skips it; the `-e` it says the unit passes is held to
the unit's own (the sentence before it also names
WITNESS_API_VIEWER_TOKEN_PATH, which the unit does not pass, so a bare "it"
could be read as either), and the container name it gives for the unit is
held to the unit's `--name`. The unit's `-e` joined it on 2026-11-03, and
a copy installed before then lacks it, so step 1 also says how to check a
copy and load the fix: the `ExecStart` to look in, then the daemon-reload
and the restart of the unit name the install copies to. Without that, the
"No restart" paragraph sends the owner of an older copy back to a step
that tells them to skip it.

docs/homeassistant_setup.md's "Viewer tokens (Witness Wall)" section, the
kernel's own reference for the credential, gave the host form and the
sidecar's (without its `--label`) but not the root image's (sweep A103,
after A118 left the section to it). It must give all three, the root form
under docs/container.md's `<container>` placeholder, and link that page's
mint walk.

A receipt pasted "into the Wall's settings" names no section a reader can
find (docs/tvos/README.md and the fast path said so; sweep A118's review).
No sentence in docs/ or tvos/ may paste into the Wall's settings without
naming the section after an arrow; the section it names is then the live
header, by the scan above.

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
    "tvos/RUN_ON_APPLE_TV.md",
    "tvos/discovery/DISCOVERY.md",
)

# The short pointers that name the mint without walking it (sweep A118):
# (file, the text that opens the paragraph holding the pointer).
MINT_POINTERS = (
    ("docs/GLOSSARY.md", "**The Witness Wall** —"),
    ("tvos/RUN_ON_APPLE_TV.md", "> **Step 0:**"),
)

# docs/container.md's mint section, which every pointer may link instead.
MINT_HEADING = "### Mint a Witness Wall viewer token"

# The kernel's reference section for the viewer token (sweep A103).
HA_SETUP = REPO / "docs/homeassistant_setup.md"
HA_VIEWER_HEADING = "### Viewer tokens (Witness Wall)"

# Names the Wall's pairing section has had or could be given. Any
# "Settings → <one of these>" in docs/ or tvos/ must be the live header.
PAIRING_SECTION_NAMES = ("Pairing", "Pair", "Verification")

# A paste into the Wall's settings that names no section: "paste" (any
# form), then within the same sentence "into the Wall's / its settings" with
# no "→ <section>" after it. Read after blockquote markers are dropped and
# whitespace collapsed.
VAGUE_SETTINGS_POINTER = re.compile(
    r"\bpast(?:e|ed|es|ing)\b[^.;]{0,160}?\binto (?:the (?:Witness )?Wall(?:'s)?|its) "
    r"\**[Ss]ettings\b(?!\**\s*(?:→|->))"
)

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
# witness_api refuses a mint with no --label (src/bin/witness_api.rs), and
# entrypoint.sh hands its arguments to witness_api, so each form carries it.
ROOT_FORM = re.compile(r"docker exec (\S+) witness_api mint-viewer-token --label ")
SIDECAR_FORM = re.compile(r"entrypoint\.sh mint-viewer-token --label ")
HOST_FORM = re.compile(r"witness_api mint-viewer-token --label ")

# What a LocalizedStringKey's Markdown reader could act on in the footer:
# each CommonMark inline opener, a `%` and a control character.
MARKDOWN_ACTIVE = (
    ("a code span", re.compile(r"`")),
    ("emphasis", re.compile(r"\*|(?<![A-Za-z0-9])_|_(?![A-Za-z0-9])")),
    ("strikethrough", re.compile(r"~")),
    ("a link", re.compile(r"[\[\]]")),
    ("an autolink or raw HTML", re.compile(r"[<>]")),
    ("an entity reference", re.compile(r"&(?:#[0-9]+|#[xX][0-9A-Fa-f]+|[A-Za-z][A-Za-z0-9]*);")),
    ("a backslash escape", re.compile(r"\\")),
    ("a format specifier", re.compile(r"%")),
    ("a control character", re.compile(r"[\x00-\x1f\x7f]")),
)


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


def header_steps(header: str) -> list[str]:
    """The header's numbered install steps, each joined with its indented lines."""
    steps: list[str] = []
    current: str | None = None
    for line in header.splitlines():
        if re.match(r"\s+\d+\.\s", line):
            if current is not None:
                steps.append(current)
            current = line.strip()
        elif current is not None and line.startswith("    ") and line.strip():
            current += " " + line.strip()
        elif current is not None:
            steps.append(current)
            current = None
    if current is not None:
        steps.append(current)
    return steps


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

    systemd.service(5) splits on whitespace and honors quotes and C escapes,
    and systemd.unit(5) rewrites `%` specifiers first; a value with none of
    those reads the same under both rules, and the leading `-` (ignore
    failure) prefix is dropped from the path.
    """
    for ch in "\"'\\%":
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


def pointer_paragraph(text: str, opener: str) -> str:
    """The paragraph that starts at `opener`, up to the next blank line.

    A blockquote's `>` markers are dropped, so a command or a link that
    wraps from one quoted line to the next reads as written.
    """
    start = text.index(opener)
    end = re.compile(r"\n[ \t>]*\n").search(text, start)
    para = text[start:end.start() if end else len(text)]
    return re.sub(r"^[ \t]*> ?", "", para, flags=re.MULTILINE)


def heading_anchor(heading: str) -> str:
    """GitHub's anchor for a Markdown heading (scripts/lint_md_links.py's rule)."""
    h = heading.lstrip("#").strip().lower().replace("`", "").replace("*", "")
    return "".join(ch for ch in h if ch.isalnum() or ch in "-_ ").replace(" ", "-")


def links_to(source: Path, text: str, target: Path) -> list[str]:
    """The #fragments of every Markdown link in `text` that resolves to `target`."""
    fragments = []
    for href in re.findall(r"\]\(([^)\s]+)\)", norm(text)):
        path, _, fragment = href.partition("#")
        if path and (source.parent / path).resolve() == target.resolve():
            fragments.append(fragment)
    return fragments


def container_step1(text: str) -> str:
    section = doc_section(text, MINT_HEADING)
    return section[section.index("\n1. "):section.index("\n2. ")]


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
        # The unit is root's (installed and run with sudo, like steps 1-3);
        # without sudo, a user outside the docker group cannot reach the socket.
        want = f"sudo docker exec {name} {mint} --label "
        self.assertTrue(
            want in header,
            f"the unit's header must name the doc's mint step under its --name: {want!r}",
        )

    def test_pairing_step_widens_a_loopback_binding(self) -> None:
        # The Wall runs on an Apple TV, another machine, and dials the
        # host's published port: a receipt minted while the unit publishes
        # on loopback only is one the Wall cannot use.
        steps = [s for s in header_steps(unit_header(self.unit_text)) if "mint-viewer-token" in s]
        self.assertEqual(len(steps), 1, f"one install step mints: {steps}")
        step = steps[0]
        publishes = self.opts.get("-p", []) + self.opts.get("--publish", [])
        self.assertEqual(len(publishes), 1, publishes)
        parts = publishes[0].rsplit(":", 2)
        self.assertEqual(len(parts), 3, f"-p {publishes[0]!r} names no host address")
        address, host_port, _ = parts
        if address in ("127.0.0.1", "[::1]", "localhost"):
            self.assertRegex(
                step, r"first widen the -p binding.*mint-viewer-token",
                f"the unit publishes on {address} only, so its pairing step must say to "
                f"widen the -p binding before it mints: {step!r}",
            )
            self.assertIn(f"port {host_port}", step, "the step names the port the Wall dials")


class ReaderRefusesWhatItCannotModel(unittest.TestCase):
    def test_quotes_and_backslashes_are_refused(self) -> None:
        for value in ('/usr/bin/docker run -e "A=b c" img', "/usr/bin/docker run -e A=b\\ c img"):
            with self.assertRaises(ValueError):
                command_words(value)

    def test_specifiers_are_refused(self) -> None:
        # systemd rewrites %i (empty outside a template) before docker runs,
        # so `--name w%i` would be compared as written and run as `--name w`.
        for value in ("/usr/bin/docker run --name w%i img", "/usr/bin/docker stop w%n"):
            with self.assertRaises(ValueError, msg=value):
                command_words(value)

    def test_header_steps(self) -> None:
        header = unit_header(
            "# Install:\n#   1. one\n#   2. two,\n#      wrapped\n#        cmd\n#\n# after\n[Unit]\n"
        )
        self.assertEqual(header_steps(header), ["1. one", "2. two, wrapped cmd"])

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
        for what, pattern in MARKDOWN_ACTIVE:
            found = pattern.search(self.footer)
            self.assertIsNone(
                found, f"{found and found.group()!r} opens {what} to a Text literal's Markdown reader"
            )

    def test_markdown_guard_catches_each_opener(self) -> None:
        # Each sample would render other than as written; the last is plain.
        def hits(text: str) -> list[str]:
            return [what for what, pattern in MARKDOWN_ACTIVE if pattern.search(text)]
        for sample in ("`x`", "*x*", "_x_ y", "x _y_", "~~x~~", "[x](y)", "<x>",
                       "a &amp; b", "a &#38; b", "a \\* b", "100%", "a\nb"):
            self.assertTrue(hits(sample), f"the guard misses {sample!r}")
        self.assertEqual(hits("witness_api mint-viewer-token --label \"tv\" — a & b …"), [])

    def test_readme_gives_the_three_forms(self) -> None:
        window = readme_pairing_window(TVOS_README.read_text(encoding="utf-8"))
        forms = mint_forms(window)
        for kind in ("host", "root", "sidecar"):
            self.assertTrue(forms[kind], f"tvos/README.md's pairing paragraph has no {kind} form: {window}")
        self.assertEqual(forms["root"], ["<container>"], "the README's placeholder is docs/container.md's")
        self.assertIn(f"Settings → {self.header}", window)


class ShortPointersReachTheRootImage(unittest.TestCase):
    def setUp(self) -> None:
        self.header, _ = pairing_section(WALL_SETTINGS.read_text(encoding="utf-8"))
        self.mint_anchor = heading_anchor(MINT_HEADING)
        doc_section(CONTAINER_DOC.read_text(encoding="utf-8"), MINT_HEADING)

    def test_each_pointer_gives_the_root_form_or_links_the_mint_walk(self) -> None:
        for rel, opener in MINT_POINTERS:
            path = REPO / rel
            with self.subTest(pointer=rel):
                para = pointer_paragraph(path.read_text(encoding="utf-8"), opener)
                # A pointer that moved out of its paragraph makes the rest vacuous.
                self.assertIn("mint-viewer-token", norm(para), f"{rel}: the mint pointer left {opener!r}")
                root = mint_forms(para)["root"]
                links = links_to(path, para, CONTAINER_DOC)
                self.assertTrue(
                    root or self.mint_anchor in links,
                    f"{rel}: gives neither the root image's `docker exec <container> witness_api "
                    f"mint-viewer-token --label` nor a link to docs/container.md#{self.mint_anchor}: {norm(para)}",
                )
                for placeholder in root:
                    self.assertEqual(placeholder, "<container>", f"{rel}: docs/container.md's placeholder")

    def test_each_pointer_names_the_settings_section(self) -> None:
        for rel, opener in MINT_POINTERS:
            with self.subTest(pointer=rel):
                para = norm(pointer_paragraph((REPO / rel).read_text(encoding="utf-8"), opener))
                self.assertIn(f"Settings → {self.header}", para, f"{rel}: where the receipt goes")

    def test_step1_tells_a_unit_user_to_skip_it(self) -> None:
        # The unit already passes the -e (the class above proves it), so
        # step 1 says a unit user skips it, names the -e it means, and names
        # the unit's container for the placeholder below.
        step1 = norm(container_step1(CONTAINER_DOC.read_text(encoding="utf-8")))
        self.assertTrue(links_to(CONTAINER_DOC, step1, UNIT), "step 1 links the stock examples/witnessd.service")
        said = re.search(r"already passes `-e ([A-Z][A-Z0-9_]*)=([^`\s]+)`, so a unit started from that "
                         r"file skips this step", step1)
        self.assertIsNotNone(said, "step 1 names the `-e NAME=VALUE` the stock unit passes, not a bare 'it' "
                                   "after a sentence naming two variables")
        opts, _, _ = docker_run(command_words(next(
            v for s, k, v in unit_entries(UNIT.read_text(encoding="utf-8")) if s == "Service" and k == "ExecStart")))
        self.assertEqual(unit_env_vars(opts).get(said.group(1)), said.group(2),
                         f"step 1 says the unit passes -e {said.group(1)}={said.group(2)}; "
                         f"it passes {unit_env_vars(opts)}")
        named = re.search(r"its container is `([^`]+)`", step1)
        self.assertIsNotNone(named, "step 1 names the unit's container for <container>")
        self.assertEqual(opts.get("--name"), [named.group(1)], "step 1's container name is the unit's --name")
        # "its loopback-only `-p` binding" holds while the unit publishes on
        # loopback (the unit class above holds its header to the widening).
        if "loopback-only" in step1:
            publishes = opts.get("-p", []) + opts.get("--publish", [])
            self.assertTrue(publishes and all(p.rsplit(":", 2)[0] in ("127.0.0.1", "[::1]", "localhost")
                                              for p in publishes),
                            f"step 1 calls the unit's binding loopback-only; it publishes {publishes}")

    def test_step1_repairs_a_unit_copied_before_the_line(self) -> None:
        # The install copies the unit once and nothing updates the copy, so
        # a unit installed before its -e joined it has no token path. Step 1
        # says where to look (`ExecStart`, for the variable the unit passes)
        # and how to load the fix: a daemon-reload, then a restart of the
        # name the install copied it to, which re-creates the container
        # (ExecStop's docker stop ends a --rm container; ExecStartPre
        # removes any leftover; the next docker run carries the new -e).
        doc = CONTAINER_DOC.read_text(encoding="utf-8")
        step1 = norm(container_step1(doc))
        install = doc_section(doc, "### Run under systemd")
        cp = re.search(r"^sudo cp examples/witnessd\.service (/etc/systemd/system/\S+\.service)$", install, re.M)
        self.assertIsNotNone(cp, "docs/container.md's install copies the unit into /etc/systemd/system")
        unit_name = Path(cp.group(1)).name
        self.assertRegex(step1, r"[Aa] copy installed before that line joined the file \(\d{4}-\d{2}-\d{2}\) lacks it",
                         "step 1 says an older copy of the unit lacks the -e")
        self.assertIn("if its `ExecStart` has no `WITNESS_API_TOKEN_PATH`", step1,
                      "step 1 says where to look in the copy")
        self.assertIn("`sudo systemctl daemon-reload`", step1, "a changed unit file loads on daemon-reload")
        self.assertIn(f"`sudo systemctl restart {unit_name}`", step1,
                      "the restart names the unit the install copied")
        # The restart re-creates the container only while the unit removes
        # and re-runs it; a unit that kept its container would keep its env.
        opts, _, _ = docker_run(command_words(next(
            v for s, k, v in unit_entries(UNIT.read_text(encoding="utf-8")) if s == "Service" and k == "ExecStart")))
        self.assertIn("--rm", opts, "the unit's container is --rm, so a stop removes it")
        self.assertIn("WITNESS_API_TOKEN_PATH", unit_env_vars(opts))

    def test_paragraph_and_link_readers(self) -> None:
        text = "x\n\n> **Step 0:** `docker exec <c> witness_api\n> mint-viewer-token --label <s>` [a](../docs/container.md#b)\n>\n> next\n"
        para = pointer_paragraph(text, "> **Step 0:**")
        self.assertEqual(para, "**Step 0:** `docker exec <c> witness_api\nmint-viewer-token --label <s>` [a](../docs/container.md#b)")
        self.assertEqual(mint_forms(para)["root"], ["<c>"])
        self.assertEqual(links_to(REPO / "tvos/RUN_ON_APPLE_TV.md", para, CONTAINER_DOC), ["b"])
        # The same href read from another directory lands elsewhere.
        self.assertEqual(links_to(REPO / "tvos/discovery/DISCOVERY.md", para, CONTAINER_DOC), [])
        self.assertEqual(heading_anchor(MINT_HEADING), "mint-a-witness-wall-viewer-token")


class KernelReferenceGivesEveryMintForm(unittest.TestCase):
    def setUp(self) -> None:
        self.section = doc_section(HA_SETUP.read_text(encoding="utf-8"), HA_VIEWER_HEADING)

    def test_section_gives_the_three_forms(self) -> None:
        forms = mint_forms(self.section)
        for kind in ("host", "root", "sidecar"):
            self.assertTrue(forms[kind], f"{HA_VIEWER_HEADING} gives no {kind} form: {norm(self.section)}")
        self.assertEqual(forms["root"], ["<container>"], "docs/container.md's placeholder")

    def test_section_links_the_root_images_mint_walk(self) -> None:
        self.assertIn(heading_anchor(MINT_HEADING), links_to(HA_SETUP, self.section, CONTAINER_DOC),
                      f"{HA_VIEWER_HEADING} links docs/container.md#{heading_anchor(MINT_HEADING)}")

    def test_section_reader_stops_at_the_next_heading(self) -> None:
        # The section is read to the next heading, so a form that moved out of it does not count.
        self.assertTrue(self.section.startswith(HA_VIEWER_HEADING))
        self.assertNotIn("\n### ", self.section[len(HA_VIEWER_HEADING):])
        self.assertIn("mint-viewer-token", self.section)


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

    def test_no_paste_into_unnamed_settings(self) -> None:
        vague = []
        for path in scanned_files():
            raw = path.read_text(encoding="utf-8", errors="replace")
            text = norm(re.sub(r"^[ \t]*> ?", "", raw, flags=re.MULTILINE))
            for m in VAGUE_SETTINGS_POINTER.finditer(text):
                vague.append(f"{path.relative_to(REPO)}: ...{m.group(0)}")
        self.assertEqual(vague, [], f"name the section a receipt goes to: Settings → {self.header}")

    def test_vague_pointer_reader(self) -> None:
        for s in ("pasted into the Wall's settings; a hub",
                  "paste that receipt into the Wall's settings once",
                  "paste the line into its Settings once",
                  "pastes it into the Wall's **Settings**, and"):
            with self.subTest(s=s):
                self.assertIsNotNone(VAGUE_SETTINGS_POINTER.search(s))
        for s in ("pasted into its Settings → Verification;",
                  "paste the one line it prints into the Wall's **Settings → Verification** once",
                  "Paste the line into the Wall's Settings -> Verification",
                  "the household picks the room in the Wall's Settings (Home",
                  "paste it. Then open the Wall's settings"):
            with self.subTest(s=s):
                self.assertIsNone(VAGUE_SETTINGS_POINTER.search(s))

    def test_pairing_guides_name_the_section(self) -> None:
        for rel in PAIRING_GUIDES:
            text = norm((REPO / rel).read_text(encoding="utf-8"))
            self.assertTrue(f"Settings → {self.header}" in text,
                            f"{rel} does not send the receipt to Settings → {self.header}")


if __name__ == "__main__":
    unittest.main()
