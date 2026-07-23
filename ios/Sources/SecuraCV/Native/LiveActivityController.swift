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
            content: .init(state: state, staleDate: nil),
            pushType: nil
        )
    }

    func update(_ state: FleetActivityAttributes.State) async {
        guard let activity else { return }
        await activity.update(.init(state: state, staleDate: nil))
    }

    func end() async {
        await activity?.end(nil, dismissalPolicy: .immediate)
        activity = nil
    }
    #else
    var isSupported: Bool { false }
    func start(fleetName: String, state: FleetActivityAttributes.State) {}
    func update(_ state: FleetActivityAttributes.State) async {}
    func end() async {}
    #endif
}
