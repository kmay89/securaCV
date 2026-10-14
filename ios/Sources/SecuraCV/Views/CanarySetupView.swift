// CanarySetupView.swift
//
// The Canary walkthrough. It starts LISTENING, not asking: a Sense, Vision
// or WAP with its Bluetooth door open names its own family on the air, so
// the first thing on this screen is "near this phone" — the nearby card the
// moment one is heard — and the family list comes second, for a Canary with
// no door (a display) or a door that has closed. Then the steps for that
// family (SetupGuide.canary), with the live part under each: read the setup
// key (scan the glass QR — the scan starts the join by itself — or type
// it), join the Canary's setup network, pick your Wi-Fi from the networks
// the Canary itself can see (prefilled when this phone remembered it), and
// watch THIS Canary appear on the network. Every family's path is the one
// its firmware actually serves; nothing here promises a mechanism a device
// in front of you does not have.

import SwiftUI
import AVFoundation

struct CanarySetupView: View {
    var body: some View {
        List {
            Section {
                NearbyListenControl(what: "it")
            } header: {
                Text("Near this phone")
            } footer: {
                Text("A new Sense, Vision or WAP asks for Wi-Fi over Bluetooth — a card appears here, and on any screen, within seconds.")
            }
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
                Text("No card? Pick yours")
            } footer: {
                Text("A display shows a QR on its glass; the others have a setup network as a fallback.")
            }
        }
        .navigationTitle("Add a Canary")
        .navigationBarTitleDisplayMode(.inline)
        .navigationDestination(for: CanaryFamily.self) { CanaryFamilySetupView(family: $0) }
    }
}

/// The Bluetooth door, in place: ask (the tap is the consent), listen (or
/// say why it can't), and show the card the moment a Canary is heard.
struct NearbyListenControl: View {
    /// How the listening line names the device ("it", "the radar witness").
    var what: String
    @EnvironmentObject var store: FleetStore

    var body: some View {
        if store.discoveryConsent != true {
            VStack(alignment: .leading, spacing: Theme.s) {
                Text("This phone listens for it over Bluetooth and your Wi-Fi — nothing leaves your home. iOS asks for both next.")
                    .font(.footnote).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Button("Find my Canary") { store.setDiscoveryConsent(true) }
                    .buttonStyle(.borderedProminent)
            }
        } else if store.nearbyCanaries.isEmpty {
            ListeningRow(text: "Listening — power \(what) on within a few meters of this phone.",
                         advice: store.bluetoothAdvice)
        } else {
            NearbyCanaryCard()
                .listRowInsets(EdgeInsets())
                .listRowBackground(Color.clear)
        }
    }
}

struct CanaryFamilySetupView: View {
    let family: CanaryFamily
    @EnvironmentObject var store: FleetStore
    @StateObject private var portal = SetupPortalClient()

    @State private var setupSSID = ""
    @State private var setupKey = ""
    /// The typed home network name ("Another name…", or no list came back).
    @State private var homeSSID = ""
    /// The network picked from the Canary's own list ("" = typed).
    @State private var portalChoice = ""
    @State private var homePassword = ""
    @State private var showPassword = false
    @State private var remember = true
    /// Read on appear (prefill), not here: a @State initial value is
    /// evaluated on every init of this view, and the Keychain is not free.
    @State private var remembered: HouseholdWiFi?
    @State private var showingScanner = false
    @State private var scanNote: String?
    @State private var cameraDenied = false
    /// The watch for it on the network gave up waiting (it never spins on).
    @State private var watchTimedOut = false

