# Contributing to SecuraCV

This is not a typical open-source project.

The **witness kernel** at the heart of SecuraCV is **infrastructure with
constitutional guarantees**. Contributions to it are welcome **only** if they
preserve those guarantees.

If you are here to add features to the kernel quickly, this is the wrong
project. **But most of SecuraCV is not the kernel** — read the next section
before you assume this wall is meant for you.

---

## Start here — your first contribution

The constitutional bar below applies to `spec/`, `kernel/`, and `src/`. The
rest of SecuraCV is built to be welcoming, and that's where a first-time
contributor should start:

- **Suggest something — you don't have to write any code.** The most useful
  thing many people ever contribute is one sentence about what they wish their
  Canary did. Say it on the [Community Ideas board](https://securacv.com/ideas)
  or straight in the [idea form](https://github.com/kmay89/securaCV/issues/new?template=idea.yml);
  one box is required and plain words are perfect. It becomes a public issue,
  people back it with a 👍, and the most-wanted are what get built next.
  [`docs/IDEAS.md`](docs/IDEAS.md) is the whole path — including why this
  project runs its suggestion box on GitHub, and what that actually costs you.
- **Build a Canary in the browser** — no hardware, no soldering. Run the Lab
  (`canary-local/`, hosted at the site's `/lab`), then add your virtual build
  to the community gallery. That's a real, mergeable first PR.
- **Port a board** — a new board is a **data-only PR** (pin map + capability
  flags + build env + registry entry) that can't touch shared logic. Start
  with [`firmware/PORTING.md`](firmware/PORTING.md).
- **Improve a guide, an enclosure, or a translation** — content and design
  PRs under `docs/`, `docs/hardware/enclosure/`, and the guides are low-risk
  and high-value.
- Look for issues labeled **`good first issue`** and **`help wanted`**.

Who owns what — and which maintainer seats are open — is in
[`MAINTAINERS.md`](MAINTAINERS.md). Be kind; see
[`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md).

---

## First: Read These Documents

Before writing or modifying code, you **must** read:

1. `spec/invariants.md`
2. `kernel/architecture.md`
3. `spec/threat_model.md`
4. `SECURITY.md`
5. `firmware/ARCHITECTURE.md` (if touching anything under `firmware/`)

If a proposed change conflicts with any of these, it will not be accepted.

---

## The Core Rule

> **Invariants are law.**

They are not:
- Suggestions
- Best practices
- Configurable options

They are enforced mechanically in code, tests, and architecture.

Any change that weakens, bypasses, or makes an invariant optional is a **security regression**.

---

## What We Welcome

Contributions that strengthen or clarify guarantees, including:

- Additional **conformance tests**
- Stronger **compile-time enforcement**
- Improved **auditability or verification tooling**
- Clearer documentation of *why* something is forbidden
- Performance improvements that **do not** expand observability or retention

When in doubt, assume “no” and justify why it must be “yes.”

---

## What We Do Not Accept

The following will be closed without discussion:

- Identity, biometric, or tracking features
- Optional privacy modes or configuration flags
- Longer retention “for convenience”
- Cross-bucket correlation extensions
- Retroactive processing of historical data
- “Advanced” modules that weaken isolation
- Changes justified only by performance or product needs

If you need these, you are building a different system.

---

## How to Propose a Change

Every non-trivial PR **must** include:

1. **Invariant impact statement**
   - Which invariants does this touch?
   - Why are they still preserved?

2. **Threat model impact**
   - Does this introduce new metadata?
   - Does it change correlation surface?
   - Does it affect failure modes?

3. **Enforcement point**
   - Where is the guarantee enforced in code?
   - What test fails if it breaks?

PRs without these will not be reviewed.

## Licensing & provenance of contributions

The legal canon is [`docs/LEGAL.md`](docs/LEGAL.md); the short version:

- **Inbound = outbound.** By submitting a contribution you license it
  under **Apache-2.0**, the same license the project grants out
  (Apache-2.0 §5). You keep your copyright — there is no CLA and no
  copyright assignment, on purpose.
- **Sign your commits off** (`git commit -s`) on non-trivial PRs — the
  [Developer Certificate of Origin 1.1](https://developercertificate.org/)
  is your on-record statement that you have the right to submit the work.
- **Only submit what you may submit.** No employer-owned code without
  permission, no copied code without a compatible permissive license and
  attribution intact. The dependency policy is permissive-only (no
  GPL/AGPL/EUPL in-tree), repo-wide; CI enforces it for the Rust workspace
  today (`cargo deny`), and the npm and firmware/vendored trees are held to
  the same policy by review (see [`docs/LEGAL.md`](docs/LEGAL.md) §3.5).
- **AI-assisted work is your work.** Same certificate, same standard —
  you vouch for its provenance. This project is built with AI assistance
  in the open; the rule applies to the maintainers first.

## Issue Templates

When reporting regressions or drift, use the structured issue forms:
- [Security Report](.github/ISSUE_TEMPLATE/security_report.yml)
- [Conformance Report](.github/ISSUE_TEMPLATE/conformance_report.yml)
- [Hardware Support Request](.github/ISSUE_TEMPLATE/hardware_support_request.yml)
- [Hardware Test Report](.github/ISSUE_TEMPLATE/hardware_test_report.yml)

---

## Hardware Contributions

Board ports are the one contribution type designed to be low-friction:
a new board is a **data-only PR** (pin map + capability flags + build env +
registry entry) that never touches shared logic, so it can't weaken an
invariant. Start with [`firmware/PORTING.md`](firmware/PORTING.md); what
"supported" means (and how your port earns its tier) is defined in
[`firmware/HARDWARE.md`](firmware/HARDWARE.md). Validated a board on real
hardware? File a **Hardware Test Report** — it becomes the permanent
evidence that promotes the board's support tier.

---

## Tests Are Not Optional

If your change adds behavior, it must add a test that proves:
- The allowed path works
- The forbidden path fails **loudly**

Tests that only cover the happy path are insufficient.

CI enforces formatting and linting. Before submitting, run:

- `cargo fmt --check`
- `cargo clippy -- -D warnings`
- `scripts/ci/conformance.sh`
- `scripts/lint_feature_flags.sh` (if you touched any feature flag)

### Feature flags

Flags select *which code is compiled* or *gate unbuilt surface* — they are **not**
a back door for the "optional privacy modes or configuration flags" forbidden
above. A flag may never make an invariant optional (e.g. `camera_peek_enabled`
stays immutable). Every flag is indexed in [`docs/feature-flags.md`](docs/feature-flags.md);
add or update its row in the same PR (the lint fails otherwise).

---

## Releasing the Home Assistant integration (HACS)

HACS users install the integration from its distribution mirror,
[`kmay89/securacv-homeassistant`](https://github.com/kmay89/securacv-homeassistant),
which carries `custom_components/securacv/` from here byte-for-byte. To ship
a change:

1. Land it here, bumping `custom_components/securacv/manifest.json`
   `"version"` and adding a matching `CHANGELOG.md` entry in the same PR.
2. On merge to `main`, the
   [`homeassistant-mirror.yml`](.github/workflows/homeassistant-mirror.yml)
   workflow copies the carried set to the mirror and opens a pull request
   there (it needs the `MIRROR_PAT` secret; without one it stays green and
   raises an issue instead). The mirror's own tests, hassfest, and
   freshness gates run on that PR before it merges.

For everything else release-shaped — firmware, the apps, the flashers —
start from [`docs/RELEASE_BUTTONS.md`](docs/RELEASE_BUTTONS.md), the
operator's index of every release button.

---

## Cleverness Warning

Many systems fail because of “reasonable” ideas like:
- Caching frames briefly
- Making tokens last longer
- Adding debug access
- Reusing hashes across windows

If you find yourself saying:
> “This should be fine…”

Stop. It probably violates an invariant.

---

## Review Philosophy

Expect reviews to focus on:
- Failure modes
- Adversarial interpretation
- Future misuse, not present intent

This is deliberate.

We optimize for **long-term civic safety**, not short-term velocity.

---

## Final Note

If reading this document makes you uncomfortable, that’s okay.

This project is designed to make unsafe ideas feel unwelcome.

That is a feature, not a bug.
