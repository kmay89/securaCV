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
        // Every CD_DEVICE_TYPE a display config publishes, plus the family
        // string and the two spellings kept for forward tolerance.
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

    /// Every key in the naming table must be a string some build actually
    /// publishes. An entry for a type nothing sends reads like precision and
    /// is fiction — "canary-dash7" and "canary-nightstand-touch" were both in
    /// here once, and no device has ever answered to either.
    ///
    /// Keep this list matched to CD_DEVICE_TYPE across
    /// firmware/configs/canary-display/*/config.h.
    func testTheNamingTableOnlyCarriesTypesThatArePublished() {
        // Display configs (CD_DEVICE_TYPE), plus the Sentinel's
        // SENT_DEVICE_TYPE from firmware/configs/canary-sentinel/door.
        for published in ["canary-dash", "canary-nightstand", "canary-nightstand7",
                          "canary-nightlight", "canary-watch", "canary-sentinel"] {
            XCTAssertNotNil(DeviceNaming.productTitle(forPublishedType: published),
                            "\(published) is published by a firmware config and needs a name")
        }
        // These are NOT published by anything — the Dash 7 answers
        // "canary-dash" and the Nightstand Touch answers "canary-nightstand",
        // because those flavors share an OTA product with their siblings.
        for fiction in ["canary-dash7", "canary-nightstand-touch"] {
            XCTAssertNil(DeviceNaming.productTitle(forPublishedType: fiction),
                         "\(fiction) is not a device type any build publishes")
        }
    }

    /// The board is asked BEFORE the type, because it is sometimes the more
    /// product-precise of the two — and both surfaces that name a device go
    /// through this, so they cannot disagree.
    func testNamingAsksTheBoardBeforeTheTypeWhereTheBoardKnowsBetter() {
        // The Nightstand Touch: same published type as the plain Nightstand,
        // its own pins header. The board names it exactly.
        XCTAssertEqual(DeviceNaming.productName(published: "canary-nightstand",
                                                hardware: "waveshare-esp32s3-touch-lcd169"),
                       "Canary Nightstand Touch")
        // A shared board must NOT lend its title: the 7" glass serves both the
        // Dash 7 and the Nightstand 7, so the type is all we have. "Canary
        // Dash" is less specific than the truth and is not wrong; claiming
        // "Canary Dash 7" would need the device to say so, and it doesn't.
        XCTAssertEqual(DeviceNaming.productName(published: "canary-dash",
                                                hardware: "waveshare-esp32s3-lcd7"),
                       "Canary Dash")
        // The Nightstand 7 on that same shared board DOES publish a precise
        // type, so it gets the precise name.
        XCTAssertEqual(DeviceNaming.productName(published: "canary-nightstand7",
                                                hardware: "waveshare-esp32s3-lcd7"),
                       "Canary Nightstand 7")
        // No board, or a board this build never heard of: fall through.
        XCTAssertEqual(DeviceNaming.productName(published: "canary-watch", hardware: nil),
                       "Canary Watch Station")
        XCTAssertEqual(DeviceNaming.productName(published: "canary-watch",
                                                hardware: "some-future-board"),
                       "Canary Watch Station")
        // Nothing pins a product → nil, and the caller says something coarse.
        XCTAssertNil(DeviceNaming.productName(published: nil, hardware: nil))
        XCTAssertNil(DeviceNaming.productName(published: "canary-something-new", hardware: nil))
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
