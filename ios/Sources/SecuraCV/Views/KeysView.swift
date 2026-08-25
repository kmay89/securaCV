// KeysView.swift
//
// The part no web SPA can do well: your key ring. Pinned-key trust (TOFU) with
// a loud "this key changed" alarm, the entry point for the on-device .svlt
// unseal (still a placeholder — UnsealView says so), and the self-healing
// About panel — build rev, firmware train, last-checked, "heals forward" —
// ported from the desktop app's renderAbout() so every surface tells the same
// story from one source of truth.

import SwiftUI

struct KeysView: View {
    @EnvironmentObject var store: FleetStore

    var body: some View {
        NavigationStack {
            List {
                Section("Pinned trust") {
                    if store.witnesses.isEmpty {
                        Text("Pair a Canary and its signing key pins here on first sight.")
                            .font(.subheadline).foregroundStyle(.secondary)
                    }
                    ForEach(store.witnesses) { w in
                        HStack {
                            Image(systemName: w.badge.sfSymbol)
                                .foregroundStyle(w.badge.isTrusted ? Theme.color(.calm) : .secondary)
                            VStack(alignment: .leading) {
                                Text(w.displayName)
                                Text(w.fingerprint.isEmpty ? w.badge.label : w.fingerprint)
                                    .font(.caption.monospaced()).foregroundStyle(.secondary)
                            }
                        }
                    }
                }

                // Section has no title-string + footer initializer — spell
                // the header out (SwiftUI API shape, not a style choice).
                Section {
                    NavigationLink {
                        UnsealView()
                    } label: {
                        Label("Unseal a snapshot", systemImage: "lock.open.rotation")
                    }
                } header: {
                    Text("Vault")
                } footer: {
                    Text("Sealed snapshots are encrypted to your key. The Canary holds only the public half — it's structurally unable to open them. Unsealing in this app is still being built; today the repo's unseal tool is the working path.")
                }

                Section {
                    NavigationLink {
                        AppleHomeView()
                    } label: {
                        Label("Apple Home", systemImage: "house")
                    }
                } header: {
                    Text("The house")
                } footer: {
                    Text("Publish the fleet's coarse signals into the Home app so automations can answer the witness. Off until you turn it on; the house learns booleans, never footage.")
                }

                // The family, named from the app's most personal surface —
                // the fix for the audit finding that every SecuraCV surface
                // was engineered to agree with the others and none ever told
                // a user the others exist. Routing, never a funnel: no
                // account, no store push, honest availability on every row.
                Section {
                    ForEach(EcosystemMap.surfaces) { surface in
                        Link(destination: surface.url) {
                            VStack(alignment: .leading, spacing: 2) {
                                Label(surface.name, systemImage: surface.sfSymbol)
                                Text(surface.job)
                                    .font(.caption).foregroundStyle(.secondary)
                                Text(surface.availability)
                                    .font(.caption2).foregroundStyle(.tertiary)
                            }
                        }
                        .foregroundStyle(.primary)
                    }
                } header: {
                    Text("SecuraCV everywhere")
                } footer: {
                    Text("One fleet, several windows onto it — this phone for living with it, the Flasher for hatching and tending, the Lab for learning, the Wall for the shared screen. All free; none needs an account.")
                }

                AboutSection()
            }
            .navigationTitle("Keys")
        }
    }
}

/// Placeholder unseal surface — and it SAYS so (the honest-status doctrine):
/// there is no importer and no decrypt code in this app yet. The crypto is
/// already built repo-side (tools/unseal_snapshot.py); when the flow lands
/// here it will import a .svlt from Files and decrypt on this phone.
struct UnsealView: View {
    var body: some View {
        ContentUnavailableView("Unsealing isn't in the app yet",
            systemImage: "doc.badge.gearshape",
            description: Text("This screen will import a .svlt from Files and decrypt it on this phone, never through any cloud. Until it lands, the repo's unseal tool (tools/unseal_snapshot.py) is the working path."))
            .navigationTitle("Unseal")
    }
}

/// The self-healing About/Health panel — mirrors desktop/src/app.js renderAbout().
struct AboutSection: View {
    var body: some View {
        Section("About") {
            LabeledContent("Version", value: BuildInfo.version)
            LabeledContent("Build", value: BuildInfo.buildRev)
            LabeledContent("Firmware train", value: BuildInfo.firmwareTrain)
            VStack(alignment: .leading, spacing: 4) {
                Label("Heals forward", systemImage: "arrow.triangle.2.circlepath")
                Text("The app renders what each Canary describes, so new firmware features light up here without an App Store update. It fails quietly when offline — never an alarm you didn't earn.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}

// BuildInfo moved to Shared/BuildInfo.swift — the watch About screen shows
// the same stamp, from the same code.

#Preview("Keys — demo fleet") {
    KeysView().environmentObject(DemoFleet.previewStore())
}
