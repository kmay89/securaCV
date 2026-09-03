#!/usr/bin/env python3
"""Vendor board-facts snapshot — the anti-rot loop for the hardware we lean on.

We heavily use the Waveshare ESP32-S3-Touch-LCD-4.3 dash board, so its facts
(the pin map, the onboard silicon, the electrical + screen parameters) need to
be *ours to read* without going stale. This tool keeps a machine-readable
snapshot of those FACTS in `canary-local/devices/board_facts.json`, transcribed
from the vendor's own wiki — which stays the canonical source and is linked
everywhere the data appears. We store facts (GPIO numbers, part numbers, spec
values), never the vendor's prose or images; the human-facing reference
(`docs/hardware/waveshare_43_reference.md`) is our own words over this data.

Two jobs, mirroring the Home-Assistant freshness loop
(`gen_homeassistant.py` + `homeassistant-freshness.yml`):

  * default (no flags): VALIDATE the committed snapshot's shape — a CI drift
    guard so the JSON can't rot into something the reference/tests can't read.
  * `--refresh-upstream`: fetch each board's vendor page, parse the facts, and
    rewrite the snapshot. **Self-healing:** a board's values move forward ONLY
    on a clean fetch+parse; a dead feed, a 403, or a reshaped page keeps the
    previous snapshot verbatim for that board (→ "no diff, no PR"), never a
    broken file. `verified_utc` moves only when the FACTS move (a clean fetch
    that finds nothing new leaves the entry — date included — verbatim, so the
    weekly job never files a date-only PR); the reference
    page shows that date's age so a broken loop tells on itself.

  * `--from-file <html>` parses a local HTML file instead of the network — how
    the snapshot was seeded from the vendor page, and how the parser is tested
    (`canary-local/tests/board_facts.test.mjs` runs it over a committed
    fixture, so the parser can't silently rot either).

Usage:
    python3 canary-local/tools/gen_board_facts.py                 # validate
    python3 canary-local/tools/gen_board_facts.py --refresh-upstream
    python3 canary-local/tools/gen_board_facts.py --board waveshare-esp32s3-lcd43 \
        --from-file some.html            # seed/reparse one board from local HTML
    python3 canary-local/tools/gen_board_facts.py --emit-facts    # facts JSON to stdout (tests)
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import pathlib
import re
import sys
import urllib.error
import urllib.request
from html.parser import HTMLParser

from _tooling import repo_root

REPO_ROOT = repo_root()
SNAPSHOT = REPO_ROOT / "canary-local/devices/board_facts.json"
SCHEMA = "securacv-board-facts-1"

# The boards we track. Add the 4.3B / 4.3C here (their own wiki URLs) when we
# want their facts watched too — the loop then covers them with no new code.
TARGETS = {
    "waveshare-esp32s3-lcd43": {
        "vendor": "Waveshare",
        "product": "ESP32-S3-Touch-LCD-4.3",
        "url": "https://docs.waveshare.com/ESP32-S3-Touch-LCD-4.3",
    },
    "waveshare-esp32s3-lcd43b": {
        "vendor": "Waveshare",
        "product": "ESP32-S3-Touch-LCD-4.3B",
        "url": "https://docs.waveshare.com/ESP32-S3-Touch-LCD-4.3B",
    },
    "waveshare-esp32s3-lcd43c": {
        "vendor": "Waveshare",
        "product": "ESP32-S3-Touch-LCD-4.3C",
        "url": "https://docs.waveshare.com/ESP32-S3-Touch-LCD-4.3C",
    },
}

# Interface tables we lift as pin maps. Each is matched by a keyword in the
# vendor's <summary> label — robust to the two label styles the wiki uses
# ("LCD Interface: …" on the 4.3/4.3B, "… to LCD Pin Mapping Table" on the
# 4.3C). First match wins, so order specific keywords before generic ones.
PIN_SECTIONS = [
    ("lcd", r"\blcd\b"),
    ("touch", r"\btouch\b"),
    ("tf", r"\btf\b"),
    ("usb", r"\busb\b"),
    ("rs485", r"rs[\s\-]?485|\b485\b"),
    ("can", r"\bcan\b"),
    ("rtc", r"\brtc\b"),
    ("audio", r"\baudio\b|\bcodec\b"),
    ("isolation", r"\bisolat"),
    ("i2c", r"\bi2c\b"),
]


def _section_for(label: str) -> str | None:
    # Match only the label HEAD — the peripheral name before the colon
    # ("I2C Interface: …") or before "Pin Mapping Table" (the 4.3C style) —
    # so a description that happens to mention another peripheral ("connects
    # to the touch interface") can't hijack the classification.
    low = label.lower()
    head = low.split(":", 1)[0]
    if "pin mapping" in head:
        head = head.split("pin mapping", 1)[0]
    for key, pat in PIN_SECTIONS:
        if re.search(pat, head):
            return key
    return None


# ── HTML → structured facts (stdlib only) ───────────────────────────────────

class _Parser(HTMLParser):
    """Single pass: collect every <table> tagged with the H2 section it's
    under and the nearest preceding <summary> label, so parameter tables and
    the per-interface pin tables sort themselves out without brittle offset
    math."""

    def __init__(self) -> None:
        super().__init__()
        self.tables: list[tuple[str, str, list[list[str]]]] = []  # (h2, summary, rows)
        self._h2 = ""
        self._summary = ""
        self._grab: list[str] | None = None  # buffer for h2/summary text
        self._grab_kind: str | None = None
        self._rows: list[list[str]] | None = None
        self._row: list[str] | None = None
        self._cell: list[str] | None = None

    def handle_starttag(self, tag, attrs):
        if tag == "table":
            self._rows = []
        elif tag == "tr" and self._rows is not None:
            self._row = []
        elif tag in ("td", "th") and self._row is not None:
            self._cell = []
        elif tag in ("h2", "summary"):
            self._grab, self._grab_kind = [], tag

    def handle_endtag(self, tag):
        if tag == "table" and self._rows is not None:
            self.tables.append((self._h2, self._summary, self._rows))
            self._rows = None
        elif tag == "tr" and self._row is not None:
            self._rows.append([c.strip() for c in self._row])
            self._row = None
        elif tag in ("td", "th") and self._cell is not None:
            self._row.append(" ".join("".join(self._cell).split()))
            self._cell = None
        elif tag in ("h2", "summary") and self._grab is not None:
            text = " ".join("".join(self._grab).split())
            if tag == "h2":
                self._h2 = text
                self._summary = ""  # a new H2 clears the last interface label
            else:
                self._summary = text
            self._grab, self._grab_kind = None, None

    def handle_data(self, data):
        if self._cell is not None:
            self._cell.append(data)
        elif self._grab is not None:
            self._grab.append(data)


def _onboard(html: str) -> list[dict]:
    """Onboard Resources list → [{part, role}]. Handles both wiki styles:
    `<li><b>Part</b><br>role</li>` (4.3/4.3B) and
    `<li><strong>Part</strong>: role</li>` (4.3C)."""
    i = html.lower().find("onboard resources")
    if i < 0:
        return []
    j = html.lower().find("interface description", i)
    seg = html[i : j if j > 0 else len(html)]
    out = []
    for m in re.finditer(r"(?is)<li[^>]*>\s*<(?:b|strong)>(.*?)</(?:b|strong)>(.*?)</li>", seg):
        part = " ".join(re.sub(r"(?s)<[^>]+>", " ", m.group(1)).split())
        rest = re.sub(r"(?is)<br\s*/?>", " — ", m.group(2))
        role = " ".join(re.sub(r"(?s)<[^>]+>", " ", rest).split()).lstrip(":").strip(" —:")
        if part:
            out.append({"part": part, "role": role})
    return out


def _pin_rows(rows: list[list[str]]) -> list[dict]:
    """A 3-column interface table → [{pin, signal, desc}], header dropped."""
    out = []
    for r in rows:
        if not r or not r[0]:
            continue
        if r[0].lower().startswith("esp32-s3"):  # header row
            continue
        out.append({
            "pin": r[0],
            "signal": r[1] if len(r) > 1 else "",
            "desc": r[2] if len(r) > 2 else "",
        })
    return out


def parse_content(html: str) -> dict:
    """Vendor page HTML → the facts we keep. Raises ValueError if the page
    doesn't yield the load-bearing data (so a reshaped page self-heals rather
    than committing an empty snapshot)."""
    p = _Parser()
    p.feed(html)

    parameters: dict[str, str] = {}
    pins: dict[str, list[dict]] = {}
    for h2, summary, rows in p.tables:
        rows = [r for r in rows if r]
        if not rows:
            continue
        head0 = rows[0][0].lower() if rows[0] else ""
        section = _section_for(summary) if summary else None
        # A pin table is one under a matched interface label OR whose header
        # column is the MCU ("ESP32-S3"). First match wins per section.
        if section or head0.startswith("esp32-s3"):
            key = section or _section_for(h2) or "misc"
            entries = _pin_rows(rows)
            if entries and key not in pins:
                pins[key] = entries
            continue
        # Otherwise: clean 2-column key/value rows are product parameters.
        for r in rows:
            if len(r) == 2 and r[0] and r[1] and r[0].lower() != "esp32-s3":
                parameters.setdefault(r[0], r[1])

    onboard = _onboard(html)

    # The pin map is the load-bearing data every board page carries; require
    # it (and touch, the other universal). Parameters/onboard vary by page —
    # the 4.3C "AI voice" page has no Product Parameters section at all — so a
    # thin params/onboard is allowed, just noted, not a parse failure.
    if not pins.get("lcd") or not pins.get("touch"):
        raise ValueError(
            "parse produced no LCD/touch pin map (params=%d pins=%s onboard=%d) — "
            "the vendor page shape may have changed" % (len(parameters), list(pins), len(onboard))
        )

    facts = {"parameters": parameters, "onboard": onboard, "pins": pins}
    facts["content_hash"] = "sha256:" + hashlib.sha256(
        json.dumps(facts, sort_keys=True, ensure_ascii=False).encode("utf-8")
    ).hexdigest()
    return facts


# ── network (graceful) ──────────────────────────────────────────────────────

def fetch_upstream(url: str, timeout: int = 30) -> str | None:
    """Return the page HTML, or None on any failure (the caller self-heals).
    A real browser User-Agent — the docs CDN 403s default/bot agents."""
    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": (
                "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                "(KHTML, like Gecko) Chrome/125.0 Safari/537.36"
            ),
            "Accept": "text/html,application/xhtml+xml",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = resp.read()
        return data.decode("utf-8", "ignore")
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, OSError) as e:
        print(f"::warning::board_facts: could not fetch {url} ({e}); keeping the committed snapshot.")
        return None


# ── snapshot I/O + operations ───────────────────────────────────────────────

def load_snapshot() -> dict:
    if SNAPSHOT.exists():
        return json.loads(SNAPSHOT.read_text(encoding="utf-8"))
    return {"schema": SCHEMA, "boards": {}}


def write_snapshot(snap: dict) -> None:
    SNAPSHOT.write_text(json.dumps(snap, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def today_utc() -> str:
    # Passed in by callers that must be deterministic; here we stamp the real
    # date only on a refresh (never during validate), so validate stays pure.
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")


def validate(snap: dict) -> list[str]:
    errs: list[str] = []
    if snap.get("schema") != SCHEMA:
        errs.append(f"schema is {snap.get('schema')!r}, want {SCHEMA!r}")
    boards = snap.get("boards", {})
    if not boards:
        errs.append("no boards in the snapshot")
    for bid, b in boards.items():
        for k in ("vendor", "product", "source_url", "verified_utc", "facts"):
            if k not in b:
                errs.append(f"{bid}: missing '{k}'")
        facts = b.get("facts", {})
        if not facts.get("parameters"):
            errs.append(f"{bid}: no parameters")
        if not facts.get("pins", {}).get("lcd"):
            errs.append(f"{bid}: no LCD pin map")
        if not facts.get("onboard"):
            errs.append(f"{bid}: no onboard resources")
        if not str(facts.get("content_hash", "")).startswith("sha256:"):
            errs.append(f"{bid}: missing/!sha256 content_hash")
    return errs


def refresh_board(bid: str, snap: dict, html: str | None, stamp: str) -> bool:
    """Update one board in `snap` from `html`. Returns True if facts changed.
    A None html or a parse failure keeps the previous board entry verbatim."""
    meta = TARGETS[bid]
    prev = snap.setdefault("boards", {}).get(bid)
    if html is None:
        return False
    try:
        facts = parse_content(html)
    except ValueError as e:
        print(f"::warning::board_facts: {bid} parse failed ({e}); keeping the committed snapshot.")
        return False
    changed = (prev or {}).get("facts", {}).get("content_hash") != facts["content_hash"]
    if not changed:
        # A clean fetch that finds nothing new leaves the committed entry —
        # verified_utc included — VERBATIM. Otherwise a weekly no-op fetch
        # would bump the date, dirty the file, and the freshness job would
        # open a date-only PR every week. So verified_utc means "the date these
        # facts were last confirmed to have this value", not "last fetched".
        return False
    snap["boards"][bid] = {
        "vendor": meta["vendor"],
        "product": meta["product"],
        "source_url": meta["url"],
        "source_note": (
            "Facts transcribed from the vendor wiki (canonical). Waveshare owns "
            "the hardware and its documentation; this is our machine-readable "
            "snapshot of the pin map / silicon / parameters, refreshed by "
            "gen_board_facts.py so it can't go stale."
        ),
        "verified_utc": stamp,  # the date this fact set was recorded / last moved
        "facts": facts,
    }
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--refresh-upstream", action="store_true", help="fetch the vendor pages and rewrite the snapshot")
    ap.add_argument("--from-file", type=pathlib.Path, help="parse this local HTML instead of the network")
    ap.add_argument("--board", default=None, help="limit --from-file/--refresh to this board id")
    ap.add_argument("--date", default=None, help="stamp verified_utc with this date (deterministic tests)")
    ap.add_argument("--emit-facts", action="store_true", help="print parsed facts from --from-file as JSON and exit")
    args = ap.parse_args()

    # Parser-only mode for tests: parse a file, print facts, don't touch disk.
    if args.emit_facts:
        if not args.from_file:
            ap.error("--emit-facts requires --from-file")
        print(json.dumps(parse_content(args.from_file.read_text(encoding="utf-8")), indent=2, ensure_ascii=False))
        return 0

    snap = load_snapshot()

    if args.from_file:
        bid = args.board or next(iter(TARGETS))
        if bid not in TARGETS:
            ap.error(f"unknown board {bid!r}; known: {', '.join(TARGETS)}")
        stamp = args.date or today_utc()
        changed = refresh_board(bid, snap, args.from_file.read_text(encoding="utf-8"), stamp)
        snap["schema"] = SCHEMA
        write_snapshot(snap)
        print(f"board_facts: {bid} {'updated (facts changed)' if changed else 'refreshed (no change)'} from {args.from_file}")
        return 0

    if args.refresh_upstream:
        stamp = args.date or today_utc()
        any_changed = False
        ids = [args.board] if args.board else list(TARGETS)
        for bid in ids:
            html = fetch_upstream(TARGETS[bid]["url"])
            if refresh_board(bid, snap, html, stamp):
                any_changed = True
                print(f"board_facts: {bid} facts CHANGED — review the diff.")
        snap["schema"] = SCHEMA
        write_snapshot(snap)
        print("board_facts: refresh complete;", "changes to review." if any_changed else "no factual changes.")
        return 0

    # Default: validate the committed snapshot.
    errs = validate(snap)
    if errs:
        for e in errs:
            print("::error::board_facts: " + e)
        return 1
    n = len(snap.get("boards", {}))
    print(f"board_facts: snapshot OK — {n} board(s) tracked.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
