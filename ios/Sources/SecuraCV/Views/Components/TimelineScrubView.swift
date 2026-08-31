// TimelineScrubView.swift
//
// The day, as a shape you can drag.
//
// The Alerts tab answers "did something need me?" one row at a time, which is
// the right answer to the wrong question at 7am: the thing people actually do
// with a day of alerts is GLANCE at it. So this is the phone's half of the
// same idea as the offline viewer's minimap — a day drawn as one ribbon, dense
// where the day was busy, blank where the export never covered, and hatched
// where the device declared it could not see. You read the shape first and the
// words only if the shape asks you to.
//
// Three rules it inherits, none of them negotiable:
//
//   1. COARSE TIME ONLY. Every column is a 10-minute bucket (Invariant III).
//      The scrubber snaps to buckets because finer timing was never recorded —
//      a smooth continuous scrub would be a privacy regression wearing a UI hat.
//   2. WORST-FIRST. A column reports the most serious thing in it, never an
//      average, and tamper keeps its own reserved role. A busy hour must never
//      dilute the one thing in it that mattered.
//   3. UNCOVERED IS NOT QUIET. Hours outside the covered window render as
//      absent, not as a flat "nothing happened" — the same distinction the
//      glass histogram makes when the clock was not yet known.
//
// The arithmetic lives in Shared/TimelineScrub.swift (pure, host-tested, and
// pinned against the JavaScript implementation by
// viewer/fixtures/timeline/scrub_parity.json). This file is the thin shell:
// it draws and it handles input, and it decides nothing about what a day was.

import SwiftUI

/// Which day is being scrubbed, and where. Scoped to a day on purpose: one
/// shared `Date` made every OTHER ribbon print and announce a time that was
/// not its own — VoiceOver read "Sunday … at 2:30 PM" off a bucket the user
/// had picked on Monday.
struct TimelineScrubPosition: Equatable {
    var dayT0: Int
    var bucket: Date
}

/// One day drawn as a draggable ribbon of 10-minute columns.
struct TimelineDayShapeView: View {
    let day: TimelineDay
    /// The active scrub across ALL ribbons; this one reads it only when it
    /// names this day (see `myBucket`).
    @Binding var position: TimelineScrubPosition?
    /// This day's first tamper bucket, derived by the host FROM THE RECORDS.
    /// The cells cannot answer it: a bucket where a tamper shares ten minutes
    /// with a declared gap is family `.gap` by the worst-first rule, so a
    /// cell scan misses exactly the "lens covered, then pried" pairing that
    /// matters most. Nil when the day holds no tamper records.
    var firstTamperBucket: Date?
    /// Fired when the drag ENDS, already snapped to a bucket, so the caller
    /// can bring the matching row into view.
    var onScrub: (Date) -> Void

