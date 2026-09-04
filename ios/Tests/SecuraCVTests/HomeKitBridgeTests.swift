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

    /// Consent persists now, so every bridge in these tests gets its own
    /// throwaway store — sharing `.standard` would let one test's consent
    /// leak into the next test's restore.
    private func freshDefaults() -> UserDefaults {
        let suite = "homekit-bridge-tests-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        addTeardownBlock { defaults.removePersistentDomain(forName: suite) }
        return defaults
    }

    @MainActor
    private func freshBridge() -> HomeKitBridge {
        HomeKitBridge(defaults: freshDefaults())
    }

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
        let bridge = freshBridge()
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
        let bridge = freshBridge()
        var w = Witness(id: "canary-a")
        w.tamper = true
        XCTAssertTrue(bridge.signals(for: w).contains(.tamper),
                      "tamper rides the union, not the consent set")
    }

    @MainActor
    func testADarkWitnessIsNotResponding() {
        let bridge = freshBridge()
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
        let bridge = freshBridge()
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
        let bridge = freshBridge()
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

    // ── consent persistence: decisions survive a relaunch ──

    @MainActor
    func testConsentSurvivesRelaunch() {
        let defaults = freshDefaults()
        let before = HomeKitBridge(defaults: defaults)
        before.isEnabled = true
        XCTAssertTrue(before.setSignal(.motionPerson, enabled: true))
        XCTAssertTrue(before.setSignal(.occupancy, enabled: false))

        // "Relaunch" is a new bridge over the same store.
        let after = HomeKitBridge(defaults: defaults)
        XCTAssertTrue(after.isEnabled, "the opt-in is a decision, not a session")
        XCTAssertTrue(after.enabledSignals.contains(.motionPerson),
                      "a granted class consent survives the relaunch")
        XCTAssertFalse(after.enabledSignals.contains(.occupancy),
                       "a revoked consent stays revoked")
        XCTAssertTrue(after.enabledSignals.contains(.tamper))
    }

    @MainActor
    func testAStoredSetMissingTamperGetsItBack() {
        // A doctored (or stale) store is not a quieter way around the
        // setSignal refusal: restore re-arms tamper unconditionally.
        let defaults = freshDefaults()
        defaults.set([HomeSignal.motion.rawValue, HomeSignal.occupancy.rawValue],
                     forKey: HomeKitBridge.enabledSignalsKey)
        let bridge = HomeKitBridge(defaults: defaults)
        XCTAssertTrue(bridge.enabledSignals.contains(.tamper),
                      "restore must re-insert tamper, whatever the plist says")
        XCTAssertTrue(bridge.enabledSignals.contains(.motion),
                      "the stored consents themselves are honored")
    }

    @MainActor
    func testUnknownStoredIdsDropSilently() {
        // The tolerant-decoder rule: ids from a newer build (or garbage)
        // vanish without taking the known consents with them.
        let defaults = freshDefaults()
        defaults.set(["motion", "motion_unicorn", "42", ""],
                     forKey: HomeKitBridge.enabledSignalsKey)
        let bridge = HomeKitBridge(defaults: defaults)
        XCTAssertEqual(bridge.enabledSignals, [.motion, .tamper])
    }

    @MainActor
    func testAnEmptyOrGarbledStoreMeansTheDefaults() {
        // Nothing stored: off, and the dumb-PIR default set.
        let empty = freshBridge()
        XCTAssertFalse(empty.isEnabled, "opt-in; off by default")
        XCTAssertEqual(empty.enabledSignals, HomeSignal.defaultEnabled)

        // The wrong TYPE under the key reads as nothing stored — the
        // defaults again, not a crash and not an empty consent set.
        let defaults = freshDefaults()
        defaults.set("not an array", forKey: HomeKitBridge.enabledSignalsKey)
        let garbled = HomeKitBridge(defaults: defaults)
        XCTAssertEqual(garbled.enabledSignals, HomeSignal.defaultEnabled)
    }

    // ── the class-scoped motion signals, live via the attributed fold ──

    /// A witness whose plain .motion signal is derivable and whose seeing
    /// claim is fresh — the shape a paired camera-line device produces once
    /// its /api/fleet self row carries `seeing`.
    @MainActor
    private func seeingWitness(at seeingAt: Date) -> Witness {
        var w = Witness(id: "canary-a")
        w.deviceType = .vision
        w.lastEventSeverity = .notice
        w.seeingClass = .person
        w.seeingScore = 91
        w.seeingAt = seeingAt
        return w
    }

    @MainActor
    func testClassScopedMotionDerivesOnlyWithConsentAndAFreshClaim() {
        let bridge = freshBridge()

        // Fresh claim, but the class signal was never consented: the house
        // hears plain motion (dumb-PIR grade, default-on) and nothing finer.
        let fresh = seeingWitness(at: Date())
        XCTAssertTrue(bridge.signals(for: fresh).contains(.motion))
        XCTAssertFalse(bridge.signals(for: fresh).contains(.motionPerson),
                       "a class projects only after its own consent, one by one")

        // Consented and fresh: the refinement projects beside plain motion.
        XCTAssertTrue(bridge.setSignal(.motionPerson, enabled: true))
        XCTAssertTrue(bridge.signals(for: fresh).contains(.motionPerson))

        // Consented but STALE: seeingNow ages the claim out, so the class
        // derives nothing — "was seeing a person two hours ago" must never
        // wear the present tense in someone's home.
        let stale = seeingWitness(at: Date().addingTimeInterval(-3600))
        XCTAssertTrue(bridge.signals(for: stale).contains(.motion))
        XCTAssertFalse(bridge.signals(for: stale).contains(.motionPerson))
    }

    @MainActor
    func testClassIsARefinementOfMotionNeverASecondOpinion() {
        let bridge = freshBridge()
        _ = bridge.setSignal(.motionPerson, enabled: true)

        // A fresh seeing claim on a witness whose plain .motion is NOT
        // derivable (no motion-grade event) projects no class signal: the
        // class narrows motion, it must not invent it.
        var w = Witness(id: "canary-a")
        w.deviceType = .vision
        w.lastEventSeverity = .ok
        w.seeingClass = .person
        w.seeingAt = Date()
        XCTAssertFalse(bridge.signals(for: w).contains(.motion))
        XCTAssertFalse(bridge.signals(for: w).contains(.motionPerson))
    }
}
