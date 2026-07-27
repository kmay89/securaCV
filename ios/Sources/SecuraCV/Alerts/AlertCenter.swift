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

    static let defaults: [AlertRule] = [
        .init(id: "tamper", title: "Tamper or panic", minSeverity: .tamper, reach: .anywhere),
        .init(id: "integrity", title: "Signature / chain broke", minSeverity: .alert, reach: .anywhere),
        .init(id: "dark", title: "A Canary went dark", minSeverity: .alert, reach: .anywhere),
        .init(id: "activity", title: "Everyday activity", minSeverity: .notice, reach: .onWiFiOnly),
    ]
}

@MainActor
final class AlertCenter: ObservableObject {
    @Published var rules: [AlertRule] = AlertRule.defaults
    @Published private(set) var authorized = false

    /// True once Apple grants the Critical Alert entitlement AND the user
    /// allows it. Handed to AlertLevel.interruption(critical:) to decide
    /// whether .critical is real.
    static var hasCriticalEntitlement = false

    private let center = UNUserNotificationCenter.current()

    func requestAuthorization() async {
        // Ask for the critical option too; iOS ignores it gracefully if the
        // app isn't entitled, so this is safe to request unconditionally.
        let opts: UNAuthorizationOptions = [.alert, .sound, .badge, .criticalAlert, .timeSensitive]
        let granted = (try? await center.requestAuthorization(options: opts)) ?? false
        authorized = granted
        let settings = await center.notificationSettings()
        Self.hasCriticalEntitlement = settings.criticalAlertSetting == .enabled
    }

    /// Decide the level for an incoming event, honoring the armed rules. Returns
    /// nil when the event is pull-only (must NOT push) — the abuse guard.
    func level(for severity: Severity, awayFromHome: Bool) -> AlertLevel? {
        let matching = rules.filter { $0.enabled && severity >= $0.minSeverity }
        guard let strongest = matching.max(by: { $0.minSeverity < $1.minSeverity }) else { return nil }
        if awayFromHome && strongest.reach == .onWiFiOnly { return nil }   // reach honesty
        switch strongest.minSeverity {
        case .tamper: return .critical
        case .alert: return .important
        default: return .digest      // digest events are pulled, never pushed
        }
    }

    /// Fire a LOCAL notification (used on-LAN and for Test Alerts). Remote
    /// pushes arrive via APNs and are shaped identically by the NSE.
    func post(title: String, body: String, level: AlertLevel, threadID: String) {
        guard level != .digest else { return }   // digests never buzz
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.threadIdentifier = threadID
        let interruption = level.interruption(critical: Self.hasCriticalEntitlement)
        content.interruptionLevel = interruption
        content.sound = interruption == .critical ? .defaultCritical : .default
        let req = UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil)
        center.add(req)
    }
}
