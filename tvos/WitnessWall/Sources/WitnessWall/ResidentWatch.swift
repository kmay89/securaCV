//  ResidentWatch.swift — the Apple TV stands watch for the household.
//
//  THE GAP THIS CLOSES. The away path needs something that is HOME to notice
//  trouble and post a wake; a phone that left the house cannot notice anything
//  about a hub it can no longer reach. Until now the only publisher was the
//  iPhone app, foregrounded, on the home network — which is precisely the
//  device that is *not* home when "away alerts" matter. An Apple TV is already
//  in the room, already awake, already polling the fleet to draw this screen.
//  It is the resident the design always assumed and never had.
//
//  WHAT IT PUBLISHES. The same content-free wake the phone publishes: one
//  coarse class word into the household's OWN iCloud private database
//  (`WakePayload`, shared source — one vocabulary, no drift). No device name,
//  no time of ours, no event content. SecuraCV runs no server in this path and
//  cannot read it.
//
//  WHAT IT CAN SEE, HONESTLY. The Wall polls `GET /api/fleet`, which carries
//  liveness and chain state and nothing else. So this resident can witness two
//  of the four wake classes — a Canary going dark, and a chain that stopped
//  verifying. It cannot see tamper or acoustic patterns, because they are not
//  in what it is given. It says nothing about what it cannot see rather than
//  guessing, and `ResidentStanding` tells the user exactly that.
//
//  THE LIMIT, SAID OUT LOUD. tvOS suspends an app that is not on screen. This
//  resident stands watch while the Wall is the app running on the Apple TV —
//  the dedicated-display case the Wall was built for. Switch the TV to another
//  app and the watch stops until the Wall is back. That is Apple's rule, not a
//  bug here, and the UI states it instead of implying an always-on promise.

import Foundation
#if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
import CloudKit
#endif

/// What the resident can honestly say about its own watch.
enum ResidentStanding: Equatable, Sendable {
    /// The household has not asked this Apple TV to stand watch.
    case off
    /// Asked, but this build cannot reach iCloud at all (unsigned/simulator).
    case unavailable
    /// Asked, and the build is fine, but this Apple TV is not signed into an
    /// iCloud account that can carry a wake. A separate rung from
    /// `.unavailable` because it is the household's to fix, not the build's.
    case signedOut
    /// Asked, and we have not yet heard back from iCloud about the account.
    /// Its own rung so the watch never *starts* by claiming to work.
    case checking
    /// Watching, and able to post a wake if the fleet goes wrong.
    case watching
    /// Watching, and it has posted at least one wake this session.
    case reported
    /// Watching, but the last wake did not get through. The most important
    /// rung on this list: it is the only one that reports a promise broken
    /// at the moment it mattered.
    case wakeFailed

    var line: String {
        switch self {
        case .off:
            return "Not standing watch. Turn it on to cover the household while everyone is out."
        case .unavailable:
            return "Standing watch needs iCloud on this Apple TV — sign in to use it."
        case .signedOut:
            return "Not standing watch: this Apple TV isn't signed into iCloud, so a wake has nowhere to go. Sign in under Settings → Users & Accounts."
        case .checking:
            return "Checking with iCloud…"
        case .watching:
            return "Standing watch — while the Wall is on screen, this Apple TV will tell your phones if a Canary goes dark or a chain stops verifying."
        case .reported:
            return "Standing watch — a wake has gone out this session."
        case .wakeFailed:
            return "Standing watch, but the last wake couldn't be sent to iCloud. It will be retried; check this Apple TV's network and iCloud sign-in."
        }
    }

    /// Would a household reading this line believe they are covered?
    ///
    /// Exists so the claim is testable in one place: every rung has to answer
    /// it, so a rung added later cannot quietly default to reassuring.
    var claimsCoverage: Bool {
        switch self {
        case .watching, .reported: return true
        case .off, .unavailable, .signedOut, .checking, .wakeFailed: return false
        }
    }
}

