#!/usr/bin/env python3
"""scripts/bom_pricing.py — the supply chain, fetched — never hand-typed.

The BOM CSVs (docs/hardware/bom_*.csv) state design intent: RefDes, qty,
required-vs-optional, manufacturer + MPN. Everything a distributor knows
better than we do — verified SKUs, live stock, price breaks, lifecycle —
is FETCHED into docs/hardware/pricing.json by this script and never edited
by hand. The one-person rule: humans state intent, systems fetch facts,
and a human only shows up when an exception fires.

Modes:
  --seed          build/refresh the snapshot offline from the CSVs alone
                  (provenance "csv-seed"; no network, no keys needed).
                  Non-destructive: live fields already present in
                  pricing.json are preserved for known MPNs.
  (default)       fetch live data with whichever credentials exist:
                    DIGIKEY_CLIENT_ID / DIGIKEY_CLIENT_SECRET
                      → Product Information V4 keyword search
                        (client-credentials OAuth2)
                    MOUSER_API_KEY
                      → Mouser Search API (part-number search)
                  With no credentials at all it exits 0 without touching
                  the snapshot, so the nightly workflow simply no-ops
                  until the keys are added (documented in
                  docs/hardware/bom_pipeline.md).
  --exceptions-out FILE
                  write supply-chain exceptions as JSON — out-of-stock,
                  price jump >15%, lifecycle no longer Active, expected
                  MPN no longer matching — for the nightly workflow to
                  turn into deduplicated GitHub issues. Exceptions are
                  signals, not failures: the script still exits 0.

"Generic" rows (mfr Generic/SecuraCV, or pseudo-MPNs with spaces) are
commodity parts with no canonical distributor listing; they keep their
CSV price with sourcing "generic" and are never fetched or flagged.

Run:  python3 scripts/bom_pricing.py --seed
CI:   .github/workflows/bom-pricing.yml (nightly cron)
"""
import csv
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
HW = REPO / "docs/hardware"
OUT = HW / "pricing.json"

PRICE_JUMP = 0.15  # |Δ|/old beyond this is an exception
GENERIC_MFRS = ("generic", "securacv")

DK_TOKEN_URL = "https://api.digikey.com/v1/oauth2/token"
DK_SEARCH_URL = "https://api.digikey.com/products/v4/search/keyword"
MOUSER_URL = "https://api.mouser.com/api/v1/search/partnumber"


def now_utc() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def is_generic(mfr: str, mpn: str) -> bool:
    low = mfr.strip().lower()
    if any(low.startswith(g) for g in GENERIC_MFRS):
        return True
    # Pseudo-MPNs ("M5X25+NUT", "LIFEPO4-PCM+CTRL or …", "XIAO-… / …")
    # aren't canonical manufacturer part numbers — nothing to fetch.
    return " " in mpn.strip()


