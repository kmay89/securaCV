// NotificationIDs.swift  (SHARED — phone and wrist)
//
// The notification category every witness alert carries, named once. The
// iPhone registers it with its Acknowledge / Mute actions (AlertCenter) and
// stamps it on every witness alert; the watch app registers a custom
// long-look for the same category (SecuraCVWatch/WitnessNotification.swift).
// Two targets, one string — so the wrist's layout can never silently stop
// matching the alerts the phone posts.

import Foundation

enum NotificationIDs {
    /// One category for every witness alert. Its id is part of what a
    /// mirrored notification carries to the watch, so it must never change
    /// spelling between builds of the two apps.
    static let witnessCategory = "SECURACV_WITNESS"
}
