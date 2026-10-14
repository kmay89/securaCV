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
            let door: [SetupAction] = family.hasBluetoothDoor ? [.nearbyCanary] : []
            switch family.path {
            case .setupNetwork:
                XCTAssertEqual(actions, door + [.readSetupKey, .joinSetupNetwork, .watchForCanary], family.rawValue)
            case .ownPage:
                // Its own page speaks its own wizard: explained, driven by
                // the person — no control that pretends to drive it.
                XCTAssertEqual(actions, door + [.watchForCanary], family.rawValue)
            }
            XCTAssertEqual(steps.first?.title, family.title)
            XCTAssertFalse(family.tagline.isEmpty)
            XCTAssertFalse(family.hubNote.isEmpty)
        }
    }

    func testTheWAPFallsBackToItsOwnPageAndOnlyDisplaysShowAQR() {
        // The WAP's old fallback, "its bonded Bluetooth service", could
        // never list a device: a new WAP does not advertise that console,
        // and its reads need a PIN confirmed on the device. Its real
        // fallback is its own setup page.
        XCTAssertEqual(CanaryFamily.wap.path, .ownPage)
        let page = SetupGuide.canary(.wap).first { $0.id == "page" }
        XCTAssertNotNil(page)
        XCTAssertTrue(page?.title.hasPrefix("If no card appears") ?? false)
        XCTAssertFalse(SetupGuide.canary(.wap).contains { $0.body.contains("bonded Bluetooth") },
                       "no step promises the bonded path")
        XCTAssertEqual(CanaryFamily.display.path, .setupNetwork(keySource: .glassQR))
        XCTAssertEqual(CanaryFamily.vision.path, .setupNetwork(keySource: .flasher))
        XCTAssertEqual(CanaryFamily.sense.path, .setupNetwork(keySource: .flasher))
    }

    func testTheBluetoothDoorIsOnTheHeadlessWitnessesAndLeadsTheirWalkthrough() {
        // The families whose firmware compiles common/network/improv_ble —
        // nothing here promises a door a device does not open.
        XCTAssertTrue(CanaryFamily.sense.hasBluetoothDoor)
        XCTAssertTrue(CanaryFamily.vision.hasBluetoothDoor)
        XCTAssertTrue(CanaryFamily.wap.hasBluetoothDoor, "the WAP's door hands over its pairing key too")
        XCTAssertFalse(CanaryFamily.display.hasBluetoothDoor, "a display shows a QR on its glass instead")
        for family in CanaryFamily.allCases where family.hasBluetoothDoor {
            let steps = SetupGuide.canary(family)
            XCTAssertEqual(steps[1].action, .nearbyCanary, family.rawValue)
            XCTAssertEqual(steps[1].id, "nearby")
            XCTAssertTrue(steps[2].title.hasPrefix("If no card appears"), "\(family.rawValue): the older path is the fallback")
        }
        XCTAssertTrue(SetupGuide.canary(.wap)[1].body.contains("pairing key"), "the WAP step says what the one tap earns")
        XCTAssertTrue(SetupGuide.canary(.wap)[1].body.contains("never rides Bluetooth"),
                      "and where the key crosses: a claim over the link, the key over Wi-Fi")
        XCTAssertFalse(SetupGuide.canary(.sense)[1].body.contains("pairing key"), "a Sense has no key to hand over")
        // The door's rules, as the firmware keeps them: the first-boot
        // window on every door family; the BOOT tap only where a board
        // reads one (Sense, Vision) — a WAP has no tap door yet, so its
        // step says to power-cycle it instead.
        for family in CanaryFamily.allCases where family.hasBluetoothDoor {
            let body = SetupGuide.canary(family)[1].body
            XCTAssertTrue(body.contains(SetupGuide.doorWindowSentence),
                          "\(family.rawValue): the window is the door's first rule")
            XCTAssertEqual(body.contains(SetupGuide.tapSentence), family != .wap,
                           "\(family.rawValue): only a Sense or Vision reads a BOOT tap for the door")
        }
        XCTAssertTrue(SetupGuide.canary(.wap)[1].body.contains("power-cycle"), "a WAP's door re-arms on a power cycle")
        XCTAssertFalse(SetupGuide.canary(.display)[1].body.contains(SetupGuide.doorWindowSentence),
                       "a display has no door to keep open")
    }

    func testNoStepIsAnEmptyPromise() {
        for step in SetupGuide.allSteps {
            XCTAssertFalse(step.title.isEmpty, step.id)
            XCTAssertGreaterThan(step.body.count, 40, "\(step.id) says too little")
        }
        let ids = SetupGuide.hub.map(\.id)
        XCTAssertEqual(Set(ids).count, ids.count)
    }

    func testEveryStepSaysItselfInOneShortSentence() {
        // The screen shows `short` first and the paragraph behind "How it
        // works" — a step that needs more than twenty words up front is a
        // manual, not a card. The long body stays, honest and pinned above.
        for step in SetupGuide.allSteps {
            let words = step.short.split(separator: " ").count
            XCTAssertGreaterThan(words, 2, "\(step.id): says something")
            XCTAssertLessThanOrEqual(words, 20, "\(step.id): \(step.short)")
            XCTAssertLessThan(step.short.count, step.body.count, "\(step.id): the short line is shorter")
        }
    }

    func testFamiliesMapToTheCoarseTypesTheFleetUses() {
        XCTAssertEqual(CanaryFamily.display.deviceType, .display)
        XCTAssertEqual(CanaryFamily.vision.deviceType, .vision)
        XCTAssertEqual(CanaryFamily.sense.deviceType, .sense)
        XCTAssertEqual(CanaryFamily.wap.deviceType, .wap)
    }
}
