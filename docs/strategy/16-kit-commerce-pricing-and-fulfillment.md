# 16 — Kit Commerce: pricing, printed-parts fulfillment, and the flashing pipeline

> The business-model call in [05](05-market-and-cost-comparison.md) was: monetize the
> *trust* — sell hardware, give away software. This doc turns that into an operable
> plan: a kit SKU ladder priced from the real BOMs
> ([bom_canary_wap.csv](../hardware/bom_canary_wap.csv),
> [bom_canary_vision.csv](../hardware/bom_canary_vision.csv),
> [bom_canary_display.csv](../hardware/bom_canary_display.csv)), a pipeline for getting
> printed parts to people who order boards, and a firmware flashing/provisioning
> pipeline that doesn't consume the founder's life one serial port at a time.
> Marketing materials live in [15-maker-marketing-kit.md](15-maker-marketing-kit.md).

## 0. Principles (inherited, non-negotiable)

1. **Print-it-yourself stays free forever.** STLs/`.scad` on Printables et al., BOMs
   public. Kits sell *convenience*, never *access*. This is the trust engine and the
   top of the funnel — a maker who prints the case and sources their own XIAO is a
   marketing asset, not a lost sale.
2. **Sell honestly.** Kit availability mirrors `canary-local/devices/registry.json`
   statuses. "In development — bench pending" devices get a waitlist, not a buy button.
3. **Never ship customer Wi-Fi credentials.** Provisioning happens in the customer's
   home via the shipped QR/captive-portal flow. The factory flashes *generic* firmware
   only. (This single fact makes the whole flashing problem tractable — §3.)

## 1. The SKU ladder

Four rungs of convenience per device. COGS figures use the BOM subtotals at single-unit
maker pricing; 25–50-unit sourcing improves them ~10–20% (noted inline). Packaging/label/
QR-card allowance: ~$2/kit. Prices target ~2.2–2.6× COGS — sustainable for direct sales
(Tindie/own store); a future distributor/Crowd Supply path needs the 2.6× end.

| Rung | What ships | Who it's for |
|---|---|---|
| **R0 — Plans** (free) | STLs, `.scad`, BOMs, flash-from-browser page | Pure DIY; the funnel |
| **R1 — Printed Parts Pack** | The enclosure set + hardware kit (screws, inserts, window, gasket filament piece) | Maker who owns the boards or wants to source them |
| **R2 — Full Kit** (the volume SKU) | Boards + printed parts + hardware + cable + QR quick-start card, **pre-flashed** | Maker who wants the fun of assembly without the sourcing |
| **R3 — Assembled Witness** | Built, tested, serialized, provisioned-to-first-boot | Non-builders; gift market; the at-risk persona (doc 04) |

### Per-device pricing (launch table)

| SKU | COGS (est.) | R1 Parts Pack | R2 Full Kit | R3 Assembled | Status gate |
|---|---|---|---|---|---|
| **Canary WAP** (XIAO S3 Sense $14.90 + high-endurance SD $8.50 + cable + printed case + pkg ≈ **$28**) | $28 | $15 | **$69** | $99 | released — sell now |
| **Canary Vision** (XIAO C3 $5.50 + Grove Vision AI V2 $29 + OV5647 $8 + cables + printed set + hinge hardware + pkg ≈ **$53**) | $53 | $19 | **$119** | $159 | released — sell now |
| **Canary Watch Station** (XIAO S3 $7.49 + Round Display $18 + printed puck + pkg ≈ **$31**) | $31 | $12 | **$79** | $109 | in dev — waitlist |
| **Canary Dash** (Waveshare 4.3 ≈ $33–37 + PSU $8 + printed case + pkg ≈ **$48**) | $48 | $15 | **$109** | $139 | in dev — waitlist |

Options (mirror the Lab configurator's option cards): weather kit (+$15 — GORE vent,
sealed plug, TPU gasket, PMMA window, neutral-cure silicone), battery kit (+$12 —
protected cell only, chemistry per climate, ships per BOM safety notes), doorbell
faceplate (+$9). Battery SKUs follow the BOM's safety-critical notes verbatim; no
unprotected cells, ever.

Bundles: **Starter Fleet** (Watch + 2× WAP) $199 (vs $217 à la carte) · **Whole-House**
(Dash + Vision + 2× WAP) $329 (vs $366). Bundles are where the display devices pull
the witnesses through.

Early-bird / crowd-campaign pricing: −15% is affordable inside the margin; never
discount below 1.8× COGS or the channel fees eat the line.

### Channel sequence

