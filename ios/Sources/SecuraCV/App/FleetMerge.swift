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

        // What the sender's pipeline sees RIGHT NOW (v2 beacons only) —
        // folded ONLY into the beacon's own row. A two-byte fingerprint
        // suffix may decorate a paired Canary with coarse liveness (the
        // attach() contract below), but it is a hint, never an identity:
        // attributing a semantic claim ("seeing a person") to a named,
        // trusted device on two spoofable bytes would let any nearby sender
        // put words in a paired Canary's mouth. So the claim stays on the
        // row that IS the beacon. It is live state, not a gap-fill: a
        // fresher sighting replaces an older claim. And only positive
        // claims fold — a "none" is indistinguishable from a v1 beacon on
        // the wire, so clearing is time's job: readers age the claim out
        // through `Witness.seeingNow` rather than trusting silence.
        if w.id == sighting.provisionalID,
           let seen = SeenClass(beaconClass: b.detectClass) {
            w.seeingClass = seen
            w.seeingScore = b.detectScore
            w.seeingAt = sighting.lastHeard
        }

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

    // MARK: - BLE chirp (tier 1, momentary)

    /// Fold a heard chirp into a Witness. A chirp is the most transient and
    /// least trustworthy thing on any of these wires — unsigned, checksum-
    /// free, test company id — so it may do exactly two things: prove life
    /// and raise safety-positive attention. Everything else on it is
    /// deliberately unread here: the hour bucket is boot-relative (never a
    /// time of day), and the chain-hash prefix is an existence claim nothing
    /// on this phone can verify.
    static func fold(_ sighting: ChirpSighting, into w: inout Witness) {
        w.seenViaBLE = true
        w.link = .online
        if let existing = w.lastSeen {
            w.lastSeen = max(existing, sighting.lastHeard)
        } else {
            w.lastSeen = sighting.lastHeard
        }
        // Safety-positive only, same as the beacon's tamper flag: hearing a
        // tamper chirp sets tamper; no chirp ever clears anything. The alert
        // ledger's one-alert-per-condition rule already governs how loudly
        // this may repeat.
        if sighting.chirp.kind == .tamper { w.tamper = true }

        // An ALERT chirp is the cry the whole broadcast channel exists for —
        // the display in the next room raises Alert for it, and this fold
        // used to hear it and say nothing. It raises the row's live level
        // the same way an open event does: severity that never DOWNGRADES a
        // louder story already on the row, a headline for the status line,
        // and the existing one-alert-per-condition ledger governing repeats.
        // Heartbeat/witness/boot chirps stay liveness-only — they are proof
        // of life, not cries.
        if sighting.chirp.kind == .alert, w.lastEventSeverity < .alert {
            w.lastEvent = "Alert broadcast heard"
            w.lastEventAt = sighting.lastHeard
            w.lastEventSeverity = .alert
        }
    }

    /// A Canary we have only ever heard chirp — same provisional posture as
    /// the beacon rows, and the same "ble:" namespace, so a device heard
    /// both ways is one row, never a phantom twin.
    static func provisionalWitness(from sighting: ChirpSighting) -> Witness {
        var w = Witness(id: sighting.provisionalID)
        w.name = sighting.displayName
        fold(sighting, into: &w)
        return w
    }

    /// Attach chirps to the fleet under the SAME conservative matching as
    /// beacons: a two-byte suffix decorates a known row only when it picks
    /// out exactly one; ambiguity stands alone rather than putting a tamper
    /// cry in the wrong Canary's mouth.
    static func attach(chirps: [ChirpSighting], to fleet: inout [Witness]) {
        for sighting in chirps {
            let candidates = fleet.indices.filter { i in
                !fleet[i].fingerprint.isEmpty && fleet[i].id != sighting.provisionalID
                    && sighting.chirp.matches(fingerprint: fleet[i].fingerprint)
            }
            if candidates.count == 1 {
                fold(sighting, into: &fleet[candidates[0]])
                continue
            }
            if let i = fleet.firstIndex(where: { $0.id == sighting.provisionalID }) {
                fold(sighting, into: &fleet[i])
                continue
            }
            fleet.append(provisionalWitness(from: sighting))
        }
    }

    // MARK: - /api/fleet self-report (tier 2)

    /// Fold one `/api/fleet` row into a Witness. The device is describing
    /// itself over an unauthenticated endpoint, so this fills identity and
    /// liveness — and stops short of trust.
    ///
    /// `attributed` says the row came from polling the device's OWN base
    /// URL (a paired device's address), not from a name-matched peer row a
    /// hub relayed. Only the seeing claim cares — see its fold below.
    static func fold(_ row: FleetSelfDevice, into w: inout Witness, heardAt: Date = Date(),
                     attributed: Bool = false) {
        if w.name.isEmpty { w.name = row.name }
        if w.deviceType == .unknown { w.deviceType = row.deviceType }
        // The raw product string, kept beside the coarse enum: it is what the
        // figure lookup resolves at full precision, and a row that already
        // has one keeps it (fill gaps, never replace).
        if w.publishedType == nil, !row.product.isEmpty { w.publishedType = row.product }
        // Which board it is, same rule: fill the gap, never replace. This is
        // what the figure lookup asks first, so a row that already carries one
        // (from the mDNS advert) keeps it — two transports reporting the same
        // device must not be able to make its picture flicker.
        if w.hardware == nil, let hw = row.hardware, !hw.isEmpty { w.hardware = hw }
        // The hub standing, which unlike the two above is NOT a fill-the-gap
        // field: it is live state, and the device is the only authority on it.
        // A row that said "none" an hour ago must be replaced the moment the
        // device says "ok", or the app keeps telling the owner to set up a hub
        // they have just finished setting up. Only silence is ignored.
        if row.hubState != .unknown { w.hub = row.hubState }

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

        // The coarse wellbeing words — live state like `hub`, not gap-fill:
        // the device (or the display relaying its retained broker claim) is
        // the only authority, so a present key replaces and silence changes
        // nothing. Presence/health-grade, so they may ride the name-matched
        // fold: the sense device itself published these words, the display
        // only repeats them, and a wrong-row worst case is a coarse
        // someone-is-home on a neighbor row — visible, correctable, and
        // carrying no identity. The seeing claim below is held to more.
        if let p = row.radarPresent { w.radarPresent = p }
        if let o = row.radarOccupants { w.radarOccupants = o }
        if let b = row.breathing { w.breathingLock = b }
        // Stamp when the words arrived, so the detail view can say how
        // fresh the claim is instead of presenting a relayed reading as
        // the present tense forever.
        if row.radarPresent != nil || row.radarOccupants != nil || row.breathing != nil {
            w.wellbeingAt = heardAt
        }

        // The seeing claim is identity-grade — "your camera saw a person"
        // attached to the wrong named row is exactly the lie the attach()
        // rules exist to prevent — so it folds ONLY from an address-
        // attributed poll of the device's own base URL, never through the
        // name-unique peer-row match (names are documented non-unique). Live
        // state with its own timestamp: readers age it out through
        // Witness.seeingNow, the same 120 s the beacon's claim gets.
        if attributed, let seen = row.seenClass {
            w.seeingClass = seen
            w.seeingScore = row.seeingScore
            w.seeingAt = heardAt
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
