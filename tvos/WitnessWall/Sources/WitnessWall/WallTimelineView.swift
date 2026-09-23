//  WallTimelineView.swift — the sealed record's day shape, on the shared screen.
//
//  The same timeline model the phone's Alerts ribbon and the offline evidence
//  viewer draw (ios/Shared/TimelineScrub.swift, parity-pinned to
//  viewer/timeline_core.js), fed by WallModel.timeline — which exists only
//  while this TV's own walk of the hub's sealed log verified against the key
//  it pinned at pairing. A failed walk shows the alarm banner and no ribbon;
//  an unpinned walk shows no ribbon either. What is drawn is therefore always
//  a record this Apple TV checked, never a list it was handed.
//
//  A television is not a phone: there is no drag. The Siri Remote moves a
//  cursor through BUCKETS — left and right step to the previous or next
//  bucket that holds something (quiet time is skipped, the way a fold skips
//  it on the phone), up and down change day — and Play/Pause opens the
//  bucket's details. Every "when" printed is a bucket RANGE in this TV's
//  clock ("6:40 – 6:50 PM"), never an instant: the record holds no instant
//  to print (Invariant III).
//
//  Every decision — which days a room shows, where a press moves the
//  cursor, what a bucket holds, what color a cell wears — is plain data in
//  `WallTimeline`, so WallTimelineTests proves it; the SwiftUI focus and
//  remote APIs are the one part XCTest cannot drive.

import SwiftUI

/// The timeline's decisions, as data.
enum WallTimeline {
    /// How many days of the verified tail a room shows: the Board wants
    /// history per glance, the peephole wants today, the living room the
    /// last few days. A profile changes emphasis, never the data.
    static func dayLimit(for profile: WallProfile) -> Int {
        switch profile {
        case .home: return 3
        case .business: return 5
        case .apartment: return 1
        }
    }

    /// The bucket the records were sealed on (the most common size — the
    /// same rule TimelineScrub.model applies).
    static func bucketSeconds(_ records: [TimelineRecord]) -> Int {
        TimelineScrub.mostCommonSize(records) ?? TimelineScrub.defaultBucketSeconds
    }

    /// The day rows to draw, oldest first, grouped in THIS TV's calendar — a
    /// household reads "Tuesday" as its own Tuesday. Heartbeats light no
    /// cell (TimelineScrub.days), so a heartbeat-only tail has no rows.
    static func days(for records: [TimelineRecord], profile: WallProfile,
                     calendar: Calendar) -> [TimelineDay] {
        let all = TimelineScrub.days(for: TimelineScrub.sorted(records),
                                     bucketSeconds: bucketSeconds(records),
                                     calendar: calendar)
        return Array(all.suffix(dayLimit(for: profile)))
    }

    /// Where the remote's cursor sits: a day row, and a bucket cell in it.
    struct Selection: Equatable, Sendable {
        var day: Int
        var cell: Int
    }

    /// The cursor's first home: the newest day's latest lit bucket — what
    /// just happened is what a person looks up to see.
    static func initialSelection(_ days: [TimelineDay]) -> Selection? {
        guard let last = days.indices.last else { return nil }
        return Selection(day: last, cell: days[last].cells.last?.index ?? 0)
    }

    /// A press of the Siri Remote.
    enum Move: Sendable {
        case left, right, up, down
    }

    /// Move the cursor. Left/right step to the previous/next LIT bucket of
    /// the same day and stop at the ends (an empty bucket has nothing to
    /// read); up/down move to the older/newer day and land on its lit bucket
    /// nearest the current one. A selection that no longer fits the rows
    /// (the tail moved on) starts over at the initial selection.
    static func step(_ current: Selection, _ move: Move, in days: [TimelineDay]) -> Selection {
        guard days.indices.contains(current.day) else {
            return initialSelection(days) ?? current
        }
        let lit = days[current.day].cells.map(\.index)
        switch move {
        case .left:
            guard let previous = lit.last(where: { $0 < current.cell }) else { return current }
            return Selection(day: current.day, cell: previous)
        case .right:
            guard let next = lit.first(where: { $0 > current.cell }) else { return current }
            return Selection(day: current.day, cell: next)
        case .up, .down:
            let target = move == .up ? current.day - 1 : current.day + 1
            guard days.indices.contains(target) else { return current }
            let candidates = days[target].cells.map(\.index)
            let nearest = candidates.min { abs($0 - current.cell) < abs($1 - current.cell) }
            return Selection(day: target, cell: nearest ?? 0)
        }
    }

