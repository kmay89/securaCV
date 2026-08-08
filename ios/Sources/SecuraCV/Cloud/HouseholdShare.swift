// HouseholdShare.swift
//
// The CloudKit half of "if nobody answers, tell someone else". The policy and
// the copy live next door in HouseholdRelay (pure, tested); this file is the
// plumbing, and it is deliberately the only place that knows a CKShare exists.
//
// TWO ROLES, ONE OBJECT. A phone can be both at once — your own fleet, and
// your parents' fleet that you help watch:
//
//   * OWNER. Creates one custom zone, shares the ZONE (not a record), and
//     writes an escalation wake into it when an alarm of theirs goes
//     unanswered. Invitations go out through Apple's own sharing sheet, so
//     the link, the accept flow and the revoke are Apple's — we neither see
//     nor store who was invited.
//   * PARTICIPANT. Accepts the share and subscribes to their SHARED database.
//     The sentence they see is written by their own device when it creates
//     that subscription (shared-database subscriptions carry no record
//     fields), so even the wording of a household alert never crosses the
//     wire.
//
// WHY A ZONE SHARE RATHER THAN A RECORD SHARE: the zone is the access
// boundary, and this zone contains nothing but escalations. "A household
// member can't see your fleet" is then enforced by CloudKit rather than by
// our filtering — see HouseholdRelay's header for the rule that keeps it
// true (nothing else may ever be written here).
//
// Everything CloudKit is behind the same compile-time gate as the rest of the
// cloud code: an unsigned build carries no entitlements, and constructing a
// container in that process TRAPS (CloudContainer.swift explains at length).
// So this whole file degrades to a no-op that reports honestly, and CI — which
// builds exactly that — compiles it without ever touching iCloud.

import Foundation
#if canImport(CloudKit)
import CloudKit
#endif
#if canImport(UIKit)
import UIKit
#endif

/// Whether a second person could actually be reached right now, said in the
/// same shape as `AwayReach` so the UI treats both claims identically.
enum HouseholdState: Equatable {
    /// No zone, no invitations — the default, and a perfectly fine place to stay.
    case notSetUp
    /// Sharing exists; `members` says who is really reachable.
    case sharing
    /// Shown verbatim; write it as a sentence the user can act on.
    case unavailable(String)

    var isSharing: Bool { self == .sharing }
}

@MainActor
final class HouseholdShare: ObservableObject {
    static let shared = HouseholdShare()

    @Published private(set) var state: HouseholdState = .notSetUp
    /// Everyone on the owner's share, including the owner. Empty until a
    /// fetch says otherwise — never optimistic.
    @Published private(set) var members: [HouseholdMember] = []
    /// True once this device has accepted somebody else's invitation. A
    /// SEPARATE axis from `state` on purpose: one phone can be an owner who
    /// shares and a participant who helps at the same time (your fleet, and
    /// your parents' fleet), and a single enum trying to hold both would end
    /// up lying about one of them.
    @Published private(set) var isHelpingSomeone = false

    /// How many people have joined, from the roster this device last fetched.
    ///
    /// Read by the UI, and NOT by the publish path. `publishEscalation` used to
    /// gate on this cached number, which turned a "don't write into an empty
    /// zone" optimization into a way to silently drop an escalation: an invitee
    /// who accepted after the owner's last refresh still reads as invited here,
    /// and there is no participant-change subscription to correct it. An
    /// always-on iPad that hasn't opened the household sheet since before the
    /// invitation was accepted would simply never tell anyone. The publish path
    /// now re-reads the share itself and fails open.
    var joinedCount: Int { HouseholdRelay.joinedCount(members) }

    /// Why a household alert wouldn't reach THIS device, if something would
    /// stop it. nil when the path is clear.
    ///
    /// The case this exists for: somebody installs SecuraCV *only* to help
    /// watch a relative's fleet. They pair nothing, so the app's ordinary
    /// "you have devices, so let's ask about notifications" moment never
    /// arrives — and registering for remote notifications is not the same as
    /// being allowed to show one. Without this, their subscription saves,
    /// every household alert is silently suppressed, and the owner's roster
    /// cheerfully says "1 person is told."
    @Published private(set) var participantBlocked: String?

