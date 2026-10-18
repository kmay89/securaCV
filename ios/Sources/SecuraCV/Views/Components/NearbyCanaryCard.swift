// NearbyCanaryCard.swift
//
// "A new Canary is nearby." Three views over one policy (Shared/
// NearbyCanary.swift decides which sightings qualify):
//
//   * NearbyOfferOverlay — the card that comes to you. When exactly one new
//     Canary is close (NearbyCanaries.autoOffer), it slides up from the
//     bottom of whatever screen is showing, AirPods-style: the device's
//     figure, its name, Continue, Not now. Mounted once at the root; never
//     offers the same Canary twice in a session, and stands down while the
//     Fleet tab's "+" sheet, which lists the same Canaries, is up
//     (NearbyOfferLatch).
//   * NearbyCanaryCard — the same Canaries as an inline card (Today, Fleet,
//     the walkthrough), for when two are on the table and the person picks.
//     It stays mounted, naming the one being set up, for as long as that
//     setup sheet is open — a connected Canary stops advertising, so it
//     leaves the nearby list fifteen seconds in, and a card that went with
//     it would take the sheet down mid-ceremony.
//   * NearbySetupSheet — the ceremony one tap opens: the phone connects,
//     iOS asks to pair, the Canary lists the networks it can hear, the
//     person picks theirs (or, for the second Canary, the one this phone
//     remembered goes by itself after a moment's countdown) and taps Join,
//     and the Canary answers with its own verdict. A wrong password is
//     corrected on the same link; a quiet link is kept alive while the
//     password is found. A Sense or Vision then takes the hub this phone
//     knows over the same link, and says what it did with it. A WAP hands
//     this phone a one-time claim over the link instead, and the phone
//     spends it over the home Wi-Fi for the pairing receipt — the key never
//     rides Bluetooth (Shared/MagicPairPlan.swift decides each step). It
//     ends on a card that names THIS Canary, and watches the network for
//     that unit — never "any Canary of the same kind".
//
// Honesty rules: every line of status is the Canary's word or the phone's
// own action — "joining" when the Canary said provisioning, "on your
// Wi-Fi" when it said provisioned, "reports to your hub" when it answered
// stored — and no spinner outlives its reason: every wait says how long is
// normal, a radio that cannot hear says why, and a watch that found nothing
// says so. The pure decisions are in NearbySetupLogic at the bottom, so
// NearbyCanaryCardLogicTests can hold each one.

