// UnsealView.swift
//
// The sealed-snapshot screen: the whole loop, key-first. This phone makes
// and keeps the X25519 operator key (VaultKeyStore); a paired Canary is
// handed the PUBLIC half (`POST /api/vault/key`); the frames it then seals
// are listed and pulled from it (`/api/vault/list`, `/download`) or imported
// from Files, and opened HERE (SnapshotSealer) — shown once, full screen,
// and discarded on Done, on leaving the screen, and on backgrounding. Never
// written to disk, Photos, the pasteboard or any cloud; there is no share
// button by construction (RFC §4 ④: "shown once, never written").
//
// Vocabulary, kept apart on purpose: a *sealed snapshot* is one frame one
// Canary encrypted to one phone's key. The kernel's evidence *vault* opens
// only by quorum (Invariant V) and this screen has no path into it.
//
// Handling raw witnessed media is the most sensitive thing the app does,
// so the rules are structural, not procedural: the frame lives in one
// @Published var, every exit path nils it, and the only thing that can
// read it is the cover that shows it.
//
// One trap, named so nobody "fixes" it back: a full-screen cover removes
// the screen under it from the window, and SwiftUI then runs THAT screen's
// onDisappear. A discard hung there would close the frame the instant it
// opened. So the exits live on the cover itself — Done, its own
// onDisappear, the scene leaving `.active` — and the cover is all there is
// on screen while a frame exists: no tab bar, no back button, no way off it
// that is not one of those.

