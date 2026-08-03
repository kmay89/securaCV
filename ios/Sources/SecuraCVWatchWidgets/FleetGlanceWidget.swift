// FleetGlanceWidget.swift  (watch widget extension target)
//
// The wrist's thin half of the shared glance (Shared/FleetGlanceViews.swift):
// a provider over the watch-LOCAL cache and the accessory families the watch
// face offers. Renders ONLY from the cache; no widget ever talks to the
// phone (system budget honesty — the app and its WCSession own freshness,
// and they reload these timelines whenever a new snapshot lands).

import SwiftUI
import WidgetKit

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
