//  WallView.swift — the screen itself.
//
//  A calm, shared, always-on view of the fleet's verified record — NOT a video
//  wall (docs/tvos/README.md). Everything here is derived from `WallModel.state`
//  so there is no way to draw "verified" while the model knows better.
//
//  Three profiles (WallStyle.swift) dress the SAME data for the room the TV is
//  in — home wall, business board, apartment peephole — and four skins dress
//  the light. Setup is: turn it on (the model searches the LAN by itself).

import SwiftUI
import UIKit

struct WallView: View {
    @State private var model = WallModel()
    @State private var typedAddress = ""
    @State private var showSettings = false
    @State private var selectedDevice: FleetSnapshot.Device?

    // The room this TV serves, and how it dresses. Persisted like the hub
    // address: a power cut must not reset a bar's board to the home wall.
    @AppStorage("SecuraCVWallProfile") private var profileRaw = WallProfile.home.rawValue
    @AppStorage("SecuraCVWallSkin") private var skinRaw = WallSkin.midnight.rawValue
    /// The one banner that is a choice (WallSettingsView). Chain trouble is not.
    @AppStorage("SecuraCVWallOfflineBanner") private var offlineBanner = false

    private var profile: WallProfile { WallProfile(rawValue: profileRaw) ?? .home }
    private var skin: WallSkin { WallSkin(rawValue: skinRaw) ?? .midnight }

    var body: some View {
        ZStack {
            // Deep, dim background by default: this is furniture in a room,
            // on for hours. Skins retint it; the structure never changes.
            LinearGradient(
                colors: [skin.backgroundTop, skin.backgroundBottom],
                startPoint: .top,
                endPoint: .bottom
            )
            .ignoresSafeArea()

            switch model.state {
            case .searching:
                searchingCard
            case .needsHub:
                hubPrompt
            case .connecting(let address):
                connecting(to: address)
            case .live(let fleet, let asOf):
                fleetWall(fleet, asOf: asOf, stale: nil)
            case .stale(let fleet, let since, let reason):
                fleetWall(fleet, asOf: since, stale: reason)
            case .unreachable(let reason):
                doctorCard(reason: reason)
            }
        }
        .preferredColorScheme(skin.isLight ? .light : .dark)
        .fullScreenCover(isPresented: $showSettings) {
            WallSettingsView(model: model)
        }
        .fullScreenCover(item: $selectedDevice) { device in
            // The cover gets the device's IDENTITY; the view reads the live
            // row out of the model each render, so a detail screen left open
            // across a poll can never keep an out-of-date sentence up.
            DeviceDetailView(model: model, deviceID: device.id, skin: skin)
        }
        .onAppear {
            // Furniture must not doze: the whole point of a wall is that it
            // is still there when you look up.
            UIApplication.shared.isIdleTimerDisabled = true
            model.start()
        }
        .onDisappear {
            UIApplication.shared.isIdleTimerDisabled = false
            model.stop()
        }
    }

    // MARK: - States

