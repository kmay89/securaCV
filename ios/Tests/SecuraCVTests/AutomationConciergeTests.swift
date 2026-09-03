// The concierge's contract, tested where it is pure.
//
// The sentence the owner approves, the readiness ladder that decides when
// the concierge may offer itself, and the signal-to-characteristic map —
// none of it touches HomeKit, so all of it runs host-side. The impure
// writer (HMEventTrigger authoring) is reachable only downstream of a user
// tap on a real device, the same lazy-manager rule the bridge lives by.

import XCTest
@testable import SecuraCV

final class AutomationConciergeTests: XCTestCase {

    private func plan(_ signal: HomeSignal) -> PlannedAutomation {
        PlannedAutomation(
            homeID: UUID(), homeName: "Home",
            accessoryName: "Porch Canary", signal: signal,
            sceneID: UUID(), sceneName: "Bright House")
    }

    func testTheSentenceSaysExactlyWhatTheHouseWillDo() {
        let p = plan(.tamper)
        XCTAssertEqual(p.sentence, "When Porch Canary reports tamper, run Bright House.")
        // Every signal produces a readable sentence — no blank rows.
        for signal in HomeSignal.allCases {
            XCTAssertFalse(plan(signal).sentence.isEmpty)
        }
    }

    func testAuthoredTriggersAreRecognizable() {
        // The removal list keys on this prefix; a rename orphans every
        // automation users already wrote into their homes.
        XCTAssertTrue(plan(.motion).triggerName.hasPrefix("SecuraCV: "))
    }

    func testTheReadinessLadderReadsTopToBottom() {
        typealias R = ConciergeReadiness
        XCTAssertEqual(R.evaluate(isEnabled: false, authorized: true, isAdministrator: true,
                                  accessoryCount: 1, sceneCount: 1, homeHubPresent: true),
                       R.integrationOff)
        XCTAssertEqual(R.evaluate(isEnabled: true, authorized: false, isAdministrator: true,
                                  accessoryCount: 1, sceneCount: 1, homeHubPresent: true),
                       R.needsAuthorization)
        XCTAssertEqual(R.evaluate(isEnabled: true, authorized: true, isAdministrator: false,
                                  accessoryCount: 1, sceneCount: 1, homeHubPresent: true),
                       R.notAdministrator,
                       "writing a trigger needs the admin role — say so, don't fail")
        XCTAssertEqual(R.evaluate(isEnabled: true, authorized: true, isAdministrator: true,
                                  accessoryCount: 0, sceneCount: 1, homeHubPresent: true),
                       R.noAccessories,
                       "the concierge appears with the accessories, not before")
        XCTAssertEqual(R.evaluate(isEnabled: true, authorized: true, isAdministrator: true,
                                  accessoryCount: 1, sceneCount: 0, homeHubPresent: true),
                       R.noScenes,
                       "the concierge runs scenes, it doesn't invent them")
        XCTAssertEqual(R.evaluate(isEnabled: true, authorized: true, isAdministrator: true,
                                  accessoryCount: 1, sceneCount: 1, homeHubPresent: false),
                       R.readyWithoutHomeHub,
                       "no hub means no away automations — one plain warning")
        XCTAssertEqual(R.evaluate(isEnabled: true, authorized: true, isAdministrator: true,
                                  accessoryCount: 1, sceneCount: 1, homeHubPresent: true),
                       R.ready)
    }

    func testCalmStatesSayNothingProblemStatesOweOneSentence() {
        XCTAssertNil(ConciergeReadiness.integrationOff.note, "off is a choice, not a problem")
        XCTAssertNil(ConciergeReadiness.ready.note, "healthy needs no lecture")
        for problem: ConciergeReadiness in [.needsAuthorization, .notAdministrator,
                                            .noAccessories, .noScenes, .readyWithoutHomeHub] {
            XCTAssertNotNil(problem.note)
        }
    }

    @MainActor
    func testAuthoredRecognitionSurvivesRenames() {
        // The Hue lesson: name-keyed sync rots on the first rename. Ours is
        // anchored by UUID — a renamed trigger stays ours; an unanchored
        // stranger with no prefix never becomes ours.
        let id = UUID()
        XCTAssertTrue(HomeKitBridge.isAuthored(
            name: "Whatever the owner renamed it to", id: id,
            anchors: [id.uuidString]))
        XCTAssertTrue(HomeKitBridge.isAuthored(
            name: "SecuraCV: Porch Tamper → Bright House", id: UUID(), anchors: []),
            "pre-anchor authorship still recognized by prefix")
        XCTAssertFalse(HomeKitBridge.isAuthored(
            name: "Good Morning", id: UUID(), anchors: [id.uuidString]))
    }

