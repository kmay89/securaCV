//  WitnessCore.swift — the Swift side of the Rust core's C ABI.
//
//  Every unsafe pointer in this app lives in this one file. Call sites get
//  value types and `throws`; nobody else has to remember to free anything.
//
//  The C contract (tvos/witness-core/include/securacv_witness_core.h): JSON in,
//  JSON out, and every returned string must go back to `scv_string_free`
//  exactly once. `scv_core_version` returns static storage that must NOT be
//  freed — the one asymmetry, handled here and nowhere else.

import Foundation
import SecuraCVWitnessCore

/// The verdict the Wall renders. Mirrors Rust's `VerifyReport` — the JSON is
/// the contract, so adding a field on one side can't desync a struct layout.
struct VerifyReport: Decodable, Equatable, Sendable {
    enum FailureKind: String, Decodable, Sendable {
        case prevHashMismatch = "prev_hash_mismatch"
        case entryHashMismatch = "entry_hash_mismatch"
        case signatureMismatch = "signature_mismatch"
        case malformed
    }

    let ok: Bool
    let verified: UInt64
    let head: String
    let failedAt: Int64?
    let kind: FailureKind?
    let detail: String?
    let message: String

    enum CodingKeys: String, CodingKey {
        case ok, verified, head, kind, detail, message
        case failedAt = "failed_at"
    }
}

/// A fleet snapshot, normalized by Rust from `GET /api/fleet`
/// (tvos/discovery/DISCOVERY.md).
struct FleetSnapshot: Decodable, Equatable, Sendable {
    struct Device: Decodable, Equatable, Sendable, Identifiable {
        let name: String
        let online: Bool
        let chain: String?
        let product: String?
        /// WHICH BOARD this is, as the device published it (`hw`). The only
        /// field that pins the device's SHAPE — several products share one
        /// `product` string — so it is what resolves the figure the Wall
        /// draws. Nil on firmware older than the field.
        ///
        /// Defaulted, unlike the fields above it, so that adding an optional
        /// wire field does not break every construction site — which is
        /// exactly what adding these two did to ResidentWatchTests. A field
        /// the device may omit should be a field a caller may omit.
        var hw: String? = nil
        /// Where the device stands with its hub ("none" / "down" / "ok"), or
        /// nil when it did not say. Never rendered as "fine" when nil.
        var hub: String? = nil

        /// Stable within a snapshot: the fleet endpoint has no device ids, and
        /// names are what a person actually distinguishes Canaries by.
        var id: String { name }

        /// Does this device's own report describe a chain FAILURE?
        ///
        /// Three answers, not two — and the missing third one was a lie the
        /// Wall told about every display in the fleet. A display holds no
        /// witness chain of its own (it renders other devices'), so it
        /// honestly answers "unknown"; the old `!= "ok"` test painted it
        /// orange with "Record didn't verify" and counted it toward the
        /// needs-attention total. An absent claim is not a broken chain, and
        /// the explicit "unknown" is exactly as absent as a missing field.
        ///
        /// Mirrors `Device::chain_is_troubled` in witness-core's fleet.rs; the
        /// Rust tests and these must agree, since the same bytes reach both.
        var chainIsTroubled: Bool {
            guard let chain else { return false }
            return chain != "ok" && chain != "unknown"
        }

        /// The product name to show a person, resolved the same way the iPhone
        /// resolves it — board first (it is sometimes more product-precise
        /// than the type), then the shipped-product table. Nil when nothing
        /// pins a product, and the caller then says something coarse rather
        /// than printing the wire string.
        ///
        /// The Wall used to print `product` raw, so a television showed
        /// "canary-nightstand7" where the phone showed "Canary Nightstand 7".
        var productName: String? {
            DeviceNaming.productName(published: product, hardware: hw)
        }

        /// This device's hub standing, folded through the same enum the phone
        /// uses. Silence is `.unknown`, which renders as nothing.
        var hubState: HubState { HubState(tolerant: hub) }

        /// The coarse device family, decoded with the shared tolerant decoder
        /// so the Wall and the phone can never disagree about what a device
        /// IS — including the whole display line, which a strict rawValue
        /// lookup drops to `.unknown`.
        var deviceType: DeviceType { DeviceType(tolerant: product) }

        /// The figure to draw, at the same three honest precisions the phone
        /// uses: board, then published type, then the coarse family. Nil means
        /// draw the generic marker — never a guess.
        var figure: FleetFigure? {
            FleetFigure.resolve(deviceType: deviceType, published: product, hardware: hw)
        }
    }

    let kernel: String?
    let verifiedThrough: String?
    let devices: [Device]

    enum CodingKeys: String, CodingKey {
        case kernel, devices
        case verifiedThrough = "verified_through"
    }

    var onlineCount: Int { devices.filter(\.online).count }
    var hasChainTrouble: Bool { devices.contains(where: \.chainIsTroubled) }

