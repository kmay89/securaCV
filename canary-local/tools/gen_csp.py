#!/usr/bin/env python3
"""canary-local/tools/gen_csp.py — one policy table, every Lab page's CSP.

Every page under canary-local/ (plus the emulator harness) carries a
Content-Security-Policy as a <meta http-equiv> — GitHub Pages cannot set
response headers, and the desktop Lab (desktop-lab/) serves the same files
from a Tauri webview. This tool is the ONE place that policy is written:

  * BASE is the strict floor every page starts from: default-src 'none',
    scripts and styles same-origin only, no plugins, no <base> rewrite, no
    form posts. There is no 'unsafe-inline' and no 'unsafe-eval' anywhere,
    and this tool refuses to emit them.
  * SHARED is what every page needs beyond the floor, each with its reason.
  * PAGES is the per-page table: a page gets a source only if it needs it,
    and the entry says why — 'wasm-unsafe-eval' for the pages that boot the
    firmware compiled to WebAssembly, frame-src for the two pages that frame
    another, the flasher's signed release hosts. A page that is not in the
    table gets BASE + SHARED and nothing else.
  * Directives the floor could carry but no page needs are TRIMMED, not
    granted: no blob: in img-src, no worker-src, no media-src, no scheme
    source (http: https: ws: wss:) in connect-src. Nothing in the Lab shows a
    blob: image, spawns a Worker, plays media from a URL, or talks to a Canary
    from the document (discovery rides the desktop Lab's native side, through
    invoke). The benches synthesize audio, the eyes bench's <video> takes a
    MediaStream and the download links hand a blob: to <a download> — none of
    which CSP governs. When a page grows one of these it becomes a PAGES row
    with a reason; the Worker check below refuses a page that grew one silently.
  * Inline <script> bodies are hashed ONLY for the pages named in
    INLINE_SCRIPT_OK (today: flash.html's import map, which browsers cannot
    load from a file). Any other inline script, any <style> block, any
    style="…" attribute and any on*= handler fails the run: move it into the
    page's assets/<page>.js / .css instead of loosening the policy.

The tool then rewrites exactly one CSP <meta> line per page, right after the
<meta charset> line, idempotently. Run it after editing a page or this table:

    python3 canary-local/tools/gen_csp.py            # rewrite the pages
    python3 canary-local/tools/gen_csp.py --check    # CI drift gate (exit 1)
    python3 canary-local/tools/gen_csp.py --explain  # the table, per page

Consistency checks the tool also enforces, so the table cannot rot:
  * a page that references the emulator's dist/ bundles MUST carry the WASM
    entry, and a page carrying it MUST reference them (a stale reason is a
    lie in the audit trail);
  * likewise frame-src and a page's iframes;
  * likewise the signed release hosts and the modules that fetch a release
    (flash.js / we2-flash.js) — a page whose module graph reaches them without
    the hosts would fail behind a click, where the browser probe never looks;
  * a page whose modules spawn a Worker must say so in a worker-src row;
  * every table key names a page that exists.

Out of scope, on purpose: canary-local/witness/witness.html is vendored
byte-for-byte from the website repo (scripts/check_witness_emulator_sync.sh)
and canary-local/voice/index.html is emitted by gen_voice_preview.mjs — both
are documents of their own inside an iframe / a generated tree, and neither is
edited here. tests/csp.test.js mirrors these rules in the page-logic job and
tests/csp_probe.mjs loads every page in Chromium and fails on any violation.
"""
import base64
import hashlib
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LAB = REPO / "canary-local"
HARNESS = "emulator/web/harness.html"

# ── the policy table ────────────────────────────────────────────────────────

# The floor. Directive order here is the order written into every page.
BASE = [
    ("default-src", ["'none'"]),
    ("script-src", ["'self'"]),
    ("style-src", ["'self'"]),
    ("img-src", ["'self'", "data:"]),
    ("font-src", ["'self'"]),
    ("connect-src", ["'self'"]),
    ("object-src", ["'none'"]),
    ("base-uri", ["'none'"]),
    ("form-action", ["'none'"]),
]

# Every page, beyond the floor. (directive, sources, reason)
SHARED = [
    (
        "connect-src",
        ["ipc:", "http://ipc.localhost"],
        "Tauri IPC: inside the desktop Lab (desktop-lab/) every page's lab-nav.js hands "
        "external links to the OS browser through the opener plugin, and lab.html / "
        "witness-wall.html invoke native commands; where the webview carries invoke() as a "
        "fetch it goes to an ipc: URL or to http://ipc.localhost (the two origins Tauri itself "
        "adds to its connect-src), so the page's own policy must name both. In a browser "
        "these are dead letters — no such scheme, and *.localhost is loopback.",
    ),
]

