# Solar & battery sizing for outdoor Canaries (reference)

**Status:** reference / design note — the canonical "how big a panel and battery?" answer for every
solar Canary (Fence Guard, Ranger, Feeder, Clime, Curbwatch, a solar Gatekeeper). Written once here so
the device dossiers cite it. Every number is tagged **[HARD]** (datasheet / NREL / physical constant)
or **[ROT]** (rule‑of‑thumb that varies by site) — replace ROTs with *measured* data for a real build.

**The thesis you should leave with:** *"bigger is better" is the amateur answer.* The skill is finding
the **smallest *balanced* pair** — panel and battery matched to each other and to the measured load —
that survives **the worst month you must operate through**, with a sane autonomy margin. Bigger is
correct *only* when it moves you toward balance (fixing a genuine winter shortfall), never as a reflex.

---

## 1 · The one principle: energy balance

> **Energy consumed per day ≤ energy harvested per day**, evaluated over the **worst month**, with margin.

Hold that every day of the worst month and the battery ends each week where it started — the node runs
forever. Fail it and the battery walks down its state‑of‑charge a little daily until it browns out: the
classic *"worked all summer, died in November."*

**Compute daily consumption from the duty cycle** (sum‑of‑modes — rigorous):
`Daily_Wh = V_batt × Σ(I_mode × t_mode_per_day)`. Two regimes:

- **Sleep dominates low‑duty nodes.** A XIAO ESP32‑C3 deep‑sleeps ~43 µA, an ESP32‑S3 ~14 µA *on the
  module* [HARD‑ish] — real boards are often 2–10× worse once you add the LDO, USB‑serial chip, and
  pull‑ups. If the radio fires only briefly, 24 h of sleep is often 30–70% of the budget, so **chasing
  sleep µA is the highest‑leverage optimization.**
- **Active bursts dominate cameras / high‑rate radios.** A Wi‑Fi TX burst is ~150–250 mA; a camera
  capture+upload is a multi‑hundred‑mA event lasting seconds. Here sleep is noise; optimize *duty cycle*
  (captures/day) and *bytes/upload*.

**Golden rule: measure, don't trust "typical."** Put a coulomb counter (Nordic PPK II, INA219/226)
inline for 24 h of the real duty cycle before you size anything.

---

## 2 · Peak Sun Hours — the concept that makes sizing tractable

**PSH** = a day's solar energy (kWh/m²/day) expressed as equivalent hours at **1000 W/m² ("1 sun",
STC)** — so **PSH is numerically the daily kWh/m²**, and since a panel's watt rating is measured at
1000 W/m², `Panel_Wp × PSH` gives daily Wh directly (the units cancel).

**The design‑driving fact is the winter/summer swing, and it gets brutal with latitude** (optimally‑
tilted, from NREL NSRDB / PVWatts — *always pull your own coordinates*):

| Site (~lat) | Dec PSH | Jun–Jul PSH | Winter/summer |
|---|---|---|---|
| Phoenix AZ (~33°) | ~4.5 | ~7.5 | ~0.6 |
| Atlanta GA (~34°) | ~3.2 | ~6 | ~0.53 |
| Minneapolis MN (~45°) | **~2.3** | ~6.5 | ~0.35 |
| Seattle WA (~48°) | **~1.0–1.4** | ~6 | ~0.2 |
| Fairbanks AK (~65°) | **~0.2–0.4** | ~5.5 | ~0.05 (Dec ≈ unusable) |

