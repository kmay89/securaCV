// DeviceSolids3DView.swift
//
// The fleet figure you can pick up: a SceneKit rendering of the SAME massing
// solids every other surface draws (FleetSolids.swift — generated), so the
// 3D body can never disagree with the committed figure. No asset files, no
// new dependencies: the geometry is extruded here at runtime from the same
// ring vocabulary iso.mjs and FleetIsoProjector use (a solid is a closed
// outline extruded along an axis; flat facets on purpose — this is the
// technical-drawing language in three dimensions, not a marketing render).
//
// Honesty gate: a massing the ladder calls an idea (`ghost`) never gets a
// solid 3D body — the caller must not present this view for one, and the
// builder returns an empty scene if handed one anyway. Same invariant as
// the flashers' model tier and the dashed-ghost figures.
//
// Deliberately NOT parity-marked: the canonical shared picture stays the
// FleetFigures/FleetIsoProjector turntable that every Apple surface
// compiles; this view is the iPhone/iPad detail sheet's extra reach. The
// Wall can adopt it later by listing the file in its project.yml.

import SwiftUI
import SceneKit

// MARK: - massing → SceneKit geometry

enum DeviceSolidsScene {

    // Same fixed vertex budgets as iso.mjs / FleetIsoProjector, so the 3D
    // silhouette matches the drawn one.
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

    // The extrusion frame (mirrors iso.mjs AXES): axis "z" stands the
    // outline up in plan, axis "y" faces it toward the viewer.
    private static func toWorld(_ axis: String, _ u: Double, _ v: Double, _ w: Double) -> SIMD3<Double> {
        axis == "y" ? SIMD3(u, w, v) : SIMD3(u, v, w)
    }
    private static func sideNormal(_ axis: String, _ nu: Double, _ nv: Double) -> SIMD3<Double> {
        axis == "y" ? SIMD3(nu, 0, nv) : SIMD3(nu, nv, 0)
    }
    private static func capNormal(_ axis: String) -> SIMD3<Double> {
        axis == "y" ? SIMD3(0, 1, 0) : SIMD3(0, 0, 1)
    }

    private static func geometry(for s: FleetSolid) -> SCNGeometry {
        let axis = s.axis == "y" ? "y" : "z"
        let (u, v, w): (Int, Int, Int) = axis == "y" ? (0, 2, 1) : (0, 1, 2)
        let ring: [(Double, Double)]
        let a0: Double
        let span: Double
        if s.kind == .box {
            ring = ringRect(s.at[u], s.at[v], s.size[u], s.size[v], s.r)
            a0 = s.at[w]
            span = s.size[w]
        } else {
            ring = ringCircle(s.at[u], s.at[v], s.r)
            a0 = s.at[w]
            span = s.h
        }
        let a1 = a0 + span

        var pos: [SIMD3<Double>] = []
        var nrm: [SIMD3<Double>] = []
        var tris: [(Int, Int, Int)] = []
        func push(_ p: SIMD3<Double>, _ n: SIMD3<Double>) -> Int {
            pos.append(p)
            nrm.append(n)
            return pos.count - 1
        }
        let n = ring.count
        // Side walls: one flat quad per ring edge, outward normal.
        for i in 0..<n {
            let a = ring[i]
            let b = ring[(i + 1) % n]
            let eu = b.0 - a.0
            let ev = b.1 - a.1
            let len = (eu * eu + ev * ev).squareRoot()
            if len < 1e-9 { continue }
            let N = sideNormal(axis, ev / len, -eu / len)
            let i0 = push(toWorld(axis, a.0, a.1, a0), N)
            let i1 = push(toWorld(axis, b.0, b.1, a0), N)
            let i2 = push(toWorld(axis, b.0, b.1, a1), N)
            let i3 = push(toWorld(axis, a.0, a.1, a1), N)
            tris.append((i0, i1, i2))
            tris.append((i0, i2, i3))
        }
        // Caps: far (+capN) fanned over the CCW ring, near (−capN) reversed.
        let capN = capNormal(axis)
        let far = ring.map { push(toWorld(axis, $0.0, $0.1, a1), capN) }
        for i in 1..<(n - 1) { tris.append((far[0], far[i], far[i + 1])) }
        let negN = -capN
        let near = ring.map { push(toWorld(axis, $0.0, $0.1, a0), negN) }
        for i in 1..<(n - 1) { tris.append((near[0], near[i + 1], near[i])) }

        // Massing mm (+X right / +Y front / +Z up) → SceneKit (+Y up, +Z
        // toward the default camera). The swap is a reflection, so orient
        // every triangle against its own stored normal — exact for flat
        // faces, and the same discipline the flashers' GLB generator holds.
        let mapped = pos.map { SIMD3($0.x, $0.z, $0.y) }
        let mappedN = nrm.map { SIMD3($0.x, $0.z, $0.y) }
        var indices: [Int32] = []
        indices.reserveCapacity(tris.count * 3)
        for (a, b, c) in tris {
            let g = simd_cross(mapped[b] - mapped[a], mapped[c] - mapped[a])
            if simd_dot(g, mappedN[a]) >= 0 {
                indices.append(contentsOf: [Int32(a), Int32(b), Int32(c)])
            } else {
                indices.append(contentsOf: [Int32(a), Int32(c), Int32(b)])
            }
        }

        let vSource = SCNGeometrySource(vertices: mapped.map {
            SCNVector3(Float($0.x), Float($0.y), Float($0.z))
        })
        let nSource = SCNGeometrySource(normals: mappedN.map {
            SCNVector3(Float($0.x), Float($0.y), Float($0.z))
        })
        let element = SCNGeometryElement(indices: indices, primitiveType: .triangles)
        let geo = SCNGeometry(sources: [vSource, nSource], elements: [element])
        geo.materials = [material(for: s.material)]
        return geo
    }

