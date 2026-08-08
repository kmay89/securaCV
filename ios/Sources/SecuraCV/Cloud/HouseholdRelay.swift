// HouseholdRelay.swift
//
// "If nobody answers, tell someone else." The last leg of the escalation
// ladder (docs/design/alerts_event_history.md §6, iphone_companion_app.md §5b
// rule 5), and the first thing in this app that reaches a person who is not
// the owner.
//
// THE SHAPE, AND WHY IT IS A ZONE
//   Household members are invited to ONE CloudKit record zone, and escalation
//   wakes are the only thing ever written into it. That is not a filter — it
//   is the boundary itself. A participant's device can read what it was
//   invited to and nothing else, so "a household member cannot see your fleet,
//   your Canary names, your history, or your ordinary alerts" is a fact about
//   CloudKit's access control rather than a promise about our code. Ordinary
//   wakes keep going to the owner's default zone, which nobody is invited to.
//
//   The practical test for anyone extending this: if you find yourself adding
//   a field to the household record, stop. The zone's whole value is that
//   there is nothing in it worth reading.
//
// WHAT A PARTICIPANT ACTUALLY RECEIVES
//   A push whose words their OWN device wrote. Shared-database subscriptions
//   carry no per-record fields, so the sentence comes from the subscription
//   the participant's app created locally (`participantAlertBody`), not from
//   anything the owner sent. The owner's household record carries a coarse
//   class and nothing else, and the participant only sees even that after
//   opening the app.
//
// THE RATIONING, WHICH IS THE POINT
//   Only an escalation reaches the household — a top-tier alarm that has gone
//   unanswered past `EscalationPolicy.delay`. Never an ordinary alert, never a
//   lower tier, and never a second time for the same occurrence (the ledger's
//   escalation stamp already guarantees at most one). A second person's phone
//   is only worth carrying if it stays quiet; the moment household alerts
//   become routine, they are the noise this whole design has been removing.

import Foundation
import CryptoKit

/// One person the owner invited. A projection of CloudKit's participant list,
/// kept as a plain value so the roster's arithmetic and copy are testable
/// without a container.
struct HouseholdMember: Identifiable, Hashable, Sendable {
    enum Status: String, Sendable {
        case owner        // the person whose fleet this is
        case accepted     // invited, and their device joined
        case invited      // invited, hasn't accepted yet
    }

    /// CloudKit's participant identifier, stringified. Stable enough to
    /// address a row; meaningless anywhere else.
    let id: String
    /// What CloudKit says to call them. Their name, from their own Apple
    /// account — it never leaves this device (Invariant IV).
    let name: String
    let status: Status

    var isOwner: Bool { status == .owner }
}

enum HouseholdRelay {
    /// The zone that IS the privacy boundary. One name, used by the owner who
    /// creates it and the sweep that keeps it tidy.
    static let zoneName = "HouseholdEscalations"
    /// The only record type that may live in that zone.
    static let escalationRecordType = "EscalationWake"
    /// The participant's subscription on their SHARED database.
    static let subscriptionID = "securacv-household-escalation-v1"

    /// "The owner answered this one." Written to the owner's OWN private
    /// default zone — deliberately NOT the shared zone, which participants can
    /// read and which is allowed to hold exactly one kind of record.
    ///
    /// It exists because acknowledging is device-local. The owner acks on their
    /// iPhone; their iPad's escalation timer expires still believing nobody
    /// answered, and tells a household member "Nobody answered" about an alarm
    /// that WAS answered. The deterministic occurrence name dedupes two
    /// *writes*, but it cannot represent an *answer* — so the answer needs a
    /// record of its own, keyed by the same occurrence so any of the owner's
    /// devices can ask "has this one been dealt with?" before waking anybody.
    static let answeredRecordType = "AlertAnswered"

