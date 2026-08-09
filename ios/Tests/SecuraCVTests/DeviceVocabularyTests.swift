// DeviceVocabularyTests.swift
//
// What a device CALLS itself, and what the app makes of it.
//
// These pin the decoder against the firmware's actual vocabulary — the
// CD_DEVICE_TYPE values in firmware/configs/canary-display/*/config.h — rather
// than against the enum's own raw values. That distinction is the bug this
// suite exists for: `DeviceType` was a strict rawValue lookup, so every string
// the display line publishes fell through to `.unknown`, which drew a generic
// bird on the Fleet tab and switched off the screen controls on the only
// devices that serve them.

import XCTest
@testable import SecuraCV

final class DeviceVocabularyTests: XCTestCase {

    /// Every device type the display configs publish must decode as a display.
    /// Keep this list matched to CD_DEVICE_TYPE across
    /// firmware/configs/canary-display/*/config.h.
    func testTheWholeDisplayLineDecodesAsADisplay() {
        for raw in ["canary-dash", "canary-dash7", "canary-nightstand",
                    "canary-nightstand7", "canary-nightstand-touch",
                    "canary-watch", "canary-display"] {
            XCTAssertEqual(DeviceType(tolerant: raw), .display,
                           "\(raw) must decode as a display")
            XCTAssertTrue(DeviceType(tolerant: raw).servesGlassSettings,
                          "\(raw) serves /api/settings and must be offered them")
        }
    }

    /// The nightlight stays its own case — it has a lamp, and therefore a
    /// whole section of controls no other display has.
    func testTheNightlightIsNotFoldedIntoTheDisplayLine() {
        XCTAssertEqual(DeviceType(tolerant: "canary-nightlight"), .nightlight)
        XCTAssertTrue(DeviceType.nightlight.servesGlassSettings)
    }

    /// Spelling tolerance, matched to how the figure map canonicalizes: a
    /// config says `canary_wap`, the wire says `canary-wap`, and they are one
    /// device type.
    func testSpellingIsCanonicalizedTheSameWayTheFigureMapDoesIt() {
        XCTAssertEqual(DeviceType(tolerant: "canary_wap"), .wap)
        XCTAssertEqual(DeviceType(tolerant: "CANARY-WAP"), .wap)
        XCTAssertEqual(DeviceType(tolerant: "canary nightstand7"), .display)
    }

    /// A type from a newer fleet than this build is `.unknown` — reported,
    /// never dropped, and never guessed into a family it might not belong to.
    func testUnknownTypesStayUnknownRatherThanBeingGuessed() {
        XCTAssertEqual(DeviceType(tolerant: "canary-something-new"), .unknown)
        XCTAssertEqual(DeviceType(tolerant: ""), .unknown)
        XCTAssertEqual(DeviceType(tolerant: nil), .unknown)
        // Specifically NOT prefix-matched into the display line: a future
        // "canary-dashcam" is not a display, and offering it a screen it
        // hasn't got would be a tap that 404s.
        XCTAssertEqual(DeviceType(tolerant: "canary-dashcam"), .unknown)
        XCTAssertFalse(DeviceType.unknown.servesGlassSettings)
    }

    // MARK: - naming

    /// The names shown to a person, and the one thing this must never do:
    /// print a wire identifier where a product name belongs.
    func testProductNamesComeFromTheTableNotFromTheWire() {
        XCTAssertEqual(DeviceNaming.productTitle(forPublishedType: "canary-nightstand7"),
                       "Canary Nightstand 7")
        XCTAssertEqual(DeviceNaming.productTitle(forPublishedType: "canary_watch"),
                       "Canary Watch Station")
        // Unknown returns nil so the caller falls back to something coarse and
        // true, rather than to "canary-something-new" on a card.
        XCTAssertNil(DeviceNaming.productTitle(forPublishedType: "canary-something-new"))
        XCTAssertNil(DeviceNaming.productTitle(forPublishedType: ""))
    }

    // MARK: - the merge rules for the two new fields

    /// The board fills a gap and never replaces one: two transports reporting
    /// the same device must not be able to make its picture flicker.
    func testTheBoardFillsGapsAndNeverReplaces() {
        var w = Witness(id: "x")
        w.hardware = "waveshare-esp32s3-lcd7"
        let row = FleetSelfDevice(name: "n", online: true, chain: "unknown",
                                  product: "canary-nightstand",
                                  hardware: "waveshare-esp32s3-lcd147")
        FleetMerge.fold(row, into: &w)
        XCTAssertEqual(w.hardware, "waveshare-esp32s3-lcd7")

        var empty = Witness(id: "y")
        FleetMerge.fold(row, into: &empty)
        XCTAssertEqual(empty.hardware, "waveshare-esp32s3-lcd147")
    }

    /// The hub standing is the opposite: live state, so a newer answer must
    /// win. Otherwise the app keeps telling an owner to set up a hub they
    /// just finished setting up.
    func testTheHubStandingIsReplacedNotFilled() {
        var w = Witness(id: "x")
        w.hub = .absent
        FleetMerge.fold(FleetSelfDevice(name: "n", online: true, chain: "unknown",
                                        product: "canary-dash", hub: "ok"),
                        into: &w)
        XCTAssertEqual(w.hub, .ok)

        // Silence, though, is not an answer and must not clear what we know.
        FleetMerge.fold(FleetSelfDevice(name: "n", online: true, chain: "unknown",
                                        product: "canary-dash"),
                        into: &w)
        XCTAssertEqual(w.hub, .ok)
    }
}
