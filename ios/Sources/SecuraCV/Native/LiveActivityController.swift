// LiveActivityController.swift
//
// Drives the Dynamic Island + Lock Screen Live Activity. The app OWNS the
// activity's lifecycle; the widget target only renders FleetActivityAttributes
// (shared verbatim). We keep ONE activity for the whole fleet — "all's well /
// something needs you" — because a calm glanceable summary is the point, not a
// pile of per-device activities. Updates are cheap and rate-limited by iOS.

import Foundation
#if canImport(ActivityKit)
import ActivityKit
#endif

@MainActor
final class LiveActivityController: ObservableObject {
    static let shared = LiveActivityController()
    private init() {}

    #if canImport(ActivityKit)
    private var activity: Activity<FleetActivityAttributes>?

    var isSupported: Bool { ActivityAuthorizationInfo().areActivitiesEnabled }

    /// Start (or adopt) the single fleet activity.
    func start(fleetName: String, state: FleetActivityAttributes.State) {
        guard isSupported else { return }
        if let existing = Activity<FleetActivityAttributes>.activities.first {
            activity = existing
            Task { await update(state) }
            return
        }
        let attributes = FleetActivityAttributes(fleetName: fleetName)
        activity = try? Activity.request(
            attributes: attributes,
            content: content(for: state),
            pushType: nil
        )
    }

    func update(_ state: FleetActivityAttributes.State) async {
        guard let activity else { return }
        await activity.update(content(for: state))
    }

    /// Timing honesty + summary ranking: the island's truth STALE-DATES
    /// (the system dims it rather than presenting old state as current if
    /// the app stops updating), and an alarmed fleet outranks the everyday
    /// in the Smart Stack / island contention.
    private func content(for state: FleetActivityAttributes.State)
        -> ActivityContent<FleetActivityAttributes.State> {
        ActivityContent(state: state,
                        staleDate: Date().addingTimeInterval(15 * 60),
                        relevanceScore: state.severity >= .alert ? 100 : 50)
    }

    /// The episode resolved: show the final "back to quiet" state for a short
    /// linger, then leave the stage (IslandPolicy owns both the decision and
    /// the linger). Adopts an activity a previous launch left behind, so a
    /// quiet cold start CLEANS UP a stale pill rather than ignoring it —
    /// nothing to do in the common case, because quiet fleets never start one.
    func endEpisode(with state: FleetActivityAttributes.State) async {
        let live = activity ?? Activity<FleetActivityAttributes>.activities.first
        activity = nil
        guard let live else { return }
        await live.end(content(for: state),
                       dismissalPolicy: .after(Date().addingTimeInterval(IslandPolicy.allClearLinger)))
    }
    #else
    var isSupported: Bool { false }
    func start(fleetName: String, state: FleetActivityAttributes.State) {}
    func update(_ state: FleetActivityAttributes.State) async {}
    func endEpisode(with state: FleetActivityAttributes.State) async {}
    #endif
}
