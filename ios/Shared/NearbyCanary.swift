// NearbyCanary.swift  (SHARED — pure Foundation)
//
// "A new Canary is nearby." The pure policy behind the card the app shows
// when a Canary with its Bluetooth setup door open is in range: which
// sightings qualify, what to call them, which family they are, and when the
// card should stay quiet. No CoreBluetooth here — the transport hands in
// what it heard, this decides, the view draws.
//
// What a Canary says while its door is open (firmware/common/network/
// improv_ble): its fleet beacon carries FLEET_BEACON_FLAG_SETUP_OPEN and a
// local name of the form "<Family>-<4 hex>" — on a Sense or Vision the same
// four characters as its SecuraCV-XXXX setup network; on a WAP the last four
// hex of its key fingerprint, as its SCV-XXXX name — and its scan response carries the
// Improv Wi-Fi service UUID with the standard's service data (state and
// capabilities). A phone needs the UUID to be sure the device will take
// credentials; the beacon bit alone is a hint.

import Foundation

/// One Canary heard with its setup door open.
struct NearbyCanary: Identifiable, Hashable, Sendable {
    /// CoreBluetooth's local handle for the peripheral — what the transport
    /// connects to. Never an identity to trust (see BeaconSighting).
    var peripheralID: UUID
    var id: UUID { peripheralID }
    /// The family the on-air name claims, when it follows the grammar.
    var family: CanaryFamily?
    /// The four-character suffix the name and the setup network share
    /// ("AB12"), or the beacon's fingerprint suffix when the name is absent.
    var suffix: String
    /// The name as advertised, or the beacon's "SCV-XXXX" fallback.
    var displayName: String
    var rssiDBM: Int
    var lastHeard: Date
    /// Improv's own state byte from the service data, when it was heard.
    var improvState: ImprovWire.State?
    /// Capabilities from the service data: can it blink to identify itself,
    /// can it list the networks it sees.
    var canIdentify: Bool
    var canScanWiFi: Bool

    /// What the card calls it: "Canary Sense AB12", or the raw name.
    var title: String {
        if let family {
            return "\(family.deviceType.role) \(suffix)"
        }
        return displayName
    }
}

enum NearbyCanaries {
    /// A sighting older than this is not "nearby" any more — the beacon
    /// repeats every few seconds, so fifteen seconds of silence is a device
    /// that left, or shut its door (the scan response stops coming with it).
    static let freshFor: TimeInterval = 15

    /// Parse an on-air name "Sense-AB12" / "Vision-AB12" / "WAP-AB12" into
    /// the family and the suffix. Strict: the family word must be one the
    /// walkthrough knows, the suffix exactly four hex characters, upper case
    /// as the firmware spells it. Anything else is not ours to interpret.
    static func parseName(_ name: String) -> (family: CanaryFamily, suffix: String)? {
        let parts = name.split(separator: "-", maxSplits: 1, omittingEmptySubsequences: false)
        guard parts.count == 2 else { return nil }
        let word = String(parts[0]), suffix = String(parts[1])
        guard suffix.count == 4,
              suffix.allSatisfy({ ("0"..."9").contains($0) || ("A"..."F").contains($0) }) else { return nil }
        let family: CanaryFamily
        switch word {
        case "Sense": family = .sense
        case "Vision": family = .vision
        case "WAP": family = .wap
        case "Canary": family = .wap   // the flagship build pairs like a WAP
        default: return nil
        }
        return (family, suffix)
    }

    /// What one scan result contributed: the pieces the transport collects
    /// per peripheral across its advert and scan response.
    struct Heard: Hashable, Sendable {
        var peripheralID: UUID
        var beacon: FleetBeacon?
        var localName: String?
        /// The Improv service UUID was in the advertised service list.
        var improvAdvertised: Bool
        /// The 0x4677 service data, when present.
        var improvServiceData: ImprovWire.ServiceData?
        var rssiDBM: Int
        var lastHeard: Date
    }

    /// Fold what was heard into the candidates the card may show.
    ///
    /// A device qualifies when it was heard within `freshFor` of `now` AND
    /// it advertises the Improv service with a state that accepts
    /// credentials (authorized or provisioning), OR — for a firmware whose
    /// service data was not caught this time — its beacon says the setup
    /// door is open. A suffix that matches a paired device's fingerprint is
    /// excluded only when the match is unambiguous (two devices sharing a
    /// suffix is exactly when the card must still offer both); one the
    /// person dismissed stays quiet. Strongest signal first.
    static func candidates(from heard: [Heard],
                           pairedFingerprints: [String],
                           dismissedSuffixes: Set<String>,
                           now: Date) -> [NearbyCanary] {
        let cutoff = now.addingTimeInterval(-freshFor)
        var out: [NearbyCanary] = []
        for h in heard where h.lastHeard >= cutoff {
            let open: Bool
            if let sd = h.improvServiceData {
                open = sd.state.isOpen
            } else if h.improvAdvertised {
                open = true
            } else {
                open = h.beacon?.setupOpen == true
            }
            guard open else { continue }

            let parsed = h.localName.flatMap(parseName)
            let suffix = parsed?.suffix ?? h.beacon?.fingerprintSuffix.uppercased() ?? ""
            guard !suffix.isEmpty else { continue }
            if dismissedSuffixes.contains(suffix) { continue }
            if let beacon = h.beacon {
                let matches = pairedFingerprints.filter { beacon.matches(fingerprint: $0) }
                if matches.count == 1 { continue }   // ours already, door open for a tap: not "new"
            }
            out.append(NearbyCanary(
                peripheralID: h.peripheralID,
                family: parsed?.family,
                suffix: suffix,
                displayName: h.localName ?? h.beacon?.provisionalName ?? "SCV-\(suffix)",
                rssiDBM: h.rssiDBM,
                lastHeard: h.lastHeard,
                improvState: h.improvServiceData?.state,
                canIdentify: h.improvServiceData?.canIdentify ?? false,
                canScanWiFi: h.improvServiceData?.canScanWiFi ?? false))
        }
        return out.sorted { a, b in
            if a.rssiDBM != b.rssiDBM { return a.rssiDBM > b.rssiDBM }
            return a.displayName < b.displayName
        }
    }
}
