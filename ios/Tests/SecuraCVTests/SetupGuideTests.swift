// SetupGuideTests.swift
//
// The walkthroughs are data; these pin their shape. Every family has a
// path and the one action that carries it out, the hub walkthrough acts
// in the order the hub actually demands (watch → account → finish), and
// no step is an empty promise.

import XCTest
@testable import SecuraCV

final class SetupGuideTests: XCTestCase {

    func testTheHubWalkthroughActsInTheOrderTheHubDemands() {
        let actions = SetupGuide.hub.compactMap(\.action)
        XCTAssertEqual(actions, [.watchHub, .ownerAccount, .provisionHub])
        XCTAssertEqual(SetupGuide.hub.first?.id, "need")
        XCTAssertEqual(SetupGuide.hub.last?.id, "next")
    }

    func testEveryFamilyHasAPathAndTheActionThatDrivesIt() {
        for family in CanaryFamily.allCases {
            let steps = SetupGuide.canary(family)
            let actions = steps.compactMap(\.action)
            switch family.path {
            case .setupNetwork:
                XCTAssertEqual(actions, [.readSetupKey, .joinSetupNetwork, .watchForCanary], family.rawValue)
            case .bluetooth:
                XCTAssertEqual(actions, [.bluetoothProvision, .watchForCanary], family.rawValue)
            }
            XCTAssertEqual(steps.first?.title, family.title)
            XCTAssertFalse(family.tagline.isEmpty)
            XCTAssertFalse(family.hubNote.isEmpty)
        }
    }

    func testOnlyTheWAPSpeaksBluetoothAndOnlyDisplaysShowAQR() {
        XCTAssertEqual(CanaryFamily.wap.path, .bluetooth)
        XCTAssertEqual(CanaryFamily.display.path, .setupNetwork(keySource: .glassQR))
        XCTAssertEqual(CanaryFamily.vision.path, .setupNetwork(keySource: .flasher))
        XCTAssertEqual(CanaryFamily.sense.path, .setupNetwork(keySource: .flasher))
    }

    func testNoStepIsAnEmptyPromise() {
        for step in SetupGuide.allSteps {
            XCTAssertFalse(step.title.isEmpty, step.id)
            XCTAssertGreaterThan(step.body.count, 40, "\(step.id) says too little")
        }
        let ids = SetupGuide.hub.map(\.id)
        XCTAssertEqual(Set(ids).count, ids.count)
    }

    func testFamiliesMapToTheCoarseTypesTheFleetUses() {
        XCTAssertEqual(CanaryFamily.display.deviceType, .display)
        XCTAssertEqual(CanaryFamily.vision.deviceType, .vision)
        XCTAssertEqual(CanaryFamily.sense.deviceType, .sense)
        XCTAssertEqual(CanaryFamily.wap.deviceType, .wap)
    }
}
