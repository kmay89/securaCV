// FleetLiveActivity.swift  (widget extension target)
//
// The Dynamic Island + Lock Screen presentation of the fleet. Renders the
// SHARED FleetActivityAttributes — the app updates the state, this only draws.
// Every region is provided (expanded / compact leading+trailing / minimal) so
// the Island looks right at every size and when stacked with other activities.

import SwiftUI
import WidgetKit
import ActivityKit

struct FleetLiveActivity: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: FleetActivityAttributes.self) { context in
            // Lock Screen / banner presentation.
            LockScreenFleetView(state: context.state, fleetName: context.attributes.fleetName)
                .padding()
                .activityBackgroundTint(Color.black.opacity(0.25))
        } dynamicIsland: { context in
            let sev = context.state.severity
            return DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    Label {
                        Text(context.attributes.fleetName).font(.caption).bold()
                    } icon: {
                        Image(systemName: sev.sfSymbol).foregroundStyle(tint(sev))
                    }
                }
                DynamicIslandExpandedRegion(.trailing) {
                    Text("\(context.state.healthy)/\(context.state.total)")
                        .font(.caption).monospacedDigit()
                        .foregroundStyle(.secondary)
                }
                DynamicIslandExpandedRegion(.bottom) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(context.state.headline).font(.footnote).bold()
                        if let ago = context.state.lastVerifiedAgo {
                            Text("Delivery verified \(agoText(ago))")
                                .font(.caption2).foregroundStyle(.secondary)
                        }
                    }
                }
            } compactLeading: {
                Image(systemName: sev.sfSymbol).foregroundStyle(tint(sev))
            } compactTrailing: {
                Text("\(context.state.healthy)/\(context.state.total)")
                    .font(.caption2).monospacedDigit()
            } minimal: {
                Image(systemName: sev.sfSymbol).foregroundStyle(tint(sev))
            }
            .keylineTint(tint(sev))
        }
    }

    private func tint(_ sev: Severity) -> Color {
        switch sev {
        case .ok: return .green
        case .notice: return .blue
        case .warn: return .orange
        case .alert, .tamper: return .red
        }
    }

    /// One formatter for how long ago, shared with every other heartbeat
    /// surface — a persisted verification can now be days old, and "4320m
    /// ago" is not a thing anyone reads.
    private func agoText(_ s: Int) -> String { HeartbeatCopy.ago(s) }
}

struct LockScreenFleetView: View {
    let state: FleetActivityAttributes.State
    let fleetName: String

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: state.severity.sfSymbol)
                .font(.title2)
                .foregroundStyle(state.severity == .ok ? .green : .red)
            VStack(alignment: .leading, spacing: 2) {
                Text(fleetName).font(.headline)
                Text(state.headline).font(.subheadline).foregroundStyle(.secondary)
            }
            Spacer()
            VStack(alignment: .trailing, spacing: 2) {
                Text("\(state.healthy)/\(state.total)").font(.title3).monospacedDigit().bold()
                Text("healthy").font(.caption2).foregroundStyle(.secondary)
            }
        }
    }
}

// Severity and the State.severity accessor come from Shared/ (FleetEnums.swift
// + FleetActivityAttributes.swift) — this target used to keep a hand-mirrored
// copy of the ladder "identical on purpose", which is exactly the kind of
// duplicate that drifts. One ladder now compiles into every target.
