// AppRouteTests.swift
//
// The deep-link dialect, pinned. Three doors speak it — the notification
// tap (AlertCenter → pendingRoute), the widget tap (.widgetURL), and any
// external securacv:// link — so the parser's judgment calls are contracts:
// round-trip fidelity, nil for everything outside the vocabulary (a stale
// or hostile link must render as NOTHING, never as a guess), and the
// witness anchor as a hint that degrades to the plain tab.

import XCTest
@testable import SecuraCV

final class AppRouteTests: XCTestCase {

    // MARK: - round trips (the widget's url must parse back to itself)

    func testEveryRouteRoundTripsThroughItsOwnURL() {
        for route in [AppRoute.today,
                      .alerts(witnessID: nil),
                      .alerts(witnessID: "canary-porch-01"),
                      .find(witnessID: "canary-porch-01")] {
            XCTAssertEqual(AppRoute(url: route.url), route,
                           "\(route.url) does not parse back to \(route)")
        }
    }

    func testTheURLFormsAreTheDocumentedDialect() {
        XCTAssertEqual(AppRoute.today.url.absoluteString, "securacv://today")
        XCTAssertEqual(AppRoute.alerts(witnessID: nil).url.absoluteString,
                       "securacv://alerts")
        XCTAssertEqual(AppRoute.alerts(witnessID: "abc").url.absoluteString,
                       "securacv://alerts?witness=abc")
        XCTAssertEqual(AppRoute.find(witnessID: "abc").url.absoluteString,
                       "securacv://find?witness=abc")
    }

    func testFindWithNobodyToFindIsNotADestination() {
        // Unlike the alerts anchor (a hint that degrades to the tab), find's
        // witness is required: a search needs a target, so a bare or empty
        // find link renders as nothing.
        XCTAssertNil(AppRoute(url: URL(string: "securacv://find")!))
        XCTAssertNil(AppRoute(url: URL(string: "securacv://find?witness=")!))
    }

    // MARK: - the closed vocabulary (nil, never a guess)

    func testUnknownSchemesAndHostsParseToNothing() {
        for bad in ["https://securacv.com/alerts",   // right host word, wrong scheme
                    "securacv://settings",           // host this build doesn't speak
                    "securacv://",                   // no host at all
                    "otherapp://today"] {            // someone else's dialect
            XCTAssertNil(AppRoute(url: URL(string: bad)!),
                         "\(bad) must not route anywhere")
        }
    }

    func testParsingIsCaseInsensitiveWhereURLsAre() {
        // Schemes and hosts compare case-insensitively on the wire; a link
        // uppercased by a chat app must still land.
        XCTAssertEqual(AppRoute(url: URL(string: "SECURACV://Today")!), .today)
        XCTAssertEqual(AppRoute(url: URL(string: "securacv://ALERTS")!),
                       .alerts(witnessID: nil))
    }

    func testWitnessAnchorIsAHintNotARequirement() {
        // An empty or absent witness degrades to the plain Alerts tab —
        // the tab is always a safe landing.
        XCTAssertEqual(AppRoute(url: URL(string: "securacv://alerts?witness=")!),
                       .alerts(witnessID: nil))
        XCTAssertEqual(AppRoute(url: URL(string: "securacv://alerts?other=x")!),
                       .alerts(witnessID: nil))
        // A percent-encoded id survives decoding intact.
        XCTAssertEqual(AppRoute(url: URL(string: "securacv://alerts?witness=a%20b")!),
                       .alerts(witnessID: "a b"))
    }

    func testSchemeMatchesTheInfoPlistRegistration() throws {
        // Info.plist registers the scheme with iOS; AppRoute.scheme is what
        // the code speaks. The belt reads the plist off disk so the two can
        // only change together (same pattern as the const.py mirror).
        let plistURL = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // SecuraCVTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // ios
            .appendingPathComponent("Support/Info.plist")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: plistURL.path),
                          "repo checkout not visible from the test host")
        let plist = try XCTUnwrap(
            PropertyListSerialization.propertyList(
                from: Data(contentsOf: plistURL), format: nil) as? [String: Any])
        let urlTypes = try XCTUnwrap(plist["CFBundleURLTypes"] as? [[String: Any]])
        let schemes = urlTypes.flatMap { ($0["CFBundleURLSchemes"] as? [String]) ?? [] }
        XCTAssertTrue(schemes.contains(AppRoute.scheme),
                      "Info.plist no longer registers the \(AppRoute.scheme):// scheme")
    }
}
