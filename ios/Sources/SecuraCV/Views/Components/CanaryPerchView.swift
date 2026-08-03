// CanaryPerchView.swift
//
// The character at rest — now a thin wrapper over the shared CanaryActor
// (the same performer the watch stages, running the same mood engine the
// bedside glass runs). This view is the CALM-ONLY convenience for empty
// states and consent moments; screens that know the fleet's real mood
// should stage CanaryActor with the store's published face directly.

import SwiftUI

struct CanaryPerchView: View {
    var height: CGFloat = 72

    var body: some View {
        CanaryActor(face: .calm, height: height)
    }
}

#Preview("Canary at rest") {
    CanaryPerchView(height: 96)
}