import SwiftUI
import AVFoundation

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
    /// The Canary the card holds (Shared/NearbyCanary.swift,
    /// NearbyOfferLatch). Latched when the card first appears — and counted
    /// as offered right then, not when it is answered — so a card that is
    /// ignored, or whose signal wobbles across "close enough", neither
    /// flickers nor slides up a second time this session. It stays until it
    /// is answered or the Canary drops out of earshot entirely, and it stands
    /// down while the Fleet tab's "+" sheet is up (store.nearbyOfferStandsDown):
    /// that sheet lists the same Canaries inline, and its .medium detent
    /// would otherwise leave this card showing dimmed behind it.
    @State private var offer = NearbyOfferLatch()

    var body: some View {
        ZStack(alignment: .bottom) {
            if let c = offer.visible(standingDown: store.nearbyOfferStandsDown) {
                // The live sighting, so "right here / close by" keeps up.
                let live = store.nearbyCanaries.first { $0.suffix == c.suffix } ?? c
                NearbyOfferCard(canary: live,
                                onContinue: {
                                    offer.answer()
                                    target = live
                                },
                                onNotNow: { offer.answer() })
                    .padding(.horizontal, Theme.s)
                    .padding(.bottom, Theme.s)
                    .frame(maxWidth: 520)
                    .transition(reduceMotion ? .opacity : .move(edge: .bottom).combined(with: .opacity))
            }
        }
        .animation(reduceMotion ? nil : Animation.spring(response: 0.45, dampingFraction: 0.85),
                   value: offer.visible(standingDown: store.nearbyOfferStandsDown)?.id)
        .onChange(of: store.nearbyOffer?.id, initial: true) { _, _ in latchOffer() }
        .onChange(of: target?.id) { _, _ in latchOffer() }   // a sheet closed: the next one may show
        .onChange(of: store.nearbyOfferStandsDown) { _, _ in latchOffer() }   // the "+" sheet closed
        .onChange(of: store.nearbyCanaries.map(\.suffix)) { _, heard in offer.heard(heard) }
        .sheet(item: $target) { c in
            NearbySetupSheet(canary: c)
        }
    }

    private func latchOffer() {
        if let suffix = offer.latch(store.nearbyOffer, setupOpen: target != nil,
                                    standingDown: store.nearbyOfferStandsDown) {
            store.noteOffered(suffix)
        }
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
    /// The Canary whose setup sheet is open. The sheet hangs off the card,
    /// so the card must outlive the sighting: a Canary being set up stops
    /// advertising the moment the phone connects and leaves
    /// `nearbyCanaries` fifteen seconds in (NearbyCanaries.freshFor), and a
    /// card that unmounted then would tear the sheet down mid-ceremony.
    /// NearbySetupLogic.cardRows keeps this one on the card until the sheet
    /// closes — the same latch NearbyOfferOverlay keeps for its offer. (An
    /// always-present empty container would do it too, but inside Today's
    /// and Fleet's stacks an empty view still spends a spacing slot.)
    @State private var target: NearbyCanary?

    var body: some View {
        let rows = NearbySetupLogic.cardRows(nearby: store.nearbyCanaries, settingUp: target)
        if !rows.isEmpty {
            Card {
                VStack(alignment: .leading, spacing: Theme.s) {
                    HStack(spacing: Theme.s) {
                        Image(systemName: "dot.radiowaves.left.and.right")
                            .foregroundStyle(Theme.color(.info))
                        Text(rows.count == 1 ? "A new Canary is nearby" : "\(rows.count) new Canaries are nearby")
                            .font(.headline)
                    }
                    Text("It's asking for your Wi-Fi. One tap hands it over, encrypted.")
                        .font(.footnote).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    ForEach(rows) { c in
                        let settingUp = target?.id == c.id
                        HStack(spacing: Theme.m) {
                            DeviceFigureIcon(c.family?.deviceType ?? .unknown, size: 32)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(c.title).font(.body)
                                Text("\(c.displayName) · \(settingUp ? "setting up" : NearbySignal.word(c.rssiDBM))")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button("Set up") {
                                store.noteOffered(c.suffix)   // the pop-up card need not ask again
                                target = c
                            }
                            .buttonStyle(.borderedProminent)
                            .disabled(settingUp)
                            Button {
                                store.dismissNearby(c.suffix)
                            } label: {
                                Image(systemName: "xmark.circle").foregroundStyle(.secondary)
                            }
                            .buttonStyle(.plain)
                            .disabled(settingUp)
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
    /// The Canary's own verdict on the last join, shown above the password
    /// while the person corrects it — the link stayed up for the retry.
    @State private var joinVerdict: String?
    @FocusState private var passwordFocused: Bool
    /// The join goes (went) by itself, on the Wi-Fi this phone remembered.
    @State private var autoJoined = false
    /// The countdown before that automatic join — Change cancels it.
    @State private var autoJoinTask: Task<Void, Never>?
    /// The first encrypted write has gone out, so iOS's Pair sheet has had
    /// its moment; no later row asks for it.
    @State private var pairAsked = false
    @State private var showingScanner = false
    @State private var cameraDenied = false
    @State private var scanNote: String?
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
    /// How the hub hand-off ended (a Sense or Vision), for the done card.
    @State private var hubHandoff: NearbySetupLogic.HubHandoff?
    @State private var hubHost: String?

    enum Stage: Equatable {
        case connecting
        case scanning
        case choose
        /// The remembered Wi-Fi is about to be sent by itself; Change stops it.
        case aboutToJoin
        case sending
        case joining
        /// Joined; spending a WAP's claim over the home Wi-Fi for its key.
        case claiming
        /// Joined; handing a Sense or Vision the hub this phone knows.
        case handingOverHub
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
                            HStack { ProgressView(); Text("Connecting over Bluetooth — a few seconds.") }
                        }
                    }
                case .scanning:
                    Section {
                        HStack {
                            ProgressView()
                            Text("Tap Pair when iOS asks — the Canary then lists the networks it can hear, usually under 10 seconds.")
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                case .choose:
                    chooseSection
                case .aboutToJoin:
                    Section {
                        HStack {
                            ProgressView()
                            Text("Joining \(effectiveSSID) with the Wi-Fi this phone remembered — in a moment.")
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        Button("Change") { stopAutoJoin() }
                    } footer: {
                        Text("Change keeps the fields as they are; nothing has been sent yet.")
                    }
                case .sending:
                    Section {
                        HStack {
                            ProgressView()
                            Text(sendingLine).fixedSize(horizontal: false, vertical: true)
                        }
                    }
                case .joining:
                    Section {
                        HStack {
                            ProgressView()
                            Text(joiningLine).fixedSize(horizontal: false, vertical: true)
                        }
                    }
                case .claiming:
                    Section {
                        HStack { ProgressView(); Text("Collecting its pairing key over your Wi-Fi — a few seconds.") }
                    }
                case .handingOverHub:
                    Section {
                        HStack { ProgressView(); Text("Handing it your hub (\(hubHost ?? "")) — a few seconds.") }
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
                        Text("Still stuck? Unplug it and plug it back in — the door reopens for 30 minutes (a Sense or Vision also opens for a minute after one tap on its BOOT button). Its setup network is the same door with a key — the Set up walkthrough has that path.")
                    }
                }
            }
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(stage == .done ? "Done" : "Close") { dismiss() }
                }
            }
            .task { await begin() }
            // While the person finds the password, the link is kept alive
            // (and quietly re-made if it drops); the task ends with the stage.
            .task(id: stage == .choose) {
                guard stage == .choose else { return }
                await keepLinkAlive()
            }
            .onDisappear {
                autoJoinTask?.cancel()
                store.ble.endSetup()
            }
            .sheet(isPresented: $showingScanner) {
                NavigationStack {
                    SetupQRScannerSheet { code in take(qr: code) }
                }
            }
        }
    }

    // MARK: - the words for each moment

    /// The moment, not an instruction: the first thing read on the sheet.
    private var title: String {
        switch stage {
        case .connecting: return "Finding it"
        case .scanning: return "Pairing"
        case .choose, .aboutToJoin: return "Your Wi-Fi"
        case .sending, .joining: return "Joining"
        case .claiming: return "Its pairing key"
        case .handingOverHub: return "Your hub"
        case .done: return "All set"
        case .failed: return "Not yet"
        }
    }

    /// "Tap Pair" only when this write is the first encrypted one (a Canary
    /// that could not list its networks): iOS asks now, and not before.
    private var sendingLine: String {
        let what = autoJoined
            ? "Handing over \(effectiveSSID), the Wi-Fi this phone remembered"
            : "Handing over your Wi-Fi"
        return pairAsked ? what + "…" : what + " — tap Pair when iOS asks."
    }

    private var joiningLine: String {
        autoJoined
            ? "\(canary.title) is joining \(effectiveSSID) with the Wi-Fi this phone remembered — up to a minute."
            : "\(canary.title) is joining \(effectiveSSID) — up to a minute."
    }

    // MARK: - stages

    private var effectiveSSID: String {
        let picked = chosenSSID.isEmpty ? typedSSID : chosenSSID
        return picked.trimmingCharacters(in: .whitespaces)
    }

    private var chosenIsOpen: Bool {
        networks.first(where: { $0.ssid == effectiveSSID })?.isOpen ?? false
    }

    /// The remembered network is on the Canary's list — the one case the
    /// picker opens with a selection.
    private var rememberedListed: Bool {
        guard let remembered else { return false }
        return networks.contains { $0.ssid == remembered.ssid }
    }

    /// Before anything is sent: the cause most likely to fail the join,
    /// named while it can still be fixed (Model/SetupPortal.swift).
    private var preflightHints: [String] {
        var out: [String] = []
        if networks.isEmpty,
           let hint = NearbySetupLogic.emptyListHint(pairingDeclined: client?.pairingDeclined == true,
                                                      canScan: client?.canScanWiFi == true) {
            out.append(hint)
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
                if client?.canScanWiFi == true {
                    Button {
                        Task { await rescan() }
                    } label: {
                        Label("List its networks again", systemImage: "arrow.clockwise")
                    }
                }
            } else {
                Picker("Network", selection: $chosenSSID) {
                    ForEach(networks, id: \.ssid) { n in
                        Text(n.isOpen ? "\(n.ssid) (open)" : n.ssid).tag(n.ssid)
                    }
                    Text("Another name…").tag("")
                }
                Text(SetupPortal.pickYoursNote)
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if chosenSSID.isEmpty {
                    TextField("Your Wi-Fi name", text: $typedSSID)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                }
            }
            if SetupQRScannerSheet.deviceCanScan {
                // A convenience, never a gate: the router's sticker QR says
                // the same name and password the fields take.
                Button {
                    Task { await scanTapped() }
                } label: {
                    Label("Scan your router's Wi-Fi QR", systemImage: "qrcode.viewfinder")
                }
                if cameraDenied {
                    RadioAdviceRow(advice: RadioAdvice(text: NearbySetupLogic.cameraDeniedNote, opensSettings: true))
                }
                if let scanNote {
                    Text(scanNote).font(.caption).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            if let joinVerdict {
                Label(joinVerdict, systemImage: "exclamationmark.triangle")
                    .font(.footnote).foregroundStyle(Theme.color(.warn))
                    .fixedSize(horizontal: false, vertical: true)
            }
            if chosenIsOpen {
                Text(SetupPortal.openNetworkNote)
                    .font(.footnote).foregroundStyle(.secondary)
            } else {
                HStack {
                    // An EXISTING network's password: `.password`, never
                    // `.newPassword`, so iOS fills a saved one and offers to
                    // invent nothing (LESSONS_LEARNED "iOS offers to invent a
                    // password").
                    if showPassword {
                        TextField(SetupPortal.passwordFieldLabel, text: $password)
                            .textContentType(.password)
                            .textInputAutocapitalization(.never).autocorrectionDisabled()
                            .focused($passwordFocused)
                    } else {
                        SecureField(SetupPortal.passwordFieldLabel, text: $password)
                            .textContentType(.password)
                            .focused($passwordFocused)
                    }
                    Button { showPassword.toggle() } label: {
                        Image(systemName: showPassword ? "eye.slash" : "eye").foregroundStyle(.secondary)
                    }
                    .buttonStyle(.plain)
                }
                Text(SetupPortal.passwordFieldHint)
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
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
            if networks.isEmpty {
                Text("Your Wi-Fi")
            } else {
                Text(rememberedListed ? "The networks it can see" : SetupPortal.pickHeader)
            }
        } footer: {
            VStack(alignment: .leading, spacing: Theme.xs) {
                if !networks.isEmpty {
                    Text(SetupPortal.notSeenNote)
                }
                if canary.family == .wap {
                    Text(SetupPortal.wapSameNetworkNote)
                }
                if let remembered, effectiveSSID == remembered.ssid {
                    Text("Prefilled from the last Canary (\(remembered.ssid)), kept in this phone's Keychain only.")
                } else {
                    Text("The password goes only to the Canary, over the encrypted Bluetooth link. With the toggle on, this phone keeps it in its Keychain for the next Canary — never in iCloud.")
                }
            }
        }
    }

    /// The new Canary on the network — THIS unit, never any Canary of the
    /// same kind (a Vision you already own is not the one just set up). A
    /// Sense or Vision is recognized by the pseudonym its advert and its
    /// mDNS host share (NearbyCanaries.isSameDevice); a WAP by the device
    /// id its receipt named once paired, or the one its claim named when
    /// it joined but did not pair (NearbySetupLogic.isThisUnit).
    private var seenOnNetwork: [DiscoveredCanary] {
        guard let family = canary.family else { return [] }
        let known = pairedDeviceID ?? claim?.deviceID
        return store.discovery.found.filter { d in
            NearbySetupLogic.isThisUnit(family: family, suffix: canary.suffix, knownDeviceID: known,
                                        seenType: d.deviceType, seenDeviceID: d.deviceID, seenHost: d.host)
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
                if mayRetryClaim || pairNote == MagicPairPlan.Copy.localNetworkOff {
                    HStack(spacing: Theme.s) {
                        if mayRetryClaim {
                            Button("Try again") { Task { await retryClaim() } }
                                .buttonStyle(.borderedProminent)
                        }
                        if pairNote == MagicPairPlan.Copy.localNetworkOff {
                            // The fix lives in Settings; the button goes there.
                            OpenSettingsButton()
                        }
                    }
                }
            }
            if let hubHandoff {
                Label(NearbySetupLogic.hubEnding(hubHandoff),
                      systemImage: hubHandoff.settled ? "externaldrive.connected.to.line.below" : "exclamationmark.triangle")
                    .font(.footnote)
                    .foregroundStyle(hubHandoff.settled ? Theme.color(.calm) : Theme.color(.warn))
                    .fixedSize(horizontal: false, vertical: true)
            }
            appearRow
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
            if pairNote == MagicPairPlan.Copy.localNetworkOff {
                // The pairing note above already names the switch and
                // carries the Settings button: one cause, said once.
                Text("Once Local Network is on, this phone watches for it here too.")
                    .font(.footnote).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                RadioAdviceRow(advice: advice)
            }
        } else if watchTimedOut {
            // With a pairing note above, the cause is already on the card;
            // the watch only says what it saw.
            Text(pairNote == nil
                 ? "Not seen on this network yet. Check this phone is on the same Wi-Fi — it can take a couple of minutes to announce itself, and it shows on the Fleet tab when it does."
                 : "Not seen on this network yet — it can take a couple of minutes to announce itself, and it shows on the Fleet tab when it does.")
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
            // The scan is the first encrypted write: iOS asks to pair now,
            // and the scanning row says so.
            pairAsked = true
            stage = .scanning
            networks = await c.scanNetworks()
        }
        let opening = NearbySetupLogic.opening(networks: networks, remembered: remembered, canScan: c.canScanWiFi)
        chosenSSID = opening.chosenSSID
        typedSSID = opening.typedSSID
        password = opening.password
        if opening.autoJoin {
            armAutoJoin()
        } else {
            stage = .choose
        }
    }

    /// The second Canary is two taps: the Wi-Fi this phone remembered goes
    /// by itself, after a countdown long enough to tap Change. A write in
    /// flight cannot be taken back, so Change is offered only before it —
    /// and the fields stay as they were when it is tapped.
    private func armAutoJoin() {
        autoJoined = true
        stage = .aboutToJoin
        autoJoinTask?.cancel()
        autoJoinTask = Task {
            try? await Task.sleep(for: .seconds(NearbySetupLogic.autoJoinCountdown))
            guard !Task.isCancelled, stage == .aboutToJoin else { return }
            await send()
        }
    }

    private func stopAutoJoin() {
        autoJoinTask?.cancel()
        autoJoinTask = nil
        autoJoined = false
        stage = .choose
    }

    private func send() async {
        guard let client else { return }
        let ssid = effectiveSSID
        joinVerdict = nil
        stage = .sending
        let outcome = await client.provision(ssid: ssid, password: password)
        pairAsked = true   // whatever happened, the first encrypted write is behind us
        verdict = outcome
        switch outcome {
        case .joined:
            if HouseholdWiFiStore.shouldRemember(toggleOn: remember, joined: true, ssid: ssid) {
                try? HouseholdWiFiStore.save(HouseholdWiFi(ssid: ssid, password: password, savedAt: Date()))
            }
            // A WAP hands the link that provisioned a one-time CLAIM — never
            // its bearer token — and the phone spends it over the home Wi-Fi
            // for the receipt. Read it now, while the link lingers (the
            // Canary drops it ~20 s after the join).
            hasClaimService = client.offersClaim
            if hasClaimService, let data = await client.readClaim() {
                claim = ImprovWire.parseClaim(data)
                claimReadAt = Date()
            }
            // A Sense or Vision takes the hub this phone knows over the same
            // lingering link. Then let the link go: everything after this
            // is Wi-Fi.
            await handOverHub(client)
            store.ble.endSetup()
            await pair()
            stage = .done
            startWatchClock()
            Task { await store.refreshOnce() }
        case .failed(let why):
            if client.recoverAfterFailedJoin() {
                // The Canary refused the join but kept its door open on this
                // link: the fields stay, the verdict sits over the password.
                joinVerdict = NearbySetupLogic.retryVerdict(ssid: ssid)
                autoJoined = false
                stage = .choose
                passwordFocused = true
            } else {
                stage = .failed(why)
            }
        }
    }

    /// Hand a Sense or Vision the hub this phone knows — host from the hub
    /// record, login from the Keychain — and keep its answer for the done
    /// card. Nothing is written when the phone knows no hub, when the door
    /// has no HUB characteristic, or when the frame cannot be spelled.
    private func handOverHub(_ client: ImprovClient) async {
        guard canary.family == .sense || canary.family == .vision else { return }
        guard client.offersHubHandoff else {
            hubHandoff = .notOffered
            return
        }
        guard let host = HubStore.load()?.baseURL.host, !host.isEmpty else {
            hubHandoff = .noHub
            return
        }
        let login = HubSecretStore.brokerLogin()
        guard let frame = ImprovWire.hubHandoff(host: host, port: ImprovWire.hubDefaultPort,
                                                user: login?.username, pass: login?.password,
                                                tls: false) else {
            hubHandoff = .unsendable(host: host)
            return
        }
        hubHost = host
        stage = .handingOverHub
        let status = await client.handOverHub(frame)
        hubHandoff = .answered(status, host: host)
    }

    /// Ask the Canary for its list once more — after a declined pairing,
    /// or a scan that ran out of time.
    private func rescan() async {
        guard let client else { return }
        pairAsked = true
        stage = .scanning
        networks = await client.scanNetworks()
        // A name typed while there was no list, now on it: picked.
        let pick = NearbySetupLogic.pick(ssid: typedSSID.trimmingCharacters(in: .whitespaces),
                                         listed: networks.map(\.ssid))
        if !pick.chosen.isEmpty {
            chosenSSID = pick.chosen
            typedSSID = ""
        }
        stage = .choose
    }

    /// While the person finds the password: a harmless command each minute
    /// keeps the Canary from dropping a silent link (three minutes is its
    /// bound), and a link that drops anyway is quietly re-made — the typed
    /// fields stay, and the list is asked for again only when there was
    /// none. Ends when the stage does (.task(id:)).
    private func keepLinkAlive() async {
        var quietSeconds = 0
        while !Task.isCancelled {
            try? await Task.sleep(for: .seconds(1))
            guard !Task.isCancelled, stage == .choose, let client else { return }
            switch client.phase {
            case .ready:
                quietSeconds += 1
                if quietSeconds >= NearbySetupLogic.keepAliveSeconds {
                    quietSeconds = 0
                    client.ping()
                }
            case .disconnected, .failed:
                quietSeconds = 0
                let reconnected = await reconnectWhileChoosing()
                if !reconnected {
                    // Out of range for now: look again shortly, not every second.
                    try? await Task.sleep(for: .seconds(10))
                }
            default:
                break
            }
        }
    }

    /// Re-make the link without a word on screen. False when the Canary is
    /// not around to connect to (the Join button then says so when tapped).
    private func reconnectWhileChoosing() async -> Bool {
        store.ble.endSetup()
        guard let c = store.ble.beginSetup(canary.peripheralID) else { return false }
        client = c
        guard await c.waitUntilReady(), stage == .choose else { return false }
        if networks.isEmpty, c.canScanWiFi {
            networks = await c.scanNetworks()
        }
        return true
    }

    // MARK: - the router's QR

    /// The camera is asked for at the tap that needs it — never earlier,
    /// and a "no" leaves the fields and a way to Settings (CameraGate).
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

    private func take(qr code: String) {
        showingScanner = false
        guard let qr = SetupPortal.parseWiFiQR(code) else {
            scanNote = "That code isn't a Wi-Fi QR — a router's sticker QR starts with WIFI:."
            return
        }
        let pick = NearbySetupLogic.pick(ssid: qr.ssid, listed: networks.map(\.ssid))
        chosenSSID = pick.chosen
        typedSSID = pick.typed
        password = qr.password
        joinVerdict = nil
        scanNote = "Read \(qr.ssid) and its password off the QR."
    }

    // MARK: - the WAP's claim

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
            // "Nothing answered" names its likeliest cause — Local Network
            // off for this app, or this phone off Wi-Fi — and stays
            // retryable while the claim lives.
            pairNote = lastFetch == .unreachable
                ? MagicPairPlan.unreachableNote(phoneOnWiFi: vantage.onWiFi,
                                                localNetworkBlocked: store.discovery.localNetworkBlocked)
                : note
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

    /// A dropped link, or a failure the same link cannot take back from:
    /// the whole ceremony again, from the connect.
    private func restart() {
        autoJoinTask?.cancel()
        autoJoinTask = nil
        store.ble.endSetup()
        client = nil
        networks = []
        joinVerdict = nil
        autoJoined = false
        pairAsked = false
        scanNote = nil
        paired = false
        pairNote = nil
        claim = nil
        claimReadAt = nil
        lastFetch = nil
        pairedDeviceID = nil
        hubHandoff = nil
        hubHost = nil
        Task { await begin() }
    }
}

// MARK: - the pure decisions

/// What the card and the sheet decide, out of the views so each rule is a
/// line in NearbyCanaryCardLogicTests: which rows the inline card shows,
/// what the picker opens with and whether the join goes by itself, the
/// hint for an empty list, which network sighting is THIS Canary, and the
/// done card's word on the hub.
enum NearbySetupLogic {
    /// Seconds between keep-alive pings while the person finds the
    /// password — well inside the firmware's three-minute idle bound.
    static let keepAliveSeconds = 60
    /// Seconds the automatic join waits for a Change tap before it sends.
    static let autoJoinCountdown = 3

    /// The inline card's rows: the live sightings, plus the Canary being
    /// set up once it has gone quiet (a connected Canary does not
    /// advertise), so the card — and the sheet hanging off it — outlive
    /// the sighting.
    static func cardRows(nearby: [NearbyCanary], settingUp: NearbyCanary?) -> [NearbyCanary] {
        guard let settingUp, !nearby.contains(where: { $0.id == settingUp.id }) else { return nearby }
        return nearby + [settingUp]
    }

    /// What the picker opens with.
    struct Opening: Equatable, Sendable {
        /// The list entry picked ("" when none, or when the name is typed).
        var chosenSSID: String
        /// The typed name (only when there is no list to pick from).
        var typedSSID: String
        var password: String
        /// Send without stopping at the picker: this phone remembered a
        /// network the Canary can see (or the Canary cannot list networks
        /// at all, and the remembered name is typed in for it).
        var autoJoin: Bool
    }

    /// The remembered household Wi-Fi is preselected — and the join goes by
    /// itself — only when the Canary can see it. Nothing else is
    /// preselected: the strongest network it heard is as often a neighbor's
    /// as yours, so the picker opens unselected and asks. With no list
    /// (the Canary cannot scan, declined pairing, or ran out of time) the
    /// remembered name is typed in; it is sent by itself only when the
    /// Canary cannot list networks at all — an empty list it could have
    /// filled is not a reason to guess.
    static func opening(networks: [ImprovWire.Network], remembered: HouseholdWiFi?, canScan: Bool) -> Opening {
        guard let remembered else {
            return Opening(chosenSSID: "", typedSSID: "", password: "", autoJoin: false)
        }
        if networks.contains(where: { $0.ssid == remembered.ssid }) {
            return Opening(chosenSSID: remembered.ssid, typedSSID: "", password: remembered.password, autoJoin: true)
        }
        if networks.isEmpty {
            return Opening(chosenSSID: "", typedSSID: remembered.ssid, password: remembered.password,
                           autoJoin: !canScan)
        }
        return Opening(chosenSSID: "", typedSSID: "", password: "", autoJoin: false)
    }

    /// A name against the Canary's list: the list entry when it is on it,
    /// else typed (what a router's QR or a rescan hands the fields).
    static func pick(ssid: String, listed: [String]) -> (chosen: String, typed: String) {
        guard !ssid.isEmpty else { return ("", "") }
        return listed.contains(ssid) ? (ssid, "") : ("", ssid)
    }

    /// Why the list is empty, and the way back — for a Canary that can
    /// list networks (one that cannot has nothing to retry, and no hint).
    static func emptyListHint(pairingDeclined: Bool, canScan: Bool) -> String? {
        guard canScan else { return nil }
        return pairingDeclined
            ? "Pairing was declined, so it couldn't list its networks. Type your Wi-Fi name, or tap List its networks again — and tap Pair when iOS asks."
            : "Its network list didn't arrive in time. Type your Wi-Fi name, or tap List its networks again."
    }

    /// The Canary's own verdict on a join it refused, worded for the retry
    /// on the same link: Improv has one error for a wrong password and a
    /// network the board cannot see, so both are named.
    static func retryVerdict(ssid: String) -> String {
        "The Canary couldn't join \(ssid). Check the password (and that it's your network), then tap Join again."
    }

    /// When the camera is off for the app: the QR is a convenience, so the
    /// way around it is named with the way to Settings.
    static let cameraDeniedNote =
        "Camera access is off for SecuraCV, so it can't scan the QR. Turn it on in Settings, or pick the network above."

    /// Is a device seen on the network THIS Canary? Never by kind alone. A
    /// WAP is known by device id — the receipt's once paired, else the
    /// claim's when it joined but did not pair (the claim names the unit
    /// whether or not it was spent); a Sense or Vision by the pseudonym its
    /// advert and its mDNS host share (NearbyCanaries.isSameDevice).
    static func isThisUnit(family: CanaryFamily, suffix: String, knownDeviceID: String?,
                           seenType: DeviceType, seenDeviceID: String, seenHost: String?) -> Bool {
        guard seenType == family.deviceType else { return false }
        if let knownDeviceID { return seenDeviceID == knownDeviceID }
        return family != .wap && NearbyCanaries.isSameDevice(suffix: suffix, host: seenHost)
    }

    /// How the hub hand-off ended, for a Sense or Vision.
    enum HubHandoff: Equatable, Sendable {
        /// The door has no HUB characteristic (an older firmware).
        case notOffered
        /// This phone knows no hub to hand over.
        case noHub
        /// The hub's address or login would not fit the frame, or the host
        /// is not a name the Canary can dial.
        case unsendable(host: String)
        /// Written; this is the Canary's answer (`.unknown` for none in time).
        case answered(ImprovWire.HubHandoffStatus, host: String)

        /// The one ending that needs nothing more from the person.
        var settled: Bool {
            if case .answered(.stored, _) = self { return true }
            return false
        }
    }

    /// The done card's sentence on the hub — the Canary's word where it
    /// gave one, and the real next step where it did not.
    static func hubEnding(_ outcome: HubHandoff) -> String {
        switch outcome {
        case .notOffered:
            return "Its hub link could not be handed over (an older firmware) — flash it once with the Flasher's MQTT fields."
        case .noHub:
            return "No hub on this phone yet. Set one up from Set up; then tap this Canary's BOOT button once and it takes the hub from this phone."
        case .unsendable(let host):
            return "Its hub link could not be handed over — the address of your hub (\(host)) or its login is longer than the Canary's door takes. Flash it once with the Flasher's MQTT fields."
        case .answered(let status, let host):
            switch status {
            case .stored:
                return "It reports to your hub at \(host)."
            case .loginRefused:
                return "Your hub refused its login — open Set up → Your hub to re-mint the Canary login."
            case .rejected:
                return "The Canary didn't take the hub this phone handed it (\(host)) — flash it once with the Flasher's MQTT fields."
            case .none:
                return "The Canary says it kept no hub — tap its BOOT button once and this phone hands it over again."
            case .unknown:
                return "This phone handed it your hub (\(host)), but the Canary gave no answer in time — if it doesn't report there, tap its BOOT button once and this phone hands it over again."
            }
        }
    }

    /// Every sentence this file can show that a test cannot reach through
    /// a view, for the voice sweeps.
    static var allCopy: [String] {
        [emptyListHint(pairingDeclined: true, canScan: true)!,
         emptyListHint(pairingDeclined: false, canScan: true)!,
         retryVerdict(ssid: "Home"),
         cameraDeniedNote,
         hubEnding(.notOffered), hubEnding(.noHub), hubEnding(.unsendable(host: "h")),
         hubEnding(.answered(.stored, host: "h")), hubEnding(.answered(.loginRefused, host: "h")),
         hubEnding(.answered(.rejected, host: "h")), hubEnding(.answered(.none, host: "h")),
         hubEnding(.answered(.unknown, host: "h"))]
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
