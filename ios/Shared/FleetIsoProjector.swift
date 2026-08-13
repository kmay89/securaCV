// FleetIsoProjector.swift — hand-written, beside two generated neighbors.
//
// A Swift port of the fleet's one projector (canary-local/tools/figures/
// iso.mjs), generalized by the single parameter the original deliberately
// lacks: yaw. The committed figures stay pinned to the one canonical camera —
// that sameness is the whole point of the figure system — but a detail view
// can let the user pick the part up and turn it, and for that the phone needs
// to project the massing itself (FleetSolids.swift) rather than repaint
// pre-projected polygons.
//
// HOW THIS CANNOT ROT: at yaw zero this port must reproduce the committed
// FleetFigures polygons to the hundredth — same fills, same paint order, same
// points. FleetIsoProjectorTests asserts exactly that for every figure in the
// fleet, so a change to iso.mjs (which regenerates FleetFigures.swift and
// FleetSolids.swift through the CI byte gate) drags this port — or fails the
// build naming it. The port follows iso.mjs's structure closely on purpose;
// when editing, keep the two side by side.
//
// The rotation is a turntable: the object spins about the vertical axis
// through its footprint's center, under a camera and a light that stay put —
// exactly like turning a printed part in your hand under a desk lamp.
//
// SecuraCV-Parity: every Apple surface that shows a device compiles this.

import Foundation
import SwiftUI

public enum FleetIsoProjector {

    /// One painted polygon, in view coordinates. `fill` is a "#rrggbb" hex for
    /// faces, nil for shadows (the drawer supplies the shadow tone) and ghosts
    /// (stroked, never filled).
    public struct Op: Sendable {
        public enum Kind: Sendable { case shadow, face, ghost }
        public let kind: Kind
        public let pts: [CGPoint]
        public let fill: String?
    }

    /// How to fit the projection into the square frame.
    ///   .content    — the tight fit iso.mjs computes, at this yaw only. This
    ///                 is the parity mode: at yaw 0 it reproduces the
    ///                 committed figures exactly.
    ///   .turntable  — one fixed fit that holds for EVERY yaw, so the object
    ///                 doesn't breathe while it spins. Computed by sampling
    ///                 the fit around the full turn.
    public enum Frame: Sendable { case content, turntable }

    // The camera (a true 30° isometric at (+X,+Y,+Z)) — same constants,
    // same arithmetic as iso.mjs.
    private static let cos30 = 3.0.squareRoot() / 2
    private static let sin30 = 0.5

    private static func project(_ p: SIMD3<Double>) -> (Double, Double) {
        ((p.x - p.y) * cos30, (p.x + p.y) * sin30 - p.z)
    }

    private static func depthKey(_ p: SIMD3<Double>) -> Double { p.x + p.y + p.z }

    private static func shade(_ n: SIMD3<Double>) -> Double {
        let l = FleetMassing.light
        let d = n.x * l[0] + n.y * l[1] + n.z * l[2]
        return FleetMassing.ambient + FleetMassing.diffuse * max(0, min(1, d))
    }

    private static func toneHex(_ material: String, _ k: Double) -> String {
        // Unknown material names cannot happen for generated massings; falling
        // back to mid-gray keeps a hand-authored preview from crashing.
        let base = FleetMassing.materials[material] ?? [128, 128, 128]
        var out = "#"
        for c in base {
            let v = Int(max(0, min(255, (c * k).rounded())))
            out += String(format: "%02x", v)
        }
        return out
    }

    // ── outlines (fixed vertex budgets, same as iso.mjs) ─────────────────

    private static let arcSeg = 6
    private static let circleSeg = 32

    private static func ringRect(_ x0: Double, _ y0: Double, _ w: Double,
                                 _ d: Double, _ r0: Double) -> [(Double, Double)] {
        let r = min(r0, w / 2, d / 2)
        if r <= 1e-6 {
            return [(x0, y0), (x0 + w, y0), (x0 + w, y0 + d), (x0, y0 + d)]
        }
        var pts: [(Double, Double)] = []
        let corners: [(Double, Double, Double, Double)] = [
            (x0 + w - r, y0 + r, -Double.pi / 2, 0),
            (x0 + w - r, y0 + d - r, 0, Double.pi / 2),
            (x0 + r, y0 + d - r, Double.pi / 2, Double.pi),
            (x0 + r, y0 + r, Double.pi, 1.5 * Double.pi),
        ]
        for (cx, cy, a0, a1) in corners {
            for i in 0...arcSeg {
                let a = a0 + (a1 - a0) * (Double(i) / Double(arcSeg))
                pts.append((cx + r * cos(a), cy + r * sin(a)))
            }
        }
        return pts
    }

