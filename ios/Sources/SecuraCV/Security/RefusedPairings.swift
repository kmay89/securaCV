// RefusedPairings.swift
//
// Pairings whose token a Canary answered 403 to. A Canary installed again
// or erased has a new key and token, so the old pairing can never succeed
// and every further request with it is a wrong-bearer strike against this
// phone's address. A refused pairing is not asked again until it is made
// again or removed. Held in memory: a relaunch asks once more.

import Foundation

struct RefusedPairings: Equatable, Sendable {
    /// device id → the `pairedAt` of the pairing that was refused. Pairing
    /// again writes a new `pairedAt`, which no longer matches.
    private var refused: [String: Date] = [:]

    /// True for a 403 from an authenticated route: the token was refused.
    static func isRefusal(_ error: Error) -> Bool {
        if let deviceError = error as? DeviceError, case .http(403, _) = deviceError {
            return true
        }
        return false
    }

    var isEmpty: Bool { refused.isEmpty }

    func holds(_ ref: PairedDeviceRef) -> Bool {
        refused[ref.id] == ref.pairedAt
    }

    mutating func note(_ ref: PairedDeviceRef) {
        refused[ref.id] = ref.pairedAt
    }

    /// Drop every entry whose pairing was removed or made again.
    mutating func prune(to devices: [PairedDeviceRef]) {
        refused = refused.filter { entry in
            devices.contains { $0.id == entry.key && $0.pairedAt == entry.value }
        }
    }
}
