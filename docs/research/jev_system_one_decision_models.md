# Research — Jev and "System One" decision models: evaluated, declined (2026-09)

Should SecuraCV adopt [Jev](https://typesafe.ai/blog/introducing-system-one-models-and-jev),
TypeSafe AI's "System One" typed-decision model, anywhere in the stack?
**No — and structurally we cannot use the real thing.** Jev is a
closed-weight, hosted-only cloud API, and every place it could plug in sits
behind an invariant that forbids exactly that. The more interesting finding
is *why* the answer is no even setting the hosting aside: SecuraCV already
practices what Jev is selling — typed, bounded, auditable runtime decisions —
by deterministic means, and the one thing Jev would add (a probabilistic
judgment layer with confidence scores) is the thing the invariants exist to
refuse.

> **Scope note (this is a decision record, not a feature).** Nothing here
> ships anything or changes any code. The deliverable is the verdict, the
> reasoning pinned so it doesn't get re-litigated from scratch, and the
> narrow revisit trigger in §5.

---

## 1. What Jev is

Launched 2026-09-16 by TypeSafe AI, Jev is the first of what the vendor
calls **System One models**. It is not an LLM in the usual sense: instead of
generating text token by token, it takes program state plus *typed
questions* and returns decisions in a single non-autoregressive parallel
pass. Three question primitives:

| Primitive | Returns |
|---|---|
| `Choice` | one pick from a known option set, with a probability per option |
| `Score` | a position on an ordered rubric, as a continuous value |
| `Noul` | the probability that a yes/no statement is true |

Every answer carries a confidence value derived from the shape of the
probability distribution; the intended pattern is code that branches on the
typed answers and routes low-confidence cases to a human. Training uses a
method the vendor calls Reinforcement Learning for Calibrated Decisions
(RLCD). The TypeScript story (`@typesafe-ai/sdk`, Vercel AI SDK
`experimental_evaluate`, Cloudflare Workers AI) is the marketing spearhead —
"your agent burns LLM money on switch statements" is the pitch.

Vendor-reported numbers, none independently verified as of this writing:
70–500 ms end-to-end, roughly 200x faster and 400x cheaper than comparable
LLM calls ($0.042 per million input tokens, output free), 64k context,
English-centric, text only.

Three caveats the early coverage converges on:

- **"Never hallucinates" means schema-valid, not correct.** A confidently
  wrong routing decision is still wrong; the guarantee is about output
  shape, not semantics.
- **You get a number, not a rationale.** No natural-language explanation of
  why it decided — poor for debugging and for audit trails.
- **The calibration claims are the vendor's**, on a product that was about a
  week old when this was written. TypeSafe's own docs say confidence
  thresholds must be validated on your data.

**Deployment is the decisive fact for us: cloud API only.** Closed weights,
no on-prem build, no container image. A fast-growing third-party ecosystem
replicates the *idea* with open weights (NanoJev, ~0.6B parameters, ships
weights and training pipeline; "von", ~395M, sub-15 ms local inference; an
experimental `jevlike-esp32` that runs a tiny one-pass decision scorer on a
microcontroller) — those are reconstructions of the shape, not the product,
and none has an evaluation record yet.

## 2. The disqualifier: it phones home, and four walls already say no

A hosted decision API would have to break at least one of four independent
structural barriers — each a `can't`, not a `won't`:

1. **The kernel's detector sandbox denies network syscalls outright**
   ([`src/module_runtime/sandbox.rs`](../../src/module_runtime/sandbox.rs));
   the [Backend Audit Checklist](../../AGENTS.md) additionally requires "no
   network operations" and "no telemetry" of any inference backend.
2. **Free-signals Invariant B** ([`spec/canary_free_signals_v0.md`](../../spec/canary_free_signals_v0.md)):
   signal-processing code MUST NOT initiate external network connections;
   Invariant D forbids automated notifications to external parties.
3. **The website's CSP is effectively self-only** and test-enforced — the
   help-desk routing in the website repo's `js/help-catalog.js` could never
   call an API, and the help-desk design already refused a hosted-LLM widget
   for exactly this reason
   ([`docs/design/automated_help_desk.md`](../design/automated_help_desk.md)).
4. **The dependency policy** ([`AGENTS.md`](../../AGENTS.md)) forbids
   anything that phones home.

That closes the question for Jev-the-product on any runtime surface, in any
direction, permanently — the same way [`whisper_local_voice.md`](whisper_local_voice.md)
closes world-facing transcription.

## 3. The deeper reason: we already do this, deterministically

Jev's market is teams spending LLM calls on decisions that should be cheap,
typed, and bounded. SecuraCV has **no runtime LLM calls to replace** — the
only LLM anywhere is the advisory CI reviewer, which never gates a merge.
Every runtime decision the fleet or the surfaces make is already typed and
bounded, just without probabilities:

| Jev use case | What SecuraCV does instead | Where |
|---|---|---|
| Intent routing with confidence | Fuzzy routing to **pinned** answers — "routing may be fuzzy; answers must be pinned" | [`automated_help_desk.md`](../design/automated_help_desk.md), website `js/help-catalog.js` |
| Agent-step decisions | A plain finite-state machine with named thresholds | `canary-vision` `presence_fsm` |
| Scoring / triage | Rule-based sensor fusion, host-tested | [`canary_sentinel_fusion_design.md`](../canary_sentinel_fusion_design.md) |
| Guardrails / verification | Deterministic lint gates in CI, plus the advisory reviewer for judgment calls | `scripts/lint_*.py`, `claude-review.yml` |
| Classification | Coarse physical classes from local models behind an audited trait | [`inference_backends.md`](../inference_backends.md) |

And the judgment layer Jev would add is refused on principle, not just on
plumbing. Free-signals **Invariant E** ("physics, not politics") forbids
behavioral classification and threat scoring, and pins the event vocabulary
to physical phenomena. A calibrated confidence that a mover is "suspicious,"
or a `Score` on how urgent a presence event is, is the first step toward the
ranked-people, predicted-behavior systems the hard-stop table in that spec
rejects by name. The absence of a probabilistic judgment layer is a feature
with a spec, not a gap.

The explainability point cuts the same way: our decisions are auditable
because a human can read the FSM, the fusion table, or the regex that made
them. Replacing any of those with a model that returns a number and no
rationale would be a strict regression in the property the project sells.

## 4. Where a Jev-*shaped* thing could ever live (and why not now)

Two candidates were considered honestly:

- **The proposed canary-sense coarse-class feature**
  ([`canary_sense_coarse_class_design.md`](../canary_sense_coarse_class_design.md),
  "large mover" vs "small mover") is the one place in the roadmap that wants
  a learned classifier at all. Its scoping doc already commits to a light
  SVM or decision tree — local, tiny, inspectable. If that feature is built
  and genuinely outgrows an SVM, the thing to evaluate is the **open-weight,
  on-device** lineage (NanoJev / von / `jevlike-esp32`-style frozen encoder
  plus small scorer), run inside the existing seccomp sandbox, gated by the
  Backend Audit Checklist, emitting only the physical classes the free-signals
  vocabulary already names. Never the hosted API.
- **CI** is the one place we already pay for a cloud LLM, and Jev's
  guardrails pitch nominally fits. But the advisory reviewer's entire value
  is prose judgment — surfacing what a regex can't, with an explanation a
  human can weigh. A number with no rationale is the wrong tool, and the
  deterministic gates already own everything a typed decision could own.

## 5. Verdict

| Question | Answer |
|---|---|
| Adopt Jev (the hosted API) anywhere? | **No, permanently** — four structural barriers (§2), and it's the wrong shape even where it fits (§3) |
| Adopt an open-weight System One model today? | No — nothing needs it; the only candidate feature isn't built and is scoped to an SVM |
| Revisit trigger | The coarse-class feature ships its rule-based/SVM form and demonstrably outgrows it — then evaluate a small open-weight local scorer under the Backend Audit Checklist, physical classes only |
| What to watch, passively | The open-weight replicas (NanoJev, von, `jevlike-esp32`) maturing an evaluation record; independent tests of the calibration claims |

## Sources (external, read 2026-09-20)

- [TypeSafe AI — Introducing System One Models & Jev](https://typesafe.ai/blog/introducing-system-one-models-and-jev) (vendor launch post)
- [awesome-jev-by-typesafe](https://github.com/Anil-matcha/awesome-jev-by-typesafe) — API shape, limits, stated anti-patterns
- [awesome-jev](https://github.com/yibie/awesome-jev) — ecosystem survey, self-hosting and edge entries
- [Vercel KB — classify, route, and score with Jev and AI SDK](https://vercel.com/kb/guide/typesafe-jev-and-ai-sdk)
- [DataCamp — Jev: the model that "never hallucinates"](https://www.datacamp.com/blog/system-one-models-jev) and [Kingy AI review](https://kingy.ai/blog/typesafe-jev-review-the-ai-model-that-doesnt-generate-text/) — the caveats in §1
- [Can you run Jev locally?](https://www.modemguides.com/blogs/ai-news/jev-typesafe-reality-check-run-locally) — hosted-only status, open-weight alternatives
- [jevlike-esp32](https://github.com/david-cermak/jevlike-esp32) — the microcontroller experiment noted in §4
