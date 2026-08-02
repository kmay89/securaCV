// HeartbeatView.swift  (watch app target)
//
// RFC §3.3 screen 2: the dead-man's-switch as the hero. Quiet + green means
// safe — the scary failure is the alert that CAN'T be sent, so the state of
// the delivery path itself is what this screen teaches. "Test the path" runs
// the same end-to-end self-test as the phone's provably-alive card
// (Heartbeat.runTestAlert), started from the wrist; the verdict comes back
// as state, and both surfaces word it with the same shared sentence.

import SwiftUI

struct HeartbeatView: View {
    @EnvironmentObject var store: WristStore

    var body: some View {
        NavigationStack {
            content
        }
    }

    private var content: some View {
        ScrollView {
            VStack(spacing: Theme.m) {
                if let snap = store.snapshot {
                    let state = effectiveState(snap)
                    Image(systemName: state.sfSymbol)
                        .font(.system(size: 40))
                        .foregroundStyle(Theme.color(state.role))
                        .accessibilityHidden(true)
                    Text(summary(snap))
                        .font(.footnote)
                        .multilineTextAlignment(.center)

                    Button {
                        store.runPathTest()
                    } label: {
                        if store.testRequestInFlight {
                            Label("Testing…", systemImage: "arrow.triangle.2.circlepath")
                        } else {
                            Label("Test the path", systemImage: "dot.radiowaves.left.and.right")
                        }
                    }
                    .disabled(!store.isPhoneReachable || store.testRequestInFlight)

                    if !store.isPhoneReachable {
                        Text("iPhone not reachable — the test runs from your phone.")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }
                } else {
                    Image(systemName: "questionmark.circle")
                        .font(.system(size: 40))
                        .foregroundStyle(.secondary)
                    Text("Not yet verified")
                        .font(.footnote)
                }
            }
            .padding(.horizontal, Theme.s)
        }
        .navigationTitle("Heartbeat")
    }

    /// While a wrist-started test is in flight, show `.testing` even before
    /// the phone's next snapshot lands — the button press must visibly do
    /// something, and this is the same state the phone is now in.
    private func effectiveState(_ snap: WristSnapshot) -> WristHeartbeatState {
        store.testRequestInFlight ? .testing : snap.heartbeat
    }

    private func summary(_ snap: WristSnapshot) -> String {
        store.testRequestInFlight
            ? HeartbeatCopy.summary(state: .testing, secondsSinceVerified: nil)
            : snap.heartbeatSummary()
    }
}

#Preview("Heartbeat — sample") {
    HeartbeatView().environmentObject(WristStore.preview())
}
