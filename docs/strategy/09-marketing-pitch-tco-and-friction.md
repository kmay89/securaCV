# 09 — Marketing Pitch: Total Cost of Ownership, Setup Friction & Next Steps

> Companion to [05-market-and-cost-comparison.md](05-market-and-cost-comparison.md) (market
> verdict, business model) and [08-product-strategy.md](08-product-strategy.md) (north star).
> This doc does three things: (1) the customer-facing cost/competitor pitch with the math
> shown, (2) an honest audit of whether the work since the friction map shipped actually
> reduced friction, and (3) the next steps ranked by leverage. Pricing refreshed **June 2026**
> (Ring and Google both renamed/repriced their plans since doc 05 was written — see Sources).

## 1. The pitch, in one breath

**Every other camera bills you monthly to keep your footage on their servers. SecuraCV costs
$0/month forever, keeps footage on your hardware where it auto-deletes in 24 hours, and is the
only one that can *prove* — cryptographically — that nobody edited the record.**

Per persona (see [04-user-stories.md](04-user-stories.md)):

- **Priya (HA prosumer):** *"You already run Frigate. Five minutes in the App Store gets you
  a tamper-evident witness log and a verified-✓ timeline — $0/month, nothing leaves your LAN."*
- **Marcus (needs proof that holds):** *"A record your landlord, an abuser, or a deepfake
  challenge can't quietly rewrite — and that can't be turned into surveillance of you."*
- **The Chen family (mainstream, horizon):** *"A security camera with a 24-hour memory. No
  subscription, no cloud, no archive of your kids."*

## 2. Lifetime cost: the math, shown