/// The pure half: what changed between two fleet snapshots that someone away
/// from home would want to know. No CloudKit, no clock, no I/O — so the rules
/// below are proven by tests rather than argued in a comment.
enum ResidentRules {
    /// Wake classes earned by the transition from `previous` to `current`.
    ///
    /// Transitions only, and only for Canaries present in BOTH snapshots.
    /// Three deliberate refusals:
    ///   * The first sight of a fleet is never news — a Wall that boots to a
    ///     dark Canary is reporting history, not an event.
    ///   * A Canary that merely *appears* already-dark is not news either; we
    ///     do not know when it went dark, and inventing that is the timing
    ///     claim Invariant III exists to prevent.
    ///   * A chain field that is absent means "not reported", never "broken"
    ///     (`FleetSnapshot.Device.chainIsTroubled` already draws that line).
    static func wakes(previous: FleetSnapshot?, current: FleetSnapshot) -> Set<WakeClass> {
        guard let previous else { return [] }
        var out: Set<WakeClass> = []
        let before = Dictionary(previous.devices.map { ($0.id, $0) }, uniquingKeysWith: { a, _ in a })
        for device in current.devices {
            guard let was = before[device.id] else { continue }
            if was.online && !device.online {
                out.insert(.offline)
            }
            if !was.chainIsTroubled && device.chainIsTroubled {
                out.insert(.integrity)
            }
        }
        return out
    }

    /// The minimum gap between wakes of one class, in seconds. A Canary that
    /// flaps must not page a household repeatedly; the phone's own alert rules
    /// are the fine-grained control, and this is only the floor beneath them.
    static let minGapSeconds: TimeInterval = 900  // 15 min

    /// Is this class allowed to go out now, given when it last did?
    static func allowed(lastSent: Date?, now: Date) -> Bool {
        guard let lastSent else { return true }
        return now.timeIntervalSince(lastSent) >= minGapSeconds
    }
}

/// The impure half: consent, rate limiting, and the CloudKit save.
@MainActor
@Observable
final class ResidentWatch {
    private(set) var standing: ResidentStanding = .off
    private var lastSnapshot: FleetSnapshot?
    private var lastSent: [WakeClass: Date] = [:]
    private let defaults: UserDefaults

    /// What iCloud said about the account on this Apple TV.
    ///
    /// `cloudUsable` answers "may this BUILD construct a container?" and that
    /// is all it ever answered — on any signed build it is `true` whether or
    /// not anyone is signed in. Standing watch on that alone told a signed-out
    /// household they were covered while every save failed in silence, which
    /// is the exact failure this whole feature was written to end.
    enum AccountState: Equatable { case unchecked, checking, ready, signedOut }
    private(set) var account: AccountState = .unchecked

    /// Wakes that were earned but did not reach iCloud.
    ///
    /// They have to be held HERE, not left to be re-derived: `observe` stores
    /// every snapshot as the new baseline, so the still-dark Canary produces
    /// no second transition and a dropped wake would never be earned again.
    private var pending: Set<WakeClass> = []

    /// Opt-in, off until a human turns it on — the same law the phone's away
    /// path and the Apple Home projection live under.
    private static let enabledKey = "SecuraCVResidentWatch"