    private static func ringCircle(_ cx: Double, _ cy: Double, _ r: Double) -> [(Double, Double)] {
        (0..<circleSeg).map { i in
            let a = (Double(i) / Double(circleSeg)) * Double.pi * 2
            return (cx + r * cos(a), cy + r * sin(a))
        }
    }

    // ── prisms (a closed outline extruded along y or z) ──────────────────

    private struct Prism {
        let ring: [(Double, Double)]
        let a0: Double
        let a1: Double
        let axisY: Bool  // extruded along +Y (a screen, a lens); else +Z
    }

    private static func prism(of s: FleetSolid) -> Prism {
        let axisY = s.axis == "y"
        let u = 0, v = axisY ? 2 : 1, w = axisY ? 1 : 2
        switch s.kind {
        case .box:
            return Prism(ring: ringRect(s.at[u], s.at[v], s.size[u], s.size[v], s.r),
                         a0: s.at[w], a1: s.at[w] + s.size[w], axisY: axisY)
        case .cyl, .disc:
            return Prism(ring: ringCircle(s.at[u], s.at[v], s.r),
                         a0: s.at[w], a1: s.at[w] + s.h, axisY: axisY)
        }
    }

    private static func toWorld(_ p: Prism, _ u: Double, _ v: Double, _ w: Double) -> SIMD3<Double> {
        p.axisY ? SIMD3(u, w, v) : SIMD3(u, v, w)
    }

    private static func sideNormal(_ p: Prism, _ nu: Double, _ nv: Double) -> SIMD3<Double> {
        p.axisY ? SIMD3(nu, 0, nv) : SIMD3(nu, nv, 0)
    }

    private static func capNormal(_ p: Prism) -> SIMD3<Double> {
        p.axisY ? SIMD3(0, 1, 0) : SIMD3(0, 0, 1)
    }

    private static func corners(of s: FleetSolid) -> [SIMD3<Double>] {
        let p = prism(of: s)
        var out: [SIMD3<Double>] = []
        for (u, v) in p.ring {
            out.append(toWorld(p, u, v, p.a0))
            out.append(toWorld(p, u, v, p.a1))
        }
        return out
    }

    private static func center(of s: FleetSolid) -> SIMD3<Double> {
        let p = prism(of: s)
        var su = 0.0, sv = 0.0
        for (u, v) in p.ring { su += u; sv += v }
        let n = Double(p.ring.count)
        return toWorld(p, su / n, sv / n, (p.a0 + p.a1) / 2)
    }

    /// The visible faces of one convex prism after the turntable rotation,
    /// shaded — sides in ring order, then whichever cap faces the camera.
    /// Mirrors iso.mjs facesOf, plus the cap flip its fixed camera never
    /// needed: spin a screen half a turn and its BACK cap is the visible one.
    private static func faces(of s: FleetSolid, rotate: (SIMD3<Double>) -> SIMD3<Double>,
                              rotateN: (SIMD3<Double>) -> SIMD3<Double>)
        -> [(pts: [SIMD3<Double>], k: Double)] {
        let p = prism(of: s)
        var out: [(pts: [SIMD3<Double>], k: Double)] = []
        let n = p.ring.count
        for i in 0..<n {
            let a = p.ring[i]
            let b = p.ring[(i + 1) % n]
            let eu = b.0 - a.0
            let ev = b.1 - a.1
            let len = hypot(eu, ev)
            if len < 1e-9 { continue }
            let nr = rotateN(sideNormal(p, ev / len, -eu / len))
            if nr.x + nr.y + nr.z <= 1e-9 { continue }  // faces away from the camera
            out.append((pts: [rotate(toWorld(p, a.0, a.1, p.a0)),
                              rotate(toWorld(p, b.0, b.1, p.a0)),
                              rotate(toWorld(p, b.0, b.1, p.a1)),
                              rotate(toWorld(p, a.0, a.1, p.a1))],
                        k: shade(nr)))
        }
        let capN = rotateN(capNormal(p))
        if capN.x + capN.y + capN.z > 1e-9 {
            out.append((pts: p.ring.map { rotate(toWorld(p, $0.0, $0.1, p.a1)) }, k: shade(capN)))
        } else if capN.x + capN.y + capN.z < -1e-9 {
            out.append((pts: p.ring.map { rotate(toWorld(p, $0.0, $0.1, p.a0)) }, k: shade(-capN)))
        }
        return out
    }

    // ── the plan ─────────────────────────────────────────────────────────

