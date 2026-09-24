# Canary Floodlight — motion-lit witness (market research & design dossier)

**Status:** concept — sourced market research and a design, **no firmware, no bench unit, no
enclosure yet**. Outdoor, hardwired-or-PoE, year-round. Marketing numbers from vendors are flagged
as such; the honest ceiling is stated plainly. *Working name only* — the device line's naming is a
product decision, not this document's.

**The one-sentence version:** the thing on the corner of the garage that Ring, Wyze, Eufy and
Reolink sell for $100–250 — a bright motion-triggered light with a camera behind it — rebuilt from
the bricks the fleet already has (Vision's on-module person detection, Sense/Sentinel's radar + PIR,
the signed claim vocabulary, the weather-preset cases), **sold as plastic and plans, never as a
radio**, and structurally incapable of the two features the market is now selling hardest: face
recognition and neighborhood-wide footage search.

*Research date: 2026-09-24. Method note: vendor product pages (ring.com, wyze.com, eufy.com,
reolink.com, axtontech.com, cnx-software.com, eff.org and several others) refused automated fetch
from the research environment, so specifications and prices below come from search excerpts of
those pages plus reviewer coverage (PCMag, PCWorld, TechRadar, SafeHome, HomeCamCafe, TechGearLab,
Mighty Gadget, VueVille). Items verified from a single origin are marked **single-source**. Prices
are US street, Sept 2026, and move weekly — treat them as a tier, not a quote.*

---

## 0 · What this decides (read this if nothing else)

1. **The market is a subscription business wearing a hardware costume.** Every major floodlight cam
   is sold near cost and monetized at $3–20/month for the "smart" half — person/package/vehicle
   detection, event history, and now face recognition. The two vendors that sell *without* a
   subscription (Eufy, Reolink) are the ones reviewers recommend, and neither is open.
2. **We already have the sensing half.** Vision (on-module person detection, boxes only over I2C),
   Sense/Sentinel (60 GHz radar + PIR + lux), and the Combo Witness case that pairs them are the
   detection stack of a floodlight cam. What's missing is a **light**, a **power path that isn't
   USB**, and one **outdoor housing** that holds all three.
3. **The light is the regulatory problem, not the radio.** The radio story is already settled by
   [doc 29](../strategy/29-fcc-and-product-compliance-diligence.md): plans + plastic, never a
   shipped radio. But a *120 V floodlight in a junction box* is a **luminaire on a branch circuit**
   (UL 1598 territory, inspector territory, insurer territory). The simple answer is to **never
   touch mains**: the light runs on **Class 2 low voltage** (≤ 30 V, ≤ 100 VA) from a bought,
   listed **cord-and-plug** supply or a PoE splitter, which is exactly the NEC's carve-out for
   low-voltage lighting and exactly the shape of the PoE floodlights the CCTV industry already
   sells (§4).
4. **The recommended first build is a "light head that a Canary drives," not a monolith.** A
   listed 24 V LED floodlight module + a listed plug-in Class 2 supply (or an 802.3at PoE splitter),
   switched by a MOSFET on the existing Sentinel-class sensor head, with the Vision stack beside it
   in the Combo case. Every part we publish is plastic or firmware, so **no certification is owed by
   us at all** in that shape, and the builder is inside 47 CFR 15.23 (§4.1). The compliance story
   for the *light* is **conditional on the builder buying listed parts on both sides of the Class 2
   line** — a listed supply *and* listed low-voltage lighting equipment (NEC 411.4). The supply is
   easy to name; a listed 24 V DC, 3000 K, 20–30 W module is **not yet identified** (§4.2, §7), and
   an "IP65" on a store page is an ingress claim, not a safety listing.
5. **What we cannot match, and shouldn't try:** cloud video history, a phone app that streams from
   anywhere with no setup, 4K color night video, and face recognition. What we can do that none of
   them can: a light that turns on **because a signed, hash-chained claim says a large object crossed
   a boundary**, with no footage to subpoena, no network to enroll in, and no monthly fee — the
   Reolink/Eufy "no subscription" pitch taken to its structural conclusion. The "structural" word
   is earned only on the tiers built on the Grove Vision AI V2 + ESP32 stack, where no host ever
   holds a frame; the reCamera-based Pro tier is a Linux camera that *can* stream and record, so
   there the guarantee is configuration policy, stated as such (§5.4).

---

## 1 · The market — what's on the shelf

### 1.1 · The shelf, side by side

Street prices are US, Sept 2026, before sales (most of these sit 20–40 % lower on any given
week). "Sub." is the cheapest plan that unlocks the vendor's smart detection and event history;
where the row says *none*, those features work without one. Lumens are vendor-stated. Every cell is
from vendor copy or a named review; **unverified** marks a cell no source stated.

