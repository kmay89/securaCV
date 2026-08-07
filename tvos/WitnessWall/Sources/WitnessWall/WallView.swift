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

    // The room this TV serves, and how it dresses. Persisted like the hub
    // address: a power cut must not reset a bar's board to the home wall.
    @AppStorage("SecuraCVWallProfile") private var profileRaw = WallProfile.home.rawValue
    @AppStorage("SecuraCVWallSkin") private var skinRaw = WallSkin.midnight.rawValue

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
            Text("The Wall is checking your network — a hub or Canary at canary.local answers by itself. Nothing leaves this room.")
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
                    DeviceCard(device: device, skin: skin, hero: profile == .apartment && WallProfile.isDoorish(device.name))
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
                    title: "Showing the last verified record from \(asOf.formatted(date: .omitted, time: .shortened))",
                    detail: stale
                )
            } else if fleet.hasChainTrouble {
                StatusBanner(
                    tone: .alarm,
                    title: "A Canary's record didn't verify",
                    detail: "One or more devices report a chain that isn't ok. Open the Canary's page on your hub to see why."
                )
            } else if let verifiedThrough = fleet.verifiedThrough {
                StatusBanner(
                    tone: .calm,
                    title: "Verified through \(verifiedThrough)",
                    detail: nil
                )
            }
        }
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

    /// Profile + skin chips, tucked at the header's edge. Focusable with the
    /// remote; a click cycles. Two controls, zero settings screens.
    private var stylePicker: some View {
        HStack(spacing: 14) {
            Button(profile.label) {
                profileRaw = next(after: profile, in: WallProfile.allCases).rawValue
            }
            Button(skin.label) {
                skinRaw = next(after: skin, in: WallSkin.allCases).rawValue
            }
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
                Text(device.name)
                    .font(hero ? .title.weight(.bold) : .title2.weight(.semibold))
                    .foregroundStyle(skin.ink)
                    .lineLimit(1)
            }
            Text(statusLine)
                .font(.callout)
                .foregroundStyle(.secondary)
            if let product = device.product {
                Text(product)
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

    private var statusLine: String {
        if device.chainIsTroubled { return "Record didn't verify" }
        return device.online ? "Online · record ok" : "Offline"
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
