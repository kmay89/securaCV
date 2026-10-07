// CanarySetupView.swift
//
// The Canary walkthrough: pick what you have, then the steps for that
// family (SetupGuide.canary), with the live part under each — read the
// setup key (scan the glass QR or type it), hand the Canary your Wi-Fi
// (its setup network, or the WAP's Bluetooth provisioning service), and
// watch it appear on the network. Every family's path is the one its
// firmware actually serves; nothing here promises a mechanism a device in
// front of you does not have.

import SwiftUI

struct CanarySetupView: View {
    var body: some View {
        List {
            Section {
                ForEach(CanaryFamily.allCases) { f in
                    NavigationLink(value: f) {
                        HStack(spacing: Theme.m) {
                            DeviceFigureIcon(f.deviceType, size: 36)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(f.title).font(.body)
                                Text(f.tagline).font(.caption).foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            } header: {
                Text("Which Canary is it?")
            } footer: {
                Text("Each family joins its own way — a display shows a QR on its glass, a camera or radar Canary's key comes from the Flasher that hatched it, and a WAP listens over Bluetooth. Pick yours and the steps fit.")
            }
        }
        .navigationTitle("A Canary")
        .navigationBarTitleDisplayMode(.inline)
        .navigationDestination(for: CanaryFamily.self) { CanaryFamilySetupView(family: $0) }
    }
}

struct CanaryFamilySetupView: View {
    let family: CanaryFamily
    @EnvironmentObject var store: FleetStore
    @StateObject private var portal = SetupPortalClient()

    @State private var setupSSID = ""
    @State private var setupKey = ""
    @State private var homeSSID = ""
    @State private var homePassword = ""
    @State private var showPassword = false
    @State private var showingScanner = false
    @State private var scanNote: String?
    @State private var bleTarget: String?
    @State private var bleOutcome: BLEProvisionOutcome?
    @State private var bleRunning = false

    var body: some View {
        List {
            ForEach(Array(SetupGuide.canary(family).enumerated()), id: \.element.id) { i, step in
                SetupStepSection(index: i + 1, step: step) {
                    switch step.action {
                    case .readSetupKey: keyControl
                    case .joinSetupNetwork: joinControl
                    case .bluetoothProvision: bluetoothControl
                    case .watchForCanary: appearControl
                    default: EmptyView()
                    }
                }
            }
        }
        .navigationTitle(family.deviceType.role)
        .navigationBarTitleDisplayMode(.inline)
        .sheet(isPresented: $showingScanner) {
            NavigationStack {
                SetupQRScannerSheet { code in
                    showingScanner = false
                    if let qr = SetupPortal.parseWiFiQR(code) {
                        setupSSID = qr.ssid
                        setupKey = qr.password
                        scanNote = SetupPortal.isSetupNetwork(qr.ssid)
                            ? "Read \(qr.ssid) off the glass."
                            : "That QR names \(qr.ssid), which isn't a Canary's setup network — it can still be tried."
                    } else {
                        scanNote = "That code isn't a Wi-Fi QR. The glass shows one named SecuraCV-XXXX."
                    }
                }
            }
        }
    }

    // MARK: - the key

    @ViewBuilder private var keyControl: some View {
        if case .setupNetwork(let source) = family.path, source == .glassQR, SetupQRScannerSheet.isSupported {
            Button {
                showingScanner = true
            } label: {
                Label("Scan the QR on its glass", systemImage: "qrcode.viewfinder")
            }
        }
        TextField("Setup network (SecuraCV-XXXX)", text: $setupSSID)
            .textInputAutocapitalization(.never).autocorrectionDisabled()
        TextField("Its key", text: $setupKey)
            .textInputAutocapitalization(.never).autocorrectionDisabled()
            .font(.body.monospaced())
        if let scanNote {
            Text(scanNote).font(.caption).foregroundStyle(.secondary)
        }
        if !setupSSID.isEmpty && !SetupPortal.isSetupNetwork(setupSSID) {
            Text("A Canary's setup network is named SecuraCV- plus four characters.")
                .font(.caption).foregroundStyle(Theme.color(.warn))
        }
    }

    // MARK: - the join

    @ViewBuilder private var joinControl: some View {
        wifiFields
        Button {
            let s = setupSSID.trimmingCharacters(in: .whitespaces)
            let k = setupKey.trimmingCharacters(in: .whitespaces)
            let h = homeSSID.trimmingCharacters(in: .whitespaces)
            let p = homePassword
            Task { await portal.provision(setupSSID: s, setupKey: k, homeSSID: h, homePassword: p) }
        } label: {
            Label("Join it and hand over my Wi-Fi", systemImage: "wifi")
                .frame(maxWidth: .infinity)
        }
        .buttonStyle(.borderedProminent)
        .disabled(setupSSID.isEmpty || setupKey.isEmpty || homeSSID.isEmpty
                  || portal.phase == .joining || portal.phase == .posting || portal.phase == .reachingPortal)
        portalPhaseLine
    }

    @ViewBuilder private var portalPhaseLine: some View {
        switch portal.phase {
        case .idle: EmptyView()
        case .joining: HStack { ProgressView(); Text("Asking iOS to join \(setupSSID)…") }
        case .reachingPortal: HStack { ProgressView(); Text("On it — waiting for the Canary's setup page…") }
        case .posting: HStack { ProgressView(); Text("Handing over your Wi-Fi…") }
        case .waiting(let state):
            HStack {
                ProgressView()
                switch state {
                case .idle: Text("The Canary is about to try…")
                case .connecting: Text("The Canary is joining \(homeSSID)…")
                case .success, .fail: EmptyView()
                }
            }
        case .done:
            Label("It's on \(homeSSID) — its own verdict. Its setup network goes away now.", systemImage: "checkmark.circle")
                .foregroundStyle(Theme.color(.calm))
        case .failed(let why):
            Label(why, systemImage: "exclamationmark.triangle")
                .font(.footnote).foregroundStyle(Theme.color(.warn))
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    @ViewBuilder private var wifiFields: some View {
        TextField("Your Wi-Fi name", text: $homeSSID)
            .textInputAutocapitalization(.never).autocorrectionDisabled()
        HStack {
            if showPassword {
                TextField("Your Wi-Fi password", text: $homePassword)
                    .textInputAutocapitalization(.never).autocorrectionDisabled()
            } else {
                SecureField("Your Wi-Fi password", text: $homePassword)
            }
            Button { showPassword.toggle() } label: {
                Image(systemName: showPassword ? "eye.slash" : "eye").foregroundStyle(.secondary)
            }
            .buttonStyle(.plain)
        }
    }

    // MARK: - Bluetooth (WAP)

    private var bluetoothCandidates: [String] {
        store.ble.provisionableDeviceIDs.sorted()
    }

    @ViewBuilder private var bluetoothControl: some View {
        if store.discoveryConsent != true {
            Text("Bluetooth listening needs discovery on — the same consent the Fleet tab asks for.")
                .font(.footnote).foregroundStyle(.secondary)
            Button("Enable discovery") { store.setDiscoveryConsent(true) }
                .buttonStyle(.borderedProminent)
        } else if bluetoothCandidates.isEmpty {
            HStack(spacing: Theme.s) {
                ProgressView()
                Text("Listening for a Canary WAP over Bluetooth… power it on nearby.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
        } else {
            Picker("Canary", selection: Binding(
                get: { bleTarget ?? bluetoothCandidates.first ?? "" },
                set: { bleTarget = $0 })) {
                ForEach(bluetoothCandidates, id: \.self) { Text($0).tag($0) }
            }
            wifiFields
            Button {
                let target = bleTarget ?? bluetoothCandidates.first ?? ""
                let h = homeSSID.trimmingCharacters(in: .whitespaces)
                let p = homePassword
                bleRunning = true
                bleOutcome = nil
                Task {
                    let outcome = await store.ble.writeWiFiCredentials(deviceID: target, ssid: h, password: p)
                    bleOutcome = outcome
                    bleRunning = false
                }
            } label: {
                Label("Send my Wi-Fi over Bluetooth", systemImage: "dot.radiowaves.left.and.right")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(homeSSID.isEmpty || bleRunning)
            if bleRunning {
                HStack { ProgressView(); Text("Bonding, sending, waiting for the Canary's verdict (up to 75 s)…") }
            }
            if let bleOutcome {
                switch bleOutcome {
                case .joined:
                    Label("It's on \(homeSSID) — the Canary said so itself.", systemImage: "checkmark.circle")
                        .foregroundStyle(Theme.color(.calm))
                case .failed(let why):
                    Label(why, systemImage: "exclamationmark.triangle")
                        .font(.footnote).foregroundStyle(Theme.color(.warn))
                case .unreachable:
                    Label("Bluetooth couldn't reach it — move closer and try again.", systemImage: "exclamationmark.triangle")
                        .font(.footnote).foregroundStyle(Theme.color(.warn))
                }
            }
        }
    }

    // MARK: - watching it appear

    private var seen: [DiscoveredCanary] {
        store.discovery.found.filter { $0.deviceType == family.deviceType || family.deviceType == .display && $0.deviceType == .nightlight }
    }

    @ViewBuilder private var appearControl: some View {
        if store.discoveryConsent != true {
            Button("Enable discovery to watch for it") { store.setDiscoveryConsent(true) }
                .buttonStyle(.bordered)
        } else if seen.isEmpty {
            HStack(spacing: Theme.s) {
                ProgressView()
                Text("Watching this network for a \(family.deviceType.role.lowercased())…")
                    .font(.footnote).foregroundStyle(.secondary)
            }
        } else {
            ForEach(seen) { d in
                HStack(spacing: Theme.m) {
                    DeviceFigureIcon(d.deviceType, published: d.publishedType, hardware: d.hardware, size: 28)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(d.name).font(.body)
                        Text("on your network · \(d.firmware)").font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Image(systemName: "checkmark.circle.fill").foregroundStyle(Theme.color(.calm))
                }
            }
            Text("It's on the Fleet tab now.").font(.caption).foregroundStyle(.secondary)
        }
        if let login = HubSecretStore.brokerLogin(), family == .vision || family == .sense || family == .wap {
            VStack(alignment: .leading, spacing: 2) {
                Text("Broker login for its MQTT fields").font(.caption.weight(.medium))
                Text("username \(login.username)").font(.caption.monospaced())
                Text("password \(login.password)").font(.caption.monospaced())
                    .privacySensitive()
            }
            .padding(.top, Theme.xs)
        }
    }
}

#if DEBUG
#Preview("Canary walkthrough") {
    NavigationStack { CanarySetupView() }.environmentObject(DemoFleet.previewStore())
}
#endif
