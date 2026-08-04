// The Apple Home shepherd — phase A2 of docs/design/apple_home_integration.md.
//
// The app is never the accessory (no iOS API can publish one); it is the
// shepherd and concierge: it holds the owner's consent for what the house may
// know, reads back what Apple Home actually reports, and says so plainly when
// the two worlds disagree. The bar is the RFC's dumb-PIR promise — out of the
// box the house learns no more from a Canary than a $20 PIR would tell it,
// and the one sanctioned step past that (the coarse kind of thing that moved)
// stays off until a human turns it on here.

import SwiftUI

struct AppleHomeView: View {
    @ObservedObject var home: HomeKitBridge

    init(home: HomeKitBridge = .shared) {
        self.home = home
    }

    var body: some View {
        List {
            Section {
                Toggle("Publish to Apple Home", isOn: $home.isEnabled)
                    .onChange(of: home.isEnabled) { _, on in
                        if on { home.requestAccess() }
                    }
            } header: {
                Text("Apple Home")
            } footer: {
                Text("Your fleet appears in the Home app as plain sensors — present-tense booleans, no history, no footage, no identity — so the house can answer the witness: lights before eyes, tamper house-wide. Turning this on asks iOS for Home access; that prompt is this feature.")
            }

            if home.isEnabled {
                Section {
                    Label(statusExplanation,
                          systemImage: home.authorized
                              ? "house"
                              : "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(home.authorized ? .secondary : Theme.color(.warn))
                    if !home.authorized {
                        Button("Allow Home access") { home.requestAccess() }
                    }
                } header: {
                    Text("Standing")
                } footer: {
                    if home.authorized && !home.homeHubPresent {
                        // Apple's rule, said out loud exactly once (RFC §5).
                        Text("Apple Home needs a home hub — a HomePod or an Apple TV — to run automations and reach you away from home. Without one, sensors still render while your phone is on this network.")
                    }
                }

                Section {
                    ForEach(plainSignals, id: \.self) { signal in
                        signalRow(signal)
                    }
                } header: {
                    Text("What the house may know")
                } footer: {
                    Text("Tamper always reports — a witness must not be able to go quiet invisibly in a home you chose to publish into. Silence is never rendered as safety.")
                }

                Section {
                    ForEach(classSignals, id: \.self) { signal in
                        signalRow(signal)
                    }
                } header: {
                    Text("The kind of thing (opt-in)")
                } footer: {
                    Text("One step past a dumb PIR: the coarse kind of thing that moved — person, vehicle, animal, package — never who. Off until you turn it on.")
                }
            }
        }
        .navigationTitle("Apple Home")
        .navigationBarTitleDisplayMode(.inline)
    }

    private var plainSignals: [HomeSignal] {
        HomeSignal.allCases.filter { !$0.isClassScoped }
    }

    private var classSignals: [HomeSignal] {
        HomeSignal.allCases.filter(\.isClassScoped)
    }

    private var statusExplanation: String {
        if !home.authorized {
            return "iOS hasn't granted Home access yet."
        }
        if home.homeHubPresent {
            return "Home access granted; a home hub is connected."
        }
        return "Home access granted; no connected home hub seen."
    }

    /// One consent toggle, routed through the bridge so the tamper refusal is
    /// enforced by the model and *visible* in the UI, not silently ignored.
    @ViewBuilder
    private func signalRow(_ signal: HomeSignal) -> some View {
        Toggle(isOn: Binding(
            get: { home.enabledSignals.contains(signal) },
            set: { home.setSignal(signal, enabled: $0) }
        )) {
            Text(signal.label)
        }
        .disabled(signal == .tamper)
    }
}

#Preview("Apple Home — fresh consent") {
    // A private instance, never .shared: previews run unsigned, and the
    // shared bridge must stay untouched until a human flips the toggle.
    NavigationStack {
        AppleHomeView(home: HomeKitBridge())
    }
}