1. **Now — the static store on securacv.com** (decided; built as `store.html` +
   `store.json` in the website repo — see its `store-README.md`). No SaaS storefront:
   Stripe Payment Links do checkout ($0/month, ~2.9% + 30¢/sale), PirateShip does
   labels, and the entire catalog is one JSON file. Founder-time protections are
   structural: fixed SKUs only (indoor/outdoor per device, two bundles, two parts
   packs — customization is what the free STL path is for), per-batch stock caps so
   demand can never outrun one weekly build session, and price as the demand valve.
   A "$0/month forever" brand running its own store with zero subscriptions is also
   the right story. Tindie remains a good parallel listing for marketplace discovery
   and as overflow if a launch spike ever outruns payment links.
2. **Only if operational pain demands it — a SaaS storefront** (Shopify Starter $5/mo
   or Basic $39/mo): the triggers are multi-item carts, multi-state tax automation, or
   wanting a print-on-demand dropship app wired directly into checkout. Not revenue —
   pain.
3. **Later — Crowd Supply campaign** for the R3 "Canary, boxed" productization; their
   audience is the target tribe and Mouser handles logistics. This is also the moment
   for CE/FCC conversations (kits of listed dev-boards ≈ fine; a boxed product with our
   name on it wants intentional-radiator diligence — budget it into the campaign).

## 2. Printed parts: the fulfillment pipeline

The problem: customer orders boards → printed parts must appear in the box (or at their
door) without the founder babysitting printers. Three phases, switch on volume:

### Phase 1 (0–50 orders/mo): in-house micro-farm
Two printers (e.g. Bambu/Prusa class), PETG/ASA per the enclosure docs. Material cost
per set is cents-to-$2; print time is the constraint. Batch weekly against open orders;
the enclosure library's "committed STLs are print-validated" policy is the QA gate —
kits ship only print-validated geometry (registry statuses again). A rainy-Saturday
queue of 20 WAP cases is genuinely fine at this volume; the labor that matters
(packing) exists regardless.

### Phase 2 (50–300/mo): print-on-demand routed per order
Two complementary routes:
- **Batch-to-stock via industrial POD**: JLC3DP / PCBWay 3D — order 50–100 sets per
  revision in MJF nylon (hinge/detent parts love MJF) or FDM; landed cost per set
  typically $2–6; hold as kit inventory. Best $/part, one-week lead, no per-order labor.
