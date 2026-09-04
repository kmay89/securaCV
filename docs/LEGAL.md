# Legal posture — copyright, licensing, and the rules that protect the mission

Canonical home for SecuraCV's copyright and legal rules. If a footer, a
header, a policy line, or a claim anywhere disagrees with this file, this
file wins — same standing as [`BRAND.md`](BRAND.md) has for voice. Findings
and evidence live in the [legal, claims & risk audit](legal-audit-2026-07.md);
this doc is the *rules* that came out of it.

> **This is not legal advice**, and nothing here creates an attorney–client
> relationship. It is the project's own policy, written so a solo operator
> and every contributor apply the same rules — and so the genuinely
> consequential calls go to a licensed attorney with the homework done
> (§8 is that list).

The mission is trust ([`BRAND.md`](BRAND.md)). These rules exist so the
project can survive being worth something: the code is given away on
purpose, which means the things that actually need defending are **the
marks, the claims, and the person** — and the discipline below is how each
one gets its armor.

---

## 1. Who owns what

- **Errer Labs** is the legal person behind the project: owner of the
  original copyright, the trademarks, and the seller of record. The
  copyright holder is always written **"Errer Labs"** — never "SecuraCV"
  (that's the platform, not an entity; the audit found a stray
  "© SecuraCV" and it's wrong).
- **Contributors keep their copyright.** There is no copyright assignment
  and no CLA — contributions are licensed *in* under Apache-2.0 exactly as
  the project licenses *out* (inbound = outbound, per Apache-2.0 §5 and
  [`CONTRIBUTING.md`](../CONTRIBUTING.md)). This is deliberate,
  mission-aligned protection: a project no one entity can relicense out
  from under its community is the anti-MakerBot promise in
  [`BRAND.md`](BRAND.md), kept structurally.

## 2. The license: Apache-2.0, everywhere, never walked back

The project's license — the root [`LICENSE`](../LICENSE), the default for
the tree, and the license every contribution comes in under — is **Apache
License 2.0**. Why Apache and not something looser or tighter:

- **§3 patent grant:** every contributor licenses the patents their
  contribution practices — real, automatic protection against a
  contributor-turned-troll, which MIT/BSD don't provide.
- **§5 contribution term:** anything submitted for inclusion is
  Apache-licensed by default — the inbound=outbound rule has teeth even
  when someone forgets a sign-off.
- **§6 trademark exclusion:** the license explicitly does *not* grant the
  marks — which is what makes §6 of this doc possible at all.

Two carve-outs, stated honestly:

- **Brand assets** under [`brands/`](../brands/) are trademarks, governed
  by [`TRADEMARK.md`](../TRADEMARK.md) (the "Works with SecuraCV" badge
  stays free under that policy's rules).
- **Components that carry their own license keep it.** The tree is not
  uniformly Apache-2.0 today: `canary-vision/` (the Vision device-API/SPA
  tree at the repo root) and `firmware/common/csi/` ship **MIT** LICENSE
  files, and
  several firmware sources carry MIT headers (some first-party, some
  vendored — e.g. `qrcodegen`). **The nearest LICENSE/header governs that component**;
  nothing in this doc relicenses anything, and MIT components convey no
  Apache-style patent grant. Redistributors must honor the per-component
  terms. Whether to consolidate the first-party MIT pieces to Apache-2.0
  (possible only with every copyright holder's authorization; vendored
  code keeps its license regardless) is a punch-list decision (§8).

Walking any of this back — dual-licensing, a proprietary "pro" tier of the
kernel, a license change — is off the table permanently, per
[`BRAND.md`](BRAND.md) and [strategy §8](strategy/08-product-strategy.md).
The moat is the marks and the trust, not the code.

## 3. Copyright rules (the ones every file and page follows)

1. **The one true copyright line:**
   `Copyright 2026 Errer Labs and SecuraCV contributors`
   (extend the year to a range as years pass; first publication 2026).
2. **Never "All rights reserved" next to an Apache grant.** The approved
   footer for websites and apps is:
   *"© Errer Labs · Licensed under Apache-2.0"* — we are granting broad
   rights, and the footer must not say the opposite (audit M4).
3. **New source files get an SPDX line** —
   `SPDX-License-Identifier: Apache-2.0` — in the first comment. Required
   for anything designed to be copied out alone (the pure headers in
   `firmware/common/`, scripts, generators); encouraged everywhere else.
   No 30-line license banners; the SPDX line and the root LICENSE do the
   job. Retrofitting old files is welcome hygiene, not a gate.
4. **The [`NOTICE`](../NOTICE) file travels.** Apache-2.0 §4(d) obliges
   redistributors to preserve it — it is the attribution that survives
   every fork. Third-party attribution requirements we take on (vendored
   code, embedded engines) get appended there, never scattered.
5. **Vendored/embedded third-party code keeps its own headers** and its
   license lands in the tree beside it. The dependency policy is
   **permissive-only** (no GPL/AGPL/EUPL in-tree). Enforcement, scoped
   honestly: today CI enforces it **for the Rust workspace only**
   (`cargo deny check licenses` per `deny.toml`); npm and firmware/vendored
   dependencies are policy-checked by review, not by a scanner —
   repo-wide license CI is an open engineering item (§8). Engines we
   embed rather than link (e.g. a future Apache-2.0 DNS engine per
   [`design/hub_network_witness.md`](design/hub_network_witness.md)) must
   pass the same policy.

## 4. Contributions: provenance is protection

Full rules in [`CONTRIBUTING.md`](../CONTRIBUTING.md); the legal core:

- **Inbound = outbound.** Submitting a PR licenses the contribution under
  Apache-2.0. No CLA, no assignment.
- **DCO sign-off** (`git commit -s`, Developer Certificate of Origin 1.1)
  is required on non-trivial contributions — the contributor's on-record
  statement that they have the right to submit the work.
- **You must have the right to submit.** No employer's code, no copied
  code without a compatible license and attribution, no license-washing.
- **AI-assisted contributions are the contributor's responsibility** —
  same certificate, same standard: you submit it, you vouch for its
  provenance. (This project is built with AI assistance in the open; the
  rule applies to us first.)

## 5. Trademarks — the actual moat

The code is a gift; the marks are the business. Policy lives in
[`TRADEMARK.md`](../TRADEMARK.md) (root — the **only** trademark policy;
the stale stricter copy that lived in `firmware/projects/canary-vision/`
now defers to it). Grants registry: [`trademark-grants.md`](trademark-grants.md).

Standing rules:

1. **"SecuraCV" is the mark we invest in.** It is distinctive, ours, and
   the house mark that carries every product name. Registration (USPTO
   word mark, then EUIPO as sales warrant) is a punch-list item (§8) —
   file for **SecuraCV**, not for "Canary."
2. **"Canary" is used only in combination** ("SecuraCV Canary") and is
   under a known cloud: CANARY is registered in our exact category (audit
   H1). No scaling of sales, no filing, and no new Canary-branded SKUs
   until the clearance opinion lands. The corrected policy line in
   `TRADEMARK.md` reflects this — the old "nobody's property" sentence is
   retired because it read as a waiver.
3. **Every new public-facing name gets a clearance search before first
   public use** — the hub product naming
   ([`design/hub_network_witness.md`](design/hub_network_witness.md) §6)
   is the template: candidates screened, punctuation gimmicks rejected,
   attorney search before anything ships.
4. **Enforcement stays buyer-protective**, per `TRADEMARK.md` §5: friendly
   note first, never against truthful compatibility claims. A generous
   policy consistently applied is stronger evidence of a healthy mark than
   a scary one nobody follows.

## 6. Claims discipline is legal armor

The audit's biggest finding was over-claiming, not under-protection. The
standing rules, which are also brand rules:

- **No absolutes.** "Tamper-evident," never "tamper-proof"; "no code path
  that…," never "impossible"; "any modification breaks the chain and is
  detectable," never "cannot be modified." The honest phrasing is nearly
  as strong and *defensible* (audit H2 has the full swap table).
- **No legal conclusions as marketing.** Never "courtroom-grade" or
  "admissible" — admissibility is a judge's call, not a spec sheet's
  (audit H3).
- **Scope data-locality claims precisely.** The airtight sentence: *"No
  footage and no identity ever leave the device. The only outbound traffic
  is a signed update check and the events you publish to your own hub."*
  (audit M2).
- **Legal, price, and claims files are human-gated.** The self-updating
  pipeline never auto-publishes a claim, a price, or anything in this
  doc's orbit (audit §6). A human owns every outward-facing legal
  sentence, on the record.

## 7. Export note

Strong cryptography ships in this tree (Ed25519, ChaCha20-Poly1305,
optional ML-KEM/ML-DSA). As publicly available open source it sits in the
EAR's open-source carve-out **once the one-time notification is sent** —
status and instructions in [`ENCRYPTION.md`](ENCRYPTION.md) (audit L1).

## 8. The counsel punch list (decisions docs can't close)

The items only an owner's signature or an attorney's opinion can finish,
highest leverage first. Check them off here as they close, with dates.

- [ ] **Form the LLC.** The single most protective move available: makes
      "the project" and "the person" legally distinct before hardware
      revenue scales (~$100–800; audit M4/§5).
- [ ] **"Canary" clearance opinion** from a trademark attorney (~$1–3k) —
      gate for scaling sales, new Canary SKUs, and any filing (audit H1).
- [ ] **File the SecuraCV word mark** (after/with the clearance work).
- [ ] **Terms of Sale page** for the store: seller identity, the
      30-day/DOA policy as the *limited* remedy, as-is beyond that,
      no-warranty note for print-it-yourself kits (audit M1).
- [ ] **Recording-consent notice** in buyer-facing docs and store:
      operators own lawful placement; know your local law (audit H3).
- [ ] **One editorial pass** over the website absolutes using the H2 swap
      table + retire "courtroom-grade" (audit H2/H3).
- [ ] **Stand up the fiscal host (OSC)** before any donation copy goes
      live; never imply tax-deductibility (audit M3).
- [ ] **Send the BIS/NSA encryption notification** and mark
      `ENCRYPTION.md` accordingly (audit L1).
- [ ] **Contact hygiene:** role email + mailing address on the site;
      refresh the privacy policy date (audit M4/L3).
- [ ] **Website footer sweep:** apply §3's approved footer everywhere;
      fix the stray "© SecuraCV" (audit M4 — lives in the website repo).
- [ ] **License inventory & consolidation decision:** enumerate the
      first-party MIT components (§2), decide keep-as-MIT vs relicense to
      Apache-2.0 (needs every copyright holder's authorization; note
      `firmware/common/csi/LICENSE` names "SecuraCV Contributors," not
      Errer Labs, and `canary-vision/LICENSE` spells the company
      "ERRERlabs" — both copyright lines need normalizing per §3), and
      record the outcome per component.
- [ ] **Repo-wide license CI:** extend the permissive-only gate beyond
      cargo-deny to npm and the firmware/vendored tree so §3.5's policy
      is machine-enforced everywhere (engineering, not counsel).

## 9. Standing rules for everything built next

- New product name → clearance search **before** first public use (§5.3).
- New outbound network call → the M2 scoped sentence gets updated in the
  same change, or the call doesn't ship.
- New vendored dependency → permissive-only gate + NOTICE aggregation (§3.5).
- New claim in copy → passes §6 or it doesn't publish; when in doubt, the
  [`BRAND.md`](BRAND.md) test applies (*show proof, never ask for faith*).
- Anything legal-adjacent an automation touches → human approval, always.