    @MainActor
    func testAnchorsPersistAndForget() {
        let defaults = UserDefaults(suiteName: "test-anchors-\(UUID().uuidString)")!
        defer { defaults.removePersistentDomain(forName: defaults.description) }
        let id = UUID()
        HomeKitBridge.rememberAuthored(id, defaults: defaults)
        XCTAssertEqual(defaults.stringArray(forKey: HomeKitBridge.authoredIDsKey),
                       [id.uuidString])
        HomeKitBridge.forgetAuthored(id, defaults: defaults)
        XCTAssertEqual(defaults.stringArray(forKey: HomeKitBridge.authoredIDsKey), [])
    }

    func testClassScopedSignalsCollapseOntoMotion() {
        // Same collapse as hapCharacteristic: the class word is consent
        // metadata, not a different sensor.
        let motion = HomeSignal.motion.hmCharacteristicTypeID
        for signal in HomeSignal.allCases where signal.isClassScoped {
            XCTAssertEqual(signal.hmCharacteristicTypeID, motion)
        }
        // And the map is total — every signal resolves to something.
        for signal in HomeSignal.allCases {
            XCTAssertFalse(signal.hmCharacteristicTypeID.isEmpty)
        }
    }

    // ── binding: the service name is the class ──

    private func motionPair(_ service: String) -> ServiceCharacteristic {
        ServiceCharacteristic(serviceName: service,
                              characteristicType: HomeSignal.motion.hmCharacteristicTypeID)
    }

    func testServiceNamesMirrorTheHAPBridge() {
        // Pinned to service_name() in src/bridge/hap/accessory.rs — the one
        // place those strings are minted. If the bridge renames a service,
        // this table (and this test) moves with it.
        XCTAssertEqual(HomeSignal.motion.bridgeServiceName, "Motion")
        XCTAssertEqual(HomeSignal.motionPerson.bridgeServiceName, "Person")
        XCTAssertEqual(HomeSignal.motionVehicle.bridgeServiceName, "Vehicle")
        XCTAssertEqual(HomeSignal.motionAnimal.bridgeServiceName, "Animal")
        XCTAssertEqual(HomeSignal.motionPackage.bridgeServiceName, "Package")
        XCTAssertEqual(HomeSignal.classServiceNames,
                       ["Person", "Vehicle", "Animal", "Package"])
    }

    func testAClassSignalBindsOnlyItsNamedService() {
        // A bridged Canary listing plain Motion first and the class service
        // later: first-of-type would grab Motion; the selector must not.
        let pairs = [
            motionPair("Motion"),
            ServiceCharacteristic(
                serviceName: "Tamper",
                characteristicType: HomeSignal.tamper.hmCharacteristicTypeID),
            motionPair("Person"),
        ]
        XCTAssertEqual(HomeSignal.motionPerson.automationBindingIndex(in: pairs), 2,
                       "Motion (person) rides the Person service, never the first motion tile")
        XCTAssertNil(HomeSignal.motionVehicle.automationBindingIndex(in: pairs),
                     "another class's service is not this class's service")
    }

    func testAMissingClassServiceIsARefusalNotAFallback() {
        let pairs = [motionPair("Motion")]
        XCTAssertNil(HomeSignal.motionPerson.automationBindingIndex(in: pairs),
                     "no Person service, no binding — plain motion is not an honest stand-in")
        // And the writer's refusal reads as one plain sentence.
        XCTAssertFalse(HomeAuthorError.signalNotPublished(.motionPerson).line.isEmpty)
    }

    func testPlainMotionPrefersTheUnscopedService() {
        // Person listed first: plain motion skips past it, so the two
        // automations stay two different automations.
        XCTAssertEqual(HomeSignal.motion.automationBindingIndex(
            in: [motionPair("Person"), motionPair("Motion")]), 1)
        // Only class services carry the type: first-of-type stands, the
        // pre-class behavior.
        XCTAssertEqual(HomeSignal.motion.automationBindingIndex(
            in: [motionPair("Person")]), 0)
        // Nothing carries the type at all: nil — the writer's same refusal.
        XCTAssertNil(HomeSignal.motion.automationBindingIndex(in: []))
    }
}