    /// Project a massing at a yaw, fitted into a `size`-unit square. The
    /// returned ops are in paint order: shadow, then solids far-to-near.
    public static func plan(_ massing: FleetMassing, size: Double = 256,
                            pad: Double = 8, yaw: Double = 0,
                            frame: Frame = .content) -> [Op] {
        let solids = massing.solids
        guard !solids.isEmpty else { return [] }

        // The turntable axis: vertical, through the footprint's center.
        var mn = SIMD3<Double>(repeating: .infinity)
        var mx = SIMD3<Double>(repeating: -.infinity)
        for s in solids {
            for c in corners(of: s) {
                mn = SIMD3(min(mn.x, c.x), min(mn.y, c.y), min(mn.z, c.z))
                mx = SIMD3(max(mx.x, c.x), max(mx.y, c.y), max(mx.z, c.z))
            }
        }
        let cx = (mn.x + mx.x) / 2
        let cy = (mn.y + mx.y) / 2

        func rotator(_ phi: Double) -> ((SIMD3<Double>) -> SIMD3<Double>,
                                        (SIMD3<Double>) -> SIMD3<Double>) {
            let c = cos(phi), s = sin(phi)
            let rp: (SIMD3<Double>) -> SIMD3<Double> = { p in
                let dx = p.x - cx, dy = p.y - cy
                return SIMD3(cx + dx * c - dy * s, cy + dx * s + dy * c, p.z)
            }
            let rn: (SIMD3<Double>) -> SIMD3<Double> = { n in
                SIMD3(n.x * c - n.y * s, n.x * s + n.y * c, n.z)
            }
            return (rp, rn)
        }

        // The contact shadow: the rotated footprint at z=0 — skipped for
        // ghosts (nothing is there to cast one), same as iso.mjs.
        func shadowRing(rotate: (SIMD3<Double>) -> SIMD3<Double>) -> [(Double, Double)]? {
            guard !massing.ghost else { return nil }
            var fx0 = Double.infinity, fy0 = Double.infinity
            var fx1 = -Double.infinity, fy1 = -Double.infinity
            for s in solids {
                for c0 in corners(of: s) {
                    let c = rotate(c0)
                    fx0 = min(fx0, c.x); fx1 = max(fx1, c.x)
                    fy0 = min(fy0, c.y); fy1 = max(fy1, c.y)
                }
            }
            let pad2 = max(fx1 - fx0, fy1 - fy0) * 0.06
            return ringRect(fx0 - pad2, fy0 - pad2, (fx1 - fx0) + 2 * pad2,
                            (fy1 - fy0) + 2 * pad2, min(fx1 - fx0, fy1 - fy0) * 0.35 + pad2)
        }

        // Fit: the projected bounding box of everything drawn — at this yaw
        // (.content, the iso.mjs arithmetic), or over a sampled full turn
        // (.turntable, so the object holds still in the frame as it spins).
        var minX = Double.infinity, minY = Double.infinity
        var maxX = -Double.infinity, maxY = -Double.infinity
        func see(_ p: SIMD3<Double>) {
            let (px, py) = project(p)
            minX = min(minX, px); maxX = max(maxX, px)
            minY = min(minY, py); maxY = max(maxY, py)
        }
        func fit(at phi: Double) {
            let (rp, _) = rotator(phi)
            for s in solids { for c in corners(of: s) { see(rp(c)) } }
            if let ring = shadowRing(rotate: rp) {
                for (x, y) in ring { see(SIMD3(x, y, 0)) }
            }
        }
        switch frame {
        case .content:
            fit(at: yaw)
        case .turntable:
            for i in 0..<36 { fit(at: Double(i) / 36 * Double.pi * 2) }
        }
        let span = max(maxX - minX, maxY - minY, 1e-6)
        let sc = (size - 2 * pad) / span
        let ox = pad + ((size - 2 * pad) - (maxX - minX) * sc) / 2 - minX * sc
        let oy = pad + ((size - 2 * pad) - (maxY - minY) * sc) / 2 - minY * sc

        let (rp, rn) = rotator(yaw)
        func flat(_ pts: [SIMD3<Double>]) -> [CGPoint] {
            pts.map { p in
                let (px, py) = project(p)
                return CGPoint(x: px * sc + ox, y: py * sc + oy)
            }
        }

        var ops: [Op] = []
        if let ring = shadowRing(rotate: rp) {
            ops.append(Op(kind: .shadow, pts: flat(ring.map { SIMD3($0.0, $0.1, 0) }), fill: nil))
        }

        // Painter's algorithm across solids, nearest last; ties break on the
        // massing's own order so the paint order is total.
        let ordered = solids.indices
            .map { i in (i: i, key: depthKey(rp(center(of: solids[i])))) }
            .sorted { ($0.key, $0.i) < ($1.key, $1.i) }

        for entry in ordered {
            let s = solids[entry.i]
            for face in faces(of: s, rotate: rp, rotateN: rn) {
                if massing.ghost {
                    ops.append(Op(kind: .ghost, pts: flat(face.pts), fill: nil))
                } else {
                    ops.append(Op(kind: .face, pts: flat(face.pts),
                                  fill: toneHex(s.material, face.k)))
                }
            }
        }
        return ops
    }
}

