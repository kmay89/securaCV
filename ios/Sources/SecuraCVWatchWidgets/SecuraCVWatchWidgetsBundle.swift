// SecuraCVWatchWidgetsBundle.swift  (watch widget extension target)
//
// The wrist's ambient surfaces: complications + the Smart Stack card, all
// rendered from the WristSnapshot the watch app cached in the watch-local
// app group (Shared/WristCache.swift). The push is what carries urgency;
// these carry ambient state on the system's refresh budget (RFC §3.2).

import SwiftUI
import WidgetKit

@main
struct SecuraCVWatchWidgetsBundle: WidgetBundle {
    var body: some Widget {
        FleetGlanceWidget()
        FindCanaryWidget()
    }
}
