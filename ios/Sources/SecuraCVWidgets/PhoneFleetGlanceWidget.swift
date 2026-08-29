// PhoneFleetGlanceWidget.swift  (iOS widget extension target)
//
// The iPhone's half of the shared glance (Shared/FleetGlanceViews.swift):
// the fleet on the Lock Screen (accessory families) and the Home Screen
// (small/medium), rendered from the app-group cache FleetStore parks on
// every real change. The same WristSnapshot, the same views, the same
// honesty cues as the wrist — one glance contract everywhere.

import SwiftUI
import WidgetKit

struct PhoneFleetGlanceProvider: TimelineProvider {
    func placeholder(in context: Context) -> FleetGlanceEntry {
        FleetGlanceEntry(date: Date(), snapshot: .sample())
    }

    func getSnapshot(in context: Context, completion: @escaping (FleetGlanceEntry) -> Void) {
        completion(currentEntry())
    }

    func getTimeline(in context: Context, completion: @escaping (Timeline<FleetGlanceEntry>) -> Void) {
        // One entry + a periodic re-ask so the relative footer can't
        // fossilize; real updates arrive when the app reloads this kind.
        let refresh = Date().addingTimeInterval(30 * 60)
        completion(Timeline(entries: [currentEntry()], policy: .after(refresh)))
    }

    private func currentEntry() -> FleetGlanceEntry {
        FleetGlanceEntry(date: Date(), snapshot: PhoneGlanceCache.load())
    }
}

struct PhoneFleetGlanceWidget: Widget {
    var body: some WidgetConfiguration {
        StaticConfiguration(kind: PhoneGlanceCache.widgetKind,
                            provider: PhoneFleetGlanceProvider()) { entry in
            FleetGlanceWidgetView(entry: entry)
                .containerBackground(.background, for: .widget)
                // The tap lands where the state points: a glance showing
                // trouble opens the Alerts tab; a calm one — or no cached
                // snapshot at all — opens Today, because "no data yet" has
                // nothing for Alerts to show. Same securacv:// dialect as
                // every other door (Shared/AppRoute.swift), so the widget
                // can never route anywhere the app didn't already offer.
                .widgetURL((entry.snapshot?.severity ?? .ok) >= .warn
                           ? AppRoute.alerts(witnessID: nil).url
                           : AppRoute.today.url)
        }
        .configurationDisplayName("Fleet")
        .description("Your fleet's one honest answer — on the Lock Screen and Home Screen.")
        .supportedFamilies([.systemSmall, .systemMedium,
                            .accessoryCircular, .accessoryRectangular, .accessoryInline])
    }
}
