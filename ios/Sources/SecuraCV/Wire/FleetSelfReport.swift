// FleetSelfReport.swift
//
// The app-side twin of `firmware/common/fleet_selfreport/fleet_selfreport.h` —
// the coarse JSON every *networked* Canary answers at `GET /api/fleet`. One
// shared builder on the firmware side means one shared shape here; the same
// body is read by the Witness Wall emulator and the Flasher's post-flash LAN
// discovery, so this decoder is the third reader of a written contract, not a
// new guess at one.
//
// Why this matters for the fleet: the app's other HTTP path (`/api/v1/*`,
// DeviceAPI) is the canary-vision device-api contract, which only WAP-class
// devices serve. `/api/fleet` is the FLEET-WIDE surface — canary-display
// already answers it today, and any board that later gains
// `HAS_WIFI && FEATURE_HTTP_SERVER` gets picked up with no app change. That is
// the anti-rot bet: teach the app the shared contract, not the per-board one.
//
// WIRE SHAPE (from fleet_selfreport_build / _append_device):
//
//   {"kernel":"<name>","verified_through":"now",
//    "devices":[{"name":"…","online":true,"chain":"ok","product":"canary-wap"
//                [,"chain_height":42]}]}
//
// A hub may append additional peer rows before closing the array, so `devices`
// is always treated as a list — never assumed to hold exactly one entry.
//
// PURE: Foundation only. Host-testable (FleetSelfReportTests) with no network.

import Foundation

/// One row of `/api/fleet` — a device's coarse, non-extractive self-state.
/// Presence and health only; never raw media.
struct FleetSelfDevice: Codable, Hashable, Sendable {
    var name: String
    var online: Bool
    /// "ok" when the witness chain verifies; "unknown" when degraded or not yet
    /// known. Kept as a raw string plus a computed verdict so a NEW value from
    /// newer firmware degrades to `.unknown` instead of failing the decode —
    /// the same tolerance the enum decoders elsewhere in the app apply.
    var chain: String
    var product: String
    var chainHeight: Int?
    /// The day this device's KEY was born, in days since the Unix epoch — a
    /// fact about the Canary, not about whoever paired it. Nil when the device
    /// has never met a believable clock, or has no witness key of its own (a
    /// display pins other devices' keys and has no birth to report). The
    /// firmware omits the field entirely rather than sending 0, so nil here is
    /// "not known" and can never be rendered as 1970.
    var bornDay: Int?
    /// False means the day above is when the device was FIRST DATED, not when
    /// it was born — a Canary flashed in a workshop and plugged in a week later
    /// first learns the date a week late. The app must not call that a
    /// birthday. See firmware/common/identity/birth_day.h.
    var bornExact: Bool
    /// WHICH BOARD this is (`hw`) — the `boards/<id>/pins` header the build
    /// compiled against, as `CANARY_FIGURE_HARDWARE` spells it. Nil when the
    /// device omitted it, which is every device on firmware older than the
    /// field and every build with no pins header of its own.
    ///
    /// It is here because `product` cannot answer the question it answers.
    /// Several products share one device type, and one board can serve two
    /// products, so only this pins down the SHAPE — it is what lets the app
    /// draw the actual hardware instead of a generic marker. It names the
    /// BOARD, never the product.
    var hardware: String?
    /// Where this device stands with its hub, verbatim (`hub`). Kept as a raw
    /// string with a computed verdict, exactly like `chain` beside it, so a
    /// word from newer firmware degrades to `.unknown` instead of failing the
    /// decode.
    var hub: String
    /// The coarse wellbeing words a hub-shaped row MAY carry — a display's
    /// /api/fleet aggregation relaying a sense peer's own retained MQTT
    /// claim. Kept verbatim (words, per the wire's fallback rule) with
    /// folded verdicts below. nil = the row did not say — which a reader
    /// must never render as an empty calm room.
    var presence: String?      // "clear" | "present"
    var occupants: String?     // "0" | "1" | "2+"
    var breathing: Bool?       // a breathing lock held (true) or lapsed (false)
    /// What the device's own pipeline reports seeing — the BLE v2 beacon's
    /// class vocabulary given an HTTP spelling. No firmware fills it yet
    /// (vision has the classifier but no HTTP server); decoded data-first so
    /// every reader lights up the day a producer speaks, with no app change.
    var seeing: String?        // "person"|"vehicle"|"animal"|"package"
    var seeingScore: Int?      // 1–100; nil when the wire didn't score it

    enum CodingKeys: String, CodingKey {
        case name, online, chain, product
        case chainHeight = "chain_height"
        case bornDay = "born_day"
        case bornExact = "born_exact"
        case hardware = "hw"
        case hub
        case presence, occupants, breathing, seeing
        case seeingScore = "seeing_score"
    }