    /// The scrub position if it belongs to this day, otherwise nil.
    private var myBucket: Date? {
        guard let p = position, p.dayT0 == day.dayT0 else { return nil }
        return p.bucket
    }

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.colorSchemeContrast) private var contrast
    /// Dynamic Type moves the ribbon with the text it sits beside — a
    /// larger-text user gets a proportionally taller, easier target.
    @ScaledMetric(relativeTo: .caption) private var ribbonHeight: CGFloat = 44

    /// The live drag, in ribbon coordinates. `virtualX` is where the scrub
    /// IS; `lastX` is where the finger was — they separate so pulling the
    /// finger down can slow the scrub (see `gain`) without the caret leaping
    /// back to the fingertip. Non-nil exactly while a drag owns the ribbon.
    @State private var scrub: ScrubDrag?
    /// 0 = full speed, 1 = half, 2 = quarter — chosen by how far the finger
    /// has pulled down from the ribbon, the Music-scrubber gesture. Gain
    /// changes input speed only: every output still snaps to a bucket, so
    /// "fine" means reliably landing on ONE 10-minute bucket, never between.
    @State private var gainTier = 0
    /// Bumped by every seat and every scheduled clear, so a pending
    /// caret-clear task can tell "still my landing" from "someone landed
    /// here again" without comparing position values.
    @State private var caretGen = 0

    private struct ScrubDrag {
        var lastX: CGFloat
        var virtualX: CGFloat
    }

    /// Columns are keyed by index into the day, so a lookup is a dictionary
    /// hit rather than a scan per drawn column. Indices are unique by
    /// construction; `uniquingKeysWith` keeps a malformed model from trapping
    /// inside a view body rather than merely drawing oddly.
    private var cellsByIndex: [Int: TimelineDayCell] {
        Dictionary(day.cells.map { ($0.index, $0) }, uniquingKeysWith: { first, _ in first })
    }

    private var busiest: Int { max(day.cells.map(\.count).max() ?? 1, 1) }

    var body: some View {
        VStack(alignment: .leading, spacing: Theme.xs) {
            header
            GeometryReader { geo in
                let width = max(geo.size.width, 1)
                ZStack(alignment: .topLeading) {
                    Canvas { context, size in
                        draw(in: context, size: size)
                    }
                    .accessibilityHidden(true)

                    if let bucket = myBucket, let x = caretX(for: bucket, width: width) {
                        Rectangle()
                            .fill(Theme.color(.info))
                            .frame(width: 2)
                            .offset(x: x - 1)
                            .animation(reduceMotion ? nil : .interactiveSpring(), value: bucket)
                            .accessibilityHidden(true)
                    }
                }
                .contentShape(Rectangle())
                .onTapGesture { location in
                    // The most natural gesture of all: tap the peak, land on
                    // it. seat() snaps, so a tap can never claim a finer
                    // moment than the bucket it touched.
                    preview(atX: location.x, width: width)
                    if let bucket = myBucket { onScrub(bucket) }
                    scheduleCaretClear()
                }
                .gesture(
                    // `minimumDistance: 0` claimed the touch on touch-DOWN, so
                    // three 44pt bands of the Alerts list stopped scrolling:
                    // a flick that began on a ribbon scrubbed instead. Require
                    // real movement, and only take the gesture once it is
                    // mostly sideways — a vertical flick belongs to the list
                    // this ribbon is a row of. Once the drag IS claimed,
                    // vertical distance stops meaning "scroll" and starts
                    // meaning "slower": pulling down drops the horizontal
                    // gain to half, then a quarter, the same way the system
                    // media scrubber does.
                    DragGesture(minimumDistance: 8)
                        .onChanged { value in
                            if scrub == nil {
                                guard abs(value.translation.width) > abs(value.translation.height) else { return }
                                scrub = ScrubDrag(lastX: value.location.x, virtualX: value.location.x)
                            }
                            guard var drag = scrub else { return }
                            let pull = max(value.translation.height, 0)
                            let tier = pull > 120 ? 2 : pull > 60 ? 1 : 0
                            if tier != gainTier { gainTier = tier }
                            let gain: CGFloat = tier == 2 ? 0.25 : tier == 1 ? 0.5 : 1
                            drag.virtualX = min(max(drag.virtualX + (value.location.x - drag.lastX) * gain, 0), width)
                            drag.lastX = value.location.x
                            scrub = drag
                            preview(atX: drag.virtualX, width: width)
                        }
                        .onEnded { _ in
                            // Scrolling DURING the drag moved the ribbon out
                            // from under the finger — it is a row of the very
                            // list it scrolls. So the drag only previews, and
                            // the list is moved once, on release.
                            if scrub != nil, let bucket = myBucket {
                                onScrub(bucket)
                            }
                            scrub = nil
                            gainTier = 0
                            scheduleCaretClear()
                        }
                )
                // The bucket IS the detent, felt as well as seen: a tick when
                // the caret crosses into a bucket that holds records, a firmer
                // nudge entering a declared gap or a tamper bucket — the
                // tactile twin of the outline-not-fill rule. Silence over
                // empty buckets, and nothing scaled by severity or count: a
                // scrub must never become a Geiger counter.
                .sensoryFeedback(trigger: myBucket) { _, seated in
                    guard let bucket = seated, let cell = cell(at: bucket) else { return nil }
                    if cell.hasGap || cell.family == .tamper { return .impact(weight: .light) }
                    return .selection
                }
            }
            .frame(height: ribbonHeight)
            .accessibilityElement()
            .accessibilityLabel("Shape of \(day.label)")
            .accessibilityValue(spokenSummary)
            .accessibilityHint("Swipe up or down to move through the day by the hour.")
            .accessibilityAdjustableAction { direction in
                adjust(by: direction == .increment ? 3600 : -3600)
            }
            .accessibilityActions {
                // The records worth the trip get one-gesture routes: the same
                // jumps the sighted footer buttons offer. Navigation order,
                // never a filter — the full day stays exactly as spoken.
                if day.tamperCount > 0 {
                    Button("Jump to tamper") { jump(toTamper: true) }
                }
                if day.gapCount > 0 {
                    Button("Jump to declared gap") { jump(toTamper: false) }
                }
            }
            footer
        }
    }

    // MARK: - chrome

    private var header: some View {
        HStack(alignment: .firstTextBaseline, spacing: Theme.s) {
            Text(day.label)
                .font(.subheadline.weight(.semibold))
            Spacer(minLength: Theme.xs)
            if gainTier > 0 {
                // Names the gear the finger already felt shift, exactly as the
                // system media scrubber labels its speed while dragging.
                Text(gainTier == 2 ? "¼ speed" : "½ speed")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            if let bucket = myBucket {
                Text(Self.clock(bucket))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(Theme.color(.info))
            }
        }
    }

    /// The one honest sentence about the day, in words — so the shape is never
    /// the only way to get the answer (and VoiceOver, print, and a screenshot
    /// all carry it).
    private var footer: some View {
        HStack(spacing: Theme.s) {
            Text(countPhrase)
            if day.gapCount > 0 {
                // The counts are destinations, not captions: tapping one
                // scrubs straight to the first record behind the number.
                Button { jump(toTamper: false) } label: {
                    Label("\(day.gapCount) gap\(day.gapCount == 1 ? "" : "s")", systemImage: "eye.slash")
                        .foregroundStyle(Theme.color(.warn))
                }
                .buttonStyle(.plain)
            }
            if day.tamperCount > 0 {
                Button { jump(toTamper: true) } label: {
                    Label("\(day.tamperCount) tamper", systemImage: "hand.raised.slash.fill")
                        .foregroundStyle(Theme.color(.tamper))
                }
                .buttonStyle(.plain)
            }
        }
        .font(.caption)
        .foregroundStyle(.secondary)
        .accessibilityHidden(true) // the ribbon speaks this, and carries the same jumps as custom actions
    }

    private var countPhrase: String {
        day.count == 1 ? "1 record" : "\(day.count) records"
    }

    private var spokenSummary: String {
        var parts = [countPhrase]
        if let raw = day.worstSeverityRaw {
            parts.append("worst: " + Severity(tolerant: Int(raw)).label)
        }
        if day.gapCount > 0 { parts.append("\(day.gapCount) declared gap\(day.gapCount == 1 ? "" : "s")") }
        if let bucket = myBucket { parts.append("at " + Self.clock(bucket)) }
        return parts.joined(separator: ", ")
    }

    // MARK: - drawing

    private func draw(in context: GraphicsContext, size: CGSize) {
        let columns = max(day.cellsPerDay, 1)
        let colWidth = size.width / CGFloat(columns)
        let barWidth = max(colWidth - 0.5, 0.75)

        // The covered window: the ONLY region allowed to read as "quiet",
        // and only when the source actually declares one. The alert ledger
        // declares nothing, so no track is drawn there — inferring coverage
        // from where the records happen to fall would claim the device was
        // watching on exactly the evidence that cannot show it.
        let coverX0 = size.width * CGFloat(day.coverageFrom ?? 0)
        let coverX1 = size.width * CGFloat(day.coverageTo ?? 0)
        if day.coverageFrom != nil, day.coverageTo != nil, coverX1 > coverX0 {
            context.fill(
                Path(roundedRect: CGRect(x: coverX0, y: size.height - 3,
                                         width: coverX1 - coverX0, height: 3),
                     cornerRadius: 1.5),
                with: .color(Theme.color(.neutral).opacity(0.28)))
        }

        // Faint hour ticks along the baseline, a touch stronger at 6/12/18,
        // so a scrub toward "around 3pm" is aimable. Hours are the visual
        // grid; the felt grid stays the 10-minute bucket — ticking all 144
        // would promise a pointing precision no fingertip has.
        for hour in 1..<24 {
            let x = size.width * CGFloat(hour) / 24
            let tall: CGFloat = hour % 6 == 0 ? 5 : 3
            context.fill(
                Path(CGRect(x: x - 0.5, y: size.height - tall, width: 1, height: tall)),
                with: .color(Theme.color(.neutral).opacity(hour % 6 == 0 ? 0.4 : 0.22)))
        }

        let cells = cellsByIndex
        for index in 0..<columns {
            guard let cell = cells[index] else { continue }
            let x = CGFloat(index) * colWidth
            // Height is the column's count against the day's busiest column,
            // with a floor so a single record is never invisible.
            let ratio = CGFloat(cell.count) / CGFloat(busiest)
            let height = max(4, (size.height - 4) * (0.35 + 0.65 * ratio))
            let rect = CGRect(x: x, y: size.height - 3 - height, width: barWidth, height: height)
            let path = Path(roundedRect: rect, cornerRadius: min(1.5, barWidth / 2))

            if cell.hasGap {
                // A declared blind spot is drawn as an OUTLINE, not a fill:
                // shape carries the meaning so it survives grayscale, color
                // blindness, and a black-and-white printout.
                context.stroke(path, with: .color(Theme.color(.warn)), lineWidth: 1)
            } else {
                context.fill(path, with: .color(color(for: cell)))
            }
        }
    }

    /// Worst-first, in the app's established color vocabulary: severity picks
    /// a `Theme.Role`. Records with no severity at all (a sealed-log export)
    /// fall back to the neutral role rather than borrowing a status color.
    private func color(for cell: TimelineDayCell) -> Color {
        if cell.family == .tamper { return Theme.color(.tamper) }
        guard let raw = cell.worstSeverityRaw else { return Theme.color(.neutral) }
        let role = Severity(tolerant: Int(raw)).role
        // Under increased contrast, a calm column would fade into the track;
        // lift it to the informational role so every drawn column stays read.
        if contrast == .increased && role == .calm { return Theme.color(.info) }
        return Theme.color(role)
    }

    // MARK: - input

    /// The day's cell under a bucket date, if that bucket holds records.
    private func cell(at bucket: Date) -> TimelineDayCell? {
        let offset = bucket.timeIntervalSince1970 - TimeInterval(day.dayT0)
        guard offset >= 0, offset < TimeInterval(TimelineScrub.daySeconds) else { return nil }
        return cellsByIndex[Int(offset) / TimelineScrub.defaultBucketSeconds]
    }

    /// Scrub straight to the day's first tamper (or first declared-gap)
    /// bucket and walk the list there — the footer counts and the VoiceOver
    /// custom actions both land here. Tamper comes from `firstTamperBucket`
    /// (see its comment); gaps have a dedicated per-cell flag, so a cell scan
    /// is exact for them.
    private func jump(toTamper: Bool) {
        let bucket: Date? = toTamper
            ? firstTamperBucket
            : day.cells.filter(\.hasGap).map(\.index).min().map {
                Date(timeIntervalSince1970:
                    TimeInterval(day.dayT0 + $0 * TimelineScrub.defaultBucketSeconds))
            }
        guard let bucket else { return }
        position = TimelineScrubPosition(dayT0: day.dayT0, bucket: bucket)
        onScrub(bucket)
        scheduleCaretClear()
    }

    /// The finger lifts, the list glides — and only then does the caret bow
    /// out. Clearing it the same frame erased the answer ("where did I land?")
    /// at the exact moment the question was asked. The token is a GENERATION,
    /// never the position value: two landings on the same bucket compare
    /// equal, so a value check let the first landing's timer erase the
    /// second's caret — or void a live drag resting on the same bucket.
    private func scheduleCaretClear() {
        caretGen += 1
        let generation = caretGen
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 900_000_000)
            // `position` is shared across every ribbon, but the generation
            // and drag state are this ribbon's own — so also require the
            // caret to still be OURS. Without that, landing here and then
            // scrubbing a second day within 900ms let this timer erase the
            // other ribbon's live caret.
            if caretGen == generation, scrub == nil, position?.dayT0 == day.dayT0 { position = nil }
        }
    }

    private func caretX(for bucket: Date, width: CGFloat) -> CGFloat? {
        let offset = bucket.timeIntervalSince1970 - TimeInterval(day.dayT0)
        guard offset >= 0, offset < TimeInterval(TimelineScrub.daySeconds) else { return nil }
        return width * CGFloat(offset / TimeInterval(TimelineScrub.daySeconds))
    }

    /// Moves the caret and the header clock only — the list is left alone
    /// until the drag ends.
    private func preview(atX x: CGFloat, width: CGFloat) {
        let fraction = min(max(x / width, 0), 1)
        seat(secondsIntoDay: Double(TimelineScrub.daySeconds) * Double(fraction))
    }

    /// VoiceOver's adjustable action. Not a drag, so it moves the list
    /// immediately — that is the whole point of the gesture.
    private func adjust(by delta: TimeInterval) {
        let current = myBucket?.timeIntervalSince1970
            ?? TimeInterval(day.dayT0) + Double(TimelineScrub.daySeconds) / 2
        seat(secondsIntoDay: current - TimeInterval(day.dayT0) + delta)
        if let bucket = myBucket { onScrub(bucket) }
    }

    /// Snap to a bucket start and report it. Snapping is the point, not a
    /// rounding convenience: the log holds buckets, so the scrubber may not
    /// offer a position the record cannot justify.
    private func seat(secondsIntoDay: Double) {
        caretGen += 1 // a fresh seating voids any pending caret-clear
        let clamped = min(max(secondsIntoDay, 0), Double(TimelineScrub.daySeconds - 1))
        let bucketSize = Double(TimelineScrub.defaultBucketSeconds)
        let snapped = (clamped / bucketSize).rounded(.down) * bucketSize
        let date = Date(timeIntervalSince1970: TimeInterval(day.dayT0) + snapped)
        position = TimelineScrubPosition(dayT0: day.dayT0, bucket: date)
    }

    /// A bucket is a RANGE, and saying "12:20" for it would narrow a
    /// ten-minute window the log deliberately left wide (Invariant III). The
    /// phone shows the user's own wall clock; the parity-checked UTC forms
    /// stay in the model for the record itself.
    private static func clock(_ date: Date) -> String {
        let end = date.addingTimeInterval(TimeInterval(TimelineScrub.defaultBucketSeconds))
        return date.formatted(.dateTime.hour().minute())
            + " – " + end.formatted(.dateTime.hour().minute())
    }
}

