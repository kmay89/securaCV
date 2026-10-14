// RadioAdviceRow.swift
//
// "Never a spinner without a reason and a way out." The two small rows every
// add-a-Canary screen uses where it used to spin forever: the cause in one
// sentence when a radio is off or not allowed (Model/AddCanaryFlow.swift
// decides the words), with the Settings button when the fix lives there —
// and, only when the radio can actually hear, the "Listening…" line.

import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

struct RadioAdviceRow: View {
    let advice: RadioAdvice

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.xs) {
            Label(advice.text, systemImage: "exclamationmark.triangle")
                .font(.footnote)
                .foregroundStyle(Theme.color(.warn))
                .fixedSize(horizontal: false, vertical: true)
            if advice.opensSettings {
                OpenSettingsButton()
            }
        }
    }
}

/// This app's own page in Settings — where its Bluetooth, Local Network
/// and Camera switches live.
struct OpenSettingsButton: View {
    @Environment(\.openURL) private var openURL

    var body: some View {
        #if canImport(UIKit)
        if let url = URL(string: UIApplication.openSettingsURLString) {
            Button("Open Settings") { openURL(url) }
                .buttonStyle(.bordered)
        }
        #endif
    }
}

/// "Listening…" where the radio can hear; the reason and the way out where
/// it cannot.
struct ListeningRow: View {
    let text: String
    let advice: RadioAdvice?

    var body: some View {
        if let advice {
            RadioAdviceRow(advice: advice)
        } else {
            HStack(spacing: Theme.s) {
                ProgressView()
                Text(text)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}
