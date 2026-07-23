# 19 — The Open Fulfillment Network & Builds Ecosystem: tools that link makers, printers, vendors, and neighbors — and don't rot

> Doc [16](16-kit-commerce-pricing-and-fulfillment.md) answered *"how do **we** sell
> kits?"* This doc answers the next question: *"how do parts, prints, and finished
> witnesses reach people **without us** in the loop — forever?"* It is the tool-level
> operating manual for the ecosystem the website already promises: the Maker Corps
> (`maker.html`, `corps.html`), commissions (`request.html`, "funded by neighbors"),
> and the build gallery (`builds.json`). Strategy context is
> [17-ecosystem-strategy.md](17-ecosystem-strategy.md) (kernel-shaped from day one);
> pricing and the founder-run pipeline stay in doc 16. Everything below was
> research-verified July 2026; sources inline and in §10.
>
> **The one-sentence verdict:** copy the Meshtastic/Voron/ESPHome playbook — the
> project maintains *documents, firmware, a flasher, and a catalog*; independent
> vendors, community printers, fab services, and platform mirrors do all the
> shipping — and wire every piece to repo files + scheduled CI so no part of it
> depends on a person remembering to care.

## 0. Principles (inherited, plus two new ones)

1. **Print-it-yourself stays free forever** (doc 16 §0). The network below sells
   convenience and funds makers; it never gates access.
2. **No backend, no data we hold** (corps.html). Every ecosystem artifact is a file
   in the repo rendered by the static site, or an external party's own store.
3. **New — the project's only permanent obligations are firmware, docs, the
   flasher, and the catalog.** Everything else must be owned by someone with their
   own motive to keep it alive (a vendor's margin, a fab's commission, a maker's
   pride), or generated/checked by CI.
4. **New — never betray the commons.** Eighteen years of platform history (§7.1)
   says communities forgive slowness but never betrayal: no silent relicensing, no
   exclusivity deals, no breaking remix lineage, no holding contributor data
   hostage. "You can leave with everything, anytime" is a feature we advertise.

## 1. The five layers

| Layer | Who does the work | What we maintain | Status |
|---|---|---|---|
| **Parts sourcing** (§2) | Distributors (Mouser/Digi-Key/Seeed) | BOM CSVs + CI freshness checks + one-click cart page | BOMs exist; CI to add |
| **Printed parts** (§3) | Community printers (PIF), POD farms, platform mirrors | STL/`.scad` sources, PIF vetting checklist, queue file | Sources exist; program to write |
| **Kits & devices** (§4) | PCBWay, Seeed, Elecrow, marketplace vendors | Shared-project listings, vendors page | To start |
| **Blessing & money** (§5–6) | Vendors fund themselves; fiscal host holds the pool | Trademark policy, badges, OSHWA cert, Open Collective | To write |
| **Builds & remixes** (§7) | Makers, on our gallery + platform mirrors | `builds.json` schema + CI pipelines + customizer | Gallery exists; mechanics to add |

## 2. Parts sourcing that never rots

The BOMs ([bom_canary_wap.csv](../hardware/bom_canary_wap.csv) et al.) already carry
Mouser/DigiKey/LCSC columns and a Lifecycle field — they are one convention tweak away
from fully machine-driven sourcing. Verified July 2026:

- **The old answer is dead; don't build on it.** Kitspace's *1-click BOM* browser
  extension was archived Jan 2025 and delisted from the Chrome store Sept 2025. Its
  **CSV column convention lives on** (parser still maintained:
  [npm-1-click-bom](https://github.com/kitspace/npm-1-click-bom)) — keep our columns
  compatible (`References/Qty/Description/Manufacturer/MPN/Digikey/Mouser/LCSC`).
- **One-click cart, keyless, from a static page.** Digi-Key's
  [myLists third-party API](https://forum.digikey.com/t/mylists-third-party-api-easily-get-parts-into-mylists-from-third-party-applications/61445)
  takes an unauthenticated POST of part numbers + quantities and returns a
  single-use URL that lands the whole BOM in the visitor's Digi-Key cart. A tiny
  static page (Lab or website, vendored JS, Invariant IV intact) turns every BOM
  into a **"Buy the parts" button that cannot rot behind an API key.** Mouser gets
  per-part links plus their BOM-upload page fed by our CSV.
- **CI freshness loop, free forever.** [Mouser Search API](https://www.mouser.com/api-hub/)
  and [Digi-Key Product Information V4](https://developer.digikey.com/products/product-information-v4)
  both offer free ~1,000-call/day tiers. One ~100-line script on a weekly Actions
  cron checks stock/price/lifecycle per BOM line, writes `bom-status.json`, and
  opens an issue when a line is EOL/NRND or out of stock at **all** listed vendors.
  A 20-line BOM uses <1% of either quota — this runs forever.
- **Link-rot check, zero keys.** [lychee-action](https://github.com/lycheeverse/lychee-action)
  weekly over BOMs + hardware docs, failing into an auto-created issue — catches
  dead Seeed/Waveshare product pages.
- **Multi-vendor fallback is real.** Digi-Key and Mouser both stock the XIAO line
  and Grove Vision AI V2 as franchised Seeed parts (verified SKUs, e.g. Digi-Key
  [102010634](https://www.digikey.com/en/products/detail/seeed-technology-co-ltd/102010634/26553885));
  Seeed's own store is the price/global fallback. Buy links are **generated from
  the CSV in CI**, never hand-written, so they can't drift.
- **Explicitly avoid:** Octopart/Nexar as CI backbone (free tier collapsed to a
  lifetime 100-part evaluation), unofficial LCSC endpoints (rate-limited, break —
  keep LCSC as plain checked URLs), and commercial BOM SaaS.

## 3. Printed parts without us: PIF + dropship + mirrors

Doc 16's phase plan covers *our* kits. This section is the community path that runs
in parallel and eventually carries most of the volume.

### 3.1 Print It Forward — the Voron pattern, upgraded by our hardware

[Voron's PIF program](https://pif.voron.dev/) is the proven blueprint for the Maker
Corps' Certified Maker tier: vetted community printers (sample parts inspected
against a published defect checklist by a named QC steward), a public first-come
queue, **at-cost pricing** — "filament, energy, and wear" — with buyers paying
printers directly, plus one or two *commercial* PIF licensees for buyers who want
card payment and consumer protection (Voron's Fabreeko model). 15,000+ serialized
Vorons prove it scales without the project shipping anything.

Our version is structurally stronger: **the Canary's signed first-boot self-test is
the QC that Voron approximates with photo inspection** — a PIF'd build vouches for
itself cryptographically (maker.html already says exactly this). Concretely:

- A `pif.json` (or extension of the Corps roster) in the repo: vetted printers,
  region, capacity, queue state — PR-edited, site-rendered, like `builds.json`.
- The vetting checklist as a doc (adapt Voron's published defect list; add our
  fit-coupon print as the sample part — it already names the parameter to fix per
  station).
- At-cost norm for the neighbor program (funded by the §6 pool); commercial-PIF
  licensees under the §5 badge for everyone else.

### 3.2 Dropship rail for parts-packs

[Slant 3D Teleport](https://teleportpod.com/): STL mapped to an Etsy/Shopify
listing; each order auto-prints on their farm and dropships in <36h — no inventory,
no subscription, full REST API. Any third-party seller can run R1 parts-packs on
our STLs this way; it's ecosystem enablement, not our job. (Craftcloud and
Treatstock are the multi-vendor/local alternatives; Treatstock's widget+API can
even power a community print-shop page later.)

### 3.3 Platform mirrors

Publish enclosures **non-exclusively** on Printables and MakerWorld under official
brand profiles (§7.4). Skip MakerWorld's Exclusive program — its +25% multiplier
requires delisting everywhere else, the exact anti-pattern §7.1 warns about.

## 4. Kits sold and shipped by parties that aren't us

Ranked by passivity (verified terms, July 2026):

| Channel | What happens per order | Pays the project | Catch |
|---|---|---|---|
| **[Seeed Co-Create](https://www.seeedstudio.com/co-create.html)** | Seeed manufactures, stocks, lists on their Bazaar, ships worldwide | Negotiated per-unit royalty | Contract-gated, per-SKU approval. Every Canary is XIAO-based — we are their target profile; XIAO designs get free PCBA prototyping |
| **[PCBWay Shared Projects](https://www.pcbway.com/project/)** | Anyone orders our shared design; PCBWay fabs and ships | **10% of order value** — explicitly incl. assembly, 3D printing, CNC | Payout to PCBWay account; the de-facto standard open-hardware royalty channel |
| **[Elecrow Partner Seller](https://www.elecrow.com/Elecrow_partner_seller_Sell_DIY_Eletronics_online)** | Elecrow builds, warehouses, lists on elecrow.com, ships | We keep margin; ~5% platform fee, no monthly | We're still seller-of-record; zero per-order labor once stocked |
| **[Lectronz](https://lectronz.com/pages/sell)** (third-party vendors) | Independent vendors sell kits they build | Nothing direct (badge/royalty optional, §5) | 5% fee, open-hardware-focused, free JSON API — the healthy maker marketplace |
| **[Crowd Supply](https://www.crowdsupply.com/)** | Campaign + Mouser logistics; fulfillment continues post-campaign | Campaign revenue | ~12% + per-item; the later R3 "boxed" move, unchanged from doc 16 |

**Tindie caution (supersedes doc 16's "parallel listing" note):** Supplyframe sold
Tindie in April 2026; the site went dark ~2 weeks mid-sale, payouts broke, and the
new owner (EETree LLC) is unproven. Wait-and-see; list on Lectronz instead.

## 5. The blessing layer — what makes strangers safe to sell our stuff

Highest leverage per hour of any section; it is pure documents.

1. **Dual-badge trademark policy** ([Meshtastic's](https://meshtastic.org/docs/legal/licensing-and-trademark/)
   is best-in-class): a **free compatibility badge** — *"Works with SecuraCV"* — no
   permission needed (their "M-Powered" analog; ideal for Etsy enclosure sellers
   and PIF printers), plus a **grant-required primary logo** (2-year revocable
   grants, recorded on a PR-editable public page). Ban the name in third-party
   product/company/domain names. The single most copyable clause is ESPHome's:
   product names may only *end with* "for ESPHome" — ours: **"… for SecuraCV"**.
   Templates: [Model Trademark Guidelines](http://modeltrademarkguidelines.org/),
   [FOSSmarks](https://fossmarks.org/).
2. **A vendors page that is a file in the repo** (WLED/Meshtastic pattern):
   `vendors.json` next to `builds.json` — alphabetical, neutral descriptions
   required, explicit no-ranking rule, listed by PR. Add a WLED-style **"gives
   back" badge** for vendors who contribute money/code/docs/support. Listing
   criteria are our quality bar: ships current firmware pre-flashed, config open,
   user can re-flash from our web flasher.
3. **[OSHWA certification](https://certification.oshwa.org/)** — free,
   self-certified, unique ID, searchable directory, real enforcement teeth. It is
   the neutral public promise that commercial derivatives are *welcomed*, not
   tolerated — vendors can certify their own derivatives too.
4. **The web flasher is the keystone and, later, the carrot.** Doc 16 §3.1 already
   plans ESP Web Tools. Meshtastic's Aug 2025 restructuring showed the endgame:
   *inclusion in the project's flasher and device catalog is what vendors
   ultimately pay for* (their Backer/Partner tiers fund a whole support company;
   "Community Supported" hardware gradually drops out of the flasher). Buyers can
   always re-flash stock firmware in one click — which also keeps pre-flashing
   vendors honest. We don't need tiers now; we need the flasher and catalog built
   so the lever exists.

## 6. Money rails we don't hold

- **[Open Source Collective](https://oscollective.org/) fiscal hosting** (10% of
  incoming; note Open Collective *Foundation* dissolved Dec 2024 — OSC is the
  right host): a transparent, founder-independent treasury that receives PCBWay
  commissions, Seeed royalties, and donations, and **pays Certified Makers' parts
  + time via expense submissions**. This is `request.html`'s "funded by neighbors"
  pool, implemented with zero infrastructure of ours — public ledger, no accounts,
  no company. [GitHub Sponsors](https://docs.github.com/en/sponsors) (0% from
  personal accounts) pays out to the fiscal host.
- **The royalty template for later:** OpenWrt One (Banana Pi pays Software Freedom
  Conservancy **$10/unit**) and [Arduino At Heart](https://www.arduino.cc/pro/partnerships-licensing/)
  (≤5% of wholesale for the badge) show the mature form — a small per-unit royalty
  in exchange for "Official Canary Kit" branding and top catalog placement. Money
  flows **vendor → collective**, never customer → founder.

## 7. The builds & remix ecosystem — Thingiverse's mechanics, our substrate

### 7.1 The eighteen-year lesson (compressed)

Thingiverse invented the formula — free CC hosting, **structured "remixed from"
lineage** (half its content was remixes; researchers reconstructed 100k+ chains
*because* lineage was data, not prose), **"I Made One!"** photo makes, human
curation, and the **Customizer** (2013: OpenSCAD comment annotations became web
sliders; the most-loved feature in the space's history). Then MakerBot burned every
trust layer: 2012 closed-sourcing + ToS rights-grab ("Occupy Thingiverse"),
declining to defend creators in the 2016 Just3DPrint scraping scandal, letting the
Customizer rot after 2017, a 2M-user breach in 2021, fire-sale to MyMiniFactory
Feb 2026. Printables rebuilt the formula honestly (clean remix flow with external
Model Origin URLs, contests, points, brand profiles). MakerWorld out-grew everyone
on one-click print profiles + cash points but is re-running the MakerBot arc
(exclusivity bribes with retroactive clawbacks, platform-scoped remix licenses, AI
slop, the 2025 Bambu firmware lockdown — doc 17 §"3D-printing mirror" called this).

**Anti-patterns we hard-ban:** silent relicensing; exclusivity deals; breaking
`remix_of` chains in any migration; letting the customizer go down without a status
note; unlabeled AI content; holding contributor data hostage.

### 7.2 Native mechanics to graft onto the gallery

The substrate exists: `builds.json` has `remix_of`, the Showroom renders SHA-pinned
committed STLs/GLBs, `enclosures.json` is generated, and every enclosure is a
parametric `.scad` configurator. Missing pieces, in value order:

1. **Lineage rendered both directions.** Every card: "Remixed from →" *and* an
   auto-computed "← Remixes of this," breadcrumbs for chains. `remix_of` accepts
   external URLs (a Printables/Thingiverse model), like Printables' Model Origin.
2. **License field with CI-validated flow-through.** Required `license` per entry
   (default CC-BY-4.0). CI rejects a remix whose license is incompatible with its
   parent's — the check MakerWorld ships broken. Contributors keep all rights
   beyond display; say so in CONTRIBUTING.
3. **Makes with photos.** A `makes` array per entry (photo, handle, printer +
   filament, date) via the same issue-form → PR flow. Photos of real hardware are
   the emotional engine *and* the AI-slop filter.
4. **The Customizer, self-hosted and unkillable.** Official OpenSCAD WASM builds
   (~3 MB compressed, [files.openscad.org/playground](https://files.openscad.org/playground/))
   + the actively-maintained [openscad-playground](https://github.com/openscad/openscad-playground)
   prove the whole flow on static hosting: `--export-format=param` dumps each
   `.scad`'s Customizer schema as JSON at build time → site renders sliders → WASM
   re-renders (Manifold backend, seconds not minutes) → STL streams into the
   Showroom's existing `STLLoader`. Single-threaded WASM sidesteps GitHub Pages'
   header limits; vendoring satisfies Invariant IV. The Gridfinity ecosystem runs
   exactly this client-side on Pages today
   ([working template](https://github.com/vector76/Web_OpenSCAD_Customizer)).
   **Annotate the `.scad` files with Customizer syntax once** — the same file then
   powers our widget *and* MakerWorld's Parametric Model Maker (§7.4).
5. **Curation on a rhythm.** `featured` flag, themed collections, an occasional
   challenge with modest prizes (a kit, a featured slot — Thingiverse's MakerEd
   challenge pulled ~800 entries on recognition alone). Recognition economy, not
   points economy: named credit everywhere, badges ("first remix," "10 makes").
6. **Print-success packaging.** MakerWorld's deepest lesson (83% retention): ship
   what makes the first print succeed. Per enclosure: STL + generic 3MF (OpenSCAD
   nightlies export 3MF with color/metadata; 3MF is ISO 25422:2025) + curated
   PrusaSlicer/Bambu project 3MFs + validated printer/filament notes fed by makes.

### 7.3 The CI backbone (all Actions + Pages, no backend)

| Pipeline | What it does | Effort |
|---|---|---|
| **A — drift gate** | Pinned OpenSCAD nightly docker regenerates STL/3MF from `.scad` on change; `git diff --exit-code` (or geometry-hash fallback) catches stale committed STLs; GitHub's built-in STL 3D diff makes review pleasant | S |
| **B — viewer assets** | STL→GLB via trimesh + `gltf-transform optimize`; poster PNGs via headless [F3D](https://github.com/f3d-app/f3d) with one catalog-wide camera/lighting; feeds the Showroom poster-first, lazy-init | M |
| **C — customizer widget** | Param-schema JSON at build time + vendored OpenSCAD WASM + sliders → `STLLoader.parse` → download STL/3MF | M |
| **D — interop index** | Per-model [`datapackage.json` (Manyfold 3D profile)](https://manyfold.app/technology/packaging.html) + schema.org `3DModel` JSON-LD (`isBasedOn` = lineage) + site-level `models.json`; nightly stats pull (Cults3D official GraphQL; Thingiverse legacy API; Printables best-effort) into `stats.json` | S |

Pipeline D is the quiet "works by itself" move: [Manyfold](https://github.com/manyfold3d/manyfold)
(very active, NLnet-funded, ActivityPub-federated self-hosted galleries) natively
ingests those Data Packages — anyone's instance can mirror our catalog into the
fediverse with zero involvement from us.

### 7.4 Ties into their systems

- **Official brand profiles on Printables and MakerWorld** as top-of-funnel
  mirrors; every listing description and every downloadable zip's README carries
  the canonical repo URL + license. Mirroring is a manual release-checklist step —
  no worthwhile auto-sync tool exists.
- **Upload the annotated `.scad` to MakerWorld** — its Parametric Model Maker runs
  the same Customizer syntax, giving us a free hosted customizer from the same
  file. Never accept exclusivity.
- **Thingiverse:** legacy mirror with a backlink, nothing more (API decaying, new
  owner unproven).
- **Stats flow home at build time** (Pipeline D), so gallery cards can show
  cross-platform reach without us running anything.

## 8. Why this can't rot

Every component is one of: **(a)** a file in the repo rendered by the static site
(vendors, PIF roster, trademark grants, BOMs, builds, makes); **(b)** a scheduled
Action on free-tier APIs that opens an issue when reality drifts (stock, EOL, link
rot, STL drift, license conflicts); or **(c)** an external party with its own
profit motive to keep shipping (PCBWay, Seeed, Elecrow, Slant 3D, Lectronz
vendors, community printers). The project's permanent surface stays: firmware,
docs, flasher, catalog. Meshtastic's support-tiering is the last-resort pruning
lever if the catalog ever accumulates dead vendors.

## 9. Rollout sequence

1. **Documents first (unblocks everyone else):** trademark policy + free
   "Works with SecuraCV" badge + OSHWA certification + `vendors.json` page.
2. **Sourcing CI:** BOM stock/EOL/link checks + Digi-Key myLists one-click cart.
3. **Passive rails:** PCBWay Shared Projects listings (boards *and* enclosures) +
   non-exclusive Printables/MakerWorld brand profiles.
4. **Money + people:** Open Source Collective for the neighbor pool; PIF vetting
   checklist + roster for Certified Makers.
5. **Gallery mechanics:** lineage both ways, license CI, makes; Pipelines A/B.
6. **Customizer:** annotate `.scad` files (instant MakerWorld PMM win), then the
   native WASM widget; Pipeline D interop index.
7. **Later:** Seeed Co-Create pitch; per-unit royalty program; Crowd Supply for
   the boxed R3 (unchanged from doc 16).

## 10. Open items

- [ ] Trademark/branding policy doc + "Works with SecuraCV" badge SVG (Meshtastic dual-logo pattern; "… for SecuraCV" naming clause).
- [ ] OSHWA self-certification for Canary WAP + Vision.
- [ ] `vendors.json` + website page (alphabetical, neutral, no-ranking rule, gives-back badge).
- [ ] BOM CI: weekly Mouser/Digi-Key stock+EOL script → `bom-status.json` + auto-issue; lychee link-rot workflow.
- [ ] Static one-click-cart page via Digi-Key myLists third-party POST; generated per-vendor buy links in docs.
- [ ] PCBWay Shared Projects: publish WAP/Vision (and enclosure sets on PCBWay 3D); route the 10% to the collective.
- [ ] Open Source Collective application (the "funded by neighbors" pool + maker expense flow).
- [ ] PIF: vetting checklist doc (fit coupon as sample part) + `pif.json` roster; name a QC steward.
- [ ] `builds.json` schema v2: `license` (required), external `remix_of` URLs, `makes[]`, `featured`, `mirrors[]`; issue-form → PR converter; CI license-compat check.
- [ ] Pipelines A (STL drift gate) and B (GLB + F3D posters for the Showroom/gallery).
- [ ] Customizer annotations in the four device `.scad` configurators; MakerWorld PMM + Printables brand-profile uploads with canonical backlinks.
- [ ] Native WASM customizer widget (vendored `files.openscad.org/playground` build); "Open in OpenSCAD Playground" deep-link as the free escape hatch.
- [ ] Pipeline D: `datapackage.json` per model + schema.org `3DModel` JSON-LD + nightly `stats.json` (Cults3D/Thingiverse pulls).
- [ ] Seeed Co-Create application once §4's PCBWay listings prove demand.

### Key sources

Ecosystem models: [Meshtastic trademark](https://meshtastic.org/docs/legal/licensing-and-trademark/) · [Meshtastic supported-hardware tiers](https://meshtastic.org/blog/updates-to-supported-hardware/) · [Voron PIF](https://pif.voron.dev/) · [Voron sourcing guide](https://docs.vorondesign.com/sourcing.html) · [Made for ESPHome](https://esphome.io/guides/made_for_esphome/) · [WLED compatible controllers](https://kno.wled.ge/basics/compatible-controllers/) · [OpenFlexure vendors](https://openflexure.org/about/vendors) · [OpenWrt One royalty](https://sfconservancy.org/activities/openwrt-one.html) · [OSHWA](https://certification.oshwa.org/)
Sourcing: [Digi-Key myLists API](https://forum.digikey.com/t/mylists-third-party-api-easily-get-parts-into-mylists-from-third-party-applications/61445) · [Mouser API hub](https://www.mouser.com/api-hub/) · [Digi-Key PIv4](https://developer.digikey.com/products/product-information-v4) · [lychee-action](https://github.com/lycheeverse/lychee-action) · [1-click BOM convention](https://github.com/kitspace/npm-1-click-bom)
Commissions/fulfillment: [PCBWay Shared Projects](https://www.pcbway.com/project/) · [Seeed Co-Create](https://www.seeedstudio.com/co-create.html) · [Elecrow Partner Seller](https://www.elecrow.com/Elecrow_partner_seller_Sell_DIY_Eletronics_online) · [Slant 3D Teleport](https://teleportpod.com/) · [Lectronz](https://lectronz.com/pages/sell) · [Crowd Supply](https://www.crowdsupply.com/) · [Open Source Collective](https://oscollective.org/)
Builds ecosystem: [openscad-playground](https://github.com/openscad/openscad-playground) · [OpenSCAD WASM builds](https://files.openscad.org/playground/) · [client-side customizer template](https://github.com/vector76/Web_OpenSCAD_Customizer) · [F3D](https://github.com/f3d-app/f3d) · [glTF-Transform](https://github.com/donmccurdy/glTF-Transform) · [Manyfold packaging](https://manyfold.app/technology/packaging.html) · [schema.org/3DModel](https://schema.org/3DModel) · [Cults3D GraphQL](https://cults3d.com/en/pages/graphql) · [MakerWorld PMM reference](https://mindflakes.com/posts/2026/05/04/makerworld-pmm-openscad-reference/) · Remix-culture research: [Flath et al. 2017 summary](https://3dprint.com/185160/thingiverse-3d-design-remixing/)