/// The Alerts tab's mount: one ribbon per day that has records, newest first,
/// scrubbing the list it sits above.
struct TimelineScrubSection: View {
    let records: [AlertRecord]
    /// Called with the bucket the user scrubbed to, so the host can scroll.
    var onScrub: (Date) -> Void

    @State private var position: TimelineScrubPosition?

    /// `Calendar.current`, deliberately: the Alerts list below groups its
    /// sections with the user's own calendar (`AlertHistory.daySections`), and
    /// a ribbon grouped by UTC would put a Sunday-evening alert under a
    /// "Monday" heading sitting directly above the Sunday rows it scrolls to.
    /// The evidence viewer keeps the UTC default — there the record's own
    /// frame is the honest one, and the page says so.
    private var model: TimelineModel {
        TimelineScrub.model(for: TimelineScrub.records(from: records), calendar: .current)
    }

    /// Earliest tamper bucket per calendar day, straight from the records —
    /// the one question the worst-first cells cannot answer (a tamper sharing
    /// a bucket with a declared gap lives in a `.gap`-family cell).
    private func firstTamperBuckets(in model: TimelineModel, calendar: Calendar) -> [Int: Date] {
        var out: [Int: Date] = [:]
        for record in model.records where record.family == .tamper {
            let bucket = Date(timeIntervalSince1970: TimeInterval(record.t0))
            let dayT0 = Int(calendar.startOfDay(for: bucket).timeIntervalSince1970)
            if out[dayT0] == nil { out[dayT0] = bucket }
        }
        return out
    }