| Product | Price | Light | Video / FOV | Detection | Power | Local / open | Subscription | Notable |
|---|---|---|---|---|---|---|---|---|
| **Ring Floodlight Cam Pro (2nd gen, "4K Pro")** | $260–280 | 2000 lm, dimmable 200–2000 | 4K (3840×2160), 140°×85° HDR, 10× zoom, color night | **radar** ("3D Motion Detection", Bird's Eye View) + video | hardwired 120 V j-box, 2.4/5 GHz Wi-Fi | none — cloud only, no RTSP | Ring Home Basic $4.99 · Standard $10 · Premium $19.99/mo (AI, video search, 24/7, **Familiar Faces**) | the category's flagship; person/package/vehicle alerts and all history behind the plan; Search Party on by default on cloud outdoor cams |
| **Ring Floodlight Cam Wired Pro (1st gen)** | ~$200 street | 2000 lm | 2K HDR, 140°×80° | radar + video | hardwired, Wi-Fi | none | Ring Home (as above) | last year's Pro; same radar |
| **Ring Floodlight Cam Wired Plus** | $180 | 2600 lm | 1080p, 140°×80°, color night | **PIR** | hardwired, 2.4 GHz only | none | Ring Home | the volume seller; PIR-only false alerts are the #1 review complaint |
| **Wyze Cam Floodlight Pro** | $150 | 3000 lm, three independently aimed panels | 2.5K, **180°** wide, color night | video AI (Cam Plus) + PIR (**unverified**) | hardwired, Wi-Fi | **microSD to 256 GB**, 24/7 local; no official RTSP (`docker-wyze-bridge` community) | Cam Plus $2.99/mo per cam for person/pet/vehicle/package + cloud | PCMag Editors' Choice on value; TWiT: "good concept, frustrating execution" (app/setup) |
| **Wyze Cam Floodlight v2** | $70–90 | 2800 lm | 2K, 160°, color night, 105 dB siren | PIR + video | hardwired, Wi-Fi | microSD to 256 GB, 30 days 24/7 | Cam Plus $2.99/mo for AI | the price floor of the hardwired class |
| **eufy Floodlight Camera E340** | $220 ($370 w/ HomeBase S380) | 2000 lm, adjustable | **dual lens** 3K wide + 2K tele, 360° pan/tilt, AI tracking | video AI on-device + PIR (**unverified**) | hardwired, Wi-Fi | **microSD 128 GB or HomeBase**, AES-128; no fees; RTSP via HomeBase/app (**unverified** on this model) | **none** required | PCWorld: "tops in surveillance, lighting"; the reviewer default for "no subscription" |
| **eufy Floodlight Cam S330 (2 Pro)** | $200–300 | **3000 lm** | 2K, 360° pan/tilt, on-device subject lock | video AI | hardwired, Wi-Fi | local (onboard), no fees | none | praised light + PTZ; "fussy about Wi-Fi", app hiccups |
| **eufy Floodlight Camera E30** | $150 | 2000 lm | 2K, 360° pan / 70° tilt, siren | video AI | hardwired, **2.4 GHz only** | local 24/7, no fees | none | the cheap eufy |
| **Reolink Elite Floodlight WiFi** | ~$200–250 (**unverified**) | **3000 lm adjustable** | 4K 8 MP, **180°** dual-lens | video AI on-device, local video search | hardwired 100–240 V, Wi-Fi 6 dual-band | **RTSP + ONVIF native**, microSD, NVR; official Home Assistant integration | none | the open-protocol leader; closed firmware |
| **Reolink Duo Floodlight (WiFi / PoE)** | ~$180–230 | 2 × 15 W, 1800 lm, 4200 K | 4K dual-lens 180° | video AI | Wi-Fi or **PoE** | RTSP/ONVIF, NVR | none | the only mainstream **PoE floodlight cam**; VueVille: strong image, light "adequate" |
| **Reolink TrackFlex Floodlight WiFi** | ~€230 (US **unverified**) | 3000 lm | 4K dual, PTZ 360°, 6× hybrid zoom | video AI, auto-track | hardwired, Wi-Fi | RTSP/ONVIF, HA integration | none | HA community favorite |
| **TP-Link Tapo C720** | $100–120 | 2800 lm, 270° motion-light coverage | 2K QHD 2560×1440 @15 fps, 150°, color night, 93 dB siren | PIR + video (person/vehicle/pet on-device, **unverified**) | hardwired, Wi-Fi | **microSD 512 GB, RTSP + ONVIF**, local HA since fw 1.4.4 | none for local; Tapo Care for cloud | the cheapest open-protocol option; IP65 |
| **Arlo Pro 3 Floodlight** | $250 (often $170) | 2000 lm on battery, **3000 lm** on continuous power | 2K HDR, 160° | PIR + cloud AI | **battery** (≈6 mo) or wired/solar | none — cloud | Arlo Secure Plus $7.99/mo per cam, $17.99 unlimited; Premium $24.99–29.99 | the battery-install answer; light dims on battery |
| **Google Nest Cam with Floodlight** | $280 | 2400 lm | 1080p HDR @30 fps, 130° | on-device person/animal/vehicle; **Familiar Faces** with plan | hardwired, Wi-Fi | none — cloud (3 h event history free) | Nest Aware $10/mo, Plus $20/mo (24/7) | AndroidPolice: "overpriced and underpowered"; hiked plan prices 2025 |
| **Blink Outdoor 4 Floodlight** | $120–130 (often $77–99) | **700 lm** | 1080p, IR night | PIR | **battery** — AA lithium in cam, 4× D in light, ≈2 yr | none — cloud or Sync Module local | Blink Basic $3/mo, Plus $10/mo | the budget floor; AndroidCentral: "good but not great unless you pay" |
| **Lorex 2K Floodlight (Wi-Fi, panning)** | $110 (wired model MSRP $250) | 1500 lm | 2K, app-controlled pan | video AI | hardwired, Wi-Fi | microSD 32 GB incl., no sub | none | PCWorld: "high-res, no sub" |
| **Amcrest SmartHome ASH26-W** | ~$70–90 (**unverified**) | 2000 lm | 1080p @30, 114° | PIR + video | hardwired, Wi-Fi | microSD, Amcrest cloud optional | none for local | Amcrest's real value is its **PoE bullet cams + ONVIF** (Frigate's favorite), not this |

**Read across the columns and three things stand out.**

1. **The light is a solved commodity: 2000–3000 lm, 15–30 W.** Only Blink (700 lm, battery) and
   Lorex (1500 lm) sit below. Anything we build needs ~2000 lm to be taken seriously, and §4.3 shows
   an 802.3at PoE run reaches the low end of that and a 36–40 W Class 2 plug-in supply the top.
2. **Detection has bifurcated.** The cheap tier is PIR (false alerts); the premium tier is **radar +
   video** (Ring Pro) or **on-device video AI with tracking** (eufy, Reolink). Radar-corroborated
   video is exactly the Combo Witness / Sentinel fusion this repo already has.
3. **"No subscription, local storage, RTSP/ONVIF" went from niche to a purchase criterion** in two
   product cycles. eufy, Reolink and Tapo now compete on it; Ring, Nest, Arlo and Blink still
   gate person/package/vehicle detection and all history behind $3–20/month, and the two biggest
   (Ring, Nest) have added **face recognition** as the premium hook.

### 1.2 · Per-vendor notes (what the spec sheet doesn't say)

