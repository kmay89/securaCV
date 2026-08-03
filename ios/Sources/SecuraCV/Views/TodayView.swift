// TodayView.swift
//
// The emotional product. One honest answer at the top ("All quiet" or the one
// thing that needs you), the provably-alive card with the Test Alert button,
// then the verified timeline in coarse buckets. No video wall — events, with
// their trust badge. Adapts to every screen via a scrolling stack + Dynamic
// Type; nothing is pinned to a fixed width.

import SwiftUI

struct TodayView: View {
    @EnvironmentObject var store: FleetStore

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: Theme.l) {
                    if store.demoMode { DemoDataBanner() }
                    StatusHero(severity: store.worstSeverity,
                               headline: store.allQuiet ? "All quiet" : hotHeadline,
                               watchers: store.witnesses.count)
                    HeartbeatCard()
                    if store.timeline.isEmpty {
                        EmptyTimeline()
                    } else {
                        VStack(alignment: .leading, spacing: Theme.s) {
                            Text("Today").font(.headline)
                            ForEach(store.timeline) { TimelineRow(event: $0) }
                        }
                    }
                }
                .padding()
                .frame(maxWidth: 640)          // stays readable on iPad, fills iPhone
                .frame(maxWidth: .infinity)
            }
            .refreshable { await store.refreshOnce() }
            .navigationTitle(store.fleetName)
            .background(backdrop.ignoresSafeArea())
        }
    }

    private var hotHeadline: String {
        guard let hot = store.witnesses.first else { return "Needs a look" }
        return "\(hot.displayName) • \(hot.statusLine)"
    }

    private var backdrop: some View {
        LinearGradient(colors: [Theme.color(store.worstSeverity.role).opacity(0.12), .clear],
                       startPoint: .top, endPoint: .center)
    }
}

/// The big glanceable status — the one honest answer. The quiet line names
/// the fleet's size on purpose: growth reads as more Canaries "watching
/// together", never as more things to worry about.
struct StatusHero: View {
    let severity: Severity
    let headline: String
    var watchers: Int = 0

    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        VStack(spacing: Theme.s) {
            Image(systemName: severity.sfSymbol)
                .font(.system(size: 56, weight: .semibold))
                .foregroundStyle(Theme.color(severity.role))
                .symbolRenderingMode(.hierarchical)
                // A quiet pulse ONLY while something needs you — the calm
                // state never moves, and Reduce Motion stills everything.
                .symbolEffect(.pulse, options: .repeating,
                              isActive: severity >= .alert && !reduceMotion)
                .accessibilityHidden(true)
            Text(headline)
                .font(.title2).bold()
                .multilineTextAlignment(.center)
            Text(subtitle)
                .font(.subheadline).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, Theme.l)
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(severity.label). \(headline)")
    }

    private var subtitle: String {
        guard severity == .ok else { return "One thing needs your attention." }
        switch watchers {
        case 0: return "Add a Canary to start the watch."
        case 1: return "Your Canary is watching."
        default: return "\(watchers) Canaries watching together."
        }
    }
}

struct TimelineRow: View {
    let event: TimelineEvent
    var body: some View {
        Card {
            HStack(spacing: Theme.m) {
                // The event's MEANING as the glyph, tinted by its severity —
                // richer than a bare status dot, still calm.
                Image(systemName: event.symbol)
                    .foregroundStyle(Theme.color(event.severity.role))
                    .imageScale(.medium)
                    .frame(width: 24)
                    .accessibilityLabel(event.severity.label)
                VStack(alignment: .leading, spacing: 2) {
                    Text(event.headline).font(.body)
                    HStack(spacing: 6) {
                        Image(systemName: event.badge.sfSymbol)
                            .imageScale(.small)
                            .foregroundStyle(event.badge.isTrusted ? Theme.color(.calm) : .secondary)
                        Text(event.badge.label).font(.caption).foregroundStyle(.secondary)
                        Text("·").foregroundStyle(.secondary)
                        Text(event.timeBucket, format: .dateTime.hour().minute())
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
                Spacer()
            }
        }
    }
}

struct EmptyTimeline: View {
    var body: some View {
        Card {
            HStack(spacing: Theme.m) {
                CanaryPerchView(height: 56)
                VStack(alignment: .leading, spacing: Theme.xs) {
                    Text("Nothing to report").font(.headline)
                    Text("Events show up here as your Canaries witness them — a claim of what happened, never a recording.")
                        .font(.subheadline).foregroundStyle(.secondary)
                }
            }
        }
    }
}

/// The unmissable "this is sample data" chip — demo must never pass as real.
struct DemoDataBanner: View {
    @EnvironmentObject var store: FleetStore
    var body: some View {
        HStack(spacing: Theme.s) {
            Image(systemName: "sparkles.rectangle.stack")
            Text("Demo fleet — sample data").font(.footnote.weight(.medium))
            Spacer()
            Button("Turn off") { store.setDemoMode(false) }
                .font(.footnote.bold())
        }
        .padding(.horizontal, Theme.m)
        .padding(.vertical, Theme.s)
        .background(.ultraThinMaterial, in: Capsule())
        .accessibilityElement(children: .combine)
    }
}

#Preview("Today — demo fleet") {
    TodayView().environmentObject(DemoFleet.previewStore())
}