    /// Have we ever actually asked iCloud whether a share exists?
    ///
    /// `state` cannot answer this: it starts at `.notSetUp`, which is
    /// indistinguishable from "asked, and there is no share". The difference
    /// matters exactly once — an Ack tapped from a notification during a cold
    /// launch, before the launch-time refresh has returned, which is the
    /// ordinary way people answer an alarm. Without this, `noteAnswered` would
    /// read `.notSetUp`, decide there is no household to protect, and drop the
    /// marker in the commonest case it exists for.
    private var hasResolvedShareState = false

    /// Set by FleetStore so the participant path can ask for notification
    /// permission at the moment of need — the moment they accept — using the
    /// app's one authorization request rather than a second copy of the
    /// options list.
    var requestNotificationAuthorization: (() async -> Bool)?

    private init() {}

    #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
    private var zoneID: CKRecordZone.ID {
        CKRecordZone.ID(zoneName: HouseholdRelay.zoneName, ownerName: CKCurrentUserDefaultName)
    }
    /// Cached between the sheet opening and the invite being sent.
    private var share: CKShare?

    /// The saved share, for Apple's own sharing sheet. Nil until
    /// `prepareShare()` has succeeded — the sheet must never be handed a
    /// share that isn't on the server yet.
    var activeShare: CKShare? { share }
    #endif

    // MARK: - owner: set up, invite, refresh, stop

    /// Make sure the zone and its share exist, and hand back the share for
    /// Apple's sharing sheet. Idempotent: called every time the user opens the
    /// household screen, and a second call re-fetches rather than re-creating.
    @discardableResult
    func prepareShare() async -> Bool {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        let container = CloudContainer.shared
        guard (try? await container.accountStatus()) == .available else {
            state = .unavailable("Sign in to iCloud to let someone else be told.")
            return false
        }
        do {
            let db = container.privateCloudDatabase
            _ = try await db.save(CKRecordZone(zoneID: zoneID))
            if let existing = try await existingShare(in: db) {
                share = existing
            } else {
                let fresh = CKShare(recordZoneID: zoneID)
                fresh[CKShare.SystemFieldKey.title] = "SecuraCV — unanswered alarms" as CKRecordValue
                // Invitation only. A zone share must not be public, and
                // saying so here means a future edit has to argue with a line
                // of code rather than with a default.
                fresh.publicPermission = .none
                let saved = try await db.save(fresh)
                share = saved as? CKShare ?? fresh
            }
            await refreshMembers()
            state = .sharing
            return true
        } catch {
            state = .unavailable("iCloud couldn't set up sharing. Try again in a moment.")
            return false
        }
        #else
        state = .unavailable("Telling someone else needs iCloud.")
        return false
        #endif
    }

    /// Re-read who is actually on the share. Called after the sharing sheet
    /// closes and whenever the screen appears, because acceptance happens on
    /// someone else's device and we only ever learn about it by asking.
    func refreshMembers() async {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        // Set on both paths below, including the "no share" one: the point is
        // that we LOOKED, not what we found (see `hasResolvedShareState`).
        defer { hasResolvedShareState = true }
        guard let share = try? await existingShare(in: CloudContainer.shared.privateCloudDatabase) else {
            members = []
            // No share means nothing to publish into. Saying so here is what
            // makes a REVOKE take effect without a relaunch.
            if state.isSharing { state = .notSetUp }
            return
        }
        self.share = share
        members = HouseholdRelay.sorted(share.participants.map(Self.member(from:)))
        // Sharing set up in an earlier launch is still sharing. Without this,
        // `state` only ever became `.sharing` by walking through the invite
        // screen, so the first escalation after a relaunch would have found a
        // household it had forgotten it had and told nobody.
        state = .sharing
        #endif
    }

