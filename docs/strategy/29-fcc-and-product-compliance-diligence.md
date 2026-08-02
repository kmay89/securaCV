# 29 — FCC & product-compliance diligence: what selling a radio actually costs

> The memo that doc 16 §5 has been carrying as an open item ("FCC/CE diligence
> memo before any R3/Crowd Supply boxed product"). Doc 27 covers the *tort and
> contract* side — entity, caps, warranties, marketing discipline. This one
> covers the *regulatory* side: equipment authorization, labeling, power, and
> the children's-product rules. They stack; neither replaces the other.
>
> **This is not legal advice.** It is engineering-grade diligence with the rule
> citations attached so a real attorney or a test lab can check the work fast.
> The residual step is still human.
>
> **It also corrects a load-bearing error.** Docs 18 §4 and 25 §1.3 previously
> advised selling **kits** as a way to "dodge the FCC cliff." That is wrong on
> FCC, and only partly right on liability. §5 below is the correction; the two
> docs have been edited to match.

---

## 1. The one-paragraph version

A pre-certified Espressif module carries the *radio*. It does not carry your
*product*. You inherit the expensive half (intentional-radiator certification)
and still owe the cheap half (Part 15 Subpart B on the assembled thing, plus
labeling and manual statements) — call it **$1.5–4k and a few weeks per design**.
Selling it unassembled does not remove that. **Not selling the electronics at
all does** — and that is the shape this project should hold for now: free plans,
free firmware, and **printed enclosures as the only physical SKU**.

## 2. What the module grant gives you, and what it doesn't

Espressif's WROOM/WROVER modules hold **modular approval** under
[47 CFR 15.212](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-C/section-15.212).
You inherit that grant for the transmitter — provided you don't break its
conditions:

- the antenna is the one certified (module trace antenna, or the exact listed
  external part)
- no changes to RF circuitry, shielding, or the module's operating conditions
- the host is within the permitted category on the grant

Break any of those and the grant is void and the full intentional-radiator
certification ($5–20k) is yours.

| Obligation | Covered by the module grant? |
|---|---|
| Intentional radiator (the Wi-Fi/BLE radio itself) | ✅ yes |
| **Part 15 Subpart B — the assembled product as a digital device** | ❌ **no, ours** |
| Labeling on our enclosure | ❌ ours |
| User-information statements (15.19 / 15.105) | ❌ ours |
| RF exposure for a **worn or handheld** device (SAR) | ❌ almost never — see §8 |
| Being the responsible party when it interferes | ❌ ours |

FCC guidance is explicit that a host product must obtain its own authorization
for the unintentional-radiator functions even when it incorporates a certified
module, and that testing is performed **with the transmitter operating**. Our
display, its ribbon, the USB-serial bridge, the regulators, and the SPI clock
rates our firmware chooses were never in Espressif's test envelope.

The route for a product like ours is **SDoC** (Supplier's Declaration of
Conformity), not Certification: nothing is filed with the FCC, we hold the test
report and write the declaration. Budget **~$1.5–4k** for a single-SKU emissions
scan.

## 3. The dev-board trap (check this before any SKU commitment)

The *module* is certified. The **dev board generally is not a certified end
product** — it adds a USB-serial bridge, oscillators, regulators and a display
that were never in the module's test.

Worse: many dev/eval boards are marketed under
[47 CFR 2.803](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-2/subpart-I/section-2.803)'s
evaluation-kit provisions, which permit sale to a narrow class of developers but
**explicitly prohibit offering them for sale to end users in residential
environments**. Ship one of those to a consumer and we are marketing an
unauthorized device.

**The check, per board, before it enters a sellable SKU** — look the exact board
up on `fccid.io`:

| What you find | What it means |
|---|---|
| The **board** holds its own grant | Best case. Still owe Part 15B on our integration. |
| Only the **module** holds a grant | Normal and expected. Part 15B is ours. |
| Board says *"for evaluation / development only, not for resale"* | **Do not ship it to customers.** Full stop. |

This applies to every board in `canary-local/devices/registry.json`, and
specifically to the Waveshare ESP32-S3-Touch-AMOLED-2.06
([`firmware/boards/waveshare-esp32s3-amoled206/`](../../firmware/boards/waveshare-esp32s3-amoled206/README.md))
before it is ever part of a paid SKU.

## 4. What has to be on the enclosure

For an assembled, branded, US-market product:

1. **`Contains FCC ID: <module ID>`** — required because the module's own label
   is not visible through an opaque printed shell. *"Contains transmitter module
   FCC ID: …"* is equally acceptable. List every radio's ID.
2. **The [15.19](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-A/section-15.19)
   statement** — *"This device complies with part 15 of the FCC Rules. Operation
   is subject to the following two conditions: (1) This device may not cause
   harmful interference, and (2) this device must accept any interference
   received, including interference that may cause undesired operation."* If the
   device is too small to carry it, this may move to the manual or the container.
3. **Brand + model number**, and the SDoC responsible party's **name, US address,
   and phone or email**. These may live on the product, the packaging, *or* the
   manual — the box is fine.
4. **The Class B statement** per
   [15.105](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-B/section-15.105)
   — manual only, and an electronic/web manual is explicitly permitted, which
   suits a project whose docs are already the manual.

**For any device with a screen, e-label it.** The E-LABEL Act and the FCC's
implementing rules let a device with an integrated display show the FCC ID and
required statements **on screen instead of on the case**, provided: access takes
no more than three steps from the main menu, no special accessory or code is
needed, the user cannot modify or delete it, instructions for finding it appear
in the manual **and on the packaging**, and the physical label still goes **on
the box**. A `Settings → About → Regulatory` screen satisfies this completely and
keeps the printed shell clean. This is the right answer for the Watch Station,
the Dash, and the AMOLED companion devices, and it costs nothing.

**Not required federally, do it anyway:** a **serial or batch number** on every
unit. A recall or a warranty claim is unexecutable without one, and it is the
cheapest liability tool that exists. The signed self-test (doc 25 §4) already
mints a per-unit identity — surface it physically.

**Never apply a CE or UKCA mark** without having done the EU/UK conformity work.
US-only sales need neither, and applying an unearned CE mark is its own offense.

## 5. Enclosure material: what PETG does and doesn't buy us

| Concern | Position |
|---|---|
| **Marking legibility** | Labels must be permanent and legible to the naked eye. Printed-in text is permanent by definition; for FDM keep it ≥4 mm cap height at 0.4 mm line width with a filament colour swap, or recess a pocket for a laser-printed polyester label. **E-label + box label is less work and looks better.** |
| **Flammability** | PETG is typically **UL 94 HB**, not V-0. No federal rule forces V-0 on a 5 V USB limited-power device and the real fire risk there is low — but HB is the answer that fails a retailer safety questionnaire, a UL/ETL listing, or an insurer's underwriting. Flame-retardant V-0 PETG filaments exist if that day comes. |
| **Heat** | PETG's Tg is ~80 °C. Dashboards and south-facing windows will sag it. A warranty problem, not a legal one — **unless there is a lithium cell inside**, which is the combination to avoid. |
| **Skin / children** | Mechanically fine, but a children's product triggers CPSIA lead and phthalate limits, which are **third-party lab tested, not self-declared**. See §8. |

## 6. Power: the free win

**Do not ship a wall adapter.** Bundling one inherits:

- **DOE Level VI** efficiency, mandatory for any external power supply shipped
  into the US *including when packaged with a product*, and including USB-C PD
  chargers — with its own required marking
- product-safety expectations (**UL 62368-1** — not federally mandated, but what
  CPSC, insurers, and payment processors assume)
- a **separate FCC 15B obligation** for the adapter itself

Ship a USB-C cable and print *"Use any UL-listed USB power source, 5 V ⎓ 2 A."*
The mains-side liability stays with whoever made the adapter. This costs nothing
and deletes an entire regulatory domain.

**Note this contradicts a BOM line.** The Canary Dash COGS in doc 16 §1 includes
a *"PSU $8"*. Drop the PSU from the shipped configuration and the SKU gets
cheaper *and* cleaner.

**Lithium cells are the biggest single item on this page.** A battery SKU brings
UN 38.3 transport testing, IEC 62133 cells, carrier shipping rules — and a Li-po
inside an HB-flammability printed case is the exact fact pattern that produces
lawsuits. The battery option (doc 16 §1, *+$12*) should stay a documented
self-build choice, never a shipped one.

## 7. The kit myth — the correction

Docs 18 and 25 previously treated "sell it as a kit" as an FCC escape hatch. It
is not.

- [47 CFR 15.23](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-A/section-15.23)
  — the home-built exemption everyone reaches for — requires the device be **not
  marketed**, **not constructed from a kit**, and built in quantities of **five or
  fewer for personal use**. It excludes kits *by name*, and it excludes anything
  sold.
- Marketing an RF device is regulated by the **state of marketing**, not the
  state of assembly. A pre-flashed board plus a printed case that the buyer snaps
  together is a functional radio that we sold. Part 15 transmitters must be
  authorized before marketing whether they ship assembled or as kits.

**Where "kit" genuinely does help is product liability, not FCC** — and only in a
specific shape. Once our firmware is on it and our name is on the box, we are a
manufacturer for tort purposes in essentially every US state, regardless of who
turned the last screw.

So the real fork is **not** kit-vs-assembled. It is **plastic-only vs.
anything-with-a-radio**:

| SKU | Radio in the box? | Regulatory load |
|---|---|---|
| **R0 — Plans** (free) | no | none — publishing designs and firmware is unregulated |
| **R1 — Printed Parts Pack** | **no** | **none** — a plastic shell is not a digital device |
| **R2 — Full Kit** | yes | full §2–§6 stack |
| **R3 — Assembled** | yes | full §2–§6 stack |

R0 and R1 are on one side of a bright line; R2 and R3 are on the other. There is
no middle rung.

## 8. The plastic-only safe harbor (the recommended posture)

Publishing the designs, publishing the firmware, and selling **only printed
enclosures** is the cleanest available shape, and it is the one this project
should hold until a deliberate decision says otherwise. Three independently clean
activities:

1. **Publishing designs, schematics and firmware is unregulated.** The FCC
   regulates the marketing of *devices*, not information. The repo is already
   public under an AS-IS license — the right posture, already in place.
2. **Selling printed enclosures is not selling an RF device.** No SDoC, no
   labeling, no test report. It is plastic.
3. **The builder is covered by 15.23** — five or fewer, personal use, not
   marketed, not from a kit, good engineering practice expected. The certified
   module does the heavy lifting.

The whole thing works because **we never place a radio into commerce.**

**The lines that collapse it** — each turns us back into a manufacturer of a
radio:

| Don't | Why |
|---|---|
| Bundle boards with the plastic | Now marketing an RF device; 15.23 excludes kits by name |
| Pre-flash boards we ship | Same, and it makes the firmware ours in a *product* sense rather than a *publishing* sense |
| Drop-ship or resell boards, even at cost | Seller of record for a radio = marketing it |
| Call the parts pack a *"kit"* | Words are load-bearing here. It is an **enclosure set** or **printed parts pack** — never a product name implying a working device |
| Sell plastic + screws + a BOM link + a flashing tool as one checkout | Functionally a kit even if the boards ship from elsewhere. Store page sells plastic; the build guide stays free documentation |

Affiliate/commission rails (doc 19 §6) are fine — we are not the seller of record.
**Do not take the board order.**

**What we still owe, which is little:** a truthful description of the plastic
(material, dimensions, fitment — no untested weather, impact or flame claims),
the doc 27 entity and insurance floor, and **marketing discipline about the
system as a whole**. If the store sells "an enclosure" while the site sells "a
security camera that protects your home," those pages are read together. Doc 27
§5's banned-words table already governs this; it applies to store copy too.

### The exception that matters: children's products

**A plastic enclosure sold for a children's product is itself a children's
product** under CPSIA — third-party lead and phthalate testing at a CPSC-accepted
lab, a Children's Product Certificate, and a permanent tracking label, on a $3
printed part. **The plastic-only safe harbor does not apply there.**

Sell enclosures for the adult devices freely. **Do not sell Tin Can / kids-watch
shells** until that testing exists, or do not market them as being for children.
This matches the pre-launch gate already standing in
[`canary_tincan_kids_watch.md`](../design/canary_tincan_kids_watch.md) §3.3 and
§9 — and note the same doc's worn-device SAR problem: a wrist device is a
*portable* RF-exposure condition that a module grant will not cover.

## 9. What it costs to cross the line deliberately

If and when a boxed product is worth it, for **one** SKU:

| Item | Cost |
|---|---|
| Part 15B SDoC emissions test, single SKU | ~$1,500–4,000 |
| LLC (already planned — doc 27 §3) | ~$100–800 |
| Product liability insurance | ~$500–1,500/yr |
| **Total to be genuinely legal on one SKU** | **~$3–6k** |

Cheap relative to the $5–20k intentional-radiator number that docs 18 and 25 were
built around — because the module grant already bought that. The mistake was
believing kits made even this go away.

**The SKU to pick first**, when the time comes: USB-powered, **no battery, no
bundled adapter**, no camera, no microphone, no children's marketing. Each of
those choices deletes a whole regulatory category rather than mitigating one.

## 10. Checklist

- [ ] Look up every board in `canary-local/devices/registry.json` on `fccid.io`;
      record grant status per board and flag any marked evaluation-only (§3).
- [ ] Hold the **plastic-only posture**: ship R0 + R1 only; R2/R3 stay gated
      behind §9 (§7, §8).
- [ ] Rename customer-facing R1 copy from *"kit"* to **"enclosure set" /
      "printed parts pack"** across store and site (§8).
- [ ] Drop the bundled **PSU** from the Canary Dash configuration; ship a cable
      and the UL-listed-source line (§6).
- [ ] Keep the **battery option** a documented self-build choice, never shipped
      (§6).
- [ ] **Do not list Tin Can / kids-watch enclosures** until CPSIA testing exists
      (§8).
- [ ] Add the **e-label regulatory screen** (`Settings → About → Regulatory`) to
      the display-device firmware backlog — needed the day an R3 ships, cheap to
      build now (§4).
- [ ] Surface the self-test's per-unit identity as a **physical serial** on any
      assembled unit (§4).
- [ ] Before any R3 or Crowd Supply campaign: book the **Part 15B SDoC test**
      and have counsel review store terms alongside `/TERMS.md` (§9).

## 11. What this deliberately does not claim

It is not legal advice, is not a substitute for a test lab or an attorney, and
does not certify anything. Every rule citation is checkable at the linked source
and should be checked before money moves. The FCC positions here are the
mainstream reading of the cited sections; the *application* to a specific SKU is
exactly the judgement a lab and a lawyer are paid for.

## 12. Sources

- [47 CFR 15.212 — Modular transmitters](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-C/section-15.212)
- [47 CFR 15.23 — Home-built devices](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-A/section-15.23)
- [47 CFR 15.19 — Labeling requirements](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-A/section-15.19)
- [47 CFR 15.105 — Information to the user](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-B/section-15.105)
- [47 CFR 2.803 — Marketing of RF devices](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-2/subpart-I/section-2.803)
- [FCC KDB 996369 D04 — Module Integration Guide](https://apps.fcc.gov/eas/comments/GetPublishedDocument.html?id=451&tn=825049)
- [FCC — Equipment Authorization, RF Device](https://www.fcc.gov/oet/ea/rfdevice)
- [F2 Labs — FCC Part 15 testing & SDoC](https://f2labs.com/fcc-part-15)
- [FCC label requirements overview](https://markready.io/learn/fcc-label-requirements)
- [E-LABEL Act](https://en.wikipedia.org/wiki/E-LABEL_Act)
- [DOE external power supply efficiency (Level VI)](https://www.belfuse.com/resource-library/tech-paper/efficiency-standards-for-external-power-supplies)
- [EMC FastPass — FCC rules for kits and subassemblies](https://emcfastpass.com/fcc-rules-kits-subassemblies/)
- [ARRL — Part 15 radio frequency devices](http://www.arrl.org/part-15-radio-frequency-devices)
- [UL 94 flammability ratings](https://en.wikipedia.org/wiki/UL_94)
- Prior art for the posture: **StuckAtPrototype** (Leander, TX) sells assembled,
  hand-built, open-source ESP32 devices — USB-powered, no battery, no bundled
  adapter, no camera or mic — and states FCC is *"done and passed for North
  America"* on The Clock. Their own marketing claim; no grantee record was
  independently located. The instructive part is the *shape*: engineer the risk
  categories out, then pay for one test per design.