    /// Whether a human has asked this Apple TV to stand watch.
    ///
    /// Plain stored property with an explicit setter rather than a `didSet`:
    /// `@Observable` rewrites stored properties into observed ones, and an
    /// observer that also writes to `UserDefaults` is the kind of sharp edge
    /// that works until the macro changes. One setter, one place the side
    /// effects live.
    private(set) var isEnabled: Bool

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        self.isEnabled = defaults.bool(forKey: Self.enabledKey)
        refreshStanding()
    }

    func setEnabled(_ on: Bool) {
        isEnabled = on
        defaults.set(on, forKey: Self.enabledKey)
        // Turning the watch off drops the baseline, so turning it back on
        // starts fresh rather than reporting whatever changed while nobody
        // was watching — history is not news.
        if !on {
            lastSnapshot = nil
            lastSent.removeAll()
            pending.removeAll()
            account = .unchecked
        }
        refreshStanding()
        // Turning it on is a promise; asking iCloud whether we can keep it is
        // the first thing that promise owes.
        if on { Task { await refreshAccount() } }
    }

    private func refreshStanding() {
        guard isEnabled else {
            standing = .off
            return
        }
        guard Self.cloudUsable else {
            standing = .unavailable
            return
        }
        // The account gate comes BEFORE any claim of watching. Ordered so the
        // reassuring rungs are the last ones reachable, never the default.
        switch account {
        case .unchecked, .checking: standing = .checking
        case .signedOut: standing = .signedOut
        case .ready: break
        }
        guard account == .ready else { return }
        if !pending.isEmpty {
            standing = .wakeFailed
            return
        }
        standing = lastSent.isEmpty ? .watching : .reported
    }

    /// Ask iCloud whether this Apple TV has an account that can carry a wake.
    ///
    /// Called when the watch is turned on and whenever the Wall comes back to
    /// the foreground — an account can be signed out from under us, and a
    /// resident that learned "ready" once and never asked again is the same
    /// stale claim in slower motion.
    func refreshAccount() async {
        guard isEnabled, Self.cloudUsable else { return }
        account = .checking
        refreshStanding()
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        let status = try? await CloudContainer.shared.accountStatus()
        account = (status == .available) ? .ready : .signedOut
        #else
        account = .signedOut
        #endif
        refreshStanding()
        // An account that just came back is the moment to retry what was
        // dropped while it was gone.
        if account == .ready, !pending.isEmpty { flushPending() }
    }

    /// Whether this BUILD can construct a CloudKit container without dying.
    ///
    /// Compile-time on purpose, and for exactly the reason
    /// `ios/Sources/SecuraCV/Cloud/CloudContainer.swift` documents at length:
    /// constructing a container in a process with no entitlements traps in a
    /// way Swift cannot catch, and an unsigned build has no entitlements.
    /// tvOS CI builds unsigned, so this flag and `CODE_SIGNING_ALLOWED=NO` are
    /// set together in `.github/workflows/tvos.yml` — one decision, two places
    /// it cannot disagree.
    static var cloudUsable: Bool {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        return true
        #else
        return false
        #endif
    }

    /// Feed the resident every snapshot the Wall successfully fetched.
    /// Publishes at most one wake per class per window; keeps the snapshot as
    /// the new baseline either way, so a transition is never counted twice.
    func observe(_ snapshot: FleetSnapshot, now: Date = Date()) {
        defer { lastSnapshot = snapshot }
        guard isEnabled, Self.cloudUsable else { return }
        // Anything dropped earlier goes first: the transition that earned it
        // will not happen twice, so this is its only remaining chance.
        if !pending.isEmpty { flushPending() }
        let earned = ResidentRules.wakes(previous: lastSnapshot, current: snapshot)
        for wake in earned.sorted(by: { $0.rawValue < $1.rawValue }) {
            guard ResidentRules.allowed(lastSent: lastSent[wake], now: now) else { continue }
            send(wake, now: now)
        }
        refreshStanding()
    }

    /// Try one wake, and let the ANSWER decide what we claim.
    ///
    /// The rate-limit stamp lands only on a save iCloud accepted. Stamping it
    /// first — as this did — spent the 15-minute budget on a wake that never
    /// left, so a failure was punished with silence for a quarter of an hour
    /// on top of the failure itself.
    private func send(_ wake: WakeClass, now: Date) {
        Task { [weak self] in
            guard let self else { return }
            let ok = await self.publish(wake)
            if ok {
                self.lastSent[wake] = now
                self.pending.remove(wake)
            } else {
                // Held for the next observation rather than dropped. The
                // Canary is still dark; the news is still true.
                self.pending.insert(wake)
            }
            self.refreshStanding()
        }
    }

    /// Retry everything that was held back, in a stable order.
    private func flushPending() {
        let now = Date()
        let queued = pending.sorted(by: { $0.rawValue < $1.rawValue })
        Task { [weak self] in
            guard let self else { return }
            for wake in queued where self.pending.contains(wake) {
                guard await self.publish(wake) else { continue }
                self.pending.remove(wake)
                self.lastSent[wake] = now
            }
            self.refreshStanding()
        }
    }

    /// The wake itself: one class word, into the household's own iCloud.
    /// Byte-identical in shape to the phone's publisher (`AwayPush`), because
    /// the receiving half — the notification service extension — decodes one
    /// contract and must not learn a second.
    ///
    /// Returns whether iCloud ACCEPTED the save. That return value is the
    /// point: discarding CloudKit's error — as the first version did with
    /// `{ _, _ in }` — made a quota, network, schema, or signed-out failure
    /// indistinguishable from delivery, and the UI went on saying a wake had
    /// gone out. `async` rather than a completion handler so the result cannot
    /// be ignored by accident a second time.
    private func publish(_ wake: WakeClass) async -> Bool {
        #if canImport(CloudKit) && !SECURACV_NO_CLOUDKIT
        let record = CKRecord(recordType: Self.wakeRecordType)
        record[WakePayload.classKey] = wake.rawValue as CKRecordValue
        do {
            _ = try await CloudContainer.shared.privateCloudDatabase.save(record)
            return true
        } catch {
            return false
        }
        #else
        return false
        #endif
    }

    /// Named to match the phone's constant so the CloudKit gate
    /// (`scripts/lint_cloudkit_container.py`) sees one record type, not two.
    static let wakeRecordType = "WitnessWake"
}
