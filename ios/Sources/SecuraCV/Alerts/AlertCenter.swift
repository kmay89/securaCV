// AlertCenter.swift
//
// The smoke-alarm brain (docs/design/iphone_companion_app.md §5b). Two rules
// govern everything here:
//   1. Silent almost always. Only a tiny, user-armed set of events may PUSH;
//      everything else is pull. We map that onto iOS interruption levels so we
//      literally cannot spam: digests are .passive, real things .timeSensitive,
//      genuine life-safety .critical (the level that pierces silent mode — it
//      needs Apple's Critical Alert entitlement; until granted we fall back to
//      .timeSensitive automatically).
//   2. Provably alive. A signed heartbeat proves the whole delivery path works
//      BEFORE the emergency; a one-tap Test Alert round-trips it on demand.

import Foundation
import UserNotifications

/// How loud an alert is allowed to be. Ordered least→most intrusive.
enum AlertLevel {
    case digest        // pull-only; never buzzes
    case important     // time-sensitive
    case critical      // life-safety; bypasses silent/Focus if entitled

    /// The caller passes the entitlement state in (AlertCenter owns it on the
    /// main actor; this enum stays nonisolated and pure).
    func interruption(critical entitled: Bool) -> UNNotificationInterruptionLevel {
        switch self {
        case .digest: return .passive
        case .important: return .timeSensitive
        case .critical: return entitled ? .critical : .timeSensitive
        }
    }
}

/// A user-armed rule: which severities may reach me, and how far.
struct AlertRule: Codable, Hashable, Identifiable {
    enum Reach: String, Codable { case onWiFiOnly, anywhere }
    var id: String
    var title: String
    var minSeverity: Severity
    var reach: Reach = .onWiFiOnly
    var enabled: Bool = true
    // The alert *level* is derived from severity in AlertCenter.level(for:),
    // not stored, so a rule stays a small, Codable, user-facing thing.

    /// Does anything the user armed want to reach them off-network? Drives
    /// whether the away path is SET UP at all — nobody should be registered
    /// for pushes they never asked to receive. This is deliberately a global
    /// question; whether a PARTICULAR alert may travel is a different one
    /// (AlertCenter.reachesAnywhere(severity:)).
    static func anyReachesAnywhere(rules: [AlertRule]) -> Bool {
        rules.contains { $0.enabled && $0.reach == .anywhere }
    }

    static let defaults: [AlertRule] = [
        .init(id: "tamper", title: "Tamper or panic", minSeverity: .tamper, reach: .anywhere),
        .init(id: "integrity", title: "Signature / chain broke", minSeverity: .alert, reach: .anywhere),
        .init(id: "dark", title: "A Canary went dark", minSeverity: .alert, reach: .anywhere),
        .init(id: "activity", title: "Everyday activity", minSeverity: .notice, reach: .onWiFiOnly),
    ]
}

@MainActor
final class AlertCenter: NSObject, ObservableObject {
    @Published var rules: [AlertRule] = AlertRule.defaults
    @Published private(set) var authorized = false

    /// True once Apple grants the Critical Alert entitlement AND the user
    /// allows it. Handed to AlertLevel.interruption(critical:) to decide
    /// whether .critical is real.
    static var hasCriticalEntitlement = false

    /// Notification category + action identifiers — one category for every
    /// witness alert, so Ack / Mute ride along to the wrist for free (the
    /// system mirrors categories and actions to a paired watch).
    static let witnessCategoryID = "SECURACV_WITNESS"
    static let ackActionID = "SECURACV_ACK"
    static let muteActionID = "SECURACV_MUTE_1H"
    /// The self-test's thread — the one alert Mute/Ack make no sense for.
    static let selfTestThread = "selftest"

    /// Wired by FleetStore: the notification actions land here, on the
    /// witness id carried as the thread identifier.
    var onAck: ((String) -> Void)?
    var onMute: ((String) -> Void)?

    private let center = UNUserNotificationCenter.current()

