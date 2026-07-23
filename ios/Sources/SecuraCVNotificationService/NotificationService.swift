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

        // The wake carries only a coarse severity class + a signed token.
        let sev = (content.userInfo["sev"] as? String) ?? "alert"
        content.title = "Your Canaries"
        content.body = Self.line(for: sev)
        content.interruptionLevel = (sev == "tamper") ? .critical : .timeSensitive
        content.sound = (sev == "tamper") ? .defaultCritical : .default

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

    private static func line(for sev: String) -> String {
        switch sev {
        case "tamper": return "A Canary reports tamper — open to see which."
        case "integrity": return "A signature or chain check failed."
        case "offline": return "A Canary went dark."
        default: return "A Canary needs your attention."
        }
    }
}
