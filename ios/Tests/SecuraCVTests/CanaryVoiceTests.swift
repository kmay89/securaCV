// CanaryVoiceTests.swift
//
// The bird helper's promises, pinned — the same properties the website's
// tests pin on js/canary-herald.js, so the two surfaces keep one
// temperament and a drifted port fails here, on the simulator. The load-
// bearing ones: the dwell curve's clamp order (an action's beat must
// survive the floor), a warning never waits behind small talk (and the cut
// small talk never comes back), overflow trims the back of the line and
// never the front (the front is where the warning is), quiet mutes only
// chatter, and three brush-offs make the quiet permanent.

import XCTest
@testable import SecuraCV

final class CanaryVoiceTests: XCTestCase {
    private func scratchDefaults() -> UserDefaults {
        let name = "test-canary-voice-\(UUID().uuidString)"
        let d = UserDefaults(suiteName: name)!
        addTeardownBlock { d.removePersistentDomain(forName: name) }
        return d
    }

    // MARK: - the dwell curve (read time, not a flat timer)

    func testDwellIsMonotonicInLength() {
        var last: TimeInterval = -1
        for n in stride(from: 0, through: 200, by: 10) {
            let d = CanaryVoiceEngine.dwell(for: String(repeating: "a", count: n),
                                            hasAction: false)
            XCTAssertGreaterThanOrEqual(d, last, "dwell must never dip as text grows")
            last = d
        }
    }

    func testDwellClampsToFloorAndCeiling() {
        XCTAssertEqual(CanaryVoiceEngine.dwell(for: "Hi.", hasAction: false),
                       CanaryVoicePace.floor,
                       "never so brief that looking away loses it")
        XCTAssertEqual(CanaryVoiceEngine.dwell(for: String(repeating: "a", count: 500),
                                               hasAction: false),
                       CanaryVoicePace.ceiling,
                       "never so long that it becomes furniture")
    }

    func testActionBeatSurvivesTheFloor() {
        // The site's clamp-order rule: "Saved." with a button to press must
        // get MORE time than "Saved." without one — clamping after adding
        // would let the floor swallow the bonus.
        let plain = CanaryVoiceEngine.dwell(for: "Saved.", hasAction: false)
        let tappable = CanaryVoiceEngine.dwell(for: "Saved.", hasAction: true)
        XCTAssertGreaterThan(tappable, plain)
        XCTAssertEqual(tappable, CanaryVoicePace.floor + CanaryVoicePace.action,
                       accuracy: 0.001)
    }

    func testActionBeatCannotPushPastTheCeiling() {
        // The other half of the clamp order: after the beat is added, one
        // final cap — a wall of text with a button still leaves the screen.
        XCTAssertEqual(CanaryVoiceEngine.dwell(for: String(repeating: "a", count: 300),
                                               hasAction: true),
                       CanaryVoicePace.ceiling)
    }

    func testNothingToSayDwellsZero() {
        XCTAssertEqual(CanaryVoiceEngine.dwell(for: "", hasAction: true), 0)
        XCTAssertEqual(CanaryVoiceEngine.dwell(for: "   \n  ", hasAction: false), 0)
    }

    // MARK: - compose (the site's normalizeMessage)

    func testComposeTrimsAndRefusesEmptyText() {
        XCTAssertNil(CanaryVoiceEngine.compose(""))
        XCTAssertNil(CanaryVoiceEngine.compose("   \n "))
        XCTAssertEqual(CanaryVoiceEngine.compose("  hello  ")?.text, "hello")
    }

    func testComposeCapsTextAndLabelLengths() {
        let long = String(repeating: "x", count: 400)
        let m = CanaryVoiceEngine.compose(long,
                                          action: CanaryVoiceAction(label: long, section: .fleet))
        XCTAssertEqual(m?.text.count, CanaryVoiceEngine.maxTextLength)
        XCTAssertEqual(m?.action?.label.count, CanaryVoiceEngine.maxLabelLength)
    }

    func testComposeDropsAnActionWithABlankLabel() {
        let m = CanaryVoiceEngine.compose("go",
                                          action: CanaryVoiceAction(label: "   ", section: .fleet))
        XCTAssertNotNil(m)
        XCTAssertNil(m?.action, "a blank label takes the whole action with it")
    }

    func testWarningsWaitToBeAcknowledged() {
        // ttl 0 — a warning you missed did no work.
        XCTAssertEqual(CanaryVoiceEngine.compose("careful", tone: .warn)?.ttl, 0)
        // Everything else is given exactly as long as it takes to read —
        // a line long enough to sit ABOVE the floor, so the equality proves
        // the wiring and not just the clamp.
        let line = "This sentence is deliberately long enough that its read time clears the floor."
        let chat = CanaryVoiceEngine.compose(line)
        XCTAssertEqual(chat?.ttl, CanaryVoiceEngine.dwell(for: line, hasAction: false))
        XCTAssertGreaterThan(chat?.ttl ?? 0, CanaryVoicePace.floor)
    }

