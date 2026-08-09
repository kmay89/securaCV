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
    }

    func testUnambiguousTypesDoDrawTheirOwnDevice() {
        XCTAssertEqual(DeviceType.wap.figure?.id, "device.canary-wap")
        XCTAssertEqual(DeviceType.vision.figure?.id, "device.canary-vision")
        XCTAssertEqual(DeviceType.sense.figure?.id, "device.canary-sense")
        // One case, one product: the C3 pocket case entered the pipeline, so
        // the nightlight's moon symbol retired honestly.
        XCTAssertEqual(DeviceType.nightlight.figure?.id, "device.canary-nightlight")
    }

    /// The two-precision ladder (mirror of the firmware's two lookups): the
    /// raw published type resolves exactly through the generated map, the
    /// coarse enum default catches the rest, and ambiguity stays nil.
    func testResolveListensToThePublishedTypeFirst() {
        // The enum collapses "canary-watch" to .unknown, but the wire string
        // pins one product — the round drum.
        XCTAssertEqual(FleetFigure.resolve(deviceType: .unknown,
                                           published: "canary-watch")?.id,
                       "device.canary-display-watch")
        // Config spelling and wire spelling land on one row (canonical_dt).
        XCTAssertEqual(FleetFigure.resolve(deviceType: .unknown,
                                           published: "canary_wap")?.id,
                       "device.canary-wap")
        // "canary-nightstand" is published by two different panels, so the
        // map omits it and .display stays nil: no figure, no guess.
        XCTAssertNil(FleetFigure.resolve(deviceType: .display,
                                         published: "canary-nightstand"))
        // A published string the map has never heard of falls back to the
        // enum's own default rather than dropping to nothing.
        XCTAssertEqual(FleetFigure.resolve(deviceType: .sense,
                                           published: "canary-sense-v99")?.id,
                       "device.canary-sense")
        XCTAssertNil(FleetFigure.resolve(deviceType: .unknown, published: nil))
    }

    /// The BOARD outranks everything, because it is the only input that is
    /// exact about the shape.
    ///
    /// This is the tier that gave the display line its picture back. Every
    /// non-nightlight display used to self-report the family string
    /// "canary-display" — unmapped on purpose, since four products wear it —
    /// so a Nightstand resolved to nothing and drew the generic marker while
    /// the ledger held a drawing of it the whole time.
    func testResolveAsksTheBoardBeforeTheProduct() {
        // The exact case from the field: a 7" Nightstand. Its type is shared
        // (the 7" glass is also a Dash 7) and its family string pins nothing,
        // but its board pins the shape.
        XCTAssertEqual(FleetFigure.resolve(deviceType: .display,
                                           published: "canary-display",
                                           hardware: "waveshare-esp32s3-lcd7")?.id,
                       "device.canary-display-dash7")
        // The board wins even when the published type would have resolved,
        // because it is the stricter statement — not because the type is
        // wrong.
        XCTAssertEqual(FleetFigure.resolve(deviceType: .unknown,
                                           published: "canary-watch",
                                           hardware: "waveshare-esp32s3-lcd147")?.id,
                       "device.canary-display-nightstand")
        // A board this build has never heard of falls through to the type
        // rather than dropping to nothing.
        XCTAssertEqual(FleetFigure.resolve(deviceType: .unknown,
                                           published: "canary-watch",
                                           hardware: "some-future-board")?.id,
                       "device.canary-display-watch")
        // And an empty board id is the same as no board id.
        XCTAssertEqual(FleetFigure.resolve(deviceType: .sense,
                                           published: nil, hardware: "")?.id,
                       "device.canary-sense")
    }

    /// A right picture may still carry a wrong name. One board can serve two
    /// products, and the figure's title names one of them — so callers must
    /// ask before printing it as this device's product.
    func testAFigureFromASharedBoardMayNotNameTheProduct() {
        // The 7" glass is both the Dash 7 and the Nightstand 7.
        XCTAssertTrue(FleetFigure.sharesBoardAcrossProducts("waveshare-esp32s3-lcd7"))
        XCTAssertFalse(FleetFigure.namesItsProduct(hardware: "waveshare-esp32s3-lcd7"))
        // A board that serves one product names it fine.
        XCTAssertFalse(FleetFigure.sharesBoardAcrossProducts("waveshare-esp32s3-lcd147"))
        XCTAssertTrue(FleetFigure.namesItsProduct(hardware: "waveshare-esp32s3-lcd147"))
        // No board id at all: nothing to object to, so the title stands.
        XCTAssertTrue(FleetFigure.namesItsProduct(hardware: nil))
        XCTAssertTrue(FleetFigure.namesItsProduct(hardware: ""))
    }

    /// Every board the generator mapped must resolve, and to a figure that
    /// exists — the Swift mirror of the firmware's kHardware table.
    func testEveryMappedBoardResolvesToARealFigure() {
        XCTAssertFalse(FleetFigure.hardwareToFigure.isEmpty)
        for (board, figureID) in FleetFigure.hardwareToFigure {
            XCTAssertNotNil(FleetFigure.all[figureID],
                            "board \(board) maps to \(figureID), which is not a figure")
            XCTAssertEqual(FleetFigure.forHardware(board)?.id, figureID)
        }
        // Every shared board is a board we actually carry.
        for board in FleetFigure.sharedBoards {
            XCTAssertNotNil(FleetFigure.hardwareToFigure[board],
                            "\(board) is marked shared but has no figure")
        }
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
            // The map is keyed on the wire-canonical spelling (what mDNS
            // `dt` carries), so a key must already be its own canonical form.
            XCTAssertEqual(FleetFigure.canonicalDeviceType(deviceType), deviceType,
                           "\(deviceType) is not wire-canonical")
        }
        XCTAssertNil(FleetFigure.forDeviceType("not-a-real-device-type"))
    }
}
