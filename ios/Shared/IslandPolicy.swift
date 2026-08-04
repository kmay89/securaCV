// IslandPolicy.swift  (SHARED — pure policy, host-tested, beside its twin
// FeedbackPolicy)
//
// When the Dynamic Island / Lock Screen Live Activity may EXIST at all.
// A Live Activity is an episode, not wallpaper: iOS lends the app the
// status bar for something genuinely live, and squatting there with a
// permanent "all's well" pill is the ambient equivalent of the notification
// spam this app refuses to send. The polite twin of "an ordinary week
// produces zero haptics" is: an ordinary quiet day puts NOTHING in the
// status bar. The always-there glance for people who want one is the Lock
// Screen / Home Screen widget — a surface the USER placed, on ground iOS
// set aside for exactly that.

import Foundation

enum IslandPolicy {
    /// How long a resolved episode's "back to quiet" stays visible before
    /// the island returns the stage — long enough to witness the all-clear,
    /// short enough that it never becomes furniture.
    static let allClearLinger: TimeInterval = 3 * 60

    /// The island exists only while something is live:
    ///   * a fleet condition that needs a human — warn and up, on the same
    ///     mute-capped, tamper-punch-through severity every surface uses;
    ///   * the delivery path's dead-man's-switch talking (dark / failed);
    ///   * a path test in flight — an episode the user personally started.
    /// Routine activity (notice) and the everyday quiet are pull surfaces'
    /// territory, never the status bar's.
    static func shouldShow(worstSeverity: Severity,
                           heartbeat: WristHeartbeatState) -> Bool {
        if worstSeverity >= .warn { return true }
        switch heartbeat {
        case .testing, .dark, .failed: return true
        case .unknown, .alive: return false
        }
    }
}