    var body: some View {
        let built = model
        let days = built.days.sorted { $0.dayT0 > $1.dayT0 }
        let tamperBuckets = firstTamperBuckets(in: built, calendar: .current)
        if !days.isEmpty {
            VStack(alignment: .leading, spacing: Theme.m) {
                ForEach(Array(days.prefix(3))) { day in
                    TimelineDayShapeView(day: day, position: $position,
                                         firstTamperBucket: tamperBuckets[day.dayT0],
                                         onScrub: onScrub)
                }
                if days.count > 3 {
                    // The ribbons stop at three; the record does not. Say so,
                    // or the boundary reads as the history ending here.
                    Text("Shapes for the 3 most recent days — earlier days continue in the list below.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.vertical, Theme.xs)
        }
    }
}

/// Preview data, built outside the preview body so the preview itself stays a
/// single expression (the house style, and it keeps the ViewBuilder happy).
private enum TimelineScrubPreviewData {
    static var records: [AlertRecord] {
        let start = Date().addingTimeInterval(-9 * 3600)
        let severities: [Severity] = [.ok, .notice, .notice, .warn, .notice, .tamper]
        return (0..<16).map { i in
            var record = AlertRecord(id: "sample-\(i)",
                                     witnessID: "canary-1",
                                     name: "Canary 1",
                                     severity: severities[i % severities.count],
                                     headline: "Something crossed the boundary",
                                     at: start.addingTimeInterval(Double(i) * 1800))
            record.count = i % 4 == 0 ? 3 : 1
            return record
        }
    }
}

#Preview("Day shape") {
    TimelineScrubSection(records: TimelineScrubPreviewData.records, onScrub: { _ in })
        .padding()
}
