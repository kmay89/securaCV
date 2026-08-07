// FleetMerge.swift
//
// The one place transports become a fleet. Three sources now feed a Witness,
// and they are NOT equal — so the precedence lives here, written down and
// unit-tested, instead of as a growing pile of `if` branches inside the
// refresh loop.
//
// THE THREE TIERS (weakest to strongest):
//
//   1. BLE presence beacon — unauthenticated, connectionless, fleet-wide.
//      Works with no broker, no home Wi-Fi, no pairing. Coarse by construction:
//      flags, battery, health, a truncated chain height, 2 fingerprint bytes.
//   2. `GET /api/fleet` — unauthenticated, Wi-Fi, fleet-wide. The device's own
//      claim about itself: name, product, online, "chain":"ok".
//   3. `GET /api/v1/witness` — authenticated (paired token) AND locally
//      verified against a TOFU-pinned key. The only tier that can earn trust.
//
// THE RULES, and why:
//
//   * **Coarse never overwrites verified.** A beacon may not set a trust badge,
//     and neither may `/api/fleet` — a device SAYING "chain":"ok" is a claim,
//     not a verification. Only ChainVerifier's Ed25519 pass sets `.verified`.
//     Rendering a badge we didn't compute is exactly the lie the whole product
//     exists to not tell.
//   * **Coarse never downgrades.** A missing field in a weaker tier means "not
//     published", never "absent" — so gaps are filled, values are not replaced.
//   * **Safety-positive facts only ever go up.** Hearing tamper sets tamper.
//     NOT hearing it clears nothing: a beacon that stops mentioning tamper is
//     not evidence the tamper ended.
//   * **Being heard is proof of life.** Any sighting sets `.online` and moves
//     `lastSeen` forward, even if HTTP had given up — the device is demonstrably
//     alive on some radio, which is precisely what the dead-man's-switch asks.
//
// PURE: Foundation only — no CoreBluetooth, no URLSession, no SwiftUI. That is
// why `BeaconSighting` lives in Wire/. FleetMergeTests exercises every rule
// above on the host.

import Foundation

enum FleetMerge {

    // MARK: - BLE presence beacon (tier 1)

    /// Build a Witness for a Canary we have only ever *heard* — never paired
    /// with, never reached over HTTP. It renders as a real, present device
    /// (because it is one) but carries no trust and no chain.
    static func provisionalWitness(from sighting: BeaconSighting) -> Witness {
        var w = Witness(id: sighting.provisionalID)
        w.name = sighting.displayName
        fold(sighting, into: &w)
        return w
    }

    /// Fold a sighting into a Witness, obeying the precedence rules above.
    static func fold(_ sighting: BeaconSighting, into w: inout Witness) {
        let b = sighting.beacon

        // Heard it → it is alive, on some radio, right now.
        w.seenViaBLE = true
        w.link = .online
        if let existing = w.lastSeen {
            w.lastSeen = max(existing, sighting.lastHeard)
        } else {
            w.lastSeen = sighting.lastHeard
        }

        // Safety-positive only: never clear a tamper someone else reported.
        if b.tamper { w.tamper = true }

        // Fill gaps; never replace a value a stronger tier already established.
        if w.batteryPct == nil { w.batteryPct = b.batteryPct }

        // The wire carries only the LOW 16 BITS of the chain height. Writing
        // that over a full height read from /api/v1/witness would silently
        // truncate it, so it is used only when we have nothing at all.
        if w.chainLength == 0, b.chainLow16 > 0 {
            w.chainLength = UInt32(b.chainLow16)
        }

        // Deliberately NOT set from a beacon:
        //   * badge — trust is verified, never advertised (see rules above);
        //   * rssiDBM — the Witness field is the device's Wi-Fi RSSI; a BLE
        //     RSSI is a different radio at a different power, and conflating
        //     them would make the signal bars quietly lie;
        //   * fingerprint — two bytes is not a 16-hex fingerprint, and writing
        //     a padded one would fake an identity we don't have.
    }

