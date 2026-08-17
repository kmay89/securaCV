// HeartbeatCard.swift
//
// The smoke-alarm "provably alive" card + the one-tap Test Alert. This is the
// hero moment: a green check that proves the whole device→relay→APNs→phone path
// works BEFORE you need it. When the heartbeat goes quiet past the dark window,
// this card turns into the alarm itself (dead-man's-switch).

import SwiftUI

struct HeartbeatCard: View {
    @EnvironmentObject var store: FleetStore
    @State private var testing = false

    var body: some View {
        Card {
            HStack(alignment: .top, spacing: Theme.m) {
                Image(systemName: glyph)
                    .font(.title2)
                    .foregroundStyle(tint)
                    .symbolEffect(.pulse, isActive: isTesting)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Away alerts").font(.headline)
                    Text(store.heartbeat.summary)
                        .font(.subheadline).foregroundStyle(.secondary)
                    // The measured speed, shown only once a test has
                    // actually measured it — the card claims a number it
                    // holds, never a vibe (non-negotiable #4).
                    if let latency = store.heartbeat.testLatencyLine {
                        Text(latency)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .monospacedDigit()
                    }
                }
                Spacer()
                Button {
                    testing = true
                    Task { await store.runTestAlert(); testing = false }
                } label: {
                    Text(isTesting ? "Testing…" : "Test")
                        .font(.subheadline.bold())
                }
                .buttonStyle(.borderedProminent)
                .disabled(isTesting)
            }
        }
        .accessibilityElement(children: .combine)
    }

    private var isTesting: Bool {
        if case .testing = store.heartbeat.state { return true }
        return testing
    }

    private var glyph: String {
        switch store.heartbeat.state {
        case .alive: return "checkmark.circle.fill"
        case .dark: return "exclamationmark.triangle.fill"
        case .failed: return "xmark.circle.fill"
        default: return "waveform.path.ecg"
        }
    }

    private var tint: Color {
        switch store.heartbeat.state {
        case .alive: return Theme.color(.calm)
        case .dark, .failed: return Theme.color(.alert)
        default: return Theme.color(.info)
        }
    }
}
