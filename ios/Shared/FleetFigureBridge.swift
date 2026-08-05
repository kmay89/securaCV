// FleetFigureBridge.swift — hand-written, unlike FleetFigures.swift beside it.
//
// The generated file knows every figure in the fleet and how to draw it. This
// one is the small amount of judgment that connects it to what the apps
// actually hold: a `DeviceType`, decoded from whatever a witness published.
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
    /// the witness has to tell us which one it is.
    var figureID: String? {
        switch self {
        case .wap: return "device.canary-wap"
        case .vision: return "device.canary-vision"
        case .sense: return "device.canary-sense"
        // .nightlight is nil for the same reason as .display: its figure
        // (the C3 pocket case) enters through the fleet-figures pipeline,
        // and until the generated map carries it the SF moon is honest.
        case .display, .nightlight, .unknown: return nil
        }
    }

    var figure: FleetFigure? {
        guard let id = figureID else { return nil }
        return FleetFigure.all[id]
    }
}

/// What a device looks like, at whatever size the row gives it — falling back
/// to the type's SF Symbol when we cannot honestly draw the thing itself.
///
/// Same footprint either way, so a roster does not reflow as figures arrive
/// for more of the fleet.
struct DeviceFigureIcon: View {
    let deviceType: DeviceType
    var size: CGFloat = 26

    init(_ deviceType: DeviceType, size: CGFloat = 26) {
        self.deviceType = deviceType
        self.size = size
    }

    var body: some View {
        Group {
            if let figure = deviceType.figure {
                FleetFigureView(figure)
            } else {
                Image(systemName: deviceType.sfSymbol)
                    .imageScale(.medium)
                    .foregroundStyle(.secondary)
            }
        }
        .frame(width: size, height: size)
        .accessibilityLabel(deviceType.figure?.title ?? deviceType.role)
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
    }
    .padding()
}
#endif
