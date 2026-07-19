# SecuraCV Strategy & Analysis

This folder is a snapshot analysis of the SecuraCV codebase and a 5-year product
strategy. It is **documentation only** — nothing here changes code behavior or deletes
files. The cleanup section *flags* candidates for the maintainer to action separately.

| Doc | What it covers |
|-----|----------------|
| [01-codebase-map.md](01-codebase-map.md) | What every folder is, how it runs, where to start reading |
| [02-cleanup-and-rot.md](02-cleanup-and-rot.md) | Flagged folders/files to remove or consolidate (flag only) |
| [03-readme-and-marketing-audit.md](03-readme-and-marketing-audit.md) | How the product advertises itself; what the README rewrite fixes |
| [04-user-stories.md](04-user-stories.md) | Buy → install → use journeys for each user segment |
| [05-market-and-cost-comparison.md](05-market-and-cost-comparison.md) | Competitor costs, market size, good/bad-product verdict, business model |
| [06-feature-prioritization.md](06-feature-prioritization.md) | Must-have vs cut, justified for the 5-year horizon |
| [07-timeline-events-privacy-design.md](07-timeline-events-privacy-design.md) | Making the timeline/events useful without breaking privacy |
| [08-product-strategy.md](08-product-strategy.md) | **North star** — the canonical one-page strategy that sits above 01–07 |
| [09-marketing-pitch-tco-and-friction.md](09-marketing-pitch-tco-and-friction.md) | The customer-facing cost pitch: lifetime/recurring/initial cost + setup time vs. competitors (June 2026 pricing), friction-reduction scorecard, ranked next steps |
| [10-grove-vision-ai-v2-program.md](10-grove-vision-ai-v2-program.md) | Grove Vision AI V2 as a first-class Canary sensor: multi-board support (XIAO C3/S3), workflow-parity phases, dashboard/logging roadmap, SenseCraft-path rejection rationale |
| [11-home-assistant-platform-architecture.md](11-home-assistant-platform-architecture.md) | Full HA surface audit (integration vs Quality Scale, add-on vs App best practices, the three-entity-universe finding), 10 architecture principles learned from Frigate/ESPHome/UniFi Protect/Music Assistant, and the wow-factor roadmap (evidence packs, repairs-as-integrity-console, witness event entities, privacy-floor LLM tools) |
| [12-engineering-foundations-flight-rules.md](12-engineering-foundations-flight-rules.md) | Flight-software audit of kernel/firmware/CI through the NASA/JPL lens: verified P0 findings (busy_timeout evidence race, chain-fork risk, canary-vision watchdog, untested adapter surface, version drift), an FMEA of the evidence pipeline, the mishap mirror (Ariane→Knight mapped to our code), and the 14 Flight Rules — a CI-enforceable engineering constitution |
| [13-black-box-evidence-model.md](13-black-box-evidence-model.md) | How believed recorders (FDR/CVR, EU smart tachograph, EDR, VDR, body-cams, CTBTO, forensic custody standards) earn trust; mechanism-by-mechanism mapping onto our sealed log; the ten gaps (witness dictionary, correlation ritual, holds/erasure certificates, mesh witness cosigning, custody ceremony aimed at FRE 902(13)/(14)); and the Promise Card — what SecuraCV collects, promises, and refuses to overclaim |
| [14-pose-estimation-v2-ai.md](14-pose-estimation-v2-ai.md) | Studies the "Live Pose→3D" Grove Vision AI project for v2: adopt YOLOv8-Pose on the HX6538 module we already run, but refuse the skeleton stream (Invariant I treats it as raw media). Derive coarse physical claims (fall/collapse, prone, hands-raised duress candidate) on-device; ship one end-to-end as doc 10 Phase 3's exit criterion. The Processing 3D viewer is caged as an offline bench tool only |
| [15-maker-marketing-kit.md](15-maker-marketing-kit.md) | The customer-facing marketing materials: head-to-head comparison (cost/quality/features/privacy) in sellable form, the four maker-tribe audience map with per-tribe hooks, the channel playbook (HA forum, Reddit, Hackaday, Show HN, Printables, YouTube), and a repeatable launch sequence — with the Lab as the demo asset |
| [16-kit-commerce-pricing-and-fulfillment.md](16-kit-commerce-pricing-and-fulfillment.md) | The kit business made operable: four-rung SKU ladder (free plans → parts pack → full kit → assembled) priced from the real BOMs with margins, a three-phase printed-parts fulfillment pipeline (micro-farm → MJF/POD → assembler kitting), and the flashing/provisioning pipeline — web flasher for DIY, batch jig for kits, with Wi-Fi provisioning staying in the customer's home via the shipped QR flow |
| [17-ecosystem-strategy.md](17-ecosystem-strategy.md) | The ecosystem play, argued from history: what BlackBerry (trust moat ≠ enough), Apple (integration wins non-experts; the toll booth is a choice), Linux (neutral stable contract + opinionated distributions), and Signal (the open trust protocol wins even inside rivals' products; donation-only sustainability is the cautionary half) each prove; the 3D-printing mirror (RepRap/Marlin commons beat Stratasys → Bambu's closed integration took the consumer → Bambu's 2025 lock-in burned the trust); the five foundational ecosystem artifacts (frozen contract, conformance suite, standalone verifier, open mesh, delight layer); and the verdict — kernel-shaped from day one, makers now, industry later, consumer last — with three named tripwires |
| [securacv_product_strategy_whitepaper.md](securacv_product_strategy_whitepaper.md) | Long-form companion: market, competition, personas, friction/satisfaction ledgers, business model, roadmap |

## TL;DR

SecuraCV is not a normal security camera — it is a **witness**. Cameras detect events,
raw clips auto-delete, and the only thing that survives is a cryptographically signed,
tamper-evident log of *semantic* events with no faces, plates, or precise timestamps.

- **The moat**: privacy enforced *by construction* + tamper-evident proof. Nobody else
  in the consumer/prosumer market ships this. It rides a real tailwind: rising legal demand
  for authenticatable, deepfake-resistant evidence.
- **The gap**: onboarding is developer-grade, there's no polished timeline UI or app-store
  install, v1 hasn't shipped, and the hardware isn't productized.
- **Recommended lead segment**: privacy-conscious prosumers / homelabbers (largest reachable
  market today), with the at-risk / evidence user as the differentiating "why we exist" story.
- **Recommended business model**: open-core software + sold pre-flashed Canary hardware, plus
  an *optional* court-grade verification/attestation service that never touches raw footage.

**Start with [08-product-strategy.md](08-product-strategy.md)** (the north star); read
the [whitepaper](securacv_product_strategy_whitepaper.md) for depth. Docs 01–07 are
the underlying analysis. See each doc for detail and sources.
