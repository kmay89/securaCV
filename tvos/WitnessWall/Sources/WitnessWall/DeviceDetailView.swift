//  DeviceDetailView.swift — one Canary, up close.
//
//  The tvOS twin of the phone's DeviceFigureCard: the same massing, palette
//  and light, resolved by the same three-rung ladder (board, published type,
//  coarse family). On the phone you drag the part to turn it; on a television
//  the part turns by itself (FleetFigureTurntable's tvOS mode) — a piece on a
//  shelf turntable, readable from the couch.
//
//  LIVE, NOT A SOUVENIR: this view holds the device's IDENTITY and reads the
//  device itself out of `model.state` on every render. The first cut stored a
//  copy of the row instead, which meant a detail screen left open across a
//  poll kept saying "Online · record ok" after the model had learned the
//  device was dark, its chain troubled, or the whole snapshot stale — a
//  frozen sentence wearing a live screen's authority. Same rule as the Wall:
//  everything drawn derives from `model.state`, staleness included.
//
//  Where the pipeline has no honest picture, the view says so with the same
//  sentence the phone uses, instead of guessing — and the dimension caption
//  carries the figure's own confidence rung, so a sketch is labeled a
//  sketch, never sold as CAD.

import SwiftUI

struct DeviceDetailView: View {
    let model: WallModel
    /// The NAME the viewer clicked — identity, not data. The data is looked
    /// up fresh each render.
    let deviceID: String
    let skin: WallSkin

    @Environment(\.dismiss) private var dismiss

    /// The current fleet, with its staleness verdict, straight from the model.
    private var snapshot: (fleet: FleetSnapshot, asOf: Date, staleReason: String?)? {
        switch model.state {
        case .live(let fleet, let asOf): return (fleet, asOf, nil)
        case .stale(let fleet, let since, let reason): return (fleet, since, reason)
        default: return nil
        }
    }

    private var device: FleetSnapshot.Device? {
        snapshot?.fleet.devices.first { $0.id == deviceID }
    }

    var body: some View {
        ZStack {
            LinearGradient(colors: [skin.backgroundTop, skin.backgroundBottom],
                           startPoint: .top, endPoint: .bottom)
                .ignoresSafeArea()

            if let device {
                detail(for: device)
            } else {
                // The device fell out of the report (or the hub was never
                // heard from again) while this screen was open. Say that,
                // rather than keeping a portrait up as if nothing happened.
                VStack(spacing: 24) {
                    Text(deviceID)
                        .font(.system(size: 56, weight: .bold))
                        .foregroundStyle(skin.ink)
                    Text("Not in the fleet's latest report.")
                        .font(.title3)
                        .foregroundStyle(.secondary)
                    Button("Back to the Wall") { dismiss() }
                }
                .padding(60)
            }
        }
        .preferredColorScheme(skin.isLight ? .light : .dark)
    }

    private func detail(for device: FleetSnapshot.Device) -> some View {
        VStack(spacing: 28) {
            if let stale = snapshot?.staleReason, let asOf = snapshot?.asOf {
                // The same loudest-element rule the Wall applies: old data
                // never reads as now, on any screen.
                StatusBanner(
                    tone: .warning,
                    title: "Showing the last report received at \(asOf.formatted(date: .omitted, time: .shortened))",
                    detail: stale
                )
            }

            if let figure = device.figure, let massing = FleetMassing.all[figure.id] {
                FleetFigureTurntable(massing, title: figure.title)
                    .frame(width: 540, height: 540)
                Text(rungLine(figure.confidence) + dimsLine(massing.envelope))
                    .font(.callout)
                    .foregroundStyle(.tertiary)
            } else {
                Image(systemName: device.deviceType.sfSymbol)
                    .font(.system(size: 120))
                    .foregroundStyle(.secondary)
                    .frame(height: 300)
                Text("No honest picture of this hardware yet — it wears the generic marker until the fleet figures carry it.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 900)
            }

            VStack(spacing: 10) {
                Text(device.name)
                    .font(.system(size: 64, weight: .bold))
                    .foregroundStyle(skin.ink)
                if let product = device.productName {
                    Text(product)
                        .font(.title2)
                        .foregroundStyle(.secondary)
                }
                Text(device.wallStatusLine)
                    .font(.title3)
                    .foregroundStyle(device.chainIsTroubled ? Color.orange : Color.secondary)
                if device.hubState.needsAttention {
                    Text(device.wallHubLine)
                        .font(.callout)
                        .foregroundStyle(.tertiary)
                }
                // The same one quiet wellbeing line the card shows — defined
                // once (wallWellbeingLine), so the grid and this screen can
                // never phrase the room two ways. Absent keys draw nothing:
                // absence is "cannot say", never an empty calm room.
                if let wellbeing = device.wallWellbeingLine {
                    Text(wellbeing)
                        .font(.callout)
                        .foregroundStyle(.tertiary)
                }
            }

            Button("Back to the Wall") { dismiss() }
                .padding(.top, 12)
        }
        .padding(60)
    }

    /// The ladder rung, in the ladder's own words — the SAME four sentences
    /// the phone's DeviceFigureCard prints (docs/design/FLEET_FIGURES.md).
    /// Derived from evidence on disk, never hand-typed, so this caption can
    /// only repeat the verdict — a sketch is introduced as one, and nothing
    /// here can promote it to CAD.
    private func rungLine(_ confidence: FleetFigure.Confidence) -> String {
        switch confidence {
        case .shipping: return "Traced from the committed CAD"
        case .confirmed: return "In the catalog — design settled"
        case .prototype: return "In development — drawn from its design record"
        case .idea: return "An idea, drawn as one"
        }
    }

    /// The envelope, read out the user-facing way (width × height × depth) —
    /// the same readout as the phone's detail card, from the same mm.
    private func dimsLine(_ envelope: [Double]) -> String {
        guard envelope.count == 3 else { return "" }
        let mm = { (v: Double) in String(Int(v.rounded())) }
        return " · \(mm(envelope[0])) × \(mm(envelope[2])) × \(mm(envelope[1])) mm"
    }
}
