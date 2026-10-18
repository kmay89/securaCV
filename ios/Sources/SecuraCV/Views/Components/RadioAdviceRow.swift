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
/// it cannot. Given when the listening began (`since`), a second footnote
/// appears after SetupGuide.nothingYetAfter of silence — what to try when
/// no card has come (the door's rules, from SetupGuide.nothingYetLine) —
/// so the spinner never outlives its reason. Without `since` the row stays
/// as it was (a network watch has its own words for a timeout).
struct ListeningRow: View {
    let text: String
    let advice: RadioAdvice?
    var since: Date? = nil
    @State private var nothingYet = false

    var body: some View {
        if let advice {
            RadioAdviceRow(advice: advice)
        } else {
            VStack(alignment: .leading, spacing: Theme.xs) {
                HStack(spacing: Theme.s) {
                    ProgressView()
                    Text(text)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if nothingYet {
                    Text(SetupGuide.nothingYetLine)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            // Restarts whenever the listening clock does (a new consent, a
            // foreground), and is canceled with the row.
            .task(id: since) {
                nothingYet = false
                guard let since else { return }
                let wait = SetupGuide.nothingYetAfter - Date().timeIntervalSince(since)
                if wait > 0 {
                    try? await Task.sleep(for: .seconds(wait))
                }
                guard !Task.isCancelled else { return }
                nothingYet = true
            }
        }
    }
}