    /// The zero-typing path: the Wall is looking for the fleet by itself.
    private var searchingCard: some View {
        VStack(spacing: 24) {
            ProgressView()
            Text("Looking for your Canaries…")
                .font(.system(size: 56, weight: .semibold))
            Text("The Wall is checking your network — a hub at canary.local answers by itself, and so does every Canary that publishes a fleet report, hub or not. Nothing leaves this room.")
                .font(.title3)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 1100)
        }
        .padding(80)
    }

    private var hubPrompt: some View {
        VStack(spacing: 28) {
            Text("Connect your fleet")
                .font(.system(size: 64, weight: .semibold))
            Text("Nothing answered at canary.local yet — the Wall keeps looking. If your hub lives somewhere else, enter its address:")
                .font(.title3)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 1100)
            TextField("http://canary.local:8099", text: $typedAddress)
                .textContentType(.URL)
                .frame(maxWidth: 900)
            Button("Connect") { model.connect(to: typedAddress) }
                .disabled(typedAddress.trimmingCharacters(in: .whitespaces).isEmpty)
        }
        .padding(80)
    }

    private func connecting(to address: String) -> some View {
        VStack(spacing: 20) {
            ProgressView()
            Text("Looking for \(address)…")
                .font(.title2)
                .foregroundStyle(.secondary)
        }
    }

    private func fleetWall(_ fleet: FleetSnapshot, asOf: Date, stale: String?) -> some View {
        VStack(alignment: .leading, spacing: profile == .business ? 24 : 36) {
            header(fleet, asOf: asOf, stale: stale)

            // Devices, largest-first in importance: anything troubled leads.
            // The profile sets density and ordering; the data is the data.
            LazyVGrid(columns: [GridItem(.adaptive(minimum: profile.tileMinimum), spacing: 28)], spacing: 28) {
                ForEach(profile.sorted(fleet.devices)) { device in
                    // A card is a button: click it and the device opens up
                    // close, with the same turntable the phone shows. The
                    // .card style is what makes it feel like tvOS — focus
                    // lift, motion, the remote's native vocabulary.
                    Button {
                        selectedDevice = device
                    } label: {
                        DeviceCard(device: device, skin: skin, hero: profile == .apartment && WallProfile.isDoorish(device.name))
                    }
                    .buttonStyle(.card)
                }
            }

            Spacer()
            residentRow
            footer
        }
        .padding(72)
    }

    private func header(_ fleet: FleetSnapshot, asOf: Date, stale: String?) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                Text(fleet.kernel ?? "Your fleet")
                    .font(.system(size: profile == .business ? 56 : 72, weight: .bold))
                    .foregroundStyle(skin.ink)
                Spacer()
                stylePicker
            }
            Text(headline(fleet))
                .font(.title2)
                .foregroundStyle(.secondary)

            if let stale {
                // The whole point of the stale state: say so, in the loudest
                // element on screen, rather than letting old data read as now.
                StatusBanner(
                    tone: .warning,
                    title: "Showing the last report received at \(receipt(asOf))",
                    detail: stale
                )
            } else if let report = model.report, !report.ok {
                // This TV walked the chain itself and it did not verify —
                // the one verdict that outranks every self-report below.
                StatusBanner(
                    tone: .alarm,
                    title: "The record did not verify",
                    detail: report.message
                )
            } else if fleet.hasChainTrouble {
                StatusBanner(
                    tone: .alarm,
                    title: "A Canary reports its record didn't verify",
                    detail: "One or more devices report a chain that isn't ok. Open the Canary's page on your hub to see why."
                )
            } else if offlineBanner, !offlineNames(fleet).isEmpty {
                // The one banner that is a CHOICE (settings → Attention): a
                // bar wants to hear about a dark Canary, a bedroom doesn't.
                // It ranks below chain trouble and never above it — a device
                // that is merely asleep must not outshout one that is lying.
                StatusBanner(
                    tone: .warning,
                    title: offlineTitle(fleet),
                    detail: nil
                )
            } else if let verifiedThrough = fleet.verifiedThrough {
                // Two sentences for two claims — and two clocks. The time
                // after "through" is THIS TV's: when it received (and, given
                // a sealed log, checked) the report, the only time this
                // screen measured. The fleet's own `verified_through` is a
                // wire string the firmware fills with the literal word "now";
                // it is shown, but labeled as the device's report, and never
                // stitched to this screen's verdict or clock. "Verified"
                // itself belongs to this TV's own verdict (the Rust core
                // walking a served sealed log) and to nothing on the wire.
                if let report = model.report, report.ok {
                    // "Verified" is reserved (AGENTS.md rule 4) for a signature
                    // checked against a PINNED key. The Wall walks the chain
                    // against the key the sealed-log document itself supplies —
                    // it proves the log is internally consistent and signed by
                    // one key, not that the key is the kernel's. Until the Wall
                    // pins that key at first contact, the banner says exactly
                    // that, and keeps the fleet's own timestamp labeled as the
                    // fleet's report rather than this screen's verdict.
                    StatusBanner(
                        tone: .calm,
                        title: "Chain intact through \(receipt(asOf)) · \(report.verified) sealed \(report.verified == 1 ? "entry" : "entries")",
                        detail: "Signatures checked on this Apple TV against the key the log supplied (not yet pinned). Device reports “\(verifiedThrough)”."
                    )
                } else {
                    StatusBanner(
                        tone: .calm,
                        title: "Your fleet reported in through \(receipt(asOf))",
                        detail: "Device reports “\(verifiedThrough)” — its own word, not a check this Apple TV performed."
                    )
                }
            }
        }
    }

    /// This TV's own clock, as the banners print it: when THIS screen received
    /// the report it is describing. Never the fleet's self-stamped time — that
    /// one is shown only as "Device reports …".
    private func receipt(_ asOf: Date) -> String {
        asOf.formatted(date: .omitted, time: .shortened)
    }

    /// The line under the fleet name, phrased for the room. Counts come from
    /// the snapshot — a profile may choose WHICH honest sentence to lead
    /// with, never invent one.
    private func headline(_ fleet: FleetSnapshot) -> String {
        let attention = fleet.devices.filter { $0.chainIsTroubled || !$0.online }.count
        switch profile {
        case .business:
            return attention == 0
                ? "\(fleet.summary) · nothing needs attention"
                : "\(fleet.summary) · \(attention) need\(attention == 1 ? "s" : "") attention"
        case .apartment, .home:
            return fleet.summary
        }
    }

    /// The devices that are dark right now, for the opt-in banner. Chain
    /// trouble is excluded — those devices already own the louder banner.
    private func offlineNames(_ fleet: FleetSnapshot) -> [String] {
        fleet.devices.filter { !$0.online && !$0.chainIsTroubled }.map(\.name)
    }

    private func offlineTitle(_ fleet: FleetSnapshot) -> String {
        let names = offlineNames(fleet)
        switch names.count {
        case 1: return "\(names[0]) is offline"
        case 2: return "\(names[0]) and \(names[1]) are offline"
        default: return "\(names.count) Canaries are offline"
        }
    }

    /// Profile + skin chips, tucked at the header's edge — a click cycles,
    /// no screen opened. The gear is the third chip: the full panel, for when
    /// you want to see the choices instead of cycling through them.
    private var stylePicker: some View {
        HStack(spacing: 14) {
            Button(profile.label) {
                profileRaw = next(after: profile, in: WallProfile.allCases).rawValue
            }
            Button(skin.label) {
                skinRaw = next(after: skin, in: WallSkin.allCases).rawValue
            }
            Button {
                showSettings = true
            } label: {
                Image(systemName: "gearshape")
            }
            .accessibilityLabel("Settings")
        }
        .font(.caption)
        .buttonStyle(.bordered)
    }

    private func next<T: Equatable>(after current: T, in all: [T]) -> T {
        guard let i = all.firstIndex(of: current) else { return all[0] }
        return all[(i + 1) % all.count]
    }

    private func doctorCard(reason: String) -> some View {
        VStack(spacing: 24) {
            Text("The Wall can't see your fleet")
                .font(.system(size: 56, weight: .semibold))
            Text(reason)
                .font(.title3)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 1100)
            // One problem, one action — the Doctor-card posture.
            Button("Change hub address") { model.connect(to: typedAddress) }
            TextField("http://canary.local:8099", text: $typedAddress)
                .frame(maxWidth: 900)
        }
        .padding(80)
    }

    /// The resident's own row: one switch, one honest sentence. It lives on
    /// the Wall rather than in a settings screen because the promise it makes
    /// is about THIS Apple TV in THIS room, and the limit it carries — tvOS
    /// suspends an app that is not on screen — is only true while you are
    /// looking at it.
    private var residentRow: some View {
        VStack(alignment: .leading, spacing: 12) {
            Button(model.resident.isEnabled ? "Stop standing watch" : "Stand watch for the household") {
                model.resident.setEnabled(!model.resident.isEnabled)
            }
            Text(model.resident.standing.line)
                .font(.callout)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 1100, alignment: .leading)
            if model.resident.isEnabled {
                Text("While this Apple TV shows the Wall, it watches for you. Switch to another app and the watch pauses — that is Apple's rule for tvOS, not a setting here.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: 1100, alignment: .leading)
            }
        }
    }

    private var footer: some View {
        HStack(spacing: 24) {
            Text("SecuraCV Witness Wall")
            Text("core \(model.coreVersion)")
            if !model.hubAddress.isEmpty {
                Text(model.hubAddress)   // where "live" comes from — never a mystery
            }
            if let report = model.report {
                Text(report.ok ? "chain ok · \(report.verified) entries" : report.message)
                    .foregroundStyle(report.ok ? Color.secondary : Color.orange)
            }
            Spacer()
            Text("Witnessing without watching — no video on this screen.")
        }
        .font(.caption)
        .foregroundStyle(.tertiary)
    }
}