- **Ring.** The Pro's radar is real and reviewers credit it with the fewest false alerts in the
  category; the app's motion zones are drawn on a bird's-eye map. But everything past live view is
  a plan: Basic covers one camera, Standard the home, Premium adds smart video search, 24/7 and
  **Familiar Faces** (launched US Dec 2025, off by default, unavailable in IL, TX, Portland OR and
  Quebec because of biometric law). **Search Party for Dogs** enrolls cloud-connected outdoor cams
  by default. The **Flock Safety** integration into Community Requests was announced, aired as a
  Super Bowl ad, and canceled 2026-02-12 after the backlash; Ring says no video was ever sent.
  Senator Markey's office has an open correspondence on Ring's facial-recognition rollout (Nov 2025).
- **Wyze.** Best light-per-dollar (3000 lm, three panels, 180°). microSD 24/7 is genuinely free;
  AI classes are $2.99/mo per camera. RTSP: Wyze shipped official RTSP firmware only for v1/v2-era
  cams and has not for the floodlights; `docker-wyze-bridge` supports both floodlights but forced
  firmware pushes have broken it before. The 2022 disclosure history (a known vulnerability sat
  unpatched for years) still shapes how privacy reviewers write about the brand.
- **eufy (Anker).** The E340's dual-lens + PTZ + tracking at $220 with no fee is why it wins
  roundups. Local by default, AES-128, HomeBase S380 adds NAS-style storage and cross-camera AI.
  The brand carries its own 2022–23 history: cloud uploads and an unauthenticated stream that
  Anker first denied, then acknowledged (see the repo's
  [competitor app landscape](../research/competitor_app_landscape.md)).
- **Reolink.** The one vendor whose *whole line* speaks RTSP/ONVIF, sells PoE floodlights, and ships
  an official Home Assistant integration; "no fees" is the headline. Firmware is closed and the
  cloud is opt-in. VueVille's PoE Duo review: excellent image, the 1800 lm light "adequate rather
  than dazzling."
- **TP-Link Tapo.** Quietly the cheapest way to an open-protocol floodlight cam: RTSP + ONVIF,
  512 GB microSD, local HA. 15 fps at 2K and a 2.4 GHz-only radio are the trade.
- **Arlo / Nest / Blink.** Three cloud-first designs at three price points. Arlo is the battery
  install (light drops to 2000 lm on battery); Nest is 1080p at $280 with face recognition behind
  Nest Aware; Blink is 700 lm on D cells. None exposes a local stream.

### 1.3 · How big is this

Analyst numbers are vendor-marketed and **single-source**, so treat them as order-of-magnitude:
the floodlight-camera segment is quoted at ~$3.2 B globally in 2025 with North America ~38 % and a
~14 % CAGR; the broader home-camera market is ~42–48 M units/yr, and SafeHome's 2026 survey counts
~75 M US households with at least one camera, outdoor more common than indoor. Residential DIY is
the largest channel. The point for this dossier is not the size but the shape: a large installed
base, sold through Amazon and the big-box aisle, on a hardware-plus-plan model that a growing
minority of buyers now actively shops *against*.


---

## 2 · What buyers actually complain about (the openings)

The review corpus is remarkably consistent. Ranked by how often it comes up, and mapped to what
our design does about it:

