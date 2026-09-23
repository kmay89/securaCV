//  WallSettingsView.swift — the Wall's one panel.
//
//  The same controls the Mac app's Witness Wall view offers, arranged the way
//  tvOS arranges settings: which room this TV serves, how the wall dresses,
//  what earns a banner, where the fleet answers, and — the one section the
//  Mac view has no twin for — the pairing that lets this TV verify the hub's
//  sealed record itself. Everything else the Wall decides from the data, on
//  purpose; the pairing's decisions live in WallModel.pair / forgetPairing.
//
//  The header chips (WallView.stylePicker) stay: a click there cycles profile
//  or skin without opening anything. This panel is where you SEE the choices —
//  each room described, each skin swatched — and the only place the hub
//  address can be changed while the Wall is live, which used to require
//  unplugging the hub to reach the prompt.

import SwiftUI

struct WallSettingsView: View {
    let model: WallModel

    @AppStorage("SecuraCVWallProfile") private var profileRaw = WallProfile.home.rawValue
    @AppStorage("SecuraCVWallSkin") private var skinRaw = WallSkin.midnight.rawValue
    /// "Notifications" on a television means what the glass raises a banner
    /// for. Off by default: a bedroom wall should not shout about a Canary
    /// that sleeps; a bar can turn it on.
    @AppStorage("SecuraCVWallOfflineBanner") private var offlineBanner = false

