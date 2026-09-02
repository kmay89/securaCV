// NotificationService.swift  (Notification Service Extension)
//
// The relay sends a content-free wake: a severity class and nothing else — no
// zone, no time, no footage (docs §5). This extension turns that opaque wake
// into the notification the user sees, filling detail from the user's OWN data
// (their iCloud digest / the LAN when home). The relay never learns what the
// alert was; the phone composes it. The wake is NOT signature-verified here:
// what makes it trustworthy today is the transport — it arrives through the
// user's own CloudKit private database, which only their devices can write to
// (see AwayPush). Payload-level Ed25519 verification against the pinned key is
// the planned hardening for a self-hosted relay, not a property this code has.

import UserNotifications

final class NotificationService: UNNotificationServiceExtension {
    private var handler: ((UNNotificationContent) -> Void)?
    private var content: UNMutableNotificationContent?

    override func didReceive(_ request: UNNotificationRequest,
                             withContentHandler contentHandler: @escaping (UNNotificationContent) -> Void) {
        // Kept for the timeout path: on NSE expiry, iOS shows the generic
        // push unless the stored handler delivers our best-effort compose.
        self.handler = contentHandler
        let mutable = request.content.mutableCopy() as? UNMutableNotificationContent
        self.content = mutable
        guard let content = mutable else { contentHandler(request.content); return }

        // The wake carries only a coarse severity class. WakePayload knows
        // both envelope shapes (CloudKit's query notification today, a
        // self-hosted relay's flat body later) and owns the sentences, so the
        // app and this extension can never word the same wake differently.
        let wake = WakePayload.wakeClass(from: content.userInfo) ?? .pattern
        content.title = "Your Canaries"
        content.body = wake.line
        // Time-sensitive is the safe default for every class, including
        // life-safety: iOS does NOT ignore an unentitled critical alert, it
        // drops the notification. AlertCenter learned this the hard way for
        // local alerts and gates .critical on the granted setting; this
        // extension has no access to that cache, so it asks the system
        // itself below and upgrades only when critical alerts are enabled.
        content.interruptionLevel = .timeSensitive
        content.sound = .default
        // Rank inside the system's summary the same way local alerts do, so a
        // wake and an on-Wi-Fi alert about the same trouble sort together.
        content.relevanceScore = wake.isLifeSafety ? 0.9 : 0.6

        // (Full build: verify content.userInfo["sig"] against the pinned key and
        // drop the notification if it doesn't check out; then hydrate detail from
        // the local digest. Kept content-free here on purpose.)
        guard wake.isLifeSafety else { contentHandler(content); return }
        UNUserNotificationCenter.current().getNotificationSettings { settings in
            if settings.criticalAlertSetting == .enabled {
                content.interruptionLevel = .critical
                content.sound = .defaultCritical
            }
            contentHandler(content)
        }
    }

    override func serviceExtensionTimeWillExpire() {
        // Deliver our best-effort composed content instead of letting iOS
        // fall back to the generic push.
        if let content { handler?(content) }
    }
}
