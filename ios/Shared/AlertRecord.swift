// AlertRecord.swift
//
// One thing that actually needed you — the entry the Alerts tab is a list OF.
// Before this, "Alerts" was a rules editor: it could tell you what you had
// armed, but never what had happened, so the one question people open that
// tab to ask ("did something reach me, and did I miss it?") had no answer
// anywhere in the app.
//
// Four properties make a record trustworthy rather than decorative:
//   1. COARSE TIME, always. `bucket` is a 10-minute bucket, the same shape
//      WitnessChain uses, because Invariant III forbids precise timestamps on
//      witness events. A record that knew the second would be a privacy
//      regression wearing a UI hat.
//   2. DELIVERY IS RECORDED, not assumed. `delivery` says how this alert
//      actually reached the user — on the LAN, away via the wake path, or
//      not at all (with the reason). Claiming reach we didn't achieve is the
//      exact dishonesty the rules editor used to ship.
//   3. REPEATS COLLAPSE. A Canary that flaps for an hour is ONE record with a
//      count, never sixty rows. The list stays readable at 3am.
//   4. THE LOOP CLOSES. A record knows whether its condition is still live
//      (`resolvedBucket`) and whether the user has laid eyes on it
//      (`seenBucket`). "Still happening" and "over" must never look the
//      same — that distinction is what lets urgency mean something, and it
//      is what lets old rows age out honestly instead of haunting the list.

import Foundation

/// How an alert actually got to the user. Ordered by how much we managed.
enum AlertDelivery: UInt8, Codable, Hashable, Sendable {
    case notDelivered = 0   // it crossed a rule but could not reach them
    case onLAN = 1          // local notification, phone was home on Wi-Fi
    // It reached them while they were off the home network — either the wake
    // path did it, or an away phone posted locally for something it could
    // still genuinely observe. Both are "Away"; neither is "On Wi-Fi".
    case away = 2

    init(tolerant raw: Int) { self = AlertDelivery(rawValue: UInt8(clamping: raw)) ?? .notDelivered }

    var label: String {
        switch self {
        case .notDelivered: return "Not delivered"
        case .onLAN: return "On Wi-Fi"
        case .away: return "Away"
        }
    }

    var sfSymbol: String {
        switch self {
        case .notDelivered: return "bell.slash"
        case .onLAN: return "wifi"
        case .away: return "antenna.radiowaves.left.and.right"
        }
    }
}

/// What the user has done about it.
enum AlertHandling: UInt8, Codable, Hashable, Sendable {
    case new = 0
    case acknowledged = 1
    case muted = 2

    init(tolerant raw: Int) { self = AlertHandling(rawValue: UInt8(clamping: raw)) ?? .new }
}

struct AlertRecord: Codable, Hashable, Identifiable, Sendable {
    /// Stable across repeats of the SAME condition — this is the collapse key,
    /// so it is the witness id plus the condition fingerprint, never a UUID.
    var id: String
    var witnessID: String
    var name: String
    var severityRaw: UInt8
    /// The plain sentence, already in the fleet's one vocabulary.
    var headline: String
    /// Coarse 10-minute bucket (Invariant III) — first time we saw it.
    var bucket: Date
    /// Coarse bucket of the most recent repeat; equals `bucket` for a one-off.
    var lastBucket: Date
    /// How many times this same condition re-fired. 1 = once.
    var count: Int = 1
    var deliveryRaw: UInt8 = AlertDelivery.notDelivered.rawValue
    /// Why, when delivery failed — shown verbatim, because "we couldn't tell
    /// you" is useless without the reason.
    var undeliveredReason: String?
    var handlingRaw: UInt8 = AlertHandling.new.rawValue
    /// Coarse bucket when the condition CLEARED — nil while it's still live.
    /// Optional so a ledger written before this field decodes unchanged.
    var resolvedBucket: Date?
    /// Coarse bucket when the user first laid eyes on this row (opened the
    /// Alerts tab). Drives the app badge and the unseen dot — it is "did I
    /// miss something?", which is a different question from acknowledgment.
    var seenBucket: Date?
    /// When the mute the user chose for this condition runs out. Set beside
    /// `.muted` handling so the row can say WHEN the alerts come back rather
    /// than just that they stopped — a mute with no visible end is the shape
    /// of a silence people forget they set.
    var mutedUntil: Date?
    /// Coarse bucket when this alert was ESCALATED — re-alerted because it
    /// went unacknowledged. Top tier only (EscalationPolicy); nil for
    /// everything else, which is what keeps escalation meaning something.
    var escalatedBucket: Date?

    var severity: Severity { Severity(tolerant: Int(severityRaw)) }
    var delivery: AlertDelivery { AlertDelivery(tolerant: Int(deliveryRaw)) }
    var handling: AlertHandling { AlertHandling(tolerant: Int(handlingRaw)) }

    /// The condition is live right now.
    var isOpen: Bool { resolvedBucket == nil }
    /// The only rows that may ever feel urgent: unhandled AND still
    /// happening. A condition that cleared on its own moves to history by
    /// itself — urgency is about the present, never a backlog chore.
    var needsYou: Bool { handling == .new && isOpen }
    /// The user has never laid eyes on this row.
    var isUnseen: Bool { seenBucket == nil }
    /// This one was re-alerted for going unanswered.
    var wasEscalated: Bool { escalatedBucket != nil }

    /// The 10-minute bucket rule, in one place so every producer agrees.
    static func bucket(for date: Date) -> Date {
        let seconds = date.timeIntervalSince1970
        return Date(timeIntervalSince1970: (seconds / 600).rounded(.down) * 600)
    }

    init(id: String, witnessID: String, name: String, severity: Severity,
         headline: String, at date: Date) {
        self.id = id
        self.witnessID = witnessID
        self.name = name
        self.severityRaw = severity.rawValue
        self.headline = headline
        let b = Self.bucket(for: date)
        self.bucket = b
        self.lastBucket = b
    }
}

// MARK: - history shape (pure, host-tested)

/// One day of settled history — the Alerts tab renders these as sections so
/// last week reads as last week, not as an undifferentiated pile under one
/// "Earlier" heading.
struct AlertDaySection: Identifiable, Hashable {
    /// Local start-of-day of every record inside.
    let day: Date
    let records: [AlertRecord]
    var id: Date { day }
}

/// The list's arithmetic, kept pure so the tests can hold it still. Views
/// format; this decides order and grouping.
enum AlertHistory {
    /// Settled records grouped by local day, newest day first, newest record
    /// first within a day. (Ordering uses `lastBucket` — the most recent
    /// occurrence is what "when" means for a collapsed row.)
    static func daySections(_ records: [AlertRecord],
                            calendar: Calendar = .current) -> [AlertDaySection] {
        let byDay = Dictionary(grouping: records) {
            calendar.startOfDay(for: $0.lastBucket)
        }
        return byDay.keys.sorted(by: >).map { day in
            AlertDaySection(day: day,
                            records: byDay[day]!.sorted { $0.lastBucket > $1.lastBucket })
        }
    }

    /// The wrist's copy, cap-aware: rows that still need a human ALWAYS make
    /// the cut, settled history fills whatever room is left. A watch that
    /// shows twelve stale rows while the live alarm fell off the end would be
    /// the cap working against the product.
    static func wristRows(_ records: [AlertRecord], cap: Int) -> [AlertRecord] {
        let urgent = records.filter(\.needsYou)
        let settled = records.filter { !$0.needsYou }
        return Array((urgent + settled).prefix(cap))
    }
}
