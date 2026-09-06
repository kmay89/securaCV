// FindCanaryWidget.swift  (watch widget extension target)
//
// One tap from the watch face to the hot/cold search: the complication
// remembers the LAST Canary the user went finding for (WristLastFind,
// written by the Find screen when a search actually starts) and deep-links
// straight into finding it again — the keys-wallet-remote ritual, one raise
// of the wrist.
//
// Honesty rules, same as every ambient surface:
//   * The target must still be FINDABLE — named in the cached snapshot with
//     a recognizable beacon (WristLastFind.findableTarget, host-tested).
//     Anything less and the complication carries no deep link: the tap
//     opens the app plainly instead of promising a search it can't run.
//   * Renders ONLY from the watch-local cache; a widget never talks to the
//     phone or the radio (the system budget rule the glance widget states).
//   * The link speaks the shared securacv:// dialect (Shared/AppRoute), so
//     the door and the destination can't drift apart.

import SwiftUI
import WidgetKit

struct FindCanaryEntry: TimelineEntry {
    let date: Date
    /// The one-tap target, when there honestly is one.
    let target: WristWitness?
}

struct FindCanaryProvider: TimelineProvider {
    func placeholder(in context: Context) -> FindCanaryEntry {
        FindCanaryEntry(date: Date(), target: WristSnapshot.sample().witnesses.first)
    }

    func getSnapshot(in context: Context, completion: @escaping (FindCanaryEntry) -> Void) {
        completion(currentEntry())
    }

    func getTimeline(in context: Context, completion: @escaping (Timeline<FindCanaryEntry>) -> Void) {
        // Same cadence discipline as the glance widget: one entry, periodic
        // re-ask; real updates ride reloadAllTimelines() when a snapshot
        // lands or a search runs.
        let refresh = Date().addingTimeInterval(30 * 60)
        completion(Timeline(entries: [currentEntry()], policy: .after(refresh)))
    }

    private func currentEntry() -> FindCanaryEntry {
        FindCanaryEntry(date: Date(),
                        target: WristLastFind.findableTarget(id: WristLastFind.load(),
                                                             in: WristCache.load()))
    }
}

struct FindCanaryWidgetView: View {
    let entry: FindCanaryEntry
    @Environment(\.widgetFamily) private var family

    var body: some View {
        content
            .widgetURL(entry.target.map { AppRoute.find(witnessID: $0.id).url })
    }

    @ViewBuilder
    private var content: some View {
        switch family {
        case .accessoryInline:
            // Inline gets words; the graphic families get the glyph.
            Text(entry.target.map { "Find \($0.name)" } ?? "Find a Canary")
        default:
            Image(systemName: "location.north.circle.fill")
                .font(.title2)
                .widgetLabel(entry.target?.name ?? "Find")
        }
    }
}

struct FindCanaryWidget: Widget {
    var body: some WidgetConfiguration {
        StaticConfiguration(kind: WristLastFind.widgetKind, provider: FindCanaryProvider()) { entry in
            FindCanaryWidgetView(entry: entry)
                .containerBackground(for: .widget) { Color.clear }
        }
        .configurationDisplayName("Find a Canary")
        .description("One tap back into the hot/cold search for the Canary you last went finding.")
        .supportedFamilies([.accessoryCircular, .accessoryCorner, .accessoryInline])
    }
}
