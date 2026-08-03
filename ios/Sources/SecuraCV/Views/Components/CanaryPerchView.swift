// CanaryPerchView.swift
//
// The character, at rest: the one standard Canary (Assets "Canary", staged
// from brands/logo_512x512.png by scripts/make_brand_assets.py — composited,
// never redrawn), breathing gently on its perch. It appears in the calm
// places — empty states, first runs — because the mascot's job is to make
// "nothing is happening" feel like the good news it is. The bob is subtle,
// slow, and honors Reduce Motion by standing still.

import SwiftUI

struct CanaryPerchView: View {
    var height: CGFloat = 72

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var perchedHigh = false

    var body: some View {
        Image("Canary")
            .resizable()
            .scaledToFit()
            .frame(height: height)
            .offset(y: perchedHigh ? -height * 0.03 : 0)
            .animation(reduceMotion ? nil
                       : .easeInOut(duration: 2.6).repeatForever(autoreverses: true),
                       value: perchedHigh)
            .onAppear { if !reduceMotion { perchedHigh = true } }
            .accessibilityHidden(true)   // decorative; the words nearby carry the meaning
    }
}

#Preview("Canary at rest") {
    CanaryPerchView(height: 96)
}
