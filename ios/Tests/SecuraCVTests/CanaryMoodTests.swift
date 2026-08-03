// CanaryMoodTests.swift
//
// The living canary's feelings, pinned to the display firmware's engine
// (bird_mood.h is the source of truth — these tests encode ITS math, so a
// drifted port fails here, on the simulator, not on a user's wrist telling
// a different story than their nightstand). And the two rules that outrank
// cuteness: every face maps to nameable state, and a real unacknowledged
// alarm hides the bird entirely.

import XCTest
@testable import SecuraCV

final class CanaryMoodTests: XCTestCase {
    // MARK: - the anxiety floor (weights 2/4/3/2/1, cap 14 — bird_mood.h)

    func testAnxietyFloorUsesTheFirmwaresExactWeights() {
        var i = CanaryMoodInputs()
        i.staleWitnesses = 1
        XCTAssertEqual(CanaryMoodEngine.anxietyFloor(i), 2)
        i = CanaryMoodInputs(); i.lostWitnesses = 1
        XCTAssertEqual(CanaryMoodEngine.anxietyFloor(i), 4)
        i = CanaryMoodInputs(); i.hubFlapping = true
        XCTAssertEqual(CanaryMoodEngine.anxietyFloor(i), 3)
        i = CanaryMoodInputs(); i.linksDown = true
        XCTAssertEqual(CanaryMoodEngine.anxietyFloor(i), 2)
        i = CanaryMoodInputs(); i.unackedOld = 1
        XCTAssertEqual(CanaryMoodEngine.anxietyFloor(i), 1)
        i = CanaryMoodInputs(); i.lostWitnesses = 9
        XCTAssertEqual(CanaryMoodEngine.anxietyFloor(i), 14, "capped at BIRD_ANXIETY_MAX")
    }

    // MARK: - the minute tick

    func testTroubleRisesInstantlyAndQuietDecaysOnePointPerHour() {
        var m = CanaryMoodState()
        var i = CanaryMoodInputs()
        i.lostWitnesses = 2                       // floor 8
        CanaryMoodEngine.minute(&m, i)
        XCTAssertEqual(m.anxiety, 8, "rises to the floor in one tick")
        XCTAssertFalse(m.dayClean)

        i = CanaryMoodInputs()                    // trouble resolved, not verified
        for _ in 0..<59 { CanaryMoodEngine.minute(&m, i) }
        XCTAssertEqual(m.anxiety, 8, "59 quiet minutes decay nothing")
        CanaryMoodEngine.minute(&m, i)
        XCTAssertEqual(m.anxiety, 7, "the 60th quiet minute decays one point")
    }

    func testAFullyVerifiedPassSnapsAnxietyToZero() {
        var m = CanaryMoodState(anxiety: 9, trustDays: 0, dayClean: false, calmMinutes: 30)
        var i = CanaryMoodInputs()
        i.allVerified = true
        CanaryMoodEngine.minute(&m, i)
        XCTAssertEqual(m.anxiety, 0, "everything answered and proved — the snap")
    }

    // MARK: - trust ladder

    func testCleanDaysBuildTheStreakAndOneDirtyDayResetsIt() {
        var m = CanaryMoodState()
        CanaryMoodEngine.rollover(&m)
        CanaryMoodEngine.rollover(&m)
        XCTAssertEqual(m.trustDays, 2)
        m.dayClean = false
        CanaryMoodEngine.rollover(&m)
        XCTAssertEqual(m.trustDays, 0, "consecutive means consecutive")
        XCTAssertTrue(m.dayClean, "a new day starts clean")
    }

    func testMilestonesFireOnlyOnTheExactCrossing() {
        XCTAssertTrue(CanaryMoodEngine.trustMilestone(previousDays: 6, days: 7))
        XCTAssertFalse(CanaryMoodEngine.trustMilestone(previousDays: 7, days: 8))
        XCTAssertTrue(CanaryMoodEngine.trustMilestone(previousDays: 29, days: 30))
        XCTAssertFalse(CanaryMoodEngine.trustMilestone(previousDays: 30, days: 31))
    }

    // MARK: - the face ladder (same bands, same precedence)

