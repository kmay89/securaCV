//  DeviceDetailView.swift — one Canary, up close.
//
//  The tvOS twin of the phone's DeviceFigureCard: the same massing, palette
//  and light, resolved by the same three-rung ladder (board, published type,
//  coarse family). On the phone you drag the part to turn it; on a television
//  the part turns by itself (FleetFigureTurntable's tvOS mode) — a piece on a
//  shelf turntable, readable from the couch.
//
//  Where the pipeline has no honest picture, the view says so with the same
//  sentence the phone uses, instead of guessing. And every line of text below
//  the figure comes from the shared vocabulary — DeviceNaming for what to
//  call it, the massing envelope for how big it really is — so this screen
//  and the phone's detail card can never disagree about a device.

import SwiftUI

struct DeviceDetailView: View {
    let device: FleetSnapshot.Device
    let skin: WallSkin

    @Environment(\.dismiss) private var dismiss

    private var figure: FleetFigure? { device.figure }
    private var massing: FleetMassing? {
        guard let figure else { return nil }
        return FleetMassing.all[figure.id]
    }

    var body: some View {
        ZStack {
            LinearGradient(colors: [skin.backgroundTop, skin.backgroundBottom],
                           startPoint: .top, endPoint: .bottom)
                .ignoresSafeArea()

            VStack(spacing: 28) {
                if let figure, let massing {
                    FleetFigureTurntable(massing, title: figure.title)
                        .frame(width: 560, height: 560)
                    Text(dimsLine(massing.envelope))
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
                }

                Button("Back to the Wall") { dismiss() }
                    .padding(.top, 12)
            }
            .padding(60)
        }
        .preferredColorScheme(skin.isLight ? .light : .dark)
    }

    /// The envelope, read out the user-facing way (width × height × depth) —
    /// the same readout as the phone's detail card, from the same mm.
    private func dimsLine(_ envelope: [Double]) -> String {
        guard envelope.count == 3 else { return "" }
        let mm = { (v: Double) in String(Int(v.rounded())) }
        return "\(mm(envelope[0])) × \(mm(envelope[2])) × \(mm(envelope[1])) mm — true size, from the committed CAD"
    }
}