    /// Stop sharing entirely — the share is deleted, so every participant
    /// loses access at once. Opt-out has to be as real as opt-in, and this is
    /// the only revoke that needs no per-person bookkeeping.
    /// Returns true when CloudKit confirmed the revoke. On false the local
    /// state is left ALONE and `state` carries a sentence the screen shows —
    /// because the failure this guards is the worst one this feature has: both
    /// deletes discarded with `try?`, `members` cleared, `state` set to
    /// `.notSetUp`, and an owner who tapped "Stop sharing" on a plane told
    /// that sharing had stopped while every participant kept access, with no
    /// retry and no trace. Opt-out has to be as real as opt-in; a revoke that
    /// only *looks* like it worked is worse than one that admits it didn't.
    @discardableResult
    func stopSharing() async -> Bool {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        let db = CloudContainer.shared.privateCloudDatabase

        // STEP ONE IS THE ONLY ONE THAT GOVERNS ACCESS. Deleting the share is
        // what removes every participant; the zone deletion after it is
        // housekeeping. So they get separate error handling — folding both
        // into one `do` made a failed zone deletion report "everyone still has
        // access" when nobody did, and left `share` uncleared so the retry
        // re-deleted a record that was already gone, errored again, and never
        // reached the zone. A revoke that cannot be retried is barely a revoke.
        if let existing = share {
            do {
                _ = try await db.deleteRecord(withID: existing.recordID)
            } catch let error as CKError where error.code == .unknownItem {
                // Already gone — a previous attempt got this far. Not a failure.
            } catch {
                // Access really is intact. Say so, change nothing, stay retryable.
                state = .unavailable("Sharing wasn't stopped — iCloud couldn't be reached. Everyone still has access. Try again.")
                return false
            }
        }
        // Past this line nobody can read the zone any more, so the local state
        // is now telling the truth and is safe to clear.
        share = nil

        // Housekeeping: the zone still holds spent escalation records. Safe to
        // empty only now — the share is gone, so no subscriber is left for a
        // deletion to push at (see `sweepOldEscalations` for why that ordering
        // is the whole design).
        //
        // A failure here does NOT make the revoke a failure: access is already
        // revoked, and saying otherwise would be the same lie in the other
        // direction. The records are unreachable litter, and the next
        // `stopSharing` or `sweepOldEscalations` clears them.
        _ = try? await db.deleteRecordZone(withID: zoneID)
        #endif
        members = []
        state = .notSetUp
        return true
    }

    // MARK: - owner: publishing an escalation

