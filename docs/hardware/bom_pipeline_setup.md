# Hook in the BOM pipeline — the complete setup guide

This is the whole ceremony for turning the parts pipeline from
"seeded" to "distributor-verified": two free API keys, three repo
secrets, one button press, about fifteen minutes. It is written for
*anyone* — the maintainer, a future maintainer, or someone who forked
this repo and wants the same self-running parts data for their own
project. No step assumes you were here when it was built.

**Radical honesty, up front:** this guide tells you what is verified,
what is designed-but-untested until your first credentialed run, and
exactly what happens in every failure case. Nothing here pretends.
(How the pipeline works is [`bom_pipeline.md`](./bom_pipeline.md);
why it exists is
[`../strategy/20-self-running-company.md`](../strategy/20-self-running-company.md).)

## What you get, before and after

**Already working today, with zero keys and zero setup:**

- The BOM CSVs are schema-linted on every PR (`scripts/lint_bom.py`).
- `docs/hardware/pricing.json` holds a seeded snapshot (67 parts, 32
  with real manufacturer part numbers a distributor can resolve).
- The Build-it pages show the CSVs' indicative prices, labeled as such.
- The **Order the parts** panel copies a distributor-ready `MPN,qty`
  list and opens Digi-Key myLists / Mouser's BOM tool — paste, and the
  cart prices itself. This works *now*; it needs no keys, because the
  visitor's own browser session does the pricing.

**After you add the keys:**

- Every night at 09:13 UTC, the 32 orderable parts get verified SKUs,
  live stock, price breaks, and lifecycle status from Digi-Key and
  Mouser. Build-it rows gain `live · digikey · 4,213 in stock` chips
  and a snapshot date, and each verified row's MPN becomes a link to
  that distributor's own product page.
- The website's [economics page](https://securacv.com/economics) grows
  a **"The parts behind each kit"** list — every required part with its
  price, its live stock, and the same distributor link — and its
  provenance line counts what is verified ("2 of 17 required parts
  distributor-verified"). Before the keys, that line honestly reads
  "17 required parts at indicative prices — no distributor match yet."
- When something needs a *decision* — a part out of stock, a price
  moving >15% **and** ≥$0.50 (both, so a 2¢ resistor doubling never
  pages anyone), a part leaving Active lifecycle, an MPN that stops
  matching — a deduplicated GitHub issue opens. Silence means healthy.
- The website repo's nightly Margin Guard (09:47 UTC) judges every
  store price against these live parts costs. It already runs today on
  seed prices; keys just make its inputs distributor-verified.

## Step 1 — Digi-Key key (~10 minutes, free)

1. Register at <https://developer.digikey.com> (any email; free).
2. Create an **organization**, then a **production app** in it.
3. Give the app the **Product Information V4** API. The pipeline uses
   two-legged OAuth2 (*client credentials*) — no user login flow, no
   redirect URL that matters (put anything valid in that field).
4. Copy the app's **Client ID** and **Client Secret**.

Honesty notes: Digi-Key's portal UI reshuffles occasionally, so the
click-path above may drift — the invariants are "production app,
Product Information V4, client-credentials." New apps are sometimes
held for approval for up to a day or two; until approved, token
requests fail and the pipeline just no-ops (see failure table below).

## Step 2 — Mouser key (~3 minutes, free, optional but recommended)

1. Request a **Search API** key at <https://www.mouser.com/api-search/>.
2. It arrives by email.

Mouser is the fallback when Digi-Key has no match (and the primary if
you only configure Mouser). Either key alone works; both is best.

## Step 3 — Add the secrets (2 minutes)

Repo **Settings → Secrets and variables → Actions → New repository
secret**, three times:

| Secret name | Value |
|---|---|
| `DIGIKEY_CLIENT_ID` | from step 1 |
| `DIGIKEY_CLIENT_SECRET` | from step 1 |
| `MOUSER_API_KEY` | from step 2 |

Any subset works — the script uses whatever exists. Secrets never
appear in logs: the fetcher's error paths were specifically built (and
CodeQL-checked) to log only a label and an exception class, never
URL-derived text.

## Step 4 — First run (1 minute + watching a log)

Don't wait for tonight: **Actions → BOM Pricing → Run workflow**.

A healthy log reads like:

```
bom_pricing.py: fetching 32 orderable MPNs (digikey=yes, mouser=yes)
OK: 67 parts (NN distributor-verified, 0 exception(s)) → docs/hardware/pricing.json
OK: … → canary-local/devices/enclosures.json
…
[commit] bom: nightly supply-chain snapshot
```

