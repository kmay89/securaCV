// FleetFigureBridge.swift — hand-written, unlike FleetFigures.swift beside it.
//
// The generated file knows every figure in the fleet and how to draw it. This
// one is the small amount of judgment that connects it to what the apps
// actually hold: a `DeviceType`, decoded from whatever a witness published —
// and, when we kept it, the published string itself.
//
// It is deliberately thin, and deliberately incomplete. See `figureID`.

import SwiftUI

extension DeviceType {
    /// Does this kind of device serve `/api/settings` + `/api/set`?
    ///
    /// Every DISPLAY does (glass_web.cpp is shared by the whole display
    /// line), which is the point: the app used to offer these controls only
    /// on the nightlight, so a Watch Station's brightness and night window
    /// were served to nobody. A witness that isn't glass has no screen to
    /// configure, and offering the screen would be a tap that 404s.
    var servesGlassSettings: Bool {
        switch self {
        case .display, .nightlight: return true
        case .wap, .vision, .sense, .unknown: return false
        }
    }

    /// The figure of this kind of device, or nil when the type does not pin
    /// down one physical product.
    ///
    /// `.display` is nil ON PURPOSE. The display line is four different
    /// products — the round Watch Station, the 4.3" Dash, the 7" Dash, the
    /// 1.69" Nightstand Touch — and this enum collapses them all to one case,
    /// so a figure chosen here would be a coin flip. An earlier version of the
    /// firmware lookup did exactly that with a regex and handed the
    /// rectangular nightstand boards the round drum.
    ///
    /// A wrong picture is worse than no picture: nil falls back to the SF
    /// Symbol, which claims nothing about shape. To draw a specific display,
    /// the witness has to tell us which one it is — which is exactly what the
    /// published-type path in `FleetFigure.resolve` listens for.
    var figureID: String? {
        switch self {
        case .wap: return "device.canary-wap"
        case .vision: return "device.canary-vision"
        case .sense: return "device.canary-sense"
        // One case, one product, one figure: the C3 pocket case entered the
        // fleet-figures pipeline (massing.mjs traces canary_c3_lcd147.scad),
        // so the moon symbol retired honestly.
        case .nightlight: return "device.canary-nightlight"
        case .display, .unknown: return nil
        }
    }

    var figure: FleetFigure? {
        guard let id = figureID else { return nil }
        return FleetFigure.all[id]
    }
}

extension FleetFigure {
    /// The figure for a witness, resolved at three honest precisions — mirror
    /// of the firmware's own lookups in fleet_figures.h, most exact first:
    ///
    ///   1. The BOARD it published (`hw`). The only lookup that is exact
    ///      about the SHAPE, because a build compiles against exactly one
    ///      pins header and the wrong one is a dead device. This tier is what
    ///      finally gave the display line its picture: every non-nightlight
    ///      display used to self-report the family string "canary-display",
    ///      which is unmapped ON PURPOSE (four products wear it), so a
    ///      Nightstand drew the generic marker while the ledger held a
    ///      drawing of it the whole time.
    ///   2. The RAW published device type, through the generated map. Exact
    ///      where a type pins down one product — a witness that says
    ///      "canary-watch" gets the round drum even though the coarse enum
    ///      collapses it to `.display`.
    ///   3. The coarse `DeviceType` default (`figureID` above).
    ///
    /// Nil when none of them pins one product; the caller draws the generic
    /// symbol, never a guess. New boards and new device types resolve here
    /// with no app change the moment the generated maps carry them.
    ///
    /// What this returns is a SHAPE, not a name. One board can serve two
    /// products (the 7" glass is both the Dash 7 and the Nightstand 7), so a
    /// caller that wants to name the device must use the device's own name or
    /// its published type — never the figure's title. `namesItsProduct` below
    /// is the check.
    static func resolve(deviceType: DeviceType, published: String?,
                        hardware: String? = nil) -> FleetFigure? {
        if let board = hardware, !board.isEmpty, let f = FleetFigure.forHardware(board) {
            return f
        }
        if let raw = published, !raw.isEmpty, let f = FleetFigure.forDeviceType(raw) {
            return f
        }
        return deviceType.figure
    }

    /// May this figure's title be shown as the device's product name?
    ///
    /// False when the figure was resolved from a board that carries more than
    /// one product — the drawing is right for all of them, the title names
    /// one. Printing "Canary Dash 7" under a Nightstand 7 would be a wrong
    /// label attached to a right picture, which is worse than no label.
    static func namesItsProduct(hardware: String?) -> Bool {
        guard let board = hardware, !board.isEmpty else { return true }
        return !FleetFigure.sharesBoardAcrossProducts(board)
    }
}

/// What a device looks like, at whatever size the row gives it — falling back
/// to the type's SF Symbol when we cannot honestly draw the thing itself.
///
/// Same footprint either way, so a roster does not reflow as figures arrive
/// for more of the fleet.
struct DeviceFigureIcon: View {
    let deviceType: DeviceType
    var published: String?
    var hardware: String?
    var size: CGFloat = 26

    init(_ deviceType: DeviceType, published: String? = nil,
         hardware: String? = nil, size: CGFloat = 26) {
        self.deviceType = deviceType
        self.published = published
        self.hardware = hardware
        self.size = size
    }

    private var resolved: FleetFigure? {
        FleetFigure.resolve(deviceType: deviceType, published: published,
                            hardware: hardware)
    }

    var body: some View {
        Group {
            if let figure = resolved {
                FleetFigureView(figure)
            } else {
                Image(systemName: deviceType.sfSymbol)
                    .imageScale(.medium)
                    .foregroundStyle(.secondary)
            }
        }
        .frame(width: size, height: size)
        .accessibilityLabel(resolved?.title ?? deviceType.role)
    }
}

#if DEBUG
#Preview("Device figures") {
    VStack(alignment: .leading, spacing: 12) {
        ForEach([DeviceType.wap, .vision, .sense, .display, .nightlight, .unknown], id: \.rawValue) { t in
            HStack(spacing: 10) {
                DeviceFigureIcon(t, size: 44)
                VStack(alignment: .leading) {
                    Text(t.role)
                    Text(t.figureID ?? "no figure — falls back to the symbol")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
        }
        Divider()
        // The published-type path: the coarse enum knows none of these, the
        // generated map knows some — exactly the honest split.
        ForEach(["canary-watch", "canary-nightstand"], id: \.self) { raw in
            HStack(spacing: 10) {
                DeviceFigureIcon(.unknown, published: raw, size: 44)
                Text(raw).font(.caption).foregroundStyle(.secondary)
            }
        }
    }
    .padding()
}
#endif
