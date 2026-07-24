//  WallView.swift — the screen itself.
//
//  A calm, shared, always-on view of the fleet's verified record — NOT a video
//  wall (docs/tvos/README.md). Everything here is derived from `WallModel.state`
//  so there is no way to draw "verified" while the model knows better.

import SwiftUI

struct WallView: View {
    @State private var model = WallModel()
    @State private var typedAddress = ""

    var body: some View {
        ZStack {
            // Deep, dim background: this is furniture in a room, on for hours.
            LinearGradient(
                colors: [Color(white: 0.04), Color(white: 0.10)],
                startPoint: .top,
                endPoint: .bottom
            )
            .ignoresSafeArea()

            switch model.state {
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
        .onAppear { model.start() }
        .onDisappear { model.stop() }
    }

    // MARK: - States

    private var hubPrompt: some View {
        VStack(spacing: 28) {
            Text("Connect your fleet")
                .font(.system(size: 64, weight: .semibold))
            Text("Enter your hub's address — something like http://canary.local:8099")
                .font(.title3)
                .foregroundStyle(.secondary)
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
        VStack(alignment: .leading, spacing: 36) {
            header(fleet, asOf: asOf, stale: stale)

            // Devices, largest-first in importance: anything troubled leads.
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 380), spacing: 28)], spacing: 28) {
                ForEach(sorted(fleet.devices)) { device in
                    DeviceCard(device: device)
                }
            }

            Spacer()
            footer
        }
        .padding(72)
    }

    private func header(_ fleet: FleetSnapshot, asOf: Date, stale: String?) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(fleet.kernel ?? "Your fleet")
                .font(.system(size: 72, weight: .bold))
            Text(fleet.summary)
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

    private var footer: some View {
        HStack(spacing: 24) {
            Text("SecuraCV Witness Wall")
            Text("core \(model.coreVersion)")
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

    /// Troubled first, then offline, then the rest alphabetically — what needs
    /// a person is what a person should see from across the room.
    private func sorted(_ devices: [FleetSnapshot.Device]) -> [FleetSnapshot.Device] {
        devices.sorted { a, b in
            if a.chainIsTroubled != b.chainIsTroubled { return a.chainIsTroubled }
            if a.online != b.online { return !a.online }
            return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
        }
    }
}

// MARK: - Pieces

struct DeviceCard: View {
    let device: FleetSnapshot.Device

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 12) {
                Circle()
                    .fill(dotColor)
                    .frame(width: 18, height: 18)
                Text(device.name)
                    .font(.title2.weight(.semibold))
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
        .background(Color(white: 0.13), in: RoundedRectangle(cornerRadius: 20))
    }

    private var dotColor: Color {
        if device.chainIsTroubled { return .orange }
        return device.online ? .green : Color(white: 0.4)
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