    @Environment(\.dismiss) private var dismiss
    @State private var typedAddress = ""
    /// The pasted pairing receipt, and the sentence the model answered with
    /// when it could not pair (nil while there is nothing to say).
    @State private var receiptText = ""
    @State private var pairingProblem: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Picker("Room", selection: $profileRaw) {
                        ForEach(WallProfile.allCases) { profile in
                            VStack(alignment: .leading, spacing: 4) {
                                Text(profile.label)
                                Text(profile.blurb)
                                    .font(.caption)
                                    .foregroundStyle(.secondary)
                            }
                            .tag(profile.rawValue)
                        }
                    }
                    .pickerStyle(.inline)
                } header: {
                    Text("This room")
                } footer: {
                    Text("A room changes layout and emphasis, never the data — Business can't claim anything Home wouldn't.")
                }

                Section("Look") {
                    Picker("Skin", selection: $skinRaw) {
                        ForEach(WallSkin.allCases) { skin in
                            HStack(spacing: 14) {
                                SkinSwatch(skin: skin)
                                Text(skin.label)
                            }
                            .tag(skin.rawValue)
                        }
                    }
                    .pickerStyle(.inline)
                }

                Section {
                    Toggle("A Canary going offline raises a banner", isOn: $offlineBanner)
                } header: {
                    Text("Attention")
                } footer: {
                    Text("A record that doesn't verify always raises one — that's the Wall being honest, not a setting.")
                }

                Section {
                    LabeledContent("Connected to", value: model.hubAddress.isEmpty ? "Searching…" : model.hubAddress)
                    TextField("http://canary.local:8099", text: $typedAddress)
                        .textContentType(.URL)
                    Button("Connect") {
                        model.connect(to: typedAddress)
                        dismiss()
                    }
                    .disabled(typedAddress.trimmingCharacters(in: .whitespaces).isEmpty)
                } header: {
                    Text("Fleet")
                } footer: {
                    // No unconditional "nothing leaves this room" here: a
                    // remote hub URL is a supported input, and the promise
                    // must survive it. What IS always true: one address,
                    // nothing else, no cloud of ours.
                    // "Publishes a fleet report", not "announces itself":
                    // Vision- and Sense-class Canaries announce the service
                    // but serve no fleet endpoint, and a promise that finds
                    // them would be false. A hub is how THOSE reach a wall.
                    Text("The Wall finds your Canaries by itself — a hub at canary.local, or any Canary that publishes a fleet report on your network. This field is only for a hub that lives somewhere else; typing one replaces the found set. Wherever it points, the Wall talks to your Canaries and nothing else — never a cloud of ours.")
                }

                Section {
                    if let pairing = model.pairing {
                        LabeledContent("Paired", value: "key \(pairing.keyPrefix)… · \(pairing.pairedAt.formatted(date: .abbreviated, time: .shortened))")
                        if let tokenID = pairing.tokenID {
                            LabeledContent("Viewer token", value: tokenID)
                        }
                        Button("Forget pairing", role: .destructive) {
                            model.forgetPairing()
                        }
                    }
                    TextField("Pairing receipt — one line, starts with {", text: $receiptText)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                    Button(model.pairing == nil ? "Pair for verification" : "Pair again") {
                        if let problem = model.pair(receiptText: receiptText) {
                            pairingProblem = problem
                        } else {
                            pairingProblem = nil
                            receiptText = ""
                            dismiss()
                        }
                    }
                    .disabled(receiptText.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    if let pairingProblem {
                        Text(pairingProblem)
                            .font(.callout)
                            .foregroundStyle(.orange)
                    }
                } header: {
                    Text("Verification")
                } footer: {
                    // What the pairing is, what it opens, and what forgetting
                    // does — the whole promise, so nobody pastes a secret
                    // into a television without knowing what it can read.
                    Text("To check your hub's sealed record on this Apple TV — Ed25519 signatures against a key pinned now — mint a viewer token on the hub (witness_api mint-viewer-token --label \"living room tv\"; the Docker sidecar: docker compose exec securacv entrypoint.sh mint-viewer-token …) and paste the one line it prints; your iPhone's keyboard can paste here. The token opens the sealed record and nothing else. Forgetting deletes the token and the pinned key from this Apple TV; revoke it on the hub too.")
                }

                Section("About") {
                    LabeledContent("App", value: Self.appVersion)
                    LabeledContent("Build", value: Self.buildRev)
                    LabeledContent("Firmware train", value: Self.firmwareTrain)
                    LabeledContent("Witness core", value: model.coreVersion)
                    Text("Witnessing without watching — no video on this screen.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Witness Wall")
        }
        .onAppear {
            // Pre-fill only when "connected to" IS an address a person might
            // edit. In discovered multi-source mode `hubAddress` is a display
            // sentence ("2 Canaries · found on your network"), and pre-filling
            // it armed the Connect button with a non-address: pressing it fed
            // the sentence to FleetAddress.normalize, flashed an "isn't an
            // address" error, and dismissed the panel. An empty field shows
            // the placeholder and keeps Connect disabled instead.
            if model.sources.count == 1 { typedAddress = model.hubAddress }
        }
    }

    private static var appVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "dev"
    }

    /// Build identity, baked at build time: scripts/stamp_build.sh hands the
    /// git rev and firmware train to xcodebuild, and Support/Info.plist
    /// carries them into the bundle — so "which build is on this TV?" is
    /// never a guess. Same story every Apple surface's About screen tells
    /// (ios/Shared/BuildInfo.swift); the keys differ because each target
    /// reads its own Info.plist.
    private static var buildRev: String {
        Bundle.main.object(forInfoDictionaryKey: "SecuraCVBuildRev") as? String ?? "dev"
    }

    private static var firmwareTrain: String {
        Bundle.main.object(forInfoDictionaryKey: "SecuraCVFirmwareTrain") as? String ?? "0.x"
    }
}

/// A skin, shown as itself: its background gradient with its healthy accent —
/// the same idea as the Mac view's two-tone chips.
private struct SkinSwatch: View {
    let skin: WallSkin

    var body: some View {
        RoundedRectangle(cornerRadius: 6)
            .fill(LinearGradient(colors: [skin.backgroundTop, skin.backgroundBottom],
                                 startPoint: .top, endPoint: .bottom))
            .frame(width: 56, height: 34)
            .overlay(alignment: .bottomTrailing) {
                Circle().fill(skin.ok).frame(width: 10, height: 10).padding(5)
            }
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(.quaternary))
    }
}