import CryptoKit
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct UnsealView: View {
    /// The exported type Info.plist declares (UTExportedTypeDeclarations).
    /// `exportedAs:` because the type is ours — the system has no name for
    /// a Canary's sealed frame — and the importer filters on it alone.
    static let contentType = UTType(exportedAs: SvltFile.typeIdentifier, conformingTo: .data)

    @EnvironmentObject var store: FleetStore
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var model = UnsealModel()
    @State private var importing = false
    @State private var confirmingForget = false
    @State private var confirmingReplace = false
    @State private var replaceTarget: UnsealModel.CanarySnapshots?

    var body: some View {
        List {
            keySection
            if model.keyIDHex != nil {
                registerSection
                snapshotsSection
            }
            importSection
        }
        .navigationTitle("Unseal")
        .task {
            model.load()
            await model.refresh(devices: store.devices)
        }
        .refreshable { await model.refresh(devices: store.devices) }
        .fileImporter(isPresented: $importing,
                      allowedContentTypes: [Self.contentType]) { result in
            switch result {
            case .success(let url): model.importFile(at: url)
            case .failure(let error): model.problem = error.localizedDescription
            }
        }
        .fullScreenCover(isPresented: Binding(get: { model.frame != nil },
                                              set: { if !$0 { model.discard() } })) {
            if let frame = model.frame {
                UnsealedFrameView(frame: frame) { model.discard() }
            }
        }
        .alert("Couldn't do that", isPresented: Binding(get: { model.problem != nil },
                                                        set: { if !$0 { model.problem = nil } })) {
            Button("OK", role: .cancel) { model.problem = nil }
        } message: {
            Text(model.problem ?? "")
        }
        .confirmationDialog("Forget this phone's snapshot key?", isPresented: $confirmingForget,
                            titleVisibility: .visible) {
            Button("Forget the key", role: .destructive) { model.forgetKey() }
        } message: {
            Text(VaultKeyStore.forgetWarning)
        }
        // `presenting:` hands the button the target it was opened for, so
        // the dismissal clearing the state can never race the action.
        .confirmationDialog("Replace the key on \(replaceTarget?.ref.name ?? "this Canary")?",
                            isPresented: $confirmingReplace,
                            titleVisibility: .visible,
                            presenting: replaceTarget) { target in
            Button("Replace with this phone's key", role: .destructive) {
                Task { await model.register(on: target, devices: store.devices) }
            }
        } message: { _ in
            Text("Snapshots it already sealed stay locked to the other key. New ones will seal to this phone.")
        }
        .onAppear { consumePending() }
        .onChange(of: store.pendingSealedSnapshot) { _, _ in consumePending() }
        // Backgrounding is an exit (the cover watches the same phase and
        // blanks itself first — see UnsealedFrameView).
        .onChange(of: scenePhase) { _, phase in
            if phase != .active { model.discard() }
        }
    }

    /// Land a `.svlt` handed in from Files / Mail / AirDrop: consume the
    /// pending URL and CLEAR it (the `pendingRoute` contract), then open it.
    private func consumePending() {
        guard let url = store.pendingSealedSnapshot else { return }
        store.pendingSealedSnapshot = nil
        model.importFile(at: url)
    }

    // MARK: - sections

    private var keySection: some View {
        Section {
            if let keyID = model.keyIDHex, let hex = model.publicKeyHex {
                LabeledContent("Key id") {
                    Text(keyID).font(.body.monospaced())
                }
                VStack(alignment: .leading, spacing: 4) {
                    Text("Public key").foregroundStyle(.secondary).font(.subheadline)
                    Text(hex)
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                }
                Button {
                    // The PUBLIC half only — the same string a Canary's
                    // dashboard asks to be pasted. The private key has no
                    // export path anywhere in this app.
                    UIPasteboard.general.string = hex
                } label: {
                    Label("Copy public key", systemImage: "doc.on.doc")
                }
                Label(model.custodyLine, systemImage: "lock.iphone")
                    .font(.footnote).foregroundStyle(.secondary)
                Button(role: .destructive) { confirmingForget = true } label: {
                    Label("Forget this key…", systemImage: "key.slash")
                }
            } else {
                Text("This phone has no snapshot key yet. Create one, then register it on a Canary so the frames it seals can only be opened here.")
                    .font(.subheadline).foregroundStyle(.secondary)
                Button {
                    model.createKey()
                } label: {
                    Label("Create key", systemImage: "key.fill")
                }
            }
        } header: {
            Text("Your snapshot key")
        } footer: {
            Text("Made here, kept here. A Canary is handed only the public half, so it can seal a frame to you and never open it. Losing this phone's key means losing every snapshot sealed to it — there is no copy anywhere.")
        }
    }

    private var registerSection: some View {
        Section {
            if model.canaries.isEmpty {
                Text(model.loading ? "Asking your Canaries…" : "No paired Canary answers for sealed snapshots. Pair a canary-wap and it appears here.")
                    .font(.subheadline).foregroundStyle(.secondary)
            }
            ForEach(model.canaries) { canary in
                registrationRow(canary)
            }
        } header: {
            Text("Registered on")
        } footer: {
            Text("Registering hands the Canary this phone's public key and nothing else. Which alarms seal a frame (smoke, CO, glass, presence, mesh) is chosen on the Canary's own dashboard — every one starts off.")
        }
    }

    @ViewBuilder
    private func registrationRow(_ canary: UnsealModel.CanarySnapshots) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(canary.ref.name)
                Spacer()
                if canary.working {
                    ProgressView()
                }
            }
            switch canary.registration(ourKeyID: model.keyIDHex) {
            case .unreachable(let why):
                Label(why, systemImage: "wifi.slash")
                    .font(.footnote).foregroundStyle(.secondary)
            case .noSnapshots:
                Label("Its firmware doesn't seal snapshots", systemImage: "camera.metering.none")
                    .font(.footnote).foregroundStyle(.secondary)
            case .thisPhone:
                Label("Registered: this phone's key", systemImage: "checkmark.seal")
                    .font(.footnote).foregroundStyle(Theme.color(.calm))
            case .otherKey(let id):
                Label("Registered: a different key (\(id))", systemImage: "key")
                    .font(.footnote).foregroundStyle(Theme.color(.warn))
                Button("Replace with this phone's key…") {
                    replaceTarget = canary
                    confirmingReplace = true
                }
                .font(.footnote)
                .disabled(canary.working)
            case .noKey:
                Label("No key registered — it seals nothing", systemImage: "key.slash")
                    .font(.footnote).foregroundStyle(.secondary)
                Button("Register this phone's key") {
                    Task { await model.register(on: canary, devices: store.devices) }
                }
                .font(.footnote)
                .disabled(canary.working)
            }
        }
    }

    private var snapshotsSection: some View {
        Section {
            let listed = model.canaries.filter { $0.list != nil }
            if listed.isEmpty {
                Text("Sealed frames from your Canaries are listed here once one holds any.")
                    .font(.subheadline).foregroundStyle(.secondary)
            }
            ForEach(listed) { canary in
                if canary.list?.sdOK == false {
                    Label("\(canary.ref.name): SD card unavailable — nothing can be sealed or listed.",
                          systemImage: "sdcard")
                        .font(.footnote).foregroundStyle(.secondary)
                } else if let items = canary.list?.items, items.isEmpty {
                    Text("\(canary.ref.name): no sealed snapshots.")
                        .font(.footnote).foregroundStyle(.secondary)
                } else {
                    ForEach(canary.list?.items ?? []) { item in
                        snapshotRow(item, on: canary)
                    }
                }
            }
        } header: {
            Text("Sealed snapshots")
        } footer: {
            Text("Each is one frame, sealed by the Canary that took it. Unsealing pulls it here, opens it on this phone and shows it once — it is never saved, shared or sent anywhere.")
        }
    }

    private func snapshotRow(_ item: VaultItem, on canary: UnsealModel.CanarySnapshots) -> some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(item.svltTrigger?.label ?? item.trigger ?? "Sealed frame")
                Text("\(canary.ref.name) · \(item.bucketLabel)")
                    .font(.caption).foregroundStyle(.secondary)
                Text("\(item.name) · \((item.size ?? 0) / 1024) KB")
                    .font(.caption2.monospaced()).foregroundStyle(.tertiary)
            }
            Spacer()
            Button("Unseal") {
                Task { await model.unseal(item, from: canary, devices: store.devices) }
            }
            .buttonStyle(.bordered)
            .disabled(model.busy != nil || model.keyIDHex == nil)
        }
    }

    private var importSection: some View {
        Section {
            Button {
                importing = true
            } label: {
                Label("Import a .svlt from Files", systemImage: "folder")
            }
            .disabled(model.keyIDHex == nil)
            if let busy = model.busy {
                HStack { ProgressView(); Text(busy).font(.footnote).foregroundStyle(.secondary) }
            }
        } header: {
            Text("From a file")
        } footer: {
            Text(model.keyIDHex == nil
                 ? "Create a key first — a file can only be opened by the key it was sealed to."
                 : "A sealed snapshot pulled another way (the Canary's dashboard, AirDrop, Mail) opens the same way. Tapping a .svlt anywhere on this phone lands here too.")
        }
    }
}

