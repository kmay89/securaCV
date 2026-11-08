# Validation sheets — design note (item 7 CP3)

**Status: designed, not built.** This note is the design record the
`designed` rows `us_evidence` FRE 707 (proposed), `alcoa_plus`
Accurate-analytic, `capture_provenance` kit-capture-summary and
`enterprise_assurance` validation-sheets point at. Item 7's CP3 makes the
schema normative in `spec/ruleset_sheet_v1.md`, ships five sheets, packages
them in the court kit, and rewrites this page into the guide. Until then the
schema below is informative.

Proposed FRE 707 would hold machine-generated analytic output to the
reliability standard for expert testimony; its current status is
unverified. The answer latent in Invariant VI is a per-ruleset sheet: every
event is bound to its `ruleset_id`, so the documentation of what a ruleset's
claims mean can be written once and reused in every case.

## The sheet, `securacv-validation-sheet:v1` (closed)

| Key | What it holds |
|---|---|
| `schema` | `securacv-validation-sheet:v1` |
| `ruleset_id`, `ruleset_hash` | the id, and SHA-256 of the id string (checked) |
| `binds`, `binding_note` | always `label` in v1, and the sentence saying the hash binds the label, not the rules, a model or a configuration |
| `author` | `{"kind": "securacv" | "operator", "organization": …}` |
| `sheet_version`, `status` | an integer; `draft`, `issued` or `withdrawn` |
| `applies_to` | `kernel_versions` (exact versions or minor-line wildcards such as `0.9.*`, matched as strings), `producers`, `module_specs` |
| `components` | each bridge or detector in the path, with its owner (ours or third-party) |
| `emits` | event types from the dictionary; object classes from Person, Vehicle, Animal, Package, Unknown |
| `detects` | per event type: what the claim asserts, what it does not (identity, intent, count, trajectory), and its inputs |
| `confidence_meaning` | the kernel's assessment of rule satisfaction, not a probability of real-world truth |
| `known_limitations` | never empty |
| `not_claimed` | always includes the identity of any person or vehicle, the accuracy of any upstream detector, and any time finer than the 10-minute bucket |
| `validation` | `status` (`not-performed`, `method-defined` or `performed`), `method`, `dataset`, `report_sha256`, `report_ref`, `performed_by`, `performed_month` |
| `invariant_vi` | the sentence that events under this ruleset are never reprocessed under another |

A SecuraCV-authored sheet never says `performed`: the tooling never writes a
validation result, and no result number is ever committed to the tree. A
performed validation cites its report by hash; the numbers live there.

A module spec such as [`spec/modules/zone_crossing.md`](../../spec/modules/zone_crossing.md)
is a template for what a module may and must never emit. A sheet is
reliability documentation for one labeled configuration. A sheet cites the
module specs it relies on and never restates them.

## The shipped set

Five SecuraCV-authored sheets, one per ruleset id the shipped binaries use
by default (`ruleset:v0.1`, `ruleset:frigate_v1`, `ruleset:grove_vision2_v1`,
`ruleset:ingest`, `ruleset:demo`), each with `validation.status` set to
`not-performed`. Each `detects` row describes only what the code can be cited
for; anything else is listed as not characterized.

## The kit: claims apart from recordings

`court_export` gains a `claims/` directory beside `evidence/`:

- `claims/CLAIMS.md` names each `(ruleset_id, ruleset_hash)` in the bundle,
  its event count, event types and kernel versions, and its sheet state;
- `claims/validation/` holds the packaged sheets, verbatim, with a rendered
  copy;
- the capture path: event counts by attestation tier, kernel version and
  ruleset.

The sheet state per ruleset is one of `packaged`, `none`, `version_mismatch`
(packaged, with that statement), `id_hash_mismatch` (events whose id does not
hash to their `ruleset_hash`; named, never refused) and `withdrawn` (named,
not packaged). A sheet is never fabricated: `none` says plainly that the kit
makes no statement about how reliably that ruleset detects what its events
report. A supplied sheet that fails its own checks is refused before the
kit directory exists.

**Claims and recordings.** A kit holds machine claims: the event bundle and
the sheets for the rulesets that produced it. Recordings (unsealed frames,
their handoff sidecars and C2PA manifests) are packaged, when they are,
under `recordings/` and never beside claims; a sheet describes a ruleset's
claims, never a recording. The kit format string does not change; the
manifest gains additive keys.