    private static func material(for name: String) -> SCNMaterial {
        let m = SCNMaterial()
        let base = FleetMassing.materials[name] ?? [128, 128, 128]
        let color = UIColor(red: base[0] / 255, green: base[1] / 255,
                            blue: base[2] / 255, alpha: 1)
        m.lightingModel = .physicallyBased
        m.diffuse.contents = color
        m.metalness.contents = name == "metal" ? 0.8 : 0.0
        m.roughness.contents = (name == "glass" || name == "lit") ? 0.35 : 0.85
        if name == "lit" {
            // The one glowing material: a powered screen emits — quietly.
            m.emission.contents = color.withAlphaComponent(0.6)
        }
        return m
    }

    static func makeScene(_ massing: FleetMassing) -> SCNScene {
        let scene = SCNScene()
        guard !massing.ghost, massing.envelope.count == 3 else { return scene }
        let cx = massing.envelope[0] / 2
        let cy = massing.envelope[1] / 2
        let cz = massing.envelope[2] / 2
        let model = SCNNode()
        for s in massing.solids {
            let node = SCNNode(geometry: geometry(for: s))
            node.position = SCNVector3(Float(-cx), Float(-cz), Float(-cy))
            model.addChildNode(node)
        }
        scene.rootNode.addChildNode(model)

        // Slow shop-window turn; a hand on the glass (camera control) takes
        // over. Honors Reduce Motion — a still object is the whole story.
        if !UIAccessibility.isReduceMotionEnabled {
            model.runAction(.repeatForever(.rotateBy(x: 0, y: 2 * .pi, z: 0, duration: 24)))
        }

        let key = SCNNode()
        key.light = SCNLight()
        key.light?.type = .directional
        key.light?.intensity = 900
        key.eulerAngles = SCNVector3(-0.7, 0.6, 0)
        scene.rootNode.addChildNode(key)

        let fill = SCNNode()
        fill.light = SCNLight()
        fill.light?.type = .ambient
        fill.light?.intensity = 350
        scene.rootNode.addChildNode(fill)

        let camera = SCNNode()
        camera.camera = SCNCamera()
        camera.camera?.zNear = 1
        camera.camera?.zFar = 5000
        let extent = max(massing.envelope[0], massing.envelope[1], massing.envelope[2])
        camera.position = SCNVector3(0, Float(extent) * 0.25, Float(extent) * 2.1)
        scene.rootNode.addChildNode(camera)
        return scene
    }
}

// MARK: - the sheet

struct DeviceSolids3DSheet: View {
    let title: String
    let massing: FleetMassing
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            SceneView(
                scene: DeviceSolidsScene.makeScene(massing),
                options: [.allowsCameraControl]
            )
            .ignoresSafeArea(edges: .bottom)
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Done") { dismiss() }
                }
            }
            .safeAreaInset(edge: .bottom) {
                Text("The same massing every surface draws — drag to orbit, pinch to look closer.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .padding(8)
                    .frame(maxWidth: .infinity)
                    .background(.thinMaterial)
            }
        }
    }
}

#if DEBUG
#Preview("3D sheet — first massing") {
    if let massing = FleetMassing.all.values.first(where: { !$0.ghost }) {
        DeviceSolids3DSheet(title: massing.id, massing: massing)
    }
}
#endif