| Complaint | Who it bites | What we do |
|---|---|---|
| **"It's $150 and then $10/month forever."** AI detection, history and rich notifications gated behind Ring Home / Nest Aware / Arlo Secure / Cam Plus | Ring, Nest, Arlo, Blink, Wyze | No cloud exists to subscribe to. Person/vehicle/animal/package classification is on-device (Vision) and the event log is the hub's, forever, for free. |
| **"I don't own my own footage."** Cloud-only recording; Ring and Nest expose no RTSP; Wyze dropped official RTSP after v2 and the community keeps `docker-wyze-bridge` alive against forced firmware updates | Ring, Nest, Arlo, Wyze | There is no footage to own — and that is the honest trade (§6). The signed claim log is local, exportable, verifiable with a pinned key. |
| **"My neighbor's camera is now a police network."** Ring's Search Party (on by default on cloud-connected outdoor cams), Familiar Faces (Dec 2025, Ring Home Premium, blocked by law in IL/TX/Portland/Quebec), the Flock Safety integration canceled 2026-02-12 after the Super Bowl ad backlash | Ring above all; every cloud vendor by proximity | Invariant II: no face, no plate, no re-ID — not a toggle, a *can't*. No vendor cloud means no vendor can enroll the device in anything. |
| **False alerts** — headlights, branches, spiders on the lens, rain at night | every PIR/pixel-motion product; radar (Ring Pro's 3D Motion) is the industry's own fix | Radar-confirmed camera events (the Combo Witness idea) plus Sentinel's cross-modality corroboration. A claim needs two physically independent channels to agree (§5). |
| **Wi-Fi at the corner of the garage is bad** | every Wi-Fi model | The PoE tier: one cable, power + data, no radio at all on the light head (§4.3). |
| **Install is scary** — "requires an existing junction box and weatherproof cover," "hire an electrician," and the Ring Pro's manual literally says turn off the breaker | all hardwired models | Low-voltage or PoE by design: the builder plugs a listed driver into an outlet or a PoE switch. No breaker, no wire nuts, no inspector. |
| **Too bright / light trespass / neighbors** | 2000–3000 lm at 5000–6500 K aimed sideways | 3000 K, fully-shielded hood, aimed down — the DarkSky-friendly recipe (§4.4). Brightness is a knob, not a spec-sheet race. |
| **Cold-weather and heat failures**, especially battery models | Ring battery, Arlo, Blink | Mains/PoE only; no lithium in the design (the [cold-weather envelope](./cold_weather_envelope.md) and doc 29 §6 both say so). |

The privacy complaints are the ones with momentum. The Search Party / Flock Safety episode put
"a camera company enrolled my porch in a dragnet" on the evening news, and the Eufy/Reolink
"no subscription, local storage" pitch has gone from niche to reviewer default in two years. The gap
this concept fills is the next step on that ladder: **not just local, but structurally unable**.

---

## 3 · The DIY and open landscape (what already exists, and the gap)

| What people do today | What it gets them | What it lacks |
|---|---|---|
| **Reolink / Amcrest PoE camera + Frigate NVR + Home Assistant** turning a smart floodlight on when Frigate sees a person | Local AI, no subscription, ONVIF/RTSP, mature | Two products and a server; the camera is still a footage machine; Frigate's detections are unsigned |
| **Reolink Floodlight (Wi-Fi/PoE) as-is with the HA integration** | The closest "no-cloud" retail answer; RTSP/ONVIF native | Closed firmware; still records video; the vendor can change the terms |
| **Wyze Floodlight + `docker-wyze-bridge`** | Cheap; a community RTSP shim | Fragile — forced firmware pushes have broken it |
| **Existing dumb floodlight + Shelly / Zigbee relay + any HA motion source** | No new luminaire, no code | Mains work inside the fixture box; no camera; nothing signed |
| **ESP32-CAM / XIAO ESP32-S3 Sense in a printed box** | $10–15, ESPHome or our WAP firmware | 2 MP OV2640, poor low light, no light, no radar, splash-rated at best |
| **Prokyber ESP32-Stick-PoE-A-Cam** ($27, OSHW ESP32-S3 + W5500 + Si3404 802.3af, OV2640/OV5640) | The one open PoE camera board on the market | 802.3af budget (~13 W) is enough for the camera and *maybe* a 5–8 W light, not a 25 W floodlight |
| **Seeed reCamera 2002/2002w** ($35–55, SG2002, 1 TOPS, YOLOv11, RTSP + Node-RED; HQ PoE and Gimbal variants exist) | Our [Vision Lite](./canary_vision_lite_recamera.md) tier, already designed as a config-only witness | No light, no outdoor housing, 5 MP OV5647 daylight sensor |
| **Axton Blaze PoE floodlights** (25 W → 3300 lm at 6000 K on 802.3at/bt; 3–11 W models on 802.3af; IP67, US-made) | Proof that a **PoE floodlight is a real, listed, low-voltage product category** | Illuminator only — it is what our light head should look like, not a camera |

**The gap is exact:** nobody ships an *open* floodlight camera, and nobody at all ships one whose
output is a signed semantic claim rather than a video clip. The closest open pieces (reCamera, the
PoE-A-Cam, Frigate) are each one leg of the stool.

---

## 4 · Power, light and the certification question — the part that decides the shape

### 4.1 · What the radio side already settles

[Doc 29](../strategy/29-fcc-and-product-compliance-diligence.md) is binding here and this dossier
adds nothing to it: a pre-certified module carries the intentional radiator; the *assembled
product* owes a Part 15 Subpart B SDoC (~$1.5–4k per SKU; test labs quote as low as ~$800 for a
simple product, **single-source**); selling a kit does *not* escape that; publishing plans and
selling **printed enclosures only** owes nothing. The builder who assembles five or fewer for
personal use is inside [47 CFR 15.23](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-A/section-15.23).
Two board facts worth carrying forward from
[`fcc_board_status.md`](./fcc_board_status.md): the XIAO ESP32-S3 and ESP32-C3 hold FCC IDs
(Z4T-XIAOESP32S3, Z4T-XIAOESP32C3) whose grant conditions are still unread; and a wall-mounted
unit is comfortably past the 20 cm mobile-exposure distance a modular grant typically assumes,
which a doorbell or worn device is not. A floodlight on the eave is the *easy* RF-exposure case.

**The ESP32-P4 detail.** If a higher-resolution camera tier ever wants Espressif's P4 (MIPI-CSI,
H.264, an OV5647), note the P4 has **no radio**; Wi-Fi rides a separate ESP32-C6 module with its
own modular grant. That is a Part 15B-only host plus a certified module — the same posture as
today, not a new one.

### 4.2 · The light is the new problem: luminaire vs. low-voltage lighting

A floodlight is not a USB gadget. The relevant rules, in plain terms:

| If the light is… | It is governed by… | What that means for a small open project |
|---|---|---|
| **Wired to a 120 V branch circuit** (junction box, like every Ring/Wyze/Eufy/Reolink hardwired model) | NEC Article 410 + **UL 1598 (Luminaires)**, with the LED array under **UL 8750**; a permanent installation an electrical inspector and a homeowner's insurer can ask to see a listing for | A listing program per fixture — five figures and a lab relationship. **Out of reach, and out of character.** An unlisted mains luminaire is also the "unlisted device caused the fire" clause insurers reach for. |
| **Cord-and-plug, 120 V** (plugs into an outdoor outlet) | Code does not require a listing for portable plug-in equipment; an inspector can still object to anything bolted to the wall; insurers increasingly expect an NRTL mark on anything with a plug | Better, but a home-built mains PSU in a printed box is the exact fact pattern doc 29 §6 says never to ship. |
| **Low voltage, ≤ 30 V and ≤ 100 VA, from a listed Class 2 source** | **NEC Article 411** (low-voltage lighting) and **Article 725** (Class 2 circuits). 411.4 asks that a system at 30 V or less be *listed as a complete system*, **or** that the Class 2 power source and the lighting equipment connected to it be listed. UL 1310 covers the Class 2 supply. | **This is the carve-out.** Class 2 wiring is treated as inherently safe from fire and shock: reduced wiring rules, no conduit, no electrician. A bought, listed **cord-and-plug** Class 2 supply (a 24 V Class 2 desktop adapter of the Mean Well GST40A24 / GSM40B24 class, or a listed plug-in low-voltage landscape-lighting transformer — the very product NEC 411 was written for) or a bought PoE splitter is the listed source. **The lighting equipment must be listed too**, and that is the open condition: the $15–30 "24 V DC LED flood" class on the marketplaces is mostly *un*listed (an IP65 mark is not an NRTL mark), so the honest statement is "compliant *when* built from a listed supply and a listed low-voltage luminaire," with the luminaire still to be named (§7). **Our part is the printed bracket, the MOSFET switch and the firmware.** Not a Mean Well LPV-35-24: that series has flying mains-input leads and needs a box and a qualified termination, which is exactly the mains work this design exists to avoid. |
| **PoE** (802.3af 13 W / 802.3at 25.5 W / 802.3bt 51–71 W at the powered device) | Same Class 2 / low-voltage territory; PoE is a limited-power source by construction. The Tycon POE-SPLT-4824G-P (at in, 24 V out) is UL-listed as ITE. | One cable does power and data; the light head never sees mains; the switch or injector is the listed source. |

