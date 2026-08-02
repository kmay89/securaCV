// FleetGlanceWidget.swift  (watch widget extension target)
//
// One widget, every accessory family: the worst-severity glyph, the
// healthy/total count, and (where a family has room) the headline and an
// auto-updating "as of" age — the same honesty cues as the app. Renders
// ONLY from the watch-local cache; no widget ever talks to the phone
// (system budget honesty — the app and its WCSession own freshness, and
// they reload these timelines whenever a new snapshot lands).

import SwiftUI
import WidgetKit

struct FleetGlanceEntry: TimelineEntry {
    let date: Date
    let snapshot: WristSnapshot?
}

struct FleetGlanceProvider: TimelineProvider {
    func placeholder(in context: Context) -> FleetGlanceEntry {
        FleetGlanceEntry(date: Date(), snapshot: .sample())
    }

    func getSnapshot(in context: Context, completion: @escaping (FleetGlanceEntry) -> Void) {
        completion(currentEntry())
    }

    func getTimeline(in context: Context, completion: @escaping (Timeline<FleetGlanceEntry>) -> Void) {
        // One entry; ask the system back periodically so the relative "as of"
        // footer and the stale look can't fossilize. Real updates arrive via
        // reloadAllTimelines() from the app the moment a snapshot lands.
        let refresh = Date().addingTimeInterval(30 * 60)
        completion(Timeline(entries: [currentEntry()], policy: .after(refresh)))
    }

    private func currentEntry() -> FleetGlanceEntry {
        FleetGlanceEntry(date: Date(), snapshot: WristCache.load())
    }
}

struct FleetGlanceWidget: Widget {
    var body: some WidgetConfiguration {
        StaticConfiguration(kind: "SecuraCVFleetGlance", provider: FleetGlanceProvider()) { entry in
            FleetGlanceWidgetView(entry: entry)
                .containerBackground(for: .widget) { Color.clear }
        }
        .configurationDisplayName("Fleet")
        .description("Worst severity and healthy count for your fleet, at a glance.")
        .supportedFamilies([.accessoryCircular, .accessoryCorner,
                            .accessoryRectangular, .accessoryInline])
    }
}

struct FleetGlanceWidgetView: View {
    @Environment(\.widgetFamily) private var family
    let entry: FleetGlanceEntry

    var body: some View {
        if let snap = entry.snapshot {
            switch family {
            case .accessoryCircular: circular(snap)
            case .accessoryCorner: corner(snap)
            case .accessoryInline: inline(snap)
            default: rectangular(snap)
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

    private func inline(_ snap: WristSnapshot) -> some View {
        // Inline gets one line: glyph + the headline (or count when quiet).
        Label(snap.severity == .ok ? "\(snap.healthy)/\(snap.total) healthy" : snap.headline,
              systemImage: snap.severity.sfSymbol)
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
        // Honest empty state: cache empty until the watch app runs once.
        Group {
            switch family {
            case .accessoryInline:
                Label("Open SecuraCV", systemImage: "bird")
            case .accessoryRectangular:
                VStack(alignment: .leading, spacing: 1) {
                    Label("SecuraCV", systemImage: "bird").font(.headline)
                    Text("Open the watch app once to link your fleet.")
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
