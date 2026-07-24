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

        /// Stable within a snapshot: the fleet endpoint has no device ids, and
        /// names are what a person actually distinguishes Canaries by.
        var id: String { name }

        /// `chain` is optional; absent means "not reported", which is not the
        /// same as "broken" and must never be drawn as an alarm.
        var chainIsTroubled: Bool {
            guard let chain else { return false }
            return chain != "ok"
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
