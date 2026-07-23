# The BOM pipeline — parts data that runs itself

How the bills of materials stay **correct without anyone checking them**.
This is the engineering half of the self-running-company doctrine
([`docs/strategy/19-self-running-company.md`](../strategy/19-self-running-company.md));
the design borrows deliberately from the two systems that do this best:

- **Flux.ai's lesson** — a part is a live object, not a row in a file.
  Supply-chain state (stock, price, lifecycle) is *attached to* the part by
  the system, at design time, never typed by the designer.
- **Digi-Key's lesson** — the magic users feel is one canonical database
  with stable IDs feeding every surface (search, BOM tool, cart, APIs), so
  no two surfaces ever disagree. Correctness lives in ONE place;
  everything else is generated.

The rule that falls out of both: **a human never types a price, a stock
number, or a distributor SKU.** Humans state design intent; systems fetch
facts; a human appears only when an exception fires.

## The two halves

| Half | File(s) | Who writes it | Contents |
|------|---------|---------------|----------|
| **Design intent** | `docs/hardware/bom_*.csv` | a human (PR-reviewed) | RefDes, Qty, Required/Optional, Category, Description, Manufacturer, **MPN** — the decisions only an engineer can make |
| **Supply-chain facts** | `docs/hardware/pricing.json` | `scripts/bom_pricing.py` (nightly CI) | verified distributor SKUs, live stock, price breaks, lifecycle status, provenance, timestamps |

The CSVs keep their `Mouser`/`DigiKey`/`LCSC`/`UnitUSD` columns as
*indicative seed values* — they bootstrap the snapshot and serve readers of
the raw CSV — but the generated pages prefer the fetched data, row by row,
whenever provenance says a distributor verified it.

Parts are keyed by **manufacturer part number (MPN)**, never by distributor
SKU. SKUs are seller-local; the MPN is the stable ID that every
distributor API can resolve (this is also Octopart's own best-practice
guidance). Rows whose "MPN" is a commodity pseudo-number (`Generic`
manufacturer, `USB-C-DATA-1M`-style identifiers) are marked
`sourcing: "generic"` and simply keep their CSV price — nothing to fetch,
nothing to false-alarm about.

## Data flow

```
docs/hardware/bom_*.csv                 (intent: humans edit, lint-gated)
        │
        ├── scripts/lint_bom.py         (PR gate: schema, qty math, wiring)
        │
        ├── scripts/bom_pricing.py      (nightly: Digi-Key v4 + Mouser APIs)
        │        ▼
        │   docs/hardware/pricing.json  (facts: committed snapshot, MPN-keyed)
        │        │
        ▼        ▼
canary-local/tools/gen_enclosures.py    (merge: intent + facts)
        ▼
canary-local/devices/build.json         (drift-gated in canary-local.yml)
        ▼
Build-it page (build-it.js)             ("live · digikey · 4,213 in stock")
```

Every arrow is enforced, not hoped for:

- `lint_bom.py` (Repo Lints) fails a PR whose CSV breaks the schema, gets
  the Qty×Unit math wrong, or adds a BOM the generator doesn't know about.
- The canary-local drift gate fails CI if `build.json` wasn't regenerated
  after a CSV or snapshot change.
- `pricing.json` is a **committed input**, so the drift gate stays
  deterministic and the site works offline — the nightly job is the only
  writer.

## The nightly workflow (`bom-pricing.yml`)

Every night (09:13 UTC) the workflow:

1. Runs `scripts/bom_pricing.py` — resolves every orderable MPN against
   the **Digi-Key Product Information V4** API (client-credentials OAuth2,
   keyword search) and the **Mouser Search API** (part-number search).
   Digi-Key wins ties; Mouser fills gaps. ~60 MPNs at polite pacing —
   far inside both free tiers.
2. Regenerates the catalog JSONs so `build.json` rides the fresh snapshot.
3. Commits the result (`bom: nightly supply-chain snapshot`) — or does
   nothing if the market didn't move.
4. Files **exceptions as deduplicated GitHub issues** titled
   `supply-chain [kind]: MPN`.

