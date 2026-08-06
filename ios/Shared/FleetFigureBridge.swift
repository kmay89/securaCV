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
    /// The figure for a witness, resolved at two honest precisions — mirror
    /// of the firmware's own two lookups in fleet_figures.h:
    ///
    ///   1. The RAW published device type, through the generated map. Exact:
    ///      the map carries every type the firmware's own vocabulary can pin
    ///      to one product, and grows in lockstep with the pipeline — a
    ///      witness that says "canary-watch" gets the round drum even though
    ///      the coarse enum collapses it to `.unknown`.
    ///   2. The coarse `DeviceType` default (`figureID` above).
    ///
    /// Nil when neither pins one product; the caller draws the generic
    /// symbol, never a guess. New device types resolve here with no app
    /// change the moment the generated map carries them.
    static func resolve(deviceType: DeviceType, published: String?) -> FleetFigure? {
        if let raw = published, !raw.isEmpty, let f = FleetFigure.forDeviceType(raw) {
            return f
        }
        return deviceType.figure
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
    var size: CGFloat = 26

    init(_ deviceType: DeviceType, published: String? = nil, size: CGFloat = 26) {
        self.deviceType = deviceType
        self.published = published
        self.size = size
    }

    private var resolved: FleetFigure? {
        FleetFigure.resolve(deviceType: deviceType, published: published)
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
