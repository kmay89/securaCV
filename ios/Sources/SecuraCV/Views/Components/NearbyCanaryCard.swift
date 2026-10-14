// NearbyCanaryCard.swift
//
// "A new Canary is nearby." Three views over one policy (Shared/
// NearbyCanary.swift decides which sightings qualify):
//
//   * NearbyOfferOverlay — the card that comes to you. When exactly one new
//     Canary is close (NearbyCanaries.autoOffer), it slides up from the
//     bottom of whatever screen is showing, AirPods-style: the device's
//     figure, its name, Continue, Not now. Mounted once at the root; never
//     offers the same Canary twice in a session.
//   * NearbyCanaryCard — the same Canaries as an inline card (Today, Fleet,
//     the walkthrough), for when two are on the table and the person picks.
//   * NearbySetupSheet — the ceremony one tap opens: the phone connects,
//     iOS asks to pair, the Canary lists the networks it can see, the
//     person picks theirs (prefilled when this phone remembered it from the
//     last Canary) and taps Join, and the Canary answers with its own
//     verdict. A WAP then hands this phone a one-time claim over the link,
//     and the phone spends it over the home Wi-Fi for the pairing receipt —
//     the key never rides Bluetooth (Shared/MagicPairPlan.swift decides
//     each step). It ends on a card that names THIS Canary, and watches the
//     network for that unit — never "any Canary of the same kind".
//
// Honesty rules: every line of status is the Canary's word or the phone's
// own action — "joining" when the Canary said provisioning, "on your
// Wi-Fi" when it said provisioned — and no spinner outlives its reason: a
// radio that cannot hear says why, and a watch that found nothing says so.

import SwiftUI

/// Signal bands, strongest first — shared by every card that names one.
enum NearbySignal {
    static func word(_ rssi: Int) -> String {
        // (A `-55...` pattern parses as `-(55...)`, which the compiler
        // rejects; the guards say the same thing plainly.)
        switch rssi {
        case let r where r >= -55: return "right here"
        case let r where r >= NearbyCanaries.closeEnoughDBM: return "close by"
        case let r where r >= -85: return "in the room"
        default: return "faint"
        }
    }
}

// MARK: - the card that comes to you

/// Mounted once, over every section (RootView). Shows the AirPods-style
/// offer when the policy names one Canary; the setup sheet it opens is
/// attached to this always-present container, so the sheet survives the
/// offer itself going away (it does, the moment it is answered).
struct NearbyOfferOverlay: View {
    @EnvironmentObject var store: FleetStore
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var target: NearbyCanary?
    /// The Canary the card is showing. Latched when the card first appears —
    /// and counted as offered right then, not when it is answered — so a
    /// card that is ignored, or whose signal wobbles across "close enough",
    /// neither flickers nor slides up a second time this session. It stays
    /// until it is answered or the Canary drops out of earshot entirely.
    @State private var shown: NearbyCanary?

    var body: some View {
        ZStack(alignment: .bottom) {
            if let c = shown {
                // The live sighting, so "right here / close by" keeps up.
                let live = store.nearbyCanaries.first { $0.suffix == c.suffix } ?? c
                NearbyOfferCard(canary: live,
                                onContinue: {
                                    shown = nil
                                    target = live
                                },
                                onNotNow: { shown = nil })
                    .padding(.horizontal, Theme.s)
                    .padding(.bottom, Theme.s)
                    .frame(maxWidth: 520)
                    .transition(reduceMotion ? .opacity : .move(edge: .bottom).combined(with: .opacity))
            }
        }
        .animation(reduceMotion ? nil : Animation.spring(response: 0.45, dampingFraction: 0.85), value: shown?.id)
        .onChange(of: store.nearbyOffer?.id, initial: true) { _, _ in latchOffer() }
        .onChange(of: target?.id) { _, _ in latchOffer() }   // a sheet closed: the next one may show
        .onChange(of: store.nearbyCanaries.map(\.suffix)) { _, heard in
            if let c = shown, !heard.contains(c.suffix) { shown = nil }
        }
        .sheet(item: $target) { c in
            NearbySetupSheet(canary: c)
        }
    }

