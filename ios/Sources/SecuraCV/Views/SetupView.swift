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
import UniformTypeIdentifiers
#if canImport(UIKit)
import UIKit
#endif

struct SetupView: View {
    @EnvironmentObject var store: FleetStore
    @Environment(\.dismiss) private var dismiss
    /// Where to land first — a deep link or a card can open straight into
    /// one walkthrough.
    var initial: SetupTarget?

    @State private var hubRecord = HubStore.load()
    @State private var householdWiFi = HouseholdWiFiStore.load()
    @State private var path: [SetupTarget] = []

    var body: some View {
        NavigationStack(path: $path) {
            List {
                Section {
                    // A Canary first: it is what most people are holding
                    // when they open this, and the one that needs nothing
                    // else in the house to start.
                    NavigationLink(value: SetupTarget.canary) {
                        choiceRow(icon: "bird",
                                  title: "Add a Canary",
                                  body: "Power it on near this phone and tap the card that appears — this phone gives it your Wi-Fi.")
                    }
                    NavigationLink(value: SetupTarget.hub) {
                        choiceRow(icon: "externaldrive.connected.to.line.below",
                                  title: hubRecord == nil ? "Set up a hub" : "Your hub",
                                  body: hubRow)
                    }
                } header: {
                    Text("What are you setting up?")
                } footer: {
                    Text("Each step says what this phone does, and what stays yours.")
                }
                if let login = HubSecretStore.brokerLogin() {
                    Section {
                        Label("Hub login for your Canaries: \(login.username)", systemImage: "key.horizontal")
                        BrokerPasswordCopyButton(password: login.password)
                    } footer: {
                        Text("Minted on this phone when the hub was finished, kept in its Keychain. A camera or radar Canary still takes it through the Flasher's MQTT fields: copy it here and paste it there (Universal Clipboard carries it to your Mac). The copy clears after two minutes.")
                    }
                }
                if let wifi = householdWiFi {
                    Section {
                        Label("Wi-Fi remembered for the next Canary: \(wifi.ssid)", systemImage: "wifi")
                        Button("Forget it", role: .destructive) {
                            HouseholdWiFiStore.forget()
                            householdWiFi = nil
                        }
                    } footer: {
                        Text("Kept in this phone's Keychain only, never in iCloud. Every Canary you add from this phone starts with it filled in, and Update fleet Wi-Fi keeps it current. Forget it here any time.")
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
                householdWiFi = HouseholdWiFiStore.load()
                if let initial, path.isEmpty { path = [initial] }
            }
        }
    }

    private var hubRow: String {
        guard let hub = hubRecord else {
            return "A Raspberry Pi running Home Assistant and the witness kernel — the meeting point your Canaries report to. This phone finishes it, no wizard."
        }
        let when = hub.finishedAt.map { "finished \($0.formatted(date: .abbreviated, time: .shortened))" } ?? "finished partway"
        return "\(hub.name) at \(hub.baseURL.host ?? "") — \(when). Open it again to re-check; nothing is done twice."
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
enum SetupTarget: String, Hashable, Identifiable, Sendable {
    case hub, canary
    var id: String { rawValue }
}

/// One walkthrough step as a list section: the title, the step in one
/// sentence, the whole story one tap away, and — where the phone acts —
/// the control under it.
struct SetupStepSection<Control: View>: View {
    let index: Int
    let step: SetupStep
    @ViewBuilder var control: Control

    var body: some View {
        Section {
            VStack(alignment: .leading, spacing: Theme.s) {
                Text(step.short)
                    .font(.subheadline)
                    .fixedSize(horizontal: false, vertical: true)
                DisclosureGroup("How it works") {
                    Text(step.body)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                        .padding(.top, Theme.xs)
                }
                .font(.footnote)
                control
            }
            .padding(.vertical, Theme.xs)
        } header: {
            Text("\(index). \(step.title)")
        }
    }
}

/// The hub's Canary login, one tap from the clipboard and gone after two
/// minutes, instead of printed on screen for retyping. (A camera or radar
/// Canary still needs it in the Flasher's MQTT fields: this phone has no
/// channel to hand it over yet.) NOT `.localOnly`: the Flasher runs on a
/// computer, and Universal Clipboard — the person's own devices, nothing
/// else — is how a copy here becomes a paste there. A local-only copy had
/// nowhere useful to go.
struct BrokerPasswordCopyButton: View {
    let password: String
    @State private var copied = false

    var body: some View {
        Button {
            #if canImport(UIKit)
            UIPasteboard.general.setItems([[UTType.plainText.identifier: password]],
                                          options: [.expirationDate: Date().addingTimeInterval(120)])
            #endif
            copied = true
        } label: {
            Label(copied ? "Password copied — clears in two minutes" : "Copy the password",
                  systemImage: copied ? "checkmark" : "doc.on.doc")
        }
    }
}

#if DEBUG
#Preview("Set up") {
    SetupView().environmentObject(DemoFleet.previewStore())
}
#endif
