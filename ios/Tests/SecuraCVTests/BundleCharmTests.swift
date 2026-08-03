// BundleCharmTests.swift
//
// The charm is a contract too: the canary's chirp and the mascot imageset
// are generated-and-committed resources, and a bundling slip (a path missing
// from project.yml, a rename) would otherwise fail SILENTLY — the code
// degrades to no-sound/no-image by design. These tests make the absence
// loud, in CI, where it costs seconds. (The tvOS store-asset lesson: assert
// the resource exists where it ships.)

import XCTest
import UIKit
@testable import SecuraCV

final class BundleCharmTests: XCTestCase {
    func testTheChirpShipsInTheAppBundle() {
        XCTAssertNotNil(Bundle.main.url(forResource: "canary-verified", withExtension: "wav"),
                        "Sounds/canary-verified.wav missing from the app bundle — " +
                        "regenerate with scripts/make_chirp.py and check project.yml's Sounds path")
    }

    func testTheMascotShipsInTheAssetCatalog() {
        XCTAssertNotNil(UIImage(named: "Canary"),
                        "the Canary imageset is missing — regenerate with scripts/make_brand_assets.py")
    }
}