**The design rule that falls out:** *the project never touches mains.* Every path from the wall to
the LED runs through a **bought, listed, cord-and-plug or PoE** device — nothing with bare mains
leads — and everything we publish sits on the Class 2 side of it. That deletes UL 1598, deletes the inspector, deletes the
"who made the power supply" liability, and (as doc 29 §6 already found for the Dash) makes the SKU
cheaper as well as cleaner.

### 4.3 · How much light does low voltage buy?

Enough. The commercial floodlight cams run 1800–3000 lm total from two or three LED panels; that is
15–25 W of modern LED at ~100–130 lm/W.

| Power path | Budget at the load | Realistic light (3000 K, ~110 lm/W incl. driver loss) | Camera + sensor head | Verdict |
|---|---|---|---|---|
| **802.3af PoE** | ~13 W | ~8–10 W → **900–1100 lm** (a bright porch light, not a floodlight) | ~3 W | The "lit path light" tier; the Prokyber board's class |
| **802.3at PoE+** | 25.5 W at the PD; ~23 W after splitter loss | head 3–5 W leaves **≤ 18 W for the light → ~1700–2000 lm** | ~3–5 W | **Reaches the Ring Pro / Eufy E340 (2000 lm) class, at the low end.** Axton's 25 W PoE floodlight spends the whole at budget on the light alone (3300 lm at 6000 K, a more efficient phosphor) — a camera beside it needs its own run or 802.3bt |
| **802.3bt** | 51–71 W | 20–30 W light + head with margin → **2200–3300 lm**; 40 W+ → 4000+ lm | ~3–5 W | The tier for the full 20–30 W module on one cable; more than any consumer unit above that |
| **24 V Class 2 plug-in supply, 36–40 W** (GST40A24-class desktop adapter, or a listed plug-in landscape transformer) | 36–40 W | ~30 W → **2800–3300 lm** | ~3–5 W | **Matches Wyze Pro (3000 lm).** Plugs into an outdoor outlet under an in-use cover; the 24 V line runs to the eave as low-voltage wire |
| **12 V / 24 V from a solar controller** | whatever the panel and pack allow | a 20 W light for 2 min × 20 triggers/night ≈ 13 Wh/night — plausible on a 30 W panel + LiFePO₄ in summer, marginal in winter | — | Not the first build; the [solar sizing](./solar_power_sizing.md) and [cold-weather](./cold_weather_envelope.md) references apply unchanged |

**LED parts, honestly priced (single-source retail excerpts):** a 30 W COB emitter at 3000–3500 K
gives ~2700–2800 lm at 900 mA / 30–34 V (a constant-*current* driver, ~$5–10); GekPower's 30 W
constant-current COB module quotes 2000–2500 lm at 3000 K. A 20–30 W COB needs a real heatsink —
tens of grams of finned aluminum, kept under ~60 °C — which is why the practical recommendation is
**not** a bare COB in a printed shell but a **bought, sealed 24 V LED floodlight module** (the
$15–30 "12/24 V DC LED flood" class, IP65 by its own maker, aluminum body) on a printed bracket —
with the caveat §4.2 states: that class is mostly unlisted, and the design's NEC 411 story needs a
**listed** low-voltage luminaire, which this research has not yet named.
The heat then lives in a metal body someone else designed, and the printed parts carry no thermal
duty at all. A PETG or ASA shell next to a 30 W LED is the wrong material for the wrong job.

### 4.4 · Light quality: 3000 K, shielded, aimed down

The consumer units ship 4200–6500 K and aim sideways, which is what generates the "my neighbor's
floodlight" complaints and what dark-sky ordinances are written against. The recipe the lighting
industry itself recommends for motion-activated security light: **≤ 3000 K, fully shielded / full
cutoff (no light above horizontal), aimed at the ground, motion-activated rather than dusk-to-dawn.**
The hood becomes a *feature* here — it is the same rain-hood idiom the Vision weather case already
has (`part="hood"`), grown to shade a light instead of a lens. Aimed-down light is also better for
the camera: it lights the subject's face-height and the ground, not the lens.

### 4.5 · Weather rating, honestly

Same rule as everywhere in the catalog ([`field_ratings.md`](./enclosure/field_ratings.md)): a
printed case is **never IP-rated**; an IEC 60529 rating is a lab result on a specific assembly, and
a self-declared "IP65" on a store page is a claim, not a test. The honest split for this device:

- the **LED module** carries its own maker's IP rating (bought part);
- the **supply / splitter** carries its own (a desktop Class 2 adapter is an indoor part and lives
  in the in-use outlet cover or indoors; a landscape transformer is outdoor-rated by its maker; PoE
  splitters are indoor parts and live in the head);
- the **sensor + camera head** is ours, and is CER-2 (sheltered, under an eave — where a floodlight
  cam lives anyway) as printed, CER-3 with the gland + vent rules, and **IP66/67 only by putting the
  electronics in a bought Hammond 1554/1555 box** on a `canary_hammond_chassis.scad` plate, the
  route the [pool dossier](../research/pool_water_monitor.md) already chose for the same reason.

### 4.6 · Selling it: the ladder, restated for a light

| SKU | What's in the box | Regulatory load |
|---|---|---|
| **R0 — plans, firmware, BOM** | nothing | none |
| **R1 — printed parts pack** (bracket, sensor-head shell, hood, gasket) | plastic | none — and no thermal or weather claims beyond "fitment" |
| R2/R3 — anything with a board or an LED driver in it | a radio and a Class 2 luminaire | Part 15B SDoC on the head **plus** 411.4's "listed lighting equipment" question on the light — a listing conversation we don't have to have if we don't ship the light |

Hold R0 + R1. Link the LED module, the driver and the PoE splitter as bought parts with their own
listings; take no board order and no light order. **Do not call it a "kit"** (doc 29 §7).

---

## 5 · The design — a light head the fleet already knows how to drive

### 5.1 · Three tiers, one brain

