// KeysView.swift
//
// The part no web SPA can do well: your key ring. Pinned-key trust (TOFU) with
// a loud "this key changed" alarm, the sealed-snapshot key and the on-phone
// .svlt unseal (Views/UnsealView.swift), and the self-healing About panel —
// build rev, firmware train, last-checked, "heals forward" — ported from the
// desktop app's renderAbout() so every surface tells the same story from one
// source of truth.

import SwiftUI

/// The typed destinations this tab pushes. A value, so a `.svlt` handed in
/// from Files can push the Unseal screen the same way a tap does.
enum KeysRoute: Hashable {
    case unseal
}

struct KeysView: View {
    @EnvironmentObject var store: FleetStore
    /// Owned here so a sealed file arriving from outside can land on the
    /// Unseal screen without a second copy of it: the push is a path append,
    /// and an already-open screen is left alone (it consumes the file itself).
    @State private var path = NavigationPath()
    /// The snapshot key's custody, read on appear (public/first-byte reads
    /// only — never an unwrap, never a prompt). Nil when there is no key.
    @State private var snapshotCustody: String?

    var body: some View {
        NavigationStack(path: $path) {
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
                    NavigationLink(value: KeysRoute.unseal) {
                        Label("Unseal a snapshot", systemImage: "lock.open.rotation")
                    }
                    if let snapshotCustody {
                        Label("Snapshot key: \(snapshotCustody)", systemImage: "lock.shield")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                } header: {
                    Text("Sealed snapshots")
                } footer: {
                    Text("Sealed snapshots are encrypted to a key this phone holds. The Canary keeps only the public half — it's structurally unable to open them. Unsealing happens here, on this phone: shown once, never saved, never shared.")
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
            .navigationDestination(for: KeysRoute.self) { route in
                switch route {
                case .unseal: UnsealView()
                }
            }
            .onAppear {
                snapshotCustody = Self.custodyNote(VaultKeyStore.app)
                openUnsealIfFilePending()
            }
            .onChange(of: store.pendingSealedSnapshot) { _, _ in openUnsealIfFilePending() }
        }
    }

    /// Where the snapshot key's protection lives, in a few words — or nil
    /// when this phone has no snapshot key. Static + pure over the store so
    /// the tests can hold the copy to the custody it names.
    nonisolated static func custodyNote(_ keys: VaultKeyStore) -> String? {
        guard keys.exists else { return nil }
        return keys.custody?.shortLabel ?? "Keychain · this device only"
    }

    /// A `.svlt` arrived (RootView's `.onOpenURL`): make sure the Unseal
    /// screen is up so it can consume the file. When it is already up, it
    /// sees the change itself — pushing again would stack a second copy.
    private func openUnsealIfFilePending() {
        guard store.pendingSealedSnapshot != nil, path.isEmpty else { return }
        path.append(KeysRoute.unseal)
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