    /// May this reach a second person? Deliberately a separate gate from the
    /// one `escalateIfUnanswered` already passed, asked in its own file with
    /// its own test: reaching someone who is not the owner is the highest-cost
    /// thing this app can do with an alert, and it should take two independent
    /// yeses.
    ///
    /// `escalated` is the caller's statement that the alarm has already gone
    /// unanswered past the window; the severity check here makes sure nothing
    /// below the top tier can ever be smuggled in behind that flag.
    static func mayReachHousehold(severity: Severity,
                                  integrityFailed: Bool,
                                  escalated: Bool) -> Bool {
        guard escalated else { return false }
        return EscalationPolicy.isTopTier(severity: severity, integrityFailed: integrityFailed)
    }

    /// The record name for ONE occurrence, computed identically by every
    /// device the owner holds.
    ///
    /// The "at most once" stamp lives in each device's own alert ledger, so an
    /// iPhone and an iPad watching the same alarm each believe they are the
    /// first to escalate. With a random record name CloudKit accepts both
    /// writes and every participant's phone buzzes twice — the promise held
    /// locally and broke on somebody else's nightstand. A deterministic name
    /// makes the second write collide with the first and fail, which is
    /// exactly the outcome wanted: no new record, so no second push.
    ///
    /// Keyed by the occurrence (the ledger's collapse id plus the bucket the
    /// alarm landed in), never by the escalation's own clock — two devices
    /// deciding to escalate a few seconds apart must still name the same
    /// record. A condition that returns months later lands in a different
    /// bucket and is honestly a different occurrence.
    static func occurrenceRecordName(recordID: String, alarmBucket: Date) -> String {
        let seed = "\(recordID)|\(Int(alarmBucket.timeIntervalSince1970))"
        let digest = SHA256.hash(data: Data(seed.utf8))
        // Hashed rather than sanitized: a record name has a restricted
        // character set and the ledger's id carries device names and status
        // lines, which have no business in a CloudKit key that a participant
        // could read.
        return "esc-" + digest.map { String(format: "%02x", $0) }.joined().prefix(32)
    }

    /// The "answered" marker's name for the same occurrence. Same hash, a
    /// different prefix — so the two records can never collide, and reading
    /// one never depends on having written the other. Derived rather than
    /// stored for the same reason the escalation name is: every device the
    /// owner holds computes it identically, with nothing to sync.
    static func answeredRecordName(for occurrenceKey: String) -> String {
        "ans-" + occurrenceKey.dropFirst("esc-".count)
    }

    // MARK: - the words a participant's own device writes

    /// Title and body for the participant's locally-created subscription.
    /// Content-free on purpose, and it says the one true thing an escalation
    /// means: somebody's alarm went unanswered, and they are being asked to
    /// look. No fleet name (that is the owner's), no Canary, no class.
    static let participantAlertTitle = "Nobody answered"
    static let participantAlertBody = "An alarm on a fleet you help watch hasn't been answered."

    // MARK: - the roster's arithmetic and copy

    /// Owner first, then people who joined, then invitations still out —
    /// which is also the order of how much each row means.
    static func sorted(_ members: [HouseholdMember]) -> [HouseholdMember] {
        let rank: (HouseholdMember) -> Int = { member in
            switch member.status {
            case .owner: return 0
            case .accepted: return 1
            case .invited: return 2
            }
        }
        return members.sorted {
            rank($0) == rank($1) ? $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
                                 : rank($0) < rank($1)
        }
    }

    /// How many people have JOINED — the owner is not one of them (their own
    /// devices are already told directly), and an invitation nobody accepted
    /// reaches nobody.
    ///
    /// Note the word. This used to be called `reachableCount`, and the roster
    /// said "N people are told", which is a claim this app cannot support: a
    /// participant who joined and then denied or disabled notifications still
    /// reads as `.accepted` in CloudKit, so the owner was told somebody would
    /// be woken in exactly the case the code already knew they wouldn't be
    /// (`HouseholdShare.participantBlocked` — which lives on the participant's
    /// device, where the owner cannot see it). Joining is what the owner's
    /// device can actually observe, so joining is what it now says.
    static func joinedCount(_ members: [HouseholdMember]) -> Int {
        members.filter { $0.status == .accepted }.count
    }