// MARK: - the model

@MainActor
final class UnsealModel: ObservableObject {
    /// One paired Canary's sealed-snapshot side, as it last answered.
    struct CanarySnapshots: Identifiable {
        let ref: PairedDeviceRef
        var status: VaultStatus?
        var list: VaultList?
        var sealsNothing = false
        var unreachable: String?
        var working = false

        var id: String { ref.id }

        enum Registration: Equatable {
            case unreachable(String), noSnapshots, thisPhone, otherKey(String), noKey
        }

        func registration(ourKeyID: String?) -> Registration {
            if let unreachable { return .unreachable(unreachable) }
            if sealsNothing { return .noSnapshots }
            guard let status, status.hasKey == true else { return .noKey }
            let theirs = (status.keyID ?? "").lowercased()
            if let ourKeyID, theirs == ourKeyID.lowercased() { return .thisPhone }
            return .otherKey(theirs.isEmpty ? "unknown id" : theirs)
        }
    }

    @Published private(set) var keyIDHex: String?
    @Published private(set) var publicKeyHex: String?
    /// Where the key's protection lives — nil before a key exists.
    @Published private(set) var custody: CustodyKind?
    @Published private(set) var canaries: [CanarySnapshots] = []
    @Published private(set) var loading = false
    /// What the screen is doing right now, when it is doing something.
    @Published private(set) var busy: String?
    /// THE frame. One place; every exit nils it.
    @Published private(set) var frame: UnsealedFrame?
    @Published var problem: String?

