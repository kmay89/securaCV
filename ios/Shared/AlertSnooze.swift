// AlertSnooze.swift  (SHARED — phone + watch speak the same durations)
//
// How long "quiet down" lasts. Mute used to be exactly one hour on every path
// in the app, which is the wrong length for most of the reasons people mute:
// a delivery driver is 20 minutes, a party is until bedtime, a barking-dog
// night is until morning.
//
// One rule governs this whole file: **a mute always expires.** There is no
// untimed off, on any surface, ever — an untimed mute is how a camera quietly
// becomes decorative, and it is the single failure mode that would let this
// product be silent on the night it mattered. (Ring's moon button gets this
// right; plenty of others don't.) Tamper and a failed signature still punch
// through every one of these, and that guarantee lives where it can't be
// bypassed from here: Witness.effectiveSeverity.
//
// The boundaries are named once, here, so the phone's sheet, the wrist's menu
// and any future surface all mean the same thing by "tonight".

import Foundation

enum MuteDuration: String, Codable, CaseIterable, Sendable, Identifiable {
    case oneHour
    case untilTonight
    case untilMorning

    var id: String { rawValue }

    /// The evening a "tonight" mute runs to, and the morning a "morning" mute
    /// runs to. Local wall-clock hours on purpose: the user means their
    /// evening, not a fixed number of seconds.
    static let eveningHour = 21
    static let morningHour = 7

    var title: String {
        switch self {
        case .oneHour: return "For 1 hour"
        case .untilTonight: return "Until tonight"
        case .untilMorning: return "Until morning"
        }
    }

    /// The wrist's shorter label — same meaning, less glass.
    var shortTitle: String {
        switch self {
        case .oneHour: return "1 hour"
        case .untilTonight: return "Tonight"
        case .untilMorning: return "Morning"
        }
    }

    var sfSymbol: String {
        switch self {
        case .oneHour: return "clock"
        case .untilTonight: return "moon"
        case .untilMorning: return "sunrise"
        }
    }

    /// When this mute ends. Always a real future instant — the type makes an
    /// untimed mute unrepresentable rather than merely discouraged.
    func expiry(from now: Date, calendar: Calendar = .current) -> Date {
        switch self {
        case .oneHour:
            return now.addingTimeInterval(3600)
        case .untilTonight:
            return Self.nextOccurrence(hour: Self.eveningHour, after: now, calendar: calendar)
        case .untilMorning:
            return Self.nextOccurrence(hour: Self.morningHour, after: now, calendar: calendar)
        }
    }

    /// Next local wall-clock occurrence of an hour (today's if it hasn't
    /// happened yet, otherwise tomorrow's). Calendar-driven so DST changes
    /// move the boundary with the clock instead of drifting an hour off it.
    static func nextOccurrence(hour: Int, after now: Date, calendar: Calendar) -> Date {
        calendar.nextDate(after: now,
                          matching: DateComponents(hour: hour, minute: 0),
                          matchingPolicy: .nextTime)
            ?? now.addingTimeInterval(3600)
    }

    /// Which choices make sense at this hour. "Until tonight" offered at 10pm
    /// would mean *tomorrow* night — a 23-hour silence wearing a 1-hour name,
    /// which is exactly the mislabeling this file exists to prevent. So the
    /// menu is time-aware: evening choices in the day, morning choices at
    /// night, and "1 hour" always.
    static func offered(at now: Date, calendar: Calendar = .current) -> [MuteDuration] {
        let hour = calendar.component(.hour, from: now)
        var out: [MuteDuration] = [.oneHour]
        if hour < eveningHour - 1 { out.append(.untilTonight) }
        if hour >= eveningHour - 1 || hour < morningHour - 1 { out.append(.untilMorning) }
        return out
    }
}
