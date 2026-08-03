// FleetGlanceViews.swift  (SHARED — one glance, every ambient surface)
//
// The fleet-glance widget's entry and views, compiled into BOTH widget
// targets: the watch complications and the iPhone Lock Screen / Home Screen
// widgets render these same views over the same WristSnapshot — the glance
// literally cannot tell a different story on a different surface. Each
// target keeps only its own thin provider (which cache it reads, which
// families it offers).

import SwiftUI
import WidgetKit

struct FleetGlanceEntry: TimelineEntry {
    let date: Date
    let snapshot: WristSnapshot?
}

struct FleetGlanceWidgetView: View {
    @Environment(\.widgetFamily) private var family
    let entry: FleetGlanceEntry

    var body: some View {
        if let snap = entry.snapshot {
            switch family {
            case .accessoryCircular: circular(snap)
            case .accessoryInline: inline(snap)
            case .systemSmall: small(snap)
            #if os(watchOS)
            case .accessoryCorner: corner(snap)
            #endif
            default: rectangular(snap)   // accessoryRectangular, systemMedium
            }
        } else {
            noData
        }
    }

    // MARK: families

    private func circular(_ snap: WristSnapshot) -> some View {
        VStack(spacing: 0) {
            Image(systemName: snap.severity.sfSymbol)
                .font(.title3)
                .foregroundStyle(Theme.color(snap.severity.role))
            Text("\(snap.healthy)/\(snap.total)")
                .font(.caption2).monospacedDigit()
        }
        .accessibilityLabel(accessibilitySummary(snap))
    }

    #if os(watchOS)
    private func corner(_ snap: WristSnapshot) -> some View {
        Image(systemName: snap.severity.sfSymbol)
            .font(.title3)
            .foregroundStyle(Theme.color(snap.severity.role))
            .widgetLabel {
                Text("\(snap.healthy)/\(snap.total) healthy")
                    .monospacedDigit()
            }
            .accessibilityLabel(accessibilitySummary(snap))
    }
    #endif

    private func inline(_ snap: WristSnapshot) -> some View {
        // Inline gets one line: glyph + the headline (or count when quiet).
        Label(snap.severity == .ok ? "\(snap.healthy)/\(snap.total) healthy" : snap.headline,
              systemImage: snap.severity.sfSymbol)
            .accessibilityLabel(accessibilitySummary(snap))
    }

    /// Home Screen small: the one honest answer, at a glance from a meter away.
    private func small(_ snap: WristSnapshot) -> some View {
        VStack(alignment: .leading, spacing: Theme.xs) {
            Image(systemName: snap.severity.sfSymbol)
                .font(.title)
                .foregroundStyle(Theme.color(snap.severity.role))
            Spacer(minLength: 0)
            Text(snap.headline)
                .font(.subheadline.weight(.semibold))
                .lineLimit(2)
            HStack(spacing: Theme.xs) {
                Text("\(snap.healthy)/\(snap.total) healthy").monospacedDigit()
                if snap.isDemoData { Text("· sample").foregroundStyle(.secondary) }
            }
            .font(.caption2)
            .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
        .accessibilityLabel(accessibilitySummary(snap))
    }

    private func rectangular(_ snap: WristSnapshot) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            HStack(spacing: Theme.xs) {
                Image(systemName: snap.severity.sfSymbol)
                    .foregroundStyle(Theme.color(snap.severity.role))
                Text(snap.fleetName).font(.headline).lineLimit(1)
            }
            Text(snap.headline).font(.caption2).lineLimit(1)
            HStack(spacing: Theme.xs) {
                Text("\(snap.healthy)/\(snap.total) healthy").monospacedDigit()
                if snap.isDemoData {
                    Text("· sample").foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
                Text(snap.sentAt, style: .relative).foregroundStyle(.secondary)
            }
            .font(.caption2)
        }
        .accessibilityLabel(accessibilitySummary(snap))
    }

    private var noData: some View {
        // Honest empty state: cache empty until the app has run once.
        Group {
            switch family {
            case .accessoryInline:
                Label("Open SecuraCV", systemImage: "bird")
            case .accessoryRectangular, .systemSmall:
                VStack(alignment: .leading, spacing: 1) {
                    Label("SecuraCV", systemImage: "bird").font(.headline)
                    Text("Open the app once to link your fleet.")
                        .font(.caption2).foregroundStyle(.secondary)
                }
            default:
                Image(systemName: "bird").font(.title3)
            }
        }
    }

    private func accessibilitySummary(_ snap: WristSnapshot) -> String {
        "\(snap.fleetName): \(snap.headline). \(snap.healthy) of \(snap.total) healthy."
    }
}