    var body: some View {
        List {
            ForEach(Array(SetupGuide.canary(family).enumerated()), id: \.element.id) { i, step in
                SetupStepSection(index: i + 1, step: step) {
                    switch step.action {
                    case .nearbyCanary: NearbyListenControl(what: "the \(family.deviceType.role.lowercased())")
                    case .readSetupKey: keyControl
                    case .joinSetupNetwork: joinControl
                    case .watchForCanary: appearControl
                    default: EmptyView()
                    }
                }
            }
        }
        .navigationTitle(family.deviceType.role)
        .navigationBarTitleDisplayMode(.inline)
        .onAppear(perform: prefill)
        .onChange(of: portal.networks) { _, networks in
            // The Canary's own list arrived: preselect the remembered
            // network when it can see it, else the strongest it heard.
            portalChoice = SetupPortal.preselect(networks, remembered: remembered?.ssid) ?? ""
            matchPassword(to: effectiveSSID)
        }
        .onChange(of: portalChoice) { _, choice in matchPassword(to: choice.isEmpty ? homeSSID : choice) }
        .onChange(of: portal.phase) { _, phase in
            guard phase == .done else { return }
            let ssid = effectiveSSID
            if HouseholdWiFiStore.shouldRemember(toggleOn: remember, joined: true, ssid: ssid) {
                try? HouseholdWiFiStore.save(HouseholdWiFi(ssid: ssid, password: homePassword, savedAt: Date()))
                remembered = HouseholdWiFiStore.load()
            }
            startWatchClock()
        }
        .onDisappear { portal.finish() }
        .sheet(isPresented: $showingScanner) {
            NavigationStack {
                SetupQRScannerSheet { code in
                    showingScanner = false
                    if let qr = SetupPortal.parseWiFiQR(code) {
                        setupSSID = qr.ssid
                        setupKey = qr.password
                        if SetupPortal.isSetupNetwork(qr.ssid) {
                            scanNote = "Read \(qr.ssid) off the glass — joining it now."
                            // A good scan IS the go-ahead: join and list
                            // its networks without another tap.
                            Task { await portal.connect(setupSSID: qr.ssid, setupKey: qr.password) }
                        } else {
                            scanNote = "That QR names \(qr.ssid), which isn't a Canary's setup network — it can still be tried."
                        }
                    } else {
                        scanNote = "That code isn't a Wi-Fi QR. The glass shows one named SecuraCV-XXXX."
                    }
                }
            }
        }
    }

    // MARK: - the household Wi-Fi, remembered

    private var effectiveSSID: String {
        (portalChoice.isEmpty ? homeSSID : portalChoice).trimmingCharacters(in: .whitespaces)
    }

    private var chosenIsOpen: Bool {
        portal.networks.first(where: { $0.ssid == effectiveSSID }).map { !$0.secure } ?? false
    }

    /// Every path that asks for Wi-Fi opens with the remembered one.
    private func prefill() {
        remembered = HouseholdWiFiStore.load()
        if let p = HouseholdWiFiStore.prefill(typedSSID: homeSSID, remembered: remembered) {
            homeSSID = p.ssid
            homePassword = p.password
        }
    }

    /// The remembered password belongs to the remembered network only: pick
    /// another and the field clears, pick it back and the password returns.
    private func matchPassword(to ssid: String) {
        guard let remembered else { return }
        if ssid == remembered.ssid {
            if homePassword.isEmpty { homePassword = remembered.password }
        } else if homePassword == remembered.password {
            homePassword = ""
        }
    }

    // MARK: - the key