    func testRunawayTTLIsClamped() {
        XCTAssertEqual(CanaryVoiceEngine.compose("x", ttl: 9e9)?.ttl,
                       CanaryVoiceEngine.maxTTL)
        XCTAssertEqual(CanaryVoiceEngine.compose("x", ttl: -5)?.ttl, 0)
        // Non-finite means "no ttl given", exactly as the origin's
        // Number.isFinite gate reads it — a warn still waits forever.
        XCTAssertEqual(CanaryVoiceEngine.compose("x", tone: .warn, ttl: .infinity)?.ttl, 0)
    }

    func testAnEmptyKeyIsNoKey() {
        // The origin nullifies "" keys so nothing ever dedupes against them.
        XCTAssertNil(CanaryVoiceEngine.compose("x", key: "")?.key)
        var q = CanaryVoiceQueue()
        _ = q.offer(msg("one", key: ""), quiet: false)
        XCTAssertEqual(q.offer(msg("two", key: ""), quiet: false), .queued)
    }

    // MARK: - the queue (ordering is a safety property)

    private func msg(_ text: String,
                     tone: CanaryVoiceTone = .chat,
                     key: String? = nil,
                     chatter: Bool = false) -> CanaryVoiceMessage {
        CanaryVoiceEngine.compose(text, tone: tone, key: key, chatter: chatter)!
    }

    func testSameKeyNeverQueuesTwice() {
        var q = CanaryVoiceQueue()
        XCTAssertEqual(q.offer(msg("offline", tone: .warn, key: "offline"), quiet: false), .queued)
        _ = q.advance()
        XCTAssertEqual(q.offer(msg("offline", tone: .warn, key: "offline"), quiet: false),
                       .refused, "a blip that fires three times still says it once")
        XCTAssertEqual(q.offer(msg("news", key: "news"), quiet: false), .queued)
        XCTAssertEqual(q.offer(msg("news again", key: "news"), quiet: false), .refused)
        XCTAssertEqual(q.offer(msg("no key"), quiet: false), .queued)
        XCTAssertEqual(q.offer(msg("no key"), quiet: false), .queued,
                       "keyless messages never dedupe against each other")
    }

    func testAWarningCutsSmallTalkAndTheSmallTalkNeverReturns() {
        var q = CanaryVoiceQueue()
        _ = q.offer(msg("did you know…", chatter: true), quiet: false)
        _ = q.advance()
        XCTAssertEqual(q.showing?.text, "did you know…")

        XCTAssertEqual(q.offer(msg("trouble", tone: .warn, key: "w"), quiet: false),
                       .interrupted)
        XCTAssertNil(q.showing, "the chatter's slot is taken outright")
        XCTAssertEqual(q.advance()?.text, "trouble", "the warning speaks next")
        q.hide()
        XCTAssertNil(q.advance(), "the cut chatter was dropped, never requeued")
    }

    func testAWarningWaitsAtTheFrontBehindARealMessage() {
        var q = CanaryVoiceQueue()
        _ = q.offer(msg("real notice", tone: .info), quiet: false)
        _ = q.advance()
        _ = q.offer(msg("small talk", chatter: true), quiet: false)
        XCTAssertEqual(q.offer(msg("trouble", tone: .warn), quiet: false), .queued)
        XCTAssertEqual(q.showing?.text, "real notice",
                       "a warning preempts only chatter, never a real message")
        XCTAssertEqual(q.waiting.first?.text, "trouble", "but it waits at the FRONT")
    }

    func testOverflowTrimsTheBackNeverTheFront() {
        var q = CanaryVoiceQueue()
        for i in 1...4 { _ = q.offer(msg("chat \(i)"), quiet: false) }
        _ = q.offer(msg("trouble", tone: .warn), quiet: false)   // front, makes five
        XCTAssertEqual(q.waiting.count, CanaryVoiceEngine.maxQueue)
        XCTAssertEqual(q.waiting.first?.text, "trouble",
                       "the just-promoted warning survives the trim")
        XCTAssertEqual(q.waiting.last?.text, "chat 3", "the back of the line paid for it")
    }

    func testTheInterruptPathDoesNotPayTheTrim() {
        // Origin parity: the warn-cuts-chatter path returns before the
        // MAX_QUEUE trim, so a full line keeps every waiting message plus
        // the promoted warning — the cut chatter already paid for its seat.
        var q = CanaryVoiceQueue()
        _ = q.offer(msg("small talk", chatter: true), quiet: false)
        _ = q.advance()
        for i in 1...4 { _ = q.offer(msg("note \(i)", tone: .info), quiet: false) }
        XCTAssertEqual(q.offer(msg("trouble", tone: .warn), quiet: false), .interrupted)
        XCTAssertEqual(q.waiting.count, CanaryVoiceEngine.maxQueue + 1)
        XCTAssertEqual(q.waiting.first?.text, "trouble")
    }

