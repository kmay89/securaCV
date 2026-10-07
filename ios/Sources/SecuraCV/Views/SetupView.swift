// SetupView.swift
//
// "Set up" — the one door for both walkthroughs: a hub on a Raspberry Pi,
// and a Canary joining the fleet. Every other surface in SecuraCV knew how
// to tend a fleet; none explained how one comes to exist. The desktop
// Flasher hatches boards and writes the hub's card, and this phone — the
// device that is always on the home Wi-Fi and always in the hand — finishes
// both: it watches the hub through first boot and completes Home Assistant
// without a wizard, and it hands each new Canary its Wi-Fi the way that
// Canary can take it.
//
// The copy is data (Shared/SetupGuide.swift), so the tests read the same
// words the screen shows.

import SwiftUI

struct SetupView: View {
    @EnvironmentObject var store: FleetStore
    @Environment(\.dismiss) private var dismiss
    /// Where to land first — a deep link or a card can open straight into
    /// one walkthrough.
    var initial: SetupTarget?

    @State private var hubRecord = HubStore.load()
    @State private var path: [SetupTarget] = []

    var body: some View {
        NavigationStack(path: $path) {
            List {
                Section {
                    NavigationLink(value: SetupTarget.hub) {
                        choiceRow(icon: "externaldrive.connected.to.line.below",
                                  title: hubRecord == nil ? "Set up a hub" : "Your hub",
                                  body: hubRecord == nil
                                    ? "A Raspberry Pi running Home Assistant and the witness kernel — the meeting point your Canaries report to. This phone finishes it: no setup wizard, no monitor."
                                    : "\(hubRecord!.name) at \(hubRecord!.baseURL.host ?? "") — finished \(hubRecord!.finishedAt.map { $0.formatted(date: .abbreviated, time: .shortened) } ?? "partway"). Open the walkthrough again to re-check it; nothing is done twice.")
                    }
                    NavigationLink(value: SetupTarget.canary) {
                        choiceRow(icon: "bird",
                                  title: "Add a Canary",
                                  body: "Give a new Canary your Wi-Fi the way it can take it — over Bluetooth, or through its own setup network — and watch it appear on the Fleet tab.")
                    }
                } header: {
                    Text("What are you setting up?")
                } footer: {
                    Text("Both walkthroughs explain every step and do the parts a phone can do. What stays yours is said out loud, never hidden behind a spinner.")
                }
                if let login = HubSecretStore.brokerLogin() {
                    Section {
                        Label("Broker login for your Canaries: \(login.username)", systemImage: "key.horizontal")
                    } footer: {
                        Text("Minted on this phone when the hub was finished, kept in its Keychain. A Canary that asks for an MQTT login gets this one — the Canary walkthrough shows it when it's needed.")
                    }
                }
            }
            .navigationTitle("Set up")
            .navigationDestination(for: SetupTarget.self) { target in
                switch target {
                case .hub: HubSetupView()
                case .canary: CanarySetupView()
                }
            }
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } }
            }
            .onAppear {
                hubRecord = HubStore.load()
                if let initial, path.isEmpty { path = [initial] }
            }
        }
    }

    private func choiceRow(icon: String, title: String, body: String) -> some View {
        HStack(alignment: .top, spacing: Theme.m) {
            Image(systemName: icon)
                .font(.title2)
                .foregroundStyle(Theme.color(.info))
                .frame(width: 32)
            VStack(alignment: .leading, spacing: Theme.xs) {
                Text(title).font(.headline)
                Text(body).font(.footnote).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(.vertical, Theme.xs)
    }
}

/// The two walkthroughs, as a value — what a deep link names and what the
/// navigation path holds.
enum SetupTarget: String, Hashable, Sendable {
    case hub, canary
}

/// One walkthrough step as a list section: the title, the explanation,
/// and — where the phone acts — the control under it.
struct SetupStepSection<Control: View>: View {
    let index: Int
    let step: SetupStep
    @ViewBuilder var control: Control

    var body: some View {
        Section {
            VStack(alignment: .leading, spacing: Theme.s) {
                Text(step.body)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                control
            }
            .padding(.vertical, Theme.xs)
        } header: {
            Text("\(index). \(step.title)")
        }
    }
}

#if DEBUG
#Preview("Set up") {
    SetupView().environmentObject(DemoFleet.previewStore())
}
#endif
