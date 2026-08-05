import XCTest
@testable import SecuraCV

/// The figures the phone and the wrist draw (docs/design/FLEET_FIGURES.md).
///
/// `FleetFigures.swift` is generated from the committed CAD and gated by
/// `gen_figures.mjs --check`; what needs pinning HERE is the hand-written
/// bridge beside it — the small amount of judgment that maps a decoded
/// `DeviceType` onto a figure, and the honesty rules that judgment has to keep.
final class FleetFigureTests: XCTestCase {

    func testEveryFigureIDResolves() {
        for type in [DeviceType.wap, .vision, .sense, .display, .nightlight, .unknown] {
            guard let id = type.figureID else { continue }
            XCTAssertNotNil(FleetFigure.all[id],
                            "\(type.rawValue) names figure \(id), which is not in the generated set")
            XCTAssertEqual(type.figure?.id, id)
        }
    }

    /// The one that matters. The display line is four different products and
    /// `DeviceType` collapses them to one case, so any figure chosen for
    /// `.display` would be a coin flip. The firmware lookup made exactly this
    /// mistake with a regex and drew the rectangular nightstand boards as the
    /// round Watch Station drum.
    func testAmbiguousTypesDrawNoFigure() {
        XCTAssertNil(DeviceType.display.figureID,
                     "the display family is several products; a figure here would be a guess")
        XCTAssertNil(DeviceType.unknown.figureID)
        XCTAssertNil(DeviceType.display.figure)
        XCTAssertNil(DeviceType.unknown.figure)
        // The nightlight's figure (the C3 pocket case) arrives through the
        // fleet-figures pipeline; until the generated map carries it, nil
        // keeps the SF moon honest rather than borrowing a sibling's shape.
        XCTAssertNil(DeviceType.nightlight.figureID)
        XCTAssertNil(DeviceType.nightlight.figure)
    }

    func testUnambiguousTypesDoDrawTheirOwnDevice() {
        XCTAssertEqual(DeviceType.wap.figure?.id, "device.canary-wap")
        XCTAssertEqual(DeviceType.vision.figure?.id, "device.canary-vision")
        XCTAssertEqual(DeviceType.sense.figure?.id, "device.canary-sense")
    }

    /// An idea must never render as something you can buy. The generator emits
    /// only ghost faces for that rung; this asserts the rule survives the trip
    /// into Swift, so a concept can't come out solid on the wrist and dashed on
    /// the web.
    func testIdeasCarryNoFilledFaces() {
        let ideas = FleetFigure.all.values.filter { $0.confidence == .idea }
        XCTAssertFalse(ideas.isEmpty, "there are ideas in the fleet to check")
        for figure in ideas {
            XCTAssertFalse(figure.confidence.isBuilt)
            XCTAssertTrue(figure.faces.allSatisfy { $0.kind == .ghost },
                          "\(figure.id) is an idea but carries filled faces")
        }
        for figure in FleetFigure.all.values where figure.confidence != .idea {
            XCTAssertTrue(figure.faces.contains { $0.kind == .face },
                          "\(figure.id) is built but renders as a ghost")
        }
    }

    func testFacesParseIntoDrawablePolygons() {
        for figure in FleetFigure.all.values {
            XCTAssertGreaterThan(figure.size, 0)
            XCTAssertFalse(figure.faces.isEmpty, "\(figure.id) has nothing to draw")
            for face in figure.faces {
                let points = face.points
                XCTAssertGreaterThanOrEqual(points.count, 3,
                                            "\(figure.id) has a face that isn't a polygon")
                for p in points {
                    XCTAssertTrue(p.x.isFinite && p.y.isFinite, "\(figure.id) has a non-finite point")
                    // Everything drawn must be inside the frame the figure
                    // declares — including the contact shadow, which the
                    // generator folds into the fit. It did not always: a round
                    // display shadow reached y = 293.8 in a 256 box and was
                    // being clipped on every surface that drew it.
                    XCTAssertTrue(p.x >= -1 && p.x <= figure.size + 1,
                                  "\(figure.id) has a point outside its own viewBox: x=\(p.x)")
                    XCTAssertTrue(p.y >= -1 && p.y <= figure.size + 1,
                                  "\(figure.id) has a point outside its own viewBox: y=\(p.y)")
                }
            }
        }
    }

    func testColorsDecodeFromTheGeneratedHex() {
        // Shadow faces carry an 8-digit hex (with alpha); solid faces 6.
        for figure in FleetFigure.all.values {
            for face in figure.faces where face.kind != .ghost {
                XCTAssertTrue(face.hex.hasPrefix("#"), "\(figure.id): \(face.hex)")
                XCTAssertTrue(face.hex.count == 7 || face.hex.count == 9,
                              "\(figure.id) has an unparseable color \(face.hex)")
            }
        }
    }

    func testDeviceTypeLookupMatchesTheGeneratedTable() {
        for (deviceType, figureID) in FleetFigure.deviceTypeToFigure {
            XCTAssertNotNil(FleetFigure.all[figureID],
                            "\(deviceType) points at missing figure \(figureID)")
            XCTAssertEqual(FleetFigure.forDeviceType(deviceType)?.id, figureID)
        }
        XCTAssertNil(FleetFigure.forDeviceType("not-a-real-device-type"))
    }
}
