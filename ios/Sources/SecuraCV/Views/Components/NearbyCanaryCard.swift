// NearbyCanaryCard.swift
//
// "A new Canary is nearby." The card that appears when a Canary with its
// Bluetooth setup door open is in range (Shared/NearbyCanary.swift decides
// which), and the sheet one tap opens: the phone connects, asks the Canary
// which networks it sees, the person picks theirs and types the password
// once (or not at all, when this phone remembered it from the last Canary),
// iOS asks to pair, and the Canary answers with its own verdict. A WAP then
// hands this phone a one-time claim over the link, and the phone spends it
// over the home Wi-Fi for the pairing receipt — the key never rides
// Bluetooth (Shared/MagicPairPlan.swift decides each step). Then the Fleet
// tab shows it on its own.
//
// Honesty rules: every line of status is the Canary's word or the phone's
// own action — "joining" when the Canary said provisioning, "on your
// Wi-Fi" when it said provisioned, never a spinner that outlives the person.

import SwiftUI

struct NearbyCanaryCard: View {
    @EnvironmentObject var store: FleetStore
    @State private var target: NearbyCanary?

    var body: some View {
        let nearby = store.nearbyCanaries
        if !nearby.isEmpty {
            Card {
                VStack(alignment: .leading, spacing: Theme.s) {
                    HStack(spacing: Theme.s) {
                        Image(systemName: "dot.radiowaves.left.and.right")
                            .foregroundStyle(Theme.color(.info))
                        Text(nearby.count == 1 ? "A new Canary is nearby" : "\(nearby.count) new Canaries are nearby")
                            .font(.headline)
                    }
                    Text("It has no Wi-Fi yet and is asking for yours over Bluetooth. One tap hands it over; the credentials cross encrypted.")
                        .font(.footnote).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    ForEach(nearby) { c in
                        HStack(spacing: Theme.m) {
                            DeviceFigureIcon(c.family?.deviceType ?? .unknown, size: 32)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(c.title).font(.body)
                                Text("\(c.displayName) · \(signalWord(c.rssiDBM))")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button("Set up") { target = c }
                                .buttonStyle(.borderedProminent)
                            Button {
                                store.dismissNearby(c.suffix)
                            } label: {
                                Image(systemName: "xmark.circle").foregroundStyle(.secondary)
                            }
                            .buttonStyle(.plain)
                            .accessibilityLabel("Not now")
                        }
                    }
                }
            }
            .sheet(item: $target) { c in
                NearbySetupSheet(canary: c)
            }
        }
    }

    private func signalWord(_ rssi: Int) -> String {
        // Bands, strongest first. (A `-55...` pattern parses as `-(55...)`,
        // which the compiler rejects; the guards say the same thing plainly.)
        switch rssi {
        case let r where r >= -55: return "right here"
        case let r where r >= -70: return "close by"
        case let r where r >= -85: return "in the room"
        default: return "faint"
        }
    }
}

/// The ceremony, one Canary at a time.
struct NearbySetupSheet: View {
    let canary: NearbyCanary
    @EnvironmentObject var store: FleetStore
    @Environment(\.dismiss) private var dismiss

    @State private var client: ImprovClient?
    @State private var stage: Stage = .connecting
    @State private var networks: [ImprovWire.Network] = []
    @State private var chosenSSID = ""
    @State private var typedSSID = ""
    @State private var password = ""
    @State private var showPassword = false
    @State private var remember = true
    @State private var remembered: HouseholdWiFi? = HouseholdWiFiStore.load()
    @State private var verdict: ImprovClient.Outcome?
    @State private var paired = false
    /// Joined but not paired, and how to pair it later — shown in the done
    /// section in place of the "Paired" line (MagicPairPlan's words).
    @State private var pairNote: String?