WASM = (
    "script-src",
    ["'wasm-unsafe-eval'"],
    "boots the real firmware compiled to WebAssembly (emulator/dist/*.js): "
    "WebAssembly.instantiate() needs this once a policy names script-src. It permits "
    "wasm compilation only — JavaScript eval() stays blocked.",
)
FRAMES_SELF = (
    "frame-src",
    ["'self'"],
    "frames another Lab page (same origin only).",
)

RELEASE = (
    "connect-src",
    ["https://github.com", "https://*.githubusercontent.com"],
    "signed firmware releases: manifest-flash.json, the factory images and the camera "
    "module's model come from the project's GitHub release host and its asset CDN "
    "(flash.json manifest_url) — fetched by flash.js and we2-flash.js.",
)
# The modules that fetch from the release hosts. A page whose <script src>
# graph reaches one of these needs RELEASE, or the fetch fails behind a click.
RELEASE_FETCHERS = {"flash.js", "we2-flash.js"}

PAGES = {
    "flash.html": [
        RELEASE,
        (
            "connect-src",
            ["http://localhost:*", "http://127.0.0.1:*", "https://localhost:*", "https://127.0.0.1:*"],
            "a same-machine manifest server: exactly the loopback set flash-core.js "
            "manifestOverrideUrl accepts, so the code guard and the policy agree. Broader "
            "LAN hosts cannot be spelled as a static allowlist — those self-host same-origin "
            "or flash a local file.",
        ),
    ],
    "fleet.html": [WASM],
    "eyes.html": [WASM],
    "vision.html": [WASM],
    "smoke.html": [WASM],
    "senselab.html": [WASM],
    HARNESS: [WASM],
    "lab.html": [FRAMES_SELF],
    "witness-wall.html": [FRAMES_SELF],
}

# Pages allowed to keep an inline <script>, hashed into script-src. The value
# is the reason the script cannot move to a file.
INLINE_SCRIPT_OK = {
    "flash.html": "the import map that carries the vendored modules' SRI hashes must be "
    "inline — browsers do not load <script type=importmap src=…>.",
}
# Pages allowed to keep an inline <style> block, hashed into style-src. Empty
# on purpose: every block has moved into a stylesheet. The mechanism stays so
# a future exception is a table entry with a reason, not an 'unsafe-inline'.
INLINE_STYLE_OK = {}

FORBIDDEN_SOURCES = {"'unsafe-inline'", "'unsafe-eval'", "'unsafe-hashes'", "*"}

# ── page scanning ───────────────────────────────────────────────────────────

COMMENT_RE = re.compile(r"<!--[\s\S]*?-->")
SCRIPT_RE = re.compile(r"<script\b([^>]*)>([\s\S]*?)</script>", re.IGNORECASE)
STYLE_RE = re.compile(r"<style\b([^>]*)>([\s\S]*?)</style>", re.IGNORECASE)
TAG_RE = re.compile(r"<[a-zA-Z][^>]*>")
INLINE_ATTR_RE = re.compile(r"""\s(style|on[a-z]+)\s*=""", re.IGNORECASE)
CHARSET_RE = re.compile(r"^(?P<indent>[ \t]*)<meta charset=[^>]*>[ \t]*\n", re.MULTILINE)
CSP_META_RE = re.compile(
    r"""^[ \t]*<meta\s+http-equiv=["']Content-Security-Policy["'][^>]*>[ \t]*\n""",
    re.IGNORECASE | re.MULTILINE,
)
JS_IMPORT_RES = [
    re.compile(r"""\bfrom\s*["']([^"']+)["']"""),
    re.compile(r"""\bimport\s*\(\s*["']([^"']+)["']"""),
    re.compile(r"""(?:^|[;{}\s])import\s+["']([^"']+)["']"""),
]


def fail(msg, code=2):
    print(f"gen_csp.py: {msg}", file=sys.stderr)
    sys.exit(code)


def sha256_source(text):
    digest = hashlib.sha256(text.encode("utf-8")).digest()
    return "'sha256-" + base64.b64encode(digest).decode("ascii") + "'"


def strip_js_comments(src):
    src = re.sub(r"/\*[\s\S]*?\*/", " ", src)
    return re.sub(r"(^|[^:\"'`\w])//.*$", r"\1", src, flags=re.MULTILINE)


