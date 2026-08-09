//  WallStyleTests.swift — profiles change layout and order, never the data.
//
//  The rule: a profile may choose WHICH honest thing leads, not invent one.
//  These tests pin the ordering contracts each room relies on.

import XCTest
@testable import WitnessWall

final class WallStyleTests: XCTestCase {

    private func device(_ name: String, online: Bool = true, chain: String? = "ok") -> FleetSnapshot.Device {
        let json = """
        {"name":"\(name)","online":\(online),"chain":\(chain.map { "\"\($0)\"" } ?? "null")}
        """
        return try! JSONDecoder().decode(FleetSnapshot.Device.self, from: Data(json.utf8))
    }

    func testEveryProfileLeadsWithWhatNeedsAPerson() {
        let devices = [
            device("Studio"),
            device("Garage", online: false),
            device("Back Gate", chain: "degraded"),
        ]
        for profile in WallProfile.allCases {
            let sorted = profile.sorted(devices)
            XCTAssertEqual(sorted.first?.name, "Back Gate",
                           "\(profile.rawValue): a chain that didn't verify must lead")
            XCTAssertEqual(sorted[1].name, "Garage",
                           "\(profile.rawValue): offline comes before healthy")
        }
    }

    /// The counterpart, and the reason the fixture above says "degraded" rather
    /// than "unknown": a device that holds no witness chain of its own says
    /// "unknown", which is an ABSENT CLAIM, not a failure. Ranking it as
    /// trouble sorted every display in the fleet to the top of the wall under
    /// an orange "Record didn't verify" — a sentence about a chain those
    /// devices never had. A profile may choose which honest thing leads; it
    /// must not promote a device by inventing a fault for it.
    func testAChainlessDeviceDoesNotJumpTheQueue() {
        let devices = [
            device("Garage", online: false),
            device("Back Gate", chain: "unknown"),
            device("Studio"),
        ]
        for profile in WallProfile.allCases {
            let sorted = profile.sorted(devices)
            XCTAssertFalse(sorted[0].chainIsTroubled,
                           "\(profile.rawValue): nothing here is a chain failure")
            XCTAssertEqual(sorted.first?.name, "Garage",
                           "\(profile.rawValue): offline is what actually needs a person here")
        }
    }

    func testTheApartmentPullsTheDoorForward() {
        let devices = [
            device("Studio"),
            device("Front Door"),
            device("Balcony"),
        ]
        let sorted = WallProfile.apartment.sorted(devices)
        XCTAssertEqual(sorted.first?.name, "Front Door",
                       "the apartment's one question is the door — it leads when nothing is troubled")

        // But trouble still outranks the door: honesty beats the room.
        let troubled = devices + [device("Studio 2", chain: "degraded")]
        XCTAssertEqual(WallProfile.apartment.sorted(troubled).first?.name, "Studio 2",
                       "a record that didn't verify outranks even the door")
    }

    func testDoorishNamesMatchTheWordsPeopleActuallyUse() {
        for name in ["Front Door", "Entryway", "Peephole", "Hallway", "porch cam"] {
            XCTAssertTrue(WallProfile.isDoorish(name), name)
        }
        for name in ["Studio", "Garage", "Register 2"] {
            XCTAssertFalse(WallProfile.isDoorish(name), name)
        }
    }

    func testTheBoardPacksDenserAndThePeepholeLarger() {
        XCTAssertLessThan(WallProfile.business.tileMinimum, WallProfile.home.tileMinimum,
                          "the Board exists to fit more Canaries per glance")
        XCTAssertGreaterThan(WallProfile.apartment.tileMinimum, WallProfile.home.tileMinimum,
                             "the peephole wants fewer, larger cards")
    }

    func testProfileAndSkinRoundTripTheirRawValuesForPersistence() {
        for p in WallProfile.allCases {
            XCTAssertEqual(WallProfile(rawValue: p.rawValue), p)
        }
        for s in WallSkin.allCases {
            XCTAssertEqual(WallSkin(rawValue: s.rawValue), s)
        }
    }
}