Note **cloud climate matters as much as latitude** — Seattle (48°) is worse in December than
Minneapolis (45°) because of marine cloud, not sun angle. **The rule: size to the worst month you must
operate through, not the annual average** (a node sized on the ~4 PSH annual mean at 45°N is ~2×
undersized in December). Pull real monthly numbers from **[NREL PVWatts](https://pvwatts.nrel.gov/pvwatts.php)**
(the monthly "Solar Radiation kWh/m²/day" column = monthly PSH) or **Global Solar Atlas**.

**Tilt:** year‑round ≈ latitude; **winter‑optimized (the right default for worst‑month sizing) ≈
latitude + 10–15°** [ROT] — steeper catches the low winter sun *and* sheds snow (a flat panel under
1 cm of snow is ~0 W, a real >40°N failure mode). A near‑vertical panel is legitimate for extreme‑north
winter‑only operation: near‑optimal for a low sun and snow‑immune.

---

## 3 · Panel sizing (and the honest derate)

$$\text{Panel}_{Wp} \ge \frac{\text{Daily\_Wh}}{\text{PSH}_{worst} \times \text{derate}}$$

**Derate is where honesty lives** — bench watts are never delivered watts. It's the *product* (not sum)
of independent losses: charge‑path/controller efficiency (×0.75–0.95; MPPT ~0.9, PWM/diode much worse
in low light), panel temperature (cold *helps*), soiling, non‑ideal angle, cloud/diffuse haircut,
battery charge‑acceptance taper, and wiring/diode drop. NREL PVWatts uses a **14% default loss (derate
≈ 0.86) for well‑engineered grid arrays** [HARD]; **small maker off‑grid systems are worse — use
0.5–0.7, default 0.6** [ROT], because they lack MPPT, run cheap diodes, sit at bad angles, and have no
thermal mass.

---

## 4 · Battery sizing

$$\text{Battery}_{Wh} \ge \frac{\text{Daily\_Wh} \times \text{Days\_of\_autonomy}}{\text{usable\_DoD}}$$

**Days of autonomy** = consecutive sunless days the battery alone must carry (storm, snow‑covered
panel): sunny/SW‑US **2–3**, temperate **3–5**, heavy winter overcast **5–7+** [ROT]. Beyond ~5–7 days a
backup source beats an ever‑larger battery. **Usable DoD:** Li‑ion/LiPo **~80%**; LiFePO4 **80% routine,
~90% occasional** [HARD‑ish].

**Wh ↔ mAh (teach this):** `Wh = Ah × V_nominal`. A **3.7 V × 3000 mAh cell = 11.1 Wh**, ~8.9 Wh usable
at 80% DoD. (A 3.2 V LiFePO4 3000 mAh = 9.6 Wh — lower V, but deeper usable DoD compensates.)

**Cold overrides the formula:** Li chemistries must **not be *charged* below 0 °C** (lithium plating,
permanent loss) — LiFePO4 is *especially* strict. *Discharge* below 0 °C is fine. So a winter charger
needs a **low‑temp charge cutoff (NTC)**, or accept the panel is decorative all winter and the node
coasts on stored charge. Full detail: [cold‑weather envelope](./cold_weather_envelope.md).

---

## 5 · Why "bigger is better" is wrong — the optimization

**The mental model: the panel is a faucet, the battery a bucket, the load a drain.** You want the faucet
to refill the bucket faster than the drain empties it *in the worst month* — and no more. Everything
past that is paying for water that overflows.

**The balance principle — panel and battery are a matched pair:**
- **A bigger panel needs a bigger battery to absorb it.** On a clear day a big panel fills a small
  battery by mid‑morning; the charger tapers and the rest of the day's sun is *refused* (charge
  acceptance → 0). You paid for watts you can't store.
- **A bigger battery needs a bigger panel to ever refill it.** A huge battery on a small winter panel
  never returns to 100% — it floats at chronic partial state‑of‑charge, and the "extra" reserve is
  unreachable.
- **Unbalanced either way = wasted money/weight, or chronic undercharge.**

**Going bigger on the PANEL — buys:** faster storm recovery, margin against soiling/angle, full recharge
in a short winter day. **Costs:** diminishing returns once it refills the battery within the worst‑month
daily window (extra Wp just overflows); overcharge/heat stress on a small battery or under‑rated
charger; **wind load — a big panel on a fencepost is a *sail*** (force scales with area; a real mount/
aiming failure risk, not a rounding error); cost, size, and a bigger theft/vandalism target.

**Going bigger on the BATTERY — buys:** more autonomy, storm/snow ride‑through, gentler per‑cycle DoD
*if it actually gets refilled*. **Costs:** never‑completing winter recharge → chronic partial SoC (this
is *benign* for Li‑ion — the aging villains are heat and storage at high SoC, not PSoC; it's *damaging
via sulfation only for lead‑acid*, a place generic guides get sloppy — but either way the reserve is
unreachable); cost/weight/volume/embodied energy; and **in sub‑0 °C sites a battery you can't recharge
is dead weight** — the point where, for a µA‑class node, a **multi‑year primary cell beats any solar +
rechargeable system** (no controller, no cold‑charge lockout, no PSoC, 5–10+ yr life; see the
[Gatekeeper](./canary_gatekeeper_research.md) finding and [cold envelope](./cold_weather_envelope.md)).

**The honest "right size":** balanced to worst‑month insolation + measured load + a sane autonomy
margin — then one sentence on what the next size up does and doesn't get you.

---

## 6 · Charge controller — where cheap builds lose half their winter harvest

- **Bare blocking diode:** battery voltage *is* the operating point (never the panel's max‑power
  voltage); a Schottky eats ~0.3 V. Only for a panel whose Vmp comfortably exceeds full‑charge battery V
  in bright sun. Terrible in low light.
- **PWM:** clamps panel to battery voltage; fine when panel Vmp ≈ battery V, wastes the rest.
- **MPPT:** a DC‑DC converter holding the panel at its max‑power point. **MPPT matters *more* in low
  winter light** — at low irradiance the wasted fraction grows, exactly when every Wh counts. Not a
  luxury for a winter‑limited node.