def lab_pages():
    pages = sorted(p.name for p in LAB.glob("*.html"))
    pages.append(HARNESS)
    return pages


def module_graph(page):
    """Every first-party module a page's <script src> tags pull in, statically."""
    path = LAB / page
    html = path.read_text()
    queue = [(path.parent / s).resolve() for s in re.findall(r'<script\b[^>]*\ssrc="([^"]+)"', html)]
    seen = []
    while queue:
        f = queue.pop()
        if f in seen or not f.exists():
            continue
        seen.append(f)
        if "emulator/dist/" in f.as_posix():
            continue  # the compiled bundles: nothing to follow
        src = f.read_text(errors="replace")
        for pattern in JS_IMPORT_RES:
            for m in pattern.finditer(src):
                if m.group(1).startswith("."):
                    queue.append((f.parent / m.group(1)).resolve())
    return seen


# The compiled firmware bundles, by name — the harness reaches them as
# ../dist/…, the product pages as emulator/dist/…, so match the file, not the path.
BUNDLE_RE = re.compile(r"canary-(?:display-[a-z0-9]+|vision-core|wap-audio)\.js\b")


def references_wasm(page):
    html = COMMENT_RE.sub("", (LAB / page).read_text())
    if BUNDLE_RE.search(html):
        return True
    return any(
        BUNDLE_RE.search(strip_js_comments(f.read_text(errors="replace")))
        for f in module_graph(page)
        if "emulator/dist/" not in f.as_posix()
    )


def fetches_releases(page):
    return any(f.name in RELEASE_FETCHERS for f in module_graph(page))


WORKER_RE = re.compile(r"\bnew\s+(?:Shared)?Worker\s*\(")


def spawns_worker(page):
    return any(
        WORKER_RE.search(strip_js_comments(f.read_text(errors="replace")))
        for f in module_graph(page)
        if "emulator/dist/" not in f.as_posix()
    )


IFRAME_JS_RE = re.compile(r"""\bh\(\s*["']iframe["']|createElement\(\s*["']iframe["']|<iframe\b""")


def frames_pages(page):
    html = COMMENT_RE.sub("", (LAB / page).read_text())
    if "<iframe" in html:
        return True
    return any(
        IFRAME_JS_RE.search(strip_js_comments(f.read_text(errors="replace")))
        for f in module_graph(page)
        if "emulator/dist/" not in f.as_posix()
    )


def scan_inline(page, html):
    """Inline scripts, style blocks, and offending attributes in a page."""
    scripts = [m.group(2) for m in SCRIPT_RE.finditer(html) if not re.search(r"\ssrc\s*=", m.group(1))]
    styles = [m.group(2) for m in STYLE_RE.finditer(html)]
    # Attributes: look only at tags, outside comments and outside script/style bodies.
    markup = COMMENT_RE.sub("", html)
    markup = SCRIPT_RE.sub("", markup)
    markup = STYLE_RE.sub("", markup)
    bad_attrs = []
    for tag in TAG_RE.findall(markup):
        for attr in INLINE_ATTR_RE.findall(tag):
            bad_attrs.append((attr.lower(), tag[:80]))
    return scripts, styles, bad_attrs


# ── policy assembly ─────────────────────────────────────────────────────────