    override init() {
        super.init()
        // Delegate + categories must exist before the first delivery, not
        // after the first tap — set them the moment the brain is built.
        center.delegate = self
        let ack = UNNotificationAction(identifier: Self.ackActionID,
                                       title: "Acknowledge")
        let mute = UNNotificationAction(identifier: Self.muteActionID,
                                        title: "Mute 1 hour")
        let category = UNNotificationCategory(identifier: Self.witnessCategoryID,
                                              actions: [ack, mute],
                                              intentIdentifiers: [])
        center.setNotificationCategories([category])
    }

    func requestAuthorization() async {
        // iOS does NOT ignore an unentitled .criticalAlert — it fails the
        // whole request (UNErrorDomain code 1), which would cost us even
        // ordinary alerts on any build signed without the Apple-granted
        // entitlement (Debug always; Release until the grant). So ask for the
        // full set, and on error fall back to the standard ask. A user's
        // "Don't Allow" reports granted=false, not an error, so the retry
        // never re-prompts someone who declined.
        let full: UNAuthorizationOptions = [.alert, .sound, .badge, .criticalAlert, .timeSensitive]
        let standard: UNAuthorizationOptions = [.alert, .sound, .badge, .timeSensitive]
        do {
            authorized = try await center.requestAuthorization(options: full)
        } catch {
            authorized = (try? await center.requestAuthorization(options: standard)) ?? false
        }
        let settings = await center.notificationSettings()
        Self.hasCriticalEntitlement = settings.criticalAlertSetting == .enabled
    }

    /// Decide the level for an incoming event, honoring the armed rules AND
    /// the user's Focus (FocusGate — set from iOS's own Focus settings via
    /// FleetFocusFilter). Returns nil when the event is pull-only (must NOT
    /// push) — the abuse guard.
    func level(for severity: Severity, awayFromHome: Bool) -> AlertLevel? {
        let matching = rules.filter { $0.enabled && severity >= $0.minSeverity }
        guard let strongest = matching.max(by: { $0.minSeverity < $1.minSeverity }) else { return nil }
        if awayFromHome && strongest.reach == .onWiFiOnly { return nil }   // reach honesty
        switch strongest.minSeverity {
        case .tamper: return .critical
        case .alert:
            // The user's Focus said "only life-safety" — honor it. Critical
            // still passes: Focus quiets the everyday, never the smoke alarm.
            return FocusGate.criticalOnly() ? nil : .important
        default: return nil          // digest events are pulled, never pushed —
                                     // nil IS the abuse guard this method promises
        }
    }

    /// Is anything the user armed watching for this severity at all? Lets the
    /// history separate "your Focus silenced it" from "you never asked about
    /// this" — two very different sentences to read at 3am.
    func hasArmedRule(for severity: Severity) -> Bool {
        rules.contains { $0.enabled && severity >= $0.minSeverity }
    }

    /// May an alert of this severity travel off-network? Answered by the SAME
    /// strongest-matching-rule logic `level(for:)` uses, so the per-rule reach
    /// choice is honored for wakes exactly as it is for local posts — a rule
    /// left on "On Wi-Fi only" must not have its alarms published just because
    /// some other, unrelated rule wants away reach.
    ///
    /// Focus is deliberately NOT consulted here: Focus is a property of the
    /// device doing the quieting, and the device publishing a wake is not the
    /// one that will receive it. The receiving device applies its own.
    func reachesAnywhere(severity: Severity) -> Bool {
        let matching = rules.filter { $0.enabled && severity >= $0.minSeverity }
        guard let strongest = matching.max(by: { $0.minSeverity < $1.minSeverity }) else {
            return false
        }
        return strongest.reach == .anywhere
    }

    /// The app-icon badge: how many alert rows the user has never laid eyes
    /// on. Zero clears it. Best-effort by design — a badge is a hint, and a
    /// failed write must never become a thrown error in an alert path.
    /// Deduped, because the sentinel evaluates every few seconds and the
    /// number rarely changes.
    private var lastBadge: Int?
    func setBadge(_ count: Int) {
        let clamped = max(0, count)
        guard clamped != lastBadge else { return }
        lastBadge = clamped
        center.setBadgeCount(clamped)
    }