Then check three things:

1. `docs/hardware/pricing.json` — orderable parts now carry
   `"provenance": "digikey"` (or `"mouser"`), real stock numbers, and
   price-break tables.
2. The Build-it page (canary-local) — rows show live chips and the
   snapshot date under the totals.
3. The Actions run is green and a bot commit landed on `main`.

**What NN should be:** somewhere in the 20s, not 32. That is honest,
not broken — some of our parts (Seeed kits especially) may not match
at either US distributor by MPN, and they simply stay at their seed
price with `csv-seed` provenance. No alarm fires for a part that
*never* matched; `no-match` is only for parts that matched and then
vanished.

## The truth table — every way this can fail, and what actually happens

| Situation | What happens | What you do |
|---|---|---|
| No secrets configured | Fetch no-ops with a polite log line; seeded snapshot stands; site fine | Nothing (or add keys) |
| Digi-Key app not yet approved | Token fails → if Mouser key exists, Mouser carries the night; if not, run exits 1 (visible red), snapshot untouched | Wait for approval; re-run |
| Both APIs down / auth broken | Zero matches + live parts in snapshot = treated as outage: run exits 1, **snapshot untouched** — an outage can never demote the catalog | Usually nothing; it self-heals next night |
| One MPN stops matching | Entry demoted to `carried` (last-known numbers kept, no longer shown live), one `no-match` issue opens | Fix the MPN in the CSV, or treat as EOL |
| Part out of stock / price jump / EOL | One deduplicated issue with the numbers and your options | Decide; the next night verifies |
| You ignore every issue forever | Site keeps working on last-known-good data, honestly labeled | It's your company |
| You want it OFF | Actions → BOM Pricing → disable workflow (or delete the secrets) | Everything reverts to labeled seed prices |

## What is verified vs. what is designed (radical honesty)

- **Verified by CI on every PR:** the CSV schema, the snapshot schema,
  and 17 contract tests on the engine's behavior — miss→demote→one
  alarm, seed never destroys fetched history, recovery goes quiet,
  every exception kind, the price-jump noise floor, and the product-URL
  safety rule below. Plus the overlay's own tests
  (`canary-local/tools/tests/test_bom_overlay.py`,
  `canary-local/tests/build_it.test.js`) and the website's
  (`tests/economics.test.mjs`).
- **The product URL is the one fetched field that becomes a clickable
  link, so it is checked three times** and a failed check always means
  *no link*, never a broken or hostile one: on fetch
  (`safe_product_url` — https only, and only at the distributor we
  actually queried), on generate (again, because `pricing.json` is a
  committed file a human can edit), and at render in both frontends
  (because `build.json` arrives over the network). A part demoted to
  `carried` loses its link with its live status, so a listing that
  vanished can't linger as a dead link.
- **Designed against the documented APIs but not yet exercised with
  real credentials:** the exact Digi-Key/Mouser response-field parsing.
  This repo's build environment has no distributor keys, so the first
  credentialed run — yours, step 4 — is the first live contact. If a
  field assumption is wrong, the failure mode is the safe row of the
  table above: no match → seed prices stand → red run or lower NN, and
  the snapshot is never corrupted. Fixing it is editing one lookup
  function with the real response in hand.
- **Known gaps, on purpose:** LCSC has no public API — its column is a
  static courtesy. The one-click myLists deep-link stays unshipped
  until its request format is verified (we don't ship guessed API
  shapes). Distributor *ordering* stays human — a machine here never
  spends money.

## Cost and upkeep, stated plainly

$0/month. Both APIs are free tiers; 32 parts nightly is far inside
their limits. There are no servers, no databases, and no dependencies
to update — the whole pipeline is standard-library Python inside
GitHub Actions. The only files a human ever edits afterward are the
BOM CSVs (when the design changes) and `store.json` in the website
repo (when a price changes). There is no scheduled human task. The
system's only voice is a GitHub issue that names a part and the
decision it needs.

## Adopting this for your own project

Everything is in this repo under its open license: the fetcher
(`scripts/bom_pricing.py`), the schema gate (`scripts/lint_bom.py`),
the contract tests (`scripts/tests/test_bom_pricing.py`), the nightly
workflow (`.github/workflows/bom-pricing.yml`), and the CSV schema
(documented in [`README.md`](./README.md)). The pattern is three
rules: key every part by manufacturer part number; never hand-type
what a distributor knows better; make the only human touchpoint an
exception with a decision in it.