    enum Stage: Equatable {
        case connecting
        case scanning
        case choose
        case sending
        case joining
        /// Joined; spending a WAP's claim over the home Wi-Fi for its key.
        case claiming
        case done
        case failed(String)
    }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    HStack(spacing: Theme.m) {
                        DeviceFigureIcon(canary.family?.deviceType ?? .unknown, size: 44)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(canary.title).font(.headline)
                            Text(canary.displayName).font(.caption.monospaced()).foregroundStyle(.secondary)
                        }
                        Spacer()
                        if let client, client.canIdentify, stage == .choose || stage == .scanning {
                            Button("Blink it") { client.identify() }
                                .buttonStyle(.bordered)
                        }
                    }
                } footer: {
                    Text("Two on the table? Blink it to be sure which one this is. A Sense or Vision's name ends in the same four characters as its setup network; a WAP's in the last four of its key fingerprint.")
                }

                switch stage {
                case .connecting:
                    Section {
                        HStack { ProgressView(); Text("Connecting over Bluetooth…") }
                    }
                case .scanning:
                    Section {
                        HStack { ProgressView(); Text("Asking the Canary which networks it can see…") }
                    }
                case .choose:
                    chooseSection
                case .sending:
                    Section {
                        HStack { ProgressView(); Text("Handing over your Wi-Fi — tap Pair if iOS asks…") }
                    }
                case .joining:
                    Section {
                        HStack { ProgressView(); Text("The Canary is joining \(effectiveSSID)…") }
                    }
                case .claiming:
                    Section {
                        HStack { ProgressView(); Text("Collecting its pairing key over your Wi-Fi…") }
                    }
                case .done:
                    doneSection
                case .failed(let why):
                    Section {
                        Label(why, systemImage: "exclamationmark.triangle")
                            .font(.footnote).foregroundStyle(Theme.color(.warn))
                            .fixedSize(horizontal: false, vertical: true)
                        Button("Try again") { restart() }
                    } footer: {
                        Text("Its setup network is the same door with a key — the Set up walkthrough has that path too.")
                    }
                }
            }
            .navigationTitle("Hand it your Wi-Fi")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } }
            }
            .task { await begin() }
            .onDisappear { store.ble.endSetup() }
        }
    }

    // MARK: - stages

    private var effectiveSSID: String {
        let picked = chosenSSID.isEmpty ? typedSSID : chosenSSID
        return picked.trimmingCharacters(in: .whitespaces)
    }

    @ViewBuilder private var chooseSection: some View {
        Section {
            if networks.isEmpty {
                TextField("Your Wi-Fi name", text: $typedSSID)
                    .textInputAutocapitalization(.never).autocorrectionDisabled()
            } else {
                Picker("Network", selection: $chosenSSID) {
                    ForEach(networks, id: \.ssid) { n in
                        Text(n.isOpen ? "\(n.ssid) (open)" : n.ssid).tag(n.ssid)
                    }
                    Text("Another name…").tag("")
                }
                if chosenSSID.isEmpty {
                    TextField("Your Wi-Fi name", text: $typedSSID)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                }
            }
            if !(networks.first(where: { $0.ssid == effectiveSSID })?.isOpen ?? false) {
                HStack {
                    if showPassword {
                        TextField("Wi-Fi password", text: $password)
                            .textInputAutocapitalization(.never).autocorrectionDisabled()
                    } else {
                        SecureField("Wi-Fi password", text: $password)
                    }
                    Button { showPassword.toggle() } label: {
                        Image(systemName: showPassword ? "eye.slash" : "eye").foregroundStyle(.secondary)
                    }
                    .buttonStyle(.plain)
                }
            }
            Toggle("Remember for the next Canary", isOn: $remember)
            Button {
                Task { await send() }
            } label: {
                Label("Join \(effectiveSSID.isEmpty ? "my Wi-Fi" : effectiveSSID)", systemImage: "wifi")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(effectiveSSID.isEmpty)
        } header: {
            Text(networks.isEmpty ? "Your Wi-Fi" : "The networks it can see")
        } footer: {
            Text(remembered.map { "Prefilled from the last Canary (\($0.ssid)), kept in this phone's Keychain only." }
                 ?? "The password goes only to the Canary, over the encrypted Bluetooth link; with the toggle on, this phone keeps it in its Keychain for the next Canary — never in iCloud.")
        }
    }

    private var seenOnNetwork: [DiscoveredCanary] {
        guard let family = canary.family else { return [] }
        return store.discovery.found.filter { $0.deviceType == family.deviceType }
    }

    @ViewBuilder private var doneSection: some View {
        Section {
            Label("It's on \(effectiveSSID) — the Canary said so itself.", systemImage: "checkmark.circle")
                .foregroundStyle(Theme.color(.calm))
            if paired {
                Label("Paired with this phone — its key came over your Wi-Fi, never over Bluetooth.", systemImage: "key.horizontal")
                    .foregroundStyle(Theme.color(.calm))
            } else if let pairNote {
                Label(pairNote, systemImage: "exclamationmark.triangle")
                    .font(.footnote).foregroundStyle(Theme.color(.warn))
                    .fixedSize(horizontal: false, vertical: true)
            }
            if store.discoveryConsent != true {
                Button("Enable discovery to watch it appear") { store.setDiscoveryConsent(true) }
            } else if seenOnNetwork.isEmpty {
                HStack(spacing: Theme.s) {
                    ProgressView()
                    Text("Watching this network for it — it announces itself within a minute.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            } else {
                ForEach(seenOnNetwork) { d in
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
            Button("Done") { dismiss() }
        } footer: {
            if let login = HubSecretStore.brokerLogin() {
                Text("Its broker login, if it asks for one: \(login.username) — the Canary walkthrough shows the password.")
            }
        }
    }

    // MARK: - the ceremony

    private func begin() async {
        stage = .connecting
        guard let c = store.ble.beginSetup(canary.peripheralID) else {
            stage = .failed("That Canary isn't in range any more — power it on near this phone and try again.")
            return
        }
        client = c
        guard await c.waitUntilReady() else {
            if case .failed(let why) = c.phase { stage = .failed(why) }
            else { stage = .failed("The Canary didn't answer over Bluetooth.") }
            return
        }
        if c.canScanWiFi {
            stage = .scanning
            networks = await c.scanNetworks()
        }
        if let remembered, networks.isEmpty || networks.contains(where: { $0.ssid == remembered.ssid }) {
            chosenSSID = networks.isEmpty ? "" : remembered.ssid
            typedSSID = networks.isEmpty ? remembered.ssid : ""
            password = remembered.password
        } else if let first = networks.first {
            chosenSSID = first.ssid
        }
        stage = .choose
    }

    private func send() async {
        guard let client else { return }
        let ssid = effectiveSSID
        stage = .sending
        let outcome = await client.provision(ssid: ssid, password: password)
        verdict = outcome
        switch outcome {
        case .joined:
            if HouseholdWiFiStore.shouldRemember(toggleOn: remember, joined: true, ssid: ssid) {
                try? HouseholdWiFiStore.save(HouseholdWiFi(ssid: ssid, password: password, savedAt: Date()))
            }
            // A WAP hands the link that provisioned a one-time CLAIM — never
            // its bearer token — and the phone spends it over the home Wi-Fi
            // for the receipt. Read it now, while the link lingers (the
            // Canary drops it ~20 s after the join), then let the link go:
            // everything after this is Wi-Fi.
            let hasClaimService = client.offersClaim
            var claim: ImprovWire.Claim?
            if hasClaimService, let data = await client.readClaim() {
                claim = ImprovWire.parseClaim(data)
            }
            store.ble.endSetup()
            await pair(hasClaimService: hasClaimService, claim: claim)
            stage = .done
            Task { await store.refreshOnce() }
        case .failed(let why):
            stage = .failed(why)
        }
    }

    /// Follow the plan: spend a WAP's claim over the home Wi-Fi, at most
    /// twice (its `.local` name, then the address it reported), and end
    /// PAIRED — token in the Keychain, from the one tap — or with an honest
    /// note on how to pair it later. A Sense or Vision offers no claim and
    /// keeps the watch-the-fleet ending.
    private func pair(hasClaimService: Bool, claim: ImprovWire.Claim?) async {
        var step = MagicPairPlan.afterJoin(hasClaimService: hasClaimService, claim: claim,
                                           isPrivateHost: DeviceAPI.isPrivate)
        var receipt: ProvisioningReceipt?
        var triedIP = false
        dial: while true {
            let url: URL
            let pin: String?
            switch step {
            case .spendClaim(let u, let p):
                url = u
                pin = p
            case .retryViaIP(let u, let p):
                url = u
                pin = p
                triedIP = true
            case .watchFleet, .paired, .notPairedTapBoot, .refused:
                break dial
            }
            guard let claim else { break }
            stage = .claiming
            let outcome: MagicPairPlan.FetchOutcome
            do {
                receipt = try await DeviceAPI.fetchReceipt(claimURL: url, tlsFingerprint: pin)
                outcome = .receipt
            } catch {
                outcome = Self.fetchOutcome(for: error)
            }
            step = MagicPairPlan.afterFetch(outcome, claim: claim, triedIP: triedIP,
                                            isPrivateHost: DeviceAPI.isPrivate)
        }
        if case .paired = step, let receipt {
            // The same two gates PairView applies to a pasted receipt.
            step = MagicPairPlan.afterReceipt(baseURL: receipt.baseURL,
                                              tlsCertFingerprint: receipt.tlsCertFingerprint,
                                              isPrivateHost: DeviceAPI.isPrivate)
        }
        switch step {
        case .paired:
            guard let receipt else { return }
            let ref = PairedDeviceRef(id: receipt.deviceID, name: canary.title, deviceType: .wap,
                                      baseURL: receipt.baseURL, pairedAt: Date(),
                                      tlsCertFingerprint: receipt.tlsCertFingerprint)
            store.devices.add(ref, token: receipt.token)
            paired = true
        case .notPairedTapBoot(let note), .refused(let note):
            pairNote = note
        case .watchFleet, .spendClaim, .retryViaIP:
            break
        }
    }

    /// How one dial ended, in the plan's terms: a 403 is the Canary's
    /// refusal (final); DNS, connection and timeout failures never reached
    /// it (the address fallback may); anything else is named and final.
    private static func fetchOutcome(for error: Error) -> MagicPairPlan.FetchOutcome {
        if let refusal = error as? DeviceError, case .claimRefused = refusal { return .refused }
        if let urlError = error as? URLError {
            switch urlError.code {
            case .cannotFindHost, .cannotConnectToHost, .dnsLookupFailed, .timedOut,
                 .notConnectedToInternet, .networkConnectionLost:
                return .unreachable
            default:
                break
            }
        }
        return .other(error.localizedDescription)
    }

    private func restart() {
        store.ble.endSetup()
        client = nil
        networks = []
        paired = false
        pairNote = nil
        Task { await begin() }
    }
}

#if DEBUG
#Preview("Nearby card") {
    NearbyCanaryCard().environmentObject(DemoFleet.previewStore())
}
#endif
