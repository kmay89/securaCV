// SecuraCVWidgetsBundle.swift — the widget extension entry point.
//
// Hosts the Live Activity (Dynamic Island). Home Screen / Lock Screen glance
// widgets can be added to this bundle later; the Live Activity is the one that
// makes the fleet a living presence in the Island.

import SwiftUI
import WidgetKit

@main
struct SecuraCVWidgetsBundle: WidgetBundle {
    var body: some Widget {
        FleetLiveActivity()
    }
}