| Tier | Sensor head | Camera | Light | Power | ~BOM (builder-bought, excl. printed parts) | For |
|---|---|---|---|---|---|---|
| **Lantern** | Sentinel-Lite class: XIAO ESP32-C3, PIR (HC-SR501/AM312 class), BH1750 lux | none | 8–10 W 24 V module, ~1000 lm | 802.3af or a small Class 2 driver | ~$45 | a lit path or side door; the "smart porch light" — no lens at all, the strongest privacy story |
| **Floodlight** | Sentinel-Standard class: XIAO ESP32-C6 + MR60BHA2 60 GHz radar + PIR + lux | Vision stack (Grove Vision AI V2 + XIAO C3, boxes only) | 15–18 W 24 V module (~1700–2000 lm) on 802.3at; 20–30 W (2000–3000 lm) on 802.3bt or the plug-in supply | 802.3at PoE splitter (light capped at ~18 W), or 802.3bt, or a 36–40 W plug-in Class 2 supply | ~$120–140 | **the recommended build** — driveway, garage corner, backyard |
| **Floodlight Pro** | same head | [Vision Lite](./canary_vision_lite_recamera.md) (reCamera 2002, PoE baseboard) or [Vision Pro](./canary_vision_pro_recamera.md) (reCamera Pro, starlight) via the webhook adapter | same | 802.3bt or the driver + a second PoE run | ~$170 (Lite) / ~$420 (Pro) | a wide dark yard where the Grove sensor's short range and weak low light run out (the [Curbwatch](./canary_curbwatch_research.md) finding) |

The **head is the Sentinel**. The fusion engine (`firmware/common/fusion`, host-tested) already
scores PIR, radar, lux and vision as physically independent channels and treats blinding as
suspicion; the Sentinel Heavy tier already puts vision on its own MCU. This device is, mechanically,
*Sentinel Heavy in the Combo Witness case with a MOSFET and a light module bolted on*. The new
firmware surface is small: one output pin, a light policy, and a lux gate.

### 5.2 · What the light does, exactly (the policy)

- **Dusk gate:** the BH1750 says it is dark (a threshold, hysteresis, no clock dependence).
- **Trigger:** the fused score crosses the alarm threshold — **two modality classes agreeing** (radar
  + PIR, or radar + vision), never PIR alone. This is the false-alert fix the market sells as "3D
  Motion Detection" and "AI"; here it is corroboration.
- **Output:** light on, ramp over ~300 ms (a hard 0→3000 lm step at 2 a.m. is hostile, and the ramp
  is free), hold for a configurable dwell (default 90 s), re-armed by continued presence, off with a
  ramp. Brightness is a PWM knob on the MOSFET, so "porch 30 %, full on a trigger" is a policy, not a
  second light.
- **Claim:** the same signed claim the Combo case describes — a person-class `LargeObjectBoundaryCrossing`
  (or `VehiclePresenceAfterHours` on a vehicle class after hours, `SmallObjectBoundaryCrossing` for
  animal/package) — with **`light_on` as a runtime state, not a new claim kind** (Invariant VI: no new
  vocabulary). The light is an *effect* of a witnessed event; the log records the event.
- **Manual and automation:** an HA `light` entity over the existing MQTT discovery idiom so the hub, a
  Dash card or a voice satellite can turn it on; a physical override is a builder option (a switch in
  the 24 V line, no firmware involved — Class 2, no code).
- **Fail state:** the MOSFET defaults *off* and the head reboots into off. A floodlight that latches
  on when the firmware wedges is a neighbor complaint; one that latches off is a dark yard. Off is the
  safe default and the light is not life-safety (§6).

### 5.3 · Hardware, concretely

- **Switch:** a logic-level N-MOSFET (AO3400 / IRLZ44N class, or a $2 "MOSFET module") low-side on the
  24 V return, gate from a XIAO GPIO through 100 Ω, 10 k pull-down so the light is off at boot. PWM at
  ~1 kHz for dimming. Nothing above 30 V anywhere on our side.
- **24 V → 5 V for the head:** a small buck (MP1584 / LM2596 module, ~$2) or the PoE splitter's 5 V
  output; the XIAO's USB-C is not used in the field (its plug recess is sealed, per the weather
  presets).
- **PoE path:** switch → outdoor Cat 5e/6 → **Tycon POE-SPLT-4824G-P or Planet POE-162S** (802.3at in,
  24 V out, indoor part, lives in the sensor-head shell) → light + buck. On 802.3at the light is
  capped at ~18 W (§4.3); a 20–30 W module wants an 802.3bt switch or the supply path below.
- **Plug-in path:** outdoor outlet (GFCI, in-use cover) → a listed **cord-and-plug Class 2 24 V
  supply** (Mean Well GST40A24 / GSM40B24-class desktop adapter, or a listed plug-in low-voltage
  landscape transformer) → low-voltage landscape wire → light + buck. Two conductors up the wall, no
  data; the head keeps Wi-Fi (the C6 and the C3 each carry their own certified module, per §4.1).
  **Not** an LPV-series driver: its input is bare mains leads, which is a box and a qualified
  termination — the work this path exists to avoid.
- **Light:** a bought 20–30 W **24 V DC** LED floodlight module, 3000 K, aluminum body, on a printed
  **bracket** that mates to the catalog's two-T-stud wall interface (`canary_mount_lib.scad`) and
  puts the module's own yoke on a hood-shaped shade. It must be **NRTL-listed low-voltage lighting
  equipment** for the §4.2 story to hold; its maker's IP65 is a separate, ingress-only claim. The
  specific listed module is open item 7 in §7.
- **Head + camera:** `canary_combo.scad` as the starting point (Vision column + Sense column, two
  USB-C, weep, hood seat), with a third bay for the PIR's HDPE Fresnel window (a PIR needs its own
  LWIR-transmissive window; ordinary acrylic blocks it — the [Feeder](./canary_feeder_research.md)
  finding) and the BH1750 behind a diffused pinhole. Print in **ASA** (UV), light color (solar gain),
  mounted ports-down under the eave.
- **Distance from the light:** the sensor head sits *beside* the light, not on it. A 30 W LED body
  runs hot enough to blind a PIR's own thermal reference and to warm the radar's radome; 10–15 cm of
  separation and the shade between them is the difference between a witness and a self-triggering
  loop. This is also why a monolithic printed shell is the wrong idea.