/// A figure you can pick up and turn: drag horizontally to spin the part on
/// its vertical axis, double-tap to set it back down at the fleet's canonical
/// angle. The massing, the palette and the light are all generated from the
/// same CAD the thumbnails trace, so the part in your hand is the part on
/// the shelf.
public struct FleetFigureTurntable: View {
    public let massing: FleetMassing
    public let title: String

    @State private var yaw: Double = 0
    @State private var yawAtDragStart: Double?

    public init(_ massing: FleetMassing, title: String) {
        self.massing = massing
        self.title = title
    }

    /// How fast the ambient turntable turns where nobody can drag it —
    /// shelf pace, one lap in ~24 seconds, slow enough to read as an object
    /// on display rather than a loading spinner.
    private static let ambientRadiansPerSecond = 0.26

    #if os(tvOS)
    /// Reduce Motion means the part holds still. The pose it holds is the
    /// canonical camera (yaw zero) — the fleet's one shared picture — so the
    /// still view is exactly the committed figure, not a frozen mid-spin.
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    #endif

    public var body: some View {
        #if os(tvOS)
        // A television has no drag. The part turns by itself, the way a piece
        // on a shelf turntable does — the same massing, palette and light as
        // the phone, at couch distance. The phase comes from the clock so
        // every card on screen turns in step.
        if reduceMotion {
            turntable(at: 0)
                .accessibilityLabel(title)
        } else {
            TimelineView(.animation(minimumInterval: 1.0 / 30.0)) { context in
                turntable(at: context.date.timeIntervalSinceReferenceDate
                    .truncatingRemainder(dividingBy: 86_400) * Self.ambientRadiansPerSecond)
            }
            .accessibilityLabel(title)
        }
        #else
        turntable(at: yaw)
            .gesture(
                DragGesture(minimumDistance: 2)
                    .onChanged { g in
                        let start = yawAtDragStart ?? yaw
                        yawAtDragStart = start
                        yaw = start + Double(g.translation.width) * 0.012
                    }
                    .onEnded { _ in yawAtDragStart = nil }
            )
            .onTapGesture(count: 2) {
                withAnimation(.spring(duration: 0.5)) { yaw = 0 }
            }
            .accessibilityLabel(title)
            .accessibilityHint("Drag to turn the part; double-tap to reset")
        #endif
    }

    private func turntable(at yaw: Double) -> some View {
        Canvas { ctx, size in
            let side = min(size.width, size.height)
            let s = side / 256
            let ops = FleetIsoProjector.plan(massing, size: 256, yaw: yaw, frame: .turntable)
            for op in ops {
                guard op.pts.count > 2 else { continue }
                var path = Path()
                path.move(to: CGPoint(x: op.pts[0].x * s, y: op.pts[0].y * s))
                for p in op.pts.dropFirst() {
                    path.addLine(to: CGPoint(x: p.x * s, y: p.y * s))
                }
                path.closeSubpath()
                switch op.kind {
                case .ghost:
                    ctx.stroke(path, with: .color(.secondary),
                               style: StrokeStyle(lineWidth: 1.25 * s, dash: [3 * s, 2.5 * s]))
                case .shadow:
                    ctx.fill(path, with: .color(Color(.sRGB, red: 0, green: 0, blue: 0,
                                                      opacity: Double(0x24) / 255)))
                case .face:
                    ctx.fill(path, with: .color(Self.color(fromHex: op.fill ?? "#808080")))
                }
            }
        }
    }

    private static func color(fromHex hex: String) -> Color {
        var v: UInt64 = 0
        Scanner(string: String(hex.dropFirst())).scanHexInt64(&v)
        return Color(.sRGB,
                     red: Double((v >> 16) & 0xFF) / 255,
                     green: Double((v >> 8) & 0xFF) / 255,
                     blue: Double(v & 0xFF) / 255)
    }
}

#if DEBUG
#Preview("Turntable — Canary Sense") {
    VStack {
        if let m = FleetMassing.all["device.canary-sense"] {
            FleetFigureTurntable(m, title: "Canary Sense")
                .frame(width: 280, height: 280)
        }
    }
    .padding()
}
#endif