    /// Attach beacons to the fleet: fold each sighting into the Witness it
    /// plausibly belongs to, and surface the rest as provisional entries.
    ///
    /// Matching is by fingerprint suffix, which is a HINT — two bytes narrow the
    /// field, they do not prove identity. That is safe here because a match only
    /// ever adds coarse liveness to a row the user already paired; it can never
    /// grant access or manufacture trust. Ambiguity is resolved conservatively:
    /// if a suffix matches more than one known Witness, we attach to none of
    /// them and let it stand alone rather than decorate the wrong Canary.
    static func attach(_ sightings: [BeaconSighting], to fleet: inout [Witness]) {
        for sighting in sightings {
            let candidates = fleet.indices.filter { i in
                !fleet[i].fingerprint.isEmpty && fleet[i].id != sighting.provisionalID
                    && sighting.beacon.matches(fingerprint: fleet[i].fingerprint)
            }

            // Exactly one known Canary owns this suffix → decorate it.
            if candidates.count == 1 {
                fold(sighting, into: &fleet[candidates[0]])
                continue
            }
            // Already standing alone from an earlier pass → keep it one row.
            if let i = fleet.firstIndex(where: { $0.id == sighting.provisionalID }) {
                fold(sighting, into: &fleet[i])
                continue
            }
            // Zero matches (never seen) or several (ambiguous two-byte suffix):
            // stand alone rather than mislabel a Canary.
            fleet.append(provisionalWitness(from: sighting))
        }
    }

    // MARK: - /api/fleet self-report (tier 2)

    /// Fold one `/api/fleet` row into a Witness. The device is describing
    /// itself over an unauthenticated endpoint, so this fills identity and
    /// liveness — and stops short of trust.
    static func fold(_ row: FleetSelfDevice, into w: inout Witness, heardAt: Date = Date()) {
        if w.name.isEmpty { w.name = row.name }
        if w.deviceType == .unknown { w.deviceType = row.deviceType }
        // The raw product string, kept beside the coarse enum: it is what the
        // figure lookup resolves at full precision, and a row that already
        // has one keeps it (fill gaps, never replace).
        if w.publishedType == nil, !row.product.isEmpty { w.publishedType = row.product }

        if row.online {
            w.link = .online
            if let existing = w.lastSeen {
                w.lastSeen = max(existing, heardAt)
            } else {
                w.lastSeen = heardAt
            }
        } else if w.link == .unknown {
            // It told us it is down. Only believe that over "no idea" — a
            // stronger tier that just reached the device outranks this.
            w.link = .offline
        }

        if w.chainLength == 0, let h = row.chainHeight, h > 0 {
            w.chainLength = UInt32(clamping: h)
        }

        // When the key was born. Unlike everything else here this is not a
        // "fill gaps" field — a device that has just learned its own birth day
        // is the authority on it, and the row we hold may predate that. It
        // still cannot be *un*-learned: the firmware never restates a recorded
        // day (birth_day.h), so a later row can only ever carry the same value.
        if let day = row.bornDay, day > 0 {
            w.bornDay = day
            w.bornExact = row.bornExact
        }

        // `row.chainVerifies` is intentionally unused: "chain":"ok" is the
        // device's claim about itself. The badge is only ever set by
        // ChainVerifier, from a signature this phone checked against a pinned
        // key. See the rules at the top of this file.
    }

    /// Build a Witness for a Canary that answered `/api/fleet` but isn't paired.
    /// Namespaced id, like the BLE provisional rows.
    ///
    /// The id carries a per-ROW discriminator, not just the answering host: a
    /// hub reports itself *and* its peers from one address, so keying on the
    /// host alone would give several rows the same `Identifiable` id and let
    /// SwiftUI's `ForEach` reuse or drop the wrong ones. `index` keeps rows
    /// distinct even when two peers share a name (or have none).
    static func provisionalWitness(from row: FleetSelfDevice, host: String,
                                   index: Int = 0, heardAt: Date = Date()) -> Witness {
        var w = Witness(id: "lan:\(host)#\(index)")
        w.name = row.name.isEmpty ? host : row.name
        w.deviceType = row.deviceType
        fold(row, into: &w, heardAt: heardAt)
        return w
    }
}
