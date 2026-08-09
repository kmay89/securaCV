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

    enum CodingKeys: String, CodingKey {
        case name, online, chain, product
        case chainHeight = "chain_height"
        case bornDay = "born_day"
        case bornExact = "born_exact"
        case hardware = "hw"
        case hub
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
    }

    init(name: String, online: Bool, chain: String, product: String, chainHeight: Int? = nil,
         bornDay: Int? = nil, bornExact: Bool = false, hardware: String? = nil,
         hub: String = "") {
        self.name = name
        self.online = online
        self.chain = chain
        self.product = product
        self.chainHeight = chainHeight
        self.bornDay = bornDay
        self.bornExact = bornExact
        self.hardware = hardware
        self.hub = hub
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
}

/// Where a Canary stands with its MQTT broker.
///
/// Three states rather than a flag, because "nobody has given me a hub" and
/// "my hub is unreachable" want different sentences and different fixes, and
/// collapsing them would send an owner to restart a hub they never set up.
enum HubState: String, Codable, Sendable {
    /// No broker configured — this device is standing alone because nobody
    /// has pointed it at one yet. The state that wants an explanation.
    ///
    /// Spelled `absent`, not `none`, though the wire word IS "none". A case
    /// literally named `none` collides with `Optional.none` at every optional
    /// call site: `XCTAssertEqual(row?.hubState, .none)` compares against nil
    /// and quietly passes for the wrong reason, and `hub == .none` on an
    /// optional means "is nil". The rawValue keeps the contract with the
    /// firmware; the case name keeps the ambiguity out of the app.
    case absent = "none"
    /// A broker is configured and unreachable.
    case down
    case ok
    /// Not reported. Never rendered as "fine": a device that didn't say is a
    /// device we know nothing about.
    case unknown

    init(tolerant raw: String?) { self = HubState(rawValue: raw ?? "") ?? .unknown }

    /// Does this state want the owner to go and do something?
    var needsAttention: Bool { self == .absent || self == .down }
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