Reference setup: **4 cameras, 5-year horizon** (the project's design target). Hardware figures
are typical mid-2026 US street prices, rounded; subscription prices verified June 2026.

| | Initial hardware | Recurring fees | 5-yr fees | **5-yr total** | Setup time |
|---|---|---|---|---|---|
| **Ring** (cloud) | ~$400 (4 cams @ ~$100) | Multi $9.99/mo (multi-cam) · AI Pro $19.99/mo | $600–$1,200 | **~$1,000–$1,600** | ~1 hr, app-guided |
| **Google Nest** (cloud) | ~$400–$720 (4 cams @ $100–$180) | Home Premium $10/mo · Advanced $20/mo | $600–$1,200 | **~$1,000–$1,900** | ~1 hr, app-guided |
| **Eufy** (local + optional cloud) | ~$630 (HomeBase ~$150 + 4 cams @ ~$120) | $0 · optional cloud ~$3/mo/cam | $0–$720 | **~$630–$1,350** | ~45 min, app-guided |
| **Reolink** (NVR) | ~$480 (NVR ~$200 + 4 PoE cams @ ~$70) | $0 (optional cloud) | $0 | **~$480** | 1–2 hr + wiring |
| **UniFi Protect** (NVR) | ~$720–$900 (gateway $200–$380 + 4 cams @ ~$130) | $0 | $0 | **~$720–$900** | 1–2 hr |
| **Frigate** (self-host) | ~$440–$520 (mini-PC/Pi ~$120–$200 + Coral ~$40 + 4 IP cams ~$280) | $0 (optional Frigate+ ~$50/yr) | $0–$250 | **~$440–$770** | 2–6 hr (Docker, YAML) |
| **SecuraCV** (self-host) | **+$0** on an existing HA/Frigate stack · ~$440–$520 greenfield (same box as Frigate) | **$0, ever** | **$0** | **~$0 incremental / ~$440–$520 greenfield** | **~5 min** on existing stack · 1–2 hr greenfield |

Reading the table:

- **The cloud vendors' real product is the fee.** Over 5 years, Ring/Nest fees alone
  ($600–$1,200) exceed the *entire* hardware cost of a SecuraCV setup. And a Ring camera
  without a plan doesn't record at all — the "optional" subscription is structurally mandatory.
- **Payback framing for the pitch:** a ~$200 mini-PC pays for itself vs. Ring Multi in
  ~20 months, vs. any $20/mo plan in 10 months. Everything after that is $0/month, forever.
- **Against the local-first players (Eufy/Reolink/UniFi/Frigate), price is parity — the
  differentiation is what no NVR gives you:** a privacy boundary enforced in code (no faces,
  no plates, auto-deleting footage) and an Ed25519-signed hash chain that makes tampering
  *provable*. We don't replace Frigate; we ride alongside it for +$0 and +5 minutes.
- **Canary sensor nodes** (ESP32-S3) add tamper-aware coverage at ~$10–25 per DIY node —
  an order of magnitude under any commercial camera. (Pre-flashed kits are the planned
  hardware revenue line; see §8 of [08-product-strategy.md](08-product-strategy.md).)

## 3. Setup time and friction, compared honestly

"Setup time" hides two different numbers: time-to-first-event, and the *kind* of effort.

| | Time to first event | Effort profile | Accounts/cloud required |
|---|---|---|---|
| Ring / Nest | ~15 min first cam | Phone app, QR, guided | Yes — vendor account, footage in their cloud |
| Eufy | ~30–45 min | Phone app + HomeBase | Yes (account), footage local |
| Reolink / UniFi | 1–2 hr | Browser/NVR config, wiring | Optional |
| Frigate alone | 2–6 hr | Docker, `config.yml`, MQTT by hand | No |
| **SecuraCV on HA + Frigate** | **~5 min** | App Store → wizard (key auto-generated, MQTT auto-discovered, `frigate.yml` generated) | **No** |
| **SecuraCV greenfield** | 1–2 hr | HA OS image + app wizard; the wizard absorbs most of Frigate's YAML friction | **No** |

The honest caveat we keep in the pitch: a cloud camera is still faster *from zero* than a
greenfield self-host stack. Our claim is narrower and stronger — **for the ~10% of the market
already on Home Assistant, we are the fastest path to a witness, and the only path at any
speed to tamper-evident proof.** That's the beachhead; the greenfield gap closes with boxed
hardware, not with marketing.

## 4. Did we create value and reduce friction? (scorecard)

Audit of the five ranked friction items from [08-product-strategy.md §5](08-product-strategy.md)
against what's actually in the tree today.

| # | Friction item (ranked) | Status | Evidence in-repo |
|---|---|---|---|
| 1 | **The terminal** | **Largely fixed** | HA App Store install with a setup wizard: device key auto-generated, MQTT auto-discovered, `frigate.yml` generated (README §Install). Docker sidecar is a one-file compose + `doctor` end-to-end check. `curl \| bash` is now the *alternative*, not the path. API token rotation is automatic. |
| 2 | **The invisible payoff** | **Partially fixed** | "SecuraCV Verified Timeline" Lovelace card with a strict verified-✓ badge ([docs/lovelace_timeline.md](../lovelace_timeline.md)); daily-digest, chain-integrity sensors and a Verify Now button created automatically; app can generate a dashboard from live zones. **Still missing:** out-of-the-box mobile digest/alert, and screenshots of the payoff in the README — the most persuasive asset is still not in the pitch. |
| 3 | **The unbuyable device** | **Open (DIY much improved)** | BOOT-tap pairing wizard, QR scan-to-pair, unique mDNS hostnames, Identify button, fleet manager ([docs/onboarding_workflow_evaluation.md](../onboarding_workflow_evaluation.md)). Builders are well served now; non-builders still can't buy a pre-flashed Canary. |
| 4 | **The unusable proof** | **Open** | Break-glass trustees and signed export remain CLI-only. The moat still can't be *demonstrated* by a non-engineer under stress. |
| 5 | **v1 credibility gap** | **Nearly closed** | README badge is `v1-rc`; the roadmap's CI gates (Frigate→HA e2e, RTSP e2e, audit-boundary doc) are closed per [v1-roadmap.md](../v1-roadmap.md). Remaining: on-device hardware validation, then tag and align `CHANGELOG.md`. |

**Verdict: yes — real value was created and the top friction item is substantially gone.** The
install story went from developer-grade to a genuine 5-minute wizard, and the payoff (verified
timeline) exists as a product surface instead of raw sensors. But the two assets that convert
*new* customers — visible proof in the pitch (screenshots, a live demo of the ✓) and a shipped
v1 tag — are still pending, which means the friction we removed isn't yet *visible to anyone
who hasn't already installed*. That is the gap to close next.

## 5. Next steps, ranked by leverage

1. **Tag v1.** Cheapest, highest-leverage credibility move. Every pitch line above is
   undercut by "pre-release" until this lands. (Blocker: on-device hardware validation only.)
2. **Show the payoff in the pitch.** Screenshots/GIF of the verified-✓ timeline and the
   morning digest in the README top fold, plus this doc's TCO table distilled into a
   "$0/month, forever — here's the math" block. Zero engineering, pure conversion.
3. **One-tap signed export + standalone verifier.** Converts the moat from architecture into
   a demo a lawyer can run; it is also the on-ramp for the future attestation revenue line.
4. **Out-of-the-box mobile digest/alert** (HA companion-app blueprint shipped by the wizard).
   The daily "all clear" is the retention product; today it requires assembly.
5. **Pre-flashed Canary kit pilot** (small batch). First revenue, and the only fix for
   friction item 3; the DIY onboarding work has already de-risked the UX.
6. **Instrument the funnel goal:** measure time from app install to first verified-✓ event
   against the 15-minute target in [08 §10](08-product-strategy.md) — it's the one number
   that tells us whether friction is actually falling for new users.

---

### Sources

2026 pricing verified June 2026:

- [Ring subscription plan changes in 2026 (Ring)](https://ring.com/explore-ring-home) — plans
  now Solo $4.99/mo (one camera), Multi $9.99/mo (all cameras), AI Pro $19.99/mo.
- [How Much Does a Ring Subscription Cost? (HomeGuide, 2026)](https://homeguide.com/costs/ring-subscription-cost)
- [Google Home Premium subscription (Google Store)](https://store.google.com/product/google_home_premium?hl=en-US) —
  $10/mo Standard, $20/mo Advanced; replaced Nest Aware.
- [Learn about price changes for Google Home Premium (Google Nest Help)](https://support.google.com/googlenest/answer/13856600?hl=en)
- [Nest Aware Is Now Google Home Premium (Google)](https://home.google.com/get-inspired/welcome-to-google-home-premium-the-new-era-of-nest-aware/)

Market sizing, eufy/Reolink/UniFi/Frigate cost context, and the evidence-authenticity tailwind:
see the source list in [05-market-and-cost-comparison.md](05-market-and-cost-comparison.md).
Hardware figures are typical US street prices (mid-2026), rounded, and stated as assumptions —
not quotes.
