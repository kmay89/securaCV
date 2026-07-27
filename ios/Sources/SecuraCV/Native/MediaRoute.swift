// MediaRoute.swift
//
// AirPlay + speaker/microphone plumbing, done the native way. Two honest uses:
//   • A local CHIME for a critical alert can be sent to an AirPlay speaker
//     (kitchen HomePod) so the alarm is heard, not just felt — configured via
//     the system AVRoutePickerView (the standard AirPlay button).
//   • Two-way talk to a WAP-class Canary that offers it (doorbell) uses the
//     mic/speaker through a .playAndRecord session with .voiceChat mode.
// We never stream camera pixels; audio is opt-in and session-scoped.

import SwiftUI
import UIKit
import AVFoundation
#if canImport(AVKit)
import AVKit
#endif

/// The standard system AirPlay route button, wrapped for SwiftUI. Renders the
/// familiar control and lets iOS handle discovery/handoff — no custom protocol.
struct AirPlayRoutePicker: UIViewRepresentable {
    func makeUIView(context: Context) -> UIView {
        #if canImport(AVKit)
        let picker = AVRoutePickerView()
        picker.prioritizesVideoDevices = false     // audio routing (chime/talk)
        picker.activeTintColor = UIColor(Theme.color(.info))
        return picker
        #else
        return UIView()
        #endif
    }
    func updateUIView(_ uiView: UIView, context: Context) {}
}

@MainActor
enum AudioSession {
    /// Configure for playing a critical chime (routable to AirPlay).
    static func prepareForChime() {
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default, options: [.allowAirPlay, .duckOthers])
        try? session.setActive(true)
    }

    /// Configure for two-way doorbell talk (mic + speaker, echo-cancelled).
    static func prepareForTalk() {
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playAndRecord, mode: .voiceChat,
                                 options: [.allowBluetooth, .allowAirPlay, .defaultToSpeaker])
        try? session.setActive(true)
    }

    static func end() {
        try? AVAudioSession.sharedInstance().setActive(false, options: [.notifyOthersOnDeactivation])
    }
}
