# Canary Vehicle — multi-brand DBC-sourced signal profiles

**Status:** real tooling, real sourced signals, zero bench confirmation. Every byte/mask in this
system is computed by `scripts/dbc_signal_resolve.py` from a vendored excerpt of
[opendbc](https://github.com/commaai/opendbc) (MIT-licensed, the open-source CAN database behind
comma.ai's openpilot across 300+ production vehicle ports) — never hand-typed, never guessed. What
it is NOT yet: verified against an actual Honda, Toyota, or VW on a bench. Read this doc as "the
matrix is real and grounded," not "plug this in and it works."

---

## 1 · The brainstorm — what makes this the coolest thing in the fleet

[`canary_vehicle_can.md`](./canary_vehicle_can.md) shipped the hero feature: ignition on/off
becomes a fleet-wide arrival/departure claim. That's one signal, one vehicle, hand-sniffed. Here's
where it goes from "a clever hack for my car" to "an actual platform":

- **A household, not a car.** Every vehicle in the driveway is its own `can_bus` adapter instance.
  "Last car leaves → arm the house; any car arrives → disarm, lights on" falls out of the existing
  claim vocabulary with zero new kernel code — it's just N adapters publishing the same
  `vehicle_arrival_departure` claim into different zones (`zone:driveway_left`,
  `zone:driveway_right`, whatever you name them).
- **A matrix, not a memory.** Nobody should have to re-run `candump` and eyeball a hex dump every
  time a new vehicle joins the fleet, or forget which byte meant what six months later. A named
  signal (`ZAS_Kl_15`, `DOOR_OPEN_FL`) resolved from a real database is self-documenting in a way
  a bare `byte_offset = 4, mask = "0x20"` never is.
- **Tied to a source that outlives this repo.** opendbc isn't a hobbyist gist — it's the signal
  database behind a company's actual driver-assistance product, under continuous bench validation
  by people with a much stronger reason than us to get it right. Riding that means our vehicle
  coverage gets better as *their* coverage gets better, for free, on our own schedule (see §5).
- **The contribute-back loop.** If we ever bench-confirm a signal opendbc has wrong, or discover
  one it's missing for a car we own, that's a real upstream PR — a two-way relationship with the
  source, not a one-way scrape.

**Ideas beyond arrival/departure, not built, worth naming so they don't get lost:**

- **Auto-fingerprinting.** openpilot identifies a car's exact trim from which CAN IDs are present
  and their DLCs — the same technique could let `adapter_host` suggest "this looks like a
  `honda-pilot-2016-2022`-shaped bus" instead of you picking a profile by hand.
- **A "confidence" claim tier.** Right now every profile signal maps 1:1 to
  `vehicle_arrival_departure`. A future refinement: require TWO independent signals (door state
  AND gear-in-park, say) to agree before claiming arrival, the same "two lenses agree" instinct
  used elsewhere in this codebase's adversarial-verification patterns — fewer false claims from a
  single flaky signal.
- **OBD-tamper and local-telemetry** — both already named as open items in
  `canary_vehicle_can.md` §2; this profile system is exactly the piece they were waiting on
  (a real signal source) to stop being hand-wavy.

---

## 2 · Why a DBC matrix beats hand-sniffing

`candump`-and-guess doesn't scale past one car, rots the moment you forget which byte was which,
and produces a `byte_offset`/`mask` pair nobody but you can audit. A named DBC signal instead:

1. **Is auditable.** `DOOR_OPEN_FL` at bit 37, Motorola, in `DOORS_STATUS` (ID `0x405`) is a claim
   anyone can check against the source file, not a number that fell out of a `candump` session.
2. **Is drift-checked.** `scripts/dbc_signal_resolve.py check-profiles` re-derives every profile's
   byte/mask from its vendored DBC on every CI run — a hand-edit, a typo, or an upstream rename
   fails loudly, the exact discipline `scripts/lint_dictionary_sync.py` already applies to the
   kernel's claim vocabulary.
3. **Generalizes.** The bit-math (`resolve_signal` — Motorola vs. Intel byte order, single-byte
   containment) is written once, tested against golden vectors, and applies to any future vehicle
   without re-deriving anything.

---

## 3 · How to add a vehicle

1. Find the relevant `.dbc` in [opendbc](https://github.com/commaai/opendbc/tree/master/opendbc/dbc)
   for your platform (search by make; Honda's are under `dbc/generator/honda/`, VW under
   `dbc/vw_*.dbc`, Toyota under `dbc/toyota_*.dbc`).
2. Copy the `BO_`/`SG_` block(s) you need into a new or existing excerpt under
   `docs/hardware/vehicle_dbc/`, byte-for-byte (don't retype — copy/paste, so the bit math is
   exactly what upstream published). Add a row to `SOURCES.md`.
3. Resolve the signal:
   ```bash
   python3 scripts/dbc_signal_resolve.py resolve docs/hardware/vehicle_dbc/<file>.dbc <MESSAGE> <SIGNAL>
   ```
4. Add a `[[vehicle]]` / `[[vehicle.signal]]` entry to `vehicle_profiles.toml` using that output.
5. `python3 scripts/dbc_signal_resolve.py check-profiles` — confirms it matches (it will, since you
   just generated it from the same tool, but this is also what CI re-checks forever after).
6. `python3 scripts/dbc_signal_resolve.py emit-routes <vehicle-id>` — paste the output straight
   into `adapter_host.toml`.
7. **Bench-verify before trusting it.** `candump can0` while triggering the real-world event
   (open the door, turn the key) and confirm the byte actually changes the way the profile claims.
   Flip `status` to `"bench-confirmed"` once it does — and only then.

---

## 4 · Current profiles

| Vehicle | Years | Signal | Status | Note |
|---|---|---|---|---|
| Honda Pilot | 2016-2022 | `DOOR_OPEN_FL` (driver door) | documented | Honda Sensing trim, per openpilot's own [CARS.md](https://github.com/commaai/openpilot/blob/master/docs/CARS.md) — a good cross-check this generation's bus is well-trodden, though that doc is about ADAS support, not this. |
| Honda Odyssey (4th gen) | **2011-2017** | `DOOR_OPEN_FL` | documented — **platform mismatch risk** | ⚠️ Your 2017 Odyssey is 4th-gen, a DIFFERENT platform than the 2018+ redesign below. Don't assume they share a bus layout. |
| Honda Odyssey (5th gen) | 2018-newer | `DOOR_OPEN_FL` | documented | The redesigned generation — separate profile on purpose, see above. |
| Toyota Corolla | 2020-newer | `B_P` (transmission in Park) | documented — **platform mismatch risk** | ⚠️ Sourced DBC is upstream-named "2017_ref"; the 2020+ Corolla moved to the TNGA platform. Least-confident profile here — bench-sniff before trusting it. |
| Volkswagen MQB (generic) | 2015-newer | `ZAS_Kl_15` ("Klemme 15" — switched ignition power) | documented | No specific VW model given — this is the shared MQB-platform gateway signal. Strongest single-signal candidate of the four: a textbook ignition-on flag, not a proxy. Tell me your exact model/year for a tighter profile. |

Every row is `documented`, not `bench-confirmed` — the honest reason `canary_vehicle_can.md`
still calls this whole feature "not bench-validated." Full detail, notes, and the exact resolved
`(can_id, byte_offset, mask)` per signal: [`vehicle_profiles.toml`](./vehicle_dbc/vehicle_profiles.toml).

---

## 5 · Never let it rot

- **Vendored, not linked.** The DBC excerpts live in this repo (`docs/hardware/vehicle_dbc/`), not
  fetched at build/CI time — no network dependency, no silent breakage when upstream reorganizes.
- **Drift-gated against themselves.** `check-profiles` guarantees the profile always matches the
  vendored excerpt. It does **not** guarantee the vendored excerpt still matches opendbc's current
  `master` — that's a deliberate, documented manual step (`SOURCES.md`'s re-fetch policy), not an
  automatic one, because opendbc doesn't version in a way that maps cleanly to "still correct for
  this car's year."
- **Bit math tested, not trusted.** `scripts/dbc_signal_resolve.py selftest` runs in CI on every
  push — 12 golden vectors plus two "must refuse a byte-crossing signal" cases. If the Motorola/
  Intel conversion ever regresses, this fails before a wrong byte offset reaches a profile.
- **Status fields are load-bearing.** `documented-platform-mismatch` isn't decoration — it's the
  same tier discipline as `firmware/boards/boards.json`'s `compile-tested` → `verified` ladder,
  applied to vehicle signals instead of dev boards.
