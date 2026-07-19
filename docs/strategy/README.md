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