    func testQuietMutesOnlyChatter() {
        var q = CanaryVoiceQueue()
        XCTAssertEqual(q.offer(msg("small talk", chatter: true), quiet: true), .refused)
        XCTAssertEqual(q.offer(msg("trouble", tone: .warn), quiet: true), .queued,
                       "real notices always get through")
    }

    // MARK: - the keeper (what the bird remembers)

    func testHelloIsOnceEver() {
        let keeper = CanaryVoiceKeeper(defaults: scratchDefaults())
        XCTAssertFalse(keeper.hasGreeted)
        keeper.markGreeted()
        XCTAssertTrue(keeper.hasGreeted)
    }

    func testEachSectionOrientsOnceAndMembershipIsExact() {
        let keeper = CanaryVoiceKeeper(defaults: scratchDefaults())
        XCTAssertFalse(keeper.hasOriented(.fleet))
        keeper.markOriented(.fleet)
        XCTAssertTrue(keeper.hasOriented(.fleet))
        XCTAssertFalse(keeper.hasOriented(.alerts),
                       "one section's ledger entry must not vouch for another")
        keeper.markOriented(.fleet)
        keeper.markOriented(.alerts)
        XCTAssertTrue(keeper.hasOriented(.alerts))
    }

    func testThreeBrushOffsAndTheChatterStops() {
        let keeper = CanaryVoiceKeeper(defaults: scratchDefaults())
        keeper.noteChatterDismissed()
        keeper.noteChatterDismissed()
        XCTAssertFalse(keeper.isQuiet, "two is a coincidence")
        keeper.noteChatterDismissed()
        XCTAssertTrue(keeper.isQuiet, "three is a preference")
    }

    // MARK: - the stage (synchronous paths only; the clocks stay untested,
    // like the origin's DOM layer)

    @MainActor
    func testSayPresentsImmediatelyWhenTheStageIsFree() {
        let stage = CanaryVoiceStage(keeper: CanaryVoiceKeeper(defaults: scratchDefaults()))
        XCTAssertTrue(stage.say("hello there", tone: .info))
        XCTAssertEqual(stage.showing?.text, "hello there")
        stage.dismiss()
        XCTAssertNil(stage.showing)
    }

    @MainActor
    func testTheChannelHoldsItsTongueDuringAnAlarm() {
        let stage = CanaryVoiceStage(keeper: CanaryVoiceKeeper(defaults: scratchDefaults()))
        stage.stagePermitted = { false }             // the instruments own the stage
        XCTAssertTrue(stage.say("waits", tone: .info, key: "w"),
                      "the message is accepted — it waits, it doesn't vanish")
        XCTAssertNil(stage.showing)
        stage.stagePermitted = { true }
        stage.stageChanged()                          // the alarm ended
        XCTAssertEqual(stage.showing?.text, "waits")
    }

    @MainActor
    func testChatterWaitsForACalmBird() {
        let stage = CanaryVoiceStage(keeper: CanaryVoiceKeeper(defaults: scratchDefaults()))
        stage.chatterPermitted = { false }            // worried, or asleep
        XCTAssertFalse(stage.say("small talk", chatter: true))
        XCTAssertTrue(stage.say("real notice", tone: .info),
                      "only volunteered small talk defers to the mood")
    }

    @MainActor
    func testDismissingChatterThreeTimesQuietsTheBird() {
        let defaults = scratchDefaults()
        let stage = CanaryVoiceStage(keeper: CanaryVoiceKeeper(defaults: defaults))
        for i in 1...3 {
            XCTAssertTrue(stage.say("nudge \(i)", chatter: true))
            stage.dismiss()
        }
        XCTAssertFalse(stage.say("nudge 4", chatter: true),
                       "three brush-offs and the bird stops volunteering")
        XCTAssertTrue(stage.say("real notice", tone: .warn, key: "still-heard"),
                      "quiet never mutes a real notice")
    }

    @MainActor
    func testActionsNeverCountTowardQuiet() {
        let defaults = scratchDefaults()
        let stage = CanaryVoiceStage(keeper: CanaryVoiceKeeper(defaults: defaults))
        for i in 1...3 {
            XCTAssertTrue(stage.say("note \(i)", chatter: true))
            stage.actionTaken()                       // a tap is engagement, not a brush-off
        }
        XCTAssertTrue(stage.say("still chatty", chatter: true))
    }

