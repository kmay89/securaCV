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

    enum CodingKeys: String, CodingKey {
        case name, online, chain, product
        case chainHeight = "chain_height"
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
    }

    init(name: String, online: Bool, chain: String, product: String, chainHeight: Int? = nil) {
        self.name = name
        self.online = online
        self.chain = chain
        self.product = product
        self.chainHeight = chainHeight
    }

    /// True only for the explicit "ok". Anything else — "unknown", "degraded",
    /// a value this app has never heard of — is NOT a verified chain. The
    /// safe direction is the only direction: never render trust we weren't told.
    var chainVerifies: Bool { chain == "ok" }

    /// The device type the OTA product string names, folded through the same
    /// tolerant decoder the rest of the app uses so an unknown product renders
    /// as `.unknown` rather than being dropped.
    var deviceType: DeviceType { DeviceType(tolerant: product) }
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
