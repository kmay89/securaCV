// AwayPush.swift
//
// The away-from-home half of the alert path — the half that was, until now,
// only a comment. The Alerts tab has always offered an "Anywhere" reach and
// told the user it used "the metadata-only relay"; nothing in the app ever
// registered for remote notifications, so that promise could not be kept.
// A security app that overstates its reach is worse than one that admits a
// gap, so this file either delivers the away path or says why it can't.
//
// HOW, without a SecuraCV server: the wake rides the user's OWN iCloud.
// A CKQuerySubscription on their private database makes Apple's push service
// deliver a wake to every one of their devices whenever a wake record
// appears. That is the same trust boundary the design doc already blessed for
// CloudKit (§4, "the iCloud job that runs") and it satisfies §5's relay rules
// more completely than a hosted relay could:
//
//   * There is no third party at all — not even us. We hold no APNs key and
//     keep no device-token registry, so there is no server to compromise and
//     nothing to leak ("this household got some alerts" isn't learnable by
//     anyone, because nobody but the user is in the loop).
//   * The record is CONTENT-FREE by construction: a coarse severity class and
//     nothing else. No zone, no device name, no precise time (Invariant III).
//     The NSE composes the sentence on the phone from the user's own data.
//   * Per-install and revocable — deleting the subscription ends it (II).
//
// WHO writes the wake: a device that is home and can see the fleet. iOS will
// not run a socket in your pocket across town, so something on the LAN has to
// notice and post the wake. Two publishers exist, and they write the SAME
// record into the same private database:
//
//   * this phone, while it is home and looking at the fleet, and
//   * an Apple TV showing the Witness Wall, if the household turned on
//     "stand watch" (tvos/…/ResidentWatch.swift). That one matters more,
//     because the phone that left the house is exactly the device that can no
//     longer notice anything — it is the resident this design always assumed
//     and, until it shipped, did not have.
//
// This mirrors how HomeKit needs a home hub, and the limit is still honest:
// with nothing home, away alerts cannot happen, and `reach` says exactly that
// rather than pretending. The Apple TV's own limit — tvOS pauses an app that
// is not on screen — is stated on the Wall, where the promise is made.

import Foundation
#if canImport(CloudKit)
import CloudKit
#endif
#if canImport(UIKit)
import UIKit
#endif

/// Whether an away alert could actually reach this user right now — the
/// single source of truth behind every "Anywhere" claim in the UI.
enum AwayReach: Equatable {
    case ready
    /// Shown verbatim to the user; write it as a sentence they can act on.
    case unavailable(String)

    var isReady: Bool { self == .ready }

    var explanation: String {
        switch self {
        case .ready:
            return "Away alerts can reach this iPhone through your iCloud."
        case .unavailable(let why):
            return why
        }
    }
}

extension WakeClass {
    /// Derived from live fleet state by the resident device. Ordered by how
    /// much it should frighten someone: tamper first, then a broken proof,
    /// then a Canary that stopped answering. Lives here, not beside the enum,
    /// because the NSE compiles that file and must not link the fleet model.
    init(severity: Severity, badgeFailed: Bool, wentDark: Bool) {
        if severity >= .tamper { self = .tamper }
        else if badgeFailed { self = .integrity }
        else if wentDark { self = .offline }
        else { self = .pattern }
    }

    init(witness: Witness) {
        self.init(severity: witness.displaySeverity,
                  badgeFailed: witness.badge == .failed,
                  wentDark: witness.link.isDark)
    }
}

@MainActor
final class AwayPush: ObservableObject {
    static let shared = AwayPush()

    /// Honest capability, republished whenever it changes.
    @Published private(set) var reach: AwayReach = .unavailable("Not set up yet.")

    /// The CloudKit record type. The one field it may carry is named once, in
    /// WakePayload, so the publisher and the receiver cannot drift.
    static let wakeRecordType = "WitnessWake"
    static let subscriptionID = "securacv-witness-wake-v1"

    private var subscribed = false

    private init() {}

    // MARK: - setup