def read_boms():
    """All bom_*.csv rows → one part record per unique MPN."""
    parts = {}
    for path in sorted(HW.glob("bom_*.csv")):
        with open(path, newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                mpn = (r.get("MPN") or "").strip()
                if not r.get("RefDes") or not mpn:
                    continue
                try:
                    seed = float(r.get("UnitUSD") or 0)
                except ValueError:
                    seed = 0.0
                p = parts.setdefault(mpn, {
                    "mfr": (r.get("Manufacturer") or "").strip(),
                    "desc": (r.get("Description") or "").strip(),
                    "sourcing": ("generic" if is_generic(
                        r.get("Manufacturer") or "", mpn) else "orderable"),
                    "seed_usd": seed,
                    "sku": {},
                    "boms": [],
                })
                for col, key in (("Mouser", "mouser"), ("DigiKey", "digikey"),
                                 ("LCSC", "lcsc")):
                    v = (r.get(col) or "").strip()
                    if v and key not in p["sku"]:
                        p["sku"][key] = v
                if path.name not in p["boms"]:
                    p["boms"].append(path.name)
    return parts


def http_json(url, data=None, headers=None, form=False, timeout=20):
    body = None
    headers = dict(headers or {})
    if data is not None:
        if form:
            body = urllib.parse.urlencode(data).encode()
            headers.setdefault("Content-Type",
                               "application/x-www-form-urlencoded")
        else:
            body = json.dumps(data).encode()
            headers.setdefault("Content-Type", "application/json")
    req = urllib.request.Request(url, data=body, headers=headers)
    for attempt in (1, 2):
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return json.loads(resp.read().decode())
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as e:
            if attempt == 2:
                print(f"  ! {url.split('/')[2]}: {e}", file=sys.stderr)
                return None
            time.sleep(2)


# ── Digi-Key (Product Information V4) ────────────────────────────────────
def digikey_token(client_id, client_secret):
    tok = http_json(DK_TOKEN_URL, form=True, data={
        "client_id": client_id,
        "client_secret": client_secret,
        "grant_type": "client_credentials",
    })
    return (tok or {}).get("access_token")


def digikey_lookup(token, client_id, mpn):
    res = http_json(DK_SEARCH_URL, data={"Keywords": mpn, "Limit": 5}, headers={
        "Authorization": f"Bearer {token}",
        "X-DIGIKEY-Client-Id": client_id,
        "X-DIGIKEY-Locale-Site": "US",
        "X-DIGIKEY-Locale-Language": "en",
        "X-DIGIKEY-Locale-Currency": "USD",
    })
    for prod in (res or {}).get("Products") or []:
        if (prod.get("ManufacturerProductNumber") or "").lower() != mpn.lower():
            continue
        variations = prod.get("ProductVariations") or []
        # Prefer the lowest-MOQ variation (cut tape over full reel).
        variations.sort(key=lambda v: v.get("MinimumOrderQuantity") or 1)
        var = variations[0] if variations else {}
        breaks = [{"qty": b.get("BreakQuantity"), "usd": b.get("UnitPrice")}
                  for b in var.get("StandardPricing") or []
                  if b.get("BreakQuantity") and b.get("UnitPrice") is not None]
        unit = breaks[0]["usd"] if breaks else prod.get("UnitPrice")
        return {
            "provenance": "digikey",
            "unit_usd": unit,
            "stock": prod.get("QuantityAvailable"),
            "lifecycle": ((prod.get("ProductStatus") or {}).get("Status")
                          or None),
            "sku_digikey": var.get("DigiKeyProductNumber"),
            "breaks": breaks,
            "url": prod.get("ProductUrl"),
        }
    return None


# ── Mouser (Search API) ──────────────────────────────────────────────────
def mouser_lookup(api_key, mpn):
    url = f"{MOUSER_URL}?apiKey={urllib.parse.quote(api_key)}"
    res = http_json(url, data={"SearchByPartRequest": {
        "mouserPartNumber": mpn, "partSearchOptions": "",
    }})
    for part in ((res or {}).get("SearchResults") or {}).get("Parts") or []:
        if (part.get("ManufacturerPartNumber") or "").lower() != mpn.lower():
            continue
        breaks = []
        for b in part.get("PriceBreaks") or []:
            m = re.search(r"[\d.]+", b.get("Price") or "")
            if m and b.get("Quantity"):
                breaks.append({"qty": b["Quantity"], "usd": float(m.group())})
        stock_m = re.match(r"\s*([\d,]+)", part.get("Availability") or "")
        return {
            "provenance": "mouser",
            "unit_usd": breaks[0]["usd"] if breaks else None,
            "stock": (int(stock_m.group(1).replace(",", ""))
                      if stock_m else None),
            "lifecycle": part.get("LifecycleStatus") or None,
            "sku_mouser": part.get("MouserPartNumber"),
            "breaks": breaks,
            "url": part.get("ProductDetailUrl"),
        }
    return None


# ── snapshot assembly ────────────────────────────────────────────────────
def load_existing():
    if OUT.exists():
        return json.loads(OUT.read_text(encoding="utf-8"))
    return {}


def assemble(parts, old, fetched, note):
    """CSV intent + previous snapshot + fresh fetches → the new snapshot."""
    old_parts = old.get("parts") or {}
    out_parts = {}
    for mpn in sorted(parts):
        p = parts[mpn]
        prev = old_parts.get(mpn) or {}
        hit = fetched.get(mpn)
        entry = {
            "mfr": p["mfr"],
            "desc": p["desc"],
            "sourcing": p["sourcing"],
            "seed_usd": p["seed_usd"],
            "unit_usd": p["seed_usd"],
            "provenance": "csv-seed",
            "stock": None,
            "lifecycle": None,
            "sku": dict(p["sku"]),
            "breaks": [],
            "url": None,
            "boms": p["boms"],
        }
        carry = hit or (prev if prev.get("provenance") in ("digikey", "mouser")
                        else None)
        if carry:
            entry.update({
                "unit_usd": (carry["unit_usd"] if carry.get("unit_usd")
                             is not None else p["seed_usd"]),
                "provenance": carry["provenance"],
                "stock": carry.get("stock"),
                "lifecycle": carry.get("lifecycle"),
                "breaks": carry.get("breaks") or [],
                "url": carry.get("url"),
            })
            for k in ("sku_digikey", "sku_mouser"):
                if carry.get(k):
                    entry["sku"][k.split("_")[1]] = carry[k]
            # carried-forward previous snapshots keep their verified SKUs
            for k, v in (prev.get("sku") or {}).items():
                entry["sku"].setdefault(k, v)
        out_parts[mpn] = entry
    return {
        "generated_by": "scripts/bom_pricing.py",
        "as_of": now_utc(),
        "note": note,
        "sources": {
            "csv": "docs/hardware/bom_*.csv (design intent + indicative seed prices)",
            "digikey": bool(os.environ.get("DIGIKEY_CLIENT_ID")),
            "mouser": bool(os.environ.get("MOUSER_API_KEY")),
        },
        "parts": out_parts,
    }


def find_exceptions(old, new):
    """What changed that a human should decide about."""
    exceptions = []
    old_parts = old.get("parts") or {}
    for mpn, e in (new.get("parts") or {}).items():
        if e["sourcing"] != "orderable":
            continue
        prev = old_parts.get(mpn) or {}
        live = e["provenance"] in ("digikey", "mouser")
        expected = bool(e["sku"].get("digikey") or e["sku"].get("mouser"))
        if not live and expected and prev.get("provenance") in ("digikey",
                                                                "mouser"):
            exceptions.append({
                "kind": "no-match", "mpn": mpn,
                "detail": f"{mpn} ({e['desc']}) previously matched at a "
                          f"distributor but no longer does — renamed, "
                          f"delisted, or API trouble."})
        if live and e.get("stock") == 0:
            exceptions.append({
                "kind": "out-of-stock", "mpn": mpn,
                "detail": f"{mpn} ({e['desc']}) shows 0 stock at "
                          f"{e['provenance']}."})
        lc = (e.get("lifecycle") or "").lower()
        if live and lc and lc not in ("active", "new product", "new"):
            exceptions.append({
                "kind": "lifecycle", "mpn": mpn,
                "detail": f"{mpn} ({e['desc']}) lifecycle is "
                          f"'{e['lifecycle']}' at {e['provenance']}."})
        if (live and prev.get("provenance") in ("digikey", "mouser")
                and prev.get("unit_usd") and e.get("unit_usd")):
            delta = abs(e["unit_usd"] - prev["unit_usd"]) / prev["unit_usd"]
            if delta > PRICE_JUMP:
                exceptions.append({
                    "kind": "price-jump", "mpn": mpn,
                    "detail": f"{mpn} ({e['desc']}) moved "
                              f"${prev['unit_usd']:.2f} → "
                              f"${e['unit_usd']:.2f} "
                              f"({delta * 100:.0f}%)."})
    return exceptions


def main() -> int:
    seed_only = "--seed" in sys.argv
    exc_out = None
    if "--exceptions-out" in sys.argv:
        exc_out = Path(sys.argv[sys.argv.index("--exceptions-out") + 1])

    parts = read_boms()
    old = load_existing()

    dk_id = os.environ.get("DIGIKEY_CLIENT_ID")
    dk_secret = os.environ.get("DIGIKEY_CLIENT_SECRET")
    mouser_key = os.environ.get("MOUSER_API_KEY")

    fetched = {}
    if not seed_only:
        if not (dk_id and dk_secret) and not mouser_key:
            print("bom_pricing.py: no distributor credentials in the "
                  "environment — snapshot left untouched (add "
                  "DIGIKEY_CLIENT_ID/SECRET and/or MOUSER_API_KEY; see "
                  "docs/hardware/bom_pipeline.md).")
            return 0
        token = digikey_token(dk_id, dk_secret) if dk_id and dk_secret else None
        orderable = [m for m, p in parts.items()
                     if p["sourcing"] == "orderable"]
        print(f"bom_pricing.py: fetching {len(orderable)} orderable MPNs "
              f"(digikey={'yes' if token else 'no'}, "
              f"mouser={'yes' if mouser_key else 'no'})")
        for mpn in orderable:
            hit = digikey_lookup(token, dk_id, mpn) if token else None
            if not hit and mouser_key:
                hit = mouser_lookup(mouser_key, mpn)
            if hit:
                fetched[mpn] = hit
            time.sleep(0.35)  # polite pacing — both APIs allow far more

    note = ("Offline seed from the BOM CSVs — prices are the CSVs' "
            "indicative values until distributor credentials are configured."
            if seed_only or not fetched else
            "Nightly distributor snapshot; provenance marks which rows are "
            "distributor-verified vs csv-seed.")
    new = assemble(parts, old, fetched, note)

    exceptions = find_exceptions(old, new) if not seed_only else []
    if exc_out:
        exc_out.write_text(json.dumps(exceptions, indent=1) + "\n")
    for e in exceptions:
        print(f"  EXCEPTION [{e['kind']}] {e['detail']}")

    OUT.write_text(json.dumps(new, indent=1, ensure_ascii=False) + "\n")
    live = sum(1 for p in new["parts"].values()
               if p["provenance"] in ("digikey", "mouser"))
    print(f"OK: {len(new['parts'])} parts ({live} distributor-verified, "
          f"{len(exceptions)} exception(s)) → {OUT.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