    /// Tolerant decode: a device that omits a field is reported, not dropped.
    /// A Canary that answers at all is telling us something worth showing.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name        = (try? c.decode(String.self, forKey: .name)) ?? ""
        online      = (try? c.decode(Bool.self, forKey: .online)) ?? false
        chain       = (try? c.decode(String.self, forKey: .chain)) ?? ""
        product     = (try? c.decode(String.self, forKey: .product)) ?? ""
        chainHeight = try? c.decodeIfPresent(Int.self, forKey: .chainHeight)
        // A day of 0 or less is the epoch showing through, not a date. Folded
        // to nil here so exactly one representation of "not known" reaches the
        // UI, whichever way an older or stranger firmware phrased it.
        let day = (try? c.decodeIfPresent(Int.self, forKey: .bornDay)) ?? nil
        bornDay = (day ?? 0) > 0 ? day : nil
        // Absent means not exact. A firmware that reports a day but no verdict
        // has not earned the word "born" — the cautious reading is the only
        // safe one, because the flag exists to hold back a claim.
        bornExact = ((try? c.decodeIfPresent(Bool.self, forKey: .bornExact)) ?? nil) ?? false
        // Empty and absent are one answer here ("this device cannot say"),
        // folded to nil so a caller never spends a map lookup proving that an
        // empty string is not a board id.
        let hw = (try? c.decodeIfPresent(String.self, forKey: .hardware)) ?? nil
        hardware = (hw?.isEmpty == false) ? hw : nil
        hub = (try? c.decode(String.self, forKey: .hub)) ?? ""
        // The wellbeing words, verbatim-or-nil; empty folds to nil like hw.
        let pres = (try? c.decodeIfPresent(String.self, forKey: .presence)) ?? nil
        presence = (pres?.isEmpty == false) ? pres : nil
        let occ = (try? c.decodeIfPresent(String.self, forKey: .occupants)) ?? nil
        occupants = (occ?.isEmpty == false) ? occ : nil
        breathing = (try? c.decodeIfPresent(Bool.self, forKey: .breathing)) ?? nil
        let see = (try? c.decodeIfPresent(String.self, forKey: .seeing)) ?? nil
        seeing = (see?.isEmpty == false) ? see : nil
        // The firmware only writes a score in 1…100 beside a seeing word;
        // anything else reaching us is a stranger's phrasing, folded to nil
        // (unscored) rather than rendered as a confidence.
        let score = (try? c.decodeIfPresent(Int.self, forKey: .seeingScore)) ?? nil
        seeingScore = ((score ?? 0) > 0 && (score ?? 0) <= 100) ? score : nil
    }

    init(name: String, online: Bool, chain: String, product: String, chainHeight: Int? = nil,
         bornDay: Int? = nil, bornExact: Bool = false, hardware: String? = nil,
         hub: String = "", presence: String? = nil, occupants: String? = nil,
         breathing: Bool? = nil, seeing: String? = nil, seeingScore: Int? = nil) {
        self.name = name
        self.online = online
        self.chain = chain
        self.product = product
        self.chainHeight = chainHeight
        self.bornDay = bornDay
        self.bornExact = bornExact
        self.hardware = hardware
        self.hub = hub
        self.presence = presence
        self.occupants = occupants
        self.breathing = breathing
        self.seeing = seeing
        self.seeingScore = seeingScore
    }

    /// True only for the explicit "ok". Anything else — "unknown", "degraded",
    /// a value this app has never heard of — is NOT a verified chain. The
    /// safe direction is the only direction: never render trust we weren't told.
    var chainVerifies: Bool { chain == "ok" }

    /// The device type the OTA product string names, folded through the same
    /// tolerant decoder the rest of the app uses so an unknown product renders
    /// as `.unknown` rather than being dropped.
    var deviceType: DeviceType { DeviceType(tolerant: product) }

    /// This device's hub standing, folded to the app's enum. Anything this
    /// build has never heard of — including silence — is `.unknown`, which
    /// renders as nothing rather than as reassurance.
    var hubState: HubState { HubState(tolerant: hub) }

    /// The radar presence claim as the model holds it: true = present,
    /// false = clear, nil = the row did not say — and nil also for a word
    /// this build has never heard, because a future word must not read as
    /// either answer.
    var radarPresent: Bool? {
        switch presence {
        case "present": return true
        case "clear": return false
        default: return nil
        }
    }

    /// "0"/"1"/"2+" folded to the model's 0/1/2 occupant shape; nil for
    /// silence and unknown words alike.
    var radarOccupants: Int? {
        switch occupants {
        case "0": return 0
        case "1": return 1
        case "2+": return 2
        default: return nil
        }
    }

    /// The seeing word folded to the app's SeenClass — nil for silence and
    /// for any word outside the vocabulary (a face or plate class here is a
    /// rejected PR, not a render).
    var seenClass: SeenClass? { seeing.flatMap(SeenClass.init(rawValue:)) }
}

/// The whole `/api/fleet` body.
struct FleetSelfReport: Codable, Hashable, Sendable {
    /// The answering device's own name (a hub names itself here, then lists
    /// itself plus its peers in `devices`).
    var kernel: String
    /// Coarse freshness word from the device — "now" in the current contract.
    /// Kept verbatim rather than parsed into a Date: Invariant III says the app
    /// never shows a precise timestamp for an event.
    var verifiedThrough: String
    var devices: [FleetSelfDevice]

    enum CodingKeys: String, CodingKey {
        case kernel, devices
        case verifiedThrough = "verified_through"
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        kernel          = (try? c.decode(String.self, forKey: .kernel)) ?? ""
        verifiedThrough = (try? c.decode(String.self, forKey: .verifiedThrough)) ?? ""
        devices         = (try? c.decode([FleetSelfDevice].self, forKey: .devices)) ?? []
    }

    init(kernel: String, verifiedThrough: String = "now", devices: [FleetSelfDevice]) {
        self.kernel = kernel
        self.verifiedThrough = verifiedThrough
        self.devices = devices
    }

    static func decode(_ data: Data) throws -> FleetSelfReport {
        try JSONDecoder().decode(FleetSelfReport.self, from: data)
    }
}