    private func latchOffer() {
        guard shown == nil, target == nil, let c = store.nearbyOffer else { return }
        store.noteOffered(c.suffix)
        shown = c
    }
}

/// The offer itself: what it is, how near, one button.
struct NearbyOfferCard: View {
    let canary: NearbyCanary
    var onContinue: () -> Void
    var onNotNow: () -> Void

    var body: some View {
        VStack(spacing: Theme.m) {
            HStack {
                Spacer()
                Button(action: onNotNow) {
                    Image(systemName: "xmark.circle.fill")
                        .font(.title2)
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Not now")
            }
            DeviceFigureIcon(canary.family?.deviceType ?? .unknown, size: 84)
                .accessibilityHidden(true)
            VStack(spacing: Theme.xs) {
                Text(canary.title)
                    .font(.title3.bold())
                    .multilineTextAlignment(.center)
                Text("New Canary \(NearbySignal.word(canary.rssiDBM)) — ready for your Wi-Fi")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            Button(action: onContinue) {
                Text("Continue").frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.large)
        }
        .padding(Theme.l)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 28, style: .continuous))
        .shadow(color: .black.opacity(0.18), radius: 18, y: 6)
        .accessibilityElement(children: .contain)
    }
}

// MARK: - the inline card

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
                    Text("It's asking for your Wi-Fi. One tap hands it over, encrypted.")
                        .font(.footnote).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    ForEach(nearby) { c in
                        HStack(spacing: Theme.m) {
                            DeviceFigureIcon(c.family?.deviceType ?? .unknown, size: 32)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(c.title).font(.body)
                                Text("\(c.displayName) · \(NearbySignal.word(c.rssiDBM))")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button("Set up") {
                                store.noteOffered(c.suffix)   // the pop-up card need not ask again
                                target = c
                            }
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
}

// MARK: - the ceremony

/// One Canary at a time.
struct NearbySetupSheet: View {
    let canary: NearbyCanary
    @EnvironmentObject var store: FleetStore
    @ObservedObject private var vantage = NetworkVantage.shared
    @Environment(\.dismiss) private var dismiss

    @State private var client: ImprovClient?
    @State private var stage: Stage = .connecting
    @State private var networks: [ImprovWire.Network] = []
    @State private var chosenSSID = ""
    @State private var typedSSID = ""
    @State private var password = ""
    @State private var showPassword = false
    @State private var remember = true
    /// The household Wi-Fi this phone kept from the last Canary. Read once,
    /// in begin() — a @State initializer runs on every rebuild of this view
    /// (each Bluetooth sighting rebuilds it), and a Keychain read there is
    /// a main-thread stall per advert.
    @State private var remembered: HouseholdWiFi?
    @State private var verdict: ImprovClient.Outcome?
    @State private var paired = false
    /// Joined but not paired, and how to pair it later — shown in the done
    /// section in place of the "Paired" line (MagicPairPlan's words).
    @State private var pairNote: String?
    /// The WAP's claim, kept so a dial that reached nobody can be tried
    /// again while the claim lives (MagicPairPlan.mayRetry).
    @State private var claim: ImprovWire.Claim?
    @State private var claimReadAt: Date?
    @State private var hasClaimService = false
    @State private var lastFetch: MagicPairPlan.FetchOutcome?
    /// The device id the receipt named, once paired — how the done card
    /// recognizes THIS WAP on the network.
    @State private var pairedDeviceID: String?
    /// The watch for it on the network gave up waiting (it never spins on).
    @State private var watchTimedOut = false

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
                    if stage == .choose || stage == .scanning {
                        Text("Two on the table? Blink it to be sure which one this is.")
                    }
                }

                switch stage {
                case .connecting:
                    if let advice = store.bluetoothAdvice {
                        Section { RadioAdviceRow(advice: advice) }
                    } else {
                        Section {
                            HStack { ProgressView(); Text("Connecting over Bluetooth…") }
                        }
                    }
                case .scanning:
                    Section {
                        HStack {
                            ProgressView()
                            Text("Tap Pair when iOS asks — then the Canary lists the networks it can see…")
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                case .choose:
                    chooseSection
                case .sending:
                    Section {
                        HStack { ProgressView(); Text("Handing over your Wi-Fi — tap Pair if iOS asks…") }
                    }
                case .joining:
                    Section {
                        HStack { ProgressView(); Text("\(canary.title) is joining \(effectiveSSID)…") }
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
                        Text("Still stuck? Its setup network is the same door with a key — the Set up walkthrough has that path too.")
                    }
                }
            }
            .navigationTitle(stage == .done ? "All set" : "Hand it your Wi-Fi")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(stage == .done ? "Done" : "Close") { dismiss() }
                }
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

    private var chosenIsOpen: Bool {
        networks.first(where: { $0.ssid == effectiveSSID })?.isOpen ?? false
    }

    /// Before anything is sent: the cause most likely to fail the join,
    /// named while it can still be fixed (Model/SetupPortal.swift).
    private var preflightHints: [String] {
        var out: [String] = []
        if client?.pairingDeclined == true, networks.isEmpty {
            out.append("Pairing was declined, so it couldn't list its networks. Type your Wi-Fi name — and tap Pair when iOS asks again.")
        }
        let listed = networks.map(\.ssid)
        if let hint = SetupPortal.notListedHint(ssid: effectiveSSID, listed: listed) {
            out.append(hint)
        } else if let remembered, remembered.ssid != effectiveSSID,
                  let hint = SetupPortal.notListedHint(ssid: remembered.ssid, listed: listed) {
            out.append(hint)
        }
        if let hint = SetupPortal.passwordHint(password, networkIsOpen: chosenIsOpen) {
            out.append(hint)
        }
        if canary.family == .wap, !vantage.onWiFi {
            out.append("This phone is off Wi-Fi. Join your Wi-Fi first — the WAP's pairing key is collected over it, never over Bluetooth.")
        }
        return out
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
            if !chosenIsOpen {
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
            ForEach(preflightHints, id: \.self) { hint in
                Label(hint, systemImage: "info.circle")
                    .font(.footnote).foregroundStyle(Theme.color(.warn))
                    .fixedSize(horizontal: false, vertical: true)
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
                 ?? "The password goes only to the Canary, over the encrypted Bluetooth link. With the toggle on, this phone keeps it in its Keychain for the next Canary — never in iCloud.")
        }
    }

    /// The new Canary on the network — THIS unit, never any Canary of the
    /// same kind (a Vision you already own is not the one just set up). A
    /// Sense or Vision is recognized by the pseudonym its advert and its
    /// mDNS host share (NearbyCanaries.isSameDevice); a WAP by the device
    /// id its receipt named, once paired.
    private var seenOnNetwork: [DiscoveredCanary] {
        guard let family = canary.family else { return [] }
        return store.discovery.found.filter { d in
            guard d.deviceType == family.deviceType else { return false }
            if let id = pairedDeviceID { return d.deviceID == id }
            return family != .wap && NearbyCanaries.isSameDevice(suffix: canary.suffix, host: d.host)
        }
    }

    @ViewBuilder private var doneSection: some View {
        Section {
            HStack(spacing: Theme.m) {
                DeviceFigureIcon(canary.family?.deviceType ?? .unknown, size: 56)
                VStack(alignment: .leading, spacing: Theme.xxs) {
                    Text("\(canary.title) is on \(effectiveSSID)")
                        .font(.headline)
                    Text("The Canary said so itself.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            if paired {
                Label("Paired with this phone — its key came over your Wi-Fi, never over Bluetooth.", systemImage: "key.horizontal")
                    .foregroundStyle(Theme.color(.calm))
            } else if let pairNote {
                Label(pairNote, systemImage: "exclamationmark.triangle")
                    .font(.footnote).foregroundStyle(Theme.color(.warn))
                    .fixedSize(horizontal: false, vertical: true)
                if mayRetryClaim {
                    Button("Try again") { Task { await retryClaim() } }
                        .buttonStyle(.borderedProminent)
                }
            }
            appearRow
            Button("Done") { dismiss() }
        } footer: {
            if let login = HubSecretStore.brokerLogin(), canary.family == .vision || canary.family == .sense {
                Text("Its hub login, if it asks for one: \(login.username). The Set up screen copies the password for you.")
            }
        }
    }

    /// Watching the network for this unit — with the reason when the phone
    /// cannot see the network, and an end when it has waited long enough.
    @ViewBuilder private var appearRow: some View {
        if store.discoveryConsent != true {
            Button("Enable discovery to watch it appear") { store.setDiscoveryConsent(true) }
        } else if !seenOnNetwork.isEmpty {
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
        } else if let advice = store.localNetworkAdvice {
            RadioAdviceRow(advice: advice)
        } else if watchTimedOut {
            Text("Not seen on this network yet. Check this phone is on the same Wi-Fi — it can take a couple of minutes to announce itself, and it shows on the Fleet tab when it does.")
                .font(.footnote).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        } else {
            ListeningRow(text: "Watching this network for it — it announces itself within a minute.", advice: nil)
        }
    }

    // MARK: - the ceremony

    private func begin() async {
        stage = .connecting
        remembered = HouseholdWiFiStore.load()
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
            hasClaimService = client.offersClaim
            if hasClaimService, let data = await client.readClaim() {
                claim = ImprovWire.parseClaim(data)
                claimReadAt = Date()
            }
            store.ble.endSetup()
            await pair()
            stage = .done
            startWatchClock()
            Task { await store.refreshOnce() }
        case .failed(let why):
            stage = .failed(why)
        }
    }

    /// Follow the plan: spend a WAP's claim over the home Wi-Fi, at most
    /// twice (its `.local` name, then the address it reported), and end
    /// PAIRED — token in the Keychain, from the one tap — or with an honest
    /// note on how to pair it later. A Sense or Vision offers no claim and
    /// keeps the watch-the-network ending.
    private func pair() async {
        var step = MagicPairPlan.afterJoin(hasClaimService: hasClaimService, claim: claim,
                                           isPrivateHost: DeviceAPI.isPrivate)
        var receipt: ProvisioningReceipt?
        var triedIP = false
        lastFetch = nil
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
            case .watchFleet, .paired, .notPaired, .refused:
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
            lastFetch = outcome
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
            pairNote = nil
            pairedDeviceID = receipt.deviceID
        case .notPaired(let note):
            // "Nothing answered" names its likeliest cause — this phone
            // being off Wi-Fi — and stays retryable while the claim lives.
            pairNote = lastFetch == .unreachable
                ? MagicPairPlan.unreachableNote(phoneOnWiFi: vantage.onWiFi) : note
        case .refused(let note):
            pairNote = note
        case .watchFleet, .spendClaim, .retryViaIP:
            break
        }
    }

    private var mayRetryClaim: Bool {
        guard !paired, let claim, let claimReadAt else { return false }
        return MagicPairPlan.mayRetry(after: lastFetch, claim: claim, readAt: claimReadAt, now: Date())
    }

    /// Dial the same claim again — only ever after a dial that reached
    /// nobody (the claim is unspent), and only while it lives.
    private func retryClaim() async {
        guard let claim, let claimReadAt else { return }
        guard MagicPairPlan.mayRetry(after: lastFetch, claim: claim, readAt: claimReadAt, now: Date()) else {
            pairNote = MagicPairPlan.Copy.claimExpired
            lastFetch = nil
            return
        }
        await pair()
        stage = .done
        Task { await store.refreshOnce() }
    }

    /// The network watch ends in words, not an endless spinner.
    private func startWatchClock() {
        watchTimedOut = false
        Task {
            try? await Task.sleep(for: .seconds(90))
            watchTimedOut = true
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
        claim = nil
        claimReadAt = nil
        lastFetch = nil
        pairedDeviceID = nil
        Task { await begin() }
    }
}

#if DEBUG
#Preview("Nearby card") {
    NearbyCanaryCard().environmentObject(DemoFleet.previewStore())
}

#Preview("The card that comes to you") {
    NearbyOfferCard(canary: NearbyCanary(peripheralID: UUID(), family: .sense, suffix: "K7MZ",
                                         displayName: "Sense-K7MZ", rssiDBM: -52, lastHeard: Date(),
                                         improvState: .authorized, canIdentify: true, canScanWiFi: true),
                    onContinue: {}, onNotNow: {})
        .padding()
}
#endif