### Exceptions — the only time a human appears

| Kind | Trigger | Typical response |
|------|---------|------------------|
| `out-of-stock` | live stock hits 0 | check the other distributor / LCSC column; pick an ALT part into the CSV |
| `price-jump` | unit price moved >15% vs the last snapshot | re-check kit margins (strategy doc 16/18); reprice or absorb |
| `lifecycle` | status no longer Active (NRND/EOL/obsolete) | qualify the CSV's ALT row, or find a substitute and PR the CSV |
| `no-match` | an MPN that used to resolve no longer does | renamed or delisted — fix the MPN or treat as EOL |

Silence means healthy. There is no "check the BOM" chore anywhere in the
company; the BOM checks itself and *summons* a human with a specific,
actionable signal. Closing the issue = decision made in the CSV (which is
itself lint- and drift-gated), and the next night verifies the fix.

## Credentials (one-time, ~10 minutes)

Both APIs are free. Add the secrets to the repo
(**Settings → Secrets and variables → Actions**) and the nightly job picks
them up; until then it no-ops politely and the seeded snapshot stands.

- **Digi-Key** — register at <https://developer.digikey.com>, create an
  organization + production app for the *Product Information V4* API, and
  set `DIGIKEY_CLIENT_ID` / `DIGIKEY_CLIENT_SECRET`.
- **Mouser** — request a Search API key at
  <https://www.mouser.com/api-search/> and set `MOUSER_API_KEY`.

LCSC has no public API; its column stays a static courtesy reference.

## Local runbook

```bash
python3 scripts/lint_bom.py            # schema gate (what CI runs)
python3 scripts/bom_pricing.py --seed  # rebuild snapshot offline from CSVs
DIGIKEY_CLIENT_ID=… DIGIKEY_CLIENT_SECRET=… MOUSER_API_KEY=… \
python3 scripts/bom_pricing.py         # real fetch (any subset of creds works)
python3 canary-local/tools/gen_enclosures.py   # re-merge into build.json
```

`--seed` is non-destructive: it re-reads the CSVs but preserves live
distributor fields for MPNs that still exist, so a CSV edit never erases
fetched history.

## pricing.json schema

```jsonc
{
  "generated_by": "scripts/bom_pricing.py",
  "as_of": "2026-07-23T09:13:00Z",
  "sources": { "csv": "…", "digikey": true, "mouser": true },
  "parts": {
    "<MPN>": {
      "mfr": "Seeed Studio",
      "desc": "…",
      "sourcing": "orderable | generic",
      "seed_usd": 24.90,          // the CSV's indicative price (bootstrap)
      "unit_usd": 24.90,          // best current unit price
      "provenance": "digikey | mouser | csv-seed",
      "stock": 4213,              // null when not distributor-verified
      "lifecycle": "Active",
      "sku": { "digikey": "…", "mouser": "…", "lcsc": "…" },
      "breaks": [ { "qty": 1, "usd": 24.9 }, { "qty": 10, "usd": 22.4 } ],
      "url": "https://…",
      "boms": [ "bom_canary_sense.csv" ]
    }
  }
}
```

## Roadmap (in adoption order)

1. **"Buy this build" button** — Digi-Key's *myLists third-party API*
   (`POST https://www.digikey.com/mylists/api/thirdparty`, no auth) turns a
   Build-it parts list into a single-use URL that lands the visitor in a
   prefilled Digi-Key list/cart. Each click needs a fresh POST; if browser
   CORS blocks it, a ~20-line Cloudflare Worker fronts it. Mouser's cart
   API is the second target.
2. **Margin guard** — compute kit COGS from `pricing.json` + the printed
   parts estimate and warn when a `store.json` price (website repo) falls
   under target margin. Pricing stays a human call; drift becomes visible
   automatically.
3. **Batch-buy quotes** — Digi-Key's quoting/price-locking and Ordering
   APIs can turn "build 25 Witness Pairs" into a placed order with price
   breaks and attrition computed. Deliberately *not* automated until batch
   volume justifies a machine spending money.
