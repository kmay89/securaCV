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

    /// The machine-readable family map (canary-local/devices/ecosystem.json)
    /// is the canonical story; this list is a consumer copy. The belt reads
    /// the JSON off disk so a surface renamed, relinked, or shipped there
    /// fails here type-checked — and in the always-on node gate
    /// (canary-local/tests/ecosystem.test.js) for edits the gated iOS CI
    /// never sees. Same belt-over-the-repo pattern as the const.py mirror.
    func testTheFamilyMapMirrorsEcosystemJSONOnDisk() throws {
        let repoRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()   // SecuraCVTests
            .deletingLastPathComponent()   // Tests
            .deletingLastPathComponent()   // ios
            .deletingLastPathComponent()   // repo root
        let mapURL = repoRoot.appendingPathComponent("canary-local/devices/ecosystem.json")
        try XCTSkipUnless(FileManager.default.fileExists(atPath: mapURL.path),
                          "repo checkout not visible from the test host")

        let json = try XCTUnwrap(
            JSONSerialization.jsonObject(with: Data(contentsOf: mapURL)) as? [String: Any])
        XCTAssertEqual(json["site"] as? String, EcosystemMap.site)
        XCTAssertEqual(json["repo"] as? String, EcosystemMap.repo)
        let canonical = try XCTUnwrap(json["surfaces"] as? [String: [String: String]])

        for surface in EcosystemMap.surfaces {
            let canon = try XCTUnwrap(canonical[surface.id],
                                      "\(surface.id) is not a surface ecosystem.json names")
            XCTAssertEqual(surface.name, canon["name"],
                           "\(surface.id): the app renames a family member")
            XCTAssertEqual(surface.url.absoluteString, canon["url"],
                           "\(surface.id): the app links somewhere the family map doesn't")
            // Availability copy must agree with the canonical status, in both
            // directions: "pending" is the store-pending marker, and a shipped
            // surface still apologizing is as wrong as an overclaim.
            let saysPending = surface.availability.localizedCaseInsensitiveContains("pending")
            XCTAssertEqual(saysPending, canon["status"] == "store-pending",
                           "\(surface.id): availability copy disagrees with canonical status")
        }
    }
}
