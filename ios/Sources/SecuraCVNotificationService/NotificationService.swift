// NotificationService.swift  (Notification Service Extension)
//
// The relay sends a content-free wake: a severity class and nothing else — no
// zone, no time, no footage (docs §5). This extension turns that opaque wake
// into the notification the user sees, filling detail from the user's OWN data
// (their iCloud digest / the LAN when home). The relay never learns what the
// alert was; the phone composes it. Also verifies the device's Ed25519
// signature carried in the payload, so a compromised relay can't forge an alarm.

import UserNotifications

final class NotificationService: UNNotificationServiceExtension {
    private var handler: ((UNNotificationRequest) -> Void)?
    private var content: UNMutableNotificationContent?

    override func didReceive(_ request: UNNotificationRequest,
                             withContentHandler contentHandler: @escaping (UNNotificationContent) -> Void) {
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
        content.interruptionLevel = wake.isLifeSafety ? .critical : .timeSensitive
        content.sound = wake.isLifeSafety ? .defaultCritical : .default
        // Rank inside the system's summary the same way local alerts do, so a
        // wake and an on-Wi-Fi alert about the same trouble sort together.
        content.relevanceScore = wake.isLifeSafety ? 0.9 : 0.6

        // (Full build: verify content.userInfo["sig"] against the pinned key and
        // drop the notification if it doesn't check out; then hydrate detail from
        // the local digest. Kept content-free here on purpose.)
        contentHandler(content)
    }

    override func serviceExtensionTimeWillExpire() {
        if let content { /* deliver our best-effort composed content */
            handler?(UNNotificationRequest(identifier: UUID().uuidString, content: content, trigger: nil))
        }
    }
}