    /// The records sealed in the selected bucket — the details strip's rows.
    /// Placed exactly as TimelineScrub.days places them (same calendar, same
    /// clamp), so the strip never lists what the cell did not count.
    /// Heartbeats stay out: proof of watching, not something that happened.
    static func records(at selection: Selection, in days: [TimelineDay],
                        from records: [TimelineRecord], bucketSeconds: Int,
                        calendar: Calendar) -> [TimelineRecord] {
        guard days.indices.contains(selection.day), bucketSeconds > 0 else { return [] }
        let day = days[selection.day]
        return TimelineScrub.sorted(records).filter { r in
            guard r.kind != .heartbeat else { return false }
            let start = calendar.startOfDay(for: Date(timeIntervalSince1970: TimeInterval(r.t0)))
            let dayT0 = Int(start.timeIntervalSince1970)
            guard dayT0 == day.dayT0 else { return false }
            return min((r.t0 - dayT0) / bucketSeconds, day.cellsPerDay - 1) == selection.cell
        }
    }

    /// The bucket as a range in this TV's clock — a bucket IS a range, and
    /// the Wall prints it as one.
    static func bucketRange(start: Int, seconds: Int,
                            timeZone: TimeZone = .current, locale: Locale = .current) -> String {
        let formatter = DateFormatter()
        formatter.locale = locale
        formatter.timeZone = timeZone
        formatter.dateStyle = .none
        formatter.timeStyle = .short
        let from = formatter.string(from: Date(timeIntervalSince1970: TimeInterval(start)))
        let to = formatter.string(from: Date(timeIntervalSince1970: TimeInterval(start + seconds)))
        return "\(from) – \(to)"
    }

    /// The color role a lit cell wears. Sealed records carry no severity, so
    /// the phone's rule for a sealed-log feed applies: tamper and declared
    /// gaps ride their reserved status roles, everything else the neutral
    /// role — never a status color borrowed for an ordinary event.
    static func role(for cell: TimelineDayCell) -> Theme.Role {
        if cell.family == .tamper { return .tamper }
        if cell.hasGap { return .warn }
        return .neutral
    }

    /// One details row: what, where, and a gap's own words.
    static func line(for record: TimelineRecord) -> String {
        var parts = [record.label]
        if !record.zone.isEmpty { parts.append(record.zone) }
        if !record.details.isEmpty { parts.append(record.details) }
        return parts.joined(separator: " · ")
    }
}

/// The ribbon: one row per day, lit buckets on a thin baseline.
struct WallTimelineView: View {
    let records: [TimelineRecord]
    let unparsed: Int
    let profile: WallProfile
    let skin: WallSkin

    @State private var selection: WallTimeline.Selection?
    @State private var showDetails = false
    @FocusState private var focused: Bool
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    private var calendar: Calendar { Calendar.current }
    private var bucketSeconds: Int { WallTimeline.bucketSeconds(records) }

    var body: some View {
        let days = WallTimeline.days(for: records, profile: profile, calendar: calendar)
        VStack(alignment: .leading, spacing: profile == .business ? 8 : 12) {
            HStack(alignment: .firstTextBaseline) {
                Text("The sealed record, verified on this Apple TV")
                    .font(.headline)
                    .foregroundStyle(skin.ink)
                Spacer()
                Text(headerCaption)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            if days.isEmpty {
                Text("Nothing but heartbeats in the served tail — the hub watched, and nothing happened.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            ForEach(Array(days.enumerated()), id: \.element.dayT0) { index, day in
                HStack(spacing: 16) {
                    Text(dayCaption(day))
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .frame(width: 300, alignment: .leading)
                    DayRibbon(day: day,
                              selectedCell: selection?.day == index ? selection?.cell : nil,
                              focused: focused)
                        .frame(height: profile == .business ? 26 : 34)
                }
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(spokenSummary(day))
            }
            if showDetails, let selected = selection, days.indices.contains(selected.day) {
                details(selected, days: days)
            }
            if unparsed > 0 {
                Text(unparsedLine)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(20)
        .background(skin.tile.opacity(focused ? 0.9 : 0.5), in: RoundedRectangle(cornerRadius: 16))
        .focusable()
        .focused($focused)
        .onMoveCommand { direction in
            let move: WallTimeline.Move
            switch direction {
            case .left: move = .left
            case .right: move = .right
            case .up: move = .up
            case .down: move = .down
            @unknown default: return
            }
            let from = selection ?? WallTimeline.initialSelection(days)
            if let from { selection = WallTimeline.step(from, move, in: days) }
        }
        .onPlayPauseCommand { showDetails.toggle() }
        .onAppear { selection = WallTimeline.initialSelection(days) }
        .onChange(of: records) {
            // A new verified tail: the cursor returns to "what just
            // happened" rather than pointing at a bucket that moved.
            selection = WallTimeline.initialSelection(
                WallTimeline.days(for: records, profile: profile, calendar: calendar))
        }
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.2), value: selection)
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.2), value: showDetails)
    }

