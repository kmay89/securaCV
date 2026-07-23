// FleetActivityAttributes.swift  (SHARED between the app and the widget target)
//
// The contract for the Dynamic Island / Live Activity. Kept in one file that
// BOTH targets compile, so the app that starts an activity and the widget that
// renders it can never disagree about its shape. ActivityKit requires this type
// to be Codable + Hashable; keep it small — Live Activity payloads are tiny.

import Foundation
import ActivityKit

struct FleetActivityAttributes: ActivityAttributes {
    /// Immutable for the life of the activity.
    public typealias ContentState = State

    /// The live, updatable part shown in the Island and on the Lock Screen.
    public struct State: Codable, Hashable {
        /// Worst current severity across the fleet (drives color + glyph).
        var severityRaw: UInt8
        /// e.g. "All quiet" or "Front Porch • Tamper"
        var headline: String
        /// How many Canaries are reporting healthy right now.
        var healthy: Int
        var total: Int
        /// Seconds since the last verified end-to-end heartbeat — the
        /// smoke-alarm "provably alive" number. nil = never verified.
        var lastVerifiedAgo: Int?

        // NOTE: `severityRaw` is deliberately a plain UInt8 and this struct
        // stays free of the Severity enum, because this file compiles into BOTH
        // the app and the widget target and each defines its own Severity. Each
        // target adds its own `severity` accessor over severityRaw.
    }

    /// A short name for the household/fleet, set when the activity starts.
    var fleetName: String
}