    /// Write the one kind of record this zone may hold. Fire-and-forget for
    /// the same reason `AwayPush.publishWake` is: a household wake that fails
    /// to write must never stall the alert already reaching the person here.
    ///
    /// The severity gate is asserted again at the call site
    /// (`HouseholdRelay.mayReachHousehold`); this end refuses to write when
    /// there is nobody to read it, which keeps the zone empty in the common
    /// case where the owner never invited anyone.
    /// `occurrenceKey` names the alarm, not this write: every device the
    /// owner holds computes the same one, so a second device escalating the
    /// same alarm collides with the first record and its write simply fails.
    /// That failure IS the deduplication — no new record, so no second push
    /// on a participant's phone. (The "at most once" stamp lives in each
    /// device's own ledger, which is why it needed help crossing devices.)
    func publishEscalation(_ wake: WakeClass, occurrenceKey: String) {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        guard state == .sharing else { return }
        Task { [weak self] in
            guard let self else { return }
            let db = CloudContainer.shared.privateCloudDatabase

            // 1. DID SOMEBODY ALREADY ANSWER THIS, ON ANOTHER OF THE OWNER'S
            //    DEVICES? Acknowledging is device-local, so the iPad's timer
            //    can expire still believing nobody answered — and wake a
            //    household member at 3am about an alarm the owner dealt with
            //    on their iPhone twenty minutes ago. On the one channel whose
            //    entire value is that it stays quiet, that is the cry-wolf
            //    failure, arriving by a different road than the sweep did.
            //
            //    Fail OPEN, deliberately: if the check itself errors we
            //    publish. A missed escalation is the failure this whole ladder
            //    exists to prevent; a duplicate one is a household member
            //    checking their phone for nothing.
            if await self.wasAnsweredElsewhere(occurrenceKey: occurrenceKey, in: db) { return }

            // 2. IS THERE ANYBODY TO READ IT? Re-read the share rather than
            //    trusting the cached roster. The cached number exists to avoid
            //    writing into a zone nobody joined; consulting it here turned
            //    that optimization into a silent drop, because acceptance
            //    happens on someone else's device and nothing pushes it back.
            //    Also fails open — one wasted write costs nothing, and the
            //    alternative costs the escalation.
            if let fresh = try? await self.existingShare(in: db) {
                self.share = fresh
                self.members = HouseholdRelay.sorted(fresh.participants.map(Self.member(from:)))
                guard self.joinedCount > 0 else { return }
            }

            let id = CKRecord.ID(recordName: occurrenceKey, zoneID: self.zoneID)
            let record = CKRecord(recordType: HouseholdRelay.escalationRecordType, recordID: id)
            // The same one field the ordinary wake carries, and for the same
            // reason: a participant's app can say which KIND of trouble went
            // unanswered once they open it. No name, no id, no time of ours.
            record[WakePayload.classKey] = wake.rawValue as CKRecordValue
            // Still fire-and-forget from the caller's point of view, for the
            // same reason AwayPush.publishWake is: a household wake that fails
            // to write must never stall the alert already reaching the owner.
            _ = try? await db.save(record)
        }
        #endif
    }

    /// Has one of the owner's OTHER devices already marked this occurrence
    /// answered? Read from the owner's private default zone, where the marker
    /// lives — never the shared zone, which participants can read and which
    /// holds exactly one kind of record.
    ///
    /// Returns false when the answer is unknown, so an unreachable iCloud
    /// escalates rather than staying quiet.
    private func wasAnsweredElsewhere(occurrenceKey: String, in db: CKDatabase) async -> Bool {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        let id = CKRecord.ID(recordName: HouseholdRelay.answeredRecordName(for: occurrenceKey))
        do {
            _ = try await db.record(for: id)
            return true                     // the record exists ⇒ answered
        } catch let error as CKError where error.code == .unknownItem {
            return false                    // definitively not answered
        } catch {
            return false                    // unknown ⇒ fail open, escalate
        }
        #else
        return false
        #endif
    }

    /// The owner answered an alarm on THIS device. Tell their other devices,
    /// so a slower one doesn't escalate something already dealt with.
    ///
    /// Cheap and safe to call for every acknowledged alert: it writes nothing
    /// unless the owner actually shares with somebody, because a marker with
    /// no household to protect is a record for nobody.
    func noteAnswered(occurrenceKey: String) {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        // `state` starts at `.notSetUp` and only becomes `.sharing` after the
        // launch-time `refreshMembers()` has answered. Acking from a
        // notification action during a COLD LAUNCH runs before that — and
        // acking from the notification is the ordinary way people answer an
        // alarm, so a bare `guard state == .sharing` would drop the marker in
        // the commonest case and let a sibling device escalate anyway. That is
        // the whole bug this method exists to prevent, reintroduced by its own
        // guard.
        //
        // So: "not sharing" only counts once we have actually looked.
        if state == .sharing {
            writeAnswered(occurrenceKey)
            return
        }
        guard !hasResolvedShareState else { return }
        Task { [weak self] in
            await self?.refreshMembers()
            guard let self, self.state == .sharing else { return }
            self.writeAnswered(occurrenceKey)
        }
        #endif
    }