// MARK: - Pieces

/// The two sentences the Wall says about a device, defined once so the card
/// on the grid and the detail view can never phrase the same state two ways.
extension FleetSnapshot.Device {
    var wallStatusLine: String {
        if chainIsTroubled { return "Reports its record didn't verify" }
        // "record ok" is a claim about a chain, so it may only be said by a
        // device that HAS one. A display holds none — it renders other
        // devices' — and answers "unknown"; saying "record ok" there would
        // invent a verification, exactly as calling it trouble invented a
        // failure. And in either direction the sentence says "reports":
        // this field is the device's own word over an unauthenticated wire,
        // not a verification the TV performed — that claim belongs solely to
        // the header's verify verdict (WallModel.report).
        guard chain == "ok" else { return online ? "Online" : "Offline" }
        return online ? "Online · reports record ok" : "Offline"
    }

    var wallHubLine: String {
        switch hubState {
        case .absent: return "No hub yet — it works on its own"
        case .down:   return "Can't reach its hub"
        case .ok, .unknown: return ""
        }
    }
}

struct DeviceCard: View {
    let device: FleetSnapshot.Device
    let skin: WallSkin
    /// The apartment's door Canary draws larger type — the room's one question
    /// ("who's at the door?") should be answerable from the couch.
    let hero: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 12) {
                Circle()
                    .fill(dotColor)
                    .frame(width: 18, height: 18)
                // The device itself, drawn from the SAME resolver the phone
                // uses (board, then published type, then the coarse family) —
                // so a Canary that has a picture has it on every screen, and
                // one that doesn't falls back to the same honest marker rather
                // than to a different one. This card used to carry no figure
                // at all: a dot stood in for the hardware.
                DeviceFigureIcon(device.deviceType,
                                 published: device.product,
                                 hardware: device.hw,
                                 size: hero ? 46 : 34)
                Text(device.name)
                    .font(hero ? .title.weight(.bold) : .title2.weight(.semibold))
                    .foregroundStyle(skin.ink)
                    .lineLimit(1)
            }
            Text(device.wallStatusLine)
                .font(.callout)
                .foregroundStyle(.secondary)
            // The product NAME, never the wire string. A television showing
            // "canary-nightstand7" reads as a bug from across the room; the
            // shared resolver says "Canary Nightstand 7", and says nothing at
            // all rather than guessing for a type this build doesn't know.
            if let product = device.productName {
                Text(product)
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
            // A hub the owner hasn't set up is worth one quiet line on the
            // wall — never an alarm, and nothing at all when the device is
            // connected or didn't say (HubState.unknown).
            if device.hubState.needsAttention {
                Text(device.wallHubLine)
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
        }
        .padding(24)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(skin.tile, in: RoundedRectangle(cornerRadius: 20))
    }

    private var dotColor: Color {
        if device.chainIsTroubled { return .orange }
        return device.online ? skin.ok : Color(white: 0.4)
    }
}

struct StatusBanner: View {
    enum Tone { case calm, warning, alarm }

    let tone: Tone
    let title: String
    let detail: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.title3.weight(.semibold))
            if let detail {
                Text(detail).font(.callout).foregroundStyle(.secondary)
            }
        }
        .padding(20)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(background, in: RoundedRectangle(cornerRadius: 16))
    }

    private var background: Color {
        switch tone {
        case .calm: return Color.green.opacity(0.14)
        case .warning: return Color.yellow.opacity(0.16)
        case .alarm: return Color.orange.opacity(0.20)
        }
    }
}
