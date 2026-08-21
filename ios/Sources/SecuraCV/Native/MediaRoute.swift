// MediaRoute.swift
//
// AirPlay routing, done the native way: a local CHIME for a critical alert
// can be sent to an AirPlay speaker (kitchen HomePod) so the alarm is heard,
// not just felt — configured via the system AVRoutePickerView (the standard
// AirPlay button). We never stream camera pixels.
//
// There is deliberately NO microphone or two-way-talk plumbing here. An
// AudioSession helper for a doorbell talk feature used to sit below with zero
// callers, while Info.plist promised a mic permission the app never exercised
// — the "can't, not won't" rule cuts the other way for us too: capability the
// app doesn't use is capability it shouldn't declare. If talk ever lands, it
// brings its session config, the mic usage string, and its UI in one change.

import SwiftUI
import UIKit
#if canImport(AVKit)
import AVKit
#endif

/// The standard system AirPlay route button, wrapped for SwiftUI. Renders the
/// familiar control and lets iOS handle discovery/handoff — no custom protocol.
struct AirPlayRoutePicker: UIViewRepresentable {
    func makeUIView(context: Context) -> UIView {
        #if canImport(AVKit)
        let picker = AVRoutePickerView()
        picker.prioritizesVideoDevices = false     // audio routing (the chime)
        picker.activeTintColor = UIColor(Theme.color(.info))
        return picker
        #else
        return UIView()
        #endif
    }
    func updateUIView(_ uiView: UIView, context: Context) {}
}