### 5.4 · Claim mapping, privacy, alerts

| Event | `ClaimKind` | Note |
|---|---|---|
| person-class box + radar/PIR corroboration | `LargeObjectBoundaryCrossing` | identical to Vision + Combo today |
| vehicle class after the configured hours | `VehiclePresenceAfterHours` | the driveway case |
| animal / package class | `SmallObjectBoundaryCrossing` | the raccoon and the courier |
| sensor blinded (radome covered, PIR taped, lens dark in daylight) | the fusion engine's tamper-suspicion path | Sentinel's existing asymmetry — blinding raises the score |
| light state | **runtime telemetry only** | never sealed; not a claim |

What the **Lantern and Floodlight tiers** structurally cannot do: identify a face (no model, no
vocabulary — `ObjectClass` is `Person | Vehicle | Animal | Package | Unknown`), read a plate, record
video (the ESP32 host never sees pixels; the Grove module holds no frame past inference), or join a
neighbor's search. Those are the four features the incumbents have added since 2024, and they are the
four those tiers are *incapable* of adding — which is the product.

**The Pro tier is different, and says so.** A reCamera is a Linux camera computer with RTSP and
local storage; nothing structural stops it streaming or recording. What SecuraCV guarantees there is
narrower: the witness log ingests only a class + score through the webhook adapter (the payload
grammar has no image or free-text field, per the Vision Pro dossier §3), so nothing but a coarse claim
can be *sealed* — but whether the camera itself keeps footage is **configuration policy on the
reCamera, not an incapability**, and the build guide must say that in those words. A buyer who wants
the `can't` buys the Floodlight tier.

---

## 6 · What it is not, said plainly

- **Not a burglar deterrent with a warranty.** Motion-activated light is associated with less property
  crime in some studies and not in others; we say "a light that comes on when a witnessed event happens,"
  never "keeps your home safe" (the doc 27 banned-words table governs store copy too).
- **Not a video camera** (on the Lantern and Floodlight tiers; the Pro tier's reCamera can be one,
  by policy — §5.4). A buyer who wants to *see* who was there at 3 a.m. should buy a Reolink and run
  Frigate. This device tells you *that* a person crossed the driveway at 03:12, signed, and turned the
  light on. That is a smaller promise and a keepable one.
- **Not life-safety lighting.** It is not an egress light and must never be wired as one; off is the
  fail state.
- **Not IP-rated, not UL-listed, not FCC-authorized** as a product — because there is no product. The
  builder assembles listed parts around published plastic and firmware, inside 15.23.

---

## 7 · Open items and never-let-it-rot

1. **A bench unit.** Everything above is research and a design. The first build is: a Sentinel-Standard
   head + a $20 24 V LED module + a MOSFET module + a plug-in 24 V Class 2 adapter, on a plank, in a
   garage (a bench unit needs no listing; the field build does — item 7). Log the PIR
   self-trigger distance from the light, the radome temperature next to it, and the lux gate's
   hysteresis in a PR.
2. **Firmware:** one GPIO, a light policy, an HA `light` entity, a lux gate — a Sentinel preset, not a
   new project. Blocked on Sentinel's own Phase 1a bench checklist.
3. **CAD:** a bracket for a bought 24 V module on the two-T-stud interface, a shade/hood, and a third
   bay on the Combo case for the PIR window. Previews with every change, per the enclosure rules.
4. **Board lookups:** the MR60BHA2 is its own 60 GHz intentional radiator with no FCC ID recorded in
   [`fcc_board_status.md`](./fcc_board_status.md) yet; the XIAO grants' conditions are unread. Neither
   blocks the plastic-only posture; both block any future R3.
5. **A concept card in the Lab** needs a fleet figure first (every `registry.json` concept has one, by
   the [FLEET_FIGURES](../design/FLEET_FIGURES.md) rule), which needs the bracket CAD. This dossier is
   the card's research link when that exists.
6. **Re-verify prices and the subscription tiers** before any copy quotes them; the vendors reprice
   quarterly and Ring's feature set changed three times in the research window.
7. **Name a listed low-voltage luminaire.** The §4.2 compliance shape needs NRTL-listed lighting
   equipment on the Class 2 side, and no 24 V DC, 3000 K, 20–30 W module with a listing has been
   identified. Candidates to check: the listed 12/24 V landscape-lighting flood fixtures (Volt, WAC
   Landscape, Kichler, Hinkley — most are 12 V and 500–1500 lm, so two may be needed), a listed PoE
   floodlight used as the light alone (Axton-class; confirm the listing, **single-source**), or a
   listed 24 V constant-voltage LED strip/module family with a UL 8750 recognition. Until one is
   named, the light side of the story is conditional and the dossier says so.

---

## 8 · Sources

**Market and reviews (search excerpts; product pages blocked from fetch):**

- SafeHome, "Ring Floodlight Camera Review and Pricing in 2026"; TechRadar, "Ring Floodlight Cam Wired
  Pro review"; HomeCamCafe, "Ring Floodlight Cam Pro Review"; ring.com "Floodlight Cam 4K Pro" and
  "AI Features" / "Familiar Faces" support pages; Reader's Digest and Fox News coverage of Familiar Faces
  (Dec 2025); Sen. Markey's office, Amazon response on Ring facial recognition (Nov 2025)
- Gizmodo, CNBC, CBS News, Variety, Fortune (Feb 12–16, 2026) on Ring canceling the Flock Safety
  integration after the Super Bowl "Search Party" ad; EFF, "No One, Including Our Furry Friends, Will Be
  Safer in Ring's Surveillance Nightmare" (Feb 2026)
- wyze.com Cam Floodlight Pro / Cam Floodlight v2 pages and support ("Does Wyze Cam Floodlight v2 have
  local storage?"); HomeCamCafe and TWiT reviews of the Floodlight Pro; Amazon listings ($149.98 Pro;
  Cam Plus $2.99/mo per camera); Wyze forum "RTSP firmware for Wyze Cam V4"; `mrlt8/docker-wyze-bridge`
- PCWorld, TechGearLab, Cybernews, HomeCamCafe, Mighty Gadget reviews of the eufy Floodlight Camera E340
  ($219.99; $370 with HomeBase S380; 2000 lm; microSD 128 GB)