    /// The line under the fleet name. "fleet"/"your Canaries" only — never the
    /// other group noun (CLAUDE.md).
    var summary: String {
        guard !devices.isEmpty else { return "No Canaries reachable yet." }
        let noun = devices.count == 1 ? "Canary" : "Canaries"
        if onlineCount == devices.count {
            return "\(devices.count) \(noun), all online"
        }
        return "\(onlineCount) of \(devices.count) \(noun) online"
    }
}

// MARK: - Tolerant decode (the same tolerance the phone applies)

/// The phone's `FleetSelfDevice` decodes every field with a fallback on the
/// principle that *a device that omits a field is reported, not dropped* — a
/// Canary that answered at all is telling us something worth showing. The Wall
/// used strict synthesis instead, which is a stricter promise than the wire
/// makes: one device omitting `online` threw, and a throw here loses the WHOLE
/// snapshot, so a single old Canary could blank the television.
///
/// Written in an extension deliberately: an initializer in the type's own body
/// suppresses the memberwise `Device(name:online:…)` the tests construct with.
extension FleetSnapshot.Device {
    enum CodingKeys: String, CodingKey {
        case name, online, chain, product, hw, hub
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = (try? c.decode(String.self, forKey: .name)) ?? ""
        // Absent is not "online". The cautious reading is the only safe one
        // for a presence claim we were never given.
        online = (try? c.decode(Bool.self, forKey: .online)) ?? false
        chain = (try? c.decodeIfPresent(String.self, forKey: .chain)) ?? nil
        product = (try? c.decodeIfPresent(String.self, forKey: .product)) ?? nil
        // Empty and absent are one answer ("this device cannot say"), folded to
        // nil so no caller spends a lookup proving "" is not a board id. Same
        // fold as the phone's `hardware`.
        let board = (try? c.decodeIfPresent(String.self, forKey: .hw)) ?? nil
        hw = (board?.isEmpty == false) ? board : nil
        let standing = (try? c.decodeIfPresent(String.self, forKey: .hub)) ?? nil
        hub = (standing?.isEmpty == false) ? standing : nil
    }
}

/// Something the core said it couldn't do. Carries the core's own words so the
/// screen and a support transcript say the same thing.
struct WitnessCoreError: LocalizedError, Equatable {
    let message: String
    var errorDescription: String? { message }
}

/// The Rust core, as a Swift API.
///
/// Stateless and thread-safe (every C function is pure), so this is an enum of
/// static methods rather than an object anyone has to own or inject.
enum WitnessCore {
    /// Version of the linked core, for the About/Health panel.
    ///
    /// Static storage on the C side — deliberately NOT freed.
    static var version: String {
        guard let ptr = scv_core_version() else { return "unknown" }
        return String(cString: ptr)
    }

    /// Verify a sealed-log document.
    ///
    /// Never throws for bad *content* — a broken chain is a `VerifyReport` with
    /// `ok == false`, because the Wall must always have something calm to draw.
    /// It throws only when the core could not be reached or its answer could
    /// not be decoded, which is a bug, not a fleet state.
    static func verify(sealedLogJSON json: String) throws -> VerifyReport {
        let raw = try callReturningString(json) { scv_verify_sealed_log($0) }
        do {
            return try JSONDecoder().decode(VerifyReport.self, from: Data(raw.utf8))
        } catch {
            throw WitnessCoreError(message: "The verifier's answer could not be read: \(error.localizedDescription)")
        }
    }

    /// Parse a `GET /api/fleet` response.
    ///
    /// Throws on a response that isn't a fleet at all (a captive portal's HTML,
    /// say) — that IS a connection problem worth showing as one.
    static func parseFleet(json: String) throws -> FleetSnapshot {
        let raw = try callReturningString(json) { scv_parse_fleet($0) }
        let data = Data(raw.utf8)
        // The core reports a refusal as {"error": "..."} rather than a snapshot.
        if let failure = try? JSONDecoder().decode(CoreFailure.self, from: data) {
            throw WitnessCoreError(message: failure.error)
        }
        do {
            return try JSONDecoder().decode(FleetSnapshot.self, from: data)
        } catch {
            throw WitnessCoreError(message: "That hub's answer wasn't a fleet: \(error.localizedDescription)")
        }
    }

    private struct CoreFailure: Decodable { let error: String }

    /// The only place a C string is owned. `body` returns a Rust-allocated
    /// `char *`; `defer` hands it back on every exit path, including the throw.
    private static func callReturningString(
        _ input: String,
        _ body: (UnsafePointer<CChar>) -> UnsafeMutablePointer<CChar>?
    ) throws -> String {
        try input.withCString { cInput in
            guard let out = body(cInput) else {
                throw WitnessCoreError(message: "The verifier ran out of memory.")
            }
            defer { scv_string_free(out) }
            return String(cString: out)
        }
    }
}
