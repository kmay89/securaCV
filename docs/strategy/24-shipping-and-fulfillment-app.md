# 24 — The shipping & fulfillment app: labels that never carry a typo

> Doc [16](16-kit-commerce-pricing-and-fulfillment.md) built the store and the
> weekly batch; doc [22](22-rapid-batch-and-education-fulfillment.md) sized the
> batches; doc [20](20-self-running-company.md) is the operations constitution
> this obeys. All three end at the same unautomated step —
> [`store-README.md`](https://github.com/kmay89/securacv_website/blob/main/store-README.md)'s *"bulk-import
> the address CSV into PirateShip, print labels."* That hand-import is the one
> place wrong data still enters: a mistyped ZIP, a guessed weight, the wrong
> box on a real label. This doc closes it with a small custom tool, and answers
> the founder's practical questions — **which label rail, what printer, what it
> all costs** — without adding a subscription or a backend.
>
> **The one-sentence verdict:** the "magic" isn't the label printer, it's the
> data pipeline in front of it — *one canonical catalog → validate the address →
> compute the package → buy the label → cross-check the device* — so the label
> is a printout of verified facts, never a retype. Built as
> [`scripts/fulfill.mjs`](https://github.com/kmay89/securacv_website/blob/main/scripts/fulfill.mjs) in the
> website repo, the exact shape of `margin-guard.mjs`.

## 0. Principles (inherited from doc 20, applied to atoms)

1. **One canonical source.** `store.json` already is the store; it is now also
   the packing spec. A product's weight and box live next to its price and BOM,
   so they can't drift apart.
2. **Facts are computed, never typed.** The SKU, the address, the weight, the
   box — every field on the label is derived or validated, never keyed by a
   human at 11pm on a Sunday.
3. **An exception summons a human; it doesn't ship.** An unknown SKU, a bad
   ZIP, a blank return address, a scale that disagrees with the manifest — each
   **blocks** the order with a reason. Nothing wrong goes in a box quietly.
4. **Nothing here spends money by itself.** Buying a label requires an explicit
   `--buy`, a token in the environment, and a complete origin address. The
   default run is a dry run that costs nothing and touches no network.

## 1. The reframe — where wrong data actually comes from

Fulfillment errors are not label-printer errors. They are **data-entry**
errors upstream of the printer: a customer's address transcribed by hand, a SKU
matched by eye, a package weight guessed from memory. So the tool is a
data-accuracy brain, and the label rail is the last dumb step:

```
Stripe order (the queue store-README already names)
  → resolve SKU from store.json          (id/name match; unknown ⇒ STOP, not a guess)
  → validate US address                  (structural always; USPS API when creds set)
  → compute package                      (weight = ship.oz×qty + box tare; box from the SKU)
  → scale cross-check                    (measured vs expected; drift ⇒ recount)
  → Shippo shipment → one-click label    (only with --buy + token + complete origin)
  → packing slip w/ QA line              (scan the flashed serial ↔ this order — doc 16 §3.2)
```

The scale cross-check is the quiet star: if the box on the scale doesn't weigh
what the manifest says it should, an item is missing or doubled — caught before
the tape goes on, not after a support email.

## 2. The label rail — decided: Shippo (verified mid-2026)

The brief was *USA-only, don't pay for things, custom over rented.* The rails,
as they actually stand in mid-2026:

| Rail | Fees | API? | Verdict |
|---|---|---|---|
| **Pirate Ship** | $0/mo, $0/label, cheapest USPS rates | ❌ No public API (only CSV upload + an unofficial reverse-engineered wrapper) | Great rates, but the automation is a fragile scrape — the manual step stays manual |
| **Shippo** ✅ | $0/mo, **30 labels/mo free then 7¢/label** (+2¢ optional US address validation), **no surcharge on postage**, discounted USPS | ✅ Real, documented | **Chosen.** ~$0–2/mo at a weekly kit batch; one-click labels straight from the tool |
| **EasyPost** | 3,000 free labels/mo then $0.08 — **but +3% on all USPS postage spend** (Jun 2026) | ✅ | The 3% is levied on *postage*, not labels: on a postage-heavy kit batch it outweighs the free labels — dearer than Shippo here |
| **USPS direct** | Free API calls; postage via an Enterprise Payment Account | ✅ Official, but enrollment + approval ceremony | Most "custom," heaviest setup — a later option, not the start |

**Chosen: Shippo**, for the one-click "works like magic" the founder asked for
at a cost that rounds to zero at this volume — and, precisely because it does
*not* tax postage, it stays cheaper than EasyPost's otherwise-generous free
labels once you weigh the ~$5–8 of USPS postage each kit carries. Crucially, the tool validates the
address and computes the weight **identically regardless of rail** — the rail
only decides *how the label is bought*. So `fulfill.mjs` isolates the Shippo
calls (`buyLabel`) behind a pure `shippoShipment()` payload builder; swapping in
Pirate Ship's CSV or USPS-direct later is a single-function change, not a
rewrite. Free USPS Addresses-API validation (developers.usps.com, OAuth, free)
layers on top as an optional extra standardization pass.

## 3. The data model (what changed in `store.json`)

Two additions, both file-local, both guarded by the test suite:

- **Per product, a `ship` block:** `{ "oz": <packed weight>, "box": "<class>" }`.
  These weights are operator estimates in the same spirit as `cogs.extra_usd` —
  a starting point the scale then confirms on every box.
- **A top-level `fulfillment` block:** `ship_from` (return address — blank in the
  repo, and the tool refuses to buy until it's filled), `boxes` (the mailer/
  small/medium classes with dimensions + tare), `rail`, `default_service`
  (USPS Ground Advantage — cheapest US ground for a sub-1lb kit), and
  `weight_tolerance_oz`.

Package rule, deterministic and documented: `content = ship.oz × qty`; box =
the product's declared box for a single unit, escalated to the bundle box for
multi-unit orders; `total = content + box tare`. A qty above four is flagged for
a human rather than crammed into one box — the brief's *"orders can't be
complicated beyond reason"* encoded as a tripwire.

## 4. Costs — printers, materials, overhead (the founder's question)

Anchored to the BOM/print figures already in docs 16, 18, and 22, plus the
shipping-station line the earlier docs didn't cost.

### One-time, to stand fulfillment up from zero

| Item | Cost | Note |
|---|---|---|
| Enclosure printers (2× Bambu A1-class) | $400–800 | **Likely already owned** per the enclosure docs — then $0 |
| Flashing rig (powered 8-port USB hub + 8 cables) | $60–80 | 8 boards/batch, ~15–20 s/board amortized (doc 16 §3.2) |
| **4×6 thermal label printer** (Rollo / used DYMO 4XL / Munbyn) | **$100–180** | The real "magic" buy: direct thermal, **no ink ever**, no smudge |
| **0.1 oz digital shipping scale** | **$25** | Measured weight = correct postage = the never-wrong keystone |
| Starter packaging + filament + thermal label rolls | ~$100 | mailers/boxes, void fill, tape, 4×6 rolls |
| **All-in** | **~$735–1,235** (**~$335–435 if the printers already exist**) | |

The thermal printer + scale together (~$150) are what turn "never wrong" from a
hope into a mechanical fact: the weight is read off a scale, the label is
printed from validated data, and neither passes through a keyboard.

### Per-order variable (USA, ~1–1.5 lb kit)

| Line | Cost |
|---|---|
| Enclosure material (in-house PETG/ASA) | $1–2 |
| Packaging + QR quick-start card | ~$2 |
| Postage (USPS Ground Advantage, Shippo discounted) | ~$5–8 |
| Label fee (Shippo) | 7¢ (first 30/mo free) |

All of this already sits inside `store.json`'s `cogs.extra_usd` (~$9/kit) and
the fully-loaded costs in doc 22 — so the tool changes the *labor and accuracy*,
not the margins. The recurring software cost of the whole system remains **$0/mo**
(Shippo is pay-per-label with a free floor; everything else is a repo file).

## 5. The maker pairing — the Hermes idea, made literal

The "maker pairing" is already the product: the **Maker Corps** (`maker.html`)
and **"have one made for you"** (`request.html`), a Certified Maker paired with
someone who can't build their own, funded by the neighbors pool (doc 19 §6).
The Hermes instinct — *encourage people to do it themselves, and when I do it,
make it special* — lands as **two modes of one tool**:

- **Hermes (self-serve courier).** Because `fulfill.mjs` is a repo file with no
  backend and its own Shippo token, any Certified Maker fulfilling for a
  neighbor runs the same validate → label → packing-slip flow from their own
  account. Fulfillment **federates out** to the corps instead of bottlenecking
  on the founder — the doc 19 network, now with a real fulfillment tool in it.
- **"If I don't, I make it special."** When the founder fulfills, it's the R3
  *Assembled Witness* premium path (doc 16 §1): the in-house key ceremony, the
  serialized bench test, the nicer box and a signed provenance card. Same tool,
  a deliberately special branch — the atoms carry the premium, the bits stay
  free (doc 22 §5).

Both modes share the QA line that ties a **flashed board's serial to a specific
order** — the corps' promise ("the device vouches for itself", maker.html) made
operational at the packing table.

## 6. What's built vs. what's next

**Built now** (this change): `scripts/fulfill.mjs` + `fulfill-README.md`, the
`store.json` `ship`/`fulfillment` data, a runnable `fulfill.sample.csv`, and
`tests/fulfill.test.mjs` under the site's existing `node --test` gate. It runs
today as a dry run with zero setup and becomes one-click live the moment a
Shippo token and a return address exist.

**Next, in doc-20 order (cheapest lever first):**

- [ ] Founder fills `fulfillment.ship_from` and creates the Shippo token (the
      only blockers to the first real label).
- [ ] Confirm the Stripe Payments export's exact column headers against
      `FIELD_ALIASES`; adjust the one line if a header differs.
- [ ] Weigh a real packed kit of each SKU and replace the estimated `ship.oz`
      values with measured ones (the scale makes this a five-minute job).
- [ ] Wire the flash manifest (doc 16 §3.2 `create_manifest.py`) into the QA
      line so the serial field is pre-filled and *verified*, not just prompted.
- [ ] Optional: a `--rail pirateship` CSV emitter for the cheapest-rate path,
      and USPS-direct once volume justifies the Enterprise Payment Account.
- [ ] Optional: group multiple same-address payments into one shipment.

## 7. Why this can't rot

Every piece is the doc-20 shape: **one canonical source** (`store.json`);
**facts computed, never typed** (SKU, weight, box, address); **a drift gate**
(the address/weight/SKU checks, plus the test suite in CI); **exceptions summon
a human** (blocked orders, the origin guard, the scale mismatch); and **an
external party with its own motive** carries the money rail (Shippo per-label,
USPS postage). The project's permanent surface doesn't grow — it's one more
small script next to the margin guard, judged nightly by the same `node --test`
the rest of the site already trusts.