- **Dropship POD for R1 parts-packs**: a print-farm service with a storefront/API
  integration (e.g. Slant 3D's Shopify/API path in the US) — order comes in, STL job
  fires, farm prints and ships direct. Zero inventory, per-unit cost higher; use it for
  the long tail (parts-only orders, color options) while kits pull from MJF stock.

Gate to enter Phase 2: a frozen enclosure revision (parametric variants collapse to a
small SKU set — pick the 2–3 configurator presets the Lab already highlights).

### Phase 3 (300+/mo): kitted at the assembler
Elecrow / Seeed Fusion class services do sourcing + enclosure + kitting + worldwide
fulfillment for open-hardware projects; boards are Seeed's own XIAO line, which makes
Seeed Fusion unusually convenient (they can kit their own boards with our printed or
injection parts). At sustained volume, injection molding the two highest-running cases
(~$3–8k/tool) drops per-part cost below $1 — a Crowd Supply campaign's economics.

**Decision now:** start Phase 1, list on Printables the same week (free R0), and get
one JLC3DP MJF quote for the WAP + Vision sets so Phase 2's trigger is a purchase
order, not a research project.

## 3. Firmware: the flashing & provisioning pipeline

The fear ("I'd have to flash every board and do Wi-Fi provisioning — too slow") divides
into three jobs with three different answers:

| Job | When it happens | Answer |
|---|---|---|
| Put generic firmware on a board | Factory (us) — or never | Web flasher (R0/R1) · batch jig (R2/R3) · assembler pre-flash (scale) |
| Wi-Fi + hub join | **Customer's home, always** | Already shipped: QR-off-the-display / phone Wi-Fi QR / captive portal |
| Device keys / identity | First boot (auto) or bench (R3) | Firmware auto-provisions keys on first boot; R3 adds the bench ceremony |

### 3.1 The web flasher (R0/R1 — zero factory labor)

Adopt the ESPHome/WLED pattern: **ESP Web Tools** on a static page in the Lab
(`canary-local/flash.html` or the GitHub Pages deployment).

- CI already builds the flavors; add a release artifact per flavor: single merged image
  (`esptool merge_bin` of bootloader + partition table + app, with `--chip` derived
  from each PlatformIO env — `esp32s3` for WAP/displays, `esp32c3` for the Vision C3
  flavors; bootloader offsets differ per chip, so one hard-coded command cannot serve
  every flavor) plus an ESP Web Tools `manifest.json` (chip family, version, offsets).
- User plugs the XIAO into USB, opens the page in Chrome/Edge, clicks **Install** —
  WebSerial does the rest. No Python, no drivers ceremony, no cloud (the page serves
  static binaries; Invariant IV holds — nothing phones anywhere else).
- After flashing, the board reboots unprovisioned and the *existing* onboarding takes
  over: point it at the display's QR, or scan a phone-minted `WIFI:` code, or captive
  portal. **Improv Wi-Fi Serial** (set credentials from the same browser tab) is a
  nice-to-have addition to the firmware, not a blocker — the QR path is our
  differentiated, already-shipped answer.
- This page is also a marketing asset: "flash it from your browser" belongs in every
  channel post (doc 15 §4).

### 3.2 The batch jig (R2/R3 — pre-flashed kits)

Pre-flashing is worth it for kits (the out-of-box moment is "power it on and point it
at the glass", not "install esptool"). It is also cheap:

- A powered 8-port USB hub + `esptool` scripting = 8 boards per batch; flashing is
  parallel and takes ~1 min/batch, so the real cadence is seat/unseat labor —
  ~15–20 s/board amortized. 100 kits ≈ an hour of podcast time. The existing
  `firmware/provisioning/` kit (`verify_device.py`, `provision_canary.sh`) is the
  skeleton; add a `flash_batch.sh` that walks `/dev/ttyACM*`, flashes the merged
  image, reads MAC + firmware hash, and appends to `create_manifest.py`'s fleet
  manifest. Print each unit's serial/QR label from that manifest row.
- **No per-unit Wi-Fi work exists** — that was the imagined cost that made this feel
  not-worth-it. Generic image in, box closed, customer provisions at home by camera.

### 3.3 Scale + the secure tier

- At assembler volume (Phase 3), Seeed Fusion/Elecrow flash the merged image during
  test — pre-flashing leaves our building entirely.
- **R3 "Assembled Witness" and any future evidence-grade SKU** run the Phase-2 secure
  provisioning path from `firmware/provisioning/` (Secure Boot v2, flash encryption,
  eFuse burn — irreversible, dry-run first) **in-house only**. Key ceremony never goes
  to a contract manufacturer. This is the premium the R3 margin pays for.
- OTA (docs/firmware_ota.md) means a stale factory image is fine: first boot on the
  customer's LAN updates through the normal signed-OTA path. Factory images therefore
  freeze per release train, not per build.

## 4. Unit economics sanity check (WAP full kit, R2 @ $69)

| | $ |
|---|---|
| Parts (BOM, modest 25-unit sourcing) | ~25.50 |
| Printed set (Phase 2 MJF, landed) | ~3.50 |
| Packaging + label + QR card | ~2.00 |
| Flashing labor (amortized §3.2) | ~0.25 |
| **COGS** | **~31** |
| Tindie/processing fees (~8%) | ~5.50 |
| Shipping loss allowance (flat-rate delta) | ~2.00 |
| **Contribution per kit** | **~$30 (≈44%)** |

30 kits/mo ≈ $900/mo contribution — pays for filament, printers, and creator seeding
within the first quarter; it does not pay a salary, and doesn't need to (doc 05: the
software is the moat, hardware is the honest revenue line, the attestation service is
the eventual margin).

## 5. Open items

- [ ] Freeze enclosure presets per device for the kit SKUs (2–3 configurator presets each).
- [ ] `flash_batch.sh` + merged-image + `manifest.json` CI artifacts (unblocks web flasher AND jig).
- [ ] `canary-local/flash.html` with ESP Web Tools (vendored, no CDN — Invariant IV).
- [ ] JLC3DP MJF quote for WAP + Vision sets (Phase-2 trigger).
- [ ] Store go-live: create the Stripe products + payment links (adjustable qty,
      US shipping, flat rate) and paste them into the website repo's `store.json` —
      the buy buttons switch on per-product; empty link = waitlist state. Set the
      flat rate off the website repo's box-fit tool (`node scripts/box-fit.mjs`),
      not a guess — it's the packing bench's box-sizing guide, multi-box/backorder
      splitter, and fix-not-return policy in one (`docs/shipping-and-fulfillment.md`).
- [ ] Optional Tindie listing for WAP + Vision R1/R2 (marketplace discovery + overflow).
- [ ] Improv Wi-Fi Serial in canary firmware (nice-to-have, post-launch).
- [ ] FCC/CE diligence memo before any R3/Crowd Supply boxed product.
