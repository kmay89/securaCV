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

/// One day drawn as a draggable ribbon of 10-minute columns.
struct TimelineDayShapeView: View {
    let day: TimelineDay
    /// Where the scrubber currently sits, as a bucket start. Nil = not scrubbing.
    @Binding var scrubbedBucket: Date?
    /// Fired as the drag moves, already snapped to a bucket, so the caller can
    /// bring the matching row into view.
    var onScrub: (Date) -> Void

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.colorSchemeContrast) private var contrast
    /// Dynamic Type moves the ribbon with the text it sits beside — a
    /// larger-text user gets a proportionally taller, easier target.
    @ScaledMetric(relativeTo: .caption) private var ribbonHeight: CGFloat = 44

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

                    if let bucket = scrubbedBucket, let x = caretX(for: bucket, width: width) {
                        Rectangle()
                            .fill(Theme.color(.info))
                            .frame(width: 2)
                            .offset(x: x - 1)
                            .animation(reduceMotion ? nil : .interactiveSpring(), value: bucket)
                            .accessibilityHidden(true)
                    }
                }
                .contentShape(Rectangle())
                .gesture(
                    DragGesture(minimumDistance: 0)
                        .onChanged { value in scrub(toX: value.location.x, width: width) }
                        .onEnded { _ in scrubbedBucket = nil }
                )
            }
            .frame(height: ribbonHeight)
            .accessibilityElement()
            .accessibilityLabel("Shape of \(day.label)")
            .accessibilityValue(spokenSummary)
            .accessibilityHint("Swipe up or down to move through the day by the hour.")
            .accessibilityAdjustableAction { direction in
                adjust(by: direction == .increment ? 3600 : -3600)
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
            if let bucket = scrubbedBucket {
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
                Label("\(day.gapCount) gap\(day.gapCount == 1 ? "" : "s")", systemImage: "eye.slash")
                    .foregroundStyle(Theme.color(.warn))
            }
            if day.tamperCount > 0 {
                Label("\(day.tamperCount) tamper", systemImage: "hand.raised.slash.fill")
                    .foregroundStyle(Theme.color(.tamper))
            }
        }
        .font(.caption)
        .foregroundStyle(.secondary)
        .accessibilityHidden(true) // the ribbon already speaks this
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
        if let bucket = scrubbedBucket { parts.append("at " + Self.clock(bucket)) }
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

    private func caretX(for bucket: Date, width: CGFloat) -> CGFloat? {
        let offset = bucket.timeIntervalSince1970 - TimeInterval(day.dayT0)
        guard offset >= 0, offset < TimeInterval(TimelineScrub.daySeconds) else { return nil }
        return width * CGFloat(offset / TimeInterval(TimelineScrub.daySeconds))
    }

    private func scrub(toX x: CGFloat, width: CGFloat) {
        let fraction = min(max(x / width, 0), 1)
        let seconds = Double(TimelineScrub.daySeconds) * Double(fraction)
        commit(secondsIntoDay: seconds)
    }

    private func adjust(by delta: TimeInterval) {
        let current = scrubbedBucket?.timeIntervalSince1970
            ?? TimeInterval(day.dayT0) + Double(TimelineScrub.daySeconds) / 2
        commit(secondsIntoDay: current - TimeInterval(day.dayT0) + delta)
    }

    /// Snap to a bucket start and report it. Snapping is the point, not a
    /// rounding convenience: the log holds buckets, so the scrubber may not
    /// offer a position the record cannot justify.
    private func commit(secondsIntoDay: Double) {
        let clamped = min(max(secondsIntoDay, 0), Double(TimelineScrub.daySeconds - 1))
        let bucketSize = Double(TimelineScrub.defaultBucketSeconds)
        let snapped = (clamped / bucketSize).rounded(.down) * bucketSize
        let date = Date(timeIntervalSince1970: TimeInterval(day.dayT0) + snapped)
        scrubbedBucket = date
        onScrub(date)
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

    @State private var scrubbedBucket: Date?

    /// `Calendar.current`, deliberately: the Alerts list below groups its
    /// sections with the user's own calendar (`AlertHistory.daySections`), and
    /// a ribbon grouped by UTC would put a Sunday-evening alert under a
    /// "Monday" heading sitting directly above the Sunday rows it scrolls to.
    /// The evidence viewer keeps the UTC default — there the record's own
    /// frame is the honest one, and the page says so.
    private var model: TimelineModel {
        TimelineScrub.model(for: TimelineScrub.records(from: records), calendar: .current)
    }

    var body: some View {
        let days = model.days.sorted { $0.dayT0 > $1.dayT0 }
        if !days.isEmpty {
            VStack(alignment: .leading, spacing: Theme.m) {
                ForEach(Array(days.prefix(3))) { day in
                    TimelineDayShapeView(day: day, scrubbedBucket: $scrubbedBucket, onScrub: onScrub)
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
