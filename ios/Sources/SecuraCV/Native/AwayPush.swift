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
//     The NSE turns the class into its fixed sentence on the phone.
//   * Revocable — deleting the subscription ends it (II). It is ONE
//     subscription under a fixed id in the user's private database, so it
//     belongs to the iCloud account, not to this install: deleting it stops
//     wakes for every device on the account, and a device whose own rules
//     still reach Anywhere saves it again the next time it opens
//     (`follow(rules:)` at launch). The rules themselves stay on each device.
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

/// What the away path should do for the rules the user has armed — decided
/// in ONE place, so launch, the rules sheet and every rule edit agree.
///
/// This used to be one-directional: launch and the rules sheet could only
/// ever ENABLE, and `AwayPush.disable()` had no caller. Switching every rule
/// to "On Wi-Fi only" left the iCloud subscription standing, so wakes kept
/// reaching a phone whose owner had opted out — the opposite of "opt-out is
/// as real as opt-in" (docs/design/cloudkit_backend.md §5).
enum AwayArming: Equatable {
    /// Some armed rule wants to reach the user off the home network.
    case arm
    /// Nothing does, and a subscription may still exist: delete it.
    case disarm
    /// Nothing does, and nothing is standing — no network call needed.
    case leave

    /// The sentence `reach` carries while the path is off by choice.
    static let offSentence = "Away alerts are off — no rule is set to Anywhere."

    /// `mayBeSubscribed` is the device's own memory of having saved the
    /// subscription (`AwayPush.mayBeSubscribed`). It is TRUE when unknown —
    /// an install from before this memory existed may hold one — because
    /// the failure directions are not symmetric: an extra delete pauses the
    /// account's other devices until one with an Anywhere rule next opens
    /// and saves it again, a skipped one keeps waking someone who said no.
    /// Once known false, a device that never armed the path leaves it alone,
    /// so it cannot cut a path another device of the account relies on.
    static func decide(rules: [AlertRule], mayBeSubscribed: Bool) -> AwayArming {
        if AlertRule.anyReachesAnywhere(rules: rules) { return .arm }
        return mayBeSubscribed ? .disarm : .leave
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

    /// Calls to `follow(rules:)` run one at a time, in order: a quick
    /// Anywhere → On Wi-Fi only flip must not let the slower save land after
    /// the delete and leave a subscription nobody wants.
    private var lastFollow: Task<Void, Never>?

    /// This device's memory of having saved the subscription — what lets a
    /// relaunch with no away-reaching rule skip the network entirely.
    /// Missing reads as TRUE (see `AwayArming.decide`); it turns false only
    /// once iCloud confirms the subscription is gone.
    private static let savedKey = "away_subscription_saved_v1"
    static var mayBeSubscribed: Bool {
        get { UserDefaults.standard.object(forKey: savedKey) as? Bool ?? true }
        set { UserDefaults.standard.set(newValue, forKey: savedKey) }
    }

    private init() {}

    /// Bring the away path in line with the armed rules: set it up when a
    /// rule reaches Anywhere, tear it down when none does. The one entry
    /// point for launch and the rules sheet (AwayArming decides).
    func follow(rules: [AlertRule]) async {
        let previous = lastFollow
        let step = Task { @MainActor in
            await previous?.value
            switch AwayArming.decide(rules: rules, mayBeSubscribed: Self.mayBeSubscribed) {
            case .arm:
                // enable() is idempotent; skip the round trip when it is
                // already standing.
                if !self.reach.isReady { await self.enable() }
            case .disarm:
                await self.disable()
            case .leave:
                self.subscribed = false
                self.reach = .unavailable(AwayArming.offSentence)
            }
        }
        lastFollow = step
        await step.value
    }

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
            Self.mayBeSubscribed = true
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
    /// delivered again. Opt-out has to be as real as opt-in (§5). Called by
    /// `follow(rules:)` when no armed rule reaches Anywhere.
    ///
    /// The saved-memory flag clears only when iCloud confirms the deletion
    /// (or says there was nothing to delete); a failure — no network, signed
    /// out — leaves it set, so the next launch tries again rather than
    /// trusting a delete that never landed.
    func disable() async {
        subscribed = false
        reach = .unavailable(AwayArming.offSentence)
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        let container = CloudContainer.shared
        guard (try? await container.accountStatus()) == .available else { return }
        do {
            _ = try await container.privateCloudDatabase.deleteSubscription(withID: Self.subscriptionID)
            Self.mayBeSubscribed = false
        } catch let error as CKError where error.code == .unknownItem {
            Self.mayBeSubscribed = false
        } catch {
            // Left set on purpose: retried at the next launch.
        }
        #else
        Self.mayBeSubscribed = false
        #endif
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
        // class's fixed sentence (WakeClass.line), composed on-device.
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
