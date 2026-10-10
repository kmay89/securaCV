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
// local name of the form "<Family>-<4 chars>" — on a Sense or Vision the
// first four characters of its device pseudonym, upper-cased (the token is
// drawn from the 54-character no-confusion alphabet, so "Sense-K7MZ" is as
// real as "Sense-AB12"; its SecuraCV-XXXX setup network and its mDNS host
// name carry the same characters in their own case); on a WAP the last four
// hex of its key fingerprint, as its SCV-XXXX name — and its scan response
// carries the Improv Wi-Fi service UUID with the standard's service data
// (state and capabilities). A phone needs the UUID to be sure the device
// will take credentials; the beacon bit alone is a hint.

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

    /// Parse an on-air name "Sense-K7MZ" / "Vision-AB12" / "WAP-AB12" into
    /// the family and the suffix. Strict: the family word must be one the
    /// walkthrough knows, the suffix exactly four characters of 0-9 / A-Z,
    /// upper case as the firmware spells it (a Sense or Vision upper-cases
    /// its pseudonym, which is not hex — an earlier hex-only rule turned
    /// nearly every real Sense into a nameless "Sense-K7MZ" with no family
    /// and no figure). Anything else is not ours to interpret.
    static func parseName(_ name: String) -> (family: CanaryFamily, suffix: String)? {
        let parts = name.split(separator: "-", maxSplits: 1, omittingEmptySubsequences: false)
        guard parts.count == 2 else { return nil }
        let word = String(parts[0]), suffix = String(parts[1])
        guard suffix.count == 4,
              suffix.allSatisfy({ ("0"..."9").contains($0) || ("A"..."Z").contains($0) }) else { return nil }
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

    // MARK: - the card that comes to you

    /// The signal a Canary must reach before the app offers it on its own,
    /// AirPods-style: "close by" or nearer (the same band the card calls
    /// "close by"), i.e. on the table next to the phone, not two rooms away.
    static let closeEnoughDBM = -70

    /// Which one Canary, if any, the app should offer by itself — the card
    /// that slides up without being asked for. Only when exactly ONE
    /// candidate the person has not already been offered is close enough:
    /// two on the table is a choice, and choices belong to the list (the
    /// inline card, where each row has its own Blink and Set up), never to a
    /// card that guesses. Once offered — answered or waved away — a suffix
    /// is not offered again this session; the inline card still lists it.
    static func autoOffer(_ candidates: [NearbyCanary],
                          alreadyOffered: Set<String>,
                          closeEnough: Int = closeEnoughDBM) -> NearbyCanary? {
        let close = candidates.filter { $0.rssiDBM >= closeEnough && !alreadyOffered.contains($0.suffix) }
        return close.count == 1 ? close[0] : nil
    }

    // MARK: - is that THIS Canary?

    /// Does a device seen on the network (its mDNS `host`, e.g.
    /// "canary-vision-001-k7mzq2") belong to the Canary whose door was
    /// heard as "Vision-K7MZ"? The firmware builds both from one pseudonym:
    /// the host's last hyphen-separated label is its first six characters,
    /// the on-air name's suffix its first four, upper-cased — and a
    /// display's setup network SecuraCV-XXXX its first four as they are.
    /// So the match is a case-insensitive prefix of the host's last label.
    /// A host-less advert matches nothing: a family alone never proves
    /// which unit joined (a Vision you already own is not the new one).
    static func isSameDevice(suffix: String, host: String?) -> Bool {
        let want = suffix.lowercased()
        guard want.count == 4, let host, !host.isEmpty else { return false }
        let bare = host.lowercased().hasSuffix(".local") ? String(host.lowercased().dropLast(6)) : host.lowercased()
        guard let label = bare.split(separator: "-").last, label.count >= want.count else { return false }
        return label.hasPrefix(want)
    }

    /// The four identity characters in a setup network's name:
    /// "SecuraCV-aB3k" → "aB3k", and "SecuraCV-aB3k-x9" (a key that could
    /// not be made durable) → "aB3k". Nil for anything that is not a
    /// Canary's setup network.
    static func suffix(ofSetupSSID ssid: String) -> String? {
        let prefix = "SecuraCV-"
        guard ssid.hasPrefix(prefix) else { return nil }
        let rest = ssid.dropFirst(prefix.count)
        guard let first = rest.split(separator: "-", omittingEmptySubsequences: false).first,
              first.count == 4, first.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber) }) else { return nil }
        return String(first)
    }
}
