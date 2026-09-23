#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_display_portal.py — build canary-local/devices/display_portal.json from
the canary-display firmware's first-boot portal.

The display emulator compiles net/provision.cpp verbatim (canary-local/
emulator/build.sh), so the fleet page's phone sheet (assets/onboard-phone.js)
gets every HTTP byte from the firmware's own WebServer routes running in wasm.
It shows the captive page — PORTAL_HTML, as GET / serves it — in a sandboxed
<iframe srcdoc>, and a srcdoc document INHERITS the embedder's CSP. So the
page's one <style> block has to be pinned by hash in fleet.html's policy
(tools/gen_csp.py SRCDOC_STYLES), and the document framed must carry nothing
a hash cannot cover. This generator is where both of those are decided:

  * captive.html is PORTAL_HTML with its <script> blocks and its style=/on*=
    attributes removed — the exact transform onboard-phone.js stripPortal()
    applies to the served bytes at runtime (tests/onboard.test.js pins the two
    together). The portal's inline script is NOT run in the Lab: the phone
    sheet stands in for it, driving the same four routes. The one style=
    attribute (the manual-SSID field's display:none) sits inside a sheet that
    is closed until that script opens it, so dropping it changes nothing on
    screen.
  * captive.html_sha256 is the sha256 of PORTAL_HTML as SERVED (unstripped),
    so a probe can prove the committed wasm serves this source's page.
  * the routes, the captive redirect, the SoftAP facts, the zone presets the
    portal's script carries, and the join-failure labels the /status reason
    comes from — each parsed from source, and the generator refuses to write
    a document gen_csp.py would refuse to frame.

Chained like gen_wap.py -> gen_csp.py: run this, THEN gen_csp.py (it hashes
the <style> block out of the JSON this writes).

    python3 canary-local/tools/gen_display_portal.py           # write
    python3 canary-local/tools/gen_display_portal.py --check   # CI drift gate

Sources of truth (in-repo, deterministic, offline):
  firmware/projects/canary-display/src/net/provision.cpp   PORTAL_HTML, routes, AP
  firmware/common/network/wifi_join_policy.h               join_failure_label()
"""

import hashlib
import json
import re
import sys
from pathlib import Path

from _tooling import die, repo_root

REPO = repo_root()
PROVISION = REPO / "firmware/projects/canary-display/src/net/provision.cpp"
JOIN_POLICY = REPO / "firmware/common/network/wifi_join_policy.h"
OUT_JSON = REPO / "canary-local/devices/display_portal.json"

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_csp import scan_inline  # noqa: E402  (the census gen_csp.py applies to a srcdoc)

# The runtime transform, mirrored byte-for-byte by onboard-phone.js
# stripPortal(): script blocks out (to a fixed point — one pass is not a
# sanitizer), then style=/on*= attributes out of every start tag, also to a
# fixed point.
SCRIPT_BLOCK_RE = re.compile(r"<script\b[^>]*>[\s\S]*?</script\b[^>]*>", re.IGNORECASE)
TAG_RE = re.compile(r"<[a-zA-Z][^>]*>")
INLINE_ATTR_RE = re.compile(r"""\s(?:style|on[a-z]+)\s*=\s*(?:"[^"]*"|'[^']*'|[^\s>]+)""", re.IGNORECASE)


def strip_portal(html: str) -> str:
    while True:
        stripped = SCRIPT_BLOCK_RE.sub("", html)
        if stripped == html:
            break
        html = stripped
    return TAG_RE.sub(lambda m: _strip_attrs(m.group(0)), html)


def _strip_attrs(tag: str) -> str:
    # To a fixed point as well: removing one attribute can splice a new one
    # together (`<a  onx="1"onclick=2>` leaves `<a onclick=2>` after a pass).
    while True:
        stripped = INLINE_ATTR_RE.sub("", tag)
        if stripped == tag:
            return tag
        tag = stripped


_CACHE: dict = {}


def read(path: Path) -> str:
    if path not in _CACHE:
        if not path.exists():
            die(f"source missing: {path.relative_to(REPO)}")
        _CACHE[path] = path.read_text(encoding="utf-8")
    return _CACHE[path]


def grab(path: Path, pattern: str, label: str, flags=0) -> str:
    m = re.search(pattern, read(path), flags)
    if not m:
        die(f"{label}: pattern /{pattern}/ not found in {path.relative_to(REPO)} — firmware changed?")
    return m.group(1)


def must(path: Path, needle: str, label: str) -> None:
    if needle not in read(path):
        die(f"{label}: expected {needle!r} in {path.relative_to(REPO)} — firmware changed?")