**Maker ICs/boards:**
- **CN3791 — MPPT, the right default for 1S LiPo solar** (set the MPPT divider to your panel's Vmp or it
  won't track; CV 4.2 V ±1%). On MakerFocus/Soldered MPPT boards.
- **CN3065 — simple linear solar Li‑ion charger** (~500 mA, not MPPT) — fine for a small panel closely
  matched to the cell.
- **TP4056 — NOT a solar charger.** Fixed input, no MPPT, no input‑voltage regulation → in weak light the
  panel voltage collapses and charging stalls; it "works" only with a panel that massively overshoots in
  full sun, wasting the harvest. **Don't use TP4056 for solar.**
- **TI bq24074** (programmable input‑current limit — a poor‑man's MPPT that prevents the TP4056 collapse;
  on Adafruit's solar charger) and **bq25570** (nano‑power fractional‑Voc MPPT for µW–mW harvesting — the
  right IC for a µA node *if* you insist on solar). **DFRobot Solar Power Manager**, **Voltaic** (rugged
  outdoor‑rated) are known‑good board families. Confirm a **low‑temp NTC cutoff** if it will freeze.

---

## 7 · Worked examples (temperate ~45°N, Dec PSH ≈ 2.0, derate 0.6, 11.1 Wh per 3000 mAh, 80% DoD)

**(a) µA‑class gate/contact sensor — where solar is the *wrong* answer.** Load ~20 µA avg →
`0.02 mA × 3.7 V × 24 h ≈ 1.8 mWh/day` → ~3.3 Wh over 5 years. A single **Li‑SOCl₂ AA (~8.6 Wh)** covers
it with no solar, no cold‑charge risk, no PSoC aging. The clearest "more is worse" case.

**(b) Fence/radar node, ~25 mA avg on a 3000 mAh LiPo →** `2.2 Wh/day`. Panel `≥ 2.2/(2.0×0.6) ≈ 1.85 W`
→ a **3–5 W panel** (margin for soiling/snow). Battery at 3 days: `≥ 8.3 Wh` → the 11.1 Wh cell gives
~4 days. **A 10 W panel is wasted** — it can't store more than the battery holds and just overflows,
while wind‑load and theft‑risk go up. Cloudy‑climate 5‑day build: ~22 Wh (2× cells) *and* ~5 W — note how
battery and panel move **together**.

**(c) Camera node, ~1 Wh/day, winter‑solar‑starved.** Panel `≥ 1.0/(2.0×0.6) ≈ 0.83 W`… **but a cloudy
48°N December is ~1.0 PSH** → `≥ 1.7 W` → choose a **5–6 W panel**. Here bigger panel *is* justified —
it fixes a real worst‑month shortfall. Battery 5 days: `≥ 6.25 Wh` → 11–22 Wh. And re‑check §4: if the
site freezes hard the panel can't recharge in January anyway — reduce winter capture rate or accept a
seasonal outage.

| Device class | Daily_Wh | Panel | Battery | Controller | Note |
|---|---|---|---|---|---|
| µA gate/contact | ~0.002 | **none** (or 0.5 W) | **primary Li‑SOCl₂ ~8.6 Wh** | bq25570 if solar | solar is usually the *wrong* call |
| Sensor, few mA | ~0.5 | 1–2 W | 3000 mAh (11.1 Wh) | CN3791 MPPT | small & balanced |
| Fence/radar ~25 mA | ~2.2 | 3–5 W | 3000 mAh, 2× if cloudy | CN3791 MPPT | 10 W wasted unless battery grows too |
| Camera ~1 Wh/day | ~1.0 | 5–6 W | 11–22 Wh | CN3791 MPPT + NTC | bigger panel justified by worst‑month |

---

## 8 · The takeaways

$$\text{Panel}_{Wp} \ge \frac{\text{Daily\_Wh}}{\text{PSH}_{worst} \times 0.6}, \qquad \text{Battery}_{Wh} \ge \frac{\text{Daily\_Wh} \times \text{Days\_autonomy}}{0.8}$$

- **Right‑size, not biggest.** The smallest *balanced* pair that survives your worst month. Faucet,
  bucket, drain.
- **Oversized panel** → overflow, charger/battery stress, a *sail* on a fencepost, theft target.
- **Oversized battery** → never refills in winter (unreachable reserve), cost/weight, and dead weight in
  the cold — where a **primary cell beats solar** for low‑load nodes.
- **Measure the real load; pull your real PSH; use MPPT (CN3791), never TP4056, for solar.**
- Pair with the [cold‑weather envelope](./cold_weather_envelope.md): **cold decides the chemistry and the
  recharge reality; sizing decides how much panel and battery for your worst month.**

---

*Sources: NREL PVWatts calculator + v5 technical manual (PSH by coordinates; 14% default loss, losses
multiply); Unbound Solar (PSH map, tilt = lat+15°, battery‑bank sizing) and off‑grid sizing references
(days‑of‑autonomy, DoD); CN3791 datasheet + board guides and the TP4056‑not‑for‑solar caveat; RELiON /
RedArc on cold‑charge lithium plating; Seeed XIAO deep‑sleep figures. Numbers are tagged [HARD]/[ROT];
pull PSH for your own coordinates and measure the real board — the only hard numbers are datasheet
electricals, the 1000 W/m² PSH reference, and PVWatts/GSA irradiation for a given site.*
