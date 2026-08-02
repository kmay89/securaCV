// FleetActivityAttributes.swift  (SHARED between the app and the widget target)
//
// The contract for the Dynamic Island / Live Activity. Kept in one file that
// BOTH targets compile, so the app that starts an activity and the widget that
// renders it can never disagree about its shape. ActivityKit requires this type
// to be Codable + Hashable; keep it small — Live Activity payloads are tiny.
//
// The whole file is guarded on ActivityKit because Shared/ also compiles into
// the watchOS targets, where ActivityKit does not exist — the system mirrors
// the iPhone's Live Activities into the watch Smart Stack by itself, so the
// wrist needs no code here (docs/design/apple_watch_and_notifications.md §3.2).

#if canImport(ActivityKit)
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
    }

    /// A short name for the household/fleet, set when the activity starts.
    var fleetName: String
}

extension FleetActivityAttributes.State {
    /// The one Severity-ladder view of the raw byte. Severity itself lives in
    /// Shared/FleetEnums.swift and compiles into every target, so this
    /// accessor can too — the per-target mirrors it replaced are gone for good.
    var severity: Severity { Severity(tolerant: Int(severityRaw)) }
}
#endif