def policy_for(page, html):
    directives = {d: list(srcs) for d, srcs in BASE}
    order = [d for d, _ in BASE]

    def add(directive, sources):
        if directive not in directives:
            directives[directive] = []
            order.append(directive)
        for s in sources:
            if s in FORBIDDEN_SOURCES:
                fail(f"{page}: refusing to write {s} into {directive} — that is the whole point")
            if s not in directives[directive]:
                directives[directive].append(s)

    for directive, sources, _reason in SHARED:
        add(directive, sources)
    for directive, sources, _reason in PAGES.get(page, []):
        add(directive, sources)

    scripts, styles, bad_attrs = scan_inline(page, html)
    if bad_attrs:
        listing = ", ".join(f'{a} in {t!r}' for a, t in bad_attrs[:5])
        fail(
            f"{page}: inline {listing} — a CSP without 'unsafe-inline' blocks these. "
            "Move the styling into the page's stylesheet as a class and the handler into "
            "its assets/<page>.js (addEventListener)."
        )
    if scripts:
        if page not in INLINE_SCRIPT_OK:
            fail(
                f"{page}: {len(scripts)} inline <script> block(s). Move them into "
                "assets/<page>.js (or emulator/web/<page>.js) — script hashes are reserved "
                "for the scripts that cannot move (INLINE_SCRIPT_OK in this tool, with a reason)."
            )
        add("script-src", [sha256_source(body) for body in scripts])
    elif page in INLINE_SCRIPT_OK:
        fail(f"{page}: INLINE_SCRIPT_OK names it but it has no inline <script> — drop the entry")
    if styles:
        if page not in INLINE_STYLE_OK:
            fail(
                f"{page}: {len(styles)} inline <style> block(s). Move them into a stylesheet "
                "under assets/ (or emulator/web/) and <link> it — INLINE_STYLE_OK exists for a "
                "block that genuinely cannot move, with a reason."
            )
        add("style-src", [sha256_source(body) for body in styles])
    elif page in INLINE_STYLE_OK:
        fail(f"{page}: INLINE_STYLE_OK names it but it has no inline <style> — drop the entry")

    # The table must describe the page it names — both directions.
    has_wasm = "'wasm-unsafe-eval'" in directives["script-src"]
    if references_wasm(page) and not has_wasm:
        fail(f"{page}: references emulator/dist/ (WebAssembly) but PAGES has no WASM entry for it")
    if has_wasm and not references_wasm(page):
        fail(f"{page}: PAGES grants 'wasm-unsafe-eval' but nothing on the page loads emulator/dist/")
    has_frames = "frame-src" in directives
    if frames_pages(page) and not has_frames:
        fail(f"{page}: frames another page but PAGES has no frame-src entry for it")
    if has_frames and not frames_pages(page):
        fail(f"{page}: PAGES grants frame-src but the page frames nothing")
    has_release = all(s in directives["connect-src"] for s in RELEASE[1])
    if fetches_releases(page) and not has_release:
        fail(
            f"{page}: its modules reach {sorted(RELEASE_FETCHERS)} (they fetch the signed "
            "release) but PAGES has no RELEASE row for it — the fetch would fail behind a click"
        )
    if has_release and not fetches_releases(page):
        fail(f"{page}: PAGES grants the release hosts but nothing on the page fetches a release")
    if spawns_worker(page) and "worker-src" not in directives:
        fail(f"{page}: its modules spawn a Worker but PAGES has no worker-src row for it")

    return "; ".join(f"{d} {' '.join(directives[d])}" for d in order)


def render(page, html):
    """The page with exactly one CSP meta, right after <meta charset>."""
    policy = policy_for(page, html)
    if '"' in policy:
        fail(f"{page}: policy contains a double quote")
    stripped = CSP_META_RE.sub("", html)
    m = CHARSET_RE.search(stripped)
    if not m:
        fail(f"{page}: no <meta charset> line to anchor the policy after")
    close = " />" if m.group(0).rstrip().endswith("/>") else ">"
    line = f'{m.group("indent")}<meta http-equiv="Content-Security-Policy" content="{policy}"{close}\n'
    return stripped[: m.end()] + line + stripped[m.end():]


def explain():
    for page in lab_pages():
        html = (LAB / page).read_text()
        print(f"\n{page}")
        print(f"  {policy_for(page, html)}")
        for directive, sources, reason in SHARED + PAGES.get(page, []):
            print(f"  + {directive} {' '.join(sources)}")
            print(f"      {reason}")
        if page in INLINE_SCRIPT_OK:
            print(f"  + script-src 'sha256-…' (inline script)\n      {INLINE_SCRIPT_OK[page]}")
        if page in INLINE_STYLE_OK:
            print(f"  + style-src 'sha256-…' (inline style)\n      {INLINE_STYLE_OK[page]}")


def main(argv):
    check = "--check" in argv
    if "--explain" in argv:
        explain()
        return 0
    pages = lab_pages()
    for key in list(PAGES) + list(INLINE_SCRIPT_OK) + list(INLINE_STYLE_OK):
        if key not in pages:
            fail(f"the policy table names {key}, which is not a Lab page")
    stale = []
    for page in pages:
        path = LAB / page
        before = path.read_text()
        after = render(page, before)
        if after != before:
            stale.append(page)
            if not check:
                path.write_text(after)
    if check:
        if stale:
            for page in stale:
                print(f"gen_csp.py: STALE canary-local/{page}", file=sys.stderr)
            print(
                "::error::Lab CSP stale — a page or the policy table moved. Run "
                "python3 canary-local/tools/gen_csp.py and commit.",
                file=sys.stderr,
            )
            return 1
        print(f"gen_csp.py: {len(pages)} pages carry the generated policy")
        return 0
    print(f"gen_csp.py: wrote {len(stale)} of {len(pages)} pages" + (f" ({', '.join(stale)})" if stale else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
