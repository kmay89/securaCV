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

    /// What the card calls it: "Canary Sense AB12", or the raw name — the
    /// family word, not the role ("Radar witness AB12" named the job, not
    /// the product in the box). One spelling, NearbyCanaries.title, shared
    /// with the Fleet row a heard-only Canary keeps after its door shuts.
    var title: String {
        if let family {
            return NearbyCanaries.title(family: family, suffix: suffix)
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
        guard isSuffix(suffix) else { return nil }
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

    /// Four characters of 0-9 / A-Z, as the firmware spells a suffix.
    private static func isSuffix(_ suffix: String) -> Bool {
        suffix.count == 4 && suffix.allSatisfy { ("0"..."9").contains($0) || ("A"..."Z").contains($0) }
    }

    /// What every surface calls a Canary heard by its door: "Canary Sense
    /// K7MZ", "Canary Vision K7MZ", "Canary WAP AB12" — the family word and
    /// the four characters its setup network and mDNS host share.
    static func title(family: CanaryFamily, suffix: String) -> String {
        "Canary \(family.word) \(suffix)"
    }

    /// `title(family:suffix:)` read back: "Canary Sense K7MZ" → (.sense,
    /// "K7MZ"). A Fleet row the door named keeps this title after the door
    /// shuts (FleetMerge), and the dedupe against the same unit's mDNS row
    /// has to recover the suffix from it. As strict as `parseName`.
    static func parseTitle(_ title: String) -> (family: CanaryFamily, suffix: String)? {
        let parts = title.split(separator: " ", omittingEmptySubsequences: false)
        guard parts.count == 3, parts[0] == "Canary" else { return nil }
        let suffix = String(parts[2])
        guard isSuffix(suffix),
              let family = CanaryFamily.allCases.first(where: { $0.word == parts[1] && $0.hasBluetoothDoor })
        else { return nil }
        return (family, suffix)
    }

    /// Either spelling a door leaves behind — the advert's own
    /// ("Sense-K7MZ") or the title a row wears ("Canary Sense K7MZ").
    static func parseDoorName(_ name: String) -> (family: CanaryFamily, suffix: String)? {
        parseName(name) ?? parseTitle(name)
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
    /// "In the room" — the band under "close by". The gate for a brand-new
    /// owner's one Canary: a WAP powered on in its final spot across the
    /// room is the only Canary in earshot of a phone with no fleet yet, so
    /// there is no ambiguity for the nearer gate to protect.
    static let inTheRoomDBM = -85

    /// How strong a Canary must be heard before the card comes by itself.
    /// With no real fleet and exactly one candidate, "in the room" is
    /// enough; otherwise "close by", so two Canaries on one table — or a
    /// neighbor's new one through the wall of a house that already has a
    /// fleet — never get guessed at.
    static func offerThreshold(candidateCount: Int, hasRealFleet: Bool) -> Int {
        (!hasRealFleet && candidateCount == 1) ? inTheRoomDBM : closeEnoughDBM
    }

    /// Which one Canary, if any, the app should offer by itself — the card
    /// that slides up without being asked for. Only when exactly ONE
    /// candidate the person has not already been offered is close enough
    /// (`offerThreshold`): two on the table is a choice, and choices belong
    /// to the list (the inline card, where each row has its own Blink and
    /// Set up), never to a card that guesses. Once offered — answered or
    /// waved away — a suffix is not offered again until its door has been
    /// off the air for a while and come back (NearbyOfferLedger); the inline
    /// card still lists it.
    static func autoOffer(_ candidates: [NearbyCanary],
                          alreadyOffered: Set<String>,
                          hasRealFleet: Bool = true) -> NearbyCanary? {
        let threshold = offerThreshold(candidateCount: candidates.count, hasRealFleet: hasRealFleet)
        let close = candidates.filter { $0.rssiDBM >= threshold && !alreadyOffered.contains($0.suffix) }
        return close.count == 1 ? close[0] : nil
    }

    // MARK: - the one notification the background scan may post

    /// With the app in the background the phone keeps a narrower scan
    /// running (BLEConsole's door scan) and, for a Canary it would have
    /// offered, posts one local notification — once per suffix per session,
    /// and only when notifications are already allowed (the ask stays with
    /// the first Test Alert; nothing here prompts). Tapping it opens the
    /// app, where the foreground scan hears the door again and the card
    /// comes up as it would have: the background sighting is never counted
    /// as offered.
    static let noticeTitle = "A new Canary is nearby"
    static let noticeThreadID = "nearby-canary"

    static func noticeBody(for canary: NearbyCanary) -> String {
        "\(canary.title) is ready for your Wi-Fi. Tap to set it up."
    }

    /// Which candidate earns the notification: the first not yet noticed.
    static func backgroundNotice(for candidates: [NearbyCanary],
                                 alreadyNoticed: Set<String>) -> NearbyCanary? {
        candidates.first { !alreadyNoticed.contains($0.suffix) }
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

    /// The four identity characters in a setup network's name, read by the
    /// grammar of the firmware's `onboard::is_setup_ssid`
    /// (firmware/common/network/onboarding_fsm.h; SetupPortal.isSetupNetwork
    /// is its public twin and asks here): "SecuraCV-sense-aB3k" → "aB3k",
    /// "SecuraCV-sentinel-aB3k-x9" → "aB3k", and older firmware's
    /// "SecuraCV-aB3k" / "SecuraCV-aB3k-x9" → "aB3k". The node type in front
    /// (1...8 lowercase ASCII letters) is not identity, nor is the two-
    /// character tag behind (a key that could not be made durable). Nil for
    /// anything that is not a Canary's setup network — ASCII only, as the
    /// firmware reads it.
    static func suffix(ofSetupSSID ssid: String) -> String? {
        let prefix = "SecuraCV-"
        guard ssid.hasPrefix(prefix) else { return nil }
        let parts = ssid.dropFirst(prefix.count).split(separator: "-", omittingEmptySubsequences: false)
        func alnum(_ s: Substring, _ n: Int) -> Bool {
            s.count == n && s.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber) }
        }
        func nodeType(_ s: Substring) -> Bool {
            (1...8).contains(s.count) && s.allSatisfy { $0.isASCII && $0.isLetter && $0.isLowercase }
        }
        switch parts.count {
        case 1 where alnum(parts[0], 4):
            return String(parts[0])                                           // SecuraCV-aB3k
        case 2 where alnum(parts[0], 4) && alnum(parts[1], 2):
            return String(parts[0])                                           // SecuraCV-aB3k-x9
        case 2 where nodeType(parts[0]) && alnum(parts[1], 4):
            return String(parts[1])                                           // SecuraCV-sense-aB3k
        case 3 where nodeType(parts[0]) && alnum(parts[1], 4) && alnum(parts[2], 2):
            return String(parts[1])                                           // SecuraCV-sense-aB3k-x9
        default:
            return nil
        }
    }
}

/// The session's memory behind the card that comes to you: which Canaries
/// it has already offered, which the person waved away — and when it may
/// speak again. "Not now" and "already offered" hold while the Canary's
/// door stays on the air. A suffix whose open door has been off the air for
/// longer than the freshness window and then returns is a Canary somebody
/// just replugged (a new first boot — the walkthrough tells people to), or
/// one whose signal wobbled out and back, so both memories lapse for it and
/// the card may come once more. Pure; FleetStore drives it from the scan.
struct NearbyOfferLedger: Equatable, Sendable {
    /// Offered by the card that slides up (answered or not).
    private(set) var offered: Set<String> = []
    /// Waved away with "Not now" — the inline card stays quiet for these too.
    private(set) var dismissed: Set<String> = []
    /// When each remembered suffix was last heard with its door open.
    private var lastOnAir: [String: Date] = [:]

    /// Everything the pop-up card must not offer right now.
    var suppressed: Set<String> { offered.union(dismissed) }

    mutating func noteOffered(_ suffix: String, now: Date = Date()) {
        offered.insert(suffix)
        lastOnAir[suffix] = now
    }

    mutating func noteDismissed(_ suffix: String, now: Date = Date()) {
        dismissed.insert(suffix)
        lastOnAir[suffix] = now
    }

    /// Feed it the suffixes whose door is open right now (the candidates
    /// with nothing dismissed — a dismissed Canary on the air is still on
    /// the air). A remembered suffix absent for longer than `gap` is
    /// forgotten, so its next appearance is a fresh offer.
    mutating func observe(onAir: Set<String>, now: Date, gap: TimeInterval = NearbyCanaries.freshFor) {
        for suffix in onAir { lastOnAir[suffix] = now }
        for suffix in suppressed where !onAir.contains(suffix) {
            guard let last = lastOnAir[suffix] else {
                lastOnAir[suffix] = now
                continue
            }
            if now.timeIntervalSince(last) > gap {
                offered.remove(suffix)
                dismissed.remove(suffix)
                lastOnAir[suffix] = nil
            }
        }
    }
}

/// The card that slides up by itself, as a value: which Canary it holds and
/// whether it is drawn right now. `NearbyOfferOverlay` is a view over this,
/// so every rule it keeps is a line in `NearbyCanaryTests`.
///
/// The card latches a Canary when it first appears and counts it as offered
/// right then (the caller passes the suffix `latch` returns to the store), so
/// a card that is ignored, or whose signal wobbles across "close enough",
/// neither flickers nor slides up a second time this session. It holds that
/// Canary until it is answered or the Canary drops out of earshot entirely.
///
/// It stands down while a sheet that lists the same Canaries inline is up —
/// the Fleet tab's "+" sheet, whose `.medium` detent would otherwise leave
/// the card showing dimmed behind it, offering what the sheet already lists
/// (sweep A75): nothing is drawn and nothing new latches (so nothing is
/// counted as offered behind a sheet). A card already latched comes back
/// when the sheet closes, if its Canary is still in earshot and unanswered.
struct NearbyOfferLatch: Equatable, Sendable {
    private(set) var latched: NearbyCanary?

    init() {}

    /// What the card draws: the latched Canary, or nothing while it stands down.
    func visible(standingDown: Bool) -> NearbyCanary? {
        standingDown ? nil : latched
    }

    /// Latch the policy's offer (`NearbyCanaries.autoOffer`) when nothing is
    /// latched, no setup sheet is open and the card is not standing down.
    /// Returns the suffix to count as offered, or nil when nothing latched.
    mutating func latch(_ offer: NearbyCanary?, setupOpen: Bool, standingDown: Bool) -> String? {
        guard latched == nil, !setupOpen, !standingDown, let offer else { return nil }
        latched = offer
        return offer.suffix
    }

    /// Continue or Not now: the card lets go (its suffix stays offered).
    mutating func answer() {
        latched = nil
    }

    /// The suffixes still heard: a latched Canary missing from them left
    /// earshot, and the card lets go of it.
    mutating func heard(_ suffixes: [String]) {
        if let c = latched, !suffixes.contains(c.suffix) { latched = nil }
    }
}