    private let keys: VaultKeyStore

    init(keys: VaultKeyStore = .app) {
        self.keys = keys
    }

    /// Where the key lives, in one honest line (CustodyKind.label: "wrapped
    /// by", never "decrypted in").
    var custodyLine: String {
        (custody?.label ?? "Kept in this phone's Keychain, device-only.")
            + " It never syncs to iCloud."
    }

    func load() {
        // A key from before custody wrapping is wrapped now, silently:
        // wrapping needs no presence, only the wrapping key's public half.
        do {
            try keys.migrateIfNeeded()
        } catch {
            problem = "Couldn't protect this phone's snapshot key (\(error.localizedDescription))."
        }
        keyIDHex = keys.keyIDHex
        publicKeyHex = keys.publicKeyHex
        custody = keys.custody
    }

    func createKey() {
        do {
            try keys.generateIfNeeded()
            load()
        } catch {
            problem = "Couldn't create a snapshot key on this phone (\(error.localizedDescription))."
        }
    }

    func forgetKey() {
        discard()
        keys.forget()
        load()
    }

    /// Nil the frame. Called on Done, on leaving, on backgrounding, before
    /// forgetting the key — every path out.
    func discard() { frame = nil }

    /// Ask every paired WAP where it stands: status (registered key), then
    /// its list. A Canary that answers 404 is one whose firmware seals
    /// nothing; one that does not answer is named as unreachable, never
    /// dropped silently.
    func refresh(devices: DeviceStore) async {
        loading = true
        defer { loading = false }
        var rows: [CanarySnapshots] = []
        for ref in devices.devices where ref.deviceType.isHTTPPairable {
            var row = CanarySnapshots(ref: ref)
            guard let api = try? devices.api(for: ref) else {
                row.unreachable = "Not reachable from here (no address or token)"
                rows.append(row)
                continue
            }
            do {
                row.status = try await api.vaultStatus()
                row.list = try await api.vaultList()
            } catch DeviceError.noSealedSnapshots {
                row.sealsNothing = true
            } catch {
                row.unreachable = "Didn't answer: \(error.localizedDescription)"
            }
            rows.append(row)
        }
        canaries = rows
    }

    private func setWorking(_ id: String, _ working: Bool) {
        if let i = canaries.firstIndex(where: { $0.id == id }) { canaries[i].working = working }
    }

    func register(on canary: CanarySnapshots, devices: DeviceStore) async {
        guard let hex = publicKeyHex, let api = try? devices.api(for: canary.ref) else { return }
        setWorking(canary.id, true)
        defer { setWorking(canary.id, false) }
        do {
            let id = try await api.vaultRegisterKey(hex: hex)
            if let ours = keyIDHex, !id.isEmpty, id.lowercased() != ours.lowercased() {
                problem = "\(canary.ref.name) reports key id \(id) after registering, but this phone's key is \(ours). Check the Canary's dashboard."
            }
            // Look the row up AFTER the await: a refresh may have replaced
            // the array meanwhile, and an index taken before it could point
            // past the end of the new one.
            let status = try? await api.vaultStatus()
            if let i = canaries.firstIndex(where: { $0.id == canary.id }) {
                canaries[i].status = status
            }
        } catch {
            problem = error.localizedDescription
        }
    }

    func unseal(_ item: VaultItem, from canary: CanarySnapshots, devices: DeviceStore) async {
        guard let api = try? devices.api(for: canary.ref) else { return }
        busy = "Pulling \(item.name) from \(canary.ref.name)…"
        defer { busy = nil }
        do {
            let data = try await api.vaultDownload(name: item.name)
            await unseal(data: data)
        } catch {
            problem = error.localizedDescription
        }
    }

