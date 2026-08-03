// Honeycomb.swift
//
// The hive: a honeycomb arrangement for the fleet (the Apple Watch
// home-screen idiom, borrowed with affection) — cells pack in offset rows,
// so a growing fleet literally fills in a comb. The geometry is a pure,
// host-tested function (HoneycombGeometry) and the SwiftUI Layout is a thin
// shell over it: rows of N, then N−1 nudged half a pitch, vertical pitch
// sin(60°) — real hexagonal packing, not a grid pretending.

import SwiftUI

enum HoneycombGeometry {
    /// sin(60°) = √3/2, full double precision — the vertical pitch factor
    /// between hex-packed rows. (A truncated constant here once made the
    /// packing test's nearest-neighbor distance come out 0.35 µm short.)
    static let rowPitchFactor = 0.866_025_403_784_438_6

    /// How many cells fit in an even row of the given width.
    static func cellsPerRow(width: Double, diameter: Double, gap: Double) -> Int {
        guard width > diameter else { return 2 }
        let extra = (width - diameter) / (diameter + gap)
        return max(2, Int(extra.rounded(.down)) + 1)
    }

    /// Grid slot for item `index`: even rows hold `perRow` cells, odd rows
    /// hold `perRow − 1` (the brick offset), repeating.
    static func slot(index: Int, perRow: Int) -> (row: Int, col: Int) {
        precondition(perRow >= 2)
        let cycle = 2 * perRow - 1
        let block = index / cycle
        let rest = index % cycle
        if rest < perRow { return (row: block * 2, col: rest) }
        return (row: block * 2 + 1, col: rest - perRow)
    }

    /// Center of a slot, in a coordinate space whose origin is the comb's
    /// top-leading corner.
    static func center(row: Int, col: Int, diameter: Double, gap: Double) -> (x: Double, y: Double) {
        let pitch = diameter + gap
        let oddShift = row.isMultiple(of: 2) ? 0.0 : pitch / 2
        let x = diameter / 2 + Double(col) * pitch + oddShift
        let y = diameter / 2 + Double(row) * pitch * rowPitchFactor
        return (x, y)
    }

    /// Total comb height for `count` cells at `perRow`.
    static func height(count: Int, perRow: Int, diameter: Double, gap: Double) -> Double {
        guard count > 0 else { return 0 }
        let lastRow = slot(index: count - 1, perRow: perRow).row
        return diameter + Double(lastRow) * (diameter + gap) * rowPitchFactor
    }
}

/// SwiftUI Layout over the pure geometry above.
struct HoneycombLayout: Layout {
    var diameter: CGFloat = 96
    var gap: CGFloat = 10

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let width = proposal.width ?? 320
        let perRow = HoneycombGeometry.cellsPerRow(width: width, diameter: diameter, gap: gap)
        let height = HoneycombGeometry.height(count: subviews.count, perRow: perRow,
                                              diameter: diameter, gap: gap)
        return CGSize(width: width, height: CGFloat(height))
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let perRow = HoneycombGeometry.cellsPerRow(width: bounds.width, diameter: diameter, gap: gap)
        for (index, subview) in subviews.enumerated() {
            let slot = HoneycombGeometry.slot(index: index, perRow: perRow)
            let c = HoneycombGeometry.center(row: slot.row, col: slot.col,
                                             diameter: diameter, gap: gap)
            subview.place(at: CGPoint(x: bounds.minX + CGFloat(c.x), y: bounds.minY + CGFloat(c.y)),
                          anchor: .center,
                          proposal: ProposedViewSize(width: diameter, height: diameter))
        }
    }
}
