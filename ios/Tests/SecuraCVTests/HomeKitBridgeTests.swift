// The shepherd's contract, tested where it is pure.
//
// The bridge never touches HomeKit until a human enables it, so everything
// here runs host-side with no HMHomeManager: the consent vocabulary (the
// dumb-PIR bar), the tamper refusal that mirrors the Rust projection, the
// witness-to-signal mapping, and the standing ladder with its Doctor notes —
// calm states say nothing, problem states say one plain sentence.

import XCTest
@testable import SecuraCV

final class HomeKitBridgeTests: XCTestCase {

    @MainActor
    func testDefaultConsentStopsAtTheDumbPIRBar() {
        let defaults = HomeSignal.defaultEnabled
        for signal in HomeSignal.allCases {
            if signal.isClassScoped {
                XCTAssertFalse(defaults.contains(signal),
                               "\(signal.rawValue) is the step past the bar — off until consented")
            } else {
                XCTAssertTrue(defaults.contains(signal),
                              "\(signal.rawValue) is dumb-PIR grade and starts on")
            }
        }
    }

    @MainActor
    func testTamperCannotBeTurnedOff() {
        let bridge = HomeKitBridge()
        XCTAssertFalse(bridge.setSignal(.tamper, enabled: false),
                       "a witness must not go quiet invisibly")
        XCTAssertTrue(bridge.enabledSignals.contains(.tamper),
                      "the refusal leaves tamper armed")
        XCTAssertTrue(bridge.setSignal(.motionPerson, enabled: true),
                      "class signals are consentable, one by one")
        XCTAssertTrue(bridge.enabledSignals.contains(.motionPerson))
    }

    @MainActor
    func testTamperReportsEvenWhenNeverConsented() {
        let bridge = HomeKitBridge()
        var w = Witness(id: "canary-a")
        w.tamper = true
        XCTAssertTrue(bridge.signals(for: w).contains(.tamper),
                      "tamper rides the union, not the consent set")
    }

    @MainActor
    func testADarkWitnessIsNotResponding() {
        let bridge = HomeKitBridge()
        var w = Witness(id: "canary-a")
        w.link = .lost
        XCTAssertFalse(bridge.signals(for: w).contains(.active),
                       "dark means the house sees not-responding, nothing more")
    }

    @MainActor
    func testLowBatteryProjectsAtTheSeverityLadderThreshold() {
        // One number, one meaning: the projection warns exactly where the
        // app's own severity ladder warns (Witness.lowBatteryThreshold) —
        // the house and the phone can never tell different battery stories.
        let bridge = HomeKitBridge()
        var w = Witness(id: "canary-a")
        w.batteryPct = Witness.lowBatteryThreshold - 1
        XCTAssertTrue(bridge.signals(for: w).contains(.lowBattery),
                      "below the ladder's threshold the house must hear it")
        w.batteryPct = Witness.lowBatteryThreshold
        XCTAssertFalse(bridge.signals(for: w).contains(.lowBattery),
                       "at the threshold the ladder stays calm — so must the projection")
        w.batteryPct = -1
        XCTAssertFalse(bridge.signals(for: w).contains(.lowBattery),
                       "a no-reading sentinel is no signal, never a guess")
        w.batteryPct = nil
        XCTAssertFalse(bridge.signals(for: w).contains(.lowBattery))
    }

    @MainActor
    func testLowBatteryHonorsConsentLikeEveryNonTamperSignal() {
        let bridge = HomeKitBridge()
        XCTAssertTrue(bridge.setSignal(.lowBattery, enabled: false),
                      "low battery is not tamper — a human may quiet it")
        var w = Witness(id: "canary-a")
        w.batteryPct = 3
        XCTAssertFalse(bridge.signals(for: w).contains(.lowBattery),
                       "a quieted signal must stay out of the projection")
    }

    @MainActor
    func testTheStandingLadderReadsTopToBottom() {
        typealias S = HomeKitStanding
        XCTAssertEqual(HomeKitBridge.standing(isEnabled: false, authorized: false,
                                              isDark: false, presentInHome: false,
                                              homeHubPresent: false), S.off)
        XCTAssertEqual(HomeKitBridge.standing(isEnabled: true, authorized: false,
                                              isDark: false, presentInHome: true,
                                              homeHubPresent: true), S.needsAuthorization)
        XCTAssertEqual(HomeKitBridge.standing(isEnabled: true, authorized: true,
                                              isDark: false, presentInHome: false,
                                              homeHubPresent: true), S.notPaired)
        XCTAssertEqual(HomeKitBridge.standing(isEnabled: true, authorized: true,
                                              isDark: true, presentInHome: true,
                                              homeHubPresent: true), S.staleInHome,
                       "the fleet's darkness outranks Apple Home's memory of the device")
        XCTAssertEqual(HomeKitBridge.standing(isEnabled: true, authorized: true,
                                              isDark: false, presentInHome: true,
                                              homeHubPresent: false), S.pairedWithoutHomeHub,
                       "no hub means no automations — say so, once")
        XCTAssertEqual(HomeKitBridge.standing(isEnabled: true, authorized: true,
                                              isDark: false, presentInHome: true,
                                              homeHubPresent: true), S.paired)
    }

    func testCalmStatesSayNothing() {
        XCTAssertNil(HomeKitStanding.off.doctorNote, "off is a choice, not a problem")
        XCTAssertNil(HomeKitStanding.paired.doctorNote, "healthy needs no lecture")
        for problem: HomeKitStanding in [.needsAuthorization, .notPaired,
                                         .pairedWithoutHomeHub, .staleInHome] {
            XCTAssertNotNil(problem.doctorNote, "a problem state owes one plain sentence")
        }
    }
}
