// HouseholdRelayTests.swift
//
// Reaching somebody who is not the owner is the highest-cost thing this app
// can do with an alert, so the rules around it get held hard:
//
//   * ONLY an escalation. Not an ordinary alert, not a lower tier, not
//     anything that merely claims to be escalated.
//   * The roster NEVER counts an invitation as a person. "2 people are told"
//     while one of them never tapped the link is the comfortable lie this
//     whole app is built not to tell.
//   * The household's zone holds one record type and nothing else — the
//     privacy boundary is CloudKit's access control, not our filtering, and
//     that only stays true while nothing else is written there.

import XCTest
@testable import SecuraCV

final class HouseholdRelayTests: XCTestCase {

    // MARK: - the rationing

    func testOnlyAnEscalationEverReachesAnotherPerson() {
        XCTAssertFalse(HouseholdRelay.mayReachHousehold(severity: .tamper,
                                                        integrityFailed: false,
                                                        escalated: false),
                       "a live tamper alarm is the OWNER's to answer first — the household is the backstop")
    }

    func testOnlyTheTopTierReachesAnotherPerson() {
        for severity in [Severity.ok, .notice, .warn, .alert] {
            XCTAssertFalse(HouseholdRelay.mayReachHousehold(severity: severity,
                                                            integrityFailed: false,
                                                            escalated: true),
                           "\(severity) must never wake somebody else's phone, escalated or not")
        }
        XCTAssertTrue(HouseholdRelay.mayReachHousehold(severity: .tamper,
                                                       integrityFailed: false,
                                                       escalated: true))
    }

    func testAFailedSignatureReachesTheHouseholdLikeTamperDoes() {
        XCTAssertTrue(HouseholdRelay.mayReachHousehold(severity: .alert,
                                                       integrityFailed: true,
                                                       escalated: true),
                      "someone interfering with the witness is the same class of news as tamper")
    }

    func testTheGateAgreesWithTheEscalationPolicyItBacksOnto() {
        // Two files, one ladder: if EscalationPolicy ever widens what counts
        // as top tier, this must widen with it rather than drift.
        for (severity, integrity) in [(Severity.tamper, false), (.alert, true), (.alert, false), (.warn, false)] {
            XCTAssertEqual(
                HouseholdRelay.mayReachHousehold(severity: severity,
                                                 integrityFailed: integrity,
                                                 escalated: true),
                EscalationPolicy.isTopTier(severity: severity, integrityFailed: integrity))
        }
    }

    // MARK: - the roster never overstates

    private func member(_ name: String, _ status: HouseholdMember.Status) -> HouseholdMember {
        HouseholdMember(id: name, name: name, status: status)
    }

    func testAnInvitationIsNotAPerson() {
        let roster = [member("You", .owner), member("Sam", .invited)]
        XCTAssertEqual(HouseholdRelay.reachableCount(roster), 0,
                       "somebody who never tapped the link reaches nobody")
        XCTAssertEqual(HouseholdRelay.summary(roster),
                       "1 person is invited but hasn't joined yet, so nobody else can be reached.")
    }

    func testTheOwnerIsNotSomebodyElse() {
        let roster = [member("You", .owner)]
        XCTAssertEqual(HouseholdRelay.reachableCount(roster), 0,
                       "your own devices are already told directly; you are not your own backstop")
        XCTAssertEqual(HouseholdRelay.summary(roster),
                       "Nobody else is set up. If an alarm goes unanswered, only your own devices will know.")
    }

    func testItCountsOnlyThePeopleWhoJoined() {
        let roster = [member("You", .owner), member("Sam", .accepted), member("Alex", .accepted)]
        XCTAssertEqual(HouseholdRelay.reachableCount(roster), 2)
        XCTAssertEqual(HouseholdRelay.summary(roster), "2 people are told if an alarm goes unanswered.")
    }

    func testAMixedRosterNamesBothHalvesHonestly() {
        let roster = [member("You", .owner), member("Sam", .accepted), member("Alex", .invited)]
        XCTAssertEqual(HouseholdRelay.summary(roster),
                       "1 person is told if an alarm goes unanswered. 1 more hasn't joined yet.")
    }

    func testTheRosterReadsOwnerThenJoinedThenInvited() {
        let roster = [member("Zoe", .invited), member("Alex", .accepted), member("You", .owner),
                      member("Sam", .accepted)]
        XCTAssertEqual(HouseholdRelay.sorted(roster).map(\.name), ["You", "Alex", "Sam", "Zoe"],
                       "owner first, then who can actually be reached, then who can't yet")
    }

    // MARK: - one alarm, one buzz, however many devices the owner holds

