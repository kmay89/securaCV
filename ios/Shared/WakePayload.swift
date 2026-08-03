// WakePayload.swift
//
// The away wake, and the ONLY thing it is allowed to say. Compiled into both
// the app and the notification service extension (and nothing else), so the
// two halves of the away path can never disagree about what a wake means or
// what sentence it becomes.
//
// The rule this file exists to enforce: a wake carries a coarse severity
// CLASS and nothing more — no zone, no device name, no precise time, no event
// content (Invariants I and III). Everything a user reads is composed on their
// own device from their own data. Keeping the vocabulary and the copy here
// means a reviewer can check that claim by reading one short file.

import Foundation

/// The four classes a wake may carry — the same coarse vocabulary the design
/// doc names (§5) and the device firmware already speaks.
/// Note the deliberate absence of any dependency below: this file compiles
/// into the notification service extension, which links neither the fleet
/// model nor SwiftUI. Anything that needs `Severity` to build a wake lives
/// app-side (see AwayPush) so the extension stays this small.
enum WakeClass: String, Codable, Sendable, CaseIterable {
    case tamper, integrity, offline, pattern

    /// The sentence shown when a wake lands. Vague on purpose: it names a
    /// KIND of trouble and sends the user into the app, where their own data
    /// says which Canary and what happened.
    var line: String {
        switch self {
        case .tamper: return "A Canary reports tamper — open to see which."
        case .integrity: return "A signature or chain check failed."
        case .offline: return "A Canary went dark."
        case .pattern: return "A Canary needs your attention."
        }
    }

    /// Only tamper earns the smoke-alarm treatment.
    var isLifeSafety: Bool { self == .tamper }
}

enum WakePayload {
    /// The single field name, used by the publisher, the subscription's
    /// desiredKeys, and the parser below.
    static let classKey = "sev"

    /// Pull the coarse class out of a push payload. Handles BOTH shapes: the
    /// CloudKit query-notification envelope shipped today, and the flat
    /// `{"sev": …}` body a self-hosted relay would send (design doc §5) — so
    /// standing that relay up later needs no change to the receiving side.
    static func wakeClass(from userInfo: [AnyHashable: Any]) -> WakeClass? {
        if let flat = userInfo[classKey] as? String, let wake = WakeClass(rawValue: flat) {
            return wake
        }
        guard let ck = userInfo["ck"] as? [AnyHashable: Any],
              let query = ck["qry"] as? [AnyHashable: Any],
              let fields = query["af"] as? [AnyHashable: Any],
              let raw = fields[classKey] as? String else { return nil }
        return WakeClass(rawValue: raw)
    }
}
