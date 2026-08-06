// QuietHours.swift
//
// "Don't buzz me at 3am about the everyday." A window of local wall-clock
// time in which ordinary alerts stay in the app instead of on the lock
// screen — and in which life-safety still gets through, because a smoke alarm
// that honors quiet hours is a decoration.
//
// One rule, enforced by the type rather than by care: `silences(_:at:)` can
// never return true for `.critical`. Every other level may be held; that one
// may not, so no future caller can quietly widen this into a mute switch.
//
// Pure and calendar-injectable — the wrap-around-midnight case is where these
// go wrong, so it is the case the tests hold hardest.

import Foundation

struct QuietHours: Codable, Hashable, Sendable {
    var enabled: Bool = false
    var startHour: Int = 22
    var startMinute: Int = 0
    var endHour: Int = 7
    var endMinute: Int = 0

    static let storeKey = "quiet_hours_v1"

    /// Minutes-since-midnight, the one representation the comparisons use.
    private var startMinutes: Int { startHour * 60 + startMinute }
    private var endMinutes: Int { endHour * 60 + endMinute }

    func contains(_ date: Date, calendar: Calendar = .current) -> Bool {
        guard enabled else { return false }
        let comps = calendar.dateComponents([.hour, .minute], from: date)
        let minutes = (comps.hour ?? 0) * 60 + (comps.minute ?? 0)
        let start = startMinutes, end = endMinutes
        // A zero-length window silences nothing. (The alternative reading —
        // "silences everything, always" — is exactly the untimed-off this
        // design refuses to have anywhere.)
        guard start != end else { return false }
        if start < end { return minutes >= start && minutes < end }
        return minutes >= start || minutes < end        // wraps midnight
    }

    /// May this level be held right now? Critical never can.
    func silences(_ level: AlertLevel, at date: Date, calendar: Calendar = .current) -> Bool {
        guard level != .critical else { return false }
        return contains(date, calendar: calendar)
    }

    /// The reason the Alerts tab shows on a held alert. Named for the setting
    /// the user would go change, so the sentence is actionable rather than
    /// merely apologetic.
    static let undeliveredReason = "Your quiet hours are on."

    // MARK: - the pickers' shape

    /// A Date carrying just this window's start/end wall-clock time, for the
    /// hour-and-minute DatePickers. Anchored on `reference`'s day; only the
    /// time components are ever read back.
    func startDate(on reference: Date = Date(), calendar: Calendar = .current) -> Date {
        Self.time(hour: startHour, minute: startMinute, on: reference, calendar: calendar)
    }

    func endDate(on reference: Date = Date(), calendar: Calendar = .current) -> Date {
        Self.time(hour: endHour, minute: endMinute, on: reference, calendar: calendar)
    }

    mutating func setStart(_ date: Date, calendar: Calendar = .current) {
        let c = calendar.dateComponents([.hour, .minute], from: date)
        startHour = c.hour ?? startHour
        startMinute = c.minute ?? startMinute
    }

    mutating func setEnd(_ date: Date, calendar: Calendar = .current) {
        let c = calendar.dateComponents([.hour, .minute], from: date)
        endHour = c.hour ?? endHour
        endMinute = c.minute ?? endMinute
    }

    private static func time(hour: Int, minute: Int, on reference: Date,
                             calendar: Calendar) -> Date {
        calendar.date(bySettingHour: hour, minute: minute, second: 0, of: reference) ?? reference
    }
}
