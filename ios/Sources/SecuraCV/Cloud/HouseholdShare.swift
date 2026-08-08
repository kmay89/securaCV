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

    /// How many people an escalation would actually reach. The one number
    /// FleetStore consults before publishing, so a wake is never written into
    /// a zone nobody joined.
    var reachableCount: Int { HouseholdRelay.reachableCount(members) }

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
    func stopSharing() async {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        let db = CloudContainer.shared.privateCloudDatabase
        if let share {
            _ = try? await db.deleteRecord(withID: share.recordID)
        }
        share = nil
        // Now — and only now — the zone can be emptied without waking
        // anybody: the share is gone, so there is no subscriber left for a
        // deletion to push at (see `sweepOldEscalations` for why that
        // ordering is the whole design). Deleting the zone takes every
        // escalation record with it in one operation.
        _ = try? await db.deleteRecordZone(withID: zoneID)
        #endif
        members = []
        state = .notSetUp
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
        guard state == .sharing, reachableCount > 0 else { return }
        let id = CKRecord.ID(recordName: occurrenceKey, zoneID: zoneID)
        let record = CKRecord(recordType: HouseholdRelay.escalationRecordType, recordID: id)
        // The same one field the ordinary wake carries, and for the same
        // reason: a participant's app can say which KIND of trouble went
        // unanswered once they open it. No name, no id, no time of ours.
        record[WakePayload.classKey] = wake.rawValue as CKRecordValue
        CloudContainer.shared.privateCloudDatabase.save(record) { _, _ in }
        #endif
    }

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
        guard state == .sharing, reachableCount == 0 else { return }
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