- reolink.com Elite Floodlight WiFi / Duo Floodlight WiFi / TrackFlex pages, Amazon listing of the Elite
  Floodlight WiFi (3000 lm, 100–240 V hardwired, no fees), VueVille review of the Duo Floodlight PoE,
  raspberry.tips on TrackFlex + Home Assistant, Reolink's own Home Assistant guide
- SafeHome / Security.org / Digital Camera World on the Ring Floodlight Cam Wired Plus ($179.99,
  2600 lm, 140°×80°, PIR); ring.com and Best Buy / Home Depot listings for the Floodlight Cam Pro
  (2nd gen) ($259.99–279.99, 4K, 140°×85°, 200–2000 lm); ring.com "explore Ring Home" and Security.org
  on the Ring Home Basic / Standard / Premium tiers; TechCrunch (2024-10-09) on the plan revamp
- Android Police, Digital Camera World and Modern Castle reviews of the Arlo Pro 3 Floodlight;
  Security.org, HomeCamCafe and SafeWise on Arlo Secure pricing (2026)
- Tech Advisor, Android Authority, Android Police and Reviewed on the Google Nest Cam with Floodlight
  ($279, 2400 lm, 1080p); PCWorld and HomeCamCafe on the Nest Aware price increase
- Android Central, Reviewed, Tom's Guide and GearBrain on the Blink Outdoor 4 Floodlight (700 lm,
  D-cell light); Amazon listing for the plan prices
- TP-Link / Tapo store and B&H spec pages for the Tapo C720 (2K @15 fps, 2800 lm, 150°, microSD
  512 GB, RTSP + ONVIF, IP65); tapoappforpc.com review
- TechHive, Mighty Gadget and Amazon listings for the eufy S330 (3000 lm, 360° PTZ); Best Buy, Home
  Depot and Notebookcheck on the eufy E30 ($149.99, 2000 lm)
- PCWorld and GearBrain on the Lorex 2K Floodlight; Amcrest product pages for the ASH26-W
- Market sizing (analyst-report excerpts, all **single-source**, all vendor-marketed): Verified Market
  Research / Growth Market Reports (~$3.2 B global 2025, ~14 % CAGR, North America ~38 %); SafeHome 2026
  Home Security Market Report (74.9 M US households with cameras); IndexBox / Grand View US outlook
  (42–48 M units/yr)

**DIY and open hardware:**

- CNX Software (2025-06-25) and Tindie on the Prokyber ESP32-Stick-PoE-A-Cam ($27, 802.3af, W5500 +
  Si3404); Olimex ESP32-POE / ESP32-POE2 OSHW pages
- Seeed `OSHW-reCamera-Series` (GitHub), reCamera 2002/2002w product pages ($34.90–54.90), reCamera
  HQ PoE and Gimbal wiki pages; Hackster on the SG2002
- Espressif Developer Portal, "Introducing ESP32-P4-EYE" (2025-05); `r4d10n/esp32p4-uvc-video`
  (OV5647 H.264 RTSP on the P4); Olimex OV5647 lens variants for the P4 (2026-05)
- Frigate + Home Assistant guides (HomeShift, SmartWired, Home Automation Workshop, 2026)

**Power, light and rules:**

- 47 CFR 15.23, 15.212, 2.803 (eCFR); JJRLAB and Sunfire Testing cost guides for Part 15B; Compliance
  Testing, "FCC Testing & Certification for Espressif (ESP32) Devices"; Espressif certificates page
- NEC Article 411 / 411.4 (up.codes, IAEI Magazine, Electrical Contractor Magazine "Listed Systems or
  Individual Field-assembled Components?"); Article 725 Class 2 (EC&M); Super Bright LEDs, "What is Class
  2 — UL, NEC, Power Supplies"; Mike Holt forum threads on listing vs. plug-in equipment and insurers
- UL 1598 5th-edition brochure (ul.com); Dialight ProSite UL 1598/1598A floodlight manuals
- Omnitron, "What is IEEE 802.3bt"; Axton "About PoE Powered LED Floodlights" and the Blaze 25WE (25 W,
  3300 lm, 802.3at/bt) and 11WE (802.3af) pages; Tycon POE-SPLT-4824G-P and Planet POE-162S / IPOE-162S
  splitter listings; QuinLED, "Can You Run LEDs Over PoE?"
- Mean Well LPV-35-24 datasheet excerpts (TRC Electronics, Bravo Electro, GekPower — the series is
  cited only to say why it is *not* the plug-in supply: flying mains leads); Mean Well GST / GSM
  desktop-adapter families as the Class 2 cord-and-plug class (**model-level listing unverified from
  the research environment**); CHANZON 30 W COB
  and GekPower 30 W constant-current COB listings
- IEC 60529 / IP-rating explainers (instacertify, indEx Enclosures, standardclarity) on self-declared vs.
  lab-tested ratings
- DarkSky-compliance guides (1000Bulbs, LED Lighting Supply, emergencylights.net) on ≤ 3000 K, full
  cutoff, motion-activation

**Repo-internal (the bricks this reuses):**

- [`docs/strategy/29-fcc-and-product-compliance-diligence.md`](../strategy/29-fcc-and-product-compliance-diligence.md),
  [`fcc_board_status.md`](./fcc_board_status.md), [`docs/FAQ.md`](../FAQ.md) ("Is it certified?")
- [`firmware/projects/canary-sentinel/README.md`](../../firmware/projects/canary-sentinel/README.md),
  [`firmware/projects/canary-vision/README.md`](../../firmware/projects/canary-vision/README.md),
  [`enclosure/canary_combo.scad`](./enclosure/canary_combo.scad),
  [`enclosure/field_ratings.md`](./enclosure/field_ratings.md),
  [`cold_weather_envelope.md`](./cold_weather_envelope.md), [`solar_power_sizing.md`](./solar_power_sizing.md),
  [`canary_vision_lite_recamera.md`](./canary_vision_lite_recamera.md),
  [`canary_vision_pro_recamera.md`](./canary_vision_pro_recamera.md),
  [`../research/pool_water_monitor.md`](../research/pool_water_monitor.md) §5
