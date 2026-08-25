// EcosystemMapTests.swift
//
// The family map's honesty rules, pinned. The map exists to fix a
// legibility failure (no surface named the others); these tests exist so
// fixing it can't quietly recreate the competitors' failure modes — an
// availability overclaim, a link off the brand domains, a surface that
// vanishes from the list.

import XCTest
@testable import SecuraCV

final class EcosystemMapTests: XCTestCase {

    func testEverySurfaceHasCompleteHonestCopy() {
        XCTAssertFalse(EcosystemMap.surfaces.isEmpty)
        for surface in EcosystemMap.surfaces {
            XCTAssertFalse(surface.name.isEmpty)
            XCTAssertFalse(surface.sfSymbol.isEmpty)
            XCTAssertFalse(surface.job.isEmpty)
            XCTAssertFalse(surface.availability.isEmpty)
        }
    }

    func testIDsAreUnique() {
        let ids = EcosystemMap.surfaces.map(\.id)
        XCTAssertEqual(ids.count, Set(ids).count, "two rows sharing an id would fight in ForEach")
    }

    func testLinksStayOnTheBrandDomainsOverHTTPS() {
        for surface in EcosystemMap.surfaces {
            XCTAssertEqual(surface.url.scheme, "https")
            let host = surface.url.host ?? ""
            XCTAssertTrue(host == "securacv.com" || host == "github.com",
                          "\(surface.id) links off the brand domains: \(surface.url)")
        }
    }

    /// The one rule that keeps this list from becoming the overclaim it was
    /// built to prevent: a surface a normal person cannot install today must
    /// say so in its availability line, not imply a download that isn't
    /// there. The Witness Wall is that surface until the tvOS store pipeline
    /// is credentialed — if that ships, update the copy AND this pin.
    func testUninstallableSurfacesSaySo() {
        let wall = EcosystemMap.surfaces.first { $0.id == "witness-wall" }
        XCTAssertNotNil(wall)
        XCTAssertTrue(wall?.availability.localizedCaseInsensitiveContains("pending") == true,
                      "the Wall's availability line must carry the honest status")
    }

    func testTheFamilyNamesTheCoreSurfaces() {
        let ids = Set(EcosystemMap.surfaces.map(\.id))
        for expected in ["lab", "flasher", "witness-wall", "hub"] {
            XCTAssertTrue(ids.contains(expected), "the family map lost \(expected)")
        }
    }
}
