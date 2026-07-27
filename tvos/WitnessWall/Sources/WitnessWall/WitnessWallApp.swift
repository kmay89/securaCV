//  WitnessWallApp.swift — the entry point.
//
//  One scene, one view. The Wall is furniture: it starts, it connects, it
//  stays honest. There is no navigation stack because there is nowhere else
//  to go — everything the screen can say, it says on the wall itself.

import SwiftUI

@main
struct WitnessWallApp: App {
    var body: some Scene {
        WindowGroup {
            WallView()
        }
    }
}