def build() -> dict:
    # ── the captive page, as GET / serves it ────────────────────────────────
    portal = grab(PROVISION, r'PORTAL_HTML\[\]\s+PROGMEM\s*=\s*R"HTML\((.*?)\)HTML"',
                  "PORTAL_HTML", re.DOTALL)
    must(PROVISION, 'g->server.send_P(200, "text/html", PORTAL_HTML);', "GET / serves PORTAL_HTML")
    for needle in ("Connect your display", "Pick your home network.", "Time zone",
                   "<select id=\"tz\">", "fetch('/scan'", "fetch('/join'", "fetch('/status')"):
        if needle not in portal:
            die(f"PORTAL_HTML lost expected copy {needle!r}")
    served_sha = hashlib.sha256(portal.encode("utf-8")).hexdigest()

    scripts = SCRIPT_BLOCK_RE.findall(portal)
    if len(scripts) != 1:
        die(f"PORTAL_HTML carries {len(scripts)} <script> blocks; the Lab stands in for exactly one")
    attrs = sum(len(INLINE_ATTR_RE.findall(t)) for t in TAG_RE.findall(SCRIPT_BLOCK_RE.sub("", portal)))
    html = strip_portal(portal)

    # The document gen_csp.py will hash must be one it can frame: no script,
    # no inline attribute, exactly one <style> block.
    s, styles, bad = scan_inline("display_portal.json captive.html", html)
    if s or bad:
        die(f"stripped portal still carries {len(s)} script(s) / {len(bad)} inline attribute(s)")
    if len(styles) != 1:
        die(f"stripped portal carries {len(styles)} <style> blocks; expected exactly one")

    # The zone presets the portal's script builds its picker from (mirror_html.h
    # TZS's first two columns + the IANA names a phone reports). The phone sheet
    # reads them out of the SERVED script at runtime; this copy pins that read.
    tzs_src = re.search(r"var TZS=(\[\[[\s\S]*?\]\]);", scripts[0])
    if not tzs_src:
        die("PORTAL_HTML script lost its `var TZS=[...]` zone table")
    tz_presets = json.loads(tzs_src.group(1))
    if len(tz_presets) < 20 or any(len(r) != 3 or not all(isinstance(c, str) for c in r) for r in tz_presets):
        die("TZS parsed thin or malformed — the portal's zone table moved?")
    if not any(r[1] == "UTC0" for r in tz_presets):
        die("TZS lost its UTC row")

    # ── the routes and the captive redirect ─────────────────────────────────
    routes = [{"method": m, "path": p}
              for p, m in re.findall(r'ctx\.server\.on\("([^"]+)",\s*HTTP_([A-Z]+),', read(PROVISION))]
    if [r["path"] for r in routes][:1] != ["/"] or len(routes) < 4:
        die(f"portal routes parsed thin: {routes}")
    must(PROVISION, "ctx.server.onNotFound(handle_not_found);", "catch-all route")
    redirect = grab(PROVISION, r'sendHeader\("Location",\s*"([^"]+)",\s*true\)', "captive redirect")
    http_port = int(grab(PROVISION, r"WebServer server\{(\d+)\};", "WebServer port"))
    dns_port = int(grab(PROVISION, r"ctx\.dns\.begin\((\d+)\);", "DNS port"))

    # ── the SoftAP ──────────────────────────────────────────────────────────
    ssid_fmt = grab(PROVISION, r'snprintf\(ap_ssid, sizeof\(ap_ssid\), "(SecuraCV-%\.4s)"', "AP SSID format")
    pass_len = int(grab(PROVISION, r"constexpr size_t AP_PASS_LEN = (\d+);", "AP_PASS_LEN"))
    max_conn = int(grab(PROVISION, r"WiFi\.softAP\(ap_ssid, ap_pass, kApChannel, /\*hidden=\*/0, /\*max_conn=\*/(\d+)\)",
                        "softAP max_conn"))
    channel = int(grab(PROVISION, r"#else\s*constexpr int kApChannel = (\d+);", "default AP channel"))
    serial_line = grab(PROVISION, r'printf\("(First boot - onboarding AP \\"%s\\"  password %s)\\n"',
                       "first-boot serial line").replace('\\"', '"')

    # ── the /status failure vocabulary (the fleet-wide table) ───────────────
    body = grab(JOIN_POLICY, r"inline const char\* join_failure_label\(JoinFailure f\) \{([\s\S]*?)\n\}",
                "join_failure_label")
    labels = dict(re.findall(r'case JoinFailure::(\w+):\s*return "([^"]+)";', body))
    fallback = re.findall(r'\n\s*return "([^"]+)";', body)
    if not fallback:
        die("join_failure_label lost its fallback return")
    labels["Unknown"] = fallback[-1]
    for k in ("NotFound", "BadPassword", "NoAddress", "Unknown"):
        if k not in labels:
            die(f"join_failure_label lost {k}")

    return {
        "$note": "GENERATED by canary-local/tools/gen_display_portal.py from the canary-display "
                 "first-boot portal (src/net/provision.cpp). Do not edit by hand; run the generator, "
                 "then gen_csp.py. Drift-gated (--check) in .github/workflows/canary-local.yml.",
        "generated_by": "canary-local/tools/gen_display_portal.py",
        "source": "firmware/projects/canary-display/src/net/provision.cpp",
        "captive": {
            "html": html,
            "html_sha256": served_sha,
            "stripped": {"script_blocks": len(scripts), "inline_attributes": attrs},
            "ip": redirect.split("//", 1)[1].rstrip("/"),
            "http_port": http_port,
            "dns_port": dns_port,
            "routes": routes,
            "not_found_redirect": redirect,
        },
        "ap": {
            "ssid_format": ssid_fmt,
            "password_length": pass_len,
            "channel": channel,
            "max_stations": max_conn,
            "serial_line": serial_line,
        },
        "tz_presets": tz_presets,
        "join_failures": {k: labels[k] for k in ("NotFound", "BadPassword", "NoAddress", "Unknown")},
    }


def main(argv) -> int:
    text = json.dumps(build(), indent=2, ensure_ascii=True) + "\n"
    rel = OUT_JSON.relative_to(REPO)
    if "--check" in argv:
        current = OUT_JSON.read_text(encoding="utf-8") if OUT_JSON.exists() else ""
        if current != text:
            print(f"gen_display_portal.py: STALE {rel}", file=sys.stderr)
            print("::error::display portal data stale — the firmware's first-boot portal moved. Run "
                  "python3 canary-local/tools/gen_display_portal.py, THEN python3 canary-local/tools/gen_csp.py, "
                  "and commit both.", file=sys.stderr)
            return 1
        print(f"gen_display_portal.py: {rel} matches the firmware")
        return 0
    OUT_JSON.write_text(text, encoding="utf-8")
    print(f"wrote {rel}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