    func testTheFaceLadderMatchesTheFirmware() {
        var i = CanaryMoodInputs()
        var m = CanaryMoodState()
        XCTAssertEqual(CanaryMoodEngine.face(m, i), .calm)
        m.anxiety = 4
        XCTAssertEqual(CanaryMoodEngine.face(m, i), .worried)
        m.anxiety = 9
        XCTAssertEqual(CanaryMoodEngine.face(m, i), .worried)
        m.anxiety = 10
        XCTAssertEqual(CanaryMoodEngine.face(m, i), .distressed)
        i.night = true
        XCTAssertEqual(CanaryMoodEngine.face(m, i), .asleep, "night outranks anxiety")
        i.alarmUnacked = true
        XCTAssertEqual(CanaryMoodEngine.face(m, i), .hidden,
                       "never cute during a real alarm — the rule above all")
    }

    func testPosturePicksTheStoryAndLostOutranksLate() {
        var i = CanaryMoodInputs()
        i.staleWitnesses = 1
        var m = CanaryMoodState(); m.anxiety = 4
        XCTAssertEqual(CanaryMoodEngine.posture(.worried, i), .searching)
        i.lostWitnesses = 1
        XCTAssertEqual(CanaryMoodEngine.posture(.worried, i), .calling)
        XCTAssertEqual(CanaryMoodEngine.posture(.calm, i), .asFace,
                       "posture refines only the worried band")
        _ = m
    }

    // MARK: - the keeper (persistence + minute guard)

    func testTheKeeperAdoptsWorseNewsImmediatelyButTicksOncePerMinute() throws {
        let suite = "test-mood-keeper-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let keeper = CanaryMoodKeeper(defaults: defaults)
        let t0 = Date(timeIntervalSince1970: 1_784_000_000)

        var quiet = CanaryMoodInputs()
        quiet.allVerified = true
        _ = keeper.observe(quiet, now: t0)

        // Twenty seconds later — inside the same minute — a witness is lost:
        var trouble = CanaryMoodInputs()
        trouble.lostWitnesses = 1
        let reading = keeper.observe(trouble, now: t0.addingTimeInterval(20))
        XCTAssertEqual(reading.state.anxiety, 4, "trouble never waits for the clock")
        XCTAssertEqual(reading.face, .worried)
        XCTAssertEqual(reading.posture, .calling)
    }

    func testTheKeeperRollsTheTrustLadderAcrossLocalDays() throws {
        let suite = "test-mood-rollover-\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let keeper = CanaryMoodKeeper(defaults: defaults)

        var quiet = CanaryMoodInputs()
        quiet.allVerified = true
        let day1 = Date(timeIntervalSince1970: 1_784_000_000)
        _ = keeper.observe(quiet, now: day1)
        let nextDay = keeper.observe(quiet, now: day1.addingTimeInterval(86_400 + 3_600))
        XCTAssertEqual(nextDay.state.trustDays, 1, "a clean day earned a trust day")
    }

    // MARK: - the wire (older phones, honest fallback)

    func testASnapshotWithoutMoodDerivesTheHonestFace() {
        var snap = WristSnapshot.sample()
        snap.faceRaw = nil
        snap.postureRaw = nil
        snap.severityRaw = Severity.notice.rawValue
        XCTAssertEqual(snap.face, .calm, "quiet old-phone fleets get the calm bird")
        snap.severityRaw = Severity.tamper.rawValue
        XCTAssertEqual(snap.face, .hidden, "alarming ones hand the stage to the instruments")
    }

    func testMoodFieldsRideTheSnapshotRoundTrip() throws {
        var snap = WristSnapshot.sample()
        snap.faceRaw = CanaryFace.worried.rawValue
        snap.postureRaw = CanaryPosture.searching.rawValue
        snap.anxiety = 5
        snap.trustDays = 12
        snap.moodLine = "Looking for Front Porch…"
        let context = try WristSync.context(for: snap)
        let decoded = try XCTUnwrap(WristSync.snapshot(fromContext: context))
        XCTAssertEqual(decoded.face, .worried)
        XCTAssertEqual(decoded.posture, .searching)
        XCTAssertEqual(decoded.moodLine, "Looking for Front Porch…")
    }
}