    /// Fire a LOCAL notification (used on-LAN alerts). Remote pushes arrive
    /// via APNs and are shaped identically by the NSE.
    func post(title: String, body: String, level: AlertLevel, threadID: String) {
        guard let req = request(title: title, body: body, level: level, threadID: threadID) else { return }
        center.add(req)
    }

    /// The SELF-TEST's delivery: post and CONFIRM. Throws unless
    /// notifications are actually authorized and the system accepted the
    /// request — "verified" means checked, nothing looser (non-negotiable
    /// #4), so a beat must never be recorded, nor the canary's chirp earned,
    /// for a delivery that structurally couldn't reach the user.
    func postConfirmed(title: String, body: String, level: AlertLevel, threadID: String) async throws {
        struct DeliveryRefused: LocalizedError {
            let reason: String
            var errorDescription: String? { reason }
        }
        let settings = await center.notificationSettings()
        switch settings.authorizationStatus {
        case .authorized, .provisional, .ephemeral:
            break
        default:
            throw DeliveryRefused(reason: "notifications are off for SecuraCV")
        }
        guard let req = request(title: title, body: body, level: level, threadID: threadID) else {
            throw DeliveryRefused(reason: "digest-level alerts never push")
        }
        try await center.add(req)
    }

    /// One shape for every local notification; nil for digests (never buzz).
    private func request(title: String, body: String, level: AlertLevel,
                         threadID: String) -> UNNotificationRequest? {
        guard level != .digest else { return nil }
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.threadIdentifier = threadID
        let interruption = level.interruption(critical: Self.hasCriticalEntitlement)
        content.interruptionLevel = interruption
        content.sound = interruption == .critical ? .defaultCritical : .default
        // Summary honesty: the system stacks and ranks by relevance — a
        // critical must never sort under an everyday important.
        content.relevanceScore = level == .critical ? 0.9 : 0.6
        // Witness alerts are actionable (Ack / Mute, mirrored to the wrist);
        // the self-test is not — muting a test would mean nothing.
        if threadID != Self.selfTestThread {
            content.categoryIdentifier = Self.witnessCategoryID
        }
        return UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil)
    }
}

// MARK: - delegate (foreground presentation + the actions' landing pad)

extension AlertCenter: UNUserNotificationCenterDelegate {
    /// Foreground presentation: an alert the brain decided to post is worth
    /// seeing even with the app open — iOS suppresses them entirely unless
    /// the delegate opts in.
    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter,
                                            willPresent notification: UNNotification)
        async -> UNNotificationPresentationOptions {
        [.banner, .list, .sound]
    }

    /// Ack / Mute / tap-through. The witness id travels as the thread
    /// identifier, so the action needs no payload of its own.
    nonisolated func userNotificationCenter(_ center: UNUserNotificationCenter,
                                            didReceive response: UNNotificationResponse) async {
        let threadID = response.notification.request.content.threadIdentifier
        let action = response.actionIdentifier
        guard threadID != Self.selfTestThread, !threadID.isEmpty else { return }
        await MainActor.run {
            switch action {
            case Self.ackActionID:
                self.onAck?(threadID)
                self.clearDelivered(thread: threadID)
            case Self.muteActionID:
                self.onMute?(threadID)
                self.clearDelivered(thread: threadID)
            default:
                break   // default tap just opens the app — the Today tab is the answer
            }
        }
    }

    /// "Ack travels": acknowledging clears the stack for that witness here,
    /// and (because delivered notifications sync) from the mirrored wrist too.
    private func clearDelivered(thread: String) {
        let center = self.center
        center.getDeliveredNotifications { delivered in
            let ids = delivered
                .filter { $0.request.content.threadIdentifier == thread }
                .map { $0.request.identifier }
            if !ids.isEmpty { center.removeDeliveredNotifications(withIdentifiers: ids) }
        }
    }
}