    #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
    private func writeAnswered(_ occurrenceKey: String) {
        let id = CKRecord.ID(recordName: HouseholdRelay.answeredRecordName(for: occurrenceKey))
        // Named `marker`, not `record`: the container linter attributes field
        // writes by local name, and a second `record` in this file holding a
        // different type makes every write in it unattributable.
        let marker = CKRecord(recordType: HouseholdRelay.answeredRecordType, recordID: id)
        // No payload at all. Existence IS the fact — which also means a second
        // device acking the same alarm writes the same name and simply loses,
        // exactly like the escalation record it guards.
        CloudContainer.shared.privateCloudDatabase.save(marker) { _, _ in }
    }
    #endif

    /// Escalation records are litter once read — but sweeping them is only
    /// safe while NOBODY is subscribed, and that is the whole subtlety here.
    ///
    /// A shared database offers no create-only visible subscription: query
    /// subscriptions (the ones that can fire on creation alone) don't exist
    /// there, so the participant's is a database subscription, which fires on
    /// every change INCLUDING deletions. Sweeping a day-old record would
    /// therefore push "Nobody answered" to every participant again, at app
    /// launch, about an alarm from yesterday — the household channel crying
    /// wolf, on the one channel whose entire value is that it stays quiet.
    ///
    /// The alternative was a silent subscription plus a fetch-and-post dance
    /// on the participant's side. That trades a reliable alarm for a
    /// best-effort one (iOS budgets silent pushes), which is the wrong trade
    /// for the last rung of an escalation ladder.
    ///
    /// So: sweep freely while nobody is listening, hold while somebody is,
    /// and `stopSharing` deletes the whole zone at a moment when no
    /// subscriber remains to be woken by it. What that leaves is costed in
    /// docs/design/cloudkit_backend.md §6.5.
    func sweepOldEscalations(now: Date = Date()) async {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        guard state == .sharing, joinedCount == 0 else { return }
        let db = CloudContainer.shared.privateCloudDatabase
        let cutoff = now.addingTimeInterval(-86_400) as NSDate
        let query = CKQuery(recordType: HouseholdRelay.escalationRecordType,
                            predicate: NSPredicate(format: "creationDate < %@", cutoff))
        guard let result = try? await db.records(matching: query, inZoneWith: zoneID) else { return }
        for (id, _) in result.matchResults {
            _ = try? await db.deleteRecord(withID: id)
        }
        #endif
    }

    /// The answered markers are litter too, and unlike the escalations they
    /// are also a RECORD OF WHEN THE OWNER WAS AWAKE. CloudKit stamps a
    /// precise creation date on every record and we cannot switch it off
    /// (§6.4), so an unbounded pile of "answered at 03:14, answered at 02:51"
    /// is exactly the event-correlated history this project coarsens
    /// everywhere else. They exist to stop a sibling device escalating within
    /// minutes; a day is already generous.
    ///
    /// Sweeping these is far simpler than sweeping the shared zone: they live
    /// in the owner's PRIVATE default database, where nobody is subscribed, so
    /// a deletion cannot push anything at anyone. None of the cry-wolf
    /// subtlety in `sweepOldEscalations` applies.
    func sweepOldAnswered(now: Date = Date()) async {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        let db = CloudContainer.shared.privateCloudDatabase
        let cutoff = now.addingTimeInterval(-86_400) as NSDate
        let query = CKQuery(recordType: HouseholdRelay.answeredRecordType,
                            predicate: NSPredicate(format: "creationDate < %@", cutoff))
        guard let result = try? await db.records(matching: query) else { return }
        for (id, _) in result.matchResults {
            _ = try? await db.deleteRecord(withID: id)
        }
        #endif
    }

    // MARK: - participant: accept, then listen

