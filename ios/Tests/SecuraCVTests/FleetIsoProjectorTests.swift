import XCTest
@testable import SecuraCV

/// The contract that keeps the turntable honest.
///
/// FleetIsoProjector is the one hand-written port of iso.mjs on the Apple
/// side. Its rot-guard is parity: re-projecting the generated massing
/// (FleetSolids.swift) at the canonical yaw must reproduce the committed
/// polygons (FleetFigures.swift) — same ops, same order, same fills, points
/// to the hundredth — for EVERY figure in the fleet. Both inputs come out of
/// gen_figures.mjs behind the CI byte gate, so a projector change on the JS
/// side regenerates them and lands here, where a drifted port fails the
/// build by name.
final class FleetIsoProjectorTests: XCTestCase {

    /// r2 rounding in the generator is half a hundredth; the rest is float
    /// slack across two languages' libm. Anything past this is a real drift.
    private let ptTolerance = 0.02

    func testCanonicalYawReproducesEveryCommittedFigure() {
        XCTAssertFalse(FleetMassing.all.isEmpty, "the massing file carries the fleet")
        XCTAssertEqual(Set(FleetMassing.all.keys), Set(FleetFigure.all.keys),
                       "the massing file and the figure file know the same fleet")

        for (id, massing) in FleetMassing.all {
            guard let figure = FleetFigure.all[id] else { continue }
            XCTAssertEqual(massing.rev, figure.rev, "\(id): the two copies carry one rev")

            let ops = FleetIsoProjector.plan(massing, size: figure.size,
                                             yaw: 0, frame: .content)
            XCTAssertEqual(ops.count, figure.faces.count,
                           "\(id): op count diverged from the committed figure")

            for (i, (op, face)) in zip(ops, figure.faces).enumerated() {
                switch face.kind {
                case .shadow: XCTAssertEqual(op.kind, .shadow, "\(id) op \(i)")
                case .face: XCTAssertEqual(op.kind, .face, "\(id) op \(i)")
                case .ghost: XCTAssertEqual(op.kind, .ghost, "\(id) op \(i)")
                }

                let committed = face.points
                XCTAssertEqual(op.pts.count, committed.count,
                               "\(id) op \(i): point count diverged")
                for (p, q) in zip(op.pts, committed) {
                    XCTAssertEqual(Double(p.x), Double(q.x), accuracy: ptTolerance,
                                   "\(id) op \(i): x drifted")
                    XCTAssertEqual(Double(p.y), Double(q.y), accuracy: ptTolerance,
                                   "\(id) op \(i): y drifted")
                }

                if op.kind == .face {
                    assertHexClose(op.fill, face.hex, "\(id) op \(i)")
                }
            }
        }
    }

    /// A full turn stays drawable and stays in frame: the .turntable fit is
    /// computed over the whole revolution precisely so the object neither
    /// breathes nor escapes the box while it spins.
    func testEveryYawStaysInFrameAndDrawable() {
        let sampleIDs = ["device.canary-sense",      // stacked boxes
                         "device.canary-wap",        // the roster's most common
                         "device.canary-nightlight", // the new pocket case
                         "device.canary-display-watch", // y-axis cylinders
                         "board.round-display"]      // cylinders again, board role
        for id in sampleIDs {
            guard let massing = FleetMassing.all[id] else {
                XCTFail("\(id) is missing from the massing file"); continue
            }
            for step in 0..<12 {
                let yaw = Double(step) / 12 * .pi * 2
                let ops = FleetIsoProjector.plan(massing, yaw: yaw, frame: .turntable)
                XCTAssertTrue(ops.contains { $0.kind != .shadow },
                              "\(id) at yaw \(yaw): nothing drawable")
                for op in ops {
                    XCTAssertGreaterThanOrEqual(op.pts.count, 3)
                    for p in op.pts {
                        XCTAssertTrue(p.x.isFinite && p.y.isFinite,
                                      "\(id) at yaw \(yaw): non-finite point")
                        XCTAssertTrue(p.x >= -1 && p.x <= 257 && p.y >= -1 && p.y <= 257,
                                      "\(id) at yaw \(yaw): point escapes the frame (\(p))")
                    }
                }
            }
        }
    }

    /// Turning a screen half a turn shows its back, not an inside-out front:
    /// the cap flip iso.mjs's fixed camera never needed. A y-extruded box is
    /// symmetric, so the op census must hold steady while the polygons move.
    func testHalfTurnFlipsTheCapInsteadOfLosingIt() {
        let panel = FleetMassing(
            id: "test.panel", rev: "00000000", ghost: false,
            envelope: [20, 4, 30],
            solids: [FleetSolid(kind: .box, material: "shell", axis: "y",
                                at: [0, 0, 0], size: [20, 4, 30],
                                r: 0, h: 0, fullDetailOnly: false)])
        let front = FleetIsoProjector.plan(panel, yaw: 0, frame: .turntable)
        let back = FleetIsoProjector.plan(panel, yaw: .pi, frame: .turntable)
        XCTAssertEqual(front.count, back.count,
                       "a symmetric panel must stay whole through a half turn")
        XCTAssertTrue(back.contains { $0.kind == .face },
                      "the back of the panel is a face, not a hole")
    }

    /// The honesty invariant survives rotation: an idea is a dashed ghost
    /// casting no shadow at EVERY angle, not just the canonical one.
    func testIdeasStayGhostsAllTheWayAround() {
        let ideas = FleetMassing.all.values.filter { $0.ghost }
        XCTAssertFalse(ideas.isEmpty, "there are ideas in the fleet to check")
        for massing in ideas {
            for step in 0..<6 {
                let ops = FleetIsoProjector.plan(massing,
                                                 yaw: Double(step) / 6 * .pi * 2,
                                                 frame: .turntable)
                XCTAssertFalse(ops.isEmpty, "\(massing.id): a ghost still draws")
                XCTAssertTrue(ops.allSatisfy { $0.kind == .ghost },
                              "\(massing.id): an idea rendered something solid mid-turn")
            }
        }
    }

    /// Every material a massing names is in the generated palette — the
    /// projector's mid-gray fallback is for hand-authored previews, and the
    /// generated fleet must never need it.
    func testEverySolidNamesAKnownMaterial() {
        for (id, massing) in FleetMassing.all {
            for solid in massing.solids {
                XCTAssertNotNil(FleetMassing.materials[solid.material],
                                "\(id) names unknown material \(solid.material)")
            }
        }
    }

    private func assertHexClose(_ got: String?, _ want: String, _ label: String) {
        guard let got else { return XCTFail("\(label): face op carries no fill") }
        func channels(_ hex: String) -> [Int]? {
            let s = hex.dropFirst()
            guard s.count == 6, let v = Int(s, radix: 16) else { return nil }
            return [(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF]
        }
        guard let a = channels(got), let b = channels(want) else {
            return XCTFail("\(label): unparseable fill \(got) vs \(want)")
        }
        for (x, y) in zip(a, b) {
            XCTAssertLessThanOrEqual(abs(x - y), 1,
                                     "\(label): fill drifted \(got) vs \(want)")
        }
    }
}
