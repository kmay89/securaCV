// FleetSelfReportTests.swift
//
// Pins the `/api/fleet` decoder against the REAL bytes the firmware emits.
//
// The two JSON literals below are not hand-written: they are the verbatim
// stdout of `fleet_selfreport_build()` from
// `firmware/common/fleet_selfreport/fleet_selfreport.h`, compiled with g++ and
// run. So this suite fails if the app's decoder and the firmware's builder ever
// disagree — the same parity discipline as FleetBeaconTests.

import XCTest
@testable import SecuraCV

final class FleetSelfReportTests: XCTestCase {

    /// A WAP-class device with a verifying chain — and a name containing quotes,
    /// to prove the firmware's JSON escaping survives a real decode.
    static let wapBody = #"""
    {"kernel":"Front \"Door\"","verified_through":"now","devices":[{"name":"Front \"Door\"","online":true,"chain":"ok","product":"canary-wap","chain_height":42}]}
    """#

    /// A display: online, but holds no witness chain of its own, so `chain` is
    /// "unknown" and `chain_height` is omitted entirely.
    static let displayBody = #"""
    {"kernel":"Nightstand","verified_through":"now","devices":[{"name":"Nightstand","online":true,"chain":"unknown","product":"canary-display"}]}
    """#

    /// The user's own 7" Nightstand, as the display firmware now describes
    /// itself: its REAL product type (not the flattened "canary-display"), the
    /// board it compiles against, and a hub nobody has set up yet. Verbatim
    /// stdout of fleet_selfreport_build(), like every literal in this file.
    static let nightstand7Body = #"""
    {"kernel":"canary_nightstand7_001","verified_through":"now","devices":[{"name":"canary_nightstand7_001","online":true,"chain":"unknown","product":"canary-nightstand7","hw":"waveshare-esp32s3-lcd7","hub":"none"}]}
    """#

    /// The same board once its hub is configured and reachable.
    static let nightstand7WithHubBody = #"""
    {"kernel":"canary_nightstand7_001","verified_through":"now","devices":[{"name":"canary_nightstand7_001","online":true,"chain":"unknown","product":"canary-nightstand7","hw":"waveshare-esp32s3-lcd7","hub":"ok"}]}
    """#

    /// The whole point of the board field, end to end: these bytes have to
    /// come out of the decoder as a device that can be DRAWN.
    ///
    /// Before this, a display reported "canary-display" — deliberately
    /// unmapped, because four products share it — so the figure lookup
    /// correctly returned nil and every display in the fleet wore the generic
    /// symbol. The board is what makes the shape knowable.
    func testADisplayNowCarriesEnoughToDrawItself() throws {
        let report = try JSONDecoder().decode(
            FleetSelfReport.self,
            from: Data(Self.nightstand7Body.utf8))
        let row = try XCTUnwrap(report.devices.first)

        XCTAssertEqual(row.product, "canary-nightstand7")
        XCTAssertEqual(row.hardware, "waveshare-esp32s3-lcd7")
        // The coarse enum folds the whole display line to one case, so the
        // screen controls are offered and the row is labeled a display.
        XCTAssertEqual(row.deviceType, .display)
        XCTAssertTrue(row.deviceType.servesGlassSettings)
        // And it resolves to a real drawing.
        let figure = FleetFigure.resolve(deviceType: row.deviceType,
                                         published: row.product,
                                         hardware: row.hardware)
        XCTAssertEqual(figure?.id, "device.canary-display-dash7")
        // But that figure's title must NOT be printed as this device's name:
        // the 7" board is also the Dash 7.
        XCTAssertFalse(FleetFigure.namesItsProduct(hardware: row.hardware))
        XCTAssertEqual(DeviceNaming.productTitle(forPublishedType: row.product),
                       "Canary Nightstand 7")
    }

    /// The hub standing, and the one thing it must never do: read as "fine"
    /// when the device never said.
    func testHubStateIsDecodedAndSilenceIsNotReassurance() throws {
        let absent = try JSONDecoder().decode(
            FleetSelfReport.self, from: Data(Self.nightstand7Body.utf8))
        XCTAssertEqual(absent.devices.first?.hubState, HubState.absent)
        XCTAssertEqual(absent.devices.first?.hubState.needsAttention, true)

        let ok = try JSONDecoder().decode(
            FleetSelfReport.self, from: Data(Self.nightstand7WithHubBody.utf8))
        XCTAssertEqual(ok.devices.first?.hubState, HubState.ok)
        XCTAssertEqual(ok.devices.first?.hubState.needsAttention, false)

        // A device on older firmware omits the key entirely. That is
        // `.unknown`, and `.unknown` must not claim a working hub — nor nag
        // about a missing one it knows nothing about.
        let silent = try JSONDecoder().decode(
            FleetSelfReport.self, from: Data(Self.displayBody.utf8))
        XCTAssertEqual(silent.devices.first?.hubState, HubState.unknown)
        XCTAssertEqual(silent.devices.first?.hubState.needsAttention, false)
        XCTAssertNil(silent.devices.first?.hardware)
    }

    func testDecodesTheFirmwaresOwnOutput() throws {
        let r = try FleetSelfReport.decode(Data(Self.wapBody.utf8))
        XCTAssertEqual(r.kernel, #"Front "Door""#, "escaped quotes survive the round trip")
        XCTAssertEqual(r.verifiedThrough, "now")
        XCTAssertEqual(r.devices.count, 1)

        let d = try XCTUnwrap(r.devices.first)
        XCTAssertEqual(d.name, #"Front "Door""#)
        XCTAssertTrue(d.online)
        XCTAssertEqual(d.product, "canary-wap")
        XCTAssertEqual(d.deviceType, .wap, "the product string maps to a device type")
        XCTAssertEqual(d.chainHeight, 42)
        XCTAssertTrue(d.chainVerifies)
    }

    func testDecodesADisplayWithNoChain() throws {
        let r = try FleetSelfReport.decode(Data(Self.displayBody.utf8))
        let d = try XCTUnwrap(r.devices.first)
        XCTAssertEqual(d.deviceType, .display)
        XCTAssertNil(d.chainHeight, "an omitted chain_height stays nil, never 0")
        XCTAssertFalse(d.chainVerifies, #""unknown" is not a verifying chain"#)
    }

    /// Only the exact string "ok" counts. A newer firmware inventing a value —
    /// or an attacker choosing one — must not read as a good chain.
    func testOnlyLiteralOkCountsAsVerifying() {
        for word in ["unknown", "OK", "ok ", "degraded", "true", "", "okay"] {
            let d = FleetSelfDevice(name: "x", online: true, chain: word, product: "canary-wap")
            XCTAssertFalse(d.chainVerifies, "\(word.debugDescription) must not read as a verifying chain")
        }
        XCTAssertTrue(FleetSelfDevice(name: "x", online: true, chain: "ok", product: "canary-wap").chainVerifies)
    }

    /// A Canary that answers at all is worth showing, so a partial body must
    /// degrade field-by-field instead of failing the whole decode.
    func testTolerantOfMissingAndUnknownFields() throws {
        let partial = #"{"kernel":"K","devices":[{"name":"Half","future_key":123}]}"#
        let r = try FleetSelfReport.decode(Data(partial.utf8))
        let d = try XCTUnwrap(r.devices.first)
        XCTAssertEqual(d.name, "Half")
        XCTAssertFalse(d.online, "a missing online field means we were not told it is up")
        XCTAssertEqual(d.deviceType, .unknown, "an absent product is an unknown type, not a crash")
        XCTAssertEqual(r.verifiedThrough, "", "a missing freshness word decodes empty")
    }

    /// A hub answers for itself and its peers — never assume one row.
    func testHubWithSeveralDevices() throws {
        let hub = #"""
        {"kernel":"Hub","verified_through":"now","devices":[\#
        {"name":"Hub","online":true,"chain":"unknown","product":"canary-display"},\#
        {"name":"Front Door","online":true,"chain":"ok","product":"canary-wap","chain_height":7},\#
        {"name":"Nursery","online":false,"chain":"unknown","product":"canary-sense"}]}
        """#
        let r = try FleetSelfReport.decode(Data(hub.utf8))
        XCTAssertEqual(r.devices.count, 3)
        XCTAssertEqual(r.devices.map(\.deviceType), [.display, .wap, .sense])
        XCTAssertFalse(r.devices[2].online)
    }

    func testGarbageIsAnError() {
        XCTAssertThrowsError(try FleetSelfReport.decode(Data("not json".utf8)))
    }

    // ── Reaching the endpoint at all ──
    //
    // The firmware's make_hostname() writes a BARE mDNS label into the `host`
    // TXT key (e.g. "canary-display-a1b2c3"), not the "canary-a3f7.local" the
    // schema doc shows. A URL built from the bare label fails isPrivate, so the
    // probe is dropped before any request — silently disabling /api/fleet for
    // exactly the boards it exists to reach.
    func testBareMDNSLabelGetsItsImpliedLocalSuffix() throws {
        let url = try XCTUnwrap(DeviceAPI.url(forDiscoveredHost: "canary-display-a1b2c3"))
        XCTAssertEqual(url.absoluteString, "http://canary-display-a1b2c3.local")
        XCTAssertTrue(DeviceAPI.isPrivate(url), "the normalized host must survive the private-address gate")
    }

    func testAlreadyQualifiedHostsAreLeftAlone() throws {
        let dotted = try XCTUnwrap(DeviceAPI.url(forDiscoveredHost: "canary-a3f7.local"))
        XCTAssertEqual(dotted.absoluteString, "http://canary-a3f7.local")
        XCTAssertTrue(DeviceAPI.isPrivate(dotted))

        let ip = try XCTUnwrap(DeviceAPI.url(forDiscoveredHost: "192.168.1.47"))
        XCTAssertEqual(ip.absoluteString, "http://192.168.1.47", "an IP must not gain a .local suffix")
        XCTAssertTrue(DeviceAPI.isPrivate(ip))
    }

    func testPublicAddressesStillCannotBeDialed() throws {
        let url = try XCTUnwrap(DeviceAPI.url(forDiscoveredHost: "example.com"))
        XCTAssertFalse(DeviceAPI.isPrivate(url), "normalizing must not widen what we are willing to contact")
        XCTAssertNil(DeviceAPI.url(forDiscoveredHost: "   "), "an empty host is not a URL")
    }

    // ── The birth day ───────────────────────────────────────────────────────
    //
    // These two bodies are also verbatim `fleet_selfreport_build()` output, so
    // the app's reading of `born_day`/`born_exact` is pinned to the bytes the
    // firmware actually writes rather than to what this file assumes.

    /// A Canary that was provisioned and online the same evening.
    static let bornBody = #"""
    {"kernel":"Front Door","verified_through":"now","devices":[{"name":"Front Door","online":true,"chain":"ok","product":"canary-wap","chain_height":42,"born_day":20673,"born_exact":true}]}
    """#

    /// One that sat in a workshop for a week before it first met a clock.
    static let firstDatedBody = #"""
    {"kernel":"Shed","verified_through":"now","devices":[{"name":"Shed","online":true,"chain":"ok","product":"canary-wap","chain_height":7,"born_day":20666,"born_exact":false}]}
    """#

    func testABirthDayIsReadFromTheDevicesOwnBytes() throws {
        let r = try FleetSelfReport.decode(Data(Self.bornBody.utf8))
        let d = try XCTUnwrap(r.devices.first)
        XCTAssertEqual(d.bornDay, 20673)
        XCTAssertTrue(d.bornExact)

        var w = Witness(id: "canary-a3f7")
        FleetMerge.fold(d, into: &w)
        XCTAssertEqual(w.bornDay, 20673)
        XCTAssertTrue(w.bornExact)
        // Days since the epoch, rendered at UTC midnight — a birth day carries
        // no time of day, so the date must land exactly on the day boundary.
        let born = try XCTUnwrap(w.bornOn)
        XCTAssertEqual(born.timeIntervalSince1970, 20673 * 86_400, accuracy: 0.5)
    }

    func testAFirstDatedDayIsNotPromotedToABirthday() throws {
        let r = try FleetSelfReport.decode(Data(Self.firstDatedBody.utf8))
        let d = try XCTUnwrap(r.devices.first)
        XCTAssertEqual(d.bornDay, 20666)
        XCTAssertFalse(d.bornExact,
                       "the device said it learned the date late — the app must carry that through")

        var w = Witness(id: "canary-b1c2")
        FleetMerge.fold(d, into: &w)
        XCTAssertNotNil(w.bornOn, "a late day is still worth showing — it bounds the device's age")
        XCTAssertFalse(w.bornExact)
    }

    /// The device that has never met a clock omits both keys. Nil must survive
    /// as nil: a 0 here would render as 1 January 1970 on the certificate.
    func testADeviceWithNoClockYetReportsNoBirthDay() throws {
        let r = try FleetSelfReport.decode(Data(Self.displayBody.utf8))
        let d = try XCTUnwrap(r.devices.first)
        XCTAssertNil(d.bornDay)
        XCTAssertFalse(d.bornExact, "absent is not exact")

        var w = Witness(id: "lan:nightstand.local#0")
        FleetMerge.fold(d, into: &w)
        XCTAssertNil(w.bornOn, "no born line at all, rather than the epoch")
    }

    /// A firmware that reports a day but no verdict has not earned the word
    /// "born" — the cautious reading is the only safe one, because the flag
    /// exists precisely to hold a claim back.
    func testADayWithoutAVerdictIsNotExact() throws {
        let body = #"""
        {"kernel":"K","verified_through":"now","devices":[{"name":"K","online":true,"chain":"ok","product":"canary-wap","born_day":20673}]}
        """#
        let r = try FleetSelfReport.decode(Data(body.utf8))
        let d = try XCTUnwrap(r.devices.first)
        XCTAssertEqual(d.bornDay, 20673)
        XCTAssertFalse(d.bornExact)
    }

    /// Zero and negatives are the boot epoch showing through, not dates. Folded
    /// to nil at the decoder so exactly one representation of "not known"
    /// reaches the UI.
    func testTheEpochIsNotABirthDay() throws {
        for value in ["0", "-1"] {
            let body = #"{"kernel":"K","verified_through":"now","devices":[{"name":"K","online":true,"chain":"ok","product":"canary-wap","born_day":\#(value),"born_exact":true}]}"#
            let r = try FleetSelfReport.decode(Data(body.utf8))
            let d = try XCTUnwrap(r.devices.first)
            XCTAssertNil(d.bornDay, "born_day \(value) must not become a date")
        }
    }

    /// A birth day is written once on the device and never restated, so a later
    /// row can only ever repeat it. What must NOT happen is the reverse: a row
    /// that has yet to learn the day erasing one the app already holds.
    func testALaterRowCannotUnlearnABirthDay() throws {
        var w = Witness(id: "canary-a3f7")
        let known = try XCTUnwrap(try FleetSelfReport.decode(Data(Self.bornBody.utf8)).devices.first)
        FleetMerge.fold(known, into: &w)
        XCTAssertEqual(w.bornDay, 20673)

        let silent = FleetSelfDevice(name: "Front Door", online: true,
                                     chain: "ok", product: "canary-wap")
        FleetMerge.fold(silent, into: &w)
        XCTAssertEqual(w.bornDay, 20673, "a row with no birth day says nothing, it does not say 'none'")
        XCTAssertTrue(w.bornExact)
    }
}