    /// Somebody tapped an invitation and iOS handed it to us. Accepting is
    /// the only moment this device learns the share exists.
    func acceptInvitation(_ metadata: Any) async {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        guard let metadata = metadata as? CKShare.Metadata else { return }
        do {
            _ = try await CloudContainer.shared.accept(metadata)
            isHelpingSomeone = true
            // Moment of need: they just agreed to be told something. Asking
            // now is both the best time to be allowed and the only time we
            // can honestly find out whether this device can be told at all.
            let allowed = await requestNotificationAuthorization?() ?? false
            participantBlocked = allowed ? nil : HouseholdRelay.participantNeedsNotifications
            await subscribeAsParticipant()
        } catch {
            state = .unavailable("That invitation couldn't be accepted. Ask them to send it again.")
        }
        #endif
    }

    /// Listen to the shared database. The words are written HERE, on the
    /// receiving device, because a shared-database subscription carries no
    /// record fields — which is why a household alert can be delivered
    /// without the owner's iCloud ever sending a sentence.
    func subscribeAsParticipant() async {
        #if canImport(UIKit)
        UIApplication.shared.registerForRemoteNotifications()
        #endif
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        let subscription = CKDatabaseSubscription(subscriptionID: HouseholdRelay.subscriptionID)
        let info = CKSubscription.NotificationInfo()
        info.title = HouseholdRelay.participantAlertTitle
        info.alertBody = HouseholdRelay.participantAlertBody
        info.soundName = "default"
        subscription.notificationInfo = info
        _ = try? await CloudContainer.shared.sharedCloudDatabase.save(subscription)
        #endif
    }

    /// Re-arm on launch for a device that already accepted, and notice if the
    /// owner revoked (the shared database simply stops having their zone).
    func refreshParticipation() async {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        guard (try? await CloudContainer.shared.accountStatus()) == .available else { return }
        let db = CloudContainer.shared.sharedCloudDatabase
        guard let zones = try? await db.allRecordZones() else { return }
        let helping = zones.contains { $0.zoneID.zoneName == HouseholdRelay.zoneName }
        isHelpingSomeone = helping
        if helping {
            // Re-checked every launch, not just at accept: notifications can
            // be turned off in Settings months later, and this device would
            // otherwise go on quietly counting as somebody who gets told.
            let allowed = await requestNotificationAuthorization?() ?? false
            participantBlocked = allowed ? nil : HouseholdRelay.participantNeedsNotifications
            await subscribeAsParticipant()
        } else {
            participantBlocked = nil
        }
        #endif
    }

    // MARK: - CloudKit → plain values

    #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
    /// The share for our zone, if it has one. `CKFetchRecordZonesOperation`
    /// would tell us the zone exists; only the share record says who is on it.
    private func existingShare(in db: CKDatabase) async throws -> CKShare? {
        let zone = try? await db.recordZone(for: zoneID)
        // `CKRecordZone.share` is a REFERENCE to the share record, not its id —
        // `db.record(for:)` takes the id, so the reference has to be unwrapped.
        // This did not compile, and nothing said so until a signed build tried:
        // see heal.sh for why CI never type-checked this file.
        guard let shareRef = zone?.share else { return nil }
        return try await db.record(for: shareRef.recordID) as? CKShare
    }

    /// Names come from the participant's own Apple account and stay on this
    /// device. A participant who hasn't accepted has no name to show yet, so
    /// the row says what it honestly knows.
    static func member(from participant: CKShare.Participant) -> HouseholdMember {
        let status: HouseholdMember.Status
        if participant.role == .owner {
            status = .owner
        } else {
            status = participant.acceptanceStatus == .accepted ? .accepted : .invited
        }
        let components = participant.userIdentity.nameComponents
        let name = components.map { PersonNameComponentsFormatter().string(from: $0) } ?? ""
        let fallback = participant.userIdentity.lookupInfo?.emailAddress
            ?? participant.userIdentity.lookupInfo?.phoneNumber
        return HouseholdMember(id: participant.userIdentity.userRecordID?.recordName
                                    ?? fallback ?? UUID().uuidString,
                               name: name.isEmpty ? (fallback ?? "Invited person") : name,
                               status: status)
    }
    #endif
}