    /// A file from the Files picker or the `.onOpenURL` door. Security-
    /// scoped access is taken for the read and released after; a copy iOS
    /// placed in THIS APP's Documents/Inbox ("Open in…" from Mail) is
    /// removed once read, so no sealed file lingers in the container.
    func importFile(at url: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let data = try Data(contentsOf: url)
            Task { await unseal(data: data) }
        } catch {
            problem = "Couldn't read \(url.lastPathComponent): \(error.localizedDescription)"
        }
        if Self.isOurInboxCopy(url) {
            try? FileManager.default.removeItem(at: url)
        }
    }

    /// True only for a file inside this app's own Documents/Inbox — the
    /// copy iOS made for us. Matching "/Inbox/" anywhere in the path would
    /// also match a folder of that name the user keeps in Files, and an
    /// opened-in-place file there is the user's own: never ours to delete.
    nonisolated static func isOurInboxCopy(_ url: URL,
                               documents: URL? = FileManager.default.urls(
                                   for: .documentDirectory, in: .userDomainMask).first) -> Bool {
        guard url.isFileURL, let documents else { return false }
        let inbox = documents.appendingPathComponent("Inbox", isDirectory: true)
            .resolvingSymlinksInPath().standardizedFileURL.path
        let file = url.resolvingSymlinksInPath().standardizedFileURL.path
        return file.hasPrefix(inbox + "/")
    }

    /// The one decrypt path. Every check that needs no private key runs
    /// first (header, length, key id — public key only), so a file this
    /// phone cannot open never asks for Face ID. Then presence, once, when
    /// the key's custody needs it; then the unwrap and the tag. The private
    /// key is held for exactly this and dropped when the function returns;
    /// the frame goes to `frame`.
    func unseal(data: Data) async {
        do {
            guard let publicKey = keys.publicKeyRaw else {
                problem = "This phone has no snapshot key, so nothing can be opened here."
                return
            }
            try SnapshotSealer.precheck(file: data, recipientPublicKey: publicKey)
            let context = try await keys.authenticate(reason: "Open a sealed snapshot")
            guard let key = try keys.privateKey(context: context) else {
                problem = "This phone has no snapshot key, so nothing can be opened here."
                return
            }
            frame = try SnapshotSealer.unseal(file: data, key: key)
        } catch {
            problem = error.localizedDescription
        }
    }
}

// MARK: - the cover

/// The frame, shown once. No share, no save, no export: `Done` and every
/// other way off this screen discard it. The app switcher's picture is
/// taken as the scene leaves `.active`; the image is swapped for nothing in
/// that same render (no animation to wait on), and the model's discard
/// follows — so the switcher holds a blank card, not the frame.
struct UnsealedFrameView: View {
    let frame: UnsealedFrame
    let done: () -> Void
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        NavigationStack {
            VStack(spacing: 16) {
                if scenePhase != .active {
                    Color.clear
                } else if let image = UIImage(data: frame.jpeg) {
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFit()
                        .accessibilityLabel("The unsealed snapshot")
                } else {
                    ContentUnavailableView("Opened, but not a picture",
                        systemImage: "photo.badge.exclamationmark",
                        description: Text("The snapshot decrypted and its tag verified, but its \(frame.jpeg.count) bytes aren't an image this phone can show."))
                }
                VStack(spacing: 4) {
                    Text(frame.header.trigger.label).font(.headline)
                    Text("\(frame.header.bucketRange) · a ten-minute bucket is the only time the file carries")
                        .font(.footnote).foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
                Text("Shown once. Not saved, not shared — gone when you tap Done.")
                    .font(.caption).foregroundStyle(.tertiary)
                    .multilineTextAlignment(.center)
            }
            .padding()
            .navigationTitle("Sealed snapshot")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done", action: done)
                }
            }
        }
        .onChange(of: scenePhase) { _, phase in
            if phase != .active { done() }
        }
        .onDisappear(perform: done)
    }
}