    static func pendingCount(_ members: [HouseholdMember]) -> Int {
        members.filter { $0.status == .invited }.count
    }

    /// The one line the Alerts screen shows. Never overstates, which now means
    /// two different things it did not before:
    ///
    ///   * an invitation nobody accepted is named as such — "2 people can be
    ///     reached" when one never tapped the link is the comfortable lie this
    ///     app exists not to tell;
    ///   * and somebody who HAS joined is described as having joined, not as
    ///     being "told". Whether their phone will actually alert them is their
    ///     notification setting, which lives on their device and is not
    ///     something the owner's device can see. Saying "told" claimed
    ///     knowledge we don't have, in precisely the case where it fails.
    ///
    /// `notedUnknownReach` carries that second point once, rather than
    /// repeating a hedge in every branch.
    static func summary(_ members: [HouseholdMember]) -> String {
        let joined = joinedCount(members)
        let pending = pendingCount(members)
        switch (joined, pending) {
        case (0, 0):
            return "Nobody else is set up. If an alarm goes unanswered, only your own devices will know."
        case (0, let waiting):
            return waiting == 1
                ? "1 person is invited but hasn't joined yet, so nobody else can be reached."
                : "\(waiting) people are invited but haven't joined yet, so nobody else can be reached."
        case (let count, 0):
            return count == 1
                ? "1 person has joined and will be sent an alarm nobody answered."
                : "\(count) people have joined and will be sent an alarm nobody answered."
        case (let count, let waiting):
            let people = count == 1
                ? "1 person has joined"
                : "\(count) people have joined"
            let rest = waiting == 1 ? "1 more hasn't joined yet." : "\(waiting) more haven't joined yet."
            return "\(people) and will be sent an alarm nobody answered. \(rest)"
        }
    }

    /// The limit of what the owner's device can know about the other end,
    /// said once on the household screen instead of hedged into every count.
    /// A participant can join and then turn notifications off; their CloudKit
    /// acceptance still reads `.accepted`, and nothing tells the owner.
    static let reachIsTheirsToKnow = """
        Whether their phone actually alerts them is their own notification \
        setting — SecuraCV can't see it from here. Ask them to check if it \
        matters.
        """

    /// The condition the whole ladder rests on, stated where an owner is
    /// deciding to rely on it. Escalation is decided by a device that is
    /// awake and watching the fleet — this iPhone in the foreground, or a
    /// resident that stayed home (the Apple TV's "stand watch"). A phone
    /// locked in a pocket with nothing else home cannot notice that nobody
    /// answered, so it cannot tell anyone.
    ///
    /// This is the same limit the away path already states ("with nothing
    /// home, away alerts cannot happen"), and it is stated here rather than
    /// discovered, because the household leg exists precisely for the moment
    /// the owner is not looking at their phone.
    static let watchRequirement = """
        Someone has to be awake to notice that nobody answered: this iPhone \
        with SecuraCV open, or an Apple TV standing watch at home. A locked \
        phone with nothing else home can't tell anyone.
        """

    /// Why a household alert wouldn't reach THIS device — the participant's
    /// side of the same honesty. Notifications being off is the whole failure,
    /// and it is invisible unless somebody says it.
    static let participantNeedsNotifications = """
        Turn on notifications for SecuraCV, or you won't be told when an alarm \
        goes unanswered.
        """

    /// What the household may be told, stated for the invite screen. Written
    /// as a promise the invited person can check rather than a reassurance —
    /// they are about to accept something, and they deserve to know exactly
    /// how small it is.
    static let invitationExplanation = """
        They'll be told only when an alarm here goes unanswered — never your \
        everyday alerts. They can't see your Canaries, your names, your \
        history or any footage: the invitation reaches one folder in your \
        iCloud that holds nothing but "an alarm wasn't answered". You can \
        remove anyone at any time.
        """
}
