# Vehicle DBC excerpts — sources & sync policy

Every `.dbc` file in this directory is a **curated excerpt**, not a full vendor drop, of
[commaai/opendbc](https://github.com/commaai/opendbc) — the open-source (MIT-licensed) CAN
signal database used by comma.ai's openpilot across 300+ production vehicle ports. It's a real,
actively-maintained upstream, not a hobbyist snapshot — that's the whole point of building on it
instead of hand-typing byte offsets from a `candump` guess: signal names and bit positions come
from an authoritative source that other people are also bench-verifying, cross-checking, and
fixing bugs in.

**Why an excerpt, not the whole file:** the upstream `.dbc` files (especially `vw_mqb.dbc`,
`toyota_2017_ref_pt.dbc`) are hundreds of messages covering things this project has no reason to
touch (radar, ADAS torque control, ADAS camera). Vendoring the whole file would bloat the repo
with content `scripts/dbc_signal_resolve.py` never reads. Each excerpt here keeps only the
`BO_`/`SG_` blocks relevant to arrival/departure-shaped claims (door, ignition/terminal state,
gear/park) — copied byte-for-byte from upstream, not retyped, so the bit math is exactly what
upstream published.

| Excerpt | Upstream source | Fetched | License |
|---|---|---|---|
| [`honda_common_excerpt.dbc`](./honda_common_excerpt.dbc) | [`opendbc/dbc/generator/honda/_honda_common.dbc`](https://github.com/commaai/opendbc/blob/master/opendbc/dbc/generator/honda/_honda_common.dbc) + [`_gearbox_common.dbc`](https://github.com/commaai/opendbc/blob/master/opendbc/dbc/generator/honda/_gearbox_common.dbc) | 2026-07-23, `master` | MIT (opendbc) |
| [`toyota_2017_pt_excerpt.dbc`](./toyota_2017_pt_excerpt.dbc) | [`opendbc/dbc/toyota_2017_ref_pt.dbc`](https://github.com/commaai/opendbc/blob/master/opendbc/dbc/toyota_2017_ref_pt.dbc) | 2026-07-23, `master` | MIT (opendbc) |
| [`vw_mqb_excerpt.dbc`](./vw_mqb_excerpt.dbc) | [`opendbc/dbc/vw_mqb.dbc`](https://github.com/commaai/opendbc/blob/master/opendbc/dbc/vw_mqb.dbc) | 2026-07-23, `master` | MIT (opendbc) |

No commit SHA is pinned above — these were fetched from `master` on the date shown, not a tagged
release, because opendbc doesn't tag releases in a way that maps cleanly to "this vehicle year is
covered." Re-running the fetch periodically (or whenever a signal seems wrong on the bench) is a
manual step; nothing here silently goes stale without a human re-checking it, but nothing
automatically re-syncs from upstream either. That tradeoff is deliberate — see
`docs/hardware/canary_vehicle_profiles.md` §5 "never let it rot."

## What keeps this from rotting silently

1. `scripts/dbc_signal_resolve.py --check-profiles` re-parses every excerpt and confirms every
   signal referenced by `vehicle_profiles.toml` still exists with the byte offset/mask the profile
   claims — a typo, a renamed signal, or a hand-edited profile that drifted from what the DBC
   actually says fails loudly. Same drift-gate philosophy as
   `spec/witness_dictionary.json`/`scripts/lint_dictionary_sync.py` elsewhere in this repo — the
   generated artifact is checked against its real source, not trusted on faith.
2. CI (`.github/workflows/lint.yml`) runs that check on every push touching this directory or
   `vehicle_profiles.toml`.
3. Every profile entry that isn't bench-confirmed says so explicitly (`status` field) — a stale or
   wrong signal degrades to "documented, unverified," never silently presents as confirmed.

## Bit-numbering primer (why this needs a real parser, not hand arithmetic)

DBC signals have a `start_bit` and a byte order (`@0` = Motorola/big-endian, `@1` =
Intel/little-endian) that together decide which byte and which bits a signal occupies — getting
this wrong by hand is exactly the kind of mistake that produces a plausible-looking but silently
wrong route. `scripts/dbc_signal_resolve.py` implements the conversion once, tested against golden
vectors pulled from these exact excerpts (see its test suite), so nobody has to re-derive "bit 37,
Motorola, byte order @0" into a byte/mask pair by hand again. It only resolves signals fully
contained within one byte (Canary Vehicle's `CanRoute` is a single-byte model); a signal spanning
multiple bytes is refused with a clear error rather than silently truncated.