    @ViewBuilder private var keyControl: some View {
        if case .setupNetwork(let source) = family.path, source == .glassQR, SetupQRScannerSheet.deviceCanScan {
            Button {
                Task { await scanTapped() }
            } label: {
                Label("Scan the QR on its glass", systemImage: "qrcode.viewfinder")
            }
            if cameraDenied {
                RadioAdviceRow(advice: RadioAdvice(text: CameraGate.deniedNote, opensSettings: true))
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

    /// The camera is asked for at the tap that needs it — never earlier,
    /// and a "no" leaves the typed fields and a way to Settings.
    private func scanTapped() async {
        let standing: CameraGate.Standing
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized: standing = .authorized
        case .notDetermined: standing = .notDetermined
        default: standing = .denied
        }
        switch CameraGate.action(for: standing) {
        case .scan:
            cameraDenied = false
            showingScanner = true
        case .askThenScan:
            if await AVCaptureDevice.requestAccess(for: .video) {
                cameraDenied = false
                showingScanner = true
            } else {
                cameraDenied = true
            }
        case .explainDenied:
            cameraDenied = true
        }
    }

    // MARK: - the join

    private var portalBusy: Bool {
        switch portal.phase {
        case .joining, .reachingPortal, .listing, .posting, .waiting: return true
        default: return false
        }
    }

    @ViewBuilder private var joinControl: some View {
        if !portal.onSetupNetwork && portal.phase != .done {
            Button {
                let s = setupSSID.trimmingCharacters(in: .whitespaces)
                let k = setupKey.trimmingCharacters(in: .whitespaces)
                Task { await portal.connect(setupSSID: s, setupKey: k) }
            } label: {
                Label("Join its setup network", systemImage: "wifi")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(setupSSID.isEmpty || setupKey.isEmpty || portalBusy)
        }
        if portal.onSetupNetwork && portal.phase != .done {
            wifiFields
            Button {
                let h = effectiveSSID
                let p = homePassword
                Task { await portal.hand(homeSSID: h, homePassword: p) }
            } label: {
                Label("Join \(effectiveSSID.isEmpty ? "my Wi-Fi" : effectiveSSID)", systemImage: "wifi")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(effectiveSSID.isEmpty || portalBusy)
        }
        portalPhaseLine
    }

    @ViewBuilder private var portalPhaseLine: some View {
        switch portal.phase {
        case .idle, .ready: EmptyView()
        case .joining: HStack { ProgressView(); Text("Asking iOS to join \(setupSSID)…") }
        case .reachingPortal: HStack { ProgressView(); Text("On it — waiting for the Canary's setup page…") }
        case .listing: HStack { ProgressView(); Text("Asking the Canary which networks it can see…") }
        case .posting: HStack { ProgressView(); Text("Handing over your Wi-Fi…") }
        case .waiting(let state):
            HStack {
                ProgressView()
                switch state {
                case .idle: Text("The Canary is about to try…")
                case .connecting: Text("The Canary is joining \(effectiveSSID)…")
                case .success, .fail: EmptyView()
                }
            }
        case .done:
            Label("It's on \(effectiveSSID) — its own verdict. Its setup network goes away now, and this phone is back on your Wi-Fi.", systemImage: "checkmark.circle")
                .foregroundStyle(Theme.color(.calm))
        case .failed(let why):
            Label(why, systemImage: "exclamationmark.triangle")
                .font(.footnote).foregroundStyle(Theme.color(.warn))
                .fixedSize(horizontal: false, vertical: true)
            if portal.onSetupNetwork {
                Text("Fix it above and tap Join again — the phone is still on its setup network.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder private var wifiFields: some View {
        if !portal.networks.isEmpty {
            Picker("Network", selection: $portalChoice) {
                ForEach(portal.networks, id: \.ssid) { n in
                    Text(n.secure ? n.ssid : "\(n.ssid) (open)").tag(n.ssid)
                }
                Text("Another name…").tag("")
            }
        }
        if portal.networks.isEmpty || portalChoice.isEmpty {
            TextField("Your Wi-Fi name", text: $homeSSID)
                .textInputAutocapitalization(.never).autocorrectionDisabled()
        }
        if !chosenIsOpen {
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
        ForEach(preflightHints, id: \.self) { hint in
            Label(hint, systemImage: "info.circle")
                .font(.footnote).foregroundStyle(Theme.color(.warn))
                .fixedSize(horizontal: false, vertical: true)
        }
        Toggle("Remember for the next Canary", isOn: $remember)
        if let remembered, effectiveSSID == remembered.ssid {
            Text("Prefilled from the last Canary (\(remembered.ssid)), kept in this phone's Keychain only.")
                .font(.caption).foregroundStyle(.secondary)
        }
    }

    /// The cause most likely to fail the join, named before it is sent.
    private var preflightHints: [String] {
        var out: [String] = []
        let listed = portal.networks.map(\.ssid)
        if let hint = SetupPortal.notListedHint(ssid: effectiveSSID, listed: listed) {
            out.append(hint)
        } else if let remembered, remembered.ssid != effectiveSSID,
                  let hint = SetupPortal.notListedHint(ssid: remembered.ssid, listed: listed) {
            out.append(hint)
        }
        if let hint = SetupPortal.passwordHint(homePassword, networkIsOpen: chosenIsOpen) {
            out.append(hint)
        }
        return out
    }

    // MARK: - watching it appear

    /// The four identity characters of the setup network this phone used —
    /// the same characters the device's mDNS host starts its last label
    /// with (NearbyCanaries.isSameDevice).
    private var joinedSuffix: String? {
        NearbyCanaries.suffix(ofSetupSSID: setupSSID.trimmingCharacters(in: .whitespaces))
    }

    private func isFamily(_ d: DiscoveredCanary) -> Bool {
        d.deviceType == family.deviceType || family.deviceType == .display && d.deviceType == .nightlight
    }

    /// THIS Canary on the network — matched by identity, never by kind: a
    /// Vision you already own is not the one you just set up.
    private var seen: [DiscoveredCanary] {
        guard let suffix = joinedSuffix else { return [] }
        return store.discovery.found.filter { isFamily($0) && NearbyCanaries.isSameDevice(suffix: suffix, host: $0.host) }
    }

    /// The same kind of Canary on the network, when this walkthrough never
    /// learned which unit is the new one (it was set up from the card, or
    /// by the Flasher) — listed, never ticked.
    private var sameKind: [DiscoveredCanary] {
        store.discovery.found.filter(isFamily)
    }

    @ViewBuilder private var appearControl: some View {
        if store.discoveryConsent != true {
            Button("Enable discovery to watch for it") { store.setDiscoveryConsent(true) }
                .buttonStyle(.bordered)
        } else if !seen.isEmpty {
            ForEach(seen) { d in discoveredRow(d, mine: true) }
            Text("It's on the Fleet tab now.").font(.caption).foregroundStyle(.secondary)
        } else if let advice = store.localNetworkAdvice {
            RadioAdviceRow(advice: advice)
        } else if joinedSuffix == nil && !sameKind.isEmpty {
            ForEach(sameKind) { d in discoveredRow(d, mine: false) }
            Text("On your network now. A new one shows here — and on the Fleet tab — once it joins.")
                .font(.caption).foregroundStyle(.secondary)
        } else if watchTimedOut {
            Text("Not seen on this network yet. Check this phone is on the same Wi-Fi — it can take a couple of minutes to announce itself, and it shows on the Fleet tab when it does.")
                .font(.footnote).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        } else {
            ListeningRow(text: "Watching this network for the \(family.deviceType.role.lowercased())…", advice: nil)
        }
        if let login = HubSecretStore.brokerLogin(), family == .vision || family == .sense {
            VStack(alignment: .leading, spacing: Theme.xs) {
                Text("Hub login for the Flasher's MQTT fields: \(login.username)")
                    .font(.caption.weight(.medium))
                BrokerPasswordCopyButton(password: login.password)
                    .font(.caption)
            }
            .padding(.top, Theme.xs)
        }
    }

    private func discoveredRow(_ d: DiscoveredCanary, mine: Bool) -> some View {
        HStack(spacing: Theme.m) {
            DeviceFigureIcon(d.deviceType, published: d.publishedType, hardware: d.hardware, size: 28)
            VStack(alignment: .leading, spacing: 2) {
                Text(d.name).font(.body)
                Text("on your network · \(d.firmware)").font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if mine {
                Image(systemName: "checkmark.circle.fill").foregroundStyle(Theme.color(.calm))
            }
        }
    }

    /// The network watch ends in words, not an endless spinner.
    private func startWatchClock() {
        watchTimedOut = false
        Task {
            try? await Task.sleep(for: .seconds(90))
            watchTimedOut = true
        }
    }
}

#if DEBUG
#Preview("Canary walkthrough") {
    NavigationStack { CanarySetupView() }.environmentObject(DemoFleet.previewStore())
}
#endif
