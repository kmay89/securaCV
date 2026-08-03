// FocusGate.swift
//
// The bridge between the user's Focus modes and the alert brain. Apple's
// Focus filter (Native/FleetFocusFilter.swift) writes ONE bit here — "during
// this Focus, only life-safety alerts" — and AlertCenter.level(for:) reads
// it before letting an Important push through. Stored in the shared app
// group so the same answer is visible to every process the system might run
// the intent in. Tamper/critical always passes: Focus can quiet the everyday,
// never the smoke alarm.

import Foundation

enum FocusGate {
    static let key = "focus_critical_only_v1"

    static var sharedDefaults: UserDefaults {
        UserDefaults(suiteName: "group.com.securacv.witness") ?? .standard
    }

    static func setCriticalOnly(_ on: Bool, in defaults: UserDefaults = sharedDefaults) {
        defaults.set(on, forKey: key)
    }

    static func criticalOnly(in defaults: UserDefaults = sharedDefaults) -> Bool {
        defaults.bool(forKey: key)
    }
}
