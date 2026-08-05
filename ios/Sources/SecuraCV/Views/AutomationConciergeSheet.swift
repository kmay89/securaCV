// The "tell the house" sheet — the automation concierge (RFC §4.2).
//
// Pick a witness signal, pick a scene the household already made in the
// Home app, read the sentence back, and the app writes a real HomeKit
// automation that runs on the home hub with every app closed. The app is
// the author, never the runtime. Scenes are never created here — the Home
// app owns those — and nothing inbound to a Canary is offered (the RFC's
// open decision #4 stays open).

import SwiftUI

struct AutomationConciergeSheet: View {
    @ObservedObject var home: HomeKitBridge
    @Environment(\.dismiss) private var dismiss

    @State private var signal: HomeSignal = .motion
    @State private var sceneChoice: (id: UUID, name: String)?
    @State private var sources: [String] = []
    @State private var scenes: [(id: UUID, name: String)] = []
    @State private var authored: [(id: UUID, name: String)] = []
    @State private var errorLine: String?
    @State private var written = false

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Picker("Signal", selection: $signal) {
                        ForEach(offerableSignals, id: \.self) { s in
                            Text(s.label).tag(s)
                        }
                    }
                    if sources.isEmpty {
                        Label("No Canary in Apple Home carries this signal yet.",
                              systemImage: "exclamationmark.triangle")
                            .font(.caption)
                            .foregroundStyle(Theme.color(.warn))
                    }
                } header: {
                    Text("When this happens")
                } footer: {
                    Text("Only signals you consented to publish are offered. Automations fire on the projection's metronome — within a second, never on the event's own clock.")
                }

                Section {
                    if scenes.isEmpty {
                        Text("No scenes yet. Make one in the Home app first — the concierge runs your scenes, it doesn't invent them.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    } else {
                        ForEach(scenes, id: \.id) { scene in
                            Button {
                                sceneChoice = scene
                            } label: {
                                HStack {
                                    Text(scene.name)
                                    Spacer()
                                    if sceneChoice?.id == scene.id {
                                        Image(systemName: "checkmark")
                                    }
                                }
                            }
                            .foregroundStyle(.primary)
                        }
                    }
                } header: {
                    Text("Do this")
                }

                if let plan = plan {
                    Section {
                        Text(plan.sentence)
                        if !home.homeHubPresent {
                            Label(ConciergeReadiness.readyWithoutHomeHub.note ?? "",
                                  systemImage: "exclamationmark.triangle")
                                .font(.caption)
                                .foregroundStyle(Theme.color(.warn))
                        }
                        Button(written ? "Added to Apple Home" : "Tell the house") {
                            write(plan)
                        }
                        .disabled(written)
                        if let errorLine {
                            Label(errorLine, systemImage: "exclamationmark.triangle")
                                .font(.caption)
                                .foregroundStyle(Theme.color(.warn))
                        }
                    } header: {
                        Text("Confirm")
                    } footer: {
                        Text("This becomes a real automation in the Home app — it runs on your home hub with this app closed, and you can edit or delete it there like any other.")
                    }
                }

                if !authored.isEmpty {
                    Section {
                        ForEach(authored, id: \.id) { item in
                            Text(item.name).font(.footnote)
                        }
                        .onDelete { offsets in
                            for i in offsets {
                                let id = authored[i].id
                                Task { try? await home.removeAutomation(id: id); refresh() }
                            }
                        }
                    } header: {
                        Text("Told so far")
                    } footer: {
                        Text("Only automations this app authored are listed or removable here. The household's own stay untouched.")
                    }
                }
            }
            .navigationTitle("Tell the house")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .task { refresh() }
            .onChange(of: signal) { _, _ in
                written = false
                errorLine = nil
                sources = home.automationSources(for: signal)
            }
        }
    }

    /// Only consented signals are offered; tamper is always in the set,
    /// mirroring the projection's refusal to go quiet.
    private var offerableSignals: [HomeSignal] {
        HomeSignal.allCases.filter { home.enabledSignals.contains($0) || $0 == .tamper }
    }

    private var plan: PlannedAutomation? {
        guard let sceneChoice, let accessory = sources.first else { return nil }
        return PlannedAutomation(
            homeID: UUID(), homeName: "",
            accessoryName: accessory, signal: signal,
            sceneID: sceneChoice.id, sceneName: sceneChoice.name)
    }

    private func refresh() {
        sources = home.automationSources(for: signal)
        scenes = home.userScenes()
        authored = home.authoredAutomations()
    }

    private func write(_ plan: PlannedAutomation) {
        errorLine = nil
        Task {
            do {
                try await home.author(plan)
                written = true
                refresh()
            } catch let e as HomeAuthorError {
                errorLine = e.line
            } catch {
                errorLine = "Apple Home declined: \(error.localizedDescription)"
            }
        }
    }
}

#Preview("Concierge — no HomeKit touched") {
    // A private instance, never .shared: previews run unsigned, and the
    // bridge must stay untouched until a human flips the toggle.
    AutomationConciergeSheet(home: HomeKitBridge())
}