    /// The header's right-hand line: how to drive it while focused, what it
    /// holds otherwise.
    private var headerCaption: String {
        if focused { return "Swipe to move between buckets · Play/Pause for details" }
        let noun = records.count == 1 ? "entry" : "entries"
        return "\(records.count) sealed \(noun) · \(bucketSeconds / 60)-minute buckets"
    }

    /// Entries the verified log held that the timeline could not read:
    /// counted, never silently dropped.
    private var unparsedLine: String {
        let noun = unparsed == 1 ? "entry" : "entries"
        return "\(unparsed) sealed \(noun) could not be read into the timeline — counted here, not drawn."
    }

    private func dayCaption(_ day: TimelineDay) -> String {
        var caption = day.label
        if day.gapCount > 0 { caption += " · \(day.gapCount) gap\(day.gapCount == 1 ? "" : "s")" }
        if day.tamperCount > 0 { caption += " · tamper" }
        return caption
    }

    private func spokenSummary(_ day: TimelineDay) -> String {
        var sentence = "\(day.label): \(day.count) sealed \(day.count == 1 ? "record" : "records")"
        if day.gapCount > 0 { sentence += ", \(day.gapCount) declared \(day.gapCount == 1 ? "gap" : "gaps")" }
        if day.tamperCount > 0 { sentence += ", \(day.tamperCount) tamper" }
        return sentence
    }

    private func details(_ selected: WallTimeline.Selection, days: [TimelineDay]) -> some View {
        let bucket = bucketSeconds
        let start = days[selected.day].dayT0 + selected.cell * bucket
        let rows = WallTimeline.records(at: selected, in: days, from: records,
                                        bucketSeconds: bucket, calendar: calendar)
        return VStack(alignment: .leading, spacing: 6) {
            Text(WallTimeline.bucketRange(start: start, seconds: bucket))
                .font(.headline)
            if rows.isEmpty {
                Text("Nothing sealed in this bucket.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            } else {
                ForEach(Array(rows.enumerated()), id: \.offset) { _, record in
                    Text(WallTimeline.line(for: record))
                        .font(.callout)
                }
            }
        }
        .padding(.top, 6)
    }
}

/// One day's strip: a hairline for orientation (the sealed tail declares no
/// covered window, so nothing here claims an hour was quiet), the lit
/// buckets in their roles, and the cursor.
private struct DayRibbon: View {
    let day: TimelineDay
    let selectedCell: Int?
    let focused: Bool

    var body: some View {
        Canvas { context, size in
            let perDay = CGFloat(max(day.cellsPerDay, 1))
            let width = size.width / perDay
            let baseline = CGRect(x: 0, y: size.height / 2 - 0.5, width: size.width, height: 1)
            context.fill(Path(baseline), with: .color(Theme.color(.neutral).opacity(0.35)))
            for cell in day.cells {
                let rect = CGRect(x: CGFloat(cell.index) * width, y: 0,
                                  width: max(width, 2), height: size.height)
                context.fill(Path(rect), with: .color(Theme.color(WallTimeline.role(for: cell))))
            }
            if let selectedCell {
                let rect = CGRect(x: CGFloat(selectedCell) * width - 3, y: -3,
                                  width: max(width, 2) + 6, height: size.height + 6)
                context.stroke(Path(roundedRect: rect, cornerRadius: 3),
                               with: .color(focused ? Color.primary : Color.secondary),
                               lineWidth: focused ? 3 : 1.5)
            }
        }
    }
}