    /// Ask iOS for a push token and make sure the subscription exists. Safe to
    /// call repeatedly; CloudKit treats a re-saved subscription as a no-op.
    /// Never prompts the user for anything — the notification permission is
    /// asked for separately, at a moment of need.
    func enable() async {
        #if canImport(UIKit)
        UIApplication.shared.registerForRemoteNotifications()
        #endif
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        // The account IS the availability check — don't gate on a flag owned
        // elsewhere, or this path inherits that flag's bugs on top of its own.
        // Whether we may construct a container at all is a different question,
        // settled at compile time by the `#if` above (CloudContainer.swift).
        let container = CloudContainer.shared
        let status = try? await container.accountStatus()
        guard status == .available else {
            reach = .unavailable("Sign in to iCloud to get alerts when you're away.")
            return
        }
        do {
            try await saveSubscription(in: container.privateCloudDatabase)
            subscribed = true
            reach = .ready
        } catch {
            reach = .unavailable("iCloud couldn't set up away alerts. Open Alerts to retry.")
        }
        #else
        reach = .unavailable("Away alerts need iCloud.")
        #endif
    }

    /// iOS refused to hand us a push token (no network at launch, a profile
    /// without the push entitlement, a simulator). Say so plainly rather than
    /// leaving `reach` claiming a path that cannot carry anything.
    func noteRegistrationFailure() async {
        reach = .unavailable("iOS couldn't register this iPhone for alerts. Check your connection, then retry.")
    }

    /// Turn the away path off: delete the subscription so no wake can be
    /// delivered again. Opt-out has to be as real as opt-in (§5).
    func disable() async {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        let db = CloudContainer.shared.privateCloudDatabase
        _ = try? await db.deleteSubscription(withID: Self.subscriptionID)
        #endif
        subscribed = false
        reach = .unavailable("Away alerts are off.")
    }

    #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
    private func saveSubscription(in db: CKDatabase) async throws {
        let subscription = CKQuerySubscription(
            recordType: Self.wakeRecordType,
            predicate: NSPredicate(value: true),
            subscriptionID: Self.subscriptionID,
            options: [.firesOnRecordCreation])
        let info = CKSubscription.NotificationInfo()
        // A generic line so the LOCK SCREEN never leaks which Canary or what
        // happened before the phone is unlocked; the NSE replaces it with the
        // real sentence, composed on-device from the user's own data.
        info.title = "Your Canaries"
        info.alertBody = "Something needs your attention."
        info.soundName = "default"
        // The NSE only runs if the push is mutable — without this the generic
        // line IS the notification, and the away alert stays vague forever.
        info.shouldSendMutableContent = true
        // Carry the coarse class so the NSE can pick the right sentence and
        // interruption level. One key, four possible values, no content.
        info.desiredKeys = [WakePayload.classKey]
        subscription.notificationInfo = info
        _ = try await db.save(subscription)
    }
    #endif

    // MARK: - publishing (the resident device's job)

    /// Post a content-free wake so the user's OTHER devices light up. Called
    /// only by a device that can currently see the fleet on the LAN — that
    /// device is, by definition, home.
    ///
    /// Deliberately fire-and-forget: a wake that fails to write must never
    /// stall the local notification that is already reaching the person in
    /// front of us.
    func publishWake(_ wake: WakeClass) {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        // Our OWN readiness: a wake with no subscription behind it is a row
        // written into iCloud that can wake nobody.
        guard subscribed else { return }
        let record = CKRecord(recordType: Self.wakeRecordType)
        record[WakePayload.classKey] = wake.rawValue as CKRecordValue
        // No name. No id. No timestamp of ours.
        //
        // CloudKit stamps its own creation date on the record, and this comment
        // used to call that "coarse enough for routing," which read as though
        // the date were coarse. It is not: it is precise, we cannot switch it
        // off, and for a wake it is — to within seconds — the time the event
        // happened, sitting in the user's private database until sweepOldWakes
        // clears it. It never reaches the notification, it says when and never
        // what, and custody is the user's. That is the honest bound, and it is
        // argued rather than waved at in docs/design/cloudkit_backend.md §6.4.
        CloudContainer.shared.privateCloudDatabase.save(record) { _, _ in }
        #endif
    }

    /// Old wakes are litter: they carry nothing, but they are still rows in
    /// the user's iCloud. Sweep anything older than a day on launch.
    func sweepOldWakes(now: Date = Date()) async {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        guard subscribed else { return }
        let db = CloudContainer.shared.privateCloudDatabase
        let cutoff = now.addingTimeInterval(-86_400) as NSDate
        let query = CKQuery(recordType: Self.wakeRecordType,
                            predicate: NSPredicate(format: "creationDate < %@", cutoff))
        guard let result = try? await db.records(matching: query) else { return }
        for (id, _) in result.matchResults {
            _ = try? await db.deleteRecord(withID: id)
        }
        #endif
    }

}