    func testEveryOwnerDeviceNamesTheSameEscalationRecord() {
        let bucket = Date(timeIntervalSince1970: 1_800_000_000)
        let iphone = HouseholdRelay.occurrenceRecordName(recordID: "canary-a3f7|4|Tamper detected",
                                                         alarmBucket: bucket)
        let ipad = HouseholdRelay.occurrenceRecordName(recordID: "canary-a3f7|4|Tamper detected",
                                                       alarmBucket: bucket)
        XCTAssertEqual(iphone, ipad,
                       "the ‘once per occurrence’ stamp is per-device; without a shared name the "
                       + "owner's iPad buzzes every household phone a second time")
    }

    func testADifferentOccurrenceIsADifferentRecord() {
        let first = HouseholdRelay.occurrenceRecordName(
            recordID: "canary-a3f7|4|Tamper detected",
            alarmBucket: Date(timeIntervalSince1970: 1_800_000_000))
        let monthsLater = HouseholdRelay.occurrenceRecordName(
            recordID: "canary-a3f7|4|Tamper detected",
            alarmBucket: Date(timeIntervalSince1970: 1_805_000_000))
        let otherCanary = HouseholdRelay.occurrenceRecordName(
            recordID: "canary-b2c9|4|Tamper detected",
            alarmBucket: Date(timeIntervalSince1970: 1_800_000_000))
        XCTAssertNotEqual(first, monthsLater, "it came BACK — that is news again, not a duplicate")
        XCTAssertNotEqual(first, otherCanary)
    }

    func testTheRecordNameCarriesNothingReadable() {
        let name = HouseholdRelay.occurrenceRecordName(recordID: "canary-a3f7|4|Tamper detected",
                                                       alarmBucket: Date())
        // A participant can read record names in the zone they were invited
        // to. The ledger id carries a device name and a status line, so it is
        // hashed rather than sanitized.
        XCTAssertFalse(name.localizedCaseInsensitiveContains("Tamper"), name)
        XCTAssertFalse(name.localizedCaseInsensitiveContains("canary-a3f7"), name)
        XCTAssertTrue(name.hasPrefix("esc-"), name)
        XCTAssertLessThanOrEqual(name.count, 40, "CloudKit record names are not a place for essays")
    }

    // MARK: - the limits, said rather than discovered

    func testTheWatchRequirementIsStatedInPlainWords() {
        // The household leg exists for the moment the owner is not looking at
        // their phone — which is also the moment their phone can't notice
        // that nobody answered. That limit has to be on the screen.
        let text = HouseholdRelay.watchRequirement
        XCTAssertTrue(text.localizedCaseInsensitiveContains("Apple TV"), text)
        XCTAssertTrue(text.localizedCaseInsensitiveContains("locked phone"), text)
    }

    func testTheParticipantIsToldWhenNotificationsWouldStopThem() {
        XCTAssertTrue(HouseholdRelay.participantNeedsNotifications
            .localizedCaseInsensitiveContains("notifications"),
                      "being on the share is not the same as being reachable, and only their own "
                      + "device can know the difference")
    }

    // MARK: - the boundary is the zone

    func testTheHouseholdZoneIsNotWhereOrdinaryWakesGo() {
        // The ordinary wake lives in the owner's default zone, which nobody
        // is invited to; the household zone is a different place with a
        // different record type. If these ever collide, a household member
        // starts receiving every alert the owner gets.
        XCTAssertNotEqual(HouseholdRelay.escalationRecordType, AwayPush.wakeRecordType)
        XCTAssertNotEqual(HouseholdRelay.subscriptionID, AwayPush.subscriptionID)
        XCTAssertFalse(HouseholdRelay.zoneName.isEmpty)
    }

    func testTheParticipantsSentenceCarriesNothingAboutTheFleet() {
        // The words are written by the RECEIVING device, so they cannot
        // contain anything the owner sent — but they also must not invite
        // anyone to guess. No fleet name, no Canary, no class, no place.
        let line = HouseholdRelay.participantAlertTitle + " " + HouseholdRelay.participantAlertBody
        for leak in ["Canary", "Front", "Porch", "tamper", "camera"] {
            XCTAssertFalse(line.localizedCaseInsensitiveContains(leak),
                           "the household's push must name nothing: found “\(leak)” in “\(line)”")
        }
    }

    func testTheInvitationSaysWhatTheyCannotSee() {
        // Half a sentence ("they'll be told when an alarm goes unanswered")
        // is what every sharing feature says. The half that earns the tap is
        // the limit, so the copy is asserted to carry it.
        let text = HouseholdRelay.invitationExplanation
        XCTAssertTrue(text.localizedCaseInsensitiveContains("can't see"), text)
        XCTAssertTrue(text.localizedCaseInsensitiveContains("remove anyone"), text)
    }
}