    @MainActor
    func testAnAlarmShelvesTheSentenceAndItResumesAfter() {
        let stage = CanaryVoiceStage(keeper: CanaryVoiceKeeper(defaults: scratchDefaults()))
        XCTAssertTrue(stage.say("a real notice", tone: .info, key: "n"))
        XCTAssertEqual(stage.showing?.text, "a real notice")

        var permitted = true
        stage.stagePermitted = { permitted }
        permitted = false
        stage.stageSeized()                           // the instruments own the stage
        XCTAssertNil(stage.showing)

        permitted = true
        stage.stageChanged()                          // the alarm ended
        XCTAssertEqual(stage.showing?.text, "a real notice",
                       "a shelved notice resumes with its full dwell")
    }

    @MainActor
    func testAnAlarmDropsShowingChatterOutright() {
        let stage = CanaryVoiceStage(keeper: CanaryVoiceKeeper(defaults: scratchDefaults()))
        XCTAssertTrue(stage.say("small talk", chatter: true))
        stage.stageSeized()
        XCTAssertNil(stage.showing)
        stage.stageChanged()
        XCTAssertNil(stage.showing,
                     "chatter was never important enough to come back for")
    }

    @MainActor
    func testAWarningTakesTheStageFromChatterAfterTheGap() async throws {
        let stage = CanaryVoiceStage(keeper: CanaryVoiceKeeper(defaults: scratchDefaults()))
        XCTAssertTrue(stage.say("small talk", chatter: true))
        XCTAssertTrue(stage.say("trouble", tone: .warn, key: "t"))
        XCTAssertNil(stage.showing, "the chatter's slot is taken outright")
        try await Task.sleep(for: .seconds(1))        // > PACE.gap, with margin
        XCTAssertEqual(stage.showing?.text, "trouble")
        XCTAssertEqual(stage.showing?.ttl, 0, "and it waits to be acknowledged")
    }

    @MainActor
    func testOnceEverLinesBurnOnDeliveryNotOnScheduling() async throws {
        let defaults = scratchDefaults()
        let keeper = CanaryVoiceKeeper(defaults: defaults)
        let stage = CanaryVoiceStage(keeper: keeper)
        stage.chatterPermitted = { false }            // a worried first launch
        stage.arrive(at: .today)
        try await Task.sleep(for: .seconds(2.5))      // > PACE.hello, with margin
        XCTAssertNil(stage.showing)
        XCTAssertFalse(keeper.hasGreeted,
                       "a hello refused by the mood must get another chance")

        stage.chatterPermitted = { true }             // a calmer next launch
        stage.arrive(at: .today)
        try await Task.sleep(for: .seconds(2.5))
        XCTAssertEqual(stage.showing?.key, "hello")
        XCTAssertTrue(keeper.hasGreeted)
    }

    // MARK: - what the bird says on its own

    @MainActor
    func testGreetingPointsAtTheFleetExceptFromTheFleet() {
        let hello = CanaryVoiceStage.greeting(arrivingAt: .today)
        XCTAssertEqual(hello?.key, "hello")
        XCTAssertEqual(hello?.tone, .chat)
        XCTAssertTrue(hello?.chatter == true, "the introduction is volunteered")
        XCTAssertEqual(hello?.action?.section, .fleet)
        XCTAssertNil(CanaryVoiceStage.greeting(arrivingAt: .fleet)?.action,
                     "no pointer to the place you're already standing")
    }

    @MainActor
    func testTipsExistOnlyWhereANewcomerMightStall() {
        XCTAssertNil(CanaryVoiceStage.tip(for: .today), "Today explains itself")
        for section in [AppSection.fleet, .alerts, .keys] {
            let tip = CanaryVoiceStage.tip(for: section)
            XCTAssertNotNil(tip)
            XCTAssertEqual(tip?.tone, .info)
            XCTAssertEqual(tip?.key, "tip-\(section.rawValue)")
            XCTAssertTrue(tip?.chatter == true, "a tip is volunteered, so quiet mutes it")
        }
    }

    // MARK: - the bubble's tone accents

    @MainActor
    func testTonesWearTheirAccentsAndNeverAlarmRed() {
        XCTAssertNil(CanaryVoiceBubble.accent(for: .chat), "chat stays unmarked")
        XCTAssertEqual(CanaryVoiceBubble.accent(for: .info), .info)
        XCTAssertEqual(CanaryVoiceBubble.accent(for: .good), .calm)
        XCTAssertEqual(CanaryVoiceBubble.accent(for: .warn), .warn)
        for tone in [CanaryVoiceTone.chat, .info, .good, .warn] {
            XCTAssertNotEqual(CanaryVoiceBubble.accent(for: tone), .alert,
                              "pure red is for a real alarm, which never speaks here")
        }
    }
}
