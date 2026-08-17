// DeviceFigureCard.swift
//
// The device itself, up close: the detail view's hero. Where the fleet-figure
// pipeline can draw this hardware honestly, the card is a turntable — the
// same massing, palette and light every other surface uses, that the user can
// pick up and spin (FleetIsoProjector). Where it can't, the card says so
// instead of guessing: the generic symbol and one honest line. The moment the
// generated map learns this device's published type, the turntable appears
// with no app change — that is the pipeline healing the gap, not this view.

import SwiftUI

struct DeviceFigureCard: View {
    let witness: Witness
    @State private var show3D = false

    private var figure: FleetFigure? {
        FleetFigure.resolve(deviceType: witness.deviceType,
                            published: witness.publishedType,
                            hardware: witness.hardware)
    }

    var body: some View {
        VStack(spacing: Theme.s) {
            if let figure, let massing = FleetMassing.all[figure.id] {
                FleetFigureTurntable(massing, title: figure.title)
                    .frame(height: 200)
                    .frame(maxWidth: .infinity)
                VStack(spacing: 2) {
                    Text(productName)
                        .font(.subheadline.weight(.medium))
                    Text(rungLine(figure.confidence) + dimsLine(massing.envelope))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text("Drag to turn · double-tap to set it down")
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
                if !massing.ghost {
                    // The 3D sheet (DeviceSolids3DView): the same massing,
                    // picked up. Never offered for a ghost — an idea does
                    // not get a solid body, here or anywhere.
                    Button {
                        show3D = true
                    } label: {
                        Label("Pick it up in 3D", systemImage: "rotate.3d")
                            .font(.caption)
                    }
                    .buttonStyle(.bordered)
                    .sheet(isPresented: $show3D) {
                        DeviceSolids3DSheet(title: productName, massing: massing)
                    }
                }
            } else {
                Image(systemName: witness.deviceType.sfSymbol)
                    .font(.system(size: 44))
                    .foregroundStyle(.secondary)
                    .frame(height: 80)
                Text("No honest picture of this hardware yet — it wears the generic marker until the fleet figures carry it.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
        }
        .padding(.vertical, Theme.s)
    }

    /// What to CALL this device, which is not always what to call its figure:
    /// a shared board's title names one of the products it serves. Resolved
    /// through DeviceNaming so this card and the birth certificate below it
    /// can never disagree, and falling back to the coarse family rather than
    /// to a product the device never claimed.
    private var productName: String {
        DeviceNaming.productName(published: witness.publishedType,
                                 hardware: witness.hardware)
            ?? witness.deviceType.role
    }

    /// The ladder rung, in the ladder's own words (docs/design/
    /// FLEET_FIGURES.md): derived from evidence on disk, never hand-typed —
    /// so this copy only ever repeats the verdict, it cannot upgrade it.
    private func rungLine(_ c: FleetFigure.Confidence) -> String {
        switch c {
        case .shipping: return "Traced from the committed CAD"
        case .confirmed: return "In the catalog — design settled"
        case .prototype: return "In development — drawn from its design record"
        case .idea: return "An idea, drawn as one"
        }
    }

    private func dimsLine(_ envelope: [Double]) -> String {
        guard envelope.count == 3 else { return "" }
        let mm = { (v: Double) in String(Int(v.rounded())) }
        // envelope is [w, d, h] in the figure frame; read it out as the
        // user-facing width × height × depth.
        return " · \(mm(envelope[0])) × \(mm(envelope[2])) × \(mm(envelope[1])) mm"
    }
}

#if DEBUG
#Preview("Figure card — demo fleet") {
    List {
        ForEach(DemoFleet.witnesses()) { w in
            Section(w.displayName) { DeviceFigureCard(witness: w) }
        }
    }
}
#endif
